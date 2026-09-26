//
// Running Realms' level logic against the live world.
//
// Everything here is transcribed from ROTH.C and cited. Where a handler is not
// implemented it returns CMD_NOTHING and is COUNTED WITH A REASON, so an
// unhandled opcode shows up in the load report rather than silently doing
// nothing. Nothing here approximates a mechanism it could not read.
//
// THE SHAPE OF THE ORIGINAL, which decides what could be built here at all:
// the 0x30780 dispatch table (platform/boot.c:189-205) holds three kinds of
// entry, and only the first is a handler in the everyday sense.
//
//   IMMEDIATE      does the thing and returns. Implemented below.
//   VERIFIED NOP   the table slot IS cmd_default_nop (0x30ab0, raw_commands.c:
//                  5379, `sub eax,eax; ret`). Nothing to build: these records
//                  provably do nothing when dispatched, and 1,943 of the retail
//                  5,531 are one of them. Counted separately from a real gap.
//   REGISTRAR      allocates an ACTIVE EFFECT record and returns; the visible
//                  behaviour is in a per-frame TICK handler reached through a
//                  SECOND table (0x3088c, boot.c:215-229). Every light, texture,
//                  height and sector-move opcode is one of these, so they need
//                  the effect pool and its tick -- a separate piece of work, not
//                  a function each. Counted as unimplemented.
//

#include "roth_runtime.h"
#include "roth_commands.h"
#include "roth_raw.h"
#include "roth_log.h"

#include <map>
#include <vector>
#include <string>
#include <string.h>

#include "g_levellocals.h"
#include "g_level.h"
#include "gamedata/g_mapinfo.h"
#include "p_spec.h"
#include "playsim/p_local.h"
#include "playsim/po_man.h"
#include "printf.h"

namespace roth
{

namespace
{

//==========================================================================
//
// Progress flags -- global, persistent, outliving the level.
//
// cmd_set_flag and cmd_if_not_flag operate on a bitmap allocated ONCE at game
// start, one bit per DBASE100 record: ((count + 0x20) & ~0x1F) >> 3 bytes. The
// retail data has 433 records, so 56 bytes / 448 bits (ROTH_COMMANDS.md, "flags,
// items and dialogue all live outside the map"). Savegame chunk 6 writes it out
// whole, which is why progress survives a level change -- the game is one
// continuous story and the flags are its only memory.
//
// The bit arithmetic is dbase100_bitmap_test_set (renderer.c:11528) and
// test_dbase100_record_flag (raw_commands.c:80): the byte index is an ARITHMETIC
// id >> 3, and the mask is g_bit_mask_lut[id & 7], whose eight bytes are
// { 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80 } (data/obj3_owned.c:480 and 483) -- so
// plainly 1 << (id & 7).
//
// SIZED FROM THE RETAIL DATA, NOT FROM DBASE100.DAT: the loader does not read
// DBASE100 yet, so the record count the original derives this from is not
// available here. 448 is what the retail 433 records yield, and the flag ids
// actually observed across the 44 maps run 1..431.
//
//==========================================================================

static const int FLAG_BITS = 448;
uint8_t g_progressFlags[FLAG_BITS / 8];

bool FlagIsSet(uint16_t id)
{
	const int byteIndex = (int)id >> 3;
	if (byteIndex < 0 || byteIndex >= (int)sizeof(g_progressFlags)) return false;
	return (g_progressFlags[byteIndex] & (1u << (id & 7))) != 0;
}

// test_set / test_clear report "acted" only when the bit actually CHANGED.
bool FlagTestSet(uint16_t id)
{
	const int byteIndex = (int)id >> 3;
	if (byteIndex < 0 || byteIndex >= (int)sizeof(g_progressFlags)) return false;
	const uint8_t mask = (uint8_t)(1u << (id & 7));
	if (g_progressFlags[byteIndex] & mask) return false;
	g_progressFlags[byteIndex] |= mask;
	return true;
}

bool FlagTestClear(uint16_t id)
{
	const int byteIndex = (int)id >> 3;
	if (byteIndex < 0 || byteIndex >= (int)sizeof(g_progressFlags)) return false;
	const uint8_t mask = (uint8_t)(1u << (id & 7));
	if (!(g_progressFlags[byteIndex] & mask)) return false;
	g_progressFlags[byteIndex] &= (uint8_t)~mask;
	return true;
}

//==========================================================================

// A delay in flight. cmd_delay_timer allocates one of these (raw_commands.c:4420)
// and tick_delay_timer counts it down (raw_commands.c:5328).
struct PendingDelay
{
	int32_t  remaining = 0;   // chunk[6], in Realms frame ticks
	uint16_t record = 0;      // chunk[8], the delay record's own 1-based index
	uint32_t context = 0;     // chunk[0xc], "what the player just used", +1 biased
};

// An entry of the double-buffered deferred-command queue
// (process_deferred_command_queue, raw_commands.c:1587).
struct Deferred
{
	uint16_t chain = 0;       // entry.dword0, a 1-based command index
	uint32_t context = 0;     // entry.dword1
};

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
	std::map<int, int> sideToFace;                 // and back again
	std::map<int, std::vector<uint16_t>> bySide;   // built from the two above

