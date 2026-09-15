/*
** hw_emissivevolumeframe.h
**
** [EMISSIVEVOLUMES] The FROZEN layouts of emissive volumes (#15): the list image's texels, the noise volume, the frame the
** renderer's CPU side hands the backend, what the backend reports back, and the per-frame load for the perf log.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/VOLUMETRIC_FLASH_15_PLAN.md" 2d and 6; "Engine docs/EMISSIVE_VOLUMES_15_IMPL_NOTES.md".
**
** AN EMISSIVE VOLUME is a short-lived glowing gas volume -- a ball, a tongue, a star of petals, a ring -- that a mod places
** in the world (LevelLocals.SpawnEmissiveVolume) and the renderer draws with a raymarch per eye (shaders/pp/
** emissivevolume.fp). Muzzle flashes first; explosion cores, plasma bursts and the BFG blast use the same thing.
**
** WHO WRITES WHAT.
**   hw_emissivevolumes.cpp (the renderer's CPU side, game axes in)  fills EmissiveVolumeFrame: this frame's list texels.
**   vk_emissivevolumes.cpp (the Vulkan backend)                        copies them into the list image, bakes the noise once,
**                                                                      and reports EmissiveVolumeBackendStatus.
**   hw_drawinfo.cpp SetupEmissiveVolumes (per eye)                    publishes the pass (PPEmissiveVolumes) only while the
**                                                                      backend holds exactly this frame's list.
**   shaders/pp/emissivevolume.fp and smokemarch.fp's SMOKE_CURVE_NEAR_VOLUMES read the image by these rows.
**
** Every number here is read by more than one of those; change it in all of them or not at all.
**
** PRESENTATION ONLY: nothing here is read back into the playsim.
**
*/

#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------------------------------------------------------
// The list image: EMISSIVE_VOLUMES_DRAWN_MAX columns (one a drawn volume, in draw order: FARTHEST from the head first) x
// EMISSIVE_VOLUME_TEXELS rows, RGBA32F. Positions and directions are GL WORLD AXES (map x, map z, map y); positions are map
// units from the frame's list origin, a whole-map-unit point near the eye (each eye's uniforms carry origin - eye).
// ---------------------------------------------------------------------------------------------------------------------------

inline constexpr int EMISSIVE_VOLUMES_DRAWN_MAX = 32;	// the most volumes drawn a frame (the rest are skipped that frame, not killed)
inline constexpr int EMISSIVE_VOLUME_TEXELS = 12;		// rows a volume takes

// The rows. Lengths are map units ALREADY SCALED by this frame's growth and the spawn's scale.
enum EEmissiveVolumeRow
{
	EVROW_BASE = 0,		// xyz the base point (the shape's root)                  w the bounding sphere's radius
	EVROW_AXIS = 1,		// xyz the unit axis (the way the gas leaves)             w the bounding sphere's centre, along the axis from the base
	EVROW_FRAME = 2,	// xyz the unit reference across the axis (petal 0)       w the body's length along the axis
	EVROW_FOLLOW = 3,	// xyz the root's follow displacement (trail motion)      w followtip: the share of it the tip keeps, 0..1
	EVROW_BODY = 4,		// x base radius  y tip radius  z softness 0..1  w body weight
	EVROW_PETALS = 5,	// x petal count  y petal length  z petal width  w petal weight
	EVROW_PETALS2 = 6,	// x cos spread  y sin spread  z petal jitter 0..1  w the axial extent the follow's lag runs over
	EVROW_RING = 7,		// x ring radius  y ring thickness  z ring weight  w heat falloff (temperature = heat now x density ^ w)
	EVROW_NOISE = 8,	// xyz the noise's offset (tiles)                         w noise amount 0..1
	EVROW_NOISE2 = 9,	// x noise cells per map unit  y octaves (1 or 2)  z billow distance (map units)  w churn phase (cells)
	EVROW_EMISSION = 10,	// rgb emission = tint x intensity x brightness x flicker x r_emissivevolumes_brightness / EMISSIVE_EMISSION_LENGTH
						// w extinction per map unit at density 1 = absorption x EMISSIVE_ABSORPTION_PER_MAP_UNIT
	EVROW_HEAT = 11,	// x core heat now 0..1 (the engine's fixed heat ramp)  y shape seed (24 bits, exact in a float)  z shape scale  w 0
};

