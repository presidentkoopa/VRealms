/*
** hw_effectsgovernor.cpp
**
** [GOVERNOR] E8: the effects budget governor's cvars, its once-a-frame measurement and the readers' calls. See
** hw_effectsgovernor.h; the decision itself is EffectsGovernorCore::Controller there.
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

#include "hw_effectsgovernor.h"

#include <atomic>

#include "c_cvars.h"
#include "i_time.h"
#include "v_video.h"

EXTERN_CVAR(Int, vid_maxfps)	// v_video.cpp: the desktop budget when it caps below 60

// Set whenever r_effects_governor changes: the next EndFrame starts again from full with nothing learned. Only a bool, so the
// callback is safe at any point of startup.
static bool GovernorRestart = true;

// [GOVERNOR] E8: r_effects_governor -- the effects budget governor. On (the owner's choice, 2026-09-15): while the slowest frames
// of the last second pass r_effects_governor_target of the display's frame time, effect detail steps down one trim at a time,
// never past r_effects_governor_floor, and back up a trim a second when there is room. It writes no cvar: off is exactly the
// owner's settings, the same frame. Renderer-read every frame. Vulkan only (elsewhere nothing is trimmed).
CUSTOM_CVARD(Bool, r_effects_governor, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "effects budget governor: while frames run late, trim effect detail one step at a time, never past r_effects_governor_floor, and restore it when there is room (Vulkan only)")
{
	GovernorRestart = true;
}

// [GOVERNOR] E8: r_effects_governor_floor -- the deepest rung the governor may use (hw_effectsgovernor.h THE RUNGS): 0 never trims,
// 5 (default) every effect at 3/4 at most, 10 every effect at 1/2 at most.
CUSTOM_CVARD(Int, r_effects_governor_floor, EffectsGovernorCore::FLOOR_DEFAULT, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "the furthest the effects budget governor may trim, 0-10: 0 never, 5 every effect at 3/4 (default), 10 every effect at 1/2")
{
	if (self < 0) self = 0;
	else if (self > EffectsGovernorCore::RUNG_DEEPEST) self = EffectsGovernorCore::RUNG_DEEPEST;
}

// [GOVERNOR] E8: r_effects_governor_target -- the governor trims when the p95 frame passes this percentage of one display period,
// and steps back up under 15 points less. 90 by default: 10.0 ms of a 90 Hz headset's 11.1 ms.
CUSTOM_CVARD(Int, r_effects_governor_target, EffectsGovernorCore::TARGET_DEFAULT, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "the effects budget governor trims when its slowest frames pass this percentage of the display's frame time, 70-100")
{
	if (self < 70) self = 70;
	else if (self > 100) self = 100;
}

namespace
{
	EffectsGovernorCore::Controller Governor;
	std::atomic<int> GovernorRung{ 0 };			// the controller's rung, for the readers (scene workers and the playsim read it)
	std::atomic<uint64_t> PacingNs{ 0 };		// blocked on the display since the last EndFrame
	std::atomic<int64_t> DisplayPeriodNs{ 0 };	// the headset's display period since the last EndFrame, 0 = none
	uint64_t LastFrameNs = 0;
	uint64_t LastXrNs = 0;
	double LastXrPeriodMs = 0.0;
	EffectsGovernor::FrameInfo Last;
}

void EffectsGovernor::AddPacingWait(uint64_t ns)
{
	PacingNs.fetch_add(ns, std::memory_order_relaxed);
}

void EffectsGovernor::NoteDisplayPeriod(int64_t ns)
{
	if (ns > 0)
		DisplayPeriodNs.store(ns, std::memory_order_relaxed);
}

void EffectsGovernor::EndFrame()
{
	const uint64_t now = I_nsTime();
	const uint64_t pacingNs = PacingNs.exchange(0, std::memory_order_relaxed);
	const int64_t periodNs = DisplayPeriodNs.exchange(0, std::memory_order_relaxed);

	if (GovernorRestart)
	{
		GovernorRestart = false;
		Governor.Reset();
		GovernorRung.store(0, std::memory_order_relaxed);
		LastFrameNs = 0;
	}
	if (periodNs > 0)
	{
		LastXrPeriodMs = periodNs / 1e6;
		LastXrNs = now;
	}

	FrameInfo info;
	if (LastFrameNs == 0 || now <= LastFrameNs)
	{
		// The first frame only starts the clock.
		LastFrameNs = now;
		info.Rung = ActiveRung();
		Last = info;
		return;
	}

	info.IntervalMs = (now - LastFrameNs) / 1e6;
	LastFrameNs = now;
	info.Xr = LastXrNs != 0 && now - LastXrNs < 1000000000ull;
	info.PacingMs = pacingNs / 1e6;
	info.CostMs = std::max(0.0, info.IntervalMs - info.PacingMs);
	info.BudgetMs = EffectsGovernorCore::BudgetMs(info.Xr ? LastXrPeriodMs : 0.0, *vid_maxfps);

	// Only a renderer whose pacing waits are handed in is measured: the Vulkan backend (and its OpenXR device). On another, a
	// vsync wait would read as work.
	const bool measurable = screen != nullptr && screen->IsVulkan();
	info.Valid = measurable && info.IntervalMs <= Governor.P.HitchMs;

	const int before = Governor.Rung();
	Governor.Frame(info.CostMs, info.IntervalMs, info.BudgetMs, *r_effects_governor && measurable, *r_effects_governor_floor, *r_effects_governor_target);
	GovernorRung.store(Governor.Rung(), std::memory_order_relaxed);
	info.Changed = Governor.Rung() != before;
	info.Rung = ActiveRung();
	Last = info;
}

const EffectsGovernor::FrameInfo& EffectsGovernor::LastFrame()
{
	return Last;
}

// The cvars are read at every call, so turning the governor off or raising the floor acts on the very next reader.
int EffectsGovernor::ActiveRung()
{
	return EffectsGovernorCore::ActiveRung(*r_effects_governor, GovernorRung.load(std::memory_order_relaxed), *r_effects_governor_floor);
}

int EffectsGovernor::SmokeSteps(int ownerSteps)
{
	return EffectsGovernorCore::SmokeSteps(ActiveRung(), ownerSteps);
}

int EffectsGovernor::EmissiveSteps(int ownerSteps)
{
	return EffectsGovernorCore::EmissiveSteps(ActiveRung(), ownerSteps);
}

int EffectsGovernor::EmissiveDrawn(int drawnMax)
{
	return EffectsGovernorCore::EmissiveDrawn(ActiveRung(), drawnMax);
}

int EffectsGovernor::LightsPerBin(int ownerPerBin)
{
	return EffectsGovernorCore::LightsPerBin(ActiveRung(), ownerPerBin);
}

uint32_t EffectsGovernorParticleKeep()
{
	return EffectsGovernorCore::ParticleKeep(EffectsGovernor::ActiveRung());
}
