/*
** hw_smokevolume.h
**
** [SMOKEVOLUME] The smoke volume's CPU side: when it exists, where its box is, what
** goes into it this frame, and its solid mask.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SMOKE_VOLUME_PLAN.md" #13 (13a, 13b). Everything here survives a render
** rebuild: it only reads the level and fills SmokeVolumeFrame (hw_framecompute.h); the
** GPU side (vk_smokevolume.cpp) acts on it.
**
**   - DEMAND: the volume exists while r_smoke is on and something asked for smoke on
**     this map within the last LINGER_SECONDS of level time -- an EmitSmoke or
**     CarveSmoke reaching the level's per-tic queues (FEffectTicQueue, g_levellocals.h),
**     or the test source r_smoke_computetest -- or while smoke may still be in the air.
**     Level time freezes in a menu, so pausing never frees and re-makes 150 MB, and the
**     linger carries into the next map. A PushEffectImpulse alone asks for nothing.
**   - STEPS: one per world tic, at most SmokeVolumeFrame::MAX_STEPS_PER_FRAME a frame;
**     a bigger jump drops the backlog (review S7).
**   - EVENTS: every frame the queues are read from this side's own cursors (nothing is
**     written to the level). An event becomes one or more SmokeKernels for the step of
**     the tic it was queued in. An event whose cells are not yet in the solid mask waits
**     (at most MAX_HOLD_TICS) while the tiles under it are rasterised first, so smoke is
**     never injected before the walls around it exist.
**   - PLACEMENT: a world-aligned box of whole TILES (SMOKE_TILE_CELLS), centred on the
**     eye in x and y with the eye at EYE_HEIGHT_FRACTION of its height, recentred when
**     the eye strays more than RECENTRE_FRACTION of an extent. A recentre shifts the
**     contents on the GPU; a jump further than the box starts a new box.
**   - THE SOLID MASK: SH1's LevelSolidity rasterises the box on the CPU in world-aligned
**     MASK_TILE_CELLS x MASK_TILE_CELLS columns, within MASK_BUDGET_MS a frame (plus up to
**     MASK_EVENT_TILES_PER_FRAME tiles events are waiting on), nearest the eye first, and
**     the backend copies each tile in. A new box starts ALL SOLID; a recentre makes what
**     it exposes solid until rasterised; SH2 (SectorPlanes) names the sectors that moved
**     -- a door, a lift -- and their tiles are done again.
**   - QUIET: upper bounds on the density, heat and speed anywhere in the volume, from what
**     went in and how fast it decays. Below SMOKE_EMPTY_* nothing is simulated or drawn,
**     and the volume is emptied once.
**   - THE TEST SOURCE (r_smoke_computetest): a renderer-side source placed ahead of the
**     view when switched on -- a puff a tic, a round through it every 10 tics, a blast
**     every 3 seconds -- through the same path as a mod's events. Local only.
**   - [13d] THE LIGHT GRID (SmokeLightFrame, hw_framecompute.h): its quality
**     (r_smoke_light_quality, defined in hw_smokevolume.cpp), each light-grid COLUMN's
**     sector light -- the sector under the column's centre, cached world-aligned so a
**     recentre keeps what it can, new columns resolved within AMBIENT_BUDGET_MS a frame
**     nearest the eye first -- and, on frames with smoke to draw, up to SMOKE_LIGHTS_MAX
**     dynamic lights whose sphere reaches the box, nearest the eye, coloured as stage 2d's
**     view lights, with their row in the engine's shadow map when that is live.
**   - [13e] SOOT: an EmitSmoke's soot share becomes a kernel's soot density, bounded like the
**     density; SmokeSimSettings::SootLive says whether any may be in the volume. BEAMS: on
**     frames with smoke to draw, the beam lines whose glow may reach the box, nearest the eye
**     (SmokeBeamFrame, hw_framecompute.h), for the smoke drawing's beam scatter and depth.
**
** Main thread only. Presentation only: nothing here writes to the playsim.
**
*/

#pragma once

#include <cstdint>
#include <vector>

#include "vectors.h"
#include "hw_framecompute.h"

struct FLevelLocals;
class SectorPlanes;

// [13e] A beam slot as the per-pixel beam upload and the drawn-line path resolve it (hw_drawinfo.cpp, beside them, so
// the smoke and the scene can never disagree about where a beam is or how it looks): false when the slot does not draw
// (not live, or intensity 0). a and b in game space; look x air glow, y halo, z taper, w flare. viewTicFrac is the
// frame's; r_beam_interpolate applies exactly as it does there. Read-only.
bool ResolveBeamLine(FLevelLocals *Level, int slot, double viewTicFrac, DVector3 &a, DVector3 &b, FVector4 &look);

class SmokeVolume
{
public:
	static constexpr int LINGER_SECONDS = 60;
	static constexpr double EYE_HEIGHT_FRACTION = 0.375;
	static constexpr double RECENTRE_FRACTION = 0.125;

