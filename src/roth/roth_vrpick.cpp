//
// roth_vrpick.cpp -- THE VR HANDS POINT AND TOUCH (phase 1 of the VR interaction).
//
// THE OWNER'S RULE FOR THIS FILE (2026-11-13): VR INTERACTION -- how the player
// points, touches and grabs -- may be new; GAME LOGIC may not. Every gesture
// here ends in a call the flat game already makes:
//
//   what the hand does                   what runs                         the original
//   ------------------------------------ --------------------------------- ------------------------------
//   points (the ray, every tic)          ClassifyPick                      classify_cursor_target_object
//   points at it and holds (dwell)       ExaminePick                       examine_object_under_cursor
//   points, taps the index on the        Activate(Use)                     activate_targeted_object (the
//     trigger and lifts it (a click)                                         port's use path, as roth_pick)
//   presses it with the fingertip        Activate(Use)                     the same
//   squeezes the grip with the palm      Activate(Use)                     the same; an item goes through
//     on it                                                                  give_item_by_dbase_id and flies
//
// and every one is gated by the original's own click rules on that
// classification (run_gameplay_frame, roth_runtime.h CursorClass): an examine
// needs a cursor of type 3 or 5 with something to examine, a use a cursor of
// type 3 within the use range. Where the original shows no cursor, the hand
// shows no marker.
//
// WHY THESE GESTURES. The controllers' buttons are already the game's -- the
// triggers fire the held weapon, A is +use, B/X/Y and the sticks are bound
// through the shared doomxr.ini, the main grip is the shift layer
// (vr_secondary_button_mappings) -- and nothing here may take one away. So the
// gestures are made of what no binding reads: the capacitive touches and the
// analogue grip, as the hands already read them to pose the fingers
// (vr_handpose.cpp). None of this suppresses or emits a key.
//
//   POINT     grip held (>= 0.5) and the index OFF the trigger (no trigger
//             touch). The thumb may be down or up (point or finger gun): both
//             have the index out. This is the hands' own "point" pose.
//   EXAMINE   DWELL: the ray held on something examinable for 0.8 s fires the
//             examine once; a ring closes on the marker while it counts. Leave
//             the target or stop pointing and it re-arms. Chosen over a button
//             because every button is taken, and over a thumb gesture because
//             the thumb rests on the stick that walks: lifting it to stop
//             walking would read as a click. An examine only speaks or shows a
//             close-up, so a dwell that fires when not meant costs a line of
//             dialogue, never an item or a lever.
//   USE far   INDEX TAP: while pointing, touch the trigger with the index and
//             lift it again within 0.5 s without pulling it (< 0.3). It is the
//             mouse click of a pointing hand, and a touch emits no key: the
//             trigger's key is its PULL. The use acts on what was under the ray
//             when the index came down, and only if the ray is still on it when
//             the index lifts -- so a tap that slid off acts on nothing.
//   USE near  FINGERTIP PRESS: pointing, the fingertip within 2.5 cm of a usable
//             thing (a switch face, a door, a floor plate, an object): fires
//             once, re-arms when the tip backs off past 6 cm.
//             GRIP SQUEEZE: the grip closing (0.35 -> 0.7) with the palm on a
//             Realms object (15 cm) or a usable surface 20 cm along the hand.
// Both hands work the same way.
//
// THE RAY. From the index fingertip (the FBX's INDEX_UP_TOP_joint) along the
// finger (INDEX_BASE_joint -> tip), both as last drawn by the posed hand
// (VRHandPose_FingerPoints) and placed through the hand actor's own draw
// transform (Model_ActorPointToWorld), so the beam leaves the finger the player
// sees. With no hand model (no hand actor this tic) it falls back to the
// controller's aim pose, which is what roth_pick and the panels use.
//
// THE DRAWING is billboards, as every Realms panel is: twelve small dots along
// the ray and a ring at the hit, tinted by the original's cursor family --
//   usable in reach (shapes 0x248, 0x250, 0x278)   candle white
//   too far        (0x398, 0x3a0)                  dim grey
//   blocked / keyed / an attack target (0x258, 0x260)  dull red
//   nothing        (0x240)                         a fainter beam, no ring
// In VR they are welded to the controller (BBFL_HANDMAIN / BBFL_HANDOFF), so
// they move at the display's rate, not the tic's. Subtle on purpose.
//
// HAPTICS (VR_ScriptHaptic, the engine's one live call): a light tick when the
// ray comes onto something the hand could act on, a firmer one when an action
// fires.
//
// RANGES are the original's own: the pick depth 2z + 64 against 600 / 2000 /
// 5000, z measured from the player's x,y along the ray's yaw (measured,
// REMAROTH_MEASURED.md 13; roth_runtime.h says why the ray's yaw). A hand's own
// reach -- fingertip contact, the palm -- is in METRES times VR_UnitsPerMeter(),
// so a hand reaches as far on every map.
//
// TEST (flat, no headset): roth_vrpicktest, with the hand test's poses
// (roth_handtest) as the finger inputs. See the command at the bottom.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_vrpick.h"
#include "roth_runtime.h"
#include "roth_interface.h"
#include "roth_hands.h"
#include "roth_objects.h"
#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "hw_vrmodes.h"
#include "r_data/models.h"
#include "r_data/vr_handpose.h"
#include "r_data/vr_handpose_math.h"
#include "i_time.h"
#include "hw_perflog.h"
#include <math.h>
#include <algorithm>
#include <vector>

