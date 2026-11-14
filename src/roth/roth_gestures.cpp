// roth_gestures.cpp -- see roth_gestures.h (what each gesture does) and roth_gesture.h (the detectors, and the
// port checklist of RSQ Melee's punch reader).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roth_gestures.h"
#include "roth_gesture.h"
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

//===========================================================================
// THE CHAPTER-1 TABLE (docs/VR_CHAPTER01_SHADOWS.md: the entries named; the owner's list, Q4). Each zone is
// built from the original's own object (by its art, as the map wears it: Pack::ResolveObjectArt's pack and
// index -- DEMO = STUDY1's own pack, ADEMO the shared one) or its own sectors, and plays the examine of that
// object or floor: the original's line, through the trigger's path (VRPickExamineAt). Nothing else.
//===========================================================================

struct ObjectAttempt
{
	const char *map;       // the level the art index belongs to (the level pack); nullptr: any (shared pack)
	bool shared;           // ADEMO
	int art;
	int kind;              // ATTEMPT_PUSH / ATTEMPT_TUG
	double bandLo, bandHi; // the zone: this slice of the object's pick box, by height
	const char *what;
};
const ObjectAttempt kObjectAttempts[] =
{
	// The seat-back crease of the sofa (mesh 1.76 x 0.80 x 1.05 m): a hand pushed DOWN into the middle slice of
	// its box [R: the crease's place on the mesh is not measured; the slice covers seat to back cushions].
	{ "STUDY1", false, 4106, ATTEMPT_PUSH, 0.30, 0.60, "the sofa's crease (entry 14, examine #314 \"Nothing shoved down the upholstery.\")" },
	// The roll-top desk (mesh 114 x 68 x 80, OPENABLES E1): the drawers are its lower part; a grip there pulled
	// out (OPENABLES: no drawer opens anywhere, and this desk never opens).
	{ "STUDY1", false, 4113, ATTEMPT_TUG, 0.00, 0.60, "the roll-top desk's stuck drawers (OPENABLES E1, examine #288 \"The drawers are stuck.\")" },
};

struct FloorAttempt
{
	const char *map;
	int sectors[6];        // Realms sector indexes, -1 ends
	int kind;              // ATTEMPT_PRESS
	bool tip;              // the fingertip presses (else the palm is laid)
	double aboveM;         // the zone starts this far above the floor
	const char *what;
};
const FloorAttempt kFloorAttempts[] =
{
	{ "STUDY1", { 306, 313, 314, 315, 316, -1 }, ATTEMPT_PRESS, true, 0.02,
	  "the typewriter's keys (entry 21, examine #286 / #90 \"Ink ribbon's missing. / Some of the keys as well.\")" },
	{ "STUDY1", { 31, 257, -1 }, ATTEMPT_PRESS, false, 0.04,
	  "the boot prints, the open hand laid in one (entry 28, examine #241 \"Boots. Bet he shops at oversize.\")" },
};

struct CloudAttempt
{
	const char *map;
	bool shared; int art;            // the object examined (the armour)
	bool cloudShared; int cloudArt;  // the sprites the cloud is (the flies)
	double radiusM;                  // within this of a sprite is in the cloud (doc entry 5: 30 cm; the hand's metres)
	double gatherUnits;              // sprites this near the object's box in the plane (MAP units: the map's own
	                                 // relation, the same in flat and in the headset) are its cloud
	const char *what;
};
const CloudAttempt kCloudAttempts[] =
{
	{ "STUDY1", false, 4141, true, 198, 0.30, 96.0, "the flies on the armour (entry 5, examine #337 \"God what a stench!\")" },
};

struct WardKind { const char *map; bool shared; int art; const char *what; };
const WardKind kWardKinds[] =
{
	{ "STUDY1", false, 4207, "the green pentagram DEMO[4207]" },
	{ "STUDY1", false, 4208, "the tan pentagram DEMO[4208]" },
	{ nullptr,  true,  232,  "the green sigil ADEMO[232]" },
};

// THE DOOR SEARCH: a palm sweeping a door leaf (doc entries 10 and 26: "the palm sweeping the leaf's latch edge,
// searching for a lock, plays the examine"). Any Realms door leaf the open palm is on, whose examine the original
// offers there; a slower wave than the flies'.
constexpr double kSweepTouchM = 0.04, kSweepMps = 0.30;

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
};

