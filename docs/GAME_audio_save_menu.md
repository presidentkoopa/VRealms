# REMAROTH game layer — Audio, Save/Load, Menus/HUD/Flow, Automap

Spec reader 5 of 6. Source of truth: ROTH.C `src/engine/`. Paths below are relative to that directory unless a different root is named. Addresses are canon VAs. Tags: [inferred] = my reading of the code, not stated in it. [open] = unresolved. Player position globals, confirmed by `map_load.c:552-553` and `savegame.c:146-148`: **X = 0x90a8c, height = 0x90a90, Y = 0x90a94**, all 16.16. The `VA_g_player_angle/_x/_z + 2` macro names do not match these meanings.

---

## 1. What the player experiences

- **Audio.** Positional sound effects play from monsters, doors, weapons, landings and scripted triggers. They pan left/right and fade linearly with distance. Map-placed ambient emitters fire as one-shots, loop with a fade-in, or repeat on a timer while the player is within range. Invisible rectangles ("zones") can muffle an emitter. Music is one HMI/HMP song at a time, loaded from DBASE300. Only DBASE100 script opcodes change it, never a map load. The song loops forever and ducks around full-screen images and cutscenes. Speech is streamed from DBASE500.
- **Save/Load.** There are nine slots (`SAVEGAME\SAVE<n>.SAV`) plus a quicksave slot 0. A save holds the player, the inventory, the global story flags, the music track, and **a snapshot of every map visited so far**. Returning to a map, whether by walking back or by loading a save, restores its doors, lights, objects, monsters and ambient emitters as they were left.
- **Menus/HUD/flow.** Boot runs a scripted intro and then the main menu (Play / Load / Settings / Quit). Once in the game, the options/pause menu is reached from the inventory screen. It offers Resume / Load / Save / Settings / cutscene gallery. The HUD is a status panel with a health bar (it swaps to a "low" image below half), a weapon/ammo panel, the held item stuck to the cursor, and a top-left character-portrait corner widget. On death a death script runs, then a short wait (Esc skips it, a key quick-loads). The game then resets to the start map and returns to the main menu. Level exits load the target map and place the player in the centre of the arrival sector.
- **Automap.** The retail game has no automap. `automap.c` is a developer wireframe overlay behind a dev flag that nothing in the retail keymap sets.

---

## 2. Audio

### 2.1 SFX bank (FX22.SFX / FXSCRIPT.SFX)
- The file name comes from the CONFIG.INI "SFX" asset field (`file_config.c:840`). It is loaded once at boot by `load_sfx_file_wrapper` (`game_core.c:1053`, `audio.c:1104-1112`). The vanilla pack is `DATA/DATA/FX22.SFX` (rotheditor_rules.md:12).
- **Header** (0x1c bytes): +0 magic `"0XFS"` (dword 0x53465830), +8 FAT offset, +0x10 FAT size in bytes (`audio.c:1069-1087`, `fxscript.gd:4-13`). Sample count = FAT size / 12 (`audio.c:1085`).
- **FAT row** (12 bytes): +0 file offset, +4 byte size, +8 u16 sound key, +0xa type/flags (1 = 16-bit 11025 Hz, 3 = 16-bit 22050 Hz), +0xb runtime slot byte, set to 0xff at load (`audio.c:1088-1092`, `fxscript.gd:15-21`).
- Flag bits read by the engine:
  - bit0 = loop: voice flags 0x80 when set, 0x20 (one-shot) when clear (`audio.c:674-675`).
  - bit1 = 22 kHz (`audio.c:676-680`).
  - If `(flags & 0xa)==0` and memory is not low, the sample is upsampled 2x by interpolation at load (`audio.c:457-479`).
- **Lookup.** Scripts store a sound *key*. `find_sound_sample_index` maps key to 1-based FAT row by matching +8 (`renderer.c:11405-11418`). `resolve_dbase100_sound_ids` rewrites DBASE100 sound opcodes {0x19,0x21,0x29,0x2a,0x31,0x32} in place at startup (`audio.c:1893-1924`, called from `game_core.c:428`). Every play site passes `id-1`, where 0 means silent (e.g. `raw_commands.c:1295`, `dbase100.c:849`).
- **Cache.** Up to 32 loaded samples live in a descriptor table. Eviction first takes the oldest unlocked sample, then the oldest one past its grace time, stopping its voices (`audio.c:443-539`). A port may use the engine's sound cache, but "≤16 simultaneous SFX" is a rule (`find_free_slot_83ed4`, `renderer.c:1432-1440`). When all 16 handles are busy, new sounds are **dropped**, not stolen (`audio.c:741`, `783`).

