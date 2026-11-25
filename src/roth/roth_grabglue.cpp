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
#include "c_cvars.h"
#include "d_eventbase.h"   // event_t, EV_KeyDown / EV_KeyUp
#include "keydef.h"
#include "i_time.h"
#include "r_utility.h"   // r_viewpoint (the pull trace)
#include "hw_vrmodes.h"     // VR_MAINHAND
#include "r_data/vr_handpose.h"   // VRHandPose_InputLive, VRHandPose_SideForRole
#include "r_data/vr_handpose_math.h"   // SIDE_RIGHT
#include <algorithm>

EXTERN_CVAR(Bool, vr_secondary_button_mappings)

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

// THE PULL, AS IT HAPPENS (owner in the headset, 2026-11-24: "why does everything rocket up into the stratosphere
// when i try to distance grab it"). Every 4th tic while a Realms item moves under RSQ Distance Grab: where it is
// against its floor and ceiling, where the package's palm is (AttackPos / OffhandPos, what it flies to), and where
// the hand VRealms draws is. Log only; nothing here moves anything. At most 80 lines a level.
static void TracePulls(FLevelLocals *level)
{
	static FLevelLocals *lvl = nullptr;
	static int lines = 0;
	static TMap<AActor *, DVector3> last;
	if (lvl != level) { lvl = level; lines = 0; last.Clear(); }
	if (lines >= 80 || (level->maptime & 3) != 0) return;
	PClassActor *cls = PClass::FindActor("RealmsTakeable");
	if (cls == nullptr) return;
	AActor *pmo = level->GetConsolePlayer() != nullptr ? level->GetConsolePlayer()->mo : nullptr;
	if (pmo == nullptr) return;
	const double upm = VRPickUnitsPerMetre() > 1.0 ? VRPickUnitsPerMetre() : 34.0;
	auto it = level->GetThinkerIterator<AActor>(cls->TypeName);
	AActor *mo;
	while ((mo = it.Next()) != nullptr && lines < 80)
	{
		if ((mo->ObjectFlags & OF_EuthanizeMe) || (mo->flags5 & MF5_NOINTERACTION)) continue;
		DVector3 *was = last.CheckKey(mo);
		const DVector3 now = mo->Pos();
		if (was == nullptr) { last[mo] = now; continue; }
		if ((now - *was).Length() < 0.5) continue;
		*was = now;
		VRPickHand h[2];
		FString hands;
		for (int r = 0; r < 2; r++)
			if (VRPickHandNow(r, h[r]) && h[r].havePoints)
				hands.AppendFormat("  %s drawn palm (%.0f %.0f %.0f) %.2f m away", r == 0 ? "main" : "off", h[r].palm.X, h[r].palm.Y,
					h[r].palm.Z, (h[r].palm - mo->PosPlusZ(mo->Height * 0.5)).Length() / upm);
		const double viewz = pmo->player != nullptr ? pmo->player->viewz : 0.0;
		Printf("[roth_grab] pull: %s at (%.0f %.0f %.0f), %.2f m over its floor, %.2f m under its ceiling, vel (%.1f %.1f %.1f);"
			" the package's palms: main (%.0f %.0f %.0f) off (%.0f %.0f %.0f); viewz %.0f, the last frame's eye %.0f, %.0f units a metre%s\n",
			mo->GetClass()->TypeName.GetChars(), now.X, now.Y, now.Z, (now.Z - mo->floorz) / upm, (mo->ceilingz - now.Z - mo->Height) / upm,
			mo->Vel.X, mo->Vel.Y, mo->Vel.Z, pmo->AttackPos.X, pmo->AttackPos.Y, pmo->AttackPos.Z,
			pmo->OffhandPos.X, pmo->OffhandPos.Y, pmo->OffhandPos.Z, viewz, r_viewpoint.CenterEyePos.Z, upm, hands.GetChars());
		lines++;
	}
}

void GrabGlueTick(FLevelLocals *level)
{
	if (level == nullptr) return;
	if (gLevel != level) { gLevel = level; gNextCheck = 0; gTakeable = gNotItems = -1; }
	TracePulls(level);
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

// ---- the main grip on the squeeze (see the header) ----
namespace
{
bool gFed[2] = { false, false };        // a press sent at the squeeze, not yet released
bool gSwallow[2] = { false, false };    // the late tap pulse of such a press is still to come
bool gSwallowingDown[2] = { false, false };
uint64_t gSwallowUntil[2] = { 0, 0 };   // after the release: how long the pulse is waited for (ms)
}

int GrabGlueGripKey(int role)
{
	return VRHandPose_SideForRole(role) == VRHandPose::SIDE_RIGHT ? KEY_PAD_RSHOULDER : KEY_PAD_LSHOULDER;
}

bool GrabGlueSqueezeFeeds(int role)
{
	return role == VR_MAINHAND && vr_secondary_button_mappings && VRHandPose_InputLive();
}

void GrabGlueGripPress(FLevelLocals *level, int role)
{
	if (role != 0 && role != 1) return;
	if (!GrabGlueSqueezeFeeds(role) || gFed[role] || level == nullptr || level->localEventManager == nullptr) return;
	level->localEventManager->SendNetworkEvent("rsqdg_ngrip", role, 1, 0, false);
	gFed[role] = true;
	gSwallow[role] = true;
	gSwallowingDown[role] = false;
	gSwallowUntil[role] = 0;
	Printf("[roth_grab] the main grip squeezed (analog): RSQ Distance Grab's grip press sent now, tic %d (its key, %d, comes only as"
		" a tap's pulse at the release under vr_secondary_button_mappings)\n", level->maptime, GrabGlueGripKey(role));
}

void GrabGlueGripRelease(FLevelLocals *level, int role)
{
	if ((role != 0 && role != 1) || !gFed[role]) return;
	gFed[role] = false;
	gSwallowUntil[role] = I_msTime() + 500;   // the device posts the pulse on the release's own frame
	if (level != nullptr && level->localEventManager != nullptr)
		level->localEventManager->SendNetworkEvent("rsqdg_ngrip", role, 0, 0, false);
	Printf("[roth_grab] the main grip opened (analog): RSQ Distance Grab's grip release sent, tic %d\n", level != nullptr ? level->maptime : -1);
}

bool GrabGlueResponder(const event_t *ev)
{
	if (ev == nullptr || (ev->type != EV_KeyDown && ev->type != EV_KeyUp)) return false;
	for (int role = 0; role < 2; role++)
	{
		if (!gSwallow[role] || ev->data1 != GrabGlueGripKey(role)) continue;
		if (ev->type == EV_KeyDown)
		{
			if (!gFed[role] && gSwallowUntil[role] != 0 && I_msTime() > gSwallowUntil[role]) { gSwallow[role] = false; continue; }
			gSwallowingDown[role] = true;
			Printf("[roth_grab] the main grip's key %d (the tap's pulse at its release) swallowed: its press was sent at the squeeze\n", ev->data1);
			return true;
		}
		if (gSwallowingDown[role])
		{
			gSwallowingDown[role] = false;
			if (!gFed[role]) gSwallow[role] = false;
			return true;
		}
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
