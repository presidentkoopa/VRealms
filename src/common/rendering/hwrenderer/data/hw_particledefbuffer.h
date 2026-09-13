/*
** hw_particledefbuffer.h
**
** [PARTICLEDEFS] The GPU half of the particle definitions table: one storage
** buffer of fixed-size definitions that gpuparticles.vp (and, from stage 2c/2d,
** gpuparticles.fp) reads by the index each particle record carries.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: a stage 1 particle record carried every parameter of its
** look, so there was no room for curves over life, flipbooks, lighting, softness
** or collision. A definition holds all of that once, and a record keeps only
** what differs per particle. See "Engine docs/GPU_PARTICLES_STAGE2_PLAN.md" 2b.
**
** The CPU table -- the PARTICLEDEFS parser, the named definitions, the inline
** cache that SpawnGpuParticles feeds -- lives in src/gamedata/particledefs.cpp and
** survives a renderer rebuild. This class, the descriptor binding and the shader
** evaluation are what a rebuild replaces.
**
** Vulkan only, exactly like GpuParticleBuffer: created beside it in
** VulkanRenderDevice::InitializeState, null on GL and GLES. Set 1 binding 7
** (vk_descriptorset.cpp), vertex and fragment stages.
**
*/

#pragma once

#include <cstdint>

class IDataBuffer;

class ParticleDefinitionBuffer
{
public:
	// One definition: sixteen vec4s, std430, no padding. Must equal
	// sizeof(ParticleDefinitionGpu) in particledefs.h and the ParticleDefinitionData
	// struct in vk_shader.cpp's prolog (both assert it).
	static const unsigned RECORD_BYTES = 256;

	// Slots 0..255 hold named definitions (PARTICLEDEFS lumps), 256..511 the
	// inline cache that SpawnGpuParticles' parameter tuples fill. Separate ranges,
	// so inline churn can never touch a named definition.
	static const unsigned NAMED_SLOTS = 256;
	static const unsigned INLINE_SLOTS = 256;
	static const unsigned SLOTS = NAMED_SLOTS + INLINE_SLOTS;

	ParticleDefinitionBuffer();
	~ParticleDefinitionBuffer();

	// THE SYNC RULE. The CPU table stamps each slot with the table generation at
	// which it last changed, and bumps the table generation on every change. This
	// copies every slot stamped later than the generation it last synced, in runs,
	// then remembers `generation`. A new buffer starts at 0 and so takes every slot
	// ever written; slots never written are the zeros it was created with.
	//
	// Frames in flight: one persistently mapped buffer, like the particle ring. The
	// inline cache only rewrites a slot once every particle that used it is dead,
	// plus a margin for frames already recorded.
	void Sync(const void *definitions, const uint64_t *slotGenerations, unsigned slotCount, uint64_t generation);

	IDataBuffer *GetBuffer() const { return mBuffer; }
	uint64_t GetSyncedGeneration() const { return mSyncedGeneration; }
	uint64_t GetUploadedSlots() const { return mUploadedSlots; }	// since creation, for the `particles` CCMD

private:
	IDataBuffer *mBuffer = nullptr;
	uint64_t mSyncedGeneration = 0;
	uint64_t mUploadedSlots = 0;
	bool mWarnedCount = false;
};
