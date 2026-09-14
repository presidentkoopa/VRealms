/*
** vk_renderdevice.cpp
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

#include <cinttypes>

#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkancompatibledevice.h>
#include <zvulkan/vulkanobjects.h>
#include <zvulkan/vulkansurface.h>

#include <inttypes.h>
#include <limits>
#include <thread>

#include "c_dispatch.h"
#include "flatvertices.h"
#include "hw_bonebuffer.h"
#include "hw_drawnlinebuffer.h"	// [DRAWNLINES]
#include "hw_gpuparticlebuffer.h"	// [GPUPARTICLES]
#include "hw_particledefbuffer.h"	// [PARTICLEDEFS]
#include "hw_viewlightbuffer.h"	// [VIEWLIGHTS]
#include "hw_sectorplanebuffer.h"	// [SECTORPLANES]
#include "hw_framecompute.h"	// [COMPUTE]
#include "vulkan/renderer/vk_compute.h"	// [COMPUTE]
#include "hw_clock.h"
#include "hw_lightbuffer.h"
#include "hw_skydome.h"
#include "hw_vrmodes.h"	// UZDXREMA: VRMode::GetVRModeCached / VR_OPENXR_MOBILE
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "m_png.h"
#include "printf.h"
#include "v_draw.h"
#include "v_video.h"
#include "version.h"
#include "vk_renderdevice.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vulkan/renderer/vk_raytrace.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/system/vk_buffer.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_texture.h"

// [UZDXREMA] fork-only headers: VR/OpenXR presentation, video scaling and model support.
#include <zvulkan/vulkanswapchain.h>
#include "engineerrors.h"
#include "i_time.h"
#include "model.h"
#include "r_videoscale.h"
#include "v_text.h"
#include "common/rendering/stereo3d/openxr/oxr_loader.h"

EXTERN_CVAR(Bool, vr_openxr_multiview_postprocess);
EXTERN_CVAR(Int, vr_openxr_sync_mode);

extern bool cinemamode;

FString JitCaptureStackTrace(int framesToSkip, bool includeNativeFrames, int maxFrames = -1);

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)
EXTERN_CVAR(Int, vr_mode)
EXTERN_CVAR(Bool, gl_texture_thread)
EXTERN_CVAR(Bool, gl_texture_thread_models)
EXTERN_CVAR(Bool, gl_texture_thread_upload)
EXTERN_CVAR(Int, gl_texture_thread_workers)
EXTERN_CVAR(Int, gl_background_flush_count)
EXTERN_CVAR(Int, vk_max_transfer_threads)

CVAR(Bool, vk_raytrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// RS FORK -- GPU STABILITY DIAGNOSTICS. Both are read when the device is
// created, so a change takes effect on the next restart. The driver's own fault
// report has no switch: it costs nothing until the GPU is lost, so it is always
// asked for.
//
//   vk_gpu_checkpoints       mark every render pass in the command stream, so a
//                            device loss names the pass the GPU was in. On by
//                            default: it only ever reports, and it costs one
//                            marker per pass.
//   vk_robust_buffer_access  out-of-range buffer reads return zero instead of
//                            faulting the GPU. CHANGES BEHAVIOUR -- a test for
//                            "is this an out-of-bounds read", never a fix.
CVAR(Bool, vk_gpu_checkpoints, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vk_robust_buffer_access, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Physical device info
static std::vector<VulkanCompatibleDevice> SupportedDevices;
int vkversion;

CUSTOM_CVAR(Bool, vk_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CVAR(Bool, vk_debug_callstack, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, vk_device, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CCMD(vk_listdevices)
{
	for (size_t i = 0; i < SupportedDevices.size(); i++)
	{
		Printf("#%d - %s\n", (int)i, SupportedDevices[i].Device->Properties.Properties.deviceName);
	}
}

void VulkanError(const char* text)
{
	throw CVulkanError(text);
}

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	bool showcallstack = strstr(typestr, "error") != nullptr;

	if (showcallstack)
		Printf("\n");

	Printf(TEXTCOLOR_RED "[%s] ", typestr);
	Printf(TEXTCOLOR_WHITE "%s\n", msg.c_str());

	if (vk_debug_callstack && showcallstack)
	{
		FString callstack = JitCaptureStackTrace(0, true, 5);
		if (!callstack.IsEmpty())
			Printf("%s\n", callstack.GetChars());
	}
}

bool VkTexLoadThread::loadResource(VkTexLoadIn& input, VkTexLoadOut& output)
{
	if (input.texture == nullptr || input.hardwareTexture == nullptr)
	{
		return false;
	}

	auto texBuffer = input.texture->CreateTexBuffer(input.translation, input.scaleFlags | CTF_ProcessData);
	output.pixels = std::shared_ptr<uint8_t>(texBuffer.mBuffer, std::default_delete<uint8_t[]>());
	texBuffer.mBuffer = nullptr;
	output.width = texBuffer.mWidth;
	output.height = texBuffer.mHeight;
	output.scaleFlags = input.scaleFlags;
	output.hardwareTexture = input.hardwareTexture;

	const bool indexed = !!(input.scaleFlags & CTF_Indexed);
	const bool canUploadInThread = cmd != nullptr && uploadQueue.familySupportsGraphics;
	if (canUploadInThread && output.pixels != nullptr)
	{
		output.hardwareTexture->BackgroundCreateTexture(
			cmd,
			output.width,
			output.height,
			indexed ? 1 : 4,
			indexed ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM,
			output.pixels.get(),
			indexed ? 0 : -1,
			!indexed);
		output.hardwareTexture->CheckFinalTransition(cmd->GetTransferCommands(), true);
		cmd->WaitForCommands(false, true);
		output.uploadedInThread = true;
		output.needsQueueOwnershipTransfer = uploadQueue.queueFamily >= 0 && uploadQueue.queueFamily != cmd->GetRenderDevice()->device->GraphicsFamily;
		output.uploadQueueFamily = uploadQueue.queueFamily;

		if (output.needsQueueOwnershipTransfer)
		{
			auto releaseCommands = cmd->CreateUnmanagedCommands();
			output.hardwareTexture->ReleaseLoadedFromQueue(releaseCommands.get(), output.uploadQueueFamily, cmd->GetRenderDevice()->device->GraphicsFamily);
			releaseCommands->end();

			auto releaseFence = std::make_unique<VulkanFence>(cmd->GetRenderDevice()->device.get());
			QueueSubmit releaseSubmit;
			releaseSubmit.AddCommandBuffer(releaseCommands.get());
			releaseSubmit.Execute(cmd->GetRenderDevice()->device.get(), uploadQueue.queue, releaseFence.get());

			vkWaitForFences(cmd->GetRenderDevice()->device->device, 1, &releaseFence->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
			vkResetFences(cmd->GetRenderDevice()->device->device, 1, &releaseFence->fence);
		}

		output.pixels.reset();
	}

	return output.pixels != nullptr || output.uploadedInThread;
}

bool VkModelLoadThread::loadResource(VkModelLoadIn& input, VkModelLoadOut& output)
{
	if (input.model == nullptr || input.lump < 0)
	{
		return false;
	}

	FileReader reader;
	try
	{
		reader = fileSystem.OpenFileReader(input.lump, FileSys::EReaderType::READER_NEW, 0);
	}
	catch (...)
	{
		return false;
	}

	if (!reader.isOpen())
	{
		return false;
	}

	output.data = reader.Read();
	reader.Close();
	output.lump = input.lump;
	output.model = input.model;
	return true;
}

VulkanRenderDevice::VulkanRenderDevice(void *hMonitor, bool fullscreen, std::shared_ptr<VulkanSurface> surface) :
	Super(hMonitor, fullscreen)
{
	VulkanDeviceBuilder builder;
	builder.OptionalRayQuery();
	// RS FORK -- see the diagnostics cvars above.
	builder.OptionalDeviceFaultReport();
	if (vk_gpu_checkpoints) builder.OptionalGpuCheckpoints();
	if (vk_robust_buffer_access) builder.OptionalRobustBufferAccess();
	builder.Surface(surface);
	builder.SelectDevice(vk_device);
	if (vr_mode == VR_OPENXR_MOBILE)
	{
		OpenXRVulkanBootstrapInfo xrInfo;
		if (QueryOpenXRVulkanBootstrapInfo(xrInfo))
		{
			for (const auto& ext : xrInfo.requiredDeviceExtensions)
				builder.RequireExtension(ext);
		}

		VkPhysicalDevice preferredDevice = VK_NULL_HANDLE;
		if (surface != nullptr && surface->Instance != nullptr && QueryOpenXRVulkanPreferredPhysicalDevice(surface->Instance->Instance, preferredDevice))
		{
			builder.PreferredPhysicalDevice(preferredDevice);
		}
	}
	SupportedDevices = builder.FindDevices(surface->Instance);
	device = builder.Create(surface->Instance, gl_texture_thread ? vk_max_transfer_threads : 0, 0);

	// RS FORK -- SAY WHAT IS WATCHING, AND WHAT TO DO WHEN THE GPU DIES.
	//
	// The banner exists because a report that is off looks exactly like a
	// report that found nothing, and the one time it matters is the one time
	// nobody can go back and check.
	Printf("Vulkan diagnostics: driver fault report %s, GPU checkpoints %s, robust buffer access %s\n",
		(device->SupportsExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME) && device->EnabledFeatures.Fault.deviceFault) ? "ON" : "unavailable",
		device->SupportsExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) ? "ON" : (vk_gpu_checkpoints ? "unavailable" : "off"),
		device->EnabledFeatures.Features.robustBufferAccess ? "ON" : "off");

	// Printf, not a dialog: the log is flushed line by line (c_console.cpp), so
	// this survives the process being killed -- which is often how a device
	// loss ends, with the OpenXR runtime taking the game down before the
	// exception ever reaches a message box. That is how a week of GPU resets
	// went by with nothing in any log.
	std::weak_ptr<VulkanDevice> weakDevice = device;
	VulkanSetDeviceLostHandler([weakDevice](const char* where)
	{
		Printf(TEXTCOLOR_RED "\n==================== GPU DEVICE LOST ====================\n");
		Printf("Noticed at: %s\n", where);
		Printf("Last render pass the CPU opened: %s\n", VkCommandBufferManager::LastGroupLabel());
		if (auto dev = weakDevice.lock())
			Printf("%s", dev->DescribeDeviceLoss().c_str());
		Printf(TEXTCOLOR_RED "==================== end of GPU report ====================\n");
	});

	if (gl_texture_thread)
	{
		modelThread = std::make_unique<VkModelLoadThread>(&modelInQueue, &modelOutQueue);
		unsigned int cpuThreadCount = std::max(1u, std::thread::hardware_concurrency());
		cpuThreadCount = cpuThreadCount > 1 ? cpuThreadCount - 1 : 1;
		cpuThreadCount = std::min(cpuThreadCount, (unsigned int)gl_texture_thread_workers);
		unsigned int threadCount = cpuThreadCount;
		const bool useUploadQueues = gl_texture_thread_upload && !device->uploadQueues.empty();

		if (useUploadQueues)
		{
			threadCount = std::min<unsigned int>(threadCount, (unsigned int)device->uploadQueues.size());
			bgTransferThreads.reserve(threadCount);
			mBGTransferCommands.reserve(threadCount);
			for (unsigned int i = 0; i < threadCount; i++)
			{
				const auto& slot = device->uploadQueues[i];
				if (!slot.familySupportsGraphics)
				{
					break;
				}

				auto bgCommands = std::make_unique<VkCommandBufferManager>(this, &device->uploadQueues[i].queue, device->uploadQueues[i].queueFamily, true);
				auto worker = std::make_unique<VkTexLoadThread>(bgCommands.get(), device.get(), (int)i, &primaryTexQueue, &secondaryTexQueue, &outputTexQueue);
				worker->start();
				mBGTransferCommands.push_back(std::move(bgCommands));
				bgTransferThreads.push_back(std::move(worker));
			}
		}

		if (bgTransferThreads.empty())
		{
			threadCount = std::max(1u, cpuThreadCount);
			bgTransferThreads.reserve(threadCount);
			for (unsigned int i = 0; i < threadCount; i++)
			{
				auto worker = std::make_unique<VkTexLoadThread>(nullptr, device.get(), -1, &primaryTexQueue, &secondaryTexQueue, &outputTexQueue);
				worker->start();
				bgTransferThreads.push_back(std::move(worker));
			}
		}
		modelThread->start();
		bgTransferEnabled = !bgTransferThreads.empty();

		if (gl_texture_thread_upload && mBGTransferCommands.empty() && !device->uploadQueues.empty())
		{
			Printf("Asset streaming upload queues requested, but available upload queues do not support graphics. Falling back to load-only worker threads.\n");
		}
	}
}

VulkanRenderDevice::~VulkanRenderDevice()
{
	// RS FORK -- cleared first: a restart builds a new device and registers its
	// own handler, and this one must not answer for it.
	VulkanSetDeviceLostHandler(nullptr);
	StopBackgroundCache();
	vkDeviceWaitIdle(device->device); // make sure the GPU is no longer using any objects before RAII tears them down
	// [COMPUTE] First, while everything it records through still exists. The GPU is idle
	// now, so its images, descriptor sets, pipelines and pools are destroyed directly.
	mCompute.reset();
	PPResource::ResetAll();

	delete mVertexData;
	delete mSkyData;
	delete mViewpoints;
	delete mLights;
	delete mBones;
	// [GPUPARTICLES] beside the bones, which it is created beside
	delete mGpuParticles;
	mGpuParticles = nullptr;
	// [DRAWNLINES] beside the particles, which it is created beside
	delete mDrawnLines;
	mDrawnLines = nullptr;
	// [PARTICLEDEFS] beside the particles, which index it
	delete mParticleDefinitions;
	mParticleDefinitions = nullptr;
	// [VIEWLIGHTS] beside the particles, which read it
	delete mViewLights;
	mViewLights = nullptr;
	// [SECTORPLANES] beside the view lights, which it is created beside
	delete mSectorPlanes;
	mSectorPlanes = nullptr;
	mShadowMap.Reset();

	if (mDescriptorSetManager)
		mDescriptorSetManager->Deinit();
	if (mTextureManager)
		mTextureManager->Deinit();
	if (mBufferManager)
		mBufferManager->Deinit();
	if (mShaderManager)
		mShaderManager->Deinit();

	mCommands->DeleteFrameObjects();
	for (auto& bgCommands : mBGTransferCommands)
	{
		bgCommands->DeleteFrameObjects();
	}
}

void VulkanRenderDevice::InitializeState()
{
	static bool first = true;
	if (first)
	{
		PrintStartupLog();
		first = false;
	}

	// Use the same names here as OpenGL returns.
	switch (device->PhysicalDevice.Properties.Properties.vendorID)
	{
	case 0x1002: vendorstring = "ATI Technologies Inc.";     break;
	case 0x10DE: vendorstring = "NVIDIA Corporation";  break;
	case 0x8086: vendorstring = "Intel";   break;
	default:     vendorstring = "Unknown"; break;
	}

	hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
	glslversion = 4.50f;
	uniformblockalignment = (unsigned int)device->PhysicalDevice.Properties.Properties.limits.minUniformBufferOffsetAlignment;
	maxuniformblock = device->PhysicalDevice.Properties.Properties.limits.maxUniformBufferRange;

	mCommands.reset(new VkCommandBufferManager(this, &device->GraphicsQueue, device->GraphicsFamily));

	mSamplerManager.reset(new VkSamplerManager(this));
	mTextureManager.reset(new VkTextureManager(this));
	mFramebufferManager.reset(new VkFramebufferManager(this));
	mBufferManager.reset(new VkBufferManager(this));
	mBufferManager->Init();

	mScreenBuffers.reset(new VkRenderBuffers(this));
	mSaveBuffers.reset(new VkRenderBuffers(this));
	mActiveRenderBuffers = mScreenBuffers.get();

	mPostprocess.reset(new VkPostprocess(this));
	mDescriptorSetManager.reset(new VkDescriptorSetManager(this));
	mRenderPassManager.reset(new VkRenderPassManager(this));
	mRaytrace.reset(new VkRaytrace(this));

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight());
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer;
	mLights = new FLightBuffer();
	mBones = new BoneBuffer();
	// [GPUPARTICLES] Vulkan only (OpenXR only exists here, and useSSBO() is
	// unconditionally true). Must exist before mDescriptorSetManager->Init()
	// below, which writes it to set 1 binding 5.
	mGpuParticles = new GpuParticleBuffer();
	// [DRAWNLINES] Beside the particles and for the same reasons; set 1 binding 6.
	mDrawnLines = new DrawnLineBuffer();
	// [PARTICLEDEFS] The definitions the particle records index; set 1 binding 7.
	// Filled from the CPU table (gamedata/particledefs.cpp) by ProcessScene.
	mParticleDefinitions = new ParticleDefinitionBuffer();
	// [VIEWLIGHTS] The dynamic lights in view, which lit particles loop over in the
	// vertex shader; set 1 binding 8. Filled every main-view scene by
	// HWDrawInfo::ProcessScene (SyncViewLights).
	mViewLights = new ViewLightBuffer();
	// [SECTORPLANES] Sectors' current floor and ceiling planes for GPU effects; set 1
	// binding 12. Written by SectorPlanes (hw_sectorplanes.cpp) only for the sectors an
	// active effect polls.
	mSectorPlanes = new SectorPlaneBuffer();

	mShaderManager.reset(new VkShaderManager(this));
	mDescriptorSetManager->Init();
#ifdef __APPLE__
	mRenderState.reset(new VkRenderStateMolten(this));
#else
	mRenderState.reset(new VkRenderState(this));
#endif

	// [COMPUTE] GPU compute for effects. Creates nothing until an effect asks for it.
	mCompute.reset(new VkComputeManager(this));
}

// [COMPUTE] See DFrameBuffer::RunFrameCompute and VkComputeManager::RunFrame.
void VulkanRenderDevice::RunFrameCompute(const FrameComputeInput& input)
{
	if (mCompute)
		mCompute->RunFrame(input);
}

void VulkanRenderDevice::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();

	const auto vrmode = VRMode::GetVRModeCached(true);
	auto* postprocess = GetPostprocess();
	if (postprocess)
	{
		if (vrmode != nullptr && vrmode->IsVR())
		{
			const int eyeCount = vrmode->mEyeCount > 0 ? vrmode->mEyeCount : 1;
			const bool suppressSceneEye2D = vrmode->ShouldUseScreenLayerForCurrentFrame() || cinemamode;
			const bool useGameplayEyeViewport = vrmode->ShouldUseRecommendedRenderSizeThisFrame() && !vrmode->IsRenderingVirtualScreen();
			const bool useSharedMultiviewPostprocess =
				mXRFrameBeganThisFrame &&
				vrmode->ShouldUseMultiviewThisFrame() &&
				vr_openxr_multiview_postprocess &&
				GetBuffers()->GetPipelineLayers() > 1;
			const IntRect savedScreenViewport = mScreenViewport;
			VREyeComposite.Clock();
			for (int eye_ix = 0; eye_ix < eyeCount; ++eye_ix)
			{
				mCurrentEyeIndex = eye_ix;
				const auto eye = (eye_ix >= 0 && eye_ix < 2) ? vrmode->mEyes[eye_ix] : nullptr;
				const int pipelinePairStart = useSharedMultiviewPostprocess ? 0 : (eye_ix % 2) * 2;
				const int pipelineImageIndex = useSharedMultiviewPostprocess ? mEyeFinalPipelineImage[0] : mEyeFinalPipelineImage[eye_ix % 2];
				postprocess->SetPipelineImagePair(pipelinePairStart, 2);
				postprocess->SetCurrentPipelineImage(pipelineImageIndex);
				if (useGameplayEyeViewport)
				{
					mScreenViewport = mSceneViewport;
				}
				postprocess->SetActiveRenderTarget();
				if (eye != nullptr)
				{
					eye->AdjustBlend(nullptr);
				}
				if (!suppressSceneEye2D && twod != nullptr && twod->HasCommandsForPass(true))
				{
					Draw2D(true);
				}
				if (!vrmode->ShouldUseScreenLayerForCurrentFrame() && !vrmode->IsRenderingVirtualScreen() && eye != nullptr)
				{
					eye->AdjustHud();
				}
				if (!suppressSceneEye2D && twod != nullptr && twod->HasCommandsForPass(false))
				{
					Draw2D(false);
				}
				vrmode->FinalizeEyeImage(this, eye_ix);
			}
			VREyeComposite.Unclock();
			mCurrentEyeIndex = 0;
			mScreenViewport = savedScreenViewport;
			vrmode->RenderVirtualScreen();
			twod->Clear();
		}
		else
		{
			postprocess->SetActiveRenderTarget();
			Draw2D();
			twod->Clear();
		}
	}

	mRenderState->EndRenderPass();
	mRenderState->EndFrame();

	Flush3D.Unclock();

	const bool trackXrSyncWait = vrmode != nullptr && vrmode->IsVR() && mXRFrameBeganThisFrame;
	const bool deferDesktopPresentForXr = trackXrSyncWait && vr_openxr_sync_mode == 1;
	if (trackXrSyncWait)
	{
		Clocker renderSyncTimer(VRRenderSyncWait);
		mCommands->WaitForCommands(!deferDesktopPresentForXr, false, !deferDesktopPresentForXr);
	}
	else
	{
		mCommands->WaitForCommands(true);
	}
	mCommands->UpdateGpuStats();

	if (vrmode != nullptr && vrmode->IsVR() && mXRFrameBeganThisFrame)
	{
		vrmode->AcquireXRSwapchain();
		if (deferDesktopPresentForXr)
		{
			mFramebufferManager->AcquireImage();
			mCommands->WaitForCommands(true, false, false);
		}
	}
	mXRFrameBeganThisFrame = false;

	Super::Update();
}

bool VulkanRenderDevice::CompileNextShader()
{
	return mShaderManager->CompileNextShader();
}

void VulkanRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	auto BaseLayer = static_cast<VkHardwareTexture*>(tex->GetHardwareTexture(0, 0));

	VkTextureImage *image = BaseLayer->GetImage(tex, 0, 0);
	VkTextureImage *depthStencil = BaseLayer->GetDepthStencil(tex);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(image, depthStencil->GetFramebufferView(), image->Image->width, image->Image->height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = min(tex->GetWidth(), image->Image->width);
	bounds.height = min(tex->GetHeight(), image->Image->height);

	renderFunc(bounds);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	SetSceneRenderTarget(false);

	tex->SetUpdated(true);
}

void VulkanRenderDevice::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) mPostprocess->BlitSceneToPostprocess(); // Copy the resulting scene to the current post process texture
	mPostprocess->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
	const auto vrmode = VRMode::GetVRModeCached(true);
	const bool useSharedMultiviewPostprocess =
		vrmode != nullptr &&
		vrmode->IsVR() &&
		mXRFrameBeganThisFrame &&
		vrmode->ShouldUseMultiviewThisFrame() &&
		vr_openxr_multiview_postprocess &&
		GetBuffers()->GetPipelineLayers() > 1;
	if (useSharedMultiviewPostprocess)
	{
		const int finalPipelineImage = mPostprocess->GetCurrentPipelineImage();
		mEyeFinalPipelineImage[0] = finalPipelineImage;
		mEyeFinalPipelineImage[1] = finalPipelineImage;
	}
	else
	{
		mEyeFinalPipelineImage[mCurrentEyeIndex % 2] = mPostprocess->GetCurrentPipelineImage();
	}
}

const char* VulkanRenderDevice::DeviceName() const
{
	return device->PhysicalDevice.Properties.Properties.deviceName;
}

void VulkanRenderDevice::SetVSync(bool vsync)
{
	mVSync = vsync;
}

void VulkanRenderDevice::NewRefreshRate()
{
	const auto vrmode = VRMode::GetVRModeCached(true);
	if (vrmode != nullptr && vrmode->IsVR())
	{
		vrmode->ApplyRefreshRate();
	}
}

void VulkanRenderDevice::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	MaterialLayerInfo* layer;

	auto systex = static_cast<VkHardwareTexture*>(mat->GetLayer(0, translation, &layer));
	systex->GetImage(layer->layerTexture, translation, layer->scaleFlags);

	int numLayers = mat->NumLayers();
	for (int i = 1; i < numLayers; i++)
	{
		auto syslayer = static_cast<VkHardwareTexture*>(mat->GetLayer(i, 0, &layer));
		syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
	}
}

void VulkanRenderDevice::PrequeueMaterial(FMaterial *mat, int translation)
{
	BackgroundCacheMaterial(mat, FTranslationID::fromInt(translation), false, true);
}

bool VulkanRenderDevice::BackgroundCacheTextureMaterial(FGameTexture *tex, FTranslationID translation, int scaleFlags, bool makeSPI)
{
	if (!bgTransferEnabled || tex == nullptr || !tex->isValid())
	{
		return false;
	}

	patchQueue.deleteSearch([&](QueuedPatch& queued)
	{
		return queued.tex == tex &&
			queued.translation == translation.index() &&
			queued.scaleFlags == scaleFlags;
	});

	QueuedPatch patch;
	patch.tex = tex;
	patch.translation = translation.index();
	patch.scaleFlags = scaleFlags;
	patch.secondary = false;
	patchQueue.queue(std::move(patch));
	return true;
}

bool VulkanRenderDevice::BackgroundLoadModel(FModel* model)
{
	if (!bgTransferEnabled || !gl_texture_thread_models || model == nullptr || model->GetLumpNum() < 0)
	{
		return false;
	}

	if (model->GetLoadState() == FModel::READY)
	{
		return false;
	}

	if (model->GetLoadState() == FModel::LOADING)
	{
		return true;
	}

	model->SetLoadState(FModel::LOADING);

	VkModelLoadIn input;
	input.lump = model->GetLumpNum();
	input.model = model;
	modelInQueue.queue(std::move(input));
	if (modelThread) modelThread->wake();
	return true;
}

bool VulkanRenderDevice::BackgroundCacheMaterial(FMaterial *mat, FTranslationID translation, bool makeSPI, bool secondary)
{
	if (!bgTransferEnabled || mat == nullptr || mat->Source()->GetUseType() == ETextureType::SWCanvas)
	{
		return false;
	}

	MaterialLayerInfo* layer = nullptr;
	auto baseLayer = static_cast<VkHardwareTexture*>(mat->GetLayer(0, translation.index(), &layer));
	if (layer != nullptr && layer->layerTexture != nullptr)
	{
		if (!secondary && baseLayer->GetState() == IHardwareTexture::CACHING)
		{
			VkTexLoadIn input;
			if (secondaryTexQueue.dequeueSearch(input, [&](VkTexLoadIn& queued)
			{
				return queued.hardwareTexture == baseLayer;
			}))
			{
				baseLayer->SetHardwareState(IHardwareTexture::LOADING);
				primaryTexQueue.queue(std::move(input));
				for (auto& thread : bgTransferThreads) thread->wake();
			}
		}
		else if (baseLayer->GetState() == IHardwareTexture::NONE)
		{
			baseLayer->SetHardwareState(secondary ? IHardwareTexture::CACHING : IHardwareTexture::LOADING);
			VkTexLoadIn input;
			input.texture = layer->layerTexture;
			input.translation = translation.index();
			input.scaleFlags = layer->scaleFlags;
			input.hardwareTexture = baseLayer;
			if (secondary) secondaryTexQueue.queue(std::move(input));
			else primaryTexQueue.queue(std::move(input));
			for (auto& thread : bgTransferThreads) thread->wake();
		}
	}

	for (int i = 1; i < mat->NumLayers(); i++)
	{
		auto extraLayer = static_cast<VkHardwareTexture*>(mat->GetLayer(i, 0, &layer));
		if (layer != nullptr && layer->layerTexture != nullptr)
		{
			if (!secondary && extraLayer->GetState() == IHardwareTexture::CACHING)
			{
				VkTexLoadIn input;
				if (secondaryTexQueue.dequeueSearch(input, [&](VkTexLoadIn& queued)
				{
					return queued.hardwareTexture == extraLayer;
				}))
				{
					extraLayer->SetHardwareState(IHardwareTexture::LOADING);
					primaryTexQueue.queue(std::move(input));
					for (auto& thread : bgTransferThreads) thread->wake();
				}
			}
			else if (extraLayer->GetState() == IHardwareTexture::NONE)
			{
				extraLayer->SetHardwareState(secondary ? IHardwareTexture::CACHING : IHardwareTexture::LOADING);
				VkTexLoadIn input;
				input.texture = layer->layerTexture;
				input.translation = 0;
				input.scaleFlags = layer->scaleFlags;
				input.hardwareTexture = extraLayer;
				if (secondary) secondaryTexQueue.queue(std::move(input));
				else primaryTexQueue.queue(std::move(input));
				for (auto& thread : bgTransferThreads) thread->wake();
			}
		}
	}

	return true;
}

bool VulkanRenderDevice::CachingActive()
{
	if (!bgTransferEnabled)
	{
		return false;
	}

	if (primaryTexQueue.size() > 0 || secondaryTexQueue.size() > 0 || outputTexQueue.size() > 0 || patchQueue.size() > 0)
	{
		return true;
	}

	if (modelInQueue.size() > 0 || modelOutQueue.size() > 0)
	{
		return true;
	}

	for (auto& thread : bgTransferThreads)
	{
		if (thread->isActive())
		{
			return true;
		}
	}
	if (modelThread && modelThread->isActive())
	{
		return true;
	}
	return false;
}

float VulkanRenderDevice::CacheProgress()
{
	return (float)(primaryTexQueue.size() + secondaryTexQueue.size() + patchQueue.size() + modelInQueue.size());
}

void VulkanRenderDevice::StopBackgroundCache()
{
	primaryTexQueue.clear();
	secondaryTexQueue.clear();
	patchQueue.clear();
	modelInQueue.clear();

	for (auto& thread : bgTransferThreads)
	{
		thread->stop();
	}
	bgTransferThreads.clear();
	mBGTransferCommands.clear();
	if (modelThread)
	{
		modelThread->stop();
	}
	modelOutQueue.clear();
	outputTexQueue.clear();
	bgTransferEnabled = false;
}

void VulkanRenderDevice::FlushBackground()
{
	if (!bgTransferEnabled)
	{
		return;
	}

	UpdateBackgroundCache(true);
	UploadLoadedTextures(true);
}

void VulkanRenderDevice::UploadLoadedTextures(bool flush)
{
	int uploadBudget = flush ? std::numeric_limits<int>::max() : std::max(1, (int)gl_background_flush_count);

	VkTexLoadOut loaded;
	while (uploadBudget > 0 && outputTexQueue.dequeue(loaded))
	{
		if (loaded.hardwareTexture == nullptr)
		{
			uploadBudget--;
			continue;
		}

		if (loaded.needsQueueOwnershipTransfer)
		{
			loaded.hardwareTexture->AcquireLoadedFromQueue(mCommands->GetTransferCommands(), loaded.uploadQueueFamily, device->GraphicsFamily);
		}

		if (loaded.uploadedInThread)
		{
			loaded.hardwareTexture->SetHardwareState(IHardwareTexture::READY);
		}
		else if (loaded.pixels != nullptr)
		{
			const bool indexed = !!(loaded.scaleFlags & CTF_Indexed);
			loaded.hardwareTexture->BackgroundCreateTexture(
				mCommands.get(),
				loaded.width,
				loaded.height,
				indexed ? 1 : 4,
				indexed ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM,
				loaded.pixels.get(),
				indexed ? 0 : -1,
				!indexed);
			loaded.hardwareTexture->CheckFinalTransition(mCommands->GetTransferCommands(), true);
			loaded.hardwareTexture->SetHardwareState(IHardwareTexture::READY);
		}
		uploadBudget--;
	}
}

void VulkanRenderDevice::UpdateBackgroundCache(bool flush)
{
	if (!bgTransferEnabled)
	{
		return;
	}

	auto processPendingWork = [&]()
	{
		QueuedPatch patch;
		while (patchQueue.dequeue(patch))
		{
			if (patch.tex != nullptr)
			{
				auto material = FMaterial::ValidateTexture(patch.tex, patch.scaleFlags, true);
				if (material != nullptr)
				{
					BackgroundCacheMaterial(material, FTranslationID::fromInt(patch.translation), false, patch.secondary);
				}
			}
		}

		VkModelLoadOut modelOut;
		while (modelOutQueue.dequeue(modelOut))
		{
			if (modelOut.model == nullptr)
			{
				continue;
			}

			if (modelOut.model->GetLoadState() != FModel::LOADING)
			{
				modelOut.data.clear();
				continue;
			}

			modelOut.model->LoadGeometry(&modelOut.data);
			modelOut.model->SetLoadState(FModel::READY);
			modelOut.data.clear();
		}
	};

	processPendingWork();

	if (flush)
	{
		while (CachingActive())
		{
			processPendingWork();
			UploadLoadedTextures(true);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return;
	}

	UploadLoadedTextures(false);
}

IHardwareTexture *VulkanRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new VkHardwareTexture(this, numchannels);
}

FMaterial* VulkanRenderDevice::CreateMaterial(FGameTexture* tex, int scaleflags)
{
	return new VkMaterial(this, tex, scaleflags);
}

IVertexBuffer *VulkanRenderDevice::CreateVertexBuffer()
{
	return GetBufferManager()->CreateVertexBuffer();
}

IIndexBuffer *VulkanRenderDevice::CreateIndexBuffer()
{
	return GetBufferManager()->CreateIndexBuffer();
}

IDataBuffer *VulkanRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return GetBufferManager()->CreateDataBuffer(bindingpoint, ssbo, needsresize);
}

void VulkanRenderDevice::SetTextureFilterMode()
{
	if (mSamplerManager)
	{
		mDescriptorSetManager->ResetHWTextureSets();
		mSamplerManager->ResetHWSamplers();
	}
}

void VulkanRenderDevice::StartPrecaching()
{
	// Destroy the texture descriptors to avoid problems with potentially stale textures.
	mDescriptorSetManager->ResetHWTextureSets();
}

void VulkanRenderDevice::BlurScene(float amount)
{
	if (mPostprocess)
		mPostprocess->BlurScene(amount);
}

void VulkanRenderDevice::UpdatePalette()
{
	if (mPostprocess)
		mPostprocess->ClearTonemapPalette();
}

FTexture *VulkanRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");

	return tex;
}

FTexture *VulkanRenderDevice::WipeEndScreen()
{
	GetPostprocess()->SetActiveRenderTarget();
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");

	return tex;
}

void VulkanRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data)
{
	VkTextureImage image;

	// Convert from rgba16f to rgba8 using the GPU:
	image.Image = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.Size(w, h)
		.DebugName("CopyScreenToBuffer")
		.Create(device.get());

	GetPostprocess()->BlitCurrentToImage(&image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// Staging buffer for download
	auto staging = BufferBuilder()
		.Size(w * h * 4)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("CopyScreenToBuffer")
		.Create(device.get());

	// Copy from image to buffer
	VkBufferImageCopy region = {};
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	mCommands->GetDrawCommands()->copyImageToBuffer(image.Image->image, image.Layout, staging->buffer, 1, &region);

	// Submit command buffers and wait for device to finish the work
	mCommands->WaitForCommands(false);

	// Map and convert from rgba8 to rgb8
	uint8_t *dest = (uint8_t*)data;
	uint8_t *pixels = (uint8_t*)staging->Map(0, w * h * 4);
	int dindex = 0;
	for (int y = 0; y < h; y++)
	{
		int sindex = (h - y - 1) * w * 4;
		for (int x = 0; x < w; x++)
		{
			dest[dindex] = pixels[sindex];
			dest[dindex + 1] = pixels[sindex + 1];
			dest[dindex + 2] = pixels[sindex + 2];
			dindex += 3;
			sindex += 4;
		}
	}
	staging->Unmap();
}

void VulkanRenderDevice::SetActiveRenderTarget()
{
	mPostprocess->SetActiveRenderTarget();
}

void VulkanRenderDevice::FirstEye()
{
	if (mPostprocess)
	{
		mCurrentEyeIndex = 0;
		mPostprocess->SetPipelineImagePair(0, 2);
		mPostprocess->SetCurrentPipelineImage(0);
	}
}

void VulkanRenderDevice::NextEye(int eyecount)
{
	if (mPostprocess)
	{
		const auto vrmode = VRMode::GetVRModeCached(true);
		const bool useSharedMultiviewPostprocess =
			vrmode != nullptr &&
			vrmode->IsVR() &&
			mXRFrameBeganThisFrame &&
			vrmode->ShouldUseMultiviewThisFrame() &&
			vr_openxr_multiview_postprocess &&
			GetBuffers()->GetPipelineLayers() > 1;

		if (eyecount > 1)
		{
			mCurrentEyeIndex = (mCurrentEyeIndex + 1) % eyecount;
		}
		else
		{
			mCurrentEyeIndex = 0;
		}
		const int pipelinePairStart = useSharedMultiviewPostprocess ? 0 : (mCurrentEyeIndex % 2) * 2;
		mPostprocess->SetPipelineImagePair(pipelinePairStart, 2);
		mPostprocess->SetCurrentPipelineImage(pipelinePairStart);
	}
}

TArray<uint8_t> VulkanRenderDevice::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	IntRect box;
	box.left = 0;
	box.top = 0;
	box.width = w;
	box.height = h;
	mPostprocess->DrawPresentTexture(box, true, true);

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return ScreenshotBuffer;
}

void VulkanRenderDevice::BeginFrame()
{
	const auto vrmode = VRMode::GetVRModeCached(true);
	mXRFrameBeganThisFrame = false;
	mCurrentEyeIndex = 0;
	mEyeFinalPipelineImage[0] = 0;
	mEyeFinalPipelineImage[1] = 2;
	if (vrmode != nullptr && vrmode->IsVR())
	{
		vrmode->SetUp();
		mXRFrameBeganThisFrame = vrmode->BeginXRFrame();
	}

	SetViewportRects(nullptr);
	int bufferScreenWidth = mScreenViewport.width;
	int bufferScreenHeight = mScreenViewport.height;
	int bufferSceneWidth = mSceneViewport.width;
	int bufferSceneHeight = mSceneViewport.height;
	if (vrmode != nullptr && vrmode->IsVR() && mXRFrameBeganThisFrame && vrmode->ShouldUseRecommendedRenderSizeThisFrame())
	{
		int recommendedWidth = 0;
		int recommendedHeight = 0;
		if (vrmode->GetRecommendedRenderSize(recommendedWidth, recommendedHeight))
		{
			bufferSceneWidth = recommendedWidth;
			bufferSceneHeight = recommendedHeight;
			bufferScreenWidth = recommendedWidth;
			bufferScreenHeight = recommendedHeight;
		}
	}
	if (bufferSceneWidth == 0 || bufferSceneHeight == 0)
	{
		bufferSceneWidth = bufferScreenWidth;
		bufferSceneHeight = bufferScreenHeight;
	}

	mViewpoints->Clear();
	UpdateBackgroundCache(false);
	mCommands->BeginFrame();
	mTextureManager->BeginFrame();
	int eyeLayerCount = 1;
	const bool useMultiviewScene = vrmode != nullptr && vrmode->IsVR() && mXRFrameBeganThisFrame && vrmode->ShouldUseMultiviewThisFrame();
	if (vrmode != nullptr && vrmode->IsVR() && mXRFrameBeganThisFrame && vrmode->ShouldUseMultiviewThisFrame())
	{
		eyeLayerCount = std::max(1, vrmode->GetMultiviewLayerCount());
	}
	const uint32_t multiviewMask = useMultiviewScene ? vrmode->GetMultiviewViewMask() : 0;
	mScreenBuffers->BeginFrame(
		bufferScreenWidth, bufferScreenHeight,
		bufferSceneWidth, bufferSceneHeight,
		eyeLayerCount, eyeLayerCount);

	const VkSampleCountFlagBits sceneSamples = mScreenBuffers->GetSceneSamples();
	mSaveBuffers->BeginFrame(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT, 1, 1);
	mRenderState->BeginFrame();
	mDescriptorSetManager->BeginFrame();
}

void VulkanRenderDevice::SetViewportRects(IntRect *bounds)
{
	Super::SetViewportRects(bounds);

	const auto vrmode = VRMode::GetVRModeCached(true);
	if (vrmode != nullptr && vrmode->IsVR() && vrmode->ShouldUseRecommendedRenderSizeThisFrame())
	{
		vrmode->AdjustViewport(this);
	}
}

void VulkanRenderDevice::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		GetTextureManager()->SetLightmap(LMTextureSize, LMTextureCount, LMTextureData);
		LMTextureData.Reset(); // We no longer need this, release the memory
	}
}

void VulkanRenderDevice::Draw2D(bool outside2D)
{
	::Draw2D(twod, *mRenderState, outside2D);
}

void VulkanRenderDevice::WaitForCommands(bool finish)
{
	mCommands->WaitForCommands(finish);
}

unsigned int VulkanRenderDevice::GetLightBufferBlockSize() const
{
	return mLights->GetBlockSize();
}

void VulkanRenderDevice::PrintStartupLog()
{
	const auto &props = device->PhysicalDevice.Properties.Properties;

	FString deviceType;
	switch (props.deviceType)
	{
	case VK_PHYSICAL_DEVICE_TYPE_OTHER: deviceType = "other"; break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "integrated gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "discrete gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "virtual gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "cpu"; break;
	default: deviceType.Format("%d", (int)props.deviceType); break;
	}

	FString apiVersion, driverVersion;
	apiVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	driverVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
	vkversion = VK_API_VERSION_MAJOR(props.apiVersion) * 100 + VK_API_VERSION_MINOR(props.apiVersion);

	Printf("Vulkan device: " TEXTCOLOR_ORANGE "%s\n", props.deviceName);
	Printf("Vulkan device type: %s\n", deviceType.GetChars());
	Printf("Vulkan version: %s (api) %s (driver)\n", apiVersion.GetChars(), driverVersion.GetChars());

	Printf(PRINT_LOG, "Vulkan extensions:");
	for (const VkExtensionProperties &p : device->PhysicalDevice.Extensions)
	{
		Printf(PRINT_LOG, " %s", p.extensionName);
	}
	Printf(PRINT_LOG, "\n");

	const auto &limits = props.limits;
	Printf("Max. texture size: %d\n", limits.maxImageDimension2D);
	Printf("Max. uniform buffer range: %d\n", limits.maxUniformBufferRange);
	Printf("Min. uniform buffer offset alignment: %" PRIu64 "\n", limits.minUniformBufferOffsetAlignment);
	// [VIEWLIGHTS] The HW buffer set uses 5 storage buffers in the vertex stage (bones,
	// particle ring, drawn lines, particle definitions, view lights), one more than
	// Vulkan guarantees; VkDescriptorSetManager warns in red when a device reports fewer.
	Printf("Max. storage buffers per shader stage: %u\n", (unsigned)limits.maxPerStageDescriptorStorageBuffers);
	Printf("Graphics Queue Family: #%d\n", device->GraphicsFamily);
	Printf("Present Queue Family: #%d\n", device->PresentFamily);
	Printf("Upload Queue Family: #%d (%s)\n", device->UploadFamily, device->UploadFamilySupportsGraphics ? "graphics-capable" : "transfer-only");
	Printf("Upload Queue Slots: %d\n", (int)device->uploadQueues.size());
}

void VulkanRenderDevice::SetLevelMesh(hwrenderer::LevelMesh* mesh)
{
	mRaytrace->SetLevelMesh(mesh);
}

void VulkanRenderDevice::UpdateShadowMap()
{
	mPostprocess->UpdateShadowMap();
}

void VulkanRenderDevice::SetSaveBuffers(bool yes)
{
	if (yes) mActiveRenderBuffers = mSaveBuffers.get();
	else mActiveRenderBuffers = mScreenBuffers.get();
}

void VulkanRenderDevice::ImageTransitionScene(bool unknown)
{
	mPostprocess->ImageTransitionScene(unknown);
}

FRenderState* VulkanRenderDevice::RenderState()
{
	return mRenderState.get();
}

void VulkanRenderDevice::AmbientOccludeScene(float m5)
{
	mPostprocess->AmbientOccludeScene(m5);
}

void VulkanRenderDevice::SetSceneRenderTarget(bool useSSAO)
{
	// RS FORK -- THE SCENE TARGET IS REGISTERED AT THE SCENE IMAGES' OWN SIZE.
	//
	// VkRenderBuffers creates SceneColor/DepthStencil/Fog/Normal at the SCENE
	// size (CreateScene(sceneWidth, sceneHeight)), but both calls below passed
	// GetWidth/GetHeight -- the PIPELINE size, max(window, scene). Whenever the
	// window-derived pipeline was bigger than the scene, the target was
	// recorded bigger than its images, and BeginRenderPass built the
	// framebuffer and render area from that number: a framebuffer larger than
	// its attachments, which Vulkan forbids, and a full-target clear
	// (Set3DViewport opens with SetScissor(0, 0, -1, -1)) that ran off the
	// bottom of the image. The GPU reported exactly that -- "3D HEIGHT CT
	// Violation" -- and reset, 28 times on 2026-09-10 while a collapsed desktop
	// window had shrunk everything window-sized.
	//
	// Every READER of these images already uses the real size (the MSAA
	// resolve and blit read SceneColor.Image->width/height; postprocess uses
	// GetSceneWidth/Height), so only this registration was wrong. On
	// gameplay-eye frames the two sizes are equal and nothing changes.
	const auto vrmode = VRMode::GetVRModeCached(true);
	if (vrmode != nullptr && vrmode->IsVR() && vrmode->ShouldUseMultiviewThisFrame() && GetBuffers()->GetSceneLayers() > 1)
	{
		mRenderState->SetRenderTarget(
			&GetBuffers()->SceneColor,
			GetBuffers()->SceneDepthStencil.GetFramebufferView(),
			GetBuffers()->GetSceneWidth(),
			GetBuffers()->GetSceneHeight(),
			VK_FORMAT_R16G16B16A16_SFLOAT,
			GetBuffers()->GetSceneSamples(),
			std::max(1, vrmode->GetMultiviewLayerCount()),
			vrmode->GetMultiviewViewMask(),
			0);
		return;
	}

	const int layerIndex = GetBuffers()->GetSceneLayers() > 1 ? GetCurrentEyeLayer() : 0;
	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.GetLayerView(layerIndex), GetBuffers()->GetSceneWidth(), GetBuffers()->GetSceneHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, GetBuffers()->GetSceneSamples(), 1, 0, layerIndex);
}

bool VulkanRenderDevice::RaytracingEnabled()
{
	return vk_raytrace && device->SupportsExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
}

bool VulkanRenderDevice::ShouldUseCurrentEyeLayer(const PPTextureType& type, const VkTextureImage* image) const
{
	if (!image || !image->Image || image->Image->layerCount <= 1)
		return false;

	switch (type)
	{
	case PPTextureType::CurrentPipelineTexture:
	case PPTextureType::NextPipelineTexture:
	case PPTextureType::SceneColor:
	case PPTextureType::SceneNormal:
	case PPTextureType::SceneFog:
	case PPTextureType::SceneDepth:
		return true;
	default:
		return false;
	}
}
