#pragma once
//
// Realms of the Haunting's examine popup -- an item's close-up film, its
// document pages, its topic menu -- drawn as the original draws it, into an
// 8-bit picture of the popup's rectangle of the original's screen: the frame
// from DATA\ICONS.ALL, the film from DBASE300.DAT (a GDV, decoded by
// roth_gdv, frame by frame on the original's clock), the pages from
// DBASE300.DAT (DAS pictures), the item's icon from DBASE200.DAT, the texts in
// the original's font out of the player's ROTH.EXE, every piece placed by the
// original's routines. The clicks are the original's own rectangles.
// Engine-free: the C++ standard library and the roth_* readers it names.
//
// WHERE IT COMES FROM (ROTH.C, roth_c/src/engine); the numbered checklist with
// file:line on both sides is in roth_popupdraw.cpp:
//   load_dbase300_resource_at_offset  file_config.c:900-1144   the popup's life: film, modal loop, keys
//   render_inspect_popup_window       dialogue_ui.c:1299-1541  the window
//   update_inspect_popup_choices      dialogue_ui.c:1255-1283  one frame of it
//   draw_reloc_ui_row                 dialogue_ui.c:1127-1139  the buttons lit and put out
//   dispatch_dialogue_ui_action       dialogue_ui.c:1556-1631  actions 1..0x16 (the drawing half)
//   hit_test_dialogue_ui_action       dialogue_ui.c:609-661    the popup's half
//   handle_cursor_click               input.c:1260-1360        the pointer, as far as the popup sees it
//   render_choice_text_panel          dialogue_ui.c:854-892    the topic menu
//   render_text_ui                    dialogue_ui.c:971-980    (which panel it draws)
//   apply_ui_palette_rect             renderer.c:10216-10250   the topic lines' backing
//   load_inspect_document_page        dialogue_ui.c:1817-1876  a page and the page arrows
//   load_das_cache_resource           das_assets.c:421-454     (a DBASE300 resource)
//   decode_das_to_padded_buffer       das_assets.c:461-481     (the page, 8 pixels of 0 around)
//   draw_das_panel_slide_reveal       das_assets.c:1024-1104   the page on screen, its reveal
//   gdv_inspect_frame_callback        gdv_cutscene.c:1593-1656 the film's frame callback
//   blit_saved_ui_block               blit_2d.c:1691-1703      a film frame into the window
//   blit_opaque_rect                  blit_2d.c:336-353
//   gdv_decode_timer_isr_noaudio      gdv_cutscene.c:2568-2590 the film's clock
//   gdv_run_playback_loop             gdv_cutscene.c:2900-2980 (its ends: once, or looped)
//   gdv_read_frame_chunk              gdv_cutscene.c:1381-1400 (a loop's first chunk: 0x40)
//   gdv_emit_decoded_frame / gdv_invoke_user_callback  gdv_cutscene.c:1550-1571, 1666-1703
//   blit_item_icon                    roth_uidraw (UiCanvas::BlitItemIcon, shared)
//
// WHAT IS NOT IN THE PICTURE: the 4-pixel shadow the original blends around
// the window (draw_popup_shadow_border_smc) -- the world's pixels -- and the
// software cursor. The picture is opaque: the original clears the whole
// rectangle before it draws.
//

#include "roth_uidraw.h"
#include "roth_dialogue.h"
#include "roth_gdv.h"

#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <string>
#include <vector>

namespace roth
{

class GameCore;
class InventoryScreen;

// One of the six click rectangles the window publishes (0x80b40 + 8 * i,
// dialogue_ui.c:1304-1309, 1405-1414, 1484-1537, 1823-1824): action i + 1.
// hit_test_dialogue_ui_action skips a cell of height 0 and takes
// 0 <= y - this.y < h and 0 <= x - this.x < w, unsigned (dialogue_ui.c:647-659).
struct PopupCell
{
	uint16_t y = 0, h = 0, x = 0, w = 0;
};

class InspectPopupPainter : public UiCanvas
{
public:
	// g_screen_pitch (0x85498), the logical height (0x8549c), the line-doubled
	// modes' flag (0x90cbd) and the panel anchor's y (0x80b28): 640, 480, off,
	// 240 in the retail VideoMode 8.
	int screenPitch = 640, screenHeight = 480;
	bool hires = false;
	int anchorY = 240;
	uint8_t markerSelected = 0x40;          // g_map_menu_marker_selected 0x7675d
	// load_das_cache_resource on DBASE300.DAT (das_assets.c:421-454): the
	// picture of resource `res` (its bytes at res * 8 + 4, the dword before
	// them its size); empty when it cannot be read.
	std::function<std::vector<uint8_t>(uint32_t res)> dbase300;

