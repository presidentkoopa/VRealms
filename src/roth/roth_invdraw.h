#pragma once
//
// Realms of the Haunting's inventory PANEL, drawn as the original draws it:
// into an 8-bit picture of the panel's own 0x120 x 0x50 rectangle, from the
// install's own pictures (DATA\ICONS.ALL for the panel, its tiles, tabs and
// arrows; DBASE200.DAT for the item icons; the map pack's shade and blend
// tables for the tints), with the text in the original's own font out of the
// player's ROTH.EXE, every piece placed by the original's drawing routines.
// Engine-free: the C++ standard library and the readers it names, nothing
// else. The engine shows the picture through the game palette and maps a
// click on it to the original's own hotspot rectangles (HitTest).
//
// WHERE IT COMES FROM (ROTH.C, roth_c/src/engine), routine by routine; the
// numbered checklist with file:line on both sides is in roth_invdraw.cpp.
//   render_inventory_panel        inventory.c:1853-1944   the open
//   draw_inventory_tabs           inventory.c:1526-1555
//   refresh_inventory_grid        inventory.c:1591-1610
//   render_inventory_grid         inventory.c:1951-2023
//   redraw_inventory_cursor_cell  inventory.c:1615-1633   the 32-step colour cycle
//   draw_item_icon_in_slot        inventory.c:1186-1198
//   draw_equipped_item_left/right inventory.c:1215-1247
//   draw_panel_slot_tile          inventory.c:1293-1307
//   draw_inventory_entry_label    inventory.c:1573-1586
//   format_inventory_item_label   inventory.c:1031-1072
//   draw_ui_panel_count_element   menu_hud_ui.c:150-162
//   blit_item_icon                inventory.c:2025-2148
//   blit_reloc_das_image          das_assets.c:1392-1400
//   blit_das_image_to_buffer      das_assets.c:1195-1378
//   rle_decode_scroll_segment     blit_2d.c:296-327
//   screen_xy_to_framebuffer_ptr  renderer.c:579-586
//   draw_text_at_screen_xy        text_font.c:371-380
//   draw_text_to_buffer           text_font.c:281-367 (and its two helpers, :193-279)
//   measure_control_text_width    renderer.c:438-452
//   hit_test_dialogue_ui_action   dialogue_ui.c:546-604  the panel half
//
// The picture is the PANEL RECTANGLE only. What the original also does to
// the screen around it when it opens -- the 3D view turned grey
// (grayscale_background_view) and a 4-pixel shadow blended into what lies
// around the rectangle (draw_popup_shadow_border_smc) -- belongs to the world
// view behind the panel and is not drawn here (see the checklist).
//

#include "roth_invscreen.h"

#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <string>
#include <vector>

