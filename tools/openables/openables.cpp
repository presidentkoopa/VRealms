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
