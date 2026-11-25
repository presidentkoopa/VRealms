// roth_gestures.cpp -- see roth_gestures.h (what each gesture does) and roth_gesture.h (the detectors, and the
// port checklist of RSQ Melee's punch reader).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roth_gestures.h"
#include "roth_held.h"
#include "roth_gesture.h"
#include "roth_vrweapon.h"
#include "roth_vrweapons.h"
#include "roth_handclick.h"
#include "roth_vrpick.h"
#include "roth_runtime.h"
#include "roth_objects.h"
#include "roth_hands.h"
#include "roth_actors_engine.h"
#include "roth_interface.h"
#include "roth_inventory.h"
#include "roth_das.h"
#include "roth_raw.h"

#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "hw_vrmodes.h"
#include "r_data/vr_handpose.h"
#include "r_data/vr_handpose_math.h"

#include <math.h>
#include <algorithm>
#include <vector>

EXTERN_CVAR(Bool, roth_vrpick_debug)   // roth_vrpick.cpp; file scope, never inside a namespace

namespace roth
{

namespace
{

using namespace gesture;

// THE WARDS (owner Q31): the haptic tingle near a ward, followed while it is shown.
struct WardKind { const char *map; bool shared; int art; const char *what; };
const WardKind kWardKinds[] =
{
	{ "STUDY1", false, 4207, "the green pentagram DEMO[4207]" },
	{ "STUDY1", false, 4208, "the tan pentagram DEMO[4208]" },
	{ nullptr,  true,  232,  "the green sigil ADEMO[232]" },
	// Chapter 2 (owner Q31): Aelf's wards only ("This symbol's glowing. / I believe it's designed to keep us in"),
	// shown by Florentine's trap (AELF #3); not the Study's green sigils ADEMO[235] (no line says they glow).
	{ "AELF",   false, 4273, "Aelf's ward DEMO4[4273]" },
};

//===========================================================================
// State
//===========================================================================

struct Thing
{
	AActor *mo = nullptr;
	int sector = -1, index = -1;
	bool shared = false;
	int art = -1;
	DVector3 lo, hi;
	bool creature = false;   // lo / hi are the original's body (ActorsCreatureBody), not the picture's pick box
};

struct Motion { bool on = false; double from[3] = {}, to[3] = {}; int start = 0, tics = 0; };

struct HandG
{
	PunchReader punch;
	// THE BLADE (chapter 2 Q6, roth_vrweapon.h 1): the fists' reader fed the striking point of the Sword in the main hand
	PunchReader blade;
	bool bladePrev = false;
	DVector3 bladeGrip, bladeTip;   // last tic's blade
	int bladePrevTic = 0;
	bool havePrev = false;
	DVector3 prevPalm, prevTip;
	gesture::V3 prevRelPalm, prevRelTip;
	int prevTic = 0;
	int wardTic = -1000;
	int loggedWardCm = -1;
	int placedUntil = -1;   // the flat test placed this hand (reach, look, home, a move in 0 tics): a jump until then
	Motion motion;
	// THE REACH HELD (roth_gesturetest reach): the original's movement owns the player (roth_playermove_engine) and
	// pushes him out of objects, creatures and door leaves after a placement (the PC run of 367c710c: the creature
	// punch, the sofa, the typewriter, the flies, the door search, the switch all missed so). So the player stands
	// where the original's collision lets him and the TEST HAND reaches the rest: every tic until a move starts, the
	// hand's place (base, camera-relative metres) is corrected so its palm or fingertip is at holdWant. A move is
	// then relative to that base.
	bool hold = false, holdTip = false;
	DVector3 holdWant;
	double base[3] = { 0, 0, 0 };
	int holdLogged = 0;
};

// A punched creature, followed for a while so the log says what the original's attack did to it.
struct Watch { int sector = -1, index = -1, until = 0; int32_t hp = 0; uint8_t st = 0; };

struct State
{
	FLevelLocals *level = nullptr;
	std::vector<Watch> watches;
	bool built = false;
	struct WardRef { int sector, index; const char *what; };
	std::vector<WardRef> wards;             // by object, so a ward hidden at load (AELF's, shown by its trap) is followed
	HandG hand[2];
} S;

const char *RoleName(int role) { return role == VR_MAINHAND ? "main" : "off"; }
bool Logging() { return VRPickTesting() || roth_vrpick_debug; }

bool MapIs(const char *name)
{
	return name == nullptr || (S.level != nullptr && S.level->MapName.CompareNoCase(name) == 0);
}

double Stretch()
{
	return S.level != nullptr && S.level->pixelstretch > 0.1 ? S.level->pixelstretch : 1.2;
}

// Map units -> real metres (the vertical through the level's pixel stretch, roth_gesture.h).
gesture::V3 Metres(const DVector3 &d, double upm) { return gesture::V3(d.X / upm, d.Y / upm, d.Z * Stretch() / upm); }

// The distance from `p` to a box in real metres (0 inside).
double BoxDistanceM(const DVector3 &p, const DVector3 &lo, const DVector3 &hi, double upm)
{
	const double dx = std::max(0.0, std::max(lo.X - p.X, p.X - hi.X));
	const double dy = std::max(0.0, std::max(lo.Y - p.Y, p.Y - hi.Y));
	const double dz = std::max(0.0, std::max(lo.Z - p.Z, p.Z - hi.Z));
	return Metres(DVector3(dx, dy, dz), upm).Length();
}

DVector3 BoxClosest(const DVector3 &p, const DVector3 &lo, const DVector3 &hi)
{
	return DVector3(std::clamp(p.X, lo.X, hi.X), std::clamp(p.Y, lo.Y, hi.Y), std::clamp(p.Z, lo.Z, hi.Z));
}

// THE HAPTICS: VR_ScriptHaptic (hw_vrmodes.h; OpenXR's xrApplyHapticFeedback through VRMode::Vibrate), as the
// picker's own. Under the flat test there is no controller: the line says what would have been sent.
void Buzz(int role, double amp, double ms, const char *why)
{
	if (VRPickTesting())
	{
		if (Logging()) Printf("[roth_gestures] %s hand haptic %.2f for %.0f ms (%s; flat test: not sent)\n", RoleName(role), amp, ms, why);
		return;
	}
	VR_ScriptHaptic(role, amp, ms);
}

// Every Realms object with an actor, shown, and its pick box (PickFrom's and NearAct's box: ObjectPickBox).
std::vector<Thing> Things(AActor *pmo, bool hiddenToo = false)
{
	std::vector<Thing> out;
	auto it = S.level->GetThinkerIterator<AActor>();
	AActor *mo;
	while ((mo = it.Next()) != nullptr)
	{
		if (mo == pmo || (mo->ObjectFlags & OF_EuthanizeMe) || (!hiddenToo && (mo->renderflags & RF_INVISIBLE))) continue;
		Thing t;
		if (!ObjectForActor(mo, t.sector, t.index)) continue;
		// one actor per object: the one the object is found by (the PC run of 03548253 listed each fly twice)
		if (FindObjectActor(t.sector, t.index) != mo) continue;
		t.mo = mo;
		if (const Object *o = RuntimeObject(t.sector, t.index)) t.shared = Pack::ResolveObjectArt(o->textureIndex, o->textureSource, t.art);
		// A CREATURE's body is the original's (the box its shots meet), not its picture: the PC run of e145db53 had the
		// fist "land" on the picture's box 100 units from the creature's axis (ADEMO[43]'s frame is that wide), and the
		// attack aimed there met nothing.
		int r = 0, h = 0;
		if (ActorsCreatureBody(t.sector, t.index, r, h))
		{
			t.creature = true;
			t.lo = DVector3(mo->X() - r, mo->Y() - r, mo->Z());
			t.hi = DVector3(mo->X() + r, mo->Y() + r, mo->Z() + h);
		}
		else ObjectPickBox(mo, t.lo, t.hi);
		out.push_back(t);
	}
	return out;
}

const Thing *FindThing(const std::vector<Thing> &things, const AActor *mo)
{
	for (const Thing &t : things) if (t.mo == mo) return &t;
	return nullptr;
}

const Thing *FindThingAt(const std::vector<Thing> &things, int sector, int index)
{
	for (const Thing &t : things) if (t.sector == sector && t.index == index) return &t;
	return nullptr;
}

// The wards of this level (once a level, the first live tic).
void Build(const std::vector<Thing> &things)
{
	S.built = true;
	S.wards.clear();
	for (const WardKind &w : kWardKinds)
	{
		if (!MapIs(w.map)) continue;
		for (const Thing &t : things) if (t.shared == w.shared && t.art == w.art) S.wards.push_back({ t.sector, t.index, w.what });
	}
	if (Logging()) Printf("[roth_gestures] %s: %d ward(s)\n", S.level->MapName.GetChars(), (int)S.wards.size());
}

//===========================================================================
// The punch (roth_gesture.h checklist items 15-23)
//===========================================================================

// Item 18: no Realms wall between the fist's point and the body's axis at that height.
bool ClearPath(AActor *pmo, const DVector3 &from, const Thing &t)
{
	const DVector3 c = (t.lo + t.hi) * 0.5;
	const DVector3 to(c.X, c.Y, std::clamp(from.Z, t.lo.Z, t.hi.Z));
	const DVector3 d = to - from;
	const double dist = d.Length();
	if (dist < 1.0) return true;
	Pick p;
	if (!RulesPick(from, d / dist, p, dist)) return true;
	return p.kind == Pick::Prop || p.dist >= dist - 1.0;
}

// The click a landed fist makes (roth_handclick.h), and the swing's end when it made one. THE AIM of the attack: on a
// BODY (an object) its axis at the height the fist met it -- RSQ's ClearPath target (gloves.zs:774), where Deliver
// lands the blow on the body -- not the face of its pick box (the PC run of 03548253: aimed at the box's face, close
// and low, the shot dived under the creature); on a surface (glass) the point the fist met.
bool Strike(int role, PunchReader &rd, const char *blow, const Pick &p, const DVector3 &contact, const DVector3 &palm, SecondaryVerb v, int type, int now, const char *what,
	bool borrowed = false)
{
	if (v == SECONDARY_ATTACK_AIMED)
	{
		DVector3 aim = contact;
		if (p.kind == Pick::Prop && p.actor != nullptr) aim = DVector3(p.actor->X(), p.actor->Y(), contact.Z);
		if (borrowed) ActorsRequestBorrowedBlowAt(aim.X, aim.Y, aim.Z);   // the Staff: Adam's fists' attack (roth_vrweapon.h 1b)
		else ActorsRequestFireAt(aim.X, aim.Y, aim.Z);
		FString health;
		int s = -1, i = -1;
		int32_t hp = 0;
		uint8_t st = 0;
		if (p.kind == Pick::Prop && ObjectForActor(p.actor, s, i) && ActorsObjectHealth(s, i, hp, st))
		{
			health.Format("; its health %d (state 0x%02x) before", (int)hp, st);
			Watch w; w.sector = s; w.index = i; w.hp = hp; w.st = st; w.until = now + 70;
			S.watches.push_back(w);
		}
		Printf("[roth_gestures] %s hand %s (%.2f m/s average) lands on %s at (%.1f %.1f %.1f): cursor type %d -> the original's attack,"
			" aimed at (%.1f %.1f %.1f)%s%s\n", RoleName(role), blow, rd.avgMps, what, contact.X, contact.Y, contact.Z, type, aim.X, aim.Y, aim.Z,
			health.GetChars(), borrowed ? " (the blow borrowed from Adam's fists)" : "");
		Buzz(role, 0.5, 4 * 1000.0 / kTicRate, "a landed blow");
		rd.Landed(now);
		return true;
	}
	// A blow on a usable thing (a switch, a door) uses nothing (owner, 2026-11-24: one system, XR Tools': the beam's
	// grip uses). Blows are combat.
	return false;
}

void PunchContact(int role, HandG &g, AActor *pmo, const std::vector<Thing> &things, const DVector3 &a, const DVector3 &b, double upm, int now)
{
	const double gr = (kGloveM + kPadM) * upm;                                    // 16
	// 17: the bodies, nearest approach first; 19: what the original's click there does decides what is a body.
	const Thing *best = nullptr;
	double bestD = 1e30;
	DVector3 bestP;
	Pick bestPick;
	int bestType = 1;
	for (const Thing &t : things)
	{
		double dxy;
		gesture::V3 p;
		if (!SweptBoxContact(gesture::V3(a.X, a.Y, a.Z), gesture::V3(b.X, b.Y, b.Z), gr, gesture::V3(t.lo.X, t.lo.Y, t.lo.Z),
			gesture::V3(t.hi.X, t.hi.Y, t.hi.Z), dxy, p)) continue;
		if (dxy >= bestD) continue;
		const DVector3 at(p.x, p.y, p.z);
		Pick pk;
		pk.kind = Pick::Prop;
		pk.actor = t.mo;
		pk.hitPos = BoxClosest(at, t.lo, t.hi);
		pk.dist = (pk.hitPos - a).Length();
		const int type = RulesClassify(pk).type;
		if (SecondaryClick(type) == SECONDARY_NOTHING) continue;                   // the original's click does nothing there
		if (!ClearPath(pmo, at, t))                                                // 18
		{
			if (Logging()) Printf("[roth_gestures] %s hand punch: object %d/%d is behind a wall from the fist\n", RoleName(role), t.sector, t.index);
			continue;
		}
		best = &t; bestD = dxy; bestP = at; bestPick = pk; bestType = type;
	}
	if (best != nullptr)
	{
		// 20: a fist pulled back out of a body is not a punch (the velocity against the body, as RSQ's st.vel)
		const gesture::V3 body((best->lo.X + best->hi.X) * 0.5, (best->lo.Y + best->hi.Y) * 0.5, best->lo.Z);
		if (!DrivesInto(g.punch.vel, gesture::V3(a.X, a.Y, a.Z), body))
		{
			if (Logging()) Printf("[roth_gestures] %s hand punch: pulled back out of object %d/%d, not a blow\n", RoleName(role), best->sector, best->index);
			return;
		}
		Strike(role, g.punch, "punch", bestPick, bestP, b, SecondaryClick(bestType), bestType, now,
			FStringf("object %d/%d (%s at (%.0f %.0f %.0f), %s (%.0f %.0f %.0f)..(%.0f %.0f %.0f), the fist %.1f units from its axis)", best->sector,
				best->index, best->creature ? "a creature" : "an object", best->mo->X(), best->mo->Y(), best->mo->Z(),
				best->creature ? "the original's body" : "its picture's box", best->lo.X, best->lo.Y, best->lo.Z, best->hi.X, best->hi.Y, best->hi.Z,
				bestD).GetChars());                                                          // 21
		return;
	}
	// 22: no body -- what the fist meets along its travel this tic (a switch, a door, the glass).
	const DVector3 mv = b - a;
	const double len = mv.Length();
	Pick p;
	if (len < 1e-6 || !RulesPick(a, mv / len, p, len + gr))
	{
		if (Logging()) Printf("[roth_gestures] %s hand punch (%.2f m/s average): meets nothing\n", RoleName(role), g.punch.avgMps);
		return;
	}
	// A CREATURE is met by its body only (above), never by its picture's box along the way (the PC run of d3eb1564:
	// the attack came from here, on the ghoul's picture 85 units from it).
	if (p.kind == Pick::Prop)
		for (const Thing &t : things)
			if (t.mo == p.actor && t.creature)
			{
				if (Logging()) Printf("[roth_gestures] %s hand punch (%.2f m/s average) passes creature %d/%d's picture, not its body\n", RoleName(role),
					g.punch.avgMps, t.sector, t.index);
				return;
			}
	const int type = RulesClassify(p).type;
	const SecondaryVerb v = SecondaryClick(type);
	if (v == SECONDARY_NOTHING)
	{
		if (Logging()) Printf("[roth_gestures] %s hand punch (%.2f m/s average) meets %s: cursor type %d -> the original's click does nothing there\n",
			RoleName(role), g.punch.avgMps, PickKindName(p), type);
		return;
	}
	Strike(role, g.punch, "punch", p, p.hitPos, b, v, type, now, PickKindName(p));   // 23
}

//===========================================================================
// The blade (chapter 2 Q6: roth_vrweapon.h 1) -- the punch's contact (items 17-23 above) along the Sword's blade
//===========================================================================

// The blade swept from (grip0, tip0) to (grip1, tip1) this tic, map units. As PunchContact: the bodies first (a creature
// by the original's body, any other object by its pick box), the FIRST the blade met in the tic and the nearest of
// those; what the original's click does there decides (19); a wall between the blade and the body stops it (18); a blade
// drawn back out of a body is no blow (20). No body: what the tip met along its travel, then the blade along its length
// (a wall, a door, the glass) (22). The blow is Strike's: the original's attack aimed at the contact (the Sword equipped:
// its bullet, damage and rate), or the original's use where the cursor says use, as the fist's.
void BladeContact(int role, HandG &g, AActor *pmo, const std::vector<Thing> &things, const DVector3 &grip0, const DVector3 &tip0,
	const DVector3 &grip1, const DVector3 &tip1, double upm, int now, int item)
{
	const bool borrowed = vrweapon::SwingBorrowsFists(item);
	const char *blow = item == vrweapon::kStaff ? "staff swing" : "sword swing";
	const double gr = vrweapon::kBladeHalfM * upm;
	auto v3 = [](const DVector3 &d) { return gesture::V3(d.X, d.Y, d.Z); };
	const Thing *best = nullptr;
	vrweapon::BladeContact bestC;
	Pick bestPick;
	int bestType = 1;
	for (const Thing &t : things)
	{
		const vrweapon::BladeContact c = vrweapon::BladeSweptBoxContact(v3(grip0), v3(tip0), v3(grip1), v3(tip1), gr, v3(t.lo), v3(t.hi));
		if (!c.hit) continue;
		if (best != nullptr && (c.u > bestC.u || (c.u == bestC.u && c.dxy >= bestC.dxy))) continue;
		const DVector3 at(c.p.x, c.p.y, c.p.z);
		Pick pk;
		pk.kind = Pick::Prop;
		pk.actor = t.mo;
		pk.hitPos = BoxClosest(at, t.lo, t.hi);
		pk.dist = (pk.hitPos - grip1).Length();
		const int type = RulesClassify(pk).type;
		if (SecondaryClick(type) == SECONDARY_NOTHING) continue;
		if (!ClearPath(pmo, at, t))
		{
			if (Logging()) Printf("[roth_gestures] %s hand %s: object %d/%d is behind a wall from the blade\n", RoleName(role), blow, t.sector, t.index);
			continue;
		}
		best = &t; bestC = c; bestPick = pk; bestType = type;
	}
	if (best != nullptr)
	{
		// 20: the blade's point there moving into the body (its own travel this tic), not drawn back out of it
		const DVector3 p0 = grip0 + (tip0 - grip0) * bestC.s, p1 = grip1 + (tip1 - grip1) * bestC.s;
		const gesture::V3 body((best->lo.X + best->hi.X) * 0.5, (best->lo.Y + best->hi.Y) * 0.5, best->lo.Z);
		if (!DrivesInto(v3(p1 - p0), v3(p0), body))
		{
			if (Logging()) Printf("[roth_gestures] %s hand %s: drawn back out of object %d/%d, not a blow\n", RoleName(role), blow, best->sector, best->index);
			return;
		}
		Strike(role, g.blade, blow, bestPick, DVector3(bestC.p.x, bestC.p.y, bestC.p.z), grip1, SecondaryClick(bestType), bestType, now,
			FStringf("object %d/%d (%s at (%.0f %.0f %.0f), %s (%.0f %.0f %.0f)..(%.0f %.0f %.0f), the blade %.0f%% of the way to its tip, %.0f%% through the tic)",
				best->sector, best->index, best->creature ? "a creature" : "an object", best->mo->X(), best->mo->Y(), best->mo->Z(),
				best->creature ? "the original's body" : "its picture's box", best->lo.X, best->lo.Y, best->lo.Z, best->hi.X, best->hi.Y,
				best->hi.Z, bestC.s * 100.0, bestC.u * 100.0).GetChars(), borrowed);
		return;
	}
	// 22: no body -- the tip's travel, then the blade itself
	Pick p;
	bool have = false;
	const DVector3 mv = tip1 - tip0;
	if (mv.Length() > 1e-6 && RulesPick(tip0, mv / mv.Length(), p, mv.Length() + gr)) have = true;
	const DVector3 bl = tip1 - grip1;
	Pick q;
	if (bl.Length() > 1e-6 && RulesPick(grip1, bl / bl.Length(), q, bl.Length() + gr) && (!have || q.dist < p.dist)) { p = q; have = true; }
	if (!have)
	{
		if (Logging()) Printf("[roth_gestures] %s hand %s (%.2f m/s average): meets nothing\n", RoleName(role), blow, g.blade.avgMps);
		return;
	}
	if (p.kind == Pick::Prop)
		for (const Thing &t : things)
			if (t.mo == p.actor && t.creature)
			{
				if (Logging()) Printf("[roth_gestures] %s hand %s passes creature %d/%d's picture, not its body\n", RoleName(role), blow, t.sector, t.index);
				return;
			}
	const int type = RulesClassify(p).type;
	const SecondaryVerb v = SecondaryClick(type);
	if (v == SECONDARY_NOTHING)
	{
		if (Logging()) Printf("[roth_gestures] %s hand %s (%.2f m/s average) meets %s: cursor type %d -> the original's click does nothing there\n",
			RoleName(role), blow, g.blade.avgMps, PickKindName(p), type);
		return;
	}
	Strike(role, g.blade, blow, p, p.hitPos, grip1, v, type, now, PickKindName(p), borrowed);
}

// The blade this tic: the grip (the aim pose's place, where the held weapon is drawn: AttackPos) and the tip along the
// aim pose (AttackAngle is the world yaw less 90, AttackPitch positive up; the vertical through the pixel stretch).
bool BladeNow(AActor *pmo, double upm, int item, DVector3 &grip, DVector3 &tip, DVector3 &strike)
{
	if (pmo == nullptr || !pmo->OverrideAttackPosDir) return false;
	const double yaw = (pmo->AttackAngle.Degrees() + 90.0) * (M_PI / 180.0), pit = pmo->AttackPitch.Radians();
	const DVector3 dir(cos(pit) * cos(yaw), cos(pit) * sin(yaw), sin(pit) / Stretch());
	const double len = vrweapon::SwungLengthM(item) * upm;
	grip = pmo->AttackPos;
	tip = grip + dir * len;
	strike = grip + dir * (len * vrweapon::kStrikeAt);
	return true;
}

//===========================================================================
// The knock
//===========================================================================

//===========================================================================
// The attempts
//===========================================================================

//===========================================================================
// One hand, one tic
//===========================================================================

void Forget(HandG &g)
{
	g.punch.Invalidate();
	g.blade.Invalidate();
	g.bladePrev = false;
	g.havePrev = false;
}

bool HandIsEmpty(int role)
{
	if (HandHoldsWeapon(role)) return false;
	if (HeldHandCarries(role)) return false;   // the Lamp, the lit match, the open Book (roth_held.h)
	if (!HandEmptyNow(role)) return false;     // L2: another owner holds the hand (roth_handstate.h)
	if (role == ItemHandRole())
	{
		const Inventory *inv = InterfaceInventory();
		if (inv != nullptr && inv->RightHand() >= 0 && (int16_t)inv->Slot(inv->RightHand()).id > 0) return false;   // holds the right-hand item
	}
	return true;
}

void TickHand(int role, AActor *pmo, const std::vector<Thing> &things)
{
	HandG &g = S.hand[role];
	VRPickHand h;
	if (!VRPickHandNow(role, h) || !h.havePoints) { Forget(g); return; }
	const double upm = VRPickUnitsPerMetre();
	const int now = S.level->maptime;
	const DVector3 body(pmo->X(), pmo->Y(), pmo->player->viewz);
	const gesture::V3 relPalm = Metres(h.palm - body, upm), relTip = Metres(h.tip - body, upm);
	gesture::V3 velPalm, velTip;
	bool velOk = g.havePrev && now - g.prevTic >= 1 && now - g.prevTic <= kMaxGap;
	if (velOk)
	{
		const double k = (double)kTicRate / (now - g.prevTic);
		velPalm = (relPalm - g.prevRelPalm) * k;
		velTip = (relTip - g.prevRelTip) * k;
	}
	// A STEP NO HAND MAKES (roth_gesture.h HandStepIsJump), or a test hand placed: nothing of it counts.
	const bool jump = (velOk && (HandStepIsJump(velPalm.Length()) || HandStepIsJump(velTip.Length()))) || now <= g.placedUntil;
	if (jump)
	{
		if (Logging() && g.punch.armed)
			Printf("[roth_gestures] %s hand: a step of %.1f m/s %s -- a jump, not a motion: dropped\n", RoleName(role), velPalm.Length(),
				now <= g.placedUntil ? "(the test hand was placed)" : "(over 12 m/s)");
		g.punch.Jumped();
		velOk = false;
	}
	// THE SWORD in the main hand (Q6): the blade is read below, the hand is no fist; a hand whose grip is a weapon's (the
	// Shotgun's fore-end, a body slot: roth_vrweapons.h) is no fist either.
	const int heldItem = role == VR_MAINHAND ? VRWeaponsHeldItem() : 0;
	const bool swordHand = role == VR_MAINHAND && vrweapon::IsSwung(heldItem);   // the Sword or the Staff
	// L2 (roth_handstate.h): a fist is a closed hand nobody owns -- not round the Lamp's bail, an item, the fore-end, a
	// body slot, the Colt's parts or a lever.
	const bool fist = h.gripClosed && !HandHoldsWeapon(role) && !HeldHandCarries(role) && !swordHand && HandEmptyNow(role);

	// ---- THE PUNCH ----
	const bool swing = !jump && g.punch.Feed(fist, relPalm, now, pmo->Angles.Yaw.Degrees(), Stretch());
	if (jump && fist) { g.punch.prevValid = true; g.punch.prevTic = now; g.punch.loc = relPalm; g.punch.prevYaw = pmo->Angles.Yaw.Degrees(); }
	if (swing && g.havePrev) PunchContact(role, g, pmo, things, g.prevPalm, h.palm, upm, now);

	// ---- THE SWORD (Q6) AND THE STAFF (Q10): the fists' reader on the striking point; the contact along its length ----
	{
		DVector3 grip, tip, strike;
		const bool haveBlade = swordHand && BladeNow(pmo, upm, heldItem, grip, tip, strike);
		if (!haveBlade) { g.blade.Invalidate(); g.bladePrev = false; }
		else
		{
			// a step no hand makes (HandStepIsJump) on the GRIP, the hand itself: the blade's point may outrun 12 m/s in a real cut
			const int dt = now - g.bladePrevTic;
			const bool gripJump = g.bladePrev && dt >= 1 && HandStepIsJump(Metres(grip - g.bladeGrip, upm).Length() * kTicRate / dt);
			if (gripJump || now <= g.placedUntil)
			{
				if (Logging() && g.blade.armed) Printf("[roth_gestures] %s hand %s: a step the hand did not make -- dropped\n", RoleName(role), heldItem == vrweapon::kStaff ? "staff" : "sword");
				g.blade.Jumped();
			}
			const bool cut = !gripJump && now > g.placedUntil && g.blade.Feed(true, Metres(strike - body, upm), now, pmo->Angles.Yaw.Degrees(), Stretch());
			if (cut && g.bladePrev) BladeContact(role, g, pmo, things, g.bladeGrip, g.bladeTip, grip, tip, upm, now, heldItem);
			g.bladePrev = true;
			g.bladeGrip = grip;
			g.bladeTip = tip;
			g.bladePrevTic = now;
		}
	}

	// The knock to examine, the attempts (an empty palm pushed at a face, a tug at a rack) and the palm searching a door
	// for its lock are gone (owner, 2026-11-24: "Strip it, use XR Tools"): the beam and the pickup are the only ways
	// the hands use and examine (roth_vrpick.cpp THE ONE SYSTEM). The punch and the blades stay: they are combat.

	// ---- THE WARD TINGLE ----
	{
		double best = 1e30;
		const char *which = nullptr;
		// the wards SHOWN now, looked for each tic: AELF's have no actor until Florentine's trap shows them (the PC run of
		// 976cb81e: "0 ward(s)" at the level's start)
		for (const Thing &t : things)
			for (const WardKind &k : kWardKinds)
			{
				if (!MapIs(k.map) || t.shared != k.shared || t.art != k.art) continue;
				const double d = BoxDistanceM(h.palm, t.lo, t.hi, upm);
				if (d < best) { best = d; which = k.what; }
			}
		const double amp = WardTingle(best);
		if (amp > 0 && now - g.wardTic >= kWardPulseTics)
		{
			g.wardTic = now;
			if (!VRPickTesting()) VR_ScriptHaptic(role, amp, 30);
		}
		const int cm = amp > 0 ? (int)(best * 100.0) : -1;
		if (Logging() && cm != g.loggedWardCm && (cm < 0 || g.loggedWardCm < 0 || abs(cm - g.loggedWardCm) >= 2))
		{
			if (amp > 0) Printf("[roth_gestures] %s hand: %.2f m from %s -- the tingle %.2f%s\n", RoleName(role), best, which, amp,
				VRPickTesting() ? " (flat test: not sent)" : "");
			else Printf("[roth_gestures] %s hand: no ward within %.2f m\n", RoleName(role), kWardNearM);
			g.loggedWardCm = cm;
		}
	}

	g.havePrev = true;
	g.prevTic = now;
	g.prevPalm = h.palm;
	g.prevTip = h.tip;
	g.prevRelPalm = relPalm;
	g.prevRelTip = relTip;
}

// The flat test's scripted motion (roth_gesturetest move): each tic, the test hand's place along its path.
void TickMotion()
{
	for (int role = 0; role < 2; role++)
	{
		HandG &g = S.hand[role];
		Motion &m = g.motion;
		if (S.level == nullptr) continue;
		const int side = VRHandPose_SideForRole(role);
		if (g.hold && !m.on)
		{
			VRPickHand h;
			AActor *pmo = players[consoleplayer].mo;
			if (pmo != nullptr && VRPickHandNow(role, h) && h.havePoints)
			{
				const DVector3 d = g.holdWant - (g.holdTip ? h.tip : h.palm);
				const double upm = VRPickUnitsPerMetre();
				if (d.Length() > 0.2)
				{
					const double yaw = pmo->Angles.Yaw.Radians();
					g.base[0] += (d.X * cos(yaw) + d.Y * sin(yaw)) / upm;
					g.base[1] += (d.X * sin(yaw) - d.Y * cos(yaw)) / upm;
					g.base[2] += d.Z * Stretch() / upm;
					VRHandTest_SetOffset(side, g.base[0], g.base[1], g.base[2]);
					g.placedUntil = S.level->maptime + 2;
				}
				else if (g.holdLogged != 1)
				{
					Printf("[roth_gestures] test: the %s hand's %s held at (%.1f %.1f %.1f), the player at (%.0f %.0f %.0f) where the original's movement"
						" left him; the hand %.2f m ahead, %.2f m right, %.2f m up of its usual place\n", RoleName(role), g.holdTip ? "fingertip" : "palm",
						g.holdWant.X, g.holdWant.Y, g.holdWant.Z, pmo->X(), pmo->Y(), pmo->Z(), g.base[0], g.base[1], g.base[2]);
					g.holdLogged = 1;
				}
			}
			continue;
		}
		if (!m.on) continue;
		const int t = S.level->maptime - m.start;
		const double f = m.tics <= 0 ? 1.0 : std::min(1.0, (double)t / m.tics);
		double o[3];
		for (int k = 0; k < 3; k++) o[k] = m.from[k] + (m.to[k] - m.from[k]) * f;
		VRHandTest_SetOffset(side, g.base[0] + o[0], g.base[1] + o[1], g.base[2] + o[2]);
		if (f >= 1.0)
		{
			m.on = false;
			Printf("[roth_gestures] test: the %s hand's motion ends at (%.2f %.2f %.2f) m, tic %d\n", RoleName(role), o[0], o[1], o[2], S.level->maptime);
		}
	}
}

} // namespace (anonymous)

void GesturesTick(FLevelLocals *level, bool live)
{
	if (level == nullptr) return;
	if (S.level != level) { GesturesEndLevel(); S.level = level; }
	TickMotion();
	player_t *player = level->GetConsolePlayer();
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	if (!live || pmo == nullptr || pmo->player == nullptr)
	{
		for (HandG &g : S.hand) Forget(g);
		return;
	}
	// the punched creatures: what the original's attack did
	for (size_t k = 0; k < S.watches.size();)
	{
		Watch &w = S.watches[k];
		int32_t hp = 0;
		uint8_t st = 0;
		const bool have = ActorsObjectHealth(w.sector, w.index, hp, st);
		if (have && (hp != w.hp || st != w.st))
		{
			Printf("[roth_gestures] the punched object %d/%d: health %d -> %d, state 0x%02x -> 0x%02x (tic %d)\n", w.sector, w.index, (int)w.hp, (int)hp,
				w.st, st, level->maptime);
			w.hp = hp; w.st = st;
		}
		if (!have || level->maptime >= w.until)
		{
			Printf("[roth_gestures] the punched object %d/%d: health %d at tic %d (followed 2 s)\n", w.sector, w.index, (int)w.hp, level->maptime);
			S.watches.erase(S.watches.begin() + k);
		}
		else k++;
	}
	const std::vector<Thing> things = Things(pmo);
	// THE ZONES AND WARDS FROM EVERY OBJECT, shown or not (AELF's wards are hidden until Florentine's trap shows them);
	// each tic samples only the shown ones (FindThingAt on `things`).
	if (!S.built) Build(Things(pmo, true));
	for (int role = 0; role < 2; role++) TickHand(role, pmo, things);
}

void GesturesEndLevel()
{
	S.level = nullptr;
	S.built = false;
	S.wards.clear();
	S.watches.clear();
	for (HandG &g : S.hand) { Forget(g); g.motion = Motion(); g.wardTic = -1000; g.loggedWardCm = -1; }
}

} // namespace roth

