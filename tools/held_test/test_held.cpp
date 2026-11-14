// test_held.cpp -- THE HELD OBJECTS' RULES, ENGINE-FREE (src/roth/roth_heldlogic.h), and the scripts and checks of the
// differential test against the running original (tools/held_test/run_diff.sh).
//
//   held_test unit                 the strike detector, the match's timer and flame point, the lamp's hook / grab /
//                                  release, the light follower, the forced turn's view, the Book's view, the hands
//   held_test scripts DIR          the oracle scripts: every use the rules ask for, played into the original, beside
//                                  the flat clicks' reference (run_diff.sh compares the original's game state)
//   held_test follow OUT           the ORIGINAL's light byte of the niche (s58) and its inventory counts, frame by
//                                  frame (heldprobe.c "L" / "I" lines): the light follower and the Book's palm run
//                                  over them, and must follow them exactly
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roth_heldlogic.h"
#include "roth_wicks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace roth;
using namespace roth::held;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

//==========================================================================
// The stroke
//==========================================================================

// A box with a random orientation and place (deterministic LCG), so nothing depends on world axes.
static uint32_t g_rng = 12345;
static double Rnd() { g_rng = g_rng * 1103515245u + 12345u; return ((g_rng >> 8) & 0xffff) / 65535.0; }
static BoxFrame RandomBox()
{
	BoxFrame b;
	b.centre = V3(Rnd() * 4 - 2, Rnd() * 4 - 2, Rnd() * 2);
	V3 a(Rnd() - 0.5, Rnd() - 0.5, Rnd() - 0.5);
	a = a.Unit();
	V3 t(Rnd() - 0.5, Rnd() - 0.5, Rnd() - 0.5);
	t = (t - a * t.Dot(a)).Unit();
	b.along = a; b.across = t; b.thick = a.Cross(t).Unit();
	return b;
}
static V3 BoxPoint(const BoxFrame &b, double along, double across, double thick)
{
	return b.centre + b.along * along + b.across * across + b.thick * thick;
}

// A straight stroke in the box's frame from (a0, w, h) along the long axis by `run` at `speed`, sampled at 35 Hz,
// optionally with the box itself moving at `boxVel` (world m/s). Returns the number of strikes.
static int Stroke(double a0, double run, double w, double h, double speed, V3 boxVel = V3(), int lead = 3, BoxFrame *use = nullptr)
{
	BoxFrame b = use ? *use : RandomBox();
	StrikeDetector d;
	int strikes = 0;
	const double dt = kTicSeconds;
	const int n = (int)std::ceil(std::fabs(run) / (speed * dt));
	const double dir = run >= 0 ? 1.0 : -1.0;
	for (int i = -lead; i <= n; i++)
	{
		const double a = a0 + dir * std::min(std::fabs(run), speed * dt * (i < 0 ? 0 : i));   // the path ends at a0 + run
		BoxFrame now = b;
		now.centre = b.centre + boxVel * (dt * (i + lead));
		if (d.Sample(now, BoxPoint(now, a, w, h), dt)) strikes++;
	}
	return strikes;
}

