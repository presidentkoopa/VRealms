/*
** hw_clock.cpp
**
** Hardware render profiling info
**
**---------------------------------------------------------------------------
**
** Copyright 2007-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/


#include "c_console.h"
#include "c_dispatch.h"
#include "v_video.h"
#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "a_dynlight.h"
#include "hw_cvars.h"
#include "hw_perflog.h"	// RS FORK -- r_perflog keeps the CPU timers active

static const char* GetOpenXrSyncModeName(int syncMode)
{
	switch (syncMode)
	{
	case 1:
		return "defer_desktop_present";
	default:
		return "legacy";
	}
}
#include "i_time.h"
#include "i_interface.h"
#include "printf.h"
#include "version.h"
#include "cmdlib.h"

glcycle_t RenderWall,SetupWall,ClipWall;
glcycle_t RenderFlat,SetupFlat;
glcycle_t RenderSprite,SetupSprite;
glcycle_t All, Finish, PortalAll, Bsp;
glcycle_t ProcessAll, PostProcess;
glcycle_t VRSceneEyes, VRSceneBuild, VRSubsectors, VRSubsectorCull, VRSubsectorVisible, VRLineBuild, VRLineClip, VRLineDecide, VRThingBuild, VRFlatBuild, VRScenePostBSP, VRPlayerSprites, VRSceneDraw, VREyeComposite, VRFinalizeEye, VRSubmit;
glcycle_t VRPostProcessScene, VRSceneTransfer, VRFinalPresent, VRSubmitCopy, VRSubmitWait, VRRenderSyncWait;
glcycle_t RenderAll;
glcycle_t Dirty;
glcycle_t drawcalls;
glcycle_t twoD, Flush3D;
glcycle_t MTWait, WTTotal;
glcycle_t WTWallJobs, WTFlatJobs, WTThingJobs;
glcycle_t WallWorkersElapsed, WallMerge, SceneWorkerElapsed;
int64_t WallWorkersCpuSumCycles, WallWorkersWallCpuSumCycles;
int64_t WallWorkersElapsedCycles;
int WallBatchCount, WallItemsProcessed;
int VRFinalPresentPasses, VRMirrorPreparePasses, VRSceneTransferOps, VRSubmitLayerBlits;
int vertexcount, flatvertices, flatprimitives;

int rendered_lines,rendered_flats,rendered_sprites,render_vertexsplit,render_texsplit,rendered_decals, rendered_portals, rendered_commandbuffers;
int iter_dlightf, iter_dlight, draw_dlight, draw_dlightf;
int dynlights_active_updates, dynlights_link_calls, dynlights_relink_calls, dynlights_unlink_calls;
uint64_t dynlights_links_total = 0;	// [LIGHTSHADOWS] never reset (hw_clock.h)
int dynlights_collected_subsectors, dynlights_linked_sectors, dynlights_linked_sides;
int dynlights_removed_sector_links, dynlights_removed_side_links;
int dynlights_distance_culled_walls, dynlights_distance_culled_flats, dynlights_distance_culled_models;
int dynlights_model_subsectors, dynlights_model_candidates, dynlights_model_duplicates, dynlights_model_uploads;
int lightbuffer_curindex, vertexbuffer_curindex, bonebuffer_curindex;

void ResetProfilingData()
{
	All.Reset();
	All.Clock();
	Bsp.Reset();
	PortalAll.Reset();
	RenderAll.Reset();
	ProcessAll.Reset();
	PostProcess.Reset();
	VRSceneEyes.Reset();
	VRSceneBuild.Reset();
	VRSubsectors.Reset();
	VRSubsectorCull.Reset();
	VRSubsectorVisible.Reset();
	VRLineBuild.Reset();
	VRLineClip.Reset();
	VRLineDecide.Reset();
	VRThingBuild.Reset();
	VRFlatBuild.Reset();
	VRScenePostBSP.Reset();
	VRPlayerSprites.Reset();
	VRSceneDraw.Reset();
	VREyeComposite.Reset();
	VRFinalizeEye.Reset();
	VRSubmit.Reset();
	VRPostProcessScene.Reset();
	VRSceneTransfer.Reset();
	VRFinalPresent.Reset();
	VRSubmitCopy.Reset();
	VRSubmitWait.Reset();
	VRRenderSyncWait.Reset();
	RenderWall.Reset();
	SetupWall.Reset();
	ClipWall.Reset();
	RenderFlat.Reset();
	SetupFlat.Reset();
	RenderSprite.Reset();
	SetupSprite.Reset();
	drawcalls.Reset();
	MTWait.Reset();
	WTTotal.Reset();
	WTWallJobs.Reset();
	WTFlatJobs.Reset();
	WTThingJobs.Reset();
	WallWorkersElapsed.Reset();
	WallMerge.Reset();
	SceneWorkerElapsed.Reset();
	WallWorkersCpuSumCycles = 0;
	WallWorkersWallCpuSumCycles = 0;
	WallWorkersElapsedCycles = 0;
	WallBatchCount = 0;
	WallItemsProcessed = 0;
	VRFinalPresentPasses = 0;
	VRMirrorPreparePasses = 0;
	VRSceneTransferOps = 0;
	VRSubmitLayerBlits = 0;

	flatvertices=flatprimitives=vertexcount=0;
	render_texsplit=render_vertexsplit=rendered_lines=rendered_flats=rendered_sprites=rendered_decals=rendered_portals = 0;
	dynlights_active_updates = dynlights_link_calls = dynlights_relink_calls = dynlights_unlink_calls = 0;
	dynlights_collected_subsectors = dynlights_linked_sectors = dynlights_linked_sides = 0;
	dynlights_removed_sector_links = dynlights_removed_side_links = 0;
	dynlights_distance_culled_walls = dynlights_distance_culled_flats = dynlights_distance_culled_models = 0;
	dynlights_model_subsectors = dynlights_model_candidates = dynlights_model_duplicates = dynlights_model_uploads = 0;
	lightbuffer_curindex = vertexbuffer_curindex = bonebuffer_curindex = 0;
}

//-----------------------------------------------------------------------------
//
// Rendering statistics
//
//-----------------------------------------------------------------------------

// RS FORK -- the VR summary buckets, shared by AppendRenderTimes and r_perflog
// (hw_perflog.cpp) so the two can never disagree about what "Scene" means.
void GetRenderTimeSummary(RenderTimeSummary& out)
{
	out.Scene = VRSceneBuild.TimeMS() + VRSceneDraw.TimeMS() + VRScenePostBSP.TimeMS() + VRPlayerSprites.TimeMS();
	out.Post = PostProcess.TimeMS();
	out.Finalize = VRFinalizeEye.TimeMS() + VRFinalPresent.TimeMS();
	out.Submit = VRSubmit.TimeMS();
	out.Composite = VREyeComposite.TimeMS();
	out.SyncWait = VRSubmitWait.TimeMS() + VRRenderSyncWait.TimeMS();
	out.All = All.TimeMS() + Finish.TimeMS();
	out.Drawcalls = drawcalls.TimeMS();
}

static void AppendRenderTimes(FString &str)
{
	double setupwall = SetupWall.TimeMS();
	double clipwall = ClipWall.TimeMS();
	double bsp = Bsp.TimeMS() - ClipWall.TimeMS();
	double vrThingBuild = VRThingBuild.TimeMS() + WTThingJobs.TimeMS();
	double vrFlatBuild = VRFlatBuild.TimeMS() + WTFlatJobs.TimeMS();
	double wallWorkersElapsed = WallWorkersElapsed.TimeMS();
	if (WallWorkersElapsedCycles > 0)
	{
		wallWorkersElapsed = WallWorkersElapsedCycles * PerfToMillisec;
	}
	double wallWorkersCpuSum = WallWorkersCpuSumCycles * PerfToMillisec;
	double wallWorkersWallCpuSum = WallWorkersWallCpuSumCycles * PerfToMillisec;
	const double wallWorkerParallelism = wallWorkersElapsed > 0.0 ? wallWorkersCpuSum / wallWorkersElapsed : 0.0;
	const double wallWorkerBusyParallelism = wallWorkersElapsed > 0.0 ? wallWorkersWallCpuSum / wallWorkersElapsed : 0.0;
	// RS FORK -- read through GetRenderTimeSummary, which r_perflog shares.
	RenderTimeSummary summary;
	GetRenderTimeSummary(summary);
	const double vrSceneBucket = summary.Scene;
	const double vrPostprocessBucket = summary.Post;
	const double vrFinalizeBucket = summary.Finalize;
	const double vrSubmitBucket = summary.Submit;
	const double vrCompositeBucket = summary.Composite;
	const double vrSyncWaitBucket = summary.SyncWait;

	str.AppendFormat("VR Summary: Scene=%2.3f Post=%2.3f Finalize=%2.3f Submit=%2.3f Composite=%2.3f SyncWait=%2.3f\n",
		vrSceneBucket, vrPostprocessBucket, vrFinalizeBucket, vrSubmitBucket, vrCompositeBucket, vrSyncWaitBucket);

	str.AppendFormat("BSP = %2.3f, Clip=%2.3f\n"
		"W: Render=%2.3f, Setup=%2.3f\n"
		"F: Render=%2.3f, Setup=%2.3f\n"
		"S: Render=%2.3f, Setup=%2.3f\n"
		"2D: %2.3f Finish3D: %2.3f\n"
		"VR: SceneEyes=%2.3f SceneBuild=%2.3f SceneDraw=%2.3f\n"
		"VR: Subsectors=%2.3f Lines=%2.3f Things=%2.3f Flats=%2.3f\n"
		"VR: SubCull=%2.3f SubVisible=%2.3f\n"
		"VR: LineClip=%2.3f LineDecide=%2.3f\n"
		"VR: PostBSP=%2.3f PlayerSprites=%2.3f\n"
		"VR: EyeComposite=%2.3f FinalizeEye=%2.3f Submit=%2.3f\n"
		"VR: PostScene=%2.3f SceneTransfer=%2.3f FinalPresent=%2.3f SubmitCopy=%2.3f SubmitWait=%2.3f RenderSync=%2.3f\n"
		"VR: FinalPasses=%d MirrorPasses=%d SceneTransfers=%d SubmitLayerBlits=%d\n"
		"Scene worker elapsed=%2.3f, Scene worker wait=%2.3f\n"
		"Wall workers elapsed=%2.3f, Wall workers CPU-sum=%2.3f, Wall setup CPU-sum=%2.3f\n"
		"Wall worker parallelism=%2.2f busy=%2.2f\n"
		"Wall merge=%2.3f, Wall batches=%d, Wall items=%d\n"
		"All=%2.3f, Render=%2.3f, Setup=%2.3f, Portal=%2.3f, Drawcalls=%2.3f, Postprocess=%2.3f, Finish=%2.3f\n",
		bsp, clipwall,
		RenderWall.TimeMS(), setupwall,
		RenderFlat.TimeMS(), SetupFlat.TimeMS(),
		RenderSprite.TimeMS(), SetupSprite.TimeMS(),
		twoD.TimeMS(), Flush3D.TimeMS() - twoD.TimeMS(),
		VRSceneEyes.TimeMS(), VRSceneBuild.TimeMS(), VRSceneDraw.TimeMS(),
		VRSubsectors.TimeMS(), VRLineBuild.TimeMS(), vrThingBuild, vrFlatBuild,
		VRSubsectorCull.TimeMS(), VRSubsectorVisible.TimeMS(),
		VRLineClip.TimeMS(), VRLineDecide.TimeMS(),
		VRScenePostBSP.TimeMS(), VRPlayerSprites.TimeMS(),
		VREyeComposite.TimeMS(), VRFinalizeEye.TimeMS(), VRSubmit.TimeMS(),
		VRPostProcessScene.TimeMS(), VRSceneTransfer.TimeMS(), VRFinalPresent.TimeMS(), VRSubmitCopy.TimeMS(), VRSubmitWait.TimeMS(), VRRenderSyncWait.TimeMS(),
		VRFinalPresentPasses, VRMirrorPreparePasses, VRSceneTransferOps, VRSubmitLayerBlits,
		SceneWorkerElapsed.TimeMS(), MTWait.TimeMS(),
		wallWorkersElapsed, wallWorkersCpuSum, wallWorkersWallCpuSum,
		wallWorkerParallelism, wallWorkerBusyParallelism,
		WallMerge.TimeMS(), WallBatchCount, WallItemsProcessed,
		All.TimeMS() + Finish.TimeMS(), RenderAll.TimeMS(),	ProcessAll.TimeMS(), PortalAll.TimeMS(), drawcalls.TimeMS(), PostProcess.TimeMS(), Finish.TimeMS());
}

static void AppendRenderStats(FString &out)
{
	out.AppendFormat("Walls: %d (%d splits, %d t-splits, %d vertices)\n"
		"Flats: %d (%d primitives, %d vertices)\n"
		"Sprites: %d, Decals=%d, Portals: %d, Command buffers: %d\n",
		rendered_lines, render_vertexsplit, render_texsplit, vertexcount, rendered_flats, flatprimitives, flatvertices, rendered_sprites,rendered_decals, rendered_portals, rendered_commandbuffers );
}

static void AppendLightStats(FString &out)
{
	out.AppendFormat("DLight - Walls: %d processed, %d rendered\n", iter_dlight, draw_dlight);
	out.AppendFormat("DLight - Flats: %d processed, %d rendered\n", iter_dlightf, draw_dlightf);
	out.AppendFormat("DLight - Link: active_updates=%d link_calls=%d relinks=%d unlinks=%d collected_ss=%d linked_sectors=%d linked_sides=%d removed_sector_links=%d removed_side_links=%d\n",
		dynlights_active_updates, dynlights_link_calls, dynlights_relink_calls, dynlights_unlink_calls,
		dynlights_collected_subsectors, dynlights_linked_sectors, dynlights_linked_sides,
		dynlights_removed_sector_links, dynlights_removed_side_links);
	out.AppendFormat("DLight - DistanceCull: walls=%d flats=%d models=%d\n",
		dynlights_distance_culled_walls, dynlights_distance_culled_flats, dynlights_distance_culled_models);
	out.AppendFormat("DLight - ModelGather: subsectors=%d candidates=%d duplicates=%d uploads=%d\n",
		dynlights_model_subsectors, dynlights_model_candidates, dynlights_model_duplicates, dynlights_model_uploads);
}

static void AppendBufferStats(FString &out)
{
	out.AppendFormat("Buffers: vertexbuffer=%d, lightbuffer=%d, bonebuffer=%d", vertexbuffer_curindex, lightbuffer_curindex, bonebuffer_curindex);
}

ADD_STAT(rendertimes)
{
	static FString buff;
	static int64_t lasttime=0;
	int64_t t=I_msTime();
	if (t-lasttime>1000)
	{
		buff.Truncate(0);
		AppendRenderTimes(buff);
		lasttime=t;
	}
	return buff;
}

ADD_STAT(renderstats)
{
	FString out;
	AppendRenderStats(out);
	return out;
}

ADD_STAT(lightstats)
{
	FString out;
	AppendLightStats(out);
	return out;
}

ADD_STAT(bufferstats)
{
	FString out;
	AppendBufferStats(out);
	return out;
}

static int printstats;
static bool switchfps;
static uint64_t waitstart;
static FString benchlabel;
EXTERN_CVAR(Bool, vid_fps)
EXTERN_CVAR(Int, vid_refreshrate)

// RS FORK -- no longer static: r_perflog (hw_perflog.cpp) writes the same
// header at the top of each perflog.txt session.
void AppendBenchmarkHeader(FString& out)
{
	out.AppendFormat("Timestamp: %s", myasctime());
	out.AppendFormat("%s version %s (%s)\n", GAMENAME, GetVersionString(), GetGitHash());
	// GetGitDescription() no longer exists: UZDoom 5.0.0 reworked revision reporting
	// (cmake/UpdateRevision.cmake -> cmake/gitinfo.h.in, src/version.cpp). GIT_DESCRIPTION
	// is now what GetVersionString() returns, and it is already stamped on the line above,
	// so record the closest tag / distance / commit timestamp here instead - same purpose
	// (pinning the exact build a benchmark run came from), no duplicated field.
	out.AppendFormat("Git describe: %s (+%d) @ %s\n", GetGitTag(), GetGitDistance(), GetGitTime());
	if (!benchlabel.IsEmpty())
		out.AppendFormat("Bench label: %s\n", benchlabel.GetChars());

	VRBenchmarkInfo vrinfo = {};
	const auto vrmode = VRMode::GetVRModeCached(true);
	if (vrmode != nullptr)
	{
		vrmode->GetBenchmarkInfo(vrinfo);
	}

	out.AppendFormat("XR: is_vr=%s is_openxr=%s multiview=%s supported=%s active=%s mirror_mode=%d render_scale=%.3f requested_refresh=%d runtime_refresh=%.2f\n",
		vrinfo.IsVR ? "yes" : "no",
		vrinfo.IsOpenXR ? "yes" : "no",
		vrinfo.MultiviewEnabled ? "on" : "off",
		vrinfo.MultiviewSupported ? "yes" : "no",
		vrinfo.MultiviewActive ? "yes" : "no",
		vrinfo.DesktopViewMode,
		vrinfo.RenderScale,
		vrinfo.RequestedRefreshRate > 0 ? vrinfo.RequestedRefreshRate : (int)vid_refreshrate,
		vrinfo.RuntimeRefreshRate);

	out.AppendFormat("XR Targets: recommended=%ux%u present=%ux%u samples=%d view_count=%u view_mask=0x%08x dedicated_mirror=%s\n",
		vrinfo.RecommendedWidth, vrinfo.RecommendedHeight,
		vrinfo.PresentWidth, vrinfo.PresentHeight,
		vrinfo.SceneSamples,
		vrinfo.ViewCount,
		(unsigned int)vrinfo.ViewMask,
		vrinfo.DedicatedMirrorTextures ? "yes" : "no");

	out.AppendFormat("XR Flags: scene_layered=%s postprocess_layered=%s finalize_layered=%s direct_xr_render=%s vr_openxr_sync_mode=%s vr_openxr_foveation=%s\n\n",
		vrinfo.SceneLayered ? "yes" : "no",
		vrinfo.PostprocessLayered ? "yes" : "no",
		vrinfo.FinalizeLayered ? "yes" : "no",
		vrinfo.DirectXrRender ? "yes" : "no",
		GetOpenXrSyncModeName(vrinfo.SyncMode),
		"unsupported");
	out.AppendFormat("DynLight Config: r_dynlights=%s gl_lights=%s sprites=%s particles=%s weapons=%s shadowmap=%s dist_cull_cache=%s model_dedupe_cache=%s spot_cache=%s posrel_cache=%s\n",
		r_dynlights ? "on" : "off",
		gl_lights ? "on" : "off",
		gl_light_sprites ? "on" : "off",
		gl_light_particles ? "on" : "off",
		gl_light_weapons ? "on" : "off",
		gl_light_shadowmap ? "on" : "off",
		gl_light_distance_cull_cache ? "on" : "off",
		gl_light_model_dedupe_cache ? "on" : "off",
		gl_light_spot_cache ? "on" : "off",
		gl_light_pos_relative_cache ? "on" : "off");
	out.AppendFormat("DynLight Limits: distance_cull=%.1f max_intensity=%.1f max_collected_ss=%d flat_max=%d wall_max=%d flat_budget=%d wall_budget=%d range_limit=%d\n\n",
		(double)gl_light_distance_cull,
		(double)gl_light_max_intensity,
		(int)gl_light_max_collected_subsectors,
		(int)gl_light_flat_max_lights,
		(int)gl_light_wall_max_lights,
		(int)gl_light_flat_candidate_budget,
		(int)gl_light_wall_candidate_budget,
		(int)gl_light_range_limit);
	out.AppendFormat("Scene Culling: sprite=%.1f decor_sprite=%.1f line=%.1f\n\n",
		(double)gl_sprite_distance_cull,
		(double)gl_sprite_decor_distance_cull,
		(double)gl_line_distance_cull);
}

void CheckBench()
{
	if (printstats && ConsoleState == c_up)
	{
		// if we started the FPS counter ourselves or ran from the console
		// we need to wait for it to stabilize before using it.
		if (waitstart > 0 && I_msTime() - waitstart < 5000) return;

		FString compose;
		AppendBenchmarkHeader(compose);

		if (sysCallbacks.GetLocationDescription) compose << sysCallbacks.GetLocationDescription();

		AppendRenderStats(compose);
		AppendRenderTimes(compose);
		AppendLightStats(compose);
		compose << "\n\n\n";

		FILE *f = fopen("benchmarks.txt", "at");
		if (f != NULL)
		{
			fputs(compose.GetChars(), f);
			fclose(f);
		}
		Printf("Benchmark info saved\n");
		if (switchfps) vid_fps = false;
		printstats = false;
		benchlabel = "";
	}
}

CCMD(bench)
{
	benchlabel = "";
	for (int i = 1; i < argv.argc(); ++i)
	{
		if (!benchlabel.IsEmpty())
			benchlabel << ' ';
		benchlabel << argv[i];
	}
	printstats = true;
	if (vid_fps == 0)
	{
		vid_fps = 1;
		waitstart = I_msTime();
		switchfps = true;
	}
	else
	{
		if (ConsoleState == c_up) waitstart = I_msTime();
		switchfps = false;
	}
	C_HideConsole ();
}

CCMD(togglefps)
{
	vid_fps = !vid_fps;
	Printf("FPS counter %s\n", vid_fps ? "enabled" : "disabled");
}

bool glcycle_t::active = false;

void  CheckBenchActive()
{
	FStat *stat = FStat::FindStat("rendertimes");
	// RS FORK -- r_perflog samples these CPU timers, and they only tick while active.
	glcycle_t::active = ((stat != NULL && stat->isActive()) || printstats || *r_perflog > 0);
}
