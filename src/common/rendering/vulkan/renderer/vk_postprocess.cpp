/*
** vk_postprocess.cpp
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

#include "vk_postprocess.h"
#include "vulkan/shaders/vk_shader.h"
#include <zvulkan/vulkanswapchain.h>
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_hwbuffer.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_pprenderstate.h"
#include "vulkan/shaders/vk_ppshader.h"
#include "vulkan/textures/vk_pptexture.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_imagetransition.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "hw_vrmodes.h"
#include "common/rendering/stereo3d/openxr/oxr_loader.h"
#include "flatvertices.h"
#include "r_videoscale.h"
#include <algorithm>

#include "i_time.h"
#include "g_levellocals.h"

EXTERN_CVAR(Int, gl_dither_bpc)
EXTERN_CVAR(Float, vr_openxr_present_gamma_bias)
EXTERN_CVAR(Float, vr_openxr_present_contrast_bias)
EXTERN_CVAR(Float, vr_openxr_present_brightness_bias)
EXTERN_CVAR(Float, vr_openxr_present_saturation_bias)
EXTERN_CVAR(Bool, vr_openxr_multiview_postprocess)

VkPostprocess::VkPostprocess(VulkanRenderDevice* fb) : fb(fb)
{
	// Create buffer for automatic uniforms (12 bytes: 3 floats)
	AutomaticUniformsBuffer = BufferBuilder()
	.Size(16)  // 16 bytes (pad to alignment)
	.Usage(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
	.DebugName("AutomaticUniformsBuffer")
	.Create(fb->device.get());
}

VkPostprocess::~VkPostprocess()
{
}

int VkPostprocess::GetNextPipelineImage() const
{
	if (mPipelinePairSize <= 1)
	{
		return mCurrentPipelineImage;
	}

	const int localIndex = std::max(0, mCurrentPipelineImage - mPipelinePairStart);
	return mPipelinePairStart + ((localIndex + 1) % mPipelinePairSize);
}

void VkPostprocess::SetActiveRenderTarget()
{
	auto buffers = fb->GetBuffers();
	const int layerIndex = buffers->GetPipelineLayers() > 1 ? fb->GetCurrentEyeLayer() : 0;

	VkImageTransition()
		.AddImage(&buffers->PipelineImage[mCurrentPipelineImage], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
		.AddImage(&buffers->PipelineDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false)
		.Execute(fb->GetCommands()->GetDrawCommands());

	fb->GetRenderState()->SetRenderTarget(&buffers->PipelineImage[mCurrentPipelineImage], buffers->PipelineDepthStencil.GetLayerView(layerIndex), buffers->GetWidth(), buffers->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, 1, 0, layerIndex);
}

void VkPostprocess::PostProcessScene(int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	Clocker postprocessTimer(VRPostProcessScene);
	int sceneWidth = fb->GetBuffers()->GetSceneWidth();
	int sceneHeight = fb->GetBuffers()->GetSceneHeight();

	VkPPRenderState renderstate(fb);

	renderstate.TimeDelta = static_cast<float>(GetDeltaTime());
	renderstate.Time = static_cast<float>(fb->FrameTime / 1000.0);
	renderstate.TimeGame = static_cast<float>(primaryLevel->LocalWorldTimer / (double)GameTicRate);

	// Upload automatic uniforms to buffer
	struct AutomaticUniforms {
		float InputTimeDelta;
		float InputTime;
		float InputTimeGame;
		float padding;  // Align to 16 bytes
	} autoUniforms;

	autoUniforms.InputTimeDelta = renderstate.TimeDelta;
	autoUniforms.InputTime = renderstate.Time;
	autoUniforms.InputTimeGame = renderstate.TimeGame;
	autoUniforms.padding = 0.0f;

	void* data = AutomaticUniformsBuffer->Map(0, sizeof(autoUniforms));
	memcpy(data, &autoUniforms, sizeof(autoUniforms));
	AutomaticUniformsBuffer->Unmap();

	hw_postprocess.Pass1(&renderstate, fixedcm, sceneWidth, sceneHeight);
	SetActiveRenderTarget();
	afterBloomDrawEndScene2D();
	hw_postprocess.Pass2(&renderstate, fixedcm, flash, sceneWidth, sceneHeight);
}

void VkPostprocess::BlitSceneToPostprocess()
{
	auto buffers = fb->GetBuffers();
	auto cmdbuffer = fb->GetCommands()->GetDrawCommands();
	const auto vrmode = VRMode::GetVRModeCached(true);
	const bool useLayeredSceneTransfer =
		vrmode != nullptr &&
		vrmode->IsVR() &&
		vrmode->ShouldUseMultiviewThisFrame() &&
		vr_openxr_multiview_postprocess &&
		buffers->GetSceneLayers() > 1 &&
		buffers->GetPipelineLayers() > 1;

	if (useLayeredSceneTransfer && fb->GetCurrentEyeLayer() > 0)
	{
		return;
	}

	Clocker transferTimer(VRSceneTransfer);
	VRSceneTransferOps++;
	fb->GetRenderState()->EndRenderPass();

	VkImageTransition()
		.AddImage(&buffers->SceneColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.AddImage(&buffers->PipelineImage[mCurrentPipelineImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
		.Execute(fb->GetCommands()->GetDrawCommands());

	const uint32_t sceneLayer = useLayeredSceneTransfer ? 0u : (buffers->GetSceneLayers() > 1 ? (uint32_t)fb->GetCurrentEyeLayer() : 0u);
	const uint32_t pipelineLayer = useLayeredSceneTransfer ? 0u : (buffers->GetPipelineLayers() > 1 ? (uint32_t)fb->GetCurrentEyeLayer() : 0u);
	const uint32_t layerCount = useLayeredSceneTransfer ? (uint32_t)std::min(buffers->GetSceneLayers(), buffers->GetPipelineLayers()) : 1u;

	if (buffers->GetSceneSamples() != VK_SAMPLE_COUNT_1_BIT)
	{
		auto sceneColor = buffers->SceneColor.Image.get();
		VkImageResolve resolve = {};
		resolve.srcOffset = { 0, 0, 0 };
		resolve.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		resolve.srcSubresource.mipLevel = 0;
		resolve.srcSubresource.baseArrayLayer = sceneLayer;
		resolve.srcSubresource.layerCount = layerCount;
		resolve.dstOffset = { 0, 0, 0 };
		resolve.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		resolve.dstSubresource.mipLevel = 0;
		resolve.dstSubresource.baseArrayLayer = pipelineLayer;
		resolve.dstSubresource.layerCount = layerCount;
		resolve.extent = { (uint32_t)sceneColor->width, (uint32_t)sceneColor->height, 1 };
		cmdbuffer->resolveImage(
			sceneColor->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			buffers->PipelineImage[mCurrentPipelineImage].Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &resolve);
	}
	else
	{
		auto sceneColor = buffers->SceneColor.Image.get();
		VkImageBlit blit = {};
		blit.srcOffsets[0] = { 0, 0, 0 };
		blit.srcOffsets[1] = { sceneColor->width, sceneColor->height, 1 };
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel = 0;
		blit.srcSubresource.baseArrayLayer = sceneLayer;
		blit.srcSubresource.layerCount = layerCount;
		blit.dstOffsets[0] = { 0, 0, 0 };
		blit.dstOffsets[1] = { sceneColor->width, sceneColor->height, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel = 0;
		blit.dstSubresource.baseArrayLayer = pipelineLayer;
		blit.dstSubresource.layerCount = layerCount;
		cmdbuffer->blitImage(
			sceneColor->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			buffers->PipelineImage[mCurrentPipelineImage].Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, VK_FILTER_NEAREST);
	}

	// [LIGHTMASK] The light mask goes with the colour (hw_postprocess.h, PPLightMask): the same
	// operation over the same layers into the first image of its pair, so the resolve averages
	// the amounts exactly as it averages the colour and the nearest blit copies them. Only when
	// this scene drew the mask; otherwise post-processing is told this eye has none.
	// [SCENEMASK] The tag needs no transfer of its own: post-processing samples the scene attachment
	// directly (PPTextureType::SceneMask), so all this eye has to say is whether ITS scene drew one. A
	// save picture or a camera texture says no, and the resolve then hands every shader 0.
	hw_postprocess.scenemask.SetPostInput(fb->SceneHasPostMask());

	const bool lightMask = fb->SceneHasLightMask();
	hw_postprocess.lightmask.SetPostInput(lightMask);
	mCurrentLightMaskImage = 0;
	if (lightMask)
	{
		auto sceneMask = buffers->SceneLightMask.Image.get();
		auto carryImage = buffers->LightMaskImage[0].Image.get();

		VkImageTransition()
			.AddImage(&buffers->SceneLightMask, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.AddImage(&buffers->LightMaskImage[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
			.Execute(fb->GetCommands()->GetDrawCommands());

		if (buffers->GetSceneSamples() != VK_SAMPLE_COUNT_1_BIT)
		{
			VkImageResolve resolve = {};
			resolve.srcOffset = { 0, 0, 0 };
			resolve.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			resolve.srcSubresource.mipLevel = 0;
			resolve.srcSubresource.baseArrayLayer = sceneLayer;
			resolve.srcSubresource.layerCount = layerCount;
			resolve.dstOffset = { 0, 0, 0 };
			resolve.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			resolve.dstSubresource.mipLevel = 0;
			resolve.dstSubresource.baseArrayLayer = pipelineLayer;
			resolve.dstSubresource.layerCount = layerCount;
			resolve.extent = { (uint32_t)sceneMask->width, (uint32_t)sceneMask->height, 1 };
			cmdbuffer->resolveImage(
				sceneMask->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				carryImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1, &resolve);
		}
		else
		{
			VkImageBlit blit = {};
			blit.srcOffsets[0] = { 0, 0, 0 };
			blit.srcOffsets[1] = { sceneMask->width, sceneMask->height, 1 };
			blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.srcSubresource.mipLevel = 0;
			blit.srcSubresource.baseArrayLayer = sceneLayer;
			blit.srcSubresource.layerCount = layerCount;
			blit.dstOffsets[0] = { 0, 0, 0 };
			blit.dstOffsets[1] = { sceneMask->width, sceneMask->height, 1 };
			blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.dstSubresource.mipLevel = 0;
			blit.dstSubresource.baseArrayLayer = pipelineLayer;
			blit.dstSubresource.layerCount = layerCount;
			cmdbuffer->blitImage(
				sceneMask->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				carryImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1, &blit, VK_FILTER_NEAREST);
		}
	}
}

void VkPostprocess::ImageTransitionScene(bool undefinedSrcLayout)
{
	auto buffers = fb->GetBuffers();

	VkImageTransition()
		.AddImage(&buffers->SceneColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, undefinedSrcLayout)
		.AddImage(&buffers->SceneFog, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, undefinedSrcLayout)
		.AddImage(&buffers->SceneNormal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, undefinedSrcLayout)
		.AddImage(&buffers->SceneDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, undefinedSrcLayout)
		.Execute(fb->GetCommands()->GetDrawCommands());

	// [LIGHTMASK] The light mask attachment, once it exists, is a scene image like the others.
	if (buffers->SceneLightMask.Image)
	{
		VkImageTransition()
			.AddImage(&buffers->SceneLightMask, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, undefinedSrcLayout)
			.Execute(fb->GetCommands()->GetDrawCommands());
	}

	// [SCENEMASK] And the tag attachment. Post-processing reads it as a sampled image and this puts it
	// back to a colour attachment for the next scene, exactly as the light mask's line does.
	if (buffers->ScenePostMask.Image)
	{
		VkImageTransition()
			.AddImage(&buffers->ScenePostMask, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, undefinedSrcLayout)
			.Execute(fb->GetCommands()->GetDrawCommands());
	}
}

void VkPostprocess::BlitCurrentToImage(VkTextureImage *dstimage, VkImageLayout finallayout)
{
	fb->GetRenderState()->EndRenderPass();

	auto srcimage = &fb->GetBuffers()->PipelineImage[mCurrentPipelineImage];
	auto cmdbuffer = fb->GetCommands()->GetDrawCommands();
	const uint32_t srcLayer = srcimage->Image && srcimage->Image->layerCount > 1 ? (uint32_t)fb->GetCurrentEyeLayer() : 0u;
	const uint32_t dstLayer = dstimage->Image && dstimage->Image->layerCount > 1 ? (uint32_t)fb->GetCurrentEyeLayer() : 0u;

	VkImageTransition()
		.AddImage(srcimage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.AddImage(dstimage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
		.Execute(cmdbuffer);

	VkImageBlit blit = {};
	blit.srcOffsets[0] = { 0, 0, 0 };
	blit.srcOffsets[1] = { srcimage->Image->width, srcimage->Image->height, 1 };
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.mipLevel = 0;
	blit.srcSubresource.baseArrayLayer = srcLayer;
	blit.srcSubresource.layerCount = 1;
	blit.dstOffsets[0] = { 0, 0, 0 };
	blit.dstOffsets[1] = { dstimage->Image->width, dstimage->Image->height, 1 };
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.mipLevel = 0;
	blit.dstSubresource.baseArrayLayer = dstLayer;
	blit.dstSubresource.layerCount = 1;

	cmdbuffer->blitImage(
		srcimage->Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dstimage->Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_NEAREST);

	VkImageTransition()
		.AddImage(dstimage, finallayout, false)
		.Execute(cmdbuffer);
}

void VkPostprocess::CopyCurrentToImage(VkTextureImage *dstimage, VkImageLayout finallayout)
{
	fb->GetRenderState()->EndRenderPass();

	auto srcimage = &fb->GetBuffers()->PipelineImage[mCurrentPipelineImage];
	auto cmdbuffer = fb->GetCommands()->GetDrawCommands();
	if (srcimage->Image->width != dstimage->Image->width || srcimage->Image->height != dstimage->Image->height) return;

	VkImageTransition()
	.AddImage(srcimage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
	.AddImage(dstimage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
	.Execute(cmdbuffer);

	VkImageCopy region = {};
	region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.layerCount = 1;
	region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.dstSubresource.layerCount = 1;
	region.extent.width = srcimage->Image->width;
	region.extent.height = srcimage->Image->height;
	region.extent.depth = 1;

	cmdbuffer->copyImage(
		srcimage->Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dstimage->Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &region
	);

	VkImageTransition()
	.AddImage(srcimage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
	.AddImage(dstimage, finallayout, false)
	.Execute(cmdbuffer);
}

void VkPostprocess::DrawPresentTexture(const IntRect &box, bool applyGamma, bool screenshot)
{
	VkPPRenderState renderstate(fb);

	if (!screenshot) // Already applied as we are actually copying the last frame here (GetScreenshotBuffer is called after swap)
		hw_postprocess.customShaders.Run(&renderstate, "screen");

	// Zero-init: not every member is assigned on every path (padding0 never is,
	// and the two paths below set different subsets), and the whole struct is
	// memcpy-ed into the uniform buffer.
	PresentUniforms uniforms = {};
	if (!applyGamma)
	{
		uniforms.InvGamma = 1.0f;
		uniforms.Contrast = 1.0f;
		uniforms.Brightness = 0.0f;
		uniforms.Saturation = 1.0f;
		uniforms.BlackPoint = 0.0f;
		uniforms.WhitePoint = 1.0f;
	}
	else
	{
		uniforms.InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
		uniforms.Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
		uniforms.Brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
		uniforms.Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
		uniforms.BlackPoint = clamp<float>(vid_i_blackpoint, 0.f, 1.f);
		uniforms.WhitePoint = clamp<float>(vid_i_whitepoint, 0.f, 5.f);
		uniforms.GrayFormula = static_cast<int>(gl_satformula);
	}

	uniforms.ColorScale = (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);

	if (screenshot)
	{
		uniforms.Scale = { screen->mScreenViewport.width / (float)fb->GetBuffers()->GetWidth(), screen->mScreenViewport.height / (float)fb->GetBuffers()->GetHeight() };
		uniforms.Offset = { 0.0f, 0.0f };
	}
	else
	{
		uniforms.Scale = { screen->mScreenViewport.width / (float)fb->GetBuffers()->GetWidth(), -screen->mScreenViewport.height / (float)fb->GetBuffers()->GetHeight() };
		uniforms.Offset = { 0.0f, 1.0f };
	}

	if (applyGamma && fb->GetFramebufferManager()->SwapChain->Format().colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && !screenshot)
	{
		uniforms.HdrMode = 1;
	}
	else
	{
		uniforms.HdrMode = 0;
	}

	renderstate.Clear();
	renderstate.Shader = &hw_postprocess.present.Present;
	renderstate.Uniforms.Set(uniforms);
	renderstate.Viewport = box;
	renderstate.SetInputCurrent(0, ViewportLinearScale() ? PPFilterMode::Linear : PPFilterMode::Nearest);
	renderstate.SetInputTexture(1, &hw_postprocess.present.Dither, PPFilterMode::Nearest, PPWrapMode::Repeat);
	if (screenshot)
		renderstate.SetOutputNext();
	else
		renderstate.SetOutputSwapChain();
	renderstate.SetNoBlend();
	renderstate.Draw();
}

// The XR present pass's uniforms: shared by DrawPresentTextureToImage and the [SPECTATOR]
// pass below, so the stabilized desktop view gets exactly the mirror's gamma stage.
static PresentUniforms MakeXrPresentUniforms(bool applyGamma, bool outputIsSrgb, float sourceScaleX, float sourceScaleY, float sourceOffsetX, float sourceOffsetY, bool applyOpenXrBias)
{
	// Zero-init: not every member is assigned on every path (padding0 never is,
	// and the two paths below set different subsets), and the whole struct is
	// memcpy-ed into the uniform buffer.
	PresentUniforms uniforms = {};
	if (!applyGamma)
	{
		uniforms.InvGamma = 1.0f;
		uniforms.Contrast = 1.0f;
		uniforms.Brightness = 0.0f;
		uniforms.Saturation = 1.0f;
		// Identity black/white remap. Upstream added this stage to
		// DrawPresentTexture but not to this fork-only XR variant; leaving it at
		// zero makes the shader's val * (WhitePoint - BlackPoint) + BlackPoint
		// collapse to 0 and the headset goes black.
		uniforms.BlackPoint = 0.0f;
		uniforms.WhitePoint = 1.0f;
	}
	else
	{
		// sRGB XR swapchains already get the final framebuffer transfer on write,
		// so applying the full software gamma exponent here tends to over-brighten
		// the submitted eye image. Keep the present-pass shaping, but use a softer
		// compensation curve to recover some of the darker midtone contrast seen
		// in the OpenVR/OpenGL path.
		const float gammaValue = clamp<float>(vid_gamma, 0.1f, 4.f);
		uniforms.InvGamma = outputIsSrgb ? (1.0f / sqrtf(gammaValue)) : (1.0f / gammaValue);
		uniforms.Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
		uniforms.Brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
		uniforms.Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
		// Same black/white remap the flat present applies. Without it this path
		// submits a black eye image.
		uniforms.BlackPoint = clamp<float>(vid_i_blackpoint, 0.f, 1.f);
		uniforms.WhitePoint = clamp<float>(vid_i_whitepoint, 0.f, 5.f);
		uniforms.GrayFormula = static_cast<int>(gl_satformula);

		// OpenXR headset compositor path can look noticeably brighter/flatter than
		// the local mirror/OpenVR reference even with matching source images. Allow
		// XR-only final present tuning to recover headset parity without affecting
		// the non-XR present path.
		if (applyOpenXrBias && IsOpenXRPresent())
		{
			const float gammaBias = clamp<float>(vr_openxr_present_gamma_bias, 0.25f, 4.0f);
			const float contrastBias = clamp<float>(vr_openxr_present_contrast_bias, 0.25f, 4.0f);
			const float brightnessBias = clamp<float>(vr_openxr_present_brightness_bias, -0.8f, 0.8f);
			const float saturationBias = clamp<float>(vr_openxr_present_saturation_bias, 0.0f, 4.0f);

			uniforms.InvGamma = clamp<float>(uniforms.InvGamma * gammaBias, 0.1f, 4.0f);
			uniforms.Contrast = clamp<float>(uniforms.Contrast * contrastBias, 0.1f, 3.0f);
			uniforms.Brightness = clamp<float>(uniforms.Brightness + brightnessBias, -0.8f, 0.8f);
			uniforms.Saturation = clamp<float>(uniforms.Saturation * saturationBias, -15.0f, 15.0f);
		}
	}
	uniforms.ColorScale = (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);

	uniforms.Scale = { sourceScaleX, sourceScaleY };
	uniforms.Offset = { sourceOffsetX, sourceOffsetY };

	uniforms.HdrMode = 0;

	return uniforms;
}

void VkPostprocess::DrawPresentTextureToImage(VkTextureImage *image, VkFormat outputFormat, const IntRect &box, bool applyGamma, bool screenshot, float sourceScaleX, float sourceScaleY, float sourceOffsetX, float sourceOffsetY, VulkanCommandBuffer *cmdbuffer, bool applyOpenXrBias)
{
	VkPPRenderState renderstate(fb);
	const bool outputIsSrgb = outputFormat == VK_FORMAT_B8G8R8A8_SRGB || outputFormat == VK_FORMAT_R8G8B8A8_SRGB;
	const PPFilterMode presentFilter = ViewportLinearScale() ? PPFilterMode::Linear : PPFilterMode::Nearest;

	if (!screenshot)
		hw_postprocess.customShaders.Run(&renderstate, "screen");

	PresentUniforms uniforms = MakeXrPresentUniforms(applyGamma, outputIsSrgb, sourceScaleX, sourceScaleY, sourceOffsetX, sourceOffsetY, applyOpenXrBias);

	renderstate.Clear();
	renderstate.Shader = &hw_postprocess.present.Present;
	renderstate.Uniforms.Set(uniforms);
	renderstate.Viewport = box;
	renderstate.SetInputCurrent(0, presentFilter);
	renderstate.SetInputTexture(1, &hw_postprocess.present.Dither, PPFilterMode::Nearest, PPWrapMode::Repeat);
	renderstate.SetNoBlend();
	renderstate.DrawToImage(image, outputFormat, cmdbuffer);
}

// [SPECTATOR] vr_spectator: the current pipeline image (one finished eye) reprojected into
// the smoothed spectator camera and written to 'image'. Same gamma as the plain mirror
// (applyGamma on, no OpenXR headset bias); no custom "screen" shaders, which the mirror pass
// has already run on this image.
void VkPostprocess::DrawSpectatorToImage(VkTextureImage *image, VkFormat outputFormat, const IntRect &box, const float specRot[16], const FVector4 &srcTan, const FVector4 &dstTan, float sourceScaleX, float sourceScaleY, float sourceOffsetX, float sourceOffsetY, VulkanCommandBuffer *cmdbuffer)
{
	VkPPRenderState renderstate(fb);
	const bool outputIsSrgb = outputFormat == VK_FORMAT_B8G8R8A8_SRGB || outputFormat == VK_FORMAT_R8G8B8A8_SRGB;
	const PresentUniforms present = MakeXrPresentUniforms(true, outputIsSrgb, sourceScaleX, sourceScaleY, sourceOffsetX, sourceOffsetY, false);

	SpectatorUniforms uniforms = {};
	memcpy(&uniforms, &present, sizeof(PresentUniforms));	// same layout, asserted in hw_postprocess.h
	memcpy(uniforms.SpecRot, specRot, sizeof(uniforms.SpecRot));
	uniforms.SpecSrcTan = srcTan;
	uniforms.SpecDstTan = dstTan;

	renderstate.Clear();
	renderstate.Shader = &hw_postprocess.present.Spectator;
	renderstate.Uniforms.Set(uniforms);
	renderstate.Viewport = box;
	renderstate.SetInputCurrent(0, PPFilterMode::Linear);
	renderstate.SetInputTexture(1, &hw_postprocess.present.Dither, PPFilterMode::Nearest, PPWrapMode::Repeat);
	renderstate.SetNoBlend();
	renderstate.DrawToImage(image, outputFormat, cmdbuffer);
}

void VkPostprocess::AmbientOccludeScene(float m5)
{
	int sceneWidth = fb->GetBuffers()->GetSceneWidth();
	int sceneHeight = fb->GetBuffers()->GetSceneHeight();

	VkPPRenderState renderstate(fb);
	hw_postprocess.ssao.Render(&renderstate, m5, sceneWidth, sceneHeight);

	ImageTransitionScene(false);
}

void VkPostprocess::BlurScene(float gameinfobluramount)
{
	int sceneWidth = fb->GetBuffers()->GetSceneWidth();
	int sceneHeight = fb->GetBuffers()->GetSceneHeight();

	VkPPRenderState renderstate(fb);

	auto vrmode = VRMode::GetVRModeCached(true);
	// vr_menu_keep_world: the menu is on its own quad and the headset is showing
	// the live world so you can see what the sliders do. Blurring the eye images
	// here would defeat that; the menu dim is a 2D quad and stays on the panel.
	if (vrmode->IsMenuOverWorldFrame())
		return;
	int eyeCount = vrmode->mEyeCount;
	for (int i = 0; i < eyeCount; ++i)
	{
		hw_postprocess.bloom.RenderBlur(&renderstate, sceneWidth, sceneHeight, gameinfobluramount);
		if (eyeCount - i > 1) NextEye(eyeCount);
	}
}

void VkPostprocess::ClearTonemapPalette()
{
	hw_postprocess.tonemap.ClearTonemapPalette();
}

void VkPostprocess::UpdateShadowMap()
{
	if (screen->mShadowMap.PerformUpdate())
	{
		VkPPRenderState renderstate(fb);
		hw_postprocess.shadowmap.Update(&renderstate);

		VkImageTransition()
			.AddImage(&fb->GetTextureManager()->Shadowmap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(fb->GetCommands()->GetDrawCommands());

		screen->mShadowMap.FinishUpdate();
	}
}

void VkPostprocess::NextEye(int eyeCount)
{
	if (eyeCount > 1)
		mCurrentPipelineImage = (mCurrentPipelineImage + 1) % VkRenderBuffers::NumPipelineImages;
}

void VkPostprocess::SetCurrentPipelineImage(int index)
{
	int count = VkRenderBuffers::NumPipelineImages;
	if (count <= 0)
	{
		mCurrentPipelineImage = 0;
		return;
	}

	mCurrentPipelineImage = ((index % count) + count) % count;
}

void VkPostprocess::SetPipelineImagePair(int start, int size)
{
	const int count = VkRenderBuffers::NumPipelineImages;
	if (count <= 0)
	{
		mPipelinePairStart = 0;
		mPipelinePairSize = 1;
		mCurrentPipelineImage = 0;
		return;
	}

	mPipelinePairStart = ((start % count) + count) % count;
	mPipelinePairSize = std::clamp(size, 1, count);

	// Clamp the current image into the active pair so the caller can swap
	// pair ownership without accidentally sampling another eye's history.
	const int pairEnd = mPipelinePairStart + mPipelinePairSize;
	if (mCurrentPipelineImage < mPipelinePairStart || mCurrentPipelineImage >= pairEnd)
	{
		mCurrentPipelineImage = mPipelinePairStart;
	}
}

void VkPostprocess::AdvancePipelineImage()
{
	mCurrentPipelineImage = GetNextPipelineImage();
}
