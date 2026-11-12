#pragma once
//
// The DBASE100 command interpreter: Realms of the Haunting's scripting brain.
//
// Engine-free, like the reader it sits on (roth_dbase100.h): nothing but the
// C++ standard library, built and tested on its own (tools/dbase100_test,
// interp_test.cpp).
//
// WHERE THE RULES COME FROM. Every rule here is read out of ROTH.C,
// roth_c/src/engine/dbase100.c ("dbase100.c:LINE" below), plus the three flag
// leaves it calls: test_dbase100_record_flag (raw_commands.c:79-86) and
// dbase100_bitmap_test_set / _clear (renderer.c:11528-11554). The logic is
// meant to be 1:1: what fires when, the flags, the branching, the return
// values and the order of side effects. Anything ROTH.C does not settle is
// marked [unverified] where it is used.
//
// WHAT IS PORTED
//   execute_dbase100_chain                       dbase100.c:607-953
//   finalize_dbase100_chain                      dbase100.c:305-320
//   filter_dbase100_active_records               dbase100.c:41-100
//   eval_or_queue_dialogue_record_commands       dbase100.c:427-447
//   run_dialogue_action_queue                    dbase100.c:452-480
//   advance_dialogue_action_queue                dbase100.c:484-497
//   execute_dialogue_branch                      dbase100.c:502-528
//   close_dialogue_and_run_branch                dbase100.c:532-540
//   finish_dialogue_record_eval                  dbase100.c:291-296
//   eval_dialogue_record_by_id                   dbase100.c:545-555
//   eval_dialogue_record_condition_with_cleanup  dbase100.c:569-576
//   the flag bitmap and its three leaves
//
// WHAT IS NOT. read_next_dialogue_line (dbase100.c:371-403) reads DBASE400,
// caches timed messages and primes the voice clip: all of it text/voice
// work, so it is a sink call. build_entity_def_record (dbase100.c:117-181)
// is the monster-definition reader, not the interpreter.
//
// POINTERS BECOME IMAGE OFFSETS. ROTH.C walks a raw pointer into the loaded
// file. Here a "chain" is a byte offset into Dbase100's image, and every read
// goes through one checked accessor. ROTH.C can read past a record (op 0x0d
// peeks at the next word's top byte, dbase100.c:656; the main loop is
// do-while, so a count of 0 or -1 still reads one word, dbase100.c:625-637).
// Those reads are reproduced exactly as long as they stay inside the image.
// A read outside the image is where ROTH.C would read the heap; here it
// yields 0 and is counted (OutOfImageReads, and by name in Anomalies).
//
// NOTHING FAILS SILENTLY. Every command word that reaches the dispatch and
// has no handler is counted by opcode in Unhandled(). Every place this port
// refuses to follow ROTH.C off the end of a buffer is counted by a fixed
// name in Anomalies().
//

#include "roth_dbase100.h"

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

namespace roth
{

//==========================================================================
//
// The effects that belong to OTHER systems. One method per ROTH.C callee,
// named after it; the comment gives the canon address ROTH.C bridges to
// (dbase100.c:206-253) and the call site. Every default does nothing and
// returns the value that means "nothing happened".
//
// The second group is not callees but GLOBALS OF OTHER SYSTEMS the
// interpreter reads or pokes directly in ROTH.C. They are sink calls here so
// the owner of each stays the single authority for it.
//
// A sink method may call back into the interpreter (ROTH.C's
// dialogue_voice_force_end tail-calls close_dialogue_and_run_branch,
// audio.c:1343-1347). The interpreter is written to allow that.
//
//==========================================================================

class Dbase100Sink
{
public:
	virtual ~Dbase100Sink() {}

