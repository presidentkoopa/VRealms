/*
** hw_levelsolidity.cpp
**
** [LEVELSOLIDITY] Where the level is solid. See the header.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Mirrored line for line in Python ("Engine docs/SMOKE_13A_IMPL_NOTES.md", the SH1
** mirror) on a test map with a thin wall, a thick wall, a door and a step.
**
*/

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "hw_levelsolidity.h"
#include "g_levellocals.h"
#include "r_defs.h"
#include "p_3dfloors.h"

using LevelSolidity::Span;
using LevelSolidity::Box;
using LevelSolidity::MAX_SPANS;
using LevelSolidity::OPEN;
using LevelSolidity::SOLID;

namespace
{
	// A point within this distance (map units) of a subsector's boundary counts as
	// inside it. Covers the BSP walk's fixed-point rounding at partition lines; a void
	// thinner than this is not a wall anyone can see.
	const double INSIDE_TOLERANCE = 0.01;

	// A solid 3D floor thinner than this is widened to it, so a zero-thickness slab
	// still blocks.
	const double MIN_SLAB = 0.01;

	// Every line is moved this far to its LEFT and shortened this much at each end
	// before it is rasterised. Left is the void side of a one-sided line (its front
	// sidedef is on the right of v1 -> v2), so a wall lying exactly on a cell boundary
	// marks the void-side column only; the shortening keeps a wall that ends exactly on
	// a boundary from marking the next row (a door jamb keeps the doorway's width).
	const double LINE_NUDGE = 0.001;

	// Removes [bottom, top) from a sorted list of disjoint spans, in place, keeping it
	// sorted. When the result would exceed MAX_SPANS the highest pieces are dropped
	// (left solid: the conservative side).
	int SubtractSpan(Span* spans, int count, double bottom, double top)
	{
		Span result[MAX_SPANS];
		int n = 0;
		for (int i = 0; i < count; i++)
		{
			const Span s = spans[i];
			if (top <= s.Bottom || bottom >= s.Top)
			{
				if (n < MAX_SPANS) result[n++] = s;
				continue;
			}
			if (s.Bottom < bottom && n < MAX_SPANS) result[n++] = { s.Bottom, bottom };
			if (top < s.Top && n < MAX_SPANS) result[n++] = { top, s.Top };
		}
		for (int i = 0; i < n; i++)
			spans[i] = result[i];
		return n;
	}

	// The overlap of two sorted lists of disjoint spans, sorted.
	int IntersectSpans(const Span* a, int countA, const Span* b, int countB, Span* out)
	{
		int n = 0;
		int i = 0, j = 0;
		while (i < countA && j < countB && n < MAX_SPANS)
		{
			const double low = std::max(a[i].Bottom, b[j].Bottom);
			const double high = std::min(a[i].Top, b[j].Top);
			if (high > low) out[n++] = { low, high };
			if (a[i].Top < b[j].Top) i++;
			else j++;
		}
		return n;
	}

	// Rules the cells of one column out that are not in `spans`: a cell whose centre is
	// outside every span is SOLID, and each gap between two spans (a solid 3D floor)
	// marks the cell holding its middle SOLID, however thin, so nothing leaks through a
	// thin bridge. Never marks a cell OPEN. count 0 = the whole column solid.
	void ApplyColumnSpans(const Box& box, int cx, int cy, const Span* spans, int count, uint8_t* mask)
	{
		const size_t column = (size_t)cx + (size_t)box.SizeX * (size_t)cy;
		const size_t layer = (size_t)box.SizeX * (size_t)box.SizeY;

		for (int z = 0; z < box.SizeZ; z++)
		{
			const double centerZ = box.OriginZ + (z + 0.5) * box.CellSize;
			bool open = false;
			for (int i = 0; i < count; i++)
			{
				if (centerZ >= spans[i].Bottom && centerZ < spans[i].Top)
				{
					open = true;
					break;
				}
			}
			if (!open)
				mask[column + layer * (size_t)z] = SOLID;
		}

		for (int i = 0; i + 1 < count; i++)
		{
			const double middle = (spans[i].Top + spans[i + 1].Bottom) * 0.5;
			const int z = (int)std::floor((middle - box.OriginZ) / box.CellSize);
			if (z >= 0 && z < box.SizeZ)
				mask[column + layer * (size_t)z] = SOLID;
		}
	}

