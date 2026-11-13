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
//  3. HOVER, dialogue busy: update_dialogue_choice_highlight first (:1271-1279).
//     NOT PORTED HERE: the port's dialogue owns a busy pointer
//     (roth_interface.cpp UpdateHover -> DialogueUi::UpdateChoiceHighlight), as
//     before this file. See 15.
//  4. HOVER hit test with EBX 0 (:1281-1288) -> Hover: painter.HitTest.
//  5. HOVER act < 0x1c: 0x240 for 0, else 0x248 (:1289) -> Hover.
//  6. HOVER act 0x1c..0x25 (the cells): when act differs from the latch
//     [0x81300], latch = act and move_cursor_entry_clamped(act - 0x1c)
//     (:1290-1297) -> Hover: g_hover_latch, InventoryScreen::HoverCell; shape 0x250.
//  7. HOVER act 0x26 (outside): 0x248 (:1299) -> Hover.
//  8. HOVER act 0x27 (busy only): 0x240 / 8 (:1300-1303) -> see 15.
//  9. HOVER act 0x28, 0x29: 0x248; 0x2a, 0x2b: 0x250; else 0x248 (:1304-1306) -> Hover.
// 10. CLICK: nothing to do without an edge (:1311-1312) -> Click needs button 1 or 2.
// 11. CLICK hit test with EBX = the code (:1313-1318) -> Click: painter.HitTest
//     (the code only matters with bit 8, dialogue_ui.c:555-575).
// 12. CLICK act 0x27 -> dialogue_voice_force_end (:1319-1331): busy only, see 15.
// 13. CLICK act 4 (a drag) (:1332-1340): only with code 8 or over the popup;
//     never on the panel. NOT PORTED.
// 14. CLICK act != 0 -> dispatch_dialogue_ui_action(act, code) (:1341-1346),
//     then the shape 0x268 if the secondary button is held, else 0x270
//     (:1347; the poll left the clicking button held, input.c:868) -> Click, Dispatch.
// 15. DIALOGUE BUSY (g_dialogue_busy_flag): the original's hit test answers 0x27
//     everywhere (dialogue_ui.c:552-553), so a click skips the line. The port's
//     panel has never done that (roth_interface.cpp before this file clicked the
//     cell under the pointer); it is left as it was so the mouse and keys behave
//     exactly as before. A known difference, listed in the lane's report.
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
// Proof: tools/invdraw_test scripts/pointer.txt -- the original's own
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

#include <stdint.h>
#include <functional>

namespace roth
{

class InvMouse
{
public:
	// [0x81300] ("g_inventory_synthetic_primary + 4"): the action the hover last moved the cursor for.
	int32_t g_hover_latch = 0;
	// Action 0x2c's request (0x7febc): the options box over the inventory.
	std::function<void()> openOptions;
	// The last action a click hit (for a log line); -1 before any.
	int lastAction = -1;

	// The hover at panel pixel (x, y). Returns the cursor shape the original
	// would set (set_cursor_shape's id; the port draws no cursor).
	uint16_t Hover(const InventoryPanelPainter &painter, InventoryScreen &screen, int x, int y)
	{
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
		if (act == 0x27) return 0x240;                                      // :1300-1303 (busy only)
		if (act < 0x2a) return 0x248;                                       // :1304
		if (act <= 0x2b) return 0x250;                                      // :1305
		return 0x248;                                                       // :1306
	}

	// A click of `button` (InventoryScreen::PRIMARY / SECONDARY) at panel
	// pixel (x, y). Returns the shape; lastAction says what it hit.
	uint16_t Click(const InventoryPanelPainter &painter, InventoryScreen &screen, int x, int y, int button)
	{
		if (button != InventoryScreen::PRIMARY && button != InventoryScreen::SECONDARY) return 0;   // :1311-1312
		const int act = painter.HitTest(x, y);                              // :1313-1318
		lastAction = act;
		if (act != 0) Dispatch(screen, act, button);                        // :1341-1346
		return button == InventoryScreen::SECONDARY ? 0x268 : 0x270;        // :1347
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
};

} // namespace roth
