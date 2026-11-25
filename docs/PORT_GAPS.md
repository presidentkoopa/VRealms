# What of ROTH.C's game logic is not ported (2026-11-24)

## Done since this list was written (2026-11-24, evidence: flat bridge runs on the owner's PC)

- 1, 2: 0x03's load-time snapshot and move and its ascending sweep; a resized mid band redrawn (3350933f). Seen: the
  lounge typewriter's page rises after the walk-in (typewriter_flat.cfg), beside the original's frames.
- 3, 4, 5, 6: walk-in triggers ported from twe_link_state / fire_sector_trigger (f9db8a30, 838135e3), searching the
  original's own list (the command section's category slot 3). Seen: #112 fires once on entering s54, not again on
  re-entry (walkin_flat.cfg). Not yet seen: the leave edge, the height links, the "moving" gate, on a map that uses them.

- The load-time object table pass, completed (778ab873): the armed rerun of 0x0a-0x0f 0x11 0x23, 0x0c's load-time
  swap, 0x15's base, 0x24's continuous rotation (tick_rotate_object), the 0x3d timers (tick_command_timer_queue).
  Seen: ANUBIS now runs 4 armed records at load (0 before); STUDY1 queues its 2 timers. Not yet seen: a timer firing,
  a 0x24 rotation (no retail map arms one at load; chains start them).
- Animated pictures (walls, objects) step at their own rate, (rate & 0x1f) + 1 ticks of 70 Hz, as
  advance_das_sprite_animation_frame does; was one 229 ms guess. Still 229 ms: the RLE variant (its caller chooses
  the frame; which caller does so for a wall is not read) and the candles' composed pictures.
- Tools: tools/port_audit/audit.py writes docs/PORT_AUDIT.md, every ROTH.C function against the port.

- Contact triggers (cd05bec8): fire_trigger_on_contact's latches and one-shot; walking into an object; the landing
  thud plays. Movement differential against the original: 0 differ.

## Still open, beyond the logic (from docs/PORT_AUDIT.md; to be read function by function)

- automap.c: 19 functions, none named in the port. The original's map screen.
- savegame.c: 25 of 33 not named. The original's save files (SAVE<n>.SAV) are read (roth_gameflow); writing them is
  this port's own format.
