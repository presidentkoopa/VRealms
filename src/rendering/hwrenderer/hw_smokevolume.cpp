/*
** hw_smokevolume.cpp
**
** [SMOKEVOLUME] The smoke volume's CPU side. See the header.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** The event conversion (the slabs), the queue cursors, the bounds and the recentre
** arithmetic are mirrored in Python ("Engine docs/SMOKE_13B_IMPL_NOTES.md", mirror13b).
**
*/

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include "hw_smokevolume.h"
#include "hw_levelsolidity.h"
#include "hw_sectorplanes.h"
#include "hw_framecompute.h"
#include "hw_cvars.h"
#include "hw_perflog.h"
#include "g_levellocals.h"
#include "a_dynlight.h"		// [13d] FDynamicLight, r_dynlights: the lights in the smoke
#include "hw_effectlights.h"		// [EFFECTLIGHTS] LD: this frame's effect-light bins (EffectLights::FrameBins)
#include "hw_effectlightbuffer.h"	// [EFFECTLIGHTS] LD: the records and bins the GPU holds (GetLiveCount)
#include "r_utility.h"				// [13F] r_viewpoint: the camera the darkness height follows (DarkHeightFollow)
#include "doomdef.h"
#include "v_video.h"
#include "i_time.h"
#include "printf.h"

