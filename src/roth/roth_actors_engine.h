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

// The u16 mirror 0x8a0c4 as the last 0x3a left it: the id it minted for a new
// id of 0xffff, else 0 (cmd_change_object_id clears it at entry, RC:481-489).
// The runtime's delay carries it across its wait (roth_runtime.cpp DelayTimer).
uint32_t ActorsLastMintedId();

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
// the weapon, and the hands' own attack -- A FIST THAT LANDED (roth_gestures.cpp): one
// trigger_weapon_fire at the next weapon tick, its shot aimed at the contact (x, y, z map units).
void ActorsSetFireButtonAllowed(bool allowed);
void ActorsRequestFireAt(double x, double y, double z);
// A creature's slot for the object (sector, index): its health (+0x0c) and state (+0x08, 0x20 a corpse). False:
// the object has no creature slot. For a log line (the gestures' punch: the health before and after).
bool ActorsObjectHealth(int rothSector, int rothIndex, int32_t &health, uint8_t &state);
// A CREATURE's body as the original's shots meet it (sweep_move_with_collision's object test, CP:1123-1127; the
// port's roth_actor_collide.cpp): a square of half-side `radius` round its x, y (Chebyshev distance under it), from
// its z up `height` -- the dimension table's words for its picture id. False: not a creature with a slot, or no
// dimensions. The VR fist meets this body, not the picture's pick box (roth_gestures.cpp).
bool ActorsCreatureBody(int rothSector, int rothIndex, int &radius, int &height);
// How many 70 Hz ticks the actors have run (a log line: none while the inventory pauses the world).
int ActorsTickCount();

} // namespace roth
