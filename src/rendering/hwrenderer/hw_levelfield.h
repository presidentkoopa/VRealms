/*
** hw_levelfield.h
**
** [LEVELFIELD] The level collision field's CPU side: when it exists, where its windows are, which
** tiles are baked, and what goes into them.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #8, "Engine docs/COLLISION_8_IMPL_NOTES.md".
** Everything here survives a render rebuild: it only reads the level and fills LevelFieldFrame
** (hw_framecompute.h); the GPU side (vk_levelfield.cpp) acts on it.
**
**   - DEMAND: the field exists while r_particlecollision is on and a particle whose definition says
**     `collide = level` (or `plane`, while r_particlecollision_test is on) was written to the ring on
**     this map within the last LINGER_SECONDS of level time. Found by reading the ring's new records
**     from this side's own cursor (MeshParticleBuffer's sync rule); nothing is written to the level.
**   - WINDOWS: per level, whole tiles, centred on the eye in x and y with the eye at
**     EYE_HEIGHT_FRACTION of the height, moved when the eye strays more than RECENTRE_FRACTION of an
**     extent. The volume is toroidal (hw_framecompute.h), so a move invalidates the tiles whose world
**     meaning changed and queues them; a jump further than the window clears the level.
**   - THE BAKE SOURCE (review D1, S2): nothing that moves comes from the load-time mesh. Per column,
**     SH1 (LevelSolidity) at the column's centre -- the render subsector holding it and its sector's
**     open spans from the CURRENT planes (slopes, solid 3D floors). Per cell centre, the SIGN is SH1's
**     point test and the column part of the MAGNITUDE the vertical distance to the column's nearest
**     floor or ceiling (times that plane's |normal.z|). The GPU takes the minimum with every nearby
**     line's wall pieces (LevelFieldLine), heights from the current planes.
**   - MOVERS: SH2 (SectorPlanes) polls both windows; a sector that moved -- a door, a lift -- has the
**     column blocks under its extent (plus the band) baked again this frame, first and outside the
**     budget up to URGENT_BLOCKS_PER_FRAME; any it cannot do are invalidated this frame, so a stale
**     door is never trusted.
**   - WORK: one COLUMN BLOCK (16 x 16 columns and every queued tile over it) at a time, moved sectors
**     first, then nearest the eye, within BUDGET_MS a frame and the frame's tile and line room.
**
** Main thread only. Presentation only: nothing here writes to the playsim.
**
*/

#pragma once

#include <cstdint>
#include <vector>

#include "vectors.h"
#include "hw_framecompute.h"
#include "hw_levelsolidity.h"

struct FLevelLocals;
struct line_t;

class LevelField
{
public:
	static constexpr int LINGER_SECONDS = 60;
	static constexpr double EYE_HEIGHT_FRACTION = 0.375;
	static constexpr double RECENTRE_FRACTION = 0.125;

	// The CPU work: SH1 per column and the column parts per cell, like the smoke mask's.
	static constexpr double BUDGET_MS = 1.0;
	static constexpr int URGENT_BLOCKS_PER_FRAME = 16;

	// Lines are bucketed once per map in squares this size (map units).
	static constexpr double LINE_BUCKET_UNITS = 256.0;

	// The smallest magnitude written, so a cell's sign always survives the half float
	// (field_bake.comp's kMinMagnitude).
	static constexpr float MIN_MAGNITUDE = 1.0f / 64.0f;

	static LevelField& Get();

	// Once per frame for the main view, after SectorPlanes::BeginFrame (hw_entrypoint.cpp's
	// PrepareFrameCompute). eye is the view position in map units, levelSerial
	// FLevelLocals::LevelDataSerial.
	void PrepareFrame(FLevelLocals* Level, const DVector3& eye, uint64_t levelSerial, LevelFieldFrame& out);

private:
	enum : uint8_t
	{
		QUEUE_NONE = 0,
		QUEUE_BAKE = 1,
		QUEUE_URGENT = 2,	// baked, but a sector under it moved: bake again this frame or invalidate
	};

	struct TileState
	{
		int32_t World[3] = { 0, 0, 0 };	// the world tile its texels hold, when Baked
		bool Baked = false;
		uint8_t Queue = QUEUE_NONE;
	};

	struct FieldLevel
	{
		LevelFieldSpec Spec;
		int Tiles[3] = { 0, 0, 0 };
		bool Placed = false;
		int Origin[3] = { 0, 0, 0 };	// the window's first world tile
		std::vector<TileState> State;	// by texel tile: x + Tiles[0] * (y + Tiles[1] * z)
		int Queued = 0;
	};

	// One column's open spans at its centre (SH1), with each boundary's slope factor.
	struct Column
	{
		bool Inside = false;	// the centre is in a render subsector
		int Count = 0;
		double Bottom[LevelSolidity::MAX_SPANS];
		double Top[LevelSolidity::MAX_SPANS];
		double BottomFactor[LevelSolidity::MAX_SPANS];
		double TopFactor[LevelSolidity::MAX_SPANS];
	};

	void Reset();
	bool ScanRing(FLevelLocals* Level, bool on, bool test);
	void QueueAll(int level);
	void PlaceLevel(int level, const DVector3& eye, LevelFieldFrame& out);
	void MarkMovedSectors(FLevelLocals* Level, LevelFieldFrame& out);
	void RunWork(FLevelLocals* Level, const DVector3& eye, LevelFieldFrame& out);
	bool BakeBlock(FLevelLocals* Level, int level, int tx, int ty);
	void AddInvalidation(int level, const std::vector<char>& changed, LevelFieldFrame& out);
	void EnsureLineGrid(FLevelLocals* Level);
	void GatherLines(FLevelLocals* Level, double minX, double minY, double maxX, double maxY);

	static void SampleColumn(FLevelLocals* Level, double x, double y, Column& column);
	static float ColumnValue(const Column& column, double z, double band);
	static LevelFieldLine MakeLine(const line_t& line);

	uint64_t mLevelSerial = 0;
	int mQuality = 0;
	uint64_t mEpoch = 0;		// the backend allocation the tile states describe

	// Demand and linger.
	bool mHasDemand = false;
	int mLastDemandTime = 0;
	uint64_t mRingSerial = 0;
	uint64_t mRingWritten = 0;
	bool mRingFullScan = true;

	FieldLevel mLevels[LEVEL_FIELD_LEVELS];

	// The build log after a level is cleared.
	bool mBuildLogPending = false;
	uint64_t mBuildNs = 0;
	int mBuildFrames = 0;

	// Line buckets, per map (compressed rows: the lines of bucket b are mGridLines[mGridStart[b] ..
	// mGridStart[b + 1])).
	uint64_t mGridSerial = 0;
	bool mGridReady = false;
	double mGridMinX = 0;
	double mGridMinY = 0;
	int mGridWidth = 0;
	int mGridHeight = 0;
	std::vector<int> mGridStart;
	std::vector<int> mGridLines;
	std::vector<uint32_t> mLineStamp;
	uint32_t mStamp = 0;
	std::vector<int> mGathered;
	bool mLineCapWarned = false;

	std::vector<Column> mColumns;

	// This frame's hand-over to the backend; the frame points into these.
	std::vector<LevelFieldTile> mTiles;
	std::vector<float> mDistances;
	std::vector<LevelFieldLine> mLines;
	std::vector<LevelFieldBox> mBoxes;
};
