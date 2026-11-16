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
namespace VRHandPose { struct HeldState; }

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

// THE ONE HAND FRAME EVERY READER USES (2026-11-16): VRHandPose_HandFrame -- the very matrix the drawn hand
// is seated in -- in MAP coordinates (r_data/vr_posemath.h VRPose_HandMapFrame): origin at the grip socket,
// `fwd` along the hand (wrist to knuckles), `right` to the hand's right, `up` = right x fwd (the thumb's
// side). Hand-locked billboards (g_levellocals.h BillboardHandFrame), the picker's controller ray and its
// locks, the panels' pointer and the held objects all read THIS, never AttackAngle / AttackPitch /
// MainHandRoll: those are the gun's AIM pose (another tracked pose, with the device's -40 degree pitch bias)
// in the playsim's stored convention, and three readers took its pitch with the wrong sign. False: no grip.
bool VRHandPose_MapFrame(int role, DVector3 &origin, DVector3 &fwd, DVector3 &right, DVector3 &up);

// THE HANDS' INPUT DOOR: a role's grip transform and finger values from the injected controller pose
// (roth_xrpose, below) when that is on, else the headset's (VRMode::GetGripTransform / GetFingerInput, only
// while the VR mode is VR). Every game-layer reader of the controllers goes through these.
bool VRHandPose_Grip(int role, VSMatrix &out);
bool VRHandPose_Fingers(int role, struct VRFingerInput &out);
// A headset is on, or the controller-pose test drives the hands.
bool VRHandPose_InputLive();

// ---- THE CONTROLLER-POSE TEST (roth_xrpose): test-only, inert until typed ----
// Not roth_handtest's camera-relative grip: given OpenXR TRACKING-SPACE poses (metres, +X right, +Y up,
// -Z ahead; a quaternion) for the head and each hand, the grip goes through the device's own arithmetic
// (vr_posemath.h VRPose_HeadRelativeOffset, VRPose_OffsetToWorld), with the player's view yaw as the body
// yaw and the eye at the head, so everything downstream -- the drawn hands, their fingers, the picker, the
// hand-locked billboards -- runs exactly as in the headset.
struct FVRPoseTestHand
{
	bool valid = false;
	float pos[3] = { 0, 0, 0 };      // tracking space, metres
	float quat[4] = { 0, 0, 0, 1 };  // tracking space, the grip pose's orientation (x, y, z, w)
	float trigger = 0.f, grip = 0.f;
	bool triggerTouch = false, thumbTouch = false;
};
void VRPoseTest_Set(bool on, float hmdYawDeg, const float hmdPos[3]);
void VRPoseTest_SetHand(int role, const FVRPoseTestHand &h);
bool VRPoseTest_Active();
bool VRPoseTest_Hand(int role, FVRPoseTestHand &out);
// The hmd pose the test gives (tracking space): position, yaw. False while off.
bool VRPoseTest_Head(float pos[3], float &yawDeg);
// Each hand's AIM pose (the gun's: OpenXR's aim space), tracking space. Unset, it is the grip turned so the
// aim's -Z runs along the fingers (the grip x Rx(-90)).
void VRPoseTest_SetAim(int role, const float quat[4]);
// WHAT THE DEVICE PUBLISHES ON THE PAWN, from the test's poses (vr_posemath.h VRPose_AimAngles,
// VRPose_PublishedAim, OffsetToWorld's origin for the position): AttackPos / AttackAngle / AttackPitch /
// AttackRoll / MainHandRoll and the Offhand* five. Called by the playsim (p_user.cpp
// UpdateCanonicalMainHandPose) after its own canonical writes, so scripts that read those fields -- RSQ
// Distance Grab -- see the test's hands as they see a headset's. Single player only.
void VRPoseTest_PublishAim(AActor *mo);

// The model's seat in that hand frame (vr_handpose_math.h ModelSeat), measured once per model from
// its own bind pose. False when the model lacks the wrist or knuckle joints.
bool VRHandPose_ModelSeat(FModel *model, int side, VSMatrix &out);

