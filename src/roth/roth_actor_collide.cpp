//
// SimpleCollision: the collision answers the actor rules need, worked out
// from the Realms map itself (roth_actors.h). Engine-free.
//
// What it does and what the original does (collision_physics.c, CP):
//
//   locating a point      the same: ActorWorld::LocateSector is CP:906-1028.
//   a creature's step     the original slides the body's circle along walls
//                         (clip_query_circle_to_edge, CP:210) and pushes it
//                         off objects (CP:1213); this STOPS the step when the
//                         circle at the destination overlaps a wall it may not
//                         cross. The rules then steer exactly as the original
//                         does on a hit (EA:128-169), so a creature still turns
//                         away from walls -- but a glancing step that the
//                         original would slide along, this refuses.
//   a bullet's tick       the original's ray sweep (CP:1556) in 1/2/4/8 sub
//                         steps; the same sub steps here, each one stopping at
//                         a sector the point leaves through a wall, at the
//                         floor or ceiling (half the width, WC:117), at the
//                         player (CP:43-72, exact), or at an actor or a
//                         shootable object (the Chebyshev path of CP:1083-1170).
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

// point_segment_distance_sq (RC:123-148) without the radius reject.
int64_t SegDist2(const Vertex &A, const Vertex &B, int32_t px, int32_t py)
{
	const int64_t dx = B.x - A.x, dy = B.y - A.y;
	const int64_t l2 = dx * dx + dy * dy;
	if (l2 == 0) return (int64_t)(px - A.x) * (px - A.x) + (int64_t)(py - A.y) * (py - A.y);
	int64_t t = ((px - A.x) * dx + (py - A.y) * dy);
	if (t < 0) t = 0;
	if (t > l2) t = l2;
	const double cx = A.x + (double)dx * t / l2, cy = A.y + (double)dy * t / l2;
	const double ex = px - cx, ey = py - cy;
	return (int64_t)(ex * ex + ey * ey);
}
}

// Does a body of this radius and height, standing at (x,y,z) in `sector`,
// overlap a face it may not cross? One-sided faces always block; a face
// into another sector blocks when the step up is more than `step` or the
// opening is lower than the body.
bool SimpleCollision::Blocked(int sector, int16_t x, int16_t y, int16_t z, uint16_t radius, uint16_t height, uint16_t step) const
{
	const Map &m = *mWorld->MapPtr();
	if (sector < 0 || (size_t)sector >= m.sectors.size()) return true;
	const Sector &s = m.sectors[sector];
	const int64_t r2 = (int64_t)radius * radius;
	for (int k = 0; k < s.faceCount; k++)
	{
		const int fi = s.firstFaceIndex + k;
		if (fi < 0 || (size_t)fi >= m.faces.size()) break;
		const Face &f = m.faces[fi];
		if (f.vertex1 < 0 || f.vertex2 < 0) continue;
		if (SegDist2(m.vertices[f.vertex1], m.vertices[f.vertex2], x, y) >= r2) continue;
		if (f.sister < 0) return true;
		const int ns = m.faces[f.sister].sector;
		if (ns < 0) return true;
		const Sector &o = m.sectors[ns];
		if (o.floorHeight - z > (int)step) return true;
		const int top = o.ceilingHeight < s.ceilingHeight ? o.ceilingHeight : s.ceilingHeight;
		const int bot = o.floorHeight > z ? o.floorHeight : z;
		if (top - bot < (int)height) return true;
	}
	return false;
}

CollideAnswer SimpleCollision::Collide(const CollideQuery &q)
{
	CollideAnswer a;
	a.x = q.fromX; a.y = q.fromY; a.z = q.fromZ;
	a.backX = q.fromX; a.backY = q.fromY;
	const int16_t tx = (int16_t)(q.toX >> 16), ty = (int16_t)(q.toY >> 16), z = (int16_t)(q.fromZ >> 16);
	const int sec = mWorld->LocateSector(tx, ty, z, q.sector);
	if (sec < 0) { a.sector = q.sector; return a; }         // CP:1468-1478: no sector, no collision pass
	a.located = true;
	if (Blocked(sec, tx, ty, z, q.radius, q.height, q.stepHeight))
	{
		a.hit = 1;
		a.sector = q.sector;
	}
	else
	{
		a.x = q.toX; a.y = q.toY;
		a.sector = sec;
	}
	const Sector &s = mWorld->MapPtr()->sectors[a.sector >= 0 ? a.sector : sec];
	a.floorZ = (uint16_t)s.floorHeight;
	a.ceilZ = (uint16_t)s.ceilingHeight;
	return a;
}

