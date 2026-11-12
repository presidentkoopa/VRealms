/*
** roth_surface.h
** THE ONE PLACE THAT ANSWERS "WHAT WOULD ROTH.C SAMPLE HERE".
**
** WHY THIS MODULE EXISTS
**
** The flat scale has been implemented five different ways in this codebase, in
** five places, with different numbers. Every fix so far has landed in one copy
** and not the others, so the level kept looking wrong in a way that made the
** fix look wrong too. The same is true of wall horizontal scale, the transpose
** rule, object facing and player height.
**
** That is the actual disease. The arguments about which VALUE is right were
** downstream of there being more than one place to keep it.
**
** So: every rule about how a surface is textured lives here, once, as a pure
** function of the map data. The loader, the runtime (moving platforms, texture
** change opcodes, doors) and the tests all call these. Nothing else computes a
** scale, an offset, a flip or an anchor.
**
** HOW IT IS PROVEN. roth_surface answers in ROTH's OWN terms -- a texture and a
** texel (col,row) -- not in engine terms. Each loader conversion then becomes a
** single checkable claim: "choose engine values such that the engine's (u,v)
** equals roth_surface's answer". The static texel test does exactly that
** comparison over every sector and wall piece in every map, without rendering.
**
** NO ENGINE TYPES. This header includes nothing from GZDoom on purpose. It is
** compiled into the standalone selftest as well as the engine, and staying
** engine-free is what keeps it testable without a level loaded.
**
** SOURCE OF THE RULES. ROTH_SURFACES_FIX.md, which traces each one from the
** VISIBLE pass (render_world_face_list, 0x2ad21) to the texel fetch. Rules
** taken from the cursor-pick pass (0x28dbe) or from the 3D-mesh face driver
** (0x3a84e) have already been wrong once each; the citations below name which
** pass they came from for that reason.
*/

#ifndef ROTH_SURFACE_H
#define ROTH_SURFACE_H

#include <stdint.h>

