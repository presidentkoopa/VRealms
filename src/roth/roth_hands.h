#pragma once
//
// THE VR HANDS' ACTORS (RealmsHandRight / RealmsHandLeft, wadsrc zscript
// realmshands.zs). Spawned for the player while a headset is on, and only then;
// their place on the controller and their fingers are the renderer's
// (r_data/vr_handpose.h). Called from roth_runtime.cpp only.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

struct FLevelLocals;
class AActor;

namespace roth
{

// Once per engine tic, at the top of TickLevelLogic (so a Realms menu or the
// death wait does not take the hands away).
void HandsTick(FLevelLocals *level);

// The level's actors are going: forget them (EndLevel).
void HandsEndLevel();

// The hand actor standing for a physical hand (VRHandPose::SIDE_RIGHT / SIDE_LEFT), or null when
// there is none (flat, no controller this tic). The VR picker (roth_vrpick.cpp) reads its fingertip.
AActor *HandActor(int side);

// Whether the hand of `role` (VR_MAINHAND / VR_OFFHAND) holds a weapon: the weapon slot's thing drawn in the
// main hand, Adam's own fists not counted (what PublishHeld poses the hand for). Its trigger fires the weapon
// (the binding's +attack), so the VR picker gives that trigger nothing else to do.
bool HandHoldsWeapon(int role);

} // namespace roth
