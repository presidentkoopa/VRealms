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

#include "maploader/roth_portalplan.h"   // rothplan::SectorAt, for RealmsToEngineXY
#include "actorinlines.h"                 // AActor::PosRelative
#include "roth_runtime.h"
#include "roth_commands.h"
#include "roth_cmdcore.h"     // counters, texture oddments, player effects, particles
#include "roth_raw.h"
#include "roth_surface.h"
#include "roth_log.h"
#include "roth_dbase100.h"
#include "roth_gamecore.h"
#include "roth_itemcmds.h"    // 0x27/0x29/0x2a/0x2b/0x42, shared with tools/gameproof
#include "roth_gameflow.h"     // new game, travel, per-map state, saves, death
#include "roth_menus.h"        // the main menu, options, slot lists, death screen
#include "roth_install.h"
#include "roth_sound.h"       // Realms' own sound: effects, nodes, doors, music, speech
#include "roth_interface.h"   // the dialogue window, the inventory screen, pickups
#include "roth_movie.h"       // the GDV player: movies the flow asks for
#include "roth_actors_engine.h"   // objects, creatures, bullets, player damage (roth_actors.h)

#include <map>
#include <vector>
#include <string>
#include <string.h>
#include <stdlib.h>

#include "g_levellocals.h"
#include "p_3dfloors.h"
#include "g_level.h"
#include "gamedata/g_mapinfo.h"
#include "p_spec.h"
#include "playsim/p_local.h"
#include "roth_objects.h"
#include "roth_platforms.h"      // roth_platforms: moving sectors as platform actors
#include "roth_platformplan.h"   // which vertices cmd_move_sector takes
#include "actor.h"
#include "d_player.h"
#include <math.h>
#include "playsim/po_man.h"
#include "texturemanager.h"   // ReapplyWallFace: a swapped-in picture's size
#include "p_trace.h"       // Trace, for the floor-click probe in UseFlat
#include "printf.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "v_font.h"
#include "serializer.h"    // SerializeRealmsGlobals
#include "filesystem.h"    // FResourceFile, for the save slots' names
#include "savegamemanager.h"   // G_BuildSaveName
#include "g_game.h"        // G_SaveGame, G_LoadGame
#include "cmdlib.h"        // FileExists
#include "menustate.h"     // menuactive
#include "d_eventbase.h"   // event_t, for FlowResponder
#include "keydef.h"
#include <time.h>
#include "gi.h"            // gameinfo.ConfigName, for the start map

// Declared in p_map.cpp at GLOBAL scope, next to the use path it reports on.
// It cannot live in namespace roth beside roth_trigger_debug, because p_map.cpp
// is where the use ray is built and a namespaced cvar does not link from there
// -- the same reason the comment above its definition gives.
EXTERN_CVAR(Bool, roth_useray_debug)

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

//==========================================================================
//
// The game core, and the engine's side of it.
//
// roth::GameCore (roth_gamecore.h) owns the DBASE100 interpreter, the
// inventory and their shared generator, with no engine headers. It is
// attached ONCE, on the first level that finds DBASE100 loaded, because its
// state is the game's and not the map's: flags and items outlive a level.
//
// Once it is attached, THE FLAG BITMAP ABOVE IS NO LONGER THE FLAGS. The
// interpreter's bitmap is, sized from DBASE100's own header as the original
// sizes it (game_core.c:420-432), and RAW 0x26/0x28 and DBASE100 ops
// 0x01/0x04/0x0d read and write the same bits, as they do in ROTH.C. The
// array stays only as the fallback for an install with no DBASE100.
//
// CoreHost is everything the interpreter asks of a system that is not built
// yet. Text is real: read_next_dialogue_line (dbase100.c:371-403) reads an
// 8-byte header { voice clip u32, length u16, colour u8 } and then the text
// from DBASE400.DAT. Its timed-message cache and the voice clip are NOT
// ported -- the cache only saves a file seek, the voice is sound (M8). What
// the original draws in a dialogue window is, for now, printed to the
// console: enough to see a conversation happen and to stop the hub menus
// looping on an empty line (HANDOFF_LANE_GAMELAYER_2026-11-11.md section 5).
// Everything else is counted by name, so a missing system is a number in the
// load report and never a silent success.
//
// NOT COMPILED BY MSVC in the lane that wrote it. Syntax-checked with g++
// against the engine headers (tools/roth_syntaxcheck.sh), never linked, never run.
//
//==========================================================================

// The interpreter's two calls back into the map's own command chains
// (DBASE100 ops 0x23 and 0x36). Defined after Fire, which they run.
int32_t HostResetCommandChainNoSource(uint16_t index);
int32_t HostRunObjectCommandsById(uint16_t id);

CVAR(Bool, roth_newgame_on_start, true, 0)
CVAR(Bool, roth_intro_movies, false, 0)   // the boot's GREMLOGO + INTRO; off until a release build (owner)

void CmdHealHealth(uint32_t amount, uint32_t max);
void CmdZeroHealth();
void CmdPullHealth();
void CmdHealthFromFlow();

class CoreHost : public FlowInterpSink   // movies, images, health: the game flow (roth_gameflow.h)
{
public:
	std::map<std::string, int> calls;       // what was asked for and not built

	// The dialogue window, its text and its speech, and the inventory screen
	// the interpreter asks about: roth_interface (rules in roth_dialogue and
	// roth_invscreen, differential-tested against the original).
	uint32_t ReadNextDialogueLine(uint8_t *dest, uint32_t maxlen, uint32_t voiceOffset, uint32_t flag) override
	{ return InterfaceReadNextDialogueLine(dest, maxlen, voiceOffset, flag); }
	uint32_t Dbase100OpenDialogueWindow(const uint8_t *t, uint32_t bytes) override { return InterfaceOpenDialogueWindow(t, bytes); }
	uint32_t Dbase100OpenDialogueWindowAlt(const uint8_t *t, uint32_t bytes) override { return InterfaceOpenDialogueWindowAlt(t, bytes); }
	void DialogueVoiceForceEnd(uint32_t eax, uint32_t edx) override { InterfaceDialogueVoiceForceEnd(eax, edx); }
	int32_t InventoryPanelOpen() override { return InterfaceInventoryPanelOpen(); }
	int32_t InspectPopupActive() override { return InterfaceInspectPopupActive(); }
	void ArmInventoryPanelExit() override { InterfaceArmPanelExit(); }
	void SetLatch80b2c() override { InterfaceRequestPanelClose(); }

	// play_sound_effect (audio.c:758). The id is a sample row: an action
	// record carries one directly, and an item record's key was rewritten to
	// one when the bank was loaded (resolve_dbase100_sound_ids, audio.c:1898).
	uint32_t PlaySoundEffect(uint32_t id, uint32_t param) override { return SoundPlayEffect(id, param) ? 1u : 0u; }
	// The music: DBASE100 ops 0x1a and 0x1b, and nothing else, change it.
	uint32_t LoadDbase300Chunk(uint32_t index) override { return MusicLoadChunk(index); }
	void ProcessAudioSequenceChunk() override { MusicProcess(); }
	uint32_t CurrentDbase300ChunkId() override { return MusicCurrentChunk(); }
	// The byte at 0x71124. The music slider is the engine's own volume here,
	// so the song is asked for at full (0x7f, the clamp at audio.c:1880).
	uint8_t MusicMasterVolume() override { return 0x7f; }
	int32_t ResetCommandChainNoSource(uint16_t index) override { return HostResetCommandChainNoSource(index); }
	int32_t RunObjectCommandsById(uint16_t id) override { return HostRunObjectCommandsById(id); }
	// ONE health, as in the original (g_player_health 0x8a0f0). The game flow
	// owns it (death sequence, maximum, saves); the command core keeps a mirror
	// for the RAW damage opcodes, synced in CmdPullHealth/CmdPushHealth. A
	// DBASE100 heal or kill goes through the flow first (it also sets the
	// no-death-wait flag on a kill), then the core and the engine follow.
	void AdvanceClamp8a0f0(uint32_t amount, uint32_t max) override { CmdPullHealth(); FlowInterpSink::AdvanceClamp8a0f0(amount, max); CmdHealthFromFlow(); }
	void ZeroHealthAndFlagDeath() override { CmdPullHealth(); FlowInterpSink::ZeroHealthAndFlagDeath(); CmdHealthFromFlow(); calls["death"]++; }
};

GameCore g_core;
CoreHost g_coreHost;

// What the flow asks of the engine. A movie is queued for the GDV player
// (roth_movie.h, decoder roth_gdv.h) and plays from the next level tic as a
// blocking cutscene; every state change around it is the flow's own (the
// gallery stamp, the mode save and restore and the overlay latch:
// roth_gameflow.cpp, PlayRecordGdvCutscene). The full-screen still (op 0x0e)
// has no viewer yet; it is counted in the core host's `calls`, so a missing
// picture is a number, not silence.
class FlowEngineHost : public FlowHost
{
public:
	void PlayMovie(const char *name, uint32_t subOffset, int32_t subSize) override
	{
		MovieQueue(name, subOffset, subSize);       // gdv_cutscene.c:2840
	}
	void ShowImage(uint32_t id) override
	{
		g_coreHost.calls["fullscreen image not shown"]++;
		Printf("Realms: full-screen image DBASE200 +0x%x -- not shown, no viewer yet\n", id);
	}
};

GameFlow g_flow;
FlowEngineHost g_flowHost;
// Every map left this session, by name: the port's <MAP>.TMP for the command
// records and their queues (map_load.c:694-807). The world half -- planes,
// light, textures, doors, props -- is the engine's own hub snapshot (the
// cluster is a hub in vrealms_iwad/MAPINFO). Cleared by a new game, as
// delete_temp_files clears the .TMP files (game_core.c:449).
std::map<std::string, MapLogicState> g_mapStates;
// A death restarted the game: the next level runs the new-game block.
bool g_flowNewGamePending = false;
// The next level shows the main menu instead (the boot's, or after a death:
// game_core.c:806-816, 1006-1012).
bool g_flowMenuPending = false;
// A restart has been asked for and the level is about to change: the death
// check must not run again on the level that is going away.
bool g_flowRestarting = false;
// The slot a load was asked for, so the load can record it (savegame.c:1497).
int g_flowPendingLoadSlot = -1;
// The menus (roth_menus.h); drawn and pointed at by roth_interface.
RealmsMenus g_menus;
// The new-game block, wherever it is asked for (defined beside the death tick).
void RunNewGameBlock();
// The map ROTH.RES lists first (game_core.c:1062-1063); a death returns here.
std::string g_flowDefaultMap;
// g@0x8a26c: set by the first 0x2b of a chain, cleared by the chain's tail
// (raw_commands.c:4701-4704, 1313-1314).
bool g_dialogueLatch = false;

bool FlagIsSet(uint16_t id)
{
	// test_dbase100_record_flag: non-zero when set (raw_commands.c:79-86).
	if (g_core.IsAttached()) return g_core.Interp().TestDbase100RecordFlag(id) != 0;
	const int byteIndex = (int)id >> 3;
	if (byteIndex < 0 || byteIndex >= (int)sizeof(g_progressFlags)) return false;
	return (g_progressFlags[byteIndex] & (1u << (id & 7))) != 0;
}

// test_set / test_clear report "acted" only when the bit actually CHANGED.
bool FlagTestSet(uint16_t id)
{
	// dbase100_bitmap_test_set: 0 when already set (renderer.c:11528-11539).
	if (g_core.IsAttached()) return g_core.Interp().Dbase100BitmapTestSet(id) != 0;
	const int byteIndex = (int)id >> 3;
	if (byteIndex < 0 || byteIndex >= (int)sizeof(g_progressFlags)) return false;
	const uint8_t mask = (uint8_t)(1u << (id & 7));
	if (g_progressFlags[byteIndex] & mask) return false;
	g_progressFlags[byteIndex] |= mask;
	return true;
}

bool FlagTestClear(uint16_t id)
{
	// dbase100_bitmap_test_clear: 0 when already clear (renderer.c:11543-11554).
	if (g_core.IsAttached()) return g_core.Interp().Dbase100BitmapTestClear(id) != 0;
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

	// The sound node a height mover links (chunk[+0xc], with chunk[+5] bit
	// 0x01 for "has one" and 0x02 for "started"; cmd_change_height,
	// raw_commands.c:4498-4505). -1 when the record names none.
	int  sfxNode = -1;
	bool sfxStarted = false;

	// The members of an OBJECT effect. Sectors go in `sectors` above; these are
	// the (sector, index) pairs of the props a command named.
	std::vector<ObjectRef> objects;

	// 0x09 move-sector only: chunk[+0x0e..+0x15], the sector's four vertices in
	// ASCENDING order of the coordinate that moves, as indices into
	// Map::vertices. The tick tests [3] going up and [0] going down.
	int moveVerts[4] = { -1, -1, -1, -1 };
};

// TrigEvent -- what makes a trigger fire, as opposed to what it is bound to --
// is declared in roth_runtime.h, because the dispatch sites outside this file
// have to name an event when they call in. The derivation and the GAME_core.md
// 5.2 citations are at EventForOpcode below.

// One trigger bound to one piece of geometry: the chain to run, and the single
// event that runs it. Before this carried an event, every chain on a given
// sidedef answered every way the player could touch it.
struct Binding
{
	uint16_t  chain = 0;                  // 1-based command index
	TrigEvent event = TrigEvent::None;
	uint8_t   opcode = 0;                 // kept for the load report only
	uint8_t   fireFlags = 0;              // the record's +0x06: the facing mask
	uint8_t   subFlags = 0;               // the record's +0x07: the 0x13 variant
	uint16_t  sound = 0;                  // the record's +0x0a: the chain's sound, plus one
	uint16_t  record = 0;                 // the trigger record ITSELF, 1-based (chain is its link)
};

// Which link test an 0x13 record wants, from its +0x07. See NotifyPlayerSector
// for the derivation (mark_geometry_records_by_id, raw_commands.c:4994-5005).
enum class LinkVariant : uint8_t
{
	Water = 0,   // marker 0x40: inside the link only while at or below the floor
	Plain,       // marker 0x80: no Z test at all
	Linked,      // marker 0xc0: standing on the linked mid-platform's top
};

LinkVariant VariantOf(uint8_t subFlags)
{
	if (subFlags & 1) return LinkVariant::Plain;
	if (subFlags & 2) return LinkVariant::Linked;
	return LinkVariant::Water;
}

const char *VariantName(LinkVariant v)
{
	switch (v)
	{
	case LinkVariant::Plain:  return "plain";
	case LinkVariant::Linked: return "linked platform";
	default:                  return "water (Z)";
	}
}

//==========================================================================
//
// THE FACING MASK at a trigger record's +0x06, read out of ROTH.C.
//
// fire_sector_trigger (E/raw_commands.c:2876-2878) does exactly this:
//
//     t   = (g_player_angle - 0x40) & 0x1ff
//     bit = 1 << ((t >> 7) & 7)
//     if (!(f6 & bit)) { ...fire... }
//
// So it is the PLAYER'S FACING, not the direction of travel; the angle is in
// 512 units to the turn; subtracting 0x40 rotates the boundaries by 45 degrees
// so the four 128-unit quadrants are centred on the cardinals rather than
// straddling them; and -- the part that is easy to get backwards -- a SET bit
// FORBIDS that quadrant. An all-clear mask fires from anywhere, which is why
// ignoring the mask entirely looked right for so long: most records have one.
//
// Only bits 0-3 are facing. The upper bits are unrelated and are read after a
// chain runs: 0x40 asks for the LEAVE refire, 0x20 drives the re-find latch,
// 0x10 gates the spent bit. See NotifyPlayerSector for what is and is not
// implemented of those.
//
// THIS RULE IS FOR THE SECTOR-ENTER CHANNEL ONLY. DO NOT REUSE IT.
//
// +0x06 means different things per channel, and the three readings do not even
// share a polarity. Assuming they did is what put this function in the floor
// path, where it silently suppressed a real trigger.
//
//   0x13, enter sector      (fire_sector_trigger:2876)
//       index computed from the PLAYER ANGLE, (angle - 0x40) >> 7, four
//       quadrants; a SET bit BLOCKS. This function.
//
//   0x18/0x32, a wall face  (dispatch_entry_command_trigger:3029)
//       `record[+6] & dir_mask1[byte[pick+0x1a] & 3]`, where
//       dir_mask1[4] = { 0x01, 0x04, 0x02, 0x01 } (:3012). The index comes from
//       the PICK, not from the player's angle, and a SET bit ALLOWS -- the
//       opposite sense. Index 3 repeats index 0, so it is three directions, not
//       four. NOT IMPLEMENTED.
//
//   0x19/0x31, a floor      (dispatch_entry_command_trigger:3068)
//       NO direction mask at all. Bit 0 instead selects WHICH SURFACE:
//       `(record[+6] & 1) ? (type == 8) : (type == 2)`, the mid-platform top
//       versus the sector floor. Implemented in UseFlat.
//       The category-B twin (:3100) uses a third table, dir_mask2 = { 1, 2, 4,
//       8 }, with four distinct bits. NOT IMPLEMENTED.
//
// Also unimplemented on both click channels: the AUTHORED BOUNDING BOX. A
// record whose +0x0c is non-zero additionally requires the player's x/z inside
// [+0x0c..+0x0e] x [+0x10..+0x12] (:3034-3038, :3075-3079). Records with it
// unset fire from anywhere, which is most of them.
//
//==========================================================================

//==========================================================================
//
// WHICH PART OF A WALL, and why +0x06 on a FACE record is not a direction mask.
//
// dispatch_entry_command_trigger gates a face trigger on
// `record[+6] & dir_mask1[byte[pick+0x1a] & 3]`, with
// dir_mask1 = { 0x01, 0x04, 0x02, 0x01 } (E/raw_commands.c:3012). ROTH.C's
// author named it dir_mask and called the index "the approach", and that is
// wrong: tracing the index to where it is written shows it is the WALL SPAN
// KIND, not a direction.
//
// The pick record is the deferred-surface descriptor at 0x90a48, and its +0x1a
// is 0x90a62 == g_subpass_reflect_param_b + 0x6, which exit_f_writeout fills
// from byte[g_turn_view_scale_state + 2] (E/renderer.c:4500). That byte is set
// by the span builders, one value per part of the wall:
//
//   0  the main face span          build_world_face_spans, renderer.c:8716
//   1  the TOP edge span           renderer.c:8673
//   2  the BOTTOM edge span        renderer.c:8694
//   3  mid base+overlay multipass  renderer.c:8772
//
// Through dir_mask1 that gives the bits:
//
//   0x01  the MID texture   (span 0, and span 3 which is the mid drawn in two
//                            passes -- both map to the same bit, which is the
//                            clearest sign this is about wall PARTS)
//   0x02  the LOWER section (span 2, below the opening)
//   0x04  the UPPER section (span 1, above the opening)
//
// and a SET bit ALLOWS that part. So +0x06 on a face record says WHICH BAND OF
// THE WALL the trigger answers on -- a switch on the mid texture, a panel in
// the lower section -- and has nothing to do with where the player stands.
//
// The authored data says the same thing plainly. STUDY1's 26 0x18 records carry
// 0x01 twenty-four times (mid only), 0x02 once (lower only) and 0x06 once
// (lower and upper, but NOT the mid); its single 0x32 carries 0x06. Those are
// choices about wall sections, not about approach angles.
//
// IMPLEMENTING IT AS A DIRECTION GATE WOULD HAVE BEEN WRONG AND QUIET: 24 of 26
// records would have been restricted to one quadrant of player facing, and the
// triggers would simply have stopped working from most directions.
//
// GZDoom classifies the same three bands, and p_trace.cpp:526-529 already does
// it with the identical rule, so this only needs the hit height.
//
//==========================================================================

uint8_t WallPartBit(line_t *line, double z)
{
	// The sector behind the line: its back sector, or -- for a cut face of the portal plan, which is a
	// linked portal on a one-sided line -- the sector beyond the portal, measured where it really is.
	// So a cut face keeps the three bands it had when the two rooms shared the line.
	sector_t *back = line != nullptr ? line->EffectiveBackSector() : nullptr;
	if (back == nullptr) return 0x01;   // one-sided: mid

	DVector2 at = line->v1->fPos();
	if (line->backsector == nullptr) at += line->getPortalDisplacement();
	const double bf = back->floorplane.ZatPoint(at);
	const double bc = back->ceilingplane.ZatPoint(at);
	if (z <= bf) return 0x02;   // below the opening -> lower
	if (z >= bc) return 0x04;   // above it -> upper
	return 0x01;                // the opening itself -> mid
}

int DoomYawToRoth512(DAngle yaw)
{
	// The inverse of RothAngleToDoom (roth_diff.cpp:95), which is
	// 90 + a512 * 360/512. Two's-complement `& 511` is the right modulo here
	// and keeps a negative angle positive.
	return (int)lround((yaw.Degrees() - 90.0) * (512.0 / 360.0)) & 511;
}

bool FacingAllows(uint8_t fireFlags, AActor *who)
{
	if (who == nullptr) return true;
	const uint32_t t = ((uint32_t)DoomYawToRoth512(who->Angles.Yaw) - 0x40u) & 0x1ffu;
	const uint8_t bit = (uint8_t)(1u << ((t >> 7) & 7));
	return (fireFlags & bit) == 0;
}

struct Runtime
{
	bool active = false;
	Map map;                       // the level's own copy of its logic
	FLevelLocals *level = nullptr;

	// Trigger bindings, built once at load. A face or sector can carry several,
	// and they need not share an event -- one wall can have a use trigger and an
	// examine trigger on it.
	std::map<int, std::vector<Binding>> byFace;     // Realms face  -> bindings
	std::map<int, std::vector<Binding>> bySector;   // Realms sector-> bindings
	// Realms OBJECT id (Object::commandID, the field at +0x0e) -> bindings.
	// Keyed by the record's key because the original searches for a matching
	// id at every activation rather than resolving an index at load.
	std::map<int, std::vector<Binding>> byObject;

	// Doorway sector -> the polyobject tag of the panel that fills it.
	std::map<int, int> doorTag;
	// Player number -> the sector it was in when last looked at, so a crossing
	// can be noticed. Realms' own enter-sector machinery is a per-frame poll of
	// the same shape (twe_link_state, raw_commands.c:3202), not a collision
	// callback, so a per-tic comparison is the faithful mechanism and not an
	// approximation of one.
	std::map<int, int> lastSectorByPlayer;

	// Engine sidedef -> the polyobject tag of the door leaf it belongs to.
	//
	// A leaf is NOT a Realms face: the loader generates its four lines, so it
	// has no face id, no mapping record and no entry in faceToSide. It is still
	// the thing the player clicks -- GAME_core.md §4.5 sends a type-6 door
	// straight to toggle_door_open_state with no trigger chain -- so the use
	// path needs a way from the line it hit to the panel to swing.
	std::map<int, int> doorLeafSide;
	// Engine sidedef of a broad leaf side -> the Realms face on the room's side.
	std::map<int, int> doorLeafRoomFace;
	// Polyobject tag -> what decides the swing (RegisterDoorSwing).
	struct DoorSwing
	{
		int marker = 0xFFFE;
		int secC1 = -1, secC3 = -1;
		DVector2 v1 = { 0., 0. }, v2 = { 0., 0. };
	};
	std::map<int, DoorSwing> doorSwing;
	// Realms face -> engine sidedef, so a trigger's face can be found from a side.
	std::map<int, int> faceToSide;
	std::map<int, int> sideToFace;                 // and back again
	// Realms texture-map record -> the faces that use it, for the scroll effect:
	// its members are MAPPING records and the sidedefs hang off the faces.
	std::map<int, std::vector<int>> texmapToFaces;
	std::map<int, std::vector<Binding>> bySide;    // built from byFace

	// "Whatever the player just used" -- the original's g_active_object /
	// g_active_object_secondary (ROTH_COMMANDS.md, "key = 0 means the thing the
	// player just used"). A key of 0 routes here instead of naming geometry,
	// which is how ONE command record serves many doors.
	int activeFace = -1;
	int activeSector = -1;
	// The object the player just acted on, as (sector, index). Staged the same
	// way activeFace and activeSector are, so a chain command whose own key is
	// 0 resolves to it. -1 / -1 when the last action was not on an object.
	int activeObjectSector = -1;
	int activeObjectIndex = -1;

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
	std::map<int, std::vector<int>> lightFollowers;
	std::map<int, std::pair<double, double>> sectorShift;   // roth_portals: moved sectors

	// roth_platforms. How far each moving sector has slid from where it was at
	// load, summed from the 0x09 tick's own per-frame deltas, and the sectors
	// the loader turned into platform actors (in ascending order).
	bool platformsOn = false;
	std::map<int, std::pair<int, int>> movers;
	std::vector<int> platformSectors;
	int moversUnshown = 0;        // 0x09 effects running with nothing to show them

	// Realms FLAT index -> the engine texture, and whether the index is the
	// pack's sky marker. See RegisterFlat.
	// index -> (texture, isSky, opaque256). The third field is the 256x256
	// opaque scale exception, carried from the loader because the artwork is
	// gone by the time the runtime needs it.
	struct FlatReg { FTextureID tex; bool isSky; bool opaque256; };
	std::map<int, FlatReg> flatByIndex;

	// Realms texture index -> the engine texture the loader made for it.
	// PRE-RESOLVED rather than looked up on demand, because the loader's
	// TextureSet is a local that dies with the load; the loader registers every
	// index its command records can name. See RegisterTexture.
	std::map<int, FTextureID> texByIndex;
	std::map<int, FTextureID> mirroredByIndex;   // RegisterMirroredTexture

	// The wall rule's inputs per face. See RegisterWallFace and ReapplyWallFace.
	struct WallReg { WallFace wf; int texelLength = 0; int own = -1, nbr = -1; WallPieceShape piece[3]; };
	std::map<int, WallReg> wallByFace;
	int wallBakedKept = 0;      // a shift change on a baked piece: the picture is left as loaded
	int wallBakedDropped = 0;   // a swap onto a piece that would need a baked image: drawn from the plain one

	// What ran and what could not, for the report and for the console.
	int fired = 0;
	std::map<uint8_t, int> unhandledOps;
};

Runtime g;

// Carried ACROSS a level change, because the arrival point is latched in the map
// the player is leaving and applied in the one they arrive in.
std::string g_warpArrivalMapName;
int g_warpArrivalSector = -1;
// Also carried: how far above its floor the player stood (the original keeps
// that, map_load.c:518-520 and 627-631), and for an arrival key of 0, where
// it stood in REALMS coordinates, which it keeps outright (no relocation
// runs, map_load.c:661).
double g_warpAboveFloor = 0.;
bool g_warpCarryXY = false;
double g_warpCarryX = 0., g_warpCarryY = 0., g_warpCarryZ = 0.;

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
// WHICH GEOMETRY and WHICH EVENT ARE TWO DIFFERENT QUESTIONS, and conflating
// them is what went wrong here twice.
//
// The markers above answer the first: they say which index space a trigger's
// key lives in, so they decide what it is BOUND to. They say nothing about what
// makes it fire. "Sector-keyed" is not "fires when you enter the sector" --
// 0x19 is keyed by a sector and fired by a CLICK on that sector's floor.
//
// The event comes from GAME_core.md 5.2, which gives the firer of each of the
// 15 categories from the original's code and states that it "corrects
// R/ROTH_COMMANDS.md and R/src/roth/roth_runtime.cpp:54-55" -- this file. The
// correction is applied here. Its table, for the six that reach geometry:
//
//   0x18  left-click a wall face         face-keyed    5.2 row 5
//   0x32  right-click (examine) a face   face-keyed    5.2 row 11
//   0x1a  an ATTACK or projectile hits a face          5.2 row 7
//         -- and 5.2 says of it: "the player's POINT sweep never fires it"
//   0x19  left-click a floor / platform top   sector-keyed  5.2 row 6
//   0x31  right-click a floor / ceiling / platform      5.2 row 12
//   0x13  ENTER or LEAVE a sector        sector-keyed  5.2 row 4
//         -- twe_link_state -> fire_sector_trigger, raw_commands.c:3202-3270
//
// 0x13 IS THE ENTER-SECTOR TRIGGER, and nothing in this file used to classify
// it at all, so every one of those was bound to nothing and could never fire.
// The comment that used to sit here dismissed it as "the water/lava machine ...
// not one of these" on the strength of ONE of its three variants; 5.2 row 4
// lists them as "plain, Z-below-floor (water), linked platform", so the water
// machine is a variant of the enter-sector trigger rather than an alternative
// to it.
//
// 0x1a WAS IN THE USE BUCKET and called a BUMP. It is neither: it is the weapon
// -hit channel (collision_physics.c:556-568 -> fire_wall_object_trigger), and
// the player walking into a wall does not fire it.
//
// THE DIRECTION MASK AND BOUNDING BOX ARE STILL NOT IMPLEMENTED. Every trigger
// record carries an approach mask at +0x06 (`fireFlags`) and the dispatcher
// checks it first (5.1, "check the direction mask"), so a trigger fires here
// from any approach rather than only the authored one. A KNOWN over-fire,
// counted in the load report, not an approximation.
// What fires this trigger. GAME_core.md 5.2; see the comment above.
TrigEvent EventForOpcode(uint8_t op)
{
	switch (op)
	{
	case 0x18: case 0x19: return TrigEvent::Use;
	case 0x32: case 0x31: return TrigEvent::Examine;
	case 0x1A:            return TrigEvent::Impact;
	case 0x13:            return TrigEvent::SectorEnter;

	// THE OBJECT CHANNEL. 0x08 and 0x02 are both category 1 -- left-clicking an
	// object -- and 0x30 is category 13, examining one. Verified in ROTH.C, not
	// taken from the opcode tables:
	//
	//   run_leftclick_object_trigger (E/raw_commands.c:2920) finds the record by
	//   matching the record's key at +0x08 against the OBJECT's own id at +0x0e,
	//   then branches on the base byte at +0x03: base 2 is a light switch, base
	//   8 runs the chain. So "0x08 / 0x02" in GAME_core.md 5.2's row 1 is two
	//   BASES in one category, not two opcodes in two.
	//
	//   The examine side is dispatch_entry_command_trigger_b's channel 2
	//   (:3145-3164), type 4: the same key-against-id match, and explicitly NO
	//   direction mask and NO bounding box, unlike the face channel beside it.
	//
	// Both are keyed to an object, which is a third key space beside faces and
	// sectors -- see IsObjectTrigger and g.byObject.
	case 0x08: case 0x02: return TrigEvent::Use;
	case 0x30:            return TrigEvent::Examine;

	default:              return TrigEvent::None;
	}
}

