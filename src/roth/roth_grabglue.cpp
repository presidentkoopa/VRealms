//
// roth_grabglue.cpp -- RSQ DISTANCE GRAB IN THE REALMS: THE GLUE, AND ONLY THE GLUE.
//
// The owner's RSQ Distance Grab (RSQ_UBQSeries/DistanceGrab, 9eb2766d) is COPIED, unchanged but for its
// cvars' `noarchive`, into the engine pk3 (wadsrc/static/filter/vrealms: the engine moves those files to the
// end of the pk3 as its own lumps when the game is VRealms; docs/rsq_distancegrab/CHECKLIST.md). It knows
// Doom's things. Two things are said here, in Realms terms, and nothing else:
//
//   WHAT IT MAY TAKE. A Realms object is a LoaderProp, +NOINTERACTION, and the grab's own Decide refuses a
//   NOINTERACTION actor whatever its table says (policy.zs); a LoaderProp also matches no row of its table
//   ("not a kind of thing the grab takes (no rule)"). A picture object with no command chain is spawned as
//   RealmsTakeable instead (roth_objects.cpp Pending::plainPicture; wadsrc/static/zscript/actors/
//   realmsgrab.zs), an Inventory that draws and behaves exactly as LoaderProp. Here, once the runtime and the
//   original's database are up, each one whose use IS the original's pickup (roth_runtime.h
//   ObjectIsPickupItem: no chain, the type-4 branch would take it) loses NOINTERACTION and gets its pick box as
//   its size: the grab's table then takes it by its own catch-all row ("a pickup in the world", Inventory, weight
//   1). Everything else stays NOINTERACTION and is refused by the grab's own Decide.
//
//   WHAT A CATCH DOES. The grab's catch asks every "GrabEventService" first ("pickup.first", flight.zs
//   TakeInto, feel.zs RSQDG_Events.Ask). realmsgrab.zs answers for a RealmsTakeable through the native below:
//   the original's pickup from the hand that caught it -- roth_vrpick.h VRPickTakeCaught, the hands' own use
//   path (DoUse -> Activate(Use) -> activate_targeted_object's type-4 branch -> give_item_by_dbase_id), gated
//   by the original's cursor on it. Taken: the object's actor is destroyed by that path and the answer is 1
//   ("given to this hand and taken away"). Refused (the pickup lock while another item flies, GiveItem's own
//   answer): 0, and the grab carries it as it carries anything else it caught.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_grabglue.h"
#include "roth_runtime.h"
#include "roth_vrpick.h"
#include "roth_objects.h"
#include "actor.h"
#include "actorinlines.h"
#include "g_levellocals.h"
#include "printf.h"
#include "roth_raw.h"
#include "roth_dbase100.h"
#include "vm.h"
#include "events.h"
#include "types.h"    // PField, PType (RSQDG_Near.ready)
#include <algorithm>