enum ZoneShape { ZONE_BAND, ZONE_FLOOR, ZONE_CLOUD };

struct Zone
{
	ZoneShape shape = ZONE_BAND;
	int kind = ATTEMPT_PUSH;
	const char *what = "";
	AActor *target = nullptr;               // BAND, CLOUD: the object examined
	double bandLo = 0, bandHi = 1;
	std::vector<AActor *> cloud;            // CLOUD: the sprites
	double radiusM = 0;
	const FloorAttempt *floor = nullptr;    // FLOOR
	AttemptDetector det[2];
};

struct Motion { bool on = false; double from[3] = {}, to[3] = {}; int start = 0, tics = 0; };

struct HandG
{
	PunchReader punch;
	RapDetector rap;
	AttemptDetector sweep;
	long sweepKey = -1;
	bool havePrev = false;
	DVector3 prevPalm, prevTip;
	gesture::V3 prevRelPalm, prevRelTip;
	int prevTic = 0;
	int wardTic = -1000;
	int loggedWardCm = -1;
	Motion motion;
};

struct State
{
	FLevelLocals *level = nullptr;
	bool built = false;
	std::vector<Zone> zones;
	std::vector<std::pair<AActor *, const char *>> wards;
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
std::vector<Thing> Things(AActor *pmo)
{
	std::vector<Thing> out;
	auto it = S.level->GetThinkerIterator<AActor>();
	AActor *mo;
	while ((mo = it.Next()) != nullptr)
	{
		if (mo == pmo || (mo->ObjectFlags & OF_EuthanizeMe) || (mo->renderflags & RF_INVISIBLE)) continue;
		Thing t;
		if (!ObjectForActor(mo, t.sector, t.index)) continue;
		t.mo = mo;
		if (const Object *o = RuntimeObject(t.sector, t.index)) t.shared = Pack::ResolveObjectArt(o->textureIndex, o->textureSource, t.art);
		ObjectPickBox(mo, t.lo, t.hi);
		out.push_back(t);
	}
	return out;
}

const Thing *FindThing(const std::vector<Thing> &things, const AActor *mo)
{
	for (const Thing &t : things) if (t.mo == mo) return &t;
	return nullptr;
}

// The zones of this level, from the table (once a level, the first live tic).
void Build(const std::vector<Thing> &things)
{
	S.built = true;
	S.zones.clear();
	S.wards.clear();
	for (const ObjectAttempt &a : kObjectAttempts)
	{
		if (!MapIs(a.map)) continue;
		for (const Thing &t : things)
		{
			if (t.shared != a.shared || t.art != a.art) continue;
			Zone z;
			z.shape = ZONE_BAND; z.kind = a.kind; z.what = a.what; z.target = t.mo; z.bandLo = a.bandLo; z.bandHi = a.bandHi;
			for (AttemptDetector &d : z.det) d.kind = a.kind;
			S.zones.push_back(z);
		}
	}
	for (const FloorAttempt &a : kFloorAttempts)
	{
		if (!MapIs(a.map)) continue;
		Zone z;
		z.shape = ZONE_FLOOR; z.kind = a.kind; z.what = a.what; z.floor = &a;
		for (AttemptDetector &d : z.det) d.kind = a.kind;
		S.zones.push_back(z);
	}
	for (const CloudAttempt &a : kCloudAttempts)
	{
		if (!MapIs(a.map)) continue;
		for (const Thing &t : things)
		{
			if (t.shared != a.shared || t.art != a.art) continue;
			Zone z;
			z.shape = ZONE_CLOUD; z.kind = ATTEMPT_WAVE; z.what = a.what; z.target = t.mo; z.radiusM = a.radiusM;
			for (const Thing &f : things)
			{
				if (f.shared != a.cloudShared || f.art != a.cloudArt) continue;
				const DVector3 fc = (f.lo + f.hi) * 0.5;
				const double dx = std::max(0.0, std::max(t.lo.X - fc.X, fc.X - t.hi.X)), dy = std::max(0.0, std::max(t.lo.Y - fc.Y, fc.Y - t.hi.Y));
				if (sqrt(dx * dx + dy * dy) <= a.gatherUnits) z.cloud.push_back(f.mo);
			}
			for (AttemptDetector &d : z.det) d.kind = ATTEMPT_WAVE;
			if (!z.cloud.empty()) S.zones.push_back(z);
		}
	}
	for (const WardKind &w : kWardKinds)
	{
		if (!MapIs(w.map)) continue;
		for (const Thing &t : things) if (t.shared == w.shared && t.art == w.art) S.wards.push_back({ t.mo, w.what });
	}
	if (Logging())
	{
		Printf("[roth_gestures] %s: %d attempt zone(s), %d ward(s)\n", S.level->MapName.GetChars(), (int)S.zones.size(), (int)S.wards.size());
		for (const Zone &z : S.zones)
		{
			int s = -1, i = -1;
			if (z.target != nullptr) ObjectForActor(z.target, s, i);
			Printf("[roth_gestures]   %s %s%s\n", AttemptName(z.kind), z.what,
				z.target != nullptr ? FStringf(" -- object %d/%d%s", s, i, z.shape == ZONE_CLOUD ? FStringf(", %d fly sprite(s)", (int)z.cloud.size()).GetChars() : "").GetChars() : "");
		}
	}
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
	if (!PickFrom(pmo, from, d / dist, dist, p)) return true;
	return p.kind == Pick::Prop || p.dist >= dist - 1.0;
}

// The click a landed fist makes (roth_handclick.h), and the swing's end when it made one.
bool Strike(int role, HandG &g, const Pick &p, const DVector3 &contact, const DVector3 &palm, SecondaryVerb v, int type, int now, const char *what)
{
	if (v == SECONDARY_ATTACK_AIMED)
	{
		ActorsRequestFireAt(contact.X, contact.Y, contact.Z);
		Printf("[roth_gestures] %s hand punch (%.2f m/s average) lands on %s at (%.1f %.1f %.1f): cursor type %d -> the original's attack,"
			" aimed at the contact\n", RoleName(role), g.punch.avgMps, what, contact.X, contact.Y, contact.Z, type);
		Buzz(role, 0.5, 4 * 1000.0 / kTicRate, "a landed punch");
		g.punch.Landed(now);
		return true;
	}
	if (v == SECONDARY_USE)
	{
		const bool acted = VRPickUseAt(role, p, palm, "punch");
		Printf("[roth_gestures] %s hand punch (%.2f m/s average) meets %s: cursor type %d -> the original's use: %s\n", RoleName(role),
			g.punch.avgMps, what, type, acted ? "acted" : "nothing happened");
		if (!acted) return false;
		Buzz(role, 0.5, 4 * 1000.0 / kTicRate, "a punch worked it");
		g.punch.Landed(now);
		return true;
	}
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
		const gesture::V3 base((t.lo.X + t.hi.X) * 0.5, (t.lo.Y + t.hi.Y) * 0.5, t.lo.Z);
		const double radius = std::max(t.hi.X - t.lo.X, t.hi.Y - t.lo.Y) * 0.5;
		double dxy;
		gesture::V3 p;
		if (!SweptContact(gesture::V3(a.X, a.Y, a.Z), gesture::V3(b.X, b.Y, b.Z), gr, base, radius, t.hi.Z - t.lo.Z, dxy, p)) continue;
		if (dxy >= bestD) continue;
		const DVector3 at(p.x, p.y, p.z);
		Pick pk;
		pk.kind = Pick::Prop;
		pk.actor = t.mo;
		pk.hitPos = BoxClosest(at, t.lo, t.hi);
		pk.dist = (pk.hitPos - a).Length();
		const int type = VRPickCursorType(role, pk, b);
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
		Strike(role, g, bestPick, bestP, b, SecondaryClick(bestType), bestType, now,
			FStringf("object %d/%d", best->sector, best->index).GetChars());                // 21
		return;
	}
	// 22: no body -- what the fist meets along its travel this tic (a switch, a door, the glass).
	const DVector3 mv = b - a;
	const double len = mv.Length();
	Pick p;
	if (len < 1e-6 || !PickFrom(pmo, a, mv / len, len + gr, p))
	{
		if (Logging()) Printf("[roth_gestures] %s hand punch (%.2f m/s average): meets nothing\n", RoleName(role), g.punch.avgMps);
		return;
	}
	const int type = VRPickCursorType(role, p, b);
	const SecondaryVerb v = SecondaryClick(type);
	if (v == SECONDARY_NOTHING)
	{
		if (Logging()) Printf("[roth_gestures] %s hand punch (%.2f m/s average) meets %s: cursor type %d -> the original's click does nothing there\n",
			RoleName(role), g.punch.avgMps, PickKindName(p), type);
		return;
	}
	Strike(role, g, p, p.hitPos, b, v, type, now, PickKindName(p));                  // 23
}

