/*
** r_levelray.h
**
** [LEVELRAY] A read-only ray through the level's sectors.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SENSORY_IMPULSES_PLAN.md" section 4 (the text below the rule is its frozen header) and
** "Engine docs/EXPOSURE_IMPULSE_SI_L_IMPL_NOTES.md". Named for what it does, not for its callers: flash sight
** (hw_exposureimpulse.cpp) and the room probe (s_hearingimpulse.cpp) now, #23 acoustics and any other cheap presentation
** ray later.
**
** THE RULES OF THE WALK (r_levelray.cpp):
**   - It starts in the sector PointInSector gives for `from`. A line met at the very start (within 1/10000 of a map unit)
**     is not crossed: the segment begins on that side.
**   - Each step takes the nearest line of the current sector the segment meets further on. A line with this sector on both
**     sides is not a crossing.
**   - A crossing BLOCKS when the line is one-sided or a line portal, or when the segment's height there is below the higher
**     floor or above the lower ceiling of the two sectors it joins (both planes evaluated at the crossing).
**   - Where the segment meets a vertex, the sector's own corner there decides (its two lines at the vertex; a sector lies on
**     a line's front, the right walking v1 to v2, or its back). Running into that corner it only grazes: nothing crossed.
**     Running out of it, the walk goes round the vertex through open lines (two-sided, not portals) to the sector whose
**     corner the segment runs into -- one crossing, the opening tested at the vertex -- and blocks when there is none (the
**     void, or behind a one-sided line or a portal). A pinched sector (other than two lines at the vertex) falls back to
**     the sector PointInSector gives a hundredth of a unit past it.
**   - After maxCrossings crossings, a further crossing ends the walk unblocked, Distance at the last crossing walked.
**   - The end point is not tested against its own sector's floor or ceiling, and a line exactly at `to` is crossed like
**     any other: a caller whose point sits on a wall's face stops its segment a little short (hw_exposureimpulse.cpp stops
**     1 unit short).
**
*/

// [LEVELRAY] A READ-ONLY RAY THROUGH THE LEVEL'S SECTORS, for presentation code (flash sight, room probes, later #23).
// A BSP walk (PointInSector) and each sector's own line list, as hw_debrislanding.cpp's LevelQuery. No validcount,
// nothing written, no allocation, no RNG. Main thread, outside the playsim tick. 3D floors, polyobjects and portals
// are not considered: a line portal blocks.
#pragma once
#include "vectors.h"
struct FLevelLocals;
namespace LevelRay
{
	struct Result
	{
		bool   Blocked = false;   // a one-sided line, or an opening that does not contain the segment's height there
		double Distance = 0.;     // map units from `from` to the stop, or to `to` / the last crossing walked
		int    Crossings = 0;     // two-sided lines crossed
	};
	Result Walk(FLevelLocals* level, const DVector3& from, const DVector3& to, int maxCrossings = 32);
}
