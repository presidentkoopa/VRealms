/*
** hw_perflog.cpp
**
** RS FORK -- the render performance log (r_perflog). See hw_perflog.h.
**
** One block per window, appended to perflog.txt in the working directory --
** the same place CheckBench (hw_clock.cpp) writes benchmarks.txt:
**
**   perflog map=MAP01 [label: x] r_gpuparticles=1 r_beams_drawn=0 vr_menu_keep_world=1 t=312.0s window=5.0s frames=450 fps=90.0 frame_ms avg=11.10 p95=11.90 max=14.20
**   cpu_ms  Scene=3.10/3.60/4.02 Post=... (avg/p95/max)
**   gpu_ms  scene.opaque=2.10/2.40/2.60 ... (avg/p95/max)
**   load    particles_spawned=250000 drawnlines_live=4 beams=6 stamps_live=11 disturb_live=3 dlights=38/52 sprites=140 walls=900 flats=410
**
** Off (r_perflog 0) the only per-frame cost is d_main.cpp's integer check.
**
**---------------------------------------------------------------------------
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "hw_perflog.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "c_dispatch.h"
#include "printf.h"
#include "i_time.h"
#include "i_system.h"
#include "v_video.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_drawnlinebuffer.h"

extern bool keepGpuStatActive;	// hw_postprocess.cpp
EXTERN_CVAR(Int, r_gpuparticles_looks)	// [LOOKS] hw_particledefbuffer.cpp

// Set whenever r_perflog changes: the next EndFrame starts a new session
// (fresh window, fresh header). Only a bool, so the cvar callback is safe to
// run at any point of startup.
static bool PerfLogRestart = true;

CUSTOM_CVARD(Int, r_perflog, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "write render timings and effect load to perflog.txt every N seconds (0 = off)")
{
	if (self < 0) { self = 0; return; }
	if (self > 600) { self = 600; return; }
	PerfLogRestart = true;
}

namespace
{
	// Geometric histogram for p95: bin 0 holds <= 0.01 ms, each later bin is 2%
	// wider than the one before, so every value from a cheap post pass to a
	// multi-second hitch lands within 2% of its bin edge. 600 bins reach ~1.4 s;
	// anything longer shares the last bin and p95 is clamped to the max.
	const int HistBins = 600;
	const double HistBaseMs = 0.01;
	const double HistStep = 1.02;

	struct Stat
	{
		FString Name;
		uint32_t Count = 0;
		double Sum = 0.0;
		double Max = 0.0;
		uint32_t Hist[HistBins] = {};

		// GPU groups only: this frame's total so far (both stereo eyes).
		double FrameValue = 0.0;
		bool FrameTouched = false;

		void Clear()
		{
			Count = 0;
			Sum = Max = 0.0;
			memset(Hist, 0, sizeof(Hist));
			FrameValue = 0.0;
			FrameTouched = false;
		}

		void Add(double ms)
		{
			if (!(ms >= 0.0)) ms = 0.0;	// negative or NaN
			Count++;
			Sum += ms;
			if (ms > Max) Max = ms;
			Hist[Bin(ms)]++;
		}

		static int Bin(double ms)
		{
			if (ms <= HistBaseMs) return 0;
			int b = (int)(std::log(ms / HistBaseMs) / std::log(HistStep)) + 1;
			return b < HistBins ? b : HistBins - 1;
		}

		double Avg() const { return Count ? Sum / Count : 0.0; }

		double P95() const
		{
			if (Count == 0) return 0.0;
			const uint32_t target = (uint32_t)std::ceil(Count * 0.95);
			uint32_t seen = 0;
			for (int b = 0; b < HistBins; b++)
			{
				seen += Hist[b];
				if (seen >= target)
				{
					const double edge = HistBaseMs * std::pow(HistStep, b);
					return edge < Max ? edge : Max;
				}
			}
			return Max;
		}
	};

	enum { CPU_Scene, CPU_Post, CPU_Finalize, CPU_Submit, CPU_Composite, CPU_SyncWait, CPU_All, CPU_Drawcalls, CPU_Count };
	const char* const CpuNames[CPU_Count] = { "Scene", "Post", "Finalize", "Submit", "Composite", "SyncWait", "All", "drawcalls" };

	// Custom post-process shaders are named by their authors; this only bounds
	// memory if something ever names groups dynamically.
	const size_t MaxGpuNames = 96;

	struct Window
	{
		uint64_t StartNs = 0;
		uint64_t LastFrameNs = 0;	// 0 = the next frame only starts the clock

		Stat Frame;
		Stat Cpu[CPU_Count];
		std::vector<Stat> Gpu;	// kept across windows so the line order is stable
		std::vector<Stat> CpuFx;	// [2d] AddCpuSample's named CPU timings, kept the same way

		uint64_t ParticlesSpawned = 0;
		unsigned DrawnLinesMax = 0;
		int BeamsMax = 0, StampsMax = 0, DisturbMax = 0;
		int64_t DlightsSum = 0;
		int DlightsMax = 0;
		int64_t SpritesSum = 0, WallsSum = 0, FlatsSum = 0;
	};

	Window W;
	FString CurrentMap;
	FString Label, PendingLabel;
	bool LabelChanged = false;
	bool HeaderWritten = false;
	uint64_t LastParticlesWritten = 0;
	bool HaveParticles = false;

	FString LogPath()
	{
		FString path = I_GetCWD();
		if (path.Len() > 0 && path[path.Len() - 1] != '/')
			path += "/";
		path += "perflog.txt";
		return path;
	}

	void ResetWindow(uint64_t now, bool restartClock)
	{
		W.Frame.Clear();
		for (auto& c : W.Cpu) c.Clear();
		for (auto& g : W.Gpu) g.Clear();
		for (auto& c : W.CpuFx) c.Clear();	// [2d]
		W.ParticlesSpawned = 0;
		W.DrawnLinesMax = 0;
		W.BeamsMax = W.StampsMax = W.DisturbMax = 0;
		W.DlightsSum = 0;
		W.DlightsMax = 0;
		W.SpritesSum = W.WallsSum = W.FlatsSum = 0;
		W.StartNs = now;
		if (restartClock) W.LastFrameNs = 0;
	}

	void AppendTriple(FString& out, const Stat& s)
	{
		out.AppendFormat(" %s=%.2f/%.2f/%.2f", s.Name.GetChars(), s.Avg(), s.P95(), s.Max);
	}

	void WriteBlock(uint64_t now)
	{
		FString out;
		if (!HeaderWritten)
		{
			out.AppendFormat("\n===== perflog session: r_perflog %d =====\n", (int)*r_perflog);
			AppendBenchmarkHeader(out);
			out << "Legend: cpu_ms and gpu_ms are avg/p95/max per frame over the window. Same-name GPU groups in one frame "
				"(both stereo eyes) are summed; fx.* groups are nested inside scene.translucent. load: particles_spawned is "
				"the window total, dlights (walls+flats) is avg/max, sprites/walls/flats are avg, the rest are max. "
				"cpu_fx_ms, when present, is named CPU work of effects (fx.viewlights: the view light fill; fx.heatsources: "
				"the heat source fill), avg/p95/max per frame with same-name samples summed. pp.heatoffset and pp.heatwarp "
				"are the heat shimmer passes (r_heatrefraction).\n\n";
			// [COMPUTE] The compute names, on a legend line of their own.
			out << "Legend (compute): fx.compute is the frame's compute hook -- on cpu_fx_ms every frame while this log "
				"is on (its cost when nothing has compute work), on gpu_ms only when compute work was recorded. fx.smokesim "
				"is ONE smoke volume step, counted per step, not per frame (avg/p95/max per step; its GPU group nests inside "
				"fx.compute). fx.sectorplanes is the renderer's sector plane poll; fx.solidity the level solidity mask "
				"(r_smoke_computetest).\n\n";
			HeaderWritten = true;
		}

		const double windowS = (now - W.StartNs) / 1e9;
		const uint32_t frames = W.Frame.Count;
		const double fps = windowS > 0.0 ? frames / windowS : 0.0;
		const char* map = CurrentMap.IsEmpty() ? "(none)" : CurrentMap.GetChars();

		// vr_menu_keep_world lives in the OpenXR device; looked up by name so this
		// file does not depend on which VR backends are compiled in.
		FBaseCVar* keepWorld = FindCVar("vr_menu_keep_world", nullptr);

		out.AppendFormat("perflog map=%s", map);
		if (!Label.IsEmpty())
			out.AppendFormat(" [label: %s]", Label.GetChars());
		out.AppendFormat(" r_gpuparticles=%d r_beams_drawn=%d vr_menu_keep_world=%s",
			(int)*r_gpuparticles, (int)*r_beams_drawn,
			keepWorld != nullptr ? (keepWorld->GetGenericRep(CVAR_Int).Int ? "1" : "0") : "n/a");
		// [2a] So a window with soft particles on (and the fx.depthread switch it
		// costs) can be told from one with it off in the same run.
		out.AppendFormat(" r_gpuparticles_soft=%g", (double)(float)*r_gpuparticles_soft);
		// [2b] Likewise for the legacy particle path A/B, so the two windows label themselves.
		out.AppendFormat(" r_gpuparticles_legacy=%d", (int)*r_gpuparticles_legacy);
		// [2d] And the view light count, so a fx.viewlights before/after labels itself.
		out.AppendFormat(" r_gpuparticles_lights=%d", (int)*r_gpuparticles_lights);
		// [LOOKS] And the generated particle looks quality, so a fx.gpuparticles before/after labels itself.
		out.AppendFormat(" r_gpuparticles_looks=%d", (int)*r_gpuparticles_looks);
		// [HEATREFRACTION] And the heat shimmer switch, so a pp.heatoffset / pp.heatwarp
		// before/after labels itself.
		out.AppendFormat(" r_heatrefraction=%d", (int)*r_heatrefraction);
		// [SMOKEVOLUME] And the smoke switches, so a fx.compute / fx.smokesim / fx.solidity
		// before/after labels itself.
		out.AppendFormat(" r_smoke=%d r_smoke_quality=%d r_smoke_computetest=%d",
			(int)*r_smoke, (int)*r_smoke_quality, (int)*r_smoke_computetest);
		out.AppendFormat(" t=%.1fs window=%.1fs frames=%u fps=%.1f frame_ms avg=%.2f p95=%.2f max=%.2f\n",
			I_msTime() / 1000.0, windowS, frames, fps, W.Frame.Avg(), W.Frame.P95(), W.Frame.Max);

		out << "cpu_ms ";
		for (auto& c : W.Cpu) AppendTriple(out, c);
		out << "\n";

		out << "gpu_ms ";
		bool anyGpu = false;
		for (auto& g : W.Gpu)
		{
			if (g.Count == 0) continue;
			AppendTriple(out, g);
			anyGpu = true;
		}
		if (!anyGpu) out << " (none: needs Vulkan with timestamp queries)";
		out << "\n";

		// [2d] Named CPU timings, only when some effect took one this window.
		bool anyCpuFx = false;
		for (auto& c : W.CpuFx)
		{
			if (c.Count == 0) continue;
			if (!anyCpuFx) out << "cpu_fx_ms ";
			AppendTriple(out, c);
			anyCpuFx = true;
		}
		if (anyCpuFx) out << "\n";

		const double n = frames > 0 ? (double)frames : 1.0;
		out.AppendFormat("load    particles_spawned=%llu drawnlines_live=%u beams=%d stamps_live=%d disturb_live=%d dlights=%.0f/%d sprites=%.0f walls=%.0f flats=%.0f\n\n",
			(unsigned long long)W.ParticlesSpawned, W.DrawnLinesMax, W.BeamsMax, W.StampsMax, W.DisturbMax,
			W.DlightsSum / n, W.DlightsMax, W.SpritesSum / n, W.WallsSum / n, W.FlatsSum / n);

		FILE* f = fopen("perflog.txt", "at");
		if (f != nullptr)
		{
			fputs(out.GetChars(), f);
			fclose(f);
		}

		// One summary line for a running logfile; kept off the notify HUD so a
		// headset run is not covered in text every few seconds.
		Printf(static_cast<PrintFlag>(PRINT_HIGH | PRINT_NONOTIFY), "perflog: map=%s fps=%.1f frame_ms avg=%.2f p95=%.2f max=%.2f\n",
			map, fps, W.Frame.Avg(), W.Frame.P95(), W.Frame.Max);
	}
}

// [COMPUTE] CountEachRun's names, kept for the process: a perf log restart clears the
// stats, not these.
static std::vector<FString> EachRunNames;

void PerfLog::CountEachRun(const char* name)
{
	for (auto& known : EachRunNames)
	{
		if (known.Compare(name) == 0)
			return;
	}
	EachRunNames.push_back(name);
}

// A CountEachRun name goes straight into its stat, one sample per run. False (and
// nothing done) for any other name.
static bool AddEachRun(std::vector<Stat>& list, const char* name, double ms)
{
	bool eachRun = false;
	for (auto& known : EachRunNames)
	{
		if (known.Compare(name) == 0)
		{
			eachRun = true;
			break;
		}
	}
	if (!eachRun)
		return false;

	for (auto& s : list)
	{
		if (s.Name.Compare(name) == 0)
		{
			s.Add(ms);
			return true;
		}
	}
	if (list.size() >= MaxGpuNames)
		return true;
	list.emplace_back();
	list.back().Name = name;
	list.back().Add(ms);
	return true;
}

void PerfLog::AddGpuSample(const char* name, double ms)
{
	// [COMPUTE] A CountEachRun name: per run, not per frame.
	if (AddEachRun(W.Gpu, name, ms))
		return;
	for (auto& g : W.Gpu)
	{
		if (g.Name.Compare(name) == 0)
		{
			g.FrameValue += ms;
			g.FrameTouched = true;
			return;
		}
	}
	if (W.Gpu.size() >= MaxGpuNames)
		return;
	W.Gpu.emplace_back();
	W.Gpu.back().Name = name;
	W.Gpu.back().FrameValue = ms;
	W.Gpu.back().FrameTouched = true;
}

void PerfLog::AddCpuSample(const char* name, double ms)
{
	// [COMPUTE] A CountEachRun name: per run, not per frame.
	if (AddEachRun(W.CpuFx, name, ms))
		return;
	// [2d] AddGpuSample's rule, on its own list.
	for (auto& c : W.CpuFx)
	{
		if (c.Name.Compare(name) == 0)
		{
			c.FrameValue += ms;
			c.FrameTouched = true;
			return;
		}
	}
	if (W.CpuFx.size() >= MaxGpuNames)
		return;
	W.CpuFx.emplace_back();
	W.CpuFx.back().Name = name;
	W.CpuFx.back().FrameValue = ms;
	W.CpuFx.back().FrameTouched = true;
}

void PerfLog::EndFrame(const SceneLoad& load)
{
	const int seconds = *r_perflog;
	if (seconds <= 0)
		return;

	// Keep the GPU timestamp groups running without "stat gpu" on screen. Vulkan
	// only: on GL the same flag would switch on GL timer queries, and GL is not
	// a target of this log.
	if (screen != nullptr && screen->IsVulkan())
		keepGpuStatActive = true;

	const uint64_t now = I_nsTime();
	const char* map = load.MapName != nullptr ? load.MapName : "";
	bool announce = false;

	if (PerfLogRestart)
	{
		PerfLogRestart = false;
		for (int i = 0; i < CPU_Count; i++)
			W.Cpu[i].Name = CpuNames[i];	// Clear() keeps names; set once per session
		W.Gpu.clear();
		W.CpuFx.clear();	// [2d]
		ResetWindow(now, true);
		HeaderWritten = false;
		HaveParticles = false;
		if (LabelChanged) { Label = PendingLabel; LabelChanged = false; }
		CurrentMap = map;
		announce = true;
	}
	else if (CurrentMap.Compare(map) != 0)
	{
		// A new map closes the old map's partial window, so no block mixes two
		// maps, and the loading frame is not counted as a frame.
		if (W.Frame.Count > 0) WriteBlock(now);
		ResetWindow(now, true);
		HaveParticles = false;
		CurrentMap = map;
		announce = true;
	}
	else if (LabelChanged)
	{
		// A new label closes the window under the old one.
		if (W.Frame.Count > 0) WriteBlock(now);
		Label = PendingLabel;
		LabelChanged = false;
		ResetWindow(now, false);
	}

	if (announce)
		Printf("perflog: writing every %d s to %s\n", seconds, LogPath().GetChars());

	if (W.LastFrameNs == 0)
	{
		// First frame of a session or map: only starts the clock and the
		// particle baseline. Anything the GPU reported for it is dropped.
		W.StartNs = now;
		W.LastFrameNs = now;
		for (auto& g : W.Gpu) { g.FrameValue = 0.0; g.FrameTouched = false; }
		for (auto& c : W.CpuFx) { c.FrameValue = 0.0; c.FrameTouched = false; }	// [2d]
		LastParticlesWritten = load.GpuParticlesWritten;
		HaveParticles = true;
		return;
	}

	// Frame interval.
	W.Frame.Add((now - W.LastFrameNs) / 1e6);
	W.LastFrameNs = now;

	// CPU timers (hw_clock.cpp; CheckBenchActive keeps them ticking).
	RenderTimeSummary cpu;
	GetRenderTimeSummary(cpu);
	W.Cpu[CPU_Scene].Add(cpu.Scene);
	W.Cpu[CPU_Post].Add(cpu.Post);
	W.Cpu[CPU_Finalize].Add(cpu.Finalize);
	W.Cpu[CPU_Submit].Add(cpu.Submit);
	W.Cpu[CPU_Composite].Add(cpu.Composite);
	W.Cpu[CPU_SyncWait].Add(cpu.SyncWait);
	W.Cpu[CPU_All].Add(cpu.All);
	W.Cpu[CPU_Drawcalls].Add(cpu.Drawcalls);

	// GPU groups the backend reported for this frame.
	for (auto& g : W.Gpu)
	{
		if (!g.FrameTouched) continue;
		g.Add(g.FrameValue);
		g.FrameValue = 0.0;
		g.FrameTouched = false;
	}

	// [2d] Named CPU timings taken this frame (AddCpuSample).
	for (auto& c : W.CpuFx)
	{
		if (!c.FrameTouched) continue;
		c.Add(c.FrameValue);
		c.FrameValue = 0.0;
		c.FrameTouched = false;
	}

	// Effect load.
	if (!HaveParticles)
	{
		LastParticlesWritten = load.GpuParticlesWritten;
		HaveParticles = true;
	}
	else
	{
		// The running total drops back to zero when a level clears its ring.
		W.ParticlesSpawned += load.GpuParticlesWritten >= LastParticlesWritten
			? load.GpuParticlesWritten - LastParticlesWritten : load.GpuParticlesWritten;
		LastParticlesWritten = load.GpuParticlesWritten;
	}

	unsigned drawnLines = (screen != nullptr && screen->mDrawnLines != nullptr) ? screen->mDrawnLines->GetLiveCount() : 0u;
	if (drawnLines > W.DrawnLinesMax) W.DrawnLinesMax = drawnLines;
	if (load.BeamsLive > W.BeamsMax) W.BeamsMax = load.BeamsLive;
	if (load.StampsLive > W.StampsMax) W.StampsMax = load.StampsLive;
	if (load.DisturbLive > W.DisturbMax) W.DisturbMax = load.DisturbLive;

	const int dlights = draw_dlight + draw_dlightf;
	W.DlightsSum += dlights;
	if (dlights > W.DlightsMax) W.DlightsMax = dlights;
	W.SpritesSum += rendered_sprites;
	W.WallsSum += rendered_lines;
	W.FlatsSum += rendered_flats;

	if (now - W.StartNs >= (uint64_t)seconds * 1000000000ull)
	{
		WriteBlock(now);
		ResetWindow(now, false);
	}
}

// Tags the following blocks, like "bench <label>". Desktop use: the headset
// has no keyboard, and each block already names its map and the effect cvars.
CCMD(perflog_label)
{
	PendingLabel = "";
	for (int i = 1; i < argv.argc(); ++i)
	{
		if (!PendingLabel.IsEmpty())
			PendingLabel << ' ';
		PendingLabel << argv[i];
	}
	LabelChanged = true;
	Printf("perflog: label %s\n", PendingLabel.IsEmpty() ? "cleared" : PendingLabel.GetChars());
}
