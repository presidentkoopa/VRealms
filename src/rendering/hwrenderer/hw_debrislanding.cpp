/*
** hw_debrislanding.cpp
**
** [DEBRISSOUNDS] Debris landing sounds: the engine side. See the header.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** The prediction, the level trace, the merge and the limits are mirrored in Python, and the header's namespace is run in
** a C++ harness on the mirror's inputs ("Engine docs/DEBRIS_SOUNDS_11_IMPL_NOTES.md", Checks).
**
*/

#include <algorithm>
#include <cmath>

#include "hw_debrislanding.h"
#include "hw_debrisframe.h"
#include "hw_perflog.h"
#include "particledefs.h"
#include "g_levellocals.h"
#include "r_defs.h"
#include "s_sound.h"
#include "m_random.h"
#include "c_cvars.h"
#include "doomdef.h"
#include "i_time.h"
#include "printf.h"

// [DEBRISSOUNDS] r_debris_sounds -- "Debris landing sounds" ("Engine docs/DEBRIS_SOUNDS_11_IMPL_NOTES.md"). ON BY DEFAULT
// (owner, 2026-09-14: effects our mods use default ON); inert until a debris definition that names a `landsound` goes into
// the debris pool, so a mod without one costs nothing. Renderer-read every frame. Presentation only -- not SERVERINFO: each
// machine hears its own.
CVARD(Bool, r_debris_sounds, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "debris landing sounds: a group of debris pieces whose definition names a landsound makes it once where it first lands (Vulkan only, with the debris pool)")
// Their loudness, on top of each definition's landvolume and the sound volume. Read when a sound starts.
CUSTOM_CVARD(Float, r_debris_sounds_volume, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "debris landing sound volume, 0..1, on top of each definition's landvolume (Vulkan only)")
{
	if (self < 0.f) { self = 0.f; return; }
	if (self > 1.f) { self = 1.f; return; }
}

EXTERN_CVAR(Bool, r_particlecollision)	// [LEVELFIELD] hw_levelfield.cpp: while off, the step collides with the spawn's plane only

// The pitch each landing picks: a CLIENT-side RNG. FCRandom links itself into the client list, which savegames never store
// and nothing compares between machines (m_random.cpp), so it can differ everywhere without touching the game.
static FCRandom pr_debrisland("DebrisLanding");

namespace
{
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
					continue;	// this sector on both sides: nothing to stop at
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

	// One group's sound: the big variant past its count, the volume by its size, a pitch from the client RNG, at the point
	// (no actor). A silent sector stays silent.
	void PlayLanding(FLevelLocals* Level, const DebrisLanding::Event& e)
	{
		const bool big = DebrisLanding::PlaysBig(e);
		const double share = big ? 1.0 : DebrisLanding::VolumeShare(e.Pieces, e.BigCount);
		const float volume = (float)(e.Volume * share * (double)(float)r_debris_sounds_volume);
		if (!(volume > 0.f))
			return;
		sector_t* sector = Level->PointInSector(e.Point.x, e.Point.y);
		if (sector != nullptr && (sector->Flags & SECF_SILENT))
			return;
		const float pitch = (float)pr_debrisland.RandomFloat(e.PitchMin, e.PitchMax);
		S_SoundPitchAt(Level, DVector3(e.Point.x, e.Point.y, e.Point.z), CHAN_AUTO, CHANF_OVERLAP,
			FSoundID::fromInt(big ? e.BigSound : e.Sound), volume, ATTN_NORM, pitch);
	}
}

