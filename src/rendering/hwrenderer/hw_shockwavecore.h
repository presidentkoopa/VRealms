/*
** hw_shockwavecore.h
**
** [SHOCKWAVE] The maths of blast ripples, with no engine dependency.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** A blast ripple is a ring of bent air racing out from a blast ("Engine docs/BLAST_RIPPLE_PLAN.md" with its owner answers of
** 2026-09-15; "Engine docs/BLAST_RIPPLE_IMPL_NOTES.md"). A mod starts one with LevelLocals.SpawnShockwave (FLevelLocals::
** Shockwave, g_levellocals.h); the renderer animates it every frame (SetupShockwaves, hw_drawinfo.cpp); the heat shimmer pass
** draws it (PPHeatRefraction, hw_postprocess.h; shaders/pp/shockwaveoffset.fp and heatwarp.fp).
**
** Everything a ripple does that is not plumbing lives here:
**   - the owner's menu choices: the look (LookFor), the near-eye feel (NearFade, Coverage), the colour fringe (ChromaFor);
**   - one ripple at an age: its crest, thickness and envelope (CurveAt), and this frame's bend, fringe and rank (Evaluate);
**   - the cap on whole-view wobbles (WobbleCap) and each ripple's memory of it (NearState);
**   - one eye's conservative screen rectangle of a ball, which is also the cull (ProjectBall);
**   - the test ripples' schedule (TestSchedule).
**
** BOTH EYES GET THE SAME STRENGTH. Evaluate takes the HEAD centre (the mean of the eye positions), never one eye: the near fade
** and the coverage guard change with distance, and two eyes an interpupillary distance apart must not disagree about how hard
** the same air bends.
**
** Deterministic: no RNG and no clock of its own (the caller passes a ripple's age and the frame time). Presentation only:
** nothing here is read back by the playsim.
**
** AXES. Positions are GL world axes (game x, game z, game y) in map units, as the shader takes them.
*/

#pragma once

#include <algorithm>
#include <cmath>

