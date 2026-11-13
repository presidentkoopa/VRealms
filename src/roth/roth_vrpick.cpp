//
// roth_vrpick.cpp -- THE VR HANDS POINT AND TOUCH (phase 1 of the VR interaction).
//
// THE OWNER'S RULE FOR THIS FILE (2026-11-13): VR INTERACTION -- how the player
// points, touches and grabs -- may be new; GAME LOGIC may not. Every gesture
// here ends in a call the flat game already makes:
//
// THE OWNER'S BUTTONS (2026-11-13): the TRIGGER is the original's PRIMARY click,
// the GRIP its SECONDARY. With the original's default button swap the primary
// is the right mouse button and EXAMINES, the secondary the left and USES
// (docs/GAME_core.md 4.2-4.3: E/game_core.c:747, :651-699; E/input.c:848-873).
//
//   what the hand does                         what runs                 the original
//   ------------------------------------------ ------------------------- ------------------------------
//   points (the index straight, PoseRule's;    ClassifyPick, the beam    classify_cursor_target_object
//     the other fingers may be curled)
//   TRIGGER pulled, the palm on the thing      ExaminePick               examine_object_under_cursor
//     (in reach: NearAct, GrabNear's search)                               (the primary click)
//   TRIGGER pulled, pointing at it             ExaminePick               the same, at range
//     (what the finger pointed at when the
//     index left it)
//   GRIP squeezed, the palm on it (in reach)   Activate(Use)             activate_targeted_object (the
//                                                                          secondary click); an item goes
//                                                                          through give_item_by_dbase_id
//   GRIP squeezed while pointing at it         Activate(Use)             the same, at range (depth < 600,
//     (nothing in reach)                                                   planar 268)
//   presses it with the fingertip (the point   Activate(Use)             the same
//     pose with the grip held, as before)
//
// IN REACH OR POINTING: as the picker always chose -- what the palm is on first
// (NearAct: the objects within 15 cm of the palm, nearest first; a surface 20 cm
// along the hand; the wall or door the palm is on), else along the pointing
// finger. A button acts once, on its press edge (pressed at 0.70, let go at 0.35:
// the grip's hysteresis, for both); holding it does nothing more and letting go
// does nothing. The hand that holds a WEAPON (HandHoldsWeapon: the weapon drawn
// in the main hand) fires on its trigger, as before -- the binding's +attack,
// which nothing here touches -- so its trigger examines nothing; its grip uses.
//
// A USE CARRIES THE ITEM OF THE HAND THAT MAKES IT (roth_handuse.h, DoUse).
// The original has no item-on-object verb: its click runs the object's chain,
// and the chain's RAW 0x27 tests the right-hand item. So the hand on the RIGHT
// side, which holds the original's right-hand item, uses with it selected;
// the other hand uses with nothing selected, as the original does when no item
// is. The grip of the hand holding the Matchbox on the candle lights it; the
// empty hand gets the original's auto-select (the Matchbox goes to the right
// hand and nothing else happens).
//
// Every action is gated by the original's own click rules on that
// classification (run_gameplay_frame, roth_runtime.h CursorClass): an examine
// needs a cursor of type 3 or 5 with something to examine, a use a cursor of
// type 3 within the use range. Where the original shows no cursor, the hand
// shows no marker.
//
// NOTHING HERE SUPPRESSES OR EMITS A KEY. The trigger and the grip keep their
// bindings (the shared doomxr.ini: the main trigger is +attack, the main grip
// the shift layer); the picker reads the same analogue values the hands' poses
// read (vr_handpose.cpp). So on a hand that holds no weapon a pull still sends
// its key as well (the main trigger: +attack, with Adam's fists) -- the owner's
// to decide; this file only adds the examine.
//
// EXAMINE BY DWELL (0.8 s on a target, a closing ring) is gone: the trigger
// replaced it (owner, 2026-11-13). So is the index tap: the grip uses.
//
// THE RAY. From the index fingertip (the FBX's INDEX_UP_TOP_joint) along the
// finger (INDEX_BASE_joint -> tip), both as last drawn by the posed hand
// (VRHandPose_FingerPoints) and placed through the hand actor's own draw
// transform (Model_ActorPointToWorld), so the beam leaves the finger the player
// sees. With no hand model (no hand actor this tic) it falls back to the
// controller's aim pose, which is what roth_pick and the panels use.
//
// THE DRAWING is billboards, as every Realms panel is: twelve small dots along
// the ray and a ring marker at the hit, tinted by the original's cursor family --
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
// ON THE INVENTORY SCREEN (and the examine popup over it) the hands do not
// stand down: each is the original's MOUSE on the panel (roth_invhand.h for
// the gestures, roth_panelmap.h for where the panel's pixels are, and the
// click through the same code as every pointer's, roth_invmouse.h):
//   points at the panel        the mouse is there: the laser, a dot where it
//                              meets the picture; the cursor cell follows it
//   trigger pulled             a PRIMARY click where the laser was when the
//                              index left it
//   grip squeezed (pointing)   a SECONDARY click at the laser
//   fingertip touch            a PRIMARY click where it touched (on backing off)
//   grip while touching        a SECONDARY click where it touched
// Either hand; which hand changes nothing (the original has one mouse). What
// a click does -- examine, take onto the cursor, use on Adam, combine, swap,
// into a hand box, a tab, an arrow, outside = commit and close -- is the
// original's dispatch at that point. A menu box or the dialogue still owns
// the pointer alone (the hands stand down for those).
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
#include "roth_inventory.h"
#include "roth_handuse.h"
#include "roth_invhand.h"
#include "roth_panelmap.h"
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
#include <utility>
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
constexpr double kTouchRadius = 0.10;    // a palm this close to a wall or a door is on it
constexpr double kGrabRadius  = 0.15;    // palm to an object's box
constexpr double kNoHitBeam   = 0.60;    // the beam's length with nothing at its end
constexpr float  kPointGrip   = 0.50f;   // the fingertip press's point pose holds the grip this far
constexpr float  kGripClose   = 0.70f, kGripOpen = 0.35f;   // a button pressed / let go: the grip AND the trigger
static_assert(kGripClose == invhand::kPressOn && kGripOpen == invhand::kPressOff, "the panel's buttons are the world's");
constexpr int    kDots        = 12;
constexpr double kDotM = 0.004, kMarkerM = 0.014;