void DebrisLandingSounds::SyncDefinitions(uint64_t levelSerial)
{
	const uint64_t generation = ParticleDebrisGeneration();
	if (mSynced && generation == mDebrisListSeen && levelSerial == mLevelSerialSeen)
		return;
	mSynced = true;
	mDebrisListSeen = generation;
	mLevelSerialSeen = levelSerial;

	for (SlotSound& slot : mSlots)
		slot = SlotSound();
	mSoundSlots = 0;

	const ParticleDebrisDefinition* debris = ParticleDebrisDefinitionData();
	const unsigned count = debris != nullptr ? ParticleDebrisDefinitionCount() : 0;
	if (count == 0 || soundEngine == nullptr)
		return;

	// Names are resolved again on every map (a map's own SNDINFO can add sounds); a missing one is reported once per load
	// of the definitions.
	const bool report = mMissingLoggedGeneration != generation;
	mMissingLoggedGeneration = generation;
	for (unsigned i = 0; i < count; i++)
	{
		const ParticleDebrisDefinition& d = debris[i];
		if (d.Slot < 0 || d.Slot >= DEBRIS_DEFINITION_SLOTS || d.LandSound.IsEmpty())
			continue;
		const FSoundID sound = S_FindSound(d.LandSound);
		if (!sound.isvalid())
		{
			if (report)
				Printf(TEXTCOLOR_ORANGE "DebrisLanding: landsound \"%s\" (particle definition slot %d) is not a sound in SNDINFO -- those pieces land silently\n",
					d.LandSound.GetChars(), d.Slot);
			continue;
		}
		SlotSound& slot = mSlots[d.Slot];
		slot.Sound = sound.index();
		if (!d.LandSoundBig.IsEmpty())
		{
			const FSoundID big = S_FindSound(d.LandSoundBig);
			if (big.isvalid())
				slot.BigSound = big.index();
			else if (report)
				Printf(TEXTCOLOR_ORANGE "DebrisLanding: landsound's big-group sound \"%s\" (particle definition slot %d) is not a sound in SNDINFO -- a big group makes \"%s\"\n",
					d.LandSoundBig.GetChars(), d.Slot, d.LandSound.GetChars());
		}
		slot.BigCount = std::max(d.LandBigCount, 1);
		slot.Volume = std::clamp((double)d.LandVolume, 0.0, 1.0);
		slot.PitchMin = std::min((double)d.LandPitchMin, (double)d.LandPitchMax);
		slot.PitchMax = std::max((double)d.LandPitchMin, (double)d.LandPitchMax);
		mSoundSlots++;
	}
}

bool DebrisLandingSounds::BeginBurst(FLevelLocals* Level, const FDebrisBurstEvent& burst, int tic, const DebrisDefinitionGpu& definition, float sizeTuning)
{
	mBurstSlot = -1;
	const int slot = burst.DefinitionSlot;
	if (mSoundSlots == 0 || slot < 0 || slot >= DEBRIS_DEFINITION_SLOTS || mSlots[slot].Sound <= 0 || !r_debris_sounds || Level == nullptr)
		return false;
	mBurstStartNs = PerfLog::GroupsWanted() ? I_nsTime() : 0;

	// What every piece of the burst shares, as debris_step.comp reads it: the definition's forces and bounce, the step's
	// radius (a mesh's thinnest half extent at its scale, a billboard's half its first size), the contact's reach, and
	// SpawnParticles' plane and floor in shader axes.
	DebrisLanding::Flight shared;
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
		// The step's reach is DebrisSupport in the chunk's current turn; over uniformly random turns a box around its origin
		// reaches a quarter of its extents' sum on average.
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

	// The level, where the step collides with it. A burst in the air knows no surface of its own; its pieces are first
	// flown against the floor under it.
	mBurstLevelCollide = definition.Motion[2] > 1.5f && r_particlecollision;
	if (mBurstLevelCollide && !shared.HasPlane && !shared.HasFloor)
	{
		if (sector_t* sector = Level->PointInSector(burst.Pos.X, burst.Pos.Y))
		{
			shared.HasFloor = true;
			shared.FloorHeight = sector->floorplane.ZatPoint(DVector2(burst.Pos.X, burst.Pos.Y));
		}
	}

	mBuilder.Begin(shared);
	mBurstLevel = Level;
	mBurstSlot = slot;
	mBurstBirth = tic / (double)TICRATE;
	mBurstRestLife = definition.Physics[2];
	return true;
}

void DebrisLandingSounds::AddPiece(const float* spawnA, const float* spawnB)
{
	if (mBurstSlot < 0)
		return;
	// A piece that never rests is freed once its life is over (the step's `now - birth > life`), so it can only land before.
	int maxSteps = DebrisLanding::kMaxSteps;
	if (!(mBurstRestLife > 0.0))
		maxSteps = (int)std::floor((double)spawnB[3] / DebrisLanding::kStepSeconds + 1.0e-4);
	mBuilder.AddPiece({ spawnA[0], spawnA[1], spawnA[2] }, { spawnB[0], spawnB[1], spawnB[2] }, maxSteps);
	mPredicted++;
}