//===========================================================================
//
// roth_gesturetest -- THE FLAT TEST'S MOVING HANDS. Test-only, inert until typed; with roth_handtest (the
// fingers: "roth_handtest 1 0 0 1 0" a fist, "roth_handtest open 0" an open hand) and roth_vrpicktest on.
//
//   roth_gesturetest move <main|off> <fwd> <right> <up> [tics]
//        the test hand's place moves from where it is to (fwd, right, up) metres off its usual place in front
//        of the camera, in a straight line over <tics> (0 or none: at once). A punch is a quick move forward
//        (0.40 m in 3 tics = 4.7 m/s); a rap a short slow one in and back; a wave a few quick strokes.
//   roth_gesturetest reach <main|off> <palm|tip> thing <shared 0|1> <art> [band] [metres]
//        stand the player and lift the test hand so its palm (or fingertip) is <metres> (default 0.25) short
//        of the nearest object wearing that art (ADEMO when shared), at <band> (0 bottom .. 1 top, default 0.5)
//        of its box, toward the player, facing it.
//   roth_gesturetest reach <main|off> <palm|tip> sector <rothSector> [metres]
//        ... <metres> (default 0.10) above that sector's floor, over its middle.
//   roth_gesturetest reach <main|off> <palm|tip> creature [metres]
//        ... short of the nearest object the original's cursor makes an attack target (type 5): a creature.
//   roth_gesturetest reach <main|off> <palm|tip> door [metres]
//        ... <metres> (default 0.08) short of the nearest door leaf the picker's seek finds.
//   EVERY REACH IS HELD: the original's movement (roth_playermove_engine) may push the player off where he was stood
//   (out of an object's square, off a door leaf); the test hand then reaches the rest, corrected every tic until a
//   move starts, and a move is relative to that place. The player is first stood clear of the thing's push-out.
//   roth_gesturetest look thing <shared 0|1> <art> <units> [bearing]
//        a capture's pose: stand <units> from the nearest object wearing that art at <bearing> degrees (object ->
//        camera; default the last one given for that object, else where the player stands), facing its middle,
//        both test hands at their usual place and lift (as roth_vrpicktest look does for a candle)
//   roth_gesturetest home     every test hand back to its usual place
//
//===========================================================================

