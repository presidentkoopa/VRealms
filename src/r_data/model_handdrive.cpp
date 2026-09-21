/*
** model_handdrive.cpp
**
** RS FORK -- THE HAND DRIVE. See model_handdrive.h.
**
** Moved from models.cpp (plan piece A). Each function keeps the original's comment; where one read or wrote
** md->SurfOvDriveX[s] it now reads or writes d.x.
*/

#include <math.h>

#include "model_handdrive.h"
#include "matrix.h"
#include "i_time.h"

double   HandDrive_OffhandSlideTravel = 0.0;
uint64_t HandDrive_OffhandSlideMs = 0;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- THE PLAIN DRIVE ---------------------------------------------------------------------------------------------

// Where a hand is along a part's travel axis, in the model's own space.
//
// NO UNIT CONSTANT, DELIBERATELY. Whatever scale the model's path applied is
// inside modelToWorld, so inverting it undoes that scale along with everything
// else: the hand lands in the space the mesh's own vertices are in, which is
// the space the drive's axis and distance are measured in.
float HandDrive_Projection(const VSMatrix &handMat, VSMatrix modelToWorld, const FVector3 &axis, FVector3 &handModel)
{
	const float *hm = handMat.get();
	FVector3 handWorld(hm[12], hm[13], hm[14]);

	VSMatrix worldToModel;
	modelToWorld.inverseMatrix(worldToModel);
	const float *wm = worldToModel.get();
	handModel = FVector3(
		wm[0]*handWorld.X + wm[4]*handWorld.Y + wm[8] *handWorld.Z + wm[12],
		wm[1]*handWorld.X + wm[5]*handWorld.Y + wm[9] *handWorld.Z + wm[13],
		wm[2]*handWorld.X + wm[6]*handWorld.Y + wm[10]*handWorld.Z + wm[14]);

	return handModel.X*axis.X + handModel.Y*axis.Y + handModel.Z*axis.Z;
}

// A driven part at travel v (0..1): slid along its axis, and turned about its
// pivot if SetModelSurfaceDriveRotation gave it a turn. Turn about the pivot,
// then slide: v' = R(v - P) + P + slide. The surface transform is translate-
// then-rotate about the mesh origin, so the pivot folds into the offset as
// P - RP. RP comes from the very matrix multQuaternion builds from this
// quaternion, so the pivot holds still whatever handedness that conversion has.
void HandDrive_Pose(const FHandDrive &d, float v, FVector3 &offset, FVector4 &rotation)
{
	const FVector3 axis = d.axis;
	const float dist = (d.dist != 0.f) ? d.dist : 1.f;

	offset = axis * (v * dist);
	rotation = FVector4(0.f, 0.f, 0.f, 1.f);

	const float turn = d.turnDeg * v;
	if (turn != 0.f)
	{
		const FVector3 ta = d.turnAxis;
		const FVector3 P  = d.turnPivot;
		const double half = turn * (M_PI / 360.0);   // AxisAngle's half angle
		const float sh = (float)sin(half);
		const FVector4 q(ta.X * sh, ta.Y * sh, ta.Z * sh, (float)cos(half));

		VSMatrix turnMat;
		turnMat.loadIdentity();
		turnMat.multQuaternion(q);
		const float *tm = turnMat.get();
		const FVector3 RP(
			tm[0]*P.X + tm[4]*P.Y + tm[8] *P.Z,
			tm[1]*P.X + tm[5]*P.Y + tm[9] *P.Z,
			tm[2]*P.X + tm[6]*P.Y + tm[10]*P.Z);
		offset += P - RP;
		rotation = q;
	}
}

// The travel a FOLLOWER sees: the owner's formula without the owner's writes.
// Clamped the same way, so it is the number the owner draws this frame even
// though only the owner re-anchors. Before the owner has drawn the part once
// there is no anchor yet, and the value the drive resumed from is the honest
// answer.
float HandDrive_ValueForFollower(const FHandDrive &d, float proj)
{
	float v = d.base;
	if (d.armed)
	{
		const float dist = (d.dist != 0.f) ? d.dist : 1.f;
		v = d.base + (proj - d.anchor) / dist;
	}
	if (v < 0.f) v = 0.f;
	else if (v > 1.f) v = 1.f;
	return v;
}

