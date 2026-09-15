/*
** r_levelray.cpp
**
** [LEVELRAY] A read-only ray through the level's sectors. See the header for the rules of the walk.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Checked on synthetic sector worlds against a brute-force reference that samples the segment and tests every sample's sector by a
** point-in-polygon test -- box rooms, a corridor, a door closed and open, a step, a lift, a pillar, a sloped ceiling, a portal, and
** segments exactly through vertices against their nudged copies -- by levelray_harness_gen.py (L-P7 in
** "Engine docs/EXPOSURE_IMPULSE_SI_L_IMPL_NOTES.md"), which compiles this very text over a mock level.
**
*/

#include <cmath>

#include "r_levelray.h"
#include "g_levellocals.h"
#include "r_defs.h"

namespace
{
	// Two places along the segment closer than this (map units) are one place: lines meeting at a shared vertex, or the
	// line just crossed seen again from the sector beyond it.
	const double kTieUnits = 1.0e-4;

	// How far past a place the walk looks (map units) when a pinched sector leaves its corner rule no answer.
	const double kProbeUnits = 0.01;

	// The walk's own bound on steps, over and above maxCrossings: a step that grazes a corner crosses nothing, and a
	// malformed map must never make the walk spin.
	const int kExtraSteps = 64;

	// The most sectors the walk visits going round one vertex.
	const int kRoundVertexMax = 16;

	// Where the segment from `from` along `delta` meets one line: `share` along the segment (0..1), `onLine` along the line
	// from v1 (0..1). False when they are parallel or the line has no length.
	bool Meet(const DVector3& from, const DVector3& delta, line_t* line, double& share, double& onLine, double& lineTie)
	{
		const DVector2 start = line->v1->fPos();
		const DVector2 along = line->delta;
		const double lineLength = along.Length();
		if (!(lineLength > 1.0e-9))
			return false;
		const double denominator = delta.X * along.Y - delta.Y * along.X;
		if (std::fabs(denominator) < 1.0e-12)
			return false;
		const double ax = start.X - from.X;
		const double ay = start.Y - from.Y;
		share = (ax * along.Y - ay * along.X) / denominator;
		onLine = (ax * delta.Y - ay * delta.X) / denominator;
		lineTie = kTieUnits / lineLength;
		return true;
	}

	// The other side of a line from `sector`; null for a one-sided line. A line with `sector` on both sides returns it.
	sector_t* Beyond(line_t* line, sector_t* sector)
	{
		return line->frontsector == sector ? line->backsector : line->frontsector;
	}

	// The two lines of `sector` that meet at `v`. False when it has other than two there (a pinch).
	bool LinesAt(sector_t* sector, vertex_t* v, line_t*& a, line_t*& b)
	{
		a = b = nullptr;
		int count = 0;
		for (line_t* line : sector->Lines)
		{
			if (line == nullptr || line->v1 == nullptr || line->v2 == nullptr || Beyond(line, sector) == sector)
				continue;
			if (line->v1 != v && line->v2 != v)
				continue;
			if (count == 0)
				a = line;
			else if (count == 1)
				b = line;
			count++;
		}
		return count == 2;
	}

	// Whether direction (ux, uy) from `v` runs into `sector`'s own corner there, bounded by its lines a and b. A sector lies on a
	// line's front side -- the right, walking v1 to v2 -- or its back side. A convex corner is the directions inside both lines; a
	// reflex corner those inside either. A direction along a line counts as inside.
	bool IntoCorner(sector_t* sector, vertex_t* v, line_t* a, line_t* b, double ux, double uy)
	{
		const auto inward = [sector](line_t* line, double& nx, double& ny)
		{
			const double dx = line->delta.X, dy = line->delta.Y;
			if (line->frontsector == sector)
			{
				nx = dy;
				ny = -dx;
			}
			else
			{
				nx = -dy;
				ny = dx;
			}
		};
		double nax, nay, nbx, nby;
		inward(a, nax, nay);
		inward(b, nbx, nby);
		const DVector2 at = v->fPos();
		const DVector2 bFar = (b->v1 == v ? b->v2 : b->v1)->fPos();
		const bool convex = (bFar.X - at.X) * nax + (bFar.Y - at.Y) * nay > 0.0;
		const bool insideA = ux * nax + uy * nay >= 0.0;
		const bool insideB = ux * nbx + uy * nby >= 0.0;
		return convex ? (insideA && insideB) : (insideA || insideB);
	}
}

