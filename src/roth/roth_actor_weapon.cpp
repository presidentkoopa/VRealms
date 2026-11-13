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
#include <stdio.h>

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

int16_t ActorWorld::EquipWeapon(int itemIndex, int heldSlot)
{
	PlayerWeapon &w = weapon;
	const bool held = heldSlot >= 0;
	w.active = 0;                                            // WC:431: g_active_weapon_ptr = &attrs
	w.held = held;                                           // WC:432
	w.heldSlot = heldSlot;
	if (mDb != nullptr) w.slots = ParseWeaponActions(*mDb, itemIndex, w.actions, 4);   // WC:433-434
	else
	{
		memset(w.actions[0].b, 0, sizeof(w.actions[0].b));
		w.actions[0].SetD(0x0c, 1);
		w.slots = 0;
	}
	// WC:441-442: the held slot's quantity word (0 with no slot: the original
	// reads DS:2, which is zero).
	w.item = held ? (int32_t)(int16_t)mHost->SlotQuantity(heldSlot) : 0;
	int16_t ammoItem = 0;
	if (w.actions[0].b[0x31] & 0x80) { w.ammo = 0; w.ammoCap = 0x100; }      // WC:443-445
	else
	{
		w.ammoCap = 1;
		w.ammo = 0;
		if (w.actions[0].D(0x30) != 0)                       // WC:448-454
			ammoItem = (int16_t)(w.actions[0].b[0x30] | (w.actions[0].b[0x31] << 8));
	}
	// arm_weapon_and_cache_def (WC:319-325).
	ArmWeaponFire();
	w.sprite = w.actions[0].D(0x00);
	w.xoff = w.actions[0].D(0x20);
	w.yoff = w.actions[0].D(0x1c);
	w.equips++;                                              // WC:458
	w.item0 = itemIndex;
	weaponEvents |= WE_EQUIP;
	return ammoItem;
}

int ActorWorld::FindUsableWeapon(const std::vector<CarriedSlot> &carried, const std::function<int(int16_t)> &ammoCount, int &id) const
{
	id = 0;
	if (mDb == nullptr) return -1;
	for (const CarriedSlot &c : carried)                     // WC:1386-1425
	{
		const int32_t s = (int16_t)c.id;
		if (s == 0 || (c.id & 0x8000)) continue;
		const DbItem *it = mDb->ItemSlot(s);
		if (it == nullptr || it->offset == 0 || (size_t)it->offset + 8 > mDb->Size()) continue;
		if ((mDb->Image()[it->offset + 5] & 0xf) != 1) continue;   // category != weapon
		WeaponAction a;
		ParseWeaponActions(*mDb, s, &a, 1);
		int32_t have = (int16_t)c.quantity;
		if (a.b[0x31] & 0x80) have = (int32_t)(((uint32_t)have & 0x7fff) >> 8);
		else if (have == 0 && a.D(0x30) != 0) have = ammoCount((int16_t)(a.b[0x30] | (a.b[0x31] << 8)));
		if (have == 0 || a.D(8) == 0) continue;
		id = s;
		return c.slot;
	}
	return -1;
}

void ActorWorld::ArmWeaponFire()
{
	PlayerWeapon &w = weapon;
	w.timer = 0;                                             // R:10963-10969
	if (w.anim == 0) w.lower = 0x64;
	else if (w.anim != 1) return;
	w.anim = 2;
}

void ActorWorld::ResetWeaponFireTiming()
{
	weapon.timer = 0;                                        // WC:309-314
	weapon.lower = 0x64;
	weapon.anim = 2;
}

uint32_t ActorWorld::WeaponRng(uint32_t range)
{
	weapon.rng = (uint16_t)(weapon.rng * 0x5e5 + 0x29);      // R:10835-10840
	return ((uint32_t)weapon.rng * range) >> 16;
}

