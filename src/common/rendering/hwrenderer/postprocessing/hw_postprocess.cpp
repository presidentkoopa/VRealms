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
#include "hw_perflog.h"	// [EMISSIVETILES] E5: PerfLog::GroupsWanted, for the tile counts
#include "hw_emissivevolumeframe.h"	// [EMISSIVETILES] E5: EmissiveVolumeStats, where the tile counts go

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

	// [13e] In smoke, each step of a cone is dimmed by the haze in front of it (r_smoke_cones_depth): this eye's smoke pass
	// drew its transmittance curve just before (Pass1 order). Otherwise the programs and inputs are exactly as before.
	PPSmokeVolume &smoke = hw_postprocess.smokevolume;
	const bool inSmoke = smoke.ConesDimmedByHaze();

	for (int i = 0; i < count; i++)
	{
		if (uniforms[i].Density <= 0.0f || uniforms[i].BeamLength <= 0.0f)
			continue;

		VolumetricBeamUniforms u = uniforms[i];
		u.SceneScale = sceneScale;
		u.SceneOffset = sceneOffset;

		renderstate->Clear();
		if (inSmoke)
			renderstate->Shader = multisampled ? &BeamSmokeMS : &BeamSmoke;
		else
			renderstate->Shader = multisampled ? &BeamMS : &Beam;
		renderstate->Uniforms.Set(u);
		renderstate->Viewport = screen->mSceneViewport;
		renderstate->SetInputSceneDepth(0);
		if (inSmoke)
		{
			renderstate->SetInputTexture(1, smoke.GetMarchTexture());
			renderstate->SetInputTexture(2, smoke.GetDepthTexture());
			renderstate->SetInputTexture(3, smoke.GetCurveTexture());
		}
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

// [SHOCKWAVE] The offset texture's texels that a blast ripple's scene-UV rectangle { u0, v0, u1, v1 } needs (hw_shockwavecore.h,
// ProjectBall): one texel of margin on every side, for the texel centres at its edge and the bend pass's linear filter; never
// empty, because a draw needs at least one texel (an eye that does not see the ripple draws one texel of zeros). TexCoord runs
// from the viewport's first row, so the rectangle's scene UV is its texel origin and size over the texture's.
static PPViewport ShockwaveViewport(const float *rect, int width, int height)
{
	const auto span = [](float a, float b, int size, int &first, int &count)
	{
		const double lo = std::floor((a < 0.0f ? 0.0 : (a > 1.0f ? 1.0 : (double)a)) * size) - 1.0;
		const double hi = std::ceil((b < 0.0f ? 0.0 : (b > 1.0f ? 1.0 : (double)b)) * size) + 1.0;
		const double lastFirst = size > 1 ? (double)(size - 1) : 0.0;
		const double firstD = lo < 0.0 ? 0.0 : (lo > lastFirst ? lastFirst : lo);
		const double end = size > 1 ? (double)size : 1.0;
		const double lastD = hi < firstD + 1.0 ? firstD + 1.0 : (hi > end ? end : hi);
		first = (int)firstD;
		count = (int)lastD - first;
	};
	PPViewport viewport;
	span(rect[0], rect[2], width, viewport.left, viewport.width);
	span(rect[1], rect[3], height, viewport.top, viewport.height);
	return viewport;
}

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
	// [SHOCKWAVE] Heat sources draw while r_heatrefraction is on and blast ripples while r_shockwave is; with neither for this
	// eye it returns here, and with no ripple published every draw below is exactly the heat pass's.
	const int set = (eyeSets >= 2 && currentEye == 1) ? 1 : 0;
	const bool heatDraws = counts[set] > 0 && r_heatrefraction;
	const bool shockDraws = shockCounts[set] > 0 && r_shockwave;
	if (eyeSets <= 0 || (!heatDraws && !shockDraws) || sceneWidth <= 0 || sceneHeight <= 0)
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
	if (heatDraws)
	{
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
	}

	// [SHOCKWAVE] Pass 1b: each blast ripple adds its shift, after the heat sources, over its own rectangle (ShockwaveViewport
	// above). When no heat source drew, the first ripple is this eye's first draw: it covers the whole viewport with no blend,
	// which is the clear. Both eyes of a multiview scene hold the same ripples (SetupShockwaves), so they draw the same number
	// of times; only the rectangles differ.
	if (shockDraws)
	{
		renderstate->PushGroup("pp.shockwave");
		for (int i = 0; i < shockCounts[set]; i++)
		{
			const bool first = !heatDraws && i == 0;
			const PPViewport viewport = first ? OffsetViewport : ShockwaveViewport(shockRects[set][i], OffsetViewport.width, OffsetViewport.height);

			ShockwaveUniforms u = shockwaves[set][i];
			u.SceneScale = sceneScale;
			u.SceneOffset = sceneOffset;
			u.RectScale = FVector2((float)viewport.width / (float)OffsetViewport.width, (float)viewport.height / (float)OffsetViewport.height);
			u.RectOffset = FVector2((float)viewport.left / (float)OffsetViewport.width, (float)viewport.top / (float)OffsetViewport.height);

			renderstate->Clear();
			renderstate->Shader = multisampled ? &ShockwaveShaderMS : &ShockwaveShader;
			renderstate->Uniforms.Set(u);
			renderstate->Viewport = viewport;
			renderstate->SetInputSceneDepth(0);
			renderstate->SetOutputTexture(&OffsetTexture);
			if (first)
				renderstate->SetNoBlend();
			else
				renderstate->SetAdditiveBlend();
			renderstate->Draw();
		}
		renderstate->PopGroup();
	}

	// Pass 2: bend the image. Over the whole screen viewport, as the lens pass, because
	// the next pipeline image starts undefined.
	HeatWarpUniforms w = {};
	w.SceneScale = sceneScale;
	w.SceneOffset = sceneOffset;
	w.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	w.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);
	w.MaxShift = MAX_SHIFT;
	w.DepthMargin = DEPTH_MARGIN;

	// [SHOCKWAVE] A frame whose blast ripples ask for a colour fringe bends with the CHROMATIC program: one value for the frame
	// (SetupShockwaves), so both eyes run the same program. Every other frame takes the plain program with HEAD's uniforms.
	const bool chromatic = shockDraws && shockChroma > 0.0f;
	renderstate->PushGroup("pp.heatwarp");
	renderstate->Clear();
	if (chromatic)
	{
		HeatWarpChromaUniforms wc = {};
		wc.SceneScale = w.SceneScale;
		wc.SceneOffset = w.SceneOffset;
		wc.LinearizeDepthA = w.LinearizeDepthA;
		wc.LinearizeDepthB = w.LinearizeDepthB;
		wc.MaxShift = w.MaxShift;
		wc.DepthMargin = w.DepthMargin;
		wc.Chroma = shockChroma;
		renderstate->Shader = multisampled ? &WarpChromaShaderMS : &WarpChromaShader;
		renderstate->Uniforms.Set(wc);
	}
	else
	{
		renderstate->Shader = multisampled ? &WarpShaderMS : &WarpShader;
		renderstate->Uniforms.Set(w);
	}
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
	// [13e] The transmittance curve and the beam scatter. A PPTexture takes no memory until a draw first uses it, so
	// with no beams or cones in the smoke these stay unmade.
	CurveTexture = { HalfViewport.width, HalfViewport.height, PixelFormat::Rgba16f };
	BeamTexture = { HalfViewport.width, HalfViewport.height, PixelFormat::Rgba16f };
	// [SMOKE_TEMPORAL] The temporal march and each eye's history and depth: no memory until accumulation first runs. A new
	// size starts every eye's history again.
	TemporalMarchTexture = { HalfViewport.width, HalfViewport.height, PixelFormat::Rgba32f };
	for (int eye = 0; eye < 2; eye++)
	{
		TemporalHistory[eye] = { HalfViewport.width, HalfViewport.height, PixelFormat::Rgba16f };
		TemporalPreviousDepth[eye] = { HalfViewport.width, HalfViewport.height, PixelFormat::R32f };
		temporalEyes[eye].Valid = false;
	}
	// [SHAREDMARCH] E2: the shared march both eyes carry from. No memory until one runs; a new size starts it again, so no
	// eye can carry from a texture of the wrong size.
	SharedMarchTexture = { HalfViewport.width, HalfViewport.height, PixelFormat::Rgba32f };
	sharedReady = false;

	lastWidth = sceneWidth;
	lastHeight = sceneHeight;
}

// [SMOKE_TEMPORAL] A 4x4 inverse in double (cofactors; the same formula serves column- and row-major arrays). False when the
// matrix is singular: no history is then trusted.
static bool SmokeTemporalInverse(const double m[16], double out[16])
{
	double inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (!(std::abs(det) > 1e-300))
		return false;
	for (int i = 0; i < 16; i++)
		out[i] = inv[i] / det;
	return true;
}

