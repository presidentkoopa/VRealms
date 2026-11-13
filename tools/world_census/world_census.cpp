// world_census.cpp -- every kind of thing placed in Realms of the Haunting's levels that a VR hand may
// want to touch, counted from the player's own install, so docs/WORLD_DESIGN.md stays reproducible.
//
// Engine-free; reads the .RAW maps, the .DAS packs and DBASE100/400 through the src/roth readers.
// Writes nothing unless asked to dump pictures (and then only to the folder named, never into the repo:
// the pictures are the retail art).
//
//   world_census <DATA>                      all four tables (objects, wall triggers, doors, light switches)
//   world_census <DATA> -objects             only the object-kind table
//   world_census <DATA> -kind DEMO1[4101]    every placement of one object kind, with its scripts
//   world_census <DATA> -faces               only the wall-trigger table (0x18 / 0x32 / 0x1a by texture)
//   world_census <DATA> -doors               one line per door, with its class (KEY / FREE / LATER / NEVER)
//   world_census <DATA> -switches            the light switches (click records of base 0x02)
//   world_census <DATA> -chain MAP N         record N (1-based) and the records its chain walks, raw
//   world_census <DATA> -pic PACK INDEX f    one entry's first picture to f.ppm (art no object is placed with,
//                                            such as the lit state a script swaps in)
//   world_census <DATA> -dump <dir>          also write each object kind's and wall texture's first picture
//                                            as <dir>/obj_<PACK>_<index>.ppm and <dir>/tex_<PACK>_<index>.ppm
//   tools/world_census/sheet.py              contact sheets from a census and a dump (to /tmp only)
//
// The checked-in output is tools/world_census/census_retail.txt (no pictures). docs/WORLD_DESIGN.md is built on it.
//
// Build:  sh tools/world_census/build.sh        (writes /tmp/world_census)
//
// WHAT AN OBJECT KIND IS. The art an object wears: its textureIndex and textureSource resolved to one entry
// of one pack (Pack::ResolveObjectArt: sources 0/1 are the map's own pack, 2/3 the shared ADEMO). Keyed as
// PACK[index]. The same index in two map packs is two different pictures, so the pack is part of the key.
//
// WHAT IS COUNTED PER KIND, and where each rule comes from (ROTH.C, roth_c/src/engine):
//   placed      objects wearing it, in how many maps
//   hidden      flags & 0x80: not drawn at load (propcheck "not-drawn"; a script may show it)
//   fixed       renderType & 0x80: a fixed-angle quad rather than a billboard
//   keyed       renderType & 0x01: activate_targeted_object takes the keyed path (input.c:1038-1052)
//   click       a record in the object CLICK directory (category 1, header +0x08) whose key is the
//               object's id, base 8 (run the chain; the tail flips word[obj+4] bit 0, raw_commands.c:1301)
//   switch      the same directory, base 2: a LIGHT SWITCH (cmd_light_switch, raw_commands.c:4331; the
//               object's art word is XORed with 1 on success, raw_commands.c:2954-2959)
//   exam        a record in the EXAMINE-object directory (header +0x38, opcode 0x30) keyed to its id
//   hit         ... the ATTACK-HITS-object directory (header +0x24, opcode 0x1b)
//   walk        ... the WALK-INTO-object directory (header +0x40, opcode 0x39)
//   item        the art word is an inventory record's render id (+2), shown as the FIRST PICKABLE record with
//               it, which is the one give_item_by_dbase_id resolves (inventory.c:340-368: it skips flags 0x80
//               and category 2). "nopick": a record has the render id but none is pickable.
//   pickup      placements a plain use PICKS UP: activate_targeted_object (input.c:1033-1085) reaches
//               give_item_by_dbase_id only when renderType has none of 0x01 (keyed), 0x02, 0x10, the object
//               has no live click record (run_leftclick_object_trigger returns 0: id 0, or no record keyed to
//               it, or only disabled ones), and give_item_by_dbase_id accepts the art word: it refuses any
//               word below 0x200 (inventory.c:351), i.e. every map-pack picture -- only ADEMO art (sources
//               2 and 3) can be picked up without a script.
//   give        placements whose click chain gives an item instead (0x29), i.e. picked up BY SCRIPT.
//
// WALL TRIGGERS. The 0x18 (use), 0x32 (examine) and 0x1a (attack hits) records, by the face they resolve to
// (the two-hop key rule and the door-sister move, roth_raw.cpp), grouped by the face's mid picture (upper or
// lower when it has no mid), with what their chains do.
//
// CHAINS are summarised by walking NEXT from the trigger's start (and into 0x40 sub-chains), up to 96
// records, and listing the effect families met. A 0x12 delay is walked through (it resumes NEXT). Branches
// taken at run time (0x27/0x28 fail, 0x38 else-index) are NOT followed separately: the summary is what the
// chain CAN do, not what one run does. Good enough to classify; not a substitute for the interpreter.

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"
#include "roth_dbase100.h"
#include "roth_log.h"

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

//---------------------------------------------------------------------------
// Packs
//---------------------------------------------------------------------------
struct LoadedPack { std::string name; std::vector<uint8_t> bytes; Pack pack; bool ok = false; };
static std::map<std::string, LoadedPack *> gPacks;
static LoadedPack *GetPack(const std::string &name)
{
	auto it = gPacks.find(name);
	if (it != gPacks.end()) return it->second;
	auto *lp = new LoadedPack; lp->name = name;
	lp->bytes = ReadFile(TheInstall().PackFile(name.c_str()));
	lp->ok = !lp->bytes.empty() && lp->pack.Load(lp->bytes.data(), lp->bytes.size());
	gPacks[name] = lp; return lp;
}

