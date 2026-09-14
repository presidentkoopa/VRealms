/*
** hw_surfacedamage.cpp
**
** [SURFACEDAMAGE] The surface damage atlas's CPU side. See the header.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** The cell cover, the pegging anchors, the tile allocator and hash, the stamps' combine rules and the cooling are mirrored
** in Python ("Engine docs/SURFACE_DAMAGE_17_IMPL_NOTES.md", mirror17).
**
*/

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hw_surfacedamage.h"
#include "hw_surfacedamageframe.h"
#include "damagedefs.h"
#include "g_levellocals.h"
#include "r_defs.h"
#include "p_trace.h"
#include "r_sky.h"
#include "textures.h"
#include "texturemanager.h"
#include "hw_perflog.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "doomdef.h"
#include "doomdata.h"
#include "v_video.h"
#include "i_time.h"
#include "printf.h"
#include "v_text.h"

// [SURFACEDAMAGE] r_damage -- "Wall damage" ("Engine docs/SURFACE_DAMAGE_PLAN.md" #17). ON BY DEFAULT (owner answer, 2026-09-14);
// inert until a mod calls PaintSurfaceDamage, so a map nobody paints allocates and dispatches nothing. Off: nothing is drawn,
// the atlas is freed, and a mod may bring its decals back. Renderer-read every frame. Presentation only -- not SERVERINFO.
CVARD(Bool, r_damage, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "lasting surface damage painted by mods (PaintSurfaceDamage): holes, soot, heat, wet; off frees it (Vulkan only)")
// "Damage memory": 64, 128, 256 (default) or 512 MB of damage tiles -- 900, 1,800, 3,600 or 7,200 damaged 64-unit patches (owner
// answer 10). Each page's mip level adds a quarter on the GPU. A change clears all damage: the pages are made again.
CUSTOM_CVARD(Int, r_damage_memory, SURFACE_DAMAGE_MEMORY_DEFAULT, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "surface damage tile memory in MB: 64, 128, 256 or 512 (900 .. 7,200 patches); changing it clears all damage (Vulkan only)")
{
	const int snapped = SurfaceDamageMemorySnap(self);
	if (snapped != self) { self = snapped; return; }
}
// "Heat glow": struck metal glows where hit and cools. Off: no glow is drawn (the heat still cools). Renderer-read every frame.
CVARD(Bool, r_damage_heat, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "struck surfaces glow with the heat a mod paints, then cool (Vulkan only)")
// The live sliders (review M9): renderer-read scales over what the mods paint, 1 = as painted. A menu freezes the game but not
// these: they change the drawing, not the paint.
CUSTOM_CVARD(Float, r_damage_soot_scale, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "multiplies how dark surface damage soot draws (0-4; Vulkan only)")
{
	if (self < 0.f) { self = 0.f; return; }
	if (self > 4.f) { self = 4.f; return; }
}
CUSTOM_CVARD(Float, r_damage_depth_scale, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "multiplies how deep surface damage holes draw (0-4; Vulkan only)")
{
	if (self < 0.f) { self = 0.f; return; }
	if (self > 4.f) { self = 4.f; return; }
}
CUSTOM_CVARD(Float, r_damage_heat_scale, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "multiplies how brightly hot surface damage glows (0-4; Vulkan only)")
{
	if (self < 0.f) { self = 0.f; return; }
	if (self > 4.f) { self = 4.f; return; }
}
// "Show damage tiles": each damaged surface's tiles tinted by look, with their edges drawn. A desk check, not saved.
CVARD(Bool, r_damage_debug, false, CVAR_GLOBALCONFIG, "tints every surface damage tile and draws its edges -- a check of the atlas (Vulkan only)")
// "Wall damage test (paints where you look)": a test, not a setting. While on, every quarter second of level time the wall or
// flat at the centre of the main view (within 2,048 map units) is painted, cycling through every DAMAGEDEFS brush and four kinds
// of hit -- a hole, a hot hit that glows and cools, scorch, a wet patch -- so the atlas can be judged before any mod paints.
// Presentation only (the view is this machine's; nothing reaches the playsim). Not saved.
CVARD(Bool, r_damage_test, false, CVAR_GLOBALCONFIG, "paints wall damage where the view looks, every quarter second -- a test of the atlas (Vulkan only)")

namespace
{
	const double kPi = 3.14159265358979323846;

	// hw_smokevolume.cpp's ReadQueue (the smoke lane's file; hw_debrispool.cpp copies it the same way): every event of one of
	// the level's queues not read before, the older generation first, from this reader's own cursor. Nothing is written.
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

	const secplane_t& PlaneOf(const sector_t* sector, int plane)
	{
		return plane == 1 ? sector->ceilingplane : sector->floorplane;
	}
}

// The surface a paint lands on, and its (u, v) frame now.
struct SurfaceDamage::PaintTarget
{
	uint32_t Key = 0;
	int AnchorSector = -1;
	int AnchorPlane = 0;
	double Anchor[4] = {};
	double U[4] = {};
	double V[4] = {};
	// The part's extent in (u, v) at the hit, map units; a flat is unbounded.
	double UMin = -1e30, UMax = 1e30, VMin = -1e30, VMax = 1e30;
	DVector3 Hit;
	FTextureID Texture;
};

int SurfaceDamageWallKey(const side_t* side, int part)
{
	return SurfaceDamage::Get().WallKey(side, part);
}

int SurfaceDamageFlatKey(const sector_t* sector, bool ceiling)
{
	return SurfaceDamage::Get().FlatKey(sector, ceiling);
}

SurfaceDamage& SurfaceDamage::Get()
{
	static SurfaceDamage damage;
	return damage;
}

int SurfaceDamage::WallKey(const side_t* side, int part) const
{
	if (!mDrawable || side == nullptr || part < 0 || part > 2)
		return 0;
	const auto it = mKeyToSurface.find(((uint32_t)side->Index() << 2) | (uint32_t)part);
	return it != mKeyToSurface.end() ? it->second + 1 : 0;
}

int SurfaceDamage::FlatKey(const sector_t* sector, bool ceiling) const
{
	if (!mDrawable || sector == nullptr)
		return 0;
	const auto it = mKeyToSurface.find(0x80000000u | ((uint32_t)sector->Index() << 1) | (ceiling ? 1u : 0u));
	return it != mKeyToSurface.end() ? it->second + 1 : 0;
}

//-----------------------------------------------------------------------------
//
// The frame
//
//-----------------------------------------------------------------------------