	// render_inspect_popup_window's inputs.
	struct Window
	{
		int width = 0, height = 0;          // the film's (DBASE300 offset + 0x14, file_config.c:944-946)
		uint8_t flags4 = 0;                 // the item record's +4
		uint8_t runtime7 = 0;               // its +7, before the first view sets 0x80
		std::vector<uint32_t> pages;        // copy_record_block_op7's result: DBASE300 resources (<= 0x20)
		int32_t *currentPage = nullptr;     // g_inspect_current_page 0x81184: clamped here (:1341-1342)
		uint32_t topicCount = 0;            // build_available_choice_menu's count (:1350)
		const std::vector<TextLine> *topics = nullptr;   // its laid-out lines (g_choice_line_records)
		int topicLines = 0;                 // g_choice_line_count
		uint8_t *label = nullptr;           // 0x8105c, {flag, colour, text...} (0x64): the inventory label; written here
		size_t labelSize = 0;
		const uint8_t *icon = nullptr;      // **p4: the item's DBASE200 icon, or null
		size_t iconSize = 0;
	};
	// render_inspect_popup_window (dialogue_ui.c:1299-1541): the window
	// composed; the picture covers its rectangle.
	void OpenWindow(const Window &w);
	// free_inspect_popup_and_redraw (dialogue_ui.c:770-782): the window goes
	// (the original puts back what lay under it).
	void CloseWindow();
	bool IsOpen() const { return mOpen; }

	// The window on the original's screen: x, y ([0x810e4]/[0x810e6]), the
	// width as the window was cleared and the height.
	int WinX() const { return mEdi; }
	int WinY() const { return mEsi; }
	int WinW() const { return mW - 1; }
	int WinH() const { return mH; }
	const PopupCell *Cells() const { return mCells; }
	int PageCount() const { return (int)mPages.size(); }
	// The topic menu's origin (0x83add / 0x83ae1) and its box (0x83ad1..0x83ad9).
	int ChoiceX() const { return mChoiceX; }
	int ChoiceY() const { return mChoiceY; }
	int ChoiceHeight() const { return mChoiceH; }
	// g_inspect_popup_state [0x810c0] as the window leaves it: 1 with pages.
	int StateAtOpen() const { return mStateAtOpen; }
	// The geometry words [0x810dc..0x810f2]: the film's x, y, width, height;
	// the window's x, y, w, h; the page rectangle's x, y, w, h.
	void Words(int out[12]) const;
	int ChoiceX0() const { return mChoiceX0; }
	int ChoiceX1() const { return mChoiceX1; }
	// How many times the screen took a picture (Present / PresentChoices).
	uint32_t Presents() const { return mPresents; }

	// ---- the film --------------------------------------------------------------------
	// blit_saved_ui_block's copy (blit_2d.c:1693-1703): `w` x `h` bytes of
	// the decode buffer into the window's film rectangle; nothing while the
	// topic menu is up (mode 1).
	void FilmFrame(const uint8_t *buf, int mode);

	// ---- the buttons -----------------------------------------------------------------
	// draw_reloc_ui_row (dialogue_ui.c:1127-1139) for the magnifier (bit 0 of
	// 0x80b34: lit 0xa0 / out 0x98) and the "i" (bit 1: lit 0x90 / out 0x88):
	// the rows the original draws when a bit changes.
	void MarksChanged(int before, int after);
	// Actions 5 / 6 (dialogue_ui.c:1605-1626): the pressed page arrow, drawn
	// when the page did change.
	void PageArrowPressed(bool down);

	// ---- the document page (the modal loop) -------------------------------------------
	// The page descriptor (file_config.c:887-898, 991-996).
	struct Page
	{
		std::vector<uint8_t> pix;           // the padded picture ([+0]), w x h
		int pw = 0, ph = 0;                 // +0x0c / +0x10
		int x = 0, y = 0;                   // +0x04 / +0x08
		int w = 0, h = 0;                   // +0x14 / +0x18: the rectangle shown
		int sx = 0, sy = 0;                 // +0x20 / +0x24: the scroll
		int redraw = 0;                     // +0x28: the reveal's progress
		int prevX = 0, prevY = 0;           // +0x48 / +0x4c
		bool loaded = false;                // `page` (the handle) != 0
	};
	Page &Desc() { return mPage; }
	const Page &Desc() const { return mPage; }
	// The modal loop's set-up (file_config.c:991-1005).
	void BeginModal(int32_t currentPage);
	// load_inspect_document_page (dialogue_ui.c:1817-1876).
	void LoadDocumentPage(int32_t currentPage);
	// draw_das_panel_slide_reveal (das_assets.c:1024-1104) with frame scale `scale`.
	void SlideReveal(int scale);
	// The page turn the modal loop does when one is pending, and the redraw
	// when the scroll moved or the reveal runs (file_config.c:1014-1030).
	void ModalPage(int32_t currentPage, int scale);
	int PagePending() const { return mPagePending; }