// [SHAREDMARCH] E2: the carry's matrices (see PPSharedMarchWarp, hw_postprocess.h), worked out in double so the eye's world
// position -- tens of thousands of units -- cancels before anything becomes a float, exactly as PrepareTemporal does below.
// False when either view matrix is singular: the caller then marches this eye itself, as it did before this step.
//
// General: it knows nothing of smoke or of volumes. Both callers hand it their own eye's view and the shared view the
// shared march was actually drawn from, which on the second eye of a frame is the first eye's.
static bool SharedMarchBuildWarp(const float eyeViewToWorld[16], FVector2 eyeTanHalfFov, FVector2 eyeProjOffset,
	const float sharedViewToWorld[16], FVector2 sharedTanHalfFov, FVector2 sharedProjOffset, SharedMarchWarpUniforms &out)
{
	double eye[16], centre[16], eyeInverse[16], centreInverse[16];
	for (int i = 0; i < 16; i++)
	{
		eye[i] = eyeViewToWorld[i];
		centre[i] = sharedViewToWorld[i];
	}
	if (!SmokeTemporalInverse(eye, eyeInverse) || !SmokeTemporalInverse(centre, centreInverse))
		return false;

	out = SharedMarchWarpUniforms();
	for (int column = 0; column < 4; column++)
	{
		for (int row = 0; row < 4; row++)
		{
			double toShared = 0.0, toEye = 0.0;
			for (int k = 0; k < 4; k++)
			{
				toShared += centreInverse[k * 4 + row] * eye[column * 4 + k];
				toEye += eyeInverse[k * 4 + row] * centre[column * 4 + k];
			}
			out.EyeToShared[column * 4 + row] = (float)toShared;
			out.SharedToEye[column * 4 + row] = (float)toEye;
		}
	}
	out.TanHalfFov = eyeTanHalfFov;
	out.ProjOffset = eyeProjOffset;
	out.SharedTanHalfFov = sharedTanHalfFov;
	out.SharedProjOffset = sharedProjOffset;
	out.DepthTolerance = SHARED_MARCH_DEPTH_TOLERANCE;
	out.DepthToleranceUnits = SHARED_MARCH_DEPTH_TOLERANCE_UNITS;
	return true;
}

// [SHAREDMARCH] E2: the carry, one draw over the whole viewport with no blend, which rewrites whatever the last eye or
// frame left in the texture (post-process attachments are loaded, never cleared).
void PPSharedMarchWarp::Render(PPRenderState *renderstate, const PPViewport &viewport, PPTexture *sharedMarch, PPTexture *eyeDepth, const SharedMarchWarpUniforms &uniforms)
{
	if (viewport.width != lastWidth || viewport.height != lastHeight)
	{
		Texture = { viewport.width, viewport.height, PixelFormat::Rgba32f };
		lastWidth = viewport.width;
		lastHeight = viewport.height;
	}

	renderstate->Clear();
	renderstate->Shader = &Shader;
	renderstate->Uniforms.Set(uniforms);
	renderstate->Viewport = viewport;
	renderstate->SetInputTexture(0, sharedMarch);
	renderstate->SetInputTexture(1, eyeDepth);
	renderstate->SetOutputTexture(&Texture);
	renderstate->SetNoBlend();
	renderstate->Draw();
}

// [SMOKE_TEMPORAL] This eye's temporal march and resolve uniforms (see PPSmokeVolume), and this frame recorded as its history.
bool PPSmokeVolume::PrepareTemporal(int eye, const SmokeMarchUniforms &march, SmokeTemporalMarchUniforms &marchOut, SmokeTemporalResolveUniforms &resolveOut)
{
	TemporalEye &history = temporalEyes[eye];
	const uint64_t frame = screen->FrameCount;

	double current[16], previous[16], previousInverse[16];
	for (int i = 0; i < 16; i++)
	{
		current[i] = march.ViewToWorld[i];
		previous[i] = history.ViewToWorld[i];
	}

	// The history is this eye's frame right before this one, at this size, in textures the backend still holds -- and the
	// camera did not cut: an eye that moved or turned further in one frame than a head or a player does starts again.
	bool valid = history.Valid && history.Frame + 1 == frame && history.Width == HalfViewport.width && history.Height == HalfViewport.height &&
		TemporalHistory[eye].Backend != nullptr && TemporalPreviousDepth[eye].Backend != nullptr;
	if (valid)
	{
		// ViewToWorld's translation is the eye; its third column the view's depth axis in the world (normalised: the view
		// matrix carries the pixel stretch).
		const double dx = current[12] - previous[12], dy = current[13] - previous[13], dz = current[14] - previous[14];
		const double lengthNow = std::sqrt(current[8] * current[8] + current[9] * current[9] + current[10] * current[10]);
		const double lengthThen = std::sqrt(previous[8] * previous[8] + previous[9] * previous[9] + previous[10] * previous[10]);
		const double facing = (lengthNow > 0.0 && lengthThen > 0.0) ?
			(current[8] * previous[8] + current[9] * previous[9] + current[10] * previous[10]) / (lengthNow * lengthThen) : -1.0;
		if (!(dx * dx + dy * dy + dz * dz <= TEMPORAL_CUT_DISTANCE * TEMPORAL_CUT_DISTANCE) || !(facing >= TEMPORAL_CUT_COSINE) ||
			!SmokeTemporalInverse(previous, previousInverse))
			valid = false;
	}

	// The march: this eye's own, then the jitter's turn -- the golden-ratio sequence, the second eye half a turn apart, so each
	// eye's frames fall between its earlier ones and the two eyes between each other's. Both blocks start zeroed, so every byte
	// pushed is defined.
	marchOut = SmokeTemporalMarchUniforms();
	resolveOut = SmokeTemporalResolveUniforms();
	memcpy(&marchOut, &march, sizeof(SmokeMarchUniforms));
	const double turn = (double)history.Sequence * 0.6180339887498949 + (eye == 1 ? 0.5 : 0.0);
	marchOut.JitterOffset = (float)(turn - std::floor(turn));

	// The resolve: this eye's ray terms, and last frame's view of it -- previous view <- world <- this view, in double, so the
	// eye's world position (tens of thousands of units) cancels before anything becomes a float.
	resolveOut.TanHalfFov = march.TanHalfFov;
	resolveOut.ProjOffset = march.ProjOffset;
	resolveOut.HistoryValid = valid ? 1 : 0;
	if (valid)
	{
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				double value = 0.0;
				for (int k = 0; k < 4; k++)
					value += previousInverse[k * 4 + row] * current[column * 4 + k];
				resolveOut.CurrentToPrevious[column * 4 + row] = (float)value;
			}
		}
		resolveOut.PreviousTanHalfFov = history.TanHalfFov;
		resolveOut.PreviousProjOffset = history.ProjOffset;
	}
	else
	{
		for (int i = 0; i < 16; i++)
			resolveOut.CurrentToPrevious[i] = (i % 5 == 0) ? 1.0f : 0.0f;
		resolveOut.PreviousTanHalfFov = march.TanHalfFov;
		resolveOut.PreviousProjOffset = march.ProjOffset;
	}

	// This frame is the next frame's history.
	history.Valid = true;
	history.Frame = frame;
	history.Sequence++;
	history.Width = HalfViewport.width;
	history.Height = HalfViewport.height;
	memcpy(history.ViewToWorld, march.ViewToWorld, sizeof(history.ViewToWorld));
	history.TanHalfFov = march.TanHalfFov;
	history.ProjOffset = march.ProjOffset;
	return valid;
}

