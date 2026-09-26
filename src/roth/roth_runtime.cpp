//
// Running Realms' level logic against the live world.
//
// Everything here is transcribed from ROTH.C and cited. Where a handler is not
// implemented yet it returns CMD_NOTHING and is COUNTED, so an unhandled opcode
// shows up in the load report rather than silently doing nothing.
//

#include "roth_runtime.h"
#include "roth_commands.h"
#include "roth_raw.h"
#include "roth_log.h"

#include <map>
#include <vector>

#include "g_levellocals.h"
#include "p_spec.h"
#include "playsim/po_man.h"
#include "printf.h"

namespace roth
{

namespace
{

struct Runtime
{
	bool active = false;
	Map map;                       // the level's own copy of its logic
	FLevelLocals *level = nullptr;

	// Trigger bindings, built once at load. A face or sector can carry several.
	std::map<int, std::vector<uint16_t>> byFace;     // Realms face  -> chain starts
	std::map<int, std::vector<uint16_t>> bySector;   // Realms sector-> chain starts

	// Doorway sector -> the polyobject tag of the panel that fills it.
	std::map<int, int> doorTag;
	// Realms face -> engine sidedef, so a trigger's face can be found from a side.
	std::map<int, int> faceToSide;
	std::map<int, std::vector<uint16_t>> bySide;   // built from the two above

