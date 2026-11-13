//
// Realms of the Haunting's message boxes, drawn as the original draws them.
// See roth_menudraw.h.
//
// THE CHECKLIST (porting rule 2). ROTH.C file:line -> here.
//
//  show_message_box, menu_hud_ui.c:1284-1902
//   1  :1292-1306 save / freeze the UI state, end_screen_draw ............. not ported: the host's (no drawing)
//   2  :1309-1314 count, boxh = count * 0xb + 0x17, box_y = (h - boxh) >> 1 unsigned ... Open
//   3  :1320-1349 title (or a lone 0x0a), body, rows: resolve_dbase100_text(0x50); a
//      0x80 row with its toggle set takes `alt`; an id 0: " " with 0x400, else empty;
//      every text's last byte becomes 0x0a; a 0 ends ...................................... Open
//   4  :1352 g_timed_message_color = g_default_message_color[5] ............................ Open (defaultColor5)
//   5  :1355-1356 layout_timed_message_text(pitch, entry count as the line budget) ......... Open (roth_dialogue's port)
//   6  :1359-1369 widest / narrowest row (lines 1..n-1), the title's width - 0x16 .......... Open
//   7  :1371-1415 flag 0x10: slider rows full width, others - 0x26; the slider labels'
//      x re-embedded (the stale carry of [ebp-0x22] kept) ................................... Open
//   8  :1422-1458 flag 1: the fixed 0x120 x 0x50 box: anchor (flag 0x100 centred, else
//      compute_viewport_half_extents), row offset 0x2e, the save-under ........................ Open
//   9  :1438-1443 the shadow (and with 0x200 the zoom) unless flag 0x40 ........ not drawn: ShadowRect (a blend of what lies around)
//  10  :1444 ICONS.ALL 0x328 at the box; :1445-1446 0x20 at (3, 0x22) with 0x80; :1447-1448 the
//      icon word at (4, 4) .................................................................... Open
//  11  :1449-1454 flag 2: the panel snapshot (0x811ac, double deref) at (0x24, 0x10) ......... Open (panelSnapshot; the engine has none: see report)
//  12  :1455-1456 else unless flag 4: 0x360 at (0x27, 0xf) .................................. Open
//  13  :1458 draw_scroll_indicators ................................................................ Open
//  14  :1459-1510 the sized box: width content + 0x41 centred, row offset 0xd, the
//      save-under, shadow / zoom, the 0xd0 column with its height patched to boxh - 3,
//      icon, 0xd8 at the bottom, the top edge 0x2b8 + 0x2a0 x n + the remainder + 0x318,
//      the interior cleared, 0x2a8 per row, 0x320 / 0x2c8 stretched (packed EBX) / 0x2b0 ... Open
//  15  :1513-1531 the title at (box_x + 0x23, box_y + 2) in the marker colour; the body at
//      x + 0x58, y content - 0xe; the line counts .............................................. Open
//  16  :1538-1542 dirty rects .............................................. not ported: VGA bookkeeping
//  17  :1544 render_menu_entry_list ................................................................ Open
//  18  :1545-1548 sel = word[0xa]; the click flags cleared; the tick ........................ Open (sel kept for the loop)
//  19  :1550-1557 flag 0x20: draw only, the handle back ................... not ported: the host decides what is shown
//  20  :1561-1570 flag 0x200: the inner save-under and the shade block ...................... Open (mZoomSave)
//  21  :1585-1657 the loop's INPUT half (keys, mouse, hit, hold repeat) ....... not here: roth_menus decides the selection;
//      the hit test it reads is HitTest / LoopHit
//  22  :1658-1678 a scroll: the indicators, the list area cleared (list_x + row offset, list_y,
//      0x8c x 0x38), the list forced to re-render ................................................ Pass
//  23  :1680-1681 with the zoom block: put back each pass ............................................. Pass
//  24  :1683-1699 a new selection: every row's colour byte to the default, the selected
//      row's to the marker, the thumbnail asked for, the list re-rendered ........................ Pass
//  25  :1701-1716 flag 4: the selected slot's thumbnail (read_savegame_thumbnail(scroll + sel))
//      at (0x24, 0x10) when it has one .............................................................. Pass (thumbnail)
//  26  :1718-1732 the zoom fade: ticks counted from 2; >= 0x20 the block is dropped, else the
//      screen block captured again (blit_save_region) and shaded by row 0x20 - ticks .............. Pass
//  27  :1733-1737 present ................................................. not ported: the host presents
//  28  :1739-1877 results, activation, toggles, text input, sliders, settings ...... not here: roth_menus (and not ported there:
//      the text input row, the sliders, the toggles, the gallery)
//  29  :1880-1899 teardown: the save-under put back, sel into word[0xa] ............ the host drops the picture; roth_menus keeps sel
//  render_menu_entry_list, menu_hud_ui.c:872-937
//  30  type 0 with a label table: the gallery's row ................. not ported: no box shown here has one (Problems says so)
//  31  type 0x31 / 0x32: "%D: %s" of slot scroll + idx (the name; '~' -> the row's own
//      text at +5; empty -> text 0x33) at anchor + 0x54 + row offset, flags 4;
//      rec[0] / rec[1] = anchor + 0x54 / 0xd4 ............................................. RenderMenuEntryList
//  32  else: a 0x10 row's value bar (draw_menu_value_bar at row offset + rec[0], the
//      label's colour, value & 0x1ff), then the row text at the row offset, flags 4 ......... RenderMenuEntryList
//  33  y += 0xb per row; the dirty rect ....................... RenderMenuEntryList (the rect not ported)
//  draw_menu_value_bar, menu_hud_ui.c:803-844
//  34  fill = value * 0x23 >> 8, 1 when value != 0, at most 0x23; lit 0x350 / 0x340, unlit
//      0x358 / 0x348 by the marker; each into a 0x24-pitch buffer; 9 rows copied (doubled
//      in hires) ...................................................................................... DrawMenuValueBar
//  draw_scroll_indicators, menu_hud_ui.c:782-795
//  35  only with a range: 0xa8 / 0xb0 at (0x116, 0x12), 0xb8 / 0xc0 at (0x116, 0x31) ............. DrawScrollIndicators
//  hit_test_ui_element, menu_hud_ui.c:46-87
//  36  rows: xbase < my < xbase + 0xb (unsigned), x0 < mx - rowoff < x1; xbase += 0xb ............ HitTest
//  37  the close box (4..0x20 both ways, exclusive) or with 0x80 the title box -> -1;
//      0x800 the body box -> -9; 8 the arrows -> -6 / -7 ............................................. HitTest
//  38  :1595-1596 -6 at scroll 0, -7 at scroll == range -> 0 .............................................. LoopHit
//
//  THE DESCRIPTORS: the EXE image's bytes 0x71900-0x71d5f, read out of the running
//  original by tools/menudraw_test (its "X" line) and transcribed below; the test
//  compares every field with those bytes.
//

