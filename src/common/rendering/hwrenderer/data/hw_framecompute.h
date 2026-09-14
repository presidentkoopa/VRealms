/*
** hw_framecompute.h
**
** [COMPUTE] What the game side hands the renderer backend once per frame for its GPU
** compute work (DFrameBuffer::RunFrameCompute).
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: compute work (the smoke volume first -- "Engine docs/
** SMOKE_VOLUME_PLAN.md" #13) needs to know about the level -- where the eye is,
** whether the world clock advanced, whether the map changed -- but the common
** renderer and the Vulkan backend never see FLevelLocals. So the game side
** (PrepareFrameCompute, src/rendering/hwrenderer/hw_entrypoint.cpp) reads the
** level and fills this plain struct, and the backend acts on it. That also keeps
** the backend replaceable: a render rebuild re-implements RunFrameCompute and
** keeps everything that fills this.
**
** One member per compute client. The level field (#8), the debris pool (#9), the
** impulse field (#12) and surface damage (#17) add theirs beside Smoke.
**
** Presentation only: nothing here is ever read back into the playsim.
**
*/

#pragma once

#include <cstdint>

// [SMOKEVOLUME] One smoke grid quality (r_smoke_quality): cells per axis and the
// cell size in map units. Doom axes: x, y horizontal, z up.
struct SmokeGridSpec
{
	int SizeX = 0;
	int SizeY = 0;
	int SizeZ = 0;
	int CellSize = 0;

	uint64_t Cells() const { return (uint64_t)SizeX * (uint64_t)SizeY * (uint64_t)SizeZ; }
};

// Bytes per cell of the volume the backend allocates (the plan's table): density and
// heat RG16F as a ping-pong pair (2 x 4), velocity RGBA16F as a pair (2 x 8), and the
// R8 solid mask (1). The pair holds the last two simulation states, which is what the
// march blends by TicFrac for smoothing (13b/13c) -- nothing extra is stored for it.
// The light grid (13d) is a separate allocation.
inline constexpr int SMOKE_BYTES_PER_CELL = 25;

inline constexpr int SMOKE_QUALITY_MIN = 1;
inline constexpr int SMOKE_QUALITY_DEFAULT = 2;
inline constexpr int SMOKE_QUALITY_MAX = 4;

// Resolution and area together, per the owner's answer 1 (about 150 MB at default,
// finer and bigger than the plan's first 20 MB grid). Every size is a multiple of 8,
// the compute shaders' local size.
//   1: 192 x 192 x  64 at 10 units = 1920 x 1920 x  640   about  59 MB
//   2: 256 x 256 x  96 at  8 units = 2048 x 2048 x  768   about 157 MB (default)
//   3: 320 x 320 x 128 at  8 units = 2560 x 2560 x 1024   about 328 MB
//   4: 384 x 384 x 128 at  8 units = 3072 x 3072 x 1024   about 472 MB
inline SmokeGridSpec SmokeGridForQuality(int quality)
{
	static const SmokeGridSpec specs[SMOKE_QUALITY_MAX - SMOKE_QUALITY_MIN + 1] =
	{
		{ 192, 192,  64, 10 },
		{ 256, 256,  96,  8 },
		{ 320, 320, 128,  8 },
		{ 384, 384, 128,  8 },
	};
	if (quality < SMOKE_QUALITY_MIN) quality = SMOKE_QUALITY_MIN;
	if (quality > SMOKE_QUALITY_MAX) quality = SMOKE_QUALITY_MAX;
	return specs[quality - SMOKE_QUALITY_MIN];
}

// [SMOKEVOLUME] 13b: THE UNITS THE SIMULATION AND THE DRAWING SHARE.
//
// Density 1 at absorption 1 (FLevelLocals::SmokeLook) has this extinction: light
// through 64 map units of it keeps e^-1, about a third. 13c's march uses exactly this,
// so what EmitSmoke's documentation promises ("amount 1 is thick haze") stays true.
inline constexpr float SMOKE_EXTINCTION_PER_MAP_UNIT = 1.0f / 64.0f;

// The simulation's tiles: SMOKE_TILE_CELLS cells a side (the compute shaders' local
// size). Every grid size is a whole number of tiles, and the box only ever moves by
// whole tiles, so a tile always covers the same piece of the world. A tile with no
// smoke, heat or motion in or next to it is skipped by the step (smoke_tiles.comp).
inline constexpr int SMOKE_TILE_CELLS = 8;

