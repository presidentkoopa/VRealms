// tools/worldhands_test: chapter 2's world-by-hand rules (src/roth/roth_worldhandslogic.h), engine-free.
// Scripted hand paths with the outcome each must give: the release (one release = one use, never two), the
// pull (8 cm / 4 cm, never into the thing, once per grip), the press (2 cm, armed from in front), the twist
// (180 / 30 degrees, either way, unwrapped past 180, reset on leaving the socket), the sit, the drink, the
// contact disc's offsets and the angle helpers. Exit 0 = every check passed.
#include "roth_worldhandslogic.h"
#include <stdio.h>
#include <math.h>
#include <set>

using namespace roth::wh;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void TestDisc()
{
	const auto d = DiscOffsets(kPalmDiscM);
	CHECK(!d.empty() && d[0].first == 0 && d[0].second == 0, "the disc starts at its centre");
	double last = 0;
	bool ordered = true, inside = true;
	for (const auto &o : d)
	{
		const double r = sqrt(o.first * o.first + o.second * o.second);
		if (r + 1e-12 < last) ordered = false;
		if (r > kPalmDiscM + 1e-9) inside = false;
		last = r;
	}
	CHECK(ordered, "the disc's points go outward ring by ring");
	CHECK(inside, "every point within the radius");
	// coverage: every point of the disc is within one step of a listed point
	double worst = 0;
	for (double x = -kPalmDiscM; x <= kPalmDiscM; x += 0.001)
		for (double y = -kPalmDiscM; y <= kPalmDiscM; y += 0.001)
		{
			if (x * x + y * y > kPalmDiscM * kPalmDiscM) continue;
			double best = 1e9;
			for (const auto &o : d) best = std::min(best, hypot(x - o.first, y - o.second));
			worst = std::max(worst, best);
		}
	CHECK(worst <= kDiscStepM, "the disc covers its area within one step (worst %.4f m)", worst);
	CHECK(DiscOffsets(0).size() == 1, "radius 0: the point only");
	const auto t = DiscOffsets(kTipDiscM);
	CHECK(t.size() > 1 && t.size() < d.size(), "the fingertip's disc is smaller than the palm's (%d, %d)", (int)t.size(), (int)d.size());
	// a 4 x 3 texel box beside the point (STUDY1 #57's size, at 2 cm a texel [R]) is found by a palm overlapping it
	const double bx0 = 0.030, bx1 = 0.038, by0 = -0.003, by1 = 0.003;
	int first = -1;
	for (size_t k = 0; k < d.size(); k++)
		if (d[k].first >= bx0 && d[k].first <= bx1 && d[k].second >= by0 && d[k].second <= by1) { first = (int)k; break; }
	CHECK(first > 0, "a tiny box 3 cm off the palm's centre is met by its disc");
}

static void TestAngles()
{
	const V3 fwd(1, 0, 0), up(0, 0, 1);
	for (int deg = -170; deg <= 170; deg += 10)
	{
		const double a = deg * kPi / 180.0;
		const V3 r(0, -sin(a), cos(a));   // up turned about +x by deg (right-handed)
		CHECK(fabs(SignedAngleAbout(up, r, fwd) - deg) < 1e-6, "signed angle %d -> %.3f", deg, SignedAngleAbout(up, r, fwd));
	}
	CHECK(fabs(SignedAngleAbout(up, up * 3.0 + fwd * 5.0, fwd)) < 1e-9, "the axis component is ignored");
	CHECK(fabs(TiltDeg(up)) < 1e-9 && fabs(TiltDeg(V3(0, 1, 0)) - 90) < 1e-9 && fabs(TiltDeg(V3(0, 0, -1)) - 180) < 1e-9, "tilt");
}

static void TestRelease()
{
	ReleaseEdge r;
	int fires = 0;
	// carried closed for 30 tics, opened, held open 30 tics
	for (int t = 0; t < 30; t++) fires += r.Feed(true, true);
	CHECK(fires == 0, "no use while carried");
	fires += r.Feed(true, false);
	CHECK(fires == 1, "the release is one use");
	for (int t = 0; t < 30; t++) fires += r.Feed(true, false);
	CHECK(fires == 1, "an open hand makes no more (never two)");
	// open from the start: nothing
	ReleaseEdge r2;
	int f2 = 0;
	for (int t = 0; t < 10; t++) f2 += r2.Feed(true, false);
	CHECK(f2 == 0, "never closed: no release");
	// closed, lost (no hand), back open: no release (the closing was not seen while live)
	ReleaseEdge r3;
	int f3 = 0;
	f3 += r3.Feed(true, true); f3 += r3.Feed(false, true); f3 += r3.Feed(true, false);
	CHECK(f3 == 0, "a hand lost between closed and open is no release");
	// close / open twice: two releases (two of the original's events, as with a mouse)
	ReleaseEdge r4;
	int f4 = 0;
	const bool seq[] = { true, true, false, false, true, false };
	for (bool s : seq) f4 += r4.Feed(true, s);
	CHECK(f4 == 2, "two releases are two uses (%d)", f4);
}

