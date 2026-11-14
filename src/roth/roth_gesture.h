#pragma once
//
// roth_gesture.h -- THE HAND GESTURES OF CHAPTER 1, ENGINE-FREE: the detectors and nothing else, so
// tools/gesture_test runs every one of them on scripted hand paths. What each detector's event DOES is
// roth_gestures.cpp's (always one of the original's clicks, through the VR picker's own paths).
// The owner's decisions: docs/VR_CHAPTER01_SHADOWS.md section 6 (Q3, Q4, Q6, Q9, Q13).
//
// Units: METRES and METRES PER SECOND, real ones (the vertical already multiplied back by the level's pixel
// stretch, as RSQ Melee does, gloves.zs:539-541); time in TICS of 35 Hz (TICRATE).
//
//===========================================================================
// 1. THE PUNCH -- RSQ Melee's punch reader (github.com/presidentkoopa/RSQ_UBQSeries, Melee/, the local clone
//    /home/claude/presidentkoopa/RSQ_UBQSeries/Melee), the reader only. Owner (Q6): "A fist swung into a
//    monster at speed fires the original's attack where the fist lands, with the ORIGINAL's damage (speed
//    only decides whether it counts); a punch into a switch or door is the original's use."
//
//    PORT CHECKLIST (RSQ = Melee/zscript/rsqml/; port = this file unless named):
//     1. per hand state: armed, swinging, approach, approachTics, lastHit, loc, prevValid, prevTic,
//        prevYaw, path, vel, hand, prevHand            RSQ gloves.zs:181-197 -> PunchReader fields
//     2. a hand with no position: prevValid = false    gloves.zs:500 -> PunchReader::Feed (valid false)
//     3. a hand that is not a fist reads nothing (prevValid = false)
//                                                     gloves.zs:484-487 (FistIn) -> Feed (valid false):
//        the caller feeds a hand only while it is a closed fist and holds no weapon (roth_gestures.cpp)
//     4. the hand measured against the body (X/Y the pawn's, Z the view's)
//                                                     gloves.zs:505, handsync.zs Body() -> the caller passes
//        `loc`, the palm minus (player x, y, viewz), in metres
//     5. dt = now - prevTic; state stored; vel = 0   gloves.zs:506-516 -> Feed
//     6. no velocity unless had && 1 <= dt <= MAX_GAP (3)        gloves.zs:517 -> Feed
//     7. a snap turn is no swing: yaw step > SNAP_DEG (15) * dt   gloves.zs:518 -> Feed
//     8. a jump no arm makes: |d| > JUMP_UNITS (48 map units) * dt
//                                                     gloves.zs:520 -> Feed, in metres: 48 / 34 (RSQ's units per
//        metre, core.zs:33-36) = kJumpM 1.412 m a tic
//     9. vel = d * TICRATE / dt                      gloves.zs:521 -> Feed
//    10. mps = |vel| with the vertical in real metres gloves.zs:539-541 -> Feed (loc is real metres already)
//    11. arming: unarmed, armed again once now - lastHit >= rehit (6) and mps < minSpeed (2.0) * 0.6
//                                                     gloves.zs:567-571 -> Feed
//    12. the approach: mps >= IN_MPS (0.25) and approachTics < MAX_TICS (14): approach += mps / TICRATE,
//        approachTics++, path += v / TICRATE; else the swing ends (reset)   gloves.zs:572-586 -> Feed
//    13. not a punch until approach >= ARM_M (0.15 m) gloves.zs:587 -> Feed
//    14. avg = approach * TICRATE / approachTics >= minSpeed (2.0 m/s)       gloves.zs:588-589 -> Feed
//    15. swinging = true; the contact is looked for this tic along prevHand -> hand
//                                                     gloves.zs:590-595 -> Feed returns true, avg in `avgMps`
//    16. the glove a sphere of 0.09 m plus Extra Reach (rsqml_punch_pad 4 map units)
//                                                     gloves.zs:594, CVARINFO:27 -> kGloveM + kPadM (4 / 34 m)
//    17. CONTACT: the nearest approach in the plane of this tic's path to a body's axis within radius + gr,
//        the height within the body's z - gr .. z + height + gr, the nearest body wins
//                                                     gloves.zs:710-731 -> SweptContact (per body); the nearest is
//        the caller's loop (roth_gestures.cpp PunchContact)
//    18. a wall between the fist and the body stops it (ClearPath, a trace from the contact point to the
//        body's axis at that height)                 gloves.zs:770-787 -> roth_gestures.cpp ClearPath (PickFrom)
//    19. which bodies: RSQ's living foes (IsFoe, core.zs:194-199) -> NOT as RSQ: every Realms object with a
//        pick box, and the ORIGINAL's cursor on it decides what the click does (roth_handclick.h: a creature
//        is the original's attack target, cursor type 5 -> its attack; a type 3 thing is used). The owner's
//        rule is the original's: the click Adam's fist makes there.
//    20. a hand pulled back out of a body is not a punch: dir . toBody < -0.2
//                                                     gloves.zs:622-627 -> DrivesInto
//    21. a landed blow: Deliver, then armed = swinging = false, approach = 0, path = 0, lastHit = now
//                                                     gloves.zs:628-634 -> Landed(); the blow itself is the
//        ORIGINAL's attack aimed at the contact (roth_gestures.cpp -> ActorsRequestFireAt), with the
//        original's damage: Deliver's damage, power, kinds and knockback are NOT ported (owner: no
//        speed-scaled damage)
//    22. no body: a switch or a door the fist meets this tic, worked as a Use press along prevHand -> hand
//        for |mv| + gr                               gloves.zs:602-609, core.zs:241-257 -> roth_gestures.cpp
//        PunchSurface (PickFrom along the fist's travel, then the original's click by its cursor)
//    23. used: a buzz in that hand (0.5, 4 tics) and the same reset    gloves.zs:610-619, core.zs:342-369 ->
//        Landed() + the haptic (roth_gestures.cpp: 0.5 for 4 tics' length, through VR_ScriptHaptic)
//    24. a swing that ends with nothing: Whiff (One-Two's chain reset)  gloves.zs:580, 1238-1242 -> NOT
//        PORTED (combos)
//    NOT PORTED by the owner's decision (Q6: "without its combos, uppercut, slap or headlock"): the open-palm
//    tap and the swat (gloves.zs:241-290, 553-559, 1158), the Hadouken and bullets (:561-565, :851-),
//    the barrel kick (:599-601, :659-700), the punch kinds jab / hook / uppercut and their lift
//    (:442-464, Deliver), speed-scaled damage (Deliver; CVARINFO:22-29), One-Two, Dempsey and the combos
//    (:245-265, :532-538), Star Punch, Guard (guard.zs), the gun-butt shove and the club (shove.zs, club.zs),
//    the Headlock (headlock.zs), Ground Pound and Clear Out (twohand.zs), the sparks (sparks.zs).
//    NOT PORTED, not needed here: RSQML_HandSync (handsync.zs: QuestZDoom writes the controllers per
//    rendered frame; this engine's hand points are read through the hand actor's own transform each tic,
//    roth_vrpick.cpp FingerRay) [R: not measured on a slow frame here].
//
// 2. KNOCK TO EXAMINE (owner, Q3): "Two light raps of an open, empty hand play the original's examine line
//    (plus a light haptic)." Our design, no source: a RAP is the hand closing on one surface at a rap's speed
//    (kRapMinMps .. kRapMaxMps: quicker than a hand laid on it, below the punch's 2.0 so a rap is never a punch),
//    reaching it (within kRapContactM) and LEAVING it again (back off kRapRearmM) within kRapLeaveTics: a tap, not
//    a hand that comes to rest. Two raps on the SAME target, the second's contact at least kRapMinGapTics and at
//    most kRapWindowTics (0.6 s) after the first's, are the knock (fired at the second contact).
//    (2026-11-13, the PC run of 03548253: a hand coming to rest at 0.25 m/s by a ward counted as a rap; the first
//    design took the contact alone, at 0.25 m/s.)
//
// 3. "A PHYSICAL ATTEMPT PLAYS THE EXAMINE LINE" (owner, Q4), the try gestures, each once per entry into
//    its zone and re-armed once the hand is kAttemptRearmM (10 cm, doc 2.6) outside it:
//      PUSH   the palm driven into the zone along its axis at least kPushDepthM, moving in at kPushMps
//      PRESS  the fingertip pressed into the zone along its axis at least kPressDepthM, at kPressMps
//      TUG    the grip closed inside the zone, then the hand pulled out along the axis kTugM at kTugMps
//      WAVE   the hand moved through the zone at kWaveMps or more for kWavePathM of path (the door search,
//             roth_gestures.cpp: the palm sweeping a door leaf, at its own slower speed)
//
// 4. THE WARD TINGLE (owner, Q9): haptic only, growing as the palm nears: WardTingle.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <math.h>

