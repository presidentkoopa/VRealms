/*
** hw_cvars.h
**
** most of the hardware renderer's CVARs.
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2020 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#pragma once


#include "c_cvars.h"

EXTERN_CVAR(Bool,gl_enhanced_nightvision)
EXTERN_CVAR(Int, screenblocks);
EXTERN_CVAR(Int, gl_texture_filter)
EXTERN_CVAR(Int, gl_texture_quality)	// [DDS] hw_cvars.cpp
EXTERN_CVAR(Float, gl_texture_filter_anisotropic)
EXTERN_CVAR(Int, gl_texture_format)
EXTERN_CVAR(Bool, gl_usefb)

EXTERN_CVAR(Int, gl_weaponlight)

EXTERN_CVAR (Bool, gl_light_sprites);
EXTERN_CVAR (Bool, gl_light_particles);
EXTERN_CVAR (Bool, gl_light_weapons);
EXTERN_CVAR (Bool, gl_light_distance_cull_cache);
EXTERN_CVAR (Bool, gl_light_model_dedupe_cache);
EXTERN_CVAR (Bool, gl_light_spot_cache);
EXTERN_CVAR (Bool, gl_light_pos_relative_cache);
EXTERN_CVAR (Bool, gl_light_shadowmap);
EXTERN_CVAR (Int, gl_shadowmap_quality);
EXTERN_CVAR (Int, gl_storage_buffer_type);
EXTERN_CVAR (Float, gl_light_distance_cull);
EXTERN_CVAR (Int, gl_light_flat_max_lights);
EXTERN_CVAR (Int, gl_light_wall_max_lights);
EXTERN_CVAR (Int, gl_light_flat_candidate_budget);
EXTERN_CVAR (Int, gl_light_wall_candidate_budget);
EXTERN_CVAR (Int, gl_light_range_limit);
EXTERN_CVAR (Float, gl_sprite_distance_cull);
EXTERN_CVAR (Float, gl_sprite_decor_distance_cull);
EXTERN_CVAR (Float, gl_line_distance_cull);

EXTERN_CVAR(Bool, gl_global_fade);
EXTERN_CVAR(Bool, gl_global_fade_debug);
EXTERN_CVAR(Color, gl_global_fade_color);
EXTERN_CVAR(Float, gl_global_fade_gradient);
EXTERN_CVAR(Float, gl_global_fade_density);
EXTERN_CVAR(Bool, gl_skydome);

EXTERN_CVAR(Int, gl_fogmode)
EXTERN_CVAR(Bool,gl_mirror_envmap)
EXTERN_CVAR(Bool, gl_texture_thread)
EXTERN_CVAR(Bool, gl_texture_thread_models)
EXTERN_CVAR(Bool, gl_texture_thread_upload)
EXTERN_CVAR(Int, gl_texture_thread_workers)
EXTERN_CVAR(Int, gl_background_flush_count)
EXTERN_CVAR(Int, vk_max_transfer_threads)

EXTERN_CVAR(Int, r_portal_recursions)

EXTERN_CVAR(Bool,gl_mirrors)
EXTERN_CVAR(Bool,gl_mirror_envmap)
EXTERN_CVAR(Bool,gl_mirror_player)
EXTERN_CVAR(Bool, gl_seamless)

EXTERN_CVAR(Float, gl_mask_threshold)
EXTERN_CVAR(Float, gl_mask_sprite_threshold)

EXTERN_CVAR(Int, gl_multisample)

EXTERN_CVAR(Bool, gl_bloom)
EXTERN_CVAR(Float, gl_bloom_amount)
EXTERN_CVAR(Int, gl_bloom_kernel_size)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Float, gl_exposure)
EXTERN_CVAR(Bool, gl_lens)
EXTERN_CVAR(Float, gl_lens_k)
EXTERN_CVAR(Float, gl_lens_kcube)
EXTERN_CVAR(Float, gl_lens_chromatic)
EXTERN_CVAR(Int, gl_ssao)
EXTERN_CVAR(Int, gl_ssao_portals)
EXTERN_CVAR(Float, gl_ssao_strength)
EXTERN_CVAR(Int, gl_ssao_debug)
EXTERN_CVAR(Float, gl_ssao_bias)
EXTERN_CVAR(Float, gl_ssao_radius)
EXTERN_CVAR(Float, gl_ssao_blur_amount)

EXTERN_CVAR(Int, gl_debug_level)
EXTERN_CVAR(Bool, gl_debug_breakpoint)

EXTERN_CVAR(Int, gl_shadowmap_filter)

EXTERN_CVAR(Bool, gl_brightfog)
EXTERN_CVAR(Bool, gl_lightadditivesurfaces)
EXTERN_CVAR(Bool, gl_notexturefill)

EXTERN_CVAR(Bool, gl_no_persistent_buffer)
EXTERN_CVAR(Bool, gl_no_clip_planes)
EXTERN_CVAR(Bool, gl_no_ssbo)

EXTERN_CVAR(Bool, r_radarclipper)
EXTERN_CVAR(Bool, r_dithertransparency)
EXTERN_CVAR(Bool, vr_scene_multithread)

EXTERN_CVAR(Bool, gl_portals)

EXTERN_CVAR(Bool, gl_strict_gldefs_errors)

// [GPUPARTICLES] see hw_cvars.cpp
EXTERN_CVAR(Bool, r_gpuparticles)
EXTERN_CVAR(Float, r_gpuparticles_sizescale)
EXTERN_CVAR(Float, r_gpuparticles_maxsize)
EXTERN_CVAR(Float, r_gpuparticles_stretch)
EXTERN_CVAR(Float, r_gpuparticles_intensity)
EXTERN_CVAR(Float, r_gpuparticles_soft)	// [2a] soft particles, 0 = off
EXTERN_CVAR(Int, r_gpuparticles_ringsize)
EXTERN_CVAR(Bool, r_gpuparticles_debug)
EXTERN_CVAR(Bool, r_gpuparticles_legacy)	// [2b] A/B: stage 1 records instead of inline definitions
int GpuParticleRingCapacity();
bool GpuParticlesLegacyPath();	// [2b] r_gpuparticles_legacy, for FLevelLocals::SpawnGpuParticles
EXTERN_CVAR(Int, r_gpuparticles_atlas_size)	// [2c] particle atlas layer side in pixels
int GpuParticleAtlasLayerSize();	// [2c] r_gpuparticles_atlas_size rounded to 128, 256 or [ATLASBC7] 512
// [ATLASBC7] the compressed particle atlas, see hw_cvars.cpp
EXTERN_CVAR(Bool, r_gpuparticles_atlas_compressed)
EXTERN_CVAR(Int, r_gpuparticles_atlas_compressed_size)
EXTERN_CVAR(Int, r_gpuparticles_atlas_layers)
EXTERN_CVAR(Int, r_gpuparticles_atlas_overflow)
int GpuParticleAtlasCompressedSide();	// r_gpuparticles_atlas_compressed_size rounded to 256, 512 or 1024
unsigned GpuParticleAtlasLayersPerAtlas();	// r_gpuparticles_atlas_layers rounded to a step of 256 in 256 .. 2048
struct ParticleAtlasPolicy;
ParticleAtlasPolicy GpuParticleAtlasPolicy();	// those settings and the device's limits (hw_particledefbuffer.h)
EXTERN_CVAR(Int, r_gpuparticles_lights)	// [2d] dynamic lights in view that light lit particles, 0-32

// [DRAWNLINES] + [BEAMLINES] see hw_cvars.cpp
EXTERN_CVAR(Bool, r_beams_drawn)
EXTERN_CVAR(Bool, r_beams_drawn_surfacelight)
EXTERN_CVAR(Bool, r_drawnlines)
EXTERN_CVAR(Float, r_drawnlines_depthbias)
EXTERN_CVAR(Bool, r_beams_debug)
int DrawnLineCapacity();

// [HEATREFRACTION] heat shimmer, see hw_cvars.cpp
EXTERN_CVAR(Bool, r_heatrefraction)
EXTERN_CVAR(Float, r_heatrefraction_scale)
EXTERN_CVAR(Bool, r_heatrefraction_test)

// [SHOCKWAVE] blast ripples, see hw_cvars.cpp
EXTERN_CVAR(Bool, r_shockwave)
EXTERN_CVAR(Float, r_shockwave_scale)
EXTERN_CVAR(Int, r_shockwave_look)
EXTERN_CVAR(Int, r_shockwave_near)
EXTERN_CVAR(Int, r_shockwave_chroma)
EXTERN_CVAR(Int, r_shockwave_max)
EXTERN_CVAR(Int, r_shockwave_test)

// [SMOKEVOLUME] the smoke volume, see hw_cvars.cpp
EXTERN_CVAR(Bool, r_smoke)
EXTERN_CVAR(Int, r_smoke_quality)
EXTERN_CVAR(Bool, r_smoke_computetest)
EXTERN_CVAR(Float, r_smoke_dissipation_scale)	// [13b]