// ---- A DRIVE THAT ONLY TURNS, AND A DRIVE IN TWO STAGES ----------------------------------------------------------
//
// Everything from here to HandDrive_StagedValueForFollower runs ONLY for a drive
// that SetModelSurfaceDriveHinge or SetModelSurfaceDriveStage set up. A plain
// drive never reaches it. Both readers (the owner's draw and a follower) ask
// HandDrive_IsStaged first and leave the plain branch exactly as it was, so no
// existing drive is re-derived through new code.
//
// The same split as the plain drive. The measure, the solve and the pose are
// PURE, shared by the owner's draw and a follower. Only the owner arms, and
// only the owner writes the solved state back.

bool HandDrive_IsStaged(const FHandDrive &d)
{
	return d.hinge || d.stage2Kind != HANDDRIVE_None;
}

namespace
{

// One stage's motion, from whichever fields hold it. Stage 0 is the drive
// itself: either a hinge made of the drive's turn, or the drive's own slide
// (whose optional twist HandDrive_Pose adds when it is posed). Stage 1 is
// SetModelSurfaceDriveStage's.
struct FStageMotion
{
	int      kind;
	FVector3 axis;     // unit, model space
	float    amount;   // model units along a slide, degrees round a hinge
	FVector3 pivot;    // model space, hinge only
};

FStageMotion StageMotion(const FHandDrive &d, int k)
{
	FStageMotion m;
	if (k == 0 && d.hinge)
	{
		m.kind   = HANDDRIVE_Hinge;
		m.axis   = d.turnAxis;
		m.amount = d.turnDeg;
		m.pivot  = d.turnPivot;
	}
	else if (k == 0)
	{
		m.kind   = HANDDRIVE_Slide;
		m.axis   = d.axis;
		m.amount = d.dist;
		m.pivot  = FVector3(0.f, 0.f, 0.f);
	}
	else
	{
		m.kind   = d.stage2Kind;
		m.axis   = d.stage2Axis;
		m.amount = d.stage2Amount;
		m.pivot  = d.stage2Pivot;
	}
	// The natives refuse a zero amount. This only keeps a division honest, as
	// the plain drive's own fallback does.
	if (m.amount == 0.f) m.amount = 1.f;
	return m;
}

// Where the hand is, in stage k's own terms.
//
// A SLIDE: how far along its axis, in model units. This is the plain drive's
// projection, unchanged.
//
// A HINGE: its ANGLE about the hinge line, in degrees, from the direction
// HingeArm captured. It is NOT the hand's travel along the tangent at
// the grab point. A hand lifting a handle swings round the pivot with it, and
// that arc projected onto a fixed tangent reads short: sin(60) / (60 in
// radians), 83% of the way, at the top of a 60-degree lift. The handle would
// trail the hand by ten degrees exactly where a bolt turns its corner. The
// angle is the handle's own coordinate, so hand and part turn by the same
// amount at every point of the swing, whatever radius the hand holds it at.
//
// NO LEVER, NO MINIMUM RADIUS. atan2 of the hand's two in-plane components does
// not depend on how far out the hand is, so a hand gripping 1cm from the line
// turns the part exactly as far as one gripping 10cm out. A fixed reach added
// at the grab, which this once did, does not turn with the hand: it reads
// r / (r + reach) of the swing, and a hand gripping close in could never bring
// a bolt handle to its corner.
//
// False when the angle is undefined: the hand within kHingeOnLine of the hinge
// line, where there is no direction round it. HandDrive_StagedSolve then HOLDS the
// value drawn last frame; it does not read the hand as back at its anchor.
const float kHingeOnLine = 1e-6f;   // mesh units: nearer the line than this, no angle

bool StageMeasure(const FHandDrive &d, int k, const FVector3 &hm, float &out)
{
	const FStageMotion m = StageMotion(d, k);
	if (m.kind != HANDDRIVE_Hinge)
	{
		out = hm.X*m.axis.X + hm.Y*m.axis.Y + hm.Z*m.axis.Z;
		return true;
	}

	FVector3 dv = hm - m.pivot;
	const float along = dv.X*m.axis.X + dv.Y*m.axis.Y + dv.Z*m.axis.Z;
	dv = dv - m.axis * along;
	const FVector3 &rx = d.hingeRefX[k];
	const FVector3 &ry = d.hingeRefY[k];
	const float x = dv.X*rx.X + dv.Y*rx.Y + dv.Z*rx.Z;
	const float y = dv.X*ry.X + dv.Y*ry.Y + dv.Z*ry.Z;
	if (x*x + y*y < kHingeOnLine * kHingeOnLine) return false;
	out = (float)(atan2((double)y, (double)x) * (180.0 / M_PI));
	return true;
}

// Stage k's travel since its anchor, in that stage's own 0..1, into `out`.
// False when the measure is undefined (see StageMeasure), and `out` is
// then not written. A hinge's angle difference is taken the short way round,
// so a hand passing behind the line it is measured from does not read as a
// whole turn. That is why a hinge stage must turn less than 180 degrees:
// re-anchoring at the clamp keeps an honest swing from reaching the far side.
bool StageTravel(const FHandDrive &d, int k, const FVector3 &hm, float anchor, float &out)
{
	float meas;
	if (!StageMeasure(d, k, hm, meas)) return false;
	const FStageMotion m = StageMotion(d, k);
	float dd = meas - anchor;
	if (m.kind == HANDDRIVE_Hinge)
	{
		while (dd > 180.f)   dd -= 360.f;
		while (dd <= -180.f) dd += 360.f;
	}
	out = dd / m.amount;
	return true;
}

// Where a hinge stage measures its angle from, captured on the drive's first
// drawn frame. Same instant, same reason, as a plain drive's anchor. RefX is
// the hand's own direction from the hinge line.
//
// RefY is where RefX goes under a +90 degree turn built by the turn's own
// matrix. So the angle grows the way positive degrees turn the part, whatever
// handedness the quaternion conversion has, and a negative `degrees` divides it
// into positive travel the other way round.
//
// False, and nothing usable captured, when the hand is on the line and has no
// direction from it. A slide stage has nothing to capture and is always true.
bool HingeArm(FHandDrive &d, int k, const FVector3 &hm)
{
	const FStageMotion m = StageMotion(d, k);
	if (m.kind != HANDDRIVE_Hinge) return true;

	FVector3 dv = hm - m.pivot;
	const float along = dv.X*m.axis.X + dv.Y*m.axis.Y + dv.Z*m.axis.Z;
	dv = dv - m.axis * along;
	const float r = dv.Length();
	if (!(r >= kHingeOnLine)) return false;

	const FVector3 dir = dv / r;
	d.hingeRefX[k] = dir;

	FVector3 unusedOffset;
	FVector4 quarter;
	HandDrive_TurnAboutPivot(m.axis, 90.f, FVector3(0.f, 0.f, 0.f), unusedOffset, quarter);
	d.hingeRefY[k] = HandDrive_RotateByQuat(quarter, dir);
	return true;
}

// Stage k's own pose at its travel u. Stage 1 as a slide is HandDrive_Pose
// itself, twist included, so a staged drive whose first stage is a slide draws
// that stage exactly as a plain drive does.
void StagePose(const FHandDrive &d, int k, float u, FVector3 &offset, FVector4 &rotation)
{
	const FStageMotion m = StageMotion(d, k);
	offset   = FVector3(0.f, 0.f, 0.f);
	rotation = FVector4(0.f, 0.f, 0.f, 1.f);
	if (m.kind == HANDDRIVE_Hinge)
	{
		if (u != 0.f) HandDrive_TurnAboutPivot(m.axis, m.amount * u, m.pivot, offset, rotation);
	}
	else if (k == 0)
	{
		HandDrive_Pose(d, u, offset, rotation);
	}
	else
	{
		offset = m.axis * (u * m.amount);
	}
}

} // namespace

