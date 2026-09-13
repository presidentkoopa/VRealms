/*
** vk_imagetransition.h
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

#include <vector>

#include "zvulkan/vulkanobjects.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/renderer/vk_renderpass.h"

class VkTextureImage
{
public:
	struct VkRenderTargetFramebufferKey
	{
		VkRenderPassKey PassKey = {};
		int LayerIndex = -1;

		bool operator<(const VkRenderTargetFramebufferKey& other) const { return memcmp(this, &other, sizeof(VkRenderTargetFramebufferKey)) < 0; }
	};

	struct VkPPOutputFramebufferKey
	{
		int LayerIndex = -1;
		int DepthStencilMode = 0;

		bool operator<(const VkPPOutputFramebufferKey& other) const { return memcmp(this, &other, sizeof(VkPPOutputFramebufferKey)) < 0; }
	};

	void Reset(VulkanRenderDevice* fb)
	{
		AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		auto deletelist = fb->GetCommands()->DrawDeleteList.get();
		deletelist->Add(std::move(PPFramebuffer));
		deletelist->Add(std::move(FramebufferView));
		deletelist->Add(std::move(ArrayView));
		for (auto& it : PPOutputFramebuffers)
			deletelist->Add(std::move(it.second));
		PPOutputFramebuffers.clear();
		for (auto &it : RSFramebuffers)
			deletelist->Add(std::move(it.second));
		RSFramebuffers.clear();
		for (auto& it : LayerViews)
			deletelist->Add(std::move(it));
		LayerViews.clear();
		for (auto& it : LayerDepthOnlyViews)
			deletelist->Add(std::move(it));
		LayerDepthOnlyViews.clear();
		deletelist->Add(std::move(DepthOnlyArrayView));	// [2a]
		deletelist->Add(std::move(DepthOnlyView));
		deletelist->Add(std::move(View));
		deletelist->Add(std::move(Image));
	}

	void GenerateMipmaps(VulkanCommandBuffer *cmdbuffer);
	VulkanImageView* GetFramebufferView() const { return FramebufferView ? FramebufferView.get() : View.get(); }
	VulkanImageView* GetLayerView(int layer) const
	{
		return layer >= 0 && layer < (int)LayerViews.size() && LayerViews[layer] ? LayerViews[layer].get() : View.get();
	}
	VulkanImageView* GetLayerDepthOnlyView(int layer) const
	{
		return layer >= 0 && layer < (int)LayerDepthOnlyViews.size() && LayerDepthOnlyViews[layer] ? LayerDepthOnlyViews[layer].get() : DepthOnlyView.get();
	}

	std::unique_ptr<VulkanImage> Image;
	std::unique_ptr<VulkanImageView> View;
	std::unique_ptr<VulkanImageView> FramebufferView;
	std::unique_ptr<VulkanImageView> ArrayView;
	std::unique_ptr<VulkanImageView> DepthOnlyView;
	// [2a] Depth aspect only, 2D_ARRAY over every layer: what a layered (multiview)
	// read of the scene depth samples, one layer per eye (fixed binding 3,
	// VkDescriptorSetManager::UpdateFixedSet). Created by CreateDepthTargetViews
	// for depth images with more than one layer; null otherwise.
	std::unique_ptr<VulkanImageView> DepthOnlyArrayView;
	std::vector<std::unique_ptr<VulkanImageView>> LayerViews;
	std::vector<std::unique_ptr<VulkanImageView>> LayerDepthOnlyViews;
	VkImageLayout Layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageAspectFlags AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	std::unique_ptr<VulkanFramebuffer> PPFramebuffer;
	std::map<VkPPOutputFramebufferKey, std::unique_ptr<VulkanFramebuffer>> PPOutputFramebuffers;
	std::map<VkRenderTargetFramebufferKey, std::unique_ptr<VulkanFramebuffer>> RSFramebuffers;
};

class VkImageTransition
{
public:
	VkImageTransition& AddImage(VkTextureImage *image, VkImageLayout targetLayout, bool undefinedSrcLayout, int baseMipLevel = 0, int levelCount = 1);
	void Execute(VulkanCommandBuffer *cmdbuffer);

private:
	PipelineBarrier barrier;
	VkPipelineStageFlags srcStageMask = 0;
	VkPipelineStageFlags dstStageMask = 0;
	bool needbarrier = false;
};
