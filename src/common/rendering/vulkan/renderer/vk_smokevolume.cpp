/*
** vk_smokevolume.cpp
**
** [SMOKEVOLUME] The smoke volume's GPU side. See the header.
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
#include <cmath>
#include <cstring>
#include <exception>
#include <vector>

#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkanobjects.h>

#include "vk_smokevolume.h"
#include "vk_compute.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "hw_framecompute.h"
#include "hw_perflog.h"
#include "i_time.h"
#include "printf.h"
#include "v_text.h"

namespace
{
	const VkImageUsageFlags VolumeUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	// How far the swirl's noise drifts per tic, in noise cells: a whole cell in about
	// three seconds, so the smoke's curls change slowly.
	const float SWIRL_DRIFT_PER_TIC = 0.012f;

	// The push constant blocks, std430: every member a vec4 / ivec4, so the offsets are
	// 0, 16, 32... in both languages (checked from SPIR-V, "Engine docs/
	// SMOKE_13B_IMPL_NOTES.md").

	// shaders/compute/smoke_inject.comp
	struct SmokeInjectConstants
	{
		int32_t RegionMin[4];		// xyz first cell; w the kind (SmokeKernel::EMIT / CARVE / IMPULSE)
		int32_t RegionMax[4];		// xyz one past the last cell
		float ShapeStart[4];		// xyz grid cells; w radius, cells
		float ShapeEnd[4];			// xyz grid cells; w amount
		float HeatStrength[4];		// x heat; y strength, cells per step
		float PushVelocity[4];		// xyz cells per step
	};
	static_assert(sizeof(SmokeInjectConstants) == 96, "SmokeInjectConstants must match smoke_inject.comp's push constant block (96 bytes)");

	// shaders/compute/smoke_advect.comp
	struct SmokeAdvectConstants
	{
		int32_t GridSize[4];		// xyz cells; w cells per tile
		int32_t OriginCell[4];		// xyz world cells; w the step's time, tics
		float Wind[4];				// xyz cells per step; w the furthest a step carries anything
		float Keep[4];				// density, heat, velocity keep; w diffusion
		float Lift[4];				// heat lift, density lift, speed limit, swirl strength
		float Swirl[4];				// noise cells per world cell, drift per tic, blur diagonal, unused
	};
	static_assert(sizeof(SmokeAdvectConstants) == 96, "SmokeAdvectConstants must match smoke_advect.comp's push constant block (96 bytes)");

	// shaders/compute/smoke_tiles.comp
	struct SmokeTilesConstants
	{
		int32_t GridSize[4];		// xyz cells; w cells per tile
		int32_t TileCount[4];		// xyz tiles; w the pass: 0 content, 1 spread, 2 mark
		int32_t RegionMin[4];		// pass 2: first tile
		int32_t RegionMax[4];		// pass 2: one past the last
		float EmptyBelow[4];		// density, heat, speed (cells per step)
	};
	static_assert(sizeof(SmokeTilesConstants) == 80, "SmokeTilesConstants must match smoke_tiles.comp's push constant block (80 bytes)");

	// shaders/compute/smoke_shift.comp
	struct SmokeShiftConstants
	{
		int32_t GridSize[4];		// xyz cells
		int32_t Shift[4];			// xyz: target cell c takes source cell c + Shift
		float Fill[4];				// a target cell with no source cell
	};
	static_assert(sizeof(SmokeShiftConstants) == 48, "SmokeShiftConstants must match smoke_shift.comp's push constant block (48 bytes)");

	uint32_t Groups(int count)
	{
		return (uint32_t)std::max((count + 7) / 8, 1);
	}
}

VkSmokeVolume::VkSmokeVolume(VkComputeManager* compute) : mCompute(compute), fb(compute->GetDevice())
{
	// A step runs on some frames only (35 a second at 90 frames), sometimes twice in one,
	// and a recentre now and then: the perf log reads both per run, not per frame (review
	// NIT, plan 13b).
	PerfLog::CountEachRun("fx.smokesim");
	PerfLog::CountEachRun("fx.smokeshift");
}

VkSmokeVolume::~VkSmokeVolume()
{
	// Members only. At device teardown the GPU is idle (the render device waits for it
	// before anything is destroyed), so the images, views and sets go directly. The CPU
	// side must not believe a volume still exists.
	SmokeVolumeStatus() = SmokeVolumeBackendStatus();
}

VulkanImageView* VkSmokeVolume::GetDensityHeatView(int stepsAgo) const
{
	const int index = stepsAgo == 0 ? mLatest : 1 - mLatest;
	return mDensityHeat[index].View.get();
}

VulkanImageView* VkSmokeVolume::GetVelocityView(int stepsAgo) const
{
	const int index = stepsAgo == 0 ? mLatest : 1 - mLatest;
	return mVelocity[index].View.get();
}

//-----------------------------------------------------------------------------
//
// The frame
//
//-----------------------------------------------------------------------------

void VkSmokeVolume::Run(const SmokeVolumeFrame& frame)
{
	SmokeVolumeBackendStatus& status = SmokeVolumeStatus();

	if (!frame.Active)
	{
		// The CPU side already waited out its linger (or r_smoke went off): free now.
		Release("smoke no longer asked for");
		mRefusedQuality = 0;	// asked again later: try again
		status = SmokeVolumeBackendStatus();
		return;
	}

	if (IsAllocated() && mAllocatedQuality != frame.Quality)
		Release("quality changed");

	const auto refuse = [&]()
	{
		mRefusedQuality = frame.Quality;
		status.Allocated = false;
		status.Quality = 0;
		status.RefusedQuality = mRefusedQuality;
		status.MaskEpoch = 0;
	};

	bool fresh = false;
	if (!IsAllocated())
	{
		if (frame.Quality == mRefusedQuality)
		{
			refuse();
			return;
		}
		// The programs before any memory: a device that cannot build them gets no volume.
		if (!EnsurePrograms() || !Allocate(frame.Quality, frame.Grid))
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
		fresh = true;
		mLevelSerial = frame.LevelSerial;
		status.MaskEpoch = 0;	// all solid, but for no box until a NewBox names one
	}
	status.Allocated = true;
	status.Quality = mAllocatedQuality;
	status.RefusedQuality = 0;

	// [13c] The drawing read some volumes last frame: back to GENERAL before any compute command below
	// touches them. Nothing is recorded when every volume already is.
	RestoreComputeLayouts();

	if (frame.NewBox)
	{
		if (!fresh)
			ClearEverything();
		mLevelSerial = frame.LevelSerial;
		status.MaskEpoch = frame.BoxEpoch;
	}
	else
	{
		if (frame.LevelSerial != mLevelSerial)
		{
			// A new map or a savegame load: the old smoke is gone, the allocation stays.
			mLevelSerial = frame.LevelSerial;
			ClearContents();
		}
		if (frame.ShiftCells[0] != 0 || frame.ShiftCells[1] != 0 || frame.ShiftCells[2] != 0)
			Shift(frame.ShiftCells);
	}

	if (frame.ClearContents)
		ClearContents();

	UploadMask(frame);

	for (int axis = 0; axis < 3; axis++)
		mOriginCell[axis] = frame.OriginCell[axis];
	mHasSmoke = frame.HasSmoke;

	if (frame.Simulate)
	{
		for (int i = 0; i < frame.Steps && i < SmokeVolumeFrame::MAX_STEPS_PER_FRAME; i++)
			RunStep(frame, i);
	}

	// [13c] Smoke to draw this frame: the images the drawing reads go into the layout a post-process
	// read binds (VkTextureManager::GetTexture hands them out only then). With no smoke nothing is
	// read, nothing moves, and nothing is recorded.
	if (mHasSmoke)
		PrepareDrawLayouts();
}

//-----------------------------------------------------------------------------
//
// Images
//
//-----------------------------------------------------------------------------

bool VkSmokeVolume::CreateVolume(Volume& volume, VkFormat format, int width, int height, int depth, const char* name)
{
	volume.Image = ImageBuilder()
		.Size3D(width, height, depth)
		.Format(format)
		.Usage(VolumeUsage)
		.DebugName(name)
		.TryCreate(fb->device.get());
	if (!volume.Image)
		return false;

	volume.View = ImageViewBuilder()
		.Type(VK_IMAGE_VIEW_TYPE_3D)
		.Image(volume.Image.get(), format)
		.DebugName(name)
		.Create(fb->device.get());
	return volume.View != nullptr;
}

void VkSmokeVolume::DestroyVolumesNow()
{
	// Only for images no command has used yet (a half-finished Allocate).
	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mVelocity[0], &mVelocity[1], &mSolidMask, &mTileContent, &mTileActive })
	{
		volume->View.reset();
		volume->Image.reset();
		volume->Layout = VK_IMAGE_LAYOUT_UNDEFINED;	// [13c]
	}
}

bool VkSmokeVolume::Allocate(int quality, const SmokeGridSpec& grid)
{
	VulkanDevice* device = fb->device.get();
	const int tiles[3] =
	{
		(grid.SizeX + SMOKE_TILE_CELLS - 1) / SMOKE_TILE_CELLS,
		(grid.SizeY + SMOKE_TILE_CELLS - 1) / SMOKE_TILE_CELLS,
		(grid.SizeZ + SMOKE_TILE_CELLS - 1) / SMOKE_TILE_CELLS,
	};
	const uint64_t tileBytes = 2 * (uint64_t)tiles[0] * (uint64_t)tiles[1] * (uint64_t)tiles[2];
	const uint64_t texelBytes = grid.Cells() * (uint64_t)SMOKE_BYTES_PER_CELL + tileBytes;

	const uint32_t maxDimension = device->PhysicalDevice.Properties.Properties.limits.maxImageDimension3D;
	const int largest = std::max(grid.SizeX, std::max(grid.SizeY, grid.SizeZ));
	if (grid.Cells() == 0 || (uint32_t)largest > maxDimension)
	{
		Printf(TEXTCOLOR_RED "SmokeVolume: this device's 3D images are at most %u texels a side; quality %d needs %d -- smoke stays off at this quality\n",
			(unsigned)maxDimension, quality, largest);
		return false;
	}

	// The review's S6 probe: every format as a 3D storage and sampled image, filtered
	// linearly where the step samples it (the mask too: the step averages over open cells).
	struct FormatNeed { VkFormat Format; bool LinearFilter; bool TileSized; const char* Name; };
	static const FormatNeed needs[] =
	{
		{ VK_FORMAT_R16G16_SFLOAT, true, false, "RG16F (density and heat)" },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, true, false, "RGBA16F (velocity)" },
		{ VK_FORMAT_R8_UNORM, true, false, "R8 (solid mask)" },
		{ VK_FORMAT_R8_UNORM, false, true, "R8 (tile maps)" },
	};
	for (const FormatNeed& need : needs)
	{
		const int w = need.TileSized ? tiles[0] : grid.SizeX;
		const int h = need.TileSized ? tiles[1] : grid.SizeY;
		const int d = need.TileSized ? tiles[2] : grid.SizeZ;
		if (!mCompute->IsVolumeFormatSupported(need.Format, w, h, d, VolumeUsage, need.LinearFilter))
		{
			Printf(TEXTCOLOR_RED "SmokeVolume: this device cannot make a %d x %d x %d %s storage volume -- smoke stays off at quality %d\n",
				w, h, d, need.Name, quality);
			return false;
		}
	}

	bool created = false;
	try
	{
		created =
			CreateVolume(mDensityHeat[0], VK_FORMAT_R16G16_SFLOAT, grid.SizeX, grid.SizeY, grid.SizeZ, "SmokeVolume.DensityHeat0") &&
			CreateVolume(mDensityHeat[1], VK_FORMAT_R16G16_SFLOAT, grid.SizeX, grid.SizeY, grid.SizeZ, "SmokeVolume.DensityHeat1") &&
			CreateVolume(mVelocity[0], VK_FORMAT_R16G16B16A16_SFLOAT, grid.SizeX, grid.SizeY, grid.SizeZ, "SmokeVolume.Velocity0") &&
			CreateVolume(mVelocity[1], VK_FORMAT_R16G16B16A16_SFLOAT, grid.SizeX, grid.SizeY, grid.SizeZ, "SmokeVolume.Velocity1") &&
			CreateVolume(mSolidMask, VK_FORMAT_R8_UNORM, grid.SizeX, grid.SizeY, grid.SizeZ, "SmokeVolume.SolidMask") &&
			CreateVolume(mTileContent, VK_FORMAT_R8_UNORM, tiles[0], tiles[1], tiles[2], "SmokeVolume.TileContent") &&
			CreateVolume(mTileActive, VK_FORMAT_R8_UNORM, tiles[0], tiles[1], tiles[2], "SmokeVolume.TileActive");
	}
	catch (const std::exception& e)
	{
		Printf(TEXTCOLOR_RED "SmokeVolume: %s\n", e.what());
		created = false;
	}
	if (!created)
	{
		DestroyVolumesNow();
		Printf(TEXTCOLOR_RED "SmokeVolume: could not allocate quality %d (%llu bytes of texels) -- out of video memory? Smoke stays off at this quality\n",
			quality, (unsigned long long)texelBytes);
		return false;
	}

	mGrid = grid;
	for (int axis = 0; axis < 3; axis++)
		mTiles[axis] = tiles[axis];
	mAllocatedQuality = quality;
	mTexelBytes = texelBytes;
	mLatest = 0;

	// UNDEFINED -> GENERAL, once, for life; then empty contents over an all-solid mask.
	mCompute->BeginWork();
	PipelineBarrier barrier;
	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mVelocity[0], &mVelocity[1], &mSolidMask, &mTileContent, &mTileActive })
	{
		barrier.AddImage(volume->Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
	}
	barrier.Execute(fb->GetCommands()->GetDrawCommands(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	// [13c] Tracked from here on: the drawing's read moves some volumes out of GENERAL for a frame.
	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mVelocity[0], &mVelocity[1], &mSolidMask, &mTileContent, &mTileActive })
		volume->Layout = VK_IMAGE_LAYOUT_GENERAL;
	ClearEverything();

	Printf("SmokeVolume: allocated quality %d -- %d x %d x %d cells at %d map units (%d x %d x %d), 7 volumes, %llu bytes of texels\n",
		quality, grid.SizeX, grid.SizeY, grid.SizeZ, grid.CellSize,
		grid.SizeX * grid.CellSize, grid.SizeY * grid.CellSize, grid.SizeZ * grid.CellSize,
		(unsigned long long)texelBytes);
	return true;
}

void VkSmokeVolume::Release(const char* why)
{
	if (!IsAllocated())
		return;

	// Commands recorded this frame may still name these, so they go on the frame's
	// delete list, like every other GPU object the renderer lets go of mid-session.
	auto deleteList = fb->GetCommands()->DrawDeleteList.get();
	for (int i = 0; i < 2; i++)
	{
		deleteList->Add(std::move(mInjectSets[i]));
		deleteList->Add(std::move(mAdvectSets[i]));
		deleteList->Add(std::move(mTilesSets[i]));
		deleteList->Add(std::move(mShiftDensitySets[i]));
		deleteList->Add(std::move(mShiftVelocitySets[i]));
		deleteList->Add(std::move(mShiftMaskOutSets[i]));
		deleteList->Add(std::move(mShiftMaskInSets[i]));
	}
	mSetsReady = false;
	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mVelocity[0], &mVelocity[1], &mSolidMask, &mTileContent, &mTileActive })
	{
		deleteList->Add(std::move(volume->View));
		deleteList->Add(std::move(volume->Image));
		volume->Layout = VK_IMAGE_LAYOUT_UNDEFINED;	// [13c]
	}
	deleteList->Add(std::move(mStaging));

	Printf("SmokeVolume: released (%s) -- %llu bytes of texels freed\n", why, (unsigned long long)mTexelBytes);

	mGrid = SmokeGridSpec();
	mTiles[0] = mTiles[1] = mTiles[2] = 0;
	mAllocatedQuality = 0;
	mTexelBytes = 0;
	mLatest = 0;
	mHasSmoke = false;
	SmokeVolumeStatus().Allocated = false;
}

void VkSmokeVolume::ClearImage(Volume& volume, float value)
{
	mCompute->BeginWork();
	VkClearColorValue color = {};
	color.float32[0] = value;
	color.float32[1] = value;
	color.float32[2] = value;
	color.float32[3] = value;
	VkImageSubresourceRange range = {};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;
	fb->GetCommands()->GetDrawCommands()->clearColorImage(volume.Image->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
}

// Density, heat, velocity and the tile maps; the mask stays.
void VkSmokeVolume::ClearContents()
{
	if (!IsAllocated())
		return;
	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mVelocity[0], &mVelocity[1], &mTileContent, &mTileActive })
		ClearImage(*volume, 0.0f);
	mLatest = 0;
}

// A new box: contents empty and the mask ALL SOLID, until the CPU's tiles open it.
void VkSmokeVolume::ClearEverything()
{
	if (!IsAllocated())
		return;
	ClearContents();
	ClearImage(mSolidMask, 1.0f);
}

// [13c] THE DRAWING'S READ. Compute binds the volumes in layout GENERAL (storage images and the
// simulation's own samplers); a post-process pass binds its inputs SHADER_READ_ONLY_OPTIMAL
// (VkDescriptorSetManager::GetInput). So on a frame with smoke to draw, the three images the march reads
// go to SHADER_READ_ONLY_OPTIMAL at the end of Run, and at the start of the next Run every volume that
// is not in GENERAL goes back before any compute command. A frame with no smoke moves nothing and
// records nothing.
void VkSmokeVolume::RestoreComputeLayouts()
{
	if (!IsAllocated())
		return;

	PipelineBarrier barrier;
	bool changed = false;
	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mVelocity[0], &mVelocity[1], &mSolidMask, &mTileContent, &mTileActive })
	{
		if (volume->Image && volume->Layout != VK_IMAGE_LAYOUT_GENERAL)
		{
			barrier.AddImage(volume->Image.get(), volume->Layout, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
			volume->Layout = VK_IMAGE_LAYOUT_GENERAL;
			changed = true;
		}
	}
	if (!changed)
		return;

	mCompute->BeginWork();	// outside any render pass, inside fx.compute
	barrier.Execute(fb->GetCommands()->GetDrawCommands(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void VkSmokeVolume::PrepareDrawLayouts()
{
	if (!IsAllocated())
		return;

	PipelineBarrier barrier;
	bool changed = false;
	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mTileActive })
	{
		if (volume->Image && volume->Layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		{
			barrier.AddImage(volume->Image.get(), volume->Layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
			volume->Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			changed = true;
		}
	}
	if (!changed)
		return;

	mCompute->BeginWork();	// outside any render pass, inside fx.compute
	barrier.Execute(fb->GetCommands()->GetDrawCommands(), VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

//-----------------------------------------------------------------------------
//
// Programs and descriptor sets
//
//-----------------------------------------------------------------------------

bool VkSmokeVolume::EnsurePrograms()
{
	if (mProgramsReady)
		return true;
	if (mProgramsFailed)
		return false;

	const VkDescriptorType storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	const VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	mInjectProgram = mCompute->CreateProgram("shaders/compute/smoke_inject.comp",
		{ { 0, storage }, { 1, storage }, { 2, sampled } },
		(uint32_t)sizeof(SmokeInjectConstants));
	mAdvectProgram = mCompute->CreateProgram("shaders/compute/smoke_advect.comp",
		{ { 0, sampled }, { 1, sampled }, { 2, sampled }, { 3, storage }, { 4, storage }, { 5, storage } },
		(uint32_t)sizeof(SmokeAdvectConstants));
	mTilesProgram = mCompute->CreateProgram("shaders/compute/smoke_tiles.comp",
		{ { 0, sampled }, { 1, sampled }, { 2, storage }, { 3, storage } },
		(uint32_t)sizeof(SmokeTilesConstants));
	const std::vector<VkComputeBinding> shiftBindings = { { 0, sampled }, { 1, storage } };
	mShiftRG = mCompute->CreateProgram("shaders/compute/smoke_shift.comp", shiftBindings, (uint32_t)sizeof(SmokeShiftConstants), "#define TARGET_RG16F\n");
	mShiftRGBA = mCompute->CreateProgram("shaders/compute/smoke_shift.comp", shiftBindings, (uint32_t)sizeof(SmokeShiftConstants), "#define TARGET_RGBA16F\n");
	mShiftR8 = mCompute->CreateProgram("shaders/compute/smoke_shift.comp", shiftBindings, (uint32_t)sizeof(SmokeShiftConstants), "#define TARGET_R8\n");

	if (!mInjectProgram || !mAdvectProgram || !mTilesProgram || !mShiftRG || !mShiftRGBA || !mShiftR8)
	{
		// CreateProgram logged which one and why. Not retried this session.
		Printf(TEXTCOLOR_RED "SmokeVolume: a simulation program did not build -- smoke stays off this session\n");
		mInjectProgram.reset();
		mAdvectProgram.reset();
		mTilesProgram.reset();
		mShiftRG.reset();
		mShiftRGBA.reset();
		mShiftR8.reset();
		mProgramsFailed = true;
		return false;
	}
	mProgramsReady = true;
	return true;
}

bool VkSmokeVolume::EnsureSets()
{
	if (mSetsReady)
		return true;
	if (!mProgramsReady || !IsAllocated())
		return false;

	const auto allocate = [&](VkComputeProgram* program, std::unique_ptr<VulkanDescriptorSet>& set)
	{
		set = mCompute->AllocateSet(program);
		return set != nullptr;
	};

	VulkanSampler* sampler = mCompute->GetVolumeSampler();
	const VkImageLayout general = VK_IMAGE_LAYOUT_GENERAL;
	for (int i = 0; i < 2; i++)
	{
		const int o = 1 - i;
		const bool ok =
			allocate(mInjectProgram.get(), mInjectSets[i]) &&
			allocate(mAdvectProgram.get(), mAdvectSets[i]) &&
			allocate(mTilesProgram.get(), mTilesSets[i]) &&
			allocate(mShiftRG.get(), mShiftDensitySets[i]) &&
			allocate(mShiftRGBA.get(), mShiftVelocitySets[i]) &&
			allocate(mShiftRG.get(), mShiftMaskOutSets[i]) &&
			allocate(mShiftR8.get(), mShiftMaskInSets[i]);
		if (!ok)
		{
			Printf(TEXTCOLOR_RED "SmokeVolume: no descriptor sets for the simulation -- smoke stays off at this quality\n");
			return false;	// Release puts whatever was made on the delete list
		}

		WriteDescriptors()
			.AddStorageImage(mInjectSets[i].get(), 0, mDensityHeat[i].View.get(), general)
			.AddStorageImage(mInjectSets[i].get(), 1, mVelocity[i].View.get(), general)
			.AddCombinedImageSampler(mInjectSets[i].get(), 2, mSolidMask.View.get(), sampler, general)
			.AddCombinedImageSampler(mAdvectSets[i].get(), 0, mDensityHeat[i].View.get(), sampler, general)
			.AddCombinedImageSampler(mAdvectSets[i].get(), 1, mVelocity[i].View.get(), sampler, general)
			.AddCombinedImageSampler(mAdvectSets[i].get(), 2, mSolidMask.View.get(), sampler, general)
			.AddStorageImage(mAdvectSets[i].get(), 3, mTileActive.View.get(), general)
			.AddStorageImage(mAdvectSets[i].get(), 4, mDensityHeat[o].View.get(), general)
			.AddStorageImage(mAdvectSets[i].get(), 5, mVelocity[o].View.get(), general)
			.AddCombinedImageSampler(mTilesSets[i].get(), 0, mDensityHeat[i].View.get(), sampler, general)
			.AddCombinedImageSampler(mTilesSets[i].get(), 1, mVelocity[i].View.get(), sampler, general)
			.AddStorageImage(mTilesSets[i].get(), 2, mTileContent.View.get(), general)
			.AddStorageImage(mTilesSets[i].get(), 3, mTileActive.View.get(), general)
			.AddCombinedImageSampler(mShiftDensitySets[i].get(), 0, mDensityHeat[i].View.get(), sampler, general)
			.AddStorageImage(mShiftDensitySets[i].get(), 1, mDensityHeat[o].View.get(), general)
			.AddCombinedImageSampler(mShiftVelocitySets[i].get(), 0, mVelocity[i].View.get(), sampler, general)
			.AddStorageImage(mShiftVelocitySets[i].get(), 1, mVelocity[o].View.get(), general)
			.AddCombinedImageSampler(mShiftMaskOutSets[i].get(), 0, mSolidMask.View.get(), sampler, general)
			.AddStorageImage(mShiftMaskOutSets[i].get(), 1, mDensityHeat[i].View.get(), general)
			.AddCombinedImageSampler(mShiftMaskInSets[i].get(), 0, mDensityHeat[i].View.get(), sampler, general)
			.AddStorageImage(mShiftMaskInSets[i].get(), 1, mSolidMask.View.get(), general)
			.Execute(fb->device.get());
	}
	mSetsReady = true;
	return true;
}

//-----------------------------------------------------------------------------
//
// The mask, the recentre, the step
//
//-----------------------------------------------------------------------------

void VkSmokeVolume::UploadMask(const SmokeVolumeFrame& frame)
{
	if (frame.MaskUploadCount <= 0 || frame.MaskUploads == nullptr || frame.MaskBytes == nullptr || frame.MaskByteCount == 0)
		return;
	if (frame.MaskByteCount > SMOKE_MASK_UPLOAD_BYTES_PER_FRAME)
	{
		if (!mUploadWarned)
		{
			mUploadWarned = true;
			Printf(TEXTCOLOR_RED "SmokeVolume: a frame's mask tiles (%llu bytes) exceed the staging buffer -- skipped\n", (unsigned long long)frame.MaskByteCount);
		}
		return;
	}

	if (!mStaging)
	{
		try
		{
			mStaging = BufferBuilder()
				.Size(SMOKE_MASK_UPLOAD_BYTES_PER_FRAME)
				.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
				.DebugName("SmokeVolume.MaskStaging")
				.Create(fb->device.get());
		}
		catch (const std::exception& e)
		{
			Printf(TEXTCOLOR_RED "SmokeVolume: no staging buffer for the solid mask: %s\n", e.what());
			mStaging.reset();
		}
		if (!mStaging)
			return;
	}

	// The copy commands of the frame before have finished (every frame waits for its
	// submissions), so the one buffer is written again each frame.
	void* data = mStaging->Map(0, frame.MaskByteCount);
	memcpy(data, frame.MaskBytes, frame.MaskByteCount);
	mStaging->Unmap();

	const int size[3] = { mGrid.SizeX, mGrid.SizeY, mGrid.SizeZ };
	std::vector<VkBufferImageCopy> regions;
	regions.reserve(frame.MaskUploadCount);
	for (int i = 0; i < frame.MaskUploadCount; i++)
	{
		const SmokeMaskUpload& upload = frame.MaskUploads[i];
		bool inside = true;
		for (int axis = 0; axis < 3; axis++)
		{
			if (upload.Min[axis] < 0 || upload.Size[axis] <= 0 || upload.Min[axis] + upload.Size[axis] > size[axis])
				inside = false;
		}
		const size_t bytes = inside ? (size_t)upload.Size[0] * (size_t)upload.Size[1] * (size_t)upload.Size[2] : 0;
		if (!inside || upload.Offset + bytes > frame.MaskByteCount)
			continue;

		VkBufferImageCopy region = {};
		region.bufferOffset = upload.Offset;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageOffset.x = upload.Min[0];
		region.imageOffset.y = upload.Min[1];
		region.imageOffset.z = upload.Min[2];
		region.imageExtent.width = (uint32_t)upload.Size[0];
		region.imageExtent.height = (uint32_t)upload.Size[1];
		region.imageExtent.depth = (uint32_t)upload.Size[2];
		regions.push_back(region);
	}
	if (regions.empty())
		return;

	mCompute->BeginWork();
	fb->GetCommands()->GetDrawCommands()->copyBufferToImage(mStaging->buffer, mSolidMask.Image->image, VK_IMAGE_LAYOUT_GENERAL,
		(uint32_t)regions.size(), regions.data());
}

void VkSmokeVolume::DispatchGrid(VkComputeProgram* program, VulkanDescriptorSet* set, const void* constants)
{
	mCompute->Dispatch(program, set, constants, Groups(mGrid.SizeX), Groups(mGrid.SizeY), Groups(mGrid.SizeZ));
}

void VkSmokeVolume::DispatchTiles(int pass, int latest, const int tileMin[3], const int tileMax[3])
{
	SmokeTilesConstants constants = {};
	constants.GridSize[0] = mGrid.SizeX;
	constants.GridSize[1] = mGrid.SizeY;
	constants.GridSize[2] = mGrid.SizeZ;
	constants.GridSize[3] = SMOKE_TILE_CELLS;
	for (int axis = 0; axis < 3; axis++)
	{
		constants.TileCount[axis] = mTiles[axis];
		constants.RegionMin[axis] = tileMin[axis];
		constants.RegionMax[axis] = tileMax[axis];
	}
	constants.TileCount[3] = pass;
	constants.EmptyBelow[0] = SMOKE_EMPTY_DENSITY;
	constants.EmptyBelow[1] = SMOKE_EMPTY_HEAT;
	constants.EmptyBelow[2] = SMOKE_EMPTY_VELOCITY;

	if (pass == 2)
	{
		mCompute->Dispatch(mTilesProgram.get(), mTilesSets[latest].get(), &constants,
			Groups(tileMax[0] - tileMin[0]), Groups(tileMax[1] - tileMin[1]), Groups(tileMax[2] - tileMin[2]));
	}
	else
	{
		mCompute->Dispatch(mTilesProgram.get(), mTilesSets[latest].get(), &constants, Groups(mTiles[0]), Groups(mTiles[1]), Groups(mTiles[2]));
	}
}

// A recentre: every volume moves by whole tiles so each cell keeps its world position.
void VkSmokeVolume::Shift(const int shift[3])
{
	if (!IsAllocated() || !mSetsReady)
		return;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	VkCommandBufferManager* commands = fb->GetCommands();
	mCompute->BeginWork();	// so fx.smokeshift nests inside fx.compute
	commands->PushGroup("fx.smokeshift");

	const int latest = mLatest;
	const int other = 1 - mLatest;

	SmokeShiftConstants moved = {};
	moved.GridSize[0] = mGrid.SizeX;
	moved.GridSize[1] = mGrid.SizeY;
	moved.GridSize[2] = mGrid.SizeZ;
	for (int axis = 0; axis < 3; axis++)
		moved.Shift[axis] = shift[axis];
	SmokeShiftConstants copied = moved;
	copied.Shift[0] = copied.Shift[1] = copied.Shift[2] = 0;
	SmokeShiftConstants maskMoved = moved;
	maskMoved.Fill[0] = 1.0f;	// what comes in is solid until the CPU rasterises it

	// The mask, through the density pair's older image as scratch (overwritten next).
	DispatchGrid(mShiftRG.get(), mShiftMaskOutSets[other].get(), &maskMoved);
	DispatchGrid(mShiftR8.get(), mShiftMaskInSets[other].get(), &copied);
	// Density and heat, then velocity: moved into the older image, copied back, so both
	// images of each pair hold the latest state on the new origin.
	DispatchGrid(mShiftRG.get(), mShiftDensitySets[latest].get(), &moved);
	DispatchGrid(mShiftRG.get(), mShiftDensitySets[other].get(), &copied);
	DispatchGrid(mShiftRGBA.get(), mShiftVelocitySets[latest].get(), &moved);
	DispatchGrid(mShiftRGBA.get(), mShiftVelocitySets[other].get(), &copied);
	// The tile maps from the moved state: all active for the content pass, then spread.
	ClearImage(mTileActive, 1.0f);
	const int none[3] = { 0, 0, 0 };
	DispatchTiles(0, latest, none, none);
	DispatchTiles(1, latest, none, none);

	commands->PopGroup();

	if (timed)
		PerfLog::AddCpuSample("fx.smokeshift", (double)(I_nsTime() - startNs) / 1e6);
}

void VkSmokeVolume::RunStep(const SmokeVolumeFrame& frame, int stepIndex)
{
	if (!IsAllocated() || !mSetsReady)
		return;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	VkCommandBufferManager* commands = fb->GetCommands();
	mCompute->BeginWork();	// so fx.smokesim nests inside fx.compute
	commands->PushGroup("fx.smokesim");

	const int latest = mLatest;
	const int size[3] = { mGrid.SizeX, mGrid.SizeY, mGrid.SizeZ };

	// 1. This step's kernels, in place on the latest images, each marking its tiles (and
	//    one tile around them) active for the advection.
	if (frame.Kernels != nullptr)
	{
		for (int k = 0; k < frame.KernelCount; k++)
		{
			const SmokeKernel& kernel = frame.Kernels[k];
			if (kernel.Step != stepIndex)
				continue;

			int regionMin[3], regionMax[3];
			bool empty = false;
			for (int axis = 0; axis < 3; axis++)
			{
				regionMin[axis] = std::clamp(kernel.RegionMin[axis], 0, size[axis]);
				regionMax[axis] = std::clamp(kernel.RegionMax[axis], 0, size[axis]);
				if (regionMin[axis] >= regionMax[axis])
					empty = true;
			}
			if (empty)
				continue;

			SmokeInjectConstants constants = {};
			for (int axis = 0; axis < 3; axis++)
			{
				constants.RegionMin[axis] = regionMin[axis];
				constants.RegionMax[axis] = regionMax[axis];
				constants.ShapeStart[axis] = kernel.Start[axis];
				constants.ShapeEnd[axis] = kernel.End[axis];
				constants.PushVelocity[axis] = kernel.Velocity[axis];
			}
			constants.RegionMin[3] = kernel.Kind;
			constants.ShapeStart[3] = kernel.Radius;
			constants.ShapeEnd[3] = kernel.Amount;
			constants.HeatStrength[0] = kernel.Heat;
			constants.HeatStrength[1] = kernel.Strength;
			mCompute->Dispatch(mInjectProgram.get(), mInjectSets[latest].get(), &constants,
				Groups(regionMax[0] - regionMin[0]), Groups(regionMax[1] - regionMin[1]), Groups(regionMax[2] - regionMin[2]));

			int tileMin[3], tileMax[3];
			for (int axis = 0; axis < 3; axis++)
			{
				tileMin[axis] = std::max(regionMin[axis] / SMOKE_TILE_CELLS - 1, 0);
				tileMax[axis] = std::min((regionMax[axis] + SMOKE_TILE_CELLS - 1) / SMOKE_TILE_CELLS + 1, mTiles[axis]);
			}
			DispatchTiles(2, latest, tileMin, tileMax);
		}
	}

	// 2. Advection, forces, diffusion, dissipation: latest -> the other image.
	const SmokeSimSettings& sim = frame.Sim;
	const int stepTime = frame.MapTime - (frame.Steps - 1 - stepIndex);
	SmokeAdvectConstants advect = {};
	for (int axis = 0; axis < 3; axis++)
	{
		advect.GridSize[axis] = size[axis];
		advect.OriginCell[axis] = frame.OriginCell[axis];
		advect.Wind[axis] = sim.Wind[axis];
	}
	advect.GridSize[3] = SMOKE_TILE_CELLS;
	advect.OriginCell[3] = stepTime;
	advect.Wind[3] = sim.MaxDisplacement;
	advect.Keep[0] = sim.DensityKeep;
	advect.Keep[1] = sim.HeatKeep;
	advect.Keep[2] = sim.VelocityKeep;
	advect.Keep[3] = sim.Diffusion;
	advect.Lift[0] = sim.HeatLift;
	advect.Lift[1] = sim.DensityLift;
	advect.Lift[2] = sim.MaxSpeed;
	advect.Lift[3] = sim.Turbulence;
	advect.Swirl[0] = sim.TurbulenceFrequency;
	advect.Swirl[1] = SWIRL_DRIFT_PER_TIC;
	advect.Swirl[2] = (float)(stepTime & 3);
	DispatchGrid(mAdvectProgram.get(), mAdvectSets[latest].get(), &advect);

	// 3. The tile maps from the new state, then 4. the flip.
	const int next = 1 - latest;
	const int none[3] = { 0, 0, 0 };
	DispatchTiles(0, next, none, none);
	DispatchTiles(1, next, none, none);
	mLatest = next;

	commands->PopGroup();

	if (timed)
		PerfLog::AddCpuSample("fx.smokesim", (double)(I_nsTime() - startNs) / 1e6);
}
