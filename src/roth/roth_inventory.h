#pragma once
//
// Realms of the Haunting's inventory: the slot array and the rules that touch
// it without needing anything but DBASE100. Engine-free, like the readers.
//
// THIS IS THE DATA MODEL, NOT THE WHOLE SYSTEM. ROTH_GAME_PORT.md §4 step 6,
// and HANDOFF_REMAROTH.md §14's order inside it: the slot array and its
// primitives come first and need no DBASE100; the give/use/combine rules need
// the reader (now built) AND details the docs summarise rather than state.
// Only rules docs/GAME_inventory.md states precisely are here; each cites it.
// What is NOT here, and why, is at the bottom of this file.
//
// THE STATE (GAME_inventory.md §3)
//   g_inventory_slots  0x80c30  256 x {u16 item_id, u16 quantity}; id 0 = empty;
//                               bit 15 of the id = hidden by the RAW 0x42 filter
//   g_inventory_count  0x80c2c  used-slot count, max 0x100
//   g_selected_item_secondary   LEFT hand: the equipped weapon (a slot pointer)
//   g_selected_item_primary     RIGHT hand: the selected usable item
//   g_item_autoselected_flag    set when a select-mode query auto-selects
//   g_object_select_easy_flag   user toggle: when set, scripts never auto-select
//
// A slot's id is the DBASE100 inventory table index (1-based), NOT the world
// render id. ROTH.C keeps hand SLOT POINTERS; this keeps slot indices, -1 for
// an empty hand. Save chunk 7 is the slot array raw (Serialize/Deserialize).
//

#include <stdint.h>
#include <vector>

namespace roth
{

class Dbase100;

// The consequences of an inventory change that belong to OTHER systems
// (weapons, HUD, icon). Each default does nothing; the engine side overrides
// what it has built. Named after the ROTH.C routine each one stands for.
class InventorySink
{
public:
	virtual ~InventorySink() {}
	// activate_weapon_item(0, 0): back to fists (remove_inventory_item, left hand).
	virtual void UnequipWeapon() {}
	// update_selected_item_icon after the right hand changed.
	virtual void RightHandChanged(int slot) { (void)slot; }
	// The weapon HUD reset done by the 0x42 filter (set_inventory_list_filter).
	virtual void ResetWeaponHud() {}
	// "active ammo -> decrement the display" in remove_inventory_item. The
	// ammo id lives in the weapon system, so the sink is asked.
	virtual bool IsActiveAmmo(uint16_t itemId) { (void)itemId; return false; }
	virtual void AmmoDisplayChanged(int delta) { (void)delta; }
};

struct InventorySlot
{
	uint16_t id = 0;        // DBASE100 inventory id; bit 15 = hidden
	uint16_t quantity = 0;
};

class Inventory
{
public:
	static const int SLOT_COUNT = 0x100;               // inventory.c:381
	static const uint16_t HIDDEN_BIT = 0x8000;         // inventory.c:1266,1282
	static const int SAVE_BYTES = SLOT_COUNT * 4;      // chunk 7, 0x400

	// reset_inventory: every slot empty, both hands empty, count 0, flags
	// clear. (Whether reset also clears the easy flag is UNVERIFIED: it is a
	// user toggle, so Reset leaves it alone.)
	void Reset();

	const InventorySlot &Slot(int i) const { return mSlots[(size_t)i]; }
	int Count() const { return mCount; }
	int LeftHand() const { return mLeft; }
	int RightHand() const { return mRight; }
	bool AutoSelected() const { return mAutoSelected; }
	void ClearAutoSelected() { mAutoSelected = false; }
	bool EasySelect() const { return mEasy; }
	void SetEasySelect(bool on) { mEasy = on; }

	// find_free_inventory_slot (renderer.c:10030-10041): the FIRST empty slot,
	// and it INCREMENTS THE COUNT ITSELF (renderer.c:10035, the only increment
	// in the program -- a caller must not also). -1 when every slot is used;
	// what ROTH.C does there is UNVERIFIED, because give_item refuses a full
	// pack before it ever asks (inventory.c:381).
	int FindFreeSlot();

	// Hands, as the engine-side verbs set them.
	void SetLeftHand(int slot) { mLeft = slot; }
	void SetRightHand(int slot, InventorySink *sink = nullptr);

	// query_player_inventory (inventory.c:472-531). flags&1 = select mode,
	// flags&2 = never auto-select. See the definition for the rules.
	int Query(const Dbase100 &db, uint16_t id, int flags, InventorySink *sink = nullptr);

	// remove_item (inventory.c:283-320): right hand, then left hand, then the
	// slot scan. -1 when something was removed, else 0.
	int RemoveItem(const Dbase100 &db, uint16_t id, InventorySink *sink = nullptr);

	// remove_inventory_item (inventory.c:246-277) on one slot; also what
	// consume_held_item (inventory.c:325-338) does for a given slot.
	void RemoveFromSlot(const Dbase100 &db, int slot, InventorySink *sink = nullptr);

	// set_inventory_list_filter (inventory.c:1255-1288), RAW 0x42. Mode 0
	// hides every non-category-2 slot and empties both hands; mode 1 unhides.
	void SetFilter(const Dbase100 &db, int mode, InventorySink *sink = nullptr);

	// swap_inventory_entries (inventory.c:667-674): swap two slots' words. The
	// hands follow their items. UNVERIFIED: GAME_inventory.md §7 says "slot swap
	// and hand re-point" without the code; following the item is the reading
	// that keeps a held item held.
	void SwapSlots(int a, int b);

	// Save chunk 7: the slot array raw, hidden bits included
	// (savegame.c:1410-1417). Loading recomputes the count. Hands are chunk 2,
	// saved by the savegame code, not here.
	std::vector<uint8_t> Serialize() const;
	bool Deserialize(const uint8_t *data, size_t size);

private:
	InventorySlot mSlots[SLOT_COUNT];
	int mCount = 0;
	int mLeft = -1, mRight = -1;
	bool mAutoSelected = false;
	bool mEasy = false;
};

// is_item_id_pickable (inventory.c:48-62), on the WORLD id -- the object's
// textureIndex | textureSource << 8. Ids below 0x200 never; then the record
// must exist, must not carry +4&0x80, and must not be category 2.
bool IsItemIdPickable(const Dbase100 &db, uint16_t renderId);

// NOT HERE, AND WHY
//
// give_item (inventory.c:372-462) and give_item_by_dbase_id. The docs give the
// shape (full-pack refusal, the category-2 early return, weapon ammo set BEFORE
// the stackable test, stack-or-new-slot) but not the code: the quantity a
// plain non-stackable item gets, what the category-2 path returns, how
// WeaponAction attrs+4 becomes the ammo, and give_item_by_dbase_id's search
// order. Each is a guess away from 1:1. Transcribe it from E/inventory.c.
//
// use_item_on_self, combine_held_item_with_target, resolve_item_use_action,
// find_or_autoselect_inventory_item (needs rng_next_index_for_count, whose seed
// is an open question), the label, tab membership (the 0x7123c tab bytes are
// open), the pickup lock, and examine. All need either the interpreter or
// code the docs do not transcribe.

} // namespace roth
