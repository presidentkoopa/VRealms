# REMAROTH: engine hooks for the Realms game layer

Paths: `R/` = `/mnt/user-data/uploads/REMAROTH/`, `C/` = `/mnt/user-data/uploads/VRealms/tools/ROTH.C/roth_c/src/engine/`. Read-only; nothing modified. Tags: **[inferred]** (a reasoned claim with no line to cite), **[open]** (the staged sources cannot settle it). Not staged, so claims about them are marked: `p_tick.cpp` (P_Ticker), `po_man.cpp`, `s_doomsound.cpp`, `src/common/fonts/`, `animtexture.*`, ZScript `player.zs`/`statusbar`/`menu` classes, and ZMusic.

---

## 1. What the coder's runtime does today

**`roth_commands.*`: the engine-free spine**
- `WalkChainFlow` is a transcription of `walk_command_chain_flow`. It runs only the gate opcodes 0x12 and 0x1e, with a depth bound of 64 (R/src/roth/roth_commands.cpp:21-86).
- `ExecChain` is a transcription of `rawcmd_exec_loop` (roth_commands.cpp:110-151):
  - skips disabled records (bit 0x08)
  - masks the opcode with `&0x7f`
  - ORs the handler results together
  - stops the chain after 0x12
  - re-runs from `nextActive` when the interrupt is `Rerun`
- Handlers are plugged in through `std::function Run` (roth_commands.h:39-53).

**`roth_runtime.*`: the live half.** One static `Runtime g` holds (roth_runtime.cpp:28-49):
- a **copy of the parsed `Map`**
- trigger bindings `byFace`, `bySector` and `bySide`
- `doorTag` (Realms sector to polyobject tag)
- `faceToSide`
- counters

**Lifecycle:**
- `BeginLevel` (roth_runtime.cpp:156-206) is called at the end of `LoadRothMap` (R/src/maploader/rothmap.cpp:1577).
  - It binds use-wall triggers (0x19/0x31/0x1A) and enter-sector triggers (0x18/0x32).
  - It translates face bindings into sidedef bindings.
- `EndLevel` is called at the start of a load (rothmap.cpp:140).
- `RegisterFaceSide` and `RegisterDoor` are fed by the loader (rothmap.cpp:596, 1431).

**Hooks actually wired:**
- **One hook only:** `roth::ActivateLine` at the top of `P_ActivateLine` (R/src/playsim/p_spec.cpp:125-132). That is the Doom use-line path.
- `CrossSector` is declared and defined (roth_runtime.cpp:252-259), but **nothing calls it**. The only reference outside the runtime is the RegisterDoor call in rothmap.cpp.
- There is **no per-tic entry point**.

**Handlers:**
- Only 0x2f open-door is implemented. It maps onto `EV_OpenPolyDoor(... PODOOR_SWING)` (roth_runtime.cpp:77-115). That is an approximation: GZDoom swing units stand in for the original's door tick.
- Everything else is counted as unhandled (roth_runtime.cpp:125-136).
- The door slabs themselves were reverted (R/ROTH_STATE.md:175-186).

**Structural limits the port must remove:**
1. **Handlers see `const Map&` / `const Command&`** (roth_runtime.cpp:125; roth_commands.h:44). The original handlers **write their own records**. Examples:
   - `cmd_delay_timer` sets `[rec+2] |= 0x20` (C/raw_commands.c:4440-4441).
   - `cmd_map_transition` sets the skip bit (raw_commands.c:530-532).
   - `cmd_change_height` toggles `[rec+2] ^= 2` (raw_commands.c:4491).
   
   The record state has to become mutable, and it has to be saved.
2. **There is no per-frame effect tick.** The original's actions are mostly *registrations* into an active-effect pool. That pool is ticked every frame by `tick_world_effects` through a tick-dispatch table (raw_commands.c:3272-3287), for example `tick_change_height` (raw_commands.c:2448). Without a tick, 0x12 delays, height changes, lights and scrolls can never run.
3. **`Command` is a decoded struct.** It holds `args` as u16 words from +6 (R/src/roth/roth_raw.h:168-196). ROTH.C handlers address bytes (`rec+0xa`, `+0xe`, `+0x10`). Transcribing them line for line is simplest against the original byte image (see §2).
4. **Spawned props keep no link back to their Realms object index.** `SpawnPreparedObjects` discards it (R/src/roth/roth_objects.cpp:706-732). The core needs an object-to-actor table.
5. `Fire` makes a fresh `Handlers` per chain (roth_runtime.cpp:145). The original's interrupt flag and next-active latch are globals (`g_command_chain_interrupt` 0x8a268, roth_commands.h:30-52). One handler writes `g@0x8a268=2` directly (`cmd_change_object_texture`, raw_commands.c:4518). This is harmless only if it is kept per-chain on purpose. **[inferred]**
6. **Stale document:** `R/ROTH_ENGINE_CHANGES.md:13-19` still says "no engine changes made yet". The tree already touches p_openmap, maploader, p_spec and rothmap. That file must be brought up to date before any proposal goes upstream.