EXTERN_CVAR(Int, vr_mode)   // file scope, never inside a namespace
CVAR(Bool, roth_vrpick, true, 0)          // the VR hands' picker; not saved
CVAR(Bool, roth_vrpick_debug, false, 0)   // a line per change of what each hand points at

namespace roth
{

namespace
{

// ---- tuning (metres unless said) ----
constexpr double kPokeIn      = 0.025;   // fingertip contact
constexpr double kPokeOut     = 0.06;    // re-arm distance
constexpr double kGrabReach   = 0.20;    // palm, along the hand
constexpr double kGrabBack    = 0.08;    // the grip's ray starts this far behind the palm
constexpr double kGrabRadius  = 0.15;    // palm to an object's box
constexpr double kNoHitBeam   = 0.60;    // the beam's length with nothing at its end
constexpr int    kDwellTics   = 28;      // 0.8 s
constexpr int    kTapMaxTics  = 18;      // 0.51 s
constexpr float  kTapMaxPull  = 0.30f;
constexpr float  kPointGrip   = 0.50f;
constexpr float  kGripClose   = 0.70f, kGripOpen = 0.35f;
constexpr int    kDots        = 12;
constexpr double kDotM = 0.004, kMarkerM = 0.014, kRingM = 0.020;

struct HandState
{
	// inputs
	bool  valid = false;
	float trigger = 0.f, grip = 0.f;
	bool  triggerTouch = false, thumbTouch = false;
	bool  pointing = false, wasPointing = false;
	bool  gripClosed = false;

	// the ray and what is at its end
	bool        haveRay = false;
	DVector3    tip, dir, palm;
	bool        fromFinger = false;
	const char *rayWhy = nullptr;   // why there is no ray (a log line)
	const char *loggedRayWhy = nullptr;
	int         loggedInputs = -1;  // the input state last reported
	bool        havePick = false;
	Pick        pick;
	CursorClass cls;

	// gestures
	Pick  dwellTarget;
	Pick  noDwell;          // the last target reported as not examinable (a log line)
	int   dwellTics = 0;
	bool  dwellDone = false;
	bool  tapDown = false;
	int   tapTics = 0;
	float tapPull = 0.f;
	Pick  tapTarget;
	bool  pokeArmed = true;
	double lastPokeDist = 1e9;   // last tic's distance from the tip, and its ray
	DVector3 lastDir;
	Pick  hover;            // the last target the hand could act on (haptic tick)
	Pick  logged;           // the last target reported (debug line)
	uint16_t loggedShape = 0;

	// billboards
	int dot[kDots] = {};
	int marker = 0, ring = 0;
	bool shown = false;      // any of them visible (so an idle hand touches none)
};

struct VRPick
{
	FLevelLocals *level = nullptr;
	HandState hand[2];      // by role: VR_MAINHAND, VR_OFFHAND

	// the flat test (roth_vrpicktest)
	bool   test = false;
	int    testHands = 1;   // bit 0 main, bit 1 off
	bool   testAimAt = false;
	DVector3 testAim;
	double testPitch = 0, testYaw = 0;

	const char *loggedStand = "";   // why the hands last stood down ("" = never reported)

