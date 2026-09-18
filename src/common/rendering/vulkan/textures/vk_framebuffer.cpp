/*
** vk_framebuffer.cpp
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

#include <zvulkan/vulkanobjects.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkanswapchain.h>
#include "v_video.h"
#include "hw_vrmodes.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "hw_cvars.h"
#include "vk_framebuffer.h"
#include "hw_effectsgovernor.h"	// [GOVERNOR] E8: the swapchain's acquire and present waits are pacing
#include "i_time.h"

CVAR(Bool, vk_hdr, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, vk_exclusivefullscreen, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
EXTERN_CVAR(Int, vr_desktop_view);
EXTERN_CVAR(Bool, vr_swap_eyes);

VkFramebufferManager::VkFramebufferManager(VulkanRenderDevice* fb) : fb(fb)
{
	SwapChain = VulkanSwapChainBuilder()
		.Create(fb->device.get());

	SwapChainImageAvailableSemaphore = SemaphoreBuilder()
		.DebugName("SwapChainImageAvailableSemaphore")
		.Create(fb->device.get());
}

VkFramebufferManager::~VkFramebufferManager()
{
}

void VkFramebufferManager::AcquireImage()
{
	bool exclusiveFullscreen = fb->IsFullscreen() && vk_exclusivefullscreen;
	if (SwapChain->Lost() || fb->GetClientWidth() != CurrentWidth || fb->GetClientHeight() != CurrentHeight || fb->GetVSync() != CurrentVSync || CurrentHdr != vk_hdr || CurrentExclusiveFullscreen != exclusiveFullscreen)
	{
		Framebuffers.clear();

		CurrentWidth = fb->GetClientWidth();
		CurrentHeight = fb->GetClientHeight();
		CurrentVSync = fb->GetVSync();
		CurrentHdr = vk_hdr;
		CurrentExclusiveFullscreen = exclusiveFullscreen;

		SwapChain->Create(CurrentWidth, CurrentHeight, CurrentVSync ? 2 : 3, CurrentVSync, CurrentHdr, CurrentExclusiveFullscreen);

		RenderFinishedSemaphores.clear();
		for (int i = 0; i < SwapChain->ImageCount(); i++)
		{
			RenderFinishedSemaphores.push_back(SemaphoreBuilder()
				.DebugName("RenderFinishedSemaphore")
				.Create(fb->device.get()));
		}
	}

	// [GOVERNOR] E8: a vsync swapchain holds its image until the display is ready: pacing, not the frame's work (hw_effectsgovernor.h).
	const uint64_t governorWaitStartNs = I_nsTime();
	PresentImageIndex = SwapChain->AcquireImage(SwapChainImageAvailableSemaphore.get());
	EffectsGovernor::AddPacingWait(I_nsTime() - governorWaitStartNs);
	if (PresentImageIndex != -1)
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		if (vrmode != nullptr && vrmode->IsVR() && vr_desktop_view != -1)
		{
			auto cmdbuffer = fb->GetCommands()->GetDrawCommands();
			auto dstImage = SwapChain->GetImage(PresentImageIndex);
			auto clearColorImage = &dstImage->image;
			VkImageMemoryBarrier dstBarrier = {};
			dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			dstBarrier.srcAccessMask = 0;
			dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			dstBarrier.image = *clearColorImage;
			dstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			cmdbuffer->pipelineBarrier(
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

			VkClearColorValue clearValue = {};
			VkImageSubresourceRange clearRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			cmdbuffer->clearColorImage(*clearColorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &clearRange);

			if (!vrmode->RenderDesktopMirror(fb, dstImage))
			{
				Printf("Vulkan present: desktop mirror composition failed, falling back to default present.\n");
				fb->GetPostprocess()->DrawPresentTexture(fb->mOutputLetterbox, true, false);
			}

			VkImageMemoryBarrier dstFinalBarrier = {};
			dstFinalBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			dstFinalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			dstFinalBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			dstFinalBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			dstFinalBarrier.dstAccessMask = 0;
			dstFinalBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			dstFinalBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			dstFinalBarrier.image = *clearColorImage;
			dstFinalBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			cmdbuffer->pipelineBarrier(
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				0, 0, nullptr, 0, nullptr, 1, &dstFinalBarrier);
		}
		else
		{
			fb->GetPostprocess()->DrawPresentTexture(fb->mOutputLetterbox, true, false);
		}
	}
}

void VkFramebufferManager::QueuePresent()
{
	if (PresentImageIndex != -1)
	{
		// [GOVERNOR] E8: a present can wait for the display as well: pacing.
		const uint64_t governorWaitStartNs = I_nsTime();
		SwapChain->QueuePresent(PresentImageIndex, RenderFinishedSemaphores[PresentImageIndex].get());
		EffectsGovernor::AddPacingWait(I_nsTime() - governorWaitStartNs);
	}
}
