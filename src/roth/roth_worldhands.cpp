// roth_worldhands.cpp -- see roth_worldhands.h (what each mechanism does and which click it ends in) and
// roth_worldhandslogic.h (the rules, engine-free, tools/worldhands_test).
//
// PORT CHECKLIST -- WHAT EACH MECHANISM ENDS IN, record by record (ROTH.C is the specification of every click; none
// of its logic is ported here: each mechanism calls the port's existing click path, already diffed against it).
// Records are per map (docs/VR_CHAPTER02_SIGNS_AND_PORTENTS.md section 10; chains read with world_census -chain).
//  1. the dais plates STUDY1 #119, #124-#129 (0x19 use-floor, key 13, each its own box on DEMO[66]) <- THE RELEASE
//     of a Statuette (i4): the floor use at the base -> VRPickUseAt -> Activate(Flat) -> UseFlatAt -> UseMatchFlat
//     (roth_surfbox.h: dispatch_entry_command_trigger RC:3058-3085, the box RC:3069-3072) -> FireUseRecord.
//  2. the placed statuettes #121, #136-#141 (0x08 click-obj) <- the grip in reach (the picker's NearAct, unchanged):
//     ActivateObject (run_leftclick_object_trigger RC:2920-2968). Nothing new here.
//  3. the Orb door MAS3 #4 (0x18 use-wall, key 2, box x 26..41 y 40..55, once) <- THE RELEASE of the Orb (i18) at
//     the face: Activate(WallFace) -> ActivateLine -> UseMatchFace (RC:3027-3045). #24 (the same face and box,
//     off until #4's chain enables it) <- the grip in reach on the face (NearAct, unchanged).
//  4. Aelf's door AELF #80 (0x18, wall id 6, no box) and the MAS3 recess #113 (wall id 6, no box) <- THE TWIST of the
//     Shrive (i14), 180 degrees: the face's use from the item hand (its 0x27 tests the Shrive in hand).
//  5. the lock lever MAUSO1EA #76 (0x08 click-obj, obj id 6) <- THE TWIST of Key i22, 30 degrees: the object's use
//     from the item hand (its 0x27 tests i22 in hand).
//  6. the levers MAS4 #3 (obj 1), MAUSO1EA #18 (obj 10), #30 (obj 5), #101 (obj 8), MAS6 #20 (obj 3): 0x08 click-obj
//     <- THE PULL, 8 cm: ActivateObject. The pit's chains MAS7 #22 (0x18, wall id 4, a count to 3) <- the pull,
//     8 cm, each pull one use.
//  7. the bookcase STUDY1 #353 (wall id 38), #420 (42), from the passage #75 (43), #354 (39): 0x18 <- THE PULL from
//     a leaf's edge, 4 cm. The clock STUDY1 #35 (0x18, wall id 23, no box) <- the pull, 4 cm (the case door).
//  8. MAS6's hand prints #42 (obj 5), #8 (obj 2), #41 (obj 4): 0x08 click-obj <- THE PRESS, 2 cm: ActivateObject.
//  9. the throne MAS7 #7 (0x19 use-floor, key 8, floor, once) / #33 (0x19, key 8, +6 bit 0: the platform top)
//     <- THE SIT: the use on the seat where a pointer straight down meets it (the floor or the platform top: the
//     record's own +6 choice, RC:3068).
// 10. the Vials i143, i206, i170, i199 (every item named "Vial", item_dossier) <- THE DRINK: use_item_on_self
//     (inventory.c:691-776) by the inventory's own clicks (InterfaceUseRightHandOnSelf, dialogue_ui.c:1704-1747).
// 11. the contact disc (Q21): the point moves within the disc; the click is the same dispatcher's at that point.
// NOT HERE: the masks (STUDY1 #351, #301, #171, 0x08 click-obj giving i25 / i24 / i26) are lifted by the picker's
// grip in reach, unchanged; the attempts and the ward tingle are roth_gestures.cpp's.
//
// WHO OWNS A GRIP (with RSQ Distance Grab, roth_grabglue.h GrabGlueTakesGrip, 2026-11-16):
//  - an item LYING in the world that the grab offers (ObjectIsPickupItem: the type-4 pickup, or a click chain whose
//    0x29 gives that item) is the PACKAGE'S to take: its walk-up grip (the press) and its palm-out pull, its catch
//    being the original's use of it (VRPickTakeCaught -> DoUse -> the object's own click chain). In chapter 2 that
//    is every lying statuette (#23, #58, #74, #540, AELF #102), the statuettes standing on the plates (#121,
//    #136-#141: the catch runs that click -- given back, the count stepped down, the plate re-enabled; Q3), the
//    Orb on its ledge (#1) and the vials (tools/dgrab_test: STUDY1 28/28, MAS3 20/20, AELF 10/10 offered).
//  - an item HELD (the original's right-hand item, drawn in the item hand) is no world object: only this file acts
//    on it -- the RELEASE (a grip opening: the package takes on a close, and has no carry in the Realms, its catch
//    being taken at once by the original's use), the TWIST and the DRINK.
//  - a grip CLOSING is asked in this order (roth_vrpick.cpp TickHand): the Lamp's bail (HeldGripTaken), the package
//    (GrabGlueTakesGrip: it would take an offered item with that press, or carries one), this file
//    (WorldHandsGripTaken: a lever, a chain, a leaf's edge, the clock, a socket), then the picker's use. A press the
//    package takes is not also a pull or a twist; the twist can still be set by the closed hand moving into the
//    socket afterwards (no press), as by any closed hand.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roth_worldhands.h"
#include "roth_worldhandslogic.h"
#include "roth_vrpick.h"
#include "roth_runtime.h"
#include "roth_objects.h"
#include "roth_interface.h"
#include "roth_inventory.h"
#include "roth_held.h"
#include "roth_raw.h"
#include "roth_grabglue.h"
#include "roth_gestures.h"

#include "actor.h"
#include "actorinlines.h"
#include "p_3dfloors.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "hw_vrmodes.h"
#include "r_utility.h"
#include "r_data/vr_handpose.h"
#include "r_data/vr_handpose_math.h"
#include "r_data/vr_posemath.h"

#include <math.h>
#include <algorithm>
#include <vector>
#include <map>

EXTERN_CVAR(Bool, roth_vrpick_debug)   // roth_vrpick.cpp; file scope, never inside a namespace

