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


// ---------------------------------------------------------------------------
// Walls. Every rule is cited in roth_surface.h; the line numbers here say
// which statement of ROTH.C renderer.c each step is.
// ---------------------------------------------------------------------------

namespace
{
// texture map +8, as roth_raw.h names them
const uint8_t WF_TRANSPARENT = 0x01, WF_FLIP_X = 0x02, WF_IMAGE_FIT = 0x04, WF_TRANS_UPLO = 0x08,
	WF_HALF_PIXEL = 0x20, WF_PIN_BOTTOM = 0x80;

bool Pow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

// The row of a piece that runs past its image's last row.
//
// On a power of two the original's wrapping mapper is a repeat. On anything
// else it is NOT a function of the row at all: the mapper keeps a running
// index and ANDs it with dim-1 after every step (renderer.c:1929-1935), so
// where a bit is missing from the mask a one-row step is thrown away and the
// index sticks. Measured (tools/oracle, STUDY1 face 233, image 176, 109 rows,
// wall 256 units tall, from 120 units away): ONE row, 4, down the whole piece.
// From further away the steps are longer and other rows appear. What it draws
// depends on the distance, so there is nothing to reproduce; a repeat -- what
// the mask is on every power of two -- is used instead. tools/wallrule counts
// these pieces.
int WrapRow(int r, int dimV)
{
	if (Pow2(dimV)) return (int)((uint16_t)r & (uint16_t)(dimV - 1));
	r %= dimV;
	return r < 0 ? r + dimV : r;
}
int IMin(int a, int b) { return a < b ? a : b; }
int IMax(int a, int b) { return a > b ? a : b; }
}

bool WallOverlayFollows(uint8_t flags, bool midIsColourKey, int ownFloor, int ownCeil,
	bool nbrHasPlat, int nbrPlatUnder, int nbrPlatTop,
	bool ownHasPlat, int ownPlatUnder, int ownPlatTop)
{
	if (flags & WF_TRANSPARENT) return true;            // 8767
	if (!nbrHasPlat || midIsColourKey) return false;    // 8782-8783
	// project_wall_face_span_extents, 8596-8610: the platform's own slab, cut
	// to this sector's height and by this sector's own platform.
	int ax = nbrPlatUnder, dx = nbrPlatTop;
	if (ax >= nbrPlatTop) return false;
	if (ax >= ownCeil) return false;
	if (ax < ownFloor) ax = ownFloor;
	if (dx <= ownFloor) return false;
	if (dx > ownCeil) dx = ownCeil;
	if (ownHasPlat && ax < ownPlatTop && dx > ownPlatUnder)
	{
		if (ax >= ownPlatUnder) ax = ownPlatTop;
		if (dx <= ownPlatTop) dx = ownPlatUnder;
	}
	return ax < dx;
}

