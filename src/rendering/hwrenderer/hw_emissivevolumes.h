/*
** hw_emissivevolumes.h
**
** [EMISSIVEVOLUMES] Emissive volumes: short-lived glowing gas volumes -- a fireball, a gout, a star of petals, a ring --
** placed by a mod, moved at frame rate, drawn by a raymarch per eye.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/VOLUMETRIC_FLASH_15_PLAN.md" (#15, with its OWNER ANSWERS) and "Engine docs/EMISSIVE_VOLUMES_15_IMPL_NOTES.md".
** A render-only capability, named for what it does: muzzle flashes are its first caller; explosion cores, plasma bursts and
** the BFG blast are the next. Inert until something spawns one: no pass, no image, no light.
**
** WHERE A VOLUME COMES FROM.
**   - LevelLocals.SpawnEmissiveVolume (doombase.zs): fire and forget, queued one-way on the level (FEmissiveVolumeEvent,
**     g_levellocals.h) and read here. Its look is a VOLUMEDEFS definition (gamedata/volumedefs.h).
**   - Spawn(): a renderer-side source (C++ callers).
**   - r_emissivevolumes_test: a local test source.
**
** ONE FRAME (main view only; both eyes share the result).
**   1. BeginFrame, before EffectLights::BeginFrame: the queue drained, every volume at this frame's level time, the pool capped
**      by rank, and each lit volume's light handed to EffectLights::AddFrameLight -- so it ranks, takes a shadow-map row (walls
**      block it) and lights the haze through the smoke's pass 2 like any effect light.
**   2. PrepareFrame, in the compute block after VRMode::SetUp: every volume again with the hand poses written THIS frame, the
**      best EMISSIVE_VOLUMES_DRAWN_MAX by rank, far to near, into the list texels (EmissiveVolumeFrame) the backend uploads.
**   3. SetupEmissiveVolumes (hw_drawinfo.cpp) publishes each eye's march to PPEmissiveVolumes (hw_postprocess.h).
**
** THE OWNER'S SETTINGS, all renderer-read every frame (a menu freezes the game; these still change what is on screen):
**   r_emissivevolumes         "Volumetric flashes": 3 All, 2 Big guns and blasts, 1 Explosions only, 0 Off
**   r_emissivevolumes_length  "Flash length": 0 By gun, 1 Snap, 2 Linger (not a blast's)
**   r_emissivevolumes_motion  "Flash motion": 0 Per definition, 1 Stays where fired, 2 Rides the gun
**
** NETPLAY. Presentation only: the natives queue events and return nothing (the handle is a hash of a name; the class query
** answers only for this machine's presentation); nothing here writes the playsim or reads the GPU back; seeds are local
** hashes; a followed pose names its player; nothing is saved (a map change or savegame load empties the pool).
**
*/

#pragma once

#include <cstdint>
#include <vector>
#include "vectors.h"
#include "hw_emissivevolumecore.h"
#include "hw_emissivevolumeframe.h"

struct FLevelLocals;

// THE PRESENTATION QUERY (LevelLocals.EmissiveVolumeEnabled): whether a definition's volumes draw on THIS machine now -- the
// definition is loaded here, the backend is Vulkan, and "Volumetric flashes" draws its class. A mod uses it only to choose
// what to DRAW (a volume, or today's cone and flame card); nothing that affects play may branch on it.
bool EmissiveVolumeDefinitionEnabled(int handle);

class EmissiveVolumes
{
public:
	static EmissiveVolumes &Get();

	// Step 1. Once per displayed frame, main view only, before EffectLights::BeginFrame.
	void BeginFrame(FLevelLocals *Level, const DVector3 &eye, double yaw, double ticFrac, uint64_t levelSerial);

	// Step 2. In the compute block, after VRMode::SetUp. Fills `out` (Count 0: nothing to draw).
	void PrepareFrame(FLevelLocals *Level, const DVector3 &eye, EmissiveVolumeFrame &out);

	// A renderer-side volume: into the pool at the next BeginFrame. Its Birth is in level seconds; its Sequence is assigned then.
	void Spawn(const EmissiveVolumeCore::Spawn &spawn);

	// This frame's drawing as PrepareFrame left it, for the per-eye setup (SetupEmissiveVolumes). Read-only.
	struct DrawState
	{
		int Count = 0;
		uint64_t Serial = 0;
		double Origin[3] = { 0.0, 0.0, 0.0 };					// the list origin, game axes, whole map units
		float Centre[EMISSIVE_VOLUMES_DRAWN_MAX][3] = {};		// each drawn volume's bounding sphere: its centre, GL axes from Origin
		float Radius[EMISSIVE_VOLUMES_DRAWN_MAX] = {};			// and its radius
		bool Absorbs = false;									// some drawn volume hides what is behind it (absorption above 0)
	};
	const DrawState &GetDrawState() const { return mDraw; }

private:
	struct PoolVolume
	{
		EmissiveVolumeCore::Spawn Source;
		EmissiveVolumeCore::State State;
		double Distance = 0.0;	// the eye to its bounding sphere's centre, this frame
	};

	void MakeTestVolumes(double dt, double yaw);
	EmissiveVolumeCore::Settings CurrentSettings() const;

	std::vector<PoolVolume> mPool;
	std::vector<PoolVolume> mPoolScratch;
	std::vector<EmissiveVolumeCore::Spawn> mNew;
	std::vector<EmissiveVolumeCore::Spawn> mSpawned;
	std::vector<uint32_t> mOrder;
	std::vector<float> mTexels;
	std::vector<float> mLastTexels;
	DrawState mDraw;

	DVector3 mEye;
	uint64_t mLevelSerial = 0;
	uint64_t mSequence = 0;
	uint64_t mQueueSerial = 0;
	int mQueueCount = 0;
	uint64_t mListSerial = 0;
	double mNow = 0.0;
	double mLastNow = -1.0;
	double mTestCarry = 0.0;
	uint32_t mTestCounter = 0;
	bool mActive = false;
	bool mBegan = false;
	bool mFirstDrawLogged = false;
};
