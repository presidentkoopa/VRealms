#pragma once
//
// roth_handclick.h -- WHAT THE ORIGINAL'S SECONDARY (USE) CLICK DOES ON THE
// CURSOR'S TYPE: use, attack at the target, or nothing. Engine-free, so
// tools/handclick_test checks it against the original.
//
// WHO ASKS IT (owner, 2026-11-13, VR_CHAPTER01_SHADOWS.md 6 Q6: "physical
// melee ... Physical only: the grip on empty air no longer punches"): THE
// FIST'S CONTACT (roth_gestures.cpp, RSQ Melee's punch reader, roth_gesture.h).
// Where a fist swung at speed lands, the original's cursor on that thing
// (ClassifyPick) is classified and the click Adam's hand makes there is this:
// a creature or breakable glass is an attack target (type 5) and gets the
// original's attack aimed at the contact; a switch, a door, anything usable
// (type 3) gets the original's use. The VR grip is no longer this click on
// nothing: the grip-on-empty-air attack of 8917f41d was removed (its code, not
// a switch around it).
//
// run_gameplay_frame (E/game_core.c:515-732), the frame a button edge is
// pending, with the cursor type the hover classified (classify_cursor_target_object,
// E/input.c:1111-1258; roth_cursor.h): 1 nothing, 3 usable, 5 blocked / keyed /
// an attack target. With the default button swap the secondary is the left
// mouse button (docs/GAME_core.md 4.2).
//
// PORT CHECKLIST (E/ = roth_c/src/engine):
//  1. a secondary edge on type 3 makes it type 4 (:603-606) -> SECONDARY_USE.
//  2. type 4 + secondary -> activate_targeted_object (:686-699) -> SECONDARY_USE:
//     the caller's use path (its own depth gate refuses beyond 600: E/input.c:1031).
//  3. type 5 + secondary (no primary edge) -> trigger_weapon_fire at the cursor,
//     [0x7f568] = 1, the mouse's aim (:673-685) -> SECONDARY_ATTACK_AIMED: the
//     fist's contact is the aim (ActorsRequestFireAt, roth_actors_engine.h).
//  4. type 1 with the PRIMARY held (buttons_prev bit 0) is "steering" (:700-727):
//     after 0x0c ticks of the steer clock a secondary edge fires
//     trigger_weapon_fire(0, 0) -> NOT PORTED (owner, 2026-11-13, physical only:
//     an attack needs a fist that lands on something; a click on nothing --
//     steering or not -- attacks nothing). tools/handclick_test reports these
//     cases apart, as excluded, and compares every other one.
//  5. type 1 without the primary held: nothing (:701) -> SECONDARY_NOTHING.
//  6. the dialogue: a click while it owns the pointer (busy and the menu gate, or
//     a queued action) force-ends the line first (:597-632) -> NOT HERE: the
//     hands' clicks then go to the dialogue (InterfaceHandDialogueClick).
//  7. type 2 (the look mode, :728-731): nothing for a click -> SECONDARY_NOTHING.
// One call is one secondary click: one landed punch.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <stdint.h>

namespace roth
{

enum SecondaryVerb { SECONDARY_NOTHING, SECONDARY_USE, SECONDARY_ATTACK_AIMED };

// `cursorType` the classified type of what the fist met (1 nothing, 3 usable,
// 5 blocked / attack target).
inline SecondaryVerb SecondaryClick(int cursorType)
{
	switch (cursorType)
	{
	case 3: case 4: return SECONDARY_USE;            // 1, 2
	case 5: return SECONDARY_ATTACK_AIMED;           // 3
	default: return SECONDARY_NOTHING;               // 4 (not ported), 5, 7
	}
}

inline const char *SecondaryVerbName(SecondaryVerb v)
{
	switch (v)
	{
	case SECONDARY_USE:          return "use";
	case SECONDARY_ATTACK_AIMED: return "attack (at the target)";
	default:                     return "nothing";
	}
}

} // namespace roth
