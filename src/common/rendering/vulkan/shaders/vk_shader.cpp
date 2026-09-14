/*
** vk_shader.cpp
**
** Vulkan backend
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

#include "vk_shader.h"
#include "vk_ppshader.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/system/vk_renderdevice.h"
#include "hw_shaderpatcher.h"
#include "filesystem.h"
#include "engineerrors.h"
#include "version.h"
#include "cmdlib.h"
#include "printf.h"
#include "hw_drawnlinebuffer.h"	// [DRAWNLINES] ShaderReady / ShaderFailed
#include "hw_cvars.h"	// [DRAWNLINES] r_beams_drawn, for the compile line
#include "hw_gpuparticlebuffer.h"	// [GPUPARTICLES] ShaderReady / ShaderFailed

// [2a] Effects that read the scene depth inside a read-only depth pass, and so get
// four extra fragment variants each (single-sample / multisample x flat / layered,
// see mSceneDepthEffectFrag). Only gpuparticles in 2a, for its soft edge;
// drawnlines joins when drawnlines.fp clamps its glow at the surface. Per effect,
// never per mod: any caller that sets one of these effects gets the same shader.
static bool EffectHasSceneDepthVariants(int effect)
{
	return effect == EFF_GPUPARTICLES;
}

ShaderIncludeResult VkShaderManager::OnInclude(FString headerName, FString includerName, size_t depth)
{
	if (depth > 8)
		I_Error("Too much include recursion!");

	FString includeguardname;
	includeguardname << "_HEADERGUARD_" << headerName.GetChars();
	includeguardname.ReplaceChars("/\\.", '_');

	FString code;
	code << "#ifndef " << includeguardname.GetChars() << "\n";
	code << "#define " << includeguardname.GetChars() << "\n";
	code << "#line 1\n";

	int lumpNum = fileSystem.FindFile(headerName.GetChars());

	if(lumpNum >= 0)
	{
		code << GetStringFromLump(lumpNum, false);
	}

	code << "\n#endif\n";

	return ShaderIncludeResult(headerName.GetChars(), code.GetChars());
}

bool VkShaderManager::CompileNextShader()
{
	const char *mainvp = "shaders/glsl/main.vp";
	const char *mainfp = "shaders/glsl/main.fp";
	int i = compileIndex;

	if (compileState == 0)
	{
		// regular material shaders

		VkShaderProgram prog;
		prog.vert = LoadVertShader(defaultshaders[i].ShaderName, mainvp, defaultshaders[i].Defines);
		prog.frag = LoadFragShader(defaultshaders[i].ShaderName, mainfp, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defaultshaders[i].Defines, true, compilePass == GBUFFER_PASS);
		mMaterialShaders[compilePass].push_back(std::move(prog));

		compileIndex++;
		if (defaultshaders[compileIndex].ShaderName == nullptr)
		{
			compileIndex = 0;
			compileState++;
		}
	}
	else if (compileState == 1)
	{
		// NAT material shaders

		VkShaderProgram natprog;
		natprog.vert = LoadVertShader(defaultshaders[i].ShaderName, mainvp, defaultshaders[i].Defines);
		natprog.frag = LoadFragShader(defaultshaders[i].ShaderName, mainfp, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defaultshaders[i].Defines, false, compilePass == GBUFFER_PASS);
		mMaterialShadersNAT[compilePass].push_back(std::move(natprog));

		compileIndex++;
		if (compileIndex == SHADER_NoTexture)
		{
			compileIndex = 0;
			compileState++;
			if (usershaders.Size() == 0) compileState++;
		}
	}
	else if (compileState == 2)
	{
		// user shaders

		const FString& name = ExtractFileBase(usershaders[i].shader.GetChars());
		FString defines = defaultshaders[usershaders[i].shaderType].Defines + usershaders[i].defines;

		VkShaderProgram prog;
		prog.vert = LoadVertShader(name, mainvp, defines.GetChars());
		prog.frag = LoadFragShader(name, mainfp, usershaders[i].shader.GetChars(), defaultshaders[usershaders[i].shaderType].lightfunc, defines.GetChars(), true, compilePass == GBUFFER_PASS);
		mMaterialShaders[compilePass].push_back(std::move(prog));

		compileIndex++;
		if (compileIndex >= (int)usershaders.Size())
		{
			compileIndex = 0;
			compileState++;
		}
	}
	else if (compileState == 3)
	{
		// Effect shaders

		VkShaderProgram prog;
		if (i == EFF_GPUPARTICLES || i == EFF_DRAWNLINES)
		{
			// [GPUPARTICLES] The one effect whose compile failure must not take
			// Vulkan startup down with it: it is new, optional, and nothing else
			// depends on it. A failure is logged, the program is left empty (so
			// GetEffect returns null for it), and GpuParticleBuffer::ShaderFailed
			// below keeps the draw -- and therefore the pipeline that would
			// dereference that null -- from ever being requested.
			//
			// [DRAWNLINES] Two such effects now, guarded identically for the same
			// reasons. DrawnLineBuffer::ShaderFailed is the drawn lines' gate, and
			// it also sends r_beams_drawn back to per-pixel beams.
			const bool particles = (i == EFF_GPUPARTICLES);
			try
			{
				prog.vert = LoadVertShader(effectshaders[i].ShaderName, effectshaders[i].vp, effectshaders[i].defines);
				prog.frag = LoadFragShader(effectshaders[i].ShaderName, effectshaders[i].fp1, effectshaders[i].fp2, effectshaders[i].fp3, effectshaders[i].defines, true, compilePass == GBUFFER_PASS);
			}
			catch (const std::exception &err)
			{
				Printf(TEXTCOLOR_RED "%s: effect shader failed to compile (%s pass) -- %s disabled:\n%s\n",
					particles ? "GpuParticles" : "DrawnLines",
					compilePass == GBUFFER_PASS ? "gbuffer" : "normal",
					particles ? "particles" : "drawn lines", err.what());
				prog.vert.reset();
				prog.frag.reset();
			}
		}
		else
		{
			prog.vert = LoadVertShader(effectshaders[i].ShaderName, effectshaders[i].vp, effectshaders[i].defines);
			prog.frag = LoadFragShader(effectshaders[i].ShaderName, effectshaders[i].fp1, effectshaders[i].fp2, effectshaders[i].fp3, effectshaders[i].defines, true, compilePass == GBUFFER_PASS);
		}

		// [2a] The scene-depth variants of an effect that reads scene depth,
		// compiled here beside the effect rather than when first drawn: a glslang
		// compile of this prolog is a visible hitch in a headset, and turning soft
		// particles on must not cost one mid-fight. Only after the effect itself
		// compiled, and in their own try -- a variant that fails leaves the
		// effect's ordinary program untouched, and the effect keeps hard edges.
		if (prog.frag && EffectHasSceneDepthVariants(i))
		{
			for (int variant = 0; variant < SCENE_DEPTH_VARIANTS; variant++)
			{
				FString defines = effectshaders[i].defines;
				defines << "#define SCENE_DEPTH_READ\n";
				if (variant & 1) defines << "#define SCENE_DEPTH_MULTISAMPLE\n";
				if (variant & 2) defines << "#define SCENE_DEPTH_LAYERED\n";
				FString name;
				name.Format("%s_scenedepth%d", effectshaders[i].ShaderName, variant);
				try
				{
					mSceneDepthEffectFrag[compilePass][i][variant] = LoadFragShader(name, effectshaders[i].fp1, effectshaders[i].fp2, effectshaders[i].fp3, defines.GetChars(), true, compilePass == GBUFFER_PASS, true);
				}
				catch (const std::exception &err)
				{
					Printf(TEXTCOLOR_RED "%s: scene depth variant failed to compile (%s pass, %s, %s) -- soft edges unavailable:\n%s\n",
						effectshaders[i].ShaderName,
						compilePass == GBUFFER_PASS ? "gbuffer" : "normal",
						(variant & 1) ? "multisample" : "single-sample",
						(variant & 2) ? "layered" : "flat", err.what());
					mSceneDepthEffectFrag[compilePass][i][variant].reset();
				}
			}
		}

		mEffectShaders[compilePass].push_back(std::move(prog));

		compileIndex++;
		if (compileIndex >= MAX_EFFECTS)
		{
			compileIndex = 0;
			compilePass++;
			if (compilePass == MAX_PASS_TYPES)
			{
				compileIndex = -1; // we're done.

				// [GPUPARTICLES] Every pass is compiled now. The draw is allowed
				// only if the effect exists for all of them, because the pass a
				// pipeline is built for depends on the render target at draw time.
				if (fb->mGpuParticles != nullptr)
				{
					bool ok = true;
					for (int pass = 0; pass < MAX_PASS_TYPES; pass++)
					{
						if ((int)mEffectShaders[pass].size() <= EFF_GPUPARTICLES || !mEffectShaders[pass][EFF_GPUPARTICLES].vert || !mEffectShaders[pass][EFF_GPUPARTICLES].frag)
							ok = false;
					}
					fb->mGpuParticles->ShaderFailed = !ok;
					fb->mGpuParticles->ShaderReady = ok;
					Printf("GpuParticles: effect shader %s for %d passes\n", ok ? "compiled" : "NOT available", (int)MAX_PASS_TYPES);

					// [2a] Soft particles need every scene-depth variant for every
					// pass: which one a pipeline uses depends on the sample count and
					// layering of the target at draw time, which a menu can change.
					bool depthOk = ok;
					for (int pass = 0; pass < MAX_PASS_TYPES; pass++)
						for (int variant = 0; variant < SCENE_DEPTH_VARIANTS; variant++)
							if (!mSceneDepthEffectFrag[pass][EFF_GPUPARTICLES][variant])
								depthOk = false;
					fb->mGpuParticles->SceneDepthShaderReady = depthOk;
					Printf("GpuParticles: scene depth variants %s -- r_gpuparticles_soft %s\n",
						depthOk ? "compiled" : "NOT available", depthOk ? "can soften edges" : "keeps hard edges");
				}

				// [DRAWNLINES] The same rule: the effect for every pass, or no draw
				// and no r_beams_drawn routing at all.
				if (fb->mDrawnLines != nullptr)
				{
					bool ok = true;
					for (int pass = 0; pass < MAX_PASS_TYPES; pass++)
					{
						if ((int)mEffectShaders[pass].size() <= EFF_DRAWNLINES || !mEffectShaders[pass][EFF_DRAWNLINES].vert || !mEffectShaders[pass][EFF_DRAWNLINES].frag)
							ok = false;
					}
					fb->mDrawnLines->ShaderFailed = !ok;
					fb->mDrawnLines->ShaderReady = ok;
					Printf("DrawnLines: effect shader %s for %d passes -- r_beams_drawn is %s%s\n",
						ok ? "compiled" : "NOT available", (int)MAX_PASS_TYPES,
						r_beams_drawn ? "ON" : "off",
						(r_beams_drawn && !ok) ? ", so beams stay per-pixel" : "");
				}
				return true;
			}
			compileState = 0;
		}
	}
	return false;
}

VkShaderManager::VkShaderManager(VulkanRenderDevice* fb) : fb(fb)
{
	//CompileNextShader();
}

VkShaderManager::~VkShaderManager()
{
}

void VkShaderManager::Deinit()
{
	while (!PPShaders.empty())
		RemoveVkPPShader(PPShaders.back());
}

VkShaderProgram *VkShaderManager::GetEffect(int effect, EPassType passType)
{
	if (compileIndex == -1 && effect >= 0 && effect < MAX_EFFECTS && mEffectShaders[passType][effect].frag)
	{
		return &mEffectShaders[passType][effect];
	}
	return nullptr;
}

// [2a] See the declaration in vk_shader.h.
VulkanShader *VkShaderManager::GetSceneDepthEffectFrag(int effect, EPassType passType, bool multisample, bool layered)
{
	if (compileIndex != -1 || effect < 0 || effect >= MAX_EFFECTS || (int)passType < 0 || (int)passType >= MAX_PASS_TYPES)
		return nullptr;
	return mSceneDepthEffectFrag[passType][effect][(multisample ? 1 : 0) | (layered ? 2 : 0)].get();
}

VkShaderProgram *VkShaderManager::Get(unsigned int eff, bool alphateston, EPassType passType)
{
	if (compileIndex != -1)
		return &mMaterialShaders[0][0];
	// indices 0-2 match the warping modes, 3 no texture, the following are custom
	if (!alphateston && eff < SHADER_NoTexture)
	{
		return &mMaterialShadersNAT[passType][eff];	// Non-alphatest shaders are only created for default, warp1+2. The rest won't get used anyway
	}
	else if (eff < (unsigned int)mMaterialShaders[passType].size())
	{
		return &mMaterialShaders[passType][eff];
	}
	return nullptr;
}

static const char *shaderBindings = R"(

	layout(set = 0, binding = 0) uniform sampler2D ShadowMap;
	layout(set = 0, binding = 1) uniform sampler2DArray LightMap;
	#ifdef SUPPORTS_RAYTRACING
	layout(set = 0, binding = 2) uniform accelerationStructureEXT TopLevelAS;
	#endif

	#ifdef SUPPORTS_MULTIVIEW
	#ifdef VERTEX_SHADER
	layout(location = 15) flat out int hwViewIndex;
	#define HW_VIEWPOINT_INDEX gl_ViewIndex
	#else
	layout(location = 15) flat in int hwViewIndex;
	#define HW_VIEWPOINT_INDEX hwViewIndex
	#endif
	#else
	#define HW_VIEWPOINT_INDEX 0
	#endif

	// This must match the HWViewpointUniforms struct
	struct ViewpointData
	{
		mat4 ProjectionMatrix;
		mat4 ViewMatrix;
		mat4 NormalViewMatrix;

		vec4 uCameraPos;
		vec4 uClipLine;

		float uGlobVis;			// uGlobVis = R_GetGlobVis(r_visibility) / 32.0
		int uPalLightLevels;
		int uViewHeight;		// Software fuzz scaling
		float uClipHeight;
		float uClipHeightDirection;
		int uShadowmapFilter;

		int uLightBlendMode;

		// [BB] Glow wave. The int above plus mPadding0 in the C++ struct end
		// this block at 28 bytes; std140 aligns a vec4 to 16, so the compiler
		// pads to 32 and these land on exactly the offsets HWViewpointUniforms
		// puts them at. Do not insert a scalar before them.
		vec4 uGlowWave;
		vec4 uGlowWaveDepth;
		vec4 uGlowWavePhase;
		vec4 uGlowWaveOrigin;

		// [BB] Darkness as a shader term.
		vec4 uDarkness;
		vec4 uDarkness2;
		vec4 uDarkness3;

		// [BB] Fog slab -- fog with a top.
		vec4 uFogSlab;
		vec4 uFogSlabColor;
		vec4 uFogSlabWake;
		vec4 uFogBeamPos;
		vec4 uFogBeamDir;
		vec4 uFogBeamCol;
		vec4 uFogSlabExtra;

		// [BB] Sweep fill -- the pattern inside a band.
		vec4 uSweepFill;
		vec4 uSweepFill2;
		vec4 uSweepFill3;
		vec4 uSweepFillCol;

		// [BB] Beams -- real segment lasers.
		vec4 uBeamA[128];
		vec4 uBeamB[128];
		vec4 uBeamCol[128];
		vec4 uBeamParams;
		vec4 uBeamFX;
		vec4 uFogSurf;
		vec4 uSweepAir;
		vec4 uFogSlab2;
		vec4 uTornado;
		vec4 uTornado2;
		vec4 uTornado3;
		vec4 uTornadoCol;
		vec4 uFogDisturbA[32];
		vec4 uFogDisturbB[32];
		vec4 uFogNoise;
		vec4 uFogTendril;
		vec4 uFogTendril2;
		vec4 uFogWake2;
		vec4 uFogBow;
		vec4 uFogColor2;
		vec4 uGlowTex;
		vec4 uGlowTex2;
		vec4 uGlowTex3;
		vec4 uGlowTex4;
		vec4 uDesatKeep;
		vec4 uShapeA[128];
		vec4 uShapeB[128];
		vec4 uShapeCol[128];
		vec4 uShapeD[128];
		vec4 uShapeParams;
		vec4 uShapeUnder;
		vec4 uFogFollow;

		// Upstream 5.0.0 thick-fog knobs. They are APPENDED here, after the
		// last vec4, because that is where HWViewpointUniforms puts
		// mThickFogDistance/mThickFogMultiplier and where gl_shader.cpp's
		// ViewpointUBO puts them -- a uniform block is matched by OFFSET.
		// Note this leaves the struct 8 bytes past a vec4 boundary, so
		// HWViewpointUniforms must keep its trailing padding: the std140
		// array stride of viewpoints[2] rounds up to 16 and the second eye
		// would otherwise read from the wrong offset.
		float uThickFogDistance;
		float uThickFogMultiplier;

		// [BB] Standing shape pitch/roll -- appended after the thick-fog
		// pair for the identical reason THEY are appended after
		// uFogFollow: matched to hw_viewpointuniforms.h by offset, and
		// this is where that header's mShapeE actually sits (past its
		// std140 padding). std140 supplies this array's own leading
		// padding implicitly; nothing to declare by hand here.
		vec4 uShapeE[128];
		vec4 uSweepRoomMin;
		vec4 uSweepRoomMax;

		// [STAMP] Surface stamps. Appended last, matching
		// HWViewpointUniforms::mStamp* by offset.
		// 64 -- must equal MAX_SURFACE_STAMPS (func_surfacestamps.fp).
		vec4 uSurfaceStampPos[64];
		vec4 uSurfaceStampCol[64];
		vec4 uSurfaceStampArg[64];
		vec4 uSurfaceStampMod[64];
		vec4 uSurfaceStampParams;

		// [GPUPARTICLES] APPENDED LAST, matching HWViewpointUniforms by
		// offset. uLevelTime.x is level seconds at render rate (maptime +
		// TicFrac) / TICRATE -- the clock gpuparticles.vp ages records by.
		// uGpuParticleParams: x size scale, y max size, z stretch, w intensity.
		vec4 uLevelTime;
		vec4 uGpuParticleParams;

		// [BEAMLINES] APPENDED LAST, matching HWViewpointUniforms::mBeamLook by
		// offset: per uploaded beam line, x air glow, y halo, z taper, w flare.
		vec4 uBeamLook[128];

		// [round2 B2] APPENDED LAST, matching HWViewpointUniforms::mSweepPassed /
		// mSweepPassedColor by offset. The passed-region look: x tint mix,
		// y darken, z desaturate, w soft; rgb tint, w enable.
		vec4 uSweepPassed;
		vec4 uSweepPassedColor;

		// [2a] APPENDED LAST, matching HWViewpointUniforms::mLinearizeDepth /
		// mGpuParticleParams2 by offset. uLinearizeDepth: x LinearizeDepthA,
		// y LinearizeDepthB (distance = 1 / (raw * A + B)), for effects reading
		// scene depth. uGpuParticleParams2.x: soft distance, map units, 0 = off.
		vec4 uLinearizeDepth;
		vec4 uGpuParticleParams2;
	};

	layout(set = 1, binding = 0, std140) uniform readonly ViewpointUBO {
		ViewpointData viewpoints[2];
	};

	#define ProjectionMatrix viewpoints[HW_VIEWPOINT_INDEX].ProjectionMatrix
	#define ViewMatrix viewpoints[HW_VIEWPOINT_INDEX].ViewMatrix
	#define NormalViewMatrix viewpoints[HW_VIEWPOINT_INDEX].NormalViewMatrix
	#define uCameraPos viewpoints[HW_VIEWPOINT_INDEX].uCameraPos
	#define uClipLine viewpoints[HW_VIEWPOINT_INDEX].uClipLine
	#define uGlobVis viewpoints[HW_VIEWPOINT_INDEX].uGlobVis
	#define uPalLightLevels viewpoints[HW_VIEWPOINT_INDEX].uPalLightLevels
	#define uViewHeight viewpoints[HW_VIEWPOINT_INDEX].uViewHeight
	#define uClipHeight viewpoints[HW_VIEWPOINT_INDEX].uClipHeight
	#define uClipHeightDirection viewpoints[HW_VIEWPOINT_INDEX].uClipHeightDirection
	#define uShadowmapFilter viewpoints[HW_VIEWPOINT_INDEX].uShadowmapFilter
	#define uLightBlendMode viewpoints[HW_VIEWPOINT_INDEX].uLightBlendMode
	#define uGlowWave viewpoints[HW_VIEWPOINT_INDEX].uGlowWave
	#define uGlowWaveDepth viewpoints[HW_VIEWPOINT_INDEX].uGlowWaveDepth
	#define uGlowWavePhase viewpoints[HW_VIEWPOINT_INDEX].uGlowWavePhase
	#define uGlowWaveOrigin viewpoints[HW_VIEWPOINT_INDEX].uGlowWaveOrigin
	#define uDarkness viewpoints[HW_VIEWPOINT_INDEX].uDarkness
	#define uDarkness2 viewpoints[HW_VIEWPOINT_INDEX].uDarkness2
	#define uDarkness3 viewpoints[HW_VIEWPOINT_INDEX].uDarkness3
	#define uFogSlab viewpoints[HW_VIEWPOINT_INDEX].uFogSlab
	#define uFogSlabColor viewpoints[HW_VIEWPOINT_INDEX].uFogSlabColor
	#define uFogSlabWake viewpoints[HW_VIEWPOINT_INDEX].uFogSlabWake
	#define uFogBeamPos viewpoints[HW_VIEWPOINT_INDEX].uFogBeamPos
	#define uFogBeamDir viewpoints[HW_VIEWPOINT_INDEX].uFogBeamDir
	#define uFogBeamCol viewpoints[HW_VIEWPOINT_INDEX].uFogBeamCol
	#define uFogSlabExtra viewpoints[HW_VIEWPOINT_INDEX].uFogSlabExtra
	#define uSweepFill viewpoints[HW_VIEWPOINT_INDEX].uSweepFill
	#define uSweepFill2 viewpoints[HW_VIEWPOINT_INDEX].uSweepFill2
	#define uSweepFill3 viewpoints[HW_VIEWPOINT_INDEX].uSweepFill3
	#define uSweepFillCol viewpoints[HW_VIEWPOINT_INDEX].uSweepFillCol
	#define uBeamA viewpoints[HW_VIEWPOINT_INDEX].uBeamA
	#define uBeamB viewpoints[HW_VIEWPOINT_INDEX].uBeamB
	#define uBeamCol viewpoints[HW_VIEWPOINT_INDEX].uBeamCol
	#define uBeamParams viewpoints[HW_VIEWPOINT_INDEX].uBeamParams
	#define uBeamFX viewpoints[HW_VIEWPOINT_INDEX].uBeamFX
	#define uFogSurf viewpoints[HW_VIEWPOINT_INDEX].uFogSurf
	#define uSweepAir viewpoints[HW_VIEWPOINT_INDEX].uSweepAir
	#define uFogSlab2 viewpoints[HW_VIEWPOINT_INDEX].uFogSlab2
	#define uTornado viewpoints[HW_VIEWPOINT_INDEX].uTornado
	#define uTornado2 viewpoints[HW_VIEWPOINT_INDEX].uTornado2
	#define uTornado3 viewpoints[HW_VIEWPOINT_INDEX].uTornado3
	#define uTornadoCol viewpoints[HW_VIEWPOINT_INDEX].uTornadoCol
	#define uFogDisturbA viewpoints[HW_VIEWPOINT_INDEX].uFogDisturbA
	#define uFogDisturbB viewpoints[HW_VIEWPOINT_INDEX].uFogDisturbB
	#define uFogNoise viewpoints[HW_VIEWPOINT_INDEX].uFogNoise
	#define uFogTendril viewpoints[HW_VIEWPOINT_INDEX].uFogTendril
	#define uFogTendril2 viewpoints[HW_VIEWPOINT_INDEX].uFogTendril2
	#define uFogWake2 viewpoints[HW_VIEWPOINT_INDEX].uFogWake2
	#define uFogBow viewpoints[HW_VIEWPOINT_INDEX].uFogBow
	#define uFogColor2 viewpoints[HW_VIEWPOINT_INDEX].uFogColor2
	#define uGlowTex viewpoints[HW_VIEWPOINT_INDEX].uGlowTex
	#define uGlowTex2 viewpoints[HW_VIEWPOINT_INDEX].uGlowTex2
	#define uGlowTex3 viewpoints[HW_VIEWPOINT_INDEX].uGlowTex3
	#define uGlowTex4 viewpoints[HW_VIEWPOINT_INDEX].uGlowTex4
	#define uDesatKeep viewpoints[HW_VIEWPOINT_INDEX].uDesatKeep
	#define uShapeA viewpoints[HW_VIEWPOINT_INDEX].uShapeA
	#define uShapeB viewpoints[HW_VIEWPOINT_INDEX].uShapeB
	#define uShapeCol viewpoints[HW_VIEWPOINT_INDEX].uShapeCol
	#define uShapeD viewpoints[HW_VIEWPOINT_INDEX].uShapeD
	#define uShapeParams viewpoints[HW_VIEWPOINT_INDEX].uShapeParams
	#define uShapeUnder viewpoints[HW_VIEWPOINT_INDEX].uShapeUnder
	#define uFogFollow viewpoints[HW_VIEWPOINT_INDEX].uFogFollow
	#define uThickFogDistance viewpoints[HW_VIEWPOINT_INDEX].uThickFogDistance
	#define uThickFogMultiplier viewpoints[HW_VIEWPOINT_INDEX].uThickFogMultiplier
	// [BB] uShapeE's struct member exists (see ViewpointData above) but, unlike
	// GL's bare anonymous block, this Vulkan block is instanced as viewpoints[2]
	// and every member needs its own indirection macro or shader code referencing
	// the bare name compiles nowhere - exactly what was missing here.
	#define uShapeE viewpoints[HW_VIEWPOINT_INDEX].uShapeE
	#define uSweepRoomMin viewpoints[HW_VIEWPOINT_INDEX].uSweepRoomMin
	#define uSweepRoomMax viewpoints[HW_VIEWPOINT_INDEX].uSweepRoomMax
	#define uSurfaceStampPos viewpoints[HW_VIEWPOINT_INDEX].uSurfaceStampPos
	#define uSurfaceStampCol viewpoints[HW_VIEWPOINT_INDEX].uSurfaceStampCol
	#define uSurfaceStampArg viewpoints[HW_VIEWPOINT_INDEX].uSurfaceStampArg
	#define uSurfaceStampMod viewpoints[HW_VIEWPOINT_INDEX].uSurfaceStampMod
	#define uSurfaceStampParams viewpoints[HW_VIEWPOINT_INDEX].uSurfaceStampParams
	// [GPUPARTICLES] the fourth list -- every ViewpointData member needs one
	#define uLevelTime viewpoints[HW_VIEWPOINT_INDEX].uLevelTime
	#define uGpuParticleParams viewpoints[HW_VIEWPOINT_INDEX].uGpuParticleParams
	// [BEAMLINES] and this one
	#define uBeamLook viewpoints[HW_VIEWPOINT_INDEX].uBeamLook
	// [round2 B2] and these
	#define uSweepPassed viewpoints[HW_VIEWPOINT_INDEX].uSweepPassed
	#define uSweepPassedColor viewpoints[HW_VIEWPOINT_INDEX].uSweepPassedColor
	// [2a] and these
	#define uLinearizeDepth viewpoints[HW_VIEWPOINT_INDEX].uLinearizeDepth
	#define uGpuParticleParams2 viewpoints[HW_VIEWPOINT_INDEX].uGpuParticleParams2

	layout(set = 1, binding = 1, std140) uniform readonly MatricesUBO {
		mat4 ModelMatrix;
		mat4 NormalModelMatrix;
		mat4 TextureMatrix;
	};

	struct StreamData
	{
		vec4 uObjectColor;
		vec4 uObjectColor2;
		vec4 uDynLightColor;
		vec4 uAddColor;
		vec4 uTextureAddColor;
		vec4 uTextureModulateColor;
		vec4 uTextureBlendColor;
		vec4 uFogColor;
		float uDesaturationFactor;
		float uInterpolationFactor;
		float timer; // timer data for material shaders
		int useVertexData;
		vec4 uVertexColor;
		vec4 uVertexNormal;

		vec4 uGlowTopPlane;
		vec4 uGlowTopColor;
		vec4 uGlowBottomPlane;
		vec4 uGlowBottomColor;
		vec4 uGlowTopFar;
		vec4 uGlowBottomFar;
		int uGlowTopFalloff;
		int uGlowBottomFalloff;
		float uGlowTopIntensity;
		float uGlowBottomIntensity;

		vec4 uSweepOrigin;
		vec4 uSweepBands[8];
		vec4 uSweepColors[8];
		vec4 uSweepBandOrigin[8];
		int uSweepCount;
		float uSweepTrail;
		int uSweepPad1;
		int uSweepPad2;

		vec4 uFlatGlowColor;
		vec4 uFlatGlowFar;
		int uFlatGlowFalloff;
		int uFlatGlowLineCount;
		int uFlatGlowIsCeiling;
		float uDarknessExempt;
		vec4 uFlatGlowLines[64];

		vec4 uGradientTopPlane;
		vec4 uGradientBottomPlane;

		vec4 uSplitTopPlane;
		vec4 uSplitBottomPlane;

		vec4 uDetailParms;
		vec4 uNpotEmulation;

		vec4 uGlobalFadeColor;
		int uGlobalFade;
		int uGlobalFadeMode;
		float uGlobalFadeDensity;
		float uGlobalFadeGradient;
		int uLightRangeLimit;

		int padding1;
		int padding2;
		int padding3;

		vec4  uOutlineColorA;
		vec4  uOutlineColorB;
		vec4  uOutlineParms;

		float uFogDensityScale;
		int uFogPad0;
		int uFogPad1;
		int uFogPad2;
	};

	layout(set = 1, binding = 2, std140) uniform readonly StreamUBO {
		StreamData data[MAX_STREAM_DATA];
	};

	// light buffers
	layout(set = 1, binding = 3, std430) buffer readonly LightBufferSSO
	{
	    vec4 lights[];
	};

	// bone matrix buffers
	layout(set = 1, binding = 4, std430) buffer readonly BoneBufferSSO
	{
	    mat4 bones[];
	};

	// [GPUPARTICLES] The stateless particle ring (hw_gpuparticlebuffer.h).
	// Five vec4s, 80 bytes, std430 with no padding -- must match
	// FLevelLocals::GpuParticleRecord. Declared for every shader like the
	// bones; only gpuparticles.vp reads it, so the layout entry is vertex-only.
	struct GpuParticle { vec4 a; vec4 b; vec4 c; vec4 d; vec4 e; };
	layout(set = 1, binding = 5, std430) buffer readonly GpuParticleSSO
	{
	    GpuParticle gpuParticles[];
	};

	// [DRAWNLINES] Drawn glowing lines (hw_drawnlinebuffer.h). Seven vec4s, 112
	// bytes, std430 with no padding -- must match DrawnLineRecord. Declared for
	// every shader like the particles; only drawnlines.vp reads it, so the
	// layout entry is vertex-only. [F1] f and g, the opt-in gradient and
	// turbulence looks, are appended, so a..e keep their offsets.
	struct DrawnLine { vec4 a; vec4 b; vec4 c; vec4 d; vec4 e; vec4 f; vec4 g; };
	layout(set = 1, binding = 6, std430) buffer readonly DrawnLineSSO
	{
	    DrawnLine drawnLines[];
	};

	// [PARTICLEDEFS] The particle definitions table (hw_particledefbuffer.h),
	// indexed by a particle record's d.x. Sixteen vec4s, 256 bytes, std430 with no
	// padding -- must match ParticleDefinitionGpu (particledefs.h), which asserts
	// the offsets. Declared for every shader like the ring; gpuparticles.vp reads
	// it, gpuparticles.fp will from stage 2c, so the layout entry is vertex and
	// fragment.
	//   key[i]       x t (life fraction)  y size  z alpha  w emissive
	//   keyColor[j]  per key 0xRRGGBB as an exact float, keys 0-3 then 4-7
	//   motion       x gravity  y drag  z maxsize (0 none)  w key count
	//   shape        x orient  y stretch  z spin min  w spin max (deg/s)
	//   look         x lit  y soft (-1 unset)  z collide  w flags (1 fade smooth)
	//   flipbook     x first atlas layer (-1 none)  y frames  z fps  w 0 loop 1 once
	struct ParticleDefinitionData
	{
		vec4 key[8];
		vec4 keyColor[2];
		vec4 motion;
		vec4 shape;
		vec4 look;
		vec4 flipbook;
		vec4 spare[2];
	};
	layout(set = 1, binding = 7, std430) buffer readonly ParticleDefinitionSSO
	{
	    ParticleDefinitionData particleDefinitions[];
	};

	// textures
	layout(set = 2, binding = 0) uniform sampler2D tex;
	layout(set = 2, binding = 1) uniform sampler2D texture2;
	layout(set = 2, binding = 2) uniform sampler2D texture3;
	layout(set = 2, binding = 3) uniform sampler2D texture4;
	layout(set = 2, binding = 4) uniform sampler2D texture5;
	layout(set = 2, binding = 5) uniform sampler2D texture6;
	layout(set = 2, binding = 6) uniform sampler2D texture7;
	layout(set = 2, binding = 7) uniform sampler2D texture8;
	layout(set = 2, binding = 8) uniform sampler2D texture9;
	layout(set = 2, binding = 9) uniform sampler2D texture10;
	layout(set = 2, binding = 10) uniform sampler2D texture11;
	layout(set = 2, binding = 11) uniform sampler2D texture12;

	// This must match the PushConstants struct
	layout(push_constant) uniform PushConstants
	{
		int uTextureMode;
		float uAlphaThreshold;
		vec2 uClipSplit;

		// Lighting + Fog
		float uLightLevel;
		float uFogDensity;
		float uLightFactor;
		float uLightDist;
		int uFogEnabled;

		// dynamic lights
		int uLightIndex;

		// Blinn glossiness and specular level
		vec2 uSpecularMaterial;

		// bone animation
		int uBoneIndexBase;

		int uDataIndex;
		int padding2, padding3;
	};

	// material types
	#if defined(SPECULAR)
	#define normaltexture texture2
	#define speculartexture texture3
	#define brighttexture texture4
	#define detailtexture texture5
	#define glowtexture texture6
	#elif defined(PBR)
	#define normaltexture texture2
	#define metallictexture texture3
	#define roughnesstexture texture4
	#define aotexture texture5
	#define brighttexture texture6
	#define detailtexture texture7
	#define glowtexture texture8
	#else
	#define brighttexture texture2
	#define detailtexture texture3
	#define glowtexture texture4
	#endif

	#define uObjectColor data[uDataIndex].uObjectColor
	#define uObjectColor2 data[uDataIndex].uObjectColor2
	#define uDynLightColor data[uDataIndex].uDynLightColor
	#define uAddColor data[uDataIndex].uAddColor
	#define uTextureBlendColor data[uDataIndex].uTextureBlendColor
	#define uTextureModulateColor data[uDataIndex].uTextureModulateColor
	#define uTextureAddColor data[uDataIndex].uTextureAddColor
	#define uFogColor data[uDataIndex].uFogColor
	#define uDesaturationFactor data[uDataIndex].uDesaturationFactor
	#define uInterpolationFactor data[uDataIndex].uInterpolationFactor
	#define timer data[uDataIndex].timer
	#define useVertexData data[uDataIndex].useVertexData
	#define uVertexColor data[uDataIndex].uVertexColor
	#define uVertexNormal data[uDataIndex].uVertexNormal
	#define uGlowTopPlane data[uDataIndex].uGlowTopPlane
	#define uGlowTopColor data[uDataIndex].uGlowTopColor
	#define uGlowBottomPlane data[uDataIndex].uGlowBottomPlane
	#define uGlowBottomColor data[uDataIndex].uGlowBottomColor
	#define uGlowTopFar data[uDataIndex].uGlowTopFar
	#define uGlowBottomFar data[uDataIndex].uGlowBottomFar
	#define uGlowTopFalloff data[uDataIndex].uGlowTopFalloff
	#define uGlowBottomFalloff data[uDataIndex].uGlowBottomFalloff
	#define uGlowTopIntensity data[uDataIndex].uGlowTopIntensity
	#define uGlowBottomIntensity data[uDataIndex].uGlowBottomIntensity
	#define uSweepOrigin data[uDataIndex].uSweepOrigin
	#define uSweepBands data[uDataIndex].uSweepBands
	#define uSweepColors data[uDataIndex].uSweepColors
	#define uSweepBandOrigin data[uDataIndex].uSweepBandOrigin
	#define uSweepTrail data[uDataIndex].uSweepTrail
	#define uSweepCount data[uDataIndex].uSweepCount
	#define uFlatGlowColor data[uDataIndex].uFlatGlowColor
	#define uFlatGlowFar data[uDataIndex].uFlatGlowFar
	#define uFlatGlowFalloff data[uDataIndex].uFlatGlowFalloff
	#define uFlatGlowIsCeiling data[uDataIndex].uFlatGlowIsCeiling
	#define uDarknessExempt data[uDataIndex].uDarknessExempt
	#define uFogDensityScale data[uDataIndex].uFogDensityScale
	#define uOutlineColorA data[uDataIndex].uOutlineColorA
	#define uOutlineColorB data[uDataIndex].uOutlineColorB
	#define uOutlineParms data[uDataIndex].uOutlineParms
	#define uFlatGlowLineCount data[uDataIndex].uFlatGlowLineCount
	#define uFlatGlowLines data[uDataIndex].uFlatGlowLines
	#define uGradientTopPlane data[uDataIndex].uGradientTopPlane
	#define uGradientBottomPlane data[uDataIndex].uGradientBottomPlane
	#define uSplitTopPlane data[uDataIndex].uSplitTopPlane
	#define uSplitBottomPlane data[uDataIndex].uSplitBottomPlane
	#define uDetailParms data[uDataIndex].uDetailParms
	#define uNpotEmulation data[uDataIndex].uNpotEmulation
	#define uGlobalFadeColor data[uDataIndex].uGlobalFadeColor
	#define uGlobalFade data[uDataIndex].uGlobalFade
	#define uGlobalFadeMode data[uDataIndex].uGlobalFadeMode
	#define uGlobalFadeDensity data[uDataIndex].uGlobalFadeDensity
	#define uGlobalFadeGradient data[uDataIndex].uGlobalFadeGradient
	#define uLightRangeLimit data[uDataIndex].uLightRangeLimit

	#define SUPPORTS_SHADOWMAPS
	#define VULKAN_COORDINATE_SYSTEM
	#define HAS_UNIFORM_VERTEX_DATA

	// GLSL spec 4.60, 8.15. Noise Functions
	// https://www.khronos.org/registry/OpenGL/specs/gl/GLSLangSpec.4.60.pdf
	//  "The noise functions noise1, noise2, noise3, and noise4 have been deprecated starting with version 4.4 of GLSL.
	//   When not generating SPIR-V they are defined to return the value 0.0 or a vector whose components are all 0.0.
	//   When generating SPIR-V the noise functions are not declared and may not be used."
	// However, we need to support mods with custom shaders created for OpenGL renderer
	float noise1(float) { return 0; }
	vec2 noise2(vec2) { return vec2(0); }
	vec3 noise3(vec3) { return vec3(0); }
	vec4 noise4(vec4) { return vec4(0); }
)";

// [2a] THE SCENE DEPTH, READABLE -- declared ONLY in an effect's scene-depth
// variants (LoadFragShader's sceneDepth), never in shaderBindings above. Binding 3
// of the fixed set holds the screen's scene depth (VkDescriptorSetManager::
// UpdateFixedSet), and that image is a WRITABLE depth attachment for nearly the
// whole scene. A pipeline whose shaders merely declare a binding counts as using
// it, so a declaration in the shared prolog would put every surface draw in
// breach of the attachment-feedback and layout rules. Only these variants
// declare it, and they are only built for read-only depth passes
// (VkRenderPassKey::DepthReadOnly), where the image is
// DEPTH_STENCIL_READ_ONLY_OPTIMAL, the layout binding 3 is written with.
//
// The defines pick the sampler to match the view binding 3 holds:
//   flat, single-sample   sampler2D         (DepthOnlyView)
//   flat, multisample     sampler2DMS       (DepthOnlyView)
//   layered               sampler2DArray    (DepthOnlyArrayView, one layer per eye,
//   layered, multisample  sampler2DMSArray   indexed by the multiview view index)
//
// texelFetch at the fragment's own pixel: the depth image IS the pass's depth
// attachment, so gl_FragCoord.xy is its texel with no scene-viewport scaling.
// Multisample reads sample 0, as volumetricbeam.fp and the SSAO depth pass do.
static const char *sceneDepthBindings = R"(

	#if defined(SCENE_DEPTH_LAYERED) && defined(SCENE_DEPTH_MULTISAMPLE)
	layout(set = 0, binding = 3) uniform sampler2DMSArray SceneDepthTexture;
	#elif defined(SCENE_DEPTH_LAYERED)
	layout(set = 0, binding = 3) uniform sampler2DArray SceneDepthTexture;
	#elif defined(SCENE_DEPTH_MULTISAMPLE)
	layout(set = 0, binding = 3) uniform sampler2DMS SceneDepthTexture;
	#else
	layout(set = 0, binding = 3) uniform sampler2D SceneDepthTexture;
	#endif

	// The raw 0..1 depth-buffer value of the scene under this fragment.
	float SceneDepthRaw()
	{
		ivec2 texelPos = ivec2(gl_FragCoord.xy);
	#if defined(SCENE_DEPTH_LAYERED)
		return texelFetch(SceneDepthTexture, ivec3(texelPos, HW_VIEWPOINT_INDEX), 0).x;
	#else
		return texelFetch(SceneDepthTexture, texelPos, 0).x;
	#endif
	}

	// A raw depth value as distance along the view axis, map units: the post
	// passes' LinearizeDepthA/B, carried in uLinearizeDepth. Put a fragment's own
	// gl_FragCoord.z through the same function to compare it with the scene.
	float SceneDepthLinear(float rawDepth)
	{
		return 1.0 / (clamp(rawDepth, 0.0, 1.0) * uLinearizeDepth.x + uLinearizeDepth.y);
	}
)";

std::unique_ptr<VulkanShader> VkShaderManager::LoadVertShader(FString shadername, const char *vert_lump, const char *defines)
{
	FString code = GetTargetGlslVersion();
	code << "#extension GL_GOOGLE_include_directive : enable\n";
	// [UZDXREMA] shaderBindings below keys the SUPPORTS_MULTIVIEW per-eye
	// viewpoint index off VERTEX_SHADER (flat out vs flat in hwViewIndex).
	code << "#define VERTEX_SHADER\n";
	code << defines;
	code << "\n#define MAX_STREAM_DATA " << std::to_string(MAX_STREAM_DATA).c_str() << "\n";
#ifdef NPOT_EMULATION
	code << "#define NPOT_EMULATION\n";
#endif
	code << shaderBindings;
	if (!fb->device->EnabledFeatures.Features.shaderClipDistance) code << "#define NO_CLIPDISTANCE_SUPPORT\n";
	code << "#line 1\n";
	code << LoadPrivateShaderLump(vert_lump).GetChars() << "\n";

	return ShaderBuilder()
		.Type(ShaderType::Vertex)
		.AddSource(shadername.GetChars(), code.GetChars())
		.DebugName(shadername.GetChars())
		.OnIncludeLocal(OnInclude)
		.OnIncludeSystem(OnInclude)
		.Create(shadername.GetChars(), fb->device.get());
}

std::unique_ptr<VulkanShader> VkShaderManager::LoadFragShader(FString shadername, const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass, bool sceneDepth)
{
	FString code = GetTargetGlslVersion();
	code << "#extension GL_GOOGLE_include_directive : enable\n";
	if (fb->RaytracingEnabled())
		code << "\n#define SUPPORTS_RAYTRACING\n";
	code << "#define FRAGMENT_SHADER\n";
	code << defines;
	code << "\n$placeholder$";	// here the code can later add more needed #defines.
	code << "\n#define MAX_STREAM_DATA " << std::to_string(MAX_STREAM_DATA).c_str() << "\n";
#ifdef NPOT_EMULATION
	code << "#define NPOT_EMULATION\n";
#endif
	code << shaderBindings;
	FString placeholder = "\n";

	if (!fb->device->EnabledFeatures.Features.shaderClipDistance) code << "#define NO_CLIPDISTANCE_SUPPORT\n";
	if (!alphatest) code << "#define NO_ALPHATEST\n";
	if (gbufferpass) code << "#define GBUFFER_PASS\n";

	// [2a] Only an effect's scene-depth variant gets the scene depth declaration;
	// see sceneDepthBindings. After the prolog, so it can use HW_VIEWPOINT_INDEX
	// and uLinearizeDepth; before the effect's own lump, which calls it.
	if (sceneDepth) code << sceneDepthBindings;

	// [STAMP] The surface-stamp shape library, ahead of main.fp so main.fp can
	// call it without a forward declaration. It needs only what the preamble
	// above already provides -- timer, the viewpoint block, gl_FragCoord -- and
	// nothing main.fp declares, which is what makes prepending safe. Mirrors
	// gl_shader.cpp; both backends read the same lump.
	code << "\n#line 1\n";
	code << LoadPrivateShaderLump("shaders/glsl/func_surfacestamps.fp").GetChars() << "\n";

	code << "\n#line 1\n";
	code << LoadPrivateShaderLump("shaders/glsl/func_spriteoutline.fp").GetChars() << "\n";

	code << "\n#line 1\n";
	code << LoadPrivateShaderLump(frag_lump).GetChars() << "\n";

	if (material_lump)
	{
		if (material_lump[0] != '#')
		{
			FString pp_code = LoadPublicShaderLump(material_lump);

			if (pp_code.IndexOf("ProcessMaterial") < 0 && pp_code.IndexOf("SetupMaterial") < 0)
			{
				// this looks like an old custom hardware shader.
				// add ProcessMaterial function that calls the older ProcessTexel function

				if (pp_code.IndexOf("GetTexCoord") >= 0)
				{
					code << "\n" << LoadPrivateShaderLump("shaders/glsl/func_defaultmat2.fp").GetChars() << "\n";
				}
				else
				{
					code << "\n" << LoadPrivateShaderLump("shaders/glsl/func_defaultmat.fp").GetChars() << "\n";
					if (pp_code.IndexOf("ProcessTexel") < 0)
					{
						// this looks like an even older custom hardware shader.
						// We need to replace the ProcessTexel call to make it work.

						code.Substitute("material.Base = ProcessTexel();", "material.Base = Process(vec4(1.0));");
					}
				}

				if (pp_code.IndexOf("ProcessLight") >= 0)
				{
					// The ProcessLight signatured changed. Forward to the old one.
					code << "\nvec4 ProcessLight(vec4 color);\n";
					code << "\nvec4 ProcessLight(Material material, vec4 color) { return ProcessLight(color); }\n";
				}
			}

			code << "\n#line 1\n";
			code << RemoveLegacyUserUniforms(pp_code).GetChars();
			code.Substitute("gl_TexCoord[0]", "vTexCoord");	// fix old custom shaders.

			if (pp_code.IndexOf("ProcessLight") < 0)
			{
				code << "\n" << LoadPrivateShaderLump("shaders/glsl/func_defaultlight.fp").GetChars() << "\n";
			}

			// ProcessMaterial must be considered broken because it requires the user to fill in data they possibly cannot know all about.
			if (pp_code.IndexOf("ProcessMaterial") >= 0 && pp_code.IndexOf("SetupMaterial") < 0)
			{
				// This reactivates the old logic and disables all features that cannot be supported with that method.
				placeholder << "#define LEGACY_USER_SHADER\n";
			}
		}
		else
		{
			// material_lump is not a lump name but the source itself (from generated shaders)
			code << (material_lump + 1) << "\n";
		}
	}
	code.Substitute("$placeholder$", placeholder);

	if (light_lump)
	{
		code << "\n#line 1\n";
		code << LoadPrivateShaderLump(light_lump).GetChars();
	}

	return ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource(shadername.GetChars(), code.GetChars())
		.DebugName(shadername.GetChars())
		.OnIncludeLocal(OnInclude)
		.OnIncludeSystem(OnInclude)
		.Create(shadername.GetChars(), fb->device.get());
}

FString VkShaderManager::GetTargetGlslVersion()
{
	FString version;
	if (fb->device->Instance->ApiVersion == VK_API_VERSION_1_2)
	{
		version = "#version 460\n#extension GL_EXT_ray_query : enable\n";
	}
	else
	{
		version = "#version 450 core\n";
	}

	if (fb->device->EnabledFeatures.Multiview.multiview)
	{
		version << "#extension GL_EXT_multiview : require\n#define SUPPORTS_MULTIVIEW\n";
	}

	return version;
}

FString VkShaderManager::LoadPublicShaderLump(const char *lumpname)
{
	int lump = fileSystem.CheckNumForFullName(lumpname, 0);
	if (lump == -1) lump = fileSystem.CheckNumForFullName(lumpname);
	if (lump == -1) I_Error("Unable to load '%s'", lumpname);
	return GetStringFromLump(lump);
}

FString VkShaderManager::LoadPrivateShaderLump(const char *lumpname)
{
	int lump = fileSystem.CheckNumForFullName(lumpname, 0);
	if (lump == -1) I_Error("Unable to load '%s'", lumpname);
	return GetStringFromLump(lump);
}

VkPPShader* VkShaderManager::GetVkShader(PPShader* shader)
{
	if (!shader->Backend)
		shader->Backend = std::make_unique<VkPPShader>(fb, shader);
	return static_cast<VkPPShader*>(shader->Backend.get());
}

void VkShaderManager::AddVkPPShader(VkPPShader* shader)
{
	shader->it = PPShaders.insert(PPShaders.end(), shader);
}

void VkShaderManager::RemoveVkPPShader(VkPPShader* shader)
{
	shader->Reset();
	shader->fb = nullptr;
	PPShaders.erase(shader->it);
}