#include "roth_menudraw.h"
#include "roth_dialogue.h"

#include <stdio.h>
#include <string.h>

namespace roth
{

const std::vector<MsgBoxDesc> &MsgBoxDescriptors()
{
	static const std::vector<MsgBoxDesc> d = {
		{ 0x71914, 72, 0, 0x380, 0, 0, 0, { { 78, 0, 0x400005 }, { 79, 0, 0x400006 }, { 80, 0, 0x400007 } } },           // the movie clips
		{ 0x7194c, 44, 0, 0x2f8, 0, 0, 0, { { 54, 0, 0 } } },
		{ 0x7196c, 44, 0, 0x2f8, 0, 0, 0, { { 74, 0, 0x11000100 }, { 55, 0, 0x66 }, { 56, 0, 0x64 }, { 57, 0, 0x65 },
			{ 58, 0, 0 }, { 59, 0, 0 }, { 60, 0, 0 } } },
		{ 0x719e0, 48, 0, 0x2e8, 0, 0, 0, { { 46, 0x2e, 0x18000080 } } },
		{ 0x71a00, 43, 0, 0x2d0, 1, 0, 0, { { 43, 0, 0 }, { 17, 0, 0 } } },                                           // quit?
		{ 0x71a2c, 9, 0, 0x370, 1, 0, 0, { { 53, 0, 0 }, { 17, 0, 0 } } },                                            // new game?
		{ 0x71a58, 47, 0, 0x2d8, 0, 0, 0, { { 18, 0, 0 }, { 19, 0, 0 }, { 44, 0, 0 }, { 48, 0, 0 } } },                   // settings (in game)
		{ 0x71a9c, 47, 0, 0x2d8, 0, 0, 0, { { 18, 0, 0 }, { 19, 0, 0 }, { 48, 0, 0 } } },                               // settings (front end)
		{ 0x71ad4, 19, 0, 0x2e0, 0, 0, 0, { { 28, 0x1d, 0x80000001 }, { 30, 0x1f, 0x80000002 }, { 32, 0x21, 0x80000003 }, { 34, 0x23, 0x80000004 } } },
		{ 0x71b18, 18, 0, 0x2f0, 0, 0, 0, { { 24, 0, 0x14000080 }, { 26, 0, 0x14000080 }, { 27, 0, 0x14000080 }, { 25, 0, 0x14000080 } } },
		{ 0x71b6d, 10, 62, 0x368, 0, 0, 0, { { 9, 0, 0x2000002 }, { 14, 0, 0 }, { 15, 0, 0 }, { 47, 0, 0x2000001 }, { 43, 0, 0x2000000 } } },   // options
		{ 0x71bbd, 10, 62, 0x368, 0, 0, 0, { { 75, 0, 0 }, { 14, 0, 0 }, { 76, 0, 0x2000003 }, { 47, 0, 0x2000001 }, { 43, 0, 0x2000000 } } }, // main
		{ 0x71c0d, 15, 38, 0x338, 0, 0, 4, { { 50, 0xf, 0x60000000 }, { 50, 0xf, 0x60000000 }, { 50, 0xf, 0x60000000 }, { 50, 0xf, 0x60000000 }, { 50, 0xf, 0x60000000 } } },   // save
		{ 0x71c5d, 14, 37, 0x330, 0, 0, 4, { { 49, 0xe, 0x20000000 }, { 49, 0xe, 0x20000000 }, { 49, 0xe, 0x20000000 }, { 49, 0xe, 0x20000000 }, { 49, 0xe, 0x20000000 } } },   // load
		{ 0x71cad, 15, 0, 0x338, 0, 0, 0, { { 39, 0, 0 }, { 17, 0, 0 } } },                                           // replace?
		{ 0x71cd9, 63, 0, 0x378, 0, 0, 0, { { 70, 0, 0 }, { 17, 0, 0 } } },
		{ 0x71d05, 69, 0, 0x378, 0, 0, 0, { { 70, 0, 0 }, { 77, 0, 0 }, { 17, 0, 0 } } },
	};
	return d;
}

const MsgBoxDesc *FindMsgBoxDesc(uint32_t va)
{
	for (const MsgBoxDesc &d : MsgBoxDescriptors()) if (d.va == va) return &d;
	return nullptr;
}

uint32_t MessageBoxPainter::Resolve(uint8_t *dest, uint32_t maxlen, uint32_t id)
{
	if (resolveText) return resolveText(dest, maxlen, id);
	static const char missing[13] = "Missing Text";                         // 0x71374, dbase100.c:420-421
	memcpy(dest, missing, 13);
	return 13;
}

//==========================================================================
//
// show_message_box's phases 1-5 (menu_hud_ui.c:1292-1570).
//
//==========================================================================

void MessageBoxPainter::Open(const MsgBoxDesc &d, uint32_t flags, int sel, int scroll)
{
	mDesc = d;
	mFlags = flags;
	mOpen = true;
	mScroll = scroll;
	mSelForPass = sel;
	mPrevSel = -2;                                                          // :1533
	mThumbRedraw = false;
	mZoom = false;
	mTickAccum = 2;                                                         // :1537
	mShW = 0;
	SetScreenPitch(screenPitch);
	const uint32_t count = (uint32_t)d.entries.size();
	uint32_t boxh = count * 0xb + 0x17;                                     // :1310
	mHeaderLines = 1;
	int32_t boxY = (int32_t)(((uint32_t)screenHeight - boxh) >> 1);        // :1314

	// ---- phase 1: the texts, newline-joined (:1320-1349) -------------------------
	uint8_t text[1200 + 0x60] = {};
	uint8_t *cur = text, *const end = text + 1200;
	auto take = [&](uint32_t len) {
		if (len > 0x50) len = 0x50;
		cur += len;
		if (cur > end) cur = end;
		if (cur > text) cur[-1] = 0x0a;
	};
	take(d.title != 0 ? Resolve(cur, 0x50, d.title) : 1);
	if (d.body != 0) { take(Resolve(cur, 0x50, d.body)); mHeaderLines = 2; }
	for (const MsgBoxEntry &e : d.entries)
	{
		uint32_t id = e.text;
		if ((e.value >> 24) & 0x80 && toggles != nullptr && toggles[e.value & 0xff] != 0) id = e.alt;
		uint32_t len;
		if (id == 0) { if (flags & 0x400) { *cur = 0x20; len = 2; } else len = 1; }
		else len = Resolve(cur, 0x50, id);
		take(len);
	}
	*cur = 0;

	// ---- phase 2: the layout (:1352-1356) -------------------------------------------
	std::vector<TextLine> tl;
	uint32_t nlines = (uint32_t)LayoutTimedMessageText(tl, text, screenPitch, (int)count, defaultColor5);
	mLines.clear();
	for (const TextLine &t : tl) { Line l; l.x0 = t.x0; l.x1 = t.x1; l.text = t.text; l.text.resize(l.text.size() + 8, 0); mLines.push_back(l); }
	if (nlines == 0 || mLines.size() < nlines) { Problem("a box's text did not lay out"); mOpen = false; return; }

	// ---- phase 2b: the widths (:1359-1415) -------------------------------------------
	int32_t maxx = 0;
	uint32_t minx = 8000;
	for (uint32_t i = 1; i < nlines; i++)
	{
		if (maxx < mLines[i].x1) maxx = mLines[i].x1;
		if (mLines[i].x0 < (int32_t)minx) minx = (uint32_t)mLines[i].x0;
	}
	uint32_t contentW = (uint32_t)maxx - minx;
	const uint32_t titleW = (uint32_t)(mLines[0].x1 - mLines[0].x0);
	if (titleW > contentW + 0x16) contentW = titleW - 0x16;
	if (flags & 0x10)
	{
		uint32_t maxw = 0, nslider = 0;
		for (uint32_t i = 0; i + 1 < nlines; i++)
		{
			uint32_t w = (uint32_t)(mLines[i + 1].x1 - mLines[i + 1].x0);
			const uint8_t fl = i < count ? (uint8_t)(d.entries[i].value >> 24) : 0;
			if (fl & 0x10) nslider++;
			else w -= 0x26;
			if (maxw < w) maxw = w;
		}
		if (maxw != 0) contentW = maxw + 0x26;
		const uint32_t lo = minx + 0x13, left = minx - 0x13, hib = (lo >> 8) & 0xff;
		uint32_t oldX = 0;                                                  // [ebp-0x22]: carried across rows
		for (uint32_t i = 0; i + 1 < nlines; i++)
		{
			const uint8_t fl = i < count ? (uint8_t)(d.entries[i].value >> 24) : 0;
			if (!(fl & 0x10)) continue;
			Line &t = mLines[i + 1];
			uint8_t *lbl = t.text.data();
			if (nslider != 1)
			{
				if (lbl[2] == 2) { oldX = ((uint32_t)lbl[4] << 8) | lbl[3]; lbl[3] = (uint8_t)lo; lbl[4] = (uint8_t)hib; }
				t.x1 = (int32_t)((uint32_t)t.x1 + lo - oldX);
				t.x0 = (int32_t)left;
			}
			else
			{
				uint32_t ox = (uint32_t)t.x0 - 0x13;
				t.x0 = (int32_t)ox;
				if (lbl[2] == 2) { ox += 0x26; lbl[3] = (uint8_t)ox; lbl[4] = (uint8_t)((ox >> 8) & 0xff); }
				t.x1 += 0x13;
			}
		}
	}

	// ---- phases 3 and 4: the box and its frame (:1422-1510) ----------------------------
	if (flags & 1)
	{
		if (flags & 0x100)
		{
			anchorX = (int)(((uint32_t)screenPitch - 0x120) >> 1);
			anchorY = (int)(((uint32_t)screenHeight - 0x50) >> 1);
		}
		else
		{
			anchorY = (int)((uint32_t)screenHeight >> 1);                   // compute_viewport_half_extents, renderer.c:9617-9621
			anchorX = (int)(((uint32_t)screenPitch - 0x130) >> 1);
		}
		mRowOff = 0x2e;
		mBoxW = 0x120;
		boxh = 0x50;
		mBoxX = anchorX;
		boxY = anchorY;
	}
	else
	{
		mBoxW = contentW + 0x41;
		mBoxX = (int)(((uint32_t)screenPitch - mBoxW) >> 1);
		mRowOff = 0xd;
	}
	mBoxY = boxY;
	mBoxH = boxh;
	// The picture: the box and a margin, for whatever the routines write beside it.
	SetRect(mBoxX - 8, mBoxY - 8, (int)mBoxW + 16, (int)mBoxH + 16, hires);
	if (flags & 1)
	{
		if (!(flags & 0x40)) { mShX = mBoxX; mShY = mBoxY; mShW = 0x11f; mShH = 0x50; }   // :1438-1443
		Tile(mBoxX, mBoxY, 0x328);                                          // :1444
		if (flags & 0x80) Tile(mBoxX + 3, mBoxY + 0x22, 0x20);
		if (d.icon != 0) Tile(mBoxX + 4, mBoxY + 4, d.icon);
		if (flags & 2)
		{
			if (!panelSnapshot.empty()) BlitDasImageAutoScale(panelSnapshot.data(), panelSnapshot.size(), mBoxX + 0x24, mBoxY + 0x10);
		}
		else if (!(flags & 4)) Tile(mBoxX + 0x27, mBoxY + 0xf, 0x360);
		DrawScrollIndicators();
	}
	else
	{
		mShX = mBoxX; mShY = mBoxY; mShW = (int)mBoxW - 1; mShH = (int)boxh; // :1467-1470
		{
			size_t avail = 0;
			const uint8_t *strip = RelocImage(0xd0, &avail);               // :1471-1478, its height patched
			if (strip != nullptr) BlitDasImageAutoScale(strip, avail, mBoxX, mBoxY, (int)(uint16_t)(boxh - 3));
		}
		if (d.icon != 0) Tile(mBoxX + 4, mBoxY + 4, d.icon);
		Tile(mBoxX, mBoxY + (int)boxh - 3, 0xd8);
		uint32_t rem = mBoxW - 0x47;                                        // :1483-1493
		Tile(mBoxX + 0x22, mBoxY, 0x2b8);
		int tx = mBoxX + 0x3f;
		int guard = 0;
		while (rem >= 0x33 && guard++ < 64) { Tile(tx, mBoxY, 0x2a0); rem -= 0x33; tx += 0x33; }
		if (guard >= 64) Problem("a box narrower than its frame (the edge loop would not end)");
		if (rem != 0) Tile(tx - 0x33 + (int)rem, mBoxY, 0x2a0);
		Tile(mBoxX + (int)mBoxW - 8, mBoxY, 0x318);
		int rowy = mBoxY + 0xf;                                             // :1495-1509
		ClearFramebufferRect(mBoxX + 0x22, rowy, (int)mBoxW - 0x2a, (int)(count * 0xb + 1));
		for (uint32_t i = 0; i < count; i++) { Tile(mBoxX + (int)mBoxW - 8, rowy, 0x2a8); rowy += 0xb; }
		rowy += 1;
		Tile(mBoxX + 0x21, rowy, 0x320);
		BlitRelocDasImage(mBoxX + 0x23, rowy, 0x2c8, mBoxW - 0x2a);         // the packed EBX: a stretched rule
		Tile(mBoxX + (int)mBoxW - 8, rowy, 0x2b0);
	}

	// ---- phase 5: the title, the body, the rows (:1513-1544) ------------------------------
	mContentY = mBoxY + 0x10;
	int textX = mBoxX + 0x23;
	{
		uint8_t *t0 = mLines[0].text.data();
		t0[3] = (uint8_t)textX;
		t0[1] = markerSelected;
		t0[4] = (uint8_t)((uint32_t)textX >> 8);
		DrawTextAtScreenXy(t0, mLines[0].text.size(), 0, mBoxY + 2, 0);
	}
	if (d.body != 0)
	{
		textX += 0x58;
		uint8_t *t1 = mLines[1].text.data();
		t1[1] = markerSelected;
		t1[3] = (uint8_t)textX;
		t1[4] = (uint8_t)((uint32_t)textX >> 8);
		nlines--;
		DrawTextAtScreenXy(t1, mLines[1].text.size(), 0, mContentY - 0xe, 0);
	}
	nlines--;
	mRows = nlines;
	if (mLines.size() < (size_t)mHeaderLines + mRows) { Problem("a box's rows did not lay out"); mRows = (uint32_t)(mLines.size() - mHeaderLines); }
	RenderMenuEntryList();
	if (flags & 0x200)                                                      // :1561-1570
	{
		const int xa = mBoxX & ~3;
		const int wa = ((mBoxX + (int)mBoxW + 3) & ~3) - xa;
		mZoomSave = SaveRegion(xa, mBoxY, wa, (int)mBoxH);
		mZoom = true;
	}
}

//==========================================================================
//
// render_menu_entry_list (menu_hud_ui.c:872-937).
//
//==========================================================================

void MessageBoxPainter::RenderMenuEntryList()
{
	int y = mContentY;
	for (uint32_t idx = 0; idx < mRows; idx++, y += 0xb)
	{
		Line &rec = Row(idx);
		const MsgBoxEntry e = idx < mDesc.entries.size() ? mDesc.entries[idx] : MsgBoxEntry();
		const uint32_t type = e.text;
		if (type == 0)
		{
			if (e.alt != 0) Problem("a gallery row (type 0 with a label table) is not drawn");   // :888-898
		}
		else if (type < 0x31 || type > 0x32)
		{
			if ((e.value >> 24) & 0x10)
				DrawMenuValueBar(mRowOff + rec.x0, y, rec.text.size() > 1 ? rec.text[1] : 0, (int)(e.value & 0x1ff));
			DrawTextAtScreenXy(rec.text.data(), rec.text.size(), mRowOff, y, 4);
		}
		else
		{
			const uint32_t l3c = idx + (uint32_t)mScroll;
			const uint8_t *pc = nullptr;
			size_t pcLen = 0;
			uint8_t dc[0x50] = {};
			if ((size_t)l3c * 0x30 + 2 <= slotNames.size())
			{
				pc = slotNames.data() + (size_t)l3c * 0x30;
				pcLen = slotNames.size() - (size_t)l3c * 0x30;
			}
			if (pc == nullptr) Problem("no slot names for a save / load row");
			if (pc != nullptr && pc[0] == 0)
			{
				if (pc[1] == '~') { pc = rec.text.data() + 5; pcLen = rec.text.size() - 5; }     // the row's own text
				else { Resolve(dc, 0x50, 0x33); pc = dc; pcLen = sizeof dc; }
			}
			uint8_t sb[80] = {};
			sb[0] = 1;
			sb[1] = rec.text.size() > 1 ? rec.text[1] : 0;
			// "%D: %s" (0x75ef9; Watcom's %D a signed decimal)
			char num[16];
			snprintf(num, sizeof num, "%d: ", (int)(l3c + 1));
			size_t o = 2;
			for (const char *q = num; *q && o < sizeof sb - 1; q++) sb[o++] = (uint8_t)*q;
			for (size_t i = 0; pc != nullptr && i < pcLen && pc[i] != 0 && o < sizeof sb - 1; i++) sb[o++] = pc[i];
			sb[o] = 0;
			DrawTextAtScreenXy(sb, sizeof sb, anchorX + 0x54 + mRowOff, y, 4);
			rec.x0 = anchorX + 0x54;
			rec.x1 = anchorX + 0xd4;
		}
	}
}

//==========================================================================
//
// draw_menu_value_bar (menu_hud_ui.c:803-844).
//
//==========================================================================

void MessageBoxPainter::DrawMenuValueBar(int x, int y, uint8_t marker, int value)
{
	uint32_t fill = (uint32_t)(value * 0x23) >> 8;
	if (value != 0 && fill == 0) fill = 1;
	if (fill > 0x23) fill = 0x23;
	const uint32_t unlitLen = 0x23 - fill;
	const bool lit = marker == markerSelected;
	// Each strip into its own 0x24-pitch work buffer (blit mode 1), as the
	// original does; then 9 rows copied.
	uint8_t buf1[324] = {}, buf2[324] = {};
	auto into = [&](uint8_t *buf, uint32_t slot) {
		size_t avail = 0;
		const uint8_t *img = RelocImage(slot, &avail);
		if (img == nullptr) return false;
		UiCanvas tmp;
		tmp.SetAssets(mA);
		tmp.SetScreenPitch(0x24);
		tmp.SetRect(0, 0, 0x24, 9, false);
		tmp.BlitDasImageToBuffer(img, avail, tmp.Addr(0, 0), 0x24, 1);
		for (int i = 0; i < 0x24 * 9; i++) buf[i] = tmp.Pixels()[(size_t)i];
		return true;
	};
	bool haveLit = false, haveUnlit = false;
	if (unlitLen != 0) haveUnlit = into(buf2, lit ? 0x358 : 0x348);
	if (fill != 0) haveLit = into(buf1, lit ? 0x350 : 0x340);
	(void)haveUnlit;
	const uint8_t *b1 = buf1, *b2 = buf2;
	if (haveLit) b2 += fill;
	long fp = Addr(x, y);
	for (int row = 0; row < 9; row++)
	{
		if (mHires)
		{
			for (uint32_t i = 0; i < fill; i++) Put(fp + (long)i, b1[i]);
			for (uint32_t i = 0; i < unlitLen; i++) Put(fp + (long)fill + (long)i, b2[i]);
			fp += VPITCH;
		}
		if (fill) { for (uint32_t i = 0; i < fill; i++) Put(fp + (long)i, b1[i]); b1 += 0x24; }
		if (unlitLen) { for (uint32_t i = 0; i < unlitLen; i++) Put(fp + (long)fill + (long)i, b2[i]); b2 += 0x24; }
		fp += VPITCH;
	}
}

//==========================================================================
//
// draw_scroll_indicators (menu_hud_ui.c:782-795).
//
//==========================================================================

void MessageBoxPainter::DrawScrollIndicators()
{
	if (mDesc.range == 0) return;
	Tile(mBoxX + 0x116, mBoxY + 0x12, mScroll != 0 ? 0xa8 : 0xb0);
	Tile(mBoxX + 0x116, mBoxY + 0x31, mScroll != (int)mDesc.range ? 0xb8 : 0xc0);
}

//==========================================================================
//
// One pass of the loop's drawing (menu_hud_ui.c:1658-1732).
//
//==========================================================================

void MessageBoxPainter::Pass(int sel, int newScroll, uint32_t ticks)
{
	if (!mOpen) return;
	if (newScroll >= 0 && newScroll != mScroll)                             // :1658-1678
	{
		mScroll = newScroll;
		DrawScrollIndicators();
		ClearFramebufferRect(mBoxX + 0x55 + mRowOff, mBoxY + 0xf, 0x8c, 0x38);
		mPrevSel = sel - 1;
	}
	if (mZoom) RestoreRegion(mZoomSave);                                    // :1680-1681
	if (sel != mPrevSel)                                                    // :1683-1699
	{
		mPrevSel = sel;
		for (uint32_t i = 0; i < mRows; i++)
		{
			Line &l = Row(i);
			if (l.text.size() > 1 && l.text[0] == 1) l.text[1] = defaultColor5;
		}
		if ((uint32_t)sel < mRows)
		{
			Line &l = Row((uint32_t)sel);
			if (l.text.size() > 1 && l.text[0] == 1) l.text[1] = markerSelected;
			mThumbRedraw = true;
		}
		RenderMenuEntryList();
	}
	if (mThumbRedraw)                                                       // :1701-1716
	{
		if ((uint32_t)sel < mRows && (mFlags & 4) && thumbnail)
		{
			const std::vector<uint8_t> th = thumbnail((uint32_t)mScroll + (uint32_t)sel);
			if (!th.empty()) BlitDasImageAutoScale(th.data(), th.size(), mBoxX + 0x24, mBoxY + 0x10);
		}
		mThumbRedraw = false;
	}
	if ((mFlags & 0x200) && mZoom)                                          // :1718-1732
	{
		mTickAccum += ticks;
		if (mTickAccum >= 0x20) mZoom = false;
		else
		{
			mZoomSave = SaveRegion(mZoomSave.x, mBoxY, mZoomSave.w, (int)mBoxH);   // blit_save_region: the screen into the block
			ShadeRemapBlit(mBoxX, mBoxY, (int)mBoxW, hires ? (int)mBoxH * 2 : (int)mBoxH, (uint8_t)(0x20 - mTickAccum));
		}
	}
}

//==========================================================================
//
// hit_test_ui_element (menu_hud_ui.c:46-87), as the loop calls it (:1591-1596).
//
//==========================================================================

int MessageBoxPainter::HitTest(int mx, int my) const
{
	if (!mOpen) return 0;
	// The loop passes (ctab, mouse_x, mouse_y, nlines, content_y, flags, box_x,
	// box_y) into parameters the lift names (cells, y, x, count, xbase, flags,
	// ybox, xbox): "y" is the mouse x, "ybox" the box x.
	const int32_t pitch = mRowOff;                                          // [0x83e88]
	const int32_t y = mx;
	const uint32_t x = (uint32_t)my;
	uint32_t xbase = (uint32_t)mContentY;
	int32_t iv = y - pitch;
	int idx = 1;
	for (uint32_t i = 0; i < mRows; i++, idx++, xbase += 0xb)
	{
		const Line &c = Row(i);
		if (xbase < x && x < xbase + 0xb && c.x0 < iv && iv < c.x1) return idx;
	}
	iv += pitch;
	const int32_t xs = (int32_t)x, ybox = mBoxX, xbox = mBoxY;
	if ((ybox + 4 < iv && iv < ybox + 0x20 && xbox + 4 < xs && xs < xbox + 0x20)
		|| ((mFlags & 0x80) && ybox + 4 <= iv && iv <= ybox + 0x17 && xbox + 0x23 <= xs && xs <= xbox + 0x36))
		return -1;
	if (!(!(mFlags & 0x800) || iv < ybox + 0x22 || ybox + 0x73 < iv || xs < xbox + 0xf || xbox + 0x4a < xs))
		return -9;
	if (mFlags & 8)
	{
		if (ybox + 0x116 <= iv && iv <= ybox + 0x11e && xbox + 0x12 <= xs && xs <= xbox + 0x26) return -6;
		if (ybox + 0x116 <= iv && iv <= ybox + 0x11e && xbox + 0x31 <= xs && xs <= xbox + 0x45) return -7;
	}
	return 0;
}

int MessageBoxPainter::LoopHit(int mx, int my) const
{
	int hit = HitTest(mx, my);
	if (hit == -6 && mScroll == 0) hit = 0;
	if (hit == -7 && mScroll == (int)mDesc.range) hit = 0;
	return hit;
}

} // namespace roth