**Keep:** the spine, the key resolution, the face-to-side and sector identity (Realms sector i is GZDoom sector i, roth_objects.cpp:697-699) and the loader. **Grow** `roth_runtime` into the core described below. Do not write a second runtime.

---

## 2. Architecture: a Realms game core

### 2.1 Shape

`src/roth/core/` is a C++ module ported from ROTH.C. It owns **all game state and rules**:
- the mutable raw level image (sectors, faces, command records, object table, sfx nodes)
- the effect pools and entity pools
- player kinematics
- inventory, the DBASE100 flag bitmap, dialogue state and weapon state
- the modal mode (`g_player_movement_enabled`: 1 = play, 3 = inventory, 4/5 = dialogue, 0x20 = dead; C/game_core.c:735-741 comment, 861-898)

**Transcription rule:** keep the original's raw heap image. The original reads the .RAW into one block and uses it in place (rothc_rules §1, ML:417-494). Globals become fields of one `RealmsState` struct named after `g_names.h`, so that `G32(VA_x)` becomes `S.x`. Handlers then port mechanically from ROTH.C, keep its offsets, and stay verifiable against the oracle. `roth::Map` stays the loader's parsed view.

**Authority: the core is authoritative, and the engine mirrors it.**
- ROTH.C handlers and ticks write geometry directly. `tick_change_height` edits sector heights in the geometry buffer (raw_commands.c:2448-2475).
- Collision reads the same buffer (C/collision_physics.c:1677-1720).

If the engine owned sectors, doors or actors, every rule would have to be re-expressed through engine calls, and door and platform timing would follow engine semantics, as in today's `EV_OpenPolyDoor`. So after each core step, the changed state is pushed out to the engine:
- sector planes
- side and flat textures
- light
- polyobject angle
- puppet actor pose and frame

### 2.2 Where it ticks

**Primary hook: inside `P_Ticker`,** next to `staticEventManager.WorldTick()`:
- Per events.h:532-533, WorldTick "executes on every tick, before everything, only when in valid level and not paused". The `P_Ticker` body lives in p_tick.cpp, which is not staged, so the exact spot is **[open]**.
- Call `roth::core::Tick()` **before** `Thinkers.RunThinkers`, so puppets get this tic's pose before actors save their previous positions for interpolation. **[inferred]**
- **Fallback with a line cite:** `G_Ticker`, `case GS_LEVEL`, immediately before `P_Ticker()` (R/src/g_game.cpp:2098-2101).
- Use the fork's world clock deliberately:
  - `WorldStep` is "once per WORLD step, after it" (events.h:534-538). Thinkers skip ticks between world steps (dthinker.h:77-82).
  - Tick the core on world steps, so that any slow-motion feature slows the core too. At time scale 1 the two are the same.

