/*
** vr_handpose_math.h
**
** THE VR HANDS, AS PURE MATH: the controller's finger input and the authored pose it picks, the
** display-rate smoothing, each finger joint's canonical bone frame, the pose rotations made from the
** FBX's own frames, the turn applied to the joints, and where the hand sits on the controller (its
** frame from the wrist and knuckles, the grip socket).
**
** Nothing of the engine in it -- floats, doubles and std containers only -- so the exact code the
** renderer runs is compiled and run by tools/hands_test/ and tools/fbxload_test/ outside the engine.
** vr_handpose.cpp owns the engine half: which actor is a hand, reading the controller, reading the
** model's joints and frames, the palette.
**
** PORTED FROM UZDXREMA (read-only, HEAD 39e4dad):
**   src/r_data/vr_avatar_pose.cpp ("VAP") UpdateCurls (:419-698): the rate, the touch rule's 0.3, the
**     no-controller rule, the [POSEROT] blend between two named poses and gRot's smoothing; and the
**     finger turn on the joint globals (:1771-1838);
**   tools/avatar/measure_avatar.py ("MA") the palm normal (:365-377) and the canonical bone frame
**     of every finger joint (:476-555);
**   tools/avatar/mkfbxposes.py ("MKP") a joint's pose rotation from two FBX frames, in that frame
**     (main(), the `D = ...; quat(F^T D F)` lines, and quat()).
**   VAP :466-656 and :667-684, a hand holding a weapon or an object: which named pose shapes each finger and its
**     rotation target (SelectHold, HoldTargets), with the poses of the owner's tools/avatar/fbx_poses.json
**     (kOwnerPoses), each hand from its own take only.
** NOT FROM A SOURCE: which authored frame each pose is (kPoseSources, chosen by walking the takes,
** tools/fbxload_test/walk_poses.py) and which touches pick which pose (PoseRule), on the owner's word.
** The checklist, source line -> here, is at the top of vr_handpose.cpp. Where this departs from a
** source it says so at the line, with the reason.
**
** CONVENTIONS. A quaternion is float[4] or double[4] = (x, y, z, w), the order FQuaternion and
** UZDXREMA's gRot use. A 4x4 is column-major float[16], VSMatrix's layout. A 3x3 frame is three
** column vectors.
**
**---------------------------------------------------------------------------
*/

#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }


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

//===========================================================================
//
// THE DRAWN ROTATIONS, smoothed. UZDXREMA FRotState (:385-390) and :691-696, unchanged:
//
//     first frame: take the target;  then  cur = slerp(cur, target, 1 - retain)
//
// UZDXREMA also smooths a scalar curl beside it (FCurlState, VAP :658-665) that no bone reads; with the poses
// there is no scalar curl, so it is not kept.
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


//===========================================================================
//
// THE AUTHORED POSES. The owner, 2026-11-13: "walk the hand poses", "the hands have poses, why are we
// bending fingers". The hands are driven by poses AUTHORED in hand_final.fbx's own takes, never by a
// per-finger fold about an axis:
//
//   the touches pick the TARGET POSE      index on the trigger (trigger touch, or any pull) / off it,
//                                         thumb on a surface (thumb touch) / off it:
//                                             index on,  thumb on   POSE_FIST
//                                             index on,  thumb off  POSE_THUMBUP
//                                             index off, thumb on   POSE_POINT
//                                             index off, thumb off  POSE_FINGERGUN
//   the analog inputs say HOW FAR         the index: the trigger when it is on it (UZDXREMA's rule, VAP :444:
//   from the open pose toward it          a touch without a pull rests it 0.3 of the way); every other
//                                         finger, and the index when it is off the trigger: the grip.
//   no controller this frame              every amount 0: the open hand (VAP :450-454, unchanged)
//
// so "open" is any pose at no grip and no trigger, and "trigger only" is POSE_FIST / POSE_THUMBUP at no
// grip: the index curls to the fist's index, the rest stay open.
//
// Each joint's target is QSlerp(open, pose, amount) on rotations made from two AUTHORED frames (PoseRotation,
// relative to the drawn frame, in the joint's canonical frame), so every in-between is a mix of two authored
// poses and the ends are the authored frames exactly. This is UZDXREMA's [POSEROT] blend (VAP :674-681,
// QSlerp(ready, fire, idxT) between two named poses) and its named-pose target (VAP :682-683), with the
// poses taken from the take by tools/fbxload_test/walk_poses.py (kPoseSources below) instead of from
// vrhandpose.txt. The smoothing is UZDXREMA's, unchanged (Smooth, VAP :691-696).
//
// THE PALM (HANDPALM_joint, parent of all five fingers) follows its pose's authored frame the same way, by the
// grip, ONLY in a pose where it carries no straight finger: measured in the take (walk_poses.py), the fist's
// palm would carry the pointing index 47 mm (right) / 33 mm (left) and the raised thumb 24 / 21 mm -- the swing
// the owner saw in the headset. kPoseSources says per pose whether the palm is driven; walk_poses.py checks it.
//
//===========================================================================

enum
{
	POSE_FIST = 0,		// index on the trigger, thumb on a surface
	POSE_POINT,			// index off, thumb on
	POSE_THUMBUP,		// index on, thumb off
	POSE_FINGERGUN,		// index off, thumb off
	POSE_COUNT
};

inline constexpr float kIndexTouchRest = 0.3f;	// UZDXREMA VAP :444: a finger resting on the trigger, not pulling

// What the controller says, as a pose and how far toward it each finger (and the palm) goes.
struct PoseInputs
{
	int   Pose = POSE_FIST;
	float Amount[FINGER_COUNT] = {};
	float Palm = 0.f;
};

inline void PoseRule(const Inputs &h, PoseInputs &out)
{
	const bool indexOn = h.triggerTouch || h.trigger > 0.f;
	const bool thumbOn = h.thumbTouch;
	out.Pose = indexOn ? (thumbOn ? POSE_FIST : POSE_THUMBUP) : (thumbOn ? POSE_POINT : POSE_FINGERGUN);
	const float grip = Clamp01(h.grip);
	for (int f = 0; f < FINGER_COUNT; ++f) out.Amount[f] = grip;
	out.Amount[FINGER_INDEX] = indexOn ? (h.trigger > 0.f ? Clamp01(h.trigger) : kIndexTouchRest) : grip;
	out.Palm = grip;
	if (!h.valid)
	{
		for (int f = 0; f < FINGER_COUNT; ++f) out.Amount[f] = 0.f;
		out.Palm = 0.f;
	}
}

// One hand's poses, built once per model and side (BuildPoses): every finger joint's rotation in each pose and in
// the open pose, and the palm's, in the joint's canonical frame (the palm: its own axes), relative to the frame the
// model draws.
struct HandPoses
{
	float Open[FINGER_COUNT][FINGER_JOINTS][4] = {};
	float Target[POSE_COUNT][FINGER_COUNT][FINGER_JOINTS][4] = {};
	float PalmOpen[4] = { 0.f, 0.f, 0.f, 1.f };
	float PalmTarget[POSE_COUNT][4] = {};
	bool  PalmDriven[POSE_COUNT] = {};
	bool  Have = false;
};

// The targets for one set of inputs: QSlerp(open, pose, amount) per joint; the palm likewise where its pose drives
// it, else its open rotation. `poses` null (no authored poses on this model): identity, the drawn frame.
inline void PoseTargets(const HandPoses *poses, const PoseInputs &pi, float target[FINGER_COUNT][FINGER_JOINTS][4], float palm[4])
{
	for (int f = 0; f < FINGER_COUNT; ++f)
		for (int j = 0; j < FINGER_JOINTS; ++j)
		{
			if (poses != nullptr && poses->Have) QSlerp(poses->Open[f][j], poses->Target[pi.Pose][f][j], pi.Amount[f], target[f][j]);
			else { target[f][j][0] = target[f][j][1] = target[f][j][2] = 0.f; target[f][j][3] = 1.f; }
		}
	palm[0] = palm[1] = palm[2] = 0.f; palm[3] = 1.f;
	if (poses != nullptr && poses->Have)
	{
		if (poses->PalmDriven[pi.Pose]) QSlerp(poses->PalmOpen, poses->PalmTarget[pi.Pose], pi.Palm, palm);
		else for (int k = 0; k < 4; ++k) palm[k] = poses->PalmOpen[k];
	}
}

// The palm's drawn rotation, smoothed exactly as each finger joint's (Smooth).
struct PalmState
{
	float Q[4] = { 0.f, 0.f, 0.f, 1.f };
	bool  Init = false;
};

