/*
** vr_handpose.h
**
** RS FORK -- THE VR HANDS: two hand models, each riding its controller's GRIP pose at draw rate,
** their fingers posed every displayed frame from that controller's trigger, squeeze and capacitive
** touches. The math is vr_handpose_math.h; this is the engine half. The hand ACTORS are spawned by
** the game layer (src/roth/roth_hands.cpp); the MODELDEF is vrealms_iwad/MODELDEF.txt.
**
** PRESENTATION ONLY. Nothing here is read by the playsim or saved: the finger pose and the hand's
** place are this machine's pixels, read straight from the VR device at draw time (UZDXREMA reads
** them the same way, vr_rig.cpp FVRRig::Recompute -> GetFingerInput). Nothing goes through
** usercmd_t because nothing the game decides depends on it.
**
**---------------------------------------------------------------------------
*/

#pragma once

#include "tarray.h"
#include "vectors.h"
#include "matrix.h"

class AActor;
class FModel;

// AActor::FollowHandMode values that ride a controller's GRIP pose instead of its aim pose
// (models.cpp ObjectToWorldMatrix). 3 = the main hand's controller, 4 = the off hand's.
enum
{
	FOLLOWHAND_GRIP_MAIN = 3,
	FOLLOWHAND_GRIP_OFF  = 4,
};

// Which physical hand (VRHandPose::SIDE_RIGHT / SIDE_LEFT) a role (VR_MAINHAND / VR_OFFHAND) is,
// by the player's handedness (vr_control_scheme, as VR_ControllerForHand reads it).
int  VRHandPose_SideForRole(int role);
// And the other way: the role whose controller a physical hand rides.
int  VRHandPose_RoleForSide(int side);

// The hand frame of `role`'s controller this frame, in world render space, unit = one METRE:
// the controller's grip pose (VRMode::GetGripTransform) times UZDXREMA's DefaultHandFit. The
// flat test (roth_handtest) supplies a grip in front of the camera instead. False: no grip now.
bool VRHandPose_HandFrame(int role, VSMatrix &out);

// The model's seat in that hand frame (vr_handpose_math.h ModelSeat), measured once per model from
// its own bind pose. False when the model lacks the wrist or knuckle joints.
bool VRHandPose_ModelSeat(FModel *model, int side, VSMatrix &out);

// The finger pose, on a finished bone palette (models.cpp RenderModelFrame, beside
// ModelDrawPose_Apply). Returns &bones untouched unless `actor` is a VR hand and `model` carries
// the hand's joints and poses; otherwise a posed copy, valid until the next call on this thread.
const TArray<VSMatrix> *VRHandPose_Apply(const AActor *actor, FModel *model, int modelIndex, const TArray<VSMatrix> &bones);

// ---- the flat test (roth_handtest): test-only, inert until the command is typed ----
struct FVRHandTestInputs
{
	float trigger = 0.f, grip = 0.f;
	bool  triggerTouch = false, thumbTouch = false;
};
void VRHandTest_Set(bool on, const FVRHandTestInputs &in, float turnDeg);
bool VRHandTest_Active();
// Where roth_hands.cpp stands the test hand actors, in map units (so they are not culled): the
// camera-relative spot the test grip uses, from the player's eye and yaw.
void VRHandTest_ActorSpot(int side, const DVector3 &eye, double yawDeg, DVector3 &out);
