/*
** hw_surfacedamage.h
**
** [SURFACEDAMAGE] The surface damage atlas's CPU side: the paints, the surfaces they land on, the tiles, the hash, the
** surface records and their pegging anchors, the cooling clock, and the draw keys.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SURFACE_DAMAGE_PLAN.md" #17, "Engine docs/SURFACE_DAMAGE_17_IMPL_NOTES.md". Everything here survives a render
** rebuild: it reads the level and fills SurfaceDamageFrame (hw_surfacedamageframe.h); the GPU side (vk_surfacedamage.cpp)
** acts on it, and main.fp draws it.
**
**   - PAINTS: LevelLocals.PaintSurfaceDamage queues an event (FLevelLocals::SurfaceDamagePaints, SH4). Once per frame this
**     side drains the queue: a short Trace() into the hit finds the wall part or the flat, its texture's #16 surface name
**     picks the look, the brush's variant (and a hashed turn or flip) comes from a hash of the paint's position, and the
**     rotated brush square's cells (clipped to the wall part) get tiles and one stamp each.
**   - TILES: pages x 225, handed out least recently painted first. A cell's tile is found through an open-addressing hash
**     with four probes, mirrored into the data buffer for main.fp; an insert with all four probes taken evicts the least
**     recently painted of those four.
**   - SURFACES: a wall part (side, part) or a flat (sector, plane) that owns at least one tile has a record slot: two planes
**     over the world position giving its own u and v in map units. A wall's v is measured from its pegging anchor -- a
**     sector plane, read from the level each frame -- so holes ride a door's face with the door.
**   - COOLING: every 8 world tics the hot tiles lose heat (damage_cool.comp), on maptime, so a menu or a pause freezes it.
**   - DRAW KEYS: RenderTexturedWall and DrawFlat ask for their surface's slot + 1 (0 = none), which rides in the per-draw
**     data (uSurfaceDamageKey, hw_renderstate.h). Zero whenever this machine does not draw damage.
**
** Main thread only. Presentation only: nothing here writes to the playsim, nothing is read back from the GPU, no RNG, and
** nothing is saved -- damage lasts the map (or until its tiles are reused) and a savegame load starts clean.
**
*/

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "hw_surfacedamageframe.h"
#include "zstring.h"

struct FLevelLocals;
struct side_t;
struct sector_t;
struct FSurfaceDamagePaintEvent;

class SurfaceDamage
{
public:
	// The atlas lingers this long (level seconds) after its last paint while no tile is in use, then is freed.
	static constexpr int LINGER_SECONDS = 60;
	// The trace: from the paint's position backed off this far along its normal, this far into the surface.
	static constexpr double TRACE_BACKOFF = 4.0;
	static constexpr double TRACE_DISTANCE = 12.0;

	static SurfaceDamage& Get();

	// Once per frame for the main view, after DebrisPool::PrepareFrame (hw_entrypoint.cpp's PrepareFrameCompute, right before
	// RunFrameCompute). Fills SurfaceDamageFrameForBackend(). eye, yaw and pitch (map units, radians) are the main view's, for
	// the desk test (r_damage_test) only.
	void PrepareFrame(FLevelLocals* Level, double eyeX, double eyeY, double eyeZ, double yaw, double pitch, uint64_t levelSerial);

	// r_damage_test paints every TEST_TICS world tics.
	static constexpr int TEST_TICS = 9;

	// The draw keys: the surface's record slot + 1, or 0. part: 0 upper, 1 one-sided middle, 2 lower, -1 any other wall.
	int WallKey(const side_t* side, int part) const;
	int FlatKey(const sector_t* sector, bool ceiling) const;

	// For `damagebrushes`: the definitions, the switches and what the atlas holds.
	FString Report() const;

private:
	struct QueueCursor
	{
		uint64_t Serial = 0;
		int Count = 0;
	};

	struct Tile
	{
		int32_t Surface = -1;		// the surface slot, -1 free
		int32_t CellU = 0;
		int32_t CellV = 0;
		int32_t HashEntry = -1;
		int32_t Prev = -1;			// the LRU list, most recently painted first
		int32_t Next = -1;
		uint32_t Generation = 0;	// rises every time the tile is handed out
		uint64_t LastPaint = 0;
		int32_t HotUntil = -1;		// the cooling pass it is listed until; -1 not hot
		uint8_t Look = 0;
		bool ClearPending = false;	// handed out, not yet cleared by a dispatch
	};

