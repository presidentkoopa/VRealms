//
// A solid, moving piece of a level that a native map loader places and then
// steers from C++.
//
// The counterpart of LoaderProp for things that MOVE and CARRY. A loader that
// reads a format where geometry slides sideways -- Realms of the Haunting moves
// a whole sector by its vertices -- has nothing in a static BSP to slide. It
// builds the moving piece as a mesh in memory instead and hangs it on one of
// these.
//
// ALL OF THE PLATFORM BEHAVIOUR IS INHERITED. FishyPlatform is "3D Platform" by
// FishyClock, GPL-3.0, https://github.com/FishyClock/3Dplatform, shipped
// unmodified in zscript/fishy/. Standing on it, being carried by it, being
// pushed out of its way, several boxes moving as one group and portal crossing
// are that library's work. This class adds only what the library keeps private
// from the outside and a loader needs: a way to say "be HERE this tic".
//
// The loader sets, from C++, after spawning (src/roth/roth_platforms.cpp):
//   radius / height      the solid box
//   SpawnPoint           where the box was when the level loaded
//   args[1]              the library's option flags (1 linear | 32 ignore geometry)
//   args[2] and the TID  the group, when one moving piece needs several boxes
//   sprite               the frame its in-memory mesh is bound to, or it is
//                        made invisible when another box of the group draws it
//
// The spawn state has tics -1, as LoaderProp's does, so nothing ever puts the
// sprite back to what the state names. FishyPlatform.Tick only advances a
// state whose tics are not -1.
//
class LoaderPlatform : FishyPlatform
{
	Default
	{
		Radius 16;
		Height 16;
		+NOTONAUTOMAP;
		+DONTSPLASH;
		+SYNCHRONIZED;
	}

	States
	{
	Spawn:
		TNT1 A -1;
		Stop;
	}

	// Move to SpawnPoint + (dx, dy, dz), now, as one straight step.
	//
	// Called by the loader's level logic once per world step while this piece
	// is not where the logic says it is. Two library calls, both from its
	// public "ACS utility" set:
	//
	//   ACSFuncMove         sets up a move from the current position to an
	//                       exact one, with a travel time of one tic. It also
	//                       makes this platform the origin of its group, so the
	//                       other boxes follow rigidly.
	//   ACSFuncInterpolate  runs that move to its end (time 1.0) immediately,
	//                       as an ordinary collision-checked move: passengers
	//                       ride along and obstacles are pushed.
	//
	// Without the second call the library would spend this tic at time 0 --
	// standing still -- and only arrive on the next, and a driver that renews
	// the request every tic would then never move the platform at all.
	//
	// If the move is refused (something solid cannot be pushed out of the way)
	// the platform stays where it is. The caller notices it is still astray and
	// asks again next tic with the logic's new position, so a blocked platform
	// catches up when it is freed instead of desynchronising for good.
	void LoaderDrive (double dx, double dy, double dz)
	{
		if (IsPortalCopy())
			return;

		vector3 want = SpawnPoint + (dx, dy, dz);
		FishyPlatform.ACSFuncMove(self, 0, want.x, want.y, want.z, true, 1);
		FishyPlatform.ACSFuncInterpolate(self, 0, 1.0, false);
	}
}
