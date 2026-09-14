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
#include "doomdef.h"
#include "v_video.h"
#include "i_time.h"
#include "printf.h"

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

	double sums[SmokeVolumeFrame::MAX_STEPS_PER_FRAME][3] = {};
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
		}
	}
	else if (quietBefore && mResidue)
	{
		// Went quiet: whatever is left is below every empty line. Emptied once, for good.
		out.ClearContents = true;
		mResidue = false;
	}
	out.HasSmoke = mBound.Density > SMOKE_EMPTY_DENSITY;

	// [13c] For the drawing, later this frame (GetDrawState). Every earlier return leaves the reset
	// state of the top of this function: nothing to draw.
	mDraw.HasSmoke = out.HasSmoke;
	mDraw.Quality = out.Quality;
	mDraw.Grid = out.Grid;
	for (int axis = 0; axis < 3; axis++)
		mDraw.OriginCell[axis] = out.OriginCell[axis];
	mDraw.TicFrac = out.TicFrac;

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

void SmokeVolume::BuildKernels(const SmokeVolumeFrame& frame, double sums[][3])
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
