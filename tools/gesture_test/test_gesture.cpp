// tools/gesture_test: the chapter-1 gesture detectors (src/roth/roth_gesture.h), engine-free.
//
// 1. THE PUNCH against RSQ Melee's own reader: RsqReader below is gloves.zs:494-590 (Read + Swing, the parts the
//    port takes) transcribed line by line from /home/claude/presidentkoopa/RSQ_UBQSeries/Melee/zscript/rsqml/
//    gloves.zs, in RSQ's own MAP UNITS (vr_vunits_per_meter 34, pixelstretch 1.2), with the parts the port
//    leaves out (the open palm, the Hadouken, Dempsey) at their off settings. Both readers get the same random
//    hand paths (rest, jabs, slow pushes, snap turns, one-tic jumps, frame gaps, long swings, landed blows); every
//    tic's answer ("this tic the swing counts") and its average speed must agree. Then the named cases.
// 2. The contact (SweptContact) and the pulled-back rule (DrivesInto) against gloves.zs:710-731 / :622-627 cases.
// 3. The knock, the attempts (push, press, tug, wave), the ward tingle: scripted hand paths with the expected
//    outcome of each.
// Exit 0 = every check passed.
#include "roth_gesture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>

using namespace roth::gesture;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

//===========================================================================
// RSQ Melee's reader, transcribed (gloves.zs). Map units; TICRATE 35.
//===========================================================================
struct RsqHand
{
	bool armed = false, swinging = false;
	double approach = 0;
	int approachTics = 0;
	int lastHit = -1000000;   // RSQ: 0 at the start; -1000000 here so both start alike (the port's default)
	V3 loc, vel, path;
	bool prevValid = false;
	int prevTic = 0;
	double prevYaw = 0;
};

struct RsqReader
{
	static constexpr double UPM = 34.0, PS = 1.2;
	static constexpr double ARM_M = 0.15, IN_MPS = 0.25;
	static constexpr int MAX_TICS = 14;
	static constexpr double JUMP_UNITS = 48.0;
	static constexpr int MAX_GAP = 3;
	static constexpr double SNAP_DEG = 15.0;
	static constexpr double minSpeed = 2.0;
	static constexpr int rehit = 6;
	RsqHand st;
	double avg = 0;

	static double DeltaAngle(double a, double b) { double d = fmod(b - a, 360.0); if (d > 180) d -= 360; if (d < -180) d += 360; return d; }

	// Read (gloves.zs:494-524) then Swing (:526-590); `hp` the hand, `body` the body (map units). Returns true
	// at :590 (swinging = true; the contact is then looked for).
	bool Read(bool ok, V3 hp, V3 body, int now, double angle)
	{
		if (!ok) { st.prevValid = false; return false; }                                   // :500
		V3 loc = hp - body;                                                                // :505
		int dt = now - st.prevTic;                                                         // :506
		bool had = st.prevValid;
		V3 prev = st.loc;
		double yawStep = fabs(DeltaAngle(st.prevYaw, angle));                              // :509
		st.loc = loc;
		st.prevYaw = angle;
		st.prevTic = now;
		st.prevValid = true;
		st.vel = V3(0, 0, 0);                                                              // :516
		if (!had || dt < 1 || dt > MAX_GAP) return false;                                  // :517
		if (yawStep > SNAP_DEG * dt) return false;                                         // :518
		V3 d = loc - prev;
		if (d.Length() > JUMP_UNITS * dt) return false;                                    // :520
		st.vel = d * (35.0 / dt);                                                          // :521
		return Swing(now);
	}
	bool Swing(int now)
	{
		V3 v = st.vel;
		v.z *= PS;                                                                         // :540
		double mps = v.Length() / UPM;                                                     // :541
		if (!st.armed)                                                                     // :567-571
		{
			if (now - st.lastHit >= rehit && mps < minSpeed * 0.6) st.armed = true;
			return false;
		}
		if (mps >= IN_MPS && st.approachTics < MAX_TICS)                                   // :572-577
		{
			st.approach += mps / 35.0;
			st.approachTics++;
			st.path = st.path + v * (1.0 / (UPM * 35.0));
		}
		else                                                                               // :578-586
		{
			st.swinging = false;
			st.approach = 0;
			st.approachTics = 0;
			st.path = V3(0, 0, 0);
			return false;
		}
		if (st.approach < ARM_M) return false;                                             // :587
		avg = st.approach * 35.0 / st.approachTics;                                        // :588
		if (avg < minSpeed) return false;                                                  // :589
		st.swinging = true;                                                                // :590
		return true;
	}
	void Landed(int now)                                                                   // :629-634, :613-618
	{
		st.armed = false; st.swinging = false; st.approach = 0; st.approachTics = 0; st.path = V3(0, 0, 0); st.lastHit = now;
	}
};