// ONE SIDE, ONE DISPLAYED FRAME: the rule, the targets, and UZDXREMA's smoothing toward them.
inline void StepPose(SideState &rot, PalmState &palm, const Inputs &in, float retain, const HandPoses *poses)
{
	PoseInputs pi;
	PoseRule(in, pi);
	float target[FINGER_COUNT][FINGER_JOINTS][4], pt[4];
	PoseTargets(poses, pi, target, pt);
	Smooth(rot, target, retain);
	if (!palm.Init) { for (int k = 0; k < 4; ++k) palm.Q[k] = pt[k]; }
	else { float o[4]; QSlerp(palm.Q, pt, 1.f - retain, o); for (int k = 0; k < 4; ++k) palm.Q[k] = o[k]; }
	palm.Init = true;
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

//===========================================================================
//
// THE CANONICAL BONE FRAME OF EVERY FINGER JOINT. MA :365-377 (the palm normal) and :476-555 (the
// frame), in doubles, on the model's REST pose, at load instead of offline:
//
//     +X  along the bone toward its child (the child's own translation, already in this joint's
//         frame); the child furthest from the joint when there are several; a joint with no child
//         borrows the joint before it's X ("tipaxis")
//     +Z  = unit(X x palm_l), palm_l the palm normal brought into the joint's frame by the
//         TRANSPOSE of its global 3x3 (MA :509, as written: equal to the inverse for a rotation
//         with any +-1 scale)
//     X re-squared = unit(Z x unit(X x Z));  +Y = Z x X (palmward)
//     refused: det off 1 by more than 1e-3; Y . palm_l <= 0.05 (the finger would fold backwards)
//
// The palm normal (MA :366-377): f = unit(middle_1 - hand), k = (index_1 - pinky_1) squared off f,
// palm = unit(k x f) on the right hand, unit(f x k) on the left. The hand joint and the joints'
// roles are mkfbxposes.py's for hand_final.fbx: hand = Root_joint, finger 1/2/3 = BASE/MID/TOP.
//
//===========================================================================

struct RestJoint
{
	int    parent = -1;
	double G[16] = {};		// rest global, column-major: rotation * scale, position (MA global_transforms)
	double t[3] = {};		// rest LOCAL translation (MA model.joints[i].t)
};

struct FingerFrame
{
	double X[3] = {}, Y[3] = {}, Z[3] = {};
	bool   borrowed = false;	// MA "tipaxis"
	double fold = 0;		// Y . palm_l (MA :549)
};

namespace Detail
{
	inline void Sub3(const double a[3], const double b[3], double o[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
	inline double Dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
	inline void Cross3(const double a[3], const double b[3], double o[3])
	{
		const double x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
		o[0] = x; o[1] = y; o[2] = z;
	}
	// MA unit(): None below 1e-9
	inline bool Unit3(const double a[3], double o[3])
	{
		const double L = sqrt(Dot3(a, a));
		if (L < 1e-9) return false;
		o[0] = a[0] / L; o[1] = a[1] / L; o[2] = a[2] / L;
		return true;
	}
	inline void Pos3(const RestJoint &j, double o[3]) { o[0] = j.G[12]; o[1] = j.G[13]; o[2] = j.G[14]; }
}

// MA :366-377. False (with why) where MA dies.
inline bool MeasurePalm(const std::vector<RestJoint> &J, int hand, int index1, int middle1, int pinky1, bool right,
	double palm[3], char *why, size_t whyLen)
{
	using namespace Detail;
	double h[3], m[3], ix[3], pk[3], f[3], kr[3], k[3];
	Pos3(J[hand], h); Pos3(J[middle1], m); Pos3(J[index1], ix); Pos3(J[pinky1], pk);
	Sub3(m, h, f);
	if (!Unit3(f, f)) { snprintf(why, whyLen, "the hand has no finger direction"); return false; }
	Sub3(ix, pk, kr);
	const double d = Dot3(kr, f);
	const double kk[3] = { kr[0] - f[0] * d, kr[1] - f[1] * d, kr[2] - f[2] * d };
	if (!Unit3(kk, k)) { snprintf(why, whyLen, "index_1 and pinky_1 are on the finger axis -- no knuckle line"); return false; }
	double c[3];
	if (right) Cross3(k, f, c); else Cross3(f, k, c);
	if (!Unit3(c, palm)) { snprintf(why, whyLen, "zero palm normal"); return false; }
	return true;
}

// MA :476-555 for one hand. joints[f][j] = the model joint of finger f, joint j. False (with why) where
// MA dies or errs.
inline bool MeasureFingerFrames(const std::vector<RestJoint> &J, const int joints[FINGER_COUNT][FINGER_JOINTS],
	const double palm[3], FingerFrame out[FINGER_COUNT][FINGER_JOINTS], char *why, size_t whyLen)
{
	using namespace Detail;
	static const char *const names[FINGER_COUNT] = { "thumb", "index", "middle", "ring", "pinky" };
	const int n = (int)J.size();
	for (int f = 0; f < FINGER_COUNT; ++f)
	{
		for (int j = 0; j < FINGER_JOINTS; ++j)
		{
			const int idx = joints[f][j];
			if (idx < 0 || idx >= n) { snprintf(why, whyLen, "%s joint %d is missing", names[f], j + 1); return false; }

			// X: toward this bone's own child, the child furthest from the joint (MA :490-505)
			double X[3];
			bool haveX = false;
			int best = -1;
			double bestLen = -1;
			for (int c = 0; c < n; ++c)
			{
				if (J[c].parent != idx) continue;
				const double L = sqrt(Dot3(J[c].t, J[c].t));
				if (L > bestLen) { bestLen = L; best = c; }	// first of equals, as Python's max()
			}
			if (best >= 0) haveX = Unit3(J[best].t, X);
			bool borrowed = false;
			if (!haveX)
			{
				if (j == 0) { snprintf(why, whyLen, "%s joint 1 has no child joint and nothing to take a bone axis from", names[f]); return false; }
				memcpy(X, out[f][j - 1].X, sizeof(X));
				borrowed = true;
			}

			// palm normal, brought into this joint's own frame (MA :508-511): Gr^T palm
			const double *G = J[idx].G;
			const double pl[3] = { G[0] * palm[0] + G[1] * palm[1] + G[2] * palm[2],
								   G[4] * palm[0] + G[5] * palm[1] + G[6] * palm[2],
								   G[8] * palm[0] + G[9] * palm[1] + G[10] * palm[2] };
			double palm_l[3];
			if (!Unit3(pl, palm_l)) { snprintf(why, whyLen, "%s joint %d: the palm normal came out zero in its own frame", names[f], j + 1); return false; }

			// Z = X x palm_l (MA :529), X re-squared (:536), Y = Z x X (:539)
			double Z[3], t[3], u[3];
			Cross3(X, palm_l, t);
			if (!Unit3(t, Z)) { snprintf(why, whyLen, "%s joint %d folds about nothing (bone axis parallel to the palm normal)", names[f], j + 1); return false; }
			Cross3(X, Z, t);
			if (!Unit3(t, u)) { snprintf(why, whyLen, "%s joint %d: could not square its bone axis", names[f], j + 1); return false; }
			Cross3(Z, u, t);
			if (!Unit3(t, X)) { snprintf(why, whyLen, "%s joint %d: could not square its bone axis against the flex axis", names[f], j + 1); return false; }
			double Y[3];
			Cross3(Z, X, Y);

			// MA :542-552
			double yz[3];
			Cross3(Y, Z, yz);
			const double det = Dot3(X, yz);
			if (fabs(det - 1.0) > 1e-3) { snprintf(why, whyLen, "%s joint %d's bone frame is not right-handed (det %.4f)", names[f], j + 1, det); return false; }
			const double fold = Dot3(Y, palm_l);
			if (fold <= 0.05) { snprintf(why, whyLen, "%s joint %d folds AWAY from its palm (Y.palm = %+.3f)", names[f], j + 1, fold); return false; }

			FingerFrame &F = out[f][j];
			memcpy(F.X, X, sizeof(X)); memcpy(F.Y, Y, sizeof(Y)); memcpy(F.Z, Z, sizeof(Z));
			F.borrowed = borrowed;
			F.fold = fold;
		}
	}
	return true;
}

//===========================================================================
//
// A JOINT'S POSE ROTATION FROM TWO FRAMES OF THE FBX. MKP main():
//
//     D = local(ref)^T . local(pose)          the joint's local rotation away from the reference
//     q = quat(F^T . D . F)                   in its canonical bone frame; w >= 0 (MKP quat())
//
// MKP's local is ortho(parent global)^T . ortho(global) in Blender's armature space; here it is the
// joint's local TRS rotation from this engine's own loader, the same rotation when the scales along
// the chain are uniform (tools/fbxload_test checks the result against the owner's vrhandpose.txt,
// generated by MKP). MKP's reference is the FBX's open frame; here it is the frame the model DRAWS
// (frame 0 = rest), so the engine's turn D lands on the drawn joint as VAP applies a pose to a
// body's rest. When the open frame is frame 0 the two are the same.
//
//===========================================================================

namespace Detail
{
	// row-major R[i][j], the rotation of a unit quaternion (x, y, z, w) (MA quat_to_mat, transposed to rows)
	inline void QuatToRows(const double qi[4], double R[3][3])
	{
		double x = qi[0], y = qi[1], z = qi[2], w = qi[3];
		const double n = sqrt(x * x + y * y + z * z + w * w);
		if (n < 1e-12) { x = y = z = 0; w = 1; } else { x /= n; y /= n; z /= n; w /= n; }
		R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - z * w);     R[0][2] = 2 * (x * z + y * w);
		R[1][0] = 2 * (x * y + z * w);     R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - x * w);
		R[2][0] = 2 * (x * z - y * w);     R[2][1] = 2 * (y * z + x * w);     R[2][2] = 1 - 2 * (x * x + y * y);
	}
	// MKP quat(), verbatim
	inline void RowsToQuat(const double r[3][3], double q[4])
	{
		double x, y, z, w;
		const double t = r[0][0] + r[1][1] + r[2][2];
		if (t > 0)
		{
			const double s = sqrt(t + 1.0) * 2; w = 0.25 * s;
			x = (r[2][1] - r[1][2]) / s; y = (r[0][2] - r[2][0]) / s; z = (r[1][0] - r[0][1]) / s;
		}
		else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
		{
			const double s = sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2; w = (r[2][1] - r[1][2]) / s;
			x = 0.25 * s; y = (r[0][1] + r[1][0]) / s; z = (r[0][2] + r[2][0]) / s;
		}
		else if (r[1][1] > r[2][2])
		{
			const double s = sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2; w = (r[0][2] - r[2][0]) / s;
			x = (r[0][1] + r[1][0]) / s; y = 0.25 * s; z = (r[1][2] + r[2][1]) / s;
		}
		else
		{
			const double s = sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2; w = (r[1][0] - r[0][1]) / s;
			x = (r[0][2] + r[2][0]) / s; y = (r[1][2] + r[2][1]) / s; z = 0.25 * s;
		}
		if (w < 0) { x = -x; y = -y; z = -z; w = -w; }
		q[0] = x; q[1] = y; q[2] = z; q[3] = w;
	}
}

inline void PoseRotation(const FingerFrame &F, const double qRef[4], const double qPose[4], double out[4])
{
	using namespace Detail;
	double A[3][3], B[3][3], D[3][3], Fm[3][3], T1[3][3], C[3][3];
	QuatToRows(qRef, A);
	QuatToRows(qPose, B);
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
		{
			D[i][j] = A[0][i] * B[0][j] + A[1][i] * B[1][j] + A[2][i] * B[2][j];	// A^T B
			Fm[i][j] = (j == 0 ? F.X : j == 1 ? F.Y : F.Z)[i];					// columns X, Y, Z
		}
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			T1[i][j] = D[i][0] * Fm[0][j] + D[i][1] * Fm[1][j] + D[i][2] * Fm[2][j];	// D F
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			C[i][j] = Fm[0][i] * T1[0][j] + Fm[1][i] * T1[1][j] + Fm[2][i] * T1[2][j];	// F^T (D F)
	RowsToQuat(C, out);
}

//===========================================================================
//
// THE TURN ON THE JOINTS. VAP :1771-1838, on the joint globals in the model file's space:
//
//     q = gRot[side][f][j];  |q.w| > 0.999999 -> no turn
//     D = F . R(q) . F^T                       (F's columns: the canonical axes in joint space;
//                                               Dm.Col[c] = F.Apply(R.Apply(F.Unapply(e_c))))
//     after = before . D;  carry = after . before^-1
//     every joint below it: global = carry . global
//
// in finger order, joint 1 to 3, so a joint is turned after its parent has carried it.
//
// "EVERY JOINT BELOW IT" (Below[f][j]): VAP carries the finger's own later joints (:1827-1834) on
// the ground its comment gives, "turning a joint moves everything below it ... finger chains are a
// straight line, so the rest of the finger is just the joints after this one". hand_final.fbx's
// fingers end in an _UP_TOP_joint below joint 3, skinned, which no finger role names; carried by
// VAP's literal loop it would stay behind and stretch the fingertip. Below[] is every descendant,
// which for joints 1-3 is exactly VAP's set, plus the tip.
//
// WHY THE SOURCE'S MATRICES AND NOT ModelReach::FJointPoseWork. FJointPoseWork edits a joint's LOCAL
// TRS and re-composes; replacing L by L.D through a TRS is exact only when the joint's local scale
// commutes with D. VAP turns the GLOBAL matrix, so this does the same, then converts back exactly as
// VAP does (VRRig_PaletteToGlobal / VRRig_GlobalToPalette, below).
//
//===========================================================================

struct HandRig
{
	int Joint[FINGER_COUNT][FINGER_JOINTS] = {};
	float Frame[FINGER_COUNT][FINGER_JOINTS][3][3] = {};	// [f][j][column X/Y/Z][component]
	std::vector<int> Below[FINGER_COUNT][FINGER_JOINTS];	// every descendant, ascending
	int Palm = -1;											// HANDPALM_joint (BuildHand); -1 = none
	std::vector<int> PalmBelow;								// every descendant of it, ascending
};

namespace Detail
{
	// out = a . b, column-major (VSMatrix::multMatrix)
	inline void Mul4(const float a[16], const float b[16], float out[16])
	{
		float r[16];
		for (int c = 0; c < 4; ++c)
			for (int rr = 0; rr < 4; ++rr)
				r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] + a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
		memcpy(out, r, sizeof(r));
	}

	// General 4x4 inverse (cofactors, in double). False when singular.
	inline bool Inverse4(const float mf[16], float out[16])
	{
		double m[16], inv[16];
		for (int i = 0; i < 16; ++i) m[i] = mf[i];
		inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
		inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
		inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
		inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
		inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
		inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
		inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
		inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
		inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
		inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
		inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
		inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
		inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
		inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
		inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
		inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
		const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
		if (det == 0.0 || !std::isfinite(det)) return false;
		for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] / det);
		return true;
	}

	inline constexpr float kSwap[16] = { 1, 0, 0, 0,  0, 0, 1, 0,  0, 1, 0, 0,  0, 0, 0, 1 };
}

