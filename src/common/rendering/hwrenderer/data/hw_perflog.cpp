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
**   cpu_ms think=1.20/2.10/3.40 csthink=0.30/0.40/0.90 thinkers=1840 csthinkers=210 tics=175   ([PERFLOG] E9: per tic run)
**   governor rung=0.40/0/2 changes=4 cost_ms=8.10/9.90/13.20 pacing_ms=3.00 budget_ms=11.11 source=xr overbudget=12 missed=3 frames=449   ([GOVERNOR] E8)
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
#include "hwrenderer/postprocessing/hw_postprocess.h"	// [LIGHTMASK] the frame's light mask decision
#include "hw_drawnlinebuffer.h"
#include "hw_gpuparticlebuffer.h"	// [PARTICLEWINDOW] E1: the ring slots drawn
#include "hw_effectlightbuffer.h"	// [EFFECTLIGHTS] EffectLightStats
#include "hw_emissivevolumeframe.h"	// [EMISSIVEVOLUMES] EmissiveVolumeStats
#include "hw_framecompute.h"	// [SMOKELIGHTCULL] E6: SmokeVolumeStatus, the smoke light grid's fill
#include "hw_effectsgovernor.h"	// [GOVERNOR] E8: EffectsGovernor::LastFrame
#include "hw_perftrack.h"	// RS FORK -- perf_track: the same samples, forwarded to the per-frame record

