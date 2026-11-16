#pragma once
//
// roth_vrweapon.h -- CHAPTER 2'S WEAPONS IN VR, ENGINE-FREE: the decisions and the geometry, so tools/vrweapon_test
// runs every one of them on scripted hands. What each answer DOES is roth_vrweapons.cpp's: always one of the
// original's own calls (its attack aimed at a contact, its weapon click in the inventory, its in-hand test), or a
// haptic. The owner's decisions: docs/VR_CHAPTER02_SIGNS_AND_PORTENTS.md section 6, Q6-Q10 (2026-11-13/14).
//
// THE ORIGINAL'S FOUR CHAPTER-2 WEAPONS, read from the retail DBASE100 with the port's own reader (ParseWeaponActions,
// roth_actor_weapon.cpp; proven against the running original, tools/actors_test weaponcmp) and AsBullet (BulletFor):
//   Sword  i29  3 WeaponAction blocks (three swing pictures c0d2 / d6cd / e54d), each: bullet i164, frame 3, rate 4,
//               no ammo (cap 0: every pull is free, WC:793-796; the next swing's picture is rng(3) at each cycle's end,
//               player.c:535-558). Bullet i164: damage 200, speed 12, life 10 ticks, width 16 -- the short-lived bullet
//               the original's melee is (GAME_combat.md 1).
//   Shield i28  1 block: picture 23eed, rate 8, op 0x2d = 5 (tracks the aim X), panel 15, NO BULLET (op 0x13 absent):
//               the trigger plays its picture and fires nothing. Its only effect in the game is the in-hand test
//               (RAW 0x27 flags 0x22 = mode 3: query_player_inventory select mode, no scan, inventory.c:472-492 --
//               1 when it is the right-hand item 0x81044 or the weapon 0x81038) in ANUBIS: #83 (enter sector id 3:
//               "if not i28 in hand: damage"), #11 and #49 (no shield: a spawn, else record #112).
//   Shotgun i38 1 block: picture 30373, bullet i62, frame 2, rate 5, magazine 6 (cap 6), ammo item 0x30 = i48, flash
//               32; no reload picture. Bullet i62: damage 350, speed 32, life 0 (flies until it hits), width 16.
//   Staff  i12  1 block: picture 1dd0e, bullet i59, frame 5, rate 8, charge 6 (op 0x20 0x8002: a RECHARGING weapon,
//               rate 2), flash 32. Bullet i59: damage 205, speed 15, life 0, hit picture 2b7. A PROJECTILE ONLY: the
//               original's Staff has no melee block (Eternity i97 is the original's one weapon with both: block 1 a
//               recharging projectile, blocks 2-3 a melee bullet i280, damage 350, life 10).
//   Adam   i68  his fists (activate_weapon_item(0, 0) parses his own record, WC:430): 2 blocks (pictures 80d / 1880),
//               bullet i107, frames 1 / 0, rate 6, no ammo. Bullet i107: damage 100, speed 16, life 8, width 16.
//
// 1b. THE STAFF SWUNG (owner 2026-11-16, Q10: "it is a vr game and we are holding a staff, we will be able to swing
//    it"). The same reader and contact on the Staff's length; the trigger keeps the original's projectile. The original's
//    Staff has no melee, so the swing's numbers are BORROWED FROM ADAM'S FISTS (the owner's call): the blow is his fists'
//    attack as the original has it (roth_actors_engine.cpp ActorsRequestBorrowedBlowAt: Adam's own WeaponAction table,
//    its first block -- bullet i107, its sound -- fired from his spot, aimed at the contact as the fist's), and the next
//    blow waits for the fists' cycle (that picture's frames x its rate). The Staff's own fire lock, charge and timing are
//    not touched; a blow is refused while the Staff's own shot is cycling. NOT the original's: the fists' second block
//    (picked by the original's rng at each cycle's end) is not used (its bullet is the same i107; frame 0 instead of 1).
//    [R: the Staff's length 1.20 m from the hand, no model.]
//
// 1. THE SWORD (owner Q6: "a physical swing fires the original's sword attack at the blade, its own rate and damage;
//    the trigger does nothing"). The fists' reader (RSQ Melee's punch reader, roth_gesture.h PunchReader, unchanged:
//    its arming, approach, 0.15 m, 2.0 m/s) fed the BLADE'S STRIKING POINT instead of the palm -- the point a blow
//    is measured at, as RSQ Melee's club measures a gun's own striking points (club.zs:1-12: the stock, the muzzle)
//    -- and the contact looked for along the whole BLADE swept between two tics (BladeSweptBoxContact), not the fist.
//    The blow is the original's attack (trigger_weapon_fire with the Sword equipped: bullet i164, its damage, its
//    rate -- a swing during the last swing's cycle is refused by the original's own fire lock, WC:781), aimed at the
//    contact (roth_actors_engine.cpp ActorsRequestFireAt, the fist's path).
//    NOT FROM A SOURCE (no sword in RSQ Melee): the blade's length and thickness and the striking point's place.
//    [R: kBladeM 0.85 m (an arming sword's blade; no model of i29 yet), the striking point 0.65 of the way to the tip
//    (a blade's centre of percussion lies about a third from the tip), the blade's half-thickness for contact 0.03 m.]
//
// 2. TWO HANDS ON THE SHOTGUN (owner Q9: "TWO-HANDED NOW, the off hand on the fore-end aims it"). The off hand takes
//    the gun exactly as the engine's own support hold does (vk_openxrdevice.cpp:4513-4600, FRIK's test: the off hand
//    ALONG THE BARREL within a cone, between a minimum and a maximum distance, held with the grip in vr_support_mode 3),
//    ported here as SupportLatch so the flat controller-pose test reaches it; while it holds, the shot leaves along
//    the line from the rear (main) hand through the fore (off) hand (TwoHandAim, vk_openxrdevice.cpp:4602-4618).
//    Firing, ammo, magazine, reload: the original's (the shot is the original's trigger_weapon_fire; only its
//    direction comes from the hands). The pump: not built (owner: it waits for real meshes).
//
// 3. BODY SLOTS (owner Q8: "both the inventory and body slots that run the original's select click"). The places and
//    the rules are RSQ Tactical Rig's gun holsters (RSQ_UBQSeries/TacticalRig/zscript/rsqtr/rig.zs, the owner's own
//    holster design), ported for the MAIN hand (owner: right-hand weapons now, left later); what a press does is the
//    original's: a draw is the inventory's weapon click on that weapon (Inventory::EquipLeft, as commit_held_cursor_
//    item's list 1, inventory.c:2183-2192), a weapon put away is activate_weapon_item(0, 0) (the inventory's own way
//    to empty the weapon hand, dialogue_ui.c:1776-1778). See HolsterRig for the checklist.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_gesture.h"

