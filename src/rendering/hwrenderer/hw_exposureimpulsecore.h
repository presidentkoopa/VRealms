/*
** hw_exposureimpulsecore.h
**
** [EXPOSUREIMPULSE] Flash blindness: the numbers and the maths, with no engine types.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SENSORY_IMPULSES_PLAN.md" 2b-2d with its OWNER ANSWERS (2026-09-15), and
** "Engine docs/EXPOSURE_IMPULSE_SI_L_IMPL_NOTES.md". hw_exposureimpulse.cpp runs this every frame; mirror_si_l.py reads its
** constants from this very text and checks the rules below over fuzzed and scripted call patterns (L-P5).
**
** THE DOSE of one flash for one viewer: strength x distance x facing x sight x the player's strength x the look's.
**   distance  1 / (1 + (d / reach)^2): half at `reach`
**   facing    1 within 24 units; else 0.2 + 0.8 * smoothstep(-0.3, 0.8, cos of the angle off the view) -- behind still 0.2
**   sight     1, or 0.3 when LevelRay finds a wall in the way (the flash still lights walls you can see)
** A dose under 0.02 is ignored. A frame's doses sum to S (clamped 0..4).
**
** THE ENVELOPE: T the target, L the shown wash, both 0..1. The picture's wash is Amount = Cap x L.
**   merge     add = 1 - exp(-S); T += (1 - T) x add. Each blast fills the same share of what is left: never past 1.
**   A1 hold   a frame with S >= 0.02 holds until now + Hold (0.35 s; Comfort 0.6), and so does every frame on which L rises.
**             L NEVER FALLS WITHIN HOLD OF AN IMPULSE OR OF ITS OWN LAST RISE.
**   A2 attack L = min(T, L + dt / Attack) (1/16 s; Comfort 1/4 s): no step in one frame, never a one-frame white.
**   A3 re-rise a rise that begins after L began falling, within 1 s of the last peak, takes add x 0.5.
**   recovery  after the hold: T *= exp(-dt / tau), L = min(L, T); tau = the look's recovery x the player's scale x the burst's
**             recovery / 3 (5% left after that many seconds).
**   idle      T and L both under 0.003: exactly 0, not live; the pass stops drawing.
**   A4 cap    Cap = the player's limit, 0.1..0.6 (the standard limit); Comfort at most 0.35.
**
** ANTI-STROBE. L can only fall once Hold has passed since both the last impulse and its own last rise, and a new rise needs a
** new impulse, so two rises begin more than Hold apart WHATEVER THE FRAME TIMING (the rise-hold matters after a hitch: without
** it, a rise delayed past its impulse's hold could fall at once and rise again within a few frames): at most 3 in any second
** (0.35 s), at most 2 with Comfort (0.6 s) -- the owner's limits, and the three-flashes-a-second photosensitivity line.
** Chaingun fire (every 4 tics) and twin chainguns read as one plateau.
**
*/

#pragma once

#include <cmath>
#include <cstdint>

namespace ExposureImpulseCore
{
	// ---- Reading the queue and weighing a flash (2b) ----------------------------------------------------------------------
	constexpr int STALE_TICS = 9;                 // an event older than this many tics is dropped: a hitch never saves up a blinding
	constexpr double MIN_DOSE = 0.02;             // a smaller dose is ignored; a frame summing at least this holds the envelope
	constexpr double MAX_FRAME_SUM = 4.0;         // a frame's doses, clamped
	constexpr double NEAR_UNITS = 24.0;           // closer than this a flash counts as faced, whatever the view
	constexpr double FACING_BEHIND = 0.2;         // a flash straight behind counts this much: the room lights up
	constexpr double FACING_EDGE0 = -0.3;         // the facing curve over the cosine off the view
	constexpr double FACING_EDGE1 = 0.8;
	constexpr double SIGHT_BLOCKED = 0.3;         // a flash behind a wall still lights the walls you see
	constexpr double WALL_FACE_UNITS = 1.0;       // the sight ray stops this short of the flash: a flash on a wall's face is not behind it
	constexpr double MIN_REACH = 1.0;             // the natives clamp reach to 16..8192; this only keeps the maths finite

