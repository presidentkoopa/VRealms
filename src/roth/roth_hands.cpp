//
// THE VR HANDS' ACTORS. See roth_hands.h; the drawing is r_data/vr_handpose.cpp.
//
// While a headset is on (vr_mode set and the VR mode reporting VR), one
// RealmsHandRight and one RealmsHandLeft stand with the player: the physical
// right hand rides the controller that hand holds (FollowHandMode 3 for the
// main hand, 4 for the off hand, by vr_control_scheme). In flat there are none
// -- they are destroyed if VR goes away -- unless the flat test below is on.
//
// The actor's own position only decides culling and the sector light it is
// drawn in, so it stands at the matching shooting point (AttackPos for the main
// hand, OffhandPos for the off hand), as RealmsHeld does. With no grip pose for
// that controller this tic it is hidden: UZDXREMA's "no controller, no hand".
//
// SAVES. The actors are ordinary world actors and are written into a save. On a
// load the pointers are gone (EndLevel) and the ones from the save are adopted
// the next tic, so a load never doubles the hands; loaded in flat, they are
// destroyed.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_hands.h"
#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "hw_vrmodes.h"
#include "r_data/vr_handpose.h"
#include "r_data/vr_handpose_math.h"

EXTERN_CVAR(Int, vr_mode)   // file scope, never inside a namespace