static void TestStrike()
{
	const double face = kBoxWidth * 0.5;
	// The design's stroke: the thumb-side strip, 5 mm off its face, 7 cm along at 0.6 m/s.
	CHECK(Stroke(-0.035, 0.07, face + 0.005, 0, 0.6) == 1, "a stroke along the strip lights");
	CHECK(Stroke(-0.035, 0.07, -face - 0.005, 0, 0.6) == 1, "the other strip lights too");
	CHECK(Stroke(0.035, -0.07, face + 0.005, 0, 0.6) == 1, "either direction");
	// Speed: at and above 0.4 m/s it lights, below it does not.
	CHECK(Stroke(-0.035, 0.07, face + 0.005, 0, 0.42) == 1, "0.42 m/s lights");
	CHECK(Stroke(-0.035, 0.07, face + 0.005, 0, 0.30) == 0, "0.30 m/s does not");
	CHECK(Stroke(-0.035, 0.07, face + 0.005, 0, 0.39) == 0, "0.39 m/s does not");
	// Fast: the head crosses the whole strip in one or two tics (35 Hz sampling) and still lights.
	CHECK(Stroke(-0.035, 0.07, face + 0.005, 0, 2.0) == 1, "2 m/s (one tic across the strip) lights");
	CHECK(Stroke(-0.06, 0.12, face + 0.005, 0, 4.0) == 1, "4 m/s lights");
	// Distance from the face: within 1 cm lights, beyond does not; a little inside (a virtual box) lights.
	CHECK(Stroke(-0.035, 0.07, face + 0.0095, 0, 0.6) == 1, "9.5 mm off the face lights");
	CHECK(Stroke(-0.035, 0.07, face + 0.0120, 0, 0.6) == 0, "12 mm off the face does not");
	CHECK(Stroke(-0.035, 0.07, face - 0.003, 0, 0.6) == 1, "3 mm inside lights");
	CHECK(Stroke(-0.035, 0.07, face - 0.010, 0, 0.6) == 0, "10 mm inside (the box's top or bottom) does not");
	// Off the strip's height (the box's flat faces) does not.
	CHECK(Stroke(-0.035, 0.07, face + 0.005, kBoxThick * 0.5 + 0.008, 0.6) == 0, "above the strip does not");
	// Length: a run of under 3 cm along the strip does not light; 3.2 cm does.
	CHECK(Stroke(-0.012, 0.025, face + 0.005, 0, 0.6) == 0, "2.5 cm does not");
	CHECK(Stroke(-0.016, 0.034, face + 0.005, 0, 0.6) == 1, "3.4 cm lights");
	// Short scrubs back and forth never add up.
	{
		BoxFrame b = RandomBox();
		StrikeDetector d;
		int strikes = 0;
		double a = -0.01;
		for (int i = 0; i < 200; i++)
		{
			a += (i / 2) % 2 == 0 ? 0.015 : -0.015;    // 2 cm one way, 2 cm back, at 0.5 m/s
			if (d.Sample(b, BoxPoint(b, a, face + 0.004, 0), kTicSeconds)) strikes++;
		}
		CHECK(strikes == 0, "scrubbing 2-3 cm back and forth never lights (%d)", strikes);
	}
	// Across the strip (perpendicular to the long axis) does not.
	{
		BoxFrame b = RandomBox();
		StrikeDetector d;
		int strikes = 0;
		for (int i = 0; i < 20; i++)
			if (d.Sample(b, BoxPoint(b, 0.0, face + 0.05 - i * 0.02, 0), kTicSeconds)) strikes++;
		CHECK(strikes == 0, "a stroke across the box does not light");
	}
	// The box moving with the hand (the head still relative to the box) never lights; a stroke while both move does.
	CHECK(Stroke(-0.035, 0.0001, face + 0.005, 0, 0.001, V3(1.0, 0.5, 0)) == 0, "box and head moving together: no stroke");
	{
		// both hands moving together fast for half a second, the head resting on the strip
		BoxFrame b = RandomBox();
		StrikeDetector d;
		int strikes = 0;
		for (int i = 0; i < 18; i++)
		{
			BoxFrame now = b;
			now.centre = b.centre + b.along * (1.5 * kTicSeconds * i);     // the BOX moves along its own axis at 1.5 m/s
			if (d.Sample(now, BoxPoint(now, 0.0, face + 0.004, 0), kTicSeconds)) strikes++;
		}
		CHECK(strikes == 0, "the box sliding along its own axis with the head riding it does not light (%d)", strikes);
	}
	CHECK(Stroke(-0.035, 0.07, face + 0.005, 0, 0.6, V3(0.8, -0.3, 0.2)) == 1, "a stroke while both hands move lights");
	// Invariance: the same stroke over 500 random box placements: all light, and none at 0.3 m/s.
	{
		int lit = 0, slow = 0;
		for (int i = 0; i < 500; i++)
		{
			lit += Stroke(-0.035, 0.07, face + 0.005, 0, 0.5 + Rnd());
			slow += Stroke(-0.035, 0.07, face + 0.005, 0, 0.1 + 0.25 * Rnd());
		}
		CHECK(lit == 500 && slow == 0, "500 random boxes: %d of 500 lit fast, %d lit slow", lit, slow);
	}
	// The predicate over a grid, against the rule written out (ITEM_DESIGN 16 step 4): speed >= 0.4, distance off
	// the face in [-4 mm, +10 mm], height within the strip (+-4 mm), and at least 3 cm of the path in that slab.
	{
		int cases = 0, wrong = 0;
		for (int si = 0; si < 12; si++)
			for (int oi = 0; oi < 16; oi++)
				for (int li = 0; li < 9; li++)
				{
					const double speed = 0.2 + si * 0.1;
					const double off = -0.008 + oi * 0.0013;
					const double len = 0.01 + li * 0.008;      // the path, centred on the strip
					// The path's samples move speed x dt a tic; the last tic moves only what is left, and a segment
					// slower than 0.4 m/s does not count (the hand stopping): what counts is the path up to there.
					const double step = speed * kTicSeconds;
					double counted = std::floor(len / step + 1e-9) * step;
					if ((len - counted) / kTicSeconds >= kStrikeSpeed) counted = len;
					const double lo = -len * 0.5, hi = lo + counted;
					const double inSlab = std::max(0.0, std::min(hi, kBoxLength * 0.5) - std::max(lo, -kBoxLength * 0.5));
					const bool expect = speed >= kStrikeSpeed && off >= -kStrikeInside && off <= kStrikeNear && inSlab >= kStrikeRun;
					const bool got = Stroke(-len * 0.5, len, face + off, 0, speed) > 0;
					// cases right on a boundary (within a sample's rounding) are not judged
					const bool edge = std::fabs(speed - kStrikeSpeed) < 0.011 || std::fabs(inSlab - kStrikeRun) < 0.0021
						|| std::fabs(off + kStrikeInside) < 0.0002 || std::fabs(off - kStrikeNear) < 0.0002;
					if (edge) continue;
					cases++;
					if (got != expect) { wrong++; if (wrong < 5) printf("  grid: speed %.2f off %.4f len %.3f: got %d expect %d\n", speed, off, len, got, expect); }
				}
		CHECK(wrong == 0, "the stroke rule over %d grid cases: %d wrong", cases, wrong);
		printf("strike: the stroke rule over %d grid cases (speed x distance x length), %d wrong\n", cases, wrong);
	}
}