namespace roth
{

class Dbase100;

// The original's font (g_font_descriptor, 0x70f12) is ROTH.EXE's own
// initialised data, not one of the game files. FindRothExeFont finds it in
// the bytes of the player's ROTH.EXE and accepts it only when it is exactly
// the original's (an FNV-1a hash of the bytes the font uses, measured from the
// running original by tools/invdraw_test); the bytes themselves are never
// copied into this program.
struct RothFont
{
	std::vector<uint8_t> bytes;     // the glyphs, then the descriptor
	size_t desc = 0;                // where the descriptor starts in `bytes`
	bool ok() const { return !bytes.empty(); }
};
// The span of the executable's data the font uses, around the descriptor: its
// glyphs lie BELOW it (the offset table is signed, renderer.c:342), the lowest
// at -0x4d6; the descriptor's 6 header bytes and 256 offsets end at +0x206.
static const size_t ROTH_FONT_BEFORE = 0x4d6;
static const size_t ROTH_FONT_AFTER = 0x206;
extern const uint32_t ROTH_FONT_HASH;          // measured from the running original; see roth_invdraw.cpp
uint32_t RothFontHash(const uint8_t *p, size_t n);
// Search `exe` for the font and verify it. False with `error` set when the
// file does not hold exactly the original's font.
bool FindRothExeFont(const std::vector<uint8_t> &exe, RothFont &out, std::string &error);

// The executable's small tables the panel's drawing reads. These ARE
// transcribed (as the font's advance widths in roth_dialogue.cpp are), each
// one the running original's own bytes, read out by tools/invdraw_test's probe
// ("exe" lines) and compared with these by the test.
struct InvPanelTables
{
	// g_ui_slot_layout_table 0x71208: {x, y} of grid cells 0..9, the left
	// hand box (10), the right hand box (11), the held item's box (12).
	static const uint16_t slotXY[13][2];
	// 0x712da: the five tabs' ICONS.ALL slots, {unlit, lit}.
	static const uint32_t tabDas[5][2];
	// 0x712d5: the tabs' x.
	static const uint8_t tabX[5];
	// 0x7123c: each tab's list category.
	static const uint8_t tabCategory[5];
	// 0x71255: the cursor cell's 32-step tint (a shade-table byte offset; 0 = untinted).
	static const uint32_t cursorCycle[32];
	// 0x7675d g_map_menu_marker_selected; 0x7675e g_default_message_color[0]
	// (left hand) and [5] (right hand); 0x76768 the label's text colour.
	static const uint8_t markerSelected, leftHandColor, rightHandColor, labelColor;
	// 0x75e2c: the label format when the item has a quantity.
	static const char labelFormat[];
};

// One hotspot as the original stores it (dialogue_ui.c:583-591): a click at
// (mx, my) is on it when 0 <= mx - x <= w and 0 <= my - y <= h, compared
// unsigned. Panel coordinates (the original's minus the panel's anchor).
struct InvHotspot
{
	int16_t x = 0, y = 0;
	uint8_t w = 0, h = 0;
	int16_t action = 0;
};

// What the painter reads from outside, besides the inventory screen's state.
struct InvPanelAssets
{
	const std::vector<uint8_t> *iconsAll = nullptr;     // DATA\ICONS.ALL, whole
	const std::vector<uint8_t> *dbase200 = nullptr;     // DBASE200.DAT, whole
	const uint8_t *shade = nullptr;     // the map pack's 0x4000 shade tables (Pack::ShadeTables): [0x86d14]
	const uint8_t *blend = nullptr;     // the map pack's 0x10000 table (Pack::BlendTable): [0x85d08]
	const RothFont *font = nullptr;     // null: no text is drawn (and the log says why)
	// read_next_dialogue_line(dest, maxlen, offset, flag 0) (dbase100.c:371-403):
	// the bytes of a DBASE400 entry. The engine binds DialogueUi's, whose side
	// effects (the timed-message colour, the short-line cache) are the original's.
	std::function<uint32_t(uint8_t *dest, uint32_t maxlen, uint32_t offset)> readLine;
	// The text an item's label shows: ItemLabelText (roth_inventory.h), which
	// may run the item's ChangeName condition through the interpreter, as the
	// original does when it formats the label.
	std::function<uint32_t(int32_t itemId)> labelText;
};

class InventoryPanelPainter : public InventoryScreenDraw
{
public:
	static const int W = 0x120, H = 0x50;

	// `hires` is g_hires_line_doubling_flag (0x90cbd): set in the portrait
	// video modes, where every logical row is two rows of the picture. The
	// retail ROTH.INI (VideoMode 8, 640x480) leaves it clear.
	void Bind(InventoryScreen *screen, GameCore *core, const Dbase100 *db, const InvPanelAssets &assets, bool hires = false);

	// The map's pack tables, when a map opens (the tints read them).
	void SetPackTables(const uint8_t *shade, const uint8_t *blend) { mA.shade = shade; mA.blend = blend; }

	// The picture: Width() x Height() palette indices, row-major.
	int Width() const { return W; }
	int Height() const { return mHires ? H * 2 : H; }
	const std::vector<uint8_t> &Pixels() const { return mPix; }
	// Bumped on every change to the picture.
	uint32_t Version() const { return mVersion; }