WallPiece WallPieceRule(const WallFace &f, WallTier tier, int dimU, int dimV)
{
	WallPiece p;
	if (dimU <= 0 || dimV <= 0) return p;
	uint8_t pf = f.flags;
	int top, bot, anchor, span;
	if (!f.twoSided)
	{
		if (tier != WALL_MID) return p;
		top = anchor = f.ownCeil; bot = f.ownFloor; span = top - bot;     // 8866-8876
	}
	else if (tier == WALL_UPPER)
	{
		if (!(f.nbrCeil < f.ownCeil)) return p;                            // 8659
		top = anchor = f.ownCeil; bot = IMax(f.nbrCeil, f.ownFloor);       // 8660-8664
		span = top - bot;                                                  // 8671
		if (f.overlayFollows) pf &= 0xfa;                                  // 8773, 8785
	}
	else if (tier == WALL_LOWER)
	{
		if (!(f.nbrFloor > f.ownFloor)) return p;                          // 8681
		top = anchor = IMin(f.nbrFloor, f.ownCeil); bot = f.ownFloor;      // 8682-8683
		span = top - bot;                                                  // 8693
		if (f.overlayFollows) pf &= 0xfa;
	}
	else
	{
		if (!(f.flags & WF_TRANSPARENT)) return p;
		const int openTop = IMin(f.ownCeil, f.nbrCeil), openBot = IMax(f.ownFloor, f.nbrFloor);
		int bandTop = openTop, bandBot = openBot;                          // 8191-8199
		if ((f.flags & WF_TRANS_UPLO) && f.ownOverride != 0)               // 8201-8212
		{
			if (f.ownOverride > 0) bandBot = bandTop - 4 * f.ownOverride;
			else                   bandTop = bandBot - 4 * f.ownOverride;
		}
		anchor = bandTop; span = bandTop - bandBot;                        // 8730, 8734
		// The band can reach past the opening (an override of -125 stands a
		// 500-unit band on the floor of a 384-unit room). What the original
		// shows of the overhang depends on its draw order, which is not
		// modelled: the piece is kept to the opening here.
		top = IMin(bandTop, openTop); bot = IMax(bandBot, openBot);
	}
	if (top <= bot || span <= 0) return p;

	p.drawn = true;
	p.top = top; p.bot = bot; p.anchor = anchor; p.span = span;
	p.mirror = (pf & WF_FLIP_X) != 0;                                      // 4735
	const bool half = (pf & WF_HALF_PIXEL) != 0;
	if (!(pf & WF_IMAGE_FIT))                                              // 8272 -> 13334
	{
		const int extU = f.extent * (half ? 2 : 1);                        // 13335-13336
		p.u0 = f.shiftX;                                                   // 9274, 4733-4734
		p.su = f.length > 0 ? (extU * 0.5) / f.length : 0.;
		p.cols = (extU + 1) / 2;
		// 13337-13338, as the 16-bit signed compare it is
		const int16_t room = (int16_t)(uint16_t)(2 * dimU - 2 * f.shiftX);
		p.uMasked = !(room >= (int16_t)(uint16_t)extU);
		const int extV = span * (half ? 2 : 1);
		int start = f.shiftY;                                              // 5050
		if (pf & WF_PIN_BOTTOM)                                            // 5056-5063
		{
			start = (int16_t)(((int16_t)(uint16_t)(2 * dimV - extV)) >> 1) - f.shiftY;
			if (start < 0) start &= (dimV - 1);
		}
		p.v0 = start;
		p.sv = half ? 1. : 0.5;                                            // 4739-4740
		p.vMasked = 2 * (dimV - start) < extV;                             // 5065-5069
	}
	else
	{
		p.fit = true;                                                      // 13342-13349
		p.u0 = 0.; p.su = f.length > 0 ? (double)dimU / f.length : 0.; p.cols = dimU;
		p.v0 = 0.; p.sv = (double)dimV / span;
	}
	return p;
}

Texel WallTexel(const WallPiece &p, int dimU, int dimV, double d, double z)
{
	Texel t; t.ok = false; t.col = t.row = 0;
	if (!p.drawn || z > p.top || z < p.bot) return t;
	int c = (int)floor(p.u0 + p.su * d + 1e-7);
	if (p.mirror) c = dimU - 1 - c;
	if (p.uMasked) c = (int)((uint16_t)c & (uint16_t)(dimU - 1));
	int r = (int)floor(p.v0 + p.sv * (p.anchor - z) + 1e-7);
	if (p.vMasked) r = WrapRow(r, dimV);
	if (c < 0 || c >= dimU || r < 0 || r >= dimV) return t;
	t.ok = true; t.col = (uint16_t)c; t.row = (uint16_t)r;
	return t;
}

int WallTexelLength(const WallFace &f)
{
	if (f.extent > 0) return f.extent < 65535 ? f.extent : 65535;
	// No extent: the original lays ZERO half texels across an unfitted piece
	// (one column, repeated). A fitted face never reads the extent.
	return (f.flags & WF_IMAGE_FIT) ? -1 : 0;
}

