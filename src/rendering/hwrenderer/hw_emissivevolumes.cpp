/*
** hw_emissivevolumes.cpp
**
** [EMISSIVEVOLUMES] Emissive volumes: the level's queue, the pool, the frame's lights and list. See hw_emissivevolumes.h; the
** maths is hw_emissivevolumecore.h.
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

#include "hw_emissivevolumes.h"
#include "hw_emissivevolumecore.h"
#include "hw_emissivevolumeframe.h"
#include "hw_effectlights.h"
#include "hw_perflog.h"
#include "hw_effectsgovernor.h"	// [GOVERNOR] E8: the volumes drawn a frame
#include "volumedefs.h"
#include "g_levellocals.h"
#include "d_player.h"
#include "r_utility.h"
#include "doomdef.h"
#include "c_cvars.h"
#include "v_video.h"
#include "i_time.h"
#include "printf.h"

using namespace EmissiveVolumeCore;

// ---------------------------------------------------------------------------------------------------------------------------
// The switches. Every one is read by the renderer every frame, so it responds with a menu open (a menu pauses the game:
// volumes stop ageing, but a change to what draws, how long, how it moves or how bright shows at once). Local: nothing here
// reaches another machine. The drawing's quality switches (steps, resolution) are hw_postprocess_cvars.cpp's.
// ---------------------------------------------------------------------------------------------------------------------------

// "Volumetric flashes" (owner answer 3): which classes of definition draw. Off also stops their lights and frees nothing
// but the pool; a mod asks LevelLocals.EmissiveVolumeEnabled to draw today's cone and flame card instead.
CUSTOM_CVARD(Int, r_emissivevolumes, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "volumetric flashes: 3 all, 2 big guns and blasts, 1 explosions only, 0 off (Vulkan only)")
{
	if (self < 0) self = 0;
	else if (self > 3) self = 3;
}

// "Flash length" (owner answer 1): 0 each definition's own life (by gun), 1 every flash under 1/30 s, 2 every flash about 1/4 s.
// A blast keeps its own life.
CUSTOM_CVARD(Int, r_emissivevolumes_length, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "flash length: 0 by gun, 1 snap (under 1/30 s), 2 linger (about 1/4 s); blasts keep their own (Vulkan only)")
{
	if (self < 0) self = 0;
	else if (self > 2) self = 2;
}

// "Flash motion" (owner answer 6): 0 each definition's own motion, 1 every volume stays where it was fired, 2 every volume rides
// the gun.
CUSTOM_CVARD(Int, r_emissivevolumes_motion, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "flash motion: 0 per definition, 1 stays where fired, 2 rides the gun (Vulkan only)")
{
	if (self < 0) self = 0;
	else if (self > 2) self = 2;
}

CUSTOM_CVARD(Int, r_emissivevolumes_max, 128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "the most emissive volumes alive at once, 32-512; past it the lowest-ranked go first (Vulkan only)")
{
	if (self < 32) self = 32;
	else if (self > 512) self = 512;
}

// Owner answer 2: each volume lights the haze around it in its own colour, flickering with it.
CVARD(Bool, r_emissivevolumes_light, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "emissive volumes throw their effect lights, lighting the haze around them (Vulkan only)")

CUSTOM_CVARD(Float, r_emissivevolumes_brightness, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "a live scale over every emissive volume's brightness and light, 0-4 (Vulkan only)")
{
	if (self < 0.f) self = 0.f;
	else if (self > 4.f) self = 4.f;
}

// A test, not a setting: not archived, like r_effectlights_test.
CUSTOM_CVARD(Int, r_emissivevolumes_test, 0, CVAR_GLOBALCONFIG, "N test emissive volumes a second about 192 units ahead of you -- a star, a gout, a ring and a fireball in turn, each with a light (Vulkan only)")
{
	if (self < 0) self = 0;
	else if (self > 16) self = 16;
}

namespace
{
	// hw_effectlights.cpp's ReadQueue (after hw_debrispool.cpp's and hw_smokevolume.cpp's), with the event's place in its
	// generation passed along: a volume's seed is keyed on it, so it does not depend on how frames split the tics. Every event
	// not read before, the older generation first, from this reader's own cursor. Nothing is written to the queue.
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
	const size_t kSpawnedMax = 512;

	// The pose a volume follows, as the VR backend last wrote it for that player (ResolveTrackedPose): never the console
	// player's unless the spawn named it.
	Pose PoseNow(FLevelLocals* Level, const EmissiveVolumeCore::Spawn& s)
	{
		Pose pose;
		if (Level == nullptr || s.Follow == FOLLOW_WORLD || !s.FollowValid)
			return pose;
		if (s.FollowPlayer < 0 || s.FollowPlayer >= MAXPLAYERS || !Level->PlayerInGame(s.FollowPlayer))
			return pose;
		DVector3 pos;
		DAngle yaw, pitch;
		const int source = ResolveTrackedPose(Level, s.Follow, DVector3(0., 0., 0.), pos, yaw, pitch, &players[s.FollowPlayer]);
		if (source == TPOSE_NONE || source == TPOSE_NOPLAYER)
			return pose;
		pose.Valid = true;
		pose.Pos = ToVec3(pos);
		pose.Yaw = yaw.Radians();
		pose.Pitch = pitch.Radians();
		return pose;
	}

	// The test source's four looks: a star (gun), a long gout (big gun), a brake ring (big gun), a fireball (blast).
	const Definition& TestDefinition(int kind)
	{
		static Definition looks[4];
		static bool made = false;
		if (!made)
		{
			made = true;
			Definition& star = looks[0];
			star.Class = CLASS_GUN;
			star.BaseRadius = star.TipRadius = 5.0;
			star.PetalCount = 5; star.PetalLength = 16.0; star.PetalWidth = 5.0; star.PetalSpread = 55.0;
			star.Life = 0.08;
			star.IntensityKeys = 2; star.IntensityTime[0] = 0.f; star.IntensityValue[0] = 6.f; star.IntensityTime[1] = 1.f; star.IntensityValue[1] = 0.f;
			star.LightRadius = 160.0; star.LightIntensity = 1.5;
			star.NameHash = NameHash("r_emissivevolumes_test star");

			Definition& gout = looks[1];
			gout.Class = CLASS_BIGGUN;
			gout.Length = 110.0; gout.BaseRadius = 9.0; gout.TipRadius = 3.0;
			gout.PetalCount = 6; gout.PetalLength = 22.0; gout.PetalWidth = 6.0; gout.PetalSpread = 30.0;
			gout.NoiseAmount = 0.6; gout.Billow = 120.0;
			gout.Life = 0.14;
			gout.IntensityKeys = 3; gout.IntensityTime[0] = 0.f; gout.IntensityValue[0] = 5.f; gout.IntensityTime[1] = 0.3f; gout.IntensityValue[1] = 3.f;
			gout.IntensityTime[2] = 1.f; gout.IntensityValue[2] = 0.f;
			gout.LightRadius = 220.0; gout.LightIntensity = 1.5; gout.LightLine = 1;
			gout.NameHash = NameHash("r_emissivevolumes_test gout");

			Definition& ring = looks[2];
			ring.Class = CLASS_BIGGUN;
			ring.Length = 18.0; ring.BaseRadius = 4.0; ring.TipRadius = 2.0;
			ring.RingRadius = 16.0; ring.RingThickness = 5.0; ring.RingWeight = 1.0;
			ring.Life = 0.12;
			ring.IntensityKeys = 2; ring.IntensityTime[0] = 0.f; ring.IntensityValue[0] = 5.f; ring.IntensityTime[1] = 1.f; ring.IntensityValue[1] = 0.f;
			ring.LightRadius = 160.0; ring.LightIntensity = 1.2;
			ring.NameHash = NameHash("r_emissivevolumes_test ring");

			Definition& ball = looks[3];
			ball.Class = CLASS_BLAST;
			ball.BaseRadius = ball.TipRadius = 36.0;
			ball.GrowStart = 0.2; ball.GrowShare = 0.4; ball.Expand = 40.0;
			ball.NoiseAmount = 0.7; ball.Billow = 60.0; ball.Churn = 2.0;
			ball.Life = 0.5;
			ball.HeatStart = 1.0; ball.HeatEnd = 0.25; ball.HeatFalloff = 0.8;
			ball.IntensityKeys = 3; ball.IntensityTime[0] = 0.f; ball.IntensityValue[0] = 4.f; ball.IntensityTime[1] = 0.5f; ball.IntensityValue[1] = 2.f;
			ball.IntensityTime[2] = 1.f; ball.IntensityValue[2] = 0.f;
			ball.Absorption = 0.6;
			ball.LightRadius = 320.0; ball.LightIntensity = 1.5;
			ball.NameHash = NameHash("r_emissivevolumes_test fireball");
		}
		return looks[std::clamp(kind, 0, 3)];
	}

	// The effect light flags for a volume's light: flashes always light (answer 6 of the lights plan: important), and each
	// consumer it does not light is marked off.
	int EffectLightFlagsFor(int lightFlags)
	{
		int flags = EffectLightCore::EFL_IMPORTANT;
		if (!(lightFlags & LIGHT_SURFACES)) flags |= EffectLightCore::EFL_NOSURFACES;
		if (!(lightFlags & LIGHT_SMOKE)) flags |= EffectLightCore::EFL_NOSMOKE;
		if (!(lightFlags & LIGHT_PARTICLES)) flags |= EffectLightCore::EFL_NOPARTICLES;
		return flags;
	}
}

bool EmissiveVolumeDefinitionEnabled(int handle)
{
	if (screen == nullptr || !screen->IsVulkan())
		return false;
	const Definition* d = FindEmissiveVolumeDefinition(handle, false);
	return d != nullptr && ClassEnabled(d->Class, *r_emissivevolumes);
}

EmissiveVolumes& EmissiveVolumes::Get()
{
	static EmissiveVolumes volumes;
	return volumes;
}

void EmissiveVolumes::Spawn(const EmissiveVolumeCore::Spawn& spawn)
{
	if (mSpawned.size() < kSpawnedMax)
		mSpawned.push_back(spawn);
}

EmissiveVolumeCore::Settings EmissiveVolumes::CurrentSettings() const
{
	Settings settings;
	settings.LengthMode = std::clamp((int)*r_emissivevolumes_length, 0, 2);
	settings.MotionMenu = std::clamp((int)*r_emissivevolumes_motion, 0, 2);
	settings.Brightness = std::clamp((double)(float)*r_emissivevolumes_brightness, 0.0, 4.0);
	return settings;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Step 1
// ---------------------------------------------------------------------------------------------------------------------------

void EmissiveVolumes::BeginFrame(FLevelLocals* Level, const DVector3& eye, double yaw, double ticFrac, uint64_t levelSerial)
{
	mBegan = false;
	EmissiveVolumeFrameStats& stats = EmissiveVolumeStats();
	stats = EmissiveVolumeFrameStats();
	if (Level == nullptr)
		return;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	// Vulkan only, like every effect since the particle stage 2a: GL and GLES have no pass.
	const int setting = std::clamp((int)*r_emissivevolumes, 0, 3);
	mActive = screen != nullptr && screen->IsVulkan() && setting > SETTING_OFF;

	if (levelSerial != mLevelSerial)
	{
		// A new map or a savegame load: the old map's volumes go with it (ClearLevelData emptied the queue).
		mLevelSerial = levelSerial;
		mQueueSerial = 0;
		mQueueCount = 0;
		mPool.clear();
		mSpawned.clear();
		mLastNow = -1.0;
		mTestCarry = 0.0;
	}

	// The draw's level clock, exactly as StartScene builds mLevelTime: it stops while the game is paused.
	// RS FORK -- WORLD CLOCK: the shared value (FLevelLocals::WorldSeconds), which with
	// no slow motion IS (maptime + ticFrac) / TICRATE.
	mNow = Level->WorldSeconds(ticFrac);
	const double dt = mLastNow >= 0.0 ? std::max(mNow - mLastNow, 0.0) : 0.0;
	mLastNow = mNow;
	mEye = eye;

	// The level's new volumes. Always read, so the cursor keeps up; kept only while volumes are on and their class draws.
	mNew.clear();
	int refused = 0;
	ReadQueue(Level->EmissiveVolumeSpawns, mQueueSerial, mQueueCount, [&](const FEmissiveVolumeEvent& e, int tic, int index)
	{
		if (!mActive)
			return;
		const Definition* definition = FindEmissiveVolumeDefinition(e.Definition, true);
		if (definition == nullptr || !ClassEnabled(definition->Class, setting))
		{
			refused++;
			return;
		}
		EmissiveVolumeCore::Spawn s;
		s.Def = *definition;
		s.Pos = ToVec3(e.Pos);
		s.Dir = ToVec3(e.Dir);
		s.Vel = ToVec3(e.Vel);
		s.Scale = e.Scale;
		s.Brightness = e.Brightness;
		s.LifeScale = e.LifeScale;
		s.FollowShare = e.FollowShare;
		s.LightScale = e.LightScale;
		const PalEntry tint(e.Tint);
		s.Tint[0] = tint.r / 255.f;
		s.Tint[1] = tint.g / 255.f;
		s.Tint[2] = tint.b / 255.f;
		s.Seed = e.Seed != 0 ? (uint32_t)e.Seed : SeedFor(tic, index, s.Pos);
		s.Birth = tic / (double)TICRATE;	// a particle's birth: maptime / TICRATE of the tic that spawned it
		s.Follow = e.Follow;
		s.FollowPlayer = e.FollowPlayer;
		s.FollowValid = e.FollowValid;
		s.FollowAt.Valid = e.FollowValid;
		s.FollowAt.Pos = ToVec3(e.FollowPos);
		s.FollowAt.Yaw = e.FollowYaw;
		s.FollowAt.Pitch = e.FollowPitch;
		s.Sequence = ++mSequence;
		mNew.push_back(s);
	});

	if (mActive)
	{
		for (EmissiveVolumeCore::Spawn& s : mSpawned)
		{
			if (!ClassEnabled(s.Def.Class, setting))
			{
				refused++;
				continue;
			}
			s.Sequence = ++mSequence;
			mNew.push_back(s);
		}
		if (*r_emissivevolumes_test > 0)
			MakeTestVolumes(dt, yaw);
	}
	mSpawned.clear();

	if (!mActive)
	{
		mPool.clear();
		if (timed)
			PerfLog::AddCpuSample("fx.emissive", (double)(I_nsTime() - startNs) / 1e6);
		return;
	}

	// Every volume at this frame's clock, with the followed poses as they stand; the gone go, and so do volumes of a class
	// "Volumetric flashes" no longer draws. New volumes already gone never enter.
	const Settings settings = CurrentSettings();
	const Vec3 eyeAt = ToVec3(eye);
	size_t live = 0;
	for (size_t i = 0; i < mPool.size(); i++)
	{
		PoolVolume& p = mPool[i];
		p.State = Evaluate(p.Source, mNow, PoseNow(Level, p.Source), settings);
		if (!p.State.Alive || !ClassEnabled(p.Source.Def.Class, setting))
			continue;
		if (live != i)
			mPool[live] = std::move(mPool[i]);
		live++;
	}
	mPool.resize(live);
	for (const EmissiveVolumeCore::Spawn& s : mNew)
	{
		PoolVolume p;
		p.Source = s;
		p.State = Evaluate(s, mNow, PoseNow(Level, s), settings);
		if (p.State.Alive)
			mPool.push_back(std::move(p));
	}
	for (PoolVolume& p : mPool)
		p.Distance = Length(Sub(BoundCentre(p.State), eyeAt));

	// The pool's budget (2i): the best r_emissivevolumes_max stay, in rank order -- the same set as "a new volume replaces the
	// lowest-ranked when it ranks above it, else is refused" (every new volume is younger than every live one).
	const size_t cap = (size_t)std::clamp((int)*r_emissivevolumes_max, 32, 512);
	if (mPool.size() > cap)
	{
		mOrder.resize(mPool.size());
		for (size_t i = 0; i < mPool.size(); i++)
			mOrder[i] = (uint32_t)i;
		std::sort(mOrder.begin(), mOrder.end(), [&](uint32_t a, uint32_t b)
		{
			const double ra = Rank(mPool[a].State, mPool[a].Distance), rb = Rank(mPool[b].State, mPool[b].Distance);
			if (ra != rb) return ra > rb;
			return mPool[a].Source.Sequence < mPool[b].Source.Sequence;
		});
		mPoolScratch.clear();
		for (size_t k = 0; k < cap; k++)
			mPoolScratch.push_back(std::move(mPool[mOrder[k]]));
		refused += (int)(mPool.size() - cap);
		mPool.swap(mPoolScratch);
	}

	// Their lights (2f): one frame light each, before the effect lights' BeginFrame takes them.
	int lights = 0;
	if (r_emissivevolumes_light)
	{
		for (const PoolVolume& p : mPool)
		{
			Light light;
			if (!LightFor(p.State, p.Source, light) || (light.Flags & LIGHT_ALL) == 0)
				continue;
			EffectLights::FrameLight f;
			f.A = { light.A.X, light.A.Y, light.A.Z };
			f.B = { light.B.X, light.B.Y, light.B.Z };
			for (int c = 0; c < 3; c++)
				f.Color[c] = light.Color[c];
			f.Radius = light.Radius;
			f.Flags = EffectLightFlagsFor(light.Flags);
			f.TailBrightness = 1.0;
			if (EffectLights::Get().AddFrameLight(f))
				lights++;
		}
	}

	stats.Live = (int)mPool.size();
	stats.Refused = refused;
	stats.Lights = lights;
	mBegan = true;

	if (timed)
		PerfLog::AddCpuSample("fx.emissive", (double)(I_nsTime() - startNs) / 1e6);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Step 2
// ---------------------------------------------------------------------------------------------------------------------------

void EmissiveVolumes::PrepareFrame(FLevelLocals* Level, const DVector3& eye, EmissiveVolumeFrame& out)
{
	out = EmissiveVolumeFrame();
	mDraw = DrawState();
	if (Level == nullptr || !mBegan || !mActive || mPool.empty())
		return;

	const bool timed = PerfLog::GroupsWanted();
	const uint64_t startNs = timed ? I_nsTime() : 0;

	// Every volume with the poses VRMode::SetUp wrote THIS frame: a root on a muzzle stays on it at frame rate.
	const Settings settings = CurrentSettings();
	const Vec3 eyeAt = ToVec3(eye);
	mOrder.clear();
	for (size_t i = 0; i < mPool.size(); i++)
	{
		PoolVolume& p = mPool[i];
		p.State = Evaluate(p.Source, mNow, PoseNow(Level, p.Source), settings);
		p.Distance = Length(Sub(BoundCentre(p.State), eyeAt));
		if (Visible(p.State) && Finite(p.State.Base))
			mOrder.push_back((uint32_t)i);
	}

	// The best EMISSIVE_VOLUMES_DRAWN_MAX by rank; the rest are skipped this frame, not killed.
	std::sort(mOrder.begin(), mOrder.end(), [&](uint32_t a, uint32_t b)
	{
		const double ra = Rank(mPool[a].State, mPool[a].Distance), rb = Rank(mPool[b].State, mPool[b].Distance);
		if (ra != rb) return ra > rb;
		return mPool[a].Source.Sequence < mPool[b].Source.Sequence;
	});
	// [GOVERNOR] E8: the effects budget governor may draw fewer while it trims (EffectsGovernor::EmissiveDrawn: 24, then 16) -- still
	// the best by rank. The rest stay alive and keep their lights, as any volume past the list does.
	const size_t drawnMax = (size_t)EffectsGovernor::EmissiveDrawn(EMISSIVE_VOLUMES_DRAWN_MAX);
	if (mOrder.size() > drawnMax)
		mOrder.resize(drawnMax);
	// Drawn far to near from the head centre, so each nearer volume composites over the farther ones.
	std::sort(mOrder.begin(), mOrder.end(), [&](uint32_t a, uint32_t b)
	{
		if (mPool[a].Distance != mPool[b].Distance) return mPool[a].Distance > mPool[b].Distance;
		return mPool[a].Source.Sequence < mPool[b].Source.Sequence;
	});

	const int count = (int)mOrder.size();
	if (count > 0)
	{
		// The list origin: the eye at whole map units, so positions stay small in a float.
		mDraw.Origin[0] = std::floor(eye.X);
		mDraw.Origin[1] = std::floor(eye.Y);
		mDraw.Origin[2] = std::floor(eye.Z);
		mTexels.assign((size_t)EMISSIVE_VOLUMES_DRAWN_MAX * EMISSIVE_VOLUME_TEXELS * 4, 0.f);
		float rows[EMISSIVE_VOLUME_TEXELS][4];
		for (int column = 0; column < count; column++)
		{
			const PoolVolume& p = mPool[mOrder[column]];
			WriteTexels(p.State, p.Source.Def, mDraw.Origin, rows);
			for (int row = 0; row < EMISSIVE_VOLUME_TEXELS; row++)
				memcpy(&mTexels[((size_t)row * EMISSIVE_VOLUMES_DRAWN_MAX + (size_t)column) * 4], rows[row], sizeof(float) * 4);
			const Vec3 centre = BoundCentre(p.State);
			mDraw.Centre[column][0] = (float)(centre.X - mDraw.Origin[0]);
			mDraw.Centre[column][1] = (float)(centre.Z - mDraw.Origin[2]);
			mDraw.Centre[column][2] = (float)(centre.Y - mDraw.Origin[1]);
			mDraw.Radius[column] = (float)p.State.Bound.Radius;
			if (p.State.Extinction > 0.0)
				mDraw.Absorbs = true;
		}
		if (mTexels != mLastTexels)
		{
			mLastTexels = mTexels;
			mListSerial++;
		}
		mDraw.Count = count;
		mDraw.Serial = mListSerial;
		out.Count = count;
		out.Texels = mTexels.data();
		out.Serial = mListSerial;

		if (!mFirstDrawLogged)
		{
			mFirstDrawLogged = true;
			Printf("EmissiveVolumes: first volume drawn this run -- %d in the list, %d alive\n", count, (int)mPool.size());
		}
	}
	EmissiveVolumeStats().Drawn = count;

	if (timed)
		PerfLog::AddCpuSample("fx.emissive", (double)(I_nsTime() - startNs) / 1e6);
}

// ---------------------------------------------------------------------------------------------------------------------------
// r_emissivevolumes_test: N volumes a second about 192 units ahead of the view, cycling a star facing you, a gout across the
// view, a brake ring and a fireball, each with a light. A local renderer test: no actor, no RNG, nothing queued on the level.
// The level clock drives it, so a paused game spawns nothing. "Volumetric flashes" still decides which of them draw.
// ---------------------------------------------------------------------------------------------------------------------------

void EmissiveVolumes::MakeTestVolumes(double dt, double yaw)
{
	const int wanted = std::clamp((int)*r_emissivevolumes_test, 0, 16);
	if (wanted <= 0 || !(dt > 0.0))
		return;
	const int setting = std::clamp((int)*r_emissivevolumes, 0, 3);
	const double fx = std::cos(yaw), fy = std::sin(yaw);
	const double rx = fy, ry = -fx;	// the view's right, game axes

	mTestCarry = std::min(mTestCarry + wanted * dt, (double)wanted);
	const int n = (int)mTestCarry;
	mTestCarry -= n;
	for (int i = 0; i < n; i++)
	{
		const uint32_t counter = ++mTestCounter;
		const uint32_t seed = Hash(counter * 0x9E3779B1U ^ 0x6a09e667U);
		const int kind = (int)(counter % 4);
		const Definition& definition = TestDefinition(kind);
		if (!ClassEnabled(definition.Class, setting))
			continue;
		EmissiveVolumeCore::Spawn s;
		s.Def = definition;
		const double side = (Share(seed, 1) - 0.5) * 96.0;
		s.Pos = { mEye.X + fx * 192.0 + rx * side, mEye.Y + fy * 192.0 + ry * side, mEye.Z - 8.0 + (Share(seed, 2) - 0.5) * 24.0 };
		switch (kind)
		{
		case 0: s.Dir = { -fx, -fy, 0.0 }; break;								// the star faces you
		case 1: s.Dir = (counter & 8) ? Vec3{ rx, ry, 0.0 } : Vec3{ -rx, -ry, 0.0 }; s.Pos = { s.Pos.X - rx * 55.0 * ((counter & 8) ? 1.0 : -1.0), s.Pos.Y - ry * 55.0 * ((counter & 8) ? 1.0 : -1.0), s.Pos.Z }; break;	// the gout crosses the view
		case 2: s.Dir = { fx, fy, 0.0 }; break;									// the ring's axis away from you
		default: s.Dir = { 0.0, 0.0, 1.0 }; break;								// the fireball rises
		}
		s.Seed = seed;
		s.Birth = mNow;
		s.Sequence = ++mSequence;
		mNew.push_back(s);
	}
}