// The most solid-mask bytes one frame copies in (SmokeMaskUpload): the CPU side stops its
// rasterisation there, and the backend's staging buffer is this big. 4 MiB is about 40
// mask tiles at the default quality, far more than the CPU budget does in a frame.
inline constexpr size_t SMOKE_MASK_UPLOAD_BYTES_PER_FRAME = size_t(4) << 20;

// Below these a cell counts as empty: the tile pass and the CPU's bounds use them.
inline constexpr float SMOKE_EMPTY_DENSITY = 1.0f / 1024.0f;
inline constexpr float SMOKE_EMPTY_HEAT = 1.0f / 256.0f;
inline constexpr float SMOKE_EMPTY_VELOCITY = 1.0f / 128.0f;	// cells per step

// [SMOKEVOLUME] One kernel the GPU applies to a box of cells at the start of a step,
// before advection (shaders/compute/smoke_inject.comp): an EmitSmoke, a CarveSmoke or
// a PushEffectImpulse, converted by the CPU side (hw_smokevolume.cpp). Positions are
// grid-relative CELLS -- cell (i, j, k) spans [i, i + 1) on each axis, so its centre is
// at i + 0.5 -- and speeds are cells per step. The shape is a capsule from Start to End
// (Start == End: a ball). A long capsule is split into several kernels whose regions
// are disjoint slabs, each still carrying the whole capsule, so no cell is changed
// twice and none is missed.
struct SmokeKernel
{
	enum : int32_t { EMIT = 0, CARVE = 1, IMPULSE = 2 };

	int32_t Kind = EMIT;
	int32_t Step = 0;				// which of this frame's steps it belongs to, 0..Steps-1
	int32_t RegionMin[3] = { 0, 0, 0 };	// first cell of the box it may touch, inside the grid
	int32_t RegionMax[3] = { 0, 0, 0 };	// one past the last
	float Start[3] = { 0, 0, 0 };
	float End[3] = { 0, 0, 0 };
	float Radius = 0;				// cells; never below SmokeKernel::MIN_RADIUS
	float Amount = 0;				// EMIT: density added at the centre; CARVE: share removed at the centre, 0..1
	float Heat = 0;					// EMIT: heat added at the centre
	float Strength = 0;				// IMPULSE: cells per step outward at the centre (negative: inward)
	float Velocity[3] = { 0, 0, 0 };	// EMIT: cells per step added at the centre

	// A kernel thinner than this would fall between cell centres. It is widened to it,
	// and an emit's amount (a carve's) is scaled by the volume (cross-section) it lost,
	// so a tiny puff adds a tiny amount rather than a full cell of smoke.
	static constexpr float MIN_RADIUS = 0.75f;
};

// [SMOKEVOLUME] One block of solid-mask texels to copy into the mask volume: Size[0] x
// Size[1] x Size[2] bytes at MaskBytes + Offset, x fastest, then y, then z (a 3D
// image's texel order), 255 solid / 0 open, landing with its first texel at grid cell
// Min. Rasterised by SH1's LevelSolidity on the CPU.
struct SmokeMaskUpload
{
	int32_t Min[3] = { 0, 0, 0 };
	int32_t Size[3] = { 0, 0, 0 };
	size_t Offset = 0;
};

// [SMOKEVOLUME] The simulation's numbers for this frame's steps, from the level's smoke
// look, its wind and the renderer-read cvars (hw_smokevolume.cpp). Per STEP (one world
// tic), in cells.
struct SmokeSimSettings
{
	float DensityKeep = 1;			// density multiplier per step (dissipation)
	float HeatKeep = 1;				// heat multiplier per step (cooling)
	float VelocityKeep = 1;			// velocity multiplier per step (damping)
	float Diffusion = 0;			// share blended toward the neighbourhood per step, 0..1
	float HeatLift = 0;				// cells per step added to upward velocity per step, per unit of heat
	float DensityLift = 0;			// the same per unit of density (counted up to 1)
	float Wind[3] = { 0, 0, 0 };	// cells per step every cell drifts, walls permitting
	float Turbulence = 0;			// cells per step of swirl (divergence-free noise over the world)
	float TurbulenceFrequency = 0;	// swirl noise cells per grid cell
	float MaxDisplacement = 4;		// a step never moves anything further than this, cells
	float MaxSpeed = 4;				// the velocity field is clamped to this, cells per step
};

// [SMOKEVOLUME] This frame's smoke volume, decided on the CPU (hw_smokevolume.cpp).
struct SmokeVolumeFrame
{
	// A frame never runs more steps than this, so a hitch does not become a burst
	// (review S7). A bigger maptime jump drops the backlog.
	static constexpr int MAX_STEPS_PER_FRAME = 2;

