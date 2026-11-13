//
// Realms of the Haunting's examine popup, drawn as the original draws it.
// See roth_popupdraw.h.
//
// THE CHECKLIST (porting rule 2). ROTH.C file:line -> here. "rules" = done by
// roth_invscreen / roth_dialogue / roth_inventory, called here at the same point.
//
//  load_dbase300_resource_at_offset, file_config.c:900-1144
//   1  :909-911 popup active, info latch [0x810f4] = 0, content ptr = 0 ...... rules (OpenPopup) / InspectFilm::Open (latch)
//   2  :913-920 open DBASE300 (CD-swap retry box) ............................. not ported: the engine reads the file it found (the bytes are passed in)
//   3  :922-936 the GDV context: decode buffer 0x50000, stream flags 0x1890 / 0x1090,
//      | 8 without hold, callback 0x18e09; state = info = 0 ....................... InspectFilm::Open (flags: the loop bit), InspectPopupView::Begin
//   4  :942 [0x7fec8] = record ................................................... rules (OpenPopup: mPopupItem)
//   5  :944-946 the film's width/height at offset + 0x14 ........................... InspectPopupView::Begin
//   6  :948-951 render_inspect_popup_window ...................................... InspectPopupPainter::OpenWindow
//   7  :958-967 decoder open; the first view runs OnInspect ...................... InspectFilm::Open / rules (ExamineFirstView, in OpenPopup)
//   8  :968-974 latch = 0; gdv_decode_frame (the film, items 30-36) ............... InspectFilm::Open (prime) + InspectPopupView::FilmBoundary
//   9  :976-977 want_loop = hold; state != 0 -> want_loop, not "ended" ........... InspectPopupView::FinishFilm
//  10  :979-987 decoder closed, blit_saved_ui_block ................................ InspectPopupView::FinishFilm
//  11  :989-1005 the modal loop's descriptor; the first page; state = 3; redraw 4 . InspectPopupPainter::BeginModal
//  12  :1007-1012 the frame clock (spin until the tick moves) ......................... InspectPopupView::ModalPassBegin (the caller's tick)
//  13  :1014-1020 a pending page turn reloads the page ................................ InspectPopupPainter::ModalPage
//  14  :1021-1030 the page redrawn when the scroll moved or the reveal runs ........... InspectPopupPainter::ModalPage
//  15  :1032-1061 the page drag: anchors, clamps, ends when no button is held ......... InspectPopupView::ModalPassBegin
//  16  :1062-1064 update_inspect_popup_choices; flip .................................. InspectPopupView::UpdateChoices
//  17  :1066-1119 the keys: Esc/I/Space (skip a line, else leave), Enter (accept a
//      topic, else as Esc), B (screenshot), Up/Down (scroll by 8 with the topics
//      hidden, else the topic selection), Left/Right (scroll by 8); the scroll keys
//      read the next key at once ...................................................... InspectPopupView::ModalKey ('B' not ported: no screenshot)
//  18  :1120-1122 leave on [0x7fecc] or the panel closed ............................... InspectPopupView::ModalPassEnd
//  19  :1124-1143 page freed; free_inspect_popup_and_redraw; [0x7fec8] = 0; mode = 0;
//      marks = 0; popup inactive ...................................................... InspectPopupView::Close (+ rules PopupFinish)
//  render_inspect_popup_window, dialogue_ui.c:1299-1541
//  20  :1305-1310 resets: latch src, page pending, the six cells' h/w, [0x7fecc] ..... OpenWindow (the exit request: rules)
//  21  :1313-1324 w = width + 0x28; widened for the label (>= 0x38, + 0x28) when its flag byte is set ... OpenWindow
//  22  :1327-1333 h + 0x13; the minimum width by pitch (0x10e / 0x130 / 0x154) ........... OpenWindow
//  23  :1336-1347 pages (record +4 & 2): count; current page clamped; min + 0x28; h = 0x9f ... OpenWindow
//  24  :1350-1356 topics: widened to their box + 0x26 / + 0x12 ........................... OpenWindow
//  25  :1359-1373 y = anchor + 0x41 - h (>= 4); x centred, 4-aligned; the topic origin .. OpenWindow
//  26  :1376-1381 the saved region (w + 9, h + 8 at -4, -4); w += 1 ........................ not ported: the save-under (the window is its own picture)
//  27  :1384-1399 the film rectangle and the geometry words .................................. OpenWindow
//  28  :1401 the shadow ....................................................................... not ported: blends the world around the window
//  29  :1402 clear_framebuffer_rect (x, y, w - 1, h) ............................................ OpenWindow
//  30  :1405-1414 the cells' x; the drag cell and state 1 with pages .......................... OpenWindow
//  31  :1417-1432 ICONS.ALL 0xd0 / 0xe0 down the sides, height patched to h - 3 ................ OpenWindow
//  32  :1435-1469 0x110, 0xd8, 0xe8, 0x300, 0x308 x n (while >= 0x3a left), 0x310, 0xf8 stretched ... OpenWindow
//  33  :1472-1487 the item's icon (blit_item_icon, mode 2 / 1); its cell ........................ OpenWindow
//  34  :1490-1494 the label in the marker colour at (x + 0x23, y + 2) ............................ OpenWindow
//  35  :1497-1498 the dirty rect ..................................................................... not ported: VGA bookkeeping
//  36  :1501-1519 the magnifier (+4 & 0x40): 0x98, or 0xa0 lit the first time (marks = 1) ........ OpenWindow (marks: rules)
//  37  :1522-1531 the "i" 0x88 with topics ............................................................ OpenWindow
//  38  :1534-1537 the page arrows' cells with more than one page ...................................... OpenWindow
//  update_inspect_popup_choices, dialogue_ui.c:1255-1283
//  39  :1257-1261 topics rebuilt after a chosen one ran; voice pump ...................... rules (PopupFrameBegin); the reveal = 2: UpdateChoices
//  40  :1262-1271 the lit buttons put out (draw_reloc_ui_row 0x98 / 0x88) ............... rules (the bits) + MarksChanged (the rows)
//  41  :1272 handle_cursor_click ........................................................... InspectPopupView::Pointer (items 52-55)
//  42  :1273-1276 a picked topic runs ......................................................... rules (PopupFrameEnd)
//  43  :1277-1282 render_text_ui (the panel it picks: items 46-48), flush, overlays put back .. UpdateChoices / PresentChoices / Present
//  draw_reloc_ui_row, dialogue_ui.c:1127-1139
//  44  x = [0x810e4] + 0x11 - (w >> 1), at row y ........................................... RelocRow
//  dispatch_dialogue_ui_action, dialogue_ui.c:1564-1631
//  45  1 leave; 2 OnInspect again + the lit magnifier; 3 topics shown/hidden + the lit "i",
//      reveal 2; 4 film skip (state 1 -> 2) / drag start (state 3); 5 / 6 the page and the
//      pressed arrow (0x120 / 0x138), a page turn pending; 7..0x16 a topic ............. InspectPopupView::Dispatch (+ rules)
//  render_text_ui, dialogue_ui.c:973-995 (which panel)
//  46  no dialogue: the topics when mode 1; a dialogue with its menu up: not this panel;
//      otherwise the topics when mode 1 ..................................................... UpdateChoices
//  render_choice_text_panel, dialogue_ui.c:854-892
//  47  top = 0x83ae1 - (box h >> 1); the reveal += frame scale, at most 0x20 ................. PresentChoices
//  48  per line: apply_ui_palette_rect (x0 + ox .. x1 - 2 + ox, y .. y + h - 1, reveal >> 1),
//      the text at (ox, y); y += h; saved region put back after (video_display.c:578-582) ... PresentChoices
//  apply_ui_palette_rect, renderer.c:10216-10250
//  49  fs[(level & 0x1f) << 8 | p]; >= 3 rows: the top and bottom rows skip their corners .... ApplyUiPaletteRect
//  hit_test_dialogue_ui_action, dialogue_ui.c:551-552, 609-661
//  50  busy -> 0x27; mode 0 with a drag -> 4 ................................................. HitTest
//  51  mode 1: markers off when nothing is key-selected; a line hit -> marker, 7 + row ....... HitTest (+ rules TopicPointerMarker)
//  52  the six cells, unsigned, cell 1 refused while busy -> action i + 1 ..................... HitTest
//  handle_cursor_click, input.c:1260-1360 (the popup's part)
//  53  the click code: secondary 2, primary + 1; else the held buttons ...................... Pointer
//  54  no button: the hover hit test (marker writes) ..................................... Pointer (the cursor shapes: not ported, no cursor is drawn)
//  55  a press: 0x27 -> dialogue_voice_force_end (mouse); else dispatch .................... Pointer
//  load_inspect_document_page, dialogue_ui.c:1817-1876
//  56  more than one page: the arrows' cells 0x13 high; 0x118 / 0x128 up, 0x130 / 0x140 down .. LoadDocumentPage
//  57  the page resource; pending = 0; padded decode; the scroll clamped; redraw = 4 ......... LoadDocumentPage
//  decode_das_to_padded_buffer, das_assets.c:461-481
//  58  (w + 16) x (h + 16) of 0, the picture at (8, 8), row mode 1 ............................. LoadDocumentPage
//  draw_das_panel_slide_reveal, das_assets.c:1024-1104
//  59  h capped; the reveal: progress += scale * 4, centred, 0x4d2 when done ................ SlideReveal
//  60  w capped and centred; the source at scroll; on the done frame a short picture zeroes
//      the whole panel first .................................................................. SlideReveal
//  61  the rows copied (doubled in hires); shade_remap_blit row 9 on the left 4 columns and
//      the top 4 rows (8 in hires) ............................................................... SlideReveal
//  gdv_inspect_frame_callback, gdv_cutscene.c:1593-1656
//  62  action 0: a frame that reaches the screen arms the latch (once) ........................ InspectFilm::DecodeNext
//  63  action 2: panel closed -> leave; leaving -> state 0, end; state 2 -> end; armed: blit,
//      the tick, update_inspect_popup_choices; then a key unless [0x80b2c] .................. InspectPopupView::FilmBoundary
//  64  keys: Esc / I leave unless a line is skipped; Enter skips a line; Space: state 1 -> 2,
//      else as Esc ............................................................................... InspectPopupView::FilmKey
//  65  action 3 (palette): handled -- the film's palette never reaches the screen ................ InspectFilm (no palette is applied: the frames are in the game palette)
//  blit_saved_ui_block / blit_opaque_rect, blit_2d.c:1691-1703, 336-353
//  66  the latch's source taken; nothing in mode 1; w x h bytes, doubled rows in hires ......... FilmFrame
//  gdv_decode_timer_isr_noaudio, gdv_cutscene.c:2568-2590 (budget [0x91dbe] = fps << 8, + 0x80 at 0x52b0 Hz, :1312, 2309-2310)
//  67  budget -= dbe; < 0: all frames done -> refill; else refill, decode, frames left - 1 ..... InspectFilm::TimerTick
//  68  "caught up" with the reader -> 0 ...................................................... not ported: the reader is never behind here (the film is read whole)
//  gdv_run_playback_loop / gdv_read_frame_chunk, gdv_cutscene.c:2900-2980, 1381-1400
//  69  db0 = 0x3c00 at the start; the first frame decoded before (gdv_prime_first_frame) ..... InspectFilm::Open
//  70  once: ends when every frame is decoded; looped: the stream re-read with 0x40 (clear) on
//      the first chunk of every pass ............................................................ InspectFilm::DecodeNext, InspectPopupView::FilmRanOut
//