namespace roth
{
namespace gesture
{

constexpr int kTicRate = 35;

struct V3
{
	double x = 0, y = 0, z = 0;
	V3() {}
	V3(double a, double b, double c) : x(a), y(b), z(c) {}
	V3 operator+(const V3 &o) const { return V3(x + o.x, y + o.y, z + o.z); }
	V3 operator-(const V3 &o) const { return V3(x - o.x, y - o.y, z - o.z); }
	V3 operator*(double k) const { return V3(x * k, y * k, z * k); }
	double Dot(const V3 &o) const { return x * o.x + y * o.y + z * o.z; }
	double Length() const { return sqrt(x * x + y * y + z * z); }
	double LengthXY() const { return sqrt(x * x + y * y); }
};

//===========================================================================
// 1. The punch (see the checklist above)
//===========================================================================

// RSQ Melee's constants (gloves.zs:229-235, CVARINFO.txt:22-27), the map-unit ones in metres at RSQ's 34
// units per metre (core.zs:31-37).
constexpr double kArmM       = 0.15;            // ARM_M
constexpr double kInMps      = 0.25;            // IN_MPS
constexpr int    kMaxTics    = 14;              // MAX_TICS
constexpr double kJumpM      = 48.0 / 34.0;     // JUMP_UNITS
constexpr int    kMaxGap     = 3;               // MAX_GAP
constexpr double kSnapDeg    = 15.0;            // SNAP_DEG
constexpr double kMinMps     = 2.0;             // rsqml_punch_min_speed
constexpr int    kRehitTics  = 6;               // rsqml_punch_rehit
constexpr double kGloveM     = 0.09;            // the glove's radius (gloves.zs:594)
constexpr double kPadM       = 4.0 / 34.0;      // rsqml_punch_pad, 4 map units

struct PunchReader
{
	bool armed = false, swinging = false;
	double approach = 0;
	int approachTics = 0;
	int lastHit = -1000000;
	V3 loc, vel, path;
	bool prevValid = false;
	int prevTic = 0;
	double prevYaw = 0;
	double mps = 0;          // this tic's speed (a log line)
	double avgMps = 0;       // the swing's average once it counts

