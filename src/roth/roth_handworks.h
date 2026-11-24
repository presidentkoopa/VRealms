#pragma once
//
// roth_handworks.h -- THINGS WORKED BY HAND, IN THE ENGINE (docs/VR_HANDWORKS.md, milestone 2 of docs/VR_PHYSICS.md).
//
// A hand that closes on a thing with a track moves it along that track, and the thing fires the ORIGINAL's use when
// it passes the threshold. The track maths is roth_track.h (engine-free, ported from VRExpansionPlugin); this is where
// it meets the level.
//
//   DOORS (every swinging door of the level, the 167 room doors and the 8 cupboard and wardrobe leaves alike: the
//   original's doors are one kind, docs/WORLD_DESIGN.md 1.1, docs/OPENABLES.md 2). A free hand closing on a door's
//   latch half takes hold of it (a Hinge about the door's own hinge, the sense SwingDoor gives it from the player's
//   room). CLOSED: the leaf is latched; the hand drawing it 4 cm along its swing (docs/WORLD_DESIGN.md 1.3, the
//   owner's door gesture) is the original's use on the door's face, once per grip: the original's chain decides --
//   a door that opens is then the hand's to swing (its own swing waits, po_man.h EV_HoldPoly), a locked one says
//   "Locked." and stays shut (the double buzz). OPEN (swinging, or standing open): the leaf follows the hand between
//   shut and its 90 degrees; let go, it carries the hand's swing on (VRLever's RetainMomentum) and the original's own
//   swing then finishes its arc from where it is. The hand is drawn on the leaf where it took hold.
//
// Levers and keys are the world hands' (roth_worldhands.cpp): the levers of the owner's table on a Hinge, a key turned
// in any lock whose use tests it, the hand pinned to each while it works it.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//
struct FLevelLocals;
class AActor;

namespace roth
{

// Once a tic from WorldHandsTick. `live`: the hands work the world this tic.
void HandworksTick(FLevelLocals *level, AActor *pmo, bool live);
void HandworksEndLevel();
// A grip closing (WorldHandsGripTaken, first): true when the hand took hold of a door.
bool HandworksGripTaken(int role, AActor *pmo);
// The hand of `role` is on a door.
bool HandworksHolding(int role);

} // namespace roth
