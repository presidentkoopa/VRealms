#pragma once
//
// roth_gestures.h -- THE CHAPTER-1 HAND GESTURES the owner approved (docs/VR_CHAPTER01_SHADOWS.md 6), in the
// engine. The detectors are roth_gesture.h (engine-free, tools/gesture_test); this is where they read the VR
// hands (roth_vrpick.h VRPickHandNow) and what each one does. EVERY ONE ENDS IN ONE OF THE ORIGINAL'S CLICKS
// through the picker's own paths, or in a haptic only:
//
//   THE PUNCH (Q6)        a closed fist that holds no weapon, swung at speed (RSQ Melee's reader) into an
//                         attack target (a creature, breakable glass): the original's attack aimed at the contact
//                         (ActorsRequestFireAt: Adam's fists, the original's damage). Blows are combat only: using
//                         and examining are the hands' one system (roth_vrpick.cpp, XR Tools' pickup and pointer;
//                         owner 2026-11-24). The knock and the attempts are gone.
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

// A test instrument (roth_worldtest hold, roth_worldhands.cpp): the test is carrying hand `role` to its place this tic
// (the controller pose corrected toward a target); for two tics nothing of its motion counts (no punch),
// as a hand placed by roth_gesturetest. Not a rule of the game.
void GesturesTestPlacing(int role);

} // namespace roth
