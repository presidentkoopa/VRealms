#pragma once
//
// Realms of the Haunting's dialogue: everything AROUND the DBASE100
// interpreter that decides what the player reads, for how long, what he may
// answer and what answering does. Engine-free: the C++ standard library and
// the interpreter (roth_dbase100_interp.h), nothing else. Built and tested
// on its own (tools/dialogue_test), and differential-tested against the
// original engine running the same scripts (tools/dialogue_test/README.md).
//
// WHERE THE RULES COME FROM. ROTH.C, roth_c/src/engine. Each rule cites
// `file.c:LINE`:
//
//   read_next_dialogue_line              dbase100.c:371-403
//   resolve_dbase100_text                dbase100.c:408-423
//   layout_timed_message_text            dialogue_ui.c:344-480
//   measure_font_char_advance            renderer.c:337-348
//   dbase100_open_dialogue_window        dialogue_ui.c:1178-1218   (the choice menu)
//   dbase100_open_dialogue_window_alt    dialogue_ui.c:1224-1247   (a spoken line)
//   render_text_ui (its sequencer half)  dialogue_ui.c:971-1014
//   render_active_timed_message (timer)  dialogue_ui.c:900-906
//   queue_timed_message_color            renderer.c:494-506
//   voice_stream_pump                    audio.c:1300-1327
//   prime_voice_clip (the header half)   audio.c:1208-1292
//   dialogue_voice_force_end             audio.c:1334-1350
//   dialogue_voice_stop_all              audio.c:1355-1366
//   try_interrupt_dialogue_voice         audio.c:1371-1378
//   choice_select_prev / _next           dialogue_ui.c:138-186
//   choice_accept_selected               dialogue_ui.c:215-228
//   update_dialogue_choice_highlight     dialogue_ui.c:231-271
//   use_enter_key_handler                input.c:718-745
//   build_available_choice_menu          dialogue_ui.c:1059-1120   (an item's topics)
//   activate_selected_choice_record      dialogue_ui.c:191-203
//
// WHAT IS DELIBERATELY NOT HERE: pixels. The original keeps a framebuffer
// rectangle per line and hit-tests the mouse against it; this keeps the LINES
// (their text, colour and wrap, which are content) and takes "which row is
// the pointer on" from the presentation, which knows its own layout. What a
// row index then DOES is the original's, unchanged.
//
// THE CLOCK. Every dwell here is in the original's frame ticks, 70 a second
// (dialogue_ui.c:1235 scales a clip's seconds by 0x46; audio.c:1835). The
// caller says how many ticks a frame took (g_frame_time_scale, 0x85324).
//
// SPEECH. The original times a spoken line by its DBASE500 clip and holds the
// last page until the sound driver reports the clip finished
// (dialogue_ui.c:1000-1001; audio.c:1178). The sound itself is another
// system. What this needs from it is a header (SpeechHeader), start / stop
// (VoiceStarted / VoiceStopped) and "the clip ended" (VoiceEnded); the engine
// side wires those to roth_sound's speech player. TickVoiceClock is a stand-in
// driver for tests: it ends the clip after exactly its own length.
//

#include "roth_dbase100_interp.h"

#include <stdint.h>
#include <string>
#include <vector>

namespace roth
{

// measure_font_char_advance (renderer.c:337-348) over the font built into the
// executable (g_font_descriptor, 0x70f12): control bytes <= 0x0d are 0 wide,
// a character with no glyph takes the default advance (3), the rest the low
// nibble of their first glyph byte plus one.
int FontCharAdvance(uint8_t ch);

// One line record of layout_timed_message_text: the 0x14-byte entry
// { min x, max x + 1, height, byte length, text pointer } (dialogue_ui.c:423-427,
// 471-475), with the text held here. `text` is the original's control-coded
// string: 01 <colour> opens a record, 02 <lo> <hi> is the x offset of each
// wrapped line, 0a separates wrapped lines inside the record, 00 ends it.
struct TextLine
{
	int32_t x0 = 0;         // +0x00
	int32_t x1 = 0;         // +0x04
	int32_t height = 0;     // +0x08  11 per wrapped line
	int32_t len = 0;        // +0x0c  bytes, as the original counts them
	std::vector<uint8_t> text;