	// "Whatever the player just used" -- the original's g_active_object /
	// g_active_object_secondary (ROTH_COMMANDS.md, "key = 0 means the thing the
	// player just used"). A key of 0 routes here instead of naming geometry,
	// which is how ONE command record serves many doors.
	int activeFace = -1;
	int activeSector = -1;

	// The 0x36 latch: g_pending_command_record (0x8a0dc), held as a 1-based
	// index here rather than the original's record pointer.
	uint16_t pendingRecord = 0;

	// cmd_delay_timer's countdowns, and the queue their expiry feeds.
	std::vector<PendingDelay> delays;
	std::vector<Deferred> deferred;

	// cmd_map_transition's latch: g_warp_dest_a/b + g_map_first_load_flag.
	std::string warpMap;
	uint16_t warpArrival = 0;
	bool warpPending = false;

	// An arrival point waiting for a player to exist to apply it to.
	int pendingArrivalSector = -1;

	// g_command_rng (0x71f48). The seed is ROTH.EXE's own initialiser,
	// data/obj3_owned.c:761.
	uint32_t rng = 0xcc61;

	// What ran and what could not, for the report and for the console.
	int fired = 0;
	std::map<uint8_t, int> unhandledOps;
};

Runtime g;

// Carried ACROSS a level change, because the arrival point is latched in the map
// the player is leaving and applied in the one they arrive in.
std::string g_warpArrivalMapName;
int g_warpArrivalSector = -1;

// Opcodes that watch for the player USING a wall, and for ENTERING a sector.
// From the original's load-time registration table (map_load.c:1089-1097) and
// ROTH_COMMANDS.md.
bool IsUseWall(uint8_t op)   { return op == 0x19 || op == 0x31 || op == 0x1A; }
bool IsEnterSector(uint8_t op) { return op == 0x18 || op == 0x32; }

// A mutable record by 1-based index -- resolve_command_by_index (renderer.c:9830).
// MUTABLE on purpose: command records are game STATE in the original, not read-
// only data. 0x17 writes another record's modifier byte, cmd_delay_timer writes
// its own, and the savegame stores the whole section back out.
Command *Rec(uint16_t index)
{
	if (index == 0) return nullptr;
	const size_t i = (size_t)(index - 1);
	if (i >= g.map.commands.size()) return nullptr;
	return &g.map.commands[i];
}

// Keep Command::disabled in step with the modifier byte it mirrors. The spine
// tests the byte, but BeginLevel's trigger scan reads the cached bool.
void SyncDisabled(Command *c) { c->disabled = (c->modifier & 0x08) != 0; }

//==========================================================================
//
// cmd_open_door -- raw_commands.c:4053, and the swing itself from doors.c
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

	// Which faces to try. A key of 0 is NOT inert: cmd_open_door's own key==0
	// paths go to g_active_object_secondary and then g_active_object
	// (raw_commands.c:4045-4051), i.e. the wall the player just used. FIFTY-FIVE
	// of the game's 116 door commands carry key == 0 (ROTH_COMMANDS.md), so
	// without this the majority of doors would have no target at all.
	std::vector<int> candidates;
	if (c.key != 0)
		candidates = c.faces;
	else if (g.activeFace >= 0)
		candidates.push_back(g.activeFace);

