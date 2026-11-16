#pragma once
//
// RSQ DISTANCE GRAB IN THE REALMS: the glue, and only the glue (roth_grabglue.cpp says what it is).
//

struct FLevelLocals;

namespace roth
{

// Once a tic from TickLevelLogic: which Realms objects the grab may take (ObjectIsPickupItem), made so.
void GrabGlueTick(FLevelLocals *level);

// THE GRIP, ONE MOD PER PRESS (owner 2026-11-16, "map to VRealms grips"). The package's grip keys are VRealms'
// grips (rsqdg_keys_main 445, rsqdg_keys_off 444 436), and its walk-up grip EATS the press (near.zs InputProcess)
// whenever it would take something with it: RSQDG_Near.ready[hand], which its WorldTick sets for a hand on a thing
// its table allows -- in the Realms only an offered item (a RealmsTakeable this glue made takeable) -- or carrying
// a thing the palm caught. VRealms' own grip (roth_vrpick.cpp, the in-reach use) reads the controller's analog
// grip, not the key, so it would act on the same press as well: it asks this first and stands down. True: this
// hand's grip press is RSQ Distance Grab's. role: VR_MAINHAND / VR_OFFHAND (the package's hand 0 / 1).
bool GrabGlueTakesGrip(FLevelLocals *level, int role);

} // namespace roth
