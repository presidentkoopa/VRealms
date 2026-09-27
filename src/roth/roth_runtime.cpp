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
#include "roth_objects.h"
#include "actor.h"
#include "d_player.h"
#include <math.h>
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

//==========================================================================
//
// THE ACTIVE-EFFECT POOL -- raw_commands.c:3307, the pool walk inside
// tick_world_effects, and alloc_active_effect at 4308.
//
// Most Realms opcodes are not handlers. The 0x30780 dispatch table's entry for
// a light, a height, a sector move or a texture animation does not do the thing:
// it ALLOCATES one of these, and the visible behaviour lives in a second table
// at 0x3088c indexed by the SAME opcode, run once per frame until it reports
// finished. So an opcode like 0x1d is two functions, a registrar and a tick.
//
// The original's record is a heap chunk with the collected geometry appended
// after a per-type payload; the layout is DOS heap discipline we do not need, so
// the fields it actually reads are named here instead:
//
//   chunk[+4]  the tick index -- stamped from the command's own base opcode
//   chunk[+6]  the per-type payload word (a ramp, a countdown, a step)
//   chunk[+8]  a back-pointer to the command record that registered it
//   chunk[+size] onward, the collected geometry group
//
// A record is marked registered ([rec+2] |= 0x20) so a second execution
// refreshes the existing effect rather than stacking another, which is what
// find_active_effect is for.
//
//==========================================================================

// A prop, addressed the way the map stores them. See ResolveCommandObjects.
struct ObjectRef { int sector; int index; };

struct Effect
{
	uint8_t  tick = 0;            // chunk[+4]: which per-frame handler
	uint16_t record = 0;          // chunk[+8]: the command, as a 1-based index
	uint16_t payload = 0;         // chunk[+6]: per-type; the ramp (0x1d) or phase (0x11)
	std::vector<int> sectors;     // the collected geometry group
	bool finished = false;

	// 0x11 flash-lights only. It animates AROUND each sector's brightness rather
	// than accumulating onto it, so the registrar snapshots the starting value
	// per sector (the original stashes it beside each match, at match+2) and
	// every frame writes base + pattern. baseLight is parallel to sectors.
	std::vector<uint8_t> baseLight;
	uint16_t hold = 0;            // chunk[+0xc]: the between-bursts countdown
	bool holding = false;         // chunk[+5] bit 0x80

	// The HEIGHT and TEXTURE families carry more state than the lighting ones.
	// chunk[+5] is their control byte and the names are the original's meanings:
	//   0x80 ascending   0x40 armed (dwelling, not moving)   0x20 / 0x10 repeat
	// and chunk[+6], which lighting uses as a ramp, is either a dwell countdown
	// (while armed) or the low 6 fractional bits of a fixed-point step.
	uint8_t flags5 = 0;

	// The members of an OBJECT effect. Sectors go in `sectors` above; these are
	// the (sector, index) pairs of the props a command named.
	std::vector<ObjectRef> objects;
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
	// Realms texture-map record -> the faces that use it, for the scroll effect:
	// its members are MAPPING records and the sidedefs hang off the faces.
	std::map<int, std::vector<int>> texmapToFaces;
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

	// THE ACTIVE-EFFECT POOL. See TickEffects.
	std::vector<Effect> effects;

	// Realms sector -> the engine sector holding its mid-platform's two planes.
	std::map<int, int> platformCtrl;

	// Realms FLAT index -> the engine texture, and whether the index is the
	// pack's sky marker. See RegisterFlat.
	std::map<int, std::pair<FTextureID, bool>> flatByIndex;

