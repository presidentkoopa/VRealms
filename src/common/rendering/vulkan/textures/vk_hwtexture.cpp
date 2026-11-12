/*
** vk_hwtexture.cpp
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


#include "c_cvars.h"
#include "hw_material.h"
#include "hw_cvars.h"
#include "hw_renderstate.h"
#include <zvulkan/vulkanobjects.h>
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vulkan/shaders/vk_shader.h"
#include "vk_hwtexture.h"
#include "filesystem.h"	// [DDS] VkCompressedPixels::Read
#include "image.h"
#include "hwrenderer/data/hw_perftrack.h"	// RS FORK -- perf_track: a texture upload is a hitch reason
#include <algorithm>

VkHardwareTexture::VkHardwareTexture(VulkanRenderDevice* fb, int numchannels) : fb(fb)
{
	mTexelsize = numchannels;
	fb->GetTextureManager()->AddTexture(this);
}

VkHardwareTexture::~VkHardwareTexture()
{
	SetHardwareState(NONE);

	if (fb)
		fb->GetTextureManager()->RemoveTexture(this);
}

void VkHardwareTexture::Reset()
{
	if (fb)
	{
		if (mappedSWFB)
		{
			mImage.Image->Unmap();
			mappedSWFB = nullptr;
		}

		mImage.Reset(fb);
		mLoadedImage.Reset(fb);
		mDepthStencil.Reset(fb);
		SetHardwareState(NONE);
	}
}

void VkHardwareTexture::SwapToLoadedImage()
{
	if (!mLoadedImage.Image)
	{
		return;
	}

	if (mappedSWFB)
	{
		mImage.Image->Unmap();
		mappedSWFB = nullptr;
	}

	mImage.Reset(fb);
	mDepthStencil.Reset(fb);
	std::swap(mImage, mLoadedImage);
}

VkTextureImage *VkHardwareTexture::GetImage(FTexture *tex, int translation, int flags)
{
	if (!mImage.Image)
	{
		if (mLoadedImage.Image && GetState() == READY)
		{
			SwapToLoadedImage();
		}
		else
		{
			CreateImage(tex, translation, flags);
		}
	}
	return &mImage;
}

VkTextureImage *VkHardwareTexture::GetDepthStencil(FTexture *tex)
{
	if (!mDepthStencil.View)
	{
		VkFormat format = fb->GetBuffers()->SceneDepthStencilFormat;
		int w = tex->GetWidth();
		int h = tex->GetHeight();

		mDepthStencil.Image = ImageBuilder()
			.Size(w, h)
			.Samples(VK_SAMPLE_COUNT_1_BIT)
			.Format(format)
			.Usage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
			.DebugName("VkHardwareTexture.DepthStencil")
			.Create(fb->device.get());

		mDepthStencil.AspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

		mDepthStencil.View = ImageViewBuilder()
			.Image(mDepthStencil.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
			.DebugName("VkHardwareTexture.DepthStencilView")
			.Create(fb->device.get());

		VkImageTransition()
			.AddImage(&mDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());
	}
	return &mDepthStencil;
}

void VkHardwareTexture::CreateImage(FTexture *tex, int translation, int flags)
{
	if (!tex->isHardwareCanvas())
	{
		// [DDS] A block-compressed image goes up as it is stored when this request allows it.
		if (!CreateCompressedImage(tex, translation, flags))
		{
			FTextureBuffer texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
			bool indexed = flags & CTF_Indexed;
			CreateTexture(texbuffer.mWidth, texbuffer.mHeight,indexed? 1 : 4, indexed? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM, texbuffer.mBuffer, !indexed);
		}
	}
	else
	{
		VkFormat format = tex->IsHDR() ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
		int w = tex->GetWidth();
		int h = tex->GetHeight();

		mImage.Image = ImageBuilder()
			.Format(format)
			.Size(w, h)
			.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
			.DebugName("VkHardwareTexture.mImage")
			.Create(fb->device.get());

		mImage.View = ImageViewBuilder()
			.Image(mImage.Image.get(), format)
			.DebugName("VkHardwareTexture.mImageView")
			.Create(fb->device.get());

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());
	}

	SetHardwareState(READY);
}

void VkHardwareTexture::CreateTexture(int w, int h, int pixelsize, VkFormat format, const void *pixels, bool mipmap)
{
	CreateTexture(fb->GetCommands(), &mImage, w, h, pixelsize, format, pixels, mipmap ? -1 : 0);
	SetHardwareState(READY);
}

void VkHardwareTexture::CheckFinalTransition(VulkanCommandBuffer *cmd, bool background)
{
	VkTextureImage* img = background ? &mLoadedImage : &mImage;
	if (img->Image && img->Layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		VkImageTransition()
			.AddImage(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, img->Layout == VK_IMAGE_LAYOUT_UNDEFINED, 0, img->Image->mipLevels)
			.Execute(cmd);
	}
}

void VkHardwareTexture::ReleaseLoadedFromQueue(VulkanCommandBuffer* cmd, int fromQueueFamily, int toQueueFamily)
{
	if (!mLoadedImage.Image)
	{
		return;
	}

	PipelineBarrier()
		.AddQueueTransfer(fromQueueFamily, toQueueFamily, mLoadedImage.Image.get(), mLoadedImage.Layout, VK_IMAGE_ASPECT_COLOR_BIT, 0, mLoadedImage.Image->mipLevels)
		.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void VkHardwareTexture::AcquireLoadedFromQueue(VulkanCommandBuffer* cmd, int fromQueueFamily, int toQueueFamily)
{
	if (!mLoadedImage.Image)
	{
		return;
	}

	PipelineBarrier()
		.AddQueueTransfer(fromQueueFamily, toQueueFamily, mLoadedImage.Image.get(), mLoadedImage.Layout, VK_IMAGE_ASPECT_COLOR_BIT, 0, mLoadedImage.Image->mipLevels)
		.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void VkHardwareTexture::BackgroundCreateTexture(VkCommandBufferManager* bufManager, int w, int h, int pixelsize, VkFormat format, const void *pixels, int numMipLevels, bool createMips, int totalSize)
{
	CreateTexture(bufManager, &mLoadedImage, w, h, pixelsize, format, pixels, numMipLevels, createMips, totalSize);
}

void VkHardwareTexture::CreateTexture(VkCommandBufferManager *bufManager, VkTextureImage *img, int w, int h, int pixelsize, VkFormat format, const void *pixels, int mipmap, bool generateMipmaps, int totalSize)
{
	// RS FORK -- perf_track: every texture and material upload, on the main thread and on the texture thread
	// alike (NoteReason is atomic), so a frame held by an upload can say so (REASON_TEXTURE). Off, no clock.
	PerfTrack::Scope uploadScope(PerfTrack::REASON_TEXTURE);
	if (w <= 0 || h <= 0)
		throw CVulkanError("Trying to create zero size texture");

	if (totalSize < 0) totalSize = w * h * pixelsize;
	if (mipmap == -1) mipmap = GetMipLevels(w, h);
	int mipLevels = mipmap <= 0 ? 1 : mipmap;

	auto stagingBuffer = BufferBuilder()
		.Size(totalSize)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("VkHardwareTexture.mStagingBuffer")
		.Create(fb->device.get());

	uint8_t *data = (uint8_t*)stagingBuffer->Map(0, totalSize);
	memcpy(data, pixels, totalSize);
	stagingBuffer->Unmap();

	img->Image = ImageBuilder()
		.Format(format)
		.Size(w, h, mipLevels)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.DebugName("VkHardwareTexture.mImage")
		.Create(fb->device.get());

	img->View = ImageViewBuilder()
		.Image(img->Image.get(), format)
		.DebugName("VkHardwareTexture.mImageView")
		.Create(fb->device.get());

	auto cmdbuffer = bufManager->GetTransferCommands();

	// [DDS] Every level the copies write must be in TRANSFER_DST. Levels made by GenerateMipmaps are
	// moved by it, so then only level 0, as always.
	VkImageTransition()
		.AddImage(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true, 0, generateMipmaps ? 1 : mipLevels)
		.Execute(cmdbuffer);

	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.depth = 1;
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	cmdbuffer->copyBufferToImage(stagingBuffer->buffer, img->Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	if (generateMipmaps && mipmap > 0) img->GenerateMipmaps(cmdbuffer);

	// If we queued more than 64 MB of data already: wait until the uploads finish before continuing
	bufManager->TransferDeleteList->Add(std::move(stagingBuffer));
	if (bufManager->TransferDeleteList->TotalSize > 64 * 1024 * 1024)
		bufManager->WaitForCommands(false, true);
}

int VkHardwareTexture::GetMipLevels(int w, int h)
{
	int levels = 1;
	while (w > 1 || h > 1)
	{
		w = max(w >> 1, 1);
		h = max(h >> 1, 1);
		levels++;
	}
	return levels;
}

/////////////////////////////////////////////////////////////////////////////
//
// [DDS] Compressed DDS textures, ported from GZSelaco 19a79ed90 (GPL v3):
// 490044c411 (the stored mip levels), 1c5f0b120d (BC1 and BC3 beside BC7),
// a46c31630a and 4e9bc832e1 (gl_texture_quality's starting level),
// 5c93e38c6c (the final layout). One upload routine serves the synchronous
// path (CreateImage) and both halves of the background loader.
//
/////////////////////////////////////////////////////////////////////////////

bool VkHardwareTexture::DeviceSupportsCompressed(VulkanDevice *device, VkFormat format)
{
	if (device == nullptr || format < VK_FORMAT_BC1_RGB_UNORM_BLOCK || format > VK_FORMAT_BC7_SRGB_BLOCK || !device->EnabledFeatures.Features.textureCompressionBC)
	{
		return false;
	}
	VkFormatProperties properties = {};
	vkGetPhysicalDeviceFormatProperties(device->PhysicalDevice.Device, format, &properties);
	const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	return (properties.optimalTilingFeatures & needed) == needed;
}

bool VkCompressedPixels::Read(FImageSource* src)
{
	free(data);
	data = nullptr;
	size = unitSize = 0;
	if (src == nullptr || !src->IsGPUOnly() || src->LumpNum() < 0)
	{
		return false;
	}

	FileReader reader;
	try
	{
		reader = fileSystem.OpenFileReader(src->LumpNum(), FileSys::EReaderType::READER_NEW, 0);
	}
	catch (...)
	{
		return false;
	}
	if (!reader.isOpen())
	{
		return false;
	}

	src->ReadCompressedPixels(&reader, &data, size, unitSize, storedMips);
	reader.Close();

	width = src->GetWidth();
	height = src->GetHeight();
	format = (VkFormat)src->getVKFormat();
	blockSize = (format == VK_FORMAT_BC1_RGB_UNORM_BLOCK || format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) ? 8 : 16;
	return data != nullptr && unitSize > 0 && size >= unitSize;
}

// BEGIN S2 HARNESS: plan
// Which stored levels go up, where each one starts in the stored data and how many bytes it is.
// A level is ceil(w/4) x ceil(h/4) blocks of blockSize bytes -- Vulkan's rule for block-compressed
// images, whose copy extent may stop short of a whole block only at the level's edge -- and each
// level is the one above halved, never below 1; the chain ends at 1x1 whatever the header claims.
// quality (gl_texture_quality) is the stored level that becomes level 0, never past the last one
// present. A partial chain uploads the levels it has (GZSelaco took the top level alone).
struct VkCompressedUploadPlan
{
	enum { MaxLevels = 17 };
	int startMip = 0;
	int levels = 0;		// 0 = nothing usable
	int width[MaxLevels] = {};
	int height[MaxLevels] = {};
	size_t offset[MaxLevels] = {};
	size_t bytes[MaxLevels] = {};
};

static VkCompressedUploadPlan PlanCompressedUpload(int width, int height, int blockSize, int storedMips, size_t totalSize, int quality)
{
	VkCompressedUploadPlan plan;
	if (width <= 0 || height <= 0 || (blockSize != 8 && blockSize != 16))
	{
		return plan;
	}

	int levelWidth[VkCompressedUploadPlan::MaxLevels], levelHeight[VkCompressedUploadPlan::MaxLevels];
	size_t levelOffset[VkCompressedUploadPlan::MaxLevels], levelBytes[VkCompressedUploadPlan::MaxLevels];
	const int stored = std::max(storedMips, 1);
	int w = width, h = height, present = 0;
	size_t pos = 0;
	while (present < stored && present < VkCompressedUploadPlan::MaxLevels)
	{
		const size_t bytes = (size_t)std::max((w + 3) / 4, 1) * (size_t)std::max((h + 3) / 4, 1) * (size_t)blockSize;
		if (pos > totalSize || totalSize - pos < bytes)
		{
			break;
		}
		levelWidth[present] = w;
		levelHeight[present] = h;
		levelOffset[present] = pos;
		levelBytes[present] = bytes;
		pos += bytes;
		present++;
		if (w == 1 && h == 1)
		{
			break;
		}
		w = std::max(w >> 1, 1);
		h = std::max(h >> 1, 1);
	}
	if (present == 0)
	{
		return plan;
	}

	plan.startMip = std::min(std::max(quality, 0), present - 1);
	plan.levels = present - plan.startMip;
	for (int i = 0; i < plan.levels; i++)
	{
		plan.width[i] = levelWidth[plan.startMip + i];
		plan.height[i] = levelHeight[plan.startMip + i];
		plan.offset[i] = levelOffset[plan.startMip + i];
		plan.bytes[i] = levelBytes[plan.startMip + i];
	}
	return plan;
}
// END S2 HARNESS: plan

bool VkHardwareTexture::CanUploadCompressed(FTexture *tex, int translation, int flags)
{
	if (fb == nullptr || tex == nullptr || tex->isHardwareCanvas())
	{
		return false;
	}
	FImageSource *src = tex->GetImage();
	if (src == nullptr || !src->IsGPUOnly() || (flags & (CTF_Indexed | CTF_Expand)) || IsLuminosityTranslation(translation))
	{
		return false;
	}
	if (translation > 0)
	{
		auto remap = GPalette.TranslationToTable(translation);
		if (remap != nullptr && !remap->Inactive)
		{
			return false;
		}
	}
	return DeviceSupportsCompressed(fb->device.get(), (VkFormat)src->getVKFormat());
}

bool VkHardwareTexture::CreateCompressedImage(FTexture *tex, int translation, int flags)
{
	if (!CanUploadCompressed(tex, translation, flags))
	{
		return false;
	}
	VkCompressedPixels pixels;
	if (!pixels.Read(tex->GetImage()) || CreateCompressedTexture(fb->GetCommands(), &mImage, pixels, !!(flags & CTF_ReduceQuality)) <= 0)
	{
		return false;
	}
	CheckFinalTransition(fb->GetCommands()->GetTransferCommands(), false);
	return true;
}

int VkHardwareTexture::CreateCompressedTexture(VkCommandBufferManager *bufManager, VkTextureImage *img, const VkCompressedPixels &pixels, bool allowQualityReduction)
{
	if (pixels.data == nullptr)
	{
		return 0;
	}
	const VkCompressedUploadPlan plan = PlanCompressedUpload(pixels.width, pixels.height, pixels.blockSize, pixels.storedMips, pixels.size, allowQualityReduction ? (int)gl_texture_quality : 0);
	if (plan.levels <= 0)
	{
		return 0;
	}

	// GZSelaco's order: the first level creates the image with room for all of them (pixelsize 0: the
	// size is the level's bytes; no generated mips), then each further level is copied in.
	CreateTexture(bufManager, img, plan.width[0], plan.height[0], 0, pixels.format, pixels.data + plan.offset[0], plan.levels, false, (int)plan.bytes[0]);
	for (int i = 1; i < plan.levels; i++)
	{
		CreateTextureMipMap(bufManager, img, i, plan.width[i], plan.height[i], 0, pixels.format, pixels.data + plan.offset[i], (int)plan.bytes[i]);
	}
	return plan.levels;
}

int VkHardwareTexture::BackgroundCreateCompressedTexture(VkCommandBufferManager* bufManager, const VkCompressedPixels& pixels, bool allowQualityReduction)
{
	return CreateCompressedTexture(bufManager, &mLoadedImage, pixels, allowQualityReduction);
}

void VkHardwareTexture::CreateTextureMipMap(VkCommandBufferManager* bufManager, VkTextureImage *img, int mipLevel, int w, int h, int pixelsize, VkFormat format, const void* pixels, int totalSize)
{
	if (w <= 0 || h <= 0)
		throw CVulkanError("Trying to create zero size mipmap!");

	auto stagingBuffer = BufferBuilder()
		.Size(totalSize)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("VkHardwareTexture.mStagingBuffer")
		.Create(fb->device.get());

	uint8_t* data = (uint8_t*)stagingBuffer->Map(0, totalSize);
	memcpy(data, pixels, totalSize);
	stagingBuffer->Unmap();

	auto cmdBuffer = bufManager->GetTransferCommands();

	// Assumes the image is still in the transfer layout
	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.mipLevel = mipLevel;
	region.imageExtent.depth = 1;
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	cmdBuffer->copyBufferToImage(stagingBuffer->buffer, img->Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// If we queued more than 64 MB of data already: wait until the uploads finish before continuing
	bufManager->TransferDeleteList->Add(std::move(stagingBuffer));
	if (bufManager->TransferDeleteList->TotalSize > 64 * 1024 * 1024)
		bufManager->WaitForCommands(false, true);
}

void VkHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	if (mImage.Image && (mImage.Image->width != w || mImage.Image->height != h || mTexelsize != texelsize))
	{
		Reset();
	}

	if (!mImage.Image)
	{
		VkFormat format = texelsize == 4 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8_UNORM;

		VkDeviceSize allocatedBytes = 0;
		mImage.Image = ImageBuilder()
			.Format(format)
			.Size(w, h)
			.LinearTiling()
			.Usage(VK_IMAGE_USAGE_SAMPLED_BIT, VMA_MEMORY_USAGE_UNKNOWN, VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
			.MemoryType(
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			.DebugName("VkHardwareTexture.mImage")
			.Create(fb->device.get(), &allocatedBytes);

		mTexelsize = texelsize;

		mImage.View = ImageViewBuilder()
			.Image(mImage.Image.get(), format)
			.DebugName("VkHardwareTexture.mImageView")
			.Create(fb->device.get());

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_GENERAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());

		bufferpitch = int(allocatedBytes / h / texelsize);
	}
}

uint8_t *VkHardwareTexture::MapBuffer()
{
	if (!mappedSWFB)
		mappedSWFB = (uint8_t*)mImage.Image->Map(0, mImage.Image->width * mImage.Image->height * mTexelsize);
	return mappedSWFB;
}

unsigned int VkHardwareTexture::CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name)
{
	// CreateTexture is used by the software renderer to create a screen output but without any screen data.
	if (buffer)
		CreateTexture(w, h, mTexelsize, mTexelsize == 4 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8_UNORM, buffer, mipmap);
	return 0;
}

void VkHardwareTexture::CreateWipeTexture(int w, int h, const char *name)
{
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;

	mImage.Image = ImageBuilder()
		.Format(format)
		.Size(w, h)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY)
		.DebugName(name)
		.Create(fb->device.get());

	mTexelsize = 4;

	mImage.View = ImageViewBuilder()
		.Image(mImage.Image.get(), format)
		.DebugName(name)
		.Create(fb->device.get());

	if (fb->GetBuffers()->GetWidth() > 0 && fb->GetBuffers()->GetHeight() > 0)
	{
		fb->GetPostprocess()->BlitCurrentToImage(&mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	else
	{
		// hwrenderer asked image data from a frame buffer that was never written into. Let's give it that..
		// (ideally the hwrenderer wouldn't do this, but the calling code is too complex for me to fix)

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
			.Execute(fb->GetCommands()->GetTransferCommands());

		VkImageSubresourceRange range = {};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.layerCount = 1;
		range.levelCount = 1;

		VkClearColorValue value = {};
		value.float32[0] = 0.0f;
		value.float32[1] = 0.0f;
		value.float32[2] = 0.0f;
		value.float32[3] = 1.0f;
		fb->GetCommands()->GetTransferCommands()->clearColorImage(mImage.Image->image, mImage.Layout, &value, 1, &range);

		VkImageTransition()
			.AddImage(&mImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(fb->GetCommands()->GetTransferCommands());
	}
}

/////////////////////////////////////////////////////////////////////////////

VkMaterial::VkMaterial(VulkanRenderDevice* fb, FGameTexture* tex, int scaleflags) : FMaterial(tex, scaleflags), fb(fb)
{
	fb->GetDescriptorSetManager()->AddMaterial(this);
}

VkMaterial::~VkMaterial()
{
	if (fb)
		fb->GetDescriptorSetManager()->RemoveMaterial(this);
}

void VkMaterial::DeleteDescriptors()
{
	if (fb)
	{
		auto deleteList = fb->GetCommands()->DrawDeleteList.get();
		for (auto& it : mDescriptorSets)
		{
			deleteList->Add(std::move(it.descriptor));
		}
		mDescriptorSets.clear();
	}
}

VulkanDescriptorSet* VkMaterial::GetDescriptorSet(const FMaterialState& state)
{
	auto base = Source();
	int clampmode = state.mClampMode;
	int translation = state.mTranslation;
	auto translationp = IsLuminosityTranslation(translation)? translation : intptr_t(GPalette.GetTranslation(GetTranslationType(translation), GetTranslationIndex(translation)));

	clampmode = base->GetClampMode(clampmode);

	for (auto& set : mDescriptorSets)
	{
		if (set.descriptor && set.clampmode == clampmode && set.remap == translationp) return set.descriptor.get();
	}

	int numLayers = NumLayers();

	auto descriptor = fb->GetDescriptorSetManager()->AllocateTextureDescriptorSet(max(numLayers, SHADER_MIN_REQUIRED_TEXTURE_LAYERS));

	descriptor->SetDebugName("VkHardwareTexture.mDescriptorSets");

	VulkanSampler* sampler = fb->GetSamplerManager()->Get(clampmode);

	WriteDescriptors update;
	MaterialLayerInfo *layer;
	auto systex = static_cast<VkHardwareTexture*>(GetLayer(0, state.mTranslation, &layer));
	auto systeximage = systex->GetImage(layer->layerTexture, state.mTranslation, layer->scaleFlags);
	update.AddCombinedImageSampler(descriptor.get(), 0, systeximage->View.get(), sampler, systeximage->Layout);

	if (!(layer->scaleFlags & CTF_Indexed))
	{
		for (int i = 1; i < numLayers; i++)
		{
			auto syslayer = static_cast<VkHardwareTexture*>(GetLayer(i, 0, &layer));
			auto syslayerimage = syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
			update.AddCombinedImageSampler(descriptor.get(), i, syslayerimage->View.get(), sampler, syslayerimage->Layout);
		}
	}
	else
	{
		for (int i = 1; i < 3; i++)
		{
			auto syslayer = static_cast<VkHardwareTexture*>(GetLayer(i, translation, &layer));
			auto syslayerimage = syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
			update.AddCombinedImageSampler(descriptor.get(), i, syslayerimage->View.get(), sampler, syslayerimage->Layout);
		}
		numLayers = 3;
	}

	auto dummyImage = fb->GetTextureManager()->GetNullTextureView();
	for (int i = numLayers; i < SHADER_MIN_REQUIRED_TEXTURE_LAYERS; i++)
	{
		update.AddCombinedImageSampler(descriptor.get(), i, dummyImage, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	update.Execute(fb->device.get());
	mDescriptorSets.emplace_back(clampmode, translationp, std::move(descriptor));
	return mDescriptorSets.back().descriptor.get();
}
