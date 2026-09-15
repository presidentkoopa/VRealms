/*
** hw_debrispool.cpp
**
** [DEBRISPOOL] The debris pool's CPU side. See the header.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** The burst expansion, the slot heap, the step clock and the pieces' starting turn are mirrored in Python ("Engine
** docs/DEBRIS_9_IMPL_NOTES.md", mirror9), with the GPU step itself.
**
*/

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hw_debrispool.h"
#include "hw_debrisframe.h"
#include "hw_debrislanding.h"	// [DEBRISSOUNDS] the groups' landing sounds
#include "hw_sectorplanes.h"
#include "hw_gpuparticlebuffer.h"
#include "hw_particledefbuffer.h"
#include "hw_meshparticles.h"
#include "hw_modelvertexbuffer.h"
#include "hw_renderstate.h"
#include "hw_perflog.h"
#include "hw_cvars.h"
#include "hwrenderer/data/buffers.h"
#include "flatvertices.h"
#include "model.h"
#include "texturemanager.h"
#include "particledefs.h"
#include "g_levellocals.h"
#include "doomdef.h"
#include "c_cvars.h"
#include "v_video.h"
#include "i_time.h"
#include "printf.h"
#include "r_defs.h"				// [PARTICLELIGHTS] sectors, planes and lines, for a light's landing through the level
#include "hw_particlelights.h"		// [PARTICLELIGHTS] which pieces throw light, and their lights
#include "hw_effectlights.h"			// [PARTICLELIGHTS] EffectLights::Spawn
#include "hw_effectlightbuffer.h"		// [PARTICLELIGHTS] EffectLightBuffer::Instance
#include "r_utility.h"	// [DEBRISSOUNDS] r_viewpoint.TicFrac: the landing sounds' clock is the draw's

// [DEBRISPOOL] r_debris -- "Debris that stays" ("Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #9). ON BY DEFAULT (owner,
// 2026-09-14: effects our mods use default ON); inert until a definition with `restitution` is spawned, so a map without
// one allocates and dispatches nothing. Off: those definitions' particles are drawn as ring particles (stateless: #8's
// field, #10's plane landing), and the pool is freed. Renderer-read every frame. Presentation only -- not SERVERINFO.
CVARD(Bool, r_debris, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "debris definitions (restitution) are simulated in the debris pool: they bounce, rest and stay; off = drawn as ring particles (Vulkan only)")
// "Debris time on the ground": every debris restlife of 5 seconds or more is scaled by this / 60, so 60 (the owner's
// default) keeps what the definitions say. Shorter rests are part of an effect's look and stay as written. Renderer-read by
// the draw every frame, so pieces on the ground respond with the menu open.
CUSTOM_CVARD(Float, r_debris_life, 60.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "debris time on the ground in seconds: scales every debris restlife of 5 s or more by this / 60 (5-600; Vulkan only)")
{
	if (self < 5.f) { self = 5.f; return; }
	if (self > 600.f) { self = 600.f; return; }
}
// "Debris pieces": how many the pool holds, 4,096 .. 65,536 (176 bytes each on the GPU: 16,384 = 2.9 MB). A change applies
// when the pool is empty, or on the next map.
CUSTOM_CVARD(Int, r_debris_pool, DEBRIS_POOL_DEFAULT, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "debris pieces the pool holds: 4096, 8192, 16384, 32768 or 65536; applies when the pool is empty or on the next map (Vulkan only)")
{
	const int snapped = DebrisPoolSizeFor(self);
	if (snapped != self) { self = snapped; return; }
}
// A test, not a setting: `collide = level` definitions without `restitution` go to the pool too, with test keys
// (restitution 0.3, friction 0.5; a mesh rests 10 seconds, a billboard never rests), so the pool can be judged on a mod's
// existing chunks and sparks before any definition says `restitution`. Not archived.
CVARD(Bool, r_debris_test, false, CVAR_GLOBALCONFIG, "collide = level particles without restitution go to the debris pool too, with test keys -- a test of the pool (Vulkan only)")

// [PARTICLELIGHTS] Read for a burst's lights.
EXTERN_CVAR(Bool, r_effectlights)			// hw_effectlights.cpp
EXTERN_CVAR(Bool, r_particlelights_test)	// hw_gpuparticlebuffer.cpp
EXTERN_CVAR(Bool, r_particlecollision)		// hw_levelfield.cpp: while off, the step collides with the spawn's plane only

namespace
{
	const double kPi = 3.14159265358979323846;

	// The test keys (r_debris_test).
	const float kTestRestitution = 0.3f;
	const float kTestFriction = 0.5f;
	const float kTestMeshRest = 10.0f;
	const float kTestRestFade = 0.5f;

	// hw_smokevolume.cpp's ReadQueue (that file is the smoke lane's): every event of one of the level's queues not read
	// before, the older generation first, from a reader's own cursor. Nothing is written to the queue.
	template<class T, int N, class Take>
	void ReadQueue(const FEffectTicQueue<T, N>& queue, uint64_t& cursorSerial, int& cursorCount, Take&& take)
	{
		int order[2] = { 0, 1 };
		if (queue.Serial[1] < queue.Serial[0])
		{
			order[0] = 1;
			order[1] = 0;
		}
		for (int g : order)
		{
			const uint64_t serial = queue.Serial[g];
			if (serial == 0 || serial < cursorSerial)
				continue;
			const int count = std::clamp(queue.Count[g], 0, N);
			const int first = serial == cursorSerial ? std::min(cursorCount, count) : 0;
			for (int i = first; i < count; i++)
				take(queue.Items[g][i], queue.Tic[g]);
			cursorSerial = serial;
			cursorCount = count;
		}
	}

	// meshparticles.vp's MeshRandom: a 24-bit value 0..1 from the seed's bits, per stream (FLevelLocals::GpuParticleHash).
	float MeshRandom(uint32_t seedBits, uint32_t stream)
	{
		return (float)(FLevelLocals::GpuParticleHash(seedBits ^ FLevelLocals::GpuParticleHash(stream * 0x9e3779b9U + 0x68bc21ebU)) >> 8) / 16777216.0f;
	}

	// [PARTICLELIGHTS] DebrisLanding::TraceLanding's questions for a piece's light, answered from the level read-only: hw_debrislanding.cpp's
	// UnitNormal and LevelQuery, token for token (that file keeps them in its own unnamed namespace).
	using DebrisLanding::V3;

	// A sector plane's unit normal (Doom axes); a degenerate plane counts as flat, facing zSign.
	V3 UnitNormal(const secplane_t& plane, double zSign)
	{
		const DVector3& n = plane.Normal();
		const double l = n.Length();
		if (!(l > 1.0e-12))
			return { 0.0, 0.0, zSign };
		return { n.X / l, n.Y / l, n.Z / l };
	}

	// DebrisLanding::TraceLanding's questions, answered from the level READ-ONLY: a BSP walk (PointInSector), sector planes and
	// a sector's own line list. No validcount, nothing written. A sector's handle is its index in Level->sectors.
	struct LevelQuery
	{
		FLevelLocals* Level = nullptr;

		intptr_t SectorAt(double x, double y) const
		{
			sector_t* sector = Level->PointInSector(x, y);
			return sector != nullptr ? (intptr_t)sector->sectornum : -1;
		}

		double FloorAt(intptr_t index, double x, double y, V3& normal) const
		{
			const secplane_t& plane = Level->sectors[(unsigned)index].floorplane;
			normal = UnitNormal(plane, 1.0);
			return plane.ZatPoint(DVector2(x, y));
		}

		double CeilingAt(intptr_t index, double x, double y, V3& normal) const
		{
			const secplane_t& plane = Level->sectors[(unsigned)index].ceilingplane;
			normal = UnitNormal(plane, -1.0);
			return plane.ZatPoint(DVector2(x, y));
		}

