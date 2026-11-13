/*
** vr_handpose.cpp
**
** RS FORK -- THE VR HANDS, engine half (see vr_handpose.h; the math is vr_handpose_math.h).
**
**---------------------------------------------------------------------------
**
** PORT CHECKLIST -- UZDXREMA (read-only, HEAD 39e4dad) -> here. The acceptance test for this port.
** "VAP" = UZDXREMA src/r_data/vr_avatar_pose.cpp, "RIG" = src/r_data/vr_rig.cpp,
** "OXR" = src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp, "VRM" = hw_vrmodes.h.
** "math" = r_data/vr_handpose_math.h, "dev" = this engine's vk_openxrdevice.cpp.
**
** Inputs
**  1. VRFingerInput {trigger, grip, triggerTouch, thumbTouch}  VRM:82-88      -> hw_vrmodes.h VRFingerInput
**  2. VRMode::GetFingerInput (default false)                    VRM:261        -> hw_vrmodes.h
**  3. finger values copied out of handInput, per controller     OXR:4343-4349  -> dev, after updateHandPose
**  4. GetFingerInput: role -> controller (VR_ControllerForHand) OXR:7957-7963  -> dev GetFingerInput
**  5. thumb touch = thumbrest|thumbstick|X|Y|A|B touch,
**     trigger touch = trigger/touch (Oculus Touch)              OXR:2789-2798  -> already in this engine
**     (dev, Touch bindings); ADDED: Index (thumbstick, trackpad, A/B touch; trigger touch), Vive and WMR
**     (trackpad touch). UZDXREMA binds touch for Oculus Touch only.
**  6. desktop source: fire/altfire as 0/1 on the main hand      RIG:614-624    -> NOT PORTED: no hands in flat
**     (owner: nothing in flat); the flat TEST supplies its own inputs (roth_handtest, item 33).
**
** The curl rule (VAP UpdateCurls :419-698), items 7-22: REMOVED (owner, 2026-11-13: "start over, use
** the hands in DXR"). They drove the fingers toward RS_Hands' pose frames (0 open, 1 point, 3 fist,
** 7 grip_tu, 8 ready_td) and RS_Hands' thumb pairing, which the owner never gave. Kept: the input
** reading (1-5), the rate (math Retain/SanitiseDt), QSlerp and Smooth (math), for when the DXR hands'
** fingers are posed the way the owner says.
** 23. [FINGERLEN] finger bones lengthened per body               VAP:2336-2400  -> NOT PORTED: one rig, its own size.
** 24. the grasp (vr_grasp.cpp)                                                 -> NOT PORTED (lead: later task).
**
** Placement (RIG, OXR)
** 25. grip pose located beside the aim pose, HMD-relative, yaw-corrected, raw quaternion
**                                                               OXR:4350-4374  -> dev, after updateHandPose
**     (RigPoseOverride, UZDXREMA's Linux headless rig, not ported: this engine has no such rig).
** 26. OffsetToWorld / GetGripTransform                          OXR:7879-7921  -> dev (GRIPCHECK harness :7848-7874,
**     :7922-7951 not ported: it compared against a construction this engine never had).
** 27. DefaultHandFit (FRIK hand axes from OpenXR grip axes)     RIG:422-460    -> math DefaultHandFit, verbatim
** 28. HandFitOffset (player's fit string vr_fit_hand_<avatar>)  RIG:514-554    -> NOT PORTED: no fit mode here; the
**     default fit is used, as UZDXREMA does for a body never fitted.
** 29. VRRig_CleanFrame (strip the metres and -Z, keep det -1)   RIG:1175-1205  -> NOT PORTED as such: the model is
**     seated in the uncleaned frame in METRES (VRHandPose_HandFrame) and ModelSeat carries the reflection,
**     so the whole chain is unmirrored by construction (tools/hands_test checks det and handedness).
** 30. hand frame from wrist + 4 knuckles, grip socket           RIG:733-750    -> math HandFrameFromJoints, verbatim
** 31. controller path: WRIST at handTarget (grip . fit)         VAP:1966-1969  -> DEPARTS: the grip SOCKET is put
**     at the grip origin, as UZDXREMA's gun-anchor path puts it on a gun's grip (VAP:1960-1965, 1981-1994):
**     OpenXR's grip position is the palm centroid within the handle, which is what the socket is. With
**     the wrist there the palm would sit a hand's width ahead of the real one.
** 32. "no controller, no hand"                                  VAP:1874-1877  -> ObjectToWorldMatrix falls back
**     to the actor's own place (a speck at metre scale) and roth_hands.cpp hides the actor.
**
** The flat test (not in UZDXREMA; test-only)
** 33. roth_handtest: both hands in front of the camera, fixed grip axes, through the placement above
**     (fit, seat). Inert until typed.
**
**---------------------------------------------------------------------------
*/