// UZDXREMA vr_rig.cpp:1136-1151 VRRig_PaletteToGlobal(swapYZ = true): G = S . P . S . B
inline void PaletteToGlobal(const float pal[16], const float base[16], float out[16])
{
	float t[16];
	Detail::Mul4(Detail::kSwap, pal, t);
	Detail::Mul4(t, Detail::kSwap, t);
	Detail::Mul4(t, base, out);
}

// UZDXREMA vr_rig.cpp:1455 VRRig_GlobalToPalette(swapYZ = true): P = S . G . B^-1 . S
inline void GlobalToPalette(const float g[16], const float invBase[16], float out[16])
{
	float t[16];
	Detail::Mul4(Detail::kSwap, g, t);
	Detail::Mul4(t, invBase, t);
	Detail::Mul4(t, Detail::kSwap, out);
}

namespace Detail
{
	// VAP :1799-1834 for one joint: D = F . R(q) . F^T (Fm[c][k]: column c of the canonical frame), after = before . D,
	// everything in `below` carried by after . before^-1. The caller has already skipped |q.w| > 0.999999.
	inline void TurnJoint(float (*G)[16], int n, int jt, const float (*Fm)[3], const float q[4], const std::vector<int> &below,
		std::vector<char> &moved)
	{
		// Rq (VAP :1799-1805), F (:1806-1807), Dm.Col[c] = F.Apply(Rq.Apply(F.Unapply(e_c))) (:1808-1813)
		const float x = q[0], y = q[1], z = q[2], w = q[3];
		const float Rq[3][3] =
		{
			{ 1.f - 2.f*(y*y + z*z), 2.f*(x*y + w*z), 2.f*(x*z - w*y) },
			{ 2.f*(x*y - w*z), 1.f - 2.f*(x*x + z*z), 2.f*(y*z + w*x) },
			{ 2.f*(x*z + w*y), 2.f*(y*z - w*x), 1.f - 2.f*(x*x + y*y) },
		};
		float D[16] = { 0 };
		for (int c3 = 0; c3 < 3; ++c3)
		{
			const float e[3] = { c3 == 0 ? 1.f : 0.f, c3 == 1 ? 1.f : 0.f, c3 == 2 ? 1.f : 0.f };
			float u[3], v[3];
			for (int k = 0; k < 3; ++k) u[k] = Fm[k][0] * e[0] + Fm[k][1] * e[1] + Fm[k][2] * e[2];		// Unapply
			for (int k = 0; k < 3; ++k) v[k] = Rq[0][k] * u[0] + Rq[1][k] * u[1] + Rq[2][k] * u[2];		// Apply
			for (int k = 0; k < 3; ++k) D[c3 * 4 + k] = Fm[0][k] * v[0] + Fm[1][k] * v[1] + Fm[2][k] * v[2];	// Apply
		}
		D[15] = 1.f;	// FromMat3Pos(Dm, 0)

		float before[16], after[16];
		memcpy(before, G[jt], sizeof(before));
		Mul4(before, D, after);
		memcpy(G[jt], after, sizeof(after));
		moved[jt] = 1;

		// Carry the rest of this finger with it (VAP :1821-1834)
		float invBefore[16];
		if (!Inverse4(before, invBefore)) return;
		float carry[16];
		Mul4(after, invBefore, carry);
		for (int d : below)
		{
			if (d < 0 || d >= n) continue;
			Mul4(carry, G[d], G[d]);
			moved[d] = 1;
		}
	}