	// ---- inventory -------------------------------------------------------
	// give_item 0x1cedc (EAX = item id as s16 sign-extended, EDX = ctx, always
	// 0 from here). Op 0x11, dbase100.c:770. The result is ignored.
	virtual uint32_t GiveItem(int32_t id, uint32_t ctx) { (void)id; (void)ctx; return 0; }
	// remove_item 0x1d077 (EAX = id as s16). Op 0x11 If-NOT, dbase100.c:771.
	virtual uint32_t RemoveItem(int32_t id) { (void)id; return 0; }
	// query_player_inventory 0x1ccf7 (EAX = id as s16, EDX = flags, always 0
	// from here) -> count. Ops 0x02 / 0x03, dbase100.c:813, 823.
	virtual uint32_t QueryPlayerInventory(int32_t id, uint32_t flags) { (void)id; (void)flags; return 0; }

	// ---- text, voice, dialogue windows ------------------------------------
	// read_next_dialogue_line 0x1e8cc (dbase100.c:371-403): read the DBASE400
	// entry at `voiceOffset` into `dest` (at most `maxlen` bytes) and return
	// the byte count; 0 means no text. flag 1 = prime its voice clip (op
	// 0x05, dbase100.c:861); flag 0 = text only, timed-message cache (the
	// choice builder, dbase100.c:698).
	virtual uint32_t ReadNextDialogueLine(uint8_t *dest, uint32_t maxlen, uint32_t voiceOffset, uint32_t flag)
	{ (void)dest; (void)maxlen; (void)voiceOffset; (void)flag; return 0; }
	// dbase100_open_dialogue_window 0x1eabc: the choice menu. `text` is every
	// visible choice, each ending '\n', NUL-terminated (dbase100.c:704-706,
	// 725). 0 = not opened. `bytes` (the length before the NUL) is this
	// port's addition; ROTH.C passes the pointer alone.
	virtual uint32_t Dbase100OpenDialogueWindow(const uint8_t *text, uint32_t bytes) { (void)text; (void)bytes; return 0; }
	// dbase100_open_dialogue_window_alt 0x1ebb4: a spoken line (op 0x05,
	// dbase100.c:862). 0 = not opened. `bytes` is what ReadNextDialogueLine
	// returned; ROTH.C passes the pointer alone.
	virtual uint32_t Dbase100OpenDialogueWindowAlt(const uint8_t *text, uint32_t bytes) { (void)text; (void)bytes; return 0; }
	// dialogue_voice_force_end 0x1f671 (audio.c:1335-1350), called with (0, 0)
	// from run_dialogue_action_queue, dbase100.c:456, 473.
	virtual void DialogueVoiceForceEnd(uint32_t eax, uint32_t edx) { (void)eax; (void)edx; }

	// ---- cutscenes and images ----------------------------------------------
	// play_record_gdv_cutscene 0x20c16 (EAX = the cutscene entry). Op 0x07,
	// dbase100.c:939. ROTH.C passes the entry's address; here its 0-based
	// index, which is Dbase100::Cutscene(index). The result is ignored.
	virtual uint32_t PlayRecordGdvCutscene(int index) { (void)index; return 0; }
	// show_fullscreen_image 0x20f81 (EAX = the 24-bit operand). Op 0x0e,
	// dbase100.c:918.
	virtual uint32_t ShowFullscreenImage(uint32_t operand) { (void)operand; return 0; }
	// exit_cutscene_overlay_mode 0x20905. finish_dialogue_record_eval,
	// dbase100.c:294.
	virtual void ExitCutsceneOverlayMode() {}

