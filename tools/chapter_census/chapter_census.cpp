// chapter_census.cpp -- the game's chapter structure, per-map puzzle chains, items and timing, read from the
// player's own install, so docs/CHAPTERS.md is reproducible. Engine-free (g++ and the src/roth readers); writes
// nothing but stdout.
//
//   sh tools/chapter_census/build.sh [out-dir]               builds <out>/chapter_census (default /tmp/chapter_census)
//   chapter_census <DATA> -chapters     the 20 chapter cards: cutscene index, DBASE100 action, where it is run
//   chapter_census <DATA> -warps        every RAW 0x3b map transition with the chain that reaches it and its gates
//   chapter_census <DATA> -map NAME     one map's units (every entry record and the chain it walks), one per line
//   chapter_census <DATA> -maps         every map's units
//   chapter_census <DATA> -digest       per map: items found, movies played, maps it leads to
//   chapter_census <DATA> -gates        every unit with a requirement, and where each requirement is produced
//   chapter_census <DATA> -items        per item: where found, where tested, where removed, combinations
//   chapter_census <DATA> -chitems      items grouped by the chapter whose maps first hold them
//   chapter_census <DATA> -chgates      markdown rows, per chapter, of the units that gate progress
//   chapter_census <DATA> -kinds        puzzle-kind counts per map and per chapter
//   chapter_census <DATA> -timing       every delay, counter, timer, random and one-shot on a gating chain
//   chapter_census <DATA> -all          all of the above (the checked-in tools/chapter_census/chapters_retail.txt)
//   chapter_census <DATA> -path [N]     (not in -all) the dependency slice to chapter card N (0 = the end, act 614)
//   chapter_census <DATA> -chpaths      (not in -all) what each slice adds over the previous card's. Over-approximate:
//                                       every producer of a needed fact is kept, and space is not modelled.
//
// WHAT IS READ, AND THE RULE EACH STEP FOLLOWS (ROTH.C = roth_c/src/engine of the reconstruction):
//
//  ENTRIES. A record is an entry when one of the command header's 15 category directories lists it
//  (hdr+0x08+4i {offset,count}, docs/GAME_core.md 5.2; read as tools/world_census does) or its base is 0x3d
//  (registered at load, raw_commands.c:5381). The directory says what fires it:
//      +08 click object (base 8 runs the chain; base 2 is a light switch itself)   +14 enter/leave sector (0x13)
//      +18 use wall (0x18)   +1c use floor (0x19)   +20 attack hits wall (0x1a)   +24 attack hits object / death (0x1b)
//      +28 texture-anim frame (0x25)   +30 examine wall (0x32)   +34 examine floor (0x31)   +38 examine object (0x30)
//      +3c texture remap (0x37)   +40 walk into object (0x39)   +10 load-time mover (0x03, not a chain)
//  An entry's word at +4 is its chain START; an instruction's is NEXT (roth_raw.h).
//
//  CHAINS are walked from the start along NEXT (rawcmd_exec_loop, raw_commands.c:1229-1316). A 0x12 delay is
//  walked through (it resumes NEXT later, tick_delay_timer :5328). 0x40 walks its sub-chain from word[+6] inline
//  (cmd_run_indexed_object_command :3955). 0x38's key is the ELSE index a failing condition re-runs from
//  (:1280-1287); it is walked as a separate branch. 0x1e names a Count record by index; when that count reaches
//  its target the Count's NEXT is queued (cmd_modify_count :4001), so that continuation is walked as a branch
//  gated by "counter #k completes". A record disabled at load (modifier & 0x08) is skipped by the executor until a
//  0x17 toggles it (cmd_toggle_command :541); the walk keeps it and marks it [off].
//
//  CONDITIONS (the chain stops when one fails; interrupt 2, :4645-4657):
//    0x28 flag test: continues iff (flag set) == (f6 & 1)                                  cmd_if_not_flag :4665
//    0x27 item test: single mode continues iff (player has it) == (f6 & 1); f6&2 = it must be IN HAND (selected),
//         f6&0x20 no auto-select, else carried anywhere; f6&8 without 4 appends to a list, f6&8|4 tests the list;
//         f6&4 alone compares the last item given                                           cmd_if_not_item :4760
//    0x36 latches a DBASE100 action to run when the next condition fails (the refusal line)   :514
//  A condition "requires" a flag or item only in its continue-when-present polarity; the other polarity is the
//  refusal path and requires nothing.
//
//  EXAMINE records (0x30/0x31/0x32) first run the DBASE100 action in their word +0x0a, then their chain
//  (examine_world_object, input.c:949-965).
//  DBASE100 ACTIONS (RAW 0x2b runs action [+8], eval_dialogue_record_by_id dbase100.c:545) are summarised by their
//  command words (execute_dbase100_chain dbase100.c:607-953): 0x01 flag gate (terminates on fail), 0x02 count >
//  n gate, 0x03 count == n gate, 0x04 set/clear flag, 0x05 a line, 0x07 cutscene (operand - 1), 0x08 a choice,
//  0x09 a choice branch, 0x0b random sub-chain, 0x0d flag skips the next word, 0x0e full-screen image, 0x11 give,
//  0x91 remove, 0x1d jump to another action (followed), 0x23 run RAW chain by index, 0x36 run the RAW click chain
//  of an object id, 0x26 health. Conditions inside an action gate the words after them; at each 0x09 the gate
//  set returns to what it was at the first choice.
//
//  ITEMS FOUND: a placed object is a plain pickup when the rules world_census uses hold (activate_targeted_object
//  input.c:1033-1085, give_item_by_dbase_id inventory.c:340-368): renderType has none of 0x01/0x02/0x10, no live
//  click record is keyed to it, and its art word (>= 0x200) is the render id of a pickable record (first by id,
//  not flags 0x80, not category 2). Also RAW 0x29, DBASE100 0x11, OnUse results (0xB4), and action 3 (new game).
//
// UNREACHABLE: a click/examine entry whose object is hidden at load (flags & 0x80) and that no 0x16 or 0x3c in the
// map names is marked "unreachable (hidden object)" and produces nothing in the tables (STUDY1 #606 gives every map).
//
// WHAT THIS DOES NOT MODEL. Space: which rooms a door or a lift closes off. A locked door's key is therefore not
// "needed" by the dependency slice unless a flag or item downstream needs it; the slice lists every KEY door
// separately. Timing: delays are walked through. The dependency slice is an over-approximation (every producer of a
// needed flag or item is kept); a map is entered freely unless every way in is gated.
//
// Quotes: none. Item names are DBASE400's short names; dialogue is named by action id only. Nothing here prints
// retail text beyond those names and the GDV file stems.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "roth_raw.h"
#include "roth_install.h"
#include "roth_dbase100.h"
#include "roth_log.h"
#include "roth_gameflow.h"

using namespace roth;

static std::vector<uint8_t> ReadAll(const std::string &p)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return b;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b.resize(n > 0 ? (size_t)n : 0);
	if (n > 0 && fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
	fclose(f);
	return b;
}
static uint16_t U16(const std::vector<uint8_t> &b, size_t o) { return o + 1 < b.size() ? (uint16_t)(b[o] | (b[o + 1] << 8)) : 0; }

static Dbase100 gDb;
static std::vector<uint8_t> gText;   // DBASE400

// A DBASE400 entry's text, cut short: only item names are printed with it.
static std::string Text(uint32_t off, size_t cut = 32)
{
	if (gText.empty() || off == 0 || (uint64_t)off + 8 > gText.size()) return "";
	const uint8_t *p = gText.data() + off;
	unsigned len = (unsigned)(p[4] | (p[5] << 8));
	std::string s;
	for (unsigned i = 0; i < len && off + 8 + i < gText.size(); i++)
	{
		uint8_t c = p[8 + i];
		if (c == 0) break;
		s += (c == '\n' || c == '\r') ? ' ' : ((c >= 0x20 && c < 0x7f) ? (char)c : '?');
	}
	if (s.size() > cut) s = s.substr(0, cut);
	return s;
}
static std::string ItemName(int id)
{
	const DbItem *it = gDb.Item(id);
	return it ? Text(it->name) : "?";
}
static std::string Cut(int idx0)
{
	const DbCutscene *c = gDb.Cutscene(idx0);
	return c && !c->name.empty() ? c->name : "#" + std::to_string(idx0);
}

//---------------------------------------------------------------------------
// The chapter cards. A chapter is the original's own unit: CHPTR<n>.GDV, the title card, is played by a DBASE100
// action (op 0x07). Found by name in the cutscene table, never assumed.
//---------------------------------------------------------------------------
struct Card { int chapter = 0; int cut = -1; std::set<int> actions; };
static std::vector<Card> gCards;     // [chapter], 1..20; [0] unused; [21] = EPILOGUE
static const int END_ACTION = 614;   // roth_gameflow.h: the last chain (two movies, a picture, health to 0)

//---------------------------------------------------------------------------
// Maps
//---------------------------------------------------------------------------
enum Dir { D_CLICK = 0, D_UNUSED1, D_LOADMOVER, D_ENTER, D_USEWALL, D_USEFLOOR, D_HITWALL, D_HITOBJ, D_TEXANIM, D_UNUSED9,
	D_EXAMWALL, D_EXAMFLOOR, D_EXAMOBJ, D_TEXREMAP, D_WALKOBJ, D_TIMER = 15 };
static const char *DirName(int d, int base)
{
	switch (d)
	{
	case D_CLICK: return base == 2 ? "switch" : "click-obj";
	case D_LOADMOVER: return "load-mover";
	case D_ENTER: return "enter-sector";
	case D_USEWALL: return "use-wall";
	case D_USEFLOOR: return "use-floor";
	case D_HITWALL: return "hit-wall";
	case D_HITOBJ: return "hit-obj";
	case D_TEXANIM: return "tex-anim";
	case D_EXAMWALL: return "exam-wall";
	case D_EXAMFLOOR: return "exam-floor";
	case D_EXAMOBJ: return "exam-obj";
	case D_TEXREMAP: return "tex-remap";
	case D_WALKOBJ: return "walk-obj";
	case D_TIMER: return "timer";
	default: return "?";
	}
}

struct MapData
{
	std::string name;
	Map m;
	std::vector<int> dirOf;                      // 0-based command -> directory, or -1
	std::map<int, std::vector<const Object *>> objById;
	std::map<int, int> sectorOfObj;              // object id -> sector index of its first placement
};
static std::vector<MapData> gMaps;
static std::map<std::string, int> gMapIdx;

