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
// The original's use of a Realms object's actor from that hand, at `at`, through the hands' own use path
// (DoUse: the item the hand holds, the original's cursor gate). True when the use acted.
bool VRPickUseObject(int role, AActor *target, const DVector3 &at);
// The hands' metres to map units (VR_UnitsPerMeter; 34 in flat).
double VRPickUnitsPerMetre();

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
// The original's click on `p` from that hand, gated by the original's cursor on it as seen from `from` (the
// hand's point): use (DoUse: the item the hand holds) or examine (DoExamine). True when it acted.
struct Pick;
bool VRPickUseAt(int role, const Pick &p, const DVector3 &from, const char *how);
bool VRPickExamineAt(int role, const Pick &p, const DVector3 &from, const char *how);
// The original's cursor type on `p` (1 nothing, 3 usable, 5 blocked / attack target), seen from `from`.
int VRPickCursorType(int role, const Pick &p, const DVector3 &from);
// Under roth_vrpicktest (the flat test).
bool VRPickTesting();

} // namespace roth