	// What a reader sees: the wrapped lines of this record, control codes
	// removed. For presentation.
	std::vector<std::string> Rows() const;
	// The record's colour byte (the 01 <colour> prefix): a palette index.
	uint8_t Color() const { return text.size() >= 2 && text[0] == 1 ? text[1] : 0; }
};

// layout_timed_message_text (dialogue_ui.c:344-480). `src` must end in a 0
// byte. `maxw` is g_screen_pitch for a dialogue (pitch - 0x50 for an item's
// topic menu), `maxLines` the wrapped-line budget of one record: 10 for a
// choice menu, 3 for a spoken line, 10 for a timed message, 0x10 for topics.
// `color` is g_timed_message_color at the time of the call. Returns the
// record count, 0 when a word is too wide to break (dialogue_ui.c:398-399):
// then nothing is shown, and `out` holds whatever was finished before it.
int LayoutTimedMessageText(std::vector<TextLine> &out, const uint8_t *src, int maxw, int maxLines, uint8_t color);

// What the presentation draws, already decided.
struct DialogueView
{
	enum Kind { NONE, LINE, CHOICES };
	Kind kind = NONE;
	// LINE: the wrapped rows of the page on screen now. CHOICES: one entry per
	// choice (a choice that wrapped has '\n' inside its entry).
	std::vector<std::string> rows;
	uint8_t color = 0;          // the DBASE400 header's colour byte: a palette index
	uint8_t highlightColor = 0; // CHOICES: the palette index the highlighted row takes
	int highlighted = -1;       // CHOICES: the row the marker is on, -1 none
	bool waitsForClick = false; // LINE with no speech: it stays until dismissed
	int page = 0, pages = 0;    // LINE: which page of how many
};

class DialogueUi : public Dbase100Sink
{
public:
	// The original's two gate values (dialogue_ui.c:22, 989, 996).
	static const int32_t GATE_MENU = 0x6ffff;       // a choice menu is up
	static const int32_t GATE_WAIT = 0x7ffff;       // an unvoiced line waits for a click
	static const int TIMED_MESSAGE_TICKS = 0x8c;    // renderer.c:504

	// Bind to the interpreter whose flow globals this shares (busy, context,
	// gate, voice state, the two choice indices: Dbase100InterpState). Not owned.
	void Bind(Dbase100Interp *interp, const Dbase100 *db);

	// DBASE400.DAT, whole.
	std::vector<uint8_t> text;

	// g_screen_pitch (0x85498): the width text is wrapped to. 640 is the retail
	// ROTH.INI's VideoMode 8; 320 is the low-resolution mode.
	int screenPitch = 640;
	// 0x83e90 ("VA_g_active_weapon_ammo_cap + 0x1c"): set, a VOICED line shows
	// no text at all (dialogue_ui.c:1226-1227). The SpeechSub option.
	bool subtitlesOff = false;
	// g_audio_decode_buffer_b (0x85490) != 0: speech can play. Clear, no clip
	// is ever primed and every line waits for a click (audio.c:1210).
	bool speechAvailable = false;
	// g_default_message_color (0x7675e), seven bytes: [0] a refusal, [5] a
	// success (inventory.c:724, 753). From the executable's initialised data.
	uint8_t defaultMessageColor[7] = { 0x67, 0x54, 0x67, 0xee, 0xea, 0xee, 0x45 };
	// g_map_menu_marker_selected (0x7675d): the colour the highlighted choice
	// is redrawn in. The byte before the table above.
	uint8_t markerSelected = 0x40;

	// ---- what the sound system supplies ---------------------------------------
	// The 0x2c-byte clip header at DBASE500.DAT offset clip << 3 (audio.c:1245).
	// False when there is no such clip. The default has no speech file.
	virtual bool SpeechHeader(uint32_t clip, uint8_t hdr[0x2c]) { (void)clip; (void)hdr; return false; }
	// A clip was primed / stopped (install_voice_sos_callback, sos_stop_voice).
	virtual void VoiceStarted(uint32_t clip, uint32_t samples, uint32_t rate) { (void)clip; (void)samples; (void)rate; }
	virtual void VoiceStopped() {}
	// The driver's end-of-stream callback (audio.c:1178): state 1 -> 2.
	void VoiceEnded();
	// Stand-in for the driver while no speech player exists: count the primed
	// clip's own length down and end it. `ticks` are 70 Hz frame ticks.
	void TickVoiceClock(int ticks);