static bool LoadMap(const std::string &name, MapData &md)
{
	std::vector<uint8_t> raw = ReadAll(TheInstall().MapFile(name.c_str()));
	if (raw.empty()) return false;
	md.name = name;
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
	md.dirOf.assign(md.m.commands.size(), -1);
	for (int c = 0; c < 15; c++)
	{
		const uint16_t off = U16(raw, base + 8 + 4 * c), n = U16(raw, base + 10 + 4 * c);
		for (int k = 0; k < n; k++)
		{
			auto it = byOff.find(U16(raw, base + off + 2 * k));
			if (it == byOff.end() || it->second >= (int)md.m.commands.size()) continue;
			md.dirOf[it->second] = c;
		}
	}
	for (size_t i = 0; i < md.m.commands.size(); i++)
		if (md.dirOf[i] < 0 && md.m.commands[i].opcode == 0x3d) md.dirOf[i] = D_TIMER;
	for (size_t s = 0; s < md.m.objects.size(); s++)
		for (const Object &o : md.m.objects[s])
		{
			md.objById[o.commandID].push_back(&o);
			if (!md.sectorOfObj.count(o.commandID)) md.sectorOfObj[o.commandID] = (int)s;
		}
	return true;
}

//---------------------------------------------------------------------------
// Facts and tokens
//---------------------------------------------------------------------------
// A fact is something a chain needs or produces.
// F_NOTFLAG / F_NOTITEM: a condition met only while the flag is clear / the item is absent. Nothing produces them;
// they are kept so a gate can be printed as it stands at a given record.
enum FactKind { F_FLAG = 1, F_ITEM, F_MAP, F_REC, F_COUNTER, F_ACTION, F_NOTFLAG, F_NOTITEM };
static std::string FactStr(int kind, int a, int b = 0)
{
	char s[64];
	switch (kind)
	{
	case F_FLAG: snprintf(s, sizeof s, "flag %d", a); break;
	case F_ITEM: snprintf(s, sizeof s, "item %d", a); break;
	case F_MAP: snprintf(s, sizeof s, "map %s", gMaps[a].name.c_str()); break;
	case F_REC: snprintf(s, sizeof s, "%s #%d enabled", gMaps[a].name.c_str(), b); break;
	case F_COUNTER: snprintf(s, sizeof s, "%s counter #%d done", gMaps[a].name.c_str(), b); break;
	case F_ACTION: snprintf(s, sizeof s, "act %d", a); break;
	case F_NOTFLAG: snprintf(s, sizeof s, "!flag %d", a); break;
	case F_NOTITEM: snprintf(s, sizeof s, "!item %d", a); break;
	default: snprintf(s, sizeof s, "?"); break;
	}
	return s;
}
struct Fact
{
	int kind = 0, a = 0, b = 0;
	bool operator<(const Fact &o) const { return kind != o.kind ? kind < o.kind : a != o.a ? a < o.a : b < o.b; }
	bool operator==(const Fact &o) const { return kind == o.kind && a == o.a && b == o.b; }
};

// One produced fact, with what must hold for it to be produced.
struct Prod
{
	Fact what;
	std::set<Fact> needs;
	int unit = -1;          // gUnits index, or -1 (from an item block / new game)
	std::string via;        // where, for printing
};
static std::vector<Prod> gProds;

//---------------------------------------------------------------------------
// DBASE100 action summary
//---------------------------------------------------------------------------
struct ActSum
{
	std::string text;                 // compact, ordered
	std::set<int> cuts, flagsSet, flagsClr, gives, removes, rawIdx, rawObj, jumps;
	int says = 0, choices = 0, randoms = 0;
	bool health = false, fullscreen = false;
	std::vector<std::pair<Fact, std::set<Fact>>> produces;   // fact, extra needs inside the action
	std::set<int> needFlags, needItems;                       // gates met anywhere in the action
};
static std::map<int, ActSum> gActCache;

// Who runs an action, and which actions run a RAW chain by object id (0x36) or by index (0x23). Filled before units.
static std::map<int, std::set<std::string>> gActCallers;     // act -> "i60.use", "i14.info", "act583", "STUDY1 #300"
static std::map<int, std::set<int>> gActItems;                // act -> items whose OnUse/inspect/info/document block reaches it
static std::map<int, std::set<int>> gRawObjActs, gRawIdxActs; // id/index -> actions

static void SumAction(int id, ActSum &s, std::set<int> &seen, std::set<Fact> gate, int depth)
{
	const DbAction *a = gDb.Action(id);
	if (!a || !seen.insert(id).second || depth > 6) return;
	std::set<Fact> atChoice;
	bool inChoice = false;
	std::string &t = s.text;
	for (const DbCommand &c : gDb.Commands(*a))
	{
		if (c.operand == 0) continue;   // the interpreter skips operand 0 (dbase100.c:633)
		char b[96];
		b[0] = 0;
		const int lo = (int)(c.operand & 0xffff);
		switch (c.opcode)
		{
		case 0x01:
			snprintf(b, sizeof b, " if%sflag%d", c.ifNot ? "!" : "", (int)c.operand);
			if (!c.ifNot) { gate.insert({ F_FLAG, (int)c.operand }); s.needFlags.insert((int)c.operand); }
			break;
		case 0x02:
			snprintf(b, sizeof b, " if%scount(i%d)>%u", c.ifNot ? "!" : "", lo, c.operand >> 16);
			if (!c.ifNot) { gate.insert({ F_ITEM, lo }); s.needItems.insert(lo); }
			break;
		case 0x03: snprintf(b, sizeof b, " if%scount(i%d)==%u", c.ifNot ? "" : "!", lo, c.operand >> 16); break;
		case 0x04:
			snprintf(b, sizeof b, " %cflag%d", c.ifNot ? '-' : '+', (int)c.operand);
			(c.ifNot ? s.flagsClr : s.flagsSet).insert((int)c.operand);
			if (!c.ifNot) s.produces.push_back({ { F_FLAG, (int)c.operand }, gate });
			break;
		case 0x05: s.says++; if (t.empty() || t.compare(t.size() - 4, 4, " say") != 0) snprintf(b, sizeof b, " say"); break;
		case 0x07: snprintf(b, sizeof b, " GDV:%s", Cut((int)c.operand - 1).c_str()); s.cuts.insert((int)c.operand - 1); break;
		case 0x08: if (!inChoice) { atChoice = gate; inChoice = true; } s.choices++; snprintf(b, sizeof b, " choice"); break;
		case 0x09: gate = atChoice; snprintf(b, sizeof b, " |"); break;
		case 0x0a: snprintf(b, sizeof b, " end"); break;
		case 0x0b: s.randoms++; snprintf(b, sizeof b, " random"); break;
		case 0x0d: snprintf(b, sizeof b, " skip-if%sflag%d", c.ifNot ? "!" : "", lo); break;
		case 0x0e: s.fullscreen = true; snprintf(b, sizeof b, " picture"); break;
		case 0x11:
			snprintf(b, sizeof b, " %s%d", c.ifNot ? "take" : "give", (int)(int16_t)lo);
			(c.ifNot ? s.removes : s.gives).insert((int)(int16_t)lo);
			if (!c.ifNot) s.produces.push_back({ { F_ITEM, (int)(int16_t)lo }, gate });
			break;
		case 0x1d:
			snprintf(b, sizeof b, " ->act%d{", (int)c.operand);
			t += b;
			s.jumps.insert((int)c.operand);
			SumAction((int)c.operand, s, seen, gate, depth + 1);
			t += " }";
			b[0] = 0;
			break;
		case 0x23: snprintf(b, sizeof b, " raw#%d", lo); s.rawIdx.insert(lo); break;
		case 0x36: snprintf(b, sizeof b, " rawobj%d", lo); s.rawObj.insert(lo); break;
		case 0x26: s.health = true; snprintf(b, sizeof b, " health%d", (int)(int16_t)lo); break;
		default: break;
		}
		t += b;
	}
}
static const ActSum &Act(int id)
{
	auto it = gActCache.find(id);
	if (it != gActCache.end()) return it->second;
	ActSum &s = gActCache[id];
	std::set<int> seen;
	SumAction(id, s, seen, {}, 0);
	return s;
}

static const char *BlockName(int t)
{
	switch (t) { case DBTRIG_ON_USE: return "use"; case DBTRIG_INSPECT: return "inspect"; case DBTRIG_INFORMATION: return "info";
	case DBTRIG_DOCUMENT: return "doc"; case DBTRIG_CHANGE_NAME: return "name"; default: return "blk"; }
}

static void BuildCallers(const std::vector<std::string> &mapNames, const std::vector<Map *> &maps)
{
	for (int id = 1; id <= gDb.ActionCount(); id++)
	{
		const DbAction *a = gDb.Action(id);
		if (!a) continue;
		for (const DbCommand &c : gDb.Commands(*a))
		{
			if (c.operand == 0) continue;
			if (c.opcode == 0x1d) gActCallers[(int)c.operand].insert("act" + std::to_string(id));
			if (c.opcode == 0x36) gRawObjActs[(int)(c.operand & 0xffff)].insert(id);
			if (c.opcode == 0x23) gRawIdxActs[(int)(c.operand & 0xffff)].insert(id);
		}
	}
	for (int id = 1; id <= gDb.ItemCount(); id++)
	{
		const DbItem *it = gDb.Item(id);
		if (!it) continue;
		for (const DbTriggerBlock &b : it->blocks)
			for (const DbCommand &c : gDb.Commands(b))
			{
				int act = 0;
				if (b.trigger == DBTRIG_ON_USE && c.opcode == 0x35 && !c.ifNot) act = (int)c.operand;
				else if (b.trigger != DBTRIG_ON_USE && c.opcode == 0x1d) act = (int)c.operand;
				if (!act) continue;
				gActCallers[act].insert("i" + std::to_string(id) + "." + BlockName(b.trigger));
				if (b.trigger == DBTRIG_ON_USE) gActItems[act].insert(id);
			}
	}
	for (size_t mi = 0; mi < maps.size(); mi++)
		for (size_t r = 0; r < maps[mi]->commands.size(); r++)
		{
			const Command &c = maps[mi]->commands[r];
			if (c.opcode == 0x2b || c.opcode == 0x36) gActCallers[c.key].insert(mapNames[mi] + " #" + std::to_string(r + 1));
		}
	// Items whose OnUse reaches an action through jumps.
	bool grew = true;
	while (grew)
	{
		grew = false;
		for (int id = 1; id <= gDb.ActionCount(); id++)
		{
			const DbAction *a = gDb.Action(id);
			if (!a || !gActItems.count(id)) continue;
			for (const DbCommand &c : gDb.Commands(*a))
				if (c.opcode == 0x1d)
					for (int i : std::set<int>(gActItems[id]))
						if (gActItems[(int)c.operand].insert(i).second) grew = true;
		}
	}
}

// The items whose OnUse runs the click chain of object id `oid` (DBASE100 0x36), e.g. the Mask at a Mantel Stone.
static std::set<int> ItemsCallingObj(int oid)
{
	std::set<int> r;
	auto it = gRawObjActs.find(oid);
	if (it == gRawObjActs.end()) return r;
	for (int a : it->second) { auto j = gActItems.find(a); if (j != gActItems.end()) r.insert(j->second.begin(), j->second.end()); }
	return r;
}
static std::string ObjCallers(int oid)
{
	std::string s;
	auto it = gRawObjActs.find(oid);
	if (it == gRawObjActs.end()) return s;
	for (int a : it->second)
	{
		s += " <-act" + std::to_string(a);
		auto c = gActCallers.find(a);
		if (c != gActCallers.end()) { s += "("; bool first = true; for (auto &x : c->second) { s += (first ? "" : ",") + x; first = false; } s += ")"; }
	}
	return s;
}

