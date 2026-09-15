/*
** s_hearingimpulse.h
**
** [HEARINGIMPULSE] Hearing impulses ("ringing ears"): a blast close to the listener, in an enclosed space, muffles the world's
** sound on the listener's machine for a moment -- menu sounds and music never -- and may leave a faint ring in the ears.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SENSORY_IMPULSES_PLAN.md" (section 3, with its OWNER ANSWERS of 2026-09-15),
** "Engine docs/EXPOSURE_IMPULSE_SI_L_IMPL_NOTES.md" (the shared front: the event, the queue, the native, LevelRay, the menu rows)
** and "Engine docs/HEARING_IMPULSE_SI_H_IMPL_NOTES.md". A sound-side capability, named for what it does: RS_Ballistics'
** flashes and impacts are its first callers; any blast is the next. Inert until an impulse arrives or the test is on: a clock
** read and a cursor check each sound update, no AL call.
**
** WHERE AN IMPULSE COMES FROM.
**   - LevelLocals.HearingImpulse (doombase.zs, vmthunks.cpp): one-way on the level's per-tic queue (FHearingImpulseEvent,
**     FLevelLocals::HearingImpulses, g_levellocals.h).
**   - snd_hearingimpulse_test: a local test source at the listener.
**
** ONE SOUND UPDATE (S_UpdateSounds, after S_SetListener, before the sound engine applies its update).
**   1. The level-data serial changed (map change, savegame load): envelope and room cache dropped, cursor to the new queue.
**   2. The queue read from this reader's own cursor, even while off; events older than STALE_TICS tics are dropped.
**   3. Each event weighed at the listener: distance, then how enclosed the space around the BLAST is -- 8 LevelRay walks and
**      the ceiling, plus a mapper's zone environment where one is set -- cached per place for half a second.
**   4. The envelope stepped on the real clock: merge, hold, attack, recovery, idle snap (HearingImpulseCore::Step).
**   5. While live, GSnd->SetWorldHearing(gain, gainHF, ring) every update; once (1, 1, 0) when it goes idle. The OpenAL
**      renderer scales its EFX low-pass filters on every non-UI channel, or ducks their volume without EFX, and plays its
**      private ring (oalsound.cpp). Sending every live update also feeds a renderer snd_reset recreated.
**
** THE OWNER'S SETTINGS, read here every sound update (a menu freezes the game; nothing is read by script). Names, types,
** defaults and ranges are the shared front's: SI-L's SoundOptions rows and defaults line read them.
**   snd_hearingimpulse                  "Ringing ears": false (default) / true
**   snd_hearingimpulse_muffle           0 Heavy (about 2 s, recommended), 1 Mild (about 1 s), 2 Nearly deaf (about 4 s)
**   snd_hearingimpulse_ring             the ring's level, 0..1: 0.35 Faint (recommended), 0.7 Clear, 0 None (no source made)
**   snd_hearingimpulse_strength         0..2, a scale over every blast
**   snd_hearingimpulse_recovery_scale   0.5..2, times the muffle's own recovery time
**   snd_hearingimpulse_test             a strength-1.5 impulse at the listener every 2.5 s (not saved; needs Ringing ears on)
**   snd_hearingimpulse_debug            one console line per sound update that counted a dose (not saved; no menu row)
**
** NETPLAY. Presentation only, on the listener's machine: the native queues an event and returns nothing; this reads the actor
** S_UpdateSounds is handed (the local camera) and names no player; no RNG (the ring is a fixed waveform, the test a fixed
** pattern); nothing is saved (the ring is no sound-engine channel; a load or map change drops everything here). LevelRay
** reads the level only, outside the playsim tick.
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "vectors.h"

class AActor;
struct FLevelLocals;

// THE PURE PART: constants, the room factor, the envelope and the mapping. No engine state, so the mirror (H-P2) copies it.
namespace HearingImpulseCore
{
	// Reading (plan 2b / 3b).
	constexpr int    STALE_TICS = 9;             // an event older than this many tics (about 1/4 s) is dropped: a hitch saves none up
	// Weighing (plan 3b).
	constexpr double CULL = 0.05;                // strength x distance factor under this: ignored before any tracing
	constexpr double DOSE_MIN = 0.02;            // a dose under this is ignored
	constexpr double SUM_MAX = 4.0;              // one sound update's doses summed, clamped
	constexpr double ROOM_RAY_LENGTH = 1536.0;   // each of the 8 horizontal walks from the blast; an unblocked walk counts this
	constexpr double ROOM_RAY_CLEARANCE = 8.0;    // the walks run at the blast's height, kept this far inside floor..ceiling
	constexpr double ROOM_SKY_HEIGHT = 1536.0;   // the height a sky ceiling counts as
	constexpr double ROOM_OPEN = 0.35;           // the room factor in open air: a point-blank blast still dulls
	constexpr double ROOM_CACHE_SECONDS = 0.5;   // a barrage in one place traces once
	constexpr double ROOM_CACHE_GRID = 32.0;     // map units: the cache's place key
	// The envelope (plan 3c).
	constexpr double HOLD_SECONDS = 0.25;        // no fall for this long after any dose: no pumping under auto fire
	constexpr double ATTACK_SECONDS = 0.03;      // the shown level rises at most 1 per this: no click
	constexpr double IDLE = 0.002;               // target, shown level and the ring's memory all under this: snap to exactly idle
	constexpr double RING_TAIL = 1.5;            // the ring's memory decays over RING_TAIL x tau, so the ring outlasts the muffle
	constexpr double RING_FROM = 0.25;           // a burst whose shown level peaks under this never rings ...
	constexpr double RING_FULL = 0.75;           // ... and one peaking at this or more rings at the full ring level
	constexpr double DT_MAX = 0.1;               // seconds: a stall never jumps the envelope further
	// The test source (snd_hearingimpulse_test).
	constexpr double TEST_STRENGTH = 1.5;
	constexpr double TEST_REACH = 256.0;
	constexpr double TEST_PERIOD = 2.5;

	struct MuffleLevel
	{
		double Cap;              // M = Cap x L: the most the world is muffled (always under 1: you still hear something)
		double GainDrop;         // the world's gain G = 1 - GainDrop x M
		double HFFloor;          // the world's high-frequency gain GHF = max(HFFloor, (1 - M)^2.5)
		double RecoverySeconds;  // 5% left this long after the hold, before snd_hearingimpulse_recovery_scale and an event's own
	};

	// snd_hearingimpulse_muffle (the owner's three, 2026-09-15; the menu's order, HearingImpulseMuffles). Heavy is the plan's
	// mapping (3c). Out-of-range choices clamp.
	inline const MuffleLevel &Muffle(int choice)
	{
		static constexpr MuffleLevel levels[3] =
		{
			{ 0.85, 0.55, 0.03, 2.0 },	// 0 heavy:       at the cap G 0.53,  GHF 0.03;  about 2 s (recommended)
			{ 0.50, 0.45, 0.10, 1.0 },	// 1 mild:        at the cap G 0.775, GHF 0.177; about 1 s
			{ 0.97, 0.85, 0.01, 4.0 },	// 2 nearly deaf: at the cap G 0.18,  GHF 0.01;  about 4 s
		};
		return levels[std::clamp(choice, 0, 2)];
	}

	inline double Smoothstep(double edge0, double edge1, double x)
	{
		const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	// 1 at the blast, 0.5 at `reach`.
	inline double DistanceFactor(double distance, double reach)
	{
		const double q = distance / reach;
		return 1.0 / (1.0 + q * q);
	}

	// How enclosed the space around a blast is, ROOM_OPEN (open air) .. 1 (a small low room).
	//   meanStop  the mean of the 8 walks' stop distances (an unblocked walk counts ROOM_RAY_LENGTH)
	//   height    the blast's sector, ceiling minus floor (a sky ceiling counts ROOM_SKY_HEIGHT)
	//   zoned     the blast's sound zone carries an environment other than the level default; envSize is its EnvSize (metres)
	// A 256-unit room with a 128 ceiling gives about 1, a 1024 hall (256 high) about 0.44, outdoors ROOM_OPEN.
	inline double RoomFactor(double meanStop, double height, bool zoned, double envSize)
	{
		const double em = 1.0 - Smoothstep(160.0, 768.0, meanStop);
		const double eh = 1.0 - Smoothstep(128.0, 512.0, height);
		const double egeo = em * (0.5 + 0.5 * eh);
		const double e = zoned ? 0.5 * egeo + 0.5 * (1.0 - Smoothstep(3.0, 25.0, envSize)) : egeo;
		return ROOM_OPEN + (1.0 - ROOM_OPEN) * e;
	}

	struct Envelope
	{
		double T = 0.0;              // target, 0..1
		double L = 0.0;              // shown, 0..1
		double RL = 0.0;             // the ring's memory of L
		double PeakL = 0.0;          // the highest L of this burst: gates the ring, which fades as RL / PeakL
		double HoldUntil = 0.0;      // clock seconds
		double BurstDose = 0.0;      // the largest single dose of this burst ...
		double BurstRecovery = 1.0;  // ... and that event's recovery scale; both reset when idle
		bool   Live = false;
	};

	// One sound update. `sum`: this update's doses; `largestDose` / `largestRecovery`: its largest single dose and that event's
	// recovery; `dt`: seconds since the last update; `now`: the clock; `recoveryScale`: snd_hearingimpulse_recovery_scale
	// (0.5..2). An idle snap leaves exactly Envelope{}.
	inline void Step(Envelope &e, double sum, double largestDose, double largestRecovery, double dt, double now, const MuffleLevel &level,
		double recoveryScale)
	{
		sum = std::isfinite(sum) ? std::clamp(sum, 0.0, SUM_MAX) : 0.0;
		dt = std::isfinite(dt) ? std::clamp(dt, 0.0, DT_MAX) : 0.0;
		recoveryScale = std::isfinite(recoveryScale) ? std::clamp(recoveryScale, 0.5, 2.0) : 1.0;
		if (sum >= DOSE_MIN)
		{
			// Merge: each blast fills the same share of what is left, so repeats saturate and never pass 1.
			const double add = 1.0 - std::exp(-sum);
			e.T = std::min(1.0, e.T + (1.0 - e.T) * add);
			e.HoldUntil = now + HOLD_SECONDS;
			if (largestDose > e.BurstDose)
			{
				e.BurstDose = largestDose;
				e.BurstRecovery = std::clamp(largestRecovery, 0.25, 4.0);
			}
			e.Live = true;
		}
		if (!e.Live)
			return;
		const double tau = std::max(level.RecoverySeconds * recoveryScale * e.BurstRecovery / 3.0, 1.0e-3);
		e.L = std::min(e.T, e.L + dt / ATTACK_SECONDS);
		if (now >= e.HoldUntil)
		{
			e.T *= std::exp(-dt / tau);
			e.L = std::min(e.L, e.T);
		}
		e.PeakL = std::max(e.PeakL, e.L);
		e.RL = std::max(e.L, e.RL * std::exp(-dt / (RING_TAIL * tau)));
		if (e.T < IDLE && e.L < IDLE && e.RL < IDLE)
			e = Envelope();
	}

	// What the renderer is handed. Neutral (1, 1, 0) when not live.
	struct Hearing
	{
		float Gain = 1.f;
		float GainHF = 1.f;
		float Ring = 0.f;
	};

	// `ringLevel`: snd_hearingimpulse_ring, the ring's full level (0..1).
	inline Hearing Map(const Envelope &e, const MuffleLevel &level, double ringLevel)
	{
		Hearing h;
		if (!e.Live)
			return h;
		// Under IDLE the muffle is exactly neutral (a step under 0.01 dB) while the ring may still fade: no re-attach for its tail.
		const double m = e.L >= IDLE ? level.Cap * std::clamp(e.L, 0.0, 1.0) : 0.0;
		h.Gain = (float)(1.0 - level.GainDrop * m);
		h.GainHF = (float)std::max(level.HFFloor, std::pow(1.0 - m, 2.5));
		// The ring (the owner's pick: faint, fading a little after the muffle clears): gated by how hard this burst hit, then fading
		// with the ring's memory -- about 13% of its level when the muffle is 95% gone (3 tau after the hold), 5% at 4.5 tau.
		const double ringShare = e.PeakL > 0.0 ? std::clamp(e.RL / e.PeakL, 0.0, 1.0) : 0.0;
		const double ringFull = std::isfinite(ringLevel) ? std::clamp(ringLevel, 0.0, 1.0) : 0.0;
		h.Ring = (float)(ringFull * Smoothstep(RING_FROM, RING_FULL, e.PeakL) * ringShare);
		return h;
	}
}

class HearingImpulses
{
public:
	static HearingImpulses &Get();

	// Once per sound update: S_UpdateSounds, after S_SetListener and before the sound engine's update (s_doomsound.cpp).
	// `listener` is the actor that update was handed (the local camera); null weighs nothing.
	void Update(FLevelLocals *level, AActor *listener);

	bool IsLive() const { return mEnvelope.Live; }

private:
	struct RoomSample
	{
		int    Sector = -1;          // -1 = unused
		int    X = 0, Y = 0, Z = 0;  // the blast's place, in ROOM_CACHE_GRID cells
		double Expires = -1.0;       // clock seconds
		double R = HearingImpulseCore::ROOM_OPEN;
		double MeanStop = 0.0;       // kept for snd_hearingimpulse_debug
		double Height = 0.0;
	};
	static constexpr int ROOM_CACHE_SIZE = 16;

	const RoomSample &RoomAt(FLevelLocals *level, const DVector3 &pos, double now);
	void Clear();

	HearingImpulseCore::Envelope mEnvelope;
	RoomSample mRooms[ROOM_CACHE_SIZE];
	RoomSample mOpenAir;             // answered when there is no level geometry to walk
	int        mNextRoom = 0;
	uint64_t   mLevelSerial = 0;
	uint64_t   mCursorSerial = 0;
	int        mCursorCount = 0;
	bool       mClockStarted = false;
	uint64_t   mClockStart = 0;      // I_nsTime
	uint64_t   mClockLast = 0;
	double     mNextTest = 0.0;
	bool       mSent = false;        // the renderer was handed a hearing that is not yet undone by a neutral one
};