	// the cost counters (VRPickTick)
	int    costTics = 0;
	double costSum = 0, costMax = 0;
};
VRPick P;

const char *RoleName(int role) { return role == VR_MAINHAND ? "main" : "off"; }

// The decision lines (what each hand is doing and why) are printed under the
// flat test or roth_vrpick_debug only.
bool Logging() { return P.test || roth_vrpick_debug; }

// Why a click on this classification would not use / examine, for a log line.
FString WhyNotUse(const CursorClass &c)
{
	if (c.CanUse()) return "usable";
	if (c.type != 3) return FStringf("not usable: cursor type %d (use needs 3)", (int)c.type);
	return FStringf("not usable: depth %.0f >= 600 (planar %.0f >= 268)", c.depth, (c.depth - 64.0) / 2.0);
}
FString WhyNotExamine(const CursorClass &c)
{
	if (c.CanExamine()) return "examinable";
	if (c.examinable == 0) return "nothing to examine (no examine trigger, no OnInspect block)";
	return FStringf("not examinable: cursor type %d (examine needs 3 or 5)", (int)c.type);
}

double UPM()
{
	const double u = VR_UnitsPerMeter();
	return u > 1e-3 ? u : 34.0;
}

bool HeadsetOn()
{
	const VRMode *vm = VRMode::GetVRModeCached(true);
	return vr_mode != 0 && vm != nullptr && vm->IsVR();
}

//--------------------------------------------------------------------------
// Billboards
//--------------------------------------------------------------------------

FBillboard *FindBB(int id)
{
	if (P.level == nullptr || id == 0) return nullptr;
	for (FBillboard &b : P.level->Billboards) if (b.id == id) return &b;
	return nullptr;
}

FBillboard *EnsureBB(int &id, int payload)
{
	FBillboard *b = FindBB(id);
	if (b != nullptr) return b;
	if (P.level == nullptr) return nullptr;
	FBillboard bb;
	bb.id = P.level->NextBillboardID++;
	bb.payload = payload;
	bb.facing = BBF_CAMERA;
	bb.flags = BBFL_PERSISTENT | BBFL_NOSAVE | BBFL_NOHIT;
	bb.alpha = 0.0;
	bb.width = bb.height = 1.0;
	bb.spawntic = P.level->maptime;
	P.level->Billboards.Push(bb);
	id = bb.id;
	return FindBB(id);
}

void HideBB(int id)
{
	FBillboard *b = FindBB(id);
	if (b != nullptr) b->alpha = 0.0;
}

void HideHand(HandState &h)
{
	if (!h.shown) return;
	h.shown = false;
	for (int &d : h.dot) HideBB(d);
	HideBB(h.marker);
	HideBB(h.ring);
}

// Where a world point sits in the frame a hand-locked billboard rides
// (g_levellocals.h BillboardHandFrame / BillboardHandAxes): resolved by the
// renderer at the display's rate, so the beam stays on the controller between
// tics instead of stepping at 35 Hz.
struct HandLock
{
	bool on = false;
	int flag = 0;
	DVector3 origin, fwd, right, up;
	DVector3 Local(const DVector3 &w) const
	{
		const DVector3 d = w - origin;
		return DVector3(d | fwd, d | right, d | up);
	}
};

HandLock LockFor(int role, AActor *pmo)
{
	HandLock L;
	if (pmo == nullptr || !pmo->OverrideAttackPosDir) return L;   // no VR pose published: world points
	FBillboard probe;
	probe.flags = (role == VR_MAINHAND) ? BBFL_HANDMAIN : BBFL_HANDOFF;
	double yaw, pitch, roll;
	if (!BillboardHandFrame(probe, L.origin, yaw, pitch, roll)) return L;
	BillboardHandAxes(yaw, pitch, roll, L.fwd, L.right, L.up);
	L.on = true;
	L.flag = probe.flags;
	return L;
}

void PlaceBB(int &id, int payload, const HandLock &L, const DVector3 &world, double size, PalEntry col, double alpha)
{
	FBillboard *b = EnsureBB(id, payload);
	if (b == nullptr) return;
	b->flags = (b->flags & ~BBFL_HANDANY) | (L.on ? L.flag : 0);
	b->pos = L.on ? L.Local(world) : world;
	b->width = b->height = size;
	b->color = col;
	b->alpha = alpha;
}

// The original's cursor families (see the header comment).
PalEntry FamilyColour(uint16_t shape, double &alpha)
{
	switch (shape)
	{
	case 0x248: case 0x250: case 0x278: alpha = 0.75; return PalEntry(255, 236, 214, 170);
	case 0x398: case 0x3a0:             alpha = 0.55; return PalEntry(255, 120, 124, 132);
	case 0x258: case 0x260:             alpha = 0.70; return PalEntry(255, 150, 36, 28);
	default:                            alpha = 0.0;  return PalEntry(255, 200, 200, 200);
	}
}

//--------------------------------------------------------------------------
// Inputs and the ray
//--------------------------------------------------------------------------

void ReadInputs(int role, HandState &h)
{
	h.valid = false;
	h.trigger = h.grip = 0.f;
	h.triggerTouch = h.thumbTouch = false;
	if (P.test)
	{
		FVRHandTestInputs in;
		if (!(P.testHands & (1 << role)) || !VRHandTest_Inputs(in)) return;
		h.trigger = in.trigger; h.grip = in.grip; h.triggerTouch = in.triggerTouch; h.thumbTouch = in.thumbTouch;
		h.valid = true;
		return;
	}
	const VRMode *vm = VRMode::GetVRModeCached(true);
	VSMatrix g;
	if (vm == nullptr || !vm->GetGripTransform(role, &g)) return;
	VRFingerInput fi;
	if (vm->GetFingerInput(role, fi))
	{
		h.trigger = fi.trigger; h.grip = fi.grip; h.triggerTouch = fi.triggerTouch; h.thumbTouch = fi.thumbTouch;
	}
	h.valid = true;
}

bool FingerRay(int role, HandState &h, AActor *pmo)
{
	h.haveRay = false;
	h.fromFinger = false;
	h.rayWhy = nullptr;
	const int side = VRHandPose_SideForRole(role);
	AActor *ha = HandActor(side);
	FModel *model = ha != nullptr ? Model_ActorModel(ha) : nullptr;
	FVRHandPoints pts;
	if (ha == nullptr) h.rayWhy = "no hand actor";
	else if (model == nullptr) h.rayWhy = "the hand actor draws no model";
	else if (!VRHandPose_FingerPoints(model, side, pts)) h.rayWhy = "the hand model lacks INDEX_UP_TOP / INDEX_BASE / HANDPALM";
	else
	{
		DVector3 tip, base, palm;
		if (Model_ActorPointToWorld(ha, pts.tip, tip) && Model_ActorPointToWorld(ha, pts.indexBase, base)
			&& Model_ActorPointToWorld(ha, pts.palm, palm))
		{
			DVector3 d = tip - base;
			const double len = d.Length();
			if (len > 1e-3)
			{
				h.tip = tip; h.dir = d / len; h.palm = palm;
				h.haveRay = h.fromFinger = true;
			}
			else h.rayWhy = "the fingertip and its knuckle coincide";
		}
		else h.rayWhy = "no draw transform for the hand";
	}
	if (!h.haveRay && !P.test && pmo != nullptr && pmo->OverrideAttackPosDir)
	{
		// The controller's aim pose, as roth_pick and the panels read it: the
		// stored angle carries the engine's +90 (UseByHand).
		const bool main = role == VR_MAINHAND;
		const DAngle yaw = (main ? pmo->AttackAngle : pmo->OffhandAngle) + DAngle::fromDeg(90.);
		const DAngle pitch = main ? pmo->AttackPitch : pmo->OffhandPitch;
		h.tip = main ? pmo->AttackPos : pmo->OffhandPos;
		h.dir = DVector3(yaw.Cos() * pitch.Cos(), yaw.Sin() * pitch.Cos(), -pitch.Sin());
		h.palm = h.tip;
		h.haveRay = true;
	}
	if (h.haveRay && P.test)
	{
		// The flat test's hands hold their palms to the camera with the fingers
		// UP, so their own finger points at the ceiling: the test gives the
		// direction (at a point, or by pitch and yaw off the view) and keeps the
		// fingertip as the origin.
		if (P.testAimAt)
		{
			DVector3 d = P.testAim - h.tip;
			if (d.Length() > 1e-6) h.dir = d / d.Length();
		}
		else if (pmo != nullptr)
		{
			const DAngle yaw = pmo->Angles.Yaw + DAngle::fromDeg(P.testYaw);
			const DAngle pitch = DAngle::fromDeg(-P.testPitch);   // + looks up
			h.dir = DVector3(yaw.Cos() * pitch.Cos(), yaw.Sin() * pitch.Cos(), -pitch.Sin());
		}
	}
	return h.haveRay;
}

// The pick record's depth for a pick made along `dir` (roth_runtime.h).
double DepthOf(AActor *pmo, const Pick &p, const DVector3 &dir) { return PickDepthOfZ(PickPlanarZ(pmo, p.hitPos, dir)); }

//--------------------------------------------------------------------------
// Acting
//--------------------------------------------------------------------------

void Haptic(int role, double amp, double ms)
{
	if (P.test || !HeadsetOn()) return;
	VR_ScriptHaptic(role, amp, ms);
}

bool DoUse(int role, const Pick &p, const CursorClass &c, AActor *pmo, const char *how)
{
	if (!c.CanUse()) return false;
	const bool acted = Activate(p, pmo, TrigEvent::Use);
	Printf("[roth_vrpick] %s hand %s: USE %s at depth %.0f (cursor type %d shape 0x%x) -> %s\n", RoleName(role), how,
		PickKindName(p), c.depth, (int)c.type, (unsigned)c.shape, acted ? "acted" : "nothing happened");
	Haptic(role, acted ? 0.45 : 0.15, acted ? 35 : 12);
	return acted;
}

bool DoExamine(int role, const Pick &p, const CursorClass &c, AActor *pmo, const char *how)
{
	if (!c.CanExamine()) return false;
	const bool acted = ExaminePick(p, pmo);
	Printf("[roth_vrpick] %s hand %s: EXAMINE %s at depth %.0f (cursor type %d shape 0x%x) -> %s\n", RoleName(role), how,
		PickKindName(p), c.depth, (int)c.type, (unsigned)c.shape, acted ? "examined" : "nothing happened");
	Haptic(role, acted ? 0.35 : 0.12, acted ? 30 : 12);
	return acted;
}

// The grip closing: what the palm is on. The Realms objects within reach of
// the palm first -- an item on a table is small and a ray misses it easily --
// nearest first, the first the original's cursor would let a click use (so a
// table the palm rests on does not hide the key lying on it); then a short ray
// along the hand, for a lever or a switch face.
bool GrabNear(int role, HandState &h, AActor *pmo)
{
	const double upm = UPM();
	const double r = kGrabRadius * upm;
	std::vector<std::pair<double, AActor *>> near;
	auto it = P.level->GetThinkerIterator<AActor>();
	AActor *mo;
	while ((mo = it.Next()) != nullptr)
	{
		if (mo == pmo || (mo->ObjectFlags & OF_EuthanizeMe)) continue;
		int rs, ri;
		if (!ObjectForActor(mo, rs, ri)) continue;
		// distance from the palm to the box the object is picked by
		DVector3 lo, hi;
		ObjectPickBox(mo, lo, hi);
		const double dx = std::max(0.0, std::max(lo.X - h.palm.X, h.palm.X - hi.X));
		const double dy = std::max(0.0, std::max(lo.Y - h.palm.Y, h.palm.Y - hi.Y));
		const double dz = std::max(0.0, std::max(lo.Z - h.palm.Z, h.palm.Z - hi.Z));
		const double d = sqrt(dx * dx + dy * dy + dz * dz);
		if (d <= r) near.push_back({ d, mo });
	}
	std::sort(near.begin(), near.end(), [](const std::pair<double, AActor *> &a, const std::pair<double, AActor *> &b) { return a.first < b.first; });
	if (Logging())
		Printf("[roth_vrpick] %s hand grip closes: palm at (%.1f %.1f %.1f), %d object(s) within %.1f units (%.2f m)\n",
			RoleName(role), h.palm.X, h.palm.Y, h.palm.Z, (int)near.size(), r, kGrabRadius);
	Pick objPick;
	CursorClass objCls;
	bool haveObj = false;
	for (const auto &n : near)
	{
		Pick p;
		p.kind = Pick::Prop;
		p.actor = n.second;
		p.hitPos = n.second->PosPlusZ(n.second->Height * 0.5);
		p.dist = n.first;
		const CursorClass c = ClassifyPick(p, pmo, DepthOf(pmo, p, p.hitPos - h.palm));
		if (Logging())
			Printf("[roth_vrpick]   object %.1f units from the palm: cursor type %d shape 0x%x -> %s\n",
				n.first, (int)c.type, (unsigned)c.shape, WhyNotUse(c).GetChars());
		if (!c.CanUse()) continue;
		objPick = p; objCls = c; haveObj = true;
		break;
	}
	// THE SURFACE ALONG THE HAND, from a little BEHIND the palm: a palm laid
	// flat on a door is already level with it, or a hair through it, and a ray
	// starting at the palm would begin past the very surface it is pressed on.
	// So the ray starts kGrabBack behind the palm, and a hit counts from there
	// to kGrabReach in front of it.
	const double back = kGrabBack * upm;
	Pick rayPick;
	CursorClass rayCls;
	bool haveRay = false;
	if (PickFrom(pmo, h.palm - h.dir * back, h.dir, back + kGrabReach * upm, rayPick))
	{
		rayPick.dist -= back;   // from the palm; negative: just behind it
		rayCls = ClassifyPick(rayPick, pmo, DepthOf(pmo, rayPick, h.dir));
		haveRay = rayCls.CanUse();
		if (Logging())
			Printf("[roth_vrpick]   surface along the hand: %s %.1f units from the palm: cursor type %d shape 0x%x -> %s\n",
				PickKindName(rayPick), rayPick.dist, (int)rayCls.type, (unsigned)rayCls.shape, WhyNotUse(rayCls).GetChars());
	}
	else if (Logging())
		Printf("[roth_vrpick]   surface along the hand: nothing from %.1f behind the palm to %.1f in front\n", back, kGrabReach * upm);
	// THE NEARER OF THE TWO, as the original's pick is the nearest thing under
	// the cursor: a door handle on the door is taken before the door, the door
	// before a key lying a hand's width beyond it.
	if (haveObj && (!haveRay || objPick.dist <= rayPick.dist)) return DoUse(role, objPick, objCls, pmo, "grip on an object");
	if (haveRay) return DoUse(role, rayPick, rayCls, pmo, "grip on a surface");
	if (Logging())
		Printf("[roth_vrpick] %s hand grip: nothing usable within reach\n", RoleName(role));
	return false;
}

//--------------------------------------------------------------------------
// One hand, one tic
//--------------------------------------------------------------------------

void ResetGestures(HandState &h)
{
	h.dwellTarget = Pick{};
	h.dwellTics = 0;
	h.dwellDone = false;
	h.tapDown = false;
	h.pokeArmed = true;
	h.hover = Pick{};
}

void TickHand(int role, AActor *pmo)
{
	HandState &h = P.hand[role];
	ReadInputs(role, h);
	h.wasPointing = h.pointing;
	h.pointing = h.valid && h.grip >= kPointGrip && !h.triggerTouch;
	const bool gripWasClosed = h.gripClosed;
	if (!h.valid) h.gripClosed = false;
	else if (h.grip >= kGripClose) h.gripClosed = true;
	else if (h.grip <= kGripOpen) h.gripClosed = false;

	// A HAND THAT IS DOING NOTHING COSTS NOTHING: no fingertip, no trace, no
	// classification, no billboards -- only the inputs above. In the headset
	// that is most tics for most hands. Work is done while pointing, while an
	// index tap is under way (or starting), and on the tic the grip closes.
	const bool gripEdge = h.gripClosed && !gripWasClosed;
	const bool tapStarting = !h.tapDown && h.wasPointing && h.triggerTouch;
	if (Logging())
	{
		const int state = (h.valid ? 1 : 0) | (h.pointing ? 2 : 0) | (h.gripClosed ? 4 : 0) | (h.triggerTouch ? 8 : 0) | (h.thumbTouch ? 16 : 0);
		if (state != h.loggedInputs)
		{
			h.loggedInputs = state;
			Printf("[roth_vrpick] %s hand inputs: %s grip %.2f trigger %.2f, trigger touch %d, thumb touch %d -> %s%s\n", RoleName(role),
				h.valid ? "" : "NO CONTROLLER,", h.grip, h.trigger, (int)h.triggerTouch, (int)h.thumbTouch,
				h.pointing ? "pointing" : "not pointing", h.gripClosed ? ", grip closed" : "");
		}
	}
	const bool busy = h.valid && (h.pointing || h.tapDown || tapStarting || gripEdge);
	if (busy && !FingerRay(role, h, pmo) && Logging() && h.rayWhy != h.loggedRayWhy)
		Printf("[roth_vrpick] %s hand: no ray -- %s\n", RoleName(role), h.rayWhy ? h.rayWhy : "unknown");
	if (busy) h.loggedRayWhy = h.haveRay ? nullptr : h.rayWhy;
	if (!busy || !h.haveRay)
	{
		h.havePick = false;
		ResetGestures(h);
		HideHand(h);
		return;
	}

	const double upm = UPM();

	// THE PICK while pointing (the beam, the dwell, the press) and while a tap
	// is under way (the tic the index lifts). The tic a tap starts reads last
	// tic's pick: what the ray was on when the index came down.
	const Pick before = h.havePick ? h.pick : Pick{};
	const bool hadPick = h.havePick;
	if (h.pointing || h.tapDown)
	{
		h.havePick = PickFrom(pmo, h.tip, h.dir, PickTraceRange(h.dir), h.pick);
		h.cls = h.havePick ? ClassifyPick(h.pick, pmo, DepthOf(pmo, h.pick, h.dir)) : CursorClass{};
	}
	else
	{
		h.havePick = false;
		h.cls = CursorClass{};
	}

	if (roth_vrpick_debug || P.test)
	{
		const bool changed = h.havePick ? (!SamePickTarget(h.pick, h.logged) || h.cls.shape != h.loggedShape)
		                                 : h.logged.kind != Pick::Nothing;
		if (changed)
		{
			if (h.havePick)
				Printf("[roth_vrpick] %s hand %s: %s at planar %.0f (depth %.0f), %.0f along the ray from the %s: cursor type %d shape 0x%x"
					" (record type %d, examinable %d%s)\n", RoleName(role), h.pointing ? "points" : "rests",
					PickKindName(h.pick), (h.cls.depth - 64.0) / 2.0, h.cls.depth, h.pick.dist, h.fromFinger ? "fingertip" : "aim pose",
					(int)h.cls.type, (unsigned)h.cls.shape, (int)h.cls.recType, h.cls.examinable,
					h.cls.outOfRange ? ", out of range" : "");
			else
				Printf("[roth_vrpick] %s hand: nothing on the ray\n", RoleName(role));
			h.logged = h.havePick ? h.pick : Pick{};
			h.loggedShape = h.havePick ? h.cls.shape : 0;
		}
	}

	// ---- GRIP SQUEEZE (near use) ----
	if (gripEdge) GrabNear(role, h, pmo);

	// ---- INDEX TAP (far use) ----
	if (h.tapDown)
	{
		h.tapTics++;
		if (h.trigger > h.tapPull) h.tapPull = h.trigger;
		if (h.tapPull >= kTapMaxPull || h.tapTics > kTapMaxTics || h.grip < kPointGrip) h.tapDown = false;   // a shot, a hold, or a let-go
		else if (!h.triggerTouch)
		{
			h.tapDown = false;
			if (h.havePick && SamePickTarget(h.pick, h.tapTarget)) DoUse(role, h.pick, h.cls, pmo, "index tap");
			else if (P.test || roth_vrpick_debug) Printf("[roth_vrpick] %s hand index tap: off the target it began on, nothing done\n", RoleName(role));
		}
	}
	else if (tapStarting && h.grip >= kPointGrip && h.trigger < kTapMaxPull && hadPick)
	{
		h.tapDown = true;
		h.tapTics = 0;
		h.tapPull = h.trigger;
		h.tapTarget = before;   // what the ray was on when the index came down
	}

	if (!h.pointing)
	{
		if (Logging() && h.dwellTics > 0 && !h.dwellDone)
			Printf("[roth_vrpick] %s hand dwell: broken off at %d/%d tics (the hand stopped pointing)\n", RoleName(role), h.dwellTics, kDwellTics);
		h.dwellTarget = Pick{};
		h.dwellTics = 0;
		h.dwellDone = false;
		h.pokeArmed = true;
		h.lastPokeDist = 1e9;
		h.hover = Pick{};
		HideHand(h);
		return;
	}

	// ---- FINGERTIP PRESS (near use) ----
	// A quick press can cross the 2.5 cm window between two tics and leave the
	// tip already through the surface, where the trace no longer meets it. So
	// a surface the tip was close to last tic and has now reached or passed
	// (along last tic's ray) counts as pressed too. Not for an object: its
	// actor may be gone by now, and a pointer to it is not followed.
	if (h.havePick && h.pick.dist <= kPokeIn * upm)
	{
		if (h.pokeArmed)
		{
			h.pokeArmed = false;
			DoUse(role, h.pick, h.cls, pmo, "fingertip press");
		}
	}
	else if (h.pokeArmed && h.wasPointing && hadPick && before.kind != Pick::Prop && h.lastPokeDist <= kPokeOut * upm
		&& ((before.hitPos - h.tip) | h.lastDir) <= kPokeIn * upm)
	{
		h.pokeArmed = false;
		const CursorClass c = ClassifyPick(before, pmo, DepthOf(pmo, before, h.lastDir));
		DoUse(role, before, c, pmo, "fingertip press (through)");
	}
	else if (!h.havePick || h.pick.dist > kPokeOut * upm) h.pokeArmed = true;
	h.lastPokeDist = h.havePick ? h.pick.dist : 1e9;
	h.lastDir = h.dir;

	// ---- DWELL (examine) ----
	if (h.havePick && h.cls.CanExamine())
	{
		if (!SamePickTarget(h.pick, h.dwellTarget))
		{
			h.dwellTarget = h.pick; h.dwellTics = 0; h.dwellDone = false;
			if (Logging()) Printf("[roth_vrpick] %s hand dwell: starts on %s (cursor type %d shape 0x%x, %d to examine)\n",
				RoleName(role), PickKindName(h.pick), (int)h.cls.type, (unsigned)h.cls.shape, h.cls.examinable);
		}
		if (!h.dwellDone)
		{
			++h.dwellTics;
			if (Logging() && h.dwellTics % 7 == 0 && h.dwellTics < kDwellTics)
				Printf("[roth_vrpick] %s hand dwell: %d/%d tics on %s\n", RoleName(role), h.dwellTics, kDwellTics, PickKindName(h.pick));
			if (h.dwellTics >= kDwellTics)
			{
				h.dwellDone = true;
				DoExamine(role, h.pick, h.cls, pmo, "dwell");
			}
		}
	}
	else
	{
		if (Logging() && h.havePick && !SamePickTarget(h.pick, h.noDwell))
			Printf("[roth_vrpick] %s hand dwell: none on %s -- %s\n", RoleName(role), PickKindName(h.pick), WhyNotExamine(h.cls).GetChars());
		h.noDwell = h.havePick ? h.pick : Pick{};
		if (Logging() && h.dwellTics > 0 && !h.dwellDone)
			Printf("[roth_vrpick] %s hand dwell: broken off at %d/%d tics\n", RoleName(role), h.dwellTics, kDwellTics);
		h.dwellTarget = Pick{};
		h.dwellTics = 0;
		h.dwellDone = false;
	}

	// ---- HAPTIC TICK on coming onto something actionable ----
	const bool actionable = h.havePick && (h.cls.CanUse() || h.cls.CanExamine());
	if (actionable && !SamePickTarget(h.pick, h.hover)) { h.hover = h.pick; Haptic(role, 0.12, 12); }
	else if (!actionable) h.hover = Pick{};

	// ---- THE BEAM ----
	h.shown = true;
	const HandLock L = LockFor(role, pmo);
	double markAlpha = 0.0;
	const PalEntry col = h.havePick ? FamilyColour(h.cls.shape, markAlpha) : PalEntry(255, 200, 200, 200);
	const double len = h.havePick ? h.pick.dist : kNoHitBeam * upm;
	const bool marked = h.havePick && markAlpha > 0.0;
	const double beamAlpha = marked ? 0.30 : 0.12;
	for (int i = 0; i < kDots; i++)
	{
		const double t = len * (i + 0.5) / kDots;
		PlaceBB(h.dot[i], BB_PANEL, L, h.tip + h.dir * t, kDotM * upm, marked ? col : PalEntry(255, 200, 200, 200), beamAlpha);
	}
	if (marked)
	{
		// Pulled back a little toward the hand so the surface does not cut it.
		const DVector3 at = h.tip + h.dir * std::max(0.0, len - 0.006 * upm);
		PlaceBB(h.marker, BB_RING, L, at, kMarkerM * upm, col, markAlpha);
		if (h.cls.CanExamine() && !h.dwellDone && h.dwellTics > 0)
		{
			const double k = 1.0 - (double)h.dwellTics / kDwellTics;   // the ring closes on the marker
			PlaceBB(h.ring, BB_RING, L, at, (kMarkerM + kRingM * 2.0 * k) * upm, col, markAlpha * 0.6);
		}
		else HideBB(h.ring);
	}
	else
	{
		HideBB(h.marker);
		HideBB(h.ring);
	}
}

void ForgetLevel()
{
	P.level = nullptr;
	for (HandState &h : P.hand)
	{
		for (int &d : h.dot) d = 0;
		h.marker = h.ring = 0;
		h.shown = false;
		h.havePick = false;
		ResetGestures(h);
	}
}

} // namespace (anonymous)

void VRPickEndLevel()
{
	ForgetLevel();
}

void VRPickTick(FLevelLocals *level, bool modal)
{
	if (level == nullptr) return;
	if (P.level != level) { ForgetLevel(); P.level = level; }

	player_t *player = level->GetConsolePlayer();
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	const bool on = roth_vrpick && pmo != nullptr && (HeadsetOn() || P.test);
	const char *owner = InterfacePointerOwner();
	const bool stand = modal || !on || player->health <= 0 || owner != nullptr;
	if (Logging())
	{
		// WHY THE HANDS STAND DOWN, on each change: a menu or the death wait,
		// a dead player, or a panel that owns the pointer (InterfacePointerOwner).
		const char *why = modal ? "a menu or the death wait" : !on ? "the picker is off"
			: player->health <= 0 ? "the player is dead" : owner;
		if (why != P.loggedStand)
		{
			if (why != nullptr) Printf("[roth_vrpick] the hands stand down: %s\n", why);
			else Printf("[roth_vrpick] the hands are live\n");
			P.loggedStand = why;
		}
	}
	const uint64_t t0 = I_nsTime();
	for (int role = 0; role < 2; role++)
	{
		if (stand)
		{
			HandState &h = P.hand[role];
			h.havePick = false;
			h.pointing = false;
			ResetGestures(h);
			HideHand(h);
			continue;
		}
		TickHand(role, pmo);
	}

	// WHAT IT COSTS, measured every tic it runs: into the perf log's CPU
	// timings as "game.vrpick" (summed per frame, like every cpu_fx_ms entry),
	// and, under the flat test or roth_vrpick_debug, a line every 70 tics with
	// the average and the worst tic in microseconds.
	if (stand) return;
	const double us = (double)(I_nsTime() - t0) / 1000.0;
	if (PerfLog::GroupsWanted()) PerfLog::AddCpuSample("game.vrpick", us / 1000.0);
	P.costSum += us;
	if (us > P.costMax) P.costMax = us;
	if (++P.costTics >= 70)
	{
		if (P.test || roth_vrpick_debug)
			Printf("[roth_vrpick] cost over %d tics: avg %.1f us, worst %.1f us a tic (both hands)\n",
				P.costTics, P.costSum / P.costTics, P.costMax);
		P.costTics = 0; P.costSum = 0; P.costMax = 0;
	}
}

} // namespace roth

