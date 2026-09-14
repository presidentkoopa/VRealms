/*
** vk_smokevolume.h
**
** [SMOKEVOLUME] The smoke volume's GPU side: its 3D images and its simulation step.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SMOKE_VOLUME_PLAN.md" #13 (13a, 13b). The first client of
** VkComputeManager.
**
** THE VOLUME, allocated only while SmokeVolumeFrame::Active, at the quality it names
** (r_smoke_quality): density and heat RG16F and velocity RGBA16F, each a ping-pong pair,
** the R8 solid mask -- 25 bytes a cell -- plus two tiny R8 tile maps (one texel per
** SMOKE_TILE_CELLS^3 cells). Formats are probed first; a device that cannot, an
** allocation that fails, or a simulation program that does not build logs one line and
** leaves smoke off at that quality. Every image stays in layout GENERAL for its life.
**
** A FRAME (Run), in SmokeVolumeFrame's order: a new box (all empty, the mask all solid)
** or a recentre (smoke_shift.comp moves every volume by whole tiles), a clear, the mask
** tiles the CPU rasterised copied in, then each simulation step:
**   1. every kernel of that step (smoke_inject.comp, in place on the latest images),
**      each also marking its tiles active (smoke_tiles.comp pass 2);
**   2. smoke_advect.comp: the latest state of each pair -> the other image;
**   3. smoke_tiles.comp passes 0 and 1: which tiles hold anything, spread by a tile;
**   4. the pairs' "latest" flips.
** So after a step each pair holds the last two whole states, which 13c's march blends by
** TicFrac (owner answer 3). A kernel changes the latest image in place before the step
** reads it, so the older state already carries that tic's event: a puff appears at the
** start of the blend rather than fading in over one tic. After a recentre both images of
** a pair hold the moved latest state (no blend across the move).
**
** Hooks for the later steps, by name: [13c] GetDensityHeatView(0 / 1) for the march's
** ExternalImage resolve, GetTileActiveView to skip empty tiles, GetOriginCell and
** HasSmoke; [13d] the light grid is a separate allocation beside this one.
**
** CPU-side decisions -- when the volume exists, where its box is, what goes in, the
** mask -- are made in hw_smokevolume.cpp and arrive in SmokeVolumeFrame, so a render
** rebuild replaces only this file and the shaders.
**
*/

#pragma once

#include <cstdint>
#include <memory>

#include <zvulkan/vulkanobjects.h>
#include "hw_framecompute.h"
#include "vulkan/textures/vk_imagetransition.h"	// [13c] VkTextureImage: a post-process pass binds the volumes

class VulkanRenderDevice;
class VkComputeManager;
class VkComputeProgram;

class VkSmokeVolume
{
public:
	explicit VkSmokeVolume(VkComputeManager* compute);
	~VkSmokeVolume();

	// One frame: allocate, re-allocate, clear or release as the frame says, then do its
	// work. Records GPU commands only when there is something to do.
	void Run(const SmokeVolumeFrame& frame);

	bool IsAllocated() const { return mDensityHeat[0].Image != nullptr; }
	int GetQuality() const { return mAllocatedQuality; }
	const SmokeGridSpec& GetGrid() const { return mGrid; }

	// [13c] Density (r) and heat (g). stepsAgo 0 = the latest state, 1 = the state one
	// step before it; the march blends the two by TicFrac.
	VulkanImageView* GetDensityHeatView(int stepsAgo) const;
	// Velocity (xyz, cells per step), the same way.
	VulkanImageView* GetVelocityView(int stepsAgo) const;
	// 1 = solid (SH1's LevelSolidity mask), 0 = open.
	VulkanImageView* GetSolidMaskView() const { return mSolidMask.View.get(); }
	// [13c] One texel per SMOKE_TILE_CELLS^3 cells: 1 = the tile may hold smoke.
	VulkanImageView* GetTileActiveView() const { return mTileActive.View.get(); }
	// [13c] The images themselves, for the drawing's external image resolve (VkTextureManager::
	// GetTexture, PPTextureType::ExternalImage). A post-process pass may bind one only while its
	// Layout is SHADER_READ_ONLY_OPTIMAL. Run puts density, heat and the tile map there at its end on
	// a frame with smoke to draw, and takes every volume back to GENERAL before its next compute work.
	VkTextureImage* GetDensityHeatImage(int stepsAgo) { return &mDensityHeat[stepsAgo == 0 ? mLatest : 1 - mLatest]; }
	VkTextureImage* GetTileActiveImage() { return &mTileActive; }
	// [13c] The box's first cell in world cells (x CellSize = map units), and whether the
	// CPU's bounds say visible smoke may exist -- both as of the last Run.
	const int* GetOriginCell() const { return mOriginCell; }
	bool HasSmoke() const { return mHasSmoke; }

private:
	// [13c] A VkTextureImage (Image and View, as before), so a post-process pass can bind one
	// (VkDescriptorSetManager::GetInput) and its Layout is tracked. Every volume is GENERAL from its
	// allocation on, except while the drawing reads it (RestoreComputeLayouts, PrepareDrawLayouts).
	using Volume = VkTextureImage;