struct HandState
{
	// inputs
	bool  valid = false;
	float trigger = 0.f, grip = 0.f;
	bool  triggerTouch = false, thumbTouch = false;
	bool  pointing = false, wasPointing = false;      // the index straight (PoseRule)
	bool  pressReady = false, wasPressReady = false;  // the fingertip press's point pose (grip held, index off)
	bool  gripClosed = false, trigDown = false;       // the two buttons, with their hysteresis
	bool  primed = false;        // the buttons were read last tic: a button already down is not a press

	// the ray and what is at its end
	bool        haveRay = false;
	DVector3    tip, dir, palm;
	DVector3    handFwd;            // wrist -> middle knuckle: where the hand itself points (the grip's ray)
	bool        fromFinger = false;
	const char *rayWhy = nullptr;   // why there is no ray (a log line)
	const char *loggedRayWhy = nullptr;
	int         loggedInputs = -1;  // the input state last reported
	bool        havePick = false;
	Pick        pick;
	CursorClass cls;

	// gestures
	bool  aimHeld = false;       // the index left its straight position: the aim it had then
	int   aimTics = 0;           // ... this many tics ago (invhand::kAimHoldTics at most)
	bool  aimHave = false;       // ... met something
	DVector3 aimTip, aimDir, aimHit;
	uint16_t aimShape = 0;
	bool  pokeArmed = true;
	double lastPokeDist = 1e9;   // last tic's distance from the tip, and its ray
	DVector3 lastTip, lastDir;
	uint16_t lastShape = 0;
	Pick  hover;            // the last target the hand could act on (haptic tick)
	Pick  logged;           // the last target reported (debug line)
	uint16_t loggedShape = 0;

	// billboards
	int dot[kDots] = {};
	int marker = 0;
	bool shown = false;      // any of them visible (so an idle hand touches none)

	// on a panel (the inventory screen, the popup): the original's mouse (roth_invhand.h)
	InvHand panel;
	InvHandOutput pout;
	std::vector<int> line;   // the laser on a panel: billboards laid end to end (DrawPanelLaser)
	int lineShown = 0;
	bool panelWasOnPicture = false;
	bool panelWasContact = false, panelWasTap = false, panelWasPointing = false;
	int loggedPx = 0x7fffffff, loggedPy = 0x7fffffff;
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

