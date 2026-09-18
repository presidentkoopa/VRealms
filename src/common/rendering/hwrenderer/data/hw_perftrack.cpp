/*
** hw_perftrack.cpp
**
** RS FORK -- perf_track: the per-frame telemetry record. See hw_perftrack.h.
**
** Two files per run, in the folder OutputDir() chooses:
**
**   perftrack_<stamp>.csv       one row a frame, every column named in the header
**   perftrack_<stamp>.txt       the run header (build, load order, changed cvars) and the summary
**
** The CSV's own clocks are the perf log's: the frame interval, the CPU buckets and the named GPU and CPU
** groups all come from the same calls the perf log reads, so a window of this CSV and the perf log's block for
** the same window agree by construction.
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

#include "hw_perftrack.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <vector>

#include "c_dispatch.h"
#include "printf.h"
#include "i_time.h"
#include "i_system.h"
#include "i_specialpaths.h"
#include "m_argv.h"
#include "filesystem.h"
#include "v_video.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hw_drawnlinebuffer.h"
#include "hw_gpuparticlebuffer.h"
#include "hw_effectlightbuffer.h"
#include "hw_emissivevolumeframe.h"
#include "hw_framecompute.h"
#include "hw_effectsgovernor.h"		// [GOVERNOR] E8: the frame's cost, pacing and budget -- measured there, not again here

extern bool keepGpuStatActive;		// hw_postprocess.cpp
const char* GetVersionString();		// version.h (game side); only the string is wanted here
EXTERN_FARG(savedir);				// d_main.cpp, declared here as savegamemanager.cpp declares it

// RS FORK -- perf_track: 0 off (the default and the archived value), 1 on. On, every frame is recorded into
// memory; the CSV and the summary are written when the run ends. Off, the only cost anywhere in the engine is
// PerfTrack::Active()'s integer test.
CUSTOM_CVARD(Int, perf_track, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "record one telemetry row per frame (0 = off); the CSV and summary are written when it is switched off, the map changes or the engine quits")
{
	if (self < 0) { self = 0; return; }
	if (self > 1) { self = 1; return; }
	PerfTrack::Flush(self > 0 ? "perf_track on" : "perf_track off");
}

// RS FORK -- perf_track: where the CSV and the summary are written. Empty (the default) means the -savedir this
// run was given, and without one the save folder. Never the exe's folder.
CVARD(String, perf_track_dir, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "folder for perf_track's CSV and summary (empty: this run's save folder)")

// RS FORK -- perf_track: a frame longer than this many milliseconds is a hitch and gets a reason. 0 (the
// default) takes one display period from the effects governor -- the headset's own while it gave one.
CVARD(Float, perf_track_hitch_ms, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "a frame longer than this is recorded as a hitch with a reason (0: one display period)")

// RS FORK -- perf_track: the most frames one run keeps. The buffer is reserved once, on the first tracked
// frame, so no allocation happens while the run is measured. 40000 is about seven minutes at 90 Hz.
CUSTOM_CVARD(Int, perf_track_frames, 40000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "the most frames one perf_track run keeps in memory before it stops recording")
{
	if (self < 1000) { self = 1000; return; }
	if (self > 2000000) { self = 2000000; return; }
}

namespace
{
	using namespace PerfTrack;

	// The named group tables. Two, because one name can be both a GPU group and a CPU sample (fx.smokelight is
	// both: its dispatches and its recording). Capped so a mod naming groups dynamically cannot grow them
	// without bound; the engine's own names come to about thirty and twenty.
	const int MaxGpuNames = 64;
	const int MaxCpuNames = 64;
	const int MaxGroups = MaxGpuNames + MaxCpuNames;

	enum { CPU_Scene, CPU_Post, CPU_Finalize, CPU_Submit, CPU_Composite, CPU_SyncWait, CPU_All, CPU_Drawcalls, CPU_Count };
	const char* const CpuNames[CPU_Count] = { "scene", "post", "finalize", "submit", "composite", "syncwait", "all", "drawcalls" };

	const char* const ReasonNames[REASON_COUNT] = { "pipeline", "texture", "buffer", "gc", "shadow", "geometry", "script", "runtime" };

	// One frame. Plain data, no pointers, no allocation: the whole run is one reserved vector of these.
	struct Row
	{
		uint32_t Frame = 0;
		double TimeMs = 0.0;			// since the run's first recorded frame
		int32_t Tic = 0;
		int32_t MapIndex = -1;
		float X = 0, Y = 0, Z = 0, Angle = 0, Pitch = 0;

		float FrameMs = 0, CostMs = 0, PacingMs = 0, BudgetMs = 0;
		int16_t Rung = 0;

		float Cpu[CPU_Count] = {};

		uint16_t Tics = 0;
		float ThinkMs = 0, CsThinkMs = 0;
		int32_t Thinkers = 0, CsThinkers = 0;

		// The headset runtime's share.
		float XrWait = 0, XrAcquire = 0, XrImageWait = 0, XrEnd = 0, XrPeriod = 0, XrAhead = 0;
		int16_t XrPeriodsAhead = 0;
		uint8_t XrFlags = 0;			// 1: a shared clock answered; 2: the metrics extension answered
		float XrAppGpu = 0, XrCompGpu = 0, XrCompCpu = 0, XrFps = 0;
		int32_t XrAppDropped = 0, XrCompDropped = 0, XrStale = 0;

		// Scene shape.
		int32_t Sprites = 0, Walls = 0, Flats = 0, Decals = 0, Portals = 0, Dlights = 0;
		int32_t Vertices = 0, FlatPrims = 0;
		uint32_t ParticlesSpawned = 0;
		int32_t Beams = 0, Stamps = 0, Disturb = 0;
		int32_t ElLive = 0, ElBinned = 0, EmLive = 0, EmDrawn = 0, ShadowRows = 0;
		float ShadowCpuMs = 0;
		uint32_t SmokeCells = 0;

		// Hitches.
		float Reason[REASON_COUNT] = {};
		int8_t Hitch = 0;
		int8_t Worst = REASON_NONE;
		float WorstMs = 0;

		float Group[MaxGroups] = {};
	};

	struct Names
	{
		FString Name[MaxGpuNames > MaxCpuNames ? MaxGpuNames : MaxCpuNames];
		int Count = 0;

		// The index of `name`, adding it when there is room. -1 when the table is full.
		int Find(const char* name, int cap)
		{
			for (int i = 0; i < Count; i++)
				if (Name[i].Compare(name) == 0)
					return i;
			if (Count >= cap)
				return -1;
			Name[Count] = name;
			return Count++;
		}
	};

	// The run.
	std::vector<Row> Rows;
	std::vector<FString> MapNames;
	Names GpuNames, CpuFxNames;
	bool Running = false;				// a run is recording (the buffer is reserved)
	bool Overflowed = false;
	uint32_t FrameCounter = 0;
	uint64_t RunStartNs = 0;
	uint64_t LastFrameNs = 0;
	int LastMapIndex = -1;
	int RunSerial = 0;

	// This frame's pending values, cleared at the end of every frame.
	float PendingGpu[MaxGpuNames] = {};
	float PendingCpu[MaxCpuNames] = {};
	std::atomic<uint64_t> PendingReasonNs[REASON_COUNT];	// any thread: the texture thread uploads
	double PendingThinkMs = 0.0, PendingCsThinkMs = 0.0;
	int PendingThinkers = 0, PendingCsThinkers = 0;
	int PendingTics = 0;
	XrFrameInfo PendingXr;
	bool PendingXrValid = false;
	uint64_t LastParticlesWritten = 0;
	bool HaveParticles = false;

	void ClearPending()
	{
		memset(PendingGpu, 0, sizeof(PendingGpu));
		memset(PendingCpu, 0, sizeof(PendingCpu));
		for (int i = 0; i < REASON_COUNT; i++)
			PendingReasonNs[i].store(0, std::memory_order_relaxed);
		PendingThinkMs = PendingCsThinkMs = 0.0;
		PendingThinkers = PendingCsThinkers = 0;
		PendingTics = 0;
		PendingXr = XrFrameInfo();
		PendingXrValid = false;
	}

	int MapIndexOf(const char* name)
	{
		const char* map = name != nullptr ? name : "";
		for (unsigned i = 0; i < MapNames.size(); i++)
			if (MapNames[i].Compare(map) == 0)
				return (int)i;
		MapNames.push_back(map);
		return (int)MapNames.size() - 1;
	}

	// A cheap, honest file fingerprint: the size, the last write time, and an FNV-1a over the first 4 MB. It is
	// NOT a whole-file hash and is not called one -- hashing several gigabytes of pk3s at the start of a capture
	// would be a stall the owner would feel. It catches the case that matters: a file rebuilt between two runs.
	void FileFingerprint(const char* path, uint64_t& size, int64_t& mtime, uint64_t& hash4m)
	{
		size = 0;
		mtime = 0;
		hash4m = 0xcbf29ce484222325ull;
		struct _stat64 st;
		if (path != nullptr && _stat64(path, &st) == 0)
		{
			size = (uint64_t)st.st_size;
			mtime = (int64_t)st.st_mtime;
		}
		FILE* f = path != nullptr ? fopen(path, "rb") : nullptr;
		if (f == nullptr)
			return;
		static const size_t Chunk = 64 * 1024;
		std::vector<unsigned char> buf(Chunk);
		size_t left = 4u * 1024u * 1024u;
		while (left > 0)
		{
			const size_t want = left < Chunk ? left : Chunk;
			const size_t got = fread(buf.data(), 1, want, f);
			for (size_t i = 0; i < got; i++)
			{
				hash4m ^= buf[i];
				hash4m *= 0x100000001b3ull;
			}
			if (got < want)
				break;
			left -= got;
		}
		fclose(f);
	}

	// The percentile of a column, by sorting a copy. Runs only when a file is written.
	double Percentile(std::vector<double>& v, double pct)
	{
		if (v.empty())
			return 0.0;
		const size_t k = (size_t)std::min((double)v.size() - 1.0, std::max(0.0, std::ceil(v.size() * pct) - 1.0));
		std::nth_element(v.begin(), v.begin() + k, v.end());
		return v[k];
	}

	FString Stamp()
	{
		time_t now = time(nullptr);
		struct tm t;
#ifdef _WIN32
		localtime_s(&t, &now);
#else
		localtime_r(&now, &t);
#endif
		FString s;
		s.Format("%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
		return s;
	}

	// Every column in order. The fixed ones, then one per named group seen in the run.
	void AppendHeaderRow(FString& out)
	{
		out << "frame,t_ms,tic,map,x,y,z,angle,pitch";
		out << ",frame_ms,cost_ms,pacing_ms,budget_ms,rung";
		for (int i = 0; i < CPU_Count; i++)
			out.AppendFormat(",cpu_%s_ms", CpuNames[i]);
		out << ",tics,think_ms,csthink_ms,thinkers,csthinkers";
		out << ",xr_wait_ms,xr_acquire_ms,xr_imagewait_ms,xr_end_ms,xr_period_ms,xr_ahead_ms,xr_periods_ahead,xr_sharedclock,xr_metrics";
		out << ",xr_app_gpu_ms,xr_comp_gpu_ms,xr_comp_cpu_ms,xr_comp_fps,xr_app_dropped,xr_comp_dropped,xr_stale";
		out << ",sprites,walls,flats,decals,portals,dlights,vertices,flatprims";
		out << ",particles_spawned,beams,stamps,disturb,effectlights_live,effectlights_binned,emissive_live,emissive_drawn";
		out << ",shadow_rows,shadow_cpu_ms,smokelight_cells";
		out << ",hitch,reason,reason_ms";
		for (int i = 0; i < REASON_COUNT; i++)
			out.AppendFormat(",r_%s_ms", ReasonNames[i]);
		for (int i = 0; i < GpuNames.Count; i++)
			out.AppendFormat(",gpu.%s", GpuNames.Name[i].GetChars());
		for (int i = 0; i < CpuFxNames.Count; i++)
			out.AppendFormat(",cpufx.%s", CpuFxNames.Name[i].GetChars());
		out << "\n";
	}

	void AppendRow(FString& out, const Row& r)
	{
		out.AppendFormat("%u,%.3f,%d,%s,%.1f,%.1f,%.1f,%.2f,%.2f",
			r.Frame, r.TimeMs, r.Tic,
			(r.MapIndex >= 0 && (size_t)r.MapIndex < MapNames.size()) ? MapNames[r.MapIndex].GetChars() : "",
			r.X, r.Y, r.Z, r.Angle, r.Pitch);
		out.AppendFormat(",%.3f,%.3f,%.3f,%.3f,%d", r.FrameMs, r.CostMs, r.PacingMs, r.BudgetMs, (int)r.Rung);
		for (int i = 0; i < CPU_Count; i++)
			out.AppendFormat(",%.3f", r.Cpu[i]);
		out.AppendFormat(",%u,%.3f,%.3f,%d,%d", (unsigned)r.Tics, r.ThinkMs, r.CsThinkMs, r.Thinkers, r.CsThinkers);
		out.AppendFormat(",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d",
			r.XrWait, r.XrAcquire, r.XrImageWait, r.XrEnd, r.XrPeriod, r.XrAhead, (int)r.XrPeriodsAhead,
			(r.XrFlags & 1) ? 1 : 0, (r.XrFlags & 2) ? 1 : 0);
		out.AppendFormat(",%.3f,%.3f,%.3f,%.2f,%d,%d,%d",
			r.XrAppGpu, r.XrCompGpu, r.XrCompCpu, r.XrFps, r.XrAppDropped, r.XrCompDropped, r.XrStale);
		out.AppendFormat(",%d,%d,%d,%d,%d,%d,%d,%d",
			r.Sprites, r.Walls, r.Flats, r.Decals, r.Portals, r.Dlights, r.Vertices, r.FlatPrims);
		out.AppendFormat(",%u,%d,%d,%d,%d,%d,%d,%d",
			r.ParticlesSpawned, r.Beams, r.Stamps, r.Disturb, r.ElLive, r.ElBinned, r.EmLive, r.EmDrawn);
		out.AppendFormat(",%d,%.3f,%u", r.ShadowRows, r.ShadowCpuMs, r.SmokeCells);
		out.AppendFormat(",%d,%s,%.3f", (int)r.Hitch,
			(r.Worst >= 0 && r.Worst < REASON_COUNT) ? ReasonNames[r.Worst] : "", r.WorstMs);
		for (int i = 0; i < REASON_COUNT; i++)
			out.AppendFormat(",%.3f", r.Reason[i]);
		for (int i = 0; i < GpuNames.Count; i++)
			out.AppendFormat(",%.3f", r.Group[i]);
		for (int i = 0; i < CpuFxNames.Count; i++)
			out.AppendFormat(",%.3f", r.Group[MaxGpuNames + i]);
		out << "\n";
	}

	// One "name p50/p95/p99/max" line from a column picked out of the rows.
	void AppendSummaryLine(FString& out, const char* name, std::vector<double>& v)
	{
		double sum = 0.0, mx = 0.0;
		for (double d : v) { sum += d; if (d > mx) mx = d; }
		const double p50 = Percentile(v, 0.50);
		const double p95 = Percentile(v, 0.95);
		const double p99 = Percentile(v, 0.99);
		out.AppendFormat("  %-28s n=%-7u avg=%8.3f p50=%8.3f p95=%8.3f p99=%8.3f max=%9.3f\n",
			name, (unsigned)v.size(), v.empty() ? 0.0 : sum / v.size(), p50, p95, p99, mx);
	}
}

const char* PerfTrack::ReasonName(int reason)
{
	return (reason >= 0 && reason < REASON_COUNT) ? ReasonNames[reason] : "none";
}

void PerfTrack::NoteGpuGroup(const char* name, double ms)
{
	const int i = GpuNames.Find(name, MaxGpuNames);
	if (i >= 0)
		PendingGpu[i] += (float)ms;
}

void PerfTrack::NoteCpuGroup(const char* name, double ms)
{
	const int i = CpuFxNames.Find(name, MaxCpuNames);
	if (i >= 0)
		PendingCpu[i] += (float)ms;
}

void PerfTrack::NoteThink(bool clientSide, double ms, int thinkers)
{
	if (clientSide)
	{
		PendingCsThinkMs += ms;
		if (thinkers > PendingCsThinkers) PendingCsThinkers = thinkers;
	}
	else
	{
		PendingThinkMs += ms;
		if (thinkers > PendingThinkers) PendingThinkers = thinkers;
	}
}

void PerfTrack::NoteTic()
{
	PendingTics++;
}

void PerfTrack::NoteReason(int reason, double ms)
{
	if (reason < 0 || reason >= REASON_COUNT || !(ms > 0.0))
		return;
	PendingReasonNs[reason].fetch_add((uint64_t)(ms * 1e6), std::memory_order_relaxed);
}

void PerfTrack::NoteXrFrame(const XrFrameInfo& xr)
{
	PendingXr = xr;
	PendingXrValid = true;
}

FString PerfTrack::OutputDir()
{
	FString dir = *perf_track_dir;
	if (dir.IsEmpty() && Args != nullptr)
	{
		const char* savedir = Args->CheckValue(FArg_savedir);
		if (savedir != nullptr)
			dir = savedir;
	}
	if (dir.IsEmpty())
		dir = M_GetSavegamesPath();
	if (dir.IsEmpty())
		dir = I_GetCWD();		// last resort: never the exe's folder unless that is also the working directory
	dir.Substitute("\\", "/");
	if (dir.Len() > 0 && dir[dir.Len() - 1] != '/')
		dir += "/";
	return dir;
}

void PerfTrack::AppendRunHeader(FString& out)
{
	out.AppendFormat("# engine=%s\n", GetVersionString());
	out.AppendFormat("# unit_built=%s %s\n", __DATE__, __TIME__);
	if (Args != nullptr && Args->GetArg(0) != nullptr)
	{
		uint64_t size; int64_t mtime; uint64_t hash;
		FileFingerprint(Args->GetArg(0), size, mtime, hash);
		out.AppendFormat("# exe=%s size=%llu mtime=%lld hash4m=%016llx\n",
			Args->GetArg(0), (unsigned long long)size, (long long)mtime, (unsigned long long)hash);
	}
	out.AppendFormat("# renderer=%s\n", (screen != nullptr && screen->IsVulkan()) ? "vulkan" : "other");

	// The load order, in the order the file system holds it, with each file's fingerprint.
	const int wads = fileSystem.GetNumWads();
	out.AppendFormat("# loadorder_count=%d\n", wads);
	for (int i = 0; i < wads; i++)
	{
		const char* full = fileSystem.GetResourceFileFullName(i);
		uint64_t size; int64_t mtime; uint64_t hash;
		FileFingerprint(full, size, mtime, hash);
		out.AppendFormat("# loadorder[%d]=%s size=%llu mtime=%lld hash4m=%016llx\n",
			i, full != nullptr ? full : "", (unsigned long long)size, (long long)mtime, (unsigned long long)hash);
	}

	// Every cvar that differs from its default, sorted, so two runs can be diffed.
	TArray<FBaseCVar*> cvars;
	FilterCompactCVars(cvars, CVAR_ARCHIVE | CVAR_SERVERINFO | CVAR_DEMOSAVE | CVAR_GLOBALCONFIG);
	int changed = 0;
	FString lines;
	for (unsigned i = 0; i < cvars.Size(); i++)
	{
		FBaseCVar* cvar = cvars[i];
		if (cvar == nullptr)
			continue;
		const char* now = cvar->GetHumanString();
		const char* def = cvar->GetHumanStringDefault();
		if (now == nullptr || def == nullptr || strcmp(now, def) == 0)
			continue;
		lines.AppendFormat("# cvar %s=%s (default %s)\n", cvar->GetName(), now, def);
		changed++;
	}
	out.AppendFormat("# cvars_changed=%d\n", changed);
	out << lines;
}

void PerfTrack::EndFrame(const SceneShape& shape)
{
	// Keep the GPU timestamp groups running without "stat gpu" on screen, exactly as the perf log does.
	if (screen != nullptr && screen->IsVulkan())
		keepGpuStatActive = true;

	const uint64_t now = I_nsTime();

	if (!Running)
	{
		// The run starts here. The whole buffer is reserved once, so no allocation happens while frames are
		// being measured; this first frame is the one that pays for it and it is not recorded.
		Running = true;
		Overflowed = false;
		FrameCounter = 0;
		RunSerial++;
		Rows.clear();
		Rows.reserve((size_t)*perf_track_frames);
		MapNames.clear();
		GpuNames.Count = 0;
		CpuFxNames.Count = 0;
		RunStartNs = now;
		LastFrameNs = now;
		LastMapIndex = -1;
		LastParticlesWritten = shape.GpuParticlesWritten;
		HaveParticles = true;
		ClearPending();
		Printf("perf_track: recording to %s\n", OutputDir().GetChars());
		return;
	}

	const int mapIndex = MapIndexOf(shape.MapName);
	const bool mapChanged = LastMapIndex >= 0 && mapIndex != LastMapIndex;

	if (Overflowed || Rows.size() >= (size_t)*perf_track_frames)
	{
		if (!Overflowed)
		{
			Overflowed = true;
			Printf("perf_track: %d frames recorded, the buffer is full -- raise perf_track_frames or flush\n", (int)Rows.size());
		}
		LastFrameNs = now;
		LastMapIndex = mapIndex;
		ClearPending();
		return;
	}

	Row r;
	r.Frame = FrameCounter++;
	r.TimeMs = (now - RunStartNs) / 1e6;
	r.FrameMs = (float)((now - LastFrameNs) / 1e6);
	LastFrameNs = now;
	r.MapIndex = mapIndex;
	r.Tic = shape.Tic;
	r.X = (float)shape.X; r.Y = (float)shape.Y; r.Z = (float)shape.Z;
	r.Angle = (float)shape.Angle; r.Pitch = (float)shape.Pitch;

	// [GOVERNOR] E8: the frame's cost less its pacing, its budget and the rung -- measured once, there.
	{
		const EffectsGovernor::FrameInfo& g = EffectsGovernor::LastFrame();
		r.CostMs = (float)g.CostMs;
		r.PacingMs = (float)g.PacingMs;
		r.BudgetMs = (float)g.BudgetMs;
		r.Rung = (int16_t)g.Rung;
	}

	// The same CPU timers the perf log reads (hw_clock.cpp).
	{
		RenderTimeSummary cpu;
		GetRenderTimeSummary(cpu);
		r.Cpu[CPU_Scene] = (float)cpu.Scene;
		r.Cpu[CPU_Post] = (float)cpu.Post;
		r.Cpu[CPU_Finalize] = (float)cpu.Finalize;
		r.Cpu[CPU_Submit] = (float)cpu.Submit;
		r.Cpu[CPU_Composite] = (float)cpu.Composite;
		r.Cpu[CPU_SyncWait] = (float)cpu.SyncWait;
		r.Cpu[CPU_All] = (float)cpu.All;
		r.Cpu[CPU_Drawcalls] = (float)cpu.Drawcalls;
	}

	r.Tics = (uint16_t)std::min(PendingTics, 65535);
	r.ThinkMs = (float)PendingThinkMs;
	r.CsThinkMs = (float)PendingCsThinkMs;
	r.Thinkers = PendingThinkers;
	r.CsThinkers = PendingCsThinkers;

	if (PendingXrValid)
	{
		r.XrWait = (float)PendingXr.WaitMs;
		r.XrAcquire = (float)PendingXr.AcquireMs;
		r.XrImageWait = (float)PendingXr.ImageWaitMs;
		r.XrEnd = (float)PendingXr.EndMs;
		r.XrPeriod = (float)PendingXr.PeriodMs;
		r.XrAhead = (float)PendingXr.AheadMs;
		r.XrPeriodsAhead = (int16_t)PendingXr.PeriodsAhead;
		r.XrFlags = (uint8_t)((PendingXr.HaveSharedClock ? 1 : 0) | (PendingXr.HaveMetrics ? 2 : 0));
		r.XrAppGpu = (float)PendingXr.AppGpuMs;
		r.XrCompGpu = (float)PendingXr.CompositorGpuMs;
		r.XrCompCpu = (float)PendingXr.CompositorCpuMs;
		r.XrFps = PendingXr.CompositorFps;
		r.XrAppDropped = PendingXr.AppDropped;
		r.XrCompDropped = PendingXr.CompositorDropped;
		r.XrStale = PendingXr.StaleFrames;
	}

	// Scene shape, from the same counters the perf log reports.
	r.Sprites = rendered_sprites;
	r.Walls = rendered_lines;
	r.Flats = rendered_flats;
	r.Decals = rendered_decals;
	r.Portals = rendered_portals;
	r.Dlights = draw_dlight + draw_dlightf;
	r.Vertices = vertexcount;
	r.FlatPrims = flatprimitives;
	r.Beams = shape.BeamsLive;
	r.Stamps = shape.StampsLive;
	r.Disturb = shape.DisturbLive;
	if (!HaveParticles)
	{
		LastParticlesWritten = shape.GpuParticlesWritten;
		HaveParticles = true;
	}
	else
	{
		// The running total drops back to zero when a level clears its ring.
		r.ParticlesSpawned = (uint32_t)std::min<uint64_t>(0xffffffffull, shape.GpuParticlesWritten >= LastParticlesWritten
			? shape.GpuParticlesWritten - LastParticlesWritten : shape.GpuParticlesWritten);
		LastParticlesWritten = shape.GpuParticlesWritten;
	}
	{
		const EffectLightFrameStats& e = EffectLightStats();
		r.ElLive = e.Live;
		r.ElBinned = e.Binned;
	}
	{
		const EmissiveVolumeFrameStats& e = EmissiveVolumeStats();
		r.EmLive = e.Live;
		r.EmDrawn = e.Drawn;
	}
	{
		const SmokeVolumeBackendStatus& smoke = SmokeVolumeStatus();
		r.SmokeCells = (uint32_t)smoke.LightCells;
	}
	// The shadow map's own clock, only on the frames that ran the pass.
	{
		static uint64_t lastShadowSerial = 0;
		static int lastShadowRunSerial = -1;
		if (lastShadowRunSerial != RunSerial)
		{
			lastShadowRunSerial = RunSerial;
			lastShadowSerial = IShadowMap::UpdateSerial;
		}
		else if (IShadowMap::UpdateSerial != lastShadowSerial)
		{
			lastShadowSerial = IShadowMap::UpdateSerial;
			r.ShadowRows = IShadowMap::LightsShadowmapped;
			r.ShadowCpuMs = (float)IShadowMap::UpdateCycles.TimeMS();
		}
	}

	// The named groups this frame reported.
	for (int i = 0; i < GpuNames.Count; i++)
		r.Group[i] = PendingGpu[i];
	for (int i = 0; i < CpuFxNames.Count; i++)
		r.Group[MaxGpuNames + i] = PendingCpu[i];

	// HITCHES. The explicit reasons were timed where they happened; the rest are read off numbers this row
	// already carries. The row's reason is whichever took the most of the frame.
	for (int i = 0; i < REASON_COUNT; i++)
		r.Reason[i] = (float)(PendingReasonNs[i].load(std::memory_order_relaxed) / 1e6);
	r.Reason[REASON_SHADOW] = std::max(r.Reason[REASON_SHADOW], r.ShadowCpuMs);
	r.Reason[REASON_SCRIPT] = std::max(r.Reason[REASON_SCRIPT], r.ThinkMs + r.CsThinkMs);
	r.Reason[REASON_RUNTIME] = std::max(r.Reason[REASON_RUNTIME], r.PacingMs);
	if (mapChanged)
		r.Reason[REASON_GEOMETRY] = std::max(r.Reason[REASON_GEOMETRY], r.FrameMs);

	const double hitchMs = *perf_track_hitch_ms > 0.0f ? (double)*perf_track_hitch_ms
		: (r.BudgetMs > 0.0f ? (double)r.BudgetMs : 1000.0 / 60.0);
	if (r.FrameMs > hitchMs)
	{
		r.Hitch = 1;
		for (int i = 0; i < REASON_COUNT; i++)
		{
			if (r.Reason[i] > r.WorstMs)
			{
				r.WorstMs = r.Reason[i];
				r.Worst = (int8_t)i;
			}
		}
	}

	Rows.push_back(r);
	LastMapIndex = mapIndex;
	ClearPending();

	// A map change closes the run, so no CSV mixes two maps and the loading frame does not read as a hitch in
	// the next map's numbers.
	if (mapChanged)
		Flush("map changed");
}

void PerfTrack::Flush(const char* why)
{
	if (!Running)
		return;
	const size_t frames = Rows.size();
	Running = false;
	if (frames == 0)
	{
		Rows.clear();
		return;
	}

	const FString dir = OutputDir();
	const FString stamp = Stamp();
	FString csvPath, txtPath;
	csvPath.Format("%sperftrack_%s.csv", dir.GetChars(), stamp.GetChars());
	txtPath.Format("%sperftrack_%s.txt", dir.GetChars(), stamp.GetChars());

	// --- the CSV
	{
		FString out;
		AppendHeaderRow(out);
		FILE* f = fopen(csvPath.GetChars(), "wt");
		if (f == nullptr)
		{
			Printf(TEXTCOLOR_RED "perf_track: cannot write %s\n", csvPath.GetChars());
			Rows.clear();
			return;
		}
		fputs(out.GetChars(), f);
		// In blocks, so one FString never has to hold the whole run.
		FString block;
		for (size_t i = 0; i < frames; i++)
		{
			AppendRow(block, Rows[i]);
			if (block.Len() > 256 * 1024)
			{
				fputs(block.GetChars(), f);
				block = "";
			}
		}
		if (block.Len() > 0)
			fputs(block.GetChars(), f);
		fclose(f);
	}

	// --- the summary
	{
		FString out;
		out.AppendFormat("perf_track run: %s\n", why != nullptr ? why : "");
		AppendRunHeader(out);
		out.AppendFormat("# frames=%u span_s=%.1f%s\n", (unsigned)frames, Rows.back().TimeMs / 1000.0,
			Overflowed ? " (buffer filled: the run was cut short)" : "");
		out.AppendFormat("# csv=%s\n\n", csvPath.GetChars());

		std::vector<double> col;
		col.reserve(frames);
		auto column = [&](const char* name, double (*pick)(const Row&))
		{
			col.clear();
			for (size_t i = 0; i < frames; i++)
			{
				const double v = pick(Rows[i]);
				if (v > 0.0)
					col.push_back(v);
			}
			if (!col.empty())
				AppendSummaryLine(out, name, col);
		};

		out << "frame (ms, every frame)\n";
		col.clear();
		for (size_t i = 0; i < frames; i++) col.push_back(Rows[i].FrameMs);
		AppendSummaryLine(out, "frame_ms", col);
		col.clear();
		for (size_t i = 0; i < frames; i++) col.push_back(Rows[i].CostMs);
		AppendSummaryLine(out, "cost_ms", col);
		col.clear();
		for (size_t i = 0; i < frames; i++) col.push_back(Rows[i].PacingMs);
		AppendSummaryLine(out, "pacing_ms", col);

		out << "\ncpu (ms, frames that reported the timer)\n";
		for (int c = 0; c < CPU_Count; c++)
		{
			col.clear();
			for (size_t i = 0; i < frames; i++)
				if (Rows[i].Cpu[c] > 0.0f) col.push_back(Rows[i].Cpu[c]);
			if (!col.empty())
			{
				FString n; n.Format("cpu.%s", CpuNames[c]);
				AppendSummaryLine(out, n.GetChars(), col);
			}
		}

		out << "\nplaysim (ms per frame, frames that ran a tic)\n";
		column("think_ms", [](const Row& r) { return (double)r.ThinkMs; });
		column("csthink_ms", [](const Row& r) { return (double)r.CsThinkMs; });

		out << "\nheadset runtime (ms, frames the runtime answered)\n";
		column("xr.wait", [](const Row& r) { return (double)r.XrWait; });
		column("xr.acquire", [](const Row& r) { return (double)r.XrAcquire; });
		column("xr.imagewait", [](const Row& r) { return (double)r.XrImageWait; });
		column("xr.end", [](const Row& r) { return (double)r.XrEnd; });
		column("xr.app_gpu", [](const Row& r) { return (double)r.XrAppGpu; });
		column("xr.comp_gpu", [](const Row& r) { return (double)r.XrCompGpu; });
		column("xr.comp_cpu", [](const Row& r) { return (double)r.XrCompCpu; });
		{
			int dropped = 0, stale = 0;
			for (size_t i = 0; i < frames; i++)
			{
				dropped = std::max(dropped, Rows[i].XrAppDropped + Rows[i].XrCompDropped);
				stale = std::max(stale, Rows[i].XrStale);
			}
			out.AppendFormat("  %-28s dropped=%d stale=%d\n", "xr.counters", dropped, stale);
		}

		out << "\ngpu groups (ms, frames the group ran)\n";
		for (int g = 0; g < GpuNames.Count; g++)
		{
			col.clear();
			for (size_t i = 0; i < frames; i++)
				if (Rows[i].Group[g] > 0.0f) col.push_back(Rows[i].Group[g]);
			if (!col.empty())
			{
				FString n; n.Format("gpu.%s", GpuNames.Name[g].GetChars());
				AppendSummaryLine(out, n.GetChars(), col);
			}
		}

		out << "\ncpu effect groups (ms, frames the group ran)\n";
		for (int g = 0; g < CpuFxNames.Count; g++)
		{
			col.clear();
			for (size_t i = 0; i < frames; i++)
				if (Rows[i].Group[MaxGpuNames + g] > 0.0f) col.push_back(Rows[i].Group[MaxGpuNames + g]);
			if (!col.empty())
			{
				FString n; n.Format("cpufx.%s", CpuFxNames.Name[g].GetChars());
				AppendSummaryLine(out, n.GetChars(), col);
			}
		}

		out << "\nhitches (frames over budget), by reason\n";
		{
			unsigned hitches = 0;
			unsigned byReason[REASON_COUNT] = {};
			double msByReason[REASON_COUNT] = {};
			unsigned unexplained = 0;
			for (size_t i = 0; i < frames; i++)
			{
				if (!Rows[i].Hitch)
					continue;
				hitches++;
				if (Rows[i].Worst >= 0 && Rows[i].Worst < REASON_COUNT)
				{
					byReason[Rows[i].Worst]++;
					msByReason[Rows[i].Worst] += Rows[i].WorstMs;
				}
				else
					unexplained++;
			}
			out.AppendFormat("  hitches=%u of %u frames (%.2f%%)\n", hitches, (unsigned)frames,
				frames ? 100.0 * hitches / frames : 0.0);
			for (int i = 0; i < REASON_COUNT; i++)
				if (byReason[i] > 0)
					out.AppendFormat("  %-28s n=%-7u total_ms=%.1f\n", ReasonNames[i], byReason[i], msByReason[i]);
			if (unexplained > 0)
				out.AppendFormat("  %-28s n=%u\n", "(no reason recorded)", unexplained);
		}

		FILE* f = fopen(txtPath.GetChars(), "wt");
		if (f != nullptr)
		{
			fputs(out.GetChars(), f);
			fclose(f);
		}
	}

	Printf("perf_track: %u frames written to %s\n", (unsigned)frames, csvPath.GetChars());
	Rows.clear();
	Rows.shrink_to_fit();
	MapNames.clear();
}

// Writes what has been recorded so far and starts a new run on the next frame. Desktop use: the headset has no
// keyboard, and switching perf_track off does the same thing.
CCMD(perf_track_flush)
{
	PerfTrack::Flush("perf_track_flush");
}