void ActorWorld::TriggerFire(int32_t aimX, int32_t aimY)
{
	PlayerWeapon &w = weapon;
	ActorHost &h = *mHost;
	auto Qty = [&]() { return h.SlotQuantity(w.heldSlot); };
	auto SetQty = [&](int q) { h.SetSlotQuantity(w.heldSlot, (uint16_t)q); };
	if (w.lock != 0) return;                                 // WC:781
	if (w.slots == 0) return;                                // WC:783
	int e = w.active;
	if (e < 0) { e = 0; w.active = 0; }                      // WC:784
	if (w.actions[e].D(0) == 0) return;                      // WC:785
	if (w.active != 0 && w.actions[0].D(4) >= 1 &&          // WC:786-790
		(uint32_t)w.actions[0].D(0x30) >= (uint32_t)w.ammoCap) { e = 0; w.active = 0; }
	w.timer = 0;                                             // WC:792
	int32_t loc4 = 0, loc8 = 0, ecx = 0, ebx = 0, esi = 0;
	const bool bulk = (w.actions[0].b[0x31] & 0x80) != 0;    // [0x811e5] & 0x80
	if (!w.held || w.actions[e].D(4) < 1) goto commit;       // WC:793-796
	{
		const int32_t step = bulk ? 0x100 : 1;
		int q = (int16_t)Qty();
		if ((uint32_t)q < (uint32_t)step) goto empty;        // WC:799-801
		SetQty((uint16_t)(q - step));             // WC:802
		weaponEvents |= WE_AMMO;
		if (step != 1 && w.actions[0].D(4) > 1)              // WC:803-815: a burst
		{
			int32_t n = 1;
			for (;;)
			{
				q = (int16_t)Qty();
				if ((uint32_t)q < (uint32_t)step) break;
				n++;
				SetQty((uint16_t)(q - step));
			}
			w.shotScale = n;
		}
		w.item = (int16_t)Qty();                  // WC:817-822
		e = 0; w.active = 0;
		goto commit;
	}
empty:                                                       // WC:825
	if (w.actions[0].D(0x30) != 0) goto reload;
noAmmo:                                                      // WC:826-833 and 866-871
	if (w.slots > 1) goto alternate;
	weaponEvents |= WE_NOAMMO;
	if (bulk) { h.NoAmmoMessage(5); return; }
	h.NoAmmoMessage(6);
	h.EquipFirstUsableWeapon();
	return;
alternate:                                                   // WC:834-841
	if (e != 0) goto commit;
	e = 1 + (int)WeaponRng((uint32_t)(w.slots - 1));
	w.active = e;
	goto commit;
reload:                                                      // WC:843-856
	if (w.actions[e].b[0x31] & 0x80) goto noAmmo;
	if (w.ammo == 0) goto noAmmo;
	h.RemoveItem((int16_t)(w.actions[e].b[0x30] | (w.actions[e].b[0x31] << 8)));
	weaponEvents |= WE_RELOAD;
	if (w.actions[e].D(4) > 1)
	{
		SetQty((uint16_t)(Qty() + (uint16_t)w.actions[e].D(4)));
		w.item = (int16_t)Qty();
	}
	e = 0; w.active = 0;
	if (w.actions[0].D(0x2c) == 0)                           // WC:858-865
	{
		w.item -= w.ammoCap;
		w.pendingDef = 0;
		SetQty((uint16_t)(Qty() - (uint16_t)w.ammoCap));
		goto commit;
	}
	w.pendingDef = 0;                                        // WC:847-853: the third commit's shot
	esi = w.actions[0].D(0x40); loc8 = w.actions[0].D(0x28); ecx = w.actions[0].D(0x34);
	loc4 = w.actions[0].D(0x24); ebx = w.actions[0].D(0x2c);
	goto fire;
commit:                                                      // WC:872-878
	loc8 = w.actions[e].D(0x20); ecx = w.actions[e].D(0x10); loc4 = w.actions[e].D(0x1c);
	ebx = w.actions[e].D(0x00); esi = w.actions[e].D(0x3c);
	w.pendingDef = w.actions[e].D(0x08);
fire:                                                        // WC:879-897
	if (ebx == 0) return;
	w.lock = -1;
	weaponEvents |= WE_PULL;
	w.bodyRequest = (w.held ? 1 : 0) + 0x101;
	ArmWeaponFire();
	if (ecx != 0) h.WeaponSound(ecx - 1);
	w.sprite = ebx; w.xoff = loc8; w.rate = esi; w.yoff = loc4;
	w.aimY = aimY; w.aimX = aimX;
}

