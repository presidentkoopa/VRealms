/*
** hw_levelfield.cpp
**
** [LEVELFIELD] The level collision field's CPU side. See the header.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** The column parts, the line records, the window arithmetic and the toroidal bookkeeping are mirrored
** in Python ("Engine docs/COLLISION_8_IMPL_NOTES.md", mirror8).
**
*/

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hw_levelfield.h"
#include "hw_levelsolidity.h"
#include "hw_sectorplanes.h"
#include "hw_debrispool.h"	// [DEBRISPOOL] the debris pool asks for the field too
#include "hw_framecompute.h"
#include "hw_perflog.h"
#include "particledefs.h"
#include "g_levellocals.h"
#include "r_defs.h"
#include "doomdef.h"
#include "c_cvars.h"
#include "v_video.h"
#include "i_time.h"
#include "printf.h"

// [LEVELFIELD] r_particlecollision -- particles whose definition says `collide = level` collide with the
// whole level through the field ("Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #8). ON BY DEFAULT (owner,
// 2026-09-14: effects our mods use default ON); inert until such a particle spawns, so a map without one
// allocates and dispatches nothing. Off: those particles keep F4's plane (SpawnParticles' surface and
// floor) and the field is freed. Renderer-read every frame. Presentation only -- not SERVERINFO.
CVARD(Bool, r_particlecollision, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "particles whose definition says collide = level collide with the whole level; off = their spawn plane only (Vulkan only)")
// The field's area (LevelFieldSpecFor, hw_framecompute.h): 1 = 12.6 MB, 2 = 34.6 MB (default), 3 = 100.7 MB.
// The cell sizes are the same at every quality; a change re-allocates the field and bakes it again.
CUSTOM_CVARD(Int, r_particlecollision_quality, LEVEL_FIELD_QUALITY_DEFAULT, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "particle collision area, 1-3 (2 = about 35 MB; Vulkan only)")
{
	if (self < LEVEL_FIELD_QUALITY_MIN) { self = LEVEL_FIELD_QUALITY_MIN; return; }
	if (self > LEVEL_FIELD_QUALITY_MAX) { self = LEVEL_FIELD_QUALITY_MAX; return; }
}
// A measurement, not a setting: `collide = plane` definitions collide with the whole level too, so the
// field can be judged on a mod's existing sparks before any definition says `level`. Not archived.
CVARD(Bool, r_particlecollision_test, false, CVAR_GLOBALCONFIG, "collide = plane particles also collide with the whole level -- a test of the collision field (Vulkan only)")

namespace
{
	int PositiveMod(int a, int b)
	{
		const int m = a % b;
		return m < 0 ? m + b : m;
	}

	// Clamped into int range before converting, so a position far outside the map never overflows.
	int Floor(double v) { return (int)std::floor(std::clamp(v, -1.0e8, 1.0e8)); }
}

LevelField& LevelField::Get()
{
	static LevelField field;
	return field;
}

void LevelField::Reset()
{
	for (FieldLevel& level : mLevels)
	{
		level.Placed = false;
		level.State.clear();
		level.Queued = 0;
	}
	mBuildLogPending = false;
}

//-----------------------------------------------------------------------------
//
// The frame
//
//-----------------------------------------------------------------------------

