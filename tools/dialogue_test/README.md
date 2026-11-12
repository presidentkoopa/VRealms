# tools/dialogue_test -- dialogue, choice menus and the inventory screen against the original

`dlgprobe.c` is a mod for the headless ROTH.C build (`/home/claude/oracle_build`). It runs a
script of commands on the ORIGINAL's own routines (eval_dialogue_record_by_id, voice_stream_pump,
render_text_ui, choice_select_*, use_enter_key_handler, dialogue_voice_force_end,
try_interrupt_dialogue_voice, give_item, query/find/remove, dispatch_dialogue_ui_action,
build_available_choice_menu, layout_timed_message_text, blit_das_image_to_buffer, ...) and prints
the state after each. `dialogue_diff.cpp` runs the same script on the port (roth_dialogue,
roth_invscreen, roth_inventory, roth_icons, GameCore) and prints the same lines.

    tools/dialogue_test/run.sh                 # every script; exit 0 = all identical
    tools/dialogue_test/run.sh scripts/topics.txt

`dialogue_diff <DATA> - <script>` seeds itself from a new game instead of from an oracle log.
Outputs go to `$DLG_WORK` (default /tmp/dialogue_test); they contain retail text: never commit them.

Scripts: `dlg_untimed` (no speech: lines wait for a click; menus 343, 344, 223, 598, 173, 602, 610,
the 8-entry queue, stop-all, timed messages), `dlg_voiced` (clip-timed lines and pages from the real
DBASE500 headers, re-queueing, skip, Enter), `layout_all` (every op 0x05/0x08 text of all 694
actions laid out at both widths and both budgets; multi-page lines), `inventory` (give with and
without context, pickups by render id, the pickup lock, query/autoselect, list mode, remove,
filter, examine, labels, the screen's click verbs, use and combine with their messages, commit),
`topics` (an item's "i" topic menu and what picking a topic says), `icons` (all 280 DBASE200 icons).