extern bool keepGpuStatActive;	// hw_postprocess.cpp
EXTERN_CVAR(Int, r_gpuparticles_looks)	// [LOOKS] hw_particledefbuffer.cpp
EXTERN_CVAR(Bool, r_meshparticles)	// [MESHPARTICLES] hw_meshparticles.cpp
EXTERN_CVAR(Bool, gl_bloom_pin_beams)	// [LIGHTMASK] hw_postprocess_cvars.cpp
EXTERN_CVAR(Int, r_lightmask_debug)	// [LIGHTMASK] hw_postprocess_cvars.cpp
EXTERN_CVAR(Int, r_smoke_steps)	// [SMOKEVOLUME] hw_postprocess_cvars.cpp
EXTERN_CVAR(Float, r_smoke_density_scale)	// [SMOKEVOLUME] hw_postprocess_cvars.cpp
EXTERN_CVAR(Bool, r_smoke_debugslice)	// [SMOKEVOLUME] hw_postprocess_cvars.cpp
EXTERN_CVAR(Int, r_smoke_light_quality)	// [SMOKEVOLUME] 13d, hw_smokevolume.cpp
EXTERN_CVAR(Bool, r_smoke_beams)	// [SMOKEVOLUME] 13e, hw_postprocess_cvars.cpp
EXTERN_CVAR(Bool, r_smoke_beams_depth)	// [SMOKEVOLUME] 13e, hw_postprocess_cvars.cpp
EXTERN_CVAR(Bool, r_smoke_cones_depth)	// [SMOKEVOLUME] 13e, hw_postprocess_cvars.cpp
EXTERN_CVAR(Bool, r_smoke_temporal)	// [SMOKE_TEMPORAL] hw_postprocess_cvars.cpp
EXTERN_CVAR(Bool, r_effects_sharedmarch)	// [SHAREDMARCH] E2: hw_postprocess_cvars.cpp
EXTERN_CVAR(Bool, r_smoke_surfaceglow)	// [SMOKEVOLUME] 13f, hw_smokevolume.cpp
EXTERN_CVAR(Bool, r_smoke_darkness)	// [SMOKEVOLUME] 13f, hw_smokevolume.cpp
EXTERN_CVAR(Bool, r_particlecollision)	// [LEVELFIELD] hw_levelfield.cpp
EXTERN_CVAR(Int, r_particlecollision_quality)	// [LEVELFIELD] hw_levelfield.cpp
EXTERN_CVAR(Bool, r_particlecollision_test)	// [LEVELFIELD] hw_levelfield.cpp
EXTERN_CVAR(Bool, r_debris)	// [DEBRISPOOL] hw_debrispool.cpp
EXTERN_CVAR(Float, r_debris_life)	// [DEBRISPOOL] hw_debrispool.cpp
EXTERN_CVAR(Int, r_debris_pool)	// [DEBRISPOOL] hw_debrispool.cpp
EXTERN_CVAR(Bool, r_debris_test)	// [DEBRISPOOL] hw_debrispool.cpp
EXTERN_CVAR(Bool, r_effectlights)	// [EFFECTLIGHTS] hw_effectlights.cpp
EXTERN_CVAR(Int, r_effectlights_max)
EXTERN_CVAR(Int, r_effectlights_quality)
EXTERN_CVAR(Int, r_effectlights_perbin)
EXTERN_CVAR(Float, r_effectlights_distance)
EXTERN_CVAR(Bool, r_effectlights_walls)
EXTERN_CVAR(Int, r_effectlights_test)
EXTERN_CVAR(Bool, r_particlelights_test)	// [PARTICLELIGHTS] hw_gpuparticlebuffer.cpp
EXTERN_CVAR(Bool, r_gpuparticles_window)	// [PARTICLEWINDOW] E1 hw_gpuparticlebuffer.cpp
EXTERN_CVAR(Bool, r_gpuparticles_cull)	// [PARTICLECULL] E11 hw_cvars.cpp
EXTERN_CVAR(Float, r_gpuparticles_light_lod_distance)	// [PARTICLECULL] E11 hw_cvars.cpp
EXTERN_CVAR(Bool, r_debris_sounds)	// [DEBRISSOUNDS] hw_debrislanding.cpp
EXTERN_CVAR(Float, r_debris_sounds_volume)	// [DEBRISSOUNDS] hw_debrislanding.cpp
EXTERN_CVAR(Bool, r_damage)	// [SURFACEDAMAGE] hw_surfacedamage.cpp
EXTERN_CVAR(Int, r_damage_memory)	// [SURFACEDAMAGE] hw_surfacedamage.cpp
EXTERN_CVAR(Bool, r_damage_heat)	// [SURFACEDAMAGE] hw_surfacedamage.cpp
EXTERN_CVAR(Float, r_damage_soot_scale)	// [SURFACEDAMAGE] hw_surfacedamage.cpp
EXTERN_CVAR(Float, r_damage_depth_scale)	// [SURFACEDAMAGE] hw_surfacedamage.cpp
EXTERN_CVAR(Float, r_damage_heat_scale)	// [SURFACEDAMAGE] hw_surfacedamage.cpp
EXTERN_CVAR(Bool, r_damage_test)	// [SURFACEDAMAGE] hw_surfacedamage.cpp
EXTERN_CVAR(Int, gl_light_castshadows)	// [LIGHTSHADOWS] hw_shadowmap.cpp
EXTERN_CVAR(Int, gl_light_shadowmap_lights)	// [LIGHTSHADOWS] hw_shadowmap.cpp
EXTERN_CVAR(Int, r_emissivevolumes)	// [EMISSIVEVOLUMES] hw_emissivevolumes.cpp
EXTERN_CVAR(Int, r_emissivevolumes_length)
EXTERN_CVAR(Int, r_emissivevolumes_motion)
EXTERN_CVAR(Int, r_emissivevolumes_max)
EXTERN_CVAR(Bool, r_emissivevolumes_light)
EXTERN_CVAR(Float, r_emissivevolumes_brightness)
EXTERN_CVAR(Int, r_emissivevolumes_test)
EXTERN_CVAR(Int, r_emissivevolumes_steps)	// [EMISSIVEVOLUMES] hw_postprocess_cvars.cpp
EXTERN_CVAR(Int, r_emissivevolumes_resolution)
EXTERN_CVAR(Bool, r_emissivevolumes_tiles)	// [EMISSIVETILES] E5: hw_postprocess_cvars.cpp
EXTERN_CVAR(Bool, r_exposureimpulse)	// [EXPOSUREIMPULSE] hw_exposureimpulse.cpp
EXTERN_CVAR(Int, r_exposureimpulse_look)
EXTERN_CVAR(Bool, r_exposureimpulse_comfort)
EXTERN_CVAR(Float, r_exposureimpulse_strength)
EXTERN_CVAR(Float, r_exposureimpulse_cap)
EXTERN_CVAR(Float, r_exposureimpulse_recovery_scale)
EXTERN_CVAR(Bool, r_exposureimpulse_holdbeams)
EXTERN_CVAR(Int, r_exposureimpulse_test)
EXTERN_CVAR(Bool, r_effects_governor)	// [GOVERNOR] E8 hw_effectsgovernor.cpp
EXTERN_CVAR(Int, r_effects_governor_floor)
EXTERN_CVAR(Int, r_effects_governor_target)

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
		uint64_t RingSlotsSum = 0, RingSlotsMax = 0;	// [PARTICLEWINDOW] E1: ring slots the particle draws drew a frame (both eyes)
		unsigned DrawnLinesMax = 0;
		int BeamsMax = 0, StampsMax = 0, DisturbMax = 0;
		int64_t DlightsSum = 0;
		int DlightsMax = 0;
		int64_t SpritesSum = 0, WallsSum = 0, FlatsSum = 0;
		EffectLightFrameStats EffectLightsMax;	// [EFFECTLIGHTS] each number's largest frame in the window
		EmissiveVolumeFrameStats EmissiveVolumesMax;	// [EMISSIVEVOLUMES] each number's largest frame in the window
		// [LIGHTSHADOWS] The shadow map: its CPU time on each frame that ran the pass (IShadowMap::UpdateCycles -- the rows, their
		// upload and the pass's recording); the largest frame's rows, and of them the rows of lights that ask to cast; and where
		// the relink total (dynlights_links_total) stood when the window began.
		Stat ShadowCpu;
		int ShadowRowsMax = 0, ShadowAskedMax = 0;
		// [PERFLOG] E9: script time per tic run (AddThinkSample): the think and client-side think clocks, and the most thinkers
		// one run ticked.
		Stat Think, CsThink;
		int ThinkersMax = 0, CsThinkersMax = 0;
		uint64_t LinksAtStart = 0;
		// [SMOKELIGHTCULL] E6: the smoke light grid's fills (SmokeVolumeStatus): frames filled and frames skipped, and the light
		// cells the fills dispatched beside the cells the whole fill covers, summed over those frames.
		uint32_t SmokeLightFilled = 0, SmokeLightSkipped = 0;
		uint64_t SmokeLightCells = 0, SmokeLightCellsUncut = 0;
		// [GOVERNOR] E8: the effects budget governor (EffectsGovernor::LastFrame): the rung the readers used after each frame's decision
		// (sum for the average, least, most) and its changes; of the frames it measured, their cost (the frame less its pacing waits),
		// their pacing, and those whose cost passed one display period; the frames longer than one and a half periods (missed, hitches
		// included); the last frame's budget and whether it was a headset's.
		Stat GovernorCost;
		int64_t GovernorRungSum = 0;
		int GovernorRungMin = 0, GovernorRungMax = 0;
		uint32_t GovernorRungFrames = 0, GovernorChanges = 0, GovernorOverBudget = 0, GovernorMissed = 0;
		double GovernorPacingSum = 0.0, GovernorBudgetMs = 0.0;
		bool GovernorXr = false;
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
		W.RingSlotsSum = W.RingSlotsMax = 0;	// [PARTICLEWINDOW] E1
		W.DrawnLinesMax = 0;
		W.BeamsMax = W.StampsMax = W.DisturbMax = 0;
		W.DlightsSum = 0;
		W.DlightsMax = 0;
		W.SpritesSum = W.WallsSum = W.FlatsSum = 0;
		W.EffectLightsMax = EffectLightFrameStats();	// [EFFECTLIGHTS]
		W.EmissiveVolumesMax = EmissiveVolumeFrameStats();	// [EMISSIVEVOLUMES]
		W.ShadowCpu.Clear();	// [LIGHTSHADOWS]
		W.ShadowRowsMax = W.ShadowAskedMax = 0;
		W.Think.Clear();	// [PERFLOG] E9
		W.CsThink.Clear();
		W.ThinkersMax = W.CsThinkersMax = 0;
		W.LinksAtStart = dynlights_links_total;
		W.SmokeLightFilled = W.SmokeLightSkipped = 0;	// [SMOKELIGHTCULL] E6
		W.SmokeLightCells = W.SmokeLightCellsUncut = 0;
		W.GovernorCost.Clear();	// [GOVERNOR] E8
		W.GovernorRungSum = 0;
		W.GovernorRungMin = W.GovernorRungMax = 0;
		W.GovernorRungFrames = W.GovernorChanges = W.GovernorOverBudget = W.GovernorMissed = 0;
		W.GovernorPacingSum = 0.0;
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
				"are the heat shimmer passes (r_heatrefraction); pp.lightmaskcarry moves the light mask with them, and "
				"pp.lightmaskdebug is the light mask's debug view (r_lightmask_debug), drawn in place of bloom. "
				"bloom.pinned is pinned bloom's second chain for beam light and exposure.pinned the pinned look's "
				"exposure (gl_bloom_pin_beams); bloomplan is the last eye's plan: A today's bloom, B one chain "
				"carrying both looks, C the second chain.\n\n";
			// [COMPUTE] The compute names, on a legend line of their own.
			out << "Legend (compute): fx.compute is the frame's compute hook -- on cpu_fx_ms every frame while this log "
				"is on (its cost when nothing has compute work), on gpu_ms only when compute work was recorded. fx.smokesim "
				"is ONE smoke volume step, counted per step, not per frame (avg/p95/max per step; its GPU group nests inside "
				"fx.compute: its kernels, advection and tile maps). fx.smokeshift is one recentre of the volume, per run. "
				"fx.sectorplanes is the renderer's sector plane poll; fx.smokemask the solid mask's rasterisation on the CPU "
				"(frames with mask work only). pp.smoke is the smoke volume's drawing per eye (depth, march, blur, composite; "
				"only while there is smoke); a pp.lightmaskcarry beside it dims the light mask by the same haze; fx.smokedraw "
				"(cpu_fx_ms) is its per-eye setup. fx.smokelight is the smoke's light grid (frames with smoke: its ambient pass, "
				"one pass per light, and one pass for every effect light in reach [EFFECTLIGHTS LD]; on gpu_ms inside "
				"fx.compute, on cpu_fx_ms its recording); fx.smokelights (cpu_fx_ms) "
				"its light list and ambient columns [13f: and, while glow, sweep bands, darkness or a passed look reach the "
				"smoke, their records and the columns' edge distances; the ambient pass inside fx.smokelight then runs their "
				"variant]. pp.smokebeams is the smoke's beam and cone work per eye (the transmittance "
				"curve, and the light the beams scatter; only while beams or a flashlight cone meet the smoke) -- the beams' "
				"depth work is inside pp.smoke's composite and the cones' inside volumetricbeam; smokebeams is how many beams "
				"the last frame's smoke list held. fx.levelfield is the particle collision field: on cpu_fx_ms its demand "
				"scan, windows and tile rasterisation (SH1, within 1 ms a frame while it builds or a door moves), on gpu_ms "
				"its invalidations, uploads and line bakes (frames with field work only). fx.debrissim is ONE debris pool step, "
				"counted per step (gpu_ms, inside fx.compute); fx.debrispool (cpu_fx_ms) is the pool's CPU side on each frame it "
				"is asked for: bursts into pieces, slots, pushes, wake boxes and the mesh instance list. fx.debrisland (cpu_fx_ms) "
				"is the debris landing sounds on frames with work: flying each new burst's pieces to their first landing, the "
				"level check of the earliest, and starting the sounds that are due. fx.damagepaint is the surface damage atlas: "
				"on gpu_ms its stamps, cooling and mip rebuilds (frames with damage work, inside fx.compute); on cpu_fx_ms its "
				"paint traces, cell cover, tiles, anchors and dispatch lists (every frame while damage is held).\n\n";
			// [PERFLOG] E9: script time, on a legend line of its own.
			out << "Legend (script time): the second cpu_ms line is script time per tic run, not per frame. think is the playsim "
				"thinkers' clock (FThinkerCollection::RunThinkers: every thinker's Tick, ZScript included, and the sleep cycle); csthink "
				"the client-side thinkers' (RunClientSideThinkers, with the effects and lights it drives); both avg/p95/max over the "
				"window's runs. thinkers and csthinkers are the most one run ticked, tics the playsim runs in the window (0 while "
				"paused). These are the numbers \"stat think\" shows.\n\n";
			// [EFFECTLIGHTS] The effect light names, on a legend line of their own.
			out << "Legend (effect lights): fx.effectlights (cpu_fx_ms) is effect lights' CPU work each frame while they are on: "
				"the queue, moving and ranking the lights, and the bins and their upload. load effectlights=live/binned/merged/evicted "
				"is each number's largest frame in the window: lights alive (fire-and-forget and drawn-line lights), lights binned, "
				"crowded bins blended into one glow, and lights evicted or refused because the pool was full. effectlightrows is the "
				"shadow-map rows they took (walls block them; the map pass then runs even with light shadows off, and shows as gpu_ms "
				"shadowmap); effectlightlines the line lights binned; effectlightsdropped=norow/trimmed the lights not lit because the "
				"rows ran out or the bins' index list was full.\n\n";
			// [EMISSIVEVOLUMES] The emissive volume names, on a legend line of their own.
			out << "Legend (emissive volumes): pp.emissive (gpu_ms) is the emissive volumes' drawing per eye -- depth, march, blur, "
				"composite -- only while a volume is drawn; a pp.lightmaskcarry beside it only while a drawn volume absorbs. In haze the "
				"smoke's pp.smokebeams also holds the transmittance curve they are dimmed by. fx.emissive (cpu_fx_ms) is their CPU work: "
				"the queue, the pool, the hand follow, their lights, the list and each eye's setup. load emissive=live/drawn/refused is "
				"each number's largest frame in the window: volumes alive, volumes in the frame's list (at most 32), and volumes refused "
				"(the pool full, no definition on this machine, or a class \"Volumetric flashes\" does not draw); emissivelights the "
				"effect lights they handed over.\n\n";
			// [EMISSIVETILES] E5: the march's tile lists, on a legend line of their own.
			out << "Legend (emissive tiles): with r_emissivevolumes_tiles on (and more than one volume drawn, or haze in front) the "
				"emissive march cuts its target into 16x16-texel tiles, and each texel loops only the drawn volumes whose widened screen "
				"bound touches its tile; pp.emissive then holds the tile mask's small draw too. The image is the same either way. load "
				"emissivetiles=lit/inrect/all is, both eyes summed, the tiles listing at least one volume, the tiles the eyes' "
				"rectangles touch (without tiles every texel there loops every drawn volume), and all their tiles; emissivetileentries "
				"the volume-tile pairs listed (without tiles, inrect x drawn); emissivetileeyes the eyes that used tile lists. Each is "
				"its largest frame in the window, counted only while the log runs.\n\n";
		// [PARTICLEWINDOW] E1: the ring window, on a legend line of its own.
		out << "Legend (particle window): load ringslots=avg/max of N is the GPU particle ring's slots the particle draws drew a frame "
			"(both eyes summed), beside the ring's size: with r_gpuparticles_window on only the chunks a live record lies in, off the "
			"whole ring on every draw. fx.gpuparticles is their time; the image is the same either way.\n\n";
			// [EXPOSUREIMPULSE] The flash blindness names, on a legend line of their own.
			out << "Legend (flash blindness): pp.exposureimpulse (gpu_ms) is the wash per eye -- one full-screen draw, and on a burst's "
				"first draw a one-texel copy of the exposure meter -- only while a wash is on screen. fx.exposureimpulse (cpu_fx_ms) is its "
				"CPU work on frames with flashes or a live wash: the queue, each flash's sight ray and the envelope. exposureimpulselive "
				"on the block line is 1 when the last frame drew a wash.\n\n";
			// [SMOKELIGHTCULL] E6: the smoke light grid's cull, on a legend line of its own.
			out << "Legend (smoke light cull): load smokelight=filled/skipped counts the window's frames whose smoke light grid was "
				"filled, and those whose fill was skipped because nothing it reads changed (r_smoke_light_cull on the block line); "
				"smokelightcells=dispatched/whole is the light cells the fill's dispatches covered per such frame, beside the cells the "
				"whole fill covers (every cell of the grid, each light's box, the effect lights' region). The light is the same either "
				"way; fx.smokelight is its time.\n\n";
			// [GOVERNOR] E8: the effects budget governor, on a legend line of its own.
			out << "Legend (effects governor): the governor line is per frame over the window. rung=avg/least/most is the trim the effect "
				"readers used (0 full; 1-5 smoke detail, flash detail, flashes drawn, particles and effect lights per box at 3/4; 6-10 the "
				"same at 1/2; never past r_effects_governor_floor), changes how often it moved. cost_ms is what the governor judges, "
				"avg/p95/max: each frame's time less its pacing -- the waits on the display (xrWaitFrame and the headset swapchain's wait, "
				"a swapchain's acquire and present, the fps limiter) -- whose average is pacing_ms. budget_ms is one display period "
				"(source xr: the headset's; desktop: 1/60 s, or the fps cap under 60). overbudget counts the measured frames whose cost "
				"passed the budget, missed the frames longer than one and a half periods (hitches included). The block line names "
				"r_effects_governor, r_effects_governor_floor and r_effects_governor_target.\n\n";
			// [LIGHTSHADOWS] The light shadow names, on a legend line of their own.
			out << "Legend (light shadows): gpu_ms shadowmap is the shadow map pass (once a frame, both eyes share it; only while it "
				"runs). load shadowlights=rows/asked is the largest frame's shadow-map rows (effect light rows included) and, of them, "
				"the rows of lights that ask to cast shadows (LF_CASTSHADOW: muzzle flashes, or any light a mod marks); shadowcpu_ms "
				"is the pass's CPU side (the rows, their upload, the pass's recording), avg/p95/max over the frames that ran it; "
				"relinks is how many times dynamic lights' section lists were rebuilt in the window (moving or resizing lights, each "
				"tic). The block line names gl_light_shadowmap (Light shadows), gl_light_castshadows (shadows for lights that ask: 0 "
				"off, 1 shadow maps, 2 ray traced), gl_light_shadowmap_lights (what that reaches: 0 lights that ask, 1 all lights), "
				"gl_shadowmap_quality and gl_shadowmap_filter, vk_raytrace, and raytraced: 1 when this session's shaders ray trace "
				"light shadows (decided at start-up).\n\n";
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
		// [MESHPARTICLES] And the mesh particle switch, so a fx.meshparticles before/after
		// (chunks as meshes vs as cards) labels itself.
		out.AppendFormat(" r_meshparticles=%d", (int)*r_meshparticles);
		// [HEATREFRACTION] And the heat shimmer switch, so a pp.heatoffset / pp.heatwarp
		// before/after labels itself.
		out.AppendFormat(" r_heatrefraction=%d", (int)*r_heatrefraction);
		// [SMOKEVOLUME] And the smoke switches, so a fx.compute / fx.smokesim / fx.smokemask / pp.smoke
		// before/after labels itself.
		out.AppendFormat(" r_smoke=%d r_smoke_quality=%d r_smoke_computetest=%d r_smoke_dissipation_scale=%g r_smoke_steps=%d r_smoke_density_scale=%g r_smoke_debugslice=%d r_smoke_light_quality=%d",
			(int)*r_smoke, (int)*r_smoke_quality, (int)*r_smoke_computetest, (double)(float)*r_smoke_dissipation_scale,
			(int)*r_smoke_steps, (double)(float)*r_smoke_density_scale, (int)*r_smoke_debugslice, (int)*r_smoke_light_quality);
		// [SMOKEVOLUME] 13e: and the beam and cone switches, with the beams the smoke drawing last published, so a
		// pp.smoke / pp.smokebeams / volumetricbeam before/after labels itself.
		out.AppendFormat(" r_smoke_beams=%d r_smoke_beams_depth=%d r_smoke_cones_depth=%d smokebeams=%d",
			(int)*r_smoke_beams, (int)*r_smoke_beams_depth, (int)*r_smoke_cones_depth, hw_postprocess.smokevolume.PublishedBeamCount());
		// [SMOKEVOLUME] 13f: and the surface light switches, so a fx.smokelight / fx.smokelights before/after labels itself.
		out.AppendFormat(" r_smoke_surfaceglow=%d r_smoke_darkness=%d", (int)*r_smoke_surfaceglow, (int)*r_smoke_darkness);
		// [SMOKE_TEMPORAL] And the march's temporal accumulation, so a pp.smoke before/after labels itself.
		out.AppendFormat(" r_smoke_temporal=%d", (int)*r_smoke_temporal);
		// [SHAREDMARCH] E2: and whether the smoke and the emissive volumes marched once from between the eyes, so a pp.smoke
		// or pp.emissive before/after labels itself.
		out.AppendFormat(" r_effects_sharedmarch=%d", (int)*r_effects_sharedmarch);
		// [SMOKELIGHTCULL] E6: and the light grid cull, so a fx.smokelight before/after labels itself. Looked up by name: the Vulkan
		// backend defines it.
		{
			FBaseCVar* lightCull = FindCVar("r_smoke_light_cull", nullptr);
			out.AppendFormat(" r_smoke_light_cull=%s", lightCull != nullptr ? (lightCull->GetGenericRep(CVAR_Int).Int ? "1" : "0") : "n/a");
		}
		// [GOVERNOR] E8: and the effects budget governor, so a frame_ms / governor before/after labels itself.
		out.AppendFormat(" r_effects_governor=%d r_effects_governor_floor=%d r_effects_governor_target=%d",
			(int)*r_effects_governor, (int)*r_effects_governor_floor, (int)*r_effects_governor_target);
		// [LEVELFIELD] And the particle collision switches, so a fx.levelfield before/after labels itself.
		out.AppendFormat(" r_particlecollision=%d r_particlecollision_quality=%d r_particlecollision_test=%d",
			(int)*r_particlecollision, (int)*r_particlecollision_quality, (int)*r_particlecollision_test);
		// [DEBRISPOOL] And the debris pool's switches, so a fx.debrissim / fx.debrispool before/after labels itself.
		out.AppendFormat(" r_debris=%d r_debris_life=%g r_debris_pool=%d r_debris_test=%d",
			(int)*r_debris, (double)(float)*r_debris_life, (int)*r_debris_pool, (int)*r_debris_test);
		// [DEBRISSOUNDS] And the landing sounds' switches, so a fx.debrisland before/after labels itself.
		out.AppendFormat(" r_debris_sounds=%d r_debris_sounds_volume=%g", (int)*r_debris_sounds, (double)(float)*r_debris_sounds_volume);
		// [SURFACEDAMAGE] And the wall damage switches, so a fx.damagepaint / scene.opaque before/after labels itself.
		out.AppendFormat(" r_damage=%d r_damage_memory=%d r_damage_heat=%d r_damage_soot_scale=%g r_damage_depth_scale=%g r_damage_heat_scale=%g r_damage_test=%d",
			(int)*r_damage, (int)*r_damage_memory, (int)*r_damage_heat, (double)(float)*r_damage_soot_scale, (double)(float)*r_damage_depth_scale,
			(double)(float)*r_damage_heat_scale, (int)*r_damage_test);
		// [EFFECTLIGHTS] And the effect light switches, so a fx.effectlights / scene.* / shadowmap before/after labels itself.
		out.AppendFormat(" r_effectlights=%d r_effectlights_max=%d r_effectlights_quality=%d r_effectlights_perbin=%d r_effectlights_distance=%g r_effectlights_walls=%d r_effectlights_test=%d",
			(int)*r_effectlights, (int)*r_effectlights_max, (int)*r_effectlights_quality, (int)*r_effectlights_perbin,
			(double)(float)*r_effectlights_distance, (int)*r_effectlights_walls, (int)*r_effectlights_test);
		// [PARTICLELIGHTS] And the particle light test, so a before/after with it labels itself.
		out.AppendFormat(" r_particlelights_test=%d", (int)*r_particlelights_test);
		// [PARTICLEWINDOW] E1: and the ring window, so a fx.gpuparticles before/after labels itself.
		out.AppendFormat(" r_gpuparticles_window=%d", (int)*r_gpuparticles_window);
		// [PARTICLECULL] E11: and the vertex cull and the effect light LOD, so a fx.gpuparticles before/after labels itself.
		out.AppendFormat(" r_gpuparticles_cull=%d r_gpuparticles_light_lod_distance=%g", (int)*r_gpuparticles_cull, (double)(float)*r_gpuparticles_light_lod_distance);
		// [EMISSIVEVOLUMES] And the emissive volume switches, so a pp.emissive / fx.emissive before/after labels itself.
		out.AppendFormat(" r_emissivevolumes=%d r_emissivevolumes_length=%d r_emissivevolumes_motion=%d r_emissivevolumes_steps=%d r_emissivevolumes_resolution=%d r_emissivevolumes_max=%d r_emissivevolumes_light=%d r_emissivevolumes_brightness=%g r_emissivevolumes_test=%d",
			(int)*r_emissivevolumes, (int)*r_emissivevolumes_length, (int)*r_emissivevolumes_motion, (int)*r_emissivevolumes_steps,
			(int)*r_emissivevolumes_resolution, (int)*r_emissivevolumes_max, (int)*r_emissivevolumes_light,
			(double)(float)*r_emissivevolumes_brightness, (int)*r_emissivevolumes_test);
		// [EMISSIVETILES] E5: and the march's tile lists, so a pp.emissive before/after labels itself.
		out.AppendFormat(" r_emissivevolumes_tiles=%d", (int)*r_emissivevolumes_tiles);
		// [EXPOSUREIMPULSE] And the flash blindness switches, with whether a wash is live, so a pp.exposureimpulse /
		// fx.exposureimpulse before/after labels itself.
		out.AppendFormat(" r_exposureimpulse=%d r_exposureimpulse_look=%d r_exposureimpulse_comfort=%d r_exposureimpulse_strength=%g r_exposureimpulse_cap=%g r_exposureimpulse_recovery_scale=%g r_exposureimpulse_holdbeams=%d r_exposureimpulse_test=%d exposureimpulselive=%d",
			(int)*r_exposureimpulse, (int)*r_exposureimpulse_look, (int)*r_exposureimpulse_comfort, (double)(float)*r_exposureimpulse_strength,
			(double)(float)*r_exposureimpulse_cap, (double)(float)*r_exposureimpulse_recovery_scale, (int)*r_exposureimpulse_holdbeams,
			(int)*r_exposureimpulse_test, (int)hw_postprocess.exposureimpulse.Live());
		// [LIGHTSHADOWS] And the light shadow switches, so a shadowmap / scene.* before/after labels itself. vk_raytrace is looked
		// up by name (the Vulkan backend defines it); raytraced is what this session's shaders really do.
		{
			FBaseCVar* raytrace = FindCVar("vk_raytrace", nullptr);
			out.AppendFormat(" gl_light_shadowmap=%d gl_light_castshadows=%d gl_light_shadowmap_lights=%d gl_shadowmap_quality=%d gl_shadowmap_filter=%d vk_raytrace=%s raytraced=%d",
				(int)*gl_light_shadowmap, (int)*gl_light_castshadows, (int)*gl_light_shadowmap_lights, (int)*gl_shadowmap_quality,
				(int)*gl_shadowmap_filter, raytrace != nullptr ? (raytrace->GetGenericRep(CVAR_Int).Int ? "1" : "0") : "n/a",
				(int)IShadowMap::RaytracedThisSession);
		}
		// [LIGHTMASK] And the light mask, so a scene.* / pp.lightmaskcarry before/after labels itself
		// (lightmask: 1 while the scene draws the mask this frame).
		out.AppendFormat(" gl_bloom_pin_beams=%d r_lightmask_debug=%d lightmask=%d",
			(int)*gl_bloom_pin_beams, (int)*r_lightmask_debug, (int)hw_postprocess.lightmask.Active());
		// [PINNEDBLOOM] And the bloom plan the last eye ran, so a bloom / bloom.pinned before/after labels itself.
		out.AppendFormat(" bloomplan=%c", "ABC"[(int)hw_postprocess.bloom.LastPlan()]);
		out.AppendFormat(" t=%.1fs window=%.1fs frames=%u fps=%.1f frame_ms avg=%.2f p95=%.2f max=%.2f\n",
			I_msTime() / 1000.0, windowS, frames, fps, W.Frame.Avg(), W.Frame.P95(), W.Frame.Max);

		out << "cpu_ms ";
		for (auto& c : W.Cpu) AppendTriple(out, c);
		out << "\n";
		// [PERFLOG] E9: script time, per tic run.
		out.AppendFormat("cpu_ms think=%.2f/%.2f/%.2f csthink=%.2f/%.2f/%.2f thinkers=%d csthinkers=%d tics=%u\n",
			W.Think.Avg(), W.Think.P95(), W.Think.Max, W.CsThink.Avg(), W.CsThink.P95(), W.CsThink.Max,
			W.ThinkersMax, W.CsThinkersMax, W.Think.Count);
		// [GOVERNOR] E8: the effects budget governor, per frame over the window.
		out.AppendFormat("governor rung=%.2f/%d/%d changes=%u cost_ms=%.2f/%.2f/%.2f pacing_ms=%.2f budget_ms=%.2f source=%s overbudget=%u missed=%u frames=%u\n",
			W.GovernorRungFrames > 0 ? (double)W.GovernorRungSum / W.GovernorRungFrames : 0.0, W.GovernorRungMin, W.GovernorRungMax,
			W.GovernorChanges, W.GovernorCost.Avg(), W.GovernorCost.P95(), W.GovernorCost.Max,
			W.GovernorCost.Count > 0 ? W.GovernorPacingSum / W.GovernorCost.Count : 0.0, W.GovernorBudgetMs, W.GovernorXr ? "xr" : "desktop",
			W.GovernorOverBudget, W.GovernorMissed, W.GovernorCost.Count);

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
		out.AppendFormat("load    particles_spawned=%llu drawnlines_live=%u beams=%d stamps_live=%d disturb_live=%d dlights=%.0f/%d sprites=%.0f walls=%.0f flats=%.0f",
			(unsigned long long)W.ParticlesSpawned, W.DrawnLinesMax, W.BeamsMax, W.StampsMax, W.DisturbMax,
			W.DlightsSum / n, W.DlightsMax, W.SpritesSum / n, W.WallsSum / n, W.FlatsSum / n);
		// [EFFECTLIGHTS] The effect light load, on the same line.
		out.AppendFormat(" effectlights=%d/%d/%d/%d effectlightrows=%d effectlightlines=%d effectlightsdropped=%d/%d",
			W.EffectLightsMax.Live, W.EffectLightsMax.Binned, W.EffectLightsMax.Merged, W.EffectLightsMax.Evicted,
			W.EffectLightsMax.Rows, W.EffectLightsMax.Lines, W.EffectLightsMax.NoRow, W.EffectLightsMax.Trimmed);
		// [PARTICLEWINDOW] E1: the ring slots the particle draws drew a frame (both eyes), avg/max, and the ring's size.
		out.AppendFormat(" ringslots=%.0f/%llu of %u", (double)W.RingSlotsSum / n, (unsigned long long)W.RingSlotsMax,
			(screen != nullptr && screen->mGpuParticles != nullptr) ? screen->mGpuParticles->GetRingSize() : 0u);
		// [EMISSIVEVOLUMES] The emissive volume load, on the same line.
		out.AppendFormat(" emissive=%d/%d/%d emissivelights=%d", W.EmissiveVolumesMax.Live, W.EmissiveVolumesMax.Drawn,
			W.EmissiveVolumesMax.Refused, W.EmissiveVolumesMax.Lights);
		// [EMISSIVETILES] E5: the march's tile lists, on the same line.
		out.AppendFormat(" emissivetiles=%d/%d/%d emissivetileentries=%d emissivetileeyes=%d", W.EmissiveVolumesMax.TilesLit,
			W.EmissiveVolumesMax.TilesInRect, W.EmissiveVolumesMax.Tiles, W.EmissiveVolumesMax.TileEntries, W.EmissiveVolumesMax.TileEyes);
		// [SMOKELIGHTCULL] E6: the smoke light grid's fills, on the same line.
		{
			const uint32_t smokeFrames = W.SmokeLightFilled + W.SmokeLightSkipped;
			const double perFrame = smokeFrames > 0 ? (double)smokeFrames : 1.0;
			out.AppendFormat(" smokelight=%u/%u smokelightcells=%.0f/%.0f", W.SmokeLightFilled, W.SmokeLightSkipped,
				(double)W.SmokeLightCells / perFrame, (double)W.SmokeLightCellsUncut / perFrame);
		}
		// [LIGHTSHADOWS] The light shadow load, closing the line.
		out.AppendFormat(" shadowlights=%d/%d shadowcpu_ms=%.2f/%.2f/%.2f relinks=%llu\n\n",
			W.ShadowRowsMax, W.ShadowAskedMax, W.ShadowCpu.Avg(), W.ShadowCpu.P95(), W.ShadowCpu.Max,
			(unsigned long long)(dynlights_links_total - W.LinksAtStart));

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
	// RS FORK -- perf_track: the per-frame record keeps this sample under its own name for the frame it fell in.
	// Before every early return below, so a CountEachRun name reaches it too.
	if (PerfTrack::Active()) PerfTrack::NoteGpuGroup(name, ms);
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
	// RS FORK -- perf_track: as AddGpuSample, on the CPU side's own table.
	if (PerfTrack::Active()) PerfTrack::NoteCpuGroup(name, ms);
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

