/*
** hw_effectlightcore.h
**
** [EFFECTLIGHTS] The maths of effect lights, with no engine dependency.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Everything an effect light does that is not plumbing ("Engine docs/LIGHTS_20_21_22_PLAN.md" 2b-2g, with its OWNER
** ANSWERS; "Engine docs/EFFECT_LIGHTS_CORE_IMPL_NOTES.md"):
**
**   - how a light moves, fades, lands and holds (Source, Evaluate) -- closed form at any time, so it moves at frame rate
**     with nothing to relink;
**   - how lights are ranked when the pool is full, when shadow-map rows run out and when a bin is crowded (RanksAbove,
**     TakesRowBefore, BinBuilder);
**   - how one frame's lights are sorted into the world grid around the eye, and a crowded bin's extra lights merged into one
**     glow (BinBuilder).
**
** hw_effectlights.cpp drives it from the level and the renderer. It has no engine dependency on purpose: the offline checks
** compile THIS header and compare it with an independent mirror. It is deterministic -- local hashes, no RNG, no clock of
** its own -- so the same inputs give the same lights on every machine. Presentation either way: nothing here is read back.
**
** Axes. Source and Evaluate are in GAME axes (x, y horizontal, z up), like the level. Records, grids and bins are in SHADER
** axes (game x, z, y), like every GPU consumer.
**
*/

#pragma once

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <vector>
#include "hw_effectlightbuffer.h"