static unsigned g_seed = 12345;
static double Rnd() { g_seed = g_seed * 1103515245u + 12345u; return ((g_seed >> 8) & 0xffffff) / (double)0x1000000; }

static void PunchDifferential()
{
	int ticsCompared = 0, counted = 0, bad = 0, paths = 0;
	for (int run = 0; run < 400; run++)
	{
		RsqReader rsq;
		PunchReader port;
		V3 hand(20, 0, 0), body(0, 0, 41);   // map units
		double yaw = Rnd() * 360.0;
		int now = 1;
		paths++;
		for (int seg = 0; seg < 30; seg++)
		{
			// one motion segment: rest, a jab, a slow push, a snap turn, a jump, a gap, a long swing, a curl back
			const int kind = (int)(Rnd() * 9);
			int len = 1 + (int)(Rnd() * 8);
			V3 dir(Rnd() - 0.5, Rnd() - 0.5, (Rnd() - 0.5) * 0.8);
			const double dl = dir.Length();
			dir = dl > 1e-6 ? dir * (1.0 / dl) : V3(1, 0, 0);
			double speedMps = 0;
			switch (kind)
			{
			case 0: speedMps = 0; break;                         // rest
			case 1: speedMps = 2.5 + Rnd() * 5.0; break;         // a jab
			case 2: speedMps = 0.3 + Rnd() * 1.6; break;         // a slow push
			case 3: speedMps = 1.8 + Rnd() * 0.5; break;         // near the line
			case 4: speedMps = 0.1; break;                       // creeping
			case 5: speedMps = 4.0; len = 16 + (int)(Rnd() * 6); break;   // longer than MAX_TICS
			default: speedMps = Rnd() * 6.0; break;
			}
			for (int k = 0; k < len; k++)
			{
				bool ok = true;
				int step = 1;
				const double r = Rnd();
				if (r < 0.03) step = 2 + (int)(Rnd() * 4);       // a gap (MAX_GAP 3 and past it)
				if (r > 0.97) ok = false;                         // no hand this tic
				double dyaw = (Rnd() < 0.03) ? 20.0 + Rnd() * 40.0 : (Rnd() - 0.5) * 4.0;   // a snap turn sometimes
				yaw += dyaw;
				// the motion in REAL metres, then into map units: the vertical / pixelstretch (the engine's transform)
				V3 dm = dir * (speedMps * step / 35.0);
				if (Rnd() < 0.01) dm = dm + V3(1.6, 0, 0);       // a one-tic jump no arm makes
				hand = hand + V3(dm.x * 34.0, dm.y * 34.0, dm.z * 34.0 / 1.2);
				// the body walks a little (its offset cancels: both read the hand against it)
				body = body + V3((Rnd() - 0.5) * 2, (Rnd() - 0.5) * 2, 0);
				hand = hand + V3(0, 0, 0);
				now += step;
				const V3 rel = hand - body;
				const V3 locM(rel.x / 34.0, rel.y / 34.0, rel.z * 1.2 / 34.0);
				const bool a = rsq.Read(ok, hand, body, now, yaw);
				const bool b = port.Feed(ok, locM, now, yaw, 1.2);
				ticsCompared++;
				if (a) counted++;
				if (a != b || (a && fabs(rsq.avg - port.avgMps) > 1e-9))
				{
					if (bad++ < 8) printf("PUNCH DIFFER run %d tic %d: RSQ %d (avg %.6f) port %d (avg %.6f)\n", run, now, a, rsq.avg, b, port.avgMps);
				}
				if (a && b && Rnd() < 0.5) { rsq.Landed(now); port.Landed(now); }   // a blow landed (or a switch worked)
				// the body moved after the read: keep the hand with it (as a hand hangs off the body)
			}
		}
	}
	printf("punch reader vs RSQ Melee's (gloves.zs:494-590): %d paths, %d tics compared, %d counted as a punch, %d differ\n", paths, ticsCompared, counted, bad);
	CHECK(bad == 0, "the port's reader differs from RSQ's on %d tics", bad);
	CHECK(counted > 100, "too few punches in the random paths (%d)", counted);
}