		bool FirstCrossing(intptr_t index, double x0, double y0, double x1, double y1, DebrisLanding::Crossing& crossing) const
		{
			sector_t* sector = &Level->sectors[(unsigned)index];
			const double dx = x1 - x0;
			const double dy = y1 - y0;
			bool found = false;
			for (line_t* line : sector->Lines)
			{
				if (line == nullptr || line->v1 == nullptr)
					continue;
				sector_t* beyond = line->frontsector == sector ? line->backsector : line->frontsector;
				if (beyond == sector)
					continue;    // this sector on both sides: nothing to stop at
				const DVector2 start = line->v1->fPos();
				const DVector2 along = line->delta;
				const double denominator = dx * along.Y - dy * along.X;
				if (std::fabs(denominator) < 1.0e-12)
					continue;
				const double ax = start.X - x0;
				const double ay = start.Y - y0;
				const double share = (ax * along.Y - ay * along.X) / denominator;
				const double onLine = (ax * dy - ay * dx) / denominator;
				if (share < 0.0 || share > 1.0 || onLine < 0.0 || onLine > 1.0)
					continue;
				if (found && share >= crossing.Share)
					continue;
				const double lineLength = along.Length();
				if (!(lineLength > 1.0e-9))
					continue;
				double nx = along.Y / lineLength;
				double ny = -along.X / lineLength;
				if (nx * (x0 - start.X) + ny * (y0 - start.Y) < 0.0)
				{
					nx = -nx;
					ny = -ny;
				}
				const DVector2 at(x0 + dx * share, y0 + dy * share);
				crossing.Share = share;
				crossing.Beyond = beyond != nullptr ? (intptr_t)beyond->sectornum : -1;
				crossing.FloorBeyond = beyond != nullptr ? beyond->floorplane.ZatPoint(at) : 0.0;
				crossing.CeilingBeyond = beyond != nullptr ? beyond->ceilingplane.ZatPoint(at) : 0.0;
				crossing.NormalX = nx;
				crossing.NormalY = ny;
				crossing.Offset = nx * start.X + ny * start.Y;
				found = true;
			}
			return found;
		}
	};


	// [PARTICLELIGHTS] The flight a burst's pieces share, as #11 builds it for their landing (DebrisLandingSounds::BeginBurst,
	// statement for statement): the definition's forces and bounce, the step's radius and the contact's reach, SpawnParticles'
	// plane and floor in shader axes, and for a burst in the air that collides with the level, the floor under it. Returns whether
	// its pieces are traced through the level: the definition collides with it and the collision field is on.
	bool LightFlight(FLevelLocals* Level, const FDebrisBurstEvent& burst, const DebrisDefinitionGpu& definition, float sizeTuning, DebrisLanding::Flight& shared)
	{
		shared = DebrisLanding::Flight();
		shared.Gravity = definition.Motion[0];
		shared.Drag = definition.Motion[1];
		shared.Restitution = definition.Physics[0];
		shared.Friction = definition.Physics[1];
		const double sizeScale = std::max(burst.SizeScale, 0.0) * (double)sizeTuning;
		double radius = 0.0;
		double reach = 0.0;
		if (definition.Body[1] > 0.5f)
		{
			const double scale = definition.Body[0] / std::max((double)definition.Body[2], 1.0e-4) * sizeScale;
			const double extentX = (double)definition.BoundsMax[0] - definition.BoundsMin[0];
			const double extentY = (double)definition.BoundsMax[1] - definition.BoundsMin[1];
			const double extentZ = (double)definition.BoundsMax[2] - definition.BoundsMin[2];
			radius = 0.5 * scale * std::min({ extentX, extentY, extentZ });
			reach = scale * (extentX + extentY + extentZ) / 4.0;
		}
		else
		{
			radius = 0.5 * definition.Body[0] * sizeScale;
			reach = radius;
		}
		shared.Radius = std::max(radius, DebrisLanding::kMinRadius);
		shared.Reach = std::max(reach, DebrisLanding::kMinRadius);
		const double normalLength = burst.SurfaceNormal.Length();
		if (normalLength > 1.0e-6)
		{
			shared.HasPlane = true;
			shared.PlaneNormal = { burst.SurfaceNormal.X / normalLength, burst.SurfaceNormal.Z / normalLength, burst.SurfaceNormal.Y / normalLength };
			shared.PlaneOffset = shared.PlaneNormal.x * burst.SurfacePoint.X + shared.PlaneNormal.y * burst.SurfacePoint.Z + shared.PlaneNormal.z * burst.SurfacePoint.Y;
		}
		if (burst.FloorZ > -32768.0)
		{
			shared.HasFloor = true;
			shared.FloorHeight = (float)burst.FloorZ;
		}
		const bool levelCollide = definition.Motion[2] > 1.5f && r_particlecollision;
		if (levelCollide && !shared.HasPlane && !shared.HasFloor)
		{
			if (sector_t* sector = Level->PointInSector(burst.Pos.X, burst.Pos.Y))
			{
				shared.HasFloor = true;
				shared.FloorHeight = sector->floorplane.ZatPoint(DVector2(burst.Pos.X, burst.Pos.Y));
			}
		}
		return levelCollide;
	}

	// [PARTICLELIGHTS] A piece's light: its first landing as #11 predicts it -- DebrisLanding::GroupBuilder's flight for one piece
	// (PredictFirstLanding against the burst's plane and floor, over DebrisLandingSounds::AddPiece's horizon), then, where its pieces
	// are traced, GroupFirstLanding's trace through the level -- and ParticleLights::DebrisSource.
	EffectLightCore::Source PieceLight(FLevelLocals* Level, const ParticleLights::SlotLight& light, const DebrisLanding::Flight& shared, bool traceLevel,
		bool neverRests, const float record[ParticleLights::RECORD_FLOATS], uint32_t hash)
	{
		DebrisLanding::Flight f = shared;
		f.Position = { record[0], record[1], record[2] };
		f.Velocity = { record[4], record[5], record[6] };
		// A piece that never rests is freed once its life is over (the step's `now - birth > life`), so it can only land before.
		int maxSteps = DebrisLanding::kMaxSteps;
		if (neverRests)
			maxSteps = (int)std::floor((double)record[ParticleLights::R_LIFE] / DebrisLanding::kStepSeconds + 1.0e-4);
		f.MaxSteps = std::clamp(maxSteps, 0, DebrisLanding::kMaxSteps);
		DebrisLanding::Landing landing = DebrisLanding::PredictFirstLanding(f, f.MaxSteps);
		if (traceLevel)
		{
			LevelQuery query;
			query.Level = Level;
			DebrisLanding::Landing trace;
			if (DebrisLanding::TraceLanding(f, query, trace))
				landing = trace;
		}
		const double point[3] = { landing.Point.x, landing.Point.y, landing.Point.z };
		return ParticleLights::DebrisSource(light, record, hash, (float)r_gpuparticles_stretch, landing.Landed ? landing.Step : 0, point);
	}
}

// SpawnParticles' question (FLevelLocals::SpawnParticles, g_levellocals.h).
bool DebrisPoolTakes(int definitionSlot)
{
	return DebrisPool::Get().Takes(definitionSlot);
}

// For `particles` (particledefs.cpp).
FString DebrisPoolReport()
{
	return DebrisPool::Get().Report();
}

DebrisPool& DebrisPool::Get()
{
	static DebrisPool pool;
	return pool;
}

bool DebrisPool::Takes(int definitionSlot) const
{
	if (definitionSlot < 0 || definitionSlot >= DEBRIS_DEFINITION_SLOTS)
		return false;
	if (screen == nullptr || !screen->IsVulkan() || !r_gpuparticles || !r_debris)
		return false;

	bool debris = ParticleDefinitionIsDebris(definitionSlot);
	if (!debris && r_debris_test)
	{
		const ParticleDefinitionGpu* table = ParticleDefinitionTableData();
		debris = table != nullptr && table[definitionSlot].look[2] > 1.5f;
	}
	if (!debris)
		return false;

	const DebrisPoolBackendStatus& status = DebrisPoolStatus();
	if (status.RefusedCapacity != 0 && status.RefusedCapacity == DebrisPoolSizeFor(r_debris_pool))
		return false;

	// The frame hook has to be running: bursts taken with nobody to read them would never be drawn.
	return mPreparedMs != 0 && I_msTime() - mPreparedMs <= 1000;
}

