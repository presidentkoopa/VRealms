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

void ParticleDefinitionBuffer::Sync(const void *definitions, const uint64_t *slotGenerations, unsigned slotCount, uint64_t generation)
{
	if (mBuffer == nullptr || definitions == nullptr || slotGenerations == nullptr) return;
	if (generation == mSyncedGeneration) return;

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
		if (slotGenerations[i] <= mSyncedGeneration)
		{
			i++;
			continue;
		}
		const unsigned first = i;
		while (i < slotCount && slotGenerations[i] > mSyncedGeneration) i++;
		memcpy(dst + (size_t)first * RECORD_BYTES, src + (size_t)first * RECORD_BYTES, (size_t)(i - first) * RECORD_BYTES);
		mUploadedSlots += i - first;
	}
	mBuffer->Unmap();

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
