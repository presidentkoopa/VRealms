/*
** hw_particlelights.h
**
** [PARTICLELIGHTS] Particle and debris lights: which particles of a burst throw an effect light, and how that light flies,
** lands and holds. The maths, with no engine dependency.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/EFFECT_LIGHTS_LC_IMPL_NOTES.md"; "Engine docs/LIGHTS_20_21_22_PLAN.md" 2e, 2f, 4 and its OWNER ANSWERS 1, 5, 6.
**
** A PARTICLEDEFS definition with `light` throws light (particledefs.h, ParticleLightDefinition). Two places make its lights,
** both renderer-side, both from what they already write:
**   - GpuParticleBuffer::Sync (hw_gpuparticlebuffer.cpp): the particle ring's records, as they go up to the GPU;
**   - DebrisPool::ExpandBurst (hw_debrispool.cpp): the debris pool's pieces, as a burst is expanded.
** Each hands EffectLights::Spawn (hw_effectlights.h) one EffectLightCore::Source per particle that carries a light and forgets
** it: the core moves it at frame rate, fades it, ranks it against the budget (small, far lights go first -- answer 6), bins it
** and blends a crowd into one glow (answer 1).
**
**   WHICH PARTICLES: of each burst, the records a hash of their own bytes picks for its `lightshare`, at most `lightmax` of
**     them. A pure function of the records, so every machine, at any frame rate and however frames split the tics, makes the
**     same lights. Never an RNG.
**   HOW IT FLIES: the particle's own closed-form flight (gpuparticles.vp's), from its record. A streak lights as a line along
**     its drawn length (`lightline`). Its brightness over life is `lightramp` -- 1, fading as `fade = smooth`, when left off.
**   WHERE IT LANDS: a ring particle that collides, where its closed-form flight comes down onto its floor or a floor-like plane
**     (meshparticles.vp's landing solve); a debris piece, where #11 predicts its first landing (DebrisLanding,
**     hw_debrislanding.h), drawn one step behind like the piece. With `lighthold` the landed light sits a little above the
**     surface -- so it lights it -- and cools there for its own hashed share of the hold (answer 5); without, it goes out.
**
** Presentation only: nothing here reads or writes the playsim, and nothing is read back from the GPU.
**
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "hw_effectlightcore.h"

namespace ParticleLights
{
	using EffectLightCore::Vec3;

	// The PARTICLEDEFS limits (particledefs.cpp refuses outside them; the renderer clamps to them again).
	inline constexpr double kMaxRadius = 1024.0;		// EffectLightCore's radius clamp
	inline constexpr double kMaxIntensity = 16.0;		// and its intensity clamp
	inline constexpr double kMaxBrightness = 16.0;		// a lightramp value
	inline constexpr int kMostPerBurst = 4096;			// lightmax, EFFECT_LIGHT_POOL_MAX
	inline constexpr double kMaxHold = 60.0;			// lighthold, seconds
	// Their defaults.
	inline constexpr double kDefaultShare = 1.0;
	inline constexpr int kDefaultMax = 16;
	inline constexpr int kDefaultPriority = 1;

	// A landed light sits this share of its radius above where it landed, at least kRiseMin and at most kRiseMax map units: a
	// light level with a surface meets it edge on (no N.L), so it would not light the floor it lies on.
	inline constexpr double kRiseShare = 0.25;
	inline constexpr double kRiseMin = 4.0;
	inline constexpr double kRiseMax = 32.0;
	// How a landed light cools over its hold (EffectLightCore::Source::HoldFade): straight down from its landing brightness.
	inline constexpr double kHoldFade = 1.0;
	// A surface whose normal points this far up is one a particle lands on (meshparticles.vp's kMeshFloorLike, #11's kFloorUp).
	inline constexpr double kFloorLike = 0.7;
	// The debris pool's step (DEBRIS_STEP_SECONDS): a piece is drawn one step behind its flight.
	inline constexpr double kDebrisStepSeconds = 1.0 / 35.0;
	// The earliest landing a light keeps: EffectLightCore::Landed needs one above 0.
	inline constexpr double kMinLand = 1.0e-4;

	// What r_particlelights_test gives a definition that glows and has no light keys.
	inline constexpr double kTestRadius = 48.0;
	inline constexpr double kTestHold = 1.0;

	// A particle record's 20 floats (FLevelLocals::GpuParticleRecord; a debris piece's Spawn), SHADER axes:
	//   a xyz position, w birth   b xyz velocity, w life   c rgb tint, w intensity
	//   d x definition, y size scale, z ambient, w seed   e xy octahedral plane normal (x 2: none), z plane offset, w floor (-32768: none)
	enum
	{
		R_POS = 0, R_BIRTH = 3, R_VEL = 4, R_LIFE = 7, R_TINT = 8, R_INTENSITY = 11,
		R_DEFINITION = 12, R_SIZESCALE = 13, R_SEED = 15, R_PLANE = 16, R_PLANEOFFSET = 18, R_FLOOR = 19,
		RECORD_FLOATS = 20,
	};

	// 0 for anything not a number at or above 0, else at most `most`.
	inline double Bounded(double v, double most)
	{
		return v >= 0.0 ? std::min(v, most) : 0.0;
	}

	// ---------------------------------------------------------------------------------------------------------------------------
	// A definition's light
	// ---------------------------------------------------------------------------------------------------------------------------

	// One named definition's light, resolved from its keys and its GPU definition whenever the definitions load.
	struct SlotLight
	{
		bool Active = false;
		double Radius = 0.0;
		double Intensity = 1.0;
		float Color[3] = { 1.f, 1.f, 1.f };
		int CurveKeys = 0;
		float CurveTime[EffectLightCore::EFFECT_LIGHT_CURVE_KEYS] = {};
		float CurveValue[EffectLightCore::EFFECT_LIGHT_CURVE_KEYS] = {};
		bool CurveEased = false;
		double Share = kDefaultShare;
		int Max = kDefaultMax;
		double LineStretch = 0.0;	// a streak lighting as a line: the seconds of travel it draws; 0 = a point
		double Hold = 0.0;
		int Tier = EffectLightCore::TIER_NORMAL;
		double Gravity = 0.0;		// the definition's flight
		double Drag = 0.0;
		int Collide = 0;			// 0 none, 1 plane, 2 level
		double SizeKey = 0.0;		// its first size key (a diameter, map units)
		double MaxSize = 0.0;		// its own size cap (0: none)
	};

	// A key colour as ParticleDefinitionGpu stores it (0xRRGGBB in a float), 0..1 a channel.
	inline void KeyColor(float packed, float out[3])
	{
		const uint32_t bits = packed >= 0.f && packed < 16777216.f ? (uint32_t)packed : 0xFFFFFFU;
		out[0] = (float)((bits >> 16) & 255U) / 255.f;
		out[1] = (float)((bits >> 8) & 255U) / 255.f;
		out[2] = (float)(bits & 255U) / 255.f;
	}

	// Keys: ParticleLightDefinition (particledefs.h); Gpu: ParticleDefinitionGpu. Templates, so this header needs neither.
	template<class Keys, class Gpu>
	inline SlotLight Resolve(const Keys &k, const Gpu &g)
	{
		SlotLight l;
		l.Radius = Bounded(k.Radius, kMaxRadius);
		l.Intensity = Bounded(k.Intensity, kMaxIntensity);
		l.Active = l.Radius > 0.0 && l.Intensity > 0.0;
		if (k.HasColor)
		{
			for (int c = 0; c < 3; c++)
				l.Color[c] = (float)Bounded(k.Color[c], 1.0);
		}
		else
		{
			KeyColor(g.keyColor[0][0], l.Color);	// the colour ramp's first key
		}
		if (k.RampKeys > 0)
		{
			l.CurveKeys = std::min(k.RampKeys, EffectLightCore::EFFECT_LIGHT_CURVE_KEYS);
			for (int i = 0; i < l.CurveKeys; i++)
			{
				l.CurveTime[i] = k.RampTime[i];
				l.CurveValue[i] = (float)Bounded(k.RampValue[i], kMaxBrightness);
			}
		}
		else
		{
			// 1, fading as `fade = smooth` does: 1 - smoothstep(0.6, 1, t) is keys 1 @0, 1 @0.6, 0 @1 eased.
			l.CurveKeys = 3;
			l.CurveTime[0] = 0.f; l.CurveValue[0] = 1.f;
			l.CurveTime[1] = 0.6f; l.CurveValue[1] = 1.f;
			l.CurveTime[2] = 1.f; l.CurveValue[2] = 0.f;
			l.CurveEased = true;
		}
		l.Share = Bounded(k.Share, 1.0);
		l.Max = std::clamp(k.Max, 1, kMostPerBurst);
		const bool streak = (int)(g.shape[0] + 0.5f) == 1;
		const bool line = streak && (k.Line < 0 || k.Line == 1);	// left off: a streak lights as a line
		l.LineStretch = line ? Bounded(g.shape[1], 1.0e6) : 0.0;
		l.Hold = Bounded(k.Hold, kMaxHold);
		l.Tier = k.Priority <= 0 ? EffectLightCore::TIER_LOW : (k.Priority >= 2 ? EffectLightCore::TIER_IMPORTANT : EffectLightCore::TIER_NORMAL);
		l.Gravity = g.motion[0];
		l.Drag = g.motion[1];
		l.MaxSize = g.motion[2];
		l.Collide = std::clamp((int)(g.look[2] + 0.5f), 0, 2);
		l.SizeKey = g.key[0][1];
		return l;
	}

	// r_particlelights_test: a definition that glows at birth (emissive above 0) and hides nothing (alpha 0) is given the keys a
	// test light has -- a radius of kTestRadius, and where it collides a kTestHold hold. False for any other definition.
	template<class Keys, class Gpu>
	inline bool TestKeys(const Gpu &g, Keys &k)
	{
		if (!(g.key[0][3] > 0.f) || g.key[0][2] > 0.f)
			return false;
		k = Keys();
		k.Radius = (float)kTestRadius;
		k.Hold = (float)((int)(g.look[2] + 0.5f) >= 1 ? kTestHold : 0.0);
		return true;
	}

	// Every named slot's light: first the definitions' keys (`keys`, in slot order, each carrying its Slot), then with `test`
	// the test keys for a slot that glows and throws no light of its own. Returns whether any slot throws light.
	template<class Keys, class Gpu>
	inline bool ResolveSlots(const Keys *keys, unsigned keyCount, const Gpu *table, int slotCount, bool test, SlotLight *slots)
	{
		for (int slot = 0; slot < slotCount; slot++)
			slots[slot] = SlotLight();
		if (table == nullptr)
			return false;
		for (unsigned i = 0; keys != nullptr && i < keyCount; i++)
		{
			if (keys[i].Slot >= 0 && keys[i].Slot < slotCount)
				slots[keys[i].Slot] = Resolve(keys[i], table[keys[i].Slot]);
		}
		bool any = false;
		for (int slot = 0; slot < slotCount; slot++)
		{
			Keys testKeys;
			if (test && !slots[slot].Active && TestKeys(table[slot], testKeys))
				slots[slot] = Resolve(testKeys, table[slot]);
			any = any || slots[slot].Active;
		}
		return any;
	}

	// ---------------------------------------------------------------------------------------------------------------------------
	// Which particles of a burst
	// ---------------------------------------------------------------------------------------------------------------------------

	// A record's own hash: its seed, birth, velocity and position, as bits. Every record of a burst differs in its seed and
	// velocity, and the same record is the same bytes on every machine.
	inline uint32_t RecordHash(const float r[RECORD_FLOATS])
	{
		static const int fields[8] = { R_SEED, R_BIRTH, R_VEL, R_VEL + 1, R_VEL + 2, R_POS, R_POS + 1, R_POS + 2 };
		uint32_t h = 0x9E3779B9U;
		for (int i = 0; i < 8; i++)
		{
			uint32_t bits;
			std::memcpy(&bits, &r[fields[i]], sizeof(bits));
			h = EffectLightCore::Hash(h ^ (bits + (uint32_t)i * 0x85EBCA77U));
		}
		return h;
	}

	// Whether a record whose hash is `recordHash` is in the `share` of its burst that carries a light: 24 bits of the hash below
	// share. share 1 takes every record, 0 none.
	inline bool CarriesLight(uint32_t recordHash, double share)
	{
		return (double)(recordHash >> 8) / 16777216.0 < share;
	}

	// The seed its light's hold share comes from (EffectLightCore::HoldShare): another stream of the same hash.
	inline uint32_t HoldSeed(uint32_t recordHash)
	{
		return EffectLightCore::Hash(recordHash ^ 0x2545F491U);
	}

	// THE RING'S BURSTS. One SpawnParticles call writes its records together, one after another, and they share everything but
	// their velocity, life and seed: position and birth (a), tint and intensity (c), definition, size scale and ambient (d.xyz),
	// plane and floor (e). A run of consecutive records with those fields the same is one burst for `lightmax`. Two calls with
	// every one of those fields the same, one right after the other, count as one burst.
	struct BurstRun
	{
		float Key[15] = {};
		bool Valid = false;
		int Lights = 0;		// lights this burst has made

		void Take(const float r[RECORD_FLOATS])
		{
			float key[15];
			std::memcpy(key, &r[R_POS], 4 * sizeof(float));
			std::memcpy(key + 4, &r[R_TINT], 4 * sizeof(float));
			std::memcpy(key + 8, &r[R_DEFINITION], 3 * sizeof(float));
			std::memcpy(key + 11, &r[R_PLANE], 4 * sizeof(float));
			if (Valid && std::memcmp(key, Key, sizeof(key)) == 0)
				return;
			std::memcpy(Key, key, sizeof(key));
			Valid = true;
			Lights = 0;
		}
	};

	// ---------------------------------------------------------------------------------------------------------------------------
	// Landing
	// ---------------------------------------------------------------------------------------------------------------------------

	// gpuparticles.vp's OctahedralDecode (shader z is the pole).
	inline void OctahedralDecode(double ex, double ey, double n[3])
	{
		double x = ex, y = ey;
		const double z = 1.0 - std::fabs(ex) - std::fabs(ey);
		const double fold = std::max(-z, 0.0);
		x += (x >= 0.0) ? -fold : fold;
		y += (y >= 0.0) ? -fold : fold;
		const double length = std::sqrt(x * x + y * y + z * z);
		if (length > 0.0)
		{
			n[0] = x / length; n[1] = y / length; n[2] = z / length;
		}
		else
		{
			n[0] = 0.0; n[1] = 0.0; n[2] = 1.0;
		}
	}

	// meshparticles.vp's MeshContactGap: the closed-form flight's height above a surface, less `rest`, along its normal.
	inline double ContactGap(double gap, double along, double fall, double k, double t)
	{
		return gap + along * (1.0 - std::exp(-k * t)) / k - 0.5 * fall * t * t;
	}

	// When the closed-form flight (start, velocity, gravity, drag k; shader axes) first comes down through `rest` above a surface
	// (unit normal n, n . p = offset): 0 when it is already there, -1 when it never is within `life`.
	//
	// Gravity toward the surface (fall > 0): meshparticles.vp's MeshLandingTime, statement for statement but for 24 bisection steps
	// where it takes 12 -- a light is placed to a hundredth of a unit, where a resting chunk needed only a steady time. The height
	// rises to one peak and then only falls, so the peak and the crossing are bracketed by bisection and two Newton steps finish it.
	// Otherwise (no gravity toward it, or away from it) the height is convex: it only comes down before its lowest point, so the
	// lowest point is bracketed and the crossing bisected before it -- a spark thrown down with no gravity lands too.
	inline double LandingTime(const double start[3], const double velocity[3], double gravity, double k, const double n[3], double offset,
		double rest, double life)
	{
		const double gap = start[0] * n[0] + start[1] * n[1] + start[2] * n[2] - offset - rest;
		const double along = velocity[0] * n[0] + velocity[1] * n[1] + velocity[2] * n[2];
		const double fall = gravity * n[1];
		if (!(life > 0.0))
			return -1.0;
		if (fall <= 0.0)
		{
			if (gap <= 0.0)
				return 0.0;
			if (!(along < 0.0))
				return -1.0;
			// The lowest point, where along e^-kt = fall t (no gravity: the end of the life).
			double lowest = life;
			if (fall < 0.0)
			{
				double lower = 0.0;
				double upper = std::min(along / fall, life);
				for (int i = 0; i < 24; i++)
				{
					const double middle = 0.5 * (lower + upper);
					if (along * std::exp(-k * middle) < fall * middle)
						lower = middle;
					else
						upper = middle;
				}
				lowest = 0.5 * (lower + upper);
			}
			if (ContactGap(gap, along, fall, k, lowest) > 0.0)
				return -1.0;
			double lower = 0.0;
			double upper = lowest;
			for (int i = 0; i < 24; i++)
			{
				const double middle = 0.5 * (lower + upper);
				if (ContactGap(gap, along, fall, k, middle) > 0.0)
					lower = middle;
				else
					upper = middle;
			}
			return upper;
		}

		double peak = 0.0;
		if (along > 0.0)
		{
			double lower = 0.0;
			double upper = std::min(along / fall, life);
			for (int i = 0; i < 24; i++)
			{
				const double middle = 0.5 * (lower + upper);
				if (along * std::exp(-k * middle) > fall * middle)
					lower = middle;
				else
					upper = middle;
			}
			peak = 0.5 * (lower + upper);
		}
		if (ContactGap(gap, along, fall, k, peak) <= 0.0)
			return 0.0;
		if (ContactGap(gap, along, fall, k, life) >= 0.0)
			return -1.0;

		double lower = peak;
		double upper = life;
		for (int i = 0; i < 24; i++)
		{
			const double middle = 0.5 * (lower + upper);
			if (ContactGap(gap, along, fall, k, middle) > 0.0)
				lower = middle;
			else
				upper = middle;
		}
		double t = upper;
		if (along > 0.0 || -k * along * std::exp(-k * lower) < fall)
		{
			for (int i = 0; i < 2; i++)
			{
				const double slope = along * std::exp(-k * t) - fall * t;
				if (slope >= -1e-6)
					break;
				t = std::max(t - ContactGap(gap, along, fall, k, t) / slope, lower);
			}
		}
		return t;
	}

	// How far above where it landed a landed light sits.
	inline double RiseFor(double radius)
	{
		return std::clamp(radius * kRiseShare, kRiseMin, kRiseMax);
	}

	// ---------------------------------------------------------------------------------------------------------------------------
	// The light
	// ---------------------------------------------------------------------------------------------------------------------------

	// What the ring's lights read from the renderer's live tuning (hw_cvars.cpp), as gpuparticles.vp reads it.
	struct Tuning
	{
		double SizeScale = 1.0;		// r_gpuparticles_sizescale
		double MaxSize = 8.0;		// r_gpuparticles_maxsize
		double Stretch = 1.0;		// r_gpuparticles_stretch
	};

	// What every particle light takes from its definition and its record. Game axes out.
	inline EffectLightCore::Source CommonSource(const SlotLight &l, const float r[RECORD_FLOATS], uint32_t recordHash, double stretch)
	{
		using namespace EffectLightCore;
		Source s;
		s.A0 = { (double)r[R_POS], (double)r[R_POS + 2], (double)r[R_POS + 1] };
		s.B0 = s.A0;
		s.Vel = { (double)r[R_VEL], (double)r[R_VEL + 2], (double)r[R_VEL + 1] };
		for (int c = 0; c < 3; c++)
			s.Color[c] = (float)(Bounded(l.Color[c], 1.0) * Bounded(r[R_TINT + c], 1.0));	// the record's tint, as the particle's colour
		s.Radius = l.Radius;
		s.Intensity = Bounded(l.Intensity * Bounded(r[R_INTENSITY], kMaxIntensity), kMaxIntensity);	// its intensity scale too
		s.Birth = r[R_BIRTH];
		s.Life = r[R_LIFE];
		s.Fade = 0.0;	// the curve is its brightness over life
		s.Gravity = l.Gravity;
		s.Drag = l.Drag;
		s.Hold = l.Hold;
		s.HoldFade = kHoldFade;
		s.CurveKeys = l.CurveKeys;
		for (int i = 0; i < EFFECT_LIGHT_CURVE_KEYS; i++)
		{
			s.CurveTime[i] = l.CurveTime[i];
			s.CurveValue[i] = l.CurveValue[i];
		}
		s.CurveEased = l.CurveEased;
		s.Tier = l.Tier;
		s.Flags = l.Tier >= TIER_IMPORTANT ? EFL_IMPORTANT : 0;
		s.Seed = HoldSeed(recordHash);
		// A streak that lights as a line: from its tail to its head along its launch velocity, centred on it, as long as it is
		// drawn at launch (gpuparticles.vp: stretch x r_gpuparticles_stretch seconds of travel each way), even along it.
		const double seconds = l.LineStretch * stretch;
		if (seconds > 0.0)
		{
			const Vec3 half = Scale(s.Vel, seconds);
			const Vec3 centre = s.A0;
			s.A0 = Add(centre, half);
			s.B0 = Sub(centre, half);
		}
		return s;
	}

	// THE RING. A record's light. Where its definition collides, it lands where its closed-form flight first comes down through its
	// drawn radius (gpuparticles.vp's halfSize at the first size key) onto its floor or a floor-like plane of its own, and holds
	// there `Hold` (a hashed share of it), kRiseShare of its radius up.
	inline EffectLightCore::Source RingSource(const SlotLight &l, const float r[RECORD_FLOATS], uint32_t recordHash, const Tuning &tuning)
	{
		using namespace EffectLightCore;
		Source s = CommonSource(l, r, recordHash, tuning.Stretch);
		if (l.Collide < 1)
			return s;

		double cap = tuning.MaxSize;
		if (l.MaxSize > 0.0)
			cap = std::min(l.MaxSize, cap);
		const double reach = 0.5 * std::min(l.SizeKey * (double)r[R_SIZESCALE] * tuning.SizeScale, cap);
		const double k = std::max(l.Drag, 1e-4);
		const double start[3] = { r[R_POS], r[R_POS + 1], r[R_POS + 2] };
		const double velocity[3] = { r[R_VEL], r[R_VEL + 1], r[R_VEL + 2] };
		const double life = r[R_LIFE];

		double best = -1.0;
		double normal[3] = { 0.0, 1.0, 0.0 };
		double offset = 0.0;
		if (r[R_FLOOR] > -32767.f)
		{
			const double up[3] = { 0.0, 1.0, 0.0 };
			const double t = LandingTime(start, velocity, l.Gravity, k, up, r[R_FLOOR], reach, life);
			if (t >= 0.0)
			{
				best = t;
				offset = r[R_FLOOR];
			}
		}
		if (r[R_PLANE] < 1.5f)
		{
			double n[3];
			OctahedralDecode(r[R_PLANE], r[R_PLANE + 1], n);
			if (n[1] > kFloorLike)
			{
				const double t = LandingTime(start, velocity, l.Gravity, k, n, r[R_PLANEOFFSET], reach, life);
				if (t >= 0.0 && (best < 0.0 || t < best))
				{
					best = t;
					normal[0] = n[0]; normal[1] = n[1]; normal[2] = n[2];
					offset = r[R_PLANEOFFSET];
				}
			}
		}
		if (!(best >= 0.0 && best < life))
			return s;

		s.Land = std::max(best, kMinLand);
		// The centre where it lands (shader axes), moved along the surface's normal to RiseFor above it.
		const Vec3 centre = { (double)r[R_POS], (double)r[R_POS + 2], (double)r[R_POS + 1] };
		const Vec3 at = PathAt(centre, s.Vel, l.Gravity, l.Drag, s.Land);
		const double p[3] = { at.X, at.Z, at.Y };
		const double lift = RiseFor(l.Radius) - (p[0] * normal[0] + p[1] * normal[1] + p[2] * normal[2] - offset);
		const Vec3 land = { p[0] + normal[0] * lift, p[2] + normal[2] * lift, p[1] + normal[1] * lift };
		s.LandPoint = Add(land, Sub(s.A0, centre));	// a line keeps its shape about it
		s.HasLandPoint = true;
		return s;
	}

	// THE DEBRIS POOL. A piece's light: drawn one step behind its flight, like the piece (the pool draws it between its last two
	// steps). `landingStep` > 0: #11 predicts its first landing in that step at `landingPoint` (shader axes, on the surface) --
	// it lands and holds there, RiseFor its radius up; a light whose particle's life ends before then keeps its curve's last
	// value to the landing.
	inline EffectLightCore::Source DebrisSource(const SlotLight &l, const float r[RECORD_FLOATS], uint32_t recordHash, double stretch,
		int landingStep, const double landingPoint[3])
	{
		using namespace EffectLightCore;
		Source s = CommonSource(l, r, recordHash, stretch);
		const Vec3 centre = { (double)r[R_POS], (double)r[R_POS + 2], (double)r[R_POS + 1] };
		s.Birth = (double)r[R_BIRTH] + kDebrisStepSeconds;
		if (landingStep < 1 || landingPoint == nullptr)
			return s;

		const double land = landingStep * kDebrisStepSeconds;
		if (!(land < s.Life))
		{
			// Its life ends first: the light lives to the landing, its curve still over the particle's life.
			const double life = s.Life;
			s.Life = land + kDebrisStepSeconds;
			for (int i = 0; i < EFFECT_LIGHT_CURVE_KEYS; i++)
				s.CurveTime[i] = (float)(s.CurveTime[i] * (life / s.Life));
		}
		s.Land = land;
		const Vec3 point = { landingPoint[0], landingPoint[2], landingPoint[1] + RiseFor(l.Radius) };
		s.LandPoint = Add(point, Sub(s.A0, centre));
		s.HasLandPoint = true;
		return s;
	}
}