namespace roth
{

bool RuntimeObjectRecord(int rothSector, int rothIndex, Object &out);   // roth_runtime.cpp (as roth_interface.cpp declares it)

namespace
{

FLevelLocals *gLevel = nullptr;
int gNextCheck = 0;
int gTakeable = -1, gNotItems = -1;   // last reported counts

void MakeTakeable(AActor *mo)
{
	DVector3 lo, hi;
	ObjectPickBox(mo, lo, hi);
	// The grab measures a thing from its middle (Pos + Height / 2, cone.zs) and its body (radius): the box the
	// original's pick uses, from the actor's own height up.
	const double r = std::max(2.0, std::max(hi.X - lo.X, hi.Y - lo.Y) * 0.5);
	const double h = std::max(2.0, hi.Z - mo->Z());
	FLinkContext ctx;
	mo->UnlinkFromWorld(&ctx);
	mo->flags5 &= ~MF5_NOINTERACTION;
	// AND BACK INTO THE BLOCKMAP. A NOINTERACTION actor's first tick unlinks it from the blockmap for good
	// (p_mobj.cpp AActor::Tick, the NOINTERACTION branch: `flags |= MF_NOBLOCKMAP`), and clearing NOINTERACTION
	// does not undo that. The grab's cone walks the blockmap (cone.zs Best / Explain, BlockThingsIterator), so
	// without this the item is never a candidate: "refused -- nothing in the cone" with no nearest
	// (owner's PC, 5d569d2f, x01_dgrab_dbg, t91).
	mo->flags &= ~MF_NOBLOCKMAP;
	mo->radius = r;
	mo->Height = h;
	mo->LinkToWorld(&ctx);
}

void MakeInert(AActor *mo)
{
	FLinkContext ctx;
	mo->UnlinkFromWorld(&ctx);
	mo->flags5 |= MF5_NOINTERACTION;
	mo->flags |= MF_NOBLOCKMAP;   // as NOINTERACTION's own tick leaves it (p_mobj.cpp AActor::Tick)
	mo->LinkToWorld(&ctx);
}

} // namespace (anonymous)

void GrabGlueTick(FLevelLocals *level)
{
	if (level == nullptr) return;
	if (gLevel != level) { gLevel = level; gNextCheck = 0; gTakeable = gNotItems = -1; }
	// Once a second: objects become items or stop being them only through the level logic, which is rare.
	if (level->maptime < gNextCheck) return;
	gNextCheck = level->maptime + 35;
	PClassActor *cls = PClass::FindActor("RealmsTakeable");
	if (cls == nullptr) return;
	int takeable = 0, notItems = 0, made = 0;
	auto it = level->GetThinkerIterator<AActor>(cls->TypeName);
	AActor *mo;
	while ((mo = it.Next()) != nullptr)
	{
		if (mo->ObjectFlags & OF_EuthanizeMe) continue;
		const bool item = ObjectIsPickupItem(mo);
		const bool inert = (mo->flags5 & MF5_NOINTERACTION) != 0;
		if (item)
		{
			takeable++;
			if (inert)
			{
				MakeTakeable(mo);
				made++;
				int rs = -1, ri = -1;
				Object o;
				if (ObjectForActor(mo, rs, ri) && RuntimeObjectRecord(rs, ri, o))
				{
					const uint16_t rid = (uint16_t)(o.textureIndex | (o.textureSource << 8));
					Printf("[roth_grab]   offered: object %d/%d, item %d (render id 0x%04x, command id %u), at (%.0f %.0f %.0f)\n",
						rs, ri, TheDbase100().FindItemByRenderId(rid), (unsigned)rid, (unsigned)o.commandID, mo->X(), mo->Y(), mo->Z());
				}
			}
		}
		else
		{
			notItems++;
			// No longer an item (the level logic changed it): inert again, unless the grab has it moving.
			if (!inert && !mo->BoolVar(FName("realmsMoved"))) MakeInert(mo);
		}
	}
	if (takeable != gTakeable || notItems != gNotItems)
	{
		Printf("[roth_grab] RSQ Distance Grab may take %d Realms object(s) here (the original's pickup items; %d newly); "
			"%d other plain picture object(s) are refused\n", takeable, made, notItems);
		gTakeable = takeable;
		gNotItems = notItems;
	}
}

bool GrabGlueTakesGrip(FLevelLocals *level, int role)
{
	if (level == nullptr || level->localEventManager == nullptr || (role != 0 && role != 1)) return false;
	static const FName nearName("RSQDG_Near"), readyName("ready");
	for (DStaticEventHandler *eh = level->localEventManager->FirstEventHandler; eh != nullptr; eh = eh->next)
	{
		if (eh->GetClass()->TypeName != nearName) continue;
		// near.zs: `transient bool ready[2]` -- the package's own answer for this tic (its WorldTick)
		PField *f = dyn_cast<PField>(eh->GetClass()->FindSymbol(readyName, true));
		if (f == nullptr || f->Type == nullptr || f->Type->Size != 2 * sizeof(bool)) return false;
		const bool *ready = (const bool *)eh->ScriptVar(readyName, nullptr);
		return ready[role];
	}
	return false;
}

} // namespace roth

//===========================================================================
//
// The catch's native (realmsgrab.zs RealmsGrab.TakeCaught).
//
//===========================================================================

static int RealmsGrab_TakeCaught(AActor *item, int hand)
{
	if (item == nullptr || (hand != 0 && hand != 1)) return 0;
	int rs = -1, ri = -1;
	roth::ObjectForActor(item, rs, ri);
	Printf("[roth_grab] RSQ Distance Grab: the %s hand caught Realms object %d/%d -- the original's pickup from that hand\n",
		hand == 0 ? "main" : "off", rs, ri);
	const bool took = roth::VRPickTakeCaught(hand, item);
	Printf("[roth_grab]   -> %s\n", took ? "taken (the original's pickup acted; the object is gone)"
		: "the original did not take it (the grab carries it)");
	return took ? 1 : 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(_RealmsGrab, TakeCaught, RealmsGrab_TakeCaught)
{
	PARAM_PROLOGUE;
	PARAM_OBJECT(item, AActor);
	PARAM_INT(hand);
	ACTION_RETURN_BOOL(RealmsGrab_TakeCaught(item, hand) != 0);
}
