/*
** hw_clock.h
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

#ifndef __GL_CLOCK_H
#define __GL_CLOCK_H

#include "stats.h"
#include "m_fixed.h"

extern glcycle_t RenderWall,SetupWall,ClipWall;
extern glcycle_t RenderFlat,SetupFlat;
extern glcycle_t RenderSprite,SetupSprite;
extern glcycle_t All, Finish, PortalAll, Bsp;
extern glcycle_t ProcessAll, PostProcess;
extern glcycle_t VRSceneEyes, VRSceneBuild, VRSubsectors, VRSubsectorCull, VRSubsectorVisible, VRLineBuild, VRLineClip, VRLineDecide, VRThingBuild, VRFlatBuild, VRScenePostBSP, VRPlayerSprites, VRSceneDraw, VREyeComposite, VRFinalizeEye, VRSubmit;
extern glcycle_t VRPostProcessScene, VRSceneTransfer, VRFinalPresent, VRSubmitCopy, VRSubmitWait, VRRenderSyncWait;
extern glcycle_t RenderAll;
extern glcycle_t Dirty;
extern glcycle_t drawcalls, twoD, Flush3D;
extern glcycle_t MTWait, WTTotal;
extern glcycle_t WTWallJobs, WTFlatJobs, WTThingJobs;
extern glcycle_t WallWorkersElapsed, WallMerge, SceneWorkerElapsed;
extern int64_t WallWorkersCpuSumCycles, WallWorkersWallCpuSumCycles;
extern int64_t WallWorkersElapsedCycles;
extern int WallBatchCount, WallItemsProcessed;
extern int VRFinalPresentPasses, VRMirrorPreparePasses, VRSceneTransferOps, VRSubmitLayerBlits;

extern int iter_dlightf, iter_dlight, draw_dlight, draw_dlightf;
extern int dynlights_active_updates, dynlights_link_calls, dynlights_relink_calls, dynlights_unlink_calls;
// [LIGHTSHADOWS] Every FDynamicLight::LinkLight since start-up: a light's section lists rebuilt (a moving or resizing light's tic
// relink, a new light, a pose-anchored light moving). Never reset. ResetProfilingData clears the per-frame counters above at the
// top of each render -- after the tics that relink -- so the performance log reads this one's growth over a window instead
// (hw_perflog.cpp, load "relinks").
extern uint64_t dynlights_links_total;
extern int dynlights_collected_subsectors, dynlights_linked_sectors, dynlights_linked_sides;
extern int dynlights_removed_sector_links, dynlights_removed_side_links;
extern int dynlights_distance_culled_walls, dynlights_distance_culled_flats, dynlights_distance_culled_models;
extern int dynlights_model_subsectors, dynlights_model_candidates, dynlights_model_duplicates, dynlights_model_uploads;
extern int rendered_lines,rendered_flats,rendered_sprites,rendered_decals,render_vertexsplit,render_texsplit;
extern int rendered_portals;
extern int lightbuffer_curindex, vertexbuffer_curindex, bonebuffer_curindex;

extern int vertexcount, flatvertices, flatprimitives;

void ResetProfilingData();
void CheckBench();
void CheckBenchActive();

// RS FORK -- r_perflog (hw_perflog.cpp) samples the same numbers "stat
// rendertimes" and "bench" show, so it reads them through these rather than a
// second copy of the formulas.
struct RenderTimeSummary
{
	double Scene, Post, Finalize, Submit, Composite, SyncWait;	// the "VR Summary" buckets
	double All, Drawcalls;
};
void GetRenderTimeSummary(RenderTimeSummary& out);
void AppendBenchmarkHeader(FString& out);


#endif
