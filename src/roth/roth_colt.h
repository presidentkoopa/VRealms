#pragma once
//
// roth_colt.h -- THE COLT .45 (i9) AS A REAL M1911 IN VR, ENGINE-FREE: the rules a physical action must obey, the
// real-parts reload as a state machine, and which frame of the mesh's own take each moving part shows. What each
// answer DOES is roth_colt.cpp's: the game-logic effect is always the original's own call (trigger_weapon_fire), never
// a new one. tools/colt_test runs everything here on scripted hands against the original's rules.
//
// THE OWNER (2026-11-13, W2): "I will find high quality meshes and we will do real part reloading. Nothing sprite
// layer, real drop mag, rack slide. World object collision, etc." (2026-11-16): the mesh, Sketchfab "Colt 1911" by
// AlexChuchvaga, CC BY 4.0 (vrealms_iwad/models/weapons/colt1911/CREDITS.txt).
//
// THE ORIGINAL'S COLT (read from the retail DBASE100 by ParseWeaponActions, proven against the running original by
// tools/actors_test weaponcmp; the oracle run of 2026-11-16 in tools/colt_test/README.txt):
//   one WeaponAction block: bullet i61, its shot on frame `actions[0].D(0x0c)`, rate 5, an 8-frame fire picture
//   (40 Realms ticks a shot), magazine (cap) 12 in the Colt's own inventory word (slot +2), ammo item i51 (the
//   Colt-45 magazine, op 0x20), a reload picture of 29 frames at rate 8 (232 ticks).
//   THE RELOAD (trigger_weapon_fire, weapon_combat.c:773-909): a pull with the magazine word EMPTY and the ammo item
//   carried (g_active_weapon_ammo != 0) does remove_item(i51) (WC:849) and adds the cap to the word (WC:851-856:
//   0 + 12 = 12), then plays the reload picture with NO shot (WC:859-866: pending bullet 0, its rate +0x40, its sound
//   +0x34). With the word empty and NO i51: message 6 and equip_first_usable_weapon (WC:826-833). The original has no
//   partial magazines and no reload while rounds remain: the reload exists only as that pull.
//
// THE RULES THIS PORT KEEPS (each one the original's, the physical action only choosing WHEN):
//   R1  The magazine is released only when the original's word is 0 and no shot is cycling (lock 0). A release with
//       rounds in it is refused (a double pulse): the original has no partial magazine to drop, so no round is ever
//       lost or kept apart from the word.
//   R2  A magazine is taken from the body (the belt pouch) only when the gun has none and i51 is carried
//       (query_player_inventory > 0). Taking it changes nothing in the game: the i51 stays in the inventory.
//   R3  A magazine let go of before it seats goes back to the pouch: no world object, nothing consumed.
//   R4  THE MOMENT THE NEW MAGAZINE SEATS IS THE ORIGINAL'S RELOAD: the engine pulls the original's own trigger once
//       (trigger_weapon_fire), which with the word 0 and i51 carried is exactly WC:843-866 -- one i51 removed, the
//       word 0 + 12, the reload picture's 232-tick lock, its sound, no shot. Why the seat and not the rack: WC:849
//       is the magazine leaving the inventory and entering the gun; the rack only chambers a round of a magazine
//       already there (in the original's terms nothing: the word counts every round). Seated with a shot still
//       cycling (lock != 0) the seat waits until the lock clears.
//   R5  While the Colt is in the hand in VR with i51 carried, a pull on an EMPTY word never reaches the original's
//       trigger (the dry click): the original's automatic reload is replaced by the hands' and must not also fire.
//       With NO i51 carried the pull IS the original's (message 6, the next usable weapon), as in flat.
//   R6  A pull reaches the original's trigger with rounds in the word only when a magazine is in the grip and the
//       slide is forward on a chambered round (racked or released after a seat). Otherwise it is a dry click.
//   R7  The slide: locked back after the shot that empties the word (as a real 1911's slide stop on an empty
//       magazine's follower); forward again by the rack (the off hand pulls it back past 90 % of its travel and lets
//       go) or by the slide release. A rack with a round chambered ejects nothing: the original's word has no
//       chambered round apart from the magazine (presentation only, written down).
//   Flat play: none of this; the original's automatic reload on the pull and its own pictures (1:1).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_actors.h"

#include <cmath>
#include <cstdint>