### 2.2 Emitter kinds and play entry points (all return 0 when sound is disabled)
| Entry | Emitter / range | Group | Notes |
|---|---|---|---|
| `play_sound_effect(id,param)` | global, non-positional (rec flags 0x20) | 0x6e | full SFX master volume; not updated per frame (`audio.c:756-763`, `691-728`) |
| `start_persistent_looping_sound` | same body | 0x20 | used for landing thud / variant sounds with no coords (`audio.c:765-771`, `player.c:280-296`, `audio.c:993-996`) |
| `play_entity_sound(id,p,x,y)` | static emitter 0x83eb0, **range 2000**, vol 0x40 | 0x20 | monsters, weapons, doors (`audio.c:827-833`; ranges set `audio.c:1062-1063`) |
| `play_entity_object_sound` | static emitter 0x83ec2, **range 1000** | 0x20 | `audio.c:835-840`, `entity_ai.c:1271` |
| `play_command_sound` | emitter 0x83eb0 | 0x64 | RAW chain done-tail with a target position (`audio.c:842-851`) |
| `play_world_sound_at_pos` / `_squared_dist` | a node-layout record (e.g. door +0x14, range forced 1000) | 0x20 / 0x6e | via `play_sound_unique` (`audio.c:895-921`, `952-970`) |
| `play_sound_unique(entry)` | map SFX node | running | refuses when the node is already playing (`audio.c:853-893`) |

### 2.3 Map SFX nodes and zones (RAW section 7)
- **Load** (`map_load.c:466-479`): section base → `g_sfx_nodes` [0x85c44]; zone list base = base + u16 at base+0 (`sizeA`); node count = u16 at base+2; nodes start at +4.
- **Node (0x12 bytes)**, from the engine reads plus `raw.gd:141-154`:

| off | type | meaning |
|---|---|---|
| +0,+2 | s16 x,y | position, map units (`audio.c:635-636`) |
| +4 | u16 | sound id (`audio.c:887`) |
| +6 | u16 | key for `cmd_activate_sfx_node` (`audio.c:166`, `raw_commands.c:4855-4879`) |
| +8 | u8 | bit7 = active; bits0-2 = mode (`audio.c:616-618`) |
| +9 | u8 | zone index, 1-based; 0 = none (`audio.c:870`, `84-86`) |
| +0xa | u16 | range (`audio.c:68`, `638`) |
| +0xc | u16 | period, modes ≥2 (`audio.c:624`) |
| +0xe | u16 | runtime timer (`renderer.c:10134-10150`) |
| +0x10 | u8 | volume, 0x40 = unity (`audio.c:82-83`) |
| +0x11 | u8 | [open] unused by audio.c |

- **Zone record (0x20 bytes)** at `zones + (idx-1)*0x20`: u16 rectCount (≤3), then rectCount × 10 bytes {u8 dampen, u8 flags, s16 xLo, s16 yLo, s16 xHi, s16 yHi}. The rects are **relative to the emitter position** (`audio.c:86-101`, `raw.gd:156-175`).
- **Per-tick pump.** `play_nearby_sfx_emitters` runs from `update_player_tick` (`player.c:420`; `audio.c:938-950`).
  1. It collects at most 32 **active** nodes with dx²+dy² ≤ range² (unsigned) around the player (`audio.c:597-648`).
  2. It bubble-sorts them by distance (`audio.c:178-208`).
  3. It plays each with `play_sound_unique`.
- **Modes** (`audio.c:619-633`, `859-893`, `1970-1974`):
  - **0 one-shot:** fires once, then bit7 is cleared. It is *also* cleared if the computed volume is 0, for example when fully zone-dampened (quirk).
  - **1 looping ambient:**
    - The start volume is capped at `max(0x7ff - range, 0x10) << 4`.
    - `update_active_sounds` then slews the volume toward its target at ≤0x200 **per frame**, not time-scaled. This is a fade-in.
    - On buffer drain the voice is re-queued while the node is active, mode==1 and the applied volume is ≠0. It therefore stops once it has faded to 0 out of range.
  - **2 periodic:** the timer decrements by the frame scale [0x85324]. On expiry it reloads with `(7*period)>>1` minus the overshoot (clamped ≥0), and the node plays that pass.
  - **≥3 randomised periodic:** the same, with the period first multiplied by the 16-bit word [0x85328] >> 16. [inferred] [0x85328] is a per-frame random/jitter word.
