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

namespace roth
{

// Once per engine tic, at the top of TickLevelLogic (so a Realms menu or the
// death wait does not take the hands away).
void HandsTick(FLevelLocals *level);

// The level's actors are going: forget them (EndLevel).
void HandsEndLevel();

} // namespace roth
