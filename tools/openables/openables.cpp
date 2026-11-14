// openables.cpp -- every scripted record in Realms of the Haunting's levels with what it touches and the whole
// chain it runs, spelled out, so the furniture that opens (drawers, cupboards, chests, desks, wardrobes, ...)
// can be found, counted and checked. Read from the player's own install; engine-free; writes nothing unless
// asked to dump pictures (and then only into the folder named: the pictures are the retail art, never commit them).
//
//   openables <DATA>                     one block per trigger record: header line + every chain step
//   openables <DATA> -grep WORD[,WORD]   only the records whose own chain, or a record on the same target,
//                                        speaks a line containing one of the words (case-insensitive)
//   openables <DATA> -map MAP            only one map
//   openables <DATA> -summary            one entry per target (object id / face id / sector id): its pictures,
//                                        what examining it says, and every use record's chain in short form
//   openables <DATA> -sector MAP S...      one sector's box, heights, faces and objects (a small door's room and
//                                        what it hides)
//   openables <DATA> -dump DIR           also write every target picture and every picture a chain swaps in
//                                        (DIR/<PACK>_<index>.ppm), for contact sheets in /tmp
//   openables <DATA> -area MAP [-stop ID,ID] [-have I,I] [-dump DIR] [-poly FILE]
//                                        what the player reaches ON FOOT from the map's start, room by room, with
//                                        every scripted record on it, the hidden objects and who shows them, and the
//                                        light groups its records ramp (the rules: the comment above AreaMode).
//                                        docs/VR_CHAPTER01_SHADOWS.md: -area STUDY1 -stop 29 -have 1,15,16,68
//                                        -poly writes a plan-view dump (sectors, objects, faces with records)
//
// Build: sh tools/openables/build.sh   (writes /tmp/openables)
//
// WHAT A TRIGGER RECORD IS AND WHAT IT TARGETS (the category directories of the command header, as
// tools/world_census reads them; GAME_core.md 5.2):
//   CLICK  object click directory, opcode 0x08: objects whose id word (+0x0e) equals the key
//          (run_leftclick_object_trigger, raw_commands.c:2925-2933; activate_targeted_object, input.c:1026-1100)
//   USEW   wall use, opcode 0x18: the faces the key resolves to (faceID two-hop, roth_raw.cpp)
//   USEF   floor/platform use, opcode 0x19: the sector whose commandID is the key
//   EXO / EXW / EXF  examine object 0x30 / wall 0x32 / floor 0x31 (same targets as above); the line said is
//          the record's word +0x0a as a DBASE100 action id (examine_world_object, input.c:964)
//
// WHAT EACH CHAIN STEP PRINTS (the handler named is the one the dispatch at raw_commands.c:1485-1556 calls):
//   0x27 if-not-item  item (inventory id) + name          cmd_if_not_item,        raw_commands.c:4760
//   0x29 give         item + name                          cmd_give_item,          raw_commands.c:4799
//   0x2a take         item + name                          cmd_remove_item,        raw_commands.c:4918
//   0x2b / 0x36 talk  DBASE100 action id + its lines       run_command_dbase100_record (0x3540b) / cmd_dbase100_if_next_fails, :4053 area
//   0x0d objtex       object id (0 = the clicked one) -> new art word at +0x0c, resolved to PACK[index]
//                                                          cmd_change_object_texture, raw_commands.c:4520
//   0x3a objid        new object id word at +0x06          cmd_change_object_id,   raw_commands.c:479
//   0x34 walltex      face id -> texture word at +0x0a     cmd_change_face_texture, raw_commands.c:396
//   0x0c walltex+     face (by raw offset)                 cmd_change_face_texture_adv, raw_commands.c:4721
//   0x2e smash        face id: toggles bit 0 of the slot   cmd_smash_face_texture, raw_commands.c:361
//   0x07 height       sector id, limits                    cmd_change_height,      raw_commands.c:4483
//   0x09 move sector  sector id                            cmd_move_sector,        raw_commands.c:4588
//   0x03 modify sec.  sector id                            cmd_modify_sector,      raw_commands.c:4953
//   0x23 obj height   object id                            cmd_change_object_height, raw_commands.c:4554
//   0x24 obj rotate   object id                            cmd_rotate_object,      raw_commands.c:241
//   0x2f door         face id (0 = the face used) + door size  cmd_open_door,      raw_commands.c:4053
//   0x16 / 0x3c spawn object id (0x16 toggles the hidden bit, flags & 0x80, or wakes an actor)
//                                                          cmd_spawn_object :622 / cmd_spawn_object_adv :4108
//   0x10 sfx node     key                                  cmd_activate_sfx_node,  raw_commands.c:4855
//   0x12 delay        ticks (word +0x06)                   cmd_delay_timer,        raw_commands.c:4420
//   0x26 / 0x28 flag  DBASE100 flag id                     cmd_set_flag :592 / cmd_if_not_flag :4665
//   other opcodes print their raw words.
// The walk follows NEXT from the trigger's start and enters 0x40 sub-chains (word +0x06); it does NOT take
// branches separately (a failed 0x27/0x28 ends the run in the original; here every step is listed).

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"
#include "roth_dbase100.h"
#include "roth_log.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace roth;

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}
static uint16_t U16(const std::vector<uint8_t> &b, size_t at) { return at + 1 < b.size() ? (uint16_t)(b[at] | (b[at + 1] << 8)) : 0; }

struct LoadedPack { std::vector<uint8_t> bytes; Pack pack; bool ok = false; };
static std::map<std::string, LoadedPack *> gPacks;
static LoadedPack *GetPack(const std::string &name)
{
	auto it = gPacks.find(name);
	if (it != gPacks.end()) return it->second;
	auto *lp = new LoadedPack;
	lp->bytes = ReadFile(TheInstall().PackFile(name.c_str()));
	lp->ok = !lp->bytes.empty() && lp->pack.Load(lp->bytes.data(), lp->bytes.size());
	gPacks[name] = lp; return lp;
}

// DBASE400 text entry: u32 voice, u16 length, u8 colour, u8, then the text (GAME_dialogue.md 2.1).
static std::vector<uint8_t> gText;
static std::string Text400(uint32_t off)
{
	if (off == 0 || (size_t)off + 8 > gText.size()) return "";
	uint32_t len = gText[off + 4] | (gText[off + 5] << 8);
	if (len > 200) len = 200;
	std::string s;
	for (uint32_t i = 0; i < len && off + 8 + i < gText.size(); i++)
	{
		const char c = (char)gText[off + 8 + i];
		if (c == 0) break;
		s += (c >= 32 && c < 127) ? c : ' ';
	}
	return s;
}

static Dbase100 gDb;
// The lines an action record says (opcode 5 "say", 8 "choice"; dbase100_dump's reading), joined.
static std::string ActionText(int id)
{
	const DbAction *a = gDb.Action(id);
	if (!a) return "";
	std::string s;
	for (const DbCommand &c : gDb.Commands(*a))
		if (c.opcode == 5 || c.opcode == 8)
		{
			if (!s.empty()) s += " / ";
			s += Text400(c.operand);
		}
	return s;
}
static std::string ItemName(int id)
{
	const DbItem *it = gDb.Item(id);
	return it ? Text400(it->name) : "";
}

enum Category { CAT_CLICK_OBJ = 0, CAT_USE_WALL = 4, CAT_USE_FLOOR = 5, CAT_EXAM_WALL = 10, CAT_EXAM_FLOOR = 11, CAT_EXAM_OBJ = 12 };

struct MapData
{
	std::string name, pack;
	Map m;
	std::vector<std::vector<int>> cat;
	std::vector<int> catOf;
};