// v turned by quaternion q, through the very matrix multQuaternion builds. That
// is the matrix the surface is drawn with, so this cannot disagree with the
// draw about handedness.
FVector3 HandDrive_RotateByQuat(const FVector4 &q, const FVector3 &v)
{
	VSMatrix m;
	m.loadIdentity();
	m.multQuaternion(q);
	const float *t = m.get();
	return FVector3(
		t[0]*v.X + t[4]*v.Y + t[8] *v.Z,
		t[1]*v.X + t[5]*v.Y + t[9] *v.Z,
		t[2]*v.X + t[6]*v.Y + t[10]*v.Z);
}

// A turn of `deg` about unit `axis` through pivot P, as a surface transform:
// the quaternion, and the offset P - RP that holds the pivot still. The same
// arithmetic as the turn inside HandDrive_Pose, which is left as it was.
void HandDrive_TurnAboutPivot(const FVector3 &axis, float deg, const FVector3 &P, FVector3 &offset, FVector4 &rotation)
{
	const double half = deg * (M_PI / 360.0);   // AxisAngle's half angle
	const float sh = (float)sin(half);
	rotation = FVector4(axis.X * sh, axis.Y * sh, axis.Z * sh, (float)cos(half));
	offset = P - HandDrive_RotateByQuat(rotation, P);
}

