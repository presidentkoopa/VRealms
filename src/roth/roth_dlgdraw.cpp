//
// Realms of the Haunting's dialogue text on screen. See roth_dlgdraw.h.
//
// THE CHECKLIST (porting rule 2). ROTH.C file:line -> here.
//
//  dbase100_open_dialogue_window, dialogue_ui.c:1189-1212 (the panel rectangle; the
//  rest of the routine is roth_dialogue's)
//   1  min x0 (from record 0), max x1, sum of (height + 4) - 4 over the records ......... RenderDialogueTextPanel
//   2  with a topic menu up (mode != 0) both x shifted by 0x83add - 0x28; else 0x83add = 0x28 ... RenderDialogueTextPanel (originX)
//  render_dialogue_text_panel, dialogue_ui.c:814-848
//   3  x shift = 0x83add - 0x28; top = (screen h - panel h) >> 1, unsigned ............... RenderDialogueTextPanel
//   4  the saved region (x0, top, x1 - x0 + 1, h + 1) ....................................... RenderDialogueTextPanel (RegionX..)
//   5  per record: apply_ui_palette_rect over its rectangle (param != 1) ........ not drawn: it remaps the world behind
//   6  per record: draw_text_to_buffer(text, fb + shift + y * pitch, pitch, hires); y doubled
//      in hires; y += height + 4 ................................................................... RenderDialogueTextPanel
//   7  the dirty rect ..................................................................... not ported: VGA bookkeeping
//  update_dialogue_choice_highlight, dialogue_ui.c:255-266 (the hit areas; the rest is roth_dialogue's)
//   8  x' = mx + 0x28 - 0x83add; per record: top <= my <= top + height, x0 <= x' <= x1,
//      all signed; top += height + 4 ................................................................ ChoiceRowAt
//  render_text_ui's drawing, dialogue_ui.c:1016-1047 (the sequencer half is roth_dialogue's)
//   9  x0 = record x0, w = x1 - x0, h = height + 1 ........................................... RenderTextUiSegment
//  10  y = anchor y + 0x54 with the inventory open, else view y + 4 + view h; when
//      screen h < h + y + 3 (unsigned) y = screen h - h - 3 ........................................ RenderTextUiSegment
//  11  the saved region (x0, y, w, h); the fade rect (param != 1) .......... the rect not drawn: the world behind
//  12  draw_text_to_buffer(text, fb + y * pitch, pitch, hires); y doubled in hires .................. RenderTextUiSegment
//  render_active_timed_message, dialogue_ui.c:900-964
//  13  the timer and the expiry (:902-906) ............................................ roth_dialogue (TickTextUi)
//  14  <= 1 line: x0 / x1 / h + 1 of record 0; y = anchor y - (h + 1) with the inventory
//      open, else view y + 2; the region; the text at fb + y * pitch .......................... RenderActiveTimedMessage
//  15  >= 2 lines: min x0, max x1, h = (record 0's height + 1) * count; the region at y 0x20;
//      each record at y 0x20 + 0xb * i (0x40 / 0x16 framebuffer rows in hires) ............... RenderActiveTimedMessage
//

#include "roth_dlgdraw.h"

