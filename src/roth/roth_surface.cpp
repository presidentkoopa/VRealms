/*
** roth_surface.cpp
** The rules, in one place. See roth_surface.h for why this module exists.
**
** Every rule here is traced from ROTH.C's VISIBLE pass. Where a rule is derived
** rather than read, it says so in the code, not only in the commit message --
** three constants on this project have shipped as "measured" while resting on
** an inference, and each had to be reversed.
*/

#include "roth_surface.h"

#include <math.h>

namespace roth
{

// ---------------------------------------------------------------------------

SurfaceClass ClassifySurface(uint16_t textureWord, uint16_t colourKey)
{
	SurfaceClass c;

	// ORDER MATTERS, and it is the original's order (renderer.c:9222-9225):
	// the sign test comes FIRST, so a colour key that happened to be negative
	// would still be a solid colour. Testing the key first would change which
	// surfaces disappear.
	if ((int16_t)textureWord < 0)
	{
		c.kind = SurfaceKind::SolidColour;
		c.solidIndex = (uint8_t)(textureWord & 0xFF);
		return c;
	}
	if (textureWord == colourKey)
	{
		// The original emits no span at all. It does NOT draw sky here: the sky
		// is a 2D band drawn above EDGE_MAP walls and is never a flat. What
		// shows through a key ceiling is whatever those walls already drew.
		c.kind = SurfaceKind::Nothing;
		c.solidIndex = 0;
		return c;
	}
	c.kind = SurfaceKind::Textured;
	c.solidIndex = 0;
	return c;
}

// ---------------------------------------------------------------------------

double FlatUnitsPerTexel(const FlatSetup &s)
{
	// 2^s, and for a 256x256 OPAQUE image 2^(s-1) (renderer.c:3385-3393).
	//
	// The exception is not cosmetic: in STUDY1 it is what puts the s=2 ceiling
	// on the same 2 units per texel as the s=1 floors, which the oracle
	// measured directly. A wrong exception would have produced 4 there and
	// broken that agreement, so this is checkable rather than decorative.
	const int e = s.opaque256 ? (int)s.scaleBits - 1 : (int)s.scaleBits;
	if (e <= 0) return 1.0;
	return (double)(1 << e);
}

// A shift unit is HALF a texel normally, and a FULL texel on a 256x256 opaque
// image (the same exception, same lines).
static double ShiftTexels(const FlatSetup &s, uint8_t shift)
{
	return s.opaque256 ? (double)shift : (double)shift * 0.5;
}

// Positive modulus. C's % is not it for negative operands, and a world point
// west or south of the origin is the common case, not an edge case.
static uint16_t WrapTo(double v, uint16_t n)
{
	if (n == 0) return 0;
	double m = v;
	const double dn = (double)n;
	m = m - dn * (double)(int64_t)(m / dn);   // trunc toward zero
	if (m < 0) m += dn;
	int32_t i = (int32_t)m;
	if (i < 0) i = 0;
	if (i >= (int32_t)n) i = (int32_t)(i % n);
	return (uint16_t)i;
}

Texel FlatTexel(const FlatSetup &s, int32_t worldX, int32_t worldY, uint16_t colourKey)
{
	Texel t;
	t.ok = false;
	t.col = 0;
	t.row = 0;

	const SurfaceClass sc = ClassifySurface(s.textureWord, colourKey);
	if (sc.kind != SurfaceKind::Textured) return t;   // nothing, or a flat colour
	if (s.texW == 0 || s.texH == 0) return t;

	const double upt = FlatUnitsPerTexel(s);

	//
	// THE TWO SIGNS ARE [derived] -- see roth_surface.h. They are here so the
	// static texel test can check them against the oracle; do not cite this
	// code as evidence for them.
	//
	double col = -(double)worldX / upt + ShiftTexels(s, s.shiftX);
	double row = +(double)worldY / upt - ShiftTexels(s, s.shiftY);

	// A mirror negates that axis INCLUDING its shift term -- the mirror is
	// about the world origin, not about the surface. Negating the coordinate
	// but not the shift moves the seam instead of closing it, which is what a
	// half-applied mirror looks like on screen.
	if (s.mirrorX) col = -col;
	if (s.mirrorY) row = -row;

	t.col = WrapTo(col, s.texW);
	t.row = WrapTo(row, s.texH);
	t.ok = true;
	return t;
}

// ---------------------------------------------------------------------------

FlatEngineSetup FlatToEngine(const FlatSetup &s)
{
	FlatEngineSetup e;
	const double upt = FlatUnitsPerTexel(s);

	// THE ENGINE, AS IT REALLY IS (hw_flats.cpp:68-98, hw_vertexbuilder.cpp:176):
	//
	//     (u0, v0) = (x/64, -y/64)
	//     tc       = scale(S) * translate(Offs/D) * scale(64/D) * rotate(-angle) * (u0, v0)
	//
	// with D the texture's DISPLAY size. In texels that is
	//
	//     u = S.X * ( Offs.X + R.u )      R = rotate(-angle) * (x, -y)
	//     v = S.Y * ( Offs.Y + R.v )
	//
	// and, because of the quarter turn in roth_texture.cpp, u indexes the
	// image's STORED ROW and v its STORED COLUMN (display width = stored H).
	//
	// angle = 270  ->  rotate(-270) = rotate(90)  ->  R.u = +y, R.v = +x.
	//
	//   want  row = my * ( +y/upt - shY )       (my = -1 when mirrored)
	//     u = S.X*(Offs.X + y)  =>  S.X = my/upt,   Offs.X = -shY*upt
	//   want  col = mx * ( -x/upt + shX )
	//     v = S.Y*(Offs.Y + x)  =>  S.Y = -mx/upt,  Offs.Y = -shX*upt
	//
	// A mirror flips only the SCALE: the offset rides inside the bracket, so the
	// shift is negated with the axis -- which is what the oracle measured
	// (LRINTH1 sector 210: mirrorX, shift 64 -> col = +x/2 + 192 on a 256 image).
	const double mx = s.mirrorX ? -1.0 : 1.0;
	const double my = s.mirrorY ? -1.0 : 1.0;
	e.angle   = 270.0;
	e.xScale  = my / upt;
	e.yScale  = -mx / upt;
	e.xOffset = -ShiftTexels(s, s.shiftY) * upt;
	e.yOffset = -ShiftTexels(s, s.shiftX) * upt;
	return e;
}

Texel EngineTexel(const FlatEngineSetup &e, uint16_t texW, uint16_t texH,
	int32_t worldX, int32_t worldY)
{
	// A model of the GZDoom flat path WITH the quarter-turned texture, written
	// from the engine's matrix rather than from FlatToEngine's algebra, so the
	// round-trip test compares two independent things.
	Texel t;
	t.ok = false;
	t.col = 0;
	t.row = 0;
	if (texW == 0 || texH == 0) return t;

	const double PI = 3.14159265358979323846;
	const double th = -e.angle * PI / 180.0;
	const double c = cos(th), sn = sin(th);
	const double u0 = (double)worldX, v0 = -(double)worldY;      // x64 cancels below
	const double ru = u0 * c - v0 * sn;
	const double rv = u0 * sn + v0 * c;
	const double u = e.xScale * (e.xOffset + ru);   // texels along the display width  = stored rows
	const double v = e.yScale * (e.yOffset + rv);   // texels along the display height = stored cols

	// Round the rotation's float dust away before flooring: cos(90) is not 0.
	const double ur = floor(u * 4096.0 + 0.5) / 4096.0;
	const double vr = floor(v * 4096.0 + 0.5) / 4096.0;
	t.row = WrapTo(ur, texH);
	t.col = WrapTo(vr, texW);
	t.ok = true;
	return t;
}

} // namespace roth
