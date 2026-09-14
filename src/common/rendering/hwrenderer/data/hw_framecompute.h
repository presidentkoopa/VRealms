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

// [SMOKEVOLUME] This frame's smoke volume, decided on the CPU (hw_smokevolume.cpp).
struct SmokeVolumeFrame
{
	// A frame never runs more steps than this, so a hitch does not become a burst
	// (review S7). A bigger maptime jump drops the backlog.
	static constexpr int MAX_STEPS_PER_FRAME = 2;

	// The volume should exist: r_smoke is on AND something asked for smoke on this map
	// within the linger time -- a mod's EmitSmoke / CarveSmoke (13b) or the desk test
	// r_smoke_computetest. False frees the images (after the CPU side's linger).
	bool Active = false;

	// r_smoke_computetest: the step to run is 13a's minimal test step
	// (shaders/compute/smoke_test.comp) instead of the simulation (13b).
	bool ComputeTest = false;

	int Quality = SMOKE_QUALITY_DEFAULT;
	SmokeGridSpec Grid;

	// The grid box's minimum corner in whole cells: world position = cell x
	// Grid.CellSize. World-aligned, so both eyes and every frame agree on a cell.
	int OriginCell[3] = { 0, 0, 0 };

	// Simulation steps to run this frame, 0..MAX_STEPS_PER_FRAME: how far
	// Level->maptime advanced since the last frame that stepped.
	int Steps = 0;

	// Level->maptime this frame, and where the frame sits between the last two
	// steps. [13c] the march blends the pair's two states by TicFrac.
	int MapTime = 0;
	float TicFrac = 0.f;

	// Renewed on every map change and savegame load: the backend clears the volume's
	// contents (the old map's smoke is gone) but keeps the allocation.
	uint64_t LevelSerial = 0;
};

struct FrameComputeInput
{
	SmokeVolumeFrame Smoke;

	// [13d] the smoke light grid's per-frame data (r_smoke_light_quality) joins Smoke.
	// [LEVELFIELD] #8, [DEBRISPOOL] #9, [SURFACEDAMAGE] #17: their frame data goes here.
};
