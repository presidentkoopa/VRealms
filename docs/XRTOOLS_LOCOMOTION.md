# Locomotion from Godot XR Tools (started 2026-11-24)

Owner, 2026-11-24: "begin the XR Tools integration"; "are we looking at available open source movement-solutions ...
have we considered roomscale stuff?"; and, on how to use sources: "it isn't about porting one or the other, but what
you are reading in them is the ideas we can use to make ours, it is our custom engine".

Source: Godot XR Tools (MIT, Bastiaan Olij and contributors), `/home/claude/godotvr/godot-xr-tools` at 6c6189b.
Read in full for this list: `functions/movement_direct.gd` (107 lines), `functions/movement_turn.gd` (128),
`player/player_body.gd` (1,093: `_physics_process` 265-357, `_apply_velocity_and_control` 647-750,
`_estimate_body_forward_dir` 768-808, `_update_body_under_camera` 822-1027), `overrides/ground_physics_settings.gd`.
Still to read before their items are built: `movement_crouch.gd`, `movement_sprint.gd`, `movement_jog.gd`,
`movement_climb.gd`, `movement_world_grab.gd`, `effects/vignette.gd`, `effects/fade.gd`.

Underneath, always: the original's collision and steps (`roth_playermove`, `PLAYER_MOVEMENT_PORT.md`, diffed against
the running original). XR Tools decides how the player wants to move; the original decides where he can go.

## The checklist (source -> ours; status)

| # | XR Tools part | source | ours | status |
|---|---|---|---|---|
| 1 | The stick's push is the speed (analog), its angle the direction | movement_direct.gd:94-104 | roth_playermove MoveInput::analog, roth_playermove_engine.cpp PlayerMoveTakeCmd | BUILT 2026-11-24 (the original's acceleration and top speed kept; move_test 14 scenarios 0 differ in flat) |
| 2 | Direction from the HEAD's yaw (the camera's forward flattened), not the body's | player_body.gd:657-664 | the original's `angle` is the pawn's yaw; check that it follows the headset in VR, else use the view yaw | to check |
| 3 | Traction 30/s toward the wanted velocity, drag 5/s to a stop when the stick is let go | player_body.gd:666-672, 691-704; ground_physics_settings.gd:45-48 | the original's own ramp (6 a tick, halved every 5 ticks) is kept: the Realms feel | decided: not taken (the original's) |
| 4 | Snap turn: 20 degrees a step, 0.2 s between repeats while held; smooth turn: 2 rad/s past a 0.1 dead zone | movement_turn.gd:24-35, 85-117 | VR_SetTurnOverride (hw_vrmodes.h), vk_openxrdevice.cpp; roth_vr_turn 1 snap / 2 smooth, the owner's 45-degree snap kept | BUILT 2026-11-24 (log: "VR turn: snap (XR Tools' movement_turn)"); the turn itself only in the headset: not seen |
| 5 | Roomscale: the body stands on the ground under the head, moved there through collision every frame | player_body.gd:977-985 | roth_playermove_engine.cpp roomscale steps through PlayerMove (row 70 of PLAYER_MOVEMENT_PORT) | built before, never seen; check in the headset |
| 6 | The head pushed into a wall: the view FADES to black (head_behavior_mode 1, the default), or the body is pushed back | player_body.gd:987-1027 | none: today the eye can go through a wall | to build: the fade, the default |
| 7 | The eye sits toward the front of the body: body origin 0.5 x radius behind the eye along the body's forward | player_body.gd:963-975 | none | to build with 5 |
| 8 | Body forward = head forward, turned toward the hands' forward by 75 % (at most 60 degrees) | player_body.gd:768-808 | none (the original's angle) | to build with 5 (it decides 2 and 7) |
| 9 | Height from the headset, grown toward a ceiling only as far as a head cast allows | player_body.gd:830-919 | the real crouch (ledger row 32) drives the original's crouch | built (crouch); the head cast: to check |
| 10 | Comfort vignette while moving or turning | effects/vignette.gd:55-119 | not ported | REMOVED 2026-11-24 (owner: "WHO ASKED FOR A VIGINETTE I SAID MOVEMENT") |
| 11 | Sprint, arm-swing jog, climb, world grab | movement_sprint / jog / climb / world_grab .gd | none | later, each read first |

Not taken: Godot's scene nodes, signals, editor warnings, physics-server queries (Jolt or the original's collision do
those here), bounce (`bounciness` 0 by default), slopes (the original has its own step rule).

## Also taken from XR Tools: the pickup

`functions/function_pickup.gd` (556 lines, read whole) -> `roth_vrpick.cpp` THE PICKUP: the closest pickable thing in
the 0.3 m grab sphere, else in the 5-degree cone (as far as the original's use reaches, in sight), highlighted (THE
GLOW), taken by the grip. Seen on the PC 2026-11-24 (log-interact_flat): "closest pickup (XR Tools): in the 0.3 m grab
sphere at 0.12 m from the palm", then "the grip on the closest pickup ... -> acted", the Helm flying to the hand; "in
the 5-degree cone at 1.36 m". Not taken: snap zones, copied collisions, press-to-hold (the grip holds while closed, as
before).

## Proof

Each built row gets a flat bridge run with a log line (`roth_move`), and the owner's headset pass. Row 1: flat
move_test unchanged (14 scenarios, 0 ticks / 0 frames differ, 2026-11-24); its VR behaviour not yet seen.
