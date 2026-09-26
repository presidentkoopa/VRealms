# ROTH_GAME_PORT.md
### The game half: porting Realms of the Haunting's rules into REMAROTH, 1:1
*Written 2026-09-26. Picks up where the loader stages end (ROTH_STATE.md §6c).*

---

## 0. The rule this whole document serves

**REMAROTH runs ROTH.C's brain with REMAROTH's eyes and hands.**

- Every game rule (what a click does, how Adam moves, how a monster decides, what a script opcode
  changes, what gets saved) is **ported from ROTH.C**, not redesigned. ROTH.C is the specification.
  Where the engine disagrees, the engine changes.
- Everything the player *sees, hears and touches it with* (screens, drawing, cursor, input devices,
  mixing) is **the engine's own**, and later **VR's**.
- **Nothing is done until it matches ROTH.C side by side** (§5): same inputs, same tick schedule,
  same resulting state.

Phase 1 is Realms exactly as it was, played flat. Phase 2 (VR) swaps presentation only, and anything
that changes a *rule* is logged in ROTH_BETTER.md as a deliberate, switchable divergence.

### Detail lives in six companion specs (in `docs/`)
Every claim in them carries a ROTH.C file:line reference. This file is the plan; those are the
reference manuals.

**Paths in the companion specs:**
- `/mnt/user-data/uploads/VRealms/...` = `E:\VRealms\...`, so ROTH.C is at
  `E:\VRealms\tools\ROTH.C\roth_c\src\engine\`.
- `/mnt/user-data/uploads/REMAROTH/...` = `E:\DOOMWork\REMAROTH\...`.
- Engine line numbers are from the tree as of 2026-09-26.
- Tags: `[inferred]` = reasoned from the code but not stated in it; `[open]` = unresolved.

| File | Covers |
|---|---|
| `GAME_core.md` | main loop and timing, player movement and collision, cursor/use/examine model, **every RAW and DBASE100 opcode mapped to its system**, the 15 trigger categories, RNGs |
| `GAME_inventory.md` | inventory data and rules, hands, pick up, use, combine, examine close-up |
| `GAME_dialogue.md` | DBASE100 interpreter and queue, choice menus, GDV codec and cutscene playback, subtitles, fonts, speech |
| `GAME_combat.md` | weapons, ammo, fire cycle, damage maths, monster spawn/AI/death, projectiles, player health |
| `GAME_audio_save_menu.md` | sound effects, ambient nodes and zones, music, save/load and per-map state, menus, HUD, game flow, automap |
| `GAME_engine_hooks.md` | where each system plugs into REMAROTH, the core/presentation seam, engine changes needed |

---

## 1. Fix before anything else: the trigger wiring is wrong in code

`src/roth/roth_runtime.cpp:54-55` binds the triggers to the wrong events. Verified in ROTH.C:

| Opcode | What it really is | ROTH.C | Runtime currently treats it as |
|---|---|---|---|
| **0x13** | **enter / leave a sector** | `twe_link_state` → `fire_sector_trigger`, raw_commands.c:3202-3270 | (not bound) |
| 0x18 | **left-click (use) a wall** | dispatch type 3, raw_commands.c:3018-3057 | enter sector |
| 0x32 | **right-click (examine) a wall** | raw_commands.c:3110-3144 | enter sector |
| 0x19 | left-click a floor / platform top | raw_commands.c:3058-3091 | use wall |
| 0x31 | right-click a floor / ceiling / platform | raw_commands.c:3165-3196 | use wall |
| 0x1a | **an attack or projectile hits a wall** (never the player walking) | collision_physics.c:556-568 | use wall |

The full table of all 15 categories (object clicks 0x08/0x30, walk-into-object 0x39, attack-hits-object
0x1b, animated-texture frame 0x25, texture remap 0x37, repeating timer 0x3d) is in `GAME_core.md` §5.2.
ROTH_COMMANDS.md needs the same correction.

Also:
- `ROTH_ENGINE_CHANGES.md` still says "no engine changes made". The tree already touches
  `p_openmap.cpp`, `maploader.cpp`, `p_spec.cpp` and `rothmap.cpp`. Update it before proposing more.
- ROTH did have an automap, but only as a developer overlay retail players can't reach
  (input.c:413-421). So "turn off the engine's automap" stands; the reason is "players never had one".

---

## 2. Architecture

### 2.1 One Realms core, grown from `roth_runtime`
`src/roth/core/`, C++, ported from ROTH.C. It owns **all game state and all rules**:
- the **mutable raw level image** (geometry, objects, command records). ROTH.C's handlers write
  their own records and the geometry in place; the current `const Map&` / `const Command&` handlers
  can't. Collision reads the same buffer.
- effect pools, the deferred-command queue, the timer queue;
- player kinematics and collision;
- inventory, hands, the 448-bit flag bitmap, dialogue queue, weapon state;
- actor pool (16) and projectile pool (16), both core-owned;
- the seven RNGs;
- the game mode byte (1 play, 3 inventory, 4/5 dialogue, 8 transitional, 0x20 dead).

**Transcription rule:** globals become fields of one `RealmsState` struct named after ROTH.C's
`g_names.h`, so handlers port line for line and stay diffable against the oracle. Keep 16-bit and
16.16 integer semantics until a rule has finished; convert to floats only for presentation.

**Keep** the coder's chain spine (`WalkChainFlow`, `ExecChain`), key resolution, face↔side mapping,
1:1 sector identity. **Don't** write a second runtime.

### 2.2 The engine is a mirror
The core is authoritative. After each core step it pushes changes out through one sink interface
(`IEngineSink`, the only file that includes engine headers):

- world: sector planes, textures, light; side textures and offsets; door angles; platform heights;
- puppets: engine actors for props, monsters, projectiles are **inert** and posed by the core;
- player pose (camera);
- audio: play/stop SFX, voice stream, music;
- movie start, map change requests, unhandled-opcode log.

**Two authorities is the main risk.** No engine thinker may move a puppet, sector or door on its own.

### 2.3 Presentation talks to the core only through intents and queries
Names only; `GAME_engine_hooks.md` §6 has the full list.

- **Intents in:** move input, turn, use(target), examine(target), fire(aim), select item, use item on,
  combine items, open/close inventory, dialogue choice/advance, skip voice, examine page, new game,
  save, load. `Target = {kind, realmsId, hitPos, dist}`.
- **Queries out:** mode, cursor kind, player view, health, damage flash, inventory, held item,
  weapon pose, dialogue state, examine state, text runs, HUD panels.

The flat UI and the later VR UI differ only in the picker, the intents they send and how they draw.
That is the whole point of the split.

### 2.4 Where it plugs in (verified hook points)

| System | Engine facility | Change needed |
|---|---|---|
| Core tick | `P_Ticker`, beside `WorldTick` (or the fork's `WorldStep`, so slow-mo applies); fallback `G_Ticker` before `P_Ticker()`, g_game.cpp:2098-2101 | S |
| Player | ZScript `RealmsPlayer` overriding `PlayerThink` / `MovePlayer` / `CheckUse` / `CalcHeight`; pawn non-colliding, posed by the core | none; optional per-pawn bob opt-out (S) |
| Use / examine | engine line trace mapped to Realms ids, sends intents; **retire the `P_ActivateLine` hook** | none |
| Sound effects | engine spatialisation; core decides what plays | memory-backed sfx (S, propose upstream) |
| Voice | `S_CreateCustomStream` (already used by `MovieAudioTrack`) | none |
| Music | ZMusic, HMP bytes from DBASE300 | read-from-memory hook (S) |
| Cutscenes | new `GdvPlayer : MoviePlayer` + signature branch in `OpenMovie` | GDV player (M) + `ga_playmovie` for mid-level movies (S) |
| Inventory / examine / dialogue UI | static `EventHandler` with `UiProcess` / `RenderOverlay` (**not** `DMenu`, which pauses the playsim) | none |
| Main menu | `DMenu` | none |
| HUD | ZScript `BaseStatusBar` subclass drawing core state | none |
| Weapon view | `PSP_WEAPON` psprite as a puppet (stays the VR hand anchor) | none |
| Saves | globals beside `P_WriteACSVars`; per-level under a `"realms"` key in `FLevelLocals::Serialize` (hub snapshots carry it) | module-serializer registry (S, propose upstream) |
| Map change | `ChangeLevel(..., CHANGELEVEL_NOINTERMISSION)`, arrival held in core state | none |
| Automap, intermissions, cheats, Doom HUD/weapons/player | `DF2_NO_AUTOMAP`, `nointermission`, game definition | none |
| Monster spawn on first draw | "was this object rendered this tic" flag | S-M |
| Font | glyph textures drawn by ZScript (or an `FFont` subclass later) | S (or M) |

---

## 3. Timing: how 1996 time maps onto a 35 Hz engine

- The original's heartbeat is **70 Hz** (audio.c:1835-1845). Input becomes velocity inside that
  interrupt. Every other system scales by `g_frame_time_scale`, the number of 70 Hz ticks since the
  last frame (clamped to 45).
- REMAROTH ticks at 35 Hz (doomdef.h:57). **Each engine tic: run the input/movement tick twice, then
  one frame step with frame_time_scale = 2.** That is bit-identical to the original running at a
  steady 35 fps, and deterministic.
- Do **not** run two frame steps at scale 1: that is the original at 70 fps, a different game.
- Side effects to accept and document: the 45-tick clamp and the 16-entry velocity-queue overflow
  never trigger.
- Blocking loops in the original (close-up viewer, cutscenes, death wait) become core modes, not
  loops.
- **For the side-by-side tests, run ROTH.C capped at 35 fps too**, so both sides see scale 2 every
  frame.

---

## 4. The systems, and the order to build them

Sizes are lines of *rule* logic to port. Total ≈ 15,000, with overlap: the DBASE100 interpreter is
shared by several systems.

### Step 0: The foundations and the test rig
- `RealmsState` + mutable raw image + `Core_AdvanceTicks` + `IEngineSink`.
- Fix the trigger bindings (§1).
- **Build the comparison rig first** (§5). Every later step is judged by it.
- Turn lighting on (ROTH_LIGHTING.md is transcribed; working in the dark is a workflow problem,
  solvable with a debug brightness cvar, not by leaving the rule off).

**Done when:** the core ticks inside the engine, and both ROTH.C and REMAROTH produce a per-tick log
in the same format.

### Step 1: The command interpreter, complete (`GAME_core.md` §5), ≈4,000 lines
- The exec loop's missing pieces:
  - the post-chain tail (chain SFX, lever flip, dialogue finish);
  - the chain-state reset;
  - the deferred-command queue;
  - the else-rerun via 0x38.
- The **per-frame effect tick table** (0x3088c) and the **load-time init table** (0x30998).
- Every RAW opcode, 0x00-0x42. Doors (0x2f) plug into the door system that's already hinged.
- The DBASE100 interpreter (`execute_dbase100_chain`) and the shared flag bitmap. RAW and DBASE100
  call each other (RAW 0x2b/0x36 → DBASE100; DBASE100 0x23/0x36 → RAW).

**Done when:** acceptance tests 9-11 in `GAME_core.md` §10 pass. Every trigger category fires the
same chain, in the same order, with the same results. Delay gating blocks re-triggers.

### Step 2: The player (`GAME_core.md` §3-4), ≈2,500 lines
- **Port movement and collision; don't tune the engine's.** The original's velocity decay
  ("MOVE_SPEED" is a half-life period, not a speed), run-as-double-enqueue, asymmetric turn ramp,
  jump table, crouch, fall damage, step-up and headroom rules can't be reproduced by GZDoom's friction
  model.
- Cursor modes and the click model:
  - Mouse buttons are **swapped by default** (game_core.c:747): right is primary.
  - Right-click examines, left-click uses, a held primary button steers, and a secondary click while
    steering fires.
  - Use range is measured in the renderer's pick depth (< 600), not world distance: **measure the
    world-distance equivalent in ROTH.C** and use that for the engine ray.
- Map transitions and **per-map revisit state**:
  - Leaving a map snapshots it.
  - Returning restores it and skips its first-visit setup.
  - The arrival point is the centre of the target sector's bounding box.

**Done when:** `GAME_core.md` §10 tests 1-8, 12 pass. The walk trajectory matches tick by tick, the
turn ramp is exact (+264 / −271 over 70 ticks), the jump arc and fall damage match, and a revisited
map keeps its changes.

### Step 3: Sound and music (`GAME_audio_save_menu.md` §2), ≈750 lines
- SFX lookup (key → FX bank row, ids stored +1, 0 = silent), **16-handle limit with drop, not steal**.
- Emitter ranges 2000 / 1000; the attenuation and zone maths:
  - only the first rect that hits counts;
  - flag bit 0 inverts a rect;
  - rects are relative to the node.
- Ambient nodes, modes 0-3, their timers and `cmd_activate_sfx_node`.
- Music:
  - **only DBASE100 ops 0x1a/0x1b (and loading a save) change it; loading a map never does**;
  - every song loops forever;
  - ducking around full-screen images.

**Done when:** tests 1-8 in `GAME_audio_save_menu.md` §8 pass.

### Step 4: Text, speech and dialogue (`GAME_dialogue.md` §2, §4-5), ≈1,450 lines
- DBASE400 text, the original font's advance widths, the wrapping and segment rules (they decide
  timing even if glyphs are drawn differently).
- DBASE500 DPCM speech.
- Monologue lines (op 0x05) and their timing.
- The queue:
  - max 8, extra entries dropped;
  - the REQUEUE overwrites slot 0.
- Choice menus:
  - hidden choices still count toward the branch number;
  - a single visible choice auto-runs;
  - a `0x0d` before an op 8 starts a menu.
- The random pick (op 0x0b).

**Done when:** `GAME_dialogue.md` §8 tests T6-T11 pass. Every dialogue record with choices gives the
same visible choices, flags, items and spoken lines under every gate state.

### Step 5: Cutscenes (`GAME_dialogue.md` §3), ≈750 lines
- **Port the GDV codec as is.** It's a pure bytes-to-frames transform, all encodings (8, 6, 5, 2, 1,
  0, 3), including the lookup table that back-references reach into, and the stereo DPCM audio.
- `GdvPlayer : MoviePlayer`:
  - audio-led sync;
  - a frame is decoded but not shown while 6 or more are behind;
  - +0.5 fps at 21168 Hz;
  - Esc-only skip.
- Subtitles on the decoded-sample clock, colour remapped to the movie palette.
- The gallery counters.
- 164 files in `DATA\GDV`; the index comes from DBASE100's cutscene table.

**Done when:** T1-T5 and T12 pass. All 164 files decode frame-identical, audio sample-identical,
subtitles appear on the same frame.

### Step 6: Inventory and examine (`GAME_inventory.md`), ≈800 lines
- A slot holds a **DBASE100 table index, not a render id**. A world object is an item when
  `textureIndex | textureSource<<8` matches a record, and only ADEMO objects (≥ 0x200) qualify.
- Hands:
  - **left = weapon**;
  - **right = selected item**.
- Rules:
  - pickup range and pickup lock;
  - stacking;
  - the hidden-by-filter bit (RAW 0x42);
  - use on self;
  - combine (the 5-way result rule);
  - swap order.
- **There is no "use item on world" verb.** Clicking an object runs its script, and opcode 0x27 tests
  the right-hand item. A carried-but-not-held item gets auto-selected on the first click, which then
  does nothing; the second click works.
- **There is no drop and no give-to-NPC.** Those are scripted checks and removals.
- **Examine:**
  - The close-up popup exists **only from the inventory**. It plays the item's GDV, and the
    toolbar has replay-comment, topics and document pages.
  - The first view speaks OnInspect and sets the "examined" bit.
  - **Right-clicking an item in the world only speaks the comment; no popup.**

**Done when:** `GAME_inventory.md` §10 tests 1-11 pass (pickup gate, the Staff, the Study key's
auto-select behaviour, stacking, full pack, combine, first examine, filter, save round-trip, new-game
inventory).

### Step 7: Weapons and player health (`GAME_combat.md` §2-3), ≈900 lines
- Weapon data comes from DBASE100 item blocks: WeaponAction (5), AsBullet (6).
- Ammo:
  - magazine = the slot's quantity;
  - recharge weapons in 8.8 fixed point, with a burst multiplier.
- Reload animation, fire cycle. **Rate of fire = animation frames × animation speed**; the bullet
  spawns at the BulletDelay frame.
- **No hitscan anywhere; melee fires a short-lived bullet too.**
- Health 0x800 default. Armour through DBASE100 op 0x37 (not applied to script damage).
- Fall damage `min((c−22)², 8000)`.
- Death runs DBASE100 record 4.

**Done when:** `GAME_combat.md` §9 tests 1, 5, 6, 11, 12 pass.

### Step 8: Monsters and projectiles (`GAME_combat.md` §4-5), ≈1,500 lines
- **Spawn:**
  - A monster comes alive the **first time it is drawn**, verified at renderer.c:5826-5840. With
    the pool full (16), it retries on the next draw.
  - Script spawns via 0x3c / 0x16. **0x16 on a live monster kills it.**
- **Stats:** AsMonster block (0x0A) → a 0x6C def, cached LRU of 10.
- **AI:**
  - no line of sight, no pathing: a monster that's been drawn always knows where Adam is;
  - turn-limited aim; contact vs ranged attacks (13/256 per tick);
  - a 257-tick attack cycle; 24-tick pain;
  - crit death when a single hit ≥ max HP;
  - a corpse becomes a prop after 257 ticks.
- **Projectiles:**
  - 1/2/4/8 sub-steps by speed;
  - **the player's shots can't hit the player; monsters' shots can hit other monsters**;
  - damage = `(B + rand·B>>16) >> 1`, with immunity ×0 and vulnerability ×2 per match.
- VR draws twice per frame, so "rendered this tic" must be a per-tic flag, not per-eye.

**Done when:** `GAME_combat.md` §9 tests 1-4, 7-10, 13-14 pass (def dumps byte-identical,
hits-to-kill table for every weapon × monster, spawn on first draw, attack timing).

### Step 9: Saves, menus, HUD and game flow (`GAME_audio_save_menu.md` §3-4), ≈1,500 lines
- Save content = the original's chunk set:
  - player record, inventory, flags, seen-counters, music chunk;
  - plus **every visited map's state** (geometry delta, objects, command records, effects, actor and
    projectile pools, SFX node bits).
- Keep RAW-order indices as stable ids, so an importer for original `.SAV` files stays possible
  (optional, ≈400 lines).
- Flow:
  - DBASE100 records 1 (boot), 3 (new game), 4 (death), 2 (quit);
  - health = max on new game;
  - the death wait (≤ 300 ticks), then reset to the start map and the main menu.
- Menus:
  - main menu Play / Load / Settings / Quit;
  - in-game options are reached **from the inventory screen**: Resume / Load / Save / Settings /
    cutscene gallery.
- HUD:
  - health bar, low-health image below half;
  - weapon/ammo panel, held item, portrait corner.

**Done when:** `GAME_audio_save_menu.md` §8 tests 9-15 pass. That's the end of phase 1: Realms,
start to finish, on your engine.

---

## 5. How "matches ROTH.C" is proven

1. **Instrument both sides the same way.**
   - ROTH.C has a mod plugin API (its README, "Modding"): a logging mod dumps per-tick state.
   - REMAROTH gets a matching log from the core.
   - Minimum fields:
     - position (16.16) and angle, sector, health;
     - the flag bitmap, inventory slots;
     - the seven RNG states;
     - the command-chain trace (which chain, which records, handler returns).
2. **Same tick schedule.** ROTH.C capped at 35 fps (frame_time_scale = 2), REMAROTH as §3.
3. **Same inputs.** Record an input script (per-tick keys, cursor position, clicks) and replay it on
   both.
4. **Diff the logs.** The first differing tick is the bug. Bit-exact is the target, and achievable
   because both sides run the same integer rules on the same schedule.
5. **Pure transforms get file-level tests:** GDV frames, DPCM samples, text layout, def dumps.
6. **What can't be diffed numerically** (drawing, mixing) is judged side by side on screen, same
   spot, same angle.

Every step in §4 names its tests. The acceptance lists in the six companion specs are the checklist.

---

## 6. Engine changes this will need (log each in ROTH_ENGINE_CHANGES.md first)

| # | Change | Size | Upstream to REMA? |
|---|---|---|---|
| 1 | Core tick call in `P_Ticker` / `WorldStep` | S | no |
| 2 | Module-serializer registry for saves | S | yes |
| 3 | Memory-backed sound effects | S | yes |
| 4 | Music from memory | S | yes, if needed |
| 5 | GDV movie player + `OpenMovie` path fallback | M | yes |
| 6 | `ga_playmovie` (mid-level movie returning to the level) | S | yes |
| 7 | Per-pawn weapon-bob opt-out | S | yes |
| 8 | "Rendered this tic" flag for first-draw spawns | S-M | yes |
| 9 | Interpolated polyobject angle / sector planes | S-M | yes |
| 10 | One-sided door slabs (the reverted door work) | M | loader only |
| 11 | ROTH font | S-M | maybe |
| 12 | The core itself | L | n/a |

---

## 7. Open questions (each needs one measurement or dump, not a design decision)

- Use range in world units (measure in ROTH.C; §4 step 2).
- Jump gravity table values (data image 0x71304).
- Opcodes 0x00, 0x04, 0x05, 0x2c: no handler identified; category 3 has no firer.
- Seeds for two RNGs (0x71364, 0x71f48).
- What DBASE100 records 1-4 contain (dump them).
- The retail start map (CONFIG.INI; expected STUDY1).
- End-of-game path (probably a DBASE100 record).
- Inventory tab category bytes (0x7123c).
- Whether monsters behind geometry spawn (does "drawn" mean visible spans, or just queued?).
- Possible ROTH.C lift bug in subtitle segment timing (dialogue_ui.c:1007; check the disassembly at
  0x1f0e8).
- GDV audio decoder bodies (in ROTH.C's `os_audio.c`, not yet read).
- The death quick-load key (code says F9, scancode reads as Space).

---

## 8. What this means for the coder, in one paragraph

Stop treating the game systems as separate features to build. They are one ported program, the
ROTH.C game layer, running inside the engine as a core that owns state and rules, with the engine as
its screen, speakers and hands. Grow `roth_runtime` into that core, fix the trigger wiring, build the
side-by-side log rig, then port in the order above. The rig is what keeps "1:1" honest. The core/
presentation split is what makes VR a swap instead of a rewrite.
