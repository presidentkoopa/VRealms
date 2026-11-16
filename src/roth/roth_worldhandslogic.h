#pragma once
//
// roth_worldhandslogic.h -- CHAPTER 2'S WORLD BY HAND, THE RULES, ENGINE-FREE (plain C++, tools/worldhands_test).
// The engine half is roth_worldhands.cpp (what each rule reads and which of the ORIGINAL's clicks it ends in).
//
// THE OWNER'S ANSWERS (docs/VR_CHAPTER02_SIGNS_AND_PORTENTS.md 6; 2026-11-13/14 and 2026-11-16) and the piece here:
//   Q1-Q3  a held statuette lowered onto the dais and RELEASED = one floor use where its base lands;
//          Q14 the Orb released into the door's aperture = #4                                    -> ReleaseEdge
//   Q15    the Shrive set in its socket and a 180 degree TWIST = the use;
//   Q16    the key in the lock lever's lock and a 30 degree turn = the use                        -> Twist
//   Q17    levers and the pit's chains: grip and PULL ~8 cm = one use;
//   Q30    the bookcase: grip a leaf's edge and pull ~4 cm = the use (Q24: the clock's case door alike) -> Pull
//   Q18    MAS6's hand prints: the palm PRESSED ~2 cm into a print = one use                     -> Press
//   Q19    the throne: the head lowered over the seat (really SITTING) = the use                  -> Sit
//   Q26    a vial TIPPED at the mouth = the original's use on self                                -> Drink
//   Q21    tiny boxes: any overlap of the fingertip or palm with a record's surface box counts (a CONTACT DISC,
//          not a point): the click is sent at an overlapping texel                               -> DiscOffsets
// Every one ends in exactly ONE of the original's clicks (the engine half sends it); none adds an outcome. A
// refusal is the original's own (its line, the item kept).
//
// UNITS: metres, degrees, tics (35 a second). Directions are in a metric world frame (z up, the level's pixel
// stretch already taken out by the caller).
//
// [R] marks a number that is reasoning, for the headset to judge (the owner's numbers are his).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <cmath>
#include <vector>
#include <utility>
#include <algorithm>

