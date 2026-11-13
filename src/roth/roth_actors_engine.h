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

// The inventory's weapon hooks (InventorySink::EquipWeapon / UnequipWeapon):
// activate_weapon_item on the actor rules' player weapon. Equip returns the
// ammo item the caller must count in the inventory (0 for none), and the
// count goes back through ActorsSetWeaponAmmo.
int ActorsEquipWeapon(int slot, int itemIndex);
void ActorsSetWeaponAmmo(int count);
// activate_weapon_item(0, 0): no slot; the original parses the item latched at
// 0x81054, Adam's own (restore_active_held_item), which the caller passes.
void ActorsUnequipWeapon(int adamItem);
// rebuild_weapon_inventory_list (WC:488), from the inventory as it stands.
void ActorsRebuildWeaponList();

} // namespace roth
