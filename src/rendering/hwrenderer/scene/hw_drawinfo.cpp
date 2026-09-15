/*
** hw_drawinfo.cpp
**
** Basic scene draw info management class
**
**---------------------------------------------------------------------------
**
** Copyright 2000-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include <algorithm>
#include <cstddef>	// [2d] offsetof, for the particle record asserts
#include "a_sharedglobal.h"
#include "r_utility.h"
#include "r_sky.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "hw_fakeflat.h"
#include "hw_portal.h"
#include "hw_renderstate.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hw_drawinfo.h"
#include "po_man.h"
#include "models.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hw_drawnlinebuffer.h"	// [DRAWNLINES]
#include "i_time.h"	// [DRAWNLINES] r_beams_debug's two-second gate
#include "hw_gpuparticlebuffer.h"	// [GPUPARTICLES]
#include "hw_particledefbuffer.h"	// [PARTICLEDEFS] the GPU copy of the definitions
#include "particledefs.h"	// [PARTICLEDEFS] the CPU table it syncs from
#include "hw_viewlightbuffer.h"	// [VIEWLIGHTS] the dynamic lights in view, for lit particles
#include "hw_meshparticles.h"	// [MESHPARTICLES] mesh particles: their sync, the view light gate, the opaque-pass draw
#include "hw_debrispool.h"	// [DEBRISPOOL] the debris pool: its gates and its two draws
#include "hw_effectlights.h"	// [EFFECTLIGHTS] ResolveDrawnLineEnds is declared there
#include "a_dynlight.h"	// [VIEWLIGHTS] FDynamicLight, walked to fill it
#include "hw_perflog.h"	// RS FORK -- r_perflog scene/effects GPU groups
#include "hw_smokevolume.h"	// [SMOKEVOLUME] SmokeVolume::GetDrawState, for SetupSmokeVolume
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"	// [SMOKEVOLUME] r_smoke_steps, r_smoke_density_scale, r_smoke_debugslice
#include "hw_emissivevolumes.h"	// [EMISSIVEVOLUMES] EmissiveVolumes::GetDrawState, for SetupEmissiveVolumes
#include "hw_vrmodes.h"
#include "hw_vrwheel.h"
#include "hw_clipper.h"
#include "v_draw.h"
#include "a_corona.h"
#include "texturemanager.h"
#include "actorinlines.h"
#include "g_levellocals.h"

void DrawLaserSightWorld(FRenderState& state);
void DrawHitscanTracers(FRenderState& state);

EXTERN_CVAR(Float, r_visibility)
EXTERN_CVAR(Int, gl_max_portals);
EXTERN_CVAR(Bool, r_visualstate_log)	// RS fork: defined in vmthunks.cpp
EXTERN_CVAR(Int, r_gpuparticles_looks)	// [LOOKS] defined in hw_particledefbuffer.cpp
CVAR(Bool, gl_bandedswlight, false, CVAR_ARCHIVE)
CVAR(Bool, gl_sort_textures, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_no_skyclear, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, gl_enhanced_nv_stealth, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, gl_texture, true, 0)
CVAR(Float, gl_mask_threshold, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_mask_sprite_threshold, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, gl_coronas, true, CVAR_ARCHIVE);

// [BB] How many samples the volumetric beam takes along each pixel's ray.
// The single knob that trades beam quality for framerate: fewer steps means
// coarser haze, not a dimmer beam, because the march normalises by count.
CVAR(Int, vol_beam_quality, 24, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// [BB] How much the volumetric cone fades as your VIEW lines up with it.
//
// A cone seen end-on is a disc, and on a flat screen the default mount points
// exactly where you look -- so without this the beam is a permanent soft halo
// over the middle of the frame, fed straight into bloom, carrying no
// information at all because it marks the place you are already looking.
//
// Not a rule, because in VR a tracked hand pointed forward is a real thing
// somebody might want to see. 0 restores the unfaded behaviour.
CVAR(Float, vol_beam_axisfade, 0.85f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// [BB] How close to the eye the beam's AXIS must pass for the fade above to
// apply, in map units. Full fade within a quarter of this, none beyond it.
//
// The fade used to depend on direction alone, so a VR hand torch pointed where
// you look -- apex on a hand a couple of feet from the eye, cone seen from the
// side -- kept 15% of its light. A head or view mount (a few units off) still
// gets the whole fade. 0 restores the direction-only fade for every beam.
CVAR(Float, vol_beam_axisfade_reach, 12.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// [BB] Volumetric beam diagnostics that can change often (the fog's torch slot
// under flicker, the per-slot axis fade band). Off by default; the rare-event
// lines (pose source, projection offset, MSAA variant, scene viewport) always
// print, once per change.
CVAR(Bool, vol_beam_debug, false, 0);

// [BB] Smooth segment beams between tics instead of stepping at 35Hz.
//
// Beams are written from script, so they only change 35 times a second while
// the view redraws 90-120. Off, a beam is exactly where script last put it and
// visibly stutters against the world; on, it is drawn where it was passing
// through at this instant. An escape hatch, not a taste setting -- leave it on
// unless a beam is doing something strange, in which case turning it off says
// whether the interpolation or the script is at fault.
CVAR(Bool, r_beam_interpolate, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

static_assert(DrawnLineBuffer::ROUTED_BEAM_RESERVE == (unsigned)FLevelLocals::MAX_BEAMS,
	"DrawnLineBuffer reserves one record per beam slot for r_beams_drawn");

// [2d] GpuParticleBuffer::Sync reads each uploaded record's birth, life, definition slot and
// plane / legacy tag at these offsets to know what its particles need from the draw.
static_assert(sizeof(FLevelLocals::GpuParticleRecord) == GpuParticleBuffer::RECORD_BYTES &&
	offsetof(FLevelLocals::GpuParticleRecord, a) + 3 * sizeof(float) == GpuParticleBuffer::RECORD_BIRTH_OFFSET &&
	offsetof(FLevelLocals::GpuParticleRecord, b) + 3 * sizeof(float) == GpuParticleBuffer::RECORD_LIFE_OFFSET &&
	offsetof(FLevelLocals::GpuParticleRecord, d) == GpuParticleBuffer::RECORD_DEFINITION_OFFSET &&
	offsetof(FLevelLocals::GpuParticleRecord, e) == GpuParticleBuffer::RECORD_PLANE_OFFSET,
	"GpuParticleBuffer's record offsets must match FLevelLocals::GpuParticleRecord: a.w birth, b.w life, d.x definition, e.x plane");

//==========================================================================
//
// [BEAMLINES] WHERE A LINE IS THIS FRAME, AND HOW IT LOOKS.
//
// Moved out of StartScene's beam upload unchanged, so the per-pixel upload and
// the drawn-line path (r_beams_drawn, SyncDrawnLines) resolve a beam slot with
// the same arithmetic and can never disagree about where it is. Game space
// out; the callers swizzle.
//
//==========================================================================

// The last StartScene's per-pixel upload count, for r_beams_debug.
static int BeamLinesUploaded = 0;

// RS FORK -- AN ANCHORED LINE STARTS AT THE HAND, NOW.
//
// Not at an interpolation between two 35Hz samples of where the hand was.
// AttackPos and OffhandPos are rewritten every frame by hw_vrmodes.cpp from the
// live controller transform, and this runs in that same frame -- so reading
// them here is the hand's actual current position rather than a stale pair.
// That is the whole fix for a laser sight that stutters while you move.
//
// ONLY THE START. The far end is a hit location in the world, which genuinely
// only changes once a tic and is correctly interpolated by the caller --
// anchoring it too would drag the far end around with your wrist.
// See FLevelLocals::BeamAnchor.
// playerNum -1 is the console player, the beam slots' long-standing rule; a drawn
// line anchored with an owner names that player instead.
static void ResolveLineAnchor(FLevelLocals *Level, int mode, DVector3 &start, int playerNum = -1)
{
	if (mode == 0) return;
	const player_t *bp = (playerNum >= 0 && playerNum < MAXPLAYERS && Level->PlayerInGame(playerNum))
		? &players[playerNum] : Level->GetConsolePlayer();
	if (bp && bp->mo)
		start = (mode == 2) ? bp->mo->OffhandPos : bp->mo->AttackPos;
}

static void ResolveBeamSlot(FLevelLocals *Level, int i, double ticFrac, DVector3 &a, DVector3 &b)
{
	// INTERPOLATE ONLY A BEAM THAT WAS ALREADY LIT AND STILL IS.
	// Callers park a released slot at (0,0,0) rather than leaving stale
	// endpoints behind, so both transitions have to snap: on the tic a beam
	// lights up prev is the map origin and lerping would drag it across the
	// level, and on the tic it goes out the same thing happens in reverse.
	// Between those it is the same beam moving, which is exactly what wants
	// smoothing.
	//
	// [BEAMLINES] PrevBeamLive[i] is the per-slot form of the old
	// `i < PrevBeamCount`, and identical to it while nothing is claimed.
	const bool lerpable =
		Level->PrevBeamLive[i] &&
		Level->PrevBeamIntensity[i] > 0.0 &&
		Level->BeamIntensity[i] > 0.0;
	const double f = lerpable ? ticFrac : 1.0;

	a = Level->PrevBeamStart[i] +
		(Level->BeamStart[i] - Level->PrevBeamStart[i]) * f;
	b = Level->PrevBeamEnd[i] +
		(Level->BeamEnd[i] - Level->PrevBeamEnd[i]) * f;

	if (Level->BeamAnchor[i] != 0)
		ResolveLineAnchor(Level, Level->BeamAnchor[i], a);
}

// x air glow, y halo, z taper, w flare: the slot's own style, or the scene look.
// The scene branch produces exactly the floats StartScene used to put in
// mBeamParams.y/.w and mBeamFX.z/.w.
static FVector4 BeamSlotLook(const FLevelLocals *Level, int i)
{
	if (Level->BeamHasStyle[i])
		return { (float)Level->BeamStyleAirGlow[i], (float)Level->BeamStyleHalo[i],
			(float)Level->BeamStyleTaper[i], (float)Level->BeamStyleFlare[i] };
	return { (float)Level->BeamAirGlow, (float)Level->BeamGlow,
		(float)Level->BeamTaper, (float)Level->BeamFlare };
}

// [13e] The smoke volume's beam list (hw_smokevolume.h, hw_smokevolume.cpp GatherBeams) resolves a slot through the two
// functions above, so the light a beam scatters in the smoke, and the glow the smoke drawing restores, sit exactly where
// the per-pixel upload (StartScene) and the drawn-line path (SyncDrawnLines) put the beam. Read-only.
bool ResolveBeamLine(FLevelLocals *Level, int slot, double viewTicFrac, DVector3 &a, DVector3 &b, FVector4 &look)
{
	if (Level == nullptr || slot < 0 || slot >= FLevelLocals::MAX_BEAMS)
		return false;
	if (!Level->BeamSlotLive(slot) || Level->BeamIntensity[slot] == 0.0)
		return false;
	ResolveBeamSlot(Level, slot, r_beam_interpolate ? viewTicFrac : 1.0, a, b);
	look = BeamSlotLook(Level, slot);
	return true;
}

//==========================================================================
//
// [DRAWNLINES] This frame's drawn-line records, handed to the GPU.
//
// Everything is re-sent every scene: a line is interpolated between tics and
// may start at a tracked hand, so its endpoints move every frame whether script
// touched it or not. That is at most capacity x 112 bytes of plain copy. Nothing
// in a record depends on the eye, so both eyes -- and a camera texture in the
// same frame -- send identical bytes; each eye's box and glow are worked out in
// the shaders from that eye's own camera.
//
// Beam slots go FIRST when r_beams_drawn routes them, so a full SetDrawnLine
// array can never push the grab lasers or the Lance off the end.
//
//==========================================================================

static void WriteDrawnLineRecord(DrawnLineRecord &r, const DVector3 &a, const DVector3 &b,
	double thick, double soft, PalEntry col, double intensity,
	const FVector4 &look, double scrollSpeed, double scrollDepth, float timerSec, float depthBias)
{
	// Swizzled like the beam uniforms: Doom's Z is the shader's Y.
	r.a[0] = (float)a.X; r.a[1] = (float)a.Z; r.a[2] = (float)a.Y; r.a[3] = (float)thick;
	r.b[0] = (float)b.X; r.b[1] = (float)b.Z; r.b[2] = (float)b.Y; r.b[3] = (float)soft;
	r.c[0] = col.r / 255.f; r.c[1] = col.g / 255.f; r.c[2] = col.b / 255.f; r.c[3] = (float)intensity;
	r.d[0] = look.X; r.d[1] = look.Y; r.d[2] = look.Z; r.d[3] = look.W;
	r.e[0] = (float)scrollSpeed; r.e[1] = (float)scrollDepth; r.e[2] = timerSec; r.e[3] = depthBias;
	// [F1] The gradient and turbulence looks, OFF: their stored defaults and look
	// flags 0, so drawnlines.vp/.fp never read f or g. Every record gets these --
	// routed beams always; a SetDrawnLine line that asked for a look has them
	// overwritten by WriteDrawnLineLooks.
	r.f[0] = r.c[0]; r.f[1] = r.c[1]; r.f[2] = r.c[2]; r.f[3] = 1.f;
	r.g[0] = 0.f; r.g[1] = 0.05f; r.g[2] = 1.5f; r.g[3] = 0.f;
}

// [F1] A SetDrawnLine line's opt-in looks, over the "off" f and g written above.
// A look that changes nothing -- a gradient to the line's own colour with swell 1,
// turbulence at strength 0 -- keeps its flag clear, so the line stays on the code
// path every line took before the looks existed. Values arrive clamped by the
// setters (FLevelLocals::SetDrawnLineGradient / SetDrawnLineTurbulence).
static void WriteDrawnLineLooks(DrawnLineRecord &r, const FLevelLocals::DrawnLine &l)
{
	unsigned flags = 0;
	if (l.HasGradient &&
		(l.ColorEnd.r != l.Color.r || l.ColorEnd.g != l.Color.g || l.ColorEnd.b != l.Color.b || l.Swell != 1.0))
	{
		r.f[0] = l.ColorEnd.r / 255.f; r.f[1] = l.ColorEnd.g / 255.f; r.f[2] = l.ColorEnd.b / 255.f;
		r.f[3] = (float)l.Swell;
		flags |= DrawnLineBuffer::LOOK_GRADIENT;
	}
	if (l.TurbulenceStrength > 0.0)
	{
		r.g[0] = (float)l.TurbulenceStrength; r.g[1] = (float)l.TurbulenceScale; r.g[2] = (float)l.TurbulenceSpeed;
		flags |= DrawnLineBuffer::LOOK_TURBULENCE;
	}
	r.g[3] = (float)flags;
}

static TArray<DrawnLineRecord> DrawnLineScratch;

static void SyncDrawnLines(FLevelLocals *Level, double viewTicFrac)
{
	DrawnLineBuffer *lines = screen->mDrawnLines;
	if (lines == nullptr || Level == nullptr) return;

	const unsigned cap = lines->GetCapacity();
	if (DrawnLineScratch.Size() < cap) DrawnLineScratch.Resize(cap);

	const double ticFrac = r_beam_interpolate ? viewTicFrac : 1.0;

	// The `timer` main.fp's scroll reads on a surface whose material runs at
	// speed 1 (VkRenderState::ApplyStreamData), so a routed beam scrolls in step
	// with the per-pixel beam it stands in for.
	const float timerSec = static_cast<float>((double)(screen->FrameTime - screen->RenderState()->firstFrame) / 1000.);
	const float depthBias = (float)r_drawnlines_depthbias;

	unsigned n = 0, routed = 0, dropped = 0;

	// [LIGHTMASK] While this frame draws the light mask (hw_postprocess.h, PPLightMask), a
	// routed beam's record is marked as pinned light, the class the per-pixel beam it stands
	// in for writes in main.fp. See DrawnLineBuffer::LIGHT_MASK_PINNED_MARK.
	const bool markPinnedLight = hw_postprocess.lightmask.Active();

	if (BeamsRouteToDrawnLines())
	{
		for (int i = 0; i < FLevelLocals::MAX_BEAMS; i++)
		{
			if (!Level->BeamSlotLive(i) || Level->BeamIntensity[i] == 0.0) continue;
			const FVector4 look = BeamSlotLook(Level, i);
			// No air glow, nothing in the air: the per-pixel air loop skips it too.
			if (look.X <= 0.f) continue;
			if (n >= cap) { dropped++; continue; }

			DVector3 a, b;
			ResolveBeamSlot(Level, i, ticFrac, a, b);
			WriteDrawnLineRecord(DrawnLineScratch[n++], a, b,
				Level->BeamThick[i], Level->BeamSoft[i], Level->BeamColor[i], Level->BeamIntensity[i],
				look, Level->BeamScrollSpeed, Level->BeamScrollDepth, timerSec, depthBias);
			if (markPinnedLight)
				DrawnLineScratch[n - 1].g[3] += DrawnLineBuffer::LIGHT_MASK_PINNED_MARK;	// [LIGHTMASK]
			routed++;
		}
	}

	for (int i = 0; i < Level->DrawnLineHigh; i++)
	{
		const FLevelLocals::DrawnLine &l = Level->DrawnLines[i];
		if (!l.Live || l.Intensity == 0.0 || l.AirGlow <= 0.0) continue;
		if (n >= cap) { dropped++; continue; }

		// The beam slots' rule, per line: lerp only a line that was lit last
		// tic and still is.
		const bool lerpable = l.PrevLive && l.PrevIntensity > 0.0 && l.Intensity > 0.0;
		const double f = lerpable ? ticFrac : 1.0;
		DVector3 a = l.PrevStart + (l.Start - l.PrevStart) * f;
		const DVector3 b = l.PrevEnd + (l.End - l.PrevEnd) * f;
		ResolveLineAnchor(Level, l.Anchor, a, l.AnchorPlayer);

		const FVector4 look = { (float)l.AirGlow, (float)l.Halo, (float)l.Taper, (float)l.Flare };
		DrawnLineRecord &rec = DrawnLineScratch[n++];
		WriteDrawnLineRecord(rec, a, b, l.Thick, l.Soft, l.Color, l.Intensity,
			look, l.ScrollSpeed, l.ScrollDepth, timerSec, depthBias);
		WriteDrawnLineLooks(rec, l);	// [F1] look flags stay 0 unless this line asked for a look
	}

	if (dropped > 0)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			Printf("DrawnLines: %u lines did not fit the %u-record buffer this frame and were not drawn "
				"(raise DrawnLineCapacity in hw_cvars.cpp); further overflows are silent\n", dropped, cap);
		}
	}

	lines->Upload(DrawnLineScratch.Data(), n, routed);
}

//==========================================================================
//
// [EFFECTLIGHTS] Where drawn line `index` is this frame, for the light it throws (SetDrawnLineLight, hw_effectlights.cpp).
// SyncDrawnLines' own interpolation and anchor, statement for statement, so the light sits exactly on the line it draws
// ("Engine docs/EFFECT_LIGHTS_CORE_IMPL_NOTES.md" checks that the two agree). Game space out. Read-only.
//
//==========================================================================

bool ResolveDrawnLineEnds(FLevelLocals *Level, int index, double viewTicFrac, DVector3 &a, DVector3 &b)
{
	if (Level == nullptr || index < 0 || index >= Level->DrawnLineHigh || (unsigned)index >= Level->DrawnLines.Size())
		return false;
	const FLevelLocals::DrawnLine &l = Level->DrawnLines[index];
	if (!l.Live)
		return false;

	const double ticFrac = r_beam_interpolate ? viewTicFrac : 1.0;

	// The beam slots' rule, per line: lerp only a line that was lit last
	// tic and still is.
	const bool lerpable = l.PrevLive && l.PrevIntensity > 0.0 && l.Intensity > 0.0;
	const double f = lerpable ? ticFrac : 1.0;
	a = l.PrevStart + (l.Start - l.PrevStart) * f;
	b = l.PrevEnd + (l.End - l.PrevEnd) * f;
	ResolveLineAnchor(Level, l.Anchor, a, l.AnchorPlayer);
	return true;
}

//==========================================================================
//
// [VIEWLIGHTS] This scene's view light list, handed to the GPU.
//
// The dynamic lights lit particles loop over in gpuparticles.vp (ViewLightBuffer).
// Filled for the main view only, and only while something reads it: a particle
// whose definition is lit and occludes is alive, r_gpuparticles_lights is above 0
// and dynamic lights are on (r_dynlights). Otherwise the list is emptied, which is
// no work at all once it is empty.
//
// WHICH LIGHTS. Active, with a radius, visible to the player (visibletoplayer), and
// not kept off actors (DONTLIGHTACTORS, DONTLIGHTOTHERS): a particle is lit the way
// a sprite is (HWDrawInfo::GetDynSpriteLight), and those flags keep a light off
// sprites. Of those, the ones whose sphere reaches into this view's frustum (either
// eye's, under multiview), nearest the eye first -- measured to the sphere, so
// standing inside a big light makes it nearest -- up to the slider. Colour as the
// sprite path: rgb x GLDEFS intensity, x the owner's alpha for LIGHTMULTALPHA,
// subtractive lights negative. Spot cones as hw_dynlightdata.cpp. No shadow map.
//
// NOT OCCLUSION. A light behind a wall, within its radius of smoke on this side,
// lights that smoke. The BSP has not run yet here (DrawScene runs it), and its
// section lists would not follow smoke drifting through a doorway either.
//
// World space, like the drawn lines: both eyes' draws read whatever the last scene
// wrote (per-eye rendering writes twice a frame, multiview once), and two eyes'
// lists can differ only at the far end of the nearest-N cut. Presentation only:
// nothing reads the list back, and it changes only this machine's pixels.
//
//==========================================================================

struct ViewLightCandidate
{
	double Distance;
	FDynamicLight *Light;
	float Color[3];
};

static TArray<ViewLightCandidate> ViewLightCandidates;
static ViewLightRecord ViewLightScratch[ViewLightBuffer::CAPACITY];

// The side and near planes of one view's frustum, in SHADER space (game x, z, y),
// taken from the rows of projection x view in GL clip conventions (-w <= x, y, z
// <= w) and normalised, so a plane's value at a point is a distance in map units.
struct ViewLightFrustum
{
	double Plane[5][4];
};

static void BuildViewLightFrustum(const HWViewpointUniforms &uniforms, ViewLightFrustum &frustum)
{
	const auto *projection = uniforms.mProjectionMatrix.get();
	const auto *view = uniforms.mViewMatrix.get();
	double m[16];	// column-major, m[column * 4 + row]
	for (int column = 0; column < 4; column++)
	{
		for (int row = 0; row < 4; row++)
		{
			double sum = 0.0;
			for (int k = 0; k < 4; k++)
				sum += (double)projection[k * 4 + row] * (double)view[column * 4 + k];
			m[column * 4 + row] = sum;
		}
	}

	// Row 3 plus or minus row 0 (left, right), row 1 (bottom, top), row 2 (near).
	static const int axis[5] = { 0, 0, 1, 1, 2 };
	static const double sign[5] = { 1.0, -1.0, 1.0, -1.0, 1.0 };
	for (int i = 0; i < 5; i++)
	{
		double lengthSquared = 0.0;
		for (int column = 0; column < 4; column++)
		{
			frustum.Plane[i][column] = m[column * 4 + 3] + sign[i] * m[column * 4 + axis[i]];
			if (column < 3) lengthSquared += frustum.Plane[i][column] * frustum.Plane[i][column];
		}
		const double length = sqrt(lengthSquared);
		if (length > 1e-12)
		{
			for (int column = 0; column < 4; column++)
				frustum.Plane[i][column] /= length;
		}
		else
		{
			// A degenerate matrix: this plane rejects nothing.
			frustum.Plane[i][0] = frustum.Plane[i][1] = frustum.Plane[i][2] = 0.0;
			frustum.Plane[i][3] = 1.0;
		}
	}
}

static bool ViewLightReachesFrustum(const ViewLightFrustum &frustum, double x, double y, double z, double radius)
{
	for (int i = 0; i < 5; i++)
	{
		const double *plane = frustum.Plane[i];
		if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < -radius)
			return false;
	}
	return true;
}

static void SyncViewLights(const HWDrawInfo *di)
{
	ViewLightBuffer *viewLights = screen->mViewLights;
	if (viewLights == nullptr) return;

	FLevelLocals *Level = di->Level;
	const GpuParticleBuffer *particles = screen->mGpuParticles;
	const int wanted = clamp((int)r_gpuparticles_lights, 0, (int)ViewLightBuffer::CAPACITY);
	// [MESHPARTICLES] Filled for a lit mesh particle as well: a chunk takes its one light value from
	// this list (meshparticles.vp). With no mesh definition this is the test it always was.
	const float viewLightTime = di->VPUniforms.mLevelTime.X;
	const bool litParticlesAlive = particles != nullptr && particles->IsDrawable() && particles->LitAliveAt(viewLightTime);
	const MeshParticleBuffer *meshParticles = screen->mMeshParticles;
	const bool litMeshParticlesAlive = meshParticles != nullptr && meshParticles->IsDrawable() && meshParticles->LitAliveAt(viewLightTime);
	// [DEBRISPOOL] And for a lit piece of the debris pool, billboard or mesh.
	const bool litDebrisAlive = DebrisPool::Get().LitAliveAt(viewLightTime);
	if (Level == nullptr || Level->lights == nullptr || !r_dynlights || !r_gpuparticles || wanted <= 0 ||
		!(litParticlesAlive || litMeshParticlesAlive || litDebrisAlive))
	{
		viewLights->Upload(nullptr, 0);
		return;
	}

	// fx.viewlights: CPU time of the fill, main view only (perflog.txt, cpu_fx_ms).
	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	ViewLightFrustum frustum[2];
	const int frustumCount = di->HasMultiviewViewpoints ? 2 : 1;
	if (di->HasMultiviewViewpoints)
	{
		BuildViewLightFrustum(di->MultiviewVPUniforms[0], frustum[0]);
		BuildViewLightFrustum(di->MultiviewVPUniforms[1], frustum[1]);
	}
	else
	{
		BuildViewLightFrustum(di->VPUniforms, frustum[0]);
	}

	const DVector3 eye = di->Viewpoint.Pos;
	ViewLightCandidates.Clear();
	for (FDynamicLight *light = Level->lights; light != nullptr; light = light->next)
	{
		if (!light->IsActive() || !light->visibletoplayer || light->DontLightActors() || light->DontLightOthers())
			continue;
		const double radius = light->GetRadius();
		if (!(radius > 0.0))
			continue;

		bool inView = false;
		for (int f = 0; f < frustumCount && !inView; f++)
			inView = ViewLightReachesFrustum(frustum[f], light->Pos.X, light->Pos.Z, light->Pos.Y, radius);
		if (!inView)
			continue;

		// GetDynSpriteLight's colour.
		float lr = light->GetRed() / 255.f;
		float lg = light->GetGreen() / 255.f;
		float lb = light->GetBlue() / 255.f;
		if (light->target && (light->target->renderflags2 & RF2_LIGHTMULTALPHA))
		{
			const float alpha = (float)light->target->Alpha;
			lr *= alpha;
			lg *= alpha;
			lb *= alpha;
		}
		const float intensity = (float)light->GetLightDefIntensity();
		lr *= intensity;
		lg *= intensity;
		lb *= intensity;
		if (light->IsSubtractive())
		{
			const float bright = sqrtf(lr * lr + lg * lg + lb * lb);
			lr = (bright - lr) * -1;
			lg = (bright - lg) * -1;
			lb = (bright - lb) * -1;
		}
		if (lr == 0.f && lg == 0.f && lb == 0.f)
			continue;

		const double distance = (light->Pos - eye).Length() - radius;
		ViewLightCandidate candidate;
		candidate.Distance = distance > 0.0 ? distance : 0.0;
		candidate.Light = light;
		candidate.Color[0] = lr;
		candidate.Color[1] = lg;
		candidate.Color[2] = lb;
		ViewLightCandidates.Push(candidate);
	}

	unsigned count = ViewLightCandidates.Size();
	if (count > (unsigned)wanted)
	{
		std::nth_element(ViewLightCandidates.Data(), ViewLightCandidates.Data() + wanted, ViewLightCandidates.Data() + count,
			[](const ViewLightCandidate &a, const ViewLightCandidate &b) { return a.Distance < b.Distance; });
		count = (unsigned)wanted;
	}

	for (unsigned i = 0; i < count; i++)
	{
		const ViewLightCandidate &candidate = ViewLightCandidates[i];
		const FDynamicLight *light = candidate.Light;
		ViewLightRecord &record = ViewLightScratch[i];

		// Shader space: game x, z, y.
		record.origin[0] = (float)light->Pos.X;
		record.origin[1] = (float)light->Pos.Z;
		record.origin[2] = (float)light->Pos.Y;
		record.origin[3] = light->GetRadius();
		record.color[0] = candidate.Color[0];
		record.color[1] = candidate.Color[1];
		record.color[2] = candidate.Color[2];
		record.color[3] = 0.f;

		if (light->IsSpot())
		{
			// hw_dynlightdata.cpp's GetSpotlightShaderParams, uncached. A cone whose
			// inner and outer angles are equal would make smoothstep's edges equal.
			float cosInner = (float)light->pSpotInnerAngle->Cos();
			float cosOuter = (float)light->pSpotOuterAngle->Cos();
			if (!(cosOuter < cosInner)) cosOuter = cosInner - 1e-4f;
			const DAngle negPitch = -light->Pitch;
			const DAngle angle = light->Yaw;
			const double xzLength = negPitch.Cos();
			record.spotDirection[0] = float(-angle.Cos() * xzLength);
			record.spotDirection[1] = float(-negPitch.Sin());
			record.spotDirection[2] = float(-angle.Sin() * xzLength);
			record.spotCone[0] = cosOuter;
			record.spotCone[1] = cosInner;
		}
		else
		{
			record.spotDirection[0] = record.spotDirection[1] = record.spotDirection[2] = 0.f;
			record.spotCone[0] = -2.f;
			record.spotCone[1] = -1.f;
		}
		record.spotDirection[3] = 0.f;
		record.spotCone[2] = record.spotCone[3] = 0.f;
	}

	viewLights->Upload(ViewLightScratch, count);

	if (timed)
		PerfLog::AddCpuSample("fx.viewlights", (double)(I_nsTime() - startNs) / 1e6);
}

//==========================================================================
//
// [HEATREFRACTION] This scene's heat sources, resolved for each eye and handed
// to the heat shimmer pass (PPHeatRefraction, hw_postprocess.h;
// "Engine docs/FLAME_ENGINE_PLAN.md" F2).
//
// MAIN VIEW ONLY (ProcessScene with toscreen). A camera texture has no post pass,
// and a save picture's post pass must not bend with the main view's sources, so
// both clear the list. Called before DrawScene: portals run RenderScene again with
// their own views, so filling from there (as SetupVolumetricBeam does) would leave
// the last portal's view in the uniforms.
//
// PER EYE (review S8). ApplyMultiviewViewpoints leaves VPUniforms at the first eye,
// and the second eye of a multiview scene post-processes with no draw info of its
// own. So a multiview scene publishes a set for each eye, from
// MultiviewVPUniforms[0] and [1]; any other scene publishes one set from
// VPUniforms, and the next eye's own ProcessScene replaces it before its post.
//
// Nothing is published unless r_heatrefraction is on and the backend is Vulkan,
// so with the switch off the pass returns on its first line.
//
// WORLD AXES, RELATIVE TO THE EYE. Positions go to the shader as GL world axes
// (map x, map z, map y) minus this eye's position, in map units: view space carries
// the pixel stretch, which would squash every source, and eye-relative keeps the
// ray solve's numbers small.
//
//==========================================================================

static_assert(PPHeatRefraction::MAX_SOURCES >= FLevelLocals::MAX_HEAT_SOURCES + 1,
	"PPHeatRefraction::MAX_SOURCES must hold every heat source slot plus the r_heatrefraction_test source");

// One source as it stands this frame: game axes, faded and anchored.
struct HeatSourceFrame
{
	DVector3 start;
	DVector3 end;
	double radiusStart;
	double radiusEnd;
	double strength;
	double noiseScale;
	double rise;
};

// r_heatrefraction_test's column, fixed where it was put when the switch went on.
// Placed again after a map change or an earlier savegame (maptime went backwards).
static bool HeatTestPlaced = false;
static int HeatTestLastMaptime = 0;
static DVector3 HeatTestBase;

static void SetupHeatSources(const HWDrawInfo *di, bool toscreen)
{
	PPHeatRefraction &pass = hw_postprocess.heatrefraction;
	pass.ClearSources();

	FLevelLocals *Level = di->Level;
	if (!r_heatrefraction_test)
		HeatTestPlaced = false;
	if (!toscreen || !r_heatrefraction || Level == nullptr || !screen->IsVulkan())
		return;

	const uint64_t startNs = I_nsTime();

	// This frame's clock in tics, as StartScene builds mLevelTime: it pauses with the
	// game, so a fade and the rising noise stop in a menu.
	const double nowTics = Level->maptime + di->Viewpoint.TicFrac;
	const double scale = clamp<double>(r_heatrefraction_scale, 0.0, 4.0);

	HeatSourceFrame frames[PPHeatRefraction::MAX_SOURCES];
	int count = 0;

	for (int i = 0; i < FLevelLocals::MAX_HEAT_SOURCES; i++)
	{
		const FLevelLocals::HeatSource &h = Level->HeatSources[i];
		if (!h.Live || h.Strength <= 0.0 || std::max(h.RadiusStart, h.RadiusEnd) < 0.5)
			continue;

		double fade = 1.0;
		if (h.Life > 0)
		{
			fade = 1.0 - (nowTics - h.Birth) / (double)h.Life;
			if (fade <= 0.0)
				continue;
			fade = std::min(fade, 1.0);
		}

		HeatSourceFrame &f = frames[count++];
		f.start = h.Start;
		f.end = h.End;
		// Only with a hand recorded when the source was set, and only while the owning
		// player is still in the game: ResolveLineAnchor falls back to the console player,
		// whose hand was never the base.
		if (h.Anchor != 0 && h.AnchorBaseValid && (h.AnchorPlayer < 0 || Level->PlayerInGame(h.AnchorPlayer)))
		{
			// The whole source follows the hand: by how far the hand has moved since
			// the source was set (FLevelLocals::HeatSource). No player to read: it stays.
			DVector3 handNow = h.AnchorBase;
			ResolveLineAnchor(Level, h.Anchor, handNow, h.AnchorPlayer);
			const DVector3 moved = handNow - h.AnchorBase;
			f.start += moved;
			f.end += moved;
		}
		f.radiusStart = h.RadiusStart;
		f.radiusEnd = h.RadiusEnd;
		f.strength = h.Strength * fade * scale;
		f.noiseScale = h.NoiseScale;
		f.rise = h.Rise;
	}

	if (r_heatrefraction_test)
	{
		if (!HeatTestPlaced || Level->maptime < HeatTestLastMaptime)
		{
			const DAngle yaw = di->Viewpoint.Angles.Yaw;
			HeatTestBase = di->Viewpoint.Pos + DVector3(yaw.Cos(), yaw.Sin(), 0.0) * 96.0;
			HeatTestPlaced = true;
			Printf("heat_refraction: test source placed at (%.0f, %.0f, %.0f)\n", HeatTestBase.X, HeatTestBase.Y, HeatTestBase.Z);
		}
		HeatTestLastMaptime = Level->maptime;

		HeatSourceFrame &f = frames[count++];
		f.start = HeatTestBase - DVector3(0.0, 0.0, 32.0);
		f.end = HeatTestBase + DVector3(0.0, 0.0, 64.0);
		f.radiusStart = 12.0;
		f.radiusEnd = 28.0;
		f.strength = 2.0 * scale;
		f.noiseScale = 0.08;
		f.rise = 20.0;
	}

	if (count == 0)
		return;

	const float levelSeconds = (float)(nowTics / (double)TICRATE);
	const float linearizeA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	const float linearizeB = max(1.0f / screen->GetZNear(), 1.e-8f);

	// Each eye's matrices, ray terms and cull planes.
	struct HeatEyeView
	{
		VSMatrix view;
		VSMatrix viewToWorld;
		double eyeX, eyeY, eyeZ;
		float tanX, tanY, offX, offY;
		double slopeX, slopeY, normX, normY, axisScale;
	};

	const int eyeSets = di->HasMultiviewViewpoints ? 2 : 1;
	HeatEyeView eyes[2];
	for (int eye = 0; eye < eyeSets; eye++)
	{
		const HWViewpointUniforms &vpu = di->HasMultiviewViewpoints ? di->MultiviewVPUniforms[eye] : di->VPUniforms;
		HeatEyeView &ev = eyes[eye];

		// Copies, so nothing here depends on which VSMatrix members are const.
		ev.view = vpu.mViewMatrix;
		VSMatrix projection = vpu.mProjectionMatrix;
		if (!ev.view.inverseMatrix(ev.viewToWorld))
			ev.viewToWorld.loadIdentity();
		const float *vm = ev.view.get();
		const float *inv = ev.viewToWorld.get();
		const float *proj = projection.get();

		// This eye in GL world axes: ViewToWorld's translation.
		ev.eyeX = inv[12];
		ev.eyeY = inv[13];
		ev.eyeZ = inv[14];

		ev.tanX = (proj[0] != 0.0f) ? 1.0f / proj[0] : 1.0f;
		ev.tanY = (proj[5] != 0.0f) ? 1.0f / proj[5] : 1.0f;
		ev.offX = proj[8];
		ev.offY = proj[9];

		// A conservative cull against this eye's frustum: the source's bounding ball, in
		// view space, against the four side planes through the eye and the eye plane.
		// The slopes are the shader's ray, (ndc + offset) x tan, at ndc -1 and 1; each
		// axis takes the larger magnitude on both sides, so no sign convention can cull
		// a visible source. The ball's radius grows by the view matrix's largest axis
		// scale (the pixel stretch).
		ev.slopeX = std::max(fabs((-1.0 + ev.offX) * ev.tanX), fabs((1.0 + ev.offX) * ev.tanX));
		ev.slopeY = std::max(fabs((-1.0 + ev.offY) * ev.tanY), fabs((1.0 + ev.offY) * ev.tanY));
		ev.normX = sqrt(1.0 + ev.slopeX * ev.slopeX);
		ev.normY = sqrt(1.0 + ev.slopeY * ev.slopeY);
		ev.axisScale = std::max({
			sqrt((double)vm[0] * vm[0] + (double)vm[1] * vm[1] + (double)vm[2] * vm[2]),
			sqrt((double)vm[4] * vm[4] + (double)vm[5] * vm[5] + (double)vm[6] * vm[6]),
			sqrt((double)vm[8] * vm[8] + (double)vm[9] * vm[9] + (double)vm[10] * vm[10]) });
	}

	for (int i = 0; i < count; i++)
	{
		const HeatSourceFrame &f = frames[i];
		// Game (x, y, z) -> GL world (x, z, y), as every beam and stamp upload.
		const DVector3 startGL(f.start.X, f.start.Z, f.start.Y);
		const DVector3 endGL(f.end.X, f.end.Z, f.end.Y);
		const DVector3 centre = (startGL + endGL) * 0.5;
		const double ballRadius = (endGL - startGL).Length() * 0.5 + std::max(f.radiusStart, f.radiusEnd);

		// VISIBLE TO ANY EYE GOES TO EVERY EYE. Under layered multiview post-processing
		// both eyes share one pair of pipeline images, and each eye's final image is taken
		// to be where the last eye ended (VulkanRenderDevice::PostProcessScene), so both
		// eyes must run exactly the same passes. A source one eye culled but the other did
		// not would give one eye a bend pass the other lacks. A source outside an eye's
		// view costs that eye only its early per-pixel exit.
		bool visible = false;
		for (int eye = 0; eye < eyeSets && !visible; eye++)
		{
			const HeatEyeView &ev = eyes[eye];
			const float *vm = ev.view.get();
			const double reach = ballRadius * ev.axisScale;
			const double cx = vm[0] * centre.X + vm[4] * centre.Y + vm[8] * centre.Z + vm[12];
			const double cy = vm[1] * centre.X + vm[5] * centre.Y + vm[9] * centre.Z + vm[13];
			const double cz = vm[2] * centre.X + vm[6] * centre.Y + vm[10] * centre.Z + vm[14];
			if (cz - reach >= 0.0) continue;                                         // wholly behind the eye
			if ((cx + ev.slopeX * cz) / ev.normX > reach) continue;                  // right of the view
			if ((-cx + ev.slopeX * cz) / ev.normX > reach) continue;                 // left of it
			if ((cy + ev.slopeY * cz) / ev.normY > reach) continue;                  // above
			if ((-cy + ev.slopeY * cz) / ev.normY > reach) continue;                 // below
			visible = true;
		}
		if (!visible)
			continue;

		for (int eye = 0; eye < eyeSets; eye++)
		{
			const HeatEyeView &ev = eyes[eye];
			HeatOffsetUniforms u = {};
			u.SourceStart = FVector3((float)(startGL.X - ev.eyeX), (float)(startGL.Y - ev.eyeY), (float)(startGL.Z - ev.eyeZ));
			u.RadiusStart = (float)f.radiusStart;
			u.SourceEnd = FVector3((float)(endGL.X - ev.eyeX), (float)(endGL.Y - ev.eyeY), (float)(endGL.Z - ev.eyeZ));
			u.RadiusEnd = (float)f.radiusEnd;
			u.TanHalfFov = FVector2(ev.tanX, ev.tanY);
			u.ProjOffset = FVector2(ev.offX, ev.offY);
			u.Bend = (float)(f.strength * PPHeatRefraction::BEND_PER_UNIT);
			u.NoiseScale = (float)f.noiseScale;
			u.NoiseRise = (float)f.rise;
			u.NoiseTime = levelSeconds;
			u.LinearizeDepthA = linearizeA;
			u.LinearizeDepthB = linearizeB;
			memcpy(u.ViewToWorld, ev.viewToWorld.get(), sizeof(float) * 16);
			pass.AddSource(eye, u);
		}
	}
	pass.SetEyeSets(eyeSets);

	PerfLog::AddCpuSample("fx.heatsources", (double)(I_nsTime() - startNs) / 1e6);
}

//==========================================================================
//
// [SMOKEVOLUME] This scene's smoke volume march, per eye, for the smoke pass (PPSmokeVolume,
// hw_postprocess.h; "Engine docs/SMOKE_VOLUME_PLAN.md" 13c).
//
// MAIN VIEW ONLY and before DrawScene, like SetupHeatSources: a camera texture has no post pass, a
// save picture's post pass must not draw the main view's march, and portals run RenderScene again
// with their own views.
//
// PUBLISHED ONLY WHEN THERE IS SMOKE TO DRAW. All of these must hold:
//   - Vulkan and r_smoke;
//   - the CPU side's bound says visible smoke may exist in its box this frame
//     (SmokeVolume::GetDrawState, decided by PrepareFrameCompute before the eye loop);
//   - the backend holds the volume at that quality (SmokeVolumeStatus, written by this frame's
//     RunFrameCompute, which also put the volume's images in the layout the pass reads them in);
//   - something to show: the look's absorption x r_smoke_density_scale above 0, or the debug slice;
//   - [13d] the backend holds the light grid at this frame's light quality (SmokeVolumeStatus::LightQuality).
// Otherwise the pass returns on its first line and the frame is the frame without it.
//
// PER EYE (review S8): a multiview scene publishes a march for each eye, from
// MultiviewVPUniforms[0] and [1]; any other scene publishes one from VPUniforms, which the next
// eye's own ProcessScene replaces before that eye's post. There is no cull: the box is placed around
// the viewer (hw_smokevolume.cpp), so every eye looks out from inside it, and both sets are always
// published together.
//
// THE LIGHT [13d]: each place's own, from the light grid this frame's compute step filled -- its column's
// sector light x the look's ambient, and up to SMOKE_LIGHTS_MAX dynamic lights x the look's scatter
// (hw_smokevolume.cpp, shaders/compute/smoke_light.comp). The march's LightColor is the look's TINT: the
// colour the light the smoke scatters takes. With no dynamic light and one sector this is exactly 13c's
// tint x ambient x sector light.
//
//==========================================================================

// How far below the eye r_smoke_debugslice's plane lies, map units.
static constexpr double SMOKE_DEBUG_SLICE_BELOW_EYE = 16.0;

static void SetupSmokeVolume(const HWDrawInfo *di, bool toscreen)
{
	PPSmokeVolume &pass = hw_postprocess.smokevolume;
	pass.ClearEyes();

	FLevelLocals *Level = di->Level;
	if (!toscreen || !r_smoke || Level == nullptr || !screen->IsVulkan())
		return;

	const SmokeVolume::DrawState &draw = SmokeVolume::Get().GetDrawState();
	const SmokeVolumeBackendStatus &status = SmokeVolumeStatus();
	if (!draw.HasSmoke || !status.Allocated || status.Quality != draw.Quality || draw.Grid.Cells() == 0 || draw.Grid.CellSize <= 0)
		return;
	// [13d] The march reads the light grid: only while the backend holds it at the frame's light quality.
	if (draw.LightQuality <= 0 || status.LightQuality != draw.LightQuality)
		return;

	// The look (SetSmokeLook) and the player's "Smoke density".
	const FLevelLocals::SmokeLookSettings &look = Level->SmokeLook;
	const bool debugSlice = r_smoke_debugslice;
	double densityScale = r_smoke_density_scale;
	if (!(densityScale >= 0.0))
		densityScale = 0.0;		// written this way so a NaN lands on 0 too
	densityScale = std::min(densityScale, 16.0);
	const double extinction = look.Absorption * SMOKE_EXTINCTION_PER_MAP_UNIT * densityScale;
	if (!(extinction > 0.0) && !debugSlice)
		return;		// nothing would hide anything or glow: skipped, not drawn at zero

	const uint64_t startNs = I_nsTime();

	// [13d] The look's tint: the colour the light the smoke scatters takes. The light itself is the grid's.
	const FVector3 lightColor((float)(look.Tint.r / 255.0), (float)(look.Tint.g / 255.0), (float)(look.Tint.b / 255.0));

	const double cell = draw.Grid.CellSize;
	// The grid's minimum corner in GL world axes (map x, map z, map y), as every beam upload.
	const double cornerGL[3] = { draw.OriginCell[0] * cell, draw.OriginCell[2] * cell, draw.OriginCell[1] * cell };
	const int steps = clamp((int)r_smoke_steps, 16, 128);

	const int eyeSets = di->HasMultiviewViewpoints ? 2 : 1;
	for (int eye = 0; eye < eyeSets; eye++)
	{
		const HWViewpointUniforms &vpu = di->HasMultiviewViewpoints ? di->MultiviewVPUniforms[eye] : di->VPUniforms;

		// Copies, so nothing here depends on which VSMatrix members are const (as SetupHeatSources).
		VSMatrix view = vpu.mViewMatrix;
		VSMatrix projection = vpu.mProjectionMatrix;
		VSMatrix viewToWorld;
		if (!view.inverseMatrix(viewToWorld))
			viewToWorld.loadIdentity();
		const float *inv = viewToWorld.get();
		const float *proj = projection.get();

		SmokeMarchUniforms u = {};
		memcpy(u.ViewToWorld, inv, sizeof(float) * 16);
		u.TanHalfFov = FVector2(proj[0] != 0.0f ? 1.0f / proj[0] : 1.0f, proj[5] != 0.0f ? 1.0f / proj[5] : 1.0f);
		u.ProjOffset = FVector2(proj[8], proj[9]);
		// Relative to this eye: ViewToWorld's translation is the eye in GL world axes.
		u.BoxMin = FVector3((float)(cornerGL[0] - inv[12]), (float)(cornerGL[1] - inv[13]), (float)(cornerGL[2] - inv[14]));
		u.CellSize = (float)cell;
		u.GridSize = FVector3((float)draw.Grid.SizeX, (float)draw.Grid.SizeY, (float)draw.Grid.SizeZ);
		u.TicFrac = clamp<float>(draw.TicFrac, 0.0f, 1.0f);
		u.TileCount = FVector3((float)(draw.Grid.SizeX / SMOKE_TILE_CELLS), (float)(draw.Grid.SizeY / SMOKE_TILE_CELLS), (float)(draw.Grid.SizeZ / SMOKE_TILE_CELLS));
		u.StepCount = steps;
		u.LightColor = lightColor;
		u.Extinction = (float)std::max(extinction, 0.0);
		u.MinStep = (float)(cell * 0.5);
		u.SliceHeight = (float)-SMOKE_DEBUG_SLICE_BELOW_EYE;
		u.DebugSlice = debugSlice ? 1 : 0;
		pass.SetEyeMarch(eye, u);
	}
	pass.SetEyeSets(eyeSets);

	// [13e] Beams and cones in the smoke: how many beams the backend's beam list holds for this frame (it copied this
	// frame's list in RunFrameCompute, before the eye loop), the three renderer-read switches, and what main.fp's beam
	// glow reads besides the list -- the look's scatter, the scene's scroll, and the scroll's clock as the drawn-line path
	// takes it (SyncDrawnLines). The same for both eyes.
	{
		PPSmokeBeamSettings beams;
		beams.BeamCount = std::clamp(status.BeamCount, 0, SMOKE_BEAMS_MAX);
		beams.Scatter = r_smoke_beams;
		beams.Depth = r_smoke_beams_depth;
		beams.Cones = r_smoke_cones_depth;
		double lookScatter = look.Scatter;
		if (!(lookScatter >= 0.0))
			lookScatter = 0.0;		// written this way so a NaN lands on 0 too
		beams.LookScatter = (float)std::min(lookScatter, 1.0);
		beams.ScrollSpeed = (float)Level->BeamScrollSpeed;
		beams.ScrollDepth = (float)Level->BeamScrollDepth;
		beams.Timer = static_cast<float>((double)(screen->FrameTime - screen->RenderState()->firstFrame) / 1000.);
		pass.SetBeams(beams);
	}

	PerfLog::AddCpuSample("fx.smokedraw", (double)(I_nsTime() - startNs) / 1e6);
}

//==========================================================================
//
// [EMISSIVEVOLUMES] This scene's emissive volume march, per eye, for PPEmissiveVolumes (hw_postprocess.h; "Engine docs/
// EMISSIVE_VOLUMES_15_IMPL_NOTES.md").
//
// MAIN VIEW ONLY and before DrawScene, like SetupSmokeVolume: a camera texture has no post pass, a save picture's post pass must
// not draw the main view's volumes, and portals run RenderScene again with their own views.
//
// PUBLISHED ONLY WHEN THERE ARE VOLUMES TO DRAW: Vulkan; this frame's list has volumes (EmissiveVolumes::GetDrawState, decided
// by PrepareFrameCompute before the eye loop); and the backend holds exactly that list (EmissiveVolumesStatus, written by this
// frame's RunFrameCompute, which put the list and the noise where a pass reads them). Otherwise the pass returns on its first
// line, and so does the smoke's request for the curve.
//
// PER EYE, as SetupSmokeVolume: a multiview scene publishes a march for each eye, any other one from VPUniforms.
//
// THE RECTANGLE. Each eye's union of its volumes' bounding spheres on screen, so a texel no volume can cover returns clear on its
// first line: a sphere's box in view space (its radius grown by the view matrix's largest axis scale, which carries the pixel
// stretch), projected with the ray's own terms (TanHalfFov, ProjOffset) from its nearest and farthest depth. A sphere reaching
// the eye's plane covers the whole screen.
//
//==========================================================================

static void SetupEmissiveVolumes(const HWDrawInfo *di, bool toscreen)
{
	PPEmissiveVolumes &pass = hw_postprocess.emissivevolumes;
	pass.ClearEyes();

	FLevelLocals *Level = di->Level;
	if (!toscreen || Level == nullptr || !screen->IsVulkan())
		return;

	const EmissiveVolumes::DrawState &draw = EmissiveVolumes::Get().GetDrawState();
	const EmissiveVolumeBackendStatus &status = EmissiveVolumesStatus();
	if (draw.Count <= 0 || !status.ListReady || status.Count != draw.Count || status.Serial != draw.Serial)
		return;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	// The list's origin in GL world axes (map x, map z, map y).
	const double originGL[3] = { draw.Origin[0], draw.Origin[2], draw.Origin[1] };
	const int steps = clamp((int)r_emissivevolumes_steps, 8, 64);

	const int eyeSets = di->HasMultiviewViewpoints ? 2 : 1;
	for (int eye = 0; eye < eyeSets; eye++)
	{
		const HWViewpointUniforms &vpu = di->HasMultiviewViewpoints ? di->MultiviewVPUniforms[eye] : di->VPUniforms;

		// Copies, so nothing here depends on which VSMatrix members are const (as SetupHeatSources).
		VSMatrix view = vpu.mViewMatrix;
		VSMatrix projection = vpu.mProjectionMatrix;
		VSMatrix viewToWorld;
		if (!view.inverseMatrix(viewToWorld))
			viewToWorld.loadIdentity();
		const float *vm = view.get();
		const float *inv = viewToWorld.get();
		const float *proj = projection.get();

		EmissiveVolumeUniforms u = {};
		memcpy(u.ViewToWorld, inv, sizeof(float) * 16);
		const double tanX = proj[0] != 0.0f ? 1.0 / proj[0] : 1.0;
		const double tanY = proj[5] != 0.0f ? 1.0 / proj[5] : 1.0;
		u.TanHalfFov = FVector2((float)tanX, (float)tanY);
		u.ProjOffset = FVector2(proj[8], proj[9]);
		// Relative to this eye: ViewToWorld's translation is the eye in GL world axes.
		u.ListOrigin = FVector3((float)(originGL[0] - inv[12]), (float)(originGL[1] - inv[13]), (float)(originGL[2] - inv[14]));
		u.VolumeCount = draw.Count;
		u.StepCount = steps;

		// The view matrix's largest axis scale: a world sphere of radius r fits in a view-space sphere of radius r x this.
		double axisScale = 0.0;
		for (int c = 0; c < 3; c++)
			axisScale = std::max(axisScale, std::sqrt((double)vm[c * 4] * vm[c * 4] + (double)vm[c * 4 + 1] * vm[c * 4 + 1] + (double)vm[c * 4 + 2] * vm[c * 4 + 2]));

		double lo[2] = { 1.0, 1.0 }, hi[2] = { 0.0, 0.0 };
		bool whole = false;
		for (int i = 0; i < draw.Count && !whole; i++)
		{
			const double wx = originGL[0] + draw.Centre[i][0], wy = originGL[1] + draw.Centre[i][1], wz = originGL[2] + draw.Centre[i][2];
			const double vx = vm[0] * wx + vm[4] * wy + vm[8] * wz + vm[12];
			const double vy = vm[1] * wx + vm[5] * wy + vm[9] * wz + vm[13];
			const double vz = vm[2] * wx + vm[6] * wy + vm[10] * wz + vm[14];
			const double r = draw.Radius[i] * axisScale;
			const double nearDepth = -vz - r, farDepth = -vz + r;
			if (!(nearDepth > 1e-3))
			{
				whole = true;
				break;
			}
			const double view[2] = { vx, vy };
			const double tan[2] = { tanX, tanY };
			const double offset[2] = { proj[8], proj[9] };
			for (int k = 0; k < 2; k++)
			{
				// The four corners' slopes (view / depth) over the nearest and farthest depth, then the ray's own mapping from a
				// slope to TexCoord: ndc = slope / TanHalfFov - ProjOffset, TexCoord = (ndc + 1) / 2.
				const double slopes[4] = { (view[k] - r) / nearDepth, (view[k] - r) / farDepth, (view[k] + r) / nearDepth, (view[k] + r) / farDepth };
				for (double s : slopes)
				{
					const double coord = (s / tan[k] - offset[k] + 1.0) * 0.5;
					lo[k] = std::min(lo[k], coord);
					hi[k] = std::max(hi[k], coord);
				}
			}
		}
		if (whole)
		{
			u.RectMin = FVector2(0.0f, 0.0f);
			u.RectMax = FVector2(1.0f, 1.0f);
		}
		else
		{
			u.RectMin = FVector2((float)clamp(lo[0] - 1e-4, 0.0, 1.0), (float)clamp(lo[1] - 1e-4, 0.0, 1.0));
			u.RectMax = FVector2((float)clamp(hi[0] + 1e-4, 0.0, 1.0), (float)clamp(hi[1] + 1e-4, 0.0, 1.0));
		}
		pass.SetEyeMarch(eye, u);
	}
	pass.SetEyeSets(eyeSets);

	PPEmissiveVolumeSettings settings;
	settings.Count = draw.Count;
	settings.Absorbs = draw.Absorbs;
	settings.Resolution = clamp((int)r_emissivevolumes_resolution, 1, 2);
	pass.SetSettings(settings);

	if (timed)
		PerfLog::AddCpuSample("fx.emissive", (double)(I_nsTime() - startNs) / 1e6);
}

//==========================================================================
//
// [BLOOMOVERRIDE] The level's bloom override (SetBloomOverride, g_levellocals.h),
// handed to the bloom pass (PPBloom::SetOverride, hw_postprocess.h). E1 in
// "Engine docs/REVIEW_BLOOM_PLAN.md".
//
// hw_postprocess is common code and cannot see FLevelLocals, so the level-side
// copy lives here, like SetupHeatSources. MAIN VIEW ONLY: called from ProcessScene
// when toscreen. A camera texture has no post pass, and a save picture's post pass
// should show the frame as it is -- neither may restart the renderer's fade.
//
// Called once per eye that draws a scene. Script does not run inside the eye loop,
// so the second eye hands over the same values, and SetOverride ignores a repeat:
// nothing advances per eye. The copy is plain double-to-float; the fade and the
// pulse are worked out in hw_postprocess.cpp from the frame clock, which is the
// same for both eyes.
//
//==========================================================================

static void SyncBloomOverride(const FLevelLocals *Level)
{
	PPBloom &bloom = hw_postprocess.bloom;
	if (Level == nullptr || !Level->BloomOverrideActive)
	{
		bloom.ClearOverride(screen->FrameTime);   // a no-op unless one was set
		return;
	}

	PPBloomOverride target;
	target.Spread = (float)Level->BloomOverrideSpread;
	target.Threshold = (float)Level->BloomOverrideThreshold;
	target.Knee = (float)Level->BloomOverrideKnee;
	target.TintR = (float)Level->BloomOverrideTintR;
	target.TintG = (float)Level->BloomOverrideTintG;
	target.TintB = (float)Level->BloomOverrideTintB;
	target.Mix = (float)Level->BloomOverrideMix;
	target.Intensity = (float)Level->BloomOverrideIntensity;
	target.Fade = (float)Level->BloomOverrideFade;
	target.Pulse = (float)Level->BloomOverridePulse;
	target.PulseRate = (float)Level->BloomOverridePulseRate;
	// The glow alarm pulse's rate inputs, as StartScene uploads them in mGlowTex4.z/.w:
	// pulseRate 0 beats at the glows' rate, from the same float values.
	target.GlowPulseLevel = (float)Level->GlowPulseLevel;
	target.GlowPulseRate = (float)Level->GlowPulseRate;
	bloom.SetOverride(target, screen->FrameTime);
}

// r_beams_debug: one line every two seconds. Runs on every backend -- the
// claim and style counts mean something on GL too.
static void ReportBeamLines(FLevelLocals *Level)
{
	if (!r_beams_debug || Level == nullptr) return;

	static uint64_t lastMs = 0;
	const uint64_t now = I_msTime();
	if (lastMs != 0 && now - lastMs < 2000) return;
	lastMs = now;

	int claimed = 0, styled = 0, lit = 0;
	for (int i = 0; i < FLevelLocals::MAX_BEAMS; i++)
	{
		if (Level->BeamClaimed[i]) claimed++;
		if (Level->BeamHasStyle[i]) styled++;
		if (Level->BeamSlotLive(i) && Level->BeamIntensity[i] != 0.0) lit++;
	}
	int ownLit = 0;
	for (int i = 0; i < Level->DrawnLineHigh; i++)
		if (Level->DrawnLines[i].Live && Level->DrawnLines[i].Intensity != 0.0) ownLit++;

	DrawnLineBuffer *lines = screen->mDrawnLines;
	Printf("Beams [debug]: BeamCount %d, claimed %d, styled %d, lit %d, per-pixel upload %d | "
		"r_beams_drawn %d, drawn path %s, routed %u, SetDrawnLine lit %d, drawn records %u, draws %u\n",
		Level->BeamCount, claimed, styled, lit, BeamLinesUploaded,
		(int)*r_beams_drawn, lines == nullptr ? "absent (not Vulkan)" : lines->StateName(),
		lines ? lines->GetRoutedCount() : 0u, ownLit,
		lines ? lines->GetLiveCount() : 0u, lines ? lines->TakeDrawCount() : 0u);
}

sector_t * hw_FakeFlat(sector_t * sec, sector_t * dest, area_t in_area, bool back);

std::pair<PalEntry, PalEntry>& R_GetSkyCapColor(FGameTexture* tex);

extern int portalsPerEye;

//==========================================================================
//
// RS FORK -- where a volumetric beam actually is THIS FRAME.
//
// Anchor 0 is the world pos/dir script published. Anchors 1-3 read a tracked
// pose instead (FLevelLocals::VolBeamAnchor): the main hand, the off hand or
// the head, as the VR backend wrote it for this frame -- the same fields
// SetBeamAnchor reads for line beams, for the same reason (a 35Hz pose steps
// against a 90Hz hand). Used by both consumers of the cone, the air pass
// (SetupVolumetricBeam) and the fog glow (mFogBeam* in StartScene), so the two
// can never disagree about where the torch is.
//
// [round2 B1] The pose itself is ResolveTrackedPose (r_utility.cpp), shared
// with anchored dynamic lights so the cone and the light it throws cannot
// disagree about where a hand is. The angle conventions are documented there.
// This keeps only what is the beam's own: the script's world pos/dir as the
// fallback, and the cone direction along the pose.
//
// Returns an EVolBeamPoseSource so the caller can log where the pose came from.
//
//==========================================================================

enum EVolBeamPoseSource
{
	VBPOSE_WORLD    = TPOSE_NONE,       // not anchored: script's pos/dir
	VBPOSE_MAINHAND = TPOSE_MAINHAND,
	VBPOSE_OFFHAND  = TPOSE_OFFHAND,
	VBPOSE_HMD      = TPOSE_HMD,
	VBPOSE_VIEW     = TPOSE_VIEW,       // head anchor, no headset pose written: r_viewpoint
	VBPOSE_NOPLAYER = TPOSE_NOPLAYER,   // anchored, but no console player: script's pos/dir
	VBPOSE_COUNT    = TPOSE_COUNT
};

static int ResolveVolBeamPose(const FLevelLocals *Level, int slot, DVector3 &pos, DVector3 &dir)
{
	pos = Level->VolBeamPos[slot];
	dir = Level->VolBeamDir[slot];

	DVector3 posed;
	DAngle yaw, pitch;
	const int source = ResolveTrackedPose(Level, Level->VolBeamAnchor[slot],
		Level->VolBeamAnchorOffset[slot], posed, yaw, pitch);
	if (source == TPOSE_NONE || source == TPOSE_NOPLAYER) return source;

	pos = posed;
	dir = TrackedPoseForward(yaw, pitch);
	return source;
}

// Log a slot's pose source when it changes -- rare (an anchor set or dropped),
// so always on. Says in one line whether an anchored torch is really reading
// the per-frame pose or silently falling back to the script's world point.
static void LogVolBeamPoseSource(int slot, int source)
{
	static int lastSource[FLevelLocals::MAX_VOL_BEAMS] = {};   // 0 = VBPOSE_WORLD
	if (slot < 0 || slot >= FLevelLocals::MAX_VOL_BEAMS || lastSource[slot] == source) return;
	lastSource[slot] = source;

	static const char *const names[VBPOSE_COUNT] =
	{
		"script world point (not anchored)",
		"main hand, per frame (AttackPos)",
		"off hand, per frame (OffhandPos)",
		"headset, per frame (HmdPos)",
		"view centre eye (no headset pose written)",
		"anchored but no console player -- using the script world point",
	};
	Printf("vol_beam: slot %d pose source: %s\n", slot,
		(source >= 0 && source < VBPOSE_COUNT) ? names[source] : "?");
}

//==========================================================================
//
//
//
//==========================================================================

class FDrawInfoList
{
public:
	TDeletingArray<HWDrawInfo *> mList;

	HWDrawInfo * GetNew();
	void Release(HWDrawInfo *);
};


FDrawInfoList di_list;

//==========================================================================
//
// Try to reuse the lists as often as possible as they contain resources that
// are expensive to create and delete.
//
// Note: If multithreading gets used, this class needs synchronization.
//
//==========================================================================

HWDrawInfo *FDrawInfoList::GetNew()
{
	if (mList.Size() > 0)
	{
		HWDrawInfo *di;
		mList.Pop(di);
		return di;
	}
	return new HWDrawInfo();
}

void FDrawInfoList::Release(HWDrawInfo * di)
{
	di->ClearBuffers();
	di->Level = nullptr;
	mList.Push(di);
}

//==========================================================================
//
// Sets up a new drawinfo struct
//
//==========================================================================

HWDrawInfo *HWDrawInfo::StartDrawInfo(FLevelLocals *lev, HWDrawInfo *parent, FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms)
{
	HWDrawInfo *di = di_list.GetNew();
	di->Level = lev;
	di->StartScene(parentvp, uniforms);
	return di;
}


//==========================================================================
//
//
//
//==========================================================================

static Clipper staticClipper;		// Since all scenes are processed sequentially we only need one clipper.
static Clipper staticVClipper;		// Another clipper to clip vertically (used if (VPSF_ALLOWOUTOFBOUNDS & camera->viewpos->Flags)).
static Clipper staticRClipper;		// Another clipper for radar (doesn't actually clip. Changes SSECMF_DRAWN setting).
static HWDrawInfo * gl_drawinfo;	// This is a linked list of all active DrawInfos and needed to free the memory arena after the last one goes out of scope.

void HWDrawInfo::StartScene(FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms)
{
	staticClipper.Clear();
	staticVClipper.Clear();
	staticRClipper.Clear();
	mClipper = &staticClipper;
	vClipper = &staticVClipper;
	rClipper = &staticRClipper;
	rClipper->amRadar = true;

	Viewpoint = parentvp;
	auto vrmode = VRMode::GetVRModeCached(true);
	IsVRScene = vrmode != nullptr && vrmode->IsVR();
	if (Level != nullptr)
		lightmode = getRealLightmode(Level, true);
	if (uniforms)
	{
		VPUniforms = *uniforms;
		// The clip planes will never be inherited from the parent drawinfo.
		VPUniforms.mClipLine.X = -1000001.f;
		VPUniforms.mClipHeight = 0;
		VPUniforms.mClipHeightDirection = 0.f;
	}
	else
	{
		VPUniforms.mProjectionMatrix.loadIdentity();
		VPUniforms.mViewMatrix.loadIdentity();
		VPUniforms.mNormalViewMatrix.loadIdentity();
		ProjectionMatrix2.loadIdentity();
		VPUniforms.mViewHeight = viewheight;
		int fogmode = Viewpoint.bDoOrtho && (lightmode == ELightMode::ZDoomSoftware) ? 2 : gl_fogmode; // Force radial if Ortho and ZDoomSoftware
		if (lightmode == ELightMode::Build)
		{
			VPUniforms.mGlobVis = 1 / 64.f;
			VPUniforms.mPalLightLevels = 32 | (static_cast<int>(fogmode) << 8) | ((int)lightmode << 16);
		}
		else
		{
			VPUniforms.mGlobVis = (float)R_GetGlobVis(r_viewwindow, r_visibility) / 32.f;
			VPUniforms.mPalLightLevels = static_cast<int>(gl_bandedswlight) | (static_cast<int>(fogmode) << 8) | ((int)lightmode << 16);
		}
		VPUniforms.mClipLine.X = -10000000.0f;
		VPUniforms.mShadowmapFilter = gl_shadowmap_filter;
		VPUniforms.mLightBlendMode = (level.info ? (int)level.info->lightblendmode : 0);
		VPUniforms.mThickFogDistance = Level->thickfogdistance;
		VPUniforms.mThickFogMultiplier = Level->thickfogmultiplier;
	}

	// [BB] Glow wave, scene-global. Copied here rather than per draw because
	// every draw in the frame reads the same wave -- only the phase differs,
	// and that is per CHANNEL, not per surface.
	//
	// Doom's Z is the shader's Y, the same swizzle the sweep origin uses just
	// below in RenderScene. Getting it wrong is not a crash, it is a ring
	// wave that expands through the floor instead of across it.
	if (Level != nullptr)
	{
		VPUniforms.mGlowWave = {
			(float)Level->GlowWaveLength, (float)Level->GlowWaveSpeed,
			(float)Level->GlowWaveSharp,  (float)Level->GlowWaveShape };
		VPUniforms.mGlowWaveDepth = {
			(float)Level->GlowWaveReach,  (float)Level->GlowWaveBright,
			(float)Level->GlowWaveColour, (float)Level->GlowWaveDetune };
		VPUniforms.mGlowWavePhase = {
			(float)Level->GlowWavePhase[0], (float)Level->GlowWavePhase[1],
			(float)Level->GlowWavePhase[2], (float)Level->GlowWavePhase[3] };
		VPUniforms.mGlowWaveOrigin = {
			(float)Level->GlowWaveOrigin.X, (float)Level->GlowWaveOrigin.Z,
			(float)Level->GlowWaveOrigin.Y, (float)Level->GlowWaveSeed };

		// [BB] Beams. Swizzled like everything else here: Doom's Z is the
		// shader's Y. A beam laid along a corridor with the axes crossed
		// becomes a beam standing in a wall, which is a memorable bug.
		//
		// [BEAMLINES] COMPACTED, WITH A LOOK PER LINE. Every slot is walked, and
		// the live ones -- below BeamCount, or claimed -- with a non-zero
		// intensity are written into consecutive uniform slots, so the shader's
		// loops pay per live line whatever the slot numbers are. Leaving out a
		// zero-intensity slot changes no pixel: both loops multiplied its colour
		// by exactly 0. The survivors keep their slot order, so the shader adds
		// the same terms in the same order it always did.
		//
		// Each uploaded line carries its look in mBeamLook: its own style if
		// SetBeamStyle gave it one, otherwise the scene values -- the very
		// numbers main.fp used to read from mBeamParams.y/.w and mBeamFX.z/.w.
		// mBeamParams.w becomes the LARGEST uploaded air glow, so BeamAirGlow's
		// whole-loop early-out still costs nothing when nothing glows in the air.
		{
			// Script sets beams at 35Hz; this loop runs every frame. Without the
			// lerp a beam holds still for a whole tic and then jumps, which at
			// 90-120Hz reads as a beam that stutters against smoothly moving
			// geometry. cl_capfps and r_NoInterpolate already pin TicFrac to 1.0
			// upstream (hw_entrypoint.cpp), so those cases collapse to the old
			// behaviour on their own and need no guard here.
			const double ticFrac = r_beam_interpolate ? Viewpoint.TicFrac : 1.0;

			// [DRAWNLINES] r_beams_drawn: every beam slot's glow IN THE AIR is drawn
			// by the drawn-line path instead (SyncDrawnLines, RenderTranslucent).
			// The slot is still uploaded here for its SURFACE light, with its air
			// glow zeroed so the per-pixel air loop skips it -- unless
			// r_beams_drawn_surfacelight is off, when it is not uploaded at all.
			// False (the default), and wherever the drawn path does not exist:
			// this loop is the one above, untouched.
			const bool routeDrawn = BeamsRouteToDrawnLines();
			const bool uploadRouted = r_beams_drawn_surfacelight;

			int nb = 0;
			float maxAirGlow = 0.f;
			for (int i = 0; i < FLevelLocals::MAX_BEAMS; i++)
			{
				if (!Level->BeamSlotLive(i) || Level->BeamIntensity[i] == 0.0) continue;
				if (routeDrawn && !uploadRouted) continue;

				DVector3 a, b;
				ResolveBeamSlot(Level, i, ticFrac, a, b);

				VPUniforms.mBeamA[nb] = {
					(float)a.X, (float)a.Z,
					(float)a.Y, (float)Level->BeamThick[i] };
				VPUniforms.mBeamB[nb] = {
					(float)b.X, (float)b.Z,
					(float)b.Y, (float)Level->BeamSoft[i] };
				// Colour is NOT interpolated on purpose. A band change is a
				// deliberate step -- see the tier bands in RS_Lance -- and
				// crossfading it would turn a power-up into a smear.
				VPUniforms.mBeamCol[nb] = {
					Level->BeamColor[i].r / 255.f, Level->BeamColor[i].g / 255.f,
					Level->BeamColor[i].b / 255.f, (float)Level->BeamIntensity[i] };

				FVector4 look = BeamSlotLook(Level, i);
				if (routeDrawn) look.X = 0.f;
				VPUniforms.mBeamLook[nb] = look;
				maxAirGlow = (nb == 0) ? look.X : std::max(maxAirGlow, look.X);
				nb++;
			}
			VPUniforms.mBeamParams = { (float)nb, (float)Level->BeamGlow,
				(float)Level->BeamFogScatter, nb > 0 ? maxAirGlow : (float)Level->BeamAirGlow };
			VPUniforms.mBeamFX = { (float)Level->BeamScrollSpeed,
				(float)Level->BeamScrollDepth, (float)Level->BeamTaper,
				(float)Level->BeamFlare };
			BeamLinesUploaded = nb;
		}

		// [STAMP] Surface stamps. Progress carries the tic fraction so a stamp
		// blooms smoothly instead of stepping 35 times a second -- the same
		// reason the beams above interpolate. Nothing else here needs
		// smoothing: position, colour and shape do not move once published.
		{
			// Viewpoint.TicFrac directly: the beams' own ticFrac above is
			// scoped to their block, and a stamp has no reason to follow
			// r_beam_interpolate.
			const double stampFrac = Viewpoint.TicFrac;
			int ns = 0;
			for (int st = 0; st < FLevelLocals::MAX_SURFACE_STAMPS; st++)
			{
				if (Level->StampLife[st] <= 0 || Level->StampRadius[st] <= 0.0)
					continue;

				const double prog = clamp((Level->StampAge[st] + stampFrac)
					/ (double)Level->StampLife[st], 0.0, 1.0);
				const DVector3 &p = Level->StampPos[st];
				const DVector3 &a = Level->StampAxis[st];
				const PalEntry  c = Level->StampColor[st];

				// Game (x, y, z) -> shader (x, z, y): y is up in shader space.
				VPUniforms.mStampPos[ns] = {
					(float)p.X, (float)p.Z, (float)p.Y,
					(float)Level->StampRadius[st] };
				VPUniforms.mStampCol[ns] = {
					c.r / 255.f, c.g / 255.f, c.b / 255.f, (float)prog };
				VPUniforms.mStampArg[ns] = {
					(float)Level->StampShape[st],
					(float)a.X, (float)a.Z, (float)a.Y };
				VPUniforms.mStampMod[ns] = {
					(float)Level->StampTex[st],
					(float)Level->StampTexStrength[st], 0.f, 0.f };
				ns++;
			}
			VPUniforms.mStampParams = { (float)ns, 0.f, 0.f, 0.f };
		}

		// [GPUPARTICLES] The clock particles age by: level seconds at render
		// rate. Not `timer` -- on Vulkan that is wall-clock time scaled by the
		// bound material's shader speed, zero with no material, and it runs
		// while paused. This is the same time basis as FogDisturb's `now` and
		// as particle birth (maptime / TICRATE), so pausing freezes particles
		// the way it freezes actors. mLevelTime is general; nothing about it is
		// particle-specific, and stamps or disturbances could age by it too.
		VPUniforms.mLevelTime = {
			(float)((Level->maptime + Viewpoint.TicFrac) / (double)TICRATE), 0.f, 0.f, 0.f };
		// Live-tuning cvars, renderer-read every frame so they respond in a menu.
		VPUniforms.mGpuParticleParams = {
			(float)r_gpuparticles_sizescale, (float)r_gpuparticles_maxsize,
			(float)r_gpuparticles_stretch,   (float)r_gpuparticles_intensity };
		// [2a] Read only by scene-depth shader variants (vk_shader.cpp). The pair
		// that turns a raw depth sample into distance is the post passes' own
		// LinearizeDepthA/B (the volumetric beam's fill further down) -- right for
		// the flat projection and for BuildOpenXREyeProjection, which both use
		// screen->GetZNear/GetZFar. Then the soft-particle distance, renderer-read
		// every frame like the knobs above so the menu slider responds live.
		VPUniforms.mLinearizeDepth = {
			1.0f / screen->GetZFar() - 1.0f / screen->GetZNear(),
			max(1.0f / screen->GetZNear(), 1.e-8f), 0.f, 0.f };
		// [2d] y: which output gpuparticles.fp writes, and so which blend RenderTranslucent
		// draws the ring with -- it reads this same value back. 1, premultiplied, while a
		// particle whose definition occludes (alpha above 0) is alive at this frame's level
		// time; 0, exactly the additive draw before 2d, otherwise. What is alive comes from
		// the ring's last Sync, one scene earlier than this frame's: a first puff draws its
		// first frame additively (nothing, at emissive 0), and switching off is exact.
		// [DEBRISPOOL] A debris pool piece whose definition occludes counts too: it is drawn inside the ring's draw.
		const bool gpuParticlesPremultiplied = screen->mGpuParticles != nullptr &&
			(screen->mGpuParticles->OccludersAliveAt(VPUniforms.mLevelTime.X) || DebrisPool::Get().OccludersAliveAt(VPUniforms.mLevelTime.X));
		// [LOOKS] z: r_gpuparticles_looks, 0..3 -- the quality gpuparticles.vp/.fp draw a
		// definition's generated `look` at ("Engine docs/GPU_PARTICLE_LOOKS_PLAN.md"); 0 draws
		// every look as the plain round dot. Renderer-read every scene like the knobs above.
		// (hw_viewpointuniforms.h still calls z spare; the comment is here so that header is not
		// touched for a value.) w is still spare.
		const float gpuParticleLooks = (float)clamp((int)r_gpuparticles_looks, 0, 3);
		VPUniforms.mGpuParticleParams2 = { max((float)r_gpuparticles_soft, 0.f), gpuParticlesPremultiplied ? 1.f : 0.f, gpuParticleLooks, 0.f };

		// [BB] Sweep fill -- the pattern inside a band. Frame-global style;
		// only the mode is per band, packed into the draw mode.
		VPUniforms.mSweepFill = {
			(float)Level->SweepFillSpacingU, (float)Level->SweepFillSpacingV,
			(float)Level->SweepFillWidth,    (float)Level->SweepFillSoft };
		VPUniforms.mSweepFill2 = {
			(float)Level->SweepFillRotate,   (float)Level->SweepFillDrift,
			(float)Level->SweepFillMajor,    (float)Level->SweepFillJitter };
		VPUniforms.mSweepFill3 = {
			(float)Level->SweepFillGrad,     (float)Level->SweepFillGradAxis,
			(float)Level->SweepFillFlicker,  (float)Level->SweepFillMajorBoost };
		VPUniforms.mSweepFillCol = {
			Level->SweepFillColor.r / 255.f, Level->SweepFillColor.g / 255.f,
			Level->SweepFillColor.b / 255.f, (float)Level->SweepFillGap };
		VPUniforms.mSweepAir = { (float)Level->SweepFillAir, 0.f, 0.f, 0.f };

		// [round2 B2] The passed-region look. Enabled (w = 1) only when a live
		// band carries the passed bit AND some look term would change a pixel,
		// so the shader's band loop never runs for a look that does nothing.
		// The bands are only uploaded under this same SweepMode/SweepCount gate.
		{
			bool passedBand = false;
			if (Level->SweepMode > 0 && Level->SweepCount > 0)
			{
				const int n = min(Level->SweepCount, FLevelLocals::MAX_SWEEP_BANDS);
				for (int i = 0; i < n; i++)
					if (Level->SweepBandPassed[i] != 0) { passedBand = true; break; }
			}
			const bool passedLook = Level->SweepPassedTintMix > 0 ||
				Level->SweepPassedDarken > 0 || Level->SweepPassedDesat > 0;
			VPUniforms.mSweepPassed = {
				(float)Level->SweepPassedTintMix, (float)Level->SweepPassedDarken,
				(float)Level->SweepPassedDesat,   (float)Level->SweepPassedSoft };
			VPUniforms.mSweepPassedColor = {
				Level->SweepPassedTint.r / 255.f, Level->SweepPassedTint.g / 255.f,
				Level->SweepPassedTint.b / 255.f, (passedBand && passedLook) ? 1.f : 0.f };
		}

		// [BB] Darkness. Frame-global for the same reason: the curve and its
		// gains are the same everywhere, and only the FRAGMENT it is asked
		// about differs.
		VPUniforms.mDarkness = {
			(float)Level->DarkMode,     (float)Level->DarkAdjust,
			(float)Level->DarkMinLight, (float)Level->DarkPreGain };
		VPUniforms.mDarkness2 = {
			(float)Level->DarkPostGain, (float)Level->DarkDistDepth,
			(float)Level->DarkDistRange, 0.0f };
		// [round2 B4] With height follow on, the reference is the viewer's feet
		// THIS frame, interpolated like the view itself, rather than the Z a
		// WorldTick last wrote into DarkHeightRef.
		double darkHeightRef = Level->DarkHeightRef;
		if (Level->DarkHeightFollow == 1 && Viewpoint.camera != nullptr)
			darkHeightRef = Viewpoint.camera->InterpolatedPosition(Viewpoint.TicFrac).Z + Level->DarkHeightOffset;
		VPUniforms.mDarkness3 = {
			(float)Level->DarkHeightDepth, (float)darkHeightRef,
			(float)Level->DarkHeightRange, 0.0f };

		// [BB] Fog slab. Doom's Z is the shader's Y, the same swizzle the
		// sweep origin and the wave origin use -- get it wrong and the mist
		// hangs against a wall instead of lying on the floor.
		//
		// ONLY THE SLAB IS BEHIND THIS GATE. Everything else made of mist --
		// the tornado, the tendrils, the disturbances, the torch that lights
		// them -- is pushed below it, unconditionally.
		//
		// They used to be inside, and that quietly cancelled the work that
		// made them independent in the first place: the shader was perfectly
		// willing to draw a funnel in clear air, and the uniform carrying its
		// density was never written unless floor fog happened to be on. A
		// feature can be switched off by code that does not mention it.
		//
		// [RS fork] WHICH SLAB. There is one set of slab uniforms, and two kinds
		// of caller: a STANDING fog (RS_Fog, pushed every tic) and a TRANSIENT
		// one (the weapon wheel's mist while it is open). Sharing SetFogSlab
		// made them fight -- the standing push replaced the transient mist, and
		// the transient ClearFogSlab wiped the standing fog. A transient caller
		// now sets the OVERRIDE, which wins while it is set and leaves the
		// standing slab untouched underneath. The override is a self-contained
		// absolute slab: its own top, density, soft edge, scatter, colour and
		// bottom, with no stack, no surface swell and no follow (see mFogFollow
		// below). Wake and pickup stay the standing slab's.
		const bool slabOvr = Level->FogSlabOverrideActive;
		const double slabTop = slabOvr ? Level->FogSlabOverrideTop : Level->FogSlabTop;
		const double slabDensity = slabOvr ? Level->FogSlabOverrideDensity : Level->FogSlabDensity;
		const double slabSoft = slabOvr ? Level->FogSlabOverrideSoft : Level->FogSlabSoft;
		const double slabScatter = slabOvr ? Level->FogSlabOverrideScatter : Level->FogSlabScatter;
		const PalEntry slabColor = slabOvr ? Level->FogSlabOverrideColor : Level->FogSlabColor;
		const bool slabOn = slabOvr || (Level->FogSlabActive && Level->FogSlabDensity > 0.0);
		if (slabOn)
		{
			VPUniforms.mFogSlab = {
				(float)slabTop, (float)slabDensity,
				(float)slabSoft, (float)slabScatter };
			VPUniforms.mFogSlabColor = {
				slabColor.r / 255.f, slabColor.g / 255.f,
				slabColor.b / 255.f, (float)Level->FogSlabWakeStrength };
			VPUniforms.mFogSlabWake = {
				(float)Level->FogSlabWakePos.X, (float)Level->FogSlabWakePos.Z,
				(float)Level->FogSlabWakePos.Y, (float)Level->FogSlabWakeRadius };
			VPUniforms.mFogSlabExtra = {
				(float)Level->FogSlabWakeStrength, (float)Level->FogSlabPickup,
				0.0f, 0.0f };
			if (slabOvr)
			{
				// The override's own bottom; no repeating stack and a flat top,
				// so a standing preset's look does not reshape a transient mist.
				VPUniforms.mFogSlab2 = { (float)Level->FogSlabOverrideBottom, 0.f, 0.f, 0.f };
				VPUniforms.mFogSurf = { 0.f, 256.f, 1.f, 0.f };
			}
			else
			{
				VPUniforms.mFogSlab2 = { (float)Level->FogSlabBottom,
					(float)Level->FogSlabPeriod, (float)Level->FogSlabRoll, 0.f };
				VPUniforms.mFogSurf = {
					(float)Level->FogSurfAmp, (float)Level->FogSurfLen,
					(float)Level->FogSurfSpeed, (float)Level->FogSurfCross };
			}
		}
		else
		{
			VPUniforms.mFogSlab = { (float)Level->FogSlabTop, 0.f, 24.f, 0.f };
			VPUniforms.mFogSurf = { 0.f, 256.f, 1.f, 0.f };

			// [RS fork] THE SLAB IS OFF, BUT ITS TOP, COLOUR, BOTTOM AND PICKUP
			// STILL HAVE TO BE CURRENT. Tendrils and ignite draw without the
			// slab (see above) and read these: with density 0 the wisps rose
			// from world Z 0 in whatever colour the last preset left (or the
			// header's orange-red). Only the components this block owns are
			// written on mFogSlabExtra/mFogSlab2, so a spare another feature
			// packs there is left alone.
			VPUniforms.mFogSlabColor = {
				Level->FogSlabColor.r / 255.f, Level->FogSlabColor.g / 255.f,
				Level->FogSlabColor.b / 255.f, (float)Level->FogSlabWakeStrength };
			VPUniforms.mFogSlabExtra.X = (float)Level->FogSlabWakeStrength;
			VPUniforms.mFogSlabExtra.Y = (float)Level->FogSlabPickup;
			VPUniforms.mFogSlab2.X = (float)Level->FogSlabBottom;
			VPUniforms.mFogSlab2.Y = (float)Level->FogSlabPeriod;
			VPUniforms.mFogSlab2.Z = (float)Level->FogSlabRoll;
		}

		// [RS fork] Diagnostic: say when the slab gate flips, and what the
		// ungated upload carries, so one test shows the colour/top are current.
		if (r_visualstate_log)
		{
			// 0 no slab drawn, 1 the standing slab, 2 the override.
			static int lastSlabGate = -1;
			const int gate = slabOvr ? 2 : (slabOn ? 1 : 0);
			if (gate != lastSlabGate)
			{
				lastSlabGate = gate;
				Printf("fog slab drawn: %s -- top %.1f density %.2f colour %02x%02x%02x (standing slab %s; colour/top/bottom/pickup uploaded either way)\n",
					gate == 2 ? "OVERRIDE" : (gate == 1 ? "standing" : "none"),
					slabTop, slabDensity, slabColor.r, slabColor.g, slabColor.b,
					(Level->FogSlabActive && Level->FogSlabDensity > 0.0) ? "set" : "off");
			}
		}

		VPUniforms.mTornado = { (float)Level->TornadoPos.X,
			(float)Level->TornadoPos.Y, (float)Level->TornadoBase,
			(float)Level->TornadoTop };
		VPUniforms.mTornado2 = { (float)Level->TornadoRadBase,
			(float)Level->TornadoRadTop, (float)Level->TornadoDensity,
			(float)Level->TornadoSwirl };
		VPUniforms.mTornado3 = { (float)Level->TornadoSpin,
			(float)Level->TornadoTwist, (float)Level->TornadoLean,
			(float)Level->TornadoLeanPeriod };
		VPUniforms.mTornadoCol = {
			Level->TornadoColor.r / 255.f, Level->TornadoColor.g / 255.f,
			Level->TornadoColor.b / 255.f, (float)Level->TornadoScatter };

		// [BB] Disturbances. Age is resolved HERE rather than in script, so a
		// ripple expands at render rate instead of in 35Hz steps -- a ring
		// crawling outward one tic at a time is a visible staircase.
		// Counted so the shader can know, from ONE compare, whether any
		// disturbance is live. Ignite adds light rather than mist and has to
		// work in a room with the fog switched off -- without this the whole
		// function early-outs before the loop that would draw it.
		int liveDisturb = 0;
		{
			// [BB] TicFrac, which this had always claimed to do and did not.
			//
			// The comment above has said "resolved HERE rather than in script,
			// so a ring expands at render rate" since this was written, but the
			// clock was plain maptime -- so a disturbance did crawl outward one
			// tic at a time, and a small fast ripple showed exactly the
			// staircase the comment warns about. Same treatment the beam block
			// above already gets from Viewpoint.TicFrac.
			double now = (Level->maptime + Viewpoint.TicFrac) / (double)TICRATE;

			for (int i = 0; i < FLevelLocals::MAX_FOG_DISTURB; i++)
			{
				double life = Level->FogDisturbLife[i];
				double age = now - Level->FogDisturbBirth[i];
				if (life <= 0.0 || age < 0.0 || age > life)
				{
					VPUniforms.mFogDisturbA[i] = { 0.f, 0.f, 0.f, 0.f };
					VPUniforms.mFogDisturbB[i] = { 0.f, 0.f, 0.f, 0.f };
					continue;
				}
				// Strength decays over the slot's life, so nothing has to be
				// freed on a schedule: an expired slot is one whose strength
				// has already reached zero. Squared, because a linear fade on
				// an expanding ring reads as a hard stop at the end.
				float fade = (float)(1.0 - age / life);
				VPUniforms.mFogDisturbA[i] = {
					(float)Level->FogDisturbPos[i].X,
					(float)Level->FogDisturbPos[i].Z,
					(float)Level->FogDisturbPos[i].Y,
					(float)Level->FogDisturbRadius[i] };
				VPUniforms.mFogDisturbB[i] = { (float)age,
					(float)Level->FogDisturbStrength[i] * fade * fade,
					(float)Level->FogDisturbSpeed[i],
					(float)Level->FogDisturbMode[i] };

				// HIGH-WATER MARK, NOT A COUNT, and the distinction is load
				// bearing now that the shader breaks on this value. Slots are
				// recycled out of order -- FogDisturb() takes the first free
				// or the oldest -- so a live set can be sparse. With slots 0
				// and 5 live, a count of 2 would stop the shader loop at 2 and
				// slot 5 would silently stop being drawn.
				//
				// Same shape as the shape loop's own `live = i + 1` a few
				// hundred lines below, for the same reason.
				liveDisturb = i + 1;
			}
		}

		VPUniforms.mFogNoise = { (float)Level->FogNoiseScale,
			(float)Level->FogNoiseDepth, (float)Level->FogNoiseDrift.X,
			(float)Level->FogNoiseDrift.Y };

		VPUniforms.mFogTendril = { (float)Level->FogTendrilSpacing,
			(float)Level->FogTendrilRadius, (float)Level->FogTendrilHeight,
			(float)Level->FogTendrilDensity };
		VPUniforms.mFogTendril2 = { (float)Level->FogTendrilRise,
			(float)Level->FogTendrilSpread, (float)Level->FogTendrilLean,
			(float)Level->FogTendrilTaper };

		VPUniforms.mFogWake2 = { (float)Level->FogWakeVel.X,
			(float)Level->FogWakeVel.Y, (float)Level->FogWakeStretch,
			// [RS fork] w: the fog IGNITE colour, packed 1 + 0xRRGGBB (exact in
			// a float, < 2^24), 0 = unset so the shader falls back to
			// uFogColor2. Was an unread 0. See SetFogIgniteColor.
			Level->FogIgniteColorSet ? (float)(1 + (int)(Level->FogIgniteColor.d & 0xffffff)) : 0.f };

		VPUniforms.mFogBow = { (float)Level->FogBowStrength,
			(float)Level->FogBowWidth, (float)Level->FogBowThin,
			(float)liveDisturb };

		// [BB] What each fog edge follows, and the floor and ceiling AT THE EYE.
		//
		// The eye pair is resolved here rather than in the shader because the
		// plane uniforms describe the FRAGMENT's sector. Using those for the
		// eye end of the ray would raise the fog around your head the moment
		// you looked at a wall on the floor above.
		{
			double eyeFloor = 0.0, eyeCeil = 0.0;
			auto vsec = Level->PointInSector(Viewpoint.Pos);
			if (vsec)
			{
				eyeFloor = vsec->floorplane.ZatPoint(Viewpoint.Pos);
				eyeCeil = vsec->ceilingplane.ZatPoint(Viewpoint.Pos);
			}
			// [RS fork] The fog slab OVERRIDE is absolute world Z, so it follows
			// nothing: a transient mist placed at a world height (the wheel's,
			// at its anchor) must not have a standing preset's follow-the-floor
			// added on top of it. Standing follow values are untouched.
			const bool followOff = Level->FogSlabOverrideActive;
			VPUniforms.mFogFollow = { followOff ? 0.f : (float)Level->FogFollowTop,
				followOff ? 0.f : (float)Level->FogFollowBottom, (float)eyeFloor, (float)eyeCeil };
		}

		// [BB] Shapes. Size, growth and the seam all resolve HERE rather than
		// in script, so a mark that opens does it at render rate instead of in
		// 35Hz steps -- a seam crawling apart one tic at a time is a visible
		// staircase, and it is the one part of the effect anyone looks at.
		//
		// [BB] TicFrac added. As with the disturbances above, this claimed
		// render rate and was reading plain maptime, so growth and the seam
		// stepped at 35Hz regardless.
		{
			double now = (Level->maptime + Viewpoint.TicFrac) / (double)TICRATE;

			// THE HIGH-WATER MARK, and it is why a 128-slot array is
			// affordable. The shader loops to this rather than to the cap, so
			// the array's size costs nothing until it is actually used.
			//
			// Recomputed here rather than tracked on add and remove: a slot
			// can also fall vacant by simply ageing out, which no caller
			// observes, and a counter that only some of the ways of becoming
			// empty know about is a counter that drifts.
			int live = 0;

			// [BB] Resolved once per slot, IN SLOT ORDER, so a linked child
			// can read its parent's ALREADY-RESOLVED world transform within
			// this same forward pass -- see the contract on ShapeParent in
			// g_levellocals.h (parent index must be smaller than the
			// child's, there is no cycle check and no topological sort).
			// Zero-initialized so a parent reference to a dead or
			// not-yet-valid slot reads a defined zero rather than garbage.
			DVector3 resolvedPos[FLevelLocals::MAX_SHAPES] = {};
			double resolvedYaw[FLevelLocals::MAX_SHAPES] = {};
			double resolvedPitch[FLevelLocals::MAX_SHAPES] = {};
			double resolvedRoll[FLevelLocals::MAX_SHAPES] = {};

			for (int i = 0; i < FLevelLocals::MAX_SHAPES; i++)
			{
				double base = Level->ShapeSize[i];
				double life = Level->ShapeLife[i];
				double age = now - Level->ShapeBirth[i];

				if (base <= 0.0 || Level->ShapeKind[i] <= 0 ||
					(life > 0.0 && (age < 0.0 || age > life)))
				{
					VPUniforms.mShapeA[i] = { 0.f, 0.f, 0.f, 0.f };
					VPUniforms.mShapeB[i] = { 0.f, 0.f, 0.f, 0.f };
					VPUniforms.mShapeCol[i] = { 0.f, 0.f, 0.f, 0.f };
					VPUniforms.mShapeD[i] = { 0.f, 0.f, 0.f, 0.f };
					VPUniforms.mShapeE[i] = { 0.f, 0.f, 0.f, 0.f };
					continue;
				}

				live = i + 1;

				// [BB] YAW/PITCH/ROLL, resolved from base + rate * age --
				// the identical shape grow/seamRate already use, just three
				// of them. Orient 0-2 (decals) never set a rate, so this is
				// a no-op arithmetic pass for every shape that isn't
				// standing -- yaw comes out exactly as authored.
				double yaw = Level->ShapeAngle[i] + Level->ShapeYawRate[i] * age;
				double pitch = Level->ShapePitch[i] + Level->ShapePitchRate[i] * age;
				double roll = Level->ShapeRoll[i] + Level->ShapeRollRate[i] * age;
				DVector3 pos = Level->ShapePos[i];

				// [BB] LINKING. A valid parent (an earlier, already-resolved
				// slot) replaces this shape's own authored position and
				// orientation with one composed onto the parent's.
				// Anything else -- no parent, or a parent index that is not
				// actually earlier -- leaves pos/yaw/pitch/roll exactly as
				// authored, which is also correct behaviour for an
				// unparented shape.
				int parent = Level->ShapeParent[i];
				if (parent >= 0 && parent < i)
				{
					double pyaw = resolvedYaw[parent];
					double ppitch = resolvedPitch[parent];
					double proll = resolvedRoll[parent];

					double pyawR = pyaw * M_PI / 180.0;
					double ppitchR = ppitch * M_PI / 180.0;
					double prollR = proll * M_PI / 180.0;

					// The parent's own facing/right/up, built the identical
					// way StandingShapesAt() builds it in main.fp -- Doom
					// space (Z up) here instead of shader space (Y up).
					// Guarded for the parent facing straight up or down,
					// where "right" would otherwise divide by a
					// near-zero-length cross product: fall back to world
					// +X as the reference instead of world-up.
					DVector3 fwd(cos(ppitchR) * cos(pyawR),
						cos(ppitchR) * sin(pyawR), sin(ppitchR));
					DVector3 worldUp(0.0, 0.0, 1.0);
					DVector3 right0 = (fabs(fwd.Z) > 0.999)
						? DVector3(1.0, 0.0, 0.0) ^ fwd
						: worldUp ^ fwd;
					if (right0.Length() > 0.0001) right0 = right0.Unit();
					DVector3 up0 = fwd ^ right0;

					double cr = cos(prollR), sr = sin(prollR);
					DVector3 right = right0 * cr + up0 * sr;
					DVector3 up = up0 * cr - right0 * sr;

					DVector3 local = Level->ShapeLocalPos[i];
					pos = resolvedPos[parent]
						+ fwd * local.X + right * local.Y + up * local.Z;

					// Euler addition onto the parent's resolved orientation
					// -- see the long comment on ShapeParent for why this
					// is exact for a pure-yaw chain and an approximation
					// once pitch and roll combine at the same joint.
					yaw = pyaw + Level->ShapeLocalYaw[i];
					pitch = ppitch + Level->ShapeLocalPitch[i];
					roll = proll + Level->ShapeLocalRoll[i];
				}

				resolvedPos[i] = pos;
				resolvedYaw[i] = yaw;
				resolvedPitch[i] = pitch;
				resolvedRoll[i] = roll;

				float fade = (life > 0.0) ? (float)(1.0 - age / life) : 1.0f;
				float size = (float)(base + Level->ShapeGrow[i] * age);
				float seam = (float)clamp(Level->ShapeSeam[i]
					+ Level->ShapeSeamRate[i] * age, 0.0, 1.0);

				VPUniforms.mShapeA[i] = { (float)pos.X, (float)pos.Z, (float)pos.Y,
					size };
				VPUniforms.mShapeB[i] = {
					(float)(Level->ShapeKind[i] + 16 * Level->ShapeOrient[i]),
					(float)yaw, (float)Level->ShapeThick[i],
					seam };
				VPUniforms.mShapeCol[i] = {
					Level->ShapeColor[i].r / 255.f, Level->ShapeColor[i].g / 255.f,
					Level->ShapeColor[i].b / 255.f,
					(float)Level->ShapeIntensity[i] * fade };
				VPUniforms.mShapeD[i] = { (float)Level->ShapeRepeat[i],
					(float)Level->ShapeRepCount[i], (float)Level->ShapeRepSpace[i],
					(float)Level->ShapeRepSpin[i] };
				// [BB] Resolved pitch/roll for StandingShapesAt() in main.fp.
				// z/w spare.
				VPUniforms.mShapeE[i] = { (float)pitch, (float)roll, 0.f, 0.f };
			}

			VPUniforms.mShapeParams = { (float)Level->ShapeSoft,
				(float)Level->ShapeHeightFade, (float)Level->ShapeReach,
				(float)live };
		}
		VPUniforms.mShapeUnder = { Level->ShapeUnder.r / 255.f,
			Level->ShapeUnder.g / 255.f, Level->ShapeUnder.b / 255.f, 0.f };

		// [BB] The sweep's room box, in SHADER space -- Y and Z swapped, the
		// same reordering every world position in this block gets. Doing it
		// here rather than in the shader keeps the swap in one place instead
		// of at every read.
		//
		// The soft distance rides on Min.w and the bound flag on Max.w, so a
		// level that never publishes a room leaves both zero and the shader's
		// test costs one compare.
		VPUniforms.mSweepRoomMin = { (float)Level->SweepRoomMin.X,
			(float)Level->SweepRoomMin.Z, (float)Level->SweepRoomMin.Y,
			(float)Level->SweepRoomSoft };
		VPUniforms.mSweepRoomMax = { (float)Level->SweepRoomMax.X,
			(float)Level->SweepRoomMax.Z, (float)Level->SweepRoomMax.Y,
			Level->SweepRoomSoft > 0 ? 1.f : 0.f };

		// w was spare and is now the global drain. See FLevelLocals::DesatGlobal.
		VPUniforms.mDesatKeep = { (float)Level->DesatKeep,
			(float)Level->DesatKeepSoft, (float)Level->DesatKeepHue,
			(float)Level->DesatGlobal };
		VPUniforms.mGlowTex = { (float)Level->GlowTexNoise,
			(float)Level->GlowTexScale, (float)Level->GlowTexDrift,
			(float)Level->GlowTexContrast };
		VPUniforms.mGlowTex2 = { (float)Level->GlowFlow,
			(float)Level->GlowFlowSpacing, (float)Level->GlowFlowSpeed,
			(float)Level->GlowFlowSharp };
		VPUniforms.mGlowTex3 = { (float)Level->GlowCell,
			(float)Level->GlowCellScale, (float)Level->GlowCellSpeed,
			(float)Level->GlowCellWidth };
		VPUniforms.mGlowTex4 = { (float)Level->GlowReact,
			(float)Level->GlowPulse, (float)Level->GlowPulseLevel,
			// [RS fork] w: pulse rate multiplier, 1 = the level's own rate.
			// Was an unread 0 -- the shader never looked at uGlowTex4.w.
			(float)Level->GlowPulseRate };

		// [round2 B3] A transient gradient wins while it is set
		// (SetFogGradientOverride); the standing FogColor2 is untouched under
		// it. Ignite's unset fallback reads uFogColor2, so it follows too.
		const bool gradOvr = Level->FogColor2OverrideActive;
		const PalEntry fogCol2 = gradOvr ? Level->FogColor2OverrideColor : Level->FogColor2;
		VPUniforms.mFogColor2 = { fogCol2.r / 255.f,
			fogCol2.g / 255.f, fogCol2.b / 255.f,
			(float)(gradOvr ? Level->FogColor2OverrideMix : Level->FogColor2Mix) };

		// The torch cone in WORLD space, so mist can be lit by it. The
		// volumetric beam pass gets its own copy in VIEW space and cannot
		// share -- see hw_viewpointuniforms.h. Outside the slab gate too:
		// a tornado standing in clear air is exactly the case that needs it.
		// The fog carries ONE torch cone -- there is a single set of mFogBeam
		// uniforms -- so with several beams live it takes the lowest live slot
		// rather than whichever was written most recently. Deterministic, and
		// slot 0 is the one a flashlight would naturally hold.
		// FirstVolBeam skips beams at density 0, so a torch at Brightness 0 (or
		// in a flicker dip) turns the fog glow off along with the air beam.
		const int fb = Level->FirstVolBeam();
		if (vol_beam_debug)
		{
			static int loggedFogSlot = -2;
			if (fb != loggedFogSlot)
			{
				loggedFogSlot = fb;
				if (fb >= 0)
					Printf("vol_beam: fog glow follows slot %d (density %.2f, falloff %.2f)\n",
						fb, Level->VolBeamDensity[fb], Level->VolBeamFalloff[fb]);
				else
					Printf("vol_beam: fog glow off (no live beam with density > 0)\n");
			}
		}
		if (fb >= 0)
		{
			// Same pose the air pass uses, anchored or not (ResolveVolBeamPose).
			DVector3 fbPos, fbDir;
			ResolveVolBeamPose(Level, fb, fbPos, fbDir);

			// THE GLOW FOLLOWS THE BEAM'S BRIGHTNESS AND FALLOFF. (FL-11)
			//
			// It used to carry position, angles and colour only, so dragging
			// Brightness to 0 hid the air beam while the fog kept a
			// full-strength torch, flicker never reached the mist, and the mist
			// used a linear fade instead of the beam's curve.
			//
			// NO NEW VIEWPOINT MEMBERS: density (already after flicker -- the
			// caller multiplies it in) is premultiplied into the colour, which
			// the shader only ever uses as a multiplier; falloff goes in
			// mFogSlabExtra.z, which was unread (main.fp reads only .y there).
			// Density 1.0 reproduces the old glow strength.
			const float fbBright = (float)std::max(Level->VolBeamDensity[fb], 0.0);
			VPUniforms.mFogBeamPos = {
				(float)fbPos.X, (float)fbPos.Z,
				(float)fbPos.Y, (float)Level->VolBeamLength[fb] };
			VPUniforms.mFogBeamDir = {
				(float)fbDir.X, (float)fbDir.Z,
				(float)fbDir.Y,
				(float)cos(Level->VolBeamInner[fb] * M_PI / 180.0) };
			VPUniforms.mFogBeamCol = {
				Level->VolBeamColor[fb].r / 255.f * fbBright,
				Level->VolBeamColor[fb].g / 255.f * fbBright,
				Level->VolBeamColor[fb].b / 255.f * fbBright,
				(float)cos(Level->VolBeamOuter[fb] * M_PI / 180.0) };
			VPUniforms.mFogSlabExtra.Z = (float)Level->VolBeamFalloff[fb];
		}
		else
		{
			VPUniforms.mFogBeamPos = { 0.f, 0.f, 0.f, 0.f };
			VPUniforms.mFogSlabExtra.Z = 0.f;
		}
	}
	mClipper->SetViewpoint(Viewpoint);
	vClipper->SetViewpoint(Viewpoint);
	rClipper->SetViewpoint(Viewpoint);

	ClearBuffers();

	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	hudsprites.Clear();
//	Coronas.Clear();
	vpIndex = 0;
	HasMultiviewViewpoints = false;
	HasMultiviewProjectionMatrix2 = false;

	// Fullbright information needs to be propagated from the main view.
	if (outer != nullptr) FullbrightFlags = outer->FullbrightFlags;
	else FullbrightFlags = 0;

	outer = gl_drawinfo;
	gl_drawinfo = this;

}

//==========================================================================
//
//
//
//==========================================================================

HWDrawInfo *HWDrawInfo::EndDrawInfo()
{
	assert(this == gl_drawinfo);
	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	gl_drawinfo = outer;
	di_list.Release(this);
	if (gl_drawinfo == nullptr)
		ResetRenderDataAllocator();
	return gl_drawinfo;
}


//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::ClearBuffers()
{
	otherFloorPlanes.Clear();
	otherCeilingPlanes.Clear();
	floodFloorSegs.Clear();
	floodCeilingSegs.Clear();

	// clear all the lists that might not have been cleared already
	MissingUpperTextures.Clear();
	MissingLowerTextures.Clear();
	MissingUpperSegs.Clear();
	MissingLowerSegs.Clear();
	SubsectorHacks.Clear();
	//CeilingStacks.Clear();
	//FloorStacks.Clear();
	HandledSubsectors.Clear();
	spriteindex = 0;

	if (Level)
	{
		CurrentMapSections.Resize(Level->NumMapSections);
		CurrentMapSections.Zero();

		section_renderflags.Resize(Level->sections.allSections.Size());
		ss_renderflags.Resize(Level->subsectors.Size());
		no_renderflags.Resize(Level->subsectors.Size());

		memset(&section_renderflags[0], 0, Level->sections.allSections.Size() * sizeof(section_renderflags[0]));
		memset(&ss_renderflags[0], 0, Level->subsectors.Size() * sizeof(ss_renderflags[0]));
		memset(&no_renderflags[0], 0, Level->nodes.Size() * sizeof(no_renderflags[0]));
	}

	Decals[0].Clear();
	Decals[1].Clear();

	mClipPortal = nullptr;
	mCurrentPortal = nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::UpdateCurrentMapSection()
{
	int mapsection = Level->PointInRenderSubsector(Viewpoint.Pos)->mapsection;
	if (Viewpoint.bDoOob || Viewpoint.bDoOrtho)
		mapsection = Level->PointInRenderSubsector(Viewpoint.OffPos)->mapsection;
	CurrentMapSections.Set(mapsection);
}


//-----------------------------------------------------------------------------
//
// Sets the area the camera is in
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetViewArea()
{
	auto &vp = Viewpoint;
	// The render_sector is better suited to represent the current position in GL
	vp.sector = Level->PointInRenderSubsector(vp.Pos)->render_sector;
	if (Viewpoint.bDoOob)
		vp.sector = Level->PointInRenderSubsector(vp.camera->Pos())->render_sector;

	// Get the heightsec state from the render sector, not the current one!
	if (vp.sector->GetHeightSec())
	{
		in_area = vp.Pos.Z <= vp.sector->heightsec->floorplane.ZatPoint(vp.Pos) ? area_below :
			(vp.Pos.Z > vp.sector->heightsec->ceilingplane.ZatPoint(vp.Pos) &&
				!(vp.sector->heightsec->MoreFlags&SECMF_FAKEFLOORONLY)) ? area_above : area_normal;
	}
	else
	{
		in_area = Level->HasHeightSecs ? area_default : area_normal;	// depends on exposed lower sectors, if map contains heightsecs.
	}
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

int HWDrawInfo::SetFullbrightFlags(player_t *player)
{
	FullbrightFlags = 0;

	// check for special colormaps
	player_t * cplayer = player? player->camera->player : nullptr;
	if (cplayer)
	{
		int cm = CM_DEFAULT;
		if (cplayer->extralight == INT_MIN)
		{
			cm = static_cast<int>(CM_FIRSTSPECIALCOLORMAP) + static_cast<int>(REALINVERSECOLORMAP);
			Viewpoint.extralight = 0;
			FullbrightFlags = Fullbright;
			// This does never set stealth vision.
		}
		else if (cplayer->fixedcolormap != NOFIXEDCOLORMAP)
		{
			cm = CM_FIRSTSPECIALCOLORMAP + cplayer->fixedcolormap;
			FullbrightFlags = Fullbright;
			if (gl_enhanced_nv_stealth > 2) FullbrightFlags |= StealthVision;
		}
		else if (cplayer->fixedlightlevel != -1 || cplayer->bForceFullbright)
		{
			EFullbrightMode fbmode = cplayer->GetFullbrightMode();
			if (fbmode != FBMODE_NONE)
			{
				FullbrightFlags = Fullbright;
				if (fbmode == FBMODE_TORCH)
				{
					FullbrightFlags |= StealthVision * (gl_enhanced_nv_stealth > 1);
				}
				else
				{
					FullbrightFlags |= Nightvision * (fbmode == FBMODE_NIGHTVISION);
					FullbrightFlags |= StealthVision * (gl_enhanced_nv_stealth > 0);
				}
			}
		}
		return cm;
	}
	else
	{
		return CM_DEFAULT;
	}
}

//-----------------------------------------------------------------------------
//
// R_FrustumAngle
//
//-----------------------------------------------------------------------------

angle_t OoBFrustumAngle(FRenderViewpoint* Viewpoint)
{
	// If pitch is larger than this you can look all around at an FOV of 90 degrees
	if (fabs(Viewpoint->HWAngles.Pitch.Degrees()) > 89.0)  return 0xffffffff;
	int aspMult = AspectMultiplier(r_viewwindow.WidescreenRatio); // 48 == square window
	double absPitch = fabs(Viewpoint->HWAngles.Pitch.Degrees());
	 // Smaller aspect ratios still clip too much. Need a better solution
	if (aspMult > 36 && absPitch > 30.0)  return 0xffffffff;
	else if (aspMult > 40 && absPitch > 25.0)  return 0xffffffff;
	else if (aspMult > 45 && absPitch > 20.0)  return 0xffffffff;
	else if (aspMult > 47 && absPitch > 10.0) return 0xffffffff;

	double xratio = r_viewwindow.FocalTangent / Viewpoint->PitchCos;
	double floatangle = 0.05 + atan ( xratio ) * 48.0 / aspMult; // this is radians
	angle_t a1 = DAngle::fromRad(floatangle).BAMs();

	if (a1 >= ANGLE_90) return 0xffffffff;
	return a1;
}

angle_t HWDrawInfo::FrustumAngle()
{
	if (Viewpoint.bDoOob)
	{
		return OoBFrustumAngle(&Viewpoint);
	}
	else
	{
		float tilt = fabs(Viewpoint.HWAngles.Pitch.Degrees());

		// If the pitch is larger than this you can look all around at a FOV of 90°
		if (tilt > 46.0f) return 0xffffffff;

		// ok, this is a gross hack that barely works...
		// but at least it doesn't overestimate too much...
		double floatangle = 2.0 + (45.0 + ((tilt / 1.9)))*Viewpoint.GetFieldOfView().Degrees() * 48.0 / AspectMultiplier(r_viewwindow.WidescreenRatio) / 90.0;
		angle_t a1 = DAngle::fromDeg(floatangle).BAMs();
		if (a1 >= ANGLE_180) return 0xffffffff;
		return a1;
	}
}

//-----------------------------------------------------------------------------
//
// Setup the modelview matrix
//
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
//
// [BB] Resolve the volumetric beam from world space into view space.
//
// Done per scene draw rather than once per frame, and that is the point: each
// eye and each portal view builds its own view matrix, so resolving here
// means stereo and portals are correct without the shader or the script
// knowing either exists.
//
// Vertex positions in this engine are (mapX, height, mapY) -- GL convention,
// Y up -- so the world point is swizzled before the matrix is applied.
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetupVolumetricBeam()
{
	if (Level == nullptr)
	{
		hw_postprocess.volbeam.ClearBeams();
		return;
	}

	auto worldToView = [this](const DVector3 &w, bool isDirection) -> FVector3
	{
		float pt[4] = { (float)w.X, (float)w.Z, (float)w.Y, isDirection ? 0.0f : 1.0f };
		float out[4];
		VPUniforms.mViewMatrix.multMatrixPoint(pt, out);
		return FVector3(out[0], out[1], out[2]);
	};

	// Every live slot gets its own uniform set. The pass draws them one after
	// another and is ADDITIVE, so they composite correctly with no blending
	// work and no shader change -- each pass contributes only its own light.
	hw_postprocess.volbeam.ClearBeams();
	for (int bi = 0; bi < FLevelLocals::MAX_VOL_BEAMS; bi++)
	{
	if (!Level->VolBeamActive[bi]) continue;

	VolumetricBeamUniforms u = {};

	// The script's world point, or the tracked pose THIS frame when the slot is
	// anchored (SetVolumetricBeamAnchor). Logged when the source changes.
	DVector3 beamPos, beamDir;
	const int poseSource = ResolveVolBeamPose(Level, bi, beamPos, beamDir);
	LogVolBeamPoseSource(bi, poseSource);

	u.BeamPos = worldToView(beamPos, false);

	FVector3 dir = worldToView(beamDir, true);
	float dl = dir.Length();
	u.BeamDir = (dl > 0.0001f) ? dir / dl : FVector3(0, 0, -1);

	u.BeamColor = FVector3(Level->VolBeamColor[bi].r / 255.f,
		Level->VolBeamColor[bi].g / 255.f,
		Level->VolBeamColor[bi].b / 255.f);

	// Half-angles arrive in degrees; the shader compares cosines, so convert
	// once here rather than per pixel.
	u.CosInner = (float)cos(Level->VolBeamInner[bi] * M_PI / 180.0);
	u.CosOuter = (float)cos(Level->VolBeamOuter[bi] * M_PI / 180.0);
	u.BeamLength = (float)Level->VolBeamLength[bi];
	u.Density = (float)Level->VolBeamDensity[bi];
	u.Falloff = (float)Level->VolBeamFalloff[bi];

	// Rebuilding the pixel ray in the shader needs the view frustum's shape,
	// which is exactly what the projection matrix's first two diagonals hold.
	const float *proj = VPUniforms.mProjectionMatrix.get();
	float px = (proj[0] != 0.0f) ? 1.0f / proj[0] : 1.0f;
	float py = (proj[5] != 0.0f) ? 1.0f / proj[5] : 1.0f;
	u.TanHalfFov = FVector2(px, py);

	// And the off-centre terms. A headset eye's frustum is asymmetric --
	// m[8]/m[9] are non-zero and opposite per eye -- and without them each
	// eye's rays were shifted sideways, putting the cone at the wrong stereo
	// depth. Zero on a symmetric projection. (FL-06; see volumetricbeam.fp.)
	u.ProjOffset = FVector2(proj[8], proj[9]);
	{
		static bool loggedAsymmetric = false;
		if (!loggedAsymmetric && (fabs(proj[8]) > 1e-4f || fabs(proj[9]) > 1e-4f))
		{
			loggedAsymmetric = true;
			Printf("vol_beam: asymmetric projection seen (offset %.4f, %.4f); beam rays include it\n",
				proj[8], proj[9]);
		}
	}

	u.StepCount = clamp((int)vol_beam_quality, 8, 64);

	// Dust is sampled in world space, so the shader needs a way back out of
	// view space. Without this the motes would ride along with the camera.
	u.DustAmount = (float)Level->VolBeamDust[bi];
	u.DustScale = (float)Level->VolBeamDustScale[bi];
	u.DustDrift = (float)Level->VolBeamDustDrift[bi];
	u.DustTime = (float)(screen->FrameTime * 0.001);
	u.AxisFade = (float)clamp<double>(vol_beam_axisfade, 0.0, 1.0);
	u.AxisFadeReach = (float)std::max<double>(vol_beam_axisfade_reach, 0.0);

	// With vol_beam_debug: which band of the axis fade this slot is in, from
	// the same distance the shader measures (eye to the beam's axis line, view
	// space). On band change only. "none" for a hand torch is the FL-09 fix.
	if (vol_beam_debug && u.AxisFade > 0.0f)
	{
		const float along = u.BeamPos.X * u.BeamDir.X + u.BeamPos.Y * u.BeamDir.Y + u.BeamPos.Z * u.BeamDir.Z;
		const float axisDist = (u.BeamPos - u.BeamDir * along).Length();
		int band = 2;   // full fade
		if (u.AxisFadeReach > 0.0f)
			band = (axisDist >= u.AxisFadeReach) ? 0 : (axisDist > u.AxisFadeReach * 0.25f ? 1 : 2);
		static int loggedBand[FLevelLocals::MAX_VOL_BEAMS] = {};   // 0 = none
		if (loggedBand[bi] != band)
		{
			loggedBand[bi] = band;
			static const char *const bandNames[] = { "none", "partial", "full" };
			Printf("vol_beam: slot %d axis fade %s (axis passes %.1f units from the eye, reach %.1f)\n",
				bi, bandNames[band], axisDist, u.AxisFadeReach);
		}
	}

	// The two constants that turn a raw depth sample back into a view-space
	// distance, identical to the pair PPAmbientOcclusion feeds lineardepth.fp.
	// The pass was comparing the raw 0..1 sample against a march distance in
	// map units, so anything in front of the camera pinned the march to under
	// one unit and the beam integrated across nothing at all.
	u.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	u.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);

	VSMatrix inv;
	if (!VPUniforms.mViewMatrix.inverseMatrix(inv)) inv.loadIdentity();
	memcpy(u.ViewToWorld, inv.get(), sizeof(float) * 16);

	hw_postprocess.volbeam.AddBeam(u);
	}
}

//-----------------------------------------------------------------------------
//
// [BB] The heatmap: where the fighting happened, painted on the floor.
//
// The grid is stamped on the CPU when something dies -- see HeatmapAdd in
// vmthunks.cpp -- and this hands it to the postprocess pass, re-uploading only
// when it has actually changed. Deaths are rare, so almost every frame this is
// four uniform writes and nothing else.
//
// A postprocess pass rather than a term in the scene shader, deliberately. The
// scene-shader route would let the heat tint the LIGHT rather than paint over
// the frame, and would be occluded correctly by translucent geometry, but it
// costs four coordinated edits inside the Vulkan backend -- a GLSL binding, a
// descriptor set layout, a descriptor POOL SIZE, and a per-frame descriptor
// write -- and missing any one of them fails either silently or on every draw.
// This route touches no backend file at all.
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetupHeatmap()
{
	if (Level == nullptr || Level->HeatScale <= 0.0 || Level->HeatIntensity.Size() == 0)
	{
		hw_postprocess.heatmap.ClearHeat();
		return;
	}

	const int R = FLevelLocals::HEAT_RES;

	// DECAY, applied here rather than on a timer, because this is the one
	// place that already knows a frame has passed and already has to re-upload
	// when the values move. A separate decay tick would dirty the grid every
	// frame forever even with nothing happening.
	if (Level->HeatDecay > 0.0)
	{
		float drop = float(Level->HeatDecay * screen->FrameTime * 0.001);
		if (drop > 0.0f)
		{
			bool moved = false;
			for (unsigned i = 0; i < Level->HeatIntensity.Size(); i++)
			{
				float v = Level->HeatIntensity[i];
				if (v <= 0.0f) continue;
				Level->HeatIntensity[i] = std::max(v - drop, 0.0f);
				moved = true;
			}
			if (moved) Level->HeatDirty = true;
		}
	}

	if (Level->HeatDirty || !hw_postprocess.heatmap.HasGrid())
	{
		// Copied rather than referenced. PPTexture keeps its data alive through
		// a shared_ptr and the backend uploads from it at an unspecified later
		// point, so handing it a pointer into a TArray the playsim is still
		// writing to would be a race the moment two monsters died in one frame.
		std::shared_ptr<void> idata(new float[R * R], [](void *p) { delete[](float*)p; });
		std::shared_ptr<void> hdata(new float[R * R], [](void *p) { delete[](float*)p; });
		memcpy(idata.get(), &Level->HeatIntensity[0], R * R * sizeof(float));
		memcpy(hdata.get(), &Level->HeatHeight[0], R * R * sizeof(float));

		hw_postprocess.heatmap.SetGrid(R, idata, hdata);
		Level->HeatDirty = false;
	}

	HeatmapUniforms u = {};
	u.HeatScale = (float)Level->HeatScale;
	u.HeatCeiling = (float)std::max(Level->HeatCeiling, 0.01);
	u.HeatTolerance = (float)std::max(Level->HeatTolerance, 1.0);
	u.HeatColorLow = FVector3(Level->HeatColorLow.r / 255.f,
		Level->HeatColorLow.g / 255.f, Level->HeatColorLow.b / 255.f);
	u.HeatColorHigh = FVector3(Level->HeatColorHigh.r / 255.f,
		Level->HeatColorHigh.g / 255.f, Level->HeatColorHigh.b / 255.f);

	// The grid covers the map's own bounding box, from the blockmap, which is
	// the one structure that already knows it.
	double mw = std::max((double)(Level->blockmap.bmapwidth * FBlockmap::MAPBLOCKUNITS), 1.0);
	double mh = std::max((double)(Level->blockmap.bmapheight * FBlockmap::MAPBLOCKUNITS), 1.0);
	u.HeatOrigin = FVector2((float)Level->blockmap.bmaporgx, (float)Level->blockmap.bmaporgy);
	u.HeatInvSize = FVector2((float)(1.0 / mw), (float)(1.0 / mh));

	// Rebuilding the pixel ray needs the frustum's shape, which is the
	// projection matrix's first two diagonals -- same as the beam pass.
	const float *proj = VPUniforms.mProjectionMatrix.get();
	u.TanHalfFov = FVector2(
		(proj[0] != 0.0f) ? 1.0f / proj[0] : 1.0f,
		(proj[5] != 0.0f) ? 1.0f / proj[5] : 1.0f);
	// And the off-centre terms of an asymmetric (headset) frustum, which the
	// rebuild ignored -- the marks landed sideways-shifted per eye. Zero on a
	// symmetric projection. Same fix as SetupVolumetricBeam (FL-06).
	u.ProjOffset = FVector2(proj[8], proj[9]);

	u.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	u.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);

	VSMatrix inv;
	if (!VPUniforms.mViewMatrix.inverseMatrix(inv)) inv.loadIdentity();
	memcpy(u.ViewToWorld, inv.get(), sizeof(float) * 16);

	hw_postprocess.heatmap.SetHeat(u);
}

void HWDrawInfo::SetViewMatrix(const FRotator &angles, float vx, float vy, float vz, bool mirror, bool planemirror)
{
	float mult = mirror ? -1.f : 1.f;
	float planemult = planemirror ? -Level->info->pixelstretch : Level->info->pixelstretch;

	VPUniforms.mViewMatrix.loadIdentity();
	VPUniforms.mViewMatrix.rotate(angles.Roll.Degrees(), 0.0f, 0.0f, 1.0f);
	VPUniforms.mViewMatrix.rotate(angles.Pitch.Degrees(), 1.0f, 0.0f, 0.0f);
	VPUniforms.mViewMatrix.rotate(angles.Yaw.Degrees(), 0.0f, mult, 0.0f);
	VPUniforms.mViewMatrix.translate(vx * mult, -vz * planemult, -vy);
	VPUniforms.mViewMatrix.scale(-mult, planemult, 1);
}


//-----------------------------------------------------------------------------
//
// SetupView
// Setup the view rotation matrix for the given viewpoint
//
//-----------------------------------------------------------------------------
void HWDrawInfo::SetupView(FRenderState &state, float vx, float vy, float vz, bool mirror, bool planemirror, bool upload)
{
	auto &vp = Viewpoint;
	vp.SetViewAngle(r_viewwindow);
	HWViewpointUniforms previousLeft = VPUniforms;
	const HWViewpointUniforms previousRight = MultiviewVPUniforms[1];
	SetViewMatrix(vp.HWAngles, vx, vy, vz, mirror, planemirror);
	SetCameraPos({ vx, vy, vz });
	VPUniforms.CalcDependencies();
	if (HasMultiviewViewpoints)
	{
		HWViewpointUniforms nextRight = VPUniforms;
		nextRight.mProjectionMatrix = previousRight.mProjectionMatrix;

		VSMatrix inverseLeft;
		if (previousLeft.mViewMatrix.inverseMatrix(inverseLeft))
		{
			VSMatrix viewDelta = inverseLeft;
			viewDelta.multMatrix(VPUniforms.mViewMatrix);
			nextRight.mViewMatrix = previousRight.mViewMatrix;
			nextRight.mViewMatrix.multMatrix(viewDelta);
		}
		else
		{
			nextRight.mViewMatrix = previousRight.mViewMatrix;
		}

		const FVector4 cameraDelta = VPUniforms.mCameraPos - previousLeft.mCameraPos;
		nextRight.mCameraPos = previousRight.mCameraPos + cameraDelta;
		nextRight.CalcDependencies();

		MultiviewVPUniforms[0] = VPUniforms;
		MultiviewVPUniforms[1] = nextRight;
	}
	if (upload)
		ApplyViewpoint(state);
}

void HWDrawInfo::ApplyViewpoint(FRenderState &state)
{
	if (HasMultiviewViewpoints)
	{
		MultiviewVPUniforms[0] = VPUniforms;
		MultiviewVPUniforms[0].CalcDependencies();
		MultiviewVPUniforms[1].CalcDependencies();
		vpIndex = screen->mViewpoints->SetViewpoints(state, MultiviewVPUniforms, 2);
	}
	else
	{
		VPUniforms.CalcDependencies();
		vpIndex = screen->mViewpoints->SetViewpoint(state, &VPUniforms);
	}
}

void HWDrawInfo::ApplyMultiviewViewpoints(FRenderState &state, const HWViewpointUniforms *viewpoints, int count)
{
	if (viewpoints == nullptr || count <= 0)
		return;

	VPUniforms = viewpoints[0];
	HasMultiviewViewpoints = count >= 2;
	if (HasMultiviewViewpoints)
	{
		MultiviewVPUniforms[0] = viewpoints[0];
		MultiviewVPUniforms[1] = viewpoints[1];
		MultiviewVPUniforms[0].CalcDependencies();
		MultiviewVPUniforms[1].CalcDependencies();
		vpIndex = screen->mViewpoints->SetViewpoints(state, MultiviewVPUniforms, 2);
	}
	else
	{
		VPUniforms.CalcDependencies();
		vpIndex = screen->mViewpoints->SetViewpoint(state, &VPUniforms);
	}
}

void HWDrawInfo::RemoveMultiviewPositionParallax()
{
	if (!HasMultiviewViewpoints)
		return;

	FLOATTYPE leftView[16];
	FLOATTYPE rightView[16];
	MultiviewVPUniforms[0].mViewMatrix.copy(leftView);
	MultiviewVPUniforms[1].mViewMatrix.copy(rightView);

	rightView[12] = leftView[12];
	rightView[13] = leftView[13];
	rightView[14] = leftView[14];
	MultiviewVPUniforms[1].mViewMatrix.loadMatrix(rightView);
	MultiviewVPUniforms[1].mCameraPos = MultiviewVPUniforms[0].mCameraPos;
	MultiviewVPUniforms[1].CalcDependencies();
	VPUniforms = MultiviewVPUniforms[0];
}

void HWDrawInfo::TranslateViewpointMatrices(double x, double y, double z)
{
	VPUniforms.mViewMatrix.translate(x, y, z);
	if (HasMultiviewViewpoints)
	{
		MultiviewVPUniforms[0].mViewMatrix.translate(x, y, z);
		MultiviewVPUniforms[1].mViewMatrix.translate(x, y, z);
		VPUniforms = MultiviewVPUniforms[0];
	}
}

void HWDrawInfo::InheritMultiviewState(const HWDrawInfo& other)
{
	HasMultiviewViewpoints = other.HasMultiviewViewpoints;
	if (HasMultiviewViewpoints)
	{
		MultiviewVPUniforms[0] = other.MultiviewVPUniforms[0];
		MultiviewVPUniforms[1] = other.MultiviewVPUniforms[1];
		VPUniforms = MultiviewVPUniforms[0];
	}

	HasMultiviewProjectionMatrix2 = other.HasMultiviewProjectionMatrix2;
	if (HasMultiviewProjectionMatrix2)
	{
		MultiviewProjectionMatrix2[0] = other.MultiviewProjectionMatrix2[0];
		MultiviewProjectionMatrix2[1] = other.MultiviewProjectionMatrix2[1];
	}
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

HWPortal * HWDrawInfo::FindPortal(const void * src)
{
	int i = Portals.Size() - 1;

	while (i >= 0 && Portals[i] && Portals[i]->GetSource() != src) i--;
	return i >= 0 ? Portals[i] : nullptr;
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

HWDecal *HWDrawInfo::AddDecal(bool onmirror)
{
	auto decal = (HWDecal*)RenderDataAllocator.Alloc(sizeof(HWDecal));
	Decals[onmirror ? 1 : 0].Push(decal);
	return decal;
}

//-----------------------------------------------------------------------------
//
// CreateScene
//
// creates the draw lists for the current scene
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//
// [BB] DispatchBillboards
//
// Every live billboard becomes a quad in the draw lists. Attached ones read
// their actor's INTERPOLATED position here rather than the ticked one, so
// they track smoothly at render framerate instead of stepping at 35Hz.
//
//-----------------------------------------------------------------------------

CVAR(Int, rs_bb_maxpanels, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)		// 0 = unlimited
CVAR(Float, rs_bb_cullradius, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// 0 = no distance limit

void HWDrawInfo::DispatchBillboards()
{
	if (!Level || Level->Billboards.Size() == 0) return;

	const auto &vp = Viewpoint;

	// Under a budget, the nearest billboards win. Distance is measured
	// squared and only compared, never rooted -- and the far ones are the
	// ones nobody can read anyway, so dropping those first is both the
	// cheapest and the least noticeable thing to do.
	const double cullR = rs_bb_cullradius;
	const double cullR2 = cullR * cullR;
	const int budget = rs_bb_maxpanels;

	double keepDist2 = FLT_MAX;
	if (budget > 0 && (int)Level->Billboards.Size() > budget)
	{
		TArray<double> dists;
		dists.Reserve(Level->Billboards.Size());
		unsigned n = 0;
		for (auto &bb : Level->Billboards)
		{
			DVector3 probe = (bb.flags & BBFL_VIEWLOCKED) ? vp.Pos : bb.pos;
			dists[n++] = (probe - vp.Pos).LengthSquared();
		}
		TArray<double> sorted = dists;
		std::sort(sorted.begin(), sorted.end());
		keepDist2 = sorted[budget - 1];
	}

	for (auto &bb : Level->Billboards)
	{
		// [BB] The group transform, resolved HERE so it moves at frame rate.
		//
		// It scales the member's LOCAL offset -- before the view-lock or the
		// attachment is resolved -- because the group origin is expressed in
		// that same local space. Scaling the world position instead would
		// drag a head-locked panel toward the map origin as it shrank.
		//
		// Attached billboards carry their offset in attachOffset rather than
		// pos, so that is what gets scaled for them.
		double gscale = 1.0;
		DVector3 gorigin(0, 0, 0);
		DVector3 lpos = bb.pos;
		DVector3 lattach = bb.attachOffset;
		if (bb.group)
		{
			gscale = Level->BillboardGroupScale(bb.group, vp.TicFrac, &gorigin);
			if (gscale <= 0.0) continue;		// fully collapsed: nothing to submit
			lpos    = gorigin + (lpos - gorigin) * gscale;
			lattach = gorigin + (lattach - gorigin) * gscale;
		}

		DVector3 bpos = lpos;

		if (bb.flags & BBFL_VIEWLOCKED)
		{
			// pos is an offset from the viewer, not a world point: X ahead,
			// Y to the right, Z up. Resolved here rather than in script
			// because script runs at tic rate and the view does not -- a
			// head-locked panel repositioned at 35Hz lags and snaps against
			// head movement, which is exactly the thing that makes people
			// ill. Doing it against the render viewpoint keeps it welded.
			double yawRad = vp.Angles.Yaw.Radians();
			double cy = cos(yawRad), sy = sin(yawRad);
			bpos = vp.Pos
				+ DVector3(cy, sy, 0.0) * lpos.X		// ahead
				+ DVector3(-sy, cy, 0.0) * lpos.Y		// right
				+ DVector3(0.0, 0.0, 1.0) * lpos.Z;		// up
		}
		else if ((bb.flags & BBFL_ATTACHED) && bb.attachedTo != nullptr)
		{
			bpos = bb.attachedTo->InterpolatedPosition(Viewpoint.TicFrac) + lattach;
		}

		// Remember where it landed so the aim and touch queries test against
		// what was actually drawn.
		bb.drawPos = bpos;

		// View-locked panels are never culled: they are welded to the eye, so
		// distance to them is meaningless and losing one to a budget would
		// read as the UI vanishing.
		if (!(bb.flags & BBFL_VIEWLOCKED))
		{
			double d2 = (bpos - vp.Pos).LengthSquared();
			if (cullR2 > 0.0 && d2 > cullR2) continue;
			if (d2 > keepDist2) continue;
		}

		auto sector = Level->PointInSector(bpos.XY());
		if (!sector) continue;

		HWSprite sprite;
		sprite.ProcessBillboard(this, &bb, bpos, sector, gscale);
	}
}

void HWDrawInfo::CreateScene(bool drawpsprites)
{
	const auto &vp = Viewpoint;
	angle_t a1 = FrustumAngle(); // horizontally clip the back of the viewport
	mClipper->SafeAddClipRangeRealAngles(vp.Angles.Yaw.BAMs() + a1, vp.Angles.Yaw.BAMs() - a1);
	Viewpoint.FrustAngle = a1;
	if (Viewpoint.bDoOob) // No need for vertical clipper if viewpoint not allowed out of bounds
	{
		double a2 = 20.0 + 0.5*Viewpoint.GetFieldOfView().Degrees(); // FrustumPitch for vertical clipping
		if (a2 > 179.0) a2 = 179.0;
		double pitchmult = !!(portalState.PlaneMirrorFlag & 1) ? -1.0 : 1.0;
		vClipper->SafeAddClipRangeDegPitches(pitchmult * vp.HWAngles.Pitch.Degrees() - a2, pitchmult * vp.HWAngles.Pitch.Degrees() + a2); // clip the suplex range
		Viewpoint.PitchSin *= pitchmult;
	}

	// reset the portal manager
	portalState.StartFrame();

	if (IsVRScene) VRSceneBuild.Clock();
	ProcessAll.Clock();

	// clip the scene and fill the drawlists
	screen->mVertexData->Map();
	screen->mLights->Map();

	RenderBSP(Level->HeadNode(), drawpsprites);

	// [BB] billboards join the scene here -- after the BSP walk has filled
	// the draw lists, before the vertex buffer unmaps below.
	DispatchBillboards();

	// And now the crappy hacks that have to be done to avoid rendering anomalies.
	// These cannot be multithreaded when the time comes because all these depend
	// on the global 'validcount' variable.

	if (IsVRScene) VRScenePostBSP.Clock();
	HandleMissingTextures(in_area);	// Missing upper/lower textures
	HandleHackedSubsectors();	// open sector hacks for deep water
	PrepareUnhandledMissingTextures();
	DispatchRenderHacks();
	if (IsVRScene) VRScenePostBSP.Unclock();
	screen->mLights->Unmap();
	screen->mVertexData->Unmap();

	ProcessAll.Unclock();
	if (IsVRScene) VRSceneBuild.Unclock();

}

//-----------------------------------------------------------------------------
//
// RenderScene
//
// Draws the current draw lists for the non GLSL renderer
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderScene(FRenderState &state)
{
	const auto &vp = Viewpoint;
	if (IsVRScene) VRSceneDraw.Clock();
	RenderAll.Clock();

	state.SetDepthMask(true);

	// [BB] A DEFINED FOG SCALE FOR EVERYTHING, not just walls, flats and
	// sprites.
	//
	// Only those three set it, and FRenderState::Reset runs once at startup --
	// so anything drawn outside them inherited whatever the previous draw left,
	// including across frames. The sky is the worst case: it is drawn BEFORE
	// this, so it took the last draw of the previous frame. In VR each eye is
	// its own pass, so the two eyes disagreed about the sky -- binocular
	// rivalry on the largest surface in view.
	//
	// Reset here so the default is 1, and the sky portal sets the outdoor
	// value for itself, a sky being the outdoor case by definition.
	state.SetFogDensityScale(1.0f);

	// [BB] Sweep: set once for the whole scene rather than per draw. It is a
	// world-space band, not a property of any sector or surface, so every
	// draw that follows inherits it and the band stays continuous across
	// floor, wall and ceiling without any of them coordinating.
	if (Level != nullptr && Level->SweepMode > 0 && Level->SweepCount > 0)
	{
		int n = min(Level->SweepCount, FLevelLocals::MAX_SWEEP_BANDS);
		state.SetSweepOrigin(Level->SweepMode,
			(float)Level->SweepOrigin.X, (float)Level->SweepOrigin.Z, (float)Level->SweepOrigin.Y, n,
			(float)Level->SweepTrail);
		for (int i = 0; i < n; i++)
		{
			state.SetSweepBand(i,
				(float)Level->SweepRadius[i], (float)Level->SweepThickness[i], (float)Level->SweepSoftness[i],
				Level->SweepColor[i].r / 255.f, Level->SweepColor[i].g / 255.f, Level->SweepColor[i].b / 255.f,
				(float)Level->SweepIntensity[i]);

			// Same swizzle as the shared origin above: Doom's Z is the
			// shader's Y. A band left at mode 0 falls back to the shared
			// origin, which SetSweepOrigin already seeded into all eight.
			// A FILL WITH NO DRAW OVERRIDE STILL HAS TO BE WRITTEN.
			// SetSweepBand seeds mode 1 into this component, so leaving
			// the call out when SweepBandDraw is 0 would silently drop the
			// fill for every band that never overrode its draw mode --
			// which is most of them.
			// [round2 B2] ...and so does a band whose only override is its
			// passed bit, which rides in the same word.
			if (Level->SweepBandDraw[i] > 0 || Level->SweepBandFill[i] > 0 || Level->SweepBandPassed[i] > 0)
			{
				int dm = Level->SweepBandDraw[i] > 0 ? Level->SweepBandDraw[i] : 1;
				state.SetSweepBandDraw(i, dm, Level->SweepBandFill[i], Level->SweepBandPassed[i]);
			}

			if (Level->SweepBandMode[i] > 0)
			{
				state.SetSweepBandOrigin(i,
					(float)Level->SweepBandOrigin[i].X,
					(float)Level->SweepBandOrigin[i].Z,
					(float)Level->SweepBandOrigin[i].Y,
					Level->SweepBandMode[i]);
			}
		}
	}
	else
	{
		state.ClearSweep();
	}

	SetupVolumetricBeam();
	SetupHeatmap();

	state.EnableFog(true);
	state.SetRenderStyle(STYLE_Source);

	if (gl_sort_textures)
	{
		drawlists[GLDL_PLAINWALLS].SortWalls();
		drawlists[GLDL_PLAINFLATS].SortFlats();
		drawlists[GLDL_MASKEDWALLS].SortWalls();
		drawlists[GLDL_MASKEDFLATS].SortFlats();
		drawlists[GLDL_MASKEDWALLSOFS].SortWalls();
	}

	// Part 1: solid geometry. This is set up so that there are no transparent parts
	state.SetDepthFunc(DF_Less);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.ClearDepthBias();

	state.EnableTexture(gl_texture);
	state.EnableBrightmap(true);
	drawlists[GLDL_PLAINWALLS].DrawWalls(this, state, false);
	drawlists[GLDL_PLAINFLATS].DrawFlats(this, state, false);


	// Part 2: masked geometry. This is set up so that only pixels with alpha>gl_mask_threshold will show
	state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
	drawlists[GLDL_MASKEDWALLS].DrawWalls(this, state, false);
	drawlists[GLDL_MASKEDFLATS].DrawFlats(this, state, false);

	// Part 3: masked geometry with polygon offset. This list is empty most of the time so only waste time on it when in use.
	if (drawlists[GLDL_MASKEDWALLSOFS].Size() > 0)
	{
		state.SetDepthBias(-1, -128);
		drawlists[GLDL_MASKEDWALLSOFS].DrawWalls(this, state, false);
		state.ClearDepthBias();
	}

	drawlists[GLDL_MODELS].Draw(this, state, false);

	state.SetRenderStyle(STYLE_Translucent);

	// Part 4: Draw decals (not a real pass)
	state.SetDepthFunc(DF_LEqual);
	DrawDecals(state, Decals[0]);

	RenderAll.Unclock();
	if (IsVRScene) VRSceneDraw.Unclock();
}

//-----------------------------------------------------------------------------
//
// RenderTranslucent
//
//-----------------------------------------------------------------------------

// RS FORK -- r_perflog: set by DrawScene around its RenderTranslucent call to
// (drawmode == DM_MAINVIEW && PerfLog::GroupsWanted()), the same value that
// guards its scene.* groups. RenderTranslucent has no drawmode of its own, and
// this keeps the fx.* groups off portal and camera-texture views.
static bool PerfLogFxGroups = false;

void HWDrawInfo::RenderTranslucent(FRenderState &state)
{
	if (IsVRScene) VRSceneDraw.Clock();
	RenderAll.Clock();

	// RS FORK -- r_perflog: read once, so each fx.* Push and its Pop agree.
	const bool perfGroups = PerfLogFxGroups;

	// final pass: translucent stuff
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.SetRenderStyle(STYLE_Translucent);

	state.EnableBrightmap(true);
	drawlists[GLDL_TRANSLUCENTBORDER].Draw(this, state, true);
	state.SetDepthMask(false);

	drawlists[GLDL_TRANSLUCENT].DrawSorted(this, state);
	state.EnableBrightmap(false);

	// [GPUPARTICLES] Stateless additive particles, one draw for the whole ring.
	// Here because depth writing is already off and depth testing on. Additive
	// blending is order-independent, so nothing is sorted. Dead slots collapse
	// to a point in the vertex shader and cost six vertex invocations each.
	//
	// Three conditions, per the plan, plus the kill switch and a shader that
	// actually compiled (a pipeline for a missing effect dereferences null):
	//   - Vulkan (mGpuParticles is null elsewhere; IsVulkan says it plainly)
	//   - main view only: portals and mirrors render through their own draw
	//     infos with mCurrentPortal set, and phase one skips them
	//   - this level's ring has been written at least once -- an empty room
	//     costs nothing at all
	//   - [PARTICLEDEFS] the definitions buffer exists: every record indexes it
	//     (set 1 binding 7), and the bone buffer only stands in for binding's sake
	//   - [VIEWLIGHTS] likewise the view light buffer (set 1 binding 8), which lit
	//     particles read
	if (r_gpuparticles && screen->IsVulkan() && mCurrentPortal == nullptr && Level != nullptr &&
		(Level->GpuParticleWritten > 0 || DebrisPool::Get().HasBillboardsAt(VPUniforms.mLevelTime.X)) &&	// [DEBRISPOOL] or pool billboards to draw
		screen->mGpuParticles != nullptr && screen->mGpuParticles->IsDrawable() &&
		screen->mParticleDefinitions != nullptr && screen->mViewLights != nullptr)
	{
		auto particles = screen->mGpuParticles;

		// [2d] This frame's level time, the clock records age by, and the blend the
		// uploaded viewpoint block told gpuparticles.fp to write for (StartScene). Read
		// back from VPUniforms rather than decided again, so the pipeline and the shader
		// can never disagree.
		const float levelTime = VPUniforms.mLevelTime.X;
		const bool premultiplied = VPUniforms.mGpuParticleParams2.Y > 0.5f;

		// [2a] SOFT PARTICLES -- readable scene depth for this ONE draw. With
		// r_gpuparticles_soft above 0 and gpuparticles' scene-depth variants
		// compiled, the scene pass is switched to hold depth read-only, and the
		// particle draw below uses the variant that fades each particle where it
		// meets a surface.
		//
		// Legal here because depth writing is already off: SetDepthMask(false)
		// runs right after the translucent BORDER list, which still draws above
		// with depth writes on, in the ordinary pass -- the switch never reaches
		// it. Scoped to the particle draw alone: the drawn lines below keep the
		// ordinary pass and exactly their pre-2a pipelines until drawnlines.fp
		// reads depth itself; then the "off" below moves under their draw.
		//
		// SetSceneDepthReadable refuses targets it cannot serve (camera textures,
		// save pictures, per-layer stereo) and particles there draw hard, as
		// before. At 0, the default, none of this runs: no switch, no extra pass,
		// no extra pipeline. fx.depthread times each switch, main view only.
		//
		// [2d] Also while a particle whose definition sets its own soft distance is
		// alive (`soft` above 0, e.g. RS_Ballistics' flames), whatever the slider says:
		// the fragment takes the definition's distance, falling back to the slider's.
		// With the slider at 0 and no such particle alive, still no switch at all.
		bool sceneDepthReadable = false;
		// [DEBRISPOOL] A debris pool piece's own soft distance asks for it too.
		if (((float)r_gpuparticles_soft > 0.f || particles->SoftAliveAt(levelTime) || DebrisPool::Get().SoftAliveAt(levelTime)) && particles->SceneDepthShaderReady)
		{
			if (perfGroups) state.PushGroup("fx.depthread");	// [2a] r_perflog
			sceneDepthReadable = state.SetSceneDepthReadable(true);
			if (perfGroups) state.PopGroup();	// [2a] r_perflog: fx.depthread
		}

		if (perfGroups) state.PushGroup("fx.gpuparticles");	// RS FORK -- r_perflog
		state.SetEffect(EFF_GPUPARTICLES);
		if (!premultiplied)
		{
			// No occluding particle alive: the additive draw exactly as before 2d.
			state.SetRenderStyle(STYLE_Add);
			state.SetVertexBuffer(particles->GetVertexBuffer(), 0, 0);
			// [PARTICLEWINDOW] E1: only the runs of slots a record can still draw in, in slot order (hw_gpuparticlewindow.h) --
			// with r_gpuparticles_window off, the whole ring in one draw, as before.
			GpuParticleWindow::SlotRange ranges[GpuParticleWindow::MAX_RANGES];
			const int rangeCount = particles->GetDrawRanges(levelTime, false, ranges, GpuParticleWindow::MAX_RANGES);
			for (int i = 0; i < rangeCount; i++)
				state.Draw(DT_Triangles, (int)(ranges[i].First * GpuParticleBuffer::VERTICES_PER_RECORD), (int)(ranges[i].Count * GpuParticleBuffer::VERTICES_PER_RECORD));
		}
		else
		{
			// [2d] PREMULTIPLIED ALPHA (One, InvSrcAlpha): smoke and dust hide what is
			// behind them, and additive particles in the same ring -- occlusion 0 --
			// add exactly what they add under STYLE_Add. Still one pipeline and no
			// sort: the ring is drawn OLDEST FIRST, [oldest, end) then [0, oldest), so
			// rising smoke tends to draw high and far first. What this does not sort --
			// particles against each other, against the translucent lists drawn above,
			// and against the drawn lines drawn below -- is in "Engine docs/
			// STAGE2D_IMPL_NOTES.md".
			FRenderStyle premultipliedStyle;
			premultipliedStyle.AsDWORD = 0;
			premultipliedStyle.BlendOp = STYLEOP_Add;
			premultipliedStyle.SrcAlpha = STYLEALPHA_One;
			premultipliedStyle.DestAlpha = STYLEALPHA_InvSrc;
			premultipliedStyle.Flags = 0;
			state.SetRenderStyle(premultipliedStyle);
			state.SetVertexBuffer(particles->GetVertexBuffer(), 0, 0);

			// [PARTICLEWINDOW] E1: the same two parts, [oldest, end) then [0, oldest), cut to the runs of slots a record can still
			// draw in (hw_gpuparticlewindow.h) -- with r_gpuparticles_window off, exactly those two draws, as before.
			GpuParticleWindow::SlotRange ranges[GpuParticleWindow::MAX_RANGES];
			const int rangeCount = particles->GetDrawRanges(levelTime, true, ranges, GpuParticleWindow::MAX_RANGES);
			for (int i = 0; i < rangeCount; i++)
				state.Draw(DT_Triangles, (int)(ranges[i].First * GpuParticleBuffer::VERTICES_PER_RECORD), (int)(ranges[i].Count * GpuParticleBuffer::VERTICES_PER_RECORD));
		}
		// [DEBRISPOOL] The debris pool's billboards, with the same effect, blend and depth, after the ring (not sorted
		// against it). DebrisPool::DrawBillboards binds its own quad buffer; the vertex data is restored below.
		DebrisPool::Get().DrawBillboards(state, levelTime);
		particles->CountDraw();

		// Restore what the rest of the translucent pass and the portal code
		// expect, the way RenderPortal restores the vertex buffer.
		state.SetEffect(EFF_NONE);
		state.SetRenderStyle(STYLE_Translucent);
		state.SetVertexBuffer(screen->mVertexData);
		if (perfGroups) state.PopGroup();	// RS FORK -- r_perflog: fx.gpuparticles

		// [2a] Back to the ordinary, writable pass before anything else draws --
		// the drawn lines next, then everything after RenderTranslucent.
		if (sceneDepthReadable)
		{
			if (perfGroups) state.PushGroup("fx.depthread");	// [2a] r_perflog
			state.SetSceneDepthReadable(false);
			if (perfGroups) state.PopGroup();	// [2a] r_perflog: fx.depthread
		}
	}

	// [DRAWNLINES] Glowing lines drawn as boxes (drawnlines.vp/.fp): SetDrawnLine
	// lines, plus the beam slots while r_beams_drawn routes them. One draw for all
	// of them, beside the particles and for the same reasons: depth writing is
	// off, depth testing on, and additive blending needs no sort. The fragment
	// shader writes its own depth -- where the glow is, not where the box face
	// is -- so walls clip a line close to where they clip the per-pixel glow.
	//
	// The particles' gates: Vulkan, main view only (portals and mirrors render
	// through draw infos with mCurrentPortal set, and do not get these), a shader
	// that compiled, and something uploaded by SyncDrawnLines this scene.
	if (r_drawnlines && screen->IsVulkan() && mCurrentPortal == nullptr &&
		screen->mDrawnLines != nullptr && screen->mDrawnLines->IsDrawable() && screen->mDrawnLines->GetLiveCount() > 0)
	{
		auto lines = screen->mDrawnLines;
		if (perfGroups) state.PushGroup("fx.drawnlines");	// RS FORK -- r_perflog; timing only, no render change
		state.SetEffect(EFF_DRAWNLINES);
		state.SetRenderStyle(STYLE_Add);
		// drawnlines.vp keeps the box faces turned away from the eye itself;
		// culling by winding would throw away half of those.
		state.SetCulling(Cull_None);
		state.SetVertexBuffer(lines->GetVertexBuffer(), 0, 0);
		state.Draw(DT_Triangles, 0, lines->GetVertexCount());
		lines->CountDraw();

		state.SetEffect(EFF_NONE);
		state.SetRenderStyle(STYLE_Translucent);
		state.SetVertexBuffer(screen->mVertexData);
		if (perfGroups) state.PopGroup();	// RS FORK -- r_perflog: fx.drawnlines
	}


	state.AlphaFunc(Alpha_GEqual, 0.5f);
	state.SetDepthMask(true);

	RenderAll.Unclock();
	if (IsVRScene) VRSceneDraw.Unclock();
}


//-----------------------------------------------------------------------------
//
// RenderTranslucent
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderPortal(HWPortal *p, FRenderState &state, bool usestencil)
{
	if (gl_max_portals > -1 && portalsPerEye >= gl_max_portals) return;
	auto gp = static_cast<HWPortal *>(p);
	gp->SetupStencil(this, state, usestencil);
	auto new_di = StartDrawInfo(this->Level, this, Viewpoint, &VPUniforms);
	new_di->InheritMultiviewState(*this);
	new_di->ProjectionMatrix2 = ProjectionMatrix2;
	new_di->mCurrentPortal = gp;
	state.SetLightIndex(-1);
	gp->DrawContents(new_di, state);
	new_di->EndDrawInfo();
	state.SetVertexBuffer(screen->mVertexData);
	screen->mViewpoints->Bind(state, vpIndex);
	gp->RemoveStencil(this, state, usestencil);

}

void HWDrawInfo::DrawCorona(FRenderState& state, ACorona* corona, double dist)
{
#if 0
	spriteframe_t* sprframe = &SpriteFrames[sprites[corona->sprite].spriteframes + (size_t)corona->SpawnState->GetFrame()];
	FTextureID patch = sprframe->Texture[0];
	if (!patch.isValid()) return;
	auto tex = TexMan.GetGameTexture(patch, false);
	if (!tex || !tex->isValid()) return;

	// Project the corona sprite center
	FVector4 worldPos((float)corona->X(), (float)corona->Z(), (float)corona->Y(), 1.0f);
	FVector4 viewPos, clipPos;
	VPUniforms.mViewMatrix.multMatrixPoint(&worldPos[0], &viewPos[0]);
	VPUniforms.mProjectionMatrix.multMatrixPoint(&viewPos[0], &clipPos[0]);
	if (clipPos.W < -1.0f) return; // clip z nearest
	float halfViewportWidth = screen->GetWidth() * 0.5f;
	float halfViewportHeight = screen->GetHeight() * 0.5f;
	float invW = 1.0f / clipPos.W;
	float screenX = halfViewportWidth + clipPos.X * invW * halfViewportWidth;
	float screenY = halfViewportHeight - clipPos.Y * invW * halfViewportHeight;

	float alpha = corona->CoronaFade * float(corona->Alpha);

	// distance-based fade - looks better IMO
	float distNearFadeStart = float(corona->RenderRadius()) * 0.1f;
	float distFarFadeStart = float(corona->RenderRadius()) * 0.5f;
	float distFade = 1.0f;

	if (float(dist) < distNearFadeStart)
		distFade -= abs(((float(dist) - distNearFadeStart) / distNearFadeStart));
	else if (float(dist) >= distFarFadeStart)
		distFade -= (float(dist) - distFarFadeStart) / distFarFadeStart;

	alpha *= distFade;

	state.SetColorAlpha(0xffffff, alpha, 0);
	if (isSoftwareLighting()) state.SetSoftLightLevel(255);
	else state.SetNoSoftLightLevel();

	state.SetLightIndex(-1);
	state.SetRenderStyle(corona->RenderStyle);
	state.SetTextureMode(corona->RenderStyle);

	state.SetMaterial(tex, UF_Sprite, CTF_Expand, CLAMP_XY_NOMIP, 0, 0);

	float scale = screen->GetHeight() / 1000.0f;
	float tileWidth = corona->Scale.X * tex->GetDisplayWidth() * scale;
	float tileHeight = corona->Scale.Y * tex->GetDisplayHeight() * scale;
	float x0 = screenX - tileWidth, y0 = screenY - tileHeight;
	float x1 = screenX + tileWidth, y1 = screenY + tileHeight;

	float u0 = 0.0f, v0 = 0.0f;
	float u1 = 1.0f, v1 = 1.0f;

	auto vert = screen->mVertexData->AllocVertices(4);
	auto vp = vert.first;
	unsigned int vertexindex = vert.second;

	vp[0].Set(x0, y0, 1.0f, u0, v0);
	vp[1].Set(x1, y0, 1.0f, u1, v0);
	vp[2].Set(x0, y1, 1.0f, u0, v1);
	vp[3].Set(x1, y1, 1.0f, u1, v1);

	state.Draw(DT_TriangleStrip, vertexindex, 4);
#endif
}

//==========================================================================
//
// TraceCallbackForDitherTransparency
// Toggles dither flag on anything that occludes the actor's
// position from viewpoint.
//
//==========================================================================

static ETraceStatus TraceCallbackForDitherTransparency(FTraceResults& res, void* userdata)
{
	BitArray* CurMapSections = (BitArray*)userdata;
	double bf, bc;

	switch(res.HitType)
	{
	case TRACE_HitWall:
		{
			sector_t* linesec = res.Line->sidedef[res.Side]->sector;
			if (linesec->subsectorcount > 0 && (*CurMapSections)[linesec->subsectors[0]->mapsection])
			{
				bf = res.Line->sidedef[res.Side]->sector->floorplane.ZatPoint(res.HitPos.XY());
				bc = res.Line->sidedef[res.Side]->sector->ceilingplane.ZatPoint(res.HitPos.XY());
				if (res.Line->sidedef[!res.Side])
				{
					// Two sided line! So let's find out if mid, top, or bottom texture needs dithered transparency
					bf = max(bf, res.Line->sidedef[!res.Side]->sector->floorplane.ZatPoint(res.HitPos.XY()));
					bc = min(bc, res.Line->sidedef[!res.Side]->sector->ceilingplane.ZatPoint(res.HitPos.XY()));
					if (res.HitPos.Z <= bf) res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_BOTTOM;
					else if (res.HitPos.Z < bc) res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_MID;
					else res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_TOP;

					res.Line->sidedef[res.Side]->dithertranscount = max<int>(1, res.Line->sidedef[!res.Side]->sector->e->XFloor.ffloors.Size());
				}
				else if ((res.HitPos.Z <= bc) && (res.HitPos.Z >= bf))
				{
					res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_MID;
					res.Line->sidedef[res.Side]->dithertranscount = 1;
				}
			}
		}
		break;
	case TRACE_HitFloor:
		if (res.Sector->subsectorcount > 0 && (*CurMapSections)[res.Sector->subsectors[0]->mapsection] && res.HitVector.dot(res.Sector->floorplane.Normal()) < 0.0)
		{
			if (res.HitPos.Z == res.Sector->floorplane.ZatPoint(res.HitPos))
			{
				res.Sector->floorplane.dithertransflag = true;
			}
			else if (res.Sector->e->XFloor.ffloors.Size()) // Maybe it was 3D floors
			{
				F3DFloor *rover;
				int kk;
				for (kk = 0; kk < (int)res.Sector->e->XFloor.ffloors.Size(); kk++)
				{
					rover = res.Sector->e->XFloor.ffloors[kk];
					if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
					{
						if (res.HitPos.Z == rover->top.plane->ZatPoint(res.HitPos))
						{
							rover->top.plane->dithertransflag = true;
							break; // Out of for loop
						}
					}
				}
			}
		}
		break;
	case TRACE_HitCeiling:
		if (res.Sector->subsectorcount > 0 && (*CurMapSections)[res.Sector->subsectors[0]->mapsection] && res.HitVector.dot(res.Sector->ceilingplane.Normal()) < 0.0)
		{
			if (res.HitPos.Z == res.Sector->ceilingplane.ZatPoint(res.HitPos))
			{
				res.Sector->ceilingplane.dithertransflag = true;
			}
			else if (res.Sector->e->XFloor.ffloors.Size()) // Maybe it was 3D floors
			{
				F3DFloor *rover;
				int kk;
				for (kk = 0; kk < (int)res.Sector->e->XFloor.ffloors.Size(); kk++)
				{
					rover = res.Sector->e->XFloor.ffloors[kk];
					if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
					{
						if (res.HitPos.Z == rover->bottom.plane->ZatPoint(res.HitPos))
						{
							rover->bottom.plane->dithertransflag = true;
							break; // Out of for loop
						}
					}
				}
			}
		}
		break;
	case TRACE_HitActor:
	default:
		break;
	}

	return TRACE_ContinueOutOfBounds;
}


void HWDrawInfo::SetDitherTransFlags(AActor* actor)
{
	// This should really be moved to a shader and have the GPU do some shape-tracing.
	if (actor && actor->Sector)
	{
		FTraceResults results;
		double horix = Viewpoint.Sin * actor->radius;
		double horiy = Viewpoint.Cos * actor->radius;
		DVector3 actorpos = actor->Pos();
		DVector3 vvec = actorpos - Viewpoint.Pos;
		if (Viewpoint.bDoOrtho)
		{
			vvec = 5.0 * Viewpoint.camera->ViewPos->Offset.Length() * Viewpoint.ViewVector3D; // Should be 4.0? (since zNear is behind screen by 3*dist in VREyeInfo::GetProjection())
		}
		double distance = vvec.Length() - actor->radius;
		DVector3 campos = actorpos - vvec;
		sector_t* startsec;

		vvec = vvec.Unit();
		campos.X -= horix; campos.Y += horiy; campos.Z += actor->Height * 0.25;
		for (int iter = 0; iter < 3; iter++)
		{
			startsec = Level->PointInRenderSubsector(campos)->sector;
			Trace(campos, startsec, vvec, distance,
				  0, 0, actor, results, TRACE_PortalRestrict, TraceCallbackForDitherTransparency, &CurrentMapSections);
			campos.Z += actor->Height * 0.5;
			Trace(campos, startsec, vvec, distance,
				  0, 0, actor, results, TRACE_PortalRestrict, TraceCallbackForDitherTransparency, &CurrentMapSections);
			campos.Z -= actor->Height * 0.5;
			campos.X += horix; campos.Y -= horiy;
		}

		// Tracers don't work on 3D floors when you are starting in the same sector (standing under them, for example)
		if (actor->Sector->e->XFloor.ffloors.Size()) // 3D floor
		{
			F3DFloor *rover;
			for (int kk = 0; kk < (int)actor->Sector->e->XFloor.ffloors.Size(); kk++)
			{
				rover = actor->Sector->e->XFloor.ffloors[kk];
				rover->top.plane->dithertransflag = true;
				rover->bottom.plane->dithertransflag = true;
			}
		}
	}
}

static ETraceStatus CheckForViewpointActor(FTraceResults& res, void* userdata)
{
	FRenderViewpoint* data = (FRenderViewpoint*)userdata;
	if (res.HitType == TRACE_HitActor && res.Actor && res.Actor == data->ViewActor)
	{
		return TRACE_Skip;
	}

	return TRACE_Stop;
}


void HWDrawInfo::DrawCoronas(FRenderState& state)
{
	state.EnableDepthTest(false);
	state.SetDepthMask(false);

	HWViewpointUniforms vp = VPUniforms;
	vp.mViewMatrix.loadIdentity();
	vp.mProjectionMatrix = VRMode::GetVRModeCached(true)->GetHUDSpriteProjection();
	screen->mViewpoints->SetViewpoint(state, &vp);

	float timeElapsed = (screen->FrameTime - LastFrameTime) / 1000.0f;
	LastFrameTime = screen->FrameTime;

#if 0
	for (ACorona* corona : Coronas)
	{
		auto cPos = corona->Vec3Offset(0., 0., corona->Height * 0.5);
		DVector3 direction = Viewpoint.Pos - cPos;
		double dist = direction.Length();

		// skip coronas that are too far
		if (dist > corona->RenderRadius())
			continue;

		static const float fadeSpeed = 9.0f;

		direction.MakeUnit();
		FTraceResults results;
		if (!Trace(cPos, corona->Sector, direction, dist, MF_SOLID, ML_BLOCKEVERYTHING, corona, results, 0, CheckForViewpointActor, &Viewpoint))
		{
			corona->CoronaFade = std::min(corona->CoronaFade + timeElapsed * fadeSpeed, 1.0f);
		}
		else
		{
			corona->CoronaFade = std::max(corona->CoronaFade - timeElapsed * fadeSpeed, 0.0f);
		}

		if (corona->CoronaFade > 0.0f)
			DrawCorona(state, corona, dist);
	}
#endif

	state.SetTextureMode(TM_NORMAL);
	screen->mViewpoints->Bind(state, vpIndex);
	state.EnableDepthTest(true);
	state.SetDepthMask(true);
}


//-----------------------------------------------------------------------------
//
// Draws player sprites and color blend
//
//-----------------------------------------------------------------------------


void HWDrawInfo::EndDrawScene(sector_t * viewsector, FRenderState &state)
{
	HWSkyInfo skyinfo;
	skyinfo.init(this, viewsector, sector_t::ceiling, viewsector->skytransfer, viewsector->Colormap.FadeColor);
	if (skyinfo.texture[0])
	{
		auto& col = R_GetSkyCapColor(skyinfo.texture[0]);
		state.SetSceneColor(col.first);
	}
	state.InitSceneClearColor();

	state.EnableFog(false);

	/*if (gl_coronas && Coronas.Size() > 0)
	{
		DrawCoronas(state);
	}*/

	// [VR] In stereo modes the weapon/HUD model is drawn inside the scene per eye,
	// never as a flat post-scene overlay, so this whole block is gated off.
	auto vrmode = VRMode::GetVRModeCached(true);
	if (!vrmode->RenderPlayerSpritesInScene())
	{
		// [BB] HUD models need to be rendered here. 
		const bool renderHUDModel = IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player);
		if (renderHUDModel)
		{
			// [BB] The HUD model should be drawn over everything else already drawn.
			state.Clear(CT_Depth);
			screen->mBones->Map();
			DrawPlayerSprites(true, state);
			screen->mBones->Unmap();
		}
	}

	state.EnableStencil(false);
	state.SetViewport(screen->mScreenViewport.left, screen->mScreenViewport.top, screen->mScreenViewport.width, screen->mScreenViewport.height);

	// Restore standard rendering state
	state.SetRenderStyle(STYLE_Translucent);
	state.ResetColor();
	state.EnableTexture(true);
	state.SetScissor(0, 0, -1, -1);
}