bool DebrisPool::WantsLevelField() const
{
	return mLevelDemand;
}

//-----------------------------------------------------------------------------
//
// The frame
//
//-----------------------------------------------------------------------------

void DebrisPool::Reset()
{
	// No slot used: the heap is empty, and pieces take slots from 0 up as they need them. The slot record itself is made
	// when the first piece goes in (PlacePending), so a map that never spawns debris holds none of it.
	mSlots.clear();
	mHeap.clear();
	mHeapPosition.clear();
	mHighWater = 0;
	mSpawnBoxValid = false;
	mBoxRefreshTime = 0.0;
	mAliveUntil = NEVER_ALIVE;
	mMeshesUntil = NEVER_ALIVE;
	mOccludersUntil = NEVER_ALIVE;
	mLitUntil = NEVER_ALIVE;
	mSoftUntil = NEVER_ALIVE;
	mLevelUntil = NEVER_ALIVE;
	mMeshInstances.clear();
	memset(mMeshFirst, 0, sizeof(mMeshFirst));
	memset(mMeshCount, 0, sizeof(mMeshCount));
	mMeshInstanceGeneration++;
	mMeshListDirty = false;
	mMeshListExpiry = 1.0e30;
	mPendingImpulses.clear();
	mNeedClear = true;
}

void DebrisPool::PrepareFrame(FLevelLocals* Level, uint64_t levelSerial)
{
	DebrisPoolFrame& out = DebrisPoolFrameForBackend();
	out = DebrisPoolFrame();
	out.Serial = ++mFrameSerial;
	mSpawns.clear();
	mImpulses.clear();
	mWakes.clear();
	if (Level == nullptr)
		return;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;
	mPreparedMs = I_msTime();

	const int maptime = Level->maptime;
	const double now = maptime / (double)TICRATE;
	// Renderer-read every frame, so they respond with a menu open. Vulkan only: GL and GLES draw no GPU particles, so the
	// pool does nothing there, and SpawnParticles writes the ring (Takes).
	const bool vulkan = screen != nullptr && screen->IsVulkan();
	const bool on = vulkan && r_gpuparticles && r_debris;
	const bool test = on && r_debris_test;
	const int wanted = DebrisPoolSizeFor(r_debris_pool);
	const DebrisPoolBackendStatus& status = DebrisPoolStatus();
	const bool refused = status.RefusedCapacity != 0 && status.RefusedCapacity == wanted;

	if (mCapacity == 0)
	{
		mCapacity = wanted;
		Reset();
	}

	if (levelSerial != mLevelSerial)
	{
		// A new map or a savegame load: the old map's pieces and bursts are gone (ClearLevelData emptied the queues).
		mLevelSerial = levelSerial;
		mClockValid = false;
		mClearSerial = Level->DebrisClearSerial;
		mBurstCursor = QueueCursor();
		mImpulseCursor = QueueCursor();
		mPending.clear();
		mFullLogged = false;
		mDropLogged = false;
		mRecycleLogged = false;
		Reset();
		mLanding.Clear();	// [DEBRISSOUNDS] the old map's landing sounds go with its pieces
		// A pool in use keeps lingering into the new map, so it is not freed now and made again at the new map's first burst.
		if (mHasDemand)
			mLastDemandTime = maptime;
	}
	if (Level->DebrisClearSerial != mClearSerial)
	{
		// ClearGpuParticles: this machine's pool empties with the ring.
		mClearSerial = Level->DebrisClearSerial;
		mPending.clear();
		Reset();
		mLanding.Clear();	// [DEBRISSOUNDS]
	}

	SyncDefinitions(test);
	mLanding.SyncDefinitions(levelSerial);	// [DEBRISSOUNDS] the definitions' landing sounds, resolved
	SyncLights();	// [PARTICLELIGHTS] each definition's light, again when the definitions load or the test is switched

	// The level's new bursts and pushes. Always read, so the cursors keep up; kept only while this machine simulates them.
	ReadQueues(Level, on && !refused);

	// [DEBRISSOUNDS] The landing sounds now due, on the draw's level clock ("Engine docs/DEBRIS_SOUNDS_11_IMPL_NOTES.md"). Only
	// while this machine simulates debris: pieces that are not drawn make no sound.
	mLanding.Update(Level, maptime, r_viewpoint.TicFrac, on && !refused);

	// Who asks for the pool: a burst waiting to go in, or pieces still alive.
	if (!mPending.empty() || mAliveUntil > now)
	{
		mHasDemand = true;
		mLastDemandTime = maptime;
	}
	if (maptime < mLastDemandTime)
		mLastDemandTime = maptime;
	if (mHasDemand && maptime - mLastDemandTime >= LINGER_SECONDS * TICRATE)
		mHasDemand = false;

	// The level field is asked for while pieces that collide with the level live or wait (hw_levelfield.cpp's demand).
	bool pendingLevel = false;
	for (const PendingPiece& piece : mPending)
	{
		if (mDefinitions[piece.Definition].Motion[2] > 1.5f)
		{
			pendingLevel = true;
			break;
		}
	}
	mLevelDemand = on && (pendingLevel || mLevelUntil > now);

	// THE SIZE. The slot record describes mCapacity; a new size is taken when nothing is alive or waiting.
	if (wanted != mCapacity && mPending.empty() && !(mAliveUntil > now))
	{
		mCapacity = wanted;
		Reset();
	}

	out.Active = on && mHasDemand && !refused;
	out.Capacity = mCapacity;
	out.RestScale = (float)r_debris_life / DEBRIS_REST_REFERENCE;
	out.SizeScale = (float)r_gpuparticles_sizescale;

	// One step per world tic, at most DEBRIS_STEPS_PER_FRAME a frame; a bigger jump drops the backlog. The clock only runs
	// while the pool is active.
	int steps = 0;
	if (!out.Active || !mClockValid || maptime < mLastStepTime)
	{
		mClockValid = true;
		mLastStepTime = maptime;
	}
	else
	{
		steps = std::min(maptime - mLastStepTime, DEBRIS_STEPS_PER_FRAME);
		mLastStepTime = maptime;
	}

	const bool ready = out.Active && status.Allocated && status.Capacity == mCapacity;
	if (!ready)
	{
		// Nothing to put pieces into (yet, or any more): the backend allocates or frees in this frame's Run, and pieces go in
		// from the frame after its status says the pool exists. Waiting bursts keep; pushes only matter to pieces, and there
		// are none on the GPU.
		mPendingImpulses.clear();
		if (!out.Active && (!mSlots.empty() || !mPending.empty()))
		{
			mPending.clear();
			Reset();
		}
		mEpoch = 0;
		mMeshInstancesSent = 0;
		if (timed && (on && mHasDemand))
			PerfLog::AddCpuSample("fx.debrispool", (double)(I_nsTime() - startNs) / 1e6);
		return;
	}

	if (status.Epoch != mEpoch)
	{
		// A new allocation: every piece in it is free (the backend cleared it), and nothing this side remembers holds.
		mEpoch = status.Epoch;
		Reset();
		mNeedClear = false;
	}

	if (mNeedClear)
	{
		out.Clear = true;
		mNeedClear = false;
	}

	out.Steps = steps;
	for (int i = 0; i < steps; i++)
		out.StepTime[i] = (float)((maptime - (steps - 1 - i)) / (double)TICRATE);

	// "Debris time on the ground" moved: every stay is keyed again.
	if (out.RestScale != mRestScale)
	{
		mRestScale = out.RestScale;
		RebuildHeap();
	}

	EnsureQuads(mCapacity);
	PlacePending(maptime, steps);
	GatherImpulses(maptime, steps);
	GatherWakes(Level, maptime, steps);
	if (mMeshListDirty || now >= mMeshListExpiry)
		BuildMeshInstances(now);
	if (now >= mBoxRefreshTime)
		RefreshSpawnBox(now);

	out.HighWater = mHighWater;
	out.Spawns = mSpawns.empty() ? nullptr : mSpawns.data();
	out.SpawnCount = (int)mSpawns.size();
	out.Impulses = mImpulses.empty() ? nullptr : mImpulses.data();
	out.ImpulseCount = (int)mImpulses.size();
	out.Wakes = mWakes.empty() ? nullptr : mWakes.data();
	out.WakeCount = (int)mWakes.size();
	for (int i = 0; i < DEBRIS_STEPS_PER_FRAME; i++)
		out.WakeAll[i] = mWakeAll[i];
	out.Definitions = mDefinitions;
	out.DefinitionGeneration = mDefinitionGeneration;
	out.MeshInstances = mMeshInstances.empty() ? nullptr : mMeshInstances.data();
	out.MeshInstanceCount = (int)mMeshInstances.size();
	out.MeshInstanceGeneration = mMeshInstanceGeneration;
	mMeshInstancesSent = mMeshInstanceGeneration;

	if (timed)
		PerfLog::AddCpuSample("fx.debrispool", (double)(I_nsTime() - startNs) / 1e6);
}

