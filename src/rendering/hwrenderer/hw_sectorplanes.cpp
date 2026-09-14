/*
** hw_sectorplanes.cpp
**
** [SECTORPLANES] The renderer's own watch on sector floors and ceilings. See the
** header.
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

#include "hw_sectorplanes.h"
#include "hw_sectorplanebuffer.h"
#include "hw_perflog.h"
#include "g_levellocals.h"
#include "r_defs.h"
#include "p_3dfloors.h"
#include "v_video.h"
#include "i_time.h"
#include "printf.h"

SectorPlanes& SectorPlanes::Get()
{
	static SectorPlanes planes;
	return planes;
}

// A sector's two planes in shader space -- see SectorPlaneRecord.
static void MakePlaneRecord(const sector_t* sector, SectorPlaneRecord& record)
{
	const secplane_t& floorPlane = sector->floorplane;
	const secplane_t& ceilingPlane = sector->ceilingplane;

	record.floorPlane[0] = (float)floorPlane.Normal().X;
	record.floorPlane[1] = (float)floorPlane.Normal().Z;
	record.floorPlane[2] = (float)floorPlane.Normal().Y;
	record.floorPlane[3] = (float)floorPlane.fD();

	record.ceilingPlane[0] = (float)ceilingPlane.Normal().X;
	record.ceilingPlane[1] = (float)ceilingPlane.Normal().Z;
	record.ceilingPlane[2] = (float)ceilingPlane.Normal().Y;
	record.ceilingPlane[3] = (float)ceilingPlane.fD();
}

void SectorPlanes::BeginFrame(FLevelLocals* Level, uint64_t levelSerial)
{
	mChangedThisFrame.clear();
	mPolledThisFrame = 0;
	mFrameLevel = Level;
	mFrameSerial = levelSerial;

	if (++mFrame == 0)
	{
		// Wrapped (after 136 years at 90 Hz): forget which sectors this frame polled.
		mFrame = 1;
		std::fill(mPolledFrame.begin(), mPolledFrame.end(), 0u);
		std::fill(mChangedFrame.begin(), mChangedFrame.end(), 0u);
	}
}

void SectorPlanes::EnsureLevel(FLevelLocals* Level)
{
	const unsigned count = Level->sectors.Size();
	if (mLevelReady && mLevelSerial == mFrameSerial && mSectorCount == count)
		return;

	mLevelReady = true;
	mLevelSerial = mFrameSerial;
	mSectorCount = count;
	mGeneration = 0;
	mCapacityWarned = false;

	mSeen.assign(count, Seen());
	mChangedAt.assign(count, 0u);
	mPolledFrame.assign(count, 0u);
	mChangedFrame.assign(count, 0u);
	mExtent.assign((size_t)count * 4, 0.0);

	// Each sector's extent from its own lines. A sector with no lines (rare; a control
	// sector always has some) gets an empty extent that no box touches.
	for (unsigned i = 0; i < count; i++)
	{
		const sector_t& sector = Level->sectors[i];
		double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
		for (unsigned k = 0; k < sector.Lines.Size(); k++)
		{
			const line_t* line = sector.Lines[k];
			if (line == nullptr || line->v1 == nullptr || line->v2 == nullptr)
				continue;
			minX = std::min({ minX, line->v1->fX(), line->v2->fX() });
			minY = std::min({ minY, line->v1->fY(), line->v2->fY() });
			maxX = std::max({ maxX, line->v1->fX(), line->v2->fX() });
			maxY = std::max({ maxY, line->v1->fY(), line->v2->fY() });
		}
		double* extent = &mExtent[(size_t)i * 4];
		extent[0] = minX;
		extent[1] = minY;
		extent[2] = maxX;
		extent[3] = maxY;
	}

	if (screen != nullptr && screen->mSectorPlanes != nullptr)
		screen->mSectorPlanes->BeginLevel(count);

	if (count > SectorPlaneBuffer::CAPACITY)
	{
		Printf("SectorPlanes: this map has %u sectors; the GPU buffer holds %u -- sectors beyond that stay unknown to GPU effects\n",
			count, SectorPlaneBuffer::CAPACITY);
	}
}

uint32_t SectorPlanes::ChangedAt(int sectorIndex) const
{
	if (sectorIndex < 0 || (size_t)sectorIndex >= mChangedAt.size())
		return 0;
	return mChangedAt[sectorIndex];
}

bool SectorPlanes::GetExtent(int sectorIndex, double& minX, double& minY, double& maxX, double& maxY) const
{
	if (!mLevelReady || sectorIndex < 0 || (unsigned)sectorIndex >= mSectorCount || (size_t)sectorIndex * 4 + 3 >= mExtent.size())
		return false;
	const double* extent = &mExtent[(size_t)sectorIndex * 4];
	minX = extent[0];
	minY = extent[1];
	maxX = extent[2];
	maxY = extent[3];
	return minX <= maxX && minY <= maxY;
}

// [13b] One change of a sector's open space: a new generation, once a frame.
void SectorPlanes::MarkChanged(int index)
{
	if (mChangedFrame[index] == mFrame)
		return;
	mChangedFrame[index] = mFrame;
	mChangedAt[index] = ++mGeneration;
	mChangedThisFrame.push_back(index);
}

void SectorPlanes::PollBox(FLevelLocals* Level, double minX, double minY, double maxX, double maxY)
{
	// Only for the level this frame began with.
	if (Level == nullptr || Level != mFrameLevel)
		return;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	EnsureLevel(Level);

	for (unsigned i = 0; i < mSectorCount; i++)
	{
		const double* extent = &mExtent[(size_t)i * 4];
		if (extent[2] < minX || extent[0] > maxX || extent[3] < minY || extent[1] > maxY)
			continue;

		sector_t* sector = &Level->sectors[i];
		PollSector(sector);

		// What the renderer's CheckUpdate also watches for this sector. [13b] When one of
		// those moved, this sector's open space moved with it (a 3D lift's platform, a
		// deep-water transfer), so it is marked changed too -- a user re-doing the area
		// under a changed sector would otherwise look at the model's control sector,
		// usually somewhere off the map, and never here.
		bool modelChanged = false;
		if (sector_t* heightSector = sector->GetHeightSec())
			modelChanged |= PollSector(heightSector);
		if (sector->e != nullptr)
		{
			for (F3DFloor* ffloor : sector->e->XFloor.ffloors)
			{
				if (ffloor != nullptr && ffloor->model != nullptr)
					modelChanged |= PollSector(ffloor->model);
			}
		}
		if (modelChanged)
			MarkChanged((int)i);
	}

	if (timed)
		PerfLog::AddCpuSample("fx.sectorplanes", (double)(I_nsTime() - startNs) / 1e6);
}

bool SectorPlanes::PollSector(sector_t* sector)
{
	if (sector == nullptr)
		return false;
	const int index = sector->Index();
	if (index < 0 || (unsigned)index >= mSectorCount)
		return false;
	if (mPolledFrame[index] == mFrame)
		return mChangedFrame[index] == mFrame;
	mPolledFrame[index] = mFrame;
	mPolledThisFrame++;

	// The same test as CheckPlanes, against this poller's own last-seen heights -- never
	// sector_t::vboheight, which belongs to the vertex buffer -- plus the full planes, so
	// a slope that changes without moving its texture height is caught too.
	const double floorTexZ = sector->GetPlaneTexZ(sector_t::floor);
	const double ceilingTexZ = sector->GetPlaneTexZ(sector_t::ceiling);
	SectorPlaneRecord record;
	MakePlaneRecord(sector, record);

	Seen& seen = mSeen[index];
	if (seen.Valid && seen.TexZ[0] == floorTexZ && seen.TexZ[1] == ceilingTexZ &&
		memcmp(&seen.Record, &record, sizeof(record)) == 0)
	{
		return mChangedFrame[index] == mFrame;
	}

	seen.TexZ[0] = floorTexZ;
	seen.TexZ[1] = ceilingTexZ;
	seen.Record = record;
	seen.Valid = true;

	MarkChanged(index);

	if (screen != nullptr && screen->mSectorPlanes != nullptr && (unsigned)index < SectorPlaneBuffer::CAPACITY)
		screen->mSectorPlanes->Write((unsigned)index, record);
	return true;
}
