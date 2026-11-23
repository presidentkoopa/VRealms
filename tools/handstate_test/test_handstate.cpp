// tools/handstate_test -- L2 (src/roth/roth_handstate.h) and L1's engine-free rules (src/roth/roth_vrrules.h), with
// scripted hands: every state, every owner, every claim order, the hold and its end, and the reach of D2.
// Engine-free: g++ -std=c++17 -Isrc/roth tools/handstate_test/test_handstate.cpp && ./a.out
#include "roth_handstate.h"
#include "roth_vrrules.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace roth;
using hands::Owner;
using hands::State;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// A scripted set of claims: which owners would take a closing grip, and a log of who was asked, in order.
struct Script : hands::Claims
{
	bool held = false, colt = false, weapon = false, supporting = false, gesture = false;
	int use = 0;
	std::string asked;
	bool Held() override { asked += "H"; return held; }
	bool Colt() override { asked += "C"; return colt; }
	bool Weapon(bool &s) override { asked += "W"; s = supporting; return weapon; }
	bool Gesture() override { asked += "G"; return gesture; }
	int Use() override { asked += "U"; return use; }
};

// One hand's grip over time, the edges made from it as the engine makes them.
struct Driver
{
	hands::Hand h;
	bool closed = false;
	hands::Step Tic(bool grip, hands::Facts f, Script &c)
	{
		f.valid = true;
		f.gripEdge = grip && !closed;
		f.releaseEdge = !grip && closed;
		f.gripClosed = grip;
		closed = grip;
		return h.Tick(f, c);
	}
};

static void TestFreeTargeting()
{
	Driver d; Script c; hands::Facts f;
	d.Tic(false, f, c);
	CHECK(d.h.state == State::Free && d.h.owner == Owner::None, "idle: FREE nobody");
	f.target = true;
	d.Tic(false, f, c);
	CHECK(d.h.state == State::Targeting && d.h.owner == Owner::None, "a touch or a point: TARGETING");
	CHECK(c.asked.empty(), "no claim asked without a closing grip");
	CHECK(d.h.MayAct(Owner::Picker) && d.h.MayAct(Owner::Weapon) && d.h.MayAct(Owner::Gesture) && d.h.Empty(), "nobody's hand: anyone may act, empty for the fists");
	hands::Facts none; none.valid = false;
	d.h.Tick(none, c);
	CHECK(d.h.state == State::Free, "no controller: FREE");
}

static void TestClaimOrder()
{
	// every combination of claims: the first in the order wins, and nobody after it is asked
	const char *order = "HCWGU";
	for (int m = 0; m < 32; m++)
	{
		Driver d; Script c; hands::Facts f;
		c.held = m & 1; c.colt = m & 2; c.weapon = m & 4; c.gesture = m & 8; c.use = (m & 16) ? 1 : 0;
		d.Tic(false, f, c);
		const hands::Step s = d.Tic(true, f, c);
		int first = -1;
		for (int k = 0; k < 5; k++) if (m & (1 << k)) { first = k; break; }
		const Owner want[5] = { Owner::Held, Owner::Colt, Owner::Weapon, Owner::Gesture, Owner::Picker };
		if (first < 0)
		{
			CHECK(s.gripTo == Owner::None && c.asked == "HCWGU", "m=%d nobody takes it, all asked (%s)", m, c.asked.c_str());
			CHECK(d.h.owner == Owner::None, "m=%d a grip on nothing: nobody's", m);
		}
		else
		{
			CHECK(s.gripTo == want[first], "m=%d the grip goes to %s, got %s", m, hands::OwnerName(want[first]), hands::OwnerName(s.gripTo));
			CHECK(c.asked == std::string(order, first + 1), "m=%d asked %s", m, c.asked.c_str());
		}
	}
}

static void TestPickerIsOneTic()
{
	Driver d; Script c; hands::Facts f; c.use = 1;
	d.Tic(false, f, c);
	const hands::Step s = d.Tic(true, f, c);
	CHECK(s.gripTo == Owner::Picker && d.h.state == State::Acting, "the squeeze's use: ACTING, the picker");
	c.asked.clear();
	d.Tic(true, f, c);
	CHECK(d.h.owner == Owner::None && d.h.state == State::Free && c.asked.empty(), "the use is one tic: back to FREE, no claim while held closed");
	CHECK(d.h.Empty(), "after the use the closed hand is a fist again (the punch may run)");
}