//---------------------------------------------------------------------------
// DBASE400 text (an item's name): header of 8 bytes at the offset, length at +4 (dialogue.cpp:284-293).
//---------------------------------------------------------------------------
static std::vector<uint8_t> gText;
static std::string TextAt(uint32_t off)
{
	if (off == 0 || (size_t)off + 8 > gText.size()) return "";
	uint32_t len = gText[off + 4] | (gText[off + 5] << 8);
	if (len > 60) len = 60;
	std::string s;
	for (uint32_t i = 0; i < len && off + 8 + i < gText.size(); i++)
	{
		const char c = (char)gText[off + 8 + i];
		if (c == 0) break;
		s += (c >= 32 && c < 127) ? c : ' ';
	}
	return s;
}

//---------------------------------------------------------------------------
// The command header's 15 category directories (GAME_core.md 5.2): {offset, count} at hdr+0x08+4i, each a
// list of u16 record offsets relative to the header (run_leftclick_object_trigger, raw_commands.c:2925-2933).
//---------------------------------------------------------------------------
enum Category { CAT_CLICK_OBJ = 0, CAT_ENTER_SECTOR = 3, CAT_USE_WALL = 4, CAT_USE_FLOOR = 5, CAT_HIT_WALL = 6,
	CAT_HIT_OBJ = 7, CAT_EXAM_WALL = 10, CAT_EXAM_FLOOR = 11, CAT_EXAM_OBJ = 12, CAT_WALK_OBJ = 14 };

struct MapData
{
	std::string name, pack;
	Map m;
	std::vector<std::vector<int>> cat;        // category -> 0-based command indices
	std::vector<int> catOf;                   // command -> category, or -1
};

static bool LoadMap(const std::string &name, MapData &md)
{
	std::vector<uint8_t> raw = ReadFile(TheInstall().MapFile(name.c_str()));
	if (raw.empty()) return false;
	md.name = name;
	md.pack = TheInstall().PackFor(name.c_str());
	md.m = ParseRaw(raw.data(), raw.size());
	if (!md.m.ok()) return false;
	// Recompute every record's offset from the header the same way ParseRaw walks them.
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

// give_item_by_dbase_id's walk (inventory.c:345-368): the first record with this render id that is not flagged
// unpickable (+4 & 0x80) and not category 2. 0 when none, or when the word is below 0x200.
static int FirstPickable(const Dbase100 &db, uint16_t rid)
{
	if (rid < 0x200) return 0;
	for (int id = 1; id <= db.ItemCount(); id++)
	{
		const DbItem *it = db.Item(id);
		if (!it || !it->ok || it->renderId != rid) continue;
		if (it->flags & 0x80) continue;
		if (it->Category() == 2) continue;
		return id;
	}
	return 0;
}

//---------------------------------------------------------------------------
// Chain summary
//---------------------------------------------------------------------------
struct ChainInfo
{
	std::set<std::string> tags;
	std::set<int> itemsTested, itemsGiven, itemsTaken, dialogue, flagsTested, flagsSet;
	std::vector<int> doorRecords;             // 0-based indices of 0x2f records met
	bool lights = false;
	std::string Tags() const
	{
		std::string s;
		for (const std::string &t : tags) { if (!s.empty()) s += ","; s += t; }
		return s.empty() ? "-" : s;
	}
};

static const char *Family(uint8_t op)
{
	switch (op)
	{
	case 0x2f: return "DOOR";
	case 0x02: case 0x1d: case 0x11: return "LIGHT";
	case 0x03: case 0x07: case 0x09: return "MOVE";
	case 0x23: case 0x24: return "OBJMOVE";
	case 0x0a: case 0x0b: case 0x0c: case 0x0e: case 0x0f: case 0x14: case 0x1c: case 0x1f: case 0x21:
	case 0x2e: case 0x34: return "WALLTEX";
	case 0x0d: case 0x20: case 0x3a: return "OBJTEX";
	case 0x16: case 0x3c: return "SPAWN";
	case 0x27: return "HASITEM";
	case 0x29: return "GIVE";
	case 0x2a: return "TAKE";
	case 0x2b: case 0x36: return "TALK";
	case 0x26: return "SETFLAG";
	case 0x28: return "IFFLAG";
	case 0x3b: return "MAP";
	case 0x33: case 0x35: return "HURT";
	case 0x10: return "SFX";
	case 0x2d: return "PARTICLE";
	case 0x3f: case 0x41: return "PLAYER";
	case 0x42: return "INVFILTER";
	default: return nullptr;
	}
}

static void WalkChain(const Map &m, int start1, ChainInfo &ci, std::set<int> &seen, int depth = 0)
{
	int idx = start1;
	for (int steps = 0; idx > 0 && idx <= (int)m.commands.size() && steps < 96; steps++)
	{
		if (!seen.insert(idx).second) break;
		const Command &c = m.commands[idx - 1];
		if (const char *f = Family(c.opcode)) ci.tags.insert(f);
		switch (c.opcode)
		{
		case 0x27: ci.itemsTested.insert(c.key); break;
		case 0x29: ci.itemsGiven.insert(c.key); break;
		case 0x2a: ci.itemsTaken.insert(c.key); break;
		case 0x2b: case 0x36: ci.dialogue.insert(c.key); break;
		case 0x26: ci.flagsSet.insert(c.key); break;
		case 0x28: ci.flagsTested.insert(c.key); break;
		case 0x2f: ci.doorRecords.push_back(idx - 1); break;
		case 0x40: if (depth < 4) WalkChain(m, c.Word(6), ci, seen, depth + 1); break;
		default: break;
		}
		if (c.isTrigger) break;
		idx = c.nextIndex;
	}
}

static ChainInfo ChainOf(const Map &m, int cmd)
{
	ChainInfo ci; std::set<int> seen;
	const Command &c = m.commands[cmd];
	if (c.opcode == 0x02) { ci.tags.insert("LIGHT"); ci.lights = true; return ci; }   // the switch itself
	WalkChain(m, c.chainStart, ci, seen);
	return ci;
}

static std::string SetStr(const std::set<int> &s, const char *pfx)
{
	std::string o;
	for (int v : s) { char b[16]; snprintf(b, sizeof b, "%s%d", o.empty() ? pfx : " ", v); o += b; }
	return o;
}

//---------------------------------------------------------------------------
// Pictures
//---------------------------------------------------------------------------
static bool WritePPM(const std::string &path, const Image &img, const std::vector<Colour> &pal, bool rotated)
{
	if (!img.ok()) return false;
	// Stored column-major: frames[0][col * img.width + row], so the picture is img.height wide.
	const int W = rotated ? img.height : img.width, H = rotated ? img.width : img.height;
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	fprintf(f, "P6 %d %d 255\n", W, H);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			const uint8_t ci = rotated ? img.frames[0][(size_t)x * img.width + y] : img.frames[0][(size_t)y * img.width + x];
			Colour c = { 255, 0, 255 };
			if (!(ci == 0 && img.ZeroIsTransparent()) && ci < pal.size()) c = pal[ci];
			fputc(c.r, f); fputc(c.g, f); fputc(c.b, f);
		}
	fclose(f);
	return true;
}

