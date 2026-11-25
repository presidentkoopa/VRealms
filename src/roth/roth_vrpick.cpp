//
// roth_vrpick.cpp -- THE VR HANDS IN THE WORLD: ONE SYSTEM, Godot XR Tools' (owner, 2026-11-24: "Strip it, use XR
// Tools"; "WHY DOES IT MAKE SENSE FOR YOU TO HAVE SIX DIFFERENT INTERACTION THINGS").
//
// THE OWNER'S RULE FOR THIS FILE (2026-11-13): VR INTERACTION -- how the player reaches and grabs -- may be new; GAME
// LOGIC may not. Every action here ends in a call the flat game already makes.
//
// THE OWNER'S BUTTONS (2026-11-13): the TRIGGER is the original's PRIMARY click (examine), the GRIP its SECONDARY
// (use) (docs/GAME_core.md 4.2-4.3).
//
//   what the hand does                                    what runs              the original
//   ----------------------------------------------------- ---------------------- ------------------------------
//   ITEMS: RSQ Distance Grab (the owner's package, wadsrc/static/filter/vrealms, glued by roth_grabglue.cpp): the
//     palm turned out aims its cone, the flick pulls, the open palm catches; the grip on an item at the hand takes it.
//     A catch is the original's pickup from that hand (VRPickTakeCaught -> DoUse).
//   the hand on a usable thing, within 0.3 m in front of  ClassifyPick           classify_cursor_target_object
//     it (THE REACH): GRIP uses it (not an item: those    Activate(Use)          activate_targeted_object
//     are the grab's)
//   TRIGGER on what the hand is on                        ExaminePick            examine_object_under_cursor
//   No laser of VRealms' own: the one laser is Distance Grab's (owner, 2026-11-24).
//
// Reach is the original's (L1, roth_vrrules.h: depth < 600 from the body). Who owns each hand is L2's (L2Tick): the
// Colt's reload parts, a world gesture (a lever, a key in its lock, a door's handle), the fore-end, the Lamp or the
// Book, a held item, a drawn weapon, the lit match, the carried item, else nobody; a closing grip asks them in that
// order, then THE PICKUP, then THE BEAM. A button acts once, on its press edge (0.70 on, 0.35 off). The hand that
// holds a WEAPON fires on its trigger (the binding's +attack); its grip uses.
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
// THE ATTACK (owner, 2026-11-13: "we will probably use offhand for a lot of
// environmental interaction, but we need to design with left and right hand
// support in mind"). The trigger examines on EITHER hand and never sends attack
// unless that hand holds a drawn weapon. The bindings stay (the shared
// doomxr.ini: the main trigger is +attack); with the hands on (a headset, or
// roth_vrpicktest), VRPickTick holds the fire button back -- player->ButtonMask
// for BT_ATTACK and ActorsSetFireButtonAllowed(false) for the ROTH weapon tick --
// while the main hand holds no weapon, so examining never swings Adam's fists. A
// weapon hand's trigger keeps firing. PHYSICAL ONLY (owner, 2026-11-13, Q6): a
// grip on nothing does nothing; Adam's fists attack where a FIST SWUNG AT SPEED
// LANDS (roth_gestures.cpp, RSQ Melee's punch reader): the original's secondary
// click on what it met (roth_handclick.h), its attack aimed at the contact
// (ActorsRequestFireAt). Flat mouse and keyboard are untouched.
// THE GESTURES (roth_gestures.h): the punch, the knock, the physical attempts,
// the ward tingle run once a tic after the hands (GesturesTick), on the inputs
// and points read here (VRPickHandNow), and act through the same use and
// examine paths (VRPickUseAt, VRPickExamineAt). The tall wicks' point + grip
// is the grip's, below (CandlesPointGrip).
// While a dialogue owns the pointer, a hand's press is the dialogue's click
// (InterfaceHandDialogueClick): a click during a playing line skips it, as the
// original's (dialogue_ui.c:552-553).
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
// THE DRAWING in the world is THE GLOW, no beam and no ring (owner in the headset, 2026-11-24: the dotted beam
// "looks like shit", the ring that replaced it "is fucking garbage"; "it isn't about porting one or the other, but what
// you are reading in them is the ideas we can use to make ours"). The ideas, from the owner's proven systems: RSQ
// DistanceGrab's glow twin (glow.zs, after UBQS_GripGlow: a second actor wearing the target's own picture or model in
// one flat colour, a hair larger, a hair toward the eye) and its two strengths (tell.zs: "could" amber, dim, a slow
// breath; "have" green, brighter, twice as fast); REMA's VRGrab keeps the same pair (RAB_Look). Ours, in the engine:
// see THE GLOW below. The thing itself lights up in its own silhouette -- a Realms picture or its 3D model alike.
// Walls, floors and door leaves do not glow yet (no actor to wear); the light tick (haptic) still marks them.
// The panel's laser (the inventory screen) is unchanged.
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
#include "roth_physics.h"          // a let go drops the item; the pickup takes a loose one
#include "roth_img3d.h"            // Img3dWornMetres: a loose item flown in from range
#include "roth_dbase100.h"   // roth_l1diff: the item an object is
#include "roth_raw.h"        // roth_l1diff: the object record
#include "roth_vrweapons.h"
#include "roth_coltvr.h"     // ColtGripTaken (L2's claims)
#include "roth_runtime.h"
#include "roth_interface.h"
#include "roth_hands.h"
#include "roth_objects.h"
#include "roth_inventory.h"
#include "roth_handuse.h"
#include "roth_invhand.h"
#include "roth_panelmap.h"
#include "roth_actors_engine.h"
#include "roth_candles.h"
#include "roth_gestures.h"
#include "roth_held.h"
#include "roth_grabglue.h"
#include "roth_worldhands.h"
#include "roth_worldhandslogic.h"
#include "roth_handstate.h"
#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "d_event.h"
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
#include "hw_effectlights.h"   // THE BREATH: a soft light on the knob or switch the hand can work
#include <math.h>
#include <algorithm>
#include <utility>
#include <vector>

EXTERN_CVAR(Int, vr_mode)   // file scope, never inside a namespace
CVAR(Bool, roth_vrpick, true, 0)          // the VR hands' picker; not saved
CVAR(Bool, roth_vrpick_debug, false, 0)   // a line per change of what each hand points at
// THE HEADSET'S LIVE METRE for RSQ Distance Grab's scripts (wadsrc/static/filter/vrealms/zscript/rsqdg/reach.zs
// RSQDG_Reach.UnitsPerMetre): written every tic from the VR scale (VR_UnitsPerMeter). Not saved.
CVAR(Float, roth_vr_metre_live, 0.f, 0)

namespace roth
{

namespace
{

// ---- tuning (metres unless said) ----
constexpr double kGrabReach   = hands::kZoneAlongM;     // palm, along the hand (the in-reach zone, roth_handstate.h)
constexpr double kGrabBack    = 0.08;    // the grip's ray starts this far behind the palm
constexpr double kTouchRadius = hands::kZoneSurfaceM;   // a palm this close to a wall or a door is on it
constexpr double kGrabRadius  = hands::kZoneObjectM;    // palm to an object's box
constexpr float  kGripClose   = 0.70f, kGripOpen = 0.35f;   // a button pressed / let go: the grip AND the trigger
static_assert(kGripClose == invhand::kPressOn && kGripOpen == invhand::kPressOff, "the panel's buttons are the world's");

struct HandState
{
	// inputs
	bool  valid = false;
	float trigger = 0.f, grip = 0.f;
	bool  triggerTouch = false, thumbTouch = false;
	bool  pointing = false;      // on a panel: the index straight (PoseRule); in the world always false (THE BEAM)
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
	Pick  hover;            // the last target the hand could act on (haptic tick)
	Pick  logged;           // the last target reported (debug line)
	uint16_t loggedShape = 0;


	// THE PICKUP (XR Tools' function_pickup.gd, see THE PICKUP below): the closest pickable thing this tic, and whether
	// it was found by the ranged cone rather than the grab sphere
	AActor     *xrPick = nullptr;
	bool        xrRanged = false;
	int         xrLoose = -1;          // ... or the loose item (roth_physics.h), by index
	AActor     *xrLogged = nullptr;
	int         xrLooseLogged = -1;

	// THE BEAM (see THE BEAM below): on for a free hand; its ray, rigid with the hand

	// THE POKE (see THE POKE in TickHand): the touching point last tic, and the thing it is pressed on now
	bool        pokeHaveLast = false;
	DVector3    pokeLast;
	bool        pokeContact = false;
	Pick        pokeOn;
	DVector3    pokeAt;

	// billboards
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
	int testCandleSector = -1, testCandleIndex = -1;   // roth_vrpicktest candle / wick / look
	double testLookBearing = -1000.0;                  // degrees, candle -> camera; below -360: not chosen yet

	const char *loggedStand = "";   // why the hands last stood down ("" = never reported)

	// the panel the hands work (InterfaceHandPanel), and which hand is its mouse
	int panelWhich = HANDPANEL_NONE;
	int pointerRole = -1;
	bool loggedFireAllowed = true;
	uint16_t loggedShape = 0;

