/*
** vk_surfacedamage.h
**
** [SURFACEDAMAGE] The surface damage atlas's GPU side: the pages, the brush and detail images, the data buffer, and the
** stamp, mip and cooling dispatches.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SURFACE_DAMAGE_PLAN.md" #17, "Engine docs/SURFACE_DAMAGE_17_IMPL_NOTES.md". A client of VkComputeManager, after
** the debris pool.
**
** ALLOCATED only while SurfaceDamageFrame::Active, at the page count it names (r_damage_memory):
**   - Pages: a 2048 x 2048 x pages RGBA8 2D array with two mip levels, storage and sampled, GENERAL for life. Written by
**     shaders/compute/damage_stamp.comp (level 0; its DAMAGE_MIP variant level 1) and damage_cool.comp; read by main.fp at
**     fixed binding 7. Cleared once when made; a reused tile is cleared by its first stamp dispatch.
**   - Brushes: every brush variant, 128 x 128 x layers RGBA8 with every mip, read by the stamp program.
**   - Detail: the looks' tiling detail textures, 256 x 256 x layers RGBA8 with every mip, read by main.fp at fixed binding 8.
**   - Data: the looks, surface records and hash (hw_surfacedamageframe.h), host-visible, read by main.fp at set 1 binding 13.
**   - Stamps and tiles: a frame's dispatch lists, host-visible.
** A page count this device cannot allocate, or a program that does not build, logs one line and refuses: the CPU side then
** holds nothing and no draw carries a key, so every surface draws as without damage.
**
** A FRAME (Run), in SurfaceDamageFrame's order: allocate or free; the brush and detail images when their generation moved;
** the data span; the stamps; the cooling; the mip level of every stamped or cooled tile -- all in fx.damagepaint inside
** fx.compute, recorded before the scene, so this frame's draws read this frame's damage.
**
** CPU-side decisions -- which surfaces, which tiles, what to stamp and cool -- are made in hw_surfacedamage.cpp and arrive in
** SurfaceDamageFrame, so a render rebuild replaces only this file, the two compute lumps and main.fp's lookup.
**
*/

#pragma once

#include <cstdint>
#include <memory>

#include <zvulkan/vulkanobjects.h>
#include "hw_surfacedamageframe.h"

class VulkanRenderDevice;
class VkComputeManager;
class VkComputeProgram;

class VkSurfaceDamage
{
public:
	explicit VkSurfaceDamage(VkComputeManager* compute);
	~VkSurfaceDamage();

	// One frame: allocate, free, upload, stamp, cool and rebuild mips as the frame says. Records GPU commands only when there is
	// something to do. A frame whose serial it has already acted on is ignored.
	void Run(const SurfaceDamageFrame& frame);

	// Bindable: the pages, the data and the detail all exist (the descriptor sets bind them only then).
	bool IsAllocated() const { return mPagesImage != nullptr && mData != nullptr && mDetailView != nullptr; }
	bool HasPages() const { return mPagesImage != nullptr; }

	// For fixed bindings 7 and 8 and set 1 binding 13 (VkDescriptorSetManager). Null while not allocated.
	VulkanImageView* GetPagesView() const { return mPagesView.get(); }
	VulkanSampler* GetPagesSampler() const { return mPagesSampler.get(); }
	VulkanImageView* GetDetailView() const { return mDetailView.get(); }
	VulkanSampler* GetDetailSampler() const { return mDetailSampler.get(); }
	VulkanBuffer* GetDataBuffer() const { return mData.get(); }

private:
	bool EnsurePrograms();
	bool EnsureSamplers();
	bool IsPageFormatSupported(int pages);
	bool Allocate(int pages, int hashEntries);
	void DestroyNow();
	void Release(const char* why);
	bool UploadLayers(std::unique_ptr<VulkanImage>& image, std::unique_ptr<VulkanImageView>& view, const uint8_t* pixels, int layers, int side, int levels, const char* name);
	void UploadData(const SurfaceDamageFrame& frame);
	bool MakeSets();
	void DispatchTiles(VkComputeProgram* program, VulkanDescriptorSet* set, int first, int count, int slotTexels, float keep, float sub);

	VkComputeManager* mCompute = nullptr;
	VulkanRenderDevice* fb = nullptr;

	// Programs and samplers first; then the images and buffers; the descriptor sets last, so on destruction the sets (which
	// name the images and buffers) go before what they name.
	std::unique_ptr<VkComputeProgram> mStampProgram;
	std::unique_ptr<VkComputeProgram> mMipProgram;
	std::unique_ptr<VkComputeProgram> mCoolProgram;
	bool mProgramsFailed = false;
	std::unique_ptr<VulkanSampler> mPagesSampler;
	std::unique_ptr<VulkanSampler> mDetailSampler;
	std::unique_ptr<VulkanSampler> mBrushSampler;

	std::unique_ptr<VulkanImage> mPagesImage;
	std::unique_ptr<VulkanImageView> mPagesView;		// both levels, sampled
	std::unique_ptr<VulkanImageView> mLevelZeroView;	// storage
	std::unique_ptr<VulkanImageView> mLevelOneView;		// storage
	std::unique_ptr<VulkanImage> mBrushImage;
	std::unique_ptr<VulkanImageView> mBrushView;
	std::unique_ptr<VulkanImage> mDetailImage;
	std::unique_ptr<VulkanImageView> mDetailView;
	std::unique_ptr<VulkanBuffer> mData;
	std::unique_ptr<VulkanBuffer> mStamps;
	std::unique_ptr<VulkanBuffer> mTiles;

	std::unique_ptr<VulkanDescriptorSet> mStampSet;
	std::unique_ptr<VulkanDescriptorSet> mMipSet;
	std::unique_ptr<VulkanDescriptorSet> mCoolSet;

	int mPageCount = 0;
	int mHashEntries = 0;
	uint64_t mLastSerial = 0;
	uint64_t mDataGeneration = 0;
	uint64_t mBrushGeneration = 0;
	uint64_t mDetailGeneration = 0;
	bool mUploadAll = false;

	// A page count this device refused: not retried until the count changes or damage stops being asked for, so a refusal
	// logs once.
	int mRefusedPages = 0;
};
