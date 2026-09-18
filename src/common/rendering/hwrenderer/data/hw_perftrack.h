/*
** hw_perftrack.h
**
** RS FORK -- perf_track: THE PER-FRAME TELEMETRY RECORD ("Engine docs/TELEMETRY_AND_BENCH_PLAN.md" section 2,
** "Engine docs/TELEMETRY_IMPL_NOTES.md"). The perf log (hw_perflog.cpp) answers "how did this window run"; this
** answers "what happened in frame N". One row a frame: our CPU split, our GPU by group, the headset runtime's
** share, what the frame contained, and -- for every frame over budget -- a reason.
**
** WHY IT IS SEPARATE FROM THE PERF LOG. The perf log keeps a histogram per window and writes a text block every
** N seconds; it cannot answer "which frame stuttered and what ran in it", which is the whole question the
** performance plan (ENGINE_PERFORMANCE_ARCHITECTURE_PLAN.md section 2) needs answered before anything is cut.
** The two share their sources: the same named GPU and CPU groups, the same think clocks, the same governor
** measurement. Nothing is measured twice.
**
** ZERO COST WHEN OFF. perf_track is 0 by default. Off, the whole cost is Active()'s one integer test: no
** timestamp is taken, nothing is allocated, no group is recorded and no file is opened. The frame path and the
** draw stream are then exactly what they are without this file compiled in.
**
** NO DISK IN THE FRAME LOOP. Rows are kept in memory and the CSV is written when the run ends (perf_track back
** to 0, a map change, a flush, or the engine quitting). A recorder that writes a line every frame measures its
** own file I/O; this one does not touch the disk between the first frame and the last.
**
** WHERE IT WRITES. perf_track_dir when set, else the -savedir this run was given, else the save folder. Never
** beside the exe, never the owner's ini and never their logs.
**
** MEASUREMENT ONLY. Nothing here reads or writes playsim state; d_main.cpp hands in the read-only scene numbers
** it can see, exactly as it does for the perf log. Netplay is untouched: no RNG, nothing keyed on a player.
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

#pragma once

#include <cstdint>

#include "c_cvars.h"
#include "i_time.h"
#include "zstring.h"

EXTERN_CVAR(Int, perf_track)

namespace PerfTrack
{
	// THE ONE TEST. Off, this is the whole per-frame cost. A caller reads it ONCE and uses the same value at the
	// start and at the end of a timed region, so a cvar change between the two cannot unbalance anything.
	inline bool Active() { return *perf_track > 0; }

	// ---- the frame's named group times, forwarded by the perf log (hw_perflog.cpp) --------------------------
	// Same names, same samples: "scene.*", "fx.*", the post-process names, and the CPU-side effect timings.
	// Same-name samples in one frame (the stereo eyes) are summed, as the perf log sums them.
	void NoteGpuGroup(const char* name, double ms);
	void NoteCpuGroup(const char* name, double ms);

	// ---- the playsim's tic runs, forwarded by the perf log (FThinkerCollection, dthinker.cpp) ---------------
	// One call per tic run: the run's think clock and the thinkers it ticked. Summed into the frame the run
	// falls in, so a frame that ran three tics carries all three.
	void NoteThink(bool clientSide, double ms, int thinkers);

	// ---- one playsim tic ran (d_net.cpp TryRunTics, after ++gametic) ---------------------------------------
	void NoteTic();

	// HITCH REASONS. A frame over budget gets a marked row and the reason that took the most time in it. The
	// explicit ones are timed where they happen (Scope below); the rest are read off numbers the frame already
	// carries (the think clock for a script spike, the governor's pacing for a runtime wait, the shadow map's
	// own clock for a light rebuild, a map change for level geometry).
	enum EReason
	{
		REASON_NONE = -1,
		REASON_PIPELINE = 0,	// a graphics pipeline was compiled mid-frame (vk_renderpass.cpp, a cache miss)
		REASON_TEXTURE,			// a texture or material was uploaded (vk_hwtexture.cpp)
		REASON_BUFFER,			// a GPU buffer was grown or recreated
		REASON_GC,				// the VM's garbage collector ran (d_net.cpp, around GC::CheckGC)
		REASON_SHADOW,			// the shadow map was rebuilt (IShadowMap's own clock)
		REASON_GEOMETRY,		// the level changed under the frame
		REASON_SCRIPT,			// the tic's think clock took most of the frame
		REASON_RUNTIME,			// the frame was held by the headset runtime, not by us
		REASON_COUNT
	};
	const char* ReasonName(int reason);

	// Adds `ms` to this frame's total for `reason`. Safe from any thread (the texture thread uploads).
	void NoteReason(int reason, double ms);

	// A timed region that costs one integer test while perf_track is off: no clock is read, nothing is added.
	// `Active()` is sampled once, in the constructor, and used again in the destructor.
	class Scope
	{
	public:
		explicit Scope(int reason) : mReason(reason), mOn(Active()), mStartNs(mOn ? I_nsTime() : 0) {}
		~Scope()
		{
			if (mOn)
				NoteReason(mReason, (I_nsTime() - mStartNs) / 1e6);
		}
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		int mReason;
		bool mOn;
		uint64_t mStartNs;
	};

	// THE HEADSET RUNTIME'S SHARE, from OpenXR only (vk_openxrdevice.cpp). Never by hooking another process:
	// our own timing around the frame calls, the shared clock of
	// XR_KHR_win32_convert_performance_counter_time, and XR_META_performance_metrics where the runtime
	// advertises it. Every field is 0 when the runtime does not offer it.
	struct XrFrameInfo
	{
		double WaitMs = 0.0;			// held inside xrWaitFrame: the runtime saying the downstream is busy
		double AcquireMs = 0.0;			// xrAcquireSwapchainImage
		double ImageWaitMs = 0.0;		// xrWaitSwapchainImage: swapchain backpressure
		double EndMs = 0.0;				// our submit to xrEndFrame
		double PeriodMs = 0.0;			// XrFrameState::predictedDisplayPeriod
		double AheadMs = 0.0;			// predicted display time less our clock at xrWaitFrame's return, on the
										// shared clock: how far ahead the frame we are drawing is
		int PeriodsAhead = 0;			// AheadMs in whole display periods (2 is the usual pipeline depth)
		bool HaveSharedClock = false;	// XR_KHR_win32_convert_performance_counter_time answered
		bool HaveMetrics = false;		// XR_META_performance_metrics answered
		double AppGpuMs = 0.0;			// METRICS: the runtime's own measure of OUR GPU time
		double CompositorGpuMs = 0.0;	// METRICS: the compositor's GPU time
		double CompositorCpuMs = 0.0;	// METRICS: the compositor's CPU time
		float CompositorFps = 0.0f;		// METRICS: the compositor's frame rate
		int AppDropped = 0;				// METRICS: our dropped frames, running total
		int CompositorDropped = 0;		// METRICS: the compositor's dropped frames, running total
		int StaleFrames = 0;			// METRICS: frames the compositor reprojected instead of taking a new one
	};
	// Once a frame from the OpenXR device, before the frame ends. Every field is this frame's.
	void NoteXrFrame(const XrFrameInfo& xr);

	// WHAT THE FRAME CONTAINED. Filled by d_main.cpp, which can see FLevelLocals and the viewpoint; this file
	// cannot. Read-only: the same numbers the perf log's SceneLoad carries, plus where the view was.
	struct SceneShape
	{
		const char* MapName = nullptr;
		int Tic = 0;					// the level's maptime
		double X = 0.0, Y = 0.0, Z = 0.0;
		double Angle = 0.0, Pitch = 0.0;
		uint64_t GpuParticlesWritten = 0;	// FLevelLocals::GpuParticleWritten (running total)
		int BeamsLive = 0;
		int StampsLive = 0;
		int DisturbLive = 0;
	};

	// Once a frame, in d_main.cpp's End2DAndUpdate, after the governor measured the frame and beside the perf
	// log. Only call while Active().
	void EndFrame(const SceneShape& shape);

	// THE RUN HEADER, shared with the capture and the replay so two runs can be proven comparable: the engine
	// build, the exe's timestamp, the load order with each file's size and a cheap hash, every cvar that differs
	// from its default, and the map. Appended as "# key=value" lines.
	void AppendRunHeader(FString& out);

	// The folder this run writes into: perf_track_dir when set, else -savedir, else the save folder. Always ends
	// in a separator; never the exe's folder.
	FString OutputDir();

	// Writes the CSV and the summary for everything recorded so far and starts again. Called when perf_track
	// goes back to 0, when the map changes, from the perf_track_flush command, and by the replay when it quits.
	// `why` names the reason in the summary. Does nothing when nothing was recorded.
	void Flush(const char* why);
}