//---------------------------------------------------------------------------
// Doors (the rules are tools/doorcheck's, restated: doors.c:491-546)
//---------------------------------------------------------------------------
static const TextureMap *TmOf(const Map &m, int face)
{
	if (face < 0 || face >= (int)m.faces.size()) return nullptr;
	const int t = m.faces[face].textureMap;
	return (t >= 0 && t < (int)m.textureMaps.size()) ? &m.textureMaps[t] : nullptr;
}

struct DoorInfo
{
	int sector = -1; uint16_t id = 0, marker = 0;
	int c[4] = { -1, -1, -1, -1 };
	double width = 0, thick = 0; int height = 0, floorZ = 0;
	int secA = -1, secB = -1;
	uint16_t texA = 0, texB = 0;         // the closed door's pictures seen from the c1 and c3 rooms
	// Per room side (A = across c1, B = across c3): the room face's mapping flips the picture (FLIPX), and which of
	// the room face's ends lies at the door's LATCH edge (c2) rather than its hinge (c0): 1 = vertex1, 2 = vertex2.
	bool flipA = false, flipB = false; int latchEndA = 0, latchEndB = 0;
	std::vector<int> triggers;           // 0x18 (and 0x32) records on the door's room faces or its own
	ChainInfo chain;                     // union over the 0x18 use triggers
	std::vector<int> examines;
	std::vector<int> remoteOpeners;      // 0x2f records with a key resolving to this door, met in no 0x18 here
	int liveOpen = 0, liveKeyed = 0, sleepingOpen = 0, speechOnly = 0;
	// KEY: a use on it tests an item in hand before opening; FREE: a live use record opens it; LATER: only a disabled
	// opener (enabled by some event, 0x17) or a 0x2f elsewhere opens it; NEVER: nothing found that opens it.
	const char *Class() const
	{
		if (liveKeyed) return "KEY";
		if (liveOpen) return "FREE";
		if (sleepingOpen || !remoteOpeners.empty()) return "LATER";
		return "NEVER";
	}
};

static bool ResolveDoor(const Map &m, int si, DoorInfo &d)
{
	const Sector &rs = m.sectors[si];
	if (!rs.IsDoorCapable() || rs.faceCount != 4 || rs.firstFaceIndex < 0) return false;
	const int f0 = rs.firstFaceIndex;
	static const int probe[4] = { 0, 3, 2, 1 };
	int hinge = -1;
	for (int k = 0; k < 4 && hinge < 0; k++)
	{
		const TextureMap *tm = TmOf(m, f0 + probe[k]);
		if (tm && tm->extended && tm->faceID >= TRIGGER_DOOR_A) hinge = probe[k];
	}
	if (hinge < 0) return false;
	d.sector = si; d.id = rs.commandID;
	for (int k = 0; k < 4; k++) d.c[k] = f0 + ((hinge + k) & 3);
	d.marker = TmOf(m, d.c[0])->faceID;
	auto len = [&](int fi) { const Face &f = m.faces[fi];
		return hypot(double(m.vertices[f.vertex2].x) - m.vertices[f.vertex1].x, double(m.vertices[f.vertex2].y) - m.vertices[f.vertex1].y); };
	d.thick = len(d.c[0]); d.width = len(d.c[1]);
	d.floorZ = rs.floorHeight; d.height = rs.ceilingHeight - rs.floorHeight;
	if (const TextureMap *t1 = TmOf(m, d.c[1]))
		if ((t1->flags & 1) && (t1->flags & 8)) d.height = -(int(rs.textureMapOverride) << 2);
	const Face &c1 = m.faces[d.c[1]], &c3 = m.faces[d.c[3]];
	d.secA = c1.sister >= 0 ? m.faces[c1.sister].sector : -1;
	d.secB = c3.sister >= 0 ? m.faces[c3.sister].sector : -1;
	if (c1.sister >= 0) if (const TextureMap *t = TmOf(m, c1.sister)) { d.texA = t->midTexture; d.flipA = (t->flags & FF_FLIP_X) != 0; }
	if (c3.sister >= 0) if (const TextureMap *t = TmOf(m, c3.sister)) { d.texB = t->midTexture; d.flipB = (t->flags & FF_FLIP_X) != 0; }
	// The latch edge is c2; its midpoint against each end of the room face.
	{
		const Face &l = m.faces[d.c[2]];
		const double lx = (m.vertices[l.vertex1].x + m.vertices[l.vertex2].x) * 0.5, ly = (m.vertices[l.vertex1].y + m.vertices[l.vertex2].y) * 0.5;
		auto end = [&](int face) -> int
		{
			if (face < 0) return 0;
			const Face &f = m.faces[face];
			const double d1 = hypot(m.vertices[f.vertex1].x - lx, m.vertices[f.vertex1].y - ly);
			const double d2 = hypot(m.vertices[f.vertex2].x - lx, m.vertices[f.vertex2].y - ly);
			return d1 < d2 ? 1 : 2;
		};
		d.latchEndA = end(c1.sister); d.latchEndB = end(c3.sister);
	}
	return true;
}

