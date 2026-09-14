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
#include <exception>

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

	// shaders/compute/smoke_test.comp's push constants, std430: offsets 0 / 12 / 16 / 28 /
	// 32 / 36 / 40 / 44 (checked from SPIR-V, "Engine docs/SMOKE_13A_IMPL_NOTES.md").
	struct SmokeTestConstants
	{
		int32_t GridSize[3];	// cells per axis
		float Time;				// the step's maptime, tics
		float BlobCenter[3];	// cells
		float BlobRadius;		// cells
		float Decay;			// the previous state's multiplier per step
		float Rise;				// cells per step the previous state moves up
		float Amount;			// density added at the blob's centre per step
		float HeatAmount;		// heat added at the blob's centre per step
	};
	static_assert(sizeof(SmokeTestConstants) == 48, "SmokeTestConstants must match smoke_test.comp's push constant block (48 bytes)");
}

VkSmokeVolume::VkSmokeVolume(VkComputeManager* compute) : mCompute(compute), fb(compute->GetDevice())
{
	// A step runs on some frames only (35 a second at 90 frames), sometimes twice in one:
	// the perf log reads fx.smokesim per step, not per frame (review NIT, plan 13b).
	PerfLog::CountEachRun("fx.smokesim");
}

VkSmokeVolume::~VkSmokeVolume()
{
	// Members only. At device teardown the GPU is idle (the render device waits for it
	// before anything is destroyed), so the images, views and sets go directly.
}

VulkanImageView* VkSmokeVolume::GetDensityHeatView(int stepsAgo) const
{
	const int index = stepsAgo == 0 ? mLatestDensityHeat : 1 - mLatestDensityHeat;
	return mDensityHeat[index].View.get();
}

VulkanImageView* VkSmokeVolume::GetVelocityView(int stepsAgo) const
{
	const int index = stepsAgo == 0 ? mLatestVelocity : 1 - mLatestVelocity;
	return mVelocity[index].View.get();
}

void VkSmokeVolume::Run(const SmokeVolumeFrame& frame)
{
	if (!frame.Active)
	{
		// The CPU side already waited out its linger (or r_smoke went off): free now.
		Release("smoke no longer asked for");
		mRefusedQuality = 0;	// asked again later: try again
		return;
	}

	if (IsAllocated() && mAllocatedQuality != frame.Quality)
		Release("quality changed");

	if (!IsAllocated())
	{
		if (frame.Quality == mRefusedQuality)
			return;
		if (!Allocate(frame.Quality, frame.Grid))
		{
			mRefusedQuality = frame.Quality;
			return;
		}
		mLevelSerial = frame.LevelSerial;
	}
	else if (frame.LevelSerial != mLevelSerial)
	{
		// A new map or a savegame load: the old smoke is gone, the allocation stays.
		mLevelSerial = frame.LevelSerial;
		ClearContents();
	}

	for (int i = 0; i < frame.Steps && i < SmokeVolumeFrame::MAX_STEPS_PER_FRAME; i++)
	{
		if (frame.ComputeTest)
			RunTestStep(frame, i);
		// [13b] Otherwise the simulation step, in fx.smokesim: inject and carve from the
		// SH4 queues, push from SH3's impulses, forces (buoyancy, wind, damping), advect,
		// then diffuse and dissipate -- each reading one image of a pair and writing the
		// other, so the pairs keep the last two states for 13c's smoothing.
	}
}

bool VkSmokeVolume::CreateVolume(Volume& volume, VkFormat format, const SmokeGridSpec& grid, const char* name)
{
	volume.Image = ImageBuilder()
		.Size3D(grid.SizeX, grid.SizeY, grid.SizeZ)
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
	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mVelocity[0], &mVelocity[1], &mSolidMask })
	{
		volume->View.reset();
		volume->Image.reset();
	}
}