	// ---- the topic menu --------------------------------------------------------------
	// render_choice_text_panel (dialogue_ui.c:854-892) on what the window
	// shows now: the picture the screen shows (Presented()), the window itself
	// left as the original leaves its back buffer (the saved block put back,
	// video_display.c:578-582). `lines`/`count` the topic records,
	// `scale` g_frame_time_scale.
	void PresentChoices(const std::vector<TextLine> &lines, int count, int scale);
	// No topic menu this frame: the screen shows the window as it is.
	void Present();
	const UiCanvas &Presented() const { return mShown; }
	// 0x83ae5, the topic lines' reveal; 2 when the menu is (re)built or toggled.
	int32_t choiceReveal = 0;

	// ---- the pointer -----------------------------------------------------------------
	// hit_test_dialogue_ui_action with the popup up (dialogue_ui.c:551-552,
	// 609-661) at screen (mx, my): 0x27 while a line is spoken, 4 while a page
	// is being dragged, 7 + row on a shown topic line (`row` set, for the
	// marker writes), else the cell's action, else 0.
	int HitTest(int mx, int my, int mode, bool busy, bool dragging, const std::vector<TextLine> &lines, int lineCount, int *row) const;

	// apply_ui_palette_rect (renderer.c:10216-10250) on the picture.
	void ApplyUiPaletteRect(int x0, int y0, int x1, int y1, uint32_t level);

private:
	void RelocRow(int y, uint32_t slot);    // draw_reloc_ui_row
	void BlitReloc(int x, int y, uint32_t slot, int fill = 0) { BlitRelocDasImage(x, y, slot, (uint32_t)fill); }
	void BlitRecordAt(uint32_t slot, int x, int y);   // resolve + blit_das_image_at_xy
	int RecordW(uint32_t slot);
	int RecordH(uint32_t slot);

	bool mOpen = false;
	int mEdi = 0, mEsi = 0, mW = 0, mH = 0;
	int mWidth0 = 0, mHeight0 = 0;
	int mContentX = 0, mContentY = 0;       // 0x810dc / 0x810de
	int mLoX = 0, mLoY = 0, mHiX = 0, mHiY = 0;   // 0x810ec..0x810f2
	PopupCell mCells[6];
	std::vector<uint32_t> mPages;
	int mStateAtOpen = 0;
	// The topic menu's box (0x83ad1/0x83ad5/0x83ad9) and origin; kept from
	// one window to the next as the original's globals are.
	int mChoiceX0 = 0, mChoiceX1 = 0, mChoiceH = 0, mChoiceX = 0, mChoiceY = 0;
	Page mPage;
	int mPagePending = 0;                   // 0x81180
	UiCanvas mShown;
	uint32_t mPresents = 0;
};

// The close-up film on the original's clock: gdv_decode_frame's playback with
// the inspect callback (file_config.c:927-981), one 60 Hz GDV timer tick at a
// time (gdv_decode_timer_isr_noaudio, gdv_cutscene.c:2568-2590).
class InspectFilm
{
public:
	// `gdv` the DBASE300 bytes from the offset on (the GDV file, kept alive by
	// the caller); `hold` the record's +4 bit 0 (param_2): clear, the film
	// loops (stream flag 8, file_config.c:930).
	bool Open(const uint8_t *gdv, size_t size, bool hold);
	const std::string &Error() const { return mError; }
	int Width() const { return mDec.Width(); }
	int Height() const { return mDec.Height(); }
	bool Looping() const { return mLoop; }
	// One GDV timer tick (the budget, gdv_cutscene.c:2570-2587): true when a
	// frame was decoded.
	bool TimerTick();
	// gdv_invoke_user_callback(0)'s latch (gdv_cutscene.c:1597-1600, 1672-1673):
	// a decoded frame that reaches the screen arms it; the boundary takes it.
	bool Armed() const { return mArmed; }
	void TakeLatch() { mArmed = false; }
	const uint8_t *Pixels() const { return mDec.Pixels(); }
	// [0x91dba]: frames still to decode before the film ends (looping: never
	// below 0). -1 = all done.
	int Remaining() const { return mDba; }
	int Decodes() const { return mDecodes; }

private:
	bool DecodeNext();
	GdvDecoder mDec;
	const uint8_t *mData = nullptr;
	size_t mSize = 0;
	std::vector<size_t> mRecords;           // each record's offset in the file
	std::vector<uint8_t> mChunk;
	size_t mNext = 0;                       // the record decoded next
	bool mLoop = false;
	bool mArmed = false;
	int mDb0 = 0, mDbe = 0, mDba = 0;
	int mDecodes = 0;
	uint32_t mAudioBytes = 0;
	std::string mError;
};

// The popup's life: load_dbase300_resource_at_offset's film phase and modal
// loop (file_config.c:900-1144), with update_inspect_popup_choices once a
// frame, around the rules roth_invscreen keeps (InventoryScreen's popup).
// The caller hands it the original's inputs: the frame tick, the keys, the
// pointer.
class InspectPopupView
{
public:
	InspectPopupPainter painter;
	InspectFilm film;

