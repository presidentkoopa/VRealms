/*
** hw_entrypoint.cpp
**
** manages the rendering of the player's view
**
**---------------------------------------------------------------------------
**
** Copyright 2004-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "gi.h"
#include "a_dynlight.h"
#include "m_png.h"
#include "doomstat.h"
#include "r_data/r_interpolate.h"
#include "r_utility.h"
#include "d_player.h"
#include "i_time.h"
#include "swrenderer/r_swscene.h"
#include "swrenderer/r_renderer.h"
#include "hw_dynlightdata.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "v_palette.h"
#include "d_main.h"
#include "g_cvars.h"
#include "v_draw.h"

#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hw_cvars.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_clipper.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_vrmodes.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"	// [HEATREFRACTION] hw_postprocess.heatrefraction.SetEye
#include "hw_framecompute.h"	// [COMPUTE] FrameComputeInput
#include "hw_smokevolume.h"		// [SMOKEVOLUME] SmokeVolume::PrepareFrame
#include "hw_sectorplanes.h"	// [SECTORPLANES] SectorPlanes::BeginFrame
#include "hw_levelfield.h"		// [LEVELFIELD] LevelField::PrepareFrame
#include "hw_debrispool.h"		// [DEBRISPOOL] DebrisPool::PrepareFrame
#include "hw_surfacedamage.h"	// [SURFACEDAMAGE] SurfaceDamage::PrepareFrame
#include "hw_effectlights.h"	// [EFFECTLIGHTS] EffectLights::BeginFrame, AssignShadowRows, PrepareFrame
#include "hw_emissivevolumes.h"	// [EMISSIVEVOLUMES] EmissiveVolumes::BeginFrame, PrepareFrame
#include <algorithm>			// [LIGHTSHADOWS] std::nth_element in CollectLights
#include <vector>

EXTERN_CVAR(Bool, cl_capfps)
extern bool NoInterpolateView;

extern int flatVerticesPerEye;
extern int wallVerticesPerEye;
extern int portalsPerEye;
extern int lightsFlatPerEye;
extern int lightsWallPerEye;

static SWSceneDrawer *swdrawer;

void CleanSWDrawer()
{
	if (swdrawer) delete swdrawer;
	swdrawer = nullptr;
}

#include "g_levellocals.h"
#include "a_dynlight.h"


void CollectLights(FLevelLocals* Level, const DVector3& eye)
{
	IShadowMap* sm = &screen->mShadowMap;
	int lightindex = 0;

	// [EFFECTLIGHTS] The map pass also runs for effect lights while "Light shadows" is off (RenderViewpoint). Then no dynamic
	// light takes a row -- but for the lights that ask to cast, below: each keeps index 1024, which hw_dynlightdata.cpp and the
	// smoke's gather read as "no row" -- exactly what they see when the pass does not run.
	//
	// [LIGHTSHADOWS] Which dynamic lights take rows is IShadowMap::LightShadowAllowed (hw_shadowmap.cpp): a light that asks to
	// cast (LF_CASTSHADOW) by the cast-shadow setting alone, any other by "Light shadows" -- or by the cast-shadow setting too
	// while it reaches all lights. The lights that ask go FIRST, so a flash in your hand keeps its row in a map full of lamps:
	// newest first, or the nearest the eye when more of them ask than there are rows. Every other light follows in list order,
	// as before. With the setting Off the first loop does not run, and the second is exactly the loop this was for every light
	// that does not ask; a light that asks then keeps index 1024 (no shadow).
	const bool askingRows = IShadowMap::LightShadowAllowed(true);
	const bool otherRows = IShadowMap::LightShadowAllowed(false);
	if (askingRows)
	{
		static std::vector<FDynamicLight*> asking;	// main thread only; kept to spare the allocation
		asking.clear();
		for (auto light = Level->lights; light; light = light->next)
		{
			if (!light->CastShadow())
				continue;
			light->mShadowmapIndex = 1024;
			if (light->shadowmapped && light->IsActive())
				asking.push_back(light);
		}
		if (asking.size() > 1024)
		{
			std::nth_element(asking.begin(), asking.begin() + 1024, asking.end(), [&eye](const FDynamicLight* a, const FDynamicLight* b)
				{ return (a->Pos - eye).LengthSquared() < (b->Pos - eye).LengthSquared(); });
			asking.resize(1024);
		}
		for (auto light : asking)
		{
			IShadowMap::LightsShadowmapped++;
			IShadowMap::LightsCastShadow++;

			light->mShadowmapIndex = lightindex;
			sm->SetLight(lightindex, (float)light->X(), (float)light->Y(), (float)light->Z(), light->GetRadius());
			lightindex++;
		}
	}

	// Todo: this should go through the blockmap in a spiral pattern around the player so that closer lights are preferred.
	// ([LIGHTSHADOWS] The lights that ask already have the nearest rows, above.)
	for (auto light = Level->lights; light; light = light->next)
	{
		IShadowMap::LightsProcessed++;
		if (light->CastShadow())
		{
			// [LIGHTSHADOWS] Its row, or 1024, is the loop above's; with the setting Off it casts none.
			if (!askingRows)
				light->mShadowmapIndex = 1024;
			continue;
		}
		if (otherRows && light->shadowmapped && light->IsActive() && lightindex < 1024)
		{
			IShadowMap::LightsShadowmapped++;

			light->mShadowmapIndex = lightindex;
			sm->SetLight(lightindex, (float)light->X(), (float)light->Y(), (float)light->Z(), light->GetRadius());
			lightindex++;
		}
		else
		{
			light->mShadowmapIndex = 1024;
		}

	}

	// [EFFECTLIGHTS] Then the point effect lights walls block, nearest the eye first, while rows remain (hw_effectlights.cpp).
	// With none it returns lightindex unchanged.
	lightindex = EffectLights::Get().AssignShadowRows(sm, lightindex);

	for (; lightindex < 1024; lightindex++)
	{
		sm->SetLight(lightindex, 0, 0, 0, 0);
	}
}


//-----------------------------------------------------------------------------
//
// [round2 B1] Pose every anchored dynamic light from THIS frame's tracked pose.
//
// Called after VRMode::SetUp(), which is what writes this frame's AttackPos /
// OffhandPos, and before the eye loop's ++gl_dynlight_viewid, so the spot
// direction and relative-position caches keyed on that id rebuild from the
// new pose. The shadow map's CollectLights runs earlier in RenderViewpoint, so
// the SHADOW of a shadow-mapped anchored light lags one frame; the light does not.
//
// Relinks (rebuilds the per-section light lists) only once the light is more
// than 4 map units from where it was last linked. Without a relink a surface at
// the edge of the radius can miss the light for a frame; relinking for every
// sub-unit hand tremor would be wasted work.
//
// THREADING. This runs on the main thread before any RenderBSP of the frame,
// and RenderBSP starts and joins its worker threads inside itself, so nothing
// else is reading light lists or positions. The registry is only changed on the
// main thread -- by the light tick, SetAttachedLightAnchor and level teardown --
// none of which can run while a frame is being set up.
//
//-----------------------------------------------------------------------------

static void R_UpdatePoseAnchoredLights(FLevelLocals *Level)
{
	if (Level == nullptr) return;
	for (auto light : Level->PoseAnchoredLights)
	{
		if (!light->IsActive()) continue;
		if (!light->ResolvePoseAnchor()) continue;
		if ((light->Pos - light->LinkedPos).LengthSquared() > 4.0 * 4.0)
			light->LinkLight();
	}
}

//-----------------------------------------------------------------------------
//
// [COMPUTE] This frame's input for the backend's compute work (hw_framecompute.h),
// read from the level: the sector plane poll's new frame, then the smoke volume's
// demand, box and steps. Main view only, after VRMode::SetUp and the pose-anchored
// lights, before the eye loop. The backend acts on it in RunFrameCompute.
//
// THE LEVEL-DATA SERIAL. [13b] FLevelLocals::LevelDataSerial, which ClearLevelData renews
// on EVERY map change and savegame load (p_setup.cpp) -- the general serial the batched
// g_levellocals.h edit (smoke plan SH4) added. This is the one place that reads it for the
// compute clients. (13a read GpuParticleSerial here, which ClearLevelData renews at the
// same moment.)
//
//-----------------------------------------------------------------------------

static uint64_t LevelDataSerial(FLevelLocals* Level)
{
	return Level != nullptr ? Level->LevelDataSerial : 0;
}

static void PrepareFrameCompute(FLevelLocals* Level, const FRenderViewpoint& vp, FrameComputeInput& input)
{
	input = FrameComputeInput();
	if (Level == nullptr)
		return;

	const uint64_t serial = LevelDataSerial(Level);
	SectorPlanes::Get().BeginFrame(Level, serial);
	// [EFFECTLIGHTS] The frame's effect lights to the GPU: drawn-line lights (after VRMode::SetUp, for lines anchored to a hand),
	// bins and upload. Before the smoke, which a later step lights from the same bins.
	EffectLights::Get().PrepareFrame(Level, vp.Pos, vp.TicFrac);
	SmokeVolume::Get().PrepareFrame(Level, vp.Pos, vp.Angles.Yaw.Radians(), vp.TicFrac, serial, input.Smoke);
	LevelField::Get().PrepareFrame(Level, vp.Pos, serial, input.LevelField);	// [LEVELFIELD] #8
	DebrisPool::Get().PrepareFrame(Level, serial);	// [DEBRISPOOL] #9: its frame reaches the backend through DebrisPoolFrameForBackend
	SurfaceDamage::Get().PrepareFrame(Level, vp.Pos.X, vp.Pos.Y, vp.Pos.Z, vp.Angles.Yaw.Radians(), vp.Angles.Pitch.Radians(), serial);	// [SURFACEDAMAGE] #17: its frame reaches the backend through SurfaceDamageFrameForBackend
	// [EMISSIVEVOLUMES] #15: every volume with the hand poses VRMode::SetUp wrote this frame, into the list the backend uploads.
	EmissiveVolumes::Get().PrepareFrame(Level, vp.Pos, input.EmissiveVolumes);
}

//-----------------------------------------------------------------------------
//
// [PINNEDBLOOM] Whether beam light can be on screen this frame, for pinned bloom
// (hw_postprocess.h, PPLightMask::SetPinnedLightLive): any beam slot that draws with a
// non-zero intensity -- the beam upload's own test (hw_drawinfo.cpp). Read-only and
// renderer-side: it reads level state the renderer already reads, and writes only
// post-processing state.
//
//-----------------------------------------------------------------------------

static bool PinnedLightLive(FLevelLocals* Level)
{
	if (Level == nullptr)
		return false;
	for (int i = 0; i < FLevelLocals::MAX_BEAMS; i++)
	{
		if (Level->BeamSlotLive(i) && Level->BeamIntensity[i] != 0.0)
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
//
// Renders one viewpoint in a scene
//
//-----------------------------------------------------------------------------

sector_t* RenderViewpoint(FRenderViewpoint& mainvp, AActor* camera, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
{
	auto& RenderState = *screen->RenderState();

	R_SetupFrame(mainvp, r_viewwindow, camera);

	// [EMISSIVEVOLUMES] This frame's emissive volumes, once, before the effect lights: the queue drained, every volume at this
	// frame's level time, and each lit volume's light handed to the effect lights, whose BeginFrame below takes it -- so it ranks
	// and takes a shadow-map row as any effect light (hw_emissivevolumes.h). With no volume it hands over nothing.
	if (mainview && toscreen)
		EmissiveVolumes::Get().BeginFrame(camera->Level, mainvp.Pos, mainvp.Angles.Yaw.Radians(), mainvp.TicFrac, LevelDataSerial(camera->Level));

	// [EFFECTLIGHTS] This frame's effect lights, once, before the shadow map: the queue drained, every light at this frame's
	// level time, the pool capped, and the point lights walls block put in row order (hw_effectlights.h). True when they want
	// shadow-map rows, so the map pass runs for them too -- and with "Light shadows" off it then gives no dynamic light a row
	// (CollectLights). False with no effect light: the condition below is then exactly what it was.
	const bool effectLightRows = mainview && toscreen &&
		EffectLights::Get().BeginFrame(camera->Level, mainvp.Pos, mainvp.Angles.Yaw.Radians(), mainvp.TicFrac, LevelDataSerial(camera->Level),
			!(camera->Level->flags3 & LEVEL3_NOSHADOWMAP) && camera->Level->aabbTree != nullptr);

	// [LIGHTSHADOWS] The lights that ask to cast shadows run the pass by the cast-shadow setting, with "Light shadows" off too:
	// while one is live and 10 seconds after, or whenever the level has dynamic lights while the setting reaches all lights
	// (hw_dynlightdata.cpp). False while that setting is Off: the condition below is then exactly what it was.
	const bool castShadowRows = mainview && toscreen && !(camera->Level->flags3 & LEVEL3_NOSHADOWMAP) && DynamicLightShadowRowsWanted(camera->Level);

	if (mainview && toscreen && !(camera->Level->flags3 & LEVEL3_NOSHADOWMAP) && ((camera->Level->HasDynamicLights && (gl_light_shadowmap || castShadowRows)) || effectLightRows))
	{
		screen->SetAABBTree(camera->Level->aabbTree);
		// [LIGHTSHADOWS] The eye, for the rows of the lights that ask when more of them ask than there are rows (CollectLights).
		const DVector3 shadowEye = mainvp.Pos;
		screen->mShadowMap.SetCollectLights([=] {
			CollectLights(camera->Level, shadowEye);
		});
		screen->UpdateShadowMap();
	}
	else
	{
		// null all references to the level if we do not need a shadowmap. This will shortcut all internal calculations without further checks.
		screen->SetAABBTree(nullptr);
		screen->mShadowMap.SetCollectLights(nullptr);
	}

	screen->SetLevelMesh(camera->Level->levelMesh);

	// Update the attenuation flag of all light defaults for each viewpoint.
	// This function will only do something if the setting differs.
	FLightDefaults::SetAttenuationForLevel(!!(camera->Level->flags3 & LEVEL3_ATTENUATE));

	// Render (potentially) multiple views for stereo 3d
	// Fixme. The view offsetting should be done with a static table and not require setup of the entire render state for the mode.
	auto vrmode = VRMode::GetVRModeCached(mainview && toscreen);
	vrmode->SetUp();
	// [round2 B1] Anchored lights read the pose SetUp just wrote. The real frame
	// only: a camera texture's mono SetUp poses the hand from its own viewpoint.
	if (mainview && toscreen) R_UpdatePoseAnchoredLights(camera->Level);
	// [COMPUTE] This frame's GPU compute work (the smoke volume first): once, for the real
	// frame only, before the eye loop and outside any render pass. A backend records
	// nothing when no effect has work; a camera texture's view never runs it.
	if (mainview && toscreen)
	{
		FrameComputeInput computeInput;
		PrepareFrameCompute(camera->Level, mainvp, computeInput);
		screen->RunFrameCompute(computeInput);
		// [PINNEDBLOOM] Once per displayed frame, before the eye loop: whether beam light can be on
		// screen, so pinned bloom runs its extra passes only while it can (PPBloom::ChoosePlan).
		hw_postprocess.lightmask.SetPinnedLightLive(PinnedLightLive(mainvp.ViewLevel));
	}
	const int eyeCount = vrmode->mEyeCount;
	const bool useMultiviewScene = mainview && toscreen && vrmode->ShouldUseMultiviewThisFrame() && eyeCount >= 2;
	int sharedPostprocessColormap = CM_DEFAULT;
	float sharedPostprocessFlash = 1.0f;
	bool hasSharedPostprocessState = false;
	screen->FirstEye();
	for (int eye_ix = 0; eye_ix < eyeCount; ++eye_ix)
	{
		++gl_dynlight_viewid;
		flatVerticesPerEye = wallVerticesPerEye = portalsPerEye = lightsFlatPerEye = lightsWallPerEye = 0;
		const auto eye = vrmode->mEyes[eye_ix];
		if (eye == nullptr)
		{
			continue;
		}
		eye->SetUp();
		const bool isVRScene = vrmode->IsVR();
		if (isVRScene) VRSceneEyes.Clock();
		screen->SetViewportRects(bounds);
		const bool renderSceneThisEye = !useMultiviewScene || eye_ix == 0;
		const bool usePostprocessOnlyEye = useMultiviewScene &&
			!renderSceneThisEye &&
			mainview &&
			toscreen &&
			vrmode->RenderPlayerSpritesInScene() &&
			hasSharedPostprocessState;
		const bool useSSAO = (gl_ssao != 0);

		if (usePostprocessOnlyEye)
		{
			RenderState.SetSpecialColormap(sharedPostprocessColormap, sharedPostprocessFlash);
			eye->AdjustHud();

			PostProcess.Clock();
			// [HEATREFRACTION] This eye has no scene of its own: the heat pass takes the
			// source set the multiview scene published for it (review S8).
			hw_postprocess.heatrefraction.SetEye(eye_ix);
			hw_postprocess.smokevolume.SetEye(eye_ix);	// [SMOKEVOLUME] and the smoke march set
			hw_postprocess.emissivevolumes.SetEye(eye_ix);	// [EMISSIVEVOLUMES] and the emissive volume march set
			PPCustomShaders::SetEye(eye_ix);	// [PPPROJECT] and the projected uniforms' eye
			screen->PostProcessScene(false, sharedPostprocessColormap, sharedPostprocessFlash, []() {});
			eye->AdjustBlend(nullptr);
			V_DrawBlend(mainvp.sector);
			PostProcess.Unclock();

			RenderState.SetSpecialColormap(CM_DEFAULT, 1);
			eye->TearDown();
			screen->NextEye(eyeCount);
			if (isVRScene) VRSceneEyes.Unclock();
			continue;
		}

		if (mainview && renderSceneThisEye) // Bind the scene frame buffer and turn on draw buffers used by ssao
		{
			screen->SetSceneRenderTarget(useSSAO);
			RenderState.SetPassType(useSSAO ? GBUFFER_PASS : NORMAL_PASS);
			RenderState.EnableDrawBuffers(RenderState.GetPassDrawBufferCount(), true);
		}

		auto di = HWDrawInfo::StartDrawInfo(mainvp.ViewLevel, nullptr, mainvp, nullptr);
		auto& vp = di->Viewpoint;

		if (renderSceneThisEye)
			di->Set3DViewport(RenderState);
		di->SetViewArea();
		auto cm = di->SetFullbrightFlags(mainview ? vp.camera->player : nullptr);
		float flash = 1.f;
		if (renderSceneThisEye)
		{
			sharedPostprocessColormap = cm;
			sharedPostprocessFlash = flash;
			hasSharedPostprocessState = true;
		}

		// Only used by the GLES2 renderer
		RenderState.SetSpecialColormap(cm, flash);

		di->Viewpoint.SetFieldOfView(eye->GetRenderFov(DAngle::fromDeg(fov)));	// Match the clipper FOV to the active eye projection in VR.

		// Stereo mode specific perspective projection
		float inv_iso_dist = 1.0f;
		bool iso_ortho = (camera->ViewPos != NULL) && (camera->ViewPos->Flags & VPSF_ORTHOGRAPHIC);
		if (iso_ortho && (camera->ViewPos->Offset.Length() > 0)) inv_iso_dist = 1.0/camera->ViewPos->Offset.Length();
		di->VPUniforms.mProjectionMatrix = eye->GetProjection(fov, ratio, fovratio * inv_iso_dist, iso_ortho);
		di->ProjectionMatrix2 = eye->GetProjection(fov, ratio, fovratio, false); // Regular ol' perspective projection matrix

		const DVector3 baseViewPos = vp.Pos;
		FRenderViewpoint centerView = vp;
		centerView.Pos = baseViewPos;
		const DVector3 eyeShift = eye->GetViewShift(centerView);
		vp.Pos = baseViewPos + eyeShift;
		if (useMultiviewScene && eye_ix == 0 && vrmode->mEyes[1] != nullptr)
		{
			di->SetupView(RenderState, vp.Pos.X, vp.Pos.Y, vp.Pos.Z, false, false, false);
			eye->AdjustViewpointUniforms(di->VPUniforms);
			HWViewpointUniforms viewpoints[2];
			viewpoints[0] = di->VPUniforms;

			const auto secondEye = vrmode->mEyes[1];
			secondEye->SetUp();
			di->VPUniforms.mProjectionMatrix = secondEye->GetProjection(fov, ratio, fovratio * inv_iso_dist, iso_ortho);
			di->MultiviewProjectionMatrix2[0] = di->ProjectionMatrix2;
			di->MultiviewProjectionMatrix2[1] = secondEye->GetProjection(fov, ratio, fovratio, false);
			di->HasMultiviewProjectionMatrix2 = true;
			const DVector3 eyeShift2 = secondEye->GetViewShift(centerView);
			const DVector3 secondEyePos = baseViewPos + eyeShift2;
			di->SetupView(RenderState, secondEyePos.X, secondEyePos.Y, secondEyePos.Z, false, false, false);
			secondEye->AdjustViewpointUniforms(di->VPUniforms);
			viewpoints[1] = di->VPUniforms;

			di->ApplyMultiviewViewpoints(RenderState, viewpoints, 2);
		}
		else
		{
			di->SetupView(RenderState, vp.Pos.X, vp.Pos.Y, vp.Pos.Z, false, false, false);
			eye->AdjustViewpointUniforms(di->VPUniforms);
			di->ApplyViewpoint(RenderState);
		}

		if (renderSceneThisEye)
			di->ProcessScene(toscreen);
		eye->AdjustHud();

		if (mainview)
		{
			PostProcess.Clock();
			if (toscreen && renderSceneThisEye) di->EndDrawScene(mainvp.sector, RenderState); // do not call this for camera textures.

			if (renderSceneThisEye && RenderState.GetPassType() == GBUFFER_PASS) // Turn off ssao draw buffers
			{
				RenderState.SetPassType(NORMAL_PASS);
				RenderState.EnableDrawBuffers(1);
			}

			// [HEATREFRACTION] Which eye's heat sources the pass takes (review S8).
			hw_postprocess.heatrefraction.SetEye(eye_ix);
			hw_postprocess.smokevolume.SetEye(eye_ix);	// [SMOKEVOLUME] and which eye's smoke march
			hw_postprocess.emissivevolumes.SetEye(eye_ix);	// [EMISSIVEVOLUMES] and which eye's emissive volume march
			PPCustomShaders::SetEye(eye_ix);	// [PPPROJECT] and the projected uniforms' eye
			screen->PostProcessScene(false, cm, flash, [&]() {
				di->DrawEndScene2D(mainvp.sector, RenderState);
			});

			eye->AdjustBlend(di);
			V_DrawBlend(mainvp.sector);
			PostProcess.Unclock();
		}
		// Reset colormap so 2D drawing isn't affected
		RenderState.SetSpecialColormap(CM_DEFAULT, 1);

		di->EndDrawInfo();
		eye->TearDown();
		screen->NextEye(eyeCount);
		if (isVRScene) VRSceneEyes.Unclock();
	}
	vrmode->TearDown();
	
	return mainvp.sector;
}

void DoWriteSavePic(FileWriter* file, ESSType ssformat, uint8_t* scr, int width, int height, sector_t* viewsector, bool upsidedown)
{
	PalEntry palette[256];
	PalEntry modulateColor;
	auto blend = V_CalcBlend(viewsector, &modulateColor);
	int pixelsize = 1;
	// Apply the screen blend, because the renderer does not provide this.
	if (ssformat == SS_RGB)
	{
		int numbytes = width * height * 3;
		pixelsize = 3;
		if (modulateColor != 0xffffffff)
		{
			float r = modulateColor.r / 255.f;
			float g = modulateColor.g / 255.f;
			float b = modulateColor.b / 255.f;
			for (int i = 0; i < numbytes; i += 3)
			{
				scr[i] = uint8_t(scr[i] * r);
				scr[i + 1] = uint8_t(scr[i + 1] * g);
				scr[i + 2] = uint8_t(scr[i + 2] * b);
			}
		}
		float iblendfac = 1.f - blend.W;
		blend.X *= blend.W;
		blend.Y *= blend.W;
		blend.Z *= blend.W;
		for (int i = 0; i < numbytes; i += 3)
		{
			scr[i] = uint8_t(scr[i] * iblendfac + blend.X);
			scr[i + 1] = uint8_t(scr[i + 1] * iblendfac + blend.Y);
			scr[i + 2] = uint8_t(scr[i + 2] * iblendfac + blend.Z);
		}
	}
	else
	{
		// Apply the screen blend to the palette. The colormap related parts get skipped here because these are already part of the image.
		DoBlending(GPalette.BaseColors, palette, 256, uint8_t(blend.X), uint8_t(blend.Y), uint8_t(blend.Z), uint8_t(blend.W * 255));
	}

	int pitch = width * pixelsize;
	if (upsidedown)
	{
		scr += ((height - 1) * width * pixelsize);
		pitch *= -1;
	}

	M_CreatePNG(file, scr, ssformat == SS_PAL ? palette : nullptr, ssformat, width, height, pitch, vid_gamma);
}

//===========================================================================
//
// Render the view to a savegame picture
//
//===========================================================================

void WriteSavePic(player_t* player, FileWriter* file, int width, int height)
{
	if (!V_IsHardwareRenderer())
	{
		SWRenderer->WriteSavePic(player, file, width, height);
	}
	else
	{
		IntRect bounds;
		bounds.left = 0;
		bounds.top = 0;
		bounds.width = width;
		bounds.height = height;
		auto& RenderState = *screen->RenderState();

		// we must be sure the GPU finished reading from the buffer before we fill it with new data.
		screen->WaitForCommands(false);

		// Switch to render buffers dimensioned for the savepic
		screen->SetSaveBuffers(true);
		screen->ImageTransitionScene(true);

		hw_postprocess.SetTonemapMode(level.info ? level.info->tonemap : ETonemapMode::None);
		hw_ClearFakeFlat();
		screen->mVertexData->Reset();
		RenderState.SetVertexBuffer(screen->mVertexData);
		screen->mLights->Clear();
		screen->mBones->Clear();
		screen->mViewpoints->Clear();

		// This shouldn't overwrite the global viewpoint even for a short time.
		FRenderViewpoint savevp;
		sector_t* viewsector = RenderViewpoint(savevp, players[consoleplayer].camera, &bounds, r_viewpoint.GetFieldOfView().Degrees(), 1.6f, 1.6f, true, false);
		RenderState.EnableStencil(false);
		RenderState.SetNoSoftLightLevel();

		TArray<uint8_t> scr(width * height * 3, true);
		screen->CopyScreenToBuffer(width, height, scr.Data());

		DoWriteSavePic(file, SS_RGB, scr.Data(), width, height, viewsector, screen->FlipSavePic());

		// Switch back the screen render buffers
		screen->SetViewportRects(nullptr);
		screen->SetSaveBuffers(false);
	}
}

//===========================================================================
//
// Renders the main view
//
//===========================================================================

static void CheckTimer(FRenderState &state, uint64_t ShaderStartTime)
{
	// if firstFrame is not yet initialized, initialize it to current time
	// if we're going to overflow a float (after ~4.6 hours, or 24 bits), re-init to regain precision
	if ((state.firstFrame == 0) || (screen->FrameTime - state.firstFrame >= 1 << 24) || ShaderStartTime >= state.firstFrame)
		state.firstFrame = screen->FrameTime - 1;
}


sector_t* RenderView(player_t* player)
{
	auto RenderState = screen->RenderState();
	RenderState->SetVertexBuffer(screen->mVertexData);
	screen->mVertexData->Reset();
	hw_postprocess.SetTonemapMode(level.info ? level.info->tonemap : ETonemapMode::None);

	if (level.flags3 & LEVEL3_NOAMBIENTOCCLUSION)
	{
		hw_postprocess.SetNoAmbientOcclusion();
	}

	sector_t* retsec;
	if (!V_IsHardwareRenderer())
	{
		screen->SetActiveRenderTarget();	// only relevant for Vulkan

		if (!swdrawer) swdrawer = new SWSceneDrawer;
		retsec = swdrawer->RenderView(player);
	}
	else
	{
		hw_ClearFakeFlat();

		iter_dlightf = iter_dlight = draw_dlight = draw_dlightf = 0;

		CheckBenchActive();

		// reset statistics counters
		ResetProfilingData();

		// Get this before everything else
		if (cl_capfps || r_NoInterpolate) r_viewpoint.TicFrac = 1.;
		else r_viewpoint.TicFrac = I_GetTimeFrac();

		screen->mLights->Clear();
		screen->mBones->Clear();
		screen->mViewpoints->Clear();

		// NoInterpolateView should have no bearing on camera textures, but needs to be preserved for the main view below.
		bool saved_niv = NoInterpolateView;
		NoInterpolateView = false;

		// Shader start time does not need to be handled per level. Just use the one from the camera to render from.
		if (player->camera)
			CheckTimer(*RenderState, player->camera->Level->ShaderStartTime);

		// Draw all canvases that changed
		for (FCanvas* canvas : AllCanvases)
		{
			if (canvas->Tex && canvas->Tex->CheckNeedsUpdate())
			{
				screen->RenderTextureView(canvas->Tex, [=](IntRect& bounds)
					{
						screen->SetViewportRects(&bounds);
						// Translucent canvases (e.g. VR HUD, future UI surfaces) need the
						// FBO cleared to transparent black so pixels with no HUD content
						// carry alpha=0. This makes the texture correct on any surface
						// that samples it (VR quad, model texture, world geometry)
						// without needing per-consumer workarounds.
						if (canvas->Tex->bTranslucentCanvas)
						{
							auto& rs = *screen->RenderState();
							float savedClear[4];
							memcpy(savedClear, screen->mSceneClearColor, sizeof(savedClear));
							screen->mSceneClearColor[0] = screen->mSceneClearColor[1] =
							screen->mSceneClearColor[2] = screen->mSceneClearColor[3] = 0.f;
							rs.Clear(CT_Color);
							memcpy(screen->mSceneClearColor, savedClear, sizeof(savedClear));
						}
						Draw2D(&canvas->Drawer, *screen->RenderState(), 0, 0, canvas->Tex->GetWidth(), canvas->Tex->GetHeight());
						canvas->Drawer.Clear();
					});
				canvas->Tex->SetUpdated(true);
			}
		}

		// prepare all camera textures that have been used in the last frame.
		// This must be done for all levels, not just the primary one!
		for (auto Level : AllLevels())
		{
			Level->canvasTextureInfo.UpdateAll([&](AActor* camera, FCanvasTexture* camtex, double fov)
				{
					screen->RenderTextureView(camtex, [=](IntRect& bounds)
						{
							FRenderViewpoint texvp;
							float ratio = camtex->aspectRatio / Level->info->pixelstretch;
							RenderViewpoint(texvp, camera, &bounds, fov, ratio, ratio, false, false);
						});
				});
		}
		NoInterpolateView = saved_niv;

		// now render the main view
		float fovratio;
		float ratio = r_viewwindow.WidescreenRatio;
		if (r_viewwindow.WidescreenRatio >= 1.3f)
		{
			fovratio = 1.333333f;
		}
		else
		{
			fovratio = ratio;
		}

		auto vrmode = VRMode::GetVRModeCached(true);
		VR_EnsureHudSurface(screen->GetWidth() * vrmode->mHorizontalViewportScale, screen->GetHeight() * vrmode->mVerticalViewportScale);

		screen->ImageTransitionScene(true); // Only relevant for Vulkan.

		retsec = RenderViewpoint(r_viewpoint, player->camera, NULL, r_viewpoint.GetFieldOfView().Degrees(), ratio, fovratio, true, true);
	}
	All.Unclock();
	return retsec;
}
