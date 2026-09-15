/*
** hw_smoketilecover.h
**
** [SMOKELIGHTCULL] E6: light only the smoke that exists ("Engine docs/EFFECTS_OPTIMIZATION_PLAN.md" E6,
** "Engine docs/OPTIMIZATION_E9_E1_E6_IMPL_NOTES.md").
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** The smoke light grid (13d, shaders/compute/smoke_light.comp) is filled on every frame with smoke to draw: pass 0 writes the
** ambient light into EVERY cell of the grid, pass 1 runs once per dynamic light over its sphere's box, pass 2 once over the effect
** lights' reach. Passes 1 and 2 already skip each cell of a quiet tile (TileActive 0) inside the shader, but they are dispatched
** over the whole box, and pass 0 rewrites millions of cells that hold exactly what they held the frame before.
**
** This is the CPU side of a fill that dispatches only what can change and leaves the grid bit for bit what the whole fill leaves.
** It knows nothing of Vulkan: VkSmokeVolume tells it the tile map work it records and asks it for boxes. Any backend that fills
** the same grid can use it the same way.
**
** THE TILE MIRROR (SmokeTileSet, a byte a tile). The CPU cannot read TileActive the frame it is made, so it keeps a SUPERSET of
** it, from the work smoke_tiles.comp and smoke_advect.comp are recorded with:
**   - a clear: TileActive is cleared to 0. Exact.
**   - a kernel's mark before a step: tiles [min, max) are set to 1. Exact.
**   - a step: the content pass scans only tiles active during the step (marks included), smoke_advect.comp writes every other
**     tile empty, and the spread pass sets each tile next to a tile with content. So the new map lies inside the SPREAD (the tile
**     and its 26 neighbours) of the map during the step.
**   - a recentre: TileActive is set to 1, content is scanned over the moved state, then spread. Anything above the empty line was
**     in a tile with content, so in an active tile, before the move: the new map lies inside the spread of the moved map.
**   - a readback: after a frame's tile work the backend copies TileActive to the host, and the next frame, before any tile work,
**     the mirror takes it exactly. So the mirror is never wider than the truth by more than this frame's steps (at most 2).
**
** THREE MORE MAPS, about the values pass 0 reads:
**   - Nonzero: tiles whose density or soot may be above 0. After a step, only tiles active during it (smoke_advect.comp writes
**     vec4(0) everywhere else); a mark adds its tiles (a kernel writes in place).
**   - Changed: tiles whose density or soot may have changed since the last fill: each step's active tiles, and the Nonzero tiles
**     the step may have emptied.
**   - Written: tiles that may hold more than pass 0's quiet-tile value: the tiles active at the last fill (only active tiles are
**     graded by 13f's surface light, and passes 1 and 2 add only to active tiles).
**
** PASS 0 OVER ONLY WHAT CAN CHANGE. A quiet tile's cell holds, from pass 0 alone, a function of its column's ambient texel, the
** look's ambient, whether soot is live, and the soot darkness sampled at its centre -- a trilinear footprint inside its own tile
** and (kept for safety) the next. So pass 0 is dispatched over
**     Active | Written | spread(Changed)
** and over the whole grid whenever anything else it reads changed (the AmbientKey: the ambient columns' serial, the look's
** ambient, soot live, the grid), after a recentre, a clear or a new grid, and whenever the cull was off or the mirror unusable.
** Every other cell already holds what pass 0 would write: nothing it is made from changed since it was written.
**
** PASSES 1 AND 2 OVER ONLY ACTIVE TILES. A light's box (pass 2's region) is cut into boxes covering the mirror's active tiles in
** it; a light whose box holds none is not dispatched. The shader's own per-cell tile test stays, so a box wider than the truth
** changes nothing.
**
** THE WHOLE FILL SKIPPED when nothing it reads changed since the last fill: no step, mark, recentre or clear, the same AmbientKey,
** and the same light inputs byte for byte (the backend builds them: the light records, the surface light, the shadow map's serial
** when a light has a row). Never with effect lights: their pass has to be taken back out of the grid the next frame.
**
** A light cell reads and writes only itself, so cutting a dispatch into boxes changes no value, and lights keep their order.
** Renderer-side and local: nothing here reads or writes playsim state.
*/

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// [SMOKELIGHTCULL] A box of tiles or of light cells: [Min, Max) on each axis.
struct SmokeTileBox
{
	int Min[3] = { 0, 0, 0 };
	int Max[3] = { 0, 0, 0 };

