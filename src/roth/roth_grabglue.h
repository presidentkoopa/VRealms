#pragma once
//
// RSQ DISTANCE GRAB IN THE REALMS: the glue, and only the glue (roth_grabglue.cpp says what it is).
//

struct FLevelLocals;
struct event_t;

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

// THE MAIN GRIP ON THE SQUEEZE (owner 2026-11-16, "grab on squeeze"). Under vr_secondary_button_mappings (default on)
// the device sends the MAIN grip's key only as a clean tap's one-frame pulse at its RELEASE (vk_openxrdevice.cpp
// suppressGripButton; the grip is the shift layer while held), so the package's walk-up grab with the main hand would
// fire on letting go. VRealms reads the analog grip, so it hands the package the press itself, at the squeeze: the
// package's own network event, exactly what its key path sends (near.zs InputProcess: "rsqdg_ngrip" hand, 1 / 0),
// and swallows the tap's pulse at the release (GrabGlueResponder), so the press is not sent twice. No mapping and no
// setting is changed. Only the main hand, only with secondary mappings on, only with live controllers (the headset or
// the controller-pose test): everywhere else the key comes at the press and the package reads it itself.
bool GrabGlueSqueezeFeeds(int role);
// VRealms' grip edge was the package's (GrabGlueTakesGrip true): send its press now if the key will come late.
void GrabGlueGripPress(FLevelLocals *level, int role);
// The analog grip opened: the release of a press sent at the squeeze.
void GrabGlueGripRelease(FLevelLocals *level, int role);
// From G_Responder, before the event handlers: true = the main grip's late tap pulse for a press already sent, eaten.
bool GrabGlueResponder(const event_t *ev);
// The device's grip key for a role (KEY_PAD_LSHOULDER / RSHOULDER by the physical hand).
int GrabGlueGripKey(int role);

} // namespace roth