void LevelField::PrepareFrame(FLevelLocals* Level, const DVector3& eye, uint64_t levelSerial, LevelFieldFrame& out)
{
	out = LevelFieldFrame();
	mTiles.clear();
	mDistances.clear();
	mLines.clear();
	mBoxes.clear();
	if (Level == nullptr)
		return;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	const int maptime = Level->maptime;
	const int quality = std::clamp((int)r_particlecollision_quality, LEVEL_FIELD_QUALITY_MIN, LEVEL_FIELD_QUALITY_MAX);
	// Renderer-read every frame, so they respond with a menu open. Vulkan only: GL and GLES never
	// draw GPU particles, so they do no CPU work for the field either.
	const bool vulkan = screen != nullptr && screen->IsVulkan();
	const bool on = vulkan && r_particlecollision;
	const bool test = on && r_particlecollision_test;
	const LevelFieldBackendStatus& status = LevelFieldStatus();
	const bool refused = status.RefusedQuality != 0 && status.RefusedQuality == quality;

	if (levelSerial != mLevelSerial)
	{
		mLevelSerial = levelSerial;
		mRingFullScan = true;
		mGridReady = false;
		Reset();
		// A field in use keeps lingering into the new map, so it is not freed now and made again at the
		// new map's first spark.
		if (mHasDemand)
			mLastDemandTime = maptime;
	}

	// Who asks for the field: a particle that collides with the level, written since the last frame -- or [DEBRISPOOL] debris
	// pieces that collide with the level, alive or waiting to go into the pool (DebrisPool::WantsLevelField, as of last frame).
	if (ScanRing(Level, on, test) || (on && DebrisPool::Get().WantsLevelField()))
	{
		mHasDemand = true;
		mLastDemandTime = maptime;
	}
	if (maptime < mLastDemandTime)
		mLastDemandTime = maptime;
	if (mHasDemand && maptime - mLastDemandTime >= LINGER_SECONDS * TICRATE)
		mHasDemand = false;

	out.Active = on && mHasDemand;
	out.Quality = quality;
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
		out.Levels[level] = LevelFieldSpecFor(quality, level);

	const bool ready = out.Active && !refused && status.Allocated && status.Quality == quality;
	if (!ready)
	{
		// Nothing to build into (yet, or any more): the backend allocates or frees in this frame's Run,
		// and the build starts the frame after its status says the field exists. The header stays "no
		// level", so every particle keeps its plane meanwhile.
		Reset();
		mEpoch = 0;
		mQuality = 0;
		return;
	}

	if (status.Epoch != mEpoch || quality != mQuality)
	{
		// A new allocation, or one a failure emptied: nothing in it is what this side remembers.
		Reset();
		mEpoch = status.Epoch;
		mQuality = quality;
	}

	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
		PlaceLevel(level, eye, out);
	MarkMovedSectors(Level, out);
	RunWork(Level, eye, out);

	// The header: the windows as they stand after this frame's moves, which is what this frame's
	// commands leave in the texels.
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
	{
		const FieldLevel& field = mLevels[level];
		if (!field.Placed)
			continue;
		for (int axis = 0; axis < 3; axis++)
			out.Header[level][axis] = (float)(field.Origin[axis] * LEVEL_FIELD_TILE_CELLS);
		out.Header[level][3] = (float)field.Spec.CellSize;
	}
	out.Header[2][0] = 1.0f;
	out.Header[2][1] = test ? 1.0f : 0.0f;
	out.Header[2][2] = (float)mLevels[0].Spec.Band();
	out.Header[2][3] = (float)mLevels[1].Spec.Band();

	out.Invalidate = mBoxes.empty() ? nullptr : mBoxes.data();
	out.InvalidateCount = (int)mBoxes.size();
	out.Tiles = mTiles.empty() ? nullptr : mTiles.data();
	out.TileCount = (int)mTiles.size();
	out.Distances = mDistances.empty() ? nullptr : mDistances.data();
	out.DistanceCount = mDistances.size();
	out.Lines = mLines.empty() ? nullptr : mLines.data();
	out.LineCount = (int)mLines.size();

	if (timed)
		PerfLog::AddCpuSample("fx.levelfield", (double)(I_nsTime() - startNs) / 1e6);
}

// True when a record of a definition that collides with the level was written since the last call.
// Always keeps its cursor up, so switching collision on never counts old spawns.
bool LevelField::ScanRing(FLevelLocals* Level, bool on, bool test)
{
	const unsigned size = Level->GpuParticles.Size();
	const uint64_t written = Level->GpuParticleWritten;
	const uint64_t serial = Level->GpuParticleSerial;

	bool found = false;
	const ParticleDefinitionGpu* table = ParticleDefinitionTableData();
	if (on && size > 0 && written > 0 && table != nullptr)
	{
		// The ring's sync rule (GpuParticleBuffer::Sync): a new serial, the cursor going back, or a whole
		// ring or more written since the last look means every record is looked at again.
		const bool full = mRingFullScan || serial != mRingSerial || written < mRingWritten || written - mRingWritten >= size;
		const uint64_t first = full ? (written > size ? written - size : 0) : mRingWritten;
		const unsigned slots = ParticleDefinitionSlotCount();
		for (uint64_t i = first; i < written && !found; i++)
		{
			const FLevelLocals::GpuParticleRecord& record = Level->GpuParticles[(unsigned)(i % size)];
			// A legacy record (r_gpuparticles_legacy) keeps a size in d.x, not a definition.
			if (record.e[0] < -8.f || !(record.b[3] > 0.f))
				continue;
			const float definition = record.d[0];
			if (!(definition > -0.5f && definition < (float)slots - 0.5f))
				continue;
			const float collide = table[(unsigned)(definition + 0.5f)].look[2];
			if (collide > 1.5f || (test && collide > 0.5f))
				found = true;
		}
	}
	mRingFullScan = false;
	mRingSerial = serial;
	mRingWritten = written;
	return found;
}

//-----------------------------------------------------------------------------
//
// Windows
//
//-----------------------------------------------------------------------------