	bool IsEmpty() const { return Min[0] >= Max[0] || Min[1] >= Max[1] || Min[2] >= Max[2]; }
	uint64_t Count() const
	{
		return IsEmpty() ? 0 : (uint64_t)(Max[0] - Min[0]) * (uint64_t)(Max[1] - Min[1]) * (uint64_t)(Max[2] - Min[2]);
	}
};

// [SMOKELIGHTCULL] A map of tiles, a byte each (0 or 1, x fastest), and the boxes that cover its set tiles.
class SmokeTileSet
{
public:
	static constexpr int MAX_BOXES = 16;

	void Resize(const int size[3], uint8_t value)
	{
		for (int axis = 0; axis < 3; axis++)
			mSize[axis] = std::max(size[axis], 0);
		mBits.assign((size_t)mSize[0] * (size_t)mSize[1] * (size_t)mSize[2], value);
		mTableValid = false;
	}

	void Release()
	{
		mSize[0] = mSize[1] = mSize[2] = 0;
		mBits.clear();
		mTable.clear();
		mTableValid = false;
	}

	bool IsValid() const { return !mBits.empty(); }
	int Size(int axis) const { return mSize[axis]; }
	bool SameSize(const int size[3]) const { return mSize[0] == size[0] && mSize[1] == size[1] && mSize[2] == size[2]; }
	size_t ByteCount() const { return mBits.size(); }

	void Fill(uint8_t value)
	{
		std::fill(mBits.begin(), mBits.end(), value);
		mTableValid = false;
	}

	void CopyFrom(const SmokeTileSet& other)
	{
		for (int axis = 0; axis < 3; axis++)
			mSize[axis] = other.mSize[axis];
		mBits = other.mBits;
		mTableValid = false;
	}

	// A byte map of the same size, anything but 0 set (TileActive's R8 texels are 0 or 255).
	void CopyBytes(const uint8_t* bytes)
	{
		for (size_t i = 0; i < mBits.size(); i++)
			mBits[i] = bytes[i] != 0 ? 1 : 0;
		mTableValid = false;
	}

	void Or(const SmokeTileSet& other)
	{
		if (other.mBits.size() != mBits.size())
			return;
		for (size_t i = 0; i < mBits.size(); i++)
			mBits[i] |= other.mBits[i];
		mTableValid = false;
	}

	bool Any() const
	{
		for (uint8_t bit : mBits)
		{
			if (bit != 0)
				return true;
		}
		return false;
	}

	// Sets every tile of [boxMin, boxMax), clipped to the map.
	void SetBox(const int boxMin[3], const int boxMax[3])
	{
		int lo[3], hi[3];
		for (int axis = 0; axis < 3; axis++)
		{
			lo[axis] = std::clamp(boxMin[axis], 0, mSize[axis]);
			hi[axis] = std::clamp(boxMax[axis], 0, mSize[axis]);
			if (lo[axis] >= hi[axis])
				return;
		}
		for (int z = lo[2]; z < hi[2]; z++)
		{
			for (int y = lo[1]; y < hi[1]; y++)
			{
				for (int x = lo[0]; x < hi[0]; x++)
					mBits[Index(x, y, z)] = 1;
			}
		}
		mTableValid = false;
	}

	// Every set tile and its 26 neighbours (smoke_tiles.comp's spread): the 3 x 3 x 3 maximum, one axis at a time.
	void Spread()
	{
		for (int axis = 0; axis < 3; axis++)
			SpreadAxis(axis);
		mTableValid = false;
	}

