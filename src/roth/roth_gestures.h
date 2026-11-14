#pragma once
//
// roth_gestures.h -- THE CHAPTER-1 HAND GESTURES the owner approved (docs/VR_CHAPTER01_SHADOWS.md 6), in the
// engine. The detectors are roth_gesture.h (engine-free, tools/gesture_test); this is where they read the VR
// hands (roth_vrpick.h VRPickHandNow) and what each one does. EVERY ONE ENDS IN ONE OF THE ORIGINAL'S CLICKS
// through the picker's own paths, or in a haptic only:
//
//   THE PUNCH (Q6)        a closed fist that holds no weapon, swung at speed (RSQ Melee's reader) into
//                         something: the ORIGINAL's secondary click on what it met (roth_handclick.h: the
//                         cursor's type there) -- an attack target (a creature, breakable glass) gets the
//                         original's attack aimed at the contact (ActorsRequestFireAt: the weapon tick's
//                         trigger_weapon_fire, Adam's fists, the original's damage); a usable thing (a switch,
//                         a door) the original's use (VRPickUseAt). Speed only decides whether it counts.
//   THE KNOCK (Q3)        two light raps of an open, empty hand on one surface or object: the original's
//                         examine of it (VRPickExamineAt: the same path as the trigger's), a light haptic.
//   THE ATTEMPTS (Q4)     where the original has only an examine line for something a hand can try: the try
//                         (push in, tug, press, wave) plays that examine (VRPickExamineAt) with a haptic and no
//                         new outcome. Zones from the original's own objects, art and sectors (the table in
//                         roth_gestures.cpp: chapter 1's cases), and the palm sweeping a door leaf.
//   THE WARD TINGLE (Q9)  a palm nearing a glowing ward (the pentagrams DEMO[4207] / DEMO[4208], the sigil
//                         ADEMO[232]): a faint haptic that grows. Haptic only.
//   THE TALL WICKS (Q13)  the picker's grip (roth_vrpick.cpp) -> CandlesPointGrip (roth_candles.h).
//
// The flat test: roth_gesturetest (the test hands MOVE: a scripted motion of a test hand's place), with
// roth_handtest / roth_vrpicktest; see the command at the bottom of roth_gestures.cpp.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

struct FLevelLocals;

namespace roth
{

// Once a tic from VRPickTick, after the hands. `live`: the hands work the world this tic (no menu, panel,
// dialogue or death); otherwise every detector forgets its hand.
void GesturesTick(FLevelLocals *level, bool live);

// The level is going.
void GesturesEndLevel();

// A test instrument (roth_vrpicktest palm uses it): the flat test's HELD REACH, as roth_gesturetest reach places it
// -- the player stood so the test hand's palm (or fingertip, `tip`) lands at (wx, wy, wz) facing (lx, ly, lz), and
// every tic until a move or "roth_gesturetest home" the hand corrected so it stays there wherever the original's
// movement leaves the player. Not a rule of the game.
void GesturesHoldReach(int role, bool tip, double wx, double wy, double wz, double lx, double ly, double lz);

} // namespace roth