static bool LoadMap(const std::string &name, MapData &md)
{
	std::vector<uint8_t> raw = ReadFile(TheInstall().MapFile(name.c_str()));
	if (raw.empty()) return false;
	md.name = name;
	md.pack = TheInstall().PackFor(name.c_str());
	md.m = ParseRaw(raw.data(), raw.size());
	if (!md.m.ok()) return false;
	const size_t base = (size_t)md.m.header.verticesOffset + md.m.header.verticesSectionSize;
	const uint16_t count = U16(raw, base + 6);
	std::map<uint32_t, int> byOff;
	size_t at = base + 8 + 60 + 2 * (size_t)count;
	for (int i = 0; i < count; i++)
	{
		byOff[(uint32_t)(at - base)] = i;
		const uint16_t size = U16(raw, at);
		if (size < 6) break;
		at += size;
	}
	md.cat.assign(15, {});
	md.catOf.assign(md.m.commands.size(), -1);
	for (int c = 0; c < 15; c++)
	{
		const uint16_t off = U16(raw, base + 8 + 4 * c), n = U16(raw, base + 10 + 4 * c);
		for (int k = 0; k < n; k++)
		{
			auto it = byOff.find(U16(raw, base + off + 2 * k));
			if (it == byOff.end() || it->second >= (int)md.m.commands.size()) continue;
			md.cat[c].push_back(it->second);
			md.catOf[it->second] = c;
		}
	}
	return true;
}

static std::string KeyOf(const std::string &pack, int index)
{
	char b[64]; snprintf(b, sizeof b, "%s[%d]", pack.c_str(), index); return b;
}
static std::string ObjArt(const MapData &md, uint16_t word)
{
	int idx = 0;
	const bool sh = Pack::ResolveObjectArt((uint8_t)(word & 0xff), (uint8_t)(word >> 8), idx);
	return KeyOf(sh ? TheInstall().SharedPack() : md.pack, idx);
}
static std::string TexKey(const MapData &md, uint16_t tex)
{
	if (tex == 0) return "none";
	if (tex >= 0x8000) return "colour";
	int idx = 0;
	const bool sh = Pack::ResolveDasId(tex, idx);
	return KeyOf(sh ? TheInstall().SharedPack() : md.pack, idx);
}
static const TextureMap *TmOf(const Map &m, int face)
{
	if (face < 0 || face >= (int)m.faces.size()) return nullptr;
	const int t = m.faces[face].textureMap;
	return (t >= 0 && t < (int)m.textureMaps.size()) ? &m.textureMaps[t] : nullptr;
}
static double FaceLen(const Map &m, int fi)
{
	const Face &f = m.faces[fi];
	return hypot(double(m.vertices[f.vertex2].x) - m.vertices[f.vertex1].x, double(m.vertices[f.vertex2].y) - m.vertices[f.vertex1].y);
}
// A face as "f<index> s<sector> <piece>=PACK[i] len L h H" (H = the opening the piece spans).
static std::string FaceStr(const MapData &md, int fi, std::set<std::string> *pics)
{
	const Map &m = md.m;
	const Face &f = m.faces[fi];
	const TextureMap *tm = TmOf(m, fi);
	std::string s; char b[160];
	const Sector &sec = m.sectors[f.sector];
	snprintf(b, sizeof b, "f%d s%d len %.0f floor %d ceil %d", fi, f.sector, FaceLen(m, fi), sec.floorHeight, sec.ceilingHeight);
	s = b;
	if (f.sister >= 0)
	{
		const Sector &o = m.sectors[m.faces[f.sister].sector];
		snprintf(b, sizeof b, " | across s%d floor %d ceil %d", m.faces[f.sister].sector, o.floorHeight, o.ceilingHeight);
		s += b;
	}
	if (tm)
	{
		const uint16_t t[3] = { tm->midTexture, tm->upperTexture, tm->lowerTexture };
		const char *nm[3] = { "mid", "up", "low" };
		for (int k = 0; k < 3; k++)
			if (t[k])
			{
				const std::string tk = TexKey(md, t[k]);
				s += std::string(" ") + nm[k] + "=" + tk;
				if (pics && tk[0] != 'c' && tk[0] != 'n') pics->insert(tk);
			}
	}
	return s;
}

struct Step { int idx; std::string text; std::set<std::string> words; };

// Walk NEXT from `start` (1-based), into 0x40 sub-chains; one line per record.
static void Walk(const MapData &md, int start, std::vector<Step> &out, std::set<int> &seen, std::set<std::string> &pics, int depth = 0)
{
	const Map &m = md.m;
	int i = start;
	for (int steps = 0; i > 0 && i <= (int)m.commands.size() && steps < 128; steps++)
	{
		if (!seen.insert(i).second) { out.push_back({ i, "(loops back)", {} }); break; }
		const Command &c = m.commands[i - 1];
		char b[512]; std::string t;
		switch (c.opcode)
		{
		case 0x27: snprintf(b, sizeof b, "0x27 if-not-item %d \"%s\" (flags %02x)", c.key, ItemName(c.key).c_str(), c.fireFlags); break;
		case 0x29: snprintf(b, sizeof b, "0x29 GIVE item %d \"%s\"", c.key, ItemName(c.key).c_str()); break;
		case 0x2a: snprintf(b, sizeof b, "0x2a take item %d \"%s\"", c.key, ItemName(c.key).c_str()); break;
		case 0x2b: case 0x36: snprintf(b, sizeof b, "0x%02x talk %d \"%s\"", c.opcode, c.key, ActionText(c.key).c_str()); break;
		case 0x0d:
		{
			const std::string a = ObjArt(md, c.Word(0x0c));
			pics.insert(a);
			snprintf(b, sizeof b, "0x0d OBJTEX obj %d -> %04x = %s", c.key, c.Word(0x0c), a.c_str());
			break;
		}
		case 0x20: snprintf(b, sizeof b, "0x20 objtex-cycle obj %d words %04x %04x %04x", c.key, c.Word(0x0a), c.Word(0x0c), c.Word(0x0e)); break;
		case 0x3a: snprintf(b, sizeof b, "0x3a objid -> %d", c.Word(6) == 0xffff ? -1 : c.Word(6)); break;
		case 0x34:
		{
			const std::string tk = TexKey(md, c.Word(0x0a));
			if (tk[0] != 'c' && tk[0] != 'n') pics.insert(tk);
			snprintf(b, sizeof b, "0x34 WALLTEX face-id %d -> %s (fl %02x) on %zu faces", c.key, tk.c_str(), c.fireFlags, c.faces.size());
			break;
		}
		case 0x0c: snprintf(b, sizeof b, "0x0c WALLTEX+ key %d words %04x %04x %04x", c.key, c.Word(0x0a), c.Word(0x0c), c.Word(0x0e)); break;
		case 0x2e: snprintf(b, sizeof b, "0x2e SMASH face-id %d (fl %02x)", c.key, c.fireFlags); break;
		case 0x0a: case 0x0b: snprintf(b, sizeof b, "0x%02x floortex sector %d -> %s", c.opcode, c.key, TexKey(md, c.Word(0x0a)).c_str()); break;
		case 0x07: case 0x03: case 0x09:
		{
			std::string secs;
			for (size_t s = 0; s < m.sectors.size(); s++) if (m.sectors[s].commandID == c.key && c.key) secs += " s" + std::to_string(s);
			snprintf(b, sizeof b, "0x%02x %s sector-id %d [%s ] fl %02x mod %02x limits %d %d step %d sfx %d", c.opcode,
				c.opcode == 0x07 ? "HEIGHT" : c.opcode == 0x09 ? "MOVESECTOR" : "MODSECTOR", c.key, secs.c_str(), c.fireFlags, c.modifier,
				(int16_t)c.Word(0x0a) * 2, (int16_t)c.Word(0x0c) * 2, c.subFlags, c.Word(0x14));
			break;
		}
		case 0x23: snprintf(b, sizeof b, "0x23 OBJHEIGHT obj %d limits %d %d", c.key, (int16_t)c.Word(0x0a), (int16_t)c.Word(0x0c)); break;
		case 0x24: snprintf(b, sizeof b, "0x24 OBJROTATE obj %d words %04x %04x", c.key, c.Word(0x0a), c.Word(0x0c)); break;
		case 0x2f:
		{
			std::string fs;
			if (c.key == 0) fs = " (the face used)";
			else for (int fi : c.faces) fs += " s" + std::to_string(m.faces[fi].sector);
			snprintf(b, sizeof b, "0x2f DOOR face-id %d%s extent %d sound %d", c.key, fs.c_str(), c.Word(0x0a), c.Word(0x0c));
			break;
		}
		case 0x16: case 0x3c: snprintf(b, sizeof b, "0x%02x SPAWN/SHOW obj %d words %04x %04x", c.opcode, c.key, c.Word(0x0a), c.Word(0x0c)); break;
		case 0x10: snprintf(b, sizeof b, "0x10 sfx-node %d", c.key); break;
		case 0x12: snprintf(b, sizeof b, "0x12 delay %d ticks%s", c.Word(6), (c.modifier & 2) ? " (randomised)" : ""); break;   // word +6, cmd_delay_timer raw_commands.c:4420
		case 0x26: snprintf(b, sizeof b, "0x26 set-flag %d", c.key); break;
		case 0x28: snprintf(b, sizeof b, "0x28 if-not-flag %d", c.key); break;
		case 0x1d: snprintf(b, sizeof b, "0x1d light group %d step %d", c.key, (int)(int8_t)c.subFlags); break;
		case 0x17: snprintf(b, sizeof b, "0x17 toggle record %d", c.key); break;
		case 0x40: snprintf(b, sizeof b, "0x40 run record %d", c.Word(6)); break;
		default:
		{
			std::string a;
			for (size_t k = 0; k < c.args.size(); k++) { char w[8]; snprintf(w, sizeof w, " %04x", c.args[k]); a += w; }
			snprintf(b, sizeof b, "0x%02x key %d args%s", c.opcode, c.key, a.c_str());
		}
		}
		t = b;
		if (c.modifier & 8) t += " [disabled]";
		out.push_back({ i, t, {} });
		if (c.opcode == 0x40 && depth < 4)
		{
			std::vector<Step> sub;
			Walk(md, c.Word(6), sub, seen, pics, depth + 1);
			for (Step &s : sub) { s.text = "    " + s.text; out.push_back(s); }
		}
		if (c.isTrigger) break;
		i = c.nextIndex;
	}
}

