/*
** vk_emissivevolumes.h
**
** [EMISSIVEVOLUMES] The Vulkan side of emissive volumes (#15): the list image and the noise volume the drawing reads.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/VOLUMETRIC_FLASH_15_PLAN.md" 2d; "Engine docs/EMISSIVE_VOLUMES_15_IMPL_NOTES.md". A client of the compute
** manager (vk_compute.h) with no compute program: it owns two images a post-process pass reads through
** PPTextureType::ExternalImage (PPExternalImage::EmissiveVolumeList and EmissiveNoise; VkTextureManager::GetTexture):
**
**   - the LIST: EMISSIVE_VOLUMES_DRAWN_MAX x EMISSIVE_VOLUME_TEXELS RGBA32F (hw_emissivevolumeframe.h), copied in through
**     staging only when the frame's texels changed, kept SHADER_READ_ONLY_OPTIMAL -- the 13e beam list's method;
**   - the NOISE: EMISSIVE_NOISE_SIZE^3 RG8 (RGBA8 where the device refuses RG8 for a linear-filtered 3D image), baked once
**     on the CPU (EmissiveVolumeCore::BakeNoise) and copied in on first use.
**
** Made the first frame a volume is drawn (VkComputeManager::RunFrame), kept for the session (0.53 MB). Reports
** EmissiveVolumesStatus() every frame it runs, which SetupEmissiveVolumes (hw_drawinfo.cpp) reads before publishing a pass:
** a draw that names an image not ready would be skipped by VkPPRenderState::Draw, so nothing is published then.
**
** Presentation only: nothing is read back.
**
*/

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <zvulkan/vulkanobjects.h>
#include "hw_emissivevolumeframe.h"
#include "vulkan/textures/vk_imagetransition.h"	// VkTextureImage: a post-process pass binds the images

class VulkanRenderDevice;
class VkComputeManager;

class VkEmissiveVolumes
{
public:
	explicit VkEmissiveVolumes(VkComputeManager* compute);
	~VkEmissiveVolumes();

	// One frame: the noise baked the first time, the list copied in when it changed, the status written.
	void Run(const EmissiveVolumeFrame& frame);

	// For the post-process external image resolve (VkTextureManager::GetTexture). Handed out only while in
	// SHADER_READ_ONLY_OPTIMAL (the resolve checks the layout).
	VkTextureImage* GetListImage() { return &mList; }
	VkTextureImage* GetNoiseImage() { return &mNoise; }

private:
	bool EnsureList();
	bool EnsureNoise();
	void Refuse(const char* what);

	VkComputeManager* mCompute = nullptr;
	VulkanRenderDevice* fb = nullptr;

	VkTextureImage mList;
	std::unique_ptr<VulkanBuffer> mListStaging;
	std::vector<float> mListTexels;		// what the list image holds
	uint64_t mListSerial = 0;

	VkTextureImage mNoise;
	bool mRefused = false;
};