	// VAP :1785-1836 without clearing `moved` (PosePalette runs the palm first).
	inline void TurnFingers(float (*G)[16], int n, const HandRig &rig, const SideState &rot, std::vector<char> &moved)
	{
		for (int f = 0; f < FINGER_COUNT; ++f)
		{
			for (int j = 0; j < FINGER_JOINTS; ++j)
			{
				const float *q = rot.Q[f][j];
				if (fabsf(q[3]) > 0.999999f) continue;   // no turn
				const int jt = rig.Joint[f][j];
				if (jt < 0 || jt >= n) continue;
				TurnJoint(G, n, jt, rig.Frame[f][j], q, rig.Below[f][j], moved);
			}
		}
	}
}

// VAP :1785-1836 on `G` (n joint globals). `moved[i]` is set for every joint written.
inline void ApplyFingerTurns(float (*G)[16], int n, const HandRig &rig, const SideState &rot, std::vector<char> &moved)
{
	moved.assign(n, 0);
	Detail::TurnFingers(G, n, rig, rot, moved);
}

// The palm's turn (StepPose, PalmState): the same turn as a finger joint's, in the palm joint's own axes (identity frame:
// D = R(q), q = the palm's local rotation away from the drawn frame), carrying every joint below it. Does not clear
// `moved`.
inline void ApplyPalmTurn(float (*G)[16], int n, const HandRig &rig, const float q[4], std::vector<char> &moved)
{
	if (q == nullptr || rig.Palm < 0 || rig.Palm >= n) return;
	if (fabsf(q[3]) > 0.999999f) return;   // no turn
	static const float kIdentity[3][3] = { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } };
	Detail::TurnJoint(G, n, rig.Palm, kIdentity, q, rig.PalmBelow, moved);
}

// The whole draw: palette -> globals, the palm cup's turn (palmQ: the PalmState's rotation, null = none), the
// fingers' turns, globals -> palette for every joint that moved, the rest copied. base/invBase: the model's
// rest (IQM baseframe and its inverse). `scratch` is reused.
inline void PosePalette(const float (*pal)[16], const float (*base)[16], const float (*invBase)[16], int n,
	const HandRig &rig, const SideState &rot, float (*out)[16], std::vector<float> &scratch, std::vector<char> &moved,
	const float *palmQ = nullptr)
{
	scratch.resize((size_t)n * 16);
	float (*G)[16] = reinterpret_cast<float (*)[16]>(scratch.data());
	for (int i = 0; i < n; ++i) PaletteToGlobal(pal[i], base[i], G[i]);
	moved.assign(n, 0);
	ApplyPalmTurn(G, n, rig, palmQ, moved);
	Detail::TurnFingers(G, n, rig, rot, moved);
	for (int i = 0; i < n; ++i)
	{
		if (moved[i]) GlobalToPalette(G[i], invBase[i], out[i]);
		else memcpy(out[i], pal[i], sizeof(float) * 16);
	}
}

//===========================================================================
//
// THE DXR HANDS (hand_final.fbx), as mkfbxposes.py reads them: the hand joint, the three joints of
// each finger (FBX BASE/MID/TOP = joints 1/2/3; UP_TOP is a tip with no counterpart: it never turns in
// the take, tools/fbxload_test), and the palm joint.
//
// FRAME NUMBERS. The owner's fbx_poses.json (UZDXREMA tools/avatar, read by mkfbxposes.py) gives BLENDER
// frame numbers: Blender's FBX importer puts the file's time 0 at frame 1. This engine's frame k is the
// take at k / 60 s (fbx_skinned.cpp), so Blender n = take frame n - 1 (checked, not assumed: tools/
// fbxload_test F2 compares the rotations made from take 0/318 with the ones mkfbxposes.py wrote from
// Blender 1/319; 317 and 319 are off by 1.3-3.6 degrees).
//
//===========================================================================

inline constexpr const char *kHandJoint = "Root_joint";
inline constexpr const char *kFingerJointNames[FINGER_COUNT][FINGER_JOINTS] =
{
	{ "THUMB_BASE_joint",    "THUMB_MID_joint",    "THUMB_TOP_joint" },
	{ "INDEX_BASE_joint",    "INDEX_MID_joint",    "INDEX_TOP_joint" },
	{ "MIDDLE_F_BASE_joint", "MIDDLE_F_MID_joint", "MIDDLE_F_TOP_joint" },
	{ "RING_BASE_joint",     "RING_MID_joint",     "RING_TOP_joint" },
	{ "PINK_BASE_joint",     "PINK_MID_joint",     "PINK_TOP_joint" },
};
inline constexpr const char *kPalmJoint = "HANDPALM_joint";	// parent of every finger's first joint
inline constexpr int kDrawnFrame = 0;		// the frame the hands draw (MODELDEF FrameIndex ... 0)
inline constexpr int kOpenFrame = 0;		// fbx_poses.json "open" 1 (Blender), both hands; = the rest the model draws

//===========================================================================
//
// WHERE EACH POSE COMES FROM, per hand, from ITS OWN take (Hand_Low = the right hand's skeleton, the owner's
// "A"; Hand_Low1 = the left's, "B"). Each finger is taken WHOLE (its three joints) from one authored take frame,
// as the owner's composed poses in fbx_poses.json are; Palm is the palm joint's frame, or -1: not driven.
// Chosen by walking both takes frame by frame (tools/fbxload_test/walk_poses.py, which prints every number
// below); take frame = Blender - 1. Neither take has a whole frame that is a one-finger point, a thumbs-up or a
// finger gun with the middle finger closed, so those are composed.
//
//   fist      318 (the owner's "fist" 319), palm included. Its four fingertips' summed distance to the palm is
//             within 5.2% (right) / 3.2% (left) of the take's tightest fist, 688 -- a 2-frame pass at ~7 degrees a
//             frame on the way out of the fist hold; no finger is straight in it, so its palm is driven.
//   point     the index from the take's held, straightest index, the rest the fist's
//               right: 808 -- the finger-gun hold the owner's "fingergun" 803 (take 802) sits in. 802 itself is
//                      mid-fire (the index 14.8 degrees at the knuckle, 6.5 more than the hold, the thumb dropping);
//                      808 is the hold it returns to, identical to the pose before the shot (798) within 0.1 degree
//               left:  0 -- the left take holds a straight index only in its open frames (0-112, 700-797)
//   thumbup   the thumb from the take's held thumb furthest from the finger axis (straight >= 0.95, its tip not
//             across the fingers), the rest the fist's
//               right: 990 -- the owner's "twohand_gun" 985 (take 984) hold: 53.5 degrees off the finger axis.
//                      984 itself is a twitch (the thumb 3.5 degrees off the hold, 1.26 degrees a frame)
//               left:  205 -- the owner's "curl" 206: 50.2 degrees (held 155-283). The owner's "thumb_out" 944
//                      (take 943) lies along the fingers (16 degrees off, straightness 0.945)
//   fingergun the index and thumb out, the rest closed
//               right: 808 for thumb, index, ring and pinky (the owner's finger gun, held); the middle from the
//                      fist -- the take's finger gun has the middle straight beside the index
//               left:  the index from 0 and the thumb from 205 (as point and thumbup), the rest the fist's
//
//===========================================================================

struct PoseSource
{
	int Finger[FINGER_COUNT];	// thumb, index, middle, ring, pinky: take frame
	int Palm;					// take frame, or -1: the palm stays in the drawn frame
};

inline constexpr PoseSource kPoseSources[SIDE_COUNT][POSE_COUNT] =
{
	{	// RIGHT: Hand_Low
		{ { 318, 318, 318, 318, 318 }, 318 },	// POSE_FIST
		{ { 318, 808, 318, 318, 318 },  -1 },	// POSE_POINT
		{ { 990, 318, 318, 318, 318 },  -1 },	// POSE_THUMBUP
		{ { 808, 808, 318, 808, 808 },  -1 },	// POSE_FINGERGUN
	},
	{	// LEFT: Hand_Low1
		{ { 318, 318, 318, 318, 318 }, 318 },	// POSE_FIST
		{ { 318,   0, 318, 318, 318 },  -1 },	// POSE_POINT
		{ { 205, 318, 318, 318, 318 },  -1 },	// POSE_THUMBUP
		{ { 205,   0, 318, 318, 318 },  -1 },	// POSE_FINGERGUN
	},
};