	// The key names a face; the door is the sector that face belongs to.
	int rothSector = -1;
	for (int fi : candidates)
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
// The two shared continue-tails of the conditional cluster (0x27/0x28/0x29/0x2a).
//
// rawcmd_tail_consume  -- raw_commands.c:4637 (0x355ba / 0x355d1)
// rawcmd_tail_ifnext   -- raw_commands.c:4648 (0x355ef)
//
//==========================================================================

int RunDbase100Record(Command *rec);

int TailConsume(Command *rec, int ret)
{
	if (rec->fireFlags & 0x10) { rec->modifier |= 0x08; SyncDisabled(rec); }
	g.pendingRecord = 0;
	return ret;
}

int TailIfNext(Handlers &h)
{
	uint16_t pending = g.pendingRecord;
	if (pending != 0)
	{
		// g_item_autoselected_flag gates this in the original. It belongs to the
		// inventory subsystem, which is not built, and is therefore always 0
		// here -- the ungated path.
		Command *p = Rec(pending);
		if (p != nullptr) RunDbase100Record(p);
		pending = 0;
		g.pendingRecord = 0;
	}
	h.interrupt = Interrupt::Rerun;          // g_command_chain_interrupt = 2
	// The original returns the pending RECORD POINTER here, and the exec loop
	// only ORs it into the chain result, whose low bit picks the post-chain
	// sound. With no pointer to return, a consumed pending record reports
	// nothing and a latched one reports acted. UNVERIFIED in the one place it
	// could matter -- the post-chain SFX, which is not implemented either.
	return pending != 0 ? CMD_ACTED : CMD_NOTHING;
}

//==========================================================================
//
// 0x2b run_command_dbase100_record -- raw_commands.c:4697
//
// Fires a DBASE100 dialogue record. The latch is transcribed; the record itself
// is evaluated by eval_dialogue_record_by_id (0x1dc73), the DBASE100 dialogue
// interpreter, WHICH IS NOT BUILT. The original's own early-out for an unloaded
// DBASE100 base returns 0, which is exactly the state this is in, so this
// reproduces that path rather than inventing one -- but no dialogue will play
// until DBASE100 is read. Counted, so it does not look like a success.
//
//==========================================================================

int RunDbase100Record(Command *rec)
{
	(void)rec;
	g.unhandledOps[0x2B]++;
	return CMD_NOTHING;                      // eval_dialogue_record_by_id -> 0
}

//==========================================================================
//
// 0x26 cmd_set_flag -- raw_commands.c:592
//
// flags(byte[rec+6]) & 0x02 = TOGGLE (test current: clear -> set, set -> clear);
// else & 0x01 = CLEAR; else SET. Reports "acted" ONLY when the bit changed, and
// then runs the shared consume-tail.
//
//==========================================================================

int SetFlag(Command *rec)
{
	const uint16_t id = rec->key;
	const uint8_t flags = rec->fireFlags;
	bool doSet;
	if (flags & 0x02)      doSet = !FlagIsSet(id);
	else if (flags & 0x01) doSet = false;
	else                   doSet = true;

	const bool changed = doSet ? FlagTestSet(id) : FlagTestClear(id);
	if (!changed) return CMD_NOTHING;
	return TailConsume(rec, CMD_ACTED);
}

//==========================================================================
//
// 0x28 cmd_if_not_flag -- raw_commands.c:4665
//
// Query the flag, then route on the result and byte[rec+6] & 1 into the two
// shared tails. test_dbase100_record_flag (raw_commands.c:80) returns -1 when
// the bit is SET *or when there is no bitmap at all*; there is always a bitmap
// here, so only the real test applies.
//
//==========================================================================

int IfNotFlag(Command *rec, Handlers &h)
{
	const bool set = FlagIsSet(rec->key);
	const int f1 = rec->fireFlags & 1;
	if (set) return f1 ? TailConsume(rec, CMD_NOTHING) : TailIfNext(h);
	return f1 ? TailIfNext(h) : TailConsume(rec, CMD_ACTED);
}

//==========================================================================
//
// 0x17 cmd_toggle_command -- raw_commands.c:541
//
// Resolve the target by 1-based index word[rec+8]; flip a bit of its modifier
// byte -- 0x02 when the target's base byte is 0x06 or 0x2c, else 0x08, the
// disable bit. Then if this record's flag byte has 0x06 set, FORCE the bit: set
// it, and clear it instead when 0x02 is also set. Finally, unless flag 0x20,
// disable THIS record so it fires once.
//
// The single most-used instruction in the game: 400 records, 56 in STUDY1. It is
// pure command-record state, which is why it can be exact.
//
//==========================================================================

int ToggleCommand(Command *rec)
{
	Command *tgt = Rec(rec->key);
	if (tgt == nullptr) return CMD_NOTHING;

	const uint8_t bit = (tgt->opcode == 0x06 || tgt->opcode == 0x2C) ? 0x02 : 0x08;
	tgt->modifier ^= bit;
	const uint8_t fl = rec->fireFlags;
	if (fl & 0x06)
	{
		tgt->modifier |= bit;
		if (fl & 0x02) tgt->modifier &= (uint8_t)~bit;
	}
	SyncDisabled(tgt);

	if (!(rec->fireFlags & 0x20)) { rec->modifier |= 0x08; SyncDisabled(rec); }
	return CMD_ACTED;
}

//==========================================================================
//
// 0x38 cmd_jump_if_next_fails -- raw_commands.c:41
//
// `g_command_next_active = (u16)word[rec+8]; return 0`. That is the whole
// handler: the JUMP itself is the exec loop's business, which re-runs the chain
// from the latched index when a later handler sets the interrupt to 2. The spine
// already carries both halves -- Handlers::nextActive and Interrupt::Rerun --
// so this only has to latch.
//
//==========================================================================

int JumpIfNextFails(Command *rec, Handlers &h)
{
	h.nextActive = rec->key;
	return CMD_NOTHING;
}

//==========================================================================
//
// 0x36 cmd_dbase100_if_next_fails -- raw_commands.c:514
//
// Latch this record as the pending one and return 0. TailIfNext above is what
// consumes it.
//
//==========================================================================

int Dbase100IfNextFails(int index)
{
	g.pendingRecord = (uint16_t)index;
	return CMD_NOTHING;
}

//==========================================================================
//
// 0x12 cmd_delay_timer -- raw_commands.c:4420, with tick_delay_timer at 5328
//
// EXECUTE: guard on the modifier (`!(m & 4) && (m & 0x21)` -> already armed),
// then arm a countdown. The delay is the WORD AT +6 -- for this opcode alone
// that word is the operand and not a pair of flag bytes, and every one of the
// 218 retail records is exactly 8 bytes long with nothing after it. With
// modifier bit 0x02 the delay is randomised through the command RNG.
//
// The chain always STOPS here (the spine already does that, from the exec loop's
// own `if base == 0x12 break`). What RESUMES it is the tick: on expiry the delay
// record's NEXT index is pushed onto the deferred queue, and the queue is drained
// a moment later by running that chain from the top.
//
// THE ONE THING NOT READ FROM ROTH.C IS THE UNIT. The countdown is decremented
// by g_frame_time_scale, a PIT tick delta, and the PIT divisor lives in the
// hardware seam ROTH.C replaces: dos_runtime.c:278 says "keeping the 120 Hz
// divisor" and the very next line quotes a ratio that does not agree with it, and
// no `out 0x43/0x40` value survives anywhere in the tree. So the file does not
// settle it. ONE REALMS TICK IS TREATED AS ONE GZDOOM TIC BELOW, WHICH IS
// UNVERIFIED: if the rate really is 120 Hz then the retail delays (30, 60, 120
// and 300 are the common values, which read as authored against a round rate)
// will run about 3.4x too slow at GZDoom's 35 tics per second. It is one named
// constant and an OPEN QUESTION, not a number tuned until it looked right.
//
//==========================================================================

// UNVERIFIED -- see above. Realms frame ticks consumed per GZDoom world tic.
static const int ROTH_FRAME_TICKS_PER_TIC = 1;

int DelayTimer(Command *rec, int index)
{
	const uint8_t m = rec->modifier;
	if (!(m & 0x04) && (m & 0x21)) return CMD_NOTHING;      // already armed

	uint32_t d = rec->args.empty() ? 0u : (uint32_t)rec->args[0];
	if (m & 0x02)
	{
		// The command RNG, exactly: seed = seed * 0x5e5 + 0x29, then scale.
		g.rng = g.rng * 0x5e5u + 0x29u;
		d = (d * (g.rng & 0xffffu)) >> 16;
		d += 4u;
	}

	PendingDelay pd;
	pd.remaining = (int32_t)d;
	pd.record = (uint16_t)index;
	// chunk[0xc] carries "what the player just used" across the wait, so a key
	// of 0 downstream of the delay still names the right thing.
	pd.context = (uint32_t)(g.activeFace + 1);
	g.delays.push_back(pd);

	if (!(rec->modifier & 0x04)) { rec->modifier |= 0x20; SyncDisabled(rec); }
	return CMD_ACTED;
}

//==========================================================================
//
// 0x3b cmd_map_transition -- raw_commands.c:525
//
// Latches the pending level change and returns. The destination is an 8-byte
// NUL-PADDED ASCII map name in the two dwords at +0x0a and +0x0e, and the key at
// +0x08 is the ARRIVAL POINT. Verified by decoding every 0x3b record in the
// retail maps: AELF -> "STUDY1", ANUBIS -> "CHURCH1", AQUA1 -> "LRINTH1", and
// CHURCH1's "V I C A R \0 1 \0" with its stray digit behind the terminator,
// which is why this is NUL-terminated and not fixed-width. A few records carry
// an all-NUL name; those name no destination and do nothing.
//
// The arrival point is a SECTOR COMMAND ID, not a player-start number:
// process_map_warp_or_load (map_load.c:615) calls relocate_player_to_warp_sector,
// which scans the sector section for `word[sec+0x14] == target` and centres the
// player on that sector's vertex bounding box (ml_relocate_core / _found). Same
// destination name as the current map -> relocate in place, no reload. Both are
// reproduced.
//
//==========================================================================

int MapTransition(Command *rec)
{
	// args[2..5] are the bytes +0x0a..+0x11, little-endian per word.
	char name[9] = {};
	if (rec->args.size() >= 6)
	{
		for (int i = 0; i < 4; i++)
		{
			name[i * 2 + 0] = (char)(rec->args[2 + i] & 0xFF);
			name[i * 2 + 1] = (char)(rec->args[2 + i] >> 8);
		}
	}
	name[8] = 0;
	size_t len = 0;                          // NUL-terminated, not fixed-width
	while (len < 8 && name[len] != 0) len++;
	name[len] = 0;
	if (len == 0) return CMD_NOTHING;

	g.warpMap = name;
	g.warpArrival = rec->key;
	g.warpPending = true;

	if (rec->fireFlags & 0x10) { rec->modifier |= 0x08; SyncDisabled(rec); }
	return CMD_ACTED;
}

//==========================================================================

int RunCommand(const Map &m, const Command &cc, int index);

//==========================================================================
//
// 0x40 cmd_run_indexed_object_command -- raw_commands.c:3955
//
// A SECOND copy of the executor inner loop, running a sub-chain from the index in
// WORD[REC+6] -- not the key at +8. Confirmed both ways: the code reads +6, and
// every one of the 76 retail records is 8 bytes long, so there IS no +8 to read.
// (ROTH_COMMANDS.md lists this opcode's index at +8 alongside 0x17 and 0x38; for
// 0x40 that is wrong. 0x17 and 0x38 are right -- 26 of 26 and 397 of 400 land in
// range as an index there.)
//
// Deliberately not ExecChain: this loop does NOT touch prev/next-active, has no
// done-tail, does not reset the interrupt, and returns the LAST dispatched
// handler's result rather than the accumulated OR.
//
//==========================================================================

int RunIndexedCommand(const Command &rec, Handlers &h)
{
	uint16_t ax = rec.args.empty() ? 0 : rec.args[0];
	if (ax == 0) return CMD_NOTHING;

	int last = CMD_NOTHING;
	// The original is unbounded and relies on the data being sane; a malformed
	// chain would spin forever here, and a bounded bail is the lesser failure.
	for (int guard = 0; guard < 4096; guard++)
	{
		if (ax == 0) break;
		Command *r = Rec(ax);
		if (r == nullptr) break;

		if (!(r->modifier & 0x08))
		{
			last = RunCommand(g.map, *r, (int)ax);
			if (h.interrupt != Interrupt::None) break;
		}
		if (r->opcode == 0x12) break;                  // a delay stops it
		if (r->linkIndex == 0) break;
		ax = r->linkIndex;
	}
	return last;
}

//==========================================================================
//
// The handler table.
//
// The three outcomes are kept apart on purpose. A VERIFIED NOP is not a gap: its
// slot in the original's dispatch table IS cmd_default_nop, so the record
// provably does nothing, and lumping those in with the real gaps would make the
// unhandled count meaningless. The nop list is read straight off the 0x30780
// table at platform/boot.c:189-205 -- every index whose entry is 0x30ab0, across
// the opcode range the retail data actually uses (highest base observed: 0x42).
//
//==========================================================================

bool IsVerifiedNop(uint8_t op)
{
	switch (op)
	{
	// Reserved / unused slots. 0x01 (62 retail records) and 0x37 (4) are real
	// opcodes in the data whose table entry is cmd_default_nop.
	case 0x00: case 0x01: case 0x04: case 0x05:
	case 0x2C: case 0x37: case 0x43: case 0x44: case 0x47:
	// TRIGGERS. They are registered at load and their INSTRUCTION slot is the
	// nop, so a trigger record reached mid-chain is inert rather than wrong.
	case 0x08: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B:
	case 0x25: case 0x30: case 0x31: case 0x32: case 0x39: case 0x3D:
		return true;
	default:
		return false;
	}
}

// Which opcodes this file implements. One list, used by both the dispatcher's
// census and the load report, so the two can never disagree.
bool IsImplemented(uint8_t op)
{
	switch (op)
	{
	case 0x06: case 0x12: case 0x17: case 0x26: case 0x28:
	case 0x2F: case 0x36: case 0x38: case 0x3B: case 0x3E: case 0x40:
		return true;
	default:
		return false;
	}
}

int RunCommand(const Map &, const Command &cc, int index)
{
	// The 0x80 bit is masked off by the exec loop (raw_commands.c:1256) as editor
	// metadata. MEASURED: it is never set on an opcode byte in any of the 44
	// retail maps -- 0 of 5,531 records -- so the mask is a no-op in practice.
	// (ROTH_COMMANDS.md's "a third of all records carry it" is about the MODIFIER
	// byte at +0x02, not the opcode.)
	const uint8_t op = cc.opcode & 0x7f;

	Command *rec = Rec((uint16_t)index);
	if (rec == nullptr) return CMD_NOTHING;

	switch (op)
	{
	case 0x2F: return OpenDoor(*rec);
	case 0x26: return SetFlag(rec);
	case 0x17: return ToggleCommand(rec);
	case 0x3B: return MapTransition(rec);
	case 0x12: return DelayTimer(rec, index);

	// cmd_06_empty_noop (raw_commands.c:499) and cmd_empty_allow_sfx (506) are
	// both `or eax,-1; ret`: inert, but they report ACTED so the post-chain sound
	// still fires. NOT the same thing as cmd_default_nop, which returns 0.
	case 0x06: case 0x3E: return CMD_ACTED;

	default:
		if (IsVerifiedNop(op)) return CMD_NOTHING;     // cmd_default_nop
		g.unhandledOps[op]++;
		return CMD_NOTHING;
	}
}

// The handlers that need the chain's Handlers block go through here, because the
// spine's own handler signature does not carry it. Same dispatch, one level out.
int RunCommandWithChain(const Map &m, const Command &cc, int index, Handlers &h)
{
	const uint8_t op = cc.opcode & 0x7f;
	Command *rec = Rec((uint16_t)index);
	if (rec == nullptr) return CMD_NOTHING;

	switch (op)
	{
	case 0x28: return IfNotFlag(rec, h);
	case 0x36: return Dbase100IfNextFails(index);
	case 0x38: return JumpIfNextFails(rec, h);
	case 0x40: return RunIndexedCommand(*rec, h);
	case 0x2B: return RunDbase100Record(rec);
	default:   return RunCommand(m, cc, index);
	}
}

// Walk a chain: the flow pre-pass first, exactly as the original does, then the
// action pass. A gated chain does nothing this time round.
bool Fire(uint16_t chain)
{
	if (chain == 0) return false;
	if (WalkChainFlow(g.map, chain) != 0) return false;

	Handlers h;
	Handlers *hp = &h;
	h.Run = [hp](const Map &m, const Command &c, int index)
	{
		return RunCommandWithChain(m, c, index, *hp);
	};
	const int result = ExecChain(g.map, chain, h);
	if (result != CMD_NOTHING) g.fired++;
	return result != CMD_NOTHING;
}

//==========================================================================
//
// process_deferred_command_queue -- raw_commands.c:1587
//
// Double-buffered: the front buffer is swapped out, the new front cleared, and
// the old one drained, so a chain that queues something DURING the drain runs
// next step rather than in this one. Each entry restages the "what the player
// just used" context and then runs its chain from the top through
// reset_command_chain_state.
//
//==========================================================================

void DrainDeferred()
{
	if (g.deferred.empty()) return;
	std::vector<Deferred> batch;
	batch.swap(g.deferred);                   // the buffer swap, in our terms
	for (const Deferred &d : batch)
	{
		g.activeFace = (int)d.context - 1;    // 0 means "none"
		Fire(d.chain);
	}
}

//==========================================================================
//
// The arrival point, applied.
//
// ml_relocate_found (map_load.c) centres the player on the destination sector's
// vertex BOUNDING BOX -- min and max of the x and y of each face's first vertex,
// then (min + max) >> 1 -- and shifts the player's height reference by the
// difference between the old and new floor heights, which is to say the player
// keeps their height above the floor and therefore lands standing on it.
//
//==========================================================================

int FindSectorByCommandID(uint16_t id)
{
	for (size_t i = 0; i < g.map.sectors.size(); i++)
		if (g.map.sectors[i].commandID == id) return (int)i;
	return -1;
}

bool SectorCentre(int rothSector, double &outX, double &outY)
{
	if (rothSector < 0 || rothSector >= (int)g.map.sectors.size()) return false;
	const Sector &s = g.map.sectors[rothSector];
	if (s.firstFaceIndex < 0 || s.faceCount == 0) return false;

	int minx = 0x7fff, miny = 0x7fff, maxx = -0x8000, maxy = -0x8000;
	for (int j = 0; j < (int)s.faceCount; j++)
	{
		const int fi = s.firstFaceIndex + j;
		if (fi < 0 || fi >= (int)g.map.faces.size()) return false;
		const int vi = g.map.faces[fi].vertex1;
		if (vi < 0 || vi >= (int)g.map.vertices.size()) return false;
		const Vertex &v = g.map.vertices[vi];
		if (v.x < minx) minx = v.x;
		if (v.x > maxx) maxx = v.x;
		if (v.y < miny) miny = v.y;
		if (v.y > maxy) maxy = v.y;
	}
	outX = double((minx + maxx) >> 1);
	outY = double((miny + maxy) >> 1);
	return true;
}

void RelocateToArrival(int rothSector)
{
	if (g.level == nullptr) return;
	double x = 0., y = 0.;
	if (!SectorCentre(rothSector, x, y)) return;
	if (rothSector >= (int)g.level->sectors.Size()) return;

	sector_t *dest = &g.level->sectors[rothSector];
	const double z = dest->floorplane.ZatPoint(x, y);

	for (int i = 0; i < MAXPLAYERS; i++)
	{
		if (!g.level->PlayerInGame(i)) continue;
		AActor *mo = g.level->Players[i]->mo;
		if (mo == nullptr) continue;
		P_TeleportMove(mo, DVector3(x, y, z), true);
		mo->Vel.Zero();
	}
}

void ConsumeArrival()
{
	if (g.pendingArrivalSector < 0) return;
	const int sec = g.pendingArrivalSector;
	g.pendingArrivalSector = -1;
	RelocateToArrival(sec);
}

//==========================================================================
//
// The pending level change, applied.
//
// process_map_warp_or_load (map_load.c:578) compares the requested name with the
// loaded one: the SAME map relocates the player in place and does not reload; a
// DIFFERENT map loads it and relocates afterwards.
//
//==========================================================================

void ApplyPendingWarp()
{
	if (!g.warpPending) return;
	g.warpPending = false;
	if (g.level == nullptr) return;

	// Same map -> relocate in place, no reload.
	if (stricmp(g.warpMap.c_str(), g.level->MapName.GetChars()) == 0)
	{
		const int arrival = g.warpArrival != 0 ? FindSectorByCommandID(g.warpArrival) : -1;
		if (arrival >= 0) RelocateToArrival(arrival);
		else TheLog().Count("logic: 0x3b same-map warp, arrival point named no sector");
		return;
	}

	if (FindLevelInfo(g.warpMap.c_str(), false) == nullptr)
	{
		Printf(TEXTCOLOR_ORANGE "Realms: map transition to \"%s\" -- no such level is known.\n",
			g.warpMap.c_str());
		return;
	}

	// The arrival point is latched in the map being LEFT and applied in the one
	// being entered, so it has to outlive the level change.
	g_warpArrivalMapName = g.warpMap;
	g_warpArrivalSector = (int)g.warpArrival;
	g.level->ChangeLevel(g.warpMap.c_str(), 0, CHANGELEVEL_NOINTERMISSION);
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
	g.activeFace = g.activeSector = -1;
	g.pendingRecord = 0;
	g.delays.clear();
	g.deferred.clear();
	g.warpPending = false;
	g.fired = 0;
	g.unhandledOps.clear();

	// The arrival point the map we came FROM asked for.
	g.pendingArrivalSector = -1;
	if (g_warpArrivalSector >= 0 && level != nullptr
		&& stricmp(g_warpArrivalMapName.c_str(), level->MapName.GetChars()) == 0)
	{
		g.pendingArrivalSector = FindSectorByCommandID((uint16_t)g_warpArrivalSector);
		if (g.pendingArrivalSector < 0 && log)
			log->Count("logic: 0x3b arrival point named no sector in the destination map");
	}
	g_warpArrivalMapName.clear();
	g_warpArrivalSector = -1;

	// The reverse of the loader's pairing, so the wall the player just used can
	// be named as a Realms FACE -- which is what a key of 0 resolves to.
	g.sideToFace.clear();
	for (auto &kv : g.faceToSide) g.sideToFace[kv.second] = kv.first;

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

		//------------------------------------------------------------------
		// The census. Every INSTRUCTION record in this map, by opcode, split
		// three ways, so the gap is a measured number rather than a feeling.
		//------------------------------------------------------------------
		std::map<uint8_t, int> byOp;
		for (const Command &c : g.map.commands)
			if (!c.isTrigger) byOp[c.opcode & 0x7f]++;

		int done = 0, nop = 0, todo = 0;
		log->Line("  instruction records by opcode:");
		for (auto &kv : byOp)
		{
			const uint8_t op = kv.first;
			const char *state;
			if (IsImplemented(op))     { state = "implemented";     done += kv.second; }
			else if (IsVerifiedNop(op)) { state = "nop (verified)"; nop  += kv.second; }
			else                        { state = "NOT IMPLEMENTED"; todo += kv.second; }
			log->Line("    0x%02x  %5d  %s", (unsigned)op, kv.second, state);
		}
		log->Line("  totals: %d implemented, %d verified no-ops, %d not implemented",
			done, nop, todo);
		log->Count("logic: instruction records whose opcode is not implemented", todo);
		log->Line("  the not-implemented set is almost entirely ACTIVE-EFFECT REGISTRARS");
		log->Line("  (light, texture, height, sector move): they need the effect pool and its");
		log->Line("  per-frame tick, not a handler each. See the roth_runtime.h header.");
		log->Line("  0x2b dialogue needs DBASE100; the item and object opcodes need the");
		log->Line("  inventory and object-state systems. None of it is approximated.");
	}
}