static std::string Lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }

static bool WritePPM(const std::string &path, const Image &img, const std::vector<Colour> &pal)
{
	if (!img.ok()) return false;
	const int W = img.height, H = img.width;   // stored column-major
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	fprintf(f, "P6 %d %d 255\n", W, H);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			const uint8_t ci = img.frames[0][(size_t)x * img.width + y];
			Colour c = { 255, 0, 255 };
			if (!(ci == 0 && img.ZeroIsTransparent()) && ci < pal.size()) c = pal[ci];
			fputc(c.r, f); fputc(c.g, f); fputc(c.b, f);
		}
	fclose(f);
	return true;
}

//---------------------------------------------------------------------------
// -area MAP [-stop ID,ID] [-have I,I] [-dump DIR]
//
// What the player can reach ON FOOT from the map's start, room by room, and every scripted record on what he can
// reach. Written for the chapter-by-chapter VR design (docs/VR_CHAPTER01_SHADOWS.md): a chapter is a reachable
// region of its maps, and the census tools do not model space.
//
// THE RULES (each an approximation, stated so the output can be judged):
//  - start: the sector holding metadata (startX, startY) whose floor..ceiling holds startZ (rothplan::SectorAt's
//    rule, roth_portalplan.h); heights after ApplyLoadTimeHeights (the load-time 0x07 pass).
//  - a step from sector A to B crosses a two-sided face the player's sweep passes (face flags & 0x81 != 0x01:
//    collide_point_walls_recursive, collision_physics.c:705-735, with the mask 0x81 set at :1706), climbing at
//    most metadata MaxClimb, with an opening of at least MinFit (the player's height) between max(floor) and
//    min(ceiling).
//  - a DOOR sector (commandID 0xfffd/0xffff, closed at load) is entered only from a room whose face onto it carries
//    a LIVE (not disabled at load) use-wall record (0x18) whose chain reaches 0x2f, with every item its chain tests
//    (0x27) already in hand-able reach (-have, plus everything given on the reached area so far), or when a live
//    chain on the reached area opens it remotely (0x2f with a face id). Flags (0x28) are NOT evaluated: a chain is
//    walked along NEXT and into every 0x38 else-branch and 0x40 sub-chain, so a door behind a flag test counts as
//    openable. Every such door is printed with its chain so the call can be checked by hand.
//  - 0x17 enables are NOT followed (a record disabled at load stays disabled), and 0x07/0x09 movers are NOT
//    applied: they are listed under the record that runs them.
//  - -stop: sectors with one of these command ids are reached but not walked through (a chapter's end).
//  - a ROOM is a set of sectors connected without passing through a door sector. Rooms are numbered in the order
//    the walk first reaches them (breadth first from the start).
//  - TOUCHABLE: the reached sectors plus every sector one or two two-sided faces away from them, whatever its heights
//    (table tops, shelves, niches): the original clicks things up to 268 units away, so their records are listed
//    too, marked "adjacent".
//  Items given (0x29) by any live record on the touchable area, and plain pickups lying there, are added to the
//  items in hand-able reach, and the walk repeats until nothing changes.
//---------------------------------------------------------------------------
static std::set<int> ParseList(const char *s)
{
	std::set<int> o; std::string t = s; size_t p;
	while ((p = t.find(',')) != std::string::npos) { if (p) o.insert(atoi(t.substr(0, p).c_str())); t = t.substr(p + 1); }
	if (!t.empty()) o.insert(atoi(t.c_str()));
	return o;
}
static int AreaStartSector(const Map &m)
{
	const double x = m.metadata.startX, y = m.metadata.startY, z = m.metadata.startZ;
	int best = -1; double bestScore = 1e18;
	for (int s = 0; s < (int)m.sectors.size(); s++)
	{
		const Sector &rs = m.sectors[s];
		if (rs.faceCount < 3 || rs.firstFaceIndex < 0) continue;
		bool in = false;
		for (int k = 0; k < rs.faceCount; k++)
		{
			const Face &f = m.faces[rs.firstFaceIndex + k];
			const double ax = m.vertices[f.vertex1].x, ay = m.vertices[f.vertex1].y;
			const double bx = m.vertices[f.vertex2].x, by = m.vertices[f.vertex2].y;
			if ((ay > y) != (by > y)) { const double xi = ax + (y - ay) * (bx - ax) / (by - ay); if (xi > x) in = !in; }
		}
		if (!in) continue;
		double score;
		if (z >= rs.floorHeight && z <= rs.ceilingHeight) score = z - rs.floorHeight;
		else if (z > rs.ceilingHeight) score = 1e6 + (z - rs.ceilingHeight);
		else score = 2e6 + (rs.floorHeight - z);
		if (score < bestScore) { bestScore = score; best = s; }
	}
	return best;
}
struct AreaChain { bool door0 = false; std::set<int> itemsTested, given, remoteFaces, toggles, flagsTested, heights; };
static void AreaWalk(const Map &m, int start, AreaChain &ac, std::set<int> &seen, int depth = 0)
{
	int i = start;
	for (int steps = 0; i > 0 && i <= (int)m.commands.size() && steps < 128; steps++)
	{
		if (!seen.insert(i).second) break;
		const Command &c = m.commands[i - 1];
		switch (c.opcode)
		{
		case 0x27: ac.itemsTested.insert(c.key); break;
		case 0x29: if (c.key) ac.given.insert(c.key); break;
		case 0x28: ac.flagsTested.insert(c.key); break;
		case 0x17: ac.toggles.insert(c.key); break;
		case 0x07: case 0x09: ac.heights.insert(c.key); break;
		case 0x2f: if (c.key == 0) ac.door0 = true; else for (int fi : c.faces) ac.remoteFaces.insert(fi); break;
		case 0x38: if (depth < 6) AreaWalk(m, c.key, ac, seen, depth + 1); break;
		case 0x40: if (depth < 6) AreaWalk(m, c.Word(6), ac, seen, depth + 1); break;
		default: break;
		}
		if (c.isTrigger) break;
		i = c.nextIndex;
	}
}
static AreaChain AreaChainOf(const Map &m, int ci)
{
	AreaChain ac; std::set<int> seen;
	AreaWalk(m, m.commands[ci].chainStart, ac, seen);
	return ac;
}
static std::string AreaShort(const MapData &md, int ci)
{
	std::vector<Step> st; std::set<int> seen; std::set<std::string> pics;
	Walk(md, md.m.commands[ci].chainStart, st, seen, pics);
	std::string s;
	for (const Step &x : st) { s += "\n        #" + std::to_string(x.idx) + " " + x.text; }
	return s;
}
static int AreaFirstPickable(int rid)
{
	if (rid < 0x200) return 0;
	for (int id = 1; id <= gDb.ItemCount(); id++)
	{
		const DbItem *it = gDb.Item(id);
		if (!it || !it->ok || it->renderId != rid) continue;
		if (it->flags & 0x80) continue;
		if (it->Category() == 2) continue;
		return id;
	}
	return 0;
}
// Picture size in world units (columns x rows x units per texel; wickspots' rule), or the kind when not a picture.
static std::string AreaArtSize(const MapData &md, uint16_t word, const std::string &dumpDir)
{
	int idx = 0;
	const bool sh = Pack::ResolveObjectArt((uint8_t)(word & 0xff), (uint8_t)(word >> 8), idx);
	const std::string pack = sh ? TheInstall().SharedPack() : md.pack;
	LoadedPack *lp = GetPack(pack);
	if (!lp->ok) return "?";
	const FatEntry *e = lp->pack.Entry(idx);
	if (!e) return "?";
	if (e->kind == EntryKind::Object3D) return "mesh";
	if (e->kind == EntryKind::Indirection) return "directional/creature";
	Image im = lp->pack.ReadImage(idx);
	if (!im.ok()) return "?";
	double upp = 1.0;
	if (im.modifier & IM_HALF_SIZE) { const int s = (im.imageType >> 5) & 3; upp = s == 0 ? 1.0 : 2.0 * (1 << s); }
	if (!dumpDir.empty())
	{
		const std::string p = dumpDir + "/" + KeyOf(pack, idx) + ".ppm";
		FILE *t = fopen(p.c_str(), "rb");
		if (t) fclose(t); else WritePPM(p, im, lp->pack.Palette());
	}
	char b[64]; snprintf(b, sizeof b, "%gx%g u", im.height * upp, im.width * upp);
	return b;
}
static void AreaDumpTex(const MapData &md, uint16_t tex, const std::string &dumpDir)
{
	if (dumpDir.empty() || tex == 0 || tex >= 0x8000) return;
	int idx = 0;
	const bool sh = Pack::ResolveDasId(tex, idx);
	const std::string pack = sh ? TheInstall().SharedPack() : md.pack;
	LoadedPack *lp = GetPack(pack);
	if (!lp->ok) return;
	const std::string p = dumpDir + "/" + KeyOf(pack, idx) + ".ppm";
	FILE *t = fopen(p.c_str(), "rb");
	if (t) { fclose(t); return; }
	Image im = lp->pack.ReadImage(idx);
	if (im.ok()) WritePPM(p, im, lp->pack.Palette());
}