#include "vr_handpose.h"
#include "vr_handpose_math.h"
#include "model_reach_math.h"
#include "actor.h"
#include "model.h"
#include "model_fbx.h"
#include "models.h"
#include "hw_vrmodes.h"
#include "r_utility.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "v_video.h"
#include "printf.h"

EXTERN_CVAR(Int, vr_control_scheme)

using namespace VRHandPose;

//===========================================================================
//
// Roles and sides. UZDXREMA VAP:433-438: isMain = (side == right) == rightHanded.
//
//===========================================================================

static bool RightHanded() { return vr_control_scheme < 10; }

int VRHandPose_SideForRole(int role)
{
	const bool isMain = (role == VR_MAINHAND);
	return (isMain == RightHanded()) ? SIDE_RIGHT : SIDE_LEFT;
}

int VRHandPose_RoleForSide(int side)
{
	const bool isMain = (side == SIDE_RIGHT) == RightHanded();
	return isMain ? VR_MAINHAND : VR_OFFHAND;
}

//===========================================================================
//
// THE FLAT TEST. roth_handtest (src/roth/roth_hands.cpp) switches it on with a set of inputs; while
// it is on, both hands take those inputs and a grip frame in front of the camera instead of a
// controller's. Nothing else reads it.
//
// The test grip: the controller TEST_FWD metres ahead of the eye, TEST_SIDE to the side, TEST_UP up,
// built in the same render space and the same steps as OffsetToWorld (eye, metres with -Z, pixel
// stretch, body yaw) so the rest of the hand path cannot tell it from a real grip. Its axes put the
// palms toward the camera with the fingers up (right: X_grip forward, Y_grip down, Z_grip left; left:
// X_grip back, Y_grip down, Z_grip right -- DefaultHandFit then gives fingers up, back of the hand away,
// each thumb on its own outer side). `turn` swings each palm inward about the vertical, so 90 shows
// the fingers' curl in profile. Yaw only: the shots this serves are level.
//
//===========================================================================

namespace
{
	struct FHandTest
	{
		bool on = false;
		FVRHandTestInputs in;
		float turn = 0.f;
	};
	FHandTest gTest;

	constexpr double TEST_FWD = 0.40, TEST_SIDE = 0.11, TEST_UP = -0.04;	// metres

	// Columns X_grip, Y_grip, Z_grip in the camera-local frame (+x right, +y up, +z back).
	const float kTestGripAxes[SIDE_COUNT][16] =
	{
		{ 0, 0, -1, 0,   0, -1, 0, 0,   -1, 0, 0, 0,   0, 0, 0, 1 },	// right
		{ 0, 0,  1, 0,   0, -1, 0, 0,    1, 0, 0, 0,   0, 0, 0, 1 },	// left
	};

	bool TestGrip(int role, VSMatrix &out)
	{
		if (!gTest.on) return false;
		const int side = VRHandPose_SideForRole(role);
		const float upm = VR_UnitsPerMeter();
		const double ps = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
		const DVector3 &eye = r_viewpoint.Pos;
		const double yaw = r_viewpoint.Angles.Yaw.Degrees();

		out.loadIdentity();
		out.translate((float)eye.X, (float)eye.Z, (float)eye.Y);
		out.scale(upm, upm, -upm);
		out.scale(1.f, 1.f / (float)ps, 1.f);
		out.rotate((float)(-90.0 + yaw), 0.f, 1.f, 0.f);
		out.translate((float)(side == SIDE_RIGHT ? TEST_SIDE : -TEST_SIDE), (float)TEST_UP, (float)-TEST_FWD);
		out.rotate(side == SIDE_RIGHT ? -gTest.turn : gTest.turn, 0.f, 1.f, 0.f);
		out.multMatrix(kTestGripAxes[side]);
		return true;
	}
}

