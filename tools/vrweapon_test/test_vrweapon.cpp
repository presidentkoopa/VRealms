// tools/vrweapon_test: chapter 2's weapons in VR (src/roth/roth_vrweapon.h), engine-free.
//
// 1. THE BLADE: BladeSweptBoxContact on swept blades (a slash through a body, a slash over it, a tip that crosses a
//    thin body between two tics, the first contact in time); the swing reader (the fists' PunchReader) fed the
//    striking point: a sword swing counts, a slow sweep and a wrist turned in place do not.
// 2. TWO HANDS: SupportLatch, case by case against vk_openxrdevice.cpp:4513-4600 (the band, the cones, modes 0-4, the
//    mode-2 re-arm); and the same latch against a second, literal transcription of that block on random inputs;
//    TwoHandAim against known lines.
// 3. BODY SLOTS: HolsterRig scenarios after rig.zs's press rules (checklist in the header).
// Exit 0 = every check passed.
#include "roth_vrweapon.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <set>

using namespace roth::vrweapon;
using roth::gesture::V3;
using roth::gesture::PunchReader;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

//===========================================================================
// 1. The blade
//===========================================================================
static void TestBlade()
{
	// A body: a 64-unit square round (200, 0), 0..100 high; the blade 70 units, the grip at the origin, z 50.
	const V3 lo(168, -32, 0), hi(232, 32, 100);
	const double L = 300, gr = 3;
	auto dirAt = [](double deg) { return V3(cos(deg * M_PI / 180), sin(deg * M_PI / 180), 0); };
	const V3 g(0, 0, 50);
	// A slash from 40 degrees left to 40 right: the blade crosses the body's middle.
	{
		const BladeContact c = BladeSweptBoxContact(g, g + dirAt(40) * L, g, g + dirAt(-40) * L, gr, lo, hi);
		CHECK(c.hit, "a slash through the body meets it");
		CHECK(c.u > 0.0 && c.u <= 0.5, "the first contact is in the first half of the tic, the blade still sweeping (u %.3f)", c.u);
		CHECK(c.p.x >= lo.x - gr && c.p.x <= hi.x + gr && c.p.y >= lo.y - gr - 1e-9 && c.p.y <= hi.y + gr + 1e-9, "the contact is at the body (%.1f %.1f)", c.p.x, c.p.y);
	}
	// The same slash a metre over the body: nothing.
	{
		const V3 g2(0, 0, 200);
		const BladeContact c = BladeSweptBoxContact(g2, g2 + dirAt(40) * L, g2, g2 + dirAt(-40) * L, gr, lo, hi);
		CHECK(!c.hit, "a slash over the body misses it");
	}
	// Too short to reach: nothing.
	{
		const BladeContact c = BladeSweptBoxContact(g, g + dirAt(40) * 150, g, g + dirAt(-40) * 150, gr, lo, hi);
		CHECK(!c.hit, "a blade that does not reach misses");
	}
	// A thin body (a 4-unit post at (250, 0)) crossed by the tip between two tics: met.
	{
		const V3 plo(248, -2, 0), phi(252, 2, 100);
		const BladeContact c = BladeSweptBoxContact(g, g + dirAt(10) * L, g, g + dirAt(-10) * L, gr, plo, phi);
		CHECK(c.hit, "a thin post crossed between two tics is met");
		CHECK(fabs(c.u - 0.5) <= 0.13, "at the middle of the tic (u %.3f)", c.u);
	}
	// The contact is along the blade, not only at the tip: a blade lying through the body from the start.
	{
		const BladeContact c = BladeSweptBoxContact(g, g + dirAt(0) * L, g, g + dirAt(-5) * L, gr, lo, hi);
		CHECK(c.hit && c.u == 0.0, "a blade already through the body meets it at u 0");
		CHECK(c.s > 0.4 && c.s < 0.9, "where the blade lies in it (s %.2f)", c.s);
	}

	// The swing reader on the striking point (metres, 35 tics a second).
	auto swing = [](double degPerTic, int tics, double bladeM, double handMoveM) {
		PunchReader r;
		int counted = 0;
		for (int t = 0; t < 30 + tics; t++)
		{
			const double a = t < 30 ? 60.0 : 60.0 - degPerTic * (t - 30);
			const double hand = t < 30 ? 0.0 : handMoveM * (t - 30) / tics;
			const V3 grip(0.45, 0.25 - hand, -0.3);
			const V3 dir(cos(a * M_PI / 180), sin(a * M_PI / 180), 0);
			const V3 strike = BladeStrike(grip, dir, bladeM);
			if (r.Feed(true, strike, t, 0.0)) { counted++; r.Landed(t); }
		}
		return counted;
	};
	CHECK(swing(15.0, 8, kBladeM, 0.30) >= 1, "a sword slash (120 degrees in 8 tics) counts");
	CHECK(swing(1.5, 40, kBladeM, 0.05) == 0, "a slow sweep (1.5 degrees a tic) does not");
	CHECK(swing(0.0, 10, kBladeM, 0.0) == 0, "a still blade does not");
	CHECK(swing(12.0, 8, SwungLengthM(kStaff), 0.25) >= 1, "a staff swing (96 degrees in 8 tics) counts");
	CHECK(swing(1.5, 40, SwungLengthM(kStaff), 0.05) == 0, "a slow staff sweep does not");

	// which weapons: the Sword and the Staff are swung; only the Sword's trigger is held back; only the Staff borrows
	// Adam's fists' blow (owner 2026-11-16, Q10); nothing else is swung
	CHECK(IsSwung(kSword) && IsSwung(kStaff) && !IsSwung(kShotgun) && !IsSwung(kShield) && !IsSwung(9), "swung: the Sword and the Staff only");
	CHECK(IsTriggerHeldBack(kSword) && !IsTriggerHeldBack(kStaff), "the trigger: held back for the Sword, the Staff's projectile kept");
	CHECK(SwingBorrowsFists(kStaff) && !SwingBorrowsFists(kSword), "the Staff's blow is Adam's fists', the Sword's its own");
	CHECK(SwungLengthM(kStaff) > SwungLengthM(kSword), "the Staff reaches further than the blade");
}