namespace roth
{

namespace
{

using wh::V3;

//===========================================================================
// THE TABLES (the owner's answers; the records named, per map)
//===========================================================================

struct PullRecord { const char *map; uint16_t record; double metres; bool edge; const char *what; };
const PullRecord kPulls[] =
{
	{ "MAS4",     3,   wh::kLeverPullM, false, "the store-room lever (entry 48, MAS4 #3)" },
	{ "MAUSO1EA", 18,  wh::kLeverPullM, false, "the lever opening s425 (entry 38, MAUSO1EA #18)" },
	{ "MAUSO1EA", 30,  wh::kLeverPullM, false, "the lever opening s213 (entry 38, MAUSO1EA #30)" },
	{ "MAUSO1EA", 101, wh::kLeverPullM, false, "the lever moving sector id 28 (entry 38, MAUSO1EA #101)" },
	{ "MAS6",     20,  wh::kLeverPullM, false, "the gauntlet's lever (entry 52, MAS6 #20)" },
	{ "MAS7",     22,  wh::kLeverPullM, false, "the pit's chains (entry 54, MAS7 #22: three pulls open it)" },
	{ "STUDY1",   353, wh::kLeafPullM,  true,  "a bookcase leaf from the Study (entry 21, #353)" },
	{ "STUDY1",   420, wh::kLeafPullM,  true,  "a bookcase leaf from the Study (entry 21, #420)" },
	{ "STUDY1",   75,  wh::kLeafPullM,  true,  "a bookcase leaf from the passage (entry 65, #75)" },
	{ "STUDY1",   354, wh::kLeafPullM,  true,  "a bookcase leaf from the passage (entry 65, #354)" },
	{ "STUDY1",   35,  wh::kLeafPullM,  false, "the grandfather clock's case door (entry 11, #35; Q24)" },
};

struct PressRecord { const char *map; uint16_t record; const char *what; };
const PressRecord kPresses[] =
{
	{ "MAS6", 42, "the hand print obj 5 (entry 51, MAS6 #42)" },
	{ "MAS6", 8,  "the hand print obj 2 (entry 51, MAS6 #8)" },
	{ "MAS6", 41, "the hand print obj 4 (entry 51, MAS6 #41)" },
};

struct SeatRecord { const char *map; uint16_t sectorId; const char *what; };
const SeatRecord kSeats[] = { { "MAS7", 8, "the throne's seat (entry 55, MAS7 #7 / #33: BRANDED)" } };

struct ReleaseItem { int item; bool base; const char *what; };
const ReleaseItem kReleases[] = { { 4, true, "the Statuette (its base)" }, { 18, false, "the Orb" } };

struct TwistItem { int item; double degrees; const char *what; };
const TwistItem kTwists[] = { { 14, wh::kShriveTwistDeg, "the Shrive" }, { 22, wh::kKeyTurnDeg, "Key i22" } };

const int kVials[] = { 143, 206, 170, 199 };

constexpr double kPullGripM = 0.10;   // a grip within this of a lever's box takes it [R]

//===========================================================================
// State
//===========================================================================

struct PullObj { int sector = -1, index = -1; const PullRecord *rec = nullptr; };
struct PrintObj
{
	int sector = -1, index = -1;
	const PressRecord *rec = nullptr;
	bool plane = false;
	DVector2 wallAt, n, along;   // a point of the wall behind it, its normal toward the room, along the wall
	double offsetM = 0;          // the print's face in front of the wall
	double a0 = 0, a1 = 0;       // its extent along the wall (metres from wallAt)
	double z0 = 0, z1 = 0;       // its extent in height (map units)
	wh::Press press[2];
};
struct Seat
{
	const SeatRecord *rec = nullptr;
	std::vector<int> sectors;
	DVector2 lo, hi, centre, front;
	double topZ = 0;
};

struct HandW
{
	wh::ReleaseEdge release;
	wh::Pull pull;
	Pick pullPick;
	DVector3 pullFrom;
	const PullRecord *pullRec = nullptr;
	int pullLoggedCm = -1;
	wh::Twist twist;
	Pick socket;
	DVector3 socketAt;
	const TwistItem *twistItem = nullptr;
	int twistLogged = 0;
	wh::Drink drink;
	bool claimed = false;
	int loggedDrink = -1;
};

struct State
{
	FLevelLocals *level = nullptr;
	bool built = false;
	std::vector<PullObj> pullObjs;
	std::vector<PrintObj> prints;
	std::vector<Seat> seats;
	wh::Sit sit;
	int loggedSitBand = -1;
	HandW hand[2];
} S;

const char *RoleName(int role) { return role == VR_MAINHAND ? "main" : "off"; }
bool Logging() { return VRPickTesting() || roth_vrpick_debug || VRPoseTest_Active(); }
bool MapIs(const char *name) { return S.level != nullptr && S.level->MapName.CompareNoCase(name) == 0; }
double UPM() { return VRPickUnitsPerMetre(); }
double PS() { return S.level != nullptr && S.level->pixelstretch > 0.1 ? S.level->pixelstretch : 1.2; }

// A map point / offset in real metres (the vertical through the pixel stretch), and back.
V3 M(const DVector3 &d) { return V3(d.X / UPM(), d.Y / UPM(), d.Z * PS() / UPM()); }
DVector3 Units(const V3 &m) { return DVector3(m.x * UPM(), m.y * UPM(), m.z * UPM() / PS()); }
// A map-space direction, made metric and unit.
V3 Dir(const DVector3 &d) { return V3(d.X, d.Y, d.Z * PS()).Unit(); }

void Buzz(int role, double amp, double ms)
{
	if (VRHandPose_InputLive() && !VRPoseTest_Active()) VR_ScriptHaptic(role, amp, ms);
}

int ItemRole() { return VRHandPose_RoleForSide(VRHandPose::SIDE_RIGHT); }

int RightItem()
{
	const Inventory *inv = InterfaceInventory();
	if (inv == nullptr || inv->RightHand() < 0) return 0;
	return (int16_t)inv->Slot(inv->RightHand()).id;
}

DVector3 BoxClosest(const DVector3 &p, const DVector3 &lo, const DVector3 &hi)
{
	return DVector3(std::clamp(p.X, lo.X, hi.X), std::clamp(p.Y, lo.Y, hi.Y), std::clamp(p.Z, lo.Z, hi.Z));
}
double BoxDistM(const DVector3 &p, const DVector3 &lo, const DVector3 &hi) { return M(p - BoxClosest(p, lo, hi)).Length(); }

// THE FLAT UNDER A POINT, without a trace (2026-11-16, the PC runs: a pointer straight down met no flat at all in
// thousands of tries -- a vertical ray is not one the picker's trace answers): the sector's floor or a mid-platform's
// top (a 3D floor of that sector, Pick::ffloor, the original's type 8) nearest the point, the point at most `above`
// map units over it and at most `below` under it. A Flat pick, as the picker's trace would have made it.
bool FlatAt(const DVector3 &pt, double above, double below, Pick &out)
{
	if (S.level == nullptr) return false;
	sector_t *sec = S.level->PointInSector(pt.XY());
	if (sec == nullptr) return false;
	double bestD = 1e30, bestZ = 0;
	const F3DFloor *bestFF = nullptr;
	auto consider = [&](double z, const F3DFloor *ff) {
		const double d = pt.Z - z;
		if (d > above || d < -below) return;
		if (fabs(d) < bestD) { bestD = fabs(d); bestZ = z; bestFF = ff; }
	};
	consider(sec->floorplane.ZatPoint(pt.XY()), nullptr);
	if (sec->e != nullptr)
		for (F3DFloor *ff : sec->e->XFloor.ffloors)
			if (ff != nullptr && (ff->flags & FF_EXISTS) && ff->top.plane != nullptr) consider(ff->top.plane->ZatPoint(pt.XY()), ff);
	if (bestD >= 1e29) return false;
	out = Pick{};
	out.kind = Pick::Flat;
	out.sector = sec;
	out.ffloor = bestFF;
	out.ceiling = false;
	out.hitPos = DVector3(pt.X, pt.Y, bestZ);
	out.dist = bestD;
	return true;
}

// A WALL-MOUNTED OBJECT's facing (a hand print, a lever, a mask: fixed-angle quads on a wall): its actor's yaw, turned
// to the open side (the side where a point 16 units out lies in a sector whose floor and ceiling hold the object's
// middle). False: neither side is open.
bool ObjectFacing(AActor *mo, const DVector3 &centre, DVector2 &n)
{
	const DVector2 f(mo->Angles.Yaw.Cos(), mo->Angles.Yaw.Sin());
	auto open = [&](const DVector2 &d) {
		const DVector2 q = centre.XY() + d * 16.0;
		sector_t *s2 = S.level->PointInSector(q);
		return s2 != nullptr && s2->floorplane.ZatPoint(q) <= centre.Z && s2->ceilingplane.ZatPoint(q) >= centre.Z;
	};
	const bool a = open(f), b = open(-f);
	if (!a && !b) return false;
	n = (a && !b) || (a && b) ? f : -f;
	return true;
}

DVector3 EyeNow(AActor *pmo)
{
	if (r_viewpoint.ViewLevel == S.level) return r_viewpoint.Pos;
	return pmo->Pos() + DVector3(0, 0, pmo->player != nullptr ? pmo->player->viewheight : 41.0);
}

// The face's normal in the plane, turned toward the player's side.
bool FaceNormal(const Pick &p, AActor *pmo, DVector2 &n)
{
	if (p.line == nullptr) return false;
	const DVector2 d = p.line->delta;
	const double l = d.Length();
	if (l < 1e-6) return false;
	n = DVector2(d.Y, -d.X) / l;
	if (((pmo->Pos().XY() - p.hitPos.XY()) | n) < 0) n = -n;
	return true;
}

// The hand now: its drawn points (the picker's FingerRay) and its frame (VRHandPose_MapFrame).
struct HandNow
{
	bool grip = false, frame = false;
	DVector3 palm, tip, handFwd;
	V3 fwd, up;
};
bool ReadHand(int role, HandNow &h)
{
	VRPickHand v;
	if (!VRPickHandNow(role, v) || !v.havePoints) return false;
	h.grip = v.gripClosed;
	h.palm = v.palm;
	h.tip = v.tip;
	h.handFwd = v.handFwd;
	DVector3 o, f, r, u;
	h.frame = VRHandPose_MapFrame(role, o, f, r, u);
	if (h.frame) { h.fwd = Dir(f); h.up = Dir(u); }
	return true;
}

// Every Realms object's actor (one per object), shown (or hidden too: a lever a chain shows later, MAS6 #20).
template<class F> void ForObjects(F f, bool hiddenToo = false)
{
	auto it = S.level->GetThinkerIterator<AActor>();
	AActor *mo;
	while ((mo = it.Next()) != nullptr)
	{
		if ((mo->ObjectFlags & OF_EuthanizeMe) || (!hiddenToo && (mo->renderflags & RF_INVISIBLE))) continue;
		int s = -1, i = -1;
		if (!ObjectForActor(mo, s, i) || FindObjectActor(s, i) != mo) continue;
		f(mo, s, i);
	}
}

//===========================================================================
// The contact disc (Q21)
//===========================================================================

bool Disc(const Pick &p, double radiusM, bool use, Pick &out)
{
	if (S.level == nullptr) return false;
	auto rec = [use](const Pick &q) { return use ? UseRecordOf(q) : ExamineRecordOf(q); };
	const double upm = UPM();
	if (p.kind == Pick::Flat)
	{
		if (p.sector == nullptr || (use && p.ceiling)) return false;
		for (const auto &o : wh::DiscOffsets(radiusM))
		{
			Pick q = p;
			const DVector2 xy(p.hitPos.X + o.first * upm, p.hitPos.Y + o.second * upm);
			sector_t *sec = S.level->PointInSector(xy);
			if (sec == nullptr) continue;
			if (sec != p.sector) { q.sector = sec; q.ffloor = nullptr; }
			const double z = q.ffloor != nullptr ? p.hitPos.Z : (p.ceiling ? sec->ceilingplane.ZatPoint(xy) : sec->floorplane.ZatPoint(xy));
			q.hitPos = DVector3(xy.X, xy.Y, z);
			if (rec(q) != 0) { out = q; return true; }
		}
		return false;
	}
	if (p.kind == Pick::WallFace || p.kind == Pick::DoorLeaf)
	{
		if (p.line == nullptr) return false;
		const DVector2 d = p.line->delta;
		const double len = d.Length();
		if (len < 1e-6) return false;
		const DVector2 a = d / len;
		const DVector2 v1 = p.line->v1->fPos();
		for (const auto &o : wh::DiscOffsets(radiusM))
		{
			Pick q = p;
			q.hitPos = DVector3(p.hitPos.X + a.X * o.first * upm, p.hitPos.Y + a.Y * o.first * upm, p.hitPos.Z + o.second * upm / PS());
			const double t = ((q.hitPos.XY() - v1) | a) / len;
			if (t < 0 || t > 1) continue;
			if (rec(q) != 0) { out = q; return true; }
		}
		return false;
	}
	return false;
}

//===========================================================================
// Build (once a level, the first live tic)
//===========================================================================

void Build(AActor *pmo)
{
	S.built = true;
	S.pullObjs.clear();
	S.prints.clear();
	S.seats.clear();
	const double upm = UPM();
	auto objectsWithId = [](uint16_t id, std::vector<std::pair<int, int>> &out) {
		out.clear();
		ForObjects([&](AActor *, int s, int i) {
			const Object *o = RuntimeObject(s, i);
			if (o != nullptr && o->commandID == id) out.push_back({ s, i });
		}, true);
	};
	int facePulls = 0;
	for (const PullRecord &r : kPulls)
	{
		if (!MapIs(r.map)) continue;
		uint8_t op = 0;
		uint16_t key = 0;
		if (!RecordKey(r.record, op, key)) { Printf("[roth_worldhands] %s: record #%u not in this map\n", r.what, r.record); continue; }
		if (op == 0x08)
		{
			std::vector<std::pair<int, int>> objs;
			objectsWithId(key, objs);
			for (const auto &o : objs) { PullObj p; p.sector = o.first; p.index = o.second; p.rec = &r; S.pullObjs.push_back(p); }
			Printf("[roth_worldhands] pull %s: %d object(s) with id %u\n", r.what, (int)objs.size(), key);
		}
		else if (op == 0x18) { facePulls++; Printf("[roth_worldhands] pull %s: the faces whose use is #%u (wall id %u)\n", r.what, r.record, key); }
		else Printf("[roth_worldhands] pull %s: record #%u is opcode 0x%02x, neither a click (0x08) nor a face use (0x18): not built\n", r.what, r.record, op);
	}
	for (const PressRecord &r : kPresses)
	{
		if (!MapIs(r.map)) continue;
		uint8_t op = 0;
		uint16_t key = 0;
		if (!RecordKey(r.record, op, key) || op != 0x08) { Printf("[roth_worldhands] press %s: record #%u is not a click (0x08) here\n", r.what, r.record); continue; }
		std::vector<std::pair<int, int>> objs;
		objectsWithId(key, objs);
		for (const auto &o : objs)
		{
			PrintObj p;
			p.sector = o.first; p.index = o.second; p.rec = &r;
			AActor *mo = FindObjectActor(o.first, o.second);
			if (mo == nullptr) continue;
			DVector3 lo, hi;
			ObjectPickBox(mo, lo, hi);
			const DVector3 c = (lo + hi) * 0.5;
			// the print's plane: through its actor, facing the open side (its fixed-angle quad's own yaw)
			if (ObjectFacing(mo, c, p.n))
			{
				p.plane = true;
				p.wallAt = mo->Pos().XY();
				p.along = DVector2(-p.n.Y, p.n.X);
				p.offsetM = ((c.XY() - p.wallAt) | p.n) / upm;
				double a0 = 1e9, a1 = -1e9;
				const double xs[2] = { lo.X, hi.X }, ys[2] = { lo.Y, hi.Y };
				for (double x : xs) for (double y : ys) { const double a = ((DVector2(x, y) - p.wallAt) | p.along) / upm; a0 = std::min(a0, a); a1 = std::max(a1, a); }
				p.a0 = a0; p.a1 = a1; p.z0 = lo.Z; p.z1 = hi.Z;
			}
			Printf("[roth_worldhands] press %s: object %d/%d at (%.0f %.0f %.0f), %s\n", r.what, o.first, o.second, c.X, c.Y, c.Z,
				p.plane ? FStringf("facing (%.2f %.2f), %.2f m wide, %.2f m tall", p.n.X, p.n.Y, p.a1 - p.a0, (p.z1 - p.z0) * PS() / upm).GetChars()
				        : "neither side of it is open: not pressable");
			if (p.plane) S.prints.push_back(p);
		}
	}
	for (const SeatRecord &r : kSeats)
	{
		if (!MapIs(r.map)) continue;
		Seat st;
		st.rec = &r;
		st.lo = DVector2(1e9, 1e9); st.hi = DVector2(-1e9, -1e9);
		double bestDrop = 0;
		for (int sn = 0; sn < (int)S.level->sectors.Size(); sn++)
		{
			if (RuntimeSectorId(sn) != r.sectorId) continue;
			st.sectors.push_back(sn);
			sector_t *sec = &S.level->sectors[sn];
			for (line_t *ln : sec->Lines)
				for (const vertex_t *v : { ln->v1, ln->v2 })
				{
					st.lo.X = std::min(st.lo.X, v->fX()); st.lo.Y = std::min(st.lo.Y, v->fY());
					st.hi.X = std::max(st.hi.X, v->fX()); st.hi.Y = std::max(st.hi.Y, v->fY());
				}
		}
		if (st.sectors.empty()) { Printf("[roth_worldhands] %s: no sector carries id %u\n", r.what, r.sectorId); continue; }
		st.centre = (st.lo + st.hi) * 0.5;
		sector_t *seat = &S.level->sectors[st.sectors[0]];
		st.topZ = seat->floorplane.ZatPoint(st.centre);
		// THE FRONT: the neighbour whose floor is lowest below the seat's (the step in front of a throne), through the line
		// they share.
		for (int sn : st.sectors)
			for (line_t *ln : S.level->sectors[sn].Lines)
			{
				sector_t *other = ln->frontsector == &S.level->sectors[sn] ? ln->backsector : ln->frontsector;
				if (other == nullptr) continue;
				bool inSeat = false;
				for (int k : st.sectors) if (other->Index() == k) inSeat = true;
				if (inSeat) continue;
				const DVector2 mid = (ln->v1->fPos() + ln->v2->fPos()) * 0.5;
				const double drop = st.topZ - other->floorplane.ZatPoint(mid);
				if (drop > bestDrop) { bestDrop = drop; st.front = (mid - st.centre); }
			}
		Printf("[roth_worldhands] seat %s: %d sector(s), (%.0f %.0f)..(%.0f %.0f), floor %.0f, %s\n", r.what, (int)st.sectors.size(), st.lo.X, st.lo.Y,
			st.hi.X, st.hi.Y, st.topZ, bestDrop > 0 ? FStringf("its front toward (%.2f %.2f), the step %.0f units below", st.front.Unit().X, st.front.Unit().Y, bestDrop).GetChars()
			: "NO lower neighbour: no front known, never sat on");
		if (bestDrop > 0) S.seats.push_back(st);
	}
	if (Logging())
		Printf("[roth_worldhands] %s: %d lever object(s), %d face pull record(s), %d hand print(s), %d seat(s)\n", S.level->MapName.GetChars(),
			(int)S.pullObjs.size(), facePulls, (int)S.prints.size(), (int)S.seats.size());
}

//===========================================================================
// The release (Q1, Q14)
//===========================================================================

const ReleaseItem *ReleaseOf(int item) { for (const ReleaseItem &r : kReleases) if (r.item == item) return &r; return nullptr; }
const TwistItem *TwistOf(int item) { for (const TwistItem &t : kTwists) if (t.item == item) return &t; return nullptr; }
bool IsVial(int item) { for (int v : kVials) if (v == item) return true; return false; }

DVector3 ContactPoint(const HandNow &h, const ReleaseItem &r)
{
	DVector3 p = h.palm;
	if (r.base) p.Z -= HeldItemCardHeightM(r.item) * 0.5 * UPM() / PS();
	return p;
}

void Release(int role, const HandNow &h, AActor *pmo, int item)
{
	const ReleaseItem *r = ReleaseOf(item);
	if (r == nullptr)
	{
		if (Logging() && item > 0) Printf("[roth_worldhands] %s hand opens holding item %d: not set down anywhere (only the Statuette and the Orb are)\n", RoleName(role), item);
		return;
	}
	const double upm = UPM(), ps = PS();
	const DVector3 pt = ContactPoint(h, *r);
	Pick fp, wp, q;
	double df = 1e9, dw = 1e9;
	if (FlatAt(pt, wh::kSocketM * upm / ps, 0.03 * upm / ps, fp)) df = fabs(pt.Z - fp.hitPos.Z) * ps / upm;
	if (PickTouchSurface(pmo, pt, wh::kSocketM * upm, wp) && (wp.kind == Pick::WallFace || wp.kind == Pick::DoorLeaf)) dw = wp.dist / upm;
	if (df > wh::kSocketM && dw > wh::kSocketM)
	{
		Printf("[roth_worldhands] %s hand releases %s at (%.1f %.1f %.1f): nothing within %.2f m of it -- nothing (it stays in the hand)\n", RoleName(role),
			r->what, pt.X, pt.Y, pt.Z, wh::kSocketM);
		return;
	}
	q = df <= dw ? fp : wp;
	const double dist = std::min(df, dw);
	uint16_t rec = UseRecordOf(q);
	bool moved = false;
	if (rec == 0)
	{
		Pick d;
		if (Disc(q, wh::kBaseDiscM, true, d)) { moved = true; q = d; rec = UseRecordOf(d); }
	}
	if (rec == 0)
	{
		Printf("[roth_worldhands] %s hand releases %s on %s at (%.1f %.1f %.1f), %.3f m from it: the original's use matches no record there, nor within %.2f m"
			" -- nothing (it stays in the hand)\n", RoleName(role), r->what, PickKindName(q), q.hitPos.X, q.hitPos.Y, q.hitPos.Z, dist, wh::kBaseDiscM);
		ExplainSurfaceBox(q);
		return;
	}
	const bool acted = VRPickUseAt(role, q, pt, "release (set down)");
	Printf("[roth_worldhands] %s hand releases %s on %s at (%.1f %.1f %.1f)%s: the original's use, record #%u -> %s; the right hand now holds %d\n",
		RoleName(role), r->what, PickKindName(q), q.hitPos.X, q.hitPos.Y, q.hitPos.Z, moved ? " (its disc's nearest point in a box)" : "", rec,
		acted ? "acted" : "nothing happened", RightItem());
	Buzz(role, acted ? 0.35 : 0.12, acted ? 30 : 12);
}

//===========================================================================
// The twist (Q15, Q16)
//===========================================================================

// What the item at the palm is set in: the nearest thing within kSocketM whose use the original's dispatcher matches.
bool SocketAt(AActor *pmo, const DVector3 &palm, Pick &out, double &distM)
{
	const double upm = UPM();
	distM = 1e9;
	bool have = false;
	ForObjects([&](AActor *mo, int, int) {
		if (mo == pmo) return;
		DVector3 lo, hi;
		ObjectPickBox(mo, lo, hi);
		const double d = BoxDistM(palm, lo, hi);
		if (d > wh::kSocketM || d >= distM) return;
		Pick p;
		p.kind = Pick::Prop;
		p.actor = mo;
		p.hitPos = BoxClosest(palm, lo, hi);
		p.dist = d * upm;
		if (UseRecordOf(p) == 0) return;
		out = p; distM = d; have = true;
	});
	Pick w;
	if (PickTouchSurface(pmo, palm, wh::kSocketM * upm, w) && (w.kind == Pick::WallFace || w.kind == Pick::DoorLeaf) && w.dist / upm < distM)
	{
		Pick d = w;
		if (UseRecordOf(w) != 0 || Disc(w, wh::kBaseDiscM, true, d)) { out = d; distM = w.dist / upm; have = true; }
	}
	return have;
}

bool TryTwist(int role, const HandNow &h, AActor *pmo, const char *how)
{
	HandW &w = S.hand[role];
	if (role != ItemRole() || w.twist.engaged || w.pull.engaged || !h.frame) return false;
	const TwistItem *t = TwistOf(RightItem());
	if (t == nullptr) return false;
	Pick p;
	double d = 0;
	if (!SocketAt(pmo, h.palm, p, d)) return false;
	w.twist.Engage(h.up);
	w.socket = p;
	w.socketAt = p.hitPos;
	w.twistItem = t;
	w.twistLogged = 0;
	w.claimed = true;
	Printf("[roth_worldhands] %s hand: %s set in %s at (%.1f %.1f %.1f), %.3f m from the hand (%s): turn it %.0f degrees\n", RoleName(role), t->what,
		PickKindName(p), p.hitPos.X, p.hitPos.Y, p.hitPos.Z, d, how, t->degrees);
	Buzz(role, 0.15, 12);
	return true;
}

//===========================================================================
// The pull (Q17, Q30)
//===========================================================================

bool TryPull(int role, const HandNow &h, AActor *pmo)
{
	HandW &w = S.hand[role];
	if (w.pull.engaged || w.twist.engaged) return false;
	const double upm = UPM();
	// a lever object within reach of the palm
	// looked up at each grip, shown objects only: a lever a chain shows later has no actor until then (MAS6 #20's,
	// the PC run of 976cb81e: "0 object(s) with id 3" at the level's start)
	std::vector<PullObj> levers;
	for (const PullRecord &r : kPulls)
	{
		uint8_t op = 0;
		uint16_t key = 0;
		if (!MapIs(r.map) || !RecordKey(r.record, op, key) || op != 0x08) continue;
		ForObjects([&](AActor *, int s, int i) {
			const Object *o = RuntimeObject(s, i);
			if (o != nullptr && o->commandID == key) { PullObj po; po.sector = s; po.index = i; po.rec = &r; levers.push_back(po); }
		});
	}
	const PullObj *best = nullptr;
	double bestD = kPullGripM;
	DVector3 bestLo, bestHi;
	for (const PullObj &o : levers)
	{
		AActor *mo = FindObjectActor(o.sector, o.index);
		if (mo == nullptr || (mo->renderflags & RF_INVISIBLE)) continue;
		DVector3 lo, hi;
		ObjectPickBox(mo, lo, hi);
		const double d = BoxDistM(h.palm, lo, hi);
		if (d <= bestD) { bestD = d; best = &o; bestLo = lo; bestHi = hi; }
	}
	if (best != nullptr)
	{
		Pick p;
		p.kind = Pick::Prop;
		p.actor = FindObjectActor(best->sector, best->index);
		p.hitPos = BoxClosest(h.palm, bestLo, bestHi);
		p.dist = bestD * upm;
		if (VRPickCursorType(role, p, h.palm) != 3)
		{
			if (Logging()) Printf("[roth_worldhands] %s hand grips %s: the original's cursor offers no use there now -- the picker's\n", RoleName(role), best->rec->what);
			return false;
		}
		const DVector3 c = (bestLo + bestHi) * 0.5;
		DVector3 in(c.X - h.palm.X, c.Y - h.palm.Y, 0.0);
		w.pull.Engage(M(h.palm), V3(in.X, in.Y, 0), best->rec->metres);
		w.pullPick = p;
		w.pullFrom = h.palm;
		w.pullRec = best->rec;
		w.pullLoggedCm = -1;
		w.claimed = true;
		Printf("[roth_worldhands] %s hand grips %s (object %d/%d, %.3f m from its box): pull it %.2f m\n", RoleName(role), best->rec->what, best->sector,
			best->index, bestD, best->rec->metres);
		Buzz(role, 0.15, 12);
		return true;
	}
	// a face whose use is one of the table's records: the palm on it, else along the hand
	Pick p;
	bool have = PickTouchSurface(pmo, h.palm, 0.10 * upm, p) && (p.kind == Pick::WallFace || p.kind == Pick::DoorLeaf);
	if (!have)
	{
		const double back = 0.08 * upm;
		have = PickFrom(pmo, h.palm - h.handFwd * back, h.handFwd, back + 0.20 * upm, p) && (p.kind == Pick::WallFace || p.kind == Pick::DoorLeaf);
	}
	if (!have) return false;
	uint16_t rec = UseRecordOf(p);
	if (rec == 0) { Pick d; if (Disc(p, wh::kPalmDiscM, true, d)) { p = d; rec = UseRecordOf(d); } }
	const PullRecord *pr = nullptr;
	for (const PullRecord &r : kPulls) if (MapIs(r.map) && r.record == rec) pr = &r;
	if (pr == nullptr) return false;
	if (pr->edge && p.line != nullptr)
	{
		const DVector2 d = p.line->delta;
		const double len = d.Length();
		const double along = len > 1e-6 ? ((p.hitPos.XY() - p.line->v1->fPos()) | (d / len)) : 0.0;
		const double edge = wh::kLeafEdgeM * upm;
		if (along > edge && along < len - edge)
		{
			// The owner's Q30: the leaf opens by its edge. A grip on its middle is taken and does nothing (it is not
			// the picker's use either: that would open the bookcase from anywhere on it).
			Printf("[roth_worldhands] %s hand grips %s %.2f m from its nearer edge (its edge is the outer %.2f m): nothing\n", RoleName(role),
				pr->what, std::min(along, len - along) / upm, wh::kLeafEdgeM);
			w.claimed = true;
			return true;
		}
	}
	if (VRPickCursorType(role, p, h.palm) != 3)
	{
		if (Logging()) Printf("[roth_worldhands] %s hand grips %s: the original's cursor offers no use there now -- the picker's\n", RoleName(role), pr->what);
		return false;
	}
	DVector2 n;
	if (!FaceNormal(p, pmo, n)) n = DVector2(-h.handFwd.X, -h.handFwd.Y);
	w.pull.Engage(M(h.palm), V3(-n.X, -n.Y, 0), pr->metres);
	w.pullPick = p;
	w.pullFrom = h.palm;
	w.pullRec = pr;
	w.pullLoggedCm = -1;
	w.claimed = true;
	Printf("[roth_worldhands] %s hand grips %s (%s, record #%u at (%.1f %.1f %.1f)): pull it %.2f m\n", RoleName(role), pr->what, PickKindName(p), rec,
		p.hitPos.X, p.hitPos.Y, p.hitPos.Z, pr->metres);
	Buzz(role, 0.15, 12);
	return true;
}

//===========================================================================
// One hand, one tic
//===========================================================================

void Forget(HandW &w)
{
	w.release.Reset();
	w.pull.Reset();
	w.twist.Reset();
	w.drink.Reset();
	w.claimed = false;
	for (PrintObj &p : S.prints) p.press[&w - S.hand].Reset();
}

void TickHand(int role, AActor *pmo)
{
	HandW &w = S.hand[role];
	HandNow h;
	if (!ReadHand(role, h)) { Forget(w); return; }
	const int item = RightItem();
	const bool itemHand = role == ItemRole();

	// ---- THE RELEASE ----
	if (w.release.Feed(true, h.grip))
	{
		if (w.claimed) { if (Logging()) Printf("[roth_worldhands] %s hand opens: the grip was a pull or a twist -- no release\n", RoleName(role)); }
		else if (itemHand) Release(role, h, pmo, item);
	}
	if (!h.grip) w.claimed = false;

	// ---- THE PULL ----
	if (w.pull.engaged)
	{
		const V3 at = M(h.palm);
		const double travel = w.pull.Travel(at);
		const PullRecord *pr = w.pullRec;
		if (w.pull.Feed(h.grip, at))
		{
			if (w.pullPick.kind == Pick::Prop)
			{
				int s = -1, i = -1;
				if (ObjectForActor(w.pullPick.actor, s, i)) w.pullPick.actor = FindObjectActor(s, i);
			}
			const bool acted = w.pullPick.actor != nullptr || w.pullPick.kind != Pick::Prop ? VRPickUseAt(role, w.pullPick, w.pullFrom, "pull") : false;
			Printf("[roth_worldhands] %s hand pulled %s %.3f m (%.2f asked): the original's use -> %s\n", RoleName(role), pr->what, travel, pr->metres,
				acted ? "acted" : "nothing happened");
			Buzz(role, acted ? 0.5 : 0.15, acted ? 40 : 12);
		}
		else if (!w.pull.engaged)
			Printf("[roth_worldhands] %s hand let go of %s after %.3f m (%.2f asked): nothing\n", RoleName(role), pr->what, travel, pr->metres);
		else if (Logging())
		{
			const int cm = (int)(travel * 100.0);
			if (cm != w.pullLoggedCm) { Printf("[roth_worldhands] %s hand pulling %s: %.3f m\n", RoleName(role), pr->what, travel); w.pullLoggedCm = cm; }
		}
	}

	// ---- THE TWIST ----
	if (!w.twist.engaged && h.grip && itemHand) TryTwist(role, h, pmo, "moved in with the grip closed");
	if (w.twist.engaged && w.twistItem != nullptr)
	{
		const double off = M(h.palm - w.socketAt).Length();
		const bool still = h.grip && h.frame && item == w.twistItem->item && off <= wh::kSocketHoldM;
		const TwistItem *t = w.twistItem;
		if (w.twist.Feed(still, h.up, h.fwd, t->degrees))
		{
			if (w.socket.kind == Pick::Prop)
			{
				int s = -1, i = -1;
				if (ObjectForActor(w.socket.actor, s, i)) w.socket.actor = FindObjectActor(s, i);
			}
			const bool acted = VRPickUseAt(role, w.socket, h.palm, "twist");
			Printf("[roth_worldhands] %s hand turned %s %.0f degrees in %s: the original's use -> %s; the right hand now holds %d\n", RoleName(role), t->what,
				w.twist.turned, PickKindName(w.socket), acted ? "acted" : "nothing happened", RightItem());
			Buzz(role, acted ? 0.5 : 0.15, acted ? 40 : 12);
		}
		else if (!w.twist.engaged)
			Printf("[roth_worldhands] %s hand: %s out of its socket (%s, %.3f m from it) -- nothing\n", RoleName(role), t->what, !h.grip ? "the grip opened"
				: item != t->item ? "the item changed" : "moved away", off);
		else if (Logging() && (int)(fabs(w.twist.turned) / 15.0) != w.twistLogged)
		{
			w.twistLogged = (int)(fabs(w.twist.turned) / 15.0);
			Printf("[roth_worldhands] %s hand turning %s: %.0f degrees\n", RoleName(role), t->what, w.twist.turned);
		}
	}

	// ---- THE PRESS ----
	const double upm = UPM();
	for (PrintObj &p : S.prints)
	{
		AActor *mo = FindObjectActor(p.sector, p.index);
		const bool live = mo != nullptr && !(mo->renderflags & RF_INVISIBLE) && !h.grip;
		const double depth = p.offsetM - ((h.palm.XY() - p.wallAt) | p.n) / upm;
		const double along = ((h.palm.XY() - p.wallAt) | p.along) / upm;
		const double disc = wh::kPalmDiscM;
		const bool over = along >= p.a0 - disc && along <= p.a1 + disc && h.palm.Z >= p.z0 - disc * upm / PS() && h.palm.Z <= p.z1 + disc * upm / PS();
		const bool wasArmed = p.press[role].armed;
		if (p.press[role].Feed(live, over, depth))
		{
			Pick pk;
			pk.kind = Pick::Prop;
			pk.actor = mo;
			DVector3 lo, hi;
			ObjectPickBox(mo, lo, hi);
			pk.hitPos = BoxClosest(h.palm, lo, hi);
			const bool acted = VRPickUseAt(role, pk, h.palm, "palm pressed into the print");
			Printf("[roth_worldhands] %s hand: the palm pressed %.3f m into %s: the original's click -> %s\n", RoleName(role), depth, p.rec->what,
				acted ? "acted" : "nothing happened");
			Buzz(role, acted ? 0.4 : 0.15, acted ? 30 : 12);
		}
		else if (Logging() && wasArmed != p.press[role].armed && p.press[role].armed)
			Printf("[roth_worldhands] %s hand: the open palm in front of %s (%.3f m), %s\n", RoleName(role), p.rec->what, -depth, over ? "over it" : "beside it");
	}

	// ---- THE DRINK ----
	if (itemHand)
	{
		const DVector3 eye = EyeNow(pmo);
		const DVector3 mouth = eye - DVector3(0, 0, wh::kMouthBelowEyeM * upm / PS());
		const double dist = M(h.palm - mouth).Length();
		const double tilt = h.frame ? wh::TiltDeg(h.up) : 0.0;
		const bool vial = IsVial(item);
		if (w.drink.Feed(vial && h.frame, dist, tilt))
		{
			const bool started = InterfaceUseRightHandOnSelf(item);
			Printf("[roth_worldhands] %s hand: the Vial (item %d) tipped %.0f degrees %.3f m from the mouth: the original's use on self %s\n", RoleName(role),
				item, tilt, dist, started ? "(by the inventory's clicks)" : "NOT started: a panel or a line owns the pointer");
			if (started) Buzz(role, 0.3, 25);
		}
		else if (Logging() && vial)
		{
			const int band = dist > wh::kMouthNearM ? -1 : (int)(tilt / 15.0);
			if (band != w.loggedDrink)
			{
				Printf("[roth_worldhands] %s hand: the Vial %.3f m from the mouth, tipped %.0f degrees\n", RoleName(role), dist, tilt);
				w.loggedDrink = band;
			}
		}
	}
}

//===========================================================================
// The sit (Q19)
//===========================================================================

double HeadDropM(AActor *pmo)
{
	if (VRPoseTest_Active())
	{
		float pos[3]; float yaw = 0.f;
		if (!VRPoseTest_Head(pos, yaw) || pmo->player == nullptr) return 0.0;
		const double stand = pmo->player->DefaultViewHeight() * PS() / UPM();   // where roth_xrpose on puts the head
		return stand - pos[1];
	}
	const float m = VR_MeasuredHeadHeight(), c = VR_CurrentHeadHeight();
	return m > 0.f && c > 0.f ? (double)(m - c) : 0.0;
}

void TickSit(AActor *pmo)
{
	if (S.seats.empty()) return;
	const double drop = HeadDropM(pmo);
	const DVector3 eye = EyeNow(pmo);
	const DAngle yaw = r_viewpoint.ViewLevel == S.level ? r_viewpoint.Angles.Yaw : pmo->Angles.Yaw;
	for (Seat &st : S.seats)
	{
		const double dx = std::max(0.0, std::max(st.lo.X - eye.X, eye.X - st.hi.X)), dy = std::max(0.0, std::max(st.lo.Y - eye.Y, eye.Y - st.hi.Y));
		const double nearM = sqrt(dx * dx + dy * dy) / UPM();
		const bool over = nearM <= wh::kSitNearM;
		const bool away = wh::FacingAway(yaw.Cos(), yaw.Sin(), st.front.X, st.front.Y);
		if (Logging())
		{
			const int band = (int)(drop / 0.05) * 4 + (over ? 2 : 0) + (away ? 1 : 0);
			if (band != S.loggedSitBand)
			{
				Printf("[roth_worldhands] the head %.2f m below standing, %.2f m from %s in the plane, %s\n", drop, nearM, st.rec->what,
					away ? "facing away from its back" : "not facing away from its back");
				S.loggedSitBand = band;
			}
		}
		if (!S.sit.Feed(drop, over, away)) continue;
		// the seat's surface under the head (or under the seat's middle): its floor or platform top, highest first
		Pick p;
		const DVector2 at = (eye.X >= st.lo.X && eye.X <= st.hi.X && eye.Y >= st.lo.Y && eye.Y <= st.hi.Y) ? eye.XY() : st.centre;
		sector_t *sec = S.level->PointInSector(at);
		if (sec == nullptr || !FlatAt(DVector3(at.X, at.Y, sec->ceilingplane.ZatPoint(at) - 1.0), 1e6, 0.0, p))
		{
			Printf("[roth_worldhands] sat on %s: no floor under (%.0f %.0f) -- nothing\n", st.rec->what, at.X, at.Y);
			continue;
		}
		const bool acted = VRPickUseAt(ItemRole(), p, eye, "sat down");
		Printf("[roth_worldhands] sat on %s (the head %.2f m below standing, %.2f m from it): the original's use on %s at (%.1f %.1f %.1f) -> %s\n",
			st.rec->what, drop, nearM, PickKindName(p), p.hitPos.X, p.hitPos.Y, p.hitPos.Z, acted ? "acted" : "nothing happened");
		break;
	}
}

void TestTick();

} // namespace (anonymous)

//===========================================================================
// The entry points
//===========================================================================

void WorldHandsTick(FLevelLocals *level, bool live)
{
	if (level == nullptr) return;
	if (S.level != level) { WorldHandsEndLevel(); S.level = level; }
	TestTick();
	player_t *player = level->GetConsolePlayer();
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	if (!live || pmo == nullptr || pmo->player == nullptr)
	{
		for (HandW &w : S.hand) Forget(w);
		return;
	}
	if (!S.built) Build(pmo);
	for (int role = 0; role < 2; role++) TickHand(role, pmo);
	TickSit(pmo);
}

void WorldHandsEndLevel()
{
	S.level = nullptr;
	S.built = false;
	S.pullObjs.clear();
	S.prints.clear();
	S.seats.clear();
	S.sit.Reset();
	S.loggedSitBand = -1;
	for (HandW &w : S.hand) { Forget(w); w = HandW(); }
}

bool WorldHandsGripTaken(int role)
{
	if (role < 0 || role > 1 || S.level == nullptr) return false;
	player_t *player = S.level->GetConsolePlayer();
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	if (pmo == nullptr) return false;
	if (!S.built) Build(pmo);
	HandNow h;
	if (!ReadHand(role, h)) return false;
	if (GrabGlueTakesGrip(S.level, role)) return false;   // the package's (roth_vrpick.cpp asks it first; asked again for safety)
	if (TryTwist(role, h, pmo, "the grip closed in it")) return true;
	return TryPull(role, h, pmo);
}

bool WorldHandsHolding(int role)
{
	if (role < 0 || role > 1 || S.level == nullptr) return false;
	const HandW &w = S.hand[role];
	return w.claimed || w.pull.engaged || w.twist.engaged;
}

bool WorldHandsItemTipping(int role)
{
	if (S.level == nullptr || role != ItemRole() || !IsVial(RightItem())) return false;
	player_t *player = S.level->GetConsolePlayer();
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	HandNow h;
	if (pmo == nullptr || !ReadHand(role, h) || !h.frame) return false;
	const DVector3 mouth = EyeNow(pmo) - DVector3(0, 0, wh::kMouthBelowEyeM * UPM() / PS());
	return wh::Drink::Tipping(true, M(h.palm - mouth).Length(), wh::TiltDeg(h.up));
}

bool WorldHandsContactDisc(const Pick &p, double radiusM, bool use, Pick &out)
{
	return Disc(p, radiusM, use, out);
}

bool WorldHandsFlatUnder(const DVector3 &pt, double above, double below, Pick &out)
{
	return FlatAt(pt, above, below, out);
}

} // namespace roth

