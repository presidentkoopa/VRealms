/*
** hw_effectlightbuffer.cpp
**
** [EFFECTLIGHTS] The effect light records and bins on the GPU. See the header.
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

#include <cstddef>
#include <cstring>
#include "hw_effectlightbuffer.h"
#include "hwrenderer/data/buffers.h"
#include "v_video.h"
#include "printf.h"

static_assert(sizeof(EffectLightRecord) == EffectLightBuffer::RECORD_BYTES &&
	offsetof(EffectLightRecord, b) == 16 && offsetof(EffectLightRecord, color) == 32 && offsetof(EffectLightRecord, extra) == 48,
	"EffectLightRecord must be four vec4s with no padding -- see EffectLightRecord in main.fp");
static_assert(sizeof(EffectLightRecordHeader) == EffectLightBuffer::RECORD_HEADER_BYTES,
	"EffectLightRecordHeader must be four uints ahead of the records -- see EffectLightSSO in main.fp");
static_assert(sizeof(EffectLightGridHeader) == EffectLightBuffer::GRID_HEADER_BYTES && offsetof(EffectLightGridHeader, Size) == 16,
	"EffectLightGridHeader must be a vec4 and an ivec4 -- see EffectLightBinSSO in main.fp");
static_assert(EFFECT_LIGHT_BIN_LOOP_MAX <= (int)EFFECT_LIGHT_BIN_COUNT_MASK && (EFFECT_LIGHT_BIN_COUNT_MASK + 1) == (1u << EFFECT_LIGHT_BIN_COUNT_BITS),
	"a bin word's count must hold EFFECT_LIGHT_BIN_LOOP_MAX");
static_assert((uint64_t)EFFECT_LIGHT_INDEX_CAPACITY <= (uint64_t)1 << (32 - EFFECT_LIGHT_BIN_COUNT_BITS),
	"a bin word's offset must reach the end of the index list");

static EffectLightBuffer *EffectLightBufferInstance = nullptr;

EffectLightFrameStats &EffectLightStats()
{
	static EffectLightFrameStats stats;
	return stats;
}

EffectLightBuffer *EffectLightBuffer::Instance()
{
	return EffectLightBufferInstance;
}

void EffectLightBuffer::Create()
{
	delete EffectLightBufferInstance;
	EffectLightBufferInstance = new EffectLightBuffer();
}

void EffectLightBuffer::Destroy()
{
	delete EffectLightBufferInstance;
	EffectLightBufferInstance = nullptr;
}

static const size_t EffectLightRecordBytes = (size_t)EffectLightBuffer::RECORD_HEADER_BYTES + (size_t)EFFECT_LIGHT_RECORD_CAPACITY * EffectLightBuffer::RECORD_BYTES;
static const size_t EffectLightBinBytes = (size_t)EffectLightBuffer::GRID_HEADER_BYTES + ((size_t)EFFECT_LIGHT_BIN_CAPACITY + (size_t)EFFECT_LIGHT_INDEX_CAPACITY) * sizeof(uint32_t);

EffectLightBuffer::EffectLightBuffer()
{
	// Storage buffers, persistently mapped, set 1 bindings 14 and 15 on Vulkan (vk_descriptorset.cpp). They have no binding
	// point of their own: the buffer manager keys nothing on them, and the descriptor set asks Instance() for them.
	mRecords = screen->CreateDataBuffer(-1, true, false);
	mRecords->SetData(EffectLightRecordBytes, nullptr, BufferUsageType::Persistent);
	mBins = screen->CreateDataBuffer(-1, true, false);
	mBins->SetData(EffectLightBinBytes, nullptr, BufferUsageType::Persistent);

	// A persistent allocation is not guaranteed to be zeroed. Zero is no record and a grid of size 0: what every shader must
	// see until the first light.
	bool zeroed = true;
	for (IDataBuffer *buffer : { mRecords, mBins })
	{
		buffer->Map();
		if (buffer->Memory() != nullptr)
			memset(buffer->Memory(), 0, buffer == mRecords ? EffectLightRecordBytes : EffectLightBinBytes);
		else
			zeroed = false;
		buffer->Unmap();
	}
	mEmptyWritten = zeroed;

	Printf("EffectLights: buffers created -- %d records x %u B + %u B; %d bins + %d indices x 4 B + %u B = %.2f MB\n",
		EFFECT_LIGHT_RECORD_CAPACITY, RECORD_BYTES, RECORD_HEADER_BYTES, EFFECT_LIGHT_BIN_CAPACITY, EFFECT_LIGHT_INDEX_CAPACITY,
		GRID_HEADER_BYTES, (double)(EffectLightRecordBytes + EffectLightBinBytes) / (1024.0 * 1024.0));
}

EffectLightBuffer::~EffectLightBuffer()
{
	delete mRecords;
	delete mBins;
}

void EffectLightBuffer::Upload(const EffectLightRecord *records, unsigned count, unsigned binned, const EffectLightGridHeader &grid,
	const uint32_t *bins, unsigned binCount, const uint32_t *indices, unsigned indexCount)
{
	if (mRecords == nullptr || mBins == nullptr)
	{
		mLiveCount = 0;
		return;
	}

	const uint64_t gridBins = (grid.Size[0] > 0 && grid.Size[1] > 0 && grid.Size[2] > 0) ?
		(uint64_t)grid.Size[0] * (uint64_t)grid.Size[1] * (uint64_t)grid.Size[2] : 0;
	// Anything inconsistent is uploaded as an empty frame: a light too few, never a read past the data.
	bool empty = records == nullptr || count == 0 || bins == nullptr || binCount == 0 || gridBins != binCount ||
		count > (unsigned)EFFECT_LIGHT_RECORD_CAPACITY || binCount > (unsigned)EFFECT_LIGHT_BIN_CAPACITY ||
		indexCount > (unsigned)EFFECT_LIGHT_INDEX_CAPACITY || (indexCount > 0 && indices == nullptr);

	// Nothing binned last time and nothing now: the buffers already say so.
	if (empty && mLiveCount == 0 && mEmptyWritten)
		return;

	mRecords->Map();
	mBins->Map();
	uint8_t *recordDst = (uint8_t *)mRecords->Memory();
	uint8_t *binDst = (uint8_t *)mBins->Memory();
	if (recordDst == nullptr || binDst == nullptr)
	{
		mRecords->Unmap();
		mBins->Unmap();
		mEmptyWritten = false;	// try again next frame
		return;
	}

	mSerial++;
	if (empty)
	{
		const EffectLightGridHeader none = {};
		memcpy(binDst, &none, GRID_HEADER_BYTES);
		const EffectLightRecordHeader header = { 0, 0, 0, mSerial };
		memcpy(recordDst, &header, RECORD_HEADER_BYTES);
		mLiveCount = 0;
		mEmptyWritten = true;
	}
	else
	{
		// The frame before has finished on the GPU (the frame's end waits for its commands), so the order here only keeps a
		// header from ever naming data not yet written.
		memcpy(recordDst + RECORD_HEADER_BYTES, records, (size_t)count * RECORD_BYTES);
		const EffectLightRecordHeader header = { count, binned <= count ? binned : count, binned <= count ? count - binned : 0, mSerial };
		memcpy(recordDst, &header, RECORD_HEADER_BYTES);

		memcpy(binDst + GRID_HEADER_BYTES, bins, (size_t)binCount * sizeof(uint32_t));
		if (indexCount > 0)
			memcpy(binDst + GRID_HEADER_BYTES + (size_t)binCount * sizeof(uint32_t), indices, (size_t)indexCount * sizeof(uint32_t));
		EffectLightGridHeader header2 = grid;
		header2.Size[3] = (int32_t)binCount;
		memcpy(binDst, &header2, GRID_HEADER_BYTES);

		mLiveCount = count;
		mEmptyWritten = false;
	}

	mRecords->Unmap();
	mBins->Unmap();
}