**Do not rely on engine pause for Realms modal screens.** The original freezes the world in inventory and dialogue mode because `game_play_loop` simply does not call `gameplay_frame_step` in those modes (mode dispatch at game_core.c:861-898; the inner loop's only `gameplay_frame_step` call, game_core.c:877, is in the play-mode branch). The core reproduces this internally: its `Tick()` keeps running and dispatches on its own mode. Engine pause (menus) stops `P_Ticker` entirely. Keep that for the out-of-game menu only.

### 2.3 How events come in

| Original input | Original source | Engine source → core call |
|---|---|---|
| Use / examine / fire on the target under the cursor | `run_gameplay_frame`: `render_world_view` returns the **hit record under the cursor**, then `classify_cursor_target_object`, `activate_targeted_object` or `examine_object_under_cursor` (C/game_core.c:578-581, 667-671, 692-697; C/input.c:972-1030) | Presentation **picks** with an engine line trace from the view through the cursor or crosshair (later a VR ray). It maps the hit to a Realms id (sidedef→face via the inverse of `faceToSide`; actor→object via the new table; sector index 1:1), then calls `core.Use(target)`, `core.Examine(target)` or `core.Fire(aim)`. **Retire** the `P_ActivateLine` hook (p_spec.cpp:129) once this is in: Doom's use-line has the wrong range and cannot hit objects. |
| Movement keys and mouse | the int-8 ISR `player_movement_tick` (C/renderer.c:1041-1100) | `usercmd` is translated to the core's input bits and mouse deltas in the Realms PlayerPawn `PlayerThink`/`MovePlayer` override (see §4). |
| Item use, combine, dialogue choice | UI modes 3/4/5 (game_core.c:861-898) | ZScript UI → `core.Intent*()`, queued and consumed on the next core tick. The engine-sanctioned route from UI scope to play scope is `SendNetworkEvent` → `NetworkProcess` (events.h:577; events.zs:225). |
| Entering a sector | the core's own collision tracks `g_player_sector` (collision_physics.c:1486-1493), and link/enter triggers fire from `twe_link_state` (raw_commands.c:3197-3215) | **Internal to the core.** No engine hook is needed, because the core owns movement. Delete the orphaned `CrossSector`. |

### 2.4 How effects go out

A single `IEngineSink` interface is implemented by `roth_engine_bridge.cpp`, which is the only file that includes engine headers. Its function list is in §6. Mechanisms:
- **Sector heights:** `SetPlaneTexZ` plus `floorplane.set` / `ceilingplane.set`, the same calls the loader uses (engine_scope §2).
- **Textures:** `side->SetTexture` / `sector->SetTexture`.
- **Doors:** polyobject rotation to an absolute angle (po_man, **[inferred]** API).
- **Puppets:** actor `SetOrigin` / `Angles`; `sprite`/`frame`/`picnum` as roth_objects already does (roth_objects.cpp:713-722).
- **Sound, movies, text:** see §5.

### 2.5 C++ vs ZScript

- **Core in C++.** The source is C with exact integer and overflow semantics (16-bit wraps everywhere, for example game_core.c:329-341). It needs native `FSerializer` access for saves and direct `sector_t`/`side_t` writes. It must also stay buildable in the out-of-tree self-test (R/ROTH_STATE.md:43-53). None of these suit the VM.
- **Presentation in ZScript**, because that is where the engine exposes its UI surfaces:
  - status bar classes (`DBaseStatusBar` is VM-backed; sbar.h:325-391)
  - `EventHandler.RenderOverlay` / `UiProcess` / `UiTick` (events.h:345-361)
  - menus (menu.h:245-304)
  - cutscene runners (screenjob.h:61-90; `MoviePlayer` natives at movieplayer.cpp:924-974)
  - PlayerPawn virtuals (p_user.cpp:1839)
- **The seam** is one ZScript `struct RealmsCore native` with `native static` functions (`DEFINE_ACTION_FUNCTION_NATIVE`):
  - read-only getters for UI state
  - intent setters that only enqueue

---

## 3. Timing

**The original's clock:**
- An int-8 heartbeat, `vsync_timer_tick`, increments `g_frame_tick_counter` 0x90bcc. **In play mode it also runs `player_movement_tick`** (renderer.c:36-42).
  - With music on, it is the SOS event registered at **70 Hz** (`register_music_timer_event`, rate 0x46; C/audio.c:1835-1845; renderer.c:41-42 "both timer configs").
  - With music off, the rate is **[inferred]** also 70 Hz (game_core.c:374 "the 70 Hz heartbeat").
  - The GDV fade ISR also runs at about 70 Hz (C/gdv_cutscene.c:937-940).
- The main loop is **variable-rate**. `g_frame_time_scale` is the number of 70 Hz ticks since the last frame, and it is never 0 (`update_frame_time_scale` spins until the delta is nonzero; game_core.c:329-341).
- Every per-frame rule multiplies by that delta. Examples:
  - effect ticks (`tick_change_height`, raw_commands.c:2451-2455)
  - weapon raise and lower by `fts*4` (C/player.c:480-487)
  - the damage-flash decay by `4*delta` (game_core.c:336-341)
  - entities: `tick_dynamic_entities` runs `update_dynamic_entities` **once per 70 Hz tick**, looping `frame_time_scale` times (C/entity_ai.c:1348-1362)
- Player input → velocity runs at a fixed 70 Hz inside the ISR, with a per-map decay counter (renderer.c:1041-1100). The frame then drains the velocity queue through `move_player_with_collision` (collision_physics.c:1677-1720). The queue holds 0x10 entries (renderer.c:1076).

**The engine's clock:** TICRATE = 35 (R/src/doomdef.h:57).

**Faithful mapping:** the core runs on a **70 Hz virtual clock**. On each engine tic:

```
core.Tick():
  repeat 2:  core.IsrTick()          // player_movement_tick + counter++ (70 Hz, fixed)
  S.frame_time_scale = 2             // exactly what update_frame_time_scale yields at 35 fps
  core.FrameStep()                   // gameplay_frame_step + run_gameplay_frame + pending actions
```

- This is **bit-identical to the original running at a steady 35 fps**, which is within its real range.
- It is deterministic, so saves and demos are reproducible.
- The engine's render interpolation smooths the output between tics.
- Do **not** run one `FrameStep` with scale 1 twice per tic. Frame-granular logic would then run at 70 fps, and the original never did that on 1996 hardware. **[inferred]**
- Keep the core API as `AdvanceTicks(n)` so the rate stays a parameter.
- Modal and blocking loops (menus, examine pages, the message box) spin on the same counter (C/file_config.c:1006-1011; C/savegame.c prompt notes). In the port, each becomes a core *mode* stepped by the same `Tick()`, never a blocking loop.
- **[open]:** where `update_frame_time_scale` is called while in play mode. Its only direct call site is in the mode-3 branch (game_core.c:884). The semantics are still unambiguous.

---

## 4. Player movement

**Can GZDoom's movement be tuned to match? Only approximately.** The original's parameters:
- Height = 2·meta[0x0A] = 144; max climb = 2·meta[0x0C]+1 = 65; min fit = 2·meta[0x0E] = 96 (C/map_load.c:206-209).
  - The loader already forces height through `Level->ForcedPlayerViewHeight` / `ForcedPlayerHeight = ph+10` (rothmap.cpp:117-121). Compare the `+0xa` in collision_physics.c:65.
  - Radius: `collision_radius = 0x70`, `radius_sq = 0xc400`, `corner_radius_sq = 0xc440`, box ±0x1c (collision_physics.c:1688-1694). Those numbers are not mutually consistent as one radius. **[open]:** what the units are.
- Speed and friction:
  - Velocity accumulators are **halved every `move_speed_immediate` ISR ticks**. That value is per map, and the original patched it into the code at load (renderer.c:1058-1070; map_load.c:205).
  - A deadzone of 0x20 applies.
  - Running **enqueues the step twice** (renderer.c:1079-1089).
  - Command 0x41 applies a speed-reduction shift (renderer.c:1074-1076).
  
  GZDoom's per-tic multiplicative friction and forward-move scaling cannot express "halve every N ticks, with a deadzone".
- Collision: a circle against Realms sector edges, with portal walk and substeps (`sweep_move_with_collision`, `collide_point_walls_recursive`; collision_physics.c:674-1720). GZDoom uses square box-vs-line collision on its own converted lines. Results differ at doorways, sister-merged faces and door slabs.

**Recommendation: the core owns player movement and collision.**
- The core needs the Realms geometry for its commands anyway.
- Doors and moving sectors are core state, so collision against them is consistent by construction.
- Monsters (entity_ai.c) use the same collision code.

**What owning it requires. Nothing structural in `p_user.cpp`:**
- `P_PlayerThink` calls the **virtual** `PlayerPawn.PlayerThink` (R/src/playsim/p_user.cpp:1839-1843).
- A ZScript `RealmsPlayer : PlayerPawn` overrides these:
  - `PlayerThink`/`MovePlayer`: feed the core input, set no velocity.
  - `CheckUse`: disabled. Normally it calls `P_CheckUse` → `P_UseLines` (p_user.cpp:1673-1698).
  - `CalcHeight`: use the core's view Z and bob (`update_player_view_bob`, C/player.c:340-392).
  - jump and crouch: disabled.
- The pawn is flagged NOGRAVITY and non-colliding. **[inferred]** Its pose is set from the core every tic.
- **Engine touch (small, optional):** the unconditional `P_BobWeapon` / `P_BobWeapon3D` after the virtual (p_user.cpp:1845-1848) needs a per-pawn opt-out, because Realms has its own bob.
- The rest of `P_PlayerThink` is harmless for single-player:
  - angle offset targets
  - `ButtonInject`
  - VR haptics
  - the velocity record (p_user.cpp:1745-1858)
- **[inferred]** Monster collision against the player then happens in the core, not through `P_TryMove`.

---

## 5. Per-system hook points

**Sound effects**
- `sfxinfo_t` is lump-backed (`lumpnum`; s_soundinternal.h:129-163).
- `LoadSound` only loads through `ReadSound(lumpnum)` (s_sound.cpp:732-797).
- `StartSound` refuses `lumpnum == sfx_empty` (s_sound.cpp:521-528).
- `AddSoundLump(name, lump, ...)` needs a lump (s_sound.cpp:1655-1668).
- So samples from ROTH's .SFX banks (`load_sound_effect_bank`, audio.c:1059) **cannot be registered without lumps as the code stands**.

**Change needed (small, generic):** "memory-backed sfx". Two options:
- Give `sfxinfo_t` an optional owned PCM buffer, rate and bits, with `LoadSound` calling `GSnd->LoadSoundRaw` (i_sound.h:95) when it is set.
- Or add `SoundEngine::AddSoundFromMemory(name, data, rate, bits)`, where `ReadSound` resolves a reserved virtual lump range.

Playback:
- 3D sounds: `StartSound(SOURCE_Actor/Unattached, pos ...)` (s_soundinternal.h:384-386).
- UI and voice sounds: `SOURCE_None`.
- Fidelity choice: the original computes its own volume and pan from distance (`compute_sound_volume_pan`, audio.c:65). Let the core decide **what starts and stops, and when** (the rules). Hand the position to the engine and fit an `FRolloffInfo` to the original's volume-over-distance curve. Straight pan does not survive VR spatialisation.

**Ambient SFX nodes.** These are pure core state:
- node list, active bits, range query (`query_sfx_emitters_in_range`, `play_nearby_sfx_emitters` per frame; audio.c:605, 940; player.c:419-420)
- activation by the command handler `cmd_activate_sfx_node` (opcode [open]) (raw_commands.c:4855)
- the active bits are saved (`load_sfx_node_active_state`, audio.c:217)

The engine sink only needs `StartLoop(emitterId, sfx, pos, vol)` and `Stop(emitterId)`. Use a per-emitter source key, or one puppet actor per emitter.

**Speech and voice streaming** (`voice_stream_pump`, audio.c:1300; per frame in the loop, game_core.c:848):
- Use `S_CreateCustomStream(size, rate, channels, type, cb, userdata)` (s_music.h:41; music.cpp:164). It works from memory with no lumps, and it is exactly what `MovieAudioTrack` uses (movieplayer.cpp:75-96).

**Music**
- The original music is an SOS/HMI MIDI sequence. The track headers are walked at desc+0x388 (audio.c:1515-1530). Treat the format as HMI/HMP **[inferred]**, and check it against a retail file.
- ZMusic reads HMI/HMP **[inferred; not staged]**.
- `OpenMusic` accepts only a real file path (`FileExists` → `reader.OpenFile`) or a lump (music.cpp:100-123).
  - If the music files sit loose in the install, `S_ChangeMusic("<abs path>")` works **today** (music.cpp:680, 738).
  - If they are inside a container, add a `MusicCallbacks::OpenReader` hook that returns a memory `FileReader`. The struct today is only `LookupFileName` and `FindMusic` (s_music.h:45-49). This is a small, generic change.
- The core owns the rules: which song, restart on end (`service_audio_sequence`, audio.c:1822-1830), and the mute and restore around voice. Loop restart can be left to the engine's `looping` flag only if that is observably identical. **[open]**

**Cutscenes (GDV)**
- `MoviePlayer` is a four-method interface: `Start`, `Frame(clock)`, `Stop`, `GetTexture` (movieplayer.cpp:45-62).
- `MvePlayer` is the template to copy (movieplayer.cpp:240-296):
  - a decoder plus `MovieAudioTrack`
  - audio-clock sync through `GetClockTime`
  - the frame texture comes from `animTex().GetFrameID()` (paletted, 8-bit → `AnimTextures`, **[inferred]**)
- **Add `GdvPlayer : MoviePlayer`** and a signature branch in `OpenMovie` (movieplayer.cpp:863-915 marks the spot: "add more formats here"). The decoder is ported from C/gdv_cutscene.c.
- **Gaps:**
  - `OpenMovie` opens files only through `fileSystem.ReopenFileReader` (movieplayer.cpp:833-858). The 164 GDVs are in the player's install (ROTH_STATE.md:250-252). Needed: an absolute-path fallback, `FileReader::OpenFile`, or mounting `DATA/GDV` as a resource directory. Small.
  - Running a movie **mid-level**: `StartCutscene(def, flags, completion)` (screenjob.cpp:309-334) with `gamestate = GS_CUTSCENE`. The ticker and drawer already dispatch on it (g_game.cpp:2115-2118; d_main.cpp:1803-1806), and input goes to `ScreenJobResponder` (g_game.cpp:1735-1738). What is missing is a return path to `GS_LEVEL` without an intermission. Today only `ga_intermission` sets GS_CUTSCENE (g_game.cpp:2041-2045). Add a small `ga_playmovie` / `G_StartInLevelMovie(def, onDone)` (small, generic). This freezes the playsim during the movie, matching the blocking GDV loop in the original (gdv_cutscene.c:1623).
  - The core fires movies through its sink (command 0x2b, dialogue, map transition). The completion callback tells the core, which resumes its chain.

**Text, subtitles and fonts**
- The font code (`src/common/fonts/`) is not staged, so the details are **[open]**.
- Option A (small, no engine change): register each glyph of ROTH's font (C/text_font.c) as a runtime texture (as `roth_texture` already does) and draw strings from ZScript. The layout rules (wrap, dwell = `dwell*rate/divisor`, C/dialogue_ui.c:1007) stay in the core and reach ZScript as positioned glyph runs.
- Option B (medium): an `FFont` subclass built from DAS glyphs. **[inferred]** This is the better long-term route because `Screen.DrawText`, and later VR text, would work unchanged.
- The engine's own subtitle channel (`player_t::SetSubtitle`, p_user.cpp:836) is not ROTH's model. Do not use it.

**Inventory, examine and dialogue screens**
- In the original these are **core modes** with their own per-frame UI logic (`update_inventory_screen`, `handle_cursor_click`, `update_dialogue_cursor_and_click`; game_core.c:861-898).
- Use a **static `EventHandler`**, not `DMenu`:
  - `UiProcess` with `IsUiProcessor` / `RequireMouse` for the cursor and clicks (events.h:290-291, 359)
  - `RenderOverlay` to draw (events.h:346)
  - it reads the core through `RealmsCore` getters, and clicks become intents
- Reason: `DMenu` pauses the playsim (menu.h:258-279, `DontPause` is opt-in) and stops the core tick. The core must keep ticking in modes 3, 4 and 5 (frame timer, voice pump, dialogue dwell), while holding the world still itself.
- Keep **DMenu (ZScript ListMenu)** for the out-of-game main menu, options and save/load UI (`run_main_menu`, game_core.c:807). Its actions call `core.NewGame()` and the engine save/load.

**HUD.** Use a ZScript `RealmsStatusBar : BaseStatusBar`, selected through GAMEINFO `StatusBarClass` **[inferred]**. It draws only what the core exposes:
- held item icon (`draw_held_item_icon`)
- panels (`draw_active_ui_panels`)
- corner portrait
- text UI (game_core.c:492-503)

The damage flash is core state (`g_damage_flash_level`, game_core.c:336-341). Present it as a screen blend.

**Weapons.** The core owns the state machine:
- raise and lower, fire-lock, cooldown and reload (player.c:445-520)
- `trigger_weapon_fire` from the cursor state machine (game_core.c:684, 722)
- shots through the core's entity pools

Presentation is a single `RealmsHands : Weapon` whose `PSP_WEAPON` layer is a **puppet**:
- `DPSprite::Sprite`, `Frame`, `x`, `y` and `scale` are public (p_pspr.h:199-206, 583-588), so the core pose is written every tic with no state logic.
- This keeps the psprite layer, which is the engine's per-hand attachment point (p_pspr.h:224-226), available for VR later without touching the core.
- No Doom weapons and no `StartItem`s.

**Monsters**
- **Core-owned entities** (entity_ai.c pools A/B, ticked `frame_time_scale` times per frame; entity_ai.c:1348-1362). Engine actors are **puppets**: a `RealmsPuppet` class with no thinking and no collision, pose, sprite and frame set by the core. `roth_objects` already spawns this kind of actor (roth_objects.cpp:706-722); extend it with object↔actor tables.
- The coder notes that `FAT_MONSTER` art "spawns a live AI actor the first time it is drawn" (ROTH_STATE.md:268-269). That is a **render-to-rules dependency**. The core needs a "was rendered this tic" signal per puppet from the renderer. Whether the fork exposes such a flag is **[open]**; if not, add one (small). Verify the rule in entity_ai.c first.

**Saves**
- The original chunks (C/savegame.c:1512-1520): version, player 0x30, level name, dbase300 id, inventory, cursor, the **DBASE100 bitmap**, cutscenes-seen, then **every per-level temp state file** (`bundle_level_states`, savegame.c:1138-1205).
- Map onto the engine:
  - **Global core state** goes to `globals.json`. Write it next to `P_WriteACSVars` in `G_DoSaveGame` (g_game.cpp:3244-3246). Read it after `P_ReadACSVars` in `G_DoLoadGame` (g_game.cpp:2951-2953).
  - **Per-level core state** is the raw-state stream (`write_raw_state_temp`): objects, command records, sfx nodes, the effect list and entity pools. Put it under a `"realms"` key in `FLevelLocals::Serialize` (p_saveg.cpp:1012-1150). That makes hub **snapshots** carry it for free (`SnapshotLevel` / `UnSnapshotLevel`, p_saveg.cpp:1192-1230).
- **Engine change (small; propose upstream):** a registry of native "module serializers", one for globals and one for level, default-empty. That avoids hard-coding `roth::` into `p_saveg.cpp` and `g_game.cpp`.
- **Pointer fields** (for example an effect record's `chunk[8] = rec` back-pointer, raw_commands.c:4426) must be stored as indices. ROTH's own link fixups show how (savegame.c:211-270).
- **Load order works:**
  1. `P_SetupLevel` runs the loader and `BeginLevel` (g_level.cpp:1480).
  2. `UnSnapshotLevel` overwrites the state (g_level.cpp:1504).
  3. `WorldLoaded` fires (g_level.cpp:1550).
  
  The core must skip first-load entry-point execution when restored. The `IsReopen` / `FromSnapshot` flag is available (events.zs:93; g_level.cpp:1507-1535).

**Map transitions**
- `cmd_map_transition` latches the destination and `g_pending_game_action = 1` (raw_commands.c:525-533). The frame tail then calls `process_map_warp_or_load` (game_core.c:923-951).
- The core emits `RequestChangeMap(name, arrival)`. The bridge calls `primaryLevel->ChangeLevel(name, 0, CHANGELEVEL_NOINTERMISSION, ...)` (g_level.cpp:733, 790-792).
- The arrival point stays in core state and is applied at `WorldLoaded`, not through a map-thing start.
- MAPINFO:
  - put all 44 maps in **one hub cluster**, so per-map state persists (`CLUSTER_HUB`, g_level.cpp:835), matching `bundle_level_states`
  - set `nointermission` (g_level.cpp:720)
  - `G_DoCompleted` then still runs `RunIntermission` with no status screen (g_level.cpp:1116-1168). Confirm it completes immediately. **[open]**

**Doom-only behaviour to disable**
- **Automap.** `AM_ToggleMap` returns when `dmflags2 & DF2_NO_AUTOMAP` is set (am_map.cpp:3548-3556). Force that flag in the VRealms package and remove `togglemap` from the binds and menus.
  - Correction to ROTH_STATE.md:256: ROTH *has* a wireframe map, but only in dev mode (`key_toggle_wireframe_map` is gated on `g_dev_mode_flag&1`; C/input.c:413-420; C/automap.c). "Players never had one" is the accurate statement.
- **Intermissions:** `nointermission`, as above.
- **Doom HUD:** replaced by the status bar class above.
- **Doom player class and weapons:** GAMEINFO `PlayerClasses = RealmsPlayer`, no start items. **[inferred]**
- **Cheats:** force `sv_cheats 0` and drop the cheat binds **[inferred]**. ROTH's own dev keys, if wanted, are core input.
- **Doom use-line path:** retire the `P_ActivateLine` hook and the `CheckUse` override.
- **Strife conversation code, bots and the title demo loop:** unreachable, so leave them.

---

## 6. The rules–presentation seam (function names only)

**Core ← engine (lifecycle and clock)**
- `Core_Init(installPaths)`, `Core_Shutdown()`
- `Core_NewGame()`, `Core_BeginLevel(rawImage, mapName, fromSnapshot)`, `Core_EndLevel()`
- `Core_Tick()` / `Core_AdvanceTicks(n70)`
- `Core_SerializeGlobals(arc)`, `Core_SerializeLevel(arc)`
- `Core_OnMovieFinished(id, skipped)`, `Core_OnPuppetRendered(objId)`

**Core ← presentation (intents; queued, consumed on the next tick)**
- Player input: `Intent_MoveInput(bits, mouseDx, mouseDy)`, `Intent_Turn(delta512)`
- World interaction: `Intent_Use(target)`, `Intent_Examine(target)`, `Intent_Fire(aimX, aimY)`
- Items: `Intent_SelectItem(slot)`, `Intent_UseItemOn(slot, target)`, `Intent_CombineItems(a, b)`, `Intent_DropItem(slot)`
- Screens: `Intent_OpenInventory()`, `Intent_CloseInventory()`
- Dialogue: `Intent_DialogueChoice(i)`, `Intent_DialogueAdvance()`, `Intent_SkipVoice()`
- Examine pages: `Intent_ExaminePage(delta)`
- Menu actions: `Intent_MenuNewGame()`, `Intent_Save(slot)`, `Intent_Load(slot)`

where `Target = { kind: Face|Object|SectorFloor|None, id, hitPos, dist }`, built by presentation's picker.

**Presentation → core (read-only queries)**
- Mode: `Query_Mode()` (play / inventory / dialogue / examine / dead / movie)
- Hover target: `Query_CursorKind(target)` (what `classify_cursor_target_object` would say)
- Player: `Query_PlayerView()` (pos, angle512, viewZ, bob), `Query_Health()`, `Query_DamageFlash()`
- Items: `Query_Inventory()`, `Query_HeldItem()`
- Weapon: `Query_WeaponPose()` (sprite, frame, x, y)
- Text: `Query_DialogueState()` (lines, choices, speaker, portrait), `Query_ExamineState()` (image, page, text), `Query_TextRuns()` (positioned glyph runs, dwell)
- HUD: `Query_HudPanels()`

**Core → engine (`IEngineSink`, implemented by the bridge)**
- World: `SetSectorPlanes(sec, floorZ, ceilZ)`, `SetSectorTexture(sec, plane, tex)`, `SetSectorLight(sec, level)`, `SetSideTexture(face, part, tex)`, `SetSideOffset(face, part, u, v)`, `SetDoorAngle(doorId, angle)`, `SetPlatformHeights(platId, lo, hi)`
- Puppets: `SpawnPuppet(objId, kind, pose)`, `SetPuppetPose(objId, pos, angle, sprite, frame, flags)`, `DestroyPuppet(objId)`
- Player: `SetPlayerPose(pos, angle, viewZ)`
- Audio: `PlaySfx(handle, sfxId, sourceKind, pos, vol, loop)`, `StopSfx(handle)`, `StartVoiceStream(cb)`, `StopVoiceStream()`, `PlayMusic(id, loop)`, `StopMusic()`
- Movies: `StartMovie(gdvName, id)`
- Map change: `RequestChangeMap(name)`
- Diagnostics: `LogUnhandled(opcode)`

The core includes none of the engine headers, and the bridge includes both sides. The flat UI and the later VR UI both implement only the picker, the intents and the drawing from queries. The core is untouched.

---

## 7. Risks and engine changes

| # | Item | Size | Upstream? |
|---|---|---|---|
| 1 | Core tick call in `P_Ticker` / `WorldStep` (p_tick.cpp not staged) | S | No; Realms-specific. Or generalise it as a "native world-step listener". |
| 2 | Native module serializer registry (globals.json + level) | S | **Yes**, generic and default-empty |
| 3 | Memory-backed sfx (`sfxinfo_t` PCM, or a virtual-lump `ReadSound`) | S | **Yes** |
| 4 | Music from a memory `FileReader` (`MusicCallbacks` hook) | S | **Yes**. Not needed if the music files are loose on disk. |
| 5 | `GdvPlayer` + `OpenMovie` branch + absolute-path open | M (decoder port) / S (engine) | Format branch yes. The decoder is Realms content, so arguably not. |
| 6 | In-level movie gamestate return (`ga_playmovie`) | S | **Yes** |
| 7 | Per-pawn `P_BobWeapon` opt-out | S | **Yes** |
| 8 | Renderer "rendered this tic" flag per actor | S–M | **Yes** (generic visibility query) |
| 9 | Polyobject absolute-angle set with interpolation; sector plane set with interpolation | S–M; po_man and interpolation not staged **[open]** | **Yes** |
| 10 | Door slabs as one-sided polyobject lines (ROTH_STATE.md:180-186) | M | Loader-only |
| 11 | Runtime font from ROTH glyphs (`FFont` subclass) | M; option A is S with no engine change | Maybe |
| 12 | Core port itself: raw image, `RealmsState`, ~60 command and tick handlers, entity AI, collision, inventory, dialogue, weapons | **L** | n/a |

**Risks**
- **Dual authority.** If any engine thinker also moves a puppet, sector or polyobject, the state diverges. Puppets must be inert, and the core is the only writer.
- **Pick fidelity.** The original picks the exact pixel, transparency included (the renderer's hit record). An engine line trace hits bounding boxes and lines. Accepted as presentation. Document which targets differ.
- **Render-coupled rules**, such as the monster spawn-on-draw and the face "seen" bit (`face[+0xA]|=0x40`; rothc_rules §1). Their uses need auditing, because VR draws twice per frame.
- **Save compatibility:** engine-format saves only. Importing the original `SAVEn.SAV` is possible later, since the chunk format is documented (savegame.c:1512-1520). **[open]**
- **Collision units** (`0x70` vs `0xc400`) are unresolved. This blocks item 12's collision port until they are read from the disasm.
- **Process:** under the project rules, items 2-4, 6-9 and 11 are "propose before building" (R/ROTH_ENGINE_CHANGES.md:30-44). ROTH_ENGINE_CHANGES.md must first be updated to record the changes already made.