#include <math.h>

namespace roth
{
namespace vrweapon
{

using gesture::V3;

constexpr double kPi = 3.14159265358979323846;   // (no M_PI: MSVC defines it only on request)

//===========================================================================
// Which weapons
//===========================================================================

constexpr int kSword = 29, kShield = 28, kShotgun = 38, kStaff = 12;

// Swung (the blade reader reads it): the Sword (Q6) and the Staff (Q10, owner 2026-11-16: "it is a vr game and we are
// holding a staff, we will be able to swing it"). The original's data marks no weapon "melee" (a short-lived bullet is
// melee in effect, but Adam's fists, Eternity's blades and the Chalice all have one too): the owner named these two.
inline bool IsSwung(int item) { return item == kSword || item == kStaff; }
// Swung and NEVER triggered: the Sword (its attack is the swing's). The Staff's trigger keeps the original's projectile.
inline bool IsTriggerHeldBack(int item) { return item == kSword; }
// A swing whose blow is BORROWED from Adam's own fists (the original's i68 table: bullet i107, damage 100, life 8): the
// Staff, whose original has only its projectile (no melee block) -- owner 2026-11-16, Q10. The Sword's blow is its own.
inline bool SwingBorrowsFists(int item) { return item == kStaff; }
// Held in both hands (Q9).
inline bool IsTwoHanded(int item) { return item == kShotgun; }
// Worn on the off forearm whenever carried (Q7), and so in a hand for the original's in-hand test.
inline bool IsWornOnForearm(int item) { return item == kShield; }

//===========================================================================
// 1. The blade
//===========================================================================

constexpr double kBladeM = 0.85;        // [R] the blade, from the grip to the tip
constexpr double kStaffM = 1.20;        // [R] the Staff, from the main hand's grip to its far end (no model of i12 yet)
inline double SwungLengthM(int item) { return item == kStaff ? kStaffM : kBladeM; }
constexpr double kStrikeAt = 0.65;      // [R] the striking point, as a fraction of the blade from the grip
constexpr double kBladeHalfM = 0.03;    // [R] the blade's half-thickness for contact (the fist's glove is 0.09 + pad)

// The blade's points along `dir` (unit) from the grip `grip`, `len` long (the caller's units).
inline V3 BladeTip(const V3 &grip, const V3 &dir, double len) { return grip + dir * len; }
inline V3 BladeStrike(const V3 &grip, const V3 &dir, double len) { return grip + dir * (len * kStrikeAt); }

struct BladeContact
{
	bool hit = false;
	double dxy = 1e30;   // the nearest the blade came to the box in the plane (0 inside)
	V3 p;                // the blade's point there
	double s = 0;        // along the blade, 0 the grip .. 1 the tip
	double u = 0;        // through the tic, 0 the last tic's blade .. 1 this tic's
};

// Item 17 of the fist (SweptBoxContact, roth_gesture.h) for a BLADE: the segment grip0 -> tip0 at the last tic, grip1
// -> tip1 at this one, swept between them, against the box (lo, hi) grown by `gr` in the plane. Of the swept points
// within `gr` of the box in the plane and inside its height (grown by gr), the FIRST in time (smallest u), and of those
// the nearest to the box: where the blade met it. Sampled: 13 points along the blade, 9 through the tic.
inline BladeContact BladeSweptBoxContact(const V3 &grip0, const V3 &tip0, const V3 &grip1, const V3 &tip1, double gr,
	const V3 &lo, const V3 &hi)
{
	BladeContact best;
	const int ns = 12, nu = 8;
	for (int ku = 0; ku <= nu; ku++)
	{
		const double u = (double)ku / nu;
		const V3 g = grip0 + (grip1 - grip0) * u, t = tip0 + (tip1 - tip0) * u;
		bool found = false;
		for (int ks = 0; ks <= ns; ks++)
		{
			const double s = (double)ks / ns;
			const V3 q = g + (t - g) * s;
			if (q.z < lo.z - gr || q.z > hi.z + gr) continue;
			const double dx = q.x < lo.x ? lo.x - q.x : q.x > hi.x ? q.x - hi.x : 0.0;
			const double dy = q.y < lo.y ? lo.y - q.y : q.y > hi.y ? q.y - hi.y : 0.0;
			const double d = sqrt(dx * dx + dy * dy);
			if (d > gr) continue;
			if (!found || d < best.dxy) { best.hit = true; best.dxy = d; best.p = q; best.s = s; best.u = u; found = true; }
		}
		if (found) return best;
	}
	return best;
}

//===========================================================================
// 2. Two hands (vk_openxrdevice.cpp:4513-4618, ported)
//===========================================================================

// The engine's own settings' defaults (hw_vrmodes.cpp:1060-1083); the caller passes the live values.
struct SupportSettings
{
	double coneEnter = 0.955;   // vr_support_cone_enter (~17 degrees)
	double coneExit = 0.900;    // vr_support_cone_exit (~26 degrees)
	double distMin = 0.15;      // vr_support_dist_min, metres
	double distMax = 0.90;      // vr_support_dist_max
	int mode = 3;               // vr_support_mode: 0 off, 1 auto, 2 auto + button, 3 hold, 4 toggle
};

struct SupportLatch
{
	bool engaged = false;
	bool rearmRequired = false;
	bool gripWas = false;
	double along = -1.0;        // this tic's cosine (a log line)
	double dist = 0.0;          // this tic's hand separation, metres