	// Realms texture index -> the engine texture the loader made for it.
	// PRE-RESOLVED rather than looked up on demand, because the loader's
	// TextureSet is a local that dies with the load; the loader registers every
	// index its command records can name. See RegisterTexture.
	std::map<int, FTextureID> texByIndex;

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
// WHICH GEOMETRY A TRIGGER WATCHES. This is read off the MARKERS, not guessed
// from the opcode number: a trigger does not test anything itself, it sets a bit
// on the geometry at load and a reader elsewhere tests that bit. ml_objinit_tab
// (map_load.c:1087) says which marker each opcode gets, and the marker says
// which index space the key lives in:
//
//   mark_raw_state_records_by_key (raw_commands.c:5032) writes geom[rec+9] |= bit
//   over collect_raw_state_matches -- the FACE scan, the same collector
//   gather_faces_by_id uses, with the same two-hop key resolution the reader
//   implements. So its opcodes are FACE-keyed:
//       0x18 -> bit 1   0x1a -> bit 2   0x32 -> bit 4
//
//   mark_geometry_faces_by_key writes the SECTOR's own flags byte +0x16:
//       0x19 -> 0x10    0x31 -> 0x20
//
// (0x13 is a third kind -- it marks sector +0x17 and is read per-frame by
// twe_link_state, the water/lava machine. It is not one of these.)
//
// CORRECTED 2026-09: this file previously bound 0x18 and 0x32 BY SECTOR, on the
// assumption that they were enter-sector triggers. They are not; they are
// face-keyed like 0x1a, and binding them to a sector pointed them at the wrong
// geometry entirely.
//
// AND THE EVENTS ARE NOT WHAT THE OLD NAMES CLAIMED. Tracing each bit to its
// reader, there is no per-frame "player entered a sector" poll for any of these:
//
//   bit 2 (0x1a) is read in the WALL-COLLISION hit path
//   (collision_physics.c:562) and fires fire_wall_object_trigger. It is a BUMP.
//   bit 1 (0x18) is read by dispatch_entry_command_trigger's type-3 channel
//   (raw_commands.c:3019), gated on a direction mask and a bounding box.
//   bit 4 (0x32) is read by dispatch_entry_command_trigger_b (raw_commands.c:3114).
//   sector 0x10/0x20 (0x19/0x31) gate that same dispatcher's use channels.
//
// THE DIRECTION MASK AND BOUNDING BOX ARE NOT IMPLEMENTED HERE. Both live in the
// object-table refs the dispatcher scans, which this port does not build yet, so
// a face trigger fires whenever its face is activated rather than only from the
// authored approach direction. That is a KNOWN over-fire, recorded in the load
// report, not an approximation of the original's test.
bool IsFaceTrigger(uint8_t op)   { return op == 0x18 || op == 0x1A || op == 0x32; }
bool IsSectorTrigger(uint8_t op) { return op == 0x19 || op == 0x31; }

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
// THE UNIT IS DERIVED, NOT STATED. The countdown is decremented by a PIT tick
// delta, and the PIT latch reprogram itself lives in the hardware seam ROTH.C
// replaces -- no `out 0x43/0x40` value survives anywhere in the tree, so the
// rate is never written down. But the ISR's OTHER half was transcribed, and it
// pins the rate down. game_heartbeat_timer_isr (dos_runtime.c:280-289) keeps a
// divider so it can chain to the BIOS handler at the BIOS's own 18.2065 Hz:
//
//     word[0x7e918] -= 0x3e8;                   // 1000 every ISR tick
//     if ((int16_t)word[0x7e918] < 0) += 0xf17;  // 3863 on underflow
//
// so the chain fires on 1000/3863 of ISR ticks, i.e. every 3.863 of them, and
// the ISR rate is 18.2065 * 3.863 = 70.33 Hz.
//
// CAUTION -- THE PROSE BESIDE IT SAYS 120. The same comment block calls the
// latch "keeping the 120 Hz divisor" and reads 1000/3863 as "the 18.2-of-120 Hz
// ratio", which the arithmetic does not support: 18.2-of-120 would be 1000/6592.
// The constants are transcribed opcodes and the "120 Hz" is prose about the
// untranscribed seam, so the constants are taken as the evidence. Two things
// corroborate them: 70.33 Hz is VGA's 400-line refresh (70.086 Hz), and the one
// engine-visible call the ISR makes is vsync_timer_tick. Realms' heartbeat is
// the video retrace.
//
// 70.33 Hz against GZDoom's 35 tics/s is 2.008, so ONE WORLD TIC CONSUMES TWO
// REALMS TICKS. That puts the retail delay values (30, 60, 120, 200, 300) at
// 0.43s, 0.85s, 1.7s, 2.8s and 4.3s. Anyone revisiting this: the number below is
// the derivation above and nothing else -- it was not tuned against the screen.
//
//==========================================================================

// 70.33 Hz / 35 tics per second. Derived above from the ISR's chain divider.
static const int ROTH_FRAME_TICKS_PER_TIC = 2;

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

//==========================================================================
//
// cmd_change_face_texture -- raw_commands.c:396, RAW command base 0x34
//
// Repaints a wall. The original gathers the texture-MAPPING records matching the
// key, then writes one of three slots in each:
//
//     dirOff = {2, 6, 4, 2}[flags & 3]
//
// which lands exactly on our TextureMap fields -- +2 mid, +6 lower, +4 upper --
// so `flags & 3` selects mid / lower / upper / mid. The value written is the
// record's +0x0a, the texture index.
//
// This is an IMMEDIATE handler, not a registrar: 0x32738 is absent from the
// 0x3088c per-frame tick table (boot.c:211-229), so it does the work and
// returns. That is why it can be implemented before the active-effect pool.
//
// THE TRANSPARENCY FLIP IS NOT IMPLEMENTED, and deliberately not faked. The
// original also flips bit 0 of the paired face record's +0x0a -- our
// Face::collisionFlags -- under `if (!(fl & 4)) { al |= 1; if (fl & 8) al &= 0xfe; }`.
// The loader never consumes collisionFlags for anything, so there is no
// renderer or collision state for the flip to change: writing it would be a
// store nobody reads. It is recorded here and counted in the report instead.
//
//==========================================================================

int ChangeFaceTexture(const Command &c)
{
	if (g.level == nullptr) return CMD_NOTHING;

	// Which slot, and the sidedef part the loader put that slot on (rothmap.cpp:
	// top <- upperTexture, bottom <- lowerTexture).
	enum Slot { SlotMid, SlotLower, SlotUpper };
	static const Slot slotOf[4] = { SlotMid, SlotLower, SlotUpper, SlotMid };
	const Slot slot = slotOf[c.fireFlags & 3];

	auto tex = g.texByIndex.find((int)c.aux);
	if (tex == g.texByIndex.end() || !tex->second.isValid())
	{
		g.unhandledOps[0x34]++;          // the loader never registered this index
		return CMD_NOTHING;
	}

	// A key of 0 is "what the player just used", same as everywhere else.
	std::vector<int> targets;
	if (c.key == 0) { if (g.activeFace >= 0) targets.push_back(g.activeFace); }
	else targets = c.faces;
	if (targets.empty()) return CMD_NOTHING;

	int changed = 0;
	for (int fi : targets)
	{
		if (fi < 0 || (size_t)fi >= g.map.faces.size()) continue;

		// Keep the map's own copy in step, because it is the game's state: a
		// later 0x34 on the same wall compares against it, and the original
		// reports "no change" when the slot already holds the index.
		const int tmi = g.map.faces[fi].textureMap;
		if (tmi >= 0 && (size_t)tmi < g.map.textureMaps.size())
		{
			TextureMap &tm = g.map.textureMaps[tmi];
			uint16_t &field = slot == SlotMid   ? tm.midTexture
			                : slot == SlotLower ? tm.lowerTexture
			                                    : tm.upperTexture;
			if (field == c.aux) continue;                 // already there
			field = c.aux;
		}

		auto sd = g.faceToSide.find(fi);
		if (sd == g.faceToSide.end()) continue;
		if (sd->second < 0 || (size_t)sd->second >= g.level->sides.Size()) continue;

		const int part = slot == SlotMid   ? side_t::mid
		               : slot == SlotLower ? side_t::bottom
		                                   : side_t::top;
		g.level->sides[sd->second].SetTexture(part, tex->second);
		changed++;
	}

	// The original returns "did anything change", which the chain uses.
	return changed > 0 ? CMD_ACTED : CMD_NOTHING;
}

//==========================================================================
//
// The two geometry collectors the registrars choose between, selected by the
// record's own bit 0x04 (alloc_active_effect's `flag`).
//
// geom_find_matches -- every sector whose commandID is the key. The plain one.
//
// collect_connected_geometry_group (raw_commands.c) -- a FLOOD outward from the
// sector the key names: for each of that sector's faces whose collisionFlags
// bit 3 is clear, cross to the face's SISTER and take the sister's sector, then
// recurse if it has not been visited. So "light this room and everywhere it
// joins", with bit 3 the barrier that stops the spread. This is how one command
// lights a whole connected area without naming every sector in it.
//
//==========================================================================

int FindSectorByCommandID(uint16_t id);   // defined below, beside the warp code

static void FloodConnected(int sec, std::vector<int> &out, std::vector<char> &seen)
{
	if (sec < 0 || (size_t)sec >= g.map.sectors.size()) return;
	if (seen[sec]) return;
	seen[sec] = 1;
	out.push_back(sec);

	const Sector &s = g.map.sectors[sec];
	if (s.firstFaceIndex < 0) return;
	for (int i = 0; i < (int)s.faceCount; i++)
	{
		const int fi = s.firstFaceIndex + i;
		if (fi < 0 || (size_t)fi >= g.map.faces.size()) continue;
		const Face &f = g.map.faces[fi];
		if (f.collisionFlags & 0x8) continue;            // the barrier bit
		if (f.sister < 0 || (size_t)f.sister >= g.map.faces.size()) continue;
		FloodConnected(g.map.faces[f.sister].sector, out, seen);
	}
}

static std::vector<int> CollectGeometryGroup(uint16_t key, bool connected)
{
	std::vector<int> out;
	if (connected)
	{
		const int start = FindSectorByCommandID(key);
		if (start < 0) return out;
		std::vector<char> seen(g.map.sectors.size(), 0);
		FloodConnected(start, out, seen);
	}
	else
	{
		for (size_t i = 0; i < g.map.sectors.size(); i++)
			if (g.map.sectors[i].commandID == key) out.push_back((int)i);
	}
	// Both collectors are capped at 0xc8 by their caller's scratch list, and a
	// flood that reaches the cap simply stops there.
	if (out.size() > 0xc8) out.resize(0xc8);
	return out;
}

//==========================================================================
//
// apply_light_delta_to_record_list (raw_commands.c) -- what every lighting
// effect ultimately does.
//
// It adds a signed delta to the sector's brightness BYTE at +0x0b, which is
// Sector::light here, and skips any sector whose brightness is already exactly
// zero. That guard is not a bounds check: a sector authored at 0 is immune to
// being lit at all, which the original's designers used deliberately -- the same
// test keeps the muzzle flash out of those sectors (renderer.c:9187, and the
// loader's own note beside this formula).
//
// Realms brightness is not a Doom light level, so the engine-side value is
// RECOMPUTED through the loader's derivation rather than nudged in parallel.
// Keeping the Realms byte the single authority is the only way the two stay in
// step across a fade that runs for many tics.
//
//==========================================================================

static void ApplyLightDelta(const std::vector<int> &sectors, int delta)
{
	if (g.level == nullptr || delta == 0) return;
	const int shadeShift = g.level->ShadeFalloffShift;

	for (int si : sectors)
	{
		if (si < 0 || (size_t)si >= g.map.sectors.size()) continue;
		Sector &rs = g.map.sectors[si];
		if (rs.light == 0) continue;                      // authored dark: immune

		rs.light = (uint8_t)clamp<int>((int)rs.light + delta, 0, 255);

		if ((size_t)si >= g.level->sectors.Size()) continue;
		const int rows = 39 + ((int)rs.light - 128);
		const int ll = rows <= 0 ? 0
			: int((255.0 * double(rows) * double(1 << shadeShift)) / 1984.0 + 0.5);
		g.level->sectors[si].lightlevel = (short)clamp<int>(ll, 0, 255);
	}
}

static Effect *FindEffect(uint8_t tick, uint16_t record)
{
	for (Effect &e : g.effects)
		if (!e.finished && e.tick == tick && e.record == record) return &e;
	return nullptr;
}

//==========================================================================
//
// cmd_change_lighting -- raw_commands.c:4389, RAW command base 0x1d
//
// The REGISTRAR. It does not change a light. It finds or allocates the effect,
// then writes the ramp into the payload word:
//
//   step = byte[rec+7], negated when the record's bit 0x02 is set
//   payload = step < 0 ? (0xff00 | -step) : step
//
// so the payload's LOW byte is how far the brightness still has to travel and
// the HIGH byte is nonzero when travelling down. Bit 0x01 of +0x06 flips the
// direction bit afterwards, which is what lets ONE record alternate: fire it
// again and it fades back the other way.
//
//==========================================================================

int ChangeLighting(Command *rec, int index)
{
	Effect *eff = FindEffect(0x1D, (uint16_t)index);
	if (eff == nullptr)
	{
		// Already registered, or disabled: the original refuses rather than
		// stacking a second effect on the same record.
		if (rec->modifier & 0x21) return CMD_NOTHING;

		Effect e;
		e.tick = 0x1D;
		e.record = (uint16_t)index;
		e.sectors = CollectGeometryGroup(rec->key, (rec->modifier & 0x04) != 0);
		if (e.sectors.empty()) return CMD_NOTHING;       // alloc_active_effect's 0
		g.effects.push_back(e);
		eff = &g.effects.back();
		rec->modifier |= 0x20;
		SyncDisabled(rec);
	}

	int step = (int)rec->subFlags;                        // byte[rec+7]
	if (rec->modifier & 0x02) step = -step;
	eff->payload = step < 0 ? (uint16_t)(0xFF00u | (uint8_t)(-step))
	                        : (uint16_t)(uint8_t)step;
	if (rec->fireFlags & 0x01) { rec->modifier ^= 0x02; SyncDisabled(rec); }
	eff->tick = 0x1D;
	return CMD_ACTED;
}

//==========================================================================
//
// tick_change_lighting -- raw_commands.c:1788
//
// Walk the ramp down by the frame delta, applying whatever it consumed as the
// brightness change, negated when the direction byte is set. When the low byte
// reaches zero the effect is done and it finalises on the COMMAND record: +0x06
// bit 0x10 means "disable me now that I have finished", and then bits 0x21 are
// cleared so the record can be registered again.
//
//==========================================================================

static bool TickChangeLighting(Effect &e)
{
	Command *rec = Rec(e.record);
	uint8_t remaining = (uint8_t)(e.payload & 0xFF);
	const bool down = (e.payload >> 8) != 0;

	if (remaining == 0)
	{
		if (rec != nullptr)
		{
			if (rec->fireFlags & 0x10) rec->modifier |= 0x08;
			rec->modifier &= 0xDE;
			SyncDisabled(rec);
		}
		return true;                                      // finished
	}

	int stepThisTic = ROTH_FRAME_TICKS_PER_TIC;
	if (stepThisTic > 0x40) stepThisTic = 0x40;           // the original's clamp
	if (remaining <= stepThisTic) stepThisTic = remaining;

	remaining = (uint8_t)(remaining - stepThisTic);
	e.payload = (uint16_t)((e.payload & 0xFF00u) | remaining);

	ApplyLightDelta(e.sectors, down ? -stepThisTic : stepThisTic);
	return false;
}

//==========================================================================
//
// resolve_command_objects -- raw_commands.c:318. Which objects a command acts on.
//
// Every object whose commandID is the key, found by scanning: exactly the same
// "an id is searched for, never indexed" rule as sectors and faces. The original
// walks its flat object buffer; the objects live per-sector here, which is how
// the map stores them, so the pair (sector, index) is the address.
//
// A key of 0 means the single g_command_source_object -- the object the player
// just used. That is not tracked yet: this port latches the active FACE and
// SECTOR for key-0 resolution but has no equivalent for objects, so a key of 0
// is counted as unresolved rather than silently acting on the wrong prop.
//
//==========================================================================


static std::vector<ObjectRef> ResolveCommandObjects(uint16_t key)
{
	std::vector<ObjectRef> out;
	if (key == 0) return out;                // see above: no active-object latch

	for (size_t si = 0; si < g.map.objects.size(); si++)
	{
		const std::vector<Object> &list = g.map.objects[si];
		for (size_t oi = 0; oi < list.size(); oi++)
			if (list[oi].commandID == key)
			{
				ObjectRef r; r.sector = (int)si; r.index = (int)oi;
				out.push_back(r);
				if (out.size() >= 0xc8) return out;     // the original's cap
			}
	}
	return out;
}

// Push a rotation byte we have just changed out to the world. The Realms byte
// stays the authority and the actor's yaw is derived from it through the same
// conversion the spawn used, so one turn cannot drift from the other.
static void ApplyObjectRotation(const ObjectRef &r, uint8_t rotation)
{
	if (r.sector < 0 || (size_t)r.sector >= g.map.objects.size()) return;
	std::vector<Object> &list = g.map.objects[r.sector];
	if (r.index < 0 || (size_t)r.index >= list.size()) return;

	list[r.index].rotation = rotation;
	AActor *mo = FindObjectActor(r.sector, r.index);
	if (mo != nullptr) mo->Angles.Yaw = DAngle::fromDeg(ObjectYaw(rotation));
}

// compute_player_object_bearing (raw_commands.c:300): the 8-bit facing from the
// player to the object. The original is atan2_bearing(player -> object) >> 1,
// over a 512-step table halved to 256; the same angle here is the actor-space
// bearing folded into a byte, taken through the object's own yaw convention so
// "face the player" agrees with how the prop was spawned facing.
static uint8_t PlayerObjectBearing(const ObjectRef &r)
{
	if (g.level == nullptr) return 0;
	if (r.sector < 0 || (size_t)r.sector >= g.map.objects.size()) return 0;
	const std::vector<Object> &list = g.map.objects[r.sector];
	if (r.index < 0 || (size_t)r.index >= list.size()) return 0;

	AActor *player = nullptr;
	for (int i = 0; i < MAXPLAYERS; i++)
		if (g.level->PlayerInGame(i) && g.level->Players[i]->mo != nullptr)
		{ player = g.level->Players[i]->mo; break; }
	if (player == nullptr) return 0;

	const Object &o = list[r.index];
	const double dx = double(o.x) - player->X();
	const double dy = double(o.y) - player->Y();
	if (dx == 0.0 && dy == 0.0) return o.rotation;

	// The inverse of ObjectYaw: that maps a rotation byte to degrees, so a
	// bearing in degrees maps back through the same constants.
	const double deg = atan2(dy, dx) * (180.0 / M_PI);
	return RotationFromYaw(deg);
}

//==========================================================================
//
// cmd_rotate_object -- raw_commands.c:241, RAW command base 0x24
//
// IMMEDIATE, despite 0x24 also having an entry in the per-frame tick table: this
// handler turns the objects and returns. (The tick entry belongs to a continuous
// rotation registered through a different path, which is not wired here.)
//
// Two modes, on +0x06 bit 0x02:
//
//   STEP -- the record counts a FRAME through n positions, keeping the frame in
//   byte[rec+0x0b], and turns by the difference between two quantised angles,
//   newFrame*256/n - oldFrame*256/n. So a cupboard with n=4 turns in exact
//   quarters and returns to true after four uses, which adding 256/n each time
//   would not do. Bit 0x01 runs it backwards. n < 2 does nothing.
//
//   PLAYER-RELATIVE -- n <= 1 SNAPS the object to face the player; otherwise it
//   steps toward the player's bearing, clamped to +/-n.
//
// Bit 0x01 of the modifier is an early-out, and bit 0x10 of +0x06 re-arms the
// record by setting its disable bit afterwards.
//
//==========================================================================

int RotateObject(Command *rec)
{
	if (rec->modifier & 0x01) return CMD_NOTHING;

	std::vector<ObjectRef> targets = ResolveCommandObjects(rec->key);
	if (targets.empty())
	{
		if (rec->key == 0) g.unhandledOps[0x24]++;    // no active-object latch
		return CMD_NOTHING;
	}

	const uint8_t flags = rec->fireFlags;             // byte[rec+6]
	const uint8_t n = rec->Byte(0x0A);

	if (!(flags & 0x02))                              // STEP MODE
	{
		if (n <= 1) return CMD_NOTHING;

		uint8_t dir, sentinel, wrap;
		if (flags & 0x01) { dir = 0xFF; sentinel = 0xFF; wrap = (uint8_t)(n - 1); }
		else              { dir = 0x01; sentinel = n;    wrap = 0; }

		const uint8_t oldframe = rec->Byte(0x0B);
		uint8_t newframe = (uint8_t)(oldframe + dir);
		if (newframe == sentinel) newframe = wrap;
		rec->SetByte(0x0B, newframe);

		const uint8_t newQ = (uint8_t)(((uint16_t)newframe << 8) / n);
		const uint8_t oldQ = (uint8_t)(((uint16_t)oldframe << 8) / n);
		const uint8_t delta = (uint8_t)(newQ - oldQ);

		for (const ObjectRef &r : targets)
		{
			const std::vector<Object> &list = g.map.objects[r.sector];
			ApplyObjectRotation(r, (uint8_t)(list[r.index].rotation + delta));
		}
	}
	else if (n <= 1)                                  // SNAP TO PLAYER
	{
		for (const ObjectRef &r : targets)
			ApplyObjectRotation(r, PlayerObjectBearing(r));
	}
	else                                              // STEP TOWARD PLAYER
	{
		const int lim = (int)n;
		for (const ObjectRef &r : targets)
		{
			const std::vector<Object> &list = g.map.objects[r.sector];
			const uint8_t cur = list[r.index].rotation;
			int v = (int)(int8_t)(uint8_t)(PlayerObjectBearing(r) - cur);
			if (v >= lim) v = lim;
			v = -v;
			if (v >= lim) v = lim;
			ApplyObjectRotation(r, (uint8_t)(cur - (uint8_t)v));
		}
	}

	if (flags & 0x10) { rec->modifier |= 0x08; SyncDisabled(rec); }
	return CMD_ACTED;
}

//==========================================================================
//
// THE LIGHT PATTERNS -- obj1_owned.c:95, the 62-byte block at 0x322ce, with the
// five sub-block pointers from boot.c:177 and the count dword (=5) at +0x3e.
//
// This is data out of ROTH.EXE's own image, not out of the player's game files,
// so it is transcribed rather than read: the five offsets 0x00 / 0x11 / 0x20 /
// 0x2d / 0x36 slice the block into patterns whose first byte is the PERIOD and
// whose remaining bytes are the brightness deltas, period of them each. All five
// check out against their own length.
//
//==========================================================================

struct LightPattern { uint8_t period; const uint8_t *delta; };

static const uint8_t kPat0[16] = { 0x00,0x02,0x04,0x06,0x08,0x0a,0x0c,0x0e,
                                   0x10,0x12,0x14,0x16,0x18,0x1a,0x1c,0x00 };
static const uint8_t kPat1[14] = { 0x00,0x14,0x0f,0x0a,0x00,0x00,0x00,
                                   0x00,0x00,0x28,0x1e,0x14,0x0a,0x00 };
static const uint8_t kPat2[12] = { 0x04,0x08,0x0c,0x10,0x12,0x14,
                                   0x12,0x10,0x0c,0x08,0x04,0x00 };
static const uint8_t kPat3[8]  = { 0x00,0x3c,0x32,0x28,0x1e,0x14,0x0a,0x00 };
static const uint8_t kPat4[6]  = { 0x00,0x14,0x3c,0x28,0x14,0x00 };

static const LightPattern kLightPatterns[5] = {
	{ 16, kPat0 }, { 14, kPat1 }, { 12, kPat2 }, { 8, kPat3 }, { 6, kPat4 },
};
static const int kLightPatternCount = 5;      // the count dword at block +0x3e

//==========================================================================
//
// apply_flag_mask_to_record_list (raw_commands.c) -- the other thing a lighting
// effect can do on the way out: rewrite the sector FLAGS byte at +0x0a, which is
// Sector::flags here, as (flags & ~clear) | set.
//
// A light switch uses it to record which way it ended, on bit 0x02. Nothing in
// the loader reads that bit today, so this keeps the Realms byte correct without
// yet having a visible effect -- worth maintaining because it is one byte and it
// is the sector's own on/off state, which a later reader will want.
//
//==========================================================================

static void ApplyFlagMask(const std::vector<int> &sectors, uint8_t clear, uint8_t set)
{
	for (int si : sectors)
	{
		if (si < 0 || (size_t)si >= g.map.sectors.size()) continue;
		Sector &rs = g.map.sectors[si];
		rs.flags = (uint8_t)((rs.flags & (uint8_t)~clear) | set);
	}
}

// Set a sector's brightness OUTRIGHT rather than by a delta, for the flash
// effect, which writes base + pattern every frame. Same authored-dark immunity
// and the same recompute as ApplyLightDelta -- see there for both.
static void SetSectorLight(int si, int value)
{
	if (g.level == nullptr) return;
	if (si < 0 || (size_t)si >= g.map.sectors.size()) return;
	Sector &rs = g.map.sectors[si];
	if (rs.light == 0) return;                            // authored dark: immune

	rs.light = (uint8_t)clamp<int>(value, 0, 255);
	if ((size_t)si >= g.level->sectors.Size()) return;
	const int rows = 39 + ((int)rs.light - 128);
	const int ll = rows <= 0 ? 0
		: int((255.0 * double(rows) * double(1 << g.level->ShadeFalloffShift)) / 1984.0 + 0.5);
	g.level->sectors[si].lightlevel = (short)clamp<int>(ll, 0, 255);
}

//==========================================================================
//
// cmd_light_switch -- raw_commands.c:4331, RAW command base 0x02
//
// A switch, not a fade. The effect record carries nothing: the travel left is
// kept on the COMMAND record, in the signed byte at +0x0c, and each execution
// ADDS to it -- so hitting the same switch twice before it settles doubles the
// throw rather than restarting it.
//
// Two things differ from 0x1d and both matter. The key comes from +0x0a, not
// +0x08, so a switch names its geometry in a different field from every other
// lighting opcode. And the collector is ALWAYS the connected flood, never the
// flat match, so a switch necessarily lights a whole connected area.
//
//==========================================================================

int LightSwitch(Command *rec, int index)
{
	Effect *eff = FindEffect(0x02, (uint16_t)index);
	if (eff == nullptr)
	{
		Effect e;
		e.tick = 0x02;
		e.record = (uint16_t)index;
		e.sectors = CollectGeometryGroup(rec->aux, true);   // key at +0x0a, always flood
		if (e.sectors.empty()) return CMD_NOTHING;
		g.effects.push_back(e);
		eff = &g.effects.back();
		rec->modifier |= 0x20;
		SyncDisabled(rec);
	}

	uint8_t dl = rec->subFlags;                            // byte[rec+7]
	if (!(rec->fireFlags & 0x08)) dl = (uint8_t)(0u - dl);
	rec->SetByte(0x0C, (uint8_t)(rec->Byte(0x0C) + dl));
	rec->fireFlags ^= 0x08;
	rec->SetByte(0x06, rec->fireFlags);                    // fireFlags mirrors byte[rec+6]
	rec->modifier ^= 0x02;
	SyncDisabled(rec);
	eff->tick = 0x02;
	return CMD_ACTED;
}

//==========================================================================
//
// tick_light_switch -- raw_commands.c:1816
//
// Walk the accumulator at byte[rec+0x0c] toward zero, at most one frame step at
// a time, and apply exactly what it moved. Transcribed rather than rewritten,
// because the clamp is a two-sided one that is easy to simplify wrongly:
//
//     v = min(acc, step);  v = -v;  delta = (v <= step) ? v : step
//
// which yields -step while acc is positive and +step while it is negative, so a
// switch of either sign converges. On arrival, +0x06 bit 0x80 means "record which
// way I ended" -- bit 0x02 of the sector flags, set or cleared by the sign of the
// last delta.
//
//==========================================================================

static bool TickLightSwitch(Effect &e)
{
	Command *rec = Rec(e.record);
	if (rec == nullptr) return true;

	const int step = ROTH_FRAME_TICKS_PER_TIC;
	int v = (int)(int8_t)rec->Byte(0x0C);
	if (!(v < step)) v = step;
	v = -v;
	int delta = (!(v > step)) ? v : step;

	rec->SetByte(0x0C, (uint8_t)(rec->Byte(0x0C) + (uint8_t)delta));
	ApplyLightDelta(e.sectors, delta);

	if (rec->Byte(0x0C) != 0) return false;                // still throwing

	if (rec->fireFlags & 0x80)
		ApplyFlagMask(e.sectors, (delta & 0x80) ? 0 : 2, (delta & 0x80) ? 2 : 0);
	rec->modifier &= 0xDE;
	SyncDisabled(rec);
	return true;
}

//==========================================================================
//
// cmd_flash_lights -- raw_commands.c:4453, RAW command base 0x11
//
// The registrar SNAPSHOTS each sector's current brightness, because the flash
// animates around it: every frame the tick writes base + pattern, so the
// brightness returns exactly where it started instead of drifting the way a
// sequence of deltas would.
//
//==========================================================================

int FlashLights(Command *rec, int index)
{
	if (rec->modifier & 0x21) return CMD_NOTHING;          // already armed

	Effect e;
	e.tick = 0x11;
	e.record = (uint16_t)index;
	e.sectors = CollectGeometryGroup(rec->key, (rec->modifier & 0x04) != 0);
	if (e.sectors.empty()) return CMD_NOTHING;

	e.baseLight.reserve(e.sectors.size());
	for (int si : e.sectors)
		e.baseLight.push_back((size_t)si < g.map.sectors.size() ? g.map.sectors[si].light : 0);

	e.payload = 0;                                         // chunk[6] = 0: the phase
	g.effects.push_back(e);
	rec->modifier |= 0x20;
	SyncDisabled(rec);
	return CMD_ACTED;
}

//==========================================================================
//
// tick_flash_lights -- raw_commands.c:3516
//
// The pattern index is byte[rec+7], bounded by the block's own count of 5; out of
// range finishes the effect rather than reading past the table. The phase runs to
// twice the period and samples at phase/2, so every pattern byte is held for two
// frame ticks.
//
// Three paths, and the record's +0x06 picks between them:
//
//   bit 0x04 CHASE -- each sector samples the NEXT pattern byte, so the burst
//   travels along the group instead of hitting it all at once.
//   otherwise SINGLE SAMPLE -- every sector shares one delta.
//   bit 0x01 inverts the delta, so a pattern can darken instead of brighten.
//
// And at the end of a sweep, +0x06 bit 0x20 decides whether it repeats: without
// it the effect emits one last sample and finishes; with it, a nonzero +0x0a is
// latched as a hold countdown between bursts -- randomized when bit 0x02 is set,
// through the LCG the original shares with the delay opcode -- and a zero +0x0a
// just runs straight into the next sweep.
//
//==========================================================================

static bool TickFlashLights(Effect &e)
{
	Command *rec = Rec(e.record);
	if (rec == nullptr) return true;
	const int step = ROTH_FRAME_TICKS_PER_TIC;

	// Holding between bursts: nothing is emitted, just the countdown.
	if (e.holding)
	{
		const uint16_t cd = e.hold;
		e.hold = (uint16_t)(cd - step);
		if (cd < (uint16_t)step) e.holding = false;
		return false;
	}

	const uint32_t patIdx = rec->subFlags;                 // byte[rec+7]
	if (patIdx >= (uint32_t)kLightPatternCount)
	{
		rec->modifier &= 0xDE;
		SyncDisabled(rec);
		return true;
	}
	const LightPattern &pat = kLightPatterns[patIdx];
	const uint32_t period  = pat.period;
	const uint32_t period2 = 2u * period;

	uint32_t phase = (uint32_t)e.payload + (uint32_t)step;
	const uint8_t c6 = rec->fireFlags;                     // byte[rec+6]
	const bool neg = (c6 & 0x01) != 0;
	const size_t n = e.sectors.size();

	if (c6 & 0x04)                                         // CHASE
	{
		if (!(phase < period2))
		{
			phase -= period2;
			if (!(phase < period2)) phase = 0;
		}
		e.payload = (uint16_t)phase;

		uint32_t idx = phase >> 1;
		for (size_t i = 0; i < n; i++)
		{
			int d = (int)pat.delta[idx];
			if (neg) d = -d;
			idx++;
			if (!(idx < period)) idx = 0;
			SetSectorLight(e.sectors[i], (int)e.baseLight[i] + d);
		}
		return false;
	}

	// SINGLE SAMPLE.
	bool finish = false;
	if (phase >= period2)
	{
		if (!(c6 & 0x20))                                  // one-shot: last sample, then done
		{
			finish = true;
			phase = period2 - 1;
		}
		else if (rec->Word(0x0A) == 0)                     // repeat immediately
		{
			phase -= period2;
			if (!(phase < period2)) phase = 1;
		}
		else                                               // repeat after a hold
		{
			uint32_t dwell = rec->Word(0x0A);
			if (c6 & 0x02)
			{
				// The same LCG the delay opcode randomizes through -- the word at
				// g_frame_time_scale+4 is not a time scale at all, it is an RNG
				// state stepped with seed*0x5e5+0x29 (renderer.c:13416).
				g.rng = g.rng * 0x5e5u + 0x29u;
				dwell = (dwell * (g.rng & 0xffffu)) >> 16;
			}
			e.hold = (uint16_t)dwell;
			e.holding = true;
			e.payload = 0;
			return false;
		}
	}

	e.payload = (uint16_t)phase;
	int d = (int)pat.delta[phase >> 1];
	if (neg) d = -d;
	for (size_t i = 0; i < n; i++)
		SetSectorLight(e.sectors[i], (int)e.baseLight[i] + d);

	if (!finish) return false;
	rec->modifier &= 0xDE;
	SyncDisabled(rec);
	return true;
}

//==========================================================================
//
// The three helpers every HEIGHT and TEXTURE effect shares, transcribed once.
//
// rawcmd_texture_countdown -- while the armed bit is set the effect is DWELLING,
// not moving: the dwell word counts down and bit 0x40 clears when it runs out.
//
// rawcmd_texture_tick_finalize -- what happens when a sweep completes, decided by
// two things: whether the record repeats (+0x06 bit 0x20) and whether it has a
// dwell value (+0x0e for heights). The four outcomes are the whole reason a
// Realms lift can go up once, or go up and stop, or go up and come back, or run
// up and down forever with a pause at each end:
//
//     no repeat, no dwell : finish and DISABLE the record
//     repeat,    no dwell : finish, flipping the direction for next time
//     no repeat, dwell    : finish (when already mid-repeat), else turn around
//     repeat,    dwell    : turn around and dwell -- the perpetual case
//
// rawcmd_tick_height_exit -- the overshoot path. Bit 0x10 turns straight around
// without going through the finalize at all.
//
//==========================================================================

static bool TickArmedCountdown(Effect &e)
{
	const uint16_t step16 = (uint16_t)ROTH_FRAME_TICKS_PER_TIC;
	const uint16_t old = e.payload;
	e.payload = (uint16_t)(old - step16);
	if (!((int16_t)old > (int16_t)step16)) e.flags5 &= 0xBF;
	return false;                                   // never finishes here
}

static bool TickFinalize(Effect &e, Command *rec, uint16_t latch)
{
	if (rec == nullptr) return true;

	unsigned which = (rec->fireFlags & 0x20) ? 1u : 0u;
	if (latch != 0) which += 2u;

	if (which == 0)
	{
		rec->modifier = (uint8_t)((rec->modifier & 0xDE) | 8);
		SyncDisabled(rec);
		return true;
	}
	if (which == 1)
	{
		rec->modifier &= 0xDE;
		rec->modifier ^= 0x02;
		SyncDisabled(rec);
		return true;
	}
	if (which == 2 && (e.flags5 & 0x20))
	{
		rec->modifier &= 0xDE;
		SyncDisabled(rec);
		return true;
	}
	e.flags5 ^= 0xE0;                               // turn around, and dwell
	e.payload = latch;
	return false;
}

static bool TickHeightExit(Effect &e, Command *rec)
{
	const uint16_t latch = rec != nullptr ? rec->Word(0x0E) : 0;
	if (e.flags5 & 0x10) { e.flags5 ^= 0x90; return false; }
	return TickFinalize(e, rec, latch);
}

// Push a height we have just changed out to the world. Same discipline as
// rotation: the Realms word stays the authority, the actor follows it.
static void ApplyObjectHeight(const ObjectRef &r, int16_t z)
{
	if (r.sector < 0 || (size_t)r.sector >= g.map.objects.size()) return;
	std::vector<Object> &list = g.map.objects[r.sector];
	if (r.index < 0 || (size_t)r.index >= list.size()) return;

	list[r.index].z = z;
	AActor *mo = FindObjectActor(r.sector, r.index);
	if (mo != nullptr) mo->SetZ((double)z);
}

//==========================================================================
//
// cmd_change_object_height -- raw_commands.c:4554, RAW command base 0x23
//
// Two paths, and which one runs is decided by whether the record is already
// registered. Registering snapshots the objects and sets the initial direction
// from the record's bit 0x02. Re-running an ALREADY registered record does not
// start a second move: it flips the direction of the one in flight, so the same
// command both raises and lowers -- but only when +0x0e is zero and +0x06 bit
// 0x20 is set, which is the original's way of saying "this one is a toggle".
//
// There is no generation re-resolve here. The original re-collects its members
// when the object table relocates, because its members are raw buffer offsets;
// ours are (sector, index) pairs into the map, which nothing moves.
//
//==========================================================================

int ChangeObjectHeight(Command *rec, int index)
{
	const uint8_t base = (uint8_t)(rec->opcode & 0x7f);

	if (rec->modifier & 0x21)                        // TOGGLE
	{
		if (rec->Word(0x0E) != 0) return CMD_NOTHING;
		if (!(rec->fireFlags & 0x20)) return CMD_NOTHING;
		Effect *eff = FindEffect(base, (uint16_t)index);
		if (eff == nullptr) return CMD_NOTHING;
		eff->flags5 ^= 0x80;
		rec->modifier ^= 0x02;
		SyncDisabled(rec);
		return CMD_ACTED;
	}

	Effect e;
	e.tick = base;
	e.record = (uint16_t)index;
	e.objects = ResolveCommandObjects(rec->key);
	if (e.objects.empty()) return CMD_NOTHING;
	e.payload = 0;
	e.flags5 |= (rec->modifier & 0x02) ? 0 : 0x80;
	g.effects.push_back(e);
	rec->modifier |= 0x20;
	SyncDisabled(rec);
	return CMD_ACTED;
}

//==========================================================================
//
// tick_change_object_height -- raw_commands.c:2357
//
// Ramps each named prop's height toward a limit -- 2 * the record's +0x0c going
// up, 2 * +0x0a coming down -- at +0x07 units per frame tick. Bit 0x04 of +0x06
// makes that step FIXED POINT, six fractional bits kept in the effect, so a prop
// can rise slower than one unit per tick instead of not at all.
//
// The budget is shared across the group and the overshoot folds back into it: the
// first member to reach the limit gives back what it did not need, and only when
// the budget is exhausted does the sweep end. That is what keeps a group of props
// moving as one piece rather than each finishing separately.
//
//==========================================================================

static bool TickChangeObjectHeight(Effect &e)
{
	Command *rec = Rec(e.record);
	if (rec == nullptr) return true;

	if (e.flags5 & 0x40) return TickArmedCountdown(e);

	uint32_t delta = (uint32_t)(ROTH_FRAME_TICKS_PER_TIC * (int32_t)rec->Byte(0x07));
	if (rec->fireFlags & 0x04)
	{
		delta += (uint32_t)(e.payload & 0x3F);
		e.payload = (uint16_t)(uint8_t)delta;
		delta >>= 6;
		if (delta == 0) return false;
	}

	if (e.objects.empty())
	{
		rec->modifier &= 0xDE;
		SyncDisabled(rec);
		return true;
	}

	const bool ascend = (e.flags5 & 0x80) != 0;
	int16_t acc   = ascend ? (int16_t)(uint16_t)delta : (int16_t)(uint16_t)(-(int32_t)delta);
	const int16_t limit = (int16_t)(uint16_t)(2 * (int32_t)(int16_t)rec->Word(ascend ? 0x0C : 0x0A));

	for (const ObjectRef &r : e.objects)
	{
		if (r.sector < 0 || (size_t)r.sector >= g.map.objects.size()) continue;
		std::vector<Object> &list = g.map.objects[r.sector];
		if (r.index < 0 || (size_t)r.index >= list.size()) continue;

		const int16_t sum = (int16_t)(list[r.index].z + acc);
		int16_t stored;
		if (ascend ? (sum <= limit) : (sum >= limit))
		{
			stored = sum;
		}
		else
		{
			acc = (int16_t)(acc - (int16_t)(sum - limit));
			if (ascend ? (acc >= 0) : (acc <= 0)) return TickHeightExit(e, rec);
			stored = limit;
		}
		ApplyObjectHeight(r, stored);
	}
	return false;
}

//==========================================================================
//
// cmd_change_object_texture -- raw_commands.c:4528, RAW command base 0x0d
// apply_object_state_to_group -- raw_commands.c, what its tick actually does
//
// A SWAP, and the cleverness is where the other state lives: the record's +0x0c
// holds one texture word and the props wear the other, and each application
// exchanges them. The first member's previous texture is written BACK into the
// record, so running the command again puts it back -- one record, two
// appearances, no second record and no stored "original" anywhere else.
//
// That is why only the record's INITIAL +0x0c needs a sprite built at load: after
// the first swap the record holds a texture some prop was already wearing.
//
// +0x06 bit 0x08 makes it one-way. The capture is skipped, so the props take the
// record's texture and the record keeps it -- it sets rather than toggles.
//
// The texture word packs the object's +0x04 pair: index low, source high.
//
//==========================================================================

// Push a texture word we have just changed out to the world.
static void ApplyObjectTexture(const ObjectRef &r, uint16_t word)
{
	if (r.sector < 0 || (size_t)r.sector >= g.map.objects.size()) return;
	std::vector<Object> &list = g.map.objects[r.sector];
	if (r.index < 0 || (size_t)r.index >= list.size()) return;

	Object &o = list[r.index];
	o.textureIndex  = (uint8_t)(word & 0xFF);
	o.textureSource = (uint8_t)(word >> 8);

	AActor *mo = FindObjectActor(r.sector, r.index);
	if (mo == nullptr) return;
	const int sn = FindLogicSprite(word);
	if (sn >= 0) mo->sprite = sn;
	else g.unhandledOps[0x0D]++;      // no sprite was built for this word
}

int ChangeObjectTexture(Command *rec, int index)
{
	const uint8_t base = (uint8_t)(rec->opcode & 0x7f);

	if (rec->modifier & 0x21)                        // TOGGLE, as 0x23
	{
		if (rec->Word(0x0E) != 0) return CMD_NOTHING;
		if (!(rec->fireFlags & 0x20)) return CMD_NOTHING;
		Effect *eff = FindEffect(base, (uint16_t)index);
		if (eff == nullptr) return CMD_NOTHING;
		eff->flags5 ^= 0x80;
		rec->modifier ^= 0x02;
		SyncDisabled(rec);
		return CMD_ACTED;
	}

	Effect e;
	e.tick = base;
	e.record = (uint16_t)index;
	e.objects = ResolveCommandObjects(rec->key);
	if (e.objects.empty()) return CMD_NOTHING;
	e.payload = 0;
	e.flags5 |= (rec->modifier & 0x02) ? 0 : 0x80;
	g.effects.push_back(e);
	rec->modifier |= 0x20;
	SyncDisabled(rec);
	return CMD_ACTED;
}

static bool TickChangeObjectTexture(Effect &e)
{
	Command *rec = Rec(e.record);
	if (rec == nullptr) return true;

	if (e.flags5 & 0x40) return TickArmedCountdown(e);
	if (e.objects.empty())
	{
		rec->modifier &= 0xDE;
		SyncDisabled(rec);
		return true;
	}

	const bool skip = (rec->fireFlags & 0x08) != 0;
	const ObjectRef &first = e.objects[0];

	// The state being swapped IN, and the first prop's state, read before
	// anything is written -- the original reads both up front for the same reason.
	const uint16_t incoming = rec->Word(0x0C);
	uint16_t firstWord = 0;
	uint8_t firstFlags = 0;
	if (first.sector >= 0 && (size_t)first.sector < g.map.objects.size())
	{
		const std::vector<Object> &fl = g.map.objects[first.sector];
		if (first.index >= 0 && (size_t)first.index < fl.size())
		{
			firstWord  = (uint16_t)(fl[first.index].textureIndex
			                     | ((uint16_t)fl[first.index].textureSource << 8));
			firstFlags = fl[first.index].flags;
		}
	}

	if (!skip)
	{
		rec->SetWord(0x0C, firstWord);                // the swap: keep the old one
		rec->SetByte(0x07, (uint8_t)(firstFlags & 0x10));
	}

	const uint8_t carry = (uint8_t)(firstFlags & 0xEF);
	for (const ObjectRef &r : e.objects)
	{
		ApplyObjectTexture(r, incoming);
		rec->SetByte(0x07, (uint8_t)(rec->Byte(0x07) & 0xEF));
		if (r.sector >= 0 && (size_t)r.sector < g.map.objects.size())
		{
			std::vector<Object> &list = g.map.objects[r.sector];
			if (r.index >= 0 && (size_t)r.index < list.size())
				list[r.index].flags |= carry;
		}
	}

	return TickFinalize(e, rec, rec->Word(0x0A));
}

//==========================================================================
//
// Move one sector's floor or ceiling, and take with it whatever is standing on
// it. Realms heights are world units one for one (rothmap.cpp reads floorHeight
// straight into the plane), so there is no scaling here.
//
// THE PLAYER CARRY IS DELEGATED, NOT TRANSCRIBED, and this is the one place in
// the height effects where that is true. The original hands the per-frame budget
// to apply_cell_move_to_player and moves the player itself; GZDoom already does
// that for its own movers, through P_ChangeSector, including crushing and
// riders. Reproducing the original's version on top of that would fight it. So
// the ramp -- which sectors, how fast, to what limit, in what units -- is read
// from ROTH.C exactly, and only the "and the player goes up with it" step is the
// host engine's.
//
//==========================================================================

static void MoveSectorPlane(int si, bool ceiling, int16_t height)
{
	if (g.level == nullptr) return;
	if (si < 0 || (size_t)si >= g.map.sectors.size()) return;
	if ((size_t)si >= g.level->sectors.Size()) return;

	Sector &rs = g.map.sectors[si];
	const double oldZ = ceiling ? double(rs.ceilingHeight) : double(rs.floorHeight);
	const double newZ = double(height);
	if (oldZ == newZ) return;

	if (ceiling) rs.ceilingHeight = height; else rs.floorHeight = height;

	sector_t *sec = &g.level->sectors[si];
	const int which = ceiling ? sector_t::ceiling : sector_t::floor;
	if (ceiling) sec->ceilingplane.setD(newZ);
	else         sec->floorplane.setD(-newZ);
	sec->SetPlaneTexZ(which, newZ);
	P_ChangeSector(sec, 0, newZ - oldZ, which, false);
}

//==========================================================================
//
// cmd_change_height -- raw_commands.c:4483, RAW command base 0x07
// tick_change_height -- raw_commands.c:2448
//
// The sector version of 0x23, and the same record fields mean the same things:
// +0x07 is the speed, +0x0a and +0x0c the two limits, +0x0e the dwell, bit 0x04
// of +0x06 makes the step fixed point, and re-running a registered record flips
// the direction of the move already in flight.
//
// What is new is WHICH surface moves. Bit 0x01 of +0x06 picks the field: clear is
// the sector's +0x02, its FLOOR, and set is +0x00, its CEILING. One opcode
// therefore drives both lifts and closing ceilings.
//
// TWO THINGS ARE NOT REPRODUCED, and both are named rather than approximated:
//
//   The SOUND. The registrar optionally links an SFX node through +0x10 and the
//   exits start and stop it. There is no sound-node system on this side yet, so
//   the link is skipped and the moves are silent.
//
//   The PORTAL MERGE. Two of the original's four loop variants accumulate a flag
//   from apply_cell_move_to_player_portalcheck, and a completed sweep with that
//   flag set turns around immediately instead of going through the finalize.
//   There is no equivalent to accumulate here, so it is treated as never set,
//   which means those two variants always take the finalize. This is a KNOWN
//   deviation, not a reading of the original.
//
//==========================================================================

int ChangeHeight(Command *rec, int index)
{
	const uint8_t base = (uint8_t)(rec->opcode & 0x7f);

	if (rec->modifier & 0x21)                        // TOGGLE
	{
		if (rec->Word(0x0E) != 0) return CMD_NOTHING;
		if (!(rec->fireFlags & 0x20)) return CMD_NOTHING;
		Effect *eff = FindEffect(base, (uint16_t)index);
		if (eff == nullptr) return CMD_NOTHING;
		eff->flags5 ^= 0x80;
		rec->modifier ^= 0x02;
		SyncDisabled(rec);
		return CMD_ACTED;
	}

	Effect e;
	e.tick = base;
	e.record = (uint16_t)index;
	e.sectors = CollectGeometryGroup(rec->key, (rec->modifier & 0x04) != 0);
	if (e.sectors.empty()) return CMD_NOTHING;
	e.payload = 0;
	e.flags5 |= (rec->modifier & 0x02) ? 0 : 0x80;
	g.effects.push_back(e);
	rec->modifier |= 0x20;
	SyncDisabled(rec);
	return CMD_ACTED;
}

static bool TickChangeHeight(Effect &e)
{
	Command *rec = Rec(e.record);
	if (rec == nullptr) return true;

	if (e.flags5 & 0x40) return TickArmedCountdown(e);

	int32_t delta = ROTH_FRAME_TICKS_PER_TIC * (int32_t)rec->Byte(0x07);
	if (rec->fireFlags & 0x04)
	{
		delta += (int32_t)(e.payload & 0x3F);
		e.payload = (uint16_t)(uint8_t)delta;
		delta = (int32_t)((uint32_t)delta >> 6);
		if (delta == 0) return false;
	}

	// The original has no count==0 guard here: its member list is a do-while, so
	// an empty group would step member[0] regardless. There is no member[0] to
	// step on this side, so an empty group finishes instead of reading past.
	if (e.sectors.empty())
	{
		rec->modifier &= 0xDE;
		SyncDisabled(rec);
		return true;
	}

	const bool ceiling = (rec->fireFlags & 0x01) != 0;   // set -> +0x00, the ceiling
	const bool ascend  = (e.flags5 & 0x80) != 0;

	int32_t budget = ascend ? delta : -delta;
	const int32_t limit32 = 2 * (int32_t)(int16_t)rec->Word(ascend ? 0x0C : 0x0A);
	const int16_t limit = (int16_t)limit32;

	bool overshoot = false;
	for (int si : e.sectors)
	{
		if (si < 0 || (size_t)si >= g.map.sectors.size()) continue;
		const Sector &rs = g.map.sectors[si];
		const int16_t cur = ceiling ? rs.ceilingHeight : rs.floorHeight;

		if (ascend ? (cur >= limit) : (cur <= limit)) { overshoot = true; break; }

		const int16_t sum = (int16_t)(cur + (int16_t)budget);
		int16_t stored;
		if (ascend ? (sum <= limit) : (sum >= limit))
		{
			stored = sum;
		}
		else
		{
			budget -= (int32_t)(sum - limit);      // the surplus folds back
			stored = limit;
		}
		MoveSectorPlane(si, ceiling, stored);
	}

	if (overshoot) return TickHeightExit(e, rec);
	return false;                                  // see the portal-merge note
}

//==========================================================================
//
// Re-apply a sector's floor or ceiling flat from the Realms fields, after the
// logic has changed them. Deliberately a RECOMPUTE of everything the loader
// derives -- texture, scale, offsets -- rather than a patch of the one field
// that moved, because the swap changes the texture and its shift and its scale
// bits together and they are all derived from each other. rothmap.cpp:455 is the
// original of this arithmetic; if that changes, this has to change with it.
//
//==========================================================================

static void ApplySectorFlat(int si, bool isFloor)
{
	if (g.level == nullptr) return;
	if (si < 0 || (size_t)si >= g.map.sectors.size()) return;
	if ((size_t)si >= g.level->sectors.Size()) return;

	const Sector &rs = g.map.sectors[si];
	sector_t *sec = &g.level->sectors[si];

	const int which = isFloor ? sector_t::floor : sector_t::ceiling;
	const int index = isFloor ? rs.floorTexture : rs.ceilingTexture;
	const int shift = isFloor ? rs.FloorScaleShift() : rs.CeilingScaleShift();
	const int shx   = isFloor ? rs.floorShiftX : rs.ceilShiftX;
	const int shy   = isFloor ? rs.floorShiftY : rs.ceilShiftY;

	auto it = g.flatByIndex.find(index);
	if (it == g.flatByIndex.end())
	{
		g.unhandledOps[isFloor ? 0x0A : 0x0B]++;   // no flat registered for it
		return;
	}
	const FTextureID tex = it->second.second ? FNullTextureID() : it->second.first;

	// "Nothing here" becomes the sky, exactly as at load: a flat must draw
	// something or the sector renders hall of mirrors.
	sec->SetTexture(which, tex.isValid() ? tex : skyflatnum, false);

	const double unitsPerTexel = double(1 << shift);
	sec->SetXScale(which, 1. / unitsPerTexel);
	sec->SetYScale(which, 1. / unitsPerTexel);
	sec->SetXOffset(which,  shx * unitsPerTexel * 0.5);
	sec->SetYOffset(which, -shy * unitsPerTexel * 0.5);
}

//==========================================================================
//
// Re-apply a mid-platform to the control sector that renders it, after the logic
// has changed the platform record. A mid-platform is a slab: its TOP becomes the
// control sector's ceiling and its UNDERSIDE that sector's floor, which is why
// the opcodes that reach a platform treat top as a floor-ish thing and underside
// as a ceiling-ish thing -- you stand on one and look up at the other.
//
// A recompute of everything, for the same reason as ApplySectorFlat: heights,
// textures, scales and offsets are all derived together. rothmap.cpp:1018 is the
// original of this arithmetic, including its rule that a control sector NEVER
// takes the sky flat and that a missing face borrows the other's.
//
//==========================================================================

static void ApplyPlatform(int rothSector)
{
	if (g.level == nullptr) return;
	if (rothSector < 0 || (size_t)rothSector >= g.map.sectors.size()) return;

	auto cit = g.platformCtrl.find(rothSector);
	if (cit == g.platformCtrl.end()) return;
	if (cit->second < 0 || (size_t)cit->second >= g.level->sectors.Size()) return;

	const int pi = g.map.sectors[rothSector].platformIndex;
	if (pi < 0 || (size_t)pi >= g.map.platforms.size()) return;
	const MidPlatform &mp = g.map.platforms[pi];

	sector_t *cs = &g.level->sectors[cit->second];
	const double topZ = double(mp.topZ);
	const double undZ = double(mp.undersideZ);

	cs->SetPlaneTexZ(sector_t::ceiling, topZ);
	cs->SetPlaneTexZ(sector_t::floor, undZ);
	cs->floorplane.set(0., 0., 1., -undZ);
	cs->ceilingplane.set(0., 0., -1., topZ);

	auto look = [](int index) -> FTextureID
	{
		auto it = g.texByIndex.find(index);
		return it == g.texByIndex.end() ? FNullTextureID() : it->second;
	};
	const FTextureID topTex = look(mp.topTexture);
	const FTextureID undTex = look(mp.undersideTexture);
	const FTextureID slabTop = topTex.isValid() ? topTex : undTex;
	const FTextureID slabBot = undTex.isValid() ? undTex : topTex;
	cs->SetTexture(sector_t::ceiling, slabTop.isValid() ? slabTop : FNullTextureID(), false);
	cs->SetTexture(sector_t::floor,   slabBot.isValid() ? slabBot : FNullTextureID(), false);

	const double topScale = 1.0 / double(1 << ((mp.scales >> 4) & 3));
	const double undScale = 1.0 / double(1 << ((mp.scales >> 2) & 3));
	cs->SetXScale(sector_t::ceiling, topScale); cs->SetYScale(sector_t::ceiling, topScale);
	cs->SetXScale(sector_t::floor, undScale);   cs->SetYScale(sector_t::floor, undScale);
	cs->SetXOffset(sector_t::ceiling, double(mp.topShiftX));
	cs->SetYOffset(sector_t::ceiling, double(mp.topShiftY));
	cs->SetXOffset(sector_t::floor, double(mp.undersideShiftX));
	cs->SetYOffset(sector_t::floor, double(mp.undersideShiftY));
}

//==========================================================================
//
// cmd_change_floor_texture -- raw_commands.c:4356, RAW command bases 0x0a and 0x0b
// swap_cell_state_group_v1 / _v2 -- what their ticks actually do
//
// The same SWAP idea as the object texture, on a sector's flat: the record holds
// one appearance and the sector wears the other, and each application exchanges
// them. 0x0a is the FLOOR and 0x0b the CEILING, and the two collectors are exact
// mirrors -- v1 touches the sector's +0x08 texture and +0x12 shift pair, v2 the
// +0x06 texture and +0x10 pair.
//
// It is not only the picture. The SCALE bits move too, packed two at a time into
// the sector's flags (+0x0a) and the flip bits into the high byte of +0x16, with
// the sector's previous values captured back into the record's +0x07. So one
// swap can change a floor's texture, its tiling and its mirroring together, and
// swapping back restores all three.
//
// Member 0 is the one that EXCHANGES. Every other member in the group is then
// assigned member 0's new values -- they do not each swap with the record, so a
// group ends up uniform however it started. That is the original's behaviour, not
// a simplification.
//
// +0x06 bit 0x08 freezes the capture, turning the toggle into a one-way set, and
// bit 0x04 selects an ALT path that works through each member's sub-record at
// +0x18 -- the mid-platform -- instead of the sector itself. THE ALT PATH IS NOT
// IMPLEMENTED: mid-platform records are built into 3D floors at load and are not
// modelled as live state that an effect can reach, so those records are counted
// rather than approximated.
//
//==========================================================================

int ChangeFlatTexture(Command *rec, int index)
{
	const uint8_t base = (uint8_t)(rec->opcode & 0x7f);
	if (rec->modifier & 0x21) return CMD_NOTHING;     // already registered

	Effect e;
	e.tick = base;
	e.record = (uint16_t)index;
	e.sectors = CollectGeometryGroup(rec->key, false);   // the flat collector
	if (e.sectors.empty()) return CMD_NOTHING;
	g.effects.push_back(e);
	rec->modifier |= 0x20;
	SyncDisabled(rec);
	return CMD_ACTED;
}

static bool TickChangeFlatTexture(Effect &e, bool isFloor)
{
	Command *rec = Rec(e.record);
	if (rec == nullptr) return true;

	if (e.flags5 & 0x40) return TickArmedCountdown(e);
	if (e.sectors.empty()) return TickFinalize(e, rec, rec->Word(0x0E));

	const bool frozen = (rec->fireFlags & 0x08) != 0;

	// THE ALT PATH. Bit 0x04 sends the swap through each member's mid-platform
	// instead of the sector itself, and which FACE of the slab it lands on
	// mirrors the sector case exactly: v1, the floor variant, swaps the
	// platform's TOP (+0x06 texture, +0x0a shift) and v2, the ceiling variant,
	// its UNDERSIDE (+0x00 texture, +0x04 shift). Which is the right way round --
	// a slab's top is the floor you stand on and its underside the ceiling above
	// you.
	if (rec->fireFlags & 0x04)
	{
		const int first = e.sectors.empty() ? -1 : e.sectors[0];
		if (first < 0 || (size_t)first >= g.map.sectors.size())
			return TickFinalize(e, rec, rec->Word(0x0E));
		const int pi = g.map.sectors[first].platformIndex;
		if (pi < 0 || (size_t)pi >= g.map.platforms.size())
			return TickFinalize(e, rec, rec->Word(0x0E));   // no sub-record: the original returns
		MidPlatform &mp = g.map.platforms[pi];

		const uint8_t sMask = isFloor ? 0x30 : 0x0C;
		const int     sSh   = isFloor ? 4    : 2;
		const uint8_t fMask = isFloor ? 0x03 : 0x0C;
		const int     fSh   = isFloor ? 0    : 2;

		const uint16_t inTex   = rec->Word(0x0A);
		const uint16_t inShift = rec->Word(0x0C);
		const uint8_t  inPack  = rec->Byte(0x07);

		const uint16_t oldTex = isFloor ? mp.topTexture : mp.undersideTexture;
		const uint16_t oldShift = isFloor
			? (uint16_t)(mp.topShiftX | ((uint16_t)mp.topShiftY << 8))
			: (uint16_t)(mp.undersideShiftX | ((uint16_t)mp.undersideShiftY << 8));
		const uint8_t oldScales = mp.scales;
		const uint8_t oldPad    = mp.pad;

		if (!frozen)
		{
			rec->SetWord(0x0A, oldTex);
			rec->SetWord(0x0C, oldShift);
		}
		if (isFloor)
		{
			mp.topTexture = inTex;
			mp.topShiftX = (uint8_t)(inShift & 0xFF);
			mp.topShiftY = (uint8_t)(inShift >> 8);
		}
		else
		{
			mp.undersideTexture = inTex;
			mp.undersideShiftX = (uint8_t)(inShift & 0xFF);
			mp.undersideShiftY = (uint8_t)(inShift >> 8);
		}
		mp.scales = (uint8_t)((oldScales & (uint8_t)~sMask)
		          | (uint8_t)(((inPack & 0x03) << sSh) & sMask));
		mp.pad = (uint8_t)((oldPad & (uint8_t)~fMask)
		       | (uint8_t)((((inPack & 0x0C) >> 2) << fSh) & fMask));
		if (!frozen)
			rec->SetByte(0x07, (uint8_t)(((oldScales & sMask) >> sSh)
			                           | (((oldPad & fMask) >> fSh) << 2)));

		for (int si : e.sectors) ApplyPlatform(si);
		return TickFinalize(e, rec, rec->Word(0x0E));
	}

	// Which fields this variant moves. v1 is the floor, v2 the ceiling, and the
	// only differences are these offsets and the two bit positions.
	const uint8_t scaleMask = isFloor ? 0x30 : 0x0C;   // in Sector::flags
	const int     scaleSh   = isFloor ? 4    : 2;
	const uint8_t flipMask  = isFloor ? 0x03 : 0x0C;   // in flags2's high byte
	const int     flipSh    = isFloor ? 0    : 2;

	const int first = e.sectors[0];
	if (first < 0 || (size_t)first >= g.map.sectors.size()) return false;
	Sector &s0 = g.map.sectors[first];

	// Everything read before anything is written, as the original does.
	const uint16_t inTex   = rec->Word(0x0A);
	const uint16_t inShift = rec->Word(0x0C);
	const uint8_t  inPack  = rec->Byte(0x07);

	const uint16_t oldTex   = isFloor ? s0.floorTexture : s0.ceilingTexture;
	const uint16_t oldShift = isFloor
		? (uint16_t)(s0.floorShiftX | ((uint16_t)s0.floorShiftY << 8))
		: (uint16_t)(s0.ceilShiftX  | ((uint16_t)s0.ceilShiftY  << 8));
	const uint8_t oldFlags = s0.flags;
	const uint8_t oldHi    = (uint8_t)(s0.flags2 >> 8);

	if (!frozen)
	{
		rec->SetWord(0x0A, oldTex);
		rec->SetWord(0x0C, oldShift);
	}

	// Member 0 takes the record's appearance.
	if (isFloor)
	{
		s0.floorTexture = inTex;
		s0.floorShiftX = (uint8_t)(inShift & 0xFF);
		s0.floorShiftY = (uint8_t)(inShift >> 8);
	}
	else
	{
		s0.ceilingTexture = inTex;
		s0.ceilShiftX = (uint8_t)(inShift & 0xFF);
		s0.ceilShiftY = (uint8_t)(inShift >> 8);
	}
	s0.flags  = (uint8_t)((oldFlags & (uint8_t)~scaleMask)
	                      | (uint8_t)(((inPack & 0x03) << scaleSh) & scaleMask));
	const uint8_t newHi = (uint8_t)((oldHi & (uint8_t)~flipMask)
	                      | (uint8_t)((((inPack & 0x0C) >> 2) << flipSh) & flipMask));
	s0.flags2 = (uint16_t)((s0.flags2 & 0x00FF) | ((uint16_t)newHi << 8));

	if (!frozen)
		rec->SetByte(0x07, (uint8_t)(((oldFlags & scaleMask) >> scaleSh)
		                           | (((oldHi & flipMask) >> flipSh) << 2)));

	ApplySectorFlat(first, isFloor);

	// And every other member is ASSIGNED member 0's new values -- they do not
	// each swap with the record.
	for (size_t i = 1; i < e.sectors.size(); i++)
	{
		const int si = e.sectors[i];
		if (si < 0 || (size_t)si >= g.map.sectors.size()) continue;
		Sector &sn = g.map.sectors[si];
		if (isFloor)
		{
			sn.floorTexture = inTex;
			sn.floorShiftX = (uint8_t)(inShift & 0xFF);
			sn.floorShiftY = (uint8_t)(inShift >> 8);
		}
		else
		{
			sn.ceilingTexture = inTex;
			sn.ceilShiftX = (uint8_t)(inShift & 0xFF);
			sn.ceilShiftY = (uint8_t)(inShift >> 8);
		}
		ApplySectorFlat(si, isFloor);
	}

	return TickFinalize(e, rec, rec->Word(0x0E));
}

//==========================================================================
//
// THE SCROLL ACCUMULATOR, shared by both scroll ticks (raw_commands.c:1944 and
// 1976) and the reason a Realms texture can crawl instead of only sliding:
//
//     product = rate * step + carry;   delta = product >> 1;   carry = product & 1
//
// The rate is a SIGNED byte, so scrolling runs either way, and the halving with
// the carry kept means a rate of 1 advances one texel every other frame rather
// than one per frame. Each axis keeps its own carry, in the effect's +0x06 and
// +0x07.
//
//==========================================================================

struct ScrollDelta { uint8_t u, v; };

static ScrollDelta AdvanceScroll(Effect &e, const Command *rec)
{
	const int step = ROTH_FRAME_TICKS_PER_TIC;

	const int carryU = (e.payload & 0x0001) ? 1 : 0;          // +0x06 bit 0
	const int carryV = (e.payload & 0x0100) ? 1 : 0;          // +0x07 bit 0

	const int32_t prodU = (int32_t)(int8_t)rec->Byte(0x07) * step + carryU;
	const int32_t prodV = (int32_t)(int8_t)rec->Byte(0x08) * step + carryV;

	ScrollDelta d;
	d.u = (uint8_t)((uint32_t)prodU >> 1);
	d.v = (uint8_t)((uint32_t)prodV >> 1);

	e.payload = (uint16_t)(((uint32_t)prodU & 1u)
	                     | (((uint32_t)prodV & 1u) << 8));
	return d;
}

//==========================================================================
//
// Re-apply one texture-mapping record's shift to every sidedef that wears it.
// The loader's derivation is just offX = shiftX and offY = shiftY, negated when
// the record is pinned to the bottom of its piece (rothmap.cpp:775).
//
//==========================================================================

static void ApplyTexmapShift(int tmi)
{
	if (g.level == nullptr) return;
	if (tmi < 0 || (size_t)tmi >= g.map.textureMaps.size()) return;
	const TextureMap &tm = g.map.textureMaps[tmi];

	double offX = double(tm.shiftX);
	double offY = double(tm.shiftY);
	if (tm.flags & FF_PIN_BOTTOM) offY = -offY;

	auto faces = g.texmapToFaces.find(tmi);
	if (faces == g.texmapToFaces.end()) return;
	for (int fi : faces->second)
	{
		auto sd = g.faceToSide.find(fi);
		if (sd == g.faceToSide.end()) continue;
		if (sd->second < 0 || (size_t)sd->second >= g.level->sides.Size()) continue;
		side_t &side = g.level->sides[sd->second];
		for (int part = 0; part < 3; part++)
		{
			side.SetTextureXOffset(part, offX);
			side.SetTextureYOffset(part, offY);
		}
	}
}

//==========================================================================
//
// cmd_scroll_face_texture -- RAW command base 0x0f
// tick_scroll_face_texture -- raw_commands.c:1944
//
// Crawling wall art: the effect's members are texture-MAPPING records and the
// scroll lands on their +0x0a / +0x0b, which are the record's shiftX and shiftY.
// (ROTH.C's comment calls them u and v, which is the same pair under another
// name -- worth saying because this port had +0x0a on a face record labelled as
// collision flags, and these are mapping records, not faces.)
//
//==========================================================================

int ScrollFaceTexture(Command *rec, int index)
{
	const uint8_t base = (uint8_t)(rec->opcode & 0x7f);
	if (rec->modifier & 0x21) return CMD_NOTHING;

	Effect e;
	e.tick = base;
	e.record = (uint16_t)index;
	// Its members are the mapping records the key resolves to, which the reader
	// has already worked out as FACES -- take each one's mapping record.
	for (int fi : rec->faces)
		if (fi >= 0 && (size_t)fi < g.map.faces.size()
		    && g.map.faces[fi].textureMap >= 0)
			e.sectors.push_back(g.map.faces[fi].textureMap);
	if (e.sectors.empty()) return CMD_NOTHING;
	e.payload = 0;
	g.effects.push_back(e);
	rec->modifier |= 0x20;
	SyncDisabled(rec);
	return CMD_ACTED;
}

static bool TickScrollFaceTexture(Effect &e)
{
	Command *rec = Rec(e.record);
	if (rec == nullptr) return true;

	const ScrollDelta d = AdvanceScroll(e, rec);
	for (int tmi : e.sectors)          // mapping-record indices, despite the name
	{
		if (tmi < 0 || (size_t)tmi >= g.map.textureMaps.size()) continue;
		TextureMap &tm = g.map.textureMaps[tmi];
		tm.shiftX = (uint8_t)(tm.shiftX + d.u);
		tm.shiftY = (uint8_t)(tm.shiftY + d.v);
		ApplyTexmapShift(tmi);
	}
	return false;                      // a scroll runs until something stops it
}

//==========================================================================
//
// cmd_scroll_sector_texture -- raw_commands.c:4371, RAW command base 0x0e
// tick_scroll_sector_texture -- raw_commands.c:1976
//
// The same accumulator on a sector's flats, and the record's +0x06 says WHICH
// surfaces move -- it is a set of bits, dispatched in order, each subtracted as
// it is handled so the walk stops once they are exhausted:
//
//     bit 0  the sector's +0x12 / +0x13 -- the FLOOR shift
//     bit 1  the sector's +0x10 / +0x11 -- the CEILING shift
//     bit 2  through the mid-platform at +0x18, its +0x0a / +0x0b
//     bit 3  through the mid-platform at +0x18, its +0x04 / +0x05
//
// So one record can crawl a floor and a ceiling together. BITS 2 AND 3 ARE NOT
// IMPLEMENTED: they reach through the mid-platform sub-record, which becomes a
// 3D floor at load and is not live state an effect can address. They are counted.
//
//==========================================================================

int ScrollSectorTexture(Command *rec, int index)
{
	const uint8_t base = (uint8_t)(rec->opcode & 0x7f);
	if (rec->modifier & 0x21) return CMD_NOTHING;

	Effect e;
	e.tick = base;
	e.record = (uint16_t)index;
	e.sectors = CollectGeometryGroup(rec->key, false);
	if (e.sectors.empty()) return CMD_NOTHING;
	e.payload = 0;
	g.effects.push_back(e);
	rec->modifier |= 0x20;
	SyncDisabled(rec);
	return CMD_ACTED;
}

static bool TickScrollSectorTexture(Effect &e)
{
	Command *rec = Rec(e.record);
	if (rec == nullptr) return true;

	const ScrollDelta d = AdvanceScroll(e, rec);
	const uint8_t which = rec->fireFlags;

	for (int si : e.sectors)
	{
		if (si < 0 || (size_t)si >= g.map.sectors.size()) continue;
		Sector &rs = g.map.sectors[si];

		uint8_t cl = which;
		if (cl & 0x01)
		{
			rs.floorShiftX = (uint8_t)(rs.floorShiftX + d.u);
			rs.floorShiftY = (uint8_t)(rs.floorShiftY + d.v);
			ApplySectorFlat(si, true);
			cl -= 1; if (cl == 0) continue;
		}
		if (cl & 0x02)
		{
			rs.ceilShiftX = (uint8_t)(rs.ceilShiftX + d.u);
			rs.ceilShiftY = (uint8_t)(rs.ceilShiftY + d.v);
			ApplySectorFlat(si, false);
			cl -= 2; if (cl == 0) continue;
		}
		// Bits 2 and 3 reach the sector's mid-platform: bit 2 its TOP shift
		// (+0x0a / +0x0b) and bit 3 its UNDERSIDE (+0x04 / +0x05). The original
		// stops the whole dispatch if the sector has no sub-record.
		if (cl & 0x0C)
		{
			const int pi = rs.platformIndex;
			if (pi < 0 || (size_t)pi >= g.map.platforms.size()) continue;
			MidPlatform &mp = g.map.platforms[pi];
			if (cl & 0x04)
			{
				mp.topShiftX = (uint8_t)(mp.topShiftX + d.u);
				mp.topShiftY = (uint8_t)(mp.topShiftY + d.v);
				cl -= 4; if (cl == 0) { ApplyPlatform(si); continue; }
			}
			if (cl & 0x08)
			{
				mp.undersideShiftX = (uint8_t)(mp.undersideShiftX + d.u);
				mp.undersideShiftY = (uint8_t)(mp.undersideShiftY + d.v);
			}
			ApplyPlatform(si);
		}
	}
	return false;
}

//==========================================================================
//
// Re-apply a mapping record's three texture slots AND its shift to every sidedef
// that wears it. The shift-only version above is enough for a scroll; a swap
// changes the pictures too.
//
//==========================================================================

static void ApplyTexmapFull(int tmi)
{
	if (g.level == nullptr) return;
	if (tmi < 0 || (size_t)tmi >= g.map.textureMaps.size()) return;
	const TextureMap &tm = g.map.textureMaps[tmi];

	auto faces = g.texmapToFaces.find(tmi);
	if (faces == g.texmapToFaces.end()) return;

	const int slot[3] = { tm.midTexture, tm.upperTexture, tm.lowerTexture };
	const int part[3] = { side_t::mid, side_t::top, side_t::bottom };

	for (int fi : faces->second)
	{
		auto sd = g.faceToSide.find(fi);
		if (sd == g.faceToSide.end()) continue;
		if (sd->second < 0 || (size_t)sd->second >= g.level->sides.Size()) continue;
		side_t &side = g.level->sides[sd->second];
		for (int k = 0; k < 3; k++)
		{
			auto t = g.texByIndex.find(slot[k]);
			if (t == g.texByIndex.end() || !t->second.isValid()) continue;
			side.SetTexture(part[k], t->second);
		}
	}
	ApplyTexmapShift(tmi);
}

//==========================================================================
//
// cmd_change_face_texture_adv -- raw_commands.c:4721, RAW command base 0x0c
// swap_cell_state_linked_pair -- raw_commands.c, what its tick does
//
// The "extended form" of 0x34, and a much bigger swap. 0x34 writes ONE texture
// slot; this exchanges a wall's whole appearance with the record: all three
// slots, the mapping flags, the shift pair, and some of the face's own bits.
//
// It works on a LINKED PAIR. Cell A is a face and cell B is the mapping record
// that face points at through its +0x04 -- so the record's fields swap against
// the mapping while the face's +0x0a exchanges only bits 0x83, keeping 0x7c in
// place. One command therefore changes what a wall looks like and whether you can
// walk through it, together, and swapping back restores both.
//
// THIS OPCODE NAMES ITS FACE BY RAW OFFSET, alone among the geometry opcodes:
// the original does `geom + word[rec+8]` rather than searching for an id. Offsets
// in this format are absolute file positions used as foreign keys, so the
// reader's face-offset map answers it directly -- and the load report counts how
// many 0x0c keys actually land on a face, so the reading is checked against the
// retail data rather than assumed.
//
// ONLY THE REGISTER PATH IS HERE. When +0x06 bit 0x08 is set, or the key is 0,
// the original takes an immediate path that ROTH.C itself cannot reproduce: it
// bridges the original code because the block contains an irreducible read of an
// undefined register (raw_commands.c:4716). That path is counted, not invented.
//
//==========================================================================

int ChangeFaceTextureAdv(Command *rec, int index)
{
	const uint8_t base = (uint8_t)(rec->opcode & 0x7f);

	if ((rec->fireFlags & 0x08) || rec->key == 0)
	{
		g.unhandledOps[base]++;          // the bridged undefined-behaviour path
		return CMD_NOTHING;
	}
	if (rec->modifier & 0x21) return CMD_NOTHING;

	Effect e;
	e.tick = base;
	e.record = (uint16_t)index;
	g.effects.push_back(e);
	rec->modifier |= 0x20;
	SyncDisabled(rec);
	return CMD_ACTED;
}

static bool TickChangeFaceTextureAdv(Effect &e)
{
	Command *rec = Rec(e.record);
	if (rec == nullptr) return true;

	if (e.flags5 & 0x40) return TickArmedCountdown(e);

	auto fit = g.map.faceByOffset.find((uint32_t)rec->key);
	if (fit == g.map.faceByOffset.end() || (size_t)fit->second >= g.map.faces.size())
	{
		g.unhandledOps[e.tick]++;        // the key named no face
		return TickFinalize(e, rec, rec->Word(0x0E));
	}

	Face &fa = g.map.faces[fit->second];

	// Cell A: exchange bits 0x83 of the face's own byte, keeping 0x7c.
	const uint8_t av = (uint8_t)(fa.collisionFlags & 0xFF);
	const uint8_t held = rec->Byte(0x14);
	rec->SetByte(0x14, av);
	fa.collisionFlags = (uint16_t)((fa.collisionFlags & 0xFF00)
	                  | (uint8_t)((av & 0x7C) | (held & 0x83)));

	// Cell B: the mapping record the face points at. Every field exchanges.
	const int tmi = fa.textureMap;
	if (tmi < 0 || (size_t)tmi >= g.map.textureMaps.size())
		return TickFinalize(e, rec, rec->Word(0x0E));
	TextureMap &tm = g.map.textureMaps[tmi];

	uint16_t w;
	w = tm.midTexture;   tm.midTexture   = rec->Word(0x0A); rec->SetWord(0x0A, w);
	w = tm.upperTexture; tm.upperTexture = rec->Word(0x10); rec->SetWord(0x10, w);
	w = tm.lowerTexture; tm.lowerTexture = rec->Word(0x12); rec->SetWord(0x12, w);

	const uint8_t f = tm.flags;
	tm.flags = rec->Byte(0x07);
	rec->SetByte(0x07, f);

	const uint16_t shift = (uint16_t)(tm.shiftX | ((uint16_t)tm.shiftY << 8));
	const uint16_t inShift = rec->Word(0x0C);
	tm.shiftX = (uint8_t)(inShift & 0xFF);
	tm.shiftY = (uint8_t)(inShift >> 8);
	rec->SetWord(0x0C, shift);

	ApplyTexmapFull(tmi);
	return TickFinalize(e, rec, rec->Word(0x0E));
}

//==========================================================================
//
// The pool walk -- raw_commands.c:3307.
//
// Every effect gets its per-frame handler, and a handler reporting "finished" is
// unlinked and freed. The original re-dereferences the node on the not-finished
// path because a handler may relocate the heap chunk; that is DOS heap
// discipline and does not survive the port. What DOES survive is that a handler
// can register another effect while the walk is in progress, so this iterates by
// index and compacts afterwards rather than holding an iterator across a
// dispatch.
//
//==========================================================================

static void TickEffects()
{
	for (size_t i = 0; i < g.effects.size(); i++)
	{
		if (g.effects[i].finished) continue;

		bool done = false;
		switch (g.effects[i].tick)
		{
		case 0x1D: done = TickChangeLighting(g.effects[i]); break;
		case 0x02: done = TickLightSwitch(g.effects[i]); break;
		case 0x11: done = TickFlashLights(g.effects[i]); break;
		case 0x23: done = TickChangeObjectHeight(g.effects[i]); break;
		case 0x0D: done = TickChangeObjectTexture(g.effects[i]); break;
		case 0x07: done = TickChangeHeight(g.effects[i]); break;
		case 0x0A: done = TickChangeFlatTexture(g.effects[i], true);  break;
		case 0x0B: done = TickChangeFlatTexture(g.effects[i], false); break;
		case 0x0E: done = TickScrollSectorTexture(g.effects[i]); break;
		case 0x0F: done = TickScrollFaceTexture(g.effects[i]); break;
		case 0x0C: done = TickChangeFaceTextureAdv(g.effects[i]); break;
		default:
			// A registrar put this here but its tick is not written yet. Dropping
			// it is the honest outcome: left in the pool it would be walked every
			// frame forever without ever doing anything.
			g.unhandledOps[g.effects[i].tick]++;
			done = true;
			break;
		}
		if (done) g.effects[i].finished = true;
	}

	for (size_t i = 0; i < g.effects.size(); )
	{
		if (g.effects[i].finished) g.effects.erase(g.effects.begin() + i);
		else i++;
	}
}

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
	case 0x02: case 0x07: case 0x0A: case 0x0B: case 0x0D: case 0x0C: case 0x0E: case 0x0F: case 0x11: case 0x1D: case 0x2F: case 0x34: case 0x23: case 0x24: case 0x36: case 0x38: case 0x3B: case 0x3E: case 0x40:
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
	case 0x34: return ChangeFaceTexture(*rec);
	case 0x1D: return ChangeLighting(rec, index);
	case 0x02: return LightSwitch(rec, index);
	case 0x11: return FlashLights(rec, index);
	case 0x24: return RotateObject(rec);
	case 0x23: return ChangeObjectHeight(rec, index);
	case 0x0D: return ChangeObjectTexture(rec, index);
	case 0x07: return ChangeHeight(rec, index);
	case 0x0A: case 0x0B: return ChangeFlatTexture(rec, index);
	case 0x0E: return ScrollSectorTexture(rec, index);
	case 0x0F: return ScrollFaceTexture(rec, index);
	case 0x0C: return ChangeFaceTextureAdv(rec, index);

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
	g.texmapToFaces.clear();
	for (size_t fi = 0; fi < g.map.faces.size(); fi++)
	{
		const int tmi = g.map.faces[fi].textureMap;
		if (tmi >= 0) g.texmapToFaces[tmi].push_back((int)fi);
	}
	g.sideToFace.clear();
	for (auto &kv : g.faceToSide) g.sideToFace[kv.second] = kv.first;

