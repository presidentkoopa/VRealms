/*
** s_hearingimpulse.cpp
**
** [HEARINGIMPULSE] Hearing impulses ("ringing ears"): the reader, the room measure, the envelope and the settings.
** s_hearingimpulse.h says what it is and how a sound update runs.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SENSORY_IMPULSES_PLAN.md" 3, "Engine docs/EXPOSURE_IMPULSE_SI_L_IMPL_NOTES.md" ("What SI-H must match") and
** "Engine docs/HEARING_IMPULSE_SI_H_IMPL_NOTES.md".
*/

#include "s_hearingimpulse.h"

#include "actor.h"
#include "c_cvars.h"
#include "g_levellocals.h"
#include "i_sound.h"
#include "i_time.h"
#include "printf.h"
#include "r_levelray.h"
#include "r_sky.h"
#include "s_soundinternal.h"

// THE OWNER'S SETTINGS (s_hearingimpulse.h). Read by the sound update every frame, never by script. The names, types, defaults
// and ranges are the shared front's contract: SI-L's SoundOptions rows (HearingImpulseMuffles, HearingImpulseRings) and its
// defaults line read them. Off by default (the owner's answer of 2026-09-15); turned on, the recommended choices: heavy, faint.
CVARD(Bool, snd_hearingimpulse, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "ringing ears: a close blast in a small room muffles world sound for a moment -- menu sounds and music never (off by default; a volume dip only without EFX)")
CUSTOM_CVARD(Int, snd_hearingimpulse_muffle, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "0 heavy (about 2 s), 1 mild (about 1 s), 2 nearly deaf (about 4 s)")
{
	if (self < 0) { self = 0; return; }
	if (self > 2) { self = 2; return; }
}
CUSTOM_CVARD(Float, snd_hearingimpulse_ring, 0.35f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "ear ring level: 0.35 faint, 0.7 clear, 0 none (no source made)")
{
	if (!(self >= 0.f)) { self = 0.f; return; }
	if (self > 1.f) { self = 1.f; return; }
}
CUSTOM_CVARD(Float, snd_hearingimpulse_strength, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "a scale over every blast, 0-2")
{
	if (!(self >= 0.f)) { self = 0.f; return; }
	if (self > 2.f) { self = 2.f; return; }
}
CUSTOM_CVARD(Float, snd_hearingimpulse_recovery_scale, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "recovery time, times the muffle's own, 0.5-2")
{
	if (!(self >= 0.5f)) { self = 0.5f; return; }
	if (self > 2.f) { self = 2.f; return; }
}
CVARD(Bool, snd_hearingimpulse_test, false, CVAR_GLOBALCONFIG, "a strength-1.5 blast at the listener every 2.5 s (not saved)")
// Console only (no menu row): calibrates the room constants on a map.
CVARD(Bool, snd_hearingimpulse_debug, false, CVAR_GLOBALCONFIG, "prints each sound update's ringing ears dose, room measure and envelope")

using namespace HearingImpulseCore;

namespace
{
	// hw_effectlights.cpp's ReadQueue (after hw_debrispool.cpp's and hw_smokevolume.cpp's), copied per reader as the house does:
	// every event of the queue not read before, the older generation first, from this reader's own cursor. Nothing is written
	// to the queue.
	template<class T, int N, class Take>
	void ReadQueue(const FEffectTicQueue<T, N> &queue, uint64_t &cursorSerial, int &cursorCount, Take &&take)
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

	// The room walks: 8 horizontal directions, 45 degrees apart.
	constexpr double ROOM_RAY_DIRECTIONS[8][2] =
	{
		{ 1.0, 0.0 }, { 0.7071067811865476, 0.7071067811865476 }, { 0.0, 1.0 }, { -0.7071067811865476, 0.7071067811865476 },
		{ -1.0, 0.0 }, { -0.7071067811865476, -0.7071067811865476 }, { 0.0, -1.0 }, { 0.7071067811865476, -0.7071067811865476 },
	};

	// A map position's cache cell, kept in int range for any finite input.
	int RoomCell(double v)
	{
		return (int)std::floor(std::clamp(v, -1.0e6, 1.0e6) / ROOM_CACHE_GRID);
	}

	// A float cvar as a finite double inside lo..hi (NaN gives `fallback`).
	double Setting(float value, double lo, double hi, double fallback)
	{
		return std::isfinite(value) ? std::clamp((double)value, lo, hi) : fallback;
	}
}

HearingImpulses &HearingImpulses::Get()
{
	static HearingImpulses instance;
	return instance;
}

void HearingImpulses::Clear()
{
	mEnvelope = Envelope();
	for (RoomSample &room : mRooms)
		room = RoomSample();
	mNextRoom = 0;
}