static void TestPickupHoldStow()
{
	Driver d; Script c; hands::Facts f; c.use = 1;
	d.Tic(false, f, c);
	d.Tic(true, f, c);                 // the squeeze picks the item up
	d.h.BeginPickup();                 // the engine: the original gave it, the fly-in goes to this hand
	CHECK(d.h.itemHeld && d.h.landing && d.h.state == State::Holding && d.h.owner == Owner::Item, "pickup: HOLDING the item, landing");
	for (int t = 0; t < 14; t++) d.Tic(true, f, c);
	d.h.Landed();
	CHECK(d.h.state == State::Holding && d.h.owner == Owner::Item && !d.h.landing, "landed: HOLDING");
	CHECK(!d.h.MayAct(Owner::Picker) && !d.h.MayAct(Owner::Weapon) && !d.h.MayAct(Owner::Colt) && !d.h.Empty(),
		"a held item: nobody else acts with the hand, no fist");
	const hands::Step s = d.Tic(false, f, c);
	CHECK(s.holdEnded && !d.h.itemHeld && d.h.owner == Owner::None, "let go: the hold ends (stow), the hand is free");
	// let go during the flight: the hold ends at once; the engine stows when it lands
	Driver e; Script c2; c2.use = 1;
	e.Tic(false, f, c2); e.Tic(true, f, c2); e.h.BeginPickup();
	const hands::Step s2 = e.Tic(false, f, c2);
	CHECK(s2.holdEnded && !e.h.landing, "let go in flight: the hold ends");
}

static void TestCarriedItem()
{
	// the game put the item in the hand (the panel, the auto-select): drawn, the grip free
	Driver d; Script c; hands::Facts f; f.itemCarried = true;
	d.Tic(false, f, c);
	CHECK(d.h.state == State::Holding && d.h.owner == Owner::Item && !d.h.itemHeld, "carried: HOLDING (drawn), no hold");
	CHECK(d.h.MayAct(Owner::Picker) && d.h.MayAct(Owner::Weapon), "carried: the hand's grip is still the claims'");
	// a squeeze on a target uses with it; letting go stows nothing (the auto-select: let go between the two squeezes)
	c.use = 1;
	const hands::Step s = d.Tic(true, f, c);
	CHECK(s.gripTo == Owner::Picker && !s.tookHold, "carried + a target: the use");
	const hands::Step r = d.Tic(false, f, c);
	CHECK(!r.holdEnded, "carried, let go after a use: nothing stowed");
	// a squeeze on nothing closes the hand on its item; letting go stows it
	c.use = 0;
	const hands::Step t = d.Tic(true, f, c);
	CHECK(t.tookHold && d.h.itemHeld && t.gripTo == Owner::Item, "carried + nothing to use: the hand closes on it (HOLDING)");
	const hands::Step u = d.Tic(false, f, c);
	CHECK(u.holdEnded, "and letting go stows it");
}

static void TestStanding()
{
	// each standing state takes the hand and every closing grip; the order among them
	struct Row { const char *name; hands::Facts f; State s; Owner o; };
	std::vector<Row> rows;
	hands::Facts f;
	f = hands::Facts(); f.coltAction = true;  rows.push_back({ "the Colt's reload part", f, State::Acting, Owner::Colt });
	f = hands::Facts(); f.gestureHeld = true; rows.push_back({ "a pull / twist held", f, State::Acting, Owner::Gesture });
	f = hands::Facts(); f.supportHeld = true; rows.push_back({ "the fore-end", f, State::Supporting, Owner::Weapon });
	f = hands::Facts(); f.heldObject = true;  rows.push_back({ "the Lamp / the match / the Book", f, State::Holding, Owner::Held });
	for (const Row &r : rows)
	{
		Driver d; Script c; c.use = 1; c.held = c.colt = c.weapon = c.gesture = true;
		d.Tic(false, r.f, c);
		CHECK(d.h.state == r.s && d.h.owner == r.o, "%s: %s %s", r.name, hands::StateName(d.h.state), hands::OwnerName(d.h.owner));
		const hands::Step s = d.Tic(true, r.f, c);
		CHECK(s.gripTo == r.o && c.asked.empty(), "%s: a closing grip is its, nobody else asked", r.name);
		CHECK(!d.h.MayAct(Owner::Picker) && d.h.MayAct(r.o) && !d.h.Empty(), "%s: only it acts", r.name);
	}
	// SUPPORTING GIVES WAY TO A RELOAD ACTION (the lead, 2026-11-23): the off hand on the fore-end moves to the Colt's part
	Driver d; Script c; hands::Facts g; g.supportHeld = true;
	d.Tic(true, g, c);
	CHECK(d.h.state == State::Supporting, "supporting");
	g.coltAction = true;
	d.Tic(true, g, c);
	CHECK(d.h.state == State::Acting && d.h.owner == Owner::Colt && !d.h.MayAct(Owner::Weapon) && d.h.MayAct(Owner::Colt),
		"a reload action: the support gives way to the Colt at once");
	// a held item's twist is a gesture on it: the gesture owns the hand while it lasts, the hold resumes after
	Driver e; Script c2; hands::Facts h0; c2.use = 1;
	e.Tic(false, h0, c2); e.Tic(true, h0, c2); e.h.BeginPickup(); e.h.Landed();
	hands::Facts tw; tw.gestureHeld = true;
	e.Tic(true, tw, c2);
	CHECK(e.h.owner == Owner::Gesture && e.h.itemHeld, "twisting the held item: the gesture's, the item still held");
	e.Tic(true, h0, c2);
	CHECK(e.h.owner == Owner::Item && e.h.state == State::Holding, "the twist over, the grip closed: HOLDING again");
	const hands::Step end = e.Tic(false, tw, c2);
	CHECK(end.holdEnded, "the grip opening ends the hold even on the twist's last tic");
}

