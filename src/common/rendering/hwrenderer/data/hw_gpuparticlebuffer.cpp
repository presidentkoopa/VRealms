/*
** hw_gpuparticlebuffer.cpp
**
** [GPUPARTICLES] The GPU half of the stateless particle ring. See the header.
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

#include <cstring>
#include <vector>
#include "hw_gpuparticlebuffer.h"
#include "hw_particledefbuffer.h"	// [2d] the LOOK_* bits
#include "shaderuniforms.h"
#include "v_video.h"
#include "hw_cvars.h"
#include "printf.h"
#include "i_time.h"
#include <algorithm>
#include "hw_perflog.h"				// [PARTICLELIGHTS] fx.effectlights
#include "hw_effectlightbuffer.h"	// [PARTICLELIGHTS] EffectLightBuffer::Instance: Vulkan's effect lights exist
#include "hw_effectlights.h"		// [PARTICLELIGHTS] EffectLights::Spawn
#include "hw_particlelights.h"		// [PARTICLELIGHTS] which records throw light, and their lights
#include "particledefs.h"			// [PARTICLELIGHTS] the definitions' light keys

// [PARTICLELIGHTS] r_particlelights_test -- a test, not a setting ("Engine docs/EFFECT_LIGHTS_LC_IMPL_NOTES.md"): named particle
// definitions without light keys that glow at birth (emissive above 0, alpha 0) throw a test light -- radius 48, and where they
// land a hold of up to a second -- so particle and debris lights can be judged on a mod's existing sparks and embers before any
// definition says `light`. Renderer-read; not archived, like r_debris_test. The debris pool reads it too (hw_debrispool.cpp).
CVARD(Bool, r_particlelights_test, false, CVAR_GLOBALCONFIG, "particle definitions that glow but have no light keys throw a test light (radius 48, up to a 1 second hold where they land) -- a test of particle lights (Vulkan only)")
EXTERN_CVAR(Bool, r_effectlights)	// [PARTICLELIGHTS] hw_effectlights.cpp

// [PARTICLELIGHTS] What the ring's particle lights keep from one sync to the next.
struct GpuParticleBuffer::LightState
{
	ParticleLights::SlotLight Slots[ParticleDefinitionBuffer::NAMED_SLOTS];
	uint64_t ListGeneration = 0;
	int TestSeen = -1;					// r_particlelights_test when Slots was resolved (-1: never)
	bool Any = false;					// some named definition throws light
	ParticleLights::BurstRun Run;		// the burst the last lit record was on
	uint64_t RunSerial = 0;
	uint64_t Spawned = 0;
	bool FirstLogged = false;
};
static_assert(sizeof(float) * ParticleLights::RECORD_FLOATS == GpuParticleBuffer::RECORD_BYTES, "a particle light reads a whole ring record");

GpuParticleBuffer::GpuParticleBuffer(unsigned ringSize) : mRingSize(ringSize != 0 ? ringSize : (unsigned)GpuParticleRingCapacity())
{
	const size_t recordBytes = (size_t)mRingSize * RECORD_BYTES;

	// The ring. Storage buffer, persistently mapped, set 1 binding 5 on Vulkan
	// (vk_descriptorset.cpp). Same creation path as the bone buffer.
	mBuffer = screen->CreateDataBuffer(GPUPARTICLE_BINDINGPOINT, true, false);
	mBuffer->SetData(recordBytes, nullptr, BufferUsageType::Persistent);

	// A persistent allocation is not guaranteed to be zeroed, and a garbage
	// record with a positive life would draw. The first sync of a level only
	// uploads what was written, so the GPU copy has to start empty.
	mBuffer->Map();
	if (mBuffer->Memory() != nullptr)
		memset(mBuffer->Memory(), 0, recordBytes);
	mBuffer->Unmap();

	// The static quad buffer: six vertices per ring slot. Each carries the
	// slot index split into two 16-bit halves plus the corner number, in the
	// EXISTING integer attribute at location 8 (VATTR_BONESELECTOR,
	// VFmt_UShort4_UInt) -- no new attribute enum, no new vertex format, so
	// none of the three backends' positional format tables change.
	struct QuadVertex
	{
		uint16_t indexLow;
		uint16_t indexHigh;
		uint16_t corner;
		uint16_t pad;
	};

	const size_t vertexCount = (size_t)mRingSize * VERTICES_PER_RECORD;
	std::vector<QuadVertex> verts(vertexCount);
	for (unsigned i = 0; i < mRingSize; i++)
	{
		for (unsigned c = 0; c < VERTICES_PER_RECORD; c++)
		{
			QuadVertex &v = verts[(size_t)i * VERTICES_PER_RECORD + c];
			v.indexLow = (uint16_t)(i & 0xffff);
			v.indexHigh = (uint16_t)((i >> 16) & 0xffff);
			v.corner = (uint16_t)c;
			v.pad = 0;
		}
	}

	mQuads = screen->CreateVertexBuffer();
	static const FVertexBufferAttribute format[] = {
		{ 0, VATTR_BONESELECTOR, VFmt_UShort4_UInt, 0 },
	};
	mQuads->SetFormat(1, 1, sizeof(QuadVertex), format);
	mQuads->SetData(vertexCount * sizeof(QuadVertex), verts.data(), BufferUsageType::Static);

	Printf("GpuParticles: buffer created -- ring %u records x %u B = %llu bytes, quads %llu vertices x %u B = %llu bytes\n",
		mRingSize, RECORD_BYTES, (unsigned long long)recordBytes,
		(unsigned long long)vertexCount, (unsigned)sizeof(QuadVertex),
		(unsigned long long)(vertexCount * sizeof(QuadVertex)));
}

GpuParticleBuffer::~GpuParticleBuffer()
{
	delete mQuads;
	delete mBuffer;
	delete mLights;	// [PARTICLELIGHTS]
}

void GpuParticleBuffer::Upload(const void *records, unsigned first, unsigned count)
{
	if (count == 0) return;
	memcpy((uint8_t *)mBuffer->Memory() + (size_t)first * RECORD_BYTES,
		(const uint8_t *)records + (size_t)first * RECORD_BYTES,
		(size_t)count * RECORD_BYTES);
	mUploadedSinceReport += count;
	NoteRecordLooks(records, first, count);	// [2d]
}

// [2d] For each record just uploaded: if its definition needs the premultiplied blend,
// the view lights or its own soft distance, push that need's "alive until" out to the
// record's death (birth + life). Free slots and legacy (stage 1) records need nothing.
// A few compares per NEW record; a full upload looks at the whole ring once.
void GpuParticleBuffer::NoteRecordLooks(const void *records, unsigned first, unsigned count)
{
	if (mSyncLooks == nullptr || mSyncLookCount == 0) return;

	const uint8_t *record = (const uint8_t *)records + (size_t)first * RECORD_BYTES;
	for (unsigned i = 0; i < count; i++, record += RECORD_BYTES)
	{
		float birth, life, definition, planeX;
		memcpy(&birth, record + RECORD_BIRTH_OFFSET, sizeof(float));
		memcpy(&life, record + RECORD_LIFE_OFFSET, sizeof(float));
		memcpy(&definition, record + RECORD_DEFINITION_OFFSET, sizeof(float));
		memcpy(&planeX, record + RECORD_PLANE_OFFSET, sizeof(float));
		if (!(life > 0.f) || planeX < -8.f || !(definition >= 0.f)) continue;

		// The slot as gpuparticles.vp rounds it.
		const unsigned slot = definition >= (float)mSyncLookCount ? mSyncLookCount : (unsigned)(definition + 0.5f);
		if (slot >= mSyncLookCount) continue;
		const uint8_t look = mSyncLooks[slot];
		if (look == 0) continue;

		const float until = birth + life;
		if ((look & ParticleDefinitionBuffer::LOOK_OCCLUDES) && until > mOccludersUntil) mOccludersUntil = until;
		if ((look & ParticleDefinitionBuffer::LOOK_LIT) && until > mLitUntil) mLitUntil = until;
		if ((look & ParticleDefinitionBuffer::LOOK_SOFT) && until > mSoftUntil) mSoftUntil = until;
	}
}

void GpuParticleBuffer::Sync(const void *records, unsigned recordCount, uint64_t serial, uint64_t written,
	const uint8_t *definitionLooks, unsigned definitionLookCount)
{
	// A level that never spawned a particle has no CPU ring. Its draw is
	// skipped (Written == 0), so whatever the GPU still holds is never read.
	if (mBuffer == nullptr || records == nullptr || recordCount == 0) return;
	if (serial == mSyncedSerial && written == mSyncedWritten) return;

	unsigned size = recordCount;
	if (size != mRingSize)
	{
		// Both sides size from GpuParticleRingCapacity(), which latches once
		// per process, so this should be unreachable. Clamp rather than write
		// past the end, and say so once.
		if (!mWarnedSizeMismatch)
		{
			Printf(TEXTCOLOR_ORANGE "GpuParticles: CPU ring %u != GPU ring %u, clamping\n", recordCount, mRingSize);
			mWarnedSizeMismatch = true;
		}
		if (size > mRingSize) size = mRingSize;
	}

	mBuffer->Map();
	if (mBuffer->Memory() == nullptr)
	{
		mBuffer->Unmap();
		return;
	}

	// [2d] Looked up by NoteRecordLooks for each uploaded record, this Sync only.
	mSyncLooks = definitionLooks;
	mSyncLookCount = definitionLooks != nullptr ? definitionLookCount : 0;

	const bool full = serial != mSyncedSerial || written < mSyncedWritten || written - mSyncedWritten >= size;
	if (full)
	{
		// [2d] Every record is looked at again, so start from nothing: a new level, a
		// cleared ring or a burst bigger than the ring leaves no stale "alive" behind.
		mOccludersUntil = NEVER_ALIVE;
		mLitUntil = NEVER_ALIVE;
		mSoftUntil = NEVER_ALIVE;
		Upload(records, 0, size);
		mFullSinceReport++;
	}
	else
	{
		const unsigned from = (unsigned)(mSyncedWritten % size);
		const unsigned to = (unsigned)(written % size);
		if (from < to)
		{
			Upload(records, from, to - from);
		}
		else
		{
			Upload(records, from, size - from);
			Upload(records, 0, to);
		}
		mSpanSinceReport++;
	}
	mBuffer->Unmap();
	mSyncLooks = nullptr;	// [2d]
	mSyncLookCount = 0;

	// One line per level, the first time that level's particles reach the GPU.
	if (written > 0 && serial != mLoggedSerial)
	{
		mLoggedSerial = serial;
		Printf("GpuParticles: level ring (serial %llu) reached the GPU -- %llu records written, %s upload, shader %s\n",
			(unsigned long long)serial, (unsigned long long)written, full ? "full" : "span",
			ShaderFailed ? "FAILED" : (ShaderReady ? "ready" : "not ready"));
	}

	// [PARTICLELIGHTS] The new records' effect lights, while the synced serial and cursor still say which records this sync
	// brought up.
	SpawnRecordLights(records, size, serial, written);

	mSyncedSerial = serial;
	mSyncedWritten = written;
}

// [PARTICLELIGHTS] See the header. Vulkan's effect lights only, while they are on: off, or on GL and GLES, nothing is made.
void GpuParticleBuffer::SpawnRecordLights(const void *records, unsigned size, uint64_t serial, uint64_t written)
{
	if (records == nullptr || size == 0 || !r_effectlights || EffectLightBuffer::Instance() == nullptr)
		return;
	if (mLights == nullptr)
		mLights = new LightState();
	LightState &state = *mLights;

	// The definitions' lights, again whenever the definitions load or the test is switched.
	const uint64_t generation = ParticleLightGeneration();
	const int test = r_particlelights_test ? 1 : 0;
	if (generation != state.ListGeneration || test != state.TestSeen)
	{
		state.ListGeneration = generation;
		state.TestSeen = test;
		state.Any = ParticleLights::ResolveSlots(ParticleLightDefinitionData(), ParticleLightDefinitionCount(), ParticleDefinitionTableData(),
			(int)ParticleDefinitionBuffer::NAMED_SLOTS, test != 0, state.Slots);
	}
	if (!state.Any)
		return;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	// The records written since the last sync, oldest first: at most the last `size` (older ones were overwritten unseen); after a
	// new serial, the level's own from its first.
	uint64_t from = written > (uint64_t)size ? written - (uint64_t)size : 0;
	if (serial == mSyncedSerial && written >= mSyncedWritten)
		from = std::max(from, mSyncedWritten);
	if (serial != state.RunSerial)
	{
		state.Run = ParticleLights::BurstRun();
		state.RunSerial = serial;
	}

	ParticleLights::Tuning tuning;
	tuning.SizeScale = (float)r_gpuparticles_sizescale;
	tuning.MaxSize = (float)r_gpuparticles_maxsize;
	tuning.Stretch = (float)r_gpuparticles_stretch;
	const unsigned namedSlots = ParticleDefinitionBuffer::NAMED_SLOTS;
	unsigned spawned = 0;
	for (uint64_t w = from; w < written; w++)
	{
		float record[ParticleLights::RECORD_FLOATS];
		memcpy(record, (const uint8_t *)records + (size_t)(w % size) * RECORD_BYTES, sizeof(record));
		// A live stage 2 record of a named definition that throws light, its slot as gpuparticles.vp rounds it.
		const float definition = record[ParticleLights::R_DEFINITION];
		if (!(record[ParticleLights::R_LIFE] > 0.f) || record[ParticleLights::R_PLANE] < -8.f || !(definition >= 0.f))
			continue;
		const unsigned slot = definition >= (float)namedSlots ? namedSlots : (unsigned)(definition + 0.5f);
		if (slot >= namedSlots || !state.Slots[slot].Active)
			continue;
		const ParticleLights::SlotLight &light = state.Slots[slot];
		state.Run.Take(record);
		if (state.Run.Lights >= light.Max)
			continue;
		const uint32_t hash = ParticleLights::RecordHash(record);
		if (!ParticleLights::CarriesLight(hash, light.Share))
			continue;
		state.Run.Lights++;
		EffectLights::Get().Spawn(ParticleLights::RingSource(light, record, hash, tuning));
		spawned++;
	}
	state.Spawned += spawned;
	if (spawned > 0 && !state.FirstLogged)
	{
		state.FirstLogged = true;
		Printf("GpuParticles: first particle lights this run -- %u from one sync (definitions with `light`; hw_particlelights.h)\n", spawned);
	}
	if (timed)
		PerfLog::AddCpuSample("fx.effectlights", (double)(I_nsTime() - startNs) / 1e6);
}

void GpuParticleBuffer::DebugReport(uint64_t written)
{
	if (!r_gpuparticles_debug) return;

	const uint64_t now = I_msTime();
	if (mLastReportMs != 0 && now - mLastReportMs < 2000) return;

	const uint64_t spawned = written >= mWrittenAtReport ? written - mWrittenAtReport : written;
	Printf("GpuParticles [debug]: written %llu (+%llu), uploaded %llu records (%u full, %u span), %u draws, shader %s, r_gpuparticles %d\n",
		(unsigned long long)written, (unsigned long long)spawned,
		(unsigned long long)mUploadedSinceReport, mFullSinceReport, mSpanSinceReport, mDrawsSinceReport,
		ShaderFailed ? "FAILED" : (ShaderReady ? "ready" : "not ready"), (int)*r_gpuparticles);

	mLastReportMs = now;
	mWrittenAtReport = written;
	mUploadedSinceReport = 0;
	mFullSinceReport = 0;
	mSpanSinceReport = 0;
	mDrawsSinceReport = 0;
}