// ARM, DON'T ANCHOR, for a staged drive: the plain drive's rule applied to
// every stage, on the first drawn frame. Both stages are anchored at once,
// even the one the hand is not working, because the solve reads both measures
// every frame.
//
// ALL OR NOTHING. If any stage cannot be measured on this frame (a hinge with
// the hand on its line), no anchor is written and the drive is not marked
// armed. The value holds and the owner tries again on the next drawn frame, so
// no stage is ever solved against an anchor it did not capture.
bool HandDrive_StagedArm(FHandDrive &d, const FVector3 &hm)
{
	const int stages = (d.stage2Kind != HANDDRIVE_None) ? 2 : 1;
	float meas[2] = { 0.f, 0.f };
	for (int k = 0; k < stages; k++)
	{
		if (!HingeArm(d, k, hm)) return false;
		if (!StageMeasure(d, k, hm, meas[k])) return false;
	}
	for (int k = 0; k < stages; k++) d.stageAnchor[k] = meas[k];
	d.armed = true;
	return true;
}

FHandDriveStagedState HandDrive_StagedLoad(const FHandDrive &d)
{
	FHandDriveStagedState st;
	st.inStage2 = d.inStage2;
	for (int k = 0; k < 2; k++)
	{
		st.anchor[k] = d.stageAnchor[k];
		st.base[k]   = d.stageBase[k];
	}
	return st;
}

void HandDrive_StagedStore(FHandDrive &d, const FHandDriveStagedState &st)
{
	d.inStage2 = st.inStage2;
	for (int k = 0; k < 2; k++)
	{
		d.stageAnchor[k] = st.anchor[k];
		d.stageBase[k]   = st.base[k];
	}
}

