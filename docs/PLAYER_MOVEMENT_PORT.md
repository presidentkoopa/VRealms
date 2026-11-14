# The original's player movement: port checklist (acceptance test)

Ported 2026-11-13 from ROTH.C (`/home/claude/oracle_build/rothc/roth_c/src/engine`, the lifted original)
into `src/roth/roth_playermove.{h,cpp}` (engine-free) and wired to the engine's Realms player by
`src/roth/roth_playermove_engine.cpp`. Owner's decision (2026-11-13): "port it now".

Sources: R = renderer.c, PL = player.c, CP = collision_physics.c, D = doors.c, ML = map_load.c,
IN = input.c, WC = weapon_combat.c, GC = game_core.c. Port: PM = roth_playermove.cpp,
PE = roth_playermove_engine.cpp.

Status words: **diffed** = compared against the running original by `tools/move_test` (counts in
section 3); **ported, not diffed** = transcribed, no scenario reaches it; **not ported** (why);
**wired** / **not wired** = the engine binding.

## 1. Checklist

### 1.1 The data
| # | Part | Source | Port | Status |
|---|---|---|---|---|
| 1 | Jump table 0x71304, 21 dwords: 0 9 8 7 7 6 6 5 4 4 4 3 3 2 2 1 1 1 0 0 0 | data image | PM:23-31 `MoveTables::Original` | diffed (TAB jump, 21 values, 0 differ) |
| 2 | Keyboard direction offsets 0x707c9, 16 words (forward 0, right 0x180, back 0x100, left 0x80, diagonals, 0xffff none) | data image | PM:23-31 | diffed (TAB dir) |
| 3 | Walk bob table 0x724f0, 32 words | data image | PM:23-31 | diffed (TAB bob) |
| 4 | Sine table 0x72080 (512 words) | data image | roth_actors.cpp `ActorSin` (reused, not duplicated) | diffed (TAB sin, 512 values) |
| 5 | Held-key table 0x707f1, its order: 50 48 4d 4b 33 34 51 49 47 4f 1e 2c | IN:208-226 | roth_playermove.h `MoveKey`, PM:21 | diffed (TAB keys) |
| 6 | atan2_bearing, isqrt_fixed | R:614-644, R:141-188 | roth_actors.cpp `ActorBearing`, `ActorIsqrt` (reused) | diffed through the slides and push-outs below |

### 1.2 The image the collision walks (the original's memory)
| # | Part | Source | Port | Status |
|---|---|---|---|---|
| 7 | Geometry section es = RAW bytes [0, header+0); vertex section gs = next header+0x14 bytes; objects = the objects section | ML:420-479 | PM:64-81 `BuildImage` | diffed: every byte the collision reads (55,735 in STUDY1) equals the running original's memory, 0 differ |
| 8 | fixup_raw_sectors_after_load: a 0xfffd door's ceiling to +0x10, ceiling = floor | R:10267-10280 | PM:83-91 | diffed (part of 7) |
| 9 | mark_geom_sentinel_entries: solid face +0xa \|= 0x20 | R:1637-1648 | PM:92-100 | diffed (part of 7) |
| 10 | flag_sectors_with_objects: sector +0x16 \|= 2 | ML:133-147 | PM:101-111 | diffed (part of 7) |
| 11 | Live sector heights (+0, +2), platform heights (+2, +8), +0x0c, door bit +0x16&1 | doors, lifts, effects write them | PM `SetSectorHeights` etc.; PE `SyncWorld` from the live map | diffed with the original's live values as input; wired (reasoning only, not seen) |
| 12 | Bytes the collision does NOT read differ at run time and are not reproduced: vertex +0..+7 (renderer scratch), sector +4/+5, +0xa bit 1, +0xb (light), +0x16 bits 2-5, face +0xa bit 0x40, mapping +9 | renderer, scripts | none | not ported: no movement code reads them |