void VRHandTest_Set(bool on, const FVRHandTestInputs &in, float turnDeg)
{
	gTest.on = on;
	gTest.in = in;
	gTest.turn = turnDeg;
}

bool VRHandTest_Active() { return gTest.on; }

void VRHandTest_ActorSpot(int side, const DVector3 &eye, double yawDeg, DVector3 &out)
{
	const double upm = VR_UnitsPerMeter();
	const DAngle yaw = DAngle::fromDeg(yawDeg);
	const DVector3 fwd(yaw.Cos(), yaw.Sin(), 0.0);
	const DVector3 right(yaw.Sin(), -yaw.Cos(), 0.0);
	const double s = (side == SIDE_RIGHT) ? TEST_SIDE : -TEST_SIDE;
	out = eye + fwd * (TEST_FWD * upm) + right * (s * upm) + DVector3(0, 0, TEST_UP * upm);
}

//===========================================================================
//
// THE HAND FRAME: grip x DefaultHandFit, in metres.
//
// UNITS. The grip frame is left in METRES -- NOT scaled by 1/VR_UnitsPerMeter() as the aim-pose path
// in ObjectToWorldMatrix is (that path makes one model unit one MAP unit, so a model's real size moves
// with the level's units per metre). GetGripTransform (and the test grip) multiply by
// VR_UnitsPerMeter() as read at this draw, so a model unit here is one real metre on every map, and a
// live change of the metre (roth_vr_units_per_metre, rothmap.cpp; the door anchor, 107.4) resizes the
// hand on the next frame with nothing else to update. The hand's MODELDEF Scale is therefore metres
// per model unit (1/93.5 for the converter's files), and everything applied inside this frame -- the
// seat's socket, MODELDEF Offset, FollowHandOfs -- is in metres too and scales with it.
//
//===========================================================================

bool VRHandPose_HandFrame(int role, VSMatrix &out)
{
	VSMatrix g;
	bool got = TestGrip(role, g);
	if (!got)
	{
		const VRMode *vm = VRMode::GetVRModeCached(true);
		got = vm != nullptr && vm->IsVR() && vm->GetGripTransform(role, &g);
	}
	if (!got) return false;
	float fit[16];
	DefaultHandFit(VRHandPose_SideForRole(role) == SIDE_RIGHT, fit);
	out = g;
	out.multMatrix(fit);
	return true;
}

//===========================================================================
//
// Per model: the finger joints, the five pose frames, the seat for each side. Built once, from the
// model's own bind pose and frames; a model that lacks any of it is left alone (and says so, once).
//
//===========================================================================

namespace
{
	const char *const kFingerJoints[FINGER_COUNT][FINGER_JOINTS] =
	{
		{ "THUMB_BASE_joint",    "THUMB_MID_joint",    "THUMB_TOP_joint" },
		{ "INDEX_BASE_joint",    "INDEX_MID_joint",    "INDEX_TOP_joint" },
		{ "MIDDLE_F_BASE_joint", "MIDDLE_F_MID_joint", "MIDDLE_F_TOP_joint" },
		{ "RING_BASE_joint",     "RING_MID_joint",     "RING_TOP_joint" },
		{ "PINK_BASE_joint",     "PINK_MID_joint",     "PINK_TOP_joint" },
	};
	// HandFrameFromJoints' P[5]: wrist, index, middle, ring, pinky knuckles (RIG:733-750).
	const char *const kFrameJoints[5] =
	{
		"Root_joint", "INDEX_BASE_joint", "MIDDLE_F_BASE_joint", "RING_BASE_joint", "PINK_BASE_joint"
	};

	struct FHandModelCache
	{
		FModel     *model = nullptr;
		int         numJoints = -1;
		TArray<int> parents;
		bool        seatOk[SIDE_COUNT] = { false, false };
		VSMatrix    seat[SIDE_COUNT];
	};
	TArray<FHandModelCache *> gModels;