//==========================================================================
//
// The room factor at a blast (plan 3b), cached per (sector, 32-unit cell) for ROOM_CACHE_SECONDS.
//
// Geometry works on every map: 8 LevelRay walks from the blast at its height kept inside the sector's floor..ceiling
// (SI-L's rule: probe from clamp(P.z, floor + 8, ceiling - 8)), each up to ROOM_RAY_LENGTH, and the sector's height (a sky
// ceiling counts as open). Where a mapper gave the blast's sound zone an environment other than the level default, its size is
// blended in half and half. LevelRay reads the level only.
//
//==========================================================================

const HearingImpulses::RoomSample &HearingImpulses::RoomAt(FLevelLocals *level, const DVector3 &pos, double now)
{
	mOpenAir = RoomSample();
	mOpenAir.MeanStop = ROOM_RAY_LENGTH;
	mOpenAir.Height = ROOM_SKY_HEIGHT;
	if (level == nullptr || level->sectors.Size() == 0)
		return mOpenAir;
	sector_t *sector = level->PointInSector(pos.X, pos.Y);
	if (sector == nullptr)
		return mOpenAir;

	const int cx = RoomCell(pos.X), cy = RoomCell(pos.Y), cz = RoomCell(pos.Z);
	for (const RoomSample &room : mRooms)
	{
		if (room.Sector == sector->sectornum && room.X == cx && room.Y == cy && room.Z == cz && now < room.Expires)
			return room;
	}

	const DVector2 xy(pos.X, pos.Y);
	const double floorZ = sector->floorplane.ZatPoint(xy);
	const double ceilingZ = sector->ceilingplane.ZatPoint(xy);
	const bool sky = sector->GetTexture(sector_t::ceiling) == skyflatnum;
	const double height = sky ? ROOM_SKY_HEIGHT : std::max(0.0, ceilingZ - floorZ);
	const double z = ceilingZ - floorZ >= 2.0 * ROOM_RAY_CLEARANCE
		? std::clamp(pos.Z, floorZ + ROOM_RAY_CLEARANCE, ceilingZ - ROOM_RAY_CLEARANCE)
		: 0.5 * (floorZ + ceilingZ);

	const DVector3 from(pos.X, pos.Y, z);
	double total = 0.0;
	for (const auto &d : ROOM_RAY_DIRECTIONS)
	{
		const DVector3 to(pos.X + d[0] * ROOM_RAY_LENGTH, pos.Y + d[1] * ROOM_RAY_LENGTH, z);
		const LevelRay::Result walk = LevelRay::Walk(level, from, to);
		total += walk.Blocked && std::isfinite(walk.Distance) ? std::clamp(walk.Distance, 0.0, ROOM_RAY_LENGTH) : ROOM_RAY_LENGTH;
	}
	const double meanStop = total / 8.0;

	bool zoned = false;
	double envSize = 0.0;
	if (sector->ZoneNumber < level->Zones.Size())
	{
		const ReverbContainer *environment = level->Zones[sector->ZoneNumber].Environment;
		// The level default, as maploader.cpp fills every zone with it (MAPINFO's, else the built-in "Off").
		const ReverbContainer *levelDefault = S_FindEnvironment(level->DefaultEnvironment);
		if (levelDefault == nullptr)
			levelDefault = DefaultEnvironments[0];
		if (environment != nullptr && environment != levelDefault && std::isfinite(environment->Properties.EnvSize))
		{
			zoned = true;
			envSize = environment->Properties.EnvSize;
		}
	}

	// The first expired slot, else the next in turn.
	int slot = -1;
	for (int i = 0; i < ROOM_CACHE_SIZE && slot < 0; i++)
	{
		if (!(now < mRooms[i].Expires))
			slot = i;
	}
	if (slot < 0)
	{
		slot = mNextRoom;
		mNextRoom = (mNextRoom + 1) % ROOM_CACHE_SIZE;
	}
	RoomSample &room = mRooms[slot];
	room.Sector = sector->sectornum;
	room.X = cx;
	room.Y = cy;
	room.Z = cz;
	room.Expires = now + ROOM_CACHE_SECONDS;
	room.R = RoomFactor(meanStop, height, zoned, envSize);
	room.MeanStop = meanStop;
	room.Height = height;
	return room;
}

//==========================================================================
//
// One sound update (s_hearingimpulse.h, "ONE SOUND UPDATE").
//
//==========================================================================

