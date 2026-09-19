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
** [ATLASBC7] And the COMPRESSED particle atlas's list (VkTextureManager::
** ParticleAtlasCompressed, fixed set binding 10): flipbooks stored as premultiplied
** BC7 DDS frames, uploaded as they are stored, at a quarter of the memory. The
** settings both atlases are laid out by are ParticleAtlasPolicy, below. See "Engine
** docs/PARTICLE_ATLAS_COMPRESSED_IMPL_NOTES.md".
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

// [ATLASBC7] One layer of the COMPRESSED particle atlas: a BC7 DDS frame's stored levels, uploaded as they are. The frame is
// square, a power of two and no smaller than the atlas's side; StartLevel is the stored level that becomes the atlas's level 0
// (log2 of the frame's side over the atlas's), and the stored levels below it follow down to 1 x 1. Its own struct, so
// ParticleAtlasLayer keeps its bytes.
struct ParticleCompressedAtlasLayer
{
	FTextureID Texture;
	int StartLevel;
};

class ParticleDefinitionBuffer
{
public:
	// One definition: sixteen vec4s, std430, no padding. Must equal
	// sizeof(ParticleDefinitionGpu) in particledefs.h and the ParticleDefinitionData
	// struct in vk_shader.cpp's prolog (both assert it).
	// [RAMPS] 384, not 256: key2 added eight vec4s of ramped look channels (roughness,
	// churn, gravity, drag) riding the same eight time keys as `key`. particledefs.cpp
	// static_asserts both this size and every member offset against ParticleDefinitionGpu,
	// and ParticleDefinitionData in vk_shader.cpp must match the struct too -- nothing
	// checks THAT copy at build time, so a drift there reads garbage and reports nothing.
	static const unsigned RECORD_BYTES = 384;

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
	//
	// [MESHPARTICLES] `billboardHidden` (MeshParticleBuffer::GetBillboardHidden, one byte per slot for
	// the first `hiddenCount` slots): a slot marked 1 draws as a mesh this frame, so its billboard
	// must not draw. It is uploaded as 256 zero bytes -- key count 0 and size 0, which gpuparticles.vp
	// collapses before anything else, and LOOK_* 0, so an invisible billboard never switches a blend,
	// the view lights or a depth pass on -- and copied again, real or zero, whenever its mark flips,
	// generation or not. Null (the default) marks nothing: every slot is copied exactly as before.
	void Sync(const void *definitions, const uint64_t *slotGenerations, unsigned slotCount, uint64_t generation,
		const uint8_t *billboardHidden = nullptr, unsigned hiddenCount = 0);

	// [MESHPARTICLES] Whether the last Sync uploaded this slot as zeros because it draws as a mesh.
	bool IsBillboardHidden(unsigned slot) const { return slot < SLOTS && mBillboardHidden[slot] != 0; }

	IDataBuffer *GetBuffer() const { return mBuffer; }
	uint64_t GetSyncedGeneration() const { return mSyncedGeneration; }
	uint64_t GetUploadedSlots() const { return mUploadedSlots; }	// since creation, for the `particles` CCMD

	// [2d] WHAT A DEFINITION'S PARTICLES NEED FROM THE DRAW -- one byte per slot, worked
	// out from the definition's own bytes whenever Sync copies that slot, so the common
	// renderer never reaches into gamedata. GpuParticleBuffer::Sync looks the byte up for
	// every record it uploads and keeps how long particles needing each piece stay alive;
	// HWDrawInfo turns the premultiplied blend, the view light fill and the read-only
	// depth pass on only while they are. A slot never written is 0: needs nothing.
	enum : uint8_t
	{
		LOOK_OCCLUDES = 1,	// some alpha key is above 0 -- the draw needs the premultiplied blend
		LOOK_LIT = 2,		// occludes AND lit above 0 -- the view light list is worth filling
		LOOK_SOFT = 4,		// its own soft distance is above 0 -- the read-only depth pass is needed
	};
	const uint8_t *GetSlotLooks() const { return mSlotLooks; }

	// [2d] Where Sync finds those in a definition's 256 bytes. particledefs.cpp asserts
	// every one against ParticleDefinitionGpu.
	static const unsigned KEYS = 8;
	static const unsigned KEY_STRIDE = 16;			// key[i] starts at i * 16
	static const unsigned KEY_ALPHA_OFFSET = 8;		// key[i].z, alpha 0..1
	static const unsigned KEY_COUNT_OFFSET = 172;	// motion.w, the key count 1..8
	static const unsigned LOOK_OFFSET = 192;		// look: x lit 0..1, y soft (-1 unset)

	// [2c] THE PARTICLE ATLAS holds one layer per frame; a flipbook's frames are
	// consecutive layers. Stage 2c's approved budget was 256 layers of 256 x 256 with
	// mips (about 85 MiB; the byte math is in "Engine docs/STAGE2C_IMPL_NOTES.md").
	// [ATLASBC7] Each of the two atlases, uncompressed and compressed, now holds at most
	// ParticleAtlasPolicy::LayersPerAtlas layers ("Flipbook frames per atlas",
	// r_gpuparticles_atlas_layers), between these: MIN is the least any Vulkan device
	// allows in an array (maxImageArrayLayers), MAX the most this fork asks for (the
	// owner's 3080 Ti allows exactly that). The CPU table lays out only what fits --
	// thinning long flipbooks, or leaving the last ones untextured -- so a list handed
	// over never exceeds its cap.
	static const unsigned ATLAS_LAYERS_MIN = 256;
	static const unsigned ATLAS_LAYERS_MAX = 2048;