	// Bind every trigger to the geometry it watches. The reader has already
	// resolved each key to a sector and/or a set of faces; this only sorts them
	// by what kind of event they wait for.
	int faceBound = 0, sectorBound = 0, unbound = 0;
	for (const Command &c : g.map.commands)
	{
		if (!c.isTrigger || c.disabled) continue;
		if (c.chainStart == 0) continue;

		if (IsFaceTrigger(c.opcode))
		{
			if (c.faces.empty()) { unbound++; continue; }
			for (int fi : c.faces) g.byFace[fi].push_back(c.chainStart);
			faceBound++;
		}
		else if (IsSectorTrigger(c.opcode))
		{
			if (c.sector < 0) { unbound++; continue; }
			g.bySector[c.sector].push_back(c.chainStart);
			sectorBound++;
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
		log->Line("  triggers bound   %d face-keyed, %d sector-keyed", faceBound, sectorBound);
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
	g.platformCtrl.clear();
	g.flatByIndex.clear();
	g.effects.clear();
	g.texByIndex.clear();
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

void RegisterTexture(int rothIndex, FTextureID tex)
{
	if (rothIndex >= 0 && tex.isValid()) g.texByIndex[rothIndex] = tex;
}

void RegisterFlat(int rothIndex, FTextureID tex, bool isSky)
{
	if (rothIndex >= 0) g.flatByIndex[rothIndex] = std::make_pair(tex, isSky);
}

void RegisterPlatformControl(int rothSector, int ctrlSector)
{
	if (rothSector >= 0 && ctrlSector >= 0) g.platformCtrl[rothSector] = ctrlSector;
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

void FireSectorTriggers(sector_t *sec, AActor *who)
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

	// The active-effect pool: the per-frame half of every registrar opcode.
	TickEffects();

	DrainDeferred();
	ApplyPendingWarp();
}

} // namespace roth