- **Level setup.** `setup_sfx_nodes` primes timer = `(7*period)>>1` for active nodes of mode >1 (`renderer.c:10134-10150`, from `map_load.c:916`, `928`).
- **Script control.** `cmd_activate_sfx_node` (base 0x10) toggles bit7 on every node with key == rec+8 (`raw_commands.c:4855-4879`):
  - rec+6 & 6 → force the bit on.
  - rec+6 & 4 → force it off.
  - If the node just turned off and rec+6 & 1, its voice is **stopped** (`stop_sound_handle_voice`).
  - Unless rec+6 & 0x20, the record self-disables (rec+2 |= 8).

### 2.4 Volume / pan maths (`compute_sound_volume_pan`, `audio.c:65-114`)
```
d   = isqrt(dx²+dy²)                         ; dx,dy player-relative, map units
v   = 0x7fff - d*0x7fff/range                ; <0 -> 0 (silent, no master scaling)
v   = (v * vol8) >> 6   if vol8 != 0x40
if zone: rel=(player - emitter); for rect in zone (in order):
     hit = flags&1 ? strictly inside rect : outside any bound
     if hit: v -= dampen<<7; if v<0 -> 0; break      ; FIRST hit only
v   = (uint32)(v * SFXmaster) >> 15 ; clamp 0x7fff   ; SFXmaster = [0x71d84], 0..0x7fff
```
- Pan for nodes: rotate into camera space, then `0x8000 + lat*0x7400/(d+1)`, clamped to [0,0xffff] (`audio.c:37-52`).
- Per frame, `update_active_sounds` (from the render setup, `render_world.c:84`; `audio.c:1006-1048`) does the following for every positional handle:
  - recomputes the volume when |Δd²|>>4 ≠ 0;
  - slews the applied volume by ≤0x200;
  - re-pans when |Δpan|>>8 ≠ 0.
- Non-positional handles (flag 0x20) are never updated.
- **Quirk (keep, or document the deviation).** `play_object_sound` computes the *initial* pan with dy = y − **height** [0x90a90] instead of Y (`audio.c:803-805`). The first frame of `update_active_sounds` corrects it.

### 2.5 Which events play which sounds
| Event | Call | Source |
|---|---|---|
| RAW trigger chain ran and "acted" | trigger record +0xa = sound id+1, latched when fired (`raw_commands.c:1665-1673`, `2868-2882`). Played at the end of the chain as a global SFX, or positional at the latched point unless the point = 0x80008000 | `raw_commands.c:1285-1300` |
| RAW height/sector movers | `play_entity_sound` / `play_command_sound` / `play_world_sound_squared_dist` / `stop_sound_handle_voice` | `raw_commands.c:2525-2551`, `2738-2760` |
| Switch toggle | `play_sound_effect(snd-1,0)` | `raw_commands.c:2957` |
| Doors | the door record's sound id+1 (0 = silent) starts a 1000-tick open timer; stopped at the closed limit; restart plays at the door pos with range 1000 | `doors.c:620-634`, `860-880`, `950-964`; `audio.c:952-970` |
| Monster sounds | `play_entity_sound`; `play_distance_variant_sound` picks a variant (u16 ids at def+0x24, count +0x22). Beyond threshold def+8 → last variant; nearer → RNG pick | `entity_ai.c:99`, `1169`, `1271`; `audio.c:972-997`; `weapon_combat.c:147-161` |
| Weapon fire | `play_sound_effect(id-1, 0xfcff)`; item-object sounds | `weapon_combat.c:880-898`, `180-236` |
| Hard landing | `start_persistent_looping_sound` when fall speed > 0xe | `player.c:280-296` |
| Object spawn from DAS | `play_entity_sound` | `das_assets.c:243` |
| DBASE100 op 0x19 | `play_sound_effect(operand-1, 0xfcfe)` | `dbase100.c:846-849` |

### 2.6 Music
- **Storage.** DBASE300.DAT. A chunk *index* is a byte offset / 8. At `index*8` sit a u32 size (≤0x11800) and the HMP data (`file_config.c:591-625`). The HMP header is "HMIMIDIP013195": +0x30 track count, +0x38 tempo/tick rate, tracks at +0x388, per-track device preferences at +0x80 + t*0x14 (`audio.c:1603-1742`, `hmp.gd:4-19`).
- **Changing tracks.** `load_dbase300_chunk(idx)`:
  - No-op if idx == current [0x7f46c].
  - Otherwise it records idx, **stops the current music**, and returns if idx==0 (music off).
  - Otherwise it reads the chunk and sets phase=1.
