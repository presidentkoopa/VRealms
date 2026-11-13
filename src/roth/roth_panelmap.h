#pragma once
//
// roth_panelmap.h -- WHERE A POINT ON A FLOATING PANEL IS ON THE ORIGINAL'S SCREEN.
// Engine-free (plain doubles), so tools/invhand_test checks it without the engine.
//
// Every Realms panel is one picture of the original's own screen pixels (the
// inventory panel: the 0x120 x 0x50 rectangle roth_invdraw draws, anchored at
// the original's (168, 240) on its 640 x 480 screen) shown on one flat
// billboard. A point on that billboard is a point of the original's screen:
// this is the map both ways.
//
//   pixel (x, y) of the picture = left + floor(u * w), top + floor(v * h)
//   u = ((p - centre) . right / halfw + 1) / 2          (mirrored when flipu)
//   v = (1 - (p - centre) . up / halfh) / 2
//
// with right / up / halfw / halfh exactly as BillboardBasis gives them (the
// renderer and the panels' own hit tests share that basis, g_levellocals.h),
// u along the basis' right and v down from its top as hw_sprites.cpp's
// emitQuad lays the texture, mirrored by bb_flipu. Off the picture the same
// formula goes on: a point left of the panel is a negative x, which the
// original's hit test calls "outside the panel" (dialogue_ui.c:577-581).
//
// h is the picture's LOGICAL height (the original's rows). The retail
// ROTH.INI's VideoMode 8 draws no line doubling, so it is the canvas' height;
// in a line-doubled mode it would be half of it (roth_invdraw.h, `hires`).
//
// Engine side: roth_interface.cpp (PanelPixel, InterfacePanelPlane) fills a
// PanelPlane from the billboard and calls these.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <math.h>

namespace roth
{

struct PanelV3
{
	double x = 0, y = 0, z = 0;
	PanelV3() {}
	PanelV3(double a, double b, double c) : x(a), y(b), z(c) {}
	PanelV3 operator+(const PanelV3 &o) const { return PanelV3(x + o.x, y + o.y, z + o.z); }
	PanelV3 operator-(const PanelV3 &o) const { return PanelV3(x - o.x, y - o.y, z - o.z); }
	PanelV3 operator*(double s) const { return PanelV3(x * s, y * s, z * s); }
	double Dot(const PanelV3 &o) const { return x * o.x + y * o.y + z * o.z; }
	double Length() const { return sqrt(x * x + y * y + z * z); }
};

// One panel picture on its billboard.
struct PanelPlane
{
	PanelV3 centre;             // the billboard's position (its centre)
	PanelV3 right, up;          // BillboardBasis' unit axes
	PanelV3 front;              // the unit normal on the side the player stands (toward the eye)
	double halfw = 0, halfh = 0;
	bool flipu = false;         // bb_flipu
	// The picture's rectangle in the original's screen pixels: x from `left`,
	// `w` wide; y from `top`, `h` LOGICAL rows (see the header).
	int left = 0, top = 0, w = 0, h = 0;
};

// Where a point lands on the panel.
struct PanelPoint
{
	bool onPlane = false;       // the ray meets the plane ahead of it (always true for a projection)
	double t = 0;               // along the ray, to the plane
	double u = 0, v = 0;        // 0..1 across / down the picture (outside: beyond)
	int px = 0, py = 0;         // the original's screen pixel
	bool inside = false;        // on the picture itself (0 <= u < 1, 0 <= v < 1)
	PanelV3 at;                 // the point on the plane, in the world
};

// A screen point that no rectangle of the original's holds: what a ray that
// never meets the panel's plane points at (the original's mouse is always
// somewhere; off the panel its hit test says 0x26, dialogue_ui.c:577-581).
constexpr int PANEL_NOWHERE = -0x4000;

inline void PanelPixelOf(const PanelPlane &p, double u, double v, int &px, int &py)
{
	px = p.left + (int)floor(u * p.w);
	py = p.top + (int)floor(v * p.h);
}

// The point `at` of the plane (on it or not: it is taken along the axes).
inline void PanelLocate(const PanelPlane &p, const PanelV3 &at, PanelPoint &out)
{
	const PanelV3 rel = at - p.centre;
	double u = ((rel.Dot(p.right)) / p.halfw + 1.0) * 0.5;
	const double v = (1.0 - (rel.Dot(p.up)) / p.halfh) * 0.5;
	if (p.flipu) u = 1.0 - u;
	out.u = u;
	out.v = v;
	out.at = at;
	PanelPixelOf(p, u, v, out.px, out.py);
	out.inside = u >= 0.0 && u < 1.0 && v >= 0.0 && v < 1.0;
}

// A ray from `start` along `dir` (any length) onto the panel's plane. False
// (onPlane false, and the pixel PANEL_NOWHERE) when it runs parallel to the
// plane or away from it.
inline bool PanelRay(const PanelPlane &p, const PanelV3 &start, const PanelV3 &dir, PanelPoint &out)
{
	out = PanelPoint();
	out.px = out.py = PANEL_NOWHERE;
	if (p.halfw <= 0 || p.halfh <= 0) return false;
	const double denom = p.front.Dot(dir);
	if (fabs(denom) < 1e-9) return false;          // as PanelPixel always tested it
	const double t = p.front.Dot(p.centre - start) / denom;
	if (t <= 0.0) return false;
	PanelLocate(p, start + dir * t, out);
	out.onPlane = true;
	out.t = t;
	return true;
}

// A point (a fingertip) dropped straight onto the plane: where it lands and
// how far in front of the plane it is (negative: through it, behind).
inline double PanelProject(const PanelPlane &p, const PanelV3 &point, PanelPoint &out)
{
	out = PanelPoint();
	const double d = (point - p.centre).Dot(p.front);
	PanelLocate(p, point - p.front * d, out);
	out.onPlane = true;
	out.t = d;
	return d;
}

// The other way: the world point of screen position (x, y) on the panel --
// a pixel's centre is (px + 0.5, py + 0.5). For the round trips and for the
// flat test that aims a hand at a pixel.
inline PanelV3 PanelWorldOf(const PanelPlane &p, double x, double y)
{
	double u = (x - p.left) / p.w;
	const double v = (y - p.top) / p.h;
	if (p.flipu) u = 1.0 - u;
	return p.centre + p.right * ((2.0 * u - 1.0) * p.halfw) + p.up * ((1.0 - 2.0 * v) * p.halfh);
}

} // namespace roth
