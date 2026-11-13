# tools/gameproof -- the game layer against the running original

A differential test. The ORIGINAL engine (ROTH.C, the verified C reconstruction of
ROTH.EXE, built headless in `/home/claude/oracle_build`) loads `gameprobe.c` as a mod and
calls its own game-logic entry points; `replay.cpp` runs the same plan on this port
(`roth::GameCore`, `roth_itemcmds.cpp`); `compare.py` diffs the two traces call by call.
It is the regression test for anyone who touches the DBASE100 interpreter, the inventory,
GameCore or the RAW item commands.

    tools/gameproof/run.sh 0        # one stub-answer mode; exit 0 = no difference
    for m in 0 1 2 4 7; do tools/gameproof/run.sh $m || break; done    # the full set

Needs: the oracle build, g++, python3, the retail data (`ROTH_DATA`, default
`/mnt/user-data/uploads/Realms of the Haunting/DATA`; `ROTH/CONFIG.INI` beside it is used
when present). About 20 s of original and 1 s of port per mode. Everything is written to
`$PROOF_WORK` (default `/tmp/gameproof`). **The traces, `rawrecs.txt` and the game folder
are derived from retail data: never commit them.**

## Files

| file | what |
|---|---|
| `gameprobe.c` | the ROTH.C mod. Wraps the game-logic callees (give/remove/query/find/filter, RNG, weapon equip, read_next_dialogue_line, force-end, health) to LOG and pass through; STUBS the presentation and map callees (dialogue windows, GDV, images, sound, music, item icon, weapon HUD, the map's command chains) with fixed answers. |
| `run_oracle.sh` | builds the mod, makes a game folder of symlinks, runs the original under `/tmp/oracle_shot.lock`. |
| `replay.cpp`, `build_replay.sh` | the port side; same plan, same line format. `PROOF_SAN=1` builds it with ASan/UBSan. |
| `rawrecs.cpp` | lists every distinct 0x27/0x29/0x2a/0x2b/0x42 record of the 44 retail maps (488 of them). |
| `compare.py` | per-section exact-match counts, then the first divergent line of each mismatch. |
| `coverage.py` | what a trace actually reached (event and state counts). A section that matches but reached nothing proves nothing: read this too. |
| `run.sh` | all of it. |

## The plan (both sides, from the same snapshot)

After the original boots (GDVs skipped, the menu answered "new game") and runs its own
new-game block (game_core.c:821-833), the probe blocks the timer signals, prints the state
(`S0`) and snapshots game_ram, the flag bitmap and the DBASE100 image. Every test restores a
snapshot first, so tests are independent; the port does the same with its own state.

- **S0** -- the loader's rewrite of the DBASE100 image (`PATCH` lines:
  resolve_dbase100_sound_ids, audio.c:1898-1924; the port side uses the sound lane's
  `SfxBank::ResolveDbase100`) and the whole post-new-game state.
- **A** -- every action id 1..694 through `eval_dialogue_record_by_id` (what RAW 0x2b
  reaches, raw_commands.c:4705), then `finish_dialogue_record_eval`, then *driven*: a choice
  menu is answered (`close_dialogue_and_run_branch`, row k at the first menu, 0 after), a busy
  line is ended with the original's `voice_stream_pump`. One test per row of the first menu.
  Four starting states: S0; every flag set; a large inventory; every flag + full inventory.
- **B** -- every trigger block of every item record through
  `eval_dialogue_record_condition_with_cleanup`, modes 0/1/2, from S0 and from flags+full.
- **Q** -- two chains back to back with nothing ending the first line: chain 1 fires action
  id, its tail runs (raw_commands.c:1313-1315), chain 2 fires id+1 and id+2. This is where the
  queue fills and drains and lines are force-ended.
- **I** -- give, query (flags 0-3), remove, find-or-autoselect (count and select), a
  give/give/remove/select/remove/remove sequence, and the filter, for item ids 0..282, from
  four inventories (S0, every third item, a mixed 198-slot one, all 256 slots full).
