/*
** vk_smokevolume.h
**
** [SMOKEVOLUME] The smoke volume's GPU side: its 3D images and its compute steps.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SMOKE_VOLUME_PLAN.md" #13. The first client of VkComputeManager.
**
** What exists after step 1 (13a):
**   - the volume, allocated only while SmokeVolumeFrame::Active, at the quality it
**     names (r_smoke_quality): density and heat RG16F and velocity RGBA16F, each a
**     ping-pong pair, and the R8 solid mask -- 25 bytes a cell. Formats probed first;
**     a device that cannot, or an allocation that fails, logs one line and leaves
**     smoke off at that quality.
**   - every image kept in layout GENERAL for its whole life (storage writes need it;
**     readers declare GENERAL in their descriptors), cleared on creation and on a new
**     level serial.
**   - the minimal step (shaders/compute/smoke_test.comp, r_smoke_computetest): reads
**     the latest density as a sampler3D, writes the other image of the pair, then
**     the pair's "latest" flips -- the pattern 13b's steps use, so the pair always
**     holds the last two states for 13c's smoothing.
**
** Hooks for the later steps, by name: [13b] the simulation step in Run and the
** velocity pair; [13c] GetDensityHeatView(0 / 1) for the march's ExternalImage
** resolve; [13d] the light grid is a separate allocation beside this one.
**
** CPU-side decisions -- when the volume exists, where its box is, how many steps a
** frame runs -- are made in hw_smokevolume.cpp and arrive in SmokeVolumeFrame, so a
** render rebuild replaces only this file.
**
*/

#pragma once

#include <cstdint>
#include <memory>

#include <zvulkan/vulkanobjects.h>
#include "hw_framecompute.h"

class VulkanRenderDevice;
class VkComputeManager;
class VkComputeProgram;

class VkSmokeVolume
{
public:
	explicit VkSmokeVolume(VkComputeManager* compute);
	~VkSmokeVolume();

	// One frame: allocate, re-allocate, clear or release as the frame says, then run
	// its steps. Records GPU commands only when there is something to do.
	void Run(const SmokeVolumeFrame& frame);

	bool IsAllocated() const { return mDensityHeat[0].Image != nullptr; }
	int GetQuality() const { return mAllocatedQuality; }
	const SmokeGridSpec& GetGrid() const { return mGrid; }

	// [13c] Density (r) and heat (g). stepsAgo 0 = the latest state, 1 = the state one
	// step before it; the march blends the two by TicFrac.
	VulkanImageView* GetDensityHeatView(int stepsAgo) const;
	// [13b] Velocity (xyz), the same way.
	VulkanImageView* GetVelocityView(int stepsAgo) const;
	// [13b] 1 = solid (SH1's LevelSolidity mask), 0 = open.
	VulkanImageView* GetSolidMaskView() const { return mSolidMask.View.get(); }

private:
	struct Volume
	{
		std::unique_ptr<VulkanImage> Image;
		std::unique_ptr<VulkanImageView> View;
	};

	bool Allocate(int quality, const SmokeGridSpec& grid);
	bool CreateVolume(Volume& volume, VkFormat format, const SmokeGridSpec& grid, const char* name);
	void DestroyVolumesNow();
	void Release(const char* why);
	void ClearContents();
	bool EnsureTestProgram();
	void RunTestStep(const SmokeVolumeFrame& frame, int stepIndex);

	VkComputeManager* mCompute = nullptr;
	VulkanRenderDevice* fb = nullptr;

	// The images first, so the descriptor sets below (which name their views) are
	// destroyed before them.
	Volume mDensityHeat[2];
	Volume mVelocity[2];
	Volume mSolidMask;
	int mLatestDensityHeat = 0;	// which of the pair holds the latest state
	int mLatestVelocity = 0;

	SmokeGridSpec mGrid;
	int mAllocatedQuality = 0;
	uint64_t mTexelBytes = 0;
	uint64_t mLevelSerial = 0;

	// A quality this device refused (format or memory): not retried until the quality
	// changes or smoke stops being asked for, so a refusal logs once, not every frame.
	int mRefusedQuality = 0;

	std::unique_ptr<VkComputeProgram> mTestProgram;
	bool mTestProgramFailed = false;
	// [i] reads density-heat image i and writes image 1 - i.
	std::unique_ptr<VulkanDescriptorSet> mTestSets[2];
};
