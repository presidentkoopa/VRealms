//
// roth_vrweapons.cpp -- chapter 2's weapons in VR, in the engine. roth_vrweapons.h says what each part does; the rules
// and the geometry are roth_vrweapon.h (engine-free, tools/vrweapon_test).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_vrweapons.h"
#include "roth_vrweapon.h"
#include "roth_coltvr.h"
#include "roth_vrpick.h"
#include "roth_hands.h"
#include "roth_objects.h"
#include "roth_interface.h"
#include "roth_inventory.h"
#include "roth_actors_engine.h"
#include "roth_grabglue.h"
#include "roth_dbase100.h"

#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "hw_vrmodes.h"
#include "r_data/vr_handpose.h"

#include <math.h>
#include <stdlib.h>
#include <vector>

// The engine's own support-hold settings (hw_vrmodes.cpp), read only. File scope, never inside a namespace.
EXTERN_CVAR(Float, vr_support_cone_enter)
EXTERN_CVAR(Float, vr_support_cone_exit)
EXTERN_CVAR(Float, vr_support_dist_min)
EXTERN_CVAR(Float, vr_support_dist_max)
EXTERN_CVAR(Int, vr_support_mode)

namespace roth
{

namespace
{

using namespace vrweapon;
using gesture::V3;

struct State
{
	FLevelLocals *level = nullptr;
	SupportLatch latch;
	HolsterRig rig;
	int worn = 0;               // the item worn on the off forearm (Inventory::SetWorn)
	int lastHeld = -1;
	bool engagedWas = false;
	bool setStabilized = false; // this layer set WF_TWOHANDSTABILIZED (and so clears it)
	int pulse2Tic = -1;         // a refusal's second pulse
} S;

const char *RoleName(int role) { return role == VR_MAINHAND ? "main" : "off"; }

AActor *Pawn()
{
	if (consoleplayer < 0 || consoleplayer >= MAXPLAYERS) return nullptr;
	return players[consoleplayer].mo;
}

double UPM() { return VRPickUnitsPerMetre(); }
double PS() { return S.level != nullptr && S.level->pixelstretch > 0.1 ? S.level->pixelstretch : 1.2; }

// Map units -> real metres (the vertical through the level's pixel stretch, as the gestures read the hands).
V3 Metres(const DVector3 &d) { const double u = UPM(); return V3(d.X / u, d.Y / u, d.Z * PS() / u); }

// The VR hands are live: a headset, or the controller-pose test (roth_xrpose), with the aim pose on the pawn.
bool HandsOn(AActor *pmo) { return pmo != nullptr && pmo->OverrideAttackPosDir && VRHandPose_InputLive(); }

int HeldItem()
{
	const Inventory *inv = InterfaceInventory();
	if (inv == nullptr || inv->LeftHand() < 0) return 0;
	const int id = (int16_t)inv->Slot(inv->LeftHand()).id;
	return id > 0 && id != (int)inv->AdamItemId() ? id : 0;
}

int SlotOf(int item)
{
	const Inventory *inv = InterfaceInventory();
	if (inv == nullptr || item <= 0) return -1;
	for (int i = 0; i < Inventory::SLOT_COUNT; i++) if ((int16_t)inv->Slot(i).id == item) return i;
	return -1;
}

bool CarriedCb(int item, void *) { return SlotOf(item) >= 0; }

// The weapon's forward, real metres: the aim pose (AttackAngle is the world yaw less 90, AttackPitch positive up:
// g_game.cpp:1452-1455, as WeaponTick reads it).
V3 Barrel(AActor *pmo)
{
	const double yaw = (pmo->AttackAngle.Degrees() + 90.0) * (M_PI / 180.0), pit = pmo->AttackPitch.Radians();
	return V3(cos(pit) * cos(yaw), cos(pit) * sin(yaw), sin(pit));
}

SupportSettings Settings()
{
	SupportSettings s;
	s.coneEnter = vr_support_cone_enter;
	s.coneExit = vr_support_cone_exit;
	s.distMin = vr_support_dist_min;
	s.distMax = vr_support_dist_max;
	s.mode = vr_support_mode;
	return s;
}

// The main hand in the body frame: metres from the head (the eye), forward / left / up of the player's yaw.
void BodyFrame(AActor *pmo, const DVector3 &at, double &f, double &l, double &u)
{
	const DVector3 head(pmo->X(), pmo->Y(), pmo->player != nullptr ? pmo->player->viewz : pmo->Z());
	const V3 d = Metres(at - head);
	const double yaw = pmo->Angles.Yaw.Radians();
	f = d.x * cos(yaw) + d.y * sin(yaw);
	l = -d.x * sin(yaw) + d.y * cos(yaw);
	u = d.z;
}

HolsterWorld World(int held) { HolsterWorld w; w.held = held; w.carried = CarriedCb; return w; }

// THE FEEL (rig.zs:910-921, body.zs:782-788): through the headset's haptics; under the controller-pose test a log only.
void Pulse(int role, double amp, int tics, const char *why)
{
	const VRMode *vm = VRMode::GetVRModeCached(true);
	const bool real = !VRPoseTest_Active() && vm != nullptr && vm->IsVR();
	if (real) VR_ScriptHaptic(role, amp, tics * 1000.0 / TICRATE);
	else Printf("[roth_vrweapons] %s hand haptic %.2f for %d tics (%s; no headset: not sent)\n", RoleName(role), amp, tics, why);
}

const char *ItemName(int item)
{
	switch (item)
	{
	case 9: return "the Colt";
	case 12: return "the Staff";
	case 28: return "the Shield";
	case 29: return "the Sword";
	case 38: return "the Shotgun";
	default: return "a weapon";
	}
}

void Act(const HolsterEvent &e, int held)
{
	Inventory *inv = InterfaceInventory();
	InventorySink *sink = InterfaceInventorySink();
	if (inv == nullptr) return;
	const char *place = e.place >= 0 ? kPlaces[e.place].name : "-";
	switch (e.act)
	{
	case HOLSTER_ENTER:
		Pulse(VR_MAINHAND, kEnterAmp, kEnterTics, "a body slot entered");
		Printf("[roth_vrweapons] main hand enters the %s slot (%s)\n", place,
			S.rig.owner[e.place] != 0 ? FStringf("%s's, i%d", ItemName(S.rig.owner[e.place]), S.rig.owner[e.place]).GetChars() : "unassigned");
		return;
	case HOLSTER_STOW:
		inv->EmptyWeaponHand(sink);
		Pulse(VR_MAINHAND, 0.55, 5, "put away");
		Printf("[roth_vrweapons] main hand at the %s slot: %s (i%d) put away -- activate_weapon_item(0, 0), the inventory's own; %s%s\n",
			place, ItemName(e.item), e.item, e.why, e.assigned ? " (assigned now)" : "");
		break;
	case HOLSTER_DRAW:
	case HOLSTER_SWAP:
	{
		const int slot = SlotOf(e.item);
		inv->WeaponClick(slot, sink);
		Pulse(VR_MAINHAND, 0.65, 6, "drawn");
		Printf("[roth_vrweapons] main hand at the %s slot: %s (i%d, inventory slot %d) drawn -- the inventory's weapon click"
			" (activate_weapon_item(slot, id)); %s%s\n", place, ItemName(e.item), e.item, slot, e.why,
			e.act == HOLSTER_SWAP ? FStringf(" (%s, i%d, left the hand)", ItemName(held), held).GetChars() : "");
		break;
	}
	case HOLSTER_REFUSE:
		Pulse(VR_MAINHAND, 0.3, 3, "refused");
		S.pulse2Tic = S.level != nullptr ? S.level->maptime + 3 + 4 : -1;
		Printf("[roth_vrweapons] main hand at the %s slot: refused -- %s (holding %s)\n", place, e.why,
			held > 0 ? FStringf("%s, i%d", ItemName(held), held).GetChars() : "nothing: his fists");
		return;
	case HOLSTER_FREE:
		Pulse(VR_MAINHAND, 0.55, 5, "freed");
		Printf("[roth_vrweapons] main hand held 1 s in the empty %s slot: freed (it was %s's, i%d)\n", place, ItemName(e.item), e.item);
		return;
	default:
		return;
	}
	const int now = HeldItem();
	Printf("[roth_vrweapons] the weapon hand (0x81038) now holds %s\n", now > 0 ? FStringf("%s, i%d", ItemName(now), now).GetChars() : "nothing: his fists");
}

} // namespace (anonymous)

int VRWeaponsHeldItem() { return HeldItem(); }

bool VRWeaponsTriggerHeldBack()
{
	return HandsOn(Pawn()) && IsTriggerHeldBack(HeldItem());
}

bool VRWeaponsGripTaken(int role)
{
	AActor *pmo = Pawn();
	if (!HandsOn(pmo)) return false;
	if (ColtGripTaken(role)) return true;   // the Colt's release, pouch, slide or magazine (roth_coltvr.h)
	const int held = HeldItem();
	if (role == VR_OFFHAND)
	{
		if (!IsTwoHanded(held)) return false;
		if (S.latch.engaged) return true;
		SupportLatch probe = S.latch;
		probe.gripWas = false;
		SupportSettings st = Settings();
		if (st.mode == 0) return false;
		st.mode = 1;   // the cone alone: would this grip take the fore-end?
		return probe.Feed(Barrel(pmo), Metres(pmo->OffhandPos - pmo->AttackPos), true, st);
	}
	double f, l, u;
	BodyFrame(pmo, pmo->AttackPos, f, l, u);
	return S.rig.Pick(f, l, u, World(held)) >= 0;
}

bool VRWeaponsTwoHandAim(double &yawDeg, double &pitchUpDeg, DVector3 &rear)
{
	AActor *pmo = Pawn();
	if (!HandsOn(pmo) || !S.latch.engaged || !IsTwoHanded(HeldItem())) return false;
	rear = pmo->AttackPos;
	return TwoHandAim(Metres(pmo->AttackPos), Metres(pmo->OffhandPos), yawDeg, pitchUpDeg);
}

bool VRWeaponsShieldWorn(int &item)
{
	item = S.worn;
	return S.worn != 0 && HandsOn(Pawn());
}

void VRWeaponsTick(FLevelLocals *level, bool live)
{
	if (level == nullptr) return;
	if (S.level != level)
	{
		const HolsterRig keep = S.rig;   // the assignments are the player's, not the level's
		S = State();
		S.rig = keep;
		S.rig.ResetHand();
		S.level = level;
	}
	AActor *pmo = Pawn();
	Inventory *inv = InterfaceInventory();
	const bool on = HandsOn(pmo) && pmo->health > 0;

	// ---- THE SHIELD, WORN (Q7) ----
	const int worn = (on && SlotOf(kShield) >= 0) ? kShield : 0;
	if (inv != nullptr) inv->SetWorn(worn);
	if (worn != S.worn)
	{
		if (worn != 0) Printf("[roth_vrweapons] the Shield (i28) is worn on the off forearm: the original's in-hand test (RAW 0x27 select mode) counts it\n");
		else Printf("[roth_vrweapons] nothing worn on the off forearm: the in-hand test is the original's alone\n");
		S.worn = worn;
	}

	const int held = HeldItem();
	if (held != S.lastHeld)
	{
		if (on) Printf("[roth_vrweapons] the weapon hand holds %s%s\n", held > 0 ? FStringf("%s, i%d", ItemName(held), held).GetChars() : "nothing: his fists",
			IsTriggerHeldBack(held) ? " -- swung, never triggered (the fire button held back)"
			: SwingBorrowsFists(held) ? " -- swung (Adam's fists' blow) and triggered (its own projectile)" : IsTwoHanded(held) ? " -- two-handed: the off hand on the fore-end aims it" : "");
		S.lastHeld = held;
	}

	// ---- TWO HANDS ON THE SHOTGUN (Q9) ----
	bool engaged = false;
	if (on && live && IsTwoHanded(held))
	{
		VRPickHand off;
		// ONE MOD PER PRESS (roth_grabglue.h): a press RSQ Distance Grab takes (an offered item at that hand) is not the fore-end's
		const bool grip = VRPickHandNow(VR_OFFHAND, off) && off.gripClosed && (S.latch.engaged || !GrabGlueTakesGrip(level, VR_OFFHAND));
		engaged = S.latch.Feed(Barrel(pmo), Metres(pmo->OffhandPos - pmo->AttackPos), grip, Settings());
	}
	else S.latch.Reset();
	if (engaged != S.engagedWas)
	{
		double y = 0, p = 0;
		const bool aim = engaged && TwoHandAim(Metres(pmo->AttackPos), Metres(pmo->OffhandPos), y, p);
		Printf("[roth_vrweapons] %s (i%d): the off hand %s (along the barrel %.3f, %.2f m apart)%s\n", ItemName(held), held,
			engaged ? "TAKES the fore-end" : "lets go of the fore-end", S.latch.along, S.latch.dist,
			aim ? FStringf(": the shot now leaves along the rear -> fore line, yaw %.1f, pitch %.1f up", y, p).GetChars() : "");
		S.engagedWas = engaged;
	}
	// RSQ Distance Grab stands down while two-handing (reach.zs TwoHanding: WF_TWOHANDSTABILIZED). No Doom weapon runs
	// in the Realms (p_pspr.cpp never sets or clears the bit here), so this layer owns it.
	if (pmo != nullptr && pmo->player != nullptr)
	{
		if (engaged) { pmo->player->WeaponState |= WF_TWOHANDSTABILIZED; S.setStabilized = true; }
		else if (S.setStabilized) { pmo->player->WeaponState &= ~WF_TWOHANDSTABILIZED; S.setStabilized = false; }
	}

	// ---- BODY SLOTS (Q8), the main hand ----
	if (on && live)
	{
		double f, l, u;
		BodyFrame(pmo, pmo->AttackPos, f, l, u);
		VRPickHand mh;
		// ONE MOD PER PRESS: a press RSQ Distance Grab takes (an offered item at the hand) is not a body slot's
		const bool grip = VRPickHandNow(VR_MAINHAND, mh) && mh.gripClosed && !GrabGlueTakesGrip(level, VR_MAINHAND);
		const HolsterEvent e = S.rig.Step(true, f, l, u, grip, World(held));
		if (e.act != HOLSTER_NONE) Act(e, held);
	}
	else S.rig.ResetHand();
	if (S.pulse2Tic >= 0 && level->maptime >= S.pulse2Tic) { Pulse(VR_MAINHAND, 0.3, 3, "refused, the second pulse"); S.pulse2Tic = -1; }

	// ---- THE COLT'S REAL PARTS (roth_coltvr.h): the off hand against the drawn gun ----
	ColtHandsTick(level, live);
}

} // namespace roth