//===========================================================================
// The knock
//===========================================================================

// What an open hand is nearest within kRapNearM: a wall face or door leaf, an object, or the floor under it.
// `key` names it for the detector; `pick` is the original's thing to examine.
bool RapTarget(AActor *pmo, const std::vector<Thing> &things, const DVector3 &palm, const DVector3 &tip, double upm, long &key, double &distM, Pick &pick)
{
	key = -1;
	distM = 1e30;
	const DVector3 pts[2] = { palm, tip };
	for (const DVector3 &P : pts)
	{
		Pick w;
		if (PickTouchSurface(pmo, P, kRapNearM * upm, w))
		{
			const double d = w.dist / upm;
			if (d < distM) { distM = d; pick = w; key = (long)(w.line != nullptr ? w.line->Index() : 0) + (1L << 24); }
		}
		for (const Thing &t : things)
		{
			const double d = BoxDistanceM(P, t.lo, t.hi, upm);
			if (d >= distM || d > kRapNearM) continue;
			distM = d;
			pick = Pick{};
			pick.kind = Pick::Prop;
			pick.actor = t.mo;
			pick.hitPos = BoxClosest(P, t.lo, t.hi);
			pick.dist = d * upm;
			key = ((long)t.sector << 10) + t.index + (2L << 24);
		}
		if (sector_t *sec = S.level->PointInSector(P.XY()))
		{
			const double fz = sec->floorplane.ZatPoint(P.XY());
			const double d = (P.Z - fz) * Stretch() / upm;
			if (d >= -0.05 && std::max(d, 0.0) < distM && d <= kRapNearM)
			{
				distM = std::max(d, 0.0);
				pick = Pick{};
				pick.kind = Pick::Flat;
				pick.sector = sec;
				pick.hitPos = DVector3(P.X, P.Y, fz);
				pick.dist = distM * upm;
				key = (long)sec->Index() + (3L << 24);
			}
		}
	}
	return key >= 0;
}