// ONE HAND, ONE VALUE, TWO MOTIONS. Returns the value drawn this frame and
// leaves in `st` the state the owner stores. `prevValue` is the value the owner
// drew last frame (FHandDrive::value). `carry`, if given, receives the share of
// this frame's hand motion that went into the new stage, on a frame where the
// hand changed stage; on any other frame it is left untouched.
//
// A HINGE ON ITS OWN is the plain drive's formula with an angle for a length:
// base plus travel since the anchor, re-anchored at either end.
//
// TWO STAGES ARE AN L-SHAPED TRACK. Stage 2 cannot start until stage 1 is
// complete, and stage 1 cannot move while stage 2 is under way. A bolt does not
// draw back with its handle down, nor turn with the bolt out. Both hand
// measures are read every frame. The blocked stage's anchor is kept at the
// hand, so the moment that stage unblocks it moves from exactly where the hand
// is. At the corner both are live and whichever the hand moves takes over.
// Pushing back past the split returns to stage 1 by the same rule.
//
// A FAST HAND CROSSING THE CORNER INSIDE ONE FRAME. That frame's motion is cut
// at the instant the working stage reached its end. Stage 1 went from `prev`
// to w, and its end is 1, so f = (1 - prev) / (w - prev) of the frame was spent
// finishing it. Stage 2's anchor was last frame's hand (kept there while it was
// blocked), so the travel it reads this frame is the frame's whole motion along
// stage 2, and only the remaining (1 - f) of it goes to stage 2. That is exact
// for a hand moving in a straight line within one frame, which at display rate
// is the hand's path. The overshoot along stage 1 is past its end stop and is
// dropped, as at any clamp. Coming back is the mirror image.
//
// WHY NOTHING JUMPS. At the split both stages draw the identical pose (stage 2
// at 0 is the identity, see HandDrive_StagedPose). The value equals the split
// on either side of the corner, and a crossing adds only the travel the hand
// made past it. A second draw of the same frame (the other eye) reads zero
// travel everywhere, so it draws the same value.
float HandDrive_StagedSolve(const FHandDrive &d, const FVector3 &hm, float prevValue, FHandDriveStagedState &st, float *carry)
{
	// Re-anchor stage k at the hand, standing for stage travel b. Only reached
	// on a frame whose measures are defined (the hold below returns first); the
	// guard just keeps an anchor from ever being written from nothing.
	auto reanchor = [&](int k, float b)
	{
		float meas;
		if (StageMeasure(d, k, hm, meas)) st.anchor[k] = meas;
		st.base[k] = b;
	};

	// AN UNDEFINED MEASURE HOLDS. With the hand on a hinge's line there is no
	// angle this frame. Reading that as zero travel would put the part back at
	// whatever its anchor stands for (the value it had when grabbed) for a
	// frame. Instead the value drawn last frame stands and nothing is
	// re-anchored, and the next measurable frame reads the hand against the
	// same anchors as the frame before.
	const float held = (prevValue < 0.f) ? 0.f : (prevValue > 1.f) ? 1.f : prevValue;

	float t0;
	if (!StageTravel(d, 0, hm, st.anchor[0], t0)) return held;
	float w0 = st.base[0] + t0;

	if (d.stage2Kind == HANDDRIVE_None)
	{
		if (w0 < 0.f)      { reanchor(0, 0.f); w0 = 0.f; }
		else if (w0 > 1.f) { reanchor(0, 1.f); w0 = 1.f; }
		return w0;
	}

	const float S = d.split;
	float t1;
	if (!StageTravel(d, 1, hm, st.anchor[1], t1)) return held;
	float w1 = st.base[1] + t1;

	// How far into its own stage last frame's drawn value was. Within a hair of
	// the end counts as AT the end. S * 1 / S need not come back as exactly 1 in
	// float, and reading a hand parked on the corner as still finishing stage 1
	// would put all of its pull into f and none into stage 2: a bolt stuck at
	// the corner.
	const float kAtEnd = 1e-4f;

	if (!st.inStage2)
	{
		if (w0 < 1.f)
		{
			if (w0 < 0.f) { reanchor(0, 0.f); w0 = 0.f; }
			reanchor(1, 0.f);            // stage 2 blocked: kept at the hand
			return S * w0;
		}

		float prev = prevValue / S;
		if (prev < 0.f) prev = 0.f;
		const float f = (prev < 1.f - kAtEnd) ? (1.f - prev) / (w0 - prev) : 0.f;
		reanchor(0, 1.f);                // at its end stop; the overshoot is dropped
		float u1 = (1.f - f) * w1;       // stage 2's base is 0 while it is blocked
		if (u1 > 0.f)
		{
			if (u1 > 1.f) u1 = 1.f;
			reanchor(1, u1);
			st.inStage2 = true;
			if (carry) *carry = 1.f - f;
			return S + (1.f - S) * u1;
		}
		reanchor(1, 0.f);                // on the corner, pushing stage 2 below its start
		return S;
	}

	if (w1 > 0.f)
	{
		if (w1 > 1.f) { reanchor(1, 1.f); w1 = 1.f; }
		reanchor(0, 1.f);                // stage 1 blocked: kept at the hand
		return S + (1.f - S) * w1;
	}

	float prev = (prevValue - S) / (1.f - S);
	if (prev > 1.f) prev = 1.f;
	const float f = (prev > kAtEnd) ? prev / (prev - w1) : 0.f;
	reanchor(1, 0.f);                    // at its start stop; the overshoot is dropped
	float u0 = 1.f + (1.f - f) * (w0 - 1.f);   // stage 1's base is 1 while it is blocked
	if (u0 < 1.f)
	{
		if (u0 < 0.f) u0 = 0.f;
		reanchor(0, u0);
		st.inStage2 = false;
		if (carry) *carry = 1.f - f;
		return S * u0;
	}
	reanchor(0, 1.f);                    // on the corner, pushing stage 1 past its end
	return S;
}