namespace roth
{
namespace
{

bool TestPoint(int role, bool tip, DVector3 &out)
{
	VRPickHand h;
	if (!VRPickHandNow(role, h) || !h.havePoints) return false;
	out = tip ? h.tip : h.palm;
	return true;
}

// Stand the player so the hand's point lands at `want` facing `look` (the point's offset from the eye, read in
// the current yaw's frame, is kept), the height made up by the test hand's lift (as roth_vrpicktest wick does).
void PlaceHand(AActor *pmo, int role, bool tip, const DVector3 &want, const DVector3 &look, const DVector3 *clearLo = nullptr,
	const DVector3 *clearHi = nullptr, bool offFurniture = false)
{
	DVector3 now;
	if (!TestPoint(role, tip, now)) { Printf("roth_gesturetest reach: no test hand (roth_vrpicktest on, roth_handtest first)\n"); return; }
	const double yaw0 = pmo->Angles.Yaw.Radians();
	const DVector3 rel = now - pmo->Pos();
	const double fwd = rel.X * cos(yaw0) + rel.Y * sin(yaw0), left = -rel.X * sin(yaw0) + rel.Y * cos(yaw0);
	DVector3 face(look.X - want.X, look.Y - want.Y, 0.0);
	double yaw = face.Length() > 1e-3 ? atan2(face.Y, face.X) : yaw0;
	DVector3 stand(want.X - (fwd * cos(yaw) - left * sin(yaw)), want.Y - (fwd * sin(yaw) + left * cos(yaw)), pmo->Z());
	// CLEAR OF THE THING'S PUSH-OUT: the original's movement keeps the player's box (+-0x1c) off an object's dimension
	// square (roth_playermove.cpp ResolveObjects, CP:1253-1324), so stand him at least the box's half plus 0x1c (and a
	// little) from its middle in Chebyshev distance; the held reach puts the hand the rest of the way.
	if (clearLo != nullptr && clearHi != nullptr)
	{
		const DVector3 c = (*clearLo + *clearHi) * 0.5;
		const double need = std::max(clearHi->X - clearLo->X, clearHi->Y - clearLo->Y) * 0.5 + 0x1c + 4;
		const double cheb = std::max(fabs(stand.X - c.X), fabs(stand.Y - c.Y));
		if (cheb < need && cheb > 1e-3)
		{
			const double k = need / cheb;
			stand = DVector3(c.X + (stand.X - c.X) * k, c.Y + (stand.Y - c.Y) * k, stand.Z);
		}
	}
	// OFF THE FURNITURE: a floor zone (the typewriter on its desk) put the player ON the desk top (the PC run of
	// 367c710c: z 90, sector 321). Step back along the view until the floor there is well below the point.
	if (offFurniture && S.level != nullptr)
	{
		const DVector2 back(-cos(yaw), -sin(yaw));
		for (int k = 0; k < 24; k++)
		{
			sector_t *sec = S.level->PointInSector(stand.XY());
			if (sec != nullptr && sec->floorplane.ZatPoint(stand.XY()) < want.Z - 60) break;
			stand = DVector3(stand.X + back.X * 8, stand.Y + back.Y * 8, stand.Z);
		}
	}
	const double oldZ = pmo->Z();
	pmo->Angles.Yaw = DAngle::fromRad(yaw);
	pmo->SetOrigin(stand, false);
	pmo->SetZ(pmo->floorz);
	pmo->Vel.Zero();
	// THE VIEW LOOKS AT THE HAND (the captures must show the hand at the thing; the test hands follow the yaw only, so
	// pitching the view does not move them): down or up to the point, from the eye.
	{
		const double eyeZ = pmo->Z() + (pmo->player != nullptr ? pmo->player->viewheight : 41.0);
		const double run = std::max(1.0, (want.XY() - stand.XY()).Length());
		pmo->Angles.Pitch = DAngle::fromRad(atan2(eyeZ - want.Z, run));   // + looks down
	}
	pmo->ClearInterpolation();
	if (S.level != nullptr) S.hand[role].placedUntil = S.level->maptime + 2;
	{
		HandG &g = S.hand[role];
		g.hold = true; g.holdTip = tip; g.holdWant = want; g.holdLogged = 0; g.motion.on = false;
		double f, r, u;
		VRHandTest_Offset(VRHandPose_SideForRole(role), f, r, u);
		g.base[0] = f; g.base[1] = r; g.base[2] = u;
	}
	const double upm = VRPickUnitsPerMetre();
	const double dz = want.Z - (now.Z + (pmo->Z() - oldZ));
	const double ps = S.level != nullptr && S.level->pixelstretch > 0 ? S.level->pixelstretch : 1.0;
	VRHandTest_SetLift(VRHandTest_Lift() + dz * ps / upm);
	Printf("roth_gesturetest reach: the player stands at (%.0f %.0f %.0f) in sector %d facing %.0f; the %s hand's %s lifted %+.1f units"
		" to (%.1f %.1f %.1f)\n", pmo->X(), pmo->Y(), pmo->Z(), pmo->Sector ? pmo->Sector->Index() : -1, pmo->Angles.Yaw.Degrees(),
		RoleName(role), tip ? "fingertip" : "palm", dz, want.X, want.Y, want.Z);
}

} // namespace

void GesturesTestPlacing(int role)
{
	if (S.level != nullptr && role >= 0 && role < 2) S.hand[role].placedUntil = S.level->maptime + 2;
}

void GesturesHoldReach(int role, bool tip, double wx, double wy, double wz, double lx, double ly, double lz)
{
	AActor *pmo = players[consoleplayer].mo;
	if (pmo == nullptr || S.level == nullptr) { Printf("roth_gesturetest reach: no level or player yet\n"); return; }
	PlaceHand(pmo, role, tip, DVector3(wx, wy, wz), DVector3(lx, ly, lz));
}

} // namespace roth