	void Invalidate() { prevValid = false; }

	// One tic. `valid`: the hand is a fist with a position (items 2-3). `locM`: the palm against the body,
	// metres, the vertical real (item 4). `yawDeg`: the player's yaw (item 7). `stretch`: the level's pixel
	// stretch, so the jump test (item 8) measures the step in map units as RSQ does (its |d| is the map-unit
	// delta, the vertical NOT stretched; here the vertical is taken back by it). True: this tic the swing counts
	// as a punch and the caller looks for what the fist met along its travel (item 15).
	bool Feed(bool valid, const V3 &locM, int now, double yawDeg, double stretch = 1.2)
	{
		if (!valid) { prevValid = false; return false; }                  // 2, 3
		const int dt = now - prevTic;                                     // 5
		const bool had = prevValid;
		const V3 prev = loc;
		double yawStep = fmod(yawDeg - prevYaw, 360.0);                   // Actor.DeltaAngle, its size
		if (yawStep > 180.0) yawStep -= 360.0;
		if (yawStep < -180.0) yawStep += 360.0;
		yawStep = fabs(yawStep);
		loc = locM;
		prevYaw = yawDeg;
		prevTic = now;
		prevValid = true;
		vel = V3();
		mps = 0;
		if (!had || dt < 1 || dt > kMaxGap) return false;                 // 6
		if (yawStep > kSnapDeg * dt) return false;                        // 7
		const V3 d = loc - prev;
		if (V3(d.x, d.y, d.z / (stretch > 0.1 ? stretch : 1.2)).Length() > kJumpM * dt) return false;   // 8
		vel = d * ((double)kTicRate / dt);                                // 9
		mps = vel.Length();                                               // 10
		if (!armed)                                                       // 11
		{
			if (now - lastHit >= kRehitTics && mps < kMinMps * 0.6) armed = true;
			return false;
		}
		if (mps >= kInMps && approachTics < kMaxTics)                     // 12
		{
			approach += mps / kTicRate;
			approachTics++;
			path = path + vel * (1.0 / kTicRate);
		}
		else
		{
			swinging = false;
			approach = 0;
			approachTics = 0;
			path = V3();
			return false;
		}
		if (approach < kArmM) return false;                               // 13
		avgMps = approach * kTicRate / approachTics;                      // 14
		if (avgMps < kMinMps) return false;
		swinging = true;                                                  // 15
		return true;
	}

