#pragma once
//
// WHICH ITEM A VR HAND'S USE CARRIES. Engine-free (roth_inventory.h only), so
// tools/gameproof compiles it and runs it against the original.
//
// THE ORIGINAL HAS NO "USE ITEM ON OBJECT" VERB. A left click on the world is
// activate_targeted_object (E/input.c:1026-1101): the clicked thing's own chain
// runs (run_leftclick_object_trigger, E/raw_commands.c:2920-2968), and no item
// is passed to it. The item enters through one global, the right hand
// g_selected_item_primary (0x81044), which the chain's own item test reads:
// RAW 0x27 cmd_if_not_item (E/raw_commands.c:4760-4789) ->
// query_player_inventory in select mode (E/inventory.c:472-531), or
// find_or_autoselect_inventory_item in list mode (E/inventory.c:1394-1452).
// "Use the key on the door" is: hold the key in the right hand, click the door.
//
// IN VR (owner, 2026-11-13; roth_handhold.h): the original's right-hand item is
// in the physical hand on the RIGHT side. A use from THAT hand is the click
// with the item selected: the global already holds it, nothing is done here.
// A use from the OTHER hand holds no item, so it is the click the original
// makes with NOTHING selected: g_selected_item_primary = 0 for the length of
// the use. That is a state the original reaches itself (remove_inventory_item
// E/inventory.c:259-262, a hand's cell taken onto the cursor
// E/dialogue_ui.c:1773-1786, the 0x42 filter E/inventory.c:1255-1288), and
// every rule of the item test then runs unchanged on it:
//   - count mode (rec[6] & 2 clear): carried anywhere passes, from either hand;
//   - select mode, item in the right hand: 1 -- from the item hand only;
//   - select mode, item carried but in neither hand: AUTO-SELECTED into the
//     right hand, the chain interrupted, nothing else happens
//     (E/inventory.c:493-502, E/raw_commands.c:4747-4752). The VR hand on the
//     right then holds it, and the next use from that hand succeeds;
//   - the easy flag or rec[6] & 0x20: no auto-select, the refusal runs.
// The weapon slot (the original's LEFT hand, g_selected_item_secondary, which
// query_player_inventory also accepts, E/inventory.c:489-492) is untouched:
// it is equipped, not selected, and stays with Adam whichever hand acts.
//
// AFTER THE USE. In the original nothing more happens. In VR the item never
// left the right-side hand, so the selection comes back -- but only when the
// use left the right hand empty AND the slot still holds the same item. If the
// use put something in the right hand (the auto-select, a give, a pickup) that
// is the game's own change and it stands; if the slot was emptied or hidden
// (a remove, the 0x42 filter) the item is gone and nothing comes back. No
// presentation call is made either way (update_selected_item_icon): the icon
// never changed for the player.
//
// NOT COVERED [R]: a chain that is still running after the use returns (a
// delay timer's continuation, the deferred queue) reads the right hand as it
// is THEN. The retail 0x27 tests sit at the head of their chains, before any
// delay (docs/ITEM_DESIGN.md, the Matchbox's candle chains). RAW 0x3c with
// +0x0a == 0 also reads the right hand (resolve_object_template_record,
// E/raw_commands.c:3612-3617, 4129; 11 retail records) -- 0x3c is not in this
// runtime yet; when it is, it sees the same state as 0x27 does.
//
// PORT CHECKLIST -- ROTH.C (E/ = roth_c/src/engine) -> here. Port lines as of
// this commit. "unchanged" = the existing port, already diffed by gameproof R.
//  1. the click has no item argument: activate_targeted_object E/input.c:1026-1101,
//     depth gate < 0x258 :1031 -> Activate roth_runtime.cpp:6935, called by
//     DoUse roth_vrpick.cpp:463 behind CursorClass::CanUse roth_runtime.h:422.
//  2. the object's chain: run_leftclick_object_trigger E/raw_commands.c:2920-2968
//     -> ActivateObject roth_runtime.cpp:7048 (unchanged).
//  3. the right-hand item is the global g_selected_item_primary 0x81044
//     -> Inventory::mRight (roth_inventory.h).
//  4. which VR hand holds it: the RIGHT side's (roth_hands.cpp:105 PublishHeld)
//     -> DoUse: itemHand = VRHandPose_SideForRole(role) == SIDE_RIGHT.
//  5. the item hand's use = the click with the item selected -> HandUseBegin
//     does nothing for it.
//  6. the other hand's use = the click with nothing selected (0x81044 = 0, as
//     E/inventory.c:259-262, E/dialogue_ui.c:1783, E/inventory.c:1255-1288
//     leave it) -> HandUseBegin: SetRightHand(-1).
//  7. RAW 0x27 cmd_if_not_item E/raw_commands.c:4760-4789 -> CmdIfNotItem
//     roth_itemcmds.cpp:64 (unchanged).
//  8. the select-mode query: right hand -> 1 E/inventory.c:484-488; the weapon
//     slot -> 1 :489-492; else auto-select unless easy / flags&2 :493-502
//     -> Inventory::Query roth_inventory.cpp:345 (unchanged). The weapon slot
//     is NOT cleared for the other hand: it is equipped, not selected.
//  9. auto-selected: interrupt 1, the click does nothing E/raw_commands.c:4747-4752
//     -> Route roth_itemcmds.cpp:54 (unchanged).
// 10. the wrong item / not carried: the if-next tail runs the latched refusal
//     record, interrupt 2 (or consume) E/raw_commands.c:4637-4658 -> ItemCmdTailIfNext
//     roth_itemcmds.cpp:17 (unchanged).
// 11. list mode: find_or_autoselect_inventory_item E/inventory.c:1394-1452
//     -> Inventory::FindOrAutoselect roth_inventory.cpp:416 (unchanged).
// 12. the chain's end: no sound when auto-selected, the flag cleared
//     E/raw_commands.c:1288-1300, 1313-1315 -> Fire roth_runtime.cpp:4817-4848 (unchanged).
// 13. remove_item: right hand, left hand, then the slot scan E/inventory.c:283-320
//     -> Inventory::RemoveItem roth_inventory.cpp:543 (unchanged); during the
//     other hand's use the right hand is empty, so a remove scans, as the
//     original with nothing selected.
// 14. RAW 0x3c with +0x0a == 0 spawns the right-hand item
//     E/raw_commands.c:3612-3617, 4129: NOT PORTED (0x3c is not in the runtime).
// 15. after the use: NO original counterpart (the VR item never left the
//     hand) -> HandUseEnd; the rule is checked by tools/gameproof replay (H).
// 16. no update_selected_item_icon on the set-aside or the restore: the
//     original calls it where ITS hand empties (E/inventory.c:261); here
//     the player's hand never emptied. A departure, presentation only.
// Proof: tools/gameproof section H (the original with the right hand as each
// VR hand leaves it vs this scope, every retail 0x27, modes 0 1 2 4 7).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_inventory.h"