//===========================================================================
// The attempts
//===========================================================================

AttemptSample SampleZone(const Zone &z, const DVector3 &palm, const DVector3 &tip, const gesture::V3 &velPalm, const gesture::V3 &velTip,
	bool grip, const std::vector<Thing> &things, double upm, Pick &pick)
{
	AttemptSample s;
	s.grip = grip;
	if (z.shape == ZONE_BAND)
	{
		const Thing *t = FindThing(things, z.target);
		if (t == nullptr) return s;
		const double h = t->hi.Z - t->lo.Z;
		const DVector3 lo(t->lo.X, t->lo.Y, t->lo.Z + h * z.bandLo), hi(t->hi.X, t->hi.Y, t->lo.Z + h * z.bandHi);
		pick = Pick{};
		pick.kind = Pick::Prop;
		pick.actor = t->mo;
		pick.hitPos = BoxClosest(palm, lo, hi);
		if (z.kind == ATTEMPT_TUG)
		{
			// the grip closes on the drawers (within 3 cm of the slice), then pulls OUT from the object's middle
			s.outside = std::max(0.0, BoxDistanceM(palm, lo, hi, upm) - 0.03);
			const DVector3 c = (t->lo + t->hi) * 0.5;
			DVector3 out(palm.X - c.X, palm.Y - c.Y, 0.0);
			const double ol = out.Length();
			s.depth = ol / upm;
			s.axisMps = ol > 1e-6 ? (velPalm.x * out.X + velPalm.y * out.Y) / ol : 0.0;
		}
		else
		{
			s.outside = BoxDistanceM(palm, lo, hi, upm);
			s.depth = (hi.Z - palm.Z) * Stretch() / upm;    // pushed DOWN from the slice's top
			s.axisMps = -velPalm.z;
		}
		s.mps = velPalm.Length();
		return s;
	}
	if (z.shape == ZONE_CLOUD)
	{
		const Thing *t = FindThing(things, z.target);
		if (t == nullptr) return s;
		double best = 1e30;
		for (AActor *f : z.cloud)
		{
			const Thing *ft = FindThing(things, f);
			if (ft == nullptr) continue;
			best = std::min(best, Metres(palm - (ft->lo + ft->hi) * 0.5, upm).Length() - z.radiusM);
		}
		s.outside = std::max(0.0, best);
		s.mps = velPalm.Length();
		pick = Pick{};
		pick.kind = Pick::Prop;
		pick.actor = t->mo;
		pick.hitPos = BoxClosest(palm, t->lo, t->hi);
		return s;
	}
	// ZONE_FLOOR: the point over one of the listed sectors, within aboveM of its floor.
	const DVector3 &P = z.floor->tip ? tip : palm;
	const gesture::V3 &v = z.floor->tip ? velTip : velPalm;
	sector_t *sec = S.level->PointInSector(P.XY());
	bool listed = false;
	for (int k = 0; k < 6 && z.floor->sectors[k] >= 0; k++) if (sec != nullptr && sec->Index() == z.floor->sectors[k]) listed = true;
	if (!listed) return s;
	const double fz = sec->floorplane.ZatPoint(P.XY());
	const double above = (P.Z - fz) * Stretch() / upm;
	s.outside = std::max(0.0, above - z.floor->aboveM);
	s.depth = z.floor->aboveM - above;
	s.axisMps = -v.z;
	s.mps = v.Length();
	pick = Pick{};
	pick.kind = Pick::Flat;
	pick.sector = sec;
	pick.hitPos = DVector3(P.X, P.Y, fz);
	return s;
}