	// new[t] = old[t + by], 0 from outside the map: smoke_shift.comp's move (new origin minus old), in whole tiles.
	void Shift(const int by[3])
	{
		mScratch.assign(mBits.size(), 0);
		for (int z = 0; z < mSize[2]; z++)
		{
			const int fz = z + by[2];
			if (fz < 0 || fz >= mSize[2])
				continue;
			for (int y = 0; y < mSize[1]; y++)
			{
				const int fy = y + by[1];
				if (fy < 0 || fy >= mSize[1])
					continue;
				for (int x = 0; x < mSize[0]; x++)
				{
					const int fx = x + by[0];
					if (fx < 0 || fx >= mSize[0])
						continue;
					mScratch[Index(x, y, z)] = mBits[Index(fx, fy, fz)];
				}
			}
		}
		mBits.swap(mScratch);
		mTableValid = false;
	}

	// At most maxBoxes (and MAX_BOXES) boxes whose union holds every set tile inside `within`; returns how many. Each box is shrunk
	// to its set tiles, then split at the middle of its longest side while fewer than half its tiles are set and the budget lasts.
	int Cover(const SmokeTileBox& within, SmokeTileBox* out, int maxBoxes) const
	{
		maxBoxes = std::min(maxBoxes, (int)MAX_BOXES);
		if (maxBoxes <= 0 || mBits.empty())
			return 0;
		SmokeTileBox start;
		for (int axis = 0; axis < 3; axis++)
		{
			start.Min[axis] = std::clamp(within.Min[axis], 0, mSize[axis]);
			start.Max[axis] = std::clamp(within.Max[axis], 0, mSize[axis]);
		}
		if (start.IsEmpty())
			return 0;
		BuildTable();

		// Emitted plus pending never passes maxBoxes: a box is split only while two more fit.
		SmokeTileBox pending[MAX_BOXES];
		int pendingCount = 0;
		int count = 0;
		pending[pendingCount++] = start;
		while (pendingCount > 0)
		{
			const SmokeTileBox box = Shrink(pending[--pendingCount]);
			if (box.IsEmpty())
				continue;
			int longest = 0;
			for (int axis = 1; axis < 3; axis++)
			{
				if (box.Max[axis] - box.Min[axis] > box.Max[longest] - box.Min[longest])
					longest = axis;
			}
			const int extent = box.Max[longest] - box.Min[longest];
			if (CountIn(box) * 2 >= box.Count() || extent < 2 || count + pendingCount + 2 > maxBoxes)
			{
				out[count++] = box;
				continue;
			}
			SmokeTileBox low = box, high = box;
			low.Max[longest] = high.Min[longest] = box.Min[longest] + extent / 2;
			pending[pendingCount++] = high;
			pending[pendingCount++] = low;
		}
		return count;
	}

private:
	size_t Index(int x, int y, int z) const { return ((size_t)z * (size_t)mSize[1] + (size_t)y) * (size_t)mSize[0] + (size_t)x; }

	void SpreadAxis(int axis)
	{
		mScratch = mBits;
		for (int z = 0; z < mSize[2]; z++)
		{
			for (int y = 0; y < mSize[1]; y++)
			{
				for (int x = 0; x < mSize[0]; x++)
				{
					const size_t here = Index(x, y, z);
					if (mScratch[here] != 0)
						continue;
					int at[3] = { x, y, z };
					uint8_t nearby = 0;
					if (at[axis] > 0)
					{
						at[axis]--;
						nearby |= mScratch[Index(at[0], at[1], at[2])];
						at[axis]++;
					}
					if (at[axis] + 1 < mSize[axis])
					{
						at[axis]++;
						nearby |= mScratch[Index(at[0], at[1], at[2])];
					}
					mBits[here] = nearby;
				}
			}
		}
	}

