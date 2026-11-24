// tools/track_test: the tracks a hand moves things along (src/roth/roth_track.h, ported from VRExpansionPlugin's
// VRLever / VRDial / VRSlider / VRButton components), engine-free. Scripted hands with the outcome each must give.
// Exit 0 = every check passed.
#include "roth_track.h"
#include <stdio.h>
#include <math.h>

using namespace roth::track;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static bool Near(double a, double b, double e = 1e-6) { return fabs(a - b) <= e; }
static const double kDt = 1.0 / 35.0;

// A point `r` metres from the axis at `deg` degrees about it.
static V3 At(double r, double deg, double z = 0) { const double a = deg * kPi / 180.0; return V3(r * cos(a), r * sin(a), z); }

static void TestHelpers()
{
	CHECK(Near(NormalizeAxis(190), -170) && Near(NormalizeAxis(-190), 170) && Near(NormalizeAxis(180), 180), "NormalizeAxis wraps to (-180, 180]");
	CHECK(Near(ClampAxis(-10), 350) && Near(ClampAxis(370), 10), "ClampAxis wraps to [0, 360)");
	CHECK(Near(ClampAngle(45, 0, 90), 45), "inside a 0..90 range: unchanged");
	CHECK(Near(ClampAngle(120, 0, 90), 90) && Near(ClampAngle(-30, 0, 90), 0), "outside 0..90: the nearer end");
	CHECK(Near(ClampAngle(-200, 0, 90), 90) || Near(ClampAngle(-200, 0, 90), 0), "far round: an end");
	CHECK(Near(ClampAngle(-50, -45, 45), -45) && Near(ClampAngle(170, -45, 45), 45), "a range about 0");
	CHECK(Near(InterpConstantTo(0, 10, 0.1, 50), 5) && Near(InterpConstantTo(9, 10, 0.1, 50), 10), "a constant-speed step, landing on the target");
	CHECK(Near(Atan2AngleZ(V3(0, 1, 0)), 90) && Near(Atan2AngleZ(V3(1, 0, 0), 30), -30), "the angle about +Z");
	const V3 u = UnturnZ(At(1, 30), 30);
	CHECK(Near(u.x, 1) && Near(u.y, 0), "a point brought back into a frame turned 30 degrees");
	CHECK(Near(GridSnap(44, 45), 45) && Near(GridSnap(20, 45), 0), "grid snap");
}

static void TestHingeDoor()
{
	// a door leaf: hinged at the origin, the handle 0.70 m along +X; it opens toward +Y up to 90 degrees
	Hinge h;
	h.limitPositive = 90; h.limitNegative = 0; h.togglePercentage = 0.8; h.returnType = Return::Stay;
	h.Grip(At(0.70, 0));
	Hinge::Events ev = h.TickGrip(At(0.70, 30), kDt);
	CHECK(Near(h.angle, 30, 1e-6) && !ev.toggled, "the hand 30 degrees round: the leaf at 30 (%.3f)", h.angle);
	ev = h.TickGrip(At(0.70, 75), kDt);
	CHECK(Near(h.angle, 75, 1e-6) && ev.toggled && ev.newState && ev.positive, "past 72 degrees (0.8 of 90): the toggle, positive");
	ev = h.TickGrip(At(0.70, 75), kDt);
	CHECK(!ev.toggled, "the toggle once only");
	h.TickGrip(At(0.70, 120), kDt);
	CHECK(Near(h.angle, 90), "past its stop: held at 90 (%.3f)", h.angle);
	h.TickGrip(At(0.70, -100), kDt);
	CHECK(Near(h.angle, 90), "the hand swung round to the far side: the leaf does not flip (%.3f)", h.angle);
	h.TickGrip(At(0.70, 40), kDt);
	CHECK(Near(h.angle, 40) && !h.state, "back to 40: untoggled (%.3f)", h.angle);
	h.TickGrip(At(0.70, -20), kDt);
	CHECK(Near(h.angle, 0), "into the frame: held at 0 (%.3f)", h.angle);
	// the grip point need not be on the handle's line: the angle is the hand's turn since the grip
	Hinge g;
	g.limitPositive = 90;
	g.angle = 20;
	g.Grip(At(0.5, 25));
	g.TickGrip(At(0.5, 35), kDt);
	CHECK(Near(g.angle, 30), "taken at 20 degrees, the hand turned 10 more: 30 (%.3f)", g.angle);
	// height does not matter
	g.TickGrip(At(0.5, 45, 0.6), kDt);
	CHECK(Near(g.angle, 40), "the hand higher up: the turn about the hinge only (%.3f)", g.angle);
	// breaking away
	Hinge b;
	b.breakDistance = 0.30;
	b.Grip(At(0.70, 0));
	Hinge::Events e2 = b.TickGrip(At(0.70, 10), kDt);
	CHECK(!e2.drop, "on the handle: held");
	e2 = b.TickGrip(At(0.70, 10) + V3(0, 0, 0.35), kDt);
	CHECK(e2.drop, "the hand 35 cm off its hold point: let go");
}