namespace roth
{
namespace wh
{

struct V3
{
	double x = 0, y = 0, z = 0;
	V3() = default;
	V3(double a, double b, double c) : x(a), y(b), z(c) {}
	V3 operator+(const V3 &o) const { return V3(x + o.x, y + o.y, z + o.z); }
	V3 operator-(const V3 &o) const { return V3(x - o.x, y - o.y, z - o.z); }
	V3 operator*(double s) const { return V3(x * s, y * s, z * s); }
	double Dot(const V3 &o) const { return x * o.x + y * o.y + z * o.z; }
	V3 Cross(const V3 &o) const { return V3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x); }
	double Length() const { return std::sqrt(x * x + y * y + z * z); }
	V3 Unit() const { const double l = Length(); return l > 1e-12 ? *this * (1.0 / l) : V3(); }
};

constexpr double kPi = 3.14159265358979323846;
constexpr int kTicsPerSecond = 35;

//==========================================================================
// The owner's numbers, and ours
//==========================================================================

constexpr double kSocketM       = 0.05;   // 2.3: the held item within 5 cm of the socket (the owner's 2.3 / Q14 design)
constexpr double kSocketHoldM   = 0.08;   // ... and it stays set while within this [R]
constexpr double kLeverPullM    = 0.08;   // Q17: "grip and pull ~8 cm"
constexpr double kLeafPullM     = 0.04;   // Q30: "grip a leaf's edge and pull ~4 cm"; the clock's case door alike [R]
constexpr double kLeafEdgeM     = 0.15;   // a leaf's EDGE: the grip within this of either end of the leaf [R: M10 not measured]
constexpr double kPrintPressM   = 0.02;   // Q18: "the palm pressed ~2 cm into a print"
constexpr double kPrintRearmM   = 0.01;   // the palm back 1 cm in front of the print re-arms it [R]
constexpr double kShriveTwistDeg = 180.0; // Q15
constexpr double kKeyTurnDeg    = 30.0;   // Q16
constexpr double kSitDropM      = 0.35;   // N16: the head drops 35 cm or more
constexpr double kSitRearmM     = 0.20;   // ... and rises back within 20 cm of standing before another sit [R]
constexpr double kSitNearM      = 0.45;   // the head within this of the seat, in the plane [R: the seat is 0.16 m deep]
constexpr double kSitFacingCos  = 0.50;   // facing away from the back: within 60 degrees of the seat's front [R]
constexpr double kMouthBelowEyeM = 0.08;  // Q26 (a): "within 8 cm under the eyes"
constexpr double kMouthNearM    = 0.10;   // the vial within this of the mouth [R]
constexpr double kDrinkTiltDeg  = 60.0;   // Q26 (a): "tipped >= 60 degrees"
constexpr double kDrinkRearmDeg = 30.0;   // upright again (or away from the mouth) before another drink [R]
constexpr int    kDrinkTics     = 6;      // held tipped at the mouth this long (0.17 s) [R]
constexpr double kTipDiscM      = 0.010;  // Q21: the fingertip's contact disc, radius [R: a fingertip is ~2 cm wide]
constexpr double kPalmDiscM     = 0.040;  // Q21: the palm's [R]
constexpr double kBaseDiscM     = 0.030;  // a released item's base (the statuette's foot, the Orb) [R]
constexpr double kDiscStepM     = 0.005;  // the disc is searched on rings this far apart [R]

//==========================================================================
// THE CONTACT DISC (Q21). The offsets, in the surface's own two directions, at which the click may land:
// the centre first, then ring after ring outward (each ring at kDiscStepM, its points about kDiscStepM apart
// along it), all within `radius`. The first offset whose point the original's dispatcher matches to a record
// is where the ONE click goes ("never two": the search stops there).
//==========================================================================

inline std::vector<std::pair<double, double>> DiscOffsets(double radius, double step = kDiscStepM)
{
	std::vector<std::pair<double, double>> out;
	out.push_back({ 0.0, 0.0 });
	if (radius <= 0 || step <= 0) return out;
	const int rings = (int)std::floor(radius / step + 1e-9);
	for (int r = 1; r <= rings; r++)
	{
		const double rad = r * step;
		const int n = std::max(6, (int)std::ceil(2.0 * kPi * rad / step));
		for (int k = 0; k < n; k++)
		{
			const double a = 2.0 * kPi * k / n;
			out.push_back({ rad * std::cos(a), rad * std::sin(a) });
		}
	}
	return out;
}

//==========================================================================
// Angles
//==========================================================================

// The signed angle (degrees) that turns `a` into `b` about `axis`, both taken in the plane square to it.
// 0 when either has nothing in that plane.
inline double SignedAngleAbout(const V3 &a, const V3 &b, const V3 &axis)
{
	const V3 n = axis.Unit();
	const V3 pa = a - n * a.Dot(n), pb = b - n * b.Dot(n);
	if (pa.Length() < 1e-9 || pb.Length() < 1e-9) return 0.0;
	return std::atan2(pa.Cross(pb).Dot(n), pa.Dot(pb)) * (180.0 / kPi);
}

// How far `up` leans from the world's up (degrees, 0..180).
inline double TiltDeg(const V3 &up)
{
	const V3 u = up.Unit();
	if (u.Length() < 1e-9) return 0.0;
	double c = u.z;
	c = c > 1 ? 1 : (c < -1 ? -1 : c);
	return std::acos(c) * (180.0 / kPi);
}

//==========================================================================
// THE RELEASE (Q1, Q14): one release = one use, never two. True on the tic the grip opens after having been
// closed, while the hand was live both tics. The caller then sends the ONE use at the item's contact point
// (or none, if the original's dispatcher matches nothing there). A grip that opens and closes again is a
// second release only when it opens again.
//==========================================================================

struct ReleaseEdge
{
	bool primed = false, wasClosed = false;
	void Reset() { primed = false; wasClosed = false; }
	bool Feed(bool live, bool gripClosed)
	{
		if (!live) { Reset(); return false; }
		const bool fire = primed && wasClosed && !gripClosed;
		wasClosed = gripClosed;
		primed = true;
		return fire;
	}
};

//==========================================================================
// THE PULL (Q17, Q30): the grip closed on the thing at `anchor`; the hand then travels `need` metres in any
// direction but INTO the thing (the component along `into` is left out when it goes in): one use, once per
// grip. The grip opening before that ends it with nothing.
//==========================================================================

struct Pull
{
	bool engaged = false, fired = false;
	V3 anchor, into;
	double need = kLeverPullM;

