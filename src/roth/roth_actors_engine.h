#pragma once
//
// The engine side of Realms' actors (roth_actors.h holds the rules). Narrow
// hooks, called from roth_runtime.cpp only.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <stdint.h>

struct FLevelLocals;

namespace roth
{

struct Map;
struct Command;
class Log;

// After the props are spawned. `map` is the level logic's own copy, whose
// object lists the rules then edit in place.
void ActorsBeginLevel(Map &map, FLevelLocals *level, Log *log);
void ActorsEndLevel();

// Once per engine tic: `ticks` 70 Hz ticks of the original's frame
// (tick_dynamic_entities runs its pass that many times, EA:1358-1377).
void ActorsTick(FLevelLocals *level, int ticks);

// RAW commands 0x16, 0x3a and 0x3c. (0x33 and 0x35 run in the command core,
// roth_cmdcore; ActorWorld::CmdApplyDamage is the same rule, kept for the host
// test that checked it against the original.) The active object (the runtime's
// "what the player just used") is read and written back, because 0x16 and
// 0x3c set the command source object; `activeFace` is the face the chain was
// fired from, -1 none. Returns -1 (acted) or 0, and sets `interrupt` when the
// chain must stop (0x8a268 = 2).
int ActorsCommand(uint8_t op, Command &rec, int &activeObjectSector, int &activeObjectIndex,
	int activeFace, bool &interrupt);

// Whether 0x16 / 0x3a / 0x3c are served (true while a level is active).
bool ActorsActive();

} // namespace roth