//===========================================================================
//
// roth_vrpicktest -- THE FLAT TEST. Test-only: inert until typed. The picker
// runs in flat with the hand test's inputs (roth_handtest <pose> sets the
// grip, trigger and touches), from the test hands' own fingertips, so a
// capture shows the beam leaving the finger and a log line says what the
// original's cursor would be and what each gesture did. While it is on, the
// test hands' fingers are tipped forward (VRHandTest_SetTilt 90), so the drawn
// finger points the way the test ray goes rather than at the ceiling.
//
//   roth_vrpicktest on [main|off|both]   start (default: the main hand only)
//   roth_vrpicktest aim <pitch> [yaw]    the test ray's direction off the view (degrees, + up / + left)
//   roth_vrpicktest seek item|door|face [metres]
//                                        stand the player so the TEST HAND is <metres> (default 1.0)
//                                        in front of the nearest pickable item / door leaf / wall face
//                                        with a use trigger, facing it, and aim the test ray at it.
//                                        In metres, because the hand is placed in metres
//                                        (VR_UnitsPerMeter: 34 in flat, about 107 in the headset)
//   roth_vrpicktest off
//
// The gestures are the real ones, driven by the poses: an index tap is
// "roth_handtest ready" then "roth_handtest point" within 18 tics; a grip
// squeeze is "roth_handtest open" then "roth_handtest fist"; a dwell is
// pointing for 28 tics at something examinable.
//
//===========================================================================