// What the trigger's key names, which is what it is bound to. Read off the load
// markers, not the opcode number.
bool IsFaceTrigger(uint8_t op)   { return op == 0x18 || op == 0x1A || op == 0x32; }
bool IsSectorTrigger(uint8_t op) { return op == 0x13 || op == 0x19 || op == 0x31; }

// THE THIRD KEY SPACE. An object trigger's key at +0x08 is matched against the
// object's own id at +0x0e -- Object::commandID, which roth_raw.h already names
// as exactly that field -- so it resolves to neither a face nor a sector and
// fell through the binding loop entirely until this existed.
//
// 0x08 and 0x02 are the two BASES of category 1 (left-click) and 0x30 is
// category 13 (examine); see EventForOpcode for the ROTH.C citations.
bool IsObjectTrigger(uint8_t op) { return op == 0x08 || op == 0x02 || op == 0x30; }

const char *TrigEventName(TrigEvent e)
{
	switch (e)
	{
	case TrigEvent::Use:         return "use";
	case TrigEvent::Examine:     return "examine";
	case TrigEvent::Impact:      return "impact";
	case TrigEvent::SectorEnter: return "sector enter/leave";
	default:                     return "none";
	}
}

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
//   +0x07           the SPEED multiplier for the angle step (it lands in the
//                   DOOR record's +0x0c, which doors.c:1042 reads)
//   +0x0c           the sound while it OPENS (edi -> [0x8b3cc], doors.c:806,
//                   628-634)
//   +0x0e / +0x10   the sound while it CLOSES and the sound when it has SHUT
//                   (doors.c:1057-1063, 1142-1145). All three are stored plus
//                   one, 0 for silence, and are played by roth_sound.
//
// The arc is a FIXED 90 degrees: the angle byte is clamped to +/-0x40
// (doors.c:1082, 1108) and the table it drives is 256 steps to a turn.
//
//==========================================================================

// Defined just below; OpenDoor resolves the target and hands off to it.
int SwingDoor(int tag, int speedMul, int dwellTicks, int fromSector = -1);

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

	// THE KEY NAMES A FACE, AND THE DOOR IS THE SECTOR BEHIND IT.
	//
	// spawn_door_instance (doors.c:697-698) takes the door sector from the
	// face's SISTER -- `fs:[fs:[face+8]+6]` -- and hands the face's OWN sector,
	// `fs:[face+6]`, to setup_door_swing_geometry as the room the door is being
	// opened from (doors.c:707, 484). That room is what an 0xFFFF hinge compares
	// against to swing away from the opener (doors.c:525).
	//
	// The face's own sector is still tried second, with the sister's as the
	// room: the load pass moves a binding off a door's own face onto its sister
	// (map_load.c:1158-1172), and this keeps a record that names the door's own
	// face working the same way.
	int rothSector = -1, fromSector = -1;
	for (int fi : candidates)
	{
		if (fi < 0 || fi >= (int)g.map.faces.size()) continue;
		const int sec = g.map.faces[fi].sector;
		const int sis = g.map.faces[fi].sister;
		const int ss = (sis >= 0 && sis < (int)g.map.faces.size()) ? g.map.faces[sis].sector : -1;
		if (ss >= 0 && g.doorTag.count(ss)) { rothSector = ss; fromSector = sec; break; }
		if (sec >= 0 && g.doorTag.count(sec)) { rothSector = sec; fromSector = ss; break; }
	}
	if (rothSector < 0) return CMD_NOTHING;

	// SPEED AND DWELL COME FROM THE SAME COMMAND FIELD, +0x07.
	//
	// This used to read the speed from args[3], the command record's +0x0c -- a
	// confusion between the COMMAND record and the DOOR record. The COMMAND's
	// byte +0x07 travels through [0x8b3c4] and lands in the DOOR record's dword
	// +0x0c (doors.c:549), which is where the tick reads the multiplier. That it
	// is also the field the dwell is scaled by is the clearest sign the old
	// reading was wrong: one value cannot live at two offsets.
	const int acted = SwingDoor(g.doorTag[rothSector], (int)c.subFlags, (int)c.aux, fromSector);
	// The door's three sounds, each stored one higher: +0x0c while it opens,
	// +0x0e while it closes, +0x10 when it has shut (cmd_open_door passes them
	// as edi and the two halves of edx, raw_commands.c:4073-4076;
	// register_door_swing, doors.c:805-807). Only a NEW swing has them.
	if (acted == CMD_ACTED)
		SoundDoorOpened(g.doorTag[rothSector], c.Word(0x0C), c.Word(0x0E), c.Word(0x10));

	// THE OTHER HALF OF A DOUBLE DOOR opens with it. resolve_door_neighbor_sector
	// (doors.c:308-340) walks the door's faces in slot order for the first whose
	// far sector carries an id of 0xFFFE or above, and then that sector's faces
	// for the first whose far side is NOT this door; spawn_door_instance
	// (doors.c:711-733) opens the partner through that far face, so the partner
	// counts as opened from the sector that face belongs to. A partner that is
	// already open is left alone (doors.c:714-719), which SwingDoor's own
	// refusal of a moving or open leaf gives for free.
	const Sector &ds = g.map.sectors[rothSector];
	for (int j = 0; j < (int)ds.faceCount && ds.firstFaceIndex >= 0; j++)
	{
		const int fi = ds.firstFaceIndex + j;
		if (fi >= (int)g.map.faces.size()) break;
		const int sis = g.map.faces[fi].sister;
		if (sis < 0 || sis >= (int)g.map.faces.size()) continue;
		const int ps = g.map.faces[sis].sector;
		if (ps < 0 || ps >= (int)g.map.sectors.size()) continue;
		const Sector &partner = g.map.sectors[ps];
		if (partner.commandID < TRIGGER_INVIS) continue;      // doors.c:320, `>= 0xfffe`
		for (int k = 0; k < (int)partner.faceCount && partner.firstFaceIndex >= 0; k++)
		{
			const int pf = partner.firstFaceIndex + k;
			if (pf >= (int)g.map.faces.size()) break;
			const int psis = g.map.faces[pf].sister;
			if (psis < 0 || psis >= (int)g.map.faces.size()) continue;
			const int far = g.map.faces[psis].sector;
			if (far == rothSector) continue;
			if (acted == CMD_ACTED && g.doorTag.count(ps))
			{
				// The partner is allocated by the same alloc_door_record with
				// the same staged sound words (doors.c:628-634, 788), so it
				// sounds as the first leaf does.
				if (SwingDoor(g.doorTag[ps], (int)c.subFlags, (int)c.aux, far) == CMD_ACTED)
					SoundDoorOpened(g.doorTag[ps], c.Word(0x0C), c.Word(0x0E), c.Word(0x10));
			}
			break;
		}
		break;                                                 // the first such neighbour only
	}
	return acted;
}

//==========================================================================
//
// The swing itself, by polyobject tag. Split out from OpenDoor so that the
// `roth_door` console command exercises THIS code -- the real unit conversion
// and the real arguments -- rather than a parallel copy that could drift from
// it and quietly disagree.
//
// speedMul is the command's byte +0x07 and dwellTicks its word +0x0a, both RAW:
// the conversions below are the whole point and belong in one place.
//
//==========================================================================

int SwingDoor(int tag, int speedMul, int dwellTicks, int fromSector)
{
	if (g.level == nullptr) return CMD_NOTHING;

	// WHICH WAY, AND ABOUT WHICH END OF THE HINGE EDGE.
	//
	// setup_door_swing_geometry decides a "mirror" from the hinge mapping's
	// faceID (doors.c:523-531): 0xFFFE never, 0xFFFD always, and 0xFFFF when
	// the sector across corner c3 is the room the door is being opened from --
	// the door that swings away from whoever opens it. Unmirrored, the pivot is
	// vertex1 of the hinge face and the angle byte runs 0 -> +0x40
	// (doors.c:1083-1094); mirrored, the pivot is vertex2 (`rol ebx,16`,
	// doors.c:528, 546) and it runs 0 -> -0x40 (doors.c:1112-1123).
	//
	// rotate_quad turns the leaf by MINUS the angle byte (renderer.c:1562:
	// the table index is `(-2 * angle) & 0x1ff`), and the map's x,y are the
	// engine's x,y unchanged, so unmirrored is CLOCKWISE and mirrored is
	// counter-clockwise. EV_OpenPolyDoor only ever turned counter-clockwise
	// about the one spot the loader gave it, which sent 93 of the 167 retail
	// doors the wrong way (tools/doorcheck); a negative speed turns it the
	// other way, and DPolyDoor measures the arc by |speed|.
	bool mirrored = false;
	auto sw = g.doorSwing.find(tag);
	if (sw != g.doorSwing.end())
	{
		const Runtime::DoorSwing &d = sw->second;
		if (d.marker == 0xFFFD) mirrored = true;
		else if (d.marker == 0xFFFE) mirrored = false;
		else mirrored = (fromSector >= 0 && fromSector == d.secC3);

		// An 0xFFFF door has no fixed pivot, so it is put on the right end of
		// the hinge edge here, while the leaf is closed and at rest: at angle 0
		// every vertex is StartSpot + OriginalPts, so moving the spot and
		// taking the same amount off the points leaves the leaf where it is.
		// (The loader has already put an FFFD or FFFE door on its one pivot.)
		if (d.marker == 0xFFFF)
		{
			FPolyObj *po = g.level->GetPolyobj(tag);
			if (po != nullptr && po->specialdata == nullptr && !po->bBlocked
				&& po->Angle == nullAngle)
			{
				const DVector2 want = mirrored ? d.v2 : d.v1;
				const DVector2 delta = want - po->StartSpot.pos;
				if (delta.X != 0. || delta.Y != 0.)
				{
					for (unsigned i = 0; i < po->OriginalPts.Size(); i++)
						po->OriginalPts[i].pos -= delta;
					po->StartSpot.pos = want;
				}
			}
		}
	}

	// Both arguments come from the SAME command field, +0x07, which is why they
	// arrive together. The caller reads them; see OpenDoor for the chain
	// (cmd_open_door raw_commands.c:4071-4076 -> register_door_swing ->
	// spawn_door_instance -> setup_door_swing_geometry, where the COMMAND's byte
	// +0x07 lands in the DOOR record's dword +0x0c, doors.c:549).

	// M == 0 IS M == 1, NOT M == 8. The original's step is `dt * M`, or
	// `min(dt, 8)` when M is zero (doors.c:1043-1045). dt is whole 70 Hz ticks
	// elapsed since the previous rendered frame, so `min(dt, 8)` is exactly one
	// angle unit per tick at anything above 8.75 fps -- a per-frame OVERSHOOT
	// CAP, not a speed of 8. Reading it as 8 ran those doors eight times too
	// fast.
	const int M = speedMul > 0 ? speedMul : 1;

	// THE UNIT CONVERSION IS EXACTLY 16, and it falls out rather than being
	// fitted. EV_OpenPolyDoor's swing branch sets
	//     m_Speed = speed * (90./64) / 8      degrees per 35 Hz tic
	// (po_man.cpp:674), and 90/64 = 1.40625 is EXACTLY the original's degrees
	// per angle unit -- Hexen's polyobject doors inherited the same 64-step
	// quarter turn from the same era, so the two engines already agree on the
	// unit. Equating the rates:
	//     Realms  M * 1.40625 * 70 = 98.4375 * M  deg/s
	//     GZDoom  speed * 1.40625 / 8 * 35 = 6.15234 * speed  deg/s
	//     => speed = 16 * M
	// Check: M = 1 gives m_Speed = 2.8125 deg/tic, so 90 degrees takes 32 tics
	// = 0.914 s, against the original's 64 ticks at 70 Hz = 0.914 s. Exact.
	const double speed = (mirrored ? 16.0 : -16.0) * double(M);

	// DWELL IS `A` TICKS, WHATEVER THE SPEED. The reload is A*M (doors.c:749-
	// 750) but the counter is decremented by the SAME step as the swing, M per
	// tick (doors.c:1052) -- so it runs for A*M/M = A ticks and the dwell does
	// NOT scale with speed. Scaling it by M here, as a first pass of this code
	// did, would have made fast doors wait proportionally longer.
	//
	// A is in 70 Hz ticks and GZDoom's m_WaitTics is in 35 Hz tics, so halve.
	//
	// A DWELL OF ZERO MEANS THE DOOR STAYS OPEN. tick_swinging_doors tests the
	// reload word before it counts anything down -- `cmp word [edi+0xa],0; je`
	// (doors.c:1049) -- so a door with no dwell never starts to close by
	// itself. This passed 0 on, and DPolyDoor reads a wait of 0 as "close at
	// once" (po_man.cpp, `m_WaitTics >= 0`): the door opened and shut again
	// without a pause. -1 is the engine's own "wait for ever". Three retail
	// commands carry it, among them STUDY1's cabinet doors (key 0x13, M 44).
	const int delay = dwellTicks > 0 ? (dwellTicks + 1) / 2 : -1;

	// The arc is always 90 degrees: 64 angle units of 1.40625 degrees, settled
	// from the sincos table's own bytes. See HANDOFF_REMAROTH.md section 12.
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
	// raw_commands.c:4652-4656: the pending record runs only when
	// g_item_autoselected_flag is clear. Set, the record stays latched and the
	// (non-zero) latch is the answer.
	if (pending != 0 && !(g_core.IsAttached() && g_core.Items().AutoSelected()))
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
	// The original returns the pending RECORD POINTER here (raw_commands.c:
	// 4652-4659), and the exec loop only ORs it into the chain result. A record
	// pointer is even, so it makes the chain "do something" (a trigger's one-shot
	// bit, :1646) without setting bit 0, which is what lets the post-chain sound
	// play (:1296). CMD_LATCHED says exactly that; this used to answer -1.
	return pending != 0 ? CMD_LATCHED : CMD_NOTHING;
}

//==========================================================================
//
// 0x2b run_command_dbase100_record -- raw_commands.c:4697-4709
//
// Fires a DBASE100 action record: id = word[rec+8]. The first 0x2b of a chain
// sets the chain's dialogue latch (0x8a26c) and the interpreter's queue-active
// flag (0x81e18); then eval_dialogue_record_by_id runs the record. If it acted
// the shared consume-tail runs and the answer is -1, else 0.
//
// With no DBASE100 the original's own early-out answers 0 (dbase100.c:547),
// and so does this, counted so it does not look like a success.
//
// NOT IN IsImplemented(), ON PURPOSE. The interpreter runs and its lines print
// to the console, but nobody has seen it run in the engine, choices cannot be
// picked and nothing is drawn. The census keeps counting 0x2b as a hole until
// the owner has seen a conversation (tools/rothdiff/opcodecensus.cpp says why).
//
// The chain's tail -- finish_dialogue_record_eval, the latch cleared, the
// autoselect flag cleared (raw_commands.c:1313-1315) -- is in Fire.
//
//==========================================================================

// THE FIVE ITEM COMMANDS RUN roth_itemcmds.cpp, the engine-free transcription
// that tools/gameproof replays against the original (ROTH.C running, the same
// record bytes from the retail maps, call by call). This adapter only moves
// the chain state in and out of it.
int32_t g_ifItemList[16];
int g_ifItemListCount = 0;

struct RuntimeItemCmdHost : ItemCmdHost
{
	Command *cmd = nullptr;
	ItemCmdRecord rec;
	ItemCmdRecord *PendingRecord(uint32_t handle) override
	{
		cmd = Rec((uint16_t)handle);
		if (cmd == nullptr) return nullptr;
		rec.modifier = cmd->modifier; rec.fireFlags = cmd->fireFlags; rec.key = cmd->key;
		return &rec;
	}
	void PendingRecordRan(uint32_t) override
	{
		if (cmd != nullptr) { cmd->modifier = rec.modifier; SyncDisabled(cmd); }
	}
};

template<class F> int RunItemCmd(Command *rec, Handlers *h, F f)
{
	ItemCmdState st;
	st.pendingRecord = g.pendingRecord;
	st.dialogueLatch = g_dialogueLatch;
	memcpy(st.list, g_ifItemList, sizeof(st.list));
	st.listCount = g_ifItemListCount;
	ItemCmdRecord r;
	r.modifier = rec->modifier; r.fireFlags = rec->fireFlags; r.key = rec->key;
	RuntimeItemCmdHost host;
	const int32_t ret = f(st, r, host);
	rec->modifier = r.modifier; SyncDisabled(rec);
	g.pendingRecord = (uint16_t)st.pendingRecord;
	g_dialogueLatch = st.dialogueLatch;
	memcpy(g_ifItemList, st.list, sizeof(st.list));
	g_ifItemListCount = st.listCount;
	if (h != nullptr && st.interrupt == 1) h->interrupt = Interrupt::Break;
	if (h != nullptr && st.interrupt == 2) h->interrupt = Interrupt::Rerun;
	// -1 acted; a positive value is TailIfNext's still-latched pending record:
	// non-zero with bit 0 clear in the original, which CMD_LATCHED carries
	// (see TailIfNext above).
	return ret > 0 ? CMD_LATCHED : ret != 0 ? CMD_ACTED : CMD_NOTHING;
}

int RunDbase100Record(Command *rec)
{
	if (!g_core.IsAttached())
	{
		g.unhandledOps[0x2B]++;
		return CMD_NOTHING;                  // eval_dialogue_record_by_id -> 0
	}
	return RunItemCmd(rec, nullptr, [](ItemCmdState &st, ItemCmdRecord &r, ItemCmdHost &) {
		return RunCommandDbase100Record(g_core, st, r); });               // :4697-4709
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
// The inventory commands: 0x27, 0x29, 0x2a, 0x42.
//
// All four are transcribed from raw_commands.c and call the game core. With
// no core (no DBASE100) each is counted as unhandled and does nothing.
//
// NOT IN IsImplemented(), ON PURPOSE, exactly as 0x2b is not: the census moves
// when the owner has picked something up and seen it, not when this compiles.
//
// [unverified] give_item's context. The original hands it the active world
// object (0x8a134), or a drop position worked out from the secondary active
// object (0x8a0fc) -- raw_commands.c:4801-4822. That decides where the item
// is seen flying in from, and nothing else. The runtime has neither latch
// yet, so the context is null: the item is given, with no fly-in.
//
//==========================================================================

// The id list 0x27 builds in list mode, g_query_list 0x89f68 / count 0x89f64
// (raw_commands.c:4765-4770), is g_ifItemList above. Cleared with the chain
// state (raw_commands.c:1328) -- see Fire.

// 0x27 cmd_if_not_item -- raw_commands.c:4760-4789.
int IfNotItem(Command *rec, Handlers &h)
{
	if (!g_core.IsAttached()) { g.unhandledOps[0x27]++; return CMD_NOTHING; }
	return RunItemCmd(rec, &h, [](ItemCmdState &st, ItemCmdRecord &r, ItemCmdHost &host) {
		return CmdIfNotItem(g_core, st, r, host); });
}

// give_item's context (raw_commands.c:4801-4822): the object the player just
// clicked, g_command_active_chain 0x8a134, set only while that object's chain
// runs (run_leftclick_object_trigger, raw_commands.c:2929, cleared on every
// exit). Its 16 bytes are the context record (x +0, y +2, render word +4,
// flags +7, z +0xa): with it the item is seen flying in from where the object
// stood. ROTH.C's other source, a drop point between the two vertices of a
// secondary active EDGE (0x8a0fc), is set only by the original's edge
// channel, which the face channel here does not stage. Null otherwise.
static GiveContext g_activeGiveContext;
const GiveContext *ActiveGiveContext()
{
	if (g.activeObjectSector < 0 || g.activeObjectSector >= (int)g.map.objects.size()) return nullptr;
	const std::vector<Object> &grp = g.map.objects[g.activeObjectSector];
	if (g.activeObjectIndex < 0 || g.activeObjectIndex >= (int)grp.size()) return nullptr;
	const Object &o = grp[g.activeObjectIndex];
	g_activeGiveContext.x = o.x;
	g_activeGiveContext.y = o.y;
	g_activeGiveContext.id = (uint16_t)(o.textureIndex | (o.textureSource << 8));
	g_activeGiveContext.flags7 = o.flags;
	g_activeGiveContext.z = o.z;
	return &g_activeGiveContext;
}

} // namespace (anonymous) -- reopened below; roth_interface.cpp links to this

// The level's copy of an object record, for roth_interface's pickup.
bool RuntimeObjectRecord(int rothSector, int rothIndex, Object &out)
{
	if (rothSector < 0 || rothSector >= (int)g.map.objects.size()) return false;
	const std::vector<Object> &grp = g.map.objects[rothSector];
	if (rothIndex < 0 || rothIndex >= (int)grp.size()) return false;
	out = grp[rothIndex];
	return true;
}

namespace {

// 0x29 cmd_give_item -- raw_commands.c:4799-4830.
int GiveItemCmd(Command *rec, Handlers &h)
{
	if (!g_core.IsAttached()) { g.unhandledOps[0x29]++; return CMD_NOTHING; }
	return RunItemCmd(rec, &h, [](ItemCmdState &st, ItemCmdRecord &r, ItemCmdHost &) {
		return CmdGiveItem(g_core, st, r, ActiveGiveContext()); });
}

// 0x2a cmd_remove_item -- raw_commands.c:4918-4931.
int RemoveItemCmd(Command *rec)
{
	if (!g_core.IsAttached()) { g.unhandledOps[0x2A]++; return CMD_NOTHING; }
	return RunItemCmd(rec, nullptr, [](ItemCmdState &st, ItemCmdRecord &r, ItemCmdHost &) {
		return CmdRemoveItem(g_core, st, r); });
}

// 0x42 cmd_set_inventory_filter -- raw_commands.c:4940-4947. "Take inventory":
// a display filter, nothing is removed.
int SetInventoryFilterCmd(Command *rec)
{
	if (!g_core.IsAttached()) { g.unhandledOps[0x42]++; return CMD_NOTHING; }
	return RunItemCmd(rec, nullptr, [](ItemCmdState &, ItemCmdRecord &r, ItemCmdHost &) {
		return CmdSetInventoryFilter(g_core, r); });
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
	// :516 clears g_item_autoselected_flag before latching. Missing until
	// 2026-11-12, so a 0x36 after an auto-select left the pending record gated.
	if (g_core.IsAttached()) g_core.Items().ClearAutoSelected();
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
	// AN EMPTY NAME IS A WARP WITHIN THIS MAP, not a record to ignore.
	// cmd_map_transition latches and answers -1 whatever the name is
	// (raw_commands.c:525-533), and process_map_warp_or_load skips its name
	// compare when the requested name is empty, which leaves `same` set and
	// relocates in place (map_load.c:598-623). STUDY1 has one: record 383,
	// reached from dialogue action 178. This returned CMD_NOTHING until
	// 2026-11-12, so that chain "did nothing".

	g.warpMap = name;
	g.warpArrival = rec->key;
	g.warpPending = true;

	if (rec->fireFlags & 0x10) { rec->modifier |= 0x08; SyncDisabled(rec); }
	return CMD_ACTED;
}

//==========================================================================

int RunCommand(const Map &m, const Command &cc, int index);
int RunCommandWithChain(const Map &m, const Command &cc, int index, Handlers &h);
int SwingDoor(int tag, int speedMul, int dwellTicks, int fromSector);

// A door open asked for before the level exists. `roth_door` is normally typed
// from an +exec script, and exec runs BEFORE the deferred `map` -- so the
// command would otherwise always find no level and do nothing. Queued here and
// drained from the level tick, exactly as roth_diff.cpp queues its captures and
// for the same reason. `after` lets a test watch a door mid-swing.
struct PendingDoor
{
	int tag = 0;          // 0 = every door
	int speedMul = 0;
	int dwell = 0;
	int after = 0;        // tics still to wait
};
std::vector<PendingDoor> gPendingDoors;
void DrainPendingDialogue();      // roth_dialogue / roth_inventory, at the end of this file
void DrainTimedCommands(FLevelLocals *level);   // roth_at, at the end of this file

void DrainPendingDoors()
{
	for (size_t i = 0; i < gPendingDoors.size(); )
	{
		if (gPendingDoors[i].after > 0) { gPendingDoors[i].after--; i++; continue; }
		const PendingDoor d = gPendingDoors[i];
		gPendingDoors.erase(gPendingDoors.begin() + i);

		int opened = 0, refused = 0;
		for (const auto &kv : g.doorTag)
		{
			if (d.tag != 0 && kv.second != d.tag) continue;
			if (SwingDoor(kv.second, d.speedMul, d.dwell, -1) == CMD_ACTED) opened++;
			else refused++;
		}
		Printf("roth_door: %d opened, %d refused (speedM %d -> %.1f deg/s, dwell %d)\n",
			opened, refused, d.speedMul,
			98.4375 * double(d.speedMul > 0 ? d.speedMul : 1), d.dwell);
	}
}

// How deep 0x40 may nest. The original has no limit and relies on the data
// being sane; now that a sub-chain dispatches through the chain dispatcher, a
// 0x40 naming itself would recurse rather than merely loop, so this bounds the
// stack the same way the iteration guard below bounds the loop.
//
// The retail maps carry 76 0x40 records across 10 maps (measured,
// tools/rothdiff/opcodecensus.cpp). How deeply they actually nest has NOT been
// measured, so 16 is a bound chosen to be far above any plausible authored
// depth rather than a number read off the data -- if it ever fires, measure
// before raising it.
int gIndexedDepth = 0;
const int MAX_INDEXED_DEPTH = 16;

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
	if (gIndexedDepth >= MAX_INDEXED_DEPTH) return CMD_NOTHING;

	struct DepthGuard
	{
		DepthGuard()  { gIndexedDepth++; }
		~DepthGuard() { gIndexedDepth--; }
	} depth;

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
			// THROUGH THE CHAIN DISPATCHER, not the bare one. This used to call
			// RunCommand, which silently dropped every opcode that needs the
			// chain -- 0x28, 0x36, 0x38, 0x2b and a nested 0x40 -- so a
			// conditional branch inside a 0x40 sub-chain did nothing at all and
			// was counted as unhandled. The original has no such split: its 0x40
			// loop dispatches through the SAME 0x30780 table as the top level
			// (raw_commands.c:3968-3975), so those five run there.
			last = RunCommandWithChain(g.map, *r, (int)ax, h);
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

//==========================================================================
//
// RE-RUN THE WALL RULE ON ONE FACE, with the mapping record's slots and shifts
// as they are NOW and the sectors' current heights: roth_surface's
// WallPieceRule (what ROTH.C samples) and WallToEngine (the sidedef values that
// sample the same), exactly as the loader called them. Nothing here computes a
// scale or an offset of its own.
//
// texturesChanged: a swap wrote one of the record's slots (0x34, 0x0c). Without
// it only the shifts moved (0x0f) and every piece keeps its picture.
//
// What the runtime cannot do is BAKE an image (the loader's TextureSet is gone):
//   * a shift change on a piece the loader baked leaves that piece as loaded
//     (wallBakedKept) -- its picture has the old shift built in;
//   * a swap onto a piece that would need a baked image draws the plain one,
//     shifted by the rule (wallBakedDropped), as the loader does when a bake is
//     too large.
//
//==========================================================================

static void ReapplyWallFace(int fi, bool texturesChanged)
{
	if (g.level == nullptr || fi < 0 || (size_t)fi >= g.map.faces.size()) return;
	auto wr = g.wallByFace.find(fi);
	auto sdi = g.faceToSide.find(fi);
	if (wr == g.wallByFace.end() || sdi == g.faceToSide.end()) return;
	if (sdi->second < 0 || (size_t)sdi->second >= g.level->sides.Size()) return;
	const int tmi = g.map.faces[fi].textureMap;
	if (tmi < 0 || (size_t)tmi >= g.map.textureMaps.size()) return;
	const TextureMap &tm = g.map.textureMaps[tmi];
	side_t &side = g.level->sides[sdi->second];
	Runtime::WallReg &reg = wr->second;

	WallFace wf = reg.wf;
	wf.shiftX = tm.extended ? (int)tm.shiftX : 0;
	wf.shiftY = tm.extended ? (int)tm.shiftY : 0;
	auto planeZ = [&](int si, int plane) { return (int)floor(g.level->sectors[si].GetPlaneTexZ(plane) + 0.5); };
	if (reg.own >= 0 && (unsigned)reg.own < g.level->sectors.Size())
	{
		wf.ownFloor = planeZ(reg.own, sector_t::floor);
		wf.ownCeil  = planeZ(reg.own, sector_t::ceiling);
	}
	if (wf.twoSided && reg.nbr >= 0 && (unsigned)reg.nbr < g.level->sectors.Size())
	{
		wf.nbrFloor = planeZ(reg.nbr, sector_t::floor);
		wf.nbrCeil  = planeZ(reg.nbr, sector_t::ceiling);
	}

	const int slotOfPart[3] = { tm.upperTexture, tm.midTexture, tm.lowerTexture };   // side_t::top, mid, bottom
	for (int part = 0; part < 3; part++)
	{
		if (!wf.twoSided && part != side_t::mid) continue;
		WallPieceShape &shape = reg.piece[part];
		FTextureID tex = side.GetTexture(part);
		if (texturesChanged)
		{
			auto plain = g.texByIndex.find(slotOfPart[part]);
			if (plain == g.texByIndex.end() || !plain->second.isValid()) continue;   // not this slot, or nothing registered
			if (!tex.isValid() && part == side_t::mid && wf.twoSided) continue;      // never wall up an opening
			FGameTexture *gt = TexMan.GetGameTexture(plain->second, false);
			if (gt == nullptr) continue;
			// The quarter turn: the engine texture's width is the picture's
			// along-the-wall count (roth_texture.cpp).
			shape.dimU = (int)gt->GetTexelWidth();
			shape.dimV = (int)gt->GetTexelHeight();
			shape.baked = false;
			tex = plain->second;
		}
		else if (!tex.isValid())
		{
			continue;
		}
		else if (shape.baked)
		{
			g.wallBakedKept++;
			continue;
		}
		if (shape.dimU <= 0 || shape.dimV <= 0) continue;   // a solid colour: no texel to place

		const WallPiece wp = WallPieceRule(wf, (WallTier)part, shape.dimU, shape.dimV);
		WallEngineSetup we = WallToEngine(wf, (WallTier)part, wp, shape.dimU, shape.dimV, reg.texelLength);
		if (!we.drawn) continue;
		if (we.baked)
		{
			// The loader's fallback for a bake it cannot make (rothmap.cpp makeSide).
			if (we.bakeV) we.yOffset += wp.v0;
			we.xOffset = wp.u0;
			we.mirrored = wp.mirror;
			we.baked = we.bakeU = we.bakeV = false;
			g.wallBakedDropped++;
		}
		if (texturesChanged)
		{
			auto mir = g.mirroredByIndex.find(slotOfPart[part]);
			if (we.mirrored && mir != g.mirroredByIndex.end() && mir->second.isValid()) tex = mir->second;
			side.SetTexture(part, tex);
		}
		side.SetTextureXScale(part, we.xScale);
		side.SetTextureYScale(part, we.yScale);
		side.SetTextureXOffset(part, we.xOffset);
		side.SetTextureYOffset(part, we.yOffset);
	}
}

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

