/*
** hw_meshparticles.cpp
**
** [MESHPARTICLES] GPU particles drawn as small instanced meshes. See the header.
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

#include <algorithm>
#include <cstring>
#include "hw_meshparticles.h"
#include "hw_gpuparticlebuffer.h"
#include "hw_particledefbuffer.h"
#include "hw_viewlightbuffer.h"
#include "hw_modelvertexbuffer.h"
#include "hw_renderstate.h"
#include "hw_perflog.h"
#include "hw_cvars.h"
#include "hwrenderer/data/buffers.h"
#include "shaderuniforms.h"
#include "flatvertices.h"
#include "model.h"
#include "modelrenderer.h"
#include "texturemanager.h"
#include "v_video.h"
#include "c_cvars.h"
#include "i_time.h"
#include "printf.h"

// [MESHPARTICLES] r_meshparticles -- "Mesh particles" ("Engine docs/COLLISION_DEBRIS_MESH_PLAN.md"
// #10). On (the default -- owner, 2026-09-14: effects our mods use are on): a PARTICLEDEFS
// definition that names a `mesh` draws each of its particles as that model, instanced, in the
// opaque pass. Off: the same definitions draw as their billboards, exactly as they would with no
// mesh support at all. Definitions without a mesh never change either way. Renderer-read every
// frame (SyncDefinitions), so chunks already in the air switch with the menu open. Presentation
// only -- not SERVERINFO; nothing reads it back into the playsim. Defined here, beside the class
// it switches, as r_gpuparticles_looks is beside the definitions buffer.
CVARD(Bool, r_meshparticles, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "draw particle definitions that name a mesh as instanced 3D meshes; off = as their billboards (Vulkan only)")

static_assert(MeshParticleBuffer::DEFINITION_SLOTS == ParticleDefinitionBuffer::NAMED_SLOTS,
	"mesh particle definition records are indexed by the named definition slot");
static_assert(MeshParticleBuffer::DEFINITION_BYTES == ParticleDefinitionBuffer::RECORD_BYTES + 32,
	"a mesh particle definition record is a particle definition and two vec4s of bounds -- see MeshParticleDefinition in meshparticles.vp");
// [RAMPS] 106,512 now, not 73,744: 16 header bytes + 256 records of 416. The number is
// spelled out rather than computed so that a layout change has to be acknowledged HERE too.
static_assert(MeshParticleBuffer::INSTANCES_OFFSET == 106512,
	"the instance list starts after the header and 256 definition records -- see MeshParticleSSO in meshparticles.vp");

namespace
{
	// Here only so FModel::BuildVertexBuffer can ask a renderer for a buffer object and its type.
	// GLModelRendererType is the hardware renderer's type on every backend (FHWModelRenderer), so the
	// buffer built here is the very one ordinary model drawing would build and use. Draws nothing.
	class MeshParticleModelBuilder : public FModelRenderer
	{
	public:
		ModelRendererType GetType() const override { return GLModelRendererType; }
		void BeginDrawModel(FRenderStyle style, int smf_flags, const VSMatrix &objectToWorldMatrix, bool mirrored) override {}
		void EndDrawModel(FRenderStyle style, int smf_flags) override {}
		IModelVertexBuffer *CreateVertexBuffer(bool needindex, bool singleframe) override { return new FModelVertexBuffer(needindex, singleframe); }
		VSMatrix GetViewToWorldMatrix() override
		{
			VSMatrix identity;
			identity.loadIdentity();
			return identity;
		}
		void BeginDrawHUDModel(FRenderStyle style, const VSMatrix &objectToWorldMatrix, bool mirrored, int smf_flags) override {}
		void EndDrawHUDModel(FRenderStyle style, int smf_flags) override {}
		void SetInterpolation(double interpolation) override {}
		void SetMaterial(FGameTexture *skin, bool clampNoFilter, FTranslationID translation) override {}
		void DrawArrays(int start, int count) override {}
		void DrawElements(int numIndices, size_t offset) override {}
	};

	float RecordFloat(const uint8_t *bytes, unsigned offset)
	{
		float value;
		memcpy(&value, bytes + offset, sizeof(float));
		return value;
	}
}

MeshParticleBuffer::MeshParticleBuffer()
{
	// One instance per ring slot at most: every instance is a different live record. The ring's
	// capacity is latched at its first use (GpuParticleBuffer, created just before this).
	mCapacity = (unsigned)GpuParticleRingCapacity();
	const size_t bytes = (size_t)INSTANCES_OFFSET + (size_t)mCapacity * INSTANCE_BYTES;

	// Storage buffer, persistently mapped, set 1 binding 9 on Vulkan (vk_descriptorset.cpp).
	// Same creation path as the particle ring.
	mBuffer = screen->CreateDataBuffer(MESHPARTICLE_BINDINGPOINT, true, false);
	mBuffer->SetData(bytes, nullptr, BufferUsageType::Persistent);

	// A persistent allocation is not guaranteed to be zeroed. Zero is no mesh in any slot (a
	// diameter of 0, which meshparticles.vp collapses) and no instances.
	mBuffer->Map();
	if (mBuffer->Memory() != nullptr)
		memset(mBuffer->Memory(), 0, bytes);
	mBuffer->Unmap();

	Printf("MeshParticles: buffer created -- %u B header + %u definition slots x %u B + %u instances x %u B = %llu bytes\n",
		HEADER_BYTES, DEFINITION_SLOTS, DEFINITION_BYTES, mCapacity, INSTANCE_BYTES, (unsigned long long)bytes);
}

MeshParticleBuffer::~MeshParticleBuffer()
{
	delete mBuffer;
}

//==========================================================================
//
// The mesh definitions, their models, and which billboards step aside
//
//==========================================================================

void MeshParticleBuffer::SyncDefinitions(const ParticleMeshDefinition *meshes, unsigned meshCount, uint64_t meshGeneration,
	const void *definitions, unsigned definitionCount, uint64_t definitionGeneration)
{
	if (meshes == nullptr) meshCount = 0;

	// cpu fx.meshparticles: this and Sync, main view and camera textures alike, only once some
	// definition names a mesh. PerfLog sums a frame's samples of one name.
	const bool timed = PerfLog::GroupsWanted() && (meshCount > 0 || mDefinedSlots.Size() > 0);
	const uint64_t startNs = timed ? I_nsTime() : 0;

	// THE MESH LIST. It only changes when PARTICLEDEFS lumps load, so this is a generation compare
	// on every other frame. Every slot starts over: its model is resolved again and its live list is
	// rebuilt by a full scan of the ring.
	if (meshGeneration != mMeshGeneration)
	{
		for (unsigned s = 0; s < DEFINITION_SLOTS; s++)
		{
			MeshSlot &slot = mSlots[s];
			slot.Defined = false;
			slot.Mesh = ParticleMeshDefinition();
			slot.Lit = false;
			slot.ModelIndex = -1;
			slot.ModelFailed = false;
			slot.ModelReady = false;
			slot.Live.Clear();
			slot.FirstInstance = 0;
			slot.InstanceCount = 0;
		}
		mDefinedSlots.Clear();

		for (unsigned i = 0; i < meshCount; i++)
		{
			// particledefs.cpp never hands over anything these reject; they only keep a bad
			// entry from reaching the buffer or the draw.
			const ParticleMeshDefinition &mesh = meshes[i];
			if (mesh.Slot < 0 || mesh.Slot >= (int)DEFINITION_SLOTS || !(mesh.Diameter > 0.f) ||
				mesh.Triangles == 0 || mesh.Triangles > MAX_TRIANGLES || mesh.Vertices == 0 || mesh.Frame < 0 ||
				(unsigned)mesh.Frame >= mesh.Frames || !mesh.Skin.isValid())
				continue;
			MeshSlot &slot = mSlots[mesh.Slot];
			if (slot.Defined)
				continue;
			slot.Defined = true;
			slot.Mesh = mesh;
			mDefinedSlots.Push((unsigned)mesh.Slot);
		}
		if (mDefinedSlots.Size() > 1)
			std::sort(mDefinedSlots.Data(), mDefinedSlots.Data() + mDefinedSlots.Size());

		mMeshGeneration = meshGeneration;
		mDefinitionsWritten = false;
		mNeedFullScan = true;
		mListDirty = true;
		mLitUntil = NEVER_ALIVE;
	}

	// THE DEFINITION RECORDS, copied whenever either generation moves: a mesh slot gets the CPU
	// table's 256 bytes as they are (its size keys already hold scale x diameter) and its bounds;
	// every other slot is zero, which meshparticles.vp reads as "not a mesh".
	if (!mDefinitionsWritten || definitionGeneration != mDefinitionGeneration)
	{
		if (mBuffer != nullptr)
		{
			mBuffer->Map();
			uint8_t *dst = (uint8_t *)mBuffer->Memory();
			if (dst != nullptr)
			{
				const uint8_t *src = (const uint8_t *)definitions;
				for (unsigned s = 0; s < DEFINITION_SLOTS; s++)
				{
					MeshSlot &slot = mSlots[s];
					uint8_t *record = dst + HEADER_BYTES + (size_t)s * DEFINITION_BYTES;
					if (!slot.Defined || src == nullptr || s >= definitionCount)
					{
						memset(record, 0, DEFINITION_BYTES);
						slot.Lit = false;
						continue;
					}
					const uint8_t *definition = src + (size_t)s * ParticleDefinitionBuffer::RECORD_BYTES;
					memcpy(record, definition, ParticleDefinitionBuffer::RECORD_BYTES);
					const ParticleMeshDefinition &mesh = slot.Mesh;
					const float bounds[8] = {
						mesh.BoundsMin[0], mesh.BoundsMin[1], mesh.BoundsMin[2], mesh.Diameter,
						mesh.BoundsMax[0], mesh.BoundsMax[1], mesh.BoundsMax[2], 0.f };
					memcpy(record + ParticleDefinitionBuffer::RECORD_BYTES, bounds, sizeof(bounds));
					slot.Lit = RecordFloat(definition, ParticleDefinitionBuffer::LOOK_OFFSET) > 0.f;
				}
				mDefinitionsWritten = true;
				mDefinitionGeneration = definitionGeneration;
			}
			mBuffer->Unmap();
		}
	}

	// THE MODELS, AND WHAT DRAWS AS A MESH THIS FRAME. A definition's billboard steps aside only
	// when its mesh really draws: the switch on, the effect compiled for every pass, the buffers the
	// shader reads, and its model loaded with a vertex buffer. Anything short of that and it keeps
	// drawing as its billboard, so nothing ever vanishes.
	const bool drawOn = r_meshparticles && ShaderReady && !ShaderFailed && mBuffer != nullptr && mDefinitionsWritten &&
		screen->mGpuParticles != nullptr && screen->mViewLights != nullptr;
	memset(mBillboardHidden, 0, sizeof(mBillboardHidden));
	for (unsigned s : mDefinedSlots)
	{
		MeshSlot &slot = mSlots[s];
		slot.ModelReady = EnsureModel(slot);
		mBillboardHidden[s] = (drawOn && slot.ModelReady) ? 1 : 0;
	}

	if (timed)
		PerfLog::AddCpuSample("fx.meshparticles", (I_nsTime() - startNs) / 1.0e6);
}

// The slot's model, through the existing model loading, with a vertex buffer. InitModels empties
// Models after PARTICLEDEFS has loaded (it runs from P_Init), so an index is only trusted while the
// model at it still has this mesh's file name; otherwise the path is looked up again. A path that
// does not load, or loads as something other than the one-surface model PARTICLEDEFS checked, is
// reported once and not retried until the mesh list changes.
bool MeshParticleBuffer::EnsureModel(MeshSlot &slot)
{
	const ParticleMeshDefinition &mesh = slot.Mesh;
	FModel *model = nullptr;
	if (slot.ModelIndex >= 0 && (unsigned)slot.ModelIndex < Models.Size() && Models[slot.ModelIndex] != nullptr &&
		Models[slot.ModelIndex]->mFileName.CompareNoCase(mesh.Path) == 0)
	{
		model = Models[slot.ModelIndex];
	}
	else
	{
		if (slot.ModelFailed)
			return false;

		slot.ModelIndex = -1;
		const unsigned index = FindModel(nullptr, mesh.Path.GetChars(), true);
		if (index >= Models.Size() || Models[index] == nullptr)
		{
			slot.ModelFailed = true;
			Printf(TEXTCOLOR_ORANGE "MeshParticles: \"%s\" did not load as a model -- definition slot %d draws as its billboard\n",
				mesh.Path.GetChars(), mesh.Slot);
			return false;
		}
		model = Models[index];
		if (model->GetSurfaceCount() != 1 || model->NumFrames() <= mesh.Frame)
		{
			slot.ModelFailed = true;
			Printf(TEXTCOLOR_ORANGE "MeshParticles: \"%s\" loaded with %d surfaces and %d frames, not the one-surface mesh with frame %d "
				"PARTICLEDEFS checked -- definition slot %d draws as its billboard\n",
				mesh.Path.GetChars(), model->GetSurfaceCount(), model->NumFrames(), mesh.Frame, mesh.Slot);
			return false;
		}
		slot.ModelIndex = (int)index;
	}

	if (model->GetVertexBuffer(GLModelRendererType) == nullptr)
	{
		MeshParticleModelBuilder builder;
		model->BuildVertexBuffer(&builder);
	}
	const auto vbuf = static_cast<FModelVertexBuffer *>(model->GetVertexBuffer(GLModelRendererType));
	return vbuf != nullptr && vbuf->vertexBuffer() != nullptr && vbuf->indexBuffer() != nullptr;
}

//==========================================================================
//
// The live slot lists
//
//==========================================================================

void MeshParticleBuffer::Sync(const void *records, unsigned recordCount, uint64_t serial, uint64_t written, float levelTime)
{
	const uint8_t *bytes = (const uint8_t *)records;
	if (mDefinedSlots.Size() == 0 || bytes == nullptr || recordCount == 0)
	{
		// Nothing names a mesh (or there is no ring yet): nothing to follow. The ring position is
		// kept, and a mesh list arriving later asks for a full scan anyway.
		for (unsigned s : mDefinedSlots)
		{
			if (mSlots[s].Live.Size() > 0)
			{
				mSlots[s].Live.Clear();
				mListDirty = true;
			}
		}
		mSyncedSerial = serial;
		mSyncedWritten = written;
		if (mListDirty || mUploadedInstances != 0)
			Upload();
		return;
	}

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	// Record indices the draw can use: inside the CPU ring, the GPU ring (binding 5) and this list.
	// The two rings are sized from the same latched capacity, so all three agree in practice.
	unsigned usable = recordCount;
	if (screen->mGpuParticles != nullptr && screen->mGpuParticles->GetRingSize() < usable)
		usable = screen->mGpuParticles->GetRingSize();
	if (mCapacity < usable)
		usable = mCapacity;

	// THE SYNC RULE, the ring's (GpuParticleBuffer::Sync): a new serial, the cursor going back, or a
	// whole ring or more written since the last call means every record is looked at again.
	const bool full = mNeedFullScan || serial != mSyncedSerial || written < mSyncedWritten || written - mSyncedWritten >= recordCount;
	if (full)
	{
		for (unsigned s : mDefinedSlots)
			mSlots[s].Live.Clear();
		mLitUntil = NEVER_ALIVE;
		mListDirty = true;
		const unsigned count = written >= recordCount ? recordCount : (unsigned)written;
		NoteRecords(bytes, 0, count, usable, levelTime);
		mNeedFullScan = false;
	}
	else if (written != mSyncedWritten)
	{
		const unsigned first = (unsigned)(mSyncedWritten % recordCount);
		const unsigned last = (unsigned)(written % recordCount);
		if (first < last)
		{
			NoteRecords(bytes, first, last - first, usable, levelTime);
		}
		else
		{
			NoteRecords(bytes, first, recordCount - first, usable, levelTime);
			NoteRecords(bytes, 0, last, usable, levelTime);
		}
	}
	mSyncedSerial = serial;
	mSyncedWritten = written;

	// COMPACTION. An entry stays while the record at its slot is still the one noted (same birth,
	// same definition) and alive at this frame's level time -- the same test meshparticles.vp makes,
	// so nothing is drawn that the shader would collapse, bar frames already in flight.
	for (unsigned s : mDefinedSlots)
	{
		TArray<LiveInstance> &live = mSlots[s].Live;
		unsigned kept = 0;
		for (unsigned i = 0; i < live.Size(); i++)
		{
			const LiveInstance instance = live[i];
			const uint8_t *record = bytes + (size_t)instance.RingSlot * GpuParticleBuffer::RECORD_BYTES;
			const float birth = RecordFloat(record, GpuParticleBuffer::RECORD_BIRTH_OFFSET);
			const float life = RecordFloat(record, GpuParticleBuffer::RECORD_LIFE_OFFSET);
			const float definition = RecordFloat(record, GpuParticleBuffer::RECORD_DEFINITION_OFFSET);
			if (birth != instance.Birth || !(life > 0.f) || levelTime - birth > life || !(definition > (float)s - 0.5f && definition < (float)s + 0.5f))
				continue;
			live[kept++] = instance;
		}
		if (kept != live.Size())
		{
			live.Resize(kept);
			mListDirty = true;
		}
	}

	if (mListDirty)
		Upload();

	if (timed)
		PerfLog::AddCpuSample("fx.meshparticles", (I_nsTime() - startNs) / 1.0e6);
}

void MeshParticleBuffer::NoteRecords(const uint8_t *records, unsigned first, unsigned count, unsigned usable, float levelTime)
{
	const unsigned end = first + count;
	for (unsigned i = first; i < end && i < usable; i++)
	{
		const uint8_t *record = records + (size_t)i * GpuParticleBuffer::RECORD_BYTES;

		// Only named definitions have meshes; an inline one (256 and up) never does.
		const float definition = RecordFloat(record, GpuParticleBuffer::RECORD_DEFINITION_OFFSET);
		if (!(definition > -0.5f && definition < (float)DEFINITION_SLOTS - 0.5f))
			continue;
		const unsigned s = (unsigned)(definition + 0.5f);
		MeshSlot &slot = mSlots[s];
		if (!slot.Defined)
			continue;

		// A legacy record (r_gpuparticles_legacy) keeps a size in d.x, not a definition.
		if (RecordFloat(record, GpuParticleBuffer::RECORD_PLANE_OFFSET) < -8.f)
			continue;

		const float birth = RecordFloat(record, GpuParticleBuffer::RECORD_BIRTH_OFFSET);
		const float life = RecordFloat(record, GpuParticleBuffer::RECORD_LIFE_OFFSET);
		if (!(life > 0.f) || levelTime - birth > life)
			continue;

		const LiveInstance instance = { i, birth };
		slot.Live.Push(instance);
		mListDirty = true;
		if (slot.Lit && birth + life > mLitUntil)
			mLitUntil = birth + life;
	}
}

// The grouped list, each mesh definition's live slots in turn, then the header. Written only when
// something changed; the draw ranges (FirstInstance, InstanceCount) are this upload's.
void MeshParticleBuffer::Upload()
{
	if (mBuffer == nullptr)
	{
		mUploadedInstances = 0;
		mListDirty = false;
		return;
	}

	mBuffer->Map();
	uint8_t *dst = (uint8_t *)mBuffer->Memory();
	if (dst == nullptr)
	{
		mBuffer->Unmap();
		return;	// try again next scene
	}

	unsigned total = 0;
	bool clipped = false;
	for (unsigned s : mDefinedSlots)
	{
		MeshSlot &slot = mSlots[s];
		unsigned count = slot.Live.Size();
		if (count > mCapacity - total)
		{
			// Unreachable: every entry is a different ring slot and the list holds the whole ring.
			count = mCapacity - total;
			clipped = true;
		}
		slot.FirstInstance = total;
		slot.InstanceCount = count;
		uint8_t *out = dst + INSTANCES_OFFSET + (size_t)total * INSTANCE_BYTES;
		for (unsigned i = 0; i < count; i++)
		{
			const uint32_t ringSlot = slot.Live[i].RingSlot;
			memcpy(out + (size_t)i * INSTANCE_BYTES, &ringSlot, INSTANCE_BYTES);
		}
		total += count;
	}

	const float header[4] = { (float)total, (float)mCapacity, 0.f, 0.f };
	memcpy(dst, header, sizeof(header));
	mBuffer->Unmap();

	if (clipped && !mWarnedCapacity)
	{
		mWarnedCapacity = true;
		Printf(TEXTCOLOR_ORANGE "MeshParticles: more live mesh particles than the %u-instance list holds; the rest are not drawn\n", mCapacity);
	}

	mUploadedInstances = total;
	mListDirty = false;
}

//==========================================================================
//
// The draw
//
//==========================================================================

bool MeshParticleBuffer::IsDrawable() const
{
	return r_meshparticles && ShaderReady && !ShaderFailed && mBuffer != nullptr && mDefinitionsWritten && mUploadedInstances > 0 &&
		screen->mGpuParticles != nullptr && screen->mViewLights != nullptr;
}

void MeshParticleBuffer::Draw(FRenderState &state)
{
	if (!IsDrawable())
		return;

	// Opaque and writing depth, like any model. No culling: shards and splinters are thin, and an
	// open mesh must not show a hole; the depth test hides back faces behind front ones.
	state.SetEffect(EFF_MESHPARTICLES);
	state.SetRenderStyle(STYLE_Source);
	state.SetDepthMask(true);
	state.SetDepthFunc(DF_Less);
	state.SetCulling(Cull_None);
	state.EnableTexture(true);
	state.EnableModelMatrix(false);
	state.SetBoneIndexBase(-1);
	state.SetLightIndex(-1);

	for (unsigned s : mDefinedSlots)
	{
		const MeshSlot &slot = mSlots[s];
		// Only a definition whose billboard stepped aside this frame, so a particle is never drawn
		// both ways; mBillboardHidden also says its model was ready.
		if (slot.InstanceCount == 0 || !mBillboardHidden[s])
			continue;

		FModel *model = Models[slot.ModelIndex];
		const auto vbuf = static_cast<FModelVertexBuffer *>(model->GetVertexBuffer(GLModelRendererType));
		FGameTexture *skin = TexMan.GetGameTexture(slot.Mesh.Skin, true);
		if (vbuf == nullptr || skin == nullptr)
			continue;

		// One surface: its vertices start at 0 and its indices at 0 (FMD3Model::BuildVertexBuffer), and
		// frame f's vertices at f x vertices. Both vertex streams on the drawn frame: nothing blends.
		state.SetMaterial(skin, UF_Skin, 0, CLAMP_NONE, 0, -1);
		const int frameStart = slot.Mesh.Frame * (int)slot.Mesh.Vertices;
		state.SetVertexBuffer(vbuf->vertexBuffer(), frameStart, frameStart);
		state.SetIndexBuffer(vbuf->indexBuffer());
		state.DrawIndexedInstanced(DT_Triangles, 0, (int)slot.Mesh.Triangles * 3, (int)slot.InstanceCount, (int)slot.FirstInstance);
	}

	state.SetEffect(EFF_NONE);
	state.SetVertexBuffer(screen->mVertexData);
}

FString MeshParticleBuffer::DescribeSlot(int slotIndex) const
{
	FString text;
	if (slotIndex < 0 || slotIndex >= (int)DEFINITION_SLOTS || !mSlots[slotIndex].Defined)
	{
		text = "the renderer has no mesh for it (the next frame picks the list up)";
		return text;
	}
	const MeshSlot &slot = mSlots[slotIndex];
	const char *how = mBillboardHidden[slotIndex] ? "drawn as a mesh" :
		!r_meshparticles ? "drawn as its billboard (r_meshparticles 0)" :
		!ShaderReady || ShaderFailed ? "drawn as its billboard (the mesh effect did not compile)" :
		slot.ModelFailed ? "drawn as its billboard (the model did not load)" :
		!slot.ModelReady ? "drawn as its billboard (the model is not ready)" : "drawn as its billboard";
	text.Format("%s -- model #%d, %u live this frame", how, slot.ModelIndex, slot.InstanceCount);
	return text;
}