//===========================================================================
// 2. Two hands
//===========================================================================

// vk_openxrdevice.cpp:4529-4600, transcribed a second time as it stands (floats, its own names), for the random check.
struct EngineLatch { bool engaged = false, rearmRequired = false, gripWas = false; };
static bool EngineStep(EngineLatch &RS, float bx, float by, float bz, float tx, float ty, float tz, bool gripNow,
	float coneEnter, float coneExit, float dmin, float dmax, int vrmode)
{
	const float toOffLen = sqrtf(tx * tx + ty * ty + tz * tz);
	float alongBarrel = -1.0f;
	if (toOffLen > 0.0001f) alongBarrel = (bx * tx + by * ty + bz * tz) / toOffLen;
	const bool bandOk = (toOffLen >= dmin && toOffLen <= dmax);
	const bool inEnterCone = bandOk && alongBarrel >= coneEnter;
	const bool inExitCone = bandOk && alongBarrel >= coneExit;
	const int mode = vrmode < 0 ? 0 : vrmode > 4 ? 4 : vrmode;
	const bool gripEdge = gripNow && !RS.gripWas;
	RS.gripWas = gripNow;
	if (mode == 0) { RS.engaged = false; RS.rearmRequired = false; }
	else if (RS.engaged)
	{
		if ((mode == 1 || mode == 2) && !inExitCone) RS.engaged = false;
		else if (mode == 3 && !gripNow) RS.engaged = false;
		else if (mode == 4 && gripEdge) RS.engaged = false;
		else if (mode == 2 && gripEdge) { RS.engaged = false; RS.rearmRequired = true; }
	}
	else
	{
		if (RS.rearmRequired && !inExitCone) RS.rearmRequired = false;
		const bool mayTake = inEnterCone && !RS.rearmRequired;
		if (mayTake)
		{
			if (mode == 1 || mode == 2) RS.engaged = true;
			else if (mode == 3 && gripNow) RS.engaged = true;
			else if (mode == 4 && gripEdge) RS.engaged = true;
		}
	}
	return RS.engaged;
}

