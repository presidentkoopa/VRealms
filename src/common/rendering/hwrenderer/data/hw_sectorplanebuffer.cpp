/*
** hw_sectorplanebuffer.cpp
**
** [SECTORPLANES] Sectors' current floor and ceiling plane equations on the GPU.
** See the header.
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
#include "hw_sectorplanebuffer.h"
#include "hwrenderer/data/buffers.h"
#include "shaderuniforms.h"
#include "v_video.h"
#include "printf.h"

static_assert(sizeof(SectorPlaneRecord) == SectorPlaneBuffer::RECORD_BYTES,
	"SectorPlaneRecord must be two vec4s with no padding -- see the SectorPlane struct in hw_sectorplanebuffer.h");

SectorPlaneBuffer::SectorPlaneBuffer()
{
	const size_t bytes = (size_t)HEADER_BYTES + (size_t)CAPACITY * RECORD_BYTES;

	// Storage buffer, persistently mapped, set 1 binding 12 on Vulkan
	// (vk_descriptorset.cpp). Same creation path as the view light list.
	mBuffer = screen->CreateDataBuffer(SECTORPLANE_BINDINGPOINT, true, false);
	mBuffer->SetData(bytes, nullptr, BufferUsageType::Persistent);

	// A persistent allocation is not guaranteed to be zeroed. Zero is a count of 0 and
	// every record unknown, which is what every reader must see until the first poll.
	mBuffer->Map();
	if (mBuffer->Memory() != nullptr)
	{
		memset(mBuffer->Memory(), 0, bytes);
		mCountWritten = true;
	}
	mBuffer->Unmap();

	Printf("SectorPlanes: buffer created -- %u B header + %u sectors x %u B = %llu bytes\n",
		HEADER_BYTES, CAPACITY, RECORD_BYTES, (unsigned long long)bytes);
}

SectorPlaneBuffer::~SectorPlaneBuffer()
{
	delete mBuffer;
}

void SectorPlaneBuffer::SetSectorCount(unsigned count)
{
	if (count > CAPACITY) count = CAPACITY;
	if (mBuffer == nullptr) return;
	if (count == mCount && mCountWritten) return;

	mBuffer->Map();
	uint8_t *dst = (uint8_t *)mBuffer->Memory();
	if (dst == nullptr)
	{
		mBuffer->Unmap();
		mCountWritten = false;	// try again next time
		return;
	}
	const float header[4] = { (float)count, (float)CAPACITY, 0.f, 0.f };
	memcpy(dst, header, sizeof(header));
	mBuffer->Unmap();

	mCount = count;
	mCountWritten = true;
}

void SectorPlaneBuffer::BeginLevel(unsigned count)
{
	if (count > CAPACITY) count = CAPACITY;
	if (mBuffer == nullptr) return;

	const unsigned zeroed = mCount > count ? mCount : count;
	mBuffer->Map();
	uint8_t *dst = (uint8_t *)mBuffer->Memory();
	if (dst != nullptr)
		memset(dst + HEADER_BYTES, 0, (size_t)zeroed * RECORD_BYTES);
	mBuffer->Unmap();

	mCountWritten = false;	// the count is written below even when it did not change
	SetSectorCount(count);
}

bool SectorPlaneBuffer::Write(unsigned index, const SectorPlaneRecord &record)
{
	if (mBuffer == nullptr || index >= CAPACITY) return false;

	// Persistent on Vulkan, so Map and Unmap cost nothing; kept for the IDataBuffer
	// contract. A frame already in flight that reads this record while it is written
	// sees the old planes or the new ones -- presentation either way.
	mBuffer->Map();
	uint8_t *dst = (uint8_t *)mBuffer->Memory();
	if (dst == nullptr)
	{
		mBuffer->Unmap();
		return false;
	}
	memcpy(dst + HEADER_BYTES + (size_t)index * RECORD_BYTES, &record, RECORD_BYTES);
	mBuffer->Unmap();
	return true;
}
