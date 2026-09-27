# ROTH game layer, part 1: core loop, movement, cursor interaction, RNG, command hub

Scope: the game rules in ROTH.C for timing, player movement and collision, input and cursor mode, RNG, and the RAW and DBASE100 command interpreters.
Paths: `E/` = `/mnt/user-data/uploads/VRealms/tools/ROTH.C/roth_c/src/engine/`, `R/` = `/mnt/user-data/uploads/REMAROTH/`.
Every claim has a `file:line` reference. **[inferred]** means I reasoned it from the code; nothing states it outright. **[open]** means ROTH.C as staged does not settle it.

---

## 1. What the player experiences

The game is a first-person walker with a free mouse cursor over the 3D view. Moving the mouse moves the cursor, not the view. Holding the primary button (right, by default) switches the mouse into steer-and-walk mode (E/game_core.c:700-716; E/renderer.c:1150-1195). The cursor changes shape over anything interactive, and the shape says whether the target is in range (E/input.c:1111-1258). A left click on a lit target uses it: a switch, door, item or wall trigger. A right click examines it and usually plays a DBASE100 dialogue or voice record. A left click on nothing, while steering, swings or fires the weapon (E/game_core.c:651-727). The keyboard walks, strafes, turns, looks up and down, jumps (A) and crouches (Z) (E/input.c:124-135). Story progress lives in a global flag bitmap. Inventory, dialogue choices and GDV cutscenes take over the screen modally (E/game_core.c:862-921).

---

## 2. Main loop and timing

### 2.1 Clock
- **Heartbeat: 70 Hz.** The music driver registers its timer event at rate `0x46` = 70 (E/audio.c:1835-1845). Its near-callable body is `vsync_timer_tick` 0x122e3. It does `inc word[0x90bcc]` (g_frame_tick_counter). If `g_player_movement_enabled & 1`, it also runs `update_software_cursor` and `player_movement_tick`, then calls an installed hook (E/renderer.c:36-45). With music off, `install_timer_int8` is used instead (E/game_core.c:1077-1080). Its PIT rate is **[open]**; the same 70 Hz body is near-called either way (E/renderer.c:44).
- **Per-frame step `g_frame_time_scale` (0x85324)** = the 70 Hz ticks since the last render, **clamped to 0x2d (45)**. It is computed inside the renderer's `setup_frame_render_context` (E/render_world.c:73-79). In inventory mode it comes from `update_frame_time_scale`, which spins until at least one tick has passed (E/game_core.c:329-346).
- **Frame pacing:** there is no cap. The loop renders as fast as it can, and every system reads the elapsed tick count.

### 2.2 Top-level structure
`roth_main` (E/game_core.c:1189) → `roth_game_startup` (:1147) → `roth_main_sequence` (:1036), which runs subsystem init, loads DBASE100 (:1060, body :391-434), loads the first map (:1065-1073), installs int9 and the timer (:1074-1080), then calls `game_play_loop` (:1111).

`game_play_loop` (E/game_core.c:742-1028):
- **Outer loop** L17aba (:775): level or restart. It runs the main menu (`run_main_menu`, :807) and handles new game, load and quit.
- **Inner per-frame loop** L17c16 (:846-961). Each iteration:
  1. `service_audio_sequence`, then `voice_stream_pump` (:847-848). These are presentation.
  2. FPS counter (:849-858). Presentation.
  3. Switch on `g_player_movement_enabled` (0x7674a) (:861-870):
     - **1 = gameplay** (:871-880): `gameplay_frame_step`, then `run_gameplay_frame`, then `keymap_dispatch`.
     - **3 = inventory** (:881-895): `update_frame_time_scale`, `handle_cursor_click`, the inventory draw, then `update_inventory_screen`.
     - **5 = dialogue cursor** (:896) falls into **4 = dialogue text and keys** (:898-921). Enter accepts or skips, Up and Down pick a choice.
  4. **Tail** L17d8e (:923-957): the pending-action word `0x7fea8`. Bit 2 = load/save, bit 4 = resolution, bit 8 = restart, bit 1 = **warp or map transition** → `process_map_warp_or_load` (:947-955).
  5. Death check: `health <= 0` exits the inner loop (:959-960). Then comes the death sequence: DBASE100 record 4, a ≤300-tick loop, F9 quickload (:963-1009).

`gameplay_frame_step` (E/game_core.c:478-507), in order:
1. Decay the damage flash by 4×fts (:481-487).
2. **`tick_dynamic_entities`** (:488) = `tick_world_effects` once per frame, then (entity update + actor think) × fts (E/entity_ai.c:1348-1378). `tick_world_effects` (E/raw_commands.c:3287-3339) runs, in order:
   1. latch the speed-shift request;
   2. particles, then the sector damage emitter;
   3. the **water/lava/sector link state machine**, which fires opcode 0x13 triggers;
   4. every **active command effect** through the tick table 0x3088c;
   5. the **timer queue** (opcode 0x3d timers);
   6. the **deferred command queue** (chains released by delay timers and Modify Count).
3. `tick_item_pickup_lock` (:489).
4. Only if mode ∈ {1, 8, 0x20} and no modal block (:490-492): **`update_player_tick`** (:493), then the HUD draw and page flip (:494-506).

`update_player_tick` (E/player.c:445-643) → `update_player_movement` (E/player.c:410-425), in order:
1. `update_turn_view_scale`. Presentation.
2. **`move_player_with_collision`**: horizontal collision, then vertical physics (E/collision_physics.c:1677-1796).
3. `update_view_transform_params`. Camera.
4. `tick_ambient_render_and_map` → `render_world_view_pass` → `setup_frame_render_context`. This recomputes fts, advances the DAS animation tick, and runs **`tick_doors_for_frame`** and `update_active_sounds` (E/render_world.c:73-85, :335-338).
5. `play_nearby_sfx_emitters`.

After that come the weapon fire, cooldown and cycle state machine (E/player.c:459-562) and the viewmodel draw (:564-639).

`run_gameplay_frame` (E/game_core.c:515-732): the cursor state machine. It asks the renderer to pick under the cursor (`render_world_view`, :578) and then classifies, examines, activates or fires (§4).