	// the panel the hands work (InterfaceHandPanel), and which hand is its mouse
	int panelWhich = HANDPANEL_NONE;
	int pointerRole = -1;
	uint16_t loggedShape = 0;

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
	for (int i = 0; i < h.lineShown && i < (int)h.line.size(); i++) HideBB(h.line[i]);
	h.lineShown = 0;
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
		DVector3 tip, base, palm, wrist, middle;
		if (Model_ActorPointToWorld(ha, pts.tip, tip) && Model_ActorPointToWorld(ha, pts.indexBase, base)
			&& Model_ActorPointToWorld(ha, pts.palm, palm) && Model_ActorPointToWorld(ha, pts.wrist, wrist)
			&& Model_ActorPointToWorld(ha, pts.middleBase, middle))
		{
			DVector3 d = tip - base;
			const double len = d.Length();
			DVector3 f = middle - wrist;
			const double flen = f.Length();
			if (len > 1e-3 && flen > 1e-3)
			{
				h.tip = tip; h.dir = d / len; h.palm = palm;
				h.handFwd = f / flen;
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
		h.handFwd = h.dir;
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

// The right-hand item's id (sign-extended, as the hands read it), 0 for none.
int RightHandItem(const Inventory *inv)
{
	if (inv == nullptr || inv->RightHand() < 0) return 0;
	return (int16_t)inv->Slot(inv->RightHand()).id;
}

// EVERY USE A HAND MAKES -- the grip (in reach or pointing), the fingertip
// press -- is the original's secondary (use) click, through the port's use path (Activate), WITH
// THE ITEM THAT HAND HOLDS (roth_handuse.h): the hand on the RIGHT side holds
// the original's right-hand item and uses the object with it selected; the
// other hand holds none and uses it as the original does with nothing
// selected. The item test in the object's chain (RAW 0x27) decides the rest,
// unchanged.
bool DoUse(int role, const Pick &p, const CursorClass &c, AActor *pmo, const char *how)
{
	if (!c.CanUse()) return false;
	const bool itemHand = VRHandPose_SideForRole(role) == VRHandPose::SIDE_RIGHT;
	Inventory *inv = InterfaceInventory();
	const int before = RightHandItem(inv);
	HandUse scope;
	if (inv != nullptr) scope = HandUseBegin(*inv, itemHand);
	const bool acted = Activate(p, pmo, TrigEvent::Use);
	const int during = RightHandItem(inv);      // what the use left in the right hand
	const HandUseAfter after = inv != nullptr ? HandUseEnd(*inv, scope) : HandUseAfter::ItemHand;
	const int now = RightHandItem(inv);
	Printf("[roth_vrpick] %s hand (%s) %s: USE %s at depth %.0f (cursor type %d shape 0x%x) %s -> %s\n", RoleName(role),
		itemHand ? "right side" : "left side", how, PickKindName(p), c.depth, (int)c.type, (unsigned)c.shape,
		itemHand ? (before ? FStringf("with the right-hand item %d", before).GetChars() : "with no right-hand item")
		         : (before ? FStringf("with no item (the right hand's %d set aside, as the original with nothing selected)", before).GetChars()
		                   : "with no item"),
		acted ? "acted" : "nothing happened");
	if (during != before || now != before || after != HandUseAfter::ItemHand)
		Printf("[roth_vrpick]   the right hand: %d before, %d after the use, %d now (%s)\n", before, during, now, HandUseAfterName(after));
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

// IN REACH: what the palm is on -- for the grip's use, or the trigger's examine
// (`use` false). The Realms objects within reach of the palm first -- an item on
// a table is small and a ray misses it easily -- nearest first, the first the
// original's cursor would let that click act on (so a table the palm rests on
// does not hide the key lying on it); then a short ray along the hand, for a
// lever or a switch face; then the wall face or door leaf the palm is on. The
// grip's choice is the one GrabNear always made (the same search, gated by
// CursorClass::CanUse); the trigger's is the same search gated by CanExamine.
// Returns 0 when nothing in reach could take the click (the caller then looks
// along the pointing finger), 1 when it went to something in reach.
int NearAct(int role, HandState &h, AActor *pmo, bool use)
{
	auto can = [use](const CursorClass &c) { return use ? c.CanUse() : c.CanExamine(); };
	auto why = [use](const CursorClass &c) { return use ? WhyNotUse(c) : WhyNotExamine(c); };
	const char *what = use ? "grip closes" : "trigger pulled";
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
		Printf("[roth_vrpick] %s hand %s: palm at (%.1f %.1f %.1f), %d object(s) within %.1f units (%.2f m)\n",
			RoleName(role), what, h.palm.X, h.palm.Y, h.palm.Z, (int)near.size(), r, kGrabRadius);
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
				n.first, (int)c.type, (unsigned)c.shape, why(c).GetChars());
		if (!can(c)) continue;
		objPick = p; objCls = c; haveObj = true;
		break;
	}
	// THE SURFACE ALONG THE HAND -- along the HAND's own axis, wrist to
	// knuckles (h.handFwd). Not the finger ray: in a fist the index is curled
	// back into the palm, so its tip-to-knuckle line points down or back, and
	// under the flat test the finger ray is turned toward the test's aim from
	// that curled tip. (Owner's PC, 25b13e97: the palm 4.5 units from a door
	// leaf, the ray along the finger met nothing.) From a little BEHIND the
	// palm, because a palm laid flat on a door is level with it, or a hair
	// through it: the ray starts kGrabBack behind and hits count from there to
	// kGrabReach in front.
	const double back = kGrabBack * upm;
	const DVector3 rayFrom = h.palm - h.handFwd * back;
	Pick rayPick;
	CursorClass rayCls;
	bool haveRay = false;
	if (PickFrom(pmo, rayFrom, h.handFwd, back + kGrabReach * upm, rayPick))
	{
		rayPick.dist -= back;   // from the palm; negative: just behind it
		rayCls = ClassifyPick(rayPick, pmo, DepthOf(pmo, rayPick, h.handFwd));
		haveRay = can(rayCls);
		if (Logging())
			Printf("[roth_vrpick]   surface along the hand, from (%.1f %.1f %.1f) along (%.2f %.2f %.2f): %s %.1f units from the palm:"
				" cursor type %d shape 0x%x -> %s\n", rayFrom.X, rayFrom.Y, rayFrom.Z, h.handFwd.X, h.handFwd.Y, h.handFwd.Z,
				PickKindName(rayPick), rayPick.dist, (int)rayCls.type, (unsigned)rayCls.shape, why(rayCls).GetChars());
	}
	else if (Logging())
		Printf("[roth_vrpick]   surface along the hand, from (%.1f %.1f %.1f) along (%.2f %.2f %.2f): nothing from %.1f behind the palm"
			" to %.1f in front\n", rayFrom.X, rayFrom.Y, rayFrom.Z, h.handFwd.X, h.handFwd.Y, h.handFwd.Z, back, kGrabReach * upm);

	// THE SURFACE THE PALM IS ON, whichever way the hand faces: the nearest
	// Realms wall face or door leaf within kTouchRadius of the palm in the
	// map's plane (PickTouchSurface; roth_touchgeom.h).
	Pick touchPick;
	CursorClass touchCls;
	bool haveTouch = false;
	if (PickTouchSurface(pmo, h.palm, kTouchRadius * upm, touchPick))
	{
		touchCls = ClassifyPick(touchPick, pmo, DepthOf(pmo, touchPick, touchPick.hitPos - h.palm));
		haveTouch = can(touchCls);
		if (Logging())
			Printf("[roth_vrpick]   surface under the palm: %s %.1f units from it (at %.1f %.1f): cursor type %d shape 0x%x -> %s\n",
				PickKindName(touchPick), touchPick.dist, touchPick.hitPos.X, touchPick.hitPos.Y, (int)touchCls.type,
				(unsigned)touchCls.shape, why(touchCls).GetChars());
	}
	else if (Logging())
		Printf("[roth_vrpick]   surface under the palm: no Realms wall or door within %.1f units\n", kTouchRadius * upm);
	if (haveTouch && (!haveRay || touchPick.dist < rayPick.dist)) { rayPick = touchPick; rayCls = touchCls; haveRay = true; }

	// THE NEARER OF THE TWO, as the original's pick is the nearest thing under
	// the cursor: a door handle on the door is taken before the door, the door
	// before a key lying a hand's width beyond it.
	if (haveObj && (!haveRay || objPick.dist <= rayPick.dist))
	{
		if (use) DoUse(role, objPick, objCls, pmo, "grip on an object");
		else DoExamine(role, objPick, objCls, pmo, "trigger, the palm on an object");
		return 1;
	}
	if (haveRay)
	{
		if (use) DoUse(role, rayPick, rayCls, pmo, "grip on a surface");
		else DoExamine(role, rayPick, rayCls, pmo, "trigger, the palm on a surface");
		return 1;
	}
	if (Logging())
		Printf("[roth_vrpick] %s hand %s: nothing within reach to %s\n", RoleName(role), what, use ? "use" : "examine");
	return 0;
}

//--------------------------------------------------------------------------
// One hand, one tic
//--------------------------------------------------------------------------

void ResetGestures(HandState &h)
{
	h.aimHeld = false;
	h.aimHave = false;
	h.pokeArmed = true;
	h.hover = Pick{};
}

// The beam: twelve dots from the fingertip to `end`, and the marker there when
// the original's cursor family has one (FamilyColour).
void DrawBeam(int role, HandState &h, AActor *pmo, const DVector3 &end, bool hit, uint16_t shape)
{
	const double upm = UPM();
	h.shown = true;
	const HandLock L = LockFor(role, pmo);
	double markAlpha = 0.0;
	const PalEntry col = hit ? FamilyColour(shape, markAlpha) : PalEntry(255, 200, 200, 200);
	const bool marked = hit && markAlpha > 0.0;
	const double beamAlpha = marked ? 0.30 : 0.12;
	const DVector3 span = end - h.tip;
	const double len = span.Length();
	const DVector3 dir = len > 1e-6 ? span / len : h.dir;
	for (int i = 0; i < kDots; i++)
		PlaceBB(h.dot[i], BB_PANEL, L, h.tip + dir * (len * (i + 0.5) / kDots), kDotM * upm, marked ? col : PalEntry(255, 200, 200, 200), beamAlpha);
	if (marked)
		// Pulled back a little toward the hand so the surface does not cut it.
		PlaceBB(h.marker, BB_RING, L, h.tip + dir * std::max(0.0, len - 0.006 * upm), kMarkerM * upm, col, markAlpha);
	else HideBB(h.marker);
	for (int i = 0; i < h.lineShown && i < (int)h.line.size(); i++) HideBB(h.line[i]);
	h.lineShown = 0;
}

// THE OWNER'S BUTTONS (2026-11-13): the TRIGGER is the original's PRIMARY click
// (examine), the GRIP its SECONDARY (use). See the header.
void TickHand(int role, AActor *pmo)
{
	HandState &h = P.hand[role];
	ReadInputs(role, h);
	h.wasPointing = h.pointing;
	h.wasPressReady = h.pressReady;
	// POINTING: the index straight, as the hand's pose shows it (PoseRule: off the
	// trigger, not touching it); the other fingers may be curled by the grip.
	h.pointing = h.valid && InvHandIndexStraight(h.triggerTouch, h.trigger);
	// The fingertip press keeps its own readiness, unchanged: the point pose with
	// the grip held (>= 0.5) and the index off the trigger.
	h.pressReady = h.valid && h.grip >= kPointGrip && !h.triggerTouch;
	const bool gripWasClosed = h.gripClosed;
	if (!h.valid) h.gripClosed = false;
	else if (h.grip >= kGripClose) h.gripClosed = true;
	else if (h.grip <= kGripOpen) h.gripClosed = false;
	const bool trigWasDown = h.trigDown;
	if (!h.valid) h.trigDown = false;
	else if (h.trigger >= kGripClose) h.trigDown = true;
	else if (h.trigger <= kGripOpen) h.trigDown = false;
	// A button already down when the hand comes back (a controller found, the
	// hands back from a menu or a panel) is not a press.
	const bool gripEdge = h.primed && h.gripClosed && !gripWasClosed;
	const bool trigEdge = h.primed && h.trigDown && !trigWasDown;
	h.primed = h.valid;

	if (Logging())
	{
		const int state = (h.valid ? 1 : 0) | (h.pointing ? 2 : 0) | (h.gripClosed ? 4 : 0) | (h.triggerTouch ? 8 : 0) | (h.thumbTouch ? 16 : 0)
			| (h.trigDown ? 32 : 0);
		if (state != h.loggedInputs)
		{
			h.loggedInputs = state;
			Printf("[roth_vrpick] %s hand inputs: %s grip %.2f trigger %.2f, trigger touch %d, thumb touch %d -> %s%s%s\n", RoleName(role),
				h.valid ? "" : "NO CONTROLLER,", h.grip, h.trigger, (int)h.triggerTouch, (int)h.thumbTouch,
				h.pointing ? "pointing" : "not pointing", h.gripClosed ? ", grip closed" : "", h.trigDown ? ", trigger pressed" : "");
		}
	}

	// A HAND THAT IS DOING NOTHING COSTS NOTHING: no fingertip, no trace, no
	// classification, no billboards -- only the inputs above. Work is done while
	// pointing, while the aim is held (the index down after pointing), for the
	// fingertip press, and on the tic a button is pressed.
	const bool busy = h.valid && (h.pointing || h.wasPointing || h.pressReady || h.aimHeld || gripEdge || trigEdge);
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

	// THE PICK while the finger is out (the beam, the grip's far use, the press).
	const Pick before = h.havePick ? h.pick : Pick{};
	const bool hadPick = h.havePick;
	if (h.pointing || h.pressReady)
	{
		h.havePick = PickFrom(pmo, h.tip, h.dir, PickTraceRange(h.dir), h.pick);
		h.cls = h.havePick ? ClassifyPick(h.pick, pmo, DepthOf(pmo, h.pick, h.dir)) : CursorClass{};
	}
	else
	{
		h.havePick = false;
		h.cls = CursorClass{};
	}

	// THE AIM HELD: the index leaving its straight position (onto the trigger, or
	// the trigger moving) swings the finger's own ray, so a pull acts on what the
	// finger pointed at when it left -- the ray it had then, traced again at the
	// pull -- and the beam stays drawn to it while the index is down, for a
	// second at most (invhand::kAimHoldTics, as on the panels): a hand at rest
	// has its index on the trigger, and an old aim is no aim.
	if (h.aimHeld && ++h.aimTics > invhand::kAimHoldTics) { h.aimHeld = false; h.aimHave = false; }
	if (h.wasPointing && !h.pointing)
	{
		h.aimHeld = true;
		h.aimTics = 0;
		h.aimHave = hadPick;
		h.aimTip = h.lastTip;
		h.aimDir = h.lastDir;
		h.aimHit = before.hitPos;
		h.aimShape = h.lastShape;
	}
	if (h.pointing) h.aimHeld = false;

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

	// ---- THE GRIP (the original's secondary: use) ----
	// In reach first (the palm on it: NearAct, GrabNear's search), else along the
	// pointing finger at range, through the same use path and the same gate.
	// One action per press: a grip that went somewhere is not also the
	// fingertip press below (re-armed when the tip backs off).
	bool gripActed = false;
	if (gripEdge)
	{
		gripActed = NearAct(role, h, pmo, true) != 0;
		if (!gripActed && h.pointing && h.havePick && h.cls.CanUse())
		{
			DoUse(role, h.pick, h.cls, pmo, "grip while pointing");
			gripActed = true;
		}
		else if (!gripActed && Logging())
			Printf("[roth_vrpick] %s hand grip: %s\n", RoleName(role), !h.pointing ? "not pointing, nothing in reach"
				: h.havePick ? WhyNotUse(h.cls).GetChars() : "nothing on the pointing finger's ray");
		if (gripActed) h.pokeArmed = false;
	}

	// ---- THE TRIGGER (the original's primary: examine) ----
	// Not on the hand holding a weapon: its trigger fires it (the binding's
	// +attack, WeaponTick), as before. In reach first, else what the finger
	// pointed at when the index left it.
	if (trigEdge)
	{
		if (HandHoldsWeapon(role))
		{
			if (Logging()) Printf("[roth_vrpick] %s hand trigger: the hand holds a weapon -- it fires, nothing to examine\n", RoleName(role));
		}
		else if (NearAct(role, h, pmo, false) == 0)
		{
			Pick p;
			bool have = false;
			DVector3 along = h.dir;
			if (h.aimHeld && h.aimHave) { have = PickFrom(pmo, h.aimTip, h.aimDir, PickTraceRange(h.aimDir), p); along = h.aimDir; }
			else if (h.pointing && h.havePick) { p = h.pick; have = true; }
			if (have)
			{
				const CursorClass c = ClassifyPick(p, pmo, DepthOf(pmo, p, along));
				if (c.CanExamine()) DoExamine(role, p, c, pmo, "trigger");
				else if (Logging()) Printf("[roth_vrpick] %s hand trigger on %s: %s\n", RoleName(role), PickKindName(p), WhyNotExamine(c).GetChars());
			}
			else if (Logging())
				Printf("[roth_vrpick] %s hand trigger: nothing in reach and nothing the finger pointed at\n", RoleName(role));
		}
	}

	// ---- FINGERTIP PRESS (near use), as before ----
	// A quick press can cross the 2.5 cm window between two tics and leave the
	// tip already through the surface, where the trace no longer meets it. So
	// a surface the tip was close to last tic and has now reached or passed
	// (along last tic's ray) counts as pressed too. Not for an object: its
	// actor may be gone by now, and a pointer to it is not followed.
	if (h.pressReady)
	{
		if (h.havePick && h.pick.dist <= kPokeIn * upm)
		{
			if (h.pokeArmed)
			{
				h.pokeArmed = false;
				DoUse(role, h.pick, h.cls, pmo, "fingertip press");
			}
		}
		else if (h.pokeArmed && h.wasPressReady && hadPick && before.kind != Pick::Prop && h.lastPokeDist <= kPokeOut * upm
			&& ((before.hitPos - h.tip) | h.lastDir) <= kPokeIn * upm)
		{
			h.pokeArmed = false;
			const CursorClass c = ClassifyPick(before, pmo, DepthOf(pmo, before, h.lastDir));
			DoUse(role, before, c, pmo, "fingertip press (through)");
		}
		else if (!h.havePick || h.pick.dist > kPokeOut * upm) h.pokeArmed = true;
		h.lastPokeDist = h.havePick ? h.pick.dist : 1e9;
	}
	else
	{
		h.pokeArmed = true;
		h.lastPokeDist = 1e9;
	}
	h.lastTip = h.tip;
	h.lastDir = h.dir;
	h.lastShape = h.havePick ? h.cls.shape : 0;

	// ---- HAPTIC TICK on coming onto something actionable ----
	const bool actionable = h.pointing && h.havePick && (h.cls.CanUse() || h.cls.CanExamine());
	if (actionable && !SamePickTarget(h.pick, h.hover)) { h.hover = h.pick; Haptic(role, 0.12, 12); }
	else if (!actionable) h.hover = Pick{};

	// ---- THE BEAM ----
	if (h.pointing || h.pressReady)
		DrawBeam(role, h, pmo, h.havePick ? h.pick.hitPos : h.tip + h.dir * (kNoHitBeam * upm), h.havePick, h.havePick ? h.cls.shape : 0);
	else if (h.aimHeld && h.aimHave)
		DrawBeam(role, h, pmo, h.aimHit, true, h.aimShape);
	else HideHand(h);
}

//--------------------------------------------------------------------------
// On a panel: each hand is the original's mouse (see the header)
//--------------------------------------------------------------------------

PanelV3 PV(const DVector3 &v) { return PanelV3(v.X, v.Y, v.Z); }
DVector3 DV(const PanelV3 &v) { return DVector3(v.x, v.y, v.z); }

const char *PanelName(int which) { return which == HANDPANEL_POPUP ? "examine popup" : "inventory"; }

void ForgetPanel()
{
	for (HandState &h : P.hand)
	{
		h.panel.Reset();
		h.pout = InvHandOutput();
		h.panelWasOnPicture = h.panelWasContact = h.panelWasTap = h.panelWasPointing = false;
		h.loggedPx = h.loggedPy = 0x7fffffff;
	}
	P.pointerRole = -1;
	P.loggedShape = 0;
}

// THE LASER ON A PANEL: a line from the fingertip to where it meets the panel,
// and a dot there. Sized by the ANGLE it covers from the eye, so it reads the
// same in the headset and in flat however far each piece is: the line 0.35
// degrees wide (about 2 pixels of a 640-wide flat view at its 91.8 degrees,
// about 7 of a headset's at 20 per degree), the dot 1.2 degrees (6.5 / 24). The line is camera-facing squares of its own width laid edge to
// edge (each placed hand-locked like every picker billboard, so in the headset
// it stays on the controller at the display's rate). A hair of the dot's size
// toward the eye, so the panel does not cut it. The world picker's beam
// (TickHand) is a different thing and keeps its own subtle look.
constexpr double kPanelLineDeg = 0.35, kPanelDotDeg = 1.2;
constexpr int    kPanelLineMax = 320;

void DrawPanelLaser(int role, HandState &h, AActor *pmo, const PanelPlane &plane, const DVector3 &eye)
{
	const InvHandOutput &o = h.pout;
	h.shown = true;
	for (int &d : h.dot) HideBB(d);
	const HandLock L = LockFor(role, pmo);
	const double lineK = tan(kPanelLineDeg * (M_PI / 180.0)), dotK = tan(kPanelDotDeg * (M_PI / 180.0));
	const PalEntry col(255, 250, 238, 200);
	const DVector3 end = DV(o.beamEnd);
	const DVector3 span = end - h.tip;
	const double len = span.Length();
	const double lineAlpha = o.dot ? 0.95 : 0.55;
	int n = 0;
	if (len > 1e-6)
	{
		const DVector3 dir = span / len;
		// Where each square goes, then the billboards (made first, so the array
		// does not move under the pointers), then one pass to set them.
		std::vector<std::pair<DVector3, double>> at;
		for (double t = 0; t < len && n < kPanelLineMax; n++)
		{
			const DVector3 p = h.tip + dir * t;
			const double size = std::max(1e-3, (p - eye).Length() * lineK);
			at.push_back({ p + dir * (size * 0.5 < len - t ? size * 0.5 : 0.0), size });
			t += size;
		}
		n = (int)at.size();
		if ((int)h.line.size() < n) h.line.resize(n, 0);
		for (int i = 0; i < n; i++) EnsureBB(h.line[i], BB_PANEL);
		for (FBillboard &b : P.level->Billboards)
		{
			for (int i = 0; i < n; i++)
			{
				if (b.id != h.line[i]) continue;
				b.flags = (b.flags & ~BBFL_HANDANY) | (L.on ? L.flag : 0);
				b.pos = L.on ? L.Local(at[i].first) : at[i].first;
				b.width = b.height = at[i].second;
				b.color = col;
				b.alpha = lineAlpha;
				break;
			}
		}
	}
	for (int i = n; i < h.lineShown && i < (int)h.line.size(); i++) HideBB(h.line[i]);
	h.lineShown = n;
	if (o.dot)
	{
		const DVector3 dp = DV(o.dotAt);
		const double size = std::max(1e-3, (dp - eye).Length() * dotK);
		PlaceBB(h.marker, BB_PANEL, L, dp + DV(plane.front) * size, size, col, 1.0);
	}
	else HideBB(h.marker);
}

void TickPanelHand(int role, AActor *pmo, const PanelPlane &plane, const DVector3 &eye)
{
	HandState &h = P.hand[role];
	ReadInputs(role, h);
	InvHandInput in;
	in.valid = h.valid;
	in.trigger = h.trigger;
	in.grip = h.grip;
	in.triggerTouch = h.triggerTouch;
	// The finger is only looked at while it can be the mouse: pointing (the index
	// straight), the index down with the point held, or the fingertip on the panel.
	const bool wants = h.valid && (InvHandIndexStraight(h.triggerTouch, h.trigger) || h.panel.IndexDown() || h.panel.InContact());
	if (wants && FingerRay(role, h, pmo))
	{
		in.haveRay = true;
		in.fromFinger = h.fromFinger;
		in.tip = PV(h.tip);
		in.dir = PV(h.dir);
	}
	h.pout = h.panel.Tick(plane, in, UPM());
	h.pointing = h.panel.Pointing();
	h.primed = false;            // back in the world, the buttons are read afresh

	if (Logging())
	{
		if (h.pointing != h.panelWasPointing)
			Printf("[roth_vrpick] %s hand %s the panel%s\n", RoleName(role), h.pointing ? "points at" : "stops pointing at",
				h.pointing && !h.fromFinger ? " (from the aim pose: no fingertip, no touch)" : "");
		if (h.panel.IndexDown() && !h.panelWasTap)
			Printf("[roth_vrpick] %s hand: the index leaves the laser, the mouse held at %d,%d for the trigger\n", RoleName(role), h.pout.px, h.pout.py);
		if (h.panel.InContact() && !h.panelWasContact)
			Printf("[roth_vrpick] %s hand: fingertip on the panel at %d,%d (%.1f units in front)\n", RoleName(role), h.pout.px, h.pout.py, h.pout.tipDist);
		if (!h.panel.InContact() && h.panelWasContact && !h.pout.click)
			Printf("[roth_vrpick] %s hand: fingertip off the panel with no click (%.1f units in front)\n", RoleName(role), h.pout.tipDist);
	}
	h.panelWasPointing = h.pointing;
	h.panelWasTap = h.panel.IndexDown();
	h.panelWasContact = h.panel.InContact();

	// ---- the laser and the dot ----
	if (!h.pout.beam) { HideHand(h); return; }
	DrawPanelLaser(role, h, pmo, plane, eye);
}

bool OnPicture(const PanelPlane &p, int x, int y)
{
	return x >= p.left && x < p.left + p.w && y >= p.top && y < p.top + p.h;
}

// The hands after both have ticked: which of them is the mouse (the hover),
// and their clicks.
void PanelPointerAndClicks(int which, const PanelPlane &plane)
{
	// THE MOUSE: the hand with its index down or its fingertip on the panel; else the hand that
	// has just come onto the picture; else the one that was; else any hand
	// whose laser meets the panel.
	int owner = -1;
	for (int r = 0; r < 2; r++) if (P.hand[r].pout.active && (owner < 0 || r == P.pointerRole)) owner = r;
	if (owner < 0)
		for (int r = 0; r < 2; r++)
			if (P.hand[r].pout.onPicture && !P.hand[r].panelWasOnPicture) owner = r;
	if (owner < 0 && P.pointerRole >= 0 && P.hand[P.pointerRole].pout.pointer) owner = P.pointerRole;
	if (owner < 0) for (int r = 0; r < 2 && owner < 0; r++) if (P.hand[r].pout.pointer) owner = r;
	if (owner >= 0)
	{
		HandState &h = P.hand[owner];
		const uint16_t shape = InterfaceHandPointer(which, h.pout.px, h.pout.py, h.pout.onPicture);
		if (Logging() && (owner != P.pointerRole || shape != P.loggedShape || (h.pout.px >> 2) != (h.loggedPx >> 2) || (h.pout.py >> 2) != (h.loggedPy >> 2)))
		{
			Printf("[roth_vrpick] %s hand is the mouse on the %s at %d,%d%s (the original's cursor shape 0x%x)\n", RoleName(owner),
				PanelName(which), h.pout.px, h.pout.py, h.pout.onPicture ? "" : ", off its picture", (unsigned)shape);
			h.loggedPx = h.pout.px; h.loggedPy = h.pout.py;
			P.loggedShape = shape;
		}
	}
	P.pointerRole = owner;

	// THE CLICKS: each where its hand made it, the mouse moved there first.
	for (int r = 0; r < 2; r++)
	{
		HandState &h = P.hand[r];
		const InvHandOutput &o = h.pout;
		if (!o.click) continue;
		const bool on = OnPicture(plane, o.cx, o.cy);
		if (r != P.pointerRole) { InterfaceHandPointer(which, o.cx, o.cy, on); P.pointerRole = r; }
		Inventory *inv = InterfaceInventory();
		const int rh0 = inv ? inv->RightHand() : -1, lh0 = inv ? inv->LeftHand() : -1, held0 = inv ? inv->Held() : -1;
		const int act = InterfaceHandClick(which, o.cx, o.cy, on, o.click);
		const int rh1 = inv ? inv->RightHand() : -1, lh1 = inv ? inv->LeftHand() : -1, held1 = inv ? inv->Held() : -1;
		FString what = act >= 0 ? FStringf("action 0x%x", act) : FString("the popup's click");
		Printf("[roth_vrpick] %s hand (%s side) %s on the %s at %d,%d%s: %s click -> %s; right hand slot %d -> %d, left hand slot %d -> %d,"
			" held slot %d -> %d\n", RoleName(r), VRHandPose_SideForRole(r) == VRHandPose::SIDE_RIGHT ? "right" : "left",
			InvHandHowName(o.how), PanelName(which), o.cx, o.cy, on ? "" : " (off its picture)",
			o.click == 1 ? "PRIMARY" : "SECONDARY", what.GetChars(), rh0, rh1, lh0, lh1, held0, held1);
		Haptic(r, 0.45, 35);
	}
	for (HandState &h : P.hand) h.panelWasOnPicture = h.pout.onPicture;
	InterfaceHandsDrivePointer(P.pointerRole >= 0);
}

void ForgetLevel()
{
	P.level = nullptr;
	for (HandState &h : P.hand)
	{
		for (int &d : h.dot) d = 0;
		h.line.clear();
		h.lineShown = 0;
		h.marker = 0;
		h.shown = false;
		h.havePick = false;
		h.primed = false;
		ResetGestures(h);
	}
	ForgetPanel();
	P.panelWhich = HANDPANEL_NONE;
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
	// ON A PANEL (the inventory screen, the popup over it): the hands are its mouse.
	PanelPlane plane;
	int panel = HANDPANEL_NONE;
	if (!modal && on && player->health > 0 && owner != nullptr)
	{
		const DVector3 eye = pmo->Pos() + DVector3(0, 0, player->viewheight);
		panel = InterfaceHandPanel(PV(eye), plane);
	}
	if (Logging())
	{
		// WHY THE HANDS STAND DOWN, on each change: a menu or the death wait,
		// a dead player, or a panel that owns the pointer (InterfacePointerOwner).
		// On the inventory or the popup they do not: they are its mouse (reported below).
		const char *why = modal ? "a menu or the death wait" : !on ? "the picker is off"
			: player->health <= 0 ? "the player is dead" : panel != HANDPANEL_NONE ? nullptr : owner;
		if (why != P.loggedStand)
		{
			if (why != nullptr) Printf("[roth_vrpick] the hands stand down: %s\n", why);
			else Printf("[roth_vrpick] the hands are live\n");
			P.loggedStand = why;
		}
	}
	const uint64_t t0 = I_nsTime();

	if (panel != P.panelWhich)
	{
		ForgetPanel();
		for (HandState &h : P.hand) { h.havePick = false; ResetGestures(h); HideHand(h); }
		if (Logging())
		{
			if (panel != HANDPANEL_NONE) Printf("[roth_vrpick] the hands work the %s (each is the original's mouse on it)\n", PanelName(panel));
			else if (P.panelWhich != HANDPANEL_NONE) Printf("[roth_vrpick] the hands leave the %s\n", PanelName(P.panelWhich));
		}
		P.panelWhich = panel;
	}
	if (panel != HANDPANEL_NONE)
	{
		const DVector3 eye = pmo->Pos() + DVector3(0, 0, player->viewheight);
		for (int role = 0; role < 2; role++) TickPanelHand(role, pmo, plane, eye);
		PanelPointerAndClicks(panel, plane);
	}
	else InterfaceHandsDrivePointer(false);

	for (int role = 0; role < 2 && panel == HANDPANEL_NONE; role++)
	{
		if (stand)
		{
			HandState &h = P.hand[role];
			h.havePick = false;
			h.pointing = false;
			h.primed = false;
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
	if (stand && panel == HANDPANEL_NONE) return;
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
//   roth_vrpicktest seek item|door|face|examine [metres]
//                                        stand the player so the TEST HAND is <metres> (default 1.0)
//                                        in front of the nearest pickable item / door leaf / wall face
//                                        with a use trigger, facing it, and aim the test ray at it.
//                                        The test hand is the main hand, or the off hand when only it is on.
//   roth_vrpicktest seek needs <item> [metres]
//                                        the same, at the nearest object or wall face whose use chain
//                                        tests DBASE100 item <item> with RAW 0x27 (the item-on-the-world
//                                        case: a candle and the Matchbox, 16, in STUDY1)
//                                        In metres, because the hand is placed in metres
//                                        (VR_UnitsPerMeter: 34 in flat, about 107 in the headset)
//   roth_vrpicktest panel <x> <y> [metres]
//                                        with the inventory (or the examine popup) up: aim the test ray
//                                        at the centre of pixel (x, y) of its picture (the original's
//                                        panel pixels: the inventory's are from its top left, 0..287 x
//                                        0..79). With <metres>, also move the player across the floor
//                                        so the TEST HAND's fingertip is <metres> in front of the
//                                        panel at that pixel -- 0.10 hovers, -0.01 is through it, a
//                                        touch -- raising or lowering the drawn test hand to the pixel's
//                                        row (VRHandTest_SetLift, in the test grip's own units).
//   roth_vrpicktest off
//
// The buttons are the real ones, driven by the hand test's inputs
// (roth_handtest <grip> <trigger> <triggertouch> <thumbtouch>): pointing with the
// grip open is "roth_handtest 0 0 0 1"; the trigger pulled from there is
// "roth_handtest 0 1 1 1"; the grip squeezed while pointing is
// "roth_handtest 1 0 0 1" (= "point"); a grip in reach with the index resting on
// the trigger, not pulling it, is "open" then "ready" (grip 1, trigger touched).
//
//===========================================================================

CCMD(roth_vrpicktest)
{
	using namespace roth;
	if (argv.argc() < 2)
	{
		Printf("roth_vrpicktest on [main|off|both] | aim <pitch> [yaw] | seek item|door|face|examine [metres]"
			" | seek needs <item> [metres] | panel <x> <y> [metres] | off\n");
		return;
	}
	if (!stricmp(argv[1], "off"))
	{
		P.test = false;
		P.testAimAt = false;
		VRHandTest_SetTilt(0.f);
		VRHandTest_SetLift(0.0);
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
	if (!stricmp(argv[1], "panel") && argv.argc() >= 4)
	{
		AActor *pmo = players[consoleplayer].mo;
		if (pmo == nullptr || players[consoleplayer].mo == nullptr) { Printf("roth_vrpicktest panel: no player\n"); return; }
		const DVector3 eye = pmo->Pos() + DVector3(0, 0, players[consoleplayer].viewheight);
		PanelPlane plane;
		const int which = InterfaceHandPanel(PV(eye), plane);
		if (which == HANDPANEL_NONE) { Printf("roth_vrpicktest panel: neither the inventory nor the examine popup is up\n"); return; }
		const double x = atof(argv[2]) + 0.5, y = atof(argv[3]) + 0.5;
		const DVector3 at = DV(PanelWorldOf(plane, x, y));
		P.testAimAt = true;
		P.testAim = at;
		Printf("roth_vrpicktest panel: aiming at pixel %s,%s of the %s, world (%.2f %.2f %.2f)\n", argv[2], argv[3], PanelName(which), at.X, at.Y, at.Z);
		if (argv.argc() >= 5)
		{
			const double metres = atof(argv[4]);
			const int testRole = (P.testHands & 1) ? VR_MAINHAND : VR_OFFHAND;
			HandState tmp;
			if (!P.test || !FingerRay(testRole, tmp, pmo)) { Printf("roth_vrpicktest panel: no fingertip yet (roth_vrpicktest on, roth_handtest point first)\n"); return; }
			const DVector3 target = at + DV(plane.front) * (metres * UPM());
			// Across the floor: the player. Up or down: the drawn test hand itself,
			// by its own units (VRHandTest_SetLift, as seek's grip does): the test
			// grip hangs (TEST_UP + lift) of VR_UnitsPerMeter under the eye drawn
			// through the level's pixel stretch (vr_handpose.cpp TestGrip), so a
			// lift of L moves the drawn fingertip L * units per metre / stretch.
			const DVector3 move(target.X - tmp.tip.X, target.Y - tmp.tip.Y, 0.0);
			const double ps = pmo->Level != nullptr && pmo->Level->pixelstretch > 0 ? pmo->Level->pixelstretch : 1.0;
			const double dz = target.Z - tmp.tip.Z;
			const double lift = VRHandTest_Lift() + dz * ps / UPM();
			VRHandTest_SetLift(lift);
			pmo->SetOrigin(pmo->Pos() + move, false);
			pmo->Vel.Zero();
			pmo->ClearInterpolation();
			PanelPoint lands;
			const double d = PanelProject(plane, PV(tmp.tip + move + DVector3(0, 0, dz)), lands);
			Printf("roth_vrpicktest panel: the player moved (%.2f %.2f) and the %s hand lifted %+.2f units (the test hand's lift now %.3f m)"
				" so its fingertip is %.3f m (%.1f units) in front of the panel over pixel %d,%d\n", move.X, move.Y, RoleName(testRole),
				dz, lift, d / UPM(), d, lands.px, lands.py);
		}
		return;
	}
	if (!stricmp(argv[1], "seek") && argv.argc() >= 3)
	{
		AActor *pmo = players[consoleplayer].mo;
		if (pmo == nullptr) { Printf("roth_vrpicktest seek: no player\n"); return; }
		// "needs <item>" takes one more word: the DBASE100 item the target's chain tests.
		const bool needs = !stricmp(argv[2], "needs");
		if (needs && (argv.argc() < 4 || atoi(argv[3]) <= 0)) { Printf("roth_vrpicktest seek needs <item> [metres]\n"); return; }
		const int at = needs ? 4 : 3;
		const double metres = argv.argc() > at ? atof(argv[at]) : 1.0;
		// Where the test hand hangs off the eye: roth_hands.cpp stands it at
		// VRHandTest_ActorSpot, the same placement the test grip uses. THE TEST
		// HAND: the main hand unless only the off hand is on.
		const int testRole = (P.testHands & 1) ? VR_MAINHAND : VR_OFFHAND;
		const int side = VRHandPose_SideForRole(testRole);
		VRHandTest_SetLift(0.0);   // the hand's own place first; a grip on an object sets its lift below
		DVector3 spot0;
		VRHandTest_ActorSpot(side, DVector3(0, 0, 0), 0.0, spot0);   // yaw 0: ahead +x, right -y
		PickTestHand hand;
		hand.ahead = spot0.X;
		hand.right = -spot0.Y;
		hand.up = spot0.Z;
		hand.metresOff = metres * UPM();
		// A grip test wants nothing else within the palm's reach.
		hand.clearRadius = metres < 0.3 ? (kGrabRadius + 0.05) * UPM() : 0.0;
		hand.needsItem = needs ? atoi(argv[3]) : 0;
		hand.upm = UPM();
		// A grip on an OBJECT is checked the way the picker's grip chooses
		// (GrabNear): the palm's reach, nearest first, the first usable.
		hand.grabRadius = metres < 0.3 ? kGrabRadius * UPM() : 0.0;
		DVector3 aim, stand;
		if (!PickTestTarget(argv[2], pmo, hand, aim, stand)) { Printf("roth_vrpicktest seek: no reachable %s on this map\n", argv[2]); return; }
		VRHandTest_SetLift(hand.liftMetres);
		const double yawDeg = atan2(aim.Y - stand.Y, aim.X - stand.X) * (180.0 / M_PI);
		pmo->SetOrigin(stand, false);
		pmo->Vel.Zero();
		pmo->Angles.Yaw = DAngle::fromDeg(yawDeg);
		pmo->Angles.Pitch = nullAngle;
		pmo->ClearInterpolation();
		P.testAimAt = true;
		P.testAim = aim;
		Printf("roth_vrpicktest seek %s%s%s: aiming at (%.0f %.0f %.0f), standing at (%.0f %.0f %.0f): the %s hand (%s side) %.2f m"
			" (%.1f units) from it, lifted %.2f m\n", argv[2], needs ? " " : "", needs ? argv[3] : "", aim.X, aim.Y, aim.Z, stand.X, stand.Y, stand.Z,
			RoleName(testRole), side == VRHandPose::SIDE_RIGHT ? "right" : "left", metres, metres * UPM(), hand.liftMetres);
		return;
	}
	Printf("roth_vrpicktest: unknown '%s'\n", argv[1]);
}