//---------------------------------------------------------------------------
// Tallies
//---------------------------------------------------------------------------
struct Kind
{
	std::string key, pack; int index = 0;
	std::string what;           // mesh / picture / anim / pack / directional / creature / empty
	int placed = 0, hidden = 0, fixed = 0, keyed = 0;
	int click = 0, sw = 0, exam = 0, hit = 0, walk = 0;
	int itemId = 0; std::string itemName; bool pickable = false;
	int pickup = 0;             // placements a plain use would PICK UP (no click record; the input.c:1053-1085 path)
	int giveClick = 0;          // placements whose click chain gives an item (0x29)
	std::set<std::string> maps;
	std::map<std::string, int> perMap;
	std::set<std::string> chainTags;
	std::set<int> itemsTested;
	int w = 0, h = 0;           // picture size in WORLD UNITS as drawn (w across): texels x units-per-texel
	bool hang = false;          // IM_HANG: anchored by its top (renderer.c:6549-6561)
	int minZ = 1 << 30, maxZ = -(1 << 30);      // the object's z above its own sector's floor
	std::vector<int> lightBytes;
};

struct FaceKind
{
	std::string key;            // PACK[index] of the picture, or "colour"/"none"
	std::string piece;          // mid/upper/lower
	int use = 0, exam = 0, hit = 0;
	std::set<std::string> maps;
	std::map<std::string, int> tags;
	std::set<int> itemsTested;
	int w = 0, h = 0;
};

static std::string KeyOf(const std::string &pack, int index)
{
	char b[64]; snprintf(b, sizeof b, "%s[%d]", pack.c_str(), index); return b;
}

