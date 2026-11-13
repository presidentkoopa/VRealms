# tools/popdraw_test -- the examine popup against the original, pixel for pixel

`src/roth/roth_popupdraw.{h,cpp}` draws and runs the inventory's examine popup as the original
does: the window (render_inspect_popup_window: the ICONS.ALL frame, the item's icon, its label, the
magnifier and "i" buttons), the close-up film (a DBASE300 GDV on the original's 60 Hz GDV timer,
looped or held, frame by frame through gdv_inspect_frame_callback / blit_saved_ui_block), the
document pages (load_inspect_document_page, draw_das_panel_slide_reveal: the reveal, the arrows,
the scroll keys, the drag), the topic menu (render_choice_text_panel over the window, with
apply_ui_palette_rect), and the original's click rectangles (hit_test_dialogue_ui_action's popup
half). The checklist is at the top of roth_popupdraw.cpp. This test proves it.

    tools/popdraw_test/run.sh                    # every script; exit 0 = every picture identical
    tools/popdraw_test/run.sh scripts/film.txt

| file | what |
|---|---|
| `popprobe.c` | A mod for the headless ROTH.C build (`/home/claude/oracle_build`). At gameplay frame 200 of STUDY1 it gives items, opens the inventory (`render_inventory_panel`), clicks a cell (`dispatch_dialogue_ui_action`) and runs the next `update_inventory_screen`, which runs the ORIGINAL's popup (`load_dbase300_resource_at_offset`) to its end. Every key the popup reads comes from a queue handed out through `input_ring_dequeue`; entries also dump the framebuffer (`qd`: the back buffer as the popup leaves it; `qp`: the next presented frame, taken in `mark_overlay_dirty_rects` before the topic menu's block is put back), move the mouse, press or hold a button, and run the hit test over a grid (`qh`). The clock is deterministic: one GDV timer tick (`gdv_decode_timer_isr_noaudio`) and one frame tick per film boundary, one frame tick per modal pass (see the file's top). It logs the popup's state at every dump (`P`: the geometry words, marks, mode, selection, origin/box/reveal of the topics, page, pending turn, drag, exit; the six cells; the film's boundary and decode counts; the topic records `TL`; the label; the right hand), each key read (`K`), the page descriptor at every redraw (`SLIDE`). Log writes hold SIGALRM blocked. |
| `oracle.sh` | Builds the probe and runs one script through the original. |
| `popdraw_diff.cpp` | The same script through the PORT: roth_invscreen's rules, roth_invdraw's panel, roth_popupdraw's `InspectPopupView`, seeded from the original's first state block. It hands the queue out in the same order as the original's loops read it (one entry per film boundary, the modal loop's drain), compares the popup's rectangle with the original's screen at each dump byte for byte, and prints the state in the probe's format. With no popup up it checks the original's screen instead: the panel's rectangle against the port's panel picture, everything else against the screen before the popup (the original put it back). |
| `run.sh` | Both sides, then the counts; diffs the state, cell, film, topic, label, key and hit-test lines (the info latch is not compared: the original keeps a count, the port a flag). |
| `scripts/` | `film` (a looping film across its loop point, a held film through its end into the modal loop, a first and a second view), `document` (item 84: Space past the film, the page's reveal frame by frame, a spoken line dismissed by a click, page turns by the arrows, scroll keys, a drag, the magnifier, the close box, a second view on the page it was left at), `topics` (the "i" during the film and after, the reveal pass by pass, the highlight by key and by pointer, the menu hidden and shown, a topic picked). |

Outputs go to `$POPDRAW_WORK` (default /tmp/popdraw_test), never into the repo: the dumps and the PPMs
(`<script>.ppm/<dump>.{orig,port,diff}.ppm`) are retail art.

What it does NOT cover: the hi-res (line-doubled) drawing (ported, not compared); the 4-pixel shadow
the original blends around the window (the world's pixels, not drawn); the save-under and the CD-swap
retry box; the timing of the original's real interrupts (the probe's clock is one tick per boundary or
pass; the engine runs the GDV timer at 60 Hz and one pass a tic -- reasoning only).