	// A summed-volume table of the bits, (size + 1) a side: any box's count in eight lookups.
	void BuildTable() const
	{
		if (mTableValid)
			return;
		const size_t sx = (size_t)mSize[0] + 1, sy = (size_t)mSize[1] + 1, sz = (size_t)mSize[2] + 1;
		mTable.assign(sx * sy * sz, 0);
		for (int z = 0; z < mSize[2]; z++)
		{
			for (int y = 0; y < mSize[1]; y++)
			{
				for (int x = 0; x < mSize[0]; x++)
				{
					const int32_t sum = (int32_t)mBits[Index(x, y, z)]
						+ Table(x, y + 1, z + 1) + Table(x + 1, y, z + 1) + Table(x + 1, y + 1, z)
						- Table(x, y, z + 1) - Table(x, y + 1, z) - Table(x + 1, y, z)
						+ Table(x, y, z);
					mTable[((size_t)(z + 1) * sy + (size_t)(y + 1)) * sx + (size_t)(x + 1)] = sum;
				}
			}
		}
		mTableValid = true;
	}

	int32_t Table(int x, int y, int z) const
	{
		return mTable[((size_t)z * ((size_t)mSize[1] + 1) + (size_t)y) * ((size_t)mSize[0] + 1) + (size_t)x];
	}

	uint64_t CountIn(const SmokeTileBox& b) const
	{
		if (b.IsEmpty())
			return 0;
		const int64_t n = (int64_t)Table(b.Max[0], b.Max[1], b.Max[2])
			- Table(b.Min[0], b.Max[1], b.Max[2]) - Table(b.Max[0], b.Min[1], b.Max[2]) - Table(b.Max[0], b.Max[1], b.Min[2])
			+ Table(b.Min[0], b.Min[1], b.Max[2]) + Table(b.Min[0], b.Max[1], b.Min[2]) + Table(b.Max[0], b.Min[1], b.Min[2])
			- Table(b.Min[0], b.Min[1], b.Min[2]);
		return n > 0 ? (uint64_t)n : 0;
	}

	// The box cut down to the slabs that hold a set tile on each axis; an empty box when it holds none.
	SmokeTileBox Shrink(SmokeTileBox box) const
	{
		if (CountIn(box) == 0)
			return SmokeTileBox();
		for (int axis = 0; axis < 3; axis++)
		{
			SmokeTileBox slab = box;
			while (box.Min[axis] < box.Max[axis])
			{
				slab.Min[axis] = box.Min[axis];
				slab.Max[axis] = box.Min[axis] + 1;
				if (CountIn(slab) > 0)
					break;
				box.Min[axis]++;
			}
			while (box.Max[axis] > box.Min[axis])
			{
				slab.Min[axis] = box.Max[axis] - 1;
				slab.Max[axis] = box.Max[axis];
				if (CountIn(slab) > 0)
					break;
				box.Max[axis]--;
			}
		}
		return box;
	}

	int mSize[3] = { 0, 0, 0 };
	std::vector<uint8_t> mBits;
	std::vector<uint8_t> mScratch;
	mutable std::vector<int32_t> mTable;
	mutable bool mTableValid = false;
};

// [SMOKELIGHTCULL] The smoke light grid fill's planner: the tile mirror, the value maps, and what a fill must dispatch (see the top
// of this file).
class SmokeLightCull
{
public:
	static constexpr int PASS0_BOXES = SmokeTileSet::MAX_BOXES;
	static constexpr int LIGHT_BOXES = 8;

	// What pass 0 writes into a quiet tile is made from, beside the smoke itself. Any change: pass 0 over the whole grid.
	struct AmbientKey
	{
		int GridSize[3] = { 0, 0, 0 };	// light cells
		int CellsPerTile = 0;
		double CellSize = 0;
		uint64_t AmbientSerial = 0;		// the ambient columns the GPU holds
		float AmbientScale = 0;
		float SootLive = 0;
	};

	struct Plan
	{
		bool Skip = false;		// the grid already holds this fill's light: dispatch nothing
		bool Full = false;		// pass 0 over the whole grid
		int Pass0Count = 0;		// pass 0's boxes, light cells
		SmokeTileBox Pass0[PASS0_BOXES];
	};

	// ---- The tile map work, told in the order it is recorded. ----

	// The volume was freed: nothing is known until its next clear.
	void Release()
	{
		mActive.Release();
		mNonzero.Release();
		mChanged.Release();
		mWritten.Release();
		mFilled = false;
		mFullNext = true;
		mOpSerial++;
	}

