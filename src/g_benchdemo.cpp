/*
** g_benchdemo.cpp
**
** RS FORK -- -benchdemo: THE UNATTENDED REPLAY ("Engine docs/TELEMETRY_AND_BENCH_PLAN.md" section 4,
** "Engine docs/TELEMETRY_IMPL_NOTES.md").
**
** WHAT IT IS, AND WHAT IT IS NOT. -benchdemo <name> is `playdemo <name>` with the per-frame record turned on
** and a quit at the end. That is all it is. There is no second playback path and no second capture format: the
** demo system plays the demo, the VR demo frame (p_vrdemo.cpp) feeds the hands and the body so the reload rigs
** and gestures run as they did, g_perfbench.cpp compares each tic's state hash, and perf_track
** (hw_perftrack.cpp) records every frame.
**
** THE OWNER'S OWN PLAYBACK IS MEASURED THE SAME WAY. Everything that lands on disk -- the per-frame CSV, the
** summary with its verdict, the two run headers -- is written by PerfBench::EndPlayback at the end of ANY
** playback while perf_track is on, so a demo the owner is WATCHING in the headset produces exactly the same
** files as an unattended run. They see the fight; the numbers from that same viewing are on disk when it ends.
** This file exists only to start the thing unattended, pick the pacing, and quit at the end.
**
** TWO MODES (-benchmode locked | unlocked, locked by default). BOTH run the same fight over the same wall
** clock: the playsim advances at its own rate either way, because changing that would change what happens.
**   locked    the frame rate cap and vsync as configured -- the pacing a player would feel, for HITCH SHAPE.
**   unlocked  the cap and vsync off -- every frame the machine can draw, for THROUGHPUT and for straight
**             before/after comparisons where pacing would otherwise hide the difference.
** Neither fast-forwards the playsim. A mode that did would have to drive tics from the render loop, which is a
** change to the playsim's own scheduling and belongs with the performance plan's phases, not with the tool that
** measures them.
**
** THE RUNNER. tools/bench_replay/bench_replay.ps1 starts it the way tools/main_boottest/main_boottest.ps1
** starts a boot test: a hidden desktop, a scratch copy of the ini with vr_mode 0, a scratch working directory
** and savedir, the owner's ini and logs guarded, and a kill on timeout. Compare two runs with
** tools/bench_compare.py.
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

#include "g_perfbench.h"

#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "doomstat.h"
#include "g_game.h"
#include "printf.h"

EXTERN_CVAR(Bool, cl_capfps)
EXTERN_CVAR(Bool, vid_vsync)
EXTERN_CVAR(Int, vid_maxfps)
EXTERN_CVAR(Int, perf_track)

namespace
{
	bool Running = false;
	bool Done = false;
	FString DemoName;
	FString ModeName;
}

bool BenchDemo::Active()
{
	return Running;
}

void BenchDemo::Begin(const char* demoName, const char* mode)
{
	if (demoName == nullptr || *demoName == 0)
		return;

	DemoName = demoName;
	ModeName = (mode != nullptr && stricmp(mode, "unlocked") == 0) ? "unlocked" : "locked";
	if (ModeName.Compare("unlocked") == 0)
	{
		cl_capfps = false;
		vid_vsync = false;
		vid_maxfps = 0;
	}

	// The record is what this mode exists to produce. Everything it writes is written by
	// PerfBench::EndPlayback when the demo ends -- the same path a watched playback takes.
	perf_track = 1;

	Running = true;
	Done = false;
	Printf("-benchdemo: replaying %s, %s pacing, with the per-frame record on.\n",
		DemoName.GetChars(), ModeName.GetChars());
	G_DeferedPlayDemo(DemoName.GetChars());
}

void BenchDemo::Finish()
{
	if (!Running || Done)
		return;
	Done = true;
	Running = false;
	// The CSV, the summary and the two headers are already written, and the verdict already printed, by
	// PerfBench::EndPlayback earlier in G_CheckDemoStatus. All that is left is to stop -- the only thing an
	// unattended run needs that a watched one does not.
	Printf("-benchdemo: %s finished (%s pacing); quitting.\n", DemoName.GetChars(), ModeName.GetChars());
	AddCommandString("quit");
}
