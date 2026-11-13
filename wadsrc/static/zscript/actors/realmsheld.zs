//
// WHAT THE PLAYER HOLDS IN REALMS OF THE HAUNTING, as a thing in the world.
//
// The weapon's LOGIC is the original's, ported and proven against it in the
// engine (src/roth/roth_actor_weapon.cpp): the trigger, the shot cycle, rate,
// ammo, reload and recharge, damage. It draws nothing. Every tic it publishes
// what the weapon is doing into the fields below, and this actor -- spawned
// in the world by the engine -- is what gets drawn: at the main hand in VR
// (FollowHandMode 1: the model rides the controller at draw rate), in front
// of the eye in flat.
//
// One class per Realms weapon, named by its DBASE100 index, so a MODELDEF can
// give each its own mesh. Two ways to animate one (README: docs/REALMS_WEAPONS.md):
//
//   1. FRAME FOR FRAME. The sprite is R + the index in three digits (R012 for
//      item 12) and the frame letter is the original's weapon frame (A = 0,
//      B = 1, ...). MODELDEF FrameIndex per letter reproduces the original's
//      timing exactly, because the frame changes when the original's does.
//   2. NAMED STATES. RealmsState says Raise / Ready / Fire / Reload / Lower /
//      Away, RealmsProgress runs 0..1 through it, stretched to the original's
//      length (RealmsStateTics of RealmsStateLength), so a mesh's own
//      animations can be played at the right speed from script.
//
// A weapon with no model is drawn as the original's own picture on a flat card
// at the hand (the engine builds the card); nothing is ever invisible.
//
class RealmsHeld : Actor
{
	// Written by the engine every tic. Read them; do not write them.
	int RealmsItem;          // DBASE100 index of the held thing (0: nothing)
	int RealmsState;         // RHS_* below
	int RealmsFrame;         // the original's weapon picture frame
	int RealmsFrameCount;    // frames in that picture
	int RealmsStateTics;     // Realms ticks (70 a second) since the state began
	int RealmsStateLength;   // the state's length in the original, Realms ticks (0: open-ended)
	double RealmsProgress;   // 0..1 through the state
	int RealmsCharge, RealmsChargeFull;   // a recharging weapon's charge (-1: not one)
	// Level time (Level.maptime) of the last time each thing happened, -1 never.
	int RealmsPullTic, RealmsShotTic, RealmsAmmoTic, RealmsReloadTic, RealmsNoAmmoTic, RealmsEquipTic, RealmsCycleEndTic;

	enum ERealmsHeldState
	{
		RHS_AWAY = 0,     // put away, not drawn (the original hides it after 20 s idle)
		RHS_RAISE,        // coming into view (25 Realms ticks)
		RHS_READY,        // held, idle
		RHS_FIRE,         // the shot cycle; the shot leaves on the record's frame
		RHS_RELOAD,       // a cycle drawn with the reload picture (ammo item used)
		RHS_LOWER         // going out of view (25 Realms ticks)
	};

	clearscope string RealmsStateName() const
	{
		switch (RealmsState)
		{
		case RHS_RAISE:  return "Raise";
		case RHS_READY:  return "Ready";
		case RHS_FIRE:   return "Fire";
		case RHS_RELOAD: return "Reload";
		case RHS_LOWER:  return "Lower";
		}
		return "Away";
	}

	override void BeginPlay()
	{
		Super.BeginPlay();
		RealmsCharge = -1;
		RealmsPullTic = -1;
		RealmsShotTic = -1;
		RealmsAmmoTic = -1;
		RealmsReloadTic = -1;
		RealmsNoAmmoTic = -1;
		RealmsEquipTic = -1;
		RealmsCycleEndTic = -1;
	}

	Default
	{
		+NOGRAVITY;
		+NOINTERACTION;
		+NOTELEPORT;
		+DONTSPLASH;
		+NOTONAUTOMAP;
		+NOBLOCKMAP;
		+NODAMAGE;
	}

	States
	{
	Spawn:
		TNT1 A -1;
		Stop;
	}
}

class RealmsWeapon9 : RealmsHeld { States { Spawn: R009 A -1; Stop; } }
class RealmsWeapon12 : RealmsHeld { States { Spawn: R012 A -1; Stop; } }
class RealmsWeapon19 : RealmsHeld { States { Spawn: R019 A -1; Stop; } }
class RealmsWeapon28 : RealmsHeld { States { Spawn: R028 A -1; Stop; } }
class RealmsWeapon29 : RealmsHeld { States { Spawn: R029 A -1; Stop; } }
class RealmsWeapon33 : RealmsHeld { States { Spawn: R033 A -1; Stop; } }
class RealmsWeapon38 : RealmsHeld { States { Spawn: R038 A -1; Stop; } }
class RealmsWeapon54 : RealmsHeld { States { Spawn: R054 A -1; Stop; } }
class RealmsWeapon68 : RealmsHeld { States { Spawn: R068 A -1; Stop; } }
class RealmsWeapon97 : RealmsHeld { States { Spawn: R097 A -1; Stop; } }
class RealmsWeapon127 : RealmsHeld { States { Spawn: R127 A -1; Stop; } }
class RealmsWeapon150 : RealmsHeld { States { Spawn: R150 A -1; Stop; } }
class RealmsWeapon156 : RealmsHeld { States { Spawn: R156 A -1; Stop; } }
class RealmsWeapon221 : RealmsHeld { States { Spawn: R221 A -1; Stop; } }
class RealmsWeapon222 : RealmsHeld { States { Spawn: R222 A -1; Stop; } }