void SurfaceDamage::PrepareFrame(FLevelLocals* Level, double eyeX, double eyeY, double eyeZ, double yaw, double pitch, uint64_t levelSerial)
{
	SurfaceDamageFrame& out = SurfaceDamageFrameForBackend();
	out = SurfaceDamageFrame();
	out.Serial = ++mFrameSerial;
	mDrawable = false;
	mStamps.clear();
	mClearTiles.clear();
	mStampsOut.clear();
	mStampTilesOut.clear();
	mCoolTilesOut.clear();
	if (Level == nullptr)
		return;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	const int maptime = Level->maptime;
	// Renderer-read every frame. Vulkan only: GL and GLES draw no damage, so nothing here runs for them.
	const bool vulkan = screen != nullptr && screen->IsVulkan();
	const bool on = vulkan && r_damage;
	const int pages = SurfaceDamagePagesFor(r_damage_memory);
	const SurfaceDamageBackendStatus& status = SurfaceDamageStatus();
	const bool refused = status.ProgramsFailed || (status.RefusedPages != 0 && status.RefusedPages == pages);

	if (levelSerial != mLevelSerial)
	{
		// A new map or a savegame load: the old map's damage is gone (ClearLevelData emptied the queue). An atlas in use keeps
		// lingering into the new map, so it is not freed now and made again at the new map's first paint.
		mLevelSerial = levelSerial;
		mCursor = QueueCursor();
		if (mPages != 0)
			Reset();
		mCoolClockValid = false;
		mStampsFullLogged = false;
		mTileStampsLogged = false;
		mUnknownBrushesLogged.clear();
		mTestNextTic = 0;
		if (mHasDemand)
			mLastPaintTime = maptime;
	}

	// The level's new paints. Always read, so the cursor keeps up; kept only while this machine draws damage. The events stay
	// in the level's queue until its next tic, so pointers to them hold for this frame.
	mPending.clear();
	ReadQueue(Level->SurfaceDamagePaints, mCursor.Serial, mCursor.Count, [&](const FSurfaceDamagePaintEvent& paint, int)
	{
		mPending.push_back(&paint);
	});

	if (!on || refused)
	{
		// Nothing is kept while this machine does not draw damage: out.Active stays false and the backend frees the atlas.
		mPending.clear();
		if (mPages != 0)
			Release();
		mHasDemand = false;
		return;
	}

	// THE DESK TEST (r_damage_test): a paint of its own, when one is due. A paused game (maptime frozen) paints nothing more.
	FSurfaceDamagePaintEvent testPaint;
	bool hasTest = false;
	if (r_damage_test && maptime >= mTestNextTic)
	{
		hasTest = MakeTestPaint(Level, eyeX, eyeY, eyeZ, yaw, pitch, testPaint);
		mTestNextTic = maptime + TEST_TICS;
	}

	// Who asks for the atlas: a paint, or tiles in use (damage lasts the map), or a paint within the linger.
	if (!mPending.empty() || hasTest)
	{
		mHasDemand = true;
		mLastPaintTime = maptime;
	}
	if (maptime < mLastPaintTime)
		mLastPaintTime = maptime;
	if (mHasDemand && mTilesInUse == 0 && maptime - mLastPaintTime >= LINGER_SECONDS * TICRATE)
		mHasDemand = false;
	if (!mHasDemand && mTilesInUse == 0)
	{
		// INERT: a map nobody paints holds nothing on the CPU or the GPU, and this is the whole cost of a frame.
		if (mPages != 0)
			Release();
		return;
	}

	if (mDefinitionsGeneration == 0 || mDefinitionsGeneration != SurfaceDamageDefinitions().Generation)
	{
		// The first paint this session: DAMAGEDEFS (and the images it names) are read now.
		if (SurfaceDamageDefinitions().Generation == 0)
			LoadSurfaceDamageDefinitions();
		mDefinitionsGeneration = SurfaceDamageDefinitions().Generation;
		if (mPages != 0)
			WriteLooks();
	}

	// THE MEMORY. A new size starts from nothing (the pages are made again).
	if (pages != mPages)
	{
		mPages = pages;
		Reset();
	}

	// THE PAINTS.
	for (const FSurfaceDamagePaintEvent* paint : mPending)
		Paint(Level, *paint);
	mPending.clear();
	if (hasTest)
		Paint(Level, testPaint);

	UpdateAnchors(Level);
	Cool(maptime, out);
	BuildDispatch(out);

	// The renderer-read scales, every frame (the menu's sliders respond while it is open).
	mData[SURFACE_DAMAGE_DATA_SCALES * 4 + 0] = (float)r_damage_soot_scale;
	mData[SURFACE_DAMAGE_DATA_SCALES * 4 + 1] = (float)r_damage_depth_scale;
	mData[SURFACE_DAMAGE_DATA_SCALES * 4 + 2] = r_damage_heat ? (float)r_damage_heat_scale : 0.f;
	mData[SURFACE_DAMAGE_DATA_SCALES * 4 + 3] = r_damage_debug ? 1.f : 0.f;
	MarkDirty(SURFACE_DAMAGE_DATA_SCALES, SURFACE_DAMAGE_DATA_SCALES + 1);

	const SurfaceDamageDefinitionSet& defs = SurfaceDamageDefinitions();
	out.Active = true;
	out.Pages = mPages;
	out.HashEntries = mHashEntries;
	out.Data = mData.data();
	out.DataVec4s = (int)(mData.size() / 4);
	out.DirtyFirst = mDirtyFirst;
	out.DirtyEnd = mDirtyEnd;
	out.DataGeneration = mDataGeneration;
	out.Stamps = mStampsOut.data();
	out.StampCount = (int)mStampsOut.size();
	out.StampTiles = mStampTilesOut.data();
	out.StampTileCount = (int)mStampTilesOut.size();
	out.BrushPixels = defs.BrushPixels.Data();
	out.BrushLayers = defs.BrushLayers;
	out.BrushGeneration = defs.Generation;
	out.DetailPixels = defs.DetailPixels.Data();
	out.DetailLayers = defs.DetailLayers;
	out.DetailGeneration = defs.Generation;
	mDirtyFirst = 0;
	mDirtyEnd = 0;

	// THE DRAW KEYS, for this frame's draws: only while this frame's descriptor sets hold the atlas this data describes. A frame
	// in which the backend makes the atlas (or makes it again at a new size) draws no damage; the next one does.
	mDrawable = status.Bound && status.Allocated && status.Pages == mPages && !mLiveSurfaces.empty();

	if (timed)
		PerfLog::AddCpuSample("fx.damagepaint", (double)(I_nsTime() - startNs) / 1e6);
}

