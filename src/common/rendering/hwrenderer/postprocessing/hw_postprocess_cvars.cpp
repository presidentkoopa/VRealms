/*
** hw_postprocess_cvars.cpp
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

#include "hw_postprocess_cvars.h"
#include "v_video.h"

//==========================================================================
//
// CVARs
//
//==========================================================================
// [BB] On by default in this fork. Stock GZDoom ships it off, which is a
// reasonable default for vanilla Doom and the wrong one here -- the glow
// system relies on bloom to read as emissive rather than as paint, so
// shipping it off means shipping the headline feature broken.
CVAR(Bool, gl_bloom, true, CVAR_ARCHIVE);
CUSTOM_CVAR(Float, gl_bloom_amount, 1.4f, CVAR_ARCHIVE)
{
	if (self < 0.1f) self = 0.1f;
}

// [BB] How bright a pixel must be before it blooms. This was hardcoded at
// 1.0, which meant only pixels that had already blown past full white could
// glow -- fine for a muzzle flash, useless for anything trying to read as
// emissive at a sane brightness. Lowering it lets glow bloom without being
// driven to absurd intensities first.
CUSTOM_CVAR(Float, gl_bloom_threshold, 1.0f, CVAR_ARCHIVE)
{
	if (self < 0.05f) self = 0.05f;
	if (self > 4.0f) self = 4.0f;
}

// [BB] Soft knee. A hard threshold makes things POP in and out of blooming
// as they cross it, which is constant and obvious when glow is pulsing or a
// beam is sweeping. This rolls the transition over a range instead, so bloom
// eases in rather than snapping on. 0 is the old hard cutoff.
CUSTOM_CVAR(Float, gl_bloom_knee, 0.5f, CVAR_ARCHIVE)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 8.0f) self = 8.0f;
}

// [BB] Anamorphic: blur wider horizontally than vertically, so bright things
// streak sideways the way they do through an anamorphic lens. Free, because
// the blur already runs as separate horizontal and vertical passes -- this
// only gives them different amounts.
CVAR(Bool, gl_bloom_anamorphic, false, CVAR_ARCHIVE)
CUSTOM_CVAR(Float, gl_bloom_anamorphic_ratio, 3.0f, CVAR_ARCHIVE)
{
	if (self < 1.0f) self = 1.0f;
	if (self > 16.0f) self = 16.0f;
}

// [BB] Bloom tint and chromatic fringing. Tint colours the bloom
// independently of what produced it; fringing offsets the colour channels
// radially so bright edges break up toward the screen edges, the way light
// through glass does.
CVAR(Float, gl_bloom_tint_r, 1.0f, CVAR_ARCHIVE)
CVAR(Float, gl_bloom_tint_g, 1.0f, CVAR_ARCHIVE)
CVAR(Float, gl_bloom_tint_b, 1.0f, CVAR_ARCHIVE)
CUSTOM_CVAR(Float, gl_bloom_chromatic, 0.0f, CVAR_ARCHIVE)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 0.1f) self = 0.1f;
}

// [BLOOMSTEP] Wider blur (Engine docs/REVIEW_BLOOM_PLAN.md E3). The bloom blur is
// seven taps one texel apart, and gl_bloom_amount only reshapes their weights: past
// an amount of about 4 the kernel is already a flat box, so neither the amount nor
// the anamorphic ratio can widen it any further. The step spreads the taps this
// many texels apart (the anamorphic ratio spreads the horizontal ones further), so
// the amount keeps meaning a real width. 1 is today's blur exactly -- the very same
// shaders and uniforms (PPBloom::RenderBloom). Renderer-read every frame.
CUSTOM_CVAR(Float, gl_bloom_step, 1.0f, CVAR_ARCHIVE)
{
	if (!(self >= 1.0f)) self = 1.0f;   // written this way so a NaN lands on 1 too
	if (self > 8.0f) self = 8.0f;
}

// [BLOOMOVERRIDE] How much of any scripted bloom change applies
// (LevelLocals.SetBloomOverride; E1 in REVIEW_BLOOM_PLAN.md). It multiplies the
// override's mix: 1 = as the script asked, 0 = the gl_bloom_* settings alone, as
// if no override were set. One live depth control for every reactive bloom a mod
// drives -- script-read sliders cannot move while a menu pauses the game, and this
// is read by the renderer every frame -- and a comfort control for flashes in VR.
CUSTOM_CVAR(Float, gl_bloom_override_strength, 1.0f, CVAR_ARCHIVE)
{
	if (!(self >= 0.0f)) self = 0.0f;   // NaN lands on 0
	if (self > 1.0f) self = 1.0f;
}

// [LIGHTMASK] Beam light keeps its own bloom ("Engine docs/EMISSIVE_BLOOM_PLAN.md" E6; the
// glow lane's "Keep legacy lasers" row). Step E6a builds only the light mask this needs:
// while this is on (and gl_bloom is), the main scene pass carries one more colour
// attachment recording, per pixel, how much of the light is beam light, and the scene
// programs write it (hw_postprocess.h, PPLightMask). Nothing reads the mask yet but
// r_lightmask_debug, so the picture is exactly the picture with this off; E6b gives the
// switch its effect (the pinned bloom), the capture of the pinned look and the
// CVAR_NOINITCALL callback that capture runs in. The first switch-on in a session
// compiles the mask programs once (a pause) and creates the attachment, which then stays
// allocated until the render buffers are re-created anyway (a resolution or multisample
// change), so flipping it back and forth costs no re-create. Renderer-read every frame
// (VulkanRenderDevice::BeginFrame). OpenGL and GLES ignore it.
CVAR(Bool, gl_bloom_pin_beams, false, CVAR_ARCHIVE)

// [LIGHTMASK] A test view of the light mask. 1: the scene in grey with each pixel's
// share of beam light in green and of emissive light in red; 2: the beam light share
// alone. It turns the mask on by itself (gl_bloom_pin_beams says what that costs), and it
// is drawn where bloom would run, in bloom's place: a share is measured against the image
// bloom reads, not the one bloom writes. Not saved. Vulkan only.
CUSTOM_CVAR(Int, r_lightmask_debug, 0, 0)
{
	if (self < 0) self = 0;
	if (self > 2) self = 2;
}

// [SMOKEVOLUME] THE SMOKE VOLUME'S DRAWING ("Engine docs/SMOKE_VOLUME_PLAN.md" 13c; PPSmokeVolume in
// hw_postprocess.h). The volume's own switches -- r_smoke, r_smoke_quality, r_smoke_dissipation_scale
// and the test source -- live with the simulation in hw_cvars.cpp; these three belong to the
// post-process pass. All are read by the renderer every frame (SetupSmokeVolume, hw_drawinfo.cpp), so
// they respond with a menu open. OpenGL and GLES draw no smoke.
//
// r_smoke_steps: the most samples one view ray takes through the smoke, 16..128. They are spread over
// only the stretch of the ray that crosses tiles holding smoke, so a small cloud in a big room gets
// every one. More is smoother and makes pp.smoke cost more.
CUSTOM_CVARD(Int, r_smoke_steps, 48, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "the most samples a view ray takes through the smoke volume, 16-128 (Vulkan only)")
{
	if (self < 16) self = 16;
	if (self > 128) self = 128;
}

// r_smoke_density_scale: the player's "Smoke density". It multiplies how strongly smoke hides what is
// behind it and how much it glows, on top of the mod's own look (SetSmokeLook's absorption). 1 = as the
// mod made it; 0 = invisible, and then the pass does not run at all. Clamped 0..16 where it is read.
CVARD(Float, r_smoke_density_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "multiplies how thick the smoke volume looks, 0-16 (Vulkan only)")

// r_smoke_debugslice: a test view. It shows the smoke's density on a level plane a little below your
// eyes, over the scene, inside the smoke box: blue where the smoke is thin, white where it is thick, and
// green where the simulation's tiles are awake but nearly empty. It draws only while there is smoke.
// Not saved.
CVARD(Bool, r_smoke_debugslice, false, 0, "show the smoke volume's density on a level plane below the eye (debug; Vulkan only)")

CVAR(Float, gl_exposure_scale, 1.3f, CVAR_ARCHIVE)
CVAR(Float, gl_exposure_min, 0.35f, CVAR_ARCHIVE)
CVAR(Float, gl_exposure_base, 0.35f, CVAR_ARCHIVE)
CVAR(Float, gl_exposure_speed, 0.05f, CVAR_ARCHIVE)

CUSTOM_CVAR(Int, gl_tonemap, 0, CVAR_ARCHIVE)
{
	if (self < 0 || self > 5)
		self = 0;
}

CVAR(Bool, gl_lens, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, gl_lens_k, -0.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_lens_kcube, 0.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_lens_chromatic, 1.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, gl_fxaa, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0 || self >= IFXAAShader::Count)
	{
		self = 0;
	}
}

CUSTOM_CVAR(Int, gl_ssao, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0 || self > 3)
		self = 0;
}

CUSTOM_CVAR(Int, gl_ssao_portals, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
		self = 0;
}

CVAR(Float, gl_ssao_strength, 0.7f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, gl_ssao_debug, 0, 0)
CVAR(Float, gl_ssao_bias, 0.2f, 0)
CVAR(Float, gl_ssao_radius, 80.0f, 0)
CUSTOM_CVAR(Float, gl_ssao_blur, 16.0f, 0)
{
	if (self < 0.1f) self = 0.1f;
}

CUSTOM_CVAR(Float, gl_ssao_exponent, 1.8f, 0)
{
	if (self < 0.1f) self = 0.1f;
}

CUSTOM_CVAR(Float, gl_paltonemap_powtable, 2.0f, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	screen->UpdatePalette();
}

CUSTOM_CVAR(Bool, gl_paltonemap_reverselookup, true, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	screen->UpdatePalette();
}
// reminder: if is negative, use the gameinfo entry
CVAR(Float, gl_menu_blur, -1.0f, CVAR_ARCHIVE)
