#pragma once
//
// roth_candles.h -- THE CANDLES IN THE ENGINE: each wick shown lit as the player lights it, and one light per
// holder that grows with each. The pictures, the state machine and the light's shape are roth_wicks.h
// (engine-free, tested by tools/wicks_test); this is where they meet the actors, the hands and the renderer.
//
// WHAT RUNS WHERE
//   CandlesPrepare     PrepareObjects (roth_objects.cpp), at load: every object whose click chain swaps its
//                      own picture is looked at (FindLightableObjects); a pair that is a candle
//                      (BuildWickModel) gets one composite picture per set of lit wicks, built from the install's
//                      own two pictures (TextureSet::CompositeSprite). Nothing is written anywhere.
//   CandlesBeforeUse / CandlesAfterUse   around every use a VR hand makes (roth_vrpick.cpp DoUse): when that
//                      use is the one that starts a candle's lighting -- its chain's 0x3a re-keyed the candle,
//                      which every retail candle chain does right after its item test (tools/wicks_test) -- the
//                      wick the hand was at (or the one nearest where it pointed) is the first wick lit.
//   CandlesTick        TickLevelLogic, after every chain of the tic: the flames at the wicks (the item hand
//                      holding the item the chain tests, or any fingertip when it tests none). On an unlit
//                      candle a flame coming to a wick fires the original's use from that hand (the VR use
//                      path, VRPickUseObject); on a candle being lit it lights the wick. The picture: while
//                      wicks are still dark, the composite of the lit ones in place of whatever the original
//                      shows; once all are lit (or caught, 15 s) and the original's own art is the lit picture,
//                      the original's picture again. The 15 s count stands still while the world does.
//   CandlesFrameLights the renderer, once a frame before the effect lights take this frame's lights
//                      (hw_entrypoint.cpp): one EffectLights frame light per holder with a wick lit.
//   CandlesBeforeSave  the level snapshot (p_saveg.cpp): every actor wears the original's picture when saved.
//   CandlesRestored    after a restore: nothing per wick survives (presentation); a candle shows the original.
// GAME STATE: none of this writes any. The one thing it asks of the game is the original's use, once per
// candle, through the VR use path (roth_vrpick.cpp DoUse -> Activate); examine, use and the pick box
// (ObjectPickBox) read the original's object and picture.
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

struct Map;
class TextureSet;
class Log;

void CandlesPrepare(const Map &rm, TextureSet &levelArt, TextureSet *sharedArt, Log *log);
void CandlesTick(FLevelLocals *level);
// THE TALL WICKS (the VR grip, roth_vrpick.cpp): the flame hand `role` pointing from `tip` along `dir` grips. The
// wick nearest the ray, in the original's use band, lights by the same route as a flame brought to it. True when
// a wick was pointed at (the grip is taken).
bool CandlesPointGrip(int role, const DVector3 &tip, const DVector3 &dir);
void CandlesBeforeUse(AActor *target);
// `tip`, `palm`: the hand's points; `hit`: where the use pointed; `inReach`: the palm was on the candle.
void CandlesAfterUse(AActor *target, int role, const DVector3 &tip, const DVector3 &palm, const DVector3 &hit, bool inReach, bool acted);
// The sprite the original has the actor wear: `shown` unless a composite stands in for it.
int CandleOriginalSprite(const AActor *mo, int shown);
void CandlesBeforeSave(FLevelLocals *level);
void CandlesRestored(FLevelLocals *level);
void CandlesFrameLights(FLevelLocals *level, double ticFrac, double viewYawDeg);

// For the flat test (roth_vrpicktest wick / look): wick `wick` (0-based, left to right as drawn) of the candle
// object (sector, index), in the world as the view `viewYawDeg` sees it; its middle height and its height.
bool CandleWickWorld(int sector, int index, int wick, double viewYawDeg, DVector3 &out);
bool CandleExtent(int sector, int index, DVector3 &centre, double &height, int &wicks);

} // namespace roth