// A staged drive's pose at combined value v, in the mesh's own space.
//
// Below the split, stage 1 alone. Above it, stage 1 complete and stage 2
// applied ON TOP of it, in the mesh frame:
//     x' = R2 (R1 x + o1) + o2  =  (R2 R1) x + (R2 o1 + o2)
// multQuaternion builds the standard rotation matrix of a unit Hamilton
// quaternion, for which R(q2 q1) = R(q2) R(q1), so the combined turn is the
// product q2 q1. R2 o1 goes through HandDrive_RotateByQuat, the draw's own matrix.
// At v = split, stage 2 is the identity and this is exactly stage 1's end pose,
// so the two sides of the split meet.
void HandDrive_StagedPose(const FHandDrive &d, float v, FVector3 &offset, FVector4 &rotation)
{
	if (d.stage2Kind == HANDDRIVE_None)
	{
		StagePose(d, 0, v, offset, rotation);
		return;
	}

	const float S = d.split;
	float u0 = (v >= S) ? 1.f : v / S;
	if (u0 < 0.f) u0 = 0.f;
	StagePose(d, 0, u0, offset, rotation);

	float u1 = (v - S) / (1.f - S);
	if (u1 <= 0.f) return;
	if (u1 > 1.f) u1 = 1.f;

	FVector3 o2;
	FVector4 q2;
	StagePose(d, 1, u1, o2, q2);
	const FVector4 q1 = rotation;
	offset = HandDrive_RotateByQuat(q2, offset) + o2;
	rotation = FVector4(
		q2.W*q1.X + q2.X*q1.W + q2.Y*q1.Z - q2.Z*q1.Y,
		q2.W*q1.Y - q2.X*q1.Z + q2.Y*q1.W + q2.Z*q1.X,
		q2.W*q1.Z + q2.X*q1.Y - q2.Y*q1.X + q2.Z*q1.W,
		q2.W*q1.W - q2.X*q1.X - q2.Y*q1.Y - q2.Z*q1.Z);
}

// The value a FOLLOWER sees on a staged drive: the owner's solve, run on a copy
// of the state that is then discarded. Before the owner has armed the drive
// there are no anchors, and the value the drive resumed from is the honest
// answer, as for a plain drive.
float HandDrive_StagedValueForFollower(const FHandDrive &d, const FVector3 &handModel)
{
	if (!d.armed) return d.value;
	FHandDriveStagedState st = HandDrive_StagedLoad(d);
	return HandDrive_StagedSolve(d, handModel, d.value, st);
}

// The follower's branch of a driven part, from the live hand: a hinge or two-stage drive rides its own solve; a plain
// drive exactly as it always has.
void HandDrive_PoseForFollower(const FHandDrive &d, const VSMatrix &handMat, const VSMatrix &modelToWorld, FVector3 &offset, FVector4 &rotation)
{
	FVector3 handModel;
	const float proj = HandDrive_Projection(handMat, modelToWorld, d.axis, handModel);
	if (HandDrive_IsStaged(d))
		HandDrive_StagedPose(d, HandDrive_StagedValueForFollower(d, handModel), offset, rotation);
	else
		HandDrive_Pose(d, HandDrive_ValueForFollower(d, proj), offset, rotation);
}