	// The solid mask's CPU work.
	static constexpr int MASK_TILE_CELLS = 32;
	static constexpr double MASK_BUDGET_MS = 1.0;
	static constexpr int MASK_EVENT_TILES_PER_FRAME = 8;

	// Events.
	static constexpr int MAX_PENDING_EVENTS = 1024;
	static constexpr int MAX_HOLD_TICS = 70;
	static constexpr int MAX_KERNELS_PER_FRAME = 4096;
	static constexpr int SLAB_MIN_CELLS = 32;
	static constexpr int MAX_SLABS_PER_EVENT = 4;

	// The simulation's own constants (the look and the cvars scale some of them).
	static constexpr double HEAT_COOLING_PER_SECOND = 0.8;
	static constexpr double VELOCITY_DAMPING_PER_SECOND = 2.0;
	static constexpr double DIFFUSION_PER_STEP = 0.06;
	static constexpr double HEAT_LIFT = 160.0;			// map units per second squared, per unit of heat, x buoyancy
	static constexpr double DENSITY_LIFT = 12.0;		// the same per unit of density (up to 1), x buoyancy
	static constexpr double TURBULENCE_SPEED = 20.0;	// map units per second of swirl
	static constexpr double TURBULENCE_WAVELENGTH = 96.0;	// map units
	static constexpr double MAX_CELLS_PER_STEP = 4.0;	// at most half a tile, so one tile of margin always holds a step

	// [13d] The light grid's ambient columns: resolving a column's sector costs a BSP walk, so new columns
	// are resolved within this much CPU a frame, a block of this many columns a side at a time.
	static constexpr double AMBIENT_BUDGET_MS = 0.25;
	static constexpr int AMBIENT_BLOCK_COLUMNS = 16;

	static SmokeVolume& Get();

	// Once per frame for the main view, after SectorPlanes::BeginFrame. eye is the view
	// position (map units), viewYaw the view's yaw in radians (only the test source
	// uses it), levelSerial FLevelLocals::LevelDataSerial.
	void PrepareFrame(FLevelLocals* Level, const DVector3& eye, double viewYaw, double ticFrac, uint64_t levelSerial, SmokeVolumeFrame& out);

	// [13c] What this frame's PrepareFrame decided, for the drawing: SetupSmokeVolume
	// (hw_drawinfo.cpp) reads it later in the same frame. HasSmoke is false whenever nothing may
	// be drawn: no level, smoke off or not asked for, a refused quality, or the volume quiet.
	struct DrawState
	{
		bool HasSmoke = false;
		int Quality = 0;
		SmokeGridSpec Grid;
		int OriginCell[3] = { 0, 0, 0 };	// the box's first cell in world cells, as SmokeVolumeFrame
		float TicFrac = 0.f;
		int LightQuality = 0;				// [13d] the light grid this frame asks for (SmokeLightFrame::Quality)
	};
	const DrawState& GetDrawState() const { return mDraw; }

private:
	struct QueueCursor
	{
		uint64_t Serial = 0;
		int Count = 0;
	};

	struct PendingEvent
	{
		int Kind = SmokeKernel::EMIT;
		int Tic = 0;				// maptime of the tic it was queued in
		int HeldSince = -1;			// maptime it was first held back for the mask, or -1
		DVector3 Start{ 0., 0., 0. };
		DVector3 End{ 0., 0., 0. };
		DVector3 Vel{ 0., 0., 0. };	// map units per second
		double Radius = 0;			// map units
		double Amount = 0;
		double Heat = 0;
		double Strength = 0;		// map units per second
		double Soot = 0;			// [13e] EMIT: the share of the amount that is soot, 0..1
	};

	// An event in this frame's box, in grid cells.
	struct EventShape
	{
		double A[3] = { 0, 0, 0 };
		double B[3] = { 0, 0, 0 };
		double Radius = 0;			// cells, at least SmokeKernel::MIN_RADIUS
		double Scale = 1;			// what widening to MIN_RADIUS costs the amount
		int Min[3] = { 0, 0, 0 };	// the cells it can reach, clipped to the grid
		int Max[3] = { 0, 0, 0 };
	};

	// A world-aligned column block of mask work. Absolute cells: tile (TileX, TileY)
	// covers cells [TileX, TileX + 1) x MASK_TILE_CELLS, likewise y, full height.
	struct MaskTile
	{
		int TileX = 0;
		int TileY = 0;
		bool Unbuilt = false;		// part of it is solid only because it was never rasterised
		int UnbuiltZMin = 0;		// absolute cells: the layers that are unbuilt
		int UnbuiltZMax = 0;
	};

	struct Bounds
	{
		double Density = 0;
		double Heat = 0;
		double Speed = 0;			// cells per step
		double Soot = 0;			// [13e] soot density: carried exactly as the density, so bounded the same way
	};

	void ForgetEvents();
	void ReadQueues(FLevelLocals* Level, bool keep);
	void AddPending(const PendingEvent& e);
	bool HasPendingSmoke() const;
	void AddTestSourceEvents(int tic);