		// The new picture goes through the wall rule like any other: a FLIP_X
		// face gets the mirrored image, a fitted piece one copy of the NEW
		// picture's size, a DRAW_FROM_BOTTOM piece the new picture's start row.
		if (g.faceToSide.count(fi) == 0) continue;
		ReapplyWallFace(fi, true);
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

// The engine-side lightlevel for a Realms light byte. Under Realms shading the
// shader reads the byte itself (R_RothShade); otherwise the old Build mapping.
static short EngineLight(uint8_t light, int shadeShift)
{
	if (g.level != nullptr && g.level->RothLighting) return (short)light;
	const int rows = 39 + ((int)light - 128);
	const int ll = rows <= 0 ? 0
		: int((255.0 * double(rows) * double(1 << shadeShift)) / 1984.0 + 0.5);
	return (short)clamp<int>(ll, 0, 255);
}

// Push a Realms sector's light byte and lights-out bit to its engine sector
// and to every engine sector registered as following it.
static void SyncSectorLight(int si)
{
	if (g.level == nullptr || si < 0 || (size_t)si >= g.map.sectors.size()) return;
	const Sector &rs = g.map.sectors[si];
	const short ll = EngineLight(rs.light, g.level->ShadeFalloffShift);
	// Desaturation carries TWO Realms flags, both from the sector's flags byte
	// at +0x0a: bit 0 is the lantern/candle bit (that byte's bit 1), bit 1 is
	// the GLOW bit (its bit 6), which draws the surface through the glow row
	// for as long as the storm phase is lit. It is a bitfield, not a flag, so
	// nothing may test it for non-zero -- see R_RothShade and the lantern test
	// in hw_drawinfo.cpp, both of which were doing exactly that.
	//
	// 618 sectors across the 44 maps carry the glow bit, 335 of them in STUDY2.
	const uint8_t lantern = (g.level->RothLighting && (rs.flags & 2)) ? 1 : 0;
	const uint8_t glow = (g.level->RothLighting && (rs.flags & 0x40)) ? 2 : 0;
	const uint8_t desat = (uint8_t)(lantern | glow);
	auto apply = [&](int es)
	{
		if (es < 0 || (size_t)es >= g.level->sectors.Size()) return;
		sector_t &sec = g.level->sectors[es];
		const bool changed = sec.Colormap.Desaturation != desat;
		sec.lightlevel = ll;
		sec.Colormap.Desaturation = desat;
		// 3D-floor light lists hold a COPY of the colormap (p_3dfloors.cpp:464),
		// so a changed lantern bit must be pushed into them explicitly.
		if (changed && sec.e != nullptr) P_RecalculateAttachedLights(&sec);
	};
	apply(si);
	auto it = g.lightFollowers.find(si);
	if (it != g.lightFollowers.end()) for (int es : it->second) apply(es);
}

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

		SyncSectorLight(si);
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
	if (mo != nullptr) mo->Angles.Yaw = DAngle::fromDeg(ObjectActorYaw(mo, rotation));
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
	// o.x, o.y are REALMS coordinates and the player's are the engine's. They
	// are the same numbers unless the loader set the object's sector aside
	// (roth_portals): then the object stands at o + shift, and a player in
	// another region is seen from there through the linked portals, which is
	// what PosRelative gives.
	double ox = double(o.x), oy = double(o.y);
	DVector3 ppos = player->Pos();
	{
		double sx = 0., sy = 0.;
		if (SectorShift(r.sector, sx, sy)) { ox += sx; oy += sy; }
		if ((unsigned)r.sector < g.level->sectors.Size())
			ppos = player->PosRelative(&g.level->sectors[r.sector]);
	}
	const double dx = ox - ppos.X;
	const double dy = oy - ppos.Y;
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
		// Bit 0x02 is the "lights out" state, and it is what the renderer
		// keys the candle (lantern) shading on (renderer.c:9194). Measured
		// 2026-10-01: STUDY1's opening switches put 139 sectors into it.
		SyncSectorLight(si);
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
	SyncSectorLight(si);
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

// THE MOVERS' SOUND. 0x07 and 0x03 may link a sound node by key (+0x10 and
// +0x14). The first sweep that moves starts it (the 0x32e2f finalize,
// raw_commands.c:2545-2552); the exit, when the move has run out, stops it and
// plays the record's end sound at the node (:2516-2535, 2743-2764). Both exits
// need the link: a record with an end sound and no node plays nothing.
static void LinkMoverSound(Effect &e, uint16_t key)
{
	if (key != 0) e.sfxNode = SoundFindNodeByKey(key);
}

static void MoverSoundFinalize(Effect &e)
{
	if (e.sfxNode < 0 || e.sfxStarted) return;
	e.sfxStarted = true;
	SoundMoverFinalize(e.sfxNode);
}

static void MoverSoundExit(Effect &e, uint16_t endSoundPlusOne, bool asCommandSound)
{
	if (e.sfxNode < 0) return;
	const bool was = e.sfxStarted;
	e.sfxStarted = false;
	SoundMoverExit(e.sfxNode, was, endSoundPlusOne, asCommandSound);
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

// DRAW_FROM_BOTTOM PIECES RIDE THE EDGE THEY ARE PINNED TO.
//
// A wall piece with face flag 0x80 has its texture's bottom row on the piece's
// bottom edge (renderer.c:5057-5063). The loader expresses that as a row
// offset from the engine's top anchor, texH - pieceHeight * sy (rothmap.cpp,
// "FF_PIN_BOTTOM"), which is right for the heights the map starts with and
// goes stale the moment either sector's plane moves. So when a plane moves by
// `delta`, every such piece whose height it changes takes the opposite change
// in its offset. Four cases, by which edge of the piece the plane is:
//
//   this sector's own UPPER piece  top edge = this ceiling       height += delta
//   the neighbour's UPPER piece    bottom edge = this ceiling    height -= delta
//   this sector's own LOWER piece  bottom edge = this floor      height -= delta
//   the neighbour's LOWER piece    top edge = this floor         height += delta
//
// STUDY1's corridor-end painting is the neighbour's-upper case: the picture is
// sector 56's upper piece over sector 57, and record 9 rolls it up by raising
// 57's ceiling from 500 to 600. In the original the whole picture slides up
// into the wall; with the stale offset it stood still and was cut away from
// the bottom.
//
// An image-fit piece (flag 0x04) is one copy tall whatever its height and has
// no such term (renderer.c:13343); the loader leaves it out and so does this.
static void RepinWallPieces(int si, bool ceiling, double delta)
{
	const Sector &rs = g.map.sectors[si];
	if (rs.firstFaceIndex < 0) return;
	for (int k = 0; k < (int)rs.faceCount; k++)
	{
		const int fi = rs.firstFaceIndex + k;
		if (fi < 0 || (size_t)fi >= g.map.faces.size()) continue;
		const int pair[2] = { fi, g.map.faces[fi].sister };
		if (pair[1] < 0 || (size_t)pair[1] >= g.map.faces.size()) continue;   // one-sided: one mid piece, no edge of it moves with a neighbour
		for (int w = 0; w < 2; w++)
		{
			const Face &f = g.map.faces[pair[w]];
			if (f.textureMap < 0 || (size_t)f.textureMap >= g.map.textureMaps.size()) continue;
			const uint8_t tf = g.map.textureMaps[f.textureMap].flags;
			if (!(tf & FF_PIN_BOTTOM) || (tf & FF_IMAGE_FIT)) continue;
			auto it = g.faceToSide.find(pair[w]);
			if (it == g.faceToSide.end()) continue;
			if (it->second < 0 || (unsigned)it->second >= g.level->sides.Size()) continue;
			side_t *sd = &g.level->sides[it->second];
			const int part = ceiling ? side_t::top : side_t::bottom;
			if (!sd->GetTexture(part).isValid()) continue;
			// own piece (w == 0) or the neighbour's piece looking at this sector (w == 1)
			const double heightChange = ceiling ? (w == 0 ? delta : -delta)
			                                    : (w == 0 ? -delta : delta);
			sd->AddTextureYOffset(part, -heightChange * sd->GetTextureYScale(part));
		}
	}
}

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

	// roth_platforms: this plane was taken out of the static map and given to a
	// platform actor, whose box is re-measured from the Realms height every
	// world step (DrivePlatforms). The engine plane underneath is the
	// neighbours' and must stay where the loader put it.
	if (PlatformOwnsPlane(si, ceiling)) return;

	sector_t *sec = &g.level->sectors[si];
	const int which = ceiling ? sector_t::ceiling : sector_t::floor;
	if (ceiling) sec->ceilingplane.setD(newZ);
	else         sec->floorplane.setD(-newZ);
	sec->SetPlaneTexZ(which, newZ);
	RepinWallPieces(si, ceiling, newZ - oldZ);
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
//   (The SOUND is done: the registrar links an SFX node through +0x10 and the
//   exits start and stop it. See "THE MOVERS' SOUND" above.)
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
	LinkMoverSound(e, rec->Word(0x10));              // raw_commands.c:4498-4505
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

	if (overshoot)
	{
		MoverSoundExit(e, rec->Word(0x12), false);     // play_entity_sound, :2528-2533
		return TickHeightExit(e, rec);
	}
	MoverSoundFinalize(e);                         // :2545-2552
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

	// roth_platforms: a plane the loader gave to a platform actor wears the
	// NEIGHBOURS' flat in the static map; re-applying this sector's own would
	// paint the moving piece's picture onto the floor it left. The box's mesh is
	// built once at load, so a flat SWAP on such a sector is not shown -- no
	// retail record does one (tools/platforms), and it is counted if one does.
	if (PlatformOwnsPlane(si, !isFloor))
	{
		g.unhandledOps[isFloor ? 0x0A : 0x0B]++;
		return;
	}

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
	const FTextureID tex = it->second.isSky ? FNullTextureID() : it->second.tex;

	// "Nothing here" becomes the sky, exactly as at load: a flat must draw
	// something or the sector renders hall of mirrors.
	sec->SetTexture(which, tex.isValid() ? tex : skyflatnum, false);

	// THROUGH roth_surface -- the same call the loader makes.
	//
	// This used to be a second, divergent copy of the rule: it recomputed the
	// scale and silently dropped the MIRROR bits, so a platform changed
	// appearance the moment the level logic moved it or swapped its texture.
	// There is now one implementation and no way for the two to disagree.
	roth::FlatSetup fs = {};
	fs.textureWord = (uint16_t)index;
	fs.scaleBits = (uint8_t)shift;
	fs.shiftX = (uint8_t)shx;
	fs.shiftY = (uint8_t)shy;

	const uint8_t flipBits = (uint8_t)(rs.flags2 >> 8);
	fs.mirrorX = isFloor ? (flipBits & 0x01) != 0 : (flipBits & 0x04) != 0;
	fs.mirrorY = isFloor ? (flipBits & 0x02) != 0 : (flipBits & 0x08) != 0;
	fs.opaque256 = it->second.opaque256;

	const roth::FlatEngineSetup fe = roth::FlatToEngine(fs);
	sec->SetXScale(which, fe.xScale);
	sec->SetYScale(which, fe.yScale);
	sec->SetXOffset(which, fe.xOffset);
	sec->SetYOffset(which, fe.yOffset);
	sec->SetAngle(which, DAngle::fromDeg(fe.angle));   // the quarter turn, as at load
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

	// THROUGH roth_surface, exactly as the loader does (rothmap.cpp, midFlat).
	// This used to keep its own copy -- a positive 1/2^s scale, raw unscaled
	// shifts and no quarter turn -- so any script that moved a platform put the
	// rug and table-top error straight back. No mirrors: a mid-platform's span
	// setup writes a bare 0x38/0xb8 (renderer.c:9339, :9354).
	auto opaque256 = [](int index)
	{
		auto it = g.flatByIndex.find(index);
		return it != g.flatByIndex.end() && it->second.opaque256;
	};
	auto midFlat = [&](uint16_t texWord, uint8_t s, uint8_t shX, uint8_t shY)
	{
		roth::FlatSetup f = {};
		f.textureWord = texWord;
		f.scaleBits = s;
		f.shiftX = shX;
		f.shiftY = shY;
		f.opaque256 = opaque256((int)texWord);
		return roth::FlatToEngine(f);
	};
	const roth::FlatEngineSetup top = midFlat(mp.topTexture, (uint8_t)((mp.scales >> 4) & 3),
		mp.topShiftX, mp.topShiftY);
	const roth::FlatEngineSetup und = midFlat(mp.undersideTexture, (uint8_t)((mp.scales >> 2) & 3),
		mp.undersideShiftX, mp.undersideShiftY);
	cs->SetXScale(sector_t::ceiling, top.xScale);
	cs->SetYScale(sector_t::ceiling, top.yScale);
	cs->SetXOffset(sector_t::ceiling, top.xOffset);
	cs->SetYOffset(sector_t::ceiling, top.yOffset);
	cs->SetAngle(sector_t::ceiling, DAngle::fromDeg(top.angle));
	cs->SetXScale(sector_t::floor, und.xScale);
	cs->SetYScale(sector_t::floor, und.yScale);
	cs->SetXOffset(sector_t::floor, und.xOffset);
	cs->SetYOffset(sector_t::floor, und.yOffset);
	cs->SetAngle(sector_t::floor, DAngle::fromDeg(und.angle));
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

	(void)tm;

	// Through the wall rule (ReapplyWallFace): this used to write offX = shiftX
	// and offY = +-shiftY onto all three pieces, which dropped the anchor term,
	// the DRAW_FROM_BOTTOM start row and IMAGE_FIT's zeroed shifts.
	auto faces = g.texmapToFaces.find(tmi);
	if (faces == g.texmapToFaces.end()) return;
	for (int fi : faces->second) ReapplyWallFace(fi, false);
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

	(void)tm;

	// Pictures and shifts together, through the wall rule (ReapplyWallFace).
	for (int fi : faces->second) ReapplyWallFace(fi, true);
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

	// WHICH FACE. The original does geom + word[rec+8], a raw offset -- but
	// MEASURED against the retail data that resolves 0 times out of 8 in STUDY1,
	// so our offset base is not the one it indexes from. The reader's ordinary
	// two-hop key resolution is tried as well, and whichever answers is used;
	// the load report counts both so the reading stays evidence-backed rather
	// than a transcription that happens to compile.
	int faceIdx = -1;
	auto fit = g.map.faceByOffset.find((uint32_t)rec->key);
	if (fit != g.map.faceByOffset.end() && (size_t)fit->second < g.map.faces.size())
		faceIdx = fit->second;
	else if (!rec->faces.empty() && rec->faces[0] >= 0
	         && (size_t)rec->faces[0] < g.map.faces.size())
		faceIdx = rec->faces[0];

	if (faceIdx < 0)
	{
		g.unhandledOps[e.tick]++;        // the key named no face either way
		return TickFinalize(e, rec, rec->Word(0x0E));
	}

	Face &fa = g.map.faces[faceIdx];

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
// tms_step -- raw_commands.c, the clamp shared by every sub-pass of 0x03.
//
// One step of a 16-bit height toward a limit, with a signed budget. Returns
// whether it moved, which is what decides the whole tick's exit. `decr` says
// whether a clamp CONSUMES the budget by the surplus -- two of the sub-passes
// pass false, so their move does not bill the shared allowance.
//
//==========================================================================

static bool TmsStep(int16_t &field, int16_t lim, int16_t &bud, bool desc, bool decr)
{
	const int16_t cur = field;
	if (desc ? (cur <= lim) : (cur >= lim)) return false;

	const int16_t sum = (int16_t)(cur + bud);
	int16_t stored;
	if (desc ? (sum > lim) : (sum < lim))
	{
		stored = sum;
	}
	else
	{
		if (decr) bud = (int16_t)(bud - (int16_t)(sum - lim));
		stored = lim;
	}
	field = stored;
	return true;
}

//==========================================================================
//
// cmd_modify_sector -- raw_commands.c:4954, RAW command base 0x03
// tick_modify_sector -- raw_commands.c:2590, the largest tick in the game
//
// The compound mover: it sinks a group of floors, and optionally the slab inside
// each of them and a per-sector byte, all on one shared budget. This is what a
// Realms stone floor grinding down into the ground is made of.
//
// Register and toggle are 0x07's, with one difference worth noting because it is
// the sort of thing that silently breaks the neighbouring field: the linked-SFX
// key is read from +0x14 here, not +0x10 as 0x07 reads it.
//
// What +0x06's low bits select:
//     0            SIMPLE -- the members' floors and nothing else
//     bit 0 (1)    also the sector's mid-platform, its top and its underside
//     bit 1 (2)    also the sector's +0x0c byte, ramped alongside
//
// THE BUDGET IS RESET PER MEMBER HERE, unlike 0x07 where it accumulates across
// the group. So every floor in the group gets the full per-frame allowance rather
// than sharing one, and they move together instead of in sequence. Transcribed as
// written; it reads like a difference the original meant.
//
// TWO THINGS ARE NOT IMPLEMENTED, both stated rather than approximated:
//
//   THE ASCENDING SWEEP. Descending drives every member toward one limit,
//   2 * +0x0a, which is right there in the record. Ascending instead reads a
//   per-member array of SNAPSHOT targets through a pointer at the record's +0x10,
//   dereferenced twice -- and nothing in ROTH.C that I could find writes that
//   pointer. The registrar does not. Without knowing what it points at, the
//   targets cannot be reproduced, and inventing them would be inventing the
//   distances every one of these floors travels. So the ascending half is counted
//   and the record is left registered rather than moved wrongly.
//
//   (THE SOUNDS are done, as in 0x07, for the descending half: the node is
//   linked through +0x14 and the end sound is the word at +0x0c.)
//
// WHAT THE +0x0c BYTE IS, now settled and it was neither guess. RAW.md calls it
// TEXTURE_MAP_OVERRIDE: it overrides the position and SIZE of the MID_TEXTURE on
// this sector's double-sided faces that carry TRANSPARENT and
// TRANSPARENT_FIXED_SIZE. The value is the size, 0 is fit-to-size, negative
// anchors to the floor and positive to the ceiling. This port had guessed
// "overrideHeight" and ROTH.C's comment guessed "sector light"; both were wrong,
// and the confirmed brightness is +0x0b.
//
// So this sub-pass animates a transparent mid-texture's SIZE alongside the floor
// move, which is a sensible thing for a sinking floor to do. The ramp below was
// already faithful to the code; it is only now legible. Nothing in the loader
// consumes the field yet, so it still has no visible effect.
//
//==========================================================================

int ModifySector(Command *rec, int index)
{
	const uint8_t base = (uint8_t)(rec->opcode & 0x7f);

	if (rec->modifier & 0x21)                        // TOGGLE, as 0x07
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
	LinkMoverSound(e, rec->Word(0x14));              // raw_commands.c:4968-4975
	e.flags5 |= (rec->modifier & 0x02) ? 0 : 0x80;
	g.effects.push_back(e);
	rec->modifier |= 0x20;
	SyncDisabled(rec);
	return CMD_ACTED;
}

static bool TickModifySector(Effect &e)
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

	if (e.flags5 & 0x80)
	{
		// The ascending sweep. See the note above: its targets come from a
		// snapshot this port cannot locate the source of.
		g.unhandledOps[e.tick]++;
		return false;
	}

	const uint8_t f6 = rec->fireFlags;
	const int16_t master = (int16_t)(-delta);
	const int16_t limD = (int16_t)(2 * (int32_t)(int16_t)rec->Word(0x0A));

	// The companion ramp's per-frame amount, carried at half rate with its odd
	// bit kept in the effect -- the same trick the scrolls use.
	int32_t companion = 0;
	if (f6 & 0x03)
	{
		const uint32_t acc = (uint32_t)delta + (uint32_t)(e.flags5 & 0x01 ? 1u : 0u);
		companion = -(int32_t)(acc >> 1);
	}

	bool moved = false;
	for (int si : e.sectors)
	{
		if (si < 0 || (size_t)si >= g.map.sectors.size()) continue;
		Sector &rs = g.map.sectors[si];

		// Reset per member, NOT shared across the group. See above.
		int16_t bud = master;

		int16_t floorH = rs.floorHeight;
		if (TmsStep(floorH, limD, bud, true, true))
		{
			moved = true;
			MoveSectorPlane(si, false, floorH);
		}

		if ((f6 & 0x02) && !(rs.flags & 0x01) && (int8_t)rs.textureMapOverride < -1)
		{
			int32_t pos = -(int32_t)(int8_t)rs.textureMapOverride;
			int32_t ee = pos + companion;
			if (!(ee > 1)) ee = 1;
			rs.textureMapOverride = (int8_t)(-(int8_t)(uint8_t)ee);
			moved = true;
		}

		if (f6 & 0x01)
		{
			const int pi = rs.platformIndex;
			if (pi >= 0 && (size_t)pi < g.map.platforms.size())
			{
				MidPlatform &mp = g.map.platforms[pi];
				int16_t bud2 = master;
				int16_t top = mp.topZ;
				if (TmsStep(top, limD, bud2, true, true)) { mp.topZ = top; moved = true; }

				// The second pass does NOT bill the budget -- decr is false.
				int16_t rem = master;
				int16_t und = mp.undersideZ;
				if (TmsStep(und, limD, rem, true, false)) { mp.undersideZ = und; moved = true; }

				ApplyPlatform(si);
			}
		}
	}

	if (!moved)
	{
		MoverSoundExit(e, rec->Word(0x0C), true);      // play_command_sound, :2756-2762
		return TickHeightExit(e, rec);
	}
	MoverSoundFinalize(e);                             // :2731-2741
	return false;
}

//==========================================================================
//
// cmd_move_sector -- raw_commands.c:4588, RAW command base 0x09
// tick_moving_sector -- raw_commands.c:5869
//
// The one effect that slides geometry SIDEWAYS. It adds a signed per-frame
// delta to one coordinate -- x or y, chosen by bit 0x40 of +0x06 -- of the
// sector's four vertices, until the leading vertex reaches a limit held in the
// record. Because the neighbouring sectors share those vertices they stretch
// and shrink to follow, which is what makes it look like a platform crossing a
// pit, a floor plate travelling down a corridor or a block sliding in a grid.
//
// WHAT IS TRANSCRIBED AND WHAT IS NOT. The LOGIC is the original's, in the
// original's own 16-bit vertex coordinates: which vertices, the per-frame
// budget, the clamp, the two limits, the dwell and turn-around, the toggle,
// and the fold of the delta into the flats' shift bytes. It runs on
// Map::vertices, the Realms state, whether or not anything can show it.
//
// The PRESENTATION is this port's. A Doom BSP cannot move a vertex, so the
// level's own vertices never move; the loader turned the sector into a platform
// actor where roth_platformplan.h says that is the same picture (roth_platforms,
// src/roth/roth_platforms.h), and DrivePlatforms below tells that actor how far
// the Realms vertices have travelled. A moving sector the plan could not turn
// into a platform still runs here, invisibly, so that its record's state bits
// and its timing stay true for the commands that test them. It is counted.
//
// THE CARRY IS DELEGATED, as it is for MoveSectorPlane, and for the same
// reason. The original moves the player and the sector's objects itself
// (apply_moving_sector_carry, renderer.c:985): a pure position add with no wall
// test, for a player standing EXACTLY on the sector's floor (+0x06 bit 0x01) or
// on its mid-platform's top (bit 0x04), and for objects only in a sector whose
// +0x16 has bit 0x02. Here whatever stands on the platform actor rides it,
// through the platform library, with collision. Every record the plan builds
// has the carry bit for the surface it makes walkable, so the two agree on WHO
// rides; they differ in that ours can be stopped by a wall.
//
// All 32 retail records are 18 bytes: +0x06 flags, +0x07 speed, +0x08 the
// sector id, +0x0a and +0x0c the two limits, +0x0e the dwell.
//
//==========================================================================

// A vertex coordinate as the ORIGINAL holds it. The reader recentres a map that
// straddles the 16-bit wrap (RAQUIA2), and every comparison below is the
// original's signed 16-bit one, so it has to be made on the original's value.
static int16_t RawVertexCoord(int vi, bool axisX)
{
	const Vertex &v = g.map.vertices[vi];
	return axisX ? (int16_t)(uint16_t)(v.x - g.map.wrapShiftX)
	             : (int16_t)(uint16_t)(v.y - g.map.wrapShiftY);
}

int MoveSector(Command *rec, int index)
{
	const uint8_t base = (uint8_t)(rec->opcode & 0x7f);

	// TOGGLE (raw_commands.c:4590-4598). Tested on bit 0x20 ALONE here, where
	// 0x07 and 0x03 test 0x21 -- the armed bit does not send this one down the
	// toggle path.
	if (rec->modifier & 0x20)
	{
		if (rec->Word(0x0E) != 0) return CMD_NOTHING;          // :4591
		if (!(rec->fireFlags & 0x20)) return CMD_NOTHING;      // :4592
		Effect *eff = FindEffect(base, (uint16_t)index);       // :4593
		if (eff == nullptr) return CMD_NOTHING;
		eff->flags5 ^= 0x80;                                   // :4595
		rec->modifier ^= 0x02;                                 // :4596
		SyncDisabled(rec);
		return CMD_ACTED;
	}

	// REGISTER. The FIRST sector carrying the id (find_geometry_record, :4600),
	// not a group: this opcode moves one sector.
	const int si = FindSectorByCommandID(rec->key);
	if (si < 0) return CMD_NOTHING;                            // :4601

	// vertex1 of its first four faces (:4609-4616). The original reads four
	// face slots whatever the sector's face count; with fewer than four it
	// would read the next sector's. There is nothing faithful to read here
	// then, so such a record does nothing. All 32 retail ones have four.
	Effect e;
	if (!rothplat::MovedVertices(g.map, si, e.moveVerts))
	{
		g.unhandledOps[base]++;
		return CMD_NOTHING;
	}

	// Sorted ascending by the coordinate that will move (:4617-4623): a bubble
	// sort that swaps only when strictly greater, so equal coordinates keep
	// their face order.
	const bool axisX = (rec->fireFlags & 0x40) != 0;           // :4606
	for (int pass = 0; pass < 3; pass++)
		for (int k = 0; k < 3 - pass; k++)
			if (RawVertexCoord(e.moveVerts[k], axisX) > RawVertexCoord(e.moveVerts[k + 1], axisX))
				std::swap(e.moveVerts[k], e.moveVerts[k + 1]);

	e.tick = base;                                             // :4629
	e.record = (uint16_t)index;                                // :4604
	e.sectors.push_back(si);                                   // chunk[0xc], :4605
	e.payload = 0;
	rec->modifier |= 0x20;                                     // :4626
	e.flags5 |= (rec->modifier & 0x02) ? 0 : 0x80;             // :4627-4628
	g.effects.push_back(e);
	SyncDisabled(rec);

	if (!SectorIsPlatform(si)) g.moversUnshown++;
	return CMD_ACTED;
}

static bool TickMovingSector(Effect &e)
{
	Command *rec = Rec(e.record);
	if (rec == nullptr) return true;

	// A disabled record (raw_commands.c:5873). AS LIFTED this returns with EAX
	// still holding the effect record -- nonzero -- and the pool walk frees any
	// effect whose tick returns nonzero (:3317). So disabling a 0x09 record
	// drops its effect, while the record keeps its registered bit. Transcribed
	// as written; it is the one tick in the family that does this.
	if (rec->modifier & 0x08) return true;

	if (e.flags5 & 0x40) return TickArmedCountdown(e);         // :5875-5876

	if (e.sectors.empty() || e.moveVerts[0] < 0) return true;
	const int si = e.sectors[0];
	if (si < 0 || (size_t)si >= g.map.sectors.size()) return true;

	const bool axisX = (rec->fireFlags & 0x40) != 0;
	const bool descending = (e.flags5 & 0x80) != 0;            // :5881
	const int32_t speed = (int32_t)rec->Byte(0x07);            // :5879

	// Going UP the coordinate, the LARGEST vertex is the one tested and the
	// limit is -2 * +0x0a (:5882-5884); going down, the SMALLEST and
	// -2 * +0x0c (:5886-5888). Both products are truncated to 16 bits.
	int32_t budget32;
	int16_t target;
	int probe;
	if (!descending)
	{
		budget32 = ROTH_FRAME_TICKS_PER_TIC * speed;
		target = (int16_t)(-2 * (int32_t)(int16_t)rec->Word(0x0A));
		probe = e.moveVerts[3];
	}
	else
	{
		budget32 = -(ROTH_FRAME_TICKS_PER_TIC * speed);
		target = (int16_t)(-2 * (int32_t)(int16_t)rec->Word(0x0C));
		probe = e.moveVerts[0];
	}

	const int16_t current = RawVertexCoord(probe, axisX);
	if (current == target) return TickHeightExit(e, rec);      // :5891

	// The step, cut short so the tested vertex lands ON the limit (:5892-5896).
	int16_t budget = (int16_t)budget32;
	const int16_t reached = (int16_t)(current + budget);
	if ((!descending && reached > target) || (descending && reached < target))
		budget = (int16_t)(budget - (int16_t)(reached - target));

	// All four vertices take the same step (:5899-5903), in 16 bits.
	for (int k = 0; k < 4; k++)
	{
		Vertex &v = g.map.vertices[e.moveVerts[k]];
		if (axisX) v.x = (int16_t)(v.x + budget); else v.y = (int16_t)(v.y + budget);
	}
	std::pair<int, int> &slid = g.movers[si];
	if (axisX) slid.first += budget; else slid.second += budget;

	// The carry (:5905-5916) is the platform actor's. See the note above.

	// The low byte of the step goes into the shift bytes the record's low four
	// bits pick (:5917-5929): the sector's floor and ceiling flats, then its
	// mid-platform's top and underside. This is what keeps a flat's picture ON
	// a sector that moves, where it would otherwise stay pinned to the world.
	// The X byte for an X move, the Y byte for a Y move.
	const uint8_t f6 = rec->fireFlags;
	const uint8_t low = (uint8_t)budget;
	Sector &rs = g.map.sectors[si];
	if (f6 & 0x01) { if (axisX) rs.floorShiftX += low; else rs.floorShiftY += low; }
	if (f6 & 0x02) { if (axisX) rs.ceilShiftX += low;  else rs.ceilShiftY += low; }
	if ((f6 & 0x0C) && rs.platformIndex >= 0 && (size_t)rs.platformIndex < g.map.platforms.size())
	{
		MidPlatform &mp = g.map.platforms[rs.platformIndex];
		if (f6 & 0x04) { if (axisX) mp.topShiftX += low; else mp.topShiftY += low; }
		if (f6 & 0x08) { if (axisX) mp.undersideShiftX += low; else mp.undersideShiftY += low; }
	}
	return false;                                              // :5930
}

//==========================================================================
//
// The load-time pass for armed 0x09 records -- init_loaded_object_table's
// second pass (map_load.c:1179-1197) through the per-type table at 0x30998,
// whose entry for type 0x09 is tick_rerun_command_execute (raw_commands.c:5278).
//
// This is how a Realms platform is already crossing its pit when the player
// arrives: the record is stored ARMED (modifier bit 0x01), and at load every
// armed record has its arm and registered bits cleared, is run once through
// the ordinary command table -- which for 0x09 registers the effect -- and is
// armed again. A record that is also DISABLED is left with its arm bit clear
// and is not run, exactly as written (:5281-5282).
//
// The same table entry serves 0x0d, 0x0e, 0x0f, 0x11 and 0x23. Only 0x09 is
// done here, behind roth_platforms; the others are outside this change.
//
// First visit only in the original (it tests g@0x89f5c, the revisit count, in
// the sibling handlers); this port has no revisit state, as the light-switch
// pass above says.
//
//==========================================================================

// The same re-run for 0x07, the tail of its own init handler
// (tick_apply_geometry_effect falls into rawcmd_tick_rerun, raw_commands.c:
// 5270 -> :5278). The first half of that handler -- writing the record's first
// limit into its sectors -- is roth::ApplyLoadTimeHeights (roth_raw.cpp), which
// the loader runs on the map before it builds anything, so the heights in
// g.map and in the engine's planes already agree when this runs. Not behind
// roth_platforms: a 0x07 moves planes, which the engine has always done.
static void InitChangeHeightAtLoad(Log *log)
{
	int records = 0, armed = 0, started = 0;
	for (size_t i = 0; i < g.map.commands.size(); i++)
	{
		Command *rec = &g.map.commands[i];
		if (rec->isTrigger || (rec->opcode & 0x7f) != 0x07) continue;
		records++;
		if (rec->modifier & 0x08) continue;                    // :5245, before the rerun is reached
		if (!(rec->modifier & 0x01)) continue;                 // :5280
		armed++;
		rec->modifier &= 0xDE;                                 // :5281
		SyncDisabled(rec);
		if (rec->modifier & 0x08) continue;                    // :5282
		if (ChangeHeight(rec, (int)i + 1) == CMD_ACTED) started++;   // :5283-5290
		rec->modifier |= 0x01;                                 // :5292
	}
	if (log != nullptr)
		log->Line("  change-height    %d 0x07 record(s), %d armed at load, %d started",
			records, armed, started);
}

static void InitMovingSectorsAtLoad(Log *log)
{
	int armed = 0, started = 0;
	for (size_t i = 0; i < g.map.commands.size(); i++)
	{
		Command *rec = &g.map.commands[i];
		if (rec->isTrigger || (rec->opcode & 0x7f) != 0x09) continue;
		if (!(rec->modifier & 0x01)) continue;                 // :5280
		armed++;
		rec->modifier &= 0xDE;                                 // :5281
		SyncDisabled(rec);
		if (rec->modifier & 0x08) continue;                    // :5282
		if (MoveSector(rec, (int)i + 1) == CMD_ACTED) started++;   // :5283-5290
		rec->modifier |= 0x01;                                 // :5292
	}
	if (log != nullptr)
		log->Line("  moving sectors   %d armed 0x09 record(s) at load, %d started"
			" (roth_platforms)", armed, started);
}

//==========================================================================
//
// DrivePlatforms -- once per world step, after the effect pool has ticked.
//
// Hands every platform sector's CURRENT Realms state to its actors: how far
// the 0x09 ticks have slid its vertices since load, its floor and ceiling, its
// mid-platform's two heights, and its light. All of those can change without a
// 0x09 running -- 0x07 raises RAQUIA2's travelling floor plate, 0x03 sinks
// RAQUIA3's slabs, a light switch darkens the room -- which is why this is a
// pass over the sectors and not something the 0x09 tick does on its way out.
//
// The light is read back from the ENGINE sector, which every light opcode
// already keeps in step (SyncSectorLight), so the box is lit like the sector it
// came from wherever it has slid to -- as in the original, where the light
// byte travels with the sector record.
//
//==========================================================================

static void DrivePlatforms()
{
	if (!g.platformsOn || g.level == nullptr) return;
	for (int si : g.platformSectors)
	{
		if (si < 0 || (size_t)si >= g.map.sectors.size()) continue;
		const Sector &rs = g.map.sectors[si];

		PlatformHeights h;
		h.floorZ = double(rs.floorHeight);
		h.ceilingZ = double(rs.ceilingHeight);
		if (rs.platformIndex >= 0 && (size_t)rs.platformIndex < g.map.platforms.size())
		{
			h.slabBottom = double(g.map.platforms[rs.platformIndex].undersideZ);
			h.slabTop = double(g.map.platforms[rs.platformIndex].topZ);
		}

		double dx = 0., dy = 0.;
		auto mv = g.movers.find(si);
		if (mv != g.movers.end()) { dx = double(mv->second.first); dy = double(mv->second.second); }

		const int light = (size_t)si < g.level->sectors.Size()
			? (int)g.level->sectors[si].lightlevel : -1;
		DrivePlatform(g.level, si, dx, dy, h, light);
	}
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
		case 0x03: done = TickModifySector(g.effects[i]); break;
		case 0x09: done = TickMovingSector(g.effects[i]); break;
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
	case 0x02: case 0x03: case 0x07: case 0x10: case 0x0A: case 0x0B: case 0x0D: case 0x0C: case 0x0E: case 0x0F: case 0x11: case 0x1D: case 0x2F: case 0x34: case 0x23: case 0x24: case 0x36: case 0x38: case 0x3B: case 0x3E: case 0x40:
		return true;
	// 0x09 only when the loader was told to build platforms for it. With
	// roth_platforms 0 it is not implemented, exactly as before this existed,
	// and the dispatcher counts it.
	case 0x09:
		return g.platformsOn;
	default:
		return false;
	}
}

