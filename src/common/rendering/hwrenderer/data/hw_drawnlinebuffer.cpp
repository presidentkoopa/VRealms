/*
** hw_drawnlinebuffer.cpp
**
** [DRAWNLINES] The GPU half of drawn glowing lines. See the header.
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
#include "hw_drawnlinebuffer.h"
#include "shaderuniforms.h"
#include "v_video.h"
#include "hw_cvars.h"
#include "printf.h"

static_assert(sizeof(DrawnLineRecord) == DrawnLineBuffer::RECORD_BYTES,
	"DrawnLineRecord must be seven vec4s (112 bytes) with no padding -- see the DrawnLine struct in vk_shader.cpp");

DrawnLineBuffer::DrawnLineBuffer(unsigned records)
	: mCapacity(records != 0 ? records : (unsigned)DrawnLineCapacity() + ROUTED_BEAM_RESERVE)
{
	const size_t recordBytes = (size_t)mCapacity * RECORD_BYTES;

	// The records. Storage buffer, persistently mapped, set 1 binding 6 on Vulkan
	// (vk_descriptorset.cpp). Same creation path as the particle ring.
	mBuffer = screen->CreateDataBuffer(DRAWNLINE_BINDINGPOINT, true, false);
	mBuffer->SetData(recordBytes, nullptr, BufferUsageType::Persistent);

	// Only the first GetLiveCount() records are ever drawn, so stale memory is
	// never read -- zeroed anyway, so a debugger shows nothing misleading.
	mBuffer->Map();
	if (mBuffer->Memory() != nullptr)
		memset(mBuffer->Memory(), 0, recordBytes);
	mBuffer->Unmap();

	// The static box buffer: 36 vertices per record. Each carries the record
	// index in two 16-bit halves plus the corner number, in the EXISTING integer
	// attribute at location 8 (VATTR_BONESELECTOR, VFmt_UShort4_UInt) -- the
	// particles' arrangement, so no backend's vertex format table changes.
	struct BoxVertex
	{
		uint16_t indexLow;
		uint16_t indexHigh;
		uint16_t corner;
		uint16_t pad;
	};

	const size_t vertexCount = (size_t)mCapacity * VERTICES_PER_RECORD;
	std::vector<BoxVertex> verts(vertexCount);
	for (unsigned i = 0; i < mCapacity; i++)
	{
		for (unsigned c = 0; c < VERTICES_PER_RECORD; c++)
		{
			BoxVertex &v = verts[(size_t)i * VERTICES_PER_RECORD + c];
			v.indexLow = (uint16_t)(i & 0xffff);
			v.indexHigh = (uint16_t)((i >> 16) & 0xffff);
			v.corner = (uint16_t)c;
			v.pad = 0;
		}
	}

	mBoxes = screen->CreateVertexBuffer();
	static const FVertexBufferAttribute format[] = {
		{ 0, VATTR_BONESELECTOR, VFmt_UShort4_UInt, 0 },
	};
	mBoxes->SetFormat(1, 1, sizeof(BoxVertex), format);
	mBoxes->SetData(vertexCount * sizeof(BoxVertex), verts.data(), BufferUsageType::Static);

	Printf("DrawnLines: buffer created -- %u records (capacity %d + %u for routed beams) x %u B = %llu bytes, boxes %llu vertices x %u B = %llu bytes\n",
		mCapacity, DrawnLineCapacity(), ROUTED_BEAM_RESERVE, RECORD_BYTES, (unsigned long long)recordBytes,
		(unsigned long long)vertexCount, (unsigned)sizeof(BoxVertex),
		(unsigned long long)(vertexCount * sizeof(BoxVertex)));
}

DrawnLineBuffer::~DrawnLineBuffer()
{
	delete mBoxes;
	delete mBuffer;
}

void DrawnLineBuffer::Upload(const DrawnLineRecord *records, unsigned count, unsigned routed)
{
	if (count > mCapacity)
	{
		// SyncDrawnLines already stops at GetCapacity(), so this should be
		// unreachable. Clamp rather than write past the end, and say so once.
		if (!mWarnedClamp)
		{
			Printf("DrawnLines: asked to upload %u records into %u, clamping\n", count, mCapacity);
			mWarnedClamp = true;
		}
		count = mCapacity;
	}

	if (mBuffer == nullptr || records == nullptr || count == 0)
	{
		mLiveCount = 0;
		mRoutedCount = 0;
		return;
	}

	mBuffer->Map();
	if (mBuffer->Memory() == nullptr)
	{
		mBuffer->Unmap();
		mLiveCount = 0;
		mRoutedCount = 0;
		return;
	}
	memcpy(mBuffer->Memory(), records, (size_t)count * RECORD_BYTES);
	mBuffer->Unmap();

	mLiveCount = count;
	mRoutedCount = routed < count ? routed : count;
}

const char *DrawnLineBuffer::StateName() const
{
	if (ShaderFailed) return "FAILED (shader)";
	if (!ShaderReady) return "not ready";
	if (!r_drawnlines) return "off (r_drawnlines 0)";
	return "ready";
}

bool BeamsRouteToDrawnLines()
{
	return r_beams_drawn && r_drawnlines && screen != nullptr &&
		screen->mDrawnLines != nullptr && screen->mDrawnLines->IsDrawable();
}

void DrawnLinesLogToggle(bool on)
{
	// Also runs when the config file sets the value at startup, before any
	// renderer exists -- hence the null checks and the careful wording. The
	// shader compile line repeats the state once the path is real.
	const DrawnLineBuffer *lines = screen != nullptr ? screen->mDrawnLines : nullptr;
	const bool routes = on && BeamsRouteToDrawnLines();
	Printf("r_beams_drawn %s -- drawn path %s; SetBeam lines draw %s%s\n",
		on ? "ON" : "off",
		lines == nullptr ? "not available (Vulkan only, or the renderer is not up yet)" : lines->StateName(),
		routes ? "as drawn lines" : "per pixel, as always",
		routes ? (r_beams_drawn_surfacelight ? " (surface light kept per pixel)" : " (no surface light: r_beams_drawn_surfacelight 0)") : "");
}