- **Starting.** `process_audio_sequence_chunk` inits the sequence, starts its timer at the song's rate, and emits volume 0x7f then the master volume. Phase bits: 1 loaded, 2 inited, 4 playing (`audio.c:1761-1784`).
- **Looping.** `service_audio_sequence` runs every frame (`game_core.c:847`, `980`). When the driver clears the running flag it re-parses and restarts: **every song loops forever** (`audio.c:1824-1833`).
- **Who changes music** (callers of `load_dbase300_chunk`):
  - DBASE100 **op 0x1a** loads chunk `operand`, then starts it. It re-queues itself while dialogue is busy, and un-ducks first if state==2 (`dbase100.c:786-800`).
  - **op 0x1b** stages the current chunk id as a pending topic. `finalize_dbase100_chain` reloads and starts it (`dbase100.c:774-779`, `298-320`).
  - Save chunk 14 (`savegame.c:1449-1459`).
  - **No map-load path touches music** (grep of all callers). Music per map is therefore whatever the map's scripts request through DBASE100 records. RAW opcode 0x2b runs a record (ROTH_STATE.md:245-247).
  - At boot `process_audio_sequence_chunk` runs with nothing loaded (`game_core.c:1058`), so there is silence until a script picks a track [inferred].
- **Ducking.**
  - Full-screen image slide-in/out ramps the music volume by 2/tick over 0x3e ticks and toggles duck state [0x83c4c] (0 none / 1 ramping / 2 ducked) (`blit_2d.c:627-705`).
  - Cutscene exit restores the volume (`gdv_cutscene.c:2427-2471`).
  - `resume_music_sequence` / `finalize_audio_sequence_ref` call the driver's restore/mute channel volumes (`audio.c:1799-1814`).
