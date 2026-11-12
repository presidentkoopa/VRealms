/*
** vk_hwtexture.h
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

#ifdef LoadImage
#undef LoadImage
#endif

#define SHADED_TEXTURE -1
#define DIRECT_PALETTE -2

#include "tarray.h"
#include "hw_ihwtexture.h"
#include <zvulkan/vulkanobjects.h>
#include "vk_imagetransition.h"
#include "hw_material.h"
#include <list>
#include <cstdlib>	// [DDS] free, VkCompressedPixels

struct FMaterialState;
class VulkanDescriptorSet;
class VulkanImage;
class VulkanImageView;
class VulkanBuffer;
class VulkanRenderDevice;
class FGameTexture;
class FImageSource;
class VulkanDevice;

// [DDS] An image's stored block-compressed levels (FImageSource::ReadCompressedPixels), top level
// first, as VkHardwareTexture::CreateCompressedTexture uploads them. Read on any thread.
struct VkCompressedPixels
{
	unsigned char* data = nullptr;	// malloc'd by ReadCompressedPixels
	size_t size = 0;				// every stored level
	size_t unitSize = 0;			// the top level
	int storedMips = 0;				// the file's mip count (0 or 1: the top level only)
	int width = 0;
	int height = 0;
	int blockSize = 0;				// bytes per 4x4 block: 8 for BC1, 16 for BC3 and BC7
	VkFormat format = VK_FORMAT_UNDEFINED;

	VkCompressedPixels() = default;
	VkCompressedPixels(const VkCompressedPixels&) = delete;
	VkCompressedPixels& operator=(const VkCompressedPixels&) = delete;
	~VkCompressedPixels() { free(data); }

	bool Read(FImageSource* src);
};

class VkHardwareTexture : public IHardwareTexture
{
	friend class VkMaterial;
	friend class VulkanRenderDevice;
public:
	VkHardwareTexture(VulkanRenderDevice* fb, int numchannels);
	~VkHardwareTexture();

	void Reset();

	// Software renderer stuff
	void AllocateBuffer(int w, int h, int texelsize) override;
	uint8_t *MapBuffer() override;
	unsigned int CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name) override;
	void BackgroundCreateTexture(VkCommandBufferManager* bufManager, int w, int h, int pixelsize, VkFormat format, const void *pixels, int numMipLevels, bool createMips, int totalSize = -1);
	void CheckFinalTransition(VulkanCommandBuffer* cmd, bool background);
	void ReleaseLoadedFromQueue(VulkanCommandBuffer* cmd, int fromQueueFamily, int toQueueFamily);
	void AcquireLoadedFromQueue(VulkanCommandBuffer* cmd, int fromQueueFamily, int toQueueFamily);

	// [DDS] Compressed DDS textures (GZSelaco 490044c411, a46c31630a, 1c5f0b120d, 5c93e38c6c).
	// CanUploadCompressed: this request may use the image's stored levels as they are -- no
	// translation, no sprite frame, not indexed, and the device has the format. Anything else
	// decodes on the CPU exactly as before.
	bool CanUploadCompressed(FTexture *tex, int translation, int flags);
	static bool DeviceSupportsCompressed(VulkanDevice *device, VkFormat format);
	// The stored levels into mLoadedImage (a background loader's image). Returns the level count; 0 = nothing was created.
	int BackgroundCreateCompressedTexture(VkCommandBufferManager* bufManager, const VkCompressedPixels& pixels, bool allowQualityReduction);
	// Selaco's name: one stored level into an image created with room for it, still in TRANSFER_DST.
	void CreateTextureMipMap(VkCommandBufferManager* bufManager, VkTextureImage* img, int mipLevel, int w, int h, int pixelsize, VkFormat format, const void* pixels, int totalSize);

	// Wipe screen
	void CreateWipeTexture(int w, int h, const char *name);

	VkTextureImage *GetImage(FTexture *tex, int translation, int flags);
	VkTextureImage *GetDepthStencil(FTexture *tex);

	VulkanRenderDevice* fb = nullptr;
	std::list<VkHardwareTexture*>::iterator it;

private:
	void CreateImage(FTexture *tex, int translation, int flags);
	bool CreateCompressedImage(FTexture *tex, int translation, int flags);	// [DDS] false: decode on the CPU
	int CreateCompressedTexture(VkCommandBufferManager *bufManager, VkTextureImage *img, const VkCompressedPixels &pixels, bool allowQualityReduction);	// [DDS]

	void CreateTexture(int w, int h, int pixelsize, VkFormat format, const void *pixels, bool mipmap);
	void CreateTexture(VkCommandBufferManager *bufManager, VkTextureImage *img, int w, int h, int pixelsize, VkFormat format, const void *pixels, int mipmap, bool generateMipmaps = true, int totalSize = -1);
	void SwapToLoadedImage();
	static int GetMipLevels(int w, int h);

	VkTextureImage mImage;
	VkTextureImage mLoadedImage;
	int mTexelsize = 4;

	VkTextureImage mDepthStencil;

	uint8_t* mappedSWFB = nullptr;
};

class VkMaterial : public FMaterial
{
public:
	VkMaterial(VulkanRenderDevice* fb, FGameTexture* tex, int scaleflags);
	~VkMaterial();

	VulkanDescriptorSet* GetDescriptorSet(const FMaterialState& state);

	void DeleteDescriptors() override;

	VulkanRenderDevice* fb = nullptr;
	std::list<VkMaterial*>::iterator it;

private:
	struct DescriptorEntry
	{
		int clampmode;
		intptr_t remap;
		std::unique_ptr<VulkanDescriptorSet> descriptor;

		DescriptorEntry(int cm, intptr_t f, std::unique_ptr<VulkanDescriptorSet>&& d)
		{
			clampmode = cm;
			remap = f;
			descriptor = std::move(d);
		}
	};

	std::vector<DescriptorEntry> mDescriptorSets;
};
