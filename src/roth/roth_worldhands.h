#pragma once
//
// roth_worldhands.h -- CHAPTER 2'S WORLD BY HAND, IN THE ENGINE (the owner's answers of 2026-11-16,
// docs/VR_CHAPTER02_SIGNS_AND_PORTENTS.md 6). The rules are roth_worldhandslogic.h (engine-free, tools/worldhands_test);
// this is where they read the VR hands and the head, and which of the ORIGINAL's clicks each one ends in -- always
// one, through the hands' own use path (roth_vrpick.h VRPickUseAt: the cursor gate, the item of the hand that makes
// it), or the original's use on self by its own inventory clicks (InterfaceUseRightHandOnSelf). No new game logic.
//
//   THE RELEASE (Q1, Q14)   the item hand holding a Statuette (i4) or the Orb (i18) opens its grip with the item's
//                           contact point (the statuette's base: the bottom of the card drawn in the hand; the Orb's
//                           middle) within 5 cm of a floor, a platform top or a wall face: the original's use there
//                           (the record its dispatcher matches at that texel, box included; the base's disc, Q21).
//                           The dais plates #119, #124-#129 (STUDY1), the Orb door's aperture #4 (MAS3).
//   THE PULL (Q17, Q30, Q24) a grip closing on a lever object or on a face whose use is one of the table's records
//                           (kPulls) takes the grip (no use on the close); the hand then travels 8 cm (levers, the
//                           chains) or 4 cm (a bookcase leaf, from within 15 cm of its edge; the clock's case
//                           door) in any direction but into it: that use, once per grip.
//   THE TWIST (Q15, Q16)    the item hand holding the Shrive (i14) or Key i22 with the grip closed and the item within
//                           5 cm of something its use matches (a face's record, an object's click): set; the hand
//                           turned 180 (the Shrive) / 30 (the key) degrees about its own axis, either way: the use
//                           there, from the item hand. A grip closing in the socket is taken (no use on the close).
//   THE PRESS (Q18)         an open palm pushed 2 cm past the face of one of MAS6's hand prints (#42, #8, #41),
//                           over it: its click, once per push.
//   THE SIT (Q19)           the head 35 cm or more below standing, within 45 cm of the throne's seat (MAS7 sector id 8)
//                           in the plane, facing away from its back: the original's use on the seat (#7 / #33).
//   THE DRINK (Q26)         a Vial in the item hand within 10 cm of the mouth (8 cm under the eyes), tipped 60 degrees
//                           or more for 6 tics: the original's use on self.
//   THE CONTACT DISC (Q21)  WorldHandsContactDisc: where a contact's point matches no record, the first point of its
//                           disc (the fingertip's 1 cm, the palm's 4 cm) that does -- the click there (roth_vrpick.cpp).
//
// The flat test: roth_worldtest (below, at the bottom of roth_worldhands.cpp), on roth_xrpose's real controller path.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

struct FLevelLocals;

namespace roth
{

struct Pick;

// Once a tic from VRPickTick, after the gestures. `live`: the hands work the world this tic.
void WorldHandsTick(FLevelLocals *level, bool live);
void WorldHandsEndLevel();

// roth_vrpick.cpp TickHand, on a grip's closing edge, before its own use: true when the grip went to a pull or a
// twist (it set one up), so the picker makes no use of it.
bool WorldHandsGripTaken(int role);

// roth_held.cpp: the item hand's vial is being drunk (at the mouth and leaning): the eyes' examine waits.
bool WorldHandsItemTipping(int role);

// Q21: `p` a wall face, door leaf or flat met by a contact (a fingertip, a palm, an item's base). The first point
// of the disc of `radiusM` metres round its hit, on its surface, centre first, whose USE (`use`) or EXAMINE record
// the original's dispatcher matches, as `out`. False: none in the disc (or not a surface).
bool WorldHandsContactDisc(const Pick &p, double radiusM, bool use, Pick &out);

} // namespace roth