// The last take frame any pose reads, per side: a model whose take is shorter is refused (BuildPoses).
inline int PoseSourceLastFrame(int side)
{
	int last = kOpenFrame;
	for (int p = 0; p < POSE_COUNT; ++p)
	{
		for (int f = 0; f < FINGER_COUNT; ++f) last = kPoseSources[side][p].Finger[f] > last ? kPoseSources[side][p].Finger[f] : last;
		last = kPoseSources[side][p].Palm > last ? kPoseSources[side][p].Palm : last;
	}
	return last;
}

//===========================================================================
//
// ONE HAND, BUILT ONCE PER MODEL AND SIDE: the palm normal and the canonical frames (MA), and the turn's rig
// (the joints, their frames as floats, everything below each, the palm joint and everything below it).
//
//===========================================================================

inline bool BuildHand(const std::vector<RestJoint> &J, int hand, int palmJoint, const int joints[FINGER_COUNT][FINGER_JOINTS], bool right,
	HandRig &rig, FingerFrame frames[FINGER_COUNT][FINGER_JOINTS], double palm[3], char *why, size_t whyLen)
{
	const int n = (int)J.size();
	if (hand < 0 || hand >= n) { snprintf(why, whyLen, "no hand joint"); return false; }
	if (!MeasurePalm(J, hand, joints[FINGER_INDEX][0], joints[FINGER_MIDDLE][0], joints[FINGER_PINKY][0], right, palm, why, whyLen)) return false;
	if (!MeasureFingerFrames(J, joints, palm, frames, why, whyLen)) return false;
	auto below = [&](int jt, std::vector<int> &out) {
		out.clear();
		for (int i = 0; i < n; ++i)
		{
			int guard = 0;
			for (int p = J[i].parent; p >= 0 && guard < 4096; p = J[p].parent, ++guard)
				if (p == jt) { out.push_back(i); break; }
		}
	};
	for (int f = 0; f < FINGER_COUNT; ++f)
	{
		for (int j = 0; j < FINGER_JOINTS; ++j)
		{
			const int jt = joints[f][j];
			rig.Joint[f][j] = jt;
			const FingerFrame &F = frames[f][j];
			for (int k = 0; k < 3; ++k)
			{
				rig.Frame[f][j][0][k] = (float)F.X[k];
				rig.Frame[f][j][1][k] = (float)F.Y[k];
				rig.Frame[f][j][2][k] = (float)F.Z[k];
			}
			below(jt, rig.Below[f][j]);
		}
	}
	rig.Palm = (palmJoint >= 0 && palmJoint < n) ? palmJoint : -1;
	if (rig.Palm >= 0) below(rig.Palm, rig.PalmBelow);
	else rig.PalmBelow.clear();
	return true;
}

//===========================================================================
//
// THE POSES OF ONE HAND (kPoseSources[side]), built once per model and side: every finger joint's rotation in
// each pose and in the open frame, by mkfbxposes.py's formula (PoseRotation) relative to the frame the model
// draws; the palm's likewise in its own axes (identity frame: q = quat(local(drawn)^T . local(pose)); with the
// palm's uniform local scale, turning its global by q gives it exactly that frame's local rotation).
// getQ(frame, joint, q[4]) gives a joint's LOCAL rotation in a take frame. False (with why) when the take is too
// short for the table or the palm joint is missing where a pose drives it.
//
//===========================================================================

template<class GetQ>
inline bool BuildPoses(const HandRig &rig, const FingerFrame frames[FINGER_COUNT][FINGER_JOINTS], int side, int numFrames,
	GetQ getQ, HandPoses &out, char *why, size_t whyLen)
{
	out = HandPoses();
	if (side < 0 || side >= SIDE_COUNT) { snprintf(why, whyLen, "no such side %d", side); return false; }
	const int last = PoseSourceLastFrame(side);
	if (numFrames <= last || numFrames <= kDrawnFrame)
	{
		snprintf(why, whyLen, "the take has %d frames; the poses read frames up to %d", numFrames, last);
		return false;
	}
	auto rot = [&](const FingerFrame &F, int joint, int frame, float q[4]) {
		double qd[4], qp[4], r[4];
		getQ(kDrawnFrame, joint, qd);
		getQ(frame, joint, qp);
		PoseRotation(F, qd, qp, r);
		for (int k = 0; k < 4; ++k) q[k] = (float)r[k];
	};
	for (int f = 0; f < FINGER_COUNT; ++f)
		for (int j = 0; j < FINGER_JOINTS; ++j)
		{
			const int jt = rig.Joint[f][j];
			rot(frames[f][j], jt, kOpenFrame, out.Open[f][j]);
			for (int p = 0; p < POSE_COUNT; ++p) rot(frames[f][j], jt, kPoseSources[side][p].Finger[f], out.Target[p][f][j]);
		}
	FingerFrame I;
	I.X[0] = 1; I.Y[1] = 1; I.Z[2] = 1;
	for (int p = 0; p < POSE_COUNT; ++p)
	{
		const int src = kPoseSources[side][p].Palm;
		out.PalmDriven[p] = src >= 0;
		if (src < 0) continue;
		if (rig.Palm < 0) { snprintf(why, whyLen, "pose %d drives %s, which the model lacks", p, kPalmJoint); return false; }
		rot(I, rig.Palm, src, out.PalmTarget[p]);
	}
	if (rig.Palm >= 0) rot(I, rig.Palm, kOpenFrame, out.PalmOpen);
	out.Have = true;
	return true;
}


//===========================================================================
//
// A HAND HOLDING SOMETHING: UZDXREMA vr_avatar_pose.cpp ("VAP") :466-656 (which pose shapes a hand round a drawn
// weapon or a held object), :667-684 (that pose as the joint's rotation target), and the poses it looks up --
// the owner's tools/avatar/fbx_poses.json, the table mkfbxposes.py turned into wadsrc/static/vrhandpose.txt.
// The checklist (source line -> here) is items 16-17 and 48-75 at the top of vr_handpose.cpp.
//
// EACH HAND FROM ITS OWN TAKE. mkfbxposes.py writes a pose one FBX hand authored for the OTHER side too, mirrored
// ("A pose only one hand authored is also written for the other side, mirrored, and says so"). That is never done
// here (owner: two separate hands, never cloned or mirrored): a side has a pose only when its own take has every
// frame the pose reads. Measured (tools/fbxload_test/walk_poses.py, check_hold_table): the right take (Hand_Low,
// the owner's "A") holds its two-finger gun from about frame 760 to the end while the left take wraps, cups and
// supports, so the right take has no wrap_half, wrap_tight, cup, thumb_out or twohand_support frame anywhere, and
// the left take has no twofinger_point, fingergun or twohand_gun frame. A pose a side lacks is ABSENT for that
// side, exactly as VAP treats a pose a table does not carry ("ADDITIVE AND SILENT IF ABSENT", :577-579): the hand
// keeps the controller's pose.
//
//===========================================================================

// UZDXREMA vk_openxrdevice.h:105-131 EGripSubject: the same names, the same values (tools/hands_test checks them
// against the source's own enum, cut out of that header).
enum EGripSubject
{
	GRIPSUBJ_None = 0,
	GRIPSUBJ_Round,      // one pistol or rifle cartridge -- fingertip pinch
	GRIPSUBJ_Shell,      // a shotgun shell -- fatter, and a fuller grip
	GRIPSUBJ_Inserting,  // that round being pushed home, thumb driving it
	GRIPSUBJ_Magazine,   // magazine, clip or speedloader -- wrapped in the palm
	GRIPSUBJ_Grip,       // a pistol grip: a one-handed gun, or a longarm's firing hand
	GRIPSUBJ_Forend,     // pump or handguard -- a fat cylinder across the palm
	GRIPSUBJ_Foregrip,   // vertical foregrip, as on an SMG
	GRIPSUBJ_Slide,      // slide or charging handle -- pinched from the sides
	GRIPSUBJ_Support,    // supporting the OTHER hand's weapon, wrapped round its fist
	GRIPSUBJ_Holster,    // inside a WEAPON holster volume: reaching, not yet holding
	GRIPSUBJ_Pouch,
	GRIPSUBJ_MAX
};

inline const char *GripSubjectName(int s)
{
	static const char *const n[GRIPSUBJ_MAX] = { "None", "Round", "Shell", "Inserting", "Magazine", "Grip", "Forend",
		"Foregrip", "Slide", "Support", "Holster", "Pouch" };
	return (s >= 0 && s < GRIPSUBJ_MAX) ? n[s] : "?";
}