	// What ran and what could not, for the report and for the console.
	int fired = 0, unhandled = 0;
	std::map<uint8_t, int> unhandledOps;
};

Runtime g;

// Opcodes that watch for the player USING a wall, and for ENTERING a sector.
// From the original's load-time registration table (map_load.c:1089-1097) and
// ROTH_COMMANDS.md.
bool IsUseWall(uint8_t op)   { return op == 0x19 || op == 0x31 || op == 0x1A; }
bool IsEnterSector(uint8_t op) { return op == 0x18 || op == 0x32; }

//==========================================================================
//
// cmd_open_door -- raw_commands.c:4052, and the swing itself from doors.c
//
// The original resolves the key to a FACE, then registers a swing. What the
// record's operands actually mean was read out of the door tick rather than
// assumed, and two of them are NOT what this repo previously believed:
//
//   +0x0a x +0x07   the open DWELL -- how long it stays open before closing
//                   (doors.c:1049-1055 counts word[rec+8] down to it)
//   +0x0c           the SPEED multiplier for the angle step (doors.c:1042)
//   +0x0e / +0x10   SOUND ids, plus one; 0 means silent. They are not a target
//                   point -- their only readers play them on close
//                   (doors.c:1057-1063, 1142-1145)
//
// The arc is a FIXED 90 degrees: the angle byte is clamped to +/-0x40
// (doors.c:1082, 1108) and the table it drives is 256 steps to a turn.
//
//==========================================================================

int OpenDoor(const Command &c)
{
	if (g.level == nullptr) return CMD_NOTHING;

	// The key names a face; the door is the sector that face belongs to.
	int rothSector = -1;
	for (int fi : c.faces)
	{
		if (fi < 0 || fi >= (int)g.map.faces.size()) continue;
		const int sec = g.map.faces[fi].sector;
		if (sec >= 0 && g.doorTag.count(sec)) { rothSector = sec; break; }
		// The face may be the room's side of the doorway; try its sister.
		const int sis = g.map.faces[fi].sister;
		if (sis >= 0 && sis < (int)g.map.faces.size())
		{
			const int ss = g.map.faces[sis].sector;
			if (ss >= 0 && g.doorTag.count(ss)) { rothSector = ss; break; }
		}
	}
	if (rothSector < 0) return CMD_NOTHING;

	const int tag = g.doorTag[rothSector];

	// Speed: the original multiplies the frame delta by byte[rec+0x0c], or
	// clamps the delta to 8 when that is zero. GZDoom's swing speed is in its
	// own units, so the multiplier is carried across proportionally and the
	// zero case takes the same floor the original does.
	const int speedMul = c.args.size() > 3 ? (int)(c.args[3] & 0xFF) : 0;
	const double speed = speedMul > 0 ? double(speedMul) : 8.0;

	// Dwell, in tics. word[rec+0x0a] times byte[rec+7] when that is non-zero.
	const int dwellRaw = c.aux;
	const int dwellMul = c.subFlags != 0 ? c.subFlags : 1;
	const int delay = dwellRaw > 0 ? dwellRaw * dwellMul : 0;

	// The arc is always 90 degrees -- see the note above.
	return EV_OpenPolyDoor(g.level, nullptr, tag, speed, DAngle::fromDeg(90.),
		delay, 0., PODOOR_SWING) ? CMD_ACTED : CMD_NOTHING;
}

//==========================================================================
//
// The handler table. Only the opcodes that are implemented appear; everything
// else is counted by opcode so the gap is visible and measurable rather than
// silent.
//
//==========================================================================

int RunCommand(const Map &, const Command &c, int)
{
	switch (c.opcode & 0x7f)      // the 0x80 bit is editor metadata (raw_commands.c:1256)
	{
	case 0x2f: return OpenDoor(c);

	default:
		g.unhandled++;
		g.unhandledOps[c.opcode & 0x7f]++;
		return CMD_NOTHING;
	}
}

// Walk a chain: the flow pre-pass first, exactly as the original does, then the
// action pass. A gated chain does nothing this time round.
bool Fire(uint16_t chain)
{
	if (chain == 0) return false;
	if (WalkChainFlow(g.map, chain) != 0) return false;

	Handlers h;
	h.Run = RunCommand;
	const int result = ExecChain(g.map, chain, h);
	if (result != CMD_NOTHING) g.fired++;
	return result != CMD_NOTHING;
}

} // namespace

//==========================================================================

void BeginLevel(const Map &map, FLevelLocals *level, Log *log)
{
	// NOT EndLevel() here: the loader registers its face-to-sidedef pairing and
	// its door tags on the way past, BEFORE this is called, and clearing them
	// now would throw away the very bindings this function needs. The reset
	// happens at the START of a load instead.
	g.map = map;
	g.level = level;
	g.active = true;

	// Bind every trigger to the geometry it watches. The reader has already
	// resolved each key to a sector and/or a set of faces; this only sorts them
	// by what kind of event they wait for.
	int useWall = 0, enterSector = 0, unbound = 0;
	for (const Command &c : g.map.commands)
	{
		if (!c.isTrigger || c.disabled) continue;
		if (c.chainStart == 0) continue;

		if (IsUseWall(c.opcode))
		{
			if (c.faces.empty()) { unbound++; continue; }
			for (int fi : c.faces) g.byFace[fi].push_back(c.chainStart);
			useWall++;
		}
		else if (IsEnterSector(c.opcode))
		{
			if (c.sector < 0) { unbound++; continue; }
			g.bySector[c.sector].push_back(c.chainStart);
			enterSector++;
		}
	}

	// Translate the face bindings into sidedef bindings now, so activation is a
	// single lookup rather than a search.
	for (auto &kv : g.byFace)
	{
		auto it = g.faceToSide.find(kv.first);
		if (it == g.faceToSide.end()) continue;
		auto &dst = g.bySide[it->second];
		dst.insert(dst.end(), kv.second.begin(), kv.second.end());
	}

	if (log)
	{
		log->Section("Level logic");
		log->Line("  triggers bound   %d use-wall, %d enter-sector", useWall, enterSector);
		log->Line("  doors reachable  %d   walls wired %d", (int)g.doorTag.size(), (int)g.bySide.size());
		log->Count("logic: triggers whose key named no geometry", unbound);
	}
}

void EndLevel()
{
	g.active = false;
	g.level = nullptr;
	g.byFace.clear();
	g.bySide.clear();
	g.faceToSide.clear();
	g.bySector.clear();
	g.doorTag.clear();
	g.map = Map();
	g.fired = g.unhandled = 0;
	g.unhandledOps.clear();
}

void RegisterDoor(int rothSector, int polyTag)
{
	g.doorTag[rothSector] = polyTag;
}

void RegisterFaceSide(int rothFace, int sideIndex)
{
	g.faceToSide[rothFace] = sideIndex;
}

bool ActivateLine(line_t *line, AActor *who, int side)
{
	if (!g.active || line == nullptr) return false;

	// A Doom line carries no Realms face index, so the binding is by the SIDE's
	// own index, which the loader assigns in face order.
	bool any = false;
	for (int s = 0; s < 2; s++)
	{
		side_t *sd = line->sidedef[s];
		if (sd == nullptr) continue;
		const int idx = (int)(sd - &g.level->sides[0]);
		auto it = g.bySide.find(idx);
		if (it == g.bySide.end()) continue;
		for (uint16_t chain : it->second) any |= Fire(chain);
	}
	(void)who; (void)side;
	return any;
}

void CrossSector(sector_t *sec, AActor *who)
{
	if (!g.active || sec == nullptr) return;
	auto it = g.bySector.find(sec->sectornum);
	if (it == g.bySector.end()) return;
	for (uint16_t chain : it->second) Fire(chain);
	(void)who;
}

} // namespace roth