namespace roth
{

// ---------------------------------------------------------------------------
// What a stored texture word means.
//
// The test is THREE-WAY, not two (renderer.c:9222-9225, visible pass):
//
//     negative (0xFFxx)        -> a SOLID palette colour, index xx
//     equal to the colour key  -> draw NOTHING at all
//     otherwise                -> textured
//
// The middle case is the one this port did not have. The key is the DAS pack
// header word at +0x22 -- the same word this codebase used to call the "sky
// marker", which is why 6,208 surfaces across 36 maps were being painted with
// a sky texture instead of being left empty.
// ---------------------------------------------------------------------------
enum class SurfaceKind
{
	Textured,
	SolidColour,   // solidIndex carries the palette index
	Nothing,       // the colour key: the original draws no pixels here
};

struct SurfaceClass
{
	SurfaceKind kind;
	uint8_t     solidIndex;   // meaningful only when kind == SolidColour
};

SurfaceClass ClassifySurface(uint16_t textureWord, uint16_t colourKey);

// ---------------------------------------------------------------------------
// Flats: floors, ceilings and mid-platform surfaces.
//
// World-anchored, exactly as in Doom: the grid is global and must NOT become
// per-sector. What a surface adds on top is a scale, a shift and a mirror.
// ---------------------------------------------------------------------------
struct FlatSetup
{
	uint16_t textureWord;
	uint16_t texW, texH;      // the image's stored row length and height
	uint8_t  scaleBits;       // s, 0..3
	uint8_t  shiftX, shiftY;  // unsigned, in HALF-texel steps
	bool     mirrorX, mirrorY;
	bool     opaque256;       // a 256x256 opaque image: see the exception below
};

struct Texel
{
	bool ok;        // false when the surface draws nothing
	uint16_t col, row;
};

// The texel ROTH.C would sample at a world point.
//
//     col = ( -x / 2^s + shiftX/2 ) mod W
//     row = ( +y / 2^s - shiftY/2 ) mod H
//
// 2^s WORLD UNITS PER TEXEL, s = 0..3 -> 1, 2, 4, 8. Note the direction: this
// project shipped 2^(s+1) and, before that, argued for 2^(s-1) and 2^(s+2).
// The oracle now measures every textured flat in STUDY1 at 2 units per texel,
// with s = 1 on floors and s = 2 on the 256x256 ceiling.
//
// THE 256x256 OPAQUE EXCEPTION (renderer.c:3385-3393): those use 2^(s-1) units
// per texel, and one shift unit is a FULL texel rather than a half. Translucent
// 256x256 images keep the normal rule. This is what makes the s=2 ceiling land
// on 2 units per texel alongside the s=1 floors.
//
// THE TWO SIGNS (-x, +y) are MEASURED (oracle, 2026-10-01): ROTH.C was made to
// paint every texel with its own coordinates and the frame was decoded per
// pixel against the map. Every flat in view on STUDY1 and LRINTH1 fits this
// formula, including 256x256 opaque ceilings at s=1 and s=2 and mirrored floors
// on both axes.
Texel FlatTexel(const FlatSetup &s, int32_t worldX, int32_t worldY, uint16_t colourKey);

// World units per texel for a flat. Split out because the loader needs it on
// its own to choose an engine scale.
double FlatUnitsPerTexel(const FlatSetup &s);

// ---------------------------------------------------------------------------
// The engine encoding.
//
// This is the seam the whole module exists for. FlatTexel says what ROTH.C
// samples; FlatToEngine says which GZDoom sector-plane values make GZDoom
// sample the same thing. The loader and the runtime both call THIS and do
// nothing but push the four numbers into the sector -- so they cannot drift
// apart again, which they had (roth_runtime.cpp computed the same scale as the
// loader but silently dropped the mirrors, so a platform changed appearance the
// moment it moved).
//
// The target convention is read out of the engine, not assumed:
//
//     base UV      u0 = x/64,  v0 = -y/64      hw_vertexbuilder.cpp:176-177
//     texture matrix  scale(S1) * translate(T) * scale(64/W, 64/H)
//                                               hw_flats.cpp:86-94
//
// which composes to
//
//     texelCol = Scale.X * ( worldX + Offs.X)
//     texelRow = Scale.Y * (-worldY + Offs.Y)
//
// Offsets are therefore in WORLD units and are scaled afterwards; getting that
// backwards silently halves or doubles every shift.
//
// THE QUARTER TURN (measured 2026-10-01, ROTH.C oracle). roth_texture.cpp
// registers every world texture -- flats included -- with its dimensions
// exchanged and its bytes declared column-major, so the GZDoom texture's u axis
// runs along the image's STORED ROWS and its v axis along STORED COLUMNS. ROTH
// samples a flat in stored layout with the row on +y and the column on -x. The
// two only line up if the plane is turned a quarter: angle 270 makes the
// engine's u follow world +y and its v follow world +x, and the scales/offsets
// are then solved against THOSE axes. Without the turn every flat in the game
// is drawn transposed (rotated 90 degrees and mirrored), which no encoding test
// that models the engine without the quarter turn can see.
//
// NOTE what is proven and what is not. The static test proves this ENCODING is
// faithful -- that the engine, given these numbers, samples exactly what
// FlatTexel says. Whether FlatTexel's own two signs are right is a separate
// question -- ANSWERED 2026-10-01: the oracle (ROTH.C painting texel codes,
// read back per pixel) confirms -x / +y, the 2^s scale, the 256x256 opaque
// exception, the half-texel shift and both mirrors including their shift, on
// STUDY1 and LRINTH1. See REMAROTH_ORACLE_RESULTS.md.
// ---------------------------------------------------------------------------
struct FlatEngineSetup
{
	double xScale, yScale;     // sector_t::SetXScale / SetYScale
	double xOffset, yOffset;   // sector_t::SetXOffset / SetYOffset, world units
	double angle;              // sector_t::SetAngle, DEGREES. Always 270: see below.
};

FlatEngineSetup FlatToEngine(const FlatSetup &s);

// What the engine will sample at a world point, given those values. Exists so
// the test can check the encoding without a level loaded, and so the round trip
// is written down once rather than re-derived at each call site.
Texel EngineTexel(const FlatEngineSetup &e, uint16_t texW, uint16_t texH,
	int32_t worldX, int32_t worldY);


// ---------------------------------------------------------------------------
// Walls.
//
// A face draws up to three PIECES: on a face with no neighbour, the whole
// sector height (the "mid"); on a face with a neighbour, an upper piece (own
// ceiling down to the neighbour's), a lower piece (the neighbour's floor down
// to own floor) and, when the texture map has TRANSPARENT, a see-through mid
// piece in the opening. All of it is ROTH.C renderer.c, visible pass:
//
//   9271-9279   per face: flags = map +8; an extended record (fit word bit 15)
//               gives start = 2*shiftX, row shift = shiftY, extent = word & 0xfff
//   7383-7390, 7484, 9280   EVERY face with a neighbour goes to
//               draw_world_face_clipped_spans
//   8767-8793   there: the upper and lower are built with flags & 0xfa
//               (TRANSPARENT and IMAGE_FIT cleared) whenever the mid overlay
//               follows -- the face is TRANSPARENT, or the neighbour has a
//               mid-platform the face shows the edge of -- and with the stored
//               flags otherwise
//   8658-8697   upper: own ceiling .. max(neighbour ceiling, own floor);
//               lower: min(neighbour floor, own ceiling) .. own floor;
//               each anchored at its OWN top (wrap_reoffset+0xa)
//   8171-8215, 8727-8742   see-through mid: min(ceilings) .. max(floors), cut
//               by the own sector's override byte under map flag 0x08
//   8270-8277   draw flag 0x100 is set exactly when IMAGE_FIT is clear
//   13334-13340 0x100: extents are the span's own -- the stored extent across,
//               the piece height down -- both doubled by HALF_PIXEL, in HALF
//               texels; the column mask stays dim-1 unless the run fits a copy
//   13342-13349 else (fit): shifts zeroed, extents 2*dim: one copy each way
//   4733-4737   column = ((a + start) >> 1); FLIP_X: ~column + dim; & mask
//   5050-5069   start row = shiftY; DRAW_FROM_BOTTOM: ((2*dim - extent) >> 1)
//               - shiftY, masked if negative; the WRAPPING column mapper (row &
//               (dim-1) on every pixel, 1929-1935) is chosen when the piece
//               runs past the image's last row
//
// "dimU" is the image's texel count ALONG the wall (stored height, block +0xe)
// and "dimV" the count DOWN it (stored row length, block +0xc).
//
// The masks are ANDs with dim-1. On a power of two that is a repeat. On
// anything else the COLUMN mask is a fixed scramble of the image's own columns,
// which WallTexel returns as the original draws it; the ROW mask is not a
// function of the row at all (see WrapRow in roth_surface.cpp) and is
// returned as a repeat.
// ---------------------------------------------------------------------------
enum WallTier { WALL_UPPER = 0, WALL_MID = 1, WALL_LOWER = 2 };   // side_t::top / mid / bottom

struct WallFace
{
	uint8_t flags = 0;          // texture map +8 (FF_*)
	bool    extended = false;   // fit word bit 15
	int     extent = 0;         // the stored extent: fit word, or its low 12 bits when extended
	int     shiftX = 0, shiftY = 0;   // extended records only; 0 otherwise
	double  length = 0;         // the face's measured length
	bool    twoSided = false;
	int     ownFloor = 0, ownCeil = 0, nbrFloor = 0, nbrCeil = 0;
	int     ownOverride = 0;    // own sector +0x0c, signed
	bool    overlayFollows = false;   // WallOverlayFollows()
};

// renderer.c:8767 and 8781-8790 with project_wall_face_span_extents (8585-8621).
// nbrPlat*/ownPlat*: the neighbour's / own sector's mid-platform underside and top
// (pass has = false where there is none).
bool WallOverlayFollows(uint8_t flags, bool midIsColourKey, int ownFloor, int ownCeil,
	bool nbrHasPlat, int nbrPlatUnder, int nbrPlatTop,
	bool ownHasPlat, int ownPlatUnder, int ownPlatTop);

struct WallPiece
{
	bool drawn = false;
	int  top = 0, bot = 0;      // what is drawn, world z
	int  anchor = 0;            // rows count down from here (the piece's own top)
	int  span = 0;              // the height ROTH.C lays the vertical extent over
	bool fit = false, mirror = false;
	double u0 = 0, su = 0;      // column = floor(u0 + su * distanceFromVertex1), then mirror, then mask
	bool uMasked = false;
	double v0 = 0, sv = 0;      // row = floor(v0 + sv * (anchor - z)), then mask
	bool vMasked = false;
	int  cols = 0;              // texels laid across the face
};

WallPiece WallPieceRule(const WallFace &f, WallTier tier, int dimU, int dimV);

// The texel ROTH.C samples d units along the face from its first vertex at
// height z. ok is false where the piece draws nothing or the original reads
// outside the image.
Texel WallTexel(const WallPiece &p, int dimU, int dimV, double d, double z);

// ---------------------------------------------------------------------------
// The engine encoding of a wall piece.
//
// The engine (hw_walls.cpp SetWallCoordinates / DoTexture / DoMidTexture,
// gametexture.cpp GetFromTexture) samples
//
//     column = offsetX/|xScale| ... (xOffset + TexelLength * xScale * d / length) mod dimU
//     row    = yOffset + yScale * (reference - z)                             mod dimV
//
// where `reference` is the front ceiling for a one-sided wall and an upper
// piece (ML_DONTPEGTOP), the REAL back floor for a lower piece -- even where
// that is above the front ceiling and the piece starts lower -- and
// min(ceilings) for a two-sided mid piece, of which it draws ONE copy clipped
// to the opening.
//
// Two things the engine cannot do with scales and offsets, and what is done
// instead:
//
//   * A NEGATIVE xScale is not a mirror here. With the offset at 0 the run is
//     0 .. -1, which hw_walls.cpp:1585 takes for "inside one copy" and clamps:
//     the whole piece becomes one smeared column. FLIP_X therefore asks for
//     the X-mirrored registration of the image (`mirrored`) and keeps the
//     scale positive -- which is also what the original does: it mirrors
//     inside the image's own columns (4735).
//   * The non-power-of-two column mask, and a see-through mid piece that has
//     to tile, stop short of its image or start part-way down it. Those ask for a
//     BAKED registration: an image made of exactly the columns and rows the
//     original draws, in order (`baked`, colMap / rowMap from WallBakeMaps),
//     laid down once.
// ---------------------------------------------------------------------------
struct WallEngineSetup
{
	bool   drawn = false;
	double xScale = 1, yScale = 1, xOffset = 0, yOffset = 0;
	bool   mirrored = false;
	bool   baked = false;
	bool   bakeU = false, bakeV = false;
};

// side_t::TexelLength for the face's side: the stored extent, which is the
// number of half texels ROTH.C lays across the face. -1: leave the engine's.
int WallTexelLength(const WallFace &f);

WallEngineSetup WallToEngine(const WallFace &f, WallTier tier, const WallPiece &p,
	int dimU, int dimV, int texelLength);

// The baked image: entry i of colMap / rowMap is the source column / row for
// baked column / row i, or 0xFFFF for "outside the image".
void WallBakeMaps(const WallPiece &p, const WallEngineSetup &e, int dimU, int dimV,
	uint16_t *colMap, int &cols, uint16_t *rowMap, int &rows, int maxEntries);

} // namespace roth

#endif // ROTH_SURFACE_H