namespace ShockwaveCore
{

// Radians of deflection per unit of shockwaveoffset.fp's profile integral, at strength 1 and every other factor 1. Calibrated
// by the float64 mirror (scratchpad ripple/mirror_ripple.py, "SB"; 0.00296497): a 192-unit Clear-ring ripple seen from 480
// units by a headset eye (tan 1) shifts its crest pixels by 0.008 scene UV at the height of its envelope -- about 16 pixels at
// 2016 wide. The bend pass still caps any pixel's summed shift at PPHeatRefraction::MAX_SHIFT (0.03).
static constexpr double SHOCK_BEND = 2.965e-3;

// Below this bend (radians per profile unit) a ripple is not drawn: nothing it could shift would reach a hundredth of a pixel.
static constexpr double BEND_DRAW_MIN = 1.0e-7;

// A whole-view wobble lasts a tenth of a second (3.5 tics of the ripple's own clock), and at most WOBBLES_PER_WINDOW of them
// start inside WOBBLE_WINDOW_MS of real time.
static constexpr double WOBBLE_TICS = 3.5;
static constexpr double WOBBLE_WINDOW_MS = 1000.0;
static constexpr int WOBBLES_PER_WINDOW = 2;

// The colour fringe: below CHROMA_MIN the frame takes the plain bend program; never more than CHROMA_MAX of the shift.
static constexpr double CHROMA_MIN = 0.01;
static constexpr double CHROMA_MAX = 0.9;

inline double Smoothstep(double edge0, double edge1, double x)
{
	const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

// ---------------------------------------------------------------------------------------------------------------------
// The owner's choices (r_shockwave_look, r_shockwave_near, r_shockwave_chroma; hw_cvars.cpp). Out-of-range values read as the
// default, so a hand-edited ini cannot reach a look nobody chose.

struct Look
{
	double Gain;			// multiplies the bend
	double Trough;			// how deep the thin air behind the crest dips, against the crest's 1 (shockwaveoffset.fp)
	double ThicknessScale;	// multiplies the ring's thickness
};

inline Look LookFor(int look)
{
	switch (look)
	{
	case 1:  return { 0.45, 0.05, 0.8 };	// Subtle: noticed against detailed walls
	case 2:  return { 1.6, 0.35, 1.6 };		// Heavy lens: a thick shell that bulges, then breaks away as a ring
	default: return { 1.0, 0.15, 1.0 };		// Clear ring (default)
	}
}

inline int NearMode(int near) { return (near == 1 || near == 2) ? near : 0; }
inline int ChromaMode(int chroma) { return (chroma == 0 || chroma == 2) ? chroma : 1; }

// ---------------------------------------------------------------------------------------------------------------------
// One ripple at age k = age / tics (0..1).
//   crest radius     R x (0.08 + 0.92 x (1 - (1 - k)^3))   fast out, slowing like a blast wave
//   half thickness   T x (0.4 + 0.6 k) / 2                 T = thickness, or auto 0.3 R clamped to 8..256, times the look
//   envelope         smoothstep(0, 0.08, k) x (1 - k)^2    a hard hit in the first 8% of life, then fading

struct Curve
{
	double Crest = 0.0;
	double HalfThickness = 0.0;
	double Envelope = 0.0;
};

inline double AutoThickness(double radius) { return std::clamp(0.3 * radius, 8.0, 256.0); }

inline Curve CurveAt(double k, double radius, double thickness, const Look &look)
{
	k = std::clamp(k, 0.0, 1.0);
	const double left = 1.0 - k;
	Curve c;
	c.Crest = radius * (0.08 + 0.92 * (1.0 - left * left * left));
	const double t = (thickness > 0.0 ? thickness : AutoThickness(radius)) * look.ThicknessScale;
	c.HalfThickness = t * (0.4 + 0.6 * k) * 0.5;
	c.Envelope = Smoothstep(0.0, 0.08, k) * left * left;
	return c;
}

// ---------------------------------------------------------------------------------------------------------------------
// NEAR THE EYES. gap = distance(head, centre) - crest: how far the crest still is from reaching the head (negative once it has
// passed). The whole view warps only while the head is inside the ripple's outer ball (gap <= half thickness): every ray then
// starts inside the shell.
//
//   0 "Fade before it reaches you" (default): smoothstep(24 + h, 96 + 2h, gap). At the outer ball the fade is already 0, and it
//     stays 0 while the head is inside, so the whole view never warps.
//   1 "Feel it pass" / 2 "Feel it pass (strong)": the fade never drops below a floor (0.35 / 0.7) while the crest approaches;
//     when the head comes inside, that floor fades out over WOBBLE_TICS -- one brief whole-view wobble -- and then the ripple
//     is gone for this viewer. Only WOBBLES_PER_WINDOW ripples a second get the floor; the decision is made once per ripple,
//     when its crest first comes within the fade (gap <= 96 + 2h), where the default fade is still 1, so it never pops.
//     A ripple refused the floor takes the default fade.

struct WobbleCap
{
	double Grants[WOBBLES_PER_WINDOW] = { -1.0e30, -1.0e30 };	// frame times (ms) of the latest grants

	bool TryGrant(double nowMs)
	{
		int oldest = 0;
		for (int i = 1; i < WOBBLES_PER_WINDOW; i++)
			if (Grants[i] < Grants[oldest]) oldest = i;
		if (nowMs - Grants[oldest] < WOBBLE_WINDOW_MS)
			return false;
		Grants[oldest] = nowMs;
		return true;
	}
};

// The renderer's memory of one ripple, keyed by its spawn serial.
struct NearState
{
	unsigned Serial = 0;
	bool Decided = false;
	bool Granted = false;
	double PassStart = -1.0;	// the ripple's age (tics) when the head first came inside its outer ball
};

// The memory for `serial`, forgotten when the slot now holds a different ripple.
inline NearState &Remember(NearState &state, unsigned serial)
{
	if (state.Serial != serial)
	{
		state = NearState();
		state.Serial = serial;
	}
	return state;
}

inline double FloorFor(int near) { return near == 1 ? 0.35 : (near == 2 ? 0.7 : 0.0); }
inline double GapFade(double gap, double h) { return Smoothstep(24.0 + h, 96.0 + 2.0 * h, gap); }
inline double ApproachGap(double h) { return 96.0 + 2.0 * h; }

inline double NearFade(int near, double gap, double h, double age, double nowMs, NearState &state, WobbleCap &cap)
{
	const double fade = GapFade(gap, h);
	near = NearMode(near);
	if (near == 0)
		return fade;
	if (!state.Decided && gap <= ApproachGap(h))
	{
		state.Decided = true;
		state.Granted = cap.TryGrant(nowMs);
	}
	if (!state.Granted)
		return fade;
	if (state.PassStart < 0.0 && gap <= h)
		state.PassStart = age;
	const double floor = FloorFor(near);
	if (state.PassStart < 0.0)
		return std::max(fade, floor);
	return std::max(fade, floor * (1.0 - Smoothstep(0.0, 1.0, (age - state.PassStart) / WOBBLE_TICS)));
}

// THE BIGGER IT LOOKS, THE GENTLER. theta is the outer ball's angular radius from the head (90 degrees inside it).
inline double Coverage(int near, double outerRadius, double distance)
{
	const double sine = distance > outerRadius ? outerRadius / distance : 1.0;
	const double theta = std::asin(std::clamp(sine, 0.0, 1.0)) * (180.0 / 3.14159265358979323846);
	const double give = NearMode(near) == 0 ? 0.5 : 0.3;
	return 1.0 - give * Smoothstep(35.0, 75.0, theta);
}

// ---------------------------------------------------------------------------------------------------------------------
// THE COLOUR FRINGE, one value for the frame (the largest over the drawn ripples), so both eyes pick the same program.
//   0 None; 1 Faint, where the blast asks for it (its chroma 0..1; the default); 2 Strong on every ripple.
// `visibility` is the near fade times the coverage guard: a ripple faded near the eyes does not split the whole frame.

inline double ChromaFor(int setting, double blastChroma, double envelope, double visibility)
{
	switch (ChromaMode(setting))
	{
	case 0:  return 0.0;
	case 2:  return 0.6 * envelope * visibility;
	default: return 0.3 * std::clamp(blastChroma, 0.0, 1.0) * envelope * visibility;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// ONE RIPPLE THIS FRAME.

struct Input
{
	double Centre[3] = { 0.0, 0.0, 0.0 };	// GL world axes, the anchor already applied
	double Radius = 0.0;
	double Thickness = 0.0;
	double Strength = 0.0;
	double Chroma = 0.0;
	double Tics = 0.0;
	double Age = 0.0;						// tics since it started (level time; real time for a test ripple)
};

struct Settings
{
	int Look = 0;
	int Near = 0;
	int Chroma = 1;
	double Scale = 1.0;
};

inline Settings SettingsFrom(int look, int near, int chroma, double scale)
{
	Settings s;
	s.Look = (look == 1 || look == 2) ? look : 0;
	s.Near = NearMode(near);
	s.Chroma = ChromaMode(chroma);
	s.Scale = std::isfinite(scale) ? std::clamp(scale, 0.0, 2.0) : 1.0;
	return s;
}

struct Output
{
	bool Live = false;			// still within its life
	bool Draw = false;			// bends enough to be drawn
	double Crest = 0.0;
	double HalfThickness = 0.0;
	double OuterRadius = 0.0;	// crest + half thickness: the shell's bounding ball
	double Trough = 0.0;
	double Envelope = 0.0;
	double Near = 0.0;
	double Coverage = 0.0;
	double Bend = 0.0;			// the shader's Bend: radians per unit of the profile integral
	double Chroma = 0.0;		// this ripple's fringe; the frame takes the largest drawn
	double Rank = 0.0;			// bend x apparent size, for the visible budget
	double Distance = 0.0;		// head to centre
};

inline Output Evaluate(const Input &in, const Settings &s, const double head[3], double nowMs, NearState &state, WobbleCap &cap)
{
	Output o;
	if (!(in.Tics > 0.0) || !(in.Age >= 0.0) || in.Age >= in.Tics)
		return o;
	o.Live = true;
	const Look look = LookFor(s.Look);
	const Curve c = CurveAt(in.Age / in.Tics, in.Radius, in.Thickness, look);
	o.Crest = c.Crest;
	o.HalfThickness = c.HalfThickness;
	o.OuterRadius = c.Crest + c.HalfThickness;
	o.Trough = look.Trough;
	o.Envelope = c.Envelope;

	const double dx = in.Centre[0] - head[0], dy = in.Centre[1] - head[1], dz = in.Centre[2] - head[2];
	o.Distance = std::sqrt(dx * dx + dy * dy + dz * dz);
	o.Near = NearFade(s.Near, o.Distance - c.Crest, c.HalfThickness, in.Age, nowMs, state, cap);
	o.Coverage = Coverage(s.Near, o.OuterRadius, o.Distance);

	const double strength = in.Strength * c.Envelope * look.Gain * s.Scale * o.Near * o.Coverage;
	o.Bend = strength * SHOCK_BEND;
	o.Draw = o.Bend > BEND_DRAW_MIN;
	if (!o.Draw)
		return o;
	o.Chroma = ChromaFor(s.Chroma, in.Chroma, c.Envelope, o.Near * o.Coverage);
	o.Rank = strength * (o.Distance > o.OuterRadius ? o.OuterRadius / o.Distance : 1.0);
	return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// ONE EYE'S VIEW OF A BALL, in scene UV, built exactly as shockwaveoffset.fp builds a pixel's ray: the view vector v of a
// point is the eye's view matrix applied to it (column-major 4x4, VSMatrix::get's order, GL world to view space) and
//   uv = ((v.xy / -v.z) / TanHalfFov - ProjOffset + 1) / 2.
// The eight corners of the ball's box are projected:
//   Hidden  every corner at or behind the eye plane, or the rectangle wholly off the view;
//   Full    some corners ahead of the eye plane and some not: no finite rectangle, the draw covers the viewport;
//   Rect    rect = { u0, v0, u1, v1 }, clamped to 0..1.
// A box holds its ball, and a perspective projection of points all ahead of the eye takes the box's hull to the hull of the
// projected corners, so every pixel whose ray can meet the ball lies inside the rectangle.

enum class BallView { Hidden, Rect, Full };

static constexpr double EYE_PLANE_MARGIN = 1.0e-3;	// map units

inline BallView ProjectBall(const float *view, double tanX, double tanY, double offX, double offY,
	double cx, double cy, double cz, double radius, double rect[4])
{
	int ahead = 0;
	double u0 = 1.0e30, v0 = 1.0e30, u1 = -1.0e30, v1 = -1.0e30;
	for (int i = 0; i < 8; i++)
	{
		const double px = cx + ((i & 1) ? radius : -radius);
		const double py = cy + ((i & 2) ? radius : -radius);
		const double pz = cz + ((i & 4) ? radius : -radius);
		const double vx = view[0] * px + view[4] * py + view[8] * pz + view[12];
		const double vy = view[1] * px + view[5] * py + view[9] * pz + view[13];
		const double vz = view[2] * px + view[6] * py + view[10] * pz + view[14];
		if (!(vz < -EYE_PLANE_MARGIN))
			continue;
		ahead++;
		const double u = ((vx / -vz) / tanX - offX + 1.0) * 0.5;
		const double v = ((vy / -vz) / tanY - offY + 1.0) * 0.5;
		u0 = std::min(u0, u); u1 = std::max(u1, u);
		v0 = std::min(v0, v); v1 = std::max(v1, v);
	}
	if (ahead == 0)
		return BallView::Hidden;
	if (ahead < 8)
	{
		rect[0] = 0.0; rect[1] = 0.0; rect[2] = 1.0; rect[3] = 1.0;
		return BallView::Full;
	}
	if (!(u1 >= 0.0 && u0 <= 1.0 && v1 >= 0.0 && v0 <= 1.0))
		return BallView::Hidden;
	rect[0] = std::clamp(u0, 0.0, 1.0);
	rect[1] = std::clamp(v0, 0.0, 1.0);
	rect[2] = std::clamp(u1, 0.0, 1.0);
	rect[3] = std::clamp(v1, 0.0, 1.0);
	return BallView::Rect;
}

// The texels a rectangle needs in the offset texture (one texel of margin, never empty) are the pass's business:
// ShockwaveViewport in hw_postprocess.cpp, beside the texture it sizes.

// ---------------------------------------------------------------------------------------------------------------------
// THE TEST RIPPLES (r_shockwave_test): 1 a ripple every TEST_PERIOD_MS, 192 units ahead of where the head looks when it starts;
// 2 the near-eye test, 24 units ahead. Real time (the frame clock), so they run with a menu open. Made by the renderer: no
// level slot, nothing in the playsim sees them.

static constexpr double TEST_PERIOD_MS = 2000.0;
static constexpr double TEST_RADIUS = 192.0;
static constexpr double TEST_STRENGTH = 1.0;
static constexpr double TEST_TICS = 14.0;
static constexpr double TEST_CHROMA = 0.3;

inline double TestDistance(int mode) { return mode == 2 ? 24.0 : 192.0; }

struct TestSchedule
{
	int Mode = 0;
	double OnMs = 0.0;
	long long Period = -1;
	double StartMs = 0.0;
	double Pos[3] = { 0.0, 0.0, 0.0 };
};

// Moves the schedule to nowMs. True when a new test ripple starts this frame: the caller places it (Pos) ahead of the head.
inline bool TestAdvance(TestSchedule &t, int mode, double nowMs)
{
	if (mode != 1 && mode != 2)
	{
		t.Mode = 0;
		t.Period = -1;
		return false;
	}
	if (t.Mode != mode || nowMs < t.OnMs)
	{
		t.Mode = mode;
		t.OnMs = nowMs;
		t.Period = -1;
	}
	const long long period = (long long)std::floor((nowMs - t.OnMs) / TEST_PERIOD_MS);
	if (period == t.Period)
		return false;
	t.Period = period;
	t.StartMs = t.OnMs + (double)period * TEST_PERIOD_MS;
	return true;
}

// The test ripple's age in tics (35 a second).
inline double TestAge(const TestSchedule &t, double nowMs) { return (nowMs - t.StartMs) * 35.0 / 1000.0; }

} // namespace ShockwaveCore