	// [2c] Takes the CPU table's atlas layer list when `generation` differs from the
	// one last taken (a new buffer, after a renderer restart, starts at 0 and so takes
	// the current list). The backend compares GetAtlasGeneration with the generation
	// it built from, and rebuilds when they differ.
	// [ATLASBC7] The compressed atlas's list goes with it, by its own generation, every
	// layer `compressedSide` pixels on a side. A compressed generation of 0 (the
	// defaults) hands nothing over: the compressed list stays as it was.
	void SyncAtlasLayers(const ParticleAtlasLayer *layers, unsigned count, uint64_t generation,
		const ParticleCompressedAtlasLayer *compressedLayers = nullptr, unsigned compressedCount = 0, int compressedSide = 0,
		uint64_t compressedGeneration = 0);
	const TArray<ParticleAtlasLayer> &GetAtlasLayers() const { return mAtlasLayers; }
	uint64_t GetAtlasGeneration() const { return mAtlasGeneration; }
	// [ATLASBC7] The compressed atlas's list, its layer side in pixels (0 = no layers) and its generation.
	const TArray<ParticleCompressedAtlasLayer> &GetCompressedAtlasLayers() const { return mCompressedAtlasLayers; }
	int GetCompressedAtlasSide() const { return mCompressedAtlasSide; }
	uint64_t GetCompressedAtlasGeneration() const { return mCompressedAtlasGeneration; }

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
	// [ATLASBC7] The same for the compressed atlas.
	void SetCompressedAtlasBuilt(unsigned layers, int side, uint64_t bytes)
	{
		mCompressedAtlasBuiltLayers = layers;
		mCompressedAtlasBuiltSide = side;
		mCompressedAtlasBuiltBytes = bytes;
	}
	unsigned GetCompressedAtlasBuiltLayers() const { return mCompressedAtlasBuiltLayers; }
	int GetCompressedAtlasBuiltSide() const { return mCompressedAtlasBuiltSide; }
	uint64_t GetCompressedAtlasBuiltBytes() const { return mCompressedAtlasBuiltBytes; }

private:
	IDataBuffer *mBuffer = nullptr;
	uint64_t mSyncedGeneration = 0;
	uint64_t mUploadedSlots = 0;
	bool mWarnedCount = false;

	// [2d] LOOK_* per slot, see GetSlotLooks.
	uint8_t mSlotLooks[SLOTS] = {};

	// [MESHPARTICLES] 1 per slot last uploaded as zeros because it draws as a mesh, see Sync.
	uint8_t mBillboardHidden[SLOTS] = {};

	// [2c] The atlas layer list, see SyncAtlasLayers.
	TArray<ParticleAtlasLayer> mAtlasLayers;
	uint64_t mAtlasGeneration = 0;
	bool mWarnedAtlasCount = false;
	unsigned mAtlasBuiltLayers = 0;
	int mAtlasBuiltSize = 0;
	uint64_t mAtlasBuiltBytes = 0;

	// [ATLASBC7] The compressed atlas's list and what the backend built of it, see SyncAtlasLayers.
	TArray<ParticleCompressedAtlasLayer> mCompressedAtlasLayers;
	int mCompressedAtlasSide = 0;
	uint64_t mCompressedAtlasGeneration = 0;
	bool mWarnedCompressedAtlasCount = false;
	unsigned mCompressedAtlasBuiltLayers = 0;
	int mCompressedAtlasBuiltSide = 0;
	uint64_t mCompressedAtlasBuiltBytes = 0;
};

// [ATLASBC7] THE ATLAS SETTINGS the CPU table lays flipbook frames out by (RefreshParticleAtlasLayout, particledefs.h): the
// menu's settings and what the device can hold, built every scene by GpuParticleAtlasPolicy (hw_cvars.cpp). The defaults are the
// layout at load, before the renderer's first scene: every flipbook uncompressed, the most frames an atlas may ever hold.
struct ParticleAtlasPolicy
{
	bool Compressed = false;	// BC7 flipbooks may use the compressed atlas: r_gpuparticles_atlas_compressed, on a device that can
	int CompressedSide = 512;	// the compressed atlas's side cap in pixels, 256 / 512 / 1024 (r_gpuparticles_atlas_compressed_size)
	unsigned LayersPerAtlas = ParticleDefinitionBuffer::ATLAS_LAYERS_MAX;	// frames each atlas may hold, never above the device's limit
	bool ThinOverflow = true;	// flipbooks wanting more: true, long ones play fewer frames; false, those past the cap draw as dots

	bool operator==(const ParticleAtlasPolicy &other) const
	{
		return Compressed == other.Compressed && CompressedSide == other.CompressedSide && LayersPerAtlas == other.LayersPerAtlas &&
			ThinOverflow == other.ThinOverflow;
	}
	bool operator!=(const ParticleAtlasPolicy &other) const { return !(*this == other); }
};
