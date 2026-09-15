/*
** hw_emissivevolumecore.h
**
** [EMISSIVEVOLUMES] The maths of emissive volumes, with no engine dependency.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Everything an emissive volume does that is not plumbing ("Engine docs/VOLUMETRIC_FLASH_15_PLAN.md" 2b, 2c, 2i, 2j with
** its OWNER ANSWERS; "Engine docs/EMISSIVE_VOLUMES_15_IMPL_NOTES.md"):
**
**   - what a definition says (Definition: the VOLUMEDEFS keys, gamedata/volumedefs.cpp reads them into this);
**   - how the owner's three menu settings resolve against a definition: which classes draw (ClassEnabled), how long a flash
**     lasts (ResolveLife), how it moves with the hand (ResolveMotion);
**   - one volume at a level time: its life, growth, shape lengths, flight, hand follow, bound, heat, intensity, flicker and
**     emission (Evaluate) -- closed form, so it moves at frame rate;
**   - its effect light (LightFor), its rank (Rank), its list texels (WriteTexels);
**   - the noise volume's bytes (BakeNoise).
**
** hw_emissivevolumes.cpp drives it from the level and the renderer. The shape FIELD itself (density at a point) exists only
** in shaders/pp/emissivevolume.fp; this header gives it its bound and its numbers. It is deterministic -- local hashes, no
** RNG, no clock of its own -- so the same inputs give the same volume on every machine at any frame rate. Presentation either
** way: nothing here is read back.
**
** AXES. Spawn, State, Pose and Light are GAME axes (x, y horizontal, z up). WriteTexels writes GL axes (x, z, y), as the list
** image holds them (hw_emissivevolumeframe.h).
**
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "hw_emissivevolumeframe.h"

namespace EmissiveVolumeCore
{

struct Vec3
{
	double X = 0.0, Y = 0.0, Z = 0.0;
};

inline Vec3 Add(const Vec3 &a, const Vec3 &b) { return { a.X + b.X, a.Y + b.Y, a.Z + b.Z }; }
inline Vec3 Sub(const Vec3 &a, const Vec3 &b) { return { a.X - b.X, a.Y - b.Y, a.Z - b.Z }; }
inline Vec3 Scale(const Vec3 &a, double s) { return { a.X * s, a.Y * s, a.Z * s }; }
inline double Dot(const Vec3 &a, const Vec3 &b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
inline Vec3 Cross(const Vec3 &a, const Vec3 &b) { return { a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X }; }
inline double Length(const Vec3 &a) { return std::sqrt(Dot(a, a)); }
inline bool Finite(const Vec3 &a) { return std::isfinite(a.X) && std::isfinite(a.Y) && std::isfinite(a.Z); }

// The unit vector along v, or `fallback` for a vector too short (or not finite) to have a direction.
inline Vec3 Normalize(const Vec3 &v, const Vec3 &fallback)
{
	const double length = Length(v);
	if (!(length > 1e-9) || !std::isfinite(length))
		return fallback;
	return Scale(v, 1.0 / length);
}

inline double Clamp(double v, double lo, double hi)
{
	if (!(v >= lo)) return lo;	// written this way so a NaN lands on lo
	return v > hi ? hi : v;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The settings (owner answers 1, 3 and 6: every look an option, the recommended one the default)
// ---------------------------------------------------------------------------------------------------------------------------

// A definition's class (VOLUMEDEFS `class`).
enum
{
	CLASS_GUN = 0,		// pistols, chainguns, rifles: every shot right in front of your eyes
	CLASS_BIGGUN = 1,	// shotguns, the Super Shotgun, rockets, plasma, the BFG at the muzzle
	CLASS_BLAST = 2,	// explosions and blasts
	CLASS_COUNT = 3,
};

// r_emissivevolumes, "Volumetric flashes": 3 All (default), 2 Big guns and blasts, 1 Explosions only, 0 Off.
enum
{
	SETTING_OFF = 0,
	SETTING_BLASTS = 1,
	SETTING_BIGGUNS = 2,
	SETTING_ALL = 3,
};

// Whether the "Volumetric flashes" setting draws a class. Monotonic: each step down drops the class of the smallest, most
// frequent flashes first.
inline bool ClassEnabled(int cls, int setting)
{
	static const int need[CLASS_COUNT] = { SETTING_ALL, SETTING_BIGGUNS, SETTING_BLASTS };
	const int c = cls < 0 ? 0 : (cls >= CLASS_COUNT ? CLASS_COUNT - 1 : cls);
	const int s = setting < SETTING_OFF ? SETTING_OFF : (setting > SETTING_ALL ? SETTING_ALL : setting);
	return s >= need[c];
}

// r_emissivevolumes_length, "Flash length": 0 By gun (default), 1 Snap, 2 Linger.
enum
{
	LENGTH_BYGUN = 0,	// each definition's own `life` (the recommended by-gun sizes)
	LENGTH_SNAP = 1,	// every flash under 1/30 s
	LENGTH_LINGER = 2,	// every flash about 1/4 s or longer
};

inline constexpr double SNAP_LIFE = 0.03;
inline constexpr double LINGER_LIFE = 0.25;
inline constexpr double LIFE_MIN = 0.005;
inline constexpr double LIFE_MAX = 10.0;

// How long one spawn lives. "Flash length" is about muzzle flashes: a blast keeps its definition's life whatever it says.
// lifeScale (the spawn's variety) still applies in every mode.
inline double ResolveLife(double definitionLife, double lifeScale, int cls, int lengthMode)
{
	const double base = Clamp(definitionLife, LIFE_MIN, LIFE_MAX);
	const double scale = Clamp(lifeScale, 0.05, 16.0);
	double life = base * scale;
	if (cls != CLASS_BLAST)
	{
		if (lengthMode == LENGTH_SNAP)
			life = std::min(life, SNAP_LIFE);
		else if (lengthMode == LENGTH_LINGER)
			life = std::max(base, LINGER_LIFE) * scale;
	}
	return Clamp(life, LIFE_MIN, LIFE_MAX);
}

// A definition's hand motion (VOLUMEDEFS `motion`), and r_emissivevolumes_motion, "Flash motion".
enum
{
	MOTION_TRAIL = 0,	// the root stays on the muzzle while the outer flame trails in the air (default)
	MOTION_STAY = 1,	// the whole volume stays where it was fired
	MOTION_RIDE = 2,	// the whole volume rides the gun like a solid part
};

enum
{
	MOTIONMENU_PERDEFINITION = 0,	// each definition's own `motion` (default)
	MOTIONMENU_STAY = 1,
	MOTIONMENU_RIDE = 2,
};

inline int ResolveMotion(int definitionMotion, int motionMenu)
{
	if (motionMenu == MOTIONMENU_STAY)
		return MOTION_STAY;
	if (motionMenu == MOTIONMENU_RIDE)
		return MOTION_RIDE;
	return definitionMotion < MOTION_TRAIL || definitionMotion > MOTION_RIDE ? MOTION_TRAIL : definitionMotion;
}

// SpawnEmissiveVolume's `follow` (doombase.zs EEmissiveVolumeFollow) -- ResolveTrackedPose's anchors (r_utility.h).
enum
{
	FOLLOW_WORLD = 0,
	FOLLOW_MAINHAND = 1,
	FOLLOW_OFFHAND = 2,
	FOLLOW_HEAD = 3,
};

// VOLUMEDEFS `lightflags`: what the volume's effect light lights.
enum
{
	LIGHT_SURFACES = 1,
	LIGHT_SMOKE = 2,
	LIGHT_PARTICLES = 4,
	LIGHT_ALL = 7,
};

// ---------------------------------------------------------------------------------------------------------------------------
// Hashes: FLevelLocals::GpuParticleHash's mixer, as EffectLightCore::Hash (its body token for token), so a volume's variety
// comes from the same kind of local hash as a particle's and a light's. Never an RNG.
// ---------------------------------------------------------------------------------------------------------------------------

inline uint32_t Hash(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU;
	x ^= x >> 15; x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

// 0 <= share < 1: 24 bits of an independent stream of `seed`.
inline double Share(uint32_t seed, uint32_t stream)
{
	return (double)(Hash(seed ^ Hash(stream * 0x85EBCA77U + 0x68bc21ebU)) >> 8) / 16777216.0;
}

// A quarter-unit grid position's bits: nearby spawns in the same tic still differ.
inline uint32_t PositionBits(double v)
{
	if (!std::isfinite(v))
		return 0;
	return (uint32_t)(int64_t)std::floor(Clamp(v, -1.0e8, 1.0e8) * 4.0);
}

// A queued volume's seed when the caller gives none: the tic it was spawned in, its place in that tic's queue and where it
// began (EffectLightCore::SeedFor's rule, its own salt). The same on every machine, whatever the frame rate.
inline uint32_t SeedFor(int tic, int index, const Vec3 &pos)
{
	uint32_t h = Hash((uint32_t)tic * 0x9E3779B1U + 0x3c6ef372U);
	h = Hash(h ^ ((uint32_t)index * 0x85EBCA77U));
	h = Hash(h ^ PositionBits(pos.X));
	h = Hash(h ^ (PositionBits(pos.Y) * 0xC2B2AE3DU));
	h = Hash(h ^ (PositionBits(pos.Z) * 0x27D4EB2FU));
	return h != 0 ? h : 1;
}

// A name's hash: FNV-1a over ASCII lower case, so it never depends on a locale or on how a machine spelled the name first.
inline uint32_t NameHash(const char *name)
{
	uint32_t hash = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)name; p != nullptr && *p != 0; p++)
	{
		unsigned char ch = *p;
		if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
		hash ^= ch;
		hash *= 16777619u;
	}
	return hash;
}

// The script handle of a definition name (LevelLocals.EmissiveVolumeDefinition): 31 bits of NameHash, never 0.
inline int HandleFor(const char *name)
{
	const int handle = (int)(NameHash(name) & 0x7fffffffu);
	return handle != 0 ? handle : 1;
}

// A hash smoothed between whole steps of t: the flicker's shape. Continuous in t, between 0 and 1.
inline double SmoothHash(uint32_t seed, double t)
{
	if (!std::isfinite(t) || t < 0.0)
		t = 0.0;
	const double step = std::floor(t);
	const double f = t - step;
	const uint32_t i = (uint32_t)(int64_t)step;
	const double a = Share(seed ^ 0x27d4eb2fU, i);
	const double b = Share(seed ^ 0x27d4eb2fU, i + 1u);
	const double s = f * f * (3.0 - 2.0 * f);
	return a + (b - a) * s;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The definition (VOLUMEDEFS `emissive <name> { ... }`; every key optional, gamedata/volumedefs.cpp). Defaults make a plain
// soft ball that never lights.
// ---------------------------------------------------------------------------------------------------------------------------

inline constexpr int INTENSITY_KEYS = 8;	// a defblocks ramp's most keys
inline constexpr int PETALS_MAX = 16;

struct Definition
{
	int Class = CLASS_GUN;						// class = gun | biggun | blast
	double Length = 0.0;						// shape = length, base radius, tip radius[, weight]
	double BaseRadius = 8.0;
	double TipRadius = 8.0;
	double BodyWeight = 1.0;
	double Offset = 0.0;						// offset = the base point along the axis
	int PetalCount = 0;							// petals = count, length, width, spread (degrees)[, weight]
	double PetalLength = 0.0;
	double PetalWidth = 0.0;
	double PetalSpread = 0.0;
	double PetalWeight = 1.0;
	double PetalJitter = 0.35;					// petaljitter = 0..1: how much each petal's length (and half as much its width) varies
	double RingRadius = 0.0;					// ring = radius, thickness, weight
	double RingThickness = 0.0;
	double RingWeight = 0.0;
	double Softness = 0.5;						// softness = 0..1: the edge's falloff, a share of each part's thickness
	double NoiseScale = 0.08;					// noise = cells per map unit, amount 0..1, octaves 1..2
	double NoiseAmount = 0.5;
	int NoiseOctaves = 2;
	double Billow = 40.0;						// billow = map units a second the noise flows outward
	double Churn = 1.0;							// churn = noise cells a second the noise changes in place
	double GrowStart = 0.4;						// grow = start scale, share of life
	double GrowShare = 0.3;
	double Expand = 0.0;						// expand = map units a second at the bound after growth
	double Life = 0.06;							// life = seconds
	double HeatStart = 1.0;						// heat = core heat at birth, at death (0..1 on the heat ramp)
	double HeatEnd = 0.3;
	double HeatFalloff = 0.5;					// heatfalloff = 0..4: temperature = core heat x density ^ this
	float Tint[3] = { 1.f, 1.f, 1.f };			// tint = r g b (0..255)
	int IntensityKeys = 2;						// intensity = v @t, ... (up to 8 keys over life)
	float IntensityTime[INTENSITY_KEYS] = { 0.f, 1.f };
	float IntensityValue[INTENSITY_KEYS] = { 4.f, 0.f };
	double Absorption = 0.0;					// absorption = 0..4
	double Flicker = 0.25;						// flicker = amount 0..1, rate (Hz)
	double FlickerRate = 30.0;
	double Drag = 4.0;							// drag = per second
	int Motion = MOTION_TRAIL;					// motion = trail | stay | ride
	double FollowTip = 0.3;						// followtip = 0..1
	double LightRadius = 0.0;					// light = radius, intensity (0 = none)
	double LightIntensity = 0.0;
	int LightLine = 0;							// lightline = 0 point | 1 along the axis
	int LightFlags = LIGHT_ALL;					// lightflags = surfaces, smoke, particles
	int Signature = 0;							// signature = 0 every shot varies | 1 one fixed shape
	double Wobble = 0.12;						// wobble = 0..0.5: how much each shot's size varies
	uint32_t NameHash = 0;						// the definition's name hash: the fixed shape's seed with `signature`
};

// The intensity ramp at share x of the life: a straight line between keys, flat before the first and after the last. Never
// below 0.
inline double IntensityAt(const Definition &d, double x)
{
	const int keys = d.IntensityKeys < 0 ? 0 : (d.IntensityKeys > INTENSITY_KEYS ? INTENSITY_KEYS : d.IntensityKeys);
	if (keys == 0)
		return 0.0;
	if (keys == 1 || !(x > d.IntensityTime[0]))
		return std::max((double)d.IntensityValue[0], 0.0);
	for (int i = 1; i < keys; i++)
	{
		if (x < d.IntensityTime[i])
		{
			const double t0 = d.IntensityTime[i - 1], t1 = d.IntensityTime[i];
			const double f = t1 > t0 ? Clamp((x - t0) / (t1 - t0), 0.0, 1.0) : 1.0;
			return std::max((double)d.IntensityValue[i - 1] + ((double)d.IntensityValue[i] - (double)d.IntensityValue[i - 1]) * f, 0.0);
		}
	}
	return std::max((double)d.IntensityValue[keys - 1], 0.0);
}

// The engine's fixed heat ramp -- gpuparticles.fp's kHeatRamp and ParticleHeatColor, the same five keys.
inline void HeatColor(double heat, float out[3])
{
	static const float ramp[5][3] = {
		{ 0.0f, 0.0f, 0.0f },
		{ 0.42f, 0.045f, 0.0f },
		{ 1.0f, 0.32f, 0.03f },
		{ 1.0f, 0.68f, 0.22f },
		{ 1.0f, 0.96f, 0.86f } };
	const float position = (float)Clamp(heat, 0.0, 1.0) * 4.0f;
	int k = (int)position;
	if (k > 3) k = 3;
	const float f = position - (float)k;
	for (int c = 0; c < 3; c++)
		out[c] = ramp[k][c] + (ramp[k + 1][c] - ramp[k][c]) * f;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The shape's extent (2b "Bounds"): what the field can reach, from its scaled lengths
// ---------------------------------------------------------------------------------------------------------------------------

struct Lengths
{
	double Length = 0.0, BaseRadius = 0.0, TipRadius = 0.0;
	double PetalLength = 0.0, PetalWidth = 0.0, CosSpread = 1.0, SinSpread = 0.0;
	double RingRadius = 0.0, RingThickness = 0.0;
};

inline Lengths LengthsAt(const Definition &d, double scale)
{
	Lengths l;
	const double s = std::max(scale, 0.0);
	l.Length = std::max(d.Length, 0.0) * s;
	l.BaseRadius = std::max(d.BaseRadius, 0.0) * s;
	l.TipRadius = std::max(d.TipRadius, 0.0) * s;
	l.PetalLength = std::max(d.PetalLength, 0.0) * s;
	l.PetalWidth = std::max(d.PetalWidth, 0.0) * s;
	const double spread = Clamp(d.PetalSpread, 0.0, 180.0) * (3.14159265358979323846 / 180.0);
	l.CosSpread = std::cos(spread);
	l.SinSpread = std::sin(spread);
	l.RingRadius = std::max(d.RingRadius, 0.0) * s;
	l.RingThickness = std::max(d.RingThickness, 0.0) * s;
	return l;
}

struct Extent
{
	double AxialMin = 0.0, AxialMax = 0.0;	// along the axis from the base
	double Radial = 0.0;					// the farthest from the axis
	bool Any = false;						// some part can have density
	double CentreAlong = 0.0;				// the bounding sphere: its centre along the axis, its radius
	double Radius = 0.0;
};

// Every point where the field can be above 0 (emissivevolume.fp, VolumeDensity: a part with weight 0 adds nothing, and a
// part is 0 outside its own surface) lies in the returned cylinder around the axis, and so in its sphere. Petals at their
// longest and widest jitter; round ends included.
inline Extent ExtentOf(const Definition &d, const Lengths &l)
{
	Extent e;
	double amin = 0.0, amax = 0.0, radial = 0.0;
	const auto take = [&](double lo, double hi, double r)
	{
		if (!e.Any) { amin = lo; amax = hi; radial = r; e.Any = true; return; }
		amin = std::min(amin, lo);
		amax = std::max(amax, hi);
		radial = std::max(radial, r);
	};
	if (d.BodyWeight > 0.0 && (l.BaseRadius > 0.0 || l.TipRadius > 0.0))
		take(std::min(-l.BaseRadius, l.Length - l.TipRadius), std::max(l.BaseRadius, l.Length + l.TipRadius), std::max(l.BaseRadius, l.TipRadius));
	const int petals = d.PetalCount < 0 ? 0 : (d.PetalCount > PETALS_MAX ? PETALS_MAX : d.PetalCount);
	if (petals > 0 && d.PetalWeight > 0.0 && l.PetalWidth > 0.0)
	{
		const double jitter = Clamp(d.PetalJitter, 0.0, 1.0);
		const double length = l.PetalLength * (1.0 + jitter);
		const double width = l.PetalWidth * (1.0 + 0.5 * jitter);
		const double r0 = width * 0.5, r1 = width * 0.1;
		const double tipAxial = length * l.CosSpread, tipRadial = length * std::fabs(l.SinSpread);
		take(std::min(-r0, tipAxial - r1), std::max(r0, tipAxial + r1), std::max(r0, tipRadial + r1));
	}
	if (d.RingWeight > 0.0 && l.RingThickness > 0.0)
		take(-l.RingThickness, l.RingThickness, l.RingRadius + l.RingThickness);
	if (!e.Any)
		return e;
	e.AxialMin = amin;
	e.AxialMax = amax;
	e.Radial = radial;
	e.CentreAlong = 0.5 * (amin + amax);
	const double halfAxial = 0.5 * (amax - amin);
	// A hundredth and half a unit to spare, for float32 in the shader.
	e.Radius = std::sqrt(halfAxial * halfAxial + radial * radial) * 1.01 + 0.5;
	return e;
}

// ---------------------------------------------------------------------------------------------------------------------------
// One volume
// ---------------------------------------------------------------------------------------------------------------------------

// A pose as ResolveTrackedPose writes it (r_utility.h): Doom yaw and pitch in RADIANS, pitch positive down.
struct Pose
{
	bool Valid = false;
	Vec3 Pos;
	double Yaw = 0.0;
	double Pitch = 0.0;
};

// The pose's own frame, exactly as ResolveTrackedPose places an offset: forward, right, up. Orthonormal, so its transpose is
// its inverse.
struct Frame
{
	Vec3 Forward, Right, Up;
};

inline Frame PoseFrame(double yaw, double pitch)
{
	const double cp = std::cos(pitch), sp = std::sin(pitch);
	const double cy = std::cos(yaw), sy = std::sin(yaw);
	Frame f;
	f.Forward = { cp * cy, cp * sy, -sp };
	f.Right = { sy, -cy, 0.0 };
	f.Up = { sp * cy, sp * sy, cp };
	return f;
}

inline Vec3 ToLocal(const Frame &f, const Vec3 &v) { return { Dot(v, f.Forward), Dot(v, f.Right), Dot(v, f.Up) }; }
inline Vec3 FromLocal(const Frame &f, const Vec3 &l) { return Add(Add(Scale(f.Forward, l.X), Scale(f.Right, l.Y)), Scale(f.Up, l.Z)); }

// A unit vector across `axis` that does not depend on anything but the axis: the world axis least along it, crossed.
inline Vec3 AcrossOf(const Vec3 &axis)
{
	const double ax = std::fabs(axis.X), ay = std::fabs(axis.Y), az = std::fabs(axis.Z);
	const Vec3 pick = (ax <= ay && ax <= az) ? Vec3{ 1.0, 0.0, 0.0 } : (ay <= az ? Vec3{ 0.0, 1.0, 0.0 } : Vec3{ 0.0, 0.0, 1.0 });
	return Normalize(Cross(axis, pick), { 1.0, 0.0, 0.0 });
}

// What one spawn was given (the queue's FEmissiveVolumeEvent with its definition resolved, or a renderer-side spawn).
struct Spawn
{
	Definition Def;
	Vec3 Pos;						// where it was fired (the root), game axes
	Vec3 Dir{ 0.0, 0.0, 1.0 };		// which way the gas leaves (any length)
	Vec3 Vel;						// map units a second, with the definition's drag
	double Scale = 1.0;				// multiplies every length
	double Brightness = 1.0;		// multiplies the intensity
	double LifeScale = 1.0;			// multiplies the life
	double FollowShare = 1.0;		// 0..1: how much of the follow the root takes
	double LightScale = 1.0;		// multiplies the light's intensity
	float Tint[3] = { 1.f, 1.f, 1.f };	// multiplies the definition's tint
	uint32_t Seed = 1;
	double Birth = 0.0;				// level seconds: maptime / TICRATE of the tic that spawned it
	int Follow = FOLLOW_WORLD;
	int FollowPlayer = -1;
	bool FollowValid = false;		// the pose below was read when it was spawned
	Pose FollowAt;					// that pose
	uint64_t Sequence = 0;			// spawn order: ties go to the older volume
};

// The renderer-read settings a frame evaluates with.
struct Settings
{
	int LengthMode = LENGTH_BYGUN;
	int MotionMenu = MOTIONMENU_PERDEFINITION;
	double Brightness = 1.0;		// r_emissivevolumes_brightness, 0..4
};

// One volume at this frame's level time.
struct State
{
	bool Alive = false;
	double Age = 0.0, Life = 0.0, Share = 0.0;	// seconds, seconds, age / life
	int Motion = MOTION_TRAIL;
	uint32_t ShapeSeed = 0;
	double ShapeScale = 0.0;		// the spawn's scale x its wobble x its growth now
	Lengths L;
	Vec3 Base, Axis, Across;		// the root with its offset; unit; unit and across the axis
	Vec3 Follow;					// the root's follow displacement (trail motion; 0 otherwise)
	double FollowTip = 1.0;			// the share of it the tip keeps
	double LagExtent = 1.0;			// the axial reach the lag runs over
	Extent Bound;					// with Radius grown by the follow's length
	double HeatNow = 0.0;
	double Intensity = 0.0;
	double Flicker = 1.0;			// the brightness factor now
	float Emission[3] = { 0.f, 0.f, 0.f };	// tint x intensity x brightness x flicker x the settings' brightness
	double Extinction = 0.0;		// per map unit at density 1
	double NoiseCells = 0.0;		// noise cells per map unit at this shape scale
	double NoiseOffset[3] = { 0.0, 0.0, 0.0 };
	double BillowDistance = 0.0;
	double ChurnPhase = 0.0;
};

// Growth from the start scale to 1 over the growth share of the life (eased out), then the expansion.
inline double GrowthAt(const Definition &d, double age, double life, double unscaledBound)
{
	const double share = Clamp(d.GrowShare, 0.0, 1.0);
	const double start = Clamp(d.GrowStart, 0.0, 4.0);
	const double growTime = share * life;
	double g = 1.0;
	if (growTime > 0.0)
	{
		const double u = Clamp(age / growTime, 0.0, 1.0);
		const double eased = 1.0 - (1.0 - u) * (1.0 - u);
		g = start + (1.0 - start) * eased;
	}
	return g + std::max(d.Expand, 0.0) * std::max(age - growTime, 0.0) / std::max(unscaledBound, 1.0);
}

// Where and how bright the volume is at level time `now`. `poseNow` is the followed pose this frame (Valid false: none).
//
// FOLLOW (2j, owner answer 6). The spawn recorded the followed pose when it was fired; the root's offset in THAT pose's frame
// is put back in the pose's frame NOW, and the difference is the follow, times followShare.
//   trail  the root takes all of it and the tip `followtip` of it (the shader lags every sample by its axial share of
//          LagExtent); the axis keeps its spawn direction: gas leaves along the bore it was fired from.
//   ride   the whole volume moves with the pose: the root by the follow, the axis and the reference across it turned by the
//          pose's turn since the spawn.
//   stay   no follow.
// The lag never folds the field over itself: when the follow along the axis times (1 - followtip) passes half the axial
// reach, the tip keeps more of it (the root stays exact).
inline State Evaluate(const Spawn &s, double now, const Pose &poseNow, const Settings &settings)
{
	State e;
	const Definition &d = s.Def;
	e.Life = ResolveLife(d.Life, s.LifeScale, d.Class, settings.LengthMode);
	double age = now - s.Birth;
	if (!(age >= 0.0))
		age = 0.0;
	e.Age = age;
	e.Alive = age < e.Life;
	e.Share = Clamp(age / e.Life, 0.0, 1.0);
	e.Motion = ResolveMotion(d.Motion, settings.MotionMenu);
	e.ShapeSeed = d.Signature ? (d.NameHash != 0 ? d.NameHash : 1u) : s.Seed;

	// Size: the spawn's scale, this shot's wobble, and the growth now.
	const double wobble = 1.0 + Clamp(d.Wobble, 0.0, 0.5) * (2.0 * Share(e.ShapeSeed, 1) - 1.0);
	const Extent unscaled = ExtentOf(d, LengthsAt(d, 1.0));
	e.ShapeScale = Clamp(s.Scale, 0.0, 16.0) * wobble * GrowthAt(d, age, e.Life, unscaled.Radius);
	e.L = LengthsAt(d, e.ShapeScale);

	// The axis, and the reference across it turned by this shot's hash (petals turn every shot).
	Vec3 axis = Normalize(s.Dir, { 0.0, 0.0, 1.0 });
	const double turn = 2.0 * 3.14159265358979323846 * Share(e.ShapeSeed, 2);
	const Vec3 ref = AcrossOf(axis);
	Vec3 across = Add(Scale(ref, std::cos(turn)), Scale(Cross(axis, ref), std::sin(turn)));

	// Flight: the particle maths with linear drag, the root only (the shape moves with it).
	const double k = std::max(d.Drag, 1e-4);
	const double flight = (1.0 - std::exp(-k * age)) / k;
	Vec3 root = Add(s.Pos, Scale(s.Vel, flight));

	Vec3 follow;
	double tip = 1.0;
	if (e.Motion != MOTION_STAY && s.Follow != FOLLOW_WORLD && s.FollowValid && poseNow.Valid)
	{
		const Frame then = PoseFrame(s.FollowAt.Yaw, s.FollowAt.Pitch);
		const Frame nowFrame = PoseFrame(poseNow.Yaw, poseNow.Pitch);
		const Vec3 local = ToLocal(then, Sub(s.Pos, s.FollowAt.Pos));
		const Vec3 rootNow = Add(poseNow.Pos, FromLocal(nowFrame, local));
		const Vec3 moved = Scale(Sub(rootNow, s.Pos), Clamp(s.FollowShare, 0.0, 1.0));
		if (Finite(moved))
		{
			if (e.Motion == MOTION_RIDE)
			{
				axis = Normalize(FromLocal(nowFrame, ToLocal(then, axis)), axis);
				across = FromLocal(nowFrame, ToLocal(then, across));
				root = Add(root, moved);
			}
			else
			{
				follow = moved;
				tip = Clamp(d.FollowTip, 0.0, 1.0);
			}
		}
	}
	// Across exactly across the (possibly turned) axis.
	across = Normalize(Sub(across, Scale(axis, Dot(across, axis))), AcrossOf(axis));

	e.Axis = axis;
	e.Across = across;
	e.Base = Add(root, Scale(axis, Clamp(d.Offset, -4096.0, 4096.0) * e.ShapeScale));

	e.Bound = ExtentOf(d, e.L);
	e.LagExtent = std::max(e.Bound.AxialMax, 1.0);
	const double along = Dot(follow, axis);
	if (std::fabs(along) * (1.0 - tip) > 0.5 * e.LagExtent)
		tip = 1.0 - 0.5 * e.LagExtent / std::fabs(along);
	e.Follow = follow;
	e.FollowTip = Clamp(tip, 0.0, 1.0);
	if (e.Bound.Any)
		e.Bound.Radius += Length(follow);

	e.HeatNow = Clamp(d.HeatStart + (d.HeatEnd - d.HeatStart) * e.Share, 0.0, 1.0);
	e.Intensity = IntensityAt(d, e.Share);
	e.Flicker = 1.0 - Clamp(d.Flicker, 0.0, 1.0) * SmoothHash(s.Seed, age * Clamp(d.FlickerRate, 0.0, 240.0));
	const double gain = e.Alive ? e.Intensity * Clamp(s.Brightness, 0.0, 16.0) * e.Flicker * Clamp(settings.Brightness, 0.0, 4.0) : 0.0;
	for (int c = 0; c < 3; c++)
		e.Emission[c] = (float)Clamp((double)d.Tint[c] * (double)s.Tint[c] * gain, 0.0, 4096.0);
	e.Extinction = Clamp(d.Absorption, 0.0, 4.0) * EMISSIVE_ABSORPTION_PER_MAP_UNIT;

	e.NoiseCells = e.ShapeScale > 0.0 ? Clamp(d.NoiseScale, 0.0, 16.0) / e.ShapeScale : 0.0;
	for (int i = 0; i < 3; i++)
		e.NoiseOffset[i] = Share(e.ShapeSeed, 3 + (uint32_t)i);
	e.BillowDistance = Clamp(d.Billow, 0.0, 4096.0) * age;
	e.ChurnPhase = Clamp(d.Churn, 0.0, 64.0) * age;
	return e;
}

// The root's follow a point at axial share `along` (map units from the base) keeps: all of it at the base, `FollowTip` of it
// at LagExtent and past it (emissivevolume.fp's FollowWeight).
inline double FollowWeight(const State &e, double along)
{
	return 1.0 - (1.0 - e.FollowTip) * Clamp(along / e.LagExtent, 0.0, 1.0);
}

// Whether the volume draws anything: alive, some part with density, some light.
inline bool Visible(const State &e)
{
	return e.Alive && e.Bound.Any && e.Bound.Radius > 0.0 && (e.Emission[0] > 0.f || e.Emission[1] > 0.f || e.Emission[2] > 0.f);
}

// Its bounding sphere's centre, game axes.
inline Vec3 BoundCentre(const State &e)
{
	return Add(e.Base, Scale(e.Axis, e.Bound.CentreAlong));
}

// Rank (2i): screen importance -- bound / (1 + distance / 256) -- times its brightness now. Higher first; ties to the older.
inline double Rank(const State &e, double distanceToEye)
{
	const double luminance = 0.2126 * e.Emission[0] + 0.7152 * e.Emission[1] + 0.0722 * e.Emission[2];
	return std::max(e.Bound.Radius, 0.0) / (1.0 + std::max(distanceToEye, 0.0) / 256.0) * std::max(luminance, 0.0);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Its effect light (2f; owner answer 2: each flash lights the haze in its own shape and colour, flickering with it)
// ---------------------------------------------------------------------------------------------------------------------------

struct Light
{
	Vec3 A, B;					// game axes; A == B for a point
	float Color[3] = { 0.f, 0.f, 0.f };	// 0..64 a channel
	double Radius = 0.0;
	int Flags = LIGHT_ALL;		// LIGHT_
};

// The light a volume throws this frame: its colour is the heat ramp at the core heat now x the emission the list uploads
// (tint, intensity, brightness, the flicker) x the definition's light intensity x the spawn's light scale -- so the glow in
// the haze flickers in step with the gas. A point at the emission centre (the bound's centre, with the follow it keeps
// there), or with `lightline` a line along the axis from the root to the axial reach. False: no light.
inline bool LightFor(const State &e, const Spawn &s, Light &out)
{
	const Definition &d = s.Def;
	if (!Visible(e) || !(d.LightRadius >= 1.0) || !(d.LightIntensity > 0.0))
		return false;
	float heat[3];
	HeatColor(e.HeatNow, heat);
	const double gain = Clamp(d.LightIntensity, 0.0, 16.0) * Clamp(s.LightScale, 0.0, 16.0);
	double luminance = 0.0;
	for (int c = 0; c < 3; c++)
	{
		out.Color[c] = (float)Clamp((double)heat[c] * (double)e.Emission[c] * gain, 0.0, 64.0);
		luminance += out.Color[c];
	}
	if (!(luminance > 0.0))
		return false;
	out.Radius = Clamp(d.LightRadius * Clamp(s.Scale, 0.0, 16.0), 1.0, 1024.0);
	out.Flags = d.LightFlags & LIGHT_ALL;
	if (d.LightLine)
	{
		out.A = Add(e.Base, e.Follow);
		out.B = Add(Add(e.Base, Scale(e.Axis, e.LagExtent)), Scale(e.Follow, e.FollowTip));
	}
	else
	{
		out.A = Add(BoundCentre(e), Scale(e.Follow, FollowWeight(e, e.Bound.CentreAlong)));
		out.B = out.A;
	}
	return Finite(out.A) && Finite(out.B);
}

// ---------------------------------------------------------------------------------------------------------------------------
// The list texels (hw_emissivevolumeframe.h, EEmissiveVolumeRow)
// ---------------------------------------------------------------------------------------------------------------------------

// One volume's column, GL axes, positions from `origin` (game axes, whole map units).
inline void WriteTexels(const State &e, const Definition &d, const double origin[3], float rows[EMISSIVE_VOLUME_TEXELS][4])
{
	const auto gl = [](const Vec3 &v, float out[4]) { out[0] = (float)v.X; out[1] = (float)v.Z; out[2] = (float)v.Y; };
	memset(rows, 0, sizeof(float) * EMISSIVE_VOLUME_TEXELS * 4);
	const Vec3 base = { e.Base.X - origin[0], e.Base.Y - origin[1], e.Base.Z - origin[2] };
	gl(base, rows[EVROW_BASE]);
	rows[EVROW_BASE][3] = (float)e.Bound.Radius;
	gl(e.Axis, rows[EVROW_AXIS]);
	rows[EVROW_AXIS][3] = (float)e.Bound.CentreAlong;
	gl(e.Across, rows[EVROW_FRAME]);
	rows[EVROW_FRAME][3] = (float)e.L.Length;
	gl(e.Follow, rows[EVROW_FOLLOW]);
	rows[EVROW_FOLLOW][3] = (float)e.FollowTip;
	rows[EVROW_BODY][0] = (float)e.L.BaseRadius;
	rows[EVROW_BODY][1] = (float)e.L.TipRadius;
	rows[EVROW_BODY][2] = (float)Clamp(d.Softness, 0.0, 1.0);
	rows[EVROW_BODY][3] = (float)Clamp(d.BodyWeight, 0.0, 4.0);
	rows[EVROW_PETALS][0] = (float)(d.PetalCount < 0 ? 0 : (d.PetalCount > PETALS_MAX ? PETALS_MAX : d.PetalCount));
	rows[EVROW_PETALS][1] = (float)e.L.PetalLength;
	rows[EVROW_PETALS][2] = (float)e.L.PetalWidth;
	rows[EVROW_PETALS][3] = (float)Clamp(d.PetalWeight, 0.0, 4.0);
	rows[EVROW_PETALS2][0] = (float)e.L.CosSpread;
	rows[EVROW_PETALS2][1] = (float)e.L.SinSpread;
	rows[EVROW_PETALS2][2] = (float)Clamp(d.PetalJitter, 0.0, 1.0);
	rows[EVROW_PETALS2][3] = (float)e.LagExtent;
	rows[EVROW_RING][0] = (float)e.L.RingRadius;
	rows[EVROW_RING][1] = (float)e.L.RingThickness;
	rows[EVROW_RING][2] = (float)Clamp(d.RingWeight, 0.0, 4.0);
	rows[EVROW_RING][3] = (float)Clamp(d.HeatFalloff, 0.0, 4.0);
	for (int i = 0; i < 3; i++)
		rows[EVROW_NOISE][i] = (float)e.NoiseOffset[i];
	rows[EVROW_NOISE][3] = (float)Clamp(d.NoiseAmount, 0.0, 1.0);
	rows[EVROW_NOISE2][0] = (float)e.NoiseCells;
	rows[EVROW_NOISE2][1] = (float)(d.NoiseOctaves >= 2 ? 2 : 1);
	rows[EVROW_NOISE2][2] = (float)e.BillowDistance;
	rows[EVROW_NOISE2][3] = (float)e.ChurnPhase;	// not wrapped: the churn's direction is not a whole tile, so a wrap would jump
	for (int c = 0; c < 3; c++)
		rows[EVROW_EMISSION][c] = e.Emission[c] / EMISSIVE_EMISSION_LENGTH;
	rows[EVROW_EMISSION][3] = (float)e.Extinction;
	rows[EVROW_HEAT][0] = (float)e.HeatNow;
	rows[EVROW_HEAT][1] = (float)(e.ShapeSeed & 0xFFFFFFu);
	rows[EVROW_HEAT][2] = (float)e.ShapeScale;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The noise volume (hw_emissivevolumeframe.h): EMISSIVE_NOISE_SIZE^3 texels of `channels` (2 or 4) bytes, x fastest. r and g
// are smooth value noise on a lattice EMISSIVE_NOISE_LATTICE texels apart that wraps at the volume's edge (tileable), each
// from its own stream of a fixed hash, eased with the quintic; with 4 channels b is 0 and a 255.
// ---------------------------------------------------------------------------------------------------------------------------

inline void BakeNoise(std::vector<uint8_t> &out, int channels)
{
	const int n = EMISSIVE_NOISE_SIZE, lattice = EMISSIVE_NOISE_LATTICE, cells = EMISSIVE_NOISE_SIZE / EMISSIVE_NOISE_LATTICE;
	const int stride = channels == 4 ? 4 : 2;
	out.assign((size_t)n * n * n * stride, 0);
	const auto ease = [](double u) { return u * u * u * (u * (u * 6.0 - 15.0) + 10.0); };
	for (int channel = 0; channel < 2; channel++)
	{
		const uint32_t seed = 0x51ed270bU + (uint32_t)channel * 0x9E3779B1U;
		const auto value = [&](int x, int y, int z)
		{
			x = (x % cells + cells) % cells; y = (y % cells + cells) % cells; z = (z % cells + cells) % cells;
			return Share(seed, (uint32_t)((z * cells + y) * cells + x));
		};
		for (int z = 0; z < n; z++)
		{
			const int cz = z / lattice;
			const double wz = ease((double)(z % lattice) / lattice);
			for (int y = 0; y < n; y++)
			{
				const int cy = y / lattice;
				const double wy = ease((double)(y % lattice) / lattice);
				for (int x = 0; x < n; x++)
				{
					const int cx = x / lattice;
					const double wx = ease((double)(x % lattice) / lattice);
					const double c00 = value(cx, cy, cz) + (value(cx + 1, cy, cz) - value(cx, cy, cz)) * wx;
					const double c10 = value(cx, cy + 1, cz) + (value(cx + 1, cy + 1, cz) - value(cx, cy + 1, cz)) * wx;
					const double c01 = value(cx, cy, cz + 1) + (value(cx + 1, cy, cz + 1) - value(cx, cy, cz + 1)) * wx;
					const double c11 = value(cx, cy + 1, cz + 1) + (value(cx + 1, cy + 1, cz + 1) - value(cx, cy + 1, cz + 1)) * wx;
					const double c0 = c00 + (c10 - c00) * wy;
					const double c1 = c01 + (c11 - c01) * wy;
					const double v = c0 + (c1 - c0) * wz;
					out[((size_t)(z * n + y) * n + x) * stride + channel] = (uint8_t)std::lround(Clamp(v, 0.0, 1.0) * 255.0);
				}
			}
		}
	}
	if (stride == 4)
	{
		for (size_t i = 0; i < out.size(); i += 4)
			out[i + 3] = 255;
	}
}

} // namespace EmissiveVolumeCore
