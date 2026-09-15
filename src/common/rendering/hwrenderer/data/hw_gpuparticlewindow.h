/*
** hw_gpuparticlewindow.h
**
** [PARTICLEWINDOW] E1: draw only the live part of the particle ring ("Engine docs/EFFECTS_OPTIMIZATION_PLAN.md" E1,
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
** The ring draw (hw_drawinfo.cpp, RenderTranslucent) drew every slot of the ring on every scene -- 65536 slots of six vertices,
** per eye -- and gpuparticles.vp put each dead slot on one point outside the clip volume. Those triangles never reached the
** rasteriser, but their vertex shader ran, in a quiet room as in a fight.
**
** THE WINDOW. The ring is cut into chunks of CHUNK_SLOTS slots. For each chunk this keeps the latest level time any record in it
** can still be drawn at (birth + life), worked out again from the CPU ring whenever an upload touches the chunk: after an upload
** the CPU ring and the GPU copy are equal slot for slot (GpuParticleBuffer's sync rule). A chunk whose every record is past its
** life at this scene's level time is not drawn. The chunks that are drawn go out as runs of slots in the ring's own draw order --
** ascending, or oldest first for the premultiplied blend -- merged over the smallest gaps down to MAX_RANGES draws. A long-lived
** record (smoke) keeps only its own chunk drawn, not everything written after it.
**
** WHY THE IMAGE IS THE SAME. gpuparticles.vp's main() collapses a record when life <= 0, age < 0 or age > life, with
** age = uLevelTime.x - birth in float32. A chunk is culled only when every record in it has levelTime > birth + life + margin, the
** margin being 1/256 s plus 2^-20 of the magnitudes: far above the float rounding of `levelTime - birth > life` and of a compiler's
** reassociated `levelTime > birth + life`, so the shader collapses every culled record too. A NaN birth or life makes every test
** false and draws: its chunk is always drawn. Removing triangles that were never rasterised, and keeping the order of the rest
** (primitives within a draw, and draws, are processed in order), leaves blending the same fragments in the same order.
**
** Render-side and local, like the ring itself: nothing here reads or writes playsim state.
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

class GpuParticleWindow
{
public:
	static constexpr unsigned CHUNK_SLOTS = 1024;
	static constexpr int MAX_RANGES = 8;

	struct SlotRange
	{
		unsigned First = 0;
		unsigned Count = 0;
	};

	// A ring of ringSize slots, every one empty (the GPU copy starts zeroed).
	void Resize(unsigned ringSize)
	{
		mRingSize = ringSize;
		mUntil.assign((ringSize + CHUNK_SLOTS - 1) / CHUNK_SLOTS, NEVER);
		mScale.assign(mUntil.size(), 0.0);
	}

	unsigned RingSize() const { return mRingSize; }

	// Slots [first, first + count) of the CPU ring (`size` records, recordBytes each) were just copied to the GPU: each chunk they
	// touch is worked out again over all its slots. Slots at or past `size` were never uploaded and stay zero on the GPU.
	void NoteUpload(const void* records, unsigned size, unsigned first, unsigned count, unsigned recordBytes, unsigned birthOffset, unsigned lifeOffset)
	{
		if (records == nullptr || count == 0 || mUntil.empty())
			return;
		const unsigned firstChunk = first / CHUNK_SLOTS;
		const unsigned lastChunk = std::min((first + count - 1) / CHUNK_SLOTS, (unsigned)mUntil.size() - 1);
		const unsigned end = std::min(size, mRingSize);
		for (unsigned chunk = firstChunk; chunk <= lastChunk; chunk++)
		{
			const unsigned from = chunk * CHUNK_SLOTS;
			const unsigned to = std::min(from + CHUNK_SLOTS, end);
			double until = NEVER, scale = 0.0;
			for (unsigned slot = from; slot < to; slot++)
			{
				const uint8_t* record = (const uint8_t*)records + (size_t)slot * recordBytes;
				float birth, life;
				memcpy(&birth, record + birthOffset, sizeof(float));
				memcpy(&life, record + lifeOffset, sizeof(float));
				const double drawn = DrawnUntil(birth, life);
				if (drawn == NEVER)
					continue;
				until = std::max(until, drawn);
				if (std::isfinite(birth) && std::isfinite(life))
					scale = std::max(scale, (double)std::fabs(birth) + (double)std::fabs(life));
			}
			mUntil[chunk] = until;
			mScale[chunk] = scale;
		}
	}

	// The last level time gpuparticles.vp's main() can draw a record at: never for life <= 0 (a free slot) or a birth so far back
	// it can never be young enough; always when a NaN makes each of its tests false.
	static double DrawnUntil(float birth, float life)
	{
		if (life <= 0.0f)
			return NEVER;
		if (std::isnan(birth) || std::isnan(life))
			return ALWAYS;
		const double until = (double)birth + (double)life;
		return std::isnan(until) ? ALWAYS : until;
	}

	// Whether any record of the chunk can draw at levelTime (conservative: see the top of this file).
	bool ChunkDrawn(unsigned chunk, float levelTime) const
	{
		const double until = mUntil[chunk];
		if (until == NEVER)
			return false;
		if (until == ALWAYS || std::isnan(levelTime))
			return true;
		const double t = levelTime;
		const double margin = 1.0 / 256.0 + (std::fabs(t) + mScale[chunk]) / 1048576.0;
		return !(t > until + margin);
	}

	// The ring's draw at levelTime: runs of slots that hold every record that can draw, in draw order -- ascending, or
	// [oldestSlot, end) then [0, oldestSlot) when oldestFirst -- at most maxRanges (and MAX_RANGES). Returns how many; 0 draws
	// nothing.
	int Ranges(float levelTime, unsigned oldestSlot, bool oldestFirst, SlotRange* out, int maxRanges) const
	{
		maxRanges = std::min(maxRanges, MAX_RANGES);
		if (mRingSize == 0 || maxRanges < 2)
			return WholeRanges(oldestSlot, oldestFirst, out, maxRanges);

		const unsigned oldest = oldestSlot < mRingSize ? oldestSlot : 0;
		unsigned segmentFirst[2] = { 0, 0 }, segmentEnd[2] = { mRingSize, 0 };
		int segments = 1;
		if (oldestFirst)
		{
			segmentFirst[0] = oldest;
			segmentEnd[0] = mRingSize;
			segmentFirst[1] = 0;
			segmentEnd[1] = oldest;
			segments = 2;
		}

		mRuns.clear();
		for (int segment = 0; segment < segments; segment++)
		{
			if (segmentFirst[segment] >= segmentEnd[segment])
				continue;
			for (unsigned chunk = segmentFirst[segment] / CHUNK_SLOTS; chunk * CHUNK_SLOTS < segmentEnd[segment]; chunk++)
			{
				if (!ChunkDrawn(chunk, levelTime))
					continue;
				const unsigned lo = std::max(chunk * CHUNK_SLOTS, segmentFirst[segment]);
				const unsigned hi = std::min(chunk * CHUNK_SLOTS + CHUNK_SLOTS, segmentEnd[segment]);
				if (lo >= hi)
					continue;
				if (!mRuns.empty() && mRuns.back().Segment == segment && mRuns.back().End == lo)
					mRuns.back().End = hi;
				else
					mRuns.push_back({ lo, hi, segment });
			}
		}

		// Too many draws: join the two neighbouring runs of one part with the smallest gap between them (the dead slots in the gap
		// are drawn collapsed, as before). Runs of the two parts are never joined -- that would change the order.
		while ((int)mRuns.size() > maxRanges)
		{
			size_t best = mRuns.size();
			unsigned bestGap = 0;
			for (size_t i = 0; i + 1 < mRuns.size(); i++)
			{
				if (mRuns[i].Segment != mRuns[i + 1].Segment)
					continue;
				const unsigned gap = mRuns[i + 1].First - mRuns[i].End;
				if (best == mRuns.size() || gap < bestGap)
				{
					best = i;
					bestGap = gap;
				}
			}
			if (best == mRuns.size())
				return WholeRanges(oldestSlot, oldestFirst, out, maxRanges);
			mRuns[best].End = mRuns[best + 1].End;
			mRuns.erase(mRuns.begin() + (ptrdiff_t)best + 1);
		}

		int count = 0;
		for (const Run& run : mRuns)
		{
			out[count].First = run.First;
			out[count].Count = run.End - run.First;
			count++;
		}
		return count;
	}

	// The whole ring as it was always drawn: [0, end) in one draw, or [oldest, end) then [0, oldest).
	int WholeRanges(unsigned oldestSlot, bool oldestFirst, SlotRange* out, int maxRanges) const
	{
		int count = 0;
		if (mRingSize == 0 || maxRanges <= 0)
			return 0;
		if (!oldestFirst)
		{
			out[count].First = 0;
			out[count].Count = mRingSize;
			return ++count;
		}
		const unsigned oldest = oldestSlot < mRingSize ? oldestSlot : 0;
		if (mRingSize - oldest > 0)
		{
			out[count].First = oldest;
			out[count].Count = mRingSize - oldest;
			count++;
		}
		if (oldest > 0 && count < maxRanges)
		{
			out[count].First = 0;
			out[count].Count = oldest;
			count++;
		}
		return count;
	}

private:
	static constexpr double NEVER = -std::numeric_limits<double>::infinity();
	static constexpr double ALWAYS = std::numeric_limits<double>::infinity();

	struct Run
	{
		unsigned First;
		unsigned End;
		int Segment;
	};

	unsigned mRingSize = 0;
	std::vector<double> mUntil;		// per chunk: the latest level time a record in it can draw at
	std::vector<double> mScale;		// per chunk: the largest |birth| + |life| of its finite records, for the margin
	mutable std::vector<Run> mRuns;
};
