//
// RSQ DISTANCE GRAB IN THE REALMS: the glue, and only the glue (src/roth/roth_grabglue.cpp has the whole of it).
// The grab itself is the owner's package, copied (wadsrc/static/filter/vrealms, docs/rsq_distancegrab/CHECKLIST.md).
//

// A PICTURE OBJECT WITH NO COMMAND CHAIN (roth_objects.cpp Pending::plainPicture), spawned in place of LoaderProp.
// Until the runtime finds its use is the original's pickup it IS a LoaderProp in all but name: the same flags
// (loaderprop.zs says why each), NOINTERACTION included, so RSQ Distance Grab's own Decide refuses it. An item
// loses NOINTERACTION (roth_grabglue.cpp MakeTakeable): an Inventory lying in the world, which the grab's table
// takes by its catch-all row.
//
// RESTING WHERE THE MAP PUT IT. LoaderProp's NOINTERACTION is what keeps P_ZMovement from snapping a prop the
// map placed below its floor back up to the floor (loaderprop.zs, measured). Without it, the actor does not
// tick at all until something moves it -- the grab's flight, its carry, its throw -- so it lies exactly where
// the original drew it; from the first move on it is an ordinary thing in the world.
class RealmsTakeable : Inventory
{
	bool realmsMoved;      // read by roth_grabglue.cpp: something moved it from where the map put it
	private bool realmsRestKnown;
	private Vector3 realmsRest;

	Default
	{
		+NOGRAVITY;
		+NOINTERACTION;
		+NOTELEPORT;
		+DONTSPLASH;
		+NOTONAUTOMAP;
		+SYNCHRONIZED;
		+NODAMAGE;
		-SPECIAL;          // never taken by walking into it: the original takes an item by its click alone
		-COUNTITEM;
	}

	override void Tick()
	{
		if (!bNOINTERACTION && !realmsMoved)
		{
			if (!realmsRestKnown) { realmsRest = Pos; realmsRestKnown = true; }
			if (Pos == realmsRest && Vel == (0, 0, 0)) return;
			realmsMoved = true;
		}
		Super.Tick();
	}

	// Never the engine's pickup: the original's, through the catch (RealmsGrabEventService).
	override bool TryPickup(in out Actor toucher) { return false; }

	States
	{
	Spawn:
		TNT1 A -1;
		Stop;
	}
}

// The original's pickup of a caught Realms item (roth_grabglue.cpp RealmsGrab_TakeCaught).
class RealmsGrab play
{
	native static bool TakeCaught(Actor item, int hand);
}

// THE CATCH, IN REALMS TERMS. RSQ Distance Grab asks every Service whose name holds "GrabEventService" before a
// caught pickup is used ("pickup.first", its flight.zs TakeInto / feel.zs RSQDG_Events.Ask): 1 = this pack gave it
// to that hand and took the thing away. For a Realms item that is the original's pickup from that hand; for
// anything else, and when the original does not take it, no answer (0), and the grab goes on as it always does.
class RealmsGrabEventService : Service
{
	override int GetInt(String request, String stringArg, int intArg, double doubleArg, Object objectArg, Name nameArg)
	{
		if (!(request ~== "grab.event") || !(stringArg ~== "pickup.first")) return 0;
		let t = RealmsTakeable(objectArg);
		if (!t || t.bNOINTERACTION) return 0;
		return RealmsGrab.TakeCaught(t, intArg) ? 1 : 0;
	}
}