	struct Surface
	{
		uint32_t Key = 0;
		int32_t Tiles = 0;
		int32_t LiveIndex = -1;		// in mLiveSurfaces
		int32_t AnchorSector = -1;	// walls: the sector whose plane pegs v; -1 for a flat
		int32_t AnchorPlane = 0;	// 0 floor, 1 ceiling
		double Anchor[4] = {};		// that plane (normal xyz, D) when V was last written
		double U[4] = {};			// map axes: u = U.xyz . pos + U.w
		double V[4] = {};
	};

	struct Stamp
	{
		SurfaceDamageStampGpu Gpu;
		int32_t Tile;
		uint32_t Generation;
	};

	struct HotTile
	{
		int32_t Tile;
		uint32_t Generation;
	};

	struct PaintTarget;

	void Reset();
	void Release();
	void Paint(FLevelLocals* Level, const FSurfaceDamagePaintEvent& paint);
	bool MakeTestPaint(FLevelLocals* Level, double eyeX, double eyeY, double eyeZ, double yaw, double pitch, FSurfaceDamagePaintEvent& paint);
	bool FindTarget(FLevelLocals* Level, const FSurfaceDamagePaintEvent& paint, PaintTarget& target);
	int FindOrAddSurface(const PaintTarget& target);
	int FindOrAllocateTile(int surface, int32_t cellU, int32_t cellV, int look);
	int TakeTile();
	void FreeTile(int tile);
	void DropSurfaceIfEmpty(int slot);
	void Touch(int tile);
	void Unlink(int tile);
	void UpdateAnchors(FLevelLocals* Level);
	void Cool(int maptime, SurfaceDamageFrame& out);
	void BuildDispatch(SurfaceDamageFrame& out);
	void WriteSurface(int slot);
	void WriteHashEntry(int entry);
	void WriteLooks();
	void MarkDirty(int first, int end);

	uint64_t mFrameSerial = 0;
	uint64_t mLevelSerial = 0;
	uint64_t mDefinitionsGeneration = 0;
	QueueCursor mCursor;
	std::vector<const FSurfaceDamagePaintEvent*> mPending;	// this frame's paints, in the level's queue until its next tic

	int mPages = 0;				// what the tiles below describe; 0 = nothing held
	int mHashEntries = 0;
	std::vector<Tile> mTiles;
	std::vector<int32_t> mFreeTiles;
	int32_t mLruHead = -1;
	int32_t mLruTail = -1;
	int mTilesInUse = 0;
	uint64_t mPaintClock = 0;
	std::vector<int32_t> mHash;		// entry -> tile, -1 empty
	std::vector<Surface> mSurfaces;
	std::vector<int32_t> mFreeSurfaces;
	std::vector<int32_t> mLiveSurfaces;
	std::unordered_map<uint32_t, int32_t> mKeyToSurface;

	std::vector<float> mData;		// SurfaceDamageDataVec4s(mHashEntries) x 4
	int mDirtyFirst = 0;
	int mDirtyEnd = 0;
	uint64_t mDataGeneration = 0;

	std::vector<Stamp> mStamps;
	std::vector<int32_t> mClearTiles;
	std::vector<SurfaceDamageStampGpu> mStampsOut;
	std::vector<SurfaceDamageTileGpu> mStampTilesOut;
	std::vector<HotTile> mHot;
	std::vector<SurfaceDamageTileGpu> mCoolTilesOut;

	int mCoolPass = 0;
	int mLastCoolTic = 0;
	bool mCoolClockValid = false;

	bool mHasDemand = false;
	int mLastPaintTime = 0;
	bool mDrawable = false;
	int mTestNextTic = 0;
	int mTestCount = 0;

	// Once-per-map log lines and counters for the report.
	bool mStampsFullLogged = false;
	bool mTileStampsLogged = false;
	std::vector<int> mUnknownBrushesLogged;
	uint64_t mPaints = 0;
	uint64_t mPaintsMissed = 0;
	uint64_t mTilesEvicted = 0;
	uint64_t mHashEvictions = 0;
};

// For RenderTexturedWall and DrawFlat (hw_walls.cpp, hw_flats.cpp).
int SurfaceDamageWallKey(const side_t* side, int part);
int SurfaceDamageFlatKey(const sector_t* sector, bool ceiling);
