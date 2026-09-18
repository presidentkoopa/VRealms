/*
** vk_postprocess.h
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

#include <functional>
#include <map>
#include <array>

#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "zvulkan/vulkanobjects.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/textures/vk_imagetransition.h"

class FString;

class VkPPShader;
class VkPPTexture;
class PipelineBarrier;
class VulkanRenderDevice;
class VulkanCommandBuffer;

class VkPostprocess
{
public:
	VkPostprocess(VulkanRenderDevice* fb);
	~VkPostprocess();

	void SetActiveRenderTarget();
	void PostProcessScene(int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D);

	void AmbientOccludeScene(float m5);
	void BlurScene(float gameinfobluramount);
	void ClearTonemapPalette();

	void UpdateShadowMap();

	void ImageTransitionScene(bool undefinedSrcLayout);

	void BlitSceneToPostprocess();
	void BlitCurrentToImage(VkTextureImage *image, VkImageLayout finallayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	void CopyCurrentToImage(VkTextureImage *image, VkImageLayout finallayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	void DrawPresentTexture(const IntRect &box, bool applyGamma, bool screenshot);
	void DrawPresentTextureToImage(VkTextureImage *image, VkFormat outputFormat, const IntRect &box, bool applyGamma, bool screenshot, float sourceScaleX, float sourceScaleY, float sourceOffsetX, float sourceOffsetY, VulkanCommandBuffer *cmdbuffer, bool applyOpenXrBias = true);
	void DrawSpectatorToImage(VkTextureImage *image, VkFormat outputFormat, const IntRect &box, const float specRot[16], const FVector4 &srcTan, const FVector4 &dstTan, float sourceScaleX, float sourceScaleY, float sourceOffsetX, float sourceOffsetY, VulkanCommandBuffer *cmdbuffer);	// [SPECTATOR]

	int GetCurrentPipelineImage() const { return mCurrentPipelineImage; }
	int GetNextPipelineImage() const;
	void SetCurrentPipelineImage(int index);
	void SetPipelineImagePair(int start, int size = 2);
	void AdvancePipelineImage();

	// [LIGHTMASK] Which of VkRenderBuffers::LightMaskImage[2] holds the light mask so far for
	// the eye being post-processed. The scene transfer and each eye's start set 0; a carry that
	// writes LightMaskNext swaps it (VkPPRenderState::Draw).
	int GetCurrentLightMaskImage() const { return mCurrentLightMaskImage; }
	void SetCurrentLightMaskImage(int index) { mCurrentLightMaskImage = index & 1; }
	void AdvanceLightMaskImage() { mCurrentLightMaskImage ^= 1; }

	VulkanBuffer* GetAutomaticUniformsBuffer() { return AutomaticUniformsBuffer.get(); }

private:
	void NextEye(int eyeCount);

private:
	VulkanRenderDevice* fb = nullptr;

	int mCurrentPipelineImage = 0;
	int mPipelinePairStart = 0;
	int mPipelinePairSize = 2;
	int mCurrentLightMaskImage = 0;	// [LIGHTMASK]

	std::unique_ptr<VulkanBuffer> AutomaticUniformsBuffer;

	friend class VkPPRenderState;
};
