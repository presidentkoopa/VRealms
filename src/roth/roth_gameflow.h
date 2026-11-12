#pragma once
//
// Realms game flow and persistence: new game, map-to-map travel, death, the
// end of the game, cutscenes as far as game state goes, and what a save holds.
//
// Every rule here is read from ROTH.C and cited as file.c:LINE (the
// reconstruction's roth_c/src/engine). Standard library only: no engine
// headers, so tools/gameflow_test builds and runs it against the retail data
// and against dumps taken from the running original.
//
// WHAT THE ORIGINAL DOES, IN ONE PLACE
//
//   boot      roth_main_sequence deletes every *.TMP, loads the databases and
//             the FIRST map of ROTH.RES's `maps {` list with its art pack
//             (game_core.c:1039-1068). game_play_loop then runs action 1 (two
//             movies and the title music), sets health to the maximum, and
//             shows the main menu (game_core.c:755-773, 806-816).
//   new game  menu code 10: clear the inventory cursors and the inventory,
//             run action 3 (a movie, the game music, four gives), latch Adam,
//             health = maximum, armour factor 0, no weapon, mode 1
//             (game_core.c:821-845). The flag bitmap is NOT cleared here: it
//             is zero because boot, and every return to the menu after a
//             death, reloads the databases (game_core.c:444-459).
//   travel    RAW 0x3b latches an 8-byte name, an arrival key and
//             "pending" (raw_commands.c:525-533); the frame's tail consumes
//             it (game_core.c:936-945) through process_map_warp_or_load
//             (map_load.c:574-670). See ClassifyWarp and FindArrivalSector.
//   per map   leaving a map writes <MAP>.TMP: geometry delta, object buffer,
//             command-record bytes, active effects, both entity pools, sound
//             node bits (map_load.c:694-807). Returning reads it back and
//             skips the first-visit init (map_load.c:651-662, 1026-1037).
//   save      SAVE<n>.SAV = the global chunks (player, map name, music,
//             inventory, cursors, flags, movie gallery) plus every *.TMP
//             (savegame.c:1522-1633). Loading deletes the *.TMP files,
//             restores the chunks and warps to the saved map with key 0, so
//             the saved position stands (savegame.c:1357-1501).
//   death     health <= 0 leaves the frame loop: mode 0x20, no weapon, action
//             4, then up to 300 frame-time units of waiting in which Esc
//             ends the wait and scancode 0x39 loads the last slot used; then
//             everything is reset and the main menu returns
//             (game_core.c:948-1020, 438-471).
//   the end   there is no end-of-game routine. The last chain is DBASE100
//             action 614: two movies, a full-screen image, then op 0x26 with
//             a negative operand, which zeroes health AND sets the "no death
//             wait" latch (dbase100.c:900-906). The death path above then
//             runs without its wait and lands on the main menu.
//

#include <stdint.h>
#include <string>
#include <vector>

#include "roth_gamecore.h"

namespace roth
{

struct Map;

//==========================================================================
//
// A byte stream, little-endian, for everything this file serializes.
//
//==========================================================================

class SaveWriter
{
public:
	void U8(uint8_t v) { mBuf.push_back(v); }
	void U16(uint16_t v) { U8((uint8_t)v); U8((uint8_t)(v >> 8)); }
	void U32(uint32_t v) { U16((uint16_t)v); U16((uint16_t)(v >> 16)); }
	void I32(int32_t v) { U32((uint32_t)v); }
	void Bytes(const uint8_t *p, size_t n) { if (n != 0) mBuf.insert(mBuf.end(), p, p + n); }
	void Str(const std::string &s) { U32((uint32_t)s.size()); Bytes((const uint8_t *)s.data(), s.size()); }
	void Blob(const std::vector<uint8_t> &b) { U32((uint32_t)b.size()); Bytes(b.data(), b.size()); }

	// A chunk is { u16 id, u32 size, payload }. Begin returns a mark for End.
	size_t BeginChunk(uint16_t id) { U16(id); U32(0); return mBuf.size(); }
	void EndChunk(size_t mark);

	const std::vector<uint8_t> &Data() const { return mBuf; }
	std::vector<uint8_t> &Data() { return mBuf; }

private:
	std::vector<uint8_t> mBuf;
};

class SaveReader
{
public:
	SaveReader(const uint8_t *p, size_t n) : mP(p), mN(n) {}

