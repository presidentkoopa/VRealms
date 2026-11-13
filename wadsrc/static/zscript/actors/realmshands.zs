//
// THE VR HANDS: one actor per physical hand, spawned and placed by the engine
// (src/roth/roth_hands.cpp) while a headset is on -- never in flat -- and drawn on
// its controller's GRIP pose at draw rate (FollowHandMode 3 / 4). Its fingers are
// posed by the engine every displayed frame from that controller's trigger, squeeze
// and capacitive touches (src/r_data/vr_handpose.cpp).
//
// No mesh yet: the IQM hands were removed (owner, 2026-11-13); the engine will load hand_final.fbx itself;
// the MODELDEF is vrealms_iwad/MODELDEF.txt. Presentation only: no collision, no
// interaction, nothing in the game reads them. The weapon (RealmsHeld) is separate.
//
class RealmsHand : Actor
{
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

class RealmsHandRight : RealmsHand { States { Spawn: TNT1 A -1; Stop; } }
class RealmsHandLeft : RealmsHand { States { Spawn: TNT1 A -1; Stop; } }
