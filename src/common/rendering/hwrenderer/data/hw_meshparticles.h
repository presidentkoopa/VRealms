/*
** hw_meshparticles.h
**
** [MESHPARTICLES] GPU particles drawn as small instanced meshes -- chunks, shards,
** splinters, casings -- instead of flat cards.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: a GPU particle is a camera-facing quad. Debris that should read as
** matter -- a concrete chip turning over, a splinter landing flat, a casing on the floor --
** needs real geometry, and thousands of pieces of it. A PARTICLEDEFS definition that names
** a `mesh` (particledefs.cpp) keeps everything a particle already has -- the ring record,
** the definition's ramps, motion, spin and collision -- and is drawn as that model, one
** instanced draw per mesh definition, in the opaque pass, writing depth. See "Engine docs/
** COLLISION_DEBRIS_MESH_PLAN.md" #10 and "Engine docs/MESH_PARTICLES_10_IMPL_NOTES.md".
**
** HOW AN INSTANCE FINDS ITS PARTICLE (review D3). Every definition's particles are
** interleaved in the one ring, so instance i is not ring slot i. Each frame this class keeps,
** per mesh definition, the ring slots of its live particles -- noted when records are written,
** dropped when they die or are overwritten -- compacts them into one list and uploads it. The
** draw for definition d covers [first_d, first_d + count_d) of that list, and meshparticles.vp
** reads meshInstanceSlots[gl_InstanceIndex] (Vulkan's gl_InstanceIndex includes the draw's
** firstInstance).
**
** THE BILLBOARD STEPS ASIDE. While a definition draws as a mesh, its slot in the particle
** definitions buffer (set 1 binding 7) is uploaded as zeros, so gpuparticles.vp collapses its
** quads before doing anything else (ParticleDefinitionBuffer::Sync, GetBillboardHidden), and its
** real bytes live here, where only meshparticles.vp reads them. With r_meshparticles off, the
** mesh effect not compiled, or the model not loaded, the definition draws as its billboard.
**
** WHAT A RENDER REBUILD KEEPS: the `mesh` key, the md3 validation and the mesh list on the CPU
** table (particledefs.cpp), and the live-slot bookkeeping below. It replaces the storage buffer,
** the instanced draw and meshparticles.vp/.fp.
**
** NETPLAY: presentation only. It reads the particle ring and never writes it; nothing is read
** back; no RNG (orientation comes from the record's seed); a definition refused, or a model that
** fails to load, changes only that machine's pixels.
**
** Vulkan only, exactly like GpuParticleBuffer: created beside it in
** VulkanRenderDevice::InitializeState, null on GL and GLES. Set 1 binding 9, vertex stage.
**
*/

#pragma once

#include <cstdint>
#include "tarray.h"
#include "zstring.h"
#include "textureid.h"

class IDataBuffer;
class FRenderState;

// One definition that names a mesh, as the CPU table (particledefs.cpp) hands it to the
// renderer. Everything in it was checked when the PARTICLEDEFS lump loaded.
struct ParticleMeshDefinition
{
	int Slot = -1;				// the named definition slot, 0 .. MeshParticleBuffer::DEFINITION_SLOTS - 1
	FString Path;				// the md3's full path in its package, as FindModel takes it
	FTextureID Skin;			// the texture the mesh is drawn with; valid
	int Frame = 0;				// the frame drawn, 0 .. Frames - 1
	unsigned Frames = 0;		// frames in the file
	unsigned Vertices = 0;		// the one surface's vertices per frame
	unsigned Triangles = 0;		// 1 .. MeshParticleBuffer::MAX_TRIANGLES
	// The drawn frame's bounds in the axes the model vertex buffer holds (md3 x, z, y -- y up),
	// in md3 units, which draw as map units at scale 1.
	float BoundsMin[3] = { 0.f, 0.f, 0.f };
	float BoundsMax[3] = { 0.f, 0.f, 0.f };
	float Diameter = 0.f;		// twice the farthest vertex of that frame from the origin; above 0
};

class MeshParticleBuffer
{
public:
	// THE STORAGE BUFFER (set 1 binding 9), std430, laid out as meshparticles.vp declares it:
	//   0       vec4 header: x live instances uploaded, y instance capacity
	//   16      DEFINITION_SLOTS records of DEFINITION_BYTES, indexed by the named definition slot
	//           (the record's d.x): the definition's 256 bytes as the CPU table holds them, then
	//           vec4 bounds min (xyz, w diameter -- 0 for a slot that is not a mesh), vec4 bounds
	//           max (xyz, w 0)
	//   INSTANCES_OFFSET  uint ring slots, one per live instance, grouped by definition
	static const unsigned HEADER_BYTES = 16;
	// [RAMPS] 416, not 288: the particle definition record grew from 256 to 384 when key2
	// added eight vec4s of ramped look channels (particledefs.h), and this is that record
	// plus two vec4s of bounds. THREE places mirror that layout -- this, ParticleDefinitionData
	// in vk_shader.cpp, and MeshParticleDefinition in meshparticles.vp -- and only the two
	// static_asserts in hw_meshparticles.cpp and particledefs.cpp catch a drift. The shader
	// copies are checked by nothing at all and read garbage silently if they fall behind.
	// [BUOYANCY] 432, not 416: the particle definition record grew to 400 bytes when it gained
	// buoyancy, and this is always that record plus two vec4s of bounds. hw_meshparticles.cpp
	// static_asserts BOTH this relationship and the resulting INSTANCES_OFFSET, which is the
	// only thing connecting this file to particledefs.h -- get it wrong and the GPU reads
	// garbage with no error at all.
	static const unsigned DEFINITION_BYTES = 432;
	static const unsigned DEFINITION_SLOTS = 256;	// ParticleDefinitionBuffer::NAMED_SLOTS: only named definitions have meshes
	static const unsigned INSTANCES_OFFSET = HEADER_BYTES + DEFINITION_SLOTS * DEFINITION_BYTES;	// 73,744
	static const unsigned INSTANCE_BYTES = 4;

