/*
** vk_renderstate.cpp
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

#include "vk_renderstate.h"
#include "vulkan/system/vk_renderdevice.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/system/vk_buffer.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_hwtexture.h"

#include "hw_skydome.h"
#include "hw_viewpointuniforms.h"
#include "hw_lightbuffer.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "printf.h"	// [2a] the scene-depth variant line in SetSceneDepthReadable

CVAR(Int, vk_submit_size, 1000, 0);
EXTERN_CVAR(Bool, r_skipmats)

VkRenderState::VkRenderState(VulkanRenderDevice* fb) : fb(fb), mStreamBufferWriter(fb), mMatrixBufferWriter(fb)
{
	Reset();
}

void VkRenderState::ClearScreen()
{
	screen->mViewpoints->Set2D(*this, SCREENWIDTH, SCREENHEIGHT);
	SetColor(0, 0, 0);
	Apply(DT_TriangleStrip);
	mCommandBuffer->draw(4, 1, FFlatVertexBuffer::FULLSCREEN_INDEX, 0);
}

void VkRenderState::Draw(int dt, int index, int count, bool apply)
{
	if (apply || mNeedApply)
		Apply(dt);

	mCommandBuffer->draw(count, 1, index, 0);
}

void VkRenderState::DrawIndexed(int dt, int index, int count, bool apply)
{
	if (apply || mNeedApply)
		Apply(dt);

	mCommandBuffer->drawIndexed(count, 1, index, 0, 0);
}

// [MESHPARTICLES] Instanced draws (FRenderState::DrawInstanced / DrawIndexedInstanced).
// The same Apply as Draw and DrawIndexed; the instance count and the first instance go
// to the command, so gl_InstanceIndex in the vertex shader runs from firstInstance to
// firstInstance + instances - 1 (Vulkan's gl_InstanceIndex includes firstInstance).
// Nothing to draw is not drawn at all.
void VkRenderState::DrawInstanced(int dt, int index, int count, int instances, int firstInstance, bool apply)
{
	if (count <= 0 || instances <= 0)
		return;

	if (apply || mNeedApply)
		Apply(dt);

	mCommandBuffer->draw(count, instances, index, firstInstance);
}

void VkRenderState::DrawIndexedInstanced(int dt, int index, int count, int instances, int firstInstance, bool apply)
{
	if (count <= 0 || instances <= 0)
		return;

	if (apply || mNeedApply)
		Apply(dt);

	mCommandBuffer->drawIndexed(count, instances, index, 0, firstInstance);
}

bool VkRenderState::SetDepthClamp(bool on)
{
	bool lastValue = mDepthClamp;
	mDepthClamp = on;
	mNeedApply = true;
	return lastValue;
}

void VkRenderState::SetDepthMask(bool on)
{
	mDepthWrite = on;
	mNeedApply = true;
}

void VkRenderState::SetDepthFunc(int func)
{
	mDepthFunc = func;
	mNeedApply = true;
}

void VkRenderState::SetDepthRange(float min, float max)
{
	mViewportDepthMin = min;
	mViewportDepthMax = max;
	mViewportChanged = true;
	mNeedApply = true;
}

void VkRenderState::SetColorMask(bool r, bool g, bool b, bool a)
{
	int rr = r, gg = g, bb = b, aa = a;
	mColorMask = (aa << 3) | (bb << 2) | (gg << 1) | rr;
	mNeedApply = true;
}

void VkRenderState::SetStencil(int offs, int op, int flags)
{
	mStencilRef = screen->stencilValue + offs;
	mStencilRefChanged = true;
	mStencilOp = op;

	if (flags != -1)
	{
		bool cmon = !(flags & SF_ColorMaskOff);
		SetColorMask(cmon, cmon, cmon, cmon); // don't write to the graphics buffer
		mDepthWrite = !(flags & SF_DepthMaskOff);
	}

	mNeedApply = true;
}

void VkRenderState::SetCulling(int mode)
{
	mCullMode = mode;
	mNeedApply = true;
}

void VkRenderState::EnableClipDistance(int num, bool state)
{
}

void VkRenderState::Clear(int targets)
{
	if (targets & CT_Color)
	{
		// Snapshot the color clear value now. Vulkan performs attachment clears
		// when the next render pass begins, so callers may legitimately restore
		// screen->mSceneClearColor immediately after requesting a clear.
		mQueuedClearColor[0] = screen->mSceneClearColor[0];
		mQueuedClearColor[1] = screen->mSceneClearColor[1];
		mQueuedClearColor[2] = screen->mSceneClearColor[2];
		mQueuedClearColor[3] = screen->mSceneClearColor[3];
	}

	// Multiple clear requests can be queued before the next render pass begins
	// (e.g. translucent HUD canvas requests CT_Color, then Draw2D requests
	// CT_Stencil). Keep all requested bits so earlier clears are not lost.
	mClearTargets |= targets;
	EndRenderPass();
}

void VkRenderState::EnableStencil(bool on)
{
	mStencilTest = on;
	mNeedApply = true;
}

void VkRenderState::SetScissor(int x, int y, int w, int h)
{
	mScissorX = x;
	mScissorY = y;
	mScissorWidth = w;
	mScissorHeight = h;
	mScissorChanged = true;
	mNeedApply = true;
}

void VkRenderState::SetViewport(int x, int y, int w, int h)
{
	mViewportX = x;
	mViewportY = y;
	mViewportWidth = w;
	mViewportHeight = h;
	mViewportChanged = true;
	mNeedApply = true;
}

void VkRenderState::EnableDepthTest(bool on)
{
	mDepthTest = on;
	mNeedApply = true;
}

void VkRenderState::EnableMultisampling(bool on)
{
}

void VkRenderState::EnableLineSmooth(bool on)
{
}

void VkRenderState::Apply(int dt)
{
	drawcalls.Clock();

	mApplyCount++;
	if (mApplyCount >= vk_submit_size)
	{
		fb->GetCommands()->FlushCommands(false);
		mApplyCount = 0;
	}

	ApplyStreamData();
	ApplyMatrices();
	ApplyRenderPass(dt);
	ApplyScissor();
	ApplyViewport();
	ApplyStencilRef();
	ApplyDepthBias();
	ApplyPushConstants();
	ApplyVertexBuffers();
	ApplyHWBufferSet();
	ApplyMaterial();
	mNeedApply = false;

	drawcalls.Unclock();
}

void VkRenderState::ApplyDepthBias()
{
	if (mBias.mChanged)
	{
		mCommandBuffer->setDepthBias(mBias.mUnits, 0.0f, mBias.mFactor);
		mBias.mChanged = false;
	}
}

void VkRenderState::ApplyRenderPass(int dt)
{
	// Find a pipeline that matches our state
	VkPipelineKey pipelineKey;
	pipelineKey.DrawType = dt;
	pipelineKey.VertexFormat = static_cast<VkHardwareVertexBuffer*>(mVertexBuffer)->VertexFormat;
	pipelineKey.RenderStyle = mRenderStyle;
	pipelineKey.DepthTest = mDepthTest;
	pipelineKey.DepthWrite = mDepthTest && mDepthWrite;
	pipelineKey.DepthFunc = mDepthFunc;
	pipelineKey.DepthClamp = mDepthClamp;
	pipelineKey.DepthBias = !(mBias.mFactor == 0 && mBias.mUnits == 0);
	pipelineKey.StencilTest = mStencilTest;
	pipelineKey.StencilPassOp = mStencilOp;
	pipelineKey.ColorMask = mColorMask;
	pipelineKey.CullMode = mCullMode;
	pipelineKey.NumTextureLayers = mMaterial.mMaterial ? mMaterial.mMaterial->NumLayers() : 0;
	pipelineKey.NumTextureLayers = max(pipelineKey.NumTextureLayers, SHADER_MIN_REQUIRED_TEXTURE_LAYERS);// Always force minimum 8 textures as the shader requires it
	if (mSpecialEffect > EFF_NONE)
	{
		pipelineKey.SpecialEffect = mSpecialEffect;
		pipelineKey.EffectState = 0;
		pipelineKey.AlphaTest = false;
	}
	else
	{
		int effectState = mMaterial.mOverrideShader >= 0 ? mMaterial.mOverrideShader : (mMaterial.mMaterial ? mMaterial.mMaterial->GetShaderIndex() : 0);
		pipelineKey.SpecialEffect = EFF_NONE;
		pipelineKey.EffectState = mTextureEnabled ? effectState : SHADER_NoTexture;
		if (r_skipmats && pipelineKey.EffectState >= 3 && pipelineKey.EffectState <= 4)
			pipelineKey.EffectState = 0;
		pipelineKey.AlphaTest = mAlphaThreshold >= 0.f;
	}

	// Is this the one we already have?
	bool inRenderPass = mCommandBuffer;
	bool changingPipeline = (!inRenderPass) || (pipelineKey != mPipelineKey);

	if (!inRenderPass)
	{
		mCommandBuffer = fb->GetCommands()->GetDrawCommands();
		mScissorChanged = true;
		mViewportChanged = true;
		mStencilRefChanged = true;
		mBias.mChanged = true;

		BeginRenderPass(mCommandBuffer);
	}

	if (changingPipeline)
	{
		mCommandBuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, mPassSetup->GetPipeline(pipelineKey));
		mPipelineKey = pipelineKey;
	}
}

void VkRenderState::ApplyStencilRef()
{
	if (mStencilRefChanged)
	{
		mCommandBuffer->setStencilReference(VK_STENCIL_FRONT_AND_BACK, mStencilRef);
		mStencilRefChanged = false;
	}
}

void VkRenderState::ApplyScissor()
{
	if (mScissorChanged)
	{
		VkRect2D scissor;
		if (mScissorWidth >= 0)
		{
			int x0 = clamp(mScissorX, 0, mRenderTarget.Width);
			int y0 = clamp(mScissorY, 0, mRenderTarget.Height);
			int x1 = clamp(mScissorX + mScissorWidth, 0, mRenderTarget.Width);
			int y1 = clamp(mScissorY + mScissorHeight, 0, mRenderTarget.Height);

			scissor.offset.x = x0;
			scissor.offset.y = y0;
			scissor.extent.width = x1 - x0;
			scissor.extent.height = y1 - y0;
		}
		else
		{
			scissor.offset.x = 0;
			scissor.offset.y = 0;
			scissor.extent.width = mRenderTarget.Width;
			scissor.extent.height = mRenderTarget.Height;
		}
		mCommandBuffer->setScissor(0, 1, &scissor);
		mScissorChanged = false;
	}
}

void VkRenderState::ApplyViewport()
{
	if (mViewportChanged)
	{
		VkViewport viewport;
		if (mViewportWidth >= 0)
		{
			viewport.x = (float)mViewportX;
			viewport.y = (float)mViewportY;
			viewport.width = (float)mViewportWidth;
			viewport.height = (float)mViewportHeight;
		}
		else
		{
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = (float)mRenderTarget.Width;
			viewport.height = (float)mRenderTarget.Height;
		}
		viewport.minDepth = mViewportDepthMin;
		viewport.maxDepth = mViewportDepthMax;
		mCommandBuffer->setViewport(0, 1, &viewport);
		mViewportChanged = false;
	}
}

void VkRenderState::ApplyStreamData()
{
	auto passManager = fb->GetRenderPassManager();

	mStreamData.useVertexData = passManager->GetVertexFormat(static_cast<VkHardwareVertexBuffer*>(mVertexBuffer)->VertexFormat)->UseVertexData;

	if (mMaterial.mMaterial && mMaterial.mMaterial->Source())
		mStreamData.timer = static_cast<float>((double)(screen->FrameTime - firstFrame) * (double)mMaterial.mMaterial->Source()->GetShaderSpeed() / 1000.);
	else
		mStreamData.timer = 0.0f;

	if (!mStreamBufferWriter.Write(mStreamData))
	{
		WaitForStreamBuffers();
		mStreamBufferWriter.Write(mStreamData);
	}
}

void VkRenderState::ApplyPushConstants()
{
	int fogset = 0;
	if (mFogEnabled)
	{
		if (mFogEnabled == 2)
		{
			fogset = -3;	// 2D rendering with 'foggy' overlay.
		}
		else if ((GetFogColor() & 0xffffff) == 0)
		{
			fogset = gl_fogmode;
		}
		else
		{
			fogset = -gl_fogmode;
		}
	}

	int tempTM = TM_NORMAL;
	if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas() && !mMaterial.mMaterial->Source()->GetTranslucency())
	{
		// Match GL behavior: only force opaque for fully-opaque canvas textures.
		// Translucent UI canvases (VR HUD surface) must preserve alpha.
		auto* canvasTex = static_cast<FCanvasTexture*>(mMaterial.mMaterial->Source()->GetTexture());
		tempTM = (canvasTex && canvasTex->bTranslucentCanvas) ? TM_NORMAL : TM_OPAQUE;
	}

	mPushConstants.uFogEnabled = fogset;
	mPushConstants.uTextureMode = GetTextureModeAndFlags(tempTM);
	mPushConstants.uLightDist = mLightParms[0];
	mPushConstants.uLightFactor = mLightParms[1];
	mPushConstants.uFogDensity = mLightParms[2];
	mPushConstants.uLightLevel = mLightParms[3];
	mPushConstants.uAlphaThreshold = mAlphaThreshold;
	mPushConstants.uClipSplit = { mClipSplit[0], mClipSplit[1] };

	if (mMaterial.mMaterial)
	{
		auto source = mMaterial.mMaterial->Source();
		mPushConstants.uSpecularMaterial = { source->GetGlossiness(), source->GetSpecularLevel() };
	}

	mPushConstants.uLightIndex = mLightIndex;
	mPushConstants.uBoneIndexBase = mBoneIndexBase;
	mPushConstants.uDataIndex = mStreamBufferWriter.DataIndex();

	auto passManager = fb->GetRenderPassManager();
	mCommandBuffer->pushConstants(passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(PushConstants), &mPushConstants);
}

void VkRenderState::ApplyMatrices()
{
	if (!mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix, mTextureMatrixEnabled))
	{
		WaitForStreamBuffers();
		mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix, mTextureMatrixEnabled);
	}
}

void VkRenderState::ApplyVertexBuffers()
{
	if ((mVertexBuffer != mLastVertexBuffer || mVertexOffsets[0] != mLastVertexOffsets[0] || mVertexOffsets[1] != mLastVertexOffsets[1]) && mVertexBuffer)
	{
		auto vkbuf = static_cast<VkHardwareVertexBuffer*>(mVertexBuffer);
		const VkVertexFormat *format = fb->GetRenderPassManager()->GetVertexFormat(vkbuf->VertexFormat);
		VkBuffer vertexBuffers[2] = { vkbuf->mBuffer->buffer, vkbuf->mBuffer->buffer };
		VkDeviceSize offsets[] = { mVertexOffsets[0] * format->Stride, mVertexOffsets[1] * format->Stride };
		mCommandBuffer->bindVertexBuffers(0, 2, vertexBuffers, offsets);
		mLastVertexBuffer = mVertexBuffer;
		mLastVertexOffsets[0] = mVertexOffsets[0];
		mLastVertexOffsets[1] = mVertexOffsets[1];
	}

	if (mIndexBuffer != mLastIndexBuffer && mIndexBuffer)
	{
		mCommandBuffer->bindIndexBuffer(static_cast<VkHardwareIndexBuffer*>(mIndexBuffer)->mBuffer->buffer, 0, VK_INDEX_TYPE_UINT32);
		mLastIndexBuffer = mIndexBuffer;
	}
}

void VkRenderState::ApplyMaterial()
{
	if (mMaterial.mChanged)
	{
		auto passManager = fb->GetRenderPassManager();
		auto descriptors = fb->GetDescriptorSetManager();

		if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas()) static_cast<FCanvasTexture*>(mMaterial.mMaterial->Source()->GetTexture())->NeedUpdate();

		VulkanDescriptorSet* descriptorset = mMaterial.mMaterial ? static_cast<VkMaterial*>(mMaterial.mMaterial)->GetDescriptorSet(mMaterial) : descriptors->GetNullTextureDescriptorSet();

		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, fb->GetRenderPassManager()->GetPipelineLayout(mPipelineKey.NumTextureLayers), 0, fb->GetDescriptorSetManager()->GetFixedDescriptorSet());
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), 2, descriptorset);
		mMaterial.mChanged = false;
	}
}

void VkRenderState::ApplyHWBufferSet()
{
	uint32_t matrixOffset = mMatrixBufferWriter.Offset();
	uint32_t streamDataOffset = mStreamBufferWriter.StreamDataOffset();
	if (mViewpointOffset != mLastViewpointOffset || matrixOffset != mLastMatricesOffset || streamDataOffset != mLastStreamDataOffset)
	{
		auto passManager = fb->GetRenderPassManager();
		auto descriptors = fb->GetDescriptorSetManager();

		uint32_t offsets[3] = { mViewpointOffset, matrixOffset, streamDataOffset };
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), 0, fb->GetDescriptorSetManager()->GetFixedDescriptorSet());
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), 1, descriptors->GetHWBufferDescriptorSet(), 3, offsets);

		mLastViewpointOffset = mViewpointOffset;
		mLastMatricesOffset = matrixOffset;
		mLastStreamDataOffset = streamDataOffset;
	}
}

void VkRenderState::WaitForStreamBuffers()
{
	fb->WaitForCommands(false);
	mApplyCount = 0;
	mStreamBufferWriter.Reset();
	mMatrixBufferWriter.Reset();
}

void VkRenderState::Bind(int bindingpoint, uint32_t offset)
{
	if (bindingpoint == VIEWPOINT_BINDINGPOINT)
	{
		mViewpointOffset = offset;
		mNeedApply = true;
	}
}

void VkRenderState::BeginFrame()
{
	mMaterial.Reset();
	mApplyCount = 0;
	mSceneDepthReadOnly = false;	// [2a] never carried into a new frame
}

void VkRenderState::EndRenderPass()
{
	if (mCommandBuffer)
	{
		mCommandBuffer->endRenderPass();
		mCommandBuffer = nullptr;
		mPipelineKey = {};

		mLastViewpointOffset = 0xffffffff;
		mLastVertexBuffer = nullptr;
		mLastIndexBuffer = nullptr;
		mLastModelMatrixEnabled = true;
		mLastTextureMatrixEnabled = true;
	}
}

// RS FORK -- r_perflog: named GPU groups around the scene passes
// (hw_drawinfo.cpp), routed to the same command-buffer timestamp groups the
// post-process passes use, so they show in "stat gpu" and feed perflog.txt
// (they also drop a vk_gpu_checkpoints marker, which only helps a device-lost
// report). Unlike the post-process groups these are written INSIDE the scene
// render pass, and a timestamp written inside a multiview pass takes one query
// index per view (vkCmdWriteTimestamp), so the count is passed along.
int VkRenderState::TimestampViewCount() const
{
	uint32_t mask = mCommandBuffer != nullptr ? mRenderTarget.ViewMask : 0;
	int views = 0;
	for (; mask != 0; mask &= mask - 1)
		views++;
	return views > 0 ? views : 1;
}

void VkRenderState::PushGroup(const FString& name)
{
	fb->GetCommands()->PushGroup(name, TimestampViewCount());
}

void VkRenderState::PopGroup()
{
	fb->GetCommands()->PopGroup(TimestampViewCount());
}

// [2a] READABLE SCENE DEPTH (FRenderState::SetSceneDepthReadable).
//
// Vulkan cannot sample an image that is a writable attachment of the pass being
// drawn. So "readable" means: end the scene pass and begin it again with the
// depth/stencil attachment referenced as DEPTH_STENCIL_READ_ONLY_OPTIMAL
// (VkRenderPassKey::DepthReadOnly). Same attachments, LOAD/STORE, no clears, the
// same initial and final layouts, so every pass after it sees what it saw before
// 2a; pipelines built for it never write depth or stencil. No copy is made.
//
// Only for the depth image that fixed binding 3 holds this frame, attached the
// same way (VkDescriptorSetManager::IsSceneDepthReadTarget), and never while a
// depth or stencil clear is still queued, which a read-only pass cannot perform.
// Otherwise it returns false and changes nothing.
//
// The next pass begins HERE rather than lazily at the next draw, so a perf group
// around the call (fx.depthread) times the whole switch, and its Push and Pop
// both sit inside a pass on the same target and take the same number of
// timestamp views (TimestampViewCount).
bool VkRenderState::SetSceneDepthReadable(bool on)
{
	const bool readable = on && !(mClearTargets & (CT_Depth | CT_Stencil)) &&
		fb->GetDescriptorSetManager()->IsSceneDepthReadTarget(mRenderTarget.DepthStencil, mRenderTarget.Layers, mRenderTarget.ViewMask);

	if (on)
	{
		// One console line per distinct outcome per session, so a log from a
		// headset run shows which variant soft particles used, or that a target
		// was refused. Once each, not on change: the main view and a camera texture
		// in the same frame would otherwise alternate every frame.
		static unsigned loggedOutcomes = 0;
		const int outcome = !readable ? 0 : 1 + (mRenderTarget.Samples > 1 ? 1 : 0) + (mRenderTarget.ViewMask != 0 ? 2 : 0);
		if (!(loggedOutcomes & (1u << outcome)))
		{
			loggedOutcomes |= 1u << outcome;
			if (readable)
				Printf("Scene depth: read-only depth pass, %s %s variant\n",
					mRenderTarget.Samples > 1 ? "multisample" : "single-sample",
					mRenderTarget.ViewMask != 0 ? "layered" : "flat");
			else
				Printf("Scene depth: a target was refused (layers %d, view mask %u) -- its effects draw as before\n",
					mRenderTarget.Layers, (unsigned)mRenderTarget.ViewMask);
		}
	}

	if (readable == mSceneDepthReadOnly)
		return readable;

	const bool wasInPass = mCommandBuffer != nullptr;
	EndRenderPass();
	mSceneDepthReadOnly = readable;
	mNeedApply = true;

	if (wasInPass)
	{
		// What ApplyRenderPass does when it has to begin a pass; EndRenderPass has
		// already cleared the pipeline, viewpoint and vertex buffer bindings, and
		// BeginRenderPass marks the descriptor sets for rebinding.
		mCommandBuffer = fb->GetCommands()->GetDrawCommands();
		mScissorChanged = true;
		mViewportChanged = true;
		mStencilRefChanged = true;
		mBias.mChanged = true;
		BeginRenderPass(mCommandBuffer);
	}
	return readable;
}

void VkRenderState::EndFrame()
{
	mMatrixBufferWriter.Reset();
	mStreamBufferWriter.Reset();
}

void VkRenderState::EnableDrawBuffers(int count, bool apply)
{
	if (mRenderTarget.DrawBuffers != count)
	{
		EndRenderPass();
		mRenderTarget.DrawBuffers = count;
	}
}

void VkRenderState::SetRenderTarget(VkTextureImage *image, VulkanImageView *depthStencilView, int width, int height, VkFormat format, VkSampleCountFlagBits samples, int layers, uint32_t viewMask, int layerIndex, bool lightMask)
{
	EndRenderPass();
	mSceneDepthReadOnly = false;	// [2a] a new target never inherits a read-only depth pass

	mRenderTarget.Image = image;
	mRenderTarget.DepthStencil = depthStencilView;
	mRenderTarget.Width = width;
	mRenderTarget.Height = height;
	mRenderTarget.Format = format;
	mRenderTarget.Samples = samples;
	mRenderTarget.Layers = layers;
	mRenderTarget.ViewMask = viewMask;
	mRenderTarget.LayerIndex = layerIndex;
	mRenderTarget.LightMask = lightMask;	// [LIGHTMASK] every other target sets it back to false
}

void VkRenderState::BeginRenderPass(VulkanCommandBuffer *cmdbuffer)
{
	VkRenderPassKey key = {};
	key.DrawBufferFormat = mRenderTarget.Format;
	key.Samples = mRenderTarget.Samples;
	key.DrawBuffers = mRenderTarget.DrawBuffers;
	key.DepthStencil = !!mRenderTarget.DepthStencil;
	key.Layers = mRenderTarget.Layers;
	key.ViewMask = mRenderTarget.ViewMask;
	// [2a] Zero unless SetSceneDepthReadable(true) accepted this target, so every
	// other pass gets exactly the key -- and so the render pass, framebuffer and
	// pipelines -- it had before 2a.
	key.DepthReadOnly = (mSceneDepthReadOnly && key.DepthStencil) ? 1 : 0;
	// [LIGHTMASK] Only the main view's scene target, and only while the mask programs exist for
	// the pass these draw buffers make -- which the frame's decision already ensured for the pass
	// the view uses (VulkanRenderDevice::BeginFrame). Zero for every other pass.
	key.LightMask = (mRenderTarget.LightMask && fb->GetShaderManager()->LightMaskProgramsReady(key.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS)) ? 1 : 0;

	mPassSetup = fb->GetRenderPassManager()->GetRenderPass(key);

	const bool useLayerView = mRenderTarget.Layers == 1 && mRenderTarget.ViewMask == 0;
	VkTextureImage::VkRenderTargetFramebufferKey framebufferKey = {};
	framebufferKey.PassKey = key;
	framebufferKey.LayerIndex = useLayerView ? mRenderTarget.LayerIndex : -1;

	auto &framebuffer = mRenderTarget.Image->RSFramebuffers[framebufferKey];
	if (!framebuffer)
	{
		auto buffers = fb->GetBuffers();
		FramebufferBuilder builder;
		builder.RenderPass(mPassSetup->GetRenderPass(0));
		builder.Size(mRenderTarget.Width, mRenderTarget.Height, mRenderTarget.Layers);
		builder.AddAttachment(useLayerView ? mRenderTarget.Image->GetLayerView(mRenderTarget.LayerIndex) : mRenderTarget.Image->GetFramebufferView());
		if (key.DrawBuffers > 1)
			builder.AddAttachment(useLayerView ? buffers->SceneFog.GetLayerView(mRenderTarget.LayerIndex) : buffers->SceneFog.GetFramebufferView());
		if (key.DrawBuffers > 2)
			builder.AddAttachment(useLayerView ? buffers->SceneNormal.GetLayerView(mRenderTarget.LayerIndex) : buffers->SceneNormal.GetFramebufferView());
		if (key.LightMask)	// [LIGHTMASK] after the draw buffers, before depth (VkRenderPassSetup::CreateRenderPass)
			builder.AddAttachment(useLayerView ? buffers->SceneLightMask.GetLayerView(mRenderTarget.LayerIndex) : buffers->SceneLightMask.GetFramebufferView());
		if (key.DepthStencil)
			builder.AddAttachment(mRenderTarget.DepthStencil);
		builder.DebugName("VkRenderPassSetup.Framebuffer");
		framebuffer = builder.Create(fb->device.get());
	}

	// Only clear depth+stencil if the render target actually has that
	if (!mRenderTarget.DepthStencil)
		mClearTargets &= ~(CT_Depth | CT_Stencil);

	RenderPassBegin beginInfo;
	beginInfo.RenderPass(mPassSetup->GetRenderPass(mClearTargets));
	beginInfo.RenderArea(0, 0, mRenderTarget.Width, mRenderTarget.Height);
	beginInfo.Framebuffer(framebuffer.get());
	beginInfo.AddClearColor(mQueuedClearColor[0], mQueuedClearColor[1], mQueuedClearColor[2], mQueuedClearColor[3]);
	if (key.DrawBuffers > 1)
		beginInfo.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	if (key.DrawBuffers > 2)
		beginInfo.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	if (key.LightMask)	// [LIGHTMASK] no light of either class
		beginInfo.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	beginInfo.AddClearDepthStencil(1.0f, 0);
	beginInfo.Execute(cmdbuffer);

	mMaterial.mChanged = true;
	mClearTargets = 0;
}

/////////////////////////////////////////////////////////////////////////////

void VkRenderStateMolten::Draw(int dt, int index, int count, bool apply)
{
	if (dt == DT_TriangleFan)
	{
		IIndexBuffer *oldIndexBuffer = mIndexBuffer;
		mIndexBuffer = fb->GetBufferManager()->FanToTrisIndexBuffer.get();

		if (apply || mNeedApply)
			Apply(DT_Triangles);
		else
			ApplyVertexBuffers();

		mCommandBuffer->drawIndexed((count - 2) * 3, 1, 0, index, 0);

		mIndexBuffer = oldIndexBuffer;
	}
	else
	{
		if (apply || mNeedApply)
			Apply(dt);

		mCommandBuffer->draw(count, 1, index, 0);
	}
}