CCMD(roth_gesturetest)
{
	using namespace roth;
	if (argv.argc() < 2)
	{
		Printf("roth_gesturetest move <main|off> <fwd> <right> <up> [tics] | reach <main|off> <palm|tip> thing <shared> <art> [band] [metres]"
			" | reach <main|off> <palm|tip> sector <n> [metres] | reach <main|off> <palm|tip> creature [metres]"
			" | look thing <shared> <art> <units> [bearing] | home\n");
		return;
	}
	AActor *pmo = players[consoleplayer].mo;
	if (!stricmp(argv[1], "look") && argv.argc() >= 6 && !stricmp(argv[2], "thing") && pmo != nullptr && S.level != nullptr)
	{
		static int lookArt = -1;
		static double lookBearing = -1000.0;
		const bool shared = atoi(argv[3]) != 0;
		const int art = atoi(argv[4]);
		const double units = atof(argv[5]);
		const std::vector<Thing> things = Things(pmo);
		const Thing *t = nullptr;
		double best = 1e30;
		for (const Thing &c : things)
			if (c.shared == shared && c.art == art && (c.mo->Pos() - pmo->Pos()).Length() < best) { best = (c.mo->Pos() - pmo->Pos()).Length(); t = &c; }
		if (t == nullptr) { Printf("roth_gesturetest look: no such object on this map\n"); return; }
		const DVector3 c = (t->lo + t->hi) * 0.5;
		const int key = (shared ? 0x10000 : 0) | art;
		if (argv.argc() >= 7) lookBearing = atof(argv[6]);
		else if (lookArt != key || lookBearing < -360.0) lookBearing = atan2(pmo->Y() - c.Y, pmo->X() - c.X) * (180.0 / M_PI);
		lookArt = key;
		const DAngle b = DAngle::fromDeg(lookBearing);
		pmo->SetOrigin(DVector3(c.X + b.Cos() * units, c.Y + b.Sin() * units, pmo->Z()), false);
		pmo->SetZ(pmo->floorz);
		pmo->Vel.Zero();
		pmo->Angles.Yaw = b + DAngle::fromDeg(180.0);
		const double eye = pmo->Z() + (pmo->player != nullptr ? pmo->player->viewheight : 41.0);
		pmo->Angles.Pitch = DAngle::fromDeg(atan2(eye - c.Z, units) * (180.0 / M_PI));
		pmo->ClearInterpolation();
		for (int side = 0; side < 2; side++) VRHandTest_SetOffset(side, 0, 0, 0);
		VRHandTest_SetLift(0.0);
		for (HandG &g : S.hand) { g.motion.on = false; g.hold = false; g.base[0] = g.base[1] = g.base[2] = 0; g.placedUntil = S.level->maptime + 2; }
		Printf("roth_gesturetest look: %.0f units from object %d/%d (%s[%d]) at bearing %.0f, facing its middle (%.0f %.0f %.0f), pitch %.1f\n",
			units, t->sector, t->index, shared ? "ADEMO" : "DEMO", art, lookBearing, c.X, c.Y, c.Z, pmo->Angles.Pitch.Degrees());
		return;
	}
	if (!stricmp(argv[1], "home"))
	{
		for (int side = 0; side < 2; side++) VRHandTest_SetOffset(side, 0, 0, 0);
		for (HandG &g : S.hand) { g.motion.on = false; g.hold = false; g.base[0] = g.base[1] = g.base[2] = 0; if (S.level != nullptr) g.placedUntil = S.level->maptime + 2; }
		Printf("roth_gesturetest: both test hands at their usual place\n");
		return;
	}
	if (argv.argc() < 3) { Printf("roth_gesturetest: which hand?\n"); return; }
	const int role = !stricmp(argv[2], "off") ? VR_OFFHAND : VR_MAINHAND;
	if (!stricmp(argv[1], "move") && argv.argc() >= 6)
	{
		HandG &g = S.hand[role];
		Motion &m = g.motion;
		const int side = VRHandPose_SideForRole(role);
		VRHandTest_Offset(side, m.from[0], m.from[1], m.from[2]);
		for (int k = 0; k < 3; k++) m.from[k] -= g.base[k];   // relative to the held reach's base (0 without one)
		g.hold = false;
		m.to[0] = atof(argv[3]); m.to[1] = atof(argv[4]); m.to[2] = atof(argv[5]);
		m.tics = argv.argc() >= 7 ? atoi(argv[6]) : 0;
		m.start = S.level != nullptr ? S.level->maptime : 0;
		m.on = true;
		const double dist = sqrt((m.to[0] - m.from[0]) * (m.to[0] - m.from[0]) + (m.to[1] - m.from[1]) * (m.to[1] - m.from[1])
			+ (m.to[2] - m.from[2]) * (m.to[2] - m.from[2]));
		Printf("roth_gesturetest move: the %s hand from (%.2f %.2f %.2f) to (%.2f %.2f %.2f) m in %d tic(s): %.2f m%s\n", RoleName(role),
			m.from[0], m.from[1], m.from[2], m.to[0], m.to[1], m.to[2], m.tics, dist,
			m.tics > 0 ? FStringf(", %.2f m/s", dist * kTicRate / m.tics).GetChars() : "");
		if (m.tics <= 0) { VRHandTest_SetOffset(side, g.base[0] + m.to[0], g.base[1] + m.to[1], g.base[2] + m.to[2]); m.on = false; if (S.level != nullptr) S.hand[role].placedUntil = S.level->maptime + 2; }
		return;
	}
	if (!stricmp(argv[1], "reach") && argv.argc() >= 5 && pmo != nullptr && S.level != nullptr)
	{
		const bool tip = !stricmp(argv[3], "tip");
		const char *what = argv[4];
		const double upm = VRPickUnitsPerMetre();
		const std::vector<Thing> things = Things(pmo);
		if (!stricmp(what, "sector") && argv.argc() >= 6)
		{
			const int sn = atoi(argv[5]);
			const double metres = argv.argc() >= 7 ? atof(argv[6]) : 0.10;
			if (sn < 0 || sn >= (int)S.level->sectors.Size()) { Printf("roth_gesturetest reach: no sector %d\n", sn); return; }
			sector_t *sec = &S.level->sectors[sn];
			DVector2 c(0, 0);
			int n = 0;
			for (line_t *ln : sec->Lines) { c += ln->v1->fPos(); c += ln->v2->fPos(); n += 2; }
			if (n == 0) { Printf("roth_gesturetest reach: sector %d has no lines\n", sn); return; }
			c /= n;
			const double fz = sec->floorplane.ZatPoint(c);
			const DVector3 want(c.X, c.Y, fz + metres * upm / Stretch());
			const DVector3 look = want + DVector3(pmo->Angles.Yaw.Cos(), pmo->Angles.Yaw.Sin(), 0) * 10.0;
			PlaceHand(pmo, role, tip, want, look, nullptr, nullptr, true);
			return;
		}
		if (!stricmp(what, "door"))
		{
			// the nearest door leaf the picker's seek finds (PickTestTarget "door", a test instrument), the hand <metres>
			// (default 0.08) short of it toward where seek would stand; the player kept 0x1c + 4 off the leaf
			const double metres = argv.argc() >= 6 ? atof(argv[5]) : 0.08;
			const int side = VRHandPose_SideForRole(role);
			DVector3 spot0;
			VRHandTest_ActorSpot(side, DVector3(0, 0, 0), 0.0, spot0);
			PickTestHand hand;
			hand.ahead = spot0.X; hand.right = -spot0.Y; hand.up = spot0.Z;
			hand.metresOff = metres * upm;
			hand.upm = upm;
			DVector3 aim, stand;
			if (!PickTestTarget("door", pmo, hand, aim, stand)) { Printf("roth_gesturetest reach: no reachable door leaf\n"); return; }
			DVector3 away(stand.X - aim.X, stand.Y - aim.Y, 0.0);
			if (away.Length() < 1e-3) away = DVector3(1, 0, 0);
			away /= away.Length();
			const DVector3 want = aim + away * (metres * upm);
			const DVector3 lo(aim.X, aim.Y, aim.Z), hi(aim.X, aim.Y, aim.Z);
			Printf("roth_gesturetest reach: the door leaf at (%.1f %.1f %.1f)\n", aim.X, aim.Y, aim.Z);
			PlaceHand(pmo, role, tip, want, aim, &lo, &hi);
			return;
		}
		const Thing *t = nullptr;
		double band = 0.5, metres = 0.25;
		if (!stricmp(what, "thing") && argv.argc() >= 7)
		{
			const bool shared = atoi(argv[5]) != 0;
			const int art = atoi(argv[6]);
			band = argv.argc() >= 8 ? atof(argv[7]) : 0.5;
			metres = argv.argc() >= 9 ? atof(argv[8]) : 0.25;
			double best = 1e30;
			for (const Thing &c : things)
				if (c.shared == shared && c.art == art && (c.mo->Pos() - pmo->Pos()).Length() < best) { best = (c.mo->Pos() - pmo->Pos()).Length(); t = &c; }
		}
		else if (!stricmp(what, "creature"))
		{
			metres = argv.argc() >= 6 ? atof(argv[5]) : 0.25;
			double best = 1e30;
			for (const Thing &c : things)
			{
				Pick p;
				p.kind = Pick::Prop;
				p.actor = c.mo;
				p.hitPos = (c.lo + c.hi) * 0.5;
				if (RulesClassify(p).type != 5) continue;
				const double d = (c.mo->Pos() - pmo->Pos()).Length();
				if (d < best) { best = d; t = &c; }
			}
		}
		if (t == nullptr) { Printf("roth_gesturetest reach: no such object on this map\n"); return; }
		const DVector3 c = (t->lo + t->hi) * 0.5;
		const double z = t->lo.Z + (t->hi.Z - t->lo.Z) * band;
		DVector3 toward(pmo->X() - c.X, pmo->Y() - c.Y, 0.0);
		if (toward.Length() < 1e-3) toward = DVector3(1, 0, 0);
		toward /= toward.Length();
		// where the line from the middle toward the player LEAVES the box (an axis-aligned box: along a diagonal its
		// corner reaches further than its half width -- the PC run of 03548253 put the fingertip inside the armour's box)
		const double hx = (t->hi.X - t->lo.X) * 0.5, hy = (t->hi.Y - t->lo.Y) * 0.5;
		const double tx = fabs(toward.X) > 1e-9 ? hx / fabs(toward.X) : 1e30, ty = fabs(toward.Y) > 1e-9 ? hy / fabs(toward.Y) : 1e30;
		const double half = std::min(tx, ty);
		const DVector3 face(c.X + toward.X * half, c.Y + toward.Y * half, z);
		const DVector3 want = face + toward * (metres * upm);
		Printf("roth_gesturetest reach: object %d/%d (%s[%d]), %s (%.0f %.0f %.0f)..(%.0f %.0f %.0f), its face at (%.1f %.1f %.1f)\n", t->sector,
			t->index, t->shared ? "ADEMO" : "DEMO", t->art, t->creature ? "a creature: the original's body" : "its picture's box", t->lo.X, t->lo.Y,
			t->lo.Z, t->hi.X, t->hi.Y, t->hi.Z, face.X, face.Y, face.Z);
		PlaceHand(pmo, role, tip, want, DVector3(c.X, c.Y, z), &t->lo, &t->hi);
		return;
	}
	Printf("roth_gesturetest: unknown or short '%s'\n", argv[1]);
}
