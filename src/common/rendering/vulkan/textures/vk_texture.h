/*
** vk_texture.h
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

#include <zvulkan/vulkanobjects.h>
#include "vulkan/textures/vk_imagetransition.h"
#include <list>

class VulkanRenderDevice;
class VkHardwareTexture;
class VkMaterial;
class VkPPTexture;
class VkTextureImage;
enum class PPTextureType;
class PPTexture;

class VkTextureManager
{
public:
	VkTextureManager(VulkanRenderDevice* fb);
	~VkTextureManager();

	void Deinit();

	void BeginFrame();

	void SetLightmap(int LMTextureSize, int LMTextureCount, const TArray<uint16_t>& LMTextureData);

	VkTextureImage* GetTexture(const PPTextureType& type, PPTexture* tex);
	VulkanImageView* GetTextureView(const PPTextureType& type, PPTexture* tex, bool depthOnly);
	VkFormat GetTextureFormat(PPTexture* texture);

	void AddTexture(VkHardwareTexture* texture);
	void RemoveTexture(VkHardwareTexture* texture);

	void AddPPTexture(VkPPTexture* texture);
	void RemovePPTexture(VkPPTexture* texture);

	VulkanImage* GetNullTexture() { return NullTexture.get(); }
	VulkanImageView* GetNullTextureView() { return NullTextureView.get(); }

	VkTextureImage Shadowmap;
	VkTextureImage Lightmap;

	// [2c] The particle atlas ("Engine docs/GPU_PARTICLES_STAGE2_PLAN.md" 2c): every
	// frame some textured particle definition uses, one square layer per frame, a
	// flipbook's frames on consecutive layers, premultiplied alpha with mips. Fixed set
	// binding 4, read by gpuparticles.fp. A 1 x 1 transparent placeholder until a
	// loaded definition names a texture, so the binding is always valid.
	VkTextureImage ParticleAtlas;
	// [ATLASBC7] The COMPRESSED particle atlas ("Engine docs/PARTICLE_ATLAS_COMPRESSED_IMPL_NOTES.md"): flipbooks stored as
	// premultiplied BC7 DDS frames, one layer each, uploaded as stored -- BC7, every layer one side, the files' own mips. Fixed
	// set binding 10, read by gpuparticles.fp for a definition with PDF_ATLAS_COMPRESSED. A 1 x 1 transparent placeholder while
	// no flipbook is compressed, so the binding is always valid.
	VkTextureImage ParticleAtlasCompressed;

private:
	void CreateNullTexture();
	void CreateShadowmap();
	void CreateLightmap();
	// [2c] Builds ParticleAtlas from ParticleDefinitionBuffer's layer list at
	// r_gpuparticles_atlas_size (or the placeholder). BeginFrame calls it again when the
	// list or the size changes.
	void CreateParticleAtlas();
	// [ATLASBC7] Builds ParticleAtlasCompressed from ParticleDefinitionBuffer's compressed list, at the side that list gives (or
	// the placeholder). BeginFrame calls it again when that list or side changes.
	void CreateCompressedParticleAtlas();

	// [2c] What ParticleAtlas was last built from, so BeginFrame rebuilds only on a change.
	uint64_t ParticleAtlasBuiltGeneration = 0;
	int ParticleAtlasBuiltSize = 0;		// layer side in pixels; 0 = the placeholder
	unsigned ParticleAtlasBuiltLayers = 0;	// 0 = the placeholder
	// [ATLASBC7] The same for ParticleAtlasCompressed. Its side comes with its list, so the list's generation covers it.
	uint64_t ParticleAtlasCompressedBuiltGeneration = 0;
	unsigned ParticleAtlasCompressedBuiltLayers = 0;	// 0 = the placeholder

	VkPPTexture* GetVkTexture(PPTexture* texture);

	VulkanRenderDevice* fb = nullptr;

	std::list<VkHardwareTexture*> Textures;
	std::list<VkPPTexture*> PPTextures;

	std::unique_ptr<VulkanImage> NullTexture;
	std::unique_ptr<VulkanImageView> NullTextureView;
};