### 2.3 Which systems depend on frame rate
| System | Time base | Ref |
|---|---|---|
| Keyboard/mouse → velocity, turning | **70 Hz ISR, fixed** | E/renderer.c:38-42, :1044-1103 |
| Horizontal displacement | Consumed per frame from a **16-entry** queue. A run key enqueues each tick's velocity twice, so it holds 8 ticks. Overflow ticks are **dropped** (lost movement below ~4.4 fps walking, ~8.75 fps running) [inferred from :1086-1101] | E/renderer.c:1084-1101, E/collision_physics.c:1714-1747 |
| Vertical physics (jump, fall, crouch) | Loop of fts sub-steps per frame (fixed-step) | E/player.c:342-362 |
| Entity AI | fts iterations per frame (fixed-step, 8-bit counter) | E/entity_ai.c:1356-1378 |
| Command effects (lights, lifts, textures, delays, timers) | Once per frame, **variable step** of fts ticks. Some clamp the step (for example lighting to 0x40) | E/raw_commands.c:1788-1815, 1846-1866, 5328 |
| Doors | Once per frame, scaled by fts, **called from inside the renderer** | E/render_world.c:83 |
| Weapon timers | Per frame, scaled by fts | E/player.c:459-513 |

**Trap:** fts is rewritten during the render pass (step 4 above). `tick_dynamic_entities` and `move_player_with_collision` run earlier in the same frame, so they use the value measured at the **previous** frame's render [inferred from call order, E/game_core.c:488 vs E/player.c:413-419].

**GZDoom mapping:** ROTH's native unit is a 70 Hz tick, and GZDoom runs at 35 Hz. Two options:
- (a) run two ROTH ticks per GZDoom tic, sampling input twice with the same input;
- (b) run the ISR logic at 70 Hz on its own accumulator.

Both give fts = 2 per tic at a fixed tic rate. Under (a), the per-frame systems see exactly fts = 2 every frame, which the original only did at 35 fps. The 45-tick clamp and the 16-entry queue overflow then never trigger. That is acceptable, but it must be documented [inferred].

---

## 3. Player movement and collision

### 3.1 Map-supplied tuning
`init_movement_tuning_from_first_map` (E/map_load.c:197-213) reads the RAW map-metadata block (layout in beyond-the-ire RAW.md "Map Metadata Section"). It runs only for the first map (E/game_core.c:467, :1071). The values persist for the whole game (RAW.md says the same).

| Metadata | Becomes | Ref |
|---|---|---|
| +0/+2/+4/+6 | start X, Z<<16, Y, angle | :200-204 |
| **+8 "MOVE_SPEED"** | `g_move_speed_immediate` 0x12570 = the **velocity half-life period**, not a speed | :205, E/renderer.c:1055-1063 |
| +0xa ×2 | player height (`0x8c110`, resting target `0x8c114`) | :206-207 |
| +0xc ×2+1 | **max step-up** `g_max_climb` 0x8c148 | :208 |
| +0xe ×2 | **min fit** (headroom span) `g_min_fit` 0x90be0 | :209 |

### 3.2 Horizontal input → velocity (70 Hz, `player_movement_tick` E/renderer.c:1044-1103)
Each tick, in order:
1. Poll mouse buttons, then `run_active = run_toggle`. Shift inverts it unless crouched (:1046-1050).
2. **Gate:** `mode == 1`, otherwise return (:1053).
3. `rate_counter--`. If it is < 0: reload it with MOVE_SPEED, zero any velocity component with |v| ≤ 0x20, then **halve** vx and vy (arithmetic shift) (:1055-1071).
4. Clear the input bits, latch the mouse edge, then run the held-key handlers (:1073-1076).
5. **`apply_player_movement_input`** (:1150-1231):
   - Keyboard path (bits `1` forward, `2` right, `4` back, `8` left, `0x10` turn; E/renderer.c:192-210):
     - `speed_accum++` (cap 0x10) and `rate_counter--`.
     - The direction offset comes from table 0x707c9, 16 words: forward 0, right 0x180, back 0x100, left 0x80, diagonals ±0x40, and −1 = none (E/../data/obj3_symbols.h:93-95).
     - `vx += sin(-(angle+off)*2)*6`, `vy += cos(...)*6`. The sine table amplitude is 0x4000, so the impulse is 1.5 units/tick in 16.16 (E/renderer.c:1196-1211; table step 0xc9 per entry, E/../data/obj3_symbols.h:173) [amplitude inferred].
   - Mouse-steer path (cursor type 1 with relative mode on): dx, clamped to ±0x10 and smoothed, turns the player; dy, clamped to ±0x18, pushes along the facing by `(sin·-dy)>>2` (:1152-1192).
   - Turn tail: `angle += turn_accum>>1` (:1223-1230).
6. Enqueue (vx, vy), right-shifted by the opcode-0x41 slow shift. Enqueue a **second time when running** (:1084-1101).

**Angle units:** 0x200 per turn. The fire_sector_trigger direction mask uses `angle & 0x1ff` (E/raw_commands.c:2878-2880).

**Turning** (E/renderer.c:298-315):
- left: accum = 1, 2, … up to 8 (13 when running), or snaps to 2 if it was negative;
- right: snaps to −2, then −3 … down to −8 (−13 when running);
- angle += accum>>1 each tick, so left and right are **asymmetric**.

Pitch: ±2 per tick, clamped to ±0x7e (E/renderer.c:212-224, E/player.c:88-96).

**Worked result** (my transcription of :1055-1101, MOVE_SPEED = 5, rate counter starting at 5 [assumed]):
- forward from rest, with no collision: velocity saw-tooths between 3 and 6 units/tick;
- displacement 234.0 units in 35 ticks and 496.5 in 70 ticks, doubled when running.

So GZDoom's friction and thrust model **cannot** reproduce this exactly: a half-life decay every (MOVE_SPEED+1)/2 ticks gives a saw-tooth velocity. **Port it** [inferred].

### 3.3 Horizontal collision (per frame, E/collision_physics.c:1677-1796)
Player collision is fixed: box ±0x1c, perpendicular threshold 0xc400 = 64·28², radius 0x70 (:1687-1692).
- **Step-up limit** = g_max_climb, or **0x10 while crouched** (`0x819cd`) (:1698-1701).
- **Headroom span** = g_min_fit (:1693).

