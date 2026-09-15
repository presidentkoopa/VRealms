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
#include "c_dispatch.h"	// [PINNEDBLOOM] CCMD gl_bloom_pin_capture / gl_bloom_pin_reset
#include "printf.h"	// [PINNEDBLOOM]

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

// [LIGHTMASK] [PINNEDBLOOM] PINNED BLOOM FOR BEAM LIGHT -- the player's "Keep legacy lasers"
// ("Engine docs/EMISSIVE_BLOOM_PLAN.md" E6). On: beam light -- every SetBeam line (grab lasers,
// the Lance, ClaimBeam users), the light it throws on walls and into fog-slab mist, and the beams
// r_beams_drawn routes to drawn lines -- blooms with the PINNED look (the gl_bloom_pin_* cvars
// below), while presets, the other gl_bloom_* / gl_exposure_* settings and SetBloomOverride
// change only the rest of the picture. Off: beam light blooms with everything else, exactly as
// before this switch existed. Flip it both ways at any time to compare the two.
//
// How: while this is on (and gl_bloom is), the scene pass records per pixel how much of its light
// is beam light (the light mask: hw_postprocess.h, PPLightMask), and PPBloom::RenderBloom weighs the
// bloom extract by that share (PPBloomPlan). While the pinned look sends the same values as the
// live one, or no beam is live, bloom runs today's passes untouched.
//
// Renderer-read every frame (VulkanRenderDevice::BeginFrame snapshots it); flipping only changes
// which bloom passes run. The first switch-on in a session compiles the mask programs once (a
// pause) and creates the attachment, which stays allocated until the render buffers are re-created
// anyway (a resolution or multisample change), so flipping back and forth costs no re-create.
// OpenGL and GLES ignore it.
//
// THE FIRST-EVER SWITCH-ON CAPTURES the live bloom settings as the pinned look, so "legacy" means
// the look the player had at that moment; gl_bloom_pin_captured records that it happened, and after
// that flipping never recaptures. Only a real change of this cvar captures: CVAR_NOINITCALL, and
// the ini is read while cvar callbacks are off (d_main.cpp), so loading a config never does.
// gl_bloom_pin_capture recaptures on request; gl_bloom_pin_reset returns the pinned look to the
// engine defaults.
static void SetPinnedBloomLook(bool fromDefaults);
CUSTOM_CVAR(Bool, gl_bloom_pin_beams, false, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	if (self && !gl_bloom_pin_captured)
		SetPinnedBloomLook(false);
}

// [PINNEDBLOOM] Whether a pinned look was ever chosen: by the first switch-on of
// gl_bloom_pin_beams, gl_bloom_pin_capture or gl_bloom_pin_reset. While false, the first
// switch-on captures; once true it never does again.
CVAR(Bool, gl_bloom_pin_captured, false, CVAR_ARCHIVE)

// [PINNEDBLOOM] THE PINNED LOOK: the bloom beam light keeps while gl_bloom_pin_beams is on. One
// cvar per live setting, each with its live cvar's own type, default and clamp -- so a capture
// copies the very bits, and a freshly captured look sends exactly the values the live look sends
// (PPBloom then runs today's passes). No preset writes these (RS_Bloom's presets name every cvar
// they write) and no SetBloomOverride reaches them. Renderer-read every frame (PPBloom::PinnedLook).
// Normally set by gl_bloom_pin_capture and gl_bloom_pin_reset rather than by hand.
CUSTOM_CVAR(Float, gl_bloom_pin_amount, 1.4f, CVAR_ARCHIVE)
{
	if (self < 0.1f) self = 0.1f;
}
CUSTOM_CVAR(Float, gl_bloom_pin_threshold, 1.0f, CVAR_ARCHIVE)
{
	if (self < 0.05f) self = 0.05f;
	if (self > 4.0f) self = 4.0f;
}
CUSTOM_CVAR(Float, gl_bloom_pin_knee, 0.5f, CVAR_ARCHIVE)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 8.0f) self = 8.0f;
}
CVAR(Bool, gl_bloom_pin_anamorphic, false, CVAR_ARCHIVE)
CUSTOM_CVAR(Float, gl_bloom_pin_anamorphic_ratio, 3.0f, CVAR_ARCHIVE)
{
	if (self < 1.0f) self = 1.0f;
	if (self > 16.0f) self = 16.0f;
}
CVAR(Float, gl_bloom_pin_tint_r, 1.0f, CVAR_ARCHIVE)
CVAR(Float, gl_bloom_pin_tint_g, 1.0f, CVAR_ARCHIVE)
CVAR(Float, gl_bloom_pin_tint_b, 1.0f, CVAR_ARCHIVE)
CUSTOM_CVAR(Float, gl_bloom_pin_chromatic, 0.0f, CVAR_ARCHIVE)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 0.1f) self = 0.1f;
}
CUSTOM_CVAR(Float, gl_bloom_pin_step, 1.0f, CVAR_ARCHIVE)
{
	if (!(self >= 1.0f)) self = 1.0f;   // written this way so a NaN lands on 1 too
	if (self > 8.0f) self = 8.0f;
}
CVAR(Float, gl_bloom_pin_exposure_scale, 1.3f, CVAR_ARCHIVE)
CVAR(Float, gl_bloom_pin_exposure_min, 0.35f, CVAR_ARCHIVE)
CVAR(Float, gl_bloom_pin_exposure_base, 0.35f, CVAR_ARCHIVE)
CVAR(Float, gl_bloom_pin_exposure_speed, 0.05f, CVAR_ARCHIVE)