// Nothing held: every tile and surface free, the hash empty, the data zeroed but for the layout and the looks. The backend
// uploads the whole data buffer when DataGeneration moves.
void SurfaceDamage::Reset()
{
	const int tiles = mPages * SURFACE_DAMAGE_TILES_PER_PAGE;
	mHashEntries = SurfaceDamageHashEntriesFor(tiles);
	mTiles.assign((size_t)tiles, Tile());
	mFreeTiles.resize((size_t)tiles);
	for (int i = 0; i < tiles; i++)
		mFreeTiles[(size_t)i] = tiles - 1 - i;	// popped from the back: tile 0 first
	mLruHead = mLruTail = -1;
	mTilesInUse = 0;
	mHash.assign((size_t)mHashEntries, -1);
	mSurfaces.assign((size_t)SURFACE_DAMAGE_SURFACE_CAPACITY, Surface());
	mFreeSurfaces.resize((size_t)SURFACE_DAMAGE_SURFACE_CAPACITY);
	for (int i = 0; i < SURFACE_DAMAGE_SURFACE_CAPACITY; i++)
		mFreeSurfaces[(size_t)i] = SURFACE_DAMAGE_SURFACE_CAPACITY - 1 - i;
	mLiveSurfaces.clear();
	mKeyToSurface.clear();
	mHot.clear();
	mStamps.clear();
	mClearTiles.clear();
	mData.assign((size_t)SurfaceDamageDataVec4s(mHashEntries) * 4, 0.f);
	mData[SURFACE_DAMAGE_DATA_LAYOUT * 4] = (float)mHashEntries;
	WriteLooks();
	mDataGeneration++;
	mDirtyFirst = 0;
	mDirtyEnd = (int)(mData.size() / 4);
}

// Off, refused, or no longer asked for: the CPU copies go too, so a machine that does not draw damage holds none of it.
void SurfaceDamage::Release()
{
	mPages = 0;
	mHashEntries = 0;
	std::vector<Tile>().swap(mTiles);
	std::vector<int32_t>().swap(mFreeTiles);
	std::vector<int32_t>().swap(mHash);
	std::vector<Surface>().swap(mSurfaces);
	std::vector<int32_t>().swap(mFreeSurfaces);
	std::vector<int32_t>().swap(mLiveSurfaces);
	std::unordered_map<uint32_t, int32_t>().swap(mKeyToSurface);
	std::vector<float>().swap(mData);
	std::vector<HotTile>().swap(mHot);
	mLruHead = mLruTail = -1;
	mTilesInUse = 0;
	mDirtyFirst = mDirtyEnd = 0;
	mDataGeneration++;
}

void SurfaceDamage::WriteLooks()
{
	if (mData.empty())
		return;
	const SurfaceDamageDefinitionSet& defs = SurfaceDamageDefinitions();
	for (unsigned i = 0; i < (unsigned)SURFACE_DAMAGE_LOOKS; i++)
	{
		float* at = &mData[((size_t)SURFACE_DAMAGE_DATA_LOOKS + (size_t)i * 4) * 4];
		if (i < defs.Looks.Size())
		{
			const SurfaceDamageLook& look = defs.Looks[i];
			memcpy(at + 0, look.Rim, 4 * sizeof(float));
			memcpy(at + 4, look.Inside, 4 * sizeof(float));
			memcpy(at + 8, look.Detail, 4 * sizeof(float));
			memcpy(at + 12, look.Finish, 4 * sizeof(float));
		}
		else
		{
			memset(at, 0, 16 * sizeof(float));
		}
	}
	MarkDirty(SURFACE_DAMAGE_DATA_LOOKS, SURFACE_DAMAGE_DATA_SURFACES);
}

void SurfaceDamage::MarkDirty(int first, int end)
{
	if (mDirtyEnd <= mDirtyFirst)
	{
		mDirtyFirst = first;
		mDirtyEnd = end;
		return;
	}
	mDirtyFirst = std::min(mDirtyFirst, first);
	mDirtyEnd = std::max(mDirtyEnd, end);
}

// A surface record in shader axes: a position is (x, height, y) there.
void SurfaceDamage::WriteSurface(int slot)
{
	const Surface& s = mSurfaces[(size_t)slot];
	float* at = &mData[((size_t)SURFACE_DAMAGE_DATA_SURFACES + (size_t)slot * 2) * 4];
	at[0] = (float)s.U[0]; at[1] = (float)s.U[2]; at[2] = (float)s.U[1]; at[3] = (float)s.U[3];
	at[4] = (float)s.V[0]; at[5] = (float)s.V[2]; at[6] = (float)s.V[1]; at[7] = (float)s.V[3];
	MarkDirty(SURFACE_DAMAGE_DATA_SURFACES + slot * 2, SURFACE_DAMAGE_DATA_SURFACES + slot * 2 + 2);
}

void SurfaceDamage::WriteHashEntry(int entry)
{
	float* at = &mData[((size_t)SURFACE_DAMAGE_DATA_HASH + (size_t)entry) * 4];
	const int tile = mHash[(size_t)entry];
	if (tile < 0)
	{
		at[0] = at[1] = at[2] = at[3] = 0.f;
	}
	else
	{
		const Tile& t = mTiles[(size_t)tile];
		at[0] = (float)(t.Surface + 1);
		at[1] = (float)t.CellU;
		at[2] = (float)t.CellV;
		at[3] = (float)(tile + (int)t.Look * SURFACE_DAMAGE_LOOK_SHIFT);
	}
	MarkDirty(SURFACE_DAMAGE_DATA_HASH + entry, SURFACE_DAMAGE_DATA_HASH + entry + 1);
}

//-----------------------------------------------------------------------------
//
// Tiles and the hash
//
//-----------------------------------------------------------------------------

void SurfaceDamage::Unlink(int tile)
{
	Tile& t = mTiles[(size_t)tile];
	if (t.Prev >= 0) mTiles[(size_t)t.Prev].Next = t.Next; else if (mLruHead == tile) mLruHead = t.Next;
	if (t.Next >= 0) mTiles[(size_t)t.Next].Prev = t.Prev; else if (mLruTail == tile) mLruTail = t.Prev;
	t.Prev = t.Next = -1;
}