Each queued delta is applied as one sub-step through `collide_substep_track_sector` → `find_sector_and_collide` (:1448-1478). That function locates the sector (hint, then neighbours, then a global scan; :983-996) and runs `resolve_collisions_in_sector` (:1402-1434):
- **point sweep** `collide_point_walls_recursive` (:674-900): push-out and slide along walls, with a sqrt push-out and an endpoint slide (atan2 plus sin/cos). Portals are crossed only when the vertical span passes (`crw2_portal_narrow`, :595-647);
- then up to 4 push-out passes against objects (`resolve_collisions_against_objects`, :1213-1400);
- iteration budget 6 (:1455).

After the queue:
- final sector (:1752-1765);
- sector light (:1767-1781);
- write-back, then **vertical physics** `update_player_view_bob(frame)` (:1794).

Other movers (entities, projectiles) use `sweep_move_with_collision` in RAY mode, with 1/2/4/8 sub-steps chosen by magnitude thresholds 0xc/0x18/0x30 (:1556-1675).

### 3.4 Vertical (per tick, `update_player_vertical_physics` E/player.c:205-329)
- **Jump** (A, E/renderer.c:592-605): the jump phase runs 1…0x13 and adds `gravity_table[phase]` from table 0x71304 (E/player.c:247-264). The table values are **[open]**; dump them from the data image. A ceiling bonk ends the jump.
- **Crouch** (Z sets flags 0x24, E/renderer.c:248): the height approaches 0x30 by 6 per tick (E/player.c:237-241). Crouch is forced when headroom < 0x70 (:221-222) or when dead (:218-219).
- **Fall:** starts when more than 2 above the floor. It is small if ≤ 0xd, otherwise big (:267-270). Each tick the drop is `min(height above floor, acc)` and acc++, which gives an accelerating fall (:272-282).
- **Landing:** if acc > 0x16, damage = (acc−0x16)² capped at 0x1f40 (:286-291). A landing thud plays if acc > 0xe (:292-295). Default health is 0x800 (E/game_core.c:769-772).
- **Settle:** when idle, position approaches the floor by 4 and height approaches its rest value by 4 (:311-314). Clamp: `ceil − 0x1c` (:215, :316-325).
- **View bob** from table 0x724f0 × speed>>4, clamped to floor+0x18 and ceil−0x18 (E/player.c:369-383). This is presentation, but it reads game state.
- **Moving-floor carry:** `apply_moving_sector_carry` (E/renderer.c:1000-1036).

**Verdict:** port movement and collision 1:1 as a custom player-movement component. GZDoom's movement model does not reproduce the queue, half-life decay, turn ramp, jump table, step and fit rules, or push-out math. Use GZDoom only for the camera [inferred].

---

## 4. Input modes and the cursor model

### 4.1 Modes (`g_player_movement_enabled` 0x7674a)
| Value | Meaning | Set at |
|---|---|---|
| 0 | off, between loops | E/game_core.c:804, :1012 |
| 1 | gameplay | :844, E/inventory.c:1674 |
| 3 | inventory panel | E/inventory.c:1865 |
| 4 | dialogue/text screen | E/menu_hud_ui.c:1215 |
| 5 | dialogue cursor after a GDV | E/gdv_cutscene.c:2421, :2474 |
| 8 | transitional (load, restart) | E/game_core.c:762, :777 |
| 0x11 | modal menu box | E/menu_hud_ui.c:1302 |
| 0x20 | dead | E/game_core.c:965 |

- GDV playback is synchronous inside `play_record_gdv_cutscene`, called from DBASE100 op 0x07 (E/dbase100.c:922-940) [inferred: blocking].
- **Dialogue freeze:** while `dialogue_busy` and `freeze_gate == 0x6ffff`, forward and back are ignored but turning still works (E/renderer.c:198-210).

### 4.2 Buttons
INT 33h bit0 is left and bit1 is right. **Swap is on by default** (`set_default_mouse_button_swap`, E/game_core.c:747, E/renderer.c:9712). With swap on, **right = primary** (0x7e938) and **left = secondary** (0x7e939) (E/input.c:848-873). A key toggles swap (E/input.c:488-493).

### 4.3 Cursor state machine (`run_gameplay_frame`, E/game_core.c:515-732; type byte 0x7e932)
- **No button:** pick under the cursor (`render_world_view`, :578), then `classify_cursor_target_object` (E/input.c:1111-1258). This sets the type and the cursor shape:
  - 1 = nothing;
  - 3 = usable (in range);
  - 5 = blocked or keyed.
- A top-left corner hover (x, y < 0x20) sets type 3 and opens the inventory (E/game_core.c:521-527, :660-663).
- **Type 3, primary (right) click → examine** `examine_object_under_cursor` (E/game_core.c:651-672; E/input.c:979-1024).
- **Type 3, secondary (left) click → becomes type 4 → use** `activate_targeted_object` (E/game_core.c:604-607, :686-699; E/input.c:1026-1100).
- **Type 1 with primary held → steer mode**: relative mouse drives movement. Within 12 ticks, both buttons → type 2 (look/pitch). A secondary click while steering fires the weapon once per press (E/game_core.c:700-731).
- **Type 5 + secondary → aimed attack at the cursor** (:673-685).

### 4.4 Picking
The target comes from the **renderer**: `render_world_scene` is re-run with the cursor pixel and records the first opaque texel. A **transparent sprite texel is not a hit** (E/renderer.c:3261-3283, :4480-4500).

Record at 0x90a48:
- +1 = hit type: 1 ceiling, 2 floor, 3 wall face, 7/8 mid-platform under/top (E/renderer.c:8962-9090); 4 object; 6 door (E/input.c:1036, :1087) [labels partly inferred];
- +8 face offset;
- +0xa sector;
- +0xe object/def pointer;
- +0x1c door id;
- +0x20 **depth metric** (the perspective step; E/renderer.c:3283).

**Reach bands on +0x20** (E/input.c:1031, :1112-1258):
- **< 0x258 (600) = use range**;
- < 0x7d0 (2000) = "too far" cursor (0x398/0x3a0);
- < 0x1388 (5000) = the examine-trigger probe range.

The depth units are **[open]**: they are the projection's perspective step, not world distance. For VR, replace the pick with a ray cast that yields the same record fields. The 600 threshold must be recalibrated to world units by measuring in ROTH.C [inferred].