//==========================================================================
// The match
//==========================================================================

static void TestMatch()
{
	Match m;
	CHECK(!m.Lit(), "unlit at first");
	CHECK(m.Strike() && m.Lit(), "a stroke lights it");
	CHECK(!m.Strike(), "a second stroke while it burns lights nothing");
	int out = -1;
	for (int t = 1; t <= kMatchTics + 5; t++) { if (m.Tick() && out < 0) out = t; }
	CHECK(out == kMatchTics && out == 700, "it burns 20 s = 700 tics (went out at %d)", out);
	CHECK(!m.Lit(), "out after");
	m.Strike();
	int warnFrom = -1;
	for (int t = 1; t <= kMatchTics; t++) { m.Tick(); if (m.Warning() && warnFrom < 0) warnFrom = t; }
	CHECK(warnFrom == kMatchTics - kMatchWarnTics, "the warning starts 2 s before the end (tic %d)", warnFrom);
	CHECK(m.Struck() == 2, "two struck");
	// The flame: 3.8 cm beyond the fingertip along the finger, whatever the length of the finger vector given.
	const V3 tip(1, 2, 3), dir(0, 0, 5);
	const V3 f = Match::FlamePoint(tip, dir);
	CHECK(std::fabs((f - tip).Length() - (kStickAhead + kFlameBeyond)) < 1e-12 && f.z > tip.z, "the flame point");
	CHECK(std::fabs((Match::StickHead(tip, dir) - Match::StickEnd(tip, dir)).Length() - 0.045) < 1e-12, "the stick is 4.5 cm");
	double lo = 9, hi = -9;
	for (int i = 0; i < 100000; i++) { const double v = Match::Flicker(i * 0.0037); lo = std::min(lo, v); hi = std::max(hi, v); }
	CHECK(lo >= 0.78 - 1e-9 && hi <= 1.0 + 1e-9 && hi - lo > 0.15, "flicker in 0.78..1.0 (%.3f..%.3f)", lo, hi);
	printf("match: 20 s = %d tics, warning from tic %d, flame %.3f m beyond the fingertip, flicker %.3f..%.3f\n", kMatchTics, warnFrom,
		kStickAhead + kFlameBeyond, lo, hi);
}

//==========================================================================
// The lamp
//==========================================================================