	void Reset() { engaged = false; rearmRequired = false; gripWas = false; }

	// One tic. `barrel`: the weapon's forward (unit); `toOff`: the off hand minus the main hand, metres (real: the
	// vertical already taken back by the level's stretch); `gripNow`: the off hand's grip. Returns engaged.
	bool Feed(const V3 &barrel, const V3 &toOff, bool gripNow, const SupportSettings &st)
	{
		const double toOffLen = toOff.Length();                                       // :4520
		along = -1.0;
		if (toOffLen > 0.0001) along = barrel.Dot(toOff) / toOffLen;                   // :4522-4526
		dist = toOffLen;
		const bool bandOk = toOffLen >= st.distMin && toOffLen <= st.distMax;          // :4531
		const bool inEnterCone = bandOk && along >= st.coneEnter;                      // :4533
		const bool inExitCone = bandOk && along >= st.coneExit;                        // :4534
		const int mode = st.mode < 0 ? 0 : st.mode > 4 ? 4 : st.mode;                  // :4541
		const bool gripEdge = gripNow && !gripWas;                                     // :4543-4545
		gripWas = gripNow;
		if (mode == 0) { engaged = false; rearmRequired = false; }                     // :4550-4554
		else if (engaged)                                                              // :4555-4567
		{
			if ((mode == 1 || mode == 2) && !inExitCone) engaged = false;
			else if (mode == 3 && !gripNow) engaged = false;
			else if (mode == 4 && gripEdge) engaged = false;
			else if (mode == 2 && gripEdge) { engaged = false; rearmRequired = true; }
		}
		else                                                                           // :4568-4582
		{
			if (rearmRequired && !inExitCone) rearmRequired = false;
			const bool mayTake = inEnterCone && !rearmRequired;
			if (mayTake)
			{
				if (mode == 1 || mode == 2) engaged = true;
				else if (mode == 3 && gripNow) engaged = true;
				else if (mode == 4 && gripEdge) engaged = true;
			}
		}
		return engaged;
	}
};

// The aim along the line from the rear hand to the fore hand (metres, z up): yaw (degrees, atan2 in the plane, the
// map's convention: 0 along +x, counter-clockwise) and pitch (degrees, POSITIVE UP). False when the hands stand nearly
// one above the other (:4607-4617: less than 0.05 m apart in the plane, or 0.05 m apart at all), and the caller keeps
// the one-hand aim.
inline bool TwoHandAim(const V3 &rear, const V3 &fore, double &yawDeg, double &pitchDeg)
{
	const V3 d = fore - rear;
	const double run = d.LengthXY();
	if (run <= 0.05 || d.Length() <= 0.05) return false;
	yawDeg = atan2(d.y, d.x) * (180.0 / kPi);
	pitchDeg = atan2(d.z, run) * (180.0 / kPi);
	return true;
}

//===========================================================================
// 3. Body slots: RSQ Tactical Rig's gun holsters (rig.zs), for the main hand
//===========================================================================
//
// PORT CHECKLIST (rig.zs = TacticalRig/zscript/rsqtr/rig.zs, eb0136239e; port = this file unless named):
//  1. the gun holsters: places 0 1 (hips), 4 5 (shoulders), 6 7 (chest)    rig.zs:849 IsGunRegion -> kPlaces
//  2. their defaults, metres from the head in the body frame (forward, left, up, radius)
//                                                 rig.zs:86-89 DF/DL/DU/DR -> kPlaces (the same numbers)
//  3. body scale 1 (no "Measure my height" here)  rig.zs:440-445 BodyScale -> NOT PORTED: scale 1, as the rig before a
//     measurement [the Realms eye is 1.65 m, rig.zs's REF_EYE 1.62: 2% -- not applied]
//  4. distance: room metres, the vertical through the pixel stretch       body.zs RoomMetres -> the caller's metres
//  5. the hand inside a place: d <= radius; of several, a place the hand can use first (a gun in the hand: any gun
//     holster; an empty hand: one with a gun in it, SpotHolds), then the nearest centre
//                                                 rig.zs:2432-2466 -> HolsterRig::Pick
//  6. entering a place: a buzz 0.18 for 3 tics, at most every 10 tics; hoverT from 0, a pending press dropped
//                                                 rig.zs:908-909, 2481-2488 -> Step (Event::enter)
//  7. the press: the grip's edge                  rig.zs:2492-2494 -> Step (Use: not ported, Realms' use is the grip)
//  8. free a holster: an empty hand pressed in an empty, assigned gun holster and held there 35 tics frees it
//                                                 rig.zs:2546-2549, 2636-2652 FreeTick -> Step / Free
//  9. dwell: the press counts once the hand has been there 3 tics (rsqtr_dwell); a gun in the hand at a full holster
//     acts at once (quick swap)                   rig.zs:2551-2556 -> Step
// 10. a press waits for the dwell; letting go drops it                    rig.zs:2558-2562 -> Step
// 11. Press, the main hand at a gun holster (rig.zs:3045-3215, 2096-2200 Holster, 872-878 MayEnter, 1013 SwapGun), with
//     the swap mode "Into the Wheel" (rsqtr_swap 0, the default):
//       a gun in the hand: at its own holster -> in (put away); at another gun's full holster -> that gun comes to the
//       hand, this one goes home (or stays carried, "the Wheel", with no home); with no home at an unassigned empty
//       holster -> in, and the holster becomes its own (Assign); at another gun's EMPTY holster with no home ->
//       refused (Refused: double pulse); with a home elsewhere at an empty holster -> it goes home (put away)
//       an empty hand: a full holster -> its gun is drawn; an empty one -> refused (EmptyRefused)
//       a weapon with its own home (rig.zs:859-871: the ShieldSaw's shield and kin) never takes a gun holster: here
//       the Shield, worn on the forearm (Q7)
// 12. feel: a draw 0.65 for 6 tics, a put-away 0.55 for 5, a refusal two pulses of 0.3 for 3 tics 4 apart
//                                                 rig.zs:910-921, body.zs:782-788 -> roth_vrweapons.cpp Feel
// NOT PORTED: the off hand's holsters and drawing into the off hand (owner: left-hand support later); the grenade spot,
// keyring, sabers, whip, flare, pouch, cigarette; the rings, the pictures, the streaks, the ghost hand, the struggle
// hint, edit mode, profiles, the seated layout, the torso that lags the head (RSQTR_Body: here the player's yaw), the
// recentre, "ride up when down", the shoulder grace for lost tracking (rig.zs:2583-2630), the throw, saving the
// assignments in settings (they live for the session; the engine's archived settings are never written).

enum { kHolsterCount = 6 };
struct Place { int rigIndex; const char *name; double f, l, u, r; };
constexpr Place kPlaces[kHolsterCount] =
{
	{ 0, "Left hip",        0.00,  0.18, -0.70, 0.14 },
	{ 1, "Right hip",       0.00, -0.18, -0.70, 0.14 },
	{ 4, "Left shoulder",  -0.08,  0.18, -0.10, 0.14 },
	{ 5, "Right shoulder", -0.08, -0.18, -0.10, 0.14 },
	{ 6, "Left chest",      0.10,  0.12, -0.46, 0.11 },
	{ 7, "Right chest",     0.10, -0.12, -0.46, 0.11 },
};

constexpr double kEnterAmp = 0.18;  constexpr int kEnterTics = 3;       // rig.zs:908-909
constexpr int kEnterCool = 10;                                          // rig.zs:2485
constexpr int kDwellTics = 3;                                           // rsqtr_dwell (rig.zs:2553)
constexpr int kFreeTics = 35;                                           // rig.zs:2636

// What a press asks of the game (roth_vrweapons.cpp does it with the original's calls).
enum HolsterAct
{
	HOLSTER_NONE,
	HOLSTER_ENTER,       // the hand entered place `place` (a buzz)
	HOLSTER_STOW,        // the held weapon put away into `place` (activate_weapon_item(0, 0)); `assigned` when it became its own
	HOLSTER_DRAW,        // `item` drawn from `place` into the main hand (the inventory's weapon click)
	HOLSTER_SWAP,        // `item` drawn from `place`; the held weapon goes home or stays carried
	HOLSTER_REFUSE,      // nothing (a refusal)
	HOLSTER_FREE,        // `place`'s assignment (to `item`) cleared
};
struct HolsterEvent
{
	HolsterAct act = HOLSTER_NONE;
	int place = -1;
	int item = 0;
	bool assigned = false;
	const char *why = "";
};

// The game's side of the press: what the hand holds and what is carried.
struct HolsterWorld
{
	int held = 0;                                   // the weapon in the main hand (0: his fists)
	bool (*carried)(int item, void *ctx) = nullptr; // is `item` carried (and not hidden)
	void *ctx = nullptr;
	bool Carried(int item) const { return item > 0 && carried != nullptr && carried(item, ctx); }
};

struct HolsterRig
{
	int owner[kHolsterCount] = {};   // the weapon each gun holster is assigned to (lockCls), 0 none
	int hover = -1, hoverT = 0;
	bool pendPress = false, pressWas = false;
	int freeR = -1, freeT = 0;
	int buzzCool = 0;

