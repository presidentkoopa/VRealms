/*
** vk_compute.cpp
**
** [COMPUTE] GPU compute for effects. See the header.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include <exception>

#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkanobjects.h>

#include "vk_compute.h"
#include "vk_smokevolume.h"
#include "vk_renderstate.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/shaders/vk_shader.h"
#include "hw_framecompute.h"
#include "hw_perflog.h"
#include "filesystem.h"
#include "cmdlib.h"
#include "i_time.h"
#include "printf.h"
#include "v_text.h"

VkComputeManager::VkComputeManager(VulkanRenderDevice* fb) : fb(fb)
{
	// Nothing is created here: pools, the sampler and every client appear on first use,
	// so a session in which no effect asks for compute allocates nothing.
}

VkComputeManager::~VkComputeManager()
{
	// Members go in reverse order: the smoke volume (and its descriptor sets) first,
	// then the sampler, then the pools. The device is idle by now (the render device's
	// destructor waits for it), so nothing needs a delete list.
}

void VkComputeManager::RunFrame(const FrameComputeInput& input)
{
	// fx.compute on the CPU line: this whole call, every frame while the perf log is on,
	// work or not -- so the log shows what the hook costs when nothing asks for smoke.
	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	mWorkBegun = false;

	// [SMOKEVOLUME] Constructed the first time smoke is asked for; it frees its images
	// itself when it is no longer active.
	if (mSmokeVolume == nullptr && input.Smoke.Active)
		mSmokeVolume = std::make_unique<VkSmokeVolume>(this);
	if (mSmokeVolume != nullptr)
		mSmokeVolume->Run(input.Smoke);

	// [LEVELFIELD] #8, [DEBRISPOOL] #9, [SURFACEDAMAGE] #17 run here, after the smoke.

	if (mWorkBegun)
	{
		fb->GetCommands()->PopGroup();	// "fx.compute", opened by BeginWork
		mWorkBegun = false;
	}

	if (timed)
		PerfLog::AddCpuSample("fx.compute", (double)(I_nsTime() - startNs) / 1e6);
}

void VkComputeManager::BeginWork()
{
	if (mWorkBegun)
		return;
	mWorkBegun = true;

	// Compute must be recorded outside a render pass. This runs before the eye loop, so
	// normally no pass is open (the shadow map update may have left one); ending it here
	// is exactly what the scene transfer does (VkPostprocess::BlitSceneToPostprocess).
	fb->GetRenderState()->EndRenderPass();
	fb->GetCommands()->PushGroup("fx.compute");
}

std::unique_ptr<VkComputeProgram> VkComputeManager::CreateProgram(const char* lumpName, const std::vector<VkComputeBinding>& bindings, uint32_t pushConstantBytes, const char* defines)
{
	int lump = fileSystem.CheckNumForFullName(lumpName, 0);
	if (lump == -1)
	{
		Printf(TEXTCOLOR_RED "Compute: shader lump '%s' not found -- that effect stays off\n", lumpName);
		return nullptr;
	}

	FString code;
	code << "#version 450\n#extension GL_GOOGLE_include_directive : enable\n";
	if (defines != nullptr)
		code << defines;	// [13b] a variant's defines, before the lump's #line 1
	code << "#line 1\n";
	code << GetStringFromLump(lump).GetChars() << "\n";

	auto program = std::make_unique<VkComputeProgram>();
	program->Name = lumpName;
	if (defines != nullptr && defines[0] != 0)
	{
		// The variant in the name, so the log lines say which one: "lump (TARGET_R8)".
		FString variant = defines;
		variant.Substitute("#define ", "");
		variant.Substitute("\n", " ");
		variant.StripRight();
		program->Name.AppendFormat(" (%s)", variant.GetChars());
	}
	program->PushConstantBytes = pushConstantBytes;

	try
	{
		program->Shader = ShaderBuilder()
			.Type(ShaderType::Compute)
			.AddSource(lumpName, code.GetChars())
			.DebugName(lumpName)
			.OnIncludeLocal(VkShaderManager::OnInclude)
			.OnIncludeSystem(VkShaderManager::OnInclude)
			.Create(lumpName, fb->device.get());

		DescriptorSetLayoutBuilder setBuilder;
		for (const VkComputeBinding& b : bindings)
			setBuilder.AddBinding(b.Binding, b.Type, 1, VK_SHADER_STAGE_COMPUTE_BIT);
		setBuilder.DebugName("VkComputeManager.SetLayout");
		program->SetLayout = setBuilder.Create(fb->device.get());

		PipelineLayoutBuilder layoutBuilder;
		layoutBuilder.AddSetLayout(program->SetLayout.get());
		if (pushConstantBytes > 0)
			layoutBuilder.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantBytes);
		layoutBuilder.DebugName("VkComputeManager.PipelineLayout");
		program->Layout = layoutBuilder.Create(fb->device.get());

		program->Pipeline = ComputePipelineBuilder()
			.Layout(program->Layout.get())
			.ComputeShader(program->Shader.get())
			.DebugName(lumpName)
			.Create(fb->device.get());
	}
	catch (const std::exception& e)
	{
		Printf(TEXTCOLOR_RED "Compute: '%s' could not be built -- that effect stays off:\n%s\n", lumpName, e.what());
		return nullptr;
	}

	// ComputePipelineBuilder::Create does not check vkCreateComputePipelines' result; a
	// failed creation leaves VK_NULL_HANDLE, which is checked here instead.
	if (!program->Pipeline || program->Pipeline->pipeline == VK_NULL_HANDLE)
	{
		Printf(TEXTCOLOR_RED "Compute: '%s' pipeline creation failed -- that effect stays off\n", lumpName);
		return nullptr;
	}

	Printf("Compute: %s ready\n", lumpName);
	return program;
}

std::unique_ptr<VulkanDescriptorSet> VkComputeManager::AllocateSet(VkComputeProgram* program)
{
	if (program == nullptr || !program->SetLayout)
		return nullptr;

	for (auto it = mPools.rbegin(); it != mPools.rend(); ++it)
	{
		auto set = (*it)->tryAllocate(program->SetLayout.get());
		if (set)
			return set;
	}

	// A fresh pool. Sized for a handful of volume programs' sets; freed sets return to
	// their own pool (ZVulkan pools free individual sets), so this rarely grows.
	mPools.push_back(DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16)
		.MaxSets(32)
		.DebugName("VkComputeManager.DescriptorPool")
		.Create(fb->device.get()));

	return mPools.back()->tryAllocate(program->SetLayout.get());
}

bool VkComputeManager::IsVolumeFormatSupported(VkFormat format, int width, int height, int depth, VkImageUsageFlags usage, bool linearFilter)
{
	ImageBuilder builder;
	builder.Size3D(width, height, depth);
	builder.Format(format);
	builder.Usage(usage);
	if (!builder.IsFormatSupported(fb->device.get()))
		return false;

	VkFormatFeatureFlags needed = 0;
	if (usage & VK_IMAGE_USAGE_STORAGE_BIT) needed |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
	if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) needed |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	if (linearFilter) needed |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

	VkFormatProperties properties = {};
	vkGetPhysicalDeviceFormatProperties(fb->device->PhysicalDevice.Device, format, &properties);
	return (properties.optimalTilingFeatures & needed) == needed;
}

VulkanSampler* VkComputeManager::GetVolumeSampler()
{
	if (!mVolumeSampler)
	{
		mVolumeSampler = SamplerBuilder()
			.MagFilter(VK_FILTER_LINEAR)
			.MinFilter(VK_FILTER_LINEAR)
			.MipmapMode(VK_SAMPLER_MIPMAP_MODE_NEAREST)
			.AddressMode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
			.MaxLod(0.25f)
			.DebugName("VkComputeManager.VolumeSampler")
			.Create(fb->device.get());
	}
	return mVolumeSampler.get();
}

void VkComputeManager::Dispatch(VkComputeProgram* program, VulkanDescriptorSet* set, const void* pushConstants, uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
{
	if (program == nullptr || set == nullptr || !program->Pipeline)
		return;

	BeginWork();

	VulkanCommandBuffer* cmd = fb->GetCommands()->GetDrawCommands();
	cmd->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, program->Pipeline.get());
	cmd->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, program->Layout.get(), 0, set);
	if (program->PushConstantBytes > 0 && pushConstants != nullptr)
		cmd->pushConstants(program->Layout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, program->PushConstantBytes, pushConstants);
	cmd->dispatch(groupsX, groupsY, groupsZ);
}