// cmd_activate_sfx_node -- raw_commands.c:4855, RAW command base 0x10.
//
// Turns the map's sound nodes on and off by key: +0x08 is the key, +0x06 says
// how (toggle; bits 0x06 force on; bit 0x04 forces off; bit 0x01 also stops
// the voice of a node it just turned off). A record with no matching node
// does nothing and reports nothing. Otherwise it disables itself unless
// +0x06 has bit 0x20, and reports ACTED. The node work is roth_sound's.
int ActivateSfxNode(Command *rec)
{
	if (SoundActivateNodes(rec->key, rec->fireFlags) == 0) return CMD_NOTHING;   // :4861
	if (!(rec->fireFlags & 0x20)) { rec->modifier |= 0x08; SyncDisabled(rec); }  // :4876
	return CMD_ACTED;
}

//==========================================================================
//
// The command core, joined to the engine.
//
// roth_cmdcore.cpp holds the rules and the state, engine-free and checked
// against the original by tools/cmdcore_test. What is here only tells the
// engine about a change: a repainted wall or floor, a prop's new picture, the
// player's health and facing, and the particle actors.
//
//==========================================================================

AActor *CmdPlayer()
{
	if (g.level == nullptr) return nullptr;
	for (int i = 0; i < MAXPLAYERS; i++)
		if (g.level->PlayerInGame(i) && g.level->Players[i]->mo != nullptr) return g.level->Players[i]->mo;
	return nullptr;
}

class CmdEngineHost : public CmdHost
{
public:
	bool ItemAutoSelected() override { return g_core.IsAttached() && g_core.Items().AutoSelected(); }
	void RunPendingRecord(uint16_t idx) override { Command *p = Rec(idx); if (p != nullptr) RunDbase100Record(p); }
	bool QueueDeferred(uint16_t chain) override
	{
		// The front buffer holds sixteen (raw_commands.c:4033). {NEXT, 0}: no
		// wall, no source object, when it runs.
		if (g.deferred.size() >= 0x10) return false;
		Deferred d; d.chain = chain; d.context = 0;
		if (chain != 0) g.deferred.push_back(d);
		return true;
	}
	void PlayerXY(int16_t &x, int16_t &y) override
	{
		// g_player_x/y are Realms coordinates; a player standing in a sector the
		// loader set aside (roth_portals) is moved back by that sector's shift.
		x = y = 0;
		AActor *mo = CmdPlayer();
		if (mo == nullptr) return;
		double px = mo->X(), py = mo->Y(), sx = 0., sy = 0.;
		if (mo->Sector != nullptr && SectorShift(mo->Sector->Index(), sx, sy)) { px -= sx; py -= sy; }
		x = (int16_t)(int)floor(px);
		y = (int16_t)(int)floor(py);
	}
	uint16_t PlayerAngle() override
	{
		AActor *mo = CmdPlayer();
		return mo != nullptr ? (uint16_t)DoomYawToRoth512(mo->Angles.Yaw) : 0;
	}
	void SetPlayerAngle(uint16_t a) override
	{
		// The original's angle word keeps 16 bits but every reader masks it to
		// 0x1ff, 512 to the turn, 0 facing +Y (RothAngleToDoom, roth_diff.cpp).
		AActor *mo = CmdPlayer();
		if (mo == nullptr) return;
		mo->Angles.Yaw = DAngle::fromDeg(90.0 + (double)(a & 0x1ff) * (360.0 / 512.0));
	}
	void TexmapChanged(int tmi) override { ApplyTexmapFull(tmi); }
	void SectorFlatChanged(int si) override { ApplySectorFlat(si, true); ApplySectorFlat(si, false); }
	void PlatformChanged(int si) override { ApplyPlatform(si); }
	void ObjectChanged(int si, int oi) override
	{
		if (si < 0 || (size_t)si >= g.map.objects.size() || oi < 0 || (size_t)oi >= g.map.objects[si].size()) return;
		const Object &o = g.map.objects[si][oi];
		ObjectRef r; r.sector = si; r.index = oi;
		ApplyObjectTexture(r, (uint16_t)(o.textureIndex | (o.textureSource << 8)));
	}
};

CmdCore g_cmd;
CmdEngineHost g_cmdHost;
// The engine health the core's last value maps to, so a change made by
// anything else (the engine's own damage) is noticed and taken in.
int g_cmdHealthSeen = -1;

// Health runs 0..0x800 in Realms and 0..100 here: rounded up, so the player is
// never shown 0 while alive.
int CmdEngineHealth(int32_t h) { return h <= 0 ? 0 : (int)((h * 100 + 0x7ff) / 0x800); }

// Before a command reads the player's health, take in whatever the engine did
// to it since the core last looked.
void CmdPullHealth()
{
	AActor *mo = CmdPlayer();
	if (mo == nullptr || mo->player == nullptr) return;
	if (mo->health != g_cmdHealthSeen && mo->health != CmdEngineHealth(g_cmd.health))
		g_cmd.health = mo->health <= 0 ? 0 : (int32_t)((mo->health * 0x800) / 100);
	g_cmdHealthSeen = mo->health;
	g_flow.SetHealth(g_cmd.health);   // one health: the flow holds it, the core mirrors it
}

// After: hand the change to the engine. Down goes through P_DamageMobj, so a
// hurt player flinches and a dead one dies the engine's way; the flash is the
// original's own level (g_damage_flash_level, decaying 8 a tic: game_core.c:
// 337-342) turned into the engine's red-palette count, which runs out 1 a tic.
void CmdPushHealth()
{
	AActor *mo = CmdPlayer();
	if (mo == nullptr || mo->player == nullptr) return;
	g_flow.SetHealth(g_cmd.health);
	const int want = CmdEngineHealth(g_cmd.health);
	if (want < mo->health && mo->health > 0)
		P_DamageMobj(mo, nullptr, nullptr, mo->health - want, NAME_None, DMG_NO_ARMOR | DMG_THRUSTLESS | DMG_NO_ENHANCE | DMG_FORCED);
	else if (want > mo->health && mo->health > 0)
		mo->health = mo->player->health = want;
	const int flash = g_cmd.damageFlash >> 4;
	if (flash > mo->player->damagecount) mo->player->damagecount = flash > 100 ? 100 : flash;
	g_cmdHealthSeen = mo->health;
}

// A DBASE100 health op changed the flow's value: the core mirrors it and the
// engine's player follows (P_DamageMobj on a loss, as for the RAW opcodes).
void CmdHealthFromFlow() { g_cmd.health = g_flow.Health(); CmdPushHealth(); }

void CmdHealHealth(uint32_t amount, uint32_t max) { CmdPullHealth(); g_cmd.AdvanceClampHealth(amount, max); CmdPushHealth(); }
void CmdZeroHealth() { CmdPullHealth(); g_cmd.health = 0; CmdPushHealth(); }

// "What the player just used", from the runtime's latches: a wall is a face
// and the mapping record it wears; a prop is (sector, index).
void CmdStageContext()
{
	g_cmd.activeFace = g.activeFace;
	g_cmd.activeTexmap = (g.activeFace >= 0 && (size_t)g.activeFace < g.map.faces.size())
		? g.map.faces[g.activeFace].textureMap : -1;
	g_cmd.sourceSector = g.activeObjectSector;
	g_cmd.sourceIndex = g.activeObjectIndex;
	CmdPullHealth();
}

void CmdAfterDispatch() { CmdPushHealth(); }

// The particle pool's actors, one per slot, replaced when the slot is reused.
struct CmdParticleActor { AActor *mo = nullptr; uint32_t serial = 0; uint16_t texture = 0; };
CmdParticleActor g_cmdParticleActors[CmdCore::PARTICLES];

void CmdClearParticleActors(bool destroy)
{
	for (CmdParticleActor &a : g_cmdParticleActors)
	{
		if (destroy && a.mo != nullptr) a.mo->Destroy();
		a = CmdParticleActor();
	}
}

// Show the pool. Positions are Realms coordinates moved into the engine's frame
// by the particle's sector's shift (roth_portals); the light follows the
// placed-object rule with the particle's own light byte (+0x10) and its
// sector's (roth_objects.cpp, the light block in PrepareObjects).
void CmdPresentParticles()
{
	if (g.level == nullptr) return;
	for (int i = 0; i < CmdCore::PARTICLES; i++)
	{
		const CmdParticle &p = g_cmd.particles[i];
		CmdParticleActor &a = g_cmdParticleActors[i];
		if (!p.busy || (a.mo != nullptr && (a.serial != p.serial || a.texture != p.texture)))
		{
			if (a.mo != nullptr) a.mo->Destroy();
			a = CmdParticleActor();
		}
		if (!p.busy) continue;
		double x = p.x, y = p.y, sx = 0., sy = 0.;
		if (p.sector >= 0 && SectorShift(p.sector, sx, sy)) { x += sx; y += sy; }
		if (a.mo == nullptr)
		{
			const uint8_t S = (p.sector >= 0 && (size_t)p.sector < g.map.sectors.size()) ? g.map.sectors[p.sector].light : 0;
			const int light = (p.light != 0 && S != 0) ? (int)(uint8_t)((uint8_t)(p.light - 0x80) + S) : (int)p.light;
			a.mo = SpawnLogicSpriteActor(g.level, p.texture, x, y, p.z, light);
			a.serial = p.serial;
			a.texture = p.texture;
			if (a.mo == nullptr) { g.unhandledOps[0x2D]++; a.serial = p.serial; continue; }
		}
		else MoveLogicSpriteActor(a.mo, p.texture, x, y, p.z);
	}
}

// The per-frame half, in tick_world_effects' order (raw_commands.c:3288-3299):
// the speed request is latched and cleared, then the particles move.
void CmdTick()
{
	g_cmd.LatchSpeedReduction();
	// The shift applies to the input velocity (renderer.c:1080-1081); the
	// engine's equivalent is the speed MovePlayer multiplies the input by.
	if (AActor *mo = CmdPlayer())
	{
		const double base = mo->GetDefault()->Speed;
		mo->Speed = g_cmd.speedShift != 0 ? base / (double)(1u << (g_cmd.speedShift & 0x1f)) : base;
	}
	g_cmd.TickParticles();
	g_cmd.DecayDamageFlash();
	CmdPresentParticles();
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
	case 0x03: return ModifySector(rec, index);
	case 0x10: return ActivateSfxNode(rec);
	case 0x09:
		if (g.platformsOn) return MoveSector(rec, index);
		g.unhandledOps[op]++;                          // roth_platforms 0: as before
		return CMD_NOTHING;

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
	case 0x27: return IfNotItem(rec, h);
	case 0x29: return GiveItemCmd(rec, h);
	case 0x2A: return RemoveItemCmd(rec);
	case 0x42: return SetInventoryFilterCmd(rec);
	// The actor commands (roth_actors.cpp). Not marked in IsImplemented: the
	// lead flips that after seeing them work.
	case 0x16: case 0x3A: case 0x3C:
		if (ActorsActive())
		{
			bool interrupt = false;
			const int r = ActorsCommand(op, *rec, g.activeObjectSector, g.activeObjectIndex, g.activeFace, interrupt);
			if (interrupt) h.interrupt = Interrupt::Rerun;   // g_command_chain_interrupt = 2
			return r;
		}
		return RunCommand(m, cc, index);
	case 0x34:
	{
		// cmd_change_face_texture (raw_commands.c:396-437): with a key of 0, a
		// wall that was there but already wore the texture answers "the next
		// step failed" (interrupt 2), which re-runs from 0x38's latch.
		const int r = ChangeFaceTexture(*rec);
		if (r == CMD_NOTHING && rec->key == 0 && g.activeFace >= 0) h.interrupt = Interrupt::Rerun;
		return r;
	}
	default:
	{
		// The command core (roth_cmdcore.cpp): 0x14 0x15 0x1c 0x1e 0x1f 0x20
		// 0x21 0x22 0x2d 0x2e 0x33 0x35 0x3f 0x41. Its interrupt IS the chain's.
		int r = CMD_NOTHING;
		CmdStageContext();
		g_cmd.interrupt = (uint8_t)h.interrupt;
		if (g_cmd.Dispatch(op, *rec, r))
		{
			h.interrupt = (Interrupt)g_cmd.interrupt;
			SyncDisabled(rec);
			CmdAfterDispatch();
			return r;
		}
		return RunCommand(m, cc, index);
	}
	}
}

//==========================================================================
//
// THE CHAIN'S SOUND -- g@0x8a264 and g@0x8a260.
//
// A trigger record's +0x0a is a sound, stored one higher so 0 is silence.
// Whoever fires the trigger latches it (fire_trigger_on_contact,
// raw_commands.c:1670; fire_sector_trigger, :2882; run_command_chain, :3936)
// and the END of the chain plays it, once, if the chain ACTED and no item was
// auto-selected (:1288-1300). Beside it sits a position, or the sentinel
// 0x80008000 for "none": with the sentinel the sound is play_sound_effect,
// everywhere at once; with a position it is play_command_sound at that point.
//
// Who sets what, from the callers:
//   a wall or floor used               sound, no position
//       (fire_object_use_trigger -> fire_command_contact_trigger, :2797, 1705)
//   an object used                     sound, no position (run_command_chain, :3929)
//   anything examined                  NO sound (fire_trigger_on_interact, :1683)
//       An examine record's +0x0a is not a sound at all: it is the DBASE100
//       record examine_world_object evaluates (input.c:953-969). MEASURED
//       with tools/roth_sound over the 44 maps: on the use, enter, impact and
//       contact opcodes every non-zero +0x0a is a row the 320-row bank has
//       (633 records, none out of range); on the examine opcodes 0x30, 0x31
//       and 0x32, 249 of 575 are not.
//   an attack hits a wall              sound, at the wall's midpoint
//       (fire_wall_object_trigger, :1765-1776)
//   a sector entered                   sound; at the player when +0x07 has
//       bit 0x04, else no position (fire_sector_trigger, :2873-2876; the
//       sentinel is its callers', :3209, 3267)
//   the deferred queue, and a chain    NO sound (:1597, 5447-5448)
//   run from dialogue
//
// These used to be read as the warp's latches (the note that stood on
// HostResetCommandChainNoSource); ROTH.C's done-tail is their only reader and
// it plays them.
//
// NOT DONE: cmd_spawn_object_adv re-latches both mid-chain to its own +0x0e
// and the spawned object's position (:4137, 4202). Opcode 0x3c is not
// implemented in this runtime, so that sound has no caller yet.
//
//==========================================================================

struct ChainSound
{
	uint16_t sound = 0;       // plus one
	bool hasPos = false;
	int x = 0, y = 0;
};
ChainSound g_chainSound;

void StageChainSound(uint16_t soundPlusOne)
{
	g_chainSound.sound = soundPlusOne;
	g_chainSound.hasPos = false;
}

void StageChainSoundAt(uint16_t soundPlusOne, int x, int y)
{
	g_chainSound.sound = soundPlusOne;
	g_chainSound.hasPos = true;
	g_chainSound.x = x;
	g_chainSound.y = y;
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
	// The done-tail's sound (raw_commands.c:1288-1300): bit 0 of the chain's
	// result OR-ed with the interrupt byte, and no auto-selected item. It is
	// read before the dialogue tail below clears that flag, as in the original.
	{
		const bool bit0 = ((result & 1) != 0) || h.interrupt == Interrupt::Break;
		const bool autoSel = g_core.IsAttached() && g_core.Items().AutoSelected();
		if (bit0 && !autoSel && g_chainSound.sound != 0)
		{
			if (!g_chainSound.hasPos) SoundPlayEffect((uint32_t)g_chainSound.sound - 1, 0);
			else SoundPlayCommand((uint32_t)g_chainSound.sound - 1, g_chainSound.x, g_chainSound.y);
		}
	}
	// The tail of reset_command_chain_state (raw_commands.c:1313-1315).
	// [unverified]: the original runs this on the path that reaches the end of
	// that function; whether every early return skips it has not been read.
	if (g_core.IsAttached())
	{
		g_core.Interp().FinishDialogueRecordEval();
		g_dialogueLatch = false;
		g_core.Items().ClearAutoSelected();
	}
	g_ifItemListCount = 0;                       // raw_commands.c:1328
	if (result != CMD_NOTHING) g.fired++;
	return result != CMD_NOTHING;
}

//==========================================================================
//
// Dialogue calling back into the map.
//
// reset_command_chain_no_source (raw_commands.c:5445-5450): clear the pending
// rotation, re-arm the warp sentinel, run the chain at that command index.
// The two latches it resets (0x8a264, 0x8a260) are the chain's sound and the
// sound's position (raw_commands.c:1290-1299 is their reader).
//
// run_object_commands_by_id (raw_commands.c:1732-1756): walk the command table
// in order for the first record that is not disabled and whose key is the id,
// and run the chain its link word names, with the dialogue latch held across
// it. -1 when one was found, else 0.
//
// A dialogue record is itself run from a chain, so these NEST: Fire inside
// the interpreter inside Fire. The original nests the same way, and its inner
// chain's tail runs finish_dialogue_record_eval just as Fire's does. The two
// latches reset_command_chain_no_source clears are the chain's SOUND and its
// position, not the warp's: see "THE CHAIN'S SOUND" above Fire. A depth
// guard stops a record and a chain that call each other for ever; ROTH.C has
// none and would not return.
//
//==========================================================================

int g_hostChainDepth = 0;

int32_t HostResetCommandChainNoSource(uint16_t index)
{
	if (g_hostChainDepth >= 16) { g.unhandledOps[0x23]++; return 0; }
	g_hostChainDepth++;
	StageChainSound(0);                      // :5447-5448: no sound, no position
	Fire(index);
	g_hostChainDepth--;
	return -1;                               // reset_command_chain_state's own answer
}

int32_t HostRunObjectCommandsById(uint16_t id)
{
	for (size_t i = 0; i < g.map.commands.size(); i++)
	{
		Command *rec = Rec((uint16_t)(i + 1));
		if (rec == nullptr) continue;
		if ((rec->modifier & 0x08) != 0 || rec->key != id) continue;         // :1746-1747
		g_dialogueLatch = true;                                              // :1748
		const int32_t r = HostResetCommandChainNoSource(rec->linkIndex);     // :1749-1750
		g_dialogueLatch = false;                                             // :1751
		return r;
	}
	return 0;
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
		g.activeObjectSector = g.activeObjectIndex = -1;   // :1599-1601, the source is cleared
		StageChainSound(0);                   // raw_commands.c:1597
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
	// The centre is wanted as a place to PUT something, so it is an engine
	// position: follow the sector if the loader moved it (roth_portals).
	double sx = 0., sy = 0.;
	if (SectorShift(rothSector, sx, sy)) { outX += sx; outY += sy; }
	return true;
}

