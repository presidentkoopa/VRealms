#pragma once
//
// roth_handclick.h -- WHAT THE ORIGINAL'S SECONDARY (USE) CLICK DOES ON THE
// CURSOR'S TYPE: use, attack, or nothing. Engine-free, so tools/handclick_test
// checks it. The VR hands' grip is that click (roth_vrpick.cpp; the owner's
// scheme: the grip is the secondary, the trigger the primary).
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
//     [0x7f568] = 1, the mouse's aim (:673-685) -> SECONDARY_ATTACK_AIMED.
//  4. type 1 with the PRIMARY held (buttons_prev bit 0) is "steering" (:700-727):
//     the steer clock [0x7fdcc] counts the frame time from the first held frame
//     (:702-708; reset when steering starts); before 0x0c ticks a secondary held
//     makes it type 2, the look mode (:710-716: nothing fires); from 0x0c on, a
//     secondary EDGE fires trigger_weapon_fire(0, 0) once, the latch [0x7fdc8]
//     set until a frame with no secondary edge (:717-727) -> SECONDARY_ATTACK.
//  5. type 1 without the primary held: nothing (:701) -> SECONDARY_NOTHING.
//  6. the dialogue: a click while it owns the pointer (busy and the menu gate, or
//     a queued action) force-ends the line first (:597-632) -> NOT HERE: the
//     hands' clicks then go to the dialogue (InterfaceHandDialogueClick).
//  7. type 2 (the look mode, :728-731): nothing for a click -> SECONDARY_NOTHING.
// One call is one secondary EDGE (the grip's press); its once-per-press latch
// is the edge itself.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <stdint.h>

namespace roth
{

enum SecondaryVerb { SECONDARY_NOTHING, SECONDARY_USE, SECONDARY_ATTACK_AIMED, SECONDARY_ATTACK };

// `cursorType` the classified type (1 nothing, 3 usable, 5 blocked / attack
// target); `primaryHeld` the primary button down, for `primaryTicks` of the
// original's 70 Hz frame ticks so far (the steer clock).
inline SecondaryVerb SecondaryClick(int cursorType, bool primaryHeld, int primaryTicks)
{
	switch (cursorType)
	{
	case 3: case 4: return SECONDARY_USE;                                   // 1, 2
	case 5: return SECONDARY_ATTACK_AIMED;                                  // 3
	case 1:
		if (!primaryHeld) return SECONDARY_NOTHING;                         // 5
		return primaryTicks >= 0x0c ? SECONDARY_ATTACK : SECONDARY_NOTHING; // 4 (before 0x0c: the look mode)
	default: return SECONDARY_NOTHING;                                      // 7
	}
}

inline const char *SecondaryVerbName(SecondaryVerb v)
{
	switch (v)
	{
	case SECONDARY_USE:          return "use";
	case SECONDARY_ATTACK_AIMED: return "attack (at the target)";
	case SECONDARY_ATTACK:       return "attack (the primary held: steering)";
	default:                     return "nothing";
	}
}

} // namespace roth