	// ---- sound and music ----------------------------------------------------
	// play_sound_effect 0x27270 (EAX = id, EDX = param). Op 0x19 calls it
	// with (operand - 1, 0xfcfe), dbase100.c:849.
	virtual uint32_t PlaySoundEffect(uint32_t id, uint32_t param) { (void)id; (void)param; return 0; }
	// load_dbase300_chunk 0x15492 (file_config.c:596): 0 = freshly loaded,
	// 1 = already loaded or failed. Op 0x1a (dbase100.c:798) and
	// finalize_dbase100_chain (dbase100.c:308). THE DEFAULT RETURNS 1, so a
	// sink with no music never triggers the sequence calls that follow a
	// fresh load.
	virtual uint32_t LoadDbase300Chunk(uint32_t index) { (void)index; return 1; }
	// finalize_audio_sequence_ref 0x15689. dbase100.c:312, 795.
	virtual uint32_t FinalizeAudioSequenceRef() { return 0; }
	// emit_music_sequence_event 0x1555f (EAX = event byte). dbase100.c:313.
	virtual uint32_t EmitMusicSequenceEvent(uint32_t event) { (void)event; return 0; }
	// process_audio_sequence_chunk 0x1558d. dbase100.c:315, 799.
	virtual void ProcessAudioSequenceChunk() {}

	// ---- RAW map commands ---------------------------------------------------
	// reset_command_chain_no_source 0x305a1 (AX = command index). Op 0x23,
	// dbase100.c:837.
	virtual int32_t ResetCommandChainNoSource(uint16_t index) { (void)index; return 0; }
	// run_object_commands_by_id 0x30549 (AX = object id). Op 0x36,
	// dbase100.c:843.
	virtual int32_t RunObjectCommandsById(uint16_t id) { (void)id; return 0; }

	// ---- player ---------------------------------------------------------------
	// advance_clamp_8a0f0 0x320a7 (renderer.c:9818): health += amount, clamped
	// to `max`. Op 0x26 with a positive operand, dbase100.c:905.
	virtual void AdvanceClamp8a0f0(uint32_t amount, uint32_t max) { (void)amount; (void)max; }

	// ---- the RNG ----------------------------------------------------------------
	// rng_next_index_for_count 0x1c9a0 (renderer.c:11130): a value in
	// [0, count). Op 0x0b, dbase100.c:753. The default picks index 0.
	virtual int32_t RngNextIndexForCount(int32_t count) { (void)count; return 0; }

	// ===== globals of other systems =====

	// g_audio_seq_state 0x83c4c (VA_g_dialogue_busy_flag + 0x162): 2 = the
	// music is faded out. Read at dbase100.c:310, 794; written 0 at :311, 796.
	virtual int32_t AudioSeqState() { return 0; }
	virtual void SetAudioSeqState(int32_t v) { (void)v; }
	// g_current_dbase300_chunk_id 0x7f46c. Op 0x1b copies it, dbase100.c:778.
	virtual uint32_t CurrentDbase300ChunkId() { return 0; }
	// The byte at 0x71124 (VA_g_font_descriptor + 0x212): the music master
	// volume (audio.c:1752-1755), passed to EmitMusicSequenceEvent,
	// dbase100.c:313.
	virtual uint8_t MusicMasterVolume() { return 0; }
	// g_cutscene_overlay_active 0x83c70. dbase100.c:293.
	virtual int32_t CutsceneOverlayActive() { return 0; }
	// g_inventory_panel_open 0x7fec4 and g_inspect_popup_active 0x7fec0.
	// dbase100.c:894, 914, 929.
	virtual int32_t InventoryPanelOpen() { return 0; }
	virtual int32_t InspectPopupActive() { return 0; }
	// "0x7fec4 = 2; 0x7fecc = 1" (VA_g_inventory_panel_open and + 0x8): ops
	// 0x0e / 0x07 ask the open inspect popup to close, dbase100.c:914, 929.
	// The name is this port's; ROTH.C has only the two stores.
	virtual void ArmInventoryPanelExit() {}
	// "0x83b20 = 1" (VA_g_dialogue_busy_flag + 0x36). Op 0x2d sub-code 4
	// (dbase100.c:892) and op 0x07 after the movie (dbase100.c:941).
	virtual void SetLatch83b20() {}
	// "0x80b2c = 1" (VA_g_ui_panel_anchor_y + 0x4). Op 0x2d sub-code 7,
	// dbase100.c:895.
	virtual void SetLatch80b2c() {}
	// "g_player_health 0x8a0f0 = 0; 0x7feb4 = 1" (VA_g_pending_game_action
	// + 0xc). Op 0x26 with operand <= 0, dbase100.c:904. The name is this
	// port's.
	virtual void ZeroHealthAndFlagDeath() {}
	// The dword at 0x7fe44 (VA_g_help_overlay_enabled + 0xc): the health cap
	// handed to AdvanceClamp8a0f0, dbase100.c:905.
	virtual uint32_t HealthClampMax() { return 0; }
};

//==========================================================================
//
// The interpreter's state: ROTH.C's globals, under ROTH.C's names
// (g_names.h). Where g_names.h only has "neighbour + offset", the field
// carries the name dbase100.c's own comments use and the expression is
// quoted.
//
// "Pointers" (queue chain, choice cursor) are image offsets; 0 = none, as in
// ROTH.C (no command lives at offset 0: that is the file signature).
//
//==========================================================================

struct Dbase100QueueEntry
{
	uint32_t chain = 0;     // +0  the record cursor
	int32_t count = 0;      // +4  records left
	uint32_t flags = 0;     // +8  the caller flags
};

struct Dbase100InterpState
{
	static const int QUEUE_SLOTS = 8;           // 0x60 bytes / 0xc (g_names.h:292; dbase100.c:439)
	static const int CHOICE_INDEX_SLOTS = 16;   // 0x40 bytes before the draining flag
	static const int CHOICE_INDEX_EXTENT = 19;  // the 0x4c-byte extent of the global (g_names.h:298)
	static const int QUEUE_DRAINING = 16;       // index of "+ 0x40"

