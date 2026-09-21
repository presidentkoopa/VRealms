/*
** model_handdrive.h
**
** RS FORK -- THE HAND DRIVE: a rigid part placed from the live controller, on the frame being drawn.
**
** ONE DRIVE'S STATE (FHandDrive) AND THE PURE SOLVER FOR IT, shared by every kind of part a hand works:
**   * a mesh SURFACE -- DActorModelData::SurfDrive, set by Actor.SetModelSurfaceDrive and its neighbours, drawn by
**     RenderModelFrame and ridden by followers through SurfaceSlotPoseForFollower (models.cpp);
**   * a rig JOINT -- Engine docs/MODEL_JOINT_DRIVE_PLAN.md piece C.
** One solver, so a surface and a joint can never disagree about how a hand moves a part, and so a tic-time evaluator
** on a networked hand (NETWORK_HAND_INPUT_PLAN.md 4.4) can run the same arithmetic as the picture.
**
** MOVED, NOT REWRITTEN (plan piece A, 2026-09-15). These are models.cpp's SurfaceHandProjection, SurfaceDrivePose,
** SurfaceDriveValueForFollower and the staged-drive functions through SurfaceStagedValueForFollower, plus the owner's
** arm / solve / publish that RenderModelFrame did inline -- with the 16-slot SurfOvDrive* arrays of DActorModelData
** folded into one struct per drive. The formulas and their comments are the originals; a replay harness compared old
** and new over recorded hand paths before the move was merged.
**
** DEPENDS ONLY ON vectors AND VSMatrix: no actor, no renderer, no VR runtime. The caller brings the hand's matrix and
** the model's drawn matrix.
**
** NOT SERIALIZED: a drive is a live hand, and a save has no hand in it to restore.
*/

#pragma once

#include <cstdint>
#include "vectors.h"

class VSMatrix;

// A drive stage's motion. The values match DRIVESTAGE_* in actor.zs.
enum
{
	HANDDRIVE_None  = 0,
	HANDDRIVE_Slide = 1,
	HANDDRIVE_Hinge = 2,
};

struct FHandDrive
{
	// ---- THE PLAIN DRIVE (SetModelSurfaceDrive) ---------------------------
	//
	// ARMED, NOT ANCHORED. The setter turns a drive on and says which hand, which axis and how far; it does NOT
	// record where the hand was. The owner's draw captures the anchor itself, on the first frame it draws -- from the
	// same live quantity it will difference against. Capturing at script rate instead would bake in one tic of stale
	// hand position at the instant of grab, which on a short stroke is most of the travel: the part would leap most of
	// the way out the moment you touched it, and only when you grabbed FAST.
	//
	// value is published BACK by the owner's draw so script reads the same number that was drawn, rather than a second
	// estimate of it.
	bool     on     = false;
	int      hand   = 0;      // 0 main, 1 off
	FVector3 axis   = {};     // unit, model space
	float    dist   = 0.f;    // model units for full travel
	float    base   = 0.f;    // value the drive resumed FROM
	float    anchor = 0.f;    // captured by the owner's draw
	bool     armed  = false;  // false until the anchor is captured
	float    value  = 0.f;    // published back: what was DRAWN, 0..1

	// ---- A DRIVEN PART MAY ALSO TURN AS IT TRAVELS (SetModelSurfaceDriveRotation) ----
	//
	// At drive value v the part turns v * turnDeg about turnAxis through turnPivot, then slides along the drive axis --
	// one motion, still glued to the hand: a magazine that rocks into its well, a bolt handle that lifts as it draws
	// back. INERT UNTIL SET: the plain setter resets turnDeg to 0, a pure slide. All three are in the mesh's own space.
	FVector3 turnAxis  = {};   // unit, model space
	float    turnDeg   = 0.f;  // degrees at value 1; 0 = no turn
	FVector3 turnPivot = {};   // model space

	// ---- A DRIVE THAT ONLY TURNS, AND A DRIVE IN TWO STAGES ----------------
	//
	// hinge makes the drive itself a hinge: the part turns value * turnDeg about turnAxis through turnPivot with no
	// slide, and the hand is read by its ANGLE about that line (SetModelSurfaceDriveHinge).
	//
	// stage2* attaches a SECOND motion to the same drive. One drawn value 0..1: [0, split] is the drive above (stage
	// 1), [split, 1] is this one (stage 2), and stage 1 is held fully applied for all of stage 2. Stage 2's axis and
	// pivot are in the mesh's own space where they stand once stage 1 has finished (SetModelSurfaceDriveStage).
	//
	// INERT UNTIL SET, like the turn: the plain setter and the clear reset both, and a drive with neither takes the
	// plain branch exactly as it was before these existed.
	bool     hinge        = false;  // stage 1 is the turn above, with no slide
	uint8_t  stage2Kind   = 0;      // HANDDRIVE_None, _Slide or _Hinge
	FVector3 stage2Axis   = {};     // unit, model space
	float    stage2Amount = 0.f;    // model units (slide) or degrees (hinge) at full travel
	FVector3 stage2Pivot  = {};     // model space, hinge only
	float    split        = 0.f;    // drawn value where stage 1 ends, 0 < split < 1