// Named cases (each read alone).
static void PunchCases()
{
	auto run = [](PunchReader &p, double mps, int tics, int &now, V3 &at, V3 dir, double yaw = 0) {
		int hits = 0;
		for (int k = 0; k < tics; k++) { at = at + dir * (mps / 35.0); now++; if (p.Feed(true, at, now, yaw)) hits++; }
		return hits;
	};
	{
		PunchReader p; int now = 0; V3 at(0.3, 0, -0.3);
		run(p, 0, 8, now, at, V3(1, 0, 0));
		CHECK(p.armed, "a still fist arms");
		int first = -1;
		for (int k = 0; k < 4; k++) { at = at + V3(4.7 / 35.0, 0, 0); now++; if (p.Feed(true, at, now, 0) && first < 0) first = k; }
		CHECK(first == 1, "a 4.7 m/s jab counts on its second moving tic (0.13 m, then 0.27 >= 0.15): got %d", first);
		CHECK(fabs(p.avgMps - 4.7) < 1e-6, "the jab's average is its speed: %.4f", p.avgMps);
	}
	{
		PunchReader p; int now = 0; V3 at;
		run(p, 0, 8, now, at, V3(1, 0, 0));
		CHECK(run(p, 1.5, 10, now, at, V3(1, 0, 0)) == 0, "a 1.5 m/s push never counts (below 2.0)");
	}
	{
		PunchReader p; int now = 0; V3 at;
		run(p, 0, 8, now, at, V3(1, 0, 0));
		CHECK(run(p, 3.0, 3, now, at, V3(1, 0, 0)) > 0, "a 3 m/s swing counts");
		p.Landed(now);
		CHECK(run(p, 3.0, 3, now, at, V3(1, 0, 0)) == 0, "no second blow before re-arming");
		run(p, 0, 2, now, at, V3(1, 0, 0));
		CHECK(!p.armed, "not re-armed 5 tics after the blow (rehit 6)");
		run(p, 0, 1, now, at, V3(1, 0, 0));
		CHECK(p.armed, "re-armed 6 tics after the blow, the hand slow");
	}
	{
		PunchReader p; int now = 0; V3 at;
		run(p, 0, 8, now, at, V3(1, 0, 0));
		at = at + V3(0.15, 0, 0); now++;
		CHECK(!p.Feed(true, at, now, 30.0), "a snap turn (30 degrees in a tic) gives no velocity");
		CHECK(p.mps == 0, "the snap tic's speed is 0");
	}
	{
		PunchReader p; int now = 0; V3 at;
		run(p, 0, 8, now, at, V3(1, 0, 0));
		at = at + V3(1.6, 0, 0); now++;
		p.Feed(true, at, now, 0);
		CHECK(p.mps == 0, "a one-tic jump of 1.6 m is dropped (JUMP_UNITS 48 at 34 per metre)");
	}
	{
		PunchReader p; int now = 0; V3 at;
		run(p, 0, 8, now, at, V3(1, 0, 0));
		at = at + V3(0.2, 0, 0); now += 4;
		p.Feed(true, at, now, 0);
		CHECK(p.mps == 0, "a gap of 4 tics gives no velocity (MAX_GAP 3)");
	}
	{
		PunchReader p; int now = 0; V3 at;
		run(p, 0, 8, now, at, V3(1, 0, 0));
		CHECK(!p.Feed(false, at, now + 1, 0) && !p.prevValid, "a hand that is not a fist reads nothing");
	}
	// contact (gloves.zs:710-731) and the pulled-back rule (:622-627)
	{
		double dxy; V3 pt;
		const V3 body(100, 0, 0);
		CHECK(SweptContact(V3(60, 0, 40), V3(90, 0, 40), 7, body, 20, 56, dxy, pt), "a fist driven at a body's side meets it");
		CHECK(fabs(dxy - 10) < 1e-9 && fabs(pt.x - 90) < 1e-9, "the contact is the path's nearest point: dxy %.2f at x %.2f", dxy, pt.x);
		CHECK(!SweptContact(V3(60, 40, 40), V3(90, 40, 40), 7, body, 20, 56, dxy, pt), "a fist passing 40 to the side misses a radius 20 body");
		CHECK(!SweptContact(V3(60, 0, 70), V3(90, 0, 70), 7, body, 20, 56, dxy, pt), "a fist over its top (70 > 56 + 7) misses");
		CHECK(SweptContact(V3(60, 0, 60), V3(90, 0, 60), 7, body, 20, 56, dxy, pt), "a fist within gr over its top meets it");
		CHECK(DrivesInto(V3(3, 0, 0), V3(80, 0, 0), body), "driving toward the body is a blow");
		CHECK(!DrivesInto(V3(-3, 0, 0), V3(80, 0, 0), body), "pulled back out of the body is not");
		CHECK(DrivesInto(V3(0, 3, 0), V3(80, 0, 0), body), "a hook across it is a blow (dot 0 >= -0.2)");
	}
}