namespace EffectLightCore
{

struct Vec3
{
	double X = 0.0, Y = 0.0, Z = 0.0;
};

inline Vec3 Add(const Vec3 &a, const Vec3 &b) { return { a.X + b.X, a.Y + b.Y, a.Z + b.Z }; }
inline Vec3 Sub(const Vec3 &a, const Vec3 &b) { return { a.X - b.X, a.Y - b.Y, a.Z - b.Z }; }
inline Vec3 Scale(const Vec3 &a, double s) { return { a.X * s, a.Y * s, a.Z * s }; }
inline double Dot(const Vec3 &a, const Vec3 &b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
inline double Length(const Vec3 &a) { return std::sqrt(Dot(a, a)); }

// The EFL_ flags of LevelLocals.SpawnEffectLight and SetDrawnLineLight (doombase.zs). 16 is not used: walls block every
// point light (the owner's answer 4), so there is nothing to ask for.
enum
{
	EFL_IMPORTANT = 1,		// ranks above every ordinary light: evicted, refused a row or merged only by its own tier
	EFL_NOSURFACES = 2,		// does not light walls, flats, models or sprites
	EFL_NOSMOKE = 4,		// does not light the smoke
	EFL_NOPARTICLES = 8,	// does not light particles or debris
	EFL_EXACTHOLD = 32,		// a landed light holds for all of its hold, not a hashed share of it
	EFL_SCRIPT_MASK = EFL_IMPORTANT | EFL_NOSURFACES | EFL_NOSMOKE | EFL_NOPARTICLES | EFL_EXACTHOLD,
	EFL_CONSUMER_MASK = EFL_NOSURFACES | EFL_NOSMOKE | EFL_NOPARTICLES,
};

// Rank tiers, highest first. The owner's answer 6: small, far lights stop lighting first; flashes and tracers always light.
enum
{
	TIER_LOW = 0,		// a particle definition's lightpriority 0 (a later step)
	TIER_NORMAL = 1,	// every other fire-and-forget light
	TIER_IMPORTANT = 2,	// EFL_IMPORTANT (flashes, impacts, tracers a mod marks), lightpriority 2
	TIER_LINE = 3,		// a drawn-line light: the mod's own line, always lit
};

// ---------------------------------------------------------------------------------------------------------------------------
// Hashes: FLevelLocals::GpuParticleHash's mixer, so a light's variety comes from the same kind of local hash as a
// particle's. Never an RNG.
// ---------------------------------------------------------------------------------------------------------------------------

inline uint32_t Hash(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU;
	x ^= x >> 15; x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

// A quarter-unit grid position's bits: nearby spawns in the same tic still differ.
inline uint32_t PositionBits(double v)
{
	if (!std::isfinite(v))
		return 0;
	return (uint32_t)(int64_t)std::floor(std::clamp(v, -1.0e8, 1.0e8) * 4.0);
}

// A queued light's seed: the tic it was spawned in, its place in that tic's queue and where it began. The same on every
// machine, whatever the frame rate.
inline uint32_t SeedFor(int tic, int index, const Vec3 &pos)
{
	uint32_t h = Hash((uint32_t)tic * 0x9E3779B1U + 0x68bc21ebU);
	h = Hash(h ^ ((uint32_t)index * 0x85EBCA77U));
	h = Hash(h ^ PositionBits(pos.X));
	h = Hash(h ^ (PositionBits(pos.Y) * 0xC2B2AE3DU));
	h = Hash(h ^ (PositionBits(pos.Z) * 0x27D4EB2FU));
	return h;
}

// 0 <= share < 1, 24 bits of the seed.
inline double HoldShare(uint32_t seed)
{
	return (double)(Hash(seed ^ 0x5bd1e995U) >> 8) / 16777216.0;
}

// ---------------------------------------------------------------------------------------------------------------------------
// One light
// ---------------------------------------------------------------------------------------------------------------------------

// A light in the pool: what it was spawned with. CPU only, never uploaded.
struct Source
{
	Vec3 A0;						// the start (a tracer's head), game axes
	Vec3 B0;						// the end (its tail); == A0 for a point
	Vec3 Vel;						// map units per second, both ends
	float Color[3] = { 1.f, 1.f, 1.f };	// 0..1 a channel
	double Radius = 0.0;			// map units
	double Intensity = 1.0;
	double Birth = 0.0;				// level seconds: maptime / TICRATE of the tic that spawned it, as a particle's birth
	double Life = 0.1;				// seconds
	double Fade = 2.0;				// brightness x (1 - age / Life) ^ Fade; 0 holds full, then goes out; 2 is a strobe
	double Gravity = 0.0;			// map units per second squared, down
	double Drag = 0.0;				// per second, linear: the particle flight maths
	double Land = 0.0;				// seconds: when the light stops moving (a hit, a landing); 0, or Life or more: it never does
	double Hold = 0.0;				// seconds: the most a landed light keeps lighting after Land; 0 goes out on landing
	double TailLag = 0.0;			// 0..1: the end runs the start's path this much slower (0 rigid; 1 stays where it began)
	double TailBrightness = 1.0;	// 0..1: the brightness at the end as a share of the start's (a fading streak: 0)
	int Flags = 0;					// EFL_
	int Tier = TIER_NORMAL;
	uint32_t Seed = 0;				// the local hash a landed light's hold share comes from
	uint64_t Sequence = 0;			// spawn order: ties go to the older light
};

// This frame's state of one light, game axes.
struct Evaluated
{
	Vec3 A, B;
	float Color[3] = { 0.f, 0.f, 0.f };	// colour x intensity x brightness now
	double Radius = 0.0;
	bool Alive = false;
	bool Point = true;				// A == B exactly: a point light, which walls can block
};

inline bool Landed(const Source &s)
{
	return s.Land > 0.0 && s.Land < s.Life;
}

// How long a landed light holds after landing: a hashed share of Hold between a quarter and all of it (the owner's answer
// 5: each piece's hold varies, up to a set maximum), or all of it with EFL_EXACTHOLD.
inline double HoldTime(const Source &s)
{
	if (!(s.Hold > 0.0))
		return 0.0;
	if (s.Flags & EFL_EXACTHOLD)
		return s.Hold;
	return s.Hold * (0.25 + 0.75 * HoldShare(s.Seed));
}

// The age at which the light is gone.
inline double EndAge(const Source &s)
{
	return Landed(s) ? s.Land + HoldTime(s) : s.Life;
}

// gpuparticles.vp's closed-form flight with linear drag, game axes (its up is game z). k is clamped so drag 0 needs no
// branch: (1 - e^-kt) / k tends to t as k tends to 0.
inline Vec3 PathAt(const Vec3 &p0, const Vec3 &vel, double gravity, double drag, double t)
{
	const double k = std::max(drag, 1e-4);
	const double s = (1.0 - std::exp(-k * t)) / k;
	return { p0.X + vel.X * s, p0.Y + vel.Y * s, p0.Z + vel.Z * s - gravity * 0.5 * t * t };
}

// (1 - x) ^ exponent over x in 0..1; exponent 0 (or less) stays 1 until the end.
inline double FadeAt(double x, double exponent)
{
	if (!(exponent > 0.0))
		return 1.0;
	return std::pow(std::clamp(1.0 - x, 0.0, 1.0), exponent);
}

// Where the light is and how bright, at level time `now`.
//
// FLIGHT. Both ends fly the same path from their own start: the start on the path's clock, the end on a clock TailLag
// slower. A rigid streak is TailLag 0 with an end behind the start; a comet tail starts both ends together and lags the end.
// LANDING. From Land the start stops where the path had it then, and the end's clock stops at Land too, so a lagging tail
// keeps running into the landing point. The light then holds for HoldTime, its brightness carrying on from its landing
// value down to nothing with the same fade.
inline Evaluated Evaluate(const Source &s, double now)
{
	Evaluated e;
	double age = now - s.Birth;
	if (!(age >= 0.0))
		age = 0.0;

	const bool landed = Landed(s);
	const double hold = landed ? HoldTime(s) : 0.0;
	const double end = landed ? s.Land + hold : s.Life;
	e.Alive = s.Life > 0.0 && s.Radius > 0.0 && age < end;
	e.Radius = s.Radius;

	const double headTime = landed ? std::min(age, s.Land) : age;
	double tailTime = age * (1.0 - std::clamp(s.TailLag, 0.0, 1.0));
	if (landed)
		tailTime = std::min(tailTime, s.Land);
	e.A = PathAt(s.A0, s.Vel, s.Gravity, s.Drag, headTime);
	e.B = PathAt(s.B0, s.Vel, s.Gravity, s.Drag, tailTime);
	e.Point = e.A.X == e.B.X && e.A.Y == e.B.Y && e.A.Z == e.B.Z;

	double bright;
	if (!landed || age < s.Land)
		bright = FadeAt(s.Life > 0.0 ? age / s.Life : 1.0, s.Fade);
	else
		bright = FadeAt(s.Land / s.Life, s.Fade) * (hold > 0.0 ? FadeAt((age - s.Land) / hold, s.Fade) : 0.0);
	if (!e.Alive)
		bright = 0.0;

	const double gain = std::max(s.Intensity, 0.0) * bright;
	for (int c = 0; c < 3; c++)
		e.Color[c] = (float)std::clamp((double)s.Color[c] * gain, 0.0, 64.0);
	return e;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Segments
// ---------------------------------------------------------------------------------------------------------------------------

// The share t (0 at a, 1 at b) of the closest point on [a, b] to p; 0 for a point.
inline double SegmentShare(const Vec3 &a, const Vec3 &b, const Vec3 &p)
{
	const Vec3 ab = Sub(b, a);
	const double lengthSquared = Dot(ab, ab);
	if (!(lengthSquared > 0.0))
		return 0.0;
	return std::clamp(Dot(Sub(p, a), ab) / lengthSquared, 0.0, 1.0);
}

inline Vec3 PointAt(const Vec3 &a, const Vec3 &b, double t)
{
	return t > 0.0 ? Add(a, Scale(Sub(b, a), t)) : a;
}

inline double DistanceToSegment(const Vec3 &a, const Vec3 &b, const Vec3 &p)
{
	return Length(Sub(p, PointAt(a, b, SegmentShare(a, b, p))));
}

// The Rec. 709 luminance the smoke light list weighs by.
inline double Luminance(const float color[3])
{
	return 0.2126 * color[0] + 0.7152 * color[1] + 0.0722 * color[2];
}

// ---------------------------------------------------------------------------------------------------------------------------
// Ranks
// ---------------------------------------------------------------------------------------------------------------------------

// The plan's score within a tier (2f): luminance of colour x intensity x brightness now, x radius, / (1 + distance / 512).
inline double Score(const float color[3], double radius, double distanceToEye)
{
	return std::max(Luminance(color), 0.0) * std::max(radius, 0.0) / (1.0 + std::max(distanceToEye, 0.0) / 512.0);
}

struct RankKey
{
	int Tier = TIER_NORMAL;
	double Score = 0.0;
	uint64_t Sequence = 0;
};

// Higher first: tier, then score, then the older light. A strict total order while sequences differ.
inline bool RanksAbove(const RankKey &a, const RankKey &b)
{
	if (a.Tier != b.Tier)
		return a.Tier > b.Tier;
	if (a.Score != b.Score)
		return a.Score > b.Score;
	return a.Sequence < b.Sequence;
}

// Fills `order` with the indices of `keys`, best first, and returns how many of them stay within `cap`.
//
// THE POOL'S RULE WHEN IT IS FULL (2f): "a new light replaces the lowest-ranked live light when it ranks above it, and is
// refused when it does not". Applied to the live lights and a frame's new lights together, keeping the `cap` best is the
// same set as applying that rule one new light at a time, in any order: every new light's Sequence is later than every live
// light's, so on a tie the live light stays -- the "does not rank above" of the rule.
inline size_t RankAndKeep(const std::vector<RankKey> &keys, size_t cap, std::vector<uint32_t> &order)
{
	order.resize(keys.size());
	for (size_t i = 0; i < keys.size(); i++)
		order[i] = (uint32_t)i;
	std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return RanksAbove(keys[x], keys[y]); });
	return std::min(cap, keys.size());
}

// The order point lights take shadow-map rows in (2g): tier first, then nearest the eye.
struct RowCandidate
{
	int Tier = TIER_NORMAL;
	double Distance = 0.0;
	uint64_t Sequence = 0;
	uint32_t Light = 0;
};

inline bool TakesRowBefore(const RowCandidate &a, const RowCandidate &b)
{
	if (a.Tier != b.Tier)
		return a.Tier > b.Tier;
	if (a.Distance != b.Distance)
		return a.Distance < b.Distance;
	return a.Sequence < b.Sequence;
}

// Whether a light asks for a shadow-map row this frame (2g; the owner's answer 4, walls always block): a point light in range,
// with light, that lights surfaces or the smoke -- the two consumers that read rows. A line light never asks: the map is cast
// from a point.
inline bool WantsRow(bool inRange, bool point, int eflFlags, double luminance)
{
	return inRange && point && luminance > 0.0 && !((eflFlags & EFL_NOSURFACES) && (eflFlags & EFL_NOSMOKE));
}

// A light's row once the rows are handed out: 0..1023 its row (walls block it), -1 it lights with no row, -2 it does not light.
// A light that asked and got none stops lighting -- walls always block -- unless it is important (the owner's answer 6:
// flashes and tracers always light), which then lights unblocked. rowsLive: the rows really were handed out this frame.
inline int RowForFrame(bool rowsLive, bool wantsRow, int row, int tier)
{
	if (!rowsLive || !wantsRow)
		return -1;
	if (row >= 0)
		return row;
	return tier >= TIER_IMPORTANT ? -1 : -2;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------------------------------------------------------

// One light's record (hw_effectlightbuffer.h): game axes in, shader axes out. `row` -1 is none; with a row, `origin` is the
// position the row was cast from.
inline void WriteRecord(EffectLightRecord &r, const Vec3 &a, const Vec3 &b, double radius, int eflFlags, const float color[3],
	int row, const Vec3 &origin, double tailBrightness)
{
	int flags = EFFECT_LIGHT_GPU_FACING;
	if (row >= 0) flags |= EFFECT_LIGHT_GPU_ROW;
	if (eflFlags & EFL_NOSURFACES) flags |= EFFECT_LIGHT_GPU_NOSURFACES;
	if (eflFlags & EFL_NOSMOKE) flags |= EFFECT_LIGHT_GPU_NOSMOKE;
	if (eflFlags & EFL_NOPARTICLES) flags |= EFFECT_LIGHT_GPU_NOPARTICLES;

	r.a[0] = (float)a.X; r.a[1] = (float)a.Z; r.a[2] = (float)a.Y; r.a[3] = (float)radius;
	r.b[0] = (float)b.X; r.b[1] = (float)b.Z; r.b[2] = (float)b.Y; r.b[3] = (float)flags;
	r.color[0] = color[0]; r.color[1] = color[1]; r.color[2] = color[2]; r.color[3] = (float)(row >= 0 ? row : -1);
	if (row >= 0)
	{
		r.extra[0] = (float)origin.X; r.extra[1] = (float)origin.Z; r.extra[2] = (float)origin.Y;
	}
	else
	{
		r.extra[0] = r.extra[1] = r.extra[2] = 0.f;
	}
	r.extra[3] = (float)std::clamp(tailBrightness, 0.0, 1.0);
}

// ---------------------------------------------------------------------------------------------------------------------------
// The grid (2d)
// ---------------------------------------------------------------------------------------------------------------------------

struct GridSpec
{
	int BinSize;
	int SizeX, SizeUp, SizeY;
};

// r_effectlights_quality: every quality covers 3072 x 768 x 3072 map units (x, up, y).
inline GridSpec GridForQuality(int quality)
{
	static const GridSpec specs[3] = { { 64, 48, 12, 48 }, { 32, 96, 24, 96 }, { 24, 128, 32, 128 } };
	return specs[std::clamp(quality, 1, 3) - 1];
}

// The world grid around the eye, SHADER axes. Whole bins with the eye's bin in the middle, so it moves by whole bins (the
// smoke box's rule) and a light's bins never shift under it between frames while the eye stays in one bin.
struct Grid
{
	double Corner[3] = { 0.0, 0.0, 0.0 };
	int Size[3] = { 0, 0, 0 };
	double BinSize = 0.0;
};

inline Grid GridAround(double eyeX, double eyeY, double eyeZ, const GridSpec &spec)
{
	Grid g;
	const double s = spec.BinSize;
	g.BinSize = s;
	g.Size[0] = spec.SizeX;
	g.Size[1] = spec.SizeUp;
	g.Size[2] = spec.SizeY;
	g.Corner[0] = (std::floor(eyeX / s) - spec.SizeX / 2) * s;
	g.Corner[1] = (std::floor(eyeZ / s) - spec.SizeUp / 2) * s;
	g.Corner[2] = (std::floor(eyeY / s) - spec.SizeY / 2) * s;
	return g;
}

// A bin coordinate from a value in bin units, clamped before the cast so no value can overflow it.
inline int CellCeil(double v, int limit) { return (int)std::ceil(std::clamp(v, -1.0, (double)limit)); }
inline int CellFloor(double v, int limit) { return (int)std::floor(std::clamp(v, -1.0, (double)limit)); }

// The squared distance from p to the record's segment, shader axes, in double.
inline double DistanceSquaredToRecord(double px, double py, double pz, const EffectLightRecord &r)
{
	const double ax = r.a[0], ay = r.a[1], az = r.a[2];
	const double sx = (double)r.b[0] - ax, sy = (double)r.b[1] - ay, sz = (double)r.b[2] - az;
	const double lengthSquared = sx * sx + sy * sy + sz * sz;
	double t = 0.0;
	if (lengthSquared > 0.0)
		t = std::clamp(((px - ax) * sx + (py - ay) * sy + (pz - az) * sz) / lengthSquared, 0.0, 1.0);
	const double dx = ax + sx * t - px, dy = ay + sy * t - py, dz = az + sz * t - pz;
	return dx * dx + dy * dy + dz * dz;
}

// One light for BinBuilder: its record as it will upload, and its tier. Handed over best first.
struct BinLight
{
	EffectLightRecord Record = {};
	int Tier = TIER_NORMAL;
};

struct BinResult
{
	std::vector<EffectLightRecord> Records;	// the binned lights in rank order, then residual glows
	EffectLightGridHeader Grid = {};		// the occupied box; Size 0 when nothing is binned
	std::vector<uint32_t> Bins;				// one word a bin of the box
	std::vector<uint32_t> Indices;
	unsigned Binned = 0;	// lights binned (Records before the residuals)
	int Merged = 0;			// residual glows made
	int Trimmed = 0;		// lights not binned because the index list was full: the lowest-ranked
	int Outside = 0;		// lights that reach no bin of the grid
	int Dropped = 0;		// merged lights with no room left for their bin's glow
	int Lines = 0;			// binned lights with a length
};

// THE FRAME'S BINS (2d).
//
// WHICH BINS A LIGHT GOES IN. Every bin whose centre is within radius + half a bin's diagonal of the light's segment -- that
// is every bin whose box the light's capsule can reach, so a position the light reaches finds the light in its bin, from
// either side of a bin edge. Found by slabs along x: only the part of the segment within reach of a slab's centre can reach
// its bins, which keeps a long tracer's work near the bins it touches.
//
// THE INDEX LIST IS BOUNDED. Lights go in best first; when the next light's bins would pass EFFECT_LIGHT_INDEX_CAPACITY, it
// and every lower-ranked light stop binning (Trimmed).
//
// A CROWDED BIN (more than perBin lights) keeps its best perBin - 1 -- important and drawn-line lights first, then by their
// light at the bin's centre -- and merges the rest into one residual glow, so nothing goes dark (2d, 2f). Important lights
// are never merged while the loop has room: a bin keeps up to EFFECT_LIGHT_BIN_LOOP_MAX - 1 of them even past perBin.
//
// THE RESIDUAL GLOW. Placed at the merged lights' closest points to the bin centre, weighted by their light there; its
// radius is their largest; it does not face (no N.L). Its colour gives the bin centre the same light the merged lights gave
// it -- the plan's "summed colour", summed where it is measured -- divided by its own attenuation there, which is never
// below a quarter. Walls block it through the row of its strongest merged light that has one.
class BinBuilder
{
public:
	void Build(const std::vector<BinLight> &lights, const Grid &grid, int perBin, BinResult &out)
	{
		out.Records.clear();
		out.Bins.clear();
		out.Indices.clear();
		out.Grid = EffectLightGridHeader();
		out.Binned = 0;
		out.Merged = out.Trimmed = out.Outside = out.Dropped = out.Lines = 0;

		const int cap = std::clamp(perBin, 2, EFFECT_LIGHT_BIN_LOOP_MAX);
		const double s = grid.BinSize;
		if (!(s > 0.0) || grid.Size[0] <= 0 || grid.Size[1] <= 0 || grid.Size[2] <= 0)
		{
			out.Outside = (int)lights.size();
			return;
		}
		const double pad = s * 0.8660254037844386;	// half a bin's diagonal

		mTouches.clear();
		mRecordOf.assign(lights.size(), -1);
		int lo[3] = { INT_MAX, INT_MAX, INT_MAX }, hi[3] = { -1, -1, -1 };

		for (size_t li = 0; li < lights.size(); li++)
		{
			const EffectLightRecord &r = lights[li].Record;
			bool finite = std::isfinite(r.a[3]);
			for (int k = 0; k < 3; k++)
				finite = finite && std::isfinite(r.a[k]) && std::isfinite(r.b[k]);
			if (!finite || !(r.a[3] > 0.f))
			{
				out.Outside++;
				continue;
			}
			const size_t start = mTouches.size();
			TouchBins(r, grid, (double)r.a[3] + pad, (uint32_t)li);
			if (mTouches.size() == start)
			{
				out.Outside++;
				continue;
			}
			if (mTouches.size() > (size_t)EFFECT_LIGHT_INDEX_CAPACITY)
			{
				mTouches.resize(start);
				out.Trimmed += (int)(lights.size() - li);
				break;
			}
			for (size_t t = start; t < mTouches.size(); t++)
			{
				for (int k = 0; k < 3; k++)
				{
					lo[k] = std::min(lo[k], (int)mTouches[t].Cell[k]);
					hi[k] = std::max(hi[k], (int)mTouches[t].Cell[k]);
				}
			}
			mRecordOf[li] = (int32_t)out.Records.size();
			out.Records.push_back(r);
			if (r.a[0] != r.b[0] || r.a[1] != r.b[1] || r.a[2] != r.b[2])
				out.Lines++;
		}
		out.Binned = (unsigned)out.Records.size();
		if (mTouches.empty())
			return;

		// Counting sort by bin of the occupied box. Touches were made best light first, so each bin lists its lights in rank
		// order.
		const int dims[3] = { hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1 };
		const size_t binCount = (size_t)dims[0] * (size_t)dims[1] * (size_t)dims[2];
		mStarts.assign(binCount + 1, 0);
		for (const Touch &t : mTouches)
			mStarts[LocalBin(t, lo, dims) + 1]++;
		for (size_t b = 0; b < binCount; b++)
			mStarts[b + 1] += mStarts[b];
		mFill.assign(mStarts.begin(), mStarts.end() - 1);
		mSorted.resize(mTouches.size());
		for (const Touch &t : mTouches)
			mSorted[mFill[LocalBin(t, lo, dims)]++] = t.Light;

		out.Bins.assign(binCount, 0);
		out.Indices.reserve(mTouches.size());
		for (size_t b = 0; b < binCount; b++)
		{
			const uint32_t first = mStarts[b], count = mStarts[b + 1] - mStarts[b];
			if (count == 0)
				continue;
			const uint32_t offset = (uint32_t)out.Indices.size();
			if ((int)count <= cap)
			{
				for (uint32_t k = 0; k < count; k++)
					out.Indices.push_back((uint32_t)mRecordOf[mSorted[first + k]]);
				out.Bins[b] = (offset << EFFECT_LIGHT_BIN_COUNT_BITS) | count;
				continue;
			}

			const int x = lo[0] + (int)(b % (size_t)dims[0]);
			const int y = lo[1] + (int)((b / (size_t)dims[0]) % (size_t)dims[1]);
			const int z = lo[2] + (int)(b / ((size_t)dims[0] * (size_t)dims[1]));
			const double centre[3] = { grid.Corner[0] + (x + 0.5) * s, grid.Corner[1] + (y + 0.5) * s, grid.Corner[2] + (z + 0.5) * s };
			const uint32_t kept = CrowdedBin(lights, first, count, cap, centre, out);
			out.Bins[b] = (offset << EFFECT_LIGHT_BIN_COUNT_BITS) | kept;
		}

		out.Grid.Corner[0] = (float)(grid.Corner[0] + lo[0] * s);
		out.Grid.Corner[1] = (float)(grid.Corner[1] + lo[1] * s);
		out.Grid.Corner[2] = (float)(grid.Corner[2] + lo[2] * s);
		out.Grid.Corner[3] = (float)s;
		out.Grid.Size[0] = dims[0];
		out.Grid.Size[1] = dims[1];
		out.Grid.Size[2] = dims[2];
		out.Grid.Size[3] = (int32_t)binCount;
	}

private:
	struct Touch
	{
		uint16_t Cell[3];
		uint32_t Light;
	};

	struct Entry
	{
		uint32_t Light;
		int Tier;
		double Contribution;			// the light's contribution at the bin centre: luminance x attenuation x brightness share
		double Closest[3];
		double Attenuation;
		double Trail;
	};

	std::vector<Touch> mTouches;
	std::vector<int32_t> mRecordOf;
	std::vector<uint32_t> mStarts, mFill, mSorted;
	std::vector<Entry> mEntries;

	static size_t LocalBin(const Touch &t, const int lo[3], const int dims[3])
	{
		return ((size_t)(t.Cell[2] - lo[2]) * (size_t)dims[1] + (size_t)(t.Cell[1] - lo[1])) * (size_t)dims[0] + (size_t)(t.Cell[0] - lo[0]);
	}

	void TouchBins(const EffectLightRecord &r, const Grid &grid, double reach, uint32_t light)
	{
		const double s = grid.BinSize;
		const double margin = 1e-6;	// candidates only; the distance test below decides
		int ilo[3], ihi[3];
		for (int k = 0; k < 3; k++)
		{
			const double mn = std::min((double)r.a[k], (double)r.b[k]) - reach - margin;
			const double mx = std::max((double)r.a[k], (double)r.b[k]) + reach + margin;
			ilo[k] = std::max(0, CellCeil((mn - grid.Corner[k]) / s - 0.5, grid.Size[k]));
			ihi[k] = std::min(grid.Size[k] - 1, CellFloor((mx - grid.Corner[k]) / s - 0.5, grid.Size[k]));
			if (ilo[k] > ihi[k])
				return;
		}

		const double reachSquared = reach * reach;
		const double dx = (double)r.b[0] - (double)r.a[0];
		for (int ix = ilo[0]; ix <= ihi[0]; ix++)
		{
			const double cx = grid.Corner[0] + (ix + 0.5) * s;
			double t0 = 0.0, t1 = 1.0;
			if (std::fabs(dx) > 1e-9)
			{
				double ta = (cx - reach - margin - r.a[0]) / dx;
				double tb = (cx + reach + margin - r.a[0]) / dx;
				if (ta > tb) std::swap(ta, tb);
				t0 = std::max(ta, 0.0);
				t1 = std::min(tb, 1.0);
				if (t0 > t1)
					continue;
			}
			else if (std::fabs((double)r.a[0] - cx) > reach + margin)
			{
				continue;
			}

			int jlo[3] = { 0, 0, 0 }, jhi[3] = { -1, -1, -1 };
			for (int k = 1; k < 3; k++)
			{
				const double d = (double)r.b[k] - (double)r.a[k];
				const double p0 = r.a[k] + d * t0, p1 = r.a[k] + d * t1;
				jlo[k] = std::max(ilo[k], CellCeil((std::min(p0, p1) - reach - margin - grid.Corner[k]) / s - 0.5, grid.Size[k]));
				jhi[k] = std::min(ihi[k], CellFloor((std::max(p0, p1) + reach + margin - grid.Corner[k]) / s - 0.5, grid.Size[k]));
			}
			for (int iy = jlo[1]; iy <= jhi[1]; iy++)
			{
				const double cy = grid.Corner[1] + (iy + 0.5) * s;
				for (int iz = jlo[2]; iz <= jhi[2]; iz++)
				{
					const double cz = grid.Corner[2] + (iz + 0.5) * s;
					if (DistanceSquaredToRecord(cx, cy, cz, r) <= reachSquared)
						mTouches.push_back({ { (uint16_t)ix, (uint16_t)iy, (uint16_t)iz }, light });
				}
			}
		}
	}

	uint32_t CrowdedBin(const std::vector<BinLight> &lights, uint32_t first, uint32_t count, int cap, const double centre[3], BinResult &out)
	{
		mEntries.resize(count);
		int important = 0;
		for (uint32_t k = 0; k < count; k++)
		{
			const uint32_t li = mSorted[first + k];
			const EffectLightRecord &r = lights[li].Record;
			Entry &e = mEntries[k];
			e.Light = li;
			e.Tier = lights[li].Tier;
			if (e.Tier >= TIER_IMPORTANT)
				important++;

			const double ax = r.a[0], ay = r.a[1], az = r.a[2];
			const double sx = (double)r.b[0] - ax, sy = (double)r.b[1] - ay, sz = (double)r.b[2] - az;
			const double lengthSquared = sx * sx + sy * sy + sz * sz;
			double t = 0.0;
			if (lengthSquared > 0.0)
				t = std::clamp(((centre[0] - ax) * sx + (centre[1] - ay) * sy + (centre[2] - az) * sz) / lengthSquared, 0.0, 1.0);
			e.Closest[0] = t > 0.0 ? ax + sx * t : ax;
			e.Closest[1] = t > 0.0 ? ay + sy * t : ay;
			e.Closest[2] = t > 0.0 ? az + sz * t : az;
			const double ex = e.Closest[0] - centre[0], ey = e.Closest[1] - centre[1], ez = e.Closest[2] - centre[2];
			const double distance = std::sqrt(ex * ex + ey * ey + ez * ez);
			const double radius = r.a[3];
			e.Attenuation = std::clamp((radius - distance) / radius, 0.0, 1.0);
			e.Trail = t > 0.0 ? 1.0 * (1.0 - t) + (double)r.extra[3] * t : 1.0;	// GLSL's mix(1, share, t)
			e.Contribution = std::max(Luminance(r.color), 0.0) * e.Attenuation * e.Trail;
		}
		std::sort(mEntries.begin(), mEntries.end(), [](const Entry &a, const Entry &b)
		{
			if (a.Tier >= TIER_IMPORTANT) { if (b.Tier < TIER_IMPORTANT) return true; }
			else if (b.Tier >= TIER_IMPORTANT) return false;
			if (a.Contribution != b.Contribution) return a.Contribution > b.Contribution;
			return a.Light < b.Light;
		});

		uint32_t keep = (uint32_t)std::max(cap - 1, std::min(important, EFFECT_LIGHT_BIN_LOOP_MAX - 1));
		if (keep >= count)
			keep = count;
		for (uint32_t k = 0; k < keep; k++)
			out.Indices.push_back((uint32_t)mRecordOf[mEntries[k].Light]);
		if (keep == count)
			return count;

		// The residual glow of the rest.
		double weight = 0.0, sum[3] = { 0.0, 0.0, 0.0 }, colour[3] = { 0.0, 0.0, 0.0 }, radius = 0.0, best = -1.0;
		int noFlags = EFFECT_LIGHT_GPU_NOSURFACES | EFFECT_LIGHT_GPU_NOSMOKE | EFFECT_LIGHT_GPU_NOPARTICLES;
		int bestRowLight = -1;
		for (uint32_t k = keep; k < count; k++)
		{
			const Entry &e = mEntries[k];
			const EffectLightRecord &r = lights[e.Light].Record;
			const int flags = (int)r.b[3];
			noFlags &= flags;
			radius = std::max(radius, (double)r.a[3]);
			weight += e.Contribution;
			for (int c = 0; c < 3; c++)
			{
				sum[c] += e.Closest[c] * e.Contribution;
				colour[c] += (double)r.color[c] * e.Attenuation * e.Trail;
			}
			if ((flags & EFFECT_LIGHT_GPU_ROW) && e.Contribution > best)
			{
				best = e.Contribution;
				bestRowLight = (int)e.Light;
			}
		}
		if (!(weight > 0.0))
			return keep;	// the merged lights give the bin centre no light: nothing to keep of them here
		if (out.Records.size() >= (size_t)EFFECT_LIGHT_RECORD_CAPACITY)
		{
			out.Dropped += (int)(count - keep);
			return keep;
		}

		const double at[3] = { sum[0] / weight, sum[1] / weight, sum[2] / weight };
		const double fx = at[0] - centre[0], fy = at[1] - centre[1], fz = at[2] - centre[2];
		const double attenuation = std::clamp((radius - std::sqrt(fx * fx + fy * fy + fz * fz)) / radius, 0.0, 1.0);
		const double divisor = std::max(attenuation, 0.25);

		EffectLightRecord g = {};
		g.a[0] = g.b[0] = (float)at[0];
		g.a[1] = g.b[1] = (float)at[1];
		g.a[2] = g.b[2] = (float)at[2];
		g.a[3] = (float)radius;
		int flags = EFFECT_LIGHT_GPU_RESIDUAL | noFlags;
		for (int c = 0; c < 3; c++)
			g.color[c] = (float)std::min(colour[c] / divisor, 64.0);
		g.color[3] = -1.f;
		if (bestRowLight >= 0)
		{
			const EffectLightRecord &r = lights[(size_t)bestRowLight].Record;
			flags |= EFFECT_LIGHT_GPU_ROW;
			g.color[3] = r.color[3];
			g.extra[0] = r.extra[0];
			g.extra[1] = r.extra[1];
			g.extra[2] = r.extra[2];
		}
		g.b[3] = (float)flags;
		g.extra[3] = 1.f;

		out.Indices.push_back((uint32_t)out.Records.size());
		out.Records.push_back(g);
		out.Merged++;
		return keep + 1;
	}
};

} // namespace EffectLightCore