// Most recently painted: to the head of the LRU list.
void SurfaceDamage::Touch(int tile)
{
	Tile& t = mTiles[(size_t)tile];
	t.LastPaint = ++mPaintClock;
	if (mLruHead == tile)
		return;
	Unlink(tile);
	t.Next = mLruHead;
	if (mLruHead >= 0) mTiles[(size_t)mLruHead].Prev = tile;
	mLruHead = tile;
	if (mLruTail < 0) mLruTail = tile;
}

void SurfaceDamage::FreeTile(int tile)
{
	Tile& t = mTiles[(size_t)tile];
	if (t.Surface < 0)
		return;
	if (t.HashEntry >= 0)
	{
		mHash[(size_t)t.HashEntry] = -1;
		WriteHashEntry(t.HashEntry);
	}
	Unlink(tile);
	const int slot = t.Surface;
	mSurfaces[(size_t)slot].Tiles--;
	DropSurfaceIfEmpty(slot);
	t.Surface = -1;
	t.HashEntry = -1;
	t.HotUntil = -1;
	t.ClearPending = false;
	mTilesInUse--;
}

// A surface with no tile left: its slot is free (no hash entry names it any more), swap-removed from the live list. Its
// record in the data buffer is left as it was; nothing can reach it until the slot is handed out and written again.
void SurfaceDamage::DropSurfaceIfEmpty(int slot)
{
	Surface& s = mSurfaces[(size_t)slot];
	if (s.Tiles > 0 || s.LiveIndex < 0)
		return;
	mKeyToSurface.erase(s.Key);
	const int last = mLiveSurfaces.back();
	mLiveSurfaces[(size_t)s.LiveIndex] = last;
	mSurfaces[(size_t)last].LiveIndex = s.LiveIndex;
	mLiveSurfaces.pop_back();
	s = Surface();
	mFreeSurfaces.push_back(slot);
}

// A free tile, or the least recently painted one, freed.
int SurfaceDamage::TakeTile()
{
	if (!mFreeTiles.empty())
	{
		const int tile = mFreeTiles.back();
		mFreeTiles.pop_back();
		return tile;
	}
	const int tile = mLruTail;
	if (tile < 0)
		return -1;
	FreeTile(tile);		// counted out here; the caller counts it in again
	mTilesEvicted++;
	return tile;
}

int SurfaceDamage::FindOrAllocateTile(int surface, int32_t cellU, int32_t cellV, int look)
{
	const uint32_t hash = SurfaceDamageHash((uint32_t)surface, cellU, cellV);
	const uint32_t mask = (uint32_t)mHashEntries - 1u;
	int freeEntry = -1;
	for (int probe = 0; probe < SURFACE_DAMAGE_HASH_PROBES; probe++)
	{
		const int entry = (int)((hash + (uint32_t)probe) & mask);
		const int tile = mHash[(size_t)entry];
		if (tile >= 0)
		{
			Tile& t = mTiles[(size_t)tile];
			if (t.Surface == surface && t.CellU == cellU && t.CellV == cellV)
			{
				Touch(tile);
				if (t.Look != (uint8_t)look)
				{
					t.Look = (uint8_t)look;
					WriteHashEntry(entry);
				}
				return tile;
			}
		}
		else if (freeEntry < 0)
		{
			freeEntry = entry;
		}
	}

	// The surface keeps its slot while tiles are freed below, even one of its own.
	mSurfaces[(size_t)surface].Tiles++;

	if (freeEntry < 0)
	{
		// All four probes taken: the least recently painted of their tiles gives up its entry.
		int oldest = -1;
		for (int probe = 0; probe < SURFACE_DAMAGE_HASH_PROBES; probe++)
		{
			const int tile = mHash[(size_t)((hash + (uint32_t)probe) & mask)];
			if (oldest < 0 || mTiles[(size_t)tile].LastPaint < mTiles[(size_t)oldest].LastPaint)
				oldest = tile;
		}
		freeEntry = mTiles[(size_t)oldest].HashEntry;
		FreeTile(oldest);
		mFreeTiles.push_back(oldest);
		mHashEvictions++;
	}

	const int tile = TakeTile();
	if (tile < 0)
	{
		mSurfaces[(size_t)surface].Tiles--;
		return -1;
	}
	Tile& t = mTiles[(size_t)tile];
	t.Surface = surface;
	t.CellU = cellU;
	t.CellV = cellV;
	t.HashEntry = freeEntry;
	t.Generation++;
	t.Look = (uint8_t)look;
	t.HotUntil = -1;
	t.ClearPending = true;
	mClearTiles.push_back(tile);
	mHash[(size_t)freeEntry] = tile;
	mTilesInUse++;
	Touch(tile);
	WriteHashEntry(freeEntry);
	return tile;
}

//-----------------------------------------------------------------------------
//
// Surfaces
//
//-----------------------------------------------------------------------------

int SurfaceDamage::FindOrAddSurface(const PaintTarget& target)
{
	int slot;
	const auto it = mKeyToSurface.find(target.Key);
	if (it != mKeyToSurface.end())
	{
		slot = it->second;
	}
	else
	{
		if (mFreeSurfaces.empty())
			return -1;	// cannot happen while there are fewer tiles than records; kept as a guard
		slot = mFreeSurfaces.back();
		mFreeSurfaces.pop_back();
		mKeyToSurface[target.Key] = slot;
		Surface& s = mSurfaces[(size_t)slot];
		s = Surface();
		s.Key = target.Key;
		s.LiveIndex = (int)mLiveSurfaces.size();
		mLiveSurfaces.push_back(slot);
	}
	Surface& s = mSurfaces[(size_t)slot];
	const bool changed = memcmp(s.U, target.U, sizeof(s.U)) != 0 || memcmp(s.V, target.V, sizeof(s.V)) != 0;
	s.AnchorSector = target.AnchorSector;
	s.AnchorPlane = target.AnchorPlane;
	memcpy(s.Anchor, target.Anchor, sizeof(s.Anchor));
	memcpy(s.U, target.U, sizeof(s.U));
	memcpy(s.V, target.V, sizeof(s.V));
	if (changed)
		WriteSurface(slot);
	return slot;
}