	// Every read past the end returns 0 and clears Ok(); nothing is ever
	// read out of bounds.
	uint8_t U8();
	uint16_t U16() { const uint16_t lo = U8(); return (uint16_t)(lo | (U8() << 8)); }
	uint32_t U32() { const uint32_t lo = U16(); return lo | ((uint32_t)U16() << 16); }
	int32_t I32() { return (int32_t)U32(); }
	bool Bytes(uint8_t *dst, size_t n);
	std::string Str();
	std::vector<uint8_t> Blob();

	// Read a chunk header and return a reader over its payload; this reader
	// moves past it. False at the end of the stream or on a short chunk.
	bool NextChunk(uint16_t &id, SaveReader &payload);

	bool Ok() const { return mOk; }
	bool AtEnd() const { return mPos >= mN; }
	size_t Remaining() const { return mPos < mN ? mN - mPos : 0; }

private:
	const uint8_t *mP = nullptr;
	size_t mN = 0, mPos = 0;
	bool mOk = true;
};

//==========================================================================
//
// The player record: save chunk 2, 0x30 bytes
// (write_player_state_chunk / read_player_state_chunk, savegame.c:139-195).
//
//==========================================================================

struct PlayerRecord
{
	static const int BYTES = 0x30;

	int32_t x = 0;              // +0x00  16.16
	int32_t z = 0;              // +0x04  16.16, height
	int32_t y = 0;              // +0x08  16.16
	uint16_t angle = 0;         // +0x0c  512 per turn
	uint16_t sector = 0;        // +0x0e  byte offset of the sector record in the geometry block
	uint32_t secondary = 0;     // +0x10  the WEAPON hand: slot * 4, 0 = none (savegame.c:152-154)
	uint32_t primary = 0;       // +0x14  the selected item: slot * 4, 0 = none (savegame.c:155-157)
	int32_t health = 0;         // +0x18
	int32_t viewPitch = 0;      // +0x1c
	int32_t viewPitchApplied = 0; // +0x20
	int32_t valueReduction = 0; // +0x24  g_value_reduction_factor, DBASE100 op 0x37
	uint16_t eyeHeight = 0;     // +0x28  written, NOT read back (savegame.c:168-195)
	uint32_t dword8c114 = 0;    // +0x2c  written, NOT read back

