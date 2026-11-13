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
// The weapon globals the HUD reads (roth_huddraw's HudWeapon): the held
// item's quantity 0x83e70, the clips 0x83e6c, the cap 0x83e74, the panel
// number (attrs + 0x48), and the equip counter 0x810c4.
void ActorsHudWeapon(int32_t &item, int32_t &ammo, int32_t &ammoCap, int32_t &panel, int &equips);
// render_weapon_hud's write-back of a panel number above 0xf (WC:1015-1016).
void ActorsSetHudPanel(int32_t panel);
// [0x811e4]: the active weapon's ammunition item, as the 32-bit global the
// inventory compares a sign-extended id with (inventory.c:263-264, 438-439).
int32_t ActorsActiveAmmoId();
// g_active_weapon_ammo (0x83e6c) += delta (inventory.c:266, 441).
void ActorsAddWeaponAmmo(int delta);
// Is the held weapon's actor (RealmsHeld / RealmsWeapon<index>) drawn at the VR main hand this tic
// (FollowHandMode 1, not hidden)? Then `item` is its DBASE100 index and `cls` its class name. The VR hands'
// weapon grip reads it (roth_hands.cpp; UZDXREMA VRRig_HandHoldsWeapon: "a weapon DRAWN in that hand").
bool ActorsHeldWeaponDrawn(int32_t &item, const char *&cls);
// The VR hands and the attack (roth_vrpick.cpp): whether the fire button (BT_ATTACK) reaches
// the weapon, and the hands' own attack -- one trigger_weapon_fire at the next weapon tick.
void ActorsSetFireButtonAllowed(bool allowed);
void ActorsRequestFire();
// How many 70 Hz ticks the actors have run (a log line: none while the inventory pauses the world).
int ActorsTickCount();

} // namespace roth