// A wall's v: its pegging anchor's TEXTURE height minus z. The renderer pegs wall textures to a plane's TexZ (GetPlaneTexZ),
// which a slope does not change (HWWall::DoTexture: "slopes don't affect the texture's z-position"), so v is the flat plane
// V = (0, 0, -1, TexZ) and moves exactly when the art does. anchor[0] keeps the TexZ it was written with.
static void AnchorV(const sector_t* sector, int plane, double V[4], double anchor[4])
{
	const double texZ = sector->GetPlaneTexZ(plane == 1 ? sector_t::ceiling : sector_t::floor);
	V[0] = 0.0;
	V[1] = 0.0;
	V[2] = -1.0;
	V[3] = texZ;
	anchor[0] = texZ;
	anchor[1] = anchor[2] = anchor[3] = 0.0;
}

bool SurfaceDamage::FindTarget(FLevelLocals* Level, const FSurfaceDamagePaintEvent& paint, PaintTarget& target)
{
	const DVector3 start = paint.Pos + paint.Normal * TRACE_BACKOFF;
	sector_t* startSector = Level->PointInSector(start.XY());
	FTraceResults res;
	if (!Trace(start, startSector, -paint.Normal, TRACE_DISTANCE, ActorFlags::FromInt(0), 0, nullptr, res, TRACE_HitSky))
		return false;

	if (res.HitType == TRACE_HitWall)
	{
		line_t* line = res.Line;
		if (line == nullptr || res.Tier == TIER_FFloor || res.Side > 1)
			return false;
		side_t* side = line->sidedef[res.Side];
		if (side == nullptr || (side->Flags & WALLF_POLYOBJ) || side->sector == nullptr)
			return false;
		const side_t* other = line->sidedef[1 - res.Side];
		sector_t* front = side->sector;
		sector_t* back = other != nullptr ? other->sector : nullptr;

		int part;
		if (res.Tier == TIER_Upper) part = 0;
		else if (res.Tier == TIER_Lower) part = 2;
		else part = 1;
		if (part == 1 && back != nullptr)
			return false;	// a two-sided middle: masked fences and grates are not painted
		if (part != 1 && back == nullptr)
			return false;
		if (part == 0 && front->GetTexture(sector_t::ceiling) == skyflatnum && back->GetTexture(sector_t::ceiling) == skyflatnum)
			return false;	// a sky upper, which the renderer does not draw
		target.Texture = side->textures[part].texture;
		if (!target.Texture.isValid())
			return false;

		// VANILLA PEGGING, as HWWall::DoTexture computes it: v is measured from the plane the part's texture moves with.
		sector_t* anchorSector;
		int anchorPlane;
		if (part == 0)
		{
			anchorSector = (line->flags & ML_DONTPEGTOP) ? front : back;
			anchorPlane = sector_t::ceiling;
		}
		else if (part == 2)
		{
			if (line->flags & ML_DONTPEGBOTTOM)
			{
				// DoTexture's v_offset: aligned to the front ceiling -- to the back ceiling when both ceilings are sky.
				const bool skies = front->GetTexture(sector_t::ceiling) == skyflatnum && back->GetTexture(sector_t::ceiling) == skyflatnum;
				anchorSector = skies ? back : front;
				anchorPlane = sector_t::ceiling;
			}
			else
			{
				anchorSector = back;
				anchorPlane = sector_t::floor;
			}
		}
		else
		{
			anchorSector = front;
			anchorPlane = (line->flags & ML_DONTPEGBOTTOM) ? sector_t::floor : sector_t::ceiling;
		}

		// u along the side from its own first vertex: v1 for the front side, v2 for the back.
		const bool frontSide = line->sidedef[0] == side;
		const DVector2 from = frontSide ? line->v1->fPos() : line->v2->fPos();
		const DVector2 to = frontSide ? line->v2->fPos() : line->v1->fPos();
		const DVector2 along = to - from;
		const double length = along.Length();
		if (length < 0.01)
			return false;
		const DVector2 dir = along / length;

		target.Key = ((uint32_t)side->Index() << 2) | (uint32_t)part;
		target.AnchorSector = anchorSector->Index();
		target.AnchorPlane = anchorPlane == sector_t::ceiling ? 1 : 0;
		target.U[0] = dir.X;
		target.U[1] = dir.Y;
		target.U[2] = 0.0;
		target.U[3] = -(from.X * dir.X + from.Y * dir.Y);
		AnchorV(anchorSector, target.AnchorPlane, target.V, target.Anchor);

		// The part's extent at the hit: its bottom and top heights there, as v.
		const DVector2 xy = res.HitPos.XY();
		double zLow, zHigh;
		if (part == 0) { zLow = back->ceilingplane.ZatPoint(xy); zHigh = front->ceilingplane.ZatPoint(xy); }
		else if (part == 2) { zLow = front->floorplane.ZatPoint(xy); zHigh = back->floorplane.ZatPoint(xy); }
		else { zLow = front->floorplane.ZatPoint(xy); zHigh = front->ceilingplane.ZatPoint(xy); }
		const double anchorZ = target.V[3];
		target.UMin = 0.0;
		target.UMax = length;
		target.VMin = anchorZ - std::max(zLow, zHigh);
		target.VMax = anchorZ - std::min(zLow, zHigh);
	}
	else if (res.HitType == TRACE_HitFloor || res.HitType == TRACE_HitCeiling)
	{
		sector_t* sector = res.Sector;
		if (sector == nullptr || res.ffloor != nullptr)
			return false;	// a 3D floor's plane is not painted
		const int plane = res.HitType == TRACE_HitCeiling ? sector_t::ceiling : sector_t::floor;
		target.Texture = sector->GetTexture(plane);
		if (!target.Texture.isValid() || target.Texture == skyflatnum)
			return false;
		target.Key = 0x80000000u | ((uint32_t)sector->Index() << 1) | (plane == sector_t::ceiling ? 1u : 0u);
		target.AnchorSector = -1;
		target.U[0] = 1.0; target.U[1] = 0.0; target.U[2] = 0.0; target.U[3] = 0.0;
		target.V[0] = 0.0; target.V[1] = 1.0; target.V[2] = 0.0; target.V[3] = 0.0;
	}
	else
	{
		return false;
	}
	target.Hit = res.HitPos;
	return true;
}

