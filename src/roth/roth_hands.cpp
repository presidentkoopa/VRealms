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
// WHAT THEY HOLD (roth_handhold.h): every tic the hands are told what is in
// them -- the hand on the RIGHT side the original's right-hand item, the main
// hand the weapon while RealmsHeld is drawn there -- and the fingers take the
// owner's hold or grip pose for it
// (r_data/vr_handpose_math.h SelectHold, UZDXREMA VAP :466-656).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_hands.h"
#include "roth_handhold.h"
#include "roth_interface.h"
#include "roth_inventory.h"
#include "roth_actors_engine.h"
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

// What each role holds this tic (roth_handhold.h), published to the hands' fingers.
// The weapon slot's thing while its actor is drawn, Adam's own item (his fists) not counted: what the main hand
// holds as a weapon (PublishHeld; the VR picker's trigger, HandHoldsWeapon).
static bool DrawnWeapon(int32_t &wpn, const char *&cls, int adam)
{
	wpn = 0;
	cls = nullptr;
	return ActorsHeldWeaponDrawn(wpn, cls) && wpn != adam;
}

bool HandHoldsWeapon(int role)
{
	if (role != VR_MAINHAND) return false;
	Inventory *inv = InterfaceInventory();
	const int adam = inv != nullptr ? (int)inv->AdamItemId() : 0;
	int32_t wpn = 0;
	const char *cls = nullptr;
	return DrawnWeapon(wpn, cls, adam);
}

static void PublishHeld()
{
	// the RIGHT side's hand: the original's right-hand item (g_selected_item_primary); 0 = nothing. The id is read
	// sign-extended, as the HUD's icon reads it (roth_interface.cpp RightHandChanged): a hidden id holds nothing.
	int item = 0;
	int adam = 0;
	if (Inventory *inv = InterfaceInventory())
	{
		adam = (int)inv->AdamItemId();
		const int slot = inv->RightHand();
		if (slot >= 0) item = (int16_t)inv->Slot(slot).id;
	}
	VRHandPose::HeldState held[2];
	const int itemRole = VRHandPose_RoleForSide(VRHandPose::SIDE_RIGHT);
	held[itemRole] = HeldFor(item > 0 ? item : 0, false);
	// the main hand: the weapon slot's thing, while its actor is drawn there. Adam's own item (his fists) is not a
	// thing in the hand.
	int32_t wpn = 0;
	const char *cls = nullptr;
	const bool drawn = DrawnWeapon(wpn, cls, adam);
	if (drawn)
	{
		// a right-handed player's item and weapon share the main hand: the weapon's grip wins while it is drawn
		// (VAP :476 takes the weapon branch first); the item's pose comes back when the weapon is put away
		const VRHandPose::HeldState w = HeldFor(wpn, true, cls);
		held[VR_MAINHAND].weapon = true;
		held[VR_MAINHAND].gripClass = w.gripClass;
		held[VR_MAINHAND].propClass = w.propClass;
		if (held[VR_MAINHAND].item == 0) held[VR_MAINHAND].item = w.item;
	}
	VRHandPose_SetHeld(VR_MAINHAND, held[VR_MAINHAND]);
	VRHandPose_SetHeld(VR_OFFHAND, held[VR_OFFHAND]);
}

void HandsTick(FLevelLocals *level)
{
	if (level == nullptr) return;
	PublishHeld();
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
		// The hands work the inventory while it pauses the world (the level's freeze, roth_runtime.cpp).
		mo->flags5 |= MF5_NOTIMEFREEZE;
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

//===========================================================================
//
// roth_handhold -- THE FLAT TEST'S HELD THING. Test-only, inert until typed.
// While roth_handtest is on, a physical hand takes the hold or grip pose of the
// thing given here (roth_handhold.h), in place of what the game has in it.
//
//   roth_handhold <right|left|both> <item>          an item, as the original's right hand holds it
//   roth_handhold <right|left|both> weapon <item>   a weapon, as RealmsHeld is held
//   roth_handhold <right|left|both> none            the game's again for that hand
//   roth_handhold off                               the game's for both
//
//===========================================================================

CCMD(roth_handhold)
{
	if (argv.argc() < 2)
	{
		Printf("roth_handhold <right|left|both> <item> | <right|left|both> weapon <item> | <right|left|both> none | off\n");
		return;
	}
	if (!stricmp(argv[1], "off"))
	{
		for (int s = 0; s < VRHandPose::SIDE_COUNT; s++) VRHandTest_SetHeld(s, false, VRHandPose::HeldState());
		Printf("roth_handhold: off\n");
		return;
	}
	bool sides[VRHandPose::SIDE_COUNT] = { false, false };
	if (!stricmp(argv[1], "right")) sides[VRHandPose::SIDE_RIGHT] = true;
	else if (!stricmp(argv[1], "left")) sides[VRHandPose::SIDE_LEFT] = true;
	else if (!stricmp(argv[1], "both")) sides[0] = sides[1] = true;
	else { Printf("roth_handhold: '%s' is not right, left or both\n", argv[1]); return; }
	if (argv.argc() < 3) { Printf("roth_handhold: what does the hand hold?\n"); return; }

	bool on = true, weapon = false;
	int item = 0;
	if (!stricmp(argv[2], "none")) on = false;
	else if (!stricmp(argv[2], "weapon"))
	{
		if (argv.argc() < 4) { Printf("roth_handhold: weapon <item>\n"); return; }
		weapon = true;
		item = atoi(argv[3]);
	}
	else item = atoi(argv[2]);

	FString cls;
	if (weapon) cls.Format("RealmsWeapon%d", item);
	const VRHandPose::HeldState held = on ? roth::HeldFor(item, weapon, weapon ? cls.GetChars() : nullptr) : VRHandPose::HeldState();
	for (int s = 0; s < VRHandPose::SIDE_COUNT; s++)
		if (sides[s]) VRHandTest_SetHeld(s, on, held);
	if (!on) { Printf("roth_handhold: %s -- the game's held thing again\n", argv[1]); return; }
	const roth::ItemGrip *g = roth::ItemGripFor(item);
	FString what;
	if (g == nullptr) what = "no design in roth_handhold.h: the controller's pose";
	else if (weapon) what = held.gripClass.empty() ? "gun_grip" : FStringf("grip_%s", held.gripClass.c_str());
	else if (!held.pose.empty()) what = held.pose.c_str();
	else what.Format("subject %s", VRHandPose::GripSubjectName(held.subject));
	Printf("roth_handhold: %s holds %s %d (%s): %s\n", argv[1], weapon ? "weapon" : "item", item, g != nullptr ? g->name : "?", what.GetChars());
}