CCMD(roth_vrpicktest)
{
	using namespace roth;
	if (argv.argc() < 2)
	{
		Printf("roth_vrpicktest on [main|off|both] | aim <pitch> [yaw] | seek item|door|face [metres] | off\n");
		return;
	}
	if (!stricmp(argv[1], "off"))
	{
		P.test = false;
		P.testAimAt = false;
		VRHandTest_SetTilt(0.f);
		Printf("roth_vrpicktest: off\n");
		return;
	}
	if (!stricmp(argv[1], "on"))
	{
		P.test = true;
		P.testHands = 1;
		if (argv.argc() >= 3 && !stricmp(argv[2], "off")) P.testHands = 2;
		if (argv.argc() >= 3 && !stricmp(argv[2], "both")) P.testHands = 3;
		VRHandTest_SetTilt(90.f);
		Printf("roth_vrpicktest: on, %s; the original's bands: use z < 268, too far < 968, examine < 2468"
			" (depth 2z + 64 against 600 / 2000 / 5000); flat use key range %.0f; %.1f units per metre\n",
			P.testHands == 1 ? "main hand" : P.testHands == 2 ? "off hand" : "both hands",
			players[consoleplayer].mo ? players[consoleplayer].mo->FloatVar(NAME_UseRange) : 0.0, UPM());
		return;
	}
	if (!stricmp(argv[1], "aim"))
	{
		P.testAimAt = false;
		P.testPitch = argv.argc() >= 3 ? atof(argv[2]) : 0.0;
		P.testYaw = argv.argc() >= 4 ? atof(argv[3]) : 0.0;
		Printf("roth_vrpicktest: aim pitch %.1f yaw %.1f off the view\n", P.testPitch, P.testYaw);
		return;
	}
	if (!stricmp(argv[1], "seek") && argv.argc() >= 3)
	{
		AActor *pmo = players[consoleplayer].mo;
		if (pmo == nullptr) { Printf("roth_vrpicktest seek: no player\n"); return; }
		const double metres = argv.argc() >= 4 ? atof(argv[3]) : 1.0;
		// Where the test hand hangs off the eye: roth_hands.cpp stands it at
		// VRHandTest_ActorSpot, the same placement the test grip uses.
		const int side = VRHandPose_SideForRole(VR_MAINHAND);
		DVector3 spot0;
		VRHandTest_ActorSpot(side, DVector3(0, 0, 0), 0.0, spot0);   // yaw 0: ahead +x, right -y
		PickTestHand hand;
		hand.ahead = spot0.X;
		hand.right = -spot0.Y;
		hand.up = spot0.Z;
		hand.metresOff = metres * UPM();
		// A grip test wants nothing else within the palm's reach.
		hand.clearRadius = metres < 0.3 ? (kGrabRadius + 0.05) * UPM() : 0.0;
		DVector3 aim, stand;
		if (!PickTestTarget(argv[2], pmo, hand, aim, stand)) { Printf("roth_vrpicktest seek: no reachable %s on this map\n", argv[2]); return; }
		const double yawDeg = atan2(aim.Y - stand.Y, aim.X - stand.X) * (180.0 / M_PI);
		pmo->SetOrigin(stand, false);
		pmo->Vel.Zero();
		pmo->Angles.Yaw = DAngle::fromDeg(yawDeg);
		pmo->Angles.Pitch = nullAngle;
		pmo->ClearInterpolation();
		P.testAimAt = true;
		P.testAim = aim;
		Printf("roth_vrpicktest seek %s: aiming at (%.0f %.0f %.0f), standing at (%.0f %.0f %.0f): the hand %.2f m"
			" (%.1f units) from it\n", argv[2], aim.X, aim.Y, aim.Z, stand.X, stand.Y, stand.Z, metres, metres * UPM());
		return;
	}
	Printf("roth_vrpicktest: unknown '%s'\n", argv[1]);
}