WallEngineSetup WallToEngine(const WallFace &f, WallTier tier, const WallPiece &p,
	int dimU, int dimV, int texelLength)
{
	WallEngineSetup e;
	if (!p.drawn) return e;
	e.drawn = true;

	// What the engine counts rows from.
	int ref = f.ownCeil;
	if (f.twoSided && tier == WALL_LOWER) ref = f.nbrFloor;
	if (f.twoSided && tier == WALL_MID)   ref = IMin(f.ownCeil, f.nbrCeil);

	if (p.fit)
	{
		// One copy across TexelLength and down the span. The engine turns a
		// scale into an integer size with a ceil (gametexture.cpp
		// GetFromTexture), so the quotient is aimed half a unit inside the
		// integer wanted and the ceil lands on it.
		int across = texelLength;
		if (across <= 0) across = (int)floor(f.length + 0.5);
		e.xScale = across >= 1 ? (double)dimU / (across - 0.5) : 1.;
		e.yScale = (double)dimV / (p.span - 0.5);
		e.mirrored = p.mirror;
		e.yOffset = (p.anchor - ref) * e.yScale;
		return e;
	}

	e.xScale = e.yScale = p.sv;        // 0.5, or 1 under HALF_PIXEL: both exact in the engine
	// The column mask is a plain AND applied to each column afresh (4733-4737),
	// so on a non power of two it is a fixed scramble of the image's columns
	// (measured: STUDY1 face 233, 6 columns, mask 5 -> 0 1 0 1 4 5 4 5 0 1 0 1)
	// and only a baked image shows it. The rows repeat (WrapRow), which the
	// engine does by itself.
	e.bakeU = p.uMasked && !Pow2(dimU);
	e.bakeV = false;
	if (f.twoSided && tier == WALL_MID)
	{
		// The engine draws ONE copy of a two-sided mid piece, from its row 0,
		// clipped to the opening only. That is the original's picture when the
		// piece does not tile, starts at the image's top (or the rows above
		// the start are cut off by the opening) and ends at the image's bottom
		// (or at the opening's).
		const int openTop = IMin(f.ownCeil, f.nbrCeil), openBot = IMax(f.ownFloor, f.nbrFloor);
		const double rowsLeft = dimV - p.v0;
		const bool exact = !p.vMasked
			&& (p.v0 == 0. || p.anchor >= openTop)
			&& (p.anchor - p.span <= openBot || rowsLeft == p.sv * p.span);
		if (!exact) e.bakeV = true;
	}
	e.baked = e.bakeU || e.bakeV;
	e.mirrored = p.mirror && !e.baked;         // a baked image carries its own mirror
	e.xOffset = e.bakeU ? 0. : p.u0;
	e.yOffset = (e.bakeV ? 0. : p.v0) + (p.anchor - ref) * e.yScale;
	return e;
}

void WallBakeMaps(const WallPiece &p, const WallEngineSetup &e, int dimU, int dimV,
	uint16_t *colMap, int &cols, uint16_t *rowMap, int &rows, int maxEntries)
{
	cols = rows = 0;
	if (!p.drawn || !e.baked) return;
	const int nc = e.bakeU ? p.cols : dimU;
	const int nr = e.bakeV ? (int)ceil(p.sv * (p.anchor - p.bot) - 1e-7) : dimV;
	if (nc <= 0 || nr <= 0 || nc > maxEntries || nr > maxEntries) return;
	for (int j = 0; j < nc; j++)
	{
		int c = e.bakeU ? (int)p.u0 + j : j;
		if (p.mirror) c = dimU - 1 - c;
		if (e.bakeU && p.uMasked) c = (int)((uint16_t)c & (uint16_t)(dimU - 1));
		colMap[j] = (c >= 0 && c < dimU) ? (uint16_t)c : (uint16_t)0xFFFF;
	}
	for (int j = 0; j < nr; j++)
	{
		int r = e.bakeV ? (int)p.v0 + j : j;
		if (e.bakeV && p.vMasked) r = WrapRow(r, dimV);
		rowMap[j] = (r >= 0 && r < dimV) ? (uint16_t)r : (uint16_t)0xFFFF;
	}
	cols = nc; rows = nr;
}

} // namespace roth
