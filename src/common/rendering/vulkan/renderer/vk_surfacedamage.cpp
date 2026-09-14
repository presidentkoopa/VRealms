/*
** vk_surfacedamage.cpp
**
** [SURFACEDAMAGE] The surface damage atlas's GPU side. See the header.
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

#include "vk_surfacedamage.h"
#include "vk_compute.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "hw_surfacedamageframe.h"
#include "hw_perflog.h"
#include "printf.h"
#include "v_text.h"

namespace
{
	// Both compute lumps' push constant block, std430: an ivec4 and a vec4, offsets 0 and 16 in both languages (checked from
	// SPIR-V, "Engine docs/SURFACE_DAMAGE_17_IMPL_NOTES.md").
	struct DamageDispatchConstants
	{
		int32_t Range[4];	// x the first tile record, y how many
		float Cool[4];		// x keep, y subtract (damage_cool.comp)
	};
	static_assert(sizeof(DamageDispatchConstants) == 32, "DamageDispatchConstants must match the damage compute lumps' push constant block (32 bytes)");

	const VkFormat kPageFormat = VK_FORMAT_R8G8B8A8_UNORM;
	const int kLocalSize = 16;	// both lumps' local_size_x and _y

	// The tile buffer holds this frame's stamped tiles from 0 and its cooled tiles from here.
	const int kCoolTilesAt = SURFACE_DAMAGE_TILES_PER_FRAME;
	const size_t kStampBytes = (size_t)SURFACE_DAMAGE_STAMPS_PER_FRAME * sizeof(SurfaceDamageStampGpu);
	const size_t kTileBytes = (size_t)(SURFACE_DAMAGE_TILES_PER_FRAME * 2) * sizeof(SurfaceDamageTileGpu);
}

VkSurfaceDamage::VkSurfaceDamage(VkComputeManager* compute) : mCompute(compute), fb(compute->GetDevice())
{
}

VkSurfaceDamage::~VkSurfaceDamage()
{
	// Members only. At device teardown the GPU is idle (the render device waits for it before anything is destroyed). The CPU
	// side must not believe an atlas still exists.
	SurfaceDamageStatus() = SurfaceDamageBackendStatus();
}

//-----------------------------------------------------------------------------
//
// The frame
//
//-----------------------------------------------------------------------------

void VkSurfaceDamage::Run(const SurfaceDamageFrame& frame)
{
	if (frame.Serial == 0 || frame.Serial == mLastSerial)
		return;
	mLastSerial = frame.Serial;

	SurfaceDamageBackendStatus& status = SurfaceDamageStatus();

	if (!frame.Active)
	{
		Release("damage no longer asked for");
		mRefusedPages = 0;	// asked again later: try again
		status.Allocated = false;
		status.Pages = 0;
		status.RefusedPages = 0;
		return;
	}

	if (HasPages() && (mPageCount != frame.Pages || mHashEntries != frame.HashEntries))
		Release("the damage memory changed");

	if (!HasPages())
	{
		if (mProgramsFailed)
		{
			status.ProgramsFailed = true;
			return;
		}
		if (frame.Pages == mRefusedPages)
		{
			status.RefusedPages = mRefusedPages;
			return;
		}
		// The programs before any memory: a device that cannot build them gets no atlas.
		if (!EnsurePrograms())
		{
			status.ProgramsFailed = true;
			return;
		}
		if (!Allocate(frame.Pages, frame.HashEntries))
		{
			mRefusedPages = frame.Pages;
			status.Allocated = false;
			status.Pages = 0;
			status.RefusedPages = mRefusedPages;
			return;
		}
		// A new atlas takes this frame's whole picture: every byte of data, both images, and this frame's stamps (the CPU side
		// decided them without needing the atlas to exist).
		static uint64_t epochs = 0;
		status.Epoch = ++epochs;
		status.Allocated = true;
		status.Pages = mPageCount;
		status.RefusedPages = 0;
		mUploadAll = true;
		mBrushGeneration = 0;
		mDetailGeneration = 0;
	}

	bool setsStale = !mStampSet || !mMipSet || !mCoolSet;
	if (frame.BrushPixels != nullptr && frame.BrushLayers > 0 && frame.BrushGeneration != mBrushGeneration)
	{
		if (!UploadLayers(mBrushImage, mBrushView, frame.BrushPixels, frame.BrushLayers, SURFACE_DAMAGE_BRUSH_TEXELS, SURFACE_DAMAGE_BRUSH_MIPS, "SurfaceDamage.Brushes"))
			return;
		mBrushGeneration = frame.BrushGeneration;
		setsStale = true;
	}
	if (frame.DetailPixels != nullptr && frame.DetailLayers > 0 && frame.DetailGeneration != mDetailGeneration)
	{
		if (!UploadLayers(mDetailImage, mDetailView, frame.DetailPixels, frame.DetailLayers, SURFACE_DAMAGE_DETAIL_TEXELS, SURFACE_DAMAGE_DETAIL_MIPS, "SurfaceDamage.Detail"))
			return;
		mDetailGeneration = frame.DetailGeneration;
	}
	UploadData(frame);
	if (setsStale && !MakeSets())
		return;

	const int stampTiles = frame.StampTiles != nullptr ? std::clamp(frame.StampTileCount, 0, SURFACE_DAMAGE_TILES_PER_FRAME) : 0;
	const int stamps = frame.Stamps != nullptr ? std::clamp(frame.StampCount, 0, SURFACE_DAMAGE_STAMPS_PER_FRAME) : 0;
	const int coolTiles = frame.CoolTiles != nullptr ? std::clamp(frame.CoolTileCount, 0, SURFACE_DAMAGE_TILES_PER_FRAME) : 0;
	if (stampTiles == 0 && coolTiles == 0)
		return;

	// THE HOST-VISIBLE LISTS. Every frame's submissions finish before the next frame records (as the debris pool relies on), so
	// they are written again in place.
	if (stamps > 0)
	{
		uint8_t* data = (uint8_t*)mStamps->Map(0, (size_t)stamps * sizeof(SurfaceDamageStampGpu));
		if (data == nullptr)
			return;
		memcpy(data, frame.Stamps, (size_t)stamps * sizeof(SurfaceDamageStampGpu));
		mStamps->Unmap();
	}
	{
		uint8_t* data = (uint8_t*)mTiles->Map(0, kTileBytes);
		if (data == nullptr)
			return;
		if (stampTiles > 0)
			memcpy(data, frame.StampTiles, (size_t)stampTiles * sizeof(SurfaceDamageTileGpu));
		if (coolTiles > 0)
			memcpy(data + (size_t)kCoolTilesAt * sizeof(SurfaceDamageTileGpu), frame.CoolTiles, (size_t)coolTiles * sizeof(SurfaceDamageTileGpu));
		mTiles->Unmap();
	}

	mCompute->BeginWork();	// so fx.damagepaint nests inside fx.compute
	VkCommandBufferManager* commands = fb->GetCommands();
	commands->PushGroup("fx.damagepaint");
	if (stampTiles > 0)
		DispatchTiles(mStampProgram.get(), mStampSet.get(), 0, stampTiles, SURFACE_DAMAGE_SLOT, 1.0f, 0.0f);
	if (coolTiles > 0)
		DispatchTiles(mCoolProgram.get(), mCoolSet.get(), kCoolTilesAt, coolTiles, SURFACE_DAMAGE_SLOT, frame.CoolKeep, frame.CoolSub);
	if (stampTiles > 0)
		DispatchTiles(mMipProgram.get(), mMipSet.get(), 0, stampTiles, SURFACE_DAMAGE_SLOT / 2, 1.0f, 0.0f);
	if (coolTiles > 0)
		DispatchTiles(mMipProgram.get(), mMipSet.get(), kCoolTilesAt, coolTiles, SURFACE_DAMAGE_SLOT / 2, 1.0f, 0.0f);
	commands->PopGroup();
}

void VkSurfaceDamage::DispatchTiles(VkComputeProgram* program, VulkanDescriptorSet* set, int first, int count, int slotTexels, float keep, float sub)
{
	DamageDispatchConstants constants = {};
	constants.Range[0] = first;
	constants.Range[1] = count;
	constants.Cool[0] = keep;
	constants.Cool[1] = sub;
	const uint32_t groups = (uint32_t)((slotTexels + kLocalSize - 1) / kLocalSize);
	mCompute->Dispatch(program, set, &constants, groups, groups, (uint32_t)count);
}

//-----------------------------------------------------------------------------
//
// Images and buffers
//
//-----------------------------------------------------------------------------

bool VkSurfaceDamage::IsPageFormatSupported(int pages)
{
	ImageBuilder builder;
	builder.Size(SURFACE_DAMAGE_PAGE, SURFACE_DAMAGE_PAGE, SURFACE_DAMAGE_MIP_LEVELS, pages);
	builder.Format(kPageFormat);
	builder.Usage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (!builder.IsFormatSupported(fb->device.get()))
		return false;
	const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	VkFormatProperties properties = {};
	vkGetPhysicalDeviceFormatProperties(fb->device->PhysicalDevice.Device, kPageFormat, &properties);
	return (properties.optimalTilingFeatures & needed) == needed;
}

bool VkSurfaceDamage::EnsureSamplers()
{
	if (mPagesSampler && mDetailSampler && mBrushSampler)
		return true;
	try
	{
		VulkanDevice* device = fb->device.get();
		// Linear with a linear blend between the two levels, clamped; main.fp picks the level (0..1) itself.
		mPagesSampler = SamplerBuilder()
			.MagFilter(VK_FILTER_LINEAR)
			.MinFilter(VK_FILTER_LINEAR)
			.MipmapMode(VK_SAMPLER_MIPMAP_MODE_LINEAR)
			.AddressMode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
			.MaxLod((float)(SURFACE_DAMAGE_MIP_LEVELS - 1))
			.DebugName("SurfaceDamage.PagesSampler")
			.Create(device);
		// The detail tiles: repeating, every level.
		mDetailSampler = SamplerBuilder()
			.MagFilter(VK_FILTER_LINEAR)
			.MinFilter(VK_FILTER_LINEAR)
			.MipmapMode(VK_SAMPLER_MIPMAP_MODE_LINEAR)
			.AddressMode(VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
			.MaxLod((float)SURFACE_DAMAGE_DETAIL_MIPS)
			.DebugName("SurfaceDamage.DetailSampler")
			.Create(device);
		// A brush's mask: clamped (outside its square the stamp skips the texel anyway), every level.
		mBrushSampler = SamplerBuilder()
			.MagFilter(VK_FILTER_LINEAR)
			.MinFilter(VK_FILTER_LINEAR)
			.MipmapMode(VK_SAMPLER_MIPMAP_MODE_LINEAR)
			.AddressMode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
			.MaxLod((float)SURFACE_DAMAGE_BRUSH_MIPS)
			.DebugName("SurfaceDamage.BrushSampler")
			.Create(device);
	}
	catch (const std::exception& e)
	{
		Printf(TEXTCOLOR_RED "SurfaceDamage: %s\n", e.what());
		return false;
	}
	return mPagesSampler && mDetailSampler && mBrushSampler;
}

bool VkSurfaceDamage::Allocate(int pages, int hashEntries)
{
	if (pages < SURFACE_DAMAGE_PAGES_MIN || pages > SURFACE_DAMAGE_PAGES_MAX || hashEntries <= 0 || (hashEntries & (hashEntries - 1)) != 0)
	{
		Printf(TEXTCOLOR_RED "SurfaceDamage: %d pages with a hash of %d is outside what the atlas takes -- no wall damage is drawn\n", pages, hashEntries);
		return false;
	}
	if (!EnsureSamplers())
		return false;
	if (!IsPageFormatSupported(pages))
	{
		Printf(TEXTCOLOR_RED "SurfaceDamage: this device cannot make %d x %d x %d RGBA8 storage pages -- no wall damage is drawn at this size\n", SURFACE_DAMAGE_PAGE, SURFACE_DAMAGE_PAGE, pages);
		return false;
	}

	VulkanDevice* device = fb->device.get();
	const size_t dataBytes = (size_t)SurfaceDamageDataVec4s(hashEntries) * 16;
	const uint64_t pageBytes = SURFACE_DAMAGE_PAGE_BYTES * (uint64_t)pages;
	bool created = true;
	try
	{
		mPagesImage = ImageBuilder()
			.Size(SURFACE_DAMAGE_PAGE, SURFACE_DAMAGE_PAGE, SURFACE_DAMAGE_MIP_LEVELS, pages)
			.Format(kPageFormat)
			.Usage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			.DebugName("SurfaceDamage.Pages")
			.TryCreate(device);
		if (mPagesImage)
		{
			mPagesView = ImageViewBuilder()
				.Type(VK_IMAGE_VIEW_TYPE_2D_ARRAY)
				.Image(mPagesImage.get(), kPageFormat)
				.DebugName("SurfaceDamage.PagesView")
				.Create(device);
			mLevelZeroView = ImageViewBuilder()
				.Type(VK_IMAGE_VIEW_TYPE_2D_ARRAY)
				.Image(mPagesImage.get(), kPageFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0)
				.DebugName("SurfaceDamage.LevelZeroView")
				.Create(device);
			mLevelOneView = ImageViewBuilder()
				.Type(VK_IMAGE_VIEW_TYPE_2D_ARRAY)
				.Image(mPagesImage.get(), kPageFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1, 0)
				.DebugName("SurfaceDamage.LevelOneView")
				.Create(device);
		}
		mData = BufferBuilder()
			.Size(dataBytes)
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			.DebugName("SurfaceDamage.Data")
			.Create(device);
		mStamps = BufferBuilder()
			.Size(kStampBytes)
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			.DebugName("SurfaceDamage.Stamps")
			.Create(device);
		mTiles = BufferBuilder()
			.Size(kTileBytes)
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			.DebugName("SurfaceDamage.Tiles")
			.Create(device);
		created = mPagesImage && mPagesView && mLevelZeroView && mLevelOneView && mData && mStamps && mTiles;
	}
	catch (const std::exception& e)
	{
		Printf(TEXTCOLOR_RED "SurfaceDamage: %s\n", e.what());
		created = false;
	}
	if (!created)
	{
		DestroyNow();
		Printf(TEXTCOLOR_RED "SurfaceDamage: could not allocate %d pages (%llu bytes) -- out of video memory? No wall damage is drawn at this size\n",
			pages, (unsigned long long)pageBytes);
		return false;
	}
	mPageCount = pages;
	mHashEntries = hashEntries;

	// GENERAL for life (storage and sampled), both levels cleared: no tile holds anything yet.
	VulkanCommandBuffer* cmd = fb->GetCommands()->GetTransferCommands();
	PipelineBarrier()
		.AddImage(mPagesImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT, 0, SURFACE_DAMAGE_MIP_LEVELS, 0, pages)
		.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkClearColorValue zero = {};
	VkImageSubresourceRange range = {};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = (uint32_t)SURFACE_DAMAGE_MIP_LEVELS;
	range.baseArrayLayer = 0;
	range.layerCount = (uint32_t)pages;
	cmd->clearColorImage(mPagesImage->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);

	Printf("SurfaceDamage: allocated %d pages (%d tiles) -- %llu bytes of pages with their mip level, +%llu data (a hash of %d), +%llu stamp and tile lists\n",
		pages, pages * SURFACE_DAMAGE_TILES_PER_PAGE, (unsigned long long)pageBytes, (unsigned long long)dataBytes, hashEntries,
		(unsigned long long)(kStampBytes + kTileBytes));
	return true;
}

void VkSurfaceDamage::DestroyNow()
{
	// Only for objects no command has used yet (a half-finished Allocate).
	mStampSet.reset();
	mMipSet.reset();
	mCoolSet.reset();
	mLevelZeroView.reset();
	mLevelOneView.reset();
	mPagesView.reset();
	mPagesImage.reset();
	mData.reset();
	mStamps.reset();
	mTiles.reset();
}

void VkSurfaceDamage::Release(const char* why)
{
	if (!mPagesImage && !mData && !mBrushImage && !mDetailImage)
		return;

	// This frame's descriptor sets (written at the start of the frame) and its commands may still name these, so they go on the
	// frame's delete list. The next frame's sets bind the stand-ins.
	auto deleteList = fb->GetCommands()->DrawDeleteList.get();
	const uint64_t freed = mPagesImage ? SURFACE_DAMAGE_PAGE_BYTES * (uint64_t)mPageCount : 0;
	deleteList->Add(std::move(mStampSet));
	deleteList->Add(std::move(mMipSet));
	deleteList->Add(std::move(mCoolSet));
	deleteList->Add(std::move(mLevelZeroView));
	deleteList->Add(std::move(mLevelOneView));
	deleteList->Add(std::move(mPagesView));
	deleteList->Add(std::move(mPagesImage));
	deleteList->Add(std::move(mBrushView));
	deleteList->Add(std::move(mBrushImage));
	deleteList->Add(std::move(mDetailView));
	deleteList->Add(std::move(mDetailImage));
	deleteList->Add(std::move(mData));
	deleteList->Add(std::move(mStamps));
	deleteList->Add(std::move(mTiles));

	Printf("SurfaceDamage: released (%s) -- %llu bytes of pages freed\n", why, (unsigned long long)freed);
	mPageCount = 0;
	mHashEntries = 0;
	mBrushGeneration = 0;
	mDetailGeneration = 0;
	SurfaceDamageStatus().Allocated = false;
}

// A layer set's whole mip chains (level 0 of every layer, then level 1 of every layer, ...) into a new sampled 2D array, as
// the particle atlas is made (vk_texture.cpp). The old image, if any, goes on the delete list.
bool VkSurfaceDamage::UploadLayers(std::unique_ptr<VulkanImage>& image, std::unique_ptr<VulkanImageView>& view, const uint8_t* pixels, int layers, int side, int levels, const char* name)
{
	const size_t chainBytes = SurfaceDamageChainBytes(side, levels);
	const size_t total = chainBytes * (size_t)layers;
	std::unique_ptr<VulkanBuffer> staging;
	std::unique_ptr<VulkanImage> newImage;
	std::unique_ptr<VulkanImageView> newView;
	try
	{
		staging = BufferBuilder()
			.Size(total)
			.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
			.DebugName(name)
			.Create(fb->device.get());
		newImage = ImageBuilder()
			.Format(kPageFormat)
			.Size(side, side, levels, layers)
			.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			.DebugName(name)
			.Create(fb->device.get());
		newView = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D_ARRAY)
			.Image(newImage.get(), kPageFormat)
			.DebugName(name)
			.Create(fb->device.get());
	}
	catch (const std::exception& e)
	{
		Printf(TEXTCOLOR_RED "SurfaceDamage: %s: %s\n", name, e.what());
		return false;
	}
	uint8_t* data = (uint8_t*)staging->Map(0, total);
	if (data == nullptr)
		return false;
	memcpy(data, pixels, total);
	staging->Unmap();

	VulkanCommandBuffer* cmd = fb->GetCommands()->GetTransferCommands();
	PipelineBarrier()
		.AddImage(newImage.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers)
		.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	std::vector<VkBufferImageCopy> regions((size_t)levels);
	size_t offset = 0;
	for (int m = 0; m < levels; m++)
	{
		VkBufferImageCopy& region = regions[(size_t)m];
		region = {};
		region.bufferOffset = offset;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = (uint32_t)m;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = (uint32_t)layers;
		region.imageExtent.width = (uint32_t)(side >> m);
		region.imageExtent.height = (uint32_t)(side >> m);
		region.imageExtent.depth = 1;
		offset += (size_t)(side >> m) * (size_t)(side >> m) * 4 * (size_t)layers;
	}
	cmd->copyBufferToImage(staging->buffer, newImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)levels, regions.data());
	PipelineBarrier()
		.AddImage(newImage.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers)
		.Execute(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	fb->GetCommands()->TransferDeleteList->Add(std::move(staging));

	auto deleteList = fb->GetCommands()->DrawDeleteList.get();
	if (view) deleteList->Add(std::move(view));
	if (image) deleteList->Add(std::move(image));
	image = std::move(newImage);
	view = std::move(newView);
	return true;
}

void VkSurfaceDamage::UploadData(const SurfaceDamageFrame& frame)
{
	if (frame.Data == nullptr || frame.DataVec4s <= 0 || !mData)
		return;
	const int vec4s = std::min(frame.DataVec4s, SurfaceDamageDataVec4s(mHashEntries));
	int first = frame.DirtyFirst;
	int end = frame.DirtyEnd;
	if (mUploadAll || frame.DataGeneration != mDataGeneration)
	{
		first = 0;
		end = vec4s;
		mUploadAll = false;
		mDataGeneration = frame.DataGeneration;
	}
	first = std::clamp(first, 0, vec4s);
	end = std::clamp(end, first, vec4s);
	if (end <= first)
		return;
	const size_t offset = (size_t)first * 16;
	const size_t bytes = (size_t)(end - first) * 16;
	uint8_t* data = (uint8_t*)mData->Map(offset, bytes);
	if (data == nullptr)
		return;
	memcpy(data, frame.Data + (size_t)first * 4, bytes);
	mData->Unmap();
}

//-----------------------------------------------------------------------------
//
// The programs and their sets
//
//-----------------------------------------------------------------------------

bool VkSurfaceDamage::EnsurePrograms()
{
	if (mStampProgram && mMipProgram && mCoolProgram)
		return true;
	if (mProgramsFailed)
		return false;

	mStampProgram = mCompute->CreateProgram("shaders/compute/damage_stamp.comp",
		{
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE },			// the pages, level 0
			{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },	// the brushes
			{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },			// the stamps
			{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },			// the tiles
		},
		(uint32_t)sizeof(DamageDispatchConstants));
	mMipProgram = mCompute->CreateProgram("shaders/compute/damage_stamp.comp",
		{
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE },			// the pages, level 1 (written)
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE },			// the pages, level 0 (read)
			{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },			// the tiles
		},
		(uint32_t)sizeof(DamageDispatchConstants), "#define DAMAGE_MIP\n");
	mCoolProgram = mCompute->CreateProgram("shaders/compute/damage_cool.comp",
		{
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE },			// the pages, level 0
			{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },			// the tiles
		},
		(uint32_t)sizeof(DamageDispatchConstants));
	if (!mStampProgram || !mMipProgram || !mCoolProgram)
	{
		// CreateProgram logged why. Not retried this session.
		Printf(TEXTCOLOR_RED "SurfaceDamage: the damage compute programs did not build -- no wall damage is drawn this session\n");
		mStampProgram.reset();
		mMipProgram.reset();
		mCoolProgram.reset();
		mProgramsFailed = true;
		return false;
	}
	return true;
}

// The three sets name the pages' views, the brush image and the lists; made again when the brush image is replaced. The old
// sets go on the delete list.
bool VkSurfaceDamage::MakeSets()
{
	if (!HasPages() || !mData || !mStamps || !mTiles || !mBrushView || !mBrushSampler)
		return false;
	std::unique_ptr<VulkanDescriptorSet> stamp = mCompute->AllocateSet(mStampProgram.get());
	std::unique_ptr<VulkanDescriptorSet> mip = mCompute->AllocateSet(mMipProgram.get());
	std::unique_ptr<VulkanDescriptorSet> cool = mCompute->AllocateSet(mCoolProgram.get());
	if (!stamp || !mip || !cool)
		return false;
	WriteDescriptors()
		.AddStorageImage(stamp.get(), 0, mLevelZeroView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddCombinedImageSampler(stamp.get(), 1, mBrushView.get(), mBrushSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.AddBuffer(stamp.get(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mStamps.get())
		.AddBuffer(stamp.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mTiles.get())
		.AddStorageImage(mip.get(), 0, mLevelOneView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(mip.get(), 1, mLevelZeroView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddBuffer(mip.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mTiles.get())
		.AddStorageImage(cool.get(), 0, mLevelZeroView.get(), VK_IMAGE_LAYOUT_GENERAL)
		.AddBuffer(cool.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mTiles.get())
		.Execute(fb->device.get());

	auto deleteList = fb->GetCommands()->DrawDeleteList.get();
	if (mStampSet) deleteList->Add(std::move(mStampSet));
	if (mMipSet) deleteList->Add(std::move(mMipSet));
	if (mCoolSet) deleteList->Add(std::move(mCoolSet));
	mStampSet = std::move(stamp);
	mMipSet = std::move(mip);
	mCoolSet = std::move(cool);
	return true;
}
