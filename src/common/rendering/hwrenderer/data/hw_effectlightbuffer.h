/*
** hw_effectlightbuffer.h
**
** [EFFECTLIGHTS] The GPU side of effect lights: the record and bin layouts every
** consumer reads, and the two storage buffers that carry them.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: effect lights are short lights that belong to no actor -- a
** spark, an ember, an impact, a tracer ("Engine docs/LIGHTS_20_21_22_PLAN.md" 2,
** "Engine docs/EFFECT_LIGHTS_CORE_IMPL_NOTES.md"). The dynamic lights of today
** relink into section lists at tic rate and are chosen per surface; hundreds of
** lights that live a fraction of a second, and move at frame rate, need neither.
** The CPU side (hw_effectlights.cpp) moves them, sorts them into a grid of world
** bins around the eye once per frame, and hands the GPU this frame's lights and
** that grid. Both eyes and every consumer read the same two buffers:
**
**   - surfaces, models and sprites (main.fp, EFFECT_LIGHTS),
**   - GPU particles, mesh particles and debris (their vertex shaders),
**   - the smoke volume's light grid (compute).
**
** THE LAYOUTS ARE FROZEN HERE. A consumer keeps its GLSL copy identical to the
** structs below; the offline checks read its offsets back from SPIR-V.
**
** Vulkan only: made by VulkanRenderDevice::InitializeState beside the view light
** buffer (Instance() is null on GL and GLES); set 1 bindings 14 and 15
** (vk_descriptorset.cpp), vertex and fragment stages. Presentation only: nothing
** reads either buffer back.
**
*/

#pragma once

#include <cstdint>

class IDataBuffer;

// One light this frame: four vec4s, 64 bytes, std430 with no padding, SHADER axes (game x, z, y -- y is up).
//
//   a      xyz the segment's start this frame                 w radius, map units
//   b      xyz the segment's end this frame (== a: a point)   w flags, EFFECT_LIGHT_GPU_* (an exact integer)
//   color  rgb colour x intensity x brightness now            w shadow-map row 0..1023, or -1 for none
//   extra  xyz where that row was cast from                   w the brightness at b as a share of the brightness at a
//          (with EFFECT_LIGHT_GPU_ROW; 0 otherwise)             (1 = even along the segment; a point never reads it)
//
// At a position p a light is seen from c, the closest point on [a, b] to p: attenuation (radius - |p - c|) / radius, and
// N.L toward c while the light faces. With a == b, c is a, and that is material_normal.fp's point light exactly. The
// share along the segment of c, t (0 at a), scales the brightness by mix(1, extra.w, t).
struct EffectLightRecord
{
	float a[4];
	float b[4];
	float color[4];
	float extra[4];
};

enum
{
	EFFECT_LIGHT_GPU_FACING = 1,		// N.L toward the closest point: every light but a residual glow
	EFFECT_LIGHT_GPU_ROW = 2,			// walls block it: color.w is its shadow-map row, extra.xyz where the row was cast from
	EFFECT_LIGHT_GPU_RESIDUAL = 4,		// a crowded bin's merged glow (EffectLightCore::BinBuilder)
	EFFECT_LIGHT_GPU_NOSURFACES = 8,	// not on walls, flats, models or sprites
	EFFECT_LIGHT_GPU_NOSMOKE = 16,		// not in the smoke
	EFFECT_LIGHT_GPU_NOPARTICLES = 32,	// not on particles or debris
};

// Ahead of the records (binding 14). Count is how many records are valid; the rest are for the log and later readers.
struct EffectLightRecordHeader
{
	uint32_t Count;
	uint32_t Binned;		// of those, the lights; the others are residual glows
	uint32_t Residuals;
	uint32_t Serial;		// rises by one per upload
};