	// The volume should exist: r_smoke is on AND something asked for smoke on this map
	// within the linger time, or smoke is still in the air -- a mod's EmitSmoke /
	// CarveSmoke, or the test source r_smoke_computetest. False frees the images.
	bool Active = false;

	int Quality = SMOKE_QUALITY_DEFAULT;
	SmokeGridSpec Grid;

	// The grid box's minimum corner in whole cells: world position = cell x
	// Grid.CellSize. World-aligned, always a multiple of SMOKE_TILE_CELLS, so both eyes,
	// every frame and every tile agree on a cell.
	int OriginCell[3] = { 0, 0, 0 };

	// Simulation steps this frame, 0..MAX_STEPS_PER_FRAME: how far Level->maptime
	// advanced since the last frame that stepped.
	int Steps = 0;

	// Level->maptime this frame, and where the frame sits between the last two
	// steps. [13c] the march blends the pair's two states by TicFrac.
	int MapTime = 0;
	float TicFrac = 0.f;

	// Renewed on every map change and savegame load: the backend clears the volume's
	// contents (the old map's smoke is gone) but keeps the allocation.
	uint64_t LevelSerial = 0;

	// ---- [13b] the simulation. The backend does these in this order. ----

	// 1. A NEW BOX (first placement, quality change, new map, a jump too far to shift, or
	//    the backend reported a mask it did not initialise for BoxEpoch): density, heat,
	//    velocity and the tile maps are zeroed and the mask is set ALL SOLID -- nothing
	//    flows until the CPU's rasterisation opens it, tile by tile. BoxEpoch rises with
	//    every new box; the backend reports it back (SmokeVolumeBackendStatus::MaskEpoch).
	bool NewBox = false;
	uint64_t BoxEpoch = 0;

	// 2. A RECENTRE: new origin minus old, whole tiles on each axis. The contents (smoke,
	//    velocity, mask) move so each cell keeps its world position; what comes into the
	//    box is empty air over a solid mask until rasterised.
	int ShiftCells[3] = { 0, 0, 0 };

	// 3. Empty density, heat and velocity (ClearSmoke, or the volume has gone quiet).
	bool ClearContents = false;

	// 4. Copy these mask blocks in.
	const SmokeMaskUpload* MaskUploads = nullptr;
	int MaskUploadCount = 0;
	const uint8_t* MaskBytes = nullptr;
	size_t MaskByteCount = 0;

	// 5. Steps simulation steps when Simulate (false: the CPU's bounds say nothing is left
	//    that could show or move, so no step is recorded). Step i applies the kernels
	//    whose Step is i, then advects, then rebuilds the tile maps.
	bool Simulate = false;
	const SmokeKernel* Kernels = nullptr;
	int KernelCount = 0;
	SmokeSimSettings Sim;

	// [13c] Visible smoke may exist (the CPU's density bound is above empty): draw it.
	bool HasSmoke = false;
};

// [SMOKEVOLUME] What the backend did with the smoke volume, for the CPU side's NEXT
// frame (hw_smokevolume.cpp). Renderer-internal, written by the backend
// (vk_smokevolume.cpp) and read by the game-side renderer code; nothing here ever
// reaches the playsim. It is how the CPU side learns that the volume was allocated
// again underneath it (a device reset, a late allocation) and its mask must be built
// again, or that this device refused a quality.
struct SmokeVolumeBackendStatus
{
	bool Allocated = false;
	int Quality = 0;
	int RefusedQuality = 0;		// a quality this device refused until smoke stops being asked for; 0 = none
	uint64_t MaskEpoch = 0;		// the BoxEpoch of the NewBox that initialised the current mask; 0 = none did
};

inline SmokeVolumeBackendStatus& SmokeVolumeStatus()
{
	static SmokeVolumeBackendStatus status;
	return status;
}