static void TestHingeReturn()
{
	Hinge h;
	h.returnType = Return::ReturnToZero;
	h.returnSpeed = 50;
	h.Grip(At(0.6, 0));
	h.TickGrip(At(0.6, 40), kDt);
	h.Release();
	CHECK(h.lerping, "let go: it goes back");
	int tics = 0;
	Hinge::Events ev;
	while (h.lerping && tics < 200) { ev = h.Tick(kDt); tics++; }
	CHECK(Near(h.angle, 0) && ev.finishedLerp, "back at 0, finished (%.3f)", h.angle);
	CHECK(tics == 28, "40 degrees at 50 a second: 0.8 s, 28 tics (%d)", tics);

	Hinge m;
	m.returnType = Return::LerpToMax;
	m.Grip(At(0.6, 0));
	m.TickGrip(At(0.6, 50), kDt);
	m.Release();
	for (int i = 0; i < 200 && m.lerping; i++) m.Tick(kDt);
	CHECK(Near(m.angle, 90), "LerpToMax: on to 90 (%.3f)", m.angle);

	Hinge t;
	t.returnType = Return::LerpToMaxIfOverThreshold;
	t.Grip(At(0.6, 0));
	t.TickGrip(At(0.6, 50), kDt);
	t.Release();
	for (int i = 0; i < 200 && t.lerping; i++) t.Tick(kDt);
	CHECK(Near(t.angle, 0), "LerpToMaxIfOverThreshold under 72: back to 0 (%.3f)", t.angle);

	// momentum: flung at about 5 degrees a tic (175 a second) and let go
	Hinge r;
	r.returnType = Return::RetainMomentum;
	r.Grip(At(0.6, 0));
	for (int i = 1; i <= 6; i++) r.TickGrip(At(0.6, 5.0 * i), kDt);
	const double flung = r.momentum;
	CHECK(flung > 100 && flung <= 180, "the momentum it was flung with (%.1f deg/s)", flung);
	r.Release();
	double prev = r.angle;
	bool rising = true;
	for (int i = 0; i < 400 && r.lerping; i++) { r.Tick(kDt); if (r.angle < prev - 1e-9) rising = false; prev = r.angle; }
	CHECK(rising && r.angle > 30 && r.angle <= 90, "it carries on the way it was going and slows (%.2f)", r.angle);
	CHECK(!r.lerping, "and stops");
}

static void TestLeverDown()
{
	// a lever thrown DOWN: its frame's +Z along the wall, the handle up (+X), thrown toward +Y; the stop at 60
	Hinge h;
	h.limitPositive = 60; h.limitNegative = 0; h.togglePercentage = 1.0; h.returnType = Return::ReturnToZero;
	const double r = 0.12;
	h.Grip(At(r, 0));
	// the owner's 8 cm of travel, along the arc: 0.08 / 0.12 rad = 38.2 degrees
	const double need = 0.08 / r * 180.0 / kPi;
	Hinge::Events ev = h.TickGrip(At(r, need - 1), kDt);
	CHECK(!ev.toggled, "short of its stop: not thrown");
	ev = h.TickGrip(At(r, 61), kDt);
	CHECK(ev.toggled && Near(h.angle, 60), "at its stop (1.0 of 60): thrown");
}

static void TestDial()
{
	// a key: turned by the hand's own twist, 90 either way, back on release
	Dial d;
	d.clockwiseMax = 90; d.cclockwiseMax = 90; d.lerpBackOnRelease = true; d.returnSpeed = 180;
	d.Grip(V3(0, 0, 0.02));
	for (int i = 0; i < 6; i++) d.TickGrip(V3(0, 0, 0.02), 5.0);
	CHECK(Near(d.Signed(), 30), "six 5-degree twists: 30 (%.2f)", d.Signed());
	for (int i = 0; i < 20; i++) d.TickGrip(V3(0, 0, 0.02), 5.0);
	CHECK(Near(d.Signed(), 90), "stopped at 90 (%.2f)", d.Signed());
	for (int i = 0; i < 40; i++) d.TickGrip(V3(0, 0, 0.02), -5.0);
	CHECK(Near(d.Signed(), -90), "the other way: stopped at -90 (%.2f)", d.Signed());
	d.Release();
	int tics = 0;
	Dial::Events ev;
	while (d.lerping && tics < 100) { ev = d.Tick(kDt); tics++; }
	CHECK(Near(d.Signed(), 0) && ev.finishedLerp, "let go: back to 0 (%.2f, %d tics)", d.Signed(), tics);

	// a knob turned by the hand's point going round it
	Dial k;
	k.clockwiseMax = 180; k.cclockwiseMax = 180;
	k.Grip(At(0.05, 0));
	for (int a = 10; a <= 100; a += 10) k.TickGrip(At(0.05, a), NAN);
	CHECK(Near(k.Signed(), 100), "the point carried 100 degrees round: 100 (%.2f)", k.Signed());
	k.TickGrip(At(0.05, 100) + V3(0, 0, 2), NAN);
	CHECK(Near(k.Signed(), 100), "moving along the axis turns nothing");

	// snapping
	Dial s;
	s.clockwiseMax = 360; s.cclockwiseMax = 0; s.usesAngleSnap = true; s.snapIncrement = 45; s.snapThreshold = 10;
	s.Grip(V3(0, 0, 0));
	Dial::Events se;
	for (int i = 0; i < 9; i++) { Dial::Events e = s.TickGrip(V3(), 5.0); if (e.hitSnap) se = e; }
	CHECK(se.hitSnap && Near(se.snapAngle, 45), "a 45-degree notch reached (%.1f)", se.snapAngle);
}