static std::string TexKey(const MapData &md, uint16_t tex, int *w, int *h)
{
	*w = *h = 0;
	if (tex == 0) return "none";
	if (tex >= 0x8000) return "colour";
	int idx = 0;
	const bool sh = Pack::ResolveDasId(tex, idx);
	const std::string pn = sh ? TheInstall().SharedPack() : md.pack;
	LoadedPack *lp = GetPack(pn);
	if (lp->ok)
	{
		Image im = lp->pack.ReadImage(idx, false);
		if (im.ok()) { *w = im.height; *h = im.width; }
	}
	return KeyOf(pn, idx);
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: world_census <ROTH DATA folder> [-objects|-faces|-doors|-switches|-kind PACK[i]|-dump DIR]\n"); return 2; }
	if (!TheInstall().Open(argv[1])) { printf("install: %s\n", TheInstall().Error().c_str()); return 2; }
	std::string mode = argc > 2 ? argv[2] : "", arg = argc > 3 ? argv[3] : "";
	const bool all = mode.empty() || mode == "-dump";
	Dbase100 db; Log log;
	if (!LoadDbase100FromInstall(TheInstall(), db, &log)) { printf("DBASE100 not loaded\n"); return 2; }
	gText = ReadFile(std::string(argv[1]) + "/DBASE400.DAT");

	if (mode == "-pic" && argc > 5)
	{
		// world_census <DATA> -pic PACK INDEX out.ppm : one entry's first picture (for art no object is placed
		// with, such as the lit or thrown state a script swaps in). Writes only where told to.
		LoadedPack *lp = GetPack(arg);
		if (!lp->ok) return 2;
		const std::vector<Colour> &pp = lp->pack.Palette().empty() ? GetPack("DEMO")->pack.Palette() : lp->pack.Palette();
		Image im = lp->pack.ReadImage(atoi(argv[4]), false);
		printf("%s[%s]: %dx%d %s\n", arg.c_str(), argv[4], im.height, im.width, im.ok() ? "" : "(no picture)");
		return WritePPM(argv[5], im, pp, true) ? 0 : 1;
	}

	if (mode == "-chain" && argc > 4)
	{
		// world_census <DATA> -chain MAP N : record N (1-based) and, for a trigger, the records its chain walks.
		MapData md;
		if (!LoadMap(arg, md)) { printf("map %s: not read\n", arg.c_str()); return 2; }
		const Map &m = md.m;
		int idx = atoi(argv[4]);
		auto dump = [&](int i)
		{
			const Command &c = m.commands[i - 1];
			std::string a;
			for (size_t k = 0; k < c.args.size(); k++) { char b[12]; snprintf(b, sizeof b, " %04x", c.args[k]); a += b; }
			printf("  #%-4d op %02x mod %02x %s key %5d  next/start %4d  cat %2d  args(+06..):%s\n", i, c.opcode, c.modifier,
				c.isTrigger ? "TRIG" : "    ", c.key, c.linkIndex, md.catOf[i - 1], a.c_str());
		};
		if (idx < 1 || idx > (int)m.commands.size()) return 2;
		dump(idx);
		const Command &t = m.commands[idx - 1];
		std::set<int> seen;
		int i = t.isTrigger ? t.chainStart : t.nextIndex;
		for (int steps = 0; i > 0 && i <= (int)m.commands.size() && steps < 96 && seen.insert(i).second; steps++)
		{
			dump(i);
			if (m.commands[i - 1].isTrigger) break;
			i = m.commands[i - 1].nextIndex;
		}
		return 0;
	}

	std::vector<MapData> maps;
	for (const std::string &name : TheInstall().MapNames())
	{
		MapData md;
		if (LoadMap(name, md)) maps.push_back(std::move(md));
		else printf("map %s: not read\n", name.c_str());
	}

	std::map<std::string, Kind> kinds;
	std::map<std::string, FaceKind> faceKinds;
	std::vector<std::pair<std::string, DoorInfo>> doors;
	int nObjects = 0;
	// Where the things a hand takes lie: z above the sector floor, for placements picked up plainly or by a
	// giving click chain. Buckets in world units.
	static const int zEdge[] = { 1, 40, 100, 160, 240, 1 << 30 };
	static const char *zName[] = { "on the floor (0)", "1..39", "40..99", "100..159", "160..239", "240 and up" };
	int zHist[6] = {};
	struct Switch { std::string map; int sec, idx, cmd; std::string kind; int group; int step; };
	std::vector<Switch> switches;
	std::vector<std::string> kindLines;
	int catCheck[15][256] = {};

	for (const MapData &md : maps)
	{
		const Map &m = md.m;
		for (int c = 0; c < 15; c++) for (int ci : md.cat[c]) catCheck[c][m.commands[ci].opcode]++;
		// id -> records per category
		std::map<int, std::vector<int>> byKey[15];
		for (int c : { CAT_CLICK_OBJ, CAT_EXAM_OBJ, CAT_HIT_OBJ, CAT_WALK_OBJ })
			for (int ci : md.cat[c]) byKey[c][m.commands[ci].key].push_back(ci);   // disabled ones too: they count as scripts

		for (size_t si = 0; si < m.objects.size(); si++)
			for (size_t oi = 0; oi < m.objects[si].size(); oi++)
			{
				const Object &o = m.objects[si][oi];
				nObjects++;
				int idx = 0;
				const bool sh = Pack::ResolveObjectArt(o.textureIndex, o.textureSource, idx);
				const std::string pn = sh ? TheInstall().SharedPack() : md.pack;
				const std::string key = KeyOf(pn, idx);
				Kind &k = kinds[key];
				if (k.placed == 0)
				{
					k.key = key; k.pack = pn; k.index = idx;
					LoadedPack *lp = GetPack(pn);
					const FatEntry *e = lp->ok ? lp->pack.Entry(idx) : nullptr;
					if (!e) k.what = "missing";
					else switch (e->kind)
					{
					case EntryKind::Object3D: k.what = "mesh"; break;
					case EntryKind::Plain: k.what = "picture"; break;
					case EntryKind::Animated: k.what = "anim"; break;
					case EntryKind::ImagePack: k.what = "pack"; break;
					case EntryKind::Indirection:
						k.what = lp->pack.Indirect(idx) == IndirectKind::Creature ? "creature" : "directional"; break;
					default: k.what = "empty"; break;
					}
					if (e && (k.what == "picture" || k.what == "anim" || k.what == "pack"))
					{
						Image im = lp->pack.ReadImage(idx, false);
						if (im.ok())
						{
							// units per texel: TextureSet::Sprite's rule, renderer.c:6512-6525 (as propcheck SpriteSize)
							int upp2 = 4;                      // in half units: 2.0
							if (im.modifier & IM_HALF_SIZE) { const int sh2 = (im.imageType >> 5) & 3; upp2 = sh2 == 0 ? 2 : 4 * (1 << sh2); }
							k.w = im.height * upp2 / 2; k.h = im.width * upp2 / 2;
							k.hang = (im.modifier & IM_HANG) != 0;
						}
					}
					const uint16_t rid = (uint16_t)(o.textureIndex | (o.textureSource << 8));
					k.itemId = FirstPickable(db, rid);
					k.pickable = k.itemId > 0;
					if (k.itemId == 0) k.itemId = db.FindItemByRenderId(rid);
					if (k.itemId > 0)
						if (const DbItem *it = db.Item(k.itemId)) k.itemName = TextAt(it->name);
				}
				k.placed++; k.maps.insert(md.name); k.perMap[md.name]++;
				if (o.flags & 0x80) k.hidden++;
				if (o.renderType & 0x80) k.fixed++;
				if (o.renderType & 0x01) k.keyed++;
				const int zrel = (int)o.z - (int)m.sectors[si].floorHeight;
				k.minZ = std::min(k.minZ, zrel); k.maxZ = std::max(k.maxZ, zrel);
				std::string scripts;
				if (o.commandID != 0)
				{
					auto add = [&](int cat, int &counter, const char *tag)
					{
						auto it = byKey[cat].find(o.commandID);
						if (it == byKey[cat].end()) return;
						for (int ci : it->second)
						{
							const Command &c = m.commands[ci];
							if (cat == CAT_CLICK_OBJ && c.opcode == 0x02)
							{
								k.sw++;
								switches.push_back({ md.name, (int)si, (int)oi, ci, key, c.Word(0x0a), (int)(int8_t)c.Byte(7) });
								scripts += " SWITCH#" + std::to_string(ci + 1);
								k.chainTags.insert("LIGHT");
								continue;
							}
							counter++;
							ChainInfo ch = ChainOf(m, ci);
							for (const std::string &t : ch.tags) k.chainTags.insert(t);
							for (int v : ch.itemsTested) k.itemsTested.insert(v);
							scripts += std::string(" ") + tag + "#" + std::to_string(ci + 1) + "(" + ch.Tags() + SetStr(ch.itemsTested, " i") + SetStr(ch.dialogue, " d") + ")";
						}
					};
					add(CAT_CLICK_OBJ, k.click, "CLICK");
					add(CAT_EXAM_OBJ, k.exam, "EXAM");
					add(CAT_HIT_OBJ, k.hit, "HIT");
					add(CAT_WALK_OBJ, k.walk, "WALK");
				}
				{
					bool liveClick = false, gives = false;
					if (o.commandID != 0)
					{
						auto it = byKey[CAT_CLICK_OBJ].find(o.commandID);
						if (it != byKey[CAT_CLICK_OBJ].end())
							for (int ci : it->second)
								if (!(m.commands[ci].modifier & 8))
								{
									liveClick = true;
									if (ChainOf(m, ci).tags.count("GIVE")) gives = true;
								}
					}
					const uint16_t rid = (uint16_t)(o.textureIndex | (o.textureSource << 8));
					const bool pick = !(o.renderType & 0x13) && !liveClick && rid >= 0x200 && FirstPickable(db, rid) > 0;
					if (pick) { k.pickup++; scripts += " PICKUP"; }
					if (gives) k.giveClick++;
					if (pick || gives)
					{
						const int zr = (int)o.z - (int)m.sectors[si].floorHeight;
						int b = 0; while (b < 5 && zr >= zEdge[b]) b++;
						zHist[b]++;
					}
				}
				if (mode == "-kind" && key == arg)
				{
					char b[512];
					snprintf(b, sizeof b, "%-9s %4zu.%-3zu x %6d y %6d z %5d (floor %+5d) rot %3d fl %02x rt %02x light %3d id %5d%s%s",
						md.name.c_str(), si, oi, o.x, o.y, o.z, (int)o.z - (int)m.sectors[si].floorHeight, o.rotation, o.flags, o.renderType, o.light, o.commandID,
						(o.flags & 0x80) ? " HIDDEN" : "", scripts.c_str());
					kindLines.push_back(b);
				}
			}

		// Wall triggers
		auto faceTally = [&](int cat, int FaceKind::*counter)
		{
			for (int ci : md.cat[cat])
			{
				const Command &c = m.commands[ci];
				ChainInfo ch = ChainOf(m, ci);
				std::set<std::string> done;
				for (int fi : c.faces)
				{
					const TextureMap *tm = TmOf(m, fi);
					if (!tm) continue;
					const char *piece = "mid"; uint16_t tex = tm->midTexture;
					if (tex == 0) { piece = "upper"; tex = tm->upperTexture; }
					if (tex == 0) { piece = "lower"; tex = tm->lowerTexture; }
					int w, h;
					const std::string tk = TexKey(md, tex, &w, &h);
					if (!done.insert(tk).second) continue;     // one count per record per picture
					FaceKind &fk = faceKinds[tk];
					fk.key = tk; fk.piece = piece; fk.w = w; fk.h = h;
					fk.*counter += 1; fk.maps.insert(md.name);
					for (const std::string &t : ch.tags) fk.tags[t]++;
					for (int v : ch.itemsTested) fk.itemsTested.insert(v);
				}
			}
		};
		faceTally(CAT_USE_WALL, &FaceKind::use);
		faceTally(CAT_EXAM_WALL, &FaceKind::exam);
		faceTally(CAT_HIT_WALL, &FaceKind::hit);

		// Doors
		for (size_t si = 0; si < m.sectors.size(); si++)
		{
			DoorInfo d;
			if (!ResolveDoor(m, (int)si, d)) continue;
			std::set<int> doorFaces;
			for (int k = 0; k < 4; k++)
			{
				doorFaces.insert(d.c[k]);
				if (m.faces[d.c[k]].sister >= 0) doorFaces.insert(m.faces[d.c[k]].sister);
			}
			for (int cat : { CAT_USE_WALL, CAT_EXAM_WALL })
				for (int ci : md.cat[cat])
				{
					const Command &c = m.commands[ci];
					bool on = false;
					for (int fi : c.faces) if (doorFaces.count(fi)) on = true;
					if (!on) continue;
					if (cat == CAT_EXAM_WALL) { d.examines.push_back(ci); continue; }
					d.triggers.push_back(ci);
					ChainInfo ch = ChainOf(m, ci);
					const bool opens = ch.tags.count("DOOR") > 0;
					if (!opens) d.speechOnly++;
					else if (c.disabled) d.sleepingOpen++;
					else if (!ch.itemsTested.empty()) d.liveKeyed++;
					else d.liveOpen++;
					for (const std::string &t : ch.tags) d.chain.tags.insert(t);
					for (int v : ch.itemsTested) d.chain.itemsTested.insert(v);
					for (int v : ch.dialogue) d.chain.dialogue.insert(v);
					for (int v : ch.flagsTested) d.chain.flagsTested.insert(v);
				}
			for (size_t ci = 0; ci < m.commands.size(); ci++)
			{
				const Command &c = m.commands[ci];
				if (c.opcode != 0x2f || c.key == 0) continue;
				bool on = false;
				for (int fi : c.faces) if (doorFaces.count(fi)) on = true;
				if (on) d.remoteOpeners.push_back((int)ci);
			}
			doors.push_back({ md.name, d });
		}
	}

	//------------------------------------------------------------------
	// Output
	//------------------------------------------------------------------
	if (mode == "-kind")
	{
		printf("%s: %zu placements\n", arg.c_str(), kindLines.size());
		for (const std::string &l : kindLines) printf("  %s\n", l.c_str());
		return 0;
	}

	if (all)
	{
		printf("CATEGORY CHECK (directory -> opcodes of the records it lists, all maps):\n");
		static const char *catName[15] = { "click obj +08", "unused +0c", "+10", "enter sector +14", "use wall +18",
			"use floor +1c", "hit wall +20", "hit obj +24", "texanim +28", "unused +2c", "exam wall +30",
			"exam floor +34", "exam obj +38", "texremap +3c", "walk obj +40" };
		for (int c = 0; c < 15; c++)
		{
			std::string s;
			for (int op = 0; op < 256; op++) if (catCheck[c][op]) { char b[32]; snprintf(b, sizeof b, " 0x%02x:%d", op, catCheck[c][op]); s += b; }
			if (!s.empty()) printf("  %-17s%s\n", catName[c], s.c_str());
		}
		printf("\n");
	}

	if (all || mode == "-objects")
	{
		std::vector<const Kind *> v;
		for (auto &kv : kinds) v.push_back(&kv.second);
		std::sort(v.begin(), v.end(), [](const Kind *a, const Kind *b) { return a->placed != b->placed ? a->placed > b->placed : a->key < b->key; });
		printf("OBJECT KINDS: %zu kinds, %d objects, %zu maps\n", v.size(), nObjects, maps.size());
		printf("WxH = drawn size in world units (texels x units per texel; h = hangs from its anchor); z = above its own sector's floor\n");
		printf("%-14s %-11s %7s %4s %4s %4s %4s | %4s %4s %4s %4s %4s | %4s %4s | %-9s | %-12s | %s\n", "art", "what", "WxH", "plcd", "maps",
			"hide", "fix", "clik", "swch", "exam", "hit", "walk", "pick", "give", "z", "chain", "item / maps");
		for (const Kind *k : v)
		{
			char wh[16]; snprintf(wh, sizeof wh, "%dx%d%s", k->w, k->h, k->hang ? "h" : "");
			char zr[24]; snprintf(zr, sizeof zr, "%d..%d", k->minZ, k->maxZ);
			std::string tags;
			for (const std::string &t : k->chainTags) { if (!tags.empty()) tags += ","; tags += t; }
			std::string item;
			if (k->itemId > 0) item = "ITEM " + std::to_string(k->itemId) + (k->pickable ? " " : " nopick ") + "\"" + k->itemName + "\" ";
			std::string ml;
			int shown = 0;
			for (auto &pm : k->perMap) { if (shown++ < 6) ml += " " + pm.first + ":" + std::to_string(pm.second); }
			if ((int)k->perMap.size() > 6) ml += " +" + std::to_string(k->perMap.size() - 6);
			printf("%-14s %-11s %7s %4d %4zu %4d %4d | %4d %4d %4d %4d %4d | %4d %4d | %-9s | %-12s | %s%s%s\n", k->key.c_str(), k->what.c_str(), wh,
				k->placed, k->maps.size(), k->hidden, k->fixed, k->click, k->sw, k->exam, k->hit, k->walk, k->pickup, k->giveClick, zr,
				tags.empty() ? "-" : tags.c_str(), item.c_str(), SetStr(k->itemsTested, " tests i").c_str(), ml.c_str());
		}
		int tp = 0, tg = 0, tc = 0, ts = 0, te = 0, th = 0, tw = 0;
		for (const Kind *k : v) { tp += k->pickup; tg += k->giveClick; tc += k->click; ts += k->sw; te += k->exam; th += k->hit; tw += k->walk; }
		printf("TOTALS over placements: click-chain %d (of them giving an item %d), light switch %d, examine %d, attack-hit %d, walk-into %d, plain pickup %d\n",
			tc, tg, ts, te, th, tw, tp);
		printf("  where those %d taken things lie (z above the floor):", tg + tp);
		for (int b = 0; b < 6; b++) printf("  %s %d", zName[b], zHist[b]);
		printf("\n");
		printf("\n");
	}

	if (all || mode == "-faces")
	{
		std::vector<const FaceKind *> v;
		for (auto &kv : faceKinds) v.push_back(&kv.second);
		std::sort(v.begin(), v.end(), [](const FaceKind *a, const FaceKind *b) {
			const int ta = a->use + a->exam + a->hit, tb = b->use + b->exam + b->hit; return ta != tb ? ta > tb : a->key < b->key; });
		int tu = 0, te = 0, th = 0;
		for (const FaceKind *f : v) { tu += f->use; te += f->exam; th += f->hit; }
		printf("WALL TRIGGERS BY PICTURE: %zu pictures; record-picture pairs: use 0x18 %d, examine 0x32 %d, attack-hits 0x1a %d\n",
			v.size(), tu, te, th);
		printf("%-14s %-5s %7s | %4s %4s %4s | %s\n", "picture", "piece", "WxH", "use", "exam", "hit", "chain families (records) / items tested / maps");
		for (const FaceKind *f : v)
		{
			char wh[16]; snprintf(wh, sizeof wh, "%dx%d", f->w, f->h);
			std::string tags;
			for (auto &t : f->tags) { if (!tags.empty()) tags += ","; tags += t.first + ":" + std::to_string(t.second); }
			std::string ml; int shown = 0;
			for (const std::string &mn : f->maps) { if (shown++ < 5) ml += " " + mn; }
			if (f->maps.size() > 5) ml += " +" + std::to_string(f->maps.size() - 5);
			printf("%-14s %-5s %7s | %4d %4d %4d | %s%s |%s\n", f->key.c_str(), f->piece.c_str(), wh, f->use, f->exam, f->hit,
				tags.empty() ? "-" : tags.c_str(), SetStr(f->itemsTested, " i").c_str(), ml.c_str());
		}
		printf("\n");
	}

	if (all || mode == "-doors")
	{
		int n = 0, withUse = 0, withDoorOp = 0, keyed = 0, talk = 0, none = 0, remote = 0;
		std::map<std::string, int> classes;
		std::map<int, int> widths, heights;
		std::map<uint16_t, int> markers;
		printf("DOORS (one line each): class map sector id marker | width x thick x height | rooms c1/c3 | pictures c1/c3 (F = flipped, vN = the room face's end at the latch) | use records: chain | examine | 0x2f elsewhere\n");
		for (auto &pd : doors)
		{
			const DoorInfo &d = pd.second;
			n++;
			widths[(int)lround(d.width)]++; heights[d.height]++; markers[d.marker]++;
			if (!d.triggers.empty()) withUse++;
			if (d.chain.tags.count("DOOR")) withDoorOp++;
			if (!d.chain.itemsTested.empty()) keyed++;
			if (d.chain.tags.count("TALK")) talk++;
			if (d.triggers.empty() && d.remoteOpeners.empty()) none++;
			if (!d.remoteOpeners.empty()) remote++;
			classes[d.Class()]++;
			std::string tr;
			for (int ci : d.triggers) tr += " #" + std::to_string(ci + 1);
			std::string ex;
			for (int ci : d.examines) ex += " #" + std::to_string(ci + 1);
			std::string ro;
			for (int ci : d.remoteOpeners) ro += " #" + std::to_string(ci + 1);
			// picture as the room sees it: DAS id, F when the room face flips it, then the room face's end at the latch (v1/v2)
			char pa[24], pb[24];
			snprintf(pa, sizeof pa, "%d%s v%d", d.texA, d.flipA ? "F" : "", d.latchEndA);
			snprintf(pb, sizeof pb, "%d%s v%d", d.texB, d.flipB ? "F" : "", d.latchEndB);
			printf("%-5s %-9s %4d %04x %04x | %3.0f x %2.0f x %3d | %4d/%-4d | %10s / %-10s |%s: %s%s%s%s |%s |%s\n", d.Class(), pd.first.c_str(), d.sector, d.id, d.marker,
				d.width, d.thick, d.height, d.secA, d.secB, pa, pb, tr.empty() ? " none" : tr.c_str(), d.chain.Tags().c_str(),
				SetStr(d.chain.itemsTested, " i").c_str(), SetStr(d.chain.dialogue, " d").c_str(), SetStr(d.chain.flagsTested, " f").c_str(),
				ex.empty() ? " -" : ex.c_str(), ro.empty() ? " -" : ro.c_str());
		}
		printf("DOORS: %d; with a use (0x18) record on a door face %d; whose chain opens a door (0x2f) %d; testing an item in hand %d;"
			" speaking %d; opened only from elsewhere (0x2f keyed to it) %d; neither %d\n", n, withUse, withDoorOp, keyed, talk, remote, none);
		printf("  classes:"); for (auto &c : classes) printf(" %s %d", c.first.c_str(), c.second);
		printf("   (KEY: a use tests an item in hand, then opens; FREE: a live use record opens it; LATER: only a disabled opener or\n"
			"   a 0x2f elsewhere; NEVER: nothing found)\n");
		printf("  widths:"); for (auto &w : widths) printf(" %d x%d", w.first, w.second);
		printf("\n  heights:"); for (auto &h : heights) printf(" %d x%d", h.first, h.second);
		printf("\n  hinge markers:"); for (auto &mk : markers) printf(" %04x x%d", mk.first, mk.second);
		printf("\n\n");
	}

	if (all || mode == "-switches")
	{
		printf("LIGHT SWITCHES (object click records with base 0x02): %zu\n", switches.size());
		for (const Switch &s : switches)
			printf("  %-9s obj %d.%d  record #%d  art %s  light group key %d  step %d\n", s.map.c_str(), s.sec, s.idx, s.cmd + 1,
				s.kind.c_str(), s.group, s.step);
		printf("\n");
	}

	if (mode == "-dump")
	{
		int wrote = 0;
		for (auto &kv : kinds)
		{
			const Kind &k = kv.second;
			LoadedPack *lp = GetPack(k.pack);
			if (!lp->ok) continue;
			const std::vector<Colour> &pal = GetPack("DEMO")->pack.Palette();
			int idx = k.index;
			if (k.what == "directional")
			{
				Directional d = lp->pack.ReadDirectional(idx);
				if (!d.ok()) continue;
				int fi = 0; const bool fs = Pack::ResolveDasId(d.frames[0].entry, fi);
				LoadedPack *fp = fs ? GetPack(TheInstall().SharedPack()) : lp;
				if (WritePPM(arg + "/obj_" + k.pack + "_" + std::to_string(idx) + ".ppm", fp->pack.ReadImage(fi, false), pal, true)) wrote++;
				continue;
			}
			if (k.what != "picture" && k.what != "anim" && k.what != "pack") continue;
			const std::vector<Colour> &pp = lp->pack.Palette().empty() ? pal : lp->pack.Palette();
			if (WritePPM(arg + "/obj_" + k.pack + "_" + std::to_string(idx) + ".ppm", lp->pack.ReadImage(idx, false), pp, true)) wrote++;
			// a light switch's other position is the neighbouring entry (word[obj+4] ^ 1)
			if (k.sw > 0 && WritePPM(arg + "/obj_" + k.pack + "_" + std::to_string(idx ^ 1) + "_alt.ppm", lp->pack.ReadImage(idx ^ 1, false), pp, true)) wrote++;
		}
		for (auto &kv : faceKinds)
		{
			const std::string &key = kv.first;
			const size_t lb = key.find('[');
			if (lb == std::string::npos) continue;
			const std::string pn = key.substr(0, lb);
			const int idx = atoi(key.c_str() + lb + 1);
			LoadedPack *lp = GetPack(pn);
			if (!lp->ok) continue;
			const std::vector<Colour> &pp = lp->pack.Palette().empty() ? GetPack("DEMO")->pack.Palette() : lp->pack.Palette();
			if (WritePPM(arg + "/tex_" + pn + "_" + std::to_string(idx) + ".ppm", lp->pack.ReadImage(idx, false), pp, true)) wrote++;
		}
		printf("dump: %d pictures written to %s\n", wrote, arg.c_str());
	}
	return 0;
}