//===========================================================================
//
// roth_vrweapontest -- test-only, inert until typed. With roth_xrpose (the controller-pose test) driving the hands.
//
//   roth_vrweapontest status              the weapon hand, the worn item, the two-hand hold, the body slots
//   roth_vrweapontest stand <units>       stand the player <units> from the nearest creature in the original's pool
//                                         (its axis to his), facing it
//   roth_vrweapontest fists               the weapon hand emptied as the inventory does (activate_weapon_item(0, 0))
//   roth_vrweapontest click <item>        the inventory's weapon click on the slot holding <item>
//
//===========================================================================

CCMD(roth_vrweapontest)
{
	using namespace roth;
	using namespace roth::vrweapon;
	if (argv.argc() < 2) { Printf("roth_vrweapontest status | stand <units> | fists | click <item>\n"); return; }
	AActor *pmo = Pawn();
	Inventory *inv = InterfaceInventory();
	if (pmo == nullptr || inv == nullptr) { Printf("roth_vrweapontest: no player or no inventory yet\n"); return; }
	if (!stricmp(argv[1], "status"))
	{
		FString owners;
		for (int r = 0; r < kHolsterCount; r++) owners.AppendFormat(" %s=%d", kPlaces[r].name, S.rig.owner[r]);
		double f = 0, l = 0, u = 0;
		BodyFrame(pmo, pmo->AttackPos, f, l, u);
		Printf("roth_vrweapontest: hands %s; weapon hand i%d; worn i%d (inventory says %d); in-hand test of i28 (mode 3) = %d; two-hand hold %d;"
			" main hand at f %.2f l %.2f u %.2f m (slot %d); slots:%s\n", HandsOn(pmo) ? "live" : "off", HeldItem(), S.worn, inv->Worn(),
			inv->Query(TheDbase100(), 28, 3, nullptr), (int)S.latch.engaged, f, l, u, S.rig.Pick(f, l, u, World(HeldItem())), owners.GetChars());
		return;
	}
	if (!stricmp(argv[1], "fists")) { inv->EmptyWeaponHand(InterfaceInventorySink()); Printf("roth_vrweapontest: the weapon hand emptied (now i%d)\n", HeldItem()); return; }
	if (!stricmp(argv[1], "click") && argv.argc() >= 3)
	{
		const int item = atoi(argv[2]), slot = SlotOf(item);
		if (slot < 0) { Printf("roth_vrweapontest: i%d is not carried\n", item); return; }
		inv->WeaponClick(slot, InterfaceInventorySink());
		Printf("roth_vrweapontest: the inventory's weapon click on slot %d (i%d): the weapon hand holds i%d\n", slot, item, HeldItem());
		return;
	}
	if (!stricmp(argv[1], "stand") && argv.argc() >= 3)
	{
		const double units = atof(argv[2]);
		std::vector<std::pair<int, int>> pool;
		ActorsPoolObjects(pool);
		AActor *best = nullptr;
		double bestD = 1e30;
		int bs = -1, bi = -1;
		for (const auto &si : pool)
		{
			if (si.first < 0) continue;
			int r = 0, h = 0;
			if (!ActorsCreatureBody(si.first, si.second, r, h)) continue;
			AActor *mo = FindObjectActor(si.first, si.second);
			if (mo == nullptr) continue;
			const double d = (mo->Pos().XY() - pmo->Pos().XY()).Length();
			if (d < bestD) { bestD = d; best = mo; bs = si.first; bi = si.second; }
		}
		if (best == nullptr) { Printf("roth_vrweapontest stand: no creature in the original's pool (none has woken)\n"); return; }
		DVector2 away = pmo->Pos().XY() - best->Pos().XY();
		if (away.Length() < 1e-3) away = DVector2(1, 0);
		away = away.Unit();
		const DVector3 at(best->X() + away.X * units, best->Y() + away.Y * units, pmo->Z());
		pmo->SetOrigin(at, false);
		pmo->SetZ(pmo->floorz);
		pmo->Vel.Zero();
		pmo->Angles.Yaw = DAngle::fromRad(atan2(-away.Y, -away.X));
		pmo->Angles.Pitch = nullAngle;
		pmo->ClearInterpolation();
		Printf("roth_vrweapontest stand: the player at (%.0f %.0f %.0f), %.0f units from creature %d/%d at (%.0f %.0f %.0f), facing it (yaw %.1f)\n",
			pmo->X(), pmo->Y(), pmo->Z(), units, bs, bi, best->X(), best->Y(), best->Z(), pmo->Angles.Yaw.Degrees());
		return;
	}
	Printf("roth_vrweapontest: unknown \"%s\"\n", argv[1]);
}