bool VkSmokeVolume::Allocate(int quality, const SmokeGridSpec& grid)
{
	VulkanDevice* device = fb->device.get();
	const uint64_t texelBytes = grid.Cells() * (uint64_t)SMOKE_BYTES_PER_CELL;

	const uint32_t maxDimension = device->PhysicalDevice.Properties.Properties.limits.maxImageDimension3D;
	const int largest = std::max(grid.SizeX, std::max(grid.SizeY, grid.SizeZ));
	if (grid.Cells() == 0 || (uint32_t)largest > maxDimension)
	{
		Printf(TEXTCOLOR_RED "SmokeVolume: this device's 3D images are at most %u texels a side; quality %d needs %d -- smoke stays off at this quality\n",
			(unsigned)maxDimension, quality, largest);
		return false;
	}

	// The review's S6 probe: every format as a 3D storage and sampled image, filtered
	// linearly where the march and advection will sample it.
	struct FormatNeed { VkFormat Format; bool LinearFilter; const char* Name; };
	static const FormatNeed needs[] =
	{
		{ VK_FORMAT_R16G16_SFLOAT, true, "RG16F (density and heat)" },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, true, "RGBA16F (velocity)" },
		{ VK_FORMAT_R8_UNORM, false, "R8 (solid mask)" },
	};
	for (const FormatNeed& need : needs)
	{
		if (!mCompute->IsVolumeFormatSupported(need.Format, grid.SizeX, grid.SizeY, grid.SizeZ, VolumeUsage, need.LinearFilter))
		{
			Printf(TEXTCOLOR_RED "SmokeVolume: this device cannot make a %d x %d x %d %s storage volume -- smoke stays off at quality %d\n",
				grid.SizeX, grid.SizeY, grid.SizeZ, need.Name, quality);
			return false;
		}
	}

	bool created = false;
	try
	{
		created =
			CreateVolume(mDensityHeat[0], VK_FORMAT_R16G16_SFLOAT, grid, "SmokeVolume.DensityHeat0") &&
			CreateVolume(mDensityHeat[1], VK_FORMAT_R16G16_SFLOAT, grid, "SmokeVolume.DensityHeat1") &&
			CreateVolume(mVelocity[0], VK_FORMAT_R16G16B16A16_SFLOAT, grid, "SmokeVolume.Velocity0") &&
			CreateVolume(mVelocity[1], VK_FORMAT_R16G16B16A16_SFLOAT, grid, "SmokeVolume.Velocity1") &&
			CreateVolume(mSolidMask, VK_FORMAT_R8_UNORM, grid, "SmokeVolume.SolidMask");
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
	mAllocatedQuality = quality;
	mTexelBytes = texelBytes;

	// UNDEFINED -> GENERAL, once, for life; then zero every texel.
	mCompute->BeginWork();
	PipelineBarrier barrier;
	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mVelocity[0], &mVelocity[1], &mSolidMask })
	{
		barrier.AddImage(volume->Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
	}
	barrier.Execute(fb->GetCommands()->GetDrawCommands(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	ClearContents();

	Printf("SmokeVolume: allocated quality %d -- %d x %d x %d cells at %d map units (%d x %d x %d), 5 volumes, %llu bytes of texels\n",
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
	for (auto& set : mTestSets)
		deleteList->Add(std::move(set));
	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mVelocity[0], &mVelocity[1], &mSolidMask })
	{
		deleteList->Add(std::move(volume->View));
		deleteList->Add(std::move(volume->Image));
	}

	Printf("SmokeVolume: released (%s) -- %llu bytes of texels freed\n", why, (unsigned long long)mTexelBytes);

	mGrid = SmokeGridSpec();
	mAllocatedQuality = 0;
	mTexelBytes = 0;
	mLatestDensityHeat = 0;
	mLatestVelocity = 0;
}

void VkSmokeVolume::ClearContents()
{
	if (!IsAllocated())
		return;

	mCompute->BeginWork();
	VulkanCommandBuffer* cmd = fb->GetCommands()->GetDrawCommands();

	VkClearColorValue zero = {};
	VkImageSubresourceRange range = {};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;

	for (Volume* volume : { &mDensityHeat[0], &mDensityHeat[1], &mVelocity[0], &mVelocity[1], &mSolidMask })
		cmd->clearColorImage(volume->Image->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);

	mLatestDensityHeat = 0;
	mLatestVelocity = 0;
}

bool VkSmokeVolume::EnsureTestProgram()
{
	if (mTestProgram)
		return true;
	if (mTestProgramFailed)
		return false;

	mTestProgram = mCompute->CreateProgram("shaders/compute/smoke_test.comp",
		{
			{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },	// DensityPrevious, sampler3D
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE },			// DensityNext, image3D rg16f
		},
		(uint32_t)sizeof(SmokeTestConstants));

	if (!mTestProgram)
	{
		mTestProgramFailed = true;	// CreateProgram logged why
		return false;
	}
	return true;
}