void Attempted(int role, const Pick &p, const DVector3 &from, const char *what, int kind)
{
	FString how;
	how.Format("attempt (%s: %s)", AttemptName(kind), what);
	const bool acted = VRPickExamineAt(role, p, from, how.GetChars());
	Buzz(role, acted ? 0.25 : 0.10, acted ? 25 : 10, "an attempt");
}

//===========================================================================
// One hand, one tic
//===========================================================================

void Forget(HandG &g)
{
	g.punch.Invalidate();
	g.rap.Reset();
	g.sweep.Reset();
	g.sweepKey = -1;
	g.havePrev = false;
}

bool HandIsEmpty(int role)
{
	if (HandHoldsWeapon(role)) return false;
	if (VRHandPose_SideForRole(role) == VRHandPose::SIDE_RIGHT)
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
	const bool velOk = g.havePrev && now - g.prevTic >= 1 && now - g.prevTic <= kMaxGap;
	if (velOk)
	{
		const double k = (double)kTicRate / (now - g.prevTic);
		velPalm = (relPalm - g.prevRelPalm) * k;
		velTip = (relTip - g.prevRelTip) * k;
	}
	const bool fist = h.gripClosed && !HandHoldsWeapon(role);
	const bool open = !h.gripClosed && h.grip <= 0.35f && h.trigger <= 0.35f && HandIsEmpty(role);

	// ---- THE PUNCH ----
	const bool swing = g.punch.Feed(fist, relPalm, now, pmo->Angles.Yaw.Degrees(), Stretch());
	if (swing && g.havePrev) PunchContact(role, g, pmo, things, g.prevPalm, h.palm, upm, now);

	// ---- THE KNOCK ----
	if (velOk || !g.havePrev)
	{
		long key = -1;
		double d = 1e30;
		Pick target;
		const bool near = open && RapTarget(pmo, things, h.palm, h.tip, upm, key, d, target);
		const int r = g.rap.Feed(open, near ? key : -1, d, now);
		if (r == 1)
		{
			if (Logging()) Printf("[roth_gestures] %s hand: a rap on %s (%.2f m/s), the first\n", RoleName(role), PickKindName(target), g.rap.closing);
			Buzz(role, 0.12, 10, "a rap");
		}
		else if (r == 2)
		{
			Printf("[roth_gestures] %s hand: a second rap on %s (%.2f m/s) -- the knock: the original's examine\n", RoleName(role), PickKindName(target), g.rap.closing);
			const bool acted = VRPickExamineAt(role, target, h.palm, "knock (two raps)");
			Buzz(role, acted ? 0.2 : 0.1, 15, "the knock");
		}
	}

	// ---- THE ATTEMPTS ----
	if (velOk)
	{
		for (Zone &z : S.zones)
		{
			Pick p;
			const AttemptSample s = SampleZone(z, h.palm, h.tip, velPalm, velTip, h.gripClosed, things, upm, p);
			if (z.det[role].Feed(s) && p.kind != Pick::Nothing) Attempted(role, p, z.shape == ZONE_FLOOR && z.floor->tip ? h.tip : h.palm, z.what, z.kind);
		}
		// the door search: an open palm on a door leaf, sweeping
		Pick door;
		const bool onDoor = open && ((PickTouchSurface(pmo, h.palm, kSweepTouchM * upm, door) && door.kind == Pick::DoorLeaf)
			|| (PickTouchSurface(pmo, h.tip, kSweepTouchM * upm, door) && door.kind == Pick::DoorLeaf));
		const long key = onDoor && door.line != nullptr ? door.line->Index() : -1;
		if (key != g.sweepKey) { g.sweep.Reset(); g.sweep.kind = ATTEMPT_WAVE; g.sweep.waveMps = kSweepMps; g.sweepKey = key; }
		AttemptSample s;
		s.outside = onDoor ? 0.0 : 1e9;
		s.mps = velPalm.Length();
		if (g.sweep.Feed(s) && onDoor) Attempted(role, door, h.palm, "the palm searching the door for its lock (entries 10, 26)", ATTEMPT_WAVE);
	}

	// ---- THE WARD TINGLE ----
	{
		double best = 1e30;
		const char *which = nullptr;
		for (const auto &w : S.wards)
		{
			const Thing *t = FindThing(things, w.first);
			if (t == nullptr) continue;
			const double d = BoxDistanceM(h.palm, t->lo, t->hi, upm);
			if (d < best) { best = d; which = w.second; }
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
		Motion &m = S.hand[role].motion;
		if (!m.on || S.level == nullptr) continue;
		const int side = VRHandPose_SideForRole(role);
		const int t = S.level->maptime - m.start;
		const double f = m.tics <= 0 ? 1.0 : std::min(1.0, (double)t / m.tics);
		double o[3];
		for (int k = 0; k < 3; k++) o[k] = m.from[k] + (m.to[k] - m.from[k]) * f;
		VRHandTest_SetOffset(side, o[0], o[1], o[2]);
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
	const std::vector<Thing> things = Things(pmo);
	if (!S.built) Build(things);
	for (int role = 0; role < 2; role++) TickHand(role, pmo, things);
}

void GesturesEndLevel()
{
	S.level = nullptr;
	S.built = false;
	S.zones.clear();
	S.wards.clear();
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
void PlaceHand(AActor *pmo, int role, bool tip, const DVector3 &want, const DVector3 &look)
{
	DVector3 now;
	if (!TestPoint(role, tip, now)) { Printf("roth_gesturetest reach: no test hand (roth_vrpicktest on, roth_handtest first)\n"); return; }
	const double yaw0 = pmo->Angles.Yaw.Radians();
	const DVector3 rel = now - pmo->Pos();
	const double fwd = rel.X * cos(yaw0) + rel.Y * sin(yaw0), left = -rel.X * sin(yaw0) + rel.Y * cos(yaw0);
	DVector3 face(look.X - want.X, look.Y - want.Y, 0.0);
	double yaw = face.Length() > 1e-3 ? atan2(face.Y, face.X) : yaw0;
	const DVector3 stand(want.X - (fwd * cos(yaw) - left * sin(yaw)), want.Y - (fwd * sin(yaw) + left * cos(yaw)), pmo->Z());
	const double oldZ = pmo->Z();
	pmo->Angles.Yaw = DAngle::fromRad(yaw);
	pmo->Angles.Pitch = nullAngle;
	pmo->SetOrigin(stand, false);
	pmo->SetZ(pmo->floorz);
	pmo->Vel.Zero();
	pmo->ClearInterpolation();
	const double upm = VRPickUnitsPerMetre();
	const double dz = want.Z - (now.Z + (pmo->Z() - oldZ));
	const double ps = S.level != nullptr && S.level->pixelstretch > 0 ? S.level->pixelstretch : 1.0;
	VRHandTest_SetLift(VRHandTest_Lift() + dz * ps / upm);
	Printf("roth_gesturetest reach: the player stands at (%.0f %.0f %.0f) in sector %d facing %.0f; the %s hand's %s lifted %+.1f units"
		" to (%.1f %.1f %.1f)\n", pmo->X(), pmo->Y(), pmo->Z(), pmo->Sector ? pmo->Sector->Index() : -1, pmo->Angles.Yaw.Degrees(),
		RoleName(role), tip ? "fingertip" : "palm", dz, want.X, want.Y, want.Z);
}

} // namespace
} // namespace roth

CCMD(roth_gesturetest)
{
	using namespace roth;
	if (argv.argc() < 2)
	{
		Printf("roth_gesturetest move <main|off> <fwd> <right> <up> [tics] | reach <main|off> <palm|tip> thing <shared> <art> [band] [metres]"
			" | reach <main|off> <palm|tip> sector <n> [metres] | reach <main|off> <palm|tip> creature [metres] | home\n");
		return;
	}
	AActor *pmo = players[consoleplayer].mo;
	if (!stricmp(argv[1], "home"))
	{
		for (int side = 0; side < 2; side++) VRHandTest_SetOffset(side, 0, 0, 0);
		for (HandG &g : S.hand) g.motion.on = false;
		Printf("roth_gesturetest: both test hands at their usual place\n");
		return;
	}
	if (argv.argc() < 3) { Printf("roth_gesturetest: which hand?\n"); return; }
	const int role = !stricmp(argv[2], "off") ? VR_OFFHAND : VR_MAINHAND;
	if (!stricmp(argv[1], "move") && argv.argc() >= 6)
	{
		Motion &m = S.hand[role].motion;
		const int side = VRHandPose_SideForRole(role);
		VRHandTest_Offset(side, m.from[0], m.from[1], m.from[2]);
		m.to[0] = atof(argv[3]); m.to[1] = atof(argv[4]); m.to[2] = atof(argv[5]);
		m.tics = argv.argc() >= 7 ? atoi(argv[6]) : 0;
		m.start = S.level != nullptr ? S.level->maptime : 0;
		m.on = true;
		const double dist = sqrt((m.to[0] - m.from[0]) * (m.to[0] - m.from[0]) + (m.to[1] - m.from[1]) * (m.to[1] - m.from[1])
			+ (m.to[2] - m.from[2]) * (m.to[2] - m.from[2]));
		Printf("roth_gesturetest move: the %s hand from (%.2f %.2f %.2f) to (%.2f %.2f %.2f) m in %d tic(s): %.2f m%s\n", RoleName(role),
			m.from[0], m.from[1], m.from[2], m.to[0], m.to[1], m.to[2], m.tics, dist,
			m.tics > 0 ? FStringf(", %.2f m/s", dist * kTicRate / m.tics).GetChars() : "");
		if (m.tics <= 0) { VRHandTest_SetOffset(side, m.to[0], m.to[1], m.to[2]); m.on = false; }
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
			PlaceHand(pmo, role, tip, want, look);
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
				if (VRPickCursorType(role, p, pmo->Pos()) != 5) continue;
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
		const double half = std::max(t->hi.X - t->lo.X, t->hi.Y - t->lo.Y) * 0.5;
		const DVector3 face(c.X + toward.X * half, c.Y + toward.Y * half, z);
		const DVector3 want = face + toward * (metres * upm);
		Printf("roth_gesturetest reach: object %d/%d (%s[%d]), its face at (%.1f %.1f %.1f)\n", t->sector, t->index, t->shared ? "ADEMO" : "DEMO",
			t->art, face.X, face.Y, face.Z);
		PlaceHand(pmo, role, tip, want, DVector3(c.X, c.Y, z));
		return;
	}
	Printf("roth_gesturetest: unknown or short '%s'\n", argv[1]);
}