### 4.5 What a click fires
- **Left (use)**, `activate_targeted_object`:
  - type 4 object: keyed objects need a category-1 command. Otherwise `run_leftclick_object_trigger` (opcode 0x08 runs the chain; 0x02 is a light switch plus SFX plus toggling obj+4; E/raw_commands.c:2920-2965). If that returns nothing, try `give_item_by_dbase_id` (pickup) and destroy the entity (E/input.c:1053-1085).
  - type 6 door: `toggle_door_open_state` (:1087-1098).
  - anything else: `fire_object_use_trigger` → `dispatch_entry_command_trigger` (E/raw_commands.c:2787-2807, 3010-3100):
    - wall face (type 3) → **0x18** records matched by face-group id (E/raw_commands.c:3018-3057);
    - floor, or a mid-platform top (type 2 or 8) → **0x19** records matched by the sector command ID. Record flag bit0 selects the platform-top instead of the sector floor.
- **Right (examine)** → `dispatch_entry_command_trigger_b` (E/raw_commands.c:3102-3200):
  - face → **0x32**;
  - object → **0x30**;
  - floor/ceiling/platform → **0x31**, with the surface mask {floor 1, ceiling 2, platform-top 4, other 8}.
  - On a hit: eval the record's DBASE100 dialogue (`[rec+0xa]`), then fire the chain (`examine_world_object`, E/input.c:953-977). Otherwise, for an object, the item's DBASE100 **OnInspect** block (E/input.c:1000-1021).
- **Keys:** Enter = accept a dialogue choice or skip the voice (E/input.c:718-757). **Space = `dev_open_nearest_door`**, which is in the retail keymap (E/input.c:185-194).

---

## 5. The command interpreter as the hub

