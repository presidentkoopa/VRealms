//
// Realms of the Haunting inventory data model. Engine-free; see roth_inventory.h.
// Rule citations are docs/GAME_inventory.md (§3 state, §4 operations, §9 traps)
// and the ROTH.C lines it gives.
//

#include "roth_inventory.h"
#include "roth_dbase100.h"

namespace roth
{

namespace
{

// The record behind a slot id. The hidden bit is not part of the table index,
// so it is masked off for the LOOKUP -- matching ids against each other always
// uses the full u16 (inventory.c:498,512).
const DbItem *RecordFor(const Dbase100 &db, uint16_t slotId)
{
	return db.Item((int)(slotId & 0x7fff));
}

} // namespace

void Inventory::Reset()
{
	for (auto &s : mSlots) s = InventorySlot();
	mCount = 0;
	mLeft = mRight = -1;
	mAutoSelected = false;
}

int Inventory::FindFreeSlot()
{
	for (int i = 0; i < SLOT_COUNT; i++)
	{
		if (mSlots[i].id == 0)
		{
			mCount++;            // renderer.c:10035 -- the caller must not also
			return i;
		}
	}
	return -1;
}

void Inventory::SetRightHand(int slot, InventorySink *sink)
{
	mRight = slot;
	if (sink) sink->RightHandChanged(slot);
}

//==========================================================================
//
// query_player_inventory (inventory.c:472-531)
//
// Count mode (flags&1 == 0): the number of slots whose id equals `id`; when
// exactly one matches and the item is stackable, that slot's quantity instead.
//
// Select mode (flags&1): in the right hand -> 1; in the left hand -> 1;
// otherwise, unless the easy flag is set or flags&2, the FIRST match is put in
// the right hand, g_item_autoselected_flag is set, and the answer is STILL 0 --
// the trap behind "the first click on the locked door only selects the key"
// (GAME_inventory.md §9 trap 2, §10 test 4).
//
// The full u16 is compared everywhere, so a hidden item (bit 15) matches
// nothing -- [inferred] in the spec as a consequence of the compare at :498,512.
//
// Not modelled: the slide-timer reset on the right-hand path (presentation;
// §4 and §9 trap 7 also disagree about it).
//
//==========================================================================

int Inventory::Query(const Dbase100 &db, uint16_t id, int flags, InventorySink *sink)
{
	if ((flags & 1) == 0)
	{
		int matches = 0, last = -1;
		for (int i = 0; i < SLOT_COUNT; i++)
			if (mSlots[i].id != 0 && mSlots[i].id == id) { matches++; last = i; }
		if (matches == 1)
		{
			const DbItem *rec = RecordFor(db, id);
			if (rec && rec->IsStackable()) return mSlots[last].quantity;
		}
		return matches;
	}

	if (mRight >= 0 && mSlots[mRight].id == id && id != 0) return 1;
	if (mLeft >= 0 && mSlots[mLeft].id == id && id != 0) return 1;
	if (mEasy || (flags & 2)) return 0;
	for (int i = 0; i < SLOT_COUNT; i++)
	{
		if (mSlots[i].id != 0 && mSlots[i].id == id)
		{
			SetRightHand(i, sink);
			mAutoSelected = true;
			return 0;
		}
	}
	return 0;
}

//==========================================================================
//
// remove_inventory_item (inventory.c:246-277), in the order the spec lists:
// the left hand unequips (activate_weapon_item(0,0)); the right hand clears the
// selection; the active ammo decrements its display; then a stackable slot
// with quantity > 1 loses one, and anything else empties and the count drops.
//
// So removing one of a held stack still clears the hand. That follows the
// spec's ordering; the code was not in front of this lane -- UNVERIFIED.
//
//==========================================================================

void Inventory::RemoveFromSlot(const Dbase100 &db, int slot, InventorySink *sink)
{
	if (slot < 0 || slot >= SLOT_COUNT || mSlots[slot].id == 0) return;
	InventorySlot &s = mSlots[slot];

	if (slot == mLeft)
	{
		mLeft = -1;
		if (sink) sink->UnequipWeapon();
	}
	if (slot == mRight)
	{
		mRight = -1;
		if (sink) sink->RightHandChanged(-1);
	}
	if (sink && sink->IsActiveAmmo(s.id)) sink->AmmoDisplayChanged(-1);

	const DbItem *rec = RecordFor(db, s.id);
	if (rec && rec->IsStackable() && s.quantity > 1)
	{
		s.quantity--;
		return;
	}
	s = InventorySlot();
	mCount--;
}

int Inventory::RemoveItem(const Dbase100 &db, uint16_t id, InventorySink *sink)
{
	if (id == 0) return 0;
	if (mRight >= 0 && mSlots[mRight].id == id) { RemoveFromSlot(db, mRight, sink); return -1; }
	if (mLeft >= 0 && mSlots[mLeft].id == id) { RemoveFromSlot(db, mLeft, sink); return -1; }
	for (int i = 0; i < SLOT_COUNT; i++)
	{
		if (mSlots[i].id == id)
		{
			RemoveFromSlot(db, i, sink);
			return -1;
		}
	}
	return 0;
}

//==========================================================================
//
// set_inventory_list_filter (inventory.c:1255-1288), RAW 0x42.
//
// Mode 0 sets bit 15 on every non-category-2 slot, clears both hands and
// resets the weapon HUD. Mode 1 clears the bit. The spec names no other mode;
// anything else does nothing here -- UNVERIFIED.
//
// Trap kept for the weapon system: rebuild_weapon_inventory_list masks bit 15
// away, so hidden weapons stay in the weapon list (weapon_combat.c:501).
//
//==========================================================================

void Inventory::SetFilter(const Dbase100 &db, int mode, InventorySink *sink)
{
	if (mode == 0)
	{
		for (auto &s : mSlots)
		{
			if (s.id == 0) continue;
			const DbItem *rec = RecordFor(db, s.id);
			// A slot whose record is missing has no category to exempt it.
			// UNVERIFIED which way ROTH.C goes; no retail slot can hold one.
			if (rec && rec->Category() == DB_CATEGORY_INFO) continue;
			s.id |= HIDDEN_BIT;
		}
		mLeft = mRight = -1;
		if (sink)
		{
			sink->ResetWeaponHud();
			sink->RightHandChanged(-1);
		}
	}
	else if (mode == 1)
	{
		for (auto &s : mSlots)
			s.id &= (uint16_t)~HIDDEN_BIT;
	}
}

void Inventory::SwapSlots(int a, int b)
{
	if (a < 0 || b < 0 || a >= SLOT_COUNT || b >= SLOT_COUNT || a == b) return;
	InventorySlot t = mSlots[a];
	mSlots[a] = mSlots[b];
	mSlots[b] = t;
	if (mLeft == a) mLeft = b; else if (mLeft == b) mLeft = a;
	if (mRight == a) mRight = b; else if (mRight == b) mRight = a;
}

std::vector<uint8_t> Inventory::Serialize() const
{
	std::vector<uint8_t> out((size_t)SAVE_BYTES);
	for (int i = 0; i < SLOT_COUNT; i++)
	{
		uint8_t *p = out.data() + i * 4;
		p[0] = (uint8_t)mSlots[i].id;
		p[1] = (uint8_t)(mSlots[i].id >> 8);
		p[2] = (uint8_t)mSlots[i].quantity;
		p[3] = (uint8_t)(mSlots[i].quantity >> 8);
	}
	return out;
}

// Hands are left as they are: they are save chunk 2, restored by the savegame
// code, and chunk 2 is read before chunk 7.
bool Inventory::Deserialize(const uint8_t *data, size_t size)
{
	if (data == nullptr || size != (size_t)SAVE_BYTES) return false;
	mCount = 0;
	for (int i = 0; i < SLOT_COUNT; i++)
	{
		const uint8_t *p = data + i * 4;
		mSlots[i].id = (uint16_t)(p[0] | (p[1] << 8));
		mSlots[i].quantity = (uint16_t)(p[2] | (p[3] << 8));
		if (mSlots[i].id != 0) mCount++;   // the count is recomputed (savegame.c:1593-1594)
	}
	return true;
}

bool IsItemIdPickable(const Dbase100 &db, uint16_t renderId)
{
	if (renderId < 0x200) return false;                  // inventory.c:48
	const DbItem *rec = db.Item(db.FindItemByRenderId(renderId));
	if (rec == nullptr) return false;
	if (rec->flags & DBI_NOT_PICKABLE) return false;      // inventory.c:59
	if (rec->Category() == DB_CATEGORY_INFO) return false; // inventory.c:60-62
	return true;
}

} // namespace roth