//---------------------------------------------------------------------------
// Units: one entry record (or a chain an action calls by index) and what its chain does
//---------------------------------------------------------------------------
struct Unit
{
	int map = 0, rec = 0;          // rec is 1-based
	int dir = -1, base = 0;
	int key = 0;
	bool offAtLoad = false, oneShot = false;
	std::string kind, subject, seq;
	std::vector<int> recs;         // records walked (1-based)
	// derived
	std::set<int> itemsHand, itemsCarried, itemsRefused, flagsNeed, flagsRefused, flagsSet, flagsClr, flagsTgl;
	std::set<int> gives, removes, acts, cuts, toggled, counters, ifCounters, itemsList;
	std::vector<std::pair<std::string, int>> warps;   // dest, key
	std::vector<std::string> warpGates;              // the gate at each warp record
	bool warpGated = false;                          // a flag or item is tested before some warp record
	int doors = 0, delays = 0, delayTicks = 0, randomDelays = 0, counts = 0, geometry = 0, spawns = 0, lights = 0,
		textures = 0, damage = 0, says = 0, givesClicked = 0, stepsMask = 0, choices = 0, listTests = 0, lastItemTests = 0, removeLast = 0;
	bool refusalLatched = false;
	bool unreachable = false;      // its only object is hidden at load and no 0x16/0x3c in the map ever names it
	bool sequence = false;         // a counter, a step or a count in the chain
	std::set<int> viaItems;        // items whose inventory use runs this chain (0x36 from an OnUse action)
	bool onPath = false;
	std::set<int> pathChapters;
	std::string puzzleKind;
	bool puzzle = false;
};
static std::vector<Unit> gUnits;
static std::map<std::pair<int, int>, std::vector<int>> gUnitsByRec;   // (map, rec) -> units that walk it

static std::string Subject(const MapData &md, int dir, int key)
{
	char b[96];
	switch (dir)
	{
	case D_CLICK: case D_EXAMOBJ: case D_HITOBJ: case D_WALKOBJ:
	{
		auto it = md.objById.find(key);
		if (it == md.objById.end() || it->second.empty())
		{
			snprintf(b, sizeof b, "id %d (not placed)", key);
			std::string s = b;
			const std::string c = ObjCallers(key);
			return s + c;
		}
		const Object *o = it->second[0];
		const uint16_t rid = (uint16_t)(o->textureIndex | (o->textureSource << 8));
		const int item = rid >= 0x200 ? gDb.FindItemByRenderId(rid) : 0;
		const int sec = md.sectorOfObj.at(key);
		if (item) snprintf(b, sizeof b, "obj %d \"%s\" sec %d", key, ItemName(item).c_str(), sec);
		else snprintf(b, sizeof b, "obj %d art %d:%d sec %d", key, o->textureSource, o->textureIndex, sec);
		if (it->second.size() > 1) { std::string s = b; s += " x" + std::to_string(it->second.size()); return s; }
		return b;
	}
	case D_USEWALL: case D_EXAMWALL: case D_HITWALL:
	{
		int face = -1;
		for (size_t f = 0; f < md.m.faces.size() && face < 0; f++)
		{
			const Face &fc = md.m.faces[f];
			if (fc.textureMap >= 0 && md.m.textureMaps[fc.textureMap].extended && md.m.textureMaps[fc.textureMap].faceID == key) face = (int)f;
		}
		if (face < 0) { snprintf(b, sizeof b, "wall id %d (no face)", key); return b; }
		const TextureMap &tm = md.m.textureMaps[md.m.faces[face].textureMap];
		snprintf(b, sizeof b, "wall id %d tex %d sec %d", key, tm.midTexture ? tm.midTexture : tm.upperTexture ? tm.upperTexture : tm.lowerTexture,
			md.m.faces[face].sector);
		return b;
	}
	case D_USEFLOOR: case D_EXAMFLOOR: case D_ENTER:
	{
		for (size_t s = 0; s < md.m.sectors.size(); s++)
			if (md.m.sectors[s].commandID == key) { snprintf(b, sizeof b, "sector id %d (sec %zu)", key, s); return b; }
		snprintf(b, sizeof b, "sector id %d", key);
		return b;
	}
	default: return "";
	}
}

static std::string SetStrFwd(const std::set<int> &s)
{
	std::string o;
	for (int v : s) { if (!o.empty()) o += ","; o += "i" + std::to_string(v); }
	return o;
}