	// The most triangles a mesh particle may have. The plan's cost target (1.2 ms an eye for
	// 4,000 chunks) is set against this; particledefs.cpp refuses a mesh with more.
	static const unsigned MAX_TRIANGLES = 64;

	MeshParticleBuffer();
	~MeshParticleBuffer();

	// Once a scene, BEFORE ParticleDefinitionBuffer::Sync (it decides which billboards step
	// aside). `meshes` is the CPU table's mesh list and `meshGeneration` its generation;
	// `definitions` the CPU table's slots (ParticleDefinitionGpu, 256 bytes each) and
	// `definitionGeneration` the table's generation. Resolves each mesh's model through the
	// existing loader (FindModel) and builds its vertex buffer, copies the mesh definitions into
	// the storage buffer when either generation moves, and works out GetBillboardHidden from
	// r_meshparticles (renderer-read here, every frame), the shader and the models.
	void SyncDefinitions(const ParticleMeshDefinition *meshes, unsigned meshCount, uint64_t meshGeneration,
		const void *definitions, unsigned definitionCount, uint64_t definitionGeneration);

	// One byte per named definition slot (DEFINITION_SLOTS of them): 1 = drawn as a mesh this
	// frame, so its billboard must not draw. Pass to ParticleDefinitionBuffer::Sync.
	const uint8_t *GetBillboardHidden() const { return mBillboardHidden; }

	// Once a scene, AFTER the ring's GpuParticleBuffer::Sync, with the same ring and this frame's
	// level time (the clock the shaders age records by). Notes the mesh particles written since
	// the last call, drops the dead and overwritten, and uploads the grouped slot list when it
	// changed. The same sync rule as the ring: a new serial, the cursor going back, or a burst of
	// a whole ring or more rescans everything.
	void Sync(const void *records, unsigned recordCount, uint64_t serial, uint64_t written, float levelTime);

	// A lit mesh particle (its definition's `lit` above 0) is alive at `levelTime` -- the view
	// light list (SyncViewLights) is worth filling for it. Render-side only.
	bool LitAliveAt(float levelTime) const { return mLitUntil > levelTime; }

	// Something to draw this frame, with everything the draw reads: the effect compiled, the
	// buffer, r_meshparticles on, the ring and view light buffers it shares.
	bool IsDrawable() const;

	// The opaque-pass draw: one DrawIndexedInstanced per mesh definition with live instances.
	// Sets what it needs (effect, opaque style, depth write, DF_Less, no culling, the skin) and
	// leaves EFF_NONE, no culling and the flat vertex buffer bound; the caller restores its own
	// render style and depth function.
	void Draw(FRenderState &state);

	// Set by VkShaderManager once the meshparticles effect has compiled for every pass. A
	// pipeline for an effect whose program is missing dereferences null, so everything is
	// gated on this, as GpuParticleBuffer's draw is.
	bool ShaderReady = false;
	bool ShaderFailed = false;

	// For the `particles` CCMD (particledefs.cpp).
	unsigned GetUploadedInstances() const { return mUploadedInstances; }
	unsigned GetInstanceCapacity() const { return mCapacity; }
	FString DescribeSlot(int slot) const;

private:
	struct LiveInstance
	{
		uint32_t RingSlot;
		float Birth;		// the record's a.w when it was noted: a different birth there means overwritten
	};

	struct MeshSlot
	{
		bool Defined = false;		// this named slot has a mesh in the CPU list
		ParticleMeshDefinition Mesh;
		bool Lit = false;			// the definition's lit above 0
		int ModelIndex = -1;		// into Models, checked against Mesh.Path every frame
		bool ModelFailed = false;	// FindModel or the model's shape said no; retried when the list changes
		bool ModelReady = false;	// resolved, with a vertex buffer, this frame
		TArray<LiveInstance> Live;
		unsigned FirstInstance = 0;	// this frame's range in the uploaded list
		unsigned InstanceCount = 0;
	};

	bool EnsureModel(MeshSlot &slot);
	void NoteRecords(const uint8_t *records, unsigned first, unsigned count, unsigned usable, float levelTime);
	void Upload();

	IDataBuffer *mBuffer = nullptr;
	unsigned mCapacity = 0;			// instances the buffer holds: the ring's capacity

	MeshSlot mSlots[DEFINITION_SLOTS];
	TArray<unsigned> mDefinedSlots;	// the slots with Defined set, in slot order
	uint8_t mBillboardHidden[DEFINITION_SLOTS] = {};

	uint64_t mMeshGeneration = 0;
	uint64_t mDefinitionGeneration = 0;
	bool mDefinitionsWritten = false;	// the definition records match the generations above
	bool mNeedFullScan = true;

	uint64_t mSyncedSerial = 0;
	uint64_t mSyncedWritten = 0;
	bool mListDirty = true;
	unsigned mUploadedInstances = 0;
	bool mWarnedCapacity = false;

	static constexpr float NEVER_ALIVE = -1.0e30f;
	float mLitUntil = NEVER_ALIVE;
};