//===========================================================================
//
// roth_worldtest -- THE FLAT TEST OF CHAPTER 2'S WORLD BY HAND. Test-only, inert until typed. It drives roth_xrpose's
// controller poses (the real controller path: the drawn hands, the picker, the held objects run as in the headset),
// so `roth_xrpose on` and a `roth_xrpose hand` / `point` for each hand come first.
//
//   roth_worldtest spot <record>          find where trigger record <record> of this map takes a click: an object's
//                                         box middle (0x08 / 0x30), the middle of the points of its sectors' floors
//                                         (0x19 / 0x31) or of its faces (0x18 / 0x32) where the original's dispatcher
//                                         matches it (its surface box). It becomes "the spot".
//                                         A face's spot (and an object's on a wall) has a normal toward the room.
//   roth_worldtest spot mouth [ahead]     the spot: the mouth (8 cm under the eyes), `ahead` metres in front of it
//   roth_worldtest stand <metres> [bearing|front]
//                                         stand the player that far from the spot (in the plane), at that bearing
//                                         (degrees, from the spot; none: out along its normal; front: out along the
//                                         front of this map's seat), facing it
//   roth_worldtest turn <degrees>         turn the player
//   roth_worldtest hold <main|off> <palm|base|tip> <x> <y> <z>
//   roth_worldtest hold <main|off> <palm|base|tip> n <out> <up> [along]
//                                         keep that point of the hand (base: the bottom of the item card the item hand
//                                         shows) at the spot plus (x, y, z) METRES (world axes, z up) -- or <out> along
//                                         the spot's normal, <up>, <along> its surface -- correcting the controller pose
//                                         every tic; `hold <main|off> off` stops
//   roth_worldtest move <main|off> <x> <y> <z> <tics> | move <main|off> n <out> <tics>
//                                         move the held point by (x, y, z) metres (or out along the normal) over <tics>
//   roth_worldtest grip <main|off> <0..1> the grip value (the trigger 0)
//   roth_worldtest roll <main|off> <degrees> <tics>
//                                         turn the hand about its fingers' axis that far over <tics>
//   roth_worldtest head <metres>          the head that far below standing (0: standing)
//   roth_worldtest report                 the right-hand item, each hand's point and what it is engaged in
//
//===========================================================================