static void Tok(Unit &u, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void Tok(Unit &u, const char *fmt, ...)
{
	char b[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	u.seq += " ";
	u.seq += b;
}

// The walk. `gate` is the set of facts the chain must have had to reach this record.
struct WalkCtx
{
	int unit;
	std::set<int> seen;
};
static void Walk(MapData &md, int mapIdx, int start, WalkCtx &w, std::set<Fact> gate, int depth);

static void AddProd(int unit, const Fact &what, std::set<Fact> needs, const std::string &via)
{
	if (unit >= 0 && gUnits[unit].unreachable) return;
	Prod p;
	p.what = what;
	p.needs = std::move(needs);
	p.unit = unit;
	p.via = via;
	gProds.push_back(p);
}

// RAW 0x2b, and an examine record's own action (examine_world_object, input.c:949-965: the record's word +0x0a is a
// DBASE100 action, run BEFORE its chain).
static void DoAct(MapData &md, int mapIdx, WalkCtx &w, const std::set<Fact> &here, int act, int depth, const std::string &via)
{
	Map &m = md.m;
	const int n = (int)m.commands.size();
	Unit &u = gUnits[w.unit];

			const ActSum &s = Act(act);
			u.seq += " act" + std::to_string(act) + "{" + s.text + " }";
			u.acts.insert(act);
			u.says += s.says;
			u.choices += s.choices;
			for (int x : s.cuts) u.cuts.insert(x);
			for (int x : s.flagsSet) u.flagsSet.insert(x);
			for (int x : s.flagsClr) u.flagsClr.insert(x);
			for (int x : s.gives) u.gives.insert(x);
			for (int x : s.removes) u.removes.insert(x);
			// A gate inside an action counts for the unit only when it gates something the action produces (a flag,
			// an item); "if Rebecca is here, she comments" (flag 1 before a line) is not a gate on progress.
			for (auto &p : s.produces)
				for (const Fact &f : p.second)
				{
					if (f.kind == F_FLAG) u.flagsNeed.insert(f.a);
					if (f.kind == F_ITEM) u.itemsCarried.insert(f.a);
				}
			{
				std::set<Fact> h = here;
				AddProd(w.unit, { F_ACTION, act }, h, via);
			}
			for (auto &p : s.produces)
			{
				std::set<Fact> nd = here;
				nd.insert(p.second.begin(), p.second.end());
				AddProd(w.unit, p.first, nd, via + " act" + std::to_string(act));
			}
			// The chains an action calls run in this map.
			for (int ri : s.rawIdx)
				if (ri >= 1 && ri <= n && depth < 4) { Tok(u, "<call #%d:", ri); Walk(md, mapIdx, ri, w, here, depth + 1); Tok(u, ">"); }
			for (int oid : s.rawObj)
				for (int r = 1; r <= n; r++)
					if (md.dirOf[r - 1] == D_CLICK && m.commands[r - 1].key == oid && depth < 4)
					{ Tok(u, "<callobj%d #%d:", oid, r); Walk(md, mapIdx, m.commands[r - 1].linkIndex, w, here, depth + 1); Tok(u, ">"); }
}

static void Walk(MapData &md, int mapIdx, int start, WalkCtx &w, std::set<Fact> gate, int depth)
{
	Map &m = md.m;
	const int n = (int)m.commands.size();
	for (int k = start, steps = 0; k >= 1 && k <= n && steps < 200; steps++)
	{
		if (!w.seen.insert(k).second) { Tok(gUnits[w.unit], "(->#%d)", k); break; }
		Unit &u = gUnits[w.unit];
		u.recs.push_back(k);
		gUnitsByRec[{ mapIdx, k }].push_back(w.unit);
		const Command &c = m.commands[k - 1];
		if (md.dirOf[k - 1] >= 0 && k != u.rec) { Tok(u, "(entry #%d)", k); break; }
		std::set<Fact> here = gate;
		if (c.disabled) { here.insert({ F_REC, mapIdx, k }); u.seq += " [off]"; }
		const std::string via = md.name + " #" + std::to_string(u.rec);
		const uint8_t f6 = c.fireFlags;
		switch (c.opcode)
		{
		case 0x28:
		{
			const bool need = (f6 & 1) != 0;
			Tok(u, "if%sflag%d", need ? "" : "!", c.key);
			if (need) { u.flagsNeed.insert(c.key); gate.insert({ F_FLAG, c.key }); }
			else { u.flagsRefused.insert(c.key); gate.insert({ F_NOTFLAG, c.key }); }
			break;
		}
		case 0x27:
		{
			const bool need = (f6 & 1) != 0;
			// List mode: the items are alternatives ("one of these", find_or_autoselect over the list), not needs.
			if ((f6 & 8) && !(f6 & 4)) { Tok(u, "list+i%d", c.key); u.listTests++; u.itemsList.insert(c.key); break; }
			if (f6 & 8) { Tok(u, "if%slist", need ? "" : "!"); break; }
			if (f6 & 4) { Tok(u, "if-last-given==i%d", c.key); u.lastItemTests++; gate.insert({ F_ITEM, c.key }); break; }
			const bool hand = (f6 & 2) != 0;
			Tok(u, "if%si%d%s", need ? "" : "!", c.key, hand ? "@hand" : "");
			if (need) { (hand ? u.itemsHand : u.itemsCarried).insert(c.key); gate.insert({ F_ITEM, c.key }); }
			else { u.itemsRefused.insert(c.key); gate.insert({ F_NOTITEM, c.key }); }
			break;
		}
		case 0x36: Tok(u, "onfail:act%d", c.key); u.refusalLatched = true; break;
		case 0x38: Tok(u, "else->#%d", c.key); break;
		case 0x26:
		{
			const char op = (f6 & 2) ? '~' : (f6 & 1) ? '-' : '+';
			Tok(u, "%cflag%d", op, c.key);
			(op == '+' ? u.flagsSet : op == '-' ? u.flagsClr : u.flagsTgl).insert(c.key);
			if (op != '-') AddProd(w.unit, { F_FLAG, c.key }, here, via);
			break;
		}
		case 0x29:
			// key 0: give_item(0) -- the item of the object just used (a scripted pickup of the clicked thing).
			if (c.key == 0) { Tok(u, "give(clicked)"); u.givesClicked++; break; }
			Tok(u, "give%d", c.key); u.gives.insert(c.key); AddProd(w.unit, { F_ITEM, c.key }, here, via); break;
		case 0x2a:
			if (c.key) { Tok(u, "take%d", c.key); u.removes.insert(c.key); }
			else { Tok(u, "take-last"); u.removeLast++; }
			break;
		case 0x2b: DoAct(md, mapIdx, w, here, c.key, depth, via); break;
		case 0x2f: Tok(u, "door"); u.doors++; break;
		case 0x3b:
		{
			WarpRequest wr;
			const uint16_t a = c.Word(0x0a), b2 = c.Word(0x0c), c3 = c.Word(0x0e), d4 = c.Word(0x10);
			const uint16_t ws[4] = { a, b2, c3, d4 };
			for (int i = 0; i < 4; i++) { wr.name[2 * i] = (char)(ws[i] & 0xff); wr.name[2 * i + 1] = (char)(ws[i] >> 8); }
			std::string dest;
			for (int i = 0; i < 8; i++) { const uint8_t ch = (uint8_t)wr.name[i]; if (ch <= 0x20) break; dest += (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch); }
			if (dest.empty()) dest = md.name;
			Tok(u, "WARP:%s@%d", dest.c_str(), c.key);
			u.warps.push_back({ dest, c.key });
			{
				// The gate as it stands at THIS record: what the chain tested before reaching it.
				std::string g;
				for (const Fact &f : here)
				{
					if (f.kind == F_MAP) continue;
					std::string x = FactStr(f.kind, f.a, f.b);
					if (f.kind == F_ITEM || f.kind == F_NOTITEM) x += " " + ItemName(f.a);
					if (f.kind == F_REC && f.b == u.rec) x = "entry off at load";
					g += (g.empty() ? "" : ", ") + x;
				}
				u.warpGates.push_back(g.empty() ? "none" : g);
				for (const Fact &f : here) if (f.kind == F_FLAG || f.kind == F_ITEM) u.warpGated = true;
			}
			auto it = gMapIdx.find(dest);
			if (it != gMapIdx.end()) AddProd(w.unit, { F_MAP, it->second }, here, via);
			break;
		}
		case 0x12:
			Tok(u, "delay%d%s", c.Word(6), (c.modifier & 2) ? "~rnd" : "");
			u.delays++;
			u.delayTicks += c.Word(6);
			if (c.modifier & 2) u.randomDelays++;
			break;
		case 0x15: case 0x1f: case 0x21: case 0x22:
			Tok(u, "count%02x(%d->%d/%d)", c.opcode, c.Word(0x0a), c.Word(0x0c), c.Word(8));
			u.counts++;
			break;
		case 0x1e:
		{
			// key 0: cmd_modify_count sets the step mode and delta and returns before resolving a target
			// (raw_commands.c:4001-4007), so they apply to the NEXT Count the chain runs: e.g. OR in one bit.
			if (c.key == 0)
			{
				// mode (f6&3)+1: 1 subtract-clamp, 2 add, 3 xor, 4 = the switch's default, increment (:156-201).
				static const char *mode[4] = { "sub", "add", "xor", "inc" };
				Tok(u, "step(%s %d)", mode[f6 & 3], c.Word(0x0a));
				u.stepsMask++;
				break;
			}
			Tok(u, "step#%d", c.key);
			u.counters.insert(c.key);
			if (c.key >= 1 && c.key <= n)
			{
				const Command &t = m.commands[c.key - 1];
				AddProd(w.unit, { F_COUNTER, mapIdx, c.key }, here, via);
				if (t.nextIndex && depth < 4 && (t.opcode == 0x15 || t.opcode == 0x1f || t.opcode == 0x21 || t.opcode == 0x22))
				{
					std::set<Fact> g2 = here;
					g2.insert({ F_COUNTER, mapIdx, c.key });
					u.ifCounters.insert(c.key);
					Tok(u, "<when #%d(%d->%d) done:", c.key, t.Word(0x0a), t.Word(0x0c));
					Walk(md, mapIdx, t.nextIndex, w, g2, depth + 1);
					Tok(gUnits[w.unit], ">");
				}
			}
			break;
		}
		case 0x17:
		{
			const bool force = (f6 & 0x06) != 0;
			const char *what = !force ? "toggle" : (f6 & 2) ? "disable" : "enable";
			// The bit forced is 0x08 (skip) for ordinary records: "set" disables, "clear" enables (cmd_toggle_command :541).
			if (force) what = (f6 & 2) ? "enable" : "disable";
			Tok(u, "%s#%d", what, c.key);
			u.toggled.insert(c.key);
			if (strcmp(what, "disable") != 0) AddProd(w.unit, { F_REC, mapIdx, c.key }, here, via);
			break;
		}
		case 0x40:
			Tok(u, "<sub #%d:", c.Word(6));
			if (depth < 4) Walk(md, mapIdx, c.Word(6), w, here, depth + 1);
			Tok(gUnits[w.unit], ">");
			break;
		case 0x07: case 0x09: case 0x03: Tok(u, "%s", c.opcode == 0x09 ? "move" : "height"); u.geometry++; break;
		case 0x23: Tok(u, "objheight"); u.geometry++; break;
		case 0x24: Tok(u, "rotobj"); u.geometry++; break;
		case 0x16: case 0x3c: Tok(u, "spawn"); u.spawns++; break;
		case 0x3a: Tok(u, "objid"); break;
		case 0x02: case 0x11: case 0x1d: Tok(u, "light"); u.lights++; break;
		case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x14: case 0x1c: case 0x20: case 0x2e: case 0x34:
			u.textures++;
			if (u.seq.size() < 4 || u.seq.compare(u.seq.size() - 4, 4, " tex") != 0) Tok(u, "tex");
			break;
		case 0x33: case 0x35: Tok(u, "damage"); u.damage++; break;
		case 0x10: break;   // ambient sound node
		case 0x2d: Tok(u, "particles"); break;
		case 0x3f: Tok(u, "turnplayer"); break;
		case 0x41: Tok(u, "slowplayer"); break;
		case 0x42: Tok(u, "invfilter"); break;
		default: break;
		}
		// The else branch of a 0x38 is walked once, after the main line, as an alternative.
		if (c.opcode == 0x38 && c.key >= 1 && c.key <= n && !w.seen.count(c.key) && depth < 4)
		{
			const int next = c.nextIndex;
			// main line first
			Walk(md, mapIdx, next, w, gate, depth);
			if (w.seen.count(c.key)) return;
			Tok(gUnits[w.unit], "<else #%d:", c.key);
			Walk(md, mapIdx, c.key, w, gate, depth + 1);
			Tok(gUnits[w.unit], ">");
			return;
		}
		if (c.opcode == 0x12 && c.nextIndex == 0) break;
		k = c.nextIndex;
	}
}

static int NewUnit(int mapIdx, int rec, int dir)
{
	MapData &md = gMaps[mapIdx];
	const Command &c = md.m.commands[rec - 1];
	Unit u;
	u.map = mapIdx;
	u.rec = rec;
	u.dir = dir;
	u.base = c.opcode;
	u.key = c.key;
	u.offAtLoad = c.disabled;
	u.oneShot = (c.fireFlags & 0x10) != 0;
	u.kind = dir >= 0 ? DirName(dir, c.opcode) : "called";
	u.subject = dir >= 0 ? Subject(md, dir, c.key) : "";
	if (dir == D_CLICK || dir == D_EXAMOBJ || dir == D_WALKOBJ)
	{
		auto it = md.objById.find(c.key);
		if (it != md.objById.end() && !it->second.empty())
		{
			bool allHidden = true;
			for (const Object *o : it->second) if (!(o->flags & 0x80)) allHidden = false;
			bool shown = false;
			for (const Command &x : md.m.commands) if ((x.opcode == 0x16 || x.opcode == 0x3c) && x.key == c.key) shown = true;
			if (allHidden && !shown) { u.unreachable = true; u.subject += " HIDDEN,never shown"; }
		}
	}
	gUnits.push_back(u);
	return (int)gUnits.size() - 1;
}

static void BuildUnits()
{
	for (int mi = 0; mi < (int)gMaps.size(); mi++)
	{
		MapData &md = gMaps[mi];
		const int n = (int)md.m.commands.size();
		for (int r = 1; r <= n; r++)
		{
			const int d = md.dirOf[r - 1];
			if (d < 0 || d == D_LOADMOVER) continue;
			const Command &c = md.m.commands[r - 1];
			const int ui = NewUnit(mi, r, d);
			std::set<Fact> gate;
			gate.insert({ F_MAP, mi });
			if (c.disabled) gate.insert({ F_REC, mi, r });
			if (d == D_CLICK)
			{
				gUnits[ui].viaItems = ItemsCallingObj(c.key);
				if (!gUnits[ui].viaItems.empty()) { Tok(gUnits[ui], "use[%s]:", SetStrFwd(gUnits[ui].viaItems).c_str()); }
				if (gUnits[ui].viaItems.size() == 1) gate.insert({ F_ITEM, *gUnits[ui].viaItems.begin() });
			}
			WalkCtx w;
			w.unit = ui;
			w.seen.insert(r);
			gUnits[ui].recs.push_back(r);
			gUnitsByRec[{ mi, r }].push_back(ui);
			if (c.opcode == 0x02) { Tok(gUnits[ui], "lightswitch"); gUnits[ui].lights++; continue; }
			if ((d == D_EXAMOBJ || d == D_EXAMWALL || d == D_EXAMFLOOR) && c.aux)
				DoAct(md, mi, w, gate, c.aux, 0, md.name + " #" + std::to_string(r));
			Walk(md, mi, c.linkIndex, w, gate, 0);
		}
	}
}

//---------------------------------------------------------------------------
// Items: where found, where used
//---------------------------------------------------------------------------
struct ItemUse
{
	std::map<std::string, int> placed;              // map -> pickups
	std::set<std::string> givenBy, testedBy, removedBy;   // "MAP #rec"
	std::set<int> combineWith, producedFrom;        // OnUse partners / sources
	bool start = false;
};
static std::vector<ItemUse> gItems;

static int FirstPickable(uint16_t rid)
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

// Combinations: OnUse blocks with a target (inventory.c:551-594 via item_dossier).
struct Recipe { int a = 0, b = 0, action = 0; std::vector<int> results; };
static std::vector<Recipe> gRecipes;

static void BuildItems()
{
	gItems.assign(gDb.ItemCount() + 1, {});
	if (const DbAction *a = gDb.Action(3))
		for (const DbCommand &c : gDb.Commands(*a))
			if (c.opcode == 0x11 && !c.ifNot && (int)c.operand <= gDb.ItemCount())
			{
				gItems[c.operand].start = true;
				AddProd(-1, { F_ITEM, (int)c.operand }, {}, "new game (act 3)");
			}
	for (int mi = 0; mi < (int)gMaps.size(); mi++)
	{
		MapData &md = gMaps[mi];
		std::set<int> liveClick;
		for (size_t r = 0; r < md.m.commands.size(); r++)
			if (md.dirOf[r] == D_CLICK && !md.m.commands[r].disabled) liveClick.insert(md.m.commands[r].key);
		for (const auto &grp : md.m.objects)
			for (const Object &o : grp)
			{
				if (o.renderType & (0x01 | 0x02 | 0x10)) continue;
				if (o.commandID != 0 && liveClick.count(o.commandID)) continue;
				const uint16_t rid = (uint16_t)(o.textureIndex | (o.textureSource << 8));
				const int id = FirstPickable(rid);
				if (!id) continue;
				gItems[id].placed[md.name]++;
				AddProd(-1, { F_ITEM, id }, { { F_MAP, mi } }, md.name + " pickup" + ((o.flags & 0x80) ? " (hidden at load)" : ""));
			}
	}
	for (const Unit &u : gUnits)
	{
		if (u.unreachable) continue;
		const std::string w = gMaps[u.map].name + " #" + std::to_string(u.rec);
		for (int i : u.itemsList) if (i > 0 && i < (int)gItems.size()) gItems[i].testedBy.insert(w + "@hand(one of)");
		for (int i : u.gives) if (i > 0 && i < (int)gItems.size()) gItems[i].givenBy.insert(w);
		for (int i : u.itemsHand) if (i > 0 && i < (int)gItems.size()) gItems[i].testedBy.insert(w + "@hand");
		for (int i : u.itemsCarried) if (i > 0 && i < (int)gItems.size()) gItems[i].testedBy.insert(w);
		for (int i : u.removes) if (i > 0 && i < (int)gItems.size()) gItems[i].removedBy.insert(w);
	}
	for (int id = 1; id <= gDb.ItemCount(); id++)
	{
		const DbItem *it = gDb.Item(id);
		if (!it) continue;
		for (const DbTriggerBlock &b : it->blocks)
		{
			std::vector<DbCommand> cs = gDb.Commands(b);
			if (b.trigger == DBTRIG_ON_USE)
			{
				Recipe r;
				r.a = id;
				for (const DbCommand &c : cs)
				{
					if (c.opcode == 0x34 && !c.ifNot) r.b = (int)c.operand;
					if (c.opcode == 0x34 && c.ifNot) r.results.push_back((int)c.operand);
					if (c.opcode == 0x35 && !c.ifNot) r.action = (int)c.operand;
				}
				gRecipes.push_back(r);
				std::set<Fact> nd = { { F_ITEM, id } };
				if (r.b) nd.insert({ F_ITEM, r.b });
				for (int x : r.results)
					if (x > 0 && x < (int)gItems.size())
					{
						gItems[x].producedFrom.insert(id);
						AddProd(-1, { F_ITEM, x }, nd, "combine i" + std::to_string(id) + (r.b ? "+i" + std::to_string(r.b) : " (self)"));
					}
				if (r.b > 0 && r.b < (int)gItems.size()) { gItems[id].combineWith.insert(r.b); gItems[r.b].combineWith.insert(id); }
				if (r.action)
				{
					const ActSum &s = Act(r.action);
					for (auto &p : s.produces)
					{
						std::set<Fact> n2 = nd;
						n2.insert(p.second.begin(), p.second.end());
						AddProd(-1, p.first, n2, "use i" + std::to_string(id) + (r.b ? "+i" + std::to_string(r.b) : "") + " act" + std::to_string(r.action));
					}
				}
			}
			else if (b.trigger == DBTRIG_INSPECT || b.trigger == DBTRIG_INFORMATION || b.trigger == DBTRIG_DOCUMENT)
			{
				// What looking at (or asking about) an item can produce: its gives, flags and jumps.
				std::set<int> seen;
				for (const DbCommand &c : cs)
				{
					if (c.operand == 0) continue;
					std::set<Fact> nd = { { F_ITEM, id } };
					if (c.opcode == 0x11 && !c.ifNot) AddProd(-1, { F_ITEM, (int)(c.operand & 0xffff) }, nd, "inspect i" + std::to_string(id));
					if (c.opcode == 0x04 && !c.ifNot) AddProd(-1, { F_FLAG, (int)c.operand }, nd, "inspect i" + std::to_string(id));
					if (c.opcode == 0x1d)
					{
						const ActSum &s = Act((int)c.operand);
						for (auto &p : s.produces)
						{
							std::set<Fact> n2 = nd;
							n2.insert(p.second.begin(), p.second.end());
							AddProd(-1, p.first, n2, "inspect i" + std::to_string(id) + " ->act" + std::to_string(c.operand));
						}
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
// Classification
//---------------------------------------------------------------------------
static int ChapterOfCut(int cut)
{
	for (int ch = 1; ch < (int)gCards.size(); ch++) if (gCards[ch].cut == cut) return ch;
	return 0;
}

// Light-giving items (docs/WORLD_DESIGN.md 2.2, ITEM_DESIGN.md): Matchbox 16, Tinder (Burning) 194, Magical Torch 175.
static bool IsFireItem(int i) { return i == 16 || i == 194; }

static void Classify(Unit &u)
{
	const bool needs = !u.itemsHand.empty() || !u.itemsCarried.empty() || !u.flagsNeed.empty() || u.lastItemTests || u.listTests;
	const bool produces = !u.gives.empty() || !u.flagsSet.empty() || !u.flagsTgl.empty() || !u.warps.empty() || u.doors
		|| !u.toggled.empty() || !u.counters.empty() || !u.cuts.empty() || u.givesClicked;
	bool chapterCard = false;
	for (int c : u.cuts) if (ChapterOfCut(c)) chapterCard = true;
	bool fire = false;
	for (int i : u.itemsHand) if (IsFireItem(i)) fire = true;
	const bool handUse = !u.itemsHand.empty() || u.listTests || u.lastItemTests;
	// Consumed: the chain removes an item it tested, or "the last item" after a list/last test.
	bool consumed = u.removeLast > 0 && handUse;
	for (int i : u.itemsHand) if (u.removes.count(i)) consumed = true;
	for (int i : u.itemsList) if (u.removes.count(i)) consumed = true;
	const bool invUse = !u.viaItems.empty();
	u.sequence = !u.counters.empty() || u.counts || u.stepsMask;
	const bool clickish = u.dir == D_CLICK || u.dir == D_USEWALL || u.dir == D_USEFLOOR;

	std::string k;
	if (u.unreachable) k = "unreachable (hidden object)";
	else if (u.kind == "switch") k = "light switch";
	else if (chapterCard) k = "chapter card";
	else if (invUse && !u.warps.empty()) k = "use item (inventory) travel";
	else if (invUse) k = "use item (inventory)";
	else if (!u.warps.empty() && !u.warpGated) k = "map transition";
	else if (!u.warps.empty()) k = "gated map transition";
	else if (fire && (u.lights || u.textures || u.spawns)) k = "light (fire item)";
	else if (u.doors && (!u.itemsHand.empty() || !u.itemsCarried.empty())) k = "locked door + key";
	else if (handUse && consumed) k = "place / hand over item";
	else if (handUse) k = "use item on object";
	else if (!u.itemsCarried.empty()) k = "carry gate";
	else if (!u.flagsNeed.empty() && (u.dir == D_ENTER || u.dir == D_TIMER)) k = "flag-gated event";
	else if (!u.flagsNeed.empty()) k = "flag-gated use";
	else if (u.choices) k = "dialogue";
	else if ((u.dir == D_HITOBJ || u.dir == D_HITWALL) && produces) k = "attack / kill";
	else if (!u.cuts.empty()) k = "cutscene";
	else if (u.doors && u.offAtLoad) k = "door (enabled later)";
	else if (u.doors) k = "door (free)";
	else if (!u.gives.empty() || u.givesClicked) k = "take item (scripted)";
	else if (clickish && (u.sequence || !u.toggled.empty() || u.geometry || u.spawns || u.lights)) k = "switch / lever";
	else if (u.dir == D_ENTER && (u.spawns || !u.toggled.empty() || !u.flagsSet.empty() || u.geometry || u.sequence)) k = "walk-in event";
	else if (u.dir == D_TIMER) k = "timer";
	else if ((u.dir == D_EXAMOBJ || u.dir == D_EXAMWALL || u.dir == D_EXAMFLOOR) && !produces) k = "examine (comment)";
	else if (!u.flagsSet.empty()) k = "flag set";
	else if (u.says) k = "comment";
	else k = "ambient";
	u.puzzleKind = k;
	u.puzzle = !u.unreachable && (needs || invUse || (produces && k != "door (free)" && k != "map transition" && k != "examine (comment)"));
}

//---------------------------------------------------------------------------
// The dependency slice
//---------------------------------------------------------------------------
static std::map<Fact, std::vector<int>> gProdsOf;   // fact -> gProds indices

struct Slice
{
	std::set<Fact> needed;
	std::set<int> prods;                 // gProds indices kept
	std::set<int> units;
	std::map<int, std::set<Fact>> unitWhy;   // unit -> facts it produces that are needed
};

static void SliceFrom(const std::set<Fact> &goals, Slice &s)
{
	std::vector<Fact> work(goals.begin(), goals.end());
	for (const Fact &f : goals) s.needed.insert(f);
	while (!work.empty())
	{
		Fact f = work.back();
		work.pop_back();
		auto it = gProdsOf.find(f);
		if (it == gProdsOf.end()) continue;
		std::vector<int> ps = it->second;
		if (f.kind == F_MAP)
		{
			// A map is entered freely unless every way in is gated: then every gated way in is kept.
			bool anyFree = false;
			for (int p : ps)
			{
				bool gated = false;
				for (const Fact &n : gProds[p].needs) if (n.kind != F_MAP) gated = true;
				if (!gated) anyFree = true;
			}
			if (anyFree || gMaps[f.a].name == "STUDY1") continue;
		}
		for (int p : ps)
		{
			if (!s.prods.insert(p).second) continue;
			const Prod &pr = gProds[p];
			if (pr.unit >= 0) { s.units.insert(pr.unit); s.unitWhy[pr.unit].insert(f); }
			for (const Fact &n : pr.needs)
				if (s.needed.insert(n).second) work.push_back(n);
		}
	}
}

// The facts a unit needs to run at all (map, enabled entry), plus its gates up to the first chapter card it plays.
static std::set<Fact> UnitGoal(int ui)
{
	std::set<Fact> g;
	const Unit &u = gUnits[ui];
	g.insert({ F_MAP, u.map });
	if (u.offAtLoad) g.insert({ F_REC, u.map, u.rec });
	for (int f : u.flagsNeed) g.insert({ F_FLAG, f });
	for (int i : u.itemsHand) g.insert({ F_ITEM, i });
	for (int i : u.itemsCarried) g.insert({ F_ITEM, i });
	if (u.viaItems.size() == 1) g.insert({ F_ITEM, *u.viaItems.begin() });
	return g;
}

static std::vector<int> UnitsRunningAction(int act)
{
	std::vector<int> v;
	for (int ui = 0; ui < (int)gUnits.size(); ui++) if (gUnits[ui].acts.count(act)) v.push_back(ui);
	return v;
}

// Which actions reach `act` through 0x1d jumps (so a card played by a jumped-to action is found from its caller).
static std::set<int> ActionsReaching(int act)
{
	std::set<int> r = { act };
	bool grew = true;
	while (grew)
	{
		grew = false;
		for (int id = 1; id <= gDb.ActionCount(); id++)
		{
			if (r.count(id)) continue;
			const DbAction *a = gDb.Action(id);
			if (!a) continue;
			for (const DbCommand &c : gDb.Commands(*a))
				if (c.opcode == 0x1d && r.count((int)c.operand)) { r.insert(id); grew = true; break; }
		}
	}
	return r;
}

static std::vector<int> CardUnits(int ch)
{
	std::set<int> u;
	if (ch <= 0 || ch >= (int)gCards.size()) return {};
	for (int a : gCards[ch].actions)
		for (int b : ActionsReaching(a))
			for (int x : UnitsRunningAction(b)) u.insert(x);
	return std::vector<int>(u.begin(), u.end());
}

//---------------------------------------------------------------------------
// Printing
//---------------------------------------------------------------------------
static std::string SetStr(const std::set<int> &s, const char *pfx = "")
{
	std::string o;
	for (int v : s) { if (!o.empty()) o += ","; o += pfx + std::to_string(v); }
	return o;
}

static void PrintUnit(const Unit &u, bool brief = false)
{
	const MapData &md = gMaps[u.map];
	printf("  %-8s #%-4d %-12s %-34s %s%s| %-22s|%s\n", md.name.c_str(), u.rec, u.kind.c_str(), u.subject.c_str(),
		u.offAtLoad ? "[off] " : "", u.oneShot ? "[once] " : "", u.puzzleKind.c_str(), brief ? "" : u.seq.c_str());
}

static void PrintChapters()
{
	printf("CHAPTER CARDS: CHPTR<n>.GDV in the DBASE100 cutscene table (%d entries), the actions that play it (op 0x07),\n"
		"and the RAW records that run those actions (0x2b, also through 0x1d jumps). Entry = the trigger whose chain reaches it.\n",
		gDb.CutsceneCount());
	for (int ch = 1; ch < (int)gCards.size(); ch++)
	{
		const Card &c = gCards[ch];
		const DbCutscene *cs = c.cut >= 0 ? gDb.Cutscene(c.cut) : nullptr;
		printf("chapter %2d  cut %3d %-8s \"%s\" (gallery title, DBASE400)  actions %s\n", ch, c.cut, c.cut >= 0 ? Cut(c.cut).c_str() : "-",
			cs ? Text(cs->titleText, 40).c_str() : "", SetStr(c.actions, "act").c_str());
		for (int a : c.actions)
		{
			const ActSum &s = Act(a);
			printf("    act %d:%s\n", a, s.text.c_str());
			std::set<int> reach = ActionsReaching(a);
			reach.erase(a);
			if (!reach.empty()) printf("    reached also through 0x1d from %s\n", SetStr(reach, "act").c_str());
		}
		for (int ui : CardUnits(ch)) PrintUnit(gUnits[ui]);
		if (ch == 1) printf("  (new game: menu code 10 runs action 3, game_core.c:821-845; the first map is ROTH.RES's first `maps` entry, STUDY1)\n");
	}
	// Other actions that reach a card through a jump but are not run by a RAW record
	printf("\n");
}

static void PrintWarps()
{
	printf("MAP TRANSITIONS (RAW 0x3b): from map #rec(entry) -> destination @arrival key, and the gate as it stands at the 0x3b\n"
		"record (the conditions the chain met before it; '!' = met while clear/absent; 'enabled' = a record off at load).\n");
	std::map<std::string, std::map<std::string, int>> graph;
	for (const Unit &u : gUnits)
		for (size_t wi = 0; wi < u.warps.size(); wi++)
		{
			const auto &w = u.warps[wi];
			graph[gMaps[u.map].name][w.first]++;
			std::string via;
			for (int i : u.viaItems) via += " use-i" + std::to_string(i);
			printf("  %-8s #%-4d %-12s -> %-8s @%-4d %-34s gate at the record:%s %s\n", gMaps[u.map].name.c_str(), u.rec, u.kind.c_str(),
				w.first.c_str(), w.second, u.subject.c_str(), via.c_str(), u.warpGates[wi].c_str());
		}
	printf("\nMAP GRAPH (unique edges):\n");
	for (auto &g : graph)
	{
		printf("  %-8s ->", g.first.c_str());
		for (auto &e : g.second) printf(" %s%s", e.first.c_str(), e.second > 1 ? ("x" + std::to_string(e.second)).c_str() : "");
		printf("\n");
	}
	printf("  maps never warped into:");
	std::set<std::string> into;
	for (auto &g : graph) for (auto &e : g.second) into.insert(e.first);
	for (auto &md : gMaps) if (!into.count(md.name)) printf(" %s", md.name.c_str());
	printf("\n\n");
}

static void PrintMap(int mi)
{
	const MapData &md = gMaps[mi];
	int puzzles = 0;
	for (const Unit &u : gUnits) if (u.map == mi && u.puzzle) puzzles++;
	printf("MAP %s: %zu records, %d units with a gate or a progress effect\n", md.name.c_str(), md.m.commands.size(), puzzles);
	for (const Unit &u : gUnits)
		if (u.map == mi && u.puzzle) PrintUnit(u);
	for (const Unit &u : gUnits) if (u.map == mi && u.unreachable) { printf("  unreachable:"); PrintUnit(u); }
	printf("  other units by kind:");
	std::map<std::string, int> other;
	for (const Unit &u : gUnits) if (u.map == mi && !u.puzzle) other[u.puzzleKind]++;
	for (auto &o : other) printf(" %s %d;", o.first.c_str(), o.second);
	printf("\n\n");
}

//---------------------------------------------------------------------------
// The chapter -> maps assignment, from the cards and the transitions (see docs/CHAPTERS.md for the evidence of each
// row). A map can be visited in more than one chapter; it is listed under the chapter where its puzzles are solved.
//---------------------------------------------------------------------------
struct ChapterMaps { int ch; const char *title; std::vector<const char *> maps; };
// THIS TABLE IS NOT READ FROM THE DATA. Each map is listed once, under the chapter in which it is FIRST played. The
// card locations are data (-chapters); which maps a chapter walks through comes from the map graph (-warps), the map
// contents (-digest) and the walkthrough (docs/CHAPTERS.md, [R: walkthrough]). The house maps STUDY1-4 are a hub
// revisited in many chapters; their units are all counted under the chapter listed here.
static const std::vector<ChapterMaps> kChapterMaps = {
	{ 1, "Shadows", { "STUDY1" } },
	{ 2, "Sign and Portents", { "MAS3", "MAUSO1EA", "MAS4", "MAS6", "MAS7", "MAUSO1EB", "AELF" } },
	{ 3, "Keeper of Time", { "GNARL1" } },
	{ 4, "Tales of the Tower", { "STUDY3", "TOWER1", "STUDY2" } },
	{ 5, "The Offering", { "CAVERNS2", "CAVERNS" } },
	{ 6, "Journal of Evil", { "OPTEMP1", "CAVERNS3" } },
	{ 7, "Beyond the Gate", { "ANUBIS" } },
	{ 8, "The Mark of the Beast", { "STUDY4" } },
	{ 9, "Seven be Bound", { "TEMPLE1" } },
	{ 10, "The Key of Tears", { "TGATE1F", "TGATE1H", "TGATE1I", "RAQUIA4", "RAQUIA1", "RAQUIA2", "RAQUIA3", "RAQUIA5" } },
	{ 11, "Captive Angel", { } },
	{ 12, "St. Michael's", { "CHURCH1", "VICAR", "VICAR1" } },
	{ 13, "Dragon Sword", { "AQUA2", "AQUA1", "LRINTH", "LRINTH1" } },
	{ 14, "Vengeful Souls", { } },
	{ 15, "Where the dead lie", { "GRAVE" } },
	{ 16, "The Gate", { "MAZE", "TGATE1G", "ABAGATE2", "DOMINION", "DOPPLE" } },
	{ 17, "Father of Lies", { "SALVAT", "SOULST3", "ELOHIM1" } },
	{ 18, "Threads", { "SOULST2" } },
	{ 19, "The Gathering", { } },
	{ 20, "Devil take", { } },
};
static int ChapterOfMap(const std::string &name)
{
	for (auto &c : kChapterMaps) for (const char *m : c.maps) if (name == m) return c.ch;
	return 0;
}

static const char *kKinds[] = { "locked door + key", "use item on object", "place / hand over item", "use item (inventory)",
	"use item (inventory) travel", "carry gate", "light (fire item)", "switch / lever", "light switch", "flag-gated use",
	"flag-gated event", "dialogue", "attack / kill", "cutscene", "chapter card", "gated map transition", "map transition",
	"take item (scripted)", "walk-in event", "timer", "door (enabled later)", "door (free)", "flag set", "examine (comment)",
	"comment", "ambient", "unreachable (hidden object)" };

static void PrintKinds()
{
	printf("PUZZLE KINDS per map (a unit = one entry record and its chain; 'P' = has a gate or a progress effect).\n");
	printf("Kinds, in the order of the first matching rule: see Classify() in chapter_census.cpp.\n");
	std::map<int, std::map<std::string, int>> perCh, perChAll;
	std::map<std::string, int> total, totalAll;
	for (int mi = 0; mi < (int)gMaps.size(); mi++)
	{
		std::map<std::string, int> k;
		int p = 0;
		for (const Unit &u : gUnits)
			if (u.map == mi)
			{
				if (u.puzzle) { k[u.puzzleKind]++; p++; total[u.puzzleKind]++; perCh[ChapterOfMap(gMaps[mi].name)][u.puzzleKind]++; }
				totalAll[u.puzzleKind]++;
				perChAll[ChapterOfMap(gMaps[mi].name)][u.puzzleKind]++;
			}
		printf("  %-8s ch%-2d P=%-3d", gMaps[mi].name.c_str(), ChapterOfMap(gMaps[mi].name), p);
		for (auto &e : k) printf(" %s %d;", e.first.c_str(), e.second);
		printf("\n");
	}
	printf("\nPER CHAPTER (units with a gate or progress effect):\n");
	for (auto &c : kChapterMaps)
	{
		int p = 0;
		for (auto &e : perCh[c.ch]) p += e.second;
		const DbCutscene *cs = gCards[c.ch].cut >= 0 ? gDb.Cutscene(gCards[c.ch].cut) : nullptr;
		printf("  ch%-2d %-38s P=%-3d", c.ch, cs ? Text(cs->titleText, 38).c_str() : c.title, p);
		for (const char *kk : kKinds) if (perCh[c.ch].count(kk)) printf(" %s %d;", kk, perCh[c.ch][kk]);
		printf("\n");
	}
	printf("\nTOTALS over all maps (P units / all units / P units whose chain holds a counter or step):\n");
	std::map<std::string, int> seqK;
	for (const Unit &u : gUnits) if (u.puzzle && u.sequence) seqK[u.puzzleKind]++;
	for (const char *kk : kKinds)
		printf("  %-28s %4d / %4d / %3d\n", kk, total.count(kk) ? total[kk] : 0, totalAll.count(kk) ? totalAll[kk] : 0, seqK.count(kk) ? seqK[kk] : 0);
	int ip = 0, ia = 0;
	for (auto &e : total) ip += e.second;
	for (auto &e : totalAll) ia += e.second;
	int sq = 0;
	for (auto &e : seqK) sq += e.second;
	printf("  %-28s %4d / %4d / %3d\n", "ALL", ip, ia, sq);
	std::map<int, int> seqCh;
	for (const Unit &u : gUnits) if (u.puzzle && u.sequence) seqCh[ChapterOfMap(gMaps[u.map].name)]++;
	printf("  counter/step units per chapter:");
	for (auto &e : seqCh) printf(" ch%d %d;", e.first, e.second);
	printf("\n");
	// Combinations live in DBASE100, not in a map.
	int comb = 0, self = 0;
	for (const Recipe &r : gRecipes) (r.b ? comb : self)++;
	printf("  inventory OnUse recipes (DBASE100): %d with a target item (combine), %d on the item itself\n\n", comb, self);
}

static void PrintItems()
{
	printf("ITEMS: id name [cat] | found | tested (RAW 0x27 / DBASE100 count gates via actions) | removed | combines with\n");
	for (int id = 1; id <= gDb.ItemCount(); id++)
	{
		const DbItem *it = gDb.Item(id);
		if (!it) continue;
		const ItemUse &u = gItems[id];
		if (!u.start && u.placed.empty() && u.givenBy.empty() && u.testedBy.empty() && u.removedBy.empty() && u.combineWith.empty() && u.producedFrom.empty())
			continue;
		std::string found;
		if (u.start) found += " start;";
		for (auto &p : u.placed) found += " " + p.first + " pickup" + (p.second > 1 ? "x" + std::to_string(p.second) : "") + ";";
		for (auto &g : u.givenBy) found += " " + g + ";";
		for (int s : u.producedFrom) found += " from i" + std::to_string(s) + ";";
		std::string tested;
		for (auto &t : u.testedBy) tested += " " + t + ";";
		std::string rem;
		for (auto &t : u.removedBy) rem += " " + t + ";";
		printf("  %3d %-24s [%d] | found:%s | tested:%s | removed:%s | with:%s\n", id, ItemName(id).c_str(), it->Category(),
			found.empty() ? " -" : found.c_str(), tested.empty() ? " -" : tested.c_str(), rem.empty() ? " -" : rem.c_str(),
			u.combineWith.empty() ? " -" : (" " + SetStr(u.combineWith, "i")).c_str());
	}
	printf("\nRECIPES (OnUse with a target): item + target -> results (effect action)\n");
	for (const Recipe &r : gRecipes)
	{
		if (!r.b) continue;
		printf("  i%d %s + i%d %s ->", r.a, ItemName(r.a).c_str(), r.b, ItemName(r.b).c_str());
		for (int x : r.results) printf(" i%d %s;", x, ItemName(x).c_str());
		if (r.action) printf(" (act %d:%s)", r.action, Act(r.action).text.c_str());
		printf("\n");
	}
	printf("\n");
}

// The 40 items that have a dossier in docs/ITEM_DESIGN.md (its "## <id> <name>" headings, 2026-11-13).
static const std::set<int> kDossiers = { 15, 16, 1, 68, 150, 175, 4, 113, 156, 33, 223, 6, 14, 35, 60, 97, 127, 131, 28, 37, 49,
	103, 106, 114, 115, 116, 122, 123, 128, 152, 154, 155, 167, 171, 193, 194, 202, 216, 9, 18 };

static bool InMaps(const std::string &where, const std::vector<const char *> &maps)
{
	for (const char *m : maps) if (where.compare(0, strlen(m) + 1, std::string(m) + " ") == 0) return true;
	return false;
}

static void PrintChapterItems()
{
	printf("ITEMS PER CHAPTER: items first found in the chapter's maps (the -kinds table's chapter -> maps), category 2\n"
		"(characters and information topics) listed apart. D = has a dossier in docs/ITEM_DESIGN.md.\n");
	std::set<int> done;
	for (auto &c : kChapterMaps)
	{
		std::vector<int> items, info;
		for (int id = 1; id < (int)gItems.size(); id++)
		{
			if (done.count(id)) continue;
			const ItemUse &u = gItems[id];
			bool here = (c.ch == 1 && u.start);
			for (auto &p : u.placed) for (const char *m : c.maps) if (p.first == m) here = true;
			for (auto &g : u.givenBy) if (InMaps(g, c.maps)) here = true;
			if (!here) continue;
			const DbItem *it = gDb.Item(id);
			if (!it) continue;
			done.insert(id);
			(it->Category() == 2 ? info : items).push_back(id);
		}
		printf("ch%d: %zu items, %zu information records\n", c.ch, items.size(), info.size());
		for (int id : items)
		{
			const ItemUse &u = gItems[id];
			std::string found, used;
			if (u.start) found += " start;";
			for (auto &p : u.placed) found += " " + p.first + " pickup;";
			for (auto &g : u.givenBy) found += " " + g + ";";
			for (int f : u.producedFrom) found += " from i" + std::to_string(f) + ";";
			for (auto &t : u.testedBy) used += " " + t + ";";
			for (auto &t : u.removedBy) if (!u.testedBy.count(t) && !u.testedBy.count(t + "@hand")) used += " " + t + "(removed);";
			for (int w : u.combineWith) used += " +i" + std::to_string(w) + ";";
			if (found.size() > 110) found = found.substr(0, 107) + "...";
			if (used.size() > 140) used = used.substr(0, 137) + "...";
			printf("  %c %3d %-26s [%d] found:%s | used:%s\n", kDossiers.count(id) ? 'D' : ' ', id, ItemName(id).c_str(), gDb.Item(id)->Category(),
				found.c_str(), used.empty() ? " -" : used.c_str());
		}
		if (!info.empty())
		{
			printf("    information:");
			for (int id : info) printf(" %d %s;", id, ItemName(id).c_str());
			printf("\n");
		}
	}
	printf("\n");
}

// Markdown rows, per chapter, of the units that gate progress (docs/CHAPTERS.md section 2 is built from these).
static std::string Does(const Unit &u)
{
	std::string d;
	auto add = [&](const std::string &x) { if (!d.empty()) d += "; "; d += x; };
	if (!u.removes.empty()) add("takes " + SetStr(u.removes, "i"));
	if (u.removeLast) add("takes the item used");
	if (!u.gives.empty()) add("gives " + SetStr(u.gives, "i"));
	if (!u.flagsSet.empty()) add("+flag " + SetStr(u.flagsSet));
	if (u.doors) add("opens " + std::to_string(u.doors) + " door leaf" + (u.doors > 1 ? "s" : ""));
	if (u.geometry) add("moves geometry");
	if (u.spawns) add("spawns/shows " + std::to_string(u.spawns));
	if (!u.toggled.empty()) add("toggles #" + SetStr(u.toggled, ""));
	if (u.sequence) add("counter");
	if (u.delays) add("delay " + std::to_string(u.delayTicks) + "t");
	std::string cuts;
	for (int c : u.cuts) { if (!cuts.empty()) cuts += ","; cuts += Cut(c); }
	if (!cuts.empty()) add("GDV " + cuts);
	for (auto &w : u.warps) add("warp " + w.first + "@" + std::to_string(w.second));
	if (u.choices) add("dialogue choices");
	return d.empty() ? "-" : d;
}
static std::string Needs(const Unit &u)
{
	std::string n;
	auto add = [&](const std::string &x) { if (!n.empty()) n += ", "; n += x; };
	for (int i : u.itemsHand) add("i" + std::to_string(i) + " " + ItemName(i) + " in hand");
	for (int i : u.itemsCarried) add("i" + std::to_string(i) + " " + ItemName(i) + " carried");
	if (!u.itemsList.empty()) add("one of " + SetStr(u.itemsList, "i") + " in hand");
	for (int i : u.viaItems) add("use i" + std::to_string(i) + " " + ItemName(i) + " from the inventory");
	if (!u.flagsNeed.empty()) add("flag " + SetStr(u.flagsNeed));
	if (u.offAtLoad) add("enabled by a 0x17");
	return n.empty() ? "-" : n;
}
static void PrintChapterGates()
{
	static const std::set<std::string> gk = { "locked door + key", "use item on object", "place / hand over item", "use item (inventory)",
		"use item (inventory) travel", "carry gate", "light (fire item)", "chapter card", "gated map transition", "dialogue", "attack / kill",
		"flag-gated use" };
	printf("CHAPTER GATES (markdown): per chapter, the units in its maps whose kind is a gate (%zu kinds).\n", gk.size());
	for (auto &c : kChapterMaps)
	{
		printf("ch%d\n| record | kind | clicked / entered | needs | does |\n|---|---|---|---|---|\n", c.ch);
		for (const char *mn : c.maps)
		{
			auto it = gMapIdx.find(mn);
			if (it == gMapIdx.end()) continue;
			for (const Unit &u : gUnits)
			{
				if (u.map != it->second || !u.puzzle || !gk.count(u.puzzleKind)) continue;
				// Attack units that only count kills are listed once per map, below.
				if (u.puzzleKind == "attack / kill" && u.gives.empty() && u.flagsSet.empty() && u.toggled.size() < 2 && !u.sequence) continue;
				std::string subj = u.kind + " " + u.subject;
				if (subj.size() > 60) subj = subj.substr(0, 57) + "...";
				printf("| %s #%d | %s | %s | %s | %s |\n", mn, u.rec, u.puzzleKind.c_str(), subj.c_str(), Needs(u).c_str(), Does(u).c_str());
			}
		}
		printf("\n");
	}
}

static void PrintTiming()
{
	printf("TIMING AND ORDER: every unit whose chain holds a delay, a counter, a step, a random choice, is a timer, or\n"
		"re-arms through one-shot/toggle -- restricted to units with a gate or a progress effect.\n");
	std::map<std::string, int> tally;
	for (const Unit &u : gUnits)
	{
		if (!u.puzzle) continue;
		std::string why;
		if (u.delays) why += " delay" + std::to_string(u.delays) + "(" + std::to_string(u.delayTicks) + "t" + (u.randomDelays ? ",rnd" : "") + ")";
		if (u.counts) why += " count";
		if (!u.counters.empty()) why += " step";
		if (u.dir == D_TIMER) why += " timer";
		if (u.lastItemTests) why += " last-given";
		if (u.listTests) why += " list";
		if (u.seq.find("random") != std::string::npos) why += " random";
		if (u.oneShot) why += " once";
		if (!u.toggled.empty()) why += " toggles";
		if (why.empty()) continue;
		for (const char *w : { "delay", "count", "step", "timer", "last-given", "list", "random", "once", "toggles" })
			if (why.find(w) != std::string::npos) tally[w]++;
		printf("  %-8s #%-4d %-12s %-22s:%s\n", gMaps[u.map].name.c_str(), u.rec, u.kind.c_str(), u.puzzleKind.c_str(), why.c_str());
	}
	printf("  tally (units):");
	for (auto &t : tally) printf(" %s %d;", t.first.c_str(), t.second);
	printf("\n\n");
}

static void PrintSlice(int ch)
{
	std::set<Fact> goals;
	std::vector<int> ends;
	if (ch == 0) ends = UnitsRunningAction(END_ACTION);
	else ends = CardUnits(ch);
	for (int ui : ends) { std::set<Fact> g = UnitGoal(ui); goals.insert(g.begin(), g.end()); }
	Slice s;
	SliceFrom(goals, s);
	printf("DEPENDENCY SLICE to %s (entries: ", ch ? ("chapter " + std::to_string(ch) + " card").c_str() : "the end (act 614)");
	for (int ui : ends) printf("%s #%d ", gMaps[gUnits[ui].map].name.c_str(), gUnits[ui].rec);
	printf(")\n  needed facts:");
	int nf = 0;
	for (const Fact &f : s.needed) { if (f.kind == F_MAP) continue; printf(" %s;", FactStr(f.kind, f.a, f.b).c_str()); nf++; }
	printf("\n  (%d facts) maps needed:", nf);
	for (const Fact &f : s.needed) if (f.kind == F_MAP) printf(" %s", gMaps[f.a].name.c_str());
	printf("\n  units kept, by map then record:\n");
	for (int ui : s.units)
	{
		const Unit &u = gUnits[ui];
		std::string why;
		for (const Fact &f : s.unitWhy[ui]) why += " " + FactStr(f.kind, f.a, f.b) + ";";
		printf("  %-8s #%-4d %-12s %-34s | %-22s| makes:%s\n", gMaps[u.map].name.c_str(), u.rec, u.kind.c_str(), u.subject.c_str(),
			u.puzzleKind.c_str(), why.c_str());
	}
	printf("  non-map producers kept:\n");
	for (int p : s.prods) if (gProds[p].unit < 0) printf("    %s <- %s\n", FactStr(gProds[p].what.kind, gProds[p].what.a, gProds[p].what.b).c_str(), gProds[p].via.c_str());
	printf("\n");
}

// A digest per map: what is found there, what is played there, where it leads. To recognise a map by its contents.
static void PrintDigest()
{
	printf("MAP DIGEST: items found (pickup or script), GDV movies played by its chains, warps out, and unit counts.\n");
	for (int mi = 0; mi < (int)gMaps.size(); mi++)
	{
		std::set<int> found;
		std::set<std::string> cuts, out;
		int p = 0, all = 0;
		for (int id = 1; id < (int)gItems.size(); id++)
		{
			if (gItems[id].placed.count(gMaps[mi].name)) found.insert(id);
			for (auto &g : gItems[id].givenBy) if (g.compare(0, gMaps[mi].name.size() + 1, gMaps[mi].name + " ") == 0) found.insert(id);
		}
		for (const Unit &u : gUnits)
		{
			if (u.map != mi) continue;
			all++;
			if (u.puzzle) p++;
			for (int c : u.cuts) cuts.insert(Cut(c));
			for (auto &w : u.warps) out.insert(w.first);
		}
		printf("%s (%d units, %d with a gate or progress effect)\n  found:", gMaps[mi].name.c_str(), all, p);
		for (int id : found) printf(" %d %s;", id, ItemName(id).c_str());
		printf("\n  movies:");
		for (auto &c : cuts) printf(" %s", c.c_str());
		printf("\n  to:");
		for (auto &o : out) printf(" %s", o.c_str());
		printf("\n");
	}
	printf("\n");
}

// The gate network: for every unit that tests something, who makes it. This is what unlocks what.
static std::string ProdWhere(int p)
{
	const Prod &pr = gProds[p];
	if (pr.unit < 0) return pr.via;
	const Unit &u = gUnits[pr.unit];
	return gMaps[u.map].name + " #" + std::to_string(u.rec) + (pr.via.find(" act") != std::string::npos ? pr.via.substr(pr.via.find(" act")) : "");
}
static void PrintGates()
{
	printf("GATE NETWORK: each unit with a requirement, and where each requirement is produced (RAW record, pickup, recipe,\n"
		"new game). 'NOWHERE' = no producer found in the data. A requirement met by many producers lists the first 6.\n");
	for (int mi = 0; mi < (int)gMaps.size(); mi++)
		for (int ui = 0; ui < (int)gUnits.size(); ui++)
		{
			const Unit &u = gUnits[ui];
			if (u.map != mi) continue;
			std::set<Fact> req;
			for (int f : u.flagsNeed) req.insert({ F_FLAG, f });
			for (int i : u.itemsHand) req.insert({ F_ITEM, i });
			for (int i : u.itemsCarried) req.insert({ F_ITEM, i });
			for (int i : u.viaItems) req.insert({ F_ITEM, i });
			if (u.offAtLoad) req.insert({ F_REC, u.map, u.rec });
			if (req.empty()) continue;
			printf("  %-8s #%-4d %-12s %-22s needs:", gMaps[mi].name.c_str(), u.rec, u.kind.c_str(), u.puzzleKind.c_str());
			for (const Fact &f : req)
			{
				std::string nm = f.kind == F_ITEM ? " \"" + ItemName(f.a) + "\"" : "";
				printf(" [%s%s <-", FactStr(f.kind, f.a, f.b).c_str(), nm.c_str());
				auto it = gProdsOf.find(f);
				if (it == gProdsOf.end()) printf(" NOWHERE");
				else
				{
					std::set<std::string> where;
					for (int p : it->second) { if (gProds[p].unit == ui) continue; where.insert(ProdWhere(p)); }
					int k = 0;
					for (auto &w : where) { if (k++ < 6) printf(" %s;", w.c_str()); }
					if ((int)where.size() > 6) printf(" +%d", (int)where.size() - 6);
					if (where.empty()) printf(" only itself");
				}
				printf("]");
			}
			printf("\n");
		}
	printf("\n");
}

// Per chapter: what the slice to the NEXT card adds over the slice to this one. The work of a chapter is what must
// be produced between its card and the next.
static void PrintChapterPaths()
{
	printf("CHAPTER PATHS: units in the slice to card n+1 (or the end) that are not in the slice to card n.\n");
	std::vector<Slice> sl(gCards.size() + 1);
	for (int ch = 1; ch <= (int)gCards.size(); ch++)
	{
		std::set<Fact> goals;
		std::vector<int> ends = ch < (int)gCards.size() ? CardUnits(ch) : UnitsRunningAction(END_ACTION);
		for (int ui : ends) { std::set<Fact> g = UnitGoal(ui); goals.insert(g.begin(), g.end()); }
		SliceFrom(goals, sl[ch]);
		for (int ui : ends) sl[ch].units.insert(ui);
	}
	for (int ch = 1; ch < (int)gCards.size(); ch++)
	{
		const Slice &a = sl[ch], &b = sl[ch + 1];
		printf("ch%-2d -> %s: ", ch, ch + 1 < (int)gCards.size() ? ("card " + std::to_string(ch + 1)).c_str() : "end");
		std::vector<int> add;
		for (int ui : b.units) if (!a.units.count(ui)) add.push_back(ui);
		printf("%zu units\n", add.size());
		for (int ui : add)
		{
			const Unit &u = gUnits[ui];
			std::string why;
			auto it = b.unitWhy.find(ui);
			if (it != b.unitWhy.end()) for (const Fact &f : it->second) why += " " + FactStr(f.kind, f.a, f.b) + ";";
			printf("    %-8s #%-4d %-12s %-34s | %-22s| makes:%s\n", gMaps[u.map].name.c_str(), u.rec, u.kind.c_str(), u.subject.c_str(),
				u.puzzleKind.c_str(), why.empty() ? " (the card's own trigger)" : why.c_str());
		}
	}
	printf("\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: chapter_census <ROTH DATA folder> [-chapters|-warps|-map NAME|-maps|-path [N]|-chpaths|-items|-kinds|-timing|-all]\n"); return 2; }
	const std::string mode = argc > 2 ? argv[2] : "-all";
	if (!TheInstall().Open(argv[1])) { printf("install: %s\n", TheInstall().Error().c_str()); return 2; }
	Log log;
	if (!LoadDbase100FromInstall(TheInstall(), gDb, &log)) { printf("DBASE100 not loaded\n"); return 2; }
	gText = ReadAll(TheInstall().DataFile("DBASE400.DAT"));

	// Chapter cards, by name.
	gCards.assign(21, {});
	for (int ch = 1; ch <= 20; ch++) gCards[ch].chapter = ch;
	for (int i = 0; i < gDb.CutsceneCount(); i++)
	{
		const DbCutscene *c = gDb.Cutscene(i);
		if (!c || c->name.compare(0, 5, "CHPTR") != 0) continue;
		const int ch = atoi(c->name.c_str() + 5);
		if (ch >= 1 && ch <= 20) gCards[ch].cut = i;
	}
	for (int id = 1; id <= gDb.ActionCount(); id++)
	{
		const DbAction *a = gDb.Action(id);
		if (!a) continue;
		for (const DbCommand &c : gDb.Commands(*a))
			if (c.opcode == 0x07)
				for (int ch = 1; ch <= 20; ch++) if (gCards[ch].cut == (int)c.operand - 1) gCards[ch].actions.insert(id);
	}

	for (const std::string &name : TheInstall().MapNames())
	{
		MapData md;
		if (!LoadMap(name, md)) { printf("map %s: not loaded\n", name.c_str()); continue; }
		gMapIdx[name] = (int)gMaps.size();
		gMaps.push_back(std::move(md));
	}
	{
		std::vector<std::string> names;
		std::vector<Map *> ms;
		for (MapData &md : gMaps) { names.push_back(md.name); ms.push_back(&md.m); }
		BuildCallers(names, ms);
	}
	BuildUnits();
	BuildItems();
	for (Unit &u : gUnits) Classify(u);
	for (int p = 0; p < (int)gProds.size(); p++) gProdsOf[gProds[p].what].push_back(p);

	const bool all = mode == "-all";
	if (all)
	{
		printf("chapter_census: %zu maps, %zu units (entry records), %d DBASE100 actions, %d items, %d cutscenes\n\n", gMaps.size(),
			gUnits.size(), gDb.ActionCount(), gDb.ItemCount(), gDb.CutsceneCount());
	}
	if (all || mode == "-chapters") PrintChapters();
	if (all || mode == "-warps") PrintWarps();
	if (mode == "-chpaths") PrintChapterPaths();
	if (all || mode == "-digest") PrintDigest();
	if (all || mode == "-gates") PrintGates();
	if (all || mode == "-kinds") PrintKinds();
	if (all || mode == "-items") PrintItems();
	if (all || mode == "-chitems") PrintChapterItems();
	if (all || mode == "-chgates") PrintChapterGates();
	if (all || mode == "-timing") PrintTiming();
	if (mode == "-path") PrintSlice(argc > 3 ? atoi(argv[3]) : 0);
	if (all || mode == "-maps") for (int mi = 0; mi < (int)gMaps.size(); mi++) PrintMap(mi);
	if (mode == "-map" && argc > 3)
	{
		auto it = gMapIdx.find(argv[3]);
		if (it == gMapIdx.end()) { printf("no map %s\n", argv[3]); return 2; }
		PrintMap(it->second);
	}
	return 0;
}