static void TestPull()
{
	// a lever on a wall to the +x side: into = +x
	Pull p;
	p.Engage(V3(0, 0, 1), V3(1, 0, 0), kLeverPullM);
	int fires = 0;
	// pushed INTO the wall 20 cm: not a pull
	for (int t = 1; t <= 20; t++) fires += p.Feed(true, V3(0.01 * t, 0, 1));
	CHECK(fires == 0, "into the thing is no pull (%d)", fires);
	Pull q;
	q.Engage(V3(0, 0, 1), V3(1, 0, 0), kLeverPullM);
	int qf = 0, at = -1;
	for (int t = 1; t <= 20; t++) { if (q.Feed(true, V3(0, 0, 1 - 0.005 * t))) { qf++; if (at < 0) at = t; } }
	CHECK(qf == 1 && at == 16, "pulled down 8 cm: one use at 8 cm (%d at tic %d)", qf, at);
	// back and forth: still once per grip
	for (int t = 0; t < 20; t++) qf += q.Feed(true, V3(0, 0, t % 2 ? 1.0 : 0.85));
	CHECK(qf == 1, "once per grip (%d)", qf);
	// open the grip: ended; re-engaged it fires again
	q.Feed(false, V3(0, 0, 1));
	CHECK(!q.engaged, "the grip opening ends it");
	q.Engage(V3(0, 0, 1), V3(1, 0, 0), kLeverPullM);
	int qq = 0;
	for (int t = 1; t <= 20; t++) qq += q.Feed(true, V3(-0.01 * t, 0, 1));
	CHECK(qq == 1, "a fresh grip pulled out: one use");
	// the leaf: 4 cm out
	Pull l;
	l.Engage(V3(0, 0, 0), V3(0, 1, 0), kLeafPullM);
	int lf = 0, lat = -1;
	for (int t = 1; t <= 10; t++) if (l.Feed(true, V3(0, -0.01 * t, 0))) { lf++; lat = t; }
	CHECK(lf == 1 && lat == 4, "the leaf pulled 4 cm: one use (%d at %d)", lf, lat);
	// released half-way: nothing
	Pull h;
	h.Engage(V3(0, 0, 0), V3(0, 1, 0), kLeafPullM);
	int hf = h.Feed(true, V3(0, -0.02, 0)) + h.Feed(false, V3(0, -0.06, 0)) + h.Feed(true, V3(0, -0.08, 0));
	CHECK(hf == 0, "let go half-way: nothing");
}

static void TestPress()
{
	Press p;
	int fires = 0;
	// palm in front 5 cm, in to 3 cm past, out, in again
	double path[] = { -0.05, -0.03, -0.01, 0.0, 0.01, 0.02, 0.03, 0.03, 0.0, -0.005, 0.03, -0.02, 0.0, 0.025 };
	int at[16], n = 0;
	for (int i = 0; i < (int)(sizeof(path) / sizeof(path[0])); i++) if (p.Feed(true, true, path[i])) { fires++; at[n++] = i; }
	CHECK(fires == 2 && at[0] == 5 && at[1] == 13, "two presses, at 2 cm each, the second only after backing off 1 cm (%d)", fires);
	// a palm that starts inside the wall (came round it) is not armed
	Press q;
	int qf = 0;
	for (double d : { 0.03, 0.04, 0.05 }) qf += q.Feed(true, true, d);
	CHECK(qf == 0, "not armed from inside");
	// not over the print
	Press r;
	int rf = r.Feed(true, false, -0.05) + r.Feed(true, false, 0.03);
	CHECK(rf == 0, "beside the print: nothing");
	// a fast press crossing in one tic
	Press s;
	int sf = s.Feed(true, true, -0.04) + s.Feed(true, true, 0.04);
	CHECK(sf == 1, "a fast press crossing in one tic counts");
}

static V3 RollUp(double deg)
{
	const double a = deg * kPi / 180.0;
	return V3(0, -sin(a), cos(a));
}