	// ---- The envelope (2c; owner answer "Comfort") ------------------------------------------------------------------------
	constexpr double DT_MAX = 0.1;                // a frame's time step, clamped (a hitch does not jump the wash)
	constexpr double ATTACK_STANDARD = 0.0625;    // 1/16 s
	constexpr double ATTACK_COMFORT = 0.25;       // 1/4 s
	constexpr double HOLD_STANDARD = 0.35;        // at most 3 rises a second
	constexpr double HOLD_COMFORT = 0.6;          // at most 2 rises a second
	constexpr double CAP_MIN = 0.1;
	constexpr double CAP_MAX = 0.6;               // the standard limit: never past 60%
	constexpr double CAP_COMFORT = 0.35;
	constexpr double RERISE_WINDOW = 1.0;
	constexpr double RERISE_SHARE = 0.5;
	constexpr double IDLE_SNAP = 0.003;
	constexpr double RECOVERY_E_FOLDS = 3.0;      // e^-3: 5% left after the recovery time
	constexpr double MIN_RECOVERY_SECONDS = 0.05;

	// ---- The picture (2d) -------------------------------------------------------------------------------------------------
	constexpr double DARK_FLOOR = 0.15;           // a fully lit room keeps this share of the wash
	constexpr double LIT_LIGHT = 0.5;             // the exposure meter's "lit room" light (exposureextract.fp's units); calibrated by r_exposureimpulse_debug
	constexpr double METER_MIN_SPAN = 0.05;       // the meter is used only when its dark and lit values differ by at least this
	constexpr double COMFORT_TINT_TO_WHITE = 0.7; // Comfort moves the haze's colour this far toward white
	constexpr double FALLBACK_LIGHT_DARK = 96.0;  // without the meter: the view sector's light level, dark at or under this ...
	constexpr double FALLBACK_LIGHT_LIT = 224.0;  // ... and lit at or over this

	// ---- The test source (r_exposureimpulse_test) --------------------------------------------------------------------------
	constexpr int TEST_MAX_PER_SECOND = 4;
	constexpr double TEST_UNITS = 64.0;           // ahead of the head
	constexpr double TEST_STRENGTH = 1.2;
	constexpr double TEST_REACH = 256.0;
	constexpr double TEST_RECOVERY = 1.0;

	// ---- The owner's three looks ("Flash hit": all three as a menu choice, the strong wash by default) ---------------------
	enum ELook
	{
		LOOK_STRONG_WASH = 0,   // bright and hazy, lit things blow out and glow, you can still make things out; about 1.5 s
		LOOK_NEAR_WHITEOUT = 1, // near white-out for the biggest blasts; about 3 s -- a real handicap
		LOOK_QUICK_STING = 2,   // a brief bright glow, gone in under a second
		LOOK_COUNT = 3
	};

	struct Look
	{
		double Gain;            // the picture brightens by 1 + Gain x wash
		double Veil;            // haze added: VeilTint x Veil x wash
		double Desaturate;      // the share taken toward grey: Desaturate x wash
		double Glare;           // this eye's bloom re-added: Glare x wash
		double RecoverySeconds; // 5% left this long after the hold (times the player's scale and the burst's recovery)
		double DoseScale;       // how hard a flash hits in this look
	};

	inline Look LookFor(int look)
	{
		switch (look)
		{
		case LOOK_NEAR_WHITEOUT:
			return { 6.0, 1.0, 0.85, 3.0, 3.0, 1.5 };
		case LOOK_QUICK_STING:
			return { 1.5, 0.15, 0.2, 3.0, 0.5, 1.0 };
		default:
			return { 3.0, 0.5, 0.6, 2.0, 1.5, 1.0 };
		}
	}

	struct Timing
	{
		double Attack;
		double Hold;
		double Cap;
	};