void EndLevel()
{
	g.active = false;
	g.level = nullptr;
	g.byFace.clear();
	g.bySide.clear();
	g.faceToSide.clear();
	g.sideToFace.clear();
	g.bySector.clear();
	g.doorTag.clear();
	g.map = Map();
	g.delays.clear();
	g.deferred.clear();
	g.pendingRecord = 0;
	g.activeFace = g.activeSector = -1;
	g.warpPending = false;
	g.fired = 0;
	g.unhandledOps.clear();
	// g_progressFlags is NOT cleared -- see the header.
}

void ResetProgressFlags()
{
	memset(g_progressFlags, 0, sizeof(g_progressFlags));
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

		// Stage "what the player just used" BEFORE firing, because a key of 0
		// resolves to it (ROTH_COMMANDS.md). Set for every side looked at, so
		// even a side with no trigger of its own leaves the right context behind
		// for the chain the other side fires.
		auto f = g.sideToFace.find(idx);
		if (f != g.sideToFace.end()) g.activeFace = f->second;

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
	g.activeSector = sec->sectornum;
	auto it = g.bySector.find(sec->sectornum);
	if (it == g.bySector.end()) return;
	for (uint16_t chain : it->second) Fire(chain);
	(void)who;
}

//==========================================================================
//
// TickLevelLogic -- the engine-facing half of tick_world_effects
// (raw_commands.c:3288), reduced to the parts that exist here: the delay
// countdowns and the deferred queue they feed. The ACTIVE-EFFECT POOL walk,
// which is most of that function, has nothing to walk yet.
//
//==========================================================================