void HWDrawInfo::DrawEndScene2D(sector_t * viewsector, FRenderState &state)
{
	const bool renderHUDModel = IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player);
	auto vrmode = VRMode::GetVRModeCached(true);

	HWViewpointUniforms vp = VPUniforms;
	vp.mViewMatrix.loadIdentity();
	vp.mProjectionMatrix = vrmode->GetHUDProjection();
	screen->mViewpoints->SetViewpoint(state, &vp);
	state.EnableDepthTest(false);
	state.EnableMultisampling(false);

	if (!vrmode->RenderPlayerSpritesInScene())
	{
		// [BB] Only draw the sprites if we didn't render a HUD model before.
		if ( renderHUDModel == false )
		{
			DrawPlayerSprites(false, state);
		}
	}

	state.SetNoSoftLightLevel();

	// Restore standard rendering state
	state.SetRenderStyle(STYLE_Translucent);
	state.ResetColor();
	state.EnableTexture(true);
	state.SetScissor(0, 0, -1, -1);
}

//-----------------------------------------------------------------------------
//
// sets 3D viewport and initial state
//
//-----------------------------------------------------------------------------

void HWDrawInfo::Set3DViewport(FRenderState &state)
{
	// Always clear all buffers with scissor test disabled.
	// This is faster on newer hardware because it allows the GPU to skip
	// reading from slower memory where the full buffers are stored.
	state.SetScissor(0, 0, -1, -1);
	state.Clear(CT_Color | CT_Depth | CT_Stencil);

	const auto &bounds = screen->mSceneViewport;
	state.SetViewport(bounds.left, bounds.top, bounds.width, bounds.height);
	state.SetScissor(bounds.left, bounds.top, bounds.width, bounds.height);
	state.EnableMultisampling(true);
	state.EnableDepthTest(true);
	state.EnableStencil(true);
	state.SetStencil(0, SOP_Keep, SF_AllOn);
}

