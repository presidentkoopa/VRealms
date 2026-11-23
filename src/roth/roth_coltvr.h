#pragma once
//
// roth_coltvr.h -- THE COLT AS A REAL M1911 IN VR, IN THE ENGINE. The rules and the reload's state machine are
// roth_colt.h (engine-free, tools/colt_test); this reads the hands, draws the mesh in the main hand at the fit measured
// on the authored grip (tools/colt_test/colt_fit), poses its parts from the mesh's own take on the original's fire
// events, throws the spent case and the dropped magazine into the world, and hands the weapon tick its two answers:
// may this pull reach the original's trigger, and is the original's reload due (the new magazine seated).
// Flat play (no headset, no controller-pose test): nothing here acts; the Colt is the original's picture.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_colt.h"
#include "vectors.h"

struct FLevelLocals;
class AActor;
class PClassActor;
namespace VRHandPose { struct HeldState; }	// r_data/vr_handpose_math.h

namespace roth
{

// Once a tic from VRWeaponsTick (after the gestures): the hands measured against the drawn gun. `live`: the hands work
// the world this tic.
void ColtHandsTick(FLevelLocals *level, bool live);

// The weapon tick (roth_actors_engine.cpp WeaponTick), every tic the Colt is the weapon:
//   ColtPullReaches  R5/R6: may the fire button's pull reach trigger_weapon_fire (false: a dry click, logged)
//   ColtReloadDue    R4: the new magazine seated -- pull the original's trigger once now (its reload, WC:843-866)
//   ColtReloadDone   what that pull did (the word and the i51 count before and after), for the log and the check
//   ColtAfterWeapon  the original's events of this tic into the rig, then the hands' step
bool ColtPullReaches(const colt::Game &g);
bool ColtReloadDue(const colt::Game &g);
void ColtReloadDone(int qtyBefore, int magsBefore, int qtyAfter, int magsAfter, int lockAfter);
void ColtAfterWeapon(const colt::Game &g);

// PresentHeld: the class to draw the Colt with in VR (null: no mesh -- the original's card), and its pose this tic.
PClassActor *ColtMeshClass();
void ColtPresent(AActor *gun, const colt::Game &g);

// The shot along the DRAWN barrel: the muzzle and the bore's yaw (engine degrees) and pitch (positive up). False: no
// mesh drawn this tic (the aim pose, as before).
bool ColtBoreAim(double &yawDeg, double &pitchUpDeg, DVector3 &muzzle);

// The main hand's fingers measured on the mesh while it is drawn (tools/colt_test/colt_fit): the trigger finger
// (vr_handpose_math.h HeldState::indexReady / indexFire / indexBase) and the other fingers' wraps (wrap / wrapBase),
// written into `held`. False (nothing written): no mesh drawn.
bool ColtHeldFingers(int side, VRHandPose::HeldState &held);

// The off hand's grip belongs to the Colt this tic (at the release, the pouch, the slide, or carrying a magazine): L2's
// standing Colt state and its claim (roth_handstate.h Facts.coltAction, Claims::Colt).
bool ColtGripTaken(int role);

// R9, TWO HANDS (roth_colt.h; tools/colt_test/support_fit): the off hand at the main hand's grip, where the support fit
// put it, takes the gun with its grip; while it holds it is drawn there (VRHandPose_SetHeldBy) and L2 counts it
// SUPPORTING (roth_vrweapons.cpp VRWeaponsSupportHeld / VRWeaponsGripTaken ask these). It lets go when it leaves the
// place, when its grip opens, and at once on a reload action (the catch, the pouch, the slide). The aim stays the
// shooting hand's bore (ColtBoreAim).
//   ColtSupportHeld       the off hand supports the Colt this tic
//   ColtSupportWouldTake  a grip closing now would take it (the hand at the place, the gun free to be taken)
//   ColtSupportFingers    the off hand's fingers while it supports (GRIPSUBJ_Support with the fit's amounts); false: not
bool ColtSupportHeld();
bool ColtSupportWouldTake();
bool ColtSupportFingers(VRHandPose::HeldState &held);

// R8: the shooting hand's lower face button is the Colt's (the catch, the slide stop) while the Colt is drawn in the VR
// hands and the button is down: the +use key it also posts (KEY_PAD_A / KEY_PAD_X) must not use the world too
// (p_map.cpp P_UseLines asks).
bool ColtTakesUseKey();

// The drawn gun went away (put away, hidden, another weapon): its seat and its part poses with it.
void ColtGunGone();

void ColtEndLevel();

} // namespace roth
