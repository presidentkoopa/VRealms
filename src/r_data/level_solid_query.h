/*
** level_solid_query.h
**
** RS FORK -- WHAT IS SOLID NEAR A POINT, READ BY THE RENDERER.
**
** A general, read-only question for draw-time code: "which solid parts of the level
** lie within this distance of this point?" The first caller is a reach chain's
** clearance (model_reach.cpp, VR_BODY_IK_RETURN_PLAN.md 4b idea 6: an arm's elbow
** swinging clear of a wall); anything drawn at display rate that has to stay out of
** walls -- a held prop, a strap, a cable, a camera boom -- asks the same question.
**
** THE ANSWER IS CONVEX REGIONS, each up to six half-spaces in DOOM MAP coordinates
** (x, y, z up): the solid is where n_i . p + c_i <= 0 for EVERY plane i, n_i unit, so
** max_i (n_i . p + c_i) is how far outside the region p is -- exact across a face,
** continuous everywhere. What a region is:
**
**   - a one-sided wall: the void behind the line, within the line's length (no depth
**     limit -- nothing is ever behind a one-sided line);
**   - each side of any line whose sector it faces: the solid BELOW that sector's floor
**     and ABOVE its ceiling, as a slab `slabDepth` deep on that side (sloped planes
**     exact) -- a step's face, a ledge's top edge, a lintel;
**   - each solid 3D floor of such a sector: the slab between its two planes;
**   - a polyobject line: a slab `slabDepth` deep behind its face.
**
** Floors and ceilings therefore count near lines, not across the middle of a room --
** a region that appears only when a line comes into range would pop. Passable line
** portals are skipped.
**
** READ ONLY, AND THAT IS THE NETPLAY CONTRACT. Nothing in the level is written: the
** blockmap and the polyobject links are walked with a local de-duplication, never
** validcount, so this can be called from the renderer between tics or mid-frame and
** cannot change what the playsim does next. No RNG, no allocation the playsim sees.
**
** The region geometry is inline below (namespace LevelSolid), apart from the level walk
** in level_solid_query.cpp, so it can be checked outside the engine.
**
**---------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <cmath>
#include "vectors.h"
#include "tarray.h"

struct FLevelLocals;

struct FLevelSolidRegion
{
	int      planes = 0;
	DVector3 n[6];
	double   c[6] = {};
};

// Every solid region within `radius` (map units) of `center` (Doom map coordinates), into `out`
// (cleared first). `slabDepth`: how deep a face slab reaches behind its face. False when the answer
// was cut short at maxRegions -- the regions returned are still valid.
bool LevelSolidQuery(FLevelLocals *Level, const DVector3 &center, double radius, double slabDepth,
	TArray<FLevelSolidRegion> &out, unsigned maxRegions = 128);

namespace LevelSolid
{

// Sign conventions, so nobody re-derives them:
//   - a line's FRONT side is to the RIGHT of v1 -> v2 (P_PointOnLineSidePrecise returns 1, the
//     back, for a point to the left). Its front normal is (dy, -dx) / length.
//   - a sector plane is normal . p + D = 0, and which way the normal points differs between floors,
//     ceilings and 3D-floor planes, so the solid side is chosen from the sign of normal.z, never
//     assumed: "solid below" means the points under the plane are inside.

// One line, in the plane.
struct FLineGeom
{
	DVector2 a;		// v1
	DVector2 u;		// unit, v1 -> v2
	DVector2 nf;	// unit, the front side's normal
	double   len = 0.;

	bool Set(const DVector2 &v1, const DVector2 &delta)
	{
		len = delta.Length();
		if (!(len > 1.e-6) || !std::isfinite(len)) return false;
		a = v1;
		u = delta / len;
		nf = DVector2(u.Y, -u.X);
		return true;
	}

	// Distance from p to the segment, in the plane.
	double SegmentDistance(const DVector2 &p) const
	{
		const DVector2 cp = p - a;
		const double along = std::clamp(cp | u, 0., len);
		return (cp - u * along).Length();
	}
};

inline void AddPlane(FLevelSolidRegion &r, const DVector3 &n, double c)
{
	if (r.planes < 6)
	{
		r.n[r.planes] = n;
		r.c[r.planes] = c;
		r.planes++;
	}
}

// How far outside the region p is: max over its planes (<= 0 inside).
inline double RegionOutside(const FLevelSolidRegion &r, const DVector3 &p)
{
	double out = -1.e300;
	for (int i = 0; i < r.planes && i < 6; i++) out = std::max(out, (r.n[i] | p) + r.c[i]);
	return out;
}

// Within the line's length: along in [0, len].
inline void AddCaps(FLevelSolidRegion &r, const FLineGeom &g)
{
	const double ua = g.u | g.a;
	AddPlane(r, DVector3(-g.u.X, -g.u.Y, 0.), ua);			// outside when along < 0
	AddPlane(r, DVector3(g.u.X, g.u.Y, 0.), -ua - g.len);	// outside when along > len
}

// A slab on the side unit nInto points into: the solid is at depth 0..depth measured along nInto.
inline void AddSlab(FLevelSolidRegion &r, const FLineGeom &g, const DVector2 &nInto, double depth)
{
	const double na = nInto | g.a;
	AddPlane(r, DVector3(-nInto.X, -nInto.Y, 0.), na);			// outside when depth < 0
	AddPlane(r, DVector3(nInto.X, nInto.Y, 0.), -na - depth);	// outside when depth > the slab
}

// A sector-style plane (normal . p + D = 0) as a half-space of the region. solidBelow: the solid is
// under it. False for a plane with no height (a vertical or degenerate normal), which bounds nothing.
inline bool AddHeightPlane(FLevelSolidRegion &r, const DVector3 &normal, double D, bool solidBelow)
{
	const double k = normal.Length();
	if (!(k > 1.e-9) || !(fabs(normal.Z) > 1.e-9 * k)) return false;
	double sgn = normal.Z > 0. ? 1. : -1.;	// normal . p + D grows upward with this sign
	if (!solidBelow) sgn = -sgn;
	AddPlane(r, normal * (sgn / k), D * (sgn / k));
	return true;
}

// A one-sided line: the void behind it, within its length.
inline FLevelSolidRegion VoidBehind(const FLineGeom &g)
{
	FLevelSolidRegion r;
	AddPlane(r, DVector3(g.nf.X, g.nf.Y, 0.), -(g.nf | g.a));	// outside in front
	AddCaps(r, g);
	return r;
}

// The solid on one side of a line (frontSide: the front) beyond one sector plane, `depth` deep.
inline bool FaceBeyondPlane(FLevelSolidRegion &r, const FLineGeom &g, bool frontSide, double depth,
	const DVector3 &normal, double D, bool solidBelow)
{
	r = FLevelSolidRegion();
	AddSlab(r, g, frontSide ? g.nf : -g.nf, depth);
	if (!AddHeightPlane(r, normal, D, solidBelow)) return false;
	AddCaps(r, g);
	return true;
}

// The solid on one side of a line between two planes (a 3D floor: under its top, over its bottom),
// `depth` deep: side, depth, top, bottom and the two caps -- exactly six planes.
inline bool FaceBetweenPlanes(FLevelSolidRegion &r, const FLineGeom &g, bool frontSide, double depth,
	const DVector3 &topNormal, double topD, const DVector3 &bottomNormal, double bottomD)
{
	r = FLevelSolidRegion();
	AddSlab(r, g, frontSide ? g.nf : -g.nf, depth);
	if (!AddHeightPlane(r, topNormal, topD, true) || !AddHeightPlane(r, bottomNormal, bottomD, false)) return false;
	AddCaps(r, g);
	return true;
}

// A polyobject's line: a slab behind its front face, full height.
inline FLevelSolidRegion SlabBehind(const FLineGeom &g, double depth)
{
	FLevelSolidRegion r;
	AddSlab(r, g, -g.nf, depth);
	AddCaps(r, g);
	return r;
}

} // namespace LevelSolid