//===========================================================================
// The knock
//===========================================================================
static void RapCases()
{
	// a hand closing on a surface at `mps` from `from` metres to `to`, one key
	auto approach = [](RapDetector &r, int &now, double &d, double to, double mps, int key = 7, bool open = true) {
		int res = 0;
		while (fabs(d - to) > 1e-9)
		{
			const double step = mps / 35.0;
			d = d > to ? std::max(to, d - step) : std::min(to, d + step);
			now++;
			const int x = r.Feed(open, key, d, now);
			if (x) res = x;
		}
		return res;
	};
	{
		RapDetector r; int now = 0; double d = 0.08;
		r.Feed(true, 7, d, now);
		CHECK(approach(r, now, d, 0.0, 0.6) == 0, "a contact at 0.6 m/s is not yet a rap");
		CHECK(approach(r, now, d, 0.05, 0.6) == 1, "the hand leaving at once makes it the first rap");
		CHECK(approach(r, now, d, 0.0, 0.6) == 2, "a second light rap within 0.6 s is the knock");
	}
	{
		RapDetector r; int now = 0; double d = 0.08;
		r.Feed(true, 7, d, now);
		approach(r, now, d, 0.0, 2.5);
		CHECK(approach(r, now, d, 0.06, 0.6) == 0, "a blow at 2.5 m/s is no rap (a punch's speed)");
		approach(r, now, d, 0.0, 0.25);
		CHECK(approach(r, now, d, 0.06, 0.6) == 0, "a hand closing at 0.25 m/s is no rap (the PC run's ward approach)");
	}
	{
		RapDetector r; int now = 0; double d = 0.08;
		r.Feed(true, 7, d, now);
		approach(r, now, d, 0.0, 0.6);
		for (int k = 0; k < 15; k++) { now++; r.Feed(true, 7, d, now); }
		CHECK(approach(r, now, d, 0.06, 0.6) == 0, "a hand that rests on it 0.4 s before leaving made no rap");
	}
	{
		RapDetector r; int now = 0; double d = 0.08;
		r.Feed(true, 7, d, now);
		approach(r, now, d, 0.0, 0.6);
		CHECK(approach(r, now, d, 0.05, 0.6) == 1, "first rap");
		for (int k = 0; k < 30; k++) { now++; r.Feed(true, 7, d, now); }
		CHECK(approach(r, now, d, 0.0, 0.6) == 0, "a contact a second later is no knock ...");
		CHECK(approach(r, now, d, 0.05, 0.6) == 1, "... it is a new first rap");
		CHECK(approach(r, now, d, 0.0, 0.6) == 2, "and the next within the window completes the knock");
	}
	{
		RapDetector r; int now = 0; double d = 0.08;
		r.Feed(true, 7, d, now);
		approach(r, now, d, 0.0, 0.6);
		CHECK(approach(r, now, d, 0.05, 0.6) == 1, "first rap on one thing");
		d = 0.05; r.Feed(true, 9, d, ++now);
		CHECK(approach(r, now, d, 0.0, 0.6, 9) == 0 && approach(r, now, d, 0.05, 0.6, 9) == 1, "a rap on ANOTHER thing is a first again");
	}
	{
		RapDetector r; int now = 0; double d = 0.08;
		r.Feed(true, 7, d, now);
		approach(r, now, d, 0.0, 0.6);
		CHECK(approach(r, now, d, 0.03, 0.6) == 0, "backing off 3 cm (less than 3.5) confirms nothing");
		CHECK(approach(r, now, d, 0.0, 0.6) == 0, "and the hand is not re-armed for another contact");
	}
	{
		RapDetector r; int now = 0; double d = 0.08;
		r.Feed(false, 7, d, now);
		approach(r, now, d, 0.0, 0.6, 7, false);
		CHECK(approach(r, now, d, 0.05, 0.6, 7, false) == 0, "a fist (not an open hand) does not rap");
	}
}

