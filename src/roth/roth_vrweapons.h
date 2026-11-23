#pragma once
//
// roth_vrweapons.h -- CHAPTER 2'S WEAPONS IN VR, IN THE ENGINE (the owner's Q6-Q10, docs/VR_CHAPTER02_SIGNS_AND_PORTENTS.md
// section 6). The decisions and the geometry are roth_vrweapon.h (engine-free, tools/vrweapon_test); this is where they
// read the hands and the inventory, and what each one does -- always one of the original's own calls:
//
//   THE SWORD (Q6)     swung, never triggered: the fire button is held back while the main hand holds it
//                      (VRWeaponsTriggerHeldBack, read by roth_vrpick.cpp); the swing and its contact along the blade are
//                      the gestures' (roth_gestures.cpp, the fists' reader on the blade), which fire the original's attack
//                      at the contact (ActorsRequestFireAt: trigger_weapon_fire with the Sword, bullet i164, its rate).
//   THE SHIELD (Q7)    worn on the off forearm whenever it is carried (VR only): the original's in-hand test counts it
//                      (Inventory::SetWorn) -- the Shield's only effect in the game (ANUBIS #11, #49, #83); its card is
//                      drawn there (roth_held.cpp, VRWeaponsShieldWorn).
//   BODY SLOTS (Q8)    the main hand's gun holsters (roth_vrweapon.h HolsterRig, RSQ Tactical Rig's places and press
//                      rules): a draw is the inventory's weapon click, a weapon put away is activate_weapon_item(0, 0).
//   THE SHOTGUN (Q9)   the off hand on the fore-end (SupportLatch, the engine's own support hold) aims it: the shot leaves
//                      along the rear hand -> fore hand line (VRWeaponsTwoHandAim, read by WeaponTick); while it holds,
//                      the off hand's grip is the gun's (VRWeaponsGripTaken: no use, no punch -- the off hand is
//                      SUPPORTING, roth_handstate.h).
//   THE STAFF (Q10)    its trigger is the original's projectile along the staff (row 33); it is ALSO swung (owner
//                      2026-11-16): the same reader on its length, the blow Adam's fists' attack borrowed (the original's
//                      Staff has no melee; roth_actors_engine.cpp ActorsRequestBorrowedBlowAt).
// Flat (no headset, no controller-pose test): nothing here acts, and the in-hand test is the original's.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "vectors.h"

struct FLevelLocals;

namespace roth
{

// Once a tic from VRPickTick, after the gestures. `live`: the hands work the world this tic (no menu, panel, dialogue
// or death). The worn Shield is kept every tic whatever `live` is.
void VRWeaponsTick(FLevelLocals *level, bool live);

// The weapon in the main hand (the original's weapon slot, 0x81038), 0 for his fists or none.
int VRWeaponsHeldItem();

// The main hand holds a weapon that is swung and never triggered (the Sword): the fire button does not reach it.
bool VRWeaponsTriggerHeldBack();

// The grip of `role` belongs to a weapon this tic: the off hand taking or holding the Shotgun's fore-end, the main hand
// inside a body slot. The picker's grip and the gestures' fist stand down for it.
bool VRWeaponsGripTaken(int role);

// The off hand holds the two-handed weapon's support (the fore-end latch engaged): L2's SUPPORTING (roth_handstate.h).
bool VRWeaponsSupportHeld();

// A WEAPON PICKED UP WHILE THE MAIN HAND IS NOT FREE (docs/VR_INTERACTION.md 1.5): the original has equipped it
// (give_item -> activate_weapon_item); it goes to its body slot (its own, else the first free one it may enter) and
// the main hand keeps what it held: `before` re-equipped by the inventory's weapon click, or nothing. True: a slot.
bool VRWeaponsToBodySlot(int item, int before);

// The two-hand aim of a two-handed weapon while the off hand holds it: the line from the rear (main) hand through the
// fore (off) hand -- yaw in the engine's degrees, pitch POSITIVE UP -- and the rear hand (AttackPos). False: one hand.
bool VRWeaponsTwoHandAim(double &yawDeg, double &pitchUpDeg, DVector3 &rear);

// The item worn on the off forearm this tic (the Shield), for its card. False: nothing worn.
bool VRWeaponsShieldWorn(int &item);

} // namespace roth