void HearingImpulses::Update(FLevelLocals *level, AActor *listener)
{
	// The real clock: a muffle recovers in seconds whatever the game speed, and also under a menu.
	const uint64_t ns = I_nsTime();
	if (!mClockStarted)
	{
		mClockStarted = true;
		mClockStart = ns;
		mClockLast = ns;
	}
	const double now = ns > mClockStart ? double(ns - mClockStart) * 1.0e-9 : 0.0;
	const double dt = ns > mClockLast ? std::min(double(ns - mClockLast) * 1.0e-9, DT_MAX) : 0.0;
	mClockLast = std::max(ns, mClockLast);

	// 1. A new map or a savegame load: nothing carries over, and the cursor starts on the new queue.
	const uint64_t serial = level != nullptr ? level->LevelDataSerial : 0;
	if (serial != mLevelSerial)
	{
		mLevelSerial = serial;
		mCursorSerial = 0;
		mCursorCount = 0;
		Clear();
	}

	const bool enabled = *snd_hearingimpulse;
	const bool heard = enabled && listener != nullptr && level != nullptr;
	const double strengthScale = Setting(*snd_hearingimpulse_strength, 0.0, 2.0, 1.0);

	// The listener's position as the sound listener has it (S_SetListener: SoundPos, which swaps y and z).
	DVector3 ear(0.0, 0.0, 0.0);
	if (listener != nullptr)
	{
		const FVector3 p = listener->SoundPos();
		ear = DVector3(p.X, p.Z, p.Y);
	}

	double sum = 0.0;
	double largestDose = 0.0;
	double largestRecovery = 1.0;
	double largestDistance = 0.0;
	const RoomSample *largestRoom = nullptr;
	RoomSample largestRoomCopy;

	// 3. One event weighed at the listener (plan 3b). No facing (ears) and no occlusion (#23 can add it with the same walker).
	auto weigh = [&](const DVector3 &pos, double strength, double reach, double recovery)
	{
		if (!(std::isfinite(pos.X) && std::isfinite(pos.Y) && std::isfinite(pos.Z) && std::isfinite(strength) &&
			std::isfinite(reach) && std::isfinite(recovery)))
			return;
		strength = std::clamp(strength, 0.0, 16.0);
		reach = std::clamp(reach, 16.0, 8192.0);
		recovery = std::clamp(recovery, 0.25, 4.0);
		const double distance = (pos - ear).Length();
		const double fd = DistanceFactor(distance, reach);
		if (!(strength * fd >= CULL))
			return;
		const RoomSample &room = RoomAt(level, pos, now);
		const double dose = strength * fd * room.R * strengthScale;
		if (!(dose >= DOSE_MIN))
			return;
		sum += dose;
		if (dose > largestDose)
		{
			largestDose = dose;
			largestRecovery = recovery;
			largestDistance = distance;
			largestRoomCopy = room;
			largestRoom = &largestRoomCopy;
		}
	};

	// 2. The shared front's queue (FLevelLocals::HearingImpulses, written by LevelLocals.HearingImpulse) from this reader's
	// cursor, read even while off or with no listener, so nothing is saved up for later.
	if (level != nullptr)
	{
		ReadQueue(level->HearingImpulses, mCursorSerial, mCursorCount, [&](const FHearingImpulseEvent &e, int tic)
		{
			if (heard && level->maptime - tic <= STALE_TICS)
				weigh(e.Pos, e.Strength, e.Reach, e.Recovery);
		});
	}

	// The test source: a fixed pattern at the listener, the first at once.
	if (heard && *snd_hearingimpulse_test)
	{
		if (now >= mNextTest)
		{
			weigh(ear, TEST_STRENGTH, TEST_REACH, 1.0);
			mNextTest = now + TEST_PERIOD;
		}
	}
	else
	{
		mNextTest = now;
	}

	if (!enabled)
	{
		// Off: impulses dropped. A hearing handed over before it was switched off is undone once.
		mEnvelope = Envelope();
		if (mSent && GSnd != nullptr)
			GSnd->SetWorldHearing(1.f, 1.f, 0.f);
		mSent = false;
		return;
	}

	// 4. The envelope (plan 3c): the muffle's own recovery time times snd_hearingimpulse_recovery_scale.
	const MuffleLevel &muffle = Muffle(*snd_hearingimpulse_muffle);
	const double recoveryScale = Setting(*snd_hearingimpulse_recovery_scale, 0.5, 2.0, 1.0);
	Step(mEnvelope, sum, largestDose, largestRecovery, dt, now, muffle, recoveryScale);

	if (*snd_hearingimpulse_debug && largestRoom != nullptr)
	{
		Printf("hearing impulse: sum %.3f, largest %.3f at %.0f units (room %.2f: mean stop %.0f, height %.0f), T %.3f L %.3f\n",
			sum, largestDose, largestDistance, largestRoom->R, largestRoom->MeanStop, largestRoom->Height, mEnvelope.T, mEnvelope.L);
	}

	// 5. The hand-off: every update while live, once more at neutral when idle. The ring's full level is the cvar's.
	if (GSnd == nullptr)
		return;
	if (mEnvelope.Live)
	{
		const Hearing h = Map(mEnvelope, muffle, Setting(*snd_hearingimpulse_ring, 0.0, 1.0, 0.0));
		GSnd->SetWorldHearing(h.Gain, h.GainHF, h.Ring);
		mSent = true;
	}
	else if (mSent)
	{
		GSnd->SetWorldHearing(1.f, 1.f, 0.f);
		mSent = false;
	}
}
