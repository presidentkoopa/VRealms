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
#include <cmath>

#include "hw_smokevolume.h"
#include "hw_levelsolidity.h"
#include "hw_sectorplanes.h"
#include "hw_framecompute.h"
#include "hw_cvars.h"
#include "hw_perflog.h"
#include "g_levellocals.h"
#include "a_dynlight.h"		// [13d] FDynamicLight, r_dynlights: the lights in the smoke
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
	const bool shadowed = screen != nullptr && screen->mShadowMap.Enabled();

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
