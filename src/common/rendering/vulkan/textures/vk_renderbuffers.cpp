/*
** vk_renderbuffers.cpp
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

#include "vk_renderbuffers.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/shaders/vk_shader.h"
#include <zvulkan/vulkanswapchain.h>
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "hw_cvars.h"

namespace
{
void CreateColorTargetViews(VulkanRenderDevice* fb, VkTextureImage& texture, VkFormat format, const char* viewName, const char* framebufferViewName)
{
	const int layers = texture.Image ? texture.Image->layerCount : 1;
	if (layers > 1)
	{
		texture.View = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D)
			.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 1)
			.DebugName(viewName)
			.Create(fb->device.get());

		texture.ArrayView = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D_ARRAY)
			.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_COLOR_BIT)
			.DebugName(framebufferViewName)
			.Create(fb->device.get());

		texture.FramebufferView = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D_ARRAY)
			.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_COLOR_BIT)
			.DebugName(framebufferViewName)
			.Create(fb->device.get());

		texture.LayerViews.resize(layers);
		for (int layer = 0; layer < layers; ++layer)
		{
			texture.LayerViews[layer] = ImageViewBuilder()
				.Type(VK_IMAGE_VIEW_TYPE_2D)
				.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 0, 1)
				.DebugName(viewName)
				.Create(fb->device.get());
		}
	}
	else
	{
		texture.View = ImageViewBuilder()
			.Image(texture.Image.get(), format)
			.DebugName(viewName)
			.Create(fb->device.get());
	}
}

void CreateDepthTargetViews(VulkanRenderDevice* fb, VkTextureImage& texture, VkFormat format, const char* viewName, const char* depthViewName, const char* framebufferViewName)
{
	const int layers = texture.Image ? texture.Image->layerCount : 1;
	texture.AspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

	if (layers > 1)
	{
		texture.View = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D)
			.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 0, 1)
			.DebugName(viewName)
			.Create(fb->device.get());

		texture.FramebufferView = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D_ARRAY)
			.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
			.DebugName(framebufferViewName)
			.Create(fb->device.get());

		texture.DepthOnlyView = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D)
			.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 0, 1)
			.DebugName(depthViewName)
			.Create(fb->device.get());

		// [2a] Every layer, depth aspect only, for a layered scene-depth read
		// (sampler2DArray / sampler2DMSArray in an effect's scene-depth variant).
		// Same full-range call as FramebufferView above, minus the stencil aspect,
		// which a sampled view may not include alongside depth.
		texture.DepthOnlyArrayView = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D_ARRAY)
			.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT)
			.DebugName(depthViewName)
			.Create(fb->device.get());

		texture.LayerViews.resize(layers);
		texture.LayerDepthOnlyViews.resize(layers);
		for (int layer = 0; layer < layers; ++layer)
		{
			texture.LayerViews[layer] = ImageViewBuilder()
				.Type(VK_IMAGE_VIEW_TYPE_2D)
				.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, layer, 0, 1)
				.DebugName(viewName)
				.Create(fb->device.get());

			texture.LayerDepthOnlyViews[layer] = ImageViewBuilder()
				.Type(VK_IMAGE_VIEW_TYPE_2D)
				.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 0, 1)
				.DebugName(depthViewName)
				.Create(fb->device.get());
		}
	}
	else
	{
		texture.View = ImageViewBuilder()
			.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
			.DebugName(viewName)
			.Create(fb->device.get());

		texture.DepthOnlyView = ImageViewBuilder()
			.Image(texture.Image.get(), format, VK_IMAGE_ASPECT_DEPTH_BIT)
			.DebugName(depthViewName)
			.Create(fb->device.get());
	}
}
}

VkRenderBuffers::VkRenderBuffers(VulkanRenderDevice* fb) : fb(fb)
{
}

VkRenderBuffers::~VkRenderBuffers()
{
}

VkSampleCountFlagBits VkRenderBuffers::GetBestSampleCount()
{
	const auto &limits = fb->device->PhysicalDevice.Properties.Properties.limits;
	// The scene color/depth targets are rendered multisampled and later resolved.
	// Stencil sampling is not required here, and some runtimes report no sampled
	// multisample stencil support even though color/depth MSAA is available.
	VkSampleCountFlags deviceSampleCounts =
		limits.framebufferColorSampleCounts &
		limits.framebufferDepthSampleCounts &
		limits.framebufferStencilSampleCounts &
		limits.sampledImageColorSampleCounts &
		limits.sampledImageDepthSampleCounts;

	int requestedSamples = clamp((int)gl_multisample, 0, 64);

	int samples = 1;
	VkSampleCountFlags bit = VK_SAMPLE_COUNT_1_BIT;
	VkSampleCountFlags best = bit;
	while (samples <= requestedSamples)
	{
		if (deviceSampleCounts & bit)
		{
			best = bit;
		}
		samples <<= 1;
		bit <<= 1;
	}
	return (VkSampleCountFlagBits)best;
}

void VkRenderBuffers::BeginFrame(int width, int height, int sceneWidth, int sceneHeight, int sceneLayers, int pipelineLayers)
{
	VkSampleCountFlagBits samples = GetBestSampleCount();
	const int pipelineWidth = std::max(width, sceneWidth);
	const int pipelineHeight = std::max(height, sceneHeight);

	if (pipelineWidth != mWidth || pipelineHeight != mHeight || mSamples != samples || mPipelineLayers != pipelineLayers || mSceneLayers != sceneLayers)
	{
		fb->GetCommands()->WaitForCommands(false);
		fb->GetRenderPassManager()->RenderBuffersReset();
	}

	if (pipelineWidth != mWidth || pipelineHeight != mHeight || mPipelineLayers != pipelineLayers)
		CreatePipeline(pipelineWidth, pipelineHeight, pipelineLayers);

	if (sceneWidth != mSceneWidth || sceneHeight != mSceneHeight || mSamples != samples || mSceneLayers != sceneLayers)
		CreateScene(sceneWidth, sceneHeight, samples, sceneLayers);

	mWidth = pipelineWidth;
	mHeight = pipelineHeight;
	mSamples = samples;
	mSceneWidth = sceneWidth;
	mSceneHeight = sceneHeight;
	mSceneLayers = sceneLayers;
	mPipelineLayers = pipelineLayers;
}

void VkRenderBuffers::CreatePipelineDepthStencil(int width, int height, int layers)
{
	ImageBuilder builder;
	builder.Size(width, height, 1, layers);
	builder.Format(PipelineDepthStencilFormat);
	builder.Usage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	if (!builder.IsFormatSupported(fb->device.get()))
	{
		PipelineDepthStencilFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
		builder.Format(PipelineDepthStencilFormat);
		if (!builder.IsFormatSupported(fb->device.get()))
		{
			I_FatalError("This device does not support any of the required depth stencil image formats.");
		}
	}
	builder.DebugName("VkRenderBuffers.PipelineDepthStencil");

	PipelineDepthStencil.Image = builder.Create(fb->device.get());
	CreateDepthTargetViews(fb, PipelineDepthStencil, PipelineDepthStencilFormat,
		"VkRenderBuffers.PipelineDepthStencilView",
		"VkRenderBuffers.PipelineDepthView",
		"VkRenderBuffers.PipelineDepthStencilFramebufferView");
}

void VkRenderBuffers::CreatePipeline(int width, int height, int layers)
{
	for (int i = 0; i < NumPipelineImages; i++)
	{
		PipelineImage[i].Reset(fb);
	}
	PipelineDepthStencil.Reset(fb);

	CreatePipelineDepthStencil(width, height, layers);

	VkImageTransition barrier;
	barrier.AddImage(&PipelineDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true);
	for (int i = 0; i < NumPipelineImages; i++)
	{
		PipelineImage[i].Image = ImageBuilder()
			.Size(width, height, 1, layers)
			.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
			.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			.DebugName("VkRenderBuffers.PipelineImage")
			.Create(fb->device.get());

		CreateColorTargetViews(fb, PipelineImage[i], VK_FORMAT_R16G16B16A16_SFLOAT,
			"VkRenderBuffers.PipelineView",
			"VkRenderBuffers.PipelineFramebufferView");

		barrier.AddImage(&PipelineImage[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
	}
	barrier.Execute(fb->GetCommands()->GetDrawCommands());
}

void VkRenderBuffers::CreateScene(int width, int height, VkSampleCountFlagBits samples, int layers)
{
	SceneColor.Reset(fb);
	SceneDepthStencil.Reset(fb);
	SceneNormal.Reset(fb);
	SceneFog.Reset(fb);

	CreateSceneColor(width, height, samples, layers);
	CreateSceneDepthStencil(width, height, samples, layers);
	CreateSceneNormal(width, height, samples, layers);
	CreateSceneFog(width, height, samples, layers);

	VkImageTransition()
		.AddImage(&SceneColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true)
		.AddImage(&SceneDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true)
		.AddImage(&SceneNormal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true)
		.AddImage(&SceneFog, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true)
		.Execute(fb->GetCommands()->GetDrawCommands());
}

void VkRenderBuffers::CreateSceneColor(int width, int height, VkSampleCountFlagBits samples, int layers)
{
	SceneColor.Image = ImageBuilder()
		.Size(width, height, 1, layers)
		.Samples(samples)
		.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
		.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
		.DebugName("VkRenderBuffers.SceneColor")
		.Create(fb->device.get());

	CreateColorTargetViews(fb, SceneColor, VK_FORMAT_R16G16B16A16_SFLOAT,
		"VkRenderBuffers.SceneColorView",
		"VkRenderBuffers.SceneColorFramebufferView");
}

void VkRenderBuffers::CreateSceneDepthStencil(int width, int height, VkSampleCountFlagBits samples, int layers)
{
	ImageBuilder builder;
	builder.Size(width, height, 1, layers);
	builder.Samples(samples);
	builder.Format(SceneDepthStencilFormat);
	builder.Usage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	if (!builder.IsFormatSupported(fb->device.get()))
	{
		SceneDepthStencilFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
		builder.Format(SceneDepthStencilFormat);
		if (!builder.IsFormatSupported(fb->device.get()))
		{
			I_FatalError("This device does not support any of the required depth stencil image formats.");
		}
	}
	builder.DebugName("VkRenderBuffers.SceneDepthStencil");

	SceneDepthStencil.Image = builder.Create(fb->device.get());
	CreateDepthTargetViews(fb, SceneDepthStencil, SceneDepthStencilFormat,
		"VkRenderBuffers.SceneDepthStencilView",
		"VkRenderBuffers.SceneDepthView",
		"VkRenderBuffers.SceneDepthStencilFramebufferView");
}

void VkRenderBuffers::CreateSceneFog(int width, int height, VkSampleCountFlagBits samples, int layers)
{
	SceneFog.Image = ImageBuilder()
		.Size(width, height, 1, layers)
		.Samples(samples)
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
		.DebugName("VkRenderBuffers.SceneFog")
		.Create(fb->device.get());

	CreateColorTargetViews(fb, SceneFog, VK_FORMAT_R8G8B8A8_UNORM,
		"VkRenderBuffers.SceneFogView",
		"VkRenderBuffers.SceneFogFramebufferView");
}

void VkRenderBuffers::CreateSceneNormal(int width, int height, VkSampleCountFlagBits samples, int layers)
{
	ImageBuilder builder;
	builder.Size(width, height, 1, layers);
	builder.Samples(samples);
	builder.Format(SceneNormalFormat);
	builder.Usage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	if (!builder.IsFormatSupported(fb->device.get(), VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
	{
		SceneNormalFormat = VK_FORMAT_R8G8B8A8_UNORM;
		builder.Format(SceneNormalFormat);
	}
	builder.DebugName("VkRenderBuffers.SceneNormal");

	SceneNormal.Image = builder.Create(fb->device.get());

	CreateColorTargetViews(fb, SceneNormal, SceneNormalFormat,
		"VkRenderBuffers.SceneNormalView",
		"VkRenderBuffers.SceneNormalFramebufferView");
}

VulkanFramebuffer* VkRenderBuffers::GetOutput(VkPPRenderPassSetup* passSetup, const PPOutput& output, WhichDepthStencil stencilTest, int& framebufferWidth, int& framebufferHeight)
{
	VkTextureImage* tex = fb->GetTextureManager()->GetTexture(output.Type, output.Texture);

	VkImageView view;
	std::unique_ptr<VulkanFramebuffer>* framebufferptr = nullptr;
	int w, h;
	if (tex)
	{
		const bool useLayerView = fb->ShouldUseCurrentEyeLayer(output.Type, tex);
		const int layerIndex = useLayerView ? fb->GetCurrentEyeLayer() : -1;
		VkImageTransition imageTransition;
		imageTransition.AddImage(tex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, output.Type == PPTextureType::NextPipelineTexture);
		if (stencilTest == WhichDepthStencil::Scene)
			imageTransition.AddImage(&fb->GetBuffers()->SceneDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false);

		if (stencilTest == WhichDepthStencil::Pipeline)
			imageTransition.AddImage(&fb->GetBuffers()->PipelineDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false);

		imageTransition.Execute(fb->GetCommands()->GetDrawCommands());

		view = useLayerView ? tex->GetLayerView(layerIndex)->view : tex->GetFramebufferView()->view;
		w = tex->Image->width;
		h = tex->Image->height;
		VkTextureImage::VkPPOutputFramebufferKey framebufferKey = {};
		framebufferKey.LayerIndex = layerIndex;
		framebufferKey.DepthStencilMode = (int)stencilTest;
		framebufferptr = &tex->PPOutputFramebuffers[framebufferKey];
	}
	else
	{
		view = fb->GetFramebufferManager()->SwapChain->GetImageView(fb->GetFramebufferManager()->PresentImageIndex)->view;
		framebufferptr = &fb->GetFramebufferManager()->Framebuffers[fb->GetFramebufferManager()->PresentImageIndex];
		w = fb->GetFramebufferManager()->SwapChain->Width();
		h = fb->GetFramebufferManager()->SwapChain->Height();
	}

	auto& framebuffer = *framebufferptr;
	if (!framebuffer)
	{
		FramebufferBuilder builder;
		builder.RenderPass(passSetup->RenderPass.get());
		builder.Size(w, h);
		builder.AddAttachment(view);
		if (stencilTest == WhichDepthStencil::Scene)
			builder.AddAttachment(fb->GetBuffers()->GetSceneLayers() > 1 ? fb->GetBuffers()->SceneDepthStencil.GetLayerView(fb->GetCurrentEyeLayer()) : fb->GetBuffers()->SceneDepthStencil.GetFramebufferView());
		if (stencilTest == WhichDepthStencil::Pipeline)
			builder.AddAttachment(fb->GetBuffers()->GetPipelineLayers() > 1 ? fb->GetBuffers()->PipelineDepthStencil.GetLayerView(fb->GetCurrentEyeLayer()) : fb->GetBuffers()->PipelineDepthStencil.GetFramebufferView());
		builder.DebugName("PPOutputFB");
		framebuffer = builder.Create(fb->device.get());
	}

	framebufferWidth = w;
	framebufferHeight = h;
	return framebuffer.get();
}
