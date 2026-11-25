#pragma once
//
// THE VR HANDS POINT AND TOUCH (roth_vrpick.cpp has the whole design).
//
// Each hand, every tic in VR: a ray from the index fingertip along the finger,
// classified by the original's cursor rules (roth_runtime.h ClassifyPick), a
// thin beam and a marker coloured by the original's cursor state while the
// hand points, and four gestures that end in the original's own calls:
//   point + dwell         -> examine   (ExaminePick: examine_object_under_cursor)
//   point + index tap     -> use       (Activate: activate_targeted_object's port)
//   fingertip press       -> use
//   grip squeeze in reach -> use
// VR only; the flat game is untouched. A flat test drives it through the
// hand test (roth_vrpicktest, roth_handtest).
//

#include "vectors.h"
#include "roth_vrrules.h"
#include "roth_handstate.h"

struct FLevelLocals;
class AActor;

namespace roth
{

// Once per tic from TickLevelLogic. `modal`: a Realms menu or the death wait
// owns the game this tic, so the hands neither show a beam nor act.
void VRPickTick(FLevelLocals *level, bool modal);

// The level is going: its billboards go with it.
void VRPickEndLevel();

// FOR THE CANDLES (roth_candles.h). While the hands are live (a headset, or roth_vrpicktest): a hand's index
// fingertip and palm in the world, as drawn this tic (FingerRay). False when that hand has none.
bool VRPickFlamePoints(int role, DVector3 &tip, DVector3 &palm);
// The original's use of a Realms object's actor from that hand, at `at`: RulesUse on it. True when the use acted.
bool VRPickUseObject(int role, AActor *target, const DVector3 &at);
// The hands' metres to map units (VR_UnitsPerMeter; 34 in flat).
double VRPickUnitsPerMetre();

// roth_physics.h: the item the game has in `role`'s hand now (the original's selected item, carried in that hand, no
// weapon drawn in it), 0 none; and a buzz on that hand's controller (`why` logged under roth_vrpick_debug).
int VRPickHeldItem(int role);
void VRPickHaptic(int role, double amp, double ms, const char *why);

// FOR THE GESTURES (roth_gestures.h). A hand this tic: its inputs as the picker read them, and its points
// (fingertip, finger direction, palm, the hand's own axis) as drawn (FingerRay). False when the hands are not
// live or that hand has no controller; `havePoints` false when it has no drawn hand.
struct VRPickHand
{
	bool valid = false, gripClosed = false, trigDown = false, pointing = false, triggerTouch = false;
	float grip = 0.f, trigger = 0.f;
	bool havePoints = false;
	DVector3 tip, dir, palm, handFwd;
};
bool VRPickHandNow(int role, VRPickHand &out);
// L1, THE RULES ENTRY (roth_vrrules.h; docs/VR_INTERACTION.md 4). Every VR interaction goes through these four, and
// nothing else gives an item, opens a door or fires a chain from a VR hand:
//   RulesPick      the first Realms thing on the ray from `origin` along `dir` (to `range`, default: the examine
//                  band's edge); false: nothing
//   RulesClassify  the original's cursor on `p` (classify_cursor_target_object), its depth by D2: the horizontal
//                  distance from the player's position to the hit point
//   RulesUse       the original's use (activate_targeted_object) of `p` from that hand, with the item that hand holds
//                  selected (roth_handuse.h), behind the original's click gate; true when it acted. A refusal buzzes
//                  the hand twice.
//   RulesExamine   the original's examine (examine_object_under_cursor) of `p`, behind its gate; true when it acted.
struct Pick;
struct CursorClass;
bool RulesPick(const DVector3 &origin, const DVector3 &dir, Pick &out, double range = 0);
CursorClass RulesClassify(const Pick &p);
rules::Class RulesClassOf(const CursorClass &c);
bool RulesUse(int role, const Pick &p, const char *how);
bool RulesExamine(int role, const Pick &p, const char *how);
// L2, EACH HAND'S STATE (roth_handstate.h; docs/VR_INTERACTION.md 4, L2): one owner per hand per tic, decided at the
// start of the tic by VRPickTick, asked by every owner before it acts.
//   HandOwnerNow / HandStateNow   the hand's owner and state this tic
//   HandMayAct                    may `who` act with that hand this tic (its owner, or nobody's hand)
//   HandEmptyNow                  the hand is empty for the fists (the punch, the knock): nobody holds it
//   HandHoldEndedThisTic          an item's hold ended this tic (the grip opened on it): the world hands' release asks
//   ItemHandRole                  the hand the original's right-hand item is in: the hand a pickup put it in while it
//                                 is the selected item, else the hand on the right side (1.1)
//   VRPickPickupLanded            the fly-in landed in that hand (roth_interface.cpp TickFly)
hands::Owner HandOwnerNow(int role);
hands::State HandStateNow(int role);
bool HandMayAct(int role, hands::Owner who);
bool HandEmptyNow(int role);
bool HandHoldEndedThisTic(int role);
int ItemHandRole();
void VRPickPickupLanded(int role);

// Under roth_vrpicktest (the flat test).
bool VRPickTesting();
// The renderer, once a frame before the effect lights take this frame's (hw_entrypoint.cpp): THE BREATH, a soft amber
// light on the door or switch a hand's reach could use.
void VRPickFrameLights(FLevelLocals *level, double ticFrac);
// VR first: the hands are live (a headset, or the flat test's hands), so the use key uses nothing in the world
// (p_map.cpp P_UseLines); the panels and dialogue still take it.
bool VRPickHandsOwnTheWorld();

// THE HAND'S POINTING RAY, the one every reader uses (2026-11-16): from the drawn index fingertip along the
// drawn finger (FingerRay), else -- no drawn hand -- along the hand's own frame from its grip socket
// (VRHandPose_MapFrame). The panels' pointer (roth_interface.cpp PointerRays) and the runtime's hand use
// (roth_runtime.cpp UseByHand) read this instead of the aim pose's angles. False: no hand this tic.
bool VRPickHandRay(int role, DVector3 &start, DVector3 &dir, bool *fromFinger = nullptr);

// THE DISTANCE GRAB'S CATCH (RSQ Distance Grab, wadsrc_dgrab; the glue in wadsrc/static/zscript/actors/
// realmsgrab.zs): a Realms item that flew into hand `role` is taken by the original's pickup -- the hands' own
// use path (DoUse -> Activate(Use) -> activate_targeted_object's type-4 branch, give_item_by_dbase_id), gated
// by the original's cursor on it from the palm. True when the original took it (the actor is gone).
bool VRPickTakeCaught(int role, AActor *item);

} // namespace roth