#include "roth_popupdraw.h"
#include "roth_invscreen.h"
#include "roth_gamecore.h"

#include <string.h>

namespace roth
{

namespace
{

uint16_t U16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

} // namespace

//==========================================================================
//
// The window.
//
//==========================================================================

int InspectPopupPainter::RecordW(uint32_t slot)
{
	size_t avail = 0;
	const uint8_t *r = RelocImage(slot, &avail);
	return r != nullptr ? (int16_t)U16(r + 4) : 0;
}

int InspectPopupPainter::RecordH(uint32_t slot)
{
	size_t avail = 0;
	const uint8_t *r = RelocImage(slot, &avail);
	return r != nullptr ? (int16_t)U16(r + 6) : 0;
}

// resolve_reloc_record_fields(&x, &y, slot, 1) + blit_das_image_at_xy: x
// is offset by -(w >> 1) (renderer.c:11109-11110), y not.
void InspectPopupPainter::BlitRecordAt(uint32_t slot, int x, int y)
{
	size_t avail = 0;
	const uint8_t *r = RelocImage(slot, &avail);
	if (r == nullptr) return;
	BlitDasImageAutoScale(r, avail, x, y);
}

void InspectPopupPainter::OpenWindow(const Window &win)
{
	mOpen = true;
	mPage = Page();
	mPagePending = 0;                                                       // :1305
	// :1306-1309
	mCells[0].h = 6;
	for (int i = 1; i < 6; i++) mCells[i].h = 0;
	mCells[0].w = 0x1c;
	mCells[1].w = mCells[2].w = mCells[4].w = mCells[5].w = 0x14;

	int32_t h = win.height;                                                 // :1313
	mWidth0 = win.width;
	mHeight0 = win.height;
	int32_t w = win.width + 0x28;
	int32_t nameW = 0;
	if (win.label != nullptr && win.labelSize > 2 && win.label[0] != 0)     // :1319-1324
	{
		nameW = MeasureControlTextWidth(win.label + 2, win.labelSize - 2);
		if (nameW < 0x38) nameW = 0x38;
		nameW += 0x28;
		if (nameW > w) w = nameW;
	}
	int32_t minW = 0x10e;                                                   // :1327-1333
	h += 0x13;
	if ((uint32_t)screenPitch > 0x17c) minW = 0x154;
	else if ((uint32_t)screenPitch >= 0x168) minW = 0x130;

	mPages.clear();                                                         // :1336-1347
	if (win.flags4 & 2)
	{
		mPages = win.pages;
		if (mPages.size() > 0x20) mPages.resize(0x20);
		if (!mPages.empty())
		{
			if (win.currentPage != nullptr && (uint32_t)mPages.size() <= (uint32_t)*win.currentPage)
				*win.currentPage = (int32_t)mPages.size() - 1;
			minW += 0x28;
			if (minW > w) w = minW;
			h = 0x9f;
		}
	}
	if (win.topicCount != 0 && win.topics != nullptr && win.topicLines != 0 && !win.topics->empty())
	{
		// build_available_choice_menu's box (dialogue_ui.c:1099-1118): only when
		// it laid out a line; otherwise the last one stands.
		const std::vector<TextLine> &t = *win.topics;
		int32_t x0 = t[0].x0, x1 = t[0].x1, hh = t[0].height;
		for (int i = 1; i < win.topicLines && i < (int)t.size(); i++)
		{
			hh += t[(size_t)i].height;
			if (x0 > t[(size_t)i].x0) x0 = t[(size_t)i].x0;
			if (x1 < t[(size_t)i].x1) x1 = t[(size_t)i].x1;
		}
		mChoiceX0 = x0; mChoiceX1 = x1; mChoiceH = hh;
	}
	if (win.topicCount != 0)                                                // :1350-1356
	{
		const int32_t bx = mChoiceX1 - mChoiceX0 + 0x26, by = mChoiceH + 0x12;
		if (bx > w) w = bx;
		if (by > h) h = by;
	}

	int32_t esi = anchorY + 0x41 - h;                                       // :1359-1373
	if (esi < 4) esi = 4;
	const int32_t a0 = (w - 0x28 - mWidth0) >> 1;
	mChoiceY = esi + 6 + (h >> 1);
	int32_t edi = (int32_t)((uint32_t)(screenPitch - w) >> 1);
	edi += 0x24;
	edi += a0;
	const int32_t l20 = ((h - 0x13 - mHeight0) >> 1) + 0xf;
	edi &= ~3;
	const int32_t winX = edi;
	int32_t ebx = edi + (mWidth0 >> 1);
	edi -= a0 + 0x24;
	ebx -= (int32_t)((uint32_t)(screenPitch - 0x50) >> 1);
	mChoiceX = ebx;

	w += 1;                                                                 // :1377
	mEdi = edi; mEsi = esi; mW = w; mH = h;
	mContentX = winX; mContentY = l20 + esi;                                // :1384-1399
	mLoX = edi + 0x24; mLoY = esi + 0xf;
	const int32_t l4 = w - 0x29;
	mHiX = l4; mHiY = h - 0x13;
	const int32_t l30 = w - 1;

	SetScreenPitch(screenPitch);
	SetRect(edi, esi, l30, h, hires);                                       // the window's own rectangle (the shadow lies outside)
	ClearFramebufferRect(edi, esi, l30, h);                                 // :1402

	mCells[0].x = (uint16_t)(edi + 4);                                      // :1405-1407
	const uint16_t x8 = (uint16_t)(edi + 8);
	mCells[1].x = mCells[2].x = mCells[4].x = mCells[5].x = x8;
	mStateAtOpen = 0;
	if (!mPages.empty())                                                    // :1408-1414
	{
		mCells[3].y = (uint16_t)(esi + 0x10);
		mCells[3].h = (uint16_t)(h - 0x16);
		mCells[3].x = (uint16_t)(edi + 0x23);
		mStateAtOpen = 1;
		mCells[3].w = (uint16_t)l4;
	}

	{                                                                       // :1417-1432
		size_t avail = 0;
		const uint8_t *r = RelocImage(0xd0, &avail);
		if (r != nullptr) BlitDasImageAutoScale(r, avail, edi, esi, h - 3);
		r = RelocImage(0xe0, &avail);
		if (r != nullptr) BlitDasImageAutoScale(r, avail, w + edi - 5, esi, h - 3);
	}
	const int32_t ey = h + esi - 3;                                         // :1435-1469
	BlitReloc(edi + 4, esi + 4, 0x110);
	BlitReloc(edi, ey, 0xd8);
	BlitReloc(w + edi - 5, ey, 0xe8);
	int32_t l18 = w - 0x29;
	int32_t edgeX = edi + 0x23;
	BlitReloc(edgeX, esi, 0x300);
	edgeX += 1;
	while ((uint32_t)l18 >= 0x3a)
	{
		const int x = edgeX;
		l18 -= 0x36;
		BlitReloc(x, esi, 0x308);
		edgeX += 0x36;
	}
	BlitReloc(w + edi - 0x3e, esi, 0x310);
	BlitReloc(edi + 0x23, ey, 0xf8, w - 0x28);

	int32_t lc = esi + 0x24;                                                // :1472-1487
	if (win.icon != nullptr && win.iconSize >= 8)
	{
		const int iy = ((0x38 - (int)(int16_t)U16(win.icon + 6)) >> 2) + esi + 4;
		const int ix = ((0x1c - (int)(int16_t)U16(win.icon + 4)) >> 1) + edi + 4;
		BlitItemIcon(win.icon, win.iconSize, ix, iy, (hires ? 0u : 1u) + 1u);
		mCells[0].h = 0x1c;
		mCells[0].y = (uint16_t)(esi + 4);
	}
	if (nameW != 0)                                                         // :1490-1494
	{
		win.label[1] = markerSelected;
		win.label[0] = 1;
		DrawTextAtScreenXy(win.label, win.labelSize, edi + 0x23, esi + 2, 0);
	}
	int32_t l38 = 0;                                                        // :1501-1519
	edi += 0x11;
	if (win.flags4 & 0x40)
	{
		const uint32_t sprite = (win.runtime7 & 0x80) ? 0x98 : 0xa0;
		const int l34 = -(RecordW(sprite) >> 1);
		mCells[1].y = (uint16_t)(lc + l38);
		BlitRecordAt(sprite, l34 + edi, lc + l38);
		mCells[1].h = (uint16_t)RecordH(sprite);
		lc += RecordH(sprite) + 4;
	}
	if (win.topicCount != 0)                                                // :1522-1531
	{
		l38 = 0;
		const int l34 = -(RecordW(0x88) >> 1);
		mCells[2].y = (uint16_t)(lc + l38);
		BlitRecordAt(0x88, l34 + edi, lc + l38);
		mCells[2].h = (uint16_t)RecordH(0x88);
		lc += RecordH(0x88) + 4;
	}
	if (mPages.size() > 1)                                                  // :1534-1537
	{
		mCells[4].y = (uint16_t)(lc + l38);
		mCells[5].y = (uint16_t)(lc + l38 + 0x17);
	}
	mShown = static_cast<const UiCanvas &>(*this);
}

void InspectPopupPainter::CloseWindow()
{
	mOpen = false;
	mPage = Page();
	SetRect(0, 0, 0, 0, hires);
	mShown = static_cast<const UiCanvas &>(*this);
}

//==========================================================================
//
// The film's frames, the buttons, the page arrows.
//
//==========================================================================

void InspectPopupPainter::FilmFrame(const uint8_t *buf, int mode)
{
	if (!mOpen || buf == nullptr) return;
	if (mode == 1) return;                                                  // blit_2d.c:1696
	long a = Addr(mContentX, mContentY);
	for (int r = 0; r < mHeight0; r++)                                      // blit_2d.c:342-352
	{
		const uint8_t *s = buf + (size_t)r * (size_t)mWidth0;
		for (int c = 0; c < mWidth0; c++)
		{
			Put(a + c, s[c]);
			if (mHires) Put(a + VPITCH + c, s[c]);
		}
		a += mHires ? 2 * VPITCH : VPITCH;
	}
}

// draw_reloc_ui_row (dialogue_ui.c:1127-1139).
void InspectPopupPainter::RelocRow(int y, uint32_t slot)
{
	const int x = mEdi + 0x11 - (RecordW(slot) >> 1);
	BlitRecordAt(slot, x, y);
}

void InspectPopupPainter::MarksChanged(int before, int after)
{
	if (!mOpen) return;
	if (!(before & 1) && (after & 1)) RelocRow(mCells[1].y, 0xa0);          // dialogue_ui.c:1576
	if ((before & 1) && !(after & 1)) RelocRow(mCells[1].y, 0x98);          // :1264
	if (!(before & 2) && (after & 2)) RelocRow(mCells[2].y, 0x90);          // :1585
	if ((before & 2) && !(after & 2)) RelocRow(mCells[2].y, 0x88);          // :1268
}

void InspectPopupPainter::PageArrowPressed(bool down)
{
	if (!mOpen) return;
	if (!down) BlitReloc(mCells[4].x, mCells[4].y, 0x120);                  // dialogue_ui.c:1607-1612
	else BlitReloc(mCells[5].x, mCells[5].y, 0x138);                        // :1619-1623
	mPagePending += 1;
}

//==========================================================================
//
// The document page.
//
//==========================================================================

void InspectPopupPainter::BeginModal(int32_t currentPage)
{
	mPage = Page();                                                         // file_config.c:991-996
	mPage.w = mHiX; mPage.h = mHiY;
	mPage.x = mLoX; mPage.y = mLoY;
	if (!mPages.empty()) LoadDocumentPage(currentPage);                     // :998-1003
	mPage.prevX = -1; mPage.sx = 0; mPage.sy = 0; mPage.redraw = 4;         // :1005
}

void InspectPopupPainter::LoadDocumentPage(int32_t currentPage)
{
	if (mPages.size() > 1)                                                  // dialogue_ui.c:1822-1841
	{
		mCells[4].h = 0x13;
		mCells[5].h = 0x13;
		BlitReloc(mCells[4].x, mCells[4].y, currentPage != 0 ? 0x118 : 0x128);
		BlitReloc(mCells[5].x, mCells[5].y, (uint32_t)(mPages.size() - 1) > (uint32_t)currentPage ? 0x130 : 0x140);
	}
	const uint32_t res = (currentPage >= 0 && currentPage < (int)mPages.size()) ? mPages[(size_t)currentPage] : 0;   // :1843
	mPagePending = 0;                                                       // :1844
	if (res != 0)
	{
		// load_das_cache_resource + decode_das_to_padded_buffer.
		std::vector<uint8_t> pic = dbase300 ? dbase300(res) : std::vector<uint8_t>();
		if (pic.size() < 8) Problem("a document page could not be read from DBASE300.DAT");
		else
		{
			const int pw = (int)(int16_t)U16(pic.data() + 4) + 0x10, ph = (int)(int16_t)U16(pic.data() + 6) + 0x10;
			mPage.pw = pw; mPage.ph = ph;
			mPage.pix.assign((size_t)pw * (size_t)ph, 0);
			const int w = pw - 0x10, rows = ph - 0x10;
			const size_t p = DasPayloadAt(pic.data());
			std::vector<uint8_t> px;
			if (p <= pic.size())
			{
				if (pic[0] & 1)
				{
					if (!UiDecodeRle(pic.data() + p, pic.size() - p, (size_t)w * rows, px)) Problem("a document page's run-length stream ends early");
				}
				else
				{
					px.assign((size_t)w * rows, 0);
					for (size_t i = 0; i < px.size() && p + i < pic.size(); i++) px[i] = pic[p + i];
				}
				for (int r = 0; r < rows; r++)
					memcpy(mPage.pix.data() + (size_t)(r + 8) * pw + 8, px.data() + (size_t)r * w, (size_t)w);
			}
			mPage.loaded = true;
			if ((uint32_t)(mPage.w + mPage.sx) > (uint32_t)mPage.pw)            // :1863-1872
			{
				mPage.sx = mPage.pw - mPage.w;
				if (mPage.sx < 0) mPage.sx = 0;
			}
			if ((uint32_t)(mPage.h + mPage.sy) > (uint32_t)mPage.ph)
			{
				mPage.sy = mPage.ph - mPage.h;
				if (mPage.sy < 0) mPage.sy = 0;
			}
		}
	}
	mPage.redraw = 4;                                                       // :1874
}

void InspectPopupPainter::ModalPage(int32_t currentPage, int scale)
{
	if (!mOpen) return;
	if (mPage.loaded && mPagePending != 0)                                  // file_config.c:1014-1020
	{
		mPage.prevX = -1;
		LoadDocumentPage(currentPage);
	}
	if (!mPage.loaded) return;                                              // :1021-1030
	const int changed = (mPage.prevX != mPage.sx) + (mPage.prevY != mPage.sy) + (mPage.redraw != 0 ? 1 : 0);
	if (changed)
	{
		mPage.prevX = mPage.sx;
		mPage.prevY = mPage.sy;
		SlideReveal(scale);
	}
}

void InspectPopupPainter::SlideReveal(int scale)
{
	Page &d = mPage;
	const int imgw = d.pw;
	int x = d.x, y = d.y;
	int h = d.ph, y2 = d.sy;
	if ((uint32_t)h > (uint32_t)d.h) h = d.h;                               // das_assets.c:1035
	if (d.redraw != 0)                                                      // :1036-1046
	{
		d.redraw += scale * 4;
		const int prog = d.redraw;
		if (prog < h)
		{
			const int half = (h - prog) >> 1;
			h = prog;
			y += half; y2 += half;
		}
		else d.redraw = 0x4d2;
	}
	int w = d.w;                                                            // :1050-1054
	if ((uint32_t)w > (uint32_t)imgw)
	{
		w = imgw;
		x += (int)((uint32_t)(d.w - w) >> 1);
	}
	const size_t src = (size_t)imgw * (size_t)y2 + (size_t)d.sx;            // :1055
	if (d.redraw == 0x4d2)                                                  // :1063-1080
	{
		d.redraw = 0;
		if ((uint32_t)w < (uint32_t)d.w || (uint32_t)h < (uint32_t)d.h)
			FillRect(d.x, d.y, d.w, d.h, 0);
	}
	long a = Addr(x, y);                                                    // :1084-1103
	for (int r = 0; r < h; r++)
	{
		const size_t s = src + (size_t)r * (size_t)imgw;
		for (int c = 0; c < w; c++)
		{
			const uint8_t v = s + (size_t)c < d.pix.size() ? d.pix[s + (size_t)c] : 0;
			Put(a + c, v);
			if (mHires) Put(a + VPITCH + c, v);
		}
		a += mHires ? 2 * VPITCH : VPITCH;
	}
	ShadeRemapBlit(x, y, 4, mHires ? h * 2 : h, 9);                         // the left 4 columns
	ShadeRemapBlit(x + 4, y, w - 4, mHires ? 8 : 4, 9);                     // the top 4 rows
}

//==========================================================================
//
// The topic menu.
//
//==========================================================================

void InspectPopupPainter::ApplyUiPaletteRect(int x0, int y0, int x1, int y1, uint32_t level)
{
	if (mA.shade == nullptr) { Problem("no shade table: the topic lines' backing is not drawn"); return; }
	int ecx = y1 + 1;
	int fy0 = y0;
	if (mHires) { ecx += ecx; fy0 += fy0; }
	const int rows = ecx - fy0;
	const int width = (x1 - x0) + 1;
	const size_t base = (size_t)(level & 0x1f) << 8;
	// framebuffer rows are logical rows doubled in hires: address them directly
	long rowAddr = (VORG + fy0) * VPITCH + VORG + x0;
	auto remap = [&](long a) {
		uint8_t v = 0;
		if (!Get(a, v)) return;                                             // not the window's pixel: the world's
		Put(a, mA.shade[base | v]);
	};
	if (rows < 3)                                                           // renderer.c:10229-10237
	{
		int r = rows;
		do {
			int c = width;
			long p = rowAddr;
			do { remap(p); p++; } while (--c > 0);
			rowAddr += VPITCH;
		} while (--r > 0);
		return;
	}
	{                                                                       // :10239-10249
		int c = width - 2;
		long p = rowAddr + 1;
		do { remap(p); p++; } while (--c > 0);
		rowAddr += VPITCH;
	}
	int mid = rows - 2;
	do {
		int c = width;
		long p = rowAddr;
		do { remap(p); p++; } while (--c > 0);
		rowAddr += VPITCH;
	} while (--mid > 0);
	{
		int c = width - 2;
		long p = rowAddr + 1;
		do { remap(p); p++; } while (--c > 0);
	}
}

void InspectPopupPainter::PresentChoices(const std::vector<TextLine> &lines, int count, int scale)
{
	if (!mOpen) { Present(); return; }
	const int top = mChoiceY - (mChoiceH >> 1);                             // dialogue_ui.c:856
	const int ox = mChoiceX;
	if (choiceReveal < 0x20)                                                // :862-866
	{
		choiceReveal += scale;
		if (choiceReveal > 0x20) choiceReveal = 0x20;
	}
	const int x0 = mChoiceX0 + ox, x1 = mChoiceX1 + ox;                     // :858-861, 867-870: the saved block
	const Saved saved = SaveRegion(x0 & ~3, top, (((x1 + 1 + 3) & ~3) - (x0 & ~3)), mChoiceH + 1);
	int y = top;
	for (int i = 0; i < count && i < (int)lines.size(); i++)                // :877-889
	{
		const TextLine &t = lines[(size_t)i];
		ApplyUiPaletteRect(t.x0 + ox, y, t.x1 - 2 + ox, t.height + y - 1, (uint32_t)(choiceReveal >> 1));
		DrawTextToBuffer(t.text.data(), t.text.size(), Addr(ox, y), mHires ? 1u : 0u);
		y += t.height;
	}
	mShown = static_cast<const UiCanvas &>(*this);
	mPresents++;
	RestoreRegion(saved);                                                   // mark_overlay_dirty_rects (video_display.c:578-582)
}

void InspectPopupPainter::Present()
{
	mShown = static_cast<const UiCanvas &>(*this);
	mPresents++;
}

void InspectPopupPainter::Words(int out[12]) const
{
	const int v[12] = { mContentX, mContentY, mWidth0, mHeight0, mEdi, mEsi, mW, mH, mLoX, mLoY, mHiX, mHiY };
	for (int i = 0; i < 12; i++) out[i] = v[i];
}

//==========================================================================
//
// The pointer.
//
//==========================================================================

int InspectPopupPainter::HitTest(int mx, int my, int mode, bool busy, bool dragging, const std::vector<TextLine> &lines, int lineCount, int *row) const
{
	if (row != nullptr) *row = -1;
	if (busy) return 0x27;                                                  // dialogue_ui.c:551-552
	if (mode == 0 && dragging) return 4;                                    // :610-611
	if (mode == 1)                                                          // :613-644
	{
		int edx = mChoiceY - (mChoiceH >> 1);
		const int adj = mx - mChoiceX;
		for (int i = 0; i < lineCount && i < (int)lines.size(); i++)
		{
			const TextLine &t = lines[(size_t)i];
			if (adj >= t.x0 && adj <= t.x1 && my >= edx && (uint32_t)(my - edx) <= (uint32_t)t.height)
			{
				if (row != nullptr) *row = i;
				return i + 7;
			}
			edx += t.height;
		}
	}
	for (int i = 0; i < 6; i++)                                             // :647-659
	{
		const PopupCell &c = mCells[i];
		if (c.h == 0) continue;
		const int32_t cy = my - (int32_t)c.y;
		if ((uint32_t)cy >= (uint32_t)c.h) continue;
		const int32_t cx = mx - (int32_t)c.x;
		if ((uint32_t)c.w <= (uint32_t)cx) continue;
		if (i == 1 && busy) return 0;
		return i + 1;
	}
	return 0;
}

//==========================================================================
//
// The film.
//
//==========================================================================

bool InspectFilm::Open(const uint8_t *gdv, size_t size, bool hold)
{
	mError.clear();
	mData = gdv;
	mSize = size;
	mLoop = !hold;
	mArmed = false;                                                         // file_config.c:910, 968
	mDecodes = 0;
	mRecords.clear();
	// The stream flags: 0x1090 / 0x1890, | 8 without hold (file_config.c:929-930);
	// only the loop reaches what is decoded here.
	if (!mDec.Open(gdv, size, hold ? 0x1090u : 0x1098u)) { mError = mDec.Error(); return false; }
	if (mDec.Version() != 1) { mError = "a close-up film that is not 8-bit"; return false; }
	mAudioBytes = mDec.AudioBytesPerRecord();
	// The records, back to back after the header and palette (gdv_read_frame_chunk).
	size_t pos = 0x18 + 0x300;
	const uint32_t maxChunk = U16(gdv + 0x10);
	for (int i = 0; i < mDec.FrameCount(); i++)
	{
		if (pos + mAudioBytes + 8 > size) { mError = "the film ends in the middle of a record"; return false; }
		mRecords.push_back(pos);
		const uint8_t *chunk = gdv + pos + mAudioBytes;
		uint32_t payload = 0;
		if (maxChunk != 0)
		{
			if (U16(chunk) != 0x1305) { mError = "a chunk without its 0x1305 magic"; return false; }
			payload = U16(chunk + 2);
		}
		pos += mAudioBytes + 8 + payload;
	}
	if (mRecords.empty()) { mError = "a film with no frames"; return false; }
	mNext = 0;
	mDbe = (int)(uint16_t)((uint32_t)U16(gdv + 8) << 8);                    // :1312
	if (U16(gdv + 0xc) == 0x52b0) mDbe = (uint16_t)(mDbe + 0x80);           // :2309-2310
	mDba = (int)(int16_t)(uint16_t)(mDec.FrameCount() - 2);                 // gdv_preload_frame_window :1520-1522
	DecodeNext();                                                           // gdv_prime_first_frame (no frame count down)
	mDb0 = 0x3c00;                                                          // gdv_run_playback_loop :2907
	return true;
}

bool InspectFilm::DecodeNext()
{
	if (mNext >= mRecords.size()) mNext = 0;
	const uint8_t *chunk = mData + mRecords[mNext] + mAudioBytes;
	const uint32_t payload = U16(chunk + 2);
	mChunk.assign(chunk, chunk + 8 + payload);
	if (mNext == 0 && mDecodes != 0) mChunk[4] |= 0x40;                     // a loop's first chunk (gdv_cutscene.c:1395, 2940)
	mNext++;
	if (!mDec.DecodeChunk(mChunk.data())) { mError = mDec.Error(); return false; }
	mDecodes++;
	// gdv_emit_decoded_frame -> the callback's action 0 (gdv_cutscene.c:1597-1600):
	// the decode buffer is handed over unless the frame does not reach the
	// screen (:1672-1673).
	if (mDec.FrameShown() && !mArmed) mArmed = true;
	if (mLoop && mNext >= mRecords.size()) mDba += (int)mRecords.size();   // the reader's re-read (gdv_cutscene.c:2950-2965)
	return true;
}

bool InspectFilm::TimerTick()
{
	mDb0 = (int16_t)(uint16_t)(mDb0 - mDbe);                                // gdv_cutscene.c:2571-2572
	if (mDb0 >= 0) return false;
	if (mDba < 0) { mDb0 = (int16_t)(uint16_t)(mDb0 + 0x3c00); return false; }   // :2576
	mDb0 = (int16_t)(uint16_t)(mDb0 + 0x3c00);                              // :2583
	if (!DecodeNext()) return false;
	mDba = (int16_t)(uint16_t)(mDba - 1);                                   // :2586
	return true;
}

//==========================================================================
//
// The popup's life.
//
//==========================================================================

bool InspectPopupView::Begin(const uint8_t *gdv, size_t gdvSize, const uint8_t *icon, size_t iconSize, uint8_t *label, size_t labelSize, uint16_t frameTick)
{
	mPhase = NONE;
	if (mScreen == nullptr || mUi == nullptr || mCore == nullptr || !mScreen->PopupUp()) return false;
	if (gdv == nullptr || gdvSize < 0x18) return false;
	InspectPopupPainter::Window w;
	w.width = (int16_t)U16(gdv + 0x14);                                     // file_config.c:944-946
	w.height = (int16_t)U16(gdv + 0x16);
	w.flags4 = mScreen->PopupFlags();
	w.runtime7 = mScreen->PopupRuntimeAtOpen();
	w.pages = mScreen->PopupPages();
	w.currentPage = &mScreen->g_inspect_current_page;
	w.topicCount = (uint32_t)mUi->TopicCount();
	w.topics = &mUi->TopicLines();
	w.topicLines = mUi->g_choice_line_count;
	w.label = label;
	w.labelSize = labelSize;
	w.icon = icon;
	w.iconSize = iconSize;
	painter.OpenWindow(w);
	if (w.topicCount != 0 && w.topicLines != 0) painter.choiceReveal = 2;   // build_available_choice_menu (dialogue_ui.c:1114)
	mState = painter.StateAtOpen();
	mHold = (w.flags4 & 1) != 0;
	mInfo = 0;
	mPress = 0;
	mBoundaries = 0;
	mLastTick = frameTick;
	mLastMode = mUi->g_choice_interaction_mode;
	if (!film.Open(gdv, gdvSize, mHold))
	{
		painter.Problem("the close-up film: " + film.Error());
		Close();
		return false;
	}
	mPhase = FILM;
	painter.Present();
	return true;
}

bool InspectPopupView::FilmBoundary(uint16_t frameTick)
{
	if (mPhase != FILM) return false;
	mBoundaries++;
	film.TimerTick();
	if (mScreen->g_inventory_panel_open == 0) mScreen->g_popup_exit_request = 1;   // gdv_cutscene.c:1611-1612
	if (mScreen->g_popup_exit_request != 0) { mState = 0; return false; }  // :1613-1616
	if (mState == 2) return false;                                          // :1617-1618
	if (film.Armed())                                                       // :1619-1630
	{
		painter.FilmFrame(film.Pixels(), mUi->g_choice_interaction_mode);
		const int scale = (int16_t)(uint16_t)(frameTick - mLastTick);
		film.TakeLatch();
		mLastTick = frameTick;
		if (scale != 0) UpdateChoices(scale);
	}
	return true;
}

bool InspectPopupView::KeyWanted() const
{
	return mScreen != nullptr && mScreen->g_ui_close_request == 0;          // gdv_cutscene.c:1632; file_config.c (the drain reads always)
}

bool InspectPopupView::FilmKey(uint8_t k)
{
	if (k == 0x1c) { mUi->TryInterruptDialogueVoice(); return true; }       // gdv_cutscene.c:1640-1642
	if (k == 0x39 && mState == 1) { mState = 2; return true; }              // :1643-1647
	if (k != 1 && k != 0x17 && k != 0x39) return true;
	if (mUi->TryInterruptDialogueVoice() != 0) return true;                 // :1652-1653
	mScreen->g_popup_exit_request = 1;                                      // :1654
	return false;
}

bool InspectPopupView::FilmRanOut() const
{
	return mPhase == FILM && !film.Looping() && film.Remaining() == -1;     // gdv_cutscene.c:2974
}

void InspectPopupView::FinishFilm(bool endOfStream, uint16_t frameTick)
{
	if (mPhase != FILM) return;
	if (film.Armed())                                                       // file_config.c:982
	{
		painter.FilmFrame(film.Pixels(), mUi->g_choice_interaction_mode);
		film.TakeLatch();
	}
	bool wantLoop = mHold;                                                  // :976-977
	bool ended = endOfStream;
	if (mState != 0) { wantLoop = true; ended = false; }
	if (ended || !wantLoop) { Close(); return; }                            // :989
	painter.BeginModal(mScreen->g_inspect_current_page);                    // :991-1005
	mState = 3;
	mExitAcc = 0;
	mLastTick = frameTick;                                                  // :1007
	mPhase = MODAL;
}

void InspectPopupView::ModalPassBegin(uint16_t frameTick)
{
	if (mPhase != MODAL) return;
	const int scale = (int16_t)(uint16_t)(frameTick - mLastTick);           // file_config.c:1010-1012
	mLastTick = frameTick;
	painter.ModalPage(mScreen->g_inspect_current_page, scale);              // :1014-1030
	InspectPopupPainter::Page &d = painter.Desc();
	if (mInfo == 1) { mDragSx = d.sx; mDragSy = d.sy; mInfo = 2; }          // :1032-1036
	if (mInfo != 0)                                                         // :1037-1061
	{
		if ((mHeld & 3) == 0) mInfo = 0;
		else
		{
			const int rangeX = d.pw - d.w;
			const int dy = mMouseY - mDragY;
			if (rangeX > 0)
			{
				int v = mDragSx - (mMouseX - mDragX);
				if (v < 0) v = 0;
				d.sx = v;
				if (rangeX < v) d.sx = rangeX;
			}
			const int rangeY = d.ph - d.h;
			if (rangeY > 0)
			{
				int v = mDragSy - dy;
				if (v < 0) v = 0;
				d.sy = v;
				if (rangeY < v) d.sy = rangeY;
			}
		}
	}
	UpdateChoices(scale);                                                   // :1062-1064
}

bool InspectPopupView::ModalKey(uint8_t k)
{
	if (mPhase != MODAL) return false;
	InspectPopupPainter::Page &d = painter.Desc();
	const int mode = mUi->g_choice_interaction_mode;
	if (k == 1 || k == 0x17 || k == 0x39)                                   // file_config.c:1071-1073
	{
		if (mUi->TryInterruptDialogueVoice() == 0) mExitAcc++;
		return false;
	}
	if (k == 0x1c)                                                          // :1074-1081
	{
		const Dbase100InterpState &s = mCore->Interp().State();
		if (s.g_choice_selected_index == -1 || (s.g_move_freeze_gate != DialogueUi::GATE_MENU && mode != 1))
		{
			if (mUi->TryInterruptDialogueVoice() == 0) mExitAcc++;
		}
		else mUi->ChoiceAcceptSelected();
		return false;
	}
	if (k == 0x48)                                                          // :1084-1091
	{
		if (mode != 0) { mUi->ChoiceSelectPrev(); return false; }
		if (mState == 0) return false;
		if (d.sy > 0) d.sy -= 8;
		if (d.sy < 0) d.sy = 0;
		return true;
	}
	if (k == 0x4b)                                                          // :1092-1097
	{
		if (mState == 0) return false;
		if (d.sx > 0) d.sx -= 8;
		if (d.sx < 0) d.sx = 0;
		return true;
	}
	if (k == 0x4d)                                                          // :1098-1106
	{
		if (mState == 0) return false;
		const int range = d.pw - d.w;
		if (range > 0)
		{
			if (d.sx < range) d.sx += 8;
			if (range < d.sx) d.sx = range;
		}
		return true;
	}
	if (k == 0x50)                                                          // :1107-1117
	{
		if (mode != 0) { mUi->ChoiceSelectNext(); return false; }
		if (mState == 0) return false;
		const int range = d.ph - d.h;
		if (range > 0)
		{
			if (d.sy < range) d.sy += 8;
			if (range < d.sy) d.sy = range;
		}
		return true;
	}
	return false;
}

bool InspectPopupView::ModalPassEnd()
{
	if (mPhase != MODAL) return false;
	if (mScreen->g_popup_exit_request != 0) mExitAcc++;                     // file_config.c:1120-1121
	if (mScreen->g_inventory_panel_open == 0) mExitAcc++;
	if (mExitAcc == 0) return false;
	Close();
	return true;
}

void InspectPopupView::Close()
{
	painter.CloseWindow();                                                  // file_config.c:1131
	if (mScreen != nullptr && mScreen->PopupUp()) mScreen->PopupFinish();   // :1138-1142
	mPhase = NONE;
	mInfo = 0;
	mPress = 0;
}

//==========================================================================
//
// update_inspect_popup_choices, the pointer, the actions.
//
//==========================================================================

void InspectPopupView::UpdateChoices(int scale)
{
	if (mScreen == nullptr || mUi == nullptr || mCore == nullptr) return;
	const int marks0 = mScreen->PopupMarks();
	const int mode0 = mUi->g_choice_interaction_mode;
	mScreen->PopupFrameBegin();                                             // dialogue_ui.c:1257-1271
	if (mode0 == 2 && mUi->g_choice_interaction_mode == 1)                  // a rebuilt menu (:1258; build :1099-1118)
	{
		if (mUi->g_choice_line_count != 0) painter.choiceReveal = 2;
	}
	painter.MarksChanged(marks0, mScreen->PopupMarks());
	Pointer();                                                              // :1272
	mScreen->PopupFrameEnd();                                               // :1273-1276
	if (mScreen->g_ui_close_request != 0) return;                           // :1277-1278
	// render_text_ui (dialogue_ui.c:973-995): which panel.
	const Dbase100InterpState &s = mCore->Interp().State();
	const int mode = mUi->g_choice_interaction_mode;
	bool choices = false;
	if (s.g_active_dialogue_context == 0) choices = mode == 1;
	else if (s.g_move_freeze_gate != DialogueUi::GATE_MENU) choices = mode == 1;
	if (choices) painter.PresentChoices(mUi->TopicLines(), mUi->g_choice_line_count, scale);
	else painter.Present();
	if (textUi) textUi(scale);
	mLastMode = mode;
}

void InspectPopupView::Pointer()
{
	const Dbase100InterpState &s = mCore->Interp().State();
	const bool busy = s.g_dialogue_busy_flag != 0;
	int code = 0;                                                           // input.c:1264-1268
	if (mPress & 2) code = 2;
	if (mPress & 1) code += 1;
	if (code == 0) code = mHeld & 3;
	const int mode = mUi->g_choice_interaction_mode;
	int row = -1;
	// hit_test_dialogue_ui_action, with its marker writes on the shown topics
	// (dialogue_ui.c:613-638; nothing while a line is spoken, :551-552).
	auto hit = [&]() {
		const int a = painter.HitTest(mMouseX, mMouseY, mode, busy, mInfo != 0, mUi->TopicLines(), mUi->g_choice_line_count, &row);
		if (mode == 1 && !busy) mUi->TopicPointerMarker(row);
		return a;
	};
	if (code == 0)                                                          // :1269-1309, the hover
	{
		hit();
		mPress = 0;
		return;
	}
	if (mPress == 0) return;                                                // :1311-1312: held only
	const int act = hit();
	mPress = 0;
	if (act == 0x27) { mUi->DialogueVoiceForceEnd((uint32_t)mMouseX, (uint32_t)mMouseY); return; }   // :1322-1334
	if (act != 0) Dispatch(act, code);                                      // :1335-1347
}

void InspectPopupView::Dispatch(int action, int code)
{
	(void)code;
	const int marks0 = mScreen->PopupMarks();
	switch (action)
	{
	case 1: mScreen->PopupClose(); break;                                   // dialogue_ui.c:1566
	case 2: mScreen->PopupMagnifier(); break;                               // :1569-1578
	case 3:                                                                 // :1580-1593
		mScreen->PopupToggleTopics();
		painter.choiceReveal = 2;
		break;
	case 4:                                                                 // :1596-1602
		if (mState == 1) { mState = 2; break; }
		if (mState != 3) break;
		mDragX = mMouseX; mDragY = mMouseY;
		mInfo += 1;
		break;
	case 5:                                                                 // :1605-1614
	{
		const int32_t p = mScreen->g_inspect_current_page;
		mScreen->PopupPageUp();
		if (mScreen->g_inspect_current_page != p) painter.PageArrowPressed(false);
		break;
	}
	case 6:                                                                 // :1617-1626
	{
		const int32_t p = mScreen->g_inspect_current_page;
		mScreen->PopupPageDown();
		if (mScreen->g_inspect_current_page != p) painter.PageArrowPressed(true);
		break;
	}
	default:
		if (action >= 7 && action <= 0x16) mScreen->PopupTopic(action - 7);   // :1629
		break;
	}
	painter.MarksChanged(marks0, mScreen->PopupMarks());
}

} // namespace roth