namespace roth
{
namespace colt
{

constexpr int kColt = 9;          // DBASE100 i9 "Colt .45. Semi-automatic handgun."
constexpr int kMagazine = 51;     // i51 "Colt-45 magazine", the Colt's ammo item (its WeaponAction op 0x20)

// ---- the mesh's own take (Anim.fbx 'CINEMA_4D_Main', 201 frames at 30 fps), the frames each part is drawn from ----
// Measured with tools/colt_test (colt_take): the trigger presses 110-113, the hammer falls 114-115, the slide recoils
// 116-119 (60 file units a frame, 180 at 119), stands at 150 on frame 120, closes by 125; the top round (Bullet_2)
// climbs into the chamber 121-125; the magazine release presses 1-7; the hammer is cocked (68.5 deg) 85-113, 118-.
constexpr double kTakeReady = 110.0;       // trigger forward, hammer cocked, slide forward
constexpr double kTakeTriggerBack = 113.0;
constexpr double kTakeHammerDown = 115.0;
constexpr double kTakeSlideStart = 116.0;  // the slide still forward, the round gone
constexpr double kTakeSlideBack = 119.0;   // full recoil (180 units)
constexpr double kTakeSlideLock = 120.0;   // on the slide stop (150 units)
constexpr double kTakeCycleEnd = 125.0;
constexpr double kTakeMagButtonIn = 7.0;
constexpr double kTakeRest = 0.0;

// ---- what the original's weapon is doing this tic (ActorWorld, read only) ----
struct Game
{
	bool colt = false;        // the Colt is the weapon in the hand (0x81038 holds i9)
	int qty = 0;              // its magazine word (slot +2): rounds left
	int mags = 0;             // i51 carried (g_active_weapon_ammo, query_player_inventory)
	int lock = 0;             // the fire lock (0x7fdd0)
	bool firing = false;      // a shot cycle (not the reload picture) is running
	double framePos = 0;      // the cycle's position in picture frames (counter / rate)
	int frames = 8;           // the fire picture's frames
	int shotFrame = 1;        // its WeaponBulletDelay (attrs +0x0c)
	bool shotLeft = false;    // the shot left this tic (WE_SHOT)
	bool cycleEnded = false;  // WE_CYCLE_END
	bool equipped = false;    // WE_EQUIP this tic (the Colt drawn again)
	bool reloaded = false;    // WE_RELOAD this tic (the original's reload ran)
};

// What the original's weapon (ActorWorld::weapon) is doing, for the rig: `qty` the held slot's word (the host's),
// `frames` its picture's frame count (the host's WeaponFrames), `ev` this tic's WE_* events.
inline Game GameOf(const ActorWorld &w, int qty, int frames, uint32_t ev)
{
	Game g;
	const ActorWorld::PlayerWeapon &pw = w.weapon;
	g.colt = pw.item0 == kColt && pw.held && pw.heldSlot >= 0;
	if (!g.colt) return g;
	g.qty = (int16_t)qty;
	g.mags = pw.ammo;
	g.lock = pw.lock;
	const bool reloadPicture = pw.pendingDef == 0 && pw.actions[0].D(0x2c) != 0 && pw.sprite == pw.actions[0].D(0x2c);
	g.firing = pw.lock != 0 && !reloadPicture;
	const int rate = pw.rate != 0 ? pw.rate : 8;
	g.framePos = pw.lock == -1 ? 0.0 : (double)(int16_t)pw.counter / rate;
	g.frames = frames;
	const int e = pw.active < 0 ? 0 : pw.active;
	g.shotFrame = pw.actions[e].D(0x0c);
	g.shotLeft = (ev & ActorWorld::WE_SHOT) != 0;
	g.cycleEnded = (ev & ActorWorld::WE_CYCLE_END) != 0;
	g.equipped = (ev & ActorWorld::WE_EQUIP) != 0;
	g.reloaded = (ev & ActorWorld::WE_RELOAD) != 0;
	return g;
}

// ---- the hands this tic (the engine measures; VR only) ----
struct Hands
{
	bool vr = false;              // the real-parts reload is on (a headset or the controller-pose test)
	bool offAtMagButton = false;  // the off hand's index tip on the magazine release
	bool magButtonPress = false;  // ... and pressing (the trigger's edge, or the reload input's edge)
	bool reloadInput = false;     // the reload input (an edge): releases the magazine as the button does
	bool offGrip = false;         // the off hand's grip held
	bool offAtPouch = false;      // the off hand in the belt pouch
	bool magAtWell = false;       // the carried magazine's top in the magwell, aligned (the snap)
	bool offAtSlide = false;      // the off hand on the slide's rear serrations
	double slideAlong = 0;        // metres the off hand has moved back along the bore since it took the slide
	bool slideRelease = false;    // the slide release input (an edge)
};

enum Act
{
	ACT_NONE = 0,
	ACT_DROP_MAG,       // the magazine falls from the grip: a world object (presentation only)
	ACT_REFUSE_DROP,    // R1: rounds in the word
	ACT_TAKE_MAG,       // a magazine taken from the pouch into the off hand
	ACT_REFUSE_TAKE,    // R2: none carried, or the gun has one
	ACT_RETURN_MAG,     // R3: let go before it seated
	ACT_SEAT,           // R4: seated -> the engine pulls the original's trigger once (the reload)
	ACT_SEAT_WAIT,      // seated while a shot cycles: the reload waits for the lock
	ACT_RACK_BACK,      // the slide pulled to the back
	ACT_SLIDE_FORWARD,  // the slide forward (rack let go, or the release): chambers when the word has rounds
	ACT_DRY,            // a pull refused (R5/R6): the hammer's click, no game effect
};

struct Event { Act act = ACT_NONE; const char *why = ""; };

// The real parts' state (presentation; the game's numbers are only ever the original's word and inventory).
struct Rig
{
	bool magIn = true;          // a magazine in the grip
	bool magInHand = false;     // the off hand carries one from the pouch
	bool slideLocked = false;   // back on the slide stop
	bool chambered = true;      // a round in the chamber (drawn when the word has rounds)
	bool seatPending = false;   // seated while a shot cycled: the reload waits (R4)
	bool racking = false;       // the off hand holds the slide
	bool rackedBack = false;    // ... and has pulled it past 90 %
	double rack = 0;            // the slide's travel under the off hand, 0..1
	int magButtonTics = 0;      // the release drawn pressed
	bool lockAfterCycle = false;// this cycle's shot emptied the word: the slide stays back at its end
	bool gripWas = false;

