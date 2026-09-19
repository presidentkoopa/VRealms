/*
** vk_shader.h
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

#pragma once

#include <memory>

#include "vectors.h"
#include "matrix.h"
#include "name.h"
#include "hw_renderstate.h"
#include "zvulkan/vulkanbuilders.h"
#include <list>

#define SHADER_MIN_REQUIRED_TEXTURE_LAYERS 11

class VulkanRenderDevice;
class VulkanDevice;
class VulkanShader;
class VkPPShader;
class PPShader;

struct MatricesUBO
{
	VSMatrix ModelMatrix;
	VSMatrix NormalModelMatrix;
	VSMatrix TextureMatrix;
};

#define MAX_STREAM_DATA ((int)(65536 / sizeof(StreamData)))

struct StreamUBO
{
	StreamData data[MAX_STREAM_DATA];
};

struct PushConstants
{
	int uTextureMode;
	float uAlphaThreshold;
	FVector2 uClipSplit;

	// Lighting + Fog
	float uLightLevel;
	float uFogDensity;
	float uLightFactor;
	float uLightDist;
	int uFogEnabled;

	// dynamic lights
	int uLightIndex;

	// Blinn glossiness and specular level
	FVector2 uSpecularMaterial;

	// bone animation
	int uBoneIndexBase;

	int uDataIndex;
	int padding2, padding3;
};

class VkShaderProgram
{
public:
	std::unique_ptr<VulkanShader> vert;
	std::unique_ptr<VulkanShader> frag;
};

class VkShaderManager
{
public:
	VkShaderManager(VulkanRenderDevice* fb);
	~VkShaderManager();

	void Deinit();

	VkShaderProgram *GetEffect(int effect, EPassType passType);
	// [2a] The fragment shader an effect draws with inside a read-only scene depth
	// pass (FRenderState::SetSceneDepthReadable): its scene-depth variant for that
	// pass's sample count and layering, or null when the effect has no variants or
	// they did not compile -- VkRenderPassSetup::CreatePipeline then keeps the
	// effect's ordinary fragment shader. See EffectHasSceneDepthVariants.
	VulkanShader *GetSceneDepthEffectFrag(int effect, EPassType passType, bool multisample, bool layered);
	VkShaderProgram *Get(unsigned int eff, bool alphateston, EPassType passType);
	bool CompileNextShader();

	// [LIGHTMASK] [SCENEMASK] THE SCENE-EXTRA PROGRAMS: every scene fragment program -- materials,
	// NAT materials, user shaders, effects and their scene-depth variants -- once more for a scene
	// pass that carries an extra colour attachment. SCENE_LIGHT_MASK adds the light mask output at
	// LIGHT_MASK_LOCATION (hw_postprocess.h, PPLightMask); SCENE_POST_MASK adds the per-pixel tag at
	// POST_MASK_LOCATION (PPSceneMask, "Engine docs/SCENE_MASK_PLAN.md").
	//
	// Keyed by WHICH extras the pass carries, not one set per feature, because the two are not
	// independent: with both on, the tag's location is one further along. `extras` is the
	// SCENE_EXTRA_* bits and is 1, 2 or 3; 0 is the ordinary program and is never compiled here.
	//
	// Fragment only: each draws with its ordinary program's vertex shader. Compiled for ONE pass
	// type and ONE combination, synchronously, the first time a frame wants it
	// (VulkanRenderDevice::BeginFrame): a one-time pause at the switch-on, never while it stays on.
	// Any failure leaves that pass and that combination not ready for the session, with one red
	// line, and the frame's decision keeps it off. The Get* return null for anything not ready.
	enum
	{
		SCENE_EXTRA_LIGHT_MASK = 1,
		SCENE_EXTRA_POST_MASK = 2,
		SCENE_EXTRA_SETS = 4,	// index by the bits, so 0 (unused) .. 3
	};
	bool IsCompileDone() const { return compileIndex == -1; }
	bool CompileSceneExtraPrograms(EPassType passType, int extras);
	bool SceneExtraProgramsReady(EPassType passType, int extras) const;
	VulkanShader *GetSceneExtraFrag(unsigned int eff, bool alphateston, EPassType passType, int extras);
	VulkanShader *GetSceneExtraEffectFrag(int effect, EPassType passType, int extras);
	VulkanShader *GetSceneExtraSceneDepthEffectFrag(int effect, EPassType passType, bool multisample, bool layered, int extras);

	VkPPShader* GetVkShader(PPShader* shader);

	void AddVkPPShader(VkPPShader* shader);
	void RemoveVkPPShader(VkPPShader* shader);

private:
	std::unique_ptr<VulkanShader> LoadVertShader(FString shadername, const char *vert_lump, const char *defines);
	// [2a] sceneDepth adds the effect-scoped scene depth declaration (binding 3 of
	// the fixed set) after the shared prolog; the caller's defines pick the variant.
	// Default false, so every existing caller compiles exactly what it did.
	// [LIGHTMASK] lightMask adds SCENE_LIGHT_MASK and LIGHT_MASK_LOCATION (CompileSceneExtraPrograms).
	// [SCENEMASK] postMask adds SCENE_POST_MASK and POST_MASK_LOCATION, which is one further along when
	// lightMask is on too -- that is the whole reason the program sets are keyed by the combination.
	// Both default false, so every existing caller compiles exactly what it did.
	std::unique_ptr<VulkanShader> LoadFragShader(FString shadername, const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass, bool sceneDepth = false, bool lightMask = false, bool postMask = false);

	FString GetTargetGlslVersion();
	FString LoadPublicShaderLump(const char *lumpname);
	FString LoadPrivateShaderLump(const char *lumpname);

	static ShaderIncludeResult OnInclude(FString headerName, FString includerName, size_t depth);

	VulkanRenderDevice* fb = nullptr;

	std::vector<VkShaderProgram> mMaterialShaders[MAX_PASS_TYPES];
	std::vector<VkShaderProgram> mMaterialShadersNAT[MAX_PASS_TYPES];
	std::vector<VkShaderProgram> mEffectShaders[MAX_PASS_TYPES];
	// [2a] Scene-depth fragment variants, [pass][effect][variant], where variant is
	// (multisample ? 1 : 0) | (layered ? 2 : 0). Only effects that read scene depth
	// get them (EffectHasSceneDepthVariants in vk_shader.cpp); the rest stay null.
	// Fragment only: the variant draws with the effect's own vertex shader.
	static constexpr int SCENE_DEPTH_VARIANTS = 4;
	std::unique_ptr<VulkanShader> mSceneDepthEffectFrag[MAX_PASS_TYPES][MAX_EFFECTS][SCENE_DEPTH_VARIANTS];
	// [LIGHTMASK] [SCENEMASK] The scene-extra variants of the fragment shaders above, in the same order
	// and indexing (see CompileSceneExtraPrograms), one set per pass type per SCENE_EXTRA_* combination.
	// Empty / null until that pass and that combination are compiled, which only ever happens when a
	// frame asks for it: with neither mask wanted, not one of these is ever filled.
	std::vector<std::unique_ptr<VulkanShader>> mSceneExtraMaterialFrag[MAX_PASS_TYPES][SCENE_EXTRA_SETS];
	std::vector<std::unique_ptr<VulkanShader>> mSceneExtraMaterialFragNAT[MAX_PASS_TYPES][SCENE_EXTRA_SETS];
	std::unique_ptr<VulkanShader> mSceneExtraEffectFrag[MAX_PASS_TYPES][SCENE_EXTRA_SETS][MAX_EFFECTS];
	std::unique_ptr<VulkanShader> mSceneExtraSceneDepthEffectFrag[MAX_PASS_TYPES][SCENE_EXTRA_SETS][MAX_EFFECTS][SCENE_DEPTH_VARIANTS];
	enum { SCENE_EXTRA_NOT_COMPILED, SCENE_EXTRA_READY, SCENE_EXTRA_FAILED };
	int mSceneExtraState[MAX_PASS_TYPES][SCENE_EXTRA_SETS] = {};
	uint8_t compilePass = 0, compileState = 0;
	int compileIndex = 0;

	std::list<VkPPShader*> PPShaders;
	friend class VkPPShader;
	// [SMOKEVOLUME] The compute pass runner (vk_compute.cpp) compiles its lumps with the same
	// #include resolver as every other shader, so compute lumps share the house GLSL includes.
	friend class VkComputeManager;
};
