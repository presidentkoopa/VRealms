/*
** hw_effectlights.cpp
**
** [EFFECTLIGHTS] Effect lights: the level's queue, the pool, the frame's rows, bins
** and upload. See hw_effectlights.h; the maths is hw_effectlightcore.h.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hw_effectlights.h"
#include "hw_effectlightcore.h"
#include "hw_effectlightbuffer.h"
#include "hw_shadowmap.h"
#include "hw_perflog.h"
#include "hw_effectsgovernor.h"	// [GOVERNOR] E8: the lights one bin lights with
#include "g_levellocals.h"
#include "doomdef.h"
#include "c_cvars.h"
#include "v_video.h"
#include "i_time.h"
#include "printf.h"

using namespace EffectLightCore;

// ---------------------------------------------------------------------------------------------------------------------------
// The switches. Every one is read by the renderer every frame, so it responds with a menu open (a menu pauses the game:
// lights stop ageing, but a change to the budget, the bins or the distance shows at once). Local: nothing here reaches
// another machine.
// ---------------------------------------------------------------------------------------------------------------------------

CVARD(Bool, r_effectlights, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "effect lights: sparks, embers, impacts and tracers light what is around them (Vulkan only)")

CUSTOM_CVARD(Int, r_effectlights_max, 1024, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "the most effect lights alive at once: 128, 256, 512, 1024, 2048 or 4096; past it the lowest-ranked go first (Vulkan only)")
{
	int snapped = 128;
	while (snapped < self && snapped < EFFECT_LIGHT_POOL_MAX)
		snapped *= 2;
	if (snapped != self)
		self = snapped;
}

CUSTOM_CVARD(Int, r_effectlights_quality, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "effect light bins: 1 = 64, 2 = 32, 3 = 24 map units, all covering 3072 x 3072 x 768 around you (Vulkan only)")
{
	if (self < 1) self = 1;
	else if (self > 3) self = 3;
}

CUSTOM_CVARD(Int, r_effectlights_perbin, 16, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "the most effect lights one bin lights with before the rest blend into one glow, 2-32 (Vulkan only)")
{
	if (self < 2) self = 2;
	else if (self > EFFECT_LIGHT_BIN_LOOP_MAX) self = EFFECT_LIGHT_BIN_LOOP_MAX;
}

CUSTOM_CVARD(Float, r_effectlights_distance, 1536.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "effect lights farther than this from you do not light, 128-1536 map units (Vulkan only)")
{
	if (self < 128.f) self = 128.f;
	else if (self > 1536.f) self = 1536.f;
}

CVARD(Bool, r_effectlights_walls, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "walls block effect lights: each point light takes a row of the shadow map (Vulkan only)")

// A test, not a setting: not archived, like r_debris_test.
CUSTOM_CVARD(Int, r_effectlights_test, 0, CVAR_GLOBALCONFIG, "keeps about N test sparks alive ahead of you, half of them landing and holding, and a line light crossing the view twice a second -- a test of effect lights (Vulkan only)")
{
	if (self < 0) self = 0;
	else if (self > EFFECT_LIGHT_POOL_MAX) self = EFFECT_LIGHT_POOL_MAX;
}

namespace
{
	// hw_debrispool.cpp's ReadQueue (after hw_smokevolume.cpp's), with the event's place in its generation passed along: the
	// seed of a light is keyed on it, so it does not depend on how frames split the tics. Every event of the queue not read
	// before, the older generation first, from this reader's own cursor. Nothing is written to the queue.
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
				take(queue.Items[g][i], queue.Tic[g], i);
			cursorSerial = serial;
			cursorCount = count;
		}
	}

	Vec3 ToVec3(const DVector3& v)
	{
		return { v.X, v.Y, v.Z };
	}

	const double kPi = 3.14159265358979323846;
}

EffectLights& EffectLights::Get()
{
	static EffectLights lights;
	return lights;
}

int EffectLights::PoolCap() const
{
	return std::clamp((int)*r_effectlights_max, 128, EFFECT_LIGHT_POOL_MAX);
}

void EffectLights::Spawn(const Source& source)
{
	if (mSpawned.size() < (size_t)EFFECT_LIGHT_POOL_MAX)
		mSpawned.push_back(source);
}

// [EMISSIVEVOLUMES] See the header. Checked here, so a frame light can never put a bad record in the pool.
bool EffectLights::AddFrameLight(const FrameLight& light)
{
	if (mAddedFrameLights.size() >= FRAME_LIGHTS_MAX)
		return false;
	bool finite = std::isfinite(light.Radius) && std::isfinite(light.TailBrightness) &&
		std::isfinite(light.A.X) && std::isfinite(light.A.Y) && std::isfinite(light.A.Z) &&
		std::isfinite(light.B.X) && std::isfinite(light.B.Y) && std::isfinite(light.B.Z);
	for (int c = 0; c < 3; c++)
		finite = finite && std::isfinite(light.Color[c]);
	if (!finite || !(light.Radius >= 1.0) || !(light.Color[0] > 0.f || light.Color[1] > 0.f || light.Color[2] > 0.f))
		return false;
	mAddedFrameLights.push_back(light);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Step 1
// ---------------------------------------------------------------------------------------------------------------------------

bool EffectLights::BeginFrame(FLevelLocals* Level, const DVector3& eye, double yaw, double ticFrac, uint64_t levelSerial, bool levelAllowsRows)
{
	mFrame++;
	mBegan = false;
	mBlocking = false;
	mRowOrder.clear();
	// [EMISSIVEVOLUMES] The frame lights handed over for this frame; any a return below leaves unused are dropped with it.
	mTakenFrameLights.swap(mAddedFrameLights);
	mAddedFrameLights.clear();
	EffectLightFrameStats& stats = EffectLightStats();
	stats = EffectLightFrameStats();
	if (Level == nullptr)
		return false;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	// Vulkan only, like every effect since the particle stage 2a: GL and GLES have no buffer.
	mActive = screen != nullptr && screen->IsVulkan() && EffectLightBuffer::Instance() != nullptr && r_effectlights;

	if (levelSerial != mLevelSerial)
	{
		// A new map or a savegame load: the old map's lights go with it (ClearLevelData emptied the queue).
		mLevelSerial = levelSerial;
		mQueueSerial = 0;
		mQueueCount = 0;
		mPool.clear();
		mSpawned.clear();
		mLastNow = -1.0;
		mTestCarry = 0.0;
		mTestLineNext = 0.0;
	}

	// The draw's level clock, exactly as StartScene builds mLevelTime: it stops while the game is paused.
	mNow = (Level->maptime + ticFrac) / (double)TICRATE;
	const double dt = mLastNow >= 0.0 ? std::max(mNow - mLastNow, 0.0) : 0.0;
	mLastNow = mNow;
	mEye = eye;

	// The level's new lights. Always read, so the cursor keeps up; kept only while effect lights are on.
	mNew.clear();
	ReadQueue(Level->EffectLightSpawns, mQueueSerial, mQueueCount, [&](const FEffectLightEvent& e, int tic, int index)
	{
		if (!mActive)
			return;
		Source s;
		s.A0 = ToVec3(e.Pos);
		s.B0 = ToVec3(e.End);
		s.Vel = ToVec3(e.Vel);
		const PalEntry colour(e.Color);
		s.Color[0] = colour.r / 255.f;
		s.Color[1] = colour.g / 255.f;
		s.Color[2] = colour.b / 255.f;
		s.Radius = e.Radius;
		s.Intensity = e.Intensity;
		s.Birth = tic / (double)TICRATE;	// a particle's birth: maptime / TICRATE of the tic that spawned it
		s.Life = e.Life;
		s.Fade = e.Fade;
		s.Gravity = e.Gravity;
		s.Drag = e.Drag;
		s.Land = e.Land;
		s.Hold = e.Hold;
		s.TailLag = e.TailLag;
		s.TailBrightness = e.TailBrightness;
		s.Flags = e.Flags & EFL_SCRIPT_MASK;
		s.Tier = (s.Flags & EFL_IMPORTANT) ? TIER_IMPORTANT : TIER_NORMAL;
		s.Seed = SeedFor(tic, index, s.A0);
		s.Sequence = ++mSequence;
		mNew.push_back(s);
	});

	if (mActive)
	{
		for (Source& s : mSpawned)
		{
			s.Sequence = ++mSequence;
			mNew.push_back(s);
		}
		if (*r_effectlights_test > 0)
			MakeTestLights(dt, yaw);
	}
	mSpawned.clear();

	if (!mActive)
	{
		mPool.clear();
		if (timed)
			PerfLog::AddCpuSample("fx.effectlights", (double)(I_nsTime() - startNs) / 1e6);
		return false;
	}

	// Every light at this frame's clock; the gone go. New lights that are already gone never enter.
	size_t live = 0;
	for (size_t i = 0; i < mPool.size(); i++)
	{
		mPool[i].State = Evaluate(mPool[i].Source, mNow);
		if (!mPool[i].State.Alive)
			continue;
		if (live != i)
			mPool[live] = std::move(mPool[i]);
		live++;
	}
	mPool.resize(live);
	for (const Source& s : mNew)
	{
		PoolLight p;
		p.Source = s;
		p.State = Evaluate(s, mNow);
		if (p.State.Alive)
			mPool.push_back(std::move(p));
	}

	const double reach = std::clamp((double)*r_effectlights_distance, 128.0, 1536.0);
	const Vec3 eyeAt = ToVec3(eye);
	for (PoolLight& p : mPool)
	{
		p.Distance = DistanceToSegment(p.State.A, p.State.B, eyeAt);
		p.InRange = p.Distance - p.State.Radius <= reach;
		p.Row = -1;
	}

	// The pool's budget (2f): the best r_effectlights_max stay, in rank order.
	const size_t cap = (size_t)PoolCap();
	if (mPool.size() > cap)
	{
		mKeys.resize(mPool.size());
		for (size_t i = 0; i < mPool.size(); i++)
			mKeys[i] = { mPool[i].Source.Tier, Score(mPool[i].State.Color, mPool[i].State.Radius, mPool[i].Distance), mPool[i].Source.Sequence };
		const size_t kept = RankAndKeep(mKeys, cap, mOrder);
		mPoolScratch.clear();
		for (size_t k = 0; k < kept; k++)
			mPoolScratch.push_back(std::move(mPool[mOrder[k]]));
		stats.Evicted = (int)(mPool.size() - kept);	// live lights evicted and new lights refused alike
		mPool.swap(mPoolScratch);
	}

	// [EMISSIVEVOLUMES] This frame's frame lights (AddFrameLight) join the pool for this frame only: after the budget, so they
	// never evict a pool light, and with a Life of 0, so the next frame's evaluation above drops them. From here they are pool
	// lights in the state they were handed over in: rows, bins and the upload treat them as any other. None: nothing changes.
	for (const FrameLight& f : mTakenFrameLights)
	{
		PoolLight p;
		p.Source.A0 = f.A;
		p.Source.B0 = f.B;
		p.Source.Radius = std::clamp(f.Radius, 1.0, 1024.0);
		p.Source.Life = 0.0;
		p.Source.Birth = mNow;
		p.Source.Flags = f.Flags & EFL_SCRIPT_MASK;
		p.Source.Tier = (p.Source.Flags & EFL_IMPORTANT) ? TIER_IMPORTANT : TIER_NORMAL;
		p.Source.TailBrightness = std::clamp(f.TailBrightness, 0.0, 1.0);
		p.Source.Sequence = ++mSequence;
		p.State.A = f.A;
		p.State.B = f.B;
		for (int c = 0; c < 3; c++)
			p.State.Color[c] = std::clamp(f.Color[c], 0.f, 64.f);
		p.State.Radius = p.Source.Radius;
		p.State.Alive = true;
		p.State.Point = f.A.X == f.B.X && f.A.Y == f.B.Y && f.A.Z == f.B.Z;
		p.Distance = DistanceToSegment(p.State.A, p.State.B, eyeAt);
		p.InRange = p.Distance - p.State.Radius <= reach;
		p.Row = -1;
		mPool.push_back(std::move(p));
	}
	mTakenFrameLights.clear();

	// The point lights walls block, in the order they take rows (2g; the owner's answer 4). Only surfaces and the smoke read
	// rows, so a light for neither asks for none.
	mBlocking = levelAllowsRows && r_effectlights_walls;
	if (mBlocking)
	{
		for (size_t i = 0; i < mPool.size(); i++)
		{
			const PoolLight& p = mPool[i];
			if (!WantsRow(p.InRange, p.State.Point, p.Source.Flags, Luminance(p.State.Color)))
				continue;
			mRowOrder.push_back({ p.Source.Tier, p.Distance, p.Source.Sequence, (uint32_t)i });
		}
		std::sort(mRowOrder.begin(), mRowOrder.end(), TakesRowBefore);
	}

	stats.Live = (int)mPool.size();
	mBegan = true;

	// The map pass LINGERS for a while after the last point light that wanted a row. While nobody asks for it, RenderViewpoint
	// takes the level's AABB tree away from the shadow map, and handing it back re-uploads the whole tree (IShadowMap::
	// SetAABBTree marks it new) -- a hitch at every spark burst in a map without shadowed lamps. A pass with no light in any
	// row costs a texel's early out each.
	static const uint64_t kRowLingerMs = 10000;
	const bool wantsRows = mBlocking && !mRowOrder.empty();
	const uint64_t nowMs = I_msTime();
	if (wantsRows)
		mRowsWantedMs = nowMs;
	const bool lingering = mBlocking && mRowsWantedMs != 0 && nowMs - mRowsWantedMs < kRowLingerMs;

	if (timed)
		PerfLog::AddCpuSample("fx.effectlights", (double)(I_nsTime() - startNs) / 1e6);
	return wantsRows || lingering;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Step 2
// ---------------------------------------------------------------------------------------------------------------------------

int EffectLights::AssignShadowRows(IShadowMap* shadowMap, int firstFreeRow)
{
	if (!mBegan || !mBlocking || shadowMap == nullptr)
		return firstFreeRow;

	mRowsFrame = mFrame;
	int row = std::max(firstFreeRow, 0);
	int taken = 0;
	for (const RowCandidate& c : mRowOrder)
	{
		if (row >= 1024)
			break;
		PoolLight& p = mPool[c.Light];
		p.Row = row;
		// The row is cast from where the light is this frame, the position its record carries (game axes, as CollectLights).
		shadowMap->SetLight(row, (float)p.State.A.X, (float)p.State.A.Y, (float)p.State.A.Z, (float)p.State.Radius);
		row++;
		taken++;
	}
	IShadowMap::LightsShadowmapped += taken;
	EffectLightStats().Rows = taken;
	return taken > 0 ? row : firstFreeRow;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Step 3
// ---------------------------------------------------------------------------------------------------------------------------

void EffectLights::PrepareFrame(FLevelLocals* Level, const DVector3& eye, double ticFrac)
{
	EffectLightBuffer* buffer = EffectLightBuffer::Instance();
	if (buffer == nullptr)
		return;
	static const EffectLightGridHeader kNoGrid = {};
	if (Level == nullptr || !mBegan || !mActive)
	{
		buffer->Upload(nullptr, 0, 0, kNoGrid, nullptr, 0, nullptr, 0);
		return;
	}

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;
	EffectLightFrameStats& stats = EffectLightStats();

	// Walls block this frame only if the rows were really handed out: the map pass ran and called AssignShadowRows.
	const bool rowsLive = mBlocking && mRowsFrame == mFrame;
	const double reach = std::clamp((double)*r_effectlights_distance, 128.0, 1536.0);
	const Vec3 eyeAt = ToVec3(eye);

	mFrameLights.clear();
	mKeys.clear();
	int noRow = 0;
	for (const PoolLight& p : mPool)
	{
		if (!p.InRange || Luminance(p.State.Color) <= 0.0)
			continue;
		if ((p.Source.Flags & EFL_CONSUMER_MASK) == EFL_CONSUMER_MASK)
			continue;
		// Walls always block (the owner's answer 4) -- but flashes and tracers always light (answer 6): an important light the
		// rows ran out for still lights, unblocked; any other stops lighting.
		const int row = RowForFrame(rowsLive, WantsRow(p.InRange, p.State.Point, p.Source.Flags, Luminance(p.State.Color)), p.Row, p.Source.Tier);
		if (row == -2)
		{
			noRow++;
			continue;
		}
		BinLight light;
		WriteRecord(light.Record, p.State.A, p.State.B, p.State.Radius, p.Source.Flags, p.State.Color, row, p.State.A, p.Source.TailBrightness);
		light.Tier = p.Source.Tier;
		mFrameLights.push_back(light);
		mKeys.push_back({ p.Source.Tier, Score(p.State.Color, p.State.Radius, p.Distance), p.Source.Sequence });
	}

	// Drawn-line lights: the line's own ends this frame, its colour and intensity times the light's.
	int lineLights = 0;
	for (int i = 0; i < Level->DrawnLineHigh && (unsigned)i < Level->DrawnLines.Size() && lineLights < EFFECT_LIGHT_LINE_LIGHTS_MAX; i++)
	{
		const FLevelLocals::DrawnLine& l = Level->DrawnLines[i];
		if (!l.Live || !(l.LightRadius > 0.0))
			continue;
		const double gain = l.Intensity * l.LightIntensity;
		if (!(gain > 0.0))
			continue;
		if ((l.LightFlags & EFL_CONSUMER_MASK) == EFL_CONSUMER_MASK)
			continue;
		DVector3 a, b;
		if (!ResolveDrawnLineEnds(Level, i, ticFrac, a, b))
			continue;
		const Vec3 av = ToVec3(a), bv = ToVec3(b);
		const double distance = DistanceToSegment(av, bv, eyeAt);
		if (distance - l.LightRadius > reach)
			continue;
		const float colour[3] = {
			(float)std::min(l.Color.r / 255.0 * gain, 64.0),
			(float)std::min(l.Color.g / 255.0 * gain, 64.0),
			(float)std::min(l.Color.b / 255.0 * gain, 64.0) };
		BinLight light;
		WriteRecord(light.Record, av, bv, l.LightRadius, l.LightFlags, colour, -1, av, l.LightEndBrightness);
		light.Tier = TIER_LINE;
		mFrameLights.push_back(light);
		mKeys.push_back({ TIER_LINE, Score(colour, l.LightRadius, distance), (uint64_t)i });
		lineLights++;
	}

	// Best first, then the bins.
	const size_t count = RankAndKeep(mKeys, mKeys.size(), mOrder);
	mRanked.resize(count);
	for (size_t k = 0; k < count; k++)
		mRanked[k] = mFrameLights[mOrder[k]];
	const Grid grid = GridAround(eye.X, eye.Y, eye.Z, GridForQuality(*r_effectlights_quality));
	// [GOVERNOR] E8: the lights one bin lights with, through the effects budget governor (hw_effectsgovernor.h): r_effectlights_perbin
	// as it is unless the governor is trimming; the rest of a crowded bin blend into its glow, as always.
	mBuilder.Build(mRanked, grid, EffectsGovernor::LightsPerBin(*r_effectlights_perbin), mResult);

	buffer->Upload(mResult.Records.data(), (unsigned)mResult.Records.size(), mResult.Binned, mResult.Grid,
		mResult.Bins.data(), (unsigned)mResult.Bins.size(), mResult.Indices.data(), (unsigned)mResult.Indices.size());

	stats.Live += lineLights;
	stats.Binned = (int)mResult.Binned;
	stats.Merged = mResult.Merged;
	stats.NoRow = noRow;
	stats.Trimmed = mResult.Trimmed;
	stats.Lines = mResult.Lines;

	if (!mFirstBinLogged && mResult.Binned > 0)
	{
		mFirstBinLogged = true;
		Printf("EffectLights: first light binned this run -- %u lights, %d glows, %u bins of %d x %d x %d at %g units, %u indices\n",
			mResult.Binned, mResult.Merged, (unsigned)mResult.Bins.size(), mResult.Grid.Size[0], mResult.Grid.Size[1], mResult.Grid.Size[2],
			(double)mResult.Grid.Corner[3], (unsigned)mResult.Indices.size());
	}

	if (timed)
		PerfLog::AddCpuSample("fx.effectlights", (double)(I_nsTime() - startNs) / 1e6);
}

// ---------------------------------------------------------------------------------------------------------------------------
// r_effectlights_test: about N sparks alive ahead of the view -- radius 48, 0.6 s, a hashed spray under gravity, every other
// one landing and holding (the owner's answer 5) -- and a radius-128, 600-unit line light crossing the view every half second
// (#22). A local renderer test: no actor, no RNG, nothing queued on the level. The level clock drives it, so a paused game
// spawns nothing.
// ---------------------------------------------------------------------------------------------------------------------------

void EffectLights::MakeTestLights(double dt, double yaw)
{
	const int wanted = std::clamp((int)*r_effectlights_test, 0, EFFECT_LIGHT_POOL_MAX);
	const double kLife = 0.6;
	const double fx = std::cos(yaw), fy = std::sin(yaw);
	const double rx = fy, ry = -fx;	// the view's right, game axes

	if (wanted > 0 && dt > 0.0)
	{
		mTestCarry = std::min(mTestCarry + wanted / kLife * dt, (double)wanted);
		const int n = (int)mTestCarry;
		mTestCarry -= n;
		for (int i = 0; i < n; i++)
		{
			const uint32_t seed = Hash(++mTestCounter * 0x9E3779B1U ^ 0x6a09e667U);
			auto share = [seed](uint32_t stream) { return (double)(Hash(seed ^ Hash(stream * 0x85EBCA77U + 0x68bc21ebU)) >> 8) / 16777216.0; };
			Source s;
			const double side = (share(1) - 0.5) * 96.0;
			s.A0 = { mEye.X + fx * 192.0 + rx * side, mEye.Y + fy * 192.0 + ry * side, mEye.Z - 16.0 + (share(2) - 0.5) * 32.0 };
			s.B0 = s.A0;
			const double angle = share(3) * 2.0 * kPi;
			const double speed = 40.0 + share(4) * 160.0;
			s.Vel = { std::cos(angle) * speed, std::sin(angle) * speed, 120.0 + share(5) * 200.0 };
			s.Color[0] = 1.f;
			s.Color[1] = (float)(0.45 + 0.35 * share(6));
			s.Color[2] = (float)(0.12 + 0.2 * share(7));
			s.Radius = 48.0;
			s.Intensity = 1.0;
			s.Birth = mNow;
			s.Life = kLife;
			s.Fade = 1.0;
			s.Gravity = 600.0;
			s.Drag = 0.5;
			if (mTestCounter & 1)
			{
				s.Land = 0.25 + 0.15 * share(8);
				s.Hold = 0.6;
			}
			s.Tier = TIER_NORMAL;
			s.Seed = seed;
			s.Sequence = ++mSequence;
			mNew.push_back(s);
		}
	}

	if (wanted > 0 && mNow >= mTestLineNext)
	{
		mTestLineNext = mNow + 0.5;
		Source s;
		s.A0 = { mEye.X + fx * 320.0 - rx * 900.0, mEye.Y + fy * 320.0 - ry * 900.0, mEye.Z };
		s.B0 = { s.A0.X - rx * 600.0, s.A0.Y - ry * 600.0, s.A0.Z };
		s.Vel = { rx * 3600.0, ry * 3600.0, 0.0 };
		s.Color[0] = 1.f;
		s.Color[1] = 0.85f;
		s.Color[2] = 0.5f;
		s.Radius = 128.0;
		s.Intensity = 1.5;
		s.Birth = mNow;
		s.Life = 0.5;
		s.Fade = 0.0;
		s.TailBrightness = 0.0;
		s.Flags = EFL_IMPORTANT;
		s.Tier = TIER_IMPORTANT;
		s.Seed = Hash(++mTestCounter);
		s.Sequence = ++mSequence;
		mNew.push_back(s);
	}
}