	void Encode(uint8_t out[BYTES]) const;
	bool Decode(const uint8_t *in, size_t size);
};

//==========================================================================
//
// The start of the game: the first entry of ROTH.RES's `maps {` list.
//
// roth_main_sequence loads g_cfg_file_arg / g_cfg_das_arg, which
// load_map_list filled by selecting entry 0 (map_load.c:1050-1059), and keeps
// a copy as "the default map" for every later new game
// (game_core.c:1062-1063, 447-448). The retail list starts `m\study1 m\demo`.
//
//==========================================================================

struct MapListEntry
{
	std::string map;    // base name, upper case, no directory and no extension: "STUDY1"
	std::string pack;   // the art pack's base name: "DEMO"
};

// Every entry of the `maps {` block, in file order. Empty when there is none.
std::vector<MapListEntry> ParseMapList(const uint8_t *res, size_t size);

//==========================================================================
//
// RAW 0x3b: what a latched transition does.
//
//==========================================================================

struct WarpRequest
{
	char name[9] = {};      // g_warp_dest_a/b: the 8 record bytes at +0x0a, NUL-terminated here
	uint16_t key = 0;       // g_map_first_load_flag: the record's word at +0x08
};

enum class WarpKind : uint8_t
{
	// name[0] + key == 0: the frame tail does not call the warp at all
	// (game_core.c:938-939).
	Nothing,
	// Same map (or an empty name) and key != 0: move the player to the
	// arrival sector, no reload (map_load.c:612-623).
	RelocateInPlace,
	// Same map and key == 0: the latch is cleared and nothing moves
	// (map_load.c:614-622).
	SameMapNoMove,
	// A different map: write this map's state, load the other, restore or
	// initialise its state, then relocate if key != 0 (map_load.c:625-663).
	ChangeMap,
};

// The original's name compare (map_load.c:596-610): up to 8 characters,
// case-folded by & 0xdf, against the loaded map's base name, which ends at
// its '.'. An empty request compares equal. `currentBase` is the loaded
// map's base name ("STUDY1"), with or without ".RAW".
bool WarpNamesMatch(const char *requested, const char *currentBase);
WarpKind ClassifyWarp(const WarpRequest &req, const char *currentBase);
// The name the original would try to load: the request up to its first byte
// <= 0x20, at most 8 characters (map_load.c:632-641), upper-cased.
std::string WarpTargetName(const WarpRequest &req);

struct Arrival
{
	int sector = -1;        // index into Map::sectors
	int x = 0, y = 0;       // the centre of the sector's vertex bounding box
	int floor = 0;          // that sector's floor height
};

// relocate's sector search (ml_relocate_core, map_load.c:543-557): the FIRST
// sector, in file order, whose word at +0x14 equals `key`, skipping
// `skipSector` (the player's own sector in the same-map case, -1 otherwise).
// Then ml_relocate_found's centre (map_load.c:516-539): min/max over the
// sector's faces' first vertices, (min + max) >> 1 on each axis.
bool FindArrivalSector(const Map &map, uint16_t key, int skipSector, Arrival &out);

// The height rule (map_load.c:518-520, 615-617, 627-631): the player keeps
// its height ABOVE THE FLOOR. `oldFloor` is the floor of the sector the
// player stood in when the warp ran.
inline int ArrivalHeight(int oldZ, int oldFloor, int newFloor) { return oldZ - (oldFloor - newFloor); }

//==========================================================================
//
// One map's logic state, as this port holds it.
//
// The original keeps a map's state in the map's own bytes and saves those
// (map_load.c:694-807). This port keeps the world in engine structures --
// sector planes, textures and light, door polyobjects, prop actors -- which
// the engine's own level snapshot already carries. What the engine does NOT
// know about is the command records and the queues that run them, and that
// is this struct: the port's counterpart of the .TMP "command-record patch",
// "active effects" and delay-list chunks.
//
// What the original stores per command record is a handful of bytes by type
// plus the state byte +2 (map_load.c:752-783). This stores every byte from
// +6 on plus the state byte, which is a superset: nothing a handler can
// write to its own record is lost.
//
//==========================================================================

struct SavedCommand
{
	uint8_t modifier = 0;           // record +0x02
	uint8_t disabled = 0;
	std::vector<uint16_t> args;     // record +0x06 onward
};

struct SavedDelay
{
	int32_t remaining = 0;
	uint16_t record = 0;
	uint32_t context = 0;
};

struct SavedDeferred
{
	uint16_t chain = 0;
	uint32_t context = 0;
};

struct SavedEffect
{
	uint8_t tick = 0;
	uint16_t record = 0;
	uint16_t payload = 0;
	std::vector<int32_t> sectors;
	uint8_t finished = 0;
	std::vector<uint8_t> baseLight;
	uint16_t hold = 0;
	uint8_t holding = 0;
	uint8_t flags5 = 0;
	int32_t sfxNode = -1;
	uint8_t sfxStarted = 0;
	std::vector<int32_t> objects;   // (sector, index) pairs, flattened
	int32_t moveVerts[4] = { -1, -1, -1, -1 };
};

struct MapLogicState
{
	std::string mapName;
	std::vector<SavedCommand> commands;
	std::vector<SavedDelay> delays;
	std::vector<SavedDeferred> deferred;
	std::vector<SavedEffect> effects;
	uint16_t pendingRecord = 0;
	int32_t activeFace = -1, activeSector = -1;
	int32_t activeObjectSector = -1, activeObjectIndex = -1;
	std::vector<int32_t> movers;                // (sector, dx, dy) triples
	std::vector<int32_t> lastSectorByPlayer;    // (player, sector) pairs
	uint8_t dialogueLatch = 0;

