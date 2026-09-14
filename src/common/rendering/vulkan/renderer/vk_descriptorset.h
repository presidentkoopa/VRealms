/*
** vk_descriptorset.h
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

#pragma once

#include "zvulkan/vulkanobjects.h"
#include <list>
#include "tarray.h"

class VulkanRenderDevice;
class VkTextureImage;	// [2a]
class VkMaterial;
class PPTextureInput;
class VkPPRenderPassSetup;

class VkDescriptorSetManager
{
public:
	VkDescriptorSetManager(VulkanRenderDevice* fb);
	~VkDescriptorSetManager();

	void Init();
	void Deinit();
	void BeginFrame();
	void UpdateFixedSet();
	void UpdateHWBufferSet();
	void ResetHWTextureSets();

	VulkanDescriptorSetLayout* GetHWBufferSetLayout() { return HWBufferSetLayout.get(); }
	VulkanDescriptorSetLayout* GetFixedSetLayout() { return FixedSetLayout.get(); }
	VulkanDescriptorSetLayout* GetTextureSetLayout(int numLayers);

	VulkanDescriptorSet* GetHWBufferDescriptorSet() { return HWBufferSet.get(); }
	VulkanDescriptorSet* GetFixedDescriptorSet() { return FixedSet.get(); }
	VulkanDescriptorSet* GetNullTextureDescriptorSet();

	// [2a] Whether a render target's depth attachment is the scene depth image that
	// fixed binding 3 was written with this frame, attached the same way (flat, or
	// every layer under multiview). The condition for
	// VkRenderState::SetSceneDepthReadable to open a read-only depth pass whose
	// effects sample binding 3; anything else -- a camera texture, a save picture,
	// one layer of a layered image -- is refused and draws as before 2a.
	bool IsSceneDepthReadTarget(VulkanImageView* depthStencilView, int layers, uint32_t viewMask) const;

	std::unique_ptr<VulkanDescriptorSet> AllocateTextureDescriptorSet(int numLayers);

	VulkanDescriptorSet* GetInput(VkPPRenderPassSetup* passSetup, const TArray<PPTextureInput>& textures, bool bindShadowMapBuffers);

	void AddMaterial(VkMaterial* texture);
	void RemoveMaterial(VkMaterial* texture);

private:
	void CreateHWBufferSetLayout();
	void CreateFixedSetLayout();
	void CreateHWBufferPool();
	void CreateFixedSetPool();
	void EnsureLevelFieldStandIns();	// [LEVELFIELD]

	std::unique_ptr<VulkanDescriptorSet> AllocatePPDescriptorSet(VulkanDescriptorSetLayout* layout);

	VulkanRenderDevice* fb = nullptr;

	std::unique_ptr<VulkanDescriptorSetLayout> HWBufferSetLayout;
	std::unique_ptr<VulkanDescriptorSetLayout> FixedSetLayout;
	std::vector<std::unique_ptr<VulkanDescriptorSetLayout>> TextureSetLayouts;

	std::unique_ptr<VulkanDescriptorPool> HWBufferDescriptorPool;
	std::unique_ptr<VulkanDescriptorPool> FixedDescriptorPool;

	std::unique_ptr<VulkanDescriptorPool> PPDescriptorPool;

	int TextureDescriptorSetsLeft = 0;
	int TextureDescriptorsLeft = 0;
	std::vector<std::unique_ptr<VulkanDescriptorPool>> TextureDescriptorPools;

	// [LEVELFIELD] Fixed set bindings 5, 6 and 9 while the level field does not exist: a 1-texel volume
	// whose g (baked) is 0 and a zero header, so gpuparticles.vp finds "no level"; and the field's sampler
	// (linear, repeating on u, v and w). Declared before the sets, so the sets go first on destruction.
	std::unique_ptr<VulkanImage> LevelFieldStandIn;
	std::unique_ptr<VulkanImageView> LevelFieldStandInView;
	std::unique_ptr<VulkanImage> LevelFieldHeaderStandIn;
	std::unique_ptr<VulkanImageView> LevelFieldHeaderStandInView;
	std::unique_ptr<VulkanSampler> LevelFieldSampler;

	std::unique_ptr<VulkanDescriptorSet> HWBufferSet;
	std::unique_ptr<VulkanDescriptorSet> FixedSet;
	std::unique_ptr<VulkanDescriptorSet> NullTextureDescriptorSet;

	std::list<VkMaterial*> Materials;

	// [2a] What UpdateFixedSet last put in binding 3: the texture, the exact image
	// it held then (so a rebuild after the write is noticed), and whether the view
	// was the layered array view. Null texture = binding 3 left unwritten.
	VkTextureImage* SceneDepthReadTexture = nullptr;
	VulkanImage* SceneDepthReadImage = nullptr;
	bool SceneDepthReadLayered = false;

	static const int maxSets = 10;
};
