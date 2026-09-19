/*
** vk_renderpass.h
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

#include "zvulkan/vulkanobjects.h"
#include "renderstyle.h"
#include "hwrenderer/data/buffers.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hw_renderstate.h"
#include <string.h>
#include <map>

class VulkanRenderDevice;
class VkPPShader;
class GraphicsPipelineBuilder;
class ColorBlendAttachmentBuilder;

class VkPipelineKey
{
public:
	FRenderStyle RenderStyle;
	int SpecialEffect;
	int EffectState;
	int AlphaTest;
	int DepthWrite;
	int DepthTest;
	int DepthFunc;
	int DepthClamp;
	int DepthBias;
	int StencilTest;
	int StencilPassOp;
	int ColorMask;
	int CullMode;
	int VertexFormat;
	int DrawType;
	int NumTextureLayers;

	bool operator<(const VkPipelineKey &other) const { return memcmp(this, &other, sizeof(VkPipelineKey)) < 0; }
	bool operator==(const VkPipelineKey &other) const { return memcmp(this, &other, sizeof(VkPipelineKey)) == 0; }
	bool operator!=(const VkPipelineKey &other) const { return memcmp(this, &other, sizeof(VkPipelineKey)) != 0; }
};

class VkRenderPassKey
{
public:
	int DepthStencil;
	int Samples;
	int DrawBuffers;
	int Layers;
	uint32_t ViewMask;
	VkFormat DrawBufferFormat;
	// [2a] Nonzero: the subpass holds the depth/stencil attachment as
	// DEPTH_STENCIL_READ_ONLY_OPTIMAL, never clears or writes it, and effects
	// drawn in it may sample the scene depth (fixed binding 3). Set only between
	// FRenderState::SetSceneDepthReadable(true) and (false); zero -- the pass as
	// it was before 2a -- everywhere else. An int after the last int-sized member,
	// so this memcmp'd key (and VkRenderTargetFramebufferKey) gains no padding.
	int DepthReadOnly;
	// [LIGHTMASK] Nonzero: the pass carries the light mask attachment (hw_postprocess.h,
	// PPLightMask) after the draw buffers and before depth -- colour attachments target,
	// [SceneFog, SceneNormal], mask; depth at DrawBuffers + 1 -- and its pipelines draw with the
	// scene programs' SCENE_LIGHT_MASK variants. Set only on the main view's scene target
	// while the frame draws the mask; zero everywhere else, so every other pass keeps exactly
	// the key, render pass, framebuffer and pipelines it had. An int after DepthReadOnly, so
	// this memcmp'd key (and VkRenderTargetFramebufferKey) still has no padding.
	int LightMask;
	// [SCENEMASK] Nonzero: the pass carries the scene mask attachment (hw_postprocess.h, PPSceneMask) --
	// the per-pixel tag -- after the draw buffers and after the light mask, before depth: colour
	// attachments target, [SceneFog, SceneNormal], light mask, tag; depth after all of them. Its
	// pipelines draw with the scene programs' SCENE_POST_MASK variants, and the tag attachment's blend
	// state is the one thing in the pass that does not blend. Set only on the main view's scene target
	// while the frame draws the tag; zero everywhere else, so every other pass keeps exactly the key,
	// render pass, framebuffer and pipelines it had. An int after LightMask, so this memcmp'd key (and
	// VkRenderTargetFramebufferKey) still has no padding.
	int PostMask;

	bool operator<(const VkRenderPassKey &other) const { return memcmp(this, &other, sizeof(VkRenderPassKey)) < 0; }
	bool operator==(const VkRenderPassKey &other) const { return memcmp(this, &other, sizeof(VkRenderPassKey)) == 0; }
	bool operator!=(const VkRenderPassKey &other) const { return memcmp(this, &other, sizeof(VkRenderPassKey)) != 0; }
};

// [LIGHTMASK] [SCENEMASK] Nine int-sized members and no padding: memcmp compares only bytes that were set.
static_assert(sizeof(VkRenderPassKey) == 9 * sizeof(int), "VkRenderPassKey must stay padding-free (it is compared with memcmp)");

class VkRenderPassSetup
{
public:
	VkRenderPassSetup(VulkanRenderDevice* fb, const VkRenderPassKey &key);

	VulkanRenderPass *GetRenderPass(int clearTargets);
	VulkanPipeline *GetPipeline(const VkPipelineKey &key);

	VkRenderPassKey PassKey;
	std::unique_ptr<VulkanRenderPass> RenderPasses[8];
	std::map<VkPipelineKey, std::unique_ptr<VulkanPipeline>> Pipelines;

private:
	std::unique_ptr<VulkanRenderPass> CreateRenderPass(int clearTargets);
	std::unique_ptr<VulkanPipeline> CreatePipeline(const VkPipelineKey &key);

	VulkanRenderDevice* fb = nullptr;
};

class VkVertexFormat
{
public:
	int NumBindingPoints;
	size_t Stride;
	std::vector<FVertexBufferAttribute> Attrs;
	int UseVertexData;
};

enum class WhichDepthStencil;

class VkPPRenderPassKey
{
public:
	VkPPShader* Shader;
	int Uniforms;
	int InputTextures;
	PPBlendMode BlendMode;
	VkFormat OutputFormat;
	int SwapChain;
	int ShadowMapBuffers;
	WhichDepthStencil StencilTest;
	VkSampleCountFlagBits Samples;
	int Layers;
	uint32_t ViewMask;

	bool operator<(const VkPPRenderPassKey& other) const { return memcmp(this, &other, sizeof(VkPPRenderPassKey)) < 0; }
	bool operator==(const VkPPRenderPassKey& other) const { return memcmp(this, &other, sizeof(VkPPRenderPassKey)) == 0; }
	bool operator!=(const VkPPRenderPassKey& other) const { return memcmp(this, &other, sizeof(VkPPRenderPassKey)) != 0; }
};

class VkPPRenderPassSetup
{
public:
	VkPPRenderPassSetup(VulkanRenderDevice* fb, const VkPPRenderPassKey& key);

	std::unique_ptr<VulkanDescriptorSetLayout> DescriptorLayout;
	std::unique_ptr<VulkanPipelineLayout> PipelineLayout;
	std::unique_ptr<VulkanRenderPass> RenderPass;
	std::unique_ptr<VulkanPipeline> Pipeline;

private:
	void CreateDescriptorLayout(const VkPPRenderPassKey& key);
	void CreatePipelineLayout(const VkPPRenderPassKey& key);
	void CreatePipeline(const VkPPRenderPassKey& key);
	void CreateRenderPass(const VkPPRenderPassKey& key);

	VulkanRenderDevice* fb = nullptr;
};

ColorBlendAttachmentBuilder& BlendMode(ColorBlendAttachmentBuilder& builder, const FRenderStyle& style);

class VkRenderPassManager
{
public:
	VkRenderPassManager(VulkanRenderDevice* fb);
	~VkRenderPassManager();

	void RenderBuffersReset();

	VkRenderPassSetup *GetRenderPass(const VkRenderPassKey &key);
	int GetVertexFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs);
	VkVertexFormat *GetVertexFormat(int index);
	VulkanPipelineLayout* GetPipelineLayout(int numLayers);

	VkPPRenderPassSetup* GetPPRenderPass(const VkPPRenderPassKey& key);

	VulkanPipelineCache* GetCache() { return PipelineCache.get(); }

private:
	VulkanRenderDevice* fb = nullptr;

	std::map<VkRenderPassKey, std::unique_ptr<VkRenderPassSetup>> RenderPassSetup;
	std::vector<std::unique_ptr<VulkanPipelineLayout>> PipelineLayouts;
	std::vector<VkVertexFormat> VertexFormats;

	std::map<VkPPRenderPassKey, std::unique_ptr<VkPPRenderPassSetup>> PPRenderPassSetup;

	FString CacheFilename;
	std::unique_ptr<VulkanPipelineCache> PipelineCache;
};
