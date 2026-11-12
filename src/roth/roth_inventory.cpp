//
// Realms of the Haunting inventory rules. Engine-free; see roth_inventory.h.
//
// Every rule is transcribed from ROTH.C (roth_c/src/engine/) and cites
// `file.c:LINE`. "AUDIT:" marks where the first version of this file, written
// from doc summaries, differed from the source and was corrected.
// "[unverified]" marks what ROTH.C does not settle.
//
// Two conventions used throughout:
//
// "THE FIRST Count() USED SLOTS". Most of ROTH.C's slot scans do not stop at
// slot 255: they walk forward, skipping empty slots, until they have seen
// g_inventory_count non-empty ones. When the count equals the number of
// non-empty slots (always, unless something has corrupted it) that is "every
// used slot". When the count is LARGER the original walks off the end of the
// array; this stops at slot 255 instead ([unverified] beyond that point). The
// count is compared UNSIGNED where the original does so, so a count driven
// below zero (see RemoveFromSlot) behaves as "larger" there too.
//
// "THE RECORD FOR A SLOT ID". ROTH.C indexes the inventory table with the
// sign-extended id. get_dbase100_inventory_entry (renderer.c:1362-1373) returns
// null for an id <= 0; the direct `table[id]` reads (inventory.c:291, 332, 521,
// 1335, 2214) have no such guard and read outside the table for a negative
// (hidden) id. RecordForId returns null for both -- exact for the first kind,
// [unverified] for the second.
//

#include "roth_inventory.h"
#include "roth_dbase100.h"