void ActorWorld::FirePendingShot(int action, int32_t aimYaw)
{
	PlayerWeapon &w = weapon;
	if (w.pendingDef == 0) return;                           // WC:749
	if ((uint32_t)lightFlash < (uint32_t)w.actions[action].D(0x44)) lightFlash = (uint8_t)w.actions[action].D(0x44);   // WC:750-752
	const int slot = FirePlayer((uint16_t)w.pendingDef, aimYaw);   // WC:754 (screen aim: the hand's yaw)
	weaponEvents |= WE_SHOT;
	if (slot >= 0 && w.shotScale != 0)                       // WC:755-759
		flying[slot].damage = (uint16_t)(flying[slot].damage * (uint16_t)w.shotScale);
	w.shotScale = 0;
}

void ActorWorld::WeaponFrame(int fts, int32_t aimYaw)
{
	PlayerWeapon &w = weapon;
	ActorHost &h = *mHost;
	auto Qty = [&]() { return w.heldSlot >= 0 ? h.SlotQuantity(w.heldSlot) : 0; };
	mWeaponView = WeaponView();
	w.presClock += (uint32_t)fts;
	if (lightFlash != 0)                                     // player.c:458-462
	{
		const int v = (int)lightFlash - fts;
		lightFlash = (uint8_t)(v < 0 ? 0 : v);
	}
	if (w.anim == 0) return;                                 // player.c:463: put away
	if (h.Health() <= 0) return;                             // player.c:464
	if (w.timer <= 0x578) w.timer += (uint32_t)fts;          // player.c:466-468
	else if (w.anim == 3) { w.lower = 0; w.anim = 1; }
	{
		const int32_t step = fts << 2;                       // player.c:470-479
		if (w.anim == 2) { w.lower -= step; if (w.lower < 0) { w.lower = 0; w.anim = 3; } }
		else if (w.anim == 1) { w.lower += step; if (w.lower > 0x64) w.anim = 0; }
	}
	int e = w.active;
	if (e < 0) { e = 0; w.active = 0; }                      // player.c:481-482
	if (w.rate == 0) w.rate = 8;                             // player.c:483
	if (w.lock == 0) goto view;
	if (w.lock != -1) goto cooling;
	w.lock = 1;                                              // player.c:488-501: the trigger, armed
	w.shotDone = 0;
	w.cycleEnd = 0x3e8;
	w.leadSet = 1;
	w.counter = (uint16_t)((uint16_t)w.rate + (uint16_t)fts);
	w.lead = 0;
	if ((uint32_t)w.lock >= (uint32_t)w.actions[e].D(0x0c) && w.shotDone == 0) { FirePendingShot(e, aimYaw); w.shotDone = 1; }
	goto view;
cooling:                                                     // player.c:508-520
	w.lock = (int32_t)((uint32_t)(int32_t)(int16_t)w.counter / (uint32_t)w.rate);
	w.counter = (uint16_t)(w.counter + (uint16_t)fts);
	if ((uint32_t)w.lock >= (uint32_t)w.actions[e].D(0x0c) && w.shotDone == 0) { FirePendingShot(e, aimYaw); w.shotDone = 1; }
	if ((uint32_t)w.lock < w.cycleEnd) goto view;
	w.lock = 0;
	weaponEvents |= WE_CYCLE_END;
	if (w.pendingDef != 0 && w.actions[e].D(0x38) == w.actions[e].D(0x08))   // player.c:522-532: used up
	{
		if (w.held) { h.ConsumeHeldWeapon(); e = 0; w.active = 0; }
		if (w.anim == 3) { w.lower = 0; w.anim = 1; }
	}
	if (w.slots > 1)                                         // player.c:535-558: the next action
	{
		const int32_t b8 = w.actions[0].D(4);
		if (b8 == 0) e = (int)WeaponRng((uint32_t)w.slots);
		else if (e != 0)
		{
			const uint32_t n = (uint32_t)(w.slots - 1);
			if (b8 < 0) e = (-b8 <= w.word8a0f4) ? 0 : 1 + (int)WeaponRng(n);
			else if ((int16_t)Qty() == 0 && e != 0) e = 1 + (int)WeaponRng(n);
		}
	}
	w.active = e;                                            // player.c:559-562
	w.sprite = w.actions[e].D(0x00);
	w.xoff = w.actions[e].D(0x20);
	w.yoff = w.actions[e].D(0x1c);
view:                                                        // player.c:564-640: the weapon view
	{
		if (w.lock == 0) w.bob += (uint32_t)fts;
		int32_t bx = (int32_t)(int16_t)ActorSin(((w.bob << 2) & 0x1ff) << 1) >> 0xb;
		const int32_t bobY = (int32_t)(int16_t)ActorSin(((w.bob << 3) & 0x1ff) << 1) >> 0xc;
		if (w.sprite != w.spriteShown) w.spriteShown = w.sprite;
		if (w.sprite == 0) return;
		const int frames = h.WeaponFrames(w.sprite);
		if (frames <= 0) return;
		const int32_t hx = ((w.actions[e].b[0x4c] & 1) && w.leadSet) ? w.lead : 0;
		w.sway += (hx - w.sway) >> 1;
		bx += w.sway + w.xoff;
		const bool fixed = (w.actions[e].b[0x4c] & 2) != 0;
		// The camera's own bob (0x853d3 + 0x40 >> 1) is the engine's to add.
		const int32_t vbob = 0x20 + bobY + w.lower;
		mWeaponView.shown = true;
		mWeaponView.sprite = w.sprite;
		if (w.lock != 0)
		{
			w.cycleEnd = (uint32_t)(int32_t)(int16_t)frames;  // player.c:619
			mWeaponView.frame = w.lock;
			mWeaponView.x = fixed ? w.xoff : bx;
			mWeaponView.y = fixed ? w.lower + w.yoff : w.yoff + vbob;
			return;
		}
		mWeaponView.frame = 0;
		mWeaponView.x = fixed ? w.xoff : bx;
		mWeaponView.y = fixed ? w.lower + w.yoff : w.yoff + vbob;
		w.leadSet = 0;                                       // player.c:632-636
		w.sprite = w.actions[e].D(0x00);
		w.xoff = w.actions[e].D(0x20);
		w.yoff = w.actions[e].D(0x1c);
	}
}

