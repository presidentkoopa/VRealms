/*
** hw_sectorplanebuffer.h
**
** [SECTORPLANES] Sectors' current floor and ceiling plane equations on the GPU.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: effects that live on the GPU need to know where floors and
** ceilings are NOW -- a door that opened, a lift that rose -- and the level mesh
** only knows where they were at load ("Engine docs/REVIEW_SMOKE_DEBRIS_DAMAGE.md"
** S3, D1, M3). This is one shared buffer of every polled sector's two planes, so
** the smoke mask (#13), the level collision field (#8), debris waking on a moved
** floor (#9) and wall damage anchored to a moving door (#17) all read the same
** data. The renderer polls the planes itself (SectorPlanes, hw_sectorplanes.h);
** no mover or game code is involved.
**
** Presentation only: nothing reads it back. Vulkan only, exactly like
** ViewLightBuffer: created beside it in VulkanRenderDevice::InitializeState, null
** on GL and GLES. Set 1 binding 12 (vk_descriptorset.cpp), vertex and fragment.
**
** GLSL for the first shader that reads it (no lump declares it yet, so no scene
** shader changed when it was added):
**
**   struct SectorPlane { vec4 floorPlane; vec4 ceilingPlane; };
**   layout(set = 1, binding = 12, std430) readonly buffer SectorPlaneSSO
**   {
**       vec4 sectorPlaneInfo;            // x sectors with a record  y capacity  zw 0
**       SectorPlane sectorPlanes[];      // by sector index (sector_t::Index())
**   };
**
*/

#pragma once

#include <cstdint>

class IDataBuffer;

// One sector: two planes, 32 bytes, std430 with no padding, in SHADER space (x, z, y --
// y up, the axes every world position in the shaders uses).
//
// A plane is (n.xyz, w) with n = (normal.X, normal.Z, normal.Y) of the sector's
// secplane_t and w = its D. For a world position p in shader space,
// dot(n.xyz, p) + w is > 0 on the OPEN side of both planes: above the floor (its
// normal points up) and below the ceiling (its normal points down). The height of a
// plane at shader (x, z) is -(n.x * x + n.z * z + w) / n.y.
//
// A record never written is all zero: dot + w == 0 everywhere, neither side. Readers
// treat an index at or beyond sectorPlaneInfo.x as unknown.
struct SectorPlaneRecord
{
	float floorPlane[4];
	float ceilingPlane[4];
};

class SectorPlaneBuffer
{
public:
	static const unsigned RECORD_BYTES = 32;

	// One vec4 ahead of the records: x sectors covered (the level's count, clamped to
	// CAPACITY), y CAPACITY, zw 0.
	static const unsigned HEADER_BYTES = 16;

	// Sectors with a record. A map with more sectors than this leaves the rest unknown
	// (logged once per map by SectorPlanes). 2 MB at 32 B a sector.
	static const unsigned CAPACITY = 65536;

	SectorPlaneBuffer();
	~SectorPlaneBuffer();

	// The level's sector count; rewrites the header only when it changes.
	void SetSectorCount(unsigned count);

	// A new level: zeroes every record the last level could have written (so no sector
	// of the new map reads the old map's planes before it is polled), then sets the count.
	void BeginLevel(unsigned count);

	// One sector's record. False (and nothing written) at or beyond CAPACITY or when the
	// buffer cannot be written.
	bool Write(unsigned index, const SectorPlaneRecord &record);

	IDataBuffer *GetBuffer() const { return mBuffer; }
	unsigned GetSectorCount() const { return mCount; }

private:
	IDataBuffer *mBuffer = nullptr;
	unsigned mCount = 0;
	bool mCountWritten = false;
};
