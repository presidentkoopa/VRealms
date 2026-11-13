/*
** vr_handpose.cpp
**
** RS FORK -- THE VR HANDS, engine half (see vr_handpose.h; the math is vr_handpose_math.h).
**
**---------------------------------------------------------------------------
**
** PORT CHECKLIST -- UZDXREMA (read-only, HEAD 39e4dad) -> here. The acceptance test for this port.
** "VAP" = UZDXREMA src/r_data/vr_avatar_pose.cpp, "VA" = src/r_data/vr_avatar.cpp, "RIG" = src/r_data/vr_rig.cpp,
** "OXR" = src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp, "VRM" = hw_vrmodes.h, "MA" = tools/avatar/
** measure_avatar.py, "MKP" = tools/avatar/mkfbxposes.py. "math" = r_data/vr_handpose_math.h, "dev" = this engine's
** vk_openxrdevice.cpp. Verified items say how; the rest is marked [R] (reasoning only, not seen running).
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
** The curl rule and its smoothing (VAP UpdateCurls :419-698, empty hand)
**  7. once per displayed frame (rig.FrameIndex)                 VAP:421-423    -> UpdateCurls, screen->FrameCount
**  8. dt from screen->FrameTime ms; first frame/hitch -> 1/90   VAP:425-429    -> UpdateCurls, math SanitiseDt
**  9. retain = 0.75^(dt*90)                                     VAP:431        -> math Retain
** 10. which controller each side holds (handedness)             VAP:433-438    -> VRHandPose_RoleForSide
** 11. index = trigger > 0 ? trigger : (triggerTouch ? 0.3 : 0)  VAP:444        -> math FingerCurls
** 12. middle = ring = pinky = grip                              VAP:445-447    -> math FingerCurls
** 13. thumb = thumbTouch ? 0.6 : 0                              VAP:448        -> math FingerCurls
** 14. no controller -> every finger 0                           VAP:450-454    -> math FingerCurls (!valid);
**     valid = the grip pose is there this frame (ReadInputs)
** 15. one value per finger copied to its 3 joints               VAP:456-458    -> math JointCurls
** 16. weapon in the hand: gun_grip / grip_<class> / grip_<prop>, the thumb round the handle, the index's
**     ready/fire blend                                          VAP:466-557    -> NOT PORTED: no weapon in these hands yet
** 17. an object in the hand: hold_<subject>[_<prop>|_<class>]   VAP:558-656    -> NOT PORTED: nothing is held yet
** 18. scalar curl smoothed, clamped 0..1 (FCurlState)           VAP:658-665    -> math CurlState/SmoothCurls (as in the
**     source, no bone reads it)
** 19. rotation target from the FRIK table: QAxisAngle(axis, degrees * curl); identity without a table
**                                                               VAP:685-690, 410-414 -> math JointTarget, QAxisAngle
** 20. [POSEROT] full-rotation target from two poses, QSlerp(a, b, t)
**                                                               VAP:674-681    -> math JointTarget: a = the FBX's open
**     frame, b = its fist frame, t = the curl (item 11-15). DEPARTS from the source's EMPTY hand (which uses item 19
**     only) on the owner's word, 2026-11-13: "walk the frames of animation if you have to but get me badass HQ VR
**     hands". Item 19 stays as the source's fallback for a joint with no frames (none on hand_final.fbx).
** 21. gRot: first frame takes the target, then slerp(cur, target, 1 - retain)
**                                                               VAP:691-696, 385-390 -> math SideState/Smooth
** 22. QSlerp: shortest arc, lerp past 0.9995, renormalised      VAP:392-408    -> math QSlerp
**     [V items 7-22: tools/hands_test test_curls -- VAP's own lines cut out and compiled beside the port, same inputs,
**      72/80/90/120/144 Hz with jitter, hitches and dropouts: every curl and quaternion identical (difference 0).]
**
** The curl table (VA :187-281)
** 23. vrhandcurl.txt, one lump, the owner's file unchanged       VA:192         -> vrealms_iwad/vrhandcurl.txt
**     [V: byte-identical, hands_test]; read with FScanner as VA reads it: `curl <finger> <1-3> <deg> <x y z>`, axis
**     normalised, zero axis refused, unknown key refused, all 15 rows or nothing -> LoadCurlTable here.
** 24. `pose` lines (FRIK's named poses) in the same file         VA:209-225     -> read and checked as VA reads them
**     (a bad finger refuses the table), NOT KEPT: only items 16-17 look them up.
** 25. VRHANDPOSE pack lumps (LoadPackPoses)                     VA:125-179     -> NOT PORTED: items 16-17 only.
**
** The canonical bone frame (MA, offline in UZDXREMA; here at load, from the model's own rest pose)
** 26. palm normal: f = middle_1 - hand, k = index_1 - pinky_1 squared off f, right k x f, left f x k
**                                                               MA:366-377     -> math MeasurePalm
** 27. per finger joint: X toward the furthest child (tipaxis: the previous joint's), Z = unit(X x Gr^T palm),
**     X re-squared, Y = Z x X; det and fold (Y.palm > 0.05) refused
**                                                               MA:476-555     -> math MeasureFingerFrames
**     [V: tools/fbxload_test measure_ref.py runs MA's own lines 362-556 on the loader's rest joints: palm and every
**      frame axis identical (difference 0), both hands.]
** 28. roles for hand_final.fbx: hand = Root_joint, 1/2/3 = BASE/MID/TOP (UP_TOP not a role)
**                                                               MKP FINGERS/SEGS, frames_for -> math kHandJoint, kFingerJointNames
**
** The FBX's frames as poses (MKP main())
** 29. D = local(ref)^T local(pose); q = quat(F^T D F), w >= 0   MKP:main, quat -> math PoseRotation
**     ref = the frame the model draws (0), equal to MKP's open frame (Blender 1 = frame 0 here).
** 30. open = fbx_poses.json "open" 1, fist = "fist" 319 (Blender) -> frames 0 and 318 of each hand's own take
**                                                               fbx_poses.json -> math kOpenFrame, kFistFrame
**     [V: fbxload_test F2 -- from frames 0 -> 318 the right hand's 15 rotations equal vrhandpose.txt's fbx_fist R to
**      0.131 deg, the left's fbx_fist L to 0.113 deg once read in MKP's mirrored frame; frames 317 and 319 are off by
**      1.3-3.6 deg. W -- the whole take walked, both hands.]
**
** The turn on the joints (VAP :1771-1838)
** 31. palette -> joint global: G = S.P.S.B                      RIG:1136-1151  -> math PaletteToGlobal
** 32. |q.w| > 0.999999 -> no turn                               VAP:1795       -> math ApplyFingerTurns
** 33. D = F.R(q).F^T; after = before.D                          VAP:1799-1819  -> math ApplyFingerTurns
** 34. carry: carry = after.before^-1 onto the finger's later joints
**                                                               VAP:1821-1834  -> math ApplyFingerTurns, onto EVERY
**     joint below (HandRig::Below): VAP's set plus the _UP_TOP tip, which is skinned here and named by no role.
** 35. joint global -> palette: P = S.G.B^-1.S                   RIG:1455       -> math GlobalToPalette
**     [V 31-35: hands_test runs VAP :1785-1836 itself on 200 random hands (mirrored, scaled wrists): identical
**      (difference 0); the tip keeps its rest local to 2.4e-6. fbxload_test F3-F5 on the real FBX, both hands.]
** 36. [FINGERLEN] finger bones lengthened per body               VAP:2336-2400  -> NOT PORTED: one rig, its own size.
** 37. the grasp (vr_grasp.cpp)                                                 -> NOT PORTED (lead: later task).
** 38. HANDPALM_joint: the take turns it 22.4 (right) / 16.1 (left) degrees in the fist (the palm cups, the knuckles
**     move about 2 cm); no source drives it                     --             -> NOT DRIVEN: the palm stays at rest.
**
** Placement (RIG, OXR)
** 39. grip pose located beside the aim pose, HMD-relative, yaw-corrected, raw quaternion
**                                                               OXR:4350-4374  -> dev, after updateHandPose
**     (RigPoseOverride, UZDXREMA's Linux headless rig, not ported: this engine has no such rig).
** 40. OffsetToWorld / GetGripTransform                          OXR:7879-7921  -> dev (GRIPCHECK harness :7848-7874,
**     :7922-7951 not ported: it compared against a construction this engine never had).
** 41. DefaultHandFit (FRIK hand axes from OpenXR grip axes)     RIG:422-460    -> math DefaultHandFit, verbatim
** 42. HandFitOffset (player's fit string vr_fit_hand_<avatar>)  RIG:514-554    -> NOT PORTED: no fit mode here; the
**     default fit is used, as UZDXREMA does for a body never fitted.
** 43. VRRig_CleanFrame (strip the metres and -Z, keep det -1)   RIG:1175-1205  -> NOT PORTED as such: the model is
**     seated in the uncleaned frame in METRES (VRHandPose_HandFrame) and ModelSeat carries the reflection,
**     so the whole chain is unmirrored by construction (tools/hands_test checks det and handedness).
** 44. hand frame from wrist + 4 knuckles, grip socket           RIG:733-750    -> math HandFrameFromJoints, verbatim
** 45. controller path: WRIST at handTarget (grip . fit)         VAP:1966-1969  -> DEPARTS: the grip SOCKET is put
**     at the grip origin, as UZDXREMA's gun-anchor path puts it on a gun's grip (VAP:1960-1965, 1981-1994):
**     OpenXR's grip position is the palm centroid within the handle, which is what the socket is. With
**     the wrist there the palm would sit a hand's width ahead of the real one.
** 46. "no controller, no hand"                                  VAP:1874-1877  -> ObjectToWorldMatrix falls back
**     to the actor's own place (a speck at metre scale) and roth_hands.cpp hides the actor.
**
** The flat test (not in UZDXREMA; test-only)
** 47. roth_handtest: both hands in front of the camera, fixed grip axes, through the placement above
**     (fit, seat) and the fingers (items 7-35) with the typed inputs. Inert until typed.
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
#include "filesystem.h"
#include "sc_man.h"

#include <algorithm>
#include <array>
#include <vector>

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
// THE CURL TABLE. UZDXREMA vr_avatar.cpp LoadCurlTable (:187-281), read the same way from the same file
// (vrealms_iwad/vrhandcurl.txt, the owner's, unchanged): `curl <finger> <joint 1-3> <degrees> <axis x y z>`,
// the axis normalised, a zero axis, an unknown key or a bad finger refusing the whole table, and all 15
// rows or none ("some fingers curling and others not is harder to diagnose than none"). `pose` lines are
// read and checked as VA reads them and not kept: only the weapon and held-object branches look them up
// (not ported). The pack lumps (VRHANDPOSE, LoadPackPoses) are not read, for the same reason.
//
// Here the table is the FALLBACK (checklist 19-20): a joint whose FBX frames exist folds as the FBX does.
//
//===========================================================================

namespace
{
	CurlJoint gTable[FINGER_COUNT][FINGER_JOINTS];
	bool gTableLoaded = false, gTableTried = false;
	const char *const kFingerNames[FINGER_COUNT] = { "thumb", "index", "middle", "ring", "pinky" };

	int FingerByName(const char *s)
	{
		for (int i = 0; i < FINGER_COUNT; ++i)
			if (!stricmp(s, kFingerNames[i])) return i;
		return -1;
	}

	void LoadCurlTable()
	{
		if (gTableTried) return;
		gTableTried = true;

		const int lump = fileSystem.CheckNumForFullName("vrhandcurl.txt");
		if (lump < 0)
		{
			Printf(TEXTCOLOR_RED "VR hands: vrhandcurl.txt is missing from the game data. Fingers with no FBX frames cannot "
				"curl without it.\n");
			return;
		}

		bool seen[FINGER_COUNT][FINGER_JOINTS] = {};
		FScanner sc(lump);
		sc.SetCMode(false);
		while (sc.GetString())
		{
			if (sc.Compare("pose"))
			{
				sc.MustGetString();
				sc.MustGetString();
				if (FingerByName(sc.String) < 0)
				{
					sc.ScriptMessage("vrhandcurl.txt: `%s` is not a finger", sc.String);
					return;
				}
				for (int j = 0; j < FINGER_JOINTS; ++j) sc.MustGetFloat();
				continue;
			}

			if (!sc.Compare("curl"))
			{
				sc.ScriptMessage("vrhandcurl.txt: expected `curl` or `pose`, got `%s`", sc.String);
				return;
			}
			sc.MustGetString();
			const int f = FingerByName(sc.String);
			if (f < 0)
			{
				sc.ScriptMessage("vrhandcurl.txt: `%s` is not a finger", sc.String);
				return;
			}
			sc.MustGetNumber();
			const int j = sc.Number - 1;
			if (j < 0 || j >= FINGER_JOINTS)
			{
				sc.ScriptMessage("vrhandcurl.txt: joint %d is not 1, 2 or 3", sc.Number);
				return;
			}
			sc.MustGetFloat();
			const double deg = sc.Float;
			sc.MustGetFloat(); const double ax = sc.Float;
			sc.MustGetFloat(); const double ay = sc.Float;
			sc.MustGetFloat(); const double az = sc.Float;

			FVector3 axis((float)ax, (float)ay, (float)az);
			const float len = (float)axis.Length();
			if (len < 1e-4f)
			{
				sc.ScriptMessage("vrhandcurl.txt: %s joint %d has a zero curl axis", kFingerNames[f], j + 1);
				return;
			}
			axis /= len;
			gTable[f][j].Axis[0] = axis.X; gTable[f][j].Axis[1] = axis.Y; gTable[f][j].Axis[2] = axis.Z;
			gTable[f][j].Degrees = (float)deg;
			seen[f][j] = true;
		}

		FString missing;
		for (int f = 0; f < FINGER_COUNT; ++f)
			for (int j = 0; j < FINGER_JOINTS; ++j)
				if (!seen[f][j])
					missing.AppendFormat("%s%s joint %d", missing.IsEmpty() ? "" : ", ", kFingerNames[f], j + 1);
		if (!missing.IsEmpty())
		{
			Printf(TEXTCOLOR_RED "VR hands: vrhandcurl.txt is missing %s -- refusing a partial table.\n", missing.GetChars());
			return;
		}
		gTableLoaded = true;
	}
}

//===========================================================================
//
// Per model: the seat for each side, and for the side the model is drawn as, the hand built from its own
// rest pose and its own take (vr_handpose_math.h BuildHand: the canonical frames, the turn's rig, the
// FBX open and fist poses). Built once; a model that lacks any of it is left alone and says why, once.
//
//===========================================================================

namespace
{
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

		// the fingers, per side the model is drawn as
		bool        handTried[SIDE_COUNT] = { false, false };
		bool        handOk[SIDE_COUNT] = { false, false };
		bool        posesOk[SIDE_COUNT] = { false, false };
		HandRig     rig[SIDE_COUNT];
		JointPose   poses[SIDE_COUNT][FINGER_COUNT][FINGER_JOINTS];
		std::vector<float> base, invBase;	// n x 16: the rest (IQM baseframe) and its inverse
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
			if (p >= i) return c;	// parents first, as the IQM loader enforces
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

		// the rest and its inverse, for palette <-> joint global (checklist 31, 35)
		c->base.resize((size_t)n * 16);
		c->invBase.resize((size_t)n * 16);
		for (int i = 0; i < n; i++)
		{
			const FLOATTYPE *m = (*base)[i].get();
			for (int k = 0; k < 16; k++) c->base[(size_t)i * 16 + k] = (float)m[k];
			if (!Detail::Inverse4(&c->base[(size_t)i * 16], &c->invBase[(size_t)i * 16])) return c;
		}

		Printf("VR hands: %s (%d frames), seat %s; grip socket R (%.2f %.2f %.2f) L (%.2f %.2f %.2f) file units\n",
			model->mFileName.GetChars(),
			model->NumFrames(),
			(c->seatOk[0] && c->seatOk[1]) ? "measured" : "NOT measured (wrist/knuckle joints missing)",
			sock[0].x, sock[0].y, sock[0].z, sock[1].x, sock[1].y, sock[1].z);
		return c;
	}

	// The fingers of `model` drawn as `side`: built once (BuildHand), and said once.
	bool HandFor(FHandModelCache *c, int side)
	{
		if (c->handTried[side]) return c->handOk[side];
		c->handTried[side] = true;
		FModel *model = c->model;
		const int n = c->numJoints;
		const char *sideName = side == SIDE_RIGHT ? "right" : "left";
		if (n <= 0 || (int)c->parents.Size() != n || (int)c->base.size() != n * 16)
		{
			Printf(TEXTCOLOR_RED "VR hands: %s -- fingers NOT posed: no usable skeleton\n", model->mFileName.GetChars());
			return false;
		}

		const int hand = model->FindJoint(FName(kHandJoint));
		int joints[FINGER_COUNT][FINGER_JOINTS];
		for (int f = 0; f < FINGER_COUNT; f++)
			for (int j = 0; j < FINGER_JOINTS; j++)
				joints[f][j] = model->FindJoint(FName(kFingerJointNames[f][j]));

		// the rest pose as MA reads a model: each joint's rest global (rotation * scale) and its local translation
		const TArray<VSMatrix> *base = model->GetBasePose();
		std::vector<RestJoint> J(n);
		for (int i = 0; i < n; i++)
		{
			J[i].parent = c->parents[i];
			const FLOATTYPE *m = (*base)[i].get();
			for (int k = 0; k < 16; k++) J[i].G[k] = m[k];
			const TRS t = model->GetJointBaseTRS(i);
			J[i].t[0] = t.translation.X; J[i].t[1] = t.translation.Y; J[i].t[2] = t.translation.Z;
		}

		// the FBX's own frames (checklist 29-30), when the take has them
		const bool haveFrames = model->NumFrames() > kFistFrame && model->NumFrames() > kOpenFrame && model->NumFrames() > kDrawnFrame;
		std::vector<std::array<double, 4>> qD, qO, qF;
		if (haveFrames)
		{
			qD.resize(n); qO.resize(n); qF.resize(n);
			for (int i = 0; i < n; i++)
			{
				const TRS d = model->GetJointPose(i, kDrawnFrame), o = model->GetJointPose(i, kOpenFrame), f = model->GetJointPose(i, kFistFrame);
				qD[i] = { d.rotation.X, d.rotation.Y, d.rotation.Z, d.rotation.W };
				qO[i] = { o.rotation.X, o.rotation.Y, o.rotation.Z, o.rotation.W };
				qF[i] = { f.rotation.X, f.rotation.Y, f.rotation.Z, f.rotation.W };
			}
		}

		FingerFrame frames[FINGER_COUNT][FINGER_JOINTS];
		double palm[3];
		char why[256] = "";
		const bool ok = BuildHand(J, hand, joints, side == SIDE_RIGHT,
			haveFrames ? reinterpret_cast<const double (*)[4]>(qD.data()) : nullptr,
			haveFrames ? reinterpret_cast<const double (*)[4]>(qO.data()) : nullptr,
			haveFrames ? reinterpret_cast<const double (*)[4]>(qF.data()) : nullptr,
			c->rig[side], c->poses[side], frames, palm, why, sizeof(why));
		if (!ok)
		{
			Printf(TEXTCOLOR_RED "VR hands: %s as the %s hand -- fingers NOT posed: %s\n", model->mFileName.GetChars(), sideName, why);
			return false;
		}
		c->handOk[side] = true;
		c->posesOk[side] = haveFrames;
		LoadCurlTable();
		double minFold = 9;
		for (int f = 0; f < FINGER_COUNT; f++) for (int j = 0; j < FINGER_JOINTS; j++) if (frames[f][j].fold < minFold) minFold = frames[f][j].fold;
		if (haveFrames)
			Printf("VR hands: %s as the %s hand -- fingers posed from frames %d (open) and %d (fist) of its own take, 15 joints; "
				"canonical frames measured (palm %.3f %.3f %.3f, smallest fold %.3f)\n",
				model->mFileName.GetChars(), sideName, kOpenFrame, kFistFrame, palm[0], palm[1], palm[2], minFold);
		else
			Printf(TEXTCOLOR_ORANGE "VR hands: %s as the %s hand -- the take has %d frames, no fist frame %d: fingers posed from %s\n",
				model->mFileName.GetChars(), sideName, model->NumFrames(), kFistFrame,
				gTableLoaded ? "the FRIK curl table (vrhandcurl.txt)" : "nothing (no curl table either): they stay at rest");
		return true;
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
// THE CURLS, once per displayed frame, both sides (VAP UpdateCurls :419-698; checklist 7-22). The
// inputs: the controller this side holds (by handedness), or the flat test's. "Valid" is the source's
// rig.Valid(hidx): a pose for that controller this frame -- here, its grip transform.
//
//===========================================================================

namespace
{
	CurlState gCurl[SIDE_COUNT];
	SideState gRot[SIDE_COUNT];
	uint64_t gCurlFrame = ~0ull;
	double gCurlLastTime = 0.0;
	// the poses of the model last drawn as each side (null: the table's fold, or none)
	const JointPose (*gSidePoses[SIDE_COUNT])[FINGER_JOINTS] = { nullptr, nullptr };

	void ReadInputs(int side, Inputs &in)
	{
		in = Inputs();
		if (gTest.on)
		{
			in.trigger = gTest.in.trigger;
			in.grip = gTest.in.grip;
			in.triggerTouch = gTest.in.triggerTouch;
			in.thumbTouch = gTest.in.thumbTouch;
			in.valid = true;
			return;
		}
		const VRMode *vm = VRMode::GetVRModeCached(true);
		if (vm == nullptr || !vm->IsVR()) return;
		const int role = VRHandPose_RoleForSide(side);
		VSMatrix g;
		if (!vm->GetGripTransform(role, &g)) return;
		in.valid = true;
		VRFingerInput fi;
		if (vm->GetFingerInput(role, fi))
		{
			in.trigger = fi.trigger;
			in.grip = fi.grip;
			in.triggerTouch = fi.triggerTouch;
			in.thumbTouch = fi.thumbTouch;
		}
	}

	void UpdateCurls()
	{
		const uint64_t fr = screen != nullptr ? screen->FrameCount : 0;
		if (fr == gCurlFrame) return;
		gCurlFrame = fr;

		const double now = screen != nullptr ? (double)screen->FrameTime * 0.001 : 0.0;
		const double dt = now - gCurlLastTime;
		gCurlLastTime = now;
		const float retain = Retain(dt);	// SanitiseDt inside: first frame or a hitch -> 1/90

		LoadCurlTable();
		for (int side = 0; side < SIDE_COUNT; ++side)
		{
			Inputs in;
			ReadInputs(side, in);
			StepSide(gCurl[side], gRot[side], in, retain, gSidePoses[side], gTableLoaded ? gTable : nullptr);
		}
	}
}

//===========================================================================
//
// THE FINGERS ON THE PALETTE (checklist 31-35): the palette to joint globals, each finger joint turned
// by its smoothed rotation in its canonical frame with everything below it carried, and back.
//
//===========================================================================

const TArray<VSMatrix> *VRHandPose_Apply(const AActor *actor, FModel *model, int modelIndex, const TArray<VSMatrix> &bones)
{
	(void)modelIndex;
	if (actor == nullptr || model == nullptr) return &bones;
	const int side = SideOfActor(actor);
	if (side < 0) return &bones;
	FHandModelCache *c = CacheFor(model);
	if (c == nullptr || !HandFor(c, side)) return &bones;
	const int n = c->numJoints;
	if ((int)bones.Size() != n) return &bones;

	gSidePoses[side] = c->posesOk[side] ? c->poses[side] : nullptr;
	UpdateCurls();

	static thread_local TArray<VSMatrix> posed;
	static thread_local std::vector<float> pal, out, scratch;
	static thread_local std::vector<char> moved;
	pal.resize((size_t)n * 16);
	out.resize((size_t)n * 16);
	for (int i = 0; i < n; i++)
	{
		const FLOATTYPE *m = bones[i].get();
		for (int k = 0; k < 16; k++) pal[(size_t)i * 16 + k] = (float)m[k];
	}
	PosePalette(reinterpret_cast<const float (*)[16]>(pal.data()), reinterpret_cast<const float (*)[16]>(c->base.data()),
		reinterpret_cast<const float (*)[16]>(c->invBase.data()), n, c->rig[side], gRot[side],
		reinterpret_cast<float (*)[16]>(out.data()), scratch, moved);
	posed.Resize(n);
	for (int i = 0; i < n; i++) posed[i].loadMatrix(&out[(size_t)i * 16]);
	return &posed;
}
