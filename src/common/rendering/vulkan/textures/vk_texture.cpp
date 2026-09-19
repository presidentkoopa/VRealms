/*
** vk_texture.cpp
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

#include "vk_texture.h"
#include "vk_hwtexture.h"
#include "vk_pptexture.h"
#include "vk_renderbuffers.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vulkan/renderer/vk_compute.h"		// [SMOKEVOLUME] the external images: the smoke volume's
#include "vulkan/renderer/vk_smokevolume.h"
#include "vulkan/renderer/vk_emissivevolumes.h"	// [EMISSIVEVOLUMES] the emissive volumes' list and noise
#include "hwrenderer/postprocessing/hw_postprocess.h"	// [SMOKEVOLUME] PPExternalImageFromToken
#include "hw_cvars.h"
#include "hw_particledefbuffer.h"	// [2c] the particle atlas layer list
#include "texturemanager.h"	// [2c] the atlas's pixels come from TexMan's textures
#include "bitmap.h"	// [2c] FBitmap, what FTexture::GetBgraBitmap returns
#include "image.h"	// [ATLASBC7] FImageSource::HasPremultipliedAlpha, and the stored levels VkCompressedPixels reads
#include "i_time.h"	// [2c] atlas build time, for the log
#include "printf.h"
#include <algorithm>
#include <cmath>
#include <cstring>

VkTextureManager::VkTextureManager(VulkanRenderDevice* fb) : fb(fb)
{
	CreateNullTexture();
	CreateShadowmap();
	CreateLightmap();
	CreateParticleAtlas();	// [2c] the placeholder: no definitions are handed over yet
	CreateCompressedParticleAtlas();	// [ATLASBC7] and the compressed atlas's
}

VkTextureManager::~VkTextureManager()
{
	while (!Textures.empty())
		RemoveTexture(Textures.back());
	while (!PPTextures.empty())
		RemovePPTexture(PPTextures.back());
}

void VkTextureManager::Deinit()
{
	while (!Textures.empty())
		RemoveTexture(Textures.back());
	while (!PPTextures.empty())
		RemovePPTexture(PPTextures.back());
}

void VkTextureManager::BeginFrame()
{
	if (!Shadowmap.Image || Shadowmap.Image->width != gl_shadowmap_quality)
	{
		Shadowmap.Reset(fb);
		CreateShadowmap();
	}

	// [2c] The particle atlas, rebuilt when the CPU table's layer list (handed over by
	// HWDrawInfo::ProcessScene) or the layer size changed since it was built.
	// Renderer-read every frame like the shadow map above, so r_gpuparticles_atlas_size
	// applies on the next frame, menu open or not. Runs before
	// VkDescriptorSetManager::BeginFrame, which writes fixed binding 4 with the result.
	const ParticleDefinitionBuffer *definitions = fb->mParticleDefinitions;
	const uint64_t atlasGeneration = definitions != nullptr ? definitions->GetAtlasGeneration() : 0;
	const bool atlasHasLayers = definitions != nullptr && definitions->GetAtlasLayers().Size() > 0;
	if (!ParticleAtlas.Image || atlasGeneration != ParticleAtlasBuiltGeneration ||
		(atlasHasLayers && GpuParticleAtlasLayerSize() != ParticleAtlasBuiltSize))
	{
		if (ParticleAtlas.Image && !atlasHasLayers && ParticleAtlasBuiltLayers == 0)
		{
			// Still no textured definition: the placeholder stays.
			ParticleAtlasBuiltGeneration = atlasGeneration;
		}
		else
		{
			ParticleAtlas.Reset(fb);
			CreateParticleAtlas();
		}
	}

	// [ATLASBC7] The compressed particle atlas, rebuilt when its list -- which carries its side -- changed since it was built. Only
	// the atlas whose list changed is rebuilt: a compressed setting never rebuilds the uncompressed atlas, nor the other way round
	// unless the layout moved flipbooks between them.
	const uint64_t compressedGeneration = definitions != nullptr ? definitions->GetCompressedAtlasGeneration() : 0;
	const bool compressedHasLayers = definitions != nullptr && definitions->GetCompressedAtlasLayers().Size() > 0;
	if (!ParticleAtlasCompressed.Image || compressedGeneration != ParticleAtlasCompressedBuiltGeneration)
	{
		if (ParticleAtlasCompressed.Image && !compressedHasLayers && ParticleAtlasCompressedBuiltLayers == 0)
		{
			// Still no compressed flipbook: the placeholder stays.
			ParticleAtlasCompressedBuiltGeneration = compressedGeneration;
		}
		else
		{
			ParticleAtlasCompressed.Reset(fb);
			CreateCompressedParticleAtlas();
		}
	}
}

void VkTextureManager::AddTexture(VkHardwareTexture* texture)
{
	texture->it = Textures.insert(Textures.end(), texture);
}

void VkTextureManager::RemoveTexture(VkHardwareTexture* texture)
{
	texture->Reset();
	texture->fb = nullptr;
	Textures.erase(texture->it);
}

void VkTextureManager::AddPPTexture(VkPPTexture* texture)
{
	texture->it = PPTextures.insert(PPTextures.end(), texture);
}

void VkTextureManager::RemovePPTexture(VkPPTexture* texture)
{
	texture->Reset();
	texture->fb = nullptr;
	PPTextures.erase(texture->it);
}

VkTextureImage* VkTextureManager::GetTexture(const PPTextureType& type, PPTexture* pptexture)
{
	if (type == PPTextureType::CurrentPipelineTexture || type == PPTextureType::NextPipelineTexture)
	{
		int idx = fb->GetPostprocess()->GetCurrentPipelineImage();
		if (type == PPTextureType::NextPipelineTexture)
			idx = fb->GetPostprocess()->GetNextPipelineImage();

		return &fb->GetBuffers()->PipelineImage[idx];
	}
	else if (type == PPTextureType::PPTexture)
	{
		auto vktex = GetVkTexture(pptexture);
		return &vktex->TexImage;
	}
	else if (type == PPTextureType::SceneColor)
	{
		return &fb->GetBuffers()->SceneColor;
	}
	else if (type == PPTextureType::SceneNormal)
	{
		return &fb->GetBuffers()->SceneNormal;
	}
	else if (type == PPTextureType::SceneFog)
	{
		return &fb->GetBuffers()->SceneFog;
	}
	else if (type == PPTextureType::SceneDepth)
	{
		return &fb->GetBuffers()->SceneDepthStencil;
	}
	else if (type == PPTextureType::SceneMask)
	{
		// [SCENEMASK] The scene's per-pixel tag attachment, read the way the scene depth is: the scene's
		// own image, at the scene's size and samples. Its Image is null until a loaded post-process shader
		// declares the mask, and PPSceneMask::PostInputValid() is what tells a pass whether to read it.
		return &fb->GetBuffers()->ScenePostMask;
	}
	else if (type == PPTextureType::ShadowMap)
	{
		return &Shadowmap;
	}
	else if (type == PPTextureType::SwapChain)
	{
		return nullptr;
	}
	else if (type == PPTextureType::LightMaskCurrent || type == PPTextureType::LightMaskNext)
	{
		// [LIGHTMASK] The light mask's pair (VkRenderBuffers::LightMaskImage). The second image is
		// made the first time a carry writes it; its Image stays null if that was refused.
		int idx = fb->GetPostprocess()->GetCurrentLightMaskImage();
		if (type == PPTextureType::LightMaskNext)
		{
			idx ^= 1;
			fb->GetBuffers()->CreateLightMaskCarry();
		}
		return &fb->GetBuffers()->LightMaskImage[idx];
	}
	else if (type == PPTextureType::ExternalImage)
	{
		// [SMOKEVOLUME] An image the backend owns (hw_postprocess.h, PPExternalImage), named by the token
		// the pass set. A post-process read binds it SHADER_READ_ONLY_OPTIMAL (VkDescriptorSetManager::
		// GetInput), so it is handed out only while its owner has it in that layout: the smoke volume puts
		// its images there at the end of a frame's compute when it has smoke to draw (VkSmokeVolume::Run).
		// Anything else resolves to an image with no Image, and VkPPRenderState::Draw skips that draw.
		static VkTextureImage notReady;
		VkSmokeVolume* smoke = fb->GetCompute() != nullptr ? fb->GetCompute()->GetSmokeVolume() : nullptr;
		VkTextureImage* image = nullptr;
		if (smoke != nullptr && smoke->IsAllocated())
		{
			switch (PPExternalImageFromToken(pptexture))
			{
			case PPExternalImage::SmokeDensityLatest: image = smoke->GetDensityHeatImage(0); break;
			case PPExternalImage::SmokeDensityPrevious: image = smoke->GetDensityHeatImage(1); break;
			case PPExternalImage::SmokeTileActive: image = smoke->GetTileActiveImage(); break;
			case PPExternalImage::SmokeLight: image = smoke->GetLightImage(); break;	// [13d] the light grid
			case PPExternalImage::SmokeLightDirection: image = smoke->GetLightDirectionImage(); break;
			case PPExternalImage::SmokeBeams: image = smoke->GetBeamListImage(); break;	// [13e] the beam list
			default: break;
			}
		}
		// [EMISSIVEVOLUMES] The emissive volumes' list and noise (vk_emissivevolumes.h), whether or not the smoke volume exists.
		if (image == nullptr)
		{
			VkEmissiveVolumes* volumes = fb->GetCompute() != nullptr ? fb->GetCompute()->GetEmissiveVolumes() : nullptr;
			const PPExternalImage which = PPExternalImageFromToken(pptexture);
			if (volumes != nullptr && which == PPExternalImage::EmissiveVolumeList)
				image = volumes->GetListImage();
			else if (volumes != nullptr && which == PPExternalImage::EmissiveNoise)
				image = volumes->GetNoiseImage();
		}
		if (image == nullptr || !image->Image || image->Layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			return &notReady;
		return image;
	}
	else
	{
		I_FatalError("VkPPRenderState::GetTexture not implemented yet for this texture type");
		return nullptr;
	}
}

VulkanImageView* VkTextureManager::GetTextureView(const PPTextureType& type, PPTexture* pptexture, bool depthOnly)
{
	VkTextureImage* tex = GetTexture(type, pptexture);
	if (!tex)
		return nullptr;

	if (fb->ShouldUseCurrentEyeLayer(type, tex))
	{
		const int layerIndex = fb->GetCurrentEyeLayer();
		return depthOnly ? tex->GetLayerDepthOnlyView(layerIndex) : tex->GetLayerView(layerIndex);
	}

	if (depthOnly && tex->DepthOnlyView)
		return tex->DepthOnlyView.get();

	return tex->View.get();
}

VkFormat VkTextureManager::GetTextureFormat(PPTexture* texture)
{
	return GetVkTexture(texture)->Format;
}

VkPPTexture* VkTextureManager::GetVkTexture(PPTexture* texture)
{
	if (!texture->Backend)
		texture->Backend = std::make_unique<VkPPTexture>(fb, texture);
	return static_cast<VkPPTexture*>(texture->Backend.get());
}

void VkTextureManager::CreateNullTexture()
{
	NullTexture = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Size(1, 1)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT)
		.DebugName("VkDescriptorSetManager.NullTexture")
		.Create(fb->device.get());

	NullTextureView = ImageViewBuilder()
		.Image(NullTexture.get(), VK_FORMAT_R8G8B8A8_UNORM)
		.DebugName("VkDescriptorSetManager.NullTextureView")
		.Create(fb->device.get());

	PipelineBarrier()
		.AddImage(NullTexture.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT)
		.Execute(fb->GetCommands()->GetTransferCommands(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void VkTextureManager::CreateShadowmap()
{
	Shadowmap.Image = ImageBuilder()
		.Size(gl_shadowmap_quality, 1024)
		.Format(VK_FORMAT_R32_SFLOAT)
		.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.DebugName("VkRenderBuffers.Shadowmap")
		.Create(fb->device.get());

	Shadowmap.View = ImageViewBuilder()
		.Image(Shadowmap.Image.get(), VK_FORMAT_R32_SFLOAT)
		.DebugName("VkRenderBuffers.ShadowmapView")
		.Create(fb->device.get());

	VkImageTransition()
		.AddImage(&Shadowmap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true)
		.Execute(fb->GetCommands()->GetDrawCommands());
}

void VkTextureManager::CreateLightmap()
{
	TArray<uint16_t> data;
	data.Push(0);
	data.Push(0);
	data.Push(0);
	data.Push(0x3c00); // half-float 1.0
	SetLightmap(1, 1, data);
}

void VkTextureManager::SetLightmap(int LMTextureSize, int LMTextureCount, const TArray<uint16_t>& LMTextureData)
{
	int w = LMTextureSize;
	int h = LMTextureSize;
	int count = LMTextureCount;
	int pixelsize = 8;

	Lightmap.Reset(fb);

	Lightmap.Image = ImageBuilder()
		.Size(w, h, 1, count)
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.DebugName("VkRenderBuffers.Lightmap")
		.Create(fb->device.get());

	Lightmap.View = ImageViewBuilder()
		.Type(VK_IMAGE_VIEW_TYPE_2D_ARRAY)
		.Image(Lightmap.Image.get(), VK_FORMAT_R16G16B16A16_SFLOAT)
		.DebugName("VkRenderBuffers.LightmapView")
		.Create(fb->device.get());

	auto cmdbuffer = fb->GetCommands()->GetTransferCommands();

	int totalSize = w * h * count * pixelsize;

	auto stagingBuffer = BufferBuilder()
		.Size(totalSize)
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("VkHardwareTexture.mStagingBuffer")
		.Create(fb->device.get());

	uint16_t one = 0x3c00; // half-float 1.0
	const uint16_t* src = LMTextureData.Data();
	uint16_t* data = (uint16_t*)stagingBuffer->Map(0, totalSize);
	for (int i = w * h * count; i > 0; i--)
	{
		*(data++) = *(src++);
		*(data++) = *(src++);
		*(data++) = *(src++);
		*(data++) = one;
	}
	stagingBuffer->Unmap();

	VkImageTransition()
		.AddImage(&Lightmap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true, 0, count)
		.Execute(cmdbuffer);

	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = count;
	region.imageExtent.depth = 1;
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	cmdbuffer->copyBufferToImage(stagingBuffer->buffer, Lightmap.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	VkImageTransition()
		.AddImage(&Lightmap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, 0, count)
		.Execute(cmdbuffer);

	fb->GetCommands()->TransferDeleteList->Add(std::move(stagingBuffer));
}

//==========================================================================
//
// [2c] THE PARTICLE ATLAS ("Engine docs/GPU_PARTICLES_STAGE2_PLAN.md" 2c)
//
// One B8G8R8A8 2D array -- FBitmap's byte order, as VkHardwareTexture uses -- of
// square layers, r_gpuparticles_atlas_size on a side, with every mip level. Each
// layer is one frame of some textured particle definition (the list is
// ParticleDefinitionBuffer::GetAtlasLayers, built by gamedata/particledefs.cpp), a
// flipbook's frames on consecutive layers. Pixels come from TexMan's textures via
// FTexture::GetBgraBitmap, are PREMULTIPLIED -- so linear filtering and the mip
// chain never bleed the colour of transparent pixels, and stage 2d's premultiplied
// blend can use a texel as it is -- and are resampled into the rectangle the list
// gives them. Only layers the list names are allocated; with none, the atlas is a
// transparent 1 x 1 placeholder, so fixed binding 4 always holds a valid array.
//
// [ATLASBC7] THE COMPRESSED PARTICLE ATLAS ("Engine docs/PARTICLE_ATLAS_COMPRESSED_IMPL_NOTES.md"), fixed binding 10: a second
// 2D array, BC7, for flipbooks stored as premultiplied BC7 DDS frames (ParticleDefinitionBuffer::GetCompressedAtlasLayers). A
// block cannot be resampled or premultiplied, so each layer is the frame's stored levels as they are, from the stored level
// whose side is the atlas's; the CPU table only lists frames for which that holds. Both atlases stage their pixels in batches of
// whole layers (UploadAtlasImage), so a 683 MiB compressed atlas never sits in host memory at once.
//
//==========================================================================

namespace
{
	// One source pixel a layer pixel reads, and how much.
	struct AtlasTap
	{
		int Index;
		float Weight;
	};

	// A separable tent filter along one axis: for each of `layerPixels`, the source
	// pixels 0..sourcePixels-1 it reads, where the source spans layer pixels
	// [start, start + length). The radius is one texel of whichever side is coarser,
	// so it magnifies smoothly and minifies without aliasing. Weights are normalised
	// over the whole tent, so a tent reaching past the source's edge reads
	// transparency there and the art's border fades within one texel.
	void AtlasTentTaps(int layerPixels, int sourcePixels, double start, double length, TArray<AtlasTap> &taps, TArray<unsigned> &firstTap)
	{
		taps.Clear();
		firstTap.Resize((unsigned)layerPixels + 1);
		const double scale = sourcePixels / std::max(length, 1e-6);	// source pixels per layer pixel
		const double radius = std::max(1.0, scale);
		for (int d = 0; d < layerPixels; d++)
		{
			firstTap[(unsigned)d] = taps.Size();
			const double centre = ((d + 0.5) - start) * scale;
			const int lo = (int)std::floor(centre - radius);
			const int hi = (int)std::ceil(centre + radius);
			double total = 0.0;
			for (int s = lo; s <= hi; s++)
			{
				const double weight = 1.0 - std::fabs(s + 0.5 - centre) / radius;
				if (weight <= 0.0) continue;
				total += weight;
				if (s >= 0 && s < sourcePixels)
				{
					AtlasTap tap = { s, (float)weight };
					taps.Push(tap);
				}
			}
			if (total > 0.0)
			{
				for (unsigned t = firstTap[(unsigned)d]; t < taps.Size(); t++)
					taps[t].Weight = (float)(taps[t].Weight / total);
			}
		}
		firstTap[(unsigned)layerPixels] = taps.Size();
	}

	// One layer's whole mip chain into `chain` (every level, largest first, B G R A,
	// premultiplied): the texture resampled into the layer's rectangle, transparent
	// around it, then each level the average of 2 x 2 texels of the one above (sides
	// are powers of two). False when the texture has no pixels; the layer stays
	// transparent.
	bool FillParticleAtlasLayer(const ParticleAtlasLayer &layer, int side, uint8_t *chain, size_t chainBytes)
	{
		memset(chain, 0, chainBytes);

		FGameTexture *gameTexture = TexMan.GetGameTexture(layer.Texture);
		FTexture *baseTexture = gameTexture != nullptr ? gameTexture->GetTexture() : nullptr;
		if (baseTexture == nullptr) return false;

		FBitmap bitmap = baseTexture->GetBgraBitmap(nullptr);
		const int width = bitmap.GetWidth();
		const int height = bitmap.GetHeight();
		const int pitch = bitmap.GetPitch();
		const uint8_t *pixels = bitmap.GetPixels();
		if (pixels == nullptr || width <= 0 || height <= 0) return false;

		// Premultiplied, 0..1. [ATLASBC7] A source stored premultiplied already -- a DDS whose DX10 alpha mode says so
		// (FImageSource::HasPremultipliedAlpha), such as a BC7 flipbook the compressed atlas did not take -- is taken as it is:
		// premultiplying it again would darken every soft edge. Every other source goes through exactly the lines it did.
		FImageSource *sourceImage = baseTexture->GetImage();
		const bool storedPremultiplied = sourceImage != nullptr && sourceImage->HasPremultipliedAlpha();
		TArray<float> source;
		source.Resize((unsigned)width * (unsigned)height * 4);
		for (int y = 0; y < height; y++)
		{
			const uint8_t *row = pixels + (size_t)y * (size_t)pitch;
			for (int x = 0; x < width; x++)
			{
				const float alpha = row[x * 4 + 3] / 255.f;
				float *out = &source[((unsigned)y * (unsigned)width + (unsigned)x) * 4];
				if (storedPremultiplied)
				{
					out[0] = row[x * 4 + 0] / 255.f;
					out[1] = row[x * 4 + 1] / 255.f;
					out[2] = row[x * 4 + 2] / 255.f;
				}
				else
				{
					out[0] = row[x * 4 + 0] / 255.f * alpha;
					out[1] = row[x * 4 + 1] / 255.f * alpha;
					out[2] = row[x * 4 + 2] / 255.f * alpha;
				}
				out[3] = alpha;
			}
		}

		TArray<AtlasTap> tapsX, tapsY;
		TArray<unsigned> firstX, firstY;
		AtlasTentTaps(side, width, (double)layer.Left * side, (double)layer.Width * side, tapsX, firstX);
		AtlasTentTaps(side, height, (double)layer.Top * side, (double)layer.Height * side, tapsY, firstY);

		// Across: the source's columns into the layer's, row by row.
		TArray<float> across;
		across.Resize((unsigned)side * (unsigned)height * 4);
		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < side; x++)
			{
				float sum[4] = { 0.f, 0.f, 0.f, 0.f };
				for (unsigned t = firstX[(unsigned)x]; t < firstX[(unsigned)x + 1]; t++)
				{
					const float *in = &source[((unsigned)y * (unsigned)width + (unsigned)tapsX[t].Index) * 4];
					for (int c = 0; c < 4; c++) sum[c] += in[c] * tapsX[t].Weight;
				}
				memcpy(&across[((unsigned)y * (unsigned)side + (unsigned)x) * 4], sum, sizeof(sum));
			}
		}

		// Down: the rows into the layer's, as level 0. A colour channel is never above
		// alpha, so the texels stay valid premultiplied values after rounding.
		for (int y = 0; y < side; y++)
		{
			for (int x = 0; x < side; x++)
			{
				float sum[4] = { 0.f, 0.f, 0.f, 0.f };
				for (unsigned t = firstY[(unsigned)y]; t < firstY[(unsigned)y + 1]; t++)
				{
					const float *in = &across[((unsigned)tapsY[t].Index * (unsigned)side + (unsigned)x) * 4];
					for (int c = 0; c < 4; c++) sum[c] += in[c] * tapsY[t].Weight;
				}
				uint8_t *out = chain + ((size_t)y * (size_t)side + (size_t)x) * 4;
				const int alpha = std::clamp((int)std::lround(sum[3] * 255.f), 0, 255);
				out[3] = (uint8_t)alpha;
				for (int c = 0; c < 3; c++)
					out[c] = (uint8_t)std::clamp((int)std::lround(sum[c] * 255.f), 0, alpha);
			}
		}

		// The mip chain. Averages of premultiplied texels stay premultiplied.
		uint8_t *level = chain;
		int levelSide = side;
		while (levelSide > 1)
		{
			const int nextSide = levelSide >> 1;
			uint8_t *next = level + (size_t)levelSide * (size_t)levelSide * 4;
			for (int y = 0; y < nextSide; y++)
			{
				for (int x = 0; x < nextSide; x++)
				{
					const uint8_t *a = level + ((size_t)(y * 2) * (size_t)levelSide + (size_t)(x * 2)) * 4;
					const uint8_t *b = a + 4;
					const uint8_t *c = a + (size_t)levelSide * 4;
					const uint8_t *d = c + 4;
					uint8_t *out = next + ((size_t)y * (size_t)nextSide + (size_t)x) * 4;
					for (int ch = 0; ch < 4; ch++)
						out[ch] = (uint8_t)((a[ch] + b[ch] + c[ch] + d[ch] + 2) >> 2);
				}
			}
			level = next;
			levelSide = nextSide;
		}
		return true;
	}

	// BEGIN ATLASBC7 HARNESS: upload
	// [ATLASBC7] THE UPLOAD PLAN BOTH ATLASES SHARE ("Engine docs/PARTICLE_ATLAS_COMPRESSED_IMPL_NOTES.md").
	//
	// Staging in batches: one staging buffer carries as many WHOLE layers as fit in kAtlasStagingBytes (at least one) -- level 0
	// of those layers, then level 1, and so on -- so one copy region per level covers the batch. Every level of every layer
	// lands where the single staging buffer before batching put it, and host memory never holds much more than one batch: a
	// 2048-layer compressed atlas at 512 is 683 MiB. An atlas under the batch size is one batch, the upload it always was.
	const size_t kAtlasStagingBytes = 64 * 1024 * 1024;
	const int kAtlasMaxLevels = 16;

	unsigned AtlasLayersPerBatch(size_t chainBytes, unsigned layerCount)
	{
		const size_t perBatch = chainBytes > 0 ? kAtlasStagingBytes / chainBytes : (size_t)layerCount;
		if (perBatch < 1) return layerCount > 0 ? 1 : 0;
		return perBatch < (size_t)layerCount ? (unsigned)perBatch : layerCount;
	}

	// A BC7 level `level` of an image `side` pixels on a side: (side >> level, never below 1) on a side, in 4 x 4 blocks of 16
	// bytes -- Vulkan's rule for a block-compressed level, whose extent may stop short of a whole block only at its edge, as
	// VkHardwareTexture's compressed upload plans it (vk_hwtexture.cpp, PlanCompressedUpload).
	size_t CompressedAtlasLevelBytes(int side, int level)
	{
		const int levelSide = std::max(side >> level, 1);
		const size_t blocks = (size_t)std::max((levelSide + 3) / 4, 1);
		return blocks * blocks * 16;
	}

	// Where stored level `level` starts in a square BC7 image's stored levels (top level first, no padding), `width` on a side.
	size_t CompressedStoredLevelOffset(int width, int level)
	{
		size_t offset = 0;
		for (int k = 0; k < level; k++)
			offset += CompressedAtlasLevelBytes(width, k);
		return offset;
	}
	// END ATLASBC7 HARNESS: upload

	// Creates `atlas` as a side x side x layerCount 2D array of `format` with `levels` mip
	// levels and uploads it. Level m of a layer is levelBytes[m] bytes; `fillLayer(i,
	// chain, chainBytes)` writes layer i's levels one after another. Returns the image's
	// texel bytes. [ATLASBC7] Staged in batches of whole layers (AtlasLayersPerBatch).
	template<class FillLayer>
	uint64_t UploadAtlasImage(VulkanRenderDevice *fb, VkTextureImage &atlas, VkFormat format, int side, int levels, const size_t *levelBytes,
		unsigned layerCount, const char *imageName, const char *viewName, FillLayer &&fillLayer)
	{
		size_t chainBytes = 0;
		for (int m = 0; m < levels; m++)
			chainBytes += levelBytes[m];

		atlas.Image = ImageBuilder()
			.Format(format)
			.Size(side, side, levels, (int)layerCount)
			.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			.DebugName(imageName)
			.Create(fb->device.get());

		atlas.View = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D_ARRAY)
			.Image(atlas.Image.get(), format)
			.DebugName(viewName)
			.Create(fb->device.get());

		// Every level of every layer in one barrier (VkImageTransition covers one layer).
		PipelineBarrier()
			.AddImage(atlas.Image.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, (int)layerCount)
			.Execute(fb->GetCommands()->GetTransferCommands(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		TArray<uint8_t> chain;
		chain.Resize((unsigned)chainBytes);
		const unsigned perBatch = AtlasLayersPerBatch(chainBytes, layerCount);
		for (unsigned first = 0; first < layerCount; first += perBatch)
		{
			const unsigned count = std::min(perBatch, layerCount - first);
			size_t levelOffset[kAtlasMaxLevels];
			size_t total = 0;
			for (int m = 0; m < levels; m++)
			{
				levelOffset[m] = total;
				total += levelBytes[m] * count;
			}

			auto stagingBuffer = BufferBuilder()
				.Size(total)
				.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
				.DebugName("VkTextureManager.ParticleAtlasStaging")
				.Create(fb->device.get());

			uint8_t *data = (uint8_t *)stagingBuffer->Map(0, total);
			for (unsigned i = 0; i < count; i++)
			{
				fillLayer(first + i, chain.Data(), chainBytes);
				size_t chainOffset = 0;
				for (int m = 0; m < levels; m++)
				{
					memcpy(data + levelOffset[m] + (size_t)i * levelBytes[m], chain.Data() + chainOffset, levelBytes[m]);
					chainOffset += levelBytes[m];
				}
			}
			stagingBuffer->Unmap();

			std::vector<VkBufferImageCopy> regions((size_t)levels);
			for (int m = 0; m < levels; m++)
			{
				VkBufferImageCopy &region = regions[(size_t)m];
				region = {};
				region.bufferOffset = levelOffset[m];
				region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				region.imageSubresource.mipLevel = (uint32_t)m;
				region.imageSubresource.baseArrayLayer = first;
				region.imageSubresource.layerCount = count;
				region.imageExtent.width = (uint32_t)std::max(side >> m, 1);
				region.imageExtent.height = (uint32_t)std::max(side >> m, 1);
				region.imageExtent.depth = 1;
			}
			fb->GetCommands()->GetTransferCommands()->copyBufferToImage(stagingBuffer->buffer, atlas.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)levels, regions.data());

			// As VkHardwareTexture::CreateTexture does: past 64 MB of queued uploads, let
			// them finish before going on.
			fb->GetCommands()->TransferDeleteList->Add(std::move(stagingBuffer));
			if (fb->GetCommands()->TransferDeleteList->TotalSize > 64 * 1024 * 1024)
				fb->GetCommands()->WaitForCommands(false, true);
		}

		PipelineBarrier()
			.AddImage(atlas.Image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, (int)layerCount)
			.Execute(fb->GetCommands()->GetTransferCommands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		atlas.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		return (uint64_t)chainBytes * layerCount;
	}

	// Creates `atlas` as a side x side x layerCount B8G8R8A8 array with every mip level and
	// uploads it; `fillLayer(i, chain, chainBytes)` writes layer i's whole chain.
	// Returns the image's texel bytes. The staging holds level 0 of every layer, then
	// level 1 of every layer, and so on, [ATLASBC7] a batch of layers at a time.
	template<class FillLayer>
	uint64_t CreateAtlasImage(VulkanRenderDevice *fb, VkTextureImage &atlas, int side, unsigned layerCount, FillLayer &&fillLayer,
		const char *imageName = "VkTextureManager.ParticleAtlas", const char *viewName = "VkTextureManager.ParticleAtlasView")
	{
		int mipLevels = 1;
		while ((side >> mipLevels) > 0) mipLevels++;

		size_t mipBytes[kAtlasMaxLevels];
		for (int m = 0; m < mipLevels; m++)
			mipBytes[m] = (size_t)(side >> m) * (size_t)(side >> m) * 4;

		return UploadAtlasImage(fb, atlas, VK_FORMAT_B8G8R8A8_UNORM, side, mipLevels, mipBytes, layerCount, imageName, viewName, fillLayer);
	}

	// [ATLASBC7] A BC7 block all sixteen texels of which decode to 0, 0, 0, 0: mode 6 (its mode bit, bit 6 of the first byte) with
	// every endpoint, p-bit and index 0. What a compressed layer holds where its frame could not be read, as a blank uncompressed
	// layer is transparent.
	const uint8_t kTransparentBC7Block[16] = { 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

	// [ATLASBC7] One compressed layer's levels into `chain`: the frame's stored levels from layer.StartLevel, as stored, `levels`
	// of them (the atlas's side down to 1 x 1). False, and the layer transparent, when the frame cannot be read as the list
	// promised -- not a BC7 DDS any more, another side, levels missing or short.
	bool FillCompressedAtlasLayer(const ParticleCompressedAtlasLayer &layer, int side, int levels, uint8_t *chain, size_t chainBytes)
	{
		for (size_t offset = 0; offset + sizeof(kTransparentBC7Block) <= chainBytes; offset += sizeof(kTransparentBC7Block))
			memcpy(chain + offset, kTransparentBC7Block, sizeof(kTransparentBC7Block));

		FGameTexture *gameTexture = TexMan.GetGameTexture(layer.Texture);
		FTexture *baseTexture = gameTexture != nullptr ? gameTexture->GetTexture() : nullptr;
		FImageSource *image = baseTexture != nullptr ? baseTexture->GetImage() : nullptr;
		if (image == nullptr || layer.StartLevel < 0 || layer.StartLevel > 15)
			return false;
		VkCompressedPixels pixels;
		if (!pixels.Read(image))
			return false;
		if (pixels.format != VK_FORMAT_BC7_UNORM_BLOCK || pixels.blockSize != 16 || pixels.width != pixels.height ||
			pixels.width != (side << layer.StartLevel) || pixels.storedMips < layer.StartLevel + levels)
			return false;

		size_t needed = 0;
		for (int m = 0; m < levels; m++)
			needed += CompressedAtlasLevelBytes(side, m);
		if (needed != chainBytes || CompressedStoredLevelOffset(pixels.width, layer.StartLevel) + needed > pixels.size)
			return false;

		size_t out = 0;
		for (int m = 0; m < levels; m++)
		{
			const size_t bytes = CompressedAtlasLevelBytes(side, m);
			memcpy(chain + out, pixels.data + CompressedStoredLevelOffset(pixels.width, layer.StartLevel + m), bytes);
			out += bytes;
		}
		return true;
	}
}

void VkTextureManager::CreateParticleAtlas()
{
	ParticleDefinitionBuffer *definitions = fb->mParticleDefinitions;
	const unsigned listed = definitions != nullptr ? definitions->GetAtlasLayers().Size() : 0;
	const unsigned layers = listed > ParticleDefinitionBuffer::ATLAS_LAYERS_MAX ? ParticleDefinitionBuffer::ATLAS_LAYERS_MAX : listed;
	const int layerSize = GpuParticleAtlasLayerSize();
	const bool hadAtlas = ParticleAtlasBuiltLayers > 0;

	// Recorded before building, so a build that fails is not retried every frame; the
	// next change of the list or of the size tries again.
	ParticleAtlasBuiltGeneration = definitions != nullptr ? definitions->GetAtlasGeneration() : 0;
	ParticleAtlasBuiltSize = layers > 0 ? layerSize : 0;
	ParticleAtlasBuiltLayers = 0;

	if (layers > 0)
	{
		try
		{
			const uint64_t startMs = I_msTime();
			const TArray<ParticleAtlasLayer> &list = definitions->GetAtlasLayers();
			unsigned blank = 0;
			const uint64_t bytes = CreateAtlasImage(fb, ParticleAtlas, layerSize, layers,
				[&](unsigned i, uint8_t *chain, size_t chainBytes)
				{
					if (!FillParticleAtlasLayer(list[i], layerSize, chain, chainBytes)) blank++;
				});

			ParticleAtlasBuiltLayers = layers;
			definitions->SetAtlasBuilt(layers, layerSize, bytes);
			Printf("ParticleAtlas: %u layer%s of %d x %d with mips -- %.2f MiB of VRAM, built in %llu ms",
				layers, layers == 1 ? "" : "s", layerSize, layerSize, bytes / (1024.0 * 1024.0),
				(unsigned long long)(I_msTime() - startMs));
			if (blank > 0)
				Printf(TEXTCOLOR_ORANGE " -- %u layer%s had no pixels and stay%s transparent", blank, blank == 1 ? "" : "s", blank == 1 ? "s" : "");
			Printf("\n");
			return;
		}
		catch (const std::exception &err)
		{
			Printf(TEXTCOLOR_RED "ParticleAtlas: could not build %u layers of %d x %d -- textured particles draw nothing on this machine until the definitions or r_gpuparticles_atlas_size change:\n%s\n",
				layers, layerSize, layerSize, err.what());
			ParticleAtlas.Reset(fb);
		}
	}

	// The placeholder: one transparent 1 x 1 layer. A textured definition's layer
	// index is clamped to it when sampled, so such a particle draws nothing.
	CreateAtlasImage(fb, ParticleAtlas, 1, 1, [](unsigned, uint8_t *chain, size_t chainBytes) { memset(chain, 0, chainBytes); });
	if (definitions != nullptr)
		definitions->SetAtlasBuilt(0, 0, 0);
	if (hadAtlas)
		Printf("ParticleAtlas: no textured particle definitions -- atlas released, 1 x 1 placeholder\n");
}

void VkTextureManager::CreateCompressedParticleAtlas()
{
	// [ATLASBC7] See the atlas section's comment above.
	ParticleDefinitionBuffer *definitions = fb->mParticleDefinitions;
	const unsigned listed = definitions != nullptr ? definitions->GetCompressedAtlasLayers().Size() : 0;
	const unsigned layers = listed > ParticleDefinitionBuffer::ATLAS_LAYERS_MAX ? ParticleDefinitionBuffer::ATLAS_LAYERS_MAX : listed;
	const int side = definitions != nullptr ? definitions->GetCompressedAtlasSide() : 0;
	const bool hadAtlas = ParticleAtlasCompressedBuiltLayers > 0;

	// Recorded before building, so a build that fails is not retried every frame; the next change of the list (or of its side,
	// which moves its generation) tries again.
	ParticleAtlasCompressedBuiltGeneration = definitions != nullptr ? definitions->GetCompressedAtlasGeneration() : 0;
	ParticleAtlasCompressedBuiltLayers = 0;

	int levels = 0;
	while (side > 0 && levels < 31 && (side >> levels) > 0) levels++;
	const bool sideValid = side >= 4 && (side & (side - 1)) == 0 && levels <= kAtlasMaxLevels;
	if (layers > 0 && !sideValid)
	{
		Printf(TEXTCOLOR_RED "ParticleAtlas: the compressed list's side %d is not a power of two from 4 to %d -- compressed flipbooks draw nothing on this machine\n",
			side, 1 << (kAtlasMaxLevels - 1));
	}

	if (layers > 0 && sideValid)
	{
		try
		{
			const uint64_t startMs = I_msTime();
			const TArray<ParticleCompressedAtlasLayer> &list = definitions->GetCompressedAtlasLayers();
			size_t levelBytes[kAtlasMaxLevels];
			for (int m = 0; m < levels; m++)
				levelBytes[m] = CompressedAtlasLevelBytes(side, m);
			unsigned blank = 0;
			const uint64_t bytes = UploadAtlasImage(fb, ParticleAtlasCompressed, VK_FORMAT_BC7_UNORM_BLOCK, side, levels, levelBytes, layers,
				"VkTextureManager.ParticleAtlasCompressed", "VkTextureManager.ParticleAtlasCompressedView",
				[&](unsigned i, uint8_t *chain, size_t chainBytes)
				{
					if (!FillCompressedAtlasLayer(list[i], side, levels, chain, chainBytes)) blank++;
				});

			ParticleAtlasCompressedBuiltLayers = layers;
			definitions->SetCompressedAtlasBuilt(layers, side, bytes);
			Printf("ParticleAtlas: compressed (BC7) %u layer%s of %d x %d with mips -- %.2f MiB of VRAM, built in %llu ms",
				layers, layers == 1 ? "" : "s", side, side, bytes / (1024.0 * 1024.0), (unsigned long long)(I_msTime() - startMs));
			if (blank > 0)
				Printf(TEXTCOLOR_ORANGE " -- %u frame%s could not be read as listed and stay%s transparent", blank, blank == 1 ? "" : "s", blank == 1 ? "s" : "");
			Printf("\n");
			return;
		}
		catch (const std::exception &err)
		{
			Printf(TEXTCOLOR_RED "ParticleAtlas: could not build the compressed atlas, %u layers of %d x %d -- compressed flipbooks draw nothing on this machine until the definitions or the atlas settings change:\n%s\n",
				layers, side, side, err.what());
			ParticleAtlasCompressed.Reset(fb);
		}
	}

	// The placeholder: one transparent 1 x 1 B8G8R8A8 layer, so fixed binding 10 always holds a valid array. A compressed
	// definition's layer index is clamped to it when sampled, so such a particle draws nothing.
	CreateAtlasImage(fb, ParticleAtlasCompressed, 1, 1, [](unsigned, uint8_t *chain, size_t chainBytes) { memset(chain, 0, chainBytes); },
		"VkTextureManager.ParticleAtlasCompressed", "VkTextureManager.ParticleAtlasCompressedView");
	if (definitions != nullptr)
		definitions->SetCompressedAtlasBuilt(0, 0, 0);
	if (hadAtlas)
		Printf("ParticleAtlas: no compressed flipbooks -- compressed atlas released, 1 x 1 placeholder\n");
}