	// A clear (a new volume is cleared too): density, soot and both tile maps are 0.
	void OnClear(const int tiles[3])
	{
		if (!mActive.IsValid() || !mActive.SameSize(tiles))
		{
			mActive.Resize(tiles, 0);
			mNonzero.Resize(tiles, 0);
			mChanged.Resize(tiles, 0);
			mWritten.Resize(tiles, 1);
		}
		else
		{
			mActive.Fill(0);
			mNonzero.Fill(0);
		}
		mFullNext = true;
		mOpSerial++;
	}

	// A recentre by whole tiles, new origin minus old. Every light cell now lies somewhere else: pass 0 over everything next.
	void OnShift(const int byTiles[3])
	{
		if (!mActive.IsValid())
			return;
		mActive.Shift(byTiles);
		mActive.Spread();
		mNonzero.Shift(byTiles);
		mFullNext = true;
		mOpSerial++;
	}

	// A kernel's mark, before a step.
	void OnMark(const int tileMin[3], const int tileMax[3])
	{
		if (!mActive.IsValid())
			return;
		mActive.SetBox(tileMin, tileMax);
		mNonzero.SetBox(tileMin, tileMax);
		mChanged.SetBox(tileMin, tileMax);
		mOpSerial++;
	}

	// A step's advection and its tile passes, after its marks.
	void OnEndStep()
	{
		if (!mActive.IsValid())
			return;
		mChanged.Or(mActive);
		mChanged.Or(mNonzero);
		mNonzero.CopyFrom(mActive);
		mActive.Spread();
		mOpSerial++;
	}

	// ---- The readback. ----

	// Tile work was recorded since the last copy.
	bool WantsCopy() const { return mActive.IsValid() && mOpSerial != mCopySerial; }
	void CopyRecorded() { mCopySerial = mOpSerial; }

	// The copy recorded last (TileActive, R8, x fastest), once its frame has finished. Taken only when no tile work was recorded
	// after it, so a copy can never stand in for a later map.
	void TakeCopy(const uint8_t* bytes, size_t count)
	{
		if (bytes != nullptr && mActive.IsValid() && count == mActive.ByteCount() && mOpSerial == mCopySerial)
			mActive.CopyBytes(bytes);
	}

	// ---- The fill. ----

	// The light grid was made (its cells hold 0): pass 0 over everything next.
	void OnGridMade()
	{
		mFilled = false;
		mFullNext = true;
	}

	// Before a fill. cull false (r_smoke_light_cull off): the whole fill, exactly as before the cull. `inputs`: everything else the
	// fill reads, as bytes; `comparable` false when a fill cannot be judged by them (effect lights).
	void BeginFill(bool cull, const AmbientKey& key, const uint8_t* inputs, size_t inputBytes, bool comparable, Plan& plan)
	{
		plan = Plan();
		mPendingKey = key;
		mPendingInputs.assign(inputs, inputs + inputBytes);
		mPendingComparable = comparable;

		bool usable = cull && mActive.IsValid() && key.CellsPerTile > 0;
		for (int axis = 0; axis < 3 && usable; axis++)
			usable = mActive.Size(axis) * key.CellsPerTile == key.GridSize[axis];
		const bool sameKey = mFilled && SameKey(key, mKey);

		if (usable && sameKey && !mFullNext && mComparable && comparable && !mChanged.Any() && mInputs == mPendingInputs)
		{
			plan.Skip = true;
			return;
		}

		mCull = usable;
		if (!usable || !sameKey || mFullNext)
		{
			plan.Full = true;
			plan.Pass0Count = 1;
			for (int axis = 0; axis < 3; axis++)
				plan.Pass0[0].Max[axis] = std::max(key.GridSize[axis], 0);
			return;
		}

		mScratch.CopyFrom(mChanged);
		mScratch.Spread();
		mScratch.Or(mActive);
		mScratch.Or(mWritten);
		SmokeTileBox all;
		for (int axis = 0; axis < 3; axis++)
			all.Max[axis] = mActive.Size(axis);
		SmokeTileBox tiles[PASS0_BOXES];
		const int count = mScratch.Cover(all, tiles, PASS0_BOXES);
		for (int i = 0; i < count; i++)
		{
			SmokeTileBox cells;
			for (int axis = 0; axis < 3; axis++)
			{
				cells.Min[axis] = tiles[i].Min[axis] * key.CellsPerTile;
				cells.Max[axis] = std::min(tiles[i].Max[axis] * key.CellsPerTile, key.GridSize[axis]);
			}
			if (!cells.IsEmpty())
				plan.Pass0[plan.Pass0Count++] = cells;
		}
	}

