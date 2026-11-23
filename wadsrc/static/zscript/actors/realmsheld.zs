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

//
// THE COLT AS A REAL M1911 IN VR (src/roth/roth_coltvr.cpp, roth_colt.h). The owner's mesh (vrealms_iwad/models/weapons/
// colt1911, CC BY 4.0 -- CREDITS.txt there), drawn at the main hand's grip at the fit measured on the authored grip
// (tools/colt_test/colt_fit). Its parts (slide, barrel, hammer, trigger, magazine release, magazine, rounds) are posed
// by the engine every tic from the mesh's own take on the original's fire events; the weapon logic is the original's.
// Flat play keeps RealmsWeapon9 (the original's picture). Presentation only, as every RealmsHeld.
//
class RealmsColt1911 : RealmsHeld { States { Spawn: C191 A -1; Stop; } }

//
// A PART OF THE COLT IN THE WORLD: the spent case thrown from the port with each shot, the empty magazine dropped from
// the grip. The same mesh's own part (MODELDEF "?part="), falling under the level's gravity (the engine sets this
// actor's Gravity to 9.8 m/s^2 in map units), stopped by walls and floors as any actor; it tumbles while it flies, lies
// still where it lands, and goes after RealmsLife tics. Presentation only: no game record, nothing reads it.
// OBJECT-OBJECT COLLISION (owner 2026-11-16: wanted, "if not now then later as it isn't essential"): not built. A part
// is non-SOLID, so it passes through other parts and through actors; object-world collision (floors, walls, ledges) is
// the engine's own movement. When it comes, it goes here: the part made SOLID against other RealmsColtPart (and the
// Realms items' actors) only -- CanCollideWith -- with its Radius/Height from the mesh's box, and a resting contact
// instead of the Landed snap below.
//
class RealmsColtPart : Actor
{
	int RealmsLife;               // tics it stays once it has landed
	double SpinYaw, SpinPitch, SpinRoll;
	double LandPitch, LandRoll;   // how it lies
	bool Landed;
	vector3 ThrownFrom;           // where it left the gun (the log line at landing: how far it flew)
	int Flew;                     // tics in the air
	bool WallStopped;             // its sideways speed fell by 90 % in one tic while it flew: a wall
	double PrevXY;                // last tic's sideways speed

	Default
	{
		Radius 1;
		Height 1;
		+DROPOFF;
		+NOBLOCKMONST;
		+NOTELEPORT;
		+DONTSPLASH;
		+NOTONAUTOMAP;
		+CANNOTPUSH;
		+NODAMAGE;
		+NOTRIGGER;
		+MOVEWITHSECTOR;
		-SOLID;
		-SHOOTABLE;
	}

	override void Tick()
	{
		Super.Tick();
		if (bDestroyed) return;
		if (!Landed)
		{
			if (Flew == 0) ThrownFrom = pos;
			Flew++;
			double xy = vel.xy.Length();
			if (Flew > 1 && PrevXY > 0.5 && xy < PrevXY * 0.1) WallStopped = true;
			PrevXY = xy;
			if (pos.z <= floorz + 0.05 && vel.z <= 0)
			{
				Landed = true;
				vel = (0, 0, 0);
				Pitch = LandPitch;
				Roll = LandRoll;
				Console.Printf("[roth_colt] %s LANDED at (%.1f %.1f %.1f) after %d tics, %.1f units from where it was thrown (%.1f %.1f %.1f)%s",
					GetClassName(), pos.x, pos.y, pos.z, Flew, (pos.xy - ThrownFrom.xy).Length(), ThrownFrom.x, ThrownFrom.y, ThrownFrom.z,
					WallStopped ? "; its sideways speed stopped in the air: a wall" : "");
			}
			else
			{
				Angle += SpinYaw;
				Pitch += SpinPitch;
				Roll += SpinRoll;
			}
		}
		else if (--RealmsLife <= 0) Destroy();
	}

	States
	{
	Spawn:
		TNT1 A -1;
		Stop;
	}
}

class RealmsColtCasing : RealmsColtPart
{
	override void BeginPlay() { Super.BeginPlay(); RealmsLife = 35 * 15; SpinYaw = 23; SpinPitch = 31; SpinRoll = 0; LandPitch = 0; LandRoll = 90; }
	States { Spawn: C19C A -1; Stop; }
}

class RealmsColtMagazine : RealmsColtPart
{
	override void BeginPlay() { Super.BeginPlay(); RealmsLife = 35 * 60; SpinYaw = 3; SpinPitch = 9; SpinRoll = 2; LandPitch = 90; LandRoll = 0; }
	States { Spawn: C19M A -1; Stop; }
}

// The new magazine in the off hand, from the belt pouch to the magwell (FollowHandMode 4, its seat from the engine).
class RealmsColtMagazineHeld : RealmsHeld { States { Spawn: C19M A -1; Stop; } }
