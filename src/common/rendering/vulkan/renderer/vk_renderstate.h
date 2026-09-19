/*
** vk_renderstate.h
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

#include "vulkan/system/vk_hwbuffer.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_streambuffer.h"

#include "name.h"

#include "hw_renderstate.h"
#include "hw_material.h"

class VulkanRenderDevice;
class VkRenderPassSetup;
class VkTextureImage;

class VkRenderState : public FRenderState
{
public:
	VkRenderState(VulkanRenderDevice* fb);
	virtual ~VkRenderState() = default;

	// Draw commands
	void ClearScreen() override;
	void Draw(int dt, int index, int count, bool apply = true) override;
	void DrawIndexed(int dt, int index, int count, bool apply = true) override;
	// [MESHPARTICLES] Instanced draws; see FRenderState::DrawInstanced.
	void DrawInstanced(int dt, int index, int count, int instances, int firstInstance = 0, bool apply = true) override;
	void DrawIndexedInstanced(int dt, int index, int count, int instances, int firstInstance = 0, bool apply = true) override;

	// Immediate render state change commands. These only change infrequently and should not clutter the render state.
	bool SetDepthClamp(bool on) override;
	void SetDepthMask(bool on) override;
	void SetDepthFunc(int func) override;
	void SetDepthRange(float min, float max) override;
	void SetColorMask(bool r, bool g, bool b, bool a) override;
	void SetStencil(int offs, int op, int flags = -1) override;
	void SetCulling(int mode) override;
	void EnableClipDistance(int num, bool state) override;
	void Clear(int targets) override;
	void EnableStencil(bool on) override;
	void SetScissor(int x, int y, int w, int h) override;
	void SetViewport(int x, int y, int w, int h) override;
	void EnableDepthTest(bool on) override;
	void EnableMultisampling(bool on) override;
	void EnableLineSmooth(bool on) override;
	void EnableDrawBuffers(int count, bool apply) override;

	// RS FORK -- r_perflog: scene GPU groups; see vk_renderstate.cpp.
	void PushGroup(const FString& name) override;
	void PopGroup() override;

	// [2a] Readable scene depth: a read-only depth pass; see vk_renderstate.cpp.
	bool SetSceneDepthReadable(bool on) override;

	void BeginFrame();
	// [LIGHTMASK] lightMask: this target is the main view's scene and carries the light mask
	// attachment (VulkanRenderDevice::SetSceneRenderTarget). Default false for every other target.
	// [SCENEMASK] postMask: the same for the per-pixel tag attachment. Independent of lightMask --
	// either, both or neither. Default false, so every existing caller sets exactly the target it did.
	void SetRenderTarget(VkTextureImage *image, VulkanImageView *depthStencilView, int width, int height, VkFormat Format, VkSampleCountFlagBits samples, int layers = 1, uint32_t viewMask = 0, int layerIndex = 0, bool lightMask = false, bool postMask = false);
	void Bind(int bindingpoint, uint32_t offset);
	void EndRenderPass();
	// [CANVASCLEAR] Forgets a clear that Clear() queued and no render pass has carried out yet. Clear()
	// only queues: the load ops of the NEXT BeginRenderPass do the clearing, on whatever target is
	// current by then. A caller that leaves a target without drawing on it (RenderTextureView with
	// an empty canvas drawer) calls this so its clear is not carried out on the next target instead.
	void DiscardPendingClears() { mClearTargets = 0; }
	void EndFrame();

protected:
	void Apply(int dt);
	void ApplyRenderPass(int dt);
	void ApplyStencilRef();
	void ApplyDepthBias();
	void ApplyScissor();
	void ApplyViewport();
	void ApplyStreamData();
	void ApplyMatrices();
	void ApplyPushConstants();
	void ApplyHWBufferSet();
	void ApplyVertexBuffers();
	void ApplyMaterial();

	void BeginRenderPass(VulkanCommandBuffer *cmdbuffer);
	void WaitForStreamBuffers();
	int TimestampViewCount() const;	// RS FORK -- r_perflog: query indices one timestamp takes here

	VulkanRenderDevice* fb = nullptr;

	bool mDepthClamp = true;
	VulkanCommandBuffer *mCommandBuffer = nullptr;
	VkPipelineKey mPipelineKey = {};
	VkRenderPassSetup *mPassSetup = nullptr;
	int mClearTargets = 0;
	// [2a] The scene pass holds depth read-only (VkRenderPassKey::DepthReadOnly).
	// Set only by SetSceneDepthReadable; cleared by SetRenderTarget and BeginFrame,
	// so no other target or frame can inherit it.
	bool mSceneDepthReadOnly = false;
	float mQueuedClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	bool mNeedApply = true;

	int mScissorX = 0, mScissorY = 0, mScissorWidth = -1, mScissorHeight = -1;
	int mViewportX = 0, mViewportY = 0, mViewportWidth = -1, mViewportHeight = -1;
	float mViewportDepthMin = 0.0f, mViewportDepthMax = 1.0f;
	bool mScissorChanged = true;
	bool mViewportChanged = true;

	bool mDepthTest = false;
	bool mDepthWrite = false;
	bool mStencilTest = false;

	bool mStencilRefChanged = false;
	int mStencilRef = 0;
	int mStencilOp = 0;
	int mDepthFunc = 0;
	int mColorMask = 15;
	int mCullMode = 0;

	PushConstants mPushConstants = {};

	uint32_t mLastViewpointOffset = 0xffffffff;
	uint32_t mLastMatricesOffset = 0xffffffff;
	uint32_t mLastStreamDataOffset = 0xffffffff;
	uint32_t mViewpointOffset = 0;

	VkStreamBufferWriter mStreamBufferWriter;
	VkMatrixBufferWriter mMatrixBufferWriter;

	int mLastVertexOffsets[2] = { 0, 0 };
	IVertexBuffer *mLastVertexBuffer = nullptr;
	IIndexBuffer *mLastIndexBuffer = nullptr;

	bool mLastModelMatrixEnabled = true;
	bool mLastTextureMatrixEnabled = true;

	int mApplyCount = 0;

	struct RenderTarget
	{
		VkTextureImage *Image = nullptr;
		VulkanImageView *DepthStencil = nullptr;
		int Width = 0;
		int Height = 0;
		VkFormat Format = VK_FORMAT_R16G16B16A16_SFLOAT;
		VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT;
		int DrawBuffers = 1;
		int Layers = 1;
		uint32_t ViewMask = 0;
		int LayerIndex = 0;
		bool LightMask = false;	// [LIGHTMASK] see SetRenderTarget
		bool PostMask = false;	// [SCENEMASK] see SetRenderTarget
	} mRenderTarget;
};

class VkRenderStateMolten : public VkRenderState
{
public:
	using VkRenderState::VkRenderState;

	void Draw(int dt, int index, int count, bool apply = true) override;
};
