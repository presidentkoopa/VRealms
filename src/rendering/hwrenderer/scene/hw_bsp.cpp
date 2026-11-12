/*
** hw_bsp.cpp
**
** Main rendering loop / BSP traversal / visibility clipping
**
**---------------------------------------------------------------------------
**
** Copyright 2000-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "p_lnspec.h"
#include "p_local.h"
#include "a_sharedglobal.h"
#include "g_levellocals.h"
#include "p_effect.h"
#include "po_man.h"
#include "m_fixed.h"
#include "ctpl.h"
#include <future>
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_clipper.h"
#include "hwrenderer/data/hw_cvars.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/data/hw_vrmodes.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "hw_vertexbuilder.h"
#include "hw_walldispatcher.h"

#include "p_visualthinker.h"
#include <thread>

#if defined(ARCH_IA32) || defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

CVAR(Bool, gl_multithread, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, gl_bsp_worker_threads, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	if (self < 1) self = 1;
	else if (self > 8) self = 8;

	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}
CVAR(Bool, gl_bsp_worker_sky_mainthread, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, gl_bsp_wall_batch_size, 96, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static inline int GetWallBatchSize()
{
	if (gl_bsp_wall_batch_size < 80) return 64;
	if (gl_bsp_wall_batch_size < 112) return 96;
	return 128;
}

static inline bool IsGameplayRelevantThing(const AActor* thing)
{
	return thing->player != nullptr
		|| !!(thing->flags & (MF_SPECIAL | MF_SHOOTABLE | MF_MISSILE))
		|| !!(thing->flags3 & MF3_ISMONSTER);
}

static inline bool ShouldCullDecorRenderThing(const AActor* thing, double distSq)
{
	if (gl_sprite_decor_distance_cull <= 0.0 || IsGameplayRelevantThing(thing))
	{
		return false;
	}

	const double cullDistSq = gl_sprite_decor_distance_cull * gl_sprite_decor_distance_cull;
	return distSq >= cullDistSq;
}

EXTERN_CVAR(Float, r_actorspriteshadowdist)
EXTERN_CVAR(Bool, r_radarclipper)
EXTERN_CVAR(Bool, r_dithertransparency)

thread_local bool isWorkerThread;
ctpl::thread_pool renderPool(8);
bool inited = false;

const int MAXDITHERACTORS = 20; // Maximum number of enemies that can set dither-transparency flags
AActor* RenderedTargets[MAXDITHERACTORS];
int RTnum;

void ClearDitherTargets()
{
	RTnum = 0; // Number of rendered enemies/targets
	for (int ii = 0; ii < MAXDITHERACTORS; ii++)
	  RenderedTargets[ii] = nullptr;
}

struct RenderJob
{
	enum
	{
		FlatJob,
		WallJob,
		SpriteJob,
		ParticleJob,
		PortalJob,
		TerminateJob	// inserted when all work is done so that the worker can return.
	};

	int type;
	subsector_t *sub;
	seg_t *seg;
	sector_t *frontsector;
	sector_t *backsector;
	bool isculled;
};

struct WallWorkItem
{
	subsector_t *sub;
	seg_t *seg;
	sector_t *frontsector;
	sector_t *backsector;
	bool isculled;
	bool terminate;
};

struct WallBatchJob
{
	int start;
	int count;
	bool terminate;
};

class RenderJobQueue
{
	RenderJob pool[300000];	// Way more than ever needed. The largest ever seen on a single viewpoint is around 40000.
	int readindex = 0;
	std::atomic<int> writeindex{};
public:
	void AddJob(int type, subsector_t *sub, seg_t *seg = nullptr, sector_t *frontsector = nullptr, sector_t *backsector = nullptr, bool isculled = false)
	{
		// This does not check for array overflows. The pool should be large enough that it never hits the limit.

		pool[writeindex] = { type, sub, seg, frontsector, backsector, isculled };
		writeindex++;	// update index only after the value has been written.
	}

	RenderJob *GetJob()
	{
		if (readindex >= writeindex.load())
		{
			return nullptr;
		}

		return &pool[readindex++];
	}

	void ReleaseAll()
	{
		readindex = 0;
		writeindex = 0;
	}
};

class WallWorkQueue
{
	static constexpr int MaxWallItems = 300000;
	static constexpr int MaxWallBatches = 100000;

	WallWorkItem items[MaxWallItems];
	WallBatchJob batches[MaxWallBatches];
	std::atomic<int> readindex{};
	std::atomic<int> writeindex{};
	int itemwrite = 0;
	int pendingstart = 0;
	int pendingcount = 0;
public:
	void AddJob(subsector_t *sub, seg_t *seg, sector_t *frontsector, sector_t *backsector, bool isculled)
	{
		items[itemwrite++] = { sub, seg, frontsector, backsector, isculled, false };
		pendingcount++;
		if (pendingcount >= GetWallBatchSize())
		{
			FlushPending();
		}
	}

	void FlushPending()
	{
		if (pendingcount <= 0)
		{
			pendingstart = itemwrite;
			return;
		}

		batches[writeindex] = { pendingstart, pendingcount, false };
		writeindex++;
		pendingstart = itemwrite;
		pendingcount = 0;
	}

	void AddTerminate()
	{
		FlushPending();
		batches[writeindex] = { 0, 0, true };
		writeindex++;
	}

	WallBatchJob *GetJob()
	{
		while (true)
		{
			int current = readindex.load();
			const int written = writeindex.load();
			if (current >= written)
			{
				return nullptr;
			}

			if (readindex.compare_exchange_weak(current, current + 1))
			{
				return &batches[current];
			}
		}
	}

	WallWorkItem *GetItems(int start)
	{
		return &items[start];
	}

	void ReleaseAll()
	{
		readindex = 0;
		writeindex = 0;
		itemwrite = 0;
		pendingstart = 0;
		pendingcount = 0;
	}
};

static RenderJobQueue sceneJobQueue;
static WallWorkQueue wallJobQueue;
static std::vector<HWMeshHelper> wallWorkerMeshes;
static std::vector<std::future<void>> wallWorkerFutures;
static bool wallWorkersStarted = false;

static inline void RelaxWorkerSpin(unsigned int& idleSpins)
{
#if defined(ARCH_IA32) || defined(_M_X64) || defined(__x86_64__)
	_mm_pause();
	_mm_pause();
	_mm_pause();
	_mm_pause();
	_mm_pause();
	_mm_pause();
	_mm_pause();
	_mm_pause();
#endif

	idleSpins++;
}

void HWDrawInfo::WorkerThread(HWMeshHelper* helper)
{
	sector_t *front, *back;
	HWWallDispatcher disp = helper != nullptr
		? HWWallDispatcher(Level, helper, lightmode)
		: HWWallDispatcher(this);

	if (helper == nullptr)
	{
		WTTotal.Clock();
		SceneWorkerElapsed.Clock();
	}
	unsigned int idleSpins = 0;
	isWorkerThread = true;	// for adding asserts in GL API code. The worker thread may never call any GL API.
	while (true)
	{
		if (helper != nullptr)
		{
			auto batchJob = wallJobQueue.GetJob();
			if (batchJob == nullptr)
			{
				RelaxWorkerSpin(idleSpins);
				continue;
			}
			idleSpins = 0;

			if (batchJob->terminate)
			{
				return;
			}

			auto batchItems = wallJobQueue.GetItems(batchJob->start);
			helper->batchCount++;
			for (int i = 0; i < batchJob->count; i++)
			{
				auto &wallJob = batchItems[i];
				HWWall wall;
				const int64_t wallCycleStart = rdtsc();
				wall.sub = wallJob.sub;
				front = wallJob.frontsector;
				back = wallJob.backsector;
				wall.Process(&disp, wallJob.seg, front, back, wallJob.isculled);
				helper->wallCount++;
				helper->wallCycles += rdtsc() - wallCycleStart;
			}
			continue;
		}

		auto job = sceneJobQueue.GetJob();
		if (job == nullptr)
		{
			// The queue is empty. Relax the CPU briefly before retrying so extra BSP workers
			// do not just busy-spin on 64-bit builds while the main thread is still producing work.
			RelaxWorkerSpin(idleSpins);
		}
		// Note that the main thread MUST have prepared the fake sectors that get used below!
		// This worker thread cannot prepare them itself without costly synchronization.
		else switch (job->type)
		{
		case RenderJob::TerminateJob:
			WTTotal.Unclock();
			SceneWorkerElapsed.Unclock();
			return;

		case RenderJob::WallJob:
		{
			idleSpins = 0;
			HWWall wall;
			WTWallJobs.Clock();
			SetupWall.Clock();
			wall.sub = job->sub;
			front = job->frontsector;
			back = job->backsector;
			wall.Process(&disp, job->seg, front, back, job->isculled);
			rendered_lines++;
			SetupWall.Unclock();
			WTWallJobs.Unclock();
			break;
		}

		case RenderJob::FlatJob:
		{
			idleSpins = 0;
			Clocker flatJobTimer(WTFlatJobs);
			HWFlat flat;
			SetupFlat.Clock();
			flat.section = job->sub->section;
			front = hw_FakeFlat(job->sub->render_sector, in_area, false);
			flat.ProcessSector(this, front);
			SetupFlat.Unclock();
			break;
		}

		case RenderJob::SpriteJob:
			idleSpins = 0;
			WTThingJobs.Clock();
			SetupSprite.Clock();
			front = hw_FakeFlat(job->sub->sector, in_area, false);
			RenderThings(job->sub, front);
			SetupSprite.Unclock();
			WTThingJobs.Unclock();
			break;

		case RenderJob::ParticleJob:
			idleSpins = 0;
			WTThingJobs.Clock();
			SetupSprite.Clock();
			front = hw_FakeFlat(job->sub->sector, in_area, false);
			RenderParticles(job->sub, front);
			SetupSprite.Unclock();
			WTThingJobs.Unclock();
			break;

		case RenderJob::PortalJob:
			idleSpins = 0;
			AddSubsectorToPortal((FSectorPortalGroup *)job->seg, job->sub);
			break;
		}

	}
}

void HWDrawInfo::StartWallWorkersIfNeeded()
{
	if (!experimentalMultiWallWorkers || wallWorkersStarted)
		return;

	const int workerCount = gl_bsp_worker_threads;
	wallWorkersStarted = true;
	wallWorkerFutures.clear();
	wallWorkerFutures.reserve(workerCount);
	for (int i = 0; i < workerCount; i++)
	{
		wallWorkerFutures.push_back(renderPool.push([&, i](int id) {
			const int64_t workerStart = rdtsc();
			WorkerThread(&wallWorkerMeshes[i]);
			wallWorkerMeshes[i].totalCycles = rdtsc() - workerStart;
		}));
	}
}





EXTERN_CVAR(Bool, gl_render_segs)
EXTERN_CVAR(Bool, gl_seamless)

CVAR(Bool, gl_render_things, true, 0)
CVAR(Bool, gl_render_walls, true, 0)
CVAR(Bool, gl_render_flats, true, 0)

void HWDrawInfo::UnclipSubsector(subsector_t *sub)
{
	int count = sub->numlines;
	seg_t * seg = sub->firstline;
	auto &clipper = *mClipper;

	while (count--)
	{
		angle_t startAngle = clipper.GetClipAngle(seg->v2);
		angle_t endAngle = clipper.GetClipAngle(seg->v1);

		// Back side, i.e. backface culling	- read: endAngle >= startAngle!
		if (startAngle-endAngle >= ANGLE_180)
		{
			clipper.SafeRemoveClipRange(startAngle, endAngle);
			clipper.SetBlocked(false);
		}
		seg++;
	}
}

//==========================================================================
//
// R_AddLine
// Clips the given segment
// and adds any visible pieces to the line list.
//
//==========================================================================

CVAR(Float, gl_line_distance_cull, 4000.0, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

// [RS] The distance is measured from the viewpoint THE SCENE IS BEING DRAWN FROM, which the caller
// passes in (HWDrawInfo::Viewpoint). It used to be r_viewpoint, the camera's own position -- and inside a
// line portal those are different places: HWLineToLinePortal::Setup moves the scene's viewpoint by the
// portal's displacement and leaves r_viewpoint alone. A room reached through a linked portal whose two
// ends are further apart on the map than gl_line_distance_cull was therefore culled whole, however near
// the eye it was: every wall of it replaced by the cull stand-in (the front sector's ceiling texture from
// floor to ceiling, HWWall::Process) and given a solid clip range. That is the picture of Realms STUDY1 at
// (0,345) looking north with roth_portals 1: the corridor 39 units away, set aside 5504 units east.
bool IsDistanceCulled(seg_t *line, const DVector3 &viewpos)
{
	double dist3 = gl_line_distance_cull * gl_line_distance_cull;
	if (dist3 <= 0.0)
		return false;

	double dist1 = (line->v1->fPos() - viewpos.XY()).LengthSquared();
	double dist2 = (line->v2->fPos() - viewpos.XY()).LengthSquared();
	if ((dist1 > dist3) && (dist2 > dist3))
		return true;
	return false;
}

static bool ComputeSectorMainThreadWallPath(sector_t* sector)
{
	if (sector == nullptr)
		return false;

	if (gl_bsp_worker_sky_mainthread &&
		(sector->GetTexture(sector_t::ceiling) == skyflatnum || sector->GetTexture(sector_t::floor) == skyflatnum))
		return true;

	if (sector->ValidatePortal(sector_t::ceiling) != nullptr || sector->ValidatePortal(sector_t::floor) != nullptr)
		return true;

	return false;
}

static bool SectorNeedsMainThreadWallPath(HWDrawInfo* di, sector_t* sector)
{
	return ComputeSectorMainThreadWallPath(sector);
}

static bool ComputeLineMainThreadWallPath(line_t* line)
{
	if (line == nullptr)
		return false;

	if (line->special == Line_Horizon)
		return true;

	if (line->isVisualPortal() || line->GetTransferredPortal() != nullptr)
		return true;

	return false;
}

static bool LineNeedsMainThreadWallPath(HWDrawInfo* di, line_t* line)
{
	return ComputeLineMainThreadWallPath(line);
}

static bool NeedsMainThreadWallPath(HWDrawInfo* di, seg_t* seg, sector_t* frontsector, sector_t* backsector)
{
	if (seg == nullptr || seg->linedef == nullptr)
		return false;

	if (LineNeedsMainThreadWallPath(di, seg->linedef))
		return true;

	if (SectorNeedsMainThreadWallPath(di, frontsector) || SectorNeedsMainThreadWallPath(di, backsector))
		return true;

	return false;
}

void HWDrawInfo::AddLine (seg_t *seg, bool portalclip)
{
#ifdef _DEBUG
	if (seg->linedef && seg->linedef->Index() == 38)
	{
		int a = 0;
	}
#endif

	struct ScopedVRLineTimer
	{
		glcycle_t* Timer = nullptr;

		~ScopedVRLineTimer()
		{
			Stop();
		}

		void Start(glcycle_t& timer)
		{
			Stop();
			Timer = &timer;
			Timer->Clock();
		}

		void Stop()
		{
			if (Timer != nullptr)
			{
				Timer->Unclock();
				Timer = nullptr;
			}
		}
	};

	ScopedVRLineTimer vrLineTimer;
	sector_t * backsector = nullptr;
	bool cachedFakeBacksector = false;
	bool cachedBacksectorClip = false;
	bool hasCachedBacksectorClip = false;
	const bool isVisualPortalLine = seg->linedef != nullptr && seg->linedef->isVisualPortal();
	const bool allowOoB = Viewpoint.bDoOob;
	const bool useRadarClip = allowOoB && r_radarclipper && !(Level->flags3 & LEVEL3_NOFOGOFWAR);

	if (portalclip)
	{
		int clipres = mClipPortal->ClipSeg(seg, Viewpoint.Pos);
		if (clipres == PClip_InFront) return;
	}

	if (IsVRScene) vrLineTimer.Start(VRLineClip);
	auto &clipper = *mClipper;
	angle_t startAngle = clipper.GetClipAngle(seg->v2);
	angle_t endAngle = clipper.GetClipAngle(seg->v1);
	angle_t startAngleR = 0;
	angle_t endAngleR = 0;
	angle_t paddingR = 0x00200000; // Make radar clipping more aggressive (reveal less)

	if (useRadarClip)
	{
		auto &clipperr = *rClipper;
		startAngleR = clipperr.GetRadarClipAngle(seg->v2);
		endAngleR = clipperr.GetRadarClipAngle(seg->v1);

		if (startAngleR - endAngleR >= ANGLE_180)
		{
			// [RS] not for a wall with a height of its own (WALLF2_OWNHEIGHT, below): it hides nothing above itself.
			if (!seg->backsector && !(seg->sidedef != nullptr && (seg->sidedef->Flags2 & WALLF2_OWNHEIGHT)))
				clipperr.SafeAddClipRange(startAngleR - paddingR, endAngleR + paddingR);
			else if((seg->sidedef != nullptr) && !uint8_t(seg->sidedef->Flags & WALLF_POLYOBJ) && (currentsector->sectornum != seg->backsector->sectornum))
			{
				if (in_area == area_default) in_area = hw_CheckViewArea(seg->v1, seg->v2, seg->frontsector, seg->backsector);
				backsector = hw_FakeFlat(seg->backsector, in_area, true);
				cachedFakeBacksector = true;
				cachedBacksectorClip = hw_CheckClip(seg->sidedef, currentsector, backsector);
				hasCachedBacksectorClip = true;
				if (cachedBacksectorClip) clipperr.SafeAddClipRange(startAngleR - paddingR, endAngleR + paddingR);
			}
		}
	}

	// Back side, i.e. backface culling	- read: endAngle >= startAngle!
	if (startAngle-endAngle<ANGLE_180)
	{
		return;
	}

	if (seg->sidedef == nullptr)
	{
		if (!(currentsubsector->flags & SSECMF_DRAWN))
		{
			if (clipper.SafeCheckRange(startAngle, endAngle) && !allowOoB)
			{
			  currentsubsector->flags |= SSECMF_DRAWN;
			}
			if (useRadarClip && rClipper->SafeCheckRange(startAngleR, endAngleR))
			{
			  currentsubsector->flags |= SSECMF_DRAWN;
			}
		}
		return;
	}

	if (!clipper.SafeCheckRange(startAngle, endAngle))
	{
		return;
	}

	if (!allowOoB || !useRadarClip || rClipper->SafeCheckRange(startAngleR, endAngleR))
		currentsubsector->flags |= SSECMF_DRAWN;
	if (IsVRScene)
	{
		vrLineTimer.Start(VRLineDecide);
	}

	uint8_t ispoly = uint8_t(seg->sidedef->Flags & WALLF_POLYOBJ);

	if (IsDistanceCulled(seg, Viewpoint.Pos))
	{
		if (multithread)
		{
			if (experimentalMultiWallWorkers && !NeedsMainThreadWallPath(this, seg, seg->frontsector, seg->backsector))
			{
				wallJobQueue.AddJob(currentsubsector, seg, seg->frontsector, seg->backsector, true);
				StartWallWorkersIfNeeded();
			}
			else
			{
				sceneJobQueue.AddJob(RenderJob::WallJob, currentsubsector, seg, seg->frontsector, seg->backsector, true);
			}
		}
		else
		{
			HWWall wall;
			HWWallDispatcher disp(this);
			wall.sub = currentsubsector;
			wall.Process(&disp, seg, seg->frontsector, seg->backsector, true);
		}
		clipper.SafeAddClipRange(startAngle, endAngle);
		return;
	}

	// [XA] NOTE: ideally it'd be nice to collapse these checks into one,
	// but it's possible to add & remove WALLF_BLOCKRENDERING via zscript
	// so auto-setting it on 1s lines may result in a crash if someone
	// later tries to remove the flag from a line.
	if (!seg->backsector || (seg->sidedef->Flags & WALLF_BLOCKRENDERING))
	{
		// This is rendered as a one-sided wall, so drop any backsector the radar
		// clipper above may have cached into 'backsector' via hw_FakeFlat.
		backsector = nullptr;
		// [RS] WALLF2_OWNHEIGHT: a polyobject wall shorter than the sector it stands in
		// (a Realms door leaf) hides nothing above itself, so it must not close the
		// clipper: the lintel and the room beyond are still in view over it.
		if(!allowOoB)
			if (!(seg->sidedef->Flags & WALLF_DITHERTRANS_MID) && !(ispoly && (seg->sidedef->Flags2 & WALLF2_OWNHEIGHT)))
				clipper.SafeAddClipRange(startAngle, endAngle);

		// [RS] A LINKED PORTAL ON A ONE-SIDED LINE. The clip range above stays SOLID, on purpose: nothing
		// that lies behind this line in this part of the map may be drawn (it is the void, or a room that
		// belongs to another storey). What is seen in the opening is drawn by the line portal itself, which
		// has a clipper of its own -- exactly as for a mirror or a visual portal on a one-sided wall.
		// The wall, however, is built like a two-sided one: its upper and lower pieces are measured against
		// the sector beyond the portal, which is handed to HWWall::Process as the back sector.
		// Adapted from the Eternity Engine, source/r_bsp.cpp (R_AddLine: `beyond`, passed to R_1SidedLine);
		// Copyright (C) the Eternity Engine team (Stephen McGranahan et al.); GPL-3.0-or-later.
		if (isVisualPortalLine && seg->backsector == nullptr && seg->sidedef == seg->linedef->sidedef[0])
		{
			backsector = seg->linedef->getPortalBeyondSector();	// null for every other kind of line
		}
	}
	else if (!ispoly)	// Two-sided polyobjects never obstruct the view
	{
		if (currentsector->sectornum == seg->backsector->sectornum)
		{
			if (!isVisualPortalLine)
			{
				const bool hasValidTexture = seg->sidedef != nullptr && seg->sidedef->GetTexture(side_t::mid).isValid();
				if (!hasValidTexture)
				{
					// nothing to do here!
					if (seg->linedef != nullptr)
					{
						seg->linedef->validcount = validcount;
					}
					return;
				}
			}
			backsector=currentsector;
		}
 		else
		{
			// clipping checks are only needed when the backsector is not the same as the front sector
			if (in_area == area_default) in_area = hw_CheckViewArea(seg->v1, seg->v2, seg->frontsector, seg->backsector);

			if (!cachedFakeBacksector)
			{
				backsector = hw_FakeFlat(seg->backsector, in_area, true);
			}
			const bool clipBlocked = hasCachedBacksectorClip ? cachedBacksectorClip : hw_CheckClip(seg->sidedef, currentsector, backsector);
			if (clipBlocked)
			{
				if(!allowOoB && !(seg->sidedef->Flags & WALLF_DITHERTRANS_MID))
					clipper.SafeAddClipRange(startAngle, endAngle);
			}
		}
	}
	else
	{
		// Backsector for polyobj segs is always the containing sector itself
		backsector = currentsector;
	}

	if (seg->linedef != nullptr)
	{
		seg->linedef->flags |= ML_MAPPED;
	}

	if (ispoly || seg->linedef == nullptr || seg->linedef->validcount != validcount) 
	{
		if (!ispoly && seg->linedef != nullptr)
		{
			seg->linedef->validcount = validcount;
		}

		if (gl_render_walls)
		{
			if (multithread)
			{
				if (experimentalMultiWallWorkers && !NeedsMainThreadWallPath(this, seg, currentsector, backsector))
				{
					wallJobQueue.AddJob(seg->Subsector, seg, currentsector, backsector, false);
					StartWallWorkersIfNeeded();
				}
				else
				{
					sceneJobQueue.AddJob(RenderJob::WallJob, seg->Subsector, seg, currentsector, backsector, false);
				}
			}
			else
			{
				HWWall wall;
				HWWallDispatcher disp(this);
				SetupWall.Clock();
				wall.sub = seg->Subsector;
				wall.Process(&disp, seg, currentsector, backsector);
				rendered_lines++;
				SetupWall.Unclock();
			}
		}
	}
}

//==========================================================================
//
// R_Subsector
// Determine floor/ceiling planes.
// Add sprites of things in sector.
// Draw one or more line segments.
//
//==========================================================================

void HWDrawInfo::PolySubsector(subsector_t * sub)
{
	int count = sub->numlines;
	seg_t * line = sub->firstline;

	while (count--)
	{
		if (line->linedef)
		{
			AddLine (line, mClipPortal != nullptr);
		}
		line++;
	}
}

//==========================================================================
//
// RenderBSPNode
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.
//
//==========================================================================

void HWDrawInfo::RenderPolyBSPNode (void *node)
{
	while (!((size_t)node & 1))  // Keep going until found a subsector
	{
		node_t *bsp = (node_t *)node;

		// Decide which side the view point is on.
		int side = R_PointOnSide(viewx, viewy, bsp);

		// Recursively divide front space (toward the viewer).
		RenderPolyBSPNode (bsp->children[side]);

		// Possibly divide back space (away from the viewer).
		side ^= 1;

		// It is not necessary to use the slower precise version here
		if (!mClipper->CheckBox(bsp->bbox[side]))
		{
			return;
		}

		node = bsp->children[side];
	}
	PolySubsector ((subsector_t *)((uint8_t *)node - 1));
}

//==========================================================================
//
// Unlilke the software renderer this function will only draw the walls,
// not the flats. Those are handled as a whole by the parent subsector.
//
//==========================================================================

void HWDrawInfo::AddPolyobjs(subsector_t *sub)
{
	if (sub->BSP == nullptr || sub->BSP->bDirty)
	{
		sub->BuildPolyBSP();
	}
	if (sub->BSP->Nodes.Size() == 0)
	{
		PolySubsector(&sub->BSP->Subsectors[0]);
	}
	else
	{
		RenderPolyBSPNode(&sub->BSP->Nodes.Last());
	}
}


//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::AddLines(subsector_t * sub, sector_t * sector)
{
	currentsector = sector;
	currentsubsector = sub;

	if (IsVRScene) VRLineBuild.Clock();
	ClipWall.Clock();
	if (sub->polys != nullptr)
	{
		AddPolyobjs(sub);
	}
	else
	{
		int count = sub->numlines;
		seg_t * seg = sub->firstline;

		while (count--)
		{
			if (seg->linedef == nullptr)
			{
				if (!(sub->flags & SSECMF_DRAWN)) AddLine (seg, mClipPortal != nullptr);
			}
			else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
			{
				AddLine (seg, mClipPortal != nullptr);
			}
			seg++;
		}
	}
	ClipWall.Unclock();
	if (IsVRScene) VRLineBuild.Unclock();
}

//==========================================================================
//
// Adds lines that lie directly on the portal boundary.
// Only two-sided lines will be handled here, and no polyobjects
//
//==========================================================================

inline bool PointOnLine(const DVector2 &pos, const linebase_t *line)
{
	double v = (pos.Y - line->v1->fY()) * line->Delta().X + (line->v1->fX() - pos.X) * line->Delta().Y;
	return fabs(v) <= EQUAL_EPSILON;
}

void HWDrawInfo::AddSpecialPortalLines(subsector_t * sub, sector_t * sector, linebase_t *line)
{
	currentsector = sector;
	currentsubsector = sub;

	ClipWall.Clock();
	int count = sub->numlines;
	seg_t * seg = sub->firstline;

	while (count--)
	{
		if (seg->linedef != nullptr && seg->PartnerSeg != nullptr)
		{
			if (PointOnLine(seg->v1->fPos(), line) && PointOnLine(seg->v2->fPos(), line))
				AddLine(seg, false);
		}
		seg++;
	}
	ClipWall.Unclock();
}


//==========================================================================
//
// R_RenderThings
//
//==========================================================================

void HWDrawInfo::RenderThings(subsector_t * sub, sector_t * sector)
{
	if (IsVRScene) VRThingBuild.Clock();
	sector_t * sec=sub->sector;
	// Handle all things in sector.
	const auto &vp = Viewpoint;
	for (auto p = sec->touching_renderthings; p != nullptr; p = p->m_snext)
	{
		auto thing = p->m_thing;
		if (thing->validcount == validcount) continue;
		thing->validcount = validcount;
		double distSq = -1.0;

		if(Viewpoint.bDoOob && thing->Sector->isSecret() && thing->Sector->wasSecret() && !r_radarclipper) continue; // This covers things that are touching non-secret sectors
		FIntCVar *cvar = thing->GetInfo()->distancecheck;
		if (cvar != nullptr && *cvar >= 0)
		{
			distSq = (thing->Pos() - vp.Pos).LengthSquared();
			double check = (double)**cvar;
			if (distSq >= check * check)
			{
				continue;
			}
		}
		if (distSq < 0.0)
		{
			distSq = (thing->Pos() - vp.Pos).LengthSquared();
		}
		if (ShouldCullDecorRenderThing(thing, distSq))
		{
			continue;
		}
		// If this thing is in a map section that's not in view it can't possibly be visible
		if (CurrentMapSections[thing->subsector->mapsection])
		{
			HWSprite sprite;

			// [Nash] draw sprite shadow
			if (R_ShouldDrawSpriteShadow(thing))
			{
				double check = r_actorspriteshadowdist;
				if (distSq <= check * check)
				{
					sprite.Process(this, thing, sector, in_area, false, true);
				}
			}

			sprite.Process(this, thing, sector, in_area, false);
		}
	}

	for (msecnode_t *node = sec->sectorportal_thinglist; node; node = node->m_snext)
	{
		AActor *thing = node->m_thing;
		double distSq = -1.0;
		FIntCVar *cvar = thing->GetInfo()->distancecheck;
		if (cvar != nullptr && *cvar >= 0)
		{
			distSq = (thing->Pos() - vp.Pos).LengthSquared();
			double check = (double)**cvar;
			if (distSq >= check * check)
			{
				continue;
			}
		}
		if (distSq < 0.0)
		{
			distSq = (thing->Pos() - vp.Pos).LengthSquared();
		}
		if (ShouldCullDecorRenderThing(thing, distSq))
		{
			continue;
		}

		HWSprite sprite;

		// [Nash] draw sprite shadow
		if (R_ShouldDrawSpriteShadow(thing))
		{
			double check = r_actorspriteshadowdist;
			if (distSq <= check * check)
			{
				sprite.Process(this, thing, sector, in_area, true, true);
			}
		}

		sprite.Process(this, thing, sector, in_area, true);
	}
	if (IsVRScene) VRThingBuild.Unclock();
}

void HWDrawInfo::RenderParticles(subsector_t *sub, sector_t *front)
{
	if (IsVRScene) VRThingBuild.Clock();
	SetupSprite.Clock();
	for (uint32_t i = 0; i < sub->sprites.Size(); i++)
	{
		DVisualThinker *sp = sub->sprites[i];
		if (!sp || sp->ObjectFlags & OF_EuthanizeMe)
			continue;
		if (mClipPortal)
		{
			int clipres = mClipPortal->ClipPoint(sp->PT.Pos.XY());
			if (clipres == PClip_InFront) continue;
		}

		HWSprite sprite;
		sprite.ProcessParticle(this, &sp->PT, front, sp);
	}
	for (int i = Level->ParticlesInSubsec[sub->Index()]; i != NO_PARTICLE; i = Level->Particles[i].snext)
	{
		if (mClipPortal)
		{
			int clipres = mClipPortal->ClipPoint(Level->Particles[i].Pos.XY());
			if (clipres == PClip_InFront) continue;
		}

		HWSprite sprite;
		sprite.ProcessParticle(this, &Level->Particles[i], front, nullptr);
	}
	SetupSprite.Unclock();
	if (IsVRScene) VRThingBuild.Unclock();
}

void HWDrawInfo::ProcessVisibleSubsector(subsector_t* sub, sector_t* sector, sector_t* fakesector)
{
	if (sector->validcount != validcount)
	{
		CheckUpdate(screen->mVertexData, sector);
	}

	// [RH] Add particles
	if (gl_render_things && (sub->sprites.Size() > 0 || Level->ParticlesInSubsec[sub->Index()] != NO_PARTICLE))
	{
		if (multithread)
		{
			sceneJobQueue.AddJob(RenderJob::ParticleJob, sub, nullptr);
		}
		else
		{
			SetupSprite.Clock();
			RenderParticles(sub, fakesector);
			SetupSprite.Unclock();
		}
	}

	AddLines(sub, fakesector);

	// BSP is traversed by subsector.
	// A sector might have been split into several
	//	subsectors during BSP building.
	// Thus we check whether it was already added.
	if (sector->validcount != validcount)
	{
		// Well, now it will be done.
		sector->validcount = validcount;
		sector->MoreFlags |= SECMF_DRAWN;

		if (gl_render_things && (sector->touching_renderthings || sector->sectorportal_thinglist))
		{
			if (multithread)
			{
				sceneJobQueue.AddJob(RenderJob::SpriteJob, sub, nullptr);
			}
			else
			{
				SetupSprite.Clock();
				RenderThings(sub, fakesector);
				SetupSprite.Unclock();
			}
		}
		if (r_dithertransparency && Viewpoint.bDoOob && (RTnum < MAXDITHERACTORS) && mCurrentPortal == nullptr)
		{
			// [DVR] Not parallelizable due to variables RTnum and RenderedTargets[]
			for (auto p = sector->touching_renderthings; p != nullptr; p = p->m_snext)
			{
				auto thing = p->m_thing;
				if (thing->validcount == validcount) continue; // Don't double count
				if (((thing->flags3 & MF3_ISMONSTER) && !(thing->flags & MF_CORPSE)) || (thing->flags & MF_MISSILE))
				{
					if (RTnum < MAXDITHERACTORS) RenderedTargets[RTnum++] = thing;
					else break;
				}
			}
		}
	}

	if (gl_render_flats)
	{
		// Subsectors with only 2 lines cannot have any area
		if (sub->numlines > 2 || (sub->hacked & 1))
		{
			// Exclude the case when it tries to render a sector with a heightsec
			// but undetermined heightsec state. This can only happen if the
			// subsector is obstructed but not excluded due to a large bounding box.
			// Due to the way a BSP works such a subsector can never be visible
			if (!sector->GetHeightSec() || in_area != area_default)
			{
				if (sector != sub->render_sector)
				{
					sector = sub->render_sector;
					// the planes of this subsector are faked to belong to another sector
					// This means we need the heightsec parts and light info of the render sector, not the actual one.
					fakesector = hw_FakeFlat(sector, in_area, false);
				}

				uint8_t& srf = section_renderflags[Level->sections.SectionIndex(sub->section)];
				if (!(srf & SSRF_PROCESSED))
				{
					srf |= SSRF_PROCESSED;

					if (multithread)
					{
						sceneJobQueue.AddJob(RenderJob::FlatJob, sub);
					}
					else
					{
						HWFlat flat;
						flat.section = sub->section;
						if (IsVRScene) VRFlatBuild.Clock();
						SetupFlat.Clock();
						flat.ProcessSector(this, fakesector);
						SetupFlat.Unclock();
						if (IsVRScene) VRFlatBuild.Unclock();
					}
				}
				// mark subsector as processed - but mark for rendering only if it has an actual area.
				ss_renderflags[sub->Index()] =
					(sub->numlines > 2) ? SSRF_PROCESSED | SSRF_RENDERALL : SSRF_PROCESSED;
				if (sub->hacked & 1) AddHackedSubsector(sub);

				// This is for portal coverage.
				FSectorPortalGroup* portal;

				// AddSubsectorToPortal cannot be called here when using multithreaded processing,
				// because the wall processing code in the worker can also modify the portal state.
				// To avoid costly synchronization for every access to the portal list,
				// the call to AddSubsectorToPortal will be deferred to the worker.
				// (GetPortalGruop only accesses static sector data so this check can be done here, restricting the new job to the minimum possible extent.)
				portal = fakesector->GetPortalGroup(sector_t::ceiling);
				if (portal != nullptr)
				{
					if (multithread)
					{
						sceneJobQueue.AddJob(RenderJob::PortalJob, sub, (seg_t*)portal);
					}
					else
					{
						AddSubsectorToPortal(portal, sub);
					}
				}

				portal = fakesector->GetPortalGroup(sector_t::floor);
				if (portal != nullptr)
				{
					if (multithread)
					{
						sceneJobQueue.AddJob(RenderJob::PortalJob, sub, (seg_t*)portal);
					}
					else
					{
						AddSubsectorToPortal(portal, sub);
					}
				}
			}
		}
	}
}


//==========================================================================
//
// R_Subsector
// Determine floor/ceiling planes.
// Add sprites of things in sector.
// Draw one or more line segments.
//
//==========================================================================

void HWDrawInfo::DoSubsector(subsector_t * sub)
{
	if (IsVRScene) VRSubsectors.Clock();
	if (IsVRScene) VRSubsectorCull.Clock();
	sector_t * sector;
	sector_t * fakesector;
	
#ifdef _DEBUG
	if (sub->sector->sectornum==931)
	{
		int a = 0;
	}
#endif

	sector=sub->sector;
	if (!sector)
	{
		if (IsVRScene) VRSubsectorCull.Unclock();
		if (IsVRScene) VRSubsectors.Unclock();
		return;
	}

	// If the mapsections differ this subsector can't possibly be visible from the current view point
	if (!CurrentMapSections[sub->mapsection])
	{
		if (IsVRScene) VRSubsectorCull.Unclock();
		if (IsVRScene) VRSubsectors.Unclock();
		return;
	}
	if (sub->flags & SSECF_POLYORG)
	{
		if (IsVRScene) VRSubsectorCull.Unclock();
		if (IsVRScene) VRSubsectors.Unclock();
		return;
	}	// never render polyobject origin subsectors because their vertices no longer are where one may expect.

	if (ss_renderflags[sub->Index()] & SSRF_SEEN)
	{
		// This means that we have reached a subsector in a portal that has been marked 'seen'
		// from the other side of the portal. This means we must clear the clipper for the
		// range this subsector spans before going on.
		UnclipSubsector(sub);
	}
	if (mClipper->IsBlocked())
	{
		if (IsVRScene) VRSubsectorCull.Unclock();
		if (IsVRScene) VRSubsectors.Unclock();
		return;
	}	// if we are inside a stacked sector portal which hasn't unclipped anything yet.

	const bool allowOoB = Viewpoint.bDoOob;
	const bool useRadarClip = allowOoB && r_radarclipper && !(Level->flags3 & LEVEL3_NOFOGOFWAR);

	if(allowOoB && sector->isSecret() && sector->wasSecret() && !r_radarclipper)
	{
		if (IsVRScene) VRSubsectorCull.Unclock();
		if (IsVRScene) VRSubsectors.Unclock();
		return;
	}

	// cull everything if subsector outside all relevant clippers
	if (allowOoB && (sub->polys == nullptr))
	{
		auto &clipper = *mClipper;
		auto &clipperv = *vClipper;
		int count = sub->numlines;
		seg_t * seg = sub->firstline;
		bool anglevisible = false;
		bool pitchvisible = !allowOoB; // No vertical clipping if viewpoint is not allowed out of bounds
		bool radarvisible = !useRadarClip || ((sub->flags & SSECMF_DRAWN) && !deathmatch);
		const int reflect = mCurrentPortal ? mCurrentPortal->GetMirrorSide() : 0; // -1 = ceiling, 1 = floor, 0 = none
		const double planez = (reflect < 0 ? sector->ceilingplane.ZatPoint(Viewpoint.Pos) : sector->floorplane.ZatPoint(Viewpoint.Pos));
		angle_t pitchtemp;
		angle_t pitchmin = ANGLE_90;
		angle_t pitchmax = 0;

		while (count--)
		{
			if((seg->v1 != nullptr) && (seg->v2 != nullptr))
			{
				if (!anglevisible)
				{
					angle_t startAngle = clipper.GetClipAngle(seg->v2);
					angle_t endAngle = clipper.GetClipAngle(seg->v1);
					if (startAngle-endAngle >= ANGLE_180) anglevisible |= clipper.SafeCheckRange(startAngle, endAngle);
				}
				if (!radarvisible)
				{
					auto &clipperr = *rClipper;
					angle_t startAngleR = clipperr.GetRadarClipAngle(seg->v2);
					angle_t endAngleR = clipperr.GetRadarClipAngle(seg->v1);
					if (startAngleR-endAngleR >= ANGLE_180) radarvisible |= clipperr.SafeCheckRange(startAngleR, endAngleR);
				}

				if (!pitchvisible)
				{
					pitchmin = clipperv.PointToPseudoPitch(seg->v1->fX(), seg->v1->fY(),
														   (reflect != 0) ?
														   2 * planez - sector->floorplane.ZatPoint(seg->v1) :
														   sector->floorplane.ZatPoint(seg->v1));
					pitchmax = clipperv.PointToPseudoPitch(seg->v1->fX(), seg->v1->fY(),
														   (reflect != 0) ?
														   2 * planez - sector->ceilingplane.ZatPoint(seg->v1) :
														   sector->ceilingplane.ZatPoint(seg->v1));
					pitchvisible |= clipperv.SafeCheckRange(pitchmin, pitchmax);
				}
				if (pitchvisible && anglevisible && radarvisible) break;
				if (!pitchvisible)
				{
					pitchtemp = clipperv.PointToPseudoPitch(seg->v2->fX(), seg->v2->fY(),
															(reflect != 0) ?
															2 * planez - sector->floorplane.ZatPoint(seg->v2) :
															sector->floorplane.ZatPoint(seg->v2));
					if (int(pitchmin) > int(pitchtemp)) pitchmin = pitchtemp;
					pitchtemp = clipperv.PointToPseudoPitch(seg->v2->fX(), seg->v2->fY(),
															(reflect != 0) ?
															2 * planez - sector->ceilingplane.ZatPoint(seg->v2) :
															sector->ceilingplane.ZatPoint(seg->v2));
					if (int(pitchmax) < int(pitchtemp)) pitchmax = pitchtemp;
					pitchvisible |= clipperv.SafeCheckRange(pitchmin, pitchmax);
				}
				if (pitchvisible && anglevisible && radarvisible) break;
			}
			seg++;
		}
		// Skip subsector if outside vertical or horizontal clippers or is in unexplored territory (fog of war)
		if(!pitchvisible || !anglevisible || (!radarvisible && useRadarClip))
		{
			if (IsVRScene) VRSubsectorCull.Unclock();
			if (IsVRScene) VRSubsectors.Unclock();
			return;
		}
	}

	if (mClipPortal)
	{
		int clipres = mClipPortal->ClipSubsector(sub);
		if (clipres == PClip_InFront)
		{
			fakesector = hw_FakeFlat(sector, in_area, false);
			auto line = mClipPortal->ClipLine();
			// The subsector is out of range, but we still have to check lines that lie directly on the boundary and may expose their upper or lower parts.
			if (line) AddSpecialPortalLines(sub, fakesector, line);
			if (IsVRScene) VRSubsectorCull.Unclock();
			if (IsVRScene) VRSubsectors.Unclock();
			return;
		}
	}
	fakesector=hw_FakeFlat(sector, in_area, false);
	if (IsVRScene) VRSubsectorCull.Unclock();
	if (IsVRScene) VRSubsectorVisible.Clock();
	ProcessVisibleSubsector(sub, sector, fakesector);
	if (IsVRScene) VRSubsectorVisible.Unclock();
	if (IsVRScene) VRSubsectors.Unclock();
}




//==========================================================================
//
// RenderBSPNode
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.
//
//==========================================================================

void HWDrawInfo::RenderBSPNode (void *node)
{
	if (Level->nodes.Size() == 0)
	{
		DoSubsector (&Level->subsectors[0]);
		return;
	}
	while (!((size_t)node & 1))  // Keep going until found a subsector
	{
		node_t *bsp = (node_t *)node;

		// Decide which side the view point is on.
		int side = R_PointOnSide(viewx, viewy, bsp);

		// Recursively divide front space (toward the viewer).
		RenderBSPNode (bsp->children[side]);

		// Possibly divide back space (away from the viewer).
		side ^= 1;

		// It is not necessary to use the slower precise version here
		if (!mClipper->CheckBox(bsp->bbox[side]))
		{
			if (!(no_renderflags[bsp->Index()] & SSRF_SEEN))
				return;
		}
		if (Viewpoint.bDoOrtho)
		{
			if (!vClipper->CheckBoxOrthoPitch(bsp->bbox[side]))
			{
				if (!(no_renderflags[bsp->Index()] & SSRF_SEEN))
					return;
			}
		}

		node = bsp->children[side];
	}
	DoSubsector ((subsector_t *)((uint8_t *)node - 1));
}

// No need for clipping inside frustum if no fog of war (How is this faster!)
void HWDrawInfo::RenderOrthoNoFog()
{
	if (Viewpoint.bDoOrtho && ((Level->flags3 & LEVEL3_NOFOGOFWAR) || !r_radarclipper))
	{
		double vxdbl = Viewpoint.OffPos.X;
		double vydbl = Viewpoint.OffPos.Y;
		double ext = Viewpoint.camera->ViewPos->Offset.Length() ?
			3.0 * Viewpoint.camera->ViewPos->Offset.Length() * tan (Viewpoint.GetFieldOfView().Radians()*0.5) : 100.0;
		FBoundingBox viewbox(vxdbl, vydbl, ext);

		for (unsigned int kk = 0; kk < Level->subsectors.Size(); kk++)
		{
			if (Level->subsectors[kk].bbox.CheckOverlap(viewbox))
			{
				DoSubsector (&Level->subsectors[kk]);
			}
		}
	}
}

void HWDrawInfo::RenderBSP(void *node, bool drawpsprites)
{
	ClearDitherTargets();
	Bsp.Clock();

	// Give the DrawInfo the viewpoint in fixed point because that's what the nodes are.
	viewx = FLOAT2FIXED(Viewpoint.Pos.X);
	viewy = FLOAT2FIXED(Viewpoint.Pos.Y);
	if (r_radarclipper && !(Level->flags3 & LEVEL3_NOFOGOFWAR) && Viewpoint.bDoOob)
	{
		viewx = FLOAT2FIXED(Viewpoint.OffPos.X);
		viewy = FLOAT2FIXED(Viewpoint.OffPos.Y);
	}

	validcount++;	// used for processing sidedefs only once by the renderer.

	const bool allowVRMultithread = IsVRScene && vr_scene_multithread;
	multithread = gl_multithread && (!IsVRScene || allowVRMultithread);
	experimentalMultiWallWorkers = multithread && gl_bsp_worker_threads > 1;
	if (multithread)
	{
		sceneJobQueue.ReleaseAll();
		wallJobQueue.ReleaseAll();
		wallWorkersStarted = false;
		wallWorkerFutures.clear();
		auto sceneFuture = renderPool.push([&](int id) {
			WorkerThread();
		});
		if (experimentalMultiWallWorkers)
		{
			const int workerCount = gl_bsp_worker_threads;
			if ((int)wallWorkerMeshes.size() < workerCount)
			{
				wallWorkerMeshes.resize(workerCount);
			}
			for (int i = 0; i < workerCount; i++)
			{
				auto& mesh = wallWorkerMeshes[i];
				mesh.list.Clear();
				mesh.translucent.Clear();
				mesh.portals.Clear();
				mesh.lower.Clear();
				mesh.upper.Clear();
				mesh.wallCount = 0;
				mesh.batchCount = 0;
				mesh.totalCycles = 0;
				mesh.wallCycles = 0;
			}

			if (Viewpoint.bDoOrtho && ((Level->flags3 & LEVEL3_NOFOGOFWAR) || !r_radarclipper)) RenderOrthoNoFog();
			else RenderBSPNode(node);

			sceneJobQueue.AddJob(RenderJob::TerminateJob, nullptr, nullptr);
			if (wallWorkersStarted)
			{
				for (int i = 0; i < workerCount; i++)
				{
					wallJobQueue.AddTerminate();
				}
			}
			Bsp.Unclock();
			MTWait.Clock();
			sceneFuture.wait();
			for (auto& future : wallWorkerFutures)
			{
				future.wait();
			}

			HWWallDispatcher mergeDispatcher(this);
			WallMerge.Clock();
			for (int i = 0; i < workerCount; i++)
			{
				auto& mesh = wallWorkerMeshes[i];
				WallWorkersCpuSumCycles += mesh.totalCycles;
				WallWorkersWallCpuSumCycles += mesh.wallCycles;
				if (mesh.totalCycles > WallWorkersElapsedCycles) WallWorkersElapsedCycles = mesh.totalCycles;
				WallBatchCount += mesh.batchCount;
				WallItemsProcessed += mesh.wallCount;
				rendered_lines += mesh.wallCount;
				for (auto& wall : mesh.list)
				{
					wall.PutWall(&mergeDispatcher, false);
				}
				for (auto& wall : mesh.translucent)
				{
					wall.PutWall(&mergeDispatcher, true);
				}
				for (auto& wall : mesh.portals)
				{
					wall.PutPortal(&mergeDispatcher, wall.portaltype, wall.portalplane);
				}
				for (auto& missing : mesh.upper)
				{
					AddUpperMissingTexture(missing.side, missing.sub, (float)missing.plane);
				}
				for (auto& missing : mesh.lower)
				{
					AddLowerMissingTexture(missing.side, missing.sub, (float)missing.plane);
				}
			}
			WallMerge.Unclock();
			MTWait.Unclock();
		}
		else
		{
			if (Viewpoint.bDoOrtho && ((Level->flags3 & LEVEL3_NOFOGOFWAR) || !r_radarclipper)) RenderOrthoNoFog();
			else RenderBSPNode(node);

			sceneJobQueue.AddJob(RenderJob::TerminateJob, nullptr, nullptr);
			Bsp.Unclock();
			MTWait.Clock();
			sceneFuture.wait();
			MTWait.Unclock();
		}
	}
	else
	{
		if (Viewpoint.bDoOrtho && ((Level->flags3 & LEVEL3_NOFOGOFWAR) || !r_radarclipper)) RenderOrthoNoFog();
		else RenderBSPNode(node);
		Bsp.Unclock();
	}
	experimentalMultiWallWorkers = false;

	// Make rendered targets set dither transparency flags on level geometry for next pass
	// Can't do this inside DoSubsector() because both Trace() and P_CheckSight() affect 'validcount' global variable
	for (int ii = 0; ii < MAXDITHERACTORS; ii++)
	{
		if ( RenderedTargets[ii] && P_CheckSight(players[consoleplayer].mo, RenderedTargets[ii], 0) )
		{
			SetDitherTransFlags(RenderedTargets[ii]);
		}
	}

	// Process all the sprites on the current portal's back side which touch the portal.
	if (mCurrentPortal != nullptr) mCurrentPortal->RenderAttached(this);

	if (drawpsprites)
	{
		if (IsVRScene) VRPlayerSprites.Clock();
		PreparePlayerSprites(Viewpoint.sector, in_area);
		if (IsVRScene) VRPlayerSprites.Unclock();
	}
}