static void TestLamp()
{
	LampHook L;
	const V3 bail(0, 0, 1.0);
	bool valid[2] = { true, true }, grip[2] = { false, false };
	V3 palm[2] = { V3(0.4, 0, 1.3), V3(-0.4, 0, 1.3) };
	CHECK(L.Tick(valid, palm, grip, bail) == 0 && L.OnHook(), "on the hook");
	// A grip closed far away, then the hand brought to the bail still closed: no grab (no edge there).
	grip[1] = true;
	CHECK(L.Tick(valid, palm, grip, bail) == 0, "a grip far away takes nothing");
	palm[1] = bail + V3(0.05, 0, 0);
	CHECK(L.Tick(valid, palm, grip, bail) == 0 && L.OnHook(), "a closed hand arriving takes nothing");
	grip[1] = false;
	L.Tick(valid, palm, grip, bail);
	// The off hand's grip closing at the bail: grabbed.
	CHECK(L.WantsGrip(1, palm[1], bail) && !L.WantsGrip(0, palm[0], bail), "the grip at the bail is the lamp's, the other's is not");
	grip[1] = true;
	CHECK(L.Tick(valid, palm, grip, bail) == +1 && L.Hand() == 1, "grabbed by the off hand");
	// Held up: it follows (the caller passes the bail at the palm); the other hand cannot take it.
	palm[1] = V3(-0.3, 0.3, 1.8);
	palm[0] = palm[1] + V3(0.02, 0, 0);
	grip[0] = true;
	CHECK(L.Tick(valid, palm, grip, palm[1]) == 0 && L.Hand() == 1, "the main hand cannot take it from the off hand");
	CHECK(!L.WantsGrip(0, palm[0], palm[1]) && L.WantsGrip(1, palm[1], palm[1]), "the other hand's grip is not the lamp's");
	grip[0] = false;
	L.Tick(valid, palm, grip, palm[1]);
	// Let go: back to the hook.
	grip[1] = false;
	CHECK(L.Tick(valid, palm, grip, palm[1]) == -1 && L.OnHook(), "released: on the hook");
	// The main hand grabs it, then loses tracking: back on the hook.
	palm[0] = bail;
	grip[0] = true;
	CHECK(L.Tick(valid, palm, grip, bail) == +1 && L.Hand() == 0, "grabbed by the main hand");
	valid[0] = false;
	CHECK(L.Tick(valid, palm, grip, bail) == -1 && L.OnHook(), "a lost hand drops it to the hook");
	valid[0] = true;
	// A hand whose grip was already closed when tracking came back is not a grab.
	CHECK(L.Tick(valid, palm, grip, bail) == 0 && L.OnHook(), "a grip already closed as tracking returns takes nothing");
	// Reach: just inside and just outside.
	grip[0] = false; L.Tick(valid, palm, grip, bail);
	palm[0] = bail + V3(0, kLampGrabReach - 0.001, 0); grip[0] = true;
	CHECK(L.Tick(valid, palm, grip, bail) == +1, "within reach grabs");
	grip[0] = false; L.Tick(valid, palm, grip, bail);
	palm[0] = bail + V3(0, kLampGrabReach + 0.001, 0); grip[0] = true;
	CHECK(L.Tick(valid, palm, grip, bail) == 0, "beyond reach does not");
	// The hook: off-hand side at the hip.
	const V3 feet(10, 20, 0), fwd(1, 0, 0), right(0, -1, 0), up(0, 0, 1);
	const V3 hl = LampHook::HookPoint(feet, fwd, right, up, 1.65, -1), hr = LampHook::HookPoint(feet, fwd, right, up, 1.65, +1);
	CHECK(std::fabs(hl.z - 1.65 * kHookHeight) < 1e-12 && hl.y > feet.y && hr.y < feet.y && hl.x > feet.x, "the hook at the hip, off-hand side");
	CHECK(std::fabs((LampHook::GlobeBelow(bail, up) - bail).Length() - kGlobeBelowBail) < 1e-12, "the globe below the bail");
	printf("lamp: hook at %.2f x eye height, %.2f m to the side; grab within %.2f m on a grip's closing edge\n", kHookHeight, kHookSide, kLampGrabReach);
}

//==========================================================================
// The light follower, the turn, the Book, the hands
//==========================================================================