### 1.3 Map tuning
| # | Part | Source | Port | Status |
|---|---|---|---|---|
| 13 | init_movement_tuning_from_first_map: MOVE_SPEED = meta+8 (half-life period), height = 2*meta+0xa (also rest and camera), max climb = 2*meta+0xc + 1, min fit = 2*meta+0xe | ML:197-209 | PM:147-155 `InitTuning` | diffed (INIT msi/climb/fit/rest = the port's for STUDY1) |
| 14 | ...from the game's FIRST map only (startup and new game), kept for the game | GC:467, GC:1071 | PE `PlayerMoveTick` (the first entry of ROTH.RES, once a session) | wired (reasoning only). The retail maps disagree (STUDY1 5/72/32/48, CHURCH1 7/72/32/48, MAZE 9/72/32/24), so this matters |

### 1.4 The 70 Hz tick (player_movement_tick, R:1044-1103)
| # | Part | Source | Port | Status |
|---|---|---|---|---|
| 15 | run_active = run_toggle, inverted by Shift (scancode 36, the bitmap bit 0x90c42&0x40) only while 0x819cd (the crouch mode) is set | R:1046-1050 | PM `Tick` | diffed (strafe.txt holds 48+36 and 48+36+2c) |
| 16 | Gate: movement mode == 1 | R:1053 | PM `Tick` | diffed (mode 1 throughout) |
| 17 | rate_counter--; below 0: reload MOVE_SPEED, zero |v| <= 0x20, halve vx, vy (sar) | R:1055-1071 | PM `Tick` | diffed (every scenario; movespeed.txt with 9) |
| 18 | input bits cleared; clear_819c0_bits (locomotion &= 0xfa); the held keys' handlers in table order | R:1073-1076, IN:208-226, R:9688 | PM `Tick` | diffed |
| 19 | move_input_forward/backward, refused while dialogue busy and gate == 0x6ffff | R:194-208 | PM:165-174 | diffed (freeze.txt: Forward and Back held under the freeze move nothing, Left turns, strafe moves) |
| 20 | strafe right 2, left 8 | R:192-193 | PM:163-164 | diffed |
| 21 | turn_input_left: acc<0 -> 2, acc>=max -> max, else +1; max 8, 13 running; bit 0x10 | R:298-306 | PM:188-196 | diffed (turn.txt) |
| 22 | turn_input_right: acc>=0 -> -2, acc<=-max -> -max, else -1 | R:307-315 | PM:197-205 | diffed (turn.txt) |
| 23 | key_a_jump: bit 1; if grounded and the fall state allows (0; 1 with acc < 5; 2) bit 0x10 | R:592-605 | PM:206-216 | diffed (jump.txt) |
| 24 | key_z_crouch: locomotion \|= 0x24 | R:241 | PM:187 | diffed (crouch16/17.txt, strafe.txt) |
| 25 | look_pitch_up/down (+-2), recenter down/up (+-8 after dumping) | R:212-231, R:513-523 | PM:175-186 | ported, not diffed (no scenario holds them); not wired (the engine's mouse look and the headset own the view pitch) |
| 26 | Keyboard path: speed_accum++ (cap 0x10), rate_counter-- again, offset from table 2, vx += sin(-(angle+off)*2)*6, vy += cos(...)*6 | R:1194-1213 | PM `ApplyMovementInput` | diffed |
| 27 | Idle: speed_accum-- | R:1215-1216 | PM `ApplyMovementInput` | diffed |
| 28 | Turn tail: angle += acc>>1 (and the mirror 0x7e8f4), else acc = 0 | R:1223-1230 | PM `ApplyMovementInput` | diffed |
| 29 | Mouse-steer path (cursor type 1 with relative mode): dx clamped +-0x10 into a smoothing pair, angle -= ; dy clamped +-0x18, speed = \|dy\|>>1, push (sin*-dy)>>2 | R:1152-1192 | PM `ApplyMovementInput` | ported, not diffed (the headless original runs cursor type 0); not wired (the engine has no relative cursor mode; its mouse yaw is folded into the angle instead, item 47) |
| 30 | Enqueue (vx,vy) >> the 0x41 slow shift into the queue the select names; again when running (bit 0x80 or run_active); 16 entries, overflow dropped | R:1084-1101 | PM `Enqueue` | diffed (walk.txt with shift 1 and 2; run in walk/turn/jump/wander) |
| 31 | The 0x41 shift is latched from the request 0x89f54 by tick_world_effects | raw_commands.c:3290 | the command core (roth_cmdcore `speedShift`); PE `SyncWorld` reads it | diffed with the latched value as input (TK sh=); wired (reasoning only) |
| 32 | poll_mouse_input, mouse_edge_latch, the 0x76858 / 0x90bce / 0x90bd2 counters | R:1046, :1051-1052, :1074, :1102 | none | not ported: the cursor's and the clock's, nothing in the movement reads them |

### 1.5 The frame (move_player_with_collision, CP:1677-1796)
| # | Part | Source | Port | Status |
|---|---|---|---|---|
| 33 | Toggle the queue select, take the other queue | CP:1684-1685 | PM `Collide` | diffed |
| 34 | Fixed player dims: box +-0x1c, perp 0xc400, 0xc710, corner 0xc440, radius 0x70; span = min fit; step = max climb, 0x10 while 0x819cd | CP:1687-1702 | PM `Collide` | diffed (step65 climbs, step66 stops at x = 464 = 432+28+4; crouch16/17) |
| 35 | Seed query/restore/saved from the position; ref Z frame[0x1e]; budget 0x100000; flag mask 0x81 | CP:1704-1713 | PM `Collide` | diffed |
| 36 | Per entry: save, add the delta, budget = max((dx>>10)^2+(dy>>10)^2, 0x100000), fire gate 1; collide_substep_track_sector; empty queue = one no-move pass | CP:1714-1747, CP:1486-1495 | PM `Collide` | diffed |
| 37 | find_sector_and_collide: window 0x8000/0x7fff, budget 6, hint -> neighbours -> global (stores 0x90c12 only off the hint path) | CP:1448-1478 | PM `FindSectorAndCollide` | diffed |
| 38 | scan_sector_edges_at_y, search_sector_from_hint / neighbours / global, find_query_sector, locate_sector_at_position | CP:91-162, CP:906-1028 | PM:669-797 | diffed (sector changes in step, door and wander runs) |
| 39 | resolve_collisions_in_sector: gather, walls, objects (POINT branch), budget, repeat while a response asked; restore on a push-out or exhausted budget | CP:1402-1434 | PM `ResolveInSector` | diffed |
| 40 | collide_sector_walls: window from the sector, the platform record +0x18 narrowing | CP:1030-1068 | PM `CollideSectorWalls` | diffed |
| 41 | collide_point_walls_recursive: visited stack (30), portal classification (mask 0x81, stacked alt-Z from +0xc, door sentinels need a door record), box test, perpendicular reject, portal narrowing and recursion, horizontal / vertical / general response with isqrt push-out, segment test, endpoint slide via atan2 + sine table, record | CP:674-900 | PM `CollidePointWalls` | diffed (thousands of wall push-outs, 29 endpoint slides, portal passes; counts in section 3) |
| 42 | crw2_portal_narrow | CP:595-647 | PM `PortalNarrow` | diffed |
| 43 | The installed point-collision callback 0x8c178 | CP:649-660 | none | not ported: the player's query context is 0 (CP:1735), so it never fires for the player |
| 44 | gather_nearby_doors: the current sector and its neighbours: doors (byte +0x16 bit 0) and objects within 0x120 not flagged 0x82 (bit 1), then the creature pool within 0xa0 | D:347-451 | PM `GatherNearby` | diffed (objects; doors: door.txt). The creature pool: ported, not diffed (the scenarios keep away from creatures) |
| 45 | resolve_collisions_against_objects: 4 passes, isotropic / anisotropic boxes from the art's dimensions, the height windows, push-out on the nearer axis | CP:1180-1400 | PM `ResolveObjects`, `PushOut` | diffed (object push-outs and height windows in section 3) |
| 46 | clip_locate_query_to_object / clip_query_circle_to_edge for a door's leaf | CP:210-398 | PM `ClipToDoor`, `ClipCircleToEdge` | diffed (door.txt: the original's own door opener, 3,882 leaf clips, 14 pushing, 0 differ); the engine's leaf corners: wired from the polyobject (reasoning only, corner ORDER unverified, it decides the push side) |
| 47 | fire_tracked_object_trigger for an object with +9 bit 2 under the fire gate | CP:1283-1287, raw_commands.c:2974 | PM calls `PlayerMoveHost::ContactTrigger` | the call: ported, not diffed (no scenario touched one); the trigger itself: not ported (the runtime has no fire_tracked_object_trigger yet), the engine logs it |
| 48 | Final sector (portal cross or restore), sector ambient 0x8c1de / 0x8c1d8, write-back | CP:1749-1786 | PM `Collide` | diffed (position and sector); the ambient: ported, not compared (presentation) |

### 1.6 Vertical physics (update_player_view_bob PL:340-383, update_player_vertical_physics PL:205-329)
| # | Part | Source | Port | Status |
|---|---|---|---|---|
| 49 | fts sub-steps of the vertical physics on the frame's floor/ceiling window; bail on 0 or the 0x80 floor sentinel | PL:342-362 | PM `ViewBob` | diffed |
| 50 | Applied pitch eased toward -0x8497c by up to 3 a step | PL:355-361 | PM `ViewBob` | diffed with 0x8497c as input (the renderer's compute_floor_clearance_for_render, R:7036, not ported: presentation; the engine passes 0) |
| 51 | Forced crouch when dead or headroom < 0x70 | PL:218-222 | PM `VerticalPhysics` | diffed (jump.txt ceiling 130) |
| 52 | Jump: phase 1..0x13 through the jump table, ceiling bonk, apex | PL:224-264 | PM `VerticalPhysics` | diffed (jump.txt: flat, running, bonk under 200) |
| 53 | Crouch: height to 0x30 by 6, feet to floor by 4 | PL:237-243 | PM `VerticalPhysics` | diffed |
| 54 | Fall: start above floor+2 (small <= 0xd, big), drop min(height, acc), acc++ | PL:266-282 | PM `VerticalPhysics` | diffed (fall.txt, step drops) |
| 55 | Landing: (acc-0x16)^2 capped 0x1f40 through apply_direct_damage_to_player; thud when acc > 0xe and a sound is set; settle | PL:283-307 | PM `VerticalPhysics` | diffed (fall.txt: 2 fall damages, health equal every line). The thud: ported, not diffed (0x7fe50 is 0 in STUDY1's new game); not wired (logged) |
| 56 | Settle toward floor and rest height by 4; clamps | PL:309-326 | PM `VerticalPhysics` | diffed |
| 57 | Walk bob into 0x8c112, clamped to floor+0x18 / ceil-0x18 | PL:363-382 | PM `ViewBob` | diffed (cam= in every line) |
| 58 | update_view_transform_params' pitch clamp +-0x7e written back | PL:84-90 | PM `ClampViewPitch` | diffed (run after the probe's line, as in the original) |
| 59 | The camera record (0x89eec) | PL:68-83 | none | not ported: the renderer's |
| 60 | apply_moving_sector_carry / carry_player_span (a moving floor carries the player) | R:969-1036 | PM `CarryPlayerSpan` | ported, not diffed; NOT WIRED: the runtime's movers do not report a per-frame delta yet. On a lift the port's settle (4 a tick) follows the floor instead |
| 61 | Pain flash (0x89f3b) | WC:29-43 | the actors' `DamageDirect` | not compared by the move test (the frame loop decays it before the movement, GC:481-487); health is compared |

### 1.7 The engine binding (PE), VR (owner's decisions 2026-11-13)
| # | Part | Where | Status |
|---|---|---|---|
| 62 | The engine's own thrust, friction, gravity, jump, crouch and step-up do nothing for the Realms player: the command's moves are taken off before the pawn thinks, MF_NOGRAVITY, velocity 0 | p_user.cpp P_PlayerThink -> `PlayerMoveTakeCmd` | reasoning only, not seen |
| 63 | Two 70 Hz ticks and one frame of fts 2 a tic, after the actors (GAME_core.md 2.3 option (a)) | roth_runtime.cpp TickLevelLogic -> `PlayerMoveTick` | seen flat on the owner's PC (367c710c): 161 of 161 tic lines equal move_sched's (section 4) |
| 64 | Flat keys (Up, Left, A and cl_run seen on the PC, section 4; the rest reasoning only): BT_FORWARD/BACK and sticks via forwardmove/sidemove (past 1/8 of full) -> Up/Down/strafes; BT_LEFT/RIGHT -> the turn ramp (the engine's own keyboard turn suppressed, g_game.cpp); BT_JUMP -> A; BT_CROUCH or the engine's crouch toggle -> Z; BT_SPEED -> Shift; cl_run -> the run toggle (read, never written) | PE `PlayerMoveTakeCmd` | reasoning only |
| 65 | Mouse / headset / snap yaw: the engine's yaw change folded into the 512-unit angle with the fraction kept | PE `PlayerMoveTick` | reasoning only |
| 66 | VR locomotion: smooth stick by default (the stick's forward/side -> the original's keys, digital as the original's); teleport as an option (the device's vr_teleport): the target sector's floor, not through the collision | PE `PlayerMoveRenderMove`, p_vrdemo.cpp | reasoning only, not seen in a headset |
| 67 | VR snap turn 45 degrees: `roth_vr_snapturn` (non-archived, default 45) -> VR_SetSnapTurnOverride; the device turns by it (vk_openxrdevice.cpp); reset at every level load (p_setup.cpp) | hw_vrmodes, vk_openxrdevice | reasoning only; hw_vrmodes.cpp and vk_openxrdevice.cpp do not parse standalone in this container, not syntax-checked |
| 68 | VR real crouch: the head below `roth_vr_crouch_fraction` (non-archived, 0.75) of the measured standing head (71d92ff2's VR_MeasuredHeadHeight) holds Z: step 0x10, eye to 0x30, low gaps. A button (the engine's crouch toggle, `lthumb crouch`; BT_CROUCH held) for seated play | PE `PlayerMoveTakeCmd`, hw_vrmodes VR_CurrentHeadHeight | reasoning only |
| 69 | VR jump on a button: BT_JUMP (commonbinds: pad_b +jump) | PE | reasoning only |
| 70 | Roomscale walking moves the playsim: each step through the original's collision as one more queue entry (`AddExtraDelta`, NOT the original's) | PE, p_vrdemo.cpp | reasoning only |
| 71 | Wrap shift (RAQUIA2) and room shift (stacked rooms) taken off and put back; interpolation cleared on a room-shift change or teleport | PE `ToOriginal`, `WritePawn` | reasoning only |
| 72 | Anything else moving the pawn (arrival, warp, savegame, a platform actor pushing): the port takes the pawn as it stands | PE `Resync` | reasoning only |
| 73 | Eye: viewheight = 0x8c112 (height + bob), viewz = z + it | PE `WritePawn` | reasoning only |

### 1.8 New settings (none archived)
- `roth_move_log` (int, 0): one line a tic: position, angle, sector, heights, fall, jump, crouch, keys, health.
- `roth_vr_crouch_fraction` (float, 0.75): the real crouch threshold.
- `roth_vr_snapturn` (float, 45): the snap turn on Realms levels; 0 leaves `vr_snapTurn`.

## 2. The differential test

`tools/move_test/oracle/moveprobe.c` is a ROTH.C mod (overrides player_movement_tick and
move_player_with_collision). It holds scripted scancodes in the original's key bitmap so the
original's own dispatch runs them, logs the player state after every 70 Hz tick and every frame,
and dumps the tables and the original's memory (es, gs, objects, the art's dimensions) at the first
tick. `tools/move_test/move_test` builds the image from the RAW file, compares the tables and the
bytes the collision reads, then replays every tick and frame in the original's order (the frame's
fts from the log) and compares 27 state fields per line. Inputs the movement does not own are taken
from the log: the latched 0x41 shift and the dialogue freeze (per tick), the renderer's 0x8497c (per
frame), the door pool (per frame).

    tools/move_test/oracle/run_all.sh            # every scenario, original then port
    tools/move_test/oracle/run.sh STUDY1 scen out.log; tools/move_test/out/move_test RAW out.log

