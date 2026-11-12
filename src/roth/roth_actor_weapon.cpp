//
// Realms of the Haunting's weapon records: the WeaponAction blocks of a
// DBASE100 item, read into the table the original's fire code works from
// (apply_weapon_action_attributes, weapon_combat.c:338-424). Engine-free;
// proven against the running original by tools/actors_test (op weaponcmp).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_actors.h"
#include "roth_dbase100.h"

#include <string.h>

namespace roth
{

namespace
{
inline uint32_t Rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
}

int ParseWeaponActions(const Dbase100 &db, int itemIndex, WeaponAction *table, int maxSlots)
{
	if (table == nullptr || maxSlots <= 0) return 0;
	WeaponAction *slot = table;
	memset(slot->b, 0, sizeof(slot->b));                     // WC:350: the first slot only
	slot->SetD(0x0c, 1);
	if (itemIndex <= 0 || !db.IsLoaded()) return 0;          // WC:352
	slot->SetD(0x38, itemIndex);
	const DbItem *it = db.ItemSlot(itemIndex);               // the inventory table (WC:354-356)
	const uint8_t *img = db.Image();
	const size_t size = db.Size();
	if (it == nullptr || it->offset == 0 || (size_t)it->offset + 0x18 > size) return 0;
	const uint8_t *rec = img + it->offset;
	if (!(rec[4] & 0x10)) return 0;                          // WC:357
	const uint8_t *blk = rec + 0x14, *end = img + size;
	int count = 0;
	for (;;)
	{
		if (blk + 4 > end) return count;
		const uint32_t header = Rd32(blk);
		const uint32_t bytes = header & 0xffffffu;
		if (bytes == 0) return count;                        // WC:362
		if ((header >> 24) != 5) { blk += (bytes >> 2) << 2; continue; }   // WC:363-366
		const uint8_t *sub = blk + 4;
		uint32_t latch = 8, accA = 0, accB = 0;              // esi / ebx in the original
		const uint32_t n = (bytes >> 2) - 1;
		for (uint32_t k = 0; k < n && sub + 4 <= end; k++, sub += 4)
		{
			const uint32_t dw = Rd32(sub);
			const uint32_t hi = dw >> 24, op = hi & 0x7f, v = dw & 0xffffffu;
			const int32_t sv = (hi & 0x80) ? -(int32_t)v : (int32_t)v;
			switch (op)                                      // WC:378-418
			{
			case 0x12: slot->SetD(0x00, (int32_t)v); slot->SetD(0x20, (int32_t)accA); slot->SetD(0x1c, (int32_t)accB);
			           slot->SetD(0x3c, (int32_t)latch); accA = accB = latch = 0; break;
			case 0x13: slot->SetD(0x08, (int32_t)v); slot->SetD(0x18, (int32_t)accA); slot->SetD(0x14, (int32_t)accB);
			           accA = accB = latch = 0; break;
			case 0x14: slot->SetD(0x04, sv); break;
			case 0x18: slot->SetD(0x0c, (int32_t)v); break;
			case 0x19: slot->SetD(0x10, (int32_t)v); break;
			case 0x1e: accB = (uint32_t)sv; break;
			case 0x1f: slot->SetD(0x2c, (int32_t)v); slot->SetD(0x28, (int32_t)accA); slot->SetD(0x24, (int32_t)accB);
			           slot->SetD(0x40, (int32_t)latch); accA = accB = latch = 0; break;
			case 0x20: slot->SetD(0x30, (int32_t)v); break;
			case 0x21: slot->SetD(0x34, (int32_t)v); break;
			case 0x22: accA = (uint32_t)sv; break;
			case 0x2c: latch = v; break;
			case 0x2d: if (v == 5) slot->b[0x4c] |= 1; else if (v == 6) slot->b[0x4c] |= 2; break;
			case 0x30: slot->SetD(0x44, (int32_t)v); break;
			case 0x33: slot->SetD(0x48, (int32_t)v); break;
			default: break;
			}
		}
		count++;                                             // WC:420-423
		slot++;
		if (count >= maxSlots) return count;
		blk += (bytes >> 2) << 2;
	}
}

int16_t ActorWorld::EquipWeapon(int itemIndex)
{
	PlayerWeapon &w = weapon;
	if (mDb != nullptr) w.slots = ParseWeaponActions(*mDb, itemIndex, w.actions, 4);
	else
	{
		memset(w.actions[0].b, 0, sizeof(w.actions[0].b));
		w.actions[0].SetD(0x0c, 1);
		w.slots = 0;
	}
	w.item = itemIndex > 0 ? itemIndex : 0;
	int16_t ammoItem = 0;
	if (w.actions[0].b[0x31] & 0x80) { w.ammo = 0; w.ammoCap = 0x100; }      // WC:443-445
	else
	{
		w.ammoCap = 1;
		ammoItem = (int16_t)(w.actions[0].b[0x30] | (w.actions[0].b[0x31] << 8));
		w.ammo = 0;                                          // until the caller's SetWeaponAmmo (WC:448-455)
	}
	w.equips++;                                              // WC:458
	return ammoItem;
}

} // namespace roth
