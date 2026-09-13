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

	VkPPShader* GetVkShader(PPShader* shader);

	void AddVkPPShader(VkPPShader* shader);
	void RemoveVkPPShader(VkPPShader* shader);

private:
	std::unique_ptr<VulkanShader> LoadVertShader(FString shadername, const char *vert_lump, const char *defines);
	// [2a] sceneDepth adds the effect-scoped scene depth declaration (binding 3 of
	// the fixed set) after the shared prolog; the caller's defines pick the variant.
	// Default false, so every existing caller compiles exactly what it did.
	std::unique_ptr<VulkanShader> LoadFragShader(FString shadername, const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass, bool sceneDepth = false);

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
	uint8_t compilePass = 0, compileState = 0;
	int compileIndex = 0;

	std::list<VkPPShader*> PPShaders;
	friend class VkPPShader;
};
