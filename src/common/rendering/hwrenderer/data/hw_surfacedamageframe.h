/*
** hw_surfacedamageframe.h
**
** [SURFACEDAMAGE] The surface damage atlas's geometry, its GPU layouts, the maths its CPU side, its GPU side and main.fp
** share, and what the CPU side hands the GPU side each frame.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: lasting damage on walls and floors -- bullet holes, gouges, soot, scorch, hot metal that cools, wet
** patches -- that stays for the whole fight, with depth ("Engine docs/SURFACE_DAMAGE_PLAN.md" #17, "Engine docs/
** SURFACE_DAMAGE_17_IMPL_NOTES.md"). Vanilla's impact decals are quads capped at cl_maxdecals; this is a fixed amount of
** texture memory cut into tiles, one tile per painted 64-unit cell of a surface, drawn by main.fp as part of the surface.
**
** Three homes, the debris pool's arrangement:
**   - the CPU side (src/rendering/hwrenderer/hw_surfacedamage.cpp) drains the PaintSurfaceDamage queue, finds the
**     surfaces, allocates tiles, keeps the hash and the surface records, and fills SurfaceDamageFrame;
**   - the GPU side (vk_surfacedamage.cpp) owns the images and buffers and records the uploads, stamps and cooling;
**   - this header is what both agree on, with main.fp and the compute lumps. Every struct marked GPU is std430 with no
**     padding (static_asserts below; the SPIR-V readback in the notes' checks), and every function here that a shader
**     repeats is repeated token for token.
**
** Plain C++ (no engine headers), so a harness can compile it on its own.
**
** Presentation only: nothing here is ever read back into the playsim.
**
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>

// THE ATLAS. A surface (a wall part or a flat) is cut into CELL x CELL map-unit cells; a painted cell gets a TILE of
// TILE_INNER texels a side (TEXELS_PER_UNIT a map unit) inside a SLOT with a GUTTER on every side, so bilinear filtering
// at the tile's edge reads its own gutter, never the next tile. A PAGE is one layer of a 2D array image holding
// TILES_PER_ROW x TILES_PER_ROW slots, with MIP_LEVELS levels (level 1's gutter is one texel, which still holds).
inline constexpr int SURFACE_DAMAGE_CELL_UNITS = 64;
inline constexpr int SURFACE_DAMAGE_TEXELS_PER_UNIT = 2;
inline constexpr int SURFACE_DAMAGE_TILE_INNER = SURFACE_DAMAGE_CELL_UNITS * SURFACE_DAMAGE_TEXELS_PER_UNIT;	// 128
inline constexpr int SURFACE_DAMAGE_GUTTER = 2;
inline constexpr int SURFACE_DAMAGE_SLOT = SURFACE_DAMAGE_TILE_INNER + 2 * SURFACE_DAMAGE_GUTTER;			// 132
inline constexpr int SURFACE_DAMAGE_PAGE = 2048;
inline constexpr int SURFACE_DAMAGE_TILES_PER_ROW = SURFACE_DAMAGE_PAGE / SURFACE_DAMAGE_SLOT;				// 15
inline constexpr int SURFACE_DAMAGE_TILES_PER_PAGE = SURFACE_DAMAGE_TILES_PER_ROW * SURFACE_DAMAGE_TILES_PER_ROW;	// 225
inline constexpr int SURFACE_DAMAGE_MIP_LEVELS = 2;

// "Damage memory" (r_damage_memory), owner answer 10: 64, 128, 256 (default) or 512 MB of base-level pages, 16 MiB a page:
// 4 / 8 / 16 / 32 pages, 900 / 1,800 / 3,600 / 7,200 tiles. The mip level adds a quarter on the GPU.
inline constexpr int SURFACE_DAMAGE_MEMORY_DEFAULT = 256;
inline constexpr int SURFACE_DAMAGE_PAGE_MEGABYTES = 16;
inline constexpr int SURFACE_DAMAGE_PAGES_MIN = 4;
inline constexpr int SURFACE_DAMAGE_PAGES_MAX = 32;

// A megabyte figure snapped to one of the four steps (a value between two steps goes down to the lower one).
inline int SurfaceDamageMemorySnap(int megabytes)
{
	int snapped = SURFACE_DAMAGE_PAGES_MIN * SURFACE_DAMAGE_PAGE_MEGABYTES;
	while (snapped < SURFACE_DAMAGE_PAGES_MAX * SURFACE_DAMAGE_PAGE_MEGABYTES && snapped * 2 <= megabytes)
		snapped *= 2;
	return snapped;
}

inline int SurfaceDamagePagesFor(int megabytes)
{
	return SurfaceDamageMemorySnap(megabytes) / SURFACE_DAMAGE_PAGE_MEGABYTES;
}

// Bytes of one page on the GPU, both levels.
inline constexpr uint64_t SURFACE_DAMAGE_PAGE_BYTES = (uint64_t)SURFACE_DAMAGE_PAGE * SURFACE_DAMAGE_PAGE * 4 + (uint64_t)(SURFACE_DAMAGE_PAGE / 2) * (SURFACE_DAMAGE_PAGE / 2) * 4;

// THE HASH the fragment shader looks a cell's tile up in: open addressing, SURFACE_DAMAGE_HASH_PROBES linear probes from
// SurfaceDamageHash & (entries - 1), sized to the next power of two at or above SURFACE_DAMAGE_HASH_SPARE x the tiles, so an
// insert almost always finds a free entry among its probes; when none is free, the least recently painted of the probed tiles
// is evicted. The notes' mirror measured, with every tile in use and random cells churning: spare 4, 6-7 such evictions per
// 1,000 new tiles; spare 8, under 2 -- for 8,192 .. 65,536 entries (at most 1 MB), and the shader still reads four.
inline constexpr int SURFACE_DAMAGE_HASH_PROBES = 4;
inline constexpr int SURFACE_DAMAGE_HASH_SPARE = 8;

inline int SurfaceDamageHashEntriesFor(int tiles)
{
	int entries = 1024;
	while (entries < tiles * SURFACE_DAMAGE_HASH_SPARE)
		entries *= 2;
	return entries;
}

// Integer mixing of a surface slot and a cell. main.fp repeats this function token for token (uint arithmetic wraps the
// same way; a negative cell's bits are kept by the conversion in both languages).
inline uint32_t SurfaceDamageHash(uint32_t slot, int32_t cellU, int32_t cellV)
{
	uint32_t h = slot * 0x9E3779B1u;
	h ^= (uint32_t)cellU * 0x85EBCA77u;
	h = (h ^ (h >> 15)) * 0x2C1B3C6Du;
	h ^= (uint32_t)cellV * 0xC2B2AE3Du;
	h = (h ^ (h >> 12)) * 0x297A2D39u;
	return h ^ (h >> 15);
}

// A tile's page and the texel of its slot's corner in that page (level 0). main.fp and the compute lumps repeat it.
inline void SurfaceDamageTileOrigin(int tile, int& page, int& x, int& y)
{
	page = tile / SURFACE_DAMAGE_TILES_PER_PAGE;
	const int slot = tile % SURFACE_DAMAGE_TILES_PER_PAGE;
	x = (slot % SURFACE_DAMAGE_TILES_PER_ROW) * SURFACE_DAMAGE_SLOT;
	y = (slot / SURFACE_DAMAGE_TILES_PER_ROW) * SURFACE_DAMAGE_SLOT;
}

// The cell a surface coordinate (map units) lies in.
inline int32_t SurfaceDamageCellOf(double coordinate)
{
	return (int32_t)std::floor(coordinate / SURFACE_DAMAGE_CELL_UNITS);
}

// A deterministic hash of a position (a quarter-unit grid) and a salt, for everything a paint picks that must not repeat
// and must not use RNG: its brush variant, a hashed turn, a hashed flip. Every machine in a netgame computes the same pick
// from the same paint call.
inline uint32_t SurfaceDamagePlaceHash(double x, double y, double z, uint32_t salt)
{
	const int32_t qx = (int32_t)std::floor(x * 4.0);
	const int32_t qy = (int32_t)std::floor(y * 4.0);
	const int32_t qz = (int32_t)std::floor(z * 4.0);
	uint32_t h = salt * 0x27D4EB2Du;
	h ^= (uint32_t)qx * 0x9E3779B1u;
	h = (h ^ (h >> 15)) * 0x85EBCA77u;
	h ^= (uint32_t)qy * 0xC2B2AE3Du;
	h = (h ^ (h >> 13)) * 0x2C1B3C6Du;
	h ^= (uint32_t)qz * 0x165667B1u;
	h = (h ^ (h >> 16)) * 0x297A2D39u;
	return h ^ (h >> 15);
}

// THE DATA BUFFER (set 1 binding 13, SurfaceDamageSSO in main.fp): one runtime array of vec4, indexed in vec4s:
//   [SCALES]      x soot scale, y depth scale, z heat scale (0 with r_damage_heat off), w 1 = "Show damage tiles"
//   [LAYOUT]      x the hash's entry count, yzw 0
//   [LOOKS]       SURFACE_DAMAGE_LOOKS looks, 4 vec4 each:
//                   Rim     rgb the colour a hole's edge takes, a how strongly
//                   Inside  rgb what the inside of a hole multiplies the surface by, a how strongly
//                   Detail  x detail layer, y map units per repeat, z how much the detail breaks holes and rims up, w rim width
//                   Finish  x wet shine, y 0, z normal bend, w parallax depth (map units; 0 off)
//   [SURFACES]    SURFACE_DAMAGE_SURFACE_CAPACITY records, 2 vec4 each: U and V, the planes over a shader-space position
//                 (x, height, y) that give the surface's own u and v in map units
//   [HASH]        the hash's entries: x surface slot + 1 (0 = empty), y cell u, z cell v, w tile + look x 65536 -- every
//                 value an integer below 2^24, exact in a float
inline constexpr int SURFACE_DAMAGE_LOOKS = 16;
inline constexpr int SURFACE_DAMAGE_SURFACE_CAPACITY = 8192;
inline constexpr int SURFACE_DAMAGE_DATA_SCALES = 0;
inline constexpr int SURFACE_DAMAGE_DATA_LAYOUT = 1;
inline constexpr int SURFACE_DAMAGE_DATA_LOOKS = 2;
inline constexpr int SURFACE_DAMAGE_DATA_SURFACES = SURFACE_DAMAGE_DATA_LOOKS + SURFACE_DAMAGE_LOOKS * 4;				// 66
inline constexpr int SURFACE_DAMAGE_DATA_HASH = SURFACE_DAMAGE_DATA_SURFACES + SURFACE_DAMAGE_SURFACE_CAPACITY * 2;	// 16450
inline constexpr int SURFACE_DAMAGE_LOOK_SHIFT = 65536;

inline int SurfaceDamageDataVec4s(int hashEntries)
{
	return SURFACE_DAMAGE_DATA_HASH + hashEntries;
}

// WHAT ONE FRAME MAY HAND OVER. 128 paints a tic (SH4) x at most 16 cells a paint x at most 2 tics a frame.
inline constexpr int SURFACE_DAMAGE_MAX_CELLS_PER_PAINT = 16;
inline constexpr int SURFACE_DAMAGE_STAMPS_PER_FRAME = 4096;
inline constexpr int SURFACE_DAMAGE_TILES_PER_FRAME = 4096;
inline constexpr int SURFACE_DAMAGE_STAMPS_PER_TILE = 64;
inline constexpr double SURFACE_DAMAGE_RADIUS_MIN = 0.5;
inline constexpr double SURFACE_DAMAGE_RADIUS_MAX = 64.0;

// THE BRUSHES: every variant of every brush is one BRUSH_TEXELS square layer of an RGBA8 2D array with every mip level, in
// the engine's channels (r soot, g depth, b heat, a wet).
inline constexpr int SURFACE_DAMAGE_BRUSH_TEXELS = 128;
inline constexpr int SURFACE_DAMAGE_BRUSH_MIPS = 8;
inline constexpr int SURFACE_DAMAGE_BRUSH_LAYERS = 256;
inline constexpr int SURFACE_DAMAGE_BRUSH_VARIANTS = 16;

// THE DETAIL TEXTURES (fixed binding 8): DETAIL_TEXELS square layers with every mip level, tiling. Layer 0 is the engine's
// own grain; a look's own detail texture gets a layer of its own.
inline constexpr int SURFACE_DAMAGE_DETAIL_TEXELS = 256;
inline constexpr int SURFACE_DAMAGE_DETAIL_MIPS = 9;

// Bytes of one layer's whole mip chain of a square side.
inline size_t SurfaceDamageChainBytes(int side, int levels)
{
	size_t total = 0;
	for (int m = 0; m < levels; m++)
		total += (size_t)(side >> m) * (size_t)(side >> m) * 4;
	return total;
}

// COOLING: every COOL_TICS world tics, heat = max(heat x COOL_KEEP - COOL_SUB, 0) over the hot tiles. A tile stays hot for
// COOL_PASSES passes after its last heat stamp -- enough for full heat to reach 0 in RGBA8 (the notes' mirror).
inline constexpr int SURFACE_DAMAGE_COOL_TICS = 8;
inline constexpr float SURFACE_DAMAGE_COOL_KEEP = 0.84f;
inline constexpr float SURFACE_DAMAGE_COOL_SUB = 2.0f / 255.0f;
inline constexpr int SURFACE_DAMAGE_COOL_PASSES = 24;

// GPU: ONE STAMP, three vec4s, 48 bytes (damage_stamp.comp's SurfaceDamageStamp).
//   Place   xy the brush's centre in the slot's texels (the gutter included: a cell's u = 0 is texel GUTTER), zw its
//           rotation (cos, sin): the brush's +x axis in the slot's texel axes
//   Shape   x 1 / the brush's radius in texels, y the variant's layer, z 1 = mirrored across the brush's x axis, w the mask's LOD
//   Amount  x depth, y soot, z heat, w wet -- the paint's own 0..1 values
struct SurfaceDamageStampGpu
{
	float Place[4];
	float Shape[4];
	float Amount[4];
};

// GPU: ONE TILE OF A DISPATCH, an ivec4, 16 bytes (both compute lumps' SurfaceDamageTile).
//   Tile the tile's index; First, Count its stamps in the stamp buffer (0 for cooling and mips); Flags 1 = cleared first
struct SurfaceDamageTileGpu
{
	int32_t Tile;
	int32_t First;
	int32_t Count;
	int32_t Flags;
};

static_assert(sizeof(SurfaceDamageStampGpu) == 48 && offsetof(SurfaceDamageStampGpu, Shape) == 16 && offsetof(SurfaceDamageStampGpu, Amount) == 32,
	"SurfaceDamageStampGpu must be three vec4s -- see SurfaceDamageStamp in damage_stamp.comp");
static_assert(sizeof(SurfaceDamageTileGpu) == 16, "SurfaceDamageTileGpu must be an ivec4 -- see SurfaceDamageTile in the damage compute lumps");

inline constexpr int SURFACE_DAMAGE_TILE_CLEAR = 1;

// [SURFACEDAMAGE] THIS FRAME'S DAMAGE, decided on the CPU (hw_surfacedamage.cpp). The backend does it in this order: allocate
// or free; the brush and detail images when their generations moved; the data span; the stamps (each listed tile once,
// cleared first when flagged); the cooling; then the mip level of every tile stamped or cooled.
struct SurfaceDamageFrame
{
	// Raised by every PrepareFrame; the backend acts on a frame once.
	uint64_t Serial = 0;

	// The atlas should exist: Vulkan, r_damage, and damage painted on this map (or a paint within the linger). False frees it.
	bool Active = false;
	int Pages = SURFACE_DAMAGE_MEMORY_DEFAULT / SURFACE_DAMAGE_PAGE_MEGABYTES;
	int HashEntries = 0;

	// The data buffer's CPU copy (DataVec4s x 4 floats) and this frame's changed span [DirtyFirst, DirtyEnd) in vec4s.
	// DataGeneration moves when the CPU side starts again from nothing; the backend then uploads all of it.
	const float* Data = nullptr;
	int DataVec4s = 0;
	int DirtyFirst = 0;
	int DirtyEnd = 0;
	uint64_t DataGeneration = 0;

	// The stamps, and the tiles they write (each tile once, its stamps in [First, First + Count)).
	const SurfaceDamageStampGpu* Stamps = nullptr;
	int StampCount = 0;
	const SurfaceDamageTileGpu* StampTiles = nullptr;
	int StampTileCount = 0;

	// A cooling pass this frame over these tiles (0 = none), with its constants (several passes folded into one).
	const SurfaceDamageTileGpu* CoolTiles = nullptr;
	int CoolTileCount = 0;
	float CoolKeep = 1.0f;
	float CoolSub = 0.0f;

	// The brush atlas and the detail array, as whole mip chains layer by layer (level 0 of every layer, then level 1 of every
	// layer...), RGBA8. Uploaded when the generation differs from the backend's.
	const uint8_t* BrushPixels = nullptr;
	int BrushLayers = 0;
	uint64_t BrushGeneration = 0;
	const uint8_t* DetailPixels = nullptr;
	int DetailLayers = 0;
	uint64_t DetailGeneration = 0;
};

// [SURFACEDAMAGE] What the backend did, for the CPU side's next frame and the draw keys. Renderer-internal (written by
// vk_surfacedamage.cpp and vk_descriptorset.cpp, read by hw_surfacedamage.cpp); never the playsim. Bound: this frame's
// descriptor sets hold the real pages, detail and data (fixed bindings 7 and 8, set 1 binding 13), so a draw may carry a key.
struct SurfaceDamageBackendStatus
{
	bool Allocated = false;
	int Pages = 0;
	int RefusedPages = 0;		// a page count this device refused, until damage stops being asked for; 0 = none
	bool ProgramsFailed = false;	// the compute programs did not build: refused for the session
	uint64_t Epoch = 0;			// rises with every allocation
	bool Bound = false;
};

inline SurfaceDamageBackendStatus& SurfaceDamageStatus()
{
	static SurfaceDamageBackendStatus status;
	return status;
}

// Filled by SurfaceDamage::PrepareFrame right before RunFrameCompute; read by VkComputeManager::RunFrame (the debris pool's
// arrangement: this header stands without hw_framecompute.h).
inline SurfaceDamageFrame& SurfaceDamageFrameForBackend()
{
	static SurfaceDamageFrame frame;
	return frame;
}
