/*
** vk_levelfield.cpp
**
** [LEVELFIELD] The level collision field's GPU side. See the header.
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

#include "vk_levelfield.h"
#include "vk_compute.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "hw_framecompute.h"
#include "printf.h"
#include "v_text.h"

namespace
{
	const VkImageUsageFlags FieldUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	const VkImageUsageFlags HeaderUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	const size_t HEADER_BYTES = (size_t)LEVEL_FIELD_HEADER_TEXELS * 4 * sizeof(float);
	const size_t STAGING_BYTES = (size_t)LEVEL_FIELD_TILES_PER_FRAME * (size_t)LEVEL_FIELD_TILE_TEXELS * (size_t)LEVEL_FIELD_BYTES_PER_CELL + HEADER_BYTES;
	const size_t LINE_BYTES = (size_t)LEVEL_FIELD_LINES_PER_FRAME * sizeof(LevelFieldLine);

	// shaders/compute/field_bake.comp, std430: every member an ivec4 / vec4, so the offsets are 0, 16,
	// 32, 48 in both languages (checked from SPIR-V, "Engine docs/COLLISION_8_IMPL_NOTES.md").
	struct FieldBakeConstants
	{
		int32_t TexelMin[4];	// xyz first texel; w the mode, 0 bake, 1 invalidate
		int32_t TexelCount[4];	// xyz texels; w the first line record
		int32_t WorldCell[4];	// xyz the world cell TexelMin holds; w how many line records
		float Cell[4];			// x cell size, y band (map units)
	};
	static_assert(sizeof(FieldBakeConstants) == 64, "FieldBakeConstants must match field_bake.comp's push constant block (64 bytes)");
	static_assert(sizeof(LevelFieldLine) == 48, "LevelFieldLine must match FieldLine in field_bake.comp (48 bytes)");

	uint32_t Groups(int count)
	{
		return (uint32_t)std::max((count + 7) / 8, 1);
	}

	// IEEE 754 single to half precision, rounded to nearest. The field's values are within
	// +-96 and at least 1/64 in magnitude, well inside the half float's normal range; the rest is
	// handled for completeness (mirrored in "Engine docs/COLLISION_8_IMPL_NOTES.md", mirror8).
	uint16_t HalfFromFloat(float value)
	{
		uint32_t bits = 0;
		memcpy(&bits, &value, sizeof(bits));
		const uint16_t sign = (uint16_t)((bits >> 16) & 0x8000u);
		const uint32_t rawExponent = (bits >> 23) & 0xffu;
		uint32_t mantissa = bits & 0x007fffffu;

		if (rawExponent == 0xffu)
			return (uint16_t)(sign | 0x7c00u | (mantissa != 0 ? 0x0200u : 0u));	// inf, nan
		const int exponent = (int)rawExponent - 127 + 15;
		if (exponent >= 31)
			return (uint16_t)(sign | 0x7c00u);	// too large: inf
		if (exponent <= 0)
		{
			if (exponent < -10)
				return sign;	// rounds to zero
			mantissa |= 0x00800000u;
			const int shift = 14 - exponent;
			uint32_t half = mantissa >> shift;
			if ((mantissa >> (shift - 1)) & 1u)
				half++;
			return (uint16_t)(sign | half);
		}
		uint32_t half = ((uint32_t)exponent << 10) | (mantissa >> 13);
		if (mantissa & 0x00001000u)
			half++;	// a carry into the exponent is the right answer
		return (uint16_t)(sign | half);
	}
}

VkLevelField::VkLevelField(VkComputeManager* compute) : mCompute(compute), fb(compute->GetDevice())
{
}

VkLevelField::~VkLevelField()
{
	// Members only. At device teardown the GPU is idle (the render device waits for it before
	// anything is destroyed). The CPU side must not believe a field still exists.
	LevelFieldStatus() = LevelFieldBackendStatus();
}

VulkanImageView* VkLevelField::GetFieldView(int level) const
{
	if (level < 0 || level >= LEVEL_FIELD_LEVELS)
		return nullptr;
	return mLevels[level].View.get();
}

//-----------------------------------------------------------------------------
//
// The frame
//
//-----------------------------------------------------------------------------

void VkLevelField::Run(const LevelFieldFrame& frame)
{
	LevelFieldBackendStatus& status = LevelFieldStatus();

	if (!frame.Active)
	{
		Release("particle collision no longer asked for");
		mRefusedQuality = 0;	// asked again later: try again
		status = LevelFieldBackendStatus();
		return;
	}

	if (IsAllocated() && mQuality != frame.Quality)
		Release("quality changed");

	if (!IsAllocated())
	{
		const auto refuse = [&]()
		{
			mRefusedQuality = frame.Quality;
			status.Allocated = false;
			status.Quality = 0;
			status.RefusedQuality = mRefusedQuality;
		};
		if (frame.Quality == mRefusedQuality)
		{
			refuse();
			return;
		}
		// The program before any memory: a device that cannot build it gets no field.
		if (!EnsureProgram() || !Allocate(frame))
		{
			refuse();
			return;
		}
		if (!EnsureSets())
		{
			Release("no descriptor sets");
			refuse();
			return;
		}
		static uint64_t epochs = 0;
		status.Epoch = ++epochs;
		// Freshly allocated: every texel unbaked and the header "no level". What the CPU side
		// sent this frame was decided before it knew of this allocation; it starts again next frame.
		status.Allocated = true;
		status.Quality = mQuality;
		status.RefusedQuality = 0;
		return;
	}
	status.Allocated = true;
	status.Quality = mQuality;
	status.RefusedQuality = 0;

	VkCommandBufferManager* commands = fb->GetCommands();
	bool grouped = false;
	const auto begin = [&]()
	{
		if (grouped)
			return;
		grouped = true;
		mCompute->BeginWork();	// so fx.levelfield nests inside fx.compute
		commands->PushGroup("fx.levelfield");
	};

	const uint64_t epochBefore = status.Epoch;

	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
	{
		if (frame.ClearLevel[level])
		{
			begin();
			ClearLevel(level);
		}
	}

	for (int i = 0; i < frame.InvalidateCount && frame.Invalidate != nullptr; i++)
	{
		begin();
		if (!Invalidate(frame.Invalidate[i]))
		{
			Fail("an invalidation box outside the volume");
			break;
		}
	}

	const bool headerChanged = memcmp(frame.Header, mWrittenHeader, sizeof(mWrittenHeader)) != 0;
	if (status.Epoch == epochBefore && (headerChanged || frame.TileCount > 0))
	{
		begin();
		if (Upload(frame, headerChanged))
			Bake(frame);
	}

	if (grouped)
		commands->PopGroup();
}

//-----------------------------------------------------------------------------
//
// Images and buffers
//
//-----------------------------------------------------------------------------

bool VkLevelField::Allocate(const LevelFieldFrame& frame)
{
	VulkanDevice* device = fb->device.get();
	const int quality = frame.Quality;

	uint64_t texelBytes = 0;
	int largest = 0;
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
	{
		const LevelFieldSpec& spec = frame.Levels[level];
		if (spec.Cells() == 0 || spec.CellSize <= 0 || spec.BandCells <= 0 ||
			spec.SizeX % LEVEL_FIELD_TILE_CELLS != 0 || spec.SizeY % LEVEL_FIELD_TILE_CELLS != 0 || spec.SizeZ % LEVEL_FIELD_TILE_CELLS != 0)
		{
			Printf(TEXTCOLOR_RED "LevelField: quality %d's level %d is not whole tiles -- particle collision stays off\n", quality, level);
			return false;
		}
		texelBytes += spec.Cells() * (uint64_t)LEVEL_FIELD_BYTES_PER_CELL;
		largest = std::max({ largest, spec.SizeX, spec.SizeY, spec.SizeZ });
	}

	const uint32_t maxDimension = device->PhysicalDevice.Properties.Properties.limits.maxImageDimension3D;
	if ((uint32_t)largest > maxDimension)
	{
		Printf(TEXTCOLOR_RED "LevelField: this device's 3D images are at most %u texels a side; quality %d needs %d -- particle collision stays off at this quality\n",
			(unsigned)maxDimension, quality, largest);
		return false;
	}

	// The review's S6 probe: RG16F as a 3D storage and sampled image, filtered linearly (the draw
	// samples it trilinearly).
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
	{
		const LevelFieldSpec& spec = frame.Levels[level];
		if (!mCompute->IsVolumeFormatSupported(VK_FORMAT_R16G16_SFLOAT, spec.SizeX, spec.SizeY, spec.SizeZ, FieldUsage, true))
		{
			Printf(TEXTCOLOR_RED "LevelField: this device cannot make a %d x %d x %d RG16F storage volume -- particle collision stays off at quality %d\n",
				spec.SizeX, spec.SizeY, spec.SizeZ, quality);
			return false;
		}
	}

	static const char* const levelNames[LEVEL_FIELD_LEVELS] = { "LevelField.Fine", "LevelField.Coarse" };
	bool created = true;
	try
	{
		for (int level = 0; level < LEVEL_FIELD_LEVELS && created; level++)
		{
			const LevelFieldSpec& spec = frame.Levels[level];
			mLevels[level].Image = ImageBuilder()
				.Size3D(spec.SizeX, spec.SizeY, spec.SizeZ)
				.Format(VK_FORMAT_R16G16_SFLOAT)
				.Usage(FieldUsage)
				.DebugName(levelNames[level])
				.TryCreate(device);
			created = mLevels[level].Image != nullptr;
			if (created)
			{
				mLevels[level].View = ImageViewBuilder()
					.Type(VK_IMAGE_VIEW_TYPE_3D)
					.Image(mLevels[level].Image.get(), VK_FORMAT_R16G16_SFLOAT)
					.DebugName(levelNames[level])
					.Create(device);
				created = mLevels[level].View != nullptr;
			}
		}
		if (created)
		{
			mHeader.Image = ImageBuilder()
				.Size(LEVEL_FIELD_HEADER_TEXELS, 1)
				.Format(VK_FORMAT_R32G32B32A32_SFLOAT)
				.Usage(HeaderUsage)
				.DebugName("LevelField.Header")
				.TryCreate(device);
			created = mHeader.Image != nullptr;
			if (created)
			{
				mHeader.View = ImageViewBuilder()
					.Image(mHeader.Image.get(), VK_FORMAT_R32G32B32A32_SFLOAT)
					.DebugName("LevelField.Header")
					.Create(device);
				created = mHeader.View != nullptr;
			}
		}
		if (created)
		{
			mStaging = BufferBuilder()
				.Size(STAGING_BYTES)
				.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
				.DebugName("LevelField.Staging")
				.Create(device);
			mLines = BufferBuilder()
				.Size(LINE_BYTES)
				.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
				.DebugName("LevelField.Lines")
				.Create(device);
			created = mStaging != nullptr && mLines != nullptr;
		}
	}
	catch (const std::exception& e)
	{
		Printf(TEXTCOLOR_RED "LevelField: %s\n", e.what());
		created = false;
	}
	if (!created)
	{
		DestroyNow();
		Printf(TEXTCOLOR_RED "LevelField: could not allocate quality %d (%llu bytes of texels) -- out of video memory? Particle collision stays off at this quality\n",
			quality, (unsigned long long)texelBytes);
		return false;
	}

	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
		mSpecs[level] = frame.Levels[level];
	mQuality = quality;
	mTexelBytes = texelBytes;

	// UNDEFINED -> GENERAL, once, for life; then every texel unbaked and the header "no level".
	mCompute->BeginWork();
	PipelineBarrier barrier;
	for (FieldImage* image : { &mLevels[0], &mLevels[1], &mHeader })
	{
		barrier.AddImage(image->Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
	}
	barrier.Execute(fb->GetCommands()->GetDrawCommands(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
		ClearLevel(level);
	ClearImage(mHeader, 0.0f, 0.0f);
	memset(mWrittenHeader, 0, sizeof(mWrittenHeader));

	Printf("LevelField: allocated quality %d -- fine %d x %d x %d cells at %d map units, coarse %d x %d x %d at %d, %llu bytes of texels (+%llu staging, +%llu line records)\n",
		quality, mSpecs[0].SizeX, mSpecs[0].SizeY, mSpecs[0].SizeZ, mSpecs[0].CellSize,
		mSpecs[1].SizeX, mSpecs[1].SizeY, mSpecs[1].SizeZ, mSpecs[1].CellSize,
		(unsigned long long)texelBytes, (unsigned long long)STAGING_BYTES, (unsigned long long)LINE_BYTES);
	return true;
}

void VkLevelField::DestroyNow()
{
	// Only for objects no command has used yet (a half-finished Allocate).
	for (FieldImage* image : { &mLevels[0], &mLevels[1], &mHeader })
	{
		image->View.reset();
		image->Image.reset();
	}
	mStaging.reset();
	mLines.reset();
}

void VkLevelField::Release(const char* why)
{
	if (!IsAllocated())
		return;

	// This frame's fixed set (written at the start of the frame) and its commands may still name
	// these, so they go on the frame's delete list. The next frame's fixed set binds the stand-ins.
	auto deleteList = fb->GetCommands()->DrawDeleteList.get();
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
		deleteList->Add(std::move(mBakeSets[level]));
	for (FieldImage* image : { &mLevels[0], &mLevels[1], &mHeader })
	{
		deleteList->Add(std::move(image->View));
		deleteList->Add(std::move(image->Image));
	}
	deleteList->Add(std::move(mStaging));
	deleteList->Add(std::move(mLines));

	Printf("LevelField: released (%s) -- %llu bytes of texels freed\n", why, (unsigned long long)mTexelBytes);

	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
		mSpecs[level] = LevelFieldSpec();
	mQuality = 0;
	mTexelBytes = 0;
	memset(mWrittenHeader, 0, sizeof(mWrittenHeader));
	LevelFieldStatus().Allocated = false;
}

void VkLevelField::ClearImage(FieldImage& image, float r, float g)
{
	mCompute->BeginWork();
	VkClearColorValue color = {};
	color.float32[0] = r;
	color.float32[1] = g;
	color.float32[2] = 0.0f;
	color.float32[3] = 0.0f;
	VkImageSubresourceRange range = {};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;
	fb->GetCommands()->GetDrawCommands()->clearColorImage(image.Image->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
}

// Every texel of the level unbaked: (band, 0), what field_bake.comp's invalidation writes.
void VkLevelField::ClearLevel(int level)
{
	if (!IsAllocated() || level < 0 || level >= LEVEL_FIELD_LEVELS)
		return;
	ClearImage(mLevels[level], (float)mSpecs[level].Band(), 0.0f);
}

// Something the CPU side sent could not be done as it believes: empty both volumes and say so, so
// it builds again from nothing. The header keeps its windows -- with every texel unbaked no sample
// is trusted -- and is rewritten next frame.
void VkLevelField::Fail(const char* why)
{
	Printf(TEXTCOLOR_RED "LevelField: %s -- the field is emptied and built again\n", why);
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
		ClearLevel(level);
	static uint64_t failures = 0;
	LevelFieldStatus().Epoch = (uint64_t(1) << 62) + ++failures;
}

//-----------------------------------------------------------------------------
//
// Programs and descriptor sets
//
//-----------------------------------------------------------------------------

bool VkLevelField::EnsureProgram()
{
	if (mBakeProgram)
		return true;
	if (mProgramFailed)
		return false;

	mBakeProgram = mCompute->CreateProgram("shaders/compute/field_bake.comp",
		{ { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER } },
		(uint32_t)sizeof(FieldBakeConstants));
	if (!mBakeProgram)
	{
		// CreateProgram logged why. Not retried this session.
		Printf(TEXTCOLOR_RED "LevelField: the bake program did not build -- particle collision stays off this session\n");
		mProgramFailed = true;
		return false;
	}
	return true;
}

bool VkLevelField::EnsureSets()
{
	if (!mBakeProgram || !IsAllocated())
		return false;
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
	{
		if (mBakeSets[level])
			continue;
		mBakeSets[level] = mCompute->AllocateSet(mBakeProgram.get());
		if (!mBakeSets[level])
		{
			Printf(TEXTCOLOR_RED "LevelField: no descriptor sets for the bake -- particle collision stays off at this quality\n");
			return false;	// Release puts whatever was made on the delete list
		}
		WriteDescriptors()
			.AddStorageImage(mBakeSets[level].get(), 0, mLevels[level].View.get(), VK_IMAGE_LAYOUT_GENERAL)
			.AddBuffer(mBakeSets[level].get(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mLines.get())
			.Execute(fb->device.get());
	}
	return true;
}

//-----------------------------------------------------------------------------
//
// Invalidation, the uploads and the bake
//
//-----------------------------------------------------------------------------

bool VkLevelField::Invalidate(const LevelFieldBox& box)
{
	if (box.Level < 0 || box.Level >= LEVEL_FIELD_LEVELS)
		return false;
	const LevelFieldSpec& spec = mSpecs[box.Level];
	const int tiles[3] = { spec.SizeX / LEVEL_FIELD_TILE_CELLS, spec.SizeY / LEVEL_FIELD_TILE_CELLS, spec.SizeZ / LEVEL_FIELD_TILE_CELLS };

	FieldBakeConstants constants = {};
	for (int axis = 0; axis < 3; axis++)
	{
		if (box.TexelTileMin[axis] < 0 || box.TexelTileMax[axis] > tiles[axis] || box.TexelTileMin[axis] >= box.TexelTileMax[axis])
			return false;
		constants.TexelMin[axis] = box.TexelTileMin[axis] * LEVEL_FIELD_TILE_CELLS;
		constants.TexelCount[axis] = (box.TexelTileMax[axis] - box.TexelTileMin[axis]) * LEVEL_FIELD_TILE_CELLS;
	}
	constants.TexelMin[3] = 1;
	constants.Cell[0] = (float)spec.CellSize;
	constants.Cell[1] = (float)spec.Band();
	mCompute->Dispatch(mBakeProgram.get(), mBakeSets[box.Level].get(), &constants,
		Groups(constants.TexelCount[0]), Groups(constants.TexelCount[1]), Groups(constants.TexelCount[2]));
	return true;
}

// The tiles' column parts as RG16F texels and, when it changed, the header, through the staging
// buffer; the line records into the line buffer. False (after Fail) when a tile or the frame does
// not fit what the buffers and volumes are.
bool VkLevelField::Upload(const LevelFieldFrame& frame, bool header)
{
	const int tileCount = frame.Tiles != nullptr ? frame.TileCount : 0;
	const int lineCount = frame.Lines != nullptr ? frame.LineCount : 0;
	if (tileCount > LEVEL_FIELD_TILES_PER_FRAME || lineCount > LEVEL_FIELD_LINES_PER_FRAME || lineCount < 0)
	{
		Fail("a frame's tiles or line records exceed the buffers");
		return false;
	}

	for (int i = 0; i < tileCount; i++)
	{
		const LevelFieldTile& tile = frame.Tiles[i];
		if (tile.Level < 0 || tile.Level >= LEVEL_FIELD_LEVELS || frame.Distances == nullptr ||
			tile.DistanceOffset + (size_t)LEVEL_FIELD_TILE_TEXELS > frame.DistanceCount ||
			tile.LineFirst < 0 || tile.LineCount < 0 || tile.LineFirst + tile.LineCount > lineCount)
		{
			Fail("a tile outside the frame's data");
			return false;
		}
		const LevelFieldSpec& spec = mSpecs[tile.Level];
		const int tiles[3] = { spec.SizeX / LEVEL_FIELD_TILE_CELLS, spec.SizeY / LEVEL_FIELD_TILE_CELLS, spec.SizeZ / LEVEL_FIELD_TILE_CELLS };
		for (int axis = 0; axis < 3; axis++)
		{
			if (tile.TexelTile[axis] < 0 || tile.TexelTile[axis] >= tiles[axis])
			{
				Fail("a tile outside the volume");
				return false;
			}
		}
	}

	// The copy commands of the frame before have finished (every frame waits for its submissions),
	// so the one staging buffer and the one line buffer are written again each frame.
	const size_t texelBytes = (size_t)tileCount * (size_t)LEVEL_FIELD_TILE_TEXELS * (size_t)LEVEL_FIELD_BYTES_PER_CELL;
	const size_t stagingBytes = texelBytes + (header ? HEADER_BYTES : 0);
	if (stagingBytes > 0)
	{
		uint8_t* data = (uint8_t*)mStaging->Map(0, stagingBytes);
		if (data == nullptr)
		{
			Fail("the staging buffer could not be mapped");
			return false;
		}
		const uint16_t baked = HalfFromFloat(1.0f);
		for (int i = 0; i < tileCount; i++)
		{
			const LevelFieldTile& tile = frame.Tiles[i];
			uint8_t* out = data + (size_t)i * (size_t)LEVEL_FIELD_TILE_TEXELS * (size_t)LEVEL_FIELD_BYTES_PER_CELL;
			const float* in = frame.Distances + tile.DistanceOffset;
			for (int t = 0; t < LEVEL_FIELD_TILE_TEXELS; t++)
			{
				const uint16_t value = HalfFromFloat(in[t]);
				memcpy(out + (size_t)t * 4, &value, 2);
				memcpy(out + (size_t)t * 4 + 2, &baked, 2);
			}
		}
		if (header)
			memcpy(data + texelBytes, frame.Header, HEADER_BYTES);
		mStaging->Unmap();
	}

	if (lineCount > 0)
	{
		void* lines = mLines->Map(0, (size_t)lineCount * sizeof(LevelFieldLine));
		if (lines == nullptr)
		{
			Fail("the line buffer could not be mapped");
			return false;
		}
		memcpy(lines, frame.Lines, (size_t)lineCount * sizeof(LevelFieldLine));
		mLines->Unmap();
	}

	VulkanCommandBuffer* cmd = fb->GetCommands()->GetDrawCommands();
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
	{
		std::vector<VkBufferImageCopy> regions;
		for (int i = 0; i < tileCount; i++)
		{
			const LevelFieldTile& tile = frame.Tiles[i];
			if (tile.Level != level)
				continue;
			VkBufferImageCopy region = {};
			region.bufferOffset = (VkDeviceSize)i * (VkDeviceSize)LEVEL_FIELD_TILE_TEXELS * (VkDeviceSize)LEVEL_FIELD_BYTES_PER_CELL;
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = 0;
			region.imageSubresource.baseArrayLayer = 0;
			region.imageSubresource.layerCount = 1;
			region.imageOffset.x = tile.TexelTile[0] * LEVEL_FIELD_TILE_CELLS;
			region.imageOffset.y = tile.TexelTile[1] * LEVEL_FIELD_TILE_CELLS;
			region.imageOffset.z = tile.TexelTile[2] * LEVEL_FIELD_TILE_CELLS;
			region.imageExtent.width = (uint32_t)LEVEL_FIELD_TILE_CELLS;
			region.imageExtent.height = (uint32_t)LEVEL_FIELD_TILE_CELLS;
			region.imageExtent.depth = (uint32_t)LEVEL_FIELD_TILE_CELLS;
			regions.push_back(region);
		}
		if (!regions.empty())
			cmd->copyBufferToImage(mStaging->buffer, mLevels[level].Image->image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data());
	}

	if (header)
	{
		VkBufferImageCopy region = {};
		region.bufferOffset = (VkDeviceSize)texelBytes;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = (uint32_t)LEVEL_FIELD_HEADER_TEXELS;
		region.imageExtent.height = 1;
		region.imageExtent.depth = 1;
		cmd->copyBufferToImage(mStaging->buffer, mHeader.Image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
		memcpy(mWrittenHeader, frame.Header, sizeof(mWrittenHeader));
	}
	return true;
}

void VkLevelField::Bake(const LevelFieldFrame& frame)
{
	const int tileCount = frame.Tiles != nullptr ? frame.TileCount : 0;
	for (int i = 0; i < tileCount; i++)
	{
		const LevelFieldTile& tile = frame.Tiles[i];
		const LevelFieldSpec& spec = mSpecs[tile.Level];
		FieldBakeConstants constants = {};
		for (int axis = 0; axis < 3; axis++)
		{
			constants.TexelMin[axis] = tile.TexelTile[axis] * LEVEL_FIELD_TILE_CELLS;
			constants.TexelCount[axis] = LEVEL_FIELD_TILE_CELLS;
			constants.WorldCell[axis] = tile.WorldCell[axis];
		}
		constants.TexelMin[3] = 0;
		constants.TexelCount[3] = tile.LineFirst;
		constants.WorldCell[3] = tile.LineCount;
		constants.Cell[0] = (float)spec.CellSize;
		constants.Cell[1] = (float)spec.Band();
		mCompute->Dispatch(mBakeProgram.get(), mBakeSets[tile.Level].get(), &constants,
			Groups(LEVEL_FIELD_TILE_CELLS), Groups(LEVEL_FIELD_TILE_CELLS), Groups(LEVEL_FIELD_TILE_CELLS));
	}
}