	void Save(SaveWriter &w) const;
	bool Load(SaveReader &r);
	bool operator==(const MapLogicState &o) const;
};

//==========================================================================
//
// An original SAVE<n>.SAV, read. Chunks are { u16 id, u16 size } + payload
// (savegame.c:1380-1384).
//
// THE FORMAT DECISION. Saves are the ENGINE's savegames, with the Realms state
// beside the level (roth::SerializeRealmsGlobals), not the original's files:
// the original's per-map half is a byte delta of the raw map image plus its
// object buffer, entity pools and effect list (map_load.c:694-807), and this
// port does not run on that image -- its world is engine sectors, polyobjects
// and actors, which the engine's own serializer already saves completely.
// Writing a .TMP would mean keeping a second, shadow copy of the world in the
// original's layout in step with the engine's, which is two authorities. The
// GLOBAL half is different: it is plain data in both, so it keeps the
// original's chunk ids and payloads, and this reader imports an original
// save's global half exactly (tools/gameflow_test --sav). Its .TMP chunks are
// kept as bytes and measured, not applied.
//
//==========================================================================

struct OriginalSave
{
	bool ok = false;                    // the end marker (id 0xc) was reached
	bool hasPlayer = false;
	PlayerRecord player;                // id 2
	std::string mapName;                // id 3, the base name, upper-cased
	uint32_t musicChunk = 0;            // id 0xe
	std::vector<uint8_t> inventory;     // id 7, 0x400 bytes
	std::vector<uint8_t> cursorPos;     // id 4, 0x14 bytes
	std::vector<uint8_t> cursorScroll;  // id 5, 0x14 bytes
	std::vector<uint8_t> flags;         // id 6
	std::vector<uint8_t> gallery;       // id 0xd, one byte per cutscene entry
	std::string slotName;               // id 0xa, 0x30 bytes
	bool hasThumbnail = false;          // id 0xb
	struct MapState { std::string file; std::vector<uint8_t> bytes; };
	std::vector<MapState> maps;         // id 8 (name) + id 9 (the .TMP bytes) pairs
	std::vector<uint16_t> order;        // every chunk id, in file order
};

OriginalSave ParseOriginalSave(const uint8_t *data, size_t size);

// The sections of one .TMP stream (write_raw_state_stream, map_load.c:694-807),
// measured without applying them. `ok` needs the "EXIT" tag where it belongs.
struct OriginalMapState
{
	bool ok = false;
	bool geometryIsDelta = false;   // header word 0xffff
	uint32_t geometryBytes = 0;     // the encoded delta, or the literal block
	uint32_t geometryChanged = 0;   // delta only: literal bytes it carries
	uint32_t objectBytes = 0;
	uint32_t commandPatchBytes = 0;
	uint32_t effectBytes = 0;
	uint32_t entityBytes = 0;
	uint32_t soundNodeBytes = 0;
};
OriginalMapState MeasureOriginalMapState(const uint8_t *data, size_t size);

//==========================================================================
//
// The flow itself.
//
//==========================================================================

// What the engine does for the flow. Everything defaults to nothing, which
// is what the host tests use.
class FlowHost
{
public:
	virtual ~FlowHost() {}
	// A movie plays here: `name` is the GDV file stem, `subtitles` the
	// DBASE400 offset and size of its subtitle stream (0 when the original
	// would not load one). The original call is blocking (gdv_cutscene.c:2840)
	// and the state changes around it do not depend on whether it was
	// skipped; the engine queues it and plays it as a blocking cutscene from
	// the end of the tic (roth_movie.h).
	virtual void PlayMovie(const char *name, uint32_t subtitleOffset, int32_t subtitleSize)
	{ (void)name; (void)subtitleOffset; (void)subtitleSize; }
	// exit_cutscene_overlay_mode's presentation half (gdv_cutscene.c:2434-2486).
	virtual void MovieOverlayEnded() {}
	// A full-screen still (DBASE100 op 0x0e): `id` is its DBASE200 offset.
	// Returns when the player dismisses it (menu_hud_ui.c:1220-1229).
	virtual void ShowImage(uint32_t id) { (void)id; }
};

class GameFlow
{
public:
	// g_player_movement_enabled, 0x7674a (game_core.c:861-921).
	enum Mode : uint8_t
	{
		MODE_OFF = 0,
		MODE_PLAY = 1,
		MODE_PANEL = 3,
		MODE_DIALOGUE = 4,
		MODE_DIALOGUE_CLICK = 5,
		MODE_BETWEEN = 8,
		MODE_DEAD = 0x20,
	};

	static const int32_t DEFAULT_MAX_HEALTH = 0x800;   // game_core.c:767, 828
	static const int DEATH_WAIT_UNITS = 0x12c;         // game_core.c:1002
	static const int CURSOR_BYTES = 0x14;              // save chunks 4 and 5

	// `db` is the same database the core reads; the flow writes each movie's
	// gallery byte into it, as the original writes its own image
	// (gdv_cutscene.c:2772-2775).
	void Attach(GameCore *core, Dbase100 *db, FlowHost *host = nullptr);
	bool IsAttached() const { return mCore != nullptr && mDb != nullptr; }

	// ---- boot and new game ---------------------------------------------------
	// init_game_databases' resets (game_core.c:394, 420-432) and then the head
	// of game_play_loop: action 1, health = maximum, armour factor 0
	// (game_core.c:755-773).
	void Boot();
	// The new-game block (game_core.c:821-845).
	void NewGame();
	// reset_and_start_new_game, as far as this state goes (game_core.c:438-471):
	// fresh databases -- flags, gallery and the "examined" bits zero -- and no
	// per-map state. The caller reloads the default map.
	void ResetForMenu();
	bool BootDone() const { return mBooted; }