	void ResetHand() { hover = -1; hoverT = 0; pendPress = false; pressWas = false; freeR = -1; freeT = 0; }

	int HomeOf(int item) const { for (int r = 0; r < kHolsterCount; r++) if (item > 0 && owner[r] == item) return r; return -1; }
	// Content: the gun in holster r -- its own weapon, while carried and not in the hand.
	int Content(int r, const HolsterWorld &w) const { const int x = owner[r]; return (x > 0 && x != w.held && w.Carried(x)) ? x : 0; }
	// MayEnter (rig.zs:872-878): no own home elsewhere (the Shield), r unassigned or its own, and nothing else in it.
	bool MayEnter(int item, int r, const HolsterWorld &w) const
	{
		if (IsWornOnForearm(item)) return false;
		if (owner[r] != 0 && owner[r] != item) return false;
		const int c = Content(r, w);
		return c == 0 || c == item;
	}
	// Assign (rig.zs:880-886): `item` is assigned to r and to no other.
	void Assign(int item, int r)
	{
		if (IsWornOnForearm(item)) return;
		for (int k = 0; k < kHolsterCount; k++) if (owner[k] == item) owner[k] = 0;
		owner[r] = item;
	}

	// Item 5: the place the hand is in (metres in the body frame from the head), or -1.
	int Pick(double f, double l, double u, const HolsterWorld &w) const
	{
		int r = -1, bestRank = -1;
		double best = 1e9;
		const bool gun = w.held > 0;
		for (int i = 0; i < kHolsterCount; i++)
		{
			const Place &p = kPlaces[i];
			const double d = sqrt((f - p.f) * (f - p.f) + (l - p.l) * (l - p.l) + (u - p.u) * (u - p.u));
			if (d > p.r) continue;
			const int rank = gun ? 1 : (Content(i, w) != 0 ? 1 : 0);
			if (rank > bestRank || (rank == bestRank && d < best)) { bestRank = rank; best = d; r = i; }
		}
		return r;
	}

