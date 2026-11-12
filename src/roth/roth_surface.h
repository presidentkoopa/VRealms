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

} // namespace roth

#endif // ROTH_SURFACE_H