// What one hand -- a ROLE, VAP's hidx (VR_MAINHAND 0 / VR_OFFHAND 1) -- has hold of this frame. UZDXREMA's rig answers
// these (vr_rig.cpp:1802-1853); here the game layer publishes them (vr_handpose.h VRHandPose_SetHeld).
struct HeldState
{
	bool weapon = false;			// VRRig_HandHoldsWeapon: a weapon is DRAWN in this hand
	std::string gripClass;			// VRRig_HandGripPose: its grip class ("pistol", ...); "" = NAME_None
	std::string propClass;			// VRRig_HandPropClass: its prop's class; "" = NAME_None
	int subject = GRIPSUBJ_None;	// VRRig_HandGripSubject
	// NOT IN THE SOURCE: a pose of the owner's table named directly, for a held thing whose design (docs/ITEM_DESIGN.md)
	// names an authored frame no subject's pose uses (the Lamp: fbx_curl). Taken as the subject's name is (VAP :610-655,
	// the same tries and the same "one pose wins whole"); "" = none.
	std::string pose;
	int item = 0;					// for the log only: what the game holds there (a DBASE100 index), 0 none
};

inline bool EqualNoCase(const char *a, const char *b)	// stricmp, as VRAvatar_NamedPose compares names (VA:323, 338)
{
	if (a == nullptr || b == nullptr) return false;
	for (; *a && *b; ++a, ++b)
	{
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
		if (x != y) return false;
	}
	return *a == *b;
}

//---------------------------------------------------------------------------
// THE OWNER'S POSE TABLE: UZDXREMA tools/avatar/fbx_poses.json, every entry, in its order (walk_poses.py
// check_hold_table reads both and refuses any difference).
//   raw       a Blender frame and the hand(s) that authored it: "both", "A" (Hand_Low, the right hand: MKP's "own
//             sides: hand A = R") or "B" (Hand_Low1, the left); take frame = Blender - 1 (checklist item 30).
//   composed  each finger WHOLE from a raw pose named in the table (MKP main(), "a finger is never assembled from
//             two frames; a hand may be").
// Palm: the raw pose HANDPALM_joint takes its frame from, or null: the palm stays in the drawn frame. NOT FROM THE
// SOURCE (UZDXREMA's avatars have no palm joint; mkfbxposes.py writes finger rotations only). Checklist item 38's rule,
// by its own reason: a palm must never carry a STRAIGHT finger authored in another frame (measured: the fist's palm
// would swing the pointing index 47 / 33 mm). So the palm takes the frame the middle finger comes from whenever every
// straight finger of the pose comes from that frame too: every whole frame, and every composed pose here but
// `pointing` (its straight index is fbx_fingergun's, its palm would be the fist's). Applied to kPoseSources, this is
// exactly item 38's table (fist driven; point, thumb up and finger gun not: walk_poses.py checks both). Item 38's
// wording ("no straight finger") cannot be used for the owner's frames: in fbx_curl (206) and fbx_halfcurl (193)
// the bend is the palm joint's own (59-64 deg) and the fingers on it are straight, so the fingers without their
// palm turn BACK from the hand -- a shape the take never has (fbxload_test renders, holds_<hand>.png).
//---------------------------------------------------------------------------

enum { HANDS_BOTH = 0, HANDS_A, HANDS_B };

struct OwnerPose
{
	const char *name;
	int blender;							// raw: the Blender frame; composed: 0
	int hands;								// raw: HANDS_*
	const char *finger[FINGER_COUNT];		// composed: thumb, index, middle, ring, pinky from these raw poses
	const char *palm;						// see above
};

#define VRHP_ALL(x) { x, x, x, x, x }
inline constexpr OwnerPose kOwnerPoses[] =
{
	// raw: FBX frames, each a whole hand
	{ "fbx_open",               1, HANDS_BOTH, {}, "fbx_open" },
	{ "fbx_relaxed",          154, HANDS_BOTH, {}, "fbx_relaxed" },
	{ "fbx_halfcurl",         193, HANDS_BOTH, {}, "fbx_halfcurl" },
	{ "fbx_curl",             206, HANDS_BOTH, {}, "fbx_curl" },
	{ "fbx_fist",             319, HANDS_BOTH, {}, "fbx_fist" },
	{ "fbx_crossed",          304, HANDS_BOTH, {}, "fbx_crossed" },
	{ "fbx_twofinger_point",  762, HANDS_A,    {}, "fbx_twofinger_point" },
	{ "fbx_fingergun",        803, HANDS_A,    {}, "fbx_fingergun" },
	{ "fbx_wrap_half",        820, HANDS_B,    {}, "fbx_wrap_half" },
	{ "fbx_wrap_tight",       837, HANDS_B,    {}, "fbx_wrap_tight" },
	{ "fbx_cup",              985, HANDS_B,    {}, "fbx_cup" },
	{ "fbx_thumb_out",        944, HANDS_B,    {}, "fbx_thumb_out" },
	{ "fbx_twohand_gun",      985, HANDS_A,    {}, "fbx_twohand_gun" },
	{ "fbx_twohand_support",  985, HANDS_B,    {}, "fbx_twohand_support" },
	{ "open",                   1, HANDS_BOTH, {}, "open" },
	{ "fist",                 319, HANDS_BOTH, {}, "fist" },
	// composed: the names the engine looks up (thumb, index, middle, ring, pinky)
	{ "gun_grip",             0, 0, { "fbx_wrap_tight", "fbx_wrap_half", "fbx_wrap_tight", "fbx_wrap_tight", "fbx_wrap_tight" }, "fbx_wrap_tight" },
	{ "grip_pistol",          0, 0, { "fbx_wrap_tight", "fbx_wrap_half", "fbx_wrap_tight", "fbx_wrap_tight", "fbx_wrap_tight" }, "fbx_wrap_tight" },
	{ "grip_pistol_fire",     0, 0, VRHP_ALL("fbx_wrap_tight"), "fbx_wrap_tight" },
	{ "grip_shotgun",         0, 0, { "fbx_wrap_tight", "fbx_wrap_half", "fbx_wrap_tight", "fbx_wrap_tight", "fbx_wrap_tight" }, "fbx_wrap_tight" },
	{ "grip_shotgun_fire",    0, 0, VRHP_ALL("fbx_wrap_tight"), "fbx_wrap_tight" },
	{ "grip_ssg",             0, 0, { "fbx_wrap_tight", "fbx_wrap_half", "fbx_wrap_tight", "fbx_wrap_tight", "fbx_wrap_tight" }, "fbx_wrap_tight" },
	{ "grip_ssg_fire",        0, 0, VRHP_ALL("fbx_wrap_tight"), "fbx_wrap_tight" },
	{ "grip_rifle",           0, 0, { "fbx_wrap_tight", "fbx_wrap_half", "fbx_wrap_tight", "fbx_wrap_tight", "fbx_wrap_tight" }, "fbx_wrap_tight" },
	{ "grip_rifle_fire",      0, 0, VRHP_ALL("fbx_wrap_tight"), "fbx_wrap_tight" },
	{ "grip_rocket",          0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "grip_rocket_fire",     0, 0, { "fbx_wrap_half", "fbx_wrap_tight", "fbx_wrap_half", "fbx_wrap_half", "fbx_wrap_half" }, "fbx_wrap_half" },
	{ "grip_plasma",          0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "grip_plasma_fire",     0, 0, { "fbx_wrap_half", "fbx_wrap_tight", "fbx_wrap_half", "fbx_wrap_half", "fbx_wrap_half" }, "fbx_wrap_half" },
	{ "grip_bfg",             0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "grip_bfg_fire",        0, 0, { "fbx_wrap_half", "fbx_wrap_tight", "fbx_wrap_half", "fbx_wrap_half", "fbx_wrap_half" }, "fbx_wrap_half" },
	{ "grip_saw",             0, 0, VRHP_ALL("fbx_fist"), "fbx_fist" },
	{ "offhand_weapon_grip",  0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "melee_grip",           0, 0, VRHP_ALL("fbx_fist"), "fbx_fist" },
	{ "pointing",             0, 0, { "fbx_fist", "fbx_fingergun", "fbx_fist", "fbx_fist", "fbx_fist" }, nullptr },
	{ "hold_magazine",        0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "hold_inserting",       0, 0, VRHP_ALL("fbx_relaxed"), "fbx_relaxed" },
	{ "hold_round",           0, 0, VRHP_ALL("fbx_cup"), "fbx_cup" },
	{ "hold_shell",           0, 0, VRHP_ALL("fbx_cup"), "fbx_cup" },
	{ "hold_slide",           0, 0, VRHP_ALL("fbx_wrap_tight"), "fbx_wrap_tight" },
	{ "hold_forend",          0, 0, VRHP_ALL("fbx_wrap_tight"), "fbx_wrap_tight" },
	{ "hold_foregrip",        0, 0, VRHP_ALL("fbx_fist"), "fbx_fist" },
	{ "hold_support",         0, 0, VRHP_ALL("fbx_twohand_support"), "fbx_twohand_support" },
	{ "hold_support_shotgun", 0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "hold_support_ssg",     0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "hold_support_rifle",   0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "hold_support_plasma",  0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "hold_support_rocket",  0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "hold_support_bfg",     0, 0, VRHP_ALL("fbx_wrap_half"), "fbx_wrap_half" },
	{ "hold_support_saw",     0, 0, VRHP_ALL("fbx_fist"), "fbx_fist" },
};
#undef VRHP_ALL
inline constexpr int kOwnerPoseCount = (int)(sizeof(kOwnerPoses) / sizeof(kOwnerPoses[0]));