- gdv_cutscene.c 46, audio.c 52, input.c 42, menu_hud_ui.c 17, doors.c 13, map_load.c 19 not named: each to be read.
- 0x25 (a texture animation's frame fires a chain; 11 records, 5 maps): needs whether the picture is drawn this frame,
  which the engine does not report per texture.
- Sliding sectors (0x09, 32 records, 5 maps) are behind roth_platforms, off by default.

## The whole game (all 44 maps, read from the retail .RAW files with roth_raw; 2026-11-24)

Every INSTRUCTION opcode the 44 maps use is dispatched by the port (roth_runtime RunCommand / RunCommandWithChain, the
command core). Whether each does what the original does is a separate question, answered only where a suite covers
it. What is NOT ported game-wide:

| Trigger channel | Records | Maps | What it is |
|---|---|---|---|
| 0x1b | 95 | 27 | a shot hitting an OBJECT: was ported (creature death, a creature's shot); cd05bec8 adds the original's latches and one-shot, and the +0x24 list. Not checked: that every player weapon of VRealms reaches it |
| 0x25 | 11 | 5 | a texture animation's frame (DOMINION MAS3 RAQUIA5 STUDY1 STUDY3) |
| ~~0x3d~~ | 15 | 11 | a timer: PORTED 778ab873 |
| ~~0x39~~ | 1 | 1 | walking into an object (RAQUIA5): PORTED cd05bec8 (fire_tracked_object_trigger, from the movement's bump) |
| 0x09 instructions | 32 | 5 | sideways-sliding sectors, only with roth_platforms 1 (OPTEMP1 RAQUIA2 RAQUIA3 RAQUIA5 SALVAT) |
| 0x16 / 0x3a / 0x3c | 481 | 28 | creature spawn / id / spawn-adv: dispatched only while the actors system is active |

This census covers the level logic only. The creatures' behaviour, combat, the weapons, the cutscenes and the
interface have their own ports and their own suites (CLAUDE.md); they are not audited here.


Gathered from the port's own code: the gaps it records in its comments, and every place that counts a record into
`g.unhandledOps` (src/roth/roth_runtime.cpp). The load report's "instruction records by opcode" table in
`roth_<MAP>.log` is STALE: it lists 0x15 0x16 0x1e 0x1f 0x20 0x27 0x29 0x2a 0x2b 0x2d 0x2e 0x33 0x35 0x3a 0x3c 0x3f
as "NOT IMPLEMENTED", but RunCommandWithChain dispatches every one of them now (the item opcodes, dialogue, the
command core, the actor commands). Its `IsImplemented` flags were never updated. Fixing the report is item 0.

None of the "dispatched" opcodes has been differential-tested against the original as a whole; the suites in
CLAUDE.md cover parts of them.

| # | What | Where it shows in the game | Source (ROTH.C) | Port (file:line) |
|---|------|----------------------------|-----------------|------------------|
| 0 | The load report's opcode table says 213 records are not implemented; it is stale | the log only | -- | roth_runtime.cpp IsImplemented |
| 1 | 0x03 modify-sector, the ASCENDING sweep: its targets are a snapshot read through record +0x10 that the port never located | the lounge typewriter's page does not rise (#112); any floor that rises back | raw_commands.c:2590-2760 (tick_modify_sector, asc. branches) | roth_runtime.cpp TickModifySector |
| 2 | A sector's TEXTURE_MAP_OVERRIDE (+0x0c) changed at run time is never redrawn: the loader bands the mid piece once | the typewriter page, any transparent mid an effect resizes | renderer.c:8201-8212 (via roth_surface.cpp:277) | rothmap.cpp makeSide; roth_runtime.cpp ReapplyWallFace does not read it |
| 3 | Walk-in triggers: the LEAVE edge (re-fire of the latched record, bit 0x40 of +0x06) | chains meant to run on leaving a room | raw_commands.c:3204-3213 | roth_runtime.cpp NotifyPlayerSector |
| 4 | Walk-in triggers: the "only while moving" gate (+0x07 & 4) and the +0x02 0x29 gate | a trigger firing while standing still / when armed-gated | fire_sector_trigger :2869-2871 | NotifyPlayerSector |
| 5 | Walk-in triggers: the original fires ONE record per entry (the first for the sector's id); the port fires every 0x13 on the sector | STUDY1 s430 runs two | :3262-3266 | FireSectorTriggers |
| 6 | Walk-in triggers: nothing fires on the first sighting (the spawn sector); the original's buffer starts empty, so it does | a chain on the starting sector | :3240-3266 | NotifyPlayerSector |
| 7 | ~~Wall-face click triggers (0x18/0x32): the direction mask dir_mask1~~ WAS ALREADY PORTED (roth_surfbox.h SurfFlagsAdmit); the note in the code was stale | a switch usable from a side the original refuses | raw_commands.c:3012, :3029 | roth_runtime.cpp:486-492 |
| 8 | ~~Floor click category-B twin: dir_mask2~~ WAS ALREADY PORTED (SurfFlagsAdmit) | -- | :3100 | roth_runtime.cpp:499 |
| 9 | ~~The authored bounding box on click triggers~~ WAS ALREADY PORTED (roth_surfbox.h SurfFirstMatch) | a use from outside its authored area | :3034-3038, :3075-3079 | roth_runtime.cpp:501 |
| 10 | The transparency flip of a texture swap (face +0x0a bit 0): PORTED into the live map and the collision's image (0x34, and 0x0c's swap); what the RENDERER does with that bit is not read yet | a glass / grille that should turn solid or see-through | raw_commands.c (0x34) | roth_runtime.cpp:1777 |
| 11 | The 0x40 / 0x20 state-link latches of the click channels | -- | RC:1635-1644 | roth_runtime.cpp:7630 |
| 12 | 0x09 moving sector, only with `roth_platforms 1` (sliding sectors need moving vertices) | sliding floors / lifts sideways, 32 records game-wide | the 0x09 tick | roth_runtime.cpp:4786 |
| 13 | Runtime refusals counted, each a case the port cannot draw: 0x34 / 0x0c on an index the loader never registered; 0x0a/0x0b with no flat registered; 0x0d / 0x02 with no sprite built for the word; 0x24 with no active-object latch; 0x23 deeper than 16 host chains | a texture or sprite change that does not show | -- | roth_runtime.cpp:1903, 3273, 3286, 2907, 7369, 2292, 4997 |
| 14 | A baked wall piece keeps its load-time shift when the runtime scrolls or swaps it (no re-bake at run time) | a scrolled or swapped wall that does not move | -- | roth_runtime.cpp ReapplyWallFace (wallBakedKept) |