void ActorWorld::RebuildWeaponList(const std::vector<std::pair<int, uint16_t>> &usedSlots)
{
	PlayerWeapon &w = weapon;
	w.recharge.clear();
	for (const auto &u : usedSlots)                          // WC:494-515
	{
		if (u.second == 0) continue;
		if (w.recharge.size() >= 0x10) continue;
		WeaponAction a;
		ParseWeaponActions(*mDb, (int)(int16_t)(u.second & 0x7fff), &a, 1);
		const uint32_t a30 = (uint32_t)a.D(0x30);
		if (!(a30 & 0x8000)) continue;
		w.recharge.push_back({ u.first, (uint16_t)a.D(4), (uint16_t)(a30 & 0x7fff) });
	}
}

void ActorWorld::RechargeFrame(int fts)
{
	PlayerWeapon &w = weapon;
	if (w.recharge.empty()) return;                          // WC:947
	w.rechargeAccum += (uint32_t)fts;
	if (w.rechargeAccum <= 0x10) return;
	w.rechargeAccum -= 0x10;
	for (const PlayerWeapon::Recharge &r : w.recharge)       // WC:952-988
	{
		int32_t cur = (int16_t)mHost->SlotQuantity(r.slot);
		const uint32_t target = (uint32_t)cur >> 8;
		if ((uint32_t)r.full <= target) continue;
		const int32_t rate = (int32_t)r.rate * 0x46;
		int32_t step = rate != 0 ? 0x1000 / rate : 0x1000;
		if (step == 0) step = 1;
		step = (int32_t)((uint32_t)step / (target + 1));
		cur += step;
		mHost->SetSlotQuantity(r.slot, (uint16_t)cur);
		weaponEvents |= WE_RECHARGE;
		if (r.slot == w.heldSlot) w.item = cur;
	}
}

