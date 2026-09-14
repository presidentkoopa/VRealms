/*
** hw_smokevolume.cpp
**
** [SMOKEVOLUME] The smoke volume's CPU side. See the header.
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
#include <cmath>

#include "hw_smokevolume.h"
#include "hw_levelsolidity.h"
#include "hw_sectorplanes.h"
#include "hw_framecompute.h"
#include "hw_cvars.h"
#include "hw_perflog.h"
#include "g_levellocals.h"
#include "doomdef.h"
#include "v_video.h"
#include "i_time.h"
#include "printf.h"

SmokeVolume& SmokeVolume::Get()
{
	static SmokeVolume volume;
	return volume;
}

void SmokeVolume::PrepareFrame(FLevelLocals* Level, const DVector3& eye, double ticFrac, uint64_t levelSerial, SmokeVolumeFrame& out)
{
	out = SmokeVolumeFrame();
	if (Level == nullptr)
		return;

	const int maptime = Level->maptime;
	const int quality = std::clamp((int)r_smoke_quality, SMOKE_QUALITY_MIN, SMOKE_QUALITY_MAX);
	// Renderer-read every frame, so all three respond with a menu open. Vulkan only: GL
	// and GLES run no compute, so they do no CPU work for it either.
	const bool vulkan = screen != nullptr && screen->IsVulkan();
	const bool smokeOn = vulkan && r_smoke;
	const bool computeTest = smokeOn && r_smoke_computetest;

	if (levelSerial != mLevelSerial)
	{
		mLevelSerial = levelSerial;
		mClockValid = false;
		mPlaced = false;
		// A volume in use keeps lingering into the new map, so it is not freed now and
		// made again at the new map's first emit.
		if (mHasDemand)
			mLastDemandTime = maptime;
	}

	// Who asks for smoke. [13b] || an EmitSmoke or CarveSmoke reached this level's
	// per-tic queues (SH4) this tic.
	const bool demandNow = computeTest;
	if (demandNow)
	{
		mHasDemand = true;
		mLastDemandTime = maptime;
	}
	if (maptime < mLastDemandTime)
		mLastDemandTime = maptime;
	if (mHasDemand && maptime - mLastDemandTime >= LINGER_SECONDS * TICRATE)
		mHasDemand = false;

	out.Active = smokeOn && mHasDemand;
	out.ComputeTest = out.Active && computeTest;
	out.Quality = quality;
	out.Grid = SmokeGridForQuality(quality);
	out.MapTime = maptime;
	out.TicFrac = (float)ticFrac;
	out.LevelSerial = levelSerial;

	// One step per world tic, at most MAX_STEPS_PER_FRAME a frame; a bigger jump drops
	// the backlog. The clock only runs while the volume is active.
	if (!out.Active || !mClockValid || maptime < mLastStepTime)
	{
		mClockValid = true;
		mLastStepTime = maptime;
		out.Steps = 0;
	}
	else
	{
		out.Steps = std::min(maptime - mLastStepTime, SmokeVolumeFrame::MAX_STEPS_PER_FRAME);
		mLastStepTime = maptime;
	}

	if (!out.Active)
	{
		mPlaced = false;
		ResetMaskTest();
		return;
	}

	// The box: whole cells, world-aligned.
	const SmokeGridSpec& grid = out.Grid;
	const double cell = grid.CellSize;
	const double extent[3] = { grid.SizeX * cell, grid.SizeY * cell, grid.SizeZ * cell };
	const double eyeShare[3] = { 0.5, 0.5, EYE_HEIGHT_FRACTION };
	const double eyeAt[3] = { eye.X, eye.Y, eye.Z };

	bool place = !mPlaced || mPlacedQuality != quality;
	for (int axis = 0; axis < 3 && !place; axis++)
	{
		const double inBox = eyeAt[axis] - mOriginCell[axis] * cell;
		if (std::fabs(inBox - extent[axis] * eyeShare[axis]) > extent[axis] * RECENTRE_FRACTION)
			place = true;
	}
	if (place)
	{
		for (int axis = 0; axis < 3; axis++)
			mOriginCell[axis] = (int)std::floor((eyeAt[axis] - extent[axis] * eyeShare[axis]) / cell + 0.5);
		mPlaced = true;
		mPlacedQuality = quality;
		// [13b] a recentre shifts the volume's contents by the whole-cell difference
		// (smoke_shift.comp) and re-masks the slabs it exposes.
	}
	for (int axis = 0; axis < 3; axis++)
		out.OriginCell[axis] = mOriginCell[axis];

	// [SECTORPLANES] The box is an active region: watch the sectors under it.
	const double minX = mOriginCell[0] * cell;
	const double minY = mOriginCell[1] * cell;
	SectorPlanes::Get().PollBox(Level, minX, minY, minX + extent[0], minY + extent[1]);

	// [LEVELSOLIDITY] The desk test of the solid mask.
	if (out.ComputeTest)
		RunMaskTest(Level, out);
	else
		ResetMaskTest();
}

void SmokeVolume::ResetMaskTest()
{
	if (!mMaskStarted)
		return;
	std::vector<uint8_t>().swap(mMask);	// give the memory back
	mMaskStarted = false;
	mMaskDone = false;
}

void SmokeVolume::RunMaskTest(FLevelLocals* Level, const SmokeVolumeFrame& frame)
{
	const SmokeGridSpec& grid = frame.Grid;

	const bool restart = !mMaskStarted || mMaskSerial != frame.LevelSerial || mMaskQuality != frame.Quality ||
		mMaskOrigin[0] != frame.OriginCell[0] || mMaskOrigin[1] != frame.OriginCell[1] || mMaskOrigin[2] != frame.OriginCell[2];
	if (restart)
	{
		mMask.assign((size_t)grid.Cells(), LevelSolidity::OPEN);
		mMaskStarted = true;
		mMaskDone = false;
		mMaskSerial = frame.LevelSerial;
		mMaskQuality = frame.Quality;
		for (int axis = 0; axis < 3; axis++)
			mMaskOrigin[axis] = frame.OriginCell[axis];
		mMaskRow = 0;
		mMaskFrames = 0;
		mMaskNs = 0;
		mMaskSolidCells = 0;
	}
	if (mMaskDone)
		return;

	LevelSolidity::Box box;
	box.OriginX = frame.OriginCell[0] * (double)grid.CellSize;
	box.OriginY = frame.OriginCell[1] * (double)grid.CellSize;
	box.OriginZ = frame.OriginCell[2] * (double)grid.CellSize;
	box.CellSize = grid.CellSize;
	box.SizeX = grid.SizeX;
	box.SizeY = grid.SizeY;
	box.SizeZ = grid.SizeZ;

	const int rowEnd = std::min(mMaskRow + MASK_TEST_ROWS_PER_FRAME, grid.SizeY);

	const uint64_t startNs = I_nsTime();
	LevelSolidity::RasterizeColumns(Level, box, 0, mMaskRow, grid.SizeX, rowEnd, mMask.data());
	const uint64_t elapsedNs = I_nsTime() - startNs;
	mMaskNs += elapsedNs;
	mMaskFrames++;
	if (PerfLog::GroupsWanted())
		PerfLog::AddCpuSample("fx.solidity", (double)elapsedNs / 1e6);

	// The slab's solid cells, counted outside the timing.
	const size_t layer = (size_t)grid.SizeX * (size_t)grid.SizeY;
	for (int z = 0; z < grid.SizeZ; z++)
	{
		for (int y = mMaskRow; y < rowEnd; y++)
		{
			const uint8_t* row = mMask.data() + layer * (size_t)z + (size_t)grid.SizeX * (size_t)y;
			for (int x = 0; x < grid.SizeX; x++)
			{
				if (row[x] != LevelSolidity::OPEN)
					mMaskSolidCells++;
			}
		}
	}

	mMaskRow = rowEnd;
	if (mMaskRow >= grid.SizeY)
	{
		mMaskDone = true;
		const double totalMs = (double)mMaskNs / 1e6;
		Printf("SmokeVolume: solidity mask test -- %d x %d x %d cells from (%d, %d, %d), %.1f%% solid, %.1f ms of CPU over %d frames (%.3f ms a frame)\n",
			grid.SizeX, grid.SizeY, grid.SizeZ,
			frame.OriginCell[0] * grid.CellSize, frame.OriginCell[1] * grid.CellSize, frame.OriginCell[2] * grid.CellSize,
			100.0 * (double)mMaskSolidCells / (double)grid.Cells(), totalMs, mMaskFrames,
			mMaskFrames > 0 ? totalMs / mMaskFrames : 0.0);
	}
}
