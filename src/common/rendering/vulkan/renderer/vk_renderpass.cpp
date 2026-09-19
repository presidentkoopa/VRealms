/*
** vk_renderpass.cpp
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

#include "vk_renderpass.h"
#include "vk_renderstate.h"
#include "vk_descriptorset.h"
#include "vk_raytrace.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/shaders/vk_ppshader.h"
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_hwbuffer.h"
#include "flatvertices.h"
#include "hw_viewpointuniforms.h"
#include "v_2ddrawer.h"
#include "i_specialpaths.h"
#include "cmdlib.h"
#include "printf.h"
#include "hwrenderer/data/hw_perftrack.h"	// RS FORK -- perf_track: a pipeline compiled mid-frame is a hitch reason
#include <cstdio>

VkRenderPassManager::VkRenderPassManager(VulkanRenderDevice* fb) : fb(fb)
{
	FString path = M_GetCachePath(true);
	CreatePath(path.GetChars());
	CacheFilename = path + "/vulkanpipelinecache";

	PipelineCacheBuilder builder;
	builder.DebugName("PipelineCache");

	try
	{
		FileReader fr;
		if (fr.OpenFile(CacheFilename.GetChars()))
		{
			std::vector<uint8_t> data;
			data.resize(fr.GetLength());
			if (fr.Read(data.data(), data.size()) == (FileReader::Size)data.size())
			{
				builder.InitialData(data.data(), data.size());
			}
		}
	}
	catch (...)
	{
	}

	// [BB] A BAD CACHE MUST NOT BE FATAL.
	//
	// This is a CACHE. Every byte of it is regenerable, it exists only to save
	// shader compile time on startup, and losing it costs a few seconds once --
	// so refusing to launch over one is the worst possible response to it going
	// wrong.
	//
	// And it does go wrong. The builder already checks the header's version,
	// vendor, device and UUID before handing the data to the driver, so a blob
	// from another GPU is rejected safely. What that check cannot see is a body
	// that was truncated by a crash during shutdown: the header still validates,
	// the data goes in, and vkCreatePipelineCache comes back VK_ERROR_UNKNOWN
	// (-13). The engine then died on the spot, at Vulkan init, before a single
	// lump was read -- which looks exactly like a broken build and sends you
	// looking at everything except a file in AppData.
	//
	// Retry once with no initial data, which is the same thing that happens on a
	// first run and is always valid.
	try
	{
		PipelineCache = builder.Create(fb->device.get());
	}
	catch (const std::exception& e)
	{
		Printf(TEXTCOLOR_YELLOW "Pipeline cache rejected (%s); rebuilding it.\n", e.what());

		// Delete it rather than leave it to fail again next launch, and do that
		// BEFORE the retry -- if the retry somehow throws too, the bad file is
		// still gone and the next start is clean.
		remove(CacheFilename.GetChars());

		PipelineCacheBuilder fresh;
		fresh.DebugName("PipelineCache");
		PipelineCache = fresh.Create(fb->device.get());
	}
}

VkRenderPassManager::~VkRenderPassManager()
{
	try
	{
		auto data = PipelineCache->GetCacheData();
		std::unique_ptr<FileWriter> fw(FileWriter::Open(CacheFilename.GetChars()));
		if (fw)
			fw->Write(data.data(), data.size());
	}
	catch (...)
	{
	}
}

void VkRenderPassManager::RenderBuffersReset()
{
	RenderPassSetup.clear();
	PPRenderPassSetup.clear();
}

VkRenderPassSetup *VkRenderPassManager::GetRenderPass(const VkRenderPassKey &key)
{
	auto &item = RenderPassSetup[key];
	if (!item)
		item.reset(new VkRenderPassSetup(fb, key));
	return item.get();
}

int VkRenderPassManager::GetVertexFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs)
{
	for (size_t i = 0; i < VertexFormats.size(); i++)
	{
		const auto &f = VertexFormats[i];
		if (f.Attrs.size() == (size_t)numAttributes && f.NumBindingPoints == numBindingPoints && f.Stride == stride)
		{
			bool matches = true;
			for (int j = 0; j < numAttributes; j++)
			{
				if (memcmp(&f.Attrs[j], &attrs[j], sizeof(FVertexBufferAttribute)) != 0)
				{
					matches = false;
					break;
				}
			}

			if (matches)
				return (int)i;
		}
	}

	VkVertexFormat fmt;
	fmt.NumBindingPoints = numBindingPoints;
	fmt.Stride = stride;
	fmt.UseVertexData = 0;
	for (int j = 0; j < numAttributes; j++)
	{
		if (attrs[j].location == VATTR_COLOR)
			fmt.UseVertexData |= 1;
		else if (attrs[j].location == VATTR_NORMAL)
			fmt.UseVertexData |= 2;
		fmt.Attrs.push_back(attrs[j]);
	}
	VertexFormats.push_back(fmt);
	return (int)VertexFormats.size() - 1;
}

VkVertexFormat *VkRenderPassManager::GetVertexFormat(int index)
{
	return &VertexFormats[index];
}

VulkanPipelineLayout* VkRenderPassManager::GetPipelineLayout(int numLayers)
{
	if (PipelineLayouts.size() <= (size_t)numLayers)
		PipelineLayouts.resize(numLayers + 1);

	auto &layout = PipelineLayouts[numLayers];
	if (layout)
		return layout.get();

	auto descriptors = fb->GetDescriptorSetManager();

	PipelineLayoutBuilder builder;
	builder.AddSetLayout(descriptors->GetFixedSetLayout());
	builder.AddSetLayout(descriptors->GetHWBufferSetLayout());
	if (numLayers != 0)
		builder.AddSetLayout(descriptors->GetTextureSetLayout(numLayers));
	builder.AddPushConstantRange(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants));
	builder.DebugName("VkRenderPassManager.PipelineLayout");
	layout = builder.Create(fb->device.get());
	return layout.get();
}

VkPPRenderPassSetup* VkRenderPassManager::GetPPRenderPass(const VkPPRenderPassKey& key)
{
	auto& passSetup = PPRenderPassSetup[key];
	if (!passSetup)
		passSetup.reset(new VkPPRenderPassSetup(fb, key));
	return passSetup.get();
}

/////////////////////////////////////////////////////////////////////////////

VkRenderPassSetup::VkRenderPassSetup(VulkanRenderDevice* fb, const VkRenderPassKey &key) : PassKey(key), fb(fb)
{
}

std::unique_ptr<VulkanRenderPass> VkRenderPassSetup::CreateRenderPass(int clearTargets)
{
	auto buffers = fb->GetBuffers();

	VkFormat drawBufferFormats[] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, buffers->SceneNormalFormat };

	RenderPassBuilder builder;

	builder.AddAttachment(
		PassKey.DrawBufferFormat, (VkSampleCountFlagBits)PassKey.Samples,
		(clearTargets & CT_Color) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	for (int i = 1; i < PassKey.DrawBuffers; i++)
	{
		builder.AddAttachment(
			drawBufferFormats[i], (VkSampleCountFlagBits)PassKey.Samples,
			(clearTargets & CT_Color) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	}
	// [LIGHTMASK] The light mask attachment (VkRenderPassKey::LightMask), after the draw buffers
	// and before depth, loaded, stored and cleared with the colour (a colour clear clears it to 0).
	// Without the mask, colorAttachments is DrawBuffers and nothing below changes.
	// [SCENEMASK] And the tag attachment after it (VkRenderPassKey::PostMask), same rules: loaded, stored
	// and cleared with the colour, so a colour clear clears the whole frame's tags to 0 -- "nothing
	// special" -- before anything draws. With neither, colorAttachments is DrawBuffers as it always was.
	const int colorAttachments = PassKey.DrawBuffers + (PassKey.LightMask ? 1 : 0) + (PassKey.PostMask ? 1 : 0);
	if (PassKey.LightMask)
	{
		builder.AddAttachment(
			buffers->LightMaskFormat, (VkSampleCountFlagBits)PassKey.Samples,
			(clearTargets & CT_Color) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		// One line per pass layout per session, so a log shows what the mask pass really holds.
		static unsigned loggedLayouts = 0;
		const unsigned layoutBit = 1u << ((PassKey.DrawBuffers > 1 ? 1 : 0) | (PassKey.Samples > 1 ? 2 : 0) | (PassKey.ViewMask != 0 ? 4 : 0));
		if (!(loggedLayouts & layoutBit))
		{
			loggedLayouts |= layoutBit;
			Printf("LightMask: scene pass with the light mask -- colour attachments: target%s, mask (VkFormat %d) at %d; depth %s%d; %d blend attachments; %d samples; view mask %u\n",
				PassKey.DrawBuffers > 2 ? ", fog, normal" : (PassKey.DrawBuffers > 1 ? ", fog" : ""),
				(int)buffers->LightMaskFormat, PassKey.DrawBuffers,	// [SCENEMASK] its own index: the tag may follow it
				PassKey.DepthStencil ? "at " : "none, would be ", colorAttachments,
				colorAttachments, PassKey.Samples, (unsigned)PassKey.ViewMask);
		}
	}
	// [SCENEMASK] The scene mask attachment, after the light mask and before depth. R8 (or whatever the
	// device probe settled on), cleared with the colour, stored so post-processing can read it.
	if (PassKey.PostMask)
	{
		builder.AddAttachment(
			buffers->PostMaskFormat, (VkSampleCountFlagBits)PassKey.Samples,
			(clearTargets & CT_Color) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		// One line per pass layout per session, so a log shows what the tag pass really holds.
		static unsigned loggedMaskLayouts = 0;
		const unsigned layoutBit = 1u << ((PassKey.DrawBuffers > 1 ? 1 : 0) | (PassKey.Samples > 1 ? 2 : 0) | (PassKey.ViewMask != 0 ? 4 : 0) | (PassKey.LightMask ? 8 : 0));
		if (!(loggedMaskLayouts & layoutBit))
		{
			loggedMaskLayouts |= layoutBit;
			Printf("SceneMask: scene pass with the tag -- colour attachments: target%s%s, tag (VkFormat %d) at %d; depth %s%d; %d blend attachments; %d samples; view mask %u\n",
				PassKey.DrawBuffers > 2 ? ", fog, normal" : (PassKey.DrawBuffers > 1 ? ", fog" : ""),
				PassKey.LightMask ? ", light mask" : "",
				(int)buffers->PostMaskFormat, PassKey.DrawBuffers + (PassKey.LightMask ? 1 : 0),
				PassKey.DepthStencil ? "at " : "none, would be ", colorAttachments,
				colorAttachments, PassKey.Samples, (unsigned)PassKey.ViewMask);
		}
	}
	if (PassKey.DepthStencil && PassKey.DepthReadOnly)
	{
		// [2a] THE READ-ONLY DEPTH PASS (FRenderState::SetSceneDepthReadable). Load
		// and store both aspects, never clear -- a CLEAR load op is not allowed on an
		// attachment first used read-only, and "readable" means the depth drawn so
		// far. Initial and final layouts stay DEPTH_STENCIL_ATTACHMENT_OPTIMAL like
		// every other scene pass, so the pass after this one, and post-processing,
		// see exactly what they saw before 2a. The subpass reference below is what
		// makes it read-only.
		builder.AddDepthStencilAttachment(
			buffers->SceneDepthStencilFormat, (VkSampleCountFlagBits)PassKey.Samples,
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}
	else if (PassKey.DepthStencil)
	{
		builder.AddDepthStencilAttachment(
			buffers->SceneDepthStencilFormat, (VkSampleCountFlagBits)PassKey.Samples,
			(clearTargets & CT_Depth) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			(clearTargets & CT_Stencil) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}
	builder.AddSubpass();
	for (int i = 0; i < colorAttachments; i++)	// [LIGHTMASK] the mask's reference too
		builder.AddSubpassColorAttachmentRef(i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if (PassKey.DepthStencil && PassKey.DepthReadOnly)
	{
		// [2a] Depth and stencil referenced read-only, so a fragment shader may
		// sample the same image through binding 3 (the layout that binding is
		// written with). The dependency adds the fragment-shader read, as the
		// post-process passes that sample scene depth declare it.
		builder.AddSubpassDepthStencilAttachmentRef(colorAttachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);	// [LIGHTMASK] after every colour attachment
		builder.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
	}
	else if (PassKey.DepthStencil)
	{
		builder.AddSubpassDepthStencilAttachmentRef(colorAttachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);	// [LIGHTMASK] after every colour attachment
		builder.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
	}
	else
	{
		builder.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
	}
	if (PassKey.ViewMask != 0)
		builder.Multiview(PassKey.ViewMask, PassKey.ViewMask);
	builder.DebugName("VkRenderPassSetup.RenderPass");
	return builder.Create(fb->device.get());
}

VulkanRenderPass *VkRenderPassSetup::GetRenderPass(int clearTargets)
{
	if (!RenderPasses[clearTargets])
		RenderPasses[clearTargets] = CreateRenderPass(clearTargets);
	return RenderPasses[clearTargets].get();
}

VulkanPipeline *VkRenderPassSetup::GetPipeline(const VkPipelineKey &key)
{
	auto &item = Pipelines[key];
	if (!item)
	{
		// RS FORK -- perf_track: a cache miss here compiles a graphics pipeline inside the frame, which is the
		// classic mid-fight stutter. Timed so the record can name it (REASON_PIPELINE); off, no clock is read.
		PerfTrack::Scope pipelineScope(PerfTrack::REASON_PIPELINE);
		item = CreatePipeline(key);
	}
	return item.get();
}

std::unique_ptr<VulkanPipeline> VkRenderPassSetup::CreatePipeline(const VkPipelineKey &key)
{
	GraphicsPipelineBuilder builder;
	builder.Cache(fb->GetRenderPassManager()->GetCache());

	VkShaderProgram *program;
	// [2a] In a read-only depth pass, an effect that reads scene depth draws with
	// its scene-depth fragment variant for this pass's sample count and layering
	// (layered = multiview, which is the only layered target
	// VkDescriptorSetManager::IsSceneDepthReadTarget accepts). Null otherwise --
	// every other pipeline, and every pipeline of every ordinary pass, keeps the
	// fragment shader it always had.
	VulkanShader *sceneDepthFrag = nullptr;
	// [LIGHTMASK] In a pass with the light mask, the mask variant of the fragment shader this
	// pipeline would draw with -- of the scene-depth variant when that is the one. Null in every
	// other pass, and then the choice below is exactly what it was.
	// [SCENEMASK] The same, generalised: which extra attachments this pass carries decides which variant
	// of the fragment shader writes them. 0 -- every pass that carries neither -- takes the ordinary
	// program and this whole branch does nothing, exactly as before.
	VulkanShader *sceneExtraFrag = nullptr;
	const int sceneExtras = (PassKey.LightMask ? VkShaderManager::SCENE_EXTRA_LIGHT_MASK : 0) | (PassKey.PostMask ? VkShaderManager::SCENE_EXTRA_POST_MASK : 0);
	if (key.SpecialEffect != EFF_NONE)
	{
		program = fb->GetShaderManager()->GetEffect(key.SpecialEffect, PassKey.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS);
		if (PassKey.DepthReadOnly)
			sceneDepthFrag = fb->GetShaderManager()->GetSceneDepthEffectFrag(key.SpecialEffect, PassKey.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS, PassKey.Samples > 1, PassKey.ViewMask != 0);
		if (sceneExtras != 0)
			sceneExtraFrag = sceneDepthFrag
				? fb->GetShaderManager()->GetSceneExtraSceneDepthEffectFrag(key.SpecialEffect, PassKey.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS, PassKey.Samples > 1, PassKey.ViewMask != 0, sceneExtras)
				: fb->GetShaderManager()->GetSceneExtraEffectFrag(key.SpecialEffect, PassKey.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS, sceneExtras);
	}
	else
	{
		program = fb->GetShaderManager()->Get(key.EffectState, key.AlphaTest, PassKey.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS);
		if (sceneExtras != 0)
			sceneExtraFrag = fb->GetShaderManager()->GetSceneExtraFrag(key.EffectState, key.AlphaTest, PassKey.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS, sceneExtras);
	}
	builder.AddVertexShader(program->vert.get());
	builder.AddFragmentShader(sceneExtraFrag ? sceneExtraFrag : (sceneDepthFrag ? sceneDepthFrag : program->frag.get()));

	const VkVertexFormat &vfmt = *fb->GetRenderPassManager()->GetVertexFormat(key.VertexFormat);

	for (int i = 0; i < vfmt.NumBindingPoints; i++)
		builder.AddVertexBufferBinding(i, vfmt.Stride);

	const static VkFormat vkfmts[] = {
		VK_FORMAT_R32G32B32A32_SFLOAT,
		VK_FORMAT_R32G32B32_SFLOAT,
		VK_FORMAT_R32G32_SFLOAT,
		VK_FORMAT_R32_SFLOAT,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_A2B10G10R10_SNORM_PACK32,
		VK_FORMAT_R8G8B8A8_UINT,
		VK_FORMAT_R16G16B16A16_UINT	// VFmt_UShort4_UInt -- 16-bit bone selectors
	};
	static_assert((sizeof(vkfmts)/sizeof(vkfmts[0])) == VFmt_COUNT,
		"vkfmts must have one entry per VertexFormat -- see the note on VFmt_COUNT in buffers.h");

	bool inputLocations[VATTR_MAX] = {};

	for (size_t i = 0; i < vfmt.Attrs.size(); i++)
	{
		const auto &attr = vfmt.Attrs[i];
		builder.AddVertexAttribute(attr.location, attr.binding, vkfmts[attr.format], attr.offset);
		inputLocations[attr.location] = true;
	}

	// Vulkan requires an attribute binding for each location specified in the shader
	for (int i = 0; i < VATTR_MAX; i++)
	{
		if (!inputLocations[i])
			builder.AddVertexAttribute(i, 0, i != 8 ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_R8G8B8A8_UINT, 0);
	}

	builder.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT);
	builder.AddDynamicState(VK_DYNAMIC_STATE_SCISSOR);
	builder.AddDynamicState(VK_DYNAMIC_STATE_DEPTH_BIAS);
	builder.AddDynamicState(VK_DYNAMIC_STATE_STENCIL_REFERENCE);

	// Note: the actual values are ignored since we use dynamic viewport+scissor states
	builder.Viewport(0.0f, 0.0f, 320.0f, 200.0f);
	builder.Scissor(0, 0, 320, 200);

	static const VkPrimitiveTopology vktopology[] = {
		VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
		VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
	};

	static const VkStencilOp op2vk[] = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP };
	static const VkCompareOp depthfunc2vk[] = { VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_ALWAYS };

	// [2a] A pipeline for a read-only depth pass never writes depth or stencil:
	// Vulkan forbids depth writes and non-KEEP stencil ops against a read-only
	// depth/stencil layout (VUID-vkCmdDraw-None-06886 / 06887). The effects drawn
	// there already have depth writes off and the stencil op at KEEP; this makes
	// it hold for anything, and changes nothing for the ordinary pass.
	const bool depthReadOnly = PassKey.DepthReadOnly != 0;

	builder.Topology(vktopology[key.DrawType]);
	builder.DepthStencilEnable(key.DepthTest, depthReadOnly ? 0 : key.DepthWrite, key.StencilTest);
	builder.DepthFunc(depthfunc2vk[key.DepthFunc]);
	if (fb->device->EnabledFeatures.Features.depthClamp)
		builder.DepthClampEnable(key.DepthClamp);
	builder.DepthBias(key.DepthBias, 0.0f, 0.0f, 0.0f);

	// Note: CCW and CW is intentionally swapped here because the vulkan and opengl coordinate systems differ.
	// main.vp addresses this by patching up gl_Position.z, which has the side effect of flipping the sign of the front face calculations.
	builder.Cull(key.CullMode == Cull_None ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT, key.CullMode == Cull_CW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);

	builder.Stencil(VK_STENCIL_OP_KEEP, depthReadOnly ? VK_STENCIL_OP_KEEP : op2vk[key.StencilPassOp], VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffffffff, 0xffffffff, 0);	// [2a] KEEP when read-only

	ColorBlendAttachmentBuilder blendbuilder;
	blendbuilder.ColorWriteMask((VkColorComponentFlags)key.ColorMask);
	BlendMode(blendbuilder, key.RenderStyle);

	// [LIGHTMASK] One blend state per colour attachment, the light mask's included: the mask is
	// blended with its colour's own factors, which is what keeps its amounts in step with it.
	for (int i = 0; i < PassKey.DrawBuffers + (PassKey.LightMask ? 1 : 0); i++)
		builder.AddColorBlendAttachment(blendbuilder.Create());

	// [SCENEMASK] THE TAG DOES NOT BLEND. Vulkan sets blend state per attachment, so the colour keeps
	// the render style's blend while this one writes the fragment's own byte straight in. That is the
	// whole reason the tag can live in the same pass as the translucent draws: a blended tag over an
	// untagged wall would be an in-between number nothing drew. Unblended, under the depth test the
	// pass already applies, the last fragment to write a pixel owns its tag -- which is the same
	// fragment that most recently owned its colour.
	//
	// Red only: the tag is one byte and the other channels are not there to write.
	//
	// A draw that writes no colour at all -- a portal stencil, FRenderState::SetColorMask(false), which
	// is also what SF_ColorMaskOff gives the stencil effect -- writes no tag either, so what it covers
	// keeps the tag it had. That is general: it asks whether the draw paints, not who the draw is.
	if (PassKey.PostMask)
	{
		ColorBlendAttachmentBuilder maskblend;
		maskblend.ColorWriteMask((key.ColorMask & 0x7) != 0 ? (VkColorComponentFlags)VK_COLOR_COMPONENT_R_BIT : (VkColorComponentFlags)0);
		builder.AddColorBlendAttachment(maskblend.Create());
	}

	builder.RasterizationSamples((VkSampleCountFlagBits)PassKey.Samples);

	builder.Layout(fb->GetRenderPassManager()->GetPipelineLayout(key.NumTextureLayers));
	builder.RenderPass(GetRenderPass(0));
	builder.DebugName("VkRenderPassSetup.Pipeline");

	return builder.Create(fb->device.get());
}

/////////////////////////////////////////////////////////////////////////////

VkPPRenderPassSetup::VkPPRenderPassSetup(VulkanRenderDevice* fb, const VkPPRenderPassKey& key) : fb(fb)
{
	CreateDescriptorLayout(key);
	CreatePipelineLayout(key);
	CreateRenderPass(key);
	CreatePipeline(key);
}

void VkPPRenderPassSetup::CreateDescriptorLayout(const VkPPRenderPassKey& key)
{
	DescriptorSetLayoutBuilder builder;
	for (int i = 0; i < key.InputTextures; i++)
		builder.AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);

	// Automatic uniforms at dedicated binding point (separate from push constants)
	builder.AddBinding(AUTOMATIC_UNIFORMS_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);

	if (key.ShadowMapBuffers)
	{
		builder.AddBinding(LIGHTNODES_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		builder.AddBinding(LIGHTLINES_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		builder.AddBinding(LIGHTLIST_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	}
	builder.DebugName("VkPPRenderPassSetup.DescriptorLayout");
	DescriptorLayout = builder.Create(fb->device.get());
}

void VkPPRenderPassSetup::CreatePipelineLayout(const VkPPRenderPassKey& key)
{
	PipelineLayoutBuilder builder;
	builder.AddSetLayout(DescriptorLayout.get());
	if (key.Uniforms > 0)
		builder.AddPushConstantRange(VK_SHADER_STAGE_FRAGMENT_BIT, 0, key.Uniforms);
	builder.DebugName("VkPPRenderPassSetup.PipelineLayout");
	PipelineLayout = builder.Create(fb->device.get());
}

void VkPPRenderPassSetup::CreatePipeline(const VkPPRenderPassKey& key)
{
	GraphicsPipelineBuilder builder;
	builder.Cache(fb->GetRenderPassManager()->GetCache());
	builder.AddVertexShader(key.Shader->VertexShader.get());
	builder.AddFragmentShader(key.Shader->FragmentShader.get());

	builder.AddVertexBufferBinding(0, sizeof(FFlatVertex));
	builder.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FFlatVertex, x));
	builder.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(FFlatVertex, u));
	builder.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT);
	builder.AddDynamicState(VK_DYNAMIC_STATE_SCISSOR);
	// Note: the actual values are ignored since we use dynamic viewport+scissor states
	builder.Viewport(0.0f, 0.0f, 320.0f, 200.0f);
	builder.Scissor(0, 0, 320, 200);
	if (key.StencilTest != WhichDepthStencil::None)
	{
		builder.AddDynamicState(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
		builder.DepthStencilEnable(false, false, true);
		builder.Stencil(VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffffffff, 0xffffffff, 0);
	}
	builder.Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);

	ColorBlendAttachmentBuilder blendbuilder;
	BlendMode(blendbuilder, key.BlendMode);
	builder.AddColorBlendAttachment(blendbuilder.Create());

	builder.RasterizationSamples(key.Samples);
	builder.Layout(PipelineLayout.get());
	builder.RenderPass(RenderPass.get());
	builder.DebugName("VkPPRenderPassSetup.Pipeline");
	Pipeline = builder.Create(fb->device.get());
}

void VkPPRenderPassSetup::CreateRenderPass(const VkPPRenderPassKey& key)
{
	RenderPassBuilder builder;
	if (key.SwapChain)
		builder.AddAttachment(key.OutputFormat, key.Samples, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	else
		builder.AddAttachment(key.OutputFormat, key.Samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if (key.StencilTest == WhichDepthStencil::Scene)
	{
		builder.AddDepthStencilAttachment(
			fb->GetBuffers()->SceneDepthStencilFormat, key.Samples,
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}
	if (key.StencilTest == WhichDepthStencil::Pipeline)
	{
		builder.AddDepthStencilAttachment(
			fb->GetBuffers()->PipelineDepthStencilFormat, key.Samples,
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}

	builder.AddSubpass();
	builder.AddSubpassColorAttachmentRef(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if (key.StencilTest != WhichDepthStencil::None)
	{
		builder.AddSubpassDepthStencilAttachmentRef(1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		builder.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
	}
	else
	{
		builder.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
	}

	builder.DebugName("VkPPRenderPassSetup.RenderPass");
	if (key.ViewMask != 0)
		builder.Multiview(key.ViewMask, key.ViewMask);
	RenderPass = builder.Create(fb->device.get());
}

/////////////////////////////////////////////////////////////////////////////

ColorBlendAttachmentBuilder& BlendMode(ColorBlendAttachmentBuilder& builder, const FRenderStyle& style)
{
	// Just in case Vulkan doesn't do this optimization itself
	if (style.BlendOp == STYLEOP_Add && style.SrcAlpha == STYLEALPHA_One && style.DestAlpha == STYLEALPHA_Zero && style.Flags == 0)
	{
		return builder;
	}

	static const int blendstyles[] = {
		VK_BLEND_FACTOR_ZERO,
		VK_BLEND_FACTOR_ONE,
		VK_BLEND_FACTOR_SRC_ALPHA,
		VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		VK_BLEND_FACTOR_SRC_COLOR,
		VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
		VK_BLEND_FACTOR_DST_COLOR,
		VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
		VK_BLEND_FACTOR_DST_ALPHA,
		VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
	};

	static const int renderops[] = {
		0, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
	};

	int srcblend = blendstyles[style.SrcAlpha % STYLEALPHA_MAX];
	int dstblend = blendstyles[style.DestAlpha % STYLEALPHA_MAX];
	int blendequation = renderops[style.BlendOp & 15];

	if (blendequation == -1)	// This was a fuzz style.
	{
		srcblend = VK_BLEND_FACTOR_DST_COLOR;
		dstblend = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blendequation = VK_BLEND_OP_ADD;
	}

	return builder.BlendMode((VkBlendOp)blendequation, (VkBlendFactor)srcblend, (VkBlendFactor)dstblend);
}