void DebrisLandingSounds::EndBurst(int pieces)
{
	if (mBurstSlot < 0)
		return;
	const int slot = mBurstSlot;
	mBurstSlot = -1;

	// The group's first landing: the soonest candidate, or where the step collides with the level, the candidates traced
	// through it.
	LevelQuery query;
	query.Level = mBurstLevel;
	DebrisLanding::Landing first;
	int traced = 0;
	const bool have = DebrisLanding::GroupFirstLanding(mBuilder, mBurstLevelCollide && mBurstLevel != nullptr ? &query : nullptr, first, traced);
	mTraced += (uint64_t)traced;

	if (have && pieces > 0)
	{
		// Heard when it is SEEN landing: the pool draws a piece between its last two steps, one step behind (gpuparticles.vp,
		// meshparticles.vp), so a landing in step k shows at birth + (k + 1) steps.
		const SlotSound& sound = mSlots[slot];
		DebrisLanding::Event e;
		e.Time = mBurstBirth + (first.Step + 1) * DebrisLanding::kStepSeconds;
		e.Point = { first.Point.x, first.Point.z, first.Point.y };	// shader -> Doom axes
		e.Pieces = pieces;
		e.Sound = sound.Sound;
		e.BigSound = sound.BigSound;
		e.BigCount = sound.BigCount;
		e.Volume = sound.Volume;
		e.PitchMin = sound.PitchMin;
		e.PitchMax = sound.PitchMax;
		if (!mScheduler.Add(e) && !mFullLogged)
		{
			mFullLogged = true;
			Printf(TEXTCOLOR_ORANGE "DebrisLanding: more than %d landing sounds waiting -- the newest are dropped (once per map)\n", DebrisLanding::kMaxPending);
		}
	}

	if (mBurstStartNs != 0)
		mCostNs += I_nsTime() - mBurstStartNs;
	mBurstStartNs = 0;
}

void DebrisLandingSounds::Update(FLevelLocals* Level, int maptime, double ticFrac, bool simulating)
{
	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;
	const bool hadWork = mCostNs > 0 || mScheduler.Waiting() > 0;

	if (!simulating || !r_debris_sounds || Level == nullptr)
	{
		// Pieces that are not simulated are not drawn: their sounds go with them, and none waits for a switch back on.
		if (mScheduler.Waiting() > 0)
			mScheduler.Clear();
	}
	else
	{
		// The draw's level clock (hw_drawinfo.cpp's mLevelTime), never going back within a map: while the game is paused
		// TicFrac is held at 1, and after it the clock waits for level time to pass that.
		// RS FORK -- WORLD CLOCK: the shared value. Debris is a world effect, so a landing
		// is scheduled on world time and lands when the world says it does.
		const double now = Level->WorldSeconds(std::clamp(ticFrac, 0.0, 1.0));
		(void)maptime;	// the clock now comes from the level, not the caller's copy
		if (!mClockValid || now > mClock)
		{
			mClock = now;
			mClockValid = true;
		}
		mScheduler.Update(mClock, [&](const DebrisLanding::Event& e) { PlayLanding(Level, e); });
	}

	if (timed && hadWork)
		PerfLog::AddCpuSample("fx.debrisland", ((double)mCostNs + (double)(I_nsTime() - startNs)) / 1e6);
	mCostNs = 0;
}

void DebrisLandingSounds::Clear()
{
	mScheduler.Clear();
	mClock = 0.0;
	mClockValid = false;
	mFullLogged = false;
	mBurstSlot = -1;
}

FString DebrisLandingSounds::Report() const
{
	const DebrisLanding::Scheduler::Stats& stats = mScheduler.GetStats();
	FString text;
	text.Format("Debris landing sounds: r_debris_sounds %d, r_debris_sounds_volume %g -- %d debris definition%s make%s one; %llu pieces flown, "
		"%llu candidates traced through the level; %llu groups (%llu joined another), %llu sounds started, %llu dropped late, "
		"%llu dropped by the area limit, %llu dropped with the queue full, %u waiting",
		(int)*r_debris_sounds, (double)(float)*r_debris_sounds_volume, mSoundSlots, mSoundSlots == 1 ? "" : "s", mSoundSlots == 1 ? "s" : "",
		(unsigned long long)mPredicted, (unsigned long long)mTraced, (unsigned long long)stats.Groups, (unsigned long long)stats.Merged,
		(unsigned long long)stats.Started, (unsigned long long)stats.Late, (unsigned long long)stats.Limited, (unsigned long long)stats.Full,
		(unsigned)mScheduler.Waiting());
	return text;
}