// The niche's ramp as the original runs it (the 0x1d effect: 2 a tic, tick_change_lighting): +40, a wait, +30, a
// wait, -30. Returns the bytes, one a tic.
static std::vector<int> NicheTrace(int base)
{
	std::vector<int> v;
	int l = base;
	for (int i = 0; i < 10; i++) v.push_back(l);
	for (int s = 0; s < 20; s++) { l += 2; v.push_back(l); }
	for (int i = 0; i < 155; i++) v.push_back(l);
	for (int s = 0; s < 15; s++) { l += 2; v.push_back(l); }
	for (int i = 0; i < 35; i++) v.push_back(l);
	for (int s = 0; s < 15; s++) { l -= 2; v.push_back(l); }
	for (int i = 0; i < 40; i++) v.push_back(l);
	return v;
}

static void TestFollow()
{
	LightFollower f;
	f.Begin(78);
	const std::vector<int> t = NicheTrace(78);
	int starts = 0, ends = 0, bad = 0;
	double peak = 0;
	for (int l : t)
	{
		const double i = f.Sample(l);
		if (f.ChangeStarted()) starts++;
		if (f.ChangeEnded()) ends++;
		const double want = std::min(1.0, std::max(0.0, (l - 78) / 70.0));
		if (std::fabs(i - want) > 1e-12) bad++;
		peak = std::max(peak, i);
	}
	CHECK(f.Changes() == 3 && starts == 3 && ends == 3, "three changes (%d, %d starts, %d ends)", f.Changes(), starts, ends);
	CHECK(bad == 0 && std::fabs(peak - 1.0) < 1e-12, "the intensity is the byte over the pack's value, /70 (%d off)", bad);
	CHECK(std::fabs(f.Intensity(78 + 40) - 40.0 / 70.0) < 1e-12 && f.Intensity(10) == 0.0, "after the pulses it stays at +40");
	printf("follow: the niche's ramp (+40, +30, -30): %d changes, peak %.2f, rest at %.2f\n", f.Changes(), peak, f.Intensity(t.back()));

	CHECK(ForcedTurnView(false, true) == TurnView::None && ForcedTurnView(false, false) == TurnView::None, "flat: nothing");
	CHECK(ForcedTurnView(true, true) == TurnView::AtBlack, "VR, a movie queued: at the black");
	CHECK(ForcedTurnView(true, false) == TurnView::Now, "VR, no movie: now");
}

static void TestBook()
{
	int counts[300] = {};
	auto C = [&](int id) { return counts[id]; };
	BookView b;
	counts[17] = 1; counts[16] = 1;
	CHECK(b.Tick(0, 282, C) == BookView::NONE && b.Item() == 0, "no popup");
	CHECK(b.Tick(16, 282, C) == BookView::NONE && b.Item() == 0, "the Matchbox's popup is not the Book's");
	CHECK(b.Tick(0, 282, C) == BookView::NONE, "closed");
	CHECK(b.Tick(17, 282, C) == BookView::OPENED && b.Item() == 17 && b.Palm() == 0, "the Book's popup opens in both hands");
	CHECK(b.Tick(17, 282, C) == BookView::NONE, "a tic later, nothing");
	counts[20] = 1;      // the original's OnInspect gives the silver key
	CHECK(b.Tick(17, 282, C) == BookView::GIVEN && b.Palm() == 20, "the key drops into the palm the tic it is given");
	counts[5] = 1;
	CHECK(b.Tick(17, 282, C) == BookView::NONE && b.Palm() == 20, "one item in the palm");
	CHECK(b.Tick(0, 282, C) == BookView::CLOSED && b.Item() == 0 && b.Palm() == 0, "closed: the hands let go, the palm is empty");
	CHECK(b.Tick(17, 282, C) == BookView::OPENED && b.Palm() == 0, "the second look: open, nothing in the palm");
	CHECK(b.Tick(17, 282, C) == BookView::NONE && b.Palm() == 0, "and nothing given");
	CHECK(b.Tick(0, 282, C) == BookView::CLOSED, "closed again");

	HandShows s[2];
	WhatHandsShow(16, -1, false, 0, 0, 0, 1, s);
	CHECK(s[0].item == 16 && s[1].item == 0 && !s[1].match, "the right-hand item in the item hand");
	WhatHandsShow(16, -1, true, 0, 0, 0, 1, s);
	CHECK(s[0].item == 16 && s[1].match, "the match in the off hand");
	WhatHandsShow(16, 1, true, 0, 0, 0, 1, s);
	CHECK(s[1].lamp && !s[1].match && s[0].item == 16, "the off hand with the lamp holds no match");
	WhatHandsShow(16, 0, false, 0, 0, 0, 1, s);
	CHECK(s[0].lamp && s[0].item == 0, "the item hand with the lamp shows the lamp, not its item");
	WhatHandsShow(17, -1, true, 17, 20, 0, 1, s);
	CHECK(s[0].book && s[1].book && s[1].palm == 20 && s[0].item == 0 && !s[1].match, "the Book in both hands, the key in the off palm");
	WhatHandsShow(0, -1, false, 0, 0, 1, 0, s);
	CHECK(s[0].item == 0 && s[1].item == 0, "nothing selected: nothing shown");
}