### 5.1 RAW execution model
- **Record:** +2 modifier (0x08 = spent/skip, 0x20 = registered, 0x01 = armed, 0x02 = toggle state, 0x04 = connected-flood), +3 base, +4 NEXT (or a trigger's START), +6/+7 flags, +8 key, +0xa…args (R/ROTH_COMMANDS.md "Record layout"; E/raw_commands.c:1255-1275).
- **Trigger firing** (for example `fire_sector_trigger`, E/raw_commands.c:2867-2909):
  1. check the direction mask;
  2. latch the SFX word `[rec+0xa]`;
  3. **flow pre-pass** `walk_command_chain_flow` (:1094-1130). A chain containing an **armed Delay Timer**, or a Modify Count whose sub-chain is blocked, **does not fire at all**;
  4. `reset_command_chain_state` → exec loop, then `process_deferred_command_queue`;
  5. apply the one-shot (0x10) and latch flags (0x20, 0x40).
- **Exec loop** (:1229-1316): dispatch `table[base & 0x7f]` at 0x30780 (67 entries, opcodes 0x00-0x42) and OR the results. It stops on:
  - a handler setting the interrupt;
  - **a base byte == 0x12 (unmasked)**;
  - NEXT == 0.

  Interrupt == 2 with a latched index (set by 0x38 on the previous record) **re-runs from that index**, which is the else-branch (:1280-1287).
- **Post-chain tail:**
  - play `[trigger+0xa]−1` if the chain acted;
  - if the head is an 0x08 object record, flip the object's state word bit0, which is how levers toggle (:1301-1310);
  - `finish_dialogue_record_eval` (:1288-1316).
- Three consecutive 67-entry tables:
  - exec **0x30780**;
  - per-frame tick **0x3088c**, indexed by the effect chunk's stamped base (E/raw_commands.c:3314-3317);
  - load-time init **0x30998** (E/map_load.c:1088-1098; `init_loaded_object_table` :1134-1196; re-visits suppress init via `set_state_record_count(handle)`, E/map_load.c:651-660).

### 5.2 The 15 trigger categories
They are object-table header `{offset, count}` pairs at `hdr + 0x08 + 4·i` (RAW.md "Entry Command Counts"). The consumer and firer of each is taken from code; this **corrects R/ROTH_COMMANDS.md and R/src/roth/roth_runtime.cpp:54-55**.

| # | hdr | Opcode | Fires when (firer) | Load marker |
|---|---|---|---|---|
| 1 | +0x08 | 0x08 / 0x02 | **Left-click object**, by object id (`run_leftclick_object_trigger` E/raw_commands.c:2920; `find_unflagged_object_by_key` E/renderer.c:11078). DBASE100 op 0x36 fires these by id (`run_object_commands_by_id` :1735) | 0x02 → `tick_cmd_45` light pulse (E/raw_commands.c:1868) |
| 2 | +0x0c | none | unused (RAW.md); `init_loaded_object_table` pass 1 walks it (E/map_load.c:1150) **[open]** | |
| 3 | +0x10 | 0x03 | No firer found. The init entry runs the floor/ceiling mover at load (`tick_move_floorceil` E/raw_commands.c:5933) **[open]** | |
| 4 | +0x14 | 0x13 | **Enter/leave sector** (`twe_link_state` → `fire_sector_trigger`, E/raw_commands.c:3202-3270; lookup `find_object_record_by_id` :5398). Variants: plain, Z-below-floor (water), linked platform | sector +0x17 \|= 0x80/0x40/0xc0 (:4994-5011) |
| 5 | +0x18 | 0x18 | **Left-click wall face** (dispatch type 3, E/raw_commands.c:3018-3057) | texmap +9 \|= 1 |
| 6 | +0x1c | 0x19 | **Left-click floor / platform top** (type 2 or 8, :3058-3091) | sector +0x16 \|= 0x10 |
| 7 | +0x20 | 0x1a | **Attack/projectile hits wall face** (`collide_ray_walls_recursive` E/collision_physics.c:556-568 → `fire_wall_object_trigger` E/raw_commands.c:1762) | texmap +9 \|= 2 |
| 8 | +0x24 | 0x1b | **Attack hits object**, or entity death/pending (E/collision_physics.c:1155-1162; E/raw_commands.c:2809-2830 → `begin_object_command_chain` :1717) | obj +9 \|= 0x20 |
| 9 | +0x28 | 0x25 | **Animated texture wraps a frame** (`texture_anim_command_hook` E/raw_commands.c:1351-1406, run from the renderer). **Not inert**, contrary to ROTH_COMMANDS | |
| 10 | +0x2c | none | unused | |
| 11 | +0x30 | 0x32 | **Right-click (examine) wall face** (`dispatch_entry_command_trigger_b` ch1, E/raw_commands.c:3110-3144) | texmap +9 \|= 4 |
| 12 | +0x34 | 0x31 | **Right-click floor/ceiling/platform**, with the surface mask (ch3, :3165-3196) | sector +0x16 \|= 0x20 |
| 13 | +0x38 | 0x30 | **Right-click object** (ch2, :3145-3164). The marker that ROTH_COMMANDS left unresolved is read by this channel and the cursor classifier (E/raw_commands.c:456-472) | obj +9 \|= 8 |
| 14 | +0x3c | 0x37 | **Texture-id remap**: the renderer swaps a texture id per context and fires the chain on a context change (`texture_id_remap_hook` E/raw_commands.c:1410-1450) | |
| 15 | +0x40 | 0x39 | **Player walks into object** (point collision, E/collision_physics.c:1280-1288 → `fire_tracked_object_trigger` E/raw_commands.c:2974) | obj +9 \|= 4 |

Other entry points:
- **0x3d** is a repeating timer registered at load (init `tick_register_timed_effect` E/raw_commands.c:5381-5395). It counts down by fts in `tick_command_timer_queue` (:1846-1866) and fires through `fire_queued_command` (:5223).
- The **Delay Timer 0x12** resumes its chain through the deferred queue (`tick_delay_timer` :5323-5350).

**Corrections to R/ROTH_COMMANDS.md and the runtime:**
- 0x18 and 0x32 are **click-on-wall** (use and examine), not enter-sector.
- 0x19 and 0x31 are **click-on-floor** (use and examine), not use-wall.
- 0x1a is **attack-hits-wall**, not bump. The player's POINT sweep never fires it (E/collision_physics.c:1402-1434 vs :556).
- The enter-sector trigger is **0x13**.

### 5.3 RAW opcode table (exec table 0x30780; base, handler, system)
`E/rc` = E/raw_commands.c. "Tick" = the handler in the per-frame table.

| Op | Handler (line) | What it does | System |
|---|---|---|---|
| 0x00 | [open]; probably `cmd_default_nop` E/rc:5379 | nothing | other |
| 0x01 | [open] "Empty (no SFX)" per RAW.md; probably nop, returns 0 so no chain SFX | nothing | sound |
| 0x02 | `cmd_light_switch` E/rc:4331; tick `tick_light_switch` :1816 | toggle a connected light group's brightness ramp. Also an entry: left-click object | texture/visual (light) |
| 0x03 | `cmd_modify_sector` E/rc:4953; tick `tick_modify_sector` :2590 | floor/ceiling ramp as for 0x07; SFX key at +0x14 | door/lift (geometry) |
| 0x04, 0x05 | [open] no handler identified | | |
| 0x06 | `cmd_06_empty_noop` E/rc:499 | returns −1, so chain SFX plays | sound |
| 0x07 | `cmd_change_height` E/rc:4483; tick `tick_change_height` :2448 | register or toggle a floor/ceiling height ramp; optional SFX node | lift/geometry |
| 0x08 | entry (left-click object) | the post-chain tail flips obj+4 bit0 (E/rc:1301-1310) | trigger |
| 0x09 | `cmd_move_sector` E/rc:4588; tick `tick_moving_sector` :5869 | move a sector's vertices (platform) | geometry |
| 0x0a/0x0b | `cmd_change_floor_texture` E/rc:4356; ticks :2257/:2271 | animate a floor texture | texture |
| 0x0c | `cmd_change_face_texture_adv` E/rc:4721; tick :2070 | wall texture change: mid/upper/lower, shifts, auto-revert | texture |
| 0x0d | `cmd_change_object_texture` E/rc:4520; tick :2087 | change an object's sprite/texture, now or timed | texture/object |
| 0x0e | `cmd_scroll_sector_texture` E/rc:4371; tick :1976 | scroll a floor texture | texture |
| 0x0f | `cmd_scroll_face_texture` E/rc:4837; tick :1944 | scroll a wall texture | texture |
| 0x10 | `cmd_activate_sfx_node` E/rc:4855 | toggle ambient SFX nodes on or off | sound |
| 0x11 | `cmd_flash_lights` E/rc:4453; tick :3515 | flash a face group's light | visual |
| 0x12 | `cmd_delay_timer` E/rc:4420; tick `tick_delay_timer` :5328 | **stops the chain**; resumes NEXT after N ticks (optionally random, RNG 0x71f48) | flow |
| 0x13 | entry (enter/leave sector) | init marks sectors (E/rc:4994) | trigger |
| 0x14 | `cmd_sync_facegroup_texture` E/rc:1032 | set a face group's texture to reference+1 (unused in retail) | texture |
| 0x15 | `cmd_count` E/rc:216 | loop counter; interrupt = 1 re-pumps each frame | flow |
| 0x16 | `cmd_spawn_object` E/rc:622 | wake a placed actor or toggle an object's hidden bit | monster/object |
| 0x17 | `cmd_toggle_command` E/rc:541 | enable or disable another command by index | flow |
| 0x18, 0x19, 0x1a, 0x1b | entries | see §5.2 | trigger |
| 0x1c | `cmd_cycle_texture` E/rc:973 | step a sector/face texture to reference+1 | texture |
| 0x1d | `cmd_change_lighting` E/rc:4390; tick :1788 | sector brightness ramp | visual (light) |
| 0x1e | `cmd_modify_count` E/rc:4001 | add or subtract on a Count command; queue its NEXT | flow |
| 0x1f | `cmd_texture_change_count` E/rc:710 | count that repaints cell textures | texture |
| 0x20 | `cmd_cycle_object_texture` E/rc:1184 | step an object texture | texture |
| 0x21 | `cmd_animate_facegroup_texture` E/rc:945 | count-driven face animation (unused) | texture |
| 0x22 | `cmd_count_addl_arg` E/rc:800 | count that writes object slots | flow/object |
| 0x23 | `cmd_change_object_height` E/rc:4554; tick :2357 | raise or lower objects | object |
| 0x24 | `cmd_rotate_object` E/rc:241; tick :2289 | rotate an object, stepwise or to face the player | object |
| 0x25 | entry (texture-anim frame) | see §5.2 #9 | trigger |
| 0x26 | `cmd_set_flag` E/rc:592 | set, clear or toggle a DBASE100 flag bit | **flag** |
| 0x27 | `cmd_if_not_item` E/rc:4760 | branch on having an item or item list (inverted by flag bit0) | inventory/flow |
| 0x28 | `cmd_if_not_flag` E/rc:4665 | branch on a flag. On fail: interrupt 2, and run the DBASE100 record latched by 0x36 (:4645-4657) | flag/flow |
| 0x29 | `cmd_give_item` E/rc:4799 | give an item, optionally at a drop position from a wall | **inventory** |
| 0x2a | `cmd_remove_item` E/rc:4918 | remove an item (0 = the last touched) | inventory |
| 0x2b | `run_command_dbase100_record` E/rc:4698 | run DBASE100 record `[+8]` (dialogue, voice, GDV, items…) | **dialogue/cutscene** |
| 0x2c | [open]. `cmd_toggle_command` treats 0x06/0x2c specially (E/rc:536-540) | | |
| 0x2d | `cmd_particle_effect` E/rc:4885 | particle burst at an object or edge (rng_next) | visual |
| 0x2e | `cmd_smash_face_texture` E/rc:361 | toggle the "broken" variant bit on a face | texture |
| 0x2f | `cmd_open_door` E/rc:4053 | `register_door_swing` on a face (key or the last used) | **door** |
| 0x30, 0x31, 0x32 | entries | see §5.2 | trigger |
| 0x33 | `cmd_apply_damage` E/rc:4245 | radius damage to the player, from an object or wall | player |
| 0x34 | `cmd_change_face_texture` E/rc:396 | set a face texture and flip the paired face's transparency | texture |
| 0x35 | `cmd_face_emits_damage` E/rc:4282; init `tick_spawn_damage_emitter` :5511 | hazard wall damages the player when near | player |
| 0x36 | `cmd_dbase100_if_next_fails` E/rc:514 | latch a DBASE100 record to run if the next condition fails | dialogue/flow |
| 0x37 | entry (texture remap) | see §5.2 #14 | trigger |
| 0x38 | `cmd_jump_if_next_fails` E/rc:41 | latch the else-index for a re-run | flow |
| 0x39 | entry (walk into object) | | trigger |
| 0x3a | `cmd_change_object_id` E/rc:479 | re-key the source object (0xffff mints a new id) | object |
| 0x3b | `cmd_map_transition` E/rc:525 | latch map name and arrival id; set pending bit 1, handled at the frame tail | **map transition** |
| 0x3c | `cmd_spawn_object_adv` E/rc:4108 | spawn an object or projectile (65535 = at the player) | monster/weapon |
| 0x3d | entry (repeating timer) | §5.2 | trigger |
| 0x3e | `cmd_empty_allow_sfx` E/rc:506 | returns −1 | sound |
| 0x3f | `cmd_player_rotation` E/rc:565 | turn the player, optionally random or scaled by fts | player |
| 0x40 | `cmd_run_indexed_object_command` E/rc:3955 | run a sub-chain inline | flow |
| 0x41 | `cmd_set_player_speed_reduction` E/rc:444 | slow the player: velocity >>= n | player |
| 0x42 | `cmd_set_inventory_filter` E/rc:4940 | hide or unhide inventory (display filter, nothing removed) | inventory |

The tick-table index is inferred from the stamped base; 16 distinct tick targets are noted at E/rc:2849. The per-opcode tick assignment above is by name **[inferred]**.

### 5.4 DBASE100 interpreter (`execute_dbase100_chain` E/dbase100.c:607-953)
- Record word = `{bit31 If-NOT, (w>>24)&0x7f opcode, w&0xffffff operand}`. Records with operand 0 are skipped (:627-633). Opcodes outside {listed} are skipped (:641-651).
- **Flags bitmap = the same one RAW uses** (op 0x04 and op 0x01/0x0d vs RAW 0x26/0x28).
- Entry points:
  - `eval_dialogue_record_by_id` (:545);
  - item OnInspect (E/input.c:1000-1021);
  - game start (records 1 and 3), quit (2), death (4) (E/game_core.c:757, 802, 825, 971).

| Op | What (line) | System |
|---|---|---|
| 0x01 | if flag (If-NOT inverts), else **terminate** (:803-808) | flag |
| 0x02 | if inventory count > n, else terminate (:811-818) | inventory |
| 0x03 | if count == n (:821-832) | inventory |
| 0x04 | set flag / clear flag with the NOT bit (0x84) (:761-765) | flag |
| 0x05 | open a dialogue text window, re-queued if busy (:853-864) | dialogue |
| 0x07 | play GDV cutscene #operand (:922-942) | cutscene |
| 0x08 | choice line; builds the choice menu; a single choice auto-runs (:665-733) | dialogue |
| 0x0a | terminate with success (:740-742) | flow |
| 0x0b | random sub-chain, via `rng_next_index_for_count` (:745-758) | flow/RNG |
| 0x0d | if flag, then skip the next record (:655-662) | flag |
| 0x0e | show a fullscreen image ("The End") (:909-919) | cutscene/UI |
| 0x10 | count and continue (:735-737) | flow |
| 0x11 | give item / remove item (NOT, 0x91) (:768-772) | inventory |
| 0x19 | play SFX with operand−1 (:846-850) | sound |
| 0x1a | load a DBASE300 chunk: speech/music sequence (:787-800) | music/voice |
| 0x1b | stage the pending topic (:775-779) | dialogue |
| 0x1c | gate on the caller flag bit0 (0x9c = exit) (:866-871) | flow |
| 0x1d | jump to another DBASE100 record (:874-885) | flow |
| 0x23 | **run a RAW chain by command index** (`reset_command_chain_no_source`) (:835-838) | **RAW call** |
| 0x26 | set health / heal; ≤0 kills or opens the menu (:900-907) | player |
| 0x2d | sub-codes 4 and 7: UI state (:887-898) | other |
| 0x36 | **run a RAW category-1 object chain by id** (:840-844) | **RAW call** |
| 0x37 | add to the value-reduction factor 0x81e30 (:782-784) | player |

- **Not executed here:** the weapon, monster and bullet definition opcodes (0x12-0x18, 0x1f-0x21, 0x24-0x2e, 0x30-0x35, 0xa7…) listed in DBASE100_commands.md. `build_entity_def_record` consumes them (E/dbase100.c:117-188) **[inferred]**; they belong to other readers' systems.
- **Mutual calls:** RAW → DBASE100 through 0x2b, 0x36 (+0x28/0x27 fail) and 0x26/0x28 (flags), and 0x29/0x2a/0x27 (items). DBASE100 → RAW through 0x23 (by index) and 0x36 (by object id).
- **Deferral:** actions hit while a dialogue or cutscene is busy are re-queued. The queue holds 8 entries; it rejects new entries once 8 are queued (E/dbase100.c:437-446).

---

## 6. Persistent game state

- **Flag bitmap:** ((count+0x20) & ~0x1f)>>3 bytes. The retail file has 433 records → 56 bytes, 448 bits. The pointer is at 0x81e28 and the size at 0x81e2c (E/game_core.c:420-432). It is zeroed only at DB init and at new game (:432, :459). Saved as **chunk 6** (E/savegame.c:1602-1604).
- **Player state chunk 2 (0x30 bytes):** 16.16 X/Z/Y, angle, sector, equipped items, health, pitch, height, value factor (E/savegame.c:139-170). The read side restores the sector to 0x89f36, not 0x90c12 (:173-197).
- Other chunks:
  - chunk 7 inventory (0x400);
  - chunks 4/5 cursor lists;
  - chunk 3 map name;
  - chunk 0xe DBASE300 id;
  - chunk 0xd cutscenes-seen (high byte of each DBASE100 record +0x10);
  - **per-level world state**, bundled from `.TMP` files (E/savegame.c:1570-1616).
- **Inter-map persistence:** leaving a map writes its mutable world state (sector deltas, object patches, record lists, entities, SFX) to a temp file. Revisiting restores it and suppresses load-time init (E/map_load.c:574-677, :680-690). **Maps are not reset on re-entry.**
- Health 0x8a0f0, locomotion 0x819c0-0x819d1, velocity queues 0x90abe/0x90b42, map tuning (§3.1). These are globals, not per-map.

---

## 7. Random numbers

All seven generators are the same 16-bit LCG, `s = s·0x5e5 + 0x29`, each with its own state. There is **no runtime reseeding** except op 0x0b's countdown reseed; initial values come from the data image.

| State | Seed | Users | Ref |
|---|---|---|---|
| 0x7276c `rng_next` | 0x3f3 | particles (cmd 0x2d, edge sparks) | E/renderer.c:10797; E/rc:5845, 6041-6069; seed E/../data/obj3_symbols.h:213 |
| 0x7fe08 `rng_range` | BSS, 0 [inferred] | player weapon alt-slot pick, weapon_combat | E/renderer.c:10835; E/player.c:538-553; E/weapon_combat.c:838 |
| 0x71364/0x81e38 `rng_next_index_for_count` (with rotate and countdown reseed) | [open] | DBASE100 op 0x0b random lines, inventory random pick | E/renderer.c:11130-11146; E/dbase100.c:753; E/inventory.c:1432 |
| 0x72730 | 0x7e15 | AI wander/steer, enemy attack coin flip | E/entity_ai.c:493, 605; E/weapon_combat.c:266-271; obj3_symbols.h:199 |
| 0x7272c | 0x4fd | entity → player damage scaling | E/entity_ai.c:721-724, 988-1007 |
| 0x72734/0x72738 | 0x7e15 / 0x7e15 | ambient anim/sound (the dword-read quirk) | E/entity_ai.c:1250-1255 |
| 0x71f48 `g_random_seed` | [open] | cmd 0x3f random turn, cmd 0x12 random delay | E/rc:560-564, 4417-4418 |

First `rng_next` outputs from 0x3f3: 0x4788, 0xa4d1, 0x841e, 0xc4ff, 0x3344 (my evaluation of E/renderer.c:10799).

For replays: the draw order depends on fts (entity loops) and on the frame split, so bit-exact replay needs the same tick schedule per frame [inferred].

---

## 8. Rules vs presentation

**Port 1:1 (rules):**
- game_core: loop ordering, mode dispatch, the pending-action tail, death, new game (E/game_core.c:742-1028, 443-472), minus the draw calls.
- `gameplay_frame_step` minus HUD.
- `run_gameplay_frame`'s state machine (§4.3).
- Movement: `player_movement_tick`, `apply_player_movement_input`, `turn_input_*`, `move_input_*`, `key_a_jump`, `key_z_crouch`, look pitch (E/renderer.c:192-315, 592, 1044-1231).
- `update_player_vertical_physics`, `approach_value`, `move_player_with_collision` and all of collision_physics.c.
- `apply_moving_sector_carry`.
- `update_player_tick`'s weapon state machine (E/player.c:459-562).
- `classify_cursor_target_object`, `activate_targeted_object`, `examine_*` (logic only).
- All of raw_commands.c except the renderer hooks' texture plumbing.
- The dbase100.c interpreter.
- `init_loaded_object_table` and the trigger markers, `process_map_warp_or_load` and raw-state persistence.
- All RNGs.
- The flag, save and chunk logic.

**Replace (presentation or devices):**
- ISR and keyboard plumbing (E/input.c:240-325), `poll_mouse_motion` and the software cursor (E/input.c:759-845), `set_cursor_shape`, the cursor sprites, key bindings (E/input.c:414-700 display keys).
- `render_world_view` picking; keep the record contract, swap the implementation.
- Viewport, turn view-scale, view bob draw, `update_view_transform_params`, `apply_view_camera_params` (E/player.c:68-190).
- HUD, text UI, page flips, palette flash (keep the damage-flash counter as state).
- FPS counter, audio pumping.

**Hybrid:** the texture hooks 0x25/0x37 are invoked by the renderer (E/raw_commands.c:1351-1450). The engine must call them per animated texture per frame.

---

## 9. Size, dependencies, traps

**Size (code lines excluding comments, my count):**
- raw_commands.c ≈ 5,200 (≈ 4,000 port after removing lift scaffolding);
- dbase100.c ≈ 800;
- collision_physics.c ≈ 1,550;
- player.c ≈ 550 (≈ 350 rules);
- movement pieces in renderer.c ≈ 400;
- input.c ≈ 1,050 (≈ 450 rules);
- game_core.c loop ≈ 400 of rules;
- map_load trigger and persistence part ≈ 500.

**Total ≈ 8,000 lines of rule logic** for these systems. Doors, entities, inventory, dialogue UI and weapons are out of scope here.

**Dependencies:**
- the loaded RAW buffers must be kept **byte-layout-faithful** (geometry, objects and command tables are addressed by byte offset);
- the DBASE100 image;
- the door subsystem (`register_door_swing`, `toggle_door_open_state`);
- inventory (`give_item`, `remove_item`, `query_player_inventory`);
- the dialogue UI and GDV;
- the entity pools;
- audio ids.

**Traps to preserve:**
1. The flow pre-pass blocks re-triggering while a delay is armed (E/rc:1094-1130).
2. The Delay stop test uses the unmasked base byte, and dispatch uses `&0x7f` (E/rc:1257 vs 1272). Left-click dispatch compares `base == 2` / `== 8` unmasked (E/rc:2953, 2962).
3. key = 0 means the active object or face (E/rc:4043-4047). The staging globals are reset differently by each firer (E/rc:1697-1711).
4. The else-rerun through 0x38 and interrupt 2 (E/rc:1280-1287).
5. The one-shot bit 0x10 sets the skip 0x08 (E/rc:2897-2898).
6. The post-chain SFX uses the trigger's `+0xa` and plays only if the chain acted and no item was auto-selected (E/rc:1289-1300).
7. The lever-flip in the post-chain tail (E/rc:1301-1310).
8. Right-turn and left-turn asymmetry (E/renderer.c:298-315).
9. The run doubles queue entries rather than speed (E/renderer.c:1094-1101).
10. MOVE_SPEED is a decay period (§3.2).
11. fts is computed mid-frame and lags one frame (§2.3). It is clamped to 45 (E/render_world.c:78).
12. The 16-entry velocity queue drops ticks.
13. Pick depth, not distance, sets use range; transparent texels do not hit.
14. `classify` sets cursor type 3 early, and that sticks even on bail (E/input.c:1200-1203).
15. Revisited maps restore state and skip init (E/map_load.c:651-660).
16. Mouse swap defaults on (E/game_core.c:747).
17. The ROTH.C "BUGFIX" in the bbox paths is the original behaviour, not an addition (E/rc:3037-3043).
18. Entities tick fts times with an 8-bit counter; fts > 127 misbehaves, but the 45 clamp prevents it (E/entity_ai.c:1358).

**Current REMAROTH gap:**
- R/src/roth/roth_commands.cpp implements the flow pass and the exec loop. It lacks the post-chain tail, the reset state, the deferred queue and the tick/init tables.
- R/src/roth/roth_runtime.cpp:54-55 binds the wrong opcodes to use-wall and enter-sector, and implements only 0x2f (:127-133).

---

## 10. Acceptance tests (ROTH.C vs REMAROTH side by side)

Instrument both with a per-tick log of 0x90a8c/90/94 (16.16 position), 0x90a8a (angle), 0x90c12 (sector), 0x8a0f0 (health), the 56-byte bitmap, and the 7 RNG states. In ROTH.C, use a mod plugin through the documented plugin API (roth_c README "Modding").

1. **Clock:** idle 10 s in STUDY1. Δ`0x90bcc` = 700 ± 1. fts ≤ 45 always.
2. **Turn ramp:** STUDY1 start, hold Left for exactly 70 ticks, walking: Δangle = **+264** (units of 1/512 turn). Hold Right for 70: **−271**. Running: Left **+384** (from E/renderer.c:298-315 and :1223-1227).
3. **Walk:** STUDY1 start facing an open corridor with no wall within 600 units [pick the spot in ROTH.C], hold Forward for 35 ticks: the displacement sequence matches per tick byte-for-byte. The reference computation gives ≈ 234.0 units, ≈ 468.0 running. Repeat with a 0x41 command active: expect half or a quarter.
4. **MOVE_SPEED:** patch the first-map metadata +8 to 9 and repeat test 3. The trajectories must still match each other.
5. **Step and fit:** walk into a step of height max_climb and max_climb+1 (default 65/66). The first is climbed, the second blocks. Crouched, the limit is 16.
6. **Fall damage:** drop from a ledge where the fall accumulator reaches 30. Health drops by (30−22)² = 64, and the thud plays (E/player.c:286-295). Compare the health tick by tick.
7. **Jump arc:** press A on a flat floor. The Z trajectory over 19 phases is identical, and so is the ceiling-bonk case under a low ceiling.
8. **Use range:** approach a switch (a category-1 0x08 object in STUDY1). Record the pick depth +0x20 at which the cursor changes 0x398→0x248. Left-click at depth 599 fires; at 601 it does not. Calibrate the VR ray distance to the ROTH.C world distance measured here.
9. **Locked-door chain** (R/ROTH_COMMANDS.md "worked example"), STUDY1, without item 16: the object id swaps, 30 ticks pass, it swaps back, the light flickers, and no door moves. With item 16 the chain proceeds. The flag and bitmap diff is identical.
10. **Trigger categories:** for one record of each opcode 0x08, 0x13, 0x18, 0x19, 0x1a, 0x1b, 0x30, 0x31, 0x32, 0x39, 0x3d, 0x25, perform the §5.2 action and assert that the same chain index runs, in the same order, with the same handler returns and the same accumulated result.
11. **Delay gating:** retrigger a chain containing 0x12 while its delay is running. There must be no second execution (E/rc:1094).
12. **Map transition:** trigger a 0x3b (for example CHURCH1→VICAR). The arrival sector and position match. Return to CHURCH1: the modified sectors and objects persist, and the flags are unchanged.
13. **Save round-trip:** chunk 6 bytes are identical after the same scripted playthrough. Chunk 2 matches.
14. **RNG:** after N particle bursts (cmd 0x2d), `0x7276c` equals the N-th LCG value from 0x3f3. The first value is 0x4788. DBASE100 op 0x0b choices match over 20 invocations.
15. **Dialogue freeze:** during a voice line with freeze on, Forward does not move the player and Left still turns.
