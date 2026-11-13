#pragma once
//
// roth_invmouse.h -- THE ORIGINAL'S MOUSE ON THE INVENTORY PANEL. Engine-free.
//
// While the inventory screen is up the original runs handle_cursor_click
// (E/input.c:1260-1358) every frame (game_core.c:883-885): with no button
// edge it is the HOVER (the cursor's shape, and the cursor cell follows the
// pointer over the grid); with one it is the CLICK (hit_test_dialogue_ui_action
// at the mouse, then dispatch_dialogue_ui_action with the click code). This is
// that routine's panel half, on the port's own rules (roth_invscreen: the
// dispatch's verbs) and the port's own hit test (roth_invdraw's
// InventoryPanelPainter::HitTest over the original's hotspot lists).
//
// EVERY POINTER ON THE PANEL GOES THROUGH HERE: the flat crosshair / VR aim
// beam with the use key (roth_interface.cpp Press -> PanelClick, UpdateHover)
// and the VR hands' fingertips and lasers (roth_vrpick.cpp, through
// InterfacePanelPointer / InterfacePanelClick). So a hand's click at a point
// does exactly what the mouse's click at that point does.
//
// (x, y) are PANEL pixels: the original's g_mouse_x / g_mouse_y minus the
// panel's anchor (0x80b24 / 0x80b28, (168, 240) on the 640 x 480 screen),
// which is what the hotspot lists hold.
//
// PORT CHECKLIST -- E/ = ROTH.C roth_c/src/engine. "port" = this file unless named.
//  1. click code: secondary edge 2, primary edge +1 (input.c:1264-1266) -> `button`
//     (1 primary = the left button, 2 secondary = the right; poll_mouse_input
//     input.c:858-868). Both at once (3) and the held-button fallback (:1267)
//     are a mouse's; no caller here makes them. NOT PORTED.
//  2. code 8, a key-made click (:1268, inventory_ui_action) -> the keys call
//     InventoryScreen::KeyHold / KeyUse directly (roth_invscreen.cpp). Not here.
//  3. HOVER, dialogue busy: update_dialogue_choice_highlight at the mouse first
//     (:1271-1279); 1 (on a choice row) -> shape 0x248, nothing else
//     -> Hover: dlg.ChoiceHighlight (InvMouseDialogue).
//  4. HOVER hit test with EBX 0 (:1281-1288) -> Hover: painter.HitTest.
//  5. HOVER act < 0x1c: 0x240 for 0, else 0x248 (:1289) -> Hover.
//  6. HOVER act 0x1c..0x25 (the cells): when act differs from the latch
//     [0x81300], latch = act and move_cursor_entry_clamped(act - 0x1c)
//     (:1290-1297) -> Hover: g_hover_latch, InventoryScreen::HoverCell; shape 0x250.
//  7. HOVER act 0x26 (outside): 0x248 (:1299) -> Hover.
//  8. HOVER act 0x27 (dialogue busy: the hit test answers it everywhere,
//     dialogue_ui.c:552-553): 0x240 when the freeze gate is 0x6ffff, else 8
//     (:1300-1303) -> Hover: no cell is hovered while a line is busy.
//  9. HOVER act 0x28, 0x29: 0x248; 0x2a, 0x2b: 0x250; else 0x248 (:1304-1306) -> Hover.
// 10. CLICK: nothing to do without an edge (:1311-1312) -> Click needs button 1 or 2.
// 11. CLICK hit test with EBX = the code (:1313-1318) -> Click: painter.HitTest
//     (the code only matters with bit 8, dialogue_ui.c:555-575).
// 12. CLICK act 0x27 (busy, anywhere: dialogue_ui.c:552-553): the gate read
//     BEFORE the call, dialogue_voice_force_end at the mouse, then shape 0x268
//     and [0x7f564] = 2 under the menu gate 0x6ffff, else 0x108 and 1
//     (:1319-1331) -> Click: dlg.VoiceForceEnd; g_dev_mode_flag_4.
// 13. CLICK act 4 (a drag) (:1332-1340): only with code 8 or over the popup;
//     never on the panel. NOT PORTED.
// 14. CLICK act != 0 -> dispatch_dialogue_ui_action(act, code) (:1341-1346),
//     then the shape 0x268 if the secondary button is held, else 0x270
//     (:1347; the poll left the clicking button held, input.c:868) -> Click, Dispatch.
// 15. DIALOGUE BUSY (g_dialogue_busy_flag): the original's hit test answers 0x27
//     everywhere (dialogue_ui.c:552-553), so a click anywhere -- on the panel or
//     off it -- skips the line instead of clicking. PORTED (owner, 2026-11-13:
//     "match the original"); until then the port's panel clicked the cell under
//     the pointer. The keys that click (S and U, a click with code 8 at the mouse,
//     inventory.c:1708/1724 -> handle_cursor_click :1268) skip it the same way
//     -> KeyClick.
// 16. the click edge flags cleared on every exit (:1355-1357): the port keeps
//     no edge flags (the button is an argument).
// The dispatch (dialogue_ui.c:1556-1808), action by action, as the port
// already routed it (roth_interface.cpp PanelClick before this file):
// 17. 0x17..0x1b a tab: list = action - 0x17, latch 0x20 (:1635-1642)
//     -> InventoryScreen::ClickTab(the tab whose category it is); g_hover_latch = 0x20.
// 18. 0x1c..0x25 a cell (:1739-1800) -> InventoryScreen::ClickCell(cell, button).
// 19. 0x26 outside: commit the held item and close (:1650-1657) -> ClickOutside.
// 20. 0x28 / 0x29 the arrows: latch 0x20 first, whether or not it scrolls
//     (:1659-1679) -> ScrollUp / ScrollDown; g_hover_latch = 0x20.
// 21. 0x2a / 0x2b the hand boxes (:1683-1737) -> ClickHandBox(right, button).
// 22. 0x2c the options: the request flag 0x7febc (:1682), which
//     update_inventory_screen turns into the options box (inventory.c:1799-1817)
//     -> the host's openOptions (roth_interface: RealmsMenus::OpenOptions).
// 23. 0x27 and >= 0x2d: nothing (:1648, 1683).
// 24. the mouse's position for the code-8 click: g_mouse_x / g_mouse_y as the
//     last hover or click left them -> mouseX / mouseY.
// Proof: tools/invdraw_test scripts/pointer.txt and busy.txt -- the original's own
// handle_cursor_click at the same panel points (invprobe.c "ptr") against this,
// reached through the VR hands' path (roth_panelmap.h + roth_invhand.h).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_invdraw.h"
#include "roth_invscreen.h"
#include "roth_inventory.h"
#include "roth_dialogue.h"
#include "roth_dbase100_interp.h"