//==========================================================================
// The scripts for the original (run_diff.sh)
//==========================================================================

// The rules run over a session the way the engine runs them: a stroke at tic `strikeAt` (the off hand's fingertip
// along the striker at 0.6 m/s, through StrikeDetector), then the flame visiting wicks (tic, candle, wick); each
// candle's WickLighting decides which visit asks for the original's use. A visit while no match burns asks nothing.
struct Visit { int tic; int candle; int wick; };
struct Candle { const char *obj; int wicks; };
static std::vector<std::pair<int, int>> Session(int strikeAt, const std::vector<Visit> &visits, const std::vector<Candle> &candles,
	int strikeAgainAt = -1)
{
	std::vector<std::pair<int, int>> uses;   // (tic, candle)
	std::vector<WickLighting> w(candles.size());
	for (size_t i = 0; i < candles.size(); i++) w[i].Reset(candles[i].wicks);
	Match m;
	StrikeDetector d;
	BoxFrame box = RandomBox();
	const double face = kBoxWidth * 0.5;
	int last = 0;
	for (const Visit &v : visits) last = std::max(last, v.tic);
	size_t vi = 0;
	for (int t = 0; t <= last + 1; t++)
	{
		m.Tick();
		// the stroke: 3 tics resting at the start, then 0.6 m/s along 7 cm
		for (int s : { strikeAt, strikeAgainAt })
		{
			if (s < 0) continue;
			if (t >= s - 3 && t <= s + 5)
			{
				const int k = t - (s - 3) - 3;
				const double a = -0.035 + (k < 0 ? 0 : k) * 0.6 * kTicSeconds;
				if (d.Sample(box, BoxPoint(box, std::min(a, 0.04), face + 0.004, 0), kTicSeconds)) m.Strike();
			}
		}
		for (WickLighting &x : w) if (x.IsStarted()) x.Tick();
		for (; vi < visits.size() && visits[vi].tic == t; vi++)
		{
			if (!m.Lit()) continue;
			WickLighting &x = w[visits[vi].candle];
			const int ask = x.Flames(1u << visits[vi].wick);
			if (ask >= 0) { uses.push_back(std::make_pair(t, visits[vi].candle)); x.Started(ask); }
			x.Flames(0);    // the flame leaves
		}
	}
	return uses;
}