	// A blow landed, or the fist worked a switch or a door (items 21, 23).
	void Landed(int now)
	{
		armed = false;
		swinging = false;
		approach = 0;
		approachTics = 0;
		path = V3();
		lastHit = now;
	}
};

// Item 17, one body: the fist's path a -> b (map units) against a body standing on `base` (its axis, map
// units) with `radius` and `height`, the glove `gr`. `dxy`: how near the path came to the axis in the plane;
// `p`: the fist's point there.
inline bool SweptContact(const V3 &a, const V3 &b, double gr, const V3 &base, double radius, double height, double &dxy, V3 &p)
{
	const double abx = b.x - a.x, aby = b.y - a.y;
	const double l2 = abx * abx + aby * aby;
	double t = l2 > 1e-8 ? ((base.x - a.x) * abx + (base.y - a.y) * aby) / l2 : 1.0;
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	p = a + (b - a) * t;
	dxy = sqrt((p.x - base.x) * (p.x - base.x) + (p.y - base.y) * (p.y - base.y));
	if (dxy > radius + gr) return false;
	if (p.z < base.z - gr || p.z > base.z + height + gr) return false;
	return true;
}

// Item 20: the blow must drive toward the body (`vel` the fist's velocity, `from` where it was last tic).
inline bool DrivesInto(const V3 &vel, const V3 &from, const V3 &body)
{
	V3 dir = vel;
	const double l = dir.Length();
	if (l < 1e-6) return true;            // RSQ aims at the body then (gloves.zs:623): toward it
	dir = dir * (1.0 / l);
	const double tx = body.x - from.x, ty = body.y - from.y;
	const double tl = sqrt(tx * tx + ty * ty);
	if (tl <= 1.0) return true;           // the fist on the axis (map units, gloves.zs:627)
	return (dir.x * tx + dir.y * ty) / tl >= -0.2;
}

//===========================================================================
// 2. Knock to examine
//===========================================================================

constexpr double kRapMinMps    = 0.40;   // a rap closes on the surface at least this fast ...
constexpr double kRapMaxMps    = 2.0;    // ... and below the punch's minimum (RSQ's 2.0): a rap is light
constexpr double kRapContactM  = 0.02;   // the hand within this of the surface has reached it
constexpr double kRapRearmM    = 0.035;  // and must back off this far ...
constexpr int    kRapLeaveTics = 10;     // ... within this (0.29 s) for the contact to have been a rap
constexpr double kRapNearM     = 0.10;   // a surface is followed while the hand is within this of it
constexpr int    kRapMinGapTics = 3;     // two raps' contacts at least this far apart ...
constexpr int    kRapWindowTics = 21;    // ... and at most 0.6 s

struct RapDetector
{
	long target = -1;          // what the hand is near (the caller's key; -1 nothing)
	double lastDist = 1e9;     // last tic's distance to it
	bool armed = true;         // backed off since the last contact
	int contactTic = -1000000; // the last contact at a rap's speed, not yet confirmed by leaving
	bool contactPending = false;
	int firstTic = -1000000;   // a confirmed first rap's contact
	bool haveFirst = false;
	double closing = 0;        // this tic's closing speed (a log line)

	void Reset() { target = -1; lastDist = 1e9; armed = true; contactPending = false; haveFirst = false; }