void SurfaceDamage::Paint(FLevelLocals* Level, const FSurfaceDamagePaintEvent& paint)
{
	mPaints++;
	const SurfaceDamageDefinitionSet& defs = SurfaceDamageDefinitions();
	if (defs.Brushes.Size() == 0)
		return;

	const FName brushName = ENamedName(paint.Brush);
	int brushIndex = SurfaceDamageFindBrush(brushName);
	if (brushIndex < 0)
	{
		if (std::find(mUnknownBrushesLogged.begin(), mUnknownBrushesLogged.end(), paint.Brush) == mUnknownBrushesLogged.end())
		{
			mUnknownBrushesLogged.push_back(paint.Brush);
			Printf(TEXTCOLOR_RED "PaintSurfaceDamage: no DAMAGEDEFS brush '%s' -- painted with 'round' (logged once per name per map)\n", brushName.GetChars());
		}
		brushIndex = 0;
	}
	const SurfaceDamageBrush& brush = defs.Brushes[(unsigned)brushIndex];

	if ((int)mStamps.size() >= SURFACE_DAMAGE_STAMPS_PER_FRAME)
	{
		if (!mStampsFullLogged)
		{
			mStampsFullLogged = true;
			Printf("SurfaceDamage: more than %d stamps in one frame -- the rest are dropped (logged once per map)\n", SURFACE_DAMAGE_STAMPS_PER_FRAME);
		}
		return;
	}

	PaintTarget target;
	if (!FindTarget(Level, paint, target))
	{
		mPaintsMissed++;
		return;
	}

	FGameTexture* texture = TexMan.GetGameTexture(target.Texture);
	const int look = texture != nullptr ? std::clamp(SurfaceDamageLookFor(texture->GetSurface()), 0, SURFACE_DAMAGE_LOOKS - 1) : 0;

	const double u0 = target.U[0] * target.Hit.X + target.U[1] * target.Hit.Y + target.U[2] * target.Hit.Z + target.U[3];
	const double v0 = target.V[0] * target.Hit.X + target.V[1] * target.Hit.Y + target.V[2] * target.Hit.Z + target.V[3];

	// THE VARIANT, THE TURN AND THE FLIP: a hash of the paint's own position (the same on every machine), never RNG.
	const uint32_t place = SurfaceDamagePlaceHash(paint.Pos.X, paint.Pos.Y, paint.Pos.Z, brush.Salt);
	const int variant = (int)(place % (uint32_t)std::max(brush.Variants, 1));

	// THE AXIS (ask 1): projected onto the surface's u and v; zero, or square to the surface, is unrotated.
	double cs = 1.0, sn = 0.0;
	const double au = paint.Axis.X * target.U[0] + paint.Axis.Y * target.U[1] + paint.Axis.Z * target.U[2];
	const double av = paint.Axis.X * target.V[0] + paint.Axis.Y * target.V[1] + paint.Axis.Z * target.V[2];
	const double axisLength = std::sqrt(au * au + av * av);
	if (axisLength > 1e-4)
	{
		cs = au / axisLength;
		sn = av / axisLength;
	}
	else if (brush.TurnHashed)
	{
		const double angle = SurfaceDamagePlaceHash(paint.Pos.X, paint.Pos.Y, paint.Pos.Z, brush.Salt ^ 0x5BD1E995u) / 4294967296.0 * 2.0 * kPi;
		cs = std::cos(angle);
		sn = std::sin(angle);
	}
	const bool flip = brush.FlipHashed && (SurfaceDamagePlaceHash(paint.Pos.X, paint.Pos.Y, paint.Pos.Z, brush.Salt ^ 0x1B873593u) & 0x80000000u) != 0;

	// THE CELLS: the rotated brush square's box, plus the gutter, clipped to the part.
	const double radius = std::clamp(paint.Radius, SURFACE_DAMAGE_RADIUS_MIN, SURFACE_DAMAGE_RADIUS_MAX);
	const double gutterUnits = (double)SURFACE_DAMAGE_GUTTER / SURFACE_DAMAGE_TEXELS_PER_UNIT;
	const double reach = radius * (std::fabs(cs) + std::fabs(sn)) + gutterUnits;
	const double uLo = std::max(u0 - reach, target.UMin - gutterUnits);
	const double uHi = std::min(u0 + reach, target.UMax + gutterUnits);
	const double vLo = std::max(v0 - reach, target.VMin - gutterUnits);
	const double vHi = std::min(v0 + reach, target.VMax + gutterUnits);
	if (uLo > uHi || vLo > vHi)
	{
		mPaintsMissed++;
		return;
	}
	const int32_t cu0 = SurfaceDamageCellOf(uLo), cu1 = SurfaceDamageCellOf(uHi);
	const int32_t cv0 = SurfaceDamageCellOf(vLo), cv1 = SurfaceDamageCellOf(vHi);

	const int surface = FindOrAddSurface(target);
	if (surface < 0)
		return;

	const double radiusTexels = radius * SURFACE_DAMAGE_TEXELS_PER_UNIT;
	const float lod = (float)std::clamp(std::log2(SURFACE_DAMAGE_BRUSH_TEXELS / (2.0 * radiusTexels)), 0.0, (double)(SURFACE_DAMAGE_BRUSH_MIPS - 1));
	int cells = 0;
	for (int32_t cv = cv0; cv <= cv1 && cells < SURFACE_DAMAGE_MAX_CELLS_PER_PAINT; cv++)
	{
		for (int32_t cu = cu0; cu <= cu1 && cells < SURFACE_DAMAGE_MAX_CELLS_PER_PAINT; cu++)
		{
			cells++;
			if ((int)mStamps.size() >= SURFACE_DAMAGE_STAMPS_PER_FRAME)
				break;
			const int tile = FindOrAllocateTile(surface, cu, cv, look);
			if (tile < 0)
				continue;
			Stamp stamp;
			stamp.Tile = tile;
			stamp.Generation = mTiles[(size_t)tile].Generation;
			stamp.Gpu.Place[0] = (float)(SURFACE_DAMAGE_GUTTER + (u0 - (double)cu * SURFACE_DAMAGE_CELL_UNITS) * SURFACE_DAMAGE_TEXELS_PER_UNIT);
			stamp.Gpu.Place[1] = (float)(SURFACE_DAMAGE_GUTTER + (v0 - (double)cv * SURFACE_DAMAGE_CELL_UNITS) * SURFACE_DAMAGE_TEXELS_PER_UNIT);
			stamp.Gpu.Place[2] = (float)cs;
			stamp.Gpu.Place[3] = (float)sn;
			stamp.Gpu.Shape[0] = (float)(1.0 / radiusTexels);
			stamp.Gpu.Shape[1] = (float)(brush.FirstLayer + variant);
			stamp.Gpu.Shape[2] = flip ? 1.f : 0.f;
			stamp.Gpu.Shape[3] = lod;
			stamp.Gpu.Amount[0] = (float)paint.Depth;
			stamp.Gpu.Amount[1] = (float)paint.Soot;
			stamp.Gpu.Amount[2] = (float)paint.Heat;
			stamp.Gpu.Amount[3] = (float)paint.Wet;
			mStamps.push_back(stamp);

			if (paint.Heat > 0.0)
			{
				Tile& t = mTiles[(size_t)tile];
				if (t.HotUntil < 0)
					mHot.push_back({ tile, t.Generation });
				t.HotUntil = mCoolPass + SURFACE_DAMAGE_COOL_PASSES;
			}
		}
	}
	// Every cell refused (the frame's stamps ran out): the surface holds nothing.
	DropSurfaceIfEmpty(surface);
}