void LevelField::QueueAll(int level)
{
	FieldLevel& field = mLevels[level];
	const size_t count = (size_t)field.Tiles[0] * (size_t)field.Tiles[1] * (size_t)field.Tiles[2];
	field.State.assign(count, TileState());
	for (TileState& state : field.State)
		state.Queue = QUEUE_BAKE;
	field.Queued = (int)count;
	if (!mBuildLogPending)
	{
		mBuildLogPending = true;
		mBuildNs = 0;
		mBuildFrames = 0;
	}
}

void LevelField::PlaceLevel(int level, const DVector3& eye, LevelFieldFrame& out)
{
	FieldLevel& field = mLevels[level];
	field.Spec = out.Levels[level];
	const LevelFieldSpec& spec = field.Spec;
	const int size[3] = { spec.SizeX, spec.SizeY, spec.SizeZ };
	const double tileUnits = (double)spec.CellSize * LEVEL_FIELD_TILE_CELLS;
	const double eyeAt[3] = { eye.X, eye.Y, eye.Z };
	const double share[3] = { 0.5, 0.5, EYE_HEIGHT_FRACTION };

	int ideal[3];
	for (int axis = 0; axis < 3; axis++)
	{
		field.Tiles[axis] = size[axis] / LEVEL_FIELD_TILE_CELLS;
		const double extent = size[axis] * (double)spec.CellSize;
		ideal[axis] = Floor((eyeAt[axis] - extent * share[axis]) / tileUnits + 0.5);
	}

	if (!field.Placed || (int)field.State.size() != field.Tiles[0] * field.Tiles[1] * field.Tiles[2])
	{
		for (int axis = 0; axis < 3; axis++)
			field.Origin[axis] = ideal[axis];
		field.Placed = true;
		QueueAll(level);
		out.ClearLevel[level] = true;
		return;
	}

	bool recentre = false;
	for (int axis = 0; axis < 3; axis++)
	{
		const double extent = size[axis] * (double)spec.CellSize;
		const double inWindow = eyeAt[axis] - field.Origin[axis] * tileUnits;
		if (std::fabs(inWindow - extent * share[axis]) > extent * RECENTRE_FRACTION)
			recentre = true;
	}
	if (!recentre)
		return;

	bool moved = false;
	bool far = false;
	for (int axis = 0; axis < 3; axis++)
	{
		const int shift = ideal[axis] - field.Origin[axis];
		if (shift != 0)
			moved = true;
		if (std::abs(shift) >= field.Tiles[axis])
			far = true;
	}
	if (!moved)
		return;
	for (int axis = 0; axis < 3; axis++)
		field.Origin[axis] = ideal[axis];

	if (far)
	{
		// Further than the window: nothing baked is still inside it.
		QueueAll(level);
		out.ClearLevel[level] = true;
		return;
	}

	// Each texel tile now stands for the world tile of the window congruent to it. Where that is not the
	// world tile its texels hold, they are invalidated this frame and queued.
	std::vector<char> changed(field.State.size(), 0);
	bool any = false;
	for (int tz = 0; tz < field.Tiles[2]; tz++)
	{
		for (int ty = 0; ty < field.Tiles[1]; ty++)
		{
			for (int tx = 0; tx < field.Tiles[0]; tx++)
			{
				const size_t index = (size_t)tx + (size_t)field.Tiles[0] * ((size_t)ty + (size_t)field.Tiles[1] * (size_t)tz);
				TileState& state = field.State[index];
				const int t[3] = { tx, ty, tz };
				bool same = true;
				for (int axis = 0; axis < 3; axis++)
				{
					const int expected = field.Origin[axis] + PositiveMod(t[axis] - field.Origin[axis], field.Tiles[axis]);
					if (state.World[axis] != expected)
						same = false;
				}
				if (state.Baked && !same)
				{
					state.Baked = false;
					changed[index] = 1;
					any = true;
				}
				if (!state.Baked && state.Queue == QUEUE_NONE)
				{
					state.Queue = QUEUE_BAKE;
					field.Queued++;
				}
			}
		}
	}
	if (any)
		AddInvalidation(level, changed, out);
}