static int AreaMode(int argc, char **argv)
{
	MapData md;
	if (!LoadMap(argv[3], md)) { printf("map %s: not read\n", argv[3]); return 2; }
	std::set<int> stopIds, have; std::string dumpDir, polyFile;
	for (int a = 4; a + 1 < argc; a++)
	{
		if (!strcmp(argv[a], "-stop")) stopIds = ParseList(argv[a + 1]);
		else if (!strcmp(argv[a], "-have")) have = ParseList(argv[a + 1]);
		else if (!strcmp(argv[a], "-dump")) dumpDir = argv[a + 1];
		else if (!strcmp(argv[a], "-poly")) polyFile = argv[a + 1];
	}
	ApplyLoadTimeHeights(md.m);
	const Map &m = md.m;
	const int NS = (int)m.sectors.size();
	const int climb = m.metadata.MaxClimb(), fit = m.metadata.MinFit();
	const int s0 = AreaStartSector(m);
	printf("AREA %s: start sector s%d (start %d,%d z %d), max climb %d, min fit %d, stop ids", argv[3], s0,
		m.metadata.startX, m.metadata.startY, m.metadata.startZ, climb, fit);
	for (int v : stopIds) printf(" %d", v);
	printf(", items at start");
	for (int v : have) printf(" i%d", v);
	printf("\n");
	if (s0 < 0) return 2;

	auto across = [&](int fi) -> int { const Face &f = m.faces[fi]; return f.sister >= 0 ? m.faces[f.sister].sector : -1; };
	// Door openers per (room face): live 0x18 records with a 0x2f.
	struct Opener { int rec; int room; int door; AreaChain ac; };
	std::vector<Opener> openers;
	for (int ci : md.cat[CAT_USE_WALL])
	{
		const Command &c = m.commands[ci];
		if (c.disabled) continue;
		AreaChain ac = AreaChainOf(m, ci);
		if (!ac.door0) continue;
		for (int fi : c.faces)
		{
			const int a = m.faces[fi].sector, b = across(fi);
			if (b >= 0 && m.sectors[b].IsDoor() && !m.sectors[a].IsDoor()) openers.push_back({ ci + 1, a, b, ac });
			else if (b >= 0 && m.sectors[a].IsDoor() && !m.sectors[b].IsDoor()) openers.push_back({ ci + 1, b, a, ac });
		}
	}

	std::vector<int> order, depthOf(NS, -1), viaDoor(NS, -1);
	std::vector<char> reached(NS, 0), touch(NS, 0);
	std::set<int> openAll;                       // doors opened remotely
	std::map<int, std::set<int>> openFrom;       // door -> rooms it opens from
	std::map<int, std::string> openWhy;          // door -> how
	std::set<int> gotItems = have;
	std::map<int, std::string> itemWhere;
	for (int pass = 0; pass < 32; pass++)
	{
		// Doors openable with what is in reach now.
		for (const Opener &o : openers)
		{
			bool ok = true;
			for (int it : o.ac.itemsTested) if (!gotItems.count(it)) ok = false;
			if (ok && openFrom[o.door].insert(o.room).second)
				openWhy[o.door] += " #" + std::to_string(o.rec) + "(from s" + std::to_string(o.room) + ")";
		}
		// Walk.
		std::fill(reached.begin(), reached.end(), 0); order.clear();
		std::fill(depthOf.begin(), depthOf.end(), -1);
		std::vector<int> q{ s0 }; reached[s0] = 1; depthOf[s0] = 0;
		for (size_t qi = 0; qi < q.size(); qi++)
		{
			const int a = q[qi]; order.push_back(a);
			const Sector &A = m.sectors[a];
			if (stopIds.count(A.commandID) && a != s0) continue;
			for (int k = 0; k < A.faceCount; k++)
			{
				const int fi = A.firstFaceIndex + k;
				const Face &f = m.faces[fi];
				const int b = across(fi);
				if (b < 0 || reached[b]) continue;
				const Sector &B = m.sectors[b];
				// The player's sweep (collide_point_walls_recursive, collision_physics.c:705-735, mask 0x81 set at
				// :1706): face flags & 0x81 == 0 passes, bit 0 without 0x80 is solid, 0x80 passes (a step).
				if ((f.collisionFlags & 0x81) == 0x01) continue;
				if (B.IsDoor() && !A.IsDoor() && !openAll.count(b) && !openFrom[b].count(a)) continue;
				if (!B.IsDoor() && !A.IsDoor())
				{
					if (B.floorHeight - A.floorHeight > climb) continue;
					if (std::min(A.ceilingHeight, B.ceilingHeight) - std::max(A.floorHeight, B.floorHeight) < fit) continue;
				}
				reached[b] = 1; depthOf[b] = depthOf[a] + 1;
				if (B.IsDoor()) viaDoor[b] = a;
				q.push_back(b);
			}
		}
		// Two rings of portal neighbours (desk tops behind a typewriter, a window bay's shutters), never through a
		// closed door sector.
		std::fill(touch.begin(), touch.end(), 0);
		for (int s : order) touch[s] = 1;
		for (int ring = 0; ring < 2; ring++)
		{
			std::vector<char> next = touch;
			for (int s = 0; s < NS; s++)
			{
				if (!touch[s]) continue;
				const Sector &A = m.sectors[s];
				for (int k = 0; k < A.faceCount; k++) { const int b = across(A.firstFaceIndex + k); if (b >= 0 && !m.sectors[b].IsDoor()) next[b] = 1; }
			}
			touch.swap(next);
		}
		// Items and remote doors from live records on the touchable area.
		const size_t before = gotItems.size() + openAll.size();
		auto take = [&](int ci, const char *what) {
			const Command &c = m.commands[ci];
			if (c.disabled) return;
			AreaChain ac = AreaChainOf(m, ci);
			for (int it : ac.given) if (gotItems.insert(it).second) itemWhere[it] = std::string(what) + " #" + std::to_string(ci + 1);
			for (int fi : ac.remoteFaces)
			{
				int d = m.sectors[m.faces[fi].sector].IsDoor() ? m.faces[fi].sector : across(fi);
				if (d >= 0 && m.sectors[d].IsDoor() && openAll.insert(d).second) openWhy[d] += " remote #" + std::to_string(ci + 1);
			}
		};
		for (int ci : md.cat[CAT_CLICK_OBJ])
		{
			const Command &c = m.commands[ci];
			bool on = false;
			for (int s = 0; s < NS && !on; s++) if (touch[s]) for (const Object &o : m.objects[s]) if (c.key && o.commandID == c.key && !(o.flags & 0x80)) on = true;
			if (on) take(ci, "click");
		}
		for (int ci : md.cat[CAT_USE_WALL])
		{
			bool on = false;
			for (int fi : m.commands[ci].faces) if (reached[m.faces[fi].sector]) on = true;
			if (on) take(ci, "use-wall");
		}
		for (int ci : md.cat[CAT_USE_FLOOR])
		{
			const Command &c = m.commands[ci];
			bool on = false;
			for (int s = 0; s < NS; s++) if (touch[s] && c.key && m.sectors[s].commandID == c.key) on = true;
			if (on) take(ci, "use-floor");
		}
		for (int s = 0; s < NS; s++)
			if (touch[s])
				for (const Object &o : m.objects[s])
				{
					if (o.flags & 0x80) continue;
					bool live = false;
					for (int ci : md.cat[CAT_CLICK_OBJ]) if (o.commandID && m.commands[ci].key == o.commandID && !m.commands[ci].disabled) live = true;
					if (live || (o.renderType & 0x13)) continue;
					const int it = AreaFirstPickable(o.textureIndex | (o.textureSource << 8));
					if (it && gotItems.insert(it).second) itemWhere[it] = "pickup s" + std::to_string(s);
				}
		if (gotItems.size() + openAll.size() == before && pass > 0) break;
	}

	// Rooms: components of reached non-door sectors joined without crossing a door sector.
	std::vector<int> roomOf(NS, -1); int nRooms = 0;
	std::vector<int> roomFirst;
	for (int s : order)
	{
		if (m.sectors[s].IsDoor() || roomOf[s] >= 0) continue;
		std::vector<int> st{ s }; roomOf[s] = nRooms; roomFirst.push_back(s);
		while (!st.empty())
		{
			const int a = st.back(); st.pop_back();
			const Sector &A = m.sectors[a];
			for (int k = 0; k < A.faceCount; k++)
			{
				const int b = across(A.firstFaceIndex + k);
				if (b < 0 || !reached[b] || roomOf[b] >= 0 || m.sectors[b].IsDoor()) continue;
				roomOf[b] = nRooms; st.push_back(b);
			}
		}
		nRooms++;
	}
	// Adjacent-only sectors join the room they touch first (two passes: the second ring).
	for (int pass = 0; pass < 2; pass++)
		for (int s = 0; s < NS; s++)
		{
			if (roomOf[s] < 0) continue;
			const Sector &A = m.sectors[s];
			for (int k = 0; k < A.faceCount; k++)
			{
				const int b = across(A.firstFaceIndex + k);
				if (b >= 0 && touch[b] && !reached[b] && roomOf[b] < 0) roomOf[b] = roomOf[s];
			}
		}
	if (!polyFile.empty())
	{
		// Plan-view dump for a drawing: S idx state id floor ceil light x y x y ... ; O sector x y id word records
		FILE *pf = fopen(polyFile.c_str(), "w");
		if (pf)
		{
			for (int s = 0; s < NS; s++)
			{
				const Sector &S = m.sectors[s];
				fprintf(pf, "S %d %d %d %d %d %d %d", s, reached[s] ? 2 : touch[s] ? 1 : 0, S.commandID, S.floorHeight, S.ceilingHeight, S.light, S.IsDoor() ? 1 : 0);
				for (int k = 0; k < S.faceCount; k++) { const Face &f = m.faces[S.firstFaceIndex + k]; fprintf(pf, " %d %d", m.vertices[f.vertex1].x, m.vertices[f.vertex1].y); }
				fprintf(pf, "\n");
				for (int k = 0; k < S.faceCount; k++)
				{
					const int fi = S.firstFaceIndex + k;
					std::string recs;
					for (int c : { 4, 6, 10 }) for (int ci : md.cat[c]) for (int f2 : m.commands[ci].faces) if (f2 == fi) recs += (recs.empty() ? "" : ",") + std::to_string(ci + 1);
					if (recs.empty()) continue;
					const Face &f = m.faces[fi];
					fprintf(pf, "F %d %d %d %d %d %d %s\n", s, m.vertices[f.vertex1].x, m.vertices[f.vertex1].y, m.vertices[f.vertex2].x, m.vertices[f.vertex2].y, fi, recs.c_str());
				}
				for (const Object &o : m.objects[s])
				{
					std::string recs;
					if (o.commandID) for (int c : { 0, 7, 12, 14 }) for (int ci : md.cat[c]) if (m.commands[ci].key == o.commandID) recs += (recs.empty() ? "" : ",") + std::to_string(ci + 1);
					fprintf(pf, "O %d %d %d %d %d %d %s\n", s, o.x, o.y, o.commandID, (o.textureIndex | (o.textureSource << 8)), (o.flags & 0x80) ? 1 : 0, recs.empty() ? "-" : recs.c_str());
				}
			}
			fprintf(pf, "P %d %d\n", m.metadata.startX, m.metadata.startY);
			fclose(pf);
		}
	}
	printf("reached %zu sectors in %d rooms; touchable (adjacent, not walked) %d more\n", order.size(), nRooms,
		(int)std::count(touch.begin(), touch.end(), 1) - (int)order.size());
	printf("items in hand-able reach:");
	for (int it : gotItems) printf(" i%d %s%s%s;", it, ItemName(it).c_str(), itemWhere.count(it) ? " <- " : "", itemWhere.count(it) ? itemWhere[it].c_str() : "");
	printf("\n");

	static const struct { int cat; const char *tag; } allCats[] = {
		{ 0, "CLICK" }, { 3, "ENTER" }, { 4, "USEW" }, { 5, "USEF" }, { 6, "HITW" }, { 7, "HITO" }, { 10, "EXW" },
		{ 11, "EXF" }, { 12, "EXO" }, { 14, "WALKO" } };
	// Which entry records' chains reach each record (NEXT, 0x38 else, 0x40 sub-chains), for "shown by".
	std::map<int, std::set<int>> entryOf;
	for (size_t ci = 0; ci < m.commands.size(); ci++)
	{
		const Command &c = m.commands[ci];
		if (!c.isTrigger) continue;
		std::vector<int> stack{ c.chainStart }; std::set<int> seen;
		while (!stack.empty())
		{
			int i = stack.back(); stack.pop_back();
			for (int steps = 0; i > 0 && i <= (int)m.commands.size() && steps < 256; steps++)
			{
				if (!seen.insert(i).second) break;
				const Command &x = m.commands[i - 1];
				entryOf[i].insert((int)ci + 1);
				if (x.opcode == 0x38) stack.push_back(x.key);
				if (x.opcode == 0x40) stack.push_back(x.Word(6));
				if (x.isTrigger) break;
				i = x.nextIndex;
			}
		}
	}
	std::set<int> printed;
	auto recLine = [&](int ci, const char *tag) {
		const Command &c = m.commands[ci];
		if (!printed.insert(ci).second) return std::string(tag) + " #" + std::to_string(ci + 1) + " (chain printed above)";
		std::string s = std::string(tag) + " #" + std::to_string(ci + 1);
		if (c.disabled) s += " [off]";
		if (c.fireFlags & 0x10) s += " [once]";
		if (!strcmp(tag, "EXW") || !strcmp(tag, "EXF") || !strcmp(tag, "EXO"))
			s += " says " + std::to_string(c.aux) + " \"" + ActionText(c.aux) + "\"";
		else if (!strcmp(tag, "CLICK") && c.opcode == 0x02) s += " LIGHT SWITCH group " + std::to_string(c.aux);
		else s += " sound " + std::to_string((int)c.aux - 1);
		s += AreaShort(md, ci);
		return s;
	};

	for (int r = 0; r < nRooms; r++)
	{
		std::vector<int> secs;
		for (int s : order) if (roomOf[s] == r) secs.push_back(s);
		for (int s = 0; s < NS; s++) if (roomOf[s] == r && !reached[s]) secs.push_back(s);
		int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
		for (int s : secs) if (reached[s])
			for (int k = 0; k < m.sectors[s].faceCount; k++)
			{
				const Face &f = m.faces[m.sectors[s].firstFaceIndex + k];
				for (int v : { f.vertex1, f.vertex2 })
				{
					x0 = std::min(x0, (int)m.vertices[v].x); x1 = std::max(x1, (int)m.vertices[v].x);
					y0 = std::min(y0, (int)m.vertices[v].y); y1 = std::max(y1, (int)m.vertices[v].y);
				}
			}
		printf("\n=== ROOM R%d (first sector s%d, walk depth %d): box x %d..%d y %d..%d (%d x %d units)\n    sectors:", r,
			roomFirst[r], depthOf[roomFirst[r]], x0, x1, y0, y1, x1 - x0, y1 - y0);
		for (int s : secs)
			printf(" s%d%s(id %d fl %d ce %d L%d%s)", s, reached[s] ? "" : "[adj]", m.sectors[s].commandID, m.sectors[s].floorHeight,
				m.sectors[s].ceilingHeight, m.sectors[s].light, (m.sectors[s].flags & 2) ? " LANTERN" : "");
		printf("\n");
		{
			int nl = 0, nw = 0;
			for (int s : secs) if (reached[s]) { nw++; if (m.sectors[s].flags & 2) nl++; }
			// Sector byte +0x0a bit 1 as the FILE stores it: the lights-out ("candle"/lantern-cone) mode,
			// REMAROTH_MEASURED "Candle (lantern) sectors"; the load-time switches add theirs (=== LIGHT below).
			printf("    lantern-cone sectors in the file: %d of %d walked\n", nl, nw);
		}
		for (int s : secs)
		{
			const Sector &S = m.sectors[s];
			// Objects.
			for (size_t oi = 0; oi < m.objects[s].size(); oi++)
			{
				const Object &o = m.objects[s][oi];
				const uint16_t word = (uint16_t)(o.textureIndex | (o.textureSource << 8));
				std::vector<std::string> recs;
				if (o.commandID)
					for (const auto &cc : allCats)
						if (cc.cat == 0 || cc.cat == 7 || cc.cat == 12 || cc.cat == 14)
							for (int ci : md.cat[cc.cat]) if (m.commands[ci].key == o.commandID) recs.push_back(recLine(ci, cc.tag));
				const int it = AreaFirstPickable(word);
				bool liveClick = false;
				for (int ci : md.cat[CAT_CLICK_OBJ]) if (o.commandID && m.commands[ci].key == o.commandID && !m.commands[ci].disabled) liveClick = true;
				const bool pickup = it && !liveClick && !(o.renderType & 0x13);
				const DbItem *di = it ? gDb.Item(it) : nullptr;
				const bool inspect = di && di->FindBlock(4) != nullptr;
				if (recs.empty() && !pickup) continue;
				printf("  OBJ s%d.%zu %s %s at (%d,%d) z %d (floor %+d) id %d fl %02x rt %02x%s%s%s\n", s, oi, ObjArt(md, word).c_str(),
					AreaArtSize(md, word, dumpDir).c_str(), o.x, o.y, o.z, (int)o.z - (int)S.floorHeight, o.commandID, o.flags, o.renderType,
					(o.flags & 0x80) ? " HIDDEN" : "", reached[s] ? "" : " [adjacent sector]",
					pickup ? (" PLAIN PICKUP i" + std::to_string(it) + " " + ItemName(it) + (inspect ? " (OnInspect)" : "")).c_str() : "");
				if (o.flags & 0x80)
				{
					// Who can show it: every 0x16/0x3c naming its id (cmd_spawn_object :622 toggles the hidden bit;
					// cmd_spawn_object_adv :4108), wherever the record sits.
					std::string by;
					for (size_t ci = 0; ci < m.commands.size(); ci++)
						if ((m.commands[ci].opcode == 0x16 || m.commands[ci].opcode == 0x3c) && o.commandID && m.commands[ci].key == o.commandID)
						{
							by += " #" + std::to_string(ci + 1);
							auto it = entryOf.find((int)ci + 1);
							if (it != entryOf.end()) { by += " (in the chain of"; for (int e : it->second) by += " #" + std::to_string(e); by += ")"; }
							else by += " (in no entry's chain: a DBASE100 0x23/0x36 or a timer may run it)";
						}
					printf("    hidden at load; shown by:%s\n", by.empty() ? " nothing in this map" : by.c_str());
				}
				for (const std::string &l : recs) printf("    %s\n", l.c_str());
			}
			// Faces.
			for (int k = 0; k < S.faceCount; k++)
			{
				const int fi = S.firstFaceIndex + k;
				std::vector<std::string> recs;
				for (const auto &cc : allCats)
					if (cc.cat == 4 || cc.cat == 6 || cc.cat == 10)
						for (int ci : md.cat[cc.cat])
							for (int f2 : m.commands[ci].faces) if (f2 == fi) recs.push_back(recLine(ci, cc.tag));
				const int b = across(fi);
				const bool doorEdge = b >= 0 && m.sectors[b].IsDoor() && !S.IsDoor();
				if (recs.empty() && !doorEdge) continue;
				if (const TextureMap *tm = TmOf(m, fi)) { AreaDumpTex(md, tm->midTexture, dumpDir); AreaDumpTex(md, tm->upperTexture, dumpDir); AreaDumpTex(md, tm->lowerTexture, dumpDir); }
				printf("  FACE %s%s\n", FaceStr(md, fi, nullptr).c_str(), reached[s] ? "" : " [adjacent sector]");
				if (doorEdge)
				{
					const bool open = openAll.count(b) || openFrom[b].count(s);
					printf("    DOOR s%d %s here;%s; leads to:", b, open ? "OPENS from" : "CLOSED from", openWhy.count(b) ? openWhy[b].c_str() : " no live opener");
					const Sector &D = m.sectors[b];
					for (int j = 0; j < D.faceCount; j++) { const int o2 = across(D.firstFaceIndex + j); if (o2 >= 0 && o2 != s) printf(" s%d%s", o2, reached[o2] ? "" : "(not reached)"); }
					printf("\n");
				}
				for (const std::string &l : recs) printf("    %s\n", l.c_str());
			}
			// Floor / sector entries.
			std::vector<std::string> recs;
			for (const auto &cc : allCats)
				if (cc.cat == 3 || cc.cat == 5 || cc.cat == 11)
					for (int ci : md.cat[cc.cat]) if (m.commands[ci].key && m.commands[ci].key == S.commandID) recs.push_back(recLine(ci, cc.tag));
			if (!recs.empty())
			{
				printf("  SECTOR s%d id %d floor %d ceil %d floor pic %s%s\n", s, S.commandID, S.floorHeight, S.ceilingHeight,
					TexKey(md, S.floorTexture).c_str(), reached[s] ? "" : " [adjacent sector]");
				AreaDumpTex(md, S.floorTexture, dumpDir);
				for (const std::string &l : recs) printf("    %s\n", l.c_str());
			}
		}
	}
	// LIGHT. The sector-light groups the area's records ramp (0x1d; record bit 0x04 = flood from the sector the key
	// names through faces without collisionFlags bit 0x08, else every sector with that command id: the two
	// collectors roth_runtime.cpp CollectGeometryGroup restates), and the lights-out groups the armed light switches
	// (base 0x02, modifier & 0x01) set at load (REMAROTH_MEASURED: "Lights out at load").
	auto group = [&](uint16_t key, bool flood) {
		std::vector<int> out;
		if (!flood) { for (int s = 0; s < NS; s++) if (m.sectors[s].commandID == key) out.push_back(s); return out; }
		int start = -1;
		for (int s = 0; s < NS && start < 0; s++) if (m.sectors[s].commandID == key) start = s;
		if (start < 0) return out;
		std::vector<char> seen(NS, 0); std::vector<int> st{ start };
		while (!st.empty() && out.size() < 0xc8)
		{
			const int a = st.back(); st.pop_back();
			if (seen[a]) continue;
			seen[a] = 1; out.push_back(a);
			for (int k = 0; k < m.sectors[a].faceCount; k++)
			{
				const Face &f = m.faces[m.sectors[a].firstFaceIndex + k];
				if ((f.collisionFlags & 0x08) || f.sister < 0) continue;
				st.push_back(m.faces[f.sister].sector);
			}
		}
		return out;
	};
	auto groupLine = [&](const std::vector<int> &g) {
		int inArea = 0; std::string l;
		for (int s : g) if (reached[s] || touch[s]) { inArea++; if (l.size() < 400) l += " s" + std::to_string(s); }
		char b[96]; snprintf(b, sizeof b, "%zu sectors, %d of them on this area:", g.size(), inArea);
		return std::string(b) + l;
	};
	printf("\n=== LIGHT\n");
	for (int ci : md.cat[CAT_CLICK_OBJ])
	{
		const Command &c = m.commands[ci];
		if (c.opcode != 0x02 || !(c.modifier & 0x01) || c.disabled) continue;
		printf("  lights-out at load: switch #%d group %d (flood): %s\n", ci + 1, c.aux, groupLine(group(c.aux, true)).c_str());
	}
	std::set<std::pair<int, int>> shown;
	for (size_t ci = 0; ci < m.commands.size(); ci++)
	{
		const Command &c = m.commands[ci];
		if (c.opcode != 0x1d && c.opcode != 0x02) continue;
		if (c.opcode == 0x02 && md.catOf[ci] == CAT_CLICK_OBJ && c.key != 0) continue;   // the switches themselves: above
		if (c.opcode == 0x02)
		{
			// A light switch run as an instruction (0x40 from a click chain): group word +0x0a, flood (cmd_light_switch).
			auto it2 = entryOf.find((int)ci + 1);
			bool here2 = false;
			if (it2 != entryOf.end()) for (int e : it2->second) if (printed.count(e - 1)) here2 = true;
			if (here2) printf("  switch #%zu group %d (flood) step %d: %s\n", ci + 1, c.aux, (int)(int8_t)c.subFlags, groupLine(group(c.aux, true)).c_str());
			continue;
		}
		auto it = entryOf.find((int)ci + 1);
		if (it == entryOf.end()) continue;
		bool here = false;
		for (int e : it->second) if (printed.count(e - 1)) here = true;
		if (!here || !shown.insert({ c.key, (c.modifier & 4) ? 1 : 0 }).second) continue;
		printf("  ramp #%zu group %d %s step %d: %s\n", ci + 1, c.key, (c.modifier & 4) ? "(flood)" : "(by id)", (int)(int8_t)c.subFlags,
			groupLine(group(c.key, (c.modifier & 4) != 0)).c_str());
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: openables <ROTH DATA> [-grep w1,w2] [-map MAP] [-dump DIR]\n"); return 2; }
	if (!TheInstall().Open(argv[1])) { printf("install: %s\n", TheInstall().Error().c_str()); return 2; }
	std::vector<std::string> words; std::string onlyMap, dumpDir; bool summary = false;
	for (int a = 2; a < argc; a++) if (!strcmp(argv[a], "-summary")) summary = true;
	for (int a = 2; a + 1 < argc; a++)
	{
		if (!strcmp(argv[a], "-grep"))
		{
			std::string w = Lower(argv[a + 1]); size_t p;
			while ((p = w.find(',')) != std::string::npos) { words.push_back(w.substr(0, p)); w = w.substr(p + 1); }
			if (!w.empty()) words.push_back(w);
		}
		else if (!strcmp(argv[a], "-map")) onlyMap = argv[a + 1];
		else if (!strcmp(argv[a], "-dump")) dumpDir = argv[a + 1];
	}
	Log log;
	if (!LoadDbase100FromInstall(TheInstall(), gDb, &log)) { printf("DBASE100 not loaded\n"); return 2; }
	gText = ReadFile(std::string(argv[1]) + "/DBASE400.DAT");

	if (argc > 4 && !strcmp(argv[2], "-sector"))
	{
		// openables <DATA> -sector MAP S [S...]: a sector's box, heights, faces (with what lies across each and
		// its pictures) and the objects in it, so a small door's room and what it hides can be read off.
		MapData md;
		if (!LoadMap(argv[3], md)) { printf("map %s: not read\n", argv[3]); return 2; }
		const Map &m = md.m;
		for (int a = 4; a < argc; a++)
		{
			const int si = atoi(argv[a]);
			if (si < 0 || si >= (int)m.sectors.size()) continue;
			const Sector &sec = m.sectors[si];
			int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
			for (int k = 0; k < sec.faceCount; k++)
			{
				const Face &f = m.faces[sec.firstFaceIndex + k];
				for (int v : { f.vertex1, f.vertex2 })
				{
					x0 = std::min(x0, (int)m.vertices[v].x); x1 = std::max(x1, (int)m.vertices[v].x);
					y0 = std::min(y0, (int)m.vertices[v].y); y1 = std::max(y1, (int)m.vertices[v].y);
				}
			}
			printf("%s s%d: floor %d ceil %d id %d box x %d..%d y %d..%d (%d x %d) floor pic %s ceiling pic %s\n", argv[3], si, sec.floorHeight, sec.ceilingHeight, sec.commandID,
				x0, x1, y0, y1, x1 - x0, y1 - y0, TexKey(md, sec.floorTexture).c_str(), TexKey(md, sec.ceilingTexture).c_str());
			for (int k = 0; k < sec.faceCount; k++) printf("    %s\n", FaceStr(md, sec.firstFaceIndex + k, nullptr).c_str());
			for (size_t oi = 0; oi < m.objects[si].size(); oi++)
			{
				const Object &o = m.objects[si][oi];
				printf("    obj %d.%zu %s at (%d,%d) z %d (floor %+d) id %d fl %02x rt %02x\n", si, oi,
					ObjArt(md, (uint16_t)(o.textureIndex | (o.textureSource << 8))).c_str(), o.x, o.y, o.z, (int)o.z - (int)sec.floorHeight, o.commandID, o.flags, o.renderType);
			}
		}
		return 0;
	}
	if (argc > 3 && !strcmp(argv[2], "-area"))
		return AreaMode(argc, argv);
	std::set<std::string> allPics;
	static const struct { int cat; const char *tag; } cats[] = {
		{ CAT_CLICK_OBJ, "CLICK" }, { CAT_USE_WALL, "USEW" }, { CAT_USE_FLOOR, "USEF" },
		{ CAT_EXAM_OBJ, "EXO" }, { CAT_EXAM_WALL, "EXW" }, { CAT_EXAM_FLOOR, "EXF" } };

	for (const std::string &name : TheInstall().MapNames())
	{
		if (!onlyMap.empty() && name != onlyMap) continue;
		MapData md;
		if (!LoadMap(name, md)) { printf("map %s: not read\n", name.c_str()); continue; }
		const Map &m = md.m;
		// Build every record's block first, so a -grep hit on one record of a target shows them all.
		struct Block { std::string target, head; std::vector<Step> steps; std::string all; std::set<std::string> pics; };
		std::vector<Block> blocks;
		for (const auto &cc : cats)
			for (int ci : md.cat[cc.cat])
			{
				const Command &c = m.commands[ci];
				if (cc.cat == CAT_CLICK_OBJ && c.opcode == 0x02) continue;   // light switches: world_census -switches
				Block bl; char b[256];
				std::string tgt;
				if (cc.cat == CAT_CLICK_OBJ || cc.cat == CAT_EXAM_OBJ)
				{
					tgt = "obj id " + std::to_string(c.key);
					for (size_t si = 0; si < m.objects.size(); si++)
						for (size_t oi = 0; oi < m.objects[si].size(); oi++)
						{
							const Object &o = m.objects[si][oi];
							if (o.commandID != c.key || c.key == 0) continue;
							const std::string art = ObjArt(md, (uint16_t)(o.textureIndex | (o.textureSource << 8)));
							bl.pics.insert(art);
							snprintf(b, sizeof b, "\n      obj %zu.%zu %s at (%d,%d) z %d (floor %+d) rot %d fl %02x rt %02x%s", si, oi, art.c_str(), o.x, o.y, o.z,
								(int)o.z - (int)m.sectors[si].floorHeight, o.rotation, o.flags, o.renderType, (o.flags & 0x80) ? " HIDDEN" : "");
							tgt += b;
						}
				}
				else if (cc.cat == CAT_USE_WALL || cc.cat == CAT_EXAM_WALL)
				{
					tgt = "face id " + std::to_string(c.key);
					for (int fi : c.faces) tgt += "\n      " + FaceStr(md, fi, &bl.pics);
				}
				else
				{
					tgt = "sector id " + std::to_string(c.key);
					for (size_t s = 0; s < m.sectors.size(); s++)
						if (m.sectors[s].commandID == c.key && c.key)
						{
							snprintf(b, sizeof b, "\n      s%zu floor %d ceil %d", s, m.sectors[s].floorHeight, m.sectors[s].ceilingHeight);
							tgt += b;
						}
				}
				const bool exam = cc.cat == CAT_EXAM_OBJ || cc.cat == CAT_EXAM_WALL || cc.cat == CAT_EXAM_FLOOR;
				snprintf(b, sizeof b, "%s %-8s #%d op %02x mod %02x%s box %d %d %d %d", cc.tag, name.c_str(), ci + 1, c.opcode, c.modifier,
					(c.modifier & 8) ? " DISABLED" : "", (int16_t)c.Word(0x0c), (int16_t)c.Word(0x0e), (int16_t)c.Word(0x10), (int16_t)c.Word(0x12));
				bl.head = b;
				// An examine record's +0x0a is the DBASE100 action it says (examine_world_object ->
				// eval_dialogue_record_by_id(word[rec+0xa]), input.c:964). A use trigger's +0x0a is its sound + 1.
				if (exam) bl.head += " says " + std::to_string(c.aux) + " \"" + ActionText(c.aux) + "\"";
				else bl.head += " sound " + std::to_string(c.aux ? c.aux - 1 : -1);
				bl.target = tgt;
				std::set<int> seen;
				Walk(md, c.chainStart, bl.steps, seen, bl.pics);
				bl.all = Lower(bl.head) + " " + Lower(tgt);
				for (const Step &s : bl.steps) bl.all += " " + Lower(s.text);
				blocks.push_back(std::move(bl));
			}
		if (summary)
		{
			// One line per target (object id / face id / sector id): what examining it says, and per use record
			// the chain's opcodes in order, with items, lines and swapped pictures abbreviated.
			std::map<std::string, std::vector<const Block *>> byTarget;
			std::vector<std::string> order;
			for (const Block &bl : blocks)
			{
				const std::string t = bl.target.substr(0, bl.target.find('\n'));
				if (!byTarget.count(t)) order.push_back(t);
				byTarget[t].push_back(&bl);
			}
			for (const std::string &t : order)
			{
				std::string ex, use, pics;
				std::set<std::string> ps;
				for (const Block *bl : byTarget[t])
				{
					for (const std::string &p : bl->pics) { ps.insert(p); allPics.insert(p); }
					const bool isEx = bl->head.compare(0, 2, "EX") == 0;
					if (isEx) { const size_t q = bl->head.find(" says "); ex += " {" + bl->head.substr(q + 6) + "}"; continue; }
					use += " [" + bl->head.substr(0, bl->head.find(' ')) + bl->head.substr(bl->head.find('#'), bl->head.find(' ', bl->head.find('#')) - bl->head.find('#'));
					if (bl->head.find("DISABLED") != std::string::npos) use += " DIS";
					for (const Step &st : bl->steps)
					{
						std::string x = st.text;
						while (!x.empty() && x[0] == ' ') x.erase(0, 1);
						const size_t q = x.find('"');
						if (q != std::string::npos) { const size_t q2 = x.find('"', q + 1); if (q2 != std::string::npos && q2 - q > 30) x = x.substr(0, q + 28) + "..\"" + x.substr(q2 + 1); }
						use += " | " + x;
					}
					use += "]";
				}
				for (const std::string &p : ps) pics += " " + p;
				printf("%-8s %s :%s\n    exam:%s\n    use:%s\n", name.c_str(), t.c_str(), pics.c_str(), ex.empty() ? " -" : ex.c_str(), use.empty() ? " -" : use.c_str());
			}
			continue;
		}
		// -grep: a block is shown when it, or another block naming the same object id / face id, matches.
		std::set<std::string> hitTargets;
		for (const Block &bl : blocks)
			for (const std::string &w : words)
				if (bl.all.find(w) != std::string::npos) hitTargets.insert(bl.target.substr(0, bl.target.find('\n')));
		for (const Block &bl : blocks)
		{
			if (!words.empty() && !hitTargets.count(bl.target.substr(0, bl.target.find('\n')))) continue;
			printf("%s\n    target %s\n", bl.head.c_str(), bl.target.c_str());
			for (const Step &s : bl.steps) printf("    #%-4d %s\n", s.idx, s.text.c_str());
			for (const std::string &p : bl.pics) allPics.insert(p);
		}
	}

	if (!dumpDir.empty())
	{
		int wrote = 0;
		const std::vector<Colour> &dpal = GetPack("DEMO")->pack.Palette();
		for (const std::string &key : allPics)
		{
			const size_t lb = key.find('[');
			if (lb == std::string::npos) continue;
			const std::string pn = key.substr(0, lb);
			const int idx = atoi(key.c_str() + lb + 1);
			LoadedPack *lp = GetPack(pn);
			if (!lp->ok) continue;
			const std::vector<Colour> &pp = lp->pack.Palette().empty() ? dpal : lp->pack.Palette();
			Image im = lp->pack.ReadImage(idx, false);
			if (!im.ok())
			{
				// a directional entry: its first view
				Directional d = lp->pack.ReadDirectional(idx);
				if (d.ok())
				{
					int fi = 0; const bool fs = Pack::ResolveDasId(d.frames[0].entry, fi);
					LoadedPack *fp = fs ? GetPack(TheInstall().SharedPack()) : lp;
					im = fp->pack.ReadImage(fi, false);
				}
			}
			if (WritePPM(dumpDir + "/" + pn + "_" + std::to_string(idx) + ".ppm", im, pp)) wrote++;
		}
		fprintf(stderr, "dump: %d pictures written to %s\n", wrote, dumpDir.c_str());
	}
	return 0;
}