// ---- THE OWNER'S DRAW ------------------------------------------------------------------------------------------
//
// A driven part ignores whatever script last wrote and computes its own position from the LIVE controller pose, on
// the frame being drawn. That is the whole point: the part and the hand come from one pose at one instant, so they
// cannot separate. A tic-driven part is smooth and still a tic behind.
void HandDrive_OwnerStep(FHandDrive &d, const VSMatrix &handMat, const VSMatrix &modelToWorld,
	FVector3 &offset, FVector4 &rotation, FHandDriveStep &step, uint64_t frame)
{
	step = FHandDriveStep();
	step.staged = HandDrive_IsStaged(d);

	// PUBLISH THE OFF HAND'S SLIDE (HandDrive_OffhandSlideTravel): the travel along the drive axis, and any second
	// stage that also slides, carried through modelToWorld's own scale into map units.
	if (d.on && d.hand == 1 && !d.hinge)
	{
		const float *m = modelToWorld.get();
		auto worldLen = [m](const FVector3 &v, float amount)
		{
			const double x = (m[0] * v.X + m[4] * v.Y + m[8]  * v.Z) * amount;
			const double y = (m[1] * v.X + m[5] * v.Y + m[9]  * v.Z) * amount;
			const double z = (m[2] * v.X + m[6] * v.Y + m[10] * v.Z) * amount;
			return sqrt(x * x + y * y + z * z);
		};
		double travel = worldLen(d.axis, d.dist);
		if (d.stage2Kind == HANDDRIVE_Slide) travel += worldLen(d.stage2Axis, d.stage2Amount);
		HandDrive_OffhandSlideTravel = travel;
		HandDrive_OffhandSlideMs = I_msTime();
	}

	// A SECOND CALL ON A STAMPED FRAME replays what the first drew (FHandDrive::stampFrame).
	if (frame != 0 && d.stampFrame == frame)
	{
		step.replayed  = true;
		step.prevValue = d.value;
		step.value     = d.value;
		step.st        = HandDrive_StagedLoad(d);
		step.wasStage2 = step.st.inStage2;
		offset   = d.stampOffset;
		rotation = d.stampRotation;
		return;
	}

	if (step.staged)
	{
		// The same hand, in the same mesh space, as the plain branch. Only its
		// position is used here; each stage measures it in its own terms
		// (StageMeasure).
		HandDrive_Projection(handMat, modelToWorld, d.axis, step.handModel);

		// ARM, DON'T ANCHOR: the plain branch's rule, per stage. On a frame the
		// hand sits on a hinge's line nothing arms and the value holds; the next
		// drawn frame tries again (see HandDrive_StagedArm).
		if (!d.armed && HandDrive_StagedArm(d, step.handModel)) step.armedNow = true;

		step.st = HandDrive_StagedLoad(d);
		step.wasStage2 = step.st.inStage2;
		step.prevValue = d.value;
		// Not armed yet (the hand on a hinge's line since the grab): hold the
		// value the drive resumed from, as a follower does.
		float v = step.prevValue;
		if (d.armed)
		{
			v = HandDrive_StagedSolve(d, step.handModel, step.prevValue, step.st, &step.carry);
			HandDrive_StagedStore(d, step.st);
			d.value = v;
		}
		step.value = v;

		// The hinge, or both stages composed -- see HandDrive_StagedPose.
		HandDrive_StagedPose(d, v, offset, rotation);
	}
	else
	{
		// NO UNIT CONSTANT HERE, DELIBERATELY, AND NONE IS NEEDED. Whatever
		// scale this model's path applied is inside modelToWorld, so inverting
		// it undoes that scale along with everything else. The hand lands in the
		// SAME space the mesh's own vertices are in, which is the space `axis`
		// and `distance` are measured in. A conversion that is derived rather
		// than remembered cannot be forgotten.
		step.proj = HandDrive_Projection(handMat, modelToWorld, d.axis, step.handModel);
		step.prevValue = d.value;

		// ARM, DON'T ANCHOR. The anchor is captured HERE, on the first drawn
		// frame, from the same live quantity it will be differenced against.
		if (!d.armed)
		{
			d.anchor = step.proj;
			d.armed = true;
			step.armedNow = true;
		}

		const float dist = (d.dist != 0.f) ? d.dist : 1.f;
		float v = d.base + (step.proj - d.anchor) / dist;

		// RE-ANCHOR AT THE CLAMP. Without this, travel past an end is remembered
		// and has to be un-travelled before the part moves again -- you overshoot
		// a 7cm stroke by 30cm on a real pull, then wonder why the magazine
		// ignores the first third of your push back.
		if (v < 0.f)
		{
			d.anchor = step.proj;
			d.base = 0.f;
			v = 0.f;
		}
		else if (v > 1.f)
		{
			d.anchor = step.proj;
			d.base = 1.f;
			v = 1.f;
		}

		d.value = v;
		step.value = v;

		// The slide and the turn -- see HandDrive_Pose.
		HandDrive_Pose(d, v, offset, rotation);
	}

	if (frame != 0)
	{
		d.stampFrame    = frame;
		d.stampOffset   = offset;
		d.stampRotation = rotation;
	}
}