// [PINNEDBLOOM] Sets the pinned look from the live bloom settings (fromDefaults false: what the
// player sees now) or from the live cvars' DEFAULTS (true: the engine's reference bloom), and marks
// it chosen so a later first switch-on of gl_bloom_pin_beams leaves it alone. Never touches
// gl_bloom_pin_beams. One line to the console with the values. This machine's display settings
// only: nothing is sent anywhere, so it is safe in netplay.
static void SetPinnedBloomLook(bool fromDefaults)
{
	struct PinnedFloat { FFloatCVarRef *Live; FFloatCVarRef *Pinned; };
	static const PinnedFloat floats[] =
	{
		{ &gl_bloom_amount, &gl_bloom_pin_amount },
		{ &gl_bloom_threshold, &gl_bloom_pin_threshold },
		{ &gl_bloom_knee, &gl_bloom_pin_knee },
		{ &gl_bloom_anamorphic_ratio, &gl_bloom_pin_anamorphic_ratio },
		{ &gl_bloom_tint_r, &gl_bloom_pin_tint_r },
		{ &gl_bloom_tint_g, &gl_bloom_pin_tint_g },
		{ &gl_bloom_tint_b, &gl_bloom_pin_tint_b },
		{ &gl_bloom_chromatic, &gl_bloom_pin_chromatic },
		{ &gl_bloom_step, &gl_bloom_pin_step },
		{ &gl_exposure_scale, &gl_bloom_pin_exposure_scale },
		{ &gl_exposure_min, &gl_bloom_pin_exposure_min },
		{ &gl_exposure_base, &gl_bloom_pin_exposure_base },
		{ &gl_exposure_speed, &gl_bloom_pin_exposure_speed },
	};
	for (const PinnedFloat &value : floats)
		*value.Pinned = fromDefaults ? value.Live->get()->GetGenericRepDefault(CVAR_Float).Float : (float)*value.Live;
	gl_bloom_pin_anamorphic = fromDefaults ? gl_bloom_anamorphic->GetGenericRepDefault(CVAR_Bool).Bool : (bool)gl_bloom_anamorphic;
	gl_bloom_pin_captured = true;

	Printf("Bloom: laser look %s -- spread %g, threshold %g, knee %g, anamorphic %d (ratio %g), step %g, tint %g %g %g, "
		"fringing %g, exposure scale %g min %g base %g speed %g\n",
		fromDefaults ? "set to the engine defaults" : "captured from the current bloom settings",
		(double)(float)gl_bloom_pin_amount, (double)(float)gl_bloom_pin_threshold, (double)(float)gl_bloom_pin_knee,
		(int)(bool)gl_bloom_pin_anamorphic, (double)(float)gl_bloom_pin_anamorphic_ratio, (double)(float)gl_bloom_pin_step,
		(double)(float)gl_bloom_pin_tint_r, (double)(float)gl_bloom_pin_tint_g, (double)(float)gl_bloom_pin_tint_b,
		(double)(float)gl_bloom_pin_chromatic, (double)(float)gl_bloom_pin_exposure_scale, (double)(float)gl_bloom_pin_exposure_min,
		(double)(float)gl_bloom_pin_exposure_base, (double)(float)gl_bloom_pin_exposure_speed);
}

// [PINNEDBLOOM] "Capture current bloom as the laser look": the live bloom settings become the pinned
// look. Does not switch gl_bloom_pin_beams on or off.
CCMD(gl_bloom_pin_capture)
{
	SetPinnedBloomLook(false);
}

// [PINNEDBLOOM] "Laser look back to the engine defaults": the pinned look becomes the engine's own
// bloom defaults (the gl_bloom_* / gl_exposure_* defaults). Does not switch gl_bloom_pin_beams.
CCMD(gl_bloom_pin_reset)
{
	SetPinnedBloomLook(true);
}

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
CUSTOM_CVARD(Int, r_smoke_steps, 32, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "the most samples a view ray takes through the smoke volume, 16-128 (Vulkan only)")
{
	if (self < 16) self = 16;
	if (self > 128) self = 128;
}

