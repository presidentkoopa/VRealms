/*
** vk_levelfield.h
**
** [LEVELFIELD] The level collision field's GPU side: its volumes, its header image and its bake.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #8, "Engine docs/COLLISION_8_IMPL_NOTES.md". A client
** of VkComputeManager, beside the smoke volume (vk_smokevolume.h).
**
** THE FIELD, allocated only while LevelFieldFrame::Active, at the quality it names
** (r_particlecollision_quality): one RG16F volume per level (r signed distance in map units, g 1 =
** baked) plus a 3 x 1 RGBA32F header image holding the windows (hw_framecompute.h), a staging buffer
** for the tiles' column parts and the header, and a storage buffer of line records. The volume
** format is probed first; a device that cannot, an allocation that fails, or a program that does
** not build logs one line and leaves collision off at that quality (particles keep F4's plane).
** Every image stays in layout GENERAL for its life -- the bake writes it as a storage image and
** gpuparticles.vp samples it through the fixed set (vk_descriptorset.cpp, bindings 5, 6 and 9).
**
** A FRAME (Run), in LevelFieldFrame's order: allocate or free; whole levels cleared; boxes
** invalidated; the header copied in when it changed; each tile's column part copied in, then
** field_bake.comp's line pass over it. All of it by commands in one stream, so a draw recorded
** before this frame's compute (a camera texture) reads the last frame's header with the last
** frame's texels, and one recorded after reads this frame's with this frame's.
**
** A failure after the build started (a staging buffer that cannot be made, a tile out of range)
** empties both volumes and raises the status epoch, so the CPU side builds again from nothing
** rather than trusting a tile that was never written.
**
** CPU-side decisions -- when the field exists, where its windows are, which tiles, what goes in --
** are made in hw_levelfield.cpp and arrive in LevelFieldFrame, so a render rebuild replaces only
** this file, the lump and gpuparticles.vp's sampling.
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

class VkLevelField
{
public:
	explicit VkLevelField(VkComputeManager* compute);
	~VkLevelField();

	// One frame: allocate, free, clear, invalidate and bake as the frame says. Records GPU
	// commands only when there is something to do.
	void Run(const LevelFieldFrame& frame);

	bool IsAllocated() const { return mLevels[0].Image != nullptr; }
	int GetQuality() const { return mQuality; }

	// For the fixed set (VkDescriptorSetManager::UpdateFixedSet): level 0 fine, 1 coarse, and the
	// header. Null while not allocated.
	VulkanImageView* GetFieldView(int level) const;
	VulkanImageView* GetHeaderView() const { return mHeader.View.get(); }

private:
	struct FieldImage
	{
		std::unique_ptr<VulkanImage> Image;
		std::unique_ptr<VulkanImageView> View;
	};

	bool Allocate(const LevelFieldFrame& frame);
	void DestroyNow();
	void Release(const char* why);
	bool EnsureProgram();
	bool EnsureSets();
	void ClearImage(FieldImage& image, float r, float g);
	void ClearLevel(int level);
	void Fail(const char* why);
	bool Invalidate(const LevelFieldBox& box);
	bool Upload(const LevelFieldFrame& frame, bool header);
	void Bake(const LevelFieldFrame& frame);

	VkComputeManager* mCompute = nullptr;
	VulkanRenderDevice* fb = nullptr;

	// The program first, then the images, the buffers and the descriptor sets, so on destruction
	// the sets (which name the views and the line buffer) go before what they name.
	std::unique_ptr<VkComputeProgram> mBakeProgram;
	bool mProgramFailed = false;

	FieldImage mLevels[LEVEL_FIELD_LEVELS];
	FieldImage mHeader;
	std::unique_ptr<VulkanBuffer> mStaging;	// LEVEL_FIELD_TILES_PER_FRAME tiles of RG16F texels + the header
	std::unique_ptr<VulkanBuffer> mLines;	// LEVEL_FIELD_LINES_PER_FRAME line records

	std::unique_ptr<VulkanDescriptorSet> mBakeSets[LEVEL_FIELD_LEVELS];	// storage volume [level], line buffer

	LevelFieldSpec mSpecs[LEVEL_FIELD_LEVELS];
	int mQuality = 0;
	uint64_t mTexelBytes = 0;
	float mWrittenHeader[LEVEL_FIELD_HEADER_TEXELS][4] = {};

	// A quality this device refused: not retried until the quality changes or collision stops being
	// asked for, so a refusal logs once.
	int mRefusedQuality = 0;
};
