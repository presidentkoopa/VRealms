/*
** hw_effectlights.h
**
** [EFFECTLIGHTS] Effect lights: short lights that belong to no actor, moved at frame
** rate, sorted into world bins around the eye once a frame.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/LIGHTS_20_21_22_PLAN.md" (#22 moving line lights, #21 many short effect lights) and
** "Engine docs/EFFECT_LIGHTS_CORE_IMPL_NOTES.md". The lights mods place today (FDynamicLight: map lamps, muzzle flashes)
** are untouched; this is a separate, render-only path that stays inert until something spawns an effect light.
**
** WHERE A LIGHT COMES FROM.
**   - LevelLocals.SpawnEffectLight (doombase.zs): fire and forget, queued one-way on the level (FEffectLightEvent,
**     g_levellocals.h) and read here.
**   - LevelLocals.SetDrawnLineLight: a drawn line that also lights along its whole length, every frame, from the line's own
**     interpolated or anchored ends (ResolveDrawnLineEnds, hw_drawinfo.cpp).
**   - Spawn(): a renderer-side source -- the particle and debris light keys of a later step.
**   - r_effectlights_test: a local test spray.
**
** ONE FRAME (main view only, before the eye loop; both eyes share the result).
**   1. BeginFrame, before the shadow map: the queue drained, every light evaluated at this frame's level time, the pool
**      capped by rank, and the point lights walls block put in row order.
**   2. The shadow map's CollectLights (hw_entrypoint.cpp) gives them rows after the dynamic lights (AssignShadowRows). The map
**      pass runs for them even while "Light shadows" is off, and then no dynamic light takes a row.
**   3. PrepareFrame, in the compute block: drawn-line lights resolved, lights binned (EffectLightCore::BinBuilder), uploaded
**      (EffectLightBuffer, set 1 bindings 14 and 15).
**
** WHAT READS IT. The consumers are separate steps: surfaces (main.fp's EFFECT_LIGHTS block, gated per draw), particles and
** debris (vertex), the smoke (compute). None of them is switched on by this step.
**
** NETPLAY. Presentation only: the natives queue events and return nothing; nothing here writes the playsim or reads the GPU
** back; seeds are local hashes; nothing is saved (a map change or savegame load empties the pool through LevelDataSerial).
**
*/

#pragma once

#include <cstdint>
#include <vector>
#include "vectors.h"
#include "hw_effectlightcore.h"

struct FLevelLocals;
class IShadowMap;

// Where drawn line `index` is this frame: SyncDrawnLines' own interpolation and anchor (hw_drawinfo.cpp), so a drawn-line
// light sits exactly on the line it draws. False while the line is not live. Game axes.
bool ResolveDrawnLineEnds(FLevelLocals *Level, int index, double viewTicFrac, DVector3 &a, DVector3 &b);

class EffectLights
{
public:
	static EffectLights &Get();

	// Step 1. Once per displayed frame, main view only, right after R_SetupFrame and before the shadow map. levelAllowsRows:
	// the level may have a shadow map (no LEVEL3_NOSHADOWMAP, an AABB tree). Returns true when point lights want shadow-map
	// rows this frame, so the map pass must run.
	bool BeginFrame(FLevelLocals *Level, const DVector3 &eye, double yaw, double ticFrac, uint64_t levelSerial, bool levelAllowsRows);

	// Step 2. Inside the shadow map's CollectLights, after the dynamic lights: rows for the point lights walls block, in
	// row order, from `firstFreeRow` while rows remain. Returns the next free row (firstFreeRow when none is taken).
	int AssignShadowRows(IShadowMap *shadowMap, int firstFreeRow);

	// Step 3. In the compute block, after VRMode::SetUp (a drawn-line light may be anchored to a hand).
	void PrepareFrame(FLevelLocals *Level, const DVector3 &eye, double ticFrac);

	// A renderer-side light (a later step's particles and debris): into the pool at the next BeginFrame. Its Birth is in level
	// seconds; its Sequence is assigned then.
	void Spawn(const EffectLightCore::Source &source);

	// This frame's upload as the CPU built it (valid after PrepareFrame, until the next): the records, the occupied grid, the bin
	// words and indices -- for a CPU-side consumer's decisions, such as whether any binned light reaches the smoke box. Read-only.
	const EffectLightCore::BinResult &FrameBins() const { return mResult; }

private:
	struct PoolLight
	{
		EffectLightCore::Source Source;
		EffectLightCore::Evaluated State;
		double Distance = 0.0;	// eye to the segment, this frame
		bool InRange = false;	// within r_effectlights_distance (plus its radius) of the eye
		int Row = -1;			// this frame's shadow-map row
	};

	void MakeTestLights(double dt, double yaw);
	int PoolCap() const;

	std::vector<PoolLight> mPool;
	std::vector<PoolLight> mPoolScratch;
	std::vector<EffectLightCore::Source> mNew;
	std::vector<EffectLightCore::Source> mSpawned;
	std::vector<EffectLightCore::RankKey> mKeys;
	std::vector<uint32_t> mOrder;
	std::vector<EffectLightCore::RowCandidate> mRowOrder;
	std::vector<EffectLightCore::BinLight> mFrameLights;
	std::vector<EffectLightCore::BinLight> mRanked;
	EffectLightCore::BinBuilder mBuilder;
	EffectLightCore::BinResult mResult;

	DVector3 mEye;
	uint64_t mLevelSerial = 0;
	uint64_t mSequence = 0;
	uint64_t mFrame = 0;
	uint64_t mRowsFrame = 0;
	uint64_t mQueueSerial = 0;
	int mQueueCount = 0;
	uint64_t mRowsWantedMs = 0;	// I_msTime of the last frame a point light wanted a row (the map pass lingers after it)
	double mNow = 0.0;
	double mLastNow = -1.0;
	double mTestCarry = 0.0;
	double mTestLineNext = 0.0;
	uint32_t mTestCounter = 0;
	bool mActive = false;
	bool mBlocking = false;
	bool mBegan = false;
	bool mFirstBinLogged = false;
};