// r_damage_test's paint: the surface at the centre of the main view. The kind cycles hole, hot hit, scorch, wet patch; the brush
// cycles through every DAMAGEDEFS brush, one per round of four; every other hole and hot hit is laid along the view's heading
// (the axis argument).
bool SurfaceDamage::MakeTestPaint(FLevelLocals* Level, double eyeX, double eyeY, double eyeZ, double yaw, double pitch, FSurfaceDamagePaintEvent& paint)
{
	if (SurfaceDamageDefinitions().Generation == 0)
		LoadSurfaceDamageDefinitions();
	const SurfaceDamageDefinitionSet& defs = SurfaceDamageDefinitions();
	if (defs.Brushes.Size() == 0)
		return false;

	const DVector3 eye(eyeX, eyeY, eyeZ);
	const DVector3 dir(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch));
	FTraceResults res;
	if (!Trace(eye, Level->PointInSector(eye.XY()), dir, 2048.0, ActorFlags::FromInt(0), 0, nullptr, res, TRACE_HitSky))
		return false;
	DVector3 normal;
	if (res.HitType == TRACE_HitWall && res.Line != nullptr && res.Side <= 1)
	{
		// A line's front side is on the right of v1 -> v2.
		const DVector2 along = res.Line->Delta().Unit();
		normal = DVector3(along.Y, -along.X, 0.0);
		if (res.Side == 1)
			normal = -normal;
	}
	else if (res.HitType == TRACE_HitFloor)
		normal = DVector3(0.0, 0.0, 1.0);
	else if (res.HitType == TRACE_HitCeiling)
		normal = DVector3(0.0, 0.0, -1.0);
	else
		return false;

	const int kind = mTestCount % 4;
	const unsigned brush = (unsigned)((mTestCount / 4) % (int)defs.Brushes.Size());
	mTestCount++;
	paint = FSurfaceDamagePaintEvent();
	paint.Pos = res.HitPos;
	paint.Normal = normal;
	paint.Brush = defs.Brushes[brush].Name.GetIndex();
	paint.Axis = (kind <= 1 && (mTestCount / 4) % 2 == 1) ? DVector3(std::cos(yaw), std::sin(yaw), 0.0) : DVector3(0.0, 0.0, 0.0);
	switch (kind)
	{
	case 0: paint.Radius = 1.5; paint.Depth = 0.8; paint.Soot = 0.1; break;						// a bullet hole
	case 1: paint.Radius = 3.0; paint.Depth = 0.6; paint.Soot = 0.15; paint.Heat = 1.0; break;	// a hot hit: glows, then cools
	case 2: paint.Radius = 14.0; paint.Soot = 0.9; break;										// scorch
	default: paint.Radius = 10.0; paint.Wet = 1.0; break;										// a wet patch
	}
	return true;
}

// A wall record follows its anchor: rewritten when the anchor plane's texture height moved since it was written (a door, a
// lift, a crusher). The renderer interpolates TexZ during a frame, and this runs inside the frame, so holes move smoothly.
void SurfaceDamage::UpdateAnchors(FLevelLocals* Level)
{
	const int sectors = (int)Level->sectors.Size();
	for (int32_t slot : mLiveSurfaces)
	{
		Surface& s = mSurfaces[(size_t)slot];
		if (s.AnchorSector < 0 || s.AnchorSector >= sectors)
			continue;
		const sector_t* sector = &Level->sectors[(unsigned)s.AnchorSector];
		if (sector->GetPlaneTexZ(s.AnchorPlane == 1 ? sector_t::ceiling : sector_t::floor) == s.Anchor[0])
			continue;
		AnchorV(sector, s.AnchorPlane, s.V, s.Anchor);
		WriteSurface(slot);
	}
}

// Every COOL_TICS world tics a cooling pass; two due in one frame are folded into one dispatch
// (keep^2, sub x (1 + keep) -- the same as two passes while the heat stays above zero).
void SurfaceDamage::Cool(int maptime, SurfaceDamageFrame& out)
{
	if (!mCoolClockValid || maptime < mLastCoolTic)
	{
		mCoolClockValid = true;
		mLastCoolTic = maptime;
		return;
	}
	const int due = maptime / SURFACE_DAMAGE_COOL_TICS - mLastCoolTic / SURFACE_DAMAGE_COOL_TICS;
	mLastCoolTic = maptime;
	const int passes = std::clamp(due, 0, 2);
	if (passes == 0)
		return;
	mCoolPass += passes;

	size_t kept = 0;
	for (size_t i = 0; i < mHot.size(); i++)
	{
		const HotTile hot = mHot[i];
		Tile& t = mTiles[(size_t)hot.Tile];
		if (t.Surface < 0 || t.Generation != hot.Generation || t.HotUntil < 0)
			continue;
		// A tile stamped this frame is cooled after its stamps (the backend's order), which is right: the pass is due now.
		mCoolTilesOut.push_back({ hot.Tile, 0, 0, 0 });
		if (t.HotUntil < mCoolPass)
			t.HotUntil = -1;	// its last pass: listed no more
		else
			mHot[kept++] = hot;
	}
	mHot.resize(kept);
	if (mCoolTilesOut.empty())
		return;
	if ((int)mCoolTilesOut.size() > SURFACE_DAMAGE_TILES_PER_FRAME)
		mCoolTilesOut.resize((size_t)SURFACE_DAMAGE_TILES_PER_FRAME);	// the rest cool at the next pass
	out.CoolTiles = mCoolTilesOut.data();
	out.CoolTileCount = (int)mCoolTilesOut.size();
	const float keep = SURFACE_DAMAGE_COOL_KEEP;
	out.CoolKeep = passes == 2 ? keep * keep : keep;
	out.CoolSub = passes == 2 ? SURFACE_DAMAGE_COOL_SUB * (1.0f + keep) : SURFACE_DAMAGE_COOL_SUB;
}

