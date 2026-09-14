/*
** level_solid_query.cpp
**
** RS FORK -- WHAT IS SOLID NEAR A POINT, READ BY THE RENDERER. The level walk; see
** level_solid_query.h for what the regions are, their geometry and the read-only contract.
**
**---------------------------------------------------------------------------
*/

#include <algorithm>
#include <cmath>
#include "level_solid_query.h"
#include "g_levellocals.h"
#include "p_3dfloors.h"
#include "po_man.h"
#include "r_defs.h"

using namespace LevelSolid;

namespace
{

struct FQuery
{
	DVector3 center;
	double   radius = 0.;
	double   slab = 0.;
	TArray<FLevelSolidRegion> *out = nullptr;
	unsigned maxRegions = 0;
	bool     capped = false;
};

bool Room(FQuery &q)
{
	if (q.out->Size() < q.maxRegions) return true;
	q.capped = true;
	return false;
}

double PlaneMaxAt(const secplane_t &pl, const line_t *ld)
{
	return std::max(pl.ZatPoint(ld->v1), pl.ZatPoint(ld->v2));
}

double PlaneMinAt(const secplane_t &pl, const line_t *ld)
{
	return std::min(pl.ZatPoint(ld->v1), pl.ZatPoint(ld->v2));
}

// The faces of sector `sec` along the line, on its front or back side: below the floor, above the
// ceiling, and each solid 3D floor. A face whose planes lie wholly out of the query's height is skipped.
void AddSectorFaces(FQuery &q, const line_t *ld, const FLineGeom &g, bool frontSide, sector_t *sec)
{
	if (sec == nullptr) return;
	const double zlo = q.center.Z - q.radius, zhi = q.center.Z + q.radius;
	FLevelSolidRegion r;

	if (PlaneMaxAt(sec->floorplane, ld) + q.slab >= zlo && Room(q)
		&& FaceBeyondPlane(r, g, frontSide, q.slab, sec->floorplane.Normal(), sec->floorplane.fD(), true))
		q.out->Push(r);

	if (PlaneMinAt(sec->ceilingplane, ld) - q.slab <= zhi && Room(q)
		&& FaceBeyondPlane(r, g, frontSide, q.slab, sec->ceilingplane.Normal(), sec->ceilingplane.fD(), false))
		q.out->Push(r);

	if (sec->e == nullptr) return;
	for (F3DFloor *ff : sec->e->XFloor.ffloors)
	{
		if (ff == nullptr || (ff->flags & (FF_EXISTS | FF_SOLID)) != (FF_EXISTS | FF_SOLID)) continue;
		if (ff->top.plane == nullptr || ff->bottom.plane == nullptr) continue;
		if (PlaneMaxAt(*ff->top.plane, ld) + q.slab < zlo || PlaneMinAt(*ff->bottom.plane, ld) - q.slab > zhi) continue;
		if (Room(q) && FaceBetweenPlanes(r, g, frontSide, q.slab, ff->top.plane->Normal(), ff->top.plane->fD(),
			ff->bottom.plane->Normal(), ff->bottom.plane->fD()))
			q.out->Push(r);
	}
}

void ConsiderLine(FQuery &q, const line_t *ld, bool polyobject)
{
	if (ld == nullptr || ld->frontsector == nullptr || ld->isLinePortal()) return;
	FLineGeom g;
	if (!g.Set(DVector2(ld->v1->fX(), ld->v1->fY()), ld->Delta())) return;

	// Every region lies within the slab of its line, or behind a one-sided line and then at least as far
	// from the query centre as the line is.
	const double dist = g.SegmentDistance(DVector2(q.center.X, q.center.Y));
	if (dist > q.radius + q.slab) return;

	if (polyobject)
	{
		if (Room(q)) q.out->Push(SlabBehind(g, q.slab));
		return;
	}

	if (ld->backsector == nullptr)
	{
		if (dist <= q.radius && Room(q)) q.out->Push(VoidBehind(g));
		AddSectorFaces(q, ld, g, true, ld->frontsector);
		return;
	}

	AddSectorFaces(q, ld, g, true, ld->frontsector);
	AddSectorFaces(q, ld, g, false, ld->backsector);
}

bool Seen(TArray<const line_t *> &seen, const line_t *ld)
{
	for (const line_t *s : seen) if (s == ld) return true;
	seen.Push(ld);
	return false;
}

} // namespace

bool LevelSolidQuery(FLevelLocals *Level, const DVector3 &center, double radius, double slabDepth,
	TArray<FLevelSolidRegion> &out, unsigned maxRegions)
{
	out.Clear();
	if (Level == nullptr || !(radius > 0.) || !std::isfinite(radius) || !std::isfinite(center.X) || !std::isfinite(center.Y) || !std::isfinite(center.Z))
		return true;
	FBlockmap &bm = Level->blockmap;
	if (bm.bmapwidth <= 0 || bm.bmapheight <= 0) return true;

	FQuery q;
	q.center = center;
	q.radius = radius;
	q.slab = std::max(0., std::isfinite(slabDepth) ? slabDepth : 0.);
	q.out = &out;
	q.maxRegions = maxRegions;

	const double reach = radius + q.slab;
	const int x0 = std::max(bm.GetBlockX(center.X - reach), 0), x1 = std::min(bm.GetBlockX(center.X + reach), bm.bmapwidth - 1);
	const int y0 = std::max(bm.GetBlockY(center.Y - reach), 0), y1 = std::min(bm.GetBlockY(center.Y + reach), bm.bmapheight - 1);

	static thread_local TArray<const line_t *> seen;
	seen.Clear();

	// Polyobject lines first, in every cell, so a polyobject line is always read as one whichever cell
	// reaches it first.
	for (int y = y0; y <= y1; y++)
	{
		for (int x = x0; x <= x1; x++)
		{
			if (!bm.isValidBlock(x, y)) continue;
			const unsigned offset = unsigned(y) * unsigned(bm.bmapwidth) + unsigned(x);
			if (offset >= Level->PolyBlockMap.Size()) continue;
			for (polyblock_t *pb = Level->PolyBlockMap[offset]; pb != nullptr; pb = pb->next)
			{
				if (pb->polyobj == nullptr) continue;
				for (line_t *ld : pb->polyobj->Linedefs)
					if (!Seen(seen, ld)) ConsiderLine(q, ld, true);
			}
		}
	}
	for (int y = y0; y <= y1; y++)
	{
		for (int x = x0; x <= x1; x++)
		{
			if (!bm.isValidBlock(x, y)) continue;
			const int *list = bm.GetLines(x, y);
			if (list == nullptr) continue;
			for (; *list != -1; list++)
			{
				if (*list < 0 || (unsigned)*list >= Level->lines.Size()) continue;
				const line_t *ld = &Level->lines[*list];
				if (!Seen(seen, ld)) ConsiderLine(q, ld, false);
			}
		}
	}
	return !q.capped;
}
