# tools/invdraw_test -- the inventory panel's PICTURE against the original, pixel for pixel

`src/roth/roth_invdraw.{h,cpp}` draws the inventory panel as the original draws it: the
0x120 x 0x50 rectangle, from the install's own pictures (DATA\ICONS.ALL, DBASE200.DAT, the map
pack's shade and blend tables) and the original's font (ROTH.EXE's), placed by ports of
ROTH.C's routines (the checklist is at the top of roth_invdraw.cpp). This test proves it.

    tools/invdraw_test/run.sh                    # every script; exit 0 = every picture identical
    tools/invdraw_test/run.sh scripts/hands.txt

| file | what |
|---|---|
| `invprobe.c` | A mod for the headless ROTH.C build (`/home/claude/oracle_build`). At gameplay frame 200 of STUDY1 it runs a script on the ORIGINAL's own routines -- `give_item`, `render_inventory_panel`, `dispatch_dialogue_ui_action` (tab, cell, hand box and arrow clicks), `update_inventory_screen` with a key fed through `input_ring_dequeue`, `redraw_inventory_cursor_cell` at a chosen frame tick, `close_inventory_panel`, `hit_test_dialogue_ui_action` -- and on `dump` writes the panel rectangle of its framebuffer (the indexed bytes) to `<name>.pan`, with the drawing state, the label, both hotspot lists. Every log write holds SIGALRM blocked (the weapons lane's re-entrancy finding, tools/actors_test/oracle/README.txt). |
| `oracle.sh` | Builds the probe and runs one script through the original. |
| `invdraw_diff.cpp` | The same script through the PORT (roth_invscreen's rules + roth_invdraw's painter), seeded from the original's first state block. Compares each picture byte for byte, prints the state lines in the probe's format, checks the transcribed executable tables against the original's bytes ("X" lines), the pack tables against the ones the original loaded, and the font hash. |
| `run.sh` | Both sides, then the counts; diffs the state, label, hotspot and hit-test lines. |
| `scripts/` | `basic` (the open), `tabs` (all five tabs by click and by key; the characters list with an entry held), `scroll` (24 entries: keys across rows and pages, both arrows lit and put out, the colour cycle), `hands` (left/right hand tints and boxes, the held tint, items into and out of hands, a box with nothing held, a box's label), `labels` (counts, an item with no icon, empty lists, a re-open), `hires` (the line-doubled drawing), `pointer` (THE MOUSE, as the VR hands make it: below). |

Outputs go to `$INVDRAW_WORK` (default /tmp/invdraw_test), never into the repo: the dumps and
the PPMs (`<script>.ppm/<dump>.{orig,port,diff}.ppm`) are retail art.

What it does NOT cover: the 4-pixel shadow and the grey 3D view the original draws OUTSIDE the
rectangle (not drawn by roth_invdraw), the examine popup, the picture after the panel is closed
(the original then restores the world under it).

## The mouse and the VR hands (`scripts/pointer.txt`)

`ptr x y code via` moves the ORIGINAL's mouse to panel pixel (x, y) and runs its own
`handle_cursor_click` (input.c:1260-1358) there: the hover (code 0), then a primary (1) or a
secondary (2) click; `set_cursor_shape` is overridden to record the shape and draw no cursor. The
PORT stands a VR hand in front of the panel in the world (the panel as `roth_interface.cpp` places
it), aims it at that pixel and makes the click with a gesture -- `via` 0 the laser with an index tap
or an index held 18 tics, 1 the fingertip touching or held on the panel, 2 the hand pointing away
from the panel -- through `src/roth/roth_panelmap.h` (panel point -> pixel), `roth_invhand.h` (the
gestures) and `roth_invmouse.h` (the mouse on the panel, which every pointer in the engine uses).
Both print a `P` line (the cursor shape, the hover latch 0x81300, list, cursor, scroll, held, hands,
the examine request, the close and options requests); `run.sh` diffs them. `ptrsweep n` hovers
over the panel and a margin every n pixels; `noinspect` drops an examine request unopened. A
`MAP` line on the port side means a gesture's click landed off the pixel it was aimed at.
Result (2026-11-13): 1422 mouse points identical, 16 pictures identical, 28 gesture clicks, 0 off
their point. Mutations: a held index made primary instead of secondary -> 12 pictures and the state
lines differ; a hand box hovered with shape 0x248 instead of 0x250 -> 90 lines differ.