Scenarios (`tools/move_test/oracle/scenarios`, STUDY1): turn, walk (with the 0x41 shift 1 and 2 and
a running Back), movespeed (MOVE_SPEED 9), strafe (diagonals, Shift, Shift+Z), step65, step66,
crouch16, crouch17, fall (435 and 1200 above the floor), jump (flat, running, bonk, forced crouch),
freeze (acceptance 15), door (the original's door opener, a swinging leaf), wander1/wander2
(3,000 ticks each of random walking, running, turning, strafing, jumping and crouching into walls,
corners and furniture).

## 3. Results

Run of 2026-11-13 (commit eed67866 port, this commit's probe), STUDY1. Every scenario: tables 593
compared, 0 differ; the bytes the collision reads 55,735 compared, 0 differ.

| Scenario | Ticks | Frames | Differ | What it exercised (port counters) |
|---|---|---|---|---|
| turn | 299 | 3,005 | 0 | turn ramp left / right / running left |
| walk | 479 | 5,163 | 0 | walk, glide, 0x41 shift 1 and 2, running Back; 6 sector changes |
| movespeed | 139 | 1,607 | 0 | MOVE_SPEED 9 |
| strafe | 299 | 3,420 | 0 | strafes, diagonals, Shift, Shift+Z; 12 object push-outs |
| step65 | 259 | 2,702 | 0 | climbs 65, drops off; 110 wall push-outs |
| step66 | 259 | 3,302 | 0 | stopped by 66 at x = 464; 178 wall push-outs |
| crouch16 | 299 | 3,348 | 0 | crouched over 16 |
| crouch17 | 299 | 3,449 | 0 | crouched into 17 (see below) |
| fall | 319 | 9,283 | 0 | falls of 435 and 1200; 2 fall damages, health equal on every line |
| jump | 419 | 4,719 | 0 | jump, running jump, bonk under 200, forced crouch under 130 |
| freeze | 199 | 1,755 | 0 | acceptance 15; 47 object push-outs |
| door | 419 | 3,794 | 0 | the original's door opener; 3,793 leaf clips, 14 pushing |
| wander1 | 2,999 | 31,469 | 0 | 1,183 wall push-outs, 10 endpoint slides, 323 object push-outs, 121 sector changes |
| wander2 | 2,999 | 32,280 | 0 | 1,539 wall push-outs, 19 endpoint slides, 747 object push-outs, 123 sector changes |
| **total** | **9,686** | **109,296** | **0** | |

Acceptance 2 under the engine's schedule (2 ticks + 1 frame a tic, `move_sched`): Left held 70 ticks
from the STUDY1 start turns 128 -> 392, +264, as GAME_core.md 10.2 computed.

What the original does that the acceptance tests in GAME_core.md 10 guessed differently:
- crouch17: the original climbs the 17 step while Up and Z are held from the start (z reaches 17
  before it stops at x = 411); the port does the same. Why (reasoning only, not traced): the 0x10 limit is read from 0x819cd,
  which the vertical pass sets after the key. Not a port difference: the port matches it line for line.
- Shift inverts the run toggle only while crouched (R:1048-1049 tests 0x819cd), not "unless
  crouched".

## 4. Not done (named)
- Moving-floor carry (item 60) is not wired; the contact trigger (47) and the landing thud (55)
  are logged, not run.
- The mouse-steer path (29) and the pitch keys (25) are ported but neither diffed nor wired.
- Door leaf corner order in the engine (46) is unverified.
- The engine binding (62-73) has run flat on the owner's PC once (367c710c, move_flat.cfg, cl_run 1):
  all 161 `[roth_move]` tic lines equal `tools/move_test/expected/move_flat_run.txt` (compare with
  `tools/move_test/cmp_pc.sh LOG run`; health is left out, it is the engine's). The expected lines
  come from `move_sched`, which runs the engine's schedule (2 ticks + 1 frame a tic) and takes the
  cfg's roth_at tics: a key pressed by `roth_at N` is first seen at tic N+1 (roth_at runs at the end
  of a tic, after P_PlayerThink read the command) and last at the tic of its release. A one-tic press
  (`+jump` at 120, `-jump` at 121) is seen for exactly one tic, 121, and jumps.
  The positions print through integer rounding (`Fix4`): printf's rounding of an exact tie differs
  between C runtimes (647.15625: glibc 647.1562, MSVC 647.1563).
  Not seen: VR (62-73's headset parts), doors, objects, falls in the engine.