static void TestSlider()
{
	Slider s;
	s.minSlide = V3(0, 0, 0); s.maxSlide = V3(0.10, 0, 0);
	s.Grip(V3(0.3, 0.2, 0));
	s.TickGrip(V3(0.35, 0.2, 0), kDt);
	CHECK(Near(s.at.x, 0.05) && Near(s.progress, 0.5), "5 cm of 10: halfway (%.3f)", s.progress);
	s.TickGrip(V3(0.35, 0.5, 0.4), kDt);
	CHECK(Near(s.at.y, 0) && Near(s.at.z, 0), "off the track's line: only along it");
	Slider::Events ev = s.TickGrip(V3(0.6, 0.2, 0), kDt);
	CHECK(Near(s.progress, 1.0) && Near(s.at.x, 0.10) && ev.hitPoint && Near(ev.point, 1.0), "pulled past the end: at the end, its event");
	ev = s.TickGrip(V3(0.1, 0.2, 0), kDt);
	CHECK(Near(s.progress, 0.0) && ev.hitPoint && Near(ev.point, 0.0), "pushed back past the start: the start's event");
	// momentum
	Slider m;
	m.maxSlide = V3(0.30, 0, 0); m.dropBehaviour = SliderDrop::RetainMomentum;
	m.Grip(V3());
	for (int i = 1; i <= 4; i++) m.TickGrip(V3(0.02 * i, 0, 0), kDt);
	m.Release();
	const double before = m.at.x;
	for (int i = 0; i < 400 && m.lerping; i++) m.Tick(kDt);
	CHECK(m.at.x > before + 0.01 && !m.lerping, "let go moving: it slides on and stops (%.3f -> %.3f)", before, m.at.x);
}

static void TestButton()
{
	Button b;
	b.depressDistance = 0.03; b.engageDepth = 0.02; b.type = ButtonType::ToggleReturn;
	double t = 0;
	Button::Events ev = b.Tick(true, 0.0, t, kDt);
	CHECK(!ev.stateChanged, "touched: nothing yet");
	for (int i = 1; i <= 3; i++) { t += kDt; ev = b.Tick(true, -0.01 * i, t, kDt); if (i < 2) CHECK(!ev.stateChanged, "1 cm in: nothing"); if (i == 2) CHECK(ev.stateChanged && ev.newState, "2 cm in: on"); }
	CHECK(Near(b.depth, -0.03), "pushed 3 cm: the plate 3 cm in (%.3f)", b.depth);
	t += kDt; ev = b.Tick(true, -0.05, t, kDt);
	CHECK(!ev.stateChanged && Near(b.depth, -0.03), "further: held at its depth, once per touch");
	bool ended = false;
	for (int i = 0; i < 60; i++) { t += kDt; ev = b.Tick(false, 0.1, t, kDt); if (ev.endInteraction) ended = true; }
	CHECK(Near(b.depth, 0) && ended, "let go: back out (%.3f)", b.depth);
	for (int i = 0; i <= 3; i++) { t += kDt; ev = b.Tick(true, -0.01 * i, t, kDt); }
	CHECK(!b.buttonState, "a second push: off again");

	Button p;
	p.depressDistance = 0.03; p.engageDepth = 0.02; p.type = ButtonType::Press;
	t = 0;
	for (int i = 0; i <= 3; i++) { t += kDt; p.Tick(true, -0.01 * i, t, kDt); }
	CHECK(p.buttonState, "a press button: on while in");
	for (int i = 0; i < 60; i++) { t += kDt; p.Tick(false, 0.1, t, kDt); }
	CHECK(!p.buttonState, "and off once back out");
}

int main()
{
	TestHelpers();
	TestHingeDoor();
	TestHingeReturn();
	TestLeverDown();
	TestDial();
	TestSlider();
	TestButton();
	printf("track_test: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
