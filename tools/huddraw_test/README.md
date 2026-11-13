# tools/huddraw_test -- the HUD against the original, pixel for pixel

`src/roth/roth_huddraw.{h,cpp}` draws the HUD as the original draws it: the health panel, the
weapon panel (ammunition cells, charge bars, the clips and their count), the corner peek of Adam's
item and the selected item's translucent icon, from the install's own pictures (DATA\ICONS.ALL,
DBASE200.DAT, the map pack's shade and blend tables, ROTH.EXE's font), placed by ports of ROTH.C's
routines (the checklist is at the top of roth_huddraw.cpp). This test proves it, and proves that
our own ported logic (GameCore's inventory, ActorWorld's weapon and damage) reaches the HUD's
routines at the same points with the same state as the original's.

    tools/huddraw_test/run.sh                    # every script; exit 0 = every screen identical
    tools/huddraw_test/run.sh scripts/held.txt

| file | what |
|---|---|
| `hudprobe.c` | A mod for the headless ROTH.C build (`/home/claude/oracle_build`). At gameplay frame 200 of STUDY1 it runs a script on the ORIGINAL's own routines -- `give_item`, `remove_item`, `activate_weapon_item`, `equip_first_usable_weapon`, `trigger_weapon_fire`, `tick_weapon_hud_ammo_anim`, `apply_direct_damage_to_player`, `update_selected_item_icon`, `refresh_hud_layout`, `render_weapon_hud`, `draw_held_item_icon`, `draw_active_ui_panels`, `draw_character_portrait_corner`, the corner timer of `run_gameplay_frame` -- over a known "world" (xorshift indices over the whole screen, where the original renders its 3D view), and on `dump` writes the whole framebuffer to `<name>.scr`. It logs every HUD routine the original's own logic reaches (`HC` lines: render_weapon_view, redraw_weapon_hud_panel, render_weapon_hud, update_selected_item_icon, render_player_health_bar) with the state it reads, and the no-ammo messages. Every log write holds SIGALRM blocked. |
| `oracle.sh` | Builds the probe and runs one script through the original. |
| `huddraw_diff.cpp` | The same script through the PORT: our inventory (GameCore), our weapon and damage (ActorWorld, with the HUD hooks of ActorHost) and the painter, seeded from the original's first INV and START lines. Prints the same HC / INV / NOAMMO / state lines, composes the HUD over the same world (`HudPainter::ComposeOnto`) and compares each screen byte for byte; checks the transcribed tables against the original's bytes and the font hash. Any painter Problem fails the run. |
| `run.sh` | Both sides, then the counts; diffs the trace lines. |
| `scripts/` | `health` (full, hits, below half, one pixel, empty, a change with no flag, the layout's own drawing, another maximum), `w_<item>` (each of the 15 weapon items of DBASE100: equip, pulls, reloads, recharge ticks, its ammunition taken and given), `held` (the selected item's icon: slide in, slide down, changes, empty hand, nine items), `corner` (the corner peek: growing box, fade-in, settled, the timer running out, over a hit and a weapon). |

Outputs go to `$HUDDRAW_WORK` (default /tmp/huddraw_test), never into the repo: the dumps and the
PPMs (`<script>.ppm/<dump>.{orig,port,diff}.ppm`) are retail art.

What it does NOT cover: the windowed (smaller view) layout, which is not ported (the engine's view
is always whole); the hi-res line-doubled modes (VideoMode 8 does not double lines);
`restore_corner_peek_icon` after the flip (no shown frame has it).