	void Bind(InventoryScreen *screen, DialogueUi *ui, GameCore *core) { mScreen = screen; mUi = ui; mCore = core; }
	// The popup the rules just opened: its film bytes (DBASE300 from the
	// close-up offset on), the item's icon, the inventory label. Composes the
	// window and primes the film (file_config.c:944-976).
	bool Begin(const uint8_t *gdv, size_t gdvSize, const uint8_t *icon, size_t iconSize, uint8_t *label, size_t labelSize, uint16_t frameTick);
	bool Active() const { return mPhase != NONE; }
	bool InFilm() const { return mPhase == FILM; }
	bool InModal() const { return mPhase == MODAL; }
	// g_inspect_popup_state [0x810c0].
	int State() const { return mState; }

	// The pointer, in the original's screen pixels, and the buttons held
	// (g_mouse_buttons_prev & 3).
	void SetPointer(int x, int y, int held) { mMouseX = x; mMouseY = y; mHeld = held; }
	// A new press (g_cursor_primary/secondary_action_flag): taken by the next
	// update_inspect_popup_choices.
	void Press(int button) { mPress |= button; }

	// ---- the film: one GDV timer tick, then gdv_callback_frame_boundary ----------------
	// Returns false when the boundary ended the film (end of stream). Then,
	// when a key is to be read (KeyWanted), FilmKey with it; then FilmEnded.
	bool FilmBoundary(uint16_t frameTick);
	bool KeyWanted() const;
	bool FilmKey(uint8_t scancode);         // false: end of stream
	// After the boundary: has the film run out (gdv_run_playback_loop's end)?
	// Then Finish the film (enter the modal loop or close).
	bool FilmRanOut() const;
	void FinishFilm(bool endOfStream, uint16_t frameTick);

	// ---- the modal loop: one pass ----------------------------------------------------------
	void ModalPassBegin(uint16_t frameTick);
	// The drain: true when the key asks for the next one at once.
	bool ModalKey(uint8_t scancode);
	// The pass's exit test; closes the popup when it holds.
	bool ModalPassEnd();

	// update_inspect_popup_choices (dialogue_ui.c:1255-1283) with frame scale
	// `scale`. `textUi` runs render_text_ui's dialogue half (the line
	// sequencer) when set; the engine ticks its dialogue itself.
	std::function<void(int scale)> textUi;
	void UpdateChoices(int scale);

	// The popup ends (file_config.c:1131-1143).
	void Close();

	// Counters for the tests.
	int Boundaries() const { return mBoundaries; }
	int DragState() const { return mInfo; }

private:
	enum Phase { NONE, FILM, MODAL };
	void Dispatch(int action, int code);
	void Pointer();
	Phase mPhase = NONE;
	InventoryScreen *mScreen = nullptr;
	DialogueUi *mUi = nullptr;
	GameCore *mCore = nullptr;
	int mState = 0;                         // 0x810c0
	bool mHold = false;
	uint16_t mLastTick = 0;                 // g_last_frame_tick 0x85320
	int mMouseX = 0, mMouseY = 0, mHeld = 0, mPress = 0;
	// The page drag (0x81188 and on, file_config.c:1032-1061; dialogue_ui.c:1596-1602).
	int mInfo = 0, mDragX = 0, mDragY = 0, mDragSx = 0, mDragSy = 0;
	int mBoundaries = 0;
	int mLastMode = 0;
	int mExitAcc = 0;                       // the modal loop's own exit count (file_config.c:1006)
};

} // namespace roth
