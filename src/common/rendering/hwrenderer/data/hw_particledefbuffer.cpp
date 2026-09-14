/*
** hw_particledefbuffer.cpp
**
** [PARTICLEDEFS] The GPU half of the particle definitions table. See the header.
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
#include "hw_particledefbuffer.h"
#include "hwrenderer/data/buffers.h"
#include "shaderuniforms.h"
#include "v_video.h"
#include "printf.h"
#include "c_cvars.h"	// [LOOKS] r_gpuparticles_looks

// [LOOKS] r_gpuparticles_looks -- the quality of generated particle looks ("Engine docs/
// GPU_PARTICLE_LOOKS_PLAN.md"): the `look` a PARTICLEDEFS definition names, drawn by
// gpuparticles.fp from noise instead of the round dot or a flipbook frame.
//   0  every look draws as the plain round dot -- an escape hatch, not the default
//   1  2 noise octaves, no slope lighting
//   2  3 octaves, with slope lighting (the default: effects our mods use are on by default)
//   3  4 octaves
// A definition's `detail` is its octaves at 2; 1 and 3 take one away or add one (1..4).
// Renderer-read: HWDrawInfo::StartScene copies it into mGpuParticleParams2.z every scene, so a
// menu slider changes particles already in the air while the menu is open. Only definitions
// with a look are affected, and GL/GLES never draw GPU particles.
//
// It lives here, beside the GPU half of the definitions the looks are stored in, rather than in
// hw_cvars.cpp: users read it with EXTERN_CVAR (hw_drawinfo.cpp) or FindCVar (the `particles`
// CCMD). Presentation only -- not SERVERINFO; nothing reads it back into the playsim.
CUSTOM_CVARD(Int, r_gpuparticles_looks, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "generated GPU particle looks: 0 plain dots, 1 low, 2 default, 3 high (Vulkan only)")
{
	if (self < 0) self = 0;
	else if (self > 3) self = 3;
}

// [2d] LOOK_* for one definition's 256 bytes (see ParticleDefinitionBuffer::GetSlotLooks).
// Occludes: any of its keys has alpha above 0 -- the shader holds alpha flat outside the
// keys and interpolates between them, so that is exactly "alpha above 0 at some point in
// its life". The key count is clamped as gpuparticles.vp clamps it.
static uint8_t DefinitionLook(const uint8_t *def)
{
	float keyCountValue, lit, soft;
	memcpy(&keyCountValue, def + ParticleDefinitionBuffer::KEY_COUNT_OFFSET, sizeof(float));
	memcpy(&lit, def + ParticleDefinitionBuffer::LOOK_OFFSET, sizeof(float));
	memcpy(&soft, def + ParticleDefinitionBuffer::LOOK_OFFSET + sizeof(float), sizeof(float));

	const unsigned maxKeys = (unsigned)ParticleDefinitionBuffer::KEYS;
	unsigned keyCount = 1;
	if (keyCountValue > 1.f)
		keyCount = keyCountValue >= (float)maxKeys ? maxKeys : (unsigned)(keyCountValue + 0.5f);

	bool occludes = false;
	for (unsigned k = 0; k < keyCount; k++)
	{
		float alpha;
		memcpy(&alpha, def + (size_t)k * ParticleDefinitionBuffer::KEY_STRIDE + ParticleDefinitionBuffer::KEY_ALPHA_OFFSET, sizeof(float));
		if (alpha > 0.f)
		{
			occludes = true;
			break;
		}
	}

	uint8_t look = 0;
	if (occludes) look |= ParticleDefinitionBuffer::LOOK_OCCLUDES;
	if (occludes && lit > 0.f) look |= ParticleDefinitionBuffer::LOOK_LIT;
	if (soft > 0.f) look |= ParticleDefinitionBuffer::LOOK_SOFT;
	return look;
}

ParticleDefinitionBuffer::ParticleDefinitionBuffer()
{
	const size_t bytes = (size_t)SLOTS * RECORD_BYTES;

	// Storage buffer, persistently mapped, set 1 binding 7 on Vulkan
	// (vk_descriptorset.cpp). Same creation path as the particle ring.
	mBuffer = screen->CreateDataBuffer(PARTICLEDEF_BINDINGPOINT, true, false);
	mBuffer->SetData(bytes, nullptr, BufferUsageType::Persistent);

	// A persistent allocation is not guaranteed to be zeroed. Sync only copies
	// slots that were written, so the GPU copy has to start empty.
	mBuffer->Map();
	if (mBuffer->Memory() != nullptr)
		memset(mBuffer->Memory(), 0, bytes);
	mBuffer->Unmap();

	Printf("ParticleDefinitions: buffer created -- %u slots (%u named + %u inline) x %u B = %llu bytes\n",
		SLOTS, NAMED_SLOTS, INLINE_SLOTS, RECORD_BYTES, (unsigned long long)bytes);
}

ParticleDefinitionBuffer::~ParticleDefinitionBuffer()
{
	delete mBuffer;
}

void ParticleDefinitionBuffer::Sync(const void *definitions, const uint64_t *slotGenerations, unsigned slotCount, uint64_t generation,
	const uint8_t *billboardHidden, unsigned hiddenCount)
{
	if (mBuffer == nullptr || definitions == nullptr || slotGenerations == nullptr) return;

	if (slotCount > SLOTS)
	{
		// Both sides size from SLOTS, so this should be unreachable. Clamp rather
		// than write past the end, and say so once.
		if (!mWarnedCount)
		{
			Printf(TEXTCOLOR_ORANGE "ParticleDefinitions: CPU table has %u slots, GPU buffer %u, clamping\n", slotCount, SLOTS);
			mWarnedCount = true;
		}
		slotCount = SLOTS;
	}

	// [MESHPARTICLES] Which slots draw as meshes this frame. With no list every mark is 0 and so is
	// every mark already uploaded, so nothing below differs from the sync before mesh particles.
	uint8_t hidden[SLOTS];
	memset(hidden, 0, sizeof(hidden));
	if (billboardHidden != nullptr)
	{
		const unsigned markCount = hiddenCount < slotCount ? hiddenCount : slotCount;
		for (unsigned s = 0; s < markCount; s++)
			hidden[s] = billboardHidden[s] != 0 ? 1 : 0;
	}
	const bool hiddenChanged = memcmp(hidden, mBillboardHidden, sizeof(hidden)) != 0;

	if (generation == mSyncedGeneration && !hiddenChanged) return;

	mBuffer->Map();
	if (mBuffer->Memory() == nullptr)
	{
		mBuffer->Unmap();
		return;
	}

	const uint8_t *src = (const uint8_t *)definitions;
	uint8_t *dst = (uint8_t *)mBuffer->Memory();
	unsigned i = 0;
	while (i < slotCount)
	{
		if (slotGenerations[i] <= mSyncedGeneration && hidden[i] == mBillboardHidden[i])
		{
			i++;
			continue;
		}
		const unsigned first = i;
		while (i < slotCount && (slotGenerations[i] > mSyncedGeneration || hidden[i] != mBillboardHidden[i])) i++;
		memcpy(dst + (size_t)first * RECORD_BYTES, src + (size_t)first * RECORD_BYTES, (size_t)(i - first) * RECORD_BYTES);
		mUploadedSlots += i - first;
		for (unsigned s = first; s < i; s++)
		{
			if (hidden[s])
			{
				// [MESHPARTICLES] Drawn as a mesh: an empty billboard (see the header).
				memset(dst + (size_t)s * RECORD_BYTES, 0, RECORD_BYTES);
				mSlotLooks[s] = 0;
			}
			else
			{
				mSlotLooks[s] = DefinitionLook(src + (size_t)s * RECORD_BYTES);	// [2d]
			}
		}
	}
	mBuffer->Unmap();

	memcpy(mBillboardHidden, hidden, sizeof(hidden));
	mSyncedGeneration = generation;
}

void ParticleDefinitionBuffer::SyncAtlasLayers(const ParticleAtlasLayer *layers, unsigned count, uint64_t generation)
{
	// [2c] Cheap when nothing changed, which is every frame after the first: the CPU
	// list only changes when PARTICLEDEFS lumps are (re)loaded.
	if (generation == mAtlasGeneration) return;

	if (layers == nullptr) count = 0;
	if (count > ATLAS_LAYERS)
	{
		// The CPU table refuses definitions past the cap, so this should be
		// unreachable. Clamp rather than build past the budget, and say so once.
		if (!mWarnedAtlasCount)
		{
			Printf(TEXTCOLOR_ORANGE "ParticleAtlas: CPU list has %u layers, the atlas holds %u, clamping\n", count, ATLAS_LAYERS);
			mWarnedAtlasCount = true;
		}
		count = ATLAS_LAYERS;
	}

	mAtlasLayers.Resize(count);
	for (unsigned i = 0; i < count; i++)
		mAtlasLayers[i] = layers[i];
	mAtlasGeneration = generation;
}