ActorWorld::WeaponPresentation ActorWorld::PresentWeapon() const
{
	const PlayerWeapon &w = weapon;
	WeaponPresentation p;
	p.item = w.item0;
	p.sprite = mWeaponView.shown ? mWeaponView.sprite : w.sprite;
	p.frame = mWeaponView.frame;
	p.x = mWeaponView.x; p.y = mWeaponView.y;
	p.frameCount = (mHost != nullptr && p.sprite != 0) ? mHost->WeaponFrames(p.sprite) : 0;
	const int e = w.active < 0 ? 0 : w.active;
	if (w.anim == 0) p.state = WS_AWAY;
	else if (w.lock != 0)
		p.state = (w.pendingDef == 0 && w.actions[0].D(0x2c) != 0 && w.sprite == w.actions[0].D(0x2c)) ? WS_RELOAD : WS_FIRE;
	else if (w.anim == 2) p.state = WS_RAISE;
	else if (w.anim == 1) p.state = WS_LOWER;
	else p.state = WS_READY;
	(void)e;
	PlayerWeapon &mw = const_cast<PlayerWeapon &>(w);
	if (p.state != w.presState) { mw.presState = p.state; mw.presSince = w.presClock; }
	p.ticksInState = w.presClock - w.presSince;
	switch (p.state)
	{
	case WS_RAISE: p.stateLength = 25; p.progress = 1.0 - w.lower / 100.0; break;
	case WS_LOWER: p.stateLength = 25; p.progress = w.lower / 100.0; break;
	case WS_FIRE: case WS_RELOAD:
	{
		const int rate = w.rate != 0 ? w.rate : 8;
		const int frames = p.frameCount > 0 ? p.frameCount : (int)w.cycleEnd;
		p.stateLength = (uint32_t)(frames * rate);
		p.progress = p.stateLength ? (double)w.counter / (double)p.stateLength : 0.0;
		break;
	}
	default: p.stateLength = 0; p.progress = 0.0; break;
	}
	if (p.progress < 0) p.progress = 0;
	if (p.progress > 1) p.progress = 1;
	if (w.actions[0].b[0x31] & 0x80 && w.heldSlot >= 0 && mHost != nullptr)
	{
		p.charge = (int)(((uint32_t)mHost->SlotQuantity(w.heldSlot) & 0xffff) >> 8);
		p.chargeFull = w.actions[0].D(4);
	}
	return p;
}

std::string ActorWorld::DumpWeapon() const
{
	const PlayerWeapon &w = weapon;
	char b[400];
	snprintf(b, sizeof b, "WPN lock=%d done=%d ctr=%04x anim=%d lower=%d timer=%u def=%d spr=%x rate=%d end=%u act=%d n=%d item=%d ammo=%d cap=%d scale=%d rng=%04x xo=%d yo=%d sway=%d bob=%u held=%d qty=%d hs=%d fl=%u\n",
		w.lock, w.shotDone, w.counter, w.anim, w.lower, w.timer, w.pendingDef, w.sprite, w.rate, w.cycleEnd, w.active, w.slots,
		w.item, w.ammo, w.ammoCap, w.shotScale, w.rng, w.xoff, w.yoff, w.sway, w.bob, w.held ? 1 : 0, w.heldSlot >= 0 ? mHost->SlotQuantity(w.heldSlot) : -1, w.heldSlot, (unsigned)lightFlash);
	return b;
}

} // namespace roth