static int Scripts(const char *dir)
{
	auto open = [&](const char *name) {
		const std::string p = std::string(dir) + "/" + name + ".script";
		FILE *f = fopen(p.c_str(), "w");
		if (!f) { printf("cannot write %s\n", p.c_str()); exit(2); }
		// The Matchbox in the right hand, as at new game (candles_test's head), and the probes' watches.
		fprintf(f, "40 give 16\n40 right 16\n40 watchlight 58\n40 watchitem 17\n40 watchitem 20\n40 snap\n");
		return f;
	};
	const int end = 2600;   // past every chain: the burnt-out session's niche shot leaves about frame 1430
	auto tail = [&](FILE *f) { fprintf(f, "%d diff end\n%d quit\n", end, end + 1); fclose(f); };
	const std::vector<Candle> candles = { { "211 0", 5 }, { "368 0", 1 }, { "368 1", 1 } };

	// THE REFERENCE: the flat clicks. The candelabrum at 100; the two tall candles at 300 and 400 (the second well
	// past the first's 30-tic delay); the Book taken at 1700 (after every candle and every candle's chain, in every
	// session, so the chains' globals end in the same order: the original selects the Book into the right hand, and
	// a candle clicked then would only auto-select the Matchbox) and looked at, first at 1800, again at 1900.
	{
		FILE *f = open("ref");
		fprintf(f, "100 click 211 0\n300 click 368 0\n400 click 368 1\n1700 click 412 0\n1800 inspect 17\n1820 pump\n1900 inspect 17\n1920 pump\n");
		tail(f);
	}
	struct Sc { const char *name; int strike; int again; std::vector<Visit> visits; };
	const std::vector<Sc> sc = {
		// the match struck at 90, the flame at the candelabrum's wicks 3 5 1 4 2 from 100; it goes out at 790. Struck
		// again at 290 (no: it still burns -- the stroke lights nothing), the tall candles at 300 and 400.
		{ "match_session", 90, 290, { { 100, 0, 2 }, { 106, 0, 4 }, { 111, 0, 0 }, { 119, 0, 3 }, { 125, 0, 1 }, { 300, 1, 0 }, { 400, 2, 0 } } },
		// the flame at the tall candle before any match is struck asks nothing; struck at 95, lit at 100
		{ "match_late_strike", 95, -1, { { 60, 0, 0 }, { 100, 0, 0 }, { 300, 1, 0 }, { 301, 1, 0 }, { 302, 1, 0 }, { 400, 2, 0 } } },
		// the match goes out (struck at 0 + 3: out at 703 + ...), so this struck-at-99 one lights everything; then
		// the flame lingering at every wick for many tics asks nothing more
		{ "match_linger", 99, -1, { { 100, 0, 0 }, { 101, 0, 1 }, { 102, 0, 2 }, { 103, 0, 3 }, { 104, 0, 4 }, { 105, 0, 0 }, { 106, 0, 1 },
			{ 300, 1, 0 }, { 302, 1, 0 }, { 304, 1, 0 }, { 400, 2, 0 }, { 402, 2, 0 } } },
		// the match struck at 90 lights the candelabrum and burns out (20 s: about tic 792); the flame's place at the
		// tall candle at 850 asks nothing (no match burns); struck again at 880, the tall candles at 900 and 1000
		{ "match_burnt_out", 90, 880, { { 100, 0, 4 }, { 850, 1, 0 }, { 900, 1, 0 }, { 1000, 2, 0 } } },
	};
	for (const Sc &s : sc)
	{
		const auto uses = Session(s.strike, s.visits, candles, s.again);
		FILE *f = open(s.name);
		for (const auto &u : uses) fprintf(f, "%d click %s\n", u.first, candles[(size_t)u.second].obj);
		// the Book: the hands take it (the grip's use) and look at it twice; the VR layer asks nothing else
		fprintf(f, "1700 click 412 0\n1800 inspect 17\n1820 pump\n1900 inspect 17\n1920 pump\n");
		tail(f);
		printf("SCRIPT %s: %zu flame visit(s) -> %zu use(s):", s.name, s.visits.size(), uses.size());
		for (const auto &u : uses) printf(" %d@%s", u.first, candles[(size_t)u.second].obj);
		printf("\n");
	}
	// FOR THE PALM ONLY (not compared with the reference: it changes the game): while the Book's popup is up the
	// original's own give_item gives the key, as an OnInspect that passed its gates would -- the palm must show it
	// the frame the original's inventory has it, and the second look must show nothing.
	{
		FILE *f = open("follow_book_give");
		fprintf(f, "1100 click 412 0\n1200 inspect 17\n1210 give 20\n1220 pump\n1300 inspect 17\n1320 pump\n1400 quit\n");
		fclose(f);
	}
	return 0;
}

//==========================================================================
// The rules over the original's own trace (run_diff.sh: heldprobe "L" and "I" lines)
//==========================================================================