namespace roth
{

void DialogueTextPainter::Begin(int x, int y, int w, int h)
{
	mRx = x; mRy = y; mRw = w; mRh = h;
	SetScreenPitch(screenPitch);
	SetRect(x - 6, y - 6, w + 12, h + 12, hires);
}

void DialogueTextPainter::RenderDialogueTextPanel(const std::vector<TextLine> &segs, int count, int originX)
{
	if (count > (int)segs.size()) count = (int)segs.size();
	mSegs = segs;
	mCount = count;
	mOriginX = originX;
	if (count <= 0) { Begin(0, 0, 0, 0); return; }
	// :1191-1203, the panel rectangle as the open worked it out.
	int32_t x0 = segs[0].x0, x1 = segs[0].x0, h = 0;
	for (int i = 0; i < count; i++)
	{
		h += segs[(size_t)i].height + 4;
		if (x0 > segs[(size_t)i].x0) x0 = segs[(size_t)i].x0;
		if (x1 < segs[(size_t)i].x1) x1 = segs[(size_t)i].x1;
	}
	h -= 4;
	mPanelH = h;
	const int32_t shift = originX - 0x28;                                   // :816 (and :1204-1207)
	x0 += shift;
	x1 += shift;
	const int32_t top = (int32_t)((uint32_t)(screenHeight - h) >> 1);      // :817
	Begin(x0, top, x1 - x0 + 1, h + 1);                                     // :822-825
	int32_t y = top;
	for (int i = 0; i < count; i++)
	{
		const TextLine &t = segs[(size_t)i];
		DrawTextToBuffer(t.text.data(), t.text.size(), Addr(shift, y), hires ? 1u : 0u);   // :839-841
		y += t.height + 4;                                                  // :842-844
	}
}

int DialogueTextPainter::ChoiceRowAt(int mx, int my) const
{
	const int32_t xx = (mx + 0x28) - mOriginX;                              // :256
	int32_t top = (int32_t)((uint32_t)(screenHeight - mPanelH) >> 1);      // :257
	for (int i = 0; i < mCount && i < (int)mSegs.size(); i++)
	{
		const TextLine &t = mSegs[(size_t)i];
		if (my >= top && my <= t.height + top && xx >= t.x0 && xx <= t.x1) return i;
		top += t.height + 4;
	}
	return -1;
}

void DialogueTextPainter::RenderTextUiSegment(const TextLine &seg, bool inventoryOpen, int anchorY, int viewY, int viewH)
{
	mSegs.clear(); mCount = 0;
	const int32_t x0 = seg.x0;
	const int32_t w = seg.x1 - x0;                                          // :1019
	const int32_t h = seg.height + 1;                                       // :1020
	int32_t y = inventoryOpen ? anchorY + 0x54 : viewY + 4 + viewH;         // :1022-1025
	if ((uint32_t)screenHeight < (uint32_t)(h + y + 3)) y = screenHeight - h - 3;   // :1027-1028
	Begin(x0, y, w, h);                                                     // :1030-1035
	DrawTextToBuffer(seg.text.data(), seg.text.size(), Addr(0, y), hires ? 1u : 0u);   // :1042-1047
}

void DialogueTextPainter::RenderActiveTimedMessage(const std::vector<TextLine> &lines, int count, bool inventoryOpen, int anchorY, int viewY)
{
	mSegs.clear(); mCount = 0;
	if (lines.empty() || count <= 0) { Begin(0, 0, 0, 0); return; }
	if (count > (int)lines.size()) count = (int)lines.size();
	if (count <= 1)                                                         // :911-933
	{
		const TextLine &t = lines[0];
		const int32_t x0 = t.x0, x1 = t.x1, eax = t.height + 1;
		const int32_t y0 = inventoryOpen ? anchorY - eax : viewY + 2;
		Begin(x0, y0, x1 - x0, eax);
		DrawTextToBuffer(t.text.data(), t.text.size(), Addr(0, y0), hires ? 1u : 0u);
		return;
	}
	int32_t x0 = lines[0].x0, x1 = lines[0].x1;                             // :937-944
	for (int i = 1; i < count; i++)
	{
		if (x0 > lines[(size_t)i].x0) x0 = lines[(size_t)i].x0;
		if (x1 < lines[(size_t)i].x1) x1 = lines[(size_t)i].x1;
	}
	const int32_t h = (lines[0].height + 1) * count;                        // :945
	Begin(x0, 0x20, x1 - x0, h);
	for (int i = 0; i < count; i++)                                         // :955-963
		DrawTextToBuffer(lines[(size_t)i].text.data(), lines[(size_t)i].text.size(), Addr(0, 0x20 + 0xb * i), hires ? 1u : 0u);
}

} // namespace roth