//-----------------------------------------------------------------------------
//
// The definitions
//
//-----------------------------------------------------------------------------

void DebrisPool::SyncDefinitions(bool test)
{
	const uint64_t tableGeneration = ParticleDefinitionGeneration();
	const uint64_t debrisGeneration = ParticleDebrisGeneration();
	const uint64_t meshGeneration = ParticleMeshGeneration();
	if (tableGeneration == mDefinitionTableSeen && debrisGeneration == mDebrisListSeen && meshGeneration == mMeshListSeen && (int)test == mTestSeen)
		return;
	mDefinitionTableSeen = tableGeneration;
	mDebrisListSeen = debrisGeneration;
	mMeshListSeen = meshGeneration;
	mTestSeen = (int)test;

	memset(mDefinitions, 0, sizeof(mDefinitions));
	memset(mIsMeshSlot, 0, sizeof(mIsMeshSlot));

	const ParticleDefinitionGpu* table = ParticleDefinitionTableData();
	const unsigned tableSlots = ParticleDefinitionSlotCount();

	float boundsMin[DEBRIS_DEFINITION_SLOTS][3] = {};
	float boundsMax[DEBRIS_DEFINITION_SLOTS][3] = {};
	float diameter[DEBRIS_DEFINITION_SLOTS] = {};
	const ParticleMeshDefinition* meshes = ParticleMeshDefinitionData();
	const unsigned meshCount = meshes != nullptr ? ParticleMeshDefinitionCount() : 0;
	for (unsigned i = 0; i < meshCount; i++)
	{
		const ParticleMeshDefinition& mesh = meshes[i];
		if (mesh.Slot < 0 || mesh.Slot >= DEBRIS_DEFINITION_SLOTS || !(mesh.Diameter > 0.f))
			continue;
		mIsMeshSlot[mesh.Slot] = true;
		for (int a = 0; a < 3; a++)
		{
			boundsMin[mesh.Slot][a] = mesh.BoundsMin[a];
			boundsMax[mesh.Slot][a] = mesh.BoundsMax[a];
		}
		diameter[mesh.Slot] = mesh.Diameter;
	}

	const ParticleDebrisDefinition* debris = ParticleDebrisDefinitionData();
	const unsigned debrisCount = debris != nullptr ? ParticleDebrisDefinitionCount() : 0;
	const ParticleDebrisDefinition* keys[DEBRIS_DEFINITION_SLOTS] = {};
	for (unsigned i = 0; i < debrisCount; i++)
	{
		if (debris[i].Slot >= 0 && debris[i].Slot < DEBRIS_DEFINITION_SLOTS)
			keys[debris[i].Slot] = &debris[i];
	}

	for (int slot = 0; slot < DEBRIS_DEFINITION_SLOTS; slot++)
	{
		if (table == nullptr || (unsigned)slot >= tableSlots)
			break;
		const ParticleDefinitionGpu& g = table[slot];
		const float collide = g.look[2];
		ParticleDebrisDefinition testKeys;
		const ParticleDebrisDefinition* k = keys[slot];
		if (k == nullptr && test && collide > 1.5f)
		{
			testKeys.Slot = slot;
			testKeys.Restitution = kTestRestitution;
			testKeys.Friction = kTestFriction;
			testKeys.RestLife = mIsMeshSlot[slot] ? kTestMeshRest : 0.f;
			testKeys.RestFade = kTestRestFade;
			k = &testKeys;
		}
		if (k == nullptr)
			continue;

		DebrisDefinitionGpu& d = mDefinitions[slot];
		d.Physics[0] = std::clamp(k->Restitution, 0.f, 1.f);
		d.Physics[1] = std::clamp(k->Friction, 0.f, 1.f);
		d.Physics[2] = std::max(k->RestLife, 0.f);
		d.Physics[3] = std::max(k->RestFade, 0.f);
		d.Motion[0] = g.motion[0];
		d.Motion[1] = g.motion[1];
		d.Motion[2] = collide > 1.5f ? 2.f : 1.f;
		d.Motion[3] = 1.f;
		d.Body[0] = g.key[0][1];
		d.Body[1] = mIsMeshSlot[slot] ? 1.f : 0.f;
		d.Body[2] = diameter[slot];
		for (int a = 0; a < 3; a++)
		{
			d.BoundsMin[a] = boundsMin[slot][a];
			d.BoundsMax[a] = boundsMax[slot][a];
		}
	}
	mDefinitionGeneration++;
}

// [PARTICLELIGHTS] Each named definition's light (hw_particlelights.h), again whenever the definitions load or r_particlelights_test
// is switched: ExpandBurst reads it for every burst.
void DebrisPool::SyncLights()
{
	const uint64_t generation = ParticleLightGeneration();
	const int test = r_particlelights_test ? 1 : 0;
	if (generation == mLightListSeen && test == mLightTestSeen)
		return;
	mLightListSeen = generation;
	mLightTestSeen = test;
	mAnyLight = ParticleLights::ResolveSlots(ParticleLightDefinitionData(), ParticleLightDefinitionCount(), ParticleDefinitionTableData(),
		DEBRIS_DEFINITION_SLOTS, test != 0, mSlotLights);
}

//-----------------------------------------------------------------------------
//
// Bursts and pushes in
//
//-----------------------------------------------------------------------------

void DebrisPool::ReadQueues(FLevelLocals* Level, bool keep)
{
	ReadQueue(Level->DebrisBursts, mBurstCursor.Serial, mBurstCursor.Count, [&](const FDebrisBurstEvent& burst, int tic)
	{
		if (keep)
			ExpandBurst(Level, burst, tic);
	});
	ReadQueue(Level->EffectImpulses, mImpulseCursor.Serial, mImpulseCursor.Count, [&](const FEffectImpulseEvent& q, int tic)
	{
		if (!keep || mPendingImpulses.size() >= (size_t)(2 * DEBRIS_IMPULSES_PER_FRAME))
			return;
		PendingImpulse p;
		// Game (x, y, z) -> shader (x, z, y). The native clamps these (vmthunks.cpp).
		p.Impulse.Sphere[0] = (float)q.Pos.X;
		p.Impulse.Sphere[1] = (float)q.Pos.Z;
		p.Impulse.Sphere[2] = (float)q.Pos.Y;
		p.Impulse.Sphere[3] = (float)std::clamp(q.Radius, 1.0, 4096.0);
		p.Impulse.Push[0] = (float)std::clamp(q.Strength, -4096.0, 4096.0);
		p.Tic = tic;
		mPendingImpulses.push_back(p);
	});

	// A full generation means SpawnParticles wrote some of that tic's bursts to the ring instead: say so once per map.
	if (keep && !mFullLogged)
	{
		for (int g = 0; g < 2; g++)
		{
			if (Level->DebrisBursts.Serial[g] != 0 && Level->DebrisBursts.Count[g] >= FLevelLocals::MAX_DEBRIS_BURSTS_PER_TIC)
			{
				mFullLogged = true;
				Printf(TEXTCOLOR_ORANGE "DebrisPool: more than %d debris bursts in one tic -- the rest were drawn as ring particles (once per map)\n",
					FLevelLocals::MAX_DEBRIS_BURSTS_PER_TIC);
				break;
			}
		}
	}
}