#include <stdint.h>
#include <functional>

namespace roth
{

// What the original's mouse on the panel reads of the dialogue (the port's
// DialogueUi, through InvMouseDialogueUi below; a test may give its own).
// Positions are the original's SCREEN pixels (the panel's plus its anchor).
class InvMouseDialogue
{
public:
	virtual ~InvMouseDialogue() {}
	virtual bool Busy() const = 0;                          // g_dialogue_busy_flag 0x83aea
	virtual int32_t FreezeGate() const = 0;                 // g_move_freeze_gate 0x83125
	virtual uint32_t ChoiceHighlight(int sx, int sy) = 0;   // update_dialogue_choice_highlight
	virtual void VoiceForceEnd(int sx, int sy) = 0;         // dialogue_voice_force_end
};

// The port's dialogue as the mouse reads it. The port's DialogueUi takes the
// mouse as (x + y, the choice row under it) -- dialogue_ui.c:237 and :255-266 --
// so `rowAt` turns a screen pixel into the row of the shown choice menu (-1 none).
class InvMouseDialogueUi : public InvMouseDialogue
{
public:
	InvMouseDialogueUi(DialogueUi &ui, Dbase100InterpState &st, std::function<int(int, int)> rowAt = nullptr)
		: mUi(ui), mSt(st), mRowAt(rowAt) {}
	bool Busy() const override { return mSt.g_dialogue_busy_flag != 0; }
	int32_t FreezeGate() const override { return mSt.g_move_freeze_gate; }
	uint32_t ChoiceHighlight(int sx, int sy) override { return mUi.UpdateChoiceHighlight(sx + sy, Row(sx, sy)); }
	void VoiceForceEnd(int sx, int sy) override { mUi.PointerClick(sx + sy, Row(sx, sy)); }
private:
	int Row(int sx, int sy) const { return mRowAt ? mRowAt(sx, sy) : -1; }
	DialogueUi &mUi;
	Dbase100InterpState &mSt;
	std::function<int(int, int)> mRowAt;
};

class InvMouse
{
public:
	// [0x81300] ("g_inventory_synthetic_primary + 4"): the action the hover last moved the cursor for.
	int32_t g_hover_latch = 0;
	// Action 0x2c's request (0x7febc): the options box over the inventory.
	std::function<void()> openOptions;
	// The last action a click hit (for a log line); -1 before any. 0x27: the click skipped a line.
	int lastAction = -1;
	// [0x7f564] ("g_dev_mode_flag + 4"): 2 / 1 when a click skipped a line under / not under the menu gate.
	int32_t g_dev_mode_flag_4 = 0;
	// g_mouse_x / g_mouse_y, panel pixels: where the last hover or click was (the code-8 key click's point).
	int mouseX = 0, mouseY = 0;
	// The panel's anchor on the original's screen (0x80b24 / 0x80b28): the dialogue reads screen pixels.
	int anchorX = (640 - 0x130) >> 1, anchorY = 480 >> 1;
	// The dialogue, or null (no line can be busy: the clicks are the panel's alone).
	InvMouseDialogue *dlg = nullptr;