// The changed texel tiles of a level as boxes, one run of tiles up each column. When they would not
// fit the frame's boxes the whole level is cleared instead.
void LevelField::AddInvalidation(int level, const std::vector<char>& changed, LevelFieldFrame& out)
{
	if (out.ClearLevel[level])
		return;

	FieldLevel& field = mLevels[level];
	const size_t before = mBoxes.size();
	for (int ty = 0; ty < field.Tiles[1]; ty++)
	{
		for (int tx = 0; tx < field.Tiles[0]; tx++)
		{
			const auto at = [&](int tz) { return changed[(size_t)tx + (size_t)field.Tiles[0] * ((size_t)ty + (size_t)field.Tiles[1] * (size_t)tz)] != 0; };
			int tz = 0;
			while (tz < field.Tiles[2])
			{
				if (!at(tz))
				{
					tz++;
					continue;
				}
				int end = tz + 1;
				while (end < field.Tiles[2] && at(end))
					end++;
				if ((int)mBoxes.size() >= LEVEL_FIELD_BOXES_PER_FRAME)
				{
					mBoxes.resize(before);
					// The whole level is cleared instead -- and the backend clears BEFORE it bakes this frame's
					// tiles, so any tile of this level already handed over this frame is taken back: it would come
					// out baked while this side, after QueueAll, believes it is not, and a later window move would
					// never invalidate it (mirror8 M4 found this).
					mTiles.erase(std::remove_if(mTiles.begin(), mTiles.end(), [level](const LevelFieldTile& tile) { return tile.Level == level; }), mTiles.end());
					QueueAll(level);
					out.ClearLevel[level] = true;
					return;
				}
				LevelFieldBox box;
				box.Level = level;
				box.TexelTileMin[0] = tx;
				box.TexelTileMin[1] = ty;
				box.TexelTileMin[2] = tz;
				box.TexelTileMax[0] = tx + 1;
				box.TexelTileMax[1] = ty + 1;
				box.TexelTileMax[2] = end;
				mBoxes.push_back(box);
				tz = end;
			}
		}
	}
}

