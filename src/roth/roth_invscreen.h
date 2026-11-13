#pragma once
//
// Realms of the Haunting's inventory SCREEN: which verb a click or a key
// reaches, and the examine popup behind it. Engine-free. The verbs
// themselves (use, combine, the hands, the cursor-held item) are already in
// roth_inventory.h; this is the part of ROTH.C that decides which of them a
// given input means, so that a panel -- any panel -- offers what the
// original's panel offered.
//
// WHERE THE RULES COME FROM (ROTH.C, roth_c/src/engine):
//   dispatch_dialogue_ui_action      dialogue_ui.c:1556-1808   every click
//   hit_test_dialogue_ui_action      dialogue_ui.c:547-662     what a click is ON, as far as it is a rule
//   update_inventory_screen          inventory.c:1692-1845     every key, and the close
//   render_inventory_panel           inventory.c:1853-1950     what opening resets
//   close_inventory_panel            inventory.c:1653-1684
//   select_inventory_tab             inventory.c:1560-1567
//   move_cursor_entry_clamped        renderer.c:10988-11001
//   scroll_entry_into_view           menu_hud_ui.c:89-112
//   load_dbase300_resource_at_offset file_config.c:900-1144    the examine popup's life
//   update_inspect_popup_choices     dialogue_ui.c:1255-1283
//   copy_record_block_op7            dialogue_ui.c:488-510     an item's document pages
//
// WHAT A CLICK MEANS, in the original's words. "Primary" is the left button,
// "secondary" the right.
//   nothing held, primary on an item ...... examine it (its close-up popup)
//   nothing held, secondary on an item .... take it onto the cursor
//   held, secondary on the held item ...... put it back
//   held, primary on the held item ........ use it on yourself
//   held, primary on another item ......... combine the two
//   held, secondary on another item ....... swap their places
//   held, secondary on a hand box ......... it goes into that hand
//   nothing held, secondary on a hand box . jump to the tab that hand's item is on
//   primary on a hand box ................. examine the item in that hand
//   a click outside the panel ............. commit the held item and close
//
// The cell geometry (ten cells, five to a row) is the original's and is kept
// because the keys depend on it: Left / Right move one cell, Up / Down five,
// and the list scrolls by a row.
//

#include "roth_inventory.h"

#include <stdint.h>
#include <vector>

namespace roth
{

class GameCore;
class DialogueUi;
class Dbase100;

// The original's drawing calls that its inventory RULES make, at the points
// they make them (dispatch_dialogue_ui_action, update_inventory_screen,
// render_inventory_panel, refresh_inventory_panel). The original paints its
// panel incrementally: what is on screen depends on which of these ran, not
// only on the state, so a picture drawn as the original draws it needs the
// same calls in the same order. Each default does nothing; roth_invdraw's
// InventoryPanelPainter implements them.
class InventoryScreenDraw
{
public:
	virtual ~InventoryScreenDraw() {}
	// render_inventory_panel's drawing (inventory.c:1864, 1886-1942), after Open's rules.
	virtual void RenderInventoryPanel() {}
	virtual void DrawInventoryTabs() {}                                 // 0x1a2d2
	virtual void DrawPanelSlotTile(int n) { (void)n; }                  // 0x19ee6
	virtual void DrawEquippedItemLeft() {}                              // 0x1a2ef
	virtual void DrawEquippedItemRight() {}                             // 0x1bfaa
	virtual void FormatInventoryItemLabel(int32_t itemId, int32_t qty) { (void)itemId; (void)qty; }   // 0x1c0b1
	virtual void RefreshInventoryGrid() {}                              // 0x1c469
};

class InventoryScreen
{
public:
	static const int CELLS = 10;            // dialogue_ui.c:560 (d < 0xa); actions 0x1c..0x25
	static const int ROW = 5;               // inventory.c:1759, 1786

	enum Button { PRIMARY = 1, SECONDARY = 2, KEY = 8 };

	// `db` is the same database the core is attached to, writable: examining an
	// item for the first time sets a bit in its record (file_config.c:962-963).
	void Bind(GameCore *core, DialogueUi *ui, Dbase100 *db);
	// The panel's painter, or null for none (the rules alone).
	void SetDraw(InventoryScreenDraw *draw) { mDraw = draw; }

	// ---- the panel ---------------------------------------------------------
	bool IsOpen() const { return g_inventory_panel_open != 0; }
	void Open();                            // render_inventory_panel
	// close_inventory_panel. Refused while the examine popup is up (:1655).
	void Close();
	// The tail of update_inventory_screen (inventory.c:1819-1845), once a
	// frame after the input: open the examine popup the input asked for,
	// close if asked.
	void Post();

	// ---- clicks (dispatch_dialogue_ui_action) -------------------------------
	void ClickTab(int tab);                                 // actions 0x17..0x1b
	void ClickCell(int cell, int button, bool synthetic = false);   // 0x1c..0x25
	void ClickHandBox(int right, int button);               // 0x2a left / 0x2b right
	void ClickOutside();                                    // 0x26
	void ScrollUp();                                        // 0x28
	void ScrollDown();                                      // 0x29
	// The pointer resting on grid cell `cell` (handle_cursor_click's hover,
	// input.c:1290-1297): the cursor follows it.
	void HoverCell(int cell);
	// Put the cursor on entry `entry` of the open list (for a test command).
	void CursorTo(int entry) { ScrollEntryIntoView(entry); }
	// The tab key's own state, for a panel to show which tab is lit.
	int ActiveTab() const { return g_inventory_active_tab; }