	// ---- Dbase100Sink: the calls this system answers ----------------------------
	uint32_t ReadNextDialogueLine(uint8_t *dest, uint32_t maxlen, uint32_t voiceOffset, uint32_t flag) override;
	uint32_t Dbase100OpenDialogueWindow(const uint8_t *text, uint32_t bytes) override;
	uint32_t Dbase100OpenDialogueWindowAlt(const uint8_t *text, uint32_t bytes) override;
	void DialogueVoiceForceEnd(uint32_t eax, uint32_t edx) override;

	// resolve_dbase100_text (dbase100.c:408-423): interface text `index`, or
	// "Missing Text".
	std::string ResolveDbase100Text(int index, uint32_t flag = 0);
	// The same, byte for byte as the original hands it back: up to `maxlen`
	// bytes into `dest`, the count read (13 for "Missing Text" and its 0).
	uint32_t ResolveDbase100TextBytes(uint8_t *dest, uint32_t maxlen, int index, uint32_t flag = 0);
	// The text of a DBASE400 entry, at most `maxlen` bytes, through
	// ReadNextDialogueLine(flag 0) -- so it sets g_timed_message_color as the
	// original's read does.
	std::string LineText(uint32_t voiceOffset, uint32_t maxlen = 0x62);

	// ---- per frame, in the original's order (game_core.c:848; dialogue_ui.c:971) ---
	void VoiceStreamPump();                 // audio.c:1300-1327
	void TickTextUi(int frameTicks);        // render_text_ui + render_active_timed_message, minus drawing
	// Both, as one frame of the game loop runs them.
	void Frame(int frameTicks) { VoiceStreamPump(); TickTextUi(frameTicks); }

	// ---- input ---------------------------------------------------------------------
	void ChoiceSelectPrev();                // Up     dialogue_ui.c:138-160
	void ChoiceSelectNext();                // Down   dialogue_ui.c:164-186
	void ChoiceAcceptSelected();            // dialogue_ui.c:215-228
	void UseEnterKey();                     // Enter  input.c:718-745 (the dialogue half)
	// update_dialogue_choice_highlight (dialogue_ui.c:231-271). `pos` is any
	// number that changes when the pointer moves (the original adds the mouse
	// x and y); `row` is the choice row under the pointer, -1 for none.
	// Returns 0 (no menu up), 1 (the pointer is on a row: it is now the saved
	// choice), 2 (a menu is up, no row hit or the keyboard selection stands).
	uint32_t UpdateChoiceHighlight(int pos, int row);
	// A pointer click with the pointer at (pos, row): dialogue_voice_force_end
	// with the mouse position (audio.c:1334-1350; dialogue_ui.c:1163).
	void PointerClick(int pos, int row);
	// Where the pointer is now: the original's g_mouse_x / g_mouse_y, which
	// the skip key hands to the same routine a click does (audio.c:1375).
	// The presentation sets it every frame; a flat keyboard has (0, -1).
	void SetPointer(int pos, int row) { mPointerPos = pos; mPointerRow = row; }
	// The hover half of the game loop's mouse handling (game_core.c:556-569):
	// while a menu is up or a line is waiting, the pointer belongs to the
	// dialogue and the world under it is not looked at.
	bool PointerBelongsToDialogue() const;
	// try_interrupt_dialogue_voice (audio.c:1371-1378): the skip key. 1 when it
	// had something to skip.
	uint32_t TryInterruptDialogueVoice();
	// dialogue_voice_stop_all (audio.c:1355-1366): load / quit.
	void DialogueVoiceStopAll();

	// ---- timed messages (the item "I cannot use ..." line) ---------------------------
	virtual void QueueTimedMessageColor(const std::string &msg, uint8_t color);     // renderer.c:494-506
	// use / combine messages: resolve_dbase100_text(interfaceIndex) as a printf
	// format, the names read with read_next_dialogue_line(max 0x62, flag 0),
	// colour defaultMessageColor[colorIndex] (inventory.c:704-727, 812-860).
	void ItemMessage(int interfaceIndex, int colorIndex, uint32_t nameA, uint32_t nameB);