	// ---- health ---------------------------------------------------------------
	int32_t Health() const { return mHealth; }
	int32_t MaxHealth() const { return mMaxHealth; }
	void SetHealth(int32_t v) { mHealth = v; }
	// Zero means "use the default", as the original treats an unset cap.
	void SetMaxHealth(int32_t v) { mMaxHealth = v != 0 ? v : DEFAULT_MAX_HEALTH; }
	// advance_clamp_8a0f0 (renderer.c:9818-9824): add, then clamp to `max`
	// with an UNSIGNED compare.
	void AdvanceHealth(uint32_t amount, uint32_t max);
	// DBASE100 op 0x26 with an operand <= 0 (dbase100.c:904): health = 0 and
	// the latch that skips the death wait.
	void ZeroHealthAndFlagDeath() { mHealth = 0; mNoDeathWait = true; }
	bool NoDeathWait() const { return mNoDeathWait; }

	// ---- mode -----------------------------------------------------------------
	uint8_t CurrentMode() const { return mMode; }
	void SetMode(uint8_t m) { mMode = m; }

	// ---- movies ---------------------------------------------------------------
	// play_record_gdv_cutscene (gdv_cutscene.c:2753-2848), minus the picture.
	// Returns what the original returns: 0 for no entry or an unnamed one,
	// else 1.
	uint32_t PlayRecordGdvCutscene(int index);
	// show_fullscreen_image (menu_hud_ui.c:1175-1232): a still picture that
	// waits for Esc or a click. It saves and sets the mode exactly as a movie
	// does, but does NOT raise the overlay latch -- so an image with no movie
	// before it leaves mode 4 standing. Returns 0 for id 0, else 1.
	uint32_t ShowFullscreenImage(uint32_t id);
	// exit_cutscene_overlay_mode (gdv_cutscene.c:2434-2486): only when a
	// movie left the overlay up; restores the mode saved before it.
	void ExitCutsceneOverlayMode();
	int32_t CutsceneOverlayActive() const { return mOverlayActive ? 1 : 0; }
	uint32_t CutscenesSeenCount() const { return mSeenCount; }
	// The names of the movies asked for since the last ClearMovieLog, in order.
	const std::vector<std::string> &MovieLog() const { return mMovieLog; }
	void ClearMovieLog() { mMovieLog.clear(); }

	// ---- music ----------------------------------------------------------------
	// g_current_dbase300_chunk_id (0x7f46c): the DBASE300 offset of the song
	// playing. Save chunk 0xe; loading a save restarts it (savegame.c:1450-1460).
	uint32_t MusicChunk() const { return mMusicChunk; }
	void SetMusicChunk(uint32_t id) { mMusicChunk = id; }

	// ---- inventory tab cursors (0x80afc, 0x80b10) ------------------------------
	uint8_t *CursorPos() { return mCursorPos; }
	uint8_t *CursorScroll() { return mCursorScroll; }
	void ClearCursors();    // clear_dual_array_80afc, game_core.c:822

	// ---- death ----------------------------------------------------------------
	enum class DeathStep : uint8_t
	{
		Waiting,        // keep calling TickDeath
		QuickLoad,      // load LastSlot(); if that fails call DeathLoadFailed()
		Restart,        // reset everything and show the main menu
	};
	// The block the frame loop runs once health <= 0 (game_core.c:959-976):
	// mode 0x20, no weapon, action 4. Returns Restart at once when the
	// no-wait latch is set (game_core.c:977), else Waiting.
	DeathStep BeginDeath();
	// One pass of the death wait (game_core.c:978-1003). `frameScale` is
	// g_frame_time_scale for this pass; `scancode` is the key dequeued this
	// pass, 0 for none: 1 (Esc) ends the wait, 0x39 asks for the last slot.
	DeathStep TickDeath(int frameScale, uint8_t scancode);
	// A quick-load from the death wait that failed, or had no slot to load:
	// the wait ends and everything restarts (game_core.c:987-1000).
	DeathStep DeathLoadFailed() { mDeathUnits = DEATH_WAIT_UNITS; return DeathStep::Restart; }
	bool Dying() const { return mMode == MODE_DEAD; }
	int DeathUnits() const { return mDeathUnits; }

	// g@0x7138c, "the last slot used": set by every save and load
	// (savegame.c:1497, 1631); -1 from program start (obj3_owned.c).
	int32_t LastSlot() const { return mLastSlot; }
	void SetLastSlot(int32_t s) { mLastSlot = s; }

