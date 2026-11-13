//
// THE VR HANDS: one actor per physical hand, spawned and placed by the engine
// (src/roth/roth_hands.cpp) while a headset is on -- never in flat -- and drawn on
// its controller's GRIP pose at draw rate (FollowHandMode 3 / 4). Its fingers are
// to be posed by the engine every displayed frame from that controller's trigger, squeeze
// and capacitive touches (src/r_data/vr_handpose.cpp) -- not yet for the FBX hands: which
// frames of its take are the pose targets is not chosen, so they show frame 0.
//
// The meshes are the owner's hand_final.fbx, read by the engine itself (src/common/models/model_fbx.h):
// Hand_Low for the right, Hand_Low1 for the left; the MODELDEF is vrealms_iwad/MODELDEF.txt.
// Presentation only: no collision, no interaction, nothing in the game reads them. The weapon (RealmsHeld) is separate.
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

class RealmsHandRight : RealmsHand { States { Spawn: RHDR A -1; Stop; } }
class RealmsHandLeft : RealmsHand { States { Spawn: RHDL A -1; Stop; } }
