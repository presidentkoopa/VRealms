//
// The player's health: every way it goes down and the one way it goes up.
// Part of ActorWorld (roth_actors.h); engine-free.
//
// Sources: ROTH.C weapon_combat.c (WC), renderer.c (R), raw_commands.c (RC),
// player.c (PL), game_core.c (GC), dbase100.c (DB). docs/GAME_combat.md
// section 3 describes the same rules and every one of them was re-read
// against the code; the two agree.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_actors.h"
#include "roth_raw.h"
#include "roth_dbase100.h"

namespace roth
{

// GC:766-772: build_entity_def_by_id(&block, Adam's item, 2), then a zero
// maximum becomes 0x800. The statistics are built from the ITEM (by index),
// by way of its +2 word through the same builder the creatures use.
int32_t ActorWorld::AdamMaxHealth(uint32_t adamItem) const
{
	int32_t m = 0;
	if (mDb != nullptr && mDb->IsLoaded() && adamItem != 0)
	{
		const DbItem *it = mDb->ItemSlot((int)adamItem);
		if (it != nullptr && (size_t)it->offset + 4 <= mDb->Size())
		{
			const uint8_t *rec = mDb->Image() + it->offset;
			const ActorDef def = BuildDef((uint32_t)(uint16_t)(rec[2] | (rec[3] << 8)));
			if (def.valid) m = (int32_t)def.Health();
		}
	}
	return m != 0 ? m : 0x800;
}

// WC:29-43.
void ActorWorld::DamageDirect(uint32_t damage)
{
	damagedThisFrame = true;
	const int32_t f = (int32_t)(damage + 0x20);
	int32_t flash = mHost->PainFlash();
	if (f < 0x35c6 && f >= flash)
	{
		flash += (int32_t)((uint32_t)f >> 1);
		if ((uint32_t)flash >= 0x164) flash = 0x164;
		mHost->SetPainFlash(flash);
	}
	int32_t h = (int32_t)((uint32_t)mHost->Health() - damage);
	if (h < 0) h = 0;
	mHost->SetHealth(h);
}

// WC:50-61.
void ActorWorld::DamageReduced(uint32_t damage)
{
	if (damage >= 0x35c6u) { DamageDirect(damage); return; }
	const uint32_t cut = (uint32_t)((int32_t)damage * mHost->Armour()) >> 11;
	damage -= cut;
	if ((int32_t)damage > 0) DamageDirect(damage);
}

// R:9999-10026.
void ActorWorld::DamageScripted(uint32_t damage, bool perTick, uint32_t falloff, uint32_t ticksThisFrame)
{
	if (damage == 0)
	{
		damage = (uint32_t)mHost->Health();
		falloff = 0;
	}
	else
	{
		if (damage > 0xc8) damage = (damage - 0xc7) << 7;
		if (perTick) damage = (uint32_t)((int32_t)damage * (int32_t)ticksThisFrame);
	}
	if (falloff != 0)
	{
		const uint32_t cut = (uint32_t)((int32_t)falloff * (int32_t)damage) >> 8;
		damage -= cut;
		if ((int32_t)damage <= 0) return;
	}
	DamageDirect(damage);
}

// GC:334-340.
void ActorWorld::DecayPainFlash(uint32_t ticksThisFrame)
{
	int32_t flash = mHost->PainFlash();
	if (flash == 0) return;
	flash -= (int32_t)(ticksThisFrame << 2);
	if (flash < 0) flash = 0;
	mHost->SetPainFlash(flash);
}

// PL:286-291.
uint32_t ActorWorld::FallDamage(int32_t accumulator)
{
	if (accumulator <= 0x16) return 0;
	int32_t d = accumulator - 0x16;
	d *= d;
	if (d > 0x1f40) d = 0x1f40;
	return (uint32_t)d;
}

// PL:274-281 run forward: the accumulator starts at 0 (PL:303-307 leaves it
// there when the last landing has settled), each falling tick moves
// min(left, acc) and then adds one, and the landing branch is the first tick
// that finds nothing left.
int32_t ActorWorld::FallAccumulatorForDrop(int32_t height)
{
	int32_t acc = 0, left = height;
	while (left > 0)
	{
		int32_t step = left;
		if (step > acc) step = acc;
		left -= step;
		acc++;
	}
	return acc;
}

// point_segment_distance_sq (RC:123-148) with the query point at the player
// (RC:4226-4236).
uint32_t ActorWorld::PlayerToFaceDistanceSq(int face, uint16_t radius) const
{
	if (mMap == nullptr || face < 0 || (size_t)face >= mMap->faces.size()) return 0x7ffff;
	const Face &f = mMap->faces[face];
	if (f.vertex1 < 0 || f.vertex2 < 0) return 0x7ffff;
	const Vertex &A = mMap->vertices[f.vertex1], &B = mMap->vertices[f.vertex2];
	const int16_t px = player.x, py = player.y;
	const int16_t nr = (int16_t)(uint16_t)(0u - (uint32_t)radius), pr = (int16_t)radius;
	{
		const int16_t da = (int16_t)(B.x - px), db = (int16_t)(A.x - px);
		const int16_t mx = da > db ? da : db, mn = da > db ? db : da;
		if (mx <= nr || mn >= pr) return 0x7ffff;
	}
	{
		const int16_t da = (int16_t)(B.y - py), db = (int16_t)(A.y - py);
		const int16_t mx = da > db ? da : db, mn = da > db ? db : da;
		if (mx <= nr || mn >= pr) return 0x7ffff;
	}
	const int32_t segDY = (int16_t)(B.y - A.y), segDX = (int16_t)(B.x - A.x);
	const int32_t seg2 = segDY * segDY + segDX * segDX;
	// The original divides unguarded; a zero-length face would fault there.
	if (seg2 == 0) return 0x7ffff;
	const int32_t cross = (int32_t)(int16_t)(px - B.x) * segDY - (int32_t)(int16_t)(py - B.y) * segDX;
	const int32_t dist2 = (int32_t)(((int64_t)cross * cross) / seg2);
	if ((uint32_t)dist2 > (uint32_t)radius * (uint32_t)radius) return 0x7ffff;
	return (uint32_t)(dist2 + 1);
}

// gather_faces_by_id for a non-zero id (RC:88 -> R:11222) followed by
// find_face_record (RC:64) on each match, which is what both users below do:
// the extended mapping records whose face id is the key, in record order, and
// for each the FIRST face that points at it. A record no face points at
// resolves to offset 0 in the original, which is not a face; it is dropped.
static std::vector<int> FacesForKey(const Map &map, uint16_t key)
{
	std::vector<int> out;
	for (size_t t = 0; t < map.textureMaps.size() && out.size() < 0xc8 - 2; t++)
	{
		if (!map.textureMaps[t].extended || map.textureMaps[t].faceID != key) continue;
		for (size_t f = 0; f < map.faces.size(); f++)
			if (map.faces[f].textureMap == (int)t) { out.push_back((int)f); break; }
	}
	return out;
}

// RC:4245-4275.
int ActorWorld::CmdApplyDamage(Command &rec, int activeFace, bool perTick, uint32_t ticksThisFrame)
{
	const uint16_t radius = rec.Word(0x0a);
	uint32_t falloff = 0;
	bool hurt = true;
	if (radius != 0)
	{
		const uint32_t sq2r = (uint32_t)(radius * 2) * (uint32_t)(radius * 2);
		const Object *src = nullptr;
		Object synthetic = Object();
		if (mSourceSynthetic)
		{
			// The record at the player's feet (RC:4114): distance 0.
			synthetic.x = player.x; synthetic.y = player.y;
			src = &synthetic;
		}
		else if (mSource != 0) src = Find(mSource);
		if (src != nullptr)
		{
			const int16_t dx = (int16_t)((uint16_t)src->x - (uint16_t)player.x);
			const int16_t dy = (int16_t)((uint16_t)src->y - (uint16_t)player.y);
			const uint32_t d2 = (uint32_t)((int32_t)dx * dx + (int32_t)dy * dy);
			if (d2 >= sq2r) hurt = false;
			else falloff = (uint32_t)(((uint64_t)0x100u * d2) / sq2r);
		}
		else if (activeFace < 0) hurt = false;
		else
		{
			const uint32_t d2 = PlayerToFaceDistanceSq(activeFace, (uint16_t)(radius * 2 + 0x1c));
			if (d2 > 0x7ff00u || d2 >= sq2r) hurt = false;
			else falloff = (uint32_t)(((uint64_t)0x100u * d2) / sq2r);
		}
	}
	if (hurt) DamageScripted(rec.Byte(7), perTick, falloff, ticksThisFrame);
	if (rec.Byte(6) & 0x10) { rec.modifier |= 0x08; rec.disabled = true; }
	return ACTCMD_ACTED;
}

// RC:4282-4297. Run from a chain, an armed record does nothing: its work is
// the per-frame emitter.
int ActorWorld::CmdFaceEmitsDamage(Command &rec, int activeFace, uint32_t ticksThisFrame)
{
	if (rec.modifier & 0x21) return ACTCMD_NOTHING;
	std::vector<int> faces;
	if (rec.key == 0) { if (activeFace >= 0) faces.assign(1, activeFace); }   // RC:88-90
	else faces = FacesForKey(*mMap, rec.key);
	if (faces.empty()) return ACTCMD_NOTHING;
	const uint16_t reach = (uint16_t)(rec.Word(0x0a) * 2 + 0x1c);
	for (int f : faces)
		if (PlayerToFaceDistanceSq(f, reach) <= 0x7ff00u)
		{
			DamageScripted(rec.Byte(7), false, 0, ticksThisFrame);
			if (rec.Byte(6) & 0x10) { rec.modifier |= 0x08; rec.disabled = true; }
			return ACTCMD_ACTED;
		}
	return ACTCMD_ACTED;
}

// damage_player_from_emitter (WC:72-106), called once a frame while any
// emitter exists (RC:3297-3298). The walk stops at the FIRST emitter that
// hurts, so two overlapping hazards do not add.
void ActorWorld::TickDamageEmitters(uint32_t ticksThisFrame)
{
	if (mMap == nullptr) return;
	for (int ci : mEmitters)
	{
		Command &rec = mMap->commands[(size_t)ci];
		if (rec.modifier & 0x08) continue;
		const uint16_t reach = (uint16_t)(rec.Word(0x0a) * 2 + 0x1c);
		for (int f : FacesForKey(*mMap, rec.key))
		{
			if (PlayerToFaceDistanceSq(f, reach) > 0x7ff00u) continue;
			DamageScripted(rec.Byte(7), true, 0, ticksThisFrame);
			if (rec.Byte(6) & 0x10) { rec.modifier |= 0x08; rec.disabled = true; }
			return;
		}
	}
}

} // namespace roth