static void TestTwist()
{
	const V3 axis(1, 0, 0);
	// the Shrive: 180 degrees in 10-degree steps
	Twist t;
	t.Engage(RollUp(0));
	int fires = 0, at = -1;
	for (int k = 1; k <= 30; k++) if (t.Feed(true, RollUp(10.0 * k), axis, kShriveTwistDeg)) { fires++; at = k; }
	CHECK(fires == 1 && at == 18, "the Shrive: one use at 180 degrees (%d at %d), past 180 unwrapped", fires, at);
	// the other way
	Twist u;
	u.Engage(RollUp(0));
	int uf = 0;
	for (int k = 1; k <= 20; k++) uf += u.Feed(true, RollUp(-10.0 * k), axis, kShriveTwistDeg);
	CHECK(uf == 1, "either way");
	// back and forth 90 never adds to 180
	Twist w;
	w.Engage(RollUp(0));
	int wf = 0;
	for (int k = 0; k < 40; k++) wf += w.Feed(true, RollUp((k % 2) ? 90.0 : 0.0), axis, kShriveTwistDeg);
	CHECK(wf == 0, "wiggling 90 degrees back and forth is not a twist");
	// the key: 30 degrees; out of the socket at 20: nothing, and a fresh set needs the whole 30 again
	Twist k1;
	k1.Engage(RollUp(0));
	int kf = k1.Feed(true, RollUp(10), axis, kKeyTurnDeg) + k1.Feed(true, RollUp(20), axis, kKeyTurnDeg) + k1.Feed(false, RollUp(25), axis, kKeyTurnDeg);
	CHECK(kf == 0 && !k1.engaged, "out of the lock at 20 degrees: nothing");
	k1.Engage(RollUp(25));
	kf += k1.Feed(true, RollUp(45), axis, kKeyTurnDeg) + k1.Feed(true, RollUp(56), axis, kKeyTurnDeg);
	CHECK(kf == 1, "set again, turned 31 degrees: one use (%d)", kf);
	// the hand's own axis turning while the roll stays: not a twist (the forward axis is the reference)
	Twist p;
	p.Engage(V3(0, 0, 1));
	int pf = 0;
	for (int k = 1; k <= 20; k++) pf += p.Feed(true, V3(0, 0, 1), V3(cos(k * 0.1), sin(k * 0.1), 0), kKeyTurnDeg);
	CHECK(pf == 0, "yawing the hand is not a turn");
}

static void TestSit()
{
	Sit s;
	int f = 0;
	for (double d = 0; d <= 0.5; d += 0.02) f += s.Feed(d, true, true);
	CHECK(f == 1, "sat down once (%d)", f);
	for (int k = 0; k < 20; k++) f += s.Feed(0.45, true, true);
	CHECK(f == 1, "staying seated: nothing more");
	f += s.Feed(0.25, true, true) + s.Feed(0.40, true, true);
	CHECK(f == 1, "half up and down again: nothing (not stood up)");
	f += s.Feed(0.10, true, true) + s.Feed(0.40, true, true);
	CHECK(f == 2, "stood up and sat again: the second use (%d)", f);
	Sit a;
	int af = 0;
	for (double d = 0; d <= 0.5; d += 0.02) af += a.Feed(d, false, true) + 0 * a.Feed(d, true, false);
	CHECK(af == 0, "crouching away from the seat, or facing its back: nothing");
	CHECK(FacingAway(1, 0, 1, 0.5) && !FacingAway(-1, 0, 1, 0) && !FacingAway(0, 1, 1, 0), "facing away from the back");
}

static void TestDrink()
{
	Drink d;
	int f = 0, at = -1;
	// raised to the mouth upright, then tipped 10 degrees a tic
	for (int t = 0; t < 5; t++) f += d.Feed(true, 0.05, 0);
	for (int t = 1; t <= 15; t++) if (d.Feed(true, 0.05, 10.0 * t)) { f++; at = t; }
	CHECK(f == 1 && at == 11, "tipped to 60 and held 6 tics: one drink (%d at %d)", f, at);
	for (int t = 0; t < 30; t++) f += d.Feed(true, 0.05, 90);
	CHECK(f == 1, "kept tipped: no second drink");
	f += d.Feed(true, 0.05, 10);
	for (int t = 0; t < 7; t++) f += d.Feed(true, 0.05, 70);
	CHECK(f == 2, "upright and tipped again: the second (%d)", f);
	Drink e;
	int ef = 0;
	for (int t = 0; t < 20; t++) ef += e.Feed(true, 0.20, 90);
	CHECK(ef == 0, "tipped away from the mouth: nothing");
	for (int t = 0; t < 20; t++) ef += e.Feed(false, 0.02, 90);
	CHECK(ef == 0, "no vial: nothing");
	Drink g;
	int gf = 0;
	for (int t = 0; t < 20; t++) gf += g.Feed(true, 0.05, (t % 2) ? 70 : 50);
	CHECK(gf == 0, "flickering about 60: never held 6 tics");
	CHECK(Drink::Tipping(true, 0.05, 40) && !Drink::Tipping(true, 0.05, 10) && !Drink::Tipping(true, 0.3, 80), "tipping");
}

int main()
{
	TestDisc();
	TestAngles();
	TestRelease();
	TestPull();
	TestPress();
	TestTwist();
	TestSit();
	TestDrink();
	printf("worldhands_test: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