void RelocateToArrival(int rothSector, double aboveFloor = 0.)
{
	if (g.level == nullptr) return;
	double x = 0., y = 0.;
	if (!SectorCentre(rothSector, x, y)) return;
	if (rothSector >= (int)g.level->sectors.Size()) return;

	sector_t *dest = &g.level->sectors[rothSector];
	// The player keeps its height above the floor: new z = old z - (old floor
	// - new floor), ml_relocate_found (map_load.c:518-520).
	const double z = dest->floorplane.ZatPoint(x, y) + aboveFloor;

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
	if (g_warpCarryXY)
	{
		// Arrival key 0: process_map_warp_or_load runs no relocation
		// (map_load.c:661), so the player keeps x, y and height in REALMS
		// coordinates. The engine's own spot for it can differ per map where
		// rooms are stacked (roth_portals), hence the conversion both ways.
		g_warpCarryXY = false;
		double ex = 0., ey = 0.;
		if (g.level != nullptr && RealmsToEngineXY(g_warpCarryX, g_warpCarryY, true, g_warpCarryZ, ex, ey) >= 0)
		{
			for (int i = 0; i < MAXPLAYERS; i++)
			{
				if (!g.level->PlayerInGame(i)) continue;
				AActor *mo = g.level->Players[i]->mo;
				if (mo == nullptr) continue;
				P_TeleportMove(mo, DVector3(ex, ey, g_warpCarryZ), true);
				mo->Vel.Zero();
			}
		}
		else TheLog().Count("logic: 0x3b key-0 arrival, the carried position is in no sector of this map");
	}
	if (g.pendingArrivalSector < 0) return;
	const int sec = g.pendingArrivalSector;
	g.pendingArrivalSector = -1;
	RelocateToArrival(sec, g_warpAboveFloor);
	g_warpAboveFloor = 0.;
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

// The first player's pawn: what the original's single player is.
AActor *FlowPawn()
{
	if (g.level == nullptr) return nullptr;
	for (int i = 0; i < MAXPLAYERS; i++)
		if (g.level->PlayerInGame(i) && g.level->Players[i]->mo != nullptr) return g.level->Players[i]->mo;
	return nullptr;
}

void CaptureMapLogic(MapLogicState &out);

void ApplyPendingWarp()
{
	if (!g.warpPending) return;
	// A movie asked for before the warp plays first: in the original the
	// chain that latched the warp had already sat through it (the call blocks,
	// gdv_cutscene.c:2840) before the frame's tail consumes the latch.
	if (MovieBusy()) return;
	g.warpPending = false;
	if (g.level == nullptr) return;

	// What the latch asks for, decided exactly as process_map_warp_or_load
	// and the frame tail decide it (roth_gameflow.h, ClassifyWarp).
	WarpRequest req;
	for (size_t i = 0; i < 8 && i < g.warpMap.size(); i++) req.name[i] = g.warpMap[i];
	req.key = g.warpArrival;
	const WarpKind kind = ClassifyWarp(req, g.level->MapName.GetChars());

	AActor *pawn = FlowPawn();
	double aboveFloor = 0.;
	int ownSector = -1;
	if (pawn != nullptr && pawn->Sector != nullptr)
	{
		aboveFloor = pawn->Z() - pawn->Sector->floorplane.ZatPoint(pawn->Pos().XY());
		ownSector = pawn->Sector->Index();
	}

	if (kind == WarpKind::Nothing || kind == WarpKind::SameMapNoMove) return;   // map_load.c:614-622
	if (kind == WarpKind::RelocateInPlace)
	{
		// The search skips the sector the player stands in (map_load.c:549).
		Arrival a;
		if (FindArrivalSector(g.map, req.key, ownSector, a)) RelocateToArrival(a.sector, aboveFloor);
		else TheLog().Count("logic: 0x3b same-map warp, arrival point named no sector");
		return;
	}

	const std::string target = WarpTargetName(req);
	if (FindLevelInfo(target.c_str(), false) == nullptr)
	{
		// MAUSO1EA record 60 names "MAS5", which is no map: the original's
		// load fails and the game carries on where it is (map_load.c:664-669).
		Printf(TEXTCOLOR_ORANGE "Realms: map transition to \"%s\" -- no such level is known.\n", target.c_str());
		return;
	}

	// Leaving: this map's logic is kept, as write_raw_state_temp keeps it
	// (map_load.c:625). The world is kept by the engine's hub snapshot.
	{
		std::string cur = g.level->MapName.GetChars();
		for (char &c : cur) c = (char)toupper((unsigned char)c);
		CaptureMapLogic(g_mapStates[cur]);
	}

	// The arrival point is latched in the map being LEFT and applied in the one
	// being entered, so it has to outlive the level change.
	g_warpArrivalMapName = target;
	g_warpArrivalSector = (int)req.key;
	g_warpAboveFloor = aboveFloor;
	g_warpCarryXY = false;
	if (req.key == 0 && pawn != nullptr)
	{
		double dx = 0., dy = 0.;
		if (ownSector >= 0) SectorShift(ownSector, dx, dy);
		g_warpCarryXY = true;
		g_warpCarryX = pawn->X() - dx;
		g_warpCarryY = pawn->Y() - dy;
		g_warpCarryZ = pawn->Z();
	}
	// Facing is kept: nothing in the warp path touches the angle (all 86
	// arrivals measured in the original keep it, tools/gameflow_test).
	g.level->ChangeLevel(target.c_str(), 0, CHANGELEVEL_NOINTERMISSION | CHANGELEVEL_KEEPFACING);
}

} // namespace

//==========================================================================

//==========================================================================
//
// The load-time init pass for light switches -- tick_cmd_45 (raw_commands.c:
// 1875), reached for every record of type 0x02 from the per-type init table
// at 0x30998 (map_load.c:1086, init_loaded_object_table pass 2).
//
// This is how a map starts with its lights OFF. An armed switch (byte[rec+6]
// & 0x20) floods its connected group from word[rec+0x0a], applies its delta
// byte[rec+7] once, flips its own state, and when finalize-flagged writes the
// lights-out bit 0x02 into every sector it reached -- the bit the renderer
// keys the candle shading on. Measured 2026-10-01: STUDY1 runs three of these
// before the first frame (-15, -15, -10), darkening 139 sectors including
// the study itself.
//
// First visit only in the original (g@0x89f5c == 0); REMAROTH has no revisit
// state yet, so every load is a first visit.
//
//==========================================================================

// COUNTED, because "STUDY1 darkens 139 sectors" was measured against the
// ORIGINAL and had no counterpart here: this pass logged nothing at all, so
// whether our load reaches the same 139 was not a question the report could
// answer. A lighting difference that survives into the picture looks the same
// whether a sector was missed here or shaded wrongly later, and those want
// opposite fixes.
static void InitLightSwitchesAtLoad(Log *log)
{
	int records = 0, armed = 0, emptyGroup = 0, sectorsDarkened = 0, flagged = 0;
	std::vector<int> deltas;

	for (Command &c : g.map.commands)
	{
		if ((c.opcode & 0x7f) != 0x02) continue;
		records++;
		c.SetByte(0x0C, 0);
		if (!(c.fireFlags & 0x20)) continue;
		armed++;
		c.fireFlags = (uint8_t)(c.fireFlags - 0x20);
		c.SetByte(0x06, c.fireFlags);
		const std::vector<int> group = CollectGeometryGroup(c.aux, true);
		if (group.empty()) emptyGroup++;
		if (!group.empty())
		{
			sectorsDarkened += (int)group.size();
			deltas.push_back((int)(int8_t)c.subFlags);
			const int8_t delta = (int8_t)c.subFlags;
			ApplyLightDelta(group, delta);
			c.fireFlags ^= 0x08;
			c.SetByte(0x06, c.fireFlags);
			c.modifier ^= 0x02;
			if (c.fireFlags & 0x80)
			{
				ApplyFlagMask(group, (delta & 0x80) ? 0 : 2, (delta & 0x80) ? 2 : 0);
				flagged += (int)group.size();
			}
		}
		c.modifier &= 0xde;
		SyncDisabled(&c);
	}

	if (log != nullptr)
	{
		FString ds;
		for (size_t i = 0; i < deltas.size(); i++)
			ds.AppendFormat("%s%d", i ? ", " : "", deltas[i]);
		log->Line("  lights out at load %d type-0x02 record(s), %d armed, "
			"%d with an empty group", records, armed, emptyGroup);
		log->Line("                     %d sector slot(s) reached, %d given the "
			"lights-out bit; deltas %s", sectorsDarkened, flagged,
			ds.IsEmpty() ? "(none)" : ds.GetChars());
	}
}

// A non-zero `special` so the engine's dispatch gates offer us the line. The
// value is never executed -- ActivateLine claims the line before
// P_TestActivateLine reads it -- it only has to not be 0. Deliberately a high
// number no Doom special uses, so that if it ever DOES reach the engine the
// failure is a loud "unknown special" rather than a door quietly behaving like
// a lift.
static const int ROTH_LINE_SPECIAL = 9000;

// WHICH WAYS A PLAYER MAY TOUCH A LINE is now asked per line, in
// MarkTriggerLines, from the events its own triggers wait for. A single
// ROTH_LINE_ACTIVATION constant used to stand here and it was wrong twice over:
// first it included SPAC_Cross, SPAC_AnyCross and SPAC_Push, so every face
// trigger fired on brushing past its wall; then, narrowed to use-or-impact, it
// still woke an 0x1a trigger on a use and an 0x18 on a bullet, because one
// constant cannot express "this line wants a click and that one wants a hit".
// GAME_core.md §5.2 gives exactly one event per opcode, so the engine is asked
// for exactly those.

// A door leaf answers the use key and nothing else. Shooting a door or walking
// into it does not open it in the original.
static const int ROTH_LEAF_ACTIVATION = SPAC_Use | SPAC_UseThrough | SPAC_UseBack;

//==========================================================================
//
// MarkTriggerLines -- make the engine OFFER us the lines we care about.
//
// The Realms hook lives at the top of P_ActivateLine, which is the right place
// to HANDLE a trigger and the wrong place to receive one: the engine decides
// whether to call P_ActivateLine at the DISPATCH sites, and every one of them
// gates on Doom's own `line->special != 0` first -- P_UseTraverse for the use
// key (p_map.cpp:6554) and the cross-line test for walking over one (:2792)
// among twelve such calls in that file alone. A Realms line has no Doom
// special, so it was never offered and the hook never ran. Nothing reached the
// level logic from PLAY at all. Only the roth_door console command worked,
// because it calls Fire() directly and never goes near the engine -- which is
// exactly why "thirty of thirty open" and "I cannot open a door" were both true
// for a day.
//
// So give those lines a non-zero special and the activation bits. The special's
// VALUE is never acted on: ActivateLine returns true for any line it owns, so
// P_ActivateLine returns before P_TestActivateLine ever reads it. It has to be
// non-zero and nothing more.
//
// CALLED TWICE, AND IT HAS TO BE. The engine's own loader passes run after
// BeginLevel and some of them write `special`. PO_Init is the one that bites:
// it zeroes every line whose special is Polyobj_ExplicitLine or
// Polyobj_StartLine once the polyobject has been collected
// (polyobjects.cpp:431-433), which is EVERY DOOR LEAF LINE IN THE MAP. And the
// marking cannot simply be done earlier instead, because SpawnPolyobj requires
// that same special to find the lines at all (polyobjects.cpp:221) -- overwrite
// it before PO_Init and the leaf never spawns. So the leaves can only be marked
// afterwards, and this function is idempotent so the second pass is safe.
//
// Idempotent by construction: it only ever writes a special that is 0, and ORs
// activation bits that are already there on a second visit.
//
//==========================================================================

int MarkTriggerLines(FLevelLocals *level)
{
	if (!g.active || level == nullptr) return 0;

	int marked = 0;

	auto mark = [&](int sideIdx, int activation)
	{
		if (sideIdx < 0 || (size_t)sideIdx >= level->sides.Size()) return;
		line_t *ln = level->sides[sideIdx].linedef;
		if (ln == nullptr) return;
		if (ln->special == 0)
		{
			ln->special = ROTH_LINE_SPECIAL;
			marked++;
		}
		ln->activation |= activation;
	};

	// ASK FOR ONLY THE EVENTS THIS LINE'S OWN TRIGGERS WANT.
	//
	// A line carrying nothing but an 0x1a does not want to be offered a use, and
	// a line carrying nothing but an 0x18 does not want to be offered a bullet.
	// Asking for both on every line is how 0x1a came to fire when the player
	// pressed use: the engine offered the line, the hook claimed it, and the
	// event was not checked. ActivateLine checks it now, but there is no reason
	// to be woken for an event no trigger on this line is waiting for.
	for (const auto &kv : g.bySide)
	{
		int want = 0;
		for (const Binding &b : kv.second)
		{
			switch (b.event)
			{
			case TrigEvent::Use:
				want |= SPAC_Use | SPAC_UseThrough | SPAC_UseBack;
				break;
			case TrigEvent::Impact:
				want |= SPAC_Impact;
				break;
			case TrigEvent::Examine:
				// NOTHING. There is no examine input yet, and borrowing the use
				// bits for it would put examine and use back in one bucket --
				// the exact conflation 5.2 corrects. An 0x32 is bound and
				// unreachable, which the load report states per opcode rather
				// than leaving it to look like a working trigger.
				break;
			default:
				break;
			}
		}
		if (want != 0) mark(kv.first, want);
	}

	for (const auto &kv : g.doorLeafSide) mark(kv.first, ROTH_LEAF_ACTIVATION);

	return marked;
}

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
	// The command core works on this level's copy of the map. Its particle
	// actors died with the last level; the player's health carries over.
	g_cmd.Attach(&g.map, &g_cmdHost, &g.rng, &g.pendingRecord);
	g_cmd.BeginLevel();
	CmdClearParticleActors(false);
	g_cmdHealthSeen = -1;
	g.pendingRecord = 0;
	g.delays.clear();
	g.deferred.clear();
	g.warpPending = false;
	g.fired = 0;
	g.unhandledOps.clear();
	g_chainSound = ChainSound();

	// DBASE100 is game-global, not per map: ROTH.C loads it once at startup
	// (game_core.c:1060). Loaded on the first map, and its report written into
	// that map's log. A failure is retried, and so reported, on every map
	// until it loads. Nothing reads it yet -- the interpreter is the next step
	// (ROTH_GAME_PORT.md §4 step 1). NOT COMPILED in the lane that wrote it.
	if (!TheDbase100().IsLoaded())
		LoadDbase100FromInstall(TheInstall(), TheDbase100(), log);

	// Realms' sound for this level: the effect bank (opened and registered on
	// the first map), this map's sound nodes, and -- once, straight after
	// DBASE100 is loaded and BEFORE anything runs a record, as the original
	// orders it (game_core.c:414-428) -- the sound keys in DBASE100's item
	// records resolved to sample rows. The new-game action below starts the
	// first song, so this has to be up before it.
	SoundBeginLevel(map, level, log);
	ActorsBeginLevel(g.map, level, log);

	// The game core is attached once: its flags and items are the game's, not
	// the map's. There is no menu or save yet, so the first level this session
	// loads IS the new game: the original's new-game block runs (action 3 and
	// its tail, game_core.c:821-833) and the player starts with what Adam
	// starts with. When game flow exists this call moves there.
	// roth_newgame_on_start 0 leaves the inventory empty, for tests.
	if (TheDbase100().IsLoaded() && !g_core.IsAttached())
	{
		g_core.Attach(&TheDbase100(), &g_coreHost, InterfaceInventorySink());
		InterfaceAttach(g_core, TheDbase100(), TheInstall());
		g_flow.Attach(&g_core, &TheDbase100(), &g_flowHost);
		g_coreHost.flow = &g_flow;
		{
			const std::string res = TheInstall().DataFile("ROTH.RES");
			std::vector<uint8_t> bytes;
			if (FILE *f = res.empty() ? nullptr : fopen(res.c_str(), "rb"))
			{
				fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
				if (n > 0) { bytes.resize((size_t)n); if (fread(bytes.data(), 1, (size_t)n, f) != (size_t)n) bytes.clear(); }
				fclose(f);
			}
			const std::vector<MapListEntry> list = ParseMapList(bytes.data(), bytes.size());
			if (!list.empty()) g_flowDefaultMap = list[0].map;
		}
		// The boot (action 1: two movies and the title music, game_core.c:755-773)
		// and Play's new-game block (game_core.c:821-845). A savegame being
		// restored brings its own state, so neither runs under it.
		if (roth_newgame_on_start && !savegamerestore)
		{
			// The owner's call (2026-11-12): the boot's logo and INTRO are skipped
			// until a release build; CHPTR1 and every in-game cutscene still play.
			// The boot's state changes happen either way.
			MovieSetSuppressed(!roth_intro_movies);
			g_flow.Boot();
			MovieSetSuppressed(false);
			InterfaceSetMenus(&g_menus);
			// Launched with no map: the main menu, as game_play_loop shows it
			// after the boot (game_core.c:806-816). Launched with +map (the
			// bridge, the tests): straight into the new-game block, as before.
			if (g_flowMenuPending) { g_flowMenuPending = false; g_menus.OpenMain(); }
			else RunNewGameBlock();
		}
		InterfaceSetMenus(&g_menus);
		if (log != nullptr) log->Line("game core: attached (%d items, %d actions, flag bitmap %d bytes, DBASE400 text %s)",
			TheDbase100().ItemCount(), TheDbase100().ActionCount(),
			(int)g_core.Interp().State().g_dbase100_record_bitmap.size(),
			"see the [roth_ui] line");
	}
	g_dialogueLatch = false;
	// A death went back to the menu, and Play was chosen (game_core.c:1006-1012,
	// 821-845). There is no main menu yet, so Play is what happens.
	if (g_core.IsAttached() && g_flowNewGamePending && !savegamerestore)
	{
		g_flowNewGamePending = false;
		RunNewGameBlock();
	}
	// A death that ran out, or was ended: the main menu (game_core.c:1006-1012).
	if (g_core.IsAttached() && g_flowMenuPending && !savegamerestore)
	{
		g_flowMenuPending = false;
		g_menus.OpenMain();
	}
	g_flowRestarting = false;

	// The arrival point the map we came FROM asked for.
	g.pendingArrivalSector = -1;
	if (g_warpArrivalSector >= 0 && level != nullptr
		&& stricmp(g_warpArrivalMapName.c_str(), level->MapName.GetChars()) == 0)
	{
		// Key 0 relocates nothing (map_load.c:661); ConsumeArrival carries the
		// position over instead.
		Arrival a;
		if (g_warpArrivalSector != 0 && FindArrivalSector(g.map, (uint16_t)g_warpArrivalSector, -1, a))
			g.pendingArrivalSector = a.sector;
		if (g_warpArrivalSector != 0 && g.pendingArrivalSector < 0 && log)
			log->Count("logic: 0x3b arrival point named no sector in the destination map");
	}
	else g_warpCarryXY = false;     // not the map the warp asked for: nothing carries
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

	// Bind every trigger to the geometry it watches, WITH the one event that
	// fires it. The reader has already resolved each key to a sector and/or a
	// set of faces; this sorts them by key space and tags each with its event.
	//
	// A trigger whose opcode has no event is counted rather than dropped
	// silently. That is how 0x13 went unnoticed: it matched neither classifier,
	// so every enter-sector trigger in the game fell through this loop without
	// appearing anywhere in the report.
	int faceBound = 0, sectorBound = 0, objectBound = 0, unbound = 0, unclassified = 0;
	std::map<uint8_t, int> boundByOp;
	// opcode -> (fireFlags << 8 | subFlags) -> how many RECORDS carry that pair
	std::map<uint8_t, std::map<int, int>> flagHist;
	for (const Command &c : g.map.commands)
	{
		if (!c.isTrigger || c.disabled) continue;
		if (c.chainStart == 0) continue;

		const TrigEvent ev = EventForOpcode(c.opcode);
		if (ev == TrigEvent::None)
		{
			// A trigger opcode this port has no event for. 0x1b, 0x25, 0x39,
			// 0x37 and 0x3d are the attack-hits-object, texture-animation,
			// walk-into-object and timer channels (GAME_core.md 5.2 rows 8, 9,
			// 14, 15), none of which is keyed to a face, a sector or an object,
			// so they belong to later work and not to this loop.
			//
			// 0x08, 0x02 and 0x30 USED TO LAND HERE and no longer do: they are
			// the object channel, which is now a third key space below.
			unclassified++;
			continue;
		}

		const Binding b{ c.chainStart, ev, c.opcode, c.fireFlags, c.subFlags, c.aux,
			(uint16_t)(&c - g.map.commands.data() + 1) };

		if (IsFaceTrigger(c.opcode))
		{
			if (c.faces.empty()) { unbound++; continue; }
			for (int fi : c.faces) g.byFace[fi].push_back(b);
			faceBound++;
			boundByOp[c.opcode]++;
			flagHist[c.opcode][(c.fireFlags << 8) | c.subFlags]++;
		}
		else if (IsSectorTrigger(c.opcode))
		{
			if (c.sector < 0) { unbound++; continue; }
			g.bySector[c.sector].push_back(b);
			sectorBound++;
			boundByOp[c.opcode]++;
			flagHist[c.opcode][(c.fireFlags << 8) | c.subFlags]++;
		}
		else if (IsObjectTrigger(c.opcode))
		{
			// BY THE RECORD'S OWN KEY, not by a resolved index. The original
			// SEARCHES for the object whose id matches, every time it fires
			// (run_leftclick_object_trigger walks the category list), so the
			// binding is stored under the key and the match happens at
			// activation against the picked object's commandID.
			//
			// A key of 0 would mean "the object the player just used", which on
			// this channel cannot happen: the key IS how the record is found.
			if (c.key == 0) { unbound++; continue; }
			g.byObject[c.key].push_back(b);
			objectBound++;
			boundByOp[c.opcode]++;
			flagHist[c.opcode][(c.fireFlags << 8) | c.subFlags]++;
		}
	}

	InitLightSwitchesAtLoad(log);
	InitChangeHeightAtLoad(log);

	// roth_platforms. The loader has already described its platform pieces by
	// the time this runs (rothmap.cpp calls BeginLevel last), so which sectors
	// they are can be listed once, here.
	g.movers.clear();
	g.platformSectors.clear();
	g.moversUnshown = 0;
	g.platformsOn = PlatformsEnabled();
	if (g.platformsOn)
	{
		for (size_t i = 0; i < g.map.sectors.size(); i++)
			if (SectorIsPlatform((int)i)) g.platformSectors.push_back((int)i);
		InitMovingSectorsAtLoad(log);
		if (log != nullptr && g.moversUnshown > 0)
			log->Count("logic: 0x09 moving sectors running with no platform to show them",
				g.moversUnshown);
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

	// Make the engine offer us these lines at all. See MarkTriggerLines: this is
	// the FIRST of its two passes, and the second -- after PO_Init, which is
	// what reaches the door leaves -- runs from the map loader.
	const int linesMarked = MarkTriggerLines(level);

	if (log)
	{
		log->Section("Level logic");
		log->Line("  triggers bound   %d face-keyed, %d sector-keyed, %d object-keyed",
			faceBound, sectorBound, objectBound);
		log->Line("  doors reachable  %d   walls wired %d", (int)g.doorTag.size(), (int)g.bySide.size());
		log->Line("  trigger lines    %d marked so the engine offers them"
			" (each line asks only for its own events -- GAME_core.md 5.2)", linesMarked);
		log->Line("  door leaf lines  %d registered, markable only after PO_Init",
			(int)g.doorLeafSide.size());

		//------------------------------------------------------------------
		// WHICH TRIGGER WAITS FOR WHAT, per opcode, with the event named and
		// whether anything can currently deliver it.
		//
		// This table exists because the one number it replaces -- "31
		// face-keyed, 12 sector-keyed" -- could not show that every 0x13 in
		// the map was bound to nothing, nor that the 12 sector-keyed had no
		// dispatch. A trigger that is bound and unreachable looked exactly
		// like a trigger that works.
		//------------------------------------------------------------------
		log->Line("  triggers by opcode, with the event that fires them:");
		for (auto &kv : boundByOp)
		{
			const TrigEvent ev = EventForOpcode(kv.first);
			const char *reach;
			switch (ev)
			{
			case TrigEvent::Use:
				reach = IsFaceTrigger(kv.first)
					? "LIVE -- the use key, but fires from ANY approach"
					: "LIVE -- a use ray that lands on a flat";
				break;
			case TrigEvent::Examine:
				reach = "no dispatch: there is no examine input yet";
				break;
			case TrigEvent::Impact:
				reach = "LIVE -- a shot hitting the wall, never yet observed";
				break;
			case TrigEvent::SectorEnter:
				reach = "LIVE -- a per-tic sector check, with the link variant";
				break;
			default:
				reach = "no event";
				break;
			}
			log->Line("    0x%02x  %4d  %-18s  %s", (unsigned)kv.first, kv.second,
				TrigEventName(ev), reach);
		}
		//------------------------------------------------------------------
		// WHAT THE WALL-BAND GATE WILL REFUSE, counted before trusting it.
		//
		// HANDOFF_SESSION_2026-10-09.md section 6 asked for exactly these two
		// numbers BEFORE the gate was switched on, and the gate went on first
		// (roth_pick, 84d54c5c33) -- so they are reported here instead, to make
		// a surprise into a measurement at the next load.
		//
		// 1. HOW MANY FACE BINDINGS CARRY A NON-ZERO +0x06. Those are the only
		//    ones the band gate can refuse: a record with 0x00 answers on any
		//    band, so turning the gate on cannot change it. A high count means
		//    roth_pick alters a lot of behaviour at once.
		//
		// 2. HOW MANY SIT ON A TWO-SIDED LINE. This sizes the OTHER risk, the
		//    one that made TRACE_Continue mandatory: p_trace.cpp never reports
		//    a two-sided line the ray passes through, so every binding counted
		//    here is one a stop-at-first trace would silently lose. If a later
		//    change replaces the continue-callback with a plain trace, this
		//    number is what goes dark.
		//------------------------------------------------------------------
		{
			int banded = 0, bandedTwoSided = 0, openBand = 0, twoSidedTotal = 0;
			for (const auto &kv : g.bySide)
			{
				const int sideIdx = kv.first;
				bool twoSided = false;
				if (g.level != nullptr && sideIdx >= 0
					&& sideIdx < (int)g.level->sides.Size())
				{
					const line_t *ln = g.level->sides[sideIdx].linedef;
					twoSided = ln != nullptr && (ln->flags & ML_TWOSIDED) != 0;
				}
				if (twoSided) twoSidedTotal++;
				for (const Binding &b : kv.second)
				{
					if (b.fireFlags == 0) { openBand++; continue; }
					banded++;
					if (twoSided) bandedTwoSided++;
				}
			}
			log->Line("  wall-band gate   %d binding(s) carry a non-zero +0x06"
				" and CAN be refused once a hit position is supplied;"
				" %d answer on any band", banded, openBand);
			log->Line("                   of the %d, %d sit on a two-sided line",
				banded, bandedTwoSided);
			log->Line("  two-sided sides  %d of %d wired sides -- every binding on"
				" one is invisible to a stop-at-first trace, which is why the"
				" pick uses TRACE_Continue", twoSidedTotal, (int)g.bySide.size());
			log->Count("logic: face bindings the band gate can refuse", banded);
			log->Count("logic: of those, on two-sided lines", bandedTwoSided);
		}

		log->Count("logic: triggers whose key named no geometry", unbound);
		log->Count("logic: trigger records whose opcode has no event here",
			unclassified);
		log->Line("  the approach mask at record +0x06 is NOT tested, so a trigger");
		log->Line("  fires from any direction rather than the authored one (5.1)");

		//------------------------------------------------------------------
		// WHERE THE SECTOR-KEYED TRIGGERS ARE.
		//
		// In the LOAD REPORT and not behind a console command, because a
		// command cannot see this: +exec runs before the deferred `map`, so
		// anything typed from a capture script finds no level -- the same
		// ordering the capture queue exists to work around. The report is
		// written during the load, so it is the only place this can be stated
		// without someone already standing in the level.
		//
		// Printed so a test can be AIMED. Standing on an 0x13 used to mean
		// guessing a pose; these are the sectors that carry one.
		//------------------------------------------------------------------
		std::map<uint8_t, std::vector<int>> sectorsByOp;
		for (const auto &kv : g.bySector)
			for (const Binding &b : kv.second)
				sectorsByOp[b.opcode].push_back(kv.first);

		for (auto &kv : sectorsByOp)
		{
			FString list;
			for (size_t i = 0; i < kv.second.size(); i++)
				list.AppendFormat("%s%d", i ? " " : "", kv.second[i]);
			log->Line("  0x%02x sectors (%d): %s", (unsigned)kv.first,
				(int)kv.second.size(), list.GetChars());
		}

		// WHERE THE FACE-KEYED TRIGGERS ARE, by sidedef and by the line that
		// carries it. The sector listing above has had a counterpart missing:
		// an 0x1a cannot be tested without knowing which wall to shoot, and
		// there was no way to find one short of guessing a pose.
		std::map<uint8_t, std::vector<int>> sidesByOp;
		for (const auto &kv : g.bySide)
			for (const Binding &b : kv.second)
				sidesByOp[b.opcode].push_back(kv.first);

		for (auto &kv : sidesByOp)
		{
			FString list;
			for (size_t i = 0; i < kv.second.size(); i++)
			{
				const int sideIdx = kv.second[i];
				int lineIdx = -1;
				if (g.level != nullptr && sideIdx >= 0
					&& (size_t)sideIdx < g.level->sides.Size()
					&& g.level->sides[sideIdx].linedef != nullptr)
				{
					lineIdx = g.level->sides[sideIdx].linedef->Index();
				}
				list.AppendFormat("%s%d(line %d)", i ? " " : "", sideIdx, lineIdx);
			}
			log->Line("  0x%02x sides (%d): %s", (unsigned)kv.first,
				(int)kv.second.size(), list.GetChars());
		}

		// WHICH RECORD FLAGS ACTUALLY OCCUR, so a rule read out of ROTH.C can
		// be checked against the data instead of assumed to matter. +0x06 is
		// the facing mask on an 0x13 and the surface selector on an 0x19; +0x07
		// picks the 0x13 link variant. Several of the bits read so far turn out
		// to be absent from this map entirely, which is worth knowing before
		// building anything that depends on them.
		// PER RECORD, not per binding. Counting the bindings inflates every
		// face-keyed opcode, because one record binds to every face its key
		// resolves to -- 0x18's 26 records read as 68 that way, which looks
		// like a disagreement with the table above it.
		log->Line("  record flags present, by opcode (one count per RECORD):");
		for (auto &op : flagHist)
		{
			for (auto &fl : op.second)
			{
				const uint8_t f6 = (uint8_t)(fl.first >> 8);
				const uint8_t f7 = (uint8_t)(fl.first & 0xff);
				FString note;
				if (op.first == 0x13)
					note.Format("variant %s%s%s", VariantName(VariantOf(f7)),
						(f6 & 0x40) ? ", wants the LEAVE refire" : "",
						(f6 & 0x0f) ? ", facing-RESTRICTED" : "");
				else if (op.first == 0x19 || op.first == 0x31)
					note = (f6 & 1) ? "the platform TOP" : "the sector floor";
				log->Line("    0x%02x  +0x06 0x%02x  +0x07 0x%02x  x%-3d %s",
					(unsigned)op.first, (unsigned)f6, (unsigned)f7, fl.second,
					note.GetChars());
			}
		}

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
		// CORRECTED 2026-10-03. This block used to say the gap was "almost
		// entirely ACTIVE-EFFECT REGISTRARS ... they need the effect pool and
		// its per-frame tick". That stopped being true when the pool was built,
		// and the line stayed -- it sent a reader at work already finished.
		log->Line("  THE EFFECT POOL EXISTS: 14 of its 15 ticks are done");
		log->Line("  (ROTH_COMMANDS.md, 'The active-effect pool, and the 15 ticks').");
		log->Line("  Only 0x09 is outstanding there, and it needs an ENGINE capability --");
		log->Line("  it slides a sector by moving its vertices, and GZDoom's BSP is");
		log->Line("  static. 32 records game-wide.");
		log->Line("  What is left above is GAME LOGIC, not effects: the conditional");
		log->Line("  cluster 0x27/0x29/0x2a, dialogue 0x2b and the item opcodes (the");
		log->Line("  DBASE100 reader exists; its interpreter does not), object spawn");
		log->Line("  0x16. (SFX nodes, 0x10, are done: roth_sound.)");
		log->Line("  None of it is approximated.");
	}
}

