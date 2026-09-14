/*
** hw_levelsolidity.h
**
** [LEVELSOLIDITY] Where the level is solid: a point test and a column rasteriser.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists ("Engine docs/SMOKE_VOLUME_PLAN.md" SH1, review S2): anything
** that fills the level's air -- the smoke volume's solid mask (#13) first, then the
** level collision field's sign (#8) -- needs to know which cells are wall. The
** obvious tool does not work: PointInSector walks the game BSP and always returns a
** leaf without testing that the point is inside it, so the void inside a thick wall
** comes back as the neighbouring room, open floor to ceiling, and smoke would flow
** through every wall.
**
** So:
**   - A POINT is open only inside a GL-node (render) subsector -- tested against the
**     subsector's own segs, which close a convex polygon -- and between its sector's
**     CURRENT floor and ceiling at that x, y (slopes), minus solid 3D floors.
**   - A BOX OF CELLS is rasterised column by column from those points, then every
**     one-sided line marks the columns it crosses solid for their full height (a wall
**     thinner than a cell cannot leak), and every two-sided line leaves the columns it
**     crosses open only where both sides' spans overlap (a closed door is solid, an
**     open one open, a step keeps its overlap).
**
** Stateless and read-only: it reads the map and the sectors' current planes on the
** calling (main) thread and changes nothing, so it can never go stale across maps and
** never touches the playsim. The renderer interpolates planes while it draws, so a
** caller inside a frame sees the heights on screen. SectorPlanes (hw_sectorplanes.h)
** tells a caller WHEN a sector moved and its columns need doing again.
**
** Known limits: sector portals and line portals are not followed; a moved
** polyobject's walls are solid where they are now, but its inside is judged by the
** subsector underneath; ML_BLOCKING and the like are ignored (they stop players, not
** air); a sky ceiling is a ceiling.
**
*/

#pragma once

#include <cstdint>
#include "vectors.h"

struct FLevelLocals;
struct sector_t;
struct subsector_t;

namespace LevelSolidity
{
	// An open vertical interval [Bottom, Top), map units.
	struct Span
	{
		double Bottom;
		double Top;
	};

	// The most open spans one column holds. A sector needing more (more than 15 solid 3D
	// floors over one point) keeps its lowest ones; the rest of the column is solid.
	enum { MAX_SPANS = 16 };

	// Mask values.
	enum : uint8_t { OPEN = 0, SOLID = 255 };

	// The render subsector whose polygon holds (x, y), or null when (x, y) is in no
	// subsector -- the void inside a wall, outside the map, or a degenerate leaf.
	subsector_t* SubsectorContaining(FLevelLocals* Level, double x, double y);

	// The open spans of `sector` at (x, y), sorted upward: [floor, ceiling) minus every
	// existing solid 3D floor. Returns how many (0 = none: closed, or a null sector).
	int SectorOpenSpans(const sector_t* sector, double x, double y, Span* spans);

	// Is this point in the level's open air?
	bool IsPointOpen(FLevelLocals* Level, const DVector3& pos);

	// A world-aligned box of cubic cells. Cell (x, y, z) spans
	// [Origin + index * CellSize, Origin + (index + 1) * CellSize) on each axis.
	struct Box
	{
		double OriginX = 0;
		double OriginY = 0;
		double OriginZ = 0;
		double CellSize = 8;
		int SizeX = 0;
		int SizeY = 0;
		int SizeZ = 0;
	};

	// Writes the columns [x0, x1) x [y0, y1) of `box` into `mask`, which holds the whole
	// box: mask[x + SizeX * (y + SizeY * z)] -- a 3D texture's texel order -- is SOLID or
	// OPEN. Other columns are left as they are, so a caller spreads the work over frames
	// a slab at a time. A cell is open when its centre is in open air and no line rules
	// its column out; see the file comment.
	void RasterizeColumns(FLevelLocals* Level, const Box& box, int x0, int y0, int x1, int y1, uint8_t* mask);
}