// [PERFLOG] E9: see the header. One sample per run; the thinker count is the run's own.
void PerfLog::AddThinkSample(bool clientSide, double ms, int thinkers)
{
	// RS FORK -- perf_track: the same tic run, summed into the frame it fell in.
	if (PerfTrack::Active()) PerfTrack::NoteThink(clientSide, ms, thinkers);
	if (clientSide)
	{
		W.CsThink.Add(ms);
		if (thinkers > W.CsThinkersMax) W.CsThinkersMax = thinkers;
	}
	else
	{
		W.Think.Add(ms);
		if (thinkers > W.ThinkersMax) W.ThinkersMax = thinkers;
	}
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
	// [PARTICLEWINDOW] E1: the ring slots this frame's particle draws drew (both eyes).
	if (screen != nullptr && screen->mGpuParticles != nullptr)
	{
		const uint64_t slots = screen->mGpuParticles->TakeDrawnSlots();
		W.RingSlotsSum += slots;
		if (slots > W.RingSlotsMax) W.RingSlotsMax = slots;
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
	// [EFFECTLIGHTS] This frame's effect light load (hw_effectlights.cpp): each number's largest frame.
	{
		const EffectLightFrameStats& e = EffectLightStats();
		EffectLightFrameStats& m = W.EffectLightsMax;
		if (e.Live > m.Live) m.Live = e.Live;
		if (e.Binned > m.Binned) m.Binned = e.Binned;
		if (e.Merged > m.Merged) m.Merged = e.Merged;
		if (e.Evicted > m.Evicted) m.Evicted = e.Evicted;
		if (e.NoRow > m.NoRow) m.NoRow = e.NoRow;
		if (e.Trimmed > m.Trimmed) m.Trimmed = e.Trimmed;
		if (e.Rows > m.Rows) m.Rows = e.Rows;
		if (e.Lines > m.Lines) m.Lines = e.Lines;
	}
	// [EMISSIVEVOLUMES] This frame's emissive volume load (hw_emissivevolumes.cpp): each number's largest frame.
	{
		const EmissiveVolumeFrameStats& e = EmissiveVolumeStats();
		EmissiveVolumeFrameStats& m = W.EmissiveVolumesMax;
		if (e.Live > m.Live) m.Live = e.Live;
		if (e.Drawn > m.Drawn) m.Drawn = e.Drawn;
		if (e.Refused > m.Refused) m.Refused = e.Refused;
		if (e.Lights > m.Lights) m.Lights = e.Lights;
		// [EMISSIVETILES] E5
		if (e.TileEyes > m.TileEyes) m.TileEyes = e.TileEyes;
		if (e.Tiles > m.Tiles) m.Tiles = e.Tiles;
		if (e.TilesInRect > m.TilesInRect) m.TilesInRect = e.TilesInRect;
		if (e.TilesLit > m.TilesLit) m.TilesLit = e.TilesLit;
		if (e.TileEntries > m.TileEntries) m.TileEntries = e.TileEntries;
	}
	// [LIGHTSHADOWS] This frame's shadow map, when the frame ran the pass (IShadowMap::UpdateSerial moved on; its counters keep
	// the last pass's values otherwise).
	{
		static uint64_t lastShadowSerial = 0;
		if (IShadowMap::UpdateSerial != lastShadowSerial)
		{
			lastShadowSerial = IShadowMap::UpdateSerial;
			W.ShadowCpu.Add(IShadowMap::UpdateCycles.TimeMS());
			if (IShadowMap::LightsShadowmapped > W.ShadowRowsMax) W.ShadowRowsMax = IShadowMap::LightsShadowmapped;
			if (IShadowMap::LightsCastShadow > W.ShadowAskedMax) W.ShadowAskedMax = IShadowMap::LightsCastShadow;
		}
	}
	// [SMOKELIGHTCULL] E6: this frame's smoke light grid fill (vk_smokevolume.cpp): filled or skipped, and its light cells.
	{
		const SmokeVolumeBackendStatus& smoke = SmokeVolumeStatus();
		if (smoke.LightFill == 1)
			W.SmokeLightFilled++;
		else if (smoke.LightFill == 2)
			W.SmokeLightSkipped++;
		if (smoke.LightFill != 0)
		{
			W.SmokeLightCells += smoke.LightCells;
			W.SmokeLightCellsUncut += smoke.LightCellsUncut;
		}
	}
	// [GOVERNOR] E8: this frame's governor decision and measurement (EffectsGovernor::EndFrame ran just before, in End2DAndUpdate).
	{
		const EffectsGovernor::FrameInfo& g = EffectsGovernor::LastFrame();
		if (W.GovernorRungFrames == 0 || g.Rung < W.GovernorRungMin) W.GovernorRungMin = g.Rung;
		if (W.GovernorRungFrames == 0 || g.Rung > W.GovernorRungMax) W.GovernorRungMax = g.Rung;
		W.GovernorRungSum += g.Rung;
		W.GovernorRungFrames++;
		if (g.Changed) W.GovernorChanges++;
		if (g.Valid)
		{
			W.GovernorCost.Add(g.CostMs);
			W.GovernorPacingSum += g.PacingMs;
			if (g.CostMs > g.BudgetMs) W.GovernorOverBudget++;
		}
		if (g.BudgetMs > 0.0)
		{
			if (g.IntervalMs > g.BudgetMs * 1.5) W.GovernorMissed++;
			W.GovernorBudgetMs = g.BudgetMs;
			W.GovernorXr = g.Xr;
		}
	}

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