	// the cost counters (VRPickTick)
	int    costTics = 0;
	double costSum = 0, costMax = 0;
};
VRPick P;

// ---- L2, each hand's state (roth_handstate.h; the exports are below, after the anonymous namespace) ----
hands::Hand gL2[2];
bool gHoldEnded[2] = { false, false };     // this tic: an item's hold ended (the world hands' release asks)
bool gStowPending[2] = { false, false };   // ... to be stowed at the tic's end, unless set down or still flying in
int gItemRole = -1, gItemSlot = -1;        // the hand a pickup (or a closing on it) put the selected item in, its slot
uint16_t gItemId = 0;                      // ... and its id word then
hands::State gLoggedState[2] = { hands::State::Free, hands::State::Free };
hands::Owner gLoggedOwner[2] = { hands::Owner::None, hands::Owner::None };

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

// A headset is on -- or the controller-pose test (roth_xrpose) drives the hands through the headset's own path.
bool HeadsetOn()
{
	if (VRPoseTest_Active()) return true;
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

// THE DRAWN HAND'S FRAME (g_levellocals.h BillboardHandBasis -> vr_handpose.h VRHandPose_MapFrame), the one
// the renderer resolves the offset in -- never the aim pose's angles (2026-11-16: the stored pitch was taken
// with the wrong sign there, so everything locked here swung against the hand). No grip: world points.
HandLock LockFor(int role, AActor *pmo)
{
	HandLock L;
	if (pmo == nullptr || !VRHandPose_InputLive()) return L;   // roth_handtest: world points, as before
	FBillboard probe;
	probe.flags = (role == VR_MAINHAND) ? BBFL_HANDMAIN : BBFL_HANDOFF;
	if (!BillboardHandBasis(probe, L.origin, L.fwd, L.right, L.up)) return L;
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
	VSMatrix g;
	if (!VRHandPose_Grip(role, g)) return;   // the headset's, or the controller-pose test's (roth_xrpose)
	VRFingerInput fi;
	if (VRHandPose_Fingers(role, fi))
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
	if (!h.haveRay && !P.test && pmo != nullptr)
	{
		// No drawn hand: the hand's own frame (VRHandPose_MapFrame, the matrix the hand model would be seated
		// in), along the hand from the grip socket. Not the aim pose (AttackAngle / AttackPitch): another
		// tracked pose with a -40 degree pitch bias, and its stored pitch was read here with the wrong sign
		// (2026-11-16), so a raised hand's ray went down.
		DVector3 o, f, r, u;
		if (VRHandPose_MapFrame(role, o, f, r, u))
		{
			h.tip = o;
			h.dir = f;
			h.palm = o;
			h.handFwd = f;
			h.haveRay = true;
		}
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

// L1's REACH (roth_vrrules.h, D2): the pick record's depth for `p`, from the horizontal distance between the
// player's position and the hit point -- the body's reach, whichever hand and whichever way it points.
double BodyDepth(AActor *pmo, const Pick &p)
{
	return pmo != nullptr ? rules::BodyDepth(pmo->X(), pmo->Y(), p.hitPos.X, p.hitPos.Y) : 0.0;
}

//--------------------------------------------------------------------------
// Acting
//--------------------------------------------------------------------------

// THE HAND FEELS IT (docs/VR_INTERACTION.md 4b), through the engine's one live haptic call (VR_ScriptHaptic ->
// the OpenXR haptic output). With no headset -- the controller-pose test, the flat test -- nothing is sent and a line
// says what would have been, so a flat run shows the feedback.
void Haptic(int role, double amp, double ms, const char *why = nullptr)
{
	if (role < 0 || role > 1) return;
	if (P.test || VRPoseTest_Active())
	{
		if (why != nullptr) Printf("[roth_vrpick] %s hand haptic %.2f for %.0f ms (%s; no headset: not sent)\n", RoleName(role), amp, ms, why);
		return;
	}
	if (!HeadsetOn()) return;
	VR_ScriptHaptic(role, amp, ms);
}

// THE FEEDBACK TABLE'S PULSES (docs/VR_INTERACTION.md 4b), one place for their strengths.
constexpr double kTickAmp = 0.12, kTickMs = 12;       // the hand touches something usable: a light tick
constexpr double kUseAmp = 0.45, kUseMs = 35;         // a squeeze uses it: a firm pulse
constexpr double kRefuseAmp = 0.30, kRefuseMs = 12;   // a squeeze refused: a double buzz, the second 3 tics on
constexpr int    kRefuseGapTics = 3;
constexpr double kLandAmp = 0.50, kLandMs = 30;       // a pickup lands in the hand: a pulse on landing
constexpr double kStowAmp = 0.20, kStowMs = 20;       // an item put away: a soft pulse
constexpr double kHoldAmp = 0.30, kHoldMs = 20;       // the hand closes on its item
int gSecondBuzz[2] = { -1, -1 };                      // the refusal's second buzz, at this maptime

void Refused(int role, const char *how)
{
	if (role < 0 || role > 1) return;
	Haptic(role, kRefuseAmp, kRefuseMs, FStringf("refused: %s, the first buzz", how ? how : "a use").GetChars());
	gSecondBuzz[role] = P.level != nullptr ? P.level->maptime + kRefuseGapTics : -1;
}

void SecondBuzzes()
{
	for (int role = 0; role < 2; role++)
		if (gSecondBuzz[role] >= 0 && P.level != nullptr && P.level->maptime >= gSecondBuzz[role])
		{
			gSecondBuzz[role] = -1;
			Haptic(role, kRefuseAmp, kRefuseMs, "refused, the second buzz");
		}
}

// The right-hand item's id (sign-extended, as the hands read it), 0 for none.
int RightHandItem(const Inventory *inv)
{
	if (inv == nullptr || inv->RightHand() < 0) return 0;
	return (int16_t)inv->Slot(inv->RightHand()).id;
}

// The inventory's signature: it changes when anything is given or removed (a slot's id or quantity, the count).
uint64_t InvSignature(const Inventory *inv)
{
	if (inv == nullptr) return 0;
	uint64_t h = (uint64_t)inv->Count();
	for (int i = 0; i < Inventory::SLOT_COUNT; i++)
	{
		const InventorySlot &sl = inv->Slot(i);
		if (sl.id == 0) continue;
		h = h * 1000003u + ((uint64_t)i << 32) + ((uint64_t)sl.id << 16) + sl.quantity;
	}
	return h;
}

int SelectedItem()
{
	return RightHandItem(InterfaceInventory());
}

// THE MAIN HAND IS FREE for a weapon picked up (1.5): no weapon drawn in it, no item in it, no held object.
bool MainHandFree()
{
	if (HandHoldsWeapon(VR_MAINHAND)) return false;
	if (gL2[VR_MAINHAND].itemHeld) return false;
	if (ItemHandRole() == VR_MAINHAND && SelectedItem() > 0) return false;
	return !HeldHandOwned(VR_MAINHAND);
}

// A USE THAT STARTED A FLY-IN gave something (give_item's flight, from a type-4 pickup or a chain's 0x29): the original
// has already put it where it puts it -- a weapon equipped, anything else selected (inventory.c:416-434). Here the VR
// hands take it (1.5): a weapon to the main hand if it is free, else to its body slot; an item into the hand that made
// the use, HOLDING while its grip stays closed.
void AfterPickup(int role, int weapon0, bool mainFree)
{
	Inventory *inv = InterfaceInventory();
	const int w = VRWeaponsHeldItem();
	if (w > 0 && w != weapon0)
	{
		InterfaceRetargetFly(VR_MAINHAND);
		Printf("[roth_vrpick] PICKUP: the original equipped weapon i%d (activate_weapon_item); the main hand %s\n", w,
			mainFree ? "is free: it flies to it and is drawn there" : "is not free: to its body slot");
		if (!mainFree) VRWeaponsToBodySlot(w, weapon0);
		return;
	}
	const int slot = inv != nullptr ? inv->RightHand() : -1;
	const int item = RightHandItem(inv);
	if (slot < 0 || item <= 0) return;
	gItemRole = role;
	gItemSlot = slot;
	gItemId = inv->Slot(slot).id;
	const bool squeezed = P.hand[role].gripClosed;
	const bool flying = InterfaceFlyingTo(role);
	if (squeezed) gL2[role].BeginPickup();
	Printf("[roth_vrpick] PICKUP: the original gave item %d and selected it (give_item); %s the %s hand%s\n", item,
		flying ? "it flies to" : "no fly-in (the give had no picture to fly) -- straight into", RoleName(role),
		squeezed ? ", and stays in it while the grip is closed: HOLDING" : ": the grip is open, it is carried in that hand");
	if (!flying) VRPickPickupLanded(role);
}

// D7: THE STOW. The original's own calls put the selection away: the right hand's cell taken onto the cursor
// (dialogue_ui.c:1767-1787: g_selected_item_primary = 0) and the cursor's item put back (:1743-1744). The item stays
// carried; nothing is selected. Its picture is drawn into the coat over 0.2 s.
void Stow(int role)
{
	Inventory *inv = InterfaceInventory();
	InventorySink *sink = InterfaceInventorySink();
	const int slot = inv != nullptr ? inv->RightHand() : -1;
	if (slot < 0 || ItemHandRole() != role)
	{
		Printf("[roth_vrpick] %s hand let go: nothing to stow (the item is no longer the selected one in this hand)\n", RoleName(role));
		if (gItemRole == role) { gItemRole = gItemSlot = -1; gItemId = 0; }
		return;
	}
	const int item = (int16_t)inv->Slot(slot).id;
	// LET GO ANYWHERE BUT OVER THE SHOULDER (docs/VR_PHYSICS.md; owner 2026-11-24: "stays where it lands"): it falls,
	// or flies with the hand's swing, and lies where it comes to rest. The game puts it away by the same calls as a
	// stow -- carried, not selected -- and keeps it in the pack; only the hands know where it lies.
	if (PhysicsLetGo(role, item))
	{
		inv->PickUpToCursor(slot, sink);
		inv->DropHeld();
		gItemRole = gItemSlot = -1;
		gItemId = 0;
		Haptic(role, kStowAmp, kStowMs, "let go: a soft pulse");
		Printf("[roth_vrpick] %s hand let go of item %d: LOOSE in the world -- the game puts it away as a stow (its cell onto the cursor and"
			" back, dialogue_ui.c:1782-1787, :1744); carried, not selected (right hand slot now %d)\n", RoleName(role), item, inv->RightHand());
		return;
	}
	AActor *pmo = P.level != nullptr && P.level->GetConsolePlayer() != nullptr ? P.level->GetConsolePlayer()->mo : nullptr;
	const HandState &h = P.hand[role];
	DVector3 from = h.palm, to = from;
	if (pmo != nullptr)
	{
		// 3b (owner, 2026-11-23: "put item over our shoulder like a backpack"): it flies from the hand over the shoulder
		// on that hand's side, 20 cm behind the head and 15 cm below the eyes [R: tuning numbers].
		const double vh = pmo->player != nullptr ? pmo->player->viewheight : 41.0;
		const double upm = VRPickUnitsPerMetre();
		const DVector3 fwd(pmo->Angles.Yaw.Cos(), pmo->Angles.Yaw.Sin(), 0), left(-fwd.Y, fwd.X, 0);
		const double side = VRHandPose_SideForRole(role) == VRHandPose::SIDE_RIGHT ? -1.0 : 1.0;
		to = pmo->Pos() + DVector3(0, 0, vh - 0.15 * upm) - fwd * (0.20 * upm) + left * (side * 0.18 * upm);
		if (!h.haveRay) from = to;
	}
	inv->PickUpToCursor(slot, sink);
	inv->DropHeld();
	gItemRole = gItemSlot = -1;
	gItemId = 0;
	InterfaceStowPicture(item, from, to);
	Haptic(role, kStowAmp, kStowMs, "stowed: a soft pulse");
	Printf("[roth_vrpick] %s hand let go of item %d: STOWED over the shoulder into the backpack -- the original's own calls: its cell onto the cursor"
		" (dialogue_ui.c:1782-1787, g_selected_item_primary = 0) and put back (:1744); carried, not selected (right hand slot now %d)\n",
		RoleName(role), item, inv->RightHand());
}

// EVERY USE A HAND MAKES -- the grip (in reach or pointing), the fingertip
// press -- is the original's secondary (use) click, through the port's use path (Activate), WITH
// THE ITEM THAT HAND HOLDS (roth_handuse.h): the hand on the RIGHT side holds
// the original's right-hand item and uses the object with it selected; the
// other hand holds none and uses it as the original does with nothing
// selected. The item test in the object's chain (RAW 0x27) decides the rest,
// unchanged.
bool DoUse(int role, const Pick &p, const CursorClass &c, AActor *pmo, const char *how, bool inReach = false)
{
	if (!c.CanUse()) { ExplainSurfaceBox(p); Refused(role, how); return false; }
	// THE ITEM OF THE HAND THAT MAKES THE USE (1.1): the hand the selected item is in (L2, ItemHandRole) uses with it
	// selected; the other with nothing selected (roth_handuse.h).
	const bool itemHand = role == ItemHandRole();
	Inventory *inv = InterfaceInventory();
	const int before = RightHandItem(inv);
	const int fly0 = InterfaceFlyCount(), weapon0 = VRWeaponsHeldItem();
	const uint64_t sig0 = InvSignature(inv);
	const bool mainFree = MainHandFree();
	HandUse scope;
	if (inv != nullptr) scope = HandUseBegin(*inv, itemHand);
	// A candle's first wick (roth_candles.h): which use starts its lighting, and at which wick. Presentation;
	// the use itself is untouched.
	AActor *const target = p.kind == Pick::Prop ? p.actor : nullptr;
	if (target != nullptr) CandlesBeforeUse(target);
	InterfacePickupHand(role);   // a pickup this use makes flies to this hand (D6)
	const bool acted = Activate(p, pmo, TrigEvent::Use);
	InterfacePickupHand(-1);
	if (target != nullptr)
	{
		const HandState &hs = P.hand[role];
		CandlesAfterUse(target, role, hs.haveRay ? hs.tip : p.hitPos, hs.haveRay ? hs.palm : p.hitPos, p.hitPos, inReach, acted);
	}
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
	// A PICKUP: the original gave something with this use (give_item: the inventory changed) and selected or equipped it.
	const bool gave = InvSignature(inv) != sig0 && (InterfaceFlyCount() != fly0 || now != before || VRWeaponsHeldItem() != weapon0);
	if (gave) AfterPickup(role, weapon0, mainFree);
	if (acted) Haptic(role, kUseAmp, kUseMs, FStringf("a use acted: %s", how).GetChars());
	else Refused(role, how);
	return acted;
}

bool DoExamine(int role, const Pick &p, const CursorClass &c, AActor *pmo, const char *how)
{
	if (!c.CanExamine()) { ExplainSurfaceBox(p); return false; }
	const bool acted = ExaminePick(p, pmo);
	Printf("[roth_vrpick] %s hand %s: EXAMINE %s at depth %.0f (cursor type %d shape 0x%x) -> %s\n", RoleName(role), how,
		PickKindName(p), c.depth, (int)c.type, (unsigned)c.shape, acted ? "examined" : "nothing happened");
	Haptic(role, acted ? 0.35 : 0.12, acted ? 30 : 12, acted ? "an examine acted" : "an examine did nothing");
	return acted;
}




//--------------------------------------------------------------------------
// One hand, one tic
//--------------------------------------------------------------------------

void ResetGestures(HandState &h)
{
	h.hover = Pick{};
}

// THE GLOW (see THE DRAWING in the header). One twin per hand: an actor of the host's own class (so it finds the
// host's model too -- a model is found by class, sprite and frame), wearing the host's sprite, frame, angles, flips
// and offset, 1.06 times its size, 1.5 units toward the eye, drawn AddStencil in one colour: the host's silhouette
// as light. Nothing on the host is touched. Strengths and colours are DistanceGrab's (tell.zs GlowLook, 35 tics a
// second, cos in degrees): could = (0.15 + 0.35 * (0.5 - 0.5 cos(6t))) * 0.55 amber, have = 0.12 + 0.20 * (0.5 - 0.5
// cos(12t)) green. Presentation only.
constexpr uint32_t kGlowCould = 0xFFB030, kGlowHave = 0x40FF70;
constexpr double kGlowRim = 1.12, kGlowToEye = 1.5;   // rim was 1.06: too thin to see (owner, 2026-11-24)
struct Glow { AActor *twin = nullptr; AActor *host = nullptr; };
Glow gGlow[2];

void GlowHide(Glow &g)
{
	if (g.twin != nullptr && !(g.twin->ObjectFlags & OF_EuthanizeMe)) { g.twin->renderflags |= RF_INVISIBLE; g.twin->Alpha = 0.0; }
	g.host = nullptr;
}

void GlowForget()
{
	for (Glow &g : gGlow) { g.twin = nullptr; g.host = nullptr; }
}

void GlowFollow(Glow &g, FLevelLocals *level, AActor *host, const DVector3 &eye, bool have)
{
	if (host == nullptr || (host->ObjectFlags & OF_EuthanizeMe) || (host->renderflags & RF_INVISIBLE) || host->sprite <= 0)
	{
		GlowHide(g);
		return;
	}
	if (g.twin != nullptr && ((g.twin->ObjectFlags & OF_EuthanizeMe) || g.twin->Level != level || g.twin->GetClass() != host->GetClass()))
	{
		if (!(g.twin->ObjectFlags & OF_EuthanizeMe) && g.twin->Level == level) g.twin->Destroy();
		g.twin = nullptr;
	}
	if (g.twin == nullptr)
	{
		g.twin = AActor::StaticSpawn(level, host->GetClass(), host->Pos(), NO_REPLACE);
		if (g.twin == nullptr) return;
		g.twin->UnlinkFromWorld(nullptr);
		g.twin->flags &= ~(MF_SOLID | MF_SHOOTABLE | MF_SPECIAL | MF_PICKUP);
		g.twin->flags |= MF_NOBLOCKMAP | MF_NOGRAVITY;
		g.twin->flags5 |= MF5_NOINTERACTION;
		g.twin->LinkToWorld(nullptr);
	}
	const bool fresh = g.host != host || (g.twin->renderflags & RF_INVISIBLE);
	g.host = host;
	AActor *t = g.twin;
	t->sprite = host->sprite;
	t->frame = host->frame;
	t->Angles = host->Angles;
	t->renderflags = (host->renderflags & (RF_XFLIP | RF_YFLIP)) | RF_FULLBRIGHT;
	t->SpriteOffset = host->SpriteOffset;
	t->Scale = host->Scale * kGlowRim;
	t->RenderStyle = LegacyRenderStyles[STYLE_AddStencil];
	const double tt = (double)level->maptime;
	// Owner in the headset, 2026-11-24: "WHY THE FUCK IS THIS ITEM HIGHLIGHT FUCKIMG IMPOSSIBLE TO SEE". Was Distance
	// Grab's strengths (have 0.12..0.32, could 0.08..0.28): now 0.55..0.85 could, 0.85..1.0 have, plus a light on the
	// thing (VRPickFrameLights).
	const double alpha = have ? 0.85 + 0.15 * (0.5 - 0.5 * cos(tt * 12.0 * (M_PI / 180.0)))
	                          : 0.55 + 0.30 * (0.5 - 0.5 * cos(tt * 6.0 * (M_PI / 180.0)));
	t->SetShade(have ? kGlowHave : kGlowCould);
	t->Alpha = alpha;
	DVector2 d = eye.XY() - host->Pos().XY();
	const double l = d.Length();
	const DVector3 off = l > 0.01 ? DVector3(d.X / l * kGlowToEye, d.Y / l * kGlowToEye, 0) : DVector3(0, 0, 0);
	t->SetOrigin(host->Pos() + off, true);
	if (fresh) t->ClearInterpolation();
}

// What each hand's glow is about this tic: its closest dropped item (XR Tools' request_highlight); the original's items
// glow as Distance Grab's own. "Have" while that hand's grip or trigger is down on it. Both hands on one thing: one
// glow, the stronger.
void GlowTick(FLevelLocals *level, AActor *pmo, bool live)
{
	if (!live || level == nullptr || pmo == nullptr || pmo->player == nullptr)
	{
		for (Glow &g : gGlow) GlowHide(g);
		return;
	}
	AActor *want[2] = { nullptr, nullptr };
	bool have[2] = { false, false };
	for (int role = 0; role < 2; role++)
	{
		const HandState &h = P.hand[role];
		if (!h.valid) continue;
		if (h.xrPick != nullptr) want[role] = h.xrPick;   // a dropped item in reach (the original's items glow as Distance Grab's)
		have[role] = want[role] != nullptr && (h.gripClosed || h.trigDown);
	}
	if (want[0] != nullptr && want[0] == want[1]) { have[0] = have[0] || have[1]; want[1] = nullptr; }
	const DVector3 eye(pmo->X(), pmo->Y(), pmo->player->viewz);
	for (int role = 0; role < 2; role++)
	{
		if (want[role] != nullptr) GlowFollow(gGlow[role], level, want[role], eye, have[role]);
		else GlowHide(gGlow[role]);
	}
}

// The two buttons' press edges (pressed at 0.70, let go at 0.35, for both). A
// button already down when the hand comes back (a controller found, the hands
// back from a menu or a panel) is not a press.
void ReadButtons(HandState &h, bool &gripEdge, bool &trigEdge)
{
	const bool gripWasClosed = h.gripClosed;
	if (!h.valid) h.gripClosed = false;
	else if (h.grip >= kGripClose) h.gripClosed = true;
	else if (h.grip <= kGripOpen) h.gripClosed = false;
	const bool trigWasDown = h.trigDown;
	if (!h.valid) h.trigDown = false;
	else if (h.trigger >= kGripClose) h.trigDown = true;
	else if (h.trigger <= kGripOpen) h.trigDown = false;
	gripEdge = h.primed && h.gripClosed && !gripWasClosed;
	trigEdge = h.primed && h.trigDown && !trigWasDown;
	h.primed = h.valid;
}



// THE PICKUP (owner in the headset, 2026-11-24: "i can't pickup the matchbook because your grab system IS FUCKING
// INDECIPHERABLE ... DID YOU ... LOOK AT REAL VR GAMES OR OPEN SOURCE PROJECTS"). Godot XR Tools' function_pickup.gd
// (MIT, 6c6189b), read whole: every tic each free hand finds its CLOSEST pickable thing (_update_closest_object,
// :507-529) -- first in the grab sphere, `grab_distance` 0.3 m round the pickup point, by squared distance to the
// thing's middle (_get_closest_grab, :310-329); with none there and ranged grab on, the thing nearest the hand's forward
// inside a `ranged_angle` 5-degree cone (_get_closest_ranged, :332-353). That thing is highlighted (request_highlight:
// THE GLOW); the grip pressing picks it up (_on_grip_pressed, :376-381). Ours: the pickup point is the palm; "pickable"
// is the original's own pickup (ObjectIsPickupItem: an item whose use is the type-4 pickup, read only); the pick up is
// the original's use on it from this hand (DoUse), so the item flies in as before. Not XR Tools': the cone reaches as
// far as the original's use does (planar 268 units, L1: reach stays the original's), not 5 m, and the thing must be
// in the hand's sight (the original's pick from the palm meets nothing nearer); its middle is the pick box's.
// A LOOSE ITEM (one the player let go of: roth_physics.h) is pickable the same way, and is ours, not the original's
// (it is already carried): its cone reaches XR Tools' own `ranged_distance`, 5 m, and taken from range it flies to
// the hand (the pickup's fly-in, InterfaceFlyItemToHand).
constexpr double kXRGrabM = 0.3, kXRRangedDeg = 5.0, kXRRangedM = 5.0;

DVector3 PickBoxMiddle(AActor *mo)
{
	DVector3 lo, hi;
	ObjectPickBox(mo, lo, hi);
	return (lo + hi) * 0.5;
}

// In the hand's sight: the original's pick from the palm toward `mid` meets `target` (when it is an object), or
// nothing nearer than it.
bool InSight(const DVector3 &palm, const DVector3 &mid, AActor *target)
{
	DVector3 dir = mid - palm;
	const double l = dir.Length();
	if (l < 1e-6) return true;
	dir /= l;
	Pick p;
	const bool hit = RulesPick(palm, dir, p, l + 8.0);
	return !hit || (target != nullptr && p.kind == Pick::Prop && p.actor == target) || (p.hitPos - palm).Length() >= l - 8.0;
}

void UpdatePickup(int role, HandState &h, AActor *pmo, bool haveRay)
{
	h.xrPick = nullptr;
	h.xrRanged = false;
	h.xrLoose = -1;
	if (!haveRay || pmo == nullptr || P.level == nullptr || !gL2[role].MayAct(hands::Owner::Picker) || HandHoldsWeapon(role)
		|| gL2[role].itemHeld || HeldHandCarries(role))
		return;
	const double upm = UPM(), grab2 = (kXRGrabM * upm) * (kXRGrabM * upm), cosCone = cos(kXRRangedDeg * (M_PI / 180.0));
	// the cone points along the hand's own frame (the frame the hand model is seated in), else the hand's forward
	DVector3 o, fwd, r, u;
	if (!VRHandPose_MapFrame(role, o, fwd, r, u)) fwd = h.handFwd;
	if (fwd.Length() < 1e-6) return;
	// the candidates: the loose items only (the original's items lying in the level are RSQ Distance Grab's)
	struct Cand { AActor *mo; int loose; DVector3 mid; bool far; };
	std::vector<Cand> cands;
	for (int i = 0; i < PhysicsLooseCount(); i++)
	{
		int item = 0;
		DVector3 c;
		if (PhysicsLooseAt(i, item, c)) cands.push_back({ nullptr, i, c, (c - h.palm).Length() > kXRRangedM * upm });
	}
	// _get_closest_grab: the nearest middle inside the sphere; else _get_closest_ranged: the smallest angle in the cone
	const Cand *best = nullptr;
	double best2 = grab2;
	for (const Cand &c : cands)
	{
		const DVector3 d = c.mid - h.palm;
		const double d2 = d | d;
		if (d2 < best2) { best2 = d2; best = &c; }
	}
	bool ranged = false;
	if (best == nullptr)
	{
		double bestDot = cosCone;
		for (const Cand &c : cands)
		{
			if (c.far) continue;
			const DVector3 d = c.mid - h.palm;
			const double l = d.Length();
			if (l < 1e-6) continue;
			const double dp = (d / l) | fwd;
			if (dp > bestDot && InSight(h.palm, c.mid, c.mo)) { bestDot = dp; best = &c; }
		}
		ranged = best != nullptr;
	}
	if (best != nullptr)
	{
		h.xrRanged = ranged;
		if (best->loose >= 0) { h.xrLoose = best->loose; h.xrPick = PhysicsLooseActor(best->loose); }
		else h.xrPick = best->mo;
	}
	if (h.xrPick != h.xrLogged || h.xrLoose != h.xrLooseLogged)
	{
		if (best != nullptr)
		{
			Printf("[roth_vrpick] %s hand's closest pickup (XR Tools): %s%s at %.2f m from the palm -- it glows; the grip takes it\n",
				RoleName(role), best->loose >= 0 ? "a loose item, " : "", ranged ? "in the 5-degree cone" : "in the 0.3 m grab sphere",
				(best->mid - h.palm).Length() / upm);
			Haptic(role, kTickAmp, kTickMs, "a pickup in reach: the light tick");
		}
		h.xrLogged = h.xrPick;
		h.xrLooseLogged = h.xrLoose;
	}
}

// The grip on the closest pickup (XR Tools' _on_grip_pressed). An object: the original's use on it from this hand (it
// flies in as the original's pickup does). A loose item: taken out of the world and selected into this hand by the
// original's own calls (its cell onto the cursor, the cursor into the right hand: dialogue_ui.c:1768-1788,
// 1722-1730); from range it flies in first. The original holds one item at a time: with another item in the other
// hand, refused.
bool PickupGrip(int role, HandState &h, AActor *pmo)
{
	if (pmo == nullptr) return false;
	if (h.xrLoose >= 0)
	{
		Inventory *inv = InterfaceInventory();
		InventorySink *sink = InterfaceInventorySink();
		if (inv == nullptr) return false;
		if (SelectedItem() > 0 && ItemHandRole() != role)
		{
			Printf("[roth_vrpick] %s hand: a loose item to take, but item %d is in the other hand (the original holds one item at a time): refused\n",
				RoleName(role), SelectedItem());
			Refused(role, "a loose item while the other hand holds one");
			return true;
		}
		int item = 0;
		DVector3 from;
		PhysicsLooseAt(h.xrLoose, item, from);
		AActor *drawn = PhysicsLooseActor(h.xrLoose);
		const double yaw = drawn != nullptr ? drawn->Angles.Yaw.Degrees() : 0.0;
		const bool ranged = h.xrRanged;
		item = PhysicsTakeLooseAt(role, h.xrLoose, !ranged);
		h.xrLoose = -1;
		h.xrPick = nullptr;
		if (item <= 0) return false;
		const int slot = inv->FindSlotHolding(item);
		if (slot < 0) { Printf("[roth_vrpick] %s hand took loose item %d, but the game no longer carries it\n", RoleName(role), item); return true; }
		inv->PickUpToCursor(slot, sink);
		inv->HeldToRightHand(sink);
		gItemRole = role;
		gItemSlot = inv->RightHand();
		gItemId = gItemSlot >= 0 ? inv->Slot(gItemSlot).id : 0;
		if (h.gripClosed) gL2[role].BeginPickup();
		Printf("[roth_vrpick] %s hand TAKES loose item %d %s: selected by the original's calls (right hand slot %d)\n", RoleName(role), item,
			ranged ? "from range (it flies in)" : "in reach", inv->RightHand());
		if (ranged) InterfaceFlyItemToHand(role, item, from, drawn != nullptr ? Img3dWornMetres(drawn) : 0.0, yaw);   // lands: VRPickPickupLanded
		else VRPickPickupLanded(role);
		return true;
	}
	AActor *mo = h.xrPick;
	if (mo == nullptr || (mo->ObjectFlags & OF_EuthanizeMe)) return false;
	Pick p;
	p.kind = Pick::Prop;
	p.actor = mo;
	p.hitPos = PickBoxMiddle(mo);
	p.dist = (p.hitPos - h.palm).Length();
	const CursorClass c = ClassifyPick(p, pmo, BodyDepth(pmo, p));
	DoUse(role, p, c, pmo, h.xrRanged ? "the grip on the closest pickup (XR Tools: the ranged cone)" : "the grip on the closest pickup (XR Tools: the grab sphere)", !h.xrRanged);
	h.xrPick = nullptr;
	return true;
}

// THE BACKPACK, TAKING OUT (docs/VR_INTERACTION.md 3b, owner 2026-11-23: "reach over a shoulder and squeeze. The
// original's inventory panel ... opens in front of you"; in the headset 2026-11-24 there was no way to open it). A
// closing grip with the palm in the stow's zone -- behind the face, above the chest, within half a metre
// (roth_physics.cpp PhysicsLetGo's numbers) -- opens the inventory as the 'I' key does. Choosing in it is the
// inventory by hand (ledger row 11). A weapon's body slot on that shoulder claims the grip first (L2's order).
bool BackpackGrip(int role, HandState &h, AActor *pmo)
{
	if (pmo == nullptr || pmo->player == nullptr || !h.valid) return false;
	const double upm = UPM();
	const DVector3 head = pmo->Pos() + DVector3(0, 0, pmo->player->viewheight);
	const DVector3 rel = h.palm - head;
	const DVector3 fwd(pmo->Angles.Yaw.Cos(), pmo->Angles.Yaw.Sin(), 0);
	const double ahead = (rel | fwd) / upm, below = -rel.Z / upm, wide = DVector2(rel.X, rel.Y).Length() / upm;
	if (!(ahead < 0.10 && below < 0.35 && wide < 0.50)) return false;
	if (!InterfaceOpenInventory()) return false;
	Printf("[roth_vrpick] %s hand squeezes over the shoulder (%.2f m ahead of the eyes, %.2f m below): the backpack -- the inventory opens\n",
		RoleName(role), ahead, below);
	Haptic(role, 0.3, 30);
	return true;
}

// THE REACH (owner, 2026-11-24: one laser only -- Distance Grab's; no second pointer): the thing the hand is on, the
// original's pick from the palm along the hand's own frame (the frame the hand model is seated in), as far as XR Tools'
// grab_distance, 0.3 m. Read by the grip (use) and the trigger (examine).
constexpr double kReachM = 0.3;
bool ReachProbe(int role, const HandState &h, AActor *pmo, Pick &p, CursorClass &c)
{
	DVector3 o, f, r, u;
	if (pmo == nullptr || !h.valid || !VRHandPose_MapFrame(role, o, f, r, u)) return false;
	if (!RulesPick(h.palm, f, p, kReachM * UPM())) return false;
	c = ClassifyPick(p, pmo, BodyDepth(pmo, p));
	return true;
}

int GripUse(int role, HandState &h, AActor *pmo, bool haveRay)
{
	// RSQ DISTANCE GRAB'S GRIP FIRST (owner 2026-11-24: "bring back distance grab"; roth_grabglue.h GrabGlueTakesGrip): a
	// hand on an item the package takes (its walk-up grab) or carrying what its palm caught -- that press is the
	// package's, sent to it at the squeeze.
	if (GrabGlueTakesGrip(P.level, role))
	{
		if (Logging())
			Printf("[roth_vrpick] %s hand grip: RSQ Distance Grab's grip takes this press (its walk-up grab or its carry)\n", RoleName(role));
		GrabGlueGripPress(P.level, role);
		return 1;
	}
	if (BackpackGrip(role, h, pmo)) return 1;
	if (PickupGrip(role, h, pmo)) return 1;   // XR Tools' pickup: the closest pickable thing
	// THE TALL WICKS (owner, Q13): the hand with the struck match points its flame at a wick in reach and grips.
	if (haveRay && HeldMatchIn(role) && CandlesPointGrip(role, h.tip, h.dir)) return 1;
	// WHAT THE HAND IS ON (THE REACH): a usable thing within reach in front of the hand -- a switch, a wall's use, a
	// door -- is the original's use from this hand. Items are Distance Grab's.
	Pick p;
	CursorClass c;
	if (ReachProbe(role, h, pmo, p, c))
	{
		if (p.kind == Pick::Prop && ObjectIsPickupItem(p.actor)) return 0;
		if (c.CanUse()) { DoUse(role, p, c, pmo, "the grip on what the hand is on", true); return 1; }
	}
	if (Logging())
		Printf("[roth_vrpick] %s hand grip: no pickup near and nothing usable the hand is on -- nothing\n", RoleName(role));
	return 0;
}

// L2's claims for one hand's closing grip, in roth_handstate.h's order; each takes what it claims.
struct PickerClaims : hands::Claims
{
	int role;
	HandState &h;
	AActor *pmo;
	bool haveRay;
	PickerClaims(int r, HandState &hs, AActor *p, bool ray) : role(r), h(hs), pmo(p), haveRay(ray) {}
	bool Held() override { return HeldGripTaken(role); }                       // the Lamp's bail (roth_held.h)
	bool Colt() override { return ColtGripTaken(role); }                       // the Colt's reload parts (roth_coltvr.h)
	bool Weapon(bool &supporting) override                                     // the fore-end, a body slot (roth_vrweapons.h)
	{
		supporting = role == VR_OFFHAND;
		return VRWeaponsGripTaken(role);
	}
	bool Gesture() override { return WorldHandsGripTaken(role); }              // a pull, a twist (roth_worldhands.h)
	int Use() override { return GripUse(role, h, pmo, haveRay); }
};

struct Step2 { hands::Owner gripTo = hands::Owner::None; };

Step2 L2Tick(int role, HandState &h, AActor *pmo, bool haveRay, bool gripEdge, bool releaseEdge)
{
	hands::Hand &L = gL2[role];
	// THE SELECTION FOLLOWS THE HANDS (1.1): an item held by this hand that the game no longer has selected here (used,
	// given away, another pickup selected) is no longer in the hand -- nothing to stow, the game moved it.
	if (L.itemHeld && !InterfaceFlyingTo(role) && (ItemHandRole() != role || SelectedItem() <= 0))
	{
		L.Drop();
		Printf("[roth_vrpick] %s hand: its item is no longer the selected one (the game changed the selection): the hold ends, nothing to stow\n",
			RoleName(role));
	}
	hands::Facts f;
	f.valid = h.valid;
	f.gripClosed = h.gripClosed;
	f.gripEdge = gripEdge;
	f.releaseEdge = releaseEdge;
	f.target = h.xrPick != nullptr;
	f.coltAction = ColtGripTaken(role);
	f.gestureHeld = WorldHandsHolding(role);
	f.supportHeld = role == VR_OFFHAND && VRWeaponsSupportHeld();
	f.heldObject = HeldHandOwned(role);
	f.toolInHand = HeldMatchIn(role);
	f.weaponInHand = HandHoldsWeapon(role);
	// the item carried in the hand, unless a weapon is drawn in it (the weapon's grip wins while it is drawn: PublishHeld)
	f.itemCarried = role == ItemHandRole() && SelectedItem() > 0 && !f.weaponInHand;
	PickerClaims claims(role, h, pmo, haveRay);
	const hands::Step st = L.Tick(f, claims);
	gHoldEnded[role] = st.holdEnded;
	if (st.holdEnded)
	{
		gStowPending[role] = true;
		Printf("[roth_vrpick] %s hand opens: its hold of item %d ends\n", RoleName(role), SelectedItem());
	}
	if (st.tookHold)
	{
		Inventory *inv = InterfaceInventory();
		gItemRole = role;
		gItemSlot = inv != nullptr ? inv->RightHand() : -1;
		gItemId = gItemSlot >= 0 ? inv->Slot(gItemSlot).id : 0;
		Haptic(role, kHoldAmp, kHoldMs, "the hand closes on its item");
		Printf("[roth_vrpick] %s hand closes on item %d in it (nothing to use there): HOLDING -- let go to stow it\n", RoleName(role), SelectedItem());
	}
	if (L.state != gLoggedState[role] || L.owner != gLoggedOwner[role])
	{
		const bool quiet = (L.state == hands::State::Free || L.state == hands::State::Targeting)
			&& (gLoggedState[role] == hands::State::Free || gLoggedState[role] == hands::State::Targeting);
		if (!quiet || Logging())
			Printf("[roth_vrpick] L2 %s hand: %s, %s%s\n", RoleName(role), hands::StateName(L.state), hands::OwnerName(L.owner),
				st.gripTo != hands::Owner::None ? FStringf(" (the grip's press went to %s)", hands::OwnerName(st.gripTo)).GetChars() : "");
		gLoggedState[role] = L.state;
		gLoggedOwner[role] = L.owner;
	}
	Step2 r;
	r.gripTo = st.gripTo;
	return r;
}

// The end of the tic, after the world hands and the weapons: a hold that ended is stowed, unless its release set the
// item down somewhere (roth_worldhands.h) or it still flies in (it is stowed when it lands).
void L2EndTick()
{
	for (int role = 0; role < 2; role++)
	{
		if (!gStowPending[role]) continue;
		if (WorldHandsReleasedThisTic(role))
		{
			gStowPending[role] = false;
			Printf("[roth_vrpick] %s hand: its item was set down by the release (the original's use there): not stowed\n", RoleName(role));
			continue;
		}
		if (InterfaceFlyingTo(role)) continue;
		gStowPending[role] = false;
		Stow(role);
	}
}

// THE OWNER'S BUTTONS (2026-11-13): the TRIGGER is the original's PRIMARY click
// (examine), the GRIP its SECONDARY (use). See the header.
void TickHand(int role, AActor *pmo)
{
	HandState &h = P.hand[role];
	ReadInputs(role, h);
	// POINTING: the index straight, as the hand's pose shows it (PoseRule: off the trigger, not touching it).
	h.pointing = h.valid && InvHandIndexStraight(h.triggerTouch, h.trigger);
	bool gripEdge = false, trigEdge = false;
	const bool gripWasClosed = h.gripClosed;
	ReadButtons(h, gripEdge, trigEdge);
	const bool releaseEdge = h.valid && gripWasClosed && !h.gripClosed;
	// The grip opened: the release of a press RSQ Distance Grab was handed at the squeeze (roth_grabglue.h).
	if (gripWasClosed && !h.gripClosed) GrabGlueGripRelease(P.level, role);

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

	const bool haveRay = h.valid && FingerRay(role, h, pmo);   // the palm and the hand's forward: the pickup's point and cone
	if (!haveRay && Logging() && h.valid && h.rayWhy != h.loggedRayWhy)
		Printf("[roth_vrpick] %s hand: no hand points -- %s\n", RoleName(role), h.rayWhy ? h.rayWhy : "unknown");
	h.loggedRayWhy = h.haveRay ? nullptr : h.rayWhy;

	// THE ONE SYSTEM (owner, 2026-11-24, choosing: "Strip it, use XR Tools"). Godot XR Tools' pickup (THE PICKUP: the
	// closest pickable thing, grab sphere then ranged cone, highlighted, the grip takes it) and its pointer (THE BEAM: a
	// laser from every free hand, the grip uses and the trigger examines what it meets). Nothing else targets: the
	// touch rule, the in-reach zone, the fingertip press and the held aim are gone. The original's rules decide every
	// use underneath (L1).
	UpdatePickup(role, h, pmo, haveRay);
	// THE REACH (see ReachProbe): what the hand is on now, for the haptic tick and the debug line
	h.havePick = ReachProbe(role, h, pmo, h.pick, h.cls);
	if (!h.havePick) h.cls = CursorClass{};

	// ---- THE POKE (owner in the headset, 2026-11-24: "WHY CAN'T I JUST TOUCH A LIGHTSWITCH") ----
	// Godot XR Tools' poke (player/poke/poke.gd + poke_body.gd, MIT, 6c6189b, read whole): a 5 mm sphere on the
	// fingertip moved each physics frame from where it was toward where the finger is (move_and_slide); the first
	// thing that stops it is a CONTACT START -- the pointer "pressed" on that body -- and the contact ends when the
	// sphere no longer meets it ("released"). A move longer than teleport_distance, 0.1 m, is a jump: the sphere is
	// put there, nothing pressed. Ours: the sweep is the original's pick along the fingertip's path this tic (from
	// last tic's point to this one's, plus the 5 mm radius) -- for the hand carrying an item, the palm's path, the item
	// being in the palm -- and "pressed" is the original's use of what it met, from this hand, once per contact: a
	// light switch, a candle (with the Matchbox in that hand, the original's use with it), anything the original lets
	// a hand use that is a thing in the world and not an item (items are the grip's and Distance Grab's; walls and
	// doors stay the grip's). The contact ends once the point is 2 cm clear of where it pressed.
	if (haveRay && h.fromFinger && !HandHoldsWeapon(role))
	{
		const double upm = UPM();
		const DVector3 now = VRPickHeldItem(role) > 0 ? h.palm : h.tip;
		if (h.pokeContact && (now - h.pokeAt).Length() > (0.005 + 0.02) * upm) h.pokeContact = false;   // released
		if (h.pokeHaveLast && !h.pokeContact)
		{
			const DVector3 mv = now - h.pokeLast;
			const double len = mv.Length();
			if (len > 1e-3 && len <= 0.1 * upm)
			{
				Pick pk;
				if (RulesPick(h.pokeLast, mv / len, pk, len + 0.005 * upm) && pk.kind == Pick::Prop && pk.actor != nullptr
					&& !ObjectIsPickupItem(pk.actor))
				{
					const CursorClass pc = ClassifyPick(pk, pmo, BodyDepth(pmo, pk));
					if (pc.CanUse())
					{
						h.pokeContact = true;
						h.pokeOn = pk;
						h.pokeAt = now;
						if (Logging()) Printf("[roth_vrpick] %s hand POKE: the %s touches %s\n", RoleName(role),
							VRPickHeldItem(role) > 0 ? "palm (with its item)" : "fingertip", PickKindName(pk));
						DoUse(role, pk, pc, pmo, "a touch (the poke)", true);
					}
				}
			}
		}
		h.pokeLast = now;
		h.pokeHaveLast = true;
	}
	else { h.pokeHaveLast = false; h.pokeContact = false; }

	// ---- L2: WHO OWNS THIS HAND THIS TIC (roth_handstate.h) ----
	// The standing owners first, then a closing grip's claims in order -- the Lamp's bail, the Colt, the weapon (the
	// fore-end: SUPPORTING; a body slot), a world gesture (a lever, a key in its lock, a door's handle), then THE
	// PICKUP and THE BEAM (GripUse) -- and with nothing to use, the hand closes on the item it carries.
	L2Tick(role, h, pmo, haveRay, gripEdge, releaseEdge);

	if (roth_vrpick_debug || P.test)
	{
		const bool changed = h.havePick ? (!SamePickTarget(h.pick, h.logged) || h.cls.shape != h.loggedShape)
		                                 : h.logged.kind != Pick::Nothing;
		if (changed)
		{
			if (h.havePick)
				Printf("[roth_vrpick] %s hand is on %s at planar %.0f (depth %.0f), %.0f along it: cursor type %d shape 0x%x"
					" (record type %d, examinable %d%s)\n", RoleName(role), PickKindName(h.pick), (h.cls.depth - 64.0) / 2.0, h.cls.depth,
					h.pick.dist, (int)h.cls.type, (unsigned)h.cls.shape, (int)h.cls.recType, h.cls.examinable,
					h.cls.outOfRange ? ", out of range" : "");
			else
				Printf("[roth_vrpick] %s hand is on nothing\n", RoleName(role));
			h.logged = h.havePick ? h.pick : Pick{};
			h.loggedShape = h.havePick ? h.cls.shape : 0;
		}
	}

	// ---- THE TRIGGER (the original's primary: examine): the closest pickup, else what the hand is on ----
	// Not on the hand holding a weapon: its trigger fires it (the binding's +attack, WeaponTick), as before.
	if (trigEdge)
	{
		if (HandHoldsWeapon(role))
		{
			if (Logging()) Printf("[roth_vrpick] %s hand trigger: the hand holds a weapon -- it fires, nothing to examine\n", RoleName(role));
		}
		else if (gL2[role].owner == hands::Owner::Colt || gL2[role].state == hands::State::Supporting)
		{
			// L2: the off hand on the gun (a reload part, the fore-end): its trigger is the weapon's (the magazine release)
			if (Logging()) Printf("[roth_vrpick] %s hand trigger: the hand is %s's (%s) -- nothing to examine\n", RoleName(role),
				hands::OwnerName(gL2[role].owner), hands::StateName(gL2[role].state));
		}
		else
		{
			Pick p;
			bool have = false;
			if (h.xrPick != nullptr && h.xrLoose < 0 && !(h.xrPick->ObjectFlags & OF_EuthanizeMe))
			{
				p.kind = Pick::Prop;
				p.actor = h.xrPick;
				p.hitPos = PickBoxMiddle(h.xrPick);
				have = true;
			}
			else if (h.havePick) { p = h.pick; have = true; }   // what the hand is on (THE REACH)
			if (have)
			{
				const CursorClass c = ClassifyPick(p, pmo, BodyDepth(pmo, p));
				if (c.CanExamine()) DoExamine(role, p, c, pmo, "trigger");
				else
				{
					if (Logging()) Printf("[roth_vrpick] %s hand trigger on %s: %s\n", RoleName(role), PickKindName(p), WhyNotExamine(c).GetChars());
					ExplainSurfaceBox(p);   // roth_trigger_debug: the surface box's part in it
				}
			}
			else if (Logging())
				Printf("[roth_vrpick] %s hand trigger: no pickup near and nothing the hand is on\n", RoleName(role));
		}
	}

	// ---- HAPTIC TICK on the hand coming onto something actionable ----
	const bool actionable = h.havePick && (h.cls.CanUse() || h.cls.CanExamine());
	if (actionable && !SamePickTarget(h.pick, h.hover)) { h.hover = h.pick; Haptic(role, 0.12, 12); }
	else if (!actionable) h.hover = Pick{};

	// Nothing is drawn from the hand: the one laser is RSQ Distance Grab's. THE GLOW is GlowTick's.
	HideHand(h);
}

//--------------------------------------------------------------------------
// On a panel: each hand is the original's mouse (see the header)
//--------------------------------------------------------------------------

PanelV3 PV(const DVector3 &v) { return PanelV3(v.X, v.Y, v.Z); }
DVector3 DV(const PanelV3 &v) { return DVector3(v.x, v.y, v.z); }

const char *PanelName(int which) { return which == HANDPANEL_POPUP ? "examine popup" : which == HANDPANEL_MENU ? "menu box" : "inventory"; }

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
	// The finger is only looked at while it can be the mouse (InvHand::WantsRay):
	// pointing, the tic the index leaves (when the point is taken to be held: on
	// the owner's PC run of 983ef7b8 this tic was missed, the hand forgot its
	// point and the trigger clicked nothing), the point held, the fingertip on it.
	const bool wants = h.panel.WantsRay(h.valid, h.triggerTouch, h.trigger);
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

// While the dialogue owns the pointer: a press of either button of either hand
// is a click at what the hand points at (InterfaceHandDialogueClick: the line is
// skipped, or the choice row under the ray is picked), once per press.
void TickDialogueHand(int role, AActor *pmo)
{
	HandState &h = P.hand[role];
	ReadInputs(role, h);
	bool gripEdge = false, trigEdge = false;
	ReadButtons(h, gripEdge, trigEdge);
	h.pointing = false;
	h.havePick = false;
	ResetGestures(h);
	HideHand(h);
	if (!(gripEdge || trigEdge) || !FingerRay(role, h, pmo)) return;
	const bool took = InterfaceHandDialogueClick(PV(h.tip), PV(h.dir));
	Printf("[roth_vrpick] %s hand %s: a click on the dialogue -> %s\n", RoleName(role), trigEdge ? "trigger" : "grip",
		took ? "taken (the line skipped, or the choice under the ray)" : "the dialogue no longer owns the pointer");
	Haptic(role, 0.3, 25);
}

void ForgetLevel()
{
	P.level = nullptr;
	GlowForget();
	for (HandState &h : P.hand)
	{
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
	for (int r = 0; r < 2; r++) { gL2[r].Reset(); gHoldEnded[r] = gStowPending[r] = false; }
	gItemRole = gItemSlot = -1;
	gItemId = 0;
}

} // namespace (anonymous)

hands::Owner HandOwnerNow(int role) { return role >= 0 && role <= 1 ? gL2[role].owner : hands::Owner::None; }
hands::State HandStateNow(int role) { return role >= 0 && role <= 1 ? gL2[role].state : hands::State::Free; }
bool HandMayAct(int role, hands::Owner who) { return role < 0 || role > 1 || gL2[role].MayAct(who); }
bool HandEmptyNow(int role) { return role < 0 || role > 1 || gL2[role].Empty(); }
bool HandHoldEndedThisTic(int role) { return role >= 0 && role <= 1 && gHoldEnded[role]; }

int ItemHandRole()
{
	const Inventory *inv = InterfaceInventory();
	if (gItemRole >= 0 && gItemSlot >= 0 && inv != nullptr && inv->RightHand() == gItemSlot && inv->Slot(gItemSlot).id == gItemId)
		return gItemRole;
	return VRHandPose_RoleForSide(VRHandPose::SIDE_RIGHT);
}

void VRPickPickupLanded(int role)
{
	if (role < 0 || role > 1) return;
	gL2[role].Landed();
	Haptic(role, kLandAmp, kLandMs, "the pickup lands in the hand");
	Printf("[roth_vrpick] %s hand: the pickup lands in it%s\n", RoleName(role),
		gL2[role].itemHeld ? " -- HOLDING (let go to stow it)" : gStowPending[role] ? " -- the grip opened in flight: stowed now" : "");
}

int VRPickHeldItem(int role)
{
	if (role < 0 || role > 1 || HandHoldsWeapon(role)) return 0;
	const int item = SelectedItem();
	return item > 0 && ItemHandRole() == role ? item : 0;
}

void VRPickHaptic(int role, double amp, double ms, const char *why) { Haptic(role, amp, ms, why); }

void VRPickEndLevel()
{
	PhysicsEndLevel();
	ForgetLevel();
	GesturesEndLevel();
	WorldHandsEndLevel();
}

bool VRPickFlamePoints(int role, DVector3 &tip, DVector3 &palm)
{
	if (role < 0 || role > 1 || P.level == nullptr || !roth_vrpick) return false;
	if (P.test && !(P.testHands & (1 << role))) return false;
	if (!P.test && !HeadsetOn()) return false;
	player_t *player = P.level->GetConsolePlayer();
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	if (pmo == nullptr) return false;
	HandState tmp;
	if (!FingerRay(role, tmp, pmo) || !tmp.fromFinger) return false;   // the drawn hand's own points only
	tip = tmp.tip;
	palm = tmp.palm;
	return true;
}


bool VRPickTakeCaught(int role, AActor *item)
{
	if (item == nullptr || role < 0 || role > 1) return false;
	AActor *pmo = (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? players[consoleplayer].mo : nullptr;
	if (pmo == nullptr) return false;
	// The palm the item arrived at (as drawn), else the item itself: the original's cursor on it from there.
	HandState tmp;
	const DVector3 at = item->PosPlusZ(item->Height * 0.5);
	const DVector3 from = FingerRay(role, tmp, pmo) ? tmp.palm : at;
	Pick p;
	p.kind = Pick::Prop;
	p.actor = item;
	p.hitPos = at;
	p.dist = (at - from).Length();
	const CursorClass c = ClassifyPick(p, pmo, BodyDepth(pmo, p));
	if (!c.CanUse())
	{
		Printf("[roth_vrpick] %s hand: distance grab catch -- %s\n", RoleName(role), WhyNotUse(c).GetChars());
		return false;
	}
	InterfacePickupArrived(true);   // Distance Grab flew it here: the original's fly-in does not fly it again
	const bool took = DoUse(role, p, c, pmo, "distance grab catch", true);
	InterfacePickupArrived(false);
	return took;
}

double VRPickUnitsPerMetre() { return UPM(); }

bool VRPickHandNow(int role, VRPickHand &out)
{
	out = VRPickHand();
	if (role < 0 || role > 1 || P.level == nullptr || !roth_vrpick) return false;
	if (P.test && !(P.testHands & (1 << role))) return false;
	if (!P.test && !HeadsetOn()) return false;
	player_t *player = P.level->GetConsolePlayer();
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	const HandState &h = P.hand[role];
	if (pmo == nullptr || !h.valid) return false;
	out.valid = true;
	out.grip = h.grip;
	out.trigger = h.trigger;
	out.triggerTouch = h.triggerTouch;
	out.gripClosed = h.gripClosed;
	out.trigDown = h.trigDown;
	out.pointing = h.pointing;
	HandState tmp;
	if (!FingerRay(role, tmp, pmo) || !tmp.fromFinger) return true;   // the drawn hand's own points only
	out.havePoints = true;
	out.tip = tmp.tip;
	out.dir = tmp.dir;
	out.palm = tmp.palm;
	out.handFwd = tmp.handFwd;
	return true;
}

bool VRPickHandRay(int role, DVector3 &start, DVector3 &dir, bool *fromFinger)
{
	if (role < 0 || role > 1) return false;
	AActor *pmo = (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? players[consoleplayer].mo : nullptr;
	if (pmo == nullptr) return false;
	HandState tmp;
	if (!FingerRay(role, tmp, pmo)) return false;
	start = tmp.tip;
	dir = tmp.dir;
	if (fromFinger != nullptr) *fromFinger = tmp.fromFinger;
	return true;
}

//--------------------------------------------------------------------------
// L1, THE RULES ENTRY (roth_vrrules.h). Every VR interaction ends here.
//--------------------------------------------------------------------------

static AActor *ConsolePawn()
{
	if (P.level != nullptr)
	{
		player_t *player = P.level->GetConsolePlayer();
		if (player != nullptr) return player->mo;
	}
	return (consoleplayer >= 0 && consoleplayer < MAXPLAYERS) ? players[consoleplayer].mo : nullptr;
}

bool RulesPick(const DVector3 &origin, const DVector3 &dir, Pick &out, double range)
{
	AActor *pmo = ConsolePawn();
	out = Pick{};
	if (pmo == nullptr) return false;
	return PickFrom(pmo, origin, dir, range > 0 ? range : PickTraceRange(dir), out);
}

CursorClass RulesClassify(const Pick &p)
{
	AActor *pmo = ConsolePawn();
	if (pmo == nullptr) return CursorClass{};
	return ClassifyPick(p, pmo, BodyDepth(pmo, p));
}

rules::Class RulesClassOf(const CursorClass &c) { return rules::ClassOf(c.type, c.shape, c.depth); }

bool RulesUse(int role, const Pick &p, const char *how)
{
	if (role < 0 || role > 1) return false;
	AActor *pmo = ConsolePawn();
	if (pmo == nullptr) return false;
	const CursorClass c = RulesClassify(p);
	if (!c.CanUse())
	{
		Printf("[roth_vrpick] %s hand %s on %s: %s\n", RoleName(role), how, PickKindName(p), WhyNotUse(c).GetChars());
		ExplainSurfaceBox(p);
		Refused(role, how);
		return false;
	}
	return DoUse(role, p, c, pmo, how, true);
}

bool RulesExamine(int role, const Pick &p, const char *how)
{
	if (role < 0 || role > 1) return false;
	AActor *pmo = ConsolePawn();
	if (pmo == nullptr) return false;
	const CursorClass c = RulesClassify(p);
	if (!c.CanExamine())
	{
		Printf("[roth_vrpick] %s hand %s on %s: %s\n", RoleName(role), how, PickKindName(p), WhyNotExamine(c).GetChars());
		return false;
	}
	return DoExamine(role, p, c, pmo, how);
}

bool VRPickUseObject(int role, AActor *target, const DVector3 &at)
{
	if (target == nullptr) return false;
	DVector3 tip, palm;
	if (!VRPickFlamePoints(role, tip, palm)) return false;
	Pick p;
	p.kind = Pick::Prop;
	p.actor = target;
	p.hitPos = at;
	p.dist = (at - tip).Length();
	return RulesUse(role, p, "a flame at a wick");
}

// THE BREATH (owner, 2026-11-24: "we can have [doorknobs] and the light switches gently breathe some life when hand is
// close enough to interact"). Each frame: where a hand's reach (ReachProbe) is on something it could use that is not an
// item (a door, a switch, a wall's use: items are Distance Grab's and glow as its own), a small amber light breathes
// there, 3 cm toward the hand. Distance Grab's "could" breath (tell.zs GlowLook: (0.15 + 0.35 * (0.5 - 0.5 cos 6t))
// x 0.55, t in tics, 60 tics a breath) and its amber (0xFFB030), at the struck match's strength (roth_held.cpp: 0.45)
// over 0.15 m. Presentation only.
static void PickupLights(FLevelLocals *level, double t, double upm);
void VRPickFrameLights(FLevelLocals *level, double ticFrac)
{
	if (level == nullptr || level != P.level) return;
	const double upm = UPM();
	const double t = (double)level->maptime + ticFrac;
	const double breath = (0.15 + 0.35 * (0.5 - 0.5 * cos(t * 6.0 * (M_PI / 180.0)))) * 0.55;   // 0.08 .. 0.28
	static const float amber[3] = { 1.0f, 0.69f, 0.19f };
	PickupLights(level, t, upm);
	for (int role = 0; role < 2; role++)
	{
		const HandState &h = P.hand[role];
		if (!h.valid || !h.havePick || !h.cls.CanUse()) continue;
		if (h.pick.kind == Pick::Prop && (h.pick.actor == nullptr || ObjectIsPickupItem(h.pick.actor))) continue;
		DVector3 toHand = h.palm - h.pick.hitPos;
		const double l = toHand.Length();
		const DVector3 at = l > 1e-6 ? h.pick.hitPos + toHand / l * (0.03 * upm) : h.pick.hitPos;
		const double intensity = 0.45 * breath / 0.275;   // the match's 0.45 at the breath's top
		EffectLights::FrameLight f;
		f.A = { at.X, at.Y, at.Z };
		f.B = f.A;
		for (int ch = 0; ch < 3; ch++) f.Color[ch] = (float)(amber[ch] * intensity);
		f.Radius = 0.15 * upm;
		f.Flags = 0;
		EffectLights::Get().AddFrameLight(f);
	}
}

// THE PICKUP'S LIGHT (owner in the headset, 2026-11-24: the item highlight "IMPOSSIBLE TO SEE"): each hand's closest
// pickup (THE PICKUP: an item the grip takes, or a loose one -- a model on the floor, which THE GLOW cannot outline)
// gets a breathing light on it, amber, green while that hand's grip or trigger is down. 0.25 m round it, at twice the
// match's strength.
static void PickupLights(FLevelLocals *level, double t, double upm)
{
	for (int role = 0; role < 2; role++)
	{
		const HandState &h = P.hand[role];
		if (!h.valid) continue;
		DVector3 at;
		if (h.xrLoose >= 0) { int item = 0; if (!PhysicsLooseAt(h.xrLoose, item, at)) continue; }
		else if (h.xrPick != nullptr && !(h.xrPick->ObjectFlags & OF_EuthanizeMe)) at = PickBoxMiddle(h.xrPick);
		else continue;
		const bool have = h.gripClosed || h.trigDown;
		const double k = have ? 1.0 : 0.6 + 0.4 * (0.5 - 0.5 * cos(t * 6.0 * (M_PI / 180.0)));
		static const float amber[3] = { 1.0f, 0.69f, 0.19f }, green[3] = { 0.25f, 1.0f, 0.44f };
		const float *col = have ? green : amber;
		EffectLights::FrameLight f;
		f.A = { at.X, at.Y, at.Z };
		f.B = f.A;
		for (int ch = 0; ch < 3; ch++) f.Color[ch] = (float)(col[ch] * 0.9 * k);
		f.Radius = 0.25 * upm;
		f.Flags = 0;
		EffectLights::Get().AddFrameLight(f);
	}
}

bool VRPickTesting() { return P.test; }

// VR FIRST (owner in the headset, 2026-11-24: "if we are going vr first then we should disable or i will unbind the
// use key so i have to use the gestures and world"): with the hands live, only the hands use the world.
bool VRPickHandsOwnTheWorld() { return roth_vrpick && (P.test ? P.testHands != 0 : HeadsetOn()); }

void VRPickTick(FLevelLocals *level, bool modal)
{
	if (level == nullptr) return;
	if (fabs((double)roth_vr_metre_live - UPM()) > 0.01) roth_vr_metre_live = (float)UPM();   // Distance Grab's metre
	if (P.level != level) { ForgetLevel(); P.level = level; }

	player_t *player = level->GetConsolePlayer();
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	const bool on = roth_vrpick && pmo != nullptr && (HeadsetOn() || P.test);
	const char *owner = InterfacePointerOwner();
	const bool stand = modal || !on || player->health <= 0 || owner != nullptr;

	// THE FIRE BUTTON (owner, 2026-11-13: "the trigger examines on either hand,
	// and never sends attack unless that hand holds a drawn weapon"). While the
	// hands are live (a headset, or the flat test), BT_ATTACK -- the main hand's
	// trigger through the binding -- reaches the weapon only while the main hand
	// holds one (HandHoldsWeapon; the main hand is either side, VR_MAINHAND):
	// WeaponTick's gate, and the player's ButtonMask for anything else that
	// reads the button. The hands' own attack is the grip's (TickHand). In flat
	// the button is untouched.
	if (player != nullptr)
	{
		const bool handsOwnAttack = roth_vrpick && pmo != nullptr && (HeadsetOn() || P.test);
		// THE SWORD IS SWUNG, NEVER TRIGGERED (owner, chapter 2 Q6): its attack is the swing's (roth_gestures.cpp, the
		// blade), so the fire button does not reach it (roth_vrweapons.h VRWeaponsTriggerHeldBack).
		const bool swung = handsOwnAttack && VRWeaponsTriggerHeldBack();
		const bool allowed = !handsOwnAttack || (HandHoldsWeapon(VR_MAINHAND) && !swung);
		ActorsSetFireButtonAllowed(allowed);
		if (allowed) player->ButtonMask &= ~BT_ATTACK;
		else player->ButtonMask |= BT_ATTACK;
		if (Logging() && allowed != P.loggedFireAllowed)
		{
			Printf("[roth_vrpick] the fire button (attack) %s\n", allowed ? (handsOwnAttack ? "reaches the weapon: the main hand holds one" : "reaches the weapon")
				: swung ? "is held back: the main hand holds a swung weapon (a blade that lands attacks)"
				: "is held back: the main hand holds no weapon (its trigger examines; a fist that lands attacks)");
			P.loggedFireAllowed = allowed;
		}
	}
	// THE DIALOGUE OWNS THE POINTER (a choice menu, a line waiting for a click):
	// either hand's press is the original's click there (game_core.c:597-632).
	const bool dialogueOwns = !modal && on && player->health > 0 && owner != nullptr && !strcmp(owner, "dialogue");
	// ON A PANEL (the inventory screen, the popup over it): the hands are its mouse.
	PanelPlane plane;
	int panel = HANDPANEL_NONE;
	const bool menuOwns = on && player->health > 0 && owner != nullptr && !strcmp(owner, "menu");
	if ((!modal || menuOwns) && on && player->health > 0 && owner != nullptr)
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
	SecondBuzzes();

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

	if (dialogueOwns)
		for (int role = 0; role < 2; role++) TickDialogueHand(role, pmo);
	for (int role = 0; role < 2 && panel == HANDPANEL_NONE && !dialogueOwns; role++)
	{
		if (stand)
		{
			HandState &h = P.hand[role];
			h.havePick = false;
			h.pointing = false;
			h.primed = false;
			gHoldEnded[role] = false;
			ResetGestures(h);
			HideHand(h);
			continue;
		}
		TickHand(role, pmo);
	}
	// THE GLOW, after both hands have their targets (see THE DRAWING in the header).
	GlowTick(level, pmo, !stand && panel == HANDPANEL_NONE && !dialogueOwns);
	// THE GESTURES (roth_gestures.h), after both hands, while the hands work the world.
	GesturesTick(level, !stand && panel == HANDPANEL_NONE && !dialogueOwns);
	WorldHandsTick(level, !stand && panel == HANDPANEL_NONE && !dialogueOwns);   // chapter 2's world by hand (roth_worldhands.h)
	// CHAPTER 2'S WEAPONS (roth_vrweapons.h): the worn Shield, the Shotgun's second hand, the body slots.
	VRWeaponsTick(level, !stand && panel == HANDPANEL_NONE && !dialogueOwns);
	// L2's end of the tic: the stows (after the world hands' release had its say).
	if (!stand && panel == HANDPANEL_NONE && !dialogueOwns) L2EndTick();

	// WHAT IT COSTS, measured every tic it runs: into the perf log's CPU
	// timings as "game.vrpick" (summed per frame, like every cpu_fx_ms entry),
	// and, under the flat test or roth_vrpick_debug, a line every 70 tics with
	// the average and the worst tic in microseconds.
	if (stand && panel == HANDPANEL_NONE && !dialogueOwns) return;
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
//   roth_vrpicktest texel <face> <x> <y> [metres]   stand square on to a Realms face, eye and finger on its texel x,y
//   roth_vrpicktest palm <metres>                   then the test hand's palm <metres> in front of that point
//                                        with the inventory (or the examine popup) up: aim the test ray
//                                        at the centre of pixel (x, y) of its picture (the original's
//                                        panel pixels: the inventory's are from its top left, 0..287 x
//                                        0..79). With <metres>, also move the player across the floor
//                                        so the TEST HAND's fingertip is <metres> in front of the
//                                        panel at that pixel -- 0.10 hovers, -0.01 is through it, a
//                                        touch -- raising or lowering the drawn test hand to the pixel's
//                                        row (VRHandTest_SetLift, in the test grip's own units).
//   roth_vrpicktest candle <sector> <index>
//                                        the candle the next two act on (a Realms object, roth_candles.h)
//   roth_vrpicktest wick <n> [metres]    move the player across the floor and lift the drawn TEST HAND so its
//                                        index fingertip is at wick <n> of that candle (1 = the leftmost as
//                                        drawn), or <metres> in front of it toward the player; the view's yaw
//                                        is kept (a billboard's wicks lie across the view)
//   roth_vrpicktest aimwick <n>          aim the test ray from the fingertip at wick <n> of that candle (the
//                                        tall wicks' point + grip, CandlesPointGrip)
//   roth_vrpicktest look <units> [bearing]
//                                        stand on the floor <units> from that candle at <bearing> degrees
//                                        (candle -> camera; 0 east, 90 north; default the last one given for
//                                        this candle, else where the player stands), facing its middle, the
//                                        test hand lowered (for a capture)
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
			" | seek needs <item> [metres] | panel <x> <y> [metres] | texel <face> <x> <y> [metres] | palm <metres> | candle <sector> <index> | wick <n> [metres] | aimwick <n> | look <units> [bearing] | off\n");
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
	if (!stricmp(argv[1], "candle") && argv.argc() >= 4)
	{
		P.testCandleSector = atoi(argv[2]);
		P.testCandleIndex = atoi(argv[3]);
		P.testLookBearing = -1000.0;   // a new candle: `look` takes its bearing again
		DVector3 c; double h = 0; int n = 0;
		if (!CandleExtent(P.testCandleSector, P.testCandleIndex, c, h, n)) { Printf("roth_vrpicktest candle: object %s/%s is not a candle with an actor\n", argv[2], argv[3]); return; }
		Printf("roth_vrpicktest candle: object %d/%d, %d wick(s), %.0f units tall, middle at (%.0f %.0f %.0f)\n", P.testCandleSector, P.testCandleIndex, n, h, c.X, c.Y, c.Z);
		return;
	}
	if (!stricmp(argv[1], "look") && argv.argc() >= 3)
	{
		// THE BEARING IS FIXED, not taken from where the player happens to be: after a `wick` the player stands
		// wherever the hand needed him, and a bearing read from there turned the view into the walls (owner's
		// PC, candles_flat at c77146f7). Given, or the last one used on this candle.
		AActor *pmo = players[consoleplayer].mo;
		DVector3 c; double h = 0; int n = 0;
		if (pmo == nullptr || !CandleExtent(P.testCandleSector, P.testCandleIndex, c, h, n)) { Printf("roth_vrpicktest look: no candle (roth_vrpicktest candle first)\n"); return; }
		const double d = atof(argv[2]);
		if (argv.argc() >= 4) P.testLookBearing = atof(argv[3]);
		else if (P.testLookBearing < -360.0) P.testLookBearing = atan2(pmo->Y() - c.Y, pmo->X() - c.X) * (180.0 / M_PI);
		const DAngle b = DAngle::fromDeg(P.testLookBearing);
		const DVector3 stand(c.X + b.Cos() * d, c.Y + b.Sin() * d, pmo->Z());
		pmo->SetOrigin(stand, false);
		pmo->SetZ(pmo->floorz);          // stand on the floor there (SetOrigin found it), not at the old height
		pmo->Vel.Zero();
		pmo->Angles.Yaw = b + DAngle::fromDeg(180.0);
		const double eye = pmo->Z() + players[consoleplayer].viewheight;
		pmo->Angles.Pitch = DAngle::fromDeg(atan2(eye - c.Z, d) * (180.0 / M_PI));   // + looks down
		pmo->ClearInterpolation();
		VRHandTest_SetLift(0.0);
		Printf("roth_vrpicktest look: %.0f units from object %d/%d at bearing %.0f, standing at (%.0f %.0f %.0f) in sector %d facing it,"
			" pitch %.1f\n", d, P.testCandleSector, P.testCandleIndex, P.testLookBearing, pmo->X(), pmo->Y(), pmo->Z(),
			pmo->Sector != nullptr ? pmo->Sector->Index() : -1, pmo->Angles.Pitch.Degrees());
		return;
	}
	if (!stricmp(argv[1], "aimwick") && argv.argc() >= 3)
	{
		// The tall wicks' test (roth_gestures.h, CandlesPointGrip): the test ray from the fingertip at wick <n>.
		AActor *pmo = players[consoleplayer].mo;
		DVector3 at;
		if (pmo == nullptr || !CandleWickWorld(P.testCandleSector, P.testCandleIndex, atoi(argv[2]) - 1, pmo->Angles.Yaw.Degrees(), at))
		{ Printf("roth_vrpicktest aimwick: no wick %s on the candle (roth_vrpicktest candle first)\n", argv[2]); return; }
		P.testAimAt = true;
		P.testAim = at;
		Printf("roth_vrpicktest aimwick %s: aiming at (%.1f %.1f %.1f)\n", argv[2], at.X, at.Y, at.Z);
		return;
	}
	if (!stricmp(argv[1], "wick") && argv.argc() >= 3)
	{
		AActor *pmo = players[consoleplayer].mo;
		const int wick = atoi(argv[2]) - 1;
		const double metres = argv.argc() >= 4 ? atof(argv[3]) : 0.0;
		const int testRole = (P.testHands & 1) ? VR_MAINHAND : VR_OFFHAND;
		DVector3 at;
		HandState tmp;
		if (pmo == nullptr || !CandleWickWorld(P.testCandleSector, P.testCandleIndex, wick, pmo->Angles.Yaw.Degrees(), at))
		{ Printf("roth_vrpicktest wick: no wick %s on the candle (roth_vrpicktest candle first)\n", argv[2]); return; }
		if (!P.test || !FingerRay(testRole, tmp, pmo)) { Printf("roth_vrpicktest wick: no fingertip yet (roth_vrpicktest on first)\n"); return; }
		// Toward the player along the view, so <metres> in front of the wick is between it and the eye.
		const DVector3 toward(-pmo->Angles.Yaw.Cos(), -pmo->Angles.Yaw.Sin(), 0.0);
		const DVector3 target = at + toward * (metres * UPM());
		const DVector3 move(target.X - tmp.tip.X, target.Y - tmp.tip.Y, 0.0);
		const double ps = pmo->Level != nullptr && pmo->Level->pixelstretch > 0 ? pmo->Level->pixelstretch : 1.0;
		// The player stands on the floor where he lands (a step there would drop or lift him, and the hand with
		// him, on the next tic); the lift makes up the rest.
		const double oldZ = pmo->Z();
		pmo->SetOrigin(pmo->Pos() + move, false);
		pmo->SetZ(pmo->floorz);
		pmo->Vel.Zero();
		pmo->ClearInterpolation();
		const double dz = target.Z - (tmp.tip.Z + (pmo->Z() - oldZ));
		const double lift = VRHandTest_Lift() + dz * ps / UPM();
		VRHandTest_SetLift(lift);
		Printf("roth_vrpicktest wick %d: object %d/%d's wick at (%.1f %.1f %.1f); the player moved (%.1f %.1f) to sector %d, floor %.0f, the %s hand"
			" lifted %+.1f units (lift %.3f m), so its fingertip is %.3f m from it\n", wick + 1, P.testCandleSector, P.testCandleIndex, at.X, at.Y, at.Z,
			move.X, move.Y, pmo->Sector != nullptr ? pmo->Sector->Index() : -1, pmo->Z(), RoleName(testRole), dz, lift, metres);
		return;
	}
	// roth_vrpicktest texel <face> <x> <y> [metres]: the surface-box check (tools/surfbox_test,
	// tools/claude_bridge/cfg/surfbox_flat.cfg). The player stands square on to Realms face <face>, the eye
	// <metres> plus the test hand's reach in front of the point whose hit texel is (x, y), facing it and
	// pitched at it -- so the flat use key's ray (the view's) and the test finger's ray (turned to the same
	// aim) both meet the face there. Prints the point and the texel the click paths compute back.
	// roth_vrpicktest palm <metres>: after "texel", the test hand's PALM <metres> in front of the aimed point, at its
	// height, HELD there (roth_gestures.h GesturesHoldReach, the gestures' own held reach): the original's movement
	// keeps the player's box 0x1c off the wall and re-takes the pawn after a placement (the owner's PC, 66243f00:
	// the player went back to y 4129 and the palm was 16 units off face 2350), so the hand reaches the rest, every
	// tic until "roth_gesturetest home" or a move. (057c8524: placed by "texel ... 0.08" alone, the palm met the face
	// 28 units above the switch, texel row ~121, outside #385's box, and the grip was rightly refused.)
	if (!stricmp(argv[1], "palm") && argv.argc() >= 3)
	{
		AActor *pmo = players[consoleplayer].mo;
		if (pmo == nullptr || !P.testAimAt) { Printf("roth_vrpicktest palm: aim first (roth_vrpicktest texel)\n"); return; }
		if (!P.test) { Printf("roth_vrpicktest palm: no test hand (roth_vrpicktest on first)\n"); return; }
		const int testRole = (P.testHands & 1) ? VR_MAINHAND : VR_OFFHAND;
		const double metres = atof(argv[2]);
		const DVector3 toward(-pmo->Angles.Yaw.Cos(), -pmo->Angles.Yaw.Sin(), 0.0);   // from the aim back to the player
		const DVector3 want = P.testAim + toward * (metres * UPM());
		GesturesHoldReach(testRole, false, want.X, want.Y, want.Z, P.testAim.X, P.testAim.Y, P.testAim.Z);
		Printf("roth_vrpicktest palm: the %s hand's palm held at (%.1f %.1f %.1f), %.3f m in front of the aim (%.1f %.1f %.1f)\n",
			RoleName(testRole), want.X, want.Y, want.Z, metres, P.testAim.X, P.testAim.Y, P.testAim.Z);
		return;
	}
	if (!stricmp(argv[1], "texel") && argv.argc() >= 5)
	{
		AActor *pmo = players[consoleplayer].mo;
		if (pmo == nullptr) { Printf("roth_vrpicktest texel: no player\n"); return; }
		const int face = atoi(argv[2]), tx = atoi(argv[3]), ty = atoi(argv[4]);
		const double metres = argv.argc() >= 6 ? atof(argv[5]) : 1.0;
		DVector3 at; DVector2 out; int cx = -1, cy = -1;
		if (!FaceTexelWorld(face, tx, ty, at, out, cx, cy)) { Printf("roth_vrpicktest texel: face %d draws no texel %d,%d on a plain piece\n", face, tx, ty); return; }
		const int testRole = (P.testHands & 1) ? VR_MAINHAND : VR_OFFHAND;
		DVector3 spot0;
		VRHandTest_ActorSpot(VRHandPose_SideForRole(testRole), DVector3(0, 0, 0), 0.0, spot0);
		const double back = metres * UPM() + (P.test ? std::max(0.0, spot0.X) : 0.0);
		DVector3 stand(at.X + out.X * back, at.Y + out.Y * back, pmo->Z());
		if (pmo->Level != nullptr)
		{
			sector_t *sec = pmo->Level->PointInSector(stand.XY());
			if (sec != nullptr) stand.Z = sec->floorplane.ZatPoint(stand.XY());   // on the floor where it stands
		}
		pmo->SetOrigin(stand, false);
		pmo->Vel.Zero();
		pmo->Angles.Yaw = DAngle::fromDeg(atan2(-out.Y, -out.X) * (180.0 / M_PI));
		const double eye = pmo->Z() + players[consoleplayer].viewheight;
		pmo->Angles.Pitch = DAngle::fromDeg(atan2(eye - at.Z, back) * (180.0 / M_PI));
		pmo->ClearInterpolation();
		VRHandTest_SetLift(0.0);
		P.testAimAt = true;
		P.testAim = at;
		Printf("roth_vrpicktest texel: face %d texel %d,%d is at (%.2f %.2f %.2f); the click paths compute %d,%d there; standing at"
			" (%.1f %.1f %.1f), %.2f m plus the hand's reach off, facing it, pitch %.1f\n", face, tx, ty, at.X, at.Y, at.Z, cx, cy,
			stand.X, stand.Y, stand.Z, metres, pmo->Angles.Pitch.Degrees());
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

//===========================================================================
//
// roth_l1diff <oracle log> <out file> -- L1 AGAINST THE ORIGINAL (docs/VR_INTERACTION.md 6.1). Test-only, inert until
// typed. Reads the original's eye rays from a rangeprobe log (tools/userange_test/rangeprobe.c: "PK label px py pz pa
// pitch dx dy ..." -- the player's pose and the cursor's offset from the view centre) and puts each, at pitch 0, through
// L1: the player stood at (px, py), the ray from the original's eye (pz + 144) through that pixel of the original's
// measured camera (REMAROTH_MEASURED.md 1: FX 309.77, FY 355.06, centre 320.48, 240.54; angle 512 a turn, counter-
// clockwise from +Y), RulesPick, the record (PickRecord), the class by D2 AND by the original's own measure (z along
// the view yaw), and what a use and an examine there would reach (read only: the record each dispatcher matches, an
// object's pickup). One "L1" line each; tools/userange_test/userange.py l1diff compares them with the original's.
//
//===========================================================================

CCMD(roth_l1diff)
{
	using namespace roth;
	if (argv.argc() < 3) { Printf("usage: roth_l1diff <rangeprobe log> <out file>\n"); return; }
	AActor *pmo = players[consoleplayer].mo;
	if (pmo == nullptr || pmo->Level == nullptr) { Printf("roth_l1diff: no level\n"); return; }
	FILE *in = fopen(argv[1], "r");
	if (in == nullptr) { Printf("roth_l1diff: cannot read %s\n", argv[1]); return; }
	FILE *out = fopen(argv[2], "w");
	const DVector3 home = pmo->Pos();
	const DAngle homeYaw = pmo->Angles.Yaw;
	constexpr double FX = 309.77, FY = 355.06, X0 = 320.48, Y0 = 240.54, kEye = 144.0;
	char line[512];
	int n = 0, skipped = 0;
	while (fgets(line, sizeof line, in))
	{
		if (strncmp(line, "PK ", 3) != 0) continue;
		char label[64];
		int px, py, pz, pa, pitch, dx, dy;
		if (sscanf(line + 3, "%63s %d %d %d %d %d %d %d", label, &px, &py, &pz, &pa, &pitch, &dx, &dy) != 8) continue;
		if (pitch != 0) { skipped++; continue; }
		const double t = pa * (2.0 * M_PI / 512.0);
		const DVector3 fwd(-sin(t), cos(t), 0.0), right(cos(t), sin(t), 0.0), up(0, 0, 1);
		const double sx = (320 + dx + 0.5 - X0) / FX, sy = (240 + dy + 0.5 - Y0) / FY;
		DVector3 dir = fwd + right * sx - up * sy;
		dir /= dir.Length();
		pmo->SetOrigin(DVector3(px, py, pz), false);
		const DVector3 eye(px, py, pz + kEye);
		Pick p;
		const bool hit = PickFrom(pmo, eye, dir, 16384.0, p);
		rules::Record r;
		if (hit) PickRecord(p, r);
		const CursorClass c2 = hit ? ClassifyPick(p, pmo, rules::BodyDepth(px, py, p.hitPos.X, p.hitPos.Y)) : CursorClass{};
		const double zYaw = hit ? (p.hitPos.X - px) * fwd.X + (p.hitPos.Y - py) * fwd.Y : 0.0;
		const CursorClass cy = hit ? ClassifyPick(p, pmo, rules::DepthOfZ(zYaw)) : CursorClass{};
		uint16_t ur = hit ? UseRecordOf(p) : 0, er = hit ? ExamineRecordOf(p) : 0, uk = 0, ek = 0;
		uint8_t uo = 0, eo = 0;
		if (ur) RecordKey(ur, uo, uk);
		if (er) RecordKey(er, eo, ek);
		int ox = 0, oy = 0, oz = 0, item = 0, pickup = 0;
		if (hit && r.type == 4)
		{
			if (const Object *o = RuntimeObject(r.objSector, r.objIndex))
			{
				ox = o->x; oy = o->y; oz = o->z;
				if (TheDbase100().IsLoaded()) item = TheDbase100().FindItemByRenderId((uint16_t)(o->textureIndex | (o->textureSource << 8)));
			}
			pickup = p.actor != nullptr && ObjectIsPickupItem(p.actor) ? 1 : 0;
		}
		FString s = FStringf("L1 %s %d %d %d %d %d type=%u face=%d sec=%d obj=%d.%d door=%d hit=%.1f,%.1f,%.1f objxyz=%d,%d,%d "
			"d2=%.0f c2=%u/%x/%s ex2=%d yaw=%.0f cy=%u/%x/%s use=%u:%02x:%u exa=%u:%02x:%u item=%d pickup=%d\n",
			label, px, py, pa, dx, dy, (unsigned)r.type, r.face, r.sector, r.objSector, r.objIndex, r.door, r.x, r.y, r.z, ox, oy, oz,
			c2.depth, (unsigned)c2.type, (unsigned)c2.shape, rules::ClassName(RulesClassOf(c2)), c2.examinable,
			cy.depth, (unsigned)cy.type, (unsigned)cy.shape, rules::ClassName(RulesClassOf(cy)),
			(unsigned)ur, (unsigned)uo, (unsigned)uk, (unsigned)er, (unsigned)eo, (unsigned)ek, item, pickup);
		if (out != nullptr) fputs(s.GetChars(), out);
		n++;
	}
	fclose(in);
	if (out != nullptr) fclose(out);
	pmo->SetOrigin(home, false);
	pmo->Angles.Yaw = homeYaw;
	pmo->ClearInterpolation();
	Printf("roth_l1diff: %d eye rays through L1 (%d at another pitch skipped) -> %s\n", n, skipped, argv[2]);
}