// The finger pose, on a finished bone palette (models.cpp RenderModelFrame, beside
// ModelDrawPose_Apply): UZDXREMA's curl rule (trigger, grip, touches -> how far each finger folds,
// smoothed at display rate) turning each finger joint from the model's drawn frame toward its own
// take's fist frame. Returns &bones untouched unless `actor` is a VR hand and `model` carries the
// hand's joints; otherwise a posed copy, valid until the next call on this thread.
const TArray<VSMatrix> *VRHandPose_Apply(const AActor *actor, FModel *model, int modelIndex, const TArray<VSMatrix> &bones);

// WHAT EACH HAND HOLDS (UZDXREMA vr_rig.cpp:1802-1853: VRRig_HandHoldsWeapon, VRRig_HandGripPose,
// VRRig_HandPropClass, VRRig_HandGripSubject), per ROLE (VR_MAINHAND / VR_OFFHAND), published by the game
// layer once per tic (src/roth/roth_hands.cpp). The fingers then take the owner's held-object or weapon-grip
// pose (vr_handpose_math.h SelectHold, VAP :466-656) while it holds. A default HeldState = holding nothing.
void VRHandPose_SetHeld(int role, const VRHandPose::HeldState &held);

// THE INDEX FINGER AND THE PALM, for pointing and touching (src/roth/roth_vrpick.cpp): the index
// fingertip (INDEX_UP_TOP_joint), its knuckle (INDEX_BASE_joint), the palm (HANDPALM_joint), and the
// wrist and middle knuckle (the hand's axis, which a curled index does not follow), in
// the model's own render space -- the space ModelPointToWorld takes, so the caller turns them into
// map positions through the hand actor's real draw transform (Model_ActorPointToWorld). `posed` is
// true when they come from the fingers as last drawn (VRHandPose_Apply, within a few frames), false
// when the model's rest pose -- the open hand -- stood in. False: the model lacks those joints.
struct FVRHandPoints
{
	DVector3 tip, indexBase, palm;
	DVector3 wrist, middleBase;	// Root_joint and MIDDLE_F_BASE_joint: the hand's own axis, wrist to knuckles
	bool posed = false;
};
bool VRHandPose_FingerPoints(FModel *model, int side, FVRHandPoints &out);

// ---- the flat test (roth_handtest): test-only, inert until the command is typed ----
struct FVRHandTestInputs
{
	float trigger = 0.f, grip = 0.f;
	bool  triggerTouch = false, thumbTouch = false;
};
void VRHandTest_Set(bool on, const FVRHandTestInputs &in, float turnDeg);
bool VRHandTest_Active();
// The test hands' fingers tipped from up (0) toward straight ahead (90), about the camera's right.
void VRHandTest_SetTilt(float tiltDeg);
// The flat test's hands raised (+) or lowered (-) by this many metres from their place below the eye: the
// VR picker's flat test (roth_vrpicktest seek) brings the palm to a thing's height, as a player reaches
// down to a table. 0 = the usual place. Test-only.
void VRHandTest_SetLift(double metres);
double VRHandTest_Lift();
// The flat test's hand of a PHYSICAL side moved from its place by (fwd, right, up) metres in the camera's own
// frame (yaw only): the gesture test's scripted motion (roth_gesturetest move, src/roth/roth_gestures.cpp), so
// a test hand can swing, rap, push and wave. 0, 0, 0 = its usual place. Test-only.
void VRHandTest_SetOffset(int side, double fwd, double right, double up);
void VRHandTest_Offset(int side, double &fwd, double &right, double &up);
// The flat test's held thing for a PHYSICAL hand (VRHandPose::SIDE_*), in place of the game's while `on`
// (roth_handhold, src/roth/roth_hands.cpp).
void VRHandTest_SetHeld(int side, bool on, const VRHandPose::HeldState &held);
// The inputs the flat test gives both hands. False while it is off.
bool VRHandTest_Inputs(FVRHandTestInputs &out);
// Where roth_hands.cpp stands the test hand actors, in map units (so they are not culled): the
// camera-relative spot the test grip uses, from the player's eye and yaw.
void VRHandTest_ActorSpot(int side, const DVector3 &eye, double yawDeg, DVector3 &out);
