/*
** vr_handpose_math.h
**
** THE VR HANDS, AS PURE MATH: the controller's finger input, the curl rule, the display-rate
** smoothing, each finger joint's canonical bone frame, the fold targets, the turn applied to the
** joints, and where the hand sits on the controller (its frame from the wrist and knuckles, the grip
** socket).
**
** Nothing of the engine in it -- floats, doubles and std containers only -- so the exact code the
** renderer runs is compiled and run by tools/hands_test/ and tools/fbxload_test/ outside the engine.
** vr_handpose.cpp owns the engine half: which actor is a hand, reading the controller, reading the
** model's joints and frames, the curl table lump, the palette.
**
** PORTED FROM UZDXREMA (read-only, HEAD 39e4dad):
**   src/r_data/vr_avatar_pose.cpp ("VAP") UpdateCurls (:419-698), empty-hand path, and the finger
**     turn on the joint globals (:1771-1838);
**   src/r_data/vr_avatar.cpp ("VA") the curl table reader (:187-281, engine half: vr_handpose.cpp);
**   tools/avatar/measure_avatar.py ("MA") the palm normal (:365-377) and the canonical bone frame
**     of every finger joint (:476-555);
**   tools/avatar/mkfbxposes.py ("MKP") a joint's pose rotation from two FBX frames, in that frame
**     (main(), the `D = ...; quat(F^T D F)` lines, and quat()).
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
// The scalar curl UZDXREMA smooths beside it (FCurlState) is CurlState / SmoothCurls below.
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
// THE CURL RULE. VAP UpdateCurls :440-458, the empty hand, unchanged:
//
//     index  = trigger > 0 ? trigger : (triggerTouch ? 0.3 : 0)   -- a finger resting ON the trigger
//                                                                    without pulling it is not straight
//     middle = ring = pinky = grip
//     thumb  = thumbTouch ? 0.6 : 0                                -- the thumb lies along the
//                                                                    controller when it rests on it
//     no controller this frame -> every finger 0                   -- an open hand, not a frozen claw
//
// then the one number per finger copied to its three joints (:456-458). 0 is an open hand, 1 a fist.
//
// NOT PORTED (VAP :460-656): the weapon-grip branch (gun_grip / grip_<class> / grip_<prop> named
// poses, the thumb round the handle, the index ready/fire blend) and the held-object branch
// (hold_<subject>[_<class>]). Nothing is held in these hands yet; both branches only ever replace
// want[][] by a named pose, so the empty-hand path here is what runs whenever nothing is held.
//
//===========================================================================

inline void FingerCurls(const Inputs &h, float perFinger[FINGER_COUNT])
{
	perFinger[FINGER_INDEX] = (h.trigger > 0.f) ? h.trigger : (h.triggerTouch ? 0.3f : 0.f);
	perFinger[FINGER_MIDDLE] = h.grip;
	perFinger[FINGER_RING] = h.grip;
	perFinger[FINGER_PINKY] = h.grip;
	perFinger[FINGER_THUMB] = h.thumbTouch ? 0.6f : 0.f;

	if (!h.valid)
		for (int f = 0; f < FINGER_COUNT; ++f) perFinger[f] = 0.f;
}

inline void JointCurls(const float perFinger[FINGER_COUNT], float want[FINGER_COUNT][FINGER_JOINTS])
{
	for (int f = 0; f < FINGER_COUNT; ++f)
		for (int j = 0; j < FINGER_JOINTS; ++j)
			want[f][j] = perFinger[f];
}

//===========================================================================
//
// THE SCALAR CURL, smoothed. VAP FCurlState (:371-379) and :658-665, unchanged:
//
//     c = want + (cur - want) * retain;  cur = clamp(c, 0, 1)
//
// Kept because the source keeps it. As in the source, nothing turns a bone from it: the bones are
// turned by the rotations (SideState, VAP gRot) alone.
//
//===========================================================================

struct CurlState
{
	float Curl[FINGER_COUNT][FINGER_JOINTS] = {};
};

inline void SmoothCurls(CurlState &s, const float want[FINGER_COUNT][FINGER_JOINTS], float retain)
{
	for (int f = 0; f < FINGER_COUNT; ++f)
	{
		for (int j = 0; j < FINGER_JOINTS; ++j)
		{
			const float c = want[f][j] + (s.Curl[f][j] - want[f][j]) * retain;
			s.Curl[f][j] = (c < 0.f) ? 0.f : (c > 1.f ? 1.f : c);
		}
	}
}

//===========================================================================
//
// VAP QAxisAngle (:410-414), verbatim (the source's own pi and half-angle constant).
//
//===========================================================================

inline void QAxisAngle(const float axis[3], float deg, float out[4])
{
	const float h = deg * (3.14159265f / 360.f), s = sinf(h);
	out[0] = axis[0] * s; out[1] = axis[1] * s; out[2] = axis[2] * s; out[3] = cosf(h);
}

//===========================================================================
//
// ONE JOINT'S TARGET ROTATION, in its canonical bone frame. VAP :667-690, the two ways it is made:
//
//  1. FROM THE FBX'S OWN FRAMES. The hand's open frame and its fist frame, each as a full joint
//     rotation in the canonical bone frame (PoseRotation, below: mkfbxposes.py's formula), and the
//     curl blends them: wq = QSlerp(open, fist, curl). This is the source's [POSEROT] two-pose blend
//     (VAP :674-681, QSlerp(ready, fire, t) on full rotations from `rot` pose lines) with the open and
//     fist poses as its two ends and the curl rule's value as t.
//     DEPARTS FROM THE SOURCE'S EMPTY HAND, on the owner's word (2026-11-13: "walk the frames of
//     animation if you have to but get me badass HQ VR hands"; the lead: the fold targets come from the
//     FBX's own authored frames, each hand from its own take). UZDXREMA's empty hand folds about the
//     FRIK table's axis (2, below); its [POSEROT] blend exists only for the index on a gun. Here the
//     curl rule (how far) is unchanged and the FBX frames say what the fold looks like.
//  2. FROM THE FRIK TABLE (vrhandcurl.txt), when a joint has no FBX frames: QAxisAngle(axis,
//     degrees * curl), VAP :685-689 exactly; identity when the table is not loaded (VAP :689).
//
//===========================================================================

// FVRHandCurlJoint (UZDXREMA vr_avatar.h): one joint's row of vrhandcurl.txt, axis normalised at load.
struct CurlJoint
{
	float Axis[3] = { 0.f, 0.f, 1.f };
	float Degrees = 0.f;
};

// One joint's two FBX poses, in its canonical bone frame, relative to the frame the model draws.
struct JointPose
{
	float Open[4] = { 0.f, 0.f, 0.f, 1.f };
	float Fist[4] = { 0.f, 0.f, 0.f, 1.f };
	bool  Have = false;
};

inline void JointTarget(const JointPose *pose, const CurlJoint *cj, float want, float wq[4])
{
	bool have = false;
	if (pose != nullptr && pose->Have)
	{
		QSlerp(pose->Open, pose->Fist, want, wq);
		have = true;
	}
	if (!have)
	{
		if (cj != nullptr) QAxisAngle(cj->Axis, cj->Degrees * want, wq);
		else { wq[0] = wq[1] = wq[2] = 0.f; wq[3] = 1.f; }
	}
}

//===========================================================================
//
// ONE SIDE, ONE DISPLAYED FRAME. VAP :440-458 (the rule), :658-665 (the scalar), :667-696 (the
// rotation targets, then gRot's slerp). `poses` (per finger, per joint) may be null: the table, or
// identity. `table` may be null: not loaded.
//
//===========================================================================

inline void StepSide(CurlState &curl, SideState &rot, const Inputs &in, float retain,
	const JointPose (*poses)[FINGER_JOINTS], const CurlJoint (*table)[FINGER_JOINTS])
{
	float perFinger[FINGER_COUNT];
	float want[FINGER_COUNT][FINGER_JOINTS];
	FingerCurls(in, perFinger);
	JointCurls(perFinger, want);
	SmoothCurls(curl, want, retain);

	float target[FINGER_COUNT][FINGER_JOINTS][4];
	for (int f = 0; f < FINGER_COUNT; ++f)
		for (int j = 0; j < FINGER_JOINTS; ++j)
			JointTarget(poses != nullptr ? &poses[f][j] : nullptr, table != nullptr ? &table[f][j] : nullptr, want[f][j], target[f][j]);
	Smooth(rot, target, retain);
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

// VAP :1785-1836 on `G` (n joint globals). `moved[i]` is set for every joint written.
inline void ApplyFingerTurns(float (*G)[16], int n, const HandRig &rig, const SideState &rot, std::vector<char> &moved)
{
	moved.assign(n, 0);
	for (int f = 0; f < FINGER_COUNT; ++f)
	{
		for (int j = 0; j < FINGER_JOINTS; ++j)
		{
			const float *q = rot.Q[f][j];
			if (fabsf(q[3]) > 0.999999f) continue;   // no turn
			const int jt = rig.Joint[f][j];
			if (jt < 0 || jt >= n) continue;

			// Rq (VAP :1799-1805), F (:1806-1807), Dm.Col[c] = F.Apply(Rq.Apply(F.Unapply(e_c))) (:1808-1813)
			const float x = q[0], y = q[1], z = q[2], w = q[3];
			const float Rq[3][3] =
			{
				{ 1.f - 2.f*(y*y + z*z), 2.f*(x*y + w*z), 2.f*(x*z - w*y) },
				{ 2.f*(x*y - w*z), 1.f - 2.f*(x*x + z*z), 2.f*(y*z + w*x) },
				{ 2.f*(x*z + w*y), 2.f*(y*z - w*x), 1.f - 2.f*(x*x + y*y) },
			};
			const float (*Fm)[3] = rig.Frame[f][j];
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
			Detail::Mul4(before, D, after);
			memcpy(G[jt], after, sizeof(after));
			moved[jt] = 1;

			// Carry the rest of this finger with it (VAP :1821-1834)
			float invBefore[16];
			if (!Detail::Inverse4(before, invBefore)) continue;
			float carry[16];
			Detail::Mul4(after, invBefore, carry);
			for (int d : rig.Below[f][j])
			{
				if (d < 0 || d >= n) continue;
				Detail::Mul4(carry, G[d], G[d]);
				moved[d] = 1;
			}
		}
	}
}

// The whole draw: palette -> globals, the turn, globals -> palette for every joint that moved, the
// rest copied. base/invBase: the model's rest (IQM baseframe and its inverse). `scratch` is reused.
inline void PosePalette(const float (*pal)[16], const float (*base)[16], const float (*invBase)[16], int n,
	const HandRig &rig, const SideState &rot, float (*out)[16], std::vector<float> &scratch, std::vector<char> &moved)
{
	scratch.resize((size_t)n * 16);
	float (*G)[16] = reinterpret_cast<float (*)[16]>(scratch.data());
	for (int i = 0; i < n; ++i) PaletteToGlobal(pal[i], base[i], G[i]);
	ApplyFingerTurns(G, n, rig, rot, moved);
	for (int i = 0; i < n; ++i)
	{
		if (moved[i]) GlobalToPalette(G[i], invBase[i], out[i]);
		else memcpy(out[i], pal[i], sizeof(float) * 16);
	}
}

//===========================================================================
//
// THE DXR HANDS (hand_final.fbx), as mkfbxposes.py reads them: the hand joint, the three joints of
// each finger (FBX BASE/MID/TOP = joints 1/2/3; UP_TOP is a tip with no counterpart), and the two
// frames of each hand's own take the fold is made from.
//
// THE FRAMES. The owner's fbx_poses.json (UZDXREMA tools/avatar, read by mkfbxposes.py) names
// "open" = 1 and "fist" = 319, "both" (each hand from its own skeleton). Those are BLENDER frame
// numbers: the poses were dumped by hands_dump_pose_mats.py through Blender's FBX importer, whose
// animation offset puts the file's time 0 at frame 1. This engine's frame k is the take at k / 60 s
// (fbx_skinned.cpp), so Blender 1 = frame 0 and Blender 319 = frame 318. Checked, not assumed:
// tools/fbxload_test walks every frame of both takes and compares the pose rotations made from 0/318
// with the ones mkfbxposes.py wrote from 1/319 (UZDXREMA wadsrc/static/vrhandpose.txt).
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
inline constexpr int kDrawnFrame = 0;		// the frame the hands draw (MODELDEF FrameIndex ... 0)
inline constexpr int kOpenFrame = 0;		// fbx_poses.json "open" 1 (Blender)
inline constexpr int kFistFrame = 318;		// fbx_poses.json "fist" 319 (Blender)

//===========================================================================
//
// ONE HAND, BUILT ONCE PER MODEL AND SIDE: the palm normal and the canonical frames (MA), the turn's
// rig (the joints, their frames as floats, everything below each), and the two FBX poses per joint
// (MKP) when the model has the frames. qDrawn/qOpen/qFist: each joint's LOCAL rotation at those
// frames, indexed by joint; null = no FBX poses (the table, or nothing, then turns the fingers).
//
//===========================================================================

inline bool BuildHand(const std::vector<RestJoint> &J, int hand, const int joints[FINGER_COUNT][FINGER_JOINTS], bool right,
	const double (*qDrawn)[4], const double (*qOpen)[4], const double (*qFist)[4],
	HandRig &rig, JointPose poses[FINGER_COUNT][FINGER_JOINTS], FingerFrame frames[FINGER_COUNT][FINGER_JOINTS],
	double palm[3], char *why, size_t whyLen)
{
	const int n = (int)J.size();
	if (hand < 0 || hand >= n) { snprintf(why, whyLen, "no hand joint"); return false; }
	if (!MeasurePalm(J, hand, joints[FINGER_INDEX][0], joints[FINGER_MIDDLE][0], joints[FINGER_PINKY][0], right, palm, why, whyLen)) return false;
	if (!MeasureFingerFrames(J, joints, palm, frames, why, whyLen)) return false;
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
			rig.Below[f][j].clear();
			for (int i = 0; i < n; ++i)
			{
				int guard = 0;
				for (int p = J[i].parent; p >= 0 && guard < 4096; p = J[p].parent, ++guard)
					if (p == jt) { rig.Below[f][j].push_back(i); break; }
			}
			JointPose &P = poses[f][j];
			P = JointPose();
			if (qDrawn != nullptr && qOpen != nullptr && qFist != nullptr)
			{
				double a[4], b[4];
				PoseRotation(F, qDrawn[jt], qOpen[jt], a);
				PoseRotation(F, qDrawn[jt], qFist[jt], b);
				for (int k = 0; k < 4; ++k) { P.Open[k] = (float)a[k]; P.Fist[k] = (float)b[k]; }
				P.Have = true;
			}
		}
	}
	return true;
}

} // namespace VRHandPose
