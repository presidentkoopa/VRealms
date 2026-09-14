/*
** hw_viewlightbuffer.cpp
**
** [VIEWLIGHTS] The dynamic lights in view this scene. See the header.
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
#include "hw_viewlightbuffer.h"
#include "hwrenderer/data/buffers.h"
#include "shaderuniforms.h"
#include "v_video.h"
#include "printf.h"

static_assert(sizeof(ViewLightRecord) == ViewLightBuffer::RECORD_BYTES,
	"ViewLightRecord must be four vec4s with no padding -- see the ViewLight struct in gpuparticles.vp");

ViewLightBuffer::ViewLightBuffer()
{
	const size_t bytes = (size_t)HEADER_BYTES + (size_t)CAPACITY * RECORD_BYTES;

	// Storage buffer, persistently mapped, set 1 binding 8 on Vulkan
	// (vk_descriptorset.cpp). Same creation path as the particle ring.
	mBuffer = screen->CreateDataBuffer(VIEWLIGHT_BINDINGPOINT, true, false);
	mBuffer->SetData(bytes, nullptr, BufferUsageType::Persistent);

	// A persistent allocation is not guaranteed to be zeroed. Zero is a count of 0
	// and no lights, which is what every shader must see until the first fill.
	mBuffer->Map();
	if (mBuffer->Memory() != nullptr)
	{
		memset(mBuffer->Memory(), 0, bytes);
		mCountWritten = true;
	}
	mBuffer->Unmap();

	Printf("ViewLights: buffer created -- %u B header + %u lights x %u B = %llu bytes\n",
		HEADER_BYTES, CAPACITY, RECORD_BYTES, (unsigned long long)bytes);
}

ViewLightBuffer::~ViewLightBuffer()
{
	delete mBuffer;
}

void ViewLightBuffer::Upload(const ViewLightRecord *records, unsigned count)
{
	if (records == nullptr) count = 0;
	if (count > CAPACITY) count = CAPACITY;

	if (mBuffer == nullptr)
	{
		mLiveCount = 0;
		return;
	}

	// Nothing lit last time and nothing lit now: the buffer already says 0.
	if (count == 0 && mLiveCount == 0 && mCountWritten) return;

	mBuffer->Map();
	uint8_t *dst = (uint8_t *)mBuffer->Memory();
	if (dst == nullptr)
	{
		mBuffer->Unmap();
		mCountWritten = false;	// try again next scene
		return;
	}

	// Records first, then the count: a frame already in flight that reads the
	// buffer while this writes sees the old count over records that are either the
	// old or the new ones -- lights, never garbage. Presentation only either way.
	if (count > 0)
		memcpy(dst + HEADER_BYTES, records, (size_t)count * RECORD_BYTES);
	const float header[4] = { (float)count, 0.f, 0.f, 0.f };
	memcpy(dst, header, sizeof(header));
	mBuffer->Unmap();

	mLiveCount = count;
	mCountWritten = true;
}
