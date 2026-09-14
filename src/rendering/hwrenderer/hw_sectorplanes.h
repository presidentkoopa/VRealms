/*
** hw_sectorplanes.h
**
** [SECTORPLANES] The renderer's own watch on sector floors and ceilings.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists ("Engine docs/SMOKE_VOLUME_PLAN.md" SH2, review S3): an effect
** built over the level -- the smoke volume's solid mask (#13), the level collision
** field (#8), debris resting on a lift (#9), damage on a door face (#17) -- must
** notice when a door opens or a lift moves. The renderer already notices for its own
** vertex buffer (CheckPlanes, hw_vertexbuilder.cpp, against sector_t::vboheight), but
** those heights belong to that buffer. This is the same idea with its OWN memory:
**
**   - per sector, the GetPlaneTexZ of floor and ceiling as THIS poller last saw them,
**     and the plane equations it last uploaded;
**   - each frame, only for sectors under a box some active effect polls, a compare;
**     a changed sector gets a new generation, goes on the frame's change list, and its
**     record in the shared GPU buffer (SectorPlaneBuffer, set 1 binding 12) is
**     rewritten.
**
** No mover or game code is touched and nothing is written back: the playsim moves
** planes, the renderer looks. With no effect polling, nothing is done at all -- not
** even the per-level set-up.
**
** How an effect uses it, once per frame after RenderViewpoint's BeginFrame:
**   PollBox(level, its box)            -- it is active over that region
**   ChangedThisFrame()                 -- a per-frame user: the sectors that moved
**   ChangedAt(sector) > my generation  -- a user that skips frames (a per-tic mask
**                                         rebuild): everything that moved since, then
**                                         remember Generation()
**
** Main thread only, like the rest of frame set-up.
**
*/

#pragma once

#include <cstdint>
#include <vector>

#include "hw_sectorplanebuffer.h"

struct FLevelLocals;
struct sector_t;

class SectorPlanes
{
public:
	static SectorPlanes& Get();

	// Once per frame for the main view (hw_entrypoint.cpp), before any effect polls.
	// levelSerial changes on every map change and savegame load.
	void BeginFrame(FLevelLocals* Level, uint64_t levelSerial);

	// Compares every sector whose extent touches [minX, maxX] x [minY, maxY] (map units)
	// -- and the height-transfer sector and 3D floor model sectors it uses, as the
	// renderer's CheckUpdate does -- with what this poller last saw. Each sector at most
	// once a frame, however many boxes touch it.
	void PollBox(FLevelLocals* Level, double minX, double minY, double maxX, double maxY);

	// Rises by one for every change seen. 0 = nothing seen yet this level.
	uint32_t Generation() const { return mGeneration; }

	// The generation at which a sector last changed (0 = not since the level began).
	uint32_t ChangedAt(int sectorIndex) const;

	// Sector indices that changed in this frame's polls.
	const std::vector<int>& ChangedThisFrame() const { return mChangedThisFrame; }

	// How many sectors this frame's polls compared (for the perf log and tests).
	int PolledThisFrame() const { return mPolledThisFrame; }

private:
	void EnsureLevel(FLevelLocals* Level);
	void PollSector(sector_t* sector);

	struct Seen
	{
		double TexZ[2] = { 0, 0 };		// floor, ceiling: GetPlaneTexZ as this poller last saw it
		SectorPlaneRecord Record = {};	// the planes it last uploaded
		bool Valid = false;
	};

	std::vector<Seen> mSeen;
	std::vector<uint32_t> mChangedAt;
	std::vector<uint32_t> mPolledFrame;
	std::vector<double> mExtent;		// per sector: min x, min y, max x, max y of its lines
	std::vector<int> mChangedThisFrame;

	FLevelLocals* mFrameLevel = nullptr;
	uint64_t mFrameSerial = 0;
	uint64_t mLevelSerial = 0;
	unsigned mSectorCount = 0;
	bool mLevelReady = false;
	bool mCapacityWarned = false;

	uint32_t mFrame = 0;
	uint32_t mGeneration = 0;
	int mPolledThisFrame = 0;
};
