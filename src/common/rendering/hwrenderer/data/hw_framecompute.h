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

struct FrameComputeInput
{
	SmokeVolumeFrame Smoke;

	// [13d] the smoke light grid's per-frame data (r_smoke_light_quality) joins Smoke.
	// [LEVELFIELD] #8, [DEBRISPOOL] #9, [SURFACEDAMAGE] #17: their frame data goes here.
};
