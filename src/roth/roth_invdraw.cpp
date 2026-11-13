//
// Realms of the Haunting's inventory panel, drawn as the original draws it.
// See roth_invdraw.h.
//
// THE CHECKLIST (porting rule 2). ROTH.C file:line -> here. "logic" = done by
// the rules in roth_invscreen.cpp, which call this file at the same points.
//
//  render_inventory_panel, inventory.c:1853-1944
//   1  :1856-1866 click edges, screen mode, panel origin, flags ... logic (Open) / the anchor is the picture's origin
//   2  :1864 label cache = -1 ..................................... RenderInventoryPanel
//   3  :1867-1876 save-under buffer, thumbnail snapshot ........... not ported: restores the 3D view under the panel; the panel here is its own picture
//   4  :1877-1878 grayscale_background_view ...................... not ported: turns the WORLD view grey (outside this picture); engine follow-up
//   5  :1882-1885 save_framebuffer_region, dirty rect ............. not ported: VGA page bookkeeping
//   6  :1886 draw_popup_shadow_border_smc (4-px shadow outside) ... not ported: blends what lies outside the rectangle
//   7  :1887 ICONS.ALL 0x10 at (0,0), the panel ................... RenderInventoryPanel
//   8  :1888-1890 slot tiles 0xa, 0xb, 0xc ........................ RenderInventoryPanel
//   9  :1891 arrow state = 0 ...................................... RenderInventoryPanel
//  10  :1892-1893 build the entry list, nothing held ............... logic (Open: Rebuild)
//  11  :1894 label x = 0x7b ....................................... RenderInventoryPanel
//  12  :1896-1901 + build_inventory_entry_list :1322,1324 the four fixed hotspots (0x26 box, 0x2a, 0x2b) ... RenderInventoryPanel
//  13  :1902 dirty += 4 ........................................... logic
//  14  :1903-1904 the two hand boxes .............................. RenderInventoryPanel
//  15  :1905-1917 the held ("Adam") item's icon at the 0xc box ..... RenderInventoryPanel
//  16  :1918-1926 its name, marker colour, at (0x23, 2); label x = width + 0x28 if > 0x7b ... RenderInventoryPanel
//  17  :1928 ICONS.ALL 0x78 at (0x23, 0xe) ........................ RenderInventoryPanel
//  18  :1929 ICONS.ALL 0x18 at (5, 0x24) .......................... RenderInventoryPanel
//  19  :1930-1931 the options hotspot (0x2c) ...................... RenderInventoryPanel
//  20  :1932 draw_inventory_tabs .................................. RenderInventoryPanel
//  21  :1933 render_inventory_grid ................................ RenderInventoryPanel
//  22  :1934-1942 the cursor's label, or tile 0xc8 and cache 0 ...... RenderInventoryPanel
//  23  :1943 dirty += 1 ........................................... logic
//  draw_inventory_tabs, inventory.c:1526-1555
//  24  per tab: lit/unlit picture (0x712da) by category == list, x 0x712d5, y 0x38 ... DrawInventoryTabs
//  25  hotspot {x, 0x38, 0x13, 0x12, 0x17 + category}, terminator ............... DrawInventoryTabs
//  26  :1553-1554 dirty rect ...................................... not ported: VGA bookkeeping
//  refresh_inventory_grid, inventory.c:1591-1610
//  27  only when dirty bit 1; dirty -= 2 .......................... RefreshInventoryGrid
//  28  render grid; label if (pos - scroll) < count (unsigned), passing pos ... RefreshInventoryGrid
//  29  else tile 0xc8 at (label x - 2, 2), cache 0 .................. RefreshInventoryGrid
//  render_inventory_grid, inventory.c:1951-2023
//  30  ten cells: tile, then the icon if the entry has one, tinted left > right > held ... RenderInventoryGrid
//  31  a hotspot per DRAWN cell {slot xy, 0x1c, 0x1c, 0x1c + cell} ... RenderInventoryGrid
//  32  up arrow: lit 0xa8 once when scrolled, 0xb0 once when not; hotspot {0x115,0x12,9,0x18,0x28} while lit ... RenderInventoryGrid
//  33  down arrow: threshold (5 - (count%5 ?: 5)) + count - 10 > scroll; 0xb8 / 0xc0; hotspot 0x29 ... RenderInventoryGrid
//  34  dirty rects ................................................ not ported: VGA bookkeeping
//  redraw_inventory_cursor_cell, inventory.c:1615-1633
//  35  skip while the panel is asked to close; refresh the grid ... RedrawInventoryCursorCell
//  36  cursor entry in the grid and in the ten cells, with an icon .. RedrawInventoryCursorCell
//  37  tint: cycle[tick & 0x1f] (0x71255), held = marker, left = colour 0, then right = colour 5 overrides ... RedrawInventoryCursorCell
//  draw_item_icon_in_slot, inventory.c:1186-1198
//  38  x = slot x + (0x1c - w) >> 1, y = slot y + (0x38 - h) >> 2, mode = colour << 8 | (hires ? 1 : 2) ... DrawItemIconInSlot
//  draw_equipped_item_left / right, inventory.c:1215-1247
//  39  the displayed hand item's icon at (0x3a | 0x59) + (0x1c - w) >> 1, 0xf + (0x38 - h) >> 2, tinted ... DrawEquippedItemLeft/Right
//  draw_panel_slot_tile, inventory.c:1293-1307
//  40  ICONS.ALL 0x110 at the slot's xy ............................ DrawPanelSlotTile
//  draw_inventory_entry_label, inventory.c:1573-1586
//  41  the slot's quantity, >> 8 for a "rounds << 8" weapon ......... DrawInventoryEntryLabel (ItemLabelQuantity)
//  format_inventory_item_label, inventory.c:1031-1072
//  42  text = record +0x10 or the ChangeName override; cached on (text, qty) ... FormatInventoryItemLabel
//  43  text[0] = 0; qty 0: read 0x61 bytes; else "%s (%D)" ......... FormatInventoryItemLabel
//  draw_ui_panel_count_element, menu_hud_ui.c:150-162
//  44  tile 0xc8 at (label x - 2, 2); text {1, 0x76768} at (label x, 2) when not empty ... DrawUiPanelCountElement
//  blit_item_icon, inventory.c:2073-2148
//  45  header skips (flag 4: 8, no flag 2: 0x300, then 8); ALWAYS run-length (it never tests flag 1) ... UiCanvas::BlitItemIcon (moved to roth_uidraw for the HUD)
//  46  index 0 not drawn; modes 2 (every other row), 1 (plain), 0 (doubled); colour <= 0xff00 -> blend + colour, else shade + colour >> 8 ... UiCanvas::BlitItemIcon
//  The primitives 47-58 are roth_uidraw's (moved there unchanged, with its own
//  checklist, so the menus and the dialogue text share them):
//  blit_reloc_das_image + blit_das_image_to_buffer, das_assets.c:1392-1400, 1195-1378
//  47  ICONS.ALL: image = base + dword[base + slot]; mode hires ? 0 : 1 ... UiCanvas::BlitRelocDasImage
//  48  every pixel drawn, 0 included; run-length when flag 1; mode 0 doubled, 1 plain, 2 every other row (a plain image: 1 and 2 alike) ... UiCanvas::BlitDasImageToBuffer
//  49  variant B (packed stride) .................................. UiCanvas (never reached by the panel)
//  50  the shaded variants ........................................ not ported: the panel's pictures are blitted unshaded (ecx = mode only)
//  rle_decode_scroll_segment, blit_2d.c:296-327
//  51  < 0xf1 a literal, else (b - 0xf0) copies of the next byte; the stream runs on across rows ... roth_uidraw DecodeRle
//  screen_xy_to_framebuffer_ptr, renderer.c:579-586
//  52  y doubled in the hi-res modes .............................. UiCanvas::Addr
//  draw_text_at_screen_xy / draw_text_to_buffer, text_font.c:371-380, 300-367
//  53  hi-res: flags + 1, y doubled; pitch doubled; pen = dest + pitch + 1 ... UiCanvas::DrawTextAtScreenXy / DrawTextToBuffer
//  54  0 ends, 0x0d / 0x0a new line, 1 <c> ramp rows 0..11 of the shade table at column c (flat with flag 8), 2 <w> advance ... UiCanvas::DrawTextAtScreenXy / DrawTextToBuffer
//  55  glyph: signed offset, 0 -> default advance; width, +1 with 0x20; top, rows; nibble-packed when width <= 4 ... UiCanvas::DrawTextAtScreenXy / DrawTextToBuffer
//  56  outline: index 1 on 4 neighbours (6 in hi-res) unless flag 4 (text_font.c:193-245) ... UiCanvas::DrawTextAtScreenXy / DrawTextToBuffer
//  57  glyph pixels: ramp[top + row]; hi-res also the row below (text_font.c:252-279) ... UiCanvas::DrawTextAtScreenXy / DrawTextToBuffer
//  measure_control_text_width, renderer.c:436-452
//  58  sum of advances of bytes >= 0x20; 1 skips 1, 2 skips 2 ...... UiCanvas::MeasureControlTextWidth
//  hit_test_dialogue_ui_action, dialogue_ui.c:576-604
//  59  outside [0, 0x120] x [0, 0x50] -> 0x26 ...................... HitTest
//  60  the lists 0x71241 = {0x80b74 tabs, 0x80ba4 the rest}; unsigned dx <= w, dy <= h ... HitTest
//  61  characters list with an item held: 0x2a / 0x2b give 0 ......... HitTest
//  62  the examine popup's half (:606-662) ........................ not here: the popup is not drawn by this module
//