inline int FindOwnerPose(const char *name)
{
	for (int i = 0; i < kOwnerPoseCount; ++i) if (EqualNoCase(kOwnerPoses[i].name, name)) return i;
	return -1;
}

// The take frame a RAW pose gives `side`, or -1: that side's take did not author it (HANDS_A is the right hand's
// alone, HANDS_B the left's), and it is never mirrored over from the other hand.
inline int OwnerRawFrame(int raw, int side)
{
	if (raw < 0 || raw >= kOwnerPoseCount || kOwnerPoses[raw].blender <= 0) return -1;
	const OwnerPose &p = kOwnerPoses[raw];
	const bool mine = p.hands == HANDS_BOTH || (p.hands == HANDS_A && side == SIDE_RIGHT) || (p.hands == HANDS_B && side == SIDE_LEFT);
	return mine ? p.blender - 1 : -1;
}

// The take frame finger `f` of pose `p` reads on `side` (-1: not on this side); -2 = the table is malformed.
inline int OwnerFingerFrame(int p, int f, int side)
{
	const OwnerPose &op = kOwnerPoses[p];
	if (op.blender > 0) return OwnerRawFrame(p, side);
	const int raw = FindOwnerPose(op.finger[f]);
	if (raw < 0 || kOwnerPoses[raw].blender <= 0) return -2;
	return OwnerRawFrame(raw, side);
}

// One side's poses from the table, built once per model and side (BuildHoldPoses).
struct HoldPoses
{
	bool  Have[kOwnerPoseCount] = {};								// this side's own take has every frame the pose reads
	int   Frame[kOwnerPoseCount][FINGER_COUNT] = {};				// those take frames (-1: not on this side)
	float Q[kOwnerPoseCount][FINGER_COUNT][FINGER_JOINTS][4] = {};	// PoseRotation, relative to the drawn frame
	bool  PalmDriven[kOwnerPoseCount] = {};
	int   PalmFrame[kOwnerPoseCount] = {};
	float PalmQ[kOwnerPoseCount][4] = {};
	float PalmOpen[4] = { 0.f, 0.f, 0.f, 1.f };						// the palm in the drawn frame
	bool  Built = false;
};

// VRAvatar_NamedPose(pose, finger, ...) for this side (VA:334-347): does the pose carry finger f? The owner's poses
// carry whole hands, so a pose this side has carries every finger. DEPARTS from VA in being per side: VA's probe is
// side-free because UZDXREMA's table always has both sides, the missing one mirrored (see above).
inline bool HoldHas(const HoldPoses *hp, int p, int f)
{
	return hp != nullptr && hp->Built && p >= 0 && p < kOwnerPoseCount && f >= 0 && f < FINGER_COUNT && hp->Have[p];
}
inline int HoldFind(const HoldPoses *hp, const std::string &name, int f)	// the pose's index if HoldHas, else -1
{
	const int p = FindOwnerPose(name.c_str());
	return HoldHas(hp, p, f) ? p : -1;
}
// VRAvatar_NamedPoseRot(pose, side, f, j, q) (VA:319-332), for this side.
inline bool HoldRot(const HoldPoses *hp, int p, int f, int j, float q[4])
{
	if (!HoldHas(hp, p, f) || j < 0 || j >= FINGER_JOINTS) return false;
	for (int k = 0; k < 4; ++k) q[k] = hp->Q[p][f][j][k];
	return true;
}

template<class GetQ>
inline bool BuildHoldPoses(const HandRig &rig, const FingerFrame frames[FINGER_COUNT][FINGER_JOINTS], int side, int numFrames,
	GetQ getQ, HoldPoses &out, char *why, size_t whyLen)
{
	out = HoldPoses();
	if (side < 0 || side >= SIDE_COUNT) { snprintf(why, whyLen, "no such side %d", side); return false; }
	auto rot = [&](const FingerFrame &F, int joint, int frame, float q[4]) {
		double qd[4], qp[4], r[4];
		getQ(kDrawnFrame, joint, qd);
		getQ(frame, joint, qp);
		PoseRotation(F, qd, qp, r);
		for (int k = 0; k < 4; ++k) q[k] = (float)r[k];
	};
	FingerFrame I;
	I.X[0] = 1; I.Y[1] = 1; I.Z[2] = 1;
	if (rig.Palm >= 0) rot(I, rig.Palm, kDrawnFrame, out.PalmOpen);
	for (int p = 0; p < kOwnerPoseCount; ++p)
	{
		bool have = true;
		for (int f = 0; f < FINGER_COUNT; ++f)
		{
			const int fr = OwnerFingerFrame(p, f, side);
			if (fr == -2) { snprintf(why, whyLen, "the pose table is malformed at %s (finger %d)", kOwnerPoses[p].name, f); return false; }
			out.Frame[p][f] = fr;
			if (fr < 0 || fr >= numFrames) have = false;
		}
		out.PalmFrame[p] = -1;
		if (kOwnerPoses[p].palm != nullptr)
		{
			const int raw = FindOwnerPose(kOwnerPoses[p].palm);
			if (raw < 0 || kOwnerPoses[raw].blender <= 0) { snprintf(why, whyLen, "the pose table is malformed at %s (palm)", kOwnerPoses[p].name); return false; }
			out.PalmFrame[p] = OwnerRawFrame(raw, side);
			if (out.PalmFrame[p] < 0 || out.PalmFrame[p] >= numFrames || rig.Palm < 0) have = false;
		}
		out.Have[p] = have;
		if (!have) continue;
		for (int f = 0; f < FINGER_COUNT; ++f)
			for (int j = 0; j < FINGER_JOINTS; ++j) rot(frames[f][j], rig.Joint[f][j], out.Frame[p][f], out.Q[p][f][j]);
		if (out.PalmFrame[p] >= 0) { out.PalmDriven[p] = true; rot(I, rig.Palm, out.PalmFrame[p], out.PalmQ[p]); }
	}
	out.Built = true;
	return true;
}

//---------------------------------------------------------------------------
// WHICH POSE SHAPES THE HAND: VAP :476-656, line for line. VAP's srcPose (which named pose set each joint; empty =
// the controller did) is Src (an index into kOwnerPoses; -1 = the controller); idxReady / idxFire / idxT the index
// finger's ready/fire pair and the trigger blending it. VAP's `want` (the scalar curls) is not kept: no bone reads
// it here (checklist 18). tools/hands_test runs VAP's own lines beside this: difference 0.
//---------------------------------------------------------------------------

struct HoldChoice
{
	int   Src[FINGER_COUNT][FINGER_JOINTS];
	int   IdxReady = -1, IdxFire = -1;
	float IdxT = 0.f;
	HoldChoice() { for (int f = 0; f < FINGER_COUNT; ++f) for (int j = 0; j < FINGER_JOINTS; ++j) Src[f][j] = -1; }
	bool Any() const
	{
		for (int f = 0; f < FINGER_COUNT; ++f) for (int j = 0; j < FINGER_JOINTS; ++j) if (Src[f][j] >= 0) return true;
		return IdxReady >= 0;
	}
};

