/*
** hw_screentiles.h
**
** [SCREENTILES] Screen tile lists: a post-process pass whose every texel loops a short list of screen-space items loops only
** the items its tile lists.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/EFFECTS_OPTIMIZATION_PLAN.md" E5; "Engine docs/EMISSIVE_TILES_E5_IMPL_NOTES.md".
**
** A pass that loops up to SCREEN_TILE_ITEMS_MAX items (volumes, lights, ...) at every texel splits its target into tiles of
** SCREEN_TILE_TEXELS x SCREEN_TILE_TEXELS texels:
**
**   1. The CPU gives each item a conservative bound in the pass's TexCoord (ScreenTexRect; ScreenRectOfSphere for a sphere) and
**      cuts it into a rectangle of tiles at the target's size (ScreenTileRectPack): one uint32 an item, a byte a corner.
**   2. PPScreenTileMask (hw_postprocess.h) draws shaders/pp/screentilemask.fp once, at tile resolution. Each texel of its texture
**      is one tile and holds a 32-bit mask: bit i is set when item i's rectangle contains the tile. One byte a channel of RGBA8.
**   3. The pass reads its texel's tile (texelFetch at gl_FragCoord.xy / SCREEN_TILE_TEXELS) and loops the set bits from the lowest:
**      the list's own order. A tile with no bit is skipped at once.
**
** WHY THE IMAGE IS THE SAME. An item that does not reach a texel adds nothing there, and each item's work at a texel does not
** depend on the others. So a texel that loops only the listed items gets the result of looping every item, bit for bit, as long
** as every item that can reach it is listed, in the same order. Listing is conservative:
**   - the bound is widened for what a float32 march can see past the item (ScreenTileReach);
**   - a sphere's radius is taken into view space by the view matrix's largest stretch, not by its largest column (ScreenViewStretch);
**   - an item whose bound cannot be placed (it reaches the eye's plane, or a number is not finite) takes every tile;
**   - a rectangle is widened by a texel on each side before it is cut into tiles.
** The E5 mirror checks it texel by texel, with mutations that must fail.
**
** TexCoord is gl_FragCoord.xy / the target's size in every backend's full-screen pass (the present triangle maps NDC -1 to UV 0,
** and both Vulkan's upper-left and GL's lower-left window origin put that corner at gl_FragCoord 0), so a bound in TexCoord and a
** tile found from gl_FragCoord name the same texels.
**
** Dependency-free (the standard library only), so the proofs compile this exact file.
**
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

inline constexpr int SCREEN_TILE_TEXELS = 16;			// a tile's side in the target's texels (screentilemask.fp and every reader)
inline constexpr int SCREEN_TILE_ITEMS_MAX = 32;		// items a mask lists, one bit each
inline constexpr int SCREEN_TILE_AXIS_MAX = 256;		// tiles an axis a packed rectangle can name (a byte a corner)
inline constexpr uint32_t SCREEN_TILE_RECT_NONE = 0x0000FFFFu;	// first tile (255, 255), last (0, 0): it contains no tile

// The widening for a sphere's tile bound (ScreenTileReach). A float32 ray/sphere test (middle^2 - |c|^2 + r^2 > 0) cancels two
// numbers near |c|^2, so rounding can count a ray up to about sqrt(10 ulps x |c|^2) = 1.1e-3 x |c| outside the sphere as a hit; the
// centre the march adds up and the eye's inverse matrix are floats too. These cover that several times over and cost far less
// than a tile.
inline constexpr double SCREEN_TILE_REACH_SHARE = 4e-3;		// of the eye-to-centre distance
inline constexpr double SCREEN_TILE_REACH_UNITS = 0.0625;	// map units, for spheres at the eye

// An item's bound on a pass's screen, in TexCoord (0..1 across the target). Whole: it may reach any texel. Lo and Hi start empty
// (1 above 0) so a union takes min and max over them. Finite: every corner the bound was made from was a finite number.
struct ScreenTexRect
{
	double Lo[2] = { 1.0, 1.0 };
	double Hi[2] = { 0.0, 0.0 };
	bool Whole = false;
	bool Finite = true;
};

// A sphere on the screen of a pass whose view ray is smokemarch.fp's -- ((ndc + offset) x tanHalfFov, -1) in view space, out
// through the inverse of view matrix vm -- as SetupEmissiveVolumes (hw_drawinfo.cpp) bounds its rectangle: the sphere's box in
// view space (r already grown by the view matrix's stretch -- ScreenViewStretch for a tile bound, the rectangle's own largest
// column length for the rectangle) seen from its nearest and farthest
// depth, through the ray's own mapping from a slope to TexCoord: ndc = slope / tanHalfFov - offset, TexCoord = (ndc + 1) / 2.
// wx, wy, wz: the centre in GL world axes. A sphere reaching 1e-3 in front of the eye is Whole. The same numbers in the same
// order as that function's loop, so a union of these is its rectangle bit for bit (the E5 mirror, M1).
inline ScreenTexRect ScreenRectOfSphere(const float *vm, double wx, double wy, double wz, double r, double tanX, double tanY, double offsetX, double offsetY)
{
	ScreenTexRect rect;
	const double vx = vm[0] * wx + vm[4] * wy + vm[8] * wz + vm[12];
	const double vy = vm[1] * wx + vm[5] * wy + vm[9] * wz + vm[13];
	const double vz = vm[2] * wx + vm[6] * wy + vm[10] * wz + vm[14];
	const double nearDepth = -vz - r, farDepth = -vz + r;
	if (!(nearDepth > 1e-3))
	{
		rect.Whole = true;
		return rect;
	}
	const double view[2] = { vx, vy };
	const double tan[2] = { tanX, tanY };
	const double offset[2] = { offsetX, offsetY };
	for (int k = 0; k < 2; k++)
	{
		const double slopes[4] = { (view[k] - r) / nearDepth, (view[k] - r) / farDepth, (view[k] + r) / nearDepth, (view[k] + r) / farDepth };
		for (double s : slopes)
		{
			const double coord = (s / tan[k] - offset[k] + 1.0) * 0.5;
			rect.Lo[k] = std::min(rect.Lo[k], coord);
			rect.Hi[k] = std::max(rect.Hi[k], coord);
			if (!std::isfinite(coord))
				rect.Finite = false;
		}
	}
	return rect;
}

// The radius a sphere's TILE bound uses: its own radius plus what a float32 march can see past it (the constants above).
// dx, dy, dz: the centre relative to the eye, map units.
inline double ScreenTileReach(double radius, double dx, double dy, double dz)
{
	return radius + SCREEN_TILE_REACH_SHARE * std::sqrt(dx * dx + dy * dy + dz * dz) + SCREEN_TILE_REACH_UNITS;
}

// The largest factor by which a view matrix stretches a world vector -- the largest singular value of its 3x3 -- so a world sphere
// of radius r fits a view-space sphere of radius r x this (ScreenRectOfSphere's r). A view matrix that carries a pixel stretch
// scales some directions by more than it scales any axis, so the largest column length (which is what SetupEmissiveVolumes' own
// rectangle uses, and only a lower bound) can leave a bound short of a texel the march reaches -- the E5 mirror finds it. This is
// the exact value, from the closed form for the largest eigenvalue of the symmetric 3x3 M^T M, with the column lengths as a floor
// and a relative margin for the solve's own rounding. Once an eye, never per item. A non-finite matrix gives a non-finite stretch,
// which makes the bound Whole (every tile).
inline double ScreenViewStretch(const float *vm)
{
	double a[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			a[i][j] = (double)vm[i * 4] * vm[j * 4] + (double)vm[i * 4 + 1] * vm[j * 4 + 1] + (double)vm[i * 4 + 2] * vm[j * 4 + 2];
	double most = std::max(std::sqrt(a[0][0]), std::max(std::sqrt(a[1][1]), std::sqrt(a[2][2])));	// the columns: the lower bound
	const double offDiagonal = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
	if (offDiagonal > 0.0)	// otherwise M^T M is diagonal and the columns are the singular values
	{
		const double q = (a[0][0] + a[1][1] + a[2][2]) / 3.0;
		const double p2 = (a[0][0] - q) * (a[0][0] - q) + (a[1][1] - q) * (a[1][1] - q) + (a[2][2] - q) * (a[2][2] - q) + 2.0 * offDiagonal;
		const double p = std::sqrt(p2 / 6.0);
		if (p > 0.0)
		{
			double b[3][3];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					b[i][j] = (a[i][j] - (i == j ? q : 0.0)) / p;
			const double det = b[0][0] * (b[1][1] * b[2][2] - b[1][2] * b[2][1]) - b[0][1] * (b[1][0] * b[2][2] - b[1][2] * b[2][0]) +
				b[0][2] * (b[1][0] * b[2][1] - b[1][1] * b[2][0]);
			const double largest = q + 2.0 * p * std::cos(std::acos(std::clamp(det * 0.5, -1.0, 1.0)) / 3.0);
			if (largest > 0.0)
				most = std::max(most, std::sqrt(largest));
		}
	}
	return most * (1.0 + 1e-6);
}

// Tiles along an axis of a target this many texels long.
inline int ScreenTileCount(int texels)
{
	return texels <= 0 ? 0 : (texels + SCREEN_TILE_TEXELS - 1) / SCREEN_TILE_TEXELS;
}

// Whether a target's tiles can be named by a packed rectangle (up to 4096 texels an axis). A pass that does not fit loops its
// whole list, as it would without tiles.
inline bool ScreenTilesFit(int width, int height)
{
	return width > 0 && height > 0 && ScreenTileCount(width) <= SCREEN_TILE_AXIS_MAX && ScreenTileCount(height) <= SCREEN_TILE_AXIS_MAX;
}

// An item's rectangle of tiles on a target of width x height texels (ScreenTilesFit), packed from the low byte up: first tile x,
// first tile y, last tile x, last tile y. Texel t's TexCoord is (t + 0.5) / size, so a bound [Lo, Hi] reaches the texels from
// Lo x size - 0.5 to Hi x size - 0.5; one more texel on each side covers the interpolated TexCoord's rounding. A Whole or
// non-finite bound takes every tile; a bound wholly off the target takes none (SCREEN_TILE_RECT_NONE).
inline uint32_t ScreenTileRectPack(const ScreenTexRect &rect, int width, int height)
{
	const int size[2] = { width, height };
	uint32_t first[2] = { 0, 0 }, last[2] = { 0, 0 };
	for (int k = 0; k < 2; k++)
	{
		const int tiles = ScreenTileCount(size[k]);
		if (tiles <= 0)
			return SCREEN_TILE_RECT_NONE;
		const uint32_t lastTile = (uint32_t)std::min(tiles, SCREEN_TILE_AXIS_MAX) - 1;
		if (rect.Whole || !rect.Finite)
		{
			first[k] = 0;
			last[k] = lastTile;
			continue;
		}
		const double extent = (double)size[k];
		const double lowTexel = std::floor(std::clamp(rect.Lo[k] * extent - 0.5, -4.0, extent + 4.0)) - 1.0;
		const double highTexel = std::ceil(std::clamp(rect.Hi[k] * extent - 0.5, -4.0, extent + 4.0)) + 1.0;
		if (highTexel < 0.0 || lowTexel > extent - 1.0 || highTexel < lowTexel)
			return SCREEN_TILE_RECT_NONE;
		first[k] = (uint32_t)std::max(lowTexel, 0.0) / SCREEN_TILE_TEXELS;
		last[k] = std::min((uint32_t)std::min(highTexel, extent - 1.0) / SCREEN_TILE_TEXELS, lastTile);
	}
	return first[0] | (first[1] << 8) | (last[0] << 16) | (last[1] << 24);
}

// Whether a packed rectangle contains tile (tx, ty): screentilemask.fp's test, on the CPU.
inline bool ScreenTileRectContains(uint32_t rect, int tx, int ty)
{
	const int x0 = (int)(rect & 255u), y0 = (int)((rect >> 8) & 255u), x1 = (int)((rect >> 16) & 255u), y1 = (int)(rect >> 24);
	return tx >= x0 && tx <= x1 && ty >= y0 && ty <= y1;
}

// What a set of packed rectangles lists on a grid of tiles, for the perf log: the tiles, the tiles listing at least one item, and
// the item-tile pairs listed (what the pass's texels loop, a tile's texels at a time).
struct ScreenTileCoverage
{
	int Tiles = 0;
	int Lit = 0;
	int Entries = 0;
};

inline ScreenTileCoverage ScreenTileCoverageOf(const uint32_t *rects, int count, int tilesX, int tilesY)
{
	ScreenTileCoverage c;
	tilesX = std::clamp(tilesX, 0, SCREEN_TILE_AXIS_MAX);
	tilesY = std::clamp(tilesY, 0, SCREEN_TILE_AXIS_MAX);
	c.Tiles = tilesX * tilesY;
	for (int ty = 0; ty < tilesY; ty++)
	{
		uint64_t row[4] = { 0, 0, 0, 0 };
		for (int i = 0; i < count; i++)
		{
			const uint32_t r = rects[i];
			const int x0 = (int)(r & 255u), y0 = (int)((r >> 8) & 255u), x1 = std::min((int)((r >> 16) & 255u), tilesX - 1), y1 = (int)(r >> 24);
			if (ty < y0 || ty > y1 || x0 > x1)
				continue;
			c.Entries += x1 - x0 + 1;
			for (int x = x0; x <= x1; x++)
				row[x >> 6] |= 1ull << (x & 63);
		}
		for (uint64_t bits : row)
		{
			while (bits != 0)
			{
				bits &= bits - 1;
				c.Lit++;
			}
		}
	}
	return c;
}