void EndLevel()
{
	InterfaceEndLevel();
	CmdClearParticleActors(false);      // the level's thinkers go with it
	g.active = false;
	g.level = nullptr;
	g.platformCtrl.clear();
	g.lightFollowers.clear();
	g.sectorShift.clear();
	g.movers.clear();
	g.platformSectors.clear();
	g.platformsOn = false;
	g.flatByIndex.clear();
	g.effects.clear();
	g.texByIndex.clear();
	g.mirroredByIndex.clear();
	g.wallByFace.clear();
	g.wallBakedKept = g.wallBakedDropped = 0;
	g.byFace.clear();
	g.bySide.clear();
	g.faceToSide.clear();
	g.sideToFace.clear();
	g.bySector.clear();
	g.byObject.clear();
	g.doorTag.clear();
	g.doorLeafSide.clear();
	g.doorLeafRoomFace.clear();
	g.doorSwing.clear();
	g.lastSectorByPlayer.clear();
	// NOT gPendingDoors. EndLevel runs DURING a map load, before BeginLevel
	// (rothmap.cpp:215, "drop the previous level's logic before anything
	// registers against this one"), so clearing the queue here would throw away
	// exactly the request roth_door exists to serve -- one typed from an +exec
	// script, which runs before the deferred map. A tag left over from a
	// previous level simply fails to match the new level's tags and falls out of
	// the queue on its first drain.
	g.map = Map();
	g.delays.clear();
	g.deferred.clear();
	g.pendingRecord = 0;
	g.activeFace = g.activeSector = -1;
	g.warpPending = false;
	g.fired = 0;
	g.unhandledOps.clear();
	ActorsEndLevel();
	SoundEndLevel();
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

// Every sidedef carrying a trigger of one opcode. The load report prints this,
// but a capture script cannot read the report: +exec runs before the deferred
// `map`, so a cfg that wanted to shoot an 0x1a would have to carry sidedef
// numbers copied by hand from a previous run's log. This lets the rig ask at
// drain time instead, when the level is up.
//==========================================================================
//
// ReportPolyobjects -- where each door leaf actually ENDED UP after PO_Init.
//
// Door leaves render as pure black holes: not a dark surface, but the clear
// colour, with the wall beside them drawing correctly. Art, colour key, sector
// light, R_RothShade and polyobject spawn are all eliminated by measurement,
// and the leaf is provably present -- the use ray hits it and swings it.
//
// That leaves placement. The hardware renderer SKIPS a subsector flagged
// SSECF_POLYORG outright -- "never render polyobject origin subsectors because
// their vertices no longer are where one may expect" (hw_bsp.cpp:1227) -- so a
// polyobject still sitting in its own origin subsector is invisible while
// remaining solid, which is exactly the symptom.
//
// Printed from the map loader, after PO_Init, because that is the only place
// the polyobjects exist AND a console command cannot reach: +exec runs before
// the deferred `map`.
//
//==========================================================================

void ReportPolyobjects(FLevelLocals *level)
{
	if (!g.active || level == nullptr) return;

	int inOrigin = 0, total = 0;
	FString firstFew;
	for (unsigned i = 0; i < level->Polyobjects.Size(); i++)
	{
		FPolyObj *po = &level->Polyobjects[i];
		subsector_t *sub = po->CenterSubsector;
		const bool org = (sub != nullptr) && (sub->flags & SSECF_POLYORG);
		total++;
		if (org) inOrigin++;
		if (i < 6)
		{
			firstFew.AppendFormat("%stag %d -> sub %d sector %d%s", i ? ", " : "",
				po->tag,
				sub != nullptr ? (int)sub->Index() : -1,
				(sub != nullptr && sub->sector != nullptr)
					? (int)sub->sector->Index() : -1,
				org ? " [POLYORG, NOT RENDERED]" : "");
		}
	}

	Printf("roth: %d polyobject(s), %d sitting in a POLYORG subsector"
		" (those are never rendered -- hw_bsp.cpp:1227)\n", total, inOrigin);
	if (!firstFew.IsEmpty()) Printf("roth: %s\n", firstFew.GetChars());
}

std::vector<int> SidesWithOpcode(uint8_t opcode)
{
	std::vector<int> out;
	if (!g.active) return out;
	for (const auto &kv : g.bySide)
		for (const Binding &b : kv.second)
			if (b.opcode == opcode) { out.push_back(kv.first); break; }
	return out;
}

void RegisterDoorLeafSide(int sideIndex, int polyTag, int roomFace)
{
	if (sideIndex < 0) return;
	g.doorLeafSide[sideIndex] = polyTag;
	if (roomFace >= 0) g.doorLeafRoomFace[sideIndex] = roomFace;
}

void RegisterDoorSwing(int polyTag, int marker, int sectorAcrossC1, int sectorAcrossC3,
	double v1x, double v1y, double v2x, double v2y)
{
	Runtime::DoorSwing d;
	d.marker = marker;
	d.secC1 = sectorAcrossC1;
	d.secC3 = sectorAcrossC3;
	d.v1 = DVector2(v1x, v1y);
	d.v2 = DVector2(v2x, v2y);
	g.doorSwing[polyTag] = d;
}

void RegisterFaceSide(int rothFace, int sideIndex)
{
	g.faceToSide[rothFace] = sideIndex;
}

void RegisterTexture(int rothIndex, FTextureID tex)
{
	if (rothIndex >= 0 && tex.isValid()) g.texByIndex[rothIndex] = tex;
}

void RegisterMirroredTexture(int rothIndex, FTextureID tex)
{
	if (rothIndex >= 0 && tex.isValid()) g.mirroredByIndex[rothIndex] = tex;
}

void RegisterWallFace(int rothFace, const WallFace &wf, int texelLength,
	int ownSector, int nbrSector, const WallPieceShape pieces[3])
{
	Runtime::WallReg r;
	r.wf = wf; r.texelLength = texelLength; r.own = ownSector; r.nbr = nbrSector;
	for (int k = 0; k < 3; k++) r.piece[k] = pieces[k];
	g.wallByFace[rothFace] = r;
}

void RegisterFlat(int rothIndex, FTextureID tex, bool isSky, bool opaque256)
{
	if (rothIndex >= 0) g.flatByIndex[rothIndex] = Runtime::FlatReg{ tex, isSky, opaque256 };
}

void RegisterSectorShift(int rothSector, double dx, double dy)
{
	if (rothSector >= 0 && (dx != 0. || dy != 0.))
		g.sectorShift[rothSector] = std::make_pair(dx, dy);
}

// A REALMS x,y, as a pose file or the original's own player position gives it,
// to the engine position of the same spot. Where rooms are stacked the spot
// names more than one sector, and they may have been set apart; z picks the
// storey (any height inside the room; rothplan::SectorAt's rule), and without
// one the sector that was NOT moved wins, which is where the camera stood
// before there were portals. Returns the Realms sector, or -1 if no sector
// holds the spot (the position is then returned unchanged).
int RealmsToEngineXY(double x, double y, bool haveZ, double z, double &ex, double &ey)
{
	ex = x; ey = y;
	int best = -1;
	if (haveZ)
	{
		best = rothplan::SectorAt(g.map, x, y, z);
	}
	else
	{
		const int nf = (int)g.map.faces.size(), nv = (int)g.map.vertices.size();
		for (int s = 0; s < (int)g.map.sectors.size(); s++)
		{
			const Sector &rs = g.map.sectors[s];
			if (rs.faceCount < 3 || rs.firstFaceIndex < 0 || rs.firstFaceIndex + rs.faceCount > nf) continue;
			bool in = false, bad = false;
			for (int k = 0; k < rs.faceCount; k++)
			{
				const Face &f = g.map.faces[rs.firstFaceIndex + k];
				if (f.vertex1 < 0 || f.vertex2 < 0 || f.vertex1 >= nv || f.vertex2 >= nv) { bad = true; break; }
				const double ax = g.map.vertices[f.vertex1].x, ay = g.map.vertices[f.vertex1].y;
				const double bx = g.map.vertices[f.vertex2].x, by = g.map.vertices[f.vertex2].y;
				if ((ay > y) != (by > y))
				{
					const double xi = ax + (y - ay) * (bx - ax) / (by - ay);
					if (xi > x) in = !in;
				}
			}
			if (bad || !in) continue;
			const bool moved = g.sectorShift.count(s) != 0;
			if (best < 0 || (!moved && g.sectorShift.count(best) != 0)) best = s;
		}
	}
	double sx = 0., sy = 0.;
	if (best >= 0 && SectorShift(best, sx, sy)) { ex += sx; ey += sy; }
	return best;
}

bool FireObjectChain(int rothSector, int rothIndex)
{
	if (!g.active || rothSector < 0 || (size_t)rothSector >= g.map.objects.size()) return false;
	if (rothIndex < 0 || (size_t)rothIndex >= g.map.objects[rothSector].size()) return false;
	const uint16_t key = g.map.objects[rothSector][rothIndex].commandID;
	g.activeObjectSector = rothSector;
	g.activeObjectIndex = rothIndex;
	g.activeFace = g.activeSector = -1;                   // raw_commands.c:1721-1722
	for (const Command &c : g.map.commands)               // find_object_list24: the first live one
	{
		if ((c.opcode & 0x7f) != 0x1B || c.disabled || c.key != key) continue;
		if (c.modifier & 0x29) return false;              // raw_commands.c:1704
		return Fire(c.chainStart);
	}
	return false;
}

bool RealmsDoorOpen(int rothSector)
{
	if (!g.active || g.level == nullptr) return false;
	auto it = g.doorTag.find(rothSector);
	if (it == g.doorTag.end()) return false;
	FPolyObj *po = g.level->GetPolyobj(it->second);
	if (po == nullptr) return false;
	return po->specialdata != nullptr || po->Angle != nullAngle;
}

void StageObjectChainSound(uint16_t soundPlusOne, int x, int y)
{
	StageChainSoundAt(soundPlusOne, x, y);
}

// See roth_runtime.h. One health: the flow holds it and the command core
// mirrors it (CmdPullHealth / CmdPushHealth), and the actor rules go through
// the same two steps, so a creature's hit reaches the engine's player exactly
// as a RAW 0x33 does.
int32_t PlayerHealth() { CmdPullHealth(); return g_cmd.health; }
void SetPlayerHealth(int32_t v) { g_cmd.health = v; CmdPushHealth(); }
int32_t PlayerMaxHealth() { return g_flow.MaxHealth(); }
int32_t PlayerPainFlash() { return g_cmd.damageFlash; }
void SetPlayerPainFlash(int32_t v) { g_cmd.damageFlash = v; }
int32_t PlayerArmour() { return g_core.IsAttached() ? g_core.Interp().State().g_value_reduction_factor : 0; }

bool SectorShift(int rothSector, double &dx, double &dy)
{
	dx = dy = 0.;
	auto it = g.sectorShift.find(rothSector);
	if (it == g.sectorShift.end()) return false;
	dx = it->second.first;
	dy = it->second.second;
	return true;
}

void RegisterLightFollower(int rothSector, int engineSector)
{
	if (rothSector >= 0 && engineSector >= 0) g.lightFollowers[rothSector].push_back(engineSector);
}

void RegisterPlatformControl(int rothSector, int ctrlSector)
{
	if (rothSector >= 0 && ctrlSector >= 0) g.platformCtrl[rothSector] = ctrlSector;
}

// Trace what the use key actually reaches. "Thirty of thirty open" from the
// console and "I cannot open a door" in play were both true for a whole day,
// because nothing printed which lines the use ray touched or whether any of
// them carried a Realms chain. roth_trigger_debug 1 prints one line per side
// looked at; it is off by default and costs nothing.
CVAR(Bool, roth_trigger_debug, false, 0)


bool ActivateLine(line_t *line, AActor *who, int side, int activationType,
	DVector3 *hitpos)
{
	if (!g.active || line == nullptr) return false;

	// WHICH EVENT THIS IS. The engine knows -- P_ActivateLine is handed the
	// activation type by each of its dispatch sites -- and this used to throw it
	// away, so one binding answered a use, a walk-over, a bullet and a shove
	// alike. GAME_core.md §5.2 gives one event per opcode, so most of those were
	// wrong by definition.
	const bool isUse = (activationType
		& (SPAC_Use | SPAC_UseThrough | SPAC_UseBack)) != 0;

	// The Realms event this engine event corresponds to. Nothing maps to
	// Examine: the original's examine channel is a right-click
	// (dispatch_entry_command_trigger_b) and there is no such input here yet, so
	// an 0x32 trigger is bound and waiting rather than quietly firing on a use.
	const TrigEvent want =
		isUse                              ? TrigEvent::Use
		: (activationType & SPAC_Impact)   ? TrigEvent::Impact
		                                   : TrigEvent::None;

	bool any = false, owned = false;

	// A DOOR LEAF, WHICH IS NOT A TRIGGER AT ALL.
	//
	// GAME_core.md §4.5: left-clicking a type-6 door goes straight to
	// toggle_door_open_state (E/input.c:1087-1098). No trigger, no chain, no
	// command record -- the panel itself is the thing you click.
	//
	// This port only ever implemented the OTHER route, a 0x18 wall-face trigger
	// whose chain happens to contain cmd_open_door, and the leaf was left as
	// bare geometry. Being one-sided and ML_BLOCKING (rothmap.cpp:1771), a leaf
	// with no special sends P_UseTraverse down its `blocked` path, where
	// P_LineOpening on a line with no backsector gives range 0 and the traverse
	// gives up with "can't use through a wall" (p_map.cpp:6608). So a closed
	// door ATE the use ray and every trigger line behind it was unreachable --
	// which is why the use key opened nothing while roth_door opened thirty of
	// thirty.
	auto leaf = g.doorLeafSide.end();
	for (int s = 0; s < 2 && leaf == g.doorLeafSide.end(); s++)
	{
		side_t *sd = line->sidedef[s];
		if (sd == nullptr) continue;
		leaf = g.doorLeafSide.find((int)(sd - &g.level->sides[0]));
	}
	// A CLOSED DOOR IS THE ROOM'S WALL FACE. While it is shut the original has
	// no door object at all (the pool record is made by spawn_door_instance and
	// removed again on the full close, doors.c:685, 1140-1167): what the player
	// sees and clicks is the room face in front of the door sector, and it is
	// THAT face's 0x18 trigger whose chain holds cmd_open_door, with the
	// record's own speed and dwell, and with "the wall the player just used" as
	// the face a key of 0 resolves to (raw_commands.c:4060-4070). So a use on a
	// broad leaf side is handed on to the room face behind it, and the normal
	// path below fires it. Only where that face carries no chain of its own
	// does the leaf fall back to swinging by itself.
	if (leaf != g.doorLeafSide.end() && isUse)
	{
		side_t *hitSd = line->sidedef[0];
		auto rf = hitSd != nullptr
			? g.doorLeafRoomFace.find((int)(hitSd - &g.level->sides[0]))
			: g.doorLeafRoomFace.end();
		if (rf != g.doorLeafRoomFace.end())
		{
			auto fs = g.faceToSide.find(rf->second);
			if (fs != g.faceToSide.end() && fs->second >= 0
				&& fs->second < (int)g.level->sides.Size() && g.bySide.count(fs->second))
			{
				side_t *roomSide = &g.level->sides[fs->second];
				if (roomSide->linedef != nullptr)
				{
					if (roth_trigger_debug)
						Printf("roth_trigger: line %d is door leaf (poly tag %d); passing the"
							" use to room face %d, side %d\n", line->Index(), leaf->second,
							rf->second, fs->second);
					line = roomSide->linedef;
					side = (line->sidedef[1] == roomSide) ? 1 : 0;
					leaf = g.doorLeafSide.end();
				}
			}
		}
	}
	if (leaf != g.doorLeafSide.end())
	{
		// Printed BEFORE the swing as well as after, because this call is newly
		// reachable from play and the first thing it has to prove is that it
		// returns at all. Until the leaves were marked, nothing from the use
		// path had ever entered SwingDoor -- roth_door reaches it from the
		// console, outside any traversal -- so a crash inside it would
		// otherwise look identical to never arriving here.
		if (roth_trigger_debug)
		{
			Printf("roth_trigger: line %d is door leaf, poly tag %d, event 0x%x,"
				" %s\n", line->Index(), leaf->second, (unsigned)activationType,
				isUse ? "entering SwingDoor" : "not a use, no swing");
		}

		// Report the SWING'S OWN RESULT, not just that one was attempted. A
		// door that is already open, or whose tag matches no polyobject, comes
		// back CMD_NOTHING, and "swinging" printed before the call cannot tell
		// those apart from a door that moved.
		const int fromSector = (who != nullptr && who->Sector != nullptr
			&& who->Sector->Index() < (int)g.map.sectors.size()) ? who->Sector->Index() : -1;
		const int swung = isUse ? SwingDoor(leaf->second, 0, 0, fromSector) : CMD_NOTHING;
		if (roth_trigger_debug && isUse)
		{
			Printf("roth_trigger: line %d swing returned -> %s\n", line->Index(),
				swung == CMD_ACTED ? "SWUNG"
				                   : "use accepted but swing refused");
		}
		// Owned either way: the leaf carries ROTH_LINE_SPECIAL, and handing it
		// back to Doom would have the engine try to execute that special.
		//
		// speedMul 0 is the original's own default -- SwingDoor reads M == 0 as
		// M == 1 (doors.c:1043-1045) -- and dwell 0 leaves it open rather than
		// self-closing, because a clicked door in the original waits for the
		// player. This OPENS; toggle_door_open_state also CLOSES on the second
		// click, which is the outstanding "closing, blocking, the second use"
		// item and needs the door's own state word, not another argument here.
		return true;
	}

	// OWN THE LINE IF EITHER SIDE CARRIES A CHAIN, so Doom never falls through
	// to executing ROTH_LINE_SPECIAL -- but FIRE ONLY THE SIDE THE ENGINE
	// NAMED.
	//
	// This used to fire both sidedefs' chains and discard the `side` argument
	// outright: the loop ended in `(void)side`. A Realms two-sided wall is TWO
	// faces, sisters, and the loader pairs each to one sidedef, so shooting one
	// face also fired the face on the other side of the wall -- and where both
	// sisters resolve from the SAME record, the chain ran TWICE for one shot.
	// The impact test made it plain: one hitscan at side 91 printed firing for
	// both side 91 (face 85) and side 92 (face 1366).
	//
	// The original has no such ambiguity. dispatch_entry_command_trigger
	// resolves ONE texmap from the pick (`word[p1+8]`, raw_commands.c:3019) and
	// matches against that face alone.
	for (int s = 0; s < 2; s++)
	{
		side_t *sd = line->sidedef[s];
		if (sd == nullptr) continue;
		if (g.bySide.count((int)(sd - &g.level->sides[0]))) owned = true;
	}

	side_t *hit = line->sidedef[(side == 1 && line->sidedef[1] != nullptr) ? 1 : 0];
	if (hit == nullptr) return owned;
	const int idx = (int)(hit - &g.level->sides[0]);

	// Stage "what the player just used" BEFORE firing, because a key of 0
	// resolves to it (ROTH_COMMANDS.md).
	auto f = g.sideToFace.find(idx);
	if (f != g.sideToFace.end()) g.activeFace = f->second;
	// fire_wall_object_trigger (raw_commands.c:1766-1768) clears the source
	// object: a key of 0 after a wall names the wall, never an earlier prop.
	g.activeObjectSector = g.activeObjectIndex = -1;

	auto it = g.bySide.find(idx);
	if (it == g.bySide.end())
	{
		if (roth_trigger_debug)
			Printf("roth_trigger: line side %d (face %d) event 0x%x -> NO chain bound\n",
				idx, f != g.sideToFace.end() ? f->second : -1,
				(unsigned)activationType);
		return owned;
	}

	// ONLY THE TRIGGERS WAITING FOR THIS EVENT.
	//
	// A wall can carry an 0x18 (use) and an 0x1a (a shot hits it) at once, and
	// they are different triggers with different chains. Before the event
	// reached here, both fired on whichever arrived first.
	// WHICH BAND OF THE WALL was touched. A face record's +0x06 names the parts
	// it answers on -- mid 0x01, lower 0x02, upper 0x04 -- so a switch on the
	// mid texture does not fire when the lower section is hit. See WallPartBit.
	//
	// Needs the hit height, which only some callers have: P_UseTraverse passes
	// one and the tracer's impact sites were given one, but a push or a cross
	// has none. Without it the part cannot be known, so the gate is skipped
	// rather than guessed -- an over-fire, reported, not a silent pass.
	const uint8_t part = (hitpos != nullptr) ? WallPartBit(line, hitpos->Z) : 0;

	for (const Binding &b : it->second)
	{
		if (b.event != want) continue;

		if (part != 0 && b.fireFlags != 0 && !(b.fireFlags & part))
		{
			if (roth_trigger_debug)
				Printf("roth_trigger: line side %d 0x%02x wants wall part(s)"
					" 0x%02x, hit the %s (0x%02x) -- not firing\n", idx,
					(unsigned)b.opcode, (unsigned)(b.fireFlags & 0x07),
					part == 0x02 ? "lower" : part == 0x04 ? "upper" : "mid",
					(unsigned)part);
			continue;
		}

		if (roth_trigger_debug)
			Printf("roth_trigger: line side %d (face %d) 0x%02x %s part 0x%02x"
				" of 0x%02x -> firing\n", idx,
				f != g.sideToFace.end() ? f->second : -1,
				(unsigned)b.opcode, TrigEventName(b.event), (unsigned)part,
				(unsigned)(b.fireFlags & 0x07));
		// The chain's sound. A use has no position; an examine has no sound;
		// an attack that hits the wall puts it at the wall's midpoint, by the
		// same halving the original uses (raw_commands.c:1773-1776).
		if (want == TrigEvent::Examine) StageChainSound(0);
		else if (want == TrigEvent::Impact)
		{
			const int ax = (int)line->v1->fX(), bx = (int)line->v2->fX();
			const int ay = (int)line->v1->fY(), by = (int)line->v2->fY();
			StageChainSoundAt(b.sound, ((ax - bx) >> 1) + bx, ((ay - by) >> 1) + by);
		}
		else StageChainSound(b.sound);
		any |= Fire(b.chain);
	}

	(void)who; (void)any;
	return owned;
}

//==========================================================================
//
// FireSectorTriggers -- the sector-keyed triggers on one sector, for one event.
//
// THE EVENT ARGUMENT IS THE WHOLE POINT. Three opcodes are keyed by a sector
// and they are fired by three different things (GAME_core.md 5.2 rows 4, 6,
// 12): 0x13 by entering or leaving it, 0x19 by a left-click on its floor, 0x31
// by a right-click on its floor, ceiling or platform. Being "sector-keyed" is
// about where the key is looked up and says nothing about the event, and
// collapsing the three into one call is what made "wire a caller for
// FireSectorTriggers" look like a sensible next step. It is not: a caller on
// sector entry would fire the two floor-click triggers on walking through a
// doorway.
//
// Still no caller for Use or Examine -- that needs a use ray that lands on a
// flat, which P_UseLines structurally cannot give (it tests lines). The
// SectorEnter caller is a sector-transition event.
//
//==========================================================================

//==========================================================================
//
// NotifyPlayerSector -- the dispatch for 0x13, the enter-sector trigger.
//
// 23 of STUDY1's 66 bound triggers are 0x13, the second-largest category in the
// map, and until this existed every one of them was bound to nothing: the
// opcode matched neither classifier, so it fell through the binding loop in
// silence. "12 sector-keyed" in the handoffs was really 35.
//
// A PER-TIC COMPARISON IS THE ORIGINAL'S OWN MECHANISM, not a stand-in for one.
// twe_link_state (raw_commands.c:3202-3270) runs from the per-frame world tick
// and looks at where the player is; there is no enter-sector callback to hook.
// So this observes the sector once a tic and fires when it changes.
//
// TWO DELIBERATE LIMITS, both because the data to do better has not been read
// yet rather than because this is the intended end state:
//
// THE FACING MASK IS IMPLEMENTED (see FacingAllows): a set bit in the record's
// +0x06 bits 0-3 forbids that 90-degree quadrant of player facing, and
// fire_sector_trigger tests it before anything else.
//
// ENTER ONLY, NOT LEAVE -- and ROTH.C says exactly what leave would take, so
// this is a decision rather than an unknown. twe_link_state's first branch
// (raw_commands.c:3205-3213) refires the LATCHED record when the player's
// sector changes away from it, and the latch is only set at all for a record
// with `f6 & 0x40` (fire_sector_trigger:2893). So **bit 0x40 of +0x06 IS the
// "fire on leave too" flag**, and implementing leave means keeping the latched
// (record, sector) pair and refiring it on the next change. Not built here
// because it has not been measured against the running original, and a chain
// that fires twice where it should fire once corrupts state in ways that are
// much harder to see than one that never fires.
//
// THE THREE VARIANTS ARE IMPLEMENTED, and they need no sector byte: the
// variant is selected by the RECORD, which this port already carries.
//
// mark_geometry_records_by_id (raw_commands.c:4994-5005) derives the sector's
// +0x17 marker from the record's own +0x07 -- our `Command::subFlags`:
//
//     mask = 0x40;
//     if (b7 & 3) { mask = 0xc0; if (b7 & 1) mask = 0x80; }
//
// and twe_link_state (:3220-3232) branches on that marker:
//
//   0x80  PLAIN    (subFlags bit 0 set)       no Z test; fires on entry.
//   0x40  WATER    (subFlags bits 0-1 clear)  inside the link only while
//                  `player_z <= sector[+2]`, the sector's floorHeight.
//                  NOT "only while submerged", which this file claimed before
//                  the convention was checked: g_player_z is the player's FEET,
//                  not the eye -- collision_physics.c:65 builds the player's
//                  extent as [qZ, qZ + height + 0xa] -- so a player standing
//                  normally on the floor has feet AT floorHeight and is INSIDE
//                  the link. The test only excludes being ABOVE the floor:
//                  mid-jump, or stood on a platform within the sector. The
//                  "water trigger fires on walking in instead of on going
//                  under" claim was an overstatement from reading the branch
//                  without checking which end of the player Z measures.
//   0xc0  LINKED   (subFlags bit 1, bit 0 clear)  reads the linked record at
//                  the sector's +0x18 -- the MID-PLATFORM -- and requires
//                  `player_z <= topZ && player_z + 2 >= topZ`, i.e. standing ON
//                  the platform top. `cell+8` is the platform's topZ.
//
// Z needs no conversion: the loader takes `floorZ = double(rs.floorHeight)`
// (rothmap.cpp:465), so engine Z and Realms Z are the same number.
//
// KNOWN DIFFERENCE, deliberate. The original ORs the mask per SECTOR, so two
// 0x13 records of different variants on one sector combine -- 0x40|0x80 becomes
// 0xc0, "linked". STUDY1's sector 430 does carry two. This evaluates the
// variant PER RECORD instead, which is what each record asks for; the
// combination is an artefact of storing the variant in shared geometry. Noted
// rather than reproduced, because reproducing it needs a reason beyond "the
// original does it", and nothing has been measured there.
//
// ALSO A DIFFERENCE: the original fires exactly ONE record per entry. It finds
// it with `find_object_record_by_id(word[sector+0x14])` (:3264-3267), which
// returns the first match for the sector's command id. This fires every 0x13
// bound to the sector, so sector 430's two both run where the original runs
// one. Unmeasured; left as it is rather than guessing which one "first" is.
//
// Also unimplemented, and all three are gates the original applies FIRST
// (fire_sector_trigger:2869-2871): the modifier byte +0x02 blocks on mask 0x29
// (armed 0x01, spent 0x08, registered 0x20) where this port only tests the
// spent bit at bind time; and `+0x07 & 4` requires the player to be MOVING,
// checked against g_move_speed_accum, which has no equivalent here.
//
// NOTHING FIRES ON THE FIRST SIGHTING. The player spawns already standing in a
// sector, and whether the original fires that sector's trigger at spawn has not
// been measured. Firing it would run the starting room's script on load, which
// is a loud thing to invent; so the first observation is recorded and nothing
// else. If a map turns out to depend on it, that is a measurement to make
// against the running original, not a default to flip here.
//
//==========================================================================

void NotifyPlayerSector(int playerNum, AActor *mo)
{
	if (!g.active || mo == nullptr || mo->Sector == nullptr) return;

	const int now = mo->Sector->sectornum;
	auto it = g.lastSectorByPlayer.find(playerNum);
	if (it == g.lastSectorByPlayer.end())
	{
		// First sighting: remember where the player is, fire nothing.
		g.lastSectorByPlayer[playerNum] = now;
		return;
	}
	if (it->second == now) return;

	it->second = now;
	if (roth_trigger_debug)
		Printf("roth_trigger: player %d entered sector %d\n", playerNum, now);
	FireSectorTriggers(mo->Sector, mo, TrigEvent::SectorEnter);
}

//==========================================================================
//
// UseFlat -- the dispatch for 0x19 and 0x31, the FLOOR-CLICK triggers.
//
// GAME_core.md 5.2 rows 6 and 12: 0x19 is a left-click on a floor or
// platform top, 0x31 a right-click on a floor, ceiling or platform. STUDY1
// binds 12 of the former -- six of them on sector 221 alone -- and none of the
// latter.
//
// WHY P_UseLines CANNOT DO THIS. Its whole path is two-dimensional: start and
// end are DVector2 and P_UseTraverse walks lines out of the blockmap. There is
// no floor in it to hit, and no pitch either, so "the player is looking down at
// the floor" is not a question it can be asked. That is the structural reason
// these 12 triggers had no caller and why wiring FireSectorTriggers to sector
// entry looked like the only option -- it was the only option THROUGH THAT
// PATH. A ray that can hit a flat is a different instrument.
//
// THIS RUNS BEFORE THE LINE PATH, and it was a fallback after it until that
// proved wrong. P_UseTraverse's blocked path ends in `return true` -- "can't
// use through a wall", with the *usefail sound -- so an OBSTRUCTED ray reports
// the use as consumed. Guarding this on `!used` skipped it in exactly the case
// it was needed: standing in sector 409 looking down at its floor, the 2D ray
// met a wall, the use was eaten, and the floor trigger under the crosshair was
// never offered. Nothing in the log said so either, because the guard was
// outside this function.
//
// Running first is also closer to the original, not further from it:
// activate_targeted_object picks whatever is under the cursor and classifies
// it, so a floor there wins outright (GAME_core.md 4.5). It does not steal a
// use from a door, because at a shallow pitch the trace reaches the door's wall
// long before the floor, and this claims the use only when the hit really is a
// flat carrying a trigger that wants it.
//
// Still not the original's shape: that is ONE pick classifying face / floor /
// object / door together, which wants the object channels too.
//
//==========================================================================

bool UseFlat(AActor *who, TrigEvent want)
{
	if (!g.active || who == nullptr || who->Sector == nullptr) return false;

	// From the eye, along the view direction, as far as the use key reaches.
	const double range = who->FloatVar(NAME_UseRange);
	const DVector3 start = who->PosPlusZ(who->Height * 0.5);
	const DAngle pitch = who->Angles.Pitch;
	const DVector3 dir(
		who->Angles.Yaw.Cos() * pitch.Cos(),
		who->Angles.Yaw.Sin() * pitch.Cos(),
		-pitch.Sin());

	FTraceResults res;
	if (!Trace(start, who->Sector, dir, range, ActorFlags::FromInt(0), 0,
		who, res, TRACE_NoSky))
	{
		// SAY SO. A silent false here is indistinguishable from "the sector had
		// no 0x19 bound", and that cost a wrong reading already: sector 409
		// carries both an 0x13 and an 0x19, the 0x13 fired, the 0x19 did not,
		// and with nothing printed there was no way to tell whether the trace
		// failed or the binding was missing.
		if (roth_trigger_debug)
			Printf("roth_trigger: flat probe -- Trace() found nothing within"
				" %.0f units\n", range);
		return false;
	}
	// REPORT THE WHOLE RESULT, not just the rejection. Three of this function's
	// four exits used to be silent or nearly so, and they are not
	// distinguishable from outside: sector 409 carries both an 0x13 and an
	// 0x19, the 0x13 fired and the 0x19 did not, and the log said nothing about
	// which exit was taken.
	if (roth_trigger_debug)
	{
		Printf("roth_trigger: flat probe -- hit type %d, sector %d, z %.1f,"
			" from z %.1f pitch %.1f\n", (int)res.HitType,
			res.Sector != nullptr ? res.Sector->sectornum : -1,
			res.HitPos.Z, start.Z, pitch.Degrees());
	}

	// EVERYTHING PAST THE TRACE IS NOW UseFlatAt, so a pick made from a VR hand
	// applies the same flat rules to its OWN ray instead of tracing a second
	// time from the eye. Two rays answering one question is how the eye and the
	// hand came to disagree about what the player was pointing at. The half
	// below is what this function used to do from here on, unchanged.
	return UseFlatAt(who, res, want);
}

bool UseFlatAt(AActor *who, const FTraceResults &res, TrigEvent want)
{
	if (!g.active || who == nullptr) return false;

	if (res.HitType != TRACE_HitFloor && res.HitType != TRACE_HitCeiling)
	{
		if (roth_trigger_debug)
			Printf("roth_trigger: not a flat (TRACE_HitNone 0, HitFloor %d,"
				" HitCeiling %d)\n", (int)TRACE_HitFloor, (int)TRACE_HitCeiling);
		return false;
	}
	if (res.Sector == nullptr)
	{
		if (roth_trigger_debug)
			Printf("roth_trigger: flat probe hit a flat with NO sector\n");
		return false;
	}

	// Stage "what the player just used" as the SECTOR, because a key of 0 in the
	// chain resolves to it the same way a face does on a wall.
	g.activeSector = res.Sector->sectornum;

	auto it = g.bySector.find(res.Sector->sectornum);
	if (it == g.bySector.end())
	{
		if (roth_trigger_debug)
			Printf("roth_trigger: flat in sector %d -- NO chain bound\n",
				res.Sector->sectornum);
		return false;
	}

	// WHICH SURFACE THE RECORD WANTS. Not a direction mask -- this channel has
	// none, and applying the sector-enter one here was wrong.
	//
	// dispatch_entry_command_trigger's type-2/8 branch (E/raw_commands.c:3068):
	//
	//     fire = (record[+6] & 1) ? (type == 8) : (type == 2)
	//
	// type 2 is the sector floor, type 8 the mid-platform top. So bit 0 of
	// +0x06 SELECTS BETWEEN THEM -- GAME_core.md 4.5's "record flag bit0
	// selects the platform-top instead of the sector floor" -- and the branch
	// carries no dir_mask at all, unlike the type-3 face branch beside it.
	//
	// THIS CODE USED TO CALL FacingAllows HERE, which reads bits 0-3 as
	// facing-BLOCK bits. On a floor record bit 0 means "platform top", so a
	// platform record was being suppressed or not depending on which way the
	// player happened to face. It was inert for every 0x19 in STUDY1 because
	// they all carry mask 0x00, and the one place it was not inert is the one
	// place a trigger went missing: the silent `continue` it caused is the
	// unprinted exit that made sector 409's 0x19 look like it had no binding.
	//
	// A mid-platform here is a 3D floor, so the trace saying it hit one (ffloor
	// non-null) is the type-8 case.
	const bool hitPlatformTop = (res.ffloor != nullptr);

	bool any = false;
	for (const Binding &b : it->second)
	{
		if (b.event != want) continue;

		const bool wantsPlatformTop = (b.fireFlags & 1) != 0;
		if (wantsPlatformTop != hitPlatformTop)
		{
			if (roth_trigger_debug)
				Printf("roth_trigger: sector %d 0x%02x wants the %s, ray hit the"
					" %s -- not firing\n", res.Sector->sectornum,
					(unsigned)b.opcode,
					wantsPlatformTop ? "platform top" : "sector floor",
					hitPlatformTop ? "platform top" : "sector floor");
			continue;
		}

		if (roth_trigger_debug)
			Printf("roth_trigger: %s sector %d 0x%02x %s flags 0x%02x -> firing\n",
				res.HitType == TRACE_HitFloor
					? (hitPlatformTop ? "platform top" : "floor") : "ceiling",
				res.Sector->sectornum, (unsigned)b.opcode, TrigEventName(b.event),
				(unsigned)b.fireFlags);
		// A floor USED: the sound, no position. EXAMINED: no sound.
		StageChainSound(want == TrigEvent::Use ? b.sound : (uint16_t)0);
		any |= Fire(b.chain);
	}
	return any;
}

//==========================================================================
//
// POINTING AT THINGS -- ONE RAY, ASKED ONCE
//
// The header says why this exists. The two things worth repeating at the code:
//
// 1. TRACE_Continue IS NOT OPTIONAL. The loader gives a Realms face a sister
//    with ML_TWOSIDED and no ML_BLOCKING, and p_trace.cpp never REPORTS a
//    two-sided line the ray passes through -- it is crossed, not hit. A
//    stop-at-first trace therefore cannot see a mid-band wall trigger on any
//    interior wall, at any mask value. Continuing past each hit gets the same
//    offer P_UseTraverse gave, which is the only reason those triggers ever
//    fired.
//
// 2. HITS ARRIVE NEAREST FIRST, so the first one that means something in
//    Realms terms IS the answer and the scan stops there. Anything the port
//    cannot act on is stepped over rather than ending the search, which is the
//    whole difference between this and the trace UseFlat used to run.
//
//==========================================================================

// mid 0x01, lower 0x02, upper 0x04 -- the same three bits a face record's
// +0x06 names, taken from the tracer's own classification instead of being
// re-derived from a hit height. TIER_FFloor is a 3D floor's side, which is a
// mid-platform here and answers as the mid band.
static uint8_t BandFromTier(uint8_t tier)
{
	switch (tier)
	{
	case TIER_Lower: return 0x02;
	case TIER_Upper: return 0x04;
	default:         return 0x01;   // TIER_Middle, TIER_FFloor
	}
}

struct PickScan
{
	Pick best;
	bool found = false;
};

static ETraceStatus PickScanCallback(FTraceResults &res, void *data)
{
	PickScan &s = *(PickScan *)data;

	switch (res.HitType)
	{
	case TRACE_HitWall:
	{
		if (res.Line == nullptr || g.level == nullptr) break;

		// Which sidedef the ray actually met, in the engine's own numbering.
		side_t *sd = res.Line->sidedef[res.Side];
		if (sd == nullptr) break;
		const int idx = (int)(sd - &g.level->sides[0]);

		// A DOOR LEAF FIRST. It is one-sided blocking geometry with no line
		// special, so it would otherwise read as scenery and swallow the ray
		// -- which is exactly how it used to shadow every trigger behind it.
		auto leaf = g.doorLeafSide.find(idx);
		if (leaf != g.doorLeafSide.end())
		{
			s.best = Pick{};
			s.best.kind    = Pick::DoorLeaf;
			s.best.line    = res.Line;
			s.best.side    = res.Side;
			s.best.leafTag = leaf->second;
			s.best.hitPos  = res.HitPos;
			s.best.band    = BandFromTier(res.Tier);
			s.best.dist    = res.Distance;
			s.found = true;
			return TRACE_Stop;
		}

		if (g.bySide.count(idx))
		{
			s.best = Pick{};
			s.best.kind   = Pick::WallFace;
			s.best.line   = res.Line;
			s.best.side   = res.Side;
			s.best.hitPos = res.HitPos;
			s.best.band   = BandFromTier(res.Tier);
			s.best.dist   = res.Distance;
			s.found = true;
			return TRACE_Stop;
		}
		break;   // a wall with nothing bound to it: step over, keep looking
	}

	case TRACE_HitFloor:
		if (res.Sector == nullptr) break;
		s.best = Pick{};
		s.best.kind   = Pick::Flat;
		s.best.sector = res.Sector;
		s.best.hitPos = res.HitPos;
		s.best.dist   = res.Distance;
		s.found = true;
		return TRACE_Stop;

	// CEILINGS ARE GATED OUT, deliberately. 0x31 covers ceilings as well as
	// floors, but nothing in the goal needs a ceiling press and a hand pointed
	// up at one is far more likely to be an accident than an instruction.
	case TRACE_HitCeiling:
		break;

	// A PROP, if it is a Realms object with something bound to it. A Doom actor,
	// or a Realms prop that is pure scenery, is stepped over rather than ending
	// the search -- so a chair in front of a wall switch does not eat the press.
	case TRACE_HitActor:
	{
		if (res.Actor == nullptr) break;
		int rs = -1, ri = -1;
		if (!ObjectForActor(res.Actor, rs, ri)) break;
		s.best = Pick{};
		s.best.kind   = Pick::Prop;
		s.best.actor  = res.Actor;
		s.best.hitPos = res.HitPos;
		s.best.dist   = res.Distance;
		s.found = true;
		return TRACE_Stop;
	}

	default:
		break;
	}

	return TRACE_Continue;
}

bool PickFrom(AActor *who, const DVector3 &start, const DVector3 &dir,
	double range, Pick &out)
{
	out = Pick{};
	if (!g.active || who == nullptr) return false;

	// The sector the ray STARTS in, which for a hand is not always the one the
	// pawn is in -- an arm can reach across a line. PointInSector costs a BSP
	// descent and is the honest answer; who->Sector would quietly mis-seed the
	// trace for exactly the poses this exists to serve.
	sector_t *from = g.level != nullptr
		? g.level->PointInSector(start.XY()) : who->Sector;
	if (from == nullptr) from = who->Sector;
	if (from == nullptr) return false;

	PickScan scan;
	FTraceResults res;
	Trace(start, from, dir, range, ActorFlags::FromInt(0), 0, who, res,
		TRACE_NoSky, PickScanCallback, &scan);

	if (!scan.found)
	{
		if (roth_useray_debug)
			Printf("roth_pick: nothing in %.0f units from (%.0f %.0f %.0f)\n",
				range, start.X, start.Y, start.Z);
		return false;
	}

	out = scan.best;
	if (roth_useray_debug)
	{
		static const char *kindName[] = { "nothing", "wall face", "flat", "door leaf" };
		Printf("roth_pick: %s at %.1f units, band 0x%02x, z %.1f\n",
			kindName[(int)out.kind], out.dist, (unsigned)out.band, out.hitPos.Z);
	}
	return true;
}

bool Activate(const Pick &p, AActor *who, TrigEvent want)
{
	if (!g.active || who == nullptr) return false;

	switch (p.kind)
	{
	case Pick::DoorLeaf:
		// A leaf is not a trigger and has no chain: it IS the door. Examining
		// one is not a thing the original does, so only a use swings it.
		// Through ActivateLine, which passes a use on a closed leaf to the room
		// face behind it (the original's route) and swings the leaf by itself
		// only where that face has no chain.
		if (want != TrigEvent::Use || p.line == nullptr) return false;
		{
			DVector3 hp = p.hitPos;
			return ActivateLine(p.line, who, p.side, SPAC_Use, &hp);
		}

	case Pick::WallFace:
		// WITH THE HIT POSITION, which is the entire point. ActivateLine's one
		// existing caller passes nullptr, so the wall-band gate has never run
		// on any channel; handing it a real hit point is what turns that rule
		// on for the first time.
		{
			DVector3 hit = p.hitPos;
			return ActivateLine(p.line, who, p.side,
				want == TrigEvent::Examine ? SPAC_UseBack : SPAC_Use, &hit);
		}

	case Pick::Prop:
		return ActivateObject(p.actor, want);

	case Pick::Flat:
	{
		// UseFlatAt wants a trace result. Fill in the fields it reads rather
		// than tracing again -- the ray has already been walked once and a
		// second walk down a slightly different line is how two answers to one
		// question get created.
		FTraceResults res{};
		res.HitType = TRACE_HitFloor;
		res.Sector  = p.sector;
		res.HitPos  = p.hitPos;
		res.ffloor  = nullptr;
		return UseFlatAt(who, res, want);
	}

	default:
		return false;
	}
}

//==========================================================================
//
// THE OBJECT CHANNEL
//
// Transcribed from run_leftclick_object_trigger (E/raw_commands.c:2920) and
// dispatch_entry_command_trigger_b's channel 2 (:3145-3164) -- the only two
// places the original acts on a clicked object. In order, both do:
//
//   1. the object's id is word[obj + 0x0e], which roth_raw.h already names
//      Object::commandID. Id 0 means the object is not addressable at all.
//   2. walk the category's list and take the FIRST record whose key at +0x08
//      equals that id and whose +0x02 does NOT carry bit 3 (spent).
//   3. left-click only: +0x06 bit 1 means destroy-on-use, and the chain is
//      meant to run with the object already gone.
//   4. left-click only: branch on the BASE at +0x03 -- base 2 is a light
//      switch, base 8 runs the chain, anything else acts but does nothing.
//      The examine channel has no base branch: it just walks the chain.
//   5. the examine channel carries NO direction mask and NO bounding box. The
//      original says so in its own comment on that branch.
//
// THE LIGHT SWITCH IS THE POINT. LightSwitch() and its effect tick have been
// here for weeks; what was missing was any route from clicking an object to
// them -- so a switch authored as an object could not work while the identical
// switch authored as a wall face could, and nothing about that was visible
// from outside the engine.
//
//==========================================================================

// word[obj + 4] ^= 1, which is NOT a state flag: +0x04 is the texture pair
// (index in the low byte, source in the high one), so flipping bit 0 swaps the
// object to its neighbouring sprite. That IS how a lever shows which way it is
// thrown -- the two positions are two pieces of art. Done the same way opcode
// 0x0d repaints a prop, so both land on one convention.
static void FlipObjectTexture(int rothSector, int rothIndex)
{
	if (rothSector < 0 || rothSector >= (int)g.map.objects.size()) return;
	std::vector<Object> &group = g.map.objects[rothSector];
	if (rothIndex < 0 || rothIndex >= (int)group.size()) return;

	Object &o = group[rothIndex];
	const uint16_t word = (uint16_t)((o.textureIndex | (o.textureSource << 8)) ^ 1);
	o.textureIndex  = (uint8_t)(word & 0xFF);
	o.textureSource = (uint8_t)(word >> 8);

	AActor *mo = FindObjectActor(rothSector, rothIndex);
	if (mo == nullptr) return;
	const int sn = FindLogicSprite(word);
	if (sn >= 0) mo->sprite = sn;
	else g.unhandledOps[0x02]++;    // no sprite built for the flipped word
}

bool ActivateObject(AActor *mo, TrigEvent want)
{
	if (!g.active || mo == nullptr) return false;

	int rothSector = -1, rothIndex = -1;
	if (!ObjectForActor(mo, rothSector, rothIndex)) return false;
	if (rothSector < 0 || rothSector >= (int)g.map.objects.size()) return false;
	const std::vector<Object> &group = g.map.objects[rothSector];
	if (rothIndex < 0 || rothIndex >= (int)group.size()) return false;

	const uint16_t id = group[rothIndex].commandID;
	if (id == 0)
	{
		if (roth_trigger_debug)
			Printf("roth_trigger: object %d/%d carries id 0 -- not addressable\n",
				rothSector, rothIndex);
		// No command: a plain object, which the original picks up or
		// examines (input.c:1034-1086, 982-1014).
		return InterfaceObjectAction(mo, rothSector, rothIndex, want == TrigEvent::Use);
	}

	auto it = g.byObject.find((int)id);
	if (it == g.byObject.end())
	{
		if (roth_trigger_debug)
			Printf("roth_trigger: object %d/%d (id %u) -- NO chain bound\n",
				rothSector, rothIndex, (unsigned)id);
		return InterfaceObjectAction(mo, rothSector, rothIndex, want == TrigEvent::Use);
	}

	// Stage "what the player just acted on" as the OBJECT, the same way a face
	// and a sector are staged, so a chain command carrying key 0 finds it.
	g.activeObjectSector = rothSector;
	g.activeObjectIndex  = rothIndex;
	g.activeFace = -1;          // run_leftclick_object_trigger :2950, g_active_object(s) = 0

	bool any = false;
	for (const Binding &b : it->second)
	{
		if (b.event != want) continue;

		// THE MATCHED RECORD, not the record its link names. This used to be
		// Rec(b.chain) -- the trigger's +0x04 -- so a light switch was run on
		// whatever record its link pointed at, and one with a link of 0 never
		// ran at all. run_leftclick_object_trigger hands cmd_light_switch the
		// record it matched by id (raw_commands.c:2932-2936, 2953-2954), and
		// skips a matched record that is disabled (:2950).
		Command *rec = Rec(b.record);
		if (rec == nullptr) continue;

		// BASE 2: a light switch. The record IS the switch -- there is no chain
		// to walk -- so this branch returns without firing one, as the original
		// does. Use only; the examine channel has no base branch.
		if (want == TrigEvent::Use && b.opcode == 0x02)
		{
			if (rec->modifier & 0x08) continue;                        // :2950
			if (LightSwitch(rec, (int)b.record) != CMD_ACTED) continue;

			// The sound at +0x0e, stored one higher so 0 can mean silence:
			// play_sound_effect(snd - 1, 0) (raw_commands.c:2955-2957).
			const uint16_t snd = rec->Word(0x0E);
			if (snd != 0) SoundPlayEffect((uint32_t)snd - 1, 0);

			FlipObjectTexture(rothSector, rothIndex);

			if (roth_trigger_debug)
				Printf("roth_trigger: object %d/%d (id %u) LIGHT SWITCH -> acted"
					" (sound id %u, 0 = silent)\n",
					rothSector, rothIndex, (unsigned)id, (unsigned)snd);
			any = true;
			continue;
		}

		if (roth_trigger_debug)
			Printf("roth_trigger: object %d/%d (id %u) base 0x%02x %s -> firing\n",
				rothSector, rothIndex, (unsigned)id, (unsigned)b.opcode,
				TrigEventName(b.event));

		// DESTROY ON USE, before the chain runs, which is the order the original
		// uses -- the chain is written expecting the object to be gone.
		const bool destroys = (want == TrigEvent::Use) && (b.fireFlags & 2) != 0;
		if (destroys)
		{
			if (roth_trigger_debug)
				Printf("roth_trigger: object %d/%d destroyed on use\n",
					rothSector, rothIndex);
			mo->Destroy();
		}

		// An object USED runs its chain with the record's sound and no
		// position (run_command_chain, raw_commands.c:3929-3936); an object
		// EXAMINED clears the sound (fire_trigger_on_interact, :1683).
		StageChainSound(want == TrigEvent::Use ? b.sound : (uint16_t)0);
		any |= Fire(b.chain);
		if (destroys) break;    // the actor is gone; nothing else may touch it
	}
	// g_command_active_chain is cleared when the object's chain exits
	// (raw_commands.c:1331, 2960, 2964): a later chain has no clicked object.
	g.activeObjectSector = -1;
	g.activeObjectIndex  = -1;
	return any;
}

bool UseByHand(player_t *player, TrigEvent want)
{
	if (!g.active || player == nullptr || player->mo == nullptr) return false;

	// NOT in a netgame and not without a controller. AttackPos and its offhand
	// pair are written by the VR backend every frame and are local state; with
	// no device they are the pawn's own position and the ray would be a
	// confusing duplicate of the eye probe rather than a hand.
	if (!player->mo->OverrideAttackPosDir) return false;

	AActor *const mo = player->mo;

	// THE HAND'S OWN RANGE, not the weapon's 48 units. A press is meant to
	// reach what the beam is visibly touching, and the beam crosses rooms.
	// UseRange on the pawn is the player's configured reach and is what the eye
	// probe already uses, so the two agree about how far a press carries.
	const double range = mo->FloatVar(NAME_UseRange);

	struct HandRay { DVector3 pos; DAngle pitch; DAngle yaw; const char *name; };
	const HandRay hands[2] = {
		{ mo->AttackPos,  mo->AttackPitch,  mo->AttackAngle,  "main" },
		{ mo->OffhandPos, mo->OffhandPitch, mo->OffhandAngle, "off"  },
	};

	for (const HandRay &h : hands)
	{
		// THE +90 IS THE ENGINE'S, NOT THE CONTROLLER'S. P_UseLines builds its
		// hand ray as AttackAngle + 90 and that is the convention the stored
		// angle carries, so it is kept here rather than "corrected" -- the
		// drawn laser disagreeing with it is a separate fault in the laser.
		const DAngle yaw = h.yaw + DAngle::fromDeg(90.);
		const DVector3 dir(
			yaw.Cos() * h.pitch.Cos(),
			yaw.Sin() * h.pitch.Cos(),
			-h.pitch.Sin());

		Pick p;
		if (!PickFrom(mo, h.pos, dir, range, p)) continue;
		if (roth_useray_debug)
			Printf("roth_pick: %s hand -> ", h.name);
		if (Activate(p, mo, want)) return true;
	}
	return false;
}

void FireSectorTriggers(sector_t *sec, AActor *who, TrigEvent want)
{
	if (!g.active || sec == nullptr) return;
	g.activeSector = sec->sectornum;
	// fire_sector_trigger (raw_commands.c:2883) clears the wall and the source
	// object before the chain runs.
	g.activeFace = -1;
	g.activeObjectSector = g.activeObjectIndex = -1;
	auto it = g.bySector.find(sec->sectornum);
	if (it == g.bySector.end()) return;
	for (const Binding &b : it->second)
	{
		if (b.event != want) continue;

		// THE LINK TEST, for 0x13 only. A water record is inside its link only
		// while the player is at or below the sector's floor, and a linked one
		// only while standing on the mid-platform's top. Without this a water
		// trigger fires on walking into the sector instead of on going under.
		if (want == TrigEvent::SectorEnter && who != nullptr)
		{
			const LinkVariant var = VariantOf(b.subFlags);
			if (var != LinkVariant::Plain)
			{
				const double pz = who->Z();
				bool inLink = false;
				if (var == LinkVariant::Water)
				{
					// exit_link = player_z > sector[+2]; sector[+2] is floorHeight.
					const int rs = sec->sectornum;
					if (rs >= 0 && rs < (int)g.map.sectors.size())
						inLink = pz <= double(g.map.sectors[rs].floorHeight);
				}
				else
				{
					// exit_link = (pz > topZ) || (pz + 2 < topZ): standing ON it.
					const int rs = sec->sectornum;
					const int pi = (rs >= 0 && rs < (int)g.map.sectors.size())
						? g.map.sectors[rs].platformIndex : -1;
					if (pi >= 0 && pi < (int)g.map.platforms.size())
					{
						const double topZ = double(g.map.platforms[pi].topZ);
						inLink = (pz <= topZ) && (pz + 2.0 >= topZ);
					}
				}
				if (!inLink)
				{
					if (roth_trigger_debug)
						Printf("roth_trigger: sector %d 0x%02x %s variant -> OUTSIDE"
							" the link at z %.1f, not firing\n", sec->sectornum,
							(unsigned)b.opcode, VariantName(var), pz);
					continue;
				}
			}
		}

		// The facing mask, from the record's own +0x06. fire_sector_trigger
		// tests this before anything else runs (raw_commands.c:2876), so a
		// trigger the player is facing the wrong way for does not fire at all.
		if (!FacingAllows(b.fireFlags, who))
		{
			if (roth_trigger_debug)
				Printf("roth_trigger: sector %d 0x%02x %s -> BLOCKED by facing"
					" mask 0x%02x\n", sec->sectornum, (unsigned)b.opcode,
					TrigEventName(b.event), (unsigned)b.fireFlags);
			continue;
		}

		if (roth_trigger_debug)
			Printf("roth_trigger: sector %d 0x%02x %s mask 0x%02x -> firing\n",
				sec->sectornum, (unsigned)b.opcode, TrigEventName(b.event),
				(unsigned)b.fireFlags);
		// fire_sector_trigger latches the player's position only for a record
		// whose +0x07 has bit 0x04 (raw_commands.c:2873-2876).
		if ((b.subFlags & 4) && who != nullptr)
			StageChainSoundAt(b.sound, (int)floor(who->X()), (int)floor(who->Y()));
		else
			StageChainSound(b.sound);
		Fire(b.chain);
	}
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

namespace { void TickGameFlow(); void TickMenus(); void HoldPlayer(bool hold); }   // the death tick, beside the map-state code below

void TickLevelLogic(FLevelLocals *level)
{
	if (!g.active || level == nullptr || level != g.level) return;

	// An arrival point waits for a player to exist, which it does not at
	// BeginLevel time.
	ConsumeArrival();

	// The command core's per-frame half -- the speed latch, the particles, the
	// pain flash -- comes first in tick_world_effects (raw_commands.c:3290-3297),
	// before the effect pool and its delay timers.
	CmdTick();

	// Health at 0: the death block and its wait (game_core.c:948-1020). The
	// frame loop tests health after the world has ticked.
	TickGameFlow();

	// A menu or the death wait is modal in the original: the world does not
	// tick under it. The panels, the sound and a movie the menu asked for
	// (the movie clips) still run.
	TickMenus();
	HoldPlayer(g_menus.IsOpen() || g_flow.Dying());
	if (g_menus.IsOpen() || g_flow.Dying())
	{
		InterfaceTick(level);
		SoundTick(level);
		MoviePump();
		DrainTimedCommands(level);   // tests drive menus with roth_at
		return;
	}

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

	// roth_platforms: tell the platform actors where the effects just put
	// their sectors. After the pool, so an actor is never a frame behind it.
	DrivePlatforms();

	// The actors and their bullets: tick_dynamic_entities, from the frame
	// step after the world effects (game_core.c, gameplay_frame_step).
	ActorsTick(level, ROTH_FRAME_TICKS_PER_TIC);

	DrainDeferred();
	ApplyPendingWarp();
	DrainPendingDoors();
	DrainPendingDialogue();
	DrainTimedCommands(level);
	// The dialogue window and the inventory screen: after the chains, as the
	// original's text pass follows its logic in the frame.
	InterfaceTick(level);

	// Realms' sound: the node pump (play_nearby_sfx_emitters, from
	// update_player_tick, player.c:420), update_active_sounds, the doors.
	SoundTick(level);

	// Movies the flow asked for this tic start now, as one blocking cutscene
	// (roth_movie.h), so the level does not tick again before them: the
	// original's play_gdv_cutscene returns only when the movie is over
	// (gdv_cutscene.c:2840).
	MoviePump();
}

//==========================================================================
//
// roth_door -- swing a door leaf on demand.
//
// WHY THIS EXISTS. The leaves have been built, hinged and tagged for weeks and
// NOTHING HAS EVER OPENED ONE: the only route in is a 0x2F command, which needs
// a trigger to fire, which needs the player to use the right wall. So the swing
// itself -- the geometry, the winding, the arc, the rate -- has never once been
// watched. This is the shortest path to seeing it, and it deliberately goes
// through SwingDoor so it tests the REAL conversion rather than a copy.
//
// It takes the command record's RAW fields, so the numbers here are the numbers
// a map would supply:
//   roth_door <tag|all> [speedM] [dwellTicks]
// speedM is byte +0x07 (0 behaves as 1); dwellTicks is word +0x0a in 70 Hz
// ticks. With no arguments it lists the tags instead of opening anything, so a
// mistyped tag does not look like a broken door.
//
//==========================================================================

//==========================================================================
//
// roth_trigger_list -- every trigger binding, with the geometry it is on, the
// opcode, and the event that fires it.
//
// The load report counts triggers per opcode; this says WHERE each one is, which
// is what you need to stand on one. Finding a sector that carries an 0x13 used
// to mean guessing a pose and hoping.
//
// Optional argument filters by opcode: `roth_trigger_list 13`, in hex, as the
// specs write it.
//
//==========================================================================

CCMD(roth_trigger_list)
{
	if (!g.active)
	{
		Printf("roth_trigger_list: no Realms level loaded\n");
		return;
	}

	int filter = -1;
	if (argv.argc() >= 2) filter = (int)strtol(argv[1], nullptr, 16);

	Printf("Realms trigger bindings%s:\n",
		filter >= 0 ? FStringf(" for opcode 0x%02x", (unsigned)filter).GetChars() : "");

	int shown = 0;
	for (const auto &kv : g.bySector)
	{
		for (const Binding &b : kv.second)
		{
			if (filter >= 0 && b.opcode != (uint8_t)filter) continue;
			Printf("  sector %5d  0x%02x  %-18s  chain %d\n", kv.first,
				(unsigned)b.opcode, TrigEventName(b.event), (int)b.chain);
			shown++;
		}
	}
	for (const auto &kv : g.bySide)
	{
		for (const Binding &b : kv.second)
		{
			if (filter >= 0 && b.opcode != (uint8_t)filter) continue;
			auto f = g.sideToFace.find(kv.first);
			Printf("  side   %5d  0x%02x  %-18s  chain %d  (face %d)\n", kv.first,
				(unsigned)b.opcode, TrigEventName(b.event), (int)b.chain,
				f != g.sideToFace.end() ? f->second : -1);
			shown++;
		}
	}
	Printf("  %d binding(s)\n", shown);
}

CCMD(roth_door)
{
	if (argv.argc() < 2)
	{
		Printf("roth_door <tag|all> [speedM] [dwellTicks] [afterTics]\n");
		if (g.level != nullptr && !g.doorTag.empty())
		{
			FString tags;
			for (const auto &kv : g.doorTag) tags.AppendFormat("%d ", kv.second);
			Printf("  %d door leaves, tags: %s\n", (int)g.doorTag.size(), tags.GetChars());
		}
		else Printf("  (no Realms level loaded yet)\n");
		return;
	}

	PendingDoor d;
	d.tag      = (strcmp(argv[1], "all") == 0) ? 0 : atoi(argv[1]);
	d.speedMul = argv.argc() > 2 ? atoi(argv[2]) : 0;
	d.dwell    = argv.argc() > 3 ? atoi(argv[3]) : 0;
	d.after    = argv.argc() > 4 ? atoi(argv[4]) : 0;
	gPendingDoors.push_back(d);
	Printf("roth_door: queued (tag %d, speedM %d, dwell %d, after %d tics)\n",
		d.tag, d.speedMul, d.dwell, d.after);
}

//==========================================================================
//
// roth_dialogue <action id> -- run one DBASE100 action record through the game
// core, exactly as RAW 0x2b would (eval_dialogue_record_by_id, then the chain
// tail), and say what came of it. A test instrument: it lets a config file
// prove the interpreter runs inside the engine with nobody at the controls.
//
// roth_inventory -- print what the core's inventory holds.
//
//==========================================================================

// Typed from an +exec script these run BEFORE the deferred `map`, so, like
// roth_door, they are queued and drained from the level tick, in order, once
// the core is attached. A negative id asks for the inventory listing.
namespace
{

std::vector<int> gPendingDialogue;

void DrainPendingDialogue()
{
	if (gPendingDialogue.empty() || !g_core.IsAttached()) return;
	std::vector<int> batch;
	batch.swap(gPendingDialogue);
	for (int id : batch)
	{
		if (id >= 100000)
		{
			StageChainSound(0);
			const bool acted = Fire((uint16_t)(id - 100000));
			Printf("roth_fire: chain %d %s\n", id - 100000, acted ? "acted" : "did nothing");
			continue;
		}
		if (id < 0)
		{
			Inventory &inv = g_core.Items();
			Printf("roth_inventory: %d item(s), left hand slot %d, right hand slot %d\n",
				inv.Count(), inv.LeftHand(), inv.RightHand());
			for (int i = 0; i < inv.Count(); i++)
				Printf("  slot %d: item %d x%d\n", i, (int)inv.Slot(i).id, (int)inv.Slot(i).quantity);
			continue;
		}
		const std::map<std::string, int> before = g_coreHost.calls;
		const int firedBefore = g.fired;
		const uint32_t r = g_core.RunAction((uint32_t)id);
		Printf("roth_dialogue: action %d returned %d, map chains that acted because of it: %d\n",
			id, (int)r, (int)(g.fired - firedBefore));
		for (const auto &kv : g_coreHost.calls)
		{
			const auto it = before.find(kv.first);
			const int was = it == before.end() ? 0 : it->second;
			if (kv.second != was) Printf("  asked for, not built: %s x%d\n", kv.first.c_str(), kv.second - was);
		}
	}
}

//==========================================================================
//
// One map's logic, kept and restored (roth_gameflow.h, MapLogicState).
//
//==========================================================================

void CaptureMapLogic(MapLogicState &out)
{
	MapLogicState s;
	s.mapName = g.level != nullptr ? g.level->MapName.GetChars() : "";
	s.commands.reserve(g.map.commands.size());
	for (const Command &c : g.map.commands)
	{
		SavedCommand sc;
		sc.modifier = c.modifier;
		sc.disabled = c.disabled ? 1 : 0;
		sc.args = c.args;
		s.commands.push_back(sc);
	}
	for (const PendingDelay &d : g.delays) s.delays.push_back({ d.remaining, d.record, d.context });
	for (const Deferred &d : g.deferred) s.deferred.push_back({ d.chain, d.context });
	for (const Effect &e : g.effects)
	{
		SavedEffect se;
		se.tick = e.tick; se.record = e.record; se.payload = e.payload;
		se.sectors.assign(e.sectors.begin(), e.sectors.end());
		se.finished = e.finished ? 1 : 0;
		se.baseLight = e.baseLight;
		se.hold = e.hold; se.holding = e.holding ? 1 : 0; se.flags5 = e.flags5;
		se.sfxNode = e.sfxNode; se.sfxStarted = e.sfxStarted ? 1 : 0;
		for (const ObjectRef &o : e.objects) { se.objects.push_back(o.sector); se.objects.push_back(o.index); }
		for (int k = 0; k < 4; k++) se.moveVerts[k] = e.moveVerts[k];
		s.effects.push_back(se);
	}
	s.pendingRecord = g.pendingRecord;
	s.activeFace = g.activeFace; s.activeSector = g.activeSector;
	s.activeObjectSector = g.activeObjectSector; s.activeObjectIndex = g.activeObjectIndex;
	for (const auto &kv : g.movers) { s.movers.push_back(kv.first); s.movers.push_back(kv.second.first); s.movers.push_back(kv.second.second); }
	for (const auto &kv : g.lastSectorByPlayer) { s.lastSectorByPlayer.push_back(kv.first); s.lastSectorByPlayer.push_back(kv.second); }
	s.dialogueLatch = g_dialogueLatch ? 1 : 0;
	out = s;
}

// The revisit: what was kept replaces what the load-time passes just built,
// as read_raw_state_stream replaces it (map_load.c:812-904). Refused, and
// counted, when the record count no longer matches the map.
bool RestoreMapLogic(const MapLogicState &s)
{
	if (s.commands.size() != g.map.commands.size())
	{
		TheLog().Count("flow: kept map state does not match this map's command records; not restored");
		return false;
	}
	for (size_t i = 0; i < s.commands.size(); i++)
	{
		Command &c = g.map.commands[i];
		c.modifier = s.commands[i].modifier;
		c.disabled = s.commands[i].disabled != 0;
		if (s.commands[i].args.size() == c.args.size()) c.args = s.commands[i].args;
	}
	g.delays.clear();
	for (const SavedDelay &d : s.delays) { PendingDelay p; p.remaining = d.remaining; p.record = d.record; p.context = d.context; g.delays.push_back(p); }
	g.deferred.clear();
	for (const SavedDeferred &d : s.deferred) { Deferred q; q.chain = d.chain; q.context = d.context; g.deferred.push_back(q); }
	g.effects.clear();
	for (const SavedEffect &se : s.effects)
	{
		Effect e;
		e.tick = se.tick; e.record = se.record; e.payload = se.payload;
		e.sectors.assign(se.sectors.begin(), se.sectors.end());
		e.finished = se.finished != 0;
		e.baseLight = se.baseLight;
		e.hold = se.hold; e.holding = se.holding != 0; e.flags5 = se.flags5;
		e.sfxNode = se.sfxNode; e.sfxStarted = se.sfxStarted != 0;
		for (size_t k = 0; k + 1 < se.objects.size(); k += 2) e.objects.push_back({ se.objects[k], se.objects[k + 1] });
		for (int k = 0; k < 4; k++) e.moveVerts[k] = se.moveVerts[k];
		g.effects.push_back(e);
	}
	g.pendingRecord = s.pendingRecord;
	g.activeFace = s.activeFace; g.activeSector = s.activeSector;
	g.activeObjectSector = s.activeObjectSector; g.activeObjectIndex = s.activeObjectIndex;
	g.movers.clear();
	for (size_t k = 0; k + 2 < s.movers.size(); k += 3) g.movers[s.movers[k]] = { s.movers[k + 1], s.movers[k + 2] };
	g.lastSectorByPlayer.clear();
	for (size_t k = 0; k + 1 < s.lastSectorByPlayer.size(); k += 2) g.lastSectorByPlayer[s.lastSectorByPlayer[k]] = s.lastSectorByPlayer[k + 1];
	g_dialogueLatch = s.dialogueLatch != 0;
	return true;
}

//==========================================================================
//
// Death (game_core.c:948-1020), driven once per engine tic.
//
// The original's wait counts g_frame_time_scale per pass up to 0x12c; at this
// port's fixed schedule a tic is one frame step at scale 2
// (ROTH_FRAME_TICKS_PER_TIC), so the wait is 150 tics, 4.3 seconds -- the
// same 4.3 seconds the original waits (measured in the oracle: 300 units).
// What the wait's two keys do is not wired here: Esc (ends the wait) and
// scancode 0x39 (loads the last slot) need the input layer.
//
//==========================================================================

//==========================================================================
//
// The flow's engine side: the new-game block, restarts, the save slots, the
// menus' outcomes, and the death wait (game_core.c:948-1020) shown as a box.
//
//==========================================================================

void RunNewGameBlock()
{
	// game_core.c:821-845: inventory cursors and inventory cleared, action 3,
	// Adam latched, health to the maximum, no weapon, mode 1.
	g_flow.NewGame();
	g_cmd.NewGame();                    // the RAW command core's own new-game state
	g_cmd.health = g_flow.Health();     // one health (see CoreHost)
	CmdPushHealth();
	g_mapStates.clear();
}

// reset_and_start_new_game (game_core.c:438-471): every map's kept state gone,
// fresh databases, the default map. Then either the main menu (after a death,
// game_core.c:1006-1012) or straight into the new-game block (the options'
// New game: game_play_loop's outer loop with ebx 3 skips the menu).
void RestartGame(bool toMenu)
{
	g_flow.ResetForMenu();
	g_mapStates.clear();
	g_menus.CloseAll();
	g_flowNewGamePending = !toMenu;
	g_flowMenuPending = toMenu;
	g_flowRestarting = true;
	g_warpCarryXY = false;
	g_warpArrivalSector = -1;
	if (!g_flowDefaultMap.empty()) G_DeferedInitNew(g_flowDefaultMap.c_str());
}

// SAVE<n>.SAV becomes the engine's own savegame "realms<n>" in its own save
// folder (G_BuildSaveName); slot 0 is the quick slot (input.c:545-559).
FString SlotFile(int slot) { return G_BuildSaveName(FStringf("realms%d", slot).GetChars()); }

// read_savegame_slot_names (savegame.c:1262-1295): the name stored in each of
// slots 1..9, empty where there is no save.
std::vector<std::string> ReadSlotNames()
{
	std::vector<std::string> out(RealmsMenus::SLOTS);
	for (int slot = 1; slot <= RealmsMenus::SLOTS; slot++)
	{
		const FString path = SlotFile(slot);
		if (!FileExists(path.GetChars())) continue;
		std::unique_ptr<FResourceFile> f(FResourceFile::OpenResourceFile(path.GetChars(), true));
		std::string name = "(saved game)";
		if (f != nullptr)
		{
			const int info = f->FindEntry("info.json");
			if (info >= 0)
			{
				auto data = f->Read(info);
				FSerializer arc;
				if (arc.OpenReader(data.string(), data.size()))
				{
					const FString t = arc.GetString("Title");
					if (t.IsNotEmpty()) name = t.GetChars();
				}
			}
		}
		out[(size_t)(slot - 1)] = name;
	}
	return out;
}

std::string DefaultSaveName()
{
	// The original types the name into a text row (show_message_box's
	// 0x40000000 row, menu_hud_ui.c:1276). There is no keyboard in the
	// headset, so the save is named for the player: the place and the time.
	std::string place = g.level != nullptr ? g.level->LevelName.GetChars() : "Realms";
	char t[32] = "";
	const time_t now = time(nullptr);
	if (const struct tm *lt = localtime(&now)) strftime(t, sizeof t, " %d %b %H:%M", lt);
	std::string n = place + t;
	if (n.size() > 0x2f) n.resize(0x2f);            // the 0x30-byte name chunk (savegame.c:1568)
	return n;
}

bool SaveSlot(int slot, const std::string &name)
{
	if (gamestate != GS_LEVEL || g_flow.Dying()) return false;
	G_SaveGame(SlotFile(slot).GetChars(), name.c_str());
	g_flow.SetLastSlot(slot);                       // savegame.c:1631
	Printf("Realms: saving slot %d (\"%s\")\n", slot, name.c_str());
	return true;
}

bool LoadSlot(int slot)
{
	const FString path = SlotFile(slot);
	if (slot < 0 || !FileExists(path.GetChars()))
	{
		Printf("Realms: slot %d holds no save\n", slot);
		return false;
	}
	g_flowPendingLoadSlot = slot;
	g_menus.CloseAll();
	G_LoadGame(path.GetChars(), true);
	Printf("Realms: loading slot %d\n", slot);
	return true;
}

// What a chosen row asks for (roth_menus.h, MenuOutcome).
void ApplyMenuOutcome(const MenuOutcome &o)
{
	switch (o.kind)
	{
	case MenuOutcome::PLAY: RunNewGameBlock(); break;
	case MenuOutcome::LOAD: LoadSlot(o.slot); break;
	case MenuOutcome::SAVE: SaveSlot(o.slot, o.name); break;
	case MenuOutcome::NEW_GAME: RestartGame(false); break;
	case MenuOutcome::QUIT:
		// game_core.c:799-805: action 2, then the program ends.
		g_core.RunAction(GameCore::ACTION_QUIT);
		AddCommandString("quit");
		break;
	case MenuOutcome::RUN_ACTION: g_core.RunAction((uint32_t)o.slot); break;
	case MenuOutcome::DEATH_LOAD:
		// game_core.c:984-997: a failed load ends the wait and restarts.
		if (!LoadSlot(g_flow.LastSlot())) { g_flow.DeathLoadFailed(); RestartGame(true); }
		break;
	case MenuOutcome::DEATH_END: RestartGame(true); break;
	default: break;
	}
}

// Under a menu or the death wait the original's frame loop does not run the
// world (show_message_box and the death loop are modal): the player is held
// still. The engine's own flag; set only by this, cleared only by this.
player_t *g_frozenPlayer = nullptr;

void HoldPlayer(bool hold)
{
	player_t *p = g.level != nullptr ? g.level->GetConsolePlayer() : nullptr;
	if (g_frozenPlayer != nullptr && (g_frozenPlayer != p || !hold))
	{
		if (g_frozenPlayer == p) g_frozenPlayer->cheats &= ~CF_TOTALLYFROZEN;
		g_frozenPlayer = nullptr;
	}
	if (hold && p != nullptr && g_frozenPlayer == nullptr && !(p->cheats & CF_TOTALLYFROZEN))
	{
		p->cheats |= CF_TOTALLYFROZEN;
		g_frozenPlayer = p;
	}
}

RealmsMenus::Box g_lastBox = RealmsMenus::NONE;

void TickMenus()
{
	// Entering a slot list reads the slots afresh (read_savegame_slot_names
	// runs each time a list opens, menu_hud_ui.c:2047, 2061, 2097).
	const RealmsMenus::Box b = g_menus.Current();
	if (b != g_lastBox && (b == RealmsMenus::LOAD || b == RealmsMenus::SAVE))
	{
		g_menus.SetSlotNames(ReadSlotNames());
		g_menus.SetDefaultSaveName(DefaultSaveName());
	}
	g_lastBox = b;
	while (g_menus.HasOutcome()) ApplyMenuOutcome(g_menus.Take());
}

void TickGameFlow()
{
	if (!g_flow.IsAttached() || !g_flow.BootDone() || g_flowRestarting) return;
	CmdPullHealth();                    // the engine's own damage, taken in
	GameFlow::DeathStep step;
	if (!g_flow.Dying())
	{
		if (g_flow.Health() > 0) return;
		Printf("Realms: health is 0 -- the death sequence (action 4)\n");
		step = g_flow.BeginDeath();
		const bool canLoad = g_flow.LastSlot() != -1 && FileExists(SlotFile(g_flow.LastSlot()).GetChars());
		if (step == GameFlow::DeathStep::Waiting) g_menus.OpenDeath(canLoad);
	}
	else step = g_flow.TickDeath(ROTH_FRAME_TICKS_PER_TIC, 0);
	if (step == GameFlow::DeathStep::Restart) RestartGame(true);
}

} // namespace

CCMD(roth_dialogue)
{
	if (argv.argc() < 2) { Printf("roth_dialogue <action id>\n"); return; }
	const int id = atoi(argv[1]);
	if (id < 1) { Printf("roth_dialogue: ids start at 1\n"); return; }
	gPendingDialogue.push_back(id);
}

// roth_fire <command index> -- run the chain that starts at that 1-based
// command record, as a trigger naming it would. A test instrument.
CCMD(roth_fire)
{
	if (argv.argc() < 2) { Printf("roth_fire <1-based command index>\n"); return; }
	const int idx = atoi(argv[1]);
	if (idx < 1 || idx > 65535) { Printf("roth_fire: index out of range\n"); return; }
	gPendingDialogue.push_back(100000 + idx);
}

CCMD(roth_inventory)
{
	gPendingDialogue.push_back(-1);
}

// The menus, for binding (flat) and for a controller binding (VR):
//   roth_options        the in-game options (New game, Load, Save, Settings, Quit)
//   roth_mainmenu       the front-end main menu
//   roth_quicksave      F9: slot 0          roth_quickload   F10: slot 0
//   roth_menu up|down|enter|back|scrollup|scrolldown|choose <row>|dump
// While a box is up, roth_ui up/down/enter/close drive it too, and the use
// button clicks the row under the beam or the crosshair.
CCMD(roth_options) { if (g_flow.IsAttached() && !g_flow.Dying()) g_menus.OpenOptions(); }
CCMD(roth_mainmenu) { if (g_flow.IsAttached() && !g_flow.Dying()) g_menus.OpenMain(); }
CCMD(roth_quicksave) { if (g_flow.IsAttached()) SaveSlot(0, DefaultSaveName()); }
CCMD(roth_quickload) { if (g_flow.IsAttached()) LoadSlot(0); }
CCMD(roth_menu)
{
	if (argv.argc() < 2) { Printf("roth_menu up|down|enter|back|scrollup|scrolldown|choose <row>|dump\n"); return; }
	const std::string v = argv[1];
	if (v == "up") g_menus.Up();
	else if (v == "down") g_menus.Down();
	else if (v == "enter") g_menus.Enter();
	else if (v == "back") g_menus.Back();
	else if (v == "scrollup") g_menus.ScrollUp();
	else if (v == "scrolldown") g_menus.ScrollDown();
	else if (v == "choose" && argv.argc() > 2) g_menus.Choose(atoi(argv[2]));
	Printf("[roth_menu] %s, %d row(s)%s\n", g_menus.Describe().c_str(), (int)g_menus.Rows().size(),
		g_menus.HasOutcome() ? ", a choice is pending" : "");
}

//==========================================================================
//
// roth_at <tic> <command ...> -- TEST TOOL. Runs any console command at a given
// level tic (the level's own maptime), in order. A +exec config runs before
// the map exists, so without this a test cannot say "open the panel, wait,
// then screenshot". Several entries for one tic run in the order given.
// Cleared on every level change; does nothing outside a Realms level.
//
//==========================================================================

static std::vector<std::pair<int, std::string>> gTimedCommands;

CCMD(roth_at)
{
	if (argv.argc() < 3) { Printf("roth_at <level tic> <console command ...>\n"); return; }
	std::string cmd;
	for (int i = 2; i < argv.argc(); i++) { if (i > 2) cmd += ' '; cmd += argv[i]; }
	gTimedCommands.emplace_back(atoi(argv[1]), cmd);
}

namespace {   // declared inside the file's anonymous namespace near the top, so defined in one too
void DrainTimedCommands(FLevelLocals *level)
{
	if (gTimedCommands.empty() || level == nullptr) return;
	const int now = level->maptime;
	std::vector<std::pair<int, std::string>> due, keep;
	for (auto &e : gTimedCommands) (e.first <= now ? due : keep).push_back(e);
	gTimedCommands.swap(keep);
	for (auto &e : due)
	{
		Printf("roth_at %d (now %d): %s\n", e.first, now, e.second.c_str());
		C_DoCommand(e.second.c_str());
	}
}
} // namespace

//==========================================================================
//
// Game flow: the engine's hooks. See roth_runtime.h.
//
//==========================================================================

bool DefaultStartMap(std::string &out)
{
	// Only for the Realms game itself (vrealms_iwad/IWADINFO, Config =
	// "VRealms"): the hands build shares the engine and must not start here.
	if (gameinfo.ConfigName.CompareNoCase("VRealms") != 0) return false;
	if (!TheInstall().IsOpen()) return false;
	const std::string res = TheInstall().DataFile("ROTH.RES");
	if (res.empty()) return false;
	std::vector<uint8_t> bytes;
	if (FILE *f = fopen(res.c_str(), "rb"))
	{
		fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
		if (n > 0) { bytes.resize((size_t)n); if (fread(bytes.data(), 1, (size_t)n, f) != (size_t)n) bytes.clear(); }
		fclose(f);
	}
	// load_map_list selects entry 0 (map_load.c:1050-1059), and the boot loads
	// it (game_core.c:1062-1068).
	const std::vector<MapListEntry> list = ParseMapList(bytes.data(), bytes.size());
	if (list.empty() || !TheInstall().HasMap(list[0].map.c_str())) return false;
	out = list[0].map;
	// A game started this way gets the main menu after the boot, as the
	// original's does (game_core.c:806-816). +map launches never come here.
	g_flowMenuPending = true;
	return true;
}

bool FlowWantsEscape()
{
	// Esc belongs to a Realms box or the death wait while one is up; in play
	// it stays the engine's (its own menu holds the VR and video settings).
	return g_flow.IsAttached() && (g_menus.IsOpen() || g_flow.Dying());
}

bool FlowResponder(const event_t *ev)
{
	if (ev == nullptr || ev->type != EV_KeyDown || !g_flow.IsAttached() || !g.active) return false;
	if (gamestate != GS_LEVEL || menuactive != MENU_Off) return false;
	const int k = ev->data1;
	if (g_menus.IsOpen())
	{
		if (g_menus.Current() == RealmsMenus::DEATH && k == KEY_SPACE)
		{
			// Scancode 0x39 in the death wait (game_core.c:984-997): the last
			// slot, or, with none, the end of the wait.
			if (g_menus.Rows()[0].enabled) g_menus.Choose(0); else g_menus.Back();
			return true;
		}
		switch (k)
		{
		case KEY_ESCAPE: case KEY_PAD_B: case KEY_PAD_BACK: g_menus.Back(); return true;
		case KEY_UPARROW: case KEY_PAD_DPAD_UP: g_menus.Up(); return true;
		case KEY_DOWNARROW: case KEY_PAD_DPAD_DOWN: g_menus.Down(); return true;
		case KEY_ENTER: case KEY_PAD_A: g_menus.Enter(); return true;
		default: return false;
		}
	}
	if (!g_flow.BootDone() || g_flow.Dying()) return false;
	// The keymap at 0x7093d: F9 (0x43) key_quicksave, F10 (0x44) key_quickload,
	// both on slot 0 (input.c:545-559).
	if (k == KEY_F9) { SaveSlot(0, DefaultSaveName()); return true; }
	if (k == KEY_F10) { LoadSlot(0); return true; }
	return false;
}

bool FlowRebornRequested(player_t *player)
{
	// A dead player's use press reaches the engine as a respawn request
	// (DeathThink -> PST_REBORN). While the Realms death wait is up it is a
	// press on the death box instead, by beam or crosshair.
	// Only for the dead pawn of the level being played: a new level's players
	// enter through the same call and must not be held back.
	if (player == nullptr || !g_flow.IsAttached() || !(g_flow.Dying() || g_flowRestarting)) return false;
	if (player->mo == nullptr || player->mo->health > 0 || !g.active || player->mo->Level != g.level) return false;
	// DeathThink asks every tic the button is held: one press per hold.
	static int lastAsk = -10;
	const int now = g.level->maptime;
	const bool fresh = now - lastAsk > 1;
	lastAsk = now;
	if (fresh && !g_flowRestarting) InterfaceUse(player);
	return true;
}

void LevelRestored(FLevelLocals *level)
{
	// The engine just replaced every thinker from a hub snapshot or a save, so
	// the props the loader spawned are gone and their saved copies stand in
	// their place.
	RebindObjectActors(level);
	if (!g.active || level != g.level) return;
	std::string cur = level->MapName.GetChars();
	for (char &c : cur) c = (char)toupper((unsigned char)c);
	auto it = g_mapStates.find(cur);
	if (it != g_mapStates.end()) RestoreMapLogic(it->second);
}

void SerializeRealmsGlobals(FSerializer &arc)
{
	static const char *KEY_G = "realms_globals", *KEY_GN = "realms_globals_size";
	static const char *KEY_M = "realms_maps", *KEY_MN = "realms_maps_size";
	if (arc.isWriting())
	{
		if (!g_flow.IsAttached() || g.level == nullptr) return;
		std::string cur = g.level->MapName.GetChars();
		for (char &c : cur) c = (char)toupper((unsigned char)c);
		CaptureMapLogic(g_mapStates[cur]);                       // write_raw_state_temp, savegame.c:1577
		g_flow.SetMusicChunk(MusicCurrentChunk());

		SaveWriter gw;
		g_flow.SaveGlobals(gw, PlayerRecord(), cur);             // the pawn is the engine's own
		SaveWriter mw;
		mw.U32((uint32_t)g_mapStates.size());                    // bundle_level_states, savegame.c:1138-1192
		for (const auto &kv : g_mapStates) { mw.Str(kv.first); kv.second.Save(mw); }

		int gn = (int)gw.Data().size(), mn = (int)mw.Data().size();
		arc(KEY_GN, gn);
		arc.SerializeMemory(KEY_G, gw.Data().data(), gw.Data().size());
		arc(KEY_MN, mn);
		arc.SerializeMemory(KEY_M, mw.Data().data(), mw.Data().size());
		return;
	}

	if (!g_flow.IsAttached() || !arc.HasKey(KEY_G)) return;
	int gn = 0, mn = 0;
	arc(KEY_GN, gn);
	arc(KEY_MN, mn);
	if (gn <= 0 || mn < 4 || gn > (1 << 24) || mn > (1 << 28)) { Printf(TEXTCOLOR_RED "Realms: the save's Realms state is malformed\n"); return; }
	std::vector<uint8_t> gb((size_t)gn), mb((size_t)mn);
	arc.SerializeMemory(KEY_G, gb.data(), gb.size());
	arc.SerializeMemory(KEY_M, mb.data(), mb.size());

	SaveReader gr(gb.data(), gb.size());
	std::string mapName;
	if (!g_flow.LoadGlobals(gr, nullptr, &mapName)) { Printf(TEXTCOLOR_RED "Realms: the save's Realms state could not be read\n"); return; }
	// Chunk 0xe: the song is loaded and, when it was not already the current
	// one, started (savegame.c:1450-1460).
	if (g_flow.MusicChunk() != 0 && MusicLoadChunk(g_flow.MusicChunk()) == 0) MusicProcess();

	std::map<std::string, MapLogicState> maps;
	SaveReader mr(mb.data(), mb.size());
	const uint32_t n = mr.U32();
	for (uint32_t i = 0; i < n && mr.Ok(); i++)
	{
		const std::string name = mr.Str();
		MapLogicState st;
		if (!st.Load(mr)) break;
		maps[name] = st;
	}
	g_mapStates.swap(maps);                                      // the .TMP files, recreated (savegame.c:1422-1424)
	g_flowNewGamePending = false;
	g_flowMenuPending = false;
	g_flowRestarting = false;
	g_menus.CloseAll();
	if (g_flowPendingLoadSlot >= 0) g_flow.SetLastSlot(g_flowPendingLoadSlot);   // savegame.c:1497
	g_flowPendingLoadSlot = -1;
	if (g.active && g.level != nullptr)
	{
		std::string cur = g.level->MapName.GetChars();
		for (char &c : cur) c = (char)toupper((unsigned char)c);
		auto it = g_mapStates.find(cur);
		if (it != g_mapStates.end()) RestoreMapLogic(it->second);
	}
}

} // namespace roth