// [SMOKEVOLUME] 13d: r_smoke_light_quality -- how finely the light inside the smoke is worked out, as light cells
// per smoke tile: 1 = 4 (half the smoke grid's resolution), 2 = 6 (three quarters, the default), 3 = 8 (the smoke
// grid's own); about 10 / 32 / 76 MB at the default smoke quality (hw_framecompute.h, SmokeLightGridFor).
// Renderer-read every frame in PrepareLight, so it responds with a menu open; a change re-makes only the light
// grid. Defined beside its one reader.
CVAR(Int, r_smoke_light_quality, SMOKE_LIGHT_QUALITY_DEFAULT, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [13F] r_smoke_surfaceglow -- the smoke takes the light its room's surfaces give it: glowing floors, walls and ceilings (a
// sector's glow lanes from any mod, or a texture's GLDEFS glow) and sweep bands (their light, lift, crush and recolour). An A/B
// check, on by default. Renderer-read every frame in PrepareLight (UpdateSurfaceLight), so it responds with a menu open.
CVAR(Bool, r_smoke_surfaceglow, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [13F] r_smoke_darkness -- the smoke's room light takes the room's grading as surfaces do: the darkness curve (SetDarkness,
// SetDarknessSpace, SetDarknessHeightFollow) and a sweep's passed look. An A/B check, on by default. Renderer-read every frame.
CVAR(Bool, r_smoke_darkness, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [13F] smoke_light.comp's SmokeSurfaceSSO holds a fixed eight bands: the level's band count.
static_assert(SMOKE_SURFACE_BANDS == FLevelLocals::MAX_SWEEP_BANDS, "SMOKE_SURFACE_BANDS must be FLevelLocals::MAX_SWEEP_BANDS (smoke_light.comp)");

namespace
{
	// Floor division for cell -> tile, correct for negative cells.
	int FloorDiv(int a, int b)
	{
		int q = a / b;
		if ((a % b != 0) && ((a < 0) != (b < 0)))
			q--;
		return q;
	}

	// Clamped into int range before converting, so an event far outside the map never
	// overflows; anything that far is outside every box anyway.
	int CellFloor(double v) { return (int)std::floor(std::clamp(v, -1.0e8, 1.0e8)); }
	int CellCeil(double v) { return (int)std::ceil(std::clamp(v, -1.0e8, 1.0e8)); }

	// Reads one of the level's queues from a reader's own cursor: every event not read
	// before, the older generation first. Nothing is written to the queue.
	template<class T, int N, class Take>
	void ReadQueue(const FEffectTicQueue<T, N>& queue, uint64_t& cursorSerial, int& cursorCount, Take&& take)
	{
		int order[2] = { 0, 1 };
		if (queue.Serial[1] < queue.Serial[0])
		{
			order[0] = 1;
			order[1] = 0;
		}
		for (int g : order)
		{
			const uint64_t serial = queue.Serial[g];
			if (serial == 0 || serial < cursorSerial)
				continue;
			const int count = std::clamp(queue.Count[g], 0, N);
			const int first = serial == cursorSerial ? std::min(cursorCount, count) : 0;
			for (int i = first; i < count; i++)
				take(queue.Items[g][i], queue.Tic[g]);
			cursorSerial = serial;
			cursorCount = count;
		}
	}
}

SmokeVolume& SmokeVolume::Get()
{
	static SmokeVolume volume;
	return volume;
}

//-----------------------------------------------------------------------------
//
// The frame
//
//-----------------------------------------------------------------------------

void SmokeVolume::PrepareFrame(FLevelLocals* Level, const DVector3& eye, double viewYaw, double ticFrac, uint64_t levelSerial, SmokeVolumeFrame& out)
{
	out = SmokeVolumeFrame();
	mDraw = DrawState();	// [13c] nothing to draw, unless the end of this function says otherwise
	mKernels.clear();
	mMaskUploads.clear();
	mMaskBytes.clear();
	if (Level == nullptr)
		return;

	const int maptime = Level->maptime;
	const int quality = std::clamp((int)r_smoke_quality, SMOKE_QUALITY_MIN, SMOKE_QUALITY_MAX);
	// Renderer-read every frame, so they all respond with a menu open. Vulkan only: GL
	// and GLES run no compute, so they do no CPU work for it either.
	const bool vulkan = screen != nullptr && screen->IsVulkan();
	const bool smokeOn = vulkan && r_smoke;
	const bool testOn = smokeOn && r_smoke_computetest;
	const SmokeVolumeBackendStatus& status = SmokeVolumeStatus();
	const bool refused = status.RefusedQuality != 0 && status.RefusedQuality == quality;

	if (levelSerial != mLevelSerial)
	{
		mLevelSerial = levelSerial;
		mClockValid = false;
		mPlaced = false;
		// The old map's events and smoke are gone (ClearLevelData emptied the queues).
		ForgetEvents();
		mClearSerial = Level->SmokeClearSerial;
		mTestPlaced = false;
		// A volume in use keeps lingering into the new map, so it is not freed now and
		// made again at the new map's first emit.
		if (mHasDemand)
			mLastDemandTime = maptime;
	}

	// The level's new events. Always read, so the cursors keep up; kept only when this
	// machine will simulate them, so switching smoke on never replays old ones.
	ReadQueues(Level, smokeOn && !refused);

	// ClearSmoke: this machine's volume empties. Events from before this tic went with
	// it; this tic's may have been queued after the call, and stay.
	bool clearNow = false;
	if (Level->SmokeClearSerial != mClearSerial)
	{
		mClearSerial = Level->SmokeClearSerial;
		clearNow = true;
		const int clearTic = Level->SmokeEmits.Tic[Level->SmokeEmits.Current & 1];
		mPending.erase(std::remove_if(mPending.begin(), mPending.end(),
			[clearTic](const PendingEvent& e) { return e.Tic < clearTic; }), mPending.end());
		mBound = Bounds();
	}

	// Who asks for smoke: a waiting emit or carve, or the test source -- and smoke still
	// in the air keeps asking, so a slow haze outlives the linger.
	const bool quietBefore = mBound.Density <= SMOKE_EMPTY_DENSITY && mBound.Heat <= SMOKE_EMPTY_HEAT && mBound.Speed <= SMOKE_EMPTY_VELOCITY;
	const bool demandNow = testOn || HasPendingSmoke();
	if (demandNow)
	{
		mHasDemand = true;
		mLastDemandTime = maptime;
	}
	if (maptime < mLastDemandTime)
		mLastDemandTime = maptime;
	if (mHasDemand && !quietBefore)
		mLastDemandTime = maptime;
	if (mHasDemand && maptime - mLastDemandTime >= LINGER_SECONDS * TICRATE)
		mHasDemand = false;

	out.Active = smokeOn && mHasDemand;
	out.Quality = quality;
	out.Grid = SmokeGridForQuality(quality);
	out.MapTime = maptime;
	out.TicFrac = (float)ticFrac;
	out.LevelSerial = levelSerial;

	// One step per world tic, at most MAX_STEPS_PER_FRAME a frame; a bigger jump drops
	// the backlog. The clock only runs while the volume is active.
	if (!out.Active || !mClockValid || maptime < mLastStepTime)
	{
		mClockValid = true;
		mLastStepTime = maptime;
		out.Steps = 0;
	}
	else
	{
		out.Steps = std::min(maptime - mLastStepTime, SmokeVolumeFrame::MAX_STEPS_PER_FRAME);
		mLastStepTime = maptime;
	}

	if (!out.Active || refused)
	{
		// Inactive: the backend frees the volume. Refused: this device cannot make this
		// quality (logged by the backend), so there is nothing to build or inject until the
		// quality changes or smoke stops being asked for.
		mPlaced = false;
		mMaskTiles.clear();
		mUnbuiltTiles = 0;
		mBuildLogPending = false;
		mPending.clear();
		mBound = Bounds();
		mResidue = false;
		if (!testOn)
			mTestPlaced = false;
		return;
	}

	// ---- The box: whole tiles, world-aligned. ----
	const SmokeGridSpec& grid = out.Grid;
	const int size[3] = { grid.SizeX, grid.SizeY, grid.SizeZ };
	const double cell = grid.CellSize;
	const double extent[3] = { size[0] * cell, size[1] * cell, size[2] * cell };
	const double eyeShare[3] = { 0.5, 0.5, EYE_HEIGHT_FRACTION };
	const double eyeAt[3] = { eye.X, eye.Y, eye.Z };

	int snapped[3];
	for (int axis = 0; axis < 3; axis++)
	{
		const double ideal = (eyeAt[axis] - extent[axis] * eyeShare[axis]) / cell;
		snapped[axis] = SMOKE_TILE_CELLS * CellFloor(ideal / SMOKE_TILE_CELLS + 0.5);
	}

	bool newBox = !mPlaced || mPlacedQuality != quality;
	// The backend's mask was initialised for a different box than the one this side is
	// building -- a device reset re-made the volume underneath it: start again.
	if (!newBox && status.Allocated && status.Quality == quality && status.MaskEpoch != mBoxEpoch)
		newBox = true;

	bool recentre = false;
	if (!newBox)
	{
		for (int axis = 0; axis < 3; axis++)
		{
			const double inBox = eyeAt[axis] - mOriginCell[axis] * cell;
			if (std::fabs(inBox - extent[axis] * eyeShare[axis]) > extent[axis] * RECENTRE_FRACTION)
				recentre = true;
		}
	}

	int shift[3] = { 0, 0, 0 };
	if (recentre)
	{
		bool moved = false;
		for (int axis = 0; axis < 3; axis++)
		{
			shift[axis] = snapped[axis] - mOriginCell[axis];
			if (std::abs(shift[axis]) >= size[axis])
				newBox = true;	// further than the box: nothing to keep
			if (shift[axis] != 0)
				moved = true;
		}
		if (!moved)
			recentre = false;
	}

	if (newBox)
	{
		for (int axis = 0; axis < 3; axis++)
			mOriginCell[axis] = snapped[axis];
		mPlaced = true;
		mPlacedQuality = quality;
		mBoxEpoch++;
		out.NewBox = true;
		// The backend zeroes everything: nothing is left to bound.
		mBound = Bounds();
		mResidue = false;
	}
	else if (recentre)
	{
		for (int axis = 0; axis < 3; axis++)
		{
			out.ShiftCells[axis] = shift[axis];
			mOriginCell[axis] = snapped[axis];
		}
	}
	out.BoxEpoch = mBoxEpoch;
	for (int axis = 0; axis < 3; axis++)
		out.OriginCell[axis] = mOriginCell[axis];

	if (out.NewBox)
		StartMaskBuild(out);
	else if (recentre)
		AddExposedWork(out, shift);

	// [SECTORPLANES] The box is an active region: watch the sectors under it. A new box's
	// build reads the planes as they are now, so only later changes re-do tiles.
	SectorPlanes& planes = SectorPlanes::Get();
	const double minX = mOriginCell[0] * cell;
	const double minY = mOriginCell[1] * cell;
	planes.PollBox(Level, minX, minY, minX + extent[0], minY + extent[1]);
	if (out.NewBox)
		mPlanesGeneration = planes.Generation();
	else
		MarkDirtySectors(out, planes);

	// The test source: placed ahead of the view when switched on, fixed in the world.
	if (testOn)
	{
		if (!mTestPlaced)
		{
			mTestPlaced = true;
			mTestDir = DVector3(std::cos(viewYaw), std::sin(viewYaw), 0.);
			mTestPos = eye + mTestDir * 128. + DVector3(0., 0., -16.);
			Printf("SmokeVolume: test source placed at (%.0f, %.0f, %.0f) -- a puff every tic, a round through it every 10 tics, a blast every 3 seconds\n",
				mTestPos.X, mTestPos.Y, mTestPos.Z);
		}
		for (int i = 0; i < out.Steps; i++)
		{
			const int stepTime = maptime - (out.Steps - 1 - i);
			AddTestSourceEvents(stepTime - 1);
		}
	}
	else
	{
		mTestPlaced = false;
	}

	// ---- The mask, then the events that can go in. ----
	RunMaskWork(Level, out, eye);

	FillSimSettings(Level, out);

	double sums[SmokeVolumeFrame::MAX_STEPS_PER_FRAME][4] = {};	// density, heat, speed, [13e] soot
	if (out.Steps > 0)
		BuildKernels(out, sums);

	if (clearNow)
	{
		out.ClearContents = true;
		mResidue = false;
	}

	// ---- Quiet or not. ----
	const SmokeSimSettings& sim = out.Sim;
	if (out.Steps > 0 && (!quietBefore || !mKernels.empty()))
	{
		out.Simulate = true;
		mResidue = true;
		for (int i = 0; i < out.Steps; i++)
		{
			// Upper bounds: advection, diffusion and the walls only ever average what is
			// there, so nothing grows but by what goes in and what heat and density lift.
			mBound.Density = mBound.Density * sim.DensityKeep + sums[i][0];
			mBound.Heat = mBound.Heat * sim.HeatKeep + sums[i][1];
			mBound.Speed = mBound.Speed * sim.VelocityKeep + sums[i][2] +
				std::fabs(sim.HeatLift) * mBound.Heat + std::fabs(sim.DensityLift) * std::min(mBound.Density, 1.0);
			mBound.Speed = std::min(mBound.Speed, (double)sim.MaxSpeed);
			// [13e] Soot is carried with the density's own keep, diffusion and snap, and never above it.
			mBound.Soot = std::min(mBound.Soot * sim.DensityKeep + sums[i][3], mBound.Density);
		}
	}
	else if (quietBefore && mResidue)
	{
		// Went quiet: whatever is left is below every empty line. Emptied once, for good.
		out.ClearContents = true;
		mResidue = false;
	}
	out.HasSmoke = mBound.Density > SMOKE_EMPTY_DENSITY;
	// [13e] While no soot can be in the volume the step neither reads nor carries it, and the light grid is not darkened.
	out.Sim.SootLive = mBound.Soot > SMOKE_EMPTY_DENSITY ? 1.0f : 0.0f;

	// [13c] For the drawing, later this frame (GetDrawState). Every earlier return leaves the reset
	// state of the top of this function: nothing to draw.
	mDraw.HasSmoke = out.HasSmoke;
	mDraw.Quality = out.Quality;
	mDraw.Grid = out.Grid;
	for (int axis = 0; axis < 3; axis++)
		mDraw.OriginCell[axis] = out.OriginCell[axis];
	mDraw.TicFrac = out.TicFrac;

	// [13d] The light grid: its quality and ambient columns while the volume is active, its lights when there is
	// smoke to draw.
	PrepareLight(Level, eye, out);
	mDraw.LightQuality = out.Light.Quality;

	// [13e] The beams that may meet the smoke, when there is smoke to draw.
	if (out.HasSmoke)
		GatherBeams(Level, eye, ticFrac, out);
	else
		mBeams.clear();

	out.Kernels = mKernels.empty() ? nullptr : mKernels.data();
	out.KernelCount = (int)mKernels.size();
	out.MaskUploads = mMaskUploads.empty() ? nullptr : mMaskUploads.data();
	out.MaskUploadCount = (int)mMaskUploads.size();
	out.MaskBytes = mMaskBytes.empty() ? nullptr : mMaskBytes.data();
	out.MaskByteCount = mMaskBytes.size();
}

//-----------------------------------------------------------------------------
//
// Events
//
//-----------------------------------------------------------------------------

void SmokeVolume::ForgetEvents()
{
	mEmitCursor = QueueCursor();
	mCarveCursor = QueueCursor();
	mImpulseCursor = QueueCursor();
	mPending.clear();
	mPendingFullLogged = false;
	mBound = Bounds();
}

void SmokeVolume::ReadQueues(FLevelLocals* Level, bool keep)
{
	// Emits, then carves, then pushes: within one tic a round carves the puff it passes.
	ReadQueue(Level->SmokeEmits, mEmitCursor.Serial, mEmitCursor.Count, [&](const FSmokeEmitEvent& q, int tic)
	{
		if (!keep)
			return;
		PendingEvent e;
		e.Kind = SmokeKernel::EMIT;
		e.Tic = tic;
		e.Start = q.Pos;
		e.End = q.Capsule ? q.End : q.Pos;
		e.Vel = q.Vel;
		e.Radius = q.Radius;
		e.Amount = q.Amount;
		e.Heat = q.Heat;
		e.Soot = q.Soot;	// [13e]
		AddPending(e);
	});
	ReadQueue(Level->SmokeCarves, mCarveCursor.Serial, mCarveCursor.Count, [&](const FSmokeCarveEvent& q, int tic)
	{
		if (!keep)
			return;
		PendingEvent e;
		e.Kind = SmokeKernel::CARVE;
		e.Tic = tic;
		e.Start = q.Start;
		e.End = q.End;
		e.Radius = q.Radius;
		e.Amount = q.Amount;
		AddPending(e);
	});
	ReadQueue(Level->EffectImpulses, mImpulseCursor.Serial, mImpulseCursor.Count, [&](const FEffectImpulseEvent& q, int tic)
	{
		if (!keep)
			return;
		PendingEvent e;
		e.Kind = SmokeKernel::IMPULSE;
		e.Tic = tic;
		e.Start = q.Pos;
		e.End = q.Pos;
		e.Radius = q.Radius;
		e.Strength = q.Strength;
		AddPending(e);
	});
}

void SmokeVolume::AddPending(const PendingEvent& e)
{
	if ((int)mPending.size() >= MAX_PENDING_EVENTS)
	{
		if (!mPendingFullLogged)
		{
			mPendingFullLogged = true;
			Printf("SmokeVolume: more than %d smoke events waiting -- newer ones are dropped (logged once per map)\n", MAX_PENDING_EVENTS);
		}
		return;
	}
	mPending.push_back(e);
}

bool SmokeVolume::HasPendingSmoke() const
{
	for (const PendingEvent& e : mPending)
	{
		if (e.Kind != SmokeKernel::IMPULSE)
			return true;
	}
	return false;
}

// The test source's events for one tic. A function of the tic alone -- no RNG -- so it
// is the same load every run.
void SmokeVolume::AddTestSourceEvents(int tic)
{
	PendingEvent puff;
	puff.Kind = SmokeKernel::EMIT;
	puff.Tic = tic;
	puff.Start = puff.End = mTestPos;
	puff.Vel = DVector3(0., 0., 40.);
	puff.Radius = 20.;
	puff.Amount = 0.35;
	puff.Heat = 0.8;
	AddPending(puff);

	if (tic % 10 == 0)
	{
		PendingEvent round;
		round.Kind = SmokeKernel::CARVE;
		round.Tic = tic;
		round.Start = mTestPos - mTestDir * 256.;
		round.End = mTestPos + mTestDir * 256.;
		round.Radius = 6.;
		round.Amount = 0.9;
		AddPending(round);
	}

	if (tic % 105 == 0)
	{
		PendingEvent cloud;
		cloud.Kind = SmokeKernel::EMIT;
		cloud.Tic = tic;
		cloud.Start = cloud.End = mTestPos;
		cloud.Radius = 48.;
		cloud.Amount = 2.;
		cloud.Heat = 4.;
		AddPending(cloud);

		PendingEvent blast;
		blast.Kind = SmokeKernel::IMPULSE;
		blast.Tic = tic;
		blast.Start = blast.End = mTestPos + DVector3(0., 0., -16.);
		blast.Radius = 192.;
		blast.Strength = 700.;
		AddPending(blast);
	}
}

// An event in grid cells of this frame's box, and the cells it can reach. False when it
// reaches none (outside the box).
bool SmokeVolume::MakeShape(const PendingEvent& e, const SmokeVolumeFrame& frame, EventShape& shape) const
{
	const double cell = frame.Grid.CellSize;
	const int size[3] = { frame.Grid.SizeX, frame.Grid.SizeY, frame.Grid.SizeZ };
	const double start[3] = { e.Start.X, e.Start.Y, e.Start.Z };
	const double end[3] = { e.End.X, e.End.Y, e.End.Z };

	double length2 = 0;
	for (int axis = 0; axis < 3; axis++)
	{
		shape.A[axis] = start[axis] / cell - frame.OriginCell[axis];
		shape.B[axis] = end[axis] / cell - frame.OriginCell[axis];
		const double d = shape.B[axis] - shape.A[axis];
		length2 += d * d;
	}

	const double radius = e.Radius / cell;
	shape.Radius = std::max(radius, (double)SmokeKernel::MIN_RADIUS);
	// Widening a kernel to MIN_RADIUS would add smoke (or remove it, or push) over more
	// cells than the event covers: scale by the volume a ball lost, the cross-section a
	// capsule lost.
	const bool ball = std::sqrt(length2) < shape.Radius;
	shape.Scale = radius >= shape.Radius ? 1.0 : std::pow(radius / shape.Radius, ball ? 3.0 : 2.0);

	for (int axis = 0; axis < 3; axis++)
	{
		const double lo = std::min(shape.A[axis], shape.B[axis]) - shape.Radius;
		const double hi = std::max(shape.A[axis], shape.B[axis]) + shape.Radius;
		// Cells whose centre (i + 0.5) lies in [lo, hi].
		shape.Min[axis] = std::max(CellCeil(lo - 0.5), 0);
		shape.Max[axis] = std::min(CellFloor(hi - 0.5) + 1, size[axis]);
		if (shape.Min[axis] >= shape.Max[axis])
			return false;
	}
	return true;
}

void SmokeVolume::BuildKernels(const SmokeVolumeFrame& frame, double sums[][4])
{
	const double toCellsPerStep = 1.0 / (TICRATE * (double)frame.Grid.CellSize);
	const int firstStepTime = frame.MapTime - frame.Steps + 1;

	size_t keep = 0;
	for (size_t i = 0; i < mPending.size(); i++)
	{
		PendingEvent e = mPending[i];
		EventShape shape;
		if (!MakeShape(e, frame, shape))
			continue;	// outside the box: nothing of it can be simulated or seen

		if (mUnbuiltTiles > 0 && TouchesUnbuilt(shape, frame))
		{
			// Its walls are not in the mask yet: wait for them (their tiles go first).
			if (e.HeldSince < 0)
				e.HeldSince = frame.MapTime;
			if (frame.MapTime - e.HeldSince <= MAX_HOLD_TICS)
				mPending[keep++] = e;
			continue;
		}

		if ((int)mKernels.size() + MAX_SLABS_PER_EVENT > MAX_KERNELS_PER_FRAME)
		{
			mPending[keep++] = e;	// next frame
			continue;
		}

		// The step of the tic it was queued in; one that waited goes into the first.
		const int step = std::clamp(e.Tic + 1 - firstStepTime, 0, frame.Steps - 1);
		AppendKernels(e, shape, step, frame);

		switch (e.Kind)
		{
		case SmokeKernel::EMIT:
			sums[step][0] += e.Amount * shape.Scale;
			sums[step][1] += e.Heat * shape.Scale;
			sums[step][2] += e.Vel.Length() * toCellsPerStep * shape.Scale;
			sums[step][3] += e.Amount * e.Soot * shape.Scale;	// [13e]
			break;
		case SmokeKernel::IMPULSE:
			sums[step][2] += std::fabs(e.Strength) * toCellsPerStep * shape.Scale;
			break;
		default:
			break;
		}
	}
	mPending.resize(keep);
}

// One event as kernels. A region longer than SLAB_MIN_CELLS on its longest axis is cut
// into at most MAX_SLABS_PER_EVENT slabs across that axis, each with the box of cells the
// capsule can reach inside the slab, so a long diagonal round never dispatches over a
// huge mostly-empty box. The slabs are disjoint and together hold every cell the capsule
// reaches (mirror13b).
void SmokeVolume::AppendKernels(const PendingEvent& e, const EventShape& shape, int step, const SmokeVolumeFrame& frame)
{
	const double toCellsPerStep = 1.0 / (TICRATE * (double)frame.Grid.CellSize);

	SmokeKernel base;
	base.Kind = e.Kind;
	base.Step = step;
	for (int axis = 0; axis < 3; axis++)
	{
		base.Start[axis] = (float)shape.A[axis];
		base.End[axis] = (float)shape.B[axis];
	}
	base.Radius = (float)shape.Radius;
	switch (e.Kind)
	{
	case SmokeKernel::EMIT:
		base.Amount = (float)(e.Amount * shape.Scale);
		base.Heat = (float)(e.Heat * shape.Scale);
		base.Velocity[0] = (float)(e.Vel.X * toCellsPerStep * shape.Scale);
		base.Velocity[1] = (float)(e.Vel.Y * toCellsPerStep * shape.Scale);
		base.Velocity[2] = (float)(e.Vel.Z * toCellsPerStep * shape.Scale);
		base.Soot = (float)(e.Amount * e.Soot * shape.Scale);	// [13e] soot density: the amount's soot share
		break;
	case SmokeKernel::CARVE:
		base.Amount = (float)std::clamp(e.Amount * shape.Scale, 0.0, 1.0);
		break;
	case SmokeKernel::IMPULSE:
		base.Strength = (float)(e.Strength * toCellsPerStep * shape.Scale);
		break;
	}

	int axis = 0;
	for (int a = 1; a < 3; a++)
	{
		if (shape.Max[a] - shape.Min[a] > shape.Max[axis] - shape.Min[axis])
			axis = a;
	}
	const int length = shape.Max[axis] - shape.Min[axis];
	if (length <= SLAB_MIN_CELLS)
	{
		for (int a = 0; a < 3; a++)
		{
			base.RegionMin[a] = shape.Min[a];
			base.RegionMax[a] = shape.Max[a];
		}
		mKernels.push_back(base);
		return;
	}

	const int thickness = std::max(SLAB_MIN_CELLS, (length + MAX_SLABS_PER_EVENT - 1) / MAX_SLABS_PER_EVENT);
	const double R = shape.Radius;
	for (int s0 = shape.Min[axis]; s0 < shape.Max[axis]; s0 += thickness)
	{
		const int s1 = std::min(s0 + thickness, shape.Max[axis]);

		// The part of the segment within R of a cell centre in [s0 + 0.5, s1 - 0.5] on this
		// axis: parameters t in [t0, t1].
		const double lo = s0 + 0.5 - R;
		const double hi = s1 - 0.5 + R;
		const double a0 = shape.A[axis];
		const double d = shape.B[axis] - shape.A[axis];
		double t0 = 0, t1 = 1;
		if (std::fabs(d) < 1e-12)
		{
			if (a0 < lo || a0 > hi)
				continue;
		}
		else
		{
			double ta = (lo - a0) / d;
			double tb = (hi - a0) / d;
			if (ta > tb)
				std::swap(ta, tb);
			t0 = std::max(0.0, ta);
			t1 = std::min(1.0, tb);
			if (t0 > t1)
				continue;
		}

		SmokeKernel k = base;
		bool empty = false;
		for (int b = 0; b < 3; b++)
		{
			if (b == axis)
			{
				k.RegionMin[b] = s0;
				k.RegionMax[b] = s1;
				continue;
			}
			const double p0 = shape.A[b] + (shape.B[b] - shape.A[b]) * t0;
			const double p1 = shape.A[b] + (shape.B[b] - shape.A[b]) * t1;
			k.RegionMin[b] = std::max(CellCeil(std::min(p0, p1) - R - 0.5), shape.Min[b]);
			k.RegionMax[b] = std::min(CellFloor(std::max(p0, p1) + R - 0.5) + 1, shape.Max[b]);
			if (k.RegionMin[b] >= k.RegionMax[b])
				empty = true;
		}
		if (!empty)
			mKernels.push_back(k);
	}
}

void SmokeVolume::FillSimSettings(FLevelLocals* Level, SmokeVolumeFrame& out) const
{
	const double cell = out.Grid.CellSize;
	const double dt = 1.0 / TICRATE;
	const FLevelLocals::SmokeLookSettings& look = Level->SmokeLook;

	// r_smoke_dissipation_scale: the player's "Smoke fade speed", renderer-read over the
	// mod's look. NaN reads as 1; clamped 0..16.
	double fade = r_smoke_dissipation_scale;
	if (std::isnan(fade))
		fade = 1.0;
	fade = std::clamp(fade, 0.0, 16.0);

	SmokeSimSettings& sim = out.Sim;
	sim.DensityKeep = (float)std::exp(-std::clamp(look.Dissipation, 0.0, 10.0) * fade * dt);
	sim.HeatKeep = (float)std::exp(-HEAT_COOLING_PER_SECOND * dt);
	sim.VelocityKeep = (float)std::exp(-VELOCITY_DAMPING_PER_SECOND * dt);
	sim.Diffusion = (float)DIFFUSION_PER_STEP;

	const double buoyancy = std::clamp(look.Buoyancy, -4.0, 4.0);
	sim.HeatLift = (float)(buoyancy * HEAT_LIFT * dt * dt / cell);
	sim.DensityLift = (float)(buoyancy * DENSITY_LIFT * dt * dt / cell);

	const double wind[3] = { Level->SmokeWind.X, Level->SmokeWind.Y, Level->SmokeWind.Z };
	for (int axis = 0; axis < 3; axis++)
		sim.Wind[axis] = (float)std::clamp(wind[axis] * dt / cell, -MAX_CELLS_PER_STEP, MAX_CELLS_PER_STEP);

	sim.Turbulence = (float)(TURBULENCE_SPEED * dt / cell);
	sim.TurbulenceFrequency = (float)(cell / TURBULENCE_WAVELENGTH);
	sim.MaxDisplacement = (float)MAX_CELLS_PER_STEP;
	sim.MaxSpeed = (float)MAX_CELLS_PER_STEP;
}

//-----------------------------------------------------------------------------
//
// The solid mask
//
//-----------------------------------------------------------------------------

void SmokeVolume::AddTileWork(int cellX0, int cellY0, int cellX1, int cellY1, bool unbuilt, int zMin, int zMax)
{
	if (cellX0 >= cellX1 || cellY0 >= cellY1)
		return;
	if (unbuilt && zMin >= zMax)
		return;

	const int tx0 = FloorDiv(cellX0, MASK_TILE_CELLS);
	const int tx1 = FloorDiv(cellX1 - 1, MASK_TILE_CELLS);
	const int ty0 = FloorDiv(cellY0, MASK_TILE_CELLS);
	const int ty1 = FloorDiv(cellY1 - 1, MASK_TILE_CELLS);
	for (int ty = ty0; ty <= ty1; ty++)
	{
		for (int tx = tx0; tx <= tx1; tx++)
		{
			MaskTile* tile = nullptr;
			for (MaskTile& t : mMaskTiles)
			{
				if (t.TileX == tx && t.TileY == ty)
				{
					tile = &t;
					break;
				}
			}
			if (tile == nullptr)
			{
				mMaskTiles.push_back(MaskTile());
				tile = &mMaskTiles.back();
				tile->TileX = tx;
				tile->TileY = ty;
			}
			if (!unbuilt)
				continue;	// a tile to do again; what is there stays valid meanwhile
			if (tile->Unbuilt)
			{
				tile->UnbuiltZMin = std::min(tile->UnbuiltZMin, zMin);
				tile->UnbuiltZMax = std::max(tile->UnbuiltZMax, zMax);
			}
			else
			{
				tile->Unbuilt = true;
				tile->UnbuiltZMin = zMin;
				tile->UnbuiltZMax = zMax;
				mUnbuiltTiles++;
			}
		}
	}
}

// A new box: the backend sets the whole mask solid; every tile is to be rasterised.
void SmokeVolume::StartMaskBuild(const SmokeVolumeFrame& frame)
{
	mMaskTiles.clear();
	mUnbuiltTiles = 0;
	const int* o = frame.OriginCell;
	AddTileWork(o[0], o[1], o[0] + frame.Grid.SizeX, o[1] + frame.Grid.SizeY, true, o[2], o[2] + frame.Grid.SizeZ);

	mBuildLogPending = true;
	mBuildNs = 0;
	mBuildFrames = 0;
	mBuildSolidCells = 0;
	mBuildCells = 0;
}

// A recentre by `shift` cells (the frame's origin is already the new one): what came into
// the box is solid until rasterised. Columns that came in on x or y are done whole;
// layers that came in on z are done in every column.
void SmokeVolume::AddExposedWork(const SmokeVolumeFrame& frame, const int shift[3])
{
	const int* o = frame.OriginCell;
	const int size[3] = { frame.Grid.SizeX, frame.Grid.SizeY, frame.Grid.SizeZ };
	const int zMin = o[2];
	const int zMax = o[2] + size[2];

	if (shift[0] > 0)
		AddTileWork(o[0] + size[0] - shift[0], o[1], o[0] + size[0], o[1] + size[1], true, zMin, zMax);
	else if (shift[0] < 0)
		AddTileWork(o[0], o[1], o[0] - shift[0], o[1] + size[1], true, zMin, zMax);

	if (shift[1] > 0)
		AddTileWork(o[0], o[1] + size[1] - shift[1], o[0] + size[0], o[1] + size[1], true, zMin, zMax);
	else if (shift[1] < 0)
		AddTileWork(o[0], o[1], o[0] + size[0], o[1] - shift[1], true, zMin, zMax);

	if (shift[2] > 0)
		AddTileWork(o[0], o[1], o[0] + size[0], o[1] + size[1], true, o[2] + size[2] - shift[2], o[2] + size[2]);
	else if (shift[2] < 0)
		AddTileWork(o[0], o[1], o[0] + size[0], o[1] + size[1], true, o[2], o[2] - shift[2]);
}

// [SECTORPLANES] A door opened, a lift moved: the tiles under that sector's lines are
// done again. Only changes after the box's build started count.
void SmokeVolume::MarkDirtySectors(const SmokeVolumeFrame& frame, SectorPlanes& planes)
{
	const double cell = frame.Grid.CellSize;
	const int* o = frame.OriginCell;
	const int boxX1 = o[0] + frame.Grid.SizeX;
	const int boxY1 = o[1] + frame.Grid.SizeY;

	for (int index : planes.ChangedThisFrame())
	{
		if (planes.ChangedAt(index) <= mPlanesGeneration)
			continue;
		double minX, minY, maxX, maxY;
		if (!planes.GetExtent(index, minX, minY, maxX, maxY))
			continue;
		// The columns its lines can mark, with a cell of margin.
		const int x0 = std::max(CellFloor(minX / cell) - 1, o[0]);
		const int y0 = std::max(CellFloor(minY / cell) - 1, o[1]);
		const int x1 = std::min(CellFloor(maxX / cell) + 2, boxX1);
		const int y1 = std::min(CellFloor(maxY / cell) + 2, boxY1);
		AddTileWork(x0, y0, x1, y1, false, 0, 0);
	}
}

bool SmokeVolume::TouchesUnbuilt(const EventShape& shape, const SmokeVolumeFrame& frame) const
{
	const int* o = frame.OriginCell;
	for (const MaskTile& tile : mMaskTiles)
	{
		if (!tile.Unbuilt)
			continue;
		const int x0 = tile.TileX * MASK_TILE_CELLS - o[0];
		const int y0 = tile.TileY * MASK_TILE_CELLS - o[1];
		const int z0 = tile.UnbuiltZMin - o[2];
		const int z1 = tile.UnbuiltZMax - o[2];
		if (shape.Max[0] <= x0 || shape.Min[0] >= x0 + MASK_TILE_CELLS)
			continue;
		if (shape.Max[1] <= y0 || shape.Min[1] >= y0 + MASK_TILE_CELLS)
			continue;
		if (shape.Max[2] <= z0 || shape.Min[2] >= z1)
			continue;
		return true;
	}
	return false;
}

void SmokeVolume::RunMaskWork(FLevelLocals* Level, const SmokeVolumeFrame& frame, const DVector3& eye)
{
	if (mMaskTiles.empty())
		return;

	const SmokeGridSpec& grid = frame.Grid;
	const double cell = grid.CellSize;
	const int* o = frame.OriginCell;

	// Tiles that left the box are forgotten.
	size_t keep = 0;
	for (size_t i = 0; i < mMaskTiles.size(); i++)
	{
		const MaskTile& tile = mMaskTiles[i];
		const int x0 = std::max(tile.TileX * MASK_TILE_CELLS - o[0], 0);
		const int x1 = std::min((tile.TileX + 1) * MASK_TILE_CELLS - o[0], grid.SizeX);
		const int y0 = std::max(tile.TileY * MASK_TILE_CELLS - o[1], 0);
		const int y1 = std::min((tile.TileY + 1) * MASK_TILE_CELLS - o[1], grid.SizeY);
		if (x0 >= x1 || y0 >= y1)
		{
			if (tile.Unbuilt)
				mUnbuiltTiles--;
			continue;
		}
		mMaskTiles[keep++] = tile;
	}
	mMaskTiles.resize(keep);
	if (mMaskTiles.empty())
		return;

	// The events waiting, in this box, so the tiles under them go first.
	std::vector<EventShape> waiting;
	if (mUnbuiltTiles > 0)
	{
		for (const PendingEvent& e : mPending)
		{
			EventShape shape;
			if (MakeShape(e, frame, shape))
				waiting.push_back(shape);
		}
	}

	struct Order
	{
		int Index;
		int Class;			// 0 = an event waits on it, 1 = the rest
		double Distance2;	// from the eye, cells squared
		int X0, Y0, X1, Y1;	// grid cells, clipped
	};
	std::vector<Order> order;
	order.reserve(mMaskTiles.size());
	const double eyeX = eye.X / cell - o[0];
	const double eyeY = eye.Y / cell - o[1];
	for (size_t i = 0; i < mMaskTiles.size(); i++)
	{
		const MaskTile& tile = mMaskTiles[i];
		Order item;
		item.Index = (int)i;
		item.X0 = std::max(tile.TileX * MASK_TILE_CELLS - o[0], 0);
		item.X1 = std::min((tile.TileX + 1) * MASK_TILE_CELLS - o[0], grid.SizeX);
		item.Y0 = std::max(tile.TileY * MASK_TILE_CELLS - o[1], 0);
		item.Y1 = std::min((tile.TileY + 1) * MASK_TILE_CELLS - o[1], grid.SizeY);
		item.Class = 1;
		if (tile.Unbuilt)
		{
			const int z0 = tile.UnbuiltZMin - o[2];
			const int z1 = tile.UnbuiltZMax - o[2];
			for (const EventShape& shape : waiting)
			{
				if (shape.Max[0] > item.X0 && shape.Min[0] < item.X1 && shape.Max[1] > item.Y0 && shape.Min[1] < item.Y1 &&
					shape.Max[2] > z0 && shape.Min[2] < z1)
				{
					item.Class = 0;
					break;
				}
			}
		}
		const double dx = (item.X0 + item.X1) * 0.5 - eyeX;
		const double dy = (item.Y0 + item.Y1) * 0.5 - eyeY;
		item.Distance2 = dx * dx + dy * dy;
		order.push_back(item);
	}
	std::sort(order.begin(), order.end(), [](const Order& a, const Order& b)
	{
		if (a.Class != b.Class)
			return a.Class < b.Class;
		return a.Distance2 < b.Distance2;
	});

	if (mMaskBytes.capacity() < SMOKE_MASK_UPLOAD_BYTES_PER_FRAME)
		mMaskBytes.reserve(SMOKE_MASK_UPLOAD_BYTES_PER_FRAME);

	const uint64_t startNs = I_nsTime();
	int eventTiles = 0;
	bool anyDone = false;
	std::vector<char> done(mMaskTiles.size(), 0);
	for (const Order& item : order)
	{
		const size_t bytes = (size_t)(item.X1 - item.X0) * (size_t)(item.Y1 - item.Y0) * (size_t)grid.SizeZ;
		if (mMaskBytes.size() + bytes > SMOKE_MASK_UPLOAD_BYTES_PER_FRAME)
			break;
		const bool urgent = item.Class == 0 && eventTiles < MASK_EVENT_TILES_PER_FRAME;
		if (!urgent && anyDone && (double)(I_nsTime() - startNs) / 1e6 >= MASK_BUDGET_MS)
			break;
		if (urgent)
			eventTiles++;

		LevelSolidity::Box box;
		box.OriginX = (o[0] + item.X0) * cell;
		box.OriginY = (o[1] + item.Y0) * cell;
		box.OriginZ = o[2] * cell;
		box.CellSize = cell;
		box.SizeX = item.X1 - item.X0;
		box.SizeY = item.Y1 - item.Y0;
		box.SizeZ = grid.SizeZ;

		const size_t offset = mMaskBytes.size();
		mMaskBytes.resize(offset + bytes);
		LevelSolidity::RasterizeColumns(Level, box, 0, 0, box.SizeX, box.SizeY, mMaskBytes.data() + offset);

		SmokeMaskUpload upload;
		upload.Min[0] = item.X0;
		upload.Min[1] = item.Y0;
		upload.Min[2] = 0;
		upload.Size[0] = box.SizeX;
		upload.Size[1] = box.SizeY;
		upload.Size[2] = box.SizeZ;
		upload.Offset = offset;
		mMaskUploads.push_back(upload);

		done[item.Index] = 1;
		anyDone = true;
	}
	const uint64_t elapsedNs = I_nsTime() - startNs;

	if (!anyDone)
		return;

	if (PerfLog::GroupsWanted())
		PerfLog::AddCpuSample("fx.smokemask", (double)elapsedNs / 1e6);

	if (mBuildLogPending)
	{
		// Counted outside the timing.
		mBuildNs += elapsedNs;
		mBuildFrames++;
		mBuildCells += mMaskBytes.size();
		for (uint8_t value : mMaskBytes)
		{
			if (value != LevelSolidity::OPEN)
				mBuildSolidCells++;
		}
	}

	keep = 0;
	for (size_t i = 0; i < mMaskTiles.size(); i++)
	{
		if (done[i])
		{
			if (mMaskTiles[i].Unbuilt)
				mUnbuiltTiles--;
			continue;
		}
		mMaskTiles[keep++] = mMaskTiles[i];
	}
	mMaskTiles.resize(keep);

	if (mBuildLogPending && mUnbuiltTiles <= 0)
	{
		mBuildLogPending = false;
		mUnbuiltTiles = 0;
		const double totalMs = (double)mBuildNs / 1e6;
		Printf("SmokeVolume: solid mask built -- %d x %d x %d cells from (%d, %d, %d), %.1f%% solid, %.1f ms of CPU over %d frames (%.2f ms a frame; budget %.1f)\n",
			grid.SizeX, grid.SizeY, grid.SizeZ,
			o[0] * grid.CellSize, o[1] * grid.CellSize, o[2] * grid.CellSize,
			mBuildCells > 0 ? 100.0 * (double)mBuildSolidCells / (double)mBuildCells : 0.0,
			totalMs, mBuildFrames, mBuildFrames > 0 ? totalMs / mBuildFrames : 0.0, MASK_BUDGET_MS);
	}
}

//-----------------------------------------------------------------------------
//
// [13d] The light grid ("Engine docs/SMOKE_VOLUME_PLAN.md" 13d): its quality, each
// column's sector light, and the dynamic lights that reach the box
//
//-----------------------------------------------------------------------------

namespace
{
	// A light whose sphere reaches the box, before the nearest SMOKE_LIGHTS_MAX are kept.
	struct LightCandidate
	{
		double Distance = 0;
		FDynamicLight* Light = nullptr;
		float Color[3] = { 0, 0, 0 };
	};
	std::vector<LightCandidate> LightCandidates;

	// A sector's light as the ambient columns hold it: 13c's lightlevel / 255 x the sector's light colour, RGBA8
	// packed r | g << 8 | b << 16, alpha 255 (so 0 means "not listed yet").
	uint32_t PackSectorLight(const sector_t* sec)
	{
		if (sec == nullptr)
			return 0xff000000u;
		const int level = std::clamp((int)sec->lightlevel, 0, 255);
		const PalEntry color = sec->Colormap.LightColor;
		const uint32_t r = (uint32_t)((level * (int)color.r + 127) / 255);
		const uint32_t g = (uint32_t)((level * (int)color.g + 127) / 255);
		const uint32_t b = (uint32_t)((level * (int)color.b + 127) / 255);
		return r | (g << 8) | (b << 16) | 0xff000000u;
	}
}

void SmokeVolume::PrepareLight(FLevelLocals* Level, const DVector3& eye, SmokeVolumeFrame& out)
{
	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	SmokeLightFrame& light = out.Light;
	light.Quality = std::clamp((int)r_smoke_light_quality, SMOKE_LIGHT_QUALITY_MIN, SMOKE_LIGHT_QUALITY_MAX);
	light.Grid = SmokeLightGridFor(out.Grid, light.Quality);
	double ambient = Level->SmokeLook.Ambient;
	if (!(ambient >= 0.0))
		ambient = 0.0;		// written this way so a NaN lands on 0 too
	light.AmbientScale = (float)std::min(ambient, 4.0);

	UpdateAmbientColumns(Level, eye, out, light);
	if (out.HasSmoke)
		GatherLights(Level, eye, out, light);
	else
		mLights.clear();
	// [EFFECTLIGHTS] LD: the effect lights that reach the grid (smoke_light.comp's pass 2), on frames with smoke to draw.
	if (out.HasSmoke)
		GatherEffectLights(Level, out, light);
	// [13F] Surface glow, sweep bands, darkness and the passed look in the ambient pass, on frames with smoke to draw.
	if (out.HasSmoke)
		UpdateSurfaceLight(Level, eye, out, light);

	// fx.smokelights (cpu_fx_ms): the light list and the ambient columns, every frame the volume is active.
	if (timed)
		PerfLog::AddCpuSample("fx.smokelights", (double)(I_nsTime() - startNs) / 1e6);
}

// Each light-grid column's sector light (see the header). A column is resolved once for as long as it stays in
// the box; the bytes are rebuilt only when a listed sector's light or colour, a column, or the fallback changed.
void SmokeVolume::UpdateAmbientColumns(FLevelLocals* Level, const DVector3& eye, const SmokeVolumeFrame& frame, SmokeLightFrame& light)
{
	light.AmbientColumns = nullptr;
	light.AmbientByteCount = 0;
	light.AmbientSerial = 0;

	const SmokeLightGridSpec& grid = light.Grid;
	const int sizeX = grid.SizeX;
	const int sizeY = grid.SizeY;
	if (sizeX <= 0 || sizeY <= 0 || grid.CellsPerTile <= 0 || !(grid.CellSize > 0.0))
		return;

	const size_t columns = (size_t)sizeX * (size_t)sizeY;
	const int B = AMBIENT_BLOCK_COLUMNS;
	const int blocksX = (sizeX + B - 1) / B;
	const int blocksY = (sizeY + B - 1) / B;
	const unsigned sectorCount = Level->sectors.Size();
	// The box's first column in world light columns: its origin is whole tiles, so this is exact.
	const int origin[2] =
	{
		frame.OriginCell[0] / SMOKE_TILE_CELLS * grid.CellsPerTile,
		frame.OriginCell[1] / SMOKE_TILE_CELLS * grid.CellsPerTile,
	};

	const auto recount = [&]()
	{
		mBlockUnknown.assign((size_t)blocksX * (size_t)blocksY, 0);
		mUnknownColumns = 0;
		mFallbackColumns = 0;
		for (int y = 0; y < sizeY; y++)
		{
			for (int x = 0; x < sizeX; x++)
			{
				const int index = mColumnSector[x + (size_t)y * (size_t)sizeX];
				if (index == -1)
				{
					mBlockUnknown[x / B + (size_t)(y / B) * (size_t)blocksX]++;
					mUnknownColumns++;
				}
				if (index < 0)
					mFallbackColumns++;
			}
		}
	};

	if (mAmbientLevelSerial != mLevelSerial || mAmbientSize[0] != sizeX || mAmbientSize[1] != sizeY ||
		mAmbientCellSize != grid.CellSize || mSectorPacked.size() != sectorCount || mColumnSector.size() != columns)
	{
		// A new map, or another grid: nothing known.
		mAmbientLevelSerial = mLevelSerial;
		mAmbientSize[0] = sizeX;
		mAmbientSize[1] = sizeY;
		mAmbientCellSize = grid.CellSize;
		mAmbientOrigin[0] = origin[0];
		mAmbientOrigin[1] = origin[1];
		mColumnSector.assign(columns, -1);
		mAmbientSectors.clear();
		mSectorPacked.assign(sectorCount, 0);
		mAmbientBytes.assign(columns * 4, 0);
		recount();
		mAmbientDirty = true;
	}
	else if (origin[0] != mAmbientOrigin[0] || origin[1] != mAmbientOrigin[1])
	{
		// A recentre: every column still inside keeps its sector, world-aligned.
		const int dx = origin[0] - mAmbientOrigin[0];
		const int dy = origin[1] - mAmbientOrigin[1];
		std::vector<int> moved(columns, -1);
		for (int y = 0; y < sizeY; y++)
		{
			const int sy = y + dy;
			if (sy < 0 || sy >= sizeY)
				continue;
			for (int x = 0; x < sizeX; x++)
			{
				const int sx = x + dx;
				if (sx < 0 || sx >= sizeX)
					continue;
				moved[x + (size_t)y * (size_t)sizeX] = mColumnSector[sx + (size_t)sy * (size_t)sizeX];
			}
		}
		mColumnSector.swap(moved);
		mAmbientOrigin[0] = origin[0];
		mAmbientOrigin[1] = origin[1];
		recount();
		mAmbientDirty = true;
	}

	// New columns, nearest the eye first, within the budget (at least one block a frame).
	if (mUnknownColumns > 0)
	{
		const double cell = grid.CellSize;
		struct Block
		{
			int Index;
			double Distance2;
		};
		std::vector<Block> order;
		for (int b = 0; b < (int)mBlockUnknown.size(); b++)
		{
			if (mBlockUnknown[b] <= 0)
				continue;
			const double cx = (origin[0] + (b % blocksX + 0.5) * B) * cell - eye.X;
			const double cy = (origin[1] + (b / blocksX + 0.5) * B) * cell - eye.Y;
			order.push_back({ b, cx * cx + cy * cy });
		}
		std::sort(order.begin(), order.end(), [](const Block& a, const Block& b) { return a.Distance2 < b.Distance2; });

		const uint64_t resolveStartNs = I_nsTime();
		bool resolvedAny = false;
		for (const Block& block : order)
		{
			if (resolvedAny && (double)(I_nsTime() - resolveStartNs) / 1e6 >= AMBIENT_BUDGET_MS)
				break;
			const int bx = block.Index % blocksX;
			const int by = block.Index / blocksX;
			const int x1 = std::min((bx + 1) * B, sizeX);
			const int y1 = std::min((by + 1) * B, sizeY);
			for (int y = by * B; y < y1; y++)
			{
				for (int x = bx * B; x < x1; x++)
				{
					int& sectorIndex = mColumnSector[x + (size_t)y * (size_t)sizeX];
					if (sectorIndex != -1)
						continue;
					const DVector2 at((origin[0] + x + 0.5) * cell, (origin[1] + y + 0.5) * cell);
					const subsector_t* ss = Level->PointInRenderSubsector(at);
					const sector_t* sec = ss != nullptr ? ss->sector : nullptr;
					const int index = sec != nullptr ? sec->Index() : -1;
					if (index >= 0 && (unsigned)index < sectorCount)
					{
						sectorIndex = index;
						mFallbackColumns--;
						if ((mSectorPacked[index] >> 24) == 0)
						{
							mSectorPacked[index] = PackSectorLight(&Level->sectors[index]);
							mAmbientSectors.push_back(index);
						}
					}
					else
					{
						sectorIndex = -2;	// no sector here: the fallback, and never looked up again
					}
				}
			}
			mUnknownColumns -= mBlockUnknown[block.Index];
			mBlockUnknown[block.Index] = 0;
			resolvedAny = true;
		}
		if (resolvedAny)
			mAmbientDirty = true;
	}

	// Sector light specials (flicker, strobe, glow) change a sector's light at tic rate.
	for (int index : mAmbientSectors)
	{
		const uint32_t packed = PackSectorLight(&Level->sectors[index]);
		if (packed != mSectorPacked[index])
		{
			mSectorPacked[index] = packed;
			mAmbientDirty = true;
		}
	}
	const uint32_t fallback = PackSectorLight(Level->PointInSector(eye.X, eye.Y));
	if (fallback != mFallbackPacked)
	{
		mFallbackPacked = fallback;
		if (mFallbackColumns > 0)
			mAmbientDirty = true;
	}

	if (mAmbientDirty)
	{
		mAmbientDirty = false;
		for (size_t i = 0; i < columns; i++)
		{
			const int index = mColumnSector[i];
			const uint32_t packed = index >= 0 ? mSectorPacked[index] : mFallbackPacked;
			uint8_t* texel = &mAmbientBytes[i * 4];
			texel[0] = (uint8_t)(packed & 0xff);
			texel[1] = (uint8_t)((packed >> 8) & 0xff);
			texel[2] = (uint8_t)((packed >> 16) & 0xff);
			texel[3] = 255;
		}
		if (++mAmbientSerial == 0)
			mAmbientSerial = 1;
	}

	light.AmbientColumns = mAmbientBytes.data();
	light.AmbientByteCount = mAmbientBytes.size();
	light.AmbientSerial = mAmbientSerial;
}

// The dynamic lights in the smoke: every light whose sphere reaches the box, the nearest SMOKE_LIGHTS_MAX to the eye
// (distance to the sphere). No frustum test -- a light behind the viewer still lights the smoke ahead. Colour as stage
// 2d's view lights (GetDynSpriteLight's), times the look's scatter. DontLightOthers lights light only their own actor
// and are left out; DontLightActors lights are taken (smoke is air, not an actor).
void SmokeVolume::GatherLights(FLevelLocals* Level, const DVector3& eye, const SmokeVolumeFrame& frame, SmokeLightFrame& light)
{
	mLights.clear();
	light.Lights = nullptr;
	light.LightCount = 0;

	double scatter = Level->SmokeLook.Scatter;
	if (!(scatter > 0.0) || !r_dynlights || Level->lights == nullptr)
		return;		// no dynamic light shows in this smoke
	scatter = std::min(scatter, 1.0);

	const double cell = frame.Grid.CellSize;
	const double boxMin[3] = { frame.OriginCell[0] * cell, frame.OriginCell[1] * cell, frame.OriginCell[2] * cell };
	const double boxMax[3] = { boxMin[0] + frame.Grid.SizeX * cell, boxMin[1] + frame.Grid.SizeY * cell, boxMin[2] + frame.Grid.SizeZ * cell };

	LightCandidates.clear();
	for (FDynamicLight* dl = Level->lights; dl != nullptr; dl = dl->next)
	{
		if (!dl->IsActive() || !dl->visibletoplayer || dl->DontLightOthers())
			continue;
		const double radius = dl->GetRadius();
		if (!(radius > 0.0))
			continue;

		const double at[3] = { dl->Pos.X, dl->Pos.Y, dl->Pos.Z };
		double outsideSquared = 0.0;
		for (int axis = 0; axis < 3; axis++)
		{
			const double d = at[axis] < boxMin[axis] ? boxMin[axis] - at[axis] : (at[axis] > boxMax[axis] ? at[axis] - boxMax[axis] : 0.0);
			outsideSquared += d * d;
		}
		if (!(outsideSquared < radius * radius))
			continue;		// its sphere misses the box (and a light at no finite position)

		float lr = dl->GetRed() / 255.f;
		float lg = dl->GetGreen() / 255.f;
		float lb = dl->GetBlue() / 255.f;
		if (dl->target && (dl->target->renderflags2 & RF2_LIGHTMULTALPHA))
		{
			const float alpha = (float)dl->target->Alpha;
			lr *= alpha;
			lg *= alpha;
			lb *= alpha;
		}
		const float intensity = (float)dl->GetLightDefIntensity();
		lr *= intensity;
		lg *= intensity;
		lb *= intensity;
		if (dl->IsSubtractive())
		{
			const float bright = sqrtf(lr * lr + lg * lg + lb * lb);
			lr = (bright - lr) * -1;
			lg = (bright - lg) * -1;
			lb = (bright - lb) * -1;
		}
		if (lr == 0.f && lg == 0.f && lb == 0.f)
			continue;

		LightCandidate candidate;
		candidate.Distance = std::max((dl->Pos - eye).Length() - radius, 0.0);
		candidate.Light = dl;
		candidate.Color[0] = lr;
		candidate.Color[1] = lg;
		candidate.Color[2] = lb;
		LightCandidates.push_back(candidate);
	}

	size_t count = LightCandidates.size();
	if (count > (size_t)SMOKE_LIGHTS_MAX)
	{
		std::nth_element(LightCandidates.begin(), LightCandidates.begin() + SMOKE_LIGHTS_MAX, LightCandidates.end(),
			[](const LightCandidate& a, const LightCandidate& b) { return a.Distance < b.Distance; });
		count = (size_t)SMOKE_LIGHTS_MAX;
	}

	// The engine's shadow map is live this frame exactly when surfaces use it (hw_dynlightdata.cpp): its rows and
	// every light's mShadowmapIndex were set at the top of this frame's RenderViewpoint, before the compute hook.
	// [EFFECTLIGHTS] And only while "Light shadows" (gl_light_shadowmap) is on (LIGHTS_20_21_22_PLAN.md 2g): the map pass also
	// runs for effect lights with the switch off, and then a dynamic light has no row, whatever its index says. Tested here
	// rather than trusted to CollectLights' indices, so a dynamic light is blocked in the haze exactly while it is blocked on
	// surfaces; with the switch on this is the test it was.
	const bool shadowed = gl_light_shadowmap && screen != nullptr && screen->mShadowMap.Enabled();

	mLights.resize(count);
	for (size_t i = 0; i < count; i++)
	{
		const LightCandidate& candidate = LightCandidates[i];
		const FDynamicLight* dl = candidate.Light;
		SmokeLightRecord& record = mLights[i];

		record.Position[0] = (float)(dl->Pos.X - boxMin[0]);
		record.Position[1] = (float)(dl->Pos.Y - boxMin[1]);
		record.Position[2] = (float)(dl->Pos.Z - boxMin[2]);
		record.Radius = (float)dl->GetRadius();
		for (int c = 0; c < 3; c++)
			record.Color[c] = (float)(candidate.Color[c] * scatter);
		record.Weight = std::max(0.2126f * record.Color[0] + 0.7152f * record.Color[1] + 0.0722f * record.Color[2], 0.0f);

		if (dl->IsSpot())
		{
			// hw_dynlightdata.cpp's GetSpotlightShaderParams, uncached, as stage 2d: shader axes (x, up, y) put back
			// into Doom's. A cone whose inner and outer angles are equal would make smoothstep's edges equal.
			float cosInner = (float)dl->pSpotInnerAngle->Cos();
			float cosOuter = (float)dl->pSpotOuterAngle->Cos();
			if (!(cosOuter < cosInner))
				cosOuter = cosInner - 1e-4f;
			const DAngle negPitch = -dl->Pitch;
			const DAngle angle = dl->Yaw;
			const double xzLength = negPitch.Cos();
			record.SpotDirection[0] = float(-angle.Cos() * xzLength);
			record.SpotDirection[1] = float(-angle.Sin() * xzLength);
			record.SpotDirection[2] = float(-negPitch.Sin());
			record.SpotCosOuter = cosOuter;
			record.SpotCosInner = cosInner;
		}

		record.ShadowRow = (shadowed && dl->mShadowmapIndex >= 0 && dl->mShadowmapIndex < 1024) ? dl->mShadowmapIndex : -1;
	}

	light.Lights = mLights.empty() ? nullptr : mLights.data();
	light.LightCount = (int)mLights.size();
}

//-----------------------------------------------------------------------------
//
// [EFFECTLIGHTS] LD: effect lights in the smoke ("Engine docs/EFFECT_LIGHTS_LD_IMPL_NOTES.md"; hw_framecompute.h,
// SmokeEffectLightPass)
//
//-----------------------------------------------------------------------------

// Whether any effect light this frame binned reaches the light grid, and the light cells they can reach: the union of each
// light's box -- its segment's box grown by its radius -- as the cells whose centre lies in it, clipped to the grid (pass 1's
// rule for a sphere's box, so no cell a light reaches is left out). Only lights the smoke takes: not EFFECT_LIGHT_GPU_NOSMOKE,
// and with light (a colourless drawn-line light adds nothing). The records are this frame's upload as the CPU built it
// (EffectLights::PrepareFrame runs before this in PrepareFrameCompute), used only while the GPU holds them (GetLiveCount): a
// frame whose upload was empty -- effect lights off, nothing binned, no Vulkan buffer -- has no pass 2. Read-only.
void SmokeVolume::GatherEffectLights(FLevelLocals* Level, const SmokeVolumeFrame& frame, SmokeLightFrame& light)
{
	SmokeEffectLightPass& pass = light.EffectLights;
	pass = SmokeEffectLightPass();

	double scatter = Level->SmokeLook.Scatter;
	if (!(scatter > 0.0))
		return;		// no light shows in this smoke (GatherLights' rule)
	scatter = std::min(scatter, 1.0);

	const EffectLightBuffer* buffer = EffectLightBuffer::Instance();
	if (buffer == nullptr || buffer->GetLiveCount() == 0)
		return;
	const EffectLightCore::BinResult& bins = EffectLights::Get().FrameBins();
	const EffectLightGridHeader& header = bins.Grid;
	if (bins.Records.size() != (size_t)buffer->GetLiveCount() || header.Size[0] <= 0 || header.Size[1] <= 0 || header.Size[2] <= 0 ||
		!(header.Corner[3] > 0.f) || (size_t)header.Size[3] != bins.Bins.size())
		return;		// not the frame the GPU holds

	const SmokeLightGridSpec& grid = light.Grid;
	const int size[3] = { grid.SizeX, grid.SizeY, grid.SizeZ };
	const double cellSize = grid.CellSize;
	if (!(cellSize > 0.0) || size[0] <= 0 || size[1] <= 0 || size[2] <= 0)
		return;
	// The light grid covers the smoke box exactly, from its minimum corner (Doom axes, whole map units).
	const double corner[3] = { (double)frame.OriginCell[0] * frame.Grid.CellSize, (double)frame.OriginCell[1] * frame.Grid.CellSize,
		(double)frame.OriginCell[2] * frame.Grid.CellSize };

	int lo[3] = { INT_MAX, INT_MAX, INT_MAX }, hi[3] = { INT_MIN, INT_MIN, INT_MIN };
	int count = 0;
	for (const EffectLightRecord& record : bins.Records)
	{
		const float flags = record.b[3];
		if (!std::isfinite(flags) || (((int)flags) & EFFECT_LIGHT_GPU_NOSMOKE))
			continue;
		const double radius = record.a[3];
		if (!(radius > 0.0) || !std::isfinite(radius) || !(EffectLightCore::Luminance(record.color) > 0.0))
			continue;

		// Records are in shader axes (x, up, game y); the grid is in Doom's.
		const double a[3] = { record.a[0], record.a[2], record.a[1] };
		const double b[3] = { record.b[0], record.b[2], record.b[1] };
		int cellLo[3] = { 0, 0, 0 }, cellHi[3] = { 0, 0, 0 };
		bool reaches = true;
		for (int axis = 0; axis < 3 && reaches; axis++)
		{
			const double low = (std::min(a[axis], b[axis]) - radius - corner[axis]) / cellSize - 0.5;
			const double high = (std::max(a[axis], b[axis]) + radius - corner[axis]) / cellSize - 0.5;
			if (!std::isfinite(low) || !std::isfinite(high))
			{
				reaches = false;
				break;
			}
			cellLo[axis] = std::clamp((int)std::ceil(std::clamp(low, -1.0e6, 1.0e6)), 0, size[axis]);
			cellHi[axis] = std::clamp((int)std::floor(std::clamp(high, -1.0e6, 1.0e6)) + 1, 0, size[axis]);
			reaches = cellLo[axis] < cellHi[axis];
		}
		if (!reaches)
			continue;		// its box misses every light cell's centre
		for (int axis = 0; axis < 3; axis++)
		{
			lo[axis] = std::min(lo[axis], cellLo[axis]);
			hi[axis] = std::max(hi[axis], cellHi[axis]);
		}
		count++;
	}
	if (count == 0)
		return;

	// The effect-light grid's corner in Doom axes. Both corners are whole multiples of their cell and bin sizes, so the offset
	// is a whole number of map units, exact in a float: the shader finds a light cell's bin from the cell's own centre.
	const double binCorner[3] = { header.Corner[0], header.Corner[2], header.Corner[1] };
	pass.LightCount = count;
	for (int axis = 0; axis < 3; axis++)
	{
		pass.RegionMin[axis] = lo[axis];
		pass.RegionMax[axis] = hi[axis];
		pass.BinOffset[axis] = (float)(corner[axis] - binCorner[axis]);
	}
	pass.Scatter = (float)scatter;
}

//-----------------------------------------------------------------------------
//
// [13F] Surface light in the smoke: glow lanes, sweep bands, darkness and the passed look in the ambient pass ("Engine docs/
// SMOKE_13F_IMPL_NOTES.md"; hw_framecompute.h, SmokeSurfaceLightFrame; smoke_light.comp, SurfaceAmbient)
//
//-----------------------------------------------------------------------------

namespace
{
	// [13F] SURFACE LIGHT, PURE HELPERS -- numbers in, numbers out (the mirror compiles this block as it stands).

	const double SURFACE_TAU = 6.283185307179586;

	// The word hw_drawinfo.cpp's RenderScene leaves in a band's uSweepBands[i].w: SetSweepBand writes 1 (add), and SetSweepBandDraw
	// (hw_renderstate.h: a draw or fill outside 0..4 is sent as 0, passed as 0 / 1) writes over it only when the band draws, fills
	// or grades -- a draw of 0 then sent as 1.
	int SurfaceBandWord(int draw, int fill, int passed)
	{
		if (!(draw > 0 || fill > 0 || passed > 0))
			return 1;
		int drawmode = draw > 0 ? draw : 1;
		if (drawmode < 0 || drawmode > 4)
			drawmode = 0;
		if (fill < 0 || fill > 4)
			fill = 0;
		passed = passed != 0 ? 1 : 0;
		return drawmode + 16 * fill + 256 * passed;
	}

	// main.fp's DarknessAt up to its curve, min light and post-gain, in the shader's float arithmetic, for a sector's light level
	// as the default (software) light modes upload it (uLightLevel = level / 255): the share of the room's light that survives,
	// 0..1 -- or -1 where DarknessAt returns 1 before its distance and height terms (mode off, a black sector) or the arithmetic is
	// not finite. G4.
	float SurfaceDarknessCurve(int mode, float adjust, float minLight, float preGain, float postGain, int lightlevel)
	{
		if (mode <= 0)
			return -1.f;
		const float lightLevel = (float)std::clamp(lightlevel, 0, 255) / 255.f;
		const float base = lightLevel * 255.0f;
		if (base <= 0.0f)
			return -1.f;

		const float A = adjust;
		const float L = std::max(base + preGain, 0.0f);
		float outL;
		if (mode == 1)
			outL = L - A;
		else if (mode == 2)
			outL = L * (1.0f - A / 256.0f);
		else if (mode == 3)
			outL = std::min(L, 256.0f - A);
		else if (A <= 0.0f)
			outL = L;
		else
			outL = (256.0f - std::pow(A, A / 256.0f)) * std::pow(L / 256.0f, 1.0f + (A / (33.0f - (A / 8.0f))));
		outL = std::max(outL, minLight);
		outL += postGain;
		const float mul = std::clamp(outL / base, 0.0f, 1.0f);
		return std::isfinite(mul) ? mul : -1.f;
	}

	// Trap 3: the glow's animated terms run on each draw's own material timer, which the CPU cannot follow, so the smoke takes
	// their means over time -- a steady glow at the level the surfaces swing about. GlowWaveRaw's mean: 2 E[pow(w, sharp)] - 1,
	// w main.fp's (detuned) swell, over one period of each sine at 64 midpoints (the detune's ratio is irrational, so its phase is
	// independent of the first's).
	double SurfaceWaveMean(double sharpness, double detune)
	{
		const int N = 64;
		const double sharp = std::max(sharpness, 0.001);
		double sum = 0.0;
		for (int i = 0; i < N; i++)
		{
			const double w1 = 0.5 + 0.5 * std::sin(SURFACE_TAU * (i + 0.5) / N);
			if (!(detune > 0.0))
			{
				sum += N * std::pow(std::clamp(w1, 0.0, 1.0), sharp);
				continue;
			}
			for (int j = 0; j < N; j++)
			{
				const double w2 = 0.5 + 0.5 * std::sin(SURFACE_TAU * (j + 0.5) / N);
				const double w = w1 + (w1 * w2 * 2.0 - w1) * detune;
				sum += std::pow(std::clamp(w, 0.0, 1.0), sharp);
			}
		}
		return 2.0 * sum / ((double)N * N) - 1.0;
	}

	// The glow flow's mean band: E[pow(0.5 + 0.5 sin, sharp)] over a period (256 midpoints).
	double SurfaceFlowMean(double sharpness)
	{
		const int N = 256;
		const double sharp = std::max(sharpness, 0.001);
		double sum = 0.0;
		for (int i = 0; i < N; i++)
			sum += std::pow(std::clamp(0.5 + 0.5 * std::sin(SURFACE_TAU * (i + 0.5) / N), 0.0, 1.0), sharp);
		return sum / N;
	}

	// main.fp's GITDHash21, in double.
	double SurfaceHash21(double x, double y)
	{
		double qx = x * 0.1031, qy = y * 0.1031, qz = x * 0.1031;
		qx -= std::floor(qx);
		qy -= std::floor(qy);
		qz -= std::floor(qz);
		const double d = qx * (qy + 33.33) + qy * (qz + 33.33) + qz * (qx + 33.33);
		qx += d;
		qy += d;
		qz += d;
		const double h = (qx + qy) * qz;
		return h - std::floor(h);
	}

	// The glow cells' mean vein: 1 - smoothstep(0, width, F2 - F1) over main.fp's jittered cells (32 x 32 samples over 4 x 4
	// cells), times the mean of each cell's own pulse, 0.45 + 0.55 x 0.5.
	double SurfaceCellMean(double width)
	{
		const int N = 32;
		const double span = 4.0;
		const double w = std::max(width, 0.01);
		double sum = 0.0;
		for (int j = 0; j < N; j++)
		{
			for (int i = 0; i < N; i++)
			{
				const double u = (i + 0.5) / N * span, v = (j + 0.5) / N * span;
				const double cellX = std::floor(u), cellY = std::floor(v);
				double d1 = 8.0, d2 = 8.0;
				for (int gy = -1; gy <= 1; gy++)
				{
					for (int gx = -1; gx <= 1; gx++)
					{
						const double idX = cellX + gx, idY = cellY + gy;
						const double sx = gx + SurfaceHash21(idX, idY) - (u - cellX);
						const double sy = gy + SurfaceHash21(idX + 37.7, idY + 37.7) - (v - cellY);
						const double d = std::sqrt(sx * sx + sy * sy);
						if (d < d1)
						{
							d2 = d1;
							d1 = d;
						}
						else if (d < d2)
						{
							d2 = d;
						}
					}
				}
				const double t = std::clamp((d2 - d1) / w, 0.0, 1.0);
				sum += 1.0 - t * t * (3.0 - 2.0 * t);
			}
		}
		return sum / ((double)N * N) * 0.725;
	}

	// The mean over a pattern period of main.fp's SweepLineAxisAA in the air, where no screen antialias widens it (its 0.0001
	// floor): lit within `width` of a line, a smoothstep edge `soft` wide (a negative softness taken as 0), lines `spacing` apart.
	double SurfaceLineMean(double spacing, double width, double soft)
	{
		if (!(spacing > 0.0))
			return 0.0;
		const double half = spacing * 0.5;
		const double edge = std::max(soft, 0.0) + 0.0001;
		const auto integral = [](double t) { return t - (t * t * t - 0.5 * t * t * t * t); };	// of 1 - smoothstep from 0 to t
		const double lit = std::max(0.0, std::min(half, width));
		const double t0 = std::clamp((0.0 - width) / edge, 0.0, 1.0);
		const double t1 = std::clamp((half - width) / edge, 0.0, 1.0);
		return std::clamp((lit + edge * (integral(t1) - integral(t0))) / half, 0.0, 1.0);
	}

	// One fill axis's mean: majors (one line in `major` about `boost` times as wide) and flicker's share of lines out.
	double SurfaceAxisMean(double spacing, double width, double soft, double major, double boost, double flicker)
	{
		double mean = SurfaceLineMean(spacing, width, soft);
		if (major >= 2.0)
			mean += (SurfaceLineMean(spacing, width * std::max(boost, 1.0), soft) - mean) / major;
		if (flicker > 0.0)
			mean *= 1.0 - std::min(flicker, 1.0);
		return mean;
	}

	// SweepFillAt's mean coverage for a fill: 1 grid (the max of the two axes: their union), 2 dots (the min: both), 3 solid,
	// 4 pickets (the U axis alone). Rotation, drift and jitter move lines without changing their share; the gradient is not taken.
	double SurfaceFillMean(int fill, double spacingU, double spacingV, double width, double soft, double major, double boost, double flicker)
	{
		if (fill == 3)
			return 1.0;
		const double u = SurfaceAxisMean(spacingU, width, soft, major, boost, flicker);
		if (fill == 4)
			return u;
		const double v = SurfaceAxisMean(spacingV, width, soft, major, boost, flicker);
		return fill == 2 ? u * v : u + v - u * v;
	}

	// [13F] SURFACE LIGHT, ENGINE READS.

	enum
	{
		SURFACE_LANE_FLOOR_WALL = 1,
		SURFACE_LANE_CEILING_WALL = 2,
		SURFACE_LANE_FLOOR_FLAT = 4,
		SURFACE_LANE_CEILING_FLAT = 8,
	};

	// One sector's glow lanes as smoke_light.comp's SurfaceWallLane / SurfaceFlatLane read them, SMOKE_SURFACE_GLOW_VEC4S vec4s
	// appended to out (the floor and ceiling planes as z = a x + b y + c from the grid's corner, w the plane's longest lane reach; then the floor wall lane, the
	// ceiling wall lane, the floor flat lane and the ceiling flat lane, three vec4s each: near colour + reach, far colour + set,
	// falloff + intensity + present). Returns which lanes glow (SURFACE_LANE_*), nothing appended when none does.
	//   - The WALL lanes exactly as a wall of this sector takes them: sector_t::GetWallGlow (trap 1 -- a GlowColor of 0 is the
	//     texture's GLDEFS glow, ~0 is none, an explicit colour glows only with a height), hw_walls.cpp's GetWallGlowFar and
	//     SetGlowFalloffIntensity (an intensity of 0 read as 1). A side's own glow belongs to that wall, not to the room's air.
	//   - The FLAT lanes as hw_flats.cpp uploads them: colour alpha and reach above 0, a sector with lines, intensity (0 read as 1)
	//     folded into both colours.
	//   - Reach takes the glow wave's mean scale, as main.fp's reach takes the wave.
	int AppendSurfaceGlow(sector_t* sec, const double corner[3], float reachScale, std::vector<float>& out)
	{
		float top[4] = { 0.f, 0.f, 0.f, 0.f };
		float bottom[4] = { 0.f, 0.f, 0.f, 0.f };
		const bool wall = sec->GetWallGlow(top, bottom);
		const bool hasLines = sec->Lines.Size() > 0;
		int lanes = 0;
		if (wall && bottom[3] > 0.f)
			lanes |= SURFACE_LANE_FLOOR_WALL;
		if (wall && top[3] > 0.f)
			lanes |= SURFACE_LANE_CEILING_WALL;
		if (hasLines && sec->planes[sector_t::floor].FlatGlowColor.a > 0 && sec->planes[sector_t::floor].FlatGlowHeight > 0.f)
			lanes |= SURFACE_LANE_FLOOR_FLAT;
		if (hasLines && sec->planes[sector_t::ceiling].FlatGlowColor.a > 0 && sec->planes[sector_t::ceiling].FlatGlowHeight > 0.f)
			lanes |= SURFACE_LANE_CEILING_FLAT;
		if (lanes == 0)
			return 0;

		const auto push = [&](float x, float y, float z, float w)
		{
			out.push_back(x);
			out.push_back(y);
			out.push_back(z);
			out.push_back(w);
		};

		// Each plane's longest lane reach -- the very float products the lanes store below -- so the lump skips a plane's two lanes
		// wherever the air is further from it than that: exact, as each lane's own reach test would give 0 there.
		float floorReach = 0.f, ceilingReach = 0.f;
		if (lanes & SURFACE_LANE_FLOOR_WALL)
			floorReach = std::max(floorReach, bottom[3] * reachScale);
		if (lanes & SURFACE_LANE_FLOOR_FLAT)
			floorReach = std::max(floorReach, sec->planes[sector_t::floor].FlatGlowHeight * reachScale);
		if (lanes & SURFACE_LANE_CEILING_WALL)
			ceilingReach = std::max(ceilingReach, top[3] * reachScale);
		if (lanes & SURFACE_LANE_CEILING_FLAT)
			ceilingReach = std::max(ceilingReach, sec->planes[sector_t::ceiling].FlatGlowHeight * reachScale);

		// secplane_t::ZatPoint as a x + b y + c over positions from the grid's corner; w that plane's longest reach.
		for (const secplane_t* plane : { &sec->floorplane, &sec->ceilingplane })
		{
			const DVector3& normal = plane->Normal();
			const float reach = plane == &sec->floorplane ? floorReach : ceilingReach;
			push((float)(normal.X * plane->negiC), (float)(normal.Y * plane->negiC), (float)(plane->ZatPoint(corner[0], corner[1]) - corner[2]), reach);
		}

		const auto wallLane = [&](int lane, const float* glow, int pos)
		{
			if (!(lanes & lane))
			{
				push(0.f, 0.f, 0.f, 0.f);
				push(0.f, 0.f, 0.f, 0.f);
				push(0.f, 0.f, 0.f, 0.f);
				return;
			}
			push(glow[0], glow[1], glow[2], glow[3] * reachScale);
			const PalEntry farColour = sec->GetGlowColorFar(pos);
			if (farColour.a > 0)
				push(farColour.r / 255.f, farColour.g / 255.f, farColour.b / 255.f, 1.f);
			else
				push(0.f, 0.f, 0.f, 0.f);
			const float intensity = sec->GetGlowIntensity(pos) > 0.f ? sec->GetGlowIntensity(pos) : 1.0f;
			push((float)sec->GetGlowFalloff(pos), intensity, 1.f, 0.f);
		};
		wallLane(SURFACE_LANE_FLOOR_WALL, bottom, sector_t::floor);
		wallLane(SURFACE_LANE_CEILING_WALL, top, sector_t::ceiling);

		const auto flatLane = [&](int lane, int pos)
		{
			if (!(lanes & lane))
			{
				push(0.f, 0.f, 0.f, 0.f);
				push(0.f, 0.f, 0.f, 0.f);
				push(0.f, 0.f, 0.f, 0.f);
				return;
			}
			const auto& plane = sec->planes[pos];
			const float inten = plane.FlatGlowIntensity > 0.f ? plane.FlatGlowIntensity : 1.f;
			push(plane.FlatGlowColor.r / 255.f * inten, plane.FlatGlowColor.g / 255.f * inten, plane.FlatGlowColor.b / 255.f * inten, plane.FlatGlowHeight * reachScale);
			const PalEntry farColour = plane.FlatGlowColorFar;
			if (farColour.a > 0)
				push(farColour.r / 255.f * inten, farColour.g / 255.f * inten, farColour.b / 255.f * inten, 1.f);
			else
				push(0.f, 0.f, 0.f, 0.f);
			push((float)plane.FlatGlowFalloff, 1.f, 1.f, 0.f);
		};
		flatLane(SURFACE_LANE_FLOOR_FLAT, sector_t::floor);
		flatLane(SURFACE_LANE_CEILING_FLAT, sector_t::ceiling);
		return lanes;
	}

	// The distance main.fp's flat-edge glow measures at (x, y): to the nearest of the lines hw_flats.cpp uploads -- the sector's
	// first 64, so a larger sector's air glows from the same truncated list its flat does -- in the shader's own arithmetic.
	// -1 for a sector with no lines.
	double SurfaceEdgeDistance(sector_t* sec, double x, double y)
	{
		const int count = std::min((int)sec->Lines.Size(), 64);
		if (count <= 0)
			return -1.0;
		double best = 999999.0 * 999999.0;
		for (int i = 0; i < count; i++)
		{
			const line_t* line = sec->Lines[i];
			const double ax = line->v1->fX(), ay = line->v1->fY();
			const double abx = line->v2->fX() - ax, aby = line->v2->fY() - ay;
			const double apx = x - ax, apy = y - ay;
			const double t = std::clamp((apx * abx + apy * aby) / std::max(abx * abx + aby * aby, 0.001), 0.0, 1.0);
			const double dx = apx - abx * t, dy = apy - aby * t;
			best = std::min(best, dx * dx + dy * dy);
		}
		return std::sqrt(best);
	}
}

// The surface light for the ambient pass (hw_framecompute.h, SmokeSurfaceLightFrame): whether pass 0 takes its surface light
// variant this frame, the records it reads and each column's record and flat-edge distance. Live only when something would change
// the light -- a listed sector glows, a band gives light, the darkness curve is on, a passed look is live -- so any other frame
// runs pass 0 exactly as before. Read-only: GetWallGlow and the glow accessors only read the level.
void SmokeVolume::UpdateSurfaceLight(FLevelLocals* Level, const DVector3& eye, const SmokeVolumeFrame& frame, SmokeLightFrame& light)
{
	SmokeSurfaceLightFrame& surface = light.Surface;
	surface = SmokeSurfaceLightFrame();

	const bool glowSwitch = r_smoke_surfaceglow;
	const bool darkSwitch = r_smoke_darkness;
	const SmokeLightGridSpec& grid = light.Grid;
	const size_t columns = (size_t)std::max(grid.SizeX, 0) * (size_t)std::max(grid.SizeY, 0);
	if ((!glowSwitch && !darkSwitch) || columns == 0 || !(grid.CellSize > 0.0) || mColumnSector.size() != columns ||
		mAmbientSize[0] != grid.SizeX || mAmbientSize[1] != grid.SizeY || mAmbientCellSize != grid.CellSize)
		return;

	// The sweep bands as the scene uploads them (hw_drawinfo.cpp, RenderScene): the count under its gate, each band's shape its
	// own or -- trap 2, a band shape of 0 is not off -- the shared one, and the word it leaves. A band with no intensity adds
	// nothing, lifts and crushes by 1 and recolours by 0, so it gives no light.
	const int bandCount = (Level->SweepMode > 0 && Level->SweepCount > 0) ? std::min(Level->SweepCount, (int)FLevelLocals::MAX_SWEEP_BANDS) : 0;
	int bandShape[SMOKE_SURFACE_BANDS] = {};
	int bandWord[SMOKE_SURFACE_BANDS] = {};
	bool bandLight = false, bandRecolour = false, bandPassed = false;
	for (int i = 0; i < bandCount; i++)
	{
		bandShape[i] = Level->SweepBandMode[i] > 0 ? Level->SweepBandMode[i] : Level->SweepMode;
		bandWord[i] = SurfaceBandWord(Level->SweepBandDraw[i], Level->SweepBandFill[i], Level->SweepBandPassed[i]);
		const int mode = bandWord[i] & 15;
		if (((bandWord[i] >> 8) & 1) && bandShape[i] > 0)
			bandPassed = true;	// [13F] 13f2: SweepPassedAt skips a slot with no shape
		if (bandShape[i] > 0 && bandWord[i] > 0 && Level->SweepIntensity[i] != 0.0)
		{
			if (mode >= 1 && mode <= 3)
				bandLight = true;
			else if (mode == 4)
				bandRecolour = true;
		}
	}
	// hw_drawinfo.cpp's mSweepPassedColor.w: a band grades its passed side and some term of the look changes the light.
	const bool passedLook = Level->SweepPassedTintMix > 0 || Level->SweepPassedDarken > 0 || Level->SweepPassedDesat > 0;
	const bool passedLive = darkSwitch && bandCount > 0 && bandPassed && passedLook;
	const bool darkLive = darkSwitch && Level->DarkMode > 0;
	// [13F] 13f2: THE LIVE SLOTS. RS_Sweeps (and any caller sharing the slots) sets SweepCount once and releases a slot by
	// zeroing it -- intensity 0, shape 0, draw / fill / passed 0 -- so the count says nothing about what still draws. The
	// records carry only the slots that can change a cell: a shape and a word (SweepBandAttenAt's own gates) and either an
	// intensity or a passed bit the live passed look reads. In slot order, so the strongest-recolour tie is main.fp's.
	int liveSlot[SMOKE_SURFACE_BANDS] = {};
	int liveCount = 0;
	for (int i = 0; i < bandCount; i++)
	{
		if (bandShape[i] > 0 && bandWord[i] > 0 && (Level->SweepIntensity[i] != 0.0 || (passedLive && ((bandWord[i] >> 8) & 1))))
			liveSlot[liveCount++] = i;
	}
	const int lightBands = (glowSwitch && (bandLight || bandRecolour)) ? liveCount : 0;
	if (!glowSwitch && !darkLive && !passedLive)
		return;

	// Trap 3: the glow's animated terms as their means over time, cached by their inputs.
	float reachScale = 1.f, brightTexture = 1.f, colourShift = 0.f;
	if (glowSwitch)
	{
		double wave = 0.0;
		if (Level->GlowWaveLength > 0.0)
		{
			if (!mSurfaceWaveValid || mSurfaceWaveInputs[0] != Level->GlowWaveSharp || mSurfaceWaveInputs[1] != Level->GlowWaveDetune)
			{
				mSurfaceWaveMean = SurfaceWaveMean(Level->GlowWaveSharp, Level->GlowWaveDetune);
				mSurfaceWaveInputs[0] = Level->GlowWaveSharp;
				mSurfaceWaveInputs[1] = Level->GlowWaveDetune;
				mSurfaceWaveValid = true;
			}
			wave = mSurfaceWaveMean;
		}
		// GlowTextureAt's mean: the noise's is 1 (0.25 + 1.5 x its mean of 0.5); the disturbance rings pass and the alarm pulse
		// swings about 1, so theirs are 1; the flow's and the cells' follow their shapes.
		double texture = 1.0;
		if (Level->GlowFlow > 0.0)
		{
			if (!mSurfaceFlowValid || mSurfaceFlowInput != Level->GlowFlowSharp)
			{
				mSurfaceFlowMean = SurfaceFlowMean(Level->GlowFlowSharp);
				mSurfaceFlowInput = Level->GlowFlowSharp;
				mSurfaceFlowValid = true;
			}
			texture *= 1.0 + (0.3 + 1.4 * mSurfaceFlowMean - 1.0) * std::clamp(Level->GlowFlow, 0.0, 1.0);
		}
		if (Level->GlowCell > 0.0)
		{
			if (!mSurfaceCellValid || mSurfaceCellInput != Level->GlowCellWidth)
			{
				mSurfaceCellMean = SurfaceCellMean(Level->GlowCellWidth);
				mSurfaceCellInput = Level->GlowCellWidth;
				mSurfaceCellValid = true;
			}
			texture *= 1.0 + (0.35 + 1.9 * mSurfaceCellMean - 1.0) * std::clamp(Level->GlowCell, 0.0, 1.0);
		}
		reachScale = (float)(1.0 + Level->GlowWaveReach * wave);
		colourShift = (float)(Level->GlowWaveColour * wave);
		brightTexture = (float)((1.0 + Level->GlowWaveBright * wave) * std::max(texture, 0.0));
	}

	// Records: one per sector a column has resolved to -- its place in 13d's mAmbientSectors, so a column's record changes only
	// when its sector does -- then the eye's sector, for the columns not resolved yet (13d's fallback).
	const unsigned sectorCount = Level->sectors.Size();
	bool reset = mSurfaceLevelSerial != mAmbientLevelSerial || mSurfaceSlot.size() != sectorCount || mSurfaceSize[0] != grid.SizeX ||
		mSurfaceSize[1] != grid.SizeY || mSurfaceCellSize != grid.CellSize || mSurfaceSlotsKnown > mAmbientSectors.size();
	for (size_t k = 0; k < mSurfaceSlotsKnown && !reset; k++)
		reset = (unsigned)mAmbientSectors[k] >= sectorCount || mSurfaceSlot[mAmbientSectors[k]] != (int)k;
	if (reset)
	{
		mSurfaceLevelSerial = mAmbientLevelSerial;
		mSurfaceSize[0] = grid.SizeX;
		mSurfaceSize[1] = grid.SizeY;
		mSurfaceCellSize = grid.CellSize;
		mSurfaceOrigin[0] = mAmbientOrigin[0];
		mSurfaceOrigin[1] = mAmbientOrigin[1];
		mSurfaceSlot.assign(sectorCount, -1);
		mSurfaceSlotsKnown = 0;
		mSurfaceColumnSector.assign(columns, INT_MIN);	// no column's: every column is compared as changed
		mSurfaceEdge.assign(columns, -1.f);
		mSurfaceFlatBefore.clear();
		mSurfaceAmbientSerial = 0;
		mSurfaceColumnsDirty = true;
		mSurfaceEdgesPending = true;
	}
	for (; mSurfaceSlotsKnown < mAmbientSectors.size(); mSurfaceSlotsKnown++)
	{
		const int index = mAmbientSectors[mSurfaceSlotsKnown];
		if ((unsigned)index < sectorCount)
			mSurfaceSlot[index] = (int)mSurfaceSlotsKnown;
	}

	const double cell = frame.Grid.CellSize;
	const double corner[3] = { frame.OriginCell[0] * cell, frame.OriginCell[1] * cell, frame.OriginCell[2] * cell };
	const size_t first = (size_t)SMOKE_SURFACE_HEADER_VEC4S;
	const size_t listed = mAmbientSectors.size();
	mSurfaceRecords.assign((first + listed + 1) * 4, 0.f);
	mSurfaceGlow.clear();
	mSurfaceFlat.assign(listed + 1, 0);
	bool glowAny = false, flatAny = false;
	const auto sectorRecord = [&](size_t k, sector_t* sec)
	{
		float* record = &mSurfaceRecords[(first + k) * 4];
		record[0] = -1.f;
		record[1] = -1.f;
		if (sec == nullptr)
			return;
		if (darkLive)
			record[0] = SurfaceDarknessCurve(Level->DarkMode, (float)Level->DarkAdjust, (float)Level->DarkMinLight, (float)Level->DarkPreGain,
				(float)Level->DarkPostGain, sec->lightlevel);
		record[2] = sec->Colormap.Desaturation * (1.0f / 255.0f);	// main.fp's uDesaturationFactor for this sector's surfaces
		if (glowSwitch)
		{
			const size_t at = mSurfaceGlow.size() / 4;
			const int lanes = AppendSurfaceGlow(sec, corner, reachScale, mSurfaceGlow);
			if (lanes != 0)
			{
				record[1] = (float)(listed + 1 + at);	// an index into the records after the header (the lump's surfaceRecords[])
				glowAny = true;
				if (lanes & (SURFACE_LANE_FLOOR_FLAT | SURFACE_LANE_CEILING_FLAT))
				{
					mSurfaceFlat[k] = 1;
					flatAny = true;
				}
			}
		}
	};
	for (size_t k = 0; k < listed; k++)
		sectorRecord(k, (unsigned)mAmbientSectors[k] < sectorCount ? &Level->sectors[mAmbientSectors[k]] : nullptr);
	sectorRecord(listed, Level->PointInSector(eye.X, eye.Y));

	if (!(darkLive || passedLive || (glowSwitch && (glowAny || bandLight))))
		return;		// nothing here changes the light: pass 0 as before
	mSurfaceRecords.insert(mSurfaceRecords.end(), mSurfaceGlow.begin(), mSurfaceGlow.end());

	// The header: smoke_light.comp's SmokeSurfaceSSO, in order.
	float* header = mSurfaceRecords.data();
	const auto put = [&](int vec, float x, float y, float z, float w)
	{
		header[vec * 4] = x;
		header[vec * 4 + 1] = y;
		header[vec * 4 + 2] = z;
		header[vec * 4 + 3] = w;
	};
	put(0, glowSwitch ? 1.f : 0.f, (float)lightBands, (float)Level->SweepTrail, (float)listed);	// w: the fallback's record, the last sector record
	put(1, (float)Level->SweepPassedTintMix, (float)Level->SweepPassedDarken, (float)Level->SweepPassedDesat, (float)Level->SweepPassedSoft);
	put(2, Level->SweepPassedTint.r / 255.f, Level->SweepPassedTint.g / 255.f, Level->SweepPassedTint.b / 255.f, passedLive ? (float)liveCount : 0.f);	// [13F] 13f2: live slots
	put(3, (float)Level->DarkDistDepth, (float)Level->DarkDistRange, (float)Level->DarkHeightDepth, (float)Level->DarkHeightRange);
	// The height reference as hw_drawinfo.cpp uploads it: with height follow on, the viewer's feet this frame, interpolated like
	// the view. PrepareFrameCompute's viewpoint is the renderer's own, r_viewpoint.
	double heightRef = Level->DarkHeightRef;
	if (Level->DarkHeightFollow == 1 && r_viewpoint.camera != nullptr)
		heightRef = r_viewpoint.camera->InterpolatedPosition(r_viewpoint.TicFrac).Z + Level->DarkHeightOffset;
	put(4, (float)(eye.X - corner[0]), (float)(eye.Y - corner[1]), (float)(eye.Z - corner[2]), (float)(heightRef - corner[2]));
	put(5, (float)Level->DesatKeep, (float)Level->DesatKeepSoft, (float)Level->DesatKeepHue, (float)Level->DesatGlobal);
	put(6, reachScale, brightTexture, colourShift, 0.f);
	for (int k = 0; k < liveCount; k++)
	{
		const int i = liveSlot[k];	// [13F] 13f2: record k holds the k-th live slot
		const DVector3& origin = Level->SweepBandMode[i] > 0 ? Level->SweepBandOrigin[i] : Level->SweepOrigin;
		put(7 + k, (float)(origin.X - corner[0]), (float)(origin.Y - corner[1]), (float)(origin.Z - corner[2]), (float)bandShape[i]);
		put(15 + k, (float)Level->SweepRadius[i], (float)Level->SweepThickness[i], (float)Level->SweepSoftness[i], (float)bandWord[i]);

		// main.fp's fill block, with the fill's mean coverage: the band's colour mixed toward the lines' by it (inverted by a
		// negative gap), its coverage multiplied by the larger of it and the gap; the solid fill covers fully. A recolour band's
		// colour is its own (main.fp's recolour pass reads no fill).
		const PalEntry colour = Level->SweepColor[i];
		double rgb[3] = { colour.r / 255.0, colour.g / 255.0, colour.b / 255.0 };
		double multiplier = 1.0;
		const int fill = (bandWord[i] >> 4) & 15;
		const int mode = bandWord[i] & 15;
		if (fill > 0 && mode != 4)
		{
			const double coverage = SurfaceFillMean(fill, Level->SweepFillSpacingU, Level->SweepFillSpacingV, Level->SweepFillWidth,
				Level->SweepFillSoft, Level->SweepFillMajor, Level->SweepFillMajorBoost, Level->SweepFillFlicker);
			const double gap = Level->SweepFillGap;
			const PalEntry lines = Level->SweepFillColor;
			const double lineRgb[3] = { lines.r / 255.0, lines.g / 255.0, lines.b / 255.0 };
			for (int c = 0; c < 3; c++)
			{
				const double field = rgb[c] * std::max(gap, 0.0);
				double mixed = field + (lineRgb[c] - field) * coverage;
				if (gap < 0.0)
					mixed = lineRgb[c] + (rgb[c] * -gap - lineRgb[c]) * coverage;
				rgb[c] = mixed;
			}
			multiplier = std::max(coverage, std::max(gap, 0.0));
		}
		put(23 + k, (float)rgb[0], (float)rgb[1], (float)rgb[2], (float)Level->SweepIntensity[i]);
		put(31 + k, (float)multiplier, (fill == 3 && mode != 4) ? 1.f : 0.f, 0.f, 0.f);
	}

	// Columns, world-aligned as 13d's column sectors: a recentre keeps what stays in the box, a column whose sector changed loses
	// its edge distance, and the edge distances of columns whose sector has a flat lane are worked out nearest the eye first
	// within SURFACE_EDGE_BUDGET_MS a frame (a column not worked out yet shows no flat glow).
	const int sizeX = grid.SizeX;
	const int sizeY = grid.SizeY;
	if (mAmbientOrigin[0] != mSurfaceOrigin[0] || mAmbientOrigin[1] != mSurfaceOrigin[1])
	{
		const int dx = mAmbientOrigin[0] - mSurfaceOrigin[0];
		const int dy = mAmbientOrigin[1] - mSurfaceOrigin[1];
		std::vector<int> movedSector(columns, INT_MIN);
		std::vector<float> movedEdge(columns, -1.f);
		for (int y = 0; y < sizeY; y++)
		{
			const int sy = y + dy;
			if (sy < 0 || sy >= sizeY)
				continue;
			for (int x = 0; x < sizeX; x++)
			{
				const int sx = x + dx;
				if (sx < 0 || sx >= sizeX)
					continue;
				movedSector[x + (size_t)y * (size_t)sizeX] = mSurfaceColumnSector[sx + (size_t)sy * (size_t)sizeX];
				movedEdge[x + (size_t)y * (size_t)sizeX] = mSurfaceEdge[sx + (size_t)sy * (size_t)sizeX];
			}
		}
		mSurfaceColumnSector.swap(movedSector);
		mSurfaceEdge.swap(movedEdge);
		mSurfaceOrigin[0] = mAmbientOrigin[0];
		mSurfaceOrigin[1] = mAmbientOrigin[1];
		mSurfaceColumnsDirty = true;
		mSurfaceEdgesPending = true;
	}
	if (mSurfaceAmbientSerial != mAmbientSerial)
	{
		// 13d's serial moves whenever a column changes (and when a sector's light does): compare.
		for (size_t i = 0; i < columns; i++)
		{
			if (mSurfaceColumnSector[i] != mColumnSector[i])
			{
				mSurfaceColumnSector[i] = mColumnSector[i];
				mSurfaceEdge[i] = -1.f;
				mSurfaceColumnsDirty = true;
				mSurfaceEdgesPending = true;
			}
		}
		mSurfaceAmbientSerial = mAmbientSerial;
	}
	if (mSurfaceFlat != mSurfaceFlatBefore)
	{
		mSurfaceFlatBefore = mSurfaceFlat;
		mSurfaceEdgesPending = true;
	}

	if (flatAny && mSurfaceEdgesPending)
	{
		const int B = AMBIENT_BLOCK_COLUMNS;
		const int blocksX = (sizeX + B - 1) / B;
		const int blocksY = (sizeY + B - 1) / B;
		const double lightCell = grid.CellSize;
		struct Block
		{
			int Index;
			double Distance2;
		};
		std::vector<Block> order;
		order.reserve((size_t)blocksX * (size_t)blocksY);
		for (int b = 0; b < blocksX * blocksY; b++)
		{
			const double cx = (mAmbientOrigin[0] + (b % blocksX + 0.5) * B) * lightCell - eye.X;
			const double cy = (mAmbientOrigin[1] + (b / blocksX + 0.5) * B) * lightCell - eye.Y;
			order.push_back({ b, cx * cx + cy * cy });
		}
		std::sort(order.begin(), order.end(), [](const Block& a, const Block& b) { return a.Distance2 < b.Distance2; });

		const uint64_t startNs = I_nsTime();
		bool workedAny = false, stopped = false;
		for (const Block& block : order)
		{
			if (workedAny && (double)(I_nsTime() - startNs) / 1e6 >= SURFACE_EDGE_BUDGET_MS)
			{
				stopped = true;
				break;
			}
			const int bx = block.Index % blocksX;
			const int by = block.Index / blocksX;
			const int x1 = std::min((bx + 1) * B, sizeX);
			const int y1 = std::min((by + 1) * B, sizeY);
			for (int y = by * B; y < y1; y++)
			{
				for (int x = bx * B; x < x1; x++)
				{
					const size_t i = x + (size_t)y * (size_t)sizeX;
					const int index = mColumnSector[i];
					if (index < 0 || (unsigned)index >= sectorCount || mSurfaceEdge[i] >= 0.f)
						continue;
					const int slot = mSurfaceSlot[index];
					if (slot < 0 || !mSurfaceFlat[(size_t)slot])
						continue;
					const double at[2] = { (mAmbientOrigin[0] + x + 0.5) * lightCell, (mAmbientOrigin[1] + y + 0.5) * lightCell };
					const double edge = SurfaceEdgeDistance(&Level->sectors[index], at[0], at[1]);
					if (edge >= 0.0)
					{
						mSurfaceEdge[i] = (float)edge;
						mSurfaceColumnsDirty = true;
					}
					workedAny = true;
				}
			}
		}
		if (!stopped)
			mSurfaceEdgesPending = false;
	}

	if (mSurfaceColumnsDirty || mSurfaceColumns.size() != 4 + columns * 2)
	{
		mSurfaceColumns.assign(4 + columns * 2, -1.f);
		const int32_t size[4] = { sizeX, sizeY, 0, 0 };
		memcpy(mSurfaceColumns.data(), size, sizeof(size));
		for (size_t i = 0; i < columns; i++)
		{
			const int index = mColumnSector[i];
			const int slot = (index >= 0 && (unsigned)index < sectorCount) ? mSurfaceSlot[index] : -1;
			mSurfaceColumns[4 + i * 2] = slot >= 0 ? (float)slot : -1.f;	// an index into the records after the header
			mSurfaceColumns[5 + i * 2] = mSurfaceEdge[i];
		}
		mSurfaceColumnsDirty = false;
		if (++mSurfaceColumnSerial == 0)
			mSurfaceColumnSerial = 1;
	}

	surface.Live = true;
	surface.Columns = mSurfaceColumns.data();
	surface.ColumnFloats = mSurfaceColumns.size();
	surface.ColumnSerial = mSurfaceColumnSerial;
	surface.Records = mSurfaceRecords.data();
	surface.RecordFloats = mSurfaceRecords.size();
}

//-----------------------------------------------------------------------------
//
// [13e] Beams in the smoke ("Engine docs/SMOKE_VOLUME_PLAN.md" 13e; hw_framecompute.h, SmokeBeamFrame)
//
//-----------------------------------------------------------------------------

namespace
{
	struct BeamCandidate
	{
		double Distance2 = 0;	// the eye's squared distance to the segment
		int Slot = 0;
		SmokeBeamRecord Record;
	};
	std::vector<BeamCandidate> BeamCandidates;
}

// Every beam slot that draws with an air glow -- a beam its author made invisible in the air is invisible in smoke too --
// resolved exactly as the scene resolves it (ResolveBeamLine, hw_drawinfo.cpp), whose segment comes within its glow's
// reach of the box; the nearest SMOKE_BEAMS_MAX to the eye, in slot order (the order the scene adds them in). Routed
// drawn beams (r_beams_drawn) are the same slots. Read-only: nothing is written to the level.
void SmokeVolume::GatherBeams(FLevelLocals* Level, const DVector3& eye, double ticFrac, SmokeVolumeFrame& out)
{
	mBeams.clear();
	out.Beams = SmokeBeamFrame();

	const double cell = out.Grid.CellSize;
	const double boxMin[3] = { out.OriginCell[0] * cell, out.OriginCell[1] * cell, out.OriginCell[2] * cell };
	const double boxMax[3] = { boxMin[0] + out.Grid.SizeX * cell, boxMin[1] + out.Grid.SizeY * cell, boxMin[2] + out.Grid.SizeZ * cell };
	const double eyeAt[3] = { eye.X, eye.Y, eye.Z };

	BeamCandidates.clear();
	for (int slot = 0; slot < FLevelLocals::MAX_BEAMS; slot++)
	{
		DVector3 a, b;
		FVector4 look;
		if (!ResolveBeamLine(Level, slot, ticFrac, a, b, look) || !(look.X > 0.f))
			continue;

		// The furthest its glow reaches (main.fp's BeamAirGlow halo), widened by a taper past 1 as drawnlines.fp's bias is.
		const double thick = std::max(Level->BeamThick[slot], 0.01);
		const double soft = std::max(Level->BeamSoft[slot], 0.01);
		const double reach = (thick + soft * 6.0 + 1.0) * std::max(1.0, std::fabs(1.0 - (double)look.Z));
		const double pa[3] = { a.X, a.Y, a.Z };
		const double pb[3] = { b.X, b.Y, b.Z };
		bool reaches = std::isfinite(reach);
		for (int axis = 0; axis < 3 && reaches; axis++)
		{
			if (!std::isfinite(pa[axis]) || !std::isfinite(pb[axis]))
				reaches = false;
			else if (std::max(pa[axis], pb[axis]) < boxMin[axis] - reach || std::min(pa[axis], pb[axis]) > boxMax[axis] + reach)
				reaches = false;
		}
		if (!reaches)
			continue;

		const double ab[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
		const double length2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
		double along = 0.0;
		if (length2 > 1e-12)
			along = std::clamp(((eyeAt[0] - pa[0]) * ab[0] + (eyeAt[1] - pa[1]) * ab[1] + (eyeAt[2] - pa[2]) * ab[2]) / length2, 0.0, 1.0);

		BeamCandidate candidate;
		candidate.Slot = slot;
		for (int axis = 0; axis < 3; axis++)
		{
			const double d = pa[axis] + ab[axis] * along - eyeAt[axis];
			candidate.Distance2 += d * d;
		}
		SmokeBeamRecord& record = candidate.Record;
		// GL axes (map x, map z, map y), from the box's corner, as every beam upload swizzles.
		record.A[0] = (float)(pa[0] - boxMin[0]);
		record.A[1] = (float)(pa[2] - boxMin[2]);
		record.A[2] = (float)(pa[1] - boxMin[1]);
		record.Thick = (float)Level->BeamThick[slot];
		record.B[0] = (float)(pb[0] - boxMin[0]);
		record.B[1] = (float)(pb[2] - boxMin[2]);
		record.B[2] = (float)(pb[1] - boxMin[1]);
		record.Soft = (float)Level->BeamSoft[slot];
		record.Color[0] = Level->BeamColor[slot].r / 255.f;
		record.Color[1] = Level->BeamColor[slot].g / 255.f;
		record.Color[2] = Level->BeamColor[slot].b / 255.f;
		record.Intensity = (float)Level->BeamIntensity[slot];
		record.Look[0] = look.X;
		record.Look[1] = look.Y;
		record.Look[2] = look.Z;
		record.Look[3] = look.W;
		BeamCandidates.push_back(candidate);
	}

	size_t count = BeamCandidates.size();
	if (count > (size_t)SMOKE_BEAMS_MAX)
	{
		std::nth_element(BeamCandidates.begin(), BeamCandidates.begin() + SMOKE_BEAMS_MAX, BeamCandidates.end(),
			[](const BeamCandidate& x, const BeamCandidate& y) { return x.Distance2 < y.Distance2; });
		count = (size_t)SMOKE_BEAMS_MAX;
	}
	std::sort(BeamCandidates.begin(), BeamCandidates.begin() + count,
		[](const BeamCandidate& x, const BeamCandidate& y) { return x.Slot < y.Slot; });

	mBeams.resize(count);
	for (size_t i = 0; i < count; i++)
		mBeams[i] = BeamCandidates[i].Record;
	out.Beams.Beams = mBeams.empty() ? nullptr : mBeams.data();
	out.Beams.Count = (int)mBeams.size();
}