	// Item 11.
	HolsterEvent Press(int r, const HolsterWorld &w)
	{
		HolsterEvent e;
		e.place = r;
		if (w.held > 0)
		{
			const int g = w.held;
			const int home = HomeOf(g);
			const int there = Content(r, w);
			const int swapW = there != 0 && there != g ? there : 0;                     // SwapGun (the gun there)
			const bool toWheel = swapW != 0 && home < 0 && !MayEnter(g, r, w);          // rig.zs:2131
			if (!toWheel && home < 0 && !MayEnter(g, r, w))                             // rig.zs:2133
			{
				e.act = HOLSTER_REFUSE;
				e.why = IsWornOnForearm(g) ? "it has its own place (the off forearm)" : "another weapon's holster";
				return e;
			}
			if (toWheel) { e.act = HOLSTER_SWAP; e.item = swapW; e.why = "the held weapon stays carried (no holster of its own)"; return e; }
			if (home >= 0 && home != r)                                                   // rig.zs:2161-2182
			{
				if (swapW != 0) { e.act = HOLSTER_SWAP; e.item = swapW; e.why = "the held weapon goes to its own holster"; return e; }
				e.act = HOLSTER_STOW; e.place = home; e.item = g; e.why = "not its holster: it goes to its own"; return e;
			}
			const bool fresh = owner[r] == 0;                                             // in it goes (Assign)
			Assign(g, r);
			e.act = HOLSTER_STOW; e.item = g; e.assigned = fresh;
			e.why = fresh ? "an empty holster: it becomes this weapon's" : "its own holster";
			return e;
		}
		const int there = Content(r, w);
		if (there != 0) { e.act = HOLSTER_DRAW; e.item = there; e.why = "its weapon is drawn"; return e; }
		e.act = HOLSTER_REFUSE;
		e.why = owner[r] != 0 ? "its weapon is not carried, or is in the hand" : "nothing in it";
		return e;
	}