	// g_dbase100_record_bitmap 0x81e28; its size is 0x81e2c (game_core.c:420-427).
	// Empty = ROTH.C's "no bitmap" (pointer 0).
	std::vector<uint8_t> g_dbase100_record_bitmap;

	int32_t g_value_reduction_factor = 0;           // 0x81e30 (op 0x37)

	// 0x81e18, "VA_g_entity_def_cache_count + 0x4"; dbase100.c:286 names it
	// g_dbase100_queue_active. RAW 0x2b sets it (raw_commands.c:4702-4703).
	int32_t g_dbase100_queue_active = 0;

	int32_t g_dialogue_action_queue_count = 0;      // 0x81e3e
	Dbase100QueueEntry g_dialogue_action_queue[QUEUE_SLOTS];   // 0x81e42

	uint32_t g_dbase100_choice_cursor = 0;          // 0x81ea2
	int32_t g_dbase100_choice_remaining = 0;        // 0x81ea6
	uint32_t g_dbase100_choice_mode = 0;            // 0x81eaa (the caller flags)
	int32_t g_dbase100_choice_count = 0;            // 0x81eae
	uint32_t g_dbase100_pending_topic = 0;          // 0x81eb2

	// 0x81eb6, 0x4c bytes. [0..15] map a visible choice to its sub-record
	// index. [16] is "VA_g_dbase100_choice_record_indices + 0x40": set while
	// run_dialogue_action_queue drains (dbase100.c:454, 479) and read by op
	// 0x05 (dbase100.c:855). ROTH.C's choice builder writes this table with
	// no bound, so a 17th visible choice lands ON the draining flag; keeping
	// one array keeps that aliasing. [17..18] are the rest of the extent.
	int32_t g_dbase100_choice_record_indices[CHOICE_INDEX_EXTENT] = {};

