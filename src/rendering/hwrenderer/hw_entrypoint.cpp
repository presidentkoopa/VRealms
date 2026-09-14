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


void CollectLights(FLevelLocals* Level)
{
	IShadowMap* sm = &screen->mShadowMap;
	int lightindex = 0;

	// Todo: this should go through the blockmap in a spiral pattern around the player so that closer lights are preferred.
	for (auto light = Level->lights; light; light = light->next)
	{
		IShadowMap::LightsProcessed++;
		if (light->shadowmapped && light->IsActive() && lightindex < 1024)
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
// Renders one viewpoint in a scene
//
//-----------------------------------------------------------------------------

sector_t* RenderViewpoint(FRenderViewpoint& mainvp, AActor* camera, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
{
	auto& RenderState = *screen->RenderState();

	R_SetupFrame(mainvp, r_viewwindow, camera);

	if (mainview && toscreen && !(camera->Level->flags3 & LEVEL3_NOSHADOWMAP) && camera->Level->HasDynamicLights && gl_light_shadowmap)
	{
		screen->SetAABBTree(camera->Level->aabbTree);
		screen->mShadowMap.SetCollectLights([=] {
			CollectLights(camera->Level);
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
