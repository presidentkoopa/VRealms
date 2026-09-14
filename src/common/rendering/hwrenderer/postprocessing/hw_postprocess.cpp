/*
** hw_postprocess.cpp
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

#include "v_video.h"
#include "hw_postprocess.h"
#include "hw_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocessshader.h"
#include <random>
#include <cmath>	// [BLOOMSAFETY] std::isfinite
#include <cstring>	// [PINNEDBLOOM] memcmp
#include "texturemanager.h"
#include "hw_renderstate.h"	// [BLOOMOVERRIDE] FRenderState::firstFrame, the scene shaders' timer origin

#include "stats.h"
#include "printf.h"   // vol_beam diagnostics in PPVolumetricBeam::Render

Postprocess hw_postprocess;

PPResource *PPResource::First = nullptr;
TArray<PostProcessShader> PostProcessShaders;

bool gpuStatActive = false;
bool keepGpuStatActive = false;
FString gpuStatOutput;

ADD_STAT(gpu)
{
	keepGpuStatActive = true;
	return gpuStatOutput;
}

/////////////////////////////////////////////////////////////////////////////

void PPBloom::UpdateTextures(int width, int height)
{
	if (width == lastWidth && height == lastHeight)
		return;

	int bloomWidth = (width + 1) / 2;
	int bloomHeight = (height + 1) / 2;

	for (int i = 0; i < NumBloomLevels; i++)
	{
		auto &blevel = levels[i];
		blevel.Viewport.left = 0;
		blevel.Viewport.top = 0;
		blevel.Viewport.width = (bloomWidth + 1) / 2;
		blevel.Viewport.height = (bloomHeight + 1) / 2;
		blevel.VTexture = { blevel.Viewport.width, blevel.Viewport.height, PixelFormat::Rgba16f };
		blevel.HTexture = { blevel.Viewport.width, blevel.Viewport.height, PixelFormat::Rgba16f };

		bloomWidth = blevel.Viewport.width;
		bloomHeight = blevel.Viewport.height;
	}

	lastWidth = width;
	lastHeight = height;
}


/////////////////////////////////////////////////////////////////////////////

// [BB] Volumetric beam. Drawn BEFORE bloom on purpose: a real beam in the air
// picks up bloom and feeds the auto-exposure meter, which is most of what
// sells it as light rather than as a drawn shape.
void PPVolumetricBeam::Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight)
{
	if (count <= 0)
		return;

	renderstate->PushGroup("volumetricbeam");

	// The depth read must match the depth texture's type: multisampled with
	// MSAA on, so the MS variant (texelFetch on sampler2DMS). Logged on change
	// so a test shows which variant ran.
	const bool multisampled = gl_multisample > 1;
	static int loggedMultisample = -1;
	if (loggedMultisample != (int)multisampled)
	{
		loggedMultisample = (int)multisampled;
		Printf("vol_beam: depth read uses the %s shader variant (gl_multisample %d)\n",
			multisampled ? "MULTISAMPLE" : "single-sample", (int)gl_multisample);
	}

	// Where the 3D view sits inside the depth texture. Read here rather than at
	// scene setup because this is the frame the pass viewport is taken from.
	const FVector2 sceneScale = screen->SceneScale();
	const FVector2 sceneOffset = screen->SceneOffset();
	static FVector2 loggedScale(-1.f, -1.f), loggedOffset(-1.f, -1.f);
	if (sceneScale.X != loggedScale.X || sceneScale.Y != loggedScale.Y ||
		sceneOffset.X != loggedOffset.X || sceneOffset.Y != loggedOffset.Y)
	{
		loggedScale = sceneScale;
		loggedOffset = sceneOffset;
		Printf("vol_beam: depth lookup inside scene viewport, scale (%.3f, %.3f) offset (%.3f, %.3f)\n",
			sceneScale.X, sceneScale.Y, sceneOffset.X, sceneOffset.Y);
	}

	for (int i = 0; i < count; i++)
	{
		if (uniforms[i].Density <= 0.0f || uniforms[i].BeamLength <= 0.0f)
			continue;

		VolumetricBeamUniforms u = uniforms[i];
		u.SceneScale = sceneScale;
		u.SceneOffset = sceneOffset;

		renderstate->Clear();
		renderstate->Shader = multisampled ? &BeamMS : &Beam;
		renderstate->Uniforms.Set(u);
		renderstate->Viewport = screen->mSceneViewport;
		renderstate->SetInputSceneDepth(0);
		renderstate->SetOutputCurrent();
		// Additive: the pass outputs only this beam's own contribution and
		// never reads the scene colour back -- which is exactly why running it
		// once per beam composites them correctly for free.
		renderstate->SetAdditiveBlend();
		renderstate->Draw();
	}

	renderstate->PopGroup();
}

void PPHeatmap::Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight)
{
	if (!active || !haveGrid || uniforms.HeatScale <= 0.0f)
		return;

	renderstate->PushGroup("heatmap");

	// Same depth handling as PPVolumetricBeam::Render: the MS variant when the
	// scene depth is multisampled, and the scene viewport's place in the depth
	// texture. One confirmation line whenever either changes.
	const bool multisampled = gl_multisample > 1;
	HeatmapUniforms u = uniforms;
	u.SceneScale = screen->SceneScale();
	u.SceneOffset = screen->SceneOffset();
	{
		static int loggedMultisample = -1;
		static FVector2 loggedScale(-1.f, -1.f), loggedOffset(-1.f, -1.f);
		if (loggedMultisample != (int)multisampled ||
			u.SceneScale.X != loggedScale.X || u.SceneScale.Y != loggedScale.Y ||
			u.SceneOffset.X != loggedOffset.X || u.SceneOffset.Y != loggedOffset.Y)
		{
			loggedMultisample = (int)multisampled;
			loggedScale = u.SceneScale;
			loggedOffset = u.SceneOffset;
			Printf("heatmap: %s depth read, scene scale (%.3f, %.3f) offset (%.3f, %.3f), projection offset (%.4f, %.4f)\n",
				multisampled ? "MULTISAMPLE" : "single-sample",
				u.SceneScale.X, u.SceneScale.Y, u.SceneOffset.X, u.SceneOffset.Y,
				u.ProjOffset.X, u.ProjOffset.Y);
		}
	}

	renderstate->Clear();
	renderstate->Shader = multisampled ? &HeatMS : &Heat;
	renderstate->Uniforms.Set(u);
	renderstate->Viewport = screen->mSceneViewport;
	renderstate->SetInputSceneDepth(0);
	// LINEAR on the grid, so the cells blend into each other. Nearest would
	// show the lattice, and a heatmap that reads as tiles reads as a debug
	// overlay rather than as something the ground remembers.
	renderstate->SetInputTexture(1, &Intensity, PPFilterMode::Linear);
	renderstate->SetInputTexture(2, &Height, PPFilterMode::Linear);
	renderstate->SetOutputCurrent();
	// Additive, so the pass emits only its own contribution and never has to
	// read the scene colour back.
	renderstate->SetAdditiveBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

// [HEATREFRACTION] Heat shimmer -- see PPHeatRefraction (hw_postprocess.h) and
// shaders/pp/heatoffset.fp, heatwarp.fp.

void PPHeatRefraction::UpdateTexture(int sceneWidth, int sceneHeight)
{
	if (sceneWidth == lastWidth && sceneHeight == lastHeight)
		return;

	OffsetViewport.left = 0;
	OffsetViewport.top = 0;
	OffsetViewport.width = (sceneWidth + 1) / 2;
	OffsetViewport.height = (sceneHeight + 1) / 2;
	// RGBA16F: rg the signed shift, b entry depth x weight, a weight (heatoffset.fp).
	OffsetTexture = { OffsetViewport.width, OffsetViewport.height, PixelFormat::Rgba16f };

	lastWidth = sceneWidth;
	lastHeight = sceneHeight;
}

void PPHeatRefraction::Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight)
{
	// SKIPPED, NOT ZERO STRENGTH: nothing published for this eye means no group, no
	// texture and no draw, so the frame is the frame without this pass. Set 1 only
	// exists for the second eye of a multiview scene.
	const int set = (eyeSets >= 2 && currentEye == 1) ? 1 : 0;
	if (eyeSets <= 0 || counts[set] <= 0 || !r_heatrefraction || sceneWidth <= 0 || sceneHeight <= 0)
		return;

	const bool multisampled = gl_multisample > 1;
	const FVector2 sceneScale = screen->SceneScale();
	const FVector2 sceneOffset = screen->SceneOffset();

	UpdateTexture(sceneWidth, sceneHeight);

	// One line whenever the variant, the eye arrangement or the texture changes, so a
	// test log shows which ran. Never per frame.
	{
		static int loggedMultisample = -1, loggedSets = -1, loggedWidth = -1, loggedHeight = -1;
		if (loggedMultisample != (int)multisampled || loggedSets != eyeSets ||
			loggedWidth != OffsetViewport.width || loggedHeight != OffsetViewport.height)
		{
			loggedMultisample = (int)multisampled;
			loggedSets = eyeSets;
			loggedWidth = OffsetViewport.width;
			loggedHeight = OffsetViewport.height;
			Printf("heat_refraction: %s depth read, %s, offset texture %dx%d\n",
				multisampled ? "MULTISAMPLE" : "single-sample",
				eyeSets >= 2 ? "a source set per eye (multiview scene)" : "one source set (each eye draws its own scene)",
				OffsetViewport.width, OffsetViewport.height);
		}
	}

	// Pass 1: each source adds its shift. The first draws with no blend: every texel of
	// the viewport is written (0 off the source), which clears what the last eye or
	// frame left -- post-process attachments are loaded, never cleared.
	renderstate->PushGroup("pp.heatoffset");
	for (int i = 0; i < counts[set]; i++)
	{
		HeatOffsetUniforms u = sources[set][i];
		u.SceneScale = sceneScale;
		u.SceneOffset = sceneOffset;

		renderstate->Clear();
		renderstate->Shader = multisampled ? &OffsetShaderMS : &OffsetShader;
		renderstate->Uniforms.Set(u);
		renderstate->Viewport = OffsetViewport;
		renderstate->SetInputSceneDepth(0);
		renderstate->SetOutputTexture(&OffsetTexture);
		if (i == 0)
			renderstate->SetNoBlend();
		else
			renderstate->SetAdditiveBlend();
		renderstate->Draw();
	}
	renderstate->PopGroup();

	// Pass 2: bend the image. Over the whole screen viewport, as the lens pass, because
	// the next pipeline image starts undefined.
	HeatWarpUniforms w = {};
	w.SceneScale = sceneScale;
	w.SceneOffset = sceneOffset;
	w.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	w.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);
	w.MaxShift = MAX_SHIFT;
	w.DepthMargin = DEPTH_MARGIN;

	renderstate->PushGroup("pp.heatwarp");
	renderstate->Clear();
	renderstate->Shader = multisampled ? &WarpShaderMS : &WarpShader;
	renderstate->Uniforms.Set(w);
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetInputTexture(1, &OffsetTexture, PPFilterMode::Linear);
	renderstate->SetInputSceneDepth(2);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();
	renderstate->PopGroup();

	// [LIGHTMASK] The light mask bends with the image (hw_postprocess.h, PPLightMask): the same
	// shader, uniforms, offsets and depth tests on the mask instead of the colour, into the other
	// image of the mask's pair. Every tap of the warp is a linear read of a linear quantity, so
	// each pixel's amounts come from exactly where its colour came from. Both eyes of a layered
	// post path take this branch alike (the frame's state), and it moves no pipeline image.
	if (hw_postprocess.lightmask.PostInputValid())
	{
		renderstate->PushGroup("pp.lightmaskcarry");
		renderstate->Clear();
		renderstate->Shader = multisampled ? &WarpShaderMS : &WarpShader;
		renderstate->Uniforms.Set(w);
		renderstate->Viewport = screen->mScreenViewport;
		renderstate->SetInputLightMask(0, PPFilterMode::Linear);
		renderstate->SetInputTexture(1, &OffsetTexture, PPFilterMode::Linear);
		renderstate->SetInputSceneDepth(2);
		renderstate->SetOutputLightMaskNext();
		renderstate->SetNoBlend();
		renderstate->Draw();
		renderstate->PopGroup();
	}
}

/////////////////////////////////////////////////////////////////////////////

// [SMOKEVOLUME] The backend-owned images a pass may read (hw_postprocess.h, PPExternalImage): one
// fixed token per name, plus one past the last for a name that is out of range. That last token
// resolves to no image, so a draw naming it draws nothing.

static PPTexture *ExternalImageTokens()
{
	static PPTexture tokens[(int)PPExternalImage::Count + 1];
	return tokens;
}

PPTexture *PPExternalImageToken(PPExternalImage image)
{
	int index = (int)image;
	if (index < 0 || index > (int)PPExternalImage::Count)
		index = (int)PPExternalImage::Count;
	return &ExternalImageTokens()[index];
}

PPExternalImage PPExternalImageFromToken(const PPTexture *token)
{
	const PPTexture *tokens = ExternalImageTokens();
	for (int i = 0; i < (int)PPExternalImage::Count; i++)
	{
		if (token == &tokens[i])
			return (PPExternalImage)i;
	}
	return PPExternalImage::Count;
}

/////////////////////////////////////////////////////////////////////////////

// [SMOKEVOLUME] The smoke volume's drawing: see PPSmokeVolume (hw_postprocess.h) and
// shaders/pp/smokedepth.fp, smokemarch.fp, smokeblur.fp, smokecomposite.fp.

void PPSmokeVolume::UpdateTextures(int sceneWidth, int sceneHeight)
{
	if (sceneWidth == lastWidth && sceneHeight == lastHeight)
		return;

	HalfViewport.left = 0;
	HalfViewport.top = 0;
	HalfViewport.width = (sceneWidth + 1) / 2;
	HalfViewport.height = (sceneHeight + 1) / 2;
	// R32F: the linear depth each texel marches to. RGBA16F: rgb light, a transmittance.
	DepthTexture = { HalfViewport.width, HalfViewport.height, PixelFormat::R32f };
	MarchTexture = { HalfViewport.width, HalfViewport.height, PixelFormat::Rgba16f };
	BlurTexture = { HalfViewport.width, HalfViewport.height, PixelFormat::Rgba16f };

	lastWidth = sceneWidth;
	lastHeight = sceneHeight;
}

void PPSmokeVolume::Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight)
{
	// SKIPPED, NOT ZERO: nothing published for this eye means no group, no texture and no draw, so
	// the frame is the frame without this pass. Set 1 only exists for the second eye of a multiview
	// scene.
	const int set = (eyeSets >= 2 && currentEye == 1) ? 1 : 0;
	if (eyeSets <= 0 || !r_smoke || sceneWidth <= 0 || sceneHeight <= 0)
		return;

	const bool multisampled = gl_multisample > 1;
	UpdateTextures(sceneWidth, sceneHeight);

	// One line whenever the variant, the eye arrangement or the textures change, so a test log shows
	// which ran. Never one per frame.
	{
		static int loggedMultisample = -1, loggedSets = -1, loggedWidth = -1, loggedHeight = -1;
		if (loggedMultisample != (int)multisampled || loggedSets != eyeSets ||
			loggedWidth != HalfViewport.width || loggedHeight != HalfViewport.height)
		{
			loggedMultisample = (int)multisampled;
			loggedSets = eyeSets;
			loggedWidth = HalfViewport.width;
			loggedHeight = HalfViewport.height;
			Printf("smoke: drawing -- %s depth read, %s, half-resolution textures %dx%d\n",
				multisampled ? "MULTISAMPLE" : "single-sample",
				eyeSets >= 2 ? "a march set per eye (multiview scene)" : "one march set (each eye draws its own scene)",
				HalfViewport.width, HalfViewport.height);
		}
	}

	SmokeDepthUniforms depth = {};
	depth.SceneScale = screen->SceneScale();
	depth.SceneOffset = screen->SceneOffset();
	depth.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	depth.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);

	renderstate->PushGroup("pp.smoke");

	// 1. The depth to march to, at half resolution. Every pass below draws the whole viewport of its
	//    target with no blend, which clears what the last eye or frame left there (post-process
	//    attachments are loaded, never cleared).
	renderstate->Clear();
	renderstate->Shader = multisampled ? &DepthShaderMS : &DepthShader;
	renderstate->Uniforms.Set(depth);
	renderstate->Viewport = HalfViewport;
	renderstate->SetInputSceneDepth(0);
	renderstate->SetOutputTexture(&DepthTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// 2. The march, reading the volume the compute step keeps (PPExternalImage).
	renderstate->Clear();
	renderstate->Shader = &MarchShader;
	renderstate->Uniforms.Set(marches[set]);
	renderstate->Viewport = HalfViewport;
	renderstate->SetInputTexture(0, &DepthTexture);
	renderstate->SetInputExternalImage(1, PPExternalImage::SmokeDensityLatest, PPFilterMode::Linear);
	renderstate->SetInputExternalImage(2, PPExternalImage::SmokeDensityPrevious, PPFilterMode::Linear);
	renderstate->SetInputExternalImage(3, PPExternalImage::SmokeTileActive);
	// [13d] The light grid the compute step filled this frame (VkSmokeVolume), filtered like the density.
	renderstate->SetInputExternalImage(4, PPExternalImage::SmokeLight, PPFilterMode::Linear);
	renderstate->SetInputExternalImage(5, PPExternalImage::SmokeLightDirection, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&MarchTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// 3. The blur that keeps to its depth: across into BlurTexture, then down back into MarchTexture.
	for (int pass = 0; pass < 2; pass++)
	{
		renderstate->Clear();
		renderstate->Shader = pass == 0 ? &BlurHorizontal : &BlurVertical;
		renderstate->Viewport = HalfViewport;
		renderstate->SetInputTexture(0, pass == 0 ? &MarchTexture : &BlurTexture);
		renderstate->SetInputTexture(1, &DepthTexture);
		renderstate->SetOutputTexture(pass == 0 ? &BlurTexture : &MarchTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// 4. Up to full resolution and onto the image: scene x T + light, premultiplied. It writes the
	//    current pipeline image in place and reads it nowhere, so no pipeline image advances.
	renderstate->Clear();
	renderstate->Shader = multisampled ? &CompositeShaderMS : &CompositeShader;
	renderstate->Uniforms.Set(depth);
	renderstate->Viewport = screen->mSceneViewport;
	renderstate->SetInputTexture(0, &MarchTexture);
	renderstate->SetInputTexture(1, &DepthTexture);
	renderstate->SetInputSceneDepth(2);
	renderstate->SetOutputCurrent();
	renderstate->SetPremultipliedAlphaBlend();
	renderstate->Draw();

	renderstate->PopGroup();

	// [LIGHTMASK] The light mask is dimmed by the same smoke ("Engine docs/EMISSIVE_BLOOM_PLAN.md" 2e,
	// contract 4): the same composite with LIGHT_MASK_CARRY, the same inputs, uniforms and blend, onto
	// the mask in place. It adds no light of either class, so every amount becomes amount x T with the
	// colour's own T. Both eyes of a layered post path take this branch alike (the frame's state).
	if (hw_postprocess.lightmask.PostInputValid())
	{
		renderstate->PushGroup("pp.lightmaskcarry");
		renderstate->Clear();
		renderstate->Shader = multisampled ? &MaskCarryShaderMS : &MaskCarryShader;
		renderstate->Uniforms.Set(depth);
		renderstate->Viewport = screen->mSceneViewport;
		renderstate->SetInputTexture(0, &MarchTexture);
		renderstate->SetInputTexture(1, &DepthTexture);
		renderstate->SetInputSceneDepth(2);
		renderstate->SetOutputLightMaskCurrent();
		renderstate->SetPremultipliedAlphaBlend();
		renderstate->Draw();
		renderstate->PopGroup();
	}
}

//==========================================================================
//
// [BLOOMOVERRIDE] The script-set bloom override, renderer side (E1 in
// "Engine docs/REVIEW_BLOOM_PLAN.md"; the level slot and natives are
// FLevelLocals::BloomOverride* and LevelLocals.SetBloomOverride).
//
// The level copy (SyncBloomOverride, hw_drawinfo.cpp) hands the slot over once per
// scene eye with the frame clock. Everything below is a pure function of that state
// and screen->FrameTime, which is set once per displayed frame before the eye loop,
// so both eyes of a frame get the same values and nothing advances per eye (layered
// post shares pipeline images, so both eyes must run identical passes).
//
// FADE. Script sets a look at 35 Hz and the headset draws at 90 or more. A new look
// eases from whatever is on screen now to its values over its fade; a clear eases
// the mix back to 0 over the last look's fade. Fade 0 snaps. A repeat of the current
// look changes nothing, so a caller may push every tic.
//
//==========================================================================

void PPBloom::SetOverride(const PPBloomOverride &target, uint64_t now)
{
	if (OverrideLive && OverrideTargetActive && target.SameLook(OverrideTo))
	{
		// The same look again. The glow pulse's inputs are live values, not a look:
		// they follow the level every frame and never restart the fade.
		OverrideTo.GlowPulseLevel = target.GlowPulseLevel;
		OverrideTo.GlowPulseRate = target.GlowPulseRate;
		return;
	}

	PPBloomOverride current;
	if (OverrideLive)
	{
		EvaluateOverride(now, current);
	}
	else
	{
		// From nothing: every value starts where it is going, at zero weight, so only
		// the mix eases in.
		current = target;
		current.Mix = 0.0f;
	}

	OverrideFrom = current;
	OverrideTo = target;
	OverrideStartMs = now;
	OverrideFadeSeconds = target.Fade;
	OverrideTargetActive = true;
	OverrideLive = true;
}

void PPBloom::ClearOverride(uint64_t now)
{
	if (!OverrideTargetActive)
		return;   // nothing set, or a clear already easing out

	PPBloomOverride current;
	EvaluateOverride(now, current);
	OverrideFrom = current;
	OverrideTo = current;
	OverrideTo.Mix = 0.0f;
	OverrideStartMs = now;
	OverrideFadeSeconds = current.Fade;
	OverrideTargetActive = false;
	OverrideLive = OverrideFadeSeconds > 0.0f;
}

float PPBloom::EvaluateOverride(uint64_t now, PPBloomOverride &out) const
{
	if (!OverrideLive)
	{
		out = PPBloomOverride();
		return 0.0f;
	}

	// The clock never runs backwards within a session; if it ever did, the ease counts as done.
	float t = 1.0f;
	if (OverrideFadeSeconds > 0.0f && now >= OverrideStartMs)
	{
		const double elapsed = (double)(now - OverrideStartMs) / 1000.0;
		t = (float)min(elapsed / (double)OverrideFadeSeconds, 1.0);
	}

	out = OverrideTo;
	if (t < 1.0f)
	{
		auto ease = [t](float from, float to) { return from + (to - from) * t; };
		out.Spread = ease(OverrideFrom.Spread, OverrideTo.Spread);
		out.Threshold = ease(OverrideFrom.Threshold, OverrideTo.Threshold);
		out.Knee = ease(OverrideFrom.Knee, OverrideTo.Knee);
		out.TintR = ease(OverrideFrom.TintR, OverrideTo.TintR);
		out.TintG = ease(OverrideFrom.TintG, OverrideTo.TintG);
		out.TintB = ease(OverrideFrom.TintB, OverrideTo.TintB);
		out.Mix = ease(OverrideFrom.Mix, OverrideTo.Mix);
		out.Intensity = ease(OverrideFrom.Intensity, OverrideTo.Intensity);
		out.Pulse = ease(OverrideFrom.Pulse, OverrideTo.Pulse);
		// PulseRate is not eased: a rate change moves the phase (timer x rate) at once,
		// in the glow shader too, so easing it would only drift away from the glows.
	}
	return out.Mix;
}

// [BLOOMOVERRIDE] The pulse: a multiplier on the override's intensity,
// 1 + pulse * (beat - 0.5) * 2.
//
// pulseRate 0 beats with the glow alarm pulse (main.fp section 5, "THE ROOM KNOWS
// SOMETHING IS WRONG"), in the same float32 operations and operand order, from the
// same inputs and the same clock:
//   timer = (float)((double)(FrameTime - firstFrame) / 1000.) -- the scene shaders'
//     `timer` for a speed-1 material (VkRenderState::ApplyStreamData; `x * 1.0` is
//     exact), as SyncDrawnLines already copies it for the drawn beams.
//   lvl = clamp(GlowPulseLevel, 0, 1); rate = (1 + 6 * lvl) * max(GlowPulseRate, 0);
//   beat = 0.5 + 0.5 * sin(timer * rate * 6.2831853 * 0.35)
// This file is not built with fast math (FASTMATH_SOURCES, src/CMakeLists.txt), so
// the argument is the shader's to the bit unless the GPU driver reassociates it; the
// two sin() implementations differ by about 1e-7. pulseRate > 0 is in beats per second.
// The caller folds the alarm level into `pulse` itself; it is not applied again here.
float PPBloom::OverridePulseFactor(const PPBloomOverride &look)
{
	if (look.Pulse <= 0.0f)
		return 1.0f;

	FRenderState *sceneState = screen->RenderState();
	const uint64_t firstFrame = sceneState != nullptr ? sceneState->firstFrame : 0;
	const float timer = static_cast<float>((double)(screen->FrameTime - firstFrame) / 1000.);

	float beat;
	if (look.PulseRate > 0.0f)
	{
		beat = 0.5f + 0.5f * std::sin(timer * look.PulseRate * 6.2831853f);
	}
	else
	{
		const float lvl = look.GlowPulseLevel < 0.0f ? 0.0f : (look.GlowPulseLevel > 1.0f ? 1.0f : look.GlowPulseLevel);
		const float rate = (1.0f + 6.0f * lvl) * (look.GlowPulseRate > 0.0f ? look.GlowPulseRate : 0.0f);
		beat = 0.5f + 0.5f * std::sin(timer * rate * 6.2831853f * 0.35f);
	}
	return 1.0f + look.Pulse * (beat - 0.5f) * 2.0f;
}

// [BLOOMOVERRIDE] Moves this frame's cvar values toward the override by mixAmount.
// Intensity (with its pulse) multiplies the tint of the final combine, which is the
// only place bloom's brightness can be raised: spread only widens.
void PPBloom::BlendOverride(const PPBloomOverride &look, float mixAmount, float &threshold, float &knee, float &amount, FVector3 &tint)
{
	auto toward = [mixAmount](float from, float to) { return from + (to - from) * mixAmount; };
	threshold = toward(threshold, look.Threshold);
	knee = toward(knee, look.Knee);
	const float spread = toward(amount, look.Spread);
	amount = spread > 0.1f ? spread : 0.1f;   // gl_bloom_amount's floor: the blur's theta must stay > 0
	const float intensity = toward(1.0f, look.Intensity * OverridePulseFactor(look));
	tint = FVector3(toward(tint.X, look.TintR) * intensity,
		toward(tint.Y, look.TintG) * intensity,
		toward(tint.Z, look.TintB) * intensity);
}

// [BLOOMOVERRIDE] gl_bloom_override_strength as a 0..1 weight (a NaN from the console reads 0).
static float BloomOverrideStrength()
{
	const float strength = gl_bloom_override_strength;
	return strength >= 0.0f ? (strength < 1.0f ? strength : 1.0f) : 0.0f;
}

//==========================================================================
//
// [PINNEDBLOOM] PINNED BLOOM FOR BEAM LIGHT ("Engine docs/EMISSIVE_BLOOM_PLAN.md" E6: the
// player's "Keep legacy lasers", gl_bloom_pin_beams).
//
// Bloom has a LOOK (PPBloomLook): threshold, knee, exposure, spread, streaks, step, tint and
// fringing. The REST look is the one bloom always used -- the gl_bloom_* / gl_exposure_* cvars,
// moved by any SetBloomOverride (E1), the knee capped (E2), the blur stepped (E3). The PINNED look
// is the gl_bloom_pin_* cvars alone. While pinned bloom is on and the light mask carries this
// eye's scene, each pixel's share of pinned light (beam light) blooms with the pinned look and the
// rest of it with the rest look, so presets and overrides change the room and not the beams
// (PPBloomPlan). Every input is the frame's -- the mask's snapshot and report, the frame clock, the
// cvars -- so both eyes, and both layers of a layered post path, run the same passes.
//
//==========================================================================

// Two looks are the same look when they send the same bytes.
static bool SameBits(float a, float b)
{
	return memcmp(&a, &b, sizeof(float)) == 0;
}

// The rest look: today's values, through today's operations in today's order.
void PPBloom::RestLook(PPBloomLook &look) const
{
	look.Threshold = gl_bloom_threshold;
	look.Knee = gl_bloom_knee;
	look.Amount = gl_bloom_amount;
	look.Tint = FVector3(gl_bloom_tint_r, gl_bloom_tint_g, gl_bloom_tint_b);

	// [BLOOMOVERRIDE] E1. A script-set look (SetBloomOverride) moves the values above
	// toward its own by its eased mix times gl_bloom_override_strength. When that is 0 --
	// nothing set, a clear that has finished fading, strength 0 -- this is skipped and
	// every value is today's, read straight from the cvars: a branch, not a blend by zero.
	// gl_bloom off returns in RenderBloom first, so an override never switches bloom on.
	// [PINNEDBLOOM] It never reaches the pinned look.
	PPBloomOverride overrideLook;
	const float overrideMix = EvaluateOverride(screen->FrameTime, overrideLook) * BloomOverrideStrength();
	if (overrideMix > 0.0f)
		BlendOverride(overrideLook, overrideMix, look.Threshold, look.Knee, look.Amount, look.Tint);

	// [BLOOMSAFETY] E2. A knee wider than the threshold makes every dark pixel emit a grey
	// (knee - threshold)^2 / (4 knee) that the blur spreads over the whole screen: a haze,
	// not a look (owner's call, 2026-09-14). Capped here, after any override, not in
	// bloomextract.fp: any knee at or under the threshold (the defaults are 0.5 and 1.0)
	// sends the very same bits, and neither the lump nor the archived cvar changes.
	look.Knee = min(look.Knee, look.Threshold);

	look.Anamorphic = gl_bloom_anamorphic;
	look.AnamorphicRatio = gl_bloom_anamorphic_ratio;
	look.Step = gl_bloom_step;
	look.Chromatic = gl_bloom_chromatic;

	const ExposureCombineUniforms exposure = PPCameraExposure::CombineUniforms(gl_exposure_base, gl_exposure_min, gl_exposure_scale, gl_exposure_speed);
	look.ExposureBase = exposure.ExposureBase;
	look.ExposureMin = exposure.ExposureMin;
	look.ExposureScale = exposure.ExposureScale;
	look.ExposureSpeed = exposure.ExposureSpeed;
}

// The pinned look: the gl_bloom_pin_* cvars through the same knee cap and exposure guards, and no
// override. The cvars mirror the live ones' clamps, so a captured look is the rest look's bits.
void PPBloom::PinnedLook(PPBloomLook &look)
{
	look.Threshold = gl_bloom_pin_threshold;
	look.Knee = gl_bloom_pin_knee;
	look.Amount = gl_bloom_pin_amount;
	look.Tint = FVector3(gl_bloom_pin_tint_r, gl_bloom_pin_tint_g, gl_bloom_pin_tint_b);
	look.Knee = min(look.Knee, look.Threshold);

	look.Anamorphic = gl_bloom_pin_anamorphic;
	look.AnamorphicRatio = gl_bloom_pin_anamorphic_ratio;
	look.Step = gl_bloom_pin_step;
	look.Chromatic = gl_bloom_pin_chromatic;

	const ExposureCombineUniforms exposure = PPCameraExposure::CombineUniforms(gl_bloom_pin_exposure_base, gl_bloom_pin_exposure_min,
		gl_bloom_pin_exposure_scale, gl_bloom_pin_exposure_speed);
	look.ExposureBase = exposure.ExposureBase;
	look.ExposureMin = exposure.ExposureMin;
	look.ExposureScale = exposure.ExposureScale;
	look.ExposureSpeed = exposure.ExposureSpeed;
}

// The blur a look asks for, computed as RenderBloom always computed it.
void PPBloom::ComputeChain(const PPBloomLook &look, PPBloomChain &chain)
{
	// [BB] Anamorphic. The blur is already two passes -- one horizontal, one
	// vertical -- so widening only the horizontal one costs nothing and gives
	// the sideways streak of an anamorphic lens.
	float hAmount = look.Amount;
	float vAmount = look.Amount;
	if (look.Anamorphic) hAmount = look.Amount * look.AnamorphicRatio;

	// [BLOOMSTEP] E3. gl_bloom_step 1 (or a NaN) is today's blur: the same programs,
	// uniforms and Nearest reads. Above 1 the wide programs spread the taps: vertical by
	// the step, horizontal by the step times the anamorphic ratio. The amounts stay the
	// Gaussian's width in texels, so a large amount becomes a real Gaussian instead of a
	// flat seven-texel box (ComputeBlurSamplesStepped). The ratio's share of the
	// horizontal step phases in over the first unit of step: taking it all at 1.01 would
	// make an anamorphic streak jump to about twice its length on a slider.
	const float stepSize = look.Step;
	chain.Stepped = stepSize > 1.0f;
	chain.HorizontalWide = {};
	chain.VerticalWide = {};
	if (!chain.Stepped)
	{
		ComputeBlurSamples(7, hAmount, chain.Horizontal.SampleWeights);
		ComputeBlurSamples(7, vAmount, chain.Vertical.SampleWeights);
	}
	else
	{
		const float ratio = look.Anamorphic ? look.AnamorphicRatio : 1.0f;
		const float phaseIn = min(stepSize - 1.0f, 1.0f);
		ComputeBlurSamplesStepped(hAmount, stepSize * (1.0f + (ratio - 1.0f) * phaseIn), chain.HorizontalWide);
		ComputeBlurSamplesStepped(vAmount, stepSize, chain.VerticalWide);
	}
}

// Whether two chains run the very same blur passes.
bool PPBloom::SameChain(const PPBloomChain &a, const PPBloomChain &b)
{
	if (a.Stepped != b.Stepped)
		return false;
	if (!a.Stepped)	// the seven weights today's blur reads; the eighth is never set, as today
		return memcmp(a.Horizontal.SampleWeights, b.Horizontal.SampleWeights, 7 * sizeof(float)) == 0 &&
			memcmp(a.Vertical.SampleWeights, b.Vertical.SampleWeights, 7 * sizeof(float)) == 0;
	return memcmp(&a.HorizontalWide, &b.HorizontalWide, sizeof(BlurSteppedUniforms)) == 0 &&
		memcmp(&a.VerticalWide, &b.VerticalWide, sizeof(BlurSteppedUniforms)) == 0;
}

// This eye's plan. Also fills the pinned look and chain (whenever pinned bloom is on for this eye)
// and says whether the pinned exposure differs from the live one -- which RenderBloom keeps running
// with a laser on screen or not.
PPBloomPlan PPBloom::ChoosePlan(const PPBloomLook &rest, const PPBloomChain &restChain, PPBloomLook &pinned, PPBloomChain &pinnedChain, bool &pinnedExposure) const
{
	pinnedExposure = false;

	// Pinned bloom off this frame, or this eye's scene drew no mask (a save picture, OpenGL): today's.
	const PPLightMask &mask = hw_postprocess.lightmask;
	if (!mask.PinnedBloomOn() || !mask.PostInputValid())
		return PPBloomPlan::Legacy;

	PinnedLook(pinned);
	ComputeChain(pinned, pinnedChain);
	pinnedExposure = !(SameBits(rest.ExposureBase, pinned.ExposureBase) && SameBits(rest.ExposureMin, pinned.ExposureMin) &&
		SameBits(rest.ExposureScale, pinned.ExposureScale) && SameBits(rest.ExposureSpeed, pinned.ExposureSpeed));

	// No beam can be on screen: every pinned share is 0, and today's bloom is the answer.
	if (!mask.PinnedLightMayBeLive())
		return PPBloomPlan::Legacy;

	if (!SameChain(restChain, pinnedChain) || !SameBits(rest.Chromatic, pinned.Chromatic))
		return PPBloomPlan::TwoChains;

	const bool sameBeforeBlur = !pinnedExposure && SameBits(rest.Threshold, pinned.Threshold) && SameBits(rest.Knee, pinned.Knee) &&
		SameBits(rest.Tint.X, pinned.Tint.X) && SameBits(rest.Tint.Y, pinned.Tint.Y) && SameBits(rest.Tint.Z, pinned.Tint.Z);
	return sameBeforeBlur ? PPBloomPlan::Legacy : PPBloomPlan::OneChain;
}

void PPBloom::RenderBloom(PPRenderState *renderstate, int sceneWidth, int sceneHeight, int fixedcm)
{
	// Only bloom things if enabled and no special fixed light mode is active
	if (!gl_bloom || fixedcm != CM_DEFAULT || gl_ssao_debug || sceneWidth <= 0 || sceneHeight <= 0)
	{
		return;
	}

	// [PINNEDBLOOM] This eye's looks and plan (above). Legacy is the bloom this function always drew.
	PPBloomLook restLook;
	RestLook(restLook);
	PPBloomChain restChain;
	ComputeChain(restLook, restChain);

	PPBloomLook pinnedLook;
	PPBloomChain pinnedChain;
	bool pinnedExposure = false;
	const PPBloomPlan plan = ChoosePlan(restLook, restChain, pinnedLook, pinnedChain, pinnedExposure);
	Plan = plan;

	// One line the first time each pinned plan runs in a session, and once if the switch is on with a
	// look never chosen (an ini saved before the capture existed), so a test log says what ran.
	if (hw_postprocess.lightmask.PinnedBloomOn() && hw_postprocess.lightmask.PostInputValid())
	{
		static bool loggedUncaptured = false, loggedOneChain = false, loggedTwoChains = false;
		if (!gl_bloom_pin_captured && !loggedUncaptured)
		{
			loggedUncaptured = true;
			Printf("Bloom: gl_bloom_pin_beams is on but no laser look was ever captured -- beam light keeps the gl_bloom_pin_* "
				"values as they are (the engine defaults unless set); gl_bloom_pin_capture takes the current bloom\n");
		}
		if (plan == PPBloomPlan::OneChain && !loggedOneChain)
		{
			loggedOneChain = true;
			Printf("Bloom: pinned laser bloom -- one chain (the laser look differs only in threshold, knee, exposure or tint)\n");
		}
		if (plan == PPBloomPlan::TwoChains && !loggedTwoChains)
		{
			loggedTwoChains = true;
			Printf("Bloom: pinned laser bloom -- a second bloom chain for beam light (the laser look differs in spread, streaks, step or fringing)\n");
		}
	}

	// [PINNEDBLOOM] The pinned look's exposure, whenever it differs from the live one -- a beam on
	// screen or not, so a laser that appears under a preset meets an exposure that has adapted.
	if (pinnedExposure)
	{
		ExposureCombineUniforms pinnedCombine;
		pinnedCombine.ExposureBase = pinnedLook.ExposureBase;
		pinnedCombine.ExposureMin = pinnedLook.ExposureMin;
		pinnedCombine.ExposureScale = pinnedLook.ExposureScale;
		pinnedCombine.ExposureSpeed = pinnedLook.ExposureSpeed;
		hw_postprocess.exposure.RenderPinned(renderstate, pinnedCombine);
	}
	PPTexture *pinnedExposureTexture = pinnedExposure ? &hw_postprocess.exposure.PinnedCameraTexture : &hw_postprocess.exposure.CameraTexture;

	renderstate->PushGroup("bloom");

	UpdateTextures(sceneWidth, sceneHeight);

	auto &level0 = levels[0];

	if (plan == PPBloomPlan::Legacy)
	{
		ExtractUniforms extractUniforms;
		extractUniforms.Scale = screen->SceneScale();
		extractUniforms.Offset = screen->SceneOffset();
		extractUniforms.Threshold = restLook.Threshold;
		extractUniforms.Knee = restLook.Knee;

		// Extract blooming pixels from scene texture:
		renderstate->Clear();
		renderstate->Shader = &BloomExtract;
		renderstate->Uniforms.Set(extractUniforms);
		renderstate->Viewport = level0.Viewport;
		renderstate->SetInputCurrent(0, PPFilterMode::Linear);
		renderstate->SetInputTexture(1, &hw_postprocess.exposure.CameraTexture);
		renderstate->SetOutputTexture(&level0.VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();

		RenderChain(renderstate, levels, restChain);
		RenderFinalCombine(renderstate, level0, restLook.Tint, restLook.Chromatic);
	}
	else if (plan == PPBloomPlan::OneChain)
	{
		// [PINNEDBLOOM] Both looks in one extract, each with its own threshold, knee, exposure and tint,
		// weighed by the pinned share. The looks share this chain (the plan's condition), and the tints
		// are in the extract, so the final combine adds a neutral tint with the shared fringing.
		ExtractDualUniforms dual = {};
		dual.Scale = screen->SceneScale();
		dual.Offset = screen->SceneOffset();
		dual.RestThreshold = restLook.Threshold;
		dual.RestKnee = restLook.Knee;
		dual.PinThreshold = pinnedLook.Threshold;
		dual.PinKnee = pinnedLook.Knee;
		dual.RestTint = restLook.Tint;
		dual.PinTint = pinnedLook.Tint;

		renderstate->Clear();
		renderstate->Shader = &BloomExtractDual;
		renderstate->Uniforms.Set(dual);
		renderstate->Viewport = level0.Viewport;
		renderstate->SetInputCurrent(0, PPFilterMode::Linear);
		renderstate->SetInputTexture(1, &hw_postprocess.exposure.CameraTexture);
		renderstate->SetInputTexture(2, pinnedExposureTexture);
		renderstate->SetInputLightMask(3, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&level0.VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();

		RenderChain(renderstate, levels, restChain);
		RenderFinalCombine(renderstate, level0, FVector3(1.0f, 1.0f, 1.0f), restLook.Chromatic);
	}
	else
	{
		// [PINNEDBLOOM] Two chains. Both extracts read the image before either combine adds to it:
		// the rest share's extract and chain, then the pinned share's extract, chain and combine
		// (group bloom.pinned), then the rest combine. The perf log sums same-name groups.
		RenderExtractShare(renderstate, level0, restLook, 0, &hw_postprocess.exposure.CameraTexture);
		RenderChain(renderstate, levels, restChain);
		renderstate->PopGroup();

		renderstate->PushGroup("bloom.pinned");
		UpdatePinnedTextures();
		RenderExtractShare(renderstate, pinLevels[0], pinnedLook, 1, pinnedExposureTexture);
		RenderChain(renderstate, pinLevels, pinnedChain);
		RenderFinalCombine(renderstate, pinLevels[0], pinnedLook.Tint, pinnedLook.Chromatic);
		renderstate->PopGroup();

		renderstate->PushGroup("bloom");
		RenderFinalCombine(renderstate, level0, restLook.Tint, restLook.Chromatic);
	}

	renderstate->PopGroup();
}

// [PINNEDBLOOM] One look's extract times one share of each pixel (bloomextract.fp BLOOM_EXTRACT_SHARE):
// weightClass 1 the pinned share, 0 the rest.
void PPBloom::RenderExtractShare(PPRenderState *renderstate, PPBlurLevel &level0, const PPBloomLook &look, int weightClass, PPTexture *exposureTexture)
{
	ExtractShareUniforms share = {};
	share.Scale = screen->SceneScale();
	share.Offset = screen->SceneOffset();
	share.Threshold = look.Threshold;
	share.Knee = look.Knee;
	share.WeightClass = weightClass;

	renderstate->Clear();
	renderstate->Shader = &BloomExtractShare;
	renderstate->Uniforms.Set(share);
	renderstate->Viewport = level0.Viewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetInputTexture(1, exposureTexture);
	renderstate->SetInputLightMask(2, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&level0.VTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();
}

// The blur chain over one set of levels, after an extract filled level 0: RenderBloom's passes as they
// always were, over `levels` or, for the pinned share, `pinLevels`.
void PPBloom::RenderChain(PPRenderState *renderstate, PPBlurLevel *chainLevels, const PPBloomChain &chain)
{
	// One level's blur, horizontal (V -> H) then vertical (H -> V), today's or wide.
	auto blurLevel = [&](PPBlurLevel &blevel)
	{
		if (!chain.Stepped)
		{
			BlurStep(renderstate, chain.Horizontal, blevel.VTexture, blevel.HTexture, blevel.Viewport, false);
			BlurStep(renderstate, chain.Vertical, blevel.HTexture, blevel.VTexture, blevel.Viewport, true);
		}
		else
		{
			BlurStepWide(renderstate, chain.HorizontalWide, blevel.VTexture, blevel.HTexture, blevel.Viewport, false);
			BlurStepWide(renderstate, chain.VerticalWide, blevel.HTexture, blevel.VTexture, blevel.Viewport, true);
		}
	};

	// Neutral for the downscale steps, which share this shader.
	BloomCombineUniforms plainCombine;
	plainCombine.Tint = FVector3(1.0f, 1.0f, 1.0f);
	plainCombine.Chromatic = 0.0f;

	// Blur and downscale:
	for (int i = 0; i < NumBloomLevels - 1; i++)
	{
		auto &blevel = chainLevels[i];
		auto &next = chainLevels[i + 1];

		blurLevel(blevel);

		// Linear downscale:
		renderstate->Clear();
		renderstate->Shader = &BloomCombine;
		renderstate->Uniforms.Set(plainCombine);
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.VTexture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// Blur and upscale:
	for (int i = NumBloomLevels - 1; i > 0; i--)
	{
		auto &blevel = chainLevels[i];
		auto &next = chainLevels[i - 1];

		blurLevel(blevel);

		// Linear upscale:
		renderstate->Clear();
		renderstate->Shader = &BloomCombine;
		renderstate->Uniforms.Set(plainCombine);
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.VTexture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	blurLevel(chainLevels[0]);
}

// Add bloom back to scene texture. This is the ONLY place tint and
// fringing apply -- every earlier use of this shader was a downscale.
// [BLOOMOVERRIDE] The rest look's tint carries any override. [PINNEDBLOOM] OneChain passes a
// neutral tint (its tints are in the extract); TwoChains adds each share's bloom with its own look.
void PPBloom::RenderFinalCombine(PPRenderState *renderstate, PPBlurLevel &level0, const FVector3 &tint, float chromatic)
{
	BloomCombineUniforms finalCombine;
	finalCombine.Tint = tint;
	finalCombine.Chromatic = chromatic;

	renderstate->Clear();
	renderstate->Shader = &BloomCombine;
	renderstate->Uniforms.Set(finalCombine);
	renderstate->Viewport = screen->mSceneViewport;
	renderstate->SetInputTexture(0, &level0.VTexture, PPFilterMode::Linear);
	renderstate->SetOutputCurrent();
	renderstate->SetAdditiveBlend();
	renderstate->Draw();
}

// [PINNEDBLOOM] The pinned chain's levels: the sizes `levels` has this frame (UpdateTextures ran first).
void PPBloom::UpdatePinnedTextures()
{
	if (pinLastWidth == lastWidth && pinLastHeight == lastHeight)
		return;

	for (int i = 0; i < NumBloomLevels; i++)
	{
		auto &pinned = pinLevels[i];
		pinned.Viewport = levels[i].Viewport;
		pinned.VTexture = { pinned.Viewport.width, pinned.Viewport.height, PixelFormat::Rgba16f };
		pinned.HTexture = { pinned.Viewport.width, pinned.Viewport.height, PixelFormat::Rgba16f };
	}

	pinLastWidth = lastWidth;
	pinLastHeight = lastHeight;
}

void PPBloom::RenderBlur(PPRenderState *renderstate, int sceneWidth, int sceneHeight, float gameinfobluramount)
{
	// No scene, no blur!
	if (sceneWidth <= 0 || sceneHeight <= 0)
		return;

	UpdateTextures(sceneWidth, sceneHeight);

	// first, respect the CVar
	float blurAmount = gl_menu_blur;

	// if CVar is negative, use the gameinfo entry
	if (gl_menu_blur < 0)
		blurAmount = gameinfobluramount;

	// if blurAmount == 0 or somehow still returns negative, exit to prevent a crash, clearly we don't want this
	if (blurAmount <= 0.0)
	{
		return;
	}

	renderstate->PushGroup("blur");

	int numLevels = 3;
	assert(numLevels <= NumBloomLevels);

	auto &level0 = levels[0];

	// Shares BloomCombine, so it needs the same neutral uniforms -- this path
	// is a plain blur and wants no tint or fringing at all.
	BloomCombineUniforms plainCombine;
	plainCombine.Tint = FVector3(1.0f, 1.0f, 1.0f);
	plainCombine.Chromatic = 0.0f;

	// Grab the area we want to bloom:
	renderstate->Clear();
	renderstate->Shader = &BloomCombine;
	renderstate->Uniforms.Set(plainCombine);
	renderstate->Viewport = level0.Viewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&level0.VTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	BlurUniforms blurUniforms;
	ComputeBlurSamples(7, blurAmount, blurUniforms.SampleWeights);

	// Blur and downscale:
	for (int i = 0; i < numLevels - 1; i++)
	{
		auto &blevel = levels[i];
		auto &next = levels[i + 1];

		BlurStep(renderstate, blurUniforms, blevel.VTexture, blevel.HTexture, blevel.Viewport, false);
		BlurStep(renderstate, blurUniforms, blevel.HTexture, blevel.VTexture, blevel.Viewport, true);

		// Linear downscale:
		renderstate->Clear();
		renderstate->Shader = &BloomCombine;
		renderstate->Uniforms.Set(plainCombine);
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.VTexture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// Blur and upscale:
	for (int i = numLevels - 1; i > 0; i--)
	{
		auto &blevel = levels[i];
		auto &next = levels[i - 1];

		BlurStep(renderstate, blurUniforms, blevel.VTexture, blevel.HTexture, blevel.Viewport, false);
		BlurStep(renderstate, blurUniforms, blevel.HTexture, blevel.VTexture, blevel.Viewport, true);

		// Linear upscale:
		renderstate->Clear();
		renderstate->Shader = &BloomCombine;
		renderstate->Uniforms.Set(plainCombine);
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.VTexture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	BlurStep(renderstate, blurUniforms, level0.VTexture, level0.HTexture, level0.Viewport, false);
	BlurStep(renderstate, blurUniforms, level0.HTexture, level0.VTexture, level0.Viewport, true);

	// Copy blur back to scene texture:
	renderstate->Clear();
	renderstate->Shader = &BloomCombine;
	renderstate->Uniforms.Clear();
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputTexture(0, &level0.VTexture, PPFilterMode::Linear);
	renderstate->SetOutputCurrent();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

void PPBloom::BlurStep(PPRenderState *renderstate, const BlurUniforms &blurUniforms, PPTexture &input, PPTexture &output, PPViewport viewport, bool vertical)
{
	renderstate->Clear();
	renderstate->Shader = vertical ? &BlurVertical : &BlurHorizontal;
	renderstate->Uniforms.Set(blurUniforms);
	renderstate->Viewport = viewport;
	renderstate->SetInputTexture(0, &input);
	renderstate->SetOutputTexture(&output);
	renderstate->SetNoBlend();
	renderstate->Draw();
}

float PPBloom::ComputeBlurGaussian(float n, float theta) // theta = Blur Amount
{
	return (float)((1.0f / sqrtf(2 * (float)M_PI * theta)) * expf(-(n * n) / (2.0f * theta * theta)));
}

void PPBloom::ComputeBlurSamples(int sampleCount, float blurAmount, float *sampleWeights)
{
	sampleWeights[0] = ComputeBlurGaussian(0, blurAmount);

	float totalWeights = sampleWeights[0];

	for (int i = 0; i < sampleCount / 2; i++)
	{
		float weight = ComputeBlurGaussian(i + 1.0f, blurAmount);

		sampleWeights[i * 2 + 1] = weight;
		sampleWeights[i * 2 + 2] = weight;

		totalWeights += weight * 2;
	}

	for (int i = 0; i < sampleCount; i++)
	{
		sampleWeights[i] /= totalWeights;
	}
}

// [BLOOMSTEP] One wide blur pass (gl_bloom_step > 1). Linear, because the reads fall
// between texels; today's BlurStep above stays Nearest and untouched.
void PPBloom::BlurStepWide(PPRenderState *renderstate, const BlurSteppedUniforms &blurUniforms, PPTexture &input, PPTexture &output, PPViewport viewport, bool vertical)
{
	renderstate->Clear();
	renderstate->Shader = vertical ? &BlurVerticalStepped : &BlurHorizontalStepped;
	renderstate->Uniforms.Set(blurUniforms);
	renderstate->Viewport = viewport;
	renderstate->SetInputTexture(0, &input, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&output);
	renderstate->SetNoBlend();
	renderstate->Draw();
}

// [BLOOMSTEP] The wide blur's weights and read layout (E3 in REVIEW_BLOOM_PLAN.md).
// theta stays the Gaussian's width in texels, as for today's blur; the taps sit
// texelStep texels apart, so each weight is the Gaussian at the tap's true distance,
// n = (i + 1) * texelStep, and a wide kernel keeps its shape instead of flattening into
// a box. Each tap averages ReadsPerTap linear reads over its own stretch, at most two
// texels apart, which the chain simulation needs to leave no ghost copies
// (BLOOM_STEP1_IMPL_NOTES.md); the step is capped so that stays bounded.
void PPBloom::ComputeBlurSamplesStepped(float theta, float texelStep, BlurSteppedUniforms &uniforms)
{
	if (texelStep > MAX_BLUR_TEXEL_STEP)
		texelStep = MAX_BLUR_TEXEL_STEP;
	uniforms.TexelStep = texelStep;
	uniforms.ReadsPerTap = clamp((int)std::ceil(texelStep * 0.5f), 2, MAX_BLUR_READS_PER_TAP);

	float *sampleWeights = uniforms.SampleWeights;
	sampleWeights[0] = ComputeBlurGaussian(0, theta);
	float totalWeights = sampleWeights[0];
	for (int i = 0; i < 3; i++)
	{
		float weight = ComputeBlurGaussian((i + 1.0f) * texelStep, theta);
		sampleWeights[i * 2 + 1] = weight;
		sampleWeights[i * 2 + 2] = weight;
		totalWeights += weight * 2;
	}
	for (int i = 0; i < 7; i++)
	{
		sampleWeights[i] /= totalWeights;
	}
	sampleWeights[7] = 0.0f;
}

/////////////////////////////////////////////////////////////////////////////

void PPLensDistort::Render(PPRenderState *renderstate)
{
	if (gl_lens == 0)
	{
		return;
	}

	float k[4] =
	{
		gl_lens_k,
		gl_lens_k * gl_lens_chromatic,
		gl_lens_k * gl_lens_chromatic * gl_lens_chromatic,
		0.0f
	};
	float kcube[4] =
	{
		gl_lens_kcube,
		gl_lens_kcube * gl_lens_chromatic,
		gl_lens_kcube * gl_lens_chromatic * gl_lens_chromatic,
		0.0f
	};

	float aspect = screen->mSceneViewport.width / (float)screen->mSceneViewport.height;

	// Scale factor to keep sampling within the input texture
	float r2 = aspect * aspect * 0.25f + 0.25f;
	float sqrt_r2 = sqrt(r2);
	float f0 = 1.0f + max(r2 * (k[0] + kcube[0] * sqrt_r2), 0.0f);
	float f2 = 1.0f + max(r2 * (k[2] + kcube[2] * sqrt_r2), 0.0f);
	float f = max(f0, f2);
	float scale = 1.0f / f;

	LensUniforms uniforms;
	uniforms.AspectRatio = aspect;
	uniforms.Scale = scale;
	uniforms.LensDistortionCoefficient = k;
	uniforms.CubicDistortionValue = kcube;

	renderstate->PushGroup("lens");

	renderstate->Clear();
	renderstate->Shader = &Lens;
	renderstate->Uniforms.Set(uniforms);
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

void PPFXAA::Render(PPRenderState *renderstate)
{
	if (0 == gl_fxaa)
	{
		return;
	}

	CreateShaders();

	FXAAUniforms uniforms;
	uniforms.ReciprocalResolution = { 1.0f / screen->mScreenViewport.width, 1.0f / screen->mScreenViewport.height };

	renderstate->PushGroup("fxaa");

	renderstate->Clear();
	renderstate->Shader = &FXAALuma;
	renderstate->Uniforms.Clear();
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Nearest);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->Shader = &FXAA;
	renderstate->Uniforms.Set(uniforms);
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->Draw();

	renderstate->PopGroup();
}

int PPFXAA::GetMaxVersion()
{
	return screen->glslversion >= 4.f ? 400 : 330;
}

void PPFXAA::CreateShaders()
{
	if (LastQuality == gl_fxaa)
		return;

	FXAALuma = { "shaders/pp/fxaa.fp", "#define FXAA_LUMA_PASS\n", {} };
	FXAA = { "shaders/pp/fxaa.fp", GetDefines(), FXAAUniforms::Desc(), GetMaxVersion() };
	LastQuality = gl_fxaa;
}

FString PPFXAA::GetDefines()
{
	int quality;

	switch (gl_fxaa)
	{
	default:
	case IFXAAShader::Low:     quality = 10; break;
	case IFXAAShader::Medium:  quality = 12; break;
	case IFXAAShader::High:    quality = 29; break;
	case IFXAAShader::Extreme: quality = 39; break;
	}

	const int gatherAlpha = GetMaxVersion() >= 400 ? 1 : 0;

	// TODO: enable FXAA_GATHER4_ALPHA on OpenGL earlier than 4.0
	// when GL_ARB_gpu_shader5/GL_NV_gpu_shader5 extensions are supported

	FString result;
	result.Format(
		"#define FXAA_QUALITY__PRESET %i\n"
		"#define FXAA_GATHER4_ALPHA %i\n",
		quality, gatherAlpha);

	return result;
}

/////////////////////////////////////////////////////////////////////////////

// [BLOOMSAFETY] [PINNEDBLOOM] E2's exposure guards (see Render), in one place for the live exposure and
// the pinned bloom look.
ExposureCombineUniforms PPCameraExposure::CombineUniforms(float base, float minimum, float scale, float speed)
{
	auto finiteOr = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
	const float exposureMin = minimum;
	const float exposureSpeed = finiteOr(speed, 0.05f);

	ExposureCombineUniforms combineUniforms;
	combineUniforms.ExposureBase = finiteOr(base, 0.35f);
	combineUniforms.ExposureMin = (exposureMin >= 0.01f) ? exposureMin : 0.01f;
	combineUniforms.ExposureScale = finiteOr(scale, 1.3f);
	combineUniforms.ExposureSpeed = exposureSpeed < 0.0f ? 0.0f : (exposureSpeed > 1.0f ? 1.0f : exposureSpeed);
	return combineUniforms;
}

void PPCameraExposure::Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight)
{
	if (!gl_bloom)
	{
		return;
	}

	renderstate->PushGroup("exposure");

	UpdateTextures(sceneWidth, sceneHeight);

	ExposureExtractUniforms extractUniforms;
	extractUniforms.Scale = screen->SceneScale();
	extractUniforms.Offset = screen->SceneOffset();

	// [BLOOMSAFETY] E2 in "Engine docs/REVIEW_BLOOM_PLAN.md". The gl_exposure_* cvars are
	// plain and unclamped, and exposurecombine.fp divides by max(Base + light * Scale, Min):
	// with Min 0 a black frame (a wipe, a fade, a dark sector, the first frame) divides by
	// zero, the inf sticks in the blended camera texture for the rest of the session, and
	// the bloom extract turns it into NaN over the whole scene. So, in the uniforms only
	// (the lump and the archived cvars are untouched):
	//   Min is floored at 0.01, so exposure is finite (at most 100x) whatever Base and
	//     Scale are; a NaN Min lands on the floor too.
	//   Speed is the blend weight of a running average: outside 0..1 the average
	//     overshoots and, at 2 or more or below 0, grows without bound. Clamped to 0..1.
	//   A non-finite Base, Scale or Speed typed at the console falls back to its default.
	// Every finite setting inside those ranges -- the defaults and everything RS_Bloom's
	// sliders can reach -- passes through bit for bit.
	// [PINNEDBLOOM] The guards live in CombineUniforms, which the pinned bloom look uses too.
	const ExposureCombineUniforms combineUniforms = CombineUniforms(gl_exposure_base, gl_exposure_min, gl_exposure_scale, gl_exposure_speed);

	auto &level0 = ExposureLevels[0];

	// Extract light blevel from scene texture:
	renderstate->Clear();
	renderstate->Shader = &ExposureExtract;
	renderstate->Uniforms.Set(extractUniforms);
	renderstate->Viewport = level0.Viewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&level0.Texture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// Find the average value:
	for (size_t i = 0; i + 1 < ExposureLevels.size(); i++)
	{
		auto &blevel = ExposureLevels[i];
		auto &next = ExposureLevels[i + 1];

		renderstate->Shader = &ExposureAverage;
		renderstate->Uniforms.Clear();
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.Texture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.Texture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// Combine average value with current camera exposure:
	renderstate->Shader = &ExposureCombine;
	renderstate->Uniforms.Set(combineUniforms);
	renderstate->Viewport.left = 0;
	renderstate->Viewport.top = 0;
	renderstate->Viewport.width = 1;
	renderstate->Viewport.height = 1;
	renderstate->SetInputTexture(0, &ExposureLevels.back().Texture, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&CameraTexture);
	if (!FirstExposureFrame)
		renderstate->SetAlphaBlend();
	else
		renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();

	FirstExposureFrame = false;
}

// [PINNEDBLOOM] The pinned bloom look's exposure (hw_postprocess.h). The same combine as Render's last draw,
// over the average Render measured for this eye, into PinnedCameraTexture. It blends with its last value
// only when it drew on this displayed frame (the other eye) or the one before, and the levels were not
// rebuilt since; otherwise that value is stale and this frame's is taken whole, as Render's first frame is.
void PPCameraExposure::RenderPinned(PPRenderState *renderstate, const ExposureCombineUniforms &combineUniforms)
{
	if (ExposureLevels.empty())
		return;

	const uint64_t frame = screen->FrameCount;
	const bool blend = PinnedHistory && (frame == PinnedLastFrame || frame == PinnedLastFrame + 1);

	renderstate->PushGroup("exposure.pinned");
	renderstate->Clear();
	renderstate->Shader = &ExposureCombine;
	renderstate->Uniforms.Set(combineUniforms);
	renderstate->Viewport.left = 0;
	renderstate->Viewport.top = 0;
	renderstate->Viewport.width = 1;
	renderstate->Viewport.height = 1;
	renderstate->SetInputTexture(0, &ExposureLevels.back().Texture, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&PinnedCameraTexture);
	if (blend)
		renderstate->SetAlphaBlend();
	else
		renderstate->SetNoBlend();
	renderstate->Draw();
	renderstate->PopGroup();

	PinnedHistory = true;
	PinnedLastFrame = frame;
}

void PPCameraExposure::UpdateTextures(int width, int height)
{
	int firstwidth = max(width / 2, 1);
	int firstheight = max(height / 2, 1);

	if (ExposureLevels.size() > 0 && ExposureLevels[0].Viewport.width == firstwidth && ExposureLevels[0].Viewport.height == firstheight)
	{
		return;
	}

	ExposureLevels.clear();

	int i = 0;
	do
	{
		width = max(width / 2, 1);
		height = max(height / 2, 1);

		PPExposureLevel blevel;
		blevel.Viewport.left = 0;
		blevel.Viewport.top = 0;
		blevel.Viewport.width = width;
		blevel.Viewport.height = height;
		blevel.Texture = { blevel.Viewport.width, blevel.Viewport.height, PixelFormat::R32f };
		ExposureLevels.push_back(std::move(blevel));

		i++;

	} while (width > 1 || height > 1);

	FirstExposureFrame = true;
	PinnedHistory = false;	// [PINNEDBLOOM] new levels: the pinned exposure starts over too
}

/////////////////////////////////////////////////////////////////////////////

void PPColormap::Render(PPRenderState *renderstate, int fixedcm, float flash)
{
	ColormapUniforms uniforms;

	if (fixedcm < CM_FIRSTSPECIALCOLORMAP || fixedcm >= CM_MAXCOLORMAP)
	{
		if (flash == 1.f)
			return;

		uniforms.MapStart = { 0,0,0, flash };
		uniforms.MapRange = { 0,0,0, 1.f };
	}
	else
	{
		FSpecialColormap* scm = &SpecialColormaps[fixedcm - CM_FIRSTSPECIALCOLORMAP];

		uniforms.MapStart = { scm->ColorizeStart[0], scm->ColorizeStart[1], scm->ColorizeStart[2], flash };
		uniforms.MapRange = { scm->ColorizeEnd[0] - scm->ColorizeStart[0],
			scm->ColorizeEnd[1] - scm->ColorizeStart[1], scm->ColorizeEnd[2] - scm->ColorizeStart[2], 0.f };
	}

	renderstate->PushGroup("colormap");

	renderstate->Clear();
	renderstate->Shader = &Colormap;
	renderstate->Uniforms.Set(uniforms);
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

void PPTonemap::UpdateTextures()
{
	// level.info->tonemap cannot be ETonemapMode::Palette, so it's fine to only check gl_tonemap here
	if (ETonemapMode((int)gl_tonemap) == ETonemapMode::Palette && !PaletteTexture.Data)
	{
		std::shared_ptr<void> data(new uint32_t[512 * 512], [](void *p) { delete[](uint32_t*)p; });

		uint8_t *lut = (uint8_t *)data.get();
		for (int r = 0; r < 64; r++)
		{
			for (int g = 0; g < 64; g++)
			{
				for (int b = 0; b < 64; b++)
				{
					PalEntry color = GPalette.BaseColors[(uint8_t)PTM_BestColor((uint32_t *)GPalette.BaseColors, (r << 2) | (r >> 4), (g << 2) | (g >> 4), (b << 2) | (b >> 4),
						gl_paltonemap_reverselookup, gl_paltonemap_powtable, 0, 256)];
					int index = ((r * 64 + g) * 64 + b) * 4;
					lut[index] = color.r;
					lut[index + 1] = color.g;
					lut[index + 2] = color.b;
					lut[index + 3] = 255;
				}
			}
		}

		PaletteTexture = { 512, 512, PixelFormat::Rgba8, data };
	}
}

void PPTonemap::Render(PPRenderState *renderstate)
{
	ETonemapMode current_tonemap = (level_tonemap != ETonemapMode::None) ? level_tonemap : ETonemapMode((int)gl_tonemap);

	if (current_tonemap == ETonemapMode::None)
	{
		return;
	}

	UpdateTextures();

	PPShader *shader = nullptr;
	switch (current_tonemap)
	{
	default:
	case ETonemapMode::Linear:		shader = &LinearShader; break;
	case ETonemapMode::Reinhard:		shader = &ReinhardShader; break;
	case ETonemapMode::HejlDawson:	shader = &HejlDawsonShader; break;
	case ETonemapMode::Uncharted2:	shader = &Uncharted2Shader; break;
	case ETonemapMode::Palette:		shader = &PaletteShader; break;
	}

	renderstate->PushGroup("tonemap");

	renderstate->Clear();
	renderstate->Shader = shader;
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0);
	if (current_tonemap == ETonemapMode::Palette)
		renderstate->SetInputTexture(1, &PaletteTexture);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

PPAmbientOcclusion::PPAmbientOcclusion()
{
	// Must match quality enum in PPAmbientOcclusion::DeclareShaders
	double numDirections[NumAmbientRandomTextures] = { 2.0, 4.0, 8.0 };

	std::mt19937 generator(1337);
	std::uniform_real_distribution<double> distribution(0.0, 1.0);
	for (int quality = 0; quality < NumAmbientRandomTextures; quality++)
	{
		std::shared_ptr<void> data(new int16_t[16 * 4], [](void *p) { delete[](int16_t*)p; });
		int16_t *randomValues = (int16_t *)data.get();

		for (int i = 0; i < 16; i++)
		{
			double angle = 2.0 * M_PI * distribution(generator) / numDirections[quality];
			double x = cos(angle);
			double y = sin(angle);
			double z = distribution(generator);
			double w = distribution(generator);

			randomValues[i * 4 + 0] = (int16_t)clamp(x * 32767.0, -32768.0, 32767.0);
			randomValues[i * 4 + 1] = (int16_t)clamp(y * 32767.0, -32768.0, 32767.0);
			randomValues[i * 4 + 2] = (int16_t)clamp(z * 32767.0, -32768.0, 32767.0);
			randomValues[i * 4 + 3] = (int16_t)clamp(w * 32767.0, -32768.0, 32767.0);
		}

		AmbientRandomTexture[quality] = { 4, 4, PixelFormat::Rgba16_snorm, data };
	}
}

void PPAmbientOcclusion::CreateShaders()
{
	if (gl_ssao == LastQuality)
		return;

	// Must match quality values in PPAmbientOcclusion::UpdateTextures
	int numDirections, numSteps;
	switch (gl_ssao)
	{
	default:
	case LowQuality:    numDirections = 2; numSteps = 4; break;
	case MediumQuality: numDirections = 4; numSteps = 4; break;
	case HighQuality:   numDirections = 8; numSteps = 4; break;
	}

	FString defines;
	defines.Format(R"(
		#define USE_RANDOM_TEXTURE
		#define RANDOM_TEXTURE_WIDTH 4.0
		#define NUM_DIRECTIONS %d.0
		#define NUM_STEPS %d.0
	)", numDirections, numSteps);

	LinearDepth = { "shaders/pp/lineardepth.fp", "", LinearDepthUniforms::Desc() };
	LinearDepthMS = { "shaders/pp/lineardepth.fp", "#define MULTISAMPLE\n", LinearDepthUniforms::Desc() };
	AmbientOcclude = { "shaders/pp/ssao.fp", defines, SSAOUniforms::Desc() };
	AmbientOccludeMS = { "shaders/pp/ssao.fp", defines + "\n#define MULTISAMPLE\n", SSAOUniforms::Desc() };
	BlurVertical = { "shaders/pp/depthblur.fp", "#define BLUR_VERTICAL\n", DepthBlurUniforms::Desc() };
	BlurHorizontal = { "shaders/pp/depthblur.fp", "#define BLUR_HORIZONTAL\n", DepthBlurUniforms::Desc() };
	Combine = { "shaders/pp/ssaocombine.fp", "", AmbientCombineUniforms::Desc() };
	CombineMS = { "shaders/pp/ssaocombine.fp", "#define MULTISAMPLE\n", AmbientCombineUniforms::Desc() };

	LastQuality = gl_ssao;
}

void PPAmbientOcclusion::UpdateTextures(int width, int height)
{
	if ((width <= 0 || height <= 0) || (width == LastWidth && height == LastHeight))
		return;

	AmbientWidth = (width + 1) / 2;
	AmbientHeight = (height + 1) / 2;

	LinearDepthTexture = { AmbientWidth, AmbientHeight, PixelFormat::R32f };
	Ambient0 = { AmbientWidth, AmbientHeight, PixelFormat::Rg16f };
	Ambient1 = { AmbientWidth, AmbientHeight, PixelFormat::Rg16f };

	LastWidth = width;
	LastHeight = height;
}

void PPAmbientOcclusion::Render(PPRenderState *renderstate, float m5, int sceneWidth, int sceneHeight)
{
	if (gl_ssao == 0 || sceneWidth == 0 || sceneHeight == 0 || level_noAmbientOcclusion)
	{
		return;
	}

	CreateShaders();
	UpdateTextures(sceneWidth, sceneHeight);

	float bias = gl_ssao_bias;
	float aoRadius = gl_ssao_radius;
	const float blurAmount = gl_ssao_blur;
	float aoStrength = gl_ssao_strength;

	//float tanHalfFovy = tan(fovy * (M_PI / 360.0f));
	float tanHalfFovy = 1.0f / m5;
	float invFocalLenX = tanHalfFovy * (sceneWidth / (float)sceneHeight);
	float invFocalLenY = tanHalfFovy;
	float nDotVBias = clamp(bias, 0.0f, 1.0f);
	float r2 = aoRadius * aoRadius;

	float blurSharpness = 1.0f / blurAmount;

	auto sceneScale = screen->SceneScale();
	auto sceneOffset = screen->SceneOffset();

	int randomTexture = clamp(gl_ssao - 1, 0, NumAmbientRandomTextures - 1);

	LinearDepthUniforms linearUniforms;
	linearUniforms.SampleIndex = 0;
	linearUniforms.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	linearUniforms.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);
	linearUniforms.InverseDepthRangeA = 1.0f;
	linearUniforms.InverseDepthRangeB = 0.0f;
	linearUniforms.Scale = sceneScale;
	linearUniforms.Offset = sceneOffset;

	SSAOUniforms ssaoUniforms;
	ssaoUniforms.SampleIndex = 0;
	ssaoUniforms.UVToViewA = { 2.0f * invFocalLenX, 2.0f * invFocalLenY };
	ssaoUniforms.UVToViewB = { -invFocalLenX, -invFocalLenY };
	ssaoUniforms.InvFullResolution = { 1.0f / AmbientWidth, 1.0f / AmbientHeight };
	ssaoUniforms.NDotVBias = nDotVBias;
	ssaoUniforms.NegInvR2 = -1.0f / r2;
	ssaoUniforms.RadiusToScreen = aoRadius * 0.5f / tanHalfFovy * AmbientHeight;
	ssaoUniforms.AOMultiplier = 1.0f / (1.0f - nDotVBias);
	ssaoUniforms.AOStrength = aoStrength;
	ssaoUniforms.Scale = sceneScale;
	ssaoUniforms.Offset = sceneOffset;
	ssaoUniforms.GlobalFade = gl_global_fade ? 1 : 0;
	ssaoUniforms.GlobalFadeDensity = gl_global_fade_density;
	ssaoUniforms.GlobalFadeGradient = gl_global_fade_gradient;

	DepthBlurUniforms blurUniforms;
	blurUniforms.BlurSharpness = blurSharpness;
	blurUniforms.PowExponent = gl_ssao_exponent;

	AmbientCombineUniforms combineUniforms;
	combineUniforms.SampleCount = gl_multisample;
	combineUniforms.Scale = screen->SceneScale();
	combineUniforms.Offset = screen->SceneOffset();
	combineUniforms.DebugMode = gl_ssao_debug;

	IntRect ambientViewport;
	ambientViewport.left = 0;
	ambientViewport.top = 0;
	ambientViewport.width = AmbientWidth;
	ambientViewport.height = AmbientHeight;

	renderstate->PushGroup("ssao");

	// Calculate linear depth values
	renderstate->Clear();
	renderstate->Shader = gl_multisample > 1 ? &LinearDepthMS : &LinearDepth;
	renderstate->Uniforms.Set(linearUniforms);
	renderstate->Viewport = ambientViewport;
	renderstate->SetInputSceneDepth(0);
	renderstate->SetInputSceneColor(1);
	renderstate->SetOutputTexture(&LinearDepthTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// Apply ambient occlusion
	renderstate->Clear();
	renderstate->Shader = gl_multisample > 1 ? &AmbientOccludeMS : &AmbientOcclude;
	renderstate->Uniforms.Set(ssaoUniforms);
	renderstate->Viewport = ambientViewport;
	renderstate->SetInputTexture(0, &LinearDepthTexture);
	renderstate->SetInputSceneNormal(1);
	renderstate->SetInputTexture(2, &AmbientRandomTexture[randomTexture], PPFilterMode::Nearest, PPWrapMode::Repeat);
	renderstate->SetOutputTexture(&Ambient0);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// Blur SSAO texture
	if (gl_ssao_debug < 2)
	{
		renderstate->Clear();
		renderstate->Shader = &BlurHorizontal;
		renderstate->Uniforms.Set(blurUniforms);
		renderstate->Viewport = ambientViewport;
		renderstate->SetInputTexture(0, &Ambient0);
		renderstate->SetOutputTexture(&Ambient1);
		renderstate->SetNoBlend();
		renderstate->Draw();

		renderstate->Clear();
		renderstate->Shader = &BlurVertical;
		renderstate->Uniforms.Set(blurUniforms);
		renderstate->Viewport = ambientViewport;
		renderstate->SetInputTexture(0, &Ambient1);
		renderstate->SetOutputTexture(&Ambient0);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// Add SSAO back to scene texture:
	renderstate->Clear();
	renderstate->Shader = gl_multisample > 1 ? &CombineMS : &Combine;
	renderstate->Uniforms.Set(combineUniforms);
	renderstate->Viewport = screen->mSceneViewport;
	if (gl_ssao_debug < 4)
		renderstate->SetInputTexture(0, &Ambient0, PPFilterMode::Linear);
	else
		renderstate->SetInputSceneNormal(0, PPFilterMode::Linear);
	renderstate->SetInputSceneFog(1);
	renderstate->SetOutputSceneColor();
	if (gl_ssao_debug != 0)
		renderstate->SetNoBlend();
	else
		renderstate->SetAlphaBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

PPPresent::PPPresent()
{
	static const float data[64] =
	{
			.0078125, .2578125, .1328125, .3828125, .0234375, .2734375, .1484375, .3984375,
			.7578125, .5078125, .8828125, .6328125, .7734375, .5234375, .8984375, .6484375,
			.0703125, .3203125, .1953125, .4453125, .0859375, .3359375, .2109375, .4609375,
			.8203125, .5703125, .9453125, .6953125, .8359375, .5859375, .9609375, .7109375,
			.0390625, .2890625, .1640625, .4140625, .0546875, .3046875, .1796875, .4296875,
			.7890625, .5390625, .9140625, .6640625, .8046875, .5546875, .9296875, .6796875,
			.1015625, .3515625, .2265625, .4765625, .1171875, .3671875, .2421875, .4921875,
			.8515625, .6015625, .9765625, .7265625, .8671875, .6171875, .9921875, .7421875,
	};

	std::shared_ptr<void> pixels(new float[64], [](void *p) { delete[](float*)p; });
	memcpy(pixels.get(), data, 64 * sizeof(float));
	Dither = { 8, 8, PixelFormat::R32f, pixels };
}

/////////////////////////////////////////////////////////////////////////////


void PPShadowMap::Update(PPRenderState* renderstate)
{
	ShadowMapUniforms uniforms;
	uniforms.ShadowmapQuality = (float)gl_shadowmap_quality;
	uniforms.NodesCount = screen->mShadowMap.NodesCount();

	renderstate->PushGroup("shadowmap");

	renderstate->Clear();
	renderstate->Shader = &ShadowMap;
	renderstate->Uniforms.Set(uniforms);
	renderstate->Viewport = { 0, 0, gl_shadowmap_quality, 1024 };
	renderstate->SetShadowMapBuffers(true);
	renderstate->SetOutputShadowMap();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

CVAR(Bool, gl_custompost, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)  // user can change this

void PPCustomShaders::Run(PPRenderState *renderstate, FString target)
{
	if (!gl_custompost)
		return;

	CreateShaders();

	int width = renderstate->Viewport.width;
	int height = renderstate->Viewport.height;

	if (width > 0 && height > 0 && width <= 16384 && height <= 16384)
	{
		mLastWidth = width;
		mLastHeight = height;
	}

	for (auto &shader : mShaders)
	{
		if (shader->Desc->Target == target && shader->Desc->Enabled)
		{
			shader->Run(renderstate);
		}
	}
}

void PPCustomShaders::UpdateLastInputTexture(PPRenderState *renderstate)
{
	int width = renderstate->Viewport.width;
	int height = renderstate->Viewport.height;

	if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
		return;

	if (!mLastInputTexture ||
		mLastInputTexture->GetRead()->Width != width ||
		mLastInputTexture->GetRead()->Height != height)
	{
		mLastInputTexture = std::make_unique<PPPersistentBuffer>(width, height, PixelFormat::Rgba16f);
	}

	renderstate->CopyToTexture(mLastInputTexture->GetWrite());

	mLastInputTexture->Swap();
}

void PPCustomShaders::CreateShaders()
{
	if (mShaders.size() == PostProcessShaders.Size())
		return;

	mShaders.clear();

	for (unsigned int i = 0; i < PostProcessShaders.Size(); i++)
	{
		mShaders.push_back(std::make_unique<PPCustomShaderInstance>(&PostProcessShaders[i], &mLastInputTexture));
	}
}

/////////////////////////////////////////////////////////////////////////////

PPCustomShaderInstance::PPCustomShaderInstance(PostProcessShader *desc, std::unique_ptr<PPPersistentBuffer> *lastInputTexture) : Desc(desc), LastInputTexture(lastInputTexture)
{
	// Build an uniform block to be used as input
	TMap<FString, PostProcessUniformValue>::Iterator it(Desc->Uniforms);
	TMap<FString, PostProcessUniformValue>::Pair *pair;
	size_t offset = 0;
	while (it.NextPair(pair))
	{
		FString type;
		FString name = pair->Key;

		switch (pair->Value.Type)
		{
		case PostProcessUniformType::Float: AddUniformField(offset, name, UniformType::Float, sizeof(float)); break;
		case PostProcessUniformType::Int: AddUniformField(offset, name, UniformType::Int, sizeof(int)); break;
		case PostProcessUniformType::Vec2: AddUniformField(offset, name, UniformType::Vec2, sizeof(float) * 2); break;
		case PostProcessUniformType::Vec3: AddUniformField(offset, name, UniformType::Vec3, sizeof(float) * 3, sizeof(float) * 4); break;
		case PostProcessUniformType::Vec4: AddUniformField(offset, name, UniformType::Vec4, sizeof(float) * 4); break;
		default: break;
		}
	}
	UniformStructSize = ((int)offset + 15) / 16 * 16;

	// Build the input textures
	FString uniformTextures;
	uniformTextures += "layout(binding=0) uniform sampler2D InputTexture;\n";

	TMap<FString, FString>::Iterator itTextures(Desc->Textures);
	TMap<FString, FString>::Pair *pairTextures;
	int binding = 1;
	while (itTextures.NextPair(pairTextures))
	{
		uniformTextures.AppendFormat("layout(binding=%d) uniform sampler2D %s;\n", binding++, pairTextures->Key.GetChars());
	}

	// Setup pipeline
	FString pipelineInOut;
	if (screen->IsVulkan())
	{
		pipelineInOut += "layout(location=0) in vec2 TexCoord;\n";
		pipelineInOut += "layout(location=0) out vec4 FragColor;\n";
	}
	else
	{
		pipelineInOut += "in vec2 TexCoord;\n";
		pipelineInOut += "out vec4 FragColor;\n";
	}

	LastInputTextureBinding = binding;
	uniformTextures.AppendFormat("layout(binding=%d) uniform sampler2D LastInputTexture;\n", LastInputTextureBinding);

	FString prolog;
	prolog += uniformTextures;
	prolog += pipelineInOut;
	// Note: Automatic uniforms (InputTimeDelta, InputTime, InputTimeGame) are added by backends

	Shader = PPShader(Desc->ShaderLumpName, prolog, Fields);
}

void PPCustomShaderInstance::Run(PPRenderState *renderstate)
{
	renderstate->PushGroup(Desc->Name);

	renderstate->Clear();
	renderstate->Shader = &Shader;
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetNoBlend();
	renderstate->SetOutputNext();
	//renderstate->SetDebugName(Desc->ShaderLumpName.GetChars());

	SetTextures(renderstate);
	SetUniforms(renderstate);

	renderstate->Draw();

	renderstate->PopGroup();
}

void PPCustomShaderInstance::SetTextures(PPRenderState *renderstate)
{
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);

	int textureIndex = 1;
	TMap<FString, FString>::Iterator it(Desc->Textures);
	TMap<FString, FString>::Pair *pair;
	while (it.NextPair(pair))
	{
		FString name = pair->Value;
		auto gtex = TexMan.GetGameTexture(TexMan.CheckForTexture(name.GetChars(), ETextureType::Any), true);
		if (gtex && gtex->isValid())
		{
			// Why does this completely circumvent the normal way of handling textures?
			// This absolutely needs fixing because it will also circumvent any potential caching system that may get implemented.
			//
			// To do: fix the above problem by adding PPRenderState::SetInput(FTexture *tex)

			auto tex = gtex->GetTexture();
			auto &pptex = Textures[tex];
			if (!pptex)
			{
				auto buffer = tex->CreateTexBuffer(0);

				std::shared_ptr<void> data(new uint32_t[buffer.mWidth * buffer.mHeight], [](void *p) { delete[](uint32_t*)p; });

				int count = buffer.mWidth * buffer.mHeight;
				uint8_t *pixels = (uint8_t *)data.get();
				for (int i = 0; i < count; i++)
				{
					int pos = i << 2;
					pixels[pos] = buffer.mBuffer[pos + 2];
					pixels[pos + 1] = buffer.mBuffer[pos + 1];
					pixels[pos + 2] = buffer.mBuffer[pos];
					pixels[pos + 3] = buffer.mBuffer[pos + 3];
				}

				pptex = std::make_unique<PPTexture>(buffer.mWidth, buffer.mHeight, PixelFormat::Rgba8, data);
			}

			renderstate->SetInputTexture(textureIndex, pptex.get(), PPFilterMode::Linear, PPWrapMode::Repeat);
			textureIndex++;
		}
	}

	if (LastInputTexture && *LastInputTexture && LastInputTextureBinding >= 0)
	{
		auto texture = (*LastInputTexture)->GetRead();
		if (texture->Backend)
		{
			renderstate->SetInputTexture(LastInputTextureBinding, texture, PPFilterMode::Linear, PPWrapMode::Clamp);
		}
	}
}

void PPCustomShaderInstance::SetUniforms(PPRenderState *renderstate)
{
	TArray<uint8_t> uniforms;
	uniforms.Resize(UniformStructSize);

	TMap<FString, PostProcessUniformValue>::Iterator it(Desc->Uniforms);
	TMap<FString, PostProcessUniformValue>::Pair *pair;
	while (it.NextPair(pair))
	{
		auto it2 = FieldOffset.find(pair->Key);
		if (it2 != FieldOffset.end())
		{
			uint8_t *dst = &uniforms[it2->second];
			float fValues[4];
			int iValues[4];
			switch (pair->Value.Type)
			{
			case PostProcessUniformType::Float:
				fValues[0] = (float)pair->Value.Values[0];
				memcpy(dst, fValues, sizeof(float));
				break;
			case PostProcessUniformType::Int:
				iValues[0] = (int)pair->Value.Values[0];
				memcpy(dst, iValues, sizeof(int));
				break;
			case PostProcessUniformType::Vec2:
				fValues[0] = (float)pair->Value.Values[0];
				fValues[1] = (float)pair->Value.Values[1];
				memcpy(dst, fValues, sizeof(float) * 2);
				break;
			case PostProcessUniformType::Vec3:
				fValues[0] = (float)pair->Value.Values[0];
				fValues[1] = (float)pair->Value.Values[1];
				fValues[2] = (float)pair->Value.Values[2];
				memcpy(dst, fValues, sizeof(float) * 3);
				break;
			case PostProcessUniformType::Vec4:
				fValues[0] = (float)pair->Value.Values[0];
				fValues[1] = (float)pair->Value.Values[1];
				fValues[2] = (float)pair->Value.Values[2];
				fValues[3] = (float)pair->Value.Values[3];
				memcpy(dst, fValues, sizeof(float) * 4);
				break;
			default:
				break;
			}
		}
	}

	auto timeDeltaOffset = FieldOffset.find("InputTimeDelta");
	if (timeDeltaOffset != FieldOffset.end())
	{
		if (uniforms.Size() < timeDeltaOffset->second + sizeof(float))
			uniforms.Resize(timeDeltaOffset->second + sizeof(float));
		memcpy(&uniforms[timeDeltaOffset->second], &renderstate->TimeDelta, sizeof(float));
	}

	auto timeOffset = FieldOffset.find("InputTime");
	if (timeOffset != FieldOffset.end())
	{
		if (uniforms.Size() < timeOffset->second + sizeof(float))
			uniforms.Resize(timeOffset->second + sizeof(float));
		memcpy(&uniforms[timeOffset->second], &renderstate->Time, sizeof(float));
	}

	auto timeGameOffset = FieldOffset.find("InputTimeGame");
	if (timeGameOffset != FieldOffset.end())
	{
		if (uniforms.Size() < timeGameOffset->second + sizeof(float))
			uniforms.Resize(timeGameOffset->second + sizeof(float));
		memcpy(&uniforms[timeGameOffset->second], &renderstate->TimeGame, sizeof(float));
	}

	renderstate->Uniforms.Data = uniforms;
}

void PPCustomShaderInstance::AddUniformField(size_t &offset, const FString &name, UniformType type, size_t fieldsize, size_t alignment)
{
	if (alignment == 0) alignment = fieldsize;
	offset = (offset + alignment - 1) / alignment * alignment;

	FieldOffset[name] = offset;

	auto name2 = std::make_unique<FString>(name);
	auto chars = name2->GetChars();
	FieldNames.push_back(std::move(name2));
	Fields.push_back({ chars, type, offset });
	offset += fieldsize;

	if (fieldsize != alignment) // Workaround for buggy OpenGL drivers that does not do std140 layout correctly for vec3
	{
		name2 = std::make_unique<FString>(name + "_F39350FF12DE_padding");
		chars = name2->GetChars();
		FieldNames.push_back(std::move(name2));
		Fields.push_back({ chars, UniformType::Float, offset });
		offset += alignment - fieldsize;
	}
}


//==========================================================================
//
// [LIGHTMASK] The light mask's frame state and its debug view (hw_postprocess.h, PPLightMask).
//
//==========================================================================

bool PPLightMask::WantedByCvars()
{
	return r_lightmask_debug > 0 || (gl_bloom && gl_bloom_pin_beams);
}

void PPLightMask::BeginFrame(bool active)
{
	FrameActive = active;
	PostInput = false;
	const int mode = r_lightmask_debug;
	DebugMode = !active ? 0 : (mode < 0 ? 0 : (mode > 2 ? 2 : mode));

	// [PINNEDBLOOM] Pinned bloom for this frame, and no pinned light report yet (SetPinnedLightLive).
	PinnedBloom = active && gl_bloom && gl_bloom_pin_beams;
	PinnedLightReported = false;
	PinnedLight = false;
}

// Where bloom runs, in its place: the colour read here is the colour bloom would have read,
// which is what a share is measured against. Over the whole screen viewport into the next
// pipeline image, like the lens pass, so no texel of that image is left unwritten. Both eyes of
// a layered post path draw it or neither (the frame's state), so their pipeline images advance
// alike. Nothing is drawn for a scene without the mask (a save picture): bloom runs there.
bool PPLightMask::RenderDebug(PPRenderState *renderstate)
{
	if (DebugMode <= 0 || !PostInput)
		return false;

	LightMaskDebugUniforms uniforms = {};
	uniforms.DebugMode = DebugMode;

	renderstate->PushGroup("pp.lightmaskdebug");
	renderstate->Clear();
	renderstate->Shader = &DebugShader;
	renderstate->Uniforms.Set(uniforms);
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0);
	renderstate->SetInputLightMask(1);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();
	renderstate->PopGroup();
	return true;
}

void Postprocess::Pass1(PPRenderState* state, int fixedcm, int sceneWidth, int sceneHeight)
{
	exposure.Render(state, sceneWidth, sceneHeight);
	customShaders.Run(state, "beforebloom");
	// [SMOKEVOLUME] The smoke volume composites HERE, before the volumetric beam pass
	// ("Engine docs/SMOKE_VOLUME_PLAN.md" 13c, owner answers 2026-09-14): a beam's own air
	// glow is never dimmed by haze behind it, and 13e adds the light beams scatter in the
	// smoke. It is skipped entirely when there is no smoke (PPSmokeVolume).
	smokevolume.Render(state, sceneWidth, sceneHeight);
	volbeam.Render(state, sceneWidth, sceneHeight);
	heatmap.Render(state, sceneWidth, sceneHeight);
	// [HEATREFRACTION] Heat refraction, then bloom: the image bends before it glows.
	heatrefraction.Render(state, sceneWidth, sceneHeight);
	// [LIGHTMASK] The light mask's debug view (r_lightmask_debug) is drawn in bloom's place.
	if (!lightmask.RenderDebug(state))
		bloom.RenderBloom(state, sceneWidth, sceneHeight, fixedcm);
}

void Postprocess::Pass2(PPRenderState* state, int fixedcm, float flash, int sceneWidth, int sceneHeight)
{
	tonemap.Render(state);
	colormap.Render(state, fixedcm, flash);
	lens.Render(state);
	fxaa.Render(state);

	customShaders.Run(state, "scene");

	customShaders.UpdateLastInputTexture(state);
}
