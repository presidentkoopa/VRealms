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
#include "printf.h"	// [VIEWLIGHTS] the per-stage storage buffer warning

#include "vk_postprocess.h"
#include "vk_compute.h"		// [LEVELFIELD] the level field's views for fixed set bindings 5, 6 and 9
#include "vk_levelfield.h"
#include "vk_debrispool.h"		// [DEBRISPOOL] the debris pool's buffers for set 1 bindings 10 and 11
#include "hw_debrisframe.h"

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

	// [VIEWLIGHTS] Binding 8, the same arrangement: always written, the bone buffer
	// standing in if the view light buffer is somehow absent -- only gpuparticles.vp
	// reads it, and the particle draw is gated on the real buffer
	// (HWDrawInfo::RenderTranslucent).
	VkHardwareDataBuffer* viewLightSSO = fb->GetBufferManager()->ViewLightSSO ? fb->GetBufferManager()->ViewLightSSO : fb->GetBufferManager()->BoneBufferSSO;

	// [SECTORPLANES] Binding 12, the same arrangement: always written, the bone buffer
	// standing in if the sector plane buffer is somehow absent. No lump declares binding 12
	// yet; its first reader (the smoke mask's draw, the level field #8, damage #17) gates
	// on the real buffer (screen->mSectorPlanes).
	VkHardwareDataBuffer* sectorPlaneSSO = fb->GetBufferManager()->SectorPlaneSSO ? fb->GetBufferManager()->SectorPlaneSSO : fb->GetBufferManager()->BoneBufferSSO;

	// [MESHPARTICLES] Binding 9 (review X1), the same arrangement: always written, the bone
	// buffer standing in if the mesh particle buffer is somehow absent -- only meshparticles.vp
	// reads it, and the mesh draw is gated on the real buffer (MeshParticleBuffer::IsDrawable).
	VkHardwareDataBuffer* meshParticleSSO = fb->GetBufferManager()->MeshParticleSSO ? fb->GetBufferManager()->MeshParticleSSO : fb->GetBufferManager()->BoneBufferSSO;

	// [DEBRISPOOL] Bindings 10 and 11 (vk_debrispool.h): the debris pool's pieces, and its definitions with the pool mesh
	// instance list. Always written, the bone buffer standing in while the pool does not exist -- only gpuparticles.vp's and
	// meshparticles.vp's pool paths read them, and those draws are gated on DebrisPoolStatus().Bound, set here to what this
	// set holds. The pool is made and freed inside a frame's compute, after this runs: a pool made this frame is bound from
	// the next; one freed this frame stays alive on the delete list until this frame's commands are done.
	VkDebrisPool* debrisPool = fb->GetCompute() != nullptr ? fb->GetCompute()->GetDebrisPool() : nullptr;
	const bool debrisBound = debrisPool != nullptr && debrisPool->IsAllocated();
	VulkanBuffer* debrisPieces = debrisBound ? debrisPool->GetPieceBuffer() : fb->GetBufferManager()->BoneBufferSSO->mBuffer.get();
	VulkanBuffer* debrisDefinitions = debrisBound ? debrisPool->GetDefinitionBuffer() : fb->GetBufferManager()->BoneBufferSSO->mBuffer.get();
	DebrisPoolStatus().Bound = debrisBound;

	WriteDescriptors()
		.AddBuffer(HWBufferSet.get(), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, fb->GetBufferManager()->ViewpointUBO->mBuffer.get(), 0, viewpointRange)
		.AddBuffer(HWBufferSet.get(), 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, fb->GetBufferManager()->MatrixBuffer->UniformBuffer->mBuffer.get(), 0, sizeof(MatricesUBO))
		.AddBuffer(HWBufferSet.get(), 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, fb->GetBufferManager()->StreamBuffer->UniformBuffer->mBuffer.get(), 0, sizeof(StreamUBO))
		.AddBuffer(HWBufferSet.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetBufferManager()->LightBufferSSO->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetBufferManager()->BoneBufferSSO->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, gpuParticleSSO->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, drawnLineSSO->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, particleDefinitionSSO->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, viewLightSSO->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, meshParticleSSO->mBuffer.get())	// [MESHPARTICLES]
		.AddBuffer(HWBufferSet.get(), 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, debrisPieces)	// [DEBRISPOOL]
		.AddBuffer(HWBufferSet.get(), 11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, debrisDefinitions)	// [DEBRISPOOL]
		.AddBuffer(HWBufferSet.get(), 12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sectorPlaneSSO->mBuffer.get())	// [SECTORPLANES]
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

	// [LEVELFIELD] Bindings 5 and 6: the level collision field's fine and coarse volumes; binding 9: its
	// header, the windows (vk_levelfield.h, "Engine docs/COLLISION_8_IMPL_NOTES.md"). Declared in GLSL
	// only by gpuparticles.vp's LEVEL_FIELD_COLLISION programs. Always written: while the field does not
	// exist, 1-texel stand-ins that say "no level" (a zero header, g = 0), so every particle keeps its
	// plane. Layout GENERAL -- the field is a storage image the compute bake writes, sampled in the layout
	// it lives in (review D6). The field is made and freed inside a frame's compute, after this runs: a
	// field made this frame is bound from the next; one freed this frame stays alive on the delete list
	// until this frame's commands are done.
	EnsureLevelFieldStandIns();
	VkLevelField* levelField = fb->GetCompute() != nullptr ? fb->GetCompute()->GetLevelField() : nullptr;
	const bool levelFieldBound = levelField != nullptr && levelField->IsAllocated();
	VulkanSampler* headerSampler = fb->GetSamplerManager()->Get(PPFilterMode::Nearest, PPWrapMode::Clamp);	// texelFetch only
	update.AddCombinedImageSampler(FixedSet.get(), 5, levelFieldBound ? levelField->GetFieldView(0) : LevelFieldStandInView.get(), LevelFieldSampler.get(), VK_IMAGE_LAYOUT_GENERAL);
	update.AddCombinedImageSampler(FixedSet.get(), 6, levelFieldBound ? levelField->GetFieldView(1) : LevelFieldStandInView.get(), LevelFieldSampler.get(), VK_IMAGE_LAYOUT_GENERAL);
	update.AddCombinedImageSampler(FixedSet.get(), 9, levelFieldBound ? levelField->GetHeaderView() : LevelFieldHeaderStandInView.get(), headerSampler, VK_IMAGE_LAYOUT_GENERAL);

	update.Execute(fb->device.get());
}

