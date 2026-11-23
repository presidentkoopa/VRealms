//
// roth_xrpose.cpp -- THE CONTROLLER-POSE TEST. Test-only, inert until typed.
//
// WHY IT EXISTS (2026-11-16). The owner, in the headset: lasers pointing behind him or off to the side,
// sprites floating through and orbiting his hand. The flat hand test (roth_handtest) never touched the
// path that did it: it builds its own grip in front of the camera and the game layer never locked a
// billboard to a hand under it. The fault was in the readers of the REAL controller path -- three of
// them took the aim pose's stored pitch with the wrong sign, and every hand-locked billboard rode the aim
// pose instead of the drawn hand. This drives that real path from given poses, on a desk, in flat:
//
//   given tracking-space poses for the head and each hand (metres, +X right, +Y up, -Z ahead)
//     -> r_data/vr_posemath.h VRPose_HeadRelativeOffset + VRPose_OffsetToWorld, the device's own
//        arithmetic (vk_openxrdevice.cpp calls the same two functions)
//     -> the grip transform every hand reader takes (VRHandPose_Grip), the hand frame and the drawn hand
//        (VRHandPose_HandFrame, FollowHandMode 3/4), its fingers (the test's finger values)
//     -> the picker's ray (roth_vrpick.h VRPickHandRay), the hand-locked billboards
//        (g_levellocals.h BillboardHandBasis), the panels' pointer, the held objects
//     -> and the aim fields the device publishes on the pawn (VRPose_AimAngles, VRPose_PublishedAim)
//
//   roth_xrpose on [headyaw]          the test on: the head at the player's eye (its height is the view
//                                     height in metres), facing `headyaw` in the tracking space (default 0)
//   roth_xrpose off
//   roth_xrpose hand <main|off> <x> <y> <z> <yaw> <pitch> <roll> [grip trigger ttouch thtouch]
//                                     a hand at (x, y, z) metres from the head in the head's own frame (x right,
//                                     y up, z back), its GRIP turned by yaw / pitch / roll (vr_posemath.h
//                                     VRPose_QuatFromYPR, after the head's yaw). Fingers default open.
//   roth_xrpose point <main|off> <x> <y> <z> <yaw> <pitch> [wristroll] [grip trigger ttouch thtouch]
//                                     the same, the hand's fingers along yaw / pitch from the head's forward
//                                     (the grip pitched +90: DefaultHandFit's fingers are -Y_grip), the wrist
//                                     turned about the fingers by `wristroll`. Fingers default to the point pose.
//   roth_xrpose aim <main|off> <yaw> <pitch> <roll> | aim <main|off> grip
//                                     the hand's AIM pose (what the device publishes on the pawn); `grip`: the
//                                     default, the aim's -Z along the fingers
//   roth_xrpose log                   each hand now: its frame, the picker's ray against the drawn finger and
//                                     against the hand's forward, and the published aim
//   roth_xrpose pin <main|off> <palm|tip>
//                                     a hand-locked marker at the drawn palm (or fingertip), placed ONCE. Every
//                                     tic after, a line: where the renderer drew it (drawPos) and where it
//                                     resolves now, against where the drawn joint is now. It must stay on it.
//   roth_xrpose pin off
//
// Single player. Nothing here is saved, archived or read by the playsim except through the published
// fields, exactly as a headset's are.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_hands.h"
#include "roth_vrpick.h"
#include "roth_objects.h"
#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "printf.h"
#include "roth_runtime.h"   // ObjectIsPickupItem (handat / pointat item)
#include "hw_vrmodes.h"
#include "r_utility.h"
#include "r_data/models.h"
#include "r_data/vr_handpose.h"
#include "r_data/vr_posemath.h"
#include <math.h>