	// Dialogue-flow globals owned by the dialogue UI / voice code in ROTH.C
	// but read AND written by the interpreter for its own control flow. The
	// engine side writes them too (the window openers, voice_stream_pump).
	int32_t g_dialogue_busy_flag = 0;               // 0x83aea
	int32_t g_active_dialogue_context = 0;          // 0x83115
	int32_t g_move_freeze_gate = 0;                 // 0x83125
	int32_t g_voice_stream_state = 0;               // 0x8200c
	// 0x71370. Starts at -1, "no row picked": the EXE's initialised data
	// (obj3_owned.c:488-489), and what the running original holds after a new
	// game (tools/gameproof, S0). file_config.c:1075 tests it against -1.
	int32_t g_choice_selected_index = -1;
	// 0x8313d, "VA_g_dialogue_reveal_ramp + 0x14"; dialogue_ui.c:224 calls
	// it g_choice_saved_index.
	int32_t g_choice_saved_index = 0;
};

//==========================================================================
//
// Names for reports. Null when ROTH.C's interpreter has no handler.
//
//==========================================================================

// The handler's name for an opcode execute_dbase100_chain dispatches
// (dbase100.c:643-652), else null.
const char *Dbase100OpcodeName(uint8_t opcode);
// True for the two opcodes the interpreter skips but other dbase100.c code
// consumes: 0x09 (branch marker, execute_dialogue_branch, dbase100.c:517)
// and 0x0c (random-list end, filter_dbase100_active_records, dbase100.c:95).
bool IsDbase100MarkerOpcode(uint8_t opcode);

//==========================================================================
//
// The interpreter.
//
//==========================================================================

class Dbase100Interp
{
public:
	// One segment found by FilterDbase100ActiveRecords: ROTH.C's 8-byte
	// {ptr, count} output entry.
	struct Segment
	{
		uint32_t chain = 0;
		int32_t count = 0;
	};

	// Bind to a loaded database. Resets all state and allocates the flag
	// bitmap as init_game_databases does (game_core.c:420-432): header +0x0C
	// through Dbase100::FlagBitmapBytes, zeroed. `sink` may be null (a
	// do-nothing sink is used). Neither is owned.
	void Attach(const Dbase100 *db, Dbase100Sink *sink);
	void SetSink(Dbase100Sink *sink);
	bool IsAttached() const { return mDb != nullptr; }

	// Everything except the flag bitmap and g_value_reduction_factor back to
	// zero. NOT a ROTH.C routine: a convenience for tests and tools.
	void ResetDialogueState();

	Dbase100InterpState &State() { return mState; }
	const Dbase100InterpState &State() const { return mState; }

	// ---- the flag bitmap ----------------------------------------------------
	// test_dbase100_record_flag (raw_commands.c:79-86): -1 when the bit is
	// set OR there is no bitmap, 0 when clear.
	int32_t TestDbase100RecordFlag(uint32_t idx);
	// dbase100_bitmap_test_set (renderer.c:11528-11539): 0 when already set,
	// else set it and return -1. No bitmap: -1.
	uint32_t Dbase100BitmapTestSet(uint32_t idx);
	// dbase100_bitmap_test_clear (renderer.c:11543-11554): 0 when already
	// clear, else clear it and return -1. No bitmap: -1.
	uint32_t Dbase100BitmapTestClear(uint32_t idx);

	// ---- entries ----------------------------------------------------------------
	// eval_dialogue_record_by_id (dbase100.c:545-555). RAW 0x2b, and the
	// boot / quit / new-game / death hooks (ids 1-4).
	uint32_t EvalDialogueRecordById(uint32_t id);
	// eval_or_queue_dialogue_record_commands (dbase100.c:427-447). `record`
	// is the image offset of a record HEADER: an action record, or an item
	// trigger block (DbTriggerBlock::offset).
	uint32_t EvalOrQueueDialogueRecordCommands(uint32_t record, uint32_t flag);
	// eval_dialogue_record_condition_with_cleanup (dbase100.c:569-576): the
	// entry the inventory / examine / topic code uses on trigger blocks, with
	// flag 0 (run), 1 (examine) or 2 (scan only).
	uint32_t EvalDialogueRecordConditionWithCleanup(uint32_t record, uint32_t flag);
	// The same two entries by reader object.
	uint32_t EvalAction(const DbAction &a, uint32_t flag) { return EvalOrQueueDialogueRecordCommands(a.offset, flag); }
	uint32_t EvalBlock(const DbTriggerBlock &b, uint32_t flag) { return EvalDialogueRecordConditionWithCleanup(b.offset, flag); }

