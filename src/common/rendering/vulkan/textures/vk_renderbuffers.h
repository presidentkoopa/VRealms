/*
** vk_renderbuffers.h
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
#include "vulkan/textures/vk_imagetransition.h"

class VulkanRenderDevice;
class VkPPRenderPassSetup;
class PPOutput;

enum class WhichDepthStencil {
	None,
	Scene,
	Pipeline,
};

class VkRenderBuffers
{
public:
	VkRenderBuffers(VulkanRenderDevice* fb);
	~VkRenderBuffers();

	void BeginFrame(int width, int height, int sceneWidth, int sceneHeight, int sceneLayers = 1, int pipelineLayers = 1);

	int GetWidth() const { return mWidth; }
	int GetHeight() const { return mHeight; }
	int GetSceneWidth() const { return mSceneWidth; }
	int GetSceneHeight() const { return mSceneHeight; }
	int GetSceneLayers() const { return mSceneLayers; }
	int GetPipelineLayers() const { return mPipelineLayers; }
	VkSampleCountFlagBits GetSceneSamples() const { return mSamples; }

	VkTextureImage SceneColor;
	VkTextureImage SceneDepthStencil;
	VkTextureImage SceneNormal;
	VkTextureImage SceneFog;

	VkFormat PipelineDepthStencilFormat = VK_FORMAT_D24_UNORM_S8_UINT;
	VkFormat SceneDepthStencilFormat = VK_FORMAT_D24_UNORM_S8_UINT;
	VkFormat SceneNormalFormat = VK_FORMAT_A2R10G10B10_UNORM_PACK32;

	static const int NumPipelineImages = 4;
	VkTextureImage PipelineDepthStencil;
	VkTextureImage PipelineImage[NumPipelineImages];

	VulkanFramebuffer* GetOutput(VkPPRenderPassSetup* passSetup, const PPOutput& output, WhichDepthStencil stencilTest, int& framebufferWidth, int& framebufferHeight);

	// [LIGHTMASK] The light mask (hw_postprocess.h, PPLightMask). SceneLightMask is the scene
	// pass's extra colour attachment: the scene's size, samples and layers. LightMaskImage[2] is
	// the pair it is carried through post-processing in, at the pipeline images' size and layers
	// (VkPostprocess::GetCurrentLightMaskImage says which holds it). All null until something
	// asks for the mask; from then on they follow every re-creation of the scene and pipeline
	// images and are never freed on their own, so turning the mask off and on costs nothing.
	// [1] is made the first time a carry needs it (the heat shimmer). Screen buffers only.
	VkTextureImage SceneLightMask;
	VkTextureImage LightMaskImage[2];
	VkFormat LightMaskFormat = VK_FORMAT_UNDEFINED;

	bool CreateLightMask(VkFormat format);
	bool CreateLightMaskCarry();
	bool HasLightMask() const { return SceneLightMask.Image && LightMaskImage[0].Image; }

private:
	void CreateSceneLightMask(int width, int height, VkSampleCountFlagBits samples, int layers);
	void CreateLightMaskImage(int index, int width, int height, int layers);
	void RefuseLightMask(const char *what);
	bool mLightMaskWanted = false;
	bool mLightMaskRefused = false;

	void CreatePipelineDepthStencil(int width, int height, int layers);
	void CreatePipeline(int width, int height, int layers);
	void CreateScene(int width, int height, VkSampleCountFlagBits samples, int layers);
	void CreateSceneColor(int width, int height, VkSampleCountFlagBits samples, int layers);
	void CreateSceneDepthStencil(int width, int height, VkSampleCountFlagBits samples, int layers);
	void CreateSceneFog(int width, int height, VkSampleCountFlagBits samples, int layers);
	void CreateSceneNormal(int width, int height, VkSampleCountFlagBits samples, int layers);
	VkSampleCountFlagBits GetBestSampleCount();

	VulkanRenderDevice* fb = nullptr;

	int mWidth = 0;
	int mHeight = 0;
	int mSceneWidth = 0;
	int mSceneHeight = 0;
	int mSceneLayers = 1;
	int mPipelineLayers = 1;
	VkSampleCountFlagBits mSamples = VK_SAMPLE_COUNT_1_BIT;
};
