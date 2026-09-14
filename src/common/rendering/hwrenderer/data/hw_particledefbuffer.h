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
** [2c] It also carries the PARTICLE ATLAS LAYER LIST from the CPU table to the
** backend that builds the atlas (VkTextureManager::CreateParticleAtlas, fixed set
** binding 4): which texture fills each layer and where in the layer it goes. The
** list is copied here, by generation, from HWDrawInfo::ProcessScene, so the
** common renderer never reaches into gamedata.
**
*/

#pragma once

#include <cstdint>
#include "tarray.h"
#include "textureid.h"

class IDataBuffer;

// [2c] One layer of the particle atlas, as the CPU table (particledefs.cpp) hands
// it to the renderer: the texture whose pixels fill the layer, and the rectangle
// they fill, in fractions of the square layer's side. Every frame of one flipbook
// shares one scale -- the largest frame side in the run fills the layer -- and
// sits centred, so the art keeps its proportions and a puff that grows in the art
// still grows on screen. The rest of the layer is transparent.
struct ParticleAtlasLayer
{
	FTextureID Texture;
	float Left;
	float Top;
	float Width;
	float Height;
};

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

	// [2c] THE PARTICLE ATLAS holds at most this many layers, one per frame; a
	// flipbook's frames are consecutive layers. 256 layers of 256 x 256 with mips is
	// the approved budget (about 85 MiB; the byte math is in "Engine docs/
	// STAGE2C_IMPL_NOTES.md"). The CPU table refuses a definition that would need
	// more, so the list handed over never exceeds it.
	static const unsigned ATLAS_LAYERS = 256;

	// [2c] Takes the CPU table's atlas layer list when `generation` differs from the
	// one last taken (a new buffer, after a renderer restart, starts at 0 and so takes
	// the current list). The backend compares GetAtlasGeneration with the generation
	// it built from, and rebuilds when they differ.
	void SyncAtlasLayers(const ParticleAtlasLayer *layers, unsigned count, uint64_t generation);
	const TArray<ParticleAtlasLayer> &GetAtlasLayers() const { return mAtlasLayers; }
	uint64_t GetAtlasGeneration() const { return mAtlasGeneration; }

	// [2c] What the backend last built, for the `particles` CCMD: layers, the layer
	// side in pixels, and the image's bytes (0 layers = the 1x1 placeholder only).
	void SetAtlasBuilt(unsigned layers, int layerSize, uint64_t bytes)
	{
		mAtlasBuiltLayers = layers;
		mAtlasBuiltSize = layerSize;
		mAtlasBuiltBytes = bytes;
	}
	unsigned GetAtlasBuiltLayers() const { return mAtlasBuiltLayers; }
	int GetAtlasBuiltSize() const { return mAtlasBuiltSize; }
	uint64_t GetAtlasBuiltBytes() const { return mAtlasBuiltBytes; }

private:
	IDataBuffer *mBuffer = nullptr;
	uint64_t mSyncedGeneration = 0;
	uint64_t mUploadedSlots = 0;
	bool mWarnedCount = false;

	// [2c] The atlas layer list, see SyncAtlasLayers.
	TArray<ParticleAtlasLayer> mAtlasLayers;
	uint64_t mAtlasGeneration = 0;
	bool mWarnedAtlasCount = false;
	unsigned mAtlasBuiltLayers = 0;
	int mAtlasBuiltSize = 0;
	uint64_t mAtlasBuiltBytes = 0;
};