namespace roth
{
namespace
{

struct TestHold
{
	bool on = false;
	int what = 0;          // 0 palm, 1 base, 2 tip
	DVector3 offset;       // map units from the spot
	DVector3 moveFrom, moveTo;
	int moveStart = 0, moveTics = 0;
	double rollPerTic = 0;
	int rollLeft = 0;
	int logged = 0;
};
struct TestState
{
	bool haveSpot = false, haveNormal = false;
	DVector3 spot;
	DVector2 normal, along;   // the spot's surface: its normal toward the room, and along it (faces; objects on a wall)
	TestHold hold[2];
} T;

void QMul(const float a[4], const float b[4], float o[4])
{
	o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

bool TestPoint(int role, int what, DVector3 &out)
{
	HandNow h;
	if (!ReadHand(role, h)) return false;
	out = what == 2 ? h.tip : h.palm;
	if (what == 1)
	{
		const ReleaseItem *r = ReleaseOf(RightItem());
		out.Z -= HeldItemCardHeightM(r != nullptr ? r->item : RightItem()) * 0.5 * UPM() / PS();
	}
	return true;
}

// Move the controller pose so the world point moves by `err` (map units): the inverse of vr_posemath.h's
// HeadRelativeOffset + OffsetToWorld for a translation (world dX = -upm * gRot.Y, dY = -upm * gRot.X, gRot = the
// head-relative (x, z) turned by -(bodyYaw - hmdYaw); dZ = upm * dy / stretch).
void Nudge(int role, const DVector3 &err, double gain)
{
	FVRPoseTestHand ph;
	if (!VRPoseTest_Hand(role, ph)) return;
	float head[3]; float hmdYaw = 0.f;
	VRPoseTest_Head(head, hmdYaw);
	const double upm = UPM(), ps = PS();
	const double gYaw = r_viewpoint.Angles.Yaw.Degrees() - hmdYaw;
	const DVector2 gRot(-err.Y / upm, -err.X / upm);
	const DVector2 go = gRot.Rotated(gYaw);
	ph.pos[0] += (float)(go.X * gain);
	ph.pos[2] += (float)(go.Y * gain);
	ph.pos[1] += (float)(err.Z * ps / upm * gain);
	VRPoseTest_SetHand(role, ph);
}

void TestTick()
{
	if (!VRPoseTest_Active() || S.level == nullptr) return;
	for (int role = 0; role < 2; role++)
	{
		TestHold &t = T.hold[role];
		if (t.rollLeft > 0)
		{
			FVRPoseTestHand ph;
			if (VRPoseTest_Hand(role, ph))
			{
				float q[4], o[4];
				VRPose_QuatFromYPR(t.rollPerTic, 0, 0, q);   // about the grip's +Y: the fingers' axis (roth_xrpose point)
				QMul(ph.quat, q, o);
				for (int k = 0; k < 4; k++) ph.quat[k] = o[k];
				VRPoseTest_SetHand(role, ph);
			}
			if (--t.rollLeft == 0) Printf("[roth_worldtest] the %s hand's roll ends (tic %d)\n", RoleName(role), S.level->maptime);
		}
		if (!t.on || !T.haveSpot) continue;
		DVector3 off = t.offset;
		if (t.moveTics > 0)
		{
			const double f = std::min(1.0, (double)(S.level->maptime - t.moveStart) / t.moveTics);
			off = t.moveFrom + (t.moveTo - t.moveFrom) * f;
			if (f >= 1.0) { t.offset = t.moveTo; t.moveTics = 0; Printf("[roth_worldtest] the %s hand's move ends (tic %d)\n", RoleName(role), S.level->maptime); }
		}
		DVector3 at;
		if (!TestPoint(role, t.what, at)) continue;
		const DVector3 want = T.spot + off;
		const DVector3 err = want - at;
		// carried to its place (not a scripted move): the gestures take none of it as a motion (the first PC run: the
		// correction swung the fist at 9.6 m/s into the Orb door -- a punch, the original's use)
		if (t.moveTics == 0 && err.Length() > 0.5) GesturesTestPlacing(role);
		if (err.Length() > 0.05) Nudge(role, err, 0.6);
		else if (t.logged != 1)
		{
			Printf("[roth_worldtest] the %s hand's %s held at (%.1f %.1f %.1f), %.2f units from the target\n", RoleName(role),
				t.what == 0 ? "palm" : t.what == 1 ? "item base" : "fingertip", at.X, at.Y, at.Z, err.Length());
			t.logged = 1;
		}
		if (err.Length() > 1.0) t.logged = 0;
	}
}

bool RecordSpot(uint16_t record, DVector3 &at, FString &how, bool &haveNormal, DVector2 &normal)
{
	haveNormal = false;
	uint8_t op = 0;
	uint16_t key = 0;
	if (S.level == nullptr || !RecordKey(record, op, key)) { how = "not a record of this map"; return false; }
	AActor *pmo = players[consoleplayer].mo;
	if (pmo == nullptr) return false;
	if (op == 0x08 || op == 0x30)
	{
		bool found = false;
		auto it = S.level->GetThinkerIterator<AActor>();
		AActor *mo;
		while ((mo = it.Next()) != nullptr && !found)
		{
			int s = -1, i = -1;
			if (!ObjectForActor(mo, s, i) || FindObjectActor(s, i) != mo) continue;
			const Object *o = RuntimeObject(s, i);
			if (o == nullptr || o->commandID != key) continue;
			DVector3 lo, hi;
			ObjectPickBox(mo, lo, hi);
			at = (lo + hi) * 0.5;
			how.Format("object %d/%d (id %u)%s, its box (%.0f %.0f %.0f)..(%.0f %.0f %.0f)", s, i, key, (mo->renderflags & RF_INVISIBLE) ? ", hidden now" : "",
				lo.X, lo.Y, lo.Z, hi.X, hi.Y, hi.Z);
			if (ObjectFacing(mo, at, normal))
			{
				haveNormal = true;
				how.AppendFormat("; facing (%.2f %.2f)", normal.X, normal.Y);
			}
			found = true;
		}
		if (!found) how.Format("no object carries id %u", key);
		return found;
	}
	if (op == 0x13)
	{
		// an enter-sector record: the middle of the first sector carrying its key (no surface to match)
		for (int sn = 0; sn < (int)S.level->sectors.Size(); sn++)
		{
			if (RuntimeSectorId(sn) != key) continue;
			at = DVector3(S.level->sectors[sn].centerspot.X, S.level->sectors[sn].centerspot.Y, S.level->sectors[sn].floorplane.ZatPoint(S.level->sectors[sn].centerspot));
			how.Format("sector %d (id %u), its middle", sn, key);
			return true;
		}
		how.Format("no sector carries id %u", key);
		return false;
	}
	const bool isUse = op == 0x18 || op == 0x19;
	DVector3 sum(0, 0, 0), lo(1e9, 1e9, 1e9), hi(-1e9, -1e9, -1e9);
	int n = 0, samples = 0, hits = 0;
	int kinds[5] = { 0, 0, 0, 0, 0 };
	std::map<int, int> met;   // the records the dispatcher matched at the samples (the diagnostic)
	std::vector<DVector3> pts;   // every matching point: the spot is the one nearest their middle (a record over several
	                             // sectors -- the tomb lids -- has its middle over none of them)
	auto count = [&](const Pick &p) {
		const uint16_t r = isUse ? UseRecordOf(p) : ExamineRecordOf(p);
		met[r]++;
		if (r != record) return;
		pts.push_back(p.hitPos);
		sum += p.hitPos; n++;
		lo = DVector3(std::min(lo.X, p.hitPos.X), std::min(lo.Y, p.hitPos.Y), std::min(lo.Z, p.hitPos.Z));
		hi = DVector3(std::max(hi.X, p.hitPos.X), std::max(hi.Y, p.hitPos.Y), std::max(hi.Z, p.hitPos.Z));
	};
	if (op == 0x19 || op == 0x31)
	{
		for (int sn = 0; sn < (int)S.level->sectors.Size(); sn++)
		{
			if (RuntimeSectorId(sn) != key) continue;
			sector_t *sec = &S.level->sectors[sn];
			double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
			for (line_t *ln : sec->Lines)
				for (const vertex_t *v : { ln->v1, ln->v2 }) { x0 = std::min(x0, v->fX()); y0 = std::min(y0, v->fY()); x1 = std::max(x1, v->fX()); y1 = std::max(y1, v->fY()); }
			const double step = std::max(2.0, std::max(x1 - x0, y1 - y0) / 160.0);
			for (double x = x0 + step * 0.5; x < x1; x += step)
				for (double y = y0 + step * 0.5; y < y1; y += step)
				{
					if (S.level->PointInSector(DVector2(x, y)) != sec) continue;
					// every surface of the point: the floor and each platform top (FlatAt: no vertical trace)
					samples++;
					const DVector2 xy(x, y);
					Pick p;
					if (FlatAt(DVector3(x, y, sec->floorplane.ZatPoint(xy)), 0.5, 0.5, p)) { hits++; kinds[Pick::Flat]++; count(p); }
					if (sec->e != nullptr)
						for (F3DFloor *ff : sec->e->XFloor.ffloors)
							if (ff != nullptr && (ff->flags & FF_EXISTS) && ff->top.plane != nullptr
								&& FlatAt(DVector3(x, y, ff->top.plane->ZatPoint(xy)), 0.5, 0.5, p) && p.ffloor == ff) { hits++; kinds[Pick::Flat]++; count(p); }
				}
		}
	}
	else if (op == 0x18 || op == 0x32)
	{
		// ONE FACE: the side with the most matching points (a door's two faces would put the spot inside the door:
		// the first PC run, MAS3 #4)
		std::vector<int> sides;
		RecordSides(record, sides);
		DVector3 bestSum(0, 0, 0), bestLo, bestHi;
		int bestN = 0;
		DVector2 bestNormal;
		for (int si : sides)
		{
			side_t *sd = &S.level->sides[si];
			line_t *ln = sd->linedef;
			if (ln == nullptr) continue;
			sector_t *sec = sd->sector;
			DVector2 nrm(ln->delta.Y, -ln->delta.X);
			nrm /= nrm.Length();
			if (sd != ln->sidedef[0]) nrm = -nrm;
			const DVector2 v1 = ln->v1->fPos();
			// both ways: a face may be seen from the sector across its line (MAUSO1EA #5's stone, the PC run of 976cb81e:
			// from its own side every sample stood inside the block and met nothing)
			for (int way = 0; way < 2; way++)
			{
				const DVector2 out = way == 0 ? nrm : -nrm;
				sector_t *zs = S.level->PointInSector(v1 + ln->delta * 0.5 + out * 8.0);
				if (zs == nullptr) continue;
				const double z0 = std::max(sec->floorplane.ZatPoint(v1), zs->floorplane.ZatPoint(v1));
				const double z1 = std::max(sec->ceilingplane.ZatPoint(v1), zs->ceilingplane.ZatPoint(v1));
				sum = DVector3(0, 0, 0); n = 0; lo = DVector3(1e9, 1e9, 1e9); hi = DVector3(-1e9, -1e9, -1e9);
				for (int k = 1; k < 64; k++)
					for (double z = std::min(z0, z1 - 2.0) + 1.0; z < z1; z += 2.0)
					{
						const DVector2 xy = v1 + ln->delta * (k / 64.0);
						const DVector3 from(xy.X + out.X * 8.0, xy.Y + out.Y * 8.0, z);
						Pick p;
						samples++;
						if (!PickFrom(pmo, from, DVector3(-out.X, -out.Y, 0), 16.0, p)) continue;
						hits++;
						kinds[std::min<int>(p.kind, 4)]++;
						if (p.kind == Pick::WallFace || p.kind == Pick::DoorLeaf) count(p);   // a leaf stands in front of its room face
					}
				if (n > bestN) { bestN = n; bestSum = sum; bestLo = lo; bestHi = hi; bestNormal = out; }
			}
		}
		sum = bestSum; n = bestN; lo = bestLo; hi = bestHi;
		if (n > 0) { normal = bestNormal; haveNormal = true; }
		if (sides.empty()) { how = "no face carries it"; return false; }
	}
	else { how.Format("opcode 0x%02x: no spot", op); return false; }
	FString diag;
	diag.Format(" [%d sample(s), %d hit(s): %d face, %d flat, %d door leaf, %d object; records met:", samples, hits, kinds[Pick::WallFace], kinds[Pick::Flat],
		kinds[Pick::DoorLeaf], kinds[Pick::Prop]);
	for (const auto &m : met) diag.AppendFormat(" #%d x%d", m.first, m.second);
	diag += "]";
	if (n == 0) { how = "no point of its surfaces matches it now (spent, or off?)" + diag; return false; }
	at = sum / n;
	{
		double bd = 1e30;
		DVector3 best = at;
		for (const DVector3 &q : pts) { const double d = (q - at).LengthSquared(); if (d < bd) { bd = d; best = q; } }
		at = best;
	}
	how.Format("%d matching point(s), (%.1f %.1f %.1f)..(%.1f %.1f %.1f)%s%s", n, lo.X, lo.Y, lo.Z, hi.X, hi.Y, hi.Z,
		haveNormal ? FStringf(", the face toward (%.2f %.2f)", normal.X, normal.Y).GetChars() : "", diag.GetChars());
	return true;
}

int RoleOf(const char *s) { return !stricmp(s, "off") ? VR_OFFHAND : !stricmp(s, "main") ? VR_MAINHAND : -1; }

} // namespace
} // namespace roth

CCMD(roth_worldtest)
{
	using namespace roth;
	if (argv.argc() < 2)
	{
		Printf("roth_worldtest spot <record> | stand <metres> <bearing> | hold <main|off> <palm|base|tip> x y z | hold <main|off> off\n"
			"               | move <main|off> x y z tics | grip <main|off> v | roll <main|off> deg tics | head <metres> | report\n");
		return;
	}
	AActor *pmo = players[consoleplayer].mo;
	if (S.level == nullptr || pmo == nullptr) { Printf("roth_worldtest: no level yet\n"); return; }
	const double upm = UPM(), ps = PS();
	const char *verb = argv[1];
	if (!stricmp(verb, "spot") && argv.argc() >= 3 && stricmp(argv[2], "mouth"))
	{
		DVector3 at;
		FString how;
		const int rec = atoi(argv[2]);
		bool hn = false;
		DVector2 nrm;
		if (RecordSpot((uint16_t)rec, at, how, hn, nrm))
		{
			T.spot = at; T.haveSpot = true; T.haveNormal = hn;
			T.normal = nrm; T.along = DVector2(-nrm.Y, nrm.X);
		}
		Printf("[roth_worldtest] spot of record #%d in %s: %s%s\n", rec, S.level->MapName.GetChars(), how.GetChars(),
			T.haveSpot ? FStringf(" -> the spot (%.1f %.1f %.1f)", T.spot.X, T.spot.Y, T.spot.Z).GetChars() : "");
		return;
	}
	if (!stricmp(verb, "stand") && argv.argc() >= 3 && T.haveSpot)
	{
		// the bearing given, else out along the spot's surface normal (a face, an object on a wall)
		// `front`: out along the front of this map's seat (the throne's step)
		const bool front = argv.argc() >= 4 && !stricmp(argv[3], "front");
		if (front && S.seats.empty()) { Printf("roth_worldtest stand: no seat on this map\n"); return; }
		if (argv.argc() < 4 && !T.haveNormal) { Printf("roth_worldtest stand: the spot has no surface normal: give a bearing\n"); return; }
		const double dist = atof(argv[2]) * upm;
		const double bearing = front ? atan2(S.seats[0].front.Y, S.seats[0].front.X)
			: argv.argc() >= 4 ? atof(argv[3]) * (M_PI / 180.0) : atan2(T.normal.Y, T.normal.X);
		const DVector3 stand(T.spot.X + cos(bearing) * dist, T.spot.Y + sin(bearing) * dist, pmo->Z());
		pmo->SetOrigin(stand, false);
		pmo->SetZ(pmo->floorz);
		pmo->Vel.Zero();
		pmo->Angles.Yaw = DAngle::fromRad(atan2(T.spot.Y - stand.Y, T.spot.X - stand.X));
		pmo->ClearInterpolation();
		Printf("[roth_worldtest] the player stands at (%.0f %.0f %.0f) in sector %d, facing %.0f, %.2f m from the spot\n", pmo->X(), pmo->Y(), pmo->Z(),
			pmo->Sector ? pmo->Sector->Index() : -1, pmo->Angles.Yaw.Degrees(), dist / upm);
		return;
	}
	if (!stricmp(verb, "turn") && argv.argc() >= 3)
	{
		pmo->Angles.Yaw += DAngle::fromDeg(atof(argv[2]));
		pmo->ClearInterpolation();
		Printf("[roth_worldtest] the player turned %s degrees: facing %.0f\n", argv[2], pmo->Angles.Yaw.Degrees());
		return;
	}
	if (!stricmp(verb, "spot") && argv.argc() >= 3 && !stricmp(argv[2], "mouth"))
	{
		// the spot: the mouth (8 cm under the eyes) and `fwd` metres ahead of it
		const double fwd = argv.argc() >= 4 ? atof(argv[3]) : 0.0;
		const DVector3 eye = EyeNow(pmo);
		T.spot = eye - DVector3(0, 0, wh::kMouthBelowEyeM * upm / ps) + DVector3(pmo->Angles.Yaw.Cos() * fwd * upm, pmo->Angles.Yaw.Sin() * fwd * upm, 0);
		T.haveSpot = true;
		T.haveNormal = false;
		Printf("[roth_worldtest] the spot: the mouth + %.2f m ahead (%.1f %.1f %.1f), the eye at (%.1f %.1f %.1f)\n", fwd, T.spot.X, T.spot.Y, T.spot.Z, eye.X, eye.Y, eye.Z);
		return;
	}
	if (!stricmp(verb, "head") && argv.argc() >= 3)
	{
		float pos[3]; float yaw = 0.f;
		if (!VRPoseTest_Head(pos, yaw)) { Printf("roth_worldtest head: roth_xrpose on first\n"); return; }
		const double stand = pmo->player->DefaultViewHeight() * ps / upm;
		pos[1] = (float)(stand - atof(argv[2]));
		VRPoseTest_Set(true, yaw, pos);
		Printf("[roth_worldtest] the head %.2f m below standing (%.3f m up in the tracking space)\n", atof(argv[2]), pos[1]);
		return;
	}
	if (!stricmp(verb, "report"))
	{
		Printf("[roth_worldtest] tic %d: the right-hand item %d; the player at (%.0f %.0f %.0f) facing %.0f\n", S.level->maptime, RightItem(), pmo->X(), pmo->Y(),
			pmo->Z(), pmo->Angles.Yaw.Degrees());
		for (int role = 0; role < 2; role++)
		{
			HandNow h;
			if (!ReadHand(role, h)) { Printf("[roth_worldtest]   %s hand: none\n", RoleName(role)); continue; }
			const HandW &w = S.hand[role];
			Printf("[roth_worldtest]   %s hand: palm (%.1f %.1f %.1f), grip %d, up tilted %.0f degrees; pull %d, twist %d (%.0f degrees)\n", RoleName(role),
				h.palm.X, h.palm.Y, h.palm.Z, (int)h.grip, h.frame ? wh::TiltDeg(h.up) : -1.0, (int)w.pull.engaged, (int)w.twist.engaged, w.twist.turned);
		}
		return;
	}
	if (argv.argc() < 3) { Printf("roth_worldtest %s: which hand?\n", verb); return; }
	const int role = RoleOf(argv[2]);
	if (role < 0) { Printf("roth_worldtest: main or off\n"); return; }
	TestHold &t = T.hold[role];
	if (!stricmp(verb, "hold") && argv.argc() >= 4 && !stricmp(argv[3], "off")) { t.on = false; t.moveTics = 0; Printf("[roth_worldtest] the %s hand is free\n", RoleName(role)); return; }
	if (!stricmp(verb, "hold") && argv.argc() >= 7)
	{
		if (!T.haveSpot) { Printf("roth_worldtest hold: no spot (roth_worldtest spot <record> first)\n"); return; }
		t.on = true;
		t.what = !stricmp(argv[3], "base") ? 1 : !stricmp(argv[3], "tip") ? 2 : 0;
		if (!stricmp(argv[4], "n"))
		{
			// n <out> <up> [<along>]: metres out along the spot's normal, up, and along its surface
			if (!T.haveNormal) { Printf("roth_worldtest hold: the spot has no surface normal\n"); t.on = false; return; }
			const double out = atof(argv[5]), up = atof(argv[6]), al = argv.argc() >= 8 ? atof(argv[7]) : 0.0;
			t.offset = Units(V3(T.normal.X * out + T.along.X * al, T.normal.Y * out + T.along.Y * al, up));
		}
		else t.offset = Units(V3(atof(argv[4]), atof(argv[5]), atof(argv[6])));
		t.moveTics = 0;
		t.logged = 0;
		Printf("[roth_worldtest] the %s hand's %s held at the spot + (%s, %s, %s) m\n", RoleName(role), argv[3], argv[4], argv[5], argv[6]);
		return;
	}
	if (!stricmp(verb, "move") && argv.argc() >= 6 && !stricmp(argv[3], "n") && T.haveNormal)
	{
		// move <role> n <out> <tics>: out along the spot's normal
		const double out = atof(argv[4]);
		t.moveFrom = t.offset;
		t.moveTo = t.offset + Units(V3(T.normal.X * out, T.normal.Y * out, 0));
		t.moveStart = S.level->maptime;
		t.moveTics = std::max(1, atoi(argv[5]));
		Printf("[roth_worldtest] the %s hand moves %s m out along the spot's normal over %d tics from tic %d\n", RoleName(role), argv[4], t.moveTics, t.moveStart);
		return;
	}
	if (!stricmp(verb, "move") && argv.argc() >= 7)
	{
		t.moveFrom = t.offset;
		t.moveTo = t.offset + Units(V3(atof(argv[3]), atof(argv[4]), atof(argv[5])));
		t.moveStart = S.level->maptime;
		t.moveTics = std::max(1, atoi(argv[6]));
		Printf("[roth_worldtest] the %s hand moves (%s, %s, %s) m over %d tics from tic %d\n", RoleName(role), argv[3], argv[4], argv[5], t.moveTics, t.moveStart);
		return;
	}
	if (!stricmp(verb, "grip") && argv.argc() >= 4)
	{
		FVRPoseTestHand ph;
		if (!VRPoseTest_Hand(role, ph)) { Printf("roth_worldtest grip: no %s hand (roth_xrpose hand first)\n", RoleName(role)); return; }
		ph.grip = (float)atof(argv[3]);   // the trigger and its touch as the pose gave them
		VRPoseTest_SetHand(role, ph);
		Printf("[roth_worldtest] the %s hand's grip %.2f (tic %d)\n", RoleName(role), ph.grip, S.level->maptime);
		return;
	}
	if (!stricmp(verb, "roll") && argv.argc() >= 5)
	{
		t.rollLeft = std::max(1, atoi(argv[4]));
		t.rollPerTic = atof(argv[3]) / t.rollLeft;
		Printf("[roth_worldtest] the %s hand rolls %s degrees over %d tics from tic %d\n", RoleName(role), argv[3], t.rollLeft, S.level->maptime);
		return;
	}
	Printf("roth_worldtest: unknown or short '%s'\n", verb);
}
