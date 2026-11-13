/*
** vr_handpose_math.h
**
** THE FINGERS OF THE VR HANDS, AS PURE MATH: controller input -> which pose each finger heads
** for -> the rotation each finger joint is drawn at, smoothed at the display rate.
**
** Nothing of the engine in it -- floats and arrays only -- so the exact code the renderer runs is
** compiled and run by tools/hands_test/ outside the engine. vr_handpose.cpp owns the engine half:
** which actor is a hand, reading the controller, the model's joints and pose frames, the palette.
**
** PORTED FROM UZDXREMA (read-only, HEAD 39e4dad), src/r_data/vr_avatar_pose.cpp UpdateCurls()
** (:419-698). The checklist, source line -> here, is at the top of vr_handpose.cpp. Where this
** departs from that source it says so at the line, with the reason (the owner's touch rule,
** 2026-11-12: "react to capacitive touch, too, so I can point and thumb and grab").
**
** CONVENTIONS. A quaternion is float[4] = (x, y, z, w), the order FQuaternion and UZDXREMA's
** gRot use. A pose is a frame of the hand model (the RS_Hands frame map below); a finger's target
** is "slerp from pose A to pose B by t", per joint, in the joint's own local (bone) space.
**
**---------------------------------------------------------------------------
*/

#pragma once

#include <cmath>

