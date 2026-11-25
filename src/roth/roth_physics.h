#pragma once
//
// roth_physics -- physical hands and loose items in the level (docs/VR_PHYSICS.md), the engine side of
// roth_physics_core (Jolt). The level's sectors are static triangles, rebuilt where a sector moves; each tracked hand is
// a body that chases its controller and stops at what is solid, the drawn hand with it (r_data/vr_handpose.h
// VRHandPose_SetBlocked); what a hand holds is part of its body; an item let go of anywhere but over the shoulder falls,
// tumbles and rests where it lands (the owner, 2026-11-24: "stays where it lands"), and a free hand that squeezes near
// it takes it again, where it touched it.
//
// THE GAME UNDERNEATH: an item let go of is put away by the original's own calls, as a stow (carried, not selected); a
// loose item taken is selected by the original's own calls (the cell onto the cursor, the cursor into the right hand).
// The original never knows where a loose item lies; walk away from it (or leave the level) and it is back in the pack,
// where the game has kept it all along.
//
// cvar roth_physics (default on, not archived): off, the hands are where the controllers are and a let go stows.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//
#include "vectors.h"
struct FLevelLocals;
class AActor;

namespace roth
{

// Once a game tic, before the hands' picker (roth_runtime.cpp). `world`: the world runs this tic (false while a menu,
// the inventory or the death wait holds it: the hands are where the controllers are, nothing moves).
void PhysicsTick(FLevelLocals *level, bool world);
void PhysicsEndLevel();
bool PhysicsOn();

// THE LET GO (roth_vrpick.cpp Stow): true when `role`'s hand dropped `item` into the world (it is loose now); false
// when it should be stowed (over the shoulder, physics off, no model, a weapon).
bool PhysicsLetGo(int role, int item);

// THE LOOSE ITEMS FOR THE PICKUP (roth_vrpick.cpp THE PICKUP, XR Tools'): how many lie loose, the item and the body's
// centre (map units) of each, and taking one out of the world into `role`'s hand -- `keepPlace` (in the grab sphere)
// keeps where it was in the hand, else (from range: it flies in) the held model's own seat. Its item id, or 0.
int PhysicsLooseCount();
bool PhysicsLooseAt(int index, int &item, DVector3 &centre);
int PhysicsTakeLooseAt(int role, int index, bool keepPlace);

// A HAND ON A TRACK (roth_handworks.cpp, docs/VR_HANDWORKS.md): while `at` is set, `role`'s hand is held with its grip
// at `at` (map units: the door's handle as the leaf turns, the point on a lever's arc, a key in its lock), turned as its
// controller is, drawn there, meeting nothing; null lets it go back to its controller. Called each tic while held.
// With roth_physics off nothing is drawn differently (the tracks still work).
void PhysicsHandPin(int role, const DVector3 *at);
bool PhysicsHandPinned(int role);

// Tests (roth_xrpose handat loose): the actor drawing loose item `index` (0 = the first), or null.
AActor *PhysicsLooseActor(int index);

} // namespace roth