void VkSmokeVolume::RunTestStep(const SmokeVolumeFrame& frame, int stepIndex)
{
	if (!IsAllocated() || !EnsureTestProgram())
		return;

	if (!mTestSets[0] || !mTestSets[1])
	{
		for (int i = 0; i < 2; i++)
		{
			mTestSets[i] = mCompute->AllocateSet(mTestProgram.get());
			if (!mTestSets[i])
			{
				Printf(TEXTCOLOR_RED "SmokeVolume: no descriptor set for the compute test step\n");
				mTestProgramFailed = true;
				mTestProgram.reset();
				return;
			}
			WriteDescriptors()
				.AddCombinedImageSampler(mTestSets[i].get(), 0, mDensityHeat[i].View.get(), mCompute->GetVolumeSampler(), VK_IMAGE_LAYOUT_GENERAL)
				.AddStorageImage(mTestSets[i].get(), 1, mDensityHeat[1 - i].View.get(), VK_IMAGE_LAYOUT_GENERAL)
				.Execute(fb->device.get());
		}
	}

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	// This step's own world time: the last step of the frame is the frame's maptime.
	// Everything below is a function of it -- no RNG -- so the pattern is the same on
	// every machine and every run.
	const int stepTime = frame.MapTime - (frame.Steps - 1 - stepIndex);
	const double angle = stepTime * (2.0 * 3.14159265358979323846 / 350.0);	// one orbit per 350 tics

	SmokeTestConstants constants = {};
	constants.GridSize[0] = mGrid.SizeX;
	constants.GridSize[1] = mGrid.SizeY;
	constants.GridSize[2] = mGrid.SizeZ;
	constants.Time = (float)stepTime;
	constants.BlobCenter[0] = (float)(mGrid.SizeX * 0.5 + std::cos(angle) * mGrid.SizeX * 0.25);
	constants.BlobCenter[1] = (float)(mGrid.SizeY * 0.5 + std::sin(angle) * mGrid.SizeY * 0.25);
	constants.BlobCenter[2] = (float)(mGrid.SizeZ * 0.375);
	constants.BlobRadius = 6.f;
	constants.Decay = 0.96f;
	constants.Rise = 0.3f;
	constants.Amount = 0.25f;
	constants.HeatAmount = 0.1f;

	VkCommandBufferManager* commands = fb->GetCommands();
	mCompute->BeginWork();	// so fx.smokesim nests inside fx.compute
	commands->PushGroup("fx.smokesim");
	mCompute->Dispatch(mTestProgram.get(), mTestSets[mLatestDensityHeat].get(), &constants,
		(uint32_t)((mGrid.SizeX + 7) / 8), (uint32_t)((mGrid.SizeY + 7) / 8), (uint32_t)((mGrid.SizeZ + 7) / 8));
	commands->PopGroup();

	// The image just written is now the latest; the other one is the state before it.
	mLatestDensityHeat = 1 - mLatestDensityHeat;

	if (timed)
		PerfLog::AddCpuSample("fx.smokesim", (double)(I_nsTime() - startNs) / 1e6);
}