	// The player's limit (a non-finite value takes the standard limit), Comfort on top.
	inline Timing TimingFor(bool comfort, double capSetting)
	{
		double cap = std::isfinite(capSetting) ? capSetting : CAP_MAX;
		cap = cap < CAP_MIN ? CAP_MIN : (cap > CAP_MAX ? CAP_MAX : cap);
		if (comfort && cap > CAP_COMFORT)
			cap = CAP_COMFORT;
		return { comfort ? ATTACK_COMFORT : ATTACK_STANDARD, comfort ? HOLD_COMFORT : HOLD_STANDARD, cap };
	}

	// A player's scale setting: finite and inside lo..hi, else `fallback` (non-finite) or the nearest end.
	inline double ClampSetting(double value, double lo, double hi, double fallback)
	{
		if (!std::isfinite(value))
			return fallback;
		return value < lo ? lo : (value > hi ? hi : value);
	}

	inline double Smoothstep(double edge0, double edge1, double x)
	{
		double t = (x - edge0) / (edge1 - edge0);
		t = t > 0.0 ? (t < 1.0 ? t : 1.0) : 0.0;
		return t * t * (3.0 - 2.0 * t);
	}

	inline double DistanceFactor(double distance, double reach)
	{
		const double r = distance / (reach > MIN_REACH ? reach : MIN_REACH);
		return 1.0 / (1.0 + r * r);
	}

	inline double FacingFactor(double distance, double cosine)
	{
		if (distance < NEAR_UNITS)
			return 1.0;
		return FACING_BEHIND + (1.0 - FACING_BEHIND) * Smoothstep(FACING_EDGE0, FACING_EDGE1, cosine);
	}

	// 0 bright .. 1 dark, from a sector light level (0..255), when the exposure meter cannot say.
	inline double FallbackDarkness(double lightLevel)
	{
		return 1.0 - Smoothstep(FALLBACK_LIGHT_DARK, FALLBACK_LIGHT_LIT, lightLevel);
	}

	// The view's forward vector from the yaw and the pitch (radians; a positive pitch looks down), as FRenderViewpoint's
	// ViewVector3D and the VR eye shift's AngleVectors build it.
	inline void Forward(double yaw, double pitch, double out[3])
	{
		const double pc = std::cos(pitch);
		out[0] = std::cos(yaw) * pc;
		out[1] = std::sin(yaw) * pc;
		out[2] = -std::sin(pitch);
	}

	// A flash's dose before the sight test (sight can only lower it), and its distance from the eye. Everything finite in,
	// something finite out; a zero-length offset counts as faced.
	inline double DoseBeforeSight(const double eye[3], const double forward[3], const double pos[3], double strength, double reach,
		double strengthScale, double doseScale, double& distance)
	{
		const double vx = pos[0] - eye[0], vy = pos[1] - eye[1], vz = pos[2] - eye[2];
		distance = std::sqrt(vx * vx + vy * vy + vz * vz);
		const double cosine = distance > 0.0 ? (forward[0] * vx + forward[1] * vy + forward[2] * vz) / distance : 1.0;
		return strength * DistanceFactor(distance, reach) * FacingFactor(distance, cosine) * strengthScale * doseScale;
	}

	// ---- One frame's flashes, summed ---------------------------------------------------------------------------------------
	struct FrameInput
	{
		double Sum = 0.0;
		double LargestDose = 0.0;
		double LargestRecovery = 1.0;
		double TintSum[3] = { 0.0, 0.0, 0.0 };   // dose x tint, each 0..1
		double TintWeight = 0.0;

		void Add(double dose, double recovery, double r, double g, double b)
		{
			Sum += dose;
			if (dose > LargestDose)
			{
				LargestDose = dose;
				LargestRecovery = recovery;
			}
			TintSum[0] += dose * r;
			TintSum[1] += dose * g;
			TintSum[2] += dose * b;
			TintWeight += dose;
		}
	};

	// ---- The envelope ------------------------------------------------------------------------------------------------------
	struct Envelope
	{
		double T = 0.0;
		double L = 0.0;
		double HoldUntil = 0.0;
		double PeakTime = -1.0e30;    // the last frame L rose
		bool Live = false;
		bool Falling = false;         // L has fallen since it last rose
		bool ReRise = false;          // this rise is A3's: its impulses fill half as fast
		uint64_t Burst = 0;           // counts bursts (onsets); the pass latches the darkness once per burst
		double TintSum[3] = { 0.0, 0.0, 0.0 };
		double TintWeight = 0.0;
		double LargestDose = 0.0;
		double Recovery = 1.0;        // the recovery of the burst's largest single dose
	};

