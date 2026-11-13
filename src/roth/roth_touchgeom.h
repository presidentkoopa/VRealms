#pragma once
//
// THE HAND TOUCHING A WALL, engine-free: the nearest point of a wall segment to
// a palm, in the map's plane, and how far the palm is from it. Used by the
// grip (roth_runtime.cpp PickTouchSurface) and checked by tools/vrpick_test
// (test_touch.cpp) on the owner's run's own numbers.
//
// A palm laid on a door is level with it, or a hair through it, so a test that
// casts a ray from the palm in some direction can start past the surface or
// point along it and miss; the distance from the palm to the segment does not
// depend on which way the hand faces.
//

#include <math.h>

namespace roth
{

struct TouchSeg { double x1, y1, x2, y2; };

// The closest point of the segment to (px, py), and the distance to it.
inline double TouchDistance(const TouchSeg &s, double px, double py, double &cx, double &cy)
{
	const double dx = s.x2 - s.x1, dy = s.y2 - s.y1;
	const double len2 = dx * dx + dy * dy;
	double t = len2 > 1e-12 ? ((px - s.x1) * dx + (py - s.y1) * dy) / len2 : 0.0;
	if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
	cx = s.x1 + dx * t; cy = s.y1 + dy * t;
	const double ex = px - cx, ey = py - cy;
	double d2 = ex * ex + ey * ey;
	return sqrt(d2);
}

} // namespace roth
