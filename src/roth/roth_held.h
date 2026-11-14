#pragma once
//
// roth_held.h -- THE HELD OBJECTS IN THE ENGINE (chapter 1, the owner's answers of 2026-11-13 evening,
// docs/VR_CHAPTER01_SHADOWS.md 6): the match struck on the Matchbox, the Lamp on its hip hook, the item the original
// puts in the right hand shown in the item hand, the Book opened in both hands with the key into the palm, the niche's
// flash as a light, and the forced turn's view snap during black. The rules are roth_heldlogic.h (engine-free, tested
// by tools/held_test); this is where they meet the hands, the inventory, the popup, the lights and the movies.
//
// WHAT RUNS WHERE
//   HeldBeginLevel   roth_runtime.cpp BeginLevel: the light groups followed (the niche) and their pack values.
//   HeldTick         TickLevelLogic, every tic, after the hands' picker and the candles (`worldTicks` false while the
//                    inventory, a menu or the death wait holds the world: the match's 20 s stand still then, as the
//                    candles' catch does). Reads the hands, the original's right-hand item and inventory, its popup
//                    and its light bytes; draws billboards; asks nothing of the game.
//   HeldMatchFlame   roth_candles.cpp CandlesTick: where the lit match's flame is. A flame at an unlit wick asks for
//                    the original's use FROM THE ITEM HAND (the right-hand item, the Matchbox, is what the chain tests),
//                    through the VR use path (VRPickUseObject) -- the one thing any of this asks of the game.
//   HeldGripTaken    roth_vrpick.cpp TickHand: a grip closing at the lamp is the lamp's; the picker leaves it alone.
//   HeldHandCarries  roth_gestures.cpp: a hand with the lamp, the match or the Book neither knocks nor punches.
//   HeldPose         roth_hands.cpp PublishHeld: the pinch for the match, the curl for the lamp, both hands round the
//                    Book (roth_handhold.h).
//   HeldFrameLights  the renderer, once a frame beside the candles' (hw_entrypoint.cpp): the match, the lamp, the niche.
//   HeldForcedTurn   the 0x3f handler (roth_runtime.cpp SetPlayerAngle), after the original's angle is set.
//   HeldMovieOver    roth_movie.cpp, when the movies end (the screen still black).
// GAME STATE: none is written. Flat (no headset, no flat test) nothing draws and nothing is asked.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "vectors.h"

struct FLevelLocals;
class AActor;
namespace VRHandPose { struct HeldState; }

namespace roth
{

struct Map;

void HeldBeginLevel(const Map &map, FLevelLocals *level);
void HeldEndLevel();
void HeldTick(FLevelLocals *level, bool worldTicks);
void HeldFrameLights(FLevelLocals *level, double ticFrac);
// The lit match's flame, in map units; false while no match burns.
bool HeldMatchFlame(DVector3 &at);
bool HeldGripTaken(int role);
// Whether the hand of `role` carries a held object this tic -- the Lamp, the lit match, the open Book -- so it is
// neither an empty hand (the knock) nor a fist (the punch) for the gestures (roth_gestures.cpp).
bool HeldHandCarries(int role);
void HeldPose(int role, VRHandPose::HeldState &held);
void HeldForcedTurn(double fromYawDeg, double toYawDeg);
void HeldMovieOver();

} // namespace roth