	// ---- save ------------------------------------------------------------------
	// The global chunks, under the original's ids, in the original's order
	// (savegame.c:1578-1627). `player` and `mapName` come from the caller: the
	// engine owns the pawn and the level. Position fields are for the record
	// and for export; the engine restores its own pawn.
	void SaveGlobals(SaveWriter &w, const PlayerRecord &player, const std::string &mapName) const;
	// Restore them (savegame.c:1385-1462 and the tail at 1474-1498). False on
	// a malformed stream; the state is then unchanged.
	bool LoadGlobals(SaveReader &r, PlayerRecord *playerOut = nullptr, std::string *mapNameOut = nullptr);
	// The same restore from an original .SAV's global chunks.
	bool ImportOriginal(const OriginalSave &sav);

	// The player record's fields the flow owns, filled in; the caller adds
	// position, angle and sector.
	void FillPlayerRecord(PlayerRecord &p) const;

private:
	void StampGallery(int index);

	GameCore *mCore = nullptr;
	Dbase100 *mDb = nullptr;
	FlowHost *mHost = nullptr;
	FlowHost mNullHost;

	bool mBooted = false;
	uint8_t mMode = MODE_OFF;
	uint8_t mSavedMode = MODE_OFF;          // g_saved_movement_enabled, 0x83d24
	bool mOverlayActive = false;            // g_cutscene_overlay_active, 0x83c70
	bool mScreenHeld = false;               // 0x83d1c: a movie run is in progress
	uint32_t mSeenCount = 1;                // g_cutscenes_seen_count, 0x82006
	uint32_t mMusicChunk = 0;
	int32_t mHealth = DEFAULT_MAX_HEALTH;
	int32_t mMaxHealth = DEFAULT_MAX_HEALTH;
	bool mNoDeathWait = false;              // 0x7feb4
	int mDeathUnits = 0;
	int32_t mLastSlot = -1;
	uint8_t mCursorPos[CURSOR_BYTES] = {};
	uint8_t mCursorScroll[CURSOR_BYTES] = {};
	std::vector<std::string> mMovieLog;
};

//==========================================================================
//
// The interpreter calls that are the flow's to answer. A host sink derives
// from this instead of Dbase100Sink and keeps its own overrides for
// everything else; with no flow set every call behaves as the bare sink.
//
//==========================================================================

class FlowInterpSink : public Dbase100Sink
{
public:
	GameFlow *flow = nullptr;

	// Op 0x07 (dbase100.c:939).
	uint32_t PlayRecordGdvCutscene(int index) override
	{ return flow != nullptr ? flow->PlayRecordGdvCutscene(index) : 0; }
	// Op 0x0e (dbase100.c:910-919).
	uint32_t ShowFullscreenImage(uint32_t operand) override
	{ return flow != nullptr ? flow->ShowFullscreenImage(operand) : 0; }
	// finish_dialogue_record_eval (dbase100.c:293-294).
	int32_t CutsceneOverlayActive() override { return flow != nullptr ? flow->CutsceneOverlayActive() : 0; }
	void ExitCutsceneOverlayMode() override { if (flow != nullptr) flow->ExitCutsceneOverlayMode(); }
	// Op 0x26 (dbase100.c:900-906).
	void AdvanceClamp8a0f0(uint32_t amount, uint32_t max) override { if (flow != nullptr) flow->AdvanceHealth(amount, max); }
	void ZeroHealthAndFlagDeath() override { if (flow != nullptr) flow->ZeroHealthAndFlagDeath(); }
	uint32_t HealthClampMax() override { return flow != nullptr ? (uint32_t)flow->MaxHealth() : 0; }
	// Ops 0x1a / 0x1b (dbase100.c:775-779, 794-798): the song is named by its
	// DBASE300 offset, and that offset is what a save keeps
	// (savegame.c:1586-1587). load_dbase300_chunk answers 1 for the song
	// already loaded and otherwise makes it the current one
	// (file_config.c:598-599); 0 tells the interpreter to start it.
	uint32_t LoadDbase300Chunk(uint32_t index) override
	{
		if (flow == nullptr) return 1;
		if (index == flow->MusicChunk()) return 1;
		flow->SetMusicChunk(index);
		return 0;
	}
	uint32_t CurrentDbase300ChunkId() override { return flow != nullptr ? flow->MusicChunk() : 0; }
};

} // namespace roth