EXTERN_CVAR(Bool, r_smoke)					// hw_cvars.cpp
EXTERN_CVAR(Int, r_smoke_quality)			// hw_cvars.cpp
EXTERN_CVAR(Int, r_smoke_light_quality)	// hw_smokevolume.cpp

// r_smoke_preset: one dial for the smoke volume's cost and look -- 0 Off, 1 Plain, 2 Normal, 3 Heavy, 4 Extreme. Moving it sets
// r_smoke, r_smoke_quality, r_smoke_steps and r_smoke_light_quality together; renderer-read, so it applies with the menu open.
// NOINITCALL: a saved preset never overwrites per-cvar tweaks at startup -- only a change applies. Any mod's menu may point at it
// (RS_Ballistics' Smoke dial does). Plain and Normal keep quality 1; Heavy and Extreme use quality 2.
CUSTOM_CVARD(Int, r_smoke_preset, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL, "smoke preset: 0 off, 1 plain, 2 normal, 3 heavy, 4 extreme (sets r_smoke, r_smoke_quality, r_smoke_steps, r_smoke_light_quality)")
{
	if (self < 0) { self = 0; return; }
	if (self > 4) { self = 4; return; }
	static const int quality[5] = { 1, 1, 1, 2, 2 };
	static const int steps[5] = { 32, 16, 24, 32, 48 };
	static const int lightQuality[5] = { 1, 1, 1, 2, 3 };
	int p = self;
	if (p == 0)
	{
		r_smoke = false;
		return;
	}
	r_smoke = true;
	r_smoke_quality = quality[p];
	r_smoke_steps = steps[p];
	r_smoke_light_quality = lightQuality[p];
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

// [SMOKEVOLUME] 13e: BEAMS AND CONES IN THE SMOKE ("Engine docs/SMOKE_VOLUME_PLAN.md" 13e, "Engine docs/
// SMOKE_13E_IMPL_NOTES.md"). Three A/B checks, all on by default and all renderer-read every frame
// (SetupSmokeVolume, hw_drawinfo.cpp), so they respond with a menu open. Each changes only pixels with smoke on
// them: with no smoke the smoke pass does not run and the flashlight cone uses its own programs, so the lasers and
// the cones look exactly as they did. OpenGL and GLES draw no smoke.
//
// r_smoke_beams: a beam line (a grab laser, the Lance) shows its path in the smoke -- the light it scatters, brightest
// where the smoke is thick, carrying the smoke's swirls. Off: beams scatter nothing (13d's look).
CVARD(Bool, r_smoke_beams, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "beam lines scatter light in the smoke volume (A/B check; Vulkan only)")

// r_smoke_beams_depth: haze BEHIND a beam line does not dim it; haze in front still does. A beam writes no depth, so
// without this the smoke composite dims its glow by all the haze on its pixel's ray, including haze behind it.
// Off: 13d's composite.
CVARD(Bool, r_smoke_beams_depth, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "haze behind a beam line does not dim it (A/B check; Vulkan only)")

// r_smoke_cones_depth: a volumetric beam cone (the flashlight's air glow) is dimmed by the haze in front of each part of
// it. The cone draws after the smoke, so without this haze never dims it. Off: the cone's own programs, as before.
CVARD(Bool, r_smoke_cones_depth, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "the haze in front of a volumetric beam cone dims it (A/B check; Vulkan only)")

// [EMISSIVEVOLUMES] THE EMISSIVE VOLUMES' DRAWING ("Engine docs/VOLUMETRIC_FLASH_15_PLAN.md" 2d; PPEmissiveVolumes in hw_postprocess.h).
// Their own switches -- which draw, how long, how they move, their lights, brightness, the pool and the test -- live with the
// volumes in hw_emissivevolumes.cpp; these two are the pass's quality. Both are read by the renderer every frame
// (SetupEmissiveVolumes, hw_drawinfo.cpp), so they respond with a menu open. OpenGL and GLES draw no emissive volume.
//
// r_emissivevolumes_steps: steps a view ray takes across a volume's whole diameter (a shorter chord takes fewer, never under 4).
// More is smoother and makes pp.emissive cost more.
CUSTOM_CVARD(Int, r_emissivevolumes_steps, 32, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "steps a view ray takes across an emissive volume's diameter, 8-64 (Vulkan only)")
{
	if (self < 8) self = 8;
	if (self > 64) self = 64;
}

// r_emissivevolumes_resolution: the march at half the scene's resolution (2, the default) or full (1, about four times the cost).
CUSTOM_CVARD(Int, r_emissivevolumes_resolution, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "emissive volume march resolution: 2 half, 1 full (Vulkan only)")
{
	if (self < 1) self = 1;
	if (self > 2) self = 2;
}

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
