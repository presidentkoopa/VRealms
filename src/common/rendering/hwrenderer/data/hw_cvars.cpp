/*
** hw_cvars.cpp
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

#include "c_cvars.h"
#include "c_dispatch.h"
#include "v_video.h"
#include "hw_cvars.h"
#include "hw_drawnlinebuffer.h"	// [DRAWNLINES] DrawnLinesLogToggle
#include "hw_particledefbuffer.h"	// [ATLASBC7] ParticleAtlasPolicy and the atlas layer limits
#include "menu.h"
#include "printf.h"
#include "version.h"
#include <algorithm>

CUSTOM_CVAR(Int, gl_fogmode, 2, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	if (self > 2) self = 2;
	if (self < 0) self = 0;
}

// Optional family toggle for Selaco-style background texture/material streaming.
// The background asset loader is on by default (owner, 2026-09-15): levels load sooner and first-use texture hitches
// drop in the headset. Drained before a level precaches and at engine cleanup (p_setup.cpp, d_main.cpp).
CVAR(Bool, gl_texture_thread, true, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
CVAR(Bool, gl_texture_thread_models, true, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
CVAR(Bool, gl_texture_thread_upload, true, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)

// Optional tuning for the texture-thread family. The master toggle above still gates the whole feature set.
CUSTOM_CVAR(Int, gl_texture_thread_workers, 2, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
{
	if (self < 1) self = 1;
	else if (self > 8) self = 8;
}

CUSTOM_CVAR(Int, vk_max_transfer_threads, 2, CVAR_GLOBALCONFIG | CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	if (self < 0) self = 0;
	else if (self > 8) self = 8;

	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CUSTOM_CVAR(Int, gl_background_flush_count, 100, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
{
	if (self < 1) self = 1;
	else if (self > 1000) self = 1000;
}


// OpenGL stuff moved here
// GL related CVARs
CVAR(Bool, gl_portals, true, 0)
CVAR(Bool, gl_mirrors, true, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
CVAR(Bool, gl_mirror_player, true, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
CVAR(Bool,gl_mirror_envmap, true, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
CVAR(Bool, gl_seamless, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

// Upstream renamed r_mirror_recursions -> r_portal_recursions (and swrenderer's
// r_portal.cpp now only EXTERN_CVARs it, so this is the single definition).
// The fork's tuned default of 2 (instead of 4) and CVAR_GLOBALCONFIG are kept.
CUSTOM_CVAR(Int, r_portal_recursions, 2, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
{
	if (self > 16) self = 16;
	if (self < 0) self = 0;
}

bool gl_plane_reflection_i;	// This is needed in a header that cannot include the CVAR stuff...
CUSTOM_CVAR(Bool, gl_plane_reflection, false, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
{
	gl_plane_reflection_i = self;
}

constexpr float GAMMA_DEFAULT = 2.2;
constexpr float GAMMA_HIGH = 3.0;
constexpr float GAMMA_LOW = 0.1;

constexpr float GAMMA_LOW_FIX = (GAMMA_LOW-GAMMA_DEFAULT) / (GAMMA_HIGH-GAMMA_DEFAULT);

CUSTOM_CVARD(Float, vid_gamma, GAMMA_DEFAULT, 0, "(internal) target output gamma")
{
	if (self < GAMMA_LOW) self = GAMMA_LOW;
}

CUSTOM_CVARD(Float, vid_fixgamma, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts gamma component of gamma ramp")
{
	if (self < GAMMA_LOW_FIX) self = GAMMA_LOW_FIX;
	else vid_gamma = self*(GAMMA_HIGH-GAMMA_DEFAULT) + GAMMA_DEFAULT;
}

CUSTOM_CVARD(Float, vid_contrast, 1.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts contrast component of gamma ramp")
{
	if (self < 0) self = 0;
	else if (self > 5) self = 5;
}

// Fork: vid_brightness is deleted upstream but is still consumed by the VR present
// path (vk_postprocess.cpp PresentUniforms::Brightness) and the DSPLYMNU_BRIGHTNESS
// slider. Keep it.
CUSTOM_CVARD(Float, vid_brightness, 0.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts brightness component of gamma ramp")
{
	if (self < -2) self = -2;
	else if (self > 2) self = 2;
}

CUSTOM_CVARD(Float, vid_saturation, 1.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts saturation component of gamma ramp")
{
	if (self < -3) self = -3;
	else if (self > 3) self = 3;
}

#ifndef BW_GAP
#define BW_GAP 0.2
#endif

CVAR(Float, vid_i_blackpoint, 1.f, CVAR_VIRTUAL | CVAR_NOINITCALL | CVAR_SYSTEM_ONLY);
CVAR(Float, vid_i_whitepoint, 1.f, CVAR_VIRTUAL | CVAR_NOINITCALL | CVAR_SYSTEM_ONLY);

CUSTOM_CVARD(Float, vid_blackpoint, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts what the engine outputs as black")
{
	if (self < 0) self = 0;
	if (self > 1) self = 1;

	float value = self*self;
	float bound = 1 - BW_GAP;
	float buffer = vid_i_whitepoint - BW_GAP;

	vid_i_blackpoint = min(min(buffer, value), bound);
}

CUSTOM_CVARD(Float, vid_whitepoint, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts what the engine outputs as white")
{
	if (self < -1) self = -1;
	if (self > 2) self = 2;

	float value = self + 1;
	float bound = 0 + BW_GAP;
	float buffer = vid_i_blackpoint + BW_GAP;
	value = (value*value*value+1)/2;

	vid_i_whitepoint = max(max(buffer, value), bound);
}

#undef BW_GAP

CVAR(Int, gl_satformula, 2, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);

//==========================================================================
//
// Texture CVARs
//
//==========================================================================
CUSTOM_CVARD(Float, gl_texture_filter_anisotropic, 4.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL, "changes the OpenGL texture anisotropy setting")
{
	if (screen != nullptr)
	{
		screen->SetTextureFilterMode();
	}
}

CUSTOM_CVARD(Int, gl_texture_filter, 0, CVAR_ARCHIVE|CVAR_GLOBALCONFIG|CVAR_NOINITCALL, "changes the texture filtering settings")
{
	if (self < 0 || self > 6) self=6;
	if (screen != nullptr)
	{
		screen->SetTextureFilterMode();
	}
}

// [DDS] GZSelaco a46c31630a: the stored mip level compressed DDS sprites, skins and decals start from
// (VkHardwareTexture::CreateCompressedTexture). 0 = the top level: full quality, as before. Textures
// that are not compressed DDS ignore it. Read by the renderer whenever such a texture is uploaded;
// changing it drops those textures, so the next draw uploads them from the new level.
extern void hw_unloadSprites();
CUSTOM_CVARD(Int, gl_texture_quality, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL, "changes texture quality. 0 = full, 2 = low")
{
	if (self < 0 || self > 4)
	{
		self = 0;
		return;
	}
	if (screen != nullptr)
	{
		hw_unloadSprites();
		screen->SetTextureFilterMode();	// For Vulkan, rebuild descriptors
	}
}

CVAR(Bool, gl_precache, true, CVAR_ARCHIVE)

// [SELACO PRECACHE] Selaco's precache switches, under Selaco's names (read by PrecacheLevel in
// p_setup.cpp and hw_PrecacheTexture in hw_precache.cpp). Client-side: they choose what this
// machine loads at level start and never reach the playsim.
//   gl_precache_actors   -- the GAMEINFO and MAPINFO PrecacheClasses lists. On = as before.
//   debug_precache_actor -- every actor class, all its states, Precache: labels ignored: the worst
//                           case, for measuring level-start time and video memory. Not saved.
CVARD(Bool, gl_precache_actors, true, CVAR_ARCHIVE, "precache the actor classes GAMEINFO and MAPINFO list, at level start")
CVARD(Bool, debug_precache_actor, false, CVAR_NOSAVE, "precache every actor class at level start, to measure the worst case")


CUSTOM_CVAR(Int, gl_shadowmap_filter, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0 || self > 8) self = 1;
}

CVAR(Bool, gl_global_fade, false, CVAR_ARCHIVE)

CUSTOM_CVAR(Float, gl_global_fade_density, 0.001f, CVAR_ARCHIVE)
{
	if (self < 0.0001f) self = 0.0001f;
	if (self > 0.005f) self = 0.005f;
}
CUSTOM_CVAR(Float, gl_global_fade_gradient, 1.5f, CVAR_ARCHIVE)
{
	if (self < 0.1f) self = 0.1f;
	if (self > 2.f) self = 2.f;
}
CVAR(Color, gl_global_fade_color, 0x3f3f3f, CVAR_ARCHIVE)
CVAR(Bool, gl_global_fade_debug, false, 0)

CUSTOM_CVAR (Int, gl_storage_buffer_type, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("You must restart " GAMENAME " for this change to take effect.\n");
}

CVARD(Bool, gl_no_persistent_buffer, false, 0, "Disable persistent buffer storage support")
CVARD(Bool, gl_no_clip_planes, false, 0, "Disable clip planes support")
CVARD(Bool, gl_no_ssbo, false, 0, "Disable SSBO support")
CVARD(Bool, vr_scene_multithread, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Allow VR BSP scene-build jobs on a worker thread")

CVAR(Bool, gl_strict_gldefs_errors, false, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)

// [GPUPARTICLES] Stateless GPU-drawn particles -- hw_gpuparticlebuffer.h,
// gpuparticles.vp, and "Engine docs/GPU_PARTICLES_PLAN.md".
//
// Every visual knob is a cvar so it can be tuned in the headset. The four
// tuning values reach the shader through HWViewpointUniforms::mGpuParticleParams,
// filled by the renderer every frame, so they respond while a menu is open.
//
// THE COST IS ADDITIVE OVERDRAW from large particles close to the eye, not the
// particle count -- which is why drawn size is capped in world units.
CVARD(Bool, r_gpuparticles, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "master switch for drawing GPU particles (Vulkan only)")
CVARD(Float, r_gpuparticles_sizescale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "multiplies every GPU particle's size")
// [F5] Default 64 since 2026-09-14 (owner: effects our mods use default ON at good
// quality): flame puffs and smoke draw at their definitions' own sizes, which their
// own maxsize still caps. It was 8, which drew every big puff as a spark. An ini that
// already stores 8 keeps 8 (RS_Ballistics' "All effects on" row sets it).
CVARD(Float, r_gpuparticles_maxsize, 64.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "world-unit cap on a GPU particle's drawn size")
CVARD(Float, r_gpuparticles_stretch, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "multiplies GPU particle velocity stretch")
CVARD(Float, r_gpuparticles_intensity, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "multiplies GPU particle brightness")
// [2a] Soft particles ("Engine docs/GPU_PARTICLES_STAGE2_PLAN.md" 2a). A particle
// fades over this many map units where it meets a surface, instead of cutting
// hard into it. It needs the scene depth readable during the particle draw, so a
// value above 0 makes RenderTranslucent switch the scene pass to a read-only
// depth pass around that one draw (FRenderState::SetSceneDepthReadable).
// DEFAULT 8 since 2026-09-14 (owner: effects our mods use default ON): that switch
// runs for every particle draw once the scene-depth variants compile (perf log
// fx.depthread). 0 = off: no switch, no new pass or pipeline, the frame exactly as
// before 2a. Renderer-read every frame into HWViewpointUniforms::
// mGpuParticleParams2.x, so the menu slider responds while the menu is open.
// Vulkan only; GL and GLES never draw GPU particles.
CVARD(Float, r_gpuparticles_soft, 8.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "GPU particles fade over this many map units where they meet a surface; 0 = off (Vulkan only)")
CUSTOM_CVARD(Int, r_gpuparticles_ringsize, 65536, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL, "GPU particle ring capacity; takes effect on restart")
{
	Printf("You must restart " GAMENAME " for this change to take effect.\n");
}
// Diagnostics: a line every two seconds with written / uploaded / drawn counts.
// Off by default so nothing prints per frame unless asked.
CVARD(Bool, r_gpuparticles_debug, false, 0, "print GPU particle spawn, upload and draw counts every two seconds")
// [PARTICLEDEFS] The `particles` CCMD -- the particle definitions and how full the
// inline cache is -- lives beside the table, in gamedata/particledefs.cpp.

// [2b] THE LEGACY PARTICLE PATH -- a temporary A/B switch for stage 2b ("Engine
// docs/GPU_PARTICLES_STAGE2_PLAN.md" 2b). OFF, the default, SpawnGpuParticles
// bursts become inline particle definitions (gamedata/particledefs.cpp) and draw
// through gpuparticles.vp's definitions path. ON, they write stage 1 records,
// tagged, which the shader draws with the stage 1 code. The two must look the
// same; this switch exists to prove that in the headset and goes once it has.
//
// Read at SPAWN, in C++ (GpuParticlesLegacyPath), because it chooses the layout a
// record is written with; a ring holding both kinds draws correctly. Nothing spawns
// while a menu is open, so the first burst after closing it uses the chosen path.
// Not CVAR_ARCHIVE: an A/B lasts one session, like r_beams_drawn. SpawnParticles
// (named definitions) is not affected.
CVARD(Bool, r_gpuparticles_legacy, false, CVAR_GLOBALCONFIG, "SpawnGpuParticles writes stage 1 records instead of inline particle definitions (stage 2b A/B test, Vulkan only)")
bool GpuParticlesLegacyPath()
{
	return r_gpuparticles_legacy;
}

// [2c] THE PARTICLE ATLAS LAYER SIZE ("Engine docs/GPU_PARTICLES_STAGE2_PLAN.md" 2c).
// Every textured particle definition's frames live in one texture array, one square
// layer per frame. This is a layer's side in pixels: 128, 256 or [ATLASBC7] 512
// (anything else rounds to the nearest). With mips, 256 layers are 21.3 MiB of VRAM at
// 128, 85.3 MiB at 256 and 341.3 MiB at 512; only the layers loaded definitions use are
// allocated, and with no textured definition the atlas is a 1 x 1 placeholder.
// [ATLASBC7] This is the UNCOMPRESSED atlas: PNG and Doom-graphic flipbooks, and BC7
// ones the compressed atlas does not take. The compressed atlas has its own side, below.
//
// Renderer-read: VkTextureManager::BeginFrame compares it every frame with the size
// the atlas was built at and rebuilds on the next frame after a change, menu open or
// not. Layer numbers do not depend on it, so no definition changes. Vulkan only.
CVARD(Int, r_gpuparticles_atlas_size, 256, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "uncompressed particle atlas layer side in pixels, 128, 256 or 512; the atlas rebuilds at once (Vulkan only)")
int GpuParticleAtlasLayerSize()
{
	return r_gpuparticles_atlas_size < 192 ? 128 : r_gpuparticles_atlas_size < 384 ? 256 : 512;
}

// [ATLASBC7] THE COMPRESSED PARTICLE ATLAS ("Engine docs/PARTICLE_ATLAS_COMPRESSED_PLAN.md", "Engine docs/
// PARTICLE_ATLAS_COMPRESSED_IMPL_NOTES.md"). A flipbook whose every frame is a premultiplied BC7 DDS goes to a second texture
// array, uploaded as the files store it -- a quarter of the uncompressed atlas's memory at the same side -- and the uncompressed
// atlas keeps everything else exactly as before. All four settings are renderer-read: HWDrawInfo::ProcessScene builds the
// atlas settings from them every scene (GpuParticleAtlasPolicy, below), the CPU table lays the frames out again when they
// changed, and VkTextureManager::BeginFrame rebuilds the atlas that changed on the next frame -- with the menu open. Rebuilding
// the compressed atlas reads its frames from the packages again: a pause of a second or a few. Presentation only (not
// SERVERINFO): the layout changes nothing but this machine's pixels. Vulkan only; GL and GLES draw no GPU particles.
//
//   r_gpuparticles_atlas_compressed       1 (default, owner 2026-09-15): BC7 flipbooks use the compressed atlas. 0: every
//                                         flipbook goes to the uncompressed atlas, decoded on the CPU -- it should look the
//                                         same at 4x the memory (the side-by-side check). A device that cannot sample BC7
//                                         arrays behaves as 0.
//   r_gpuparticles_atlas_compressed_size  the compressed atlas's side cap: 256, 512 (default, owner) or 1024 pixels (anything
//                                         else rounds to the nearest). The atlas is never bigger than its biggest book, so a
//                                         high setting costs nothing until books that big are loaded. A bigger book uploads
//                                         from a smaller stored level; a smaller book goes to the uncompressed atlas.
//   r_gpuparticles_atlas_layers           frames each atlas may hold, 256 .. 2048 (default) in steps of 256, never more than
//                                         the device allows. Memory is spent only on the frames loaded flipbooks use.
//   r_gpuparticles_atlas_overflow         when flipbooks want more frames than that: 1 (default, owner) long flipbooks play
//                                         fewer frames over the same time, so every effect still shows; 0 the flipbooks past
//                                         the cap draw as plain glowing dots.
CVARD(Bool, r_gpuparticles_atlas_compressed, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "BC7 DDS particle flipbooks use the compressed particle atlas; off decodes them into the uncompressed atlas at 4x the memory (Vulkan only)")
CVARD(Int, r_gpuparticles_atlas_compressed_size, 512, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "compressed particle atlas layer side cap in pixels, 256, 512 or 1024; the atlas rebuilds at once (Vulkan only)")
CVARD(Int, r_gpuparticles_atlas_layers, 2048, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "flipbook frames each particle atlas may hold, 256-2048 in steps of 256 (Vulkan only)")
CVARD(Int, r_gpuparticles_atlas_overflow, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "when particle flipbooks want more frames than an atlas holds: 1 long flipbooks play fewer frames, 0 the last ones draw as plain dots (Vulkan only)")

int GpuParticleAtlasCompressedSide()
{
	return r_gpuparticles_atlas_compressed_size < 384 ? 256 : r_gpuparticles_atlas_compressed_size < 768 ? 512 : 1024;
}

unsigned GpuParticleAtlasLayersPerAtlas()
{
	const int step = (int)ParticleDefinitionBuffer::ATLAS_LAYERS_MIN;
	const int clamped = std::clamp((int)r_gpuparticles_atlas_layers, step, (int)ParticleDefinitionBuffer::ATLAS_LAYERS_MAX);
	return (unsigned)((clamped + step / 2) / step * step);
}

ParticleAtlasPolicy GpuParticleAtlasPolicy()
{
	ParticleAtlasPolicy policy;
	policy.Compressed = r_gpuparticles_atlas_compressed && screen != nullptr && screen->SupportsBC7TextureArrays();
	policy.CompressedSide = GpuParticleAtlasCompressedSide();
	policy.LayersPerAtlas = GpuParticleAtlasLayersPerAtlas();
	const int deviceLayers = screen != nullptr ? screen->GetMaxTextureArrayLayers() : 0;
	if (deviceLayers > 0 && (unsigned)deviceLayers < policy.LayersPerAtlas)
		policy.LayersPerAtlas = (unsigned)deviceLayers;
	policy.ThinOverflow = r_gpuparticles_atlas_overflow != 0;
	return policy;
}

// [2d] THE VIEW LIGHTS LIT PARTICLES SEE ("Engine docs/GPU_PARTICLES_STAGE2_PLAN.md" 2d).
// How many of the dynamic lights in view -- nearest the eye first -- light particle
// definitions that are lit (`lit` above 0) and occlude (alpha above 0): 0..32
// (ViewLightBuffer::CAPACITY). 0 = sector light at spawn only, and the list is never
// filled. The list is only filled while such a particle is alive, and nothing additive
// or unlit reads it, so no existing effect changes at any value.
//
// Renderer-read every main-view scene (HWDrawInfo::ProcessScene), so the slider responds
// with the menu open. perflog.txt times the fill as fx.viewlights. Vulkan only.
CVARD(Int, r_gpuparticles_lights, 32, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "how many dynamic lights in view light lit GPU particles, nearest first, 0-32; 0 = sector light only (Vulkan only)")

// [PARTICLECULL] E11: CULL AND LOD GPU PARTICLES IN THE VERTEX SHADER ("Engine docs/EFFECTS_OPTIMIZATION_PLAN.md" E11,
// "Engine docs/PARTICLE_CULL_E11_IMPL_NOTES.md"). Both reach gpuparticles.vp through HWViewpointUniforms::mGpuParticleParams2.w,
// packed by HWDrawInfo::StartScene every scene, so they respond with a menu open. Presentation only: each machine decides from its
// own view. Vulkan only.
//
// r_gpuparticles_cull: a GPU particle or debris billboard whose whole quad lies outside the view collapses before its lights,
// flipbook and look. It had nothing to draw, so the image is the same (the notes' draw harness): the perf log's A/B switch and a
// way back, not a look. Default on, not saved.
CVARD(Bool, r_gpuparticles_cull, true, CVAR_GLOBALCONFIG, "GPU particles wholly outside the view skip their vertex work -- the same image, less work (Vulkan; an A/B switch)")
// r_gpuparticles_light_lod_distance: past this many map units from the eye, lit GPU particles and debris billboards (smoke, dust,
// chips) stop taking the effect lights (sparks, embers, impacts, tracers) and skip that light loop, fading out of them over the
// last quarter of the distance. The view lights and the sector light still light them. A LOOK CHANGE, so 0, off, until the owner
// picks a value: exactly the look before E11.
CVARD(Float, r_gpuparticles_light_lod_distance, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "past this distance in map units lit GPU particles skip the effect lights, fading out over its last quarter; 0 = off (Vulkan only)")

// The ring size, latched the first time anything asks. The CPU ring on
// FLevelLocals and the GPU ring both size from this, so they can never
// disagree within one run -- which is what "takes effect on restart" means.
int GpuParticleRingCapacity()
{
	static int latched = 0;
	if (latched == 0)
	{
		int v = r_gpuparticles_ringsize;
		if (v < 1024) v = 1024;
		if (v > (1 << 20)) v = 1 << 20;
		latched = v;
	}
	return latched;
}

// [DRAWNLINES] + [BEAMLINES] Glowing lines drawn as boxes rather than lit per
// pixel -- hw_drawnlinebuffer.h, drawnlines.vp/.fp, and FLevelLocals::DrawnLine.
//
// r_beams_drawn is the A/B switch. OFF, the default, every SetBeam line renders
// exactly as it always has. ON, the beam slots' glow in the air is drawn by the
// drawn-line path instead: the same maths, run only near each line. It falls
// back to per-pixel on its own wherever that path does not exist (GL, GLES, a
// failed shader compile, r_drawnlines 0), and says so when toggled.
// Not CVAR_ARCHIVE: an A/B switch lasts one session, so a stray click in the
// laser menu cannot quietly move the grab lasers and the Lance off their look.
CUSTOM_CVARD(Bool, r_beams_drawn, false, CVAR_GLOBALCONFIG | CVAR_NOINITCALL,"draw SetBeam lines through the drawn-line path instead of per pixel (A/B test, Vulkan only)")
{
	DrawnLinesLogToggle(self);
}
// While r_beams_drawn routes the beams, keep their SURFACE light per pixel --
// the walls a beam passes still brighten. The drawn path cannot light surfaces;
// 0 shows the pure drawn look, which is what SetDrawnLine lines always get.
CVARD(Bool, r_beams_drawn_surfacelight, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "while r_beams_drawn routes beams, keep their per-pixel surface light")
// Master switch for the drawn-line draw, like r_gpuparticles. Off also sends
// r_beams_drawn back to per-pixel beams.
CVARD(Bool, r_drawnlines, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "master switch for drawing drawn lines (Vulkan only)")
// How far toward the eye a drawn line's depth is pulled, in map units, so the
// glow around an impact is not sliced off where it meets the wall. Negative =
// automatic: the line's own halo reach. Smaller hides the line sooner behind
// something standing just in front of it; see drawnlines.fp. Renderer-read.
CVARD(Float, r_drawnlines_depthbias, -1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "drawn-line depth pulled toward the eye, map units; < 0 = the line's halo reach")
// Diagnostics: a line every two seconds with claimed / styled / uploaded / drawn
// counts. Off by default so nothing prints per frame unless asked.
CVARD(Bool, r_beams_debug, false, 0, "print beam slot and drawn-line counts every two seconds")

// A FIXED number, not a cvar. Script reads it (DrawnLineCapacity), and a value
// each player sets in their own ini is one gameplay code could branch on and
// desync. The level's array and the GPU buffer both size from it. 8192 lines is
// 917 KB of GPU records (112 B each since [F1]); raise the constant, not a
// setting, if it is ever short.
int DrawnLineCapacity()
{
	return 8192;
}

// [HEATREFRACTION] HEAT SHIMMER ("Engine docs/FLAME_ENGINE_PLAN.md" F2): the image
// behind a heat source (LevelLocals.SetHeatSource) bends, per eye, depth-aware.
// The pass is PPHeatRefraction (hw_postprocess.h); the sources are resolved in
// hw_drawinfo.cpp (SetupHeatSources). All three are renderer-read every frame, so
// they respond with a menu open. Vulkan only: GL and GLES skip the effect.
//
// ON BY DEFAULT since 2026-09-14 (owner: effects our mods use default ON). With no heat
// source published the pass still returns before it draws or allocates anything, so a
// mod that sets no source costs nothing. Off means SKIPPED: the frame is exactly the
// frame without this feature. It bends everything behind a heat source -- grab lasers
// and the Lance included (the owner's A/B is a check, not an off switch). Archived, so
// a choice made in the menu survives a restart; an ini that already stores 0 keeps 0
// (RS_Ballistics' "All effects on" row sets it).
CVARD(Bool, r_heatrefraction, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "bend the image behind heat sources (heat shimmer); off = the pass never runs (Vulkan only)")
// Multiplies every heat source's strength, so the shimmer can be judged in the
// headset without a script change. 0 draws nothing but still runs the pass (use
// r_heatrefraction to skip it); clamped to 0..4 where it is read.
CVARD(Float, r_heatrefraction_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "heat shimmer strength multiplier for every heat source, 0-4 (Vulkan only)")
// A test heat source that needs no mod: a rising column 96 map units ahead of where
// you look at the moment this is switched on, fixed in the world until it is switched
// off. Renderer-side only -- it is not a level slot, nothing in the playsim sees it.
// Not archived: it is for an A/B, and it should not be waiting in the next session.
CVARD(Bool, r_heatrefraction_test, false, CVAR_GLOBALCONFIG, "a test heat source ahead of where you look when switched on (heat shimmer A/B, Vulkan only)")

// [SMOKEVOLUME] THE SMOKE VOLUME ("Engine docs/SMOKE_VOLUME_PLAN.md" #13): a world-aligned
// 3D grid around the eye that gunsmoke fills, walls stop and blasts clear. 13a is the
// compute foundation, 13b the simulation and the mod API (EmitSmoke, CarveSmoke,
// PushEffectImpulse...); the drawing is 13c, so nothing draws yet. All of these are
// renderer-read every frame (PrepareFrameCompute, hw_entrypoint.cpp), so they respond with
// a menu open. Vulkan only: GL and GLES run no compute.
//
// ON BY DEFAULT (owner, 2026-09-14: effects our mods use default ON). The capability is
// inert until something asks for smoke -- a mod's first EmitSmoke / CarveSmoke of a map
// or r_smoke_computetest -- so a map nobody smokes allocates and dispatches nothing.
// Off frees the volume at once. Archived.
CVARD(Bool, r_smoke, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "the smoke volume; nothing is allocated until a mod emits smoke (Vulkan only)")
// Resolution and area together, 25 bytes a cell (SmokeGridForQuality, hw_framecompute.h):
// 1 = 192 x 192 x 64 cells at 10 units (about 59 MB), 2 = 256 x 256 x 96 at 8 (about
// 157 MB, the default), 3 = 320 x 320 x 128 at 8 (about 328 MB), 4 = 384 x 384 x 128 at 8
// (about 472 MB). A change re-allocates the volume, which clears the smoke in it.
CUSTOM_CVARD(Int, r_smoke_quality, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "smoke volume resolution and area together, 1-4 (2 = about 157 MB; Vulkan only)")
{
	if (self < 1) { self = 1; return; }
	if (self > 4) { self = 4; return; }
}
// A test source that needs no mod ([13b]; 13a's was a test pattern): placed 128 map units
// ahead of where you look when switched on, fixed in the world until switched off -- a
// puff every tic, a round carving through it every 10 tics, a blast every 3 seconds --
// through the same queues-to-GPU path as a mod's events, so the perf log shows fx.compute,
// fx.smokesim and fx.smokemask for the real simulation. Renderer-side only: no level slot,
// nothing in the playsim sees it. Not archived: a measurement, not a setting.
CVARD(Bool, r_smoke_computetest, false, CVAR_GLOBALCONFIG, "a smoke test source ahead of where you look when switched on: runs the real smoke simulation with no mod (Vulkan only)")
// [13b] The player's "Smoke fade speed": multiplies the dissipation of the level's smoke
// look (SetSmokeLook), so how long smoke hangs can be judged without a script change -- a
// script-fed value would do nothing while a menu is open (review S9). 1 = the mod's own
// look, 0 = smoke never thins on its own; clamped 0..16 where it is read. Renderer-read at
// every simulation step (a menu pauses the world, so the change shows the moment it
// closes). Archived.
CVARD(Float, r_smoke_dissipation_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "multiplies how fast smoke thins (the mod's dissipation), 0-16; 0 = it hangs (Vulkan only)")