void PPSmokeVolume::Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight)
{
	// SKIPPED, NOT ZERO: nothing published for this eye means no group, no texture and no draw, so
	// the frame is the frame without this pass. Set 1 only exists for the second eye of a multiview
	// scene.
	const int set = (eyeSets >= 2 && currentEye == 1) ? 1 : 0;
	transmittanceReady = false;	// [13e] this eye's own answer, set below once its curve is drawn
	if (eyeSets <= 0 || !r_smoke || sceneWidth <= 0 || sceneHeight <= 0)
		return;

	const bool multisampled = gl_multisample > 1;
	UpdateTextures(sceneWidth, sceneHeight);

	// [13e] Beams in the smoke with either beam switch on; a cone to dim with r_smoke_cones_depth on. Neither: every draw
	// below is 13d's.
	const bool beamsHere = beams.BeamCount > 0 && (beams.Scatter || beams.Depth);
	const bool conesHere = beams.Cones && hw_postprocess.volbeam.HasCones();
	// [EMISSIVEVOLUMES] An emissive volume drawn this eye reads the curve too (PPEmissiveVolumes). No volume: as above.
	const bool volumesHere = hw_postprocess.emissivevolumes.HasVolumes();
	const bool curveHere = beamsHere || conesHere || volumesHere;

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

	// [SMOKE_TEMPORAL] With r_smoke_temporal on (and the debug slice off) this eye's march turns its jitter and writes the
	// temporal march texture, and 2b-2d below blend it with the eye's own history into MarchTexture: the blur, the curve,
	// the composite and every pass after them read the accumulated march. Off: HEAD's draws exactly.
	const int temporalEye = currentEye == 1 ? 1 : 0;
	const bool temporal = r_smoke_temporal && marches[set].DebugSlice == 0;
	SmokeTemporalMarchUniforms temporalMarch = {};
	SmokeTemporalResolveUniforms temporalResolve = {};
	if (temporal)
		PrepareTemporal(temporalEye, marches[set], temporalMarch, temporalResolve);

	// [SHAREDMARCH] E2: ONE MARCH FOR BOTH EYES ("Engine docs/SHARED_MARCH_E2_IMPL_NOTES.md"; r_effects_sharedmarch). With
	// a shared view published (a stereo frame, no debug slice), the stretch past the near split is marched ONCE -- 2s, by
	// whichever eye reaches here first this frame -- and each eye carries that into its own texels (2w). Pass 2 then marches
	// only the near shell and the carry's holes. Off, or with nothing published, every draw below is the one it always was.
	//
	// WHERE IT MEETS THE TEMPORAL HISTORY (E3): at pass 2's output and nowhere else. The carry and the fill both finish
	// before it, so what enters this eye's history is what always entered it -- this eye's march of this frame, in this
	// eye's texels, in the same texture and the same packing. The resolve, the keep and the two histories are untouched.
	//
	// The carry is built against the view the shared march was ACTUALLY drawn from, not against what this eye published: the
	// second eye of a frame publishes its own projection, but the texture it carries from is the first eye's.
	const bool sharedWanted = r_effects_sharedmarch && centreValid[set] && nearSplit > 0.0f && marches[set].DebugSlice == 0;
	const bool sharedDrawn = sharedReady && sharedFrame == screen->FrameCount && SharedMarchTexture.Backend != nullptr;
	SharedMarchWarpUniforms warp = {};
	const bool shared = sharedWanted && SharedMarchBuildWarp(marches[set].ViewToWorld, marches[set].TanHalfFov, marches[set].ProjOffset,
		(sharedDrawn ? sharedView : centreMarches[set]).ViewToWorld,
		(sharedDrawn ? sharedView : centreMarches[set]).TanHalfFov,
		(sharedDrawn ? sharedView : centreMarches[set]).ProjOffset, warp);
	SmokeSharedMarchUniforms sharedUniforms = {};
	if (shared)
	{
		memcpy(&sharedUniforms, &marches[set], sizeof(SmokeMarchUniforms));
		sharedUniforms.JitterOffset = temporalMarch.JitterOffset;
		sharedUniforms.NearSplit = nearSplit;
		sharedUniforms.FillStepCount = SharedMarchFillSteps(marches[set].StepCount);
		sharedUniforms.TemporalOut = temporal ? 1 : 0;

		if (!sharedDrawn)
		{
			// 2s. The shared march. It reads the depth of the eye that draws it -- the shared view has no scene of its own --
			//     and where that is not the other eye's depth the carry marks a hole and the fill marches it per eye.
			SmokeSharedMarchUniforms centreUniforms = sharedUniforms;
			memcpy(&centreUniforms, &centreMarches[set], sizeof(SmokeMarchUniforms));
			centreUniforms.TemporalOut = 0;	// the shared march writes the packed format, never the temporal one

			renderstate->Clear();
			renderstate->Shader = &MarchSharedShader;
			renderstate->Uniforms.Set(centreUniforms);
			renderstate->Viewport = HalfViewport;
			renderstate->SetInputTexture(0, &DepthTexture);
			renderstate->SetInputExternalImage(1, PPExternalImage::SmokeDensityLatest, PPFilterMode::Linear);
			renderstate->SetInputExternalImage(2, PPExternalImage::SmokeDensityPrevious, PPFilterMode::Linear);
			renderstate->SetInputExternalImage(3, PPExternalImage::SmokeTileActive);
			renderstate->SetInputExternalImage(4, PPExternalImage::SmokeLight, PPFilterMode::Linear);
			renderstate->SetInputExternalImage(5, PPExternalImage::SmokeLightDirection, PPFilterMode::Linear);
			renderstate->SetOutputTexture(&SharedMarchTexture);
			renderstate->SetNoBlend();
			renderstate->Draw();

			sharedView = centreMarches[set];
			sharedFrame = screen->FrameCount;
			sharedReady = true;
		}

		// 2w. The carry into this eye's texels.
		Warp.Render(renderstate, HalfViewport, &SharedMarchTexture, &DepthTexture, warp);
	}

	// 2. The march, reading the volume the compute step keeps (PPExternalImage).
	//    [SHAREDMARCH] E2: with the carry made this draw is the FILL -- the near shell this eye marches itself, put over the
	//    carried stretch, and the whole ray at a lower step count where the carry left a hole. Its inputs are the march's
	//    plus the carry at 6, and it writes the same texture in the same packing, so nothing after it changes.
	renderstate->Clear();
	if (shared)
	{
		renderstate->Shader = &MarchSharedFillShader;
		renderstate->Uniforms.Set(sharedUniforms);
	}
	else if (temporal)
	{
		renderstate->Shader = &MarchTemporalShader;
		renderstate->Uniforms.Set(temporalMarch);
	}
	else
	{
		renderstate->Shader = &MarchShader;
		renderstate->Uniforms.Set(marches[set]);
	}
	renderstate->Viewport = HalfViewport;
	renderstate->SetInputTexture(0, &DepthTexture);
	renderstate->SetInputExternalImage(1, PPExternalImage::SmokeDensityLatest, PPFilterMode::Linear);
	renderstate->SetInputExternalImage(2, PPExternalImage::SmokeDensityPrevious, PPFilterMode::Linear);
	renderstate->SetInputExternalImage(3, PPExternalImage::SmokeTileActive);
	// [13d] The light grid the compute step filled this frame (VkSmokeVolume), filtered like the density.
	renderstate->SetInputExternalImage(4, PPExternalImage::SmokeLight, PPFilterMode::Linear);
	renderstate->SetInputExternalImage(5, PPExternalImage::SmokeLightDirection, PPFilterMode::Linear);
	if (shared)
		renderstate->SetInputTexture(6, Warp.GetTexture());	// [SHAREDMARCH] E2: what the carry left in this eye's texels
	renderstate->SetOutputTexture(temporal ? &TemporalMarchTexture : &MarchTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// [SMOKE_TEMPORAL] 2b-2d, only with accumulation.
	if (temporal)
	{
		// 2b. The resolve: this frame's march (its alpha carries each texel's representative depth and change level), this
		//     frame's depth, the eye's history and the depth it was drawn against -> the accumulated march in MarchTexture.
		renderstate->Clear();
		renderstate->Shader = &TemporalResolveShader;
		renderstate->Uniforms.Set(temporalResolve);
		renderstate->Viewport = HalfViewport;
		renderstate->SetInputTexture(0, &TemporalMarchTexture);
		renderstate->SetInputTexture(1, &DepthTexture);
		renderstate->SetInputTexture(2, &TemporalHistory[temporalEye]);
		renderstate->SetInputTexture(3, &TemporalPreviousDepth[temporalEye]);
		renderstate->SetOutputTexture(&MarchTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();

		// 2c, 2d. What the next frame reprojects: the resolved march before the blur, and the depth it was drawn against.
		for (int keep = 0; keep < 2; keep++)
		{
			renderstate->Clear();
			renderstate->Shader = &TemporalKeepShader;
			renderstate->Viewport = HalfViewport;
			renderstate->SetInputTexture(0, keep == 0 ? &MarchTexture : &DepthTexture);
			renderstate->SetOutputTexture(keep == 0 ? &TemporalHistory[temporalEye] : &TemporalPreviousDepth[temporalEye]);
			renderstate->SetNoBlend();
			renderstate->Draw();
		}
	}

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

	// [13e] 3a and 3b, in their own group (the perf log sums the two pp.smoke groups of one eye).
	SmokeBeamCompositeUniforms beamComposite = {};
	if (curveHere)
	{
		renderstate->PopGroup();
		renderstate->PushGroup("pp.smokebeams");

		// 3a. The transmittance curve: the march's own stretch of each ray, marched again without light, reading the
		//     volume as the march does (inputs 0-5) and the blurred march (6) so a clear texel costs one fetch. With no
		//     cone to read it, only texels a listed beam's glow can reach (7, the beam list): its cost follows the beams.
		SmokeBeamScatterUniforms curve = {};
		memcpy(&curve, &marches[set], sizeof(SmokeMarchUniforms));
		// [GOVERNOR] E8, PROTECTED LOOKS: the curve walks the same ray for the beams, the cones and the emissive volumes, so it
		// marches at the step count SetupSmokeVolume published for them (the owner's own r_smoke_steps) even while the effects
		// budget governor trims the smoke's march. Nothing else of the copied march block changes and no shader does: the curve
		// and scatter programs read the StepCount they always read. 0 (nothing published) leaves the march's own, as before.
		if (beams.ScatterStepCount > 0)
			curve.StepCount = beams.ScatterStepCount;
		curve.BeamCount = beams.BeamCount;
		curve.NearBeamsOnly = conesHere ? 0.0f : 1.0f;

		renderstate->Clear();
		// [EMISSIVEVOLUMES] With a volume drawn this eye: the SMOKE_CURVE_NEAR_VOLUMES variant -- the same curve, marched where a
		// beam OR a volume can read it (everywhere while a cone reads it: NearBeamsOnly 0, as CurveShader), binding 8 the volume
		// list. It also stands the volume list in for the beam list at binding 7 when there are no beams, whose image the smoke
		// backend makes only for beams. No volume: CurveShader, as before.
		if (volumesHere)
		{
			SmokeCurveNearVolumesUniforms nearVolumes = {};
			memcpy(&nearVolumes, &curve, sizeof(SmokeBeamScatterUniforms));
			hw_postprocess.emissivevolumes.FillCurveUniforms(nearVolumes);
			renderstate->Shader = &CurveNearVolumesShader;
			renderstate->Uniforms.Set(nearVolumes);
		}
		else
		{
			renderstate->Shader = &CurveShader;
			renderstate->Uniforms.Set(curve);
		}
		renderstate->Viewport = HalfViewport;
		renderstate->SetInputTexture(0, &DepthTexture);
		renderstate->SetInputExternalImage(1, PPExternalImage::SmokeDensityLatest, PPFilterMode::Linear);
		renderstate->SetInputExternalImage(2, PPExternalImage::SmokeDensityPrevious, PPFilterMode::Linear);
		renderstate->SetInputExternalImage(3, PPExternalImage::SmokeTileActive);
		renderstate->SetInputExternalImage(4, PPExternalImage::SmokeLight, PPFilterMode::Linear);
		renderstate->SetInputExternalImage(5, PPExternalImage::SmokeLightDirection, PPFilterMode::Linear);
		renderstate->SetInputTexture(6, &MarchTexture);
		// [13e] The beam list only while it holds beams. The backend makes that image the first time beams meet the smoke
		// (VkSmokeVolume::UploadBeams) and lets it go with the volume, and a draw naming a backend image that is not ready is
		// skipped whole (VkPPRenderState::Draw): a cone in smoke before any beam then read a curve this frame never drew
		// ("Engine docs/SMOKE_13E_BEAMLIST_FIX_NOTES.md"). With no beam the program reads nothing at 7 (NearAnyBeam runs only
		// with NearBeamsOnly and loops BeamCount times), so BlurTexture -- this eye's own half-resolution float texture, drawn
		// just above and bound nowhere else in this draw -- fills the binding. With beams: the list, as before.
		if (beams.BeamCount > 0)
			renderstate->SetInputExternalImage(7, PPExternalImage::SmokeBeams);
		else
			renderstate->SetInputTexture(7, &BlurTexture);
		if (volumesHere)
		{
			if (beams.BeamCount <= 0)
				renderstate->SetInputExternalImage(7, PPExternalImage::EmissiveVolumeList);	// [EMISSIVEVOLUMES] the stand-in: no beam is read
			renderstate->SetInputExternalImage(8, PPExternalImage::EmissiveVolumeList);	// [EMISSIVEVOLUMES]
		}
		renderstate->SetOutputTexture(&CurveTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
		transmittanceReady = true;

		// 3b. The light the beams scatter in the smoke (written clear when r_smoke_beams is off, so the composite's
		//     input is always this frame's). Inputs as the curve, then 7 the curve and 8 the beam list.
		if (beamsHere)
		{
			SmokeBeamScatterUniforms scatter = {};
			memcpy(&scatter, &marches[set], sizeof(SmokeMarchUniforms));
			// [GOVERNOR] E8, PROTECTED LOOKS: what a beam scatters in the smoke is marched at the owner's own step count at every
			// rung (PPSmokeBeamSettings::ScatterStepCount), so no rung changes a grab laser's or the Lance's look.
			if (beams.ScatterStepCount > 0)
				scatter.StepCount = beams.ScatterStepCount;
			scatter.BeamCount = beams.BeamCount;
			scatter.BeamScatter = beams.Scatter ? clamp(beams.LookScatter, 0.0f, 1.0f) : 0.0f;

			renderstate->Clear();
			renderstate->Shader = &BeamScatterShader;
			renderstate->Uniforms.Set(scatter);
			renderstate->Viewport = HalfViewport;
			renderstate->SetInputTexture(0, &DepthTexture);
			renderstate->SetInputExternalImage(1, PPExternalImage::SmokeDensityLatest, PPFilterMode::Linear);
			renderstate->SetInputExternalImage(2, PPExternalImage::SmokeDensityPrevious, PPFilterMode::Linear);
			renderstate->SetInputExternalImage(3, PPExternalImage::SmokeTileActive);
			renderstate->SetInputExternalImage(4, PPExternalImage::SmokeLight, PPFilterMode::Linear);
			renderstate->SetInputExternalImage(5, PPExternalImage::SmokeLightDirection, PPFilterMode::Linear);
			renderstate->SetInputTexture(6, &MarchTexture);
			renderstate->SetInputTexture(7, &CurveTexture);
			renderstate->SetInputExternalImage(8, PPExternalImage::SmokeBeams);
			renderstate->SetOutputTexture(&BeamTexture);
			renderstate->SetNoBlend();
			renderstate->Draw();

			beamComposite.SceneScale = depth.SceneScale;
			beamComposite.SceneOffset = depth.SceneOffset;
			beamComposite.LinearizeDepthA = depth.LinearizeDepthA;
			beamComposite.LinearizeDepthB = depth.LinearizeDepthB;
			memcpy(beamComposite.ViewToWorld, marches[set].ViewToWorld, sizeof(beamComposite.ViewToWorld));
			beamComposite.TanHalfFov = marches[set].TanHalfFov;
			beamComposite.ProjOffset = marches[set].ProjOffset;
			beamComposite.BoxMin = marches[set].BoxMin;
			beamComposite.BeamCount = beams.BeamCount;
			beamComposite.BeamScrollSpeed = beams.ScrollSpeed;
			beamComposite.BeamScrollDepth = beams.ScrollDepth;
			beamComposite.BeamTimer = beams.Timer;
			beamComposite.BeamDepth = beams.Depth ? 1 : 0;
		}

		renderstate->PopGroup();
		renderstate->PushGroup("pp.smoke");
	}

	// 4. Up to full resolution and onto the image: scene x T + light, premultiplied. It writes the
	//    current pipeline image in place and reads it nowhere, so no pipeline image advances.
	//    [13e] With beams: + the beam scatter and the beams' own glow restored where the haze was behind them
	//    (inputs 3-5: the beam scatter, the curve, the beam list). Still (light, 1 - T), premultiplied.
	renderstate->Clear();
	if (beamsHere)
	{
		renderstate->Shader = multisampled ? &CompositeBeamsShaderMS : &CompositeBeamsShader;
		renderstate->Uniforms.Set(beamComposite);
	}
	else
	{
		renderstate->Shader = multisampled ? &CompositeShaderMS : &CompositeShader;
		renderstate->Uniforms.Set(depth);
	}
	renderstate->Viewport = screen->mSceneViewport;
	renderstate->SetInputTexture(0, &MarchTexture);
	renderstate->SetInputTexture(1, &DepthTexture);
	renderstate->SetInputSceneDepth(2);
	if (beamsHere)
	{
		renderstate->SetInputTexture(3, &BeamTexture);
		renderstate->SetInputTexture(4, &CurveTexture);
		renderstate->SetInputExternalImage(5, PPExternalImage::SmokeBeams);
	}
	renderstate->SetOutputCurrent();
	renderstate->SetPremultipliedAlphaBlend();
	renderstate->Draw();

	renderstate->PopGroup();

	// [LIGHTMASK] The light mask is dimmed by the same smoke ("Engine docs/EMISSIVE_BLOOM_PLAN.md" 2e,
	// contract 4): the same composite with LIGHT_MASK_CARRY, the same inputs, uniforms and blend, onto
	// the mask in place. It adds no light of either class, so every amount becomes amount x T with the
	// colour's own T. Both eyes of a layered post path take this branch alike (the frame's state).
	// [13e] With beams, the carry adds the pinned light the composite added -- the beam scatter and the restored glow --
	// to G ("Engine docs/EMISSIVE_BLOOM_PLAN.md" 2e: the smoke row's pinned beam scatter).
	if (hw_postprocess.lightmask.PostInputValid())
	{
		renderstate->PushGroup("pp.lightmaskcarry");
		renderstate->Clear();
		if (beamsHere)
		{
			renderstate->Shader = multisampled ? &MaskCarryBeamsShaderMS : &MaskCarryBeamsShader;
			renderstate->Uniforms.Set(beamComposite);
		}
		else
		{
			renderstate->Shader = multisampled ? &MaskCarryShaderMS : &MaskCarryShader;
			renderstate->Uniforms.Set(depth);
		}
		renderstate->Viewport = screen->mSceneViewport;
		renderstate->SetInputTexture(0, &MarchTexture);
		renderstate->SetInputTexture(1, &DepthTexture);
		renderstate->SetInputSceneDepth(2);
		if (beamsHere)
		{
			renderstate->SetInputTexture(3, &BeamTexture);
			renderstate->SetInputTexture(4, &CurveTexture);
			renderstate->SetInputExternalImage(5, PPExternalImage::SmokeBeams);
		}
		renderstate->SetOutputLightMaskCurrent();
		renderstate->SetPremultipliedAlphaBlend();
		renderstate->Draw();
		renderstate->PopGroup();
	}
}

/////////////////////////////////////////////////////////////////////////////

// [SCREENTILES] E5: a pass's tile mask -- see PPScreenTileMask (hw_postprocess.h), hw_screentiles.h and shaders/pp/screentilemask.fp.

void PPScreenTileMask::Render(PPRenderState *renderstate, int width, int height, const uint32_t rects[SCREEN_TILE_ITEMS_MAX])
{
	const int tilesX = ScreenTileCount(width);
	const int tilesY = ScreenTileCount(height);
	if (tilesX != lastTilesX || tilesY != lastTilesY)
	{
		Viewport.left = 0;
		Viewport.top = 0;
		Viewport.width = tilesX;
		Viewport.height = tilesY;
		Texture = { tilesX, tilesY, PixelFormat::Rgba8 };
		lastTilesX = tilesX;
		lastTilesY = tilesY;
	}

	ScreenTileMaskUniforms uniforms;
	memcpy(&uniforms, rects, sizeof(uniforms));	// the rows are the rectangles in list order (hw_postprocess.h)

	// One texel a tile over the whole texture, no blend: every texel is rewritten.
	renderstate->Clear();
	renderstate->Shader = &Shader;
	renderstate->Uniforms.Set(uniforms);
	renderstate->Viewport = Viewport;
	renderstate->SetOutputTexture(&Texture);
	renderstate->SetNoBlend();
	renderstate->Draw();
}

/////////////////////////////////////////////////////////////////////////////

// [EMISSIVEVOLUMES] The emissive volumes' drawing: see PPEmissiveVolumes (hw_postprocess.h) and shaders/pp/emissivevolume.fp.

void PPEmissiveVolumes::UpdateTextures(int sceneWidth, int sceneHeight, int resolution)
{
	if (sceneWidth == lastWidth && sceneHeight == lastHeight && resolution == lastResolution)
		return;

	MarchViewport.left = 0;
	MarchViewport.top = 0;
	MarchViewport.width = resolution == 1 ? sceneWidth : (sceneWidth + 1) / 2;
	MarchViewport.height = resolution == 1 ? sceneHeight : (sceneHeight + 1) / 2;
	// R32F: the linear depth each texel marches to. RGBA16F: rgb light, a transmittance.
	DepthTexture = { MarchViewport.width, MarchViewport.height, PixelFormat::R32f };
	MarchTexture = { MarchViewport.width, MarchViewport.height, PixelFormat::Rgba16f };
	BlurTexture = { MarchViewport.width, MarchViewport.height, PixelFormat::Rgba16f };

	lastWidth = sceneWidth;
	lastHeight = sceneHeight;
	lastResolution = resolution;
}

void PPEmissiveVolumes::Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight)
{
	// SKIPPED, NOT ZERO: nothing published for this eye means no group, no texture and no draw.
	if (eyeSets <= 0 || settings.Count <= 0 || sceneWidth <= 0 || sceneHeight <= 0)
		return;

	const int set = CurrentSet();
	const bool multisampled = gl_multisample > 1;
	const int resolution = settings.Resolution <= 1 ? 1 : 2;
	UpdateTextures(sceneWidth, sceneHeight, resolution);

	// This eye's smoke drew its depth and transmittance curve just before (Pass1 order): the march is dimmed by the haze in
	// front of each step, and at half resolution it marches to the smoke's own depth texture.
	PPSmokeVolume &smoke = hw_postprocess.smokevolume;
	const bool inSmoke = smoke.TransmittanceReady();
	const bool sharedDepth = inSmoke && resolution == 2;
	// [EMISSIVETILES] E5 (r_emissivevolumes_tiles): the march loops only its tile's list when this eye set published tile bounds for
	// this frame's list and the march's target fits the packed tiles. Not for one volume out of haze: its rectangle already is its
	// tile list, so the plain march does less there. The image is the same either way.
	const bool tiled = r_emissivevolumes_tiles && settings.Count <= SCREEN_TILE_ITEMS_MAX && tileBoundCounts[set] == settings.Count &&
		(settings.Count > 1 || inSmoke) && ScreenTilesFit(MarchViewport.width, MarchViewport.height);

	// One line whenever the variant, the eye arrangement or the textures change, so a test log shows which ran.
	{
		static int loggedMultisample = -1, loggedSets = -1, loggedWidth = -1, loggedHeight = -1, loggedSmoke = -1;
		if (loggedMultisample != (int)multisampled || loggedSets != eyeSets || loggedWidth != MarchViewport.width ||
			loggedHeight != MarchViewport.height || loggedSmoke != (int)inSmoke)
		{
			loggedMultisample = (int)multisampled;
			loggedSets = eyeSets;
			loggedWidth = MarchViewport.width;
			loggedHeight = MarchViewport.height;
			loggedSmoke = (int)inSmoke;
			Printf("emissive volumes: drawing -- %s depth read, %s, %s, march %dx%d\n",
				multisampled ? "MULTISAMPLE" : "single-sample",
				eyeSets >= 2 ? "a march set per eye (multiview scene)" : "one march set (each eye draws its own scene)",
				inSmoke ? "dimmed by the haze in front" : "no smoke curve", MarchViewport.width, MarchViewport.height);
		}
	}

	SmokeDepthUniforms depth = {};
	depth.SceneScale = screen->SceneScale();
	depth.SceneOffset = screen->SceneOffset();
	depth.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	depth.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);

	renderstate->PushGroup("pp.emissive");

	// 1. The depth to march to. Every draw below writes the whole viewport of its target with no blend, which clears what the
	//    last eye or frame left there.
	PPTexture *depthTexture = sharedDepth ? smoke.GetDepthTexture() : &DepthTexture;
	if (!sharedDepth)
	{
		renderstate->Clear();
		renderstate->Shader = multisampled ? &DepthShaderMS : &DepthShader;
		renderstate->Uniforms.Set(depth);
		renderstate->Viewport = MarchViewport;
		renderstate->SetInputSceneDepth(0);
		renderstate->SetOutputTexture(&DepthTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// [SHAREDMARCH] E2: ONE MARCH FOR BOTH EYES ("Engine docs/SHARED_MARCH_E2_IMPL_NOTES.md"; r_effects_sharedmarch). The
	// volumes further than SHARED_MARCH_NEAR_UNITS from the head are marched ONCE, from the view between the eyes, and each
	// eye carries that into its own texels; pass 2 then marches only the NEAR volumes -- your own muzzle flash -- and puts
	// them over the carry, or marches the whole list where the carry left a hole.
	//
	// THE TILE LISTS ARE THE SPLIT (E5). The shared march's mask is built from the FAR volumes' bounds in the SHARED view;
	// the fill's from the NEAR volumes' in this eye's. A volume left out of a mask is a volume that pass never loops, so the
	// split needed no new uniform, no second list and no change to the march's own loop. That is also why the shared path
	// asks for the tile lists: with r_emissivevolumes_tiles off there is nothing to carry the split, and the pass marches per
	// eye as before.
	//
	// Decided here, before any mask is drawn, so the mask below is always the one the program that reads it expects.
	const bool sharedWanted = r_effects_sharedmarch && tiled && sharedSplits[set] && sharedCounts[set] == settings.Count;
	if (sharedWanted && (MarchViewport.width != lastSharedWidth || MarchViewport.height != lastSharedHeight))
	{
		SharedMarchTexture = { MarchViewport.width, MarchViewport.height, PixelFormat::Rgba32f };
		lastSharedWidth = MarchViewport.width;
		lastSharedHeight = MarchViewport.height;
		sharedReady = false;
	}
	const bool sharedDrawn = sharedReady && sharedFrame == screen->FrameCount && SharedMarchTexture.Backend != nullptr;
	SharedMarchWarpUniforms warp = {};
	const bool shared = sharedWanted && SharedMarchBuildWarp(marches[set].ViewToWorld, marches[set].TanHalfFov, marches[set].ProjOffset,
		(sharedDrawn ? sharedView : centreMarches[set]).ViewToWorld,
		(sharedDrawn ? sharedView : centreMarches[set]).TanHalfFov,
		(sharedDrawn ? sharedView : centreMarches[set]).ProjOffset, warp);

	// 1b. [EMISSIVETILES] E5: the tile mask -- each listed volume's widened bound cut into tiles at the march's size.
	//     [SHAREDMARCH] E2: the NEAR volumes' bounds instead, when the carry is serving the far ones.
	if (tiled)
	{
		uint32_t rects[SCREEN_TILE_ITEMS_MAX];
		for (int i = 0; i < SCREEN_TILE_ITEMS_MAX; i++)
			rects[i] = i < settings.Count ? ScreenTileRectPack(shared ? nearTileBounds[set][i] : tileBounds[set][i], MarchViewport.width, MarchViewport.height) : SCREEN_TILE_RECT_NONE;
		TileMask.Render(renderstate, MarchViewport.width, MarchViewport.height, rects);

		if (PerfLog::GroupsWanted())
		{
			// What the tiles list against what the rectangle would have looped (every texel in it, every drawn volume).
			const int tilesX = ScreenTileCount(MarchViewport.width), tilesY = ScreenTileCount(MarchViewport.height);
			const ScreenTileCoverage listed = ScreenTileCoverageOf(rects, settings.Count, tilesX, tilesY);
			ScreenTexRect rectangle;
			rectangle.Lo[0] = marches[set].RectMin.X;
			rectangle.Lo[1] = marches[set].RectMin.Y;
			rectangle.Hi[0] = marches[set].RectMax.X;
			rectangle.Hi[1] = marches[set].RectMax.Y;
			const uint32_t rectangleTiles = ScreenTileRectPack(rectangle, MarchViewport.width, MarchViewport.height);
			EmissiveVolumeFrameStats &stats = EmissiveVolumeStats();
			stats.TileEyes++;
			stats.Tiles += listed.Tiles;
			stats.TilesInRect += ScreenTileCoverageOf(&rectangleTiles, 1, tilesX, tilesY).Lit;
			stats.TilesLit += listed.Lit;
			stats.TileEntries += listed.Entries;
		}
	}

	if (shared)
	{
		if (!sharedDrawn)
		{
			// 1s. [SHAREDMARCH] E2: the FAR volumes' mask at the shared view, then the shared march. It reads the depth of the
			//     eye that draws it, and it is drawn WITHOUT the smoke's transmittance -- the shared view has no curve of its
			//     own -- so the haze in front is put on per eye by the fill, at the depth the carried light sits at.
			uint32_t farRects[SCREEN_TILE_ITEMS_MAX];
			for (int i = 0; i < SCREEN_TILE_ITEMS_MAX; i++)
				farRects[i] = i < settings.Count ? ScreenTileRectPack(centreTileBounds[set][i], MarchViewport.width, MarchViewport.height) : SCREEN_TILE_RECT_NONE;
			CentreTileMask.Render(renderstate, MarchViewport.width, MarchViewport.height, farRects);

			renderstate->Clear();
			renderstate->Shader = &MarchSharedShader;
			renderstate->Uniforms.Set(centreMarches[set]);
			renderstate->Viewport = MarchViewport;
			renderstate->SetInputTexture(0, depthTexture);
			renderstate->SetInputExternalImage(1, PPExternalImage::EmissiveVolumeList);
			renderstate->SetInputExternalImage(2, PPExternalImage::EmissiveNoise, PPFilterMode::Linear, PPWrapMode::Repeat);
			renderstate->SetInputTexture(3, CentreTileMask.GetTexture());
			renderstate->SetOutputTexture(&SharedMarchTexture);
			renderstate->SetNoBlend();
			renderstate->Draw();

			sharedView = centreMarches[set];
			sharedFrame = screen->FrameCount;
			sharedReady = true;
		}

		// 1w. [SHAREDMARCH] E2: the carry into this eye's texels.
		Warp.Render(renderstate, MarchViewport, &SharedMarchTexture, depthTexture, warp);
	}

	// 2. The march, reading the list and the noise the backend keeps (PPExternalImage), and in smoke the smoke's curve.
	//    [SHAREDMARCH] E2: with the carry made this draw is the FILL -- the near volumes its tile mask lists, put over the
	//    carry, or every volume where the carry left a hole. Same inputs, plus the carry after the tile mask.
	renderstate->Clear();
	renderstate->Shader = inSmoke ? &MarchSmokeShader : &MarchShader;
	if (tiled)	// [EMISSIVETILES] E5: the same march over its tile's list
		renderstate->Shader = inSmoke ? &MarchTilesSmokeShader : &MarchTilesShader;
	if (shared)	// [SHAREDMARCH] E2
		renderstate->Shader = inSmoke ? &MarchSharedFillSmokeShader : &MarchSharedFillShader;
	renderstate->Uniforms.Set(marches[set]);
	renderstate->Viewport = MarchViewport;
	renderstate->SetInputTexture(0, depthTexture);
	renderstate->SetInputExternalImage(1, PPExternalImage::EmissiveVolumeList);
	renderstate->SetInputExternalImage(2, PPExternalImage::EmissiveNoise, PPFilterMode::Linear, PPWrapMode::Repeat);
	if (inSmoke)
	{
		renderstate->SetInputTexture(3, smoke.GetMarchTexture());
		renderstate->SetInputTexture(4, smoke.GetDepthTexture());
		renderstate->SetInputTexture(5, smoke.GetCurveTexture());
	}
	if (tiled)	// [EMISSIVETILES] E5: the tile mask, after the march's other inputs
		renderstate->SetInputTexture(inSmoke ? 6 : 3, TileMask.GetTexture());
	if (shared)	// [SHAREDMARCH] E2: what the carry left in this eye's texels, after the tile mask
		renderstate->SetInputTexture(inSmoke ? 7 : 4, Warp.GetTexture());
	renderstate->SetOutputTexture(&MarchTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// 3. The blur that keeps to its depth: across into BlurTexture, then down back into MarchTexture.
	for (int pass = 0; pass < 2; pass++)
	{
		renderstate->Clear();
		renderstate->Shader = pass == 0 ? &BlurHorizontal : &BlurVertical;
		renderstate->Viewport = MarchViewport;
		renderstate->SetInputTexture(0, pass == 0 ? &MarchTexture : &BlurTexture);
		renderstate->SetInputTexture(1, depthTexture);
		renderstate->SetOutputTexture(pass == 0 ? &BlurTexture : &MarchTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// 4. Up to full resolution and onto the image: scene x T + light, premultiplied, in place (no pipeline image advances).
	renderstate->Clear();
	renderstate->Shader = multisampled ? &CompositeShaderMS : &CompositeShader;
	renderstate->Uniforms.Set(depth);
	renderstate->Viewport = screen->mSceneViewport;
	renderstate->SetInputTexture(0, &MarchTexture);
	renderstate->SetInputTexture(1, depthTexture);
	renderstate->SetInputSceneDepth(2);
	renderstate->SetOutputCurrent();
	renderstate->SetPremultipliedAlphaBlend();
	renderstate->Draw();

	renderstate->PopGroup();

	// 5. [LIGHTMASK] A volume that absorbs dims the light mask by its own transmittance ("Engine docs/EMISSIVE_BLOOM_PLAN.md" 2e):
	//    the same composite with LIGHT_MASK_CARRY onto the mask in place, adding no light of either class. A volume that only
	//    glows needs no carry: an additive pass after the scene leaves the pinned share falling correctly.
	if (settings.Absorbs && hw_postprocess.lightmask.PostInputValid())
	{
		renderstate->PushGroup("pp.lightmaskcarry");
		renderstate->Clear();
		renderstate->Shader = multisampled ? &MaskCarryShaderMS : &MaskCarryShader;
		renderstate->Uniforms.Set(depth);
		renderstate->Viewport = screen->mSceneViewport;
		renderstate->SetInputTexture(0, &MarchTexture);
		renderstate->SetInputTexture(1, depthTexture);
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

bool PPBloom::RenderBloom(PPRenderState *renderstate, int sceneWidth, int sceneHeight, int fixedcm)
{
	// Only bloom things if enabled and no special fixed light mode is active
	if (!gl_bloom || fixedcm != CM_DEFAULT || gl_ssao_debug || sceneWidth <= 0 || sceneHeight <= 0)
	{
		return false;	// [EXPOSUREIMPULSE] bloom did not draw for this eye
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
	return true;	// [EXPOSUREIMPULSE] bloom drew: levels[0] holds this eye's bloom
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

// ============================================================================
// [TONEMAP] THE FILMIC ROLL-OFF'S NUMBERS ("Engine docs/TONEMAP_IMPL_NOTES.md"; the curve itself is
// shaders/pp/tonemapfilmic.fp).
//
// The cvars are in the units the owner sees on screen and the curve works in linear light, so the one pow each
// way happens here, once a frame, rather than twice a pixel.  Everything is clamped here too, so a hand-edited
// ini cannot hand the shader a knee above its white point or a negative exposure.
//
// PassThrough is the promise the mode makes: while the exposure is exactly 1, a pixel whose brightest channel is
// at or below the knee leaves the pass as the bits it arrived with -- no gamma round trip, so the mid-range does
// not move by even one level.  With any other exposure every pixel is scaled and there is nothing to pass
// through, so it is set negative and the shader's test can never fire.
// ============================================================================
TonemapFilmicUniforms PPTonemap::FilmicUniforms()
{
	const float gamma = 2.2f;

	float knee = (float)gl_tonemap_knee;
	if (!(knee >= 0.30f)) knee = 0.30f;	// also catches NaN
	if (knee > 0.95f) knee = 0.95f;

	// WHITE may go ABOVE 1 on purpose.  It is the curve's asymptote in the units this image is carried in, and
	// how much of that a display path can actually show is the PRESENT pass's business, not the curve's: the
	// window's present saturates around 0.95 while the headset eye's carries usable range past 1.2 (present.fp
	// plus each path's own InvGamma, see "Engine docs/TONEMAP_IMPL_NOTES.md").  Capping white at 1 would hand
	// the headset an image that can never reach full white.
	float white = (float)gl_tonemap_white;
	if (!(white >= 0.50f)) white = 0.50f;
	if (white > 1.50f) white = 1.50f;
	if (white < knee + 0.01f) white = knee + 0.01f;	// the shoulder needs somewhere to go

	float exposure = (float)gl_tonemap_exposure;
	if (!(exposure >= 0.25f)) exposure = 0.25f;
	if (exposure > 4.0f) exposure = 4.0f;
	// SNAPPED TO EXACTLY 1 when it is within rounding of it.  A slider adds its step up in floats, so "1.00" on
	// the row can arrive as 0.99999994, and the pass-through below asks for exactly 1.  Snapping the value
	// itself rather than loosening that test keeps the scale and the pass-through agreeing about the same
	// number, so there is no step at the knee.
	if (exposure > 0.9999f && exposure < 1.0001f) exposure = 1.0f;

	float desaturate = (float)gl_tonemap_desaturate;
	if (!(desaturate >= 0.0f)) desaturate = 0.0f;
	if (desaturate > 1.0f) desaturate = 1.0f;

	TonemapFilmicUniforms uniforms = {};
	uniforms.KneeLinear = std::pow(knee, gamma);
	uniforms.WhiteLinear = std::pow(white, gamma);
	uniforms.Exposure = exposure;
	uniforms.Desaturate = desaturate;
	uniforms.PassThrough = (exposure == 1.0f) ? knee : -1.0f;
	return uniforms;
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
	// [TONEMAP] The roll-off.  Appended to the switch; every case above keeps its program.
	case ETonemapMode::Filmic:		shader = &FilmicShader; break;
	}

	renderstate->PushGroup("tonemap");

	renderstate->Clear();
	renderstate->Shader = shader;
	// [TONEMAP] Renderer-read: the settings are taken from the cvars HERE, in the frame being drawn, so a slider
	// moves the picture while the menu that owns it is open.  Only this mode has uniforms; the others are
	// handed the empty block they always were.
	if (current_tonemap == ETonemapMode::Filmic)
		renderstate->Uniforms.Set(FilmicUniforms());
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

// [PPPROJECT] Per-eye world-to-clip for custom post-process shaders' projected uniforms (hw_postprocess.h).
PPCustomShaders::EyeView PPCustomShaders::Eyes[2];
int PPCustomShaders::EyeSets = 0;
int PPCustomShaders::CurrentEye = 0;

void PPCustomShaders::SetEyeView(int set, const float *projection, const float *view)
{
	if (set < 0 || set > 1 || projection == nullptr || view == nullptr)
		return;
	float *out = Eyes[set].WorldToClip;
	for (int c = 0; c < 4; c++)
	{
		for (int r = 0; r < 4; r++)
		{
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
				sum += projection[k * 4 + r] * view[c * 4 + k];
			out[c * 4 + r] = sum;
		}
	}
	Eyes[set].FocalY = projection[5];
}

bool PPCustomShaders::ProjectWorld(int set, double worldX, double worldY, double worldZ, double &u, double &v, double &clipZ, double &focalY)
{
	if (EyeSets <= 0)
		return false;
	if (set < 0 || set >= EyeSets)
		set = 0;
	const float *m = Eyes[set].WorldToClip;
	// Game (x, y, z) -> GL world (x, z, y), as the heat and smoke passes upload positions.
	const double x = worldX, y = worldZ, z = worldY;
	const double cx = m[0] * x + m[4] * y + m[8] * z + m[12];
	const double cy = m[1] * x + m[5] * y + m[9] * z + m[13];
	const double cz = m[2] * x + m[6] * y + m[10] * z + m[14];
	const double cw = m[3] * x + m[7] * y + m[11] * z + m[15];
	if (cw == 0.0)
		return false;
	u = cx / cw * 0.5 + 0.5;
	v = cy / cw * 0.5 + 0.5;
	clipZ = cz;
	focalY = Eyes[set].FocalY;
	return true;
}

bool PP_ProjectWorldForView(double worldX, double worldY, double worldZ, double &u, double &v, double &clipZ)
{
	double focalY;
	return PPCustomShaders::ProjectWorld(0, worldX, worldY, worldZ, u, v, clipZ, focalY);
}

bool PP_GetViewFocalY(double &focalY)
{
	if (PPCustomShaders::EyeSets <= 0)
		return false;
	focalY = PPCustomShaders::Eyes[0].FocalY;
	return true;
}

/////////////////////////////////////////////////////////////////////////////

// [SCENEMASK] The only thing in the engine that ever asks for the scene mask: a loaded post-process
// shader that names a texture "SceneMask" (hw_postprocess.h, PPSceneMask). PostProcessShaders holds
// one entry per GLDEFS postprocess block, so this walks a handful of small maps once a frame.
//
// Not gated on Enabled: a mod may switch its own shader on and off from a menu, and the attachment
// should not be freed and re-made under it. Not gated on gl_custompost either -- that switches the
// DRAWS off, and a frame that draws no custom shader simply never reads what the scene wrote.
bool PPSceneMask::WantedByShaders()
{
	for (unsigned int i = 0; i < PostProcessShaders.Size(); i++)
	{
		TMap<FString, FString>::Iterator it(PostProcessShaders[i].Textures);
		TMap<FString, FString>::Pair *pair;
		while (it.NextPair(pair))
		{
			if (!pair->Value.CompareNoCase("SceneMask"))
				return true;
		}
	}
	return false;
}

void PPSceneMask::BeginFrame(bool active)
{
	FrameActive = active;
	PostInput = false;	// each eye's scene transfer says whether ITS scene drew the mask
}

/////////////////////////////////////////////////////////////////////////////

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

	bool depthResolved = false;	// [CUSTOMDEPTH] once per call, i.e. once per eye
	bool maskResolved = false;	// [SCENEMASK] the same, and for the same reason
	for (auto &shader : mShaders)
	{
		if (shader->Desc->Target == target && shader->Desc->Enabled)
		{
			if (shader->UsesSceneDepth() && !depthResolved)
			{
				ResolveSceneDepth(renderstate);
				depthResolved = true;
			}
			if (shader->UsesSceneMask() && !maskResolved)
			{
				ResolveSceneMask(renderstate);
				maskResolved = true;
			}
			shader->Run(renderstate);
		}
	}
}

// [SCENEMASK] The scene mask as a custom shader samples it: the per-pixel tag in the red channel of a
// single-sample texture over the screen viewport, so TexCoord reads the same pixel as InputTexture,
// with or without MSAA (shaders/pp/scenemask.fp). Every texel is written with no blend, which clears
// what the last eye or frame left.
//
// NEAREST, ALWAYS. With MSAA the shader takes SAMPLE 0 -- it never averages the samples and never takes
// their maximum. A tag is a name: the average of 3 and 9 is 6, which nothing drew, and the max is 9,
// which nothing drew there either. Sample 0 is a tag some fragment really wrote at that pixel.
//
// When this eye's scene drew no mask (a save picture, a camera texture, or any frame where the mask is
// not active) the NO_SCENE_MASK variant writes 0 everywhere, so a shader always reads a defined tag and
// never someone else's image.
void PPCustomShaders::ResolveSceneMask(PPRenderState *renderstate)
{
	const int width = screen->mScreenViewport.width;
	const int height = screen->mScreenViewport.height;
	if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
		return;

	if (width != mMaskWidth || height != mMaskHeight)
	{
		mResolvedMask = { width, height, PixelFormat::Rgba8 };
		mMaskWidth = width;
		mMaskHeight = height;
	}

	const bool haveMask = hw_postprocess.scenemask.PostInputValid();

	CustomDepthUniforms u = {};
	u.SceneScale = screen->SceneScale();
	u.SceneOffset = screen->SceneOffset();

	renderstate->PushGroup("pp.scenemask");
	renderstate->Clear();
	renderstate->Shader = !haveMask ? &mMaskShaderNone : (gl_multisample > 1 ? &mMaskShaderMS : &mMaskShader);
	renderstate->Uniforms.Set(u);
	renderstate->Viewport.left = 0;
	renderstate->Viewport.top = 0;
	renderstate->Viewport.width = width;
	renderstate->Viewport.height = height;
	if (haveMask) renderstate->SetInputSceneMask(0);
	else renderstate->SetInputCurrent(0, PPFilterMode::Nearest);	// a valid binding the variant ignores
	renderstate->SetOutputTexture(&mResolvedMask);
	renderstate->SetNoBlend();
	renderstate->Draw();
	renderstate->PopGroup();
}

// [CUSTOMDEPTH] The scene depth as a custom shader samples it: raw [0,1] window depth in a single-sample R32F texture over
// the screen viewport, so TexCoord reads the same pixel as InputTexture, with or without MSAA (shaders/pp/customdepth.fp).
// Every texel is written with no blend, which clears what the last eye or frame left.
void PPCustomShaders::ResolveSceneDepth(PPRenderState *renderstate)
{
	const int width = screen->mScreenViewport.width;
	const int height = screen->mScreenViewport.height;
	if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
		return;

	if (width != mDepthWidth || height != mDepthHeight)
	{
		mResolvedDepth = { width, height, PixelFormat::R32f };
		mDepthWidth = width;
		mDepthHeight = height;
	}

	CustomDepthUniforms u = {};
	u.SceneScale = screen->SceneScale();
	u.SceneOffset = screen->SceneOffset();

	renderstate->PushGroup("pp.customdepth");
	renderstate->Clear();
	renderstate->Shader = gl_multisample > 1 ? &mDepthShaderMS : &mDepthShader;
	renderstate->Uniforms.Set(u);
	renderstate->Viewport.left = 0;
	renderstate->Viewport.top = 0;
	renderstate->Viewport.width = width;
	renderstate->Viewport.height = height;
	renderstate->SetInputSceneDepth(0);
	renderstate->SetOutputTexture(&mResolvedDepth);
	renderstate->SetNoBlend();
	renderstate->Draw();
	renderstate->PopGroup();
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
		mShaders.push_back(std::make_unique<PPCustomShaderInstance>(&PostProcessShaders[i], &mLastInputTexture, &mResolvedDepth, &mResolvedMask));
	}
}

/////////////////////////////////////////////////////////////////////////////

PPCustomShaderInstance::PPCustomShaderInstance(PostProcessShader *desc, std::unique_ptr<PPPersistentBuffer> *lastInputTexture, PPTexture *resolvedDepth, PPTexture *resolvedMask) : Desc(desc), LastInputTexture(lastInputTexture), ResolvedDepth(resolvedDepth), ResolvedMask(resolvedMask)
{
	// [CUSTOMDEPTH] PPCustomShaders::Run resolves the scene depth before this shader runs when it names one.
	// [SCENEMASK] And the scene mask, the same way -- naming it here is also what makes the engine allocate
	// the attachment at all (PPSceneMask::WantedByShaders reads the same descriptions).
	{
		TMap<FString, FString>::Iterator itDepth(Desc->Textures);
		TMap<FString, FString>::Pair *pairDepth;
		while (itDepth.NextPair(pairDepth))
		{
			if (!pairDepth->Value.CompareNoCase("SceneDepth"))
				NeedsSceneDepth = true;
			if (!pairDepth->Value.CompareNoCase("SceneMask"))
				NeedsSceneMask = true;
		}
	}

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

	// SetupShader gives every texture entry a binding in this iteration order, so each entry takes its index whether or
	// not it resolves: a name that fails must not shift the later bindings.
	int textureIndex = 1;
	TMap<FString, FString>::Iterator it(Desc->Textures);
	TMap<FString, FString>::Pair *pair;
	while (it.NextPair(pair))
	{
		FString name = pair->Value;
		if (!name.CompareNoCase("SceneDepth"))
		{
			// The scene depth by name (GZSelaco): this eye's depth, resolved by PPCustomShaders::Run into a single-sample
			// texture over the screen viewport ([CUSTOMDEPTH]), so the prolog's sampler2D reads it at TexCoord with or
			// without MSAA.
			if (ResolvedDepth != nullptr) renderstate->SetInputTexture(textureIndex, ResolvedDepth, PPFilterMode::Nearest);
			else if (gl_multisample > 1) renderstate->SetInputCurrent(textureIndex, PPFilterMode::Nearest);
			else renderstate->SetInputSceneDepth(textureIndex);
			textureIndex++;
			continue;
		}
		if (!name.CompareNoCase("SceneMask"))
		{
			// [SCENEMASK] The scene's per-pixel tag: this eye's mask, resolved by PPCustomShaders::Run into a
			// single-sample texture over the screen viewport, so the prolog's sampler2D reads it at TexCoord
			// with or without MSAA, and NEAREST so the shader is handed a tag and not a blend of two.
			// `texture(SceneMask, TexCoord).r * 255.0` is the byte the content wrote. 0 means nothing special.
			if (ResolvedMask != nullptr) renderstate->SetInputTexture(textureIndex, ResolvedMask, PPFilterMode::Nearest);
			else renderstate->SetInputCurrent(textureIndex, PPFilterMode::Nearest);
			textureIndex++;
			continue;
		}
		auto gtex = TexMan.GetGameTexture(TexMan.CheckForTexture(name.GetChars(), ETextureType::Any), true);
		if (!gtex || !gtex->isValid())
		{
			// Unresolved: keep the binding valid and the later indices where the prolog put them.
			renderstate->SetInputCurrent(textureIndex, PPFilterMode::Linear);
			textureIndex++;
			continue;
		}
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

			// [PPPROJECT] A projected uniform takes the eye being post-processed; with no view published, the script's value.
			if (pair->Value.Projection != 0)
			{
				double pu, pv, clipZ, focalY;
				if (PPCustomShaders::ProjectWorld(PPCustomShaders::CurrentEyeSet(), pair->Value.World[0], pair->Value.World[1], pair->Value.World[2], pu, pv, clipZ, focalY))
				{
					if (pair->Value.Projection == 1 && pair->Value.Type == PostProcessUniformType::Vec3)
					{
						const float point[3] = { (float)pu, (float)pv, (float)clipZ };
						memcpy(dst, point, sizeof(point));
						continue;
					}
					if (pair->Value.Projection == 2 && pair->Value.Type == PostProcessUniformType::Float)
					{
						const float scale = clipZ < screen->GetZNear() ? 0.0f : (float)(pair->Value.Size * 0.5 * focalY / clipZ);
						memcpy(dst, &scale, sizeof(scale));
						continue;
					}
				}
			}

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

//==========================================================================
//
// [EXPOSUREIMPULSE] Flash blindness's wash (hw_postprocess.h, PPExposureImpulse; the frame comes from hw_exposureimpulse.cpp).
//
//==========================================================================

bool PPExposureImpulse::MeterReferences(float litLight, float minSpan, float &darkExposure, float &litExposure)
{
	// exposurecombine.fp's e = 1 / max(Base + light x Scale, Min) at no light and at the lit reference, from the live settings
	// through E2's guards: the values the meter settles on in the dark and in a lit room.
	const ExposureCombineUniforms combine = PPCameraExposure::CombineUniforms(gl_exposure_base, gl_exposure_min, gl_exposure_scale, gl_exposure_speed);
	darkExposure = 1.0f / max(combine.ExposureBase, combine.ExposureMin);
	litExposure = 1.0f / max(combine.ExposureBase + litLight * combine.ExposureScale, combine.ExposureMin);
	return gl_bloom && std::isfinite(darkExposure) && std::isfinite(litExposure) && darkExposure - litExposure >= minSpan;
}

PPShader *PPExposureImpulse::WashShader(bool meter, bool glare, bool holdBeams)
{
	PPShader *const programs[8] = { &Wash, &WashMeter, &WashGlare, &WashMeterGlare, &WashHold, &WashMeterHold, &WashGlareHold, &WashMeterGlareHold };
	return programs[(meter ? 1 : 0) | (glare ? 2 : 0) | (holdBeams ? 4 : 0)];
}

void PPExposureImpulse::Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight, bool bloomed)
{
	// SKIPPED, NOT ZERO: nothing live means no group, no texture and no draw.
	if (!Frame.Live || sceneWidth <= 0 || sceneHeight <= 0)
		return;

	float darkExposure = 1.0f, litExposure = 0.0f;
	const bool meterUsable = MeterReferences(Frame.LitLight, Frame.MeterMinSpan, darkExposure, litExposure);

	renderstate->PushGroup("pp.exposureimpulse");

	// 1. A burst's first draw latches the darkness: this eye's exposure as PPCameraExposure::Render left it at the top of this
	//    Pass1, before the burst's own light could move it far. Every later eye and frame of the burst reads the copy. Not
	//    metered then: nothing is latched, and the burst stays on the fallback darkness to its end.
	if (LatchBurst != Frame.Burst)
	{
		LatchBurst = Frame.Burst;
		LatchValid = meterUsable;
		if (LatchValid)
		{
			renderstate->Clear();
			renderstate->Shader = &LatchShader;
			renderstate->Viewport.left = 0;
			renderstate->Viewport.top = 0;
			renderstate->Viewport.width = 1;
			renderstate->Viewport.height = 1;
			renderstate->SetInputTexture(0, &hw_postprocess.exposure.CameraTexture);
			renderstate->SetOutputTexture(&LatchTexture);
			renderstate->SetNoBlend();
			renderstate->Draw();
		}
	}

	// 2. The wash, over the whole screen into the next pipeline image. The inputs are bound in the program's order: the image,
	//    then the latch, this eye's bloom and the light mask, each only where its define is on.
	const bool meter = LatchValid && meterUsable;
	const bool glare = bloomed && Frame.Glare > 0.0f;
	const bool holdBeams = Frame.HoldBeams && hw_postprocess.lightmask.PinnedBloomOn() && hw_postprocess.lightmask.PostInputValid();

	ExposureImpulseUniforms uniforms = {};
	uniforms.Scale = screen->SceneScale();
	uniforms.Offset = screen->SceneOffset();
	uniforms.Amount = Frame.Amount;
	uniforms.DarkFloor = Frame.DarkFloor;
	uniforms.FallbackDarkness = Frame.FallbackDarkness;
	uniforms.LitExposure = meter ? litExposure : 0.0f;
	uniforms.DarkExposure = meter ? darkExposure : 1.0f;
	uniforms.Gain = Frame.Gain;
	uniforms.Veil = Frame.Veil;
	uniforms.Desaturate = Frame.Desaturate;
	uniforms.VeilTint = Frame.VeilTint;
	uniforms.Glare = glare ? Frame.Glare : 0.0f;

	renderstate->Clear();
	renderstate->Shader = WashShader(meter, glare, holdBeams);
	renderstate->Uniforms.Set(uniforms);
	renderstate->Viewport = screen->mScreenViewport;
	int input = 0;
	renderstate->SetInputCurrent(input++, PPFilterMode::Nearest);
	if (meter)
		renderstate->SetInputTexture(input++, &LatchTexture);
	if (glare)
		renderstate->SetInputTexture(input++, hw_postprocess.bloom.RestBloomTexture(), PPFilterMode::Linear);
	if (holdBeams)
		renderstate->SetInputLightMask(input++, PPFilterMode::Nearest);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
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
	// [EMISSIVEVOLUMES] Emissive volumes, after the cones and before the heatmap: haze in front dims them, haze behind does not,
	// their shimmer bends them and they bloom. Skipped entirely with no volume (PPEmissiveVolumes).
	emissivevolumes.Render(state, sceneWidth, sceneHeight);
	heatmap.Render(state, sceneWidth, sceneHeight);
	// [HEATREFRACTION] Heat refraction, then bloom: the image bends before it glows.
	heatrefraction.Render(state, sceneWidth, sceneHeight);
	// [LIGHTMASK] The light mask's debug view (r_lightmask_debug) is drawn in bloom's place.
	if (!lightmask.RenderDebug(state))
	{
		const bool bloomed = bloom.RenderBloom(state, sceneWidth, sceneHeight, fixedcm);
		// [EXPOSUREIMPULSE] Flash blindness's wash, last: over this eye's bloomed image, re-adding its bloom as glare. Skipped
		// entirely while no wash is live (PPExposureImpulse), and never drawn where the debug view drew.
		exposureimpulse.Render(state, sceneWidth, sceneHeight, bloomed);
	}
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