#include "roth_invdraw.h"
#include "roth_gamecore.h"
#include "roth_dbase100.h"
#include "roth_dialogue.h"
#include "roth_uidraw.h"

#include <stdio.h>
#include <string.h>

namespace roth
{

//==========================================================================
//
// The executable's tables, transcribed from the running original's bytes
// (tools/invdraw_test, the probe's "exe" lines; the test compares them).
//
//==========================================================================

const uint16_t InvPanelTables::slotXY[13][2] = {
	{ 0x7b, 0x0f }, { 0x9a, 0x0f }, { 0xb9, 0x0f }, { 0xd8, 0x0f }, { 0xf7, 0x0f },
	{ 0x7b, 0x2e }, { 0x9a, 0x2e }, { 0xb9, 0x2e }, { 0xd8, 0x2e }, { 0xf7, 0x2e },
	{ 0x3a, 0x0f }, { 0x59, 0x0f }, { 0x04, 0x04 },
};
const uint32_t InvPanelTables::tabDas[5][2] = { { 0x68, 0x70 }, { 0x28, 0x30 }, { 0x38, 0x40 }, { 0x48, 0x50 }, { 0x58, 0x60 } };
const uint8_t InvPanelTables::tabX[5] = { 5, 25, 45, 65, 85 };
const uint8_t InvPanelTables::tabCategory[5] = { 0, 1, 3, 4, 2 };
const uint32_t InvPanelTables::cursorCycle[32] = {
	0, 0, 0, 256, 256, 512, 512, 768, 1024, 1280, 1536, 1792, 2560, 3328, 4096, 5120,
	6144, 5120, 4096, 3328, 2560, 1792, 1536, 1280, 1024, 768, 512, 512, 256, 256, 0, 0,
};
const uint8_t InvPanelTables::markerSelected = 0x40;
const uint8_t InvPanelTables::leftHandColor = 0x67;
const uint8_t InvPanelTables::rightHandColor = 0xee;
const uint8_t InvPanelTables::labelColor = 0xd7;
const char InvPanelTables::labelFormat[] = "%s (%D)";

namespace
{

uint16_t U16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

} // namespace

//==========================================================================
//
// Set-up.
//
//==========================================================================

void InventoryPanelPainter::Bind(InventoryScreen *screen, GameCore *core, const Dbase100 *db, const InvPanelAssets &assets, bool hires)
{
	mScreen = screen;
	mCore = core;
	mDb = db;
	mIA = assets;
	UiAssets ua;
	ua.iconsAll = assets.iconsAll;
	ua.shade = assets.shade;
	ua.blend = assets.blend;
	ua.font = assets.font;
	SetAssets(ua);
	SetRect(0, 0, W, H, hires);                                             // panel-relative: (0, 0) is the anchor
	mDrawn = false;
	if (mA.font == nullptr || !mA.font->ok()) Problem("no font: the panel's text is not drawn (ROTH.EXE's font was not found)");
	if (mScreen != nullptr) mScreen->SetDraw(this);
}

//==========================================================================
//
// What the rules hold.
//
//==========================================================================

int32_t InventoryPanelPainter::EntryItem(int entry) const
{
	if (mScreen == nullptr || mCore == nullptr) return 0;
	const InventoryEntryList &l = mScreen->Entries();
	if (entry < 0 || entry >= (int)l.slots.size()) return 0;
	return (int16_t)mCore->Items().Slot(l.slots[(size_t)entry]).id;     // entry +8, inventory.c:1346
}

int32_t InventoryPanelPainter::EntryQty(int entry) const
{
	if (mScreen == nullptr || mCore == nullptr || mDb == nullptr) return 0;
	const InventoryEntryList &l = mScreen->Entries();
	if (entry < 0 || entry >= (int)l.slots.size()) return 0;
	return ItemLabelQuantity(*mDb, mCore->Items().Slot(l.slots[(size_t)entry]));   // inventory.c:1577-1582
}

// The icon an item's entry loads (load_item_icon_resource, inventory.c:973-996):
// its record's +0x0c resource of DBASE200.DAT, a dword size and the picture.
bool InventoryPanelPainter::ItemIcon(int32_t itemId, const uint8_t **img, size_t *size) const
{
	if (mDb == nullptr || mIA.dbase200 == nullptr) return false;
	return ItemIconPicture(*mDb, *mIA.dbase200, itemId, img, size);
}

int InventoryPanelPainter::EntryIcon(int entry, const uint8_t **img, size_t *size) const
{
	return ItemIcon(EntryItem(entry), img, size) ? 1 : 0;
}

//==========================================================================
//
// Primitives.
//
//==========================================================================

// blit_item_icon is UiCanvas::BlitItemIcon (roth_uidraw), shared with the HUD.

//==========================================================================
//
// The routines.
//
//==========================================================================

// draw_panel_slot_tile (inventory.c:1293-1307).
void InventoryPanelPainter::DrawPanelSlotTile(int n)
{
	if (!mDrawn || n < 0 || n > 12) return;
	BlitRelocDasImage(InvPanelTables::slotXY[n][0], InvPanelTables::slotXY[n][1], 0x110);
}

// draw_item_icon_in_slot (inventory.c:1186-1198).
void InventoryPanelPainter::DrawItemIconInSlot(const uint8_t *icon, size_t size, int slot, uint32_t color)
{
	if (icon == nullptr || slot < 0 || slot > 12) return;
	const int w = (int16_t)U16(icon + 4), h = (int16_t)U16(icon + 6);
	const int x = InvPanelTables::slotXY[slot][0] + ((0x1c - w) >> 1);
	const int y = InvPanelTables::slotXY[slot][1] + ((0x38 - h) >> 2);
	BlitItemIcon(icon, size, x, y, (color << 8) | (mHires ? 1u : 2u));
}

// draw_equipped_item_left / right (inventory.c:1215-1247).
void InventoryPanelPainter::DrawEquippedItemLeft()
{
	if (!mDrawn || mScreen == nullptr || mCore == nullptr) return;
	const int shown = mScreen->Entries().shownLeft;                         // [0x81034]
	if (shown < 0) return;
	const uint8_t *icon = nullptr; size_t size = 0;
	if (!ItemIcon((int16_t)mCore->Items().Slot(shown).id, &icon, &size)) return;   // :1219
	const int w = (int16_t)U16(icon + 4), h = (int16_t)U16(icon + 6);
	const int y = 0xf + ((0x38 - h) >> 2);
	const uint32_t mode = ((mHires ? 0u : 1u) + 1u) + ((uint32_t)InvPanelTables::leftHandColor << 8);
	const int x = 0x3a + ((0x1c - w) >> 1);
	BlitItemIcon(icon, size, x, y, mode);
}

void InventoryPanelPainter::DrawEquippedItemRight()
{
	if (!mDrawn || mScreen == nullptr || mCore == nullptr) return;
	const int shown = mScreen->Entries().shownRight;                        // [0x81040]
	if (shown < 0) return;
	const uint8_t *icon = nullptr; size_t size = 0;
	if (!ItemIcon((int16_t)mCore->Items().Slot(shown).id, &icon, &size)) return;
	const int w = (int16_t)U16(icon + 4), h = (int16_t)U16(icon + 6);
	const int y = 0xf + ((0x38 - h) >> 2);
	const uint32_t mode = ((mHires ? 0u : 1u) + 1u) + ((uint32_t)InvPanelTables::rightHandColor << 8);
	const int x = 0x59 + ((0x1c - w) >> 1);
	BlitItemIcon(icon, size, x, y, mode);
}

// draw_inventory_tabs (inventory.c:1526-1555).
void InventoryPanelPainter::DrawInventoryTabs()
{
	if (!mDrawn || mScreen == nullptr) return;
	mTabHot.clear();
	for (int i = 0; i < 5; i++)
	{
		const int cat = InvPanelTables::tabCategory[i];
		const uint32_t das = cat == mScreen->ActiveList() ? InvPanelTables::tabDas[i][1] : InvPanelTables::tabDas[i][0];
		const int x = InvPanelTables::tabX[i], y = 0x38;
		InvHotspot h;
		h.x = (int16_t)x; h.y = (int16_t)y; h.w = 0x13; h.h = 0x12;
		h.action = (int16_t)(InvPanelTables::tabCategory[i] + 0x17);      // :1546-1548
		mTabHot.push_back(h);
		BlitRelocDasImage(x, y, das);
	}
}

// format_inventory_item_label (inventory.c:1031-1072).
void InventoryPanelPainter::FormatInventoryItemLabel(int32_t itemId, int32_t qty)
{
	if (itemId == 0) return;                                                // :1034
	const uint32_t text = mIA.labelText ? mIA.labelText(itemId) : 0;          // :1035-1039
	if (text == g_label_text && qty == g_label_qty) return;                 // :1040
	g_label[2] = 0;                                                         // :1041
	g_label_text = text;
	g_label_qty = qty;
	if (text == 0) return;                                                  // :1044
	if (!mIA.readLine) return;
	if (qty == 0) { mIA.readLine(g_label + 2, 0x61, text); return; }        // :1045-1054
	// :1055-1071. The stack buffer the original reads into is not cleared
	// first; DBASE400 texts carry their own terminating zero (the test reads
	// every label this one draws), so a cleared one prints the same.
	uint8_t buf[0x64] = {};
	mIA.readLine(buf, 0x61, text);
	buf[0x63] = 0;
	char out[0x100];
	// "%s (%D)": Watcom's %D is a signed decimal (the test's labels with a count).
	snprintf(out, sizeof out, "%s (%d)", (const char *)buf, (int)qty);
	const size_t n = strlen(out);
	memcpy(g_label + 2, out, n + 1 <= sizeof g_label - 2 ? n + 1 : sizeof g_label - 2);
}

// draw_ui_panel_count_element (menu_hud_ui.c:150-162).
void InventoryPanelPainter::DrawUiPanelCountElement()
{
	BlitRelocDasImage(g_label_x - 2, 2, 0xc8);
	if (g_label[2] != 0)
	{
		g_label[1] = InvPanelTables::labelColor;                            // [0x76768]
		g_label[0] = 1;
		DrawTextAtScreenXy(g_label, sizeof g_label, g_label_x, 2, 0);
	}
}

// draw_inventory_entry_label (inventory.c:1573-1586).
void InventoryPanelPainter::DrawInventoryEntryLabel(int entry)
{
	FormatInventoryItemLabel(EntryItem(entry), EntryQty(entry));
	DrawUiPanelCountElement();
}

// render_inventory_grid (inventory.c:1951-2023).
void InventoryPanelPainter::RenderInventoryGrid()
{
	if (mScreen == nullptr) return;
	mHot.resize(mHot.size() < 4 ? mHot.size() : 4);                         // the fixed four stay; 0x80bc4 on is rewritten
	const int count = mScreen->EntryCount();
	const InventoryEntryList &l = mScreen->Entries();
	const int held = mScreen->HeldEntry();
	int base = mScreen->ScrollOffset();
	for (int cell = 0; cell < 10; cell++, base++)
	{
		DrawPanelSlotTile(cell);
		if (base >= count) continue;
		const uint8_t *icon = nullptr; size_t size = 0;
		if (!EntryIcon(base, &icon, &size)) continue;                       // :1961, no icon: no picture and no hotspot
		uint32_t color = 0;
		if (base == l.leftEntry) color = InvPanelTables::leftHandColor;
		else if (base == l.rightEntry) color = InvPanelTables::rightHandColor;
		else if (base == held) color = InvPanelTables::markerSelected;
		DrawItemIconInSlot(icon, size, cell, color);
		InvHotspot h;
		h.x = (int16_t)InvPanelTables::slotXY[cell][0]; h.y = (int16_t)InvPanelTables::slotXY[cell][1];
		h.w = 0x1c; h.h = 0x1c; h.action = (int16_t)(cell + 0x1c);
		mHot.push_back(h);
	}
	const int scroll = mScreen->ScrollOffset();
	if (scroll > 0)                                                         // :1982
	{
		if (!(g_inventory_arrow_state & 1)) { g_inventory_arrow_state += 1; BlitRelocDasImage(0x116, 0x12, 0xa8); }
		InvHotspot h; h.x = 0x115; h.y = 0x12; h.w = 9; h.h = 0x18; h.action = 0x28;
		mHot.push_back(h);
	}
	else if (g_inventory_arrow_state & 1) { g_inventory_arrow_state -= 1; BlitRelocDasImage(0x116, 0x12, 0xb0); }
	const uint32_t rem = (uint32_t)count % 5;                               // :2001-2002
	const int e = rem != 0 ? (int)rem : 5;
	const int thresh = (5 - e) + count - 0xa;
	if (thresh > scroll)
	{
		if (!(g_inventory_arrow_state & 2)) { g_inventory_arrow_state += 2; BlitRelocDasImage(0x116, 0x31, 0xb8); }
		InvHotspot h; h.x = 0x115; h.y = 0x31; h.w = 9; h.h = 0x18; h.action = 0x29;
		mHot.push_back(h);
	}
	else if (g_inventory_arrow_state & 2) { g_inventory_arrow_state -= 2; BlitRelocDasImage(0x116, 0x31, 0xc0); }
}

// refresh_inventory_grid (inventory.c:1591-1610).
void InventoryPanelPainter::RefreshInventoryGrid()
{
	if (!mDrawn || mScreen == nullptr) return;
	if (!(mScreen->g_inventory_dirty_flags & 2)) return;
	const int pos = mScreen->CursorEntry();
	mScreen->g_inventory_dirty_flags -= 2;
	RenderInventoryGrid();
	const int rel = pos - mScreen->ScrollOffset();
	if ((uint32_t)rel < (uint32_t)mScreen->EntryCount()) { DrawInventoryEntryLabel(pos); return; }
	BlitRelocDasImage(g_label_x - 2, 2, 0xc8);
	g_label_text = 0;
}

// redraw_inventory_cursor_cell (inventory.c:1615-1633).
void InventoryPanelPainter::RedrawInventoryCursorCell(uint16_t tick)
{
	if (!mDrawn || mScreen == nullptr) return;
	if (mScreen->PanelCloseRequested()) return;
	RefreshInventoryGrid();
	const int pos = mScreen->CursorEntry();
	if ((uint32_t)pos >= (uint32_t)mScreen->EntryCount()) return;
	const int rel = pos - mScreen->ScrollOffset();
	if ((uint32_t)rel > 9) return;
	const uint8_t *icon = nullptr; size_t size = 0;
	if (!EntryIcon(pos, &icon, &size)) return;
	uint32_t color = InvPanelTables::cursorCycle[tick & 0x1f];
	const InventoryEntryList &l = mScreen->Entries();
	if (pos == mScreen->HeldEntry()) color = InvPanelTables::markerSelected;
	else if (pos == l.leftEntry) color = InvPanelTables::leftHandColor;
	if (pos == l.rightEntry) color = InvPanelTables::rightHandColor;
	DrawItemIconInSlot(icon, size, rel, color);
}

// render_inventory_panel's drawing (inventory.c:1864, 1886-1942).
void InventoryPanelPainter::RenderInventoryPanel()
{
	if (mScreen == nullptr) return;
	mDrawn = true;
	g_label_text = 0xffffffffu;                                             // :1864
	BlitRelocDasImage(0, 0, 0x10);                                          // :1887
	DrawPanelSlotTile(0xa);                                                 // :1888-1890
	DrawPanelSlotTile(0xb);
	DrawPanelSlotTile(0xc);
	g_inventory_arrow_state = 0;                                            // :1891
	g_label_x = 0x7b;                                                       // :1894
	mHot.clear();                                                           // :1896-1901
	{
		InvHotspot h;
		h.x = 4; h.y = 4; h.w = 0x1c; h.h = 0x1c; h.action = 0x26; mHot.push_back(h);
		h.x = 0x3a; h.y = 0xf; h.action = 0x2a; mHot.push_back(h);
		h.x = 0x59; h.y = 0xf; h.action = 0x2b; mHot.push_back(h);
		h.x = 4; h.y = 0x23; h.w = 0x13; h.h = 0x13; h.action = 0x2c; mHot.push_back(h);   // :1930-1931
	}
	DrawEquippedItemLeft();                                                 // :1903
	DrawEquippedItemRight();                                                // :1904
	const int32_t adam = mCore != nullptr ? mCore->Items().AdamItemId() : 0;   // :1905, [0x81050]
	const DbItem *rec = (adam != 0 && mDb != nullptr) ? mDb->Item(adam) : nullptr;
	if (rec != nullptr && rec->ok)
	{
		const uint8_t *icon = nullptr; size_t size = 0;
		if (ItemIcon(adam, &icon, &size))                                   // :1906-1917, [0x7fed0]
		{
			const int h = (int16_t)U16(icon + 6), w = (int16_t)U16(icon + 4);
			const int y = ((0x38 - h) >> 2) + 4;
			const int x = ((0x1c - w) >> 1) + 4;
			BlitItemIcon(icon, size, x, y, mHires ? 1u : 2u);
		}
		uint8_t loc[0x64] = {};
		const uint32_t txt = rec->name;                                     // :1918
		if (txt != 0 && mIA.readLine && mIA.readLine(loc + 2, 0x62, txt) != 0)
		{
			loc[0] = 1;
			loc[1] = InvPanelTables::markerSelected;
			DrawTextAtScreenXy(loc, sizeof loc, 0x23, 2, 0);                // :1921-1923
			const int wdt = MeasureControlTextWidth(loc + 2, sizeof loc - 2) + 0x28;   // :1924
			if (wdt > 0x7b) g_label_x = wdt;                                // :1925
		}
	}
	BlitRelocDasImage(0x23, 0xe, 0x78);                                     // :1928
	BlitRelocDasImage(5, 0x24, 0x18);                                       // :1929
	DrawInventoryTabs();                                                    // :1932
	RenderInventoryGrid();                                                  // :1933
	const int pos = mScreen->CursorEntry();                                 // :1934-1942
	if ((uint32_t)pos < (uint32_t)mScreen->EntryCount()) DrawInventoryEntryLabel(pos);
	else
	{
		BlitRelocDasImage(g_label_x - 2, 2, 0xc8);
		g_label_text = 0;
	}
}

// hit_test_dialogue_ui_action's panel half (dialogue_ui.c:576-604).
int InventoryPanelPainter::HitTest(int mx, int my) const
{
	if (my < 0 || mx < 0 || 0x50 < my || 0x120 < mx) return 0x26;          // :577-581
	const std::vector<InvHotspot> *lists[2] = { &mTabHot, &mHot };          // 0x71241: {0x80b74, 0x80ba4}
	for (const std::vector<InvHotspot> *l : lists)
		for (const InvHotspot &h : *l)
		{
			const int32_t dx = mx - h.x;
			if ((uint32_t)h.w < (uint32_t)dx) continue;
			const int32_t dy = my - h.y;
			if ((uint32_t)dy > (uint32_t)h.h) continue;
			if (mScreen != nullptr && mScreen->ActiveList() == 2 && mScreen->HeldEntry() >= 0 && (h.action == 0x2a || h.action == 0x2b))
				return 0;                                                   // :592-596
			return h.action;
		}
	return 0;
}

} // namespace roth