// [SECTORPLANES] Both windows are active regions: watch the sectors under them. A sector that moved has
// every baked tile over its extent (plus the band) queued to bake again this frame.
void LevelField::MarkMovedSectors(FLevelLocals* Level, LevelFieldFrame& out)
{
	SectorPlanes& planes = SectorPlanes::Get();
	for (FieldLevel& field : mLevels)
	{
		// The window plus the band: a sector just outside it still shapes the tiles at its edge (a door whose
		// lines are within the band of them).
		const double band = field.Spec.Band();
		const double tileUnits = (double)field.Spec.CellSize * LEVEL_FIELD_TILE_CELLS;
		const double minX = field.Origin[0] * tileUnits;
		const double minY = field.Origin[1] * tileUnits;
		planes.PollBox(Level, minX - band, minY - band, minX + field.Tiles[0] * tileUnits + band, minY + field.Tiles[1] * tileUnits + band);
	}

	for (int index : planes.ChangedThisFrame())
	{
		double minX, minY, maxX, maxY;
		if (!planes.GetExtent(index, minX, minY, maxX, maxY))
			continue;
		for (FieldLevel& field : mLevels)
		{
			const double band = field.Spec.Band();
			const double tileUnits = (double)field.Spec.CellSize * LEVEL_FIELD_TILE_CELLS;
			const int x0 = std::max(Floor((minX - band) / tileUnits), field.Origin[0]);
			const int y0 = std::max(Floor((minY - band) / tileUnits), field.Origin[1]);
			const int x1 = std::min(Floor((maxX + band) / tileUnits), field.Origin[0] + field.Tiles[0] - 1);
			const int y1 = std::min(Floor((maxY + band) / tileUnits), field.Origin[1] + field.Tiles[1] - 1);
			for (int wy = y0; wy <= y1; wy++)
			{
				for (int wx = x0; wx <= x1; wx++)
				{
					const int tx = PositiveMod(wx, field.Tiles[0]);
					const int ty = PositiveMod(wy, field.Tiles[1]);
					for (int tz = 0; tz < field.Tiles[2]; tz++)
					{
						TileState& state = field.State[(size_t)tx + (size_t)field.Tiles[0] * ((size_t)ty + (size_t)field.Tiles[1] * (size_t)tz)];
						if (!state.Baked)
							continue;
						if (state.Queue == QUEUE_NONE)
							field.Queued++;
						state.Queue = QUEUE_URGENT;
					}
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
//
// The work
//
//-----------------------------------------------------------------------------

void LevelField::RunWork(FLevelLocals* Level, const DVector3& eye, LevelFieldFrame& out)
{
	if (mLevels[0].Queued <= 0 && mLevels[1].Queued <= 0)
		return;

	struct Block
	{
		int Level;
		int TX;
		int TY;
		int Class;			// 0 = a moved sector's, 1 = the rest
		double Distance2;	// of the block's centre from the eye, map units squared
	};
	std::vector<Block> blocks;
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
	{
		const FieldLevel& field = mLevels[level];
		if (field.Queued <= 0)
			continue;
		const double tileUnits = (double)field.Spec.CellSize * LEVEL_FIELD_TILE_CELLS;
		for (int ty = 0; ty < field.Tiles[1]; ty++)
		{
			for (int tx = 0; tx < field.Tiles[0]; tx++)
			{
				bool queued = false;
				bool urgent = false;
				for (int tz = 0; tz < field.Tiles[2]; tz++)
				{
					const uint8_t queue = field.State[(size_t)tx + (size_t)field.Tiles[0] * ((size_t)ty + (size_t)field.Tiles[1] * (size_t)tz)].Queue;
					if (queue != QUEUE_NONE)
						queued = true;
					if (queue == QUEUE_URGENT)
						urgent = true;
				}
				if (!queued)
					continue;
				const int wx = field.Origin[0] + PositiveMod(tx - field.Origin[0], field.Tiles[0]);
				const int wy = field.Origin[1] + PositiveMod(ty - field.Origin[1], field.Tiles[1]);
				const double dx = (wx + 0.5) * tileUnits - eye.X;
				const double dy = (wy + 0.5) * tileUnits - eye.Y;
				blocks.push_back({ level, tx, ty, urgent ? 0 : 1, dx * dx + dy * dy });
			}
		}
	}
	std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b)
	{
		if (a.Class != b.Class)
			return a.Class < b.Class;
		return a.Distance2 < b.Distance2;
	});

	const uint64_t startNs = I_nsTime();
	int urgentDone = 0;
	bool anyDone = false;
	for (const Block& block : blocks)
	{
		const bool urgent = block.Class == 0 && urgentDone < URGENT_BLOCKS_PER_FRAME;
		if (!urgent && anyDone && (double)(I_nsTime() - startNs) / 1e6 >= BUDGET_MS)
			break;
		if (!BakeBlock(Level, block.Level, block.TX, block.TY))
			break;	// the frame's tile or line room is used up
		anyDone = true;
		if (block.Class == 0)
			urgentDone++;
	}
	const uint64_t elapsedNs = I_nsTime() - startNs;

	// A moved sector's tiles not baked this frame still hold the old planes: invalidated now (particles
	// there keep their plane meanwhile) and baked in a later frame.
	for (int level = 0; level < LEVEL_FIELD_LEVELS; level++)
	{
		FieldLevel& field = mLevels[level];
		std::vector<char> changed;
		bool any = false;
		for (size_t i = 0; i < field.State.size(); i++)
		{
			TileState& state = field.State[i];
			if (state.Queue != QUEUE_URGENT)
				continue;
			state.Queue = QUEUE_BAKE;
			if (state.Baked)
			{
				if (changed.empty())
					changed.assign(field.State.size(), 0);
				state.Baked = false;
				changed[i] = 1;
				any = true;
			}
		}
		if (any)
			AddInvalidation(level, changed, out);
	}

	if (mBuildLogPending && anyDone)
	{
		mBuildNs += elapsedNs;
		mBuildFrames++;
		if (mLevels[0].Queued <= 0 && mLevels[1].Queued <= 0)
		{
			mBuildLogPending = false;
			const FieldLevel& fine = mLevels[0];
			const FieldLevel& coarse = mLevels[1];
			const double totalMs = (double)mBuildNs / 1e6;
			Printf("LevelField: built -- fine %d x %d x %d cells from (%d, %d, %d), coarse %d x %d x %d from (%d, %d, %d), %.1f ms of CPU over %d frames (budget %.1f ms a frame)\n",
				fine.Spec.SizeX, fine.Spec.SizeY, fine.Spec.SizeZ,
				fine.Origin[0] * LEVEL_FIELD_TILE_CELLS * fine.Spec.CellSize, fine.Origin[1] * LEVEL_FIELD_TILE_CELLS * fine.Spec.CellSize, fine.Origin[2] * LEVEL_FIELD_TILE_CELLS * fine.Spec.CellSize,
				coarse.Spec.SizeX, coarse.Spec.SizeY, coarse.Spec.SizeZ,
				coarse.Origin[0] * LEVEL_FIELD_TILE_CELLS * coarse.Spec.CellSize, coarse.Origin[1] * LEVEL_FIELD_TILE_CELLS * coarse.Spec.CellSize, coarse.Origin[2] * LEVEL_FIELD_TILE_CELLS * coarse.Spec.CellSize,
				totalMs, mBuildFrames, BUDGET_MS);
		}
	}
}

// One column block: every queued tile over texel column (tx, ty) of a level. False when the frame has
// no room left for it (it waits for the next frame).
bool LevelField::BakeBlock(FLevelLocals* Level, int level, int tx, int ty)
{
	FieldLevel& field = mLevels[level];
	const LevelFieldSpec& spec = field.Spec;
	const int T = LEVEL_FIELD_TILE_CELLS;
	const double cell = spec.CellSize;
	const double band = spec.Band();

	int queuedTiles = 0;
	for (int tz = 0; tz < field.Tiles[2]; tz++)
	{
		if (field.State[(size_t)tx + (size_t)field.Tiles[0] * ((size_t)ty + (size_t)field.Tiles[1] * (size_t)tz)].Queue != QUEUE_NONE)
			queuedTiles++;
	}
	if (queuedTiles == 0)
		return true;
	if ((int)mTiles.size() + queuedTiles > LEVEL_FIELD_TILES_PER_FRAME)
		return false;

	// The world column this texel column stands for in the window.
	const int wx = field.Origin[0] + PositiveMod(tx - field.Origin[0], field.Tiles[0]);
	const int wy = field.Origin[1] + PositiveMod(ty - field.Origin[1], field.Tiles[1]);
	const int cellX0 = wx * T;
	const int cellY0 = wy * T;
	const double minX = cellX0 * cell;
	const double minY = cellY0 * cell;
	const double maxX = minX + T * cell;
	const double maxY = minY + T * cell;

	// The lines whose wall pieces can be within the band of the block.
	GatherLines(Level, minX - band, minY - band, maxX + band, maxY + band);
	int lineCount = (int)mGathered.size();
	if ((int)mLines.size() + lineCount > LEVEL_FIELD_LINES_PER_FRAME)
	{
		if (!mLines.empty())
			return false;
		if (!mLineCapWarned)
		{
			mLineCapWarned = true;
			Printf("LevelField: %d lines within reach of one column block at (%.0f, %.0f) -- only the first %d are baked (logged once)\n",
				lineCount, minX, minY, LEVEL_FIELD_LINES_PER_FRAME);
		}
		lineCount = LEVEL_FIELD_LINES_PER_FRAME;
	}
	const int lineFirst = (int)mLines.size();
	for (int i = 0; i < lineCount; i++)
		mLines.push_back(MakeLine(Level->lines[mGathered[i]]));

	// SH1 once per column, for every tile of the block.
	mColumns.resize((size_t)T * (size_t)T);
	for (int y = 0; y < T; y++)
	{
		const double centreY = (cellY0 + y + 0.5) * cell;
		for (int x = 0; x < T; x++)
			SampleColumn(Level, (cellX0 + x + 0.5) * cell, centreY, mColumns[(size_t)x + (size_t)T * (size_t)y]);
	}

	for (int tz = 0; tz < field.Tiles[2]; tz++)
	{
		TileState& state = field.State[(size_t)tx + (size_t)field.Tiles[0] * ((size_t)ty + (size_t)field.Tiles[1] * (size_t)tz)];
		if (state.Queue == QUEUE_NONE)
			continue;

		const int wz = field.Origin[2] + PositiveMod(tz - field.Origin[2], field.Tiles[2]);
		const int cellZ0 = wz * T;

		LevelFieldTile tile;
		tile.Level = level;
		tile.TexelTile[0] = tx;
		tile.TexelTile[1] = ty;
		tile.TexelTile[2] = tz;
		tile.WorldCell[0] = cellX0;
		tile.WorldCell[1] = cellY0;
		tile.WorldCell[2] = cellZ0;
		tile.LineFirst = lineFirst;
		tile.LineCount = lineCount;
		tile.DistanceOffset = mDistances.size();
		mDistances.resize(mDistances.size() + (size_t)LEVEL_FIELD_TILE_TEXELS);
		float* distances = mDistances.data() + tile.DistanceOffset;
		for (int z = 0; z < T; z++)
		{
			const double centreZ = (cellZ0 + z + 0.5) * cell;
			for (int y = 0; y < T; y++)
			{
				for (int x = 0; x < T; x++)
					distances[(size_t)x + (size_t)T * ((size_t)y + (size_t)T * (size_t)z)] = ColumnValue(mColumns[(size_t)x + (size_t)T * (size_t)y], centreZ, band);
			}
		}
		mTiles.push_back(tile);

		state.Baked = true;
		state.World[0] = wx;
		state.World[1] = wy;
		state.World[2] = wz;
		state.Queue = QUEUE_NONE;
		field.Queued--;
	}
	return true;
}

//-----------------------------------------------------------------------------
//
// The bake source
//
//-----------------------------------------------------------------------------

// SH1 at a column's centre: the render subsector holding it (none: the column is void, solid top to
// bottom) and its sector's open spans now. A boundary that is the sector's own floor or ceiling carries
// that plane's |normal.z|, so a vertical distance becomes the distance to a slope; a solid 3D floor's
// boundary counts as flat.
void LevelField::SampleColumn(FLevelLocals* Level, double x, double y, Column& column)
{
	column.Inside = false;
	column.Count = 0;
	subsector_t* ss = LevelSolidity::SubsectorContaining(Level, x, y);
	if (ss == nullptr || ss->sector == nullptr)
		return;
	column.Inside = true;

	const sector_t* sector = ss->sector;
	LevelSolidity::Span spans[LevelSolidity::MAX_SPANS];
	const int count = LevelSolidity::SectorOpenSpans(sector, x, y, spans);
	const double floorZ = sector->floorplane.ZatPoint(x, y);
	const double ceilingZ = sector->ceilingplane.ZatPoint(x, y);
	const double floorFactor = std::clamp(std::fabs(sector->floorplane.Normal().Z), 0.0, 1.0);
	const double ceilingFactor = std::clamp(std::fabs(sector->ceilingplane.Normal().Z), 0.0, 1.0);
	column.Count = count;
	for (int i = 0; i < count; i++)
	{
		column.Bottom[i] = spans[i].Bottom;
		column.Top[i] = spans[i].Top;
		column.BottomFactor[i] = spans[i].Bottom == floorZ ? floorFactor : 1.0;
		column.TopFactor[i] = spans[i].Top == ceilingZ ? ceilingFactor : 1.0;
	}
}

// A cell centre's column part: negative when the centre is solid (outside every open span, or a void
// column), its magnitude the distance to the column's nearest boundary, clamped to the band and never
// below MIN_MAGNITUDE.
float LevelField::ColumnValue(const Column& column, double z, double band)
{
	if (!column.Inside || column.Count <= 0)
		return (float)-band;

	bool open = false;
	double nearest = band;
	for (int i = 0; i < column.Count; i++)
	{
		if (z >= column.Bottom[i] && z < column.Top[i])
			open = true;
		nearest = std::min(nearest, std::fabs(z - column.Bottom[i]) * column.BottomFactor[i]);
		nearest = std::min(nearest, std::fabs(z - column.Top[i]) * column.TopFactor[i]);
	}
	nearest = std::max(nearest, (double)MIN_MAGNITUDE);
	return (float)(open ? nearest : -nearest);
}

// A line's wall pieces from the planes as they are now, at both ends (LevelFieldLine).
LevelFieldLine LevelField::MakeLine(const line_t& line)
{
	LevelFieldLine record = {};
	const double x1 = line.v1->fX(), y1 = line.v1->fY();
	const double x2 = line.v2->fX(), y2 = line.v2->fY();
	record.Segment[0] = (float)x1;
	record.Segment[1] = (float)y1;
	record.Segment[2] = (float)x2;
	record.Segment[3] = (float)y2;

	const auto parts = [&](double x, double y, float* out)
	{
		// No part: a top that is not above its bottom.
		out[0] = 1.0f;
		out[1] = -1.0f;
		out[2] = 1.0f;
		out[3] = -1.0f;
		const sector_t* front = line.frontsector;
		const sector_t* back = line.backsector;
		if (front != nullptr && back != nullptr)
		{
			// Exactly one side open: between the two floors, and between the two ceilings.
			const double frontFloor = front->floorplane.ZatPoint(x, y);
			const double backFloor = back->floorplane.ZatPoint(x, y);
			const double frontCeiling = front->ceilingplane.ZatPoint(x, y);
			const double backCeiling = back->ceilingplane.ZatPoint(x, y);
			out[0] = (float)std::min(frontFloor, backFloor);
			out[1] = (float)std::max(frontFloor, backFloor);
			out[2] = (float)std::min(frontCeiling, backCeiling);
			out[3] = (float)std::max(frontCeiling, backCeiling);
		}
		else if (front != nullptr || back != nullptr)
		{
			// A line with one sector is a wall, floor to ceiling, whatever its flags say.
			const sector_t* sector = front != nullptr ? front : back;
			out[0] = (float)sector->floorplane.ZatPoint(x, y);
			out[1] = (float)sector->ceilingplane.ZatPoint(x, y);
		}
	};
	parts(x1, y1, record.PartsStart);
	parts(x2, y2, record.PartsEnd);
	return record;
}

//-----------------------------------------------------------------------------
//
// Line buckets
//
//-----------------------------------------------------------------------------

void LevelField::EnsureLineGrid(FLevelLocals* Level)
{
	if (mGridReady && mGridSerial == mLevelSerial)
		return;
	mGridReady = true;
	mGridSerial = mLevelSerial;

	const unsigned lineCount = Level->lines.Size();
	mLineStamp.assign(lineCount, 0u);
	mStamp = 0;

	double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
	for (unsigned i = 0; i < lineCount; i++)
	{
		const line_t& line = Level->lines[i];
		if (line.v1 == nullptr || line.v2 == nullptr)
			continue;
		minX = std::min({ minX, line.v1->fX(), line.v2->fX() });
		minY = std::min({ minY, line.v1->fY(), line.v2->fY() });
		maxX = std::max({ maxX, line.v1->fX(), line.v2->fX() });
		maxY = std::max({ maxY, line.v1->fY(), line.v2->fY() });
	}
	if (minX > maxX)
	{
		mGridWidth = mGridHeight = 0;
		mGridStart.assign(1, 0);
		mGridLines.clear();
		return;
	}

	mGridMinX = minX;
	mGridMinY = minY;
	mGridWidth = std::min(Floor((maxX - minX) / LINE_BUCKET_UNITS) + 1, 4096);
	mGridHeight = std::min(Floor((maxY - minY) / LINE_BUCKET_UNITS) + 1, 4096);
	const size_t buckets = (size_t)mGridWidth * (size_t)mGridHeight;

	const auto range = [&](const line_t& line, int& x0, int& y0, int& x1, int& y1)
	{
		x0 = std::clamp(Floor((std::min(line.v1->fX(), line.v2->fX()) - mGridMinX) / LINE_BUCKET_UNITS), 0, mGridWidth - 1);
		x1 = std::clamp(Floor((std::max(line.v1->fX(), line.v2->fX()) - mGridMinX) / LINE_BUCKET_UNITS), 0, mGridWidth - 1);
		y0 = std::clamp(Floor((std::min(line.v1->fY(), line.v2->fY()) - mGridMinY) / LINE_BUCKET_UNITS), 0, mGridHeight - 1);
		y1 = std::clamp(Floor((std::max(line.v1->fY(), line.v2->fY()) - mGridMinY) / LINE_BUCKET_UNITS), 0, mGridHeight - 1);
	};

	mGridStart.assign(buckets + 1, 0);
	for (unsigned i = 0; i < lineCount; i++)
	{
		const line_t& line = Level->lines[i];
		if (line.v1 == nullptr || line.v2 == nullptr)
			continue;
		int x0, y0, x1, y1;
		range(line, x0, y0, x1, y1);
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++)
				mGridStart[(size_t)x + (size_t)mGridWidth * (size_t)y + 1]++;
	}
	for (size_t b = 0; b < buckets; b++)
		mGridStart[b + 1] += mGridStart[b];
	mGridLines.assign((size_t)mGridStart[buckets], 0);
	std::vector<int> fill(mGridStart.begin(), mGridStart.end() - 1);
	for (unsigned i = 0; i < lineCount; i++)
	{
		const line_t& line = Level->lines[i];
		if (line.v1 == nullptr || line.v2 == nullptr)
			continue;
		int x0, y0, x1, y1;
		range(line, x0, y0, x1, y1);
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++)
				mGridLines[(size_t)fill[(size_t)x + (size_t)mGridWidth * (size_t)y]++] = (int)i;
	}
}

// The lines whose bounding box (vertices as they are now) touches [minX, maxX] x [minY, maxY], each once,
// into mGathered, in line index order within each bucket.
void LevelField::GatherLines(FLevelLocals* Level, double minX, double minY, double maxX, double maxY)
{
	EnsureLineGrid(Level);
	mGathered.clear();
	if (mGridWidth <= 0 || mGridHeight <= 0)
		return;

	if (++mStamp == 0)
	{
		std::fill(mLineStamp.begin(), mLineStamp.end(), 0u);
		mStamp = 1;
	}

	const int x0 = std::clamp(Floor((minX - mGridMinX) / LINE_BUCKET_UNITS), 0, mGridWidth - 1);
	const int x1 = std::clamp(Floor((maxX - mGridMinX) / LINE_BUCKET_UNITS), 0, mGridWidth - 1);
	const int y0 = std::clamp(Floor((minY - mGridMinY) / LINE_BUCKET_UNITS), 0, mGridHeight - 1);
	const int y1 = std::clamp(Floor((maxY - mGridMinY) / LINE_BUCKET_UNITS), 0, mGridHeight - 1);
	for (int y = y0; y <= y1; y++)
	{
		for (int x = x0; x <= x1; x++)
		{
			const size_t bucket = (size_t)x + (size_t)mGridWidth * (size_t)y;
			for (int k = mGridStart[bucket]; k < mGridStart[bucket + 1]; k++)
			{
				const int index = mGridLines[(size_t)k];
				if ((size_t)index >= mLineStamp.size() || mLineStamp[(size_t)index] == mStamp)
					continue;
				mLineStamp[(size_t)index] = mStamp;
				const line_t& line = Level->lines[(unsigned)index];
				if (line.v1 == nullptr || line.v2 == nullptr)
					continue;
				if (std::max(line.v1->fX(), line.v2->fX()) < minX || std::min(line.v1->fX(), line.v2->fX()) > maxX ||
					std::max(line.v1->fY(), line.v2->fY()) < minY || std::min(line.v1->fY(), line.v2->fY()) > maxY)
					continue;
				mGathered.push_back(index);
			}
		}
	}
}