//-----------------------------------------------------------------------------
//
// [LEVELFIELD] #8 PARTICLE COLLISION WITH THE WHOLE LEVEL ("Engine docs/
// COLLISION_DEBRIS_MESH_PLAN.md" 8, "Engine docs/COLLISION_8_IMPL_NOTES.md").
//
// A signed distance field around the eye that particles sample: negative inside
// solid, in map units, clamped to a band. Two levels -- a fine one close by, a
// coarse one further out. The CPU side (hw_levelfield.cpp) decides where each
// level's window is, which tiles need (re)baking, and writes each tile's column
// part from SH1's solidity (sign) and the CURRENT floor and ceiling heights; the
// backend (vk_levelfield.cpp, shaders/compute/field_bake.comp) copies that in and
// takes the minimum with every nearby line's wall pieces, also from the current
// planes. Nothing that can move comes from the load-time level mesh (review D1).
//
// TOROIDAL ADDRESSING. A texel holds the world cell congruent to it modulo the
// volume's size (every size is whole tiles). A window move shifts nothing: the
// tiles whose world meaning changed are invalidated (g = 0) and baked again. A
// sample is trusted only inside its level's window and where g says baked.
//
// THE HEADER. Each frame's windows travel with the field as a tiny image written
// by a GPU command in the same command stream as the invalidations and bakes, so
// every draw -- whenever it was recorded -- reads a header and texels that agree.
// gpuparticles.vp reads all three through the fixed set (vk_descriptorset.cpp,
// bindings 5, 6 and 9).
//
// Presentation only: nothing here is read back into the playsim.
//
//-----------------------------------------------------------------------------

inline constexpr int LEVEL_FIELD_LEVELS = 2;			// 0 fine, 1 coarse
inline constexpr int LEVEL_FIELD_TILE_CELLS = 16;		// a tile is this many cells a side; every size is whole tiles
inline constexpr int LEVEL_FIELD_TILE_TEXELS = LEVEL_FIELD_TILE_CELLS * LEVEL_FIELD_TILE_CELLS * LEVEL_FIELD_TILE_CELLS;
inline constexpr int LEVEL_FIELD_BYTES_PER_CELL = 4;	// RG16F: r signed distance, g 1 = baked
inline constexpr int LEVEL_FIELD_HEADER_TEXELS = 3;		// the header image is 3 x 1, RGBA32F

inline constexpr int LEVEL_FIELD_QUALITY_MIN = 1;
inline constexpr int LEVEL_FIELD_QUALITY_DEFAULT = 2;
inline constexpr int LEVEL_FIELD_QUALITY_MAX = 3;

// One level of the field at one quality (r_particlecollision_quality). Doom axes: x, y
// horizontal, z up.
struct LevelFieldSpec
{
	int SizeX = 0;
	int SizeY = 0;
	int SizeZ = 0;
	int CellSize = 0;		// map units
	int BandCells = 0;		// distances are exact up to this many cells; further is clamped to it

	uint64_t Cells() const { return (uint64_t)SizeX * (uint64_t)SizeY * (uint64_t)SizeZ; }
	double Band() const { return (double)BandCells * (double)CellSize; }
};

// The cell sizes and bands are the same at every quality (a spark rests as precisely
// at 1 as at 3); the quality buys area. RG16F, 4 bytes a cell:
//   1: fine 192 x 192 x  64 at 4 (768 x 768 x 256),    coarse 128 x 128 x 48 at 16 (2048 x 2048 x 768)    12.6 MB
//   2: fine 256 x 256 x  96 at 4 (1024 x 1024 x 384),  coarse 192 x 192 x 64 at 16 (3072 x 3072 x 1024)   34.6 MB (default)
//   3: fine 384 x 384 x 128 at 4 (1536 x 1536 x 512),  coarse 256 x 256 x 96 at 16 (4096 x 4096 x 1536)  100.7 MB
inline LevelFieldSpec LevelFieldSpecFor(int quality, int level)
{
	static const LevelFieldSpec specs[LEVEL_FIELD_QUALITY_MAX - LEVEL_FIELD_QUALITY_MIN + 1][LEVEL_FIELD_LEVELS] =
	{
		{ { 192, 192,  64,  4, 8 }, { 128, 128, 48, 16, 6 } },
		{ { 256, 256,  96,  4, 8 }, { 192, 192, 64, 16, 6 } },
		{ { 384, 384, 128,  4, 8 }, { 256, 256, 96, 16, 6 } },
	};
	if (quality < LEVEL_FIELD_QUALITY_MIN) quality = LEVEL_FIELD_QUALITY_MIN;
	if (quality > LEVEL_FIELD_QUALITY_MAX) quality = LEVEL_FIELD_QUALITY_MAX;
	if (level < 0) level = 0;
	if (level >= LEVEL_FIELD_LEVELS) level = LEVEL_FIELD_LEVELS - 1;
	return specs[quality - LEVEL_FIELD_QUALITY_MIN][level];
}

// What one frame may hand over: tiles baked (their column parts are staged as texels,
// 1 MiB at 64 tiles), line records (48 bytes each), invalidation boxes.
inline constexpr int LEVEL_FIELD_TILES_PER_FRAME = 64;
inline constexpr int LEVEL_FIELD_LINES_PER_FRAME = 16384;
inline constexpr int LEVEL_FIELD_BOXES_PER_FRAME = 512;