	bool Allocate(int quality, const SmokeGridSpec& grid);
	bool CreateVolume(Volume& volume, VkFormat format, int width, int height, int depth, const char* name);
	void DestroyVolumesNow();
	void Release(const char* why);
	void ClearImage(Volume& volume, float value);
	void ClearContents();
	void ClearEverything();
	// [13c] Every volume back to GENERAL (start of Run), and density, heat and the tile map to
	// SHADER_READ_ONLY_OPTIMAL for the drawing (end of Run, only with smoke to draw). Each records
	// one barrier, and only when a layout actually changes.
	void RestoreComputeLayouts();
	void PrepareDrawLayouts();
	bool EnsurePrograms();
	bool EnsureSets();
	void UploadMask(const SmokeVolumeFrame& frame);
	void Shift(const int shift[3]);
	void RunStep(const SmokeVolumeFrame& frame, int stepIndex);
	void DispatchTiles(int pass, int latest, const int tileMin[3], const int tileMax[3]);
	void DispatchGrid(VkComputeProgram* program, VulkanDescriptorSet* set, const void* constants);

	VkComputeManager* mCompute = nullptr;
	VulkanRenderDevice* fb = nullptr;

	// Programs first, then the images, the staging buffer and the descriptor sets, so on
	// destruction the sets (which name the views) go before the images.
	std::unique_ptr<VkComputeProgram> mInjectProgram;
	std::unique_ptr<VkComputeProgram> mAdvectProgram;
	std::unique_ptr<VkComputeProgram> mTilesProgram;
	std::unique_ptr<VkComputeProgram> mShiftRG;		// target rg16f
	std::unique_ptr<VkComputeProgram> mShiftRGBA;	// target rgba16f
	std::unique_ptr<VkComputeProgram> mShiftR8;		// target r8
	bool mProgramsReady = false;
	bool mProgramsFailed = false;

	Volume mDensityHeat[2];
	Volume mVelocity[2];
	Volume mSolidMask;
	Volume mTileContent;
	Volume mTileActive;
	int mLatest = 0;	// which image of both pairs holds the latest state

	std::unique_ptr<VulkanBuffer> mStaging;	// SMOKE_MASK_UPLOAD_BYTES_PER_FRAME, for the mask tiles

	// [i] = the sets whose "latest" is image i of the pairs.
	std::unique_ptr<VulkanDescriptorSet> mInjectSets[2];			// storage D[i], V[i]; sampled mask
	std::unique_ptr<VulkanDescriptorSet> mAdvectSets[2];			// sampled D[i], V[i], mask; storage tiles, D[1-i], V[1-i]
	std::unique_ptr<VulkanDescriptorSet> mTilesSets[2];				// sampled D[i], V[i]; storage both tile maps
	std::unique_ptr<VulkanDescriptorSet> mShiftDensitySets[2];		// D[i] -> D[1-i]
	std::unique_ptr<VulkanDescriptorSet> mShiftVelocitySets[2];		// V[i] -> V[1-i]
	std::unique_ptr<VulkanDescriptorSet> mShiftMaskOutSets[2];		// mask -> D[i] (scratch)
	std::unique_ptr<VulkanDescriptorSet> mShiftMaskInSets[2];		// D[i] -> mask
	bool mSetsReady = false;

	SmokeGridSpec mGrid;
	int mTiles[3] = { 0, 0, 0 };
	int mAllocatedQuality = 0;
	uint64_t mTexelBytes = 0;
	uint64_t mLevelSerial = 0;
	int mOriginCell[3] = { 0, 0, 0 };
	bool mHasSmoke = false;
	bool mUploadWarned = false;

	// A quality this device refused (format, memory, a program, a descriptor set): not
	// retried until the quality changes or smoke stops being asked for, so a refusal
	// logs once, not every frame.
	int mRefusedQuality = 0;
};