//-----------------------------------------------------------------------------
//
// gl_drawscene - this function renders the scene from the current
// viewpoint, including mirrors and skyboxes and other portals
// It is assumed that the HWPortal::EndFrame returns with the
// stencil, z-buffer and the projection matrix intact!
//
//-----------------------------------------------------------------------------

void HWDrawInfo::DrawScene(int drawmode)
{
	static int recursion = 0;
	static int ssao_portals_available = 0;
	auto& vp = Viewpoint;

	bool applySSAO = false;
	if (drawmode == DM_MAINVIEW)
	{
		ssao_portals_available = gl_ssao_portals;
		applySSAO = true;
		if (r_dithertransparency && vp.bDoOob)
		{
			vp.camera->tracer ? SetDitherTransFlags(vp.camera->tracer) : SetDitherTransFlags(players[consoleplayer].mo);
		}
	}
	else if (drawmode == DM_OFFSCREEN)
	{
		ssao_portals_available = 0;
	}
	else if (drawmode == DM_PORTAL && ssao_portals_available > 0)
	{
		applySSAO = (mCurrentPortal->AllowSSAO() || Level->flags3&LEVEL3_SKYBOXAO);
		ssao_portals_available--;
	}

	if (vp.camera != nullptr)
	{
		ActorRenderFlags savedflags = vp.camera->renderflags;
		CreateScene(drawmode == DM_MAINVIEW);
		vp.camera->renderflags = savedflags;
	}
	else
	{
		CreateScene(false);
	}
	auto& RenderState = *screen->RenderState();

	// RS FORK -- r_perflog: named GPU groups around the main view's passes, so
	// perflog.txt (and "stat gpu") time the scene and not only post-processing.
	// Main view only: portals recurse through DrawScene and would multiply the
	// queries. Read ONCE and used at every Push and at its Pop, so each group
	// balances; the groups sit at these call sites, so early returns inside the
	// wrapped functions cannot skip a Pop.
	const bool perfGroups = drawmode == DM_MAINVIEW && PerfLog::GroupsWanted();

	RenderState.SetDepthMask(true);
	if (!gl_no_skyclear) portalState.RenderFirstSkyPortal(recursion, this, RenderState);

	if (perfGroups) RenderState.PushGroup("scene.opaque");	// RS FORK -- r_perflog
	RenderScene(RenderState);
	if (perfGroups) RenderState.PopGroup();	// RS FORK -- r_perflog: scene.opaque

	// [MESHPARTICLES] Mesh particles -- chunks, shards, casings -- in the opaque pass ("Engine docs/
	// COLLISION_DEBRIS_MESH_PLAN.md" #10): after the solid scene, before ambient occlusion reads the
	// G-buffer and before portals and the translucent pass, which sort against their depth. The particle
	// ring's gates: the master switch, Vulkan, not inside a portal or mirror (the main view and camera
	// textures). MeshParticleBuffer::IsDrawable covers the rest: r_meshparticles, the compiled effect,
	// something live. With no definition naming a mesh nothing here runs. fx.meshparticles times it,
	// main view only.
	if (r_gpuparticles && screen->IsVulkan() && mCurrentPortal == nullptr &&
		screen->mMeshParticles != nullptr && screen->mMeshParticles->IsDrawable())
	{
		if (perfGroups) RenderState.PushGroup("fx.meshparticles");	// RS FORK -- r_perflog
		screen->mMeshParticles->Draw(RenderState);
		// What RenderScene leaves for everything after it (its decals): translucent style, LEqual.
		RenderState.SetRenderStyle(STYLE_Translucent);
		RenderState.SetDepthFunc(DF_LEqual);
		if (perfGroups) RenderState.PopGroup();	// RS FORK -- r_perflog: fx.meshparticles
	}

	// [DEBRISPOOL] The debris pool's mesh pieces ("Engine docs/DEBRIS_9_IMPL_NOTES.md"), right after the ring's mesh
	// particles and under the same gates -- the master switch, Vulkan, not inside a portal or mirror; DebrisPool::HasMeshesAt
	// covers the rest: the mesh effect compiled, the pool bound this frame, a live mesh piece. Timed in the same
	// fx.meshparticles group (same-name groups in a frame are summed).
	if (r_gpuparticles && screen->IsVulkan() && mCurrentPortal == nullptr && DebrisPool::Get().HasMeshesAt(VPUniforms.mLevelTime.X))
	{
		if (perfGroups) RenderState.PushGroup("fx.meshparticles");	// RS FORK -- r_perflog
		DebrisPool::Get().DrawMeshes(RenderState, VPUniforms.mLevelTime.X);
		// What RenderScene leaves for everything after it (its decals): translucent style, LEqual.
		RenderState.SetRenderStyle(STYLE_Translucent);
		RenderState.SetDepthFunc(DF_LEqual);
		if (perfGroups) RenderState.PopGroup();	// RS FORK -- r_perflog: fx.meshparticles
	}

	auto vrmode = VRMode::GetVRModeCached(true);
	if (drawmode == DM_MAINVIEW && vrmode->RenderPlayerSpritesInScene())
	{
		if (perfGroups) RenderState.PushGroup("scene.psprites");	// RS FORK -- r_perflog
		DrawPlayerSprites(IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player), RenderState);
		if (perfGroups) RenderState.PopGroup();	// RS FORK -- r_perflog: scene.psprites
	}

	if (applySSAO && RenderState.GetPassType() == GBUFFER_PASS)
	{
		screen->AmbientOccludeScene(VPUniforms.mProjectionMatrix.get()[5]);
		screen->mViewpoints->Bind(RenderState, vpIndex);
	}

	// Handle all portals after rendering the opaque objects but before
	// doing all translucent stuff
	if (perfGroups) RenderState.PushGroup("scene.portals");	// RS FORK -- r_perflog
	recursion++;
	portalState.EndFrame(this, RenderState);
	recursion--;
	if (perfGroups) RenderState.PopGroup();	// RS FORK -- r_perflog: scene.portals

	// RS FORK -- r_perflog: scene.translucent, with the fx.* groups nested in it
	// (see PerfLogFxGroups above RenderTranslucent).
	if (perfGroups) RenderState.PushGroup("scene.translucent");
	PerfLogFxGroups = perfGroups;
	RenderTranslucent(RenderState);
	PerfLogFxGroups = false;
	if (perfGroups) RenderState.PopGroup();

	if (drawmode == DM_MAINVIEW)
	{
		// RS FORK -- r_perflog: scene.vrextras. A timestamp at the call site only;
		// nothing inside these draws changes.
		if (perfGroups) RenderState.PushGroup("scene.vrextras");
		if (vrmode->RenderPlayerSpritesInScene())
		{
			vrmode->DrawMountedHud(this, RenderState);
		}
		DrawHitscanTracers(RenderState);
		DrawLaserSightWorld(RenderState);
		VRWheel_Draw(this, RenderState);
		if (perfGroups) RenderState.PopGroup();	// RS FORK -- r_perflog: scene.vrextras
	}
}


