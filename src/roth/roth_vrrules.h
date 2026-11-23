#pragma once
//
// roth_vrrules.h -- L1, THE RULES ENTRY OF THE VR INTERACTION (docs/VR_INTERACTION.md section 4, L1). Engine-free.
//
// Every VR interaction ends in one of four calls, and nothing else in the game gives an item, opens a door or fires
// a chain from a VR hand:
//
//   Pick(origin, dir)        -> the record: what the ray meets first        roth_vrpick.h RulesPick
//   Classify(record, body)   -> Nothing / Usable / TooFar / Blocked          roth_vrpick.h RulesClassify
//   Use(record, heldItem)    -> activate_targeted_object                     roth_vrpick.h RulesUse
//   Examine(record)          -> examine_object_under_cursor                  roth_vrpick.h RulesExamine
//
// This header is the part that needs no engine: the original's reach bands, the reach of D2, the four classes, and the
// record's fields. The classification itself is classify_cursor_target_object's port (roth_cursor.h, compared with
// the original's own text by tools/vrpick_test); the matchers it is fed are roth_runtime.cpp's (ClassifyPick).
//
// D2 (docs/VR_INTERACTION.md 3): THE REACH IS THE HORIZONTAL DISTANCE FROM THE PLAYER'S POSITION TO THE HIT POINT.
// The original measures z along the view yaw (depth = 2z + 64, REMAROTH_MEASURED.md 13), which for a hand would give
// endless reach to the side. Straight ahead the two are the same number, so the bands stay the original's: use
// z < 268 (depth < 600), "too far" z < 968 (depth < 2000), examine z < 2468 (depth < 5000). Measured from the body
// (decision 2), never from the hand.
//
// PORT CHECKLIST (the porting rule, CLAUDE.md): ROTH.C (E/ = roth_c/src/engine) -> the port.
//  1. the pick record +1 type, +8 face, +0xa sector, +0xe object, +0x1c door, +0x20 depth  E/renderer.c:8962-9090,
//     3283, 4510, 13007 (GAME_core.md 4.4) -> Record (below); filled by RulesPick / RulesRecordOf (roth_vrpick.cpp)
//     from the engine's trace (roth_runtime.cpp PickFrom, ResolvePick).
//  2. the first opaque texel under the cursor  E/renderer.c:3261-3283, 4480-4500 -> PickFrom: the first Realms surface
//     or object box the ray meets. A sprite's box, not its opaque texels (decision 7's model shape is P3): the
//     differential (tools/userange_test l1diff) counts where that differs.
//  3. depth = 2z + 64, z along the view yaw  (measured)  -> DepthOfZ(ReachZ(...)): D2, z the horizontal distance.
//  4. the bands 0x258 / 0x7d0 / 0x1388  E/input.c:1031, 1123-1217, 1145 -> kUseDepth / kFarDepth / kExamineDepth.
//  5. classify_cursor_target_object  E/input.c:1111-1245 -> ClassifyCursorFacts (roth_cursor.h), unchanged.
//  6. run_gameplay_frame's click gates  E/game_core.c:604-699 -> CursorAllowsUse / CursorAllowsExamine (roth_cursor.h),
//     CursorClass::CanUse / CanExamine (roth_runtime.h), unchanged.
//  7. the cursor pictures: usable 0x248 0x250 0x278, too far 0x398 0x3a0, nothing 0x240, blocked / keyed 0x258 0x260
//     E/input.c:1111-1258 -> ClassOf (below).
//  8. activate_targeted_object  E/input.c:1026-1100 -> roth_runtime.cpp Activate, behind RulesUse, unchanged.
//  9. the item the use carries: g_selected_item_primary, read by RAW 0x27  E/raw_commands.c:4760-4789 -> RulesUse
//     selects the hand's item for the use (roth_handuse.h HandUseBegin / HandUseEnd), unchanged rules.
// 10. examine_object_under_cursor  E/input.c:979-1024 -> roth_runtime.cpp ExaminePick, behind RulesExamine, unchanged.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_cursor.h"
#include <math.h>
#include <stdint.h>

namespace roth
{
namespace rules
{

// The original's depth bands on the pick record's +0x20 (E/input.c:1031, 1123-1217, 1145), and the planar distances
// they are (depth = 2z + 64; REMAROTH_MEASURED.md 13).
constexpr double kUseDepth = 0x258, kFarDepth = 0x7d0, kExamineDepth = 0x1388;
constexpr double kUseZ = (kUseDepth - 64.0) / 2.0;          // 268
constexpr double kFarZ = (kFarDepth - 64.0) / 2.0;          // 968
constexpr double kExamineZ = (kExamineDepth - 64.0) / 2.0;  // 2468

inline double DepthOfZ(double z) { return 2.0 * z + 64.0; }

// D2: the horizontal distance from the player's position (px, py) to the hit point (hx, hy).
inline double ReachZ(double px, double py, double hx, double hy)
{
	const double dx = hx - px, dy = hy - py;
	return sqrt(dx * dx + dy * dy);
}

// The record's depth word for a hit at (hx, hy) with the player at (px, py), by D2.
inline double BodyDepth(double px, double py, double hx, double hy) { return DepthOfZ(ReachZ(px, py, hx, hy)); }

// THE FOUR CLASSES the VR hands show (docs/VR_INTERACTION.md L1), from the original's own cursor:
//   Usable   a use acts on it now: type 3 within the use band (run_gameplay_frame's gate, activate_targeted_object's)
//   Blocked  type 5: blocked or keyed (0x258, 0x260)
//   TooFar   the original's "too far" picture (0x398, 0x3a0), or a type 3 the use band does not reach
//   Nothing  everything else (0x240, and type 1)
enum class Class : uint8_t { Nothing, Usable, TooFar, Blocked };

inline Class ClassOf(uint8_t type, uint16_t shape, double depth)
{
	if (type == 3 && depth < kUseDepth) return Class::Usable;
	if (type == 5) return Class::Blocked;
	if (shape == 0x398 || shape == 0x3a0) return Class::TooFar;
	if (type == 3) return Class::TooFar;
	return Class::Nothing;
}

inline const char *ClassName(Class c)
{
	switch (c)
	{
	case Class::Usable:  return "usable";
	case Class::TooFar:  return "too far";
	case Class::Blocked: return "blocked";
	default:             return "nothing";
	}
}

// THE RECORD (the pick record at 0x90a48, GAME_core.md 4.4), in the port's indices: the Realms face and sector
// (indices into the .RAW's lists; the original's +8 / +0xa are their file offsets), the object (its sector and index
// in the .RAW's object lists; the original's +0xe is its def pointer), the door's sector. -1 where the type has none.
struct Record
{
	uint8_t type = 0;       // 1 ceiling, 2 floor, 3 wall face, 4 object, 6 door, 7 / 8 platform under / top; 0 nothing
	int face = -1;
	int sector = -1;
	int objSector = -1, objIndex = -1;
	int door = -1;          // the door's sector
	double x = 0, y = 0, z = 0;   // the hit point
};

} // namespace rules
} // namespace roth
