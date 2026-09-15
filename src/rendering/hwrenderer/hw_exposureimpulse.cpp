/*
** hw_exposureimpulse.cpp
**
** [EXPOSUREIMPULSE] Flash blindness: the level's queue weighed for this viewer, the envelope, the test source and the frame
** the wash draws. See hw_exposureimpulse.h; the maths is hw_exposureimpulsecore.h; the drawing is PPExposureImpulse
** (hw_postprocess.h / .cpp, exposureimpulse.fp).
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

#include "hw_exposureimpulse.h"
#include "hw_exposureimpulsecore.h"
#include "r_levelray.h"
#include "hw_perflog.h"
#include "g_levellocals.h"
#include "r_utility.h"
#include "r_defs.h"
#include "c_cvars.h"
#include "v_video.h"
#include "i_time.h"
#include "printf.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"

using namespace ExposureImpulseCore;

// ---------------------------------------------------------------------------------------------------------------------------
// The switches. Every one is read here every frame, so it responds with a menu open. Local: nothing reaches another machine.
// Not SERVERINFO. Each is clamped where it is set and again where it is read.
// ---------------------------------------------------------------------------------------------------------------------------

// "Flash blindness". OFF BY DEFAULT: the owner's explicit exception to "effects on" (2026-09-15). Off: flashes are read and
// dropped, and the wash never draws.
CVARD(Bool, r_exposureimpulse, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "flash blindness: a flash a mod marks washes your view out for a moment, strongest in the dark and when you face it (off by default)")

// "Flash blindness look" (owner answer 1, all three as a menu choice): 0 a strong wash (about 1.5 s, the default), 1 near
// white-out (about 3 s), 2 a quick sting (under a second). hw_exposureimpulsecore.h LookFor.
CUSTOM_CVARD(Int, r_exposureimpulse_look, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "flash blindness look: 0 strong wash (about 1.5 s), 1 near white-out (about 3 s), 2 quick sting (under 1 s)")
{
	if (self < 0) self = 0;
	else if (self > LOOK_COUNT - 1) self = LOOK_COUNT - 1;
}

// "Flash blindness comfort" (owner answer 2): rises over 1/4 s, holds 0.6 s (at most 2 rises a second), never past 35%, no
// glare, a whiter haze.
CVARD(Bool, r_exposureimpulse_comfort, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "flash blindness comfort: rises over 1/4 s, at most 35% and 2 rises a second, no glare, a whiter haze")

CUSTOM_CVARD(Float, r_exposureimpulse_strength, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "flash blindness strength: a scale over every flash, 0-2 (1 = as the mods made them)")
{
	if (!(self >= 0.f)) self = 0.f;
	else if (self > 2.f) self = 2.f;
}

// The hard cap (A4): the standard limit is 60%, and this can only lower it.
CUSTOM_CVARD(Float, r_exposureimpulse_cap, 0.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "flash blindness limit: the most the view can wash out, 0.1-0.6 (Comfort: 0.35 at most)")
{
	if (!(self >= (float)CAP_MIN)) self = (float)CAP_MIN;
	else if (self > (float)CAP_MAX) self = (float)CAP_MAX;
}

CUSTOM_CVARD(Float, r_exposureimpulse_recovery_scale, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "flash blindness recovery time: a scale over the look's own, 0.5-2")
{
	if (!(self >= 0.5f)) self = 0.5f;
	else if (self > 2.f) self = 2.f;
}

// Owner answer 3 (the protected laser look): grab lasers and the Lance stay crisp and exactly themselves over the wash where
// "Keep legacy lasers" carries the light mask. Off: they wash out with the room for that moment.
CVARD(Bool, r_exposureimpulse_holdbeams, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "grab lasers and the Lance stay crisp over a flash's wash while Keep legacy lasers is on")

// Tests, not settings: not archived.
CUSTOM_CVARD(Int, r_exposureimpulse_test, 0, CVAR_GLOBALCONFIG, "N test flashes a second, 64 units ahead of your head, 0-4 (needs flash blindness on; works with the menu open)")
{
	if (self < 0) self = 0;
	else if (self > TEST_MAX_PER_SECOND) self = TEST_MAX_PER_SECOND;
}

CVARD(Bool, r_exposureimpulse_debug, false, CVAR_GLOBALCONFIG, "flash blindness: one log line when a burst begins and one when it clears")

namespace
{
	// hw_effectlights.cpp's ReadQueue, as every reader keeps its own copy: every event not read before, the older generation
	// first, from this reader's own cursor, with the tic its generation was written. Nothing is written to the queue.
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
}

ExposureImpulses& ExposureImpulses::Get()
{
	static ExposureImpulses instance;
	return instance;
}

void ExposureImpulses::PublishNone()
{
	hw_postprocess.exposureimpulse.SetFrame(PPExposureImpulseFrame());
}

void ExposureImpulses::BeginFrame(FLevelLocals* Level, const FRenderViewpoint& vp, uint64_t levelSerial)
{
	if (Level == nullptr)
	{
		Rest(mEnvelope);
		PublishNone();
		return;
	}

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	// The real clock (screen->FrameTime, milliseconds): a wash recovers with a menu open or the game paused, as eyes would.
	const double now = (double)screen->FrameTime / 1000.0;
	double dt = mLastNow >= 0.0 ? now - mLastNow : 0.0;
	dt = dt > 0.0 ? std::min(dt, DT_MAX) : 0.0;
	mLastNow = now;

	if (levelSerial != mLevelSerial)
	{
		// A new map or a savegame load (ClearLevelData emptied the queue): the wash and the cursor start over.
		mLevelSerial = levelSerial;
		mQueueSerial = 0;
		mQueueCount = 0;
		mTestCarry = 0.0;
		Rest(mEnvelope);
	}

	const bool on = r_exposureimpulse;
	const int lookIndex = std::clamp((int)*r_exposureimpulse_look, 0, (int)LOOK_COUNT - 1);
	const Look look = LookFor(lookIndex);
	const bool comfort = r_exposureimpulse_comfort;
	const double strengthScale = ClampSetting((double)(float)*r_exposureimpulse_strength, 0.0, 2.0, 1.0);

	// The head: the frame's viewpoint after VRMode::SetUp -- in the headset its yaw and pitch are the head's.
	const double eye[3] = { vp.Pos.X, vp.Pos.Y, vp.Pos.Z };
	double forward[3];
	Forward(vp.Angles.Yaw.Radians(), (double)vp.HWAngles.Pitch.Radians(), forward);

	FrameInput input;
	int events = 0;
	int traced = 0;
	const auto weigh = [&](const double pos[3], double strength, double reach, double recovery, double r, double g, double b)
	{
		double distance = 0.0;
		double dose = DoseBeforeSight(eye, forward, pos, strength, reach, strengthScale, look.DoseScale, distance);
		if (!(dose >= MIN_DOSE))
			return;
		if (distance > WALL_FACE_UNITS)
		{
			const double k = (distance - WALL_FACE_UNITS) / distance;
			const DVector3 from(eye[0], eye[1], eye[2]);
			const DVector3 to(eye[0] + (pos[0] - eye[0]) * k, eye[1] + (pos[1] - eye[1]) * k, eye[2] + (pos[2] - eye[2]) * k);
			traced++;
			if (LevelRay::Walk(Level, from, to).Blocked)
				dose *= SIGHT_BLOCKED;
			if (!(dose >= MIN_DOSE))
				return;
		}
		input.Add(dose, recovery, r, g, b);
	};

	// The level's flashes. Always read, so the cursor keeps up; weighed only while the switch is on and the event is fresh.
	ReadQueue(Level->ExposureImpulses, mQueueSerial, mQueueCount, [&](const FExposureImpulseEvent& e, int tic)
	{
		events++;
		if (!on || Level->maptime - tic > STALE_TICS)
			return;
		const double pos[3] = { e.Pos.X, e.Pos.Y, e.Pos.Z };
		weigh(pos, e.Strength, e.Reach, e.Recovery, e.Tint.r / 255.0, e.Tint.g / 255.0, e.Tint.b / 255.0);
	});

	// The test source: N white flashes a second ahead of the head, on the real clock (it works with the menu open). No RNG,
	// nothing queued on the level.
	const int testRate = on ? std::clamp((int)*r_exposureimpulse_test, 0, TEST_MAX_PER_SECOND) : 0;
	if (testRate > 0)
	{
		mTestCarry = std::min(mTestCarry + testRate * dt, (double)testRate);
		while (mTestCarry >= 1.0)
		{
			mTestCarry -= 1.0;
			events++;
			const double pos[3] = { eye[0] + forward[0] * TEST_UNITS, eye[1] + forward[1] * TEST_UNITS, eye[2] + forward[2] * TEST_UNITS };
			weigh(pos, TEST_STRENGTH, TEST_REACH, TEST_RECOVERY, 1.0, 1.0, 1.0);
		}
	}
	else
	{
		mTestCarry = 0.0;
	}

	const bool wasLive = mEnvelope.Live;
	PPExposureImpulseFrame frame;
	if (!on)
	{
		Rest(mEnvelope);
	}
	else
	{
		const Timing timing = TimingFor(comfort, (double)(float)*r_exposureimpulse_cap);
		const double recoveryScale = ClampSetting((double)(float)*r_exposureimpulse_recovery_scale, 0.5, 2.0, 1.0);
		const bool onset = Step(mEnvelope, now, dt, input, timing, look.RecoverySeconds * recoveryScale);

		if (*r_exposureimpulse_debug)
		{
			if (onset)
			{
				float darkExposure = 0.0f, litExposure = 0.0f;
				const bool meter = PPExposureImpulse::MeterReferences((float)LIT_LIGHT, (float)METER_MIN_SPAN, darkExposure, litExposure);
				const int light = vp.sector != nullptr ? (int)vp.sector->lightlevel : -1;
				Printf("exposure impulse: burst %llu -- sum %.3f, T %.3f, largest dose %.3f (recovery x%.2f); %s (exposure dark %.3f, lit %.3f "
					"at light %.2f); fallback darkness %.2f (sector light %d); look %d%s, cap %.2f; %d flash(es), %d traced\n",
					(unsigned long long)mEnvelope.Burst, input.Sum, mEnvelope.T, input.LargestDose, input.LargestRecovery,
					meter ? "metered: the pass latches this eye's exposure (on the GPU, not read back)" : "not metered (bloom off or the exposure settings too flat)",
					darkExposure, litExposure, LIT_LIGHT, FallbackDarkness(light), light, lookIndex, comfort ? " comfort" : "", timing.Cap,
					events, traced);
				mBurstStart = now;
				mBurstPeak = 0.0;
			}
			mBurstPeak = std::max(mBurstPeak, mEnvelope.L);
		}

		if (mEnvelope.Live)
		{
			frame.Live = true;
			frame.Burst = mEnvelope.Burst;
			frame.Amount = (float)(timing.Cap * mEnvelope.L);
			frame.Gain = (float)look.Gain;
			frame.Veil = (float)look.Veil;
			frame.Desaturate = (float)look.Desaturate;
			frame.Glare = comfort ? 0.0f : (float)look.Glare;
			double tint[3];
			BurstTint(mEnvelope, comfort, tint);
			frame.VeilTint = FVector3((float)tint[0], (float)tint[1], (float)tint[2]);
			frame.DarkFloor = (float)DARK_FLOOR;
			frame.LitLight = (float)LIT_LIGHT;
			frame.MeterMinSpan = (float)METER_MIN_SPAN;
			frame.FallbackDarkness = (float)(vp.sector != nullptr ? FallbackDarkness((double)vp.sector->lightlevel) : 0.0);
			frame.HoldBeams = r_exposureimpulse_holdbeams;
		}
	}
	if (wasLive && !mEnvelope.Live && *r_exposureimpulse_debug)
		Printf("exposure impulse: burst %llu clear after %.2f s (peak wash %.3f of the limit)\n", (unsigned long long)mEnvelope.Burst,
			now - mBurstStart, mBurstPeak);

	hw_postprocess.exposureimpulse.SetFrame(frame);

	if (timed && (events > 0 || wasLive || mEnvelope.Live))
		PerfLog::AddCpuSample("fx.exposureimpulse", (double)(I_nsTime() - startNs) / 1e6);
}
