/*
** hw_gpuparticlebuffer.h
**
** [GPUPARTICLES] The GPU half of the stateless particle ring.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: every procedural effect the engine has (beams, stamps,
** shapes, sweeps, fog disturbances) is a FIELD -- each pixel measures its
** distance to a published description -- and none of them can represent many
** small independent moving objects. Sparks, embers and muzzle ejecta need
** matter. See "Engine docs/GPU_PARTICLES_PLAN.md".
**
** A record holds a particle's STARTING conditions and gpuparticles.vp works out
** where it is now from uLevelTime. Nothing is simulated or re-uploaded per
** frame; only records written since the last scene are copied.
**
** This is the renderer-specific half. The CPU ring and the script API live on
** FLevelLocals and survive a renderer rebuild; this class, the two shaders and
** the draw call in HWDrawInfo::RenderTranslucent are what a rebuild replaces.
**
** Vulkan only. It is created beside mBones in VulkanRenderDevice and never on
** GL or GLES, which also never load the effect shader.
**
*/

#pragma once

#include <cstdint>
#include "hwrenderer/data/buffers.h"

class GpuParticleBuffer
{
public:
	// Five vec4s, std430, no padding. Must equal sizeof(FLevelLocals::GpuParticleRecord)
	// and the GpuParticle struct declared in vk_shader.cpp's prolog.
	static const unsigned RECORD_BYTES = 80;

	// Two triangles per ring slot, built once at ring size.
	static const unsigned VERTICES_PER_RECORD = 6;

	// ringSize 0 = GpuParticleRingCapacity(), which is what the CPU ring on
	// FLevelLocals sizes from too. Pass a size only if you also size the CPU
	// ring to match.
	explicit GpuParticleBuffer(unsigned ringSize = 0);
	~GpuParticleBuffer();

	// THE SYNC RULE -- one rule for normal frames, level changes, savegame
	// loads and bursts larger than the ring:
	//
	//   1. serial differs, or written - syncedWritten >= size: upload everything
	//   2. otherwise upload [syncedWritten % size, written % size), two ranges
	//      when it wraps
	//   3. remember serial and written
	//
	// The serial comes from a global counter per level, never the FLevelLocals
	// pointer: a new level can be allocated at the same address, and maptime
	// restarts at zero, so a stale record from the last map would otherwise
	// come back to life with a valid-looking age.
	//
	// Frames in flight: this is ONE persistently mapped buffer, exactly like
	// Vulkan's bone buffer (created with pipeline count 1 and rewritten every
	// frame). If GL ever draws particles, give each rotating copy its own
	// synced state; the rule already works per copy.
	void Sync(const void *records, unsigned recordCount, uint64_t serial, uint64_t written);

	IDataBuffer *GetBuffer() const { return mBuffer; }
	IVertexBuffer *GetVertexBuffer() const { return mQuads; }
	unsigned GetRingSize() const { return mRingSize; }
	int GetVertexCount() const { return (int)(mRingSize * VERTICES_PER_RECORD); }

	// Set by VkShaderManager once the gpuparticles effect has compiled for
	// every pass. A pipeline built for an effect whose program is missing
	// dereferences null (VkRenderPassSetup::CreatePipeline), so the draw is
	// gated on this rather than on the buffer merely existing.
	bool ShaderReady = false;
	bool ShaderFailed = false;

	// [2a] Set by VkShaderManager once gpuparticles' scene-depth fragment
	// variants (single/multisample x flat/layered) have compiled for every pass.
	// The soft fade (r_gpuparticles_soft) asks for readable scene depth only when
	// this is set; without it particles draw with hard edges, exactly as before
	// 2a, and no read-only depth pass is ever begun.
	bool SceneDepthShaderReady = false;

	bool IsDrawable() const { return ShaderReady && !ShaderFailed && mBuffer != nullptr && mQuads != nullptr; }

	// Diagnostics. CountDraw is called per particle draw; DebugReport prints at
	// most every two seconds and only while r_gpuparticles_debug is on.
	void CountDraw() { mDrawsSinceReport++; }
	void DebugReport(uint64_t written);

private:
	void Upload(const void *records, unsigned first, unsigned count);

	IDataBuffer *mBuffer = nullptr;
	IVertexBuffer *mQuads = nullptr;
	unsigned mRingSize = 0;

	uint64_t mSyncedSerial = 0;
	uint64_t mSyncedWritten = 0;

	uint64_t mUploadedSinceReport = 0;
	unsigned mFullSinceReport = 0;
	unsigned mSpanSinceReport = 0;
	unsigned mDrawsSinceReport = 0;
	uint64_t mWrittenAtReport = 0;
	uint64_t mLastReportMs = 0;
	uint64_t mLoggedSerial = 0;
	bool mWarnedSizeMismatch = false;
};