// One line near a tile being baked, std430 with no padding -- must match FieldLine in
// shaders/compute/field_bake.comp. Its WALL PIECES are where exactly one side of it is
// open: a one-sided line's wall from its sector's floor to its ceiling; a two-sided
// line's lower part between the two floors and upper part between the two ceilings.
// Heights at both ends from the planes as they are this frame, linear along the line.
// A part whose top is not above its bottom is no part.
struct LevelFieldLine
{
	float Segment[4];		// x1, y1, x2, y2: map units, Doom axes
	float PartsStart[4];	// at (x1, y1): lower bottom, lower top, upper bottom, upper top
	float PartsEnd[4];		// the same at (x2, y2)
};

// One tile to bake this frame.
struct LevelFieldTile
{
	int32_t Level = 0;
	int32_t TexelTile[3] = { 0, 0, 0 };	// which tile of the volume
	int32_t WorldCell[3] = { 0, 0, 0 };	// the world cell its first texel holds
	int32_t LineFirst = 0;				// its lines: Lines[LineFirst .. LineFirst + LineCount)
	int32_t LineCount = 0;
	// LEVEL_FIELD_TILE_TEXELS floats at Distances + DistanceOffset, x fastest, then y, then
	// z (a 3D image's texel order): the column part, signed -- negative where the cell's
	// centre is solid -- and its magnitude the vertical distance to the column's nearest
	// floor or ceiling, clamped to the band.
	size_t DistanceOffset = 0;
};

// Texel tiles [TexelTileMin, TexelTileMax) of one level, not wrapping, to mark unbaked.
struct LevelFieldBox
{
	int32_t Level = 0;
	int32_t TexelTileMin[3] = { 0, 0, 0 };
	int32_t TexelTileMax[3] = { 0, 0, 0 };
};

// [LEVELFIELD] This frame's level field, decided on the CPU (hw_levelfield.cpp). The
// backend does it in this order: allocate or free, ClearLevel, Invalidate, the header,
// then the tiles (their column parts copied in, then the line pass).
struct LevelFieldFrame
{
	// The field should exist: r_particlecollision on, Vulkan, and a particle that collides
	// with the level was spawned on this map within the linger. False frees it.
	bool Active = false;
	int Quality = LEVEL_FIELD_QUALITY_DEFAULT;
	LevelFieldSpec Levels[LEVEL_FIELD_LEVELS];

	// Every texel of the level unbaked (a new window, a jump, a new map).
	bool ClearLevel[LEVEL_FIELD_LEVELS] = { false, false };

	const LevelFieldBox* Invalidate = nullptr;
	int InvalidateCount = 0;

	const LevelFieldTile* Tiles = nullptr;
	int TileCount = 0;
	const float* Distances = nullptr;
	size_t DistanceCount = 0;
	const LevelFieldLine* Lines = nullptr;
	int LineCount = 0;

	// The header image's texels (gpuparticles.vp):
	//   [0] the fine window: xyz its first world cell (Doom axes), w its cell size in map
	//       units (0 = no level)
	//   [1] the coarse window, the same
	//   [2] x 1 = the field may be used; y 1 = `collide = plane` definitions use it too
	//       (r_particlecollision_test); z the fine band, w the coarse band (map units)
	float Header[LEVEL_FIELD_HEADER_TEXELS][4] = {};
};

// [LEVELFIELD] What the backend did, for the CPU side's NEXT frame. Renderer-internal
// (written by vk_levelfield.cpp, read by hw_levelfield.cpp); never the playsim. Epoch
// rises with every allocation and every failure that emptied the volumes, so the CPU
// side knows its record of what is baked no longer holds.
struct LevelFieldBackendStatus
{
	bool Allocated = false;
	int Quality = 0;
	int RefusedQuality = 0;		// a quality this device refused until collision stops being asked for; 0 = none
	uint64_t Epoch = 0;
};

inline LevelFieldBackendStatus& LevelFieldStatus()
{
	static LevelFieldBackendStatus status;
	return status;
}

struct FrameComputeInput
{
	SmokeVolumeFrame Smoke;

	// [13d] the smoke light grid's per-frame data (r_smoke_light_quality) joins Smoke.
	// [DEBRISPOOL] #9, [SURFACEDAMAGE] #17: their frame data goes here.

	// [LEVELFIELD] #8, filled by LevelField::PrepareFrame (hw_levelfield.cpp).
	LevelFieldFrame LevelField;
};