static void TestWeaponHand()
{
	Driver d; Script c; hands::Facts f; f.weaponInHand = true;
	d.Tic(false, f, c);
	CHECK(d.h.state == State::Holding && d.h.owner == Owner::Weapon, "a weapon drawn: HOLDING, the weapon");
	CHECK(d.h.MayAct(Owner::Picker) && d.h.MayAct(Owner::Weapon) && !d.h.Empty(), "its grip still uses and holsters; no fist");
	c.weapon = true;   // a body slot
	const hands::Step s = d.Tic(true, f, c);
	CHECK(s.gripTo == Owner::Weapon && d.h.state == State::Acting && c.asked == "HCW", "the grip in a body slot: the weapon's");
	Driver e; Script c2; c2.use = 1;
	e.Tic(false, f, c2);
	const hands::Step u = e.Tic(true, f, c2);
	CHECK(u.gripTo == Owner::Picker, "the weapon hand's grip on a target: a use");
	// the lit match: held for its drawing, its grip still the claims' (the tall wicks' point and grip)
	Driver m; Script cm; hands::Facts fm; fm.toolInHand = true; cm.use = 1;
	m.Tic(false, fm, cm);
	CHECK(m.h.state == State::Holding && m.h.owner == Owner::Held && m.h.MayAct(Owner::Picker) && !m.h.Empty(), "the lit match: HOLDING, held, the grip open");
	const hands::Step sm = m.Tic(true, fm, cm);
	CHECK(sm.gripTo == Owner::Picker, "the match hand's squeeze: the picker (a tall wick)");
	Driver o; Script c3; c3.weapon = true; c3.supporting = true;
	o.Tic(false, hands::Facts(), c3);
	o.Tic(true, hands::Facts(), c3);
	CHECK(o.h.state == State::Supporting && o.h.owner == Owner::Weapon, "the off grip on the fore-end: SUPPORTING");
}

static void TestRules()
{
	// D2: the horizontal distance from the player to the hit point; the bands the original's
	CHECK(fabs(rules::kUseZ - 268) < 1e-9 && fabs(rules::kFarZ - 968) < 1e-9 && fabs(rules::kExamineZ - 2468) < 1e-9, "bands 268 / 968 / 2468");
	CHECK(rules::DepthOfZ(268) == 600 && rules::DepthOfZ(968) == 2000 && rules::DepthOfZ(2468) == 5000, "depth = 2z + 64");
	CHECK(fabs(rules::ReachZ(10, 20, 13, 24) - 5) < 1e-12, "reach is the horizontal distance");
	// straight ahead D2 equals the original's z along the view yaw; to the side the original's would be ~0, D2 is not
	const double yawDeg = 30, t = yawDeg * M_PI / 180;
	for (double d = 0; d < 3000; d += 37)
	{
		const double hx = cos(t) * d, hy = sin(t) * d;
		const double zYaw = hx * cos(t) + hy * sin(t);
		CHECK(fabs(rules::ReachZ(0, 0, hx, hy) - zYaw) < 1e-9, "ahead: D2 == the original's z (d %.0f)", d);
	}
	CHECK(rules::ReachZ(0, 0, 0, 500) == 500, "to the side: D2 500 (the original's yaw measure would give 0, endless reach)");
	// the four classes from the original's cursor
	CHECK(rules::ClassOf(3, 0x248, 599) == rules::Class::Usable, "type 3 in the band: usable");
	CHECK(rules::ClassOf(3, 0x278, 600) == rules::Class::TooFar, "type 3 beyond the band: too far");
	CHECK(rules::ClassOf(1, 0x398, 900) == rules::Class::TooFar && rules::ClassOf(3, 0x3a0, 900) == rules::Class::TooFar, "0x398 / 0x3a0: too far");
	CHECK(rules::ClassOf(5, 0x258, 100) == rules::Class::Blocked && rules::ClassOf(5, 0x260, 100) == rules::Class::Blocked, "type 5: blocked");
	CHECK(rules::ClassOf(1, 0x240, 100) == rules::Class::Nothing && rules::ClassOf(0, 0x240, 100) == rules::Class::Nothing, "nothing");
	// the original's gate, through roth_cursor.h, agrees with Usable exactly
	for (int type = 0; type <= 5; type++)
		for (double depth = 0; depth < 6000; depth += 13)
		{
			CursorResult r; r.type = (uint8_t)type;
			const bool gate = CursorAllowsUse(r, depth, rules::kUseDepth);
			CHECK(gate == (rules::ClassOf((uint8_t)type, 0x248, depth) == rules::Class::Usable), "type %d depth %.0f: usable iff the original's use gate", type, depth);
		}
}

int main()
{
	TestFreeTargeting();
	TestClaimOrder();
	TestPickerIsOneTic();
	TestPickupHoldStow();
	TestCarriedItem();
	TestStanding();
	TestWeaponHand();
	TestRules();
	printf("handstate_test: %d checks passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