	// The Colt drawn again (WE_EQUIP): the parts as the word says.
	void Reset(const Game &g)
	{
		magIn = true; magInHand = false; seatPending = false; racking = rackedBack = false; rack = 0;
		slideLocked = g.qty == 0; chambered = g.qty > 0; lockAfterCycle = false; magButtonTics = 0;
	}
};

// R5 / R6: does a trigger pull reach the original's trigger_weapon_fire this tic?
inline bool PullReachesOriginal(const Rig &r, const Game &g, bool vr)
{
	if (!vr || !g.colt) return true;                        // flat: the original, 1:1
	if (g.qty <= 0) return g.mags <= 0;                     // R5: with i51 carried the reload is the hands'
	return r.magIn && !r.slideLocked && r.chambered && !r.racking;   // R6
}

// One tic of the rig: the original's events first (the shot locks the slide on the last round), then the hands.
// At most one hand event a tic.
inline Event Step(Rig &r, const Game &g, const Hands &h, double travelMetres)
{
	Event out;
	if (!g.colt) { r.gripWas = h.offGrip; return out; }
	if (g.equipped) r.Reset(g);
	if (g.shotLeft) { r.lockAfterCycle = g.qty == 0; r.chambered = g.qty > 0; }
	if (g.cycleEnded && r.lockAfterCycle) { r.slideLocked = true; r.lockAfterCycle = false; }
	if (g.reloaded && !r.magIn) r.magIn = true;            // the original reloaded on its own (flat, or no hands)
	if (r.magButtonTics > 0) r.magButtonTics--;
	if (!h.vr) { r.gripWas = h.offGrip; return out; }
	const bool gripEdge = h.offGrip && !r.gripWas;
	const bool gripLet = !h.offGrip && r.gripWas;
	r.gripWas = h.offGrip;

	// R4: a seat that waited for the shot's lock
	if (r.seatPending)
	{
		if (g.lock == 0) { r.seatPending = false; out.act = ACT_SEAT; out.why = "seated (the shot's lock cleared): the original's reload"; }
		return out;
	}
	// R1: the magazine release
	if (r.magIn && ((h.offAtMagButton && h.magButtonPress) || h.reloadInput))
	{
		r.magButtonTics = 10;
		if (g.qty > 0) { out.act = ACT_REFUSE_DROP; out.why = "rounds in the magazine (the original has no partial magazines)"; return out; }
		if (g.lock != 0) { out.act = ACT_REFUSE_DROP; out.why = "a shot is cycling"; return out; }
		r.magIn = false;
		out.act = ACT_DROP_MAG; out.why = "the empty magazine falls free";
		return out;
	}
	// R3 / R4: the magazine in the off hand
	if (r.magInHand)
	{
		if (!h.offGrip) { r.magInHand = false; out.act = ACT_RETURN_MAG; out.why = "let go before it seated: back to the pouch"; return out; }
		if (h.magAtWell && !r.magIn)
		{
			r.magInHand = false;
			r.magIn = true;
			// a seat on a forward slide with no round chambered needs the rack; on a locked slide the release or the rack
			if (g.qty > 0) { out.act = ACT_SEAT; out.why = "seated"; return out; }   // (no reload: the word has rounds -- not reachable by R1)
			if (g.lock != 0) { r.seatPending = true; out.act = ACT_SEAT_WAIT; out.why = "seated while a shot cycles"; return out; }
			out.act = ACT_SEAT; out.why = "seated: the original's reload";
			return out;
		}
		return out;
	}
	// R2: a magazine from the pouch
	if (gripEdge && h.offAtPouch)
	{
		if (r.magIn) { out.act = ACT_REFUSE_TAKE; out.why = "the gun has a magazine"; return out; }
		if (g.mags <= 0) { out.act = ACT_REFUSE_TAKE; out.why = "no Colt-45 magazine carried"; return out; }
		r.magInHand = true;
		out.act = ACT_TAKE_MAG; out.why = "a magazine from the pouch (still in the inventory)";
		return out;
	}
	// R7: the slide
	if (!r.racking && gripEdge && h.offAtSlide) { r.racking = true; r.rackedBack = false; r.rack = r.slideLocked ? 1.0 : 0.0; }
	if (r.racking)
	{
		double a = travelMetres > 0 ? h.slideAlong / travelMetres : 0;
		if (r.slideLocked && a < 1.0) a = 1.0;                // a locked slide starts at the back
		r.rack = a < 0 ? 0 : (a > 1 ? 1 : a);
		if (!r.rackedBack && r.rack >= 0.9) { r.rackedBack = true; out.act = ACT_RACK_BACK; out.why = "the slide pulled back"; }
		if (gripLet || !h.offGrip)
		{
			r.racking = false;
			r.rack = 0;
			if (r.rackedBack)
			{
				r.rackedBack = false;
				// a real 1911 on an empty magazine locks back again; with no magazine it closes on an empty chamber
				if (r.magIn && g.qty == 0) { r.slideLocked = true; r.chambered = false; out.act = ACT_SLIDE_FORWARD; out.why = "racked on an empty magazine: locked back again"; }
				else { r.slideLocked = false; r.chambered = r.magIn && g.qty > 0; out.act = ACT_SLIDE_FORWARD; out.why = r.chambered ? "racked: a round chambered" : "racked: nothing to chamber"; }
			}
			return out;
		}
		return out;
	}
	if (h.slideRelease && r.slideLocked && r.magIn && g.qty > 0)
	{
		r.slideLocked = false; r.chambered = true;
		out.act = ACT_SLIDE_FORWARD; out.why = "the slide release: a round chambered";
	}
	return out;
}

// ---- which take frame each part shows ----
struct Parts
{
	double slide = kTakeReady, barrel = kTakeReady, hammer = kTakeReady, trigger = kTakeReady;
	double magButton = kTakeRest;
	double topRound = kTakeRest;      // Bullet_2 (the magazine's top round)
	bool magShown = true, topRoundShown = true, chamberShown = true;   // Magaz; Bullet_2; Bullet + Gils (the chambered round)
};

inline double Lerp(double a, double b, double t) { return a + (b - a) * (t < 0 ? 0 : (t > 1 ? 1 : t)); }

// The fire cycle in the original's own frames: before the shot frame the trigger presses and the hammer falls
// (take 110 -> 115); from the shot to the cycle's end the slide recoils and closes (take 115 -> 125), stopping on the
// slide stop (take 120) when this shot emptied the word.
inline Parts PartsFor(const Rig &r, const Game &g)
{
	Parts p;
	p.magShown = r.magIn;
	p.topRoundShown = r.magIn && g.qty >= 2;
	p.chamberShown = r.chambered && g.qty > 0 && !r.slideLocked;
	if (r.magButtonTics > 0) p.magButton = kTakeMagButtonIn;
	if (r.slideLocked) { p.slide = p.barrel = kTakeSlideLock; p.chamberShown = false; }
	if (g.colt && g.firing && g.frames > 0)
	{
		const double b = g.shotFrame > 0 ? g.shotFrame : 1;
		const double f = g.framePos;
		if (f < b)
		{
			const double t = Lerp(kTakeReady, kTakeHammerDown - 1.0, f / b);   // 110 .. 114
			p.trigger = t; p.hammer = t; p.slide = p.barrel = kTakeReady;
		}
		else
		{
			const double n = g.frames > b ? g.frames - b : 1;
			double t = Lerp(kTakeHammerDown, kTakeCycleEnd, (f - b) / n);
			p.hammer = t; p.trigger = t < kTakeSlideStart ? kTakeTriggerBack : kTakeReady;
			const bool lock = r.lockAfterCycle || g.qty == 0;
			p.slide = p.barrel = (lock && t > kTakeSlideLock) ? kTakeSlideLock : t;
			p.chamberShown = false;                                 // the round left; its case is the world's now
			p.topRoundShown = r.magIn && g.qty >= 1;
			p.topRound = (r.magIn && g.qty >= 1) ? t : kTakeRest;  // the next round climbs into the chamber
		}
	}
	if (r.racking) p.slide = p.barrel = Lerp(kTakeSlideStart, kTakeSlideBack, r.rack);
	return p;
}

} // namespace colt
} // namespace roth
