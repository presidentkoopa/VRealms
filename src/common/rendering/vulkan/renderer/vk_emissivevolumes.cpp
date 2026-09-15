/*
** vk_emissivevolumes.cpp
**
** [EMISSIVEVOLUMES] The Vulkan side of emissive volumes. See the header.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include <algorithm>
#include <cstring>
#include <exception>
#include <vector>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkanobjects.h>
#include "vk_emissivevolumes.h"
#include "vk_compute.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "hw_emissivevolumeframe.h"
#include "hw_emissivevolumecore.h"
#include "printf.h"
#include "v_text.h"

VkEmissiveVolumes::VkEmissiveVolumes(VkComputeManager* compute) : mCompute(compute), fb(compute->GetDevice())
{
}

VkEmissiveVolumes::~VkEmissiveVolumes()
{
	// The device is idle by now (the compute manager's owner waits for it): the images and the staging buffer go directly.
	EmissiveVolumesStatus() = EmissiveVolumeBackendStatus();
}

void VkEmissiveVolumes::Refuse(const char* what)
{
	if (!mRefused)
		Printf(TEXTCOLOR_RED "EmissiveVolumes: could not make %s -- no emissive volume draws this session\n", what);
	mRefused = true;
}

void VkEmissiveVolumes::Run(const EmissiveVolumeFrame& frame)
{
	EmissiveVolumeBackendStatus& status = EmissiveVolumesStatus();
	status.ListReady = false;
	status.Count = 0;
	status.Serial = 0;
	status.Refused = mRefused;

	const int count = frame.Texels != nullptr ? std::clamp(frame.Count, 0, EMISSIVE_VOLUMES_DRAWN_MAX) : 0;
	if (count == 0 || mRefused || !EnsureNoise() || !EnsureList())
	{
		status.Refused = mRefused;
		return;
	}

	const size_t floats = (size_t)EMISSIVE_VOLUMES_DRAWN_MAX * EMISSIVE_VOLUME_TEXELS * 4;
	if (mList.Layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || mListSerial != frame.Serial ||
		mListTexels.size() != floats || memcmp(mListTexels.data(), frame.Texels, floats * sizeof(float)) != 0)
	{
		// The copy commands of the frame before have finished (the frame's end waits on the GPU), as the beam list relies on.
		const size_t bytes = floats * sizeof(float);
		void* data = mListStaging->Map(0, bytes);
		memcpy(data, frame.Texels, bytes);
		mListStaging->Unmap();

		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = (uint32_t)EMISSIVE_VOLUMES_DRAWN_MAX;
		region.imageExtent.height = (uint32_t)EMISSIVE_VOLUME_TEXELS;
		region.imageExtent.depth = 1;

		mCompute->BeginWork();	// outside any render pass, before the eye loop
		const bool fresh = mList.Layout == VK_IMAGE_LAYOUT_UNDEFINED;
		PipelineBarrier toTransfer;
		toTransfer.AddImage(mList.Image.get(), mList.Layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			fresh ? 0 : VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		toTransfer.Execute(fb->GetCommands()->GetDrawCommands(), fresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT);
		fb->GetCommands()->GetDrawCommands()->copyBufferToImage(mListStaging->buffer, mList.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		PipelineBarrier toRead;
		toRead.AddImage(mList.Image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		toRead.Execute(fb->GetCommands()->GetDrawCommands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		mList.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		mListTexels.assign(frame.Texels, frame.Texels + floats);
		mListSerial = frame.Serial;
	}

	status.ListReady = mNoise.Layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && mList.Layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	status.Count = count;
	status.Serial = frame.Serial;
}

bool VkEmissiveVolumes::EnsureList()
{
	if (mList.Image)
		return true;
	const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
	try
	{
		mList.Image = ImageBuilder()
			.Size(EMISSIVE_VOLUMES_DRAWN_MAX, EMISSIVE_VOLUME_TEXELS)
			.Format(format)
			.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			.DebugName("EmissiveVolumes.List")
			.TryCreate(fb->device.get());
		if (mList.Image)
		{
			mList.View = ImageViewBuilder()
				.Image(mList.Image.get(), format)
				.DebugName("EmissiveVolumes.List")
				.Create(fb->device.get());
			mListStaging = BufferBuilder()
				.Size((size_t)EMISSIVE_VOLUMES_DRAWN_MAX * EMISSIVE_VOLUME_TEXELS * 4 * sizeof(float))
				.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
				.DebugName("EmissiveVolumes.ListStaging")
				.Create(fb->device.get());
		}
	}
	catch (const std::exception& e)
	{
		Printf(TEXTCOLOR_RED "EmissiveVolumes: %s\n", e.what());
	}
	if (!mList.Image || !mList.View || !mListStaging)
	{
		// Nothing has used them: gone now.
		mListStaging.reset();
		mList.View.reset();
		mList.Image.reset();
		mList.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		Refuse("the emissive volume list image");
		return false;
	}
	mList.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
	mListTexels.clear();
	return true;
}

bool VkEmissiveVolumes::EnsureNoise()
{
	if (mNoise.Image)
		return mNoise.Layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	const int n = EMISSIVE_NOISE_SIZE;
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	// RG8 where the device filters it linearly as a 3D image; RGBA8 otherwise (the march reads r and g only).
	const bool rg8 = mCompute->IsVolumeFormatSupported(VK_FORMAT_R8G8_UNORM, n, n, n, usage, true);
	const VkFormat format = rg8 ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
	std::vector<uint8_t> bytes;
	EmissiveVolumeCore::BakeNoise(bytes, rg8 ? 2 : 4);

	std::unique_ptr<VulkanBuffer> staging;
	try
	{
		mNoise.Image = ImageBuilder()
			.Size3D(n, n, n)
			.Format(format)
			.Usage(usage)
			.DebugName("EmissiveVolumes.Noise")
			.TryCreate(fb->device.get());
		if (mNoise.Image)
		{
			mNoise.View = ImageViewBuilder()
				.Type(VK_IMAGE_VIEW_TYPE_3D)
				.Image(mNoise.Image.get(), format)
				.DebugName("EmissiveVolumes.Noise")
				.Create(fb->device.get());
			staging = BufferBuilder()
				.Size(bytes.size())
				.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
				.DebugName("EmissiveVolumes.NoiseStaging")
				.Create(fb->device.get());
		}
	}
	catch (const std::exception& e)
	{
		Printf(TEXTCOLOR_RED "EmissiveVolumes: %s\n", e.what());
	}
	if (!mNoise.Image || !mNoise.View || !staging)
	{
		staging.reset();
		mNoise.View.reset();
		mNoise.Image.reset();
		mNoise.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		Refuse("the emissive volume noise");
		return false;
	}

	void* data = staging->Map(0, bytes.size());
	memcpy(data, bytes.data(), bytes.size());
	staging->Unmap();

	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = (uint32_t)n;
	region.imageExtent.height = (uint32_t)n;
	region.imageExtent.depth = (uint32_t)n;

	mCompute->BeginWork();	// outside any render pass, before the eye loop
	PipelineBarrier toTransfer;
	toTransfer.AddImage(mNoise.Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
	toTransfer.Execute(fb->GetCommands()->GetDrawCommands(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	fb->GetCommands()->GetDrawCommands()->copyBufferToImage(staging->buffer, mNoise.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	PipelineBarrier toRead;
	toRead.AddImage(mNoise.Image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	toRead.Execute(fb->GetCommands()->GetDrawCommands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	mNoise.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	// The staging buffer is named by this frame's commands: the frame's delete list frees it after them.
	fb->GetCommands()->DrawDeleteList->Add(std::move(staging));
	Printf("EmissiveVolumes: noise baked (%d^3 %s, %.2f MB)\n", n, rg8 ? "RG8" : "RGBA8", bytes.size() / (1024.0 * 1024.0));
	return true;
}