//-----------------------------------------------------------------------------
//
// R_RenderView - renders one view - either the screen or a camera texture
//
//-----------------------------------------------------------------------------

void HWDrawInfo::ProcessScene(bool toscreen)
{
	portalState.BeginScene();

	int mapsection = Level->PointInRenderSubsector(Viewpoint.Pos)->mapsection;
	if (Viewpoint.bDoOob || Viewpoint.bDoOrtho)
		mapsection = Level->PointInRenderSubsector(Viewpoint.OffPos)->mapsection;
	CurrentMapSections.Set(mapsection);
	screen->mBones->Map();

	// [GPUPARTICLES] Bring the GPU ring up to date with this level's CPU ring
	// before anything draws. One rule covers normal frames, level changes,
	// savegame loads and bursts bigger than the ring -- see
	// GpuParticleBuffer::Sync. Unlike the bones this is NOT cleared per frame:
	// records persist until they expire or are overwritten. Null on GL/GLES.
	//
	// [PARTICLEDEFS] The definitions those records index go up first. Only slots
	// changed since the last sync are copied: the whole named table once at
	// startup, then a slot whenever a SpawnGpuParticles look first appears. Null
	// on GL/GLES.
	if (screen->mParticleDefinitions != nullptr)
	{
		// [MESHPARTICLES] Which definitions draw as meshes this frame is settled first: their billboards
		// go up empty in the Sync below (MeshParticleBuffer::GetBillboardHidden). The mesh particle buffer
		// also takes the mesh list, resolves the models and copies the mesh definitions here. Null on
		// GL/GLES, and then nothing is hidden.
		const uint8_t *billboardHidden = nullptr;
		if (screen->mMeshParticles != nullptr)
		{
			screen->mMeshParticles->SyncDefinitions(ParticleMeshDefinitionData(), ParticleMeshDefinitionCount(), ParticleMeshGeneration(),
				ParticleDefinitionTableData(), ParticleDefinitionSlotCount(), ParticleDefinitionGeneration());
			billboardHidden = screen->mMeshParticles->GetBillboardHidden();
		}

		screen->mParticleDefinitions->Sync(ParticleDefinitionTableData(), ParticleDefinitionSlotGenerations(),
			ParticleDefinitionSlotCount(), ParticleDefinitionGeneration(),
			billboardHidden, MeshParticleBuffer::DEFINITION_SLOTS);

		// [2c] The particle atlas layer list -- which texture fills each layer, and
		// where -- goes with them. It changes only when PARTICLEDEFS lumps load, so
		// this is a generation compare on every other frame. The backend builds the
		// atlas from it on its next frame (VkTextureManager::BeginFrame).
		// [ATLASBC7] With the compressed atlas's list. First the CPU table lays the frames out again if the atlas settings changed
		// ("Engine docs/PARTICLE_ATLAS_COMPRESSED_IMPL_NOTES.md"; a compare on every other scene). It runs after the definitions'
		// Sync above, so the definitions a new layout restamps go up with the next scene, not before these lists: in a frame of
		// one scene, the scene drawn with the atlases the backend rebuilds from these lists at its next BeginFrame.
		RefreshParticleAtlasLayout(GpuParticleAtlasPolicy());
		screen->mParticleDefinitions->SyncAtlasLayers(ParticleAtlasLayerData(), ParticleAtlasLayerCount(), ParticleAtlasGeneration(),
			ParticleCompressedAtlasLayerData(), ParticleCompressedAtlasLayerCount(), ParticleCompressedAtlasSide(), ParticleCompressedAtlasGeneration());
	}
	if (screen->mGpuParticles != nullptr && Level != nullptr)
	{
		// [2d] With each definition slot's LOOK_* byte, so the ring notes how long particles
		// needing the premultiplied blend, the view lights or their own soft distance live.
		screen->mGpuParticles->Sync(Level->GpuParticles.Data(), Level->GpuParticles.Size(),
			Level->GpuParticleSerial, Level->GpuParticleWritten,
			screen->mParticleDefinitions != nullptr ? screen->mParticleDefinitions->GetSlotLooks() : nullptr,
			ParticleDefinitionBuffer::SLOTS);
		screen->mGpuParticles->DebugReport(Level->GpuParticleWritten);
	}

	// [MESHPARTICLES] The live slot lists of mesh particles, from the same ring: after its Sync (the
	// mesh draw reads the ring on the GPU) and before SyncViewLights (a lit chunk asks for the lights).
	// Null on GL/GLES.
	if (screen->mMeshParticles != nullptr && Level != nullptr)
	{
		screen->mMeshParticles->Sync(Level->GpuParticles.Data(), Level->GpuParticles.Size(),
			Level->GpuParticleSerial, Level->GpuParticleWritten, VPUniforms.mLevelTime.X);
	}

	// [DRAWNLINES] This scene's drawn lines -- SetDrawnLine lines and any beam
	// slots r_beams_drawn routes -- to the GPU before anything draws. Rebuilt
	// every scene, see SyncDrawnLines. Does nothing on GL/GLES (no buffer);
	// the r_beams_debug line prints on every backend.
	SyncDrawnLines(Level, Viewpoint.TicFrac);
	ReportBeamLines(Level);

	// [VIEWLIGHTS] The dynamic lights in view, for lit particles -- after the ring's Sync,
	// which says whether any lit particle is alive. Main view only: a camera texture
	// draws with the main view's list, which is the same world.
	if (toscreen)
		SyncViewLights(this);

	// [PPPROJECT] Each eye's world-to-clip, for custom post-process shaders' projected uniforms. Main view only. A camera
	// texture leaves the last main view's set in place, since a script's projection native runs before the frame's scene.
	if (toscreen)
	{
		const int projectionSets = HasMultiviewViewpoints ? 2 : 1;
		for (int eye = 0; eye < projectionSets; eye++)
		{
			const HWViewpointUniforms &vpu = HasMultiviewViewpoints ? MultiviewVPUniforms[eye] : VPUniforms;
			VSMatrix projection = vpu.mProjectionMatrix;
			VSMatrix view = vpu.mViewMatrix;
			PPCustomShaders::SetEyeView(eye, projection.get(), view.get());
		}
		PPCustomShaders::SetEyeSets(projectionSets);
	}

	// [HEATREFRACTION] This view's heat sources, per eye, for the heat shimmer pass --
	// before DrawScene, so no portal view reaches them. Cleared when not toscreen.
	SetupHeatSources(this, toscreen);

	// [SMOKEVOLUME] This view's smoke volume march, per eye, for the smoke pass -- before
	// DrawScene, as the heat sources. Cleared when not toscreen.
	SetupSmokeVolume(this, toscreen);

	// [EMISSIVEVOLUMES] This view's emissive volume march, per eye, for their pass -- before DrawScene, as the smoke. Cleared when not
	// toscreen.
	SetupEmissiveVolumes(this, toscreen);

	// [BLOOMOVERRIDE] The level's bloom override to the bloom pass, main view only
	// (SyncBloomOverride above). With none ever set this is a flag test.
	if (toscreen)
		SyncBloomOverride(Level);

	DrawScene(toscreen ? DM_MAINVIEW : DM_OFFSCREEN);
	screen->mBones->Unmap();
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::AddSubsectorToPortal(FSectorPortalGroup *ptg, subsector_t *sub)
{
	auto portal = FindPortal(ptg);
	if (!portal)
	{
		portal = new HWSectorStackPortal(&portalState, ptg);
		Portals.Push(portal);
	}
	auto ptl = static_cast<HWSectorStackPortal*>(portal);
	ptl->AddSubsector(sub);
}