	// execute_dbase100_chain (dbase100.c:607-953). `chain` is the image offset
	// of the first command word. Returns the effect accumulator, or 1 / -1
	// (0xffffffff) from the early-outs; see the definition.
	uint32_t ExecuteDbase100Chain(uint32_t chain, int32_t count, uint32_t flags);
	// finalize_dbase100_chain (dbase100.c:305-320): returns `arg` unchanged.
	uint32_t FinalizeDbase100Chain(uint32_t arg);
	// filter_dbase100_active_records (dbase100.c:41-100). Advances `cursor`,
	// fills `out`, sets `activeCount`, returns the remaining count.
	int32_t FilterDbase100ActiveRecords(uint32_t &cursor, int32_t count,
		std::vector<Segment> &out, int32_t &activeCount);

	// ---- queue and choices ----------------------------------------------------
	void RunDialogueActionQueue();          // dbase100.c:452-480
	void AdvanceDialogueActionQueue();      // dbase100.c:484-497
	void FinishDialogueRecordEval();        // dbase100.c:291-296
	uint32_t ExecuteDialogueBranch(uint32_t index);   // dbase100.c:502-528
	void CloseDialogueAndRunBranch();       // dbase100.c:532-540

	// ---- diagnostics --------------------------------------------------------------
	// Command words that reached the dispatch (operand != 0) and were not
	// executed, by 7-bit opcode. Includes the markers 0x09 / 0x0c and any
	// opcode outside 1..0x37.
	const std::map<int, int> &Unhandled() const { return mUnhandled; }
	// Words skipped because their operand was 0 (dbase100.c:633).
	int OperandZeroSkips() const { return mOperandZeroSkips; }
	// Places this port stopped short of ROTH.C, by fixed name.
	const std::map<std::string, int> &Anomalies() const { return mAnomalies; }
	int AnomalyTotal() const;
	int OutOfImageReads() const { return mOutOfImageReads; }
	void ClearDiagnostics();

	// Guards that ROTH.C does not have (it would hang or overrun its stack).
	// Hitting one is counted in Anomalies().
	static const int MAX_STEPS = 1 << 16;           // command words per outermost call
	static const int MAX_DEPTH = 64;                // nested ExecuteDbase100Chain calls
	static const int MAX_QUEUE_PASSES = 4096;       // RunDialogueActionQueue iterations
	static const int FILTER_SEGMENTS = 0x400 / 8;   // ROTH.C's filt_out, dbase100.c:621

private:
	uint32_t Word(uint32_t offset);
	uint8_t Byte(uint32_t offset);
	void Anomaly(const char *what) { mAnomalies[what]++; }
	Dbase100Sink &Sink() { return mSink != nullptr ? *mSink : mNullSink; }
	uint32_t RequeueA(uint32_t cur, int32_t count, uint32_t flags);
	uint32_t RequeueTail();
	bool PanelBlocks();
	void ForceEndWhileBusy();
	bool BuildChoiceMenu(uint32_t &cur, int32_t &count, uint32_t flags, int32_t &sub_idx, uint32_t &ret);

	const Dbase100 *mDb = nullptr;
	Dbase100Sink *mSink = nullptr;
	Dbase100Sink mNullSink;
	Dbase100InterpState mState;

	int mDepth = 0;
	int mSteps = 0;
	std::map<int, int> mUnhandled;
	std::map<std::string, int> mAnomalies;
	int mOperandZeroSkips = 0;
	int mOutOfImageReads = 0;
};

} // namespace roth