	// A pass 1 light's box (or pass 2's region), light cells: the boxes of it over the mirror's active tiles; none when it holds
	// none. The box itself when this fill does not cull.
	int ActiveCells(const SmokeTileBox& cells, SmokeTileBox* out, int maxBoxes) const
	{
		if (cells.IsEmpty() || maxBoxes <= 0)
			return 0;
		if (!mCull)
		{
			out[0] = cells;
			return 1;
		}
		const int perTile = mPendingKey.CellsPerTile;
		SmokeTileBox tileBox;
		for (int axis = 0; axis < 3; axis++)
		{
			tileBox.Min[axis] = std::max(cells.Min[axis], 0) / perTile;
			tileBox.Max[axis] = (std::max(cells.Max[axis], 0) + perTile - 1) / perTile;
		}
		SmokeTileBox tiles[SmokeTileSet::MAX_BOXES];
		const int tileCount = mActive.Cover(tileBox, tiles, std::min(maxBoxes, (int)SmokeTileSet::MAX_BOXES));
		int count = 0;
		for (int i = 0; i < tileCount; i++)
		{
			SmokeTileBox box;
			for (int axis = 0; axis < 3; axis++)
			{
				box.Min[axis] = std::max(tiles[i].Min[axis] * perTile, cells.Min[axis]);
				box.Max[axis] = std::min(tiles[i].Max[axis] * perTile, cells.Max[axis]);
			}
			if (!box.IsEmpty())
				out[count++] = box;
		}
		return count;
	}

	// After a fill that was dispatched (not skipped). comparable false: something the inputs do not show may differ next time
	// (a surface light variant that was refused).
	void EndFill(bool comparable)
	{
		mFilled = true;
		mFullNext = false;
		mKey = mPendingKey;
		mInputs.swap(mPendingInputs);
		mComparable = mPendingComparable && comparable;
		if (mActive.IsValid())
		{
			mWritten.CopyFrom(mActive);
			mChanged.Fill(0);
		}
	}

private:
	static bool SameKey(const AmbientKey& a, const AmbientKey& b)
	{
		return a.GridSize[0] == b.GridSize[0] && a.GridSize[1] == b.GridSize[1] && a.GridSize[2] == b.GridSize[2] &&
			a.CellsPerTile == b.CellsPerTile && memcmp(&a.CellSize, &b.CellSize, sizeof(double)) == 0 &&
			a.AmbientSerial == b.AmbientSerial && memcmp(&a.AmbientScale, &b.AmbientScale, sizeof(float)) == 0 &&
			memcmp(&a.SootLive, &b.SootLive, sizeof(float)) == 0;
	}

	SmokeTileSet mActive;		// superset of TileActive
	SmokeTileSet mNonzero;		// tiles whose density or soot may be above 0
	SmokeTileSet mChanged;		// tiles whose density or soot may have changed since the last fill
	SmokeTileSet mWritten;		// tiles that may hold more than pass 0's quiet-tile value
	SmokeTileSet mScratch;

	uint64_t mOpSerial = 1;		// moves on with every piece of tile work
	uint64_t mCopySerial = 0;	// mOpSerial when the last copy was recorded

	bool mFilled = false;		// a fill finished since the grid was made
	bool mFullNext = true;
	bool mCull = false;			// this fill cuts passes 1 and 2
	AmbientKey mKey, mPendingKey;
	std::vector<uint8_t> mInputs, mPendingInputs;
	bool mComparable = false, mPendingComparable = false;
};