	void Reset() { engaged = false; fired = false; }
	void Engage(const V3 &at, const V3 &intoDir, double needM) { engaged = true; fired = false; anchor = at; into = intoDir.Unit(); need = needM; }
	double Travel(const V3 &at) const
	{
		V3 d = at - anchor;
		const double in = d.Dot(into);
		if (in > 0) d = d - into * in;
		return d.Length();
	}
	bool Feed(bool gripClosed, const V3 &at)
	{
		if (!engaged) return false;
		if (!gripClosed) { Reset(); return false; }
		if (fired) return false;
		if (Travel(at) >= need - 1e-9) { fired = true; return true; }
		return false;
	}
};

//==========================================================================
// THE PRESS (Q18): `depth` is how far the palm is past the print's face toward the wall (metres; negative in
// front of it). Armed while the palm is at least kPrintRearmM in front; fires once when, armed, the palm is
// over the print (`over`) and kPrintPressM or more past its face. Re-armed only by backing off again.
//==========================================================================

struct Press
{
	bool armed = false;
	void Reset() { armed = false; }
	bool Feed(bool live, bool over, double depth)
	{
		if (!live) { armed = false; return false; }
		if (depth <= -kPrintRearmM) { armed = true; return false; }
		if (armed && over && depth >= kPrintPressM - 1e-9) { armed = false; return true; }
		return false;
	}
};

//==========================================================================
// THE TWIST (Q15, Q16): the item SET in its socket with the grip closed engages; each tic the hand's up axis
// turns about the hand's forward axis (the item's own axis, pushed into the socket) and the turn is added up,
// unwrapped, either way; at `need` degrees, one use. Leaving the socket or opening the grip ends it.
//==========================================================================

struct Twist
{
	bool engaged = false, fired = false;
	double turned = 0;
	V3 prevUp;

	void Reset() { engaged = false; fired = false; turned = 0; }
	void Engage(const V3 &up) { engaged = true; fired = false; turned = 0; prevUp = up; }
	bool Feed(bool stillSet, const V3 &up, const V3 &axis, double need)
	{
		if (!engaged) return false;
		if (!stillSet) { Reset(); return false; }
		turned += SignedAngleAbout(prevUp, up, axis);
		prevUp = up;
		if (!fired && std::fabs(turned) >= need) { fired = true; return true; }
		return false;
	}
};

//==========================================================================
// THE SIT (Q19, N16): the head `drop` metres below its standing height, over the seat (`over`), facing away
// from its back (`facingAway`): one use, then nothing until the head rises within kSitRearmM of standing.
//==========================================================================

struct Sit
{
	bool armed = true;
	void Reset() { armed = true; }
	bool Feed(double drop, bool over, bool facingAway)
	{
		if (drop < kSitRearmM) { armed = true; return false; }
		if (armed && drop >= kSitDropM && over && facingAway) { armed = false; return true; }
		return false;
	}
};

// Facing away from a seat's back: the view's forward (in the plane) within 60 degrees of the seat's front
// direction (from the seat to the floor in front of it).
inline bool FacingAway(double fwdX, double fwdY, double frontX, double frontY)
{
	const double lf = std::sqrt(fwdX * fwdX + fwdY * fwdY), lo = std::sqrt(frontX * frontX + frontY * frontY);
	if (lf < 1e-9 || lo < 1e-9) return false;
	return (fwdX * frontX + fwdY * frontY) / (lf * lo) >= kSitFacingCos;
}

//==========================================================================
// THE DRINK (Q26): a vial in the item hand within kMouthNearM of the mouth, tipped kDrinkTiltDeg or more,
// for kDrinkTics: one use on self. Then nothing until it is upright again (under kDrinkRearmDeg) or away
// from the mouth.
//==========================================================================

struct Drink
{
	bool armed = true;
	int held = 0;
	void Reset() { armed = true; held = 0; }
	bool Feed(bool haveVial, double mouthDistM, double tiltDeg)
	{
		const bool atMouth = haveVial && mouthDistM <= kMouthNearM;
		if (!atMouth || tiltDeg < kDrinkRearmDeg) { armed = true; held = 0; return false; }
		if (!armed || tiltDeg < kDrinkTiltDeg) { held = 0; return false; }
		if (++held < kDrinkTics) return false;
		armed = false;
		held = 0;
		return true;
	}
	// The item at the mouth and leaning: the eye's examine (ledger row 37) waits (the owner's "to the eyes stays
	// the examine" -- a vial being drunk is not being looked at).
	static bool Tipping(bool haveVial, double mouthDistM, double tiltDeg)
	{
		return haveVial && mouthDistM <= kMouthNearM && tiltDeg >= kDrinkRearmDeg;
	}
};

} // namespace wh
} // namespace roth