namespace roth
{

struct HandUse
{
	bool itemHand = true;     // the hand holding the original's right-hand item
	int savedRight = -1;      // the right hand's slot, set aside for the other hand's use; -1 none
	uint16_t savedId = 0;     // that slot's id word when it was set aside
};

enum class HandUseAfter : uint8_t
{
	ItemHand,       // the item hand: nothing was set aside
	NothingHeld,    // the other hand, and the right hand was empty already
	Restored,       // the right hand's item came back
	GameSelected,   // the use put something in the right hand (auto-select, give): it stands
	ItemGone,       // the use emptied or hid the slot (remove, filter): nothing comes back
};

inline const char *HandUseAfterName(HandUseAfter a)
{
	switch (a)
	{
	case HandUseAfter::ItemHand:     return "item hand";
	case HandUseAfter::NothingHeld:  return "nothing was in the right hand";
	case HandUseAfter::Restored:     return "the right hand's item restored";
	case HandUseAfter::GameSelected: return "the use selected into the right hand";
	case HandUseAfter::ItemGone:     return "the right hand's item is gone";
	}
	return "?";
}

// Before the use. `itemHand`: the use comes from the hand holding the
// right-hand item.
inline HandUse HandUseBegin(Inventory &inv, bool itemHand)
{
	HandUse u;
	u.itemHand = itemHand;
	if (itemHand) return u;
	u.savedRight = inv.RightHand();
	if (u.savedRight < 0) return u;
	u.savedId = inv.Slot(u.savedRight).id;
	inv.SetRightHand(-1);           // g_selected_item_primary = 0, no icon call
	return u;
}

// After the use (see the header).
inline HandUseAfter HandUseEnd(Inventory &inv, const HandUse &u)
{
	if (u.itemHand) return HandUseAfter::ItemHand;
	if (u.savedRight < 0) return HandUseAfter::NothingHeld;
	if (inv.RightHand() >= 0) return HandUseAfter::GameSelected;
	if (inv.Slot(u.savedRight).id != u.savedId) return HandUseAfter::ItemGone;
	inv.SetRightHand(u.savedRight);
	return HandUseAfter::Restored;
}

} // namespace roth