static void TestTwoHands()
{
	SupportSettings st;
	const V3 barrel(1, 0, 0);
	{
		SupportLatch s;
		CHECK(!s.Feed(barrel, V3(0.40, 0.02, 0), false, st), "mode 3: in the cone without the grip, not taken");
		CHECK(s.Feed(barrel, V3(0.40, 0.02, 0), true, st), "mode 3: the grip in the cone takes it");
		CHECK(s.Feed(barrel, V3(0.10, 0.30, 0), true, st), "mode 3: held, the hand anywhere keeps it (only the button lets go)");
		CHECK(!s.Feed(barrel, V3(0.40, 0.02, 0), false, st), "mode 3: letting go of the grip ends it");
	}
	{
		SupportLatch s;
		CHECK(!s.Feed(barrel, V3(0.10, 0.0, 0), true, st), "under the minimum (a reload) is never taken");
		SupportLatch s2;
		CHECK(!s2.Feed(barrel, V3(1.00, 0.0, 0), true, st), "beyond the maximum is never taken");
		SupportLatch s3;
		CHECK(!s3.Feed(barrel, V3(0.40, 0.20, 0), true, st), "outside the enter cone (26 degrees) is not taken");
		SupportLatch s4;
		CHECK(!s4.Feed(barrel, V3(-0.40, 0.0, 0), true, st), "behind the main hand is not taken");
	}
	{
		SupportSettings a = st; a.mode = 1;
		SupportLatch s;
		CHECK(s.Feed(barrel, V3(0.40, 0.02, 0), false, a), "mode 1: the cone alone takes it");
		CHECK(s.Feed(barrel, V3(0.40, 0.15, 0), false, a), "mode 1: 20.6 degrees, inside the exit cone, keeps it");
		CHECK(!s.Feed(barrel, V3(0.40, 0.30, 0), false, a), "mode 1: out of the exit cone lets go");
	}
	{
		SupportSettings a = st; a.mode = 4;
		SupportLatch s;
		CHECK(s.Feed(barrel, V3(0.40, 0, 0), true, a), "mode 4: a press takes it");
		CHECK(s.Feed(barrel, V3(0.40, 0, 0), false, a), "mode 4: released, still held");
		CHECK(!s.Feed(barrel, V3(0.40, 0, 0), true, a), "mode 4: a second press lets go");
	}
	{
		SupportSettings a = st; a.mode = 2;
		SupportLatch s;
		CHECK(s.Feed(barrel, V3(0.40, 0, 0), false, a), "mode 2: the cone takes it");
		CHECK(!s.Feed(barrel, V3(0.40, 0, 0), true, a), "mode 2: the button lets go");
		CHECK(!s.Feed(barrel, V3(0.40, 0, 0), false, a), "mode 2: not retaken in the cone until it has left the exit cone");
		CHECK(!s.Feed(barrel, V3(0.0, 0.40, 0), false, a), "mode 2: out of the cone re-arms");
		CHECK(s.Feed(barrel, V3(0.40, 0, 0), false, a), "mode 2: then the cone takes it again");
	}
	{
		SupportSettings a = st; a.mode = 0;
		SupportLatch s;
		CHECK(!s.Feed(barrel, V3(0.40, 0, 0), true, a), "mode 0: never");
	}
	// Random: the port and the literal transcription, the same answers every tic.
	{
		srand(12345);
		int differ = 0, ticks = 0, engagedTicks = 0;
		for (int run = 0; run < 2000; run++)
		{
			SupportSettings a = st;
			a.mode = rand() % 6 - 1 + (rand() % 3 == 0 ? 0 : 0);
			SupportLatch s;
			EngineLatch e;
			for (int t = 0; t < 60; t++)
			{
				const double yaw = (rand() % 360) * M_PI / 180, pit = ((rand() % 120) - 60) * M_PI / 180;
				const V3 b(cos(pit) * cos(yaw), cos(pit) * sin(yaw), sin(pit));
				const double near = (rand() % 3 == 0);
				const double len = (rand() % 1100) / 1000.0;
				V3 to = near ? b * len + V3((rand() % 21 - 10) / 100.0, (rand() % 21 - 10) / 100.0, (rand() % 21 - 10) / 100.0)
					: V3((rand() % 201 - 100) / 100.0, (rand() % 201 - 100) / 100.0, (rand() % 201 - 100) / 100.0);
				const bool grip = rand() % 4 != 0;
				const bool p = s.Feed(b, to, grip, a);
				const bool q = EngineStep(e, (float)b.x, (float)b.y, (float)b.z, (float)to.x, (float)to.y, (float)to.z, grip,
					(float)a.coneEnter, (float)a.coneExit, (float)a.distMin, (float)a.distMax, a.mode);
				ticks++;
				if (p) engagedTicks++;
				if (p != q) differ++;
			}
		}
		// Doubles against the engine's floats can part only on a cone's or band's very edge.
		CHECK(differ <= ticks / 10000, "the port vs the engine's block: %d of %d ticks differ", differ, ticks);
		CHECK(engagedTicks > ticks / 50, "the random hands take the hold often enough to test it (%d of %d)", engagedTicks, ticks);
		printf("two hands: %d random ticks, %d engaged, %d differ from vk_openxrdevice.cpp's block\n", ticks, engagedTicks, differ);
	}
	// The aim.
	{
		double y = 0, p = 0;
		CHECK(TwoHandAim(V3(0, 0, 0), V3(0.4, 0, 0), y, p) && fabs(y) < 1e-9 && fabs(p) < 1e-9, "straight ahead: yaw 0 pitch 0");
		CHECK(TwoHandAim(V3(0, 0, 0), V3(0.3, 0.3, 0.3 * sqrt(2.0) * tan(20 * M_PI / 180)), y, p) && fabs(y - 45) < 1e-9 && fabs(p - 20) < 1e-9,
			"left 45 and up 20 (%.3f %.3f)", y, p);
		CHECK(TwoHandAim(V3(1, 1, 1), V3(0.7, 1, 0.8), y, p) && fabs(fabs(y) - 180) < 1e-9 && p < 0, "behind and down (%.3f %.3f)", y, p);
		CHECK(!TwoHandAim(V3(0, 0, 0), V3(0.02, 0.02, 0.5), y, p), "hands one above the other: no two-hand aim");
	}
}