	// Everything back to rest (idle, a new level, the switch off) but the burst count -- it never repeats, so the pass never
	// takes a new burst for an old one -- and the last peak, so A3 still judges a rise just after an idle snap.
	inline void Rest(Envelope& e)
	{
		const uint64_t burst = e.Burst;
		const double peak = e.PeakTime;
		e = Envelope();
		e.Burst = burst;
		e.PeakTime = peak;
	}

	// One frame: `now` in seconds of the real clock, `dt` the step since the last frame, `recoverySeconds` the look's recovery
	// times the player's scale. True on the frame a burst begins.
	inline bool Step(Envelope& e, double now, double dt, const FrameInput& in, const Timing& timing, double recoverySeconds)
	{
		dt = dt > 0.0 ? (dt < DT_MAX ? dt : DT_MAX) : 0.0;
		double sum = in.Sum;
		sum = sum > 0.0 ? (sum < MAX_FRAME_SUM ? sum : MAX_FRAME_SUM) : 0.0;

		bool onset = false;
		if (sum >= MIN_DOSE)
		{
			if (!e.Live)
			{
				Rest(e);
				e.Live = true;
				e.Burst++;
				onset = true;
				e.Falling = true;	// an idle envelope has fallen: A3 judges this rise against the last peak
			}
			if (e.Falling)
			{
				e.ReRise = now - e.PeakTime < RERISE_WINDOW;	// A3
				e.Falling = false;
			}
			double add = 1.0 - std::exp(-sum);
			if (e.ReRise)
				add *= RERISE_SHARE;
			e.T += (1.0 - e.T) * add;
			e.HoldUntil = now + timing.Hold;	// A1
			for (int c = 0; c < 3; c++)
				e.TintSum[c] += in.TintSum[c];
			e.TintWeight += in.TintWeight;
			if (in.LargestDose > e.LargestDose)
			{
				e.LargestDose = in.LargestDose;
				e.Recovery = in.LargestRecovery;
			}
		}
		if (!e.Live)
			return false;

		if (e.L < e.T)
		{
			const double attack = timing.Attack > 1.0e-6 ? timing.Attack : 1.0e-6;
			const double risen = e.L + dt / attack;	// A2
			if (dt > 0.0)
			{
				e.L = risen < e.T ? risen : e.T;
				e.PeakTime = now;
				// A1 from the rise too: a frame on which L rises holds it as an impulse does. A hitch that delays a rise past the
				// impulse's own hold would otherwise let the wash rise, fall and rise again within a few frames.
				if (e.HoldUntil < now + timing.Hold)
					e.HoldUntil = now + timing.Hold;
			}
		}

		if (now >= e.HoldUntil)
		{
			double seconds = recoverySeconds * e.Recovery;
			seconds = seconds > MIN_RECOVERY_SECONDS ? seconds : MIN_RECOVERY_SECONDS;
			e.T *= std::exp(-dt * RECOVERY_E_FOLDS / seconds);
			if (e.L > e.T)
			{
				e.L = e.T;
				e.Falling = true;
				e.ReRise = false;
			}
		}

		if (e.T < IDLE_SNAP && e.L < IDLE_SNAP)
			Rest(e);	// exactly 0: the pass stops drawing
		return onset;
	}

	// The haze's colour for the burst: the dose-weighted mean of its flashes' tints (white with none), Comfort whiter.
	inline void BurstTint(const Envelope& e, bool comfort, double out[3])
	{
		for (int c = 0; c < 3; c++)
		{
			double v = e.TintWeight > 0.0 ? e.TintSum[c] / e.TintWeight : 1.0;
			v = v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0;
			if (comfort)
				v += (1.0 - v) * COMFORT_TINT_TO_WHITE;
			out[c] = v;
		}
	}
}