	FHandModelCache *CacheFor(FModel *model)
	{
		for (auto *c : gModels) if (c->model == model && c->numJoints == model->NumJoints()) return c;

		FHandModelCache *c = new FHandModelCache;
		c->model = model;
		c->numJoints = model->NumJoints();
		gModels.Push(c);

		const int n = c->numJoints;
		const TArray<VSMatrix> *base = model->GetBasePose();
		if (n <= 0 || base == nullptr || (int)base->Size() != n) return c;

		c->parents.Resize(n);
		for (int i = 0; i < n; i++)
		{
			const int p = model->GetJointParent(i);
			if (p >= i) return c;	// FJointPoseWork needs parents first, as the IQM loader enforces
			c->parents[i] = p < 0 ? -1 : p;
		}

		// THE SEAT, both sides: the bind-pose wrist and knuckles, in the file's own space (baseframe).
		Vec3d P[5];
		bool havePoints = true;
		for (int k = 0; k < 5 && havePoints; k++)
		{
			const int j = model->FindJoint(FName(kFrameJoints[k]));
			if (j < 0 || j >= n) { havePoints = false; break; }
			const FLOATTYPE *m = (*base)[j].get();
			P[k] = Vec3d(m[12], m[13], m[14]);
		}
		Vec3d sock[SIDE_COUNT];
		for (int s = 0; s < SIDE_COUNT && havePoints; s++)
		{
			float m[16];
			if (ModelSeat(P, s == SIDE_RIGHT, m, &sock[s]))
			{
				c->seat[s].loadMatrix(m);
				c->seatOk[s] = true;
			}
		}

		Printf("VR hands: %s -- fingers not posed (no finger poses yet: to be built from the DXR hands) (%d frames), seat %s; grip socket R (%.2f %.2f %.2f) L (%.2f %.2f %.2f) file units\n",
			model->mFileName.GetChars(),
			model->NumFrames(),
			(c->seatOk[0] && c->seatOk[1]) ? "measured" : "NOT measured (wrist/knuckle joints missing)",
			sock[0].x, sock[0].y, sock[0].z, sock[1].x, sock[1].y, sock[1].z);
		return c;
	}

	// Which hand an actor is: the two classes roth_hands.cpp spawns (wadsrc zscript realmshands.zs).
	PClassActor *gHandCls[SIDE_COUNT] = { nullptr, nullptr };
	uint64_t gHandClsLook = ~0ull;

	int SideOfActor(const AActor *a)
	{
		if (a->FollowHandMode != FOLLOWHAND_GRIP_MAIN && a->FollowHandMode != FOLLOWHAND_GRIP_OFF) return -1;
		if (gHandCls[0] == nullptr || gHandCls[1] == nullptr)
		{
			const uint64_t fr = screen != nullptr ? screen->FrameCount : 0;
			if (fr == gHandClsLook) return -1;
			gHandClsLook = fr;
			gHandCls[SIDE_RIGHT] = PClass::FindActor("RealmsHandRight");
			gHandCls[SIDE_LEFT]  = PClass::FindActor("RealmsHandLeft");
		}
		if (gHandCls[SIDE_RIGHT] != nullptr && a->IsKindOf(gHandCls[SIDE_RIGHT])) return SIDE_RIGHT;
		if (gHandCls[SIDE_LEFT] != nullptr && a->IsKindOf(gHandCls[SIDE_LEFT])) return SIDE_LEFT;
		return -1;
	}

}

bool VRHandPose_ModelSeat(FModel *model, int side, VSMatrix &out)
{
	if (model == nullptr || side < 0 || side >= SIDE_COUNT) return false;
	FHandModelCache *c = CacheFor(model);
	if (c == nullptr || !c->seatOk[side]) return false;
	out = c->seat[side];
	return true;
}

//===========================================================================
//
// THE FINGERS ON THE PALETTE: none yet. The finger poses were RS_Hands' frame map and are removed
// (owner, 2026-11-13); the palette is drawn as the model's own frame. The FBX hands' fingers will be
// posed from the DXR hands once the owner says how.
//
//===========================================================================

const TArray<VSMatrix> *VRHandPose_Apply(const AActor *actor, FModel *model, int modelIndex, const TArray<VSMatrix> &bones)
{
	(void)actor; (void)model; (void)modelIndex;
	return &bones;
}