	// Every cell a segment passes through, in cell units (supercover: at an exact
	// corner both side cells too). Stops at the segment's end.
	template<class Visit>
	void TraverseCells(double u0, double v0, double u1, double v1, Visit&& visit)
	{
		const double INF = 1e300;
		int i = (int)std::floor(u0);
		int j = (int)std::floor(v0);
		const int iEnd = (int)std::floor(u1);
		const int jEnd = (int)std::floor(v1);
		const double du = u1 - u0;
		const double dv = v1 - v0;
		const int stepI = du > 0 ? 1 : (du < 0 ? -1 : 0);
		const int stepJ = dv > 0 ? 1 : (dv < 0 ? -1 : 0);
		double tMaxI = stepI > 0 ? (i + 1 - u0) / du : (stepI < 0 ? (u0 - i) / -du : INF);
		double tMaxJ = stepJ > 0 ? (j + 1 - v0) / dv : (stepJ < 0 ? (v0 - j) / -dv : INF);
		const double tDeltaI = stepI != 0 ? 1.0 / std::fabs(du) : INF;
		const double tDeltaJ = stepJ != 0 ? 1.0 / std::fabs(dv) : INF;

		visit(i, j);
		int guard = std::abs(iEnd - i) + std::abs(jEnd - j) + 4;
		while ((i != iEnd || j != jEnd) && guard-- > 0)
		{
			if (std::min(tMaxI, tMaxJ) > 1.0)
				break;
			if (tMaxI < tMaxJ - 1e-12)
			{
				i += stepI;
				tMaxI += tDeltaI;
			}
			else if (tMaxJ < tMaxI - 1e-12)
			{
				j += stepJ;
				tMaxJ += tDeltaJ;
			}
			else
			{
				// Exactly through a corner: both cells beside it, then the diagonal one.
				visit(i + stepI, j);
				visit(i, j + stepJ);
				i += stepI;
				j += stepJ;
				tMaxI += tDeltaI;
				tMaxJ += tDeltaJ;
			}
			visit(i, j);
		}
	}
}

subsector_t* LevelSolidity::SubsectorContaining(FLevelLocals* Level, double x, double y)
{
	if (Level == nullptr || Level->subsectors.Size() == 0)
		return nullptr;

	subsector_t* ss = Level->PointInRenderSubsector(DVector2(x, y));
	if (ss == nullptr || ss->firstline == nullptr || ss->numlines < 3 || (ss->flags & SSECF_DEGENERATE))
		return nullptr;

	// Which side of each seg is inside: the sign of the polygon's area (shoelace), so the
	// test holds whichever way the node builder wound the segs.
	double area2 = 0;
	for (uint32_t i = 0; i < ss->numlines; i++)
	{
		const seg_t& seg = ss->firstline[i];
		if (seg.v1 == nullptr || seg.v2 == nullptr)
			return nullptr;
		area2 += seg.v1->fX() * seg.v2->fY() - seg.v2->fX() * seg.v1->fY();
	}
	if (std::fabs(area2) < 1e-6)
		return nullptr;
	const double inside = area2 > 0 ? 1.0 : -1.0;

	for (uint32_t i = 0; i < ss->numlines; i++)
	{
		const seg_t& seg = ss->firstline[i];
		const double ax = seg.v1->fX(), ay = seg.v1->fY();
		const double ex = seg.v2->fX() - ax, ey = seg.v2->fY() - ay;
		const double length = std::sqrt(ex * ex + ey * ey);
		if (length < 1e-9)
			continue;
		// > 0: (x, y) is left of the seg. Scaled by the length, so the tolerance is a distance.
		const double cross = ex * (y - ay) - ey * (x - ax);
		if (cross * inside < -INSIDE_TOLERANCE * length)
			return nullptr;
	}
	return ss;
}

int LevelSolidity::SectorOpenSpans(const sector_t* sector, double x, double y, Span* spans)
{
	if (sector == nullptr)
		return 0;

	const double floorZ = sector->floorplane.ZatPoint(x, y);
	const double ceilingZ = sector->ceilingplane.ZatPoint(x, y);
	if (!(ceilingZ > floorZ))
		return 0;

	spans[0] = { floorZ, ceilingZ };
	int count = 1;

	if (sector->e != nullptr)
	{
		for (F3DFloor* ffloor : sector->e->XFloor.ffloors)
		{
			if (ffloor == nullptr)
				continue;
			// Only 3D floors that exist and are solid. Swimmable and fog 3D floors are air
			// to smoke.
			if ((ffloor->flags & (FF_EXISTS | FF_SOLID)) != (FF_EXISTS | FF_SOLID))
				continue;
			if (ffloor->top.plane == nullptr || ffloor->bottom.plane == nullptr)
				continue;

			double top = ffloor->top.plane->ZatPoint(x, y);
			double bottom = ffloor->bottom.plane->ZatPoint(x, y);
			if (top < bottom)
				std::swap(top, bottom);
			if (top - bottom < MIN_SLAB)
			{
				const double middle = (top + bottom) * 0.5;
				bottom = middle - MIN_SLAB * 0.5;
				top = middle + MIN_SLAB * 0.5;
			}

			count = SubtractSpan(spans, count, bottom, top);
			if (count == 0)
				break;
		}
	}
	return count;
}

bool LevelSolidity::IsPointOpen(FLevelLocals* Level, const DVector3& pos)
{
	subsector_t* ss = SubsectorContaining(Level, pos.X, pos.Y);
	if (ss == nullptr || ss->sector == nullptr)
		return false;

	Span spans[MAX_SPANS];
	const int count = SectorOpenSpans(ss->sector, pos.X, pos.Y, spans);
	for (int i = 0; i < count; i++)
	{
		if (pos.Z >= spans[i].Bottom && pos.Z < spans[i].Top)
			return true;
	}
	return false;
}