	// The hover at panel pixel (x, y). Returns the cursor shape the original
	// would set (set_cursor_shape's id; the port draws no cursor).
	uint16_t Hover(const InventoryPanelPainter &painter, InventoryScreen &screen, int x, int y)
	{
		mouseX = x; mouseY = y;
		if (Busy())                                                         // :1271-1279, then the hit test's 0x27
		{
			if (dlg->ChoiceHighlight(anchorX + x, anchorY + y) == 1) return 0x248;
			return dlg->FreezeGate() == 0x6ffff ? 0x240 : 8;                // :1300-1303
		}
		const int act = painter.HitTest(x, y);
		if (act < 0x1c) return act == 0 ? 0x240 : 0x248;                    // :1289
		if (act <= 0x25)                                                    // :1290-1297
		{
			if (act != g_hover_latch)
			{
				g_hover_latch = act;
				screen.HoverCell(act - 0x1c);
			}
			return 0x250;
		}
		if (act < 0x27) return 0x248;                                       // :1299
		if (act < 0x2a) return 0x248;                                       // :1304
		if (act <= 0x2b) return 0x250;                                      // :1305
		return 0x248;                                                       // :1306
	}

	// A click of `button` (InventoryScreen::PRIMARY / SECONDARY) at panel
	// pixel (x, y). Returns the shape; lastAction says what it hit.
	uint16_t Click(const InventoryPanelPainter &painter, InventoryScreen &screen, int x, int y, int button)
	{
		if (button != InventoryScreen::PRIMARY && button != InventoryScreen::SECONDARY) return 0;   // :1311-1312
		mouseX = x; mouseY = y;
		if (Busy()) return SkipLine();                                      // act 0x27, :1319-1331
		const int act = painter.HitTest(x, y);                              // :1313-1318
		lastAction = act;
		if (act != 0) Dispatch(screen, act, button);                        // :1341-1346
		return button == InventoryScreen::SECONDARY ? 0x268 : 0x270;        // :1347
	}

	// The S and U keys (KeyHold / KeyUse): a click with code 8 at the mouse
	// (inventory.c:1708/1724 -> handle_cursor_click :1268): a busy line is
	// skipped, as any click skips it; otherwise the click on the cursor's cell.
	// Returns the shape (:1347: no button held, 0x270).
	uint16_t KeyClick(InventoryScreen &screen, bool use)
	{
		if (Busy()) return SkipLine();
		if (use) screen.KeyUse(); else screen.KeyHold();
		return 0x270;
	}

	// dispatch_dialogue_ui_action for the panel's actions (see 17-23).
	void Dispatch(InventoryScreen &screen, int action, int button)
	{
		if (action >= 0x17 && action <= 0x1b)                               // a tab: 0x17 + its category
		{
			for (int t = 0; t < 5; t++) if (TabCategory(t) == action - 0x17) screen.ClickTab(t);
			g_hover_latch = 0x20;                                           // :1637
		}
		else if (action >= 0x1c && action <= 0x25) screen.ClickCell(action - 0x1c, button);
		else if (action == 0x26) screen.ClickOutside();                     // the held item: commit and close
		else if (action == 0x28) { g_hover_latch = 0x20; screen.ScrollUp(); }     // :1661
		else if (action == 0x29) { g_hover_latch = 0x20; screen.ScrollDown(); }   // :1672
		else if (action == 0x2a || action == 0x2b) screen.ClickHandBox(action - 0x2a, button);
		else if (action == 0x2c) { if (openOptions) openOptions(); }        // the options: inventory.c:1799-1817
	}

private:
	bool Busy() const { return dlg != nullptr && dlg->Busy(); }
	// :1319-1331: the gate read BEFORE the call (the call may clear it).
	uint16_t SkipLine()
	{
		lastAction = 0x27;
		const bool full = dlg->FreezeGate() == 0x6ffff;
		dlg->VoiceForceEnd(anchorX + mouseX, anchorY + mouseY);
		g_dev_mode_flag_4 = full ? 2 : 1;
		return full ? 0x268 : 0x108;
	}
};

} // namespace roth
