# Handoff: lane/gamelayer, 2026-11-25 (doors, levers and keys worked by hand -- for the owner's local agent)

Read CLAUDE.md first. This is the hand-worked-things part of the lane; the game-logic port (animation, platforms,
PORT_GAPS) is `HANDOFF_LANE_GAMELAYER_2026-11-25.md`, written the same day by another session. Both are current.
Deeper detail: `HANDOFF_LANE_GAMELAYER_2026-11-24_M2.md`, `docs/VR_HANDWORKS.md` (behaviour, the numbered porting
checklist, proof), `docs/VR_PHYSICS.md` (M2 section).

Last commit of this work: `ff7f2d3deb` on `lane/gamelayer`, pushed.

## 0. How the owner wants it done (from this session)
- Ask him, don't search the PC for him. Locating something by crawling his drives for 15 minutes made him angry; one
  multiple-choice question would have taken seconds.
- He is not technical: report at milestones, in plain words, with "verified (how)" separate from "reasoning only".
- Never use screen control on his PC. Test through the bridge (section 3). Never comment on his hours or life.
- Never git stash, never git add -A, never commit a pk3 or anything made from the retail game's art.

## 1. What exists and where
| Piece | Files |
|---|---|
| Doors in the hand: take the latch half of any swinging door; shut, a 4 cm pull/push = the original's use (the chain decides: opens, "Locked.", ...); open, the leaf follows the hand about its hinge, momentum on release; let go within 4 cm of shut = the original's close | `src/roth/roth_handworks.{h,cpp}` |
| Levers on a hinge (owner's lever table, thrown at 8 cm of arc), any key turned in any lock its door tests (0x27). Grip order: key twist, then door, then the owner's pull rule (bookcases, chains) | `src/roth/roth_worldhands.cpp` (TryTwist, WorldHandsGripTaken) |
| VRExpansionPlugin (MIT) VRLever, VRDial, VRSlider, VRButton ported engine-free; plus Slack (ours: a controller pushed past the drawn hand must come back before it moves anything) | `src/roth/roth_track.h`, `tools/track_test/` (59 checks), licence `docs/licenses/VRExpansionPlugin.txt`, source clone `E:\DOOMWork_sources\VRExpansionPlugin` |
| The original's door close ported (toggle_door_open_state): a use on an open leaf closes it; a closing door used reopens; a blocked swing waits | `roth_runtime.cpp` ToggleDoor (INSIDE the anonymous namespace, after SwingDoor); `roth_sound.cpp` SoundDoorToggled |
| Engine, general and inert until called | `src/playsim/po_man.{h,cpp}`: EV_HoldPoly, EV_TurnHeldPoly, EV_PolySwingState, EV_TogglePolySwing, EV_PolySwingWaits, FPolyObj::bHeldByDriver, DPolyDoor::m_Driven / m_WaitIfBlocked |
| A hand pinned on a track | `src/roth/roth_physics.{h,cpp}` PhysicsHandPin; a controller jumping 0.6 m in a tic takes its hand along |

ROTH.C (the specification): `E:\DOOMWork\VRealms\tools\ROTH.C\roth_c\src\engine` (doors.c, input.c, audio.c).

## 2. Verified (flat, owner's PC, through the bridge; logs `build-dxr\RelWithDebInfo\log-<name>.txt`)
- Doors 5 and 6 on STUDY1 opened by the hand, finish at exactly 90 (log-handworks_flat).
- Locked door 2 through the hand: "Locked.", stays shut. Key i21 opens the Study door 4 (log-handworks_key_flat).
- Door 6 closing from 90: 87, 84, 76, 62, 45, 17 at 1, 2, 5, 10, 16, 26 tics, then shut = the original's 2.8125
  degrees a tic. Door 5 closed by hand (log-handworks_close_flat).
- MAS4 lever, MAUSO1EA's three levers, Key i22 in the lock lever (log-worldhands_mas4, log-worldhands_mauso);
  bookcase pull and the statue plates still work (log-worldhands_study1).
- Milestone 1 unchanged (log-physics_flat, log-interact_flat). track_test 59/0, worldhands_test 87/0.

## 3. Testing on this PC
- Bridge: `E:\DOOMWork\tools\claude_bridge_remaroth\CLAUDE_BRIDGE_REMAROTH.ps1 -Once <id>` with a request file
  `requests\<id>.req` (line 1 `run` or `build`, then e.g. `-Map STUDY1`, `-Exec tools/claude_bridge/cfg/<name>.cfg`).
  Output lands in `responses\<id>.out`. Request ids must sort after the existing ones (last used here: `zzzzl74`).
  Runs are silent by default; never add `-Sound` unless asked.
- Door configs: `tools/claude_bridge/cfg/handworks_flat.cfg`, `handworks_key_flat.cfg`, `handworks_close_flat.cfg`,
  `handworks_doors_census.cfg`. Every test cfg sets `roth_vr_units_per_metre 164.3` (not archived) and runs commands
  by `roth_at <tic> <cmd>`.
- Instruments: `roth_handworks_doors` (door census: angle, swinging, held), `roth_handworks_leafuse <tag> [main|off]`
  (the hand's use on a door's leaf), `roth_worldtest spot door <tag> [far]`, `roth_worldtest spot <record> handle [far]`,
  `roth_worldtest hold/grip/move/report`, `roth_xrpose`.
- Before calling anything done: physics_flat, interact_flat, the handworks cfgs, `tools/track_test/run.sh`,
  `tools/worldhands_test`.
- Cannot run on this PC: physics_test (needs a 64-bit compiler for Jolt), gameflow_test / menus_test (need ASan),
  the oracle suites (Linux sandbox). None compiles a file this work changed except as noted in the M2 handoff.

## 4. Traps hit here
- STUDY1 door 4's far side triggers the chapter-2 jump: test free doors on 5 and 6.
- Record #13 spans many faces: use `spot door <tag>`, not the record, to find a handle.
- The Matchbox is drawn in hand by default: never refuse a grip just because an item is drawn.
- The test rig's controller can sit inside a door at the grip; Slack handles it, so a "nothing moved" result usually
  means the hand never reached its latch, not a broken door. Read the latch progress log line (it prints controller
  and drawn palm).
- Python heredocs through Bash turn `\\n` into real newlines inside C strings: write patch scripts to a file.

## 5. Left undone, in the order I would take it
1. The close is checked against the original's arithmetic, not diffed against the running original (needs a ROTH.C
   probe mod in the headless build). The door sounds were never listened to.
2. Statuette: its own "set down" line logs "not set down" while the plates still fire by the squeeze. Check a build
   from before M2 before touching it.
3. Light switches and SALVAT's buttons onto the ported Button; the clock onto the ported Dial.
4. Levers have no models; their 20 cm arm is a guess until a model gives the real one.
5. Nothing has been tried in the headset.
6. After that (docs/VR_PHYSICS.md): sockets, two hands on one thing, the level's lying items as bodies, cloth.
