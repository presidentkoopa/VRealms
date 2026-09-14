/*
** hw_smokevolume.h
**
** [SMOKEVOLUME] The smoke volume's CPU side: when it exists, where its box is, how
** many steps a frame runs.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SMOKE_VOLUME_PLAN.md" #13. Everything here survives a render rebuild:
** it only reads the level and fills SmokeVolumeFrame (hw_framecompute.h); the GPU
** side (vk_smokevolume.cpp) acts on it.
**
** Step 1 (13a) has:
**   - DEMAND: the volume exists while r_smoke is on and something asked for smoke on
**     this map within the last LINGER_SECONDS of level time. In 13a the only asker is
**     r_smoke_computetest; [13b] a mod's EmitSmoke / CarveSmoke (the SH4 queues) joins
**     it. Level time freezes in a menu, so pausing never frees and re-makes 150 MB, and
**     the linger carries into the next map, so a map change does not either.
**   - STEPS: one per world tic, at most SmokeVolumeFrame::MAX_STEPS_PER_FRAME a frame;
**     a bigger jump (a hitch, a load) drops the backlog (review S7).
**   - PLACEMENT: a world-aligned box in whole cells, centred on the eye in x and y
**     with the eye at EYE_HEIGHT_FRACTION of its height (more room above than below:
**     smoke rises to ceilings), recentred when the eye strays more than
**     RECENTRE_FRACTION of an extent. [13b] recentring shifts the contents
**     (smoke_shift.comp) and re-masks the newly exposed slabs.
**   - SH2: the box is an active box for SectorPlanes, polled every active frame.
**   - The desk MASK TEST (r_smoke_computetest): SH1's LevelSolidity rasterises the
**     whole box on the CPU, MASK_TEST_ROWS_PER_FRAME rows a frame, timed as
**     fx.solidity, and logs the solid share and cost when done. [13b] the same
**     rasterisation feeds the R8 mask texture.
**
** Main thread only. Presentation only: nothing here writes to the playsim.
**
*/

#pragma once

#include <cstdint>
#include <vector>

#include "vectors.h"

struct FLevelLocals;
struct SmokeVolumeFrame;

class SmokeVolume
{
public:
	static constexpr int LINGER_SECONDS = 60;
	static constexpr double EYE_HEIGHT_FRACTION = 0.375;
	static constexpr double RECENTRE_FRACTION = 0.125;
	static constexpr int MASK_TEST_ROWS_PER_FRAME = 8;

	static SmokeVolume& Get();

	// Once per frame for the main view, after SectorPlanes::BeginFrame. eye is the view
	// position (map units), levelSerial changes on every map change and savegame load.
	void PrepareFrame(FLevelLocals* Level, const DVector3& eye, double ticFrac, uint64_t levelSerial, SmokeVolumeFrame& out);

private:
	void RunMaskTest(FLevelLocals* Level, const SmokeVolumeFrame& frame);
	void ResetMaskTest();

	uint64_t mLevelSerial = 0;

	// Demand and linger.
	bool mHasDemand = false;
	int mLastDemandTime = 0;

	// The step clock.
	bool mClockValid = false;
	int mLastStepTime = 0;

	// The box.
	bool mPlaced = false;
	int mPlacedQuality = 0;
	int mOriginCell[3] = { 0, 0, 0 };

	// The desk mask test.
	std::vector<uint8_t> mMask;
	bool mMaskStarted = false;
	bool mMaskDone = false;
	uint64_t mMaskSerial = 0;
	int mMaskQuality = 0;
	int mMaskOrigin[3] = { 0, 0, 0 };
	int mMaskRow = 0;
	int mMaskFrames = 0;
	uint64_t mMaskNs = 0;
	uint64_t mMaskSolidCells = 0;
};