	// The hotspot lists (0x80b74: the tabs; 0x80ba4 and on: the boxes, the
	// cells, the arrows), in the order hit_test_dialogue_ui_action scans them.
	const std::vector<InvHotspot> &TabHotspots() const { return mTabHot; }
	const std::vector<InvHotspot> &PanelHotspots() const { return mHot; }
	// hit_test_dialogue_ui_action's panel half (dialogue_ui.c:576-604) at
	// panel pixel (mx, my): 0x26 outside the panel, else the first hotspot's
	// action, else 0.
	int HitTest(int mx, int my) const;

	// One UI frame's drawing (game_core.c:886, redraw_inventory_cursor_cell):
	// the grid if it is marked, then the cursor's cell in the tint of frame
	// tick `tick` (g_frame_tick_counter, 0x90bcc).
	void RedrawInventoryCursorCell(uint16_t tick);

	// ---- InventoryScreenDraw: the calls the rules make --------------------------------
	void RenderInventoryPanel() override;
	void DrawInventoryTabs() override;
	void DrawPanelSlotTile(int n) override;
	void DrawEquippedItemLeft() override;
	void DrawEquippedItemRight() override;
	void FormatInventoryItemLabel(int32_t itemId, int32_t qty) override;
	void RefreshInventoryGrid() override;

	// What went wrong, if anything (a missing picture, no font): one line each.
	const std::vector<std::string> &Problems() const { return mProblems; }

	// ---- the drawing state the original keeps in globals ------------------------------
	uint32_t g_inventory_arrow_state = 0;   // 0x811a0: bit 0 the up arrow is lit, bit 1 the down
	uint32_t g_label_text = 0;              // 0x811a4: the label's cached text
	int32_t g_label_qty = 0;                // 0x811a8: and quantity
	int32_t g_label_x = 0x7b;               // 0x81058: where the label starts
	uint8_t g_label[0x64] = {};             // 0x8105c: {1, colour, text...}; the text at 0x8105e

private:
	// ---- the routines -------------------------------------------------------
	void RenderInventoryGrid();
	void DrawInventoryEntryLabel(int entry);
	void DrawUiPanelCountElement();
	void DrawItemIconInSlot(const uint8_t *icon, size_t size, int slot, uint32_t color);
	// ---- the primitives -------------------------------------------------------
	void BlitRelocDasImage(int x, int y, uint32_t offSlot);
	void BlitDasImage(const uint8_t *img, size_t size, int x, int y, int mode);
	void BlitItemIcon(const uint8_t *icon, size_t size, int x, int y, uint32_t ecx);
	void DrawTextAtScreenXy(const uint8_t *str, size_t len, int x, int y, uint32_t flags);
	int MeasureControlTextWidth(const uint8_t *s, size_t len) const;
	void Put(long addr, uint8_t v);
	long Addr(int x, int y) const;     // screen_xy_to_framebuffer_ptr, on the virtual screen
	uint8_t mRamp[32] = {};            // draw_text_to_buffer's colour ramp (a static there too, text_font.c:301)
	// ---- what the rules hold ---------------------------------------------------
	int EntryIcon(int entry, const uint8_t **img, size_t *size) const;  // 0 none
	bool ItemIcon(int32_t itemId, const uint8_t **img, size_t *size) const;
	int32_t EntryItem(int entry) const;
	int32_t EntryQty(int entry) const;
	void Problem(const std::string &s);

	InventoryScreen *mScreen = nullptr;
	GameCore *mCore = nullptr;
	const Dbase100 *mDb = nullptr;
	InvPanelAssets mA;
	bool mHires = false;
	bool mDrawn = false;
	std::vector<uint8_t> mPix;
	uint32_t mVersion = 0;
	std::vector<InvHotspot> mTabHot, mHot;
	std::vector<std::string> mProblems;
};

} // namespace roth