//===========================================================================
// 3. Body slots
//===========================================================================
struct Bag { std::set<int> items; };
static bool Has(int item, void *ctx) { return ((Bag *)ctx)->items.count(item) != 0; }

static HolsterEvent Reach(HolsterRig &rig, HolsterWorld &w, int place, int ticsBefore, int gripTics)
{
	const Place &p = kPlaces[place];
	HolsterEvent last;
	for (int t = 0; t < ticsBefore; t++) { const HolsterEvent e = rig.Step(true, p.f, p.l, p.u, false, w); if (e.act != HOLSTER_NONE) last = e; }
	for (int t = 0; t < gripTics; t++)
	{
		const HolsterEvent e = rig.Step(true, p.f, p.l, p.u, true, w);
		if (e.act != HOLSTER_NONE && e.act != HOLSTER_ENTER) return e;
		if (e.act != HOLSTER_NONE) last = e;
	}
	return last;
}
static void Away(HolsterRig &rig, HolsterWorld &w) { for (int t = 0; t < 3; t++) rig.Step(true, 0.4, 0.0, -0.3, false, w); }

static void TestHolsters()
{
	Bag bag;
	HolsterWorld w; w.carried = Has; w.ctx = &bag;
	HolsterRig rig;
	const int RHIP = 1, LSH = 2, RSH = 3, LHIP = 0;
	bag.items = { 9, 29, 38, 28 };

	// the places: each centre is inside its own place only, and picked
	for (int i = 0; i < kHolsterCount; i++)
		CHECK(rig.Pick(kPlaces[i].f, kPlaces[i].l, kPlaces[i].u, w) == i, "place %d's centre picks it", i);
	CHECK(rig.Pick(0.4, 0.0, -0.3, w) == -1, "a hand ahead of the body is in no place");

	// the entry buzz, then the dwell: a press in the first 3 tics waits
	w.held = 9;
	{
		HolsterEvent e = rig.Step(true, kPlaces[RHIP].f, kPlaces[RHIP].l, kPlaces[RHIP].u, false, w);
		CHECK(e.act == HOLSTER_ENTER && e.place == RHIP, "entering the right hip buzzes");
		e = rig.Step(true, kPlaces[RHIP].f, kPlaces[RHIP].l, kPlaces[RHIP].u, true, w);
		CHECK(e.act == HOLSTER_NONE, "a press at once waits for the dwell");
		rig.Step(true, kPlaces[RHIP].f, kPlaces[RHIP].l, kPlaces[RHIP].u, true, w);
		e = rig.Step(true, kPlaces[RHIP].f, kPlaces[RHIP].l, kPlaces[RHIP].u, true, w);
		CHECK(e.act == HOLSTER_STOW && e.place == RHIP && e.item == 9 && e.assigned, "the Colt goes into the empty right hip, which becomes its own");
		w.held = 0;
	}
	Away(rig, w);
	// a pressed and released grip before the dwell: dropped
	{
		HolsterRig r2;
		const Place &p = kPlaces[LHIP];
		HolsterWorld w2 = w; w2.held = 29;
		r2.Step(true, p.f, p.l, p.u, true, w2);
		HolsterEvent e = r2.Step(true, p.f, p.l, p.u, false, w2);
		e = r2.Step(true, p.f, p.l, p.u, false, w2);
		e = r2.Step(true, p.f, p.l, p.u, false, w2);
		CHECK(e.act == HOLSTER_NONE, "a press let go before the dwell does nothing");
	}
	// an empty hand draws the Colt back
	{
		const HolsterEvent e = Reach(rig, w, RHIP, 4, 2);
		CHECK(e.act == HOLSTER_DRAW && e.item == 9 && e.place == RHIP, "an empty hand draws the Colt from its hip");
		w.held = 9;
	}
	Away(rig, w);
	// the Colt at the left shoulder (not its own, empty, unassigned): it goes to its own holster
	{
		const HolsterEvent e = Reach(rig, w, LSH, 4, 2);
		CHECK(e.act == HOLSTER_STOW && e.place == RHIP && e.item == 9 && !e.assigned, "the Colt put at another holster goes home");
		w.held = 0;
	}
	Away(rig, w);
	// an empty hand at an empty holster: refused
	{
		const HolsterEvent e = Reach(rig, w, LSH, 4, 2);
		CHECK(e.act == HOLSTER_REFUSE, "an empty hand at an empty holster is refused");
	}
	Away(rig, w);
	// the Sword into the right shoulder, the Shotgun into the left shoulder
	w.held = 29;
	CHECK(Reach(rig, w, RSH, 4, 2).act == HOLSTER_STOW, "the Sword into the right shoulder");
	w.held = 0; Away(rig, w);
	w.held = 38;
	{
		const HolsterEvent e = Reach(rig, w, RSH, 0, 2);
		CHECK(e.act == HOLSTER_SWAP && e.item == 29, "the Shotgun at the Sword's full holster: quick swap at once, the Sword comes out");
		w.held = 29;
		CHECK(rig.HomeOf(38) == -1, "the Shotgun has no home yet (it stays carried)");
	}
	Away(rig, w);
	// the Sword at the Colt's EMPTY holster when the Colt is not carried: the Sword has a home -> goes home
	bag.items.erase(9);
	{
		const HolsterEvent e = Reach(rig, w, RHIP, 4, 2);
		CHECK(e.act == HOLSTER_STOW && e.place == RSH, "the Sword at the Colt's empty holster goes to its own");
		w.held = 0;
	}
	Away(rig, w);
	// the Shotgun (no home) at the Colt's empty holster: refused (another gun's holster)
	w.held = 38;
	CHECK(Reach(rig, w, RHIP, 4, 2).act == HOLSTER_REFUSE, "a homeless gun at another gun's empty holster is refused");
	Away(rig, w);
	// the Shield never takes a holster; at a full one it is swapped (it stays on the forearm)
	w.held = 28;
	CHECK(Reach(rig, w, LHIP, 4, 2).act == HOLSTER_REFUSE, "the Shield never goes into a holster");
	Away(rig, w);
	{
		const HolsterEvent e = Reach(rig, w, RSH, 0, 2);
		CHECK(e.act == HOLSTER_SWAP && e.item == 29, "the Shield in hand at the Sword's holster: the Sword comes out");
		CHECK(rig.HomeOf(28) == -1, "the Shield is never assigned");
	}
	Away(rig, w);
	// freeing: an empty hand held 1 s in the Colt's empty assigned holster
	w.held = 0;
	{
		const Place &p = kPlaces[RHIP];
		for (int t = 0; t < 4; t++) rig.Step(true, p.f, p.l, p.u, false, w);
		HolsterEvent e;
		int refusedAt = -1, freedAt = -1;
		for (int t = 0; t < 40; t++)
		{
			const HolsterEvent x = rig.Step(true, p.f, p.l, p.u, true, w);
			if (x.act == HOLSTER_REFUSE && refusedAt < 0) refusedAt = t;
			if (x.act == HOLSTER_FREE) { freedAt = t; e = x; }
		}
		CHECK(refusedAt == 0, "the press itself is refused (nothing in it), at once (%d)", refusedAt);
		CHECK(e.act == HOLSTER_FREE && e.item == 9 && freedAt == kFreeTics - 1, "an empty hand held 1 s frees the Colt's empty holster (tic %d)", freedAt);
		CHECK(rig.owner[RHIP] == 0, "freed");
	}
	Away(rig, w);
	// the hand lost: nothing, and the hover forgotten
	CHECK(rig.Step(false, 0, 0, 0, true, w).act == HOLSTER_NONE, "no hand, nothing");
	// rank: an empty hand where the Sword's shoulder and an empty place overlap prefers the full one
	{
		HolsterRig r3;
		r3.owner[RSH] = 29;
		bag.items.insert(29);
		HolsterWorld w3 = w; w3.held = 0;
		const double f = -0.08, l = -0.05, u = -0.10;   // between the shoulders, nearer the left (0.23 from right, 0.23 from left)
		const double dL = sqrt((l - kPlaces[LSH].l) * (l - kPlaces[LSH].l)), dR = sqrt((l - kPlaces[RSH].l) * (l - kPlaces[RSH].l));
		if (dL <= kPlaces[LSH].r && dR <= kPlaces[RSH].r) CHECK(r3.Pick(f, l, u, w3) == RSH, "an empty hand in two places takes the one with a gun");
		(void)f; (void)u;
	}
}

int main()
{
	TestBlade();
	TestTwoHands();
	TestHolsters();
	printf("%d checks, %d failed\n", g_checks, g_fail);
	printf("%s\n", g_fail == 0 ? "ALL VRWEAPON TESTS PASS" : "*** VRWEAPON TESTS FAILED ***");
	return g_fail == 0 ? 0 : 1;
}