// `mine`: this hand's held state; `other`: the other hand's (VAP :622-626); `in`: this hand's controller (the trigger
// for the index blend, VAP :545).
inline void SelectHold(const HoldPoses *hp, const HeldState &mine, const HeldState &other, const Inputs &in, HoldChoice &out)
{
	out = HoldChoice();
	if (hp == nullptr || !hp->Built) return;
	if (mine.weapon)																				// :476
	{
		const std::string &cls = mine.gripClass;													// :484
		std::string base = "gun_grip";																// :485
		std::string fire;																			// :486
		if (!cls.empty())																			// :487-491
		{
			base = "grip_" + cls;
			fire = "grip_" + cls + "_fire";
		}
		{																							// :494-503 [GUNPOSE]
			const std::string &prop = mine.propClass;
			const std::string own = "grip_" + prop;
			if (!prop.empty() && HoldFind(hp, own, FINGER_MIDDLE) >= 0)
			{
				base = own;
				fire = own + "_fire";
			}
		}
		static const int wrapped[] = { FINGER_MIDDLE, FINGER_RING, FINGER_PINKY };					// :505
		for (int f : wrapped)																		// :506-514
		{
			for (int j = 0; j < FINGER_JOINTS; ++j)
			{
				int p;
				if ((p = HoldFind(hp, base, f)) >= 0) out.Src[f][j] = p;
				else if ((p = HoldFind(hp, "gun_grip", f)) >= 0) out.Src[f][j] = p;
			}
		}
		{																							// :522-535 [FBXGRIP] the thumb
			int tsrc = -1;
			float qprobe[4];
			const int pb = FindOwnerPose(base.c_str()), pg = FindOwnerPose("gun_grip");
			if (HoldRot(hp, pb, FINGER_THUMB, 0, qprobe)) tsrc = pb;
			else if (HoldRot(hp, pg, FINGER_THUMB, 0, qprobe)) tsrc = pg;
			if (tsrc >= 0)
				for (int j = 0; j < FINGER_JOINTS; ++j)
					if (HoldHas(hp, tsrc, FINGER_THUMB)) out.Src[FINGER_THUMB][j] = tsrc;
		}
		if (!fire.empty())																			// :543-556 the index, ready to fire
		{
			const float t = in.valid ? in.trigger : 0.f;											// :545
			for (int j = 0; j < FINGER_JOINTS; ++j)
			{
				const int pr = HoldFind(hp, base, FINGER_INDEX), pf = HoldFind(hp, fire, FINGER_INDEX);
				if (pr >= 0 && pf >= 0) { out.IdxReady = pr; out.IdxFire = pf; out.IdxT = t; }
			}
		}
	}
	else																							// :558
	{
		const int subj = mine.subject;																// :587
		const char *sname = nullptr;																// :588
		const char *sfall = nullptr;																// :589
		switch (subj)																				// :590-609
		{
			case GRIPSUBJ_Magazine:  sname = "hold_magazine"; break;
			case GRIPSUBJ_Round:     sname = "hold_round";    break;
			case GRIPSUBJ_Shell:     sname = "hold_shell";    break;
			case GRIPSUBJ_Inserting: sname = "hold_inserting"; sfall = "hold_round"; break;
			case GRIPSUBJ_Forend:    sname = "hold_forend";   break;
			case GRIPSUBJ_Foregrip:  sname = "hold_foregrip"; break;
			case GRIPSUBJ_Slide:     sname = "hold_slide";    break;
			case GRIPSUBJ_Support:   sname = "hold_support";  sfall = "offhand_weapon_grip"; break;
			default: break;   // None, Grip, Holster, Pouch: not a held object
		}
		// NOT IN THE SOURCE (HeldState::pose): a pose the held thing names itself takes the subject's place.
		if (!mine.pose.empty()) { sname = mine.pose.c_str(); sfall = nullptr; }
		if (sname != nullptr)																		// :610
		{
			const std::string gunCls = other.weapon ? other.gripClass : std::string();				// :622-624
			const std::string gunProp = other.weapon ? other.propClass : std::string();			// :625-626
			std::string tries[6];																	// :627
			int ntries = 0;																			// :628
			const char *bases[2] = { sname, sfall };												// :629
			for (const char *b : bases)																// :630-636
			{
				if (b == nullptr) continue;
				if (!gunProp.empty()) tries[ntries++] = std::string(b) + "_" + gunProp;
				if (!gunCls.empty()) tries[ntries++] = std::string(b) + "_" + gunCls;
				tries[ntries++] = b;
			}
			int use = -1;																			// :637
			for (int t = 0; t < ntries && use < 0; ++t)												// :638-643
				for (int f = 0; f < FINGER_COUNT && use < 0; ++f)
					use = HoldFind(hp, tries[t], f);
			if (use >= 0)																			// :644-654
				for (int f = 0; f < FINGER_COUNT; ++f)
					for (int j = 0; j < FINGER_JOINTS; ++j)
						if (HoldHas(hp, use, f)) out.Src[f][j] = use;
		}
	}
}

// THE ROTATION TARGETS: VAP :674-684 over the controller's targets. `target` / `palm` come in as the controller's
// pose (PoseTargets) and leave with the held pose where it sets a joint; where VAP falls back to its curl table
// (:685-690) the controller's pose stays (checklist 19).
// THE PALM, NOT FROM THE SOURCE (item 38's rule, kOwnerPoses "Palm"): while every finger joint is the held pose's, the
// palm takes the frame of the pose the middle finger comes from where that pose drives it, else the drawn frame; while
// any finger is still the controller's (a weapon with no fire pose leaves the index there), the drawn frame -- that
// finger can be straight and is not from the pose's frame, and a turned palm would swing it (item 38).
inline void HoldTargets(const HoldPoses *hp, const HoldChoice &ch, float target[FINGER_COUNT][FINGER_JOINTS][4], float palm[4])
{
	if (hp == nullptr || !hp->Built || !ch.Any()) return;
	bool all = true;
	for (int f = 0; f < FINGER_COUNT; ++f)
	{
		for (int j = 0; j < FINGER_JOINTS; ++j)
		{
			float wq[4];																			// :672
			bool have = false;																		// :673
			if (f == FINGER_INDEX && ch.IdxReady >= 0)												// :674-682
			{
				float qa[4], qb[4];
				if (HoldRot(hp, ch.IdxReady, f, j, qa) && HoldRot(hp, ch.IdxFire, f, j, qb))
				{
					QSlerp(qa, qb, ch.IdxT, wq); have = true;
				}
			}
			if (!have && ch.Src[f][j] >= 0)															// :683-684
				have = HoldRot(hp, ch.Src[f][j], f, j, wq);
			if (have) { for (int k = 0; k < 4; ++k) target[f][j][k] = wq[k]; }
			else all = false;
		}
	}
	const int p = ch.Src[FINGER_MIDDLE][0];
	const float *pq = (all && p >= 0 && hp->PalmDriven[p]) ? hp->PalmQ[p] : hp->PalmOpen;
	for (int k = 0; k < 4; ++k) palm[k] = pq[k];
}

// StepPose with a held thing: the controller's targets (PoseRule, PoseTargets), the held pose over them (HoldTargets),
// then the same smoothing (Smooth; the palm likewise). With hold or choice null it is StepPose exactly.
inline void StepPoseHeld(SideState &rot, PalmState &palm, const Inputs &in, float retain, const HandPoses *poses,
	const HoldPoses *hold, const HoldChoice *choice)
{
	PoseInputs pi;
	PoseRule(in, pi);
	float target[FINGER_COUNT][FINGER_JOINTS][4], pt[4];
	PoseTargets(poses, pi, target, pt);
	if (hold != nullptr && choice != nullptr) HoldTargets(hold, *choice, target, pt);
	Smooth(rot, target, retain);
	if (!palm.Init) { for (int k = 0; k < 4; ++k) palm.Q[k] = pt[k]; }
	else { float o[4]; QSlerp(palm.Q, pt, 1.f - retain, o); for (int k = 0; k < 4; ++k) palm.Q[k] = o[k]; }
	palm.Init = true;
}

// One line on what shapes the hand, for the log.
inline std::string DescribeHold(const HoldPoses *hp, const HeldState &mine, const HoldChoice &ch)
{
	char b[256];
	const bool holding = mine.weapon || mine.subject != GRIPSUBJ_None || !mine.pose.empty();
	if (!holding) return "nothing held: the controller's pose";
	std::string what;
	if (mine.weapon) { snprintf(b, sizeof(b), "a weapon (grip class %s, prop %s)", mine.gripClass.empty() ? "none" : mine.gripClass.c_str(), mine.propClass.empty() ? "none" : mine.propClass.c_str()); what = b; }
	else if (!mine.pose.empty()) what = "the pose " + mine.pose;
	else what = std::string("subject ") + GripSubjectName(mine.subject);
	if (mine.item > 0) { snprintf(b, sizeof(b), " (item %d)", mine.item); what += b; }
	if (hp == nullptr || !hp->Built) return what + ": no poses built for this hand";
	if (!ch.Any()) return what + ": no pose of the owner's table for it on this hand (its own take lacks the frames) -- the controller's pose";
	std::string s = what + ":";
	static const char *const fn[FINGER_COUNT] = { "thumb", "index", "middle", "ring", "pinky" };
	for (int f = 0; f < FINGER_COUNT; ++f)
	{
		if (f == FINGER_INDEX && ch.IdxReady >= 0) { snprintf(b, sizeof(b), " index %s->%s by the trigger;", kOwnerPoses[ch.IdxReady].name, kOwnerPoses[ch.IdxFire].name); s += b; continue; }
		const int p = ch.Src[f][0];
		snprintf(b, sizeof(b), " %s %s;", fn[f], p >= 0 ? kOwnerPoses[p].name : "the controller");
		s += b;
	}
	const int pm = ch.Src[FINGER_MIDDLE][0];
	snprintf(b, sizeof(b), " palm %s", (pm >= 0 && hp->PalmDriven[pm]) ? "from the pose" : "in the drawn frame");
	return s + b;
}

} // namespace VRHandPose