static int Follow(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) { printf("cannot read %s\n", path); return 2; }
	std::map<int, int> light;                  // frame -> s58's byte (the original's)
	std::map<int, std::map<int, int>> items;   // frame -> id -> slots
	char line[256];
	int lastFrame = 0, tallClicks = 0;
	while (fgets(line, sizeof line, f))
	{
		int fr, a, b;
		if (sscanf(line, "L %d %d %d", &fr, &a, &b) == 3 && a == 58) light[fr] = b;
		if (sscanf(line, "I %d %d %d", &fr, &a, &b) == 3) items[fr][a] = b;
		if (!strncmp(line, "R click 368 ", 12)) tallClicks++;
		if (sscanf(line, "R inspect %d", &a) == 1 && sscanf(line, "%*[^(](frame %d)", &fr) == 1) items[fr][-1] = a;   // the popup opens
		lastFrame = std::max(lastFrame, fr);
	}
	fclose(f);
	if (light.empty()) { printf("FOLLOW: no light lines for s58 in %s\n", path); return 1; }
	// The follower, tic by tic over the original's byte (one frame = one tic, as the scripts).
	LightFollower lf;
	lf.Begin(light.begin()->second);
	int cur = light.begin()->second, bad = 0, peakFrame = -1, starts = 0;
	double peak = 0;
	std::vector<int> startFrames;
	for (int fr = light.begin()->first; fr <= lastFrame; fr++)
	{
		auto it = light.find(fr);
		if (it != light.end()) cur = it->second;
		const double i = lf.Sample(cur);
		if (lf.ChangeStarted()) { starts++; startFrames.push_back(fr); }
		if (std::fabs(i - lf.Intensity(cur)) > 1e-12) bad++;
		if (i > peak) { peak = i; peakFrame = fr; }
	}
	printf("FOLLOW: the original's s58 byte from %d: %zu change(s) seen; the light follower: %d change(s) started at frame(s)", light.begin()->second,
		light.size() - 1, starts);
	for (int s : startFrames) printf(" %d", s);
	printf("; peak %.2f at frame %d (byte %d); %d frame(s) where the light was not the byte's\n", peak, peakFrame, lf.Peak(), bad);
	// Both tall candles clicked: the niche's three changes, peaking 70 over the pack's. Otherwise none.
	const bool lit = tallClicks >= 2;
	CHECK(starts == (lit ? 3 : 0), "the original's niche makes %d light changes; the follower saw %d", lit ? 3 : 0, starts);
	CHECK(bad == 0, "the light follows the byte");
	CHECK(lf.Peak() - lf.Base() == (lit ? 70 : 0), "the byte peaks %d over the pack's (%d)", lit ? 70 : 0, lf.Peak() - lf.Base());
	// The Book: the palm shows what the original gave while its popup was up, the frame it gave it, and nothing
	// else. (The original's OnInspect of the Book stops at its gates -- `if-flag 4` then `!if-flag 4` can never both
	// pass, dbase100.c:803-808 -- so on this install the expected count is 0; the rule's own give case is in `unit`.)
	BookView bv;
	std::map<int, int> have;
	int popup = 0, popupLeft = 0, events = 0, wrong = 0, origGives = 0;
	for (int fr = 0; fr <= lastFrame; fr++)
	{
		int rose = 0;
		auto it = items.find(fr);
		if (it != items.end())
			for (auto &x : it->second)
			{
				if (x.first == -1) { popup = x.second; popupLeft = 60; }   // the popup up for 60 frames from its open (its lines pumped at +20)
				else { if (x.second > have[x.first] && popupLeft > 0 && x.first != popup) rose = x.first; have[x.first] = x.second; }
			}
		if (rose != 0) origGives++;
		const int ev = bv.Tick(popupLeft > 0 ? popup : 0, 282, [&](int id) { auto h = have.find(id); return h == have.end() ? 0 : h->second; });
		const bool shown = (ev & BookView::GIVEN) != 0;
		if (shown) events++;
		if (shown != (rose != 0) || (shown && bv.Palm() != rose)) { wrong++; printf("  frame %d: the original gave %d, the palm shows %d\n", fr, rose, shown ? bv.Palm() : 0); }
		if (popupLeft > 0) popupLeft--;
	}
	printf("FOLLOW: the Book looked at %s; the original gave %d item(s) while its popup was up; the palm showed %d; %d frame(s) wrong\n",
		items.empty() ? "never" : "twice", origGives, events, wrong);
	CHECK(wrong == 0, "the palm shows exactly what the original gives during the Book's popup, the frame it gives it");
	return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "scripts")) return Scripts(argv[2]);
	if (argc >= 3 && !strcmp(argv[1], "follow"))
	{
		const int r = Follow(argv[2]);
		printf("held_test follow: %d checks passed, %d failed\n", g_pass, g_fail);
		return r;
	}
	if (argc >= 2 && !strcmp(argv[1], "unit"))
	{
		TestStrike();
		TestMatch();
		TestLamp();
		TestFollow();
		TestBook();
		printf("held_test unit: %d checks passed, %d failed\n", g_pass, g_fail);
		return g_fail ? 1 : 0;
	}
	printf("usage: held_test unit | scripts DIR | follow OUT\n");
	return 2;
}
