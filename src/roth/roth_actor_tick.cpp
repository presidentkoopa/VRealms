//
// One tick of Realms of the Haunting's actors: bullets fly and land, creatures
// die, settle, rise, attack, flinch, think, walk and turn. Part of ActorWorld
// (roth_actors.h); engine-free.
//
// Sources: ROTH.C entity_ai.c (EA), weapon_combat.c (WC), renderer.c (R),
// raw_commands.c (RC), audio.c. Field offsets in the comments are the
// original's; the names are in roth_actors.h.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_actors.h"
#include "roth_raw.h"

namespace roth
{

namespace
{
inline uint16_t ObjId(const Object &o) { return (uint16_t)(o.textureIndex | (o.textureSource << 8)); }
inline void SetObjId(Object &o, uint16_t id) { o.textureIndex = (uint8_t)id; o.textureSource = (uint8_t)(id >> 8); }
}

// The damage generator (EA:721-726): a 16-bit state, x * 0x5e5 + 0x29.
uint16_t ActorWorld::DamageRoll()
{
	rngDamage = (uint16_t)((uint32_t)rngDamage * 0x5e5u + 0x29u);
	return rngDamage;
}

const ActorDef *ActorWorld::CreatureDef(int creature) const
{
	if (creature < 0 || (size_t)creature >= creatureRt.size()) return nullptr;
	return Def(creatureRt[creature].def);
}

// revalidate_entity_def (R:10739-10746). The shared creature record remembers
// the id its statistics were looked up by; when the cache node it points at no
// longer holds that id, look it up again. False when there are none.
bool ActorWorld::Revalidate(int creature)
{
	if (creature < 0 || (size_t)creature >= creatureRt.size()) return false;
	CreatureRuntime &rt = creatureRt[creature];
	const ActorDef *def = Def(rt.def);
	if (def != nullptr && def->Id() == rt.defId) return true;
	rt.def = LookupDef(rt.defId);
	return rt.def >= 0;
}

//==========================================================================
//
// The tick
//
//==========================================================================

void ActorWorld::Tick()
{
	if (mMap == nullptr || mCollision == nullptr) return;
	TickFlying();
	int left = actorCount;
	for (int i = 0; i < 16 && left > 0; i++)                 // EA:1361-1372
	{
		if (actors[i].object == 0) continue;
		if (actors[i].creature >= 0) TickActor(i);
		left--;                                              // a slot with no record still counts
	}
}

// update_dynamic_entities (EA:1053-1085).
void ActorWorld::TickFlying()
{
	int left = flyingCount;
	for (int i = 0; i < 16 && left > 0; i++)
	{
		FlyingSlot &f = flying[i];
		if (f.object == 0) continue;
		if (f.mode & 1) FlyBullet(i);
		else if (f.mode & 2)
		{
			// An impact picture: shown for 0x100 ticks, then removed (EA:1067-1075).
			if (f.owner < 0x100) f.owner++;
			else DestroyObject(f.object);
		}
		else FlyWalker(i);
		left--;
	}
}

// check_projectile_sector_clearance (WC:117-127): true when the height is
// outside the sector by more than half the bullet's width.
static bool BulletOutsideSector(const Map &map, int sector, int16_t z, uint32_t width)
{
	if (sector < 0 || (size_t)sector >= map.sectors.size()) return false;
	const uint16_t half = (uint16_t)(width >> 1);
	if ((int16_t)(uint16_t)((uint16_t)map.sectors[sector].floorHeight + half) > z) return true;
	if ((int16_t)(uint16_t)((uint16_t)map.sectors[sector].ceilingHeight - half) < z) return true;
	return false;
}

// ud_walker (EA:899-940): a flying-pool record that is neither a bullet nor
// an impact picture drifts along its angle and turns when it cannot go on.
void ActorWorld::FlyWalker(int slot)
{
	FlyingSlot &f = flying[slot];
	Object *obj = Find(f.object);
	if (obj == nullptr) return;
	uint32_t idx = (uint32_t)(uint16_t)(f.angle - 0x100u);
	idx += idx;
	const int32_t A = ActorSin(idx);
	const int32_t B = ActorSin((idx & 0xffff00ffu) | ((((idx >> 8) + 1u) & 0xffu) << 8));
	const int32_t spd4 = (int32_t)((uint32_t)f.speed << 2);
	const uint32_t a = (uint32_t)(A * spd4), b = (uint32_t)(B * spd4);

	uint32_t sum = (uint32_t)f.fracX + (uint16_t)a;
	f.fracX = (uint16_t)sum;
	const uint16_t nx = (uint16_t)((a >> 16) + (uint16_t)obj->x + (sum >> 16));
	sum = (uint32_t)f.fracY + (uint16_t)b;
	f.fracY = (uint16_t)sum;
	const uint16_t ny = (uint16_t)((b >> 16) + (uint16_t)obj->y + (sum >> 16));

	const int sec = LocateSector((int16_t)nx, (int16_t)ny, obj->z, f.sector);
	if (sec >= 0)
	{
		// EA:924-926 tests the mode bit again and so always takes the entity
		// form (R:10098): below the floor, or taller than the room.
		const Sector &s = mMap->sectors[sec];
		const int16_t tall = (int16_t)mTables->DimHeight(ObjId(*obj));
		const bool blocked = s.floorHeight > obj->z || (int16_t)(s.ceilingHeight - tall) < obj->z;
		if (!blocked)
		{
			if (sec != f.sector)
			{
				f.sector = sec;
				MoveObjectToSector(f.object, sec);
				obj = Find(f.object);
			}
			obj->y = (int16_t)ny;
			obj->x = (int16_t)nx;
			mHost->ObjectChanged(f.object);
			return;
		}
	}
	f.angle = (uint16_t)((f.angle & 0x00ffu) | ((((f.angle >> 8) + 0xffu) & 0xffu) << 8));
	obj->rotation = (uint8_t)(f.angle >> 1);
	mHost->ObjectChanged(f.object);
}

// compute_projectile_hit_damage (R:10754-10779): the bullet's damage times a
// multiplier from the target's two lists. The first immunity that matches the
// bullet's item id (or is -1) makes it 0; each matching vulnerability doubles
// it, or brings it back to 1.
uint32_t ActorWorld::HitDamage(int actor, uint16_t type, uint16_t base)
{
	const int creature = actors[actor].creature;
	if (CreatureDef(creature) == nullptr) return base;
	if (!Revalidate(creature)) return base;
	const ActorDef &def = *CreatureDef(creature);
	int32_t mult = 1;
	int32_t c = def.W(0x2c);
	for (int i = 0; i < c && 0x2e + i * 2 + 2 <= 0x6c; i++)
	{
		const int16_t v = (int16_t)def.W(0x2e + i * 2);
		if (v == -1 || v == (int16_t)type) { mult = 0; break; }
	}
	c = def.W(0x46);
	for (int i = 0; i < c && 0x48 + i * 2 + 2 <= 0x6c; i++)
	{
		const int16_t v = (int16_t)def.W(0x48 + i * 2);
		if (v == -1 || v == (int16_t)type) mult = mult != 0 ? mult * 2 : 1;
	}
	return (uint32_t)base * (uint32_t)mult;
}

// init_projectile_from_item (WC:175-204): what a bullet leaves behind. True
// when nothing is left and the record is to be destroyed.
bool ActorWorld::BulletSpent(int slot)
{
	FlyingSlot &f = flying[slot];
	if (f.damageType == 0) return true;
	const BulletDesc d = BulletFor(f.damageType);
	if (d.templateId == 0) return true;
	Object *obj = Find(f.object);
	if (obj == nullptr) return true;
	if (d.hitSound != 0) mHost->SoundAt(d.hitSound - 1, (uint16_t)obj->x, (uint16_t)obj->y);
	if (d.hitAnim == 0) return true;
	SetObjId(*obj, d.hitAnim);
	obj->flags |= 0x40;
	f.mode = 2;
	if (d.flash > lightFlash) lightFlash = d.flash;
	f.owner = 0;                                             // now the countdown (WC:202)
	mHost->ObjectChanged(f.object);
	return false;
}

// ud_projectile (EA:946-1050).
void ActorWorld::FlyBullet(int slot)
{
	FlyingSlot &f = flying[slot];
	Object *obj = Find(f.object);
	if (obj == nullptr) return;

	const uint32_t idx = (uint32_t)(uint8_t)(obj->rotation + 0x80u) << 2;
	const int32_t A = ActorSin(idx);
	const int32_t B = ActorSin((idx & 0xffff00ffu) | ((((idx >> 8) + 1u) & 0xffu) << 8));
	const int32_t spd = f.speed;

	SweepQuery q;
	q.self = f.object;
	q.x = ((uint32_t)(uint16_t)obj->x << 16) | f.fracX;
	q.y = ((uint32_t)(uint16_t)obj->y << 16) | f.fracY;
	// CP:1583-1586: the height's low half is the word at record +0x0e, which
	// is the high half of the stored aim (+0x0c is fracY, +0x10 the aim).
	q.z = ((uint32_t)(uint16_t)obj->z << 16) | f.fracZ;
	q.sector = f.sector;
	q.velX = (int32_t)((uint32_t)(A * spd) << 3);
	q.velY = (int32_t)((uint32_t)(B * spd) << 3);
	q.velZ = (int32_t)((uint32_t)(f.zVelocity * spd) << 10);
	q.speed = (uint32_t)spd;
	q.width = f.width;
	q.owner = f.owner;
	const SweepAnswer r = mCollision->Sweep(q);

	// The writeback (CP:1643-1660).
	f.fracX = (uint16_t)r.x; obj->x = (int16_t)(r.x >> 16);
	f.fracY = (uint16_t)r.y; obj->y = (int16_t)(r.y >> 16);
	f.fracZ = (uint16_t)r.z; obj->z = (int16_t)(r.z >> 16);
	moveBlock.x = r.x; moveBlock.y = r.y; moveBlock.targetZ = r.z;
	moveBlock.floor = moveBlock.standZ = (uint16_t)r.z;
	moveBlock.sector = r.sector;
	moveBlock.hit = r.hit;
	moveBlock.hitObject = r.hitObject;
	mHost->ObjectChanged(f.object);

	const int sec = r.sector;
	bool markOwner = false;
	if (!(r.hit & 3))                                        // a clean tick of flight
	{
		if (!BulletOutsideSector(*mMap, sec, obj->z, f.width))
		{
			if (f.ttl != 0)
			{
				const uint8_t was = f.ttl;
				f.ttl = (uint8_t)(was - 1);
				if (was == 1)                                // ran out (EA:967-969)
				{
					if ((int16_t)f.owner > 0) actors[(f.owner - 1) / 0x22].bits |= 0x80;
					DestroyObject(f.object);
					return;
				}
			}
			if (sec != f.sector && sec >= 0)
			{
				f.sector = sec;
				MoveObjectToSector(f.object, sec);
			}
			return;
		}
	}
	else
	{
		if (r.hit & 2)
		{
			if (r.hitObject == 0)
			{
				// The player (EA:986-996): half the bullet's damage plus a
				// random part of the other half, through the armour.
				if (mHost->Health() > 0)
				{
					const uint32_t dmg = f.damage;
					const uint32_t part = ((uint32_t)DamageRoll() * dmg) >> 16;
					const int32_t val = (int32_t)(dmg + part) >> 1;
					if (val != 0)
					{
						DamageReduced((uint32_t)val);
						mHost->PlayerHurt(val);
					}
				}
				if (BulletSpent(slot)) DestroyObject(f.object);
				return;
			}
			Object *hobj = Find(r.hitObject);
			if (hobj != nullptr && (hobj->renderType & 1) && hobj->unk0C != 0)
			{
				const int ai = (hobj->unk0C - 1) / 0x22;
				ActorSlot &a = actors[ai];
				if (!(a.state & 0x20) && !(a.bits & 1))      // not a corpse, not already dying
				{
					const uint32_t base = HitDamage(ai, f.damageType, f.damage);
					const uint32_t part = ((uint32_t)DamageRoll() * base) >> 16;
					const int32_t dmg = (int32_t)(base + part) >> 1;
					if (dmg != 0)
					{
						// resolve_projectile_target_entity (WC:133-158) ->
						// play_distance_variant_sound (audio.c:977): one of the
						// creature's pain sounds, the last when the hit is more
						// than half its full health, otherwise picked by the
						// damage generator's current value (not advanced).
						if (CreatureDef(a.creature) != nullptr && Revalidate(a.creature))
						{
							const ActorDef &def = *CreatureDef(a.creature);
							const uint32_t count = def.W(0x22);
							if (count != 0)
							{
								uint32_t pick = 0;
								if (count != 1)
								{
									pick = count - 1;
									if (!((uint32_t)dmg > def.Health()) && !((uint32_t)dmg * 2 > def.Health()))
										pick = (pick * (uint32_t)rngDamage) >> 16;
								}
								const uint16_t snd = 0x24 + pick * 2 + 2 <= 0x6c ? def.W((int)(0x24 + pick * 2)) : 0;
								if (snd != 0) mHost->SoundAt(snd - 1, (uint16_t)hobj->x, (uint16_t)hobj->y);
							}
						}
						a.pain = 0x18;
						a.health -= dmg;
						if (a.health <= 0)
						{
							a.health = dmg;                  // the size of the killing hit (EA:1016)
							a.bits = 1;
						}
					}
				}
			}
		}
		markOwner = true;                                    // EA:1027-1032
	}
	if (markOwner && (int16_t)f.owner > 0) actors[(f.owner - 1) / 0x22].bits |= 0x80;
	if (BulletSpent(slot)) DestroyObject(f.object);
}

//==========================================================================
//
// One actor (EA:1311-1341)
//
//==========================================================================

void ActorWorld::TickActor(int slot)
{
	ActorSlot &a = actors[slot];
	if (a.bits & 1)
	{
		a.bits &= 0xfe;
		ActorDied(slot);
		return;
	}
	const uint8_t st = a.state;
	if (st & 0x20) { ActorCorpse(slot); return; }
	if (st & 0x40) { if (!ActorStagger(slot)) return; }
	else if (st & 0x08) { if (!ActorAttackRecover(slot)) return; }
	else if (a.pain != 0)
	{
		ActorAim(slot, 0x3e8);                               // EA:1335-1338
		ActorVertical(slot, 0, 0);
		a.pain--;
		return;
	}
	ActorThink(slot);
}

// fire_entity_pending_trigger (RC:2809-2836) then
// reset_entity_state_with_sound (EA:80-103).
void ActorWorld::ActorDied(int slot)
{
	ActorSlot &a = actors[slot];
	Object *obj = Find(a.object);
	if (obj == nullptr) return;
	obj->flags &= 0xbf;
	if (obj->renderType & 0x20)
	{
		obj->renderType -= 0x20;
		const ObjectUid uid = a.object;
		mHost->RunObjectChain(uid);
		if (a.object == 0) return;                           // the chain removed it (RC:2817)
		obj = Find(a.object);
		if (obj == nullptr) return;
		if (obj->commandID == 0 && !(obj->renderType & 0x21)) obj->flags &= 0xfd;
	}
	if ((a.state & 0x40) && a.sector >= 0)
		obj->z = mMap->sectors[a.sector].floorHeight;        // RC:2826-2832

	const uint32_t killed = (uint32_t)a.health;
	a.state = 0x20;
	a.settle = 1;
	if (CreatureDef(a.creature) != nullptr && Revalidate(a.creature))
	{
		const ActorDef &def = *CreatureDef(a.creature);
		uint16_t snd = def.DeathSound();
		if (killed >= def.Health())                          // one hit of its full health: the critical death
		{
			snd = def.CritDeathSound();
			a.state |= 1;
		}
		if (snd != 0) mHost->SoundAt(snd - 1, (uint16_t)obj->x, (uint16_t)obj->y);
	}
	a.counter = 0;
	mHost->ObjectChanged(a.object);
}

// ta_corpse (EA:1092-1139).
void ActorWorld::ActorCorpse(int slot)
{
	ActorSlot &a = actors[slot];
	a.pain = 0;
	Object *obj = Find(a.object);
	if (obj == nullptr) return;
	if (a.sector >= 0)
	{
		int16_t d = (int16_t)((uint16_t)mMap->sectors[a.sector].floorHeight - (uint16_t)obj->z);
		if (d != 0)
		{
			const uint32_t step = (uint32_t)a.settle + 1u, neg = 0u - step;
			if (d > (int16_t)(uint16_t)step) d = (int16_t)(uint16_t)step;
			if (d < (int16_t)(uint16_t)neg) d = (int16_t)(uint16_t)neg;
			obj->z = (int16_t)(obj->z + d);
			if ((int8_t)(uint8_t)neg > (int8_t)0xe0) a.settle++;
			if (a.counter != 0xffff) a.counter++;
			mHost->ObjectChanged(a.object);
			return;
		}
	}
	if (a.counter > 0x100)
	{
		// The corpse is done (EA:1113-1134). With a "dead" picture above
		// 0x1200 the object stays as a plain prop wearing picture - 0x1000 and
		// the slot is freed; otherwise the object goes too.
		a.counter = 0;
		uint16_t z = mTables->CreatureWord(a.creature, 0x56);
		if (a.state & 1) z = mTables->CreatureWord(a.creature, 0x5a);
		if (z > 0x1200)
		{
			SetObjId(*obj, (uint16_t)(z - 0x1000));
			if (obj->commandID == 0) obj->renderType |= 2;
			obj->renderType &= 0xfe;
			obj->unk0C = 0;
			const ObjectUid uid = a.object;
			a.creature = -1;
			a.object = 0;
			actorCount--;
			mHost->ObjectChanged(uid);
		}
		else DestroyObject(a.object);
		return;
	}
	if (a.counter != 0xffff) a.counter++;
}

// ta_stagger (EA:1143-1199). True when the creature goes on to think this tick.
bool ActorWorld::ActorStagger(int slot)
{
	ActorSlot &a = actors[slot];
	a.pain = 0;
	Object *obj = Find(a.object);
	if (obj == nullptr) return false;
	if (a.state & 1)                                         // rising out of the ground
	{
		if (a.counter <= 0x100)
		{
			a.counter++;
			if (a.settle != 0)
			{
				obj->z = (int16_t)(obj->z - 2);
				a.settle = (uint8_t)(a.settle >= 2 ? a.settle - 2 : 0);
				mHost->ObjectChanged(a.object);
			}
			return false;
		}
		a.state &= 0xbc;
		a.state |= 0x40;
		a.counter = 0;
		a.settle = 0x50;
		if (const ActorDef *def = CreatureDef(a.creature))
			if (def->SpawnSound() != 0) mHost->SoundAt(def->SpawnSound() - 1, (uint16_t)obj->x, (uint16_t)obj->y);
		return false;
	}
	if (a.counter <= 0x100)
	{
		a.counter++;
		if (a.settle != 0)
		{
			obj->z = (int16_t)(obj->z + 2);
			a.settle = (uint8_t)(a.settle >= 2 ? a.settle - 2 : 0);
			mHost->ObjectChanged(a.object);
		}
		return false;
	}
	a.counter = 0;
	a.settle = 0;
	a.state &= 0xbc;
	obj->flags &= 0xbf;
	if (mTables->CreatureWord(a.creature, 0x62) != 0)        // has a special idle picture
	{
		a.state = 5;
		a.counter = 0;
	}
	return true;
}

// ta_attack_recover (EA:1204-1237).
bool ActorWorld::ActorAttackRecover(int slot)
{
	ActorSlot &a = actors[slot];
	a.pain = 0;
	if (a.fireDelay != 0 && (uint8_t)a.counter >= a.fireDelay)
	{
		a.fireDelay = 0;
		if (a.bullet != 0)
		{
			if (const ActorDef *def = CreatureDef(a.creature))
				if (def->Flash() > lightFlash) lightFlash = def->Flash();
			if (Object *obj = Find(a.object))
			{
				const Object copy = *obj;                    // the spawn may move the list it is in
				FireFrom(a.bullet, copy, 0x40, false);
			}
		}
	}
	if (a.counter <= 0x100)
	{
		a.counter++;
		ActorAim(slot, 0x2710);
		ActorVertical(slot, 0, 0);
		return false;
	}
	a.counter = 0;
	a.state &= 0xf4;
	return true;
}

// ta_ambient_sound (EA:1243-1276).
void ActorWorld::ActorAmbient(int slot, const ActorDef &def)
{
	const int32_t cd = animScheduler - 1;
	if (cd >= 0) { animScheduler = cd; return; }
	uint32_t r = rngAnim >> 8;
	if (r == 0)
	{
		r = rngAnimSeed * 0x5e5u + 0x29u;
		rngAnimSeed = (rngAnimSeed & 0xffff0000u) | (r & 0xffffu);   // a word is written back
	}
	rngAnim = r;
	const uint32_t rnd8 = r & 0xffu;
	const uint32_t count = def.W(0x18);
	uint32_t idx;
	if (count == 1) idx = 1;
	else
	{
		const uint32_t p = (count * rnd8) >> 8;
		idx = ((uint16_t)p < (uint16_t)count) ? p + 1 : count;
	}
	const uint32_t off = 0x18 + (idx & 0xffffu) * 2;
	const uint32_t entry = off + 2 <= 0x6c ? def.W((int)off) : 0;
	uint32_t add = 0x46;
	if (entry == 0xffffu) add = 0xaa;
	else if (entry != 0)
		if (const Object *obj = Find(actors[slot].object))
			mHost->SoundAt((int)entry - 1, (uint16_t)obj->x, (uint16_t)obj->y);
	animScheduler = (int32_t)((((rnd8 & 0xfu) + add) * ((uint32_t)actorCount + 1u)) >> 3);
}

// ta_think (EA:1281-1308).
void ActorWorld::ActorThink(int slot)
{
	ActorSlot &a = actors[slot];
	if (CreatureDef(a.creature) == nullptr) return;
	if (!Revalidate(a.creature)) return;
	const ActorDef &def = *CreatureDef(a.creature);
	const uint8_t moveRate = def.MoveRate();
	uint32_t turn = def.TurnLimit();
	if (a.state == 5)
	{
		turn = 0x2710;
		if (a.counter <= 0x100) a.counter++;
		else { a.counter = 0; a.state &= 0xf8; }
	}
	if (def.W(0x18) != 0) ActorAmbient(slot, def);
	ActorLocomotion(slot, moveRate);
	if (!(a.bits & 8)) ActorAim(slot, turn);
}

// check_entity_player_contact (R:10689-10729). The branch that uses it tests
// the sign flag the last 16-bit operation left, so everything here is 16-bit.
bool ActorWorld::ActorTouchesPlayer(int slot) const
{
	const ActorSlot &a = actors[slot];
	const Object *obj = const_cast<ActorWorld *>(this)->Find(a.object);
	if (obj == nullptr) return false;
	uint16_t dx = (uint16_t)((uint16_t)obj->x - (uint16_t)player.x);
	if (dx & 0x8000) dx = (uint16_t)(0 - dx);
	uint16_t dy = (uint16_t)((uint16_t)obj->y - (uint16_t)player.y);
	if (dy & 0x8000) dy = (uint16_t)(0 - dy);
	uint16_t m = dx;
	if (!((int16_t)dx > (int16_t)dy)) m = dy;
	m = (uint16_t)(m - ((a.bits & 0x80) ? 0x30 : 0x60));
	m = (uint16_t)(m - mTables->DimRadius(ObjId(*obj)));
	if (!(m & 0x8000)) return false;                         // too far in plan
	uint16_t dz = (uint16_t)((uint16_t)obj->z - (uint16_t)player.z);
	if (dz & 0x8000) dz = (uint16_t)(0 - dz);
	if (dz > 0xaa) return (((uint16_t)(dz - 0xaa)) & 0x8000) != 0;
	return true;
}

// update_actor_movement_ai (EA:574-627).
void ActorWorld::ActorLocomotion(int slot, uint8_t moveRate)
{
	ActorSlot &a = actors[slot];
	if (ActorTouchesPlayer(slot))
	{
		ActorVertical(slot, 0, 0);
		a.state |= 0x10;
		if (a.bits & 8) a.counter = 0;
		a.bits &= 0x57;
		return;
	}
	a.state &= 0xef;
	const uint8_t acc = (uint8_t)(a.stepAccum + moveRate);
	a.stepAccum = acc;
	if (acc < 0x10) return;
	a.stepAccum = (uint8_t)(acc - 0x10);

	if ((a.bits & 8) && a.counter != 0)
	{
		a.counter--;
		if (a.counter == 0) a.bits &= 0xd7;
		else
		{
			const uint8_t was = a.bits;
			a.bits = (uint8_t)(was & 0xbf);
			if (!(was & 0x40) && (int16_t)a.counter >= 0x50)
			{
				rngWander = rngWander * 0x5e5u + 0x29u;
				if ((uint8_t)(rngWander >> 8) <= 0x1e)
					a.bits = (uint8_t)((a.bits & 0xef) | (rngWander & 0x10));
			}
		}
	}
	int32_t dx = 0, dy = 0;
	if ((int16_t)a.fallSpeed >= 0)
	{
		const Object *obj = Find(a.object);
		uint32_t idx = ((((uint32_t)(obj ? obj->rotation : 0) + 0x80u) & 0xffu) << 2) & 0x3feu;
		dx = (int32_t)((uint32_t)ActorSin(idx) << 5);
		idx = (idx + 0x100u) & 0x3feu;
		dy = (int32_t)((uint32_t)ActorSin(idx) << 5);
	}
	ActorVertical(slot, dx, dy);
}

// enemy_attack_apply (WC:216-242).
void ActorWorld::AttackApply(int slot, const ActorDef &def, bool secondary)
{
	ActorSlot &a = actors[slot];
	if (secondary) a.state |= 1;
	a.fireDelay = (uint8_t)(def.AttackDelay(secondary) + 1);
	const uint16_t bullet = def.Bullet(secondary);
	if (bullet != 0)
	{
		a.bullet = bullet;
		const uint16_t snd = def.AttackSound(secondary);
		if (snd != 0)
			if (const Object *obj = Find(a.object))
				mHost->SoundAt(snd - 1, (uint16_t)obj->x, (uint16_t)obj->y);
	}
	a.state |= 8;
	a.counter = 0;
	mHost->ObjectChanged(a.object);
}

// begin_enemy_attack (WC:248-268): in contact with the player.
void ActorWorld::BeginAttack(int slot)
{
	ActorSlot &a = actors[slot];
	if (CreatureDef(a.creature) == nullptr || !Revalidate(a.creature)) return;
	const ActorDef &def = *CreatureDef(a.creature);
	a.state &= 0xfc;
	bool secondary;
	if (def.Bullet(1) == 0) secondary = false;
	else if (a.move & 2) secondary = true;
	else secondary = (rngWander & 1) != 0;                   // the generator's current value, not advanced
	AttackApply(slot, def, secondary);
}

// launch_enemy_attack_animation (WC:275-300): from a distance.
void ActorWorld::LaunchAttack(int slot)
{
	ActorSlot &a = actors[slot];
	rngWander = rngWander * 0x5e5u + 0x29u;
	if (CreatureDef(a.creature) == nullptr || !Revalidate(a.creature)) return;
	const ActorDef &def = *CreatureDef(a.creature);
	const uint8_t modes = (uint8_t)(a.bits & 6);
	bool primary;
	if (modes == 6) primary = (uint8_t)(rngWander >> 8) > 0x80;
	else primary = modes == 2;
	if (primary) { if (def.Bullet(0) == 0) return; AttackApply(slot, def, false); }
	else { if (def.Bullet(1) == 0) return; AttackApply(slot, def, true); }
}

// aim_enemy_at_player (EA:490-560).
void ActorWorld::ActorAim(int slot, uint32_t limit)
{
	ActorSlot &a = actors[slot];
	rngWander = rngWander * 0x5e5u + 0x29u;
	a.bits = (uint8_t)((a.bits & 0xef) | (rngWander & 0x10));

	Object *obj = Find(a.object);
	if (obj == nullptr) return;
	const uint32_t br = ActorBearing(player.x, obj->y, player.y, obj->x);
	const uint16_t want = (uint16_t)(br << 7);
	const uint8_t st = a.state;
	const uint16_t facing = (uint16_t)(a.settle | ((uint16_t)obj->rotation << 8));
	int32_t err = (int16_t)(uint16_t)(want - facing);
	const int32_t lim = (int16_t)(uint16_t)limit;
	if (!(err < lim)) err = lim;
	if (!(err > -lim)) err = -lim;
	uint16_t turn = (uint16_t)err;

	bool apply = false;
	const ActorDef *def = CreatureDef(a.creature);
	if (mHost->Health() <= 0) { a.state &= 0xfb; return; }            // the player is dead (EA:513)
	if (err > 2 || err < -2 || def == nullptr) apply = true;
	else if (st & 0x10)                                      // in contact
	{
		if ((a.move & 2) && def->Bullet(1) == 0) { a.move |= 4; apply = true; }
		else
		{
			const uint8_t c = (uint8_t)(a.attackCount + 1);
			a.attackCount = c;
			if (c < def->AttackRate())
			{
				// EA:532-536: the original adds (error high byte, counter)
				// here, not the clamped error.
				turn = (uint16_t)((turn & 0xff00u) | c);
				apply = true;
			}
			else
			{
				a.attackCount = 0;
				if (!(st & 8)) BeginAttack(slot);
				a.state &= 0xfb;
				return;
			}
		}
	}
	else
	{
		if (st & 8) { a.state &= 0xfb; return; }
		if (!(a.bits & 6)) return;
		if ((uint8_t)rngWander > 0x0c) return;               // 13 chances in 256 a tick
		if ((a.move & 2) && def->Bullet(1) == 0) return;
		const uint8_t c = (uint8_t)(a.attackCount + 1);
		a.attackCount = c;
		if (c < def->AttackRate()) return;
		a.attackCount = 0;
		LaunchAttack(slot);
		a.state &= 0xfb;
		return;
	}
	if (apply)
	{
		const uint16_t nf = (uint16_t)(facing + turn);
		a.settle = (uint8_t)nf;
		const uint8_t rot = (uint8_t)(nf >> 8);
		if (rot != obj->rotation) { obj->rotation = rot; mHost->ObjectChanged(a.object); }
	}
}

//==========================================================================
//
// Moving a creature: the horizontal step with its steering (EA:216-335 and
// EA:105-215), then gravity and height (EA:357-470).
//
//==========================================================================

void ActorWorld::ActorMove(int slot, int32_t dx, int32_t dy)
{
	ActorSlot &a = actors[slot];
	Object *obj = Find(a.object);
	if (obj == nullptr) return;
	MoveBlock &mb = moveBlock;

	const uint32_t z0 = (uint32_t)(uint16_t)obj->z << 16;
	const uint32_t z = z0 | a.fracZ;
	mb.z = z;
	const uint32_t x = ((uint32_t)(uint16_t)obj->x << 16) | a.fracX;
	const uint32_t y = ((uint32_t)(uint16_t)obj->y << 16) | a.fracY;

	CollideQuery q;
	q.self = a.object;
	q.fromX = x; q.fromY = y; q.fromZ = z;
	q.toX = x + (uint32_t)dx; q.toY = y + (uint32_t)dy;
	q.sector = a.sector;
	q.stepHeight = (a.move & 2) ? 0x260 : 0x80;
	q.height = (uint16_t)(mTables->DimHeight(ObjId(*obj)) - 0x10);
	q.radius = mTables->DimRadius(ObjId(*obj));
	const int32_t sx = dx >> 10, sy = dy >> 10;
	q.budget = (uint32_t)(sx * sx) + (uint32_t)(sy * sy);
	if (!(q.budget > 0x100000u)) q.budget = 0x100000u;
	mb.height = q.height;

	const CollideAnswer r = mCollision->Collide(q);
	uint32_t qx = r.x, qy = r.y, qz = r.z;
	int sector = r.located ? r.sector : a.sector;

	if (r.hit != 0)                                          // EA:128-169
	{
		const int32_t ddx = ((int32_t)r.backX - (int32_t)qx) >> 10;
		const int32_t ddy = ((int32_t)r.backY - (int32_t)qy) >> 10;
		const uint32_t dist4 = ((uint32_t)(ddx * ddx) + (uint32_t)(ddy * ddy)) << 2;
		if (dist4 <= q.budget)
		{
			bool nudge;
			if ((dist4 << 2) < q.budget) { a.bits |= 0x20; nudge = true; }      // barely moved
			else if (!(a.bits & 0x20))
			{
				// Turn along the way it was pushed (EA:147-153).
				uint32_t b = ActorBearing((int16_t)(r.backX >> 16), (int16_t)(qy >> 16),
					(int16_t)(r.backY >> 16), (int16_t)(qx >> 16));
				b = (b & 0xffff00ffu) | ((b + 0x100u) & 0x0000ff00u);
				obj->rotation = (uint8_t)((int32_t)b >> 1);
				nudge = false;
			}
			else nudge = true;
			if (nudge)
			{
				obj->rotation = (uint8_t)(obj->rotation + ((a.bits & 0x10) ? 0xfd : 3));
				a.counter = (uint16_t)(a.counter + 2);
				a.bits |= 0x08 | 0x40;
				if (a.counter >= 0x100) { a.counter = 0; a.bits &= 0xf7; }
			}
		}
	}
	// EA:172-186: confirm the sector the resolved point is in; a point in no
	// sector goes back to where it started.
	bool confirmed = false;
	if (sector >= 0)
	{
		const int ns = LocateSector((int16_t)(qx >> 16), (int16_t)(qy >> 16), (int16_t)(qz >> 16), sector);
		if (ns >= 0) { sector = ns; confirmed = true; }
	}
	if (!confirmed)
	{
		if (a.sector >= 0) { qx = x; qy = y; }
		sector = a.sector;
	}

	// The writeback (EA:270-300).
	mb.x = qx; a.fracX = (uint16_t)qx; obj->x = (int16_t)(qx >> 16);
	mb.y = qy; a.fracY = (uint16_t)qy; obj->y = (int16_t)(qy >> 16);
	if ((r.floorZ >> 8) != 0x80)
	{
		mb.standZ = r.floorZ;
		mb.targetZ = (uint32_t)r.floorZ << 16;
		a.fracZ = 0;
		obj->z = (int16_t)r.floorZ;
		mb.ceil = r.ceilZ;
		// No room to stand: 10 off its health a tick, and at nothing left it
		// dies a critical death (EA:292-299).
		const uint16_t t = (uint16_t)(r.floorZ + mb.height - 0x18 - r.ceilZ);
		if (!(t & 0x8000u))
		{
			a.health -= 10;
			if (a.health < 0) { a.health = 0x2ee0; a.bits |= 1; a.state = 1; }
		}
	}
	mb.sector = sector;
	if (sector >= 0)
	{
		// EA:303-320: the floor it is over -- the top of the sector's middle
		// platform when the creature is above it, else the sector's own floor.
		const Sector &s = mMap->sectors[sector];
		uint16_t floor = (uint16_t)s.floorHeight;
		if (s.platformIndex >= 0 && (size_t)s.platformIndex < mMap->platforms.size())
		{
			const MidPlatform &p = mMap->platforms[s.platformIndex];
			const uint16_t top = (uint16_t)p.topZ;
			if (top > (uint16_t)p.undersideZ && (uint16_t)(top - 0x20) < mb.standZ) floor = top;
		}
		mb.floor = floor;
	}
	mb.hit = r.hit;
}

void ActorWorld::ActorVertical(int slot, int32_t dx, int32_t dy)
{
	ActorMove(slot, dx, dy);
	ActorSlot &a = actors[slot];
	Object *obj = Find(a.object);
	if (obj == nullptr) return;
	const MoveBlock &mb = moveBlock;
	const int16_t floorw = (int16_t)mb.floor;

	if ((a.move & 4) || mTables->CreatureWord(a.creature, 0x04) == 0)
	{
		// Bound to the ground (EA:361-401).
		const uint32_t h = mb.z, tgt = mb.targetZ;
		if (h == tgt) { a.move &= 0xf9; a.fallSpeed = 4; }
		else if ((int32_t)h > (int32_t)tgt)
		{
			uint32_t vel = a.fallSpeed;
			if (vel & 0x8000u) { vel = 4; a.fallSpeed = 4; }
			const uint32_t n = h - (vel << 16);
			if ((int16_t)(uint16_t)(n >> 16) > floorw) { obj->z = (int16_t)(n >> 16); a.fracZ = (uint16_t)n; }
			else { obj->z = floorw; a.fracZ = 0; }
			a.fallSpeed = (uint16_t)(a.fallSpeed + 2);
		}
		else
		{
			const uint32_t n = h + 0x80000u;
			if ((int16_t)(uint16_t)(n >> 16) < floorw)
			{
				obj->z = (int16_t)(n >> 16); a.fracZ = (uint16_t)n;
				a.fallSpeed = 0xffff;
			}
			else
			{
				obj->z = floorw; a.fracZ = 0;
				a.move &= 0xf9; a.fallSpeed = 4;
			}
		}
	}
	else
	{
		// A flier (EA:403-447).
		a.fallSpeed = 0;
		uint32_t n = mb.z;
		bool land = false;
		if (mTables->CreatureWord(a.creature, 0x14) == 0) land = true;
		else
		{
			const uint16_t ihi = (uint16_t)(mb.z >> 16);
			const int16_t ceilw = (int16_t)(mb.ceil - 0x20);
			if ((int16_t)ihi < floorw || (int16_t)ihi > ceilw)
			{
				const uint16_t bound = ((int16_t)ihi < floorw) ? (uint16_t)floorw : (uint16_t)ceilw;
				int32_t d = (int32_t)((uint32_t)bound << 16) - (int32_t)n;
				if (!(d < 0x80000)) d = 0x80000;
				if (!(d > -0x80000)) d = -0x80000;
				n += (uint32_t)d;
				a.move |= 2;
				a.fracZ = (uint16_t)n;
				obj->z = (int16_t)(n >> 16);
				a.fallSpeed = 0xffff;
			}
			else if (ihi == (uint16_t)(mb.targetZ >> 16)) { a.move &= 0xfd; a.fallSpeed = 4; }   // the word at +0x0a
			else if (a.bits & 0x28) land = true;
			else if (ihi == (uint16_t)((uint16_t)player.z + 0x20)) land = true;
			else
			{
				// Toward the player's height, at most 2 a tick.
				int32_t d = (int32_t)n - (int32_t)(((uint32_t)(uint16_t)player.z << 16) | player.zFrac);
				if (!(d < 0x20000)) d = 0x20000;
				if (!(d > -0x20000)) d = -0x20000;
				n -= (uint32_t)d;
				land = true;
			}
		}
		if (land)
		{
			a.move |= 2;
			a.fracZ = 0;
			obj->z = (int16_t)(n >> 16);
		}
	}

	// The record follows the creature into the sector it is now in
	// (EA:455-466).
	if (mb.sector != a.sector)
	{
		a.sector = mb.sector;
		if (mb.sector >= 0) MoveObjectToSector(a.object, mb.sector);
	}
	mHost->ObjectChanged(a.object);
}

void ActorWorld::AnimationDrawn(ObjectUid uid, uint32_t picture, uint16_t ticksPerFrame, uint16_t frameCount)
{
	const Object *obj = Find(uid);
	if (obj == nullptr || obj->unk0C == 0) return;
	uint16_t *counter = nullptr;
	if (obj->renderType & 1)                                 // render_world.c:416-420
	{
		ActorSlot &a = actors[(obj->unk0C - 1) / 0x22];
		if (!(a.state & 0x6c)) return;
		counter = &a.counter;
	}
	else if (obj->renderType & 2)                            // render_world.c:421-425
	{
		FlyingSlot &f = flying[(obj->unk0C - 1) / 0x1c];
		if (!(f.mode & 2)) return;
		counter = &f.owner;                                  // +0x1a, the impact countdown
	}
	else return;

	uint32_t frame = *counter;
	if (ticksPerFrame > 1) frame /= ticksPerFrame;
	uint16_t *last = nullptr;
	for (auto &e : mPictureLast) if (e.first == picture) { last = &e.second; break; }
	if (last == nullptr) { mPictureLast.push_back(std::make_pair(picture, (uint16_t)0xffff)); last = &mPictureLast.back().second; }
	uint16_t shown = (uint16_t)frame;
	if (*last != (uint16_t)frame)
	{
		*last = (uint16_t)frame;
		if ((int16_t)shown >= (int16_t)frameCount) shown = (uint16_t)(frameCount - 1);
	}
	if ((uint8_t)shown != (uint8_t)frame) *counter = 0xffff;
}

// add_secondary_state_record (RC:3849-3876): out of one list, onto the end of
// the other. The uid, and so the engine's actor, goes with it.
void ActorWorld::MoveObjectToSector(ObjectUid uid, int newSector)
{
	int sector = -1, index = -1;
	Object *obj = Find(uid, &sector, &index);
	if (obj == nullptr || newSector < 0 || (size_t)newSector >= mMap->objects.size()) return;
	if (sector == newSector)
	{
		// The original still removes and re-appends, which moves the record
		// to the end of its own list.
		if ((size_t)index + 1 == mMap->objects[sector].size()) return;
	}
	const Object copy = *obj;
	RemoveObject(sector, index);
	mMap->objects[newSector].push_back(copy);
	mUids[newSector].push_back(uid);
	mHost->ListsChanged();
}

} // namespace roth
