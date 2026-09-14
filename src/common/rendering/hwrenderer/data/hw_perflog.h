/*
** hw_perflog.h
**
** RS FORK -- the render performance log (r_perflog).
**
** The flat-HUD stats ("stat gpu", "stat rendertimes") cannot be read in a
** headset and only ever show one frame. r_perflog N > 0 accumulates GPU pass
** timings, CPU frame timings and the effect load over an N-second window and
** appends one text block per window to perflog.txt, beside benchmarks.txt, so
** a headset run can be read afterwards and two runs compared.
**
** Renderer-agnostic: the backends feed it (VkCommandBufferManager::UpdateGpuStats
** feeds GPU group times; GL could feed gl_debug.cpp's later), d_main.cpp calls
** EndFrame once per frame with the level's effect load. Render-side and local
** only -- nothing here touches playsim state.
**
** Named GPU groups are the contract: "scene.*" and "fx.*" around the main view's
** passes (hw_drawinfo.cpp), the post-process names (hw_postprocess.cpp), and any
** future effects system adds its own "fx.*" group.
**
**---------------------------------------------------------------------------
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include <cstdint>
#include "c_cvars.h"

EXTERN_CVAR(Int, r_perflog)

// Defined in hw_postprocess.cpp: true for the frame while "stat gpu" (or
// r_perflog) keeps GPU timestamps on.
extern bool gpuStatActive;

namespace PerfLog
{
	// The level-side load for one frame. Filled by d_main.cpp, which can see
	// FLevelLocals; this file cannot.
	struct SceneLoad
	{
		const char* MapName = nullptr;
		uint64_t GpuParticlesWritten = 0;	// FLevelLocals::GpuParticleWritten (running total)
		int BeamsLive = 0;					// beam slots that draw (BeamSlotLive)
		int StampsLive = 0;					// StampLife > 0
		int DisturbLive = 0;				// fog disturbances inside their life
	};

	// r_perflog: true when the scene/effects GPU groups should be pushed. Also
	// true while "stat gpu" is shown, as the post-process groups already are.
	// Callers read this ONCE per function and use the same value at Push and at
	// Pop -- an unbalanced group corrupts the timestamp indices.
	inline bool GroupsWanted() { return *r_perflog > 0 || gpuStatActive; }

	// One finished GPU group for the frame just completed. Same-name groups in
	// one frame (stereo eyes) are summed into that frame's value.
	void AddGpuSample(const char* name, double ms);

	// [2d] One CPU-side timing for the frame, by name -- work an effect does on the CPU
	// that no GPU group can time (first: "fx.viewlights", the view light fill). Same-name
	// samples in one frame are summed, like GPU groups; each window writes them on a
	// "cpu_fx_ms" line. Callers time only while GroupsWanted().
	void AddCpuSample(const char* name, double ms);

	// Once per frame, after screen->Update(). Only call while r_perflog > 0.
	void EndFrame(const SceneLoad& load);
}
