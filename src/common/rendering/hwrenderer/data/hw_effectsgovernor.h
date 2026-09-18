/*
** hw_effectsgovernor.h
**
** [GOVERNOR] E8: THE EFFECTS BUDGET GOVERNOR ("Engine docs/EFFECTS_OPTIMIZATION_PLAN.md" E8, ENGINE_SUPPORT_LIST #4,
** "Engine docs/EFFECTS_GOVERNOR_E8_IMPL_NOTES.md").
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Cruise control for frame time. While the slowest frames of the last second come close to the display's frame time, effect
** detail steps down one trim at a time; when there is room again it steps back up, one trim a second. It never goes past a floor
** the player sets (r_effects_governor_floor) and never touches gameplay.
**
** THE COST OF A FRAME. The time from one frame's end to the next, less the time the frame spent blocked waiting for the display:
** xrWaitFrame and the headset swapchain's wait (vk_openxrdevice.cpp), a swapchain's acquire and present (vk_framebuffer.cpp) and
** the fps limiter's sleep (v_framebuffer.cpp) hand that time in as PACING (AddPacingWait). What is left is the frame's work, and
** it holds the GPU's: every frame waits for its command buffers to finish (VkCommandBufferManager::WaitForCommands' fence wait)
** before the next one starts. It is not the perf log's GPU timestamp groups: those run only while r_perflog or "stat gpu" is on,
** so a governor reading them would act differently in a logged run than in the owner's unlogged play, and a sum of groups misses
** the CPU half of a frame whose CPU and GPU work take their turns.
**
** THE BUDGET. One display period: the headset's own (XrFrameState::predictedDisplayPeriod, handed in each frame by xrWaitFrame:
** 11.1 ms at 90 Hz), and without a headset frame in the last second 1/60 s, or 1/vid_maxfps when the cap is below 60.
**
** THE DECISION (Controller::Frame, once a frame from d_main.cpp's End2DAndUpdate). The p95 of the frames' cost over the last
** second (only frames at the current rung: every step starts the window again):
**   - over target% of the budget (r_effects_governor_target, 90) after a quarter second at the rung: one rung down;
**   - under (target - 15)% after a full second at the rung, AND the cost one rung up, predicted, still clear of the step-down
**     line: one rung up. The prediction is the p95 times the cost ratio that rung was last seen to save (measured at each step
**     down, and at a bounce), so a rung that saves a lot is not taken back while that would cross the line again.
**   - A step up that has to come back down within 3 s is a BOUNCE: that boundary's next marginal try waits twice as long (up to
**     32 s), and halves again after a step up that holds. With clear room (the prediction under the step-up line) the wait is
**     always one second, so it returns to full as soon as the load drops.
**   - A frame over 250 ms (a load, a save, a stall) is not effect load: the window starts again. A new display rate starts the
**     window again too; what the rungs were seen to save carries over.
**
** THE RUNGS, in the plan's order, twice: a light trim of every knob, then a deeper one. Each rung adds its trim to the ones above.
**   1  smoke detail 3/4             r_smoke_steps x 3/4 (never under 16)                 hw_drawinfo.cpp SetupSmokeVolume
**   2  flash detail 3/4             r_emissivevolumes_steps x 3/4 (never under 8)        hw_drawinfo.cpp SetupEmissiveVolumes
**   3  24 flashes drawn             EMISSIVE_VOLUMES_DRAWN_MAX (32) x 3/4                hw_emissivevolumes.cpp, the best by rank
**   4  particles 3/4                SpawnParticles keeps 3/4 of a burst                  g_levellocals.h, after the hash
**   5  effect lights per box 3/4    r_effectlights_perbin x 3/4 (never under 2)          hw_effectlights.cpp
**   6-10  the same five at 1/2.
** THE FLOOR (r_effects_governor_floor, default 5): the deepest rung it may use. 5 lets every effect lose at most a quarter while
** frames run late, which keeps the look; 10 lets it halve them; 0 never trims.
**
** NEVER THE OWNER'S SETTINGS. The governor writes no cvar. Each reader passes the owner's value through one call here and uses
** what comes back: exactly that value while r_effects_governor is off or the rung is 0, so an ini, a preset (r_smoke_preset) and
** a menu row are untouched, and turning the governor off restores the owner's settings the same frame.
**
** NETPLAY. Presentation only. What a machine draws depends on its own frames; no playsim state is read or written (the particle
** scale writes a free ring slot where a skipped particle would have gone, so the ring cursor and every later seed are unchanged:
** see SpawnParticles), no playsim RNG is used, and nothing is keyed on a player.
**
** The controller (EffectsGovernorCore) includes nothing from the engine, so a CPU harness can run it on synthetic frame traces.
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace EffectsGovernorCore
{
	inline constexpr int RUNG_DEEPEST = 10;
	inline constexpr int FLOOR_DEFAULT = 5;
	inline constexpr int TARGET_DEFAULT = 90;
	inline constexpr uint32_t PARTICLE_KEEP_ALL = 65536u;	// SpawnParticles' scale, of 65536

	// One knob at a rung: the owner's value before `threeQuarterAt`, three quarters of it from there, half of it from `halfAt`.
	// Never under `minimum` (the reader's own lowest value) and never above the owner's value.
	inline int Trim(int owner, int rung, int threeQuarterAt, int halfAt, int minimum)
	{
		int value = owner;
		if (rung >= halfAt)
			value = owner / 2;
		else if (rung >= threeQuarterAt)
			value = owner - owner / 4;
		return std::min(owner, std::max(value, minimum));
	}

	inline int SmokeSteps(int rung, int owner) { return Trim(owner, rung, 1, 6, 16); }
	inline int EmissiveSteps(int rung, int owner) { return Trim(owner, rung, 2, 7, 8); }
	inline int EmissiveDrawn(int rung, int drawnMax) { return Trim(drawnMax, rung, 3, 8, 1); }
	inline uint32_t ParticleKeep(int rung) { return (uint32_t)Trim((int)PARTICLE_KEEP_ALL, rung, 4, 9, 0); }
	inline int LightsPerBin(int rung, int owner) { return Trim(owner, rung, 5, 10, 2); }

	// The rung the readers use: 0 while the governor is off, never past the floor.
	inline int ActiveRung(bool enabled, int rung, int floor)
	{
		if (!enabled)
			return 0;
		return std::clamp(rung, 0, std::clamp(floor, 0, RUNG_DEEPEST));
	}

	// One display period in ms: the headset's when it gave one, else 1/60 s (1/vid_maxfps when that cap is below 60).
	inline double BudgetMs(double displayPeriodMs, int maxFps)
	{
		if (displayPeriodMs >= 4.0 && displayPeriodMs <= 50.0)
			return displayPeriodMs;
		const int hz = (maxFps > 0 && maxFps < 60) ? std::max(maxFps, 20) : 60;
		return 1000.0 / hz;
	}

	struct Params
	{
		double GapPct = 15.0;			// a step up needs the p95 under (target - GapPct)% of the budget
		double MarginPct = 3.0;			// and a marginal step up needs its predicted cost this far under the step-down line
		double DownWindowS = 0.25;		// time at a rung before a step down is judged
		double WindowS = 1.0;			// the p95 looks back this far (and a step up needs this long at the rung)
		double UpHoldS = 1.0;			// at most one rung a second going up
		double BounceS = 3.0;			// coming back down this soon after a step up makes that step up a bounce
		int BackoffMax = 32;			// a bounced boundary's marginal tries wait up to this many UpHoldS
		double RatioHalfLifeS = 15.0;	// a learned saving fades toward none with this half-life, times its boundary's backoff
		double RatioMax = 2.0;			// the most one rung is believed to save: half the frame
		double HitchMs = 250.0;			// a frame this long is a load, a save or a stall: the window starts again
	};

	class Controller
	{
	public:
		static constexpr int WINDOW = 512;		// frames kept: a second at up to 500 Hz

		Params P;

		Controller() { Reset(); }

		int Rung() const { return mRung; }
		double LastP95() const { return mLastP95; }

		void Reset()
		{
			mRung = 0;
			mBudget = 0.0;
			for (int r = 0; r < RUNG_DEEPEST; r++)
			{
				mRatio[r] = 1.0;
				mBackoff[r] = 1;
			}
			mSinceChangeS = 0.0;
			mLastUpTo = -1;
			mUpFromP95 = 0.0;
			mPendingFrom = -1;
			mPendingBefore = 0.0;
			mLastP95 = 0.0;
			Restart();
		}

		// One frame: its cost and interval (ms), one display period (ms), and the settings as they stand. Off, the rung is 0 and
		// everything learned is forgotten. Returns true when the rung changed.
		bool Frame(double costMs, double intervalMs, double budgetMs, bool enabled, int floor, int targetPct)
		{
			if (!enabled)
			{
				const bool wasTrimming = mRung != 0;
				Reset();
				return wasTrimming;
			}
			if (!(budgetMs > 0.0))
				return false;

			bool changed = false;
			floor = std::clamp(floor, 0, RUNG_DEEPEST);
			const double target = (double)std::clamp(targetPct, 50, 100);

			if (mBudget <= 0.0 || std::fabs(budgetMs - mBudget) > mBudget * 0.02)
			{
				mBudget = budgetMs;		// a new display rate: the frames judged against the old lines go
				Restart();
			}
			if (mRung > floor)
			{
				ChangeTo(floor);		// the floor was raised past the rung
				changed = true;
			}
			if (!(intervalMs >= 0.0) || intervalMs > P.HitchMs || !(costMs >= 0.0))
			{
				Restart();
				return changed;
			}

			mCost[mHead] = costMs;
			mInterval[mHead] = intervalMs;
			mHead = (mHead + 1) % WINDOW;
			if (mCount < WINDOW)
				mCount++;
			const double dt = intervalMs / 1000.0;
			mWindowS += dt;
			mSinceChangeS += dt;
			for (int r = 0; r < RUNG_DEEPEST; r++)
			{
				if (mRatio[r] > 1.0)
					mRatio[r] = 1.0 + (mRatio[r] - 1.0) * std::pow(0.5, dt / (P.RatioHalfLifeS * mBackoff[r]));
			}

			// A step up that held: its boundary tries sooner next time.
			if (mLastUpTo >= 0 && mSinceChangeS >= P.BounceS)
			{
				mBackoff[mLastUpTo] = std::max(1, mBackoff[mLastUpTo] / 2);
				mLastUpTo = -1;
			}

			if (mWindowS < P.DownWindowS || mCount < 4)
				return changed;

			const double p = P95(P.WindowS);
			mLastP95 = p;
			const double downLine = mBudget * target / 100.0;
			const double upLine = mBudget * (target - P.GapPct) / 100.0;

			// The first judgement after a step down: what that rung saved, as a ratio of costs.
			if (mPendingFrom >= 0)
			{
				if (p > 0.0)
					mRatio[mPendingFrom] = std::max(mRatio[mPendingFrom], std::clamp(mPendingBefore / p, 1.0, P.RatioMax));
				mPendingFrom = -1;
			}

			if (p > downLine && mRung < floor)
			{
				if (mLastUpTo == mRung && mSinceChangeS < P.BounceS)
				{
					// A bounce: the step up to this rung did not hold.
					mBackoff[mRung] = std::min(P.BackoffMax, mBackoff[mRung] * 2);
					if (mUpFromP95 > 0.0)
						mRatio[mRung] = std::max(mRatio[mRung], std::clamp(p / mUpFromP95, 1.0, P.RatioMax));
				}
				const int from = mRung;
				ChangeTo(mRung + 1);
				mPendingFrom = from;
				mPendingBefore = p;
				return true;
			}

			if (mRung > 0 && mWindowS >= P.WindowS && mSinceChangeS >= P.UpHoldS && p < upLine)
			{
				const int boundary = mRung - 1;
				const double predicted = p * mRatio[boundary];
				const bool clearRoom = predicted < upLine;
				const bool marginal = predicted < downLine * (1.0 - P.MarginPct / 100.0) && mSinceChangeS >= P.UpHoldS * mBackoff[boundary];
				if (clearRoom || marginal)
				{
					mUpFromP95 = p;
					ChangeTo(boundary);
					mLastUpTo = boundary;
					return true;
				}
			}
			return changed;
		}

	private:
		void Restart()
		{
			mHead = 0;
			mCount = 0;
			mWindowS = 0.0;
		}

		void ChangeTo(int rung)
		{
			mRung = rung;
			mSinceChangeS = 0.0;
			mLastUpTo = -1;
			mPendingFrom = -1;
			Restart();
		}

		// The p95 of the newest frames covering `windowS` (or all the window holds).
		double P95(double windowS)
		{
			int n = 0;
			double covered = 0.0;
			while (n < mCount && covered < windowS * 1000.0)
			{
				const int index = (mHead - 1 - n + WINDOW) % WINDOW;
				mScratch[n] = mCost[index];
				covered += mInterval[index];
				n++;
			}
			if (n == 0)
				return 0.0;
			const int k = std::max(0, (int)std::ceil(n * 0.95) - 1);
			std::nth_element(mScratch, mScratch + k, mScratch + n);
			return mScratch[k];
		}

		int mRung = 0;
		double mBudget = 0.0;
		double mRatio[RUNG_DEEPEST] = {};		// boundary r (rung r to r + 1): the cost at r over the cost at r + 1, as last seen
		int mBackoff[RUNG_DEEPEST] = {};
		double mSinceChangeS = 0.0;
		int mLastUpTo = -1;						// the rung the last step up went to, until it holds BounceS
		double mUpFromP95 = 0.0;
		int mPendingFrom = -1;					// a step down from this rung, not yet measured
		double mPendingBefore = 0.0;
		double mLastP95 = 0.0;

		double mCost[WINDOW] = {};
		double mInterval[WINDOW] = {};
		double mScratch[WINDOW] = {};
		int mHead = 0;
		int mCount = 0;
		double mWindowS = 0.0;
	};
}

namespace EffectsGovernor
{
	// PACING: time this frame spent blocked waiting for the display, not working. Handed in by the calls that block; any thread.
	void AddPacingWait(uint64_t ns);
	// The headset's display period this frame (XrFrameState::predictedDisplayPeriod, ns), from xrWaitFrame.
	void NoteDisplayPeriod(int64_t ns);
	// Once a frame, after screen->Update() (d_main.cpp End2DAndUpdate) and before the perf log: measures the frame and decides.
	void EndFrame();

	// THE READERS. Each takes the owner's value as its reader clamps it and returns the value for this frame: exactly the owner's
	// while r_effects_governor is off or the rung is 0.
	int ActiveRung();
	int SmokeSteps(int ownerSteps);			// hw_drawinfo.cpp SetupSmokeVolume: r_smoke_steps
	int EmissiveSteps(int ownerSteps);		// hw_drawinfo.cpp SetupEmissiveVolumes: r_emissivevolumes_steps
	int EmissiveDrawn(int drawnMax);		// hw_emissivevolumes.cpp: EMISSIVE_VOLUMES_DRAWN_MAX
	int LightsPerBin(int ownerPerBin);		// hw_effectlights.cpp: r_effectlights_perbin

	// The last EndFrame, for the perf log.
	struct FrameInfo
	{
		bool Valid = false;			// a measured frame (not the first, not a hitch, Vulkan)
		bool Xr = false;			// the budget is a headset's display period
		double IntervalMs = 0.0;	// since the frame before
		double PacingMs = 0.0;		// of it, blocked on the display
		double CostMs = 0.0;		// the rest: the frame's work
		double BudgetMs = 0.0;
		int Rung = 0;				// ActiveRung after this frame's decision
		bool Changed = false;
	};
	const FrameInfo& LastFrame();
}

// [GOVERNOR] E8: the particle count scale, of 65536 (65536 keeps every particle), for FLevelLocals::SpawnParticles -- declared
// again where it is used, as g_levellocals.h declares DebrisPoolTakes.
uint32_t EffectsGovernorParticleKeep();
