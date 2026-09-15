/*
** vk_renderdevice.h
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

#include "gl_sysfb.h"
#include "engineerrors.h"
#include "TSQueue.h"
#include "textures.h"
#include "fs_files.h"
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanobjects.h>

struct FRenderViewpoint;
class VkSamplerManager;
class VkBufferManager;
class VkTextureManager;
class VkShaderManager;
class VkCommandBufferManager;
class VkDescriptorSetManager;
class VkRenderPassManager;
class VkFramebufferManager;
class VkRaytrace;
class VkRenderState;
class VkStreamBuffer;
class VkHardwareDataBuffer;
class VkHardwareTexture;
class VkRenderBuffers;
class VkPostprocess;
class VkComputeManager;		// [COMPUTE] vk_compute.h
struct FrameComputeInput;	// [COMPUTE] hw_framecompute.h
class VkTextureImage;
struct VkCompressedPixels;	// [DDS] vk_hwtexture.h
class SWSceneDrawer;
enum class PPTextureType;
class FModel;

struct VkTexLoadIn
{
	FTexture* texture = nullptr;
	int translation = 0;
	int scaleFlags = 0;
	VkHardwareTexture* hardwareTexture = nullptr;
};

struct VkTexLoadOut
{
	std::shared_ptr<uint8_t> pixels;
	int width = 0;
	int height = 0;
	int scaleFlags = 0;
	VkHardwareTexture* hardwareTexture = nullptr;
	bool uploadedInThread = false;
	bool needsQueueOwnershipTransfer = false;
	int uploadQueueFamily = -1;
	std::shared_ptr<VkCompressedPixels> compressed;	// [DDS] stored levels a load-only worker read; UploadLoadedTextures uploads them
};

struct VkModelLoadIn
{
	int lump = -1;
	FModel* model = nullptr;
};

struct VkModelLoadOut
{
	int lump = -1;
	FileSys::FileData data;
	FModel* model = nullptr;
};

class VkTexLoadThread : public ResourceLoader2<VkTexLoadIn, VkTexLoadOut>
{
public:
	VkTexLoadThread(VkCommandBufferManager* bgCmd, VulkanDevice* device, int uploadQueueIndex, TSQueue<VkTexLoadIn>* inQueue, TSQueue<VkTexLoadIn>* secondaryQueue, TSQueue<VkTexLoadOut>* outQueue)
		: ResourceLoader2<VkTexLoadIn, VkTexLoadOut>(inQueue, secondaryQueue, outQueue)
	{
		cmd = bgCmd;
		if (device != nullptr && uploadQueueIndex >= 0 && uploadQueueIndex < (int)device->uploadQueues.size())
		{
			uploadQueue = device->uploadQueues[uploadQueueIndex];
		}
	}

protected:
	VkCommandBufferManager* cmd = nullptr;
	VulkanUploadSlot uploadQueue = {};

	bool loadResource(VkTexLoadIn& input, VkTexLoadOut& output) override;
};

class VkModelLoadThread : public ResourceLoader2<VkModelLoadIn, VkModelLoadOut>
{
public:
	VkModelLoadThread(TSQueue<VkModelLoadIn>* inQueue, TSQueue<VkModelLoadOut>* outQueue)
		: ResourceLoader2<VkModelLoadIn, VkModelLoadOut>(inQueue, nullptr, outQueue)
	{
	}

protected:
	bool loadResource(VkModelLoadIn& input, VkModelLoadOut& output) override;
};

class VulkanRenderDevice : public SystemBaseFrameBuffer
{
	typedef SystemBaseFrameBuffer Super;


public:
	std::shared_ptr<VulkanDevice> device;

	VkCommandBufferManager* GetCommands() { return mCommands.get(); }
	VkShaderManager *GetShaderManager() { return mShaderManager.get(); }
	VkSamplerManager *GetSamplerManager() { return mSamplerManager.get(); }
	VkBufferManager* GetBufferManager() { return mBufferManager.get(); }
	VkTextureManager* GetTextureManager() { return mTextureManager.get(); }
	VkFramebufferManager* GetFramebufferManager() { return mFramebufferManager.get(); }
	VkDescriptorSetManager* GetDescriptorSetManager() { return mDescriptorSetManager.get(); }
	VkRenderPassManager *GetRenderPassManager() { return mRenderPassManager.get(); }
	VkRaytrace* GetRaytrace() { return mRaytrace.get(); }
	VkRenderState *GetRenderState() { return mRenderState.get(); }
	VkPostprocess *GetPostprocess() { return mPostprocess.get(); }
	VkComputeManager* GetCompute() { return mCompute.get(); }	// [COMPUTE]
	VkRenderBuffers *GetBuffers() { return mActiveRenderBuffers; }
	int GetCurrentEyeLayer() const { return std::max(0, mCurrentEyeIndex); }
	bool ShouldUseCurrentEyeLayer(const PPTextureType& type, const VkTextureImage* image) const;
	// [LIGHTMASK] The scene being drawn carries the light mask: this frame draws it, the active
	// buffers are the screen's (never a save picture's) and its images exist.
	bool SceneHasLightMask() const;
	FRenderState* RenderState() override;

	unsigned int GetLightBufferBlockSize() const;

	VulkanRenderDevice(void *hMonitor, bool fullscreen, std::shared_ptr<VulkanSurface> surface);
	~VulkanRenderDevice();
	bool IsVulkan() override { return true; }
	bool SupportsBC7TextureArrays() override;	// [ATLASBC7] see v_video.h
	int GetMaxTextureArrayLayers() override;	// [ATLASBC7]

	void Update() override;

	void InitializeState() override;
	bool CompileNextShader() override;
	void PrecacheMaterial(FMaterial *mat, int translation) override;
	void PrequeueMaterial(FMaterial *mat, int translation) override;
	bool BackgroundCacheMaterial(FMaterial *mat, FTranslationID translation, bool makeSPI = false, bool secondary = false) override;
	bool BackgroundCacheTextureMaterial(FGameTexture *tex, FTranslationID translation, int scaleFlags, bool makeSPI = false) override;
	bool BackgroundLoadModel(FModel* model) override;
	bool CachingActive() override;
	bool SupportsBackgroundCache() override { return bgTransferEnabled; }
	void StopBackgroundCache() override;
	void FlushBackground() override;
	float CacheProgress() override;
	void UpdateBackgroundCache(bool flush = false) override;
	void UpdatePalette() override;
	const char* DeviceName() const override;
	int Backend() override { return 1; }
	void SetTextureFilterMode() override;
	void NewRefreshRate() override;
	void StartPrecaching() override;
	void BeginFrame() override;
	void InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData) override;
	void BlurScene(float amount) override;
	void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D) override;
	void RunFrameCompute(const FrameComputeInput& input) override;	// [COMPUTE]
	void AmbientOccludeScene(float m5) override;
	void SetSceneRenderTarget(bool useSSAO) override;
	void SetLevelMesh(hwrenderer::LevelMesh* mesh) override;
	void UpdateShadowMap() override;
	void SetSaveBuffers(bool yes) override;
	void SetViewportRects(IntRect *bounds) override;
	void ImageTransitionScene(bool unknown) override;
	void SetActiveRenderTarget() override;
	void FirstEye() override;
	void NextEye(int eyecount) override;

	IHardwareTexture *CreateHardwareTexture(int numchannels) override;
	FMaterial* CreateMaterial(FGameTexture* tex, int scaleflags) override;
	IVertexBuffer *CreateVertexBuffer() override;
	IIndexBuffer *CreateIndexBuffer() override;
	IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override;

	FTexture *WipeStartScreen() override;
	FTexture *WipeEndScreen() override;

	TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma) override;

	bool GetVSync() { return mVSync; }
	void SetVSync(bool vsync) override;

	void Draw2D(bool outside2D = false) override;

	void WaitForCommands(bool finish) override;

	bool RaytracingEnabled();

private:
	void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc) override;
	void PrintStartupLog();
	void CopyScreenToBuffer(int w, int h, uint8_t *data) override;
	void UploadLoadedTextures(bool flush = false);
	void UpdateLightMask();	// [LIGHTMASK] the frame's light mask decision, from BeginFrame

	struct QueuedPatch
	{
		FGameTexture *tex = nullptr;
		int translation = 0;
		int scaleFlags = 0;
		bool secondary = false;
	};

	std::unique_ptr<VkCommandBufferManager> mCommands;
	std::vector<std::unique_ptr<VkCommandBufferManager>> mBGTransferCommands;
	std::unique_ptr<VkBufferManager> mBufferManager;
	std::unique_ptr<VkSamplerManager> mSamplerManager;
	std::unique_ptr<VkTextureManager> mTextureManager;
	std::unique_ptr<VkFramebufferManager> mFramebufferManager;
	std::unique_ptr<VkShaderManager> mShaderManager;
	std::unique_ptr<VkRenderBuffers> mScreenBuffers;
	std::unique_ptr<VkRenderBuffers> mSaveBuffers;
	std::unique_ptr<VkPostprocess> mPostprocess;
	std::unique_ptr<VkDescriptorSetManager> mDescriptorSetManager;
	std::unique_ptr<VkRenderPassManager> mRenderPassManager;
	std::unique_ptr<VkRaytrace> mRaytrace;
	std::unique_ptr<VkRenderState> mRenderState;
	// [COMPUTE] GPU compute for effects (vk_compute.h). Last, so it is destroyed first.
	std::unique_ptr<VkComputeManager> mCompute;

	VkRenderBuffers *mActiveRenderBuffers = nullptr;

	bool mVSync = false;
	// [LIGHTSHADOWS] Ray-traced light shadows for this device's whole life, decided once in the constructor (RaytracingEnabled).
	bool mRaytraceShadows = false;
	bool mXRFrameBeganThisFrame = false;
	int mCurrentEyeIndex = 0;
	int mEyeFinalPipelineImage[2] = { 0, 2 };
	// [LIGHTMASK] The light mask's format on this device, VK_FORMAT_UNDEFINED when the device
	// cannot carry it (InitializeState); and last frame's decision, for the on/off log line.
	VkFormat mLightMaskFormat = VK_FORMAT_UNDEFINED;
	bool mLightMaskWasActive = false;
	// [ATLASBC7] SupportsBC7TextureArrays' answer once asked (-1 not yet): it never changes for a device.
	int mSupportsBC7TextureArrays = -1;
	TSQueue<VkTexLoadIn> primaryTexQueue;
	TSQueue<VkTexLoadIn> secondaryTexQueue;
	TSQueue<VkTexLoadOut> outputTexQueue;
	TSQueue<QueuedPatch> patchQueue;
	TSQueue<VkModelLoadIn> modelInQueue;
	TSQueue<VkModelLoadOut> modelOutQueue;
	std::unique_ptr<VkModelLoadThread> modelThread;
	std::vector<std::unique_ptr<VkTexLoadThread>> bgTransferThreads;
	bool bgTransferEnabled = false;
};

class CVulkanError : public CEngineError
{
public:
	CVulkanError() : CEngineError() {}
	CVulkanError(const char* message) : CEngineError(message) {}
};