namespace roth
{

namespace
{

struct FPin
{
	bool on = false;
	int role = 0;
	bool tip = false;
	int id = 0;
	FLevelLocals *level = nullptr;
	int placedTic = 0;
	double worst = 0.0;    // the largest error seen, map units (from the second frame on)
};
FPin gPin;

const char *RoleName(int role) { return role == VR_MAINHAND ? "main" : "off"; }

double UPM()
{
	const double u = VR_UnitsPerMeter();
	return u > 1e-3 ? u : 34.0;
}

double AngleBetween(const DVector3 &a, const DVector3 &b)
{
	const double la = a.Length(), lb = b.Length();
	if (la < 1e-9 || lb < 1e-9) return -1.0;
	double c = (a | b) / (la * lb);
	c = c > 1.0 ? 1.0 : (c < -1.0 ? -1.0 : c);
	return acos(c) * (180.0 / M_PI);
}

// The drawn hand's joints in the world (the same reads as the picker's FingerRay).
bool DrawnPoints(int role, DVector3 &tip, DVector3 &base, DVector3 &palm)
{
	const int side = VRHandPose_SideForRole(role);
	AActor *ha = HandActor(side);
	FModel *model = ha != nullptr ? Model_ActorModel(ha) : nullptr;
	FVRHandPoints pts;
	if (model == nullptr || !VRHandPose_FingerPoints(model, side, pts)) return false;
	return Model_ActorPointToWorld(ha, pts.tip, tip) && Model_ActorPointToWorld(ha, pts.indexBase, base)
		&& Model_ActorPointToWorld(ha, pts.palm, palm);
}

FBillboard *FindBB(FLevelLocals *level, int id)
{
	if (level == nullptr || id == 0) return nullptr;
	for (FBillboard &b : level->Billboards) if (b.id == id) return &b;
	return nullptr;
}

// The nearest Realms object actor whose use is the original's pickup (roth_runtime.h ObjectIsPickupItem).
AActor *NearestPickupItem(AActor *pmo)
{
	if (pmo == nullptr || pmo->Level == nullptr) return nullptr;
	AActor *best = nullptr;
	double bestD = 1e18;
	auto it = pmo->Level->GetThinkerIterator<AActor>();
	AActor *mo;
	while ((mo = it.Next()) != nullptr)
	{
		int rs = -1, ri = -1;
		if (mo == pmo || (mo->ObjectFlags & OF_EuthanizeMe) || !ObjectForActor(mo, rs, ri) || !ObjectIsPickupItem(mo)) continue;
		const double d = (mo->Pos() - pmo->Pos()).Length();
		if (d < bestD) { bestD = d; best = mo; }
	}
	return best;
}

// A TEST TARGET: "item" (the nearest pickup item) or "object <sector> <index>" (that Realms object's actor), from
// argv[a] on; `a` is left after it. Null: none.
AActor *TargetActor(AActor *pmo, FCommandLine &argv, int &a)
{
	if (a >= argv.argc()) return nullptr;
	if (!stricmp(argv[a], "item")) { a++; return NearestPickupItem(pmo); }
	if (!stricmp(argv[a], "object") && a + 2 < argv.argc())
	{
		const int rs = atoi(argv[a + 1]), ri = atoi(argv[a + 2]);
		a += 3;
		return FindObjectActor(rs, ri);
	}
	return nullptr;
}

void Report()
{
	AActor *pmo = (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? players[consoleplayer].mo : nullptr;
	if (pmo == nullptr) { Printf("[roth_xrpose] no player\n"); return; }
	const double upm = UPM();
	Printf("[roth_xrpose] tic %d: player yaw %.1f, view yaw %.1f, %.2f units per metre, the test %s\n",
		pmo->Level != nullptr ? pmo->Level->maptime : -1, pmo->Angles.Yaw.Degrees(), r_viewpoint.Angles.Yaw.Degrees(), upm,
		VRPoseTest_Active() ? "on" : "OFF");
	for (int role = 0; role < 2; role++)
	{
		DVector3 o, f, r, u;
		if (!VRHandPose_MapFrame(role, o, f, r, u)) { Printf("[roth_xrpose] %s hand: no grip -- no hand frame\n", RoleName(role)); continue; }
		Printf("[roth_xrpose] %s hand frame: origin (%.1f %.1f %.1f) fwd (%.3f %.3f %.3f) right (%.3f %.3f %.3f) up (%.3f %.3f %.3f)\n",
			RoleName(role), o.X, o.Y, o.Z, f.X, f.Y, f.Z, r.X, r.Y, r.Z, u.X, u.Y, u.Z);
		DVector3 s, d;
		bool fromFinger = false;
		if (!VRPickHandRay(role, s, d, &fromFinger)) { Printf("[roth_xrpose] %s hand: no ray\n", RoleName(role)); continue; }
		DVector3 tip, base, palm;
		const bool drawn = DrawnPoints(role, tip, base, palm);
		Printf("[roth_xrpose] %s hand RAY from the %s (%.1f %.1f %.1f) dir (%.3f %.3f %.3f): yaw %.1f pitch %.1f (up +)%s\n",
			RoleName(role), fromFinger ? "drawn fingertip" : "hand frame", s.X, s.Y, s.Z, d.X, d.Y, d.Z,
			atan2(d.Y, d.X) * (180.0 / M_PI), atan2(d.Z, sqrt(d.X * d.X + d.Y * d.Y)) * (180.0 / M_PI), drawn ? "" : " (no drawn hand)");
		if (drawn)
		{
			const DVector3 fd = tip - base;
			Printf("[roth_xrpose] %s hand: drawn index finger (knuckle -> tip) dir (%.3f %.3f %.3f); ray vs drawn finger %.2f deg, "
				"start vs drawn fingertip %.2f units; ray vs the hand's forward %.1f deg; drawn palm %.1f units from the frame origin\n",
				RoleName(role), fd.X / fd.Length(), fd.Y / fd.Length(), fd.Z / fd.Length(), AngleBetween(d, fd), (s - tip).Length(),
				AngleBetween(d, f), (palm - o).Length());
		}
		else Printf("[roth_xrpose] %s hand: ray vs the hand's forward %.2f deg\n", RoleName(role), AngleBetween(d, f));
		// The aim pose as the device publishes it, as a direction.
		const bool main = role == VR_MAINHAND;
		const double ay = (main ? pmo->AttackAngle : pmo->OffhandAngle).Degrees() + 90.0;
		const double ap = (main ? pmo->AttackPitch : pmo->OffhandPitch).Degrees();
		const double ar = (main ? pmo->AttackRoll : pmo->OffhandRoll).Degrees();
		const DVector3 ad(cos(ay * M_PI / 180) * cos(ap * M_PI / 180), sin(ay * M_PI / 180) * cos(ap * M_PI / 180), sin(ap * M_PI / 180));
		const DVector3 apos = main ? pmo->AttackPos : pmo->OffhandPos;
		Printf("[roth_xrpose] %s hand published aim: pos (%.1f %.1f %.1f) yaw %.1f pitch %.1f (up +) roll %.1f -> dir (%.3f %.3f %.3f), "
			"%.1f deg from the hand's forward (the device's -40 deg aim bias is in it)\n",
			RoleName(role), apos.X, apos.Y, apos.Z, ay, ap, ar, ad.X, ad.Y, ad.Z, AngleBetween(ad, f));
	}
}

} // namespace (anonymous)

void XRPoseTick(FLevelLocals *level)
{
	if (VRPoseTest_Active() && consoleplayer >= 0 && consoleplayer < MAXPLAYERS)
		VRPoseTest_PublishAim(players[consoleplayer].mo);   // as p_user.cpp does after the canonical writes

	if (!gPin.on || level == nullptr) return;
	if (gPin.level != level) { gPin = FPin(); return; }
	FBillboard *b = FindBB(level, gPin.id);
	if (b == nullptr) { gPin.on = false; return; }
	DVector3 tip, base, palm;
	if (!DrawnPoints(gPin.role, tip, base, palm)) { Printf("[roth_xrpose] pin: no drawn %s hand this tic\n", RoleName(gPin.role)); return; }
	const DVector3 joint = gPin.tip ? tip : palm;
	DVector3 o, f, r, u;
	if (!BillboardHandBasis(*b, o, f, r, u)) { Printf("[roth_xrpose] pin: no hand frame this tic\n"); return; }
	const DVector3 now = o + f * b->pos.X + r * b->pos.Y + u * b->pos.Z;
	const double errNow = (now - joint).Length(), errDrawn = (b->drawPos - joint).Length();
	const double upm = UPM();
	if (level->maptime > gPin.placedTic + 1 && errNow > gPin.worst) gPin.worst = errNow;
	Printf("[roth_xrpose] pin %s %s tic %d: drawn %s (%.1f %.1f %.1f); the marker resolves now at (%.1f %.1f %.1f), error %.2f units (%.1f mm); "
		"last drawn at (%.1f %.1f %.1f), %.2f units off the joint now; worst so far %.2f units (%.1f mm)\n",
		RoleName(gPin.role), gPin.tip ? "tip" : "palm", level->maptime, gPin.tip ? "fingertip" : "palm", joint.X, joint.Y, joint.Z,
		now.X, now.Y, now.Z, errNow, errNow / upm * 1000.0, b->drawPos.X, b->drawPos.Y, b->drawPos.Z, errDrawn, gPin.worst, gPin.worst / upm * 1000.0);
}

} // namespace roth

//===========================================================================
//
// The command (see the header).
//
//===========================================================================

static int RoleArg(const char *s)
{
	if (!stricmp(s, "main")) return VR_MAINHAND;
	if (!stricmp(s, "off")) return VR_OFFHAND;
	return -1;
}

static void QMul(const float a[4], const float b[4], float o[4])
{
	o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

CCMD(roth_xrpose)
{
	if (argv.argc() < 2)
	{
		Printf("roth_xrpose on [headyaw] | off | hand <main|off> x y z yaw pitch roll [grip trig tt th] | point <main|off> x y z yaw pitch [wristroll] [grip trig tt th]\n"
			"            | grip|trigger <main|off> <0..1> | handat <main|off> item|object s i [palm] [dx dy dz] | pointat <main|off> item|object s i|x y z\n"
			"            | stand item|object s i <metres>\n"
			"            | aim <main|off> yaw pitch roll | aim <main|off> grip | log | pin <main|off> <palm|tip> | pin off\n");
		return;
	}
	const char *verb = argv[1];
	if (!stricmp(verb, "off"))
	{
		VRPoseTest_Set(false, 0.f, nullptr);
		Printf("roth_xrpose: off\n");
		return;
	}
	if (!stricmp(verb, "on"))
	{
		const float headYaw = argv.argc() >= 3 ? (float)atof(argv[2]) : 0.f;
		player_t *pl = (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? &players[consoleplayer] : nullptr;
		const double ps = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
		const double upm = VR_UnitsPerMeter() > 1e-3 ? VR_UnitsPerMeter() : 34.0;
		const double vh = (pl != nullptr && pl->mo != nullptr) ? pl->DefaultViewHeight() : 41.0;
		const float head[3] = { 0.f, (float)(vh * ps / upm), 0.f };
		VRPoseTest_Set(true, headYaw, head);
		Printf("roth_xrpose: on -- the head at the eye (%.3f m up in the tracking space, %.1f units of view height at %.2f units per metre), "
			"tracking yaw %.1f\n", head[1], vh, upm, headYaw);
		return;
	}
	if (!VRPoseTest_Active() && stricmp(verb, "log") && stricmp(verb, "pin"))
	{
		Printf("roth_xrpose: the test is off (roth_xrpose on first)\n");
		return;
	}
	if (!stricmp(verb, "hand") || !stricmp(verb, "point"))
	{
		const bool point = !stricmp(verb, "point");
		const int need = point ? 8 : 9;
		const int role = argv.argc() >= 3 ? RoleArg(argv[2]) : -1;
		if (role < 0 || argv.argc() < need) { Printf("roth_xrpose %s <main|off> x y z yaw pitch %s\n", verb, point ? "[wristroll]" : "roll"); return; }
		float head[3]; float headYaw = 0.f;
		VRPoseTest_Head(head, headYaw);
		const double x = atof(argv[3]), y = atof(argv[4]), z = atof(argv[5]);
		const double yaw = atof(argv[6]), pitch = atof(argv[7]);
		const double roll = argv.argc() >= 9 ? atof(argv[8]) : 0.0;
		FVRPoseTestHand h;
		h.valid = true;
		// the head's frame -> the tracking space: turned by the head's yaw about +Y
		const double hy = headYaw * (M_PI / 180.0);
		h.pos[0] = head[0] + (float)(x * cos(hy) + z * sin(hy));
		h.pos[1] = head[1] + (float)y;
		h.pos[2] = head[2] + (float)(-x * sin(hy) + z * cos(hy));
		if (point)
		{
			float qy[4], qp[4], qr[4], t[4];
			VRPose_QuatFromYPR(headYaw + yaw, 0, 0, qy);
			VRPose_QuatFromYPR(0, 90.0 + pitch, 0, qp);
			VRPose_QuatFromYPR(roll, 0, 0, qr);   // about the grip's +Y: the fingers' axis
			QMul(qy, qp, t);
			QMul(t, qr, h.quat);
			h.grip = 1.f; h.trigger = 0.f; h.triggerTouch = false; h.thumbTouch = true;   // roth_handtest "point"
		}
		else VRPose_QuatFromYPR(headYaw + yaw, pitch, roll, h.quat);
		const int first = 9;   // the finger values follow the roll (for `point`, give the wrist roll to give them)
		if (argv.argc() >= first + 4)
		{
			h.grip = (float)atof(argv[first]); h.trigger = (float)atof(argv[first + 1]);
			h.triggerTouch = atoi(argv[first + 2]) != 0; h.thumbTouch = atoi(argv[first + 3]) != 0;
		}
		VRPoseTest_SetHand(role, h);
		Printf("roth_xrpose: %s hand %s at (%.2f %.2f %.2f) m from the head, yaw %.1f pitch %.1f %s %.1f; grip %.2f trigger %.2f, touches %d %d\n",
			roth::RoleName(role), point ? "points" : "grip turned", x, y, z, yaw, pitch, point ? "wrist roll" : "roll", roll,
			h.grip, h.trigger, (int)h.triggerTouch, (int)h.thumbTouch);
		return;
	}
	// THE GRIP: the hand's analog grip, which is what VRealms' hands read (roth_vrpick.cpp ReadInputs). The device's
	// grip key is not posted: nothing in the Realms reads it.
	if (!stricmp(verb, "grip") || !stricmp(verb, "trigger"))
	{
		const bool trig = !stricmp(verb, "trigger");
		const int role = argv.argc() >= 4 ? RoleArg(argv[2]) : -1;
		if (role < 0) { Printf("roth_xrpose %s <main|off> <0..1>\n", verb); return; }
		FVRPoseTestHand h;
		if (!VRPoseTest_Hand(role, h) || !h.valid) { Printf("roth_xrpose %s: the %s hand has no pose (roth_xrpose hand/point first)\n", verb, roth::RoleName(role)); return; }
		if (trig) { h.trigger = (float)atof(argv[3]); h.triggerTouch = h.trigger > 0.f; }
		else h.grip = (float)atof(argv[3]);
		VRPoseTest_SetHand(role, h);
		Printf("roth_xrpose: %s hand %s %.2f\n", roth::RoleName(role), verb, trig ? h.trigger : h.grip);
		return;
	}
	if (!stricmp(verb, "aim"))
	{
		const int role = argv.argc() >= 3 ? RoleArg(argv[2]) : -1;
		if (role < 0 || argv.argc() < 4) { Printf("roth_xrpose aim <main|off> yaw pitch roll | grip\n"); return; }
		if (!stricmp(argv[3], "grip")) { VRPoseTest_SetAim(role, nullptr); Printf("roth_xrpose: %s hand aims along its fingers\n", roth::RoleName(role)); return; }
		if (argv.argc() < 6) { Printf("roth_xrpose aim <main|off> yaw pitch roll\n"); return; }
		float head[3]; float headYaw = 0.f;
		VRPoseTest_Head(head, headYaw);
		float q[4];
		VRPose_QuatFromYPR(headYaw + atof(argv[3]), atof(argv[4]), atof(argv[5]), q);
		VRPoseTest_SetAim(role, q);
		Printf("roth_xrpose: %s hand aim yaw %s pitch %s roll %s\n", roth::RoleName(role), argv[3], argv[4], argv[5]);
		return;
	}
	// THE HAND ON A THING (docs/VR_INTERACTION.md P1: a pickup by touch).
	//   handat <main|off> item [palm] [dx dy dz]
	// moves the hand (tracking space, its orientation kept) so that its published position (AttackPos / OffhandPos,
	// the drawn hand frame's origin) -- or, with `palm`, its DRAWN PALM as last drawn -- lands on the middle of the
	// nearest Realms object whose use is the original's pickup (roth_runtime.h ObjectIsPickupItem), plus (dx dy dz)
	// map units. The device's map from the hand's tracking position to that point is affine (vr_posemath.h
	// VRPose_OffsetToWorld: the origin does not depend on the rotation), so it is solved exactly from four probes
	// through the same publishing path the playsim calls (VRPoseTest_PublishAim). `palm` adds the drawn palm's offset
	// from the frame origin, which the rotation does not change: give the hand its pose first, and a tic to be drawn.
	if (!stricmp(verb, "handat"))
	{
		using namespace roth;
		const int role = argv.argc() >= 4 ? RoleArg(argv[2]) : -1;
		AActor *pmo = (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? players[consoleplayer].mo : nullptr;
		FVRPoseTestHand h;
		if (role < 0 || pmo == nullptr) { Printf("roth_xrpose handat <main|off> item|object <s> <i> [palm] [dx dy dz]\n"); return; }
		if (!VRPoseTest_Hand(role, h) || !h.valid) { Printf("roth_xrpose handat: the %s hand has no pose (roth_xrpose hand first)\n", RoleName(role)); return; }
		int a = 3;
		AActor *best = TargetActor(pmo, argv, a);
		const bool palm = argv.argc() > a && !stricmp(argv[a], "palm");
		if (palm) a++;
		if (best == nullptr) { Printf("roth_xrpose handat: no such target (item: the nearest pickup item; object <s> <i>)\n"); return; }
		DVector3 want = best->PosPlusZ(best->Height * 0.5);
		if (argv.argc() >= a + 3) want += DVector3(atof(argv[a]), atof(argv[a + 1]), atof(argv[a + 2]));
		DVector3 palmOff(0, 0, 0);
		if (palm)
		{
			DVector3 tip, base, dp;
			DVector3 o, f, r, u;
			if (!DrawnPoints(role, tip, base, dp) || !VRHandPose_MapFrame(role, o, f, r, u)) { Printf("roth_xrpose handat palm: no drawn %s hand yet\n", RoleName(role)); return; }
			palmOff = dp - (role == VR_MAINHAND ? pmo->AttackPos : pmo->OffhandPos);
			want -= palmOff;
		}
		auto publish = [&](const float p[3]) -> DVector3
		{
			FVRPoseTestHand t = h;
			t.pos[0] = p[0]; t.pos[1] = p[1]; t.pos[2] = p[2];
			VRPoseTest_SetHand(role, t);
			VRPoseTest_PublishAim(pmo);
			return role == VR_MAINHAND ? pmo->AttackPos : pmo->OffhandPos;
		};
		const float p0[3] = { h.pos[0], h.pos[1], h.pos[2] };
		const DVector3 b = publish(p0);
		DVector3 col[3];
		for (int i = 0; i < 3; i++)
		{
			float pi[3] = { p0[0], p0[1], p0[2] };
			pi[i] += 1.f;
			col[i] = publish(pi) - b;
		}
		const DVector3 rr = want - b;
		auto det3 = [](const DVector3 &x, const DVector3 &c1, const DVector3 &c2) { return x | (c1 ^ c2); };
		const double D = det3(col[0], col[1], col[2]);
		if (fabs(D) < 1e-9) { publish(p0); Printf("roth_xrpose handat: the device's map is singular here\n"); return; }
		const double x0 = det3(rr, col[1], col[2]) / D, x1 = det3(col[0], rr, col[2]) / D, x2 = det3(col[0], col[1], rr) / D;
		const float pf[3] = { (float)(p0[0] + x0), (float)(p0[1] + x1), (float)(p0[2] + x2) };
		const DVector3 got = publish(pf);
		int rs = -1, ri = -1;
		ObjectForActor(best, rs, ri);
		Printf("roth_xrpose handat: the %s hand%s on Realms object %d/%d%s at (%.1f %.1f %.1f): published at (%.1f %.1f %.1f)%s,"
			" %.2f units off\n", RoleName(role), palm ? "'s palm" : "", rs, ri, ObjectIsPickupItem(best) ? " (a pickup item)" : "", want.X + palmOff.X, want.Y + palmOff.Y, want.Z + palmOff.Z,
			got.X, got.Y, got.Z, palm ? FStringf(", the palm %.1f units from it", palmOff.Length()).GetChars() : "",
			(got - want).Length());
		return;
	}
	// THE FINGER AT A THING (docs/VR_INTERACTION.md P1: a pickup by point).
	//   pointat <main|off> item | pointat <main|off> <x> <y> <z>
	// keeps the hand where it is and turns it, in the point pose, so its drawn index finger points at the middle of the
	// nearest pickup item (as handat) or at a map point: the yaw and pitch of `point`, from the drawn fingertip (the
	// picker's ray start, VRPickHandRay) to the target, relative to the body's yaw. Give it twice a tic apart to settle
	// (the fingertip moves a little as the hand turns).
	if (!stricmp(verb, "pointat"))
	{
		using namespace roth;
		const int role = argv.argc() >= 4 ? RoleArg(argv[2]) : -1;
		AActor *pmo = (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? players[consoleplayer].mo : nullptr;
		FVRPoseTestHand h;
		if (role < 0 || pmo == nullptr) { Printf("roth_xrpose pointat <main|off> item | x y z\n"); return; }
		if (!VRPoseTest_Hand(role, h) || !h.valid) { Printf("roth_xrpose pointat: the %s hand has no pose\n", RoleName(role)); return; }
		DVector3 at;
		int a = 3;
		if (AActor *best = TargetActor(pmo, argv, a)) at = best->PosPlusZ(best->Height * 0.5);
		else if (argv.argc() >= 6 && stricmp(argv[3], "item") && stricmp(argv[3], "object")) at = DVector3(atof(argv[3]), atof(argv[4]), atof(argv[5]));
		else { Printf("roth_xrpose pointat <main|off> item | object <s> <i> | x y z: no such target\n"); return; }
		DVector3 s, d;
		const bool haveRay = VRPickHandRay(role, s, d);
		if (!haveRay) s = role == VR_MAINHAND ? pmo->AttackPos : pmo->OffhandPos;
		const DVector3 v = at - s;
		const double wantYaw = atan2(v.Y, v.X) * (180.0 / M_PI);
		const double wantPitch = atan2(v.Z, sqrt(v.X * v.X + v.Y * v.Y)) * (180.0 / M_PI);
		auto norm = [](double a) { while (a > 180.0) a -= 360.0; while (a < -180.0) a += 360.0; return a; };
		// THE DRAWN FINGER IS NOT THE GRIP'S AXIS (about 24 degrees off it on the FBX hands): the second and later calls
		// correct the command by what the drawn finger did with the last one (the ray as the picker reads it).
		static double sYaw[2] = { 0, 0 }, sPitch[2] = { 0, 0 };
		static bool sHave[2] = { false, false };
		double yaw = norm(wantYaw - r_viewpoint.Angles.Yaw.Degrees()), cmdPitch = wantPitch;
		if (sHave[role] && haveRay)
		{
			const double gotYaw = atan2(d.Y, d.X) * (180.0 / M_PI), gotPitch = atan2(d.Z, sqrt(d.X * d.X + d.Y * d.Y)) * (180.0 / M_PI);
			yaw = norm(sYaw[role] + norm(wantYaw - gotYaw));
			cmdPitch = sPitch[role] + (wantPitch - gotPitch);
			Printf("roth_xrpose pointat: the drawn finger went yaw %.1f pitch %.1f for the last command; correcting by %.1f / %.1f\n",
				gotYaw, gotPitch, norm(wantYaw - gotYaw), wantPitch - gotPitch);
		}
		sYaw[role] = yaw; sPitch[role] = cmdPitch; sHave[role] = true;
		float head[3]; float headYaw = 0.f;
		VRPoseTest_Head(head, headYaw);
		float qy[4], qp[4], t[4];
		VRPose_QuatFromYPR(headYaw + yaw, 0, 0, qy);
		VRPose_QuatFromYPR(0, 90.0 + cmdPitch, 0, qp);
		QMul(qy, qp, t);
		for (int k = 0; k < 4; k++) h.quat[k] = t[k];
		h.trigger = 0.f; h.triggerTouch = false; h.thumbTouch = true;   // the point pose: the index off the trigger
		VRPoseTest_SetHand(role, h);
		Printf("roth_xrpose pointat: the %s hand points from (%.1f %.1f %.1f) at (%.1f %.1f %.1f), %.1f units: yaw %.1f off the body, pitch %.1f\n",
			RoleName(role), s.X, s.Y, s.Z, at.X, at.Y, at.Z, v.Length(), yaw, wantPitch);
		return;
	}
	// THE PLAYER BEFORE A THING: `stand item|object <s> <i> <metres>` stands the player on the floor <metres> (map
	// units at roth_vr_units_per_metre) from the target, on the line from where he is, facing it, the view level.
	if (!stricmp(verb, "stand"))
	{
		using namespace roth;
		AActor *pmo = (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? players[consoleplayer].mo : nullptr;
		int a = 2;
		AActor *t = pmo != nullptr ? TargetActor(pmo, argv, a) : nullptr;
		if (t == nullptr || a >= argv.argc()) { Printf("roth_xrpose stand item|object <s> <i> <metres>\n"); return; }
		const double m = atof(argv[a]) * UPM();
		DVector2 d = pmo->Pos().XY() - t->Pos().XY();
		if (d.Length() < 1e-3) d = DVector2(1, 0);
		d /= d.Length();
		const DVector3 stand(t->X() + d.X * m, t->Y() + d.Y * m, pmo->Z());
		pmo->SetOrigin(stand, false);
		pmo->SetZ(pmo->floorz);
		pmo->Vel.Zero();
		pmo->Angles.Yaw = DAngle::fromDeg(atan2(-d.Y, -d.X) * (180.0 / M_PI));
		// the view pitched at the thing's middle (+ looks down), so a capture shows it and the hand on it
		const double eye = pmo->Z() + (pmo->player != nullptr ? pmo->player->viewheight : 41.0);
		pmo->Angles.Pitch = DAngle::fromDeg(atan2(eye - (t->Z() + t->Height * 0.5), std::max(1.0, m)) * (180.0 / M_PI));
		pmo->ClearInterpolation();
		int rs = -1, ri = -1;
		ObjectForActor(t, rs, ri);
		Printf("roth_xrpose stand: the player %.0f units (%s m) from Realms object %d/%d%s, at (%.0f %.0f %.0f), facing it\n", m, argv[a], rs, ri,
			ObjectIsPickupItem(t) ? " (a pickup item)" : "", pmo->X(), pmo->Y(), pmo->Z());
		return;
	}
	// `standat <x> <y> <angle512>`: the player on the floor at map (x, y), facing the original's angle (512 a turn,
	// counter-clockwise from +Y, as the rangeprobe's and rothdiff_shot's poses), the view level.
	if (!stricmp(verb, "standat") && argv.argc() >= 5)
	{
		AActor *pmo = (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? players[consoleplayer].mo : nullptr;
		if (pmo == nullptr) return;
		pmo->SetOrigin(DVector3(atof(argv[2]), atof(argv[3]), pmo->Z()), false);
		pmo->SetZ(pmo->floorz);
		pmo->Vel.Zero();
		pmo->Angles.Yaw = DAngle::fromDeg(90.0 + atof(argv[4]) * 360.0 / 512.0);
		pmo->Angles.Pitch = nullAngle;
		pmo->ClearInterpolation();
		Printf("roth_xrpose standat: the player at (%.0f %.0f %.0f), yaw %.1f\n", pmo->X(), pmo->Y(), pmo->Z(), pmo->Angles.Yaw.Degrees());
		return;
	}
	if (!stricmp(verb, "log")) { roth::Report(); return; }
	if (!stricmp(verb, "pin"))
	{
		using namespace roth;
		if (argv.argc() >= 3 && !stricmp(argv[2], "off"))
		{
			if (FBillboard *b = FindBB(gPin.level, gPin.id)) b->alpha = 0.0;
			Printf("roth_xrpose: pin off (worst error %.2f units)\n", gPin.worst);
			gPin = FPin();
			return;
		}
		const int role = argv.argc() >= 3 ? RoleArg(argv[2]) : -1;
		if (role < 0 || argv.argc() < 4) { Printf("roth_xrpose pin <main|off> <palm|tip> | pin off\n"); return; }
		AActor *pmo = (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? players[consoleplayer].mo : nullptr;
		FLevelLocals *level = pmo != nullptr ? pmo->Level : nullptr;
		DVector3 tip, base, palm;
		if (level == nullptr || !DrawnPoints(role, tip, base, palm)) { Printf("roth_xrpose pin: no drawn %s hand yet\n", RoleName(role)); return; }
		FBillboard probe;
		probe.flags = role == VR_MAINHAND ? BBFL_HANDMAIN : BBFL_HANDOFF;
		DVector3 o, f, r, u;
		if (!BillboardHandBasis(probe, o, f, r, u)) { Printf("roth_xrpose pin: no hand frame\n"); return; }
		const bool atTip = !stricmp(argv[3], "tip");
		const DVector3 w = atTip ? tip : palm;
		const DVector3 d = w - o;
		FBillboard bb;
		bb.id = level->NextBillboardID++;
		bb.payload = BB_RING;
		bb.facing = BBF_CAMERA;
		bb.flags = BBFL_PERSISTENT | BBFL_NOSAVE | BBFL_NOHIT | BBFL_NODEPTH | probe.flags;
		bb.pos = DVector3(d | f, d | r, d | u);
		bb.width = bb.height = 0.02 * UPM();
		bb.color = PalEntry(255, 60, 255, 90);
		bb.alpha = 0.9;
		bb.spawntic = level->maptime;
		level->Billboards.Push(bb);
		gPin = FPin();
		gPin.on = true; gPin.role = role; gPin.tip = atTip; gPin.id = bb.id; gPin.level = level; gPin.placedTic = level->maptime;
		Printf("roth_xrpose: pinned a marker to the %s hand's %s, hand-local (%.2f %.2f %.2f) units; it is never moved again\n",
			RoleName(role), atTip ? "fingertip" : "palm", bb.pos.X, bb.pos.Y, bb.pos.Z);
		return;
	}
	Printf("roth_xrpose: unknown '%s'\n", verb);
}