namespace VRHandPose
{

// UZDXREMA vr_avatar.h:33-38 and :43 (VRFINGER_*, VRFINGER_JOINTS).
enum
{
	FINGER_THUMB = 0,
	FINGER_INDEX,
	FINGER_MIDDLE,
	FINGER_RING,
	FINGER_PINKY,
	FINGER_COUNT
};
enum { FINGER_JOINTS = 3 };	// BASE, MID, TOP. UP_TOP (the tip) never rotates in the FBX and is not driven.

// UZDXREMA vr_avatar.h:47-49 (VRSIDE_*): which PHYSICAL hand, not which role.
enum
{
	SIDE_RIGHT = 0,
	SIDE_LEFT = 1,
	SIDE_COUNT
};

// THE RS_HANDS FRAME MAP (rs_hands_ref/README.md "Baked poses, frames 0-10"; rs_hands.zs:186-214),
// which the converter writes as frames 0-10 of hand_r.iqm / hand_l.iqm. Only these five are targets.
enum
{
	POSE_OPEN     = 0,	// every finger at rest
	POSE_POINT    = 1,	// index 0, middle/ring/pinky 85/90/90, thumb 14 (README: the thumb fixed for a straight index)
	POSE_FIST     = 3,	// index 85, middle/ring/pinky 85/90/90, thumb 30
	POSE_GRIP_TU  = 7,	// grip, thumb lifted (thumb 0)
	POSE_READY_TD = 8,	// index 30 resting on the trigger, thumb 25 down on the controller
	POSE_MAX_USED = 8
};

// What one controller says about the fingers, raw (UZDXREMA hw_vrmodes.h:82-88 VRFingerInput,
// and FVRRigHandState's trigger/gripValue/triggerTouch/thumbTouch, vr_rig.h:78-79).
struct Inputs
{
	float trigger = 0.f;		// 0..1, analog
	float grip = 0.f;			// 0..1, analog squeeze
	bool  triggerTouch = false;	// capacitive: a finger on the trigger
	bool  thumbTouch = false;	// capacitive: the thumb on the stick, a face button or the rest
	bool  valid = false;		// a controller pose this frame (UZDXREMA rig.Valid(hidx), :452)
};

// One finger's target: slerp(pose[from], pose[to], t), per joint.
struct FingerTarget
{
	int   from = POSE_OPEN;
	int   to = POSE_OPEN;
	float t = 0.f;
};

inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

//===========================================================================
//
// THE TARGETS. UZDXREMA UpdateCurls :441-454, with the owner's touch rule.
//
// UZDXREMA (:444-448), the starting point:
//     index  = trigger > 0 ? trigger : (triggerTouch ? 0.3 : 0)
//     middle = ring = pinky = grip
//     thumb  = thumbTouch ? 0.6 : 0
// as fractions of ONE fold (FRIK's table), so 0.3 is "a little" and 0.6 is "most of the way".
//
// HERE every finger heads for a POSE of this hand, per joint, and the touches choose which:
//
//   middle, ring, pinky: open -> fist by the analog grip. Same input as :445-447; the fist frame is
//       the fold instead of FRIK's angle table.
//
//   index: rests where the touch says, and the trigger pulls it from there into the fist:
//       base = triggerTouch ? ready_td (index 30, on the trigger) : open (straight)
//       index = slerp(base, fist, trigger)
//     DEPARTS FROM :444 IN TWO WAYS, both the owner's rule. The resting pose on a touch is
//     ready_td's index (the recipe's "index resting on the trigger") rather than 0.3 of a fold.
//     And the pull starts FROM that rest: :444 drops the touch the moment the trigger moves, so a
//     finger resting at 0.3 jumps back to 0.01 on the first millimetre of travel; here it is one
//     continuous motion from resting to pulled. Without a touch (a controller with no capacitive
//     trigger) it is exactly :444's curve, open -> fist by the trigger.
//
//   thumb: down on the controller while touching, up and out when not:
//       not touching: grip_tu's thumb (the recipe's "thumb lifted")
//       touching:     ready_td's thumb (25, "thumb wrapped") while the index is on or pulling the
//                     trigger; point's thumb (14) while the index is off it.
//     DEPARTS FROM :448. UZDXREMA folds the thumb 0.6 on a touch and leaves it at rest otherwise;
//     the owner asked for the thumb up/out off the controller and down on it, which is two poses,
//     not one fold. The two "down" thumbs are the recipe's own: rs_hands_ref/README.md "What changed
//     in this release" -- point's thumb was cut from 25 to 14 because with a straight index nothing
//     stops a 25 thumb and it swings clear of the hand -- and rs_hands.zs PoseFor (:359-362) picks
//     ready_td/gripfire (25) with the index on the trigger and point (14) with it off.
//
//   no controller (!valid): every finger open. UZDXREMA :450-454, unchanged: "an open hand, not a
//       frozen one".
//
//===========================================================================

inline void Targets(const Inputs &in, FingerTarget out[FINGER_COUNT])
{
	if (!in.valid)
	{
		for (int f = 0; f < FINGER_COUNT; ++f) out[f] = FingerTarget{ POSE_OPEN, POSE_OPEN, 0.f };
		return;
	}
	const float trigger = Clamp01(in.trigger);
	const float grip = Clamp01(in.grip);

	const int idxBase = in.triggerTouch ? POSE_READY_TD : POSE_OPEN;
	out[FINGER_INDEX] = FingerTarget{ idxBase, POSE_FIST, trigger };

	out[FINGER_MIDDLE] = FingerTarget{ POSE_OPEN, POSE_FIST, grip };
	out[FINGER_RING]   = FingerTarget{ POSE_OPEN, POSE_FIST, grip };
	out[FINGER_PINKY]  = FingerTarget{ POSE_OPEN, POSE_FIST, grip };

	const bool indexOnTrigger = in.triggerTouch || trigger > 0.f;
	const int thumbPose = !in.thumbTouch ? POSE_GRIP_TU : (indexOnTrigger ? POSE_READY_TD : POSE_POINT);
	out[FINGER_THUMB] = FingerTarget{ thumbPose, thumbPose, 0.f };
}

//===========================================================================
//
// THE RATE. UZDXREMA :424-431, unchanged:
//
//     dt = now - last;  if (!(dt > 0) || dt > 0.25) dt = 1/90;   // first frame, or a hitch
//     retain = 0.75 ^ (dt * 90)
//
// FRIK keeps 0.75 of the old value per 90 Hz frame; the exponent keeps that speed on any refresh
// rate. 50% in 27 ms, 90% in 89 ms (docs/FBX_HANDS_REPORT.md, "Latency").
//
//===========================================================================

inline double SanitiseDt(double dt)
{
	if (!(dt > 0.0) || dt > 0.25) dt = 1.0 / 90.0;
	return dt;
}

inline float Retain(double dt)
{
	return (float)pow(0.75, SanitiseDt(dt) * 90.0);
}

//===========================================================================
//
// UZDXREMA QSlerp (:392-408), verbatim: shortest arc, lerp past 0.9995, renormalised.
//
//===========================================================================

inline void QSlerp(const float a[4], const float b[4], float t, float out[4])
{
	float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
	float bb[4] = { b[0], b[1], b[2], b[3] };
	if (d < 0.f) { d = -d; for (float& x : bb) x = -x; }
	float wa, wb;
	if (d > 0.9995f) { wa = 1.f - t; wb = t; }
	else
	{
		const float th = acosf(d), s = sinf(th);
		wa = sinf((1.f - t) * th) / s; wb = sinf(t * th) / s;
	}
	float n = 0.f;
	for (int k = 0; k < 4; ++k) { out[k] = wa * a[k] + wb * bb[k]; n += out[k] * out[k]; }
	n = sqrtf(n); if (n < 1e-6f) n = 1.f;
	for (int k = 0; k < 4; ++k) out[k] /= n;
}

// The poses one model carries for the finger joints: [pose][finger][joint] = local rotation.
struct PoseTable
{
	float Q[POSE_MAX_USED + 1][FINGER_COUNT][FINGER_JOINTS][4] = {};
};

// This frame's target rotation for every finger joint.
inline void TargetRotations(const FingerTarget tgt[FINGER_COUNT], const PoseTable &poses,
	float out[FINGER_COUNT][FINGER_JOINTS][4])
{
	for (int f = 0; f < FINGER_COUNT; ++f)
	{
		const FingerTarget &ft = tgt[f];
		for (int j = 0; j < FINGER_JOINTS; ++j)
			QSlerp(poses.Q[ft.from][f][j], poses.Q[ft.to][f][j], Clamp01(ft.t), out[f][j]);
	}
}

//===========================================================================
//
// THE DRAWN ROTATIONS, smoothed. UZDXREMA FRotState (:385-390) and :691-696, unchanged:
//
//     first frame: take the target;  then  cur = slerp(cur, target, 1 - retain)
//
// UZDXREMA also smooths a scalar curl per joint (FCurlState, :376-379, :658-664) by the same
// retention, but nothing draws from it: the bones are turned by gRot alone (:1794). Not ported --
// a value nothing reads.
//
//===========================================================================

struct SideState
{
	float Q[FINGER_COUNT][FINGER_JOINTS][4] = {};
	bool  Init = false;
};

inline void Smooth(SideState &s, const float target[FINGER_COUNT][FINGER_JOINTS][4], float retain)
{
	for (int f = 0; f < FINGER_COUNT; ++f)
	{
		for (int j = 0; j < FINGER_JOINTS; ++j)
		{
			float *cur = s.Q[f][j];
			if (!s.Init) { for (int k = 0; k < 4; ++k) cur[k] = target[f][j][k]; }
			else { float o[4]; QSlerp(cur, target[f][j], 1.f - retain, o); for (int k = 0; k < 4; ++k) cur[k] = o[k]; }
		}
	}
	s.Init = true;
}

// One whole frame for one hand: inputs -> targets -> smoothed joint rotations.
inline void Step(SideState &s, const Inputs &in, const PoseTable &poses, double dt)
{
	FingerTarget tgt[FINGER_COUNT];
	Targets(in, tgt);
	float want[FINGER_COUNT][FINGER_JOINTS][4];
	TargetRotations(tgt, poses, want);
	Smooth(s, want, Retain(dt));
}

//===========================================================================
//
// THE HAND'S FRAME FROM ITS WRIST AND KNUCKLES. UZDXREMA vr_rig.cpp:733-750 HandFrameFromJoints,
// verbatim in doubles: X along the fingers (wrist to the middle knuckle), the knuckle line squared
// to it, the palm normal from the two (right hand k x f, left f x k), Y the negated palm normal,
// Z = X x Y. The grip socket: the knuckle centroid plus palm width times the vector UZDXREMA
// fitted to twelve hands, (-0.107, -0.455, +-0.138), in that frame.
//
// P = { wrist, index knuckle, middle knuckle, ring knuckle, pinky knuckle }, in a right-handed
// space (the IQM file's own). Returns false for a degenerate hand.
//
//===========================================================================

struct Vec3d
{
	double x = 0, y = 0, z = 0;
	Vec3d() = default;
	Vec3d(double a, double b, double c) : x(a), y(b), z(c) {}
	Vec3d operator+(const Vec3d &o) const { return { x + o.x, y + o.y, z + o.z }; }
	Vec3d operator-(const Vec3d &o) const { return { x - o.x, y - o.y, z - o.z }; }
	Vec3d operator*(double s) const { return { x * s, y * s, z * s }; }
	Vec3d operator-() const { return { -x, -y, -z }; }
	double Dot(const Vec3d &o) const { return x * o.x + y * o.y + z * o.z; }
	Vec3d Cross(const Vec3d &o) const { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
	double Length() const { return sqrt(Dot(*this)); }
	double LengthSquared() const { return Dot(*this); }
	void MakeUnit() { const double l = Length(); if (l > 0) { x /= l; y /= l; z /= l; } }
};

inline bool HandFrameFromJoints(const Vec3d P[5], bool right, Vec3d &X, Vec3d &Y, Vec3d &Z, Vec3d &socket)
{
	Vec3d f = P[2] - P[0];
	if (f.LengthSquared() < 1e-9) return false;
	f.MakeUnit();
	Vec3d kr = P[1] - P[4];
	Vec3d k = kr - f * kr.Dot(f);
	if (k.LengthSquared() < 1e-9) return false;
	k.MakeUnit();
	Vec3d palm = right ? k.Cross(f) : f.Cross(k);
	palm.MakeUnit();
	X = f; Y = -palm; Z = X.Cross(Y);
	const Vec3d cen = (P[1] + P[2] + P[3] + P[4]) * 0.25;
	const double width = (P[1] - P[4]).Length();
	socket = cen + (X * -0.107 + Y * -0.455 + Z * (right ? 0.138 : -0.138)) * width;
	return true;
}

//===========================================================================
//
// THE DEFAULT HAND FIT. UZDXREMA vr_rig.cpp:422-460 FVRRig::DefaultHandFit, verbatim: the FRIK hand
// frame (X wrist->fingers, -Y palm normal, thumb on -Z right / +Z left) in terms of the OpenXR grip
// frame (-Z through the ring of the fingers, +X into the right palm and away from the left):
//     right   X = -Y_grip   Y = +X_grip   Z = +Z_grip
//     left    X = -Y_grip   Y = -X_grip   Z = -Z_grip
// Column-major 4x4, columns are the hand axes in grip coordinates, no translation. Derived from the
// two specifications, not tuned; det +1 for both.
//
//===========================================================================

inline void DefaultHandFit(bool rightHand, float m[16])
{
	const float s = rightHand ? 1.f : -1.f;
	for (int i = 0; i < 16; ++i) m[i] = 0.f;
	m[0] =  0.f; m[1] = -1.f; m[2] =  0.f;	// column 0 = X_hand = -Y_grip
	m[4] =    s; m[5] =  0.f; m[6] =  0.f;	// column 1 = Y_hand = +X_grip (right) / -X_grip (left)
	m[8] =  0.f; m[9] =  0.f; m[10] =   s;	// column 2 = Z_hand = +Z_grip (right) / -Z_grip (left)
	m[15] = 1.f;
}

//===========================================================================
//
// THE MODEL'S SEAT IN THE HAND FRAME: model render coordinates -> the hand frame above, with the
// grip socket at the origin. Not a UZDXREMA function; it is the composition of three that are:
//
//   1. render -> file: the engine loads an IQM vertex (x, y, z) as (x, z, y) (models_iqm.cpp
//      LoadPosition), and that swap is its own inverse;
//   2. file -> hand frame: HandFrameFromJoints above, measured on the model's own bind pose, with
//      the wrist as origin -- UZDXREMA's "O_hand is measured per model" (vr_avatar_pose.cpp:1880),
//      done at load from the joints instead of offline by measure_avatar.py;
//   3. minus the grip socket: the socket is where a gripped handle's axis passes through the hand
//      (UZDXREMA puts it on a gun's grip frame, vr_avatar_pose.cpp:1960-1965, 1981-1994), and OpenXR's grip
//      position is "within the controller ... the palm centroid when held", the same point.
//
// Column-major 4x4. Contains the reflection that undoes the load swap (det -1), which is what makes
// the whole hand path -- grip frame (det < 0, its -Z metres) x fit x this -- come out unmirrored.
//
//===========================================================================

inline bool ModelSeat(const Vec3d P[5], bool right, float out[16], Vec3d *socketOut = nullptr)
{
	Vec3d X, Y, Z, socket;
	if (!HandFrameFromJoints(P, right, X, Y, Z, socket)) return false;
	const Vec3d &w = P[0];
	// hand = R^T (file - wrist) - (R^T (socket - wrist));  file = Swap(render)
	// => hand = R^T Swap render - R^T socket
	const Vec3d s = socket - w;
	const double sx = s.Dot(X), sy = s.Dot(Y), sz = s.Dot(Z);
	const double ox = -w.Dot(X) - sx, oy = -w.Dot(Y) - sy, oz = -w.Dot(Z) - sz;
	// Row r of R^T is axis r; multiplied by Swap on the right, file (x,y,z) = render (x,z,y), so
	// column 0 (render x) takes axis.x, column 1 (render y) takes axis.z, column 2 (render z) axis.y.
	for (int i = 0; i < 16; ++i) out[i] = 0.f;
	out[0] = (float)X.x; out[1] = (float)Y.x; out[2] = (float)Z.x;		// column 0
	out[4] = (float)X.z; out[5] = (float)Y.z; out[6] = (float)Z.z;		// column 1
	out[8] = (float)X.y; out[9] = (float)Y.y; out[10] = (float)Z.y;	// column 2
	out[12] = (float)ox; out[13] = (float)oy; out[14] = (float)oz; out[15] = 1.f;
	if (socketOut) *socketOut = socket;
	return true;
}

} // namespace VRHandPose
