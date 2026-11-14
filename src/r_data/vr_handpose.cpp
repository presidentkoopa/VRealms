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
** The rule and its smoothing (VAP UpdateCurls :419-698)
**  7. once per displayed frame (rig.FrameIndex)                 VAP:421-423    -> UpdateCurls, screen->FrameCount
**  8. dt from screen->FrameTime ms; first frame/hitch -> 1/90   VAP:425-429    -> UpdateCurls, math SanitiseDt
**  9. retain = 0.75^(dt*90)                                     VAP:431        -> math Retain
** 10. which controller each side holds (handedness)             VAP:433-438    -> VRHandPose_RoleForSide
** 11. index on the trigger: trigger > 0 ? trigger : (triggerTouch ? 0.3 : 0)
**                                                               VAP:444        -> math PoseRule (amount of the index
**     while it is on the trigger), kIndexTouchRest
** 12. middle = ring = pinky = grip                              VAP:445-447    -> math PoseRule (their amounts)
** 13. thumb = thumbTouch ? 0.6 : 0                              VAP:448        -> NOT PORTED (owner, 2026-11-13: "the
**     hands have poses, why are we bending fingers"): the thumb touch PICKS the pose (thumb closed in fist/point,
**     raised in thumbup/fingergun) and the grip says how far, as for the other fingers
** 14. no controller -> every finger 0                           VAP:450-454    -> math PoseRule (!valid): every amount 0
** 15. one value per finger copied to its 3 joints               VAP:456-458    -> math PoseTargets (one amount per finger)
** 16. weapon in the hand (gun_grip / grip_<class> ...)          VAP:466-557    -> PORTED, items 48-60
** 17. an object in the hand: hold_<subject>...                  VAP:558-656    -> PORTED, items 61-68
** 18. scalar curl smoothed (FCurlState)                         VAP:658-665    -> NOT PORTED: no bone reads it in the
**     source; there is no scalar curl with poses
** 19. rotation target from the FRIK table, QAxisAngle           VAP:685-690    -> NOT PORTED (owner: no per-finger
**     bending). vrhandcurl.txt and its reader are gone; a model without the poses' frames is not posed and says so.
** 20. [POSEROT] the target from two poses, QSlerp(a, b, t), and a named pose as the target
**                                                               VAP:674-683    -> math PoseTargets: QSlerp(open, pose,
**     amount) on rotations made from two AUTHORED take frames (BuildPoses, PoseRotation); the poses and which touch
**     picks which: math PoseRule, kPoseSources (NOT FROM A SOURCE: chosen by walking the takes, tools/fbxload_test/
**     walk_poses.py; the owner's fbx_poses.json names were the starting point)
** 21. gRot: first frame takes the target, then slerp(cur, target, 1 - retain)
**                                                               VAP:691-696, 385-390 -> math Smooth, StepPose
** 22. QSlerp: shortest arc, lerp past 0.9995, renormalised      VAP:392-408    -> math QSlerp
**     [V items 7-9, 21-22: tools/hands_test test_curls -- VAP's own lines (the rate :429-431, QSlerp :392-414, gRot's
**      smoothing :691-696) cut out and compiled beside the port, fed the port's targets, 72/80/90/120/144 Hz with
**      jitter, hitches and dropouts: every quaternion identical (difference 0). Items 11, 12, 14 and the poses:
**      test_curls (the rule's cases) and tools/fbxload_test (every state at full input = its authored frames).]
**
** The curl table (VA :187-281)                                                 -> NOT PORTED any more (item 19)
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
** 30. take frame = Blender frame - 1                             fbx_poses.json -> math kPoseSources (frame numbers)
**     [V: fbxload_test F2 -- from frames 0 -> 318 the right hand's 15 rotations equal vrhandpose.txt's fbx_fist R to
**      0.131 deg, the left's fbx_fist L to 0.113 deg once read in MKP's mirrored frame; frames 317 and 319 are off by
**      1.3-3.6 deg. W -- the whole take walked, both hands: walk_poses.py.]
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
** 38. HANDPALM_joint (parent of every finger): no source drives it -> DRIVEN PER POSE, NOT A PORT (lead, on the
**     owner's word, 2026-11-13): it follows its pose's authored frame as the fingers do (QSlerp(open, frame, grip),
**     the same smoothing, turned before the fingers so they ride it) ONLY in a pose where it carries no straight
**     finger -- the fist (frame 318: 22.4 deg right / 16.1 left). In point, thumbup and fingergun it stays in the
**     drawn frame: measured in the take, the fist's palm would swing the pointing index / raised thumb by tens of
**     mm (the ~4 cm the owner saw). -> math kPoseSources (Palm), BuildPoses, StepPose, ApplyPalmTurn.
**     [V: tools/fbxload_test S1-S4 on the real FBX, both hands -- reasoning only in the engine, not seen running.]
**     (The held poses apply the same rule by its reason: item 72.)
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
** A HAND HOLDING SOMETHING (VAP :466-656, :667-684; VA :60-176, 319-347; RIG :1802-1853; OXR-h = UZDXREMA
** vk_openxrdevice.h; MKP and the owner's fbx_poses.json "FJ"). "math:N" = vr_handpose_math.h line N. Verified items
** say how; the rest [R].
**  [V 50-68, 70: tools/hands_test test_holds -- VAP :476-656 and :672-684 and OXR-h:105-131 cut out at build time and
**   compiled beside the port; 48,000 hands (weapon / object / nothing, every grip class and subject, prop classes,
**   the other hand's gun, both handednesses, tracked or not, the real one-sided table plus random holes): every
**   choice and every joint target identical (difference 0). The VA probes are answered per side by the stub: item 49.]
** 48. EGripSubject: None, Round, Shell, Inserting, Magazine, Grip, Forend, Foregrip, Slide, Support, Holster, Pouch, MAX
**                                                               OXR-h:105-131  -> math:1015 (same values: test_holds)
** 49. VRAvatar_NamedPose(name, finger) / VRAvatar_NamedPoseRot(name, side, finger, joint), names by stricmp
**                                                               VA:319-347     -> math:1224 HoldHas, :1234 HoldRot, :1054
**     DEPARTS: VA's NamedPose has no side, because UZDXREMA's table always carries both sides (MKP writes a pose one hand
**     authored for the other side MIRRORED). Here a side's pose comes from its own take or a copied frame (item 76), so the
**     probe is per side; a pose a side lacks is absent there, as VAP treats any missing pose (:577-579).
** 50. weapon branch taken when a weapon is drawn in this hand    VAP:476        -> math:1336 SelectHold (HeldState.weapon)
** 51. base = gun_grip; with a grip class: grip_<cls>, fire grip_<cls>_fire   VAP:484-491 -> math:1338-1345
** 52. [GUNPOSE] grip_<prop class> wins when it has a middle finger, fire = grip_<prop>_fire
**                                                               VAP:494-503    -> math:1346-1354
** 53. middle, ring, pinky: per joint from base, else gun_grip  VAP:505-514    -> math:1355-1364
** 54. [FBXGRIP] the thumb from base (else gun_grip) when that pose has a thumb rotation
**                                                               VAP:522-535    -> math:1365-1374
** 55. the index ready->fire only when a fire name exists and both have the index; t = trigger if tracked, else 0
**                                                               VAP:543-556    -> math:1375-1384
** 56. otherwise the index (and any finger no pose set) stays the controller's
**                                                               VAP:470-472, 685-690 -> HoldTargets keeps the controller's
**     target (PoseRule / PoseTargets, items 11-20): VAP's fallback there is its FRIK curl table, which is not ported (19).
** 57. VRRig_HandHoldsWeapon / HandGripPose / HandPropClass: recorded as the weapon is DRAWN, stale -> none
**                                                               RIG:1802-1826  -> HeldState, published every tic by the game
**     layer (VRHandPose_SetHeld; src/roth/roth_hands.cpp PublishHeld: RealmsHeld drawn at the main hand,
**     roth_actors_engine.cpp ActorsHeldWeaponDrawn). [R: not seen running]
** 58. AActor::VRGripPose (the prop names its grip class)       actor.h:3401   -> NOT PORTED as an actor field: Realms
**     weapons are not mod props; their class comes from src/roth/roth_handhold.h (the item dossiers).
** 59. VRRig_HandGripSubject reads the console pawn's GripSubjectMain/Off, written by the OpenXR device's arbitration
**     and script claims                                         RIG:1847-1853, OXR:4560-4820 -> NOT PORTED: Realms has no
**     magazines, rounds or pumps to claim; the subject is the item in the hand (roth_handhold.h), published per tic.
** 60. netplay: the held state through usercmd_t / the pawn       (UZDXREMA's rules) -> NOT PORTED: single player; the
**     hands are presentation only (vr_handpose.h).
** 61. object branch otherwise                                   VAP:558        -> math:1385
** 62. subject -> name: Magazine hold_magazine, Round hold_round, Shell hold_shell, Inserting hold_inserting (fallback
**     hold_round), Forend hold_forend, Foregrip hold_foregrip, Slide hold_slide, Support hold_support (fallback
**     offhand_weapon_grip); None, Grip, Holster, Pouch: nothing  VAP:587-609    -> math:1387-1401
** 63. per gun type: tries <base>_<other hand's prop>, <base>_<other hand's class>, <base>, for the name then the
**     fallback (other hand's only while it holds a weapon)      VAP:622-636    -> math:1406-1417
** 64. ONE POSE WINS WHOLE: the first try with any finger; every finger it has, the index included
**                                                               VAP:637-654    -> math:1418-1426
** 65. NOT IN THE SOURCE: HeldState.pose -- a held thing may name a pose of the table itself (the Lamp's fbx_curl, the
**     frame its dossier names and no subject's pose uses); it takes the subject's name's place in 62-64. Checked by
**     value (test_holds).                                                      -> math:1403
** 66. the target: the index QSlerp(ready, fire, t) when both have it; else the named pose's rotation
**                                                               VAP:672-684    -> math:1437 HoldTargets
** 67. the targets then smoothed exactly as every other target (items 21-22)
**                                                               VAP:691-696    -> math:1468 StepPoseHeld (Smooth)
** 68. once per displayed frame, both sides, held states per ROLE (hidx), the other hand by role
**                                                               VAP:433-438, 622 -> UpdateCurls (here)
** 69. the poses: the owner's fbx_poses.json, every entry in his order (16 raw frames, 34 composed), Blender - 1,
**     then 2 MEASURED ADDITIONS (item 77)                       FJ, MKP main() -> math:1098 kOwnerPoses
**     [V: walk_poses.py check_hold_table: his 50 first and equal, then exactly the 2 additions. fbxload_test H0: every
**      raw frame of his, from take frame Blender - 1, equals mkfbxposes.py's own rotations for the hand that authored
**      it, within 0.131 deg (right, 11 frames) / 0.129 deg (left, 13 frames).]
** 70. each hand's OWN take wherever it authored the frame       MKP "own"      -> math:1245 BuildHoldPoses
**     The right take lacks wrap_half (820), wrap_tight (837), cup and twohand_support (985), thumb_out (944); the left
**     lacks twofinger_point (762), fingergun (803), twohand_gun (985). [V: walk_poses.py: nearest frame in the other
**     take 0.352-0.578 hand lengths away, beyond the 0.319 the hands' shared frames differ by.] Those are copied: 76.
** 71. a pose = PoseRotation (item 29) from the drawn frame to each finger's take frame, built once per model and
**     side                                                     MKP main()     -> math:1245 BuildHoldPoses; HandFor (here)
**     [V: fbxload_test H2, both hands, every pose by name, every subject, every grip class at trigger 0 and 1, through
**      SelectHold + StepPoseHeld converged at 90 Hz: every drawn joint rotation (and the palm's) within 8.3e-6 deg of
**      the rotation made in double from its authored frame; through the float palette, every finger joint, tip and
**      the palm within 1.6e-5 deg of the composition; each pose the hand lacks leaves exactly the controller's pose.]
** 72. HANDPALM_joint in a held pose: NOT FROM THE SOURCE (UZDXREMA's avatars have no palm joint) -- item 38's rule by
**     its reason: the palm takes the middle finger's frame whenever every straight finger of the pose comes from that
**     frame (no straight finger rides a palm from another frame). It gives item 38's table for kPoseSources exactly
**     (walk_poses.py checks both), and drives the palm in every whole frame and every composed pose but `pointing`.
**     Item 38's wording ("no straight finger") fails the owner's frames: in curl (206) / halfcurl (193) the bend is the
**     palm joint's own (59-64 deg) and the fingers on it are straight; without their palm they turn back from the hand
**     (seen: fbxload_test software renders, fingers/holds_<hand>.png). While a finger is the controller's (a weapon with
**     no fire pose), the palm stays in the drawn frame.                       -> math:1074 (Palm), :1437 HoldTargets
** 73. which Realms thing is which subject / grip class: src/roth/roth_hands.cpp + roth_handhold.h (the item dossiers,
**     docs/ITEM_DESIGN.md). Not a port: a design table, every row with the dossier's words.
** 74. the log: one line per hand whenever what shapes it changes ("VR hands: the left hand (off hand) holds ...") --
**     evidence of the choice in a run, not of the drawing.                     -> UpdateCurls, math:1483 DescribeHold
** 75. roth_handhold (flat test): a physical hand's held thing in place of the game's while roth_handtest is on
**                                                                              -> VRHandTest_SetHeld, roth_hands.cpp
** 76. A FRAME A HAND'S TAKE LACKS IS COPIED FROM THE OTHER HAND'S TAKE (owner, 2026-11-13: "just copy the poses"):
**     that frame's joint LOCAL rotations, by joint name, onto this hand's joints; only the missing frames.
**     MKP instead MIRRORS the other hand's whole pose.          -> math:1245 BuildHoldPoses (getQOther), HandFor
**     [V: fbxload_test H1: the two skeletons' rest locals equal (0 deg); right hand 33 poses use copied frames,
**      left 4; H3: every copied joint has the other hand's local rotation at that frame within 1.1e-5 deg through
**      the engine path. Software renders: the copied right-hand wraps look like the left's, mirror-image.]
** 77. MEASURED ADDITIONS (not the owner's json, which says there is no pinch): fbx_pinch = Blender 694 (take 693, tips
**     0.045 R / 0.044 L hand lengths), fbx_pinch_tight = Blender 312 (0.028 R / 0.051 L), both hands' own. The
**     pinch items use fbx_pinch (roth_handhold.h).              -> math:1152 [V: walk_poses.py prints and checks them]
** 78. the original's right-hand item is held by the hand on the RIGHT side (owner); the weapon stays at the main hand,
**     where RealmsHeld is drawn; where both are one hand the weapon's grip wins (VAP :476)  -> roth_hands.cpp PublishHeld
**
** The flat test (not in UZDXREMA; test-only)
** 47. roth_handtest: both hands in front of the camera, fixed grip axes, through the placement above
**     (fit, seat) and the fingers (items 7-38) with the typed inputs. Inert until typed.
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
#include "i_time.h"
#include "hw_perflog.h"

#include <algorithm>
#include <atomic>
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
		float tilt = 0.f;
		double lift = 0.0;		// metres, VRHandTest_SetLift
		double off[SIDE_COUNT][3] = { { 0, 0, 0 }, { 0, 0, 0 } };	// metres fwd / right / up, VRHandTest_SetOffset
		bool heldOn[SIDE_COUNT] = { false, false };	// roth_handhold: this side's held thing replaces the game's
		HeldState held[SIDE_COUNT];
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
		const double *o = gTest.off[side];
		out.translate((float)((side == SIDE_RIGHT ? TEST_SIDE : -TEST_SIDE) + o[1]), (float)(TEST_UP + gTest.lift + o[2]), (float)-(TEST_FWD + o[0]));
		out.rotate(side == SIDE_RIGHT ? -gTest.turn : gTest.turn, 0.f, 1.f, 0.f);
		// `tilt` tips the fingers from straight up toward straight ahead about the camera's right
		// axis (90 = the fingers point away from the camera, palm down): the VR picker's flat test
		// (roth_vrpicktest) points the hand at what it aims at.
		if (gTest.tilt != 0.f) out.rotate(-gTest.tilt, 1.f, 0.f, 0.f);
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

void VRHandTest_SetHeld(int side, bool on, const HeldState &held)
{
	if (side < 0 || side >= SIDE_COUNT) return;
	gTest.heldOn[side] = on;
	gTest.held[side] = on ? held : HeldState();
}

void VRHandTest_SetTilt(float tiltDeg) { gTest.tilt = tiltDeg; }
void VRHandTest_SetOffset(int side, double fwd, double right, double up)
{
	if (side < 0 || side >= SIDE_COUNT) return;
	gTest.off[side][0] = fwd; gTest.off[side][1] = right; gTest.off[side][2] = up;
}
void VRHandTest_Offset(int side, double &fwd, double &right, double &up)
{
	fwd = right = up = 0.0;
	if (side < 0 || side >= SIDE_COUNT) return;
	fwd = gTest.off[side][0]; right = gTest.off[side][1]; up = gTest.off[side][2];
}
void VRHandTest_SetLift(double metres) { gTest.lift = metres; }
double VRHandTest_Lift() { return gTest.lift; }

bool VRHandTest_Inputs(FVRHandTestInputs &out)
{
	if (!gTest.on) return false;
	out = gTest.in;
	return true;
}

void VRHandTest_ActorSpot(int side, const DVector3 &eye, double yawDeg, DVector3 &out)
{
	const double upm = VR_UnitsPerMeter();
	const DAngle yaw = DAngle::fromDeg(yawDeg);
	const DVector3 fwd(yaw.Cos(), yaw.Sin(), 0.0);
	const DVector3 right(yaw.Sin(), -yaw.Cos(), 0.0);
	const double s = (side == SIDE_RIGHT) ? TEST_SIDE : -TEST_SIDE;
	const double *o = (side >= 0 && side < SIDE_COUNT) ? gTest.off[side] : gTest.off[0];
	out = eye + fwd * ((TEST_FWD + o[0]) * upm) + right * ((s + o[1]) * upm) + DVector3(0, 0, (TEST_UP + gTest.lift + o[2]) * upm);
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
// Per model: the seat for each side, and for the side the model is drawn as, the hand built from its own
// rest pose and its own take (vr_handpose_math.h BuildHand: the canonical frames and the turn's rig;
// BuildPoses: the authored poses of kPoseSources). Built once; a model that lacks any of it is left alone
// and says why, once.
//
//===========================================================================

namespace
{
	// Each hand's FBX mesh selector, as vrealms_iwad/MODELDEF.txt names it (after "hand_final.fbx:"): the other hand's
	// model is found by it when a frame is copied from its take (checklist 76).
	const char *const kHandSelector[SIDE_COUNT] = { "Hand_Low", "Hand_Low1?mirrorskin=Hand_Low" };

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
		HandRig     rig[SIDE_COUNT];
		HandPoses   poses[SIDE_COUNT];	// the authored poses (kPoseSources), checklist 11-20, 38
		HoldPoses   hold[SIDE_COUNT];	// the owner's held-object and weapon-grip poses (kOwnerPoses), checklist 48-75
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
		const int palmJ = model->FindJoint(FName(kPalmJoint));
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

		FingerFrame frames[FINGER_COUNT][FINGER_JOINTS];
		double palm[3];
		char why[256] = "";
		if (!BuildHand(J, hand, palmJ, joints, side == SIDE_RIGHT, c->rig[side], frames, palm, why, sizeof(why)))
		{
			Printf(TEXTCOLOR_RED "VR hands: %s as the %s hand -- fingers NOT posed: %s\n", model->mFileName.GetChars(), sideName, why);
			return false;
		}
		// the authored poses (checklist 11-20, 29-30, 38): each joint's local rotation in a take frame, from the model
		auto getQ = [model](int frame, int joint, double q[4]) {
			const TRS t = model->GetJointPose(joint, frame);
			q[0] = t.rotation.X; q[1] = t.rotation.Y; q[2] = t.rotation.Z; q[3] = t.rotation.W;
		};
		if (!BuildPoses(c->rig[side], frames, side, model->NumFrames(), getQ, c->poses[side], why, sizeof(why)))
		{
			Printf(TEXTCOLOR_RED "VR hands: %s as the %s hand -- fingers NOT posed: %s\n", model->mFileName.GetChars(), sideName, why);
			return false;
		}
		// the held-object and weapon-grip poses (checklist 48-75, 76-78): the owner's table, from this hand's own take,
		// and a frame only the OTHER hand authored copied from the other hand's take, joint by joint by name (item 76).
		// The other hand is the other FBX mesh of the same file, by its MODELDEF selector (kHandSelector).
		FModel *otherModel = nullptr;
		{
			const FString &mine = model->mFileName;
			const ptrdiff_t colon = mine.LastIndexOf(':');
			if (colon > 0)
			{
				const FString otherName = mine.Left(colon + 1) + kHandSelector[1 - side];
				const unsigned idx = FindModel(nullptr, otherName.GetChars(), true);
				if (idx < Models.Size() && Models[idx] != model) otherModel = Models[idx];
			}
			if (otherModel == nullptr)
				Printf(TEXTCOLOR_RED "VR hands: %s as the %s hand -- the other hand's model (%s) is not loaded: no frame can be copied from its take\n",
					model->mFileName.GetChars(), sideName, kHandSelector[1 - side]);
		}
		auto getQOther = [otherModel](int frame, const char *name, double q[4]) {
			if (otherModel == nullptr) return false;
			const int j = otherModel->FindJoint(FName(name));
			if (j < 0 || frame < 0 || frame >= otherModel->NumFrames()) return false;
			const TRS t = otherModel->GetJointPose(j, frame);
			q[0] = t.rotation.X; q[1] = t.rotation.Y; q[2] = t.rotation.Z; q[3] = t.rotation.W;
			return true;
		};
		if (!BuildHoldPoses(c->rig[side], frames, side, model->NumFrames(), getQ, getQOther, otherModel != nullptr ? otherModel->NumFrames() : 0,
			c->hold[side], why, sizeof(why)))
		{
			Printf(TEXTCOLOR_RED "VR hands: %s as the %s hand -- fingers NOT posed: %s\n", model->mFileName.GetChars(), sideName, why);
			return false;
		}
		{
			int have = 0, copied = 0;
			FString lacks, copies, palms;
			for (int p = 0; p < kOwnerPoseCount; p++)
			{
				const OwnerPose &op = kOwnerPoses[p];
				const HoldPoses &hp = c->hold[side];
				if (!hp.Have[p]) { lacks.AppendFormat("%s%s", lacks.IsEmpty() ? "" : ", ", op.name); continue; }
				have++;
				if (hp.PalmDriven[p]) palms.AppendFormat("%s%s", palms.IsEmpty() ? "" : " ", op.name);
				if (!hp.Copied[p]) continue;
				copied++;
				FString from;
				for (int f = 0; f < FINGER_COUNT; f++)
					if (hp.FromOther[p][f] && from.IndexOf(FStringf("%d", hp.Frame[p][f]).GetChars()) < 0)
						from.AppendFormat("%s%d", from.IsEmpty() ? "" : "/", hp.Frame[p][f]);
				copies.AppendFormat("%s%s (frame %s)", copies.IsEmpty() ? "" : ", ", op.name, from.GetChars());
			}
			Printf("VR hands: %s as the %s hand -- held-object and weapon-grip poses (UZDXREMA fbx_poses.json + the measured pinches): %d of %d; "
				"%d use frames COPIED from the %s hand's take (this take lacks them): %s; absent: %s; the palm follows the pose in: %s\n",
				model->mFileName.GetChars(), sideName, have, kOwnerPoseCount, copied, side == SIDE_RIGHT ? "left" : "right",
				copies.IsEmpty() ? "none" : copies.GetChars(), lacks.IsEmpty() ? "none" : lacks.GetChars(), palms.IsEmpty() ? "none" : palms.GetChars());
		}
		c->handOk[side] = true;
		double minFold = 9;
		for (int f = 0; f < FINGER_COUNT; f++) for (int j = 0; j < FINGER_JOINTS; j++) if (frames[f][j].fold < minFold) minFold = frames[f][j].fold;
		static const char *const kPoseNames[POSE_COUNT] = { "fist", "point", "thumbup", "fingergun" };
		FString list;
		for (int p = 0; p < POSE_COUNT; p++)
		{
			const PoseSource &ps = kPoseSources[side][p];
			list.AppendFormat("%s%s %d/%d/%d/%d/%d palm %s", p ? "; " : "", kPoseNames[p], ps.Finger[0], ps.Finger[1], ps.Finger[2], ps.Finger[3], ps.Finger[4],
				ps.Palm >= 0 ? FStringf("%d", ps.Palm).GetChars() : "not driven");
		}
		Printf("VR hands: %s as the %s hand -- 15 finger joints posed by the authored poses of its own take (open = frame %d; "
			"thumb/index/middle/ring/pinky frames: %s); canonical frames measured (palm %.3f %.3f %.3f, smallest fold %.3f)\n",
			model->mFileName.GetChars(), sideName, kOpenFrame, list.GetChars(), palm[0], palm[1], palm[2], minFold);
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
// THE POSES, once per displayed frame, both sides (VAP UpdateCurls :419-698; checklist 7-22, 38). The
// inputs: the controller this side holds (by handedness), or the flat test's. "Valid" is the source's
// rig.Valid(hidx): a pose for that controller this frame -- here, its grip transform.
//
//===========================================================================

namespace
{
	SideState gRot[SIDE_COUNT];
	PalmState gPalm[SIDE_COUNT];
	uint64_t gCurlFrame = ~0ull;
	double gCurlLastTime = 0.0;
	// the poses of the model last drawn as each side (null: none built -- the fingers stay in the drawn frame)
	const HandPoses *gSidePoses[SIDE_COUNT] = { nullptr, nullptr };
	const HoldPoses *gSideHold[SIDE_COUNT] = { nullptr, nullptr };
	// what each ROLE holds (VRHandPose_SetHeld), and what was last said about each side
	HeldState gHeld[2];
	std::string gHoldSaid[SIDE_COUNT];

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

		// VAP :433-438: the role each side holds, by handedness; the held states are per role (VAP's hidx), and the
		// flat test's (roth_handhold) replace a side's role's
		HeldState held[2] = { gHeld[0], gHeld[1] };
		for (int side = 0; side < SIDE_COUNT; ++side)
			if (gTest.on && gTest.heldOn[side]) held[VRHandPose_RoleForSide(side)] = gTest.held[side];

		for (int side = 0; side < SIDE_COUNT; ++side)
		{
			Inputs in;
			ReadInputs(side, in);
			const int role = VRHandPose_RoleForSide(side);
			HoldChoice choice;
			SelectHold(gSideHold[side], held[role], held[1 - role], in, choice);	// VAP :466-656
			StepPoseHeld(gRot[side], gPalm[side], in, retain, gSidePoses[side], gSideHold[side], &choice);

			// said once per change: what shapes this hand (a log line from a run is evidence of the choice, not of the drawing)
			std::string said = DescribeHold(gSideHold[side], held[role], choice);
			if (said != gHoldSaid[side])
			{
				gHoldSaid[side] = said;
				Printf("VR hands: the %s hand (%s hand) holds %s\n", side == SIDE_RIGHT ? "right" : "left", role == VR_MAINHAND ? "main" : "off", said.c_str());
			}
		}
	}
}

void VRHandPose_SetHeld(int role, const HeldState &held)
{
	if (role < 0 || role > 1) return;
	gHeld[role] = held;
}

//===========================================================================
//
// THE INDEX FINGER AND THE PALM (vr_handpose.h FVRHandPoints). Not a port: the points the VR picker
// needs, read off the model's own joints. Rest: the base pose's joint origin, file space -> render
// space by the loader's swap (x, z, y) (models_fbx.cpp:316, as ModelSeat). Posed: that rest point
// through the palette VRHandPose_Apply produced (the palette maps rest render-space points to posed
// ones, checklist 35), kept per side with the frame it was drawn in. Written on the render thread
// into the slot the reader is not using, then published by one atomic index.
//
//===========================================================================

namespace
{
	enum { PT_TIP, PT_BASE, PT_PALM, PT_WRIST, PT_MIDDLE, PT_COUNT };
	const char *const kPointJoints[PT_COUNT] = { "INDEX_UP_TOP_joint", "INDEX_BASE_joint", "HANDPALM_joint", "Root_joint", "MIDDLE_F_BASE_joint" };

	struct FPosedPoints
	{
		const FModel *model = nullptr;
		uint64_t frame = ~0ull;
		DVector3 p[PT_COUNT];
	};
	FPosedPoints gPosedPts[SIDE_COUNT][2];
	std::atomic<int> gPosedSlot[SIDE_COUNT] = { {0}, {0} };

	bool RestPoint(FModel *model, int joint, DVector3 &out)
	{
		const TArray<VSMatrix> *base = model->GetBasePose();
		if (base == nullptr || joint < 0 || joint >= (int)base->Size()) return false;
		const FLOATTYPE *m = (*base)[joint].get();
		out = DVector3(m[12], m[14], m[13]);	// file (x, y, z) -> render (x, z, y)
		return true;
	}

	void StorePosedPoints(FModel *model, int side, const float *pal, int n)
	{
		int joint[PT_COUNT];
		DVector3 rest[PT_COUNT];
		for (int k = 0; k < PT_COUNT; k++)
		{
			joint[k] = model->FindJoint(FName(kPointJoints[k]));
			if (joint[k] < 0 || joint[k] >= n || !RestPoint(model, joint[k], rest[k])) return;
		}
		const int slot = 1 - gPosedSlot[side].load(std::memory_order_acquire);
		FPosedPoints &w = gPosedPts[side][slot];
		for (int k = 0; k < PT_COUNT; k++)
		{
			const float *P = pal + (size_t)joint[k] * 16;	// column-major
			const DVector3 &r = rest[k];
			w.p[k] = DVector3(P[0] * r.X + P[4] * r.Y + P[8] * r.Z + P[12],
				P[1] * r.X + P[5] * r.Y + P[9] * r.Z + P[13],
				P[2] * r.X + P[6] * r.Y + P[10] * r.Z + P[14]);
		}
		w.model = model;
		w.frame = screen != nullptr ? screen->FrameCount : 0;
		gPosedSlot[side].store(slot, std::memory_order_release);
	}
}

bool VRHandPose_FingerPoints(FModel *model, int side, FVRHandPoints &out)
{
	out = FVRHandPoints();
	if (model == nullptr || side < 0 || side >= SIDE_COUNT) return false;
	const FPosedPoints &r = gPosedPts[side][gPosedSlot[side].load(std::memory_order_acquire)];
	const uint64_t now = screen != nullptr ? screen->FrameCount : 0;
	if (r.model == model && r.frame != ~0ull && now >= r.frame && now - r.frame <= 4)
	{
		out.tip = r.p[PT_TIP]; out.indexBase = r.p[PT_BASE]; out.palm = r.p[PT_PALM];
		out.wrist = r.p[PT_WRIST]; out.middleBase = r.p[PT_MIDDLE];
		out.posed = true;
		return true;
	}
	DVector3 p[PT_COUNT];
	for (int k = 0; k < PT_COUNT; k++)
		if (!RestPoint(model, model->FindJoint(FName(kPointJoints[k])), p[k])) return false;
	out.tip = p[PT_TIP]; out.indexBase = p[PT_BASE]; out.palm = p[PT_PALM];
	out.wrist = p[PT_WRIST]; out.middleBase = p[PT_MIDDLE];
	return true;
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

	gSidePoses[side] = &c->poses[side];
	gSideHold[side] = &c->hold[side];
	// The posing's CPU cost, into the perf log as "fx.handpose" (both hands summed per frame), so a slow
	// frame with the hands on can be told from one with the VR picker on (roth_vrpick.cpp, "game.vrpick").
	const bool timed = PerfLog::GroupsWanted();
	const uint64_t t0 = timed ? I_nsTime() : 0;
	struct TimeOnExit { bool on; uint64_t t0; ~TimeOnExit() { if (on) PerfLog::AddCpuSample("fx.handpose", (double)(I_nsTime() - t0) / 1e6); } } timeOnExit{ timed, t0 };
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
		reinterpret_cast<float (*)[16]>(out.data()), scratch, moved, gPalm[side].Q);
	posed.Resize(n);
	for (int i = 0; i < n; i++) posed[i].loadMatrix(&out[(size_t)i * 16]);
	StorePosedPoints(model, side, out.data(), n);
	return &posed;
}
