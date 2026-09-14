/*
** hw_postprocess_cvars.h
**
** Postprocessing framework
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "c_cvars.h"

class IFXAAShader
{
public:
	enum Quality
	{
		None,
		Low,
		Medium,
		High,
		Extreme,
		Count
	};
};



//==========================================================================
//
// CVARs
//
//==========================================================================
EXTERN_CVAR(Bool, gl_bloom)
EXTERN_CVAR(Float, gl_bloom_amount)
EXTERN_CVAR(Float, gl_bloom_threshold)
EXTERN_CVAR(Float, gl_bloom_knee)
EXTERN_CVAR(Bool, gl_bloom_anamorphic)
EXTERN_CVAR(Float, gl_bloom_anamorphic_ratio)
EXTERN_CVAR(Float, gl_bloom_tint_r)
EXTERN_CVAR(Float, gl_bloom_tint_g)
EXTERN_CVAR(Float, gl_bloom_tint_b)
EXTERN_CVAR(Float, gl_bloom_chromatic)
EXTERN_CVAR(Float, gl_bloom_step)	// [BLOOMSTEP]
EXTERN_CVAR(Float, gl_bloom_override_strength)	// [BLOOMOVERRIDE]
EXTERN_CVAR(Bool, gl_bloom_pin_beams)	// [LIGHTMASK] [PINNEDBLOOM] "Keep legacy lasers"
EXTERN_CVAR(Bool, gl_bloom_pin_captured)	// [PINNEDBLOOM] the pinned look (hw_postprocess_cvars.cpp)
EXTERN_CVAR(Float, gl_bloom_pin_amount)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_threshold)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_knee)	// [PINNEDBLOOM]
EXTERN_CVAR(Bool, gl_bloom_pin_anamorphic)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_anamorphic_ratio)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_tint_r)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_tint_g)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_tint_b)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_chromatic)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_step)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_exposure_scale)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_exposure_min)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_exposure_base)	// [PINNEDBLOOM]
EXTERN_CVAR(Float, gl_bloom_pin_exposure_speed)	// [PINNEDBLOOM]
EXTERN_CVAR(Int, r_lightmask_debug)	// [LIGHTMASK]
EXTERN_CVAR(Int, r_smoke_steps)	// [SMOKEVOLUME] the smoke volume's drawing
EXTERN_CVAR(Float, r_smoke_density_scale)	// [SMOKEVOLUME]
EXTERN_CVAR(Bool, r_smoke_debugslice)	// [SMOKEVOLUME]
EXTERN_CVAR(Bool, r_smoke_beams)	// [SMOKEVOLUME] 13e: beams scatter light in the smoke
EXTERN_CVAR(Bool, r_smoke_beams_depth)	// [SMOKEVOLUME] 13e: haze behind a beam does not dim it
EXTERN_CVAR(Bool, r_smoke_cones_depth)	// [SMOKEVOLUME] 13e: a flashlight cone is dimmed by the haze in front of it
EXTERN_CVAR(Float, gl_exposure_scale)
EXTERN_CVAR(Float, gl_exposure_min)
EXTERN_CVAR(Float, gl_exposure_base)
EXTERN_CVAR(Float, gl_exposure_speed)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, gl_bloom_kernel_size)
EXTERN_CVAR(Bool, gl_lens)
EXTERN_CVAR(Float, gl_lens_k)
EXTERN_CVAR(Float, gl_lens_kcube)
EXTERN_CVAR(Float, gl_lens_chromatic)
EXTERN_CVAR(Int, gl_fxaa)
EXTERN_CVAR(Int, gl_ssao)
EXTERN_CVAR(Int, gl_ssao_portals)
EXTERN_CVAR(Float, gl_ssao_strength)
EXTERN_CVAR(Int, gl_ssao_debug)
EXTERN_CVAR(Float, gl_ssao_bias)
EXTERN_CVAR(Float, gl_ssao_radius)
EXTERN_CVAR(Float, gl_ssao_blur)
EXTERN_CVAR(Float, gl_ssao_exponent)
EXTERN_CVAR(Float, gl_paltonemap_powtable)
EXTERN_CVAR(Bool, gl_paltonemap_reverselookup)
EXTERN_CVAR(Float, gl_menu_blur)
EXTERN_CVAR(Float, vid_brightness)	// UZDXREMA: kept after upstream removed it; see hw_cvars.cpp
EXTERN_CVAR(Float, vid_contrast)
EXTERN_CVAR(Float, vid_saturation)
EXTERN_CVAR(Float, vid_i_blackpoint)
EXTERN_CVAR(Float, vid_i_whitepoint)
EXTERN_CVAR(Int, gl_satformula)
