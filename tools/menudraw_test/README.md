# tools/menudraw_test -- the message boxes and the dialogue text against the original, pixel for pixel

`src/roth/roth_menudraw.{h,cpp}` draws the original's message boxes (main menu, options, the save
and load lists, the confirmations, settings, movie clips, the overwrite prompt) by
show_message_box's own compositor; `src/roth/roth_dlgdraw.{h,cpp}` draws the dialogue's choice
panel, subtitle line and timed message by render_dialogue_text_panel / render_text_ui /
render_active_timed_message. Both use `src/roth/roth_uidraw` (the 2D primitives shared with the
inventory panel). The checklists are at the top of each .cpp. This test proves them.

    tools/menudraw_test/run.sh                   # every script; exit 0 = every picture identical, every hit the same
    tools/menudraw_test/run.sh scripts/main.txt

| file | what |
|---|---|
| `menuprobe.c` | A mod for the headless ROTH.C build (`/home/claude/oracle_build`). At gameplay frame 200 of STUDY1 it runs a script on the ORIGINAL's own routines: `run_options_menu`, run_main_menu's body (the headless host answers run_main_menu itself), `show_message_box` on any descriptor, and the dialogue verbs of `tools/dialogue_test` (`eval_dialogue_record_by_id`, `voice_stream_pump`, `render_text_ui`, `choice_select_*`, `update_dialogue_choice_highlight`, `queue_timed_message_color`, ...). The boxes' keys come from a queue handed out through `input_ring_dequeue`; `update_frame_time_scale` (which spins on the timer interrupt's counter) advances one tick per call, so every loop pass of the original is exactly one tick. On `dump` / `qd` / `dtext` it writes the whole indexed framebuffer to `<name>.scr`; it logs every box opened and closed (`O` / `C`), the state each loop pass leaves (`L`: the row carrying the marker colour, the scroll word), the dialogue records the renderers read (`T` / `TS` / `TT`), and the original's hit tests over a grid (`HG` / `H`: hit_test_ui_element with the box's own row table; `CHG` / `CH`: update_dialogue_choice_highlight, its state put back after). Log writes hold SIGALRM blocked. |
| `oracle.sh` | Builds the probe and runs one script through the original (save files go to the game copy's `savegame/`). |
| `menudraw_diff.cpp` | Reads the original's log in order and lets the port do what the original did: opens a `MessageBoxPainter` at each `O` (the descriptor from the port's transcribed table, the flags and words from the log), runs one `Pass` per `L`, drops it at `C`, draws the dialogue text with `DialogueTextPainter` from the logged records at each `dtext`; composites the open pictures over the fill and compares the whole screen with the `.scr`, byte for byte. Pixels of a box's 4-pixel drop shadow (a blend of whatever lies around it, not drawn by the port) are left out and counted. It prints the port's own hit tests over the same grids, and checks the transcribed descriptors (every field), the colours and "%D: %s" against the original's bytes, the font hash and the pack tables. |
| `run.sh` | Both sides, then the counts; diffs the `O`/`C`/`HG`/`H`/`CHG`/`CH` lines. |
| `scripts/` | `options` (the in-game options box and everything it leads to, saves present), `main` (the front end's main menu with its zoom fade frame by frame, movie clips, settings, load, quit), `boxes` (the overwrite prompt with its fade, the volume box's sliders, the subtitle box, the two resource-error boxes), `dialogue` (spoken lines, choice menus of 2, 4 and 5 rows with the highlight by key and by pointer, a two-page line, timed messages of one and three lines, the inventory-open placements). |

Outputs go to `$MENUDRAW_WORK` (default /tmp/menudraw_test), never into the repo: the dumps and the
PPMs (`<script>.ppm/<dump>.{orig,port,diff}.ppm`) are retail art.

What it does NOT cover: the hi-res (line-doubled) drawing (ported, not compared); the text-input
row of the save list (render_text_input_field: not ported); a slider's value change and the
settings toggles (roth_menus does not port those rows); the gallery row type; the topic menu
(render_choice_text_panel) and the examine popup; what the original draws behind the text
(apply_ui_palette_rect, the world darkened) and around the boxes (the shadow).
The records the dialogue renderers read come from the original's memory here; that
roth_dialogue's records are the same bytes is tools/dialogue_test's proof.