namespace LevelRay
{
	Result Walk(FLevelLocals* level, const DVector3& from, const DVector3& to, int maxCrossings)
	{
		Result result;
		const DVector3 delta = to - from;
		const double length = delta.Length();
		if (level == nullptr || level->sectors.Size() == 0 || !std::isfinite(length) || !(length > 0.0))
			return result;	// nothing to walk: not blocked, Distance 0
		if (maxCrossings < 0)
			maxCrossings = 0;

		sector_t* sector = level->PointInSector(from.X, from.Y);
		if (sector == nullptr)
		{
			result.Distance = length;
			return result;
		}

		const double tie = kTieUnits / length;
		const double probe = kProbeUnits / length;
		double walked = 0.0;	// the share of the last crossing (or grazed vertex); the next must lie beyond it

		const auto stopAt = [&](double share, bool blocked) -> Result
		{
			result.Blocked = blocked;
			result.Distance = length * share;
			return result;
		};

		for (int step = 0; step < maxCrossings + kExtraSteps; step++)
		{
			// The nearest place past the last crossing where the segment meets a line of this sector.
			bool found = false;
			double nearest = 0.0;
			for (line_t* line : sector->Lines)
			{
				if (line == nullptr || line->v1 == nullptr || Beyond(line, sector) == sector)
					continue;
				double share, onLine, lineTie;
				if (!Meet(from, delta, line, share, onLine, lineTie))
					continue;
				if (share <= walked + tie || share > 1.0 || onLine < -lineTie || onLine > 1.0 + lineTie)
					continue;
				if (!found || share < nearest)
				{
					nearest = share;
					found = true;
				}
			}
			if (!found)
			{
				// The segment ends in this sector.
				result.Distance = length;
				return result;
			}

			// Every line met at that place; the vertex, when one is met at its end.
			line_t* tied[8] = {};
			int tiedCount = 0;
			vertex_t* vertex = nullptr;
			for (line_t* line : sector->Lines)
			{
				if (line == nullptr || line->v1 == nullptr || Beyond(line, sector) == sector)
					continue;
				double share, onLine, lineTie;
				if (!Meet(from, delta, line, share, onLine, lineTie))
					continue;
				if (share <= walked + tie || share > 1.0 || onLine < -lineTie || onLine > 1.0 + lineTie)
					continue;
				if (std::fabs(share - nearest) > tie)
					continue;
				if (vertex == nullptr && onLine <= lineTie)
					vertex = line->v1;
				else if (vertex == nullptr && onLine >= 1.0 - lineTie)
					vertex = line->v2;
				if (tiedCount < 8)
					tied[tiedCount] = line;
				tiedCount++;
			}

			const DVector2 at(from.X + delta.X * nearest, from.Y + delta.Y * nearest);
			const double z = from.Z + delta.Z * nearest;
			sector_t* next = nullptr;

			if (vertex == nullptr && tiedCount == 1)
			{
				// The middle of one line: its other side, if it has an open one.
				if (result.Crossings >= maxCrossings)
					return stopAt(walked, false);	// cut short before a further crossing: the last crossing walked
				line_t* line = tied[0];
				next = Beyond(line, sector);
				if (next == nullptr || line->isLinePortal())
					return stopAt(nearest, true);
			}
			else
			{
				if (nearest + probe >= 1.0)
				{
					// The segment ends at the vertex.
					result.Distance = length;
					return result;
				}
				line_t* a = nullptr;
				line_t* b = nullptr;
				if (vertex != nullptr && LinesAt(sector, vertex, a, b))
				{
					// A VERTEX: this sector's own corner there decides. Into it, the segment only grazes the corner.
					if (IntoCorner(sector, vertex, a, b, delta.X, delta.Y))
					{
						walked = nearest;
						continue;
					}
					if (result.Crossings >= maxCrossings)
						return stopAt(walked, false);	// cut short before a further crossing: the last crossing walked
					// Out of it: round the vertex through open lines (two-sided, not portals) to the sector whose corner the segment
					// runs into. None: it runs into the void or behind a wall -- blocked.
					sector_t* visited[kRoundVertexMax] = { sector };
					int visitedCount = 1;
					for (int i = 0; i < visitedCount && next == nullptr; i++)
					{
						for (line_t* line : visited[i]->Lines)
						{
							if (line == nullptr || (line->v1 != vertex && line->v2 != vertex) || line->isLinePortal())
								continue;
							sector_t* other = Beyond(line, visited[i]);
							if (other == nullptr || other == visited[i])
								continue;
							bool seen = false;
							for (int j = 0; j < visitedCount; j++)
								seen = seen || visited[j] == other;
							if (seen || visitedCount >= kRoundVertexMax)
								continue;
							visited[visitedCount++] = other;
							line_t* oa = nullptr;
							line_t* ob = nullptr;
							if (LinesAt(other, vertex, oa, ob) && IntoCorner(other, vertex, oa, ob, delta.X, delta.Y))
							{
								next = other;
								break;
							}
						}
					}
					if (next == nullptr)
						return stopAt(nearest, true);
				}
				else
				{
					// A pinched sector, or lines too close to tell apart: the sector a hundredth of a unit past the place decides.
					const double past = nearest + probe;
					sector_t* onward = level->PointInSector(from.X + delta.X * past, from.Y + delta.Y * past);
					if (onward == nullptr || onward == sector)
					{
						walked = nearest;
						continue;
					}
					if (result.Crossings >= maxCrossings)
						return stopAt(walked, false);
					bool leads = false;
					bool allOpen = tiedCount <= 8;
					const int count = tiedCount < 8 ? tiedCount : 8;
					for (int i = 0; i < count; i++)
					{
						sector_t* other = Beyond(tied[i], sector);
						const bool open = other != nullptr && !tied[i]->isLinePortal();
						if (!open)
							allOpen = false;
						else if (other == onward)
							leads = true;
					}
					if (!leads && !allOpen)
						return stopAt(nearest, true);
					next = onward;
				}
			}

			// The opening where it crosses: the higher floor and the lower ceiling of the two sectors, at that point.
			const double floorHere = sector->floorplane.ZatPoint(at);
			const double floorNext = next->floorplane.ZatPoint(at);
			const double ceilingHere = sector->ceilingplane.ZatPoint(at);
			const double ceilingNext = next->ceilingplane.ZatPoint(at);
			const double bottom = floorHere > floorNext ? floorHere : floorNext;
			const double top = ceilingHere < ceilingNext ? ceilingHere : ceilingNext;
			if (!(z >= bottom && z <= top))
				return stopAt(nearest, true);

			result.Crossings++;
			sector = next;
			walked = nearest;
		}

		// The step bound (a malformed map): ended unblocked at the last crossing walked.
		return stopAt(walked, false);
	}
}
