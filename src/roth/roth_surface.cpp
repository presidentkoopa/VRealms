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

	// Solve the engine's own formula for our rule.
	//
	//   engine:  col = xScale * ( x + xOffset)
	//   want:    col = -x/upt + shiftTexelsX
	//     => xScale  = -1/upt
	//        xOffset = -shiftTexelsX * upt
	//
	//   engine:  row = yScale * (-y + yOffset)
	//   want:    row = +y/upt - shiftTexelsY
	//     => yScale  = -1/upt
	//        yOffset = +shiftTexelsY * upt
	//
	// BOTH SCALES COME OUT NEGATIVE. That is not a slip: the engine's v axis is
	// already negated at the vertex (v0 = -y/64), so matching ROTH's +y needs a
	// second negation, and ROTH's -x needs one of its own. The loader currently
	// uses +1/upt for both, which mirrors every flat on both axes.
	e.xScale = -1.0 / upt;
	e.yScale = -1.0 / upt;
	e.xOffset = -ShiftTexels(s, s.shiftX) * upt;
	e.yOffset = +ShiftTexels(s, s.shiftY) * upt;

	// A mirror flips the axis about the world origin: col -> -col. In this
	// encoding col = A*(x + B), so -col = (-A)*(x + B) -- the SCALE negates and
	// the OFFSET DOES NOT. The shift term still ends up negated, because it
	// rides on A.
	//
	// Negating both, which is the obvious-looking thing to write and which the
	// first draft of this function did, cancels the shift's negation and leaves
	// the mirrored copy a shift off. The round-trip test catches exactly that.
	if (s.mirrorX) e.xScale = -e.xScale;
	if (s.mirrorY) e.yScale = -e.yScale;

	return e;
}

Texel EngineTexel(const FlatEngineSetup &e, uint16_t texW, uint16_t texH,
	int32_t worldX, int32_t worldY)
{
	Texel t;
	t.ok = false;
	t.col = 0;
	t.row = 0;
	if (texW == 0 || texH == 0) return t;

	t.col = WrapTo(e.xScale * ((double)worldX + e.xOffset), texW);
	t.row = WrapTo(e.yScale * (-(double)worldY + e.yOffset), texH);
	t.ok = true;
	return t;
}

} // namespace roth