// Density 1 over this many map units emits a volume's intensity once: a 24-unit core at density 1 and intensity 4 adds 4.
inline constexpr float EMISSIVE_EMISSION_LENGTH = 24.0f;

// Absorption 1 at density 1 hides e^-1 of what is behind over 16 map units.
inline constexpr float EMISSIVE_ABSORPTION_PER_MAP_UNIT = 1.0f / 16.0f;

// ---------------------------------------------------------------------------------------------------------------------------
// The noise volume: EMISSIVE_NOISE_SIZE^3 texels, RG8 (RGBA8 where RG8 is refused), sampled linear with repeat on every axis.
// Two channels of smooth value noise on a lattice EMISSIVE_NOISE_LATTICE texels apart, tileable, baked once on the CPU from a
// fixed integer hash (EmissiveVolumeCore::BakeNoise): no RNG, the same bytes on every machine. One tile is
// EMISSIVE_NOISE_SIZE / EMISSIVE_NOISE_LATTICE noise cells; the march reads r at the sample's cells and g at 2.03 x them.
// ---------------------------------------------------------------------------------------------------------------------------

inline constexpr int EMISSIVE_NOISE_SIZE = 64;
inline constexpr int EMISSIVE_NOISE_LATTICE = 8;

// ---------------------------------------------------------------------------------------------------------------------------
// The per-eye uniforms (PPEmissiveVolumes, hw_postprocess.h: EmissiveVolumeUniforms, 128 bytes, a push constant block):
//   ViewToWorld 0 (mat4)  TanHalfFov 64  ProjOffset 72  ListOrigin 80 (vec3, origin - this eye, GL axes)  VolumeCount 92
//   RectMin 96  RectMax 104 (TexCoord 0..1: where on this eye's screen any volume can be)  StepCount 112  Pad 116, 120, 124
// And the smoke curve's near-volumes variant (SmokeCurveNearVolumesUniforms, 192 bytes): SmokeBeamScatterUniforms' 176,
// then VolumeOrigin 176 (vec3, as ListOrigin) and VolumeCount 188.
// ---------------------------------------------------------------------------------------------------------------------------

// This frame's list, decided on the CPU (hw_emissivevolumes.cpp, EmissiveVolumes::PrepareFrame), once for both eyes.
struct EmissiveVolumeFrame
{
	int Count = 0;						// 0..EMISSIVE_VOLUMES_DRAWN_MAX; 0 = nothing to draw this frame
	// EMISSIVE_VOLUMES_DRAWN_MAX x EMISSIVE_VOLUME_TEXELS x 4 floats, the image's own texel order: row r, column c at
	// (r * EMISSIVE_VOLUMES_DRAWN_MAX + c) * 4. Columns past Count are zero.
	const float* Texels = nullptr;
	uint64_t Serial = 0;				// renewed whenever the texels change (never 0 while Count > 0)
};

// What the backend did with the frame, for this frame's drawing (SetupEmissiveVolumes runs after RunFrameCompute).
// Renderer-internal (written by vk_emissivevolumes.cpp); nothing here reaches the playsim.
struct EmissiveVolumeBackendStatus
{
	bool ListReady = false;		// the list image holds Serial's texels and the noise is baked, both readable by a pass
	int Count = 0;				// the volumes it holds
	uint64_t Serial = 0;
	bool Refused = false;		// the device refused the images: no volume draws this session
};

inline EmissiveVolumeBackendStatus& EmissiveVolumesStatus()
{
	static EmissiveVolumeBackendStatus status;
	return status;
}

// This frame's load, for the perf log (load emissive=live/drawn/refused emissivelights=N).
struct EmissiveVolumeFrameStats
{
	int Live = 0;		// volumes alive
	int Drawn = 0;		// volumes in this frame's list
	int Refused = 0;	// volumes evicted or refused because the pool was full, and spawns dropped (no definition, switched off)
	int Lights = 0;		// effect lights the volumes handed over this frame
};

inline EmissiveVolumeFrameStats& EmissiveVolumeStats()
{
	static EmissiveVolumeFrameStats stats;
	return stats;
}