	// ---- keys (update_inventory_screen) --------------------------------------
	void KeyTab(int tab);           // '1'..'5'
	void KeyNextTab();              // Tab
	void KeySelect();               // Esc / I: commit the held item and close
	void KeyEnter();                // put the cursor's item in a hand, then as KeySelect
	void KeyLeft();
	void KeyRight();
	void KeyUp();
	void KeyDown();
	void KeyExamine();              // Space: examine the cursor's item
	void KeyHold();                 // S: as a secondary click on the cursor's cell
	void KeyUse();                  // U: as a primary click on the cursor's cell

	// ---- the examine popup ------------------------------------------------------
	bool PopupUp() const { return g_inspect_popup_active != 0; }
	int32_t PopupItem() const { return mPopupItem; }
	// One frame of the popup's own loop (update_inspect_popup_choices, and the
	// loop's exit test, file_config.c:1118-1122).
	void PopupFrame();
	void PopupMagnifier();          // action 2: run OnInspect again
	void PopupToggleTopics();       // action 3: the "i" button
	void PopupTopic(int row);       // actions 7..0x16: pick topic `row`
	void PopupClose();              // action 1: the close box
	void PopupKeyExit();            // Esc / I / Space (file_config.c:1071-1073)
	void PopupKeyEnter();           // file_config.c:1074-1082
	void PopupKeyUp();
	void PopupKeyDown();
	void PopupPageUp();             // action 5
	void PopupPageDown();           // action 6
	// The item's document pages: DBASE300 resource numbers (offset / 8), from
	// its Document block.
	const std::vector<uint32_t> &PopupPages() const { return mPages; }
	int PopupPage() const { return g_inspect_current_page; }
	bool PopupTopicsShown() const;

	// ---- what a panel draws --------------------------------------------------------
	const InventoryEntryList &Entries() const { return mList; }
	int EntryCount() const { return (int)mList.slots.size(); }
	int HeldEntry() const;          // g_current_cursor_entry as an entry index, -1 none
	int CursorEntry() const { return g_cursor_list_positions[ListIndex()]; }
	int ScrollOffset() const { return g_cursor_scroll_offsets[ListIndex()]; }
	int ActiveList() const { return g_cursor_active_list; }

	// InventorySink::RefreshPanel / ClearTabCursors / PanelCloseRequested land here.
	void RefreshPanel();
	void ClearTabCursors();
	bool PanelCloseRequested() const { return g_ui_close_request != 0; }
	// DBASE100 ops 0x07 / 0x0e ask an open popup to go (dbase100.c:914, 929).
	void ArmExit() { g_inventory_panel_open = 2; g_popup_exit_request = 1; }
	// Op 0x2d sub-code 7 (dbase100.c:894-895).
	void RequestClose() { g_ui_close_request = 1; }

	// 0x7f571, a byte the rules and the drawing share: bit 0 the panel is
	// drawn, bit 1 the grid must be redrawn, bit 2 the entries' icons are
	// loaded. The original ADDS 2 where it marks the grid (so a second mark
	// before the redraw carries into bit 2); this keeps that arithmetic.
	uint8_t g_inventory_dirty_flags = 0;
	int32_t g_inventory_panel_open = 0;         // 0x7fec4
	int32_t g_inspect_popup_active = 0;         // 0x7fec0
	int32_t g_popup_exit_request = 0;           // 0x7fecc "g_inventory_panel_open + 0x8"
	int32_t g_inventory_active_tab = 0;         // 0x80b70
	int32_t g_cursor_active_list = 0;           // 0x80b38: the open tab's CATEGORY
	int32_t g_cursor_list_positions[5] = {};    // 0x80afc
	int32_t g_cursor_scroll_offsets[5] = {};    // 0x80b10
	int32_t g_ui_close_request = 0;             // 0x80b2c "g_ui_panel_anchor_y + 0x4"
	int32_t g_inventory_inspect_request = -1;   // 0x80b3c, as an entry index; -2 / -3 = the left / right hand box
	int32_t g_inspect_current_page = 0;
	int32_t g_weapon_hud_dirty = 0;             // 0x810c4

private:
	int ListIndex() const { return (g_cursor_active_list >= 0 && g_cursor_active_list < 5) ? g_cursor_active_list : 0; }
	int SlotOfEntry(int entry) const;
	void Rebuild();
	void SelectInventoryTab();
	void MoveCursorEntryClamped(int delta);
	void ScrollEntryIntoView(int entry);
	void OpenPopup(int32_t itemId);
	void ClosePopup();

	InventoryScreenDraw &Draw() { return mDraw != nullptr ? *mDraw : mNoDraw; }

	GameCore *mCore = nullptr;
	Dbase100 *mDb = nullptr;
	InventoryScreenDraw *mDraw = nullptr;
	InventoryScreenDraw mNoDraw;
	DialogueUi *mUi = nullptr;
	InventoryEntryList mList;
	int32_t mPopupItem = 0;                     // 0x7fec8, as an item id
	std::vector<uint32_t> mPages;
	int32_t mPopupMarks = 0;                    // 0x80b34: bit 0 = the magnifier is lit, bit 1 = the "i" is
};

} // namespace roth
