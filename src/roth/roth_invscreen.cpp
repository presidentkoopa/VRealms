//
// The inventory screen's rules. See roth_invscreen.h.
//

#include "roth_invscreen.h"
#include "roth_gamecore.h"
#include "roth_dialogue.h"

namespace roth
{

void InventoryScreen::Bind(GameCore *core, DialogueUi *ui, Dbase100 *db)
{
	mCore = core;
	mUi = ui;
	mDb = db;
}

int InventoryScreen::SlotOfEntry(int entry) const
{
	if (entry < 0 || entry >= (int)mList.slots.size()) return -1;
	return mList.slots[(size_t)entry];
}

// g_current_cursor_entry (0x7fef0) only ever points at a GRID entry: it is
// set from scroll + cell with the index bounded by the grid count
// (dialogue_ui.c:1771, 1787).
int InventoryScreen::HeldEntry() const
{
	if (mCore == nullptr) return -1;
	const int held = mCore->Items().Held();
	if (held < 0) return -1;
	for (size_t i = 0; i < mList.slots.size(); i++)
		if (mList.slots[i] == held) return (int)i;
	return -1;
}

// build_inventory_entry_list (inventory.c:1314-1368) for the open list.
void InventoryScreen::Rebuild()
{
	if (mCore == nullptr || mDb == nullptr) return;
	mList = mCore->Items().BuildEntryList(*mDb, g_cursor_active_list, mCore->InvSink());
}

// refresh_inventory_panel (inventory.c:1638-1647).
void InventoryScreen::RefreshPanel()
{
	g_inventory_dirty_flags |= 2;                                           // :1640
	Rebuild();                                                              // :1641
	Draw().RefreshInventoryGrid();                                          // :1642
	Draw().DrawPanelSlotTile(0xa);                                          // :1643
	Draw().DrawEquippedItemLeft();                                          // :1644
	Draw().DrawPanelSlotTile(0xb);                                          // :1645
	Draw().DrawEquippedItemRight();                                         // :1646
}

// clear_dual_array_80afc (renderer.c:10843-10848).
void InventoryScreen::ClearTabCursors()
{
	for (int i = 0; i < 5; i++) { g_cursor_list_positions[i] = 0; g_cursor_scroll_offsets[i] = 0; }
}

// select_inventory_tab (inventory.c:1560-1567).
void InventoryScreen::SelectInventoryTab()
{
	g_cursor_active_list = TabCategory(g_inventory_active_tab);
	Rebuild();
	g_inventory_dirty_flags += 2;                                           // :1565
	Draw().DrawInventoryTabs();                                             // :1566
}

// move_cursor_entry_clamped (renderer.c:10988-11001).
void InventoryScreen::MoveCursorEntryClamped(int delta)
{
	const int l = ListIndex();
	const uint32_t newv = (uint32_t)delta + (uint32_t)g_cursor_scroll_offsets[l];
	if (newv >= (uint32_t)EntryCount()) return;
	if (newv == (uint32_t)g_cursor_list_positions[l]) return;
	g_cursor_list_positions[l] = (int32_t)newv;
	if (g_inventory_dirty_flags & 2) return;                                // renderer.c:10997-10999
	g_inventory_dirty_flags += 2;
}

// scroll_entry_into_view (menu_hud_ui.c:89-112).
void InventoryScreen::ScrollEntryIntoView(int entry)
{
	if (entry < 0 || entry >= EntryCount()) return;
	const int l = ListIndex();
	g_cursor_list_positions[l] = entry;
	while (entry < g_cursor_scroll_offsets[l]) g_cursor_scroll_offsets[l] -= ROW;
	const int iv = entry - 9;
	if (iv > 0) while (g_cursor_scroll_offsets[l] < iv) g_cursor_scroll_offsets[l] += ROW;
}

//==========================================================================
//
// Open and close.
//
//==========================================================================

// render_inventory_panel (inventory.c:1853-1950), the state it sets: the
// inspect request and both exit requests cleared, the list rebuilt for the
// list that was open last time, nothing held.
void InventoryScreen::Open()
{
	if (mCore == nullptr || !mCore->IsAttached()) return;                   // :1861, no dbase
	if (g_inventory_panel_open != 0) return;                                // :1857
	g_inventory_inspect_request = -1;                                       // :1860
	g_popup_exit_request = 0;                                               // :1862
	g_ui_close_request = 0;
	g_inventory_panel_open = 1;                                             // :1866
	Rebuild();                                                              // :1893 (clears the held cursor)
	g_inventory_dirty_flags += 4;                                           // :1902
	Draw().RenderInventoryPanel();                                          // :1864, 1886-1942
	g_inventory_dirty_flags += 1;                                           // :1943
	g_inventory_panel_open = 2;                                             // menu_hud_ui.c:180
}

// close_inventory_panel (inventory.c:1653-1684).
void InventoryScreen::Close()
{
	if (g_inspect_popup_active != 0) return;                                // :1655
	if (g_inventory_dirty_flags & 4) g_inventory_dirty_flags -= 4;          // :1657-1660
	if (g_inventory_dirty_flags & 1) g_inventory_dirty_flags -= 1;          // :1661-1662
	if (mCore != nullptr)
	{
		mCore->Items().DropHeld();                                          // :1656
		mCore->InvSink()->RightHandChanged(mCore->Items().RightHand());     // :1676, update_selected_item_icon
	}
	g_inventory_panel_open = 0;                                             // :1672
	g_weapon_hud_dirty = 0;                                                 // :1677-1680
	mList = InventoryEntryList();
}

// update_inventory_screen's tail (inventory.c:1819-1845).
void InventoryScreen::Post()
{
	if (mCore == nullptr || mDb == nullptr) return;
	if (g_inventory_inspect_request != -1)                                  // :1824-1836
	{
		int slot = -1;
		if (g_inventory_inspect_request == -2) slot = mList.shownLeft;
		else if (g_inventory_inspect_request == -3) slot = mList.shownRight;
		else slot = SlotOfEntry(g_inventory_inspect_request);
		g_inventory_inspect_request = -1;
		if (slot >= 0)
		{
			const int32_t id = (int16_t)mCore->Items().Slot(slot).id;
			const DbItem *rec = mDb->Item(id);
			// Only an item with a close-up has a popup (:1831-1833).
			if (rec != nullptr && rec->ok && rec->closeup != 0) OpenPopup(id);
		}
	}
	if (g_ui_close_request != 0 || g_inventory_panel_open == 0) Close();    // :1837-1840
}

//==========================================================================
//
// Clicks -- dispatch_dialogue_ui_action, dialogue_ui.c:1556-1808.
//
//==========================================================================

// Actions 0x17..0x1b (:1636-1645). The hotspot's action is 0x17 + the tab's
// CATEGORY (draw_inventory_tabs, inventory.c:1545-1546), and the click sets
// the list straight from it. g_inventory_active_tab is NOT touched: only the
// keys move it, so Tab after a click continues from where the keys left it.
void InventoryScreen::ClickTab(int tab)
{
	g_cursor_active_list = TabCategory(tab);
	Rebuild();
	g_inventory_dirty_flags += 2;                                           // :1640
	Draw().DrawInventoryTabs();                                             // :1641
}

void InventoryScreen::ClickCell(int cell, int button, bool synthetic)
{
	if (mCore == nullptr || mDb == nullptr) return;
	if (cell < 0 || cell >= CELLS) return;
	Inventory &inv = mCore->Items();
	InventorySink *sink = mCore->InvSink();
	const bool primary = button == PRIMARY;
	const bool secondary = button == SECONDARY;
	const int l = ListIndex();
	const int entry = g_cursor_scroll_offsets[l] + cell;                    // :1741
	const int held = HeldEntry();

	if (held >= 0 && entry == held)                                         // :1743
	{
		if (secondary) { inv.DropHeld(); return; }                          // :1744
		if (primary || synthetic) { inv.UseHeldOnSelf(*mDb, sink); return; }   // :1745-1747
	}
	if (held >= 0)                                                          // :1749
	{
		if (entry >= EntryCount()) return;                                  // :1751
		if (primary || synthetic)                                           // :1752-1755
		{
			inv.CombineHeldWith(*mDb, SlotOfEntry(entry), sink);
			return;
		}
		// :1756-1762. (The two tests against the hand BOXES' entries there
		// can never hold: the cursor only ever holds a grid entry.)
		const int heldSlot = inv.Held();
		inv.SwapSlots(SlotOfEntry(entry), heldSlot, sink);
		inv.DropHeld();
		g_inventory_dirty_flags |= 2;                                       // :1763
		Draw().RefreshInventoryGrid();                                      // :1800
		return;
	}

	// :1765-1789, nothing held.
	MoveCursorEntryClamped(cell);                                           // :1767
	if ((!(button & 2) && !(button & 8)) || entry >= EntryCount())          // :1768-1769
	{
		Draw().RefreshInventoryGrid();                                      // :1793
		if (entry < EntryCount()) g_inventory_inspect_request = entry;      // :1795-1796: examine
		return;
	}
	const bool wasLeft = entry == mList.leftEntry, wasRight = entry == mList.rightEntry;
	inv.PickUpToCursor(SlotOfEntry(entry), sink);                           // :1770-1787
	// The hand the entry was in is emptied at once, its box with it
	// (:1772-1785): the trackers do not wait for the next list build.
	if (wasLeft)
	{
		mList.leftEntry = -1; mList.shownLeft = -1;
		Draw().DrawPanelSlotTile(0xa);                                      // :1777
		g_inventory_dirty_flags += 2;                                       // :1778
	}
	if (wasRight)
	{
		mList.rightEntry = -1; mList.shownRight = -1;
		Draw().DrawPanelSlotTile(0xb);                                      // :1784
		g_inventory_dirty_flags += 2;                                       // :1785
	}
	Draw().RefreshInventoryGrid();                                          // :1800
}

// Actions 0x2a (left) and 0x2b (right), :1685-1737.
void InventoryScreen::ClickHandBox(int right, int button)
{
	if (mCore == nullptr || mDb == nullptr) return;
	Inventory &inv = mCore->Items();
	InventorySink *sink = mCore->InvSink();
	// The boxes are dead while an item is held on the characters list
	// (hit_test_dialogue_ui_action, dialogue_ui.c:592-596).
	if (g_cursor_active_list == 2 && inv.Held() >= 0) return;

	if (button == PRIMARY)                                                  // :1688-1693, 1716-1721
	{
		const int shown = right ? mList.shownRight : mList.shownLeft;
		if (shown < 0) return;
		g_inventory_inspect_request = right ? -3 : -2;
		Draw().FormatInventoryItemLabel((int16_t)inv.Slot(shown).id, 0);    // :1690, 1718
		return;
	}
	if (button != SECONDARY) return;                                        // :1694, 1722
	const bool had = inv.Held() >= 0;
	if (had)
	{
		if (right) inv.HeldToRightHand(sink); else inv.HeldToLeftHand(sink);    // :1696-1697, 1724-1725
		Rebuild();                                                          // :1698, 1726
		g_inventory_dirty_flags += 2;                                       // :1699, 1727
		Draw().DrawPanelSlotTile(right ? 0xb : 0xa);                        // :1700, 1728
		if (right) Draw().DrawEquippedItemRight(); else Draw().DrawEquippedItemLeft();   // :1701, 1729
		Draw().RefreshInventoryGrid();                                      // :1800
		return;
	}
	inv.DropHeld();                                                         // :1695, 1723
	const int hand = right ? inv.RightHand() : inv.LeftHand();
	if (hand >= 0)                                                          // :1704, 1732
	{
		g_inventory_active_tab = ItemTabIndex(*mDb, (int16_t)inv.Slot(hand).id);   // :1706, 1734
		SelectInventoryTab();
		ScrollEntryIntoView(right ? mList.rightEntry : mList.leftEntry);    // :1708, 1736
	}
	Draw().RefreshInventoryGrid();                                          // :1800
}

// Action 0x26 (:1656-1662): the click landed outside the panel.
void InventoryScreen::ClickOutside()
{
	KeySelect();
}

void InventoryScreen::HoverCell(int cell)
{
	if (cell < 0 || cell >= CELLS) return;
	MoveCursorEntryClamped(cell);
}

// Actions 0x28 / 0x29 (:1663-1681).
void InventoryScreen::ScrollUp()
{
	const int l = ListIndex();
	if (g_cursor_scroll_offsets[l] <= 0) return;
	g_inventory_dirty_flags += 2;                                           // :1663
	g_cursor_scroll_offsets[l] -= ROW;
}

void InventoryScreen::ScrollDown()
{
	const int l = ListIndex();
	const int lim = EntryCount() - 9;
	if ((uint32_t)lim <= (uint32_t)g_cursor_scroll_offsets[l]) return;
	g_inventory_dirty_flags += 2;                                           // :1674
	g_cursor_scroll_offsets[l] += ROW;
}

//==========================================================================
//
// Keys -- update_inventory_screen, inventory.c:1692-1817.
//
//==========================================================================

// L_tab (:1748-1757): the already-open tab's own key does nothing at all.
void InventoryScreen::KeyTab(int tab)
{
	if (tab == g_inventory_active_tab) return;
	g_inventory_active_tab = tab;
	if (tab >= 5) g_inventory_active_tab = 0;
	SelectInventoryTab();
}

void InventoryScreen::KeyNextTab()
{
	KeyTab((int16_t)g_inventory_active_tab + 1);                            // :1707
}

// L_sel (:1741-1745).
void InventoryScreen::KeySelect()
{
	if (mCore == nullptr) return;
	if (mUi != nullptr && mUi->TryInterruptDialogueVoice() != 0) return;    // a line was up: the key skips it instead
	g_ui_close_request += 1;
	mCore->Items().CommitHeld(g_cursor_active_list, mCore->InvSink());
}

// Enter (:1709-1723).
void InventoryScreen::KeyEnter()
{
	if (mCore == nullptr) return;
	if (g_cursor_active_list == 2) return;                                  // :1710
	const int pos = g_cursor_list_positions[ListIndex()];
	mCore->Items().EnterOnSlot(g_cursor_active_list, SlotOfEntry(pos), mCore->InvSink());
	KeySelect();
}

void InventoryScreen::KeyLeft()                                             // :1758-1766
{
	const int l = ListIndex();
	if (g_cursor_list_positions[l] <= 0) return;
	g_cursor_list_positions[l] -= 1;
	if (g_cursor_list_positions[l] < g_cursor_scroll_offsets[l]) g_cursor_scroll_offsets[l] -= ROW;
	g_inventory_dirty_flags += 2;                                           // :1757
}

void InventoryScreen::KeyRight()                                            // :1767-1776
{
	const int l = ListIndex();
	if ((uint32_t)(EntryCount() - 1) <= (uint32_t)g_cursor_list_positions[l]) return;
	g_cursor_list_positions[l] += 1;
	if (g_cursor_scroll_offsets[l] + 9 < g_cursor_list_positions[l]
		&& (uint32_t)(EntryCount() - 9) > (uint32_t)g_cursor_scroll_offsets[l])
		g_cursor_scroll_offsets[l] += ROW;
	g_inventory_dirty_flags += 2;                                           // :1767
}

void InventoryScreen::KeyUp()                                               // :1777-1784
{
	const int l = ListIndex();
	if (g_cursor_list_positions[l] >= ROW) g_cursor_list_positions[l] -= ROW;
	if (g_cursor_list_positions[l] < g_cursor_scroll_offsets[l]) g_cursor_scroll_offsets[l] -= ROW;
	g_inventory_dirty_flags += 2;                                           // :1775
}

void InventoryScreen::KeyDown()                                             // :1785-1798
{
	const int l = ListIndex();
	const int count = EntryCount();
	const uint32_t rem = (uint32_t)count % 5;
	const int e = rem != 0 ? (int)rem : 5;
	const int lim = (5 - e) + count;
	const int np = g_cursor_list_positions[l] + ROW;
	if (np < lim) g_cursor_list_positions[l] = np;
	if ((uint32_t)g_cursor_list_positions[l] >= (uint32_t)count) g_cursor_list_positions[l] = count - 1;
	if (g_cursor_scroll_offsets[l] + 9 < g_cursor_list_positions[l]
		&& (uint32_t)(count - 9) > (uint32_t)g_cursor_scroll_offsets[l])
		g_cursor_scroll_offsets[l] += ROW;
	g_inventory_dirty_flags += 2;                                           // :1789
}

void InventoryScreen::KeyExamine()                                          // :1799-1804
{
	const int p = g_cursor_list_positions[ListIndex()];
	if ((uint32_t)p >= (uint32_t)EntryCount()) return;
	g_inventory_inspect_request = p;
}

// 'S' and 'U' (:1708, 1724) set g_inventory_ui_action; the click router then
// turns that into a click on the cursor's own cell with code 8
// (hit_test_dialogue_ui_action, dialogue_ui.c:555-575): 'U' with the
// synthetic-primary flag.
void InventoryScreen::KeyHold()
{
	const int l = ListIndex();
	const int d = g_cursor_list_positions[l] - g_cursor_scroll_offsets[l];
	if ((uint32_t)d < (uint32_t)CELLS) ClickCell(d, KEY, false);
}

void InventoryScreen::KeyUse()
{
	const int l = ListIndex();
	const int d = g_cursor_list_positions[l] - g_cursor_scroll_offsets[l];
	if ((uint32_t)d < (uint32_t)CELLS) ClickCell(d, KEY, true);
}

//==========================================================================
//
// The examine popup -- load_dbase300_resource_at_offset, file_config.c:900-1144.
//
// The original plays the item's close-up film from DBASE300 in a window,
// runs the item's OnInspect block the first time it is ever looked at, and
// then (for an item whose close-up holds, or one with a document) stays up
// as a modal with the magnifier, the "i" topics, the document pages and a
// close box. The film is the cutscene decoder's; everything else is here.
//
//==========================================================================

void InventoryScreen::OpenPopup(int32_t itemId)
{
	if (mCore == nullptr || mDb == nullptr || mUi == nullptr) return;
	const DbItem *rec = mDb->Item(itemId);
	if (rec == nullptr || !rec->ok) return;
	g_inspect_popup_active = 1;                                             // :914
	mPopupItem = itemId;                                                    // :949
	g_popup_exit_request = 0;                                               // render_inspect_popup_window, dialogue_ui.c:1314
	mPopupMarks = 0;

	// The document pages: copy_record_block_op7 (dialogue_ui.c:488-510;
	// render_inspect_popup_window, :1334-1345). At most 0x20.
	mPages.clear();
	if (rec->flags & DBI_HAS_DOCUMENT)
	{
		const DbTriggerBlock *doc = rec->FindBlock(DBTRIG_DOCUMENT);
		if (doc != nullptr)
			for (int i = 0; i < doc->commandCount && i < 0x20; i++)
				mPages.push_back(mDb->U32At(doc->firstCommand + (uint32_t)i * 4));
	}
	if (!mPages.empty() && (uint32_t)g_inspect_current_page >= mPages.size())
		g_inspect_current_page = (int32_t)mPages.size() - 1;                // :1341-1342
	if (mPages.empty()) g_inspect_current_page = 0;

	mUi->BuildAvailableChoiceMenu(itemId);                                  // dialogue_ui.c:1350

	// First view: set the bit, then run OnInspect (file_config.c:962-967).
	ExamineFirstView(*mDb, itemId, mCore->InvSink());
}

void InventoryScreen::ClosePopup()
{
	// file_config.c:1134-1143.
	mPopupItem = 0;
	if (mUi != nullptr) mUi->g_choice_interaction_mode = 0;
	mPopupMarks = 0;
	g_inspect_popup_active = 0;
	mPages.clear();
}

bool InventoryScreen::PopupTopicsShown() const
{
	return g_inspect_popup_active != 0 && mUi != nullptr && mUi->g_choice_interaction_mode == 1;
}

// update_inspect_popup_choices (dialogue_ui.c:1255-1283), then the loop's
// exit test (file_config.c:1118-1122).
void InventoryScreen::PopupFrame()
{
	if (g_inspect_popup_active == 0 || mUi == nullptr || mCore == nullptr) return;
	Dbase100InterpState &s = mCore->Interp().State();
	if (mUi->g_choice_interaction_mode == 2 && s.g_dialogue_busy_flag == 0)     // :1257-1260
	{
		mUi->BuildAvailableChoiceMenu(mPopupItem);
		mUi->g_choice_interaction_mode = 1;
	}
	mUi->VoiceStreamPump();                                                 // :1261
	if (s.g_dialogue_busy_flag == 0 && mPopupMarks != 0)                    // :1262-1271: the lit buttons go out
	{
		if (mPopupMarks & 1) mPopupMarks -= 1;
		if (mUi->g_choice_interaction_mode == 0 && (mPopupMarks & 2)) mPopupMarks -= 2;
	}
	if (mUi->g_pending_choice_accept_index != 0)                            // :1273-1276
	{
		mUi->ActivateSelectedChoiceRecord((uint32_t)mUi->g_pending_choice_accept_index);
		mUi->g_pending_choice_accept_index = 0;
	}
	if (g_popup_exit_request != 0 || g_inventory_panel_open == 0) ClosePopup();
}

// Action 2 (dialogue_ui.c:1569-1578): the magnifier.
void InventoryScreen::PopupMagnifier()
{
	if (g_inspect_popup_active == 0 || mPopupItem == 0 || mCore == nullptr || mDb == nullptr) return;
	ReplayInspect(*mDb, mPopupItem, mCore->InvSink());
	if (!(mPopupMarks & 1)) mPopupMarks += 1;
}

// Action 3 (dialogue_ui.c:1580-1594): the "i" button shows or hides the topics.
void InventoryScreen::PopupToggleTopics()
{
	if (g_inspect_popup_active == 0 || mUi == nullptr) return;
	if (mUi->g_choice_line_count == 0) return;                              // :1581
	if (mUi->g_choice_interaction_mode == 0)
	{
		mUi->g_choice_interaction_mode = 1;
		if (!(mPopupMarks & 2)) mPopupMarks += 2;
		return;
	}
	mUi->g_choice_interaction_mode = 0;
}

// Actions 7..0x16 (dialogue_ui.c:1630-1632): a click on topic `row`. The
// record runs on the popup's next frame.
void InventoryScreen::PopupTopic(int row)
{
	if (g_inspect_popup_active == 0 || mUi == nullptr) return;
	if (row < 0 || row >= mUi->TopicCount()) return;
	mUi->g_pending_choice_accept_index = row + 1;
}

// Action 1 (dialogue_ui.c:1566).
void InventoryScreen::PopupClose()
{
	if (g_inspect_popup_active == 0) return;
	g_popup_exit_request = 1;
}

// Esc / I / Space (file_config.c:1071-1073): skip a line first; with nothing
// to skip, leave.
void InventoryScreen::PopupKeyExit()
{
	if (g_inspect_popup_active == 0 || mUi == nullptr) return;
	if (mUi->TryInterruptDialogueVoice() == 0) g_popup_exit_request = 1;
}

// Enter (file_config.c:1074-1082).
void InventoryScreen::PopupKeyEnter()
{
	if (g_inspect_popup_active == 0 || mUi == nullptr || mCore == nullptr) return;
	const Dbase100InterpState &s = mCore->Interp().State();
	if (s.g_choice_selected_index == -1
		|| (s.g_move_freeze_gate != DialogueUi::GATE_MENU && mUi->g_choice_interaction_mode != 1))
	{
		if (mUi->TryInterruptDialogueVoice() == 0) g_popup_exit_request = 1;
	}
	else mUi->ChoiceAcceptSelected();
}

// Up / Down (file_config.c:1085-1092, 1108-1116): with the topics hidden the
// keys scroll the close-up, which is presentation.
void InventoryScreen::PopupKeyUp()
{
	if (g_inspect_popup_active != 0 && mUi != nullptr && mUi->g_choice_interaction_mode != 0) mUi->ChoiceSelectPrev();
}

void InventoryScreen::PopupKeyDown()
{
	if (g_inspect_popup_active != 0 && mUi != nullptr && mUi->g_choice_interaction_mode != 0) mUi->ChoiceSelectNext();
}

// Actions 5 / 6 (dialogue_ui.c:1603-1627).
void InventoryScreen::PopupPageUp()
{
	if (g_inspect_current_page == 0) return;
	g_inspect_current_page -= 1;
}

void InventoryScreen::PopupPageDown()
{
	if (mPages.empty()) return;
	if ((uint32_t)(mPages.size() - 1) <= (uint32_t)g_inspect_current_page) return;
	g_inspect_current_page += 1;
}

} // namespace roth