// The frame's stamps grouped by tile, so a dispatch writes each tile once; a tile handed out this frame is cleared first, even
// with no stamp left (its stamps can all be dropped). A stamp whose tile was handed out again later this frame is dropped.
void SurfaceDamage::BuildDispatch(SurfaceDamageFrame& out)
{
	std::stable_sort(mStamps.begin(), mStamps.end(), [](const Stamp& a, const Stamp& b) { return a.Tile < b.Tile; });
	size_t i = 0;
	while (i < mStamps.size())
	{
		const int tile = mStamps[i].Tile;
		Tile& t = mTiles[(size_t)tile];
		SurfaceDamageTileGpu record = { tile, (int32_t)mStampsOut.size(), 0, 0 };
		for (; i < mStamps.size() && mStamps[i].Tile == tile; i++)
		{
			if (t.Surface < 0 || mStamps[i].Generation != t.Generation)
				continue;
			if (record.Count >= SURFACE_DAMAGE_STAMPS_PER_TILE)
			{
				if (!mTileStampsLogged)
				{
					mTileStampsLogged = true;
					Printf("SurfaceDamage: more than %d stamps on one tile in one frame -- the rest are dropped (logged once per map)\n", SURFACE_DAMAGE_STAMPS_PER_TILE);
				}
				continue;
			}
			mStampsOut.push_back(mStamps[i].Gpu);
			record.Count++;
		}
		if (t.Surface < 0)
			continue;
		if (t.ClearPending)
		{
			record.Flags |= SURFACE_DAMAGE_TILE_CLEAR;
			t.ClearPending = false;
		}
		if (record.Count > 0 || (record.Flags & SURFACE_DAMAGE_TILE_CLEAR))
			mStampTilesOut.push_back(record);
	}
	for (int32_t tile : mClearTiles)
	{
		Tile& t = mTiles[(size_t)tile];
		if (t.Surface >= 0 && t.ClearPending)
		{
			mStampTilesOut.push_back({ tile, (int32_t)mStampsOut.size(), 0, SURFACE_DAMAGE_TILE_CLEAR });
			t.ClearPending = false;
		}
	}
	if ((int)mStampTilesOut.size() > SURFACE_DAMAGE_TILES_PER_FRAME)
		mStampTilesOut.resize((size_t)SURFACE_DAMAGE_TILES_PER_FRAME);
	(void)out;
}

//-----------------------------------------------------------------------------
//
// The report
//
//-----------------------------------------------------------------------------

FString SurfaceDamage::Report() const
{
	const SurfaceDamageDefinitionSet& defs = SurfaceDamageDefinitions();
	const SurfaceDamageBackendStatus& status = SurfaceDamageStatus();
	FString out;
	out.AppendFormat("Surface damage: r_damage %d, r_damage_memory %d MB (%d pages, %d tiles), r_damage_heat %d, scales soot %g depth %g heat %g, r_damage_debug %d\n",
		(int)r_damage, (int)r_damage_memory, SurfaceDamagePagesFor(r_damage_memory), SurfaceDamagePagesFor(r_damage_memory) * SURFACE_DAMAGE_TILES_PER_PAGE,
		(int)r_damage_heat, (double)(float)r_damage_soot_scale, (double)(float)r_damage_depth_scale, (double)(float)r_damage_heat_scale, (int)r_damage_debug);
	if (defs.Generation == 0)
	{
		out += "  DAMAGEDEFS: not read yet (read the first time damage is drawn)\n";
	}
	else
	{
		out.AppendFormat("  DAMAGEDEFS: %u lump%s, %d block%s, %d refused (%.1f ms)\n", defs.Lumps, defs.Lumps == 1 ? "" : "s", defs.Blocks, defs.Blocks == 1 ? "" : "s", defs.Refused, defs.LoadMs);
		for (const SurfaceDamageBrush& b : defs.Brushes)
		{
			out.AppendFormat("  brush %-16s %d variant%s (layers %d..%d)%s%s -- %s", b.Name.GetChars(), b.Variants, b.Variants == 1 ? "" : "s", b.FirstLayer, b.FirstLayer + b.Variants - 1,
				b.TurnHashed ? ", turn hashed" : "", b.FlipHashed ? ", flip hashed" : "", b.Lump.IsEmpty() ? "built in\n" : "");
			if (!b.Lump.IsEmpty()) out.AppendFormat("%s, line %d\n", b.Lump.GetChars(), b.Line);
		}
		for (unsigned i = 0; i < defs.Looks.Size(); i++)
		{
			const SurfaceDamageLook& l = defs.Looks[i];
			out.AppendFormat("  look %2u %-12s rim %.2f %.2f %.2f x %.2f, inside %.2f %.2f %.2f x %.2f, detail layer %d every %g units x %.2f, rim width %g, wet %g, bend %g, depth %g -- %s",
				i, l.Surface == NAME_None ? "default" : l.Surface.GetChars(), l.Rim[0], l.Rim[1], l.Rim[2], l.Rim[3], l.Inside[0], l.Inside[1], l.Inside[2], l.Inside[3],
				(int)l.Detail[0], l.Detail[1], l.Detail[2], l.Detail[3], l.Finish[0], l.Finish[2], l.Finish[3], l.Lump.IsEmpty() ? "built in\n" : "");
			if (!l.Lump.IsEmpty()) out.AppendFormat("%s, line %d\n", l.Lump.GetChars(), l.Line);
		}
	}
	if (mPages == 0)
	{
		out += "  atlas: not held (nothing painted, damage off, or not Vulkan)\n";
	}
	else
	{
		out.AppendFormat("  atlas: %d of %d tiles in use on %d surfaces, hash %d entries; GPU %s%s; this map: %llu paints, %llu missed a surface, %llu tiles reused, %llu hash evictions\n",
			mTilesInUse, (int)mTiles.size(), (int)mLiveSurfaces.size(), mHashEntries, status.Allocated ? "allocated" : "not allocated", status.Bound ? " and bound" : "",
			(unsigned long long)mPaints, (unsigned long long)mPaintsMissed, (unsigned long long)mTilesEvicted, (unsigned long long)mHashEvictions);
	}
	return out;
}

CCMD(damagebrushes)
{
	Printf("%s", SurfaceDamage::Get().Report().GetChars());
}