	// The owner's own state for those drives: armed, re-anchored and written back by the owner's draw, exactly as
	// anchor / base are for a plain drive. [k] is the stage, 0 first and 1 second. stageBase is in that STAGE's own
	// 0..1, not the combined value.
	bool     inStage2       = false;   // which stage the hand is working
	float    stageAnchor[2] = {};      // hand measure at the anchor: units along, or degrees round
	float    stageBase[2]   = {};      // stage travel the anchor stands for
	FVector3 hingeRefX[2]   = {};      // hinge: in-plane direction its angle is measured from
	FVector3 hingeRefY[2]   = {};      // hinge: the direction the part turns toward from hingeRefX

	// ---- THE FRAME STAMP (HandDrive_OwnerStep given a frame number) --------
	//
	// A caller that may draw the same part more than once in one frame -- once per eye, or on a cache miss -- passes
	// the frame number; the second call on the same frame replays the stamped pose instead of solving again. A caller
	// that passes 0 (the surface draw) solves every time, exactly as before stamps existed. A setter that re-arms the
	// drive clears the stamp.
	uint64_t stampFrame    = 0;
	FVector3 stampOffset   = {};
	FVector4 stampRotation = {};
};

// A staged drive's solve state, copied out of the drive so a follower can run the owner's solve on a copy and throw
// the copy away.
struct FHandDriveStagedState
{
	bool  inStage2;
	float anchor[2];
	float base[2];
};

// What HandDrive_OwnerStep did this call, for the caller's diagnostics.
struct FHandDriveStep
{
	bool     staged    = false;   // a hinge or a two-stage drive
	bool     armedNow  = false;   // the drive armed on this call
	bool     wasStage2 = false;   // staged: the stage the hand was working before this solve
	bool     replayed  = false;   // a second call on a stamped frame: nothing solved
	float    prevValue = 0.f;     // the value drawn last time
	float    value     = 0.f;     // the value drawn now
	float    carry     = 0.f;     // staged: share of this frame's hand motion that crossed into the new stage
	float    proj      = 0.f;     // plain: the hand along the axis
	FVector3 handModel = {};      // the hand in the mesh's own space
	FHandDriveStagedState st = {};// staged: the solve state stored
};

// Where a hand is along a part's travel axis, in the model's own space, and the hand's model-space position.
float HandDrive_Projection(const VSMatrix &handMat, VSMatrix modelToWorld, const FVector3 &axis, FVector3 &handModel);

// A plain driven part at travel v (0..1): slid along its axis, and turned about its pivot if it has a turn.
void HandDrive_Pose(const FHandDrive &d, float v, FVector3 &offset, FVector4 &rotation);

// The travel a FOLLOWER sees on a plain drive: the owner's formula without the owner's writes.
float HandDrive_ValueForFollower(const FHandDrive &d, float proj);

// A hinge or a two-stage drive (the staged functions), rather than a plain one.
bool HandDrive_IsStaged(const FHandDrive &d);

// v turned by quaternion q, through the matrix the part is drawn with.
FVector3 HandDrive_RotateByQuat(const FVector4 &q, const FVector3 &v);

// A turn of `deg` about unit `axis` through pivot P, as a part transform: the quaternion, and the offset P - RP.
void HandDrive_TurnAboutPivot(const FVector3 &axis, float deg, const FVector3 &P, FVector3 &offset, FVector4 &rotation);

// ARM, DON'T ANCHOR, for a staged drive: every stage anchored at once, all or nothing.
bool HandDrive_StagedArm(FHandDrive &d, const FVector3 &hm);

FHandDriveStagedState HandDrive_StagedLoad(const FHandDrive &d);
void HandDrive_StagedStore(FHandDrive &d, const FHandDriveStagedState &st);

// ONE HAND, ONE VALUE, TWO MOTIONS: the value drawn this frame, leaving in `st` the state the owner stores.
float HandDrive_StagedSolve(const FHandDrive &d, const FVector3 &hm, float prevValue, FHandDriveStagedState &st, float *carry = nullptr);

// A staged drive's pose at combined value v, in the mesh's own space.
void HandDrive_StagedPose(const FHandDrive &d, float v, FVector3 &offset, FVector4 &rotation);

// The value a FOLLOWER sees on a staged drive: the owner's solve, run on a copy of the state.
float HandDrive_StagedValueForFollower(const FHandDrive &d, const FVector3 &handModel);

// The pose something riding a driven part draws it at this frame, from the hand: the follower's branch, plain or staged.
void HandDrive_PoseForFollower(const FHandDrive &d, const VSMatrix &handMat, const VSMatrix &modelToWorld, FVector3 &offset, FVector4 &rotation);

// THE OWNER'S DRAW OF A DRIVEN PART: arm if not armed, solve, publish the drawn value into d.value, and pose the part.
// frame 0 solves every call; a frame number replays a second call on the same frame (see FHandDrive::stampFrame).
// THE OFF HAND IS WORKING A SLIDE, AND HOW FAR IT GOES. Written by every owner's draw of a live off-hand drive
// that slides (not a pure hinge): the full travel in map units, and when. The support pin (vk_openxrdevice.cpp,
// [SUPPORT PIN]) reads it to let a hand pinned to the gun follow the real one back along the barrel -- on a pump's
// forend, and nowhere else -- by exactly as far as the part itself can go. Stale after 200 ms: the drive stopped.
extern double   HandDrive_OffhandSlideTravel;
extern uint64_t HandDrive_OffhandSlideMs;

void HandDrive_OwnerStep(FHandDrive &d, const VSMatrix &handMat, const VSMatrix &modelToWorld,
	FVector3 &offset, FVector4 &rotation, FHandDriveStep &step, uint64_t frame = 0);
