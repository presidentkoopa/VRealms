/*
** vk_descriptorset.cpp
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

#include "vk_descriptorset.h"
#include "vk_streambuffer.h"
#include "vk_raytrace.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/textures/vk_texture.h"
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_hwbuffer.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/system/vk_buffer.h"
#include "flatvertices.h"
#include "hw_viewpointuniforms.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "v_2ddrawer.h"

#include "vk_postprocess.h"

VkDescriptorSetManager::VkDescriptorSetManager(VulkanRenderDevice* fb) : fb(fb)
{
	CreateHWBufferSetLayout();
	CreateFixedSetLayout();
	CreateHWBufferPool();
	CreateFixedSetPool();
}

VkDescriptorSetManager::~VkDescriptorSetManager()
{
	while (!Materials.empty())
		RemoveMaterial(Materials.back());
}

void VkDescriptorSetManager::Init()
{
	UpdateFixedSet();
	UpdateHWBufferSet();
}

void VkDescriptorSetManager::Deinit()
{
	while (!Materials.empty())
		RemoveMaterial(Materials.back());
}

void VkDescriptorSetManager::BeginFrame()
{
	UpdateFixedSet();
	UpdateHWBufferSet();
}

void VkDescriptorSetManager::UpdateHWBufferSet()
{
	fb->GetCommands()->DrawDeleteList->Add(std::move(HWBufferSet));

	HWBufferSet = HWBufferDescriptorPool->tryAllocate(HWBufferSetLayout.get());
	if (!HWBufferSet)
	{
		fb->GetCommands()->WaitForCommands(false);
		HWBufferSet = HWBufferDescriptorPool->allocate(HWBufferSetLayout.get());
	}

	const size_t viewpointRange = screen->mViewpoints ? (size_t)screen->mViewpoints->GetBlockSize() * 2 : sizeof(HWViewpointUniforms);

	// [GPUPARTICLES] Binding 5 is in the layout for every pipeline, so it is
	// always written. The ring is created beside the bone buffer before Init;
	// if it somehow is not there, the bone buffer stands in -- only the
	// gpuparticles effect reads binding 5, and its draw is gated off unless the
	// real ring exists (GpuParticleBuffer::IsDrawable).
	VkHardwareDataBuffer* gpuParticleSSO = fb->GetBufferManager()->GpuParticleSSO ? fb->GetBufferManager()->GpuParticleSSO : fb->GetBufferManager()->BoneBufferSSO;

	// [DRAWNLINES] Binding 6, the same arrangement: always written, the bone
	// buffer standing in if the line buffer is somehow absent -- only the
	// drawnlines effect reads it, gated on DrawnLineBuffer::IsDrawable.
	VkHardwareDataBuffer* drawnLineSSO = fb->GetBufferManager()->DrawnLineSSO ? fb->GetBufferManager()->DrawnLineSSO : fb->GetBufferManager()->BoneBufferSSO;

	// [PARTICLEDEFS] Binding 7, the same arrangement: always written, the bone
	// buffer standing in if the definitions buffer is somehow absent -- only the
	// gpuparticles effect reads it, and its draw is gated on the real buffer
	// (HWDrawInfo::RenderTranslucent).
	VkHardwareDataBuffer* particleDefinitionSSO = fb->GetBufferManager()->ParticleDefinitionSSO ? fb->GetBufferManager()->ParticleDefinitionSSO : fb->GetBufferManager()->BoneBufferSSO;

	WriteDescriptors()
		.AddBuffer(HWBufferSet.get(), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, fb->GetBufferManager()->ViewpointUBO->mBuffer.get(), 0, viewpointRange)
		.AddBuffer(HWBufferSet.get(), 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, fb->GetBufferManager()->MatrixBuffer->UniformBuffer->mBuffer.get(), 0, sizeof(MatricesUBO))
		.AddBuffer(HWBufferSet.get(), 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, fb->GetBufferManager()->StreamBuffer->UniformBuffer->mBuffer.get(), 0, sizeof(StreamUBO))
		.AddBuffer(HWBufferSet.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetBufferManager()->LightBufferSSO->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetBufferManager()->BoneBufferSSO->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, gpuParticleSSO->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, drawnLineSSO->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, particleDefinitionSSO->mBuffer.get())
		.Execute(fb->device.get());
}

void VkDescriptorSetManager::UpdateFixedSet()
{
	fb->GetCommands()->DrawDeleteList->Add(std::move(FixedSet));

	FixedSet = FixedDescriptorPool->tryAllocate(FixedSetLayout.get());
	if (!FixedSet)
	{
		fb->GetCommands()->WaitForCommands(false);
		FixedSet = FixedDescriptorPool->allocate(FixedSetLayout.get());
	}

	WriteDescriptors update;
	update.AddCombinedImageSampler(FixedSet.get(), 0, fb->GetTextureManager()->Shadowmap.View.get(), fb->GetSamplerManager()->ShadowmapSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	update.AddCombinedImageSampler(FixedSet.get(), 1, fb->GetTextureManager()->Lightmap.View.get(), fb->GetSamplerManager()->LightmapSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (fb->RaytracingEnabled())
		update.AddAccelerationStructure(FixedSet.get(), 2, fb->GetRaytrace()->GetAccelStruct());

	// [2c] Binding 4: the particle atlas (VkTextureManager::ParticleAtlas), read by
	// gpuparticles.fp. Always written: the texture manager creates a 1 x 1
	// placeholder when it is constructed, and rebuilds the atlas in its BeginFrame,
	// which runs before this. Linear, mips, clamp (VkSamplerManager::ParticleAtlasSampler).
	update.AddCombinedImageSampler(FixedSet.get(), 4, fb->GetTextureManager()->ParticleAtlas.View.get(), fb->GetSamplerManager()->ParticleAtlasSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// [2a] Binding 3: the scene depth, depth aspect only, for effects drawn inside
	// a read-only depth pass (FRenderState::SetSceneDepthReadable). Written with
	// DEPTH_STENCIL_READ_ONLY_OPTIMAL, the layout the image has only inside such a
	// pass (VkRenderPassSetup::CreateRenderPass with DepthReadOnly). Everywhere
	// else no bound pipeline declares binding 3 (only the scene-depth variants in
	// vk_shader.cpp do), so its layout is never checked against the writable one.
	//
	// Here because this runs every frame right after VkRenderBuffers::BeginFrame
	// has (re)built the scene images, so the view is always this frame's. Left
	// unwritten before the scene images exist; IsSceneDepthReadTarget then refuses
	// every target, so nothing that reads it is ever drawn.
	SceneDepthReadTexture = nullptr;
	SceneDepthReadImage = nullptr;
	SceneDepthReadLayered = false;
	if (VkRenderBuffers* buffers = fb->GetBuffers())
	{
		VkTextureImage& depth = buffers->SceneDepthStencil;
		if (depth.Image)
		{
			const bool layered = depth.Image->layerCount > 1;
			VulkanImageView* view = layered ? depth.DepthOnlyArrayView.get() : depth.DepthOnlyView.get();
			if (view)
			{
				// Nearest and clamped; the shaders texelFetch anyway, and a
				// multisampled image ignores the sampler.
				update.AddCombinedImageSampler(FixedSet.get(), 3, view, fb->GetSamplerManager()->Get(PPFilterMode::Nearest, PPWrapMode::Clamp), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
				SceneDepthReadTexture = &depth;
				SceneDepthReadImage = depth.Image.get();
				SceneDepthReadLayered = layered;
			}
		}
	}

	update.Execute(fb->device.get());
}

bool VkDescriptorSetManager::IsSceneDepthReadTarget(VulkanImageView* depthStencilView, int layers, uint32_t viewMask) const
{
	// [2a] See the declaration, and binding 3 in UpdateFixedSet above.
	const VkTextureImage* depth = SceneDepthReadTexture;
	if (!depthStencilView || !depth || !depth->Image || depth->Image.get() != SceneDepthReadImage)
		return false;

	if (SceneDepthReadLayered)
	{
		// Multiview into every layer at once: VulkanRenderDevice::SetSceneRenderTarget's
		// layered branch. Per-layer stereo into ONE layer of this image is refused --
		// the array view would also cover a layer that is not attached and is still
		// in the writable layout.
		return viewMask != 0 && layers == (int)depth->Image->layerCount && depthStencilView == depth->GetFramebufferView();
	}

	// One layer, no multiview: SetSceneRenderTarget's flat branch (GetLayerView(0),
	// which is View for a one-layer image).
	return viewMask == 0 && layers == 1 &&
		(depthStencilView == depth->GetLayerView(0) || depthStencilView == depth->GetFramebufferView());
}

void VkDescriptorSetManager::ResetHWTextureSets()
{
	for (auto mat : Materials)
		mat->DeleteDescriptors();

	auto deleteList = fb->GetCommands()->DrawDeleteList.get();
	for (auto& desc : TextureDescriptorPools)
	{
		deleteList->Add(std::move(desc));
	}
	deleteList->Add(std::move(NullTextureDescriptorSet));

	TextureDescriptorPools.clear();
	TextureDescriptorSetsLeft = 0;
	TextureDescriptorsLeft = 0;
}

VulkanDescriptorSet* VkDescriptorSetManager::GetNullTextureDescriptorSet()
{
	if (!NullTextureDescriptorSet)
	{
		NullTextureDescriptorSet = AllocateTextureDescriptorSet(SHADER_MIN_REQUIRED_TEXTURE_LAYERS);

		WriteDescriptors update;
		for (int i = 0; i < SHADER_MIN_REQUIRED_TEXTURE_LAYERS; i++)
		{
			update.AddCombinedImageSampler(NullTextureDescriptorSet.get(), i, fb->GetTextureManager()->GetNullTextureView(), fb->GetSamplerManager()->Get(CLAMP_XY_NOMIP), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		update.Execute(fb->device.get());
	}

	return NullTextureDescriptorSet.get();
}

std::unique_ptr<VulkanDescriptorSet> VkDescriptorSetManager::AllocateTextureDescriptorSet(int numLayers)
{
	if (TextureDescriptorSetsLeft == 0 || TextureDescriptorsLeft < numLayers)
	{
		TextureDescriptorSetsLeft = 1000;
		TextureDescriptorsLeft = 2000;

		TextureDescriptorPools.push_back(DescriptorPoolBuilder()
			.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TextureDescriptorsLeft)
			.MaxSets(TextureDescriptorSetsLeft)
			.DebugName("VkDescriptorSetManager.TextureDescriptorPool")
			.Create(fb->device.get()));
	}

	TextureDescriptorSetsLeft--;
	TextureDescriptorsLeft -= numLayers;
	return TextureDescriptorPools.back()->allocate(GetTextureSetLayout(numLayers));
}

VulkanDescriptorSetLayout* VkDescriptorSetManager::GetTextureSetLayout(int numLayers)
{
	if (TextureSetLayouts.size() < (size_t)numLayers)
		TextureSetLayouts.resize(numLayers);

	auto& layout = TextureSetLayouts[numLayers - 1];
	if (layout)
		return layout.get();

	DescriptorSetLayoutBuilder builder;
	for (int i = 0; i < numLayers; i++)
	{
		builder.AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	}
	builder.DebugName("VkDescriptorSetManager.TextureSetLayout");
	layout = builder.Create(fb->device.get());
	return layout.get();
}

void VkDescriptorSetManager::AddMaterial(VkMaterial* texture)
{
	texture->it = Materials.insert(Materials.end(), texture);
}

void VkDescriptorSetManager::RemoveMaterial(VkMaterial* texture)
{
	texture->DeleteDescriptors();
	texture->fb = nullptr;
	Materials.erase(texture->it);
}

VulkanDescriptorSet* VkDescriptorSetManager::GetInput(VkPPRenderPassSetup* passSetup, const TArray<PPTextureInput>& textures, bool bindShadowMapBuffers)
{
	auto descriptors = AllocatePPDescriptorSet(passSetup->DescriptorLayout.get());
	descriptors->SetDebugName("VkPostprocess.descriptors");

	WriteDescriptors write;
	VkImageTransition imageTransition;

	for (unsigned int index = 0; index < textures.Size(); index++)
	{
		const PPTextureInput& input = textures[index];
		VulkanSampler* sampler = fb->GetSamplerManager()->Get(input.Filter, input.Wrap);
		VkTextureImage* tex = fb->GetTextureManager()->GetTexture(input.Type, input.Texture);
		VulkanImageView* view = fb->GetTextureManager()->GetTextureView(input.Type, input.Texture, tex->DepthOnlyView != nullptr);

		write.AddCombinedImageSampler(descriptors.get(), index, view, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		imageTransition.AddImage(tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
	}

	// Bind automatic uniforms buffer
	write.AddBuffer(descriptors.get(), AUTOMATIC_UNIFORMS_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, fb->GetPostprocess()->GetAutomaticUniformsBuffer());

	if (bindShadowMapBuffers)
	{
		write.AddBuffer(descriptors.get(), LIGHTNODES_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetBufferManager()->LightNodes->mBuffer.get());
		write.AddBuffer(descriptors.get(), LIGHTLINES_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetBufferManager()->LightLines->mBuffer.get());
		write.AddBuffer(descriptors.get(), LIGHTLIST_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetBufferManager()->LightList->mBuffer.get());
	}

	write.Execute(fb->device.get());
	imageTransition.Execute(fb->GetCommands()->GetDrawCommands());

	VulkanDescriptorSet* set = descriptors.get();
	fb->GetCommands()->DrawDeleteList->Add(std::move(descriptors));
	return set;
}

std::unique_ptr<VulkanDescriptorSet> VkDescriptorSetManager::AllocatePPDescriptorSet(VulkanDescriptorSetLayout* layout)
{
	if (PPDescriptorPool)
	{
		auto descriptors = PPDescriptorPool->tryAllocate(layout);
		if (descriptors)
			return descriptors;

		fb->GetCommands()->DrawDeleteList->Add(std::move(PPDescriptorPool));
	}

	PPDescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 200)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100)  // For automatic uniforms
		.MaxSets(100)
		.DebugName("PPDescriptorPool")
		.Create(fb->device.get());

	return PPDescriptorPool->allocate(layout);
}

void VkDescriptorSetManager::CreateHWBufferSetLayout()
{
	HWBufferSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)	// [GPUPARTICLES] GpuParticleSSO
		.AddBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)	// [DRAWNLINES] DrawnLineSSO
		// [PARTICLEDEFS] ParticleDefinitionSSO. Fragment too: stage 2c's flipbooks and
		// 2d's lit/soft read the definition per pixel, and the layout should not have
		// to change again then. Binding 8 is reserved for 2d's view light list.
		.AddBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("VkDescriptorSetManager.HWBufferSetLayout")
		.Create(fb->device.get());
}

void VkDescriptorSetManager::CreateFixedSetLayout()
{
	DescriptorSetLayoutBuilder builder;
	builder.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	builder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if (fb->RaytracingEnabled())
		builder.AddBinding(2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	// [2a] The scene depth, readable in a read-only depth pass (UpdateFixedSet).
	// In the layout of every pipeline, because every pipeline shares this set, but
	// declared in GLSL only by the effects' scene-depth variants -- so no other
	// pipeline ever uses it while the image is a writable attachment.
	builder.AddBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	// [2c] The particle atlas, a 2D array (UpdateFixedSet). Declared in GLSL only by
	// gpuparticles.fp; always written with a valid image, so every pipeline can carry
	// it in its layout. Binding 5 onward is reserved for later plans (see "Engine
	// docs/REVIEW_SMOKE_DEBRIS_DAMAGE.md", X1).
	builder.AddBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	builder.DebugName("VkDescriptorSetManager.FixedSetLayout");
	FixedSetLayout = builder.Create(fb->device.get());
}

void VkDescriptorSetManager::CreateHWBufferPool()
{
	HWBufferDescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * maxSets)
		// [GPUPARTICLES] 3, not 2: lights (binding 3), bones (4), particles (5).
		// [DRAWNLINES] 4: and drawn lines (6). Too few here fails set allocation.
		// [PARTICLEDEFS] 5: and particle definitions (7).
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * maxSets)
		.MaxSets(maxSets)
		.DebugName("VkDescriptorSetManager.HWBufferDescriptorPool")
		.Create(fb->device.get());
}

void VkDescriptorSetManager::CreateFixedSetPool()
{
	DescriptorPoolBuilder poolbuilder;
	// [2a] 3, not 2: shadowmap (binding 0), lightmap (1), scene depth (3).
	// [2c] 4: and the particle atlas (4). Too few here fails set allocation.
	poolbuilder.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * maxSets);
	if (fb->RaytracingEnabled())
		poolbuilder.AddPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 * maxSets);
	poolbuilder.MaxSets(maxSets);
	poolbuilder.DebugName("VkDescriptorSetManager.FixedDescriptorPool");
	FixedDescriptorPool = poolbuilder.Create(fb->device.get());
}