- **R** -- the five RAW handlers (`cmd_if_not_item`, `cmd_give_item`, `cmd_remove_item`,
  `cmd_set_inventory_filter`, `run_command_dbase100_record`) on the retail records, from the
  four inventories; each 0x27 also with the 0x36-latched pending action that precedes it in
  its map; each 0x29 also with a world-object give context.
- **H** -- the item test per VR hand (`src/roth/roth_handuse.h`): every retail 0x27 record, then
  `cmd_remove_item` on its key, from the four inventories, easy flag off and on, with the right hand
  holding the setup's own item, the tested item, or nothing. The original is run with the right hand
  as each VR hand leaves it (the item hand: unchanged; the other hand: nothing selected); the port
  starts from the same right hand and goes through `HandUseBegin`. The restore after the use
  (`HandUseEnd`) has no counterpart in the original: `replay` checks it against its own rule and
  prints the count on stderr (and exits 3 if it is ever broken).

Each test prints: every logged call in order with arguments and return values (nesting by
indent), then the state: flags changed, inventory slots changed, hands, Adam slot,
autoselect, last item, pickup lock, slide timer, the dialogue queue and choice globals, the
RNG words, health/death/latches/panel/ammo, and every DBASE100 image byte changed.

Stub modes (`run.sh <mode>`): bit 0 = dialogue windows refuse to open; bit 1 = a DBASE300
music chunk loads "fresh" (so the sequence calls follow); bit 2 = the music is faded out.

## What the port side has to model, and where

`replay.cpp` models, citing ROTH.C, the bits of other systems the two ported systems read
back: `activate_weapon_item`'s ammo rule (weapon_combat.c:426-459), `voice_stream_pump`'s
tail (audio.c:1316-1326), `dialogue_voice_force_end` (audio.c:1335-1350),
`read_next_dialogue_line` without its cache (dbase100.c:371-403), `advance_clamp_8a0f0`
(renderer.c:9818-9824). If the engine grows those systems they must agree with these lines.

## Limits (what this does NOT prove)

- Presentation is stubbed on purpose: window contents are compared as length + CRC of the
  text the original would show, GDV/sound/images by id only.
- `prime_voice_clip` is stubbed, so no voice ever plays: the "voice still playing" branches of
  force-end and the pump, and the choice-highlight tail of force-end (audio.c:1343-1347,
  `freeze == 0x6ffff`) are never reached. `UNMODELLED` in a port trace marks the latter.
- `emit_music_sequence_event` (dbase100.c:313) was not reached in any mode (op 0x1b staging a
  topic plus a fresh chunk plus faded music never coincided in the data).
- `RUNAWAY`: a test that logs more than 4000 events is cut at that event on both sides
  (with every flag set some actions loop forever in the original too; it has no guard).
  `DRIVE-LIMIT`: a conversation still going after 200 drive steps (a menu that loops).
  Both match line for line up to the cut; nothing after it is compared.
- The world-object give context is one synthetic object; the drop-position path
  (0x8a0fc, raw_commands.c:4806-4821) needs a map and is not run.
- `g_object_select_easy_flag` (0x81e34) is always 0.

## Results (2026-11-12, lane/gamelayer cde9eb9d + the proof patch)

Every mode of 0 1 2 4 7: S0 1/1, A 2954/2954 (2888 in modes 1, 7: no menus), all 694 action
ids match in every variant, B 2982/2982, Q 1388/1388, I 10208/10208, R 2364/2364.
Before the fix: 0 of 694 matched (`g_choice_selected_index` started at 0, the original's
is -1, obj3_owned.c:488-489). Mutation checks: changing op 0x19's `operand - 1` or dropping
one autoselect store in `FindOrAutoselect` shows up as 26 and 732 mismatching tests.