	void StartMaskBuild(const SmokeVolumeFrame& frame);
	void AddExposedWork(const SmokeVolumeFrame& frame, const int shift[3]);
	void AddTileWork(int cellX0, int cellY0, int cellX1, int cellY1, bool unbuilt, int zMin, int zMax);
	void MarkDirtySectors(const SmokeVolumeFrame& frame, SectorPlanes& planes);
	void RunMaskWork(FLevelLocals* Level, const SmokeVolumeFrame& frame, const DVector3& eye);
	bool TouchesUnbuilt(const EventShape& shape, const SmokeVolumeFrame& frame) const;

	bool MakeShape(const PendingEvent& e, const SmokeVolumeFrame& frame, EventShape& shape) const;
	void BuildKernels(const SmokeVolumeFrame& frame, double sums[][4]);	// [13e] per step: density, heat, speed, soot
	void AppendKernels(const PendingEvent& e, const EventShape& shape, int step, const SmokeVolumeFrame& frame);
	void FillSimSettings(FLevelLocals* Level, SmokeVolumeFrame& out) const;

	// [13d] The light grid's part of the frame (PrepareFrame calls PrepareLight once the box is placed).
	void PrepareLight(FLevelLocals* Level, const DVector3& eye, SmokeVolumeFrame& out);
	void UpdateAmbientColumns(FLevelLocals* Level, const DVector3& eye, const SmokeVolumeFrame& frame, SmokeLightFrame& light);
	void GatherLights(FLevelLocals* Level, const DVector3& eye, const SmokeVolumeFrame& frame, SmokeLightFrame& light);
	// [13e] The beam lines that may meet the smoke (only on frames with smoke to draw).
	void GatherBeams(FLevelLocals* Level, const DVector3& eye, double ticFrac, SmokeVolumeFrame& out);

	uint64_t mLevelSerial = 0;

	// [13c] GetDrawState's answer for this frame.
	DrawState mDraw;

	// Demand and linger.
	bool mHasDemand = false;
	int mLastDemandTime = 0;

	// The step clock.
	bool mClockValid = false;
	int mLastStepTime = 0;

	// The box.
	bool mPlaced = false;
	int mPlacedQuality = 0;
	int mOriginCell[3] = { 0, 0, 0 };
	uint64_t mBoxEpoch = 0;
	uint32_t mPlanesGeneration = 0;

	// Events.
	QueueCursor mEmitCursor;
	QueueCursor mCarveCursor;
	QueueCursor mImpulseCursor;
	uint32_t mClearSerial = 0;
	std::vector<PendingEvent> mPending;
	bool mPendingFullLogged = false;
	Bounds mBound;
	bool mResidue = false;			// the GPU may hold values under the bounds' empty line

	// The mask.
	std::vector<MaskTile> mMaskTiles;
	int mUnbuiltTiles = 0;
	bool mBuildLogPending = false;
	uint64_t mBuildNs = 0;
	int mBuildFrames = 0;
	uint64_t mBuildSolidCells = 0;
	uint64_t mBuildCells = 0;

	// The test source.
	bool mTestPlaced = false;
	DVector3 mTestPos{ 0., 0., 0. };
	DVector3 mTestDir{ 1., 0., 0. };

	// This frame's hand-over to the backend; the frame points into these.
	std::vector<SmokeKernel> mKernels;
	std::vector<SmokeMaskUpload> mMaskUploads;
	std::vector<uint8_t> mMaskBytes;

	// [13d] The light grid's lights this frame, and its ambient columns. mColumnSector holds each column's
	// sector index (-1 = not resolved yet), x fastest, for the columns from mAmbientOrigin (world light
	// columns); mBlockUnknown counts the unresolved columns of each AMBIENT_BLOCK_COLUMNS block. The bytes
	// (RGBA8 a column) are rebuilt only when something they show changed, and mAmbientSerial moves on then.
	std::vector<SmokeLightRecord> mLights;
	uint64_t mAmbientLevelSerial = 0;
	int mAmbientSize[2] = { 0, 0 };
	double mAmbientCellSize = 0;
	int mAmbientOrigin[2] = { 0, 0 };
	std::vector<int> mColumnSector;
	std::vector<int> mBlockUnknown;
	int mUnknownColumns = 0;
	int mFallbackColumns = 0;					// columns showing the fallback: not resolved yet, or no sector found (-2)
	std::vector<int> mAmbientSectors;			// every sector a column has resolved to, once
	std::vector<uint32_t> mSectorPacked;		// by sector index: its last packed light (RGBA8, alpha 255 once listed)
	uint32_t mFallbackPacked = 0;				// the eye's sector, for columns not resolved yet
	bool mAmbientDirty = true;
	std::vector<uint8_t> mAmbientBytes;
	uint64_t mAmbientSerial = 0;

	// [13e] This frame's beam list; the frame points into it.
	std::vector<SmokeBeamRecord> mBeams;
};