//===========================================================================
// The attempts and the tingle
//===========================================================================
static void AttemptCases()
{
	// PUSH: a palm coming down into the band at 0.3 m/s
	{
		AttemptDetector a; a.kind = ATTEMPT_PUSH;
		int fired = 0;
		for (int k = 0; k < 40; k++)
		{
			const double z = 0.10 - k * 0.3 / 35.0;     // metres above the band's top
			AttemptSample s; s.outside = std::max(0.0, z); s.depth = -z; s.axisMps = 0.3;
			if (a.Feed(s)) fired++;
		}
		CHECK(fired == 1, "a push into the sofa's crease fires once per entry: %d", fired);
		for (int k = 0; k < 10; k++) { AttemptSample s; s.outside = 0.12; a.Feed(s); }
		AttemptSample in; in.outside = 0; in.depth = 0.05; in.axisMps = 0.3;
		CHECK(a.Feed(in), "re-armed after the hand left 10 cm");
		AttemptDetector slow; slow.kind = ATTEMPT_PUSH;
		AttemptSample s; s.outside = 0; s.depth = 0.05; s.axisMps = 0.05;
		CHECK(!slow.Feed(s), "a hand resting in it (0.05 m/s) is no push");
	}
	// PRESS: a fingertip pressing 1.5 cm at 0.1 m/s; a slower drift is not
	{
		AttemptDetector a; a.kind = ATTEMPT_PRESS;
		int fired = 0;
		for (int k = 0; k < 12; k++) { AttemptSample s; const double z = 0.01 - k * 0.1 / 35.0; s.outside = std::max(0.0, z); s.depth = -z; s.axisMps = 0.1; if (a.Feed(s)) fired++; }
		CHECK(fired == 1, "a key pressed fires once: %d", fired);
		AttemptDetector b; b.kind = ATTEMPT_PRESS;
		fired = 0;
		for (int k = 0; k < 40; k++) { AttemptSample s; const double z = 0.01 - k * 0.03 / 35.0; s.outside = std::max(0.0, z); s.depth = -z; s.axisMps = 0.03; if (b.Feed(s)) fired++; }
		CHECK(fired == 0, "a fingertip drifting down at 0.03 m/s presses nothing");
	}
	// TUG: grip closed inside, pulled out 6 cm at 0.3 m/s
	{
		AttemptDetector a; a.kind = ATTEMPT_TUG;
		AttemptSample s; s.outside = 0; s.depth = 0.30; s.grip = false;
		CHECK(!a.Feed(s), "an open hand on the drawers is no tug");
		s.grip = true; CHECK(!a.Feed(s), "the grip closing is not yet a tug");
		int fired = 0;
		for (int k = 1; k <= 10; k++) { s.depth = 0.30 + k * 0.3 / 35.0; s.axisMps = 0.3; s.outside = std::max(0.0, k * 0.3 / 35.0 - 0.03); if (a.Feed(s)) fired++; }
		CHECK(fired == 1, "pulled out 6 cm: one tug: %d", fired);
		AttemptDetector b; b.kind = ATTEMPT_TUG;
		AttemptSample o; o.outside = 0.05; o.depth = 0.4; o.grip = true;
		b.Feed(o);
		fired = 0;
		for (int k = 1; k <= 10; k++) { o.depth = 0.4 + k * 0.01; o.axisMps = 0.35; if (b.Feed(o)) fired++; }
		CHECK(fired == 0, "a grip closed outside the zone tugs nothing");
		AttemptDetector c; c.kind = ATTEMPT_TUG;
		AttemptSample q; q.outside = 0; q.depth = 0.3; q.grip = true;
		c.Feed(q);
		fired = 0;
		for (int k = 1; k <= 20; k++) { q.depth = 0.3 + k * 0.1 / 35.0; q.axisMps = 0.1; if (c.Feed(q)) fired++; }
		CHECK(fired == 0, "a slow drift back (0.1 m/s) is no tug");
	}
	// WAVE: the flies at 0.8 m/s; a slow hand is not a wave; the door search at its own 0.3 m/s
	{
		AttemptDetector a; a.kind = ATTEMPT_WAVE;
		int fired = 0, at = -1;
		for (int k = 0; k < 20; k++) { AttemptSample s; s.outside = 0; s.mps = 0.8; if (a.Feed(s)) { fired++; at = k; } }
		CHECK(fired == 1 && at == 10, "a wave through the flies fires once, after 0.25 m (tic 11 at 0.8 m/s): %d at %d", fired, at);
		AttemptDetector b; b.kind = ATTEMPT_WAVE;
		fired = 0;
		for (int k = 0; k < 60; k++) { AttemptSample s; s.outside = 0; s.mps = 0.4; if (b.Feed(s)) fired++; }
		CHECK(fired == 0, "a hand moving at 0.4 m/s in the cloud is no wave");
		AttemptDetector c; c.kind = ATTEMPT_WAVE; c.waveMps = 0.30;
		fired = 0;
		for (int k = 0; k < 40; k++) { AttemptSample s; s.outside = 0; s.mps = 0.35; if (c.Feed(s)) fired++; }
		CHECK(fired == 1, "the door search: a palm sweeping at 0.35 m/s fires once");
	}
	// the tingle
	{
		CHECK(WardTingle(0.20) == 0 && WardTingle(kWardNearM) == 0, "no tingle 15 cm or more from a ward");
		double last = 0;
		bool grows = true;
		for (double d = 0.149; d >= 0; d -= 0.01) { const double a = WardTingle(d); if (a < last) grows = false; last = a; }
		CHECK(grows, "the tingle grows as the palm nears");
		CHECK(fabs(WardTingle(0) - kWardMaxAmp) < 1e-12 && kWardMaxAmp <= 0.25, "faint: at most %.2f on the ward", kWardMaxAmp);
		CHECK(WardTingle(0.14) > 0 && WardTingle(0.14) < 0.06, "faint at the edge: %.3f", WardTingle(0.14));
	}
}

int main()
{
	PunchDifferential();
	PunchCases();
	RapCases();
	AttemptCases();
	printf("gesture_test: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