	// One tic of the main hand: where it is (metres in the body frame) and its grip. At most one event a tic (the press
	// or the free wins over the entry's buzz, which is returned only when nothing else happened).
	HolsterEvent Step(bool haveHand, double f, double l, double u, bool gripNow, const HolsterWorld &w)
	{
		HolsterEvent out;
		if (buzzCool > 0) buzzCool--;
		if (!haveHand) { ResetHand(); return out; }
		const int r = Pick(f, l, u, w);
		HolsterEvent enter;
		if (r != hover)                                                                   // item 6
		{
			if (r >= 0 && buzzCool <= 0) { enter.act = HOLSTER_ENTER; enter.place = r; buzzCool = kEnterCool; }
			hover = r;
			hoverT = 0;
			pendPress = false;
		}
		else if (r >= 0) hoverT++;
		const bool pressNow = gripNow;
		const bool edge = pressNow && !pressWas;                                          // item 7
		pressWas = pressNow;
		if (edge && r >= 0) { freeR = r; freeT = 0; }                                     // item 8
		if (!pressNow || r != freeR) freeR = -1;
		else if (freeR >= 0 && ++freeT >= kFreeTics)
		{
			const int fr = freeR;
			freeR = -1;
			if (w.held == 0 && owner[fr] != 0 && Content(fr, w) == 0)
			{
				out.act = HOLSTER_FREE; out.place = fr; out.item = owner[fr]; out.why = "an empty hand held 1 s in an empty assigned holster";
				owner[fr] = 0;
				return out;
			}
		}
		int dwell = kDwellTics;                                                           // item 9
		if (r >= 0 && Content(r, w) != 0 && w.held > 0) dwell = 0;
		if (edge && r >= 0) pendPress = true;                                             // item 10
		if (!pressNow) pendPress = false;
		if (pendPress && r >= 0 && hoverT >= dwell) { pendPress = false; return Press(r, w); }
		return enter;
	}
};

} // namespace vrweapon
} // namespace roth