void TickLevelLogic(FLevelLocals *level)
{
	if (!g.active || level == nullptr || level != g.level) return;

	// An arrival point waits for a player to exist, which it does not at
	// BeginLevel time.
	ConsumeArrival();

	// tick_delay_timer (raw_commands.c:5328): count down, and on expiry push the
	// delay record's NEXT onto the deferred queue, re-arming the record -- its
	// modifier bits 0x21 are cleared unless bit 0x04 is set.
	for (size_t i = 0; i < g.delays.size(); )
	{
		PendingDelay &d = g.delays[i];
		d.remaining -= ROTH_FRAME_TICKS_PER_TIC;
		if (d.remaining > 0) { i++; continue; }

		if (g.deferred.size() < 0x10)                 // the original's cap
		{
			Command *rec = Rec(d.record);
			if (rec != nullptr)
			{
				if (!(rec->modifier & 0x04)) { rec->modifier &= 0xDE; SyncDisabled(rec); }
				Deferred q;
				q.chain = rec->linkIndex;
				q.context = d.context;
				if (q.chain != 0) g.deferred.push_back(q);
			}
			g.delays.erase(g.delays.begin() + i);
		}
		else
		{
			// Queue full: the original leaves the timer sitting at zero and tries
			// again next frame. Do the same rather than dropping it.
			i++;
		}
	}

	DrainDeferred();
	ApplyPendingWarp();
}

} // namespace roth