// Ahead of the bin data (binding 15). The grid is the OCCUPIED box of the world grid around the eye: every bin outside it
// is empty, so a position whose bin falls outside [0, Size) has no effect light.
//
//   Corner  xyz the lowest corner of bin (0, 0, 0), shader axes, map units   w the bin size, map units
//   Size    xyz bins along x, up and game y; 0 = nothing binned              w how many bins: the index list starts there
//
// Then data[(z * Size.y + y) * Size.x + x] = (offset << 6) | count for bin (x, y, z) -- count 0..32 -- and
// data[Size.w + offset + k] the record indices of that bin's lights.
struct EffectLightGridHeader
{
	float Corner[4];
	int32_t Size[4];
};

inline constexpr int EFFECT_LIGHT_BIN_COUNT_BITS = 6;
inline constexpr uint32_t EFFECT_LIGHT_BIN_COUNT_MASK = 63;
// The most lights one bin lists, and every consumer's loop bound: r_effectlights_perbin's maximum.
inline constexpr int EFFECT_LIGHT_BIN_LOOP_MAX = 32;
// r_effectlights_max's largest value: the pool of fire-and-forget lights.
inline constexpr int EFFECT_LIGHT_POOL_MAX = 4096;
// Drawn-line lights (SetDrawnLineLight) one frame binds.
inline constexpr int EFFECT_LIGHT_LINE_LIGHTS_MAX = 512;
// Records one frame uploads: every binned pool and drawn-line light, then residual glows in the room left.
inline constexpr int EFFECT_LIGHT_RECORD_CAPACITY = 8192;
// The largest grid (quality 3: 128 x 32 x 128 bins) and the index list.
inline constexpr int EFFECT_LIGHT_BIN_CAPACITY = 128 * 32 * 128;
inline constexpr int EFFECT_LIGHT_INDEX_CAPACITY = 262144;

class EffectLightBuffer
{
public:
	static const unsigned RECORD_BYTES = 64;
	static const unsigned RECORD_HEADER_BYTES = 16;
	static const unsigned GRID_HEADER_BYTES = 32;

	EffectLightBuffer();
	~EffectLightBuffer();

	// This frame's lights: `count` records (the first `binned` of them lights, the rest residual glows), and the grid with
	// `binCount` bin words then `indexCount` indices. Records, then bin data, then the two headers. A frame with nothing
	// binned (no records, or a grid of size 0) writes the empty headers once and nothing more while it stays empty.
	void Upload(const EffectLightRecord *records, unsigned count, unsigned binned, const EffectLightGridHeader &grid,
		const uint32_t *bins, unsigned binCount, const uint32_t *indices, unsigned indexCount);

	IDataBuffer *GetRecordBuffer() const { return mRecords; }
	IDataBuffer *GetBinBuffer() const { return mBins; }
	// Records on the GPU since the last upload: 0 while nothing is binned.
	unsigned GetLiveCount() const { return mLiveCount; }

	// The one instance: made by VulkanRenderDevice::InitializeState (Create), freed beside the view lights (Destroy). Null on
	// GL and GLES.
	static EffectLightBuffer *Instance();
	static void Create();
	static void Destroy();

private:
	IDataBuffer *mRecords = nullptr;
	IDataBuffer *mBins = nullptr;
	unsigned mLiveCount = 0;
	uint32_t mSerial = 0;
	bool mEmptyWritten = false;
};

// This frame's effect-light load for the performance log (hw_perflog.cpp: effectlights=...). Written by EffectLights'
// BeginFrame, AssignShadowRows and PrepareFrame (hw_effectlights.cpp) on the main thread; render-side only.
struct EffectLightFrameStats
{
	int Live = 0;		// pool lights alive, plus drawn-line lights on
	int Binned = 0;		// lights listed in at least one bin
	int Merged = 0;		// residual glows made in crowded bins
	int Evicted = 0;	// pool lights evicted, and new lights refused, because the pool was full
	int NoRow = 0;		// point lights not binned: walls block them and no shadow-map row was left
	int Trimmed = 0;	// lights not binned: the index list was full
	int Rows = 0;		// shadow-map rows effect lights took
	int Lines = 0;		// line lights binned
};

EffectLightFrameStats &EffectLightStats();