	// ---- an item's topic menu (the inspect popup's "i" list) -------------------------
	// build_available_choice_menu (dialogue_ui.c:1059-1120) for one item record:
	// every type-1 block whose condition scan holds contributes its text. The
	// visible count; the laid-out lines are TopicLines().
	uint32_t BuildAvailableChoiceMenu(int32_t itemId);
	// activate_selected_choice_record (dialogue_ui.c:191-203), 1-based.
	void ActivateSelectedChoiceRecord(uint32_t index);
	const std::vector<TextLine> &TopicLines() const { return mChoiceLines; }
	int TopicCount() const { return (int)mChoiceOptionBlocks.size(); }

	// ---- state, under the original's names ---------------------------------------------
	const std::vector<TextLine> &Segments() const { return mSegments; }     // g_dialogue_segments 0x827fd
	int32_t SegmentIndex() const { return g_dialogue_segment_index; }
	const std::vector<TextLine> &TimedMessageLines() const { return mTimedLines; }
	int32_t TimedMessageLineCount() const { return g_timed_message_line_count; }
	int32_t TimedMessageTimer() const { return g_timed_message_timer; }
	// Is anything of the dialogue on screen or pending? Movement and world
	// clicks are not blocked by the original while a LINE is up (the player
	// walks on under a monologue); a MENU takes Up / Down / Enter and the
	// pointer (game_core.c:913-920; input.c:718-727).
	bool MenuUp() const;
	bool LineUp() const;
	DialogueView View() const;

	int32_t g_dialogue_segment_index = 0;       // 0x83121
	int32_t g_dialogue_line_dwell = 0;          // 0x83119
	int32_t g_dialogue_dwell_divisor = 0;       // 0x8311d
	int32_t g_dialogue_reveal_ramp = 0;         // 0x83129
	int32_t g_dialogue_panel_color = 0;         // 0x83139 "g_dialogue_reveal_ramp + 0x10"
	int32_t g_choice_last_pos = 0;              // 0x83b16 "g_dialogue_busy_flag + 0x2c"
	uint8_t g_timed_message_color = 0;          // 0x82040
	int32_t g_timed_message_line_count = 0;     // 0x827e1
	int32_t g_timed_message_timer = 0;          // 0x827e5
	int32_t g_voice_sample_count = 0;           // 0x8202c "g_voice_bytes_remaining + 0x14"
	int32_t g_voice_rate = 0;                   // 0x82030 "g_voice_bytes_remaining + 0x18"
	int32_t g_choice_interaction_mode = 0;      // 0x83acd
	int32_t g_choice_line_count = 0;            // 0x83ac9
	int32_t g_pending_choice_accept_index = 0;  // 0x81304
	// The voice clock's remaining 70 Hz ticks (this port's; see TickVoiceClock).
	int32_t voiceClockTicks = 0;

protected:
	Dbase100InterpState &St() { return mInterp->State(); }
	const Dbase100InterpState &St() const { return mInterp->State(); }
	virtual uint32_t PrimeVoiceClip(uint32_t clip);     // audio.c:1208-1292, the header half

	Dbase100Interp *mInterp = nullptr;
	const Dbase100 *mDb = nullptr;
	std::vector<TextLine> mSegments;
	std::vector<TextLine> mTimedLines;
	std::vector<TextLine> mChoiceLines;                 // g_choice_line_records 0x83189
	std::vector<uint32_t> mChoiceOptionBlocks;          // g_choice_option_records 0x83149: block offsets
	// The row carrying the highlight marker (g_map_menu_marker_selected written
	// to a line's colour byte, dialogue_ui.c:131-132, 261): -1 none.
	int32_t mMarkerRow = -1;
	// Clear the marker off every record, then stamp it on `row` (-1: none).
	void StampMarker(std::vector<TextLine> &lines, int count, uint8_t normal, int row);
	int mPointerPos = 0, mPointerRow = -1;
	uint8_t mTopicColor = 0;                            // 0x83ae9 g_choice_marker_normal_alt

	// The timed-message cache, g_message_cache_head 0x820c1: 32 nodes, most
	// recent first (dialogue_ui.c:273-334).
	struct CacheNode
	{
		int32_t key = 0;
		int16_t len = 0;
		uint8_t color = 0;
		uint8_t text[0x14] = {};
	};
	bool mCacheBuilt = false;
	std::vector<CacheNode> mCache;
	int32_t CacheLookup(uint8_t *dest, int32_t key);
	void CacheStore(const uint8_t *textIn, int32_t key, int32_t len, uint8_t color);
};

} // namespace roth