namespace roth
{

namespace
{

const DbItem *RecordForId(const Dbase100 &db, int32_t signedId)
{
	if (signedId <= 0) return nullptr;
	return db.Item(signedId);
}

// The two WeaponAction attributes give_item and the label read, parsed as
// apply_weapon_action_attributes(index, buf, limit 1) parses them
// (weapon_combat.c:338-420): the record must carry +4 & 0x10 (:350); only the
// FIRST trigger-5 block is read (limit 1, :416); op 0x14 writes attrs+4,
// negated when the opcode byte has bit 7 (:388-391); op 0x20 writes attrs+0x30
// (:401). The last word of each kind wins.
struct GiveAttrs
{
	int32_t a4 = 0;
	uint32_t a30 = 0;
};

GiveAttrs ParseGiveAttrs(const Dbase100 &db, const DbItem &rec)
{
	GiveAttrs out;
	if (!(rec.flags & DBI_WEAPON)) return out;
	for (const DbTriggerBlock &b : rec.blocks)
	{
		if (b.trigger != DBTRIG_WEAPON_ACTION) continue;
		for (int i = 0; i < b.commandCount; i++)
		{
			const uint32_t w = db.BlockCommand(b, i).raw;
			const uint32_t hi = w >> 24;
			const uint32_t val = w & 0xffffffu;
			if ((hi & 0x7f) == 0x14) out.a4 = (hi & 0x80) ? -(int32_t)val : (int32_t)val;
			else if ((hi & 0x7f) == 0x20) out.a30 = val;
		}
		break;
	}
	return out;
}

} // namespace

//==========================================================================
//
// rng_next_index_for_count (renderer.c:11130-11147)
//
// A weight by the size of the count (2 / 4 / 8 / 0x10) is taken off the
// countdown. When that reaches zero or less the countdown restarts at
// 0x10 - weight and the LCG (x * 0x5e5 + 0x29, 16 bits) steps and becomes the
// rng word. The answer is (count * rng) >> 16 from the rng BEFORE it is
// rotated left by the weight for next time.
//
//==========================================================================

int32_t RothIndexRng::NextIndexForCount(int32_t count)
{
	const int32_t weight = count > 0x100 ? 0x10 : count > 0x10 ? 8 : count > 4 ? 4 : 2;
	countdown -= weight;
	if (countdown <= 0)
	{
		countdown = 0x10 - weight;
		seed = (uint16_t)(seed * 0x5e5u + 0x29u);
		rng = seed;
	}
	const uint16_t r = rng;
	// imul edx,eax ; sar edx,0x10 -- an arithmetic shift of the 32-bit product.
	const uint32_t product = (uint32_t)count * (uint32_t)r;
	uint32_t shifted = product >> 16;
	if (product & 0x80000000u) shifted |= 0xffff0000u;
	const unsigned w = (unsigned)weight & 0xf;
	if (w) rng = (uint16_t)((r << w) | (r >> (16 - w)));
	return (int32_t)shifted;
}

//==========================================================================
//
// reset_inventory (renderer.c:10256-10261); find_free_inventory_slot
// (renderer.c:10030-10041); stack_onto_inventory_slot (renderer.c:10045-10057)
//
//==========================================================================

void Inventory::Reset()
{
	for (auto &s : mSlots) s = InventorySlot();
	mCount = 0;
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

int Inventory::FindSlotHolding(int32_t id) const
{
	int seen = 0;
	for (int i = 0; i < SLOT_COUNT; i++)
	{
		if ((uint32_t)seen >= (uint32_t)mCount) return -1; // renderer.c:10051 (unsigned compare)
		if (mSlots[i].id == 0) continue;
		if (SignedId(i) == id) return i;                   // renderer.c:10054
		seen++;
	}
	return -1;
}

void Inventory::SetRightHand(int slot, InventorySink *sink)
{
	mRight = slot;
	if (sink) sink->RightHandChanged(slot);
}

// activate_weapon_item(slot, id): the first thing it does with the slot is
// store it in g_selected_item_secondary (weapon_combat.c:433). The rest is the
// weapon system's.
void Inventory::EquipLeft(int slot, int32_t itemId, InventorySink *sink)
{
	mLeft = slot;
	if (!sink) return;
	if (slot < 0) sink->UnequipWeapon();
	else sink->EquipWeapon(slot, itemId);
}

//==========================================================================
//
// give_item (inventory.c:372-462)
//
// The four questions the handoff left open, as the source answers them:
//
//  * A PLAIN NON-STACKABLE ITEM GETS QUANTITY 0. `quantity` starts at 0 (:377)
//    and only a weapon (:392-396) or a stackable (:399) changes it.
//  * THE CATEGORY-2 EARLY RETURN RETURNS THE INDEX (:407-409 -> L_6e, :461),
//    not 0 -- so RAW 0x29 treats a repeat give of a character as a success --
//    and it skips EVERYTHING else: no select, no ammo, no fly-in, no lock.
//  * WEAPON AMMO: quantity = attrs+4 as parsed by
//    apply_weapon_action_attributes(index, buf, 1), shifted left 8 when
//    attrs+0x31 & 0x80, i.e. when the op 0x20 value has bit 15 (:392-396). The
//    slot keeps the low 16 bits (:414). A weapon that is ALSO stackable loses
//    this: the stackable test comes after and sets quantity = 1 (:398-399).
//  * give_item_by_dbase_id's order: see GiveItemByRenderId.
//
// Order of effects on the shared tail (:416-461): hand (equip, or select +
// icon), then the ammo display, then the fly-in and its lock, then the weapon
// list, then return.
//
//==========================================================================

int32_t Inventory::GiveItem(const Dbase100 &db, int32_t index, const GiveContext *ctx, InventorySink *sink)
{
	if (mCount >= SLOT_COUNT) return 0;                                   // :381 inventory full
	const int32_t di = (int16_t)(uint16_t)index;                          // :383 only the low 16 bits count
	if (di > (int32_t)db.Header().inventoryCount) return 0;               // :384 signed `jg`: index == count is VALID
	if (di == 0)                                                          // :385-386 resolve through ctx+4
	{
		// [unverified] index 0 with no context: the original reads word [0+4]
		// through a null pointer. Refused here.
		if (ctx == nullptr) return 0;
		return GiveItemByRenderId(db, (int32_t)ctx->id, ctx, sink);       // :386 zero-extended word
	}
	// [unverified] a negative index passes the range test at :384 and then
	// reads table[negative] at :389, outside the table. Refused here.
	if (di < 0) return 0;

	// :389-391. A zero table entry leaves the record null and the item is
	// still given, as a bare slot. (A record this reader marked bad is also
	// null here; ROTH.C would use its bytes. Retail has none.)
	const DbItem *rec = db.Item(di);
	int32_t quantity = 0;                                                 // :377
	int slot = -1;
	bool stacked = false;

	if (rec != nullptr)
	{
		if (rec->flags & DBI_WEAPON)                                      // :392-397
			quantity = WeaponGiveQuantity(db, *rec);
		if (rec->itemType & DBT_STACKABLE)                                // :398
		{
			quantity = 1;                                                 // :399
			slot = FindSlotHolding(di);                                   // :400
			if (slot >= 0)
			{
				mSlots[slot].quantity++;                                  // :402 no cap, 16-bit wrap
				stacked = true;
			}
		}
	}

	if (!stacked)                                                         // L_8d, :406
	{
		if (rec != nullptr && rec->Category() == DB_CATEGORY_INFO && FindSlotHolding(di) >= 0)
			return di;                                                    // :407-409 -> :461
		slot = FindFreeSlot();                                            // :411 (bumps the count)
		if (slot < 0) return 0;                                           // :412
		mSlots[slot].id = (uint16_t)di;                                   // :413
		mSlots[slot].quantity = (uint16_t)quantity;                       // :414
	}

	// L_c8, :416-434: which hand.
	bool select = true;
	if (rec != nullptr)
	{
		if (rec->flags & DBI_WEAPON)                                      // :418-428
		{
			EquipLeft(slot, SignedId(slot), sink);                        // :421 id = the slot's word, sign-extended
			if (sink) sink->WeaponHudDirty();                             // :427
			select = false;
		}
		else if (rec->Category() == DB_CATEGORY_INFO)                     // :429-430
			select = false;
	}
	if (select)                                                           // :433-434 (also the no-record case)
	{
		mRight = slot;
		if (sink) sink->RightHandChanged(slot);
	}

	// L_fd, :436-443: the ammo display, keyed on the INDEX GIVEN.
	if (sink && sink->IsActiveAmmo(di)) sink->AmmoDisplayChanged(+1);

	// :444-457: the fly-in. The id shown is the context's +4 word, or nothing
	// when ctx+7 & 1; the record's own render id replaces either when it is
	// >= 0x200 as an UNSIGNED 32-bit value (so a negative, sign-extended id
	// also qualifies, :448-449).
	if (ctx != nullptr)
	{
		uint32_t q = ctx->id;                                             // :445 movzx
		if (ctx->flags7 & 1) q = 0;                                       // :446
		if (rec != nullptr)
		{
			const int32_t rid = (int16_t)rec->renderId;                   // :448
			if ((uint32_t)rid >= 0x200u) q = (uint32_t)rid;               // :449
		}
		if (q != 0)                                                       // :451
		{
			BeginPickupLock();                                            // renderer.c:9882
			if (sink) sink->PickupFlyStarted(ctx->x, ctx->y, (int32_t)ctx->z + 0x18, (uint16_t)q);  // :452-455
		}
	}

	if (rec != nullptr && (rec->flags & DBI_WEAPON))                      // :458-459
	{
		if (sink) sink->RebuildWeaponList();
	}
	return di;                                                            // :461
}

//==========================================================================
//
// give_item_by_dbase_id (inventory.c:343-364)
//
// The id is 32-bit and SIGNED here: below 0x200, including anything negative,
// is refused (:348 `jl`). The table is walked from entry 0 upward (:349-354);
// empty entries are skipped; a record matches when its +2 word, SIGN-EXTENDED,
// equals the id (:356-357). Unlike is_item_id_pickable, a matching record that
// is not pickable (+4 & 0x80, or category 2) does NOT end the search (:359-360)
// -- the walk goes on to the next record carrying the same id. The first
// pickable match is given by its 1-based index (:361-362).
//
// Callers pass the object's id word zero-extended (inventory.c:386,
// input.c:1077), so a record whose render id is 0x8000 or above can never be
// matched from there.
//
//==========================================================================

int32_t Inventory::GiveItemByRenderId(const Dbase100 &db, int32_t renderId, const GiveContext *ctx,
	InventorySink *sink)
{
	if (renderId < 0x200) return 0;                                       // :348
	const int n = db.ItemCount();                                         // :352 [base+0x10]
	for (int i = 0; i < n; i++)
	{
		const DbItem *rec = db.Item(i + 1);
		if (rec == nullptr) continue;                                     // :354
		if ((int32_t)(int16_t)rec->renderId != renderId) continue;        // :356-357
		if (rec->flags & DBI_NOT_PICKABLE) continue;                      // :359
		if (rec->Category() == DB_CATEGORY_INFO) continue;                // :360
		return GiveItem(db, (int32_t)(int16_t)(i + 1), ctx, sink);        // :361-362
	}
	return 0;
}

//==========================================================================
//
// query_player_inventory (inventory.c:472-531)
//
// g_item_autoselected_flag is cleared FIRST, on every call (:477). AUDIT: it
// was only ever set.
//
// Select mode (flags&1): the right hand holding the id answers 1 and resets
// the slide timer (:485-488); the left hand answers 1 and does not (:490-492).
// Both compare the raw u16, with no test for an empty slot. AUDIT: the old
// code refused id 0 here. Otherwise, unless the easy flag is set or flags&2
// (:478-481, 493), the first of the first Count() used slots that matches goes
// into the right hand, the flag is set, and the answer is STILL 0 (:499-502).
//
// Count mode: the number of matching slots among the first Count() used ones.
// Then (:518-529), if exactly one matched, its quantity (sign-extended) is
// GREATER THAN 1, and its record is stackable, that quantity is the answer.
// AUDIT: the old code returned the quantity of any single stackable match,
// so a stack whose quantity word was 0 answered 0 instead of 1.
//
//==========================================================================

int Inventory::Query(const Dbase100 &db, uint16_t id, int flags, InventorySink *sink)
{
	mAutoSelected = false;                                                // :477
	bool noScan = mEasy;                                                  // :478
	if (flags & 2) noScan = true;                                         // :481

	if (flags & 1)
	{
		if (mRight >= 0 && mSlots[mRight].id == id)                       // :484-488
		{
			if (sink) sink->HeldItemSlideReset();
			return 1;
		}
		if (mLeft >= 0 && mSlots[mLeft].id == id) return 1;               // :489-492
		if (!noScan)                                                      // :493
		{
			int seen = 0;
			for (int i = 0; i < SLOT_COUNT && (uint32_t)seen < (uint32_t)mCount; i++)
			{
				if (mSlots[i].id == 0) continue;                          // :497
				if (mSlots[i].id == id)                                   // :498
				{
					mRight = i;                                           // :499
					if (sink) sink->RightHandChanged(i);                  // :500
					mAutoSelected = true;                                 // :501
					return 0;                                             // :502
				}
				seen++;
			}
		}
		return 0;                                                         // :518 with ecx == 0
	}

	int matches = 0, last = -1, seen = 0;
	for (int i = 0; i < SLOT_COUNT && (uint32_t)seen < (uint32_t)mCount; i++)                 // :509-514
	{
		if (mSlots[i].id == 0) continue;
		if (mSlots[i].id == id) { matches++; last = i; }
		seen++;
	}
	if (matches == 1 && last >= 0)                                        // :518
	{
		const int32_t qty = (int16_t)mSlots[last].quantity;               // :519
		if (qty > 1)                                                      // :520
		{
			const DbItem *rec = RecordForId(db, SignedId(last));          // :521-523
			if (rec != nullptr && (rec->itemType & DBT_STACKABLE))        // :525
				return qty;                                               // :526
		}
	}
	return matches;
}

//==========================================================================
//
// find_or_autoselect_inventory_item (inventory.c:1394-1452)
//
// Select mode: the right hand's id (sign-extended) in the list -> 1, the slide
// timer is reset and g_last_item_record takes the id (:1407-1414); then the
// left hand -> 1 and the last-item write, no timer (:1415-1420). Otherwise,
// unless easy / flags&2, every used slot is tested against every list entry
// and each hit is collected, up to 24 (:1421-1430) -- a slot is collected once
// per list entry it equals, so a duplicated id in the list weights the draw.
// One is drawn with rng_next_index_for_count, put in the right hand, the flag
// is set, and the answer is 0 (:1431-1437).
//
// Count mode: the number of (slot, list entry) hits; each writes
// g_last_item_record (:1441-1451).
//
//==========================================================================

int Inventory::FindOrAutoselect(const int32_t *ids, int count, int flags, InventoryRng &rng, InventorySink *sink)
{
	mAutoSelected = false;                                                // :1399
	bool noScan = mEasy;                                                  // :1400
	if (flags & 2) noScan = true;                                         // :1402
	if (count < 0 || ids == nullptr) count = 0;

	if (flags & 1)
	{
		if (mRight >= 0)                                                  // :1406
		{
			const int32_t sid = SignedId(mRight);
			for (int p = 0; p < count; p++)
			{
				if (sid == ids[p])
				{
					if (sink) sink->HeldItemSlideReset();                 // :1411
					mLastItem = ids[p];
					return 1;
				}
			}
		}
		if (mLeft >= 0)                                                   // :1415
		{
			const int32_t eid = SignedId(mLeft);
			for (int p = 0; p < count; p++)
				if (eid == ids[p]) { mLastItem = eid; return 1; }         // :1419
		}
		if (!noScan)                                                      // :1421
		{
			int pick[24];
			int n = 0, seen = 0;
			for (int i = 0; i < SLOT_COUNT && (uint32_t)seen < (uint32_t)mCount; i++)
			{
				if (mSlots[i].id == 0) continue;
				const int32_t id = SignedId(i);
				for (int p = 0; p < count; p++)
					if (id == ids[p] && n < 24) pick[n++] = i;            // :1428-1429
				seen++;
			}
			if (n != 0)
			{
				// :1432 -- the low 16 bits of the draw. It is below n for the
				// original generator; an injected one is held to that.
				uint32_t idx = (uint32_t)rng.NextIndexForCount(n) & 0xffffu;
				if (idx >= (uint32_t)n) idx = (uint32_t)n - 1;            // [unverified] the original would read past pick[]
				mRight = pick[idx];                                       // :1433
				if (sink) sink->RightHandChanged(mRight);                 // :1434
				mAutoSelected = true;                                     // :1435
				return 0;
			}
		}
		return 0;                                                         // :1439
	}

	int result = 0, seen = 0;
	for (int i = 0; i < SLOT_COUNT && (uint32_t)seen < (uint32_t)mCount; i++)                 // :1443-1450
	{
		if (mSlots[i].id == 0) continue;
		const int32_t id = SignedId(i);
		for (int p = 0; p < count; p++)
			if (id == ids[p]) { result++; mLastItem = id; }
		seen++;
	}
	return result;
}

//==========================================================================
//
// remove_inventory_item (inventory.c:246-277)
//
// In this order: the left hand unequips (activate_weapon_item(0,0), :250-258);
// the right hand clears and its icon refreshes (:259-262); the active ammo
// loses one on its display (:263-267); then a stackable record with a quantity
// above 1 (signed) loses one and the slot stays (:268-273); anything else has
// its ID WORD cleared and the count drops (:275-276).
//
// So removing one of a held stack still empties the hand -- now confirmed.
// AUDIT: the old code zeroed the quantity word as well. The original leaves it
// (:275 writes one word), and save chunk 7 writes the array raw, so a cleared
// slot keeps a stale quantity on disk.
// AUDIT: the old code ignored an EMPTY slot. The original does not look: it
// clears the (already clear) id and still DECREMENTS THE COUNT. Kept.
//
//==========================================================================

void Inventory::RemoveFromSlot(const Dbase100 &db, int slot, InventorySink *sink)
{
	if (slot < 0 || slot >= SLOT_COUNT) return;
	InventorySlot &s = mSlots[slot];
	// The record is resolved by the callers from the id they are removing
	// (:289-293, :331-334), which is this slot's id.
	const int32_t id = SignedId(slot);
	const DbItem *rec = RecordForId(db, id);

	if (slot == mLeft) EquipLeft(-1, 0, sink);                            // :250-258
	if (slot == mRight)                                                   // :259-262
	{
		mRight = -1;
		if (sink) sink->RightHandChanged(-1);
	}
	if (sink && sink->IsActiveAmmo(id)) sink->AmmoDisplayChanged(-1);     // :263-267

	if (rec != nullptr && (rec->itemType & DBT_STACKABLE))                // :268
	{
		if ((int16_t)s.quantity > 1)                                      // :269-270 signed
		{
			s.quantity--;                                                 // :271
			return;
		}
	}
	s.id = 0;                                                             // :275 the quantity word is left
	mCount--;                                                             // :276
}

//==========================================================================
//
// remove_item (inventory.c:283-320)
//
// An empty pack rebuilds the weapon list and answers 0 (:288). Then the right
// hand, then the left -- each compared as a raw u16, with no test that the id
// is non-zero (:294-303) -- then the first Count() used slots (:304-317). The
// first match is removed and the answer is -1. AUDIT: only the NO-MATCH paths
// rebuild the weapon list (:288, :318); a successful removal does not.
//
//==========================================================================

int Inventory::RemoveItem(const Dbase100 &db, uint16_t id, InventorySink *sink)
{
	if (mCount == 0)                                                      // :288
	{
		if (sink) sink->RebuildWeaponList();
		return 0;
	}
	if (mRight >= 0 && mSlots[mRight].id == id) { RemoveFromSlot(db, mRight, sink); return -1; }  // :294-298
	if (mLeft >= 0 && mSlots[mLeft].id == id) { RemoveFromSlot(db, mLeft, sink); return -1; }     // :299-303
	int remaining = mCount;
	for (int i = 0; i < SLOT_COUNT; i++)                                  // :306-317
	{
		if (mSlots[i].id != 0)
		{
			if (mSlots[i].id == id)
			{
				RemoveFromSlot(db, i, sink);
				return -1;
			}
			remaining--;
		}
		if (remaining == 0) break;
	}
	if (sink) sink->RebuildWeaponList();                                  // :318
	return 0;
}

// consume_held_item (inventory.c:325-338).
int Inventory::ConsumeSlot(const Dbase100 &db, int slot, InventorySink *sink)
{
	if (slot < 0 || slot >= SLOT_COUNT) return 0;                         // :327 null slot
	if (mCount == 0) return 0;                                            // :328
	RemoveFromSlot(db, slot, sink);                                       // :335
	if (sink) sink->RebuildWeaponList();                                  // :336
	return 1;
}

//==========================================================================
//
// set_inventory_list_filter (inventory.c:1255-1288), RAW 0x42
//
// The per-tab cursors are cleared first, in both directions (:1258). Then
// only BIT 0 of the argument is read (:1260). AUDIT: the old code took 0 and 1
// as the two modes and ignored anything else.
//
// Bit 0 set: the hidden bit comes off the first Count() used slots (:1262-1268).
// Bit 0 clear: it goes on every one of them whose record is not category 2
// (:1279-1282), then both hand pointers are zeroed DIRECTLY -- no
// activate_weapon_item(0,0) -- the icon is refreshed and the weapon HUD reset,
// in that order (:1272-1275). AUDIT: the old code reset the HUD first.
// Both directions end by rebuilding the weapon list (:1287). AUDIT: missing.
//
// The record comes from get_dbase100_inventory_entry on the sign-extended id
// (:1278), so an already-hidden slot has none. [unverified]: with no record
// the original reads the category from address 4. Here "no record" is "not
// category 2" and the slot is hidden; for an already-hidden slot the answer
// cannot change anything, so only a slot whose table entry is 0 depends on it.
//
// Trap kept for the weapon system: rebuild_weapon_inventory_list masks bit 15
// away, so hidden weapons stay in the weapon list (weapon_combat.c:501).
//
//==========================================================================

void Inventory::SetFilter(const Dbase100 &db, int mode, InventorySink *sink)
{
	if (sink) sink->ClearTabCursors();                                    // :1258
	int seen = 0;
	if (mode & 1)
	{
		for (int i = 0; i < SLOT_COUNT && (uint32_t)seen < (uint32_t)mCount; i++)             // :1262-1268
		{
			if (mSlots[i].id == 0) continue;
			seen++;
			mSlots[i].id &= (uint16_t)~HIDDEN_BIT;
		}
	}
	else
	{
		for (int i = 0; i < SLOT_COUNT && (uint32_t)seen < (uint32_t)mCount; i++)             // :1270-1284
		{
			if (mSlots[i].id == 0) continue;
			const DbItem *rec = RecordForId(db, SignedId(i));             // :1278
			if (rec == nullptr || rec->Category() != DB_CATEGORY_INFO)    // :1280-1281
				mSlots[i].id |= HIDDEN_BIT;
			seen++;
		}
		mLeft = mRight = -1;                                              // :1272
		if (sink)
		{
			sink->RightHandChanged(-1);                                   // :1273
			sink->ResetWeaponHud();                                       // :1274
		}
	}
	if (sink) sink->RebuildWeaponList();                                  // :1287
}

//==========================================================================
//
// swap_inventory_entries (inventory.c:644-680)
//
// `a` is the entry that was clicked, `b` the entry held on the cursor
// (dialogue_ui.c:1761). The two slots exchange their id and quantity words
// (:667-674), and the trackers move like this:
//
//   left hand  on A -> now B's slot        (:650-653)   } only A is tested:
//   right hand on A -> now B's slot        (:654-657)   } a hand on B stays put
//   Adam       on B -> A's slot, else on A -> B's slot  (:658-664)
//
// So a hand that held the clicked item follows it; a hand on the HELD entry
// would be left pointing at the slot, which now holds the other item. AUDIT:
// the old code moved the hands in both directions. (Picking an entry onto the
// cursor empties any hand on it, so the one-sided case is the normal one.)
// The weapon list is rebuilt last (:679).
//
// ROTH.C tests the cursor ENTRY pointers (g_left_hand_item and friends), which
// build_inventory_entry_list set for the open tab; comparing slots is the same
// thing while that list is current.
//
//==========================================================================

void Inventory::SwapSlots(int a, int b, InventorySink *sink)
{
	if (a < 0 || b < 0 || a >= SLOT_COUNT || b >= SLOT_COUNT) return;
	if (mLeft == a) mLeft = b;                                            // :650-653
	if (mRight == a) mRight = b;                                          // :654-657
	if (mAdamSlot == b) mAdamSlot = a;                                    // :658-660
	else if (mAdamSlot == a) mAdamSlot = b;                               // :661-664
	const InventorySlot t = mSlots[a];                                    // :667-674
	mSlots[a] = mSlots[b];
	mSlots[b] = t;
	if (sink) sink->RebuildWeaponList();                                  // :679
}

//==========================================================================
//
// The cursor-held item: the verbs of dispatch_dialogue_ui_action
// (dialogue_ui.c:1683-1795) and inventory.c's commit / Enter.
//
//==========================================================================

// dialogue_ui.c:1768-1788. Taking a cell that is in a hand empties the hand:
// the left through activate_weapon_item(0,0) (:1776-1778); the right by
// zeroing the pointer WITHOUT refreshing the icon (:1781-1785).
void Inventory::PickUpToCursor(int slot, InventorySink *sink)
{
	if (slot < 0 || slot >= SLOT_COUNT) return;
	if (slot == mLeft) EquipLeft(-1, 0, sink);
	if (slot == mRight) mRight = -1;
	mHeld = slot;                                                         // :1787
}

// dialogue_ui.c:1694-1702: the held item goes to the LEFT hand through
// activate_weapon_item(slot, id) -- whatever it is; nothing tests for a weapon.
// With nothing held this is the "jump to the hand's tab" branch, presentation.
void Inventory::HeldToLeftHand(InventorySink *sink)
{
	const int held = mHeld;
	mHeld = -1;                                                           // :1693
	if (held < 0) return;
	EquipLeft(held, SignedId(held), sink);                                // :1695
}

// dialogue_ui.c:1722-1730: the right hand takes the slot; the icon is NOT
// refreshed here (the box is redrawn instead).
void Inventory::HeldToRightHand(InventorySink *sink)
{
	(void)sink;
	const int held = mHeld;
	mHeld = -1;                                                           // :1721
	if (held < 0) return;
	mRight = held;                                                        // :1723
}

// commit_held_cursor_item (inventory.c:2178-2196). List 2 (characters) does
// nothing; list 1 (weapons) equips; every other list selects.
void Inventory::CommitHeld(int activeList, InventorySink *sink)
{
	if (mHeld < 0) return;                                                // :2181
	if (activeList == 2) return;                                          // :2182
	if (activeList == 1)                                                  // :2183-2192
	{
		EquipLeft(mHeld, SignedId(mHeld), sink);
		return;
	}
	mRight = mHeld;                                                       // :2194
	if (sink) sink->RightHandChanged(mRight);                             // :2195
}

// Enter on the grid cursor (inventory.c:1710-1720). `slot` is the entry under
// the cursor, or -1 when the cursor is past the end of the list. The caller
// then does what the key goes on to do (:1722 -> L_sel, :1741-1745): unless a
// voice was interrupted, raise the close request and call CommitHeld.
void Inventory::EnterOnSlot(int activeList, int slot, InventorySink *sink)
{
	if (activeList == 2) return;                                          // :1711
	if (slot < 0 || slot >= SLOT_COUNT) return;                           // :1714, 1716
	if (activeList == 1)
	{
		EquipLeft(slot, SignedId(slot), sink);                            // :1715
		return;
	}
	mRight = slot;                                                        // :1717
	if (sink) sink->RightHandChanged(slot);                               // :1718
}

//==========================================================================
//
// resolve_item_use_action (inventory.c:551-594)
//
// Only a record with +5 & 0x40 has recipes (:557). Each OnUse (trigger 0x0B)
// block is one recipe. Its words: 0x34 = the target this recipe is for (the
// last one wins, default 0); 0xB4 = a result item, the newest becoming
// "second" and the one before it "first" (:580); 0x35 without the If-NOT bit =
// the effect, an action record id (:583). The effect output is zeroed at the
// start (:555) and again at each OnUse block (:571).
//
// A block whose target differs is skipped (:586). On a match: with a result,
// the answer is (first << 16) | second (:590) -- second is the full 24-bit
// operand, not masked; with no result but an effect, -1 (:591); with neither,
// the scan goes on (:592). The end of the list answers 0 (:562) and LEAVES THE
// EFFECT of the last OnUse block scanned in the output, matched or not; no
// caller reads it in that case.
//
//==========================================================================

UseAction ResolveItemUseAction(const Dbase100 &db, const DbItem *rec, int32_t target)
{
	UseAction out;                                                        // :555
	if (rec == nullptr) return out;                                       // :556
	if (!(rec->itemType & DBT_HAS_ON_USE)) return out;                    // :557
	for (const DbTriggerBlock &b : rec->blocks)
	{
		if (b.trigger != DBTRIG_ON_USE) continue;                         // :563
		uint32_t second = 0, first = 0;
		int32_t matchTarget = 0;
		out.effect = 0;                                                   // :571
		for (int i = 0; i < b.commandCount; i++)
		{
			const uint32_t w = db.BlockCommand(b, i).raw;
			const uint32_t hi = w >> 24;
			const uint32_t val = w & 0xffffffu;
			if ((hi & 0x7f) == 0x34)
			{
				if (hi & 0x80) { first = second; second = val; }          // :580
				else matchTarget = (int32_t)val;                          // :581
			}
			else if ((hi & 0x7f) == 0x35)
			{
				if (!(hi & 0x80)) out.effect = val;                       // :583
			}
		}
		if (matchTarget != target) continue;                              // :586
		if (second != 0) { out.packed = (first << 16) | second; return out; }   // :590
		if (out.effect != 0) { out.packed = 0xffffffffu; return out; }    // :591
	}
	out.packed = 0;                                                       // :562
	return out;
}

//==========================================================================
//
// use_item_on_self (inventory.c:691-776)
//
//   1. recipe = resolve(held record, target 0)                        (:714)
//   2. none: message 0x2a in the refusal colour, and nothing else     (:716-733)
//   3. otherwise, unless effect-only (-1):
//        consume the held slot, give the LOW 16 bits ("second"),
//        then give the HIGH 16 bits ("first") if non-zero             (:744-750)
//      each give as (int32)(int16), with no context
//   4. message 0x2d in the success colour -- AFTER the gives          (:751-758)
//   5. the cursor lets go                                             (:759)
//   6. the effect action runs, if any                                 (:760-773)
//   7. the panel refreshes unless it has been asked to close          (:774-775)
//
// An effect-only item is NOT consumed here; its effect must remove it.
//
// [unverified] nothing held, or a held slot with no record: the original
// dereferences null for the name and the +5 flag. Here: nothing held does
// nothing; no record takes the refusal path with name 0.
//
//==========================================================================

void Inventory::UseHeldOnSelf(const Dbase100 &db, InventorySink *sink)
{
	if (mHeld < 0) return;
	const int32_t itemId = SignedId(mHeld);                               // :701
	const DbItem *rec = RecordForId(db, itemId);                          // :703
	const uint32_t name = rec ? rec->name : 0;                            // :704
	const UseAction act = ResolveItemUseAction(db, rec, 0);               // :714

	if (act.packed == 0)                                                  // :716
	{
		if (sink) sink->ItemMessage(0x2a, 0, name, 0);
		return;
	}
	if (act.packed != 0xffffffffu)                                        // :744
	{
		ConsumeSlot(db, mHeld, sink);                                     // :745
		GiveItem(db, (int32_t)(int16_t)(uint16_t)act.packed, nullptr, sink);          // :746
		const uint32_t first = act.packed >> 16;
		if (first != 0)
			GiveItem(db, (int32_t)(int16_t)(uint16_t)first, nullptr, sink);           // :749
	}
	if (sink) sink->ItemMessage(0x2d, 5, name, 0);                        // :751-753
	mHeld = -1;                                                           // :759
	if (act.effect != 0 && sink) sink->RunActionRecord(act.effect);       // :760-773
	if (sink && !sink->PanelCloseRequested()) sink->RefreshPanel();       // :774-775
}

//==========================================================================
//
// combine_held_item_with_target (inventory.c:787-928)
//
// The recipe is looked for on the HELD item first, with the target's id, if
// the held record has +5 & 0x40 (:806-807); only if that finds nothing, and
// the target has +5 & 0x40, on the target with the held id (:808-809).
//
// No recipe: message 0x29 (held name, target name), refusal colour, and
// nothing changes -- the item stays on the cursor (:826-843).
//
// A recipe: message 0x28 FIRST (:846-860), then, unless effect-only:
//
//   two results (high word non-zero), second = low, first = high (:865-887)
//     second == held id   -> consume TARGET, give first            (cursor kept)
//     first  == held id   -> consume TARGET, give second           (cursor kept)
//     second == target id -> consume HELD,   give first
//     first  == target id -> consume HELD,   give second
//     neither             -> consume HELD, consume TARGET, give second, give first
//   one result (:888-892) -> consume HELD, consume TARGET, give it
//
// tested in that order, the ids compared as 32-bit values against the
// sign-extended slot ids. Then the cursor lets go, except on the two "cursor
// kept" paths (:895) -- and then the panel refresh, which runs on every path
// (:898-912), rebuilds the entry list, and THAT clears the cursor
// (inventory.c:1317). The loop meant to find the held entry again is dead
// (:904-909: it leaves at once), so "cursor kept" does not survive: after any
// recipe nothing is held. The effect action runs last (:914-927) -- after the
// refresh, where use-on-self runs it before.
//
// [unverified] nothing held, or either slot without a record: null
// dereferences in the original (:798-819). Here: nothing held does nothing; a
// missing record counts as "no +5 & 0x40" with name 0.
//
//==========================================================================

void Inventory::CombineHeldWith(const Dbase100 &db, int targetSlot, InventorySink *sink)
{
	if (mHeld < 0 || targetSlot < 0 || targetSlot >= SLOT_COUNT) return;
	const int heldSlot = mHeld;
	const int32_t heldId = SignedId(heldSlot);                            // :798
	const int32_t targetId = SignedId(targetSlot);                        // :800
	const DbItem *heldRec = RecordForId(db, heldId);                      // :802
	const DbItem *targetRec = RecordForId(db, targetId);                  // :803

	UseAction act;
	if (heldRec != nullptr && (heldRec->itemType & DBT_HAS_ON_USE))       // :806-807
		act = ResolveItemUseAction(db, heldRec, targetId);
	if (act.packed == 0 && targetRec != nullptr && (targetRec->itemType & DBT_HAS_ON_USE))   // :808-809
		act = ResolveItemUseAction(db, targetRec, heldId);

	const uint32_t heldName = heldRec ? heldRec->name : 0;                // :812
	const uint32_t targetName = targetRec ? targetRec->name : 0;          // :819

	if (act.packed == 0)                                                  // :826
	{
		if (sink) sink->ItemMessage(0x29, 0, heldName, targetName);
		return;
	}
	if (sink) sink->ItemMessage(0x28, 5, heldName, targetName);           // :846-860

	bool cleared = true;                                                  // :862
	if (act.packed != 0xffffffffu)                                        // :864
	{
		const uint32_t second = act.packed & 0xffffu;
		const uint32_t first = act.packed >> 16;
		const int32_t giveSecond = (int32_t)(int16_t)(uint16_t)second;
		const int32_t giveFirst = (int32_t)(int16_t)(uint16_t)first;
		if (act.packed & 0xffff0000u)                                     // :865
		{
			if ((int32_t)second == heldId)                                // :868-871
			{
				ConsumeSlot(db, targetSlot, sink);
				GiveItem(db, giveFirst, nullptr, sink);
				cleared = false;
			}
			else if ((int32_t)first == heldId)                            // :872-875
			{
				ConsumeSlot(db, targetSlot, sink);
				GiveItem(db, giveSecond, nullptr, sink);
				cleared = false;
			}
			else if ((int32_t)second == targetId)                         // :876-878
			{
				ConsumeSlot(db, heldSlot, sink);
				GiveItem(db, giveFirst, nullptr, sink);
			}
			else if ((int32_t)first == targetId)                          // :879-881
			{
				ConsumeSlot(db, heldSlot, sink);
				GiveItem(db, giveSecond, nullptr, sink);
			}
			else                                                          // :882-886
			{
				ConsumeSlot(db, heldSlot, sink);
				ConsumeSlot(db, targetSlot, sink);
				GiveItem(db, giveSecond, nullptr, sink);
				GiveItem(db, giveFirst, nullptr, sink);
			}
		}
		else                                                              // :888-891
		{
			ConsumeSlot(db, heldSlot, sink);
			ConsumeSlot(db, targetSlot, sink);
			GiveItem(db, giveSecond, nullptr, sink);
		}
	}
	if (cleared) mHeld = -1;                                              // :895
	if (sink) sink->RefreshPanel();                                       // :900 / :911
	mHeld = -1;                                                           // inventory.c:1317 via :1638-1641; the re-find at :904-909 is dead
	if (act.effect != 0 && sink) sink->RunActionRecord(act.effect);       // :914-927
}

//==========================================================================
//
// restore_active_held_item (inventory.c:2204-2244)
//
// Finds the first carried record with +5 & 0x10 -- Adam -- and latches its
// slot (0x8104c) and id (0x81054); then, if no weapon is equipped, calls
// activate_weapon_item(0, 0) (:2231-2239), which reads that latched id to arm
// the fists (weapon_combat.c:430). With an empty pack, or no such record,
// NOTHING is written: an old latch stands.
//
// The walk's counter only drops for a slot that HAS a record and is not Adam
// (:2215); an empty slot, or a used slot with no record, does not count.
//
//==========================================================================

void Inventory::RestoreActiveHeldItem(const Dbase100 &db, InventorySink *sink)
{
	int remaining = mCount;                                               // :2207
	if (remaining == 0) return;                                           // :2209
	for (int i = 0; i < SLOT_COUNT; i++)
	{
		const int32_t id = SignedId(i);                                   // :2211
		if (id != 0)
		{
			const DbItem *rec = RecordForId(db, id);                      // :2213-2214
			if (rec != nullptr)
			{
				if (!(rec->itemType & DBT_ADAM)) remaining--;             // :2216
				else
				{
					mAdamId = id;                                         // :2227
					mAdamSlot = i;                                        // :2228
					if (mLeft < 0) EquipLeft(-1, 0, sink);                // :2229-2238
					return;
				}
			}
		}
		if (remaining == 0) return;                                       // :2243
	}
}

//==========================================================================
//
// build_inventory_entry_list (inventory.c:1314-1368), the list part
//
// The held cursor is dropped first (:1317). The first Count() used slots are
// walked; a slot whose sign-extended id is not positive (hidden) is passed
// over but still counted (:1326-1328, 1350). A slot is listed when it has a
// record whose category equals `category` (:1333); a category-2 record must
// also have an available choice (:1335-1336). While listing, the hand entries
// are found: for category 2 only the Adam slot (:1337); for any other
// category the left hand, else the right (:1339-1340). After the grid entries
// come the two hand boxes: the left hand's slot, then the right's, each only
// if its id is positive (:1356-1368).
//
// Left to presentation: loading each entry's icon (:1370-1385).
//
//==========================================================================

InventoryEntryList Inventory::BuildEntryList(const Dbase100 &db, int category, InventorySink *sink)
{
	InventoryEntryList list;
	mHeld = -1;                                                           // :1317
	int remaining = mCount;                                               // :1323
	for (int i = 0; i < SLOT_COUNT && remaining > 0; i++)                 // :1325
	{
		const int32_t id = SignedId(i);
		if (id == 0) continue;                                            // :1327
		remaining--;                                                      // :1350
		if (id < 0) continue;                                             // :1328
		const DbItem *rec = db.Item(id);                                  // :1329-1331
		if (rec == nullptr) continue;
		const int cat = rec->Category();
		if (cat != category) continue;                                    // :1333
		const int entry = (int)list.slots.size();
		if (cat == DB_CATEGORY_INFO)
		{
			if (!ItemHasAvailableChoice(db, id, sink)) continue;          // :1336
			if (i == mAdamSlot) list.adamEntry = entry;                   // :1337
		}
		else
		{
			if (i == mLeft) list.leftEntry = entry;                       // :1339
			else if (i == mRight) list.rightEntry = entry;                // :1340
		}
		list.slots.push_back(i);                                          // :1342-1346
	}
	if (mLeft >= 0 && SignedId(mLeft) > 0) list.shownLeft = mLeft;        // :1355-1361
	if (mRight >= 0 && SignedId(mRight) > 0) list.shownRight = mRight;    // :1362-1368
	return list;
}

//==========================================================================
//
// The pickup lock's timer (inventory.c:106-139). Unarmed, nothing moves, not
// even the timer (:108). Armed, the timer takes the frame step (:119); below
// 0x1c the fly-in is interpolated (presentation, :121-133); at 0x1c or more
// the timer and the flags are zeroed (:134-137).
//
// begin_item_pickup_lock does not touch the timer and reset_item_pickup_lock
// clears only the flags (inventory.c:34), so a lock re-armed after a reset
// carries on from the old timer value.
//
//==========================================================================

void Inventory::TickPickupLock(int32_t step)
{
	if (!(mPickupFlags & 1)) return;                                      // :108
	mPickupTimer += step;                                                 // :119
	if (mPickupTimer >= PICKUP_LOCK_TICKS)                                // :121
	{
		mPickupTimer = 0;                                                 // :135
		mPickupFlags = 0;                                                 // :136
	}
}

//==========================================================================
//
// Save chunks 7 and 2 (savegame.c:1592-1593 write, 1410-1417 read; 152-157,
// 187-192 for the hands).
//
//==========================================================================

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

// The count is every non-zero id word in all 256 slots (savegame.c:1412-1415),
// then the weapon list is rebuilt (:1416). The hands are chunk 2 (LoadHands).
bool Inventory::Deserialize(const uint8_t *data, size_t size, InventorySink *sink)
{
	if (data == nullptr || size != (size_t)SAVE_BYTES) return false;
	mCount = 0;
	for (int i = 0; i < SLOT_COUNT; i++)
	{
		const uint8_t *p = data + i * 4;
		mSlots[i].id = (uint16_t)(p[0] | (p[1] << 8));
		mSlots[i].quantity = (uint16_t)(p[2] | (p[3] << 8));
		if (mSlots[i].id != 0) mCount++;
	}
	if (sink) sink->RebuildWeaponList();
	return true;
}

void Inventory::SaveHands(uint32_t &leftOut, uint32_t &rightOut) const
{
	// A null pointer saves as 0; anything else as pointer - base
	// (savegame.c:152-157) -- which is ALSO 0 for slot 0.
	leftOut = mLeft > 0 ? (uint32_t)mLeft * 4u : 0u;
	rightOut = mRight > 0 ? (uint32_t)mRight * 4u : 0u;
}

void Inventory::LoadHands(uint32_t left, uint32_t right)
{
	// 0 is "none"; anything else is base + offset (savegame.c:187-192).
	// [unverified] an offset that is not a slot boundary inside the array:
	// the original takes it as a pointer regardless. Read as "none" here.
	auto slotOf = [](uint32_t v) -> int
	{
		if (v == 0 || (v & 3) != 0 || v / 4 >= (uint32_t)SLOT_COUNT) return -1;
		return (int)(v / 4);
	};
	mLeft = slotOf(left);
	mRight = slotOf(right);
}

//==========================================================================
//
// is_item_id_pickable (inventory.c:44-65)
//
// Below 0x200 as an UNSIGNED 32-bit value: never (:48). Then the table is
// walked from entry 0; the FIRST record whose sign-extended +2 word equals the
// id decides, and the walk ends there either way (:56-63): +4 & 0x80 or
// category 2 answer 0, anything else 1. (give_item_by_dbase_id walks on past
// an unpickable match; this does not.)
//
// AUDIT: the id was uint16_t and matched as an unsigned word, so 0x8xxx
// matched a record carrying 0x8xxx. In ROTH.C the record side is sign-extended
// and the id is not, so such an id matches nothing unless the caller passes it
// sign-extended.
//
//==========================================================================

bool IsItemIdPickable(const Dbase100 &db, int32_t renderId)
{
	if ((uint32_t)renderId < 0x200u) return false;                        // :48
	const int n = db.ItemCount();
	for (int i = 0; i < n; i++)                                           // :50-52
	{
		const DbItem *rec = db.Item(i + 1);
		if (rec == nullptr) continue;                                     // :54
		if ((int32_t)(int16_t)rec->renderId != renderId) continue;        // :56-57
		if (rec->flags & DBI_NOT_PICKABLE) return false;                  // :59
		if (rec->Category() == DB_CATEGORY_INFO) return false;            // :60-62
		return true;                                                      // :63
	}
	return false;
}

int32_t WeaponGiveQuantity(const Dbase100 &db, const DbItem &rec)
{
	const GiveAttrs a = ParseGiveAttrs(db, rec);
	uint32_t v = (uint32_t)a.a4;                                          // inventory.c:394
	if (a.a30 & 0x8000u) v <<= 8;                                         // inventory.c:395 (attrbuf[0x31] & 0x80)
	return (int32_t)v;                                                    // inventory.c:396
}

bool WeaponQuantityIsShifted(const Dbase100 &db, const DbItem &rec)
{
	return (ParseGiveAttrs(db, rec).a30 & 0x8000u) != 0;
}

//==========================================================================
//
// node_has_available_choice (dialogue_ui.c:74-97): +5 & 0x10 -> yes at once;
// +4 & 0x08 clear -> no; otherwise yes as soon as one OnInformation (1) or
// OnInspect (4) block evaluates non-zero in scan mode 2.
//
//==========================================================================

bool ItemHasAvailableChoice(const Dbase100 &db, int32_t itemId, InventorySink *sink)
{
	const DbItem *rec = RecordForId(db, itemId);
	if (rec == nullptr) return false;
	if (rec->itemType & DBT_ADAM) return true;                            // dialogue_ui.c:76
	if (!(rec->flags & DBI_HAS_INFORMATION)) return false;                // dialogue_ui.c:78
	for (const DbTriggerBlock &b : rec->blocks)
	{
		if (b.trigger != DBTRIG_INFORMATION && b.trigger != DBTRIG_INSPECT) continue;   // dialogue_ui.c:85
		if (sink && sink->EvalItemBlock(itemId, b, 2) != 0) return true;                // dialogue_ui.c:87-89
	}
	return false;
}

// scan_tag4_chunk (renderer.c:1485-1502).
const DbTriggerBlock *FindInspectBlock(const DbItem &rec)
{
	if (!(rec.flags & DBI_HAS_INSPECT)) return nullptr;                   // renderer.c:1487
	return rec.FindBlock(DBTRIG_INSPECT);                                 // renderer.c:1492-1495
}

// find_oninspect_block_by_id (dialogue_ui.c:518-539). The id is signed and
// must be 0x200 or more (:525); the FIRST record carrying it decides, with or
// without an OnInspect block (:535-536).
int32_t FindInspectByRenderId(const Dbase100 &db, int32_t renderId, const DbTriggerBlock **blockOut)
{
	if (blockOut) *blockOut = nullptr;
	if (renderId < 0x200) return 0;
	const int n = db.ItemCount();
	for (int i = 0; i < n; i++)
	{
		const DbItem *rec = db.Item(i + 1);
		if (rec == nullptr) continue;
		if ((int32_t)(int16_t)rec->renderId != renderId) continue;
		const DbTriggerBlock *b = FindInspectBlock(*rec);
		if (b == nullptr) return 0;
		if (blockOut) *blockOut = b;
		return i + 1;
	}
	return 0;
}

// file_config.c:962-967. The bit is set BEFORE the block runs, so a comment
// that re-enters the examine sees it already set.
bool ExamineFirstView(Dbase100 &db, int32_t itemId, InventorySink *sink)
{
	const DbItem *rec = RecordForId(db, itemId);
	if (rec == nullptr) return false;
	const uint8_t runtime = db.ItemRuntime(itemId);
	if (runtime & DBI_RUNTIME_EXAMINED) return false;                     // file_config.c:962
	db.SetItemRuntime(itemId, (uint8_t)(runtime - 0x80));                 // file_config.c:963 (`- 0x80` with the bit clear sets it)
	const DbTriggerBlock *b = FindInspectBlock(*rec);                     // file_config.c:964
	if (b != nullptr && sink) sink->EvalItemBlock(itemId, *b, 0);         // file_config.c:966
	return true;
}

// dialogue_ui.c:1569-1573.
void ReplayInspect(const Dbase100 &db, int32_t itemId, InventorySink *sink)
{
	const DbItem *rec = RecordForId(db, itemId);
	if (rec == nullptr) return;
	const DbTriggerBlock *b = FindInspectBlock(*rec);
	if (b != nullptr && sink) sink->EvalItemBlock(itemId, *b, 0);
}

//==========================================================================
//
// The label. resolve_record_conditional_op2 (inventory.c:73-97): only a record
// with +4 & 0x04 (:76); each ChangeName (trigger 2) block in turn is evaluated
// with mode 0 (:90), and the first that answers non-zero gives the low 24 bits
// of the dword after its header -- its first command's operand (:83, :93).
//
// [unverified] item ids with no record: format_inventory_item_label reads
// through the null table entry (inventory.c:1035-1037). 0 here.
//
//==========================================================================

uint32_t ItemLabelText(const Dbase100 &db, int32_t itemId, InventorySink *sink)
{
	if (itemId == 0) return 0;                                            // inventory.c:1034
	const DbItem *rec = RecordForId(db, itemId);
	if (rec == nullptr) return 0;
	if (rec->flags & DBI_HAS_CHANGE_NAME)                                 // inventory.c:76
	{
		for (const DbTriggerBlock &b : rec->blocks)
		{
			if (b.trigger != DBTRIG_CHANGE_NAME) continue;                // inventory.c:82
			const uint32_t word = db.U32At(b.offset + 4);                 // inventory.c:83
			if (sink && sink->EvalItemBlock(itemId, b, 0) != 0)           // inventory.c:90-92
			{
				const uint32_t r = word & 0xffffffu;                      // inventory.c:93
				if (r != 0) return r;                                     // inventory.c:1039
				break;                                                    // a zero override falls back to the name
			}
		}
	}
	return rec->name;                                                     // inventory.c:1037
}

int32_t ItemLabelQuantity(const Dbase100 &db, const InventorySlot &slot)
{
	int32_t qty = (int16_t)slot.quantity;                                 // inventory.c:1581
	const DbItem *rec = RecordForId(db, (int16_t)slot.id);                // inventory.c:1577-1578
	if (rec != nullptr && WeaponQuantityIsShifted(db, *rec))
		qty = (int32_t)((uint32_t)qty >> 8);                              // inventory.c:1582 a LOGICAL shift of the sign-extended value
	return qty;
}

//==========================================================================
//
// Tabs. 0x7123c holds {0, 1, 3, 4, 2}: the category shown by each of the five
// tabs, in tab order.
//
//==========================================================================

static const uint8_t kTabCategory[5] = { 0, 1, 3, 4, 2 };

int TabCategory(int tab)
{
	if (tab < 0 || tab >= 5) return 0;
	return kTabCategory[tab];
}

// get_item_tab_index (inventory.c:2153-2161). [unverified] an id with no
// record: the original reads the category through a null pointer. Tab 0 here.
int ItemTabIndex(const Dbase100 &db, int32_t itemId)
{
	if (itemId == 0) return 0;                                            // :2155
	const DbItem *rec = RecordForId(db, (int16_t)(uint16_t)itemId);       // :2156
	if (rec == nullptr) return 0;
	const int cat = rec->Category();                                      // :2157
	for (int t = 0; t < 5; t++)
		if (kTabCategory[t] == cat) return t;                             // :2158-2159
	return 0;                                                             // :2160
}

//==========================================================================
//
// NOT PORTED, AND WHY
//
// The pickup verbs (activate_targeted_object, classify_cursor_target_object,
// examine_object_under_cursor: input.c:979-1101, 1199-1221). They gate on the
// world object (distance, def+9 flags, its RAW command triggers) and belong
// with the object/command layer. What they need from here is in place:
// PickupLockArmed (input.c:1055), GiveItemByRenderId (input.c:1077) and
// FindInspectByRenderId (input.c:995-1012).
//
// The RAW handlers cmd_if_not_item / cmd_give_item / cmd_remove_item /
// cmd_set_inventory_filter (raw_commands.c:4760-4947) and DBASE100 ops
// 0x02/0x03/0x11/0x91 (dbase100.c:767-832). They are the callers of Query,
// FindOrAutoselect, GiveItem, RemoveItem and SetFilter and own the
// g_last_item_record writes and the chain-interrupt results.
//
// init_inventory_item_object (inventory.c:147-193): builds the 12-byte
// projectile descriptor from an AsBullet block. Weapon system.
//
// The inventory screen's key and click mapping (update_inventory_screen,
// inventory.c:1692-1845; dispatch_dialogue_ui_action, dialogue_ui.c:1556-1808),
// the 10-cell / 5-row cursor and scroll arithmetic (inventory.c:1751-1791,
// renderer.c:10988-11001), and everything that draws. The VERBS those reach
// are here.
//
// The close-up modal itself (file_config.c:900-1144): DBASE300 / GDV.
//
// WHAT PRESENTATION NEEDS TO QUERY
//   the grid for a tab ........... BuildEntryList(db, TabCategory(tab), sink)
//   an entry's icon .............. db.Item(id)->icon  (DBASE200 offset / 8)
//   an entry's label ............. ItemLabelText + ItemLabelQuantity
//   the hand boxes ............... InventoryEntryList::shownLeft / shownRight
//   the cursor-held item ......... Held()
//   the tab an item lives on ..... ItemTabIndex
//   the close-up to open ......... db.Item(id)->closeup, flags & DBI_CLOSEUP_HOLD
//   magnifier state .............. flags & DBI_HAS_INSPECT; db.ItemRuntime(id) & 0x80
//   "i" button ................... ItemHasAvailableChoice
//   pickups blocked .............. PickupLockArmed()
//
//==========================================================================

} // namespace roth