void LevelSolidity::RasterizeColumns(FLevelLocals* Level, const Box& box, int x0, int y0, int x1, int y1, uint8_t* mask)
{
	if (mask == nullptr || box.SizeX <= 0 || box.SizeY <= 0 || box.SizeZ <= 0 || !(box.CellSize > 0))
		return;

	x0 = std::max(x0, 0);
	y0 = std::max(y0, 0);
	x1 = std::min(x1, box.SizeX);
	y1 = std::min(y1, box.SizeY);
	if (x0 >= x1 || y0 >= y1)
		return;

	const size_t layer = (size_t)box.SizeX * (size_t)box.SizeY;

	// Start open. Every pass below only ever ADDS solidity, so their order does not matter.
	for (int y = y0; y < y1; y++)
	{
		for (int x = x0; x < x1; x++)
		{
			const size_t column = (size_t)x + (size_t)box.SizeX * (size_t)y;
			for (int z = 0; z < box.SizeZ; z++)
				mask[column + layer * (size_t)z] = OPEN;
		}
	}

	// (a) Each column's centre: outside every subsector is solid top to bottom; inside,
	// the cells outside its sector's open spans are solid.
	Span spans[MAX_SPANS];
	for (int y = y0; y < y1; y++)
	{
		const double centerY = box.OriginY + (y + 0.5) * box.CellSize;
		for (int x = x0; x < x1; x++)
		{
			const double centerX = box.OriginX + (x + 0.5) * box.CellSize;
			subsector_t* ss = SubsectorContaining(Level, centerX, centerY);
			const int count = (ss != nullptr && ss->sector != nullptr) ? SectorOpenSpans(ss->sector, centerX, centerY, spans) : 0;
			ApplyColumnSpans(box, x, y, spans, count, mask);
		}
	}

	if (Level == nullptr)
		return;

	// (b) one-sided lines and (c) two-sided lines, over the columns they cross. Found by a
	// bounding-box test over every line, with the vertices read now (a moved polyobject's
	// walls are where they are this frame). One cell of margin around the rectangle.
	const double minX = box.OriginX + (x0 - 1) * box.CellSize;
	const double maxX = box.OriginX + (x1 + 1) * box.CellSize;
	const double minY = box.OriginY + (y0 - 1) * box.CellSize;
	const double maxY = box.OriginY + (y1 + 1) * box.CellSize;

	for (line_t& line : Level->lines)
	{
		if (line.v1 == nullptr || line.v2 == nullptr)
			continue;

		const double ax = line.v1->fX(), ay = line.v1->fY();
		const double bx = line.v2->fX(), by = line.v2->fY();
		if (std::max(ax, bx) < minX || std::min(ax, bx) > maxX || std::max(ay, by) < minY || std::min(ay, by) > maxY)
			continue;

		const double dx = bx - ax, dy = by - ay;
		const double length = std::sqrt(dx * dx + dy * dy);
		if (length < 1e-6)
			continue;

		// A line with no sector behind it is a wall, whatever its flags say.
		const bool twoSided = line.frontsector != nullptr && line.backsector != nullptr;

		// Nudged left, shortened at both ends (see LINE_NUDGE).
		const double ux = dx / length, uy = dy / length;
		const double leftX = -uy, leftY = ux;
		const double shorten = std::min(LINE_NUDGE, length * 0.25);
		const double sx = ax + ux * shorten + leftX * LINE_NUDGE;
		const double sy = ay + uy * shorten + leftY * LINE_NUDGE;
		const double ex = bx - ux * shorten + leftX * LINE_NUDGE;
		const double ey = by - uy * shorten + leftY * LINE_NUDGE;

		const double u0 = (sx - box.OriginX) / box.CellSize;
		const double v0 = (sy - box.OriginY) / box.CellSize;
		const double u1 = (ex - box.OriginX) / box.CellSize;
		const double v1 = (ey - box.OriginY) / box.CellSize;

		TraverseCells(u0, v0, u1, v1, [&](int cx, int cy)
		{
			if (cx < x0 || cx >= x1 || cy < y0 || cy >= y1)
				return;

			if (!twoSided)
			{
				ApplyColumnSpans(box, cx, cy, nullptr, 0, mask);	// the whole column solid
				return;
			}

			// Open only where both sides are open, at this column's centre (slopes).
			const double centerX = box.OriginX + (cx + 0.5) * box.CellSize;
			const double centerY = box.OriginY + (cy + 0.5) * box.CellSize;
			Span front[MAX_SPANS], back[MAX_SPANS], both[MAX_SPANS];
			const int frontCount = SectorOpenSpans(line.frontsector, centerX, centerY, front);
			const int backCount = SectorOpenSpans(line.backsector, centerX, centerY, back);
			const int bothCount = IntersectSpans(front, frontCount, back, backCount, both);
			ApplyColumnSpans(box, cx, cy, both, bothCount, mask);
		});
	}
}
