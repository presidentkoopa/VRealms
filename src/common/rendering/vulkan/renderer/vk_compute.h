/*
** vk_compute.h
**
** [COMPUTE] GPU compute for effects: programs, their descriptor sets, dispatches.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: effects that simulate on the GPU -- the smoke volume (#13)
** first, then the level collision field (#8), the debris pool (#9), the impulse
** field (#12) and surface damage (#17) -- all need the same few things: compile a
** shaders/compute/*.comp lump, make its pipeline, give it descriptor sets that
** never collide with the scene's set 1 or fixed set, and record dispatches OUTSIDE
** a render pass in perf groups. That is this manager. Each effect is a client with
** its own home beside it (vk_smokevolume.h), not code in here.
**
** ZVulkan already had compute (ShaderType::Compute, ComputePipelineBuilder); the
** only addition there was ImageBuilder::Size3D ("Engine docs/
** REVIEW_SMOKE_DEBRIS_DAMAGE.md" S1).
**
** The frame: VulkanRenderDevice::RunFrameCompute -> RunFrame, once per frame for the
** main view, before the eye loop (hw_entrypoint.cpp). A frame with no compute work
** records nothing at all -- no render pass end, no group.
**
** Presentation only: nothing is ever read back from the GPU into the game.
**
*/

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <zvulkan/vulkanobjects.h>
#include "zstring.h"

class VulkanRenderDevice;
class VkSmokeVolume;
class VkLevelField;	// [LEVELFIELD]
class VkDebrisPool;	// [DEBRISPOOL]
struct FrameComputeInput;

// One binding of a compute program's descriptor set (set 0), compute stage.
struct VkComputeBinding
{
	int Binding = 0;
	VkDescriptorType Type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
};

// A compiled compute shader with its set layout, pipeline layout and pipeline.
class VkComputeProgram
{
public:
	FString Name;
	std::unique_ptr<VulkanShader> Shader;
	std::unique_ptr<VulkanDescriptorSetLayout> SetLayout;
	std::unique_ptr<VulkanPipelineLayout> Layout;
	std::unique_ptr<VulkanPipeline> Pipeline;
	uint32_t PushConstantBytes = 0;
};

class VkComputeManager
{
public:
	VkComputeManager(VulkanRenderDevice* fb);
	~VkComputeManager();

	// One frame's compute work. See the file comment.
	void RunFrame(const FrameComputeInput& input);

	// Compiles `lumpName` (a private engine lump, like the scene shaders) as a compute
	// shader behind "#version 450", with set 0 made of `bindings` and a push constant
	// range of `pushConstantBytes` (0 = none). A missing lump, a compile error or a
	// pipeline failure is logged once and returns null -- never a fatal error mid-frame.
	// [13b] `defines` (e.g. "#define TARGET_R8\n") goes between the version line and the
	// lump, for variants of one lump -- a storage image's format qualifier, say.
	std::unique_ptr<VkComputeProgram> CreateProgram(const char* lumpName, const std::vector<VkComputeBinding>& bindings, uint32_t pushConstantBytes, const char* defines = nullptr);

	// A descriptor set for `program`'s set layout from this manager's own pools.
	// Null if even a fresh pool cannot hold it.
	std::unique_ptr<VulkanDescriptorSet> AllocateSet(VkComputeProgram* program);

	// Can this device make a width x height x depth 3D image of `format` with `usage`,
	// with the matching optimal-tiling features (storage, sampled, and linear filtering
	// when asked)? The format probe the review asks for (S6).
	bool IsVolumeFormatSupported(VkFormat format, int width, int height, int depth, VkImageUsageFlags usage, bool linearFilter);

	// Linear, no mips, clamp to edge on U, V AND W -- for sampling volumes. (The sampler
	// manager's samplers leave W on repeat, which wraps a volume's top into its bottom.)
	VulkanSampler* GetVolumeSampler();

	// Before the first command a client records this frame: ends the scene's render pass
	// (compute is recorded outside render passes, as the scene transfer does) and opens
	// the "fx.compute" group, closed by RunFrame. Idempotent within a frame.
	void BeginWork();

	// Binds `program` and `set`, pushes `pushConstants` (PushConstantBytes of them) and
	// dispatches that many work groups. Calls BeginWork.
	void Dispatch(VkComputeProgram* program, VulkanDescriptorSet* set, const void* pushConstants, uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ);

	VulkanRenderDevice* GetDevice() const { return fb; }

	// [13c] The smoke march's ExternalImage resolve reads the volume's views from here.
	// Null until smoke is first asked for.
	VkSmokeVolume* GetSmokeVolume() const { return mSmokeVolume.get(); }

	// [LEVELFIELD] The level collision field (#8), whose volumes and header the fixed set binds
	// (VkDescriptorSetManager::UpdateFixedSet). Null until a colliding particle is first spawned.
	VkLevelField* GetLevelField() const { return mLevelField.get(); }

	// [DEBRISPOOL] The debris pool (#9), whose pieces and definitions set 1 binds (VkDescriptorSetManager::UpdateHWBufferSet).
	// Null until a debris burst is first taken.
	VkDebrisPool* GetDebrisPool() const { return mDebrisPool.get(); }

private:
	VulkanRenderDevice* fb = nullptr;

	// Declared before the clients, so every client's descriptor sets are freed before
	// the pools that hold them.
	std::vector<std::unique_ptr<VulkanDescriptorPool>> mPools;
	std::unique_ptr<VulkanSampler> mVolumeSampler;

	std::unique_ptr<VkSmokeVolume> mSmokeVolume;
	std::unique_ptr<VkLevelField> mLevelField;	// [LEVELFIELD]
	std::unique_ptr<VkDebrisPool> mDebrisPool;	// [DEBRISPOOL]

	bool mWorkBegun = false;
};
