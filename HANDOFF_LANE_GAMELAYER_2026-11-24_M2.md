# Handoff: lane/gamelayer, 2026-11-24 (VR physics milestone 2: things worked by hand)

Read CLAUDE.md first, then `docs/VR_HANDWORKS.md` (this milestone, its porting checklist and its proof), then
`docs/VR_PHYSICS.md`, `docs/VR_INTERACTION.md`. The previous handoff (`HANDOFF_LANE_GAMELAYER_2026-11-24.md`) still holds
for milestone 1, the 3D items and the owner's standing wishes (its sections 1, 3, 4).

## 1. What was built

- **Doors in the hand** (`src/roth/roth_handworks.{h,cpp}`): every swinging door of a level. A free hand on the latch
  half takes it. Shut: the leaf holds still until the hand draws it 4 cm along its swing = the original's use on its
  face (once per grip; the chain decides). Open: the leaf follows the hand about its hinge, 0..90 degrees; let go, the
  hand's swing carries on (momentum) and the original's swing finishes the arc. The hand is drawn where it holds.
- **Levers on a hinge** and **any key in any lock** (`src/roth/roth_worldhands.cpp`): the owner's lever table thrown by
  8 cm of arc, the hand held on the arc; every item a use tests in hand (0x27) turned 30 degrees in that lock, the hand
  held there. The bookcases and the pit's chains keep the owner's pull rule.
- **The tracks, engine-free** (`src/roth/roth_track.h`, `tools/track_test`, 52 checks): VRExpansionPlugin's VRLever,
  VRDial, VRSlider and VRButton ported (MIT; the numbered checklist is `docs/VR_HANDWORKS.md` 3). Dial, Slider and
  Button are ported and tested but nothing in the game uses them yet (no drawers or wheels exist; the keys and the hand
  prints keep the owner's tested rules).
- **Engine (general, inert until called):** `src/playsim/po_man.{h,cpp}` EV_HoldPoly / EV_TurnHeldPoly /
  EV_PolySwingState, FPolyObj::bHeldByDriver, DPolyDoor::m_Driven (a driven swing's last step lands on its stop).
- **Physics:** PhysicsHandPin (a hand held on a track); a controller jumping 0.6 m in a tic takes its hand with it.
- **Runtime:** DoorHingeOf, DoorTagOfPick, RecordChain, DoorHold, DoorTurnHeld (`roth_runtime.cpp`, outside the anonymous
  namespaces).
- **Instruments:** `roth_handworks_doors` (a map's door census), `roth_worldtest spot door <tag> [far]`,
  `roth_worldtest spot <record> handle [far]`. Configs `handworks_flat.cfg`, `handworks_key_flat.cfg`,
  `handworks_doors_census.cfg`.

## 2. Verified, and how
Logs beside the exe (`build-dxr\RelWithDebInfo\log-<name>.txt`), captures in the bridge's `logs\` (hw_*.png).
- Doors 5 and 6 (STUDY1) opened by the hand, pushed and pulled, swung, finished at exactly 90 by their own swing
  (log-handworks_flat). Door 4 opened by Key i21 turned in its lock (log-handworks_key_flat).
- MAS4's lever and MAUSO1EA's three levers thrown on their hinge, Key i22 turned in the lock lever (log-worldhands_mas4,
  log-worldhands_mauso). The bookcase still opens by the owner's edge pull (log-worldhands_study1).
- M1 unchanged (log-physics_flat, log-interact_flat). track_test 52/0, worldhands_test 87/0.

## 3. Not run here, and why
- The ROTH.C differential suites (gameproof, dialogue_test, cmdcore_test, actors_test, gdv_test, wallrule) need the
  headless oracle build of the Linux sandbox (`/home/claude/oracle_build`), not on this PC; gameflow_test and menus_test
  link ASan/UBSan, which the PC's 32-bit MinGW lacks; physics_test needs a 64-bit compiler for Jolt's SSE. None of them
  compiles a file this milestone changed (physics_test covers roth_physics_core.cpp, untouched).

## 4. Left undone
1. **A locked door refused through the hand is not seen.** handworks_key_flat B1 (door 2, record #408): the test rig
   jerks the controller 18 cm on the first held tic and the push never reached the latch. Look at roth_worldtest's
   correction (TestPoint's pinned branch) against PhysicsHandPin before blaming the door code.
2. **Closing a door by hand.** The original's close (toggle_door_open_state, input.c:1086-1098) was never ported; a leaf
   pushed back toward shut is released there and the original's swing opens it again. Port the close (with a
   differential test) and give the hinge a shut threshold.
3. Statuette release in worldhands_study1 logs "nothing within 0.05 m -- not set down" while the plates still fire by the
   squeeze; untouched code, not checked against a pre-change build.
4. Levers have no models (their arm length, 20 cm, is a guess); light switches and SALVAT's buttons could use the
   ported Button; the clock could use the ported Dial.
5. Nothing tried in the headset.

## 5. Next (from docs/VR_PHYSICS.md)
Sockets (an item brought to its lock or slot as the original's use: the key rule above is half of it), two hands,
the level's own lying items as bodies, cloth.