// One burst into pieces, with SpawnParticles' own maths (g_levellocals.h): the same cone or disc, jitters, plane and
// floor, so a piece starts exactly where that burst's ring record would. Its turn and spin start as meshparticles.vp's
// tumble (a mesh) or as gpuparticles.vp's spin (a billboard).
void DebrisPool::ExpandBurst(FLevelLocals* Level, const FDebrisBurstEvent& burst, int tic)
{
	const int defSlot = burst.DefinitionSlot;
	if (defSlot < 0 || defSlot >= DEBRIS_DEFINITION_SLOTS || mDefinitions[defSlot].Motion[3] < 0.5f)
		return;	// not debris on this table any more
	if (!(burst.Life > 0.0) || burst.Count <= 0)
		return;

	int count = std::min(burst.Count, DEBRIS_POOL_MAX);
	const size_t room = (size_t)MAX_PENDING_PIECES > mPending.size() ? (size_t)MAX_PENDING_PIECES - mPending.size() : 0;
	if ((size_t)count > room)
	{
		mPiecesDropped += (uint64_t)count - room;
		count = (int)room;
		if (!mDropLogged)
		{
			mDropLogged = true;
			Printf(TEXTCOLOR_ORANGE "DebrisPool: more than %d pieces waiting to go in -- the newest are dropped (once per map)\n", MAX_PENDING_PIECES);
		}
	}
	if (count <= 0)
		return;

	const ParticleDefinitionGpu* table = ParticleDefinitionTableData();
	if (table == nullptr)
		return;
	const ParticleDefinitionGpu& g = table[defSlot];
	const bool mesh = mIsMeshSlot[defSlot];
	const int orient = (int)(g.shape[0] + 0.5f);

	const uint32_t s = burst.Seed;
	const FLevelLocals::GpuParticleBasis basis = FLevelLocals::GpuParticleMakeBasis(burst.Dir);
	const bool disc = burst.Shape == 1;
	const double cosMax = cos(std::clamp(burst.Spread, 0.0, 180.0) * kPi / 180.0);
	const double liftMax = std::clamp(burst.Spread, 0.0, 90.0) * kPi / 180.0;
	const float birth = (float)(tic / (double)TICRATE);

	// The plane in SHADER space as SpawnParticles encodes it.
	float planeX = FLevelLocals::GPUPARTICLE_NO_PLANE, planeY = 0.f, planeOffset = 0.f;
	const double normalLength = burst.SurfaceNormal.Length();
	if (normalLength > 1e-6)
	{
		const double nx = burst.SurfaceNormal.X / normalLength;
		const double ny = burst.SurfaceNormal.Z / normalLength;
		const double nz = burst.SurfaceNormal.Y / normalLength;
		const double sum = fabs(nx) + fabs(ny) + fabs(nz);
		double ox = nx / sum, oy = ny / sum;
		if (nz < 0.0)
		{
			const double fx = (1.0 - fabs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
			const double fy = (1.0 - fabs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
			ox = fx;
			oy = fy;
		}
		planeX = (float)ox;
		planeY = (float)oy;
		planeOffset = (float)(nx * burst.SurfacePoint.X + ny * burst.SurfacePoint.Z + nz * burst.SurfacePoint.Y);
	}
	const float floorHeight = burst.FloorZ > -32768.0 ? (float)burst.FloorZ : FLevelLocals::GPUPARTICLE_NO_FLOOR;
	const PalEntry tint = (PalEntry)burst.Tint;

	// [DEBRISSOUNDS] A definition with a landing sound: each piece's flight goes to the group's landing prediction.
	const bool landing = mLanding.BeginBurst(Level, burst, tic, mDefinitions[defSlot], (float)r_gpuparticles_sizescale);

	// [PARTICLELIGHTS] A definition that throws light ("Engine docs/EFFECT_LIGHTS_LC_IMPL_NOTES.md"): each piece that carries one -- a
	// hash of its record picks the burst's lightshare, at most lightmax of them -- hands EffectLights a light that flies with it and
	// lands where #11 predicts its first landing (PieceLight). Where the pieces collide with the level, a burst's first
	// DebrisLanding::kMaxTraced lights are traced through it and the rest predicted against its plane and floor. Vulkan's effect
	// lights only, while they are on.
	const ParticleLights::SlotLight* light = mAnyLight && r_effectlights && EffectLightBuffer::Instance() != nullptr && mSlotLights[defSlot].Active ?
		&mSlotLights[defSlot] : nullptr;
	DebrisLanding::Flight lightFlight;
	const bool lightLevel = light != nullptr && LightFlight(Level, burst, mDefinitions[defSlot], (float)r_gpuparticles_sizescale, lightFlight);
	const bool lightNeverRests = !(mDefinitions[defSlot].Physics[2] > 0.f);
	int lights = 0;

	mPending.reserve(mPending.size() + (size_t)count);
	for (int i = 0; i < count; i++)
	{
		const double u1 = FLevelLocals::GpuParticleRand(s, (uint32_t)i, 0);
		const double u2 = FLevelLocals::GpuParticleRand(s, (uint32_t)i, 1);
		const double u3 = FLevelLocals::GpuParticleRand(s, (uint32_t)i, 2);
		const double u4 = FLevelLocals::GpuParticleRand(s, (uint32_t)i, 3);
		const double u5 = FLevelLocals::GpuParticleRand(s, (uint32_t)i, 4);

		const DVector3 v = disc ? FLevelLocals::GpuParticleDiscDirection(basis, liftMax, u1, u2) : FLevelLocals::GpuParticleConeDirection(basis, cosMax, u1, u2);
		const double spd = burst.Speed * (1.0 + burst.SpeedJitter * (2.0 * u3 - 1.0));
		double lf = burst.Life * (1.0 + burst.LifeJitter * (2.0 * u4 - 1.0));
		if (lf < 1e-3) lf = 1e-3;

		PendingPiece p;
		memset(&p.Piece, 0, sizeof(p.Piece));
		float* a = p.Piece.Spawn[0];
		float* b = p.Piece.Spawn[1];
		float* c = p.Piece.Spawn[2];
		float* d = p.Piece.Spawn[3];
		float* e = p.Piece.Spawn[4];
		// Game (x, y, z) -> shader (x, z, y): y is up in shader space. SpawnParticles' record, field for field.
		a[0] = (float)burst.Pos.X;    a[1] = (float)burst.Pos.Z;    a[2] = (float)burst.Pos.Y;    a[3] = birth;
		b[0] = (float)(v.X * spd);    b[1] = (float)(v.Z * spd);    b[2] = (float)(v.Y * spd);    b[3] = (float)lf;
		c[0] = tint.r / 255.f;        c[1] = tint.g / 255.f;        c[2] = tint.b / 255.f;        c[3] = (float)burst.Intensity;
		d[0] = (float)defSlot;        d[1] = (float)burst.SizeScale; d[2] = burst.Ambient;         d[3] = (float)u5;
		e[0] = planeX;                e[1] = planeY;                e[2] = planeOffset;           e[3] = floorHeight;

		// [DEBRISSOUNDS] Its flight, for the group's landing sound.
		if (landing)
			mLanding.AddPiece(a, b);

		// [PARTICLELIGHTS] Its light, when it carries one.
		if (light != nullptr && lights < light->Max)
		{
			float record[ParticleLights::RECORD_FLOATS];
			memcpy(record, p.Piece.Spawn, sizeof(record));
			const uint32_t hash = ParticleLights::RecordHash(record);
			if (ParticleLights::CarriesLight(hash, light->Share))
			{
				lights++;
				EffectLights::Get().Spawn(PieceLight(Level, *light, lightFlight, lightLevel && lights <= DebrisLanding::kMaxTraced, lightNeverRests, record, hash));
				mLightsSpawned++;
			}
		}

		// The state before its first step: at its spawn point, flying at its launch velocity, its rest clock not started.
		for (int k = 0; k < 3; k++)
		{
			p.Piece.Position[k] = a[k];
			p.Piece.Previous[k] = a[k];
			p.Piece.Velocity[k] = b[k];
		}
		p.Piece.Position[3] = DEBRIS_STATE_FLYING;
		p.Piece.Previous[3] = birth;
		p.Piece.Velocity[3] = -1.f;

		if (mesh)
		{
			// meshparticles.vp's tumble from the seed: a uniformly random turn (Shoemake) and axis, the rate between the
			// definition's spin min and max. From the float the record holds, as the shader reads it.
			const uint32_t seedBits = (uint32_t)(std::clamp(d[3], 0.f, 1.f) * 16777215.0f);
			const float r1 = MeshRandom(seedBits, 0);
			const float r2 = MeshRandom(seedBits, 1);
			const float r3 = MeshRandom(seedBits, 2);
			const float twoPi = 6.28318530718f;
			p.Piece.Turn[0] = std::sqrt(1.f - r1) * std::sin(twoPi * r2);
			p.Piece.Turn[1] = std::sqrt(1.f - r1) * std::cos(twoPi * r2);
			p.Piece.Turn[2] = std::sqrt(r1) * std::sin(twoPi * r3);
			p.Piece.Turn[3] = std::sqrt(r1) * std::cos(twoPi * r3);
			const float axisZ = MeshRandom(seedBits, 3) * 2.f - 1.f;
			const float axisAngle = twoPi * MeshRandom(seedBits, 4);
			const float axisRadius = std::sqrt(std::max(1.f - axisZ * axisZ, 0.f));
			const float rate = (float)((g.shape[2] + (g.shape[3] - g.shape[2]) * MeshRandom(seedBits, 5)) * kPi / 180.0);
			p.Piece.Spin[0] = axisRadius * std::cos(axisAngle) * rate;
			p.Piece.Spin[1] = axisRadius * std::sin(axisAngle) * rate;
			p.Piece.Spin[2] = axisZ * rate;
		}
		else
		{
			// A billboard turns about the view axis only: gpuparticles.vp's spin rate, picked by the seed, kept as a turn
			// about shader y (a streak does not spin).
			p.Piece.Turn[3] = 1.f;
			if (orient != 1)
				p.Piece.Spin[1] = (float)((g.shape[2] + (g.shape[3] - g.shape[2]) * d[3]) * kPi / 180.0);
		}
		memcpy(p.Piece.PreviousTurn, p.Piece.Turn, sizeof(p.Piece.Turn));

		p.Tic = tic;
		p.Definition = defSlot;
		mPending.push_back(p);
	}

	// [DEBRISSOUNDS] The group's first landing, predicted from the pieces that went in, and its sound queued.
	if (landing)
		mLanding.EndBurst(count);
	// [PARTICLELIGHTS]
	if (lights > 0 && !mLightLogged)
	{
		mLightLogged = true;
		Printf("DebrisPool: first debris lights this run -- %d from one burst of particle definition slot %d (definitions with `light`; hw_particlelights.h)\n", lights, defSlot);
	}
}

//-----------------------------------------------------------------------------
//
// Slots
//
//-----------------------------------------------------------------------------

double DebrisPool::StayEnd(const SlotState& slot) const
{
	return slot.FixedEnd + (double)slot.FixedRest + (double)slot.ScaledRest * (double)mRestScale;
}

bool DebrisPool::HeapLess(uint32_t a, uint32_t b) const
{
	const double sa = mSlots[a].Stay;
	const double sb = mSlots[b].Stay;
	return sa < sb || (sa == sb && a < b);
}

void DebrisPool::SiftDown(size_t position)
{
	const size_t count = mHeap.size();
	for (;;)
	{
		const size_t left = 2 * position + 1;
		const size_t right = left + 1;
		size_t least = position;
		if (left < count && HeapLess(mHeap[left], mHeap[least]))
			least = left;
		if (right < count && HeapLess(mHeap[right], mHeap[least]))
			least = right;
		if (least == position)
			return;
		std::swap(mHeap[position], mHeap[least]);
		mHeapPosition[mHeap[position]] = (uint32_t)position;
		mHeapPosition[mHeap[least]] = (uint32_t)least;
		position = least;
	}
}

void DebrisPool::SiftUp(size_t position)
{
	while (position > 0)
	{
		const size_t parent = (position - 1) / 2;
		if (!HeapLess(mHeap[position], mHeap[parent]))
			return;
		std::swap(mHeap[position], mHeap[parent]);
		mHeapPosition[mHeap[position]] = (uint32_t)position;
		mHeapPosition[mHeap[parent]] = (uint32_t)parent;
		position = parent;
	}
}

void DebrisPool::RebuildHeap()
{
	for (uint32_t slot : mHeap)
		mSlots[slot].Stay = StayEnd(mSlots[slot]);
	const size_t count = mHeap.size();
	for (size_t i = 0; i < count; i++)
		mHeapPosition[mHeap[i]] = (uint32_t)i;
	for (size_t i = count / 2; i-- > 0;)
		SiftDown(i);
	mMeshListDirty = true;
	mBoxRefreshTime = 0.0;
}

// Waiting pieces into slots, up to a frame's worth: the heap's top when its stay has ended, else a never-used slot, else
// (every slot in use) the heap's top -- the piece with the least time left. A piece goes in before the step of the tic
// it was spawned in (the smoke volume's rule), or after this frame's steps when there are none.
void DebrisPool::PlacePending(int maptime, int steps)
{
	if (mPending.empty() || mCapacity <= 0)
		return;
	if (mSlots.size() != (size_t)mCapacity)
	{
		// The first pieces since the last Reset: the slot record at this capacity.
		mSlots.assign((size_t)mCapacity, SlotState());
		mHeap.clear();
		mHeap.reserve((size_t)mCapacity);
		mHeapPosition.assign((size_t)mCapacity, 0);
		mHighWater = 0;
	}

	const double now = maptime / (double)TICRATE;
	const int firstStepTime = maptime - steps + 1;
	const uint8_t* looks = screen != nullptr && screen->mParticleDefinitions != nullptr ? screen->mParticleDefinitions->GetSlotLooks() : nullptr;
	const ParticleDefinitionGpu* table = ParticleDefinitionTableData();

	const size_t take = std::min(mPending.size(), (size_t)DEBRIS_SPAWNS_PER_FRAME);
	mSpawns.reserve(take);
	for (size_t i = 0; i < take; i++)
	{
		const PendingPiece& piece = mPending[i];
		const bool reuseTop = !mHeap.empty() && (mSlots[mHeap[0]].Stay <= now || mHighWater >= mCapacity);
		const uint32_t slot = reuseTop ? mHeap[0] : (uint32_t)mHighWater;
		SlotState& state = mSlots[slot];
		if (reuseTop && state.Stay > now)
		{
			// The pool is full: the piece with the least time left gives way.
			mPiecesRecycled++;
			if (!mRecycleLogged)
			{
				mRecycleLogged = true;
				Printf("DebrisPool: the %d-piece pool is full -- pieces with the least time left are recycled (\"Debris pieces\" holds more; once per map)\n", mCapacity);
			}
		}

		const DebrisDefinitionGpu& definition = mDefinitions[piece.Definition];
		const float birth = piece.Piece.Spawn[0][3];
		const float life = piece.Piece.Spawn[1][3];
		const float rest = definition.Physics[2];
		const float fade = definition.Physics[3];
		state.FixedEnd = (double)birth + (double)life + (rest > 0.f ? (double)fade : 0.0) + STAY_MARGIN_SECONDS;
		state.ScaledRest = rest >= DEBRIS_REST_SCALE_FROM ? rest : 0.f;
		state.FixedRest = rest > 0.f && rest < DEBRIS_REST_SCALE_FROM ? rest : 0.f;
		state.Definition = piece.Definition;
		state.Mesh = definition.Body[1] > 0.5f;
		state.SpawnX = piece.Piece.Spawn[0][0];
		state.SpawnY = piece.Piece.Spawn[0][2];
		state.Stay = StayEnd(state);
		if (reuseTop)
		{
			SiftDown(0);
		}
		else
		{
			mHighWater++;
			mHeap.push_back(slot);
			mHeapPosition[slot] = (uint32_t)(mHeap.size() - 1);
			SiftUp(mHeap.size() - 1);
		}

		DebrisSpawnUpload upload;
		upload.Slot = slot;
		upload.Step = steps > 0 ? std::clamp(piece.Tic + 1 - firstStepTime, 0, steps - 1) : -1;
		upload.Piece = piece.Piece;
		mSpawns.push_back(upload);

		// What the draw needs while it lives.
		const float until = (float)state.Stay;
		mAliveUntil = std::max(mAliveUntil, until);
		const uint8_t look = looks != nullptr ? looks[piece.Definition] : 0;
		if (look & ParticleDefinitionBuffer::LOOK_OCCLUDES) mOccludersUntil = std::max(mOccludersUntil, until);
		if (look & ParticleDefinitionBuffer::LOOK_SOFT) mSoftUntil = std::max(mSoftUntil, until);
		const bool litMesh = state.Mesh && table != nullptr && table[piece.Definition].look[0] > 0.f;
		if ((look & ParticleDefinitionBuffer::LOOK_LIT) || litMesh) mLitUntil = std::max(mLitUntil, until);
		if (state.Mesh)
		{
			mMeshesUntil = std::max(mMeshesUntil, until);
			mMeshListDirty = true;
		}
		if (definition.Motion[2] > 1.5f) mLevelUntil = std::max(mLevelUntil, until);

		const double x = state.SpawnX;
		const double y = state.SpawnY;
		if (!mSpawnBoxValid)
		{
			mSpawnMin[0] = mSpawnMax[0] = x;
			mSpawnMin[1] = mSpawnMax[1] = y;
			mSpawnBoxValid = true;
		}
		else
		{
			mSpawnMin[0] = std::min(mSpawnMin[0], x);
			mSpawnMax[0] = std::max(mSpawnMax[0], x);
			mSpawnMin[1] = std::min(mSpawnMin[1], y);
			mSpawnMax[1] = std::max(mSpawnMax[1], y);
		}
		mPiecesSpawned++;
	}
	mPending.erase(mPending.begin(), mPending.begin() + (ptrdiff_t)take);
}

// The pushes, each with the step of its tic. With no step this frame they wait for the next.
void DebrisPool::GatherImpulses(int maptime, int steps)
{
	if (steps <= 0)
		return;
	const int firstStepTime = maptime - steps + 1;
	for (const PendingImpulse& pending : mPendingImpulses)
	{
		if ((int)mImpulses.size() >= DEBRIS_IMPULSES_PER_FRAME)
			break;
		DebrisImpulseGpu impulse = pending.Impulse;
		impulse.Push[1] = (float)std::clamp(pending.Tic + 1 - firstStepTime, 0, steps - 1);
		mImpulses.push_back(impulse);
	}
	mPendingImpulses.clear();
}

// SH2: the sectors under the pieces' area that moved this frame -- a door, a lift -- wake what rests over them, at this
// frame's first step. More than a frame's wake boxes wake every resting piece instead.
void DebrisPool::GatherWakes(FLevelLocals* Level, int maptime, int steps)
{
	mWakeAll[0] = mWakeAll[1] = false;
	if (steps <= 0 || !mSpawnBoxValid || !(mAliveUntil > maptime / (double)TICRATE))
		return;

	SectorPlanes& planes = SectorPlanes::Get();
	planes.PollBox(Level, mSpawnMin[0] - POLL_MARGIN, mSpawnMin[1] - POLL_MARGIN, mSpawnMax[0] + POLL_MARGIN, mSpawnMax[1] + POLL_MARGIN);
	for (int sector : planes.ChangedThisFrame())
	{
		double minX, minY, maxX, maxY;
		if (!planes.GetExtent(sector, minX, minY, maxX, maxY))
			continue;
		if ((int)mWakes.size() >= DEBRIS_WAKES_PER_FRAME)
		{
			mWakeAll[0] = true;
			mWakes.clear();
			break;
		}
		// Shader axes: x, then height (every height), then map y. A margin of a few units catches a piece on the edge.
		DebrisWakeGpu wake;
		wake.Min[0] = (float)(minX - 4.0);
		wake.Min[1] = -1.0e9f;
		wake.Min[2] = (float)(minY - 4.0);
		wake.Min[3] = 0.f;
		wake.Max[0] = (float)(maxX + 4.0);
		wake.Max[1] = 1.0e9f;
		wake.Max[2] = (float)(maxY + 4.0);
		wake.Max[3] = 0.f;
		mWakes.push_back(wake);
	}
}

// The live pieces' spawn area, once a second: pieces die, so the SH2 poll box shrinks back.
void DebrisPool::RefreshSpawnBox(double now)
{
	mBoxRefreshTime = now + 1.0;
	mSpawnBoxValid = false;
	for (int slot = 0; slot < mHighWater; slot++)
	{
		const SlotState& state = mSlots[(size_t)slot];
		if (!(state.Stay > now))
			continue;
		const double x = state.SpawnX;
		const double y = state.SpawnY;
		if (!mSpawnBoxValid)
		{
			mSpawnMin[0] = mSpawnMax[0] = x;
			mSpawnMin[1] = mSpawnMax[1] = y;
			mSpawnBoxValid = true;
		}
		else
		{
			mSpawnMin[0] = std::min(mSpawnMin[0], x);
			mSpawnMax[0] = std::max(mSpawnMax[0], x);
			mSpawnMin[1] = std::min(mSpawnMin[1], y);
			mSpawnMax[1] = std::max(mSpawnMax[1], y);
		}
	}
}

// The pool mesh instance list: each mesh definition's live slots in turn. Rebuilt when a mesh piece went in, when the
// rest scale moved, or when a listed piece's stay has ended.
void DebrisPool::BuildMeshInstances(double now)
{
	memset(mMeshCount, 0, sizeof(mMeshCount));
	for (int slot = 0; slot < mHighWater; slot++)
	{
		const SlotState& state = mSlots[(size_t)slot];
		if (state.Mesh && state.Stay > now && state.Definition >= 0 && state.Definition < DEBRIS_DEFINITION_SLOTS)
			mMeshCount[state.Definition]++;
	}
	int total = 0;
	for (int d = 0; d < DEBRIS_DEFINITION_SLOTS; d++)
	{
		mMeshFirst[d] = total;
		total += mMeshCount[d];
	}
	mMeshInstances.assign((size_t)total, 0);
	int filled[DEBRIS_DEFINITION_SLOTS] = {};
	double soonest = 1.0e30;
	for (int slot = 0; slot < mHighWater; slot++)
	{
		const SlotState& state = mSlots[(size_t)slot];
		if (!(state.Mesh && state.Stay > now && state.Definition >= 0 && state.Definition < DEBRIS_DEFINITION_SLOTS))
			continue;
		mMeshInstances[(size_t)(mMeshFirst[state.Definition] + filled[state.Definition]++)] = (uint32_t)slot;
		soonest = std::min(soonest, state.Stay);
	}
	mMeshListExpiry = soonest;
	mMeshListDirty = false;
	mMeshInstanceGeneration++;
}

//-----------------------------------------------------------------------------
//
// The draws
//
//-----------------------------------------------------------------------------

// Six vertices per pool slot, in the ring's own format (hw_gpuparticlebuffer.cpp): the slot index split into two 16-bit
// halves, the corner, and w = 1, which is how gpuparticles.vp tells a pool vertex from a ring vertex (whose w is 0).
void DebrisPool::EnsureQuads(int capacity)
{
	if (mQuads != nullptr && mQuadCapacity == capacity)
		return;
	if (screen == nullptr)
		return;

	delete mQuads;	// a hardware buffer puts its storage on the backend's delete list
	mQuads = nullptr;
	mQuadCapacity = 0;

	struct QuadVertex
	{
		uint16_t indexLow;
		uint16_t indexHigh;
		uint16_t corner;
		uint16_t pool;
	};
	const size_t vertexCount = (size_t)capacity * GpuParticleBuffer::VERTICES_PER_RECORD;
	std::vector<QuadVertex> verts(vertexCount);
	for (int i = 0; i < capacity; i++)
	{
		for (unsigned c = 0; c < GpuParticleBuffer::VERTICES_PER_RECORD; c++)
		{
			QuadVertex& v = verts[(size_t)i * GpuParticleBuffer::VERTICES_PER_RECORD + c];
			v.indexLow = (uint16_t)(i & 0xffff);
			v.indexHigh = (uint16_t)((i >> 16) & 0xffff);
			v.corner = (uint16_t)c;
			v.pool = 1;
		}
	}

	mQuads = screen->CreateVertexBuffer();
	if (mQuads == nullptr)
		return;
	static const FVertexBufferAttribute format[] = {
		{ 0, VATTR_BONESELECTOR, VFmt_UShort4_UInt, 0 },
	};
	mQuads->SetFormat(1, 1, sizeof(QuadVertex), format);
	mQuads->SetData(vertexCount * sizeof(QuadVertex), verts.data(), BufferUsageType::Static);
	mQuadCapacity = capacity;
}

bool DebrisPool::HasBillboardsAt(float levelTime) const
{
	return mAliveUntil > levelTime && mHighWater > 0 && mQuads != nullptr && mQuadCapacity >= mHighWater &&
		mEpoch != 0 && DebrisPoolStatus().Bound && DebrisPoolStatus().Epoch == mEpoch;
}

void DebrisPool::DrawBillboards(FRenderState& state, float levelTime)
{
	if (!HasBillboardsAt(levelTime))
		return;
	state.SetVertexBuffer(mQuads, 0, 0);
	state.Draw(DT_Triangles, 0, mHighWater * (int)GpuParticleBuffer::VERTICES_PER_RECORD);
}

bool DebrisPool::HasMeshesAt(float levelTime) const
{
	const DebrisPoolBackendStatus& status = DebrisPoolStatus();
	const MeshParticleBuffer* meshes = screen != nullptr ? screen->mMeshParticles : nullptr;
	return mMeshesUntil > levelTime && !mMeshInstances.empty() && mEpoch != 0 && status.Bound && status.Epoch == mEpoch &&
		status.MeshInstanceGeneration == mMeshInstanceGeneration && meshes != nullptr && meshes->ShaderReady && !meshes->ShaderFailed &&
		screen->mGpuParticles != nullptr && screen->mViewLights != nullptr;
}

// The model a mesh definition draws with, as MeshParticleBuffer resolved it: an index trusted only while the model there
// still has the mesh's file name, else looked up again. Only asked for a definition MeshParticleBuffer marks as drawn as a
// mesh this frame, so the model is loaded and its vertex buffer built.
bool DebrisPool::EnsureDrawModel(int meshIndex)
{
	const ParticleMeshDefinition* meshes = ParticleMeshDefinitionData();
	if (meshes == nullptr || meshIndex < 0 || (unsigned)meshIndex >= ParticleMeshDefinitionCount())
		return false;
	const ParticleMeshDefinition& mesh = meshes[meshIndex];
	int& index = mDrawModelIndex[mesh.Slot];
	if (!(index >= 0 && (unsigned)index < Models.Size() && Models[index] != nullptr && Models[index]->mFileName.CompareNoCase(mesh.Path) == 0))
	{
		const unsigned found = FindModel(nullptr, mesh.Path.GetChars(), true);
		if (found >= Models.Size() || Models[found] == nullptr || Models[found]->GetSurfaceCount() != 1)
		{
			index = -1;
			return false;
		}
		index = (int)found;
	}
	const auto vbuf = static_cast<FModelVertexBuffer*>(Models[index]->GetVertexBuffer(GLModelRendererType));
	return vbuf != nullptr && vbuf->vertexBuffer() != nullptr && vbuf->indexBuffer() != nullptr;
}

void DebrisPool::DrawMeshes(FRenderState& state, float levelTime)
{
	if (!HasMeshesAt(levelTime))
		return;
	const uint8_t* hidden = screen->mMeshParticles->GetBillboardHidden();
	const ParticleMeshDefinition* meshes = ParticleMeshDefinitionData();
	const unsigned meshCount = meshes != nullptr ? ParticleMeshDefinitionCount() : 0;
	if (hidden == nullptr || meshCount == 0)
		return;

	// MeshParticleBuffer::Draw's state: opaque, writing depth, no culling.
	state.SetEffect(EFF_MESHPARTICLES);
	state.SetRenderStyle(STYLE_Source);
	state.SetDepthMask(true);
	state.SetDepthFunc(DF_Less);
	state.SetCulling(Cull_None);
	state.EnableTexture(true);
	state.EnableModelMatrix(false);
	state.SetBoneIndexBase(-1);
	state.SetLightIndex(-1);

	for (unsigned i = 0; i < meshCount; i++)
	{
		const ParticleMeshDefinition& mesh = meshes[i];
		if (mesh.Slot < 0 || mesh.Slot >= DEBRIS_DEFINITION_SLOTS || mMeshCount[mesh.Slot] == 0)
			continue;
		// Only a definition drawn as a mesh this frame: its billboard (the pool's too) steps aside, so a piece is never
		// drawn both ways, and while it draws as a billboard nothing is drawn here.
		if (!hidden[mesh.Slot] || !EnsureDrawModel((int)i))
			continue;
		FModel* model = Models[mDrawModelIndex[mesh.Slot]];
		const auto vbuf = static_cast<FModelVertexBuffer*>(model->GetVertexBuffer(GLModelRendererType));
		FGameTexture* skin = TexMan.GetGameTexture(mesh.Skin, true);
		if (vbuf == nullptr || skin == nullptr)
			continue;

		state.SetMaterial(skin, UF_Skin, 0, CLAMP_NONE, 0, -1);
		const int frameStart = mesh.Frame * (int)mesh.Vertices;
		state.SetVertexBuffer(vbuf->vertexBuffer(), frameStart, frameStart);
		state.SetIndexBuffer(vbuf->indexBuffer());
		state.DrawIndexedInstanced(DT_Triangles, 0, (int)mesh.Triangles * 3, mMeshCount[mesh.Slot], DEBRIS_MESH_INSTANCE_BASE + mMeshFirst[mesh.Slot]);
	}

	state.SetEffect(EFF_NONE);
	state.SetVertexBuffer(screen->mVertexData);
}

FString DebrisPool::Report() const
{
	const DebrisPoolBackendStatus& status = DebrisPoolStatus();
	const bool vulkan = screen != nullptr && screen->IsVulkan();
	const int used = mHighWater;
	FString text;
	text.Format("Debris pool: r_debris %d, r_debris_life %g s, r_debris_pool %d, r_debris_test %d -- %s; capacity %d (%.2f MB of pieces on the GPU); "
		"%d slots used since the last clear, %llu pieces spawned, %llu recycled while still showing, %llu dropped, %d mesh instances",
		(int)*r_debris, (double)(float)*r_debris_life, (int)*r_debris_pool, (int)*r_debris_test,
		!vulkan ? "not on this renderer (Vulkan only; debris definitions draw as ring particles)" :
		status.RefusedCapacity != 0 ? "REFUSED by this device (debris definitions draw as ring particles)" :
		status.Allocated ? "allocated" : "not allocated (nothing asked for it yet)",
		status.Allocated ? status.Capacity : mCapacity, (status.Allocated ? status.Capacity : 0) * (double)DEBRIS_PIECE_BYTES / (1024.0 * 1024.0),
		used, (unsigned long long)mPiecesSpawned, (unsigned long long)mPiecesRecycled, (unsigned long long)mPiecesDropped, (int)mMeshInstances.size());
	// [DEBRISSOUNDS] And the landing sounds.
	text.AppendFormat("\n%s", mLanding.Report().GetChars());
	// [PARTICLELIGHTS] And the pieces' lights.
	text.AppendFormat("\nDebris lights: %llu spawned since the start; %s", (unsigned long long)mLightsSpawned,
		mAnyLight ? "a definition throws light (its light keys, or r_particlelights_test)" : "no definition throws light");
	return text;
}