SweepAnswer SimpleCollision::Sweep(const SweepQuery &q)
{
	SweepAnswer a;
	a.x = q.x; a.y = q.y; a.z = q.z; a.sector = q.sector;
	int n = 1;
	int32_t vx = q.velX, vy = q.velY, vz = q.velZ;
	if (q.speed >= 0x0c) { n = 2; vx >>= 1; vy >>= 1; vz >>= 1; }
	if (q.speed >= 0x18) { n = 4; vx >>= 1; vy >>= 1; vz >>= 1; }
	if (q.speed >= 0x30) { n = 8; vx >>= 1; vy >>= 1; vz >>= 1; }
	const Map &m = *mWorld->MapPtr();
	const CreatureTables *t = mWorld->Tables();
	const PlayerView &pl = mWorld->player;
	for (int i = 0; i < n; i++)
	{
		const uint32_t nx = a.x + (uint32_t)vx, ny = a.y + (uint32_t)vy, nz = a.z + (uint32_t)vz;
		const int16_t x = (int16_t)(nx >> 16), y = (int16_t)(ny >> 16), z = (int16_t)(nz >> 16);
		const int sec = mWorld->LocateSector(x, y, z, a.sector);
		if (sec < 0 || Blocked(sec, x, y, z, (uint16_t)q.width, 1, 0x7fff))
		{ a.hit = 1; return a; }      // stopped short of the wall: the last clear sub step
		const Sector &s = m.sectors[sec];
		const int half = (int)(q.width >> 1);
		if (s.floorHeight + half > z || s.ceilingHeight - half < z) { a.hit = 1; return a; }
		// A hit puts the query back where this sub step began (CP:1429-1432,
		// restore_x being re-saved per sub step by probe_collision_step).
		const SweepAnswer before = a;
		a.x = nx; a.y = ny; a.z = nz; a.sector = sec;
		const uint16_t reach = (uint16_t)(q.width + 0x20);
		if (q.owner != 0)                                    // CP:43-72
		{
			uint16_t dx = (uint16_t)(pl.x - x); if ((int16_t)dx < 0) dx = (uint16_t)(0 - dx);
			uint16_t dy = (uint16_t)(pl.y - y); if ((int16_t)dy < 0) dy = (uint16_t)(0 - dy);
			const uint16_t d = (int16_t)dy > (int16_t)dx ? dy : dx;
			if (d <= reach && !(pl.z > z) && !((int16_t)(pl.z + pl.height + 0xa) < z))
			{
				a = before;
				a.hit = 2; a.hitObject = 0; return a;
			}
		}
		// Actors, and (for the player's own shots) objects with a shot trigger.
		auto test = [&](ObjectUid uid) -> bool
		{
			if (uid == 0 || uid == q.self) return false;
			const Object *o = mWorld->Find(uid);
			if (o == nullptr || (o->flags & 0x80)) return false;
			if ((o->renderType & 1) && q.owner != 0 && o->unk0C == q.owner) return false;   // CP:1148
			int16_t dx = (int16_t)(x - o->x); if (dx < 0) dx = (int16_t)-dx;
			int16_t dy = (int16_t)(y - o->y); if (dy < 0) dy = (int16_t)-dy;
			const int16_t cheb = dy > dx ? dy : dx;
			const uint16_t id = ObjId(*o);
			if ((uint16_t)cheb >= (uint16_t)(t ? t->DimRadius(id) : 0)) return false;   // CP:1123-1125
			const int16_t h = (int16_t)(t ? t->DimHeight(id) : 0);
			if (z < o->z || z > o->z + h) return false;
			return true;
		};
		for (const ActorSlot &s2 : mWorld->actors)
			if (s2.object != 0 && test(s2.object))
			{ a = before; a.hit = 2; a.hitObject = s2.object; return a; }
		if (q.owner == 0)
			for (size_t si = 0; si < m.objects.size(); si++)
				for (size_t oi = 0; oi < m.objects[si].size(); oi++)
					if ((m.objects[si][oi].renderType & 0x21) == 0x20)
					{
						const ObjectUid uid = mWorld->UidAt((int)si, (int)oi);
						if (test(uid))
						{
							a = before;
							a.hit = 2; a.hitObject = uid;
							mWorld->Host()->RunObjectChain(uid);   // CP:1158-1162
							return a;
						}
					}
	}
	return a;
}

} // namespace roth