- **MIDI device selection** (`audio.c:1694-1733`) and the MELODIC/DRUM.BNK AdLib banks belong to the SOS driver, which is host-replaced and not lifted (`audio.c:6-9`). The ROTH.C host uses a SoundFont (README "--sf2"). **Port:** hand the HMP bytes to the engine's MIDI player. [inferred] ZMusic reads HMI/HMP. The only rules to keep are loop-forever, track=chunk index, volume mapping, and ducking.
- **The MIDI folder** is not referenced by the engine (grep: no `.BNK`/`MIDI\` string in `src/`). [open] It is probably only for the DOS setup/driver.

### 2.7 Volumes
- `apply_audio_volume_settings` reads the sliders SFX [0x71b34], speech [0x71b40], movie [0x71b4c] and music [0x71b58] (each `&0xfff`).
- SFX, speech and movie use `<<7`, clamp 0x7fff. Music uses `>>1`, clamp 0x7f (`audio.c:1863-1882`).
- The values persist in ROTH.INI through `read_roth_ini`/`write_roth_ini` (`game_core.c:1044`, `1102`).

### 2.8 Speech (for the dialogue reader)
- Clip = DBASE500 at `clip*8`, with a 0x2c header: +0x14 tag `'*'`=DPCM, +0x18 rate, +0x28 size (`audio.c:1201-1292`).
- It streams with 2×0x8000 ping-pong buffers. The end of the clip resets the dialogue timers and advances the dialogue queue (`audio.c:1294-1327`).
- A user skip goes through `try_interrupt_dialogue_voice` (`audio.c:1368-1379`).

---

## 3. Save/Load

### 3.1 File and chunks (`write_savegame_file` `savegame.c:1522-1633`, `load_savegame_file` `1357-1501`)
- Path: `<savedir 0x76540>\SAVEGAME\SAVE<n>.SAV`.
- Stream of `{u16 id, u16 size}` + payload. Written in this order:

| id | size | content | load action |
|---|---|---|---|
| 1 | 3 (header only, no payload) | version marker | size<3 → return 0 (leaks handle); else `delete_temp_files()` |
| 0xa | 0x30 | slot name (from 0x83d3c) | skipped (menu reads it: `1262-1295`) |
| 0xb | 0x1130 | thumbnail (only if captured) | skipped (menu: `1305-1337`) |
| 2 | 0x30 | player record (below) | `read_player_state_chunk` |
| 3 | 0xe | current map base name (no dir/ext, from `split_path`) | → 0x701ec |
| 0xe | 4 | current DBASE300 music chunk id | `load_dbase300_chunk` + start if ≠0 |
| 7 | 0x400 | inventory slots 0x80c30 (256 × 4 bytes, u16 id at +0) | recount nonzero → 0x80c2c, rebuild weapon list |
| 4 | 0x14 | inventory cursor list positions 0x80afc | raw |
| 5 | 0x14 | inventory scroll offsets 0x80b10 | raw |
| 6 | bitmap size | DBASE100 flag bitmap (433 records → 56 bytes; ROTH_COMMANDS.md:270-280) | raw |
| 0xd | record count | per DBASE100 record: the high byte of +0x10 ("cutscene seen" counter) | folded back into +0x10; +0x13 = 0; `g_cutscenes_seen_count = max+1` |
| 8 / 9 | 0xe / filesize | pairs per `*.TMP` in the save dir: name, then file bytes (`bundle_level_states` `1138-1192`) | 8 → name buffer; 9 → recreate that .TMP (`copy_save_chunk_to_file` `1233-1251`) |
| 0xc | 0 | end marker | success |

- **Player record (0x30)** (`savegame.c:139-195`):

| off | field |
|---|---|
| +0 | X 16.16 |
| +4 | height 16.16 |
| +8 | Y 16.16 |
| +0xc | u16 angle |
| +0xe | u16 sector |
| +0x10 | secondary equipped item, offset from 0x80c30 (0 = none) |
| +0x14 | primary equipped item, offset from 0x80c30 (0 = none) |
| +0x18 | health |
| +0x1c | view pitch |
| +0x20 | applied pitch |
| +0x24 | 0x81e30 (DBASE100 op 0x37 accumulator) |
| +0x28 | u16 eye height |
| +0x2c | dword 0x8c114 |

  Quirk: on load the sector goes to 0x89f36, not 0x90c12, and eye height and 0x8c114 are *not* restored (`savegame.c:168-195`).
- **After a successful load** (`savegame.c:1474-1498`):
  1. The map name is copied to `g_warp_dest_a`, cut at `.`.
  2. `g_map_first_load_flag` = 0 and 0x701ec[0] = 0.
  3. The weapon is re-activated and the icon rebuilt.
  4. `[0x7138c]` = slot, the "last slot" used for quick-load on death.
  5. `process_map_warp_or_load` then does a full reload. Because 0x701ec is empty, `write_raw_state_temp` is skipped (`map_load.c:1001`), so the restored .TMP files survive. The player keeps the saved X/Y/height because no relocation runs with flag 0 (`map_load.c:635-662`).
- **Slots.** The menu reads 1..9 (`savegame.c:1266`). Quicksave/quickload use slot 0 (`input.c:545-559`). An existing slot prompts before overwrite (`savegame.c:1539-1548`).

### 3.2 Per-map state (.TMP), written on leaving a map and at every save
- Written by `write_raw_state_stream` (`map_load.c:694-807`) to `<savedir>\<MAPNAME>.TMP` (`996-1020`). Chunks in order:
  1. **Geometry delta.** The live geometry block (length = u16 at +0) goes through `strip_transient_flags_for_save` (`savegame.c:88-127`) and is then literal/skip-encoded against the pristine .RAW bytes. Header {0xffff, len}. Codec: 0 = end, 0x01-0x7f = N literals, 0x80-0xff = skip N-0x80 (`renderer.c:250-266`). If the .RAW cannot be opened, the literal block is written.
  2. **Objects buffer** 0x90aa4, verbatim, length u16 at +0.
  3. **Command-record patch.** u32 len, then per command record a few type-specific bytes plus rec[2] (types 0x02, 0x0a/0x0b, 0x0d, 0x15/1f/21/22, 0x34; `map_load.c:752-783`).
  4. **Active-effect/link list.** Typed records (types 2,3,7,9-0xf,0x11,0x12,0x1d,0x23), pointers stored as offsets, trailer 0xfffe + 4 words, then 0xffff (`savegame.c:211-299`, `547-644`).
  5. **Entity pools.** u32 count + 16 × 0x22 state entities, u32 count + 16 × 0x1c dynamic entities (`renderer.c:754-759`, `savegame.c:452-496`).
  6. **SFX-node active bits**, MSB-first, 32 per dword (`renderer.c:268-290`, `audio.c:210-245`).
  7. `"EXIT"` tag.
- **Revisit rule.** On entering a different map, `open_raw_state_temp` looks for that map's .TMP. A nonzero handle is passed to `set_state_record_count`, so the first-visit object-init handlers do **not** re-impose the authored light/door state (`map_load.c:653-662`). `load_raw_state_from_temp` then applies the stream, or runs the fresh-map init when there is no .TMP (`map_load.c:1026-1037`).
- **Reset.** New game and boot run `delete_temp_files` (`game_core.c:449`, `1040`).

### 3.3 Global vs per-map
- **Global:** player record, inventory and UI cursors, DBASE100 flags and seen-counters, music chunk id, current map name (chunks 2-7, 0xd, 0xe).
- **Per-map:** everything in §3.2.
- **Not saved:** playing SFX, dialogue/voice state (stopped on load, `savegame.c:1367`), damage flash, movement queues.

### 3.4 1:1 serialization requirement
REMAROTH must be able to hold every field above as its own state. That means, beyond position/health/inventory:
- the per-sector/wall/object flag bytes the .TMP delta covers;
- per-command-record bytes;
- the active-effect list;
- both entity pools;
- per-node SFX active bits;
- the DBASE100 bitmap and seen counters;
- the music chunk id.

Round-tripping original .SAV files is feasible only if the port keeps RAW-order indices for sectors/objects/commands/nodes. **Recommend:** keep those indices as stable IDs and implement a SAV importer/exporter as a separate layer.

---

## 4. Menus, HUD, game flow

### 4.1 Boot and new game (`game_core.c:1036-1141`, `742-845`)
1. `roth_main_sequence` parses CONFIG.INI paths, deletes *.TMP, loads the SFX bank, loads ICONS.ALL/backdrop, runs `init_game_databases`, and loads ADEMO.DAS. It copies the CONFIG.INI map/DAS args into "default" buffers 0x7037c/0x7032c, loads that DAS+RAW, applies volumes, and enters `game_play_loop`.
2. `game_play_loop` runs **DBASE100 record 1** (`game_core.c:757`), [inferred] the intro/title script. It then sets health = max (def value [0x7fe44], default 0x800, `game_core.c:767-772`).
3. When the state selector is 2 it shows the main menu (`game_core.c:806-816`).
4. Main-menu **Play** (code 0xa) runs new-game setup (`game_core.c:821-834`):
   1. clear the inventory cursors and the inventory;
   2. run **DBASE100 record 3** ([inferred] new-game script: intro GDV and starting items — `dbase100.c:234-237` names give_item as "the new-game starting-items chain");
   3. set health = max;
   4. set no weapon;
   5. set mode 1 (gameplay).
- The start map is the CONFIG.INI map arg [open: confirm STUDY1 from the retail CONFIG.INI].

### 4.2 Screens
- **Main menu** (`run_main_menu`, `menu_hud_ui.c:2079-2107`):
  - row ≤1 → Play. Esc also returns Play, because raw 0 hits `m<=1` first — a quirk; `raw==0 → quit` is unreachable.
  - 2 → Load (slot list, 5 visible rows).
  - 0x3e8 action codes → settings results.
- **Settings** (`run_settings_menu`, `1931-2027`):
  - case 0: quit confirm → 0x203e9.
  - case 1: submenu with Volume (4 sliders), Subtitles toggle, Screen (VESA mode list → 0x503e9 apply / 0x603e9 low-memory), Input.
  - case 2: confirm → 0x703e9 [inferred restart].
  - case 3: info box.
- **In-game options** (`run_options_menu`, `2036-2073`): opened from the inventory screen (`inventory.c:1799-1822`). Result dispatch:

| result | action |
|---|---|
| 1 | leave the loop (`[0x7f360]=0x64`) |
| 2 | save slot |
| 3 | pending load |
| 4 | resolution |
| 6 | pending restart (`g_pending_game_action` bit 8 → new game) |

  0x7d0 opens the cutscene gallery.
- **Esc in gameplay** shows the quit prompt 0x71a00. Yes → `[0x7f360]=0xffff`, which exits the frame loop (`input.c:173-186`, `70-78`). [open] Whether that path lands in the main menu or quits to DOS depends on the ebx left over from the bridged callees.
- **Modes** (`g_player_movement_enabled` 0x7674a, `game_core.c:861-921`): 1 gameplay, 3 inventory/UI, 4/5 dialogue (Enter/Up/Down), 8 transitional, 0x20 dead.

### 4.3 HUD (all presentation except the numbers they read)
| Element | Reads | Source |
|---|---|---|
| Health bar | `fill = width*clamp(health,0,max)/max`, min 1 px if health>0; below half → image 0x298, else the normal image; empty part = alt image | `menu_hud_ui.c:290-418` |
| Weapon/ammo panel | active weapon attrs 0x811b4, anim frame attrs+0x48 (0 = hidden) [inferred: the skull icons are ammo/charge pips] | `weapon_combat.c:984-994`, `menu_hud_ui.c:745-780` |
| Held item at cursor | `g_selected_item_primary`; scale-in animation | `inventory.c:1456-1500` |
| Portrait corner (0,0,0x24²) | grows while the cursor is in the top-left 32×32, shows the active-item icon | `game_core.c:521-533`, `menu_hud_ui.c:255-288` |
| Damage flash | [0x89f3b] decays by 4×frame scale; palette tint | `game_core.c:481-487` |
| Layout | resolution-dependent offsets | `menu_hud_ui.c:420-459` |

### 4.4 Death (`game_core.c:959-1020`)
1. health ≤ 0 → leave the frame loop; mode 0x20; unequip.
2. Run **DBASE100 record 4** [inferred death script/GDV].
3. Loop up to ecx ≥ 0x12c frame-time units, still servicing music and entities:
   - Esc (code 1) ends the loop;
   - code 0x39 quick-loads the last slot [0x7138c] (the code comment says F9; 0x39 is the Space scancode — [open]).
4. Then `dialogue_voice_stop_all` and `reset_and_start_new_game` (`game_core.c:438-471`): wipe *.TMP, reload DBs and the default map. Back to the main menu.

### 4.5 Map transition
- **Record.** `cmd_map_transition` (base 0x3b) latches (`raw_commands.c:521-534`):
  - name dwords rec+0xa/+0xe → 0x8547c/0x85480 (NUL-terminated; stray bytes after the NUL are possible, ROTH_COMMANDS.md:284-305);
  - arrival key u16 rec+8 → 0x85484;
  - `g_pending_game_action=1`;
  - rec+6 & 0x10 → one-shot.
- **When it runs.** It is consumed at the end of the frame (`game_core.c:947-954`).
- **`process_map_warp_or_load`** (`map_load.c:574-670`):
  - The name compare is case-insensitive up to 8 chars or `.`.
  - **Same map:** relocate only.
  - **Different map:** write the current .TMP; build `<name>.RAW`; swap the DAS through the map list; unload; reset the entity pools; load the RAW; restore/init from .TMP; relocate.
- **Arrival.** Find the first sector (skipping the current one) whose +0x14 == key. The player goes to the **centre of that sector's vertex bounding box**. Height shifts by the floor delta (old sector floor − new sector floor, `map_load.c:512-567`). Angle is unchanged [inferred]. **key 0 = no relocation:** X/Y carry over unchanged (e.g. CAVERNS→CAVERNS2).
- **What carries over:** all globals (health, inventory, equipped items, flags, music keeps playing), because nothing in the path touches them.

### 4.6 End of game
No end-of-game code exists in the engine files read. [open] It is presumably a DBASE100 record (final GDV) followed by a quit/menu action.

---

## 5. Automap
- `automap.c` is a **developer wireframe overhead map** (`automap.c:1-40`). It draws Bresenham lines of walls, doors and entity markers, plus a player arrow, into the render target (`automap.c:425-446`).
- It draws only when `g_debug_map_enabled` [0x7f36e] is set (`game_core.c:348-382`). The toggle key does something only if dev flag [0x7f560]&1 is set, and otherwise forces the map off (`input.c:413-421`).
- The dev flag is set only by `enable_dev_mode` (`renderer.c:9748-9751`), which has no caller in `src/` and is not in the keymap (`input.c:136-176`).
- **Retail players cannot reach it. The port disables the engine automap** (ROTH_STATE.md:249-252).

---

## 6. Rules vs presentation

| Group | Port 1:1 (rules) | Replace (presentation) |
|---|---|---|
| SFX | sample-key lookup, id-1 convention, 16-handle limit and drop, emitter ranges 2000/1000, attenuation/zone/master maths, node modes/timers/activation, uniqueness, per-frame slew 0x200, loop requeue rule, sound-event table §2.5 | SOS voices, resampling, eviction cache, pan projection (use the engine's 3D panning, but keep volume/range) |
| Music | chunk index→song, change only via DBASE100 0x1a/0x1b and save chunk 14, stop-on-change, loop forever, duck states, volume mapping | HMP sequencer, device prefs, BNK/SoundFont |
| Speech | clip lookup, end-of-clip → dialogue advance | DPCM/ping-pong streaming |
| Save | chunk set, per-map .TMP content and revisit rule, delete-on-new-game, post-load warp without relocation, last-slot quickload | DOS file I/O, thumbnails, slot-name UI |
| Flow | records 1/3/4/2 at boot/new/death/quit, health=max on new game, death reset, pending-action bits, transition latching/arrival rule | menus, message boxes, slide transitions |
| HUD | health fraction, low-health threshold (<½), which weapon/ammo/item is shown | all drawing |
| Automap | — | remove / disable |

---

## 7. Dependencies, size, traps, open questions

**Depends on:**
- RAW loader indices (sectors +0x14 id, section 7);
- the DBASE100 interpreter (records, ops 0x19/0x1a/0x1b/0x37, flags bitmap) — reader "dialogue";
- inventory slots;
- entity pools/defs (variant tables) — reader "AI";
- the door pool;
- RAW command executor (done-tail SFX, 0x10, 0x3b);
- GDV player (ducking).

**Size estimate (C++/ZScript, rules only):**

| Part | Lines |
|---|---|
| SFX rules + node system | ~600 |
| music control | ~150 |
| save/load + .TMP per-map persistence | ~900 (plus ~400 for an optional .SAV importer) |
| flow/transition/death | ~300 |
| HUD data bindings | ~150 |
| **Total** | **~2,100** (plus ~400 importer) |

**Traps:**
1. The position globals are misnamed (see the header). The object-sound initial pan uses height (`audio.c:803`).
2. Mode-0 nodes self-disable when zone-dampened to 0.
3. Volume slew is per frame, not per tick. At 90 Hz VR frame rates this fades in faster than in 1996. Decide whether to tick at 35/70 Hz.
4. Zones: only the first hit rect counts; the rect test is inverted by flag bit0; rects are node-relative.
5. `play_sound_unique` blocks re-trigger only while the same node is playing.
6. Sound ids everywhere are +1 (0 = silent).
7. Arrival key 0 keeps X/Y. A matching sector is searched excluding the current sector.
8. The load path relies on 0x701ec being cleared so the .TMP files are not overwritten.
9. The sector field on load goes to 0x89f36.
10. The SFX active-state loader's scribble past the buffer end (`audio.c:210-245`) is harmless but real.
11. The main-menu Esc = Play quirk.
12. The first save chunk has size 3 with no payload.
13. u16 chunk sizes cap each .TMP at 64 KB (`bundle` writes u16 filesize).

**Open:**
- keymap scancodes for quicksave/quickload (table 0x7093d);
- the death quickload key (0x39);
- what DBASE100 records 1-4 contain (needs a record dump);
- the start map in the retail CONFIG.INI;
- where the Esc/option-1 exit lands (menu or quit);
- end-of-game path;
- node +0x11;
- the nature of [0x85328];
- mode 3+ semantics in data (count per map);
- MIDI folder usage.

---

## 8. Acceptance tests (run the same inputs in ROTH.C and REMAROTH)

Log hooks: `ROTH_SFX_TRACE` (`platform/audio.h:133-141`) and `ROTH_SAVEDBG` (`savegame.c:47-53`).

1. **Node range.** In STUDY1, pick a mode-1 node N (x,y,range). Stand at distance range+16: no voice (trace). Step to range−16: the voice starts. Its applied volume rises in 0x200 steps per frame to `0x7fff*(1-d/range)*vol/64*master>>15`. Walk out: it fades to 0 and does not requeue.
2. **Zone.** For a node with a zone rect (dampen D), stand at the same distance inside and outside the rect. The volumes differ by exactly D<<7 (before master scaling), with the side reversed when flag bit0 is set.
3. **Mode 0.** A mode-0 node plays once when entered and never again, including after leaving and re-entering. Save, reload: still off (active bit restored from .TMP).
4. **cmd_activate_sfx_node.** Trigger the RAW 0x10 record that targets key K: the node bit flips. With rec+6&1 the playing voice stops immediately.
5. **Door.** Open a door with sound id s: sample `s-1` plays at the door, range 1000, and stops when the door closes fully.
6. **Handle limit.** Fire 17 sounds in one frame: the 17th is silent.
7. **Music.** Boot to the main menu: no music until a DBASE100 0x1a runs. Enter the scene that runs op 0x1a with chunk C: song C starts, loops at end, and is unchanged by any map transition. Save, then load a save made while C played: C restarts.
8. **Duck.** Open a full-screen image: the music ramps down over 0x3e ticks and back up on close.
9. **Save round-trip.** In STUDY1, move to (X,Y,H,angle) with health h and item set I, open door D, pick up object O, kill monster M, and save in slot 3. Quit, load slot 3. Expect equal X/Y/H/angle (16.16 exact), health, inventory 0x400 bytes, and the flags bitmap. D stays open, O is gone, M is dead, the music chunk is the same.
10. **Revisit.** Change STUDY1 as in 9, take the exit to the next map, then return. The same state is restored, and authored door/light init did not re-run.
11. **Transition arrival.** Take the AELF exit (key 13 → STUDY1): the player lands at the bounding-box centre of the first sector with +0x14==13, with height shifted by the floor delta and angle unchanged. For a key=0 exit (CAVERNS→CAVERNS2): X/Y unchanged.
12. **Death.** Set health 0: record 4 runs. Wait out 0x12c units or press Esc: default map reset (all *.TMP deleted) and the main menu. Press the quickload key: the last saved slot loads.
13. **New game.** Play from the main menu: record 3 runs, health == 0x800 (or the def max), no weapon equipped, CONFIG.INI start map.
14. **HUD.** At health = max/2 − 1 the low-health bar image (0x298) shows; at max/2 the normal one. Bar width = `floor(W*h/max)`, minimum 1 at h>0.
15. **Automap.** The GZDoom automap key does nothing; there is no overhead map in any mode.