namespace roth
{

namespace
{

AActor *g_hand[VRHandPose::SIDE_COUNT] = { nullptr, nullptr };
const char *const kHandClass[VRHandPose::SIDE_COUNT] = { "RealmsHandRight", "RealmsHandLeft" };

// The ones a loaded save brought back; extra copies are destroyed.
AActor *Adopt(FLevelLocals *level, PClassActor *cls)
{
	AActor *keep = nullptr;
	auto it = level->GetThinkerIterator<AActor>(cls->TypeName);
	AActor *mo;
	TArray<AActor *> extra;
	while ((mo = it.Next()) != nullptr)
	{
		if (mo->GetClass() != cls || (mo->ObjectFlags & OF_EuthanizeMe)) continue;
		if (keep == nullptr) keep = mo;
		else extra.Push(mo);
	}
	for (AActor *x : extra) x->Destroy();
	return keep;
}

} // namespace (anonymous)

AActor *HandActor(int side)
{
	if (side < 0 || side >= VRHandPose::SIDE_COUNT) return nullptr;
	AActor *mo = g_hand[side];
	if (mo == nullptr || (mo->ObjectFlags & OF_EuthanizeMe) || (mo->renderflags & RF_INVISIBLE)) return nullptr;
	return mo;
}

void HandsEndLevel()
{
	g_hand[0] = g_hand[1] = nullptr;
}

void HandsTick(FLevelLocals *level)
{
	if (level == nullptr) return;
	AActor *pmo = (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? players[consoleplayer].mo : nullptr;
	if (pmo != nullptr && pmo->Level != level) pmo = nullptr;

	const VRMode *vm = VRMode::GetVRModeCached(true);
	const bool vr = vr_mode != 0 && vm != nullptr && vm->IsVR();
	const bool test = VRHandTest_Active();
	const bool want = pmo != nullptr && (vr || test);

	for (int side = 0; side < VRHandPose::SIDE_COUNT; side++)
	{
		PClassActor *cls = PClass::FindActor(kHandClass[side]);
		if (cls == nullptr) continue;

		AActor *&mo = g_hand[side];
		if (mo != nullptr && (mo->ObjectFlags & OF_EuthanizeMe)) mo = nullptr;
		if (mo == nullptr) mo = Adopt(level, cls);

		if (!want)
		{
			if (mo != nullptr) { mo->Destroy(); mo = nullptr; }
			continue;
		}

		const int role = VRHandPose_RoleForSide(side);
		DVector3 at;
		if (test)
			VRHandTest_ActorSpot(side, DVector3(pmo->X(), pmo->Y(), pmo->player ? pmo->player->viewz : pmo->Z()), pmo->Angles.Yaw.Degrees(), at);
		else
			at = (role == VR_MAINHAND) ? pmo->AttackPos : pmo->OffhandPos;

		if (mo == nullptr) mo = AActor::StaticSpawn(level, cls, at, NO_REPLACE);
		if (mo == nullptr) continue;

		mo->FollowHandMode = (role == VR_MAINHAND) ? FOLLOWHAND_GRIP_MAIN : FOLLOWHAND_GRIP_OFF;
		mo->SetOrigin(at, true);
		mo->Angles.Yaw = pmo->Angles.Yaw;

		VSMatrix grip;
		const bool have = test || (vm != nullptr && vm->GetGripTransform(role, &grip));
		if (have) mo->renderflags &= ~RF_INVISIBLE;
		else mo->renderflags |= RF_INVISIBLE;
	}
}

} // namespace roth

//===========================================================================
//
// roth_handtest -- THE FLAT TEST. Test-only: inert until typed, never run by
// anything else. Both hands are spawned in front of the camera with the given
// inputs, through the whole VR hand path (grip -> hand fit -> seat -> fingers
// -> smoothing), so rothdiff_shot can photograph them in flat.
//
//   roth_handtest <pose> [turn]
//       open      grip 0, trigger 0, no touches
//       fist      grip 1, trigger 1, trigger touch, thumb touch   (= grab)
//       grab      the same
//       point     grip 1, trigger 0, no trigger touch, thumb touch
//       thumbup   grip 1, trigger 1, trigger touch, NO thumb touch
//       fingergun grip 1, trigger 0, no trigger touch, no thumb touch
//       trigger   grip 0, trigger 1, trigger touch, no thumb touch
//       ready     grip 1, trigger 0, trigger touch, thumb touch
//   roth_handtest <grip> <trigger> <triggertouch 0|1> <thumbtouch 0|1> [turn]
//   roth_handtest off
//
// turn: degrees each palm swings inward from facing the camera (90 = the curl
// seen in profile). Default 0.
//
//===========================================================================

CCMD(roth_handtest)
{
	struct Named { const char *name; float grip, trigger; bool tt, th; };
	static const Named poses[] =
	{
		{ "open",      0.f, 0.f, false, false },
		{ "fist",      1.f, 1.f, true,  true  },
		{ "grab",      1.f, 1.f, true,  true  },
		{ "point",     1.f, 0.f, false, true  },
		{ "thumbup",   1.f, 1.f, true,  false },
		{ "fingergun", 1.f, 0.f, false, false },
		{ "trigger",   0.f, 1.f, true,  false },
		{ "ready",     1.f, 0.f, true,  true  },
	};

	if (argv.argc() < 2)
	{
		Printf("roth_handtest <open|fist|grab|point|thumbup|fingergun|trigger|ready> [turn]\n"
			"roth_handtest <grip> <trigger> <triggertouch 0|1> <thumbtouch 0|1> [turn]\n"
			"roth_handtest off\n");
		return;
	}
	if (!stricmp(argv[1], "off"))
	{
		VRHandTest_Set(false, FVRHandTestInputs(), 0.f);
		Printf("roth_handtest: off\n");
		return;
	}

	FVRHandTestInputs in;
	float turn = 0.f;
	const char *label = nullptr;
	for (const Named &p : poses)
	{
		if (!stricmp(argv[1], p.name))
		{
			in.grip = p.grip; in.trigger = p.trigger; in.triggerTouch = p.tt; in.thumbTouch = p.th;
			label = p.name;
			if (argv.argc() >= 3) turn = (float)atof(argv[2]);
			break;
		}
	}
	if (label == nullptr)
	{
		if (argv.argc() < 5)
		{
			Printf("roth_handtest: unknown pose '%s'\n", argv[1]);
			return;
		}
		in.grip = (float)atof(argv[1]);
		in.trigger = (float)atof(argv[2]);
		in.triggerTouch = atoi(argv[3]) != 0;
		in.thumbTouch = atoi(argv[4]) != 0;
		if (argv.argc() >= 6) turn = (float)atof(argv[5]);
		label = "custom";
	}
	VRHandTest_Set(true, in, turn);
	Printf("roth_handtest: %s -- grip %.2f trigger %.2f, trigger touch %d, thumb touch %d, turn %.0f\n",
		label, in.grip, in.trigger, (int)in.triggerTouch, (int)in.thumbTouch, turn);
}
