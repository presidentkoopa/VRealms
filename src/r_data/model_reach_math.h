/*
** model_reach_math.h
**
** RS FORK -- THE PURE MATH OF DRAW-TIME JOINT POSES AND REACH CHAINS.
**
** model_reach.cpp owns the state (which actor, which joints, which target, the
** per-chain cvars) and the draw path; everything here is a function of its
** arguments, with nothing of the engine in it beyond vectors, quaternions and
** VSMatrix. It is a header so the exact code the renderer runs can be compiled and
** checked outside the engine (the plan's section 3g test table and continuity sweeps).
**
** THE REACH SOLVE IS VR_BODY_IK_RETURN_PLAN.md section 3g, "FINAL reference solver v3",
** mirrored from _old/RS_VRIK/tools/armcut_2026-09-14/ik_mockup.py (solve_free,
** solve_arm, joint_transforms). It supersedes section 3f and every earlier alignment. Its
** core is the shelved IK_SolveTwoBoneArm (vr_armik.cpp, git 0c34be01aa) with every
** guard that file paid for: bone length < 0.01, a target on the shoulder, the reach
** clamp [|Lu-Ll|*1.001+0.01, (Lu+Ll)*0.999], the pole projected off the aim with two
** fallbacks, the forearm aimed from the elbow at the REAL target, stretch by
** lengthening capped at a ratio. v3 adds, all stateless and continuous:
**   - SOFT REACH: past 90% of the natural length the solved reach eases toward it and
**     the bones lengthen to make up the difference -- ONE rule on both sides of the
**     stretch cap (a second rule at the cap popped the elbow 2.3 units);
**   - SWIVEL ALIGNMENT: the elbow stays on the exact two-bone circle and only turns
**     about the shoulder-wrist axis, weighted by how bent the arm is, how far the hand
**     points off that axis, and (1 + cu) so the +-180 wrap is multiplied by zero;
**   - TWIST that tapers to zero at +-180 instead of clamping, times a confidence that
**     fades out when the index side lies along the forearm.
**
** NOT CARRIED FROM THE SHELVED FILE, ON PURPOSE: the wrist rate limiter and hand
** smoothing (the hand is authoritative), marine joint names and indices, the wall
** clamp, the foregrip pin, the two-hand pin, upper-arm twist (v3 has none).
**
** SPACES. Every function works in the one space its caller picked -- the model's
** joint (IQM file) space in practice -- and never converts. VSMatrix is column-major,
** get()[col*4+row].
**
**---------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <vector>
#include "vectors.h"
#include "quaternion.h"
#include "matrix.h"
#include "TRS.h"

namespace ModelReach
{

inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline bool  Finite3(const FVector3 &v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kRadToDeg = 57.29577951308232f;
inline constexpr float kDegToRad = 0.017453292519943295f;

// ---- matrices ---------------------------------------------------------------

inline FVector3 MatTranslation(const VSMatrix &m)
{
	const FLOATTYPE *d = m.get();
	return FVector3((float)d[12], (float)d[13], (float)d[14]);
}

inline FVector3 MatPoint(const VSMatrix &m, const FVector3 &v)
{
	const FLOATTYPE *d = m.get();
	return FVector3(
		(float)(d[0] * v.X + d[4] * v.Y + d[8]  * v.Z + d[12]),
		(float)(d[1] * v.X + d[5] * v.Y + d[9]  * v.Z + d[13]),
		(float)(d[2] * v.X + d[6] * v.Y + d[10] * v.Z + d[14]));
}

// The 3x3 only: a DIRECTION, so translation never reaches it.
inline FVector3 MatDir(const VSMatrix &m, const FVector3 &v)
{
	const FLOATTYPE *d = m.get();
	return FVector3(
		(float)(d[0] * v.X + d[4] * v.Y + d[8]  * v.Z),
		(float)(d[1] * v.X + d[5] * v.Y + d[9]  * v.Z),
		(float)(d[2] * v.X + d[6] * v.Y + d[10] * v.Z));
}

inline VSMatrix MatMul(const VSMatrix &a, const VSMatrix &b)
{
	VSMatrix r = a;
	r.multMatrix(b);
	return r;
}

// The rotation of a matrix whose 3x3 is R * S with S a per-axis scale -- a joint
// transform. Columns are normalised first, so a UNIFORM ancestor scale is carried; a
// non-uniform one on an ancestor would skew this (review item 18, the assumption the
// shelved IK_MatRotation made). A mirror (det < 0) has its first column flipped so the
// result is a proper rotation. Trace-based (Shepperd) extraction, the closed-form
// inverse of VSMatrix::multQuaternion's construction.
inline FQuaternion MatRotation(const VSMatrix &m)
{
	const FLOATTYPE *d = m.get();
	FVector3 c0((float)d[0], (float)d[1], (float)d[2]);
	FVector3 c1((float)d[4], (float)d[5], (float)d[6]);
	FVector3 c2((float)d[8], (float)d[9], (float)d[10]);
	if (c0.LengthSquared() < 1.e-24 || c1.LengthSquared() < 1.e-24 || c2.LengthSquared() < 1.e-24)
		return FQuaternion(0.f, 0.f, 0.f, 1.f);
	c0.MakeUnit(); c1.MakeUnit(); c2.MakeUnit();
	if ((c0 | (c1 ^ c2)) < 0.f) c0 = -c0;

	const float m00 = c0.X, m10 = c0.Y, m20 = c0.Z;
	const float m01 = c1.X, m11 = c1.Y, m21 = c1.Z;
	const float m02 = c2.X, m12 = c2.Y, m22 = c2.Z;

	const float trace = m00 + m11 + m22;
	FQuaternion q(0.f, 0.f, 0.f, 1.f);
	if (trace > 0.f)
	{
		const float s = sqrtf(trace + 1.f) * 2.f;
		q.W = 0.25f * s;
		q.X = (m21 - m12) / s;
		q.Y = (m02 - m20) / s;
		q.Z = (m10 - m01) / s;
	}
	else if (m00 > m11 && m00 > m22)
	{
		const float s = sqrtf(1.f + m00 - m11 - m22) * 2.f;
		q.W = (m21 - m12) / s;
		q.X = 0.25f * s;
		q.Y = (m01 + m10) / s;
		q.Z = (m02 + m20) / s;
	}
	else if (m11 > m22)
	{
		const float s = sqrtf(1.f + m11 - m00 - m22) * 2.f;
		q.W = (m02 - m20) / s;
		q.X = (m01 + m10) / s;
		q.Y = 0.25f * s;
		q.Z = (m12 + m21) / s;
	}
	else
	{
		const float s = sqrtf(1.f + m22 - m00 - m11) * 2.f;
		q.W = (m10 - m01) / s;
		q.X = (m02 + m20) / s;
		q.Y = (m12 + m21) / s;
		q.Z = 0.25f * s;
	}
	q.MakeUnit();
	return q;
}

// A joint's local transform exactly as the IQM loader and CalculateBonesIQM build one:
// translate, then rotate, then scale (models_iqm.cpp).
inline VSMatrix ComposeTRS(const TRS &l)
{
	VSMatrix m;
	m.loadIdentity();
	m.translate(l.translation.X, l.translation.Y, l.translation.Z);
	m.multQuaternion(l.rotation);
	m.scale(l.scaling.X, l.scaling.Y, l.scaling.Z);
	return m;
}

// The inverse of ComposeTRS for a matrix with no shear. A negative determinant is
// carried as a negative X scale, the way a mirrored joint would have been built.
inline void DecomposeTRS(const VSMatrix &m, TRS &out)
{
	const FLOATTYPE *d = m.get();
	out.translation = FVector3((float)d[12], (float)d[13], (float)d[14]);
	FVector3 c0((float)d[0], (float)d[1], (float)d[2]);
	FVector3 c1((float)d[4], (float)d[5], (float)d[6]);
	FVector3 c2((float)d[8], (float)d[9], (float)d[10]);
	out.scaling = FVector3((float)c0.Length(), (float)c1.Length(), (float)c2.Length());
	if ((c0 | (c1 ^ c2)) < 0.f) out.scaling.X = -out.scaling.X;
	out.rotation = MatRotation(m);
}

// ---- quaternions and directions ---------------------------------------------

// Shortest-arc rotation taking unit a onto unit b (the shelved IK_QuatFromTo; the
// reference's mathutils rotation_difference). 180 degrees about a perpendicular when
// the two are opposite.
inline FQuaternion QuatFromTo(const FVector3 &a, const FVector3 &b)
{
	FVector3 axis = a ^ b;
	const float al = (float)axis.Length();
	const float d = (float)(a | b);
	if (al < 0.0001f)
	{
		if (d >= 0.f) return FQuaternion(0.f, 0.f, 0.f, 1.f);
		FVector3 fallback = (fabsf(a.X) < fabsf(a.Y)) ? FVector3(1.f, 0.f, 0.f) : FVector3(0.f, 1.f, 0.f);
		FVector3 perp = a ^ fallback;
		if (perp.LengthSquared() < 1.e-8f) perp = a ^ FVector3(0.f, 0.f, 1.f);
		perp.MakeUnit();
		return FQuaternion::AxisAngle(perp, FAngle::fromDeg(180.f));
	}
	axis /= al;
	return FQuaternion::AxisAngle(axis, FAngle::fromRad(atan2f(al, d)));
}

inline FQuaternion AxisAngleRad(const FVector3 &unitAxis, float rad)
{
	return FQuaternion::AxisAngle(unitAxis, FAngle::fromRad(rad));
}

// A fraction of a rotation: slerp from identity.
inline FQuaternion QuatFraction(const FQuaternion &q, float k)
{
	FQuaternion r = FQuaternion::SLerp(FQuaternion(0.f, 0.f, 0.f, 1.f), q, Clampf(k, 0.f, 1.f));
	r.MakeUnit();
	return r;
}

inline FVector3 UnitOr(const FVector3 &v, const FVector3 &fallback)
{
	const double l = v.Length();
	return l > 1.e-6 ? v / (float)l : fallback;
}

// Where along a bone a point sits, 0 at the parent joint and 1 at the child
// (the reference's tt).
inline float BoneFraction(const FVector3 &p, const FVector3 &boneStart, const FVector3 &boneDir, float boneLen)
{
	if (!(boneLen > 1.e-6f)) return 0.f;
	return Clampf((float)((p - boneStart) | boneDir) / boneLen, 0.f, 1.f);
}

// dt-based smoothing toward want at `rate` per second; rate <= 0 is none.
inline float SmoothToward(float cur, float want, float rate, float dt)
{
	if (!(rate > 0.f) || !(dt > 0.f) || !std::isfinite(rate) || !std::isfinite(dt)) return want;
	return cur + (want - cur) * (1.f - exp(-rate * dt));
}

// ---- the reach solve, section 3g -----------------------------------------------------------

// Every per-chain constant, defaults as section 3g states them. model_reach.cpp fills this
// from the chain's renderer-read cvars.
struct FReachTuning
{
	float stretchMax    = 1.25f;	// STRETCH_MAX
	float softStart     = 0.90f;	// SOFT_START, fraction of the natural length
	float align         = 1.0f;		// w
	float alignMax      = 40.0f;	// SWIVEL_MAX, degrees
	float fadeLo        = 0.10f;	// fade = clamp((bend - fadeLo) / fadeSpan)
	float fadeSpan      = 0.25f;
	float confLo        = 0.05f;	// conf = clamp((|fperp| - confLo) / confSpan)
	float confSpan      = 0.20f;
	float twist         = 1.0f;		// weight on the forearm twist
	float twistTaper    = 110.0f;	// TWIST_MAX: past it the twist eases to 0 at 180
	float twistConfLo   = 0.15f;	// tconf = clamp((|t| - lo) / span)
	float twistConfSpan = 0.30f;
	float twistOfs      = 0.0f;		// degrees added to the raw twist (a calibration trim; 0 = the reference)
};

struct FReachArmIn
{
	FVector3 shoulder, elbow, wrist;	// the three chain joints, as drawn before the solve
	FVector3 target;					// where the end joint must land (the reference's W)
	FVector3 pole;						// the weighted pole, not yet normalised or projected
	FVector3 up;						// the rig's up: first pole fallback, aim x up
	FVector3 fallback;					// second pole fallback (the reference's +X)
};

struct FReachCircle
{
	float    upperLen0 = 0.f, lowerLen0 = 0.f;	// Lup, Llo: as drawn before the solve
	FVector3 upperDir0, lowerDir0;				// dup_b, dlo_b
	float    raw = 0.f, soft = 0.f, stretch = 1.f, sc = 1.f, reach = 0.f;
	float    Lu = 0.f, Ll = 0.f;				// the lengths the circle is solved with (Lup*sc, Llo*sc)
	FVector3 aim, centre, u0, v0;
	float    a = 0.f, rad = 0.f;
};

// solve_free + the circle of solve_arm. False on the shelved degenerate cases; the
// caller then keeps the drawn pose.
inline bool ReachSolveCircle(const FReachArmIn &in, const FReachTuning &t, FReachCircle &c)
{
	if (!Finite3(in.shoulder) || !Finite3(in.elbow) || !Finite3(in.wrist) || !Finite3(in.target)) return false;
	FVector3 up0 = in.elbow - in.shoulder, lo0 = in.wrist - in.elbow;
	c.upperLen0 = (float)up0.Length();
	c.lowerLen0 = (float)lo0.Length();
	if (!(c.upperLen0 >= 0.01f) || !(c.lowerLen0 >= 0.01f)) return false;
	c.upperDir0 = up0 / c.upperLen0;
	c.lowerDir0 = lo0 / c.lowerLen0;

	const FVector3 to = in.target - in.shoulder;
	c.raw = (float)to.Length();
	if (!(c.raw >= 1.e-4f)) return false;		// target on the shoulder: no aim
	c.aim = to / c.raw;

	// SOFT REACH, ONE CONTINUOUS RULE (section 3g step 2).
	const float nat = c.upperLen0 + c.lowerLen0;
	const float start = nat * Clampf(std::isfinite(t.softStart) ? t.softStart : 0.9f, 0.f, 1.f);
	const float span = nat - start;
	if (c.raw > start && span > 1.e-6f)
	{
		c.soft = start + span * (1.f - exp(-(c.raw - start) / span));
		c.stretch = c.raw / c.soft;
	}
	else
	{
		c.soft = c.raw;
		c.stretch = 1.f;
	}
	const float cap = Clampf(std::isfinite(t.stretchMax) ? t.stretchMax : 1.f, 1.f, 2.5f);
	c.sc = std::min(c.stretch, cap);
	c.Lu = c.upperLen0 * c.sc;
	c.Ll = c.lowerLen0 * c.sc;

	const float lo = fabsf(c.Lu - c.Ll) * 1.001f + 0.01f, hi = (c.Lu + c.Ll) * 0.999f;
	if (!(lo < hi)) return false;
	c.reach = Clampf(c.sc * c.soft, lo, hi);

	c.a = (c.Lu * c.Lu - c.Ll * c.Ll + c.reach * c.reach) / (2.f * c.reach);
	c.rad = sqrtf(std::max(c.Lu * c.Lu - c.a * c.a, 0.f));
	c.centre = in.shoulder + c.aim * c.a;

	// u0: the pole projected off the aim, then the shelved fallbacks (aim x up, then
	// the rig's lateral axis unprojected, as the reference's +X).
	const FVector3 pole = UnitOr(in.pole, FVector3(0, 0, 0));
	FVector3 pp = pole - c.aim * (float)(pole | c.aim);
	if (pp.Length() < 1.e-4)
	{
		pp = c.aim ^ in.up;
		if (pp.Length() < 1.e-4) pp = in.fallback;
	}
	if (!(pp.Length() > 1.e-6)) return false;
	c.u0 = pp / (float)pp.Length();
	c.v0 = c.aim ^ c.u0;
	return Finite3(c.centre) && Finite3(c.u0);
}

struct FReachSwivel
{
	float bend = 0.f, fade = 0.f, conf = 0.f, k = 0.f;
	float phiDeg = 0.f;
};

// section 3g step 4, kept a function of its own: the swivel angle that turns the natural
// elbow toward the one whose forearm lines up with the hand. fingerDir unit (zero
// gives conf 0 and no swivel).
inline FReachSwivel ReachSwivelAngle(const FReachCircle &c, const FVector3 &fingerDir, const FReachTuning &t)
{
	FReachSwivel s;
	s.bend = c.Lu > 1.e-6f ? c.rad / c.Lu : 0.f;
	s.fade = Clampf((s.bend - t.fadeLo) / std::max(t.fadeSpan, 1.e-6f), 0.f, 1.f);
	const FVector3 fperp = fingerDir - c.aim * (float)(fingerDir | c.aim);
	const float fl = (float)fperp.Length();
	s.conf = Clampf((fl - t.confLo) / std::max(t.confSpan, 1.e-6f), 0.f, 1.f);
	if (t.align > 0.f && s.fade > 0.f && s.conf > 0.f && fl > 1.e-6f)
	{
		const FVector3 dt = -(fperp / fl);
		const float cu = (float)(dt | c.u0), cv = (float)(dt | c.v0);
		s.k = t.align * s.fade * s.conf * std::min(1.f, 1.f + cu);
		const float maxr = Clampf(std::isfinite(t.alignMax) ? t.alignMax : 0.f, 0.f, 180.f) * kDegToRad;
		s.phiDeg = Clampf(s.k * atan2f(cv, cu), -maxr, maxr) * kRadToDeg;
	}
	return s;
}

struct FReachArmPose
{
	FVector3    elbow, wristSolved, upperDir, lowerDir;
	FQuaternion swingU, swingL;
	float       lowerLenDrawn = 0.f;	// Ll_eff = min(|W - E|, Llo * STRETCH_MAX)
	float       gap = 0.f;				// |wristSolved - target|
	float       elbowSide = 0.f;		// (E - S0) . u0 / Lu
};

// solve_arm after the swivel: the elbow on the circle at phi, the bone directions,
// the swings from the drawn directions, the drawn forearm length.
inline void ReachPlaceElbow(const FReachArmIn &in, const FReachCircle &c, float phiDeg, const FReachTuning &t, FReachArmPose &p)
{
	const float phi = phiDeg * kDegToRad;
	p.elbow = c.centre + (c.u0 * cosf(phi) + c.v0 * sinf(phi)) * c.rad;
	p.upperDir = UnitOr(p.elbow - in.shoulder, c.aim);
	const FVector3 toW = in.target - p.elbow;
	const float lw = (float)toW.Length();
	p.lowerDir = lw > 1.e-6f ? toW / lw : c.lowerDir0;
	const float cap = Clampf(std::isfinite(t.stretchMax) ? t.stretchMax : 1.f, 1.f, 2.5f);
	p.lowerLenDrawn = std::min(lw, c.lowerLen0 * cap);
	p.wristSolved = p.elbow + p.lowerDir * p.lowerLenDrawn;
	p.swingU = QuatFromTo(c.upperDir0, p.upperDir);
	p.swingL = QuatFromTo(c.lowerDir0, p.lowerDir);
	p.gap = (float)(p.wristSolved - in.target).Length();
	p.elbowSide = c.Lu > 1.e-6f ? (float)((p.elbow - in.shoulder) | c.u0) / c.Lu : 0.f;
}

struct FReachTwist
{
	float tconf = 0.f;
	float rawDeg = 0.f;
	float twistDeg = 0.f;
};

// section 3g step 6. armRef: the end bone's reference roll direction at the pre-solve pose
// (the reference's across_b); the forearm's swing carries it. targetRef: the target's
// index side, unit. Either zero: no twist.
inline FReachTwist ReachForearmTwist(const FReachArmPose &p, const FVector3 &armRef, const FVector3 &targetRef, const FReachTuning &t)
{
	FReachTwist r;
	const FVector3 &dlo = p.lowerDir;
	FVector3 c = p.swingL * armRef;
	c = c - dlo * (float)(c | dlo);
	const float cl = (float)c.Length();
	FVector3 tv = targetRef - dlo * (float)(targetRef | dlo);
	const float tl = (float)tv.Length();
	r.tconf = Clampf((tl - t.twistConfLo) / std::max(t.twistConfSpan, 1.e-6f), 0.f, 1.f);
	if (tl > 1.e-4f && cl > 1.e-6f)
	{
		c /= cl;
		tv /= tl;
		float tw = atan2f((float)(dlo | (c ^ tv)), (float)(c | tv));
		if (t.twistOfs != 0.f && std::isfinite(t.twistOfs))
			tw = (float)remainder((double)tw + (double)t.twistOfs * kDegToRad, 2.0 * kPi);
		r.rawDeg = tw * kRadToDeg;
		// TAPER, NOT CLAMP: past the limit the twist eases back to 0 at exactly 180, so
		// both sides meet continuously.
		const float maxr = Clampf(std::isfinite(t.twistTaper) ? t.twistTaper : 110.f, 1.f, 179.f) * kDegToRad;
		if (fabsf(tw) > maxr)
			tw = copysignf(maxr * (kPi - fabsf(tw)) / (kPi - maxr), tw);
		tw *= r.tconf;
		r.twistDeg = tw * kRadToDeg * (std::isfinite(t.twist) ? t.twist : 0.f);
	}
	return r;
}

// joint_transforms: where a joint of a bone's segment goes. segStart0/segStart: the
// bone's first joint before/after; dir0/len0: the bone before; lenNew: its drawn
// length after. The joint slides along the bone by where it sits on it.
inline FVector3 ReachSegmentPoint(const FVector3 &p0, const FVector3 &segStart0, const FVector3 &segStart,
	const FQuaternion &swing, const FVector3 &dir0, float len0, float lenNew)
{
	const float tt = BoneFraction(p0, segStart0, dir0, len0);
	return segStart + swing * ((p0 - segStart0) + dir0 * (tt * (lenNew - len0)));
}

// The shelved shoulder follow: the swing that would point the follow joint's child at
// the target, by a fraction, capped in degrees. u0 and u1 unit.
inline FQuaternion ReachFollowSwing(const FVector3 &u0, const FVector3 &u1, float follow, float maxDeg)
{
	const FQuaternion swing = QuatFromTo(u0, u1);
	const float ang = 2.f * acosf(Clampf(fabsf(swing.W), 0.f, 1.f)) * kRadToDeg;
	float k = Clampf(std::isfinite(follow) ? follow : 0.f, 0.f, 1.f);
	const float maxd = Clampf(std::isfinite(maxDeg) ? maxDeg : 0.f, 0.f, 90.f);
	if (ang * k > maxd && ang > 1.e-3f) k = maxd / ang;
	return QuatFraction(swing, k);
}

// ---- piece A: local edits on a finished IQM bone palette ---------------------------------
//
// The palette CalculateBonesIQMSpecialized produces is
//     palette[i] = swap * G[i] * B[i]^-1 * swap
// with G the joint's global transform in file space and B its baseframe. So
//     G[i] = swap * palette[i] * swap * B[i]
// is recoverable, a local edit on joint j gives the delta D[j] = G'[parent] * L'[j] * G[j]^-1,
// an unedited joint under an edit inherits its parent's D, and
//     palette'[i] = swap * D[i] * swap * palette[i].
// Only edited joints need an inverse, and nothing is cached across models (a flushed
// model can be re-allocated at the same address).

struct FJointEdit
{
	int joint = -1;
	TRS local;
};

inline constexpr FLOATTYPE kSwapYZ[16] =
{
	1, 0, 0, 0,
	0, 0, 1, 0,
	0, 1, 0, 0,
	0, 0, 0, 1
};

class FJointPoseWork
{
public:
	// parents[i] < i for every joint (the IQM loader enforces it), -1 for a root.
	void Begin(const int *parentsIn, const VSMatrix *paletteIn, const VSMatrix *bindIn, int count)
	{
		parents = parentsIn;
		pal = paletteIn;
		bind = bindIn;
		n = count;
		G.resize(n); Ginv.resize(n); D.resize(n); localOrig.resize(n); over.resize(n);
		Flags(hasG); Flags(hasGinv); Flags(hasLocal); Flags(hasOver); Flags(dDirty);
		dStamp.assign(n, 0);
		stamp = 1;
		minEdited = n;
		edits.clear();
	}

	int Count() const { return n; }
	int Parent(int i) const { return parents[i]; }
	bool AnyEdit() const { return !edits.empty(); }
	const std::vector<FJointEdit> &Edits() const { return edits; }

	const VSMatrix &GlobalOrig(int i)
	{
		if (!hasG[i])
		{
			VSMatrix m;
			m.loadMatrix(kSwapYZ);
			m.multMatrix(pal[i]);
			m.multMatrix(kSwapYZ);
			m.multMatrix(bind[i]);
			G[i] = m;
			hasG[i] = 1;
		}
		return G[i];
	}

	// The joint's local transform as drawn (animation and stock overrides included), or
	// as edited. False when a singular parent leaves it undefined.
	bool LocalCur(int i, TRS &outLocal)
	{
		if (hasOver[i]) { outLocal = over[i]; return true; }
		if (!hasLocal[i])
		{
			hasLocal[i] = 2;
			const int p = parents[i];
			VSMatrix L;
			bool ok = true;
			if (p >= 0)
			{
				VSMatrix pinv;
				ok = GlobalOrigInverse(p, pinv);
				if (ok) L = MatMul(pinv, GlobalOrig(i));
			}
			else L = GlobalOrig(i);
			if (ok)
			{
				DecomposeTRS(L, localOrig[i]);
				hasLocal[i] = 1;
			}
		}
		if (hasLocal[i] != 1) return false;
		outLocal = localOrig[i];
		return true;
	}

	void SetLocal(int i, const TRS &local)
	{
		over[i] = local;
		hasOver[i] = 1;
		stamp++;
		if (i < minEdited) minEdited = i;
		for (auto &e : edits)
		{
			if (e.joint == i) { e.local = local; return; }
		}
		FJointEdit e;
		e.joint = i;
		e.local = local;
		edits.push_back(e);
	}

	// Put joint i at a GLOBAL position and rotation (its local scale kept), against its
	// parent as posed so far. False when the parent's posed transform is singular.
	bool SetGlobal(int i, const FVector3 &pos, const FQuaternion &rot)
	{
		TRS l;
		if (!LocalCur(i, l)) return false;
		const int p = parents[i];
		if (p >= 0)
		{
			VSMatrix gp = GlobalPosed(p), gpInv;
			if (!gp.inverseMatrix(gpInv)) return false;
			l.translation = MatPoint(gpInv, pos);
			l.rotation = (MatRotation(gp).Inverse() * rot).Unit();
		}
		else
		{
			l.translation = pos;
			l.rotation = rot.Unit();
		}
		SetLocal(i, l);
		return true;
	}

	VSMatrix GlobalPosed(int i)
	{
		if (hasOver[i])
		{
			const int p = parents[i];
			VSMatrix gp;
			if (p >= 0) gp = GlobalPosed(p);
			else gp.loadIdentity();
			gp.multMatrix(ComposeTRS(over[i]));
			return gp;
		}
		VSMatrix d;
		if (Delta(i, d)) return MatMul(d, GlobalOrig(i));
		return GlobalOrig(i);
	}

	// Writes the posed palette into dest (n matrices) and returns true, or returns false
	// and writes nothing when nothing was edited -- the caller then uses its own palette.
	bool FinishInto(VSMatrix *dest)
	{
		if (edits.empty() || dest == nullptr) return false;
		if (n > 0) memcpy((void *)dest, (const void *)pal, sizeof(VSMatrix) * n);
		for (int i = minEdited; i < n; i++)
		{
			VSMatrix d;
			if (!Delta(i, d)) continue;
			VSMatrix m;
			m.loadMatrix(kSwapYZ);
			m.multMatrix(d);
			m.multMatrix(kSwapYZ);
			m.multMatrix(pal[i]);
			dest[i] = m;
		}
		return true;
	}

private:
	void Flags(std::vector<uint8_t> &a) { a.assign(n, 0); }

	bool GlobalOrigInverse(int i, VSMatrix &outInv)
	{
		if (!hasGinv[i])
		{
			VSMatrix g = GlobalOrig(i);
			hasGinv[i] = g.inverseMatrix(Ginv[i]) ? 1 : 2;
		}
		outInv = Ginv[i];
		return hasGinv[i] == 1;
	}

	// D(i) = posed global * drawn global^-1. An edited joint gets its own; an unedited
	// joint under an edit inherits its parent's; anything else is untouched (false).
	bool Delta(int i, VSMatrix &outD)
	{
		if (dStamp[i] != stamp)
		{
			dStamp[i] = stamp;
			dDirty[i] = 0;
			const int p = parents[i];
			if (hasOver[i])
			{
				VSMatrix ginv;
				if (GlobalOrigInverse(i, ginv))
				{
					D[i] = MatMul(GlobalPosed(i), ginv);
					dDirty[i] = 1;
				}
			}
			else if (p >= 0)
			{
				VSMatrix dp;
				if (Delta(p, dp))
				{
					D[i] = dp;
					dDirty[i] = 1;
				}
			}
		}
		if (dDirty[i]) outD = D[i];
		return dDirty[i] != 0;
	}

	const int *parents = nullptr;
	const VSMatrix *pal = nullptr;
	const VSMatrix *bind = nullptr;
	int n = 0;
	std::vector<VSMatrix> G, Ginv, D, out;
	std::vector<TRS> localOrig, over;
	std::vector<uint8_t> hasG, hasGinv, hasLocal, hasOver, dDirty;
	std::vector<int> dStamp;
	int stamp = 1;
	int minEdited = 0;
	std::vector<FJointEdit> edits;
};

// ---- a reach chain written onto the pose ---------------------------------------------------

inline bool IsUnderJoint(const int *parents, int joint, int ancestor)
{
	int guard = 0;
	for (int p = parents[joint]; p >= 0 && guard < 4096; p = parents[p], guard++)
		if (p == ancestor) return true;
	return false;
}

// The joints that ride each bone (section 3g joint_transforms): the root and everything under
// it that is not under mid; mid and everything under it that is not under end. Ascending.
inline void ReachSegmentSets(const int *parents, int n, int root, int mid, int end, std::vector<int> &upper, std::vector<int> &lower)
{
	upper.clear();
	lower.clear();
	for (int j = 0; j < n; j++)
	{
		if (!(j == root || IsUnderJoint(parents, j, root))) continue;
		const bool underMid = j == mid || IsUnderJoint(parents, j, mid);
		const bool underEnd = j == end || IsUnderJoint(parents, j, end);
		if (!underMid) upper.push_back(j);
		else if (!underEnd) lower.push_back(j);
	}
}

struct FReachSegJoint
{
	int         j = -1;
	bool        lower = false;
	FVector3    p;		// global position before the solve
	FQuaternion r;		// global rotation before the solve
};

// Every joint the solve will place, with its pre-solve global pose, fetched BEFORE anything
// is written: a joint whose local cannot be read cannot leave half a chain posed.
inline bool ReachCaptureSegments(FJointPoseWork &w, const int *upper, int nu, const int *lower, int nl, int end,
	std::vector<FReachSegJoint> &seg)
{
	seg.clear();
	TRS probe;
	int a = 0, b = 0;
	while (a < nu || b < nl)
	{
		const bool takeUpper = b >= nl || (a < nu && upper[a] < lower[b]);
		const int j = takeUpper ? upper[a++] : lower[b++];
		if (!w.LocalCur(j, probe)) return false;
		const VSMatrix g = w.GlobalPosed(j);
		FReachSegJoint s;
		s.j = j;
		s.lower = !takeUpper;
		s.p = MatTranslation(g);
		s.r = MatRotation(g);
		seg.push_back(s);
	}
	return w.LocalCur(end, probe);
}

// section 3g joint_transforms, parents first: minimal swings from the drawn bone directions;
// stretch slides each joint along its bone by where it sits (LENGTHEN, never scale); along
// the mid bone the twist turns each joint in proportion; the end joint takes all of it and
// everything under the end joint rides it. endRot0: the end joint's pre-solve global rotation.
inline void ReachWriteSegments(FJointPoseWork &w, const std::vector<FReachSegJoint> &seg, int end, const FQuaternion &endRot0,
	const FReachArmIn &in, const FReachCircle &c, const FReachArmPose &p, float twistDeg)
{
	const float twistRad = twistDeg * kDegToRad;
	for (const auto &sj : seg)
	{
		FVector3 pos;
		FQuaternion rot;
		if (!sj.lower)
		{
			pos = ReachSegmentPoint(sj.p, in.shoulder, in.shoulder, p.swingU, c.upperDir0, c.upperLen0, c.Lu);
			rot = p.swingU * sj.r;
		}
		else
		{
			const float tt = BoneFraction(sj.p, in.elbow, c.lowerDir0, c.lowerLen0);
			pos = ReachSegmentPoint(sj.p, in.elbow, p.elbow, p.swingL, c.lowerDir0, c.lowerLen0, p.lowerLenDrawn);
			rot = AxisAngleRad(p.lowerDir, tt * twistRad) * p.swingL * sj.r;
		}
		w.SetGlobal(sj.j, pos, rot.Unit());
	}
	w.SetGlobal(end, p.wristSolved, (AxisAngleRad(p.lowerDir, twistRad) * p.swingL * endRot0).Unit());
}

// ---- joint space <-> model space ----------------------------------------------------------
//
// The renderer's model space is the file's (x, z, y); the swap is its own inverse, so this one
// function turns a vector either way.
inline FVector3 SwapYZ(const FVector3 &v) { return FVector3(v.X, v.Z, v.Y); }
inline bool     ReachNonZero(const FVector3 &v) { return v.LengthSquared() > 1.e-12; }

// ---- clearance: the elbow keeps clear of solid things (VR_BODY_IK_RETURN_PLAN.md 4b idea 6) -------
//
// Mirrored from ik_mockup.py arm_penetration and wall_swivel. The reference's walls are half-
// spaces; the level is not, so a solid thing here is a CONVEX REGION -- the points p with
// n_i . p + c_i <= 0 for every one of its planes, n_i unit. Its outside distance is taken as
// max_i (n_i . p + c_i), which is exact across a face and continuous everywhere, so a sphere of
// radius R at p penetrates it by R - max_i(...). A region of one plane IS the reference's wall.
// Piece B knows nothing of levels: the draw path hands it regions (level_solid_query.h).

inline constexpr int kReachRegionPlanes = 6;

struct FReachSolidRegion
{
	int      planes = 0;
	FVector3 n[kReachRegionPlanes];
	float    c[kReachRegionPlanes] = {};
};

struct FReachClearance
{
	const FReachSolidRegion *regions = nullptr;
	int   count = 0;
	float radius = 0.f;		// ARM_RADIUS, in the chain's joint units. <= 0: off
	float maxDeg = 100.f;	// WALL_SWIVEL_MAX: the swivel the push may reach, either way
};

// A world plane -- GL layout (x, up, y): s = nW . x + cW for a world point x -- as a plane of a chain's
// joint space, where joint point j is drawn at x = objectToWorld * swapYZ(j). Normalised so s is the
// distance in JOINT units (the arm's own, whatever its fit scale). Exact for any affine matrix: the
// image of a half-space is a half-space. False when the plane has no extent in joint space.
inline bool ReachPlaneToJoint(const VSMatrix &objectToWorld, double nx, double ny, double nz, double cW, FVector3 &nJ, float &cJ)
{
	const FLOATTYPE *d = objectToWorld.get();
	// (o2w3x3 * swap)^T n = swap * o2w3x3^T n
	const double tx = d[0] * nx + d[1] * ny + d[2] * nz;
	const double ty = d[4] * nx + d[5] * ny + d[6] * nz;
	const double tz = d[8] * nx + d[9] * ny + d[10] * nz;
	const double k = sqrt(tx * tx + ty * ty + tz * tz);
	if (!(k > 1.e-12) || !std::isfinite(k)) return false;
	const double off = (d[12] * nx + d[13] * ny + d[14] * nz + cW) / k;
	if (!std::isfinite(off)) return false;
	nJ = FVector3((float)(tx / k), (float)(tz / k), (float)(ty / k));
	cJ = (float)off;
	return true;
}

// How far a sphere at p pokes into the deepest region; 0 when clear (the reference's worst = 0.0).
inline float ReachRegionsPenetration(const FVector3 &p, float R, const FReachSolidRegion *regions, int count)
{
	float worst = 0.f;
	for (int r = 0; r < count; r++)
	{
		const FReachSolidRegion &g = regions[r];
		if (g.planes <= 0) continue;
		float outside = -FLT_MAX;
		for (int i = 0; i < g.planes && i < kReachRegionPlanes; i++)
		{
			const float s = (float)(g.n[i] | p) + g.c[i];
			if (s > outside) outside = s;
			if (outside >= R) break;	// this region cannot reach the sphere
		}
		const float pen = R - outside;
		if (pen > worst) worst = pen;
	}
	return worst;
}

// arm_penetration: the upper-arm middle, the elbow and the forearm middle (toward the wrist TARGET).
inline float ReachArmPenetration(const FVector3 &shoulder, const FVector3 &elbow, const FVector3 &target, const FReachClearance &cl)
{
	if (!(cl.radius > 0.f) || cl.count <= 0 || cl.regions == nullptr) return 0.f;
	const FVector3 upperMid = shoulder + (elbow - shoulder) * 0.5f;
	const FVector3 lowerMid = elbow + (target - elbow) * 0.5f;
	return std::max({ ReachRegionsPenetration(upperMid, cl.radius, cl.regions, cl.count),
		ReachRegionsPenetration(elbow, cl.radius, cl.regions, cl.count),
		ReachRegionsPenetration(lowerMid, cl.radius, cl.regions, cl.count) });
}

// wall_swivel: from the aligned swivel, Newton steps on the penetration over the swivel angle until
// clear, capped at +-maxDeg. Lengths and the wrist are untouched -- the elbow only walks round its
// exact circle -- and nothing moves until there is contact. A nearly straight arm has no circle and
// stays (the hand leads). Returns the swivel in degrees; the penetration before and after if asked.
inline float ReachClearSwivel(const FReachArmIn &in, const FReachCircle &c, float phiDeg, const FReachClearance &cl,
	float *penBefore = nullptr, float *penAfter = nullptr)
{
	auto pen = [&](float ph)
	{
		const FVector3 e = c.centre + (c.u0 * cosf(ph) + c.v0 * sinf(ph)) * c.rad;
		return ReachArmPenetration(in.shoulder, e, in.target, cl);
	};
	float phi = phiDeg * kDegToRad;
	if (penBefore) *penBefore = pen(phi);
	if (!(cl.radius > 0.f) || cl.count <= 0 || cl.regions == nullptr || !(c.rad > 1.e-3f))
	{
		if (penAfter) *penAfter = penBefore ? *penBefore : pen(phi);
		return phiDeg;
	}
	const float h = 2.f * kDegToRad;
	const float maxr = Clampf(std::isfinite(cl.maxDeg) ? cl.maxDeg : 0.f, 0.f, 180.f) * kDegToRad;
	for (int it = 0; it < 12; it++)
	{
		const float p = pen(phi);
		if (!(p > 0.f)) break;
		const float g = (pen(phi + h) - pen(phi - h)) / (2.f * h);
		if (!(fabsf(g) >= 1.e-6f)) break;
		phi = Clampf(phi - p / g, -maxr, maxr);
	}
	if (penAfter) *penAfter = pen(phi);
	return phi * kRadToDeg;
}

// ---- the whole chain solve as one function ----------------------------------------------------------
//
// Everything section 3g's solve reads, so the solve is a pure function of it: the pose the chain
// was drawn in (captured from the model's palette), this frame's target and tuning, the previous
// frame's smoothed values and the clearance regions. model_reach.cpp calls it from whichever draw
// needs the frame's solve first -- the chain's own model, or the model it reaches (a hand aiming its
// wrist along the forearm, idea 1) -- and the chain's own draw writes the result onto its palette.
// The same inputs give the same bits, so a second call in one frame changes nothing.

// The chain's joints as drawn, joint space, before the solve writes anything.
struct FReachChainPose
{
	FVector3    shoulder, elbow, wrist;		// root, mid, end global positions
	FQuaternion midRot, endRot;				// mid and end global rotations
	FQuaternion midRotAnimated;				// the mid joint at the model's own animated pose (before any draw-time edit)
	bool        hasFollow = false;			// a follow joint (a clavicle) is resolved
	FVector3    followPos;
	FQuaternion followRot;
};

inline bool ReachSamePose(const FReachChainPose &a, const FReachChainPose &b)
{
	auto q = [](const FQuaternion &x, const FQuaternion &y) { return x.X == y.X && x.Y == y.Y && x.Z == y.Z && x.W == y.W; };
	return a.shoulder == b.shoulder && a.elbow == b.elbow && a.wrist == b.wrist && q(a.midRot, b.midRot) && q(a.endRot, b.endRot)
		&& q(a.midRotAnimated, b.midRotAnimated) && a.hasFollow == b.hasFollow
		&& (!a.hasFollow || (a.followPos == b.followPos && q(a.followRot, b.followRot)));
}

// This frame's inputs, all in the chain's joint space.
struct FReachFrameIn
{
	FVector3 target;				// where the end joint must land
	FVector3 fingerDir;				// unit, or zero: no swivel alignment
	FVector3 targetTwistRef;		// unit, or zero: no twist
	FVector3 outward, down, back;	// the rig's own directions, unit (or zero)
	FVector3 twistRef;				// the end bone's reference roll at the animated pose; zero: no twist
	float    poleOut = 1.0f, poleDown = 0.6f, poleBack = 0.35f;
	float    follow = 0.25f, followMax = 25.0f;
	FReachTuning t;
	float    swivelRate = 0.f, twistRate = 0.f, clearRate = 0.f;	// per second; 0 = none (the reference)
	bool     histValid = false;		// the previous frame solved: smooth from it
	float    histPhi = 0.f, histTwist = 0.f, histClearOfs = 0.f;
	float    dt = 0.f;				// seconds since that frame
	FReachClearance clear;
	// [ENDOFS] A bind-pose point ON THE END BONE, in the chain model's own units, that the solve
	// lands on the target instead of the bone's origin. Zero takes the single-pass path and is
	// bit-identical to before this existed. It is rigidly attached to the bone, so it ROTATES
	// with it -- which is why ReachSolveChain below cannot just subtract it once.
	FVector3 endOfs = FVector3(0, 0, 0);
};

struct FReachSolveOut
{
	bool          followApplied = false;
	FQuaternion   followSwing = FQuaternion(0.f, 0.f, 0.f, 1.f);	// the follow joint turns by this about its own position
	FReachArmIn   in;
	FReachCircle  circle;
	FReachSwivel  swivel;
	FReachArmPose pose;
	FReachTwist   twist;
	FQuaternion   endRot0 = FQuaternion(0.f, 0.f, 0.f, 1.f);		// the end joint's rotation after the follow (ReachWriteSegments)
	float         phiAligned = 0.f;		// the alignment swivel after its smoothing -- what the next frame smooths from
	float         clearOfs = 0.f;		// what the clearance added, after its smoothing
	float         phi = 0.f;			// the swivel the elbow is placed at, degrees
	float         twistDeg = 0.f;		// after the twist smoothing
	float         penBefore = 0.f, penAfter = 0.f;
};

// [ENDOFS] The solve itself, unchanged. ReachSolveChain below wraps it and is what callers use:
// this one aims the end bone's ORIGIN at f.target and knows nothing about an offset.
inline bool ReachSolveChainOnce(const FReachChainPose &pose, const FReachFrameIn &f, FReachSolveOut &o)
{
	o = FReachSolveOut();
	FVector3 S0 = pose.shoulder, E0 = pose.elbow, W0 = pose.wrist;
	FQuaternion midRot = pose.midRot, endRot = pose.endRot;

	// The shelved shoulder follow (review item 14), before the solve: the follow joint turns about its own
	// position, so everything under it turns rigidly about that point.
	if (pose.hasFollow && f.follow > 0.f)
	{
		FVector3 u0 = S0 - pose.followPos, u1 = f.target - pose.followPos;
		if (ReachNonZero(u0) && ReachNonZero(u1))
		{
			u0.MakeUnit();
			u1.MakeUnit();
			o.followSwing = ReachFollowSwing(u0, u1, f.follow, f.followMax);
			o.followApplied = true;
			S0 = pose.followPos + o.followSwing * (S0 - pose.followPos);
			E0 = pose.followPos + o.followSwing * (E0 - pose.followPos);
			W0 = pose.followPos + o.followSwing * (W0 - pose.followPos);
			midRot = (o.followSwing * midRot).Unit();
			endRot = (o.followSwing * endRot).Unit();
		}
	}
	o.endRot0 = endRot;

	FReachArmIn &in = o.in;
	in.shoulder = S0;
	in.elbow = E0;
	in.wrist = W0;
	in.target = f.target;
	in.pole = f.outward * f.poleOut + f.down * f.poleDown + f.back * f.poleBack;
	in.up = -f.down;
	in.fallback = ReachNonZero(f.outward) ? f.outward : FVector3(1, 0, 0);
	if (!ReachSolveCircle(in, f.t, o.circle)) return false;

	// Swivel alignment (section 3g step 4), then its time smoothing.
	o.swivel = ReachSwivelAngle(o.circle, f.fingerDir, f.t);
	o.phiAligned = f.histValid ? SmoothToward(f.histPhi, o.swivel.phiDeg, f.swivelRate, f.dt) : o.swivel.phiDeg;

	// Clearance (idea 6) from the aligned swivel. Its own smoothing, 0 = the reference's instant push.
	float phi = o.phiAligned;
	if (f.clear.radius > 0.f && f.clear.count > 0 && f.clear.regions != nullptr)
	{
		const float want = ReachClearSwivel(in, o.circle, o.phiAligned, f.clear, &o.penBefore, &o.penAfter);
		if (f.histValid && f.clearRate > 0.f)
		{
			o.clearOfs = SmoothToward(f.histClearOfs, want - o.phiAligned, f.clearRate, f.dt);
			phi = o.phiAligned + o.clearOfs;
		}
		else
		{
			o.clearOfs = want - o.phiAligned;
			phi = want;
		}
	}
	o.phi = phi;
	ReachPlaceElbow(in, o.circle, phi, f.t, o.pose);

	// Forearm twist (section 3g step 6). The arm's reference is stated at the model's own animated pose, so it is
	// carried to the pre-solve pose by whatever turned the mid bone since (the pose layer, the follow).
	if (ReachNonZero(f.twistRef) && ReachNonZero(f.targetTwistRef))
	{
		const FQuaternion toPre = (midRot * pose.midRotAnimated.Inverse()).Unit();
		o.twist = ReachForearmTwist(o.pose, toPre * f.twistRef, f.targetTwistRef, f.t);
		o.twistDeg = f.histValid ? SmoothToward(f.histTwist, o.twist.twistDeg, f.twistRate, f.dt) : o.twist.twistDeg;
	}
	return true;
}

// [ENDOFS] LAND A POINT ON THE END BONE RATHER THAN THE BONE'S ORIGIN.
//
// WHY THIS IS NOT `target -= ofs`. The offset is rigidly attached to the end bone, so it rotates
// with it -- and the bone's final rotation is an OUTPUT of the solve (ReachSolveCircle places the
// wrist, then the swivel and twist steps decide how it is turned). Target and orientation are
// coupled. Subtracting a fixed vector gives a small ORIENTATION-DEPENDENT error that WANDERS as
// the target moves, which is the worst kind: it looks like tracking noise rather than like a bug.
//
// It is a fixed point and it converges immediately at these magnitudes:
//   pass 1  solve with the PRE-solve endRot orienting the offset (pose.endRot, already in hand);
//   pass 2  take the SOLVED endRot, rotate the offset by it, subtract, solve again.
//
// TWO PASSES IS THE ANSWER, NOT A COMPROMISE. The offset is about 1.9 units against a ~20-unit
// arm, so pass two moves the result by a fraction of a millimetre and a third would be waste.
// DO NOT "SIMPLIFY" THIS BACK TO ONE PASS -- that reintroduces the wandering error above, and it
// is invisible in a still frame and untraceable in motion.
//
// A zero offset never enters any of this: it takes the single-pass path and is bit-identical to
// the behaviour before this existed.
inline bool ReachSolveChain(const FReachChainPose &pose, const FReachFrameIn &f, FReachSolveOut &o)
{
	if (!ReachNonZero(f.endOfs)) return ReachSolveChainOnce(pose, f, o);

	FReachFrameIn pass = f;

	// Pass one: the best orientation we have before solving is the bone's current one.
	pass.target = f.target - (pose.endRot * f.endOfs);
	if (!ReachSolveChainOnce(pose, pass, o)) return false;

	// Pass two: re-aim using the orientation the solve actually produced. endRot0 is the end
	// joint's rotation after the follow, which is what the offset is attached to.
	const FVector3 corrected = f.target - (o.endRot0 * f.endOfs);
	if (!Finite3(corrected)) return true;		// keep pass one rather than fail outright
	pass.target = corrected;
	FReachSolveOut second;
	if (!ReachSolveChainOnce(pose, pass, second)) return true;	// pass one stands
	o = second;
	return true;
}

// ---- a joint of the model a chain reaches, aimed along the chain (4b idea 1) --------------------------------
//
// Mirrored from ik_mockup.py stub_q: the RS hand's wrist stub (skinned to Root_joint) turns about the palm
// until it lies along the solved forearm, toward the elbow, at most 70 degrees; HANDPALM, its child,
// keeps its place, so the palm and fingers stay exactly where the controller put them. Generally: a joint of
// the TARGET model turns about a pivot so one of its axes points back along the chain's end bone.

// The direction from the chain's end joint back toward its mid joint, in the TARGET model's joint space.
// chainO2W / targetO2W: the two models' drawn matrices; lowerDirJ: the solved end bone (mid -> end), the
// chain's joint space. A direction through an affine map keeps its line, so only the 3x3s matter. Zero
// when the target's matrix is singular.
inline FVector3 ReachAimToward(const VSMatrix &chainO2W, const FVector3 &lowerDirJ, const VSMatrix &targetO2W)
{
	const FVector3 world = MatDir(chainO2W, SwapYZ(lowerDirJ));
	VSMatrix inv;
	VSMatrix m = targetO2W;
	if (!m.inverseMatrix(inv)) return FVector3(0, 0, 0);
	return UnitOr(SwapYZ(MatDir(inv, -world)), FVector3(0, 0, 0));
}

struct FReachAim
{
	bool        valid = false;
	FQuaternion q = FQuaternion(0.f, 0.f, 0.f, 1.f);	// the turn, the target's joint space
	FVector3    pivot;								// what it turns about, the target's joint space, as drawn
	float       wantDeg = 0.f, usedDeg = 0.f;
};

// The turn taking axisDrawn onto toward (both unit), `weight` of it and at most maxDeg -- stub_q's
// rotation_difference slerped to STUB_BEND_MAX.
inline FReachAim ReachAimRotation(const FVector3 &axisDrawn, const FVector3 &toward, float maxDeg, float weight)
{
	FReachAim a;
	if (!Finite3(axisDrawn) || !Finite3(toward) || !ReachNonZero(axisDrawn) || !ReachNonZero(toward)) return a;
	const FQuaternion q = QuatFromTo(axisDrawn, toward);
	const float ang = 2.f * acosf(Clampf(fabsf(q.W), 0.f, 1.f)) * kRadToDeg;
	float k = Clampf(std::isfinite(weight) ? weight : 0.f, 0.f, 1.f);
	const float maxd = Clampf(std::isfinite(maxDeg) ? maxDeg : 0.f, 0.f, 180.f);
	if (ang * k > maxd && ang > 1.e-4f) k = maxd / ang;
	a.q = QuatFraction(q, k);
	a.wantDeg = ang;
	a.usedDeg = ang * k;
	a.valid = true;
	return a;
}

// Where the aim turns from, as drawn: axisJ and pivotJ are the joint's axis and pivot in the model's
// joint space at REST; the joint's drawn skin (global * bind^-1) carries them to where they are drawn.
// False when the bind is singular.
inline bool ReachAimFrame(FJointPoseWork &w, const VSMatrix &bind, int joint, const FVector3 &axisJ, const FVector3 &pivotJ,
	FVector3 &axisDrawn, FVector3 &pivotDrawn)
{
	VSMatrix b = bind, binv;
	if (!b.inverseMatrix(binv)) return false;
	const VSMatrix skin = MatMul(w.GlobalPosed(joint), binv);
	axisDrawn = UnitOr(MatDir(skin, axisJ), FVector3(0, 0, 0));
	pivotDrawn = MatPoint(skin, pivotJ);
	return ReachNonZero(axisDrawn) && Finite3(pivotDrawn);
}

// Turn the joint by aim.q about aim.pivot. keepChildren: its direct children keep their drawn place
// (they counter-rotate), so only what is skinned to the joint itself moves.
inline void ReachApplyAim(FJointPoseWork &w, int joint, const FReachAim &aim, bool keepChildren)
{
	if (!aim.valid || joint < 0 || joint >= w.Count()) return;
	static thread_local std::vector<int> kids;
	static thread_local std::vector<FVector3> kidPos;
	static thread_local std::vector<FQuaternion> kidRot;
	kids.clear(); kidPos.clear(); kidRot.clear();
	if (keepChildren)
	{
		for (int k = joint + 1; k < w.Count(); k++)
		{
			if (w.Parent(k) != joint) continue;
			const VSMatrix g = w.GlobalPosed(k);
			kids.push_back(k);
			kidPos.push_back(MatTranslation(g));
			kidRot.push_back(MatRotation(g));
		}
	}
	const VSMatrix g = w.GlobalPosed(joint);
	const FVector3 p = MatTranslation(g);
	if (!w.SetGlobal(joint, aim.pivot + aim.q * (p - aim.pivot), (aim.q * MatRotation(g)).Unit())) return;
	for (size_t k = 0; k < kids.size(); k++)
		w.SetGlobal(kids[k], kidPos[k], kidRot[k]);
}

} // namespace ModelReach