	// One tic. `open`: an open, empty hand (the caller's test). `key`: the nearest surface or object within
	// kRapNearM (-1 none), `distM` the hand's distance to it. Returns 1 when a first rap is confirmed (the hand
	// left it in time), 2 on the second rap's contact (THE KNOCK), 0 otherwise.
	int Feed(bool open, long key, double distM, int now)
	{
		closing = 0;
		if (!open || key < 0) { Reset(); return 0; }
		if (key != target)
		{
			Reset();
			target = key;
			lastDist = distM;
			armed = distM > kRapContactM;
			return 0;
		}
		closing = (lastDist - distM) * kTicRate;
		const bool reached = distM <= kRapContactM && lastDist > kRapContactM;
		lastDist = distM;
		int out = 0;
		if (distM >= kRapRearmM)
		{
			armed = true;
			if (contactPending)
			{
				contactPending = false;
				if (now - contactTic <= kRapLeaveTics) { haveFirst = true; firstTic = contactTic; out = 1; }
			}
		}
		if (haveFirst && now - firstTic > kRapWindowTics) haveFirst = false;
		if (!reached || !armed) return out;
		armed = false;
		if (closing < kRapMinMps || closing >= kRapMaxMps) { contactPending = false; return out; }
		if (haveFirst && now - firstTic >= kRapMinGapTics)
		{
			haveFirst = false;
			return 2;
		}
		contactPending = true;
		contactTic = now;
		return out;
	}
};

//===========================================================================
// 3. The attempts
//===========================================================================

enum AttemptKind { ATTEMPT_PUSH, ATTEMPT_PRESS, ATTEMPT_TUG, ATTEMPT_WAVE };

inline const char *AttemptName(int k)
{
	switch (k)
	{
	case ATTEMPT_PUSH:  return "push";
	case ATTEMPT_PRESS: return "press";
	case ATTEMPT_TUG:   return "tug";
	case ATTEMPT_WAVE:  return "wave";
	}
	return "?";
}

constexpr double kAttemptRearmM = 0.10;   // doc 2.6: "re-armed when the hand leaves 10 cm"
constexpr double kPushDepthM = 0.03, kPushMps = 0.10;
constexpr double kPressDepthM = 0.012, kPressMps = 0.06;
constexpr double kTugM = 0.05, kTugMps = 0.15;
constexpr double kWaveMps = 0.6, kWavePathM = 0.25;

// One tic of the hand against one zone (the caller measures it).
struct AttemptSample
{
	double outside = 1e9;   // metres outside the zone (0 inside)
	double depth = 0;       // metres in along the zone's axis from its entry face (push, press); out along it (tug)
	double axisMps = 0;     // speed along the axis, + inward (push, press), + outward (tug)
	double mps = 0;         // the hand's speed (wave)
	bool grip = false;      // the grip closed (tug)
};

struct AttemptDetector
{
	int kind = ATTEMPT_PUSH;
	double waveMps = kWaveMps;   // a wave's speed (the door search's slower sweep sets its own)
	bool armed = true, inside = false, gripped = false;
	double gripDepth = 0, wavePath = 0, maxDepth = 0;

	void Reset() { armed = true; inside = false; gripped = false; gripDepth = 0; wavePath = 0; maxDepth = 0; }

	// True once per entry, on the tic the try is made.
	bool Feed(const AttemptSample &s)
	{
		if (s.outside > 0)
		{
			inside = false;
			wavePath = 0;
			maxDepth = 0;
			if (s.outside >= kAttemptRearmM) { armed = true; gripped = false; }
			// A tug carries the hand OUT of the zone: it is measured from the grip, inside or just past it.
			if (kind != ATTEMPT_TUG) return false;
		}
		else inside = true;
		if (!armed) return false;
		switch (kind)
		{
		case ATTEMPT_PUSH:
		case ATTEMPT_PRESS:
		{
			const double need = kind == ATTEMPT_PUSH ? kPushDepthM : kPressDepthM;
			const double speed = kind == ATTEMPT_PUSH ? kPushMps : kPressMps;
			if (s.depth > maxDepth && s.axisMps >= speed) maxDepth = s.depth;
			if (maxDepth >= need) { armed = false; return true; }
			return false;
		}
		case ATTEMPT_TUG:
			if (!s.grip) { gripped = false; return false; }
			if (!gripped)
			{
				if (s.outside > 0) return false;          // the grip must close inside
				gripped = true;
				gripDepth = s.depth;
				return false;
			}
			if (s.depth - gripDepth >= kTugM && s.axisMps >= kTugMps) { armed = false; gripped = false; return true; }
			return false;
		case ATTEMPT_WAVE:
			if (s.mps >= waveMps) wavePath += s.mps / kTicRate;
			if (wavePath >= kWavePathM) { armed = false; return true; }
			return false;
		}
		return false;
	}
};

//===========================================================================
// 4. The ward tingle
//===========================================================================

constexpr double kWardNearM = 0.15;   // the tingle starts this far from the ward's face
constexpr double kWardMinAmp = 0.04, kWardMaxAmp = 0.22;   // faint at the edge, never more than a light buzz
constexpr int    kWardPulseTics = 2;  // one short pulse every other tic while near

// The tingle's strength at `distM` from the ward (0 = none): grows as the square of the closeness.
inline double WardTingle(double distM)
{
	if (distM >= kWardNearM) return 0.0;
	if (distM < 0) distM = 0;
	const double c = 1.0 - distM / kWardNearM;
	return kWardMinAmp + (kWardMaxAmp - kWardMinAmp) * c * c;
}

} // namespace gesture
} // namespace roth