// [LEVELFIELD] The stand-ins for fixed set bindings 5, 6 and 9, and the field's sampler, made the first
// time the fixed set is written (Init, before VkComputeManager exists). A volume of one RG16F texel and a
// 3 x 1 RGBA32F header, both cleared to zero -- g 0 is "not baked", a zero window is "no level" -- and in
// GENERAL, the layout the bindings are written with. The sampler: linear, repeating on all three axes
// (the field is toroidal: a texel is a world cell modulo the volume's size), no mips.
void VkDescriptorSetManager::EnsureLevelFieldStandIns()
{
	if (LevelFieldStandInView && LevelFieldHeaderStandInView && LevelFieldSampler)
		return;

	VulkanDevice* device = fb->device.get();
	LevelFieldStandIn = ImageBuilder()
		.Size3D(1, 1, 1)
		.Format(VK_FORMAT_R16G16_SFLOAT)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.DebugName("VkDescriptorSetManager.LevelFieldStandIn")
		.Create(device);
	LevelFieldStandInView = ImageViewBuilder()
		.Type(VK_IMAGE_VIEW_TYPE_3D)
		.Image(LevelFieldStandIn.get(), VK_FORMAT_R16G16_SFLOAT)
		.DebugName("VkDescriptorSetManager.LevelFieldStandInView")
		.Create(device);
	LevelFieldHeaderStandIn = ImageBuilder()
		.Size(LEVEL_FIELD_HEADER_TEXELS, 1)
		.Format(VK_FORMAT_R32G32B32A32_SFLOAT)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.DebugName("VkDescriptorSetManager.LevelFieldHeaderStandIn")
		.Create(device);
	LevelFieldHeaderStandInView = ImageViewBuilder()
		.Image(LevelFieldHeaderStandIn.get(), VK_FORMAT_R32G32B32A32_SFLOAT)
		.DebugName("VkDescriptorSetManager.LevelFieldHeaderStandInView")
		.Create(device);
	LevelFieldSampler = SamplerBuilder()
		.MagFilter(VK_FILTER_LINEAR)
		.MinFilter(VK_FILTER_LINEAR)
		.MipmapMode(VK_SAMPLER_MIPMAP_MODE_NEAREST)
		.AddressMode(VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT)
		.MaxLod(0.25f)
		.DebugName("VkDescriptorSetManager.LevelFieldSampler")
		.Create(device);

	VulkanCommandBuffer* cmd = fb->GetCommands()->GetTransferCommands();
	PipelineBarrier()
		.AddImage(LevelFieldStandIn.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT)
		.AddImage(LevelFieldHeaderStandIn.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT)
		.Execute(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkClearColorValue zero = {};
	VkImageSubresourceRange range = {};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;
	cmd->clearColorImage(LevelFieldStandIn->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
	cmd->clearColorImage(LevelFieldHeaderStandIn->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
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
	// [VIEWLIGHTS] This layout puts NINE storage buffers in the vertex stage -- bones
	// (4), particle ring (5), drawn lines (6), particle definitions (7), view lights
	// (8), [MESHPARTICLES] mesh particles (9), [DEBRISPOOL] debris pieces (10) and debris
	// definitions (11), [SECTORPLANES] sector planes (12) -- and every pipeline layout
	// carries it. Vulkan guarantees only 4 per stage (maxPerStageDescriptorStorageBuffers);
	// desktop GPUs report far more, and this fork targets desktop Vulkan. A device below 9
	// fails pipeline layout creation, so say plainly why before it does. (The fragment stage
	// has three: lights 3, particle definitions 7, sector planes 12.)
	{
		const uint32_t vertexStageStorageBuffers = 9;
		const uint32_t allowed = fb->device->PhysicalDevice.Properties.Properties.limits.maxPerStageDescriptorStorageBuffers;
		if (allowed < vertexStageStorageBuffers)
		{
			Printf(TEXTCOLOR_RED "Vulkan: this device allows %u storage buffers per shader stage, but the renderer's buffer set needs %u "
				"in the vertex stage (bones, particle ring, drawn lines, particle definitions, view lights, mesh particles, debris pieces, "
				"debris definitions, sector planes) -- pipeline layout creation is expected to fail on this device\n", (unsigned)allowed, (unsigned)vertexStageStorageBuffers);
		}
	}

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
		// to change again then.
		.AddBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		// [VIEWLIGHTS] ViewLightSSO, the dynamic lights in view (hw_viewlightbuffer.h).
		// Vertex only, by design: effects light themselves once per vertex, and mesh
		// particles index the same list per instance in their vertex shader ("Engine
		// docs/REVIEW_SMOKE_DEBRIS_DAMAGE.md" D4). Declared in GLSL by gpuparticles.vp
		// alone. Bindings 9-11 are allocated to later plans (review X1).
		.AddBinding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		// [MESHPARTICLES] MeshParticleSSO (hw_meshparticles.h): the definitions that draw as
		// meshes and, per definition, the ring slots of its live particles, read once per
		// instance by meshparticles.vp as meshInstanceSlots[gl_InstanceIndex] (review D3).
		// Vertex only. Declared in GLSL by meshparticles.vp alone, so no other shader changed.
		.AddBinding(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		// [DEBRISPOOL] 10: the debris pool's pieces (review X1's number), written by the step compute; 11: the debris
		// definitions and the pool mesh instance list (vk_debrispool.h). Vertex only: the pool paths of gpuparticles.vp and
		// meshparticles.vp read a piece once per vertex. X1 had given 11 to #17's damage hash (fragment); #17 takes the
		// next free number when it is built ("Engine docs/DEBRIS_9_IMPL_NOTES.md").
		.AddBinding(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		// [SECTORPLANES] SectorPlaneSSO, sectors' current floor and ceiling planes
		// (hw_sectorplanebuffer.h), polled by the renderer (hw_sectorplanes.h). Vertex and
		// fragment, per review X1: #8 reads it per vertex, #17 per pixel. No lump declares
		// it yet, so no scene shader changed when it was added.
		.AddBinding(12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
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
	// [LEVELFIELD] The level collision field (UpdateFixedSet): 5 fine, 6 coarse (review X1's numbers), and
	// 9 its header -- a binding the X1 table did not have: the windows travel with the field instead of in
	// the viewpoint block ("Engine docs/COLLISION_8_IMPL_NOTES.md", deviation 1). Vertex stage: declared in
	// GLSL only by gpuparticles.vp's LEVEL_FIELD_COLLISION programs. Always written with a valid image, so
	// every pipeline can carry them in its layout. 7 and 8 stay reserved for #17's damage pages.
	builder.AddBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT);
	builder.AddBinding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT);
	builder.AddBinding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT);
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
		// [VIEWLIGHTS] 6: and the view lights (8).
		// [SECTORPLANES] 7: and the sector planes (12).
		// [MESHPARTICLES] 8: and the mesh particles (9).
		// [DEBRISPOOL] 10: and the debris pieces (10) and debris definitions (11) -- the plan's number.
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10 * maxSets)
		.MaxSets(maxSets)
		.DebugName("VkDescriptorSetManager.HWBufferDescriptorPool")
		.Create(fb->device.get());
}

void VkDescriptorSetManager::CreateFixedSetPool()
{
	DescriptorPoolBuilder poolbuilder;
	// [2a] 3, not 2: shadowmap (binding 0), lightmap (1), scene depth (3).
	// [2c] 4: and the particle atlas (4). Too few here fails set allocation.
	// [LEVELFIELD] 7: and the level field's fine (5), coarse (6) and header (9).
	poolbuilder.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7 * maxSets);
	if (fb->RaytracingEnabled())
		poolbuilder.AddPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 * maxSets);
	poolbuilder.MaxSets(maxSets);
	poolbuilder.DebugName("VkDescriptorSetManager.FixedDescriptorPool");
	FixedDescriptorPool = poolbuilder.Create(fb->device.get());
}
