# REMAROTH game layer — Dialogue, FMV, Text, Speech (reader 4 of 6)

Path keys: **E** = `VRealms/tools/ROTH.C/roth_c/src/engine/`, **P** = `VRealms/tools/roth-editor/src/resources/parsers/`, **U** = `roth-editor/src/utils/utility.gd`, **M** = `REMAROTH/src/common/cutscenes/`. All under `/mnt/user-data/uploads/`. Tags: [inferred] means reasoned from code but not stated there; [open] means not settled by the staged sources.

No retail data files (DBASE*.DAT, *.GDV) are staged, so the acceptance tests in §8 are procedures with pass criteria, not precomputed numbers.

---

## 1. What the player experiences

Walking into a trigger or using or examining an item runs a DBASE100 script. That script can:
- have Adam speak a line (DPCM speech plus a subtitle panel under the 3D view);
- show a menu of reply choices (Up/Down plus Enter, or mouse hover plus click);
- play a full-screen GDV movie with timed subtitles, which only Esc skips.

Lines play one after another. Each waits for its speech to end, or for a click when a line has no speech. Picking a choice runs that choice's branch, which can:
- set or clear story flags;
- give or remove items;
- play more lines or movies;
- jump to another script.

Every movie played is added to a replay gallery in the options menu. There are no NPC portrait panels. "Dialogue" means monologue lines, choice menus and movie subtitles (E/dialogue_ui.c:5-6).

---

## 2. Dialogue

### 2.1 Data

**DBASE100 header** (as used by the code):

| Field | Meaning | Where used |
|---|---|---|
| `+0x10` | inventory count | E/dbase100.c:193 |
| `+0x18` | dialogue (action) count | E/dbase100.c:549 |
| `+0x20` | cutscene count | E/dbase100.c:935 |
| `+0x24` | cutscene table offset, stride 0x14 | E/dbase100.c:937 |
| `+0x28` / `+0x2c` | interface-text count / table | E/dbase100.c:412-414 |
| `+0x30` | must equal 2, or op 0x07 is ignored | E/dbase100.c:936 |

**Dialogue table.** An entry is an offset from the file base; 0 means none (E/dbase100.c:550-553). The first dword of a record holds the byte length in its low 16 bits. Command count = `len/4 - 1`, and commands start at record+4 (E/dbase100.c:433-436).

**Command word** (E/dbase100.c:6-7, 630-632):
- opcode = `(w>>24)&0x7f`;
- bit 31 = If-NOT;
- operand = `w & 0xffffff`.

A record with **operand 0 is skipped** (E/dbase100.c:633). That is why marker opcodes carry 0xFFFFFF (beyond-the-ire DBASE100_commands.md).

**Cutscene entry, 0x14 bytes** (DBASE100.md "Cutscene Entry"; P/dbase_100.gd:19-25; confirmed by use in E/gdv_cutscene.c):

| Offset | Field | Where used |
|---|---|---|
| `+0` | 8-byte name | E/gdv_cutscene.c:2763, 2827 |
| `+0xa` | s16 subtitle-stream byte size | E/gdv_cutscene.c:2777-2778 |
| `+0xc` | DBASE400 offset of the gallery title | E/gdv_cutscene.c:3105-3107 |
| `+0x10` | 24-bit DBASE400 offset of the subtitle stream | E/gdv_cutscene.c:2782 |
| `+0x13` | gallery playback index, written at runtime | E/gdv_cutscene.c:2772-2775 |

**DBASE400 text entry.** The field at `+0` is a u32 DBASE500 clip index, which is seeked as `clip<<3` (E/audio.c:1245). `+4` is a u16 length, `+6` a colour byte, then the text (E/dbase100.c:383-392). Colour 0 becomes 0x20 (E/dbase100.c:388). A missing interface text yields "Missing Text" (E/dbase100.c:419-421).

**Choice strings** are op 0x08 records whose operand is a DBASE400 offset. Their text is read without speech (flag 0) (E/dbase100.c:698).

### 2.2 Who starts it

- **RAW map op 0x2b** `run_command_dbase100_record` (E/raw_commands.c:4700-4709): calls `eval_dialogue_record_by_id(word[rec+8])`.
- **RAW op 0x36** latches the record as a pending "if next fails" branch (E/raw_commands.c:512-517). It fires through `flush_pending_command_record` (E/raw_commands.c:178-188).
- **Engine events** in `game_play_loop`:
  - boot runs record 1 with `0x83b20=0x4d2` (E/game_core.c:756-757);
  - quit runs record 2 (E/game_core.c:802);
  - new game runs record 3 (E/game_core.c:824-825);
  - death runs record 4 (E/game_core.c:970-971).
- **Inventory triggers** (inspect/use/information) feed the same interpreter through `eval_dialogue_record_condition_with_cleanup` (E/dbase100.c:569-576; E/dialogue_ui.c:74-98, 191-203).

### 2.3 Interpreter and queue: control flow

Entry: `eval_dialogue_record_by_id(id)` → `eval_or_queue_dialogue_record_commands(rec, 0)` (E/dbase100.c:545-555).
- If the queue is empty, the chain runs now through `execute_dbase100_chain(chain, count, flags)` (E/dbase100.c:437-438).
- Otherwise the chain is appended to the 12-byte `{chain, count, flags}` queue. The queue holds at most 8 entries and **overflow is silently dropped** (E/dbase100.c:439-446).

`execute_dbase100_chain` (E/dbase100.c:607-953) dispatches opcodes 1..0x37 only (E/dbase100.c:641-652). Flag bit 1 is "scan mode": an effectful opcode returns 1 immediately, which is how "is this topic available" is tested (E/dbase100.c:666, 762, 769, 854, 911, 923).

**Opcodes involved:**

| Op | Semantics | file:line |
|---|---|---|
| 0x01 | If flag (IfNOT: flag clear) holds, continue; else **terminate chain** (ret 0) | E/dbase100.c:803-808 |
| 0x02 | inventory count(item=lo16) vs threshold=operand>>16: If `cnt>thr` continue, else terminate (IfNOT inverts to `<=`) | E/dbase100.c:811-818 |
| 0x03 | equality: If equal → terminate with 0; If-NOT equal → terminate | E/dbase100.c:821-832 |
| 0x04 | set flag (IfNOT: clear) in the DBASE100 bitmap | E/dbase100.c:761-765 |
| 0x05 | speak a line: read DBASE400 with voice (flag 1), open the timed window, set busy=1; if already busy → **REQUEUE** | E/dbase100.c:853-864 |
| 0x07 | play cutscene `operand-1` (see §3.7) | E/dbase100.c:922-942 |
| 0x08 | build the choice menu (see §2.4) | E/dbase100.c:665-733 |
| 0x09 | branch marker; not dispatched, only counted by `execute_dialogue_branch` | E/dbase100.c:515-524 |
| 0x0a | terminate success (ret 1); ends a branch | E/dbase100.c:740-742 |
| 0x0b | random: segment up to op 0x0c into KEEP-able sub-chains, pick with `rng_next_index_for_count`, recurse | E/dbase100.c:745-758, 41-100 |
| 0x0d | If flag holds run next record, else skip it; **if the next record is op 8, fall into the choice builder instead** | E/dbase100.c:655-662 |
| 0x0e | fullscreen image (The End) | E/dbase100.c:910-919 |
| 0x11 | give item (IfNOT: remove), operand as s16 | E/dbase100.c:768-772 |
| 0x19 | play sound effect `operand-1` | E/dbase100.c:847-850 |
| 0x1a / 0x1b | change music (DBASE300 chunk) / stage pending topic | E/dbase100.c:775-800 |
| 0x1c | flags-only gate on caller flag bit0 | E/dbase100.c:867-871 |
| 0x1d | jump: replace cursor with dialogue record `operand` and keep interpreting | E/dbase100.c:874-885 |
| 0x23 / 0x36 | run a RAW map command by index / id | E/dbase100.c:835-844 |
| 0x26 | health adjust; `<=0` kills | E/dbase100.c:901-907 |
| 0x2d / 0x37 | UI latch / value adjust | E/dbase100.c:888-898, 782-784 |

**REQUEUE_A** (E/dbase100.c:946-952) handles a line or movie that arrives while a line is on screen (`g_dialogue_busy_flag` 0x83aea set):
- it overwrites **queue slot 0** with `{current record, remaining, flags}`;
- it sets count=1 if the count was 0;
- it returns -1. `advance_dialogue_action_queue` does not pop on -1 (E/dbase100.c:493), so the rest of the chain resumes later.

**Sequencing driver.** `voice_stream_pump` runs every frame (E/audio.c:1300-1327). While busy **and** (speech playing or text context non-zero) it does nothing. Otherwise it clears busy and calls `advance_dialogue_action_queue` (E/dbase100.c:484-497), which runs the queue head. When the queue empties, `finish_dialogue_record_eval` runs (E/dbase100.c:291-296). If a movie overlay is up, that also tears it down (`exit_cutscene_overlay_mode`, E/gdv_cutscene.c:2434-2488).

### 2.4 Choice menus: the state machine

**Build** (op 0x08, E/dbase100.c:665-727):
1. Back up to the 0x08 record. Loop over sub-records.
2. `0x0d` evaluates a gate for the *next* choice (E/dbase100.c:684-690).
3. `0x08` appends its DBASE400 text to a 0x800-byte buffer (E/dbase100.c:670, 698-708). It replaces the text's last byte with `'\n'` and records its ordinal `sub_idx` in `g_dbase100_choice_record_indices`.
4. A gated-off choice is skipped **but still increments `sub_idx`** (E/dbase100.c:696, 710-712).
5. Any other opcode stops building. That record, the first 0x09, becomes the choice cursor (E/dbase100.c:692, 720-723).
6. With 0 visible choices the chain just continues (E/dbase100.c:719).
7. With **exactly 1 visible choice, branch 0 runs immediately and no menu is shown** (E/dbase100.c:724, 728-733).
8. Otherwise `dbase100_open_dialogue_window` opens the menu. It lays out the text (up to 10 wrapped lines per choice), sets `g_active_dialogue_context` = the line count and `g_move_freeze_gate=0x6ffff` (menu mode) (E/dialogue_ui.c:1178-1218). Busy=1 and the chain returns 1 (E/dbase100.c:725-727).

**Present.** `render_text_ui` draws the panel while the gate is 0x6ffff (E/dialogue_ui.c:989-992).

**Choose:**
- Keys: Up/Down call `choice_select_prev/next`, which wrap and only work while the gate is 0x6ffff (E/dialogue_ui.c:138-186; bound in E/game_core.c:913-920 and E/input.c:165-171). Enter calls `use_enter_key_handler` → `choice_accept_selected` (E/input.c:718-727; E/dialogue_ui.c:215-228).
- Mouse: `update_dialogue_choice_highlight` hit-tests the hovered line (E/dialogue_ui.c:231-271). A click goes through `update_dialogue_cursor_and_click` → `dialogue_voice_force_end` → `close_dialogue_and_run_branch` (E/dialogue_ui.c:1146-1171; E/audio.c:1343-1347; E/dbase100.c:532-540).

**Branch.** `execute_dialogue_branch(visible_idx)` (E/dbase100.c:502-528):
1. Map the visible index to `sub_idx`.
2. Walk forward from the choice cursor. Each op 0x08 adds 1 to the target (this skips the branches of nested menus). Each op 0x09 subtracts 1.
3. At the target 0x09, run the chain from the next record to the **end of the record**, then `finish_dialogue_record_eval`.

A branch therefore must end with 0x0a (terminate) or 0x1d (jump). Otherwise it falls through into the next branch [inferred from the default-skip of op 9 at E/dbase100.c:651].

**What changes.** Only what the branch's opcodes do: flags (0x04), items (0x11), lines (0x05), movies (0x07), map commands (0x23/0x36).

**End.** The branch terminates (0x0a) → the queue drains → busy clears through the pump → the overlay tears down.

### 2.5 Monologue lines (op 0x05)

`dbase100_open_dialogue_window_alt` (E/dialogue_ui.c:1224-1247):
- lays out at most 3 wrapped lines per segment;
- if speech exists, dwell = `samples*0x46/rate`, i.e. 70 Hz ticks (E/dialogue_ui.c:1235) [70 Hz: E/audio.c:1835];
- segment time is proportional to its byte length (E/dialogue_ui.c:1236-1240);
- with no speech, the gate is 0x7ffff and the line waits for a click or key (`try_interrupt_dialogue_voice`, E/audio.c:1371-1378).

The per-segment sequencer is in E/dialogue_ui.c:996-1014. The last segment is held while speech still plays (E/dialogue_ui.c:1000-1001).

There is an early exit that shows no text when speech exists and flag `0x83e90` is set (E/dialogue_ui.c:1226-1227). That is probably the subtitles-off option [inferred].

---

## 3. Cutscenes (GDV)

### 3.1 File header (0x18 bytes; E/gdv_cutscene.c:1237-1333; P/gdv.gd:14-27)

| Offset | Field | Where used |
|---|---|---|
| +0 | magic `0x29111994` | :1267 |
| +4 | size code; looks up `{code,w,h}` in the dims table when height is 0 | :1281-1291 |
| +6 | frame count | :928 |
| +8 | fps | :1297, 1312, 2307 |
| +0xa | audio flags: 1 present, 2 stereo, 4 16-bit, 8 DPCM | :1303-1310; P/gdv.gd:9-12 |
| +0xc | sample rate | :1296 |
| +0xe | pixel format: 1 = 8bpp, 2 = hi-colour | :1319-1322 |
| +0x10 | max chunk size | :1372 |
| +0x14 / +0x16 | width / height | |

With version 1, a 0x300-byte **6-bit** palette follows and is duplicated at +0x300 (E/gdv_cutscene.c:1269-1277). It is 6-bit because it goes straight to the DAC (E/video_display.c:424-428).

Audio bytes per frame = `rate/fps`, ×2 if stereo, ×2 if 16-bit, ÷2 if DPCM (E/gdv_cutscene.c:1294-1309; P/gdv.gd:86-97). With no audio, the clock step defaults to `0x5622/0xc` (E/gdv_cutscene.c:1302).

### 3.2 Chunks

Each frame is `[audio block][u16 0x1305][u16 payload len][u32 flags][payload]`. Bad magic means end of stream (E/gdv_cutscene.c:766-771, 1374-1378).

**Flag bits:**
- `&0xf`: encoding;
- `0x10`: horizontal half-res;
- `0x20`: vertical half-res;
- `0x40`: keyframe, which clears the buffer;
- `>>8`: start offset, or palette-fade RGB for fmt 1 (E/gdv_cutscene.c:772-787).

Changing half-res bits re-lays the buffer in place (`gdv_reformat_pixel_buffer`, E/gdv_cutscene.c:116-197).

**Encodings** (E/gdv_cutscene.c:795-812):

| Fmt | Kind | Code | Notes |
|---|---|---|---|
| 8 | 12-bit LZ bitstream | :432-526 | about 96% of frames (:384) |
| 6 | LZ variant | :535-610 | |
| 5 | nibble RLE | :615-643 | only HAWK03A |
| 2 | 8bpp / hi-colour | :649-708 | |
| 0 | palette | :715-721 | |
| 1 | palette + clear + DAC fade | :731-758 | about 23 frames, in INTRO/MOVIE/EPILOGUE/FULLKEEP |
| 3 | no change | | |

**Back-references can reach 0x1000×bpp bytes *before* the frame buffer.** That area holds a LUT built by `gdv_init_pixel_tables` (E/gdv_cutscene.c:66-97, 908-911), and the port must reproduce it. P/gdv.gd:186-189 builds the same 4096-byte prefix.

### 3.3 Audio

- 16-bit DPCM uses the shared step table (below). The editor decodes it as interleaved L/R with separate predictors, wrapping to s16 (P/gdv.gd:414-420).
- ROTH.C selects a mixer by function pointer: `0x4e45f` does nothing, `0x4e460` is 16-bit DPCM, `0x4e519` is 8-bit (E/gdv_cutscene.c:894-900; platform/os_api.h:302-306).
- The bodies of those mixers live in `os_audio.c`, which is **not staged** [open].
- The step table is identical to the speech table (E/gdv_cutscene.c:846-862 = E/renderer.c:10115-10132).

### 3.4 Decoder structure and entry points

| Group | Functions | Port as |
|---|---|---|
| Bitstream / codec | `gdv_decode_video_chunk`, fmt8/6/5/2/0/1, `gdv_reformat_pixel_buffer`, `gdv_init_pixel_tables` | **pure transform, port verbatim** |
| Container parse | `gdv_read_file_header`, `gdv_setup_decode_buffers`, `gdv_read_frame_chunk`, `gdv_preload_frame_window`, `gdv_advance_chunk_ptr_inner` (:33-47, 877-931, 1237-1526) | port the parsing math; replace DOS I/O and the ring buffer with FileReader |
| Lifecycle | `gdv_decoder_open/close`, `gdv_decode_frame` (:1188-1207, 1431-1487, 2991-3029) | re-implement as the MoviePlayer lifecycle |
| Timing | decode pump + ISRs, `gdv_drain_pending_subframes`, `gdv_decode_subframe` (:1741-1764, 2531-2600) | port the **rules** (§3.5) onto the engine clock |
| Presentation | `gdv_setup_video_mode`, `gdv_init_frame_geometry`, blit, fades, VESA, `swap_cutscene_display_buffers` (:213-375, 950-1060, 1828-2200) | replace |
| Game glue | `play_gdv_cutscene`, `play_record_gdv_cutscene`, `run_timed_message_sequence`, gallery (:2615-2847, 3071-3141; E/dialogue_ui.c:1884-1996) | port the logic; drawing is engine-side |

### 3.5 Timing and sync

- The timer runs at 60 Hz [inferred from ECX=0x3c passed to `gdv_audio_setup_voices`, E/gdv_cutscene.c:2313, and the 0x3c00 = 60·256 refill].
- Each tick, `[db0] -= fps<<8`; on underflow one frame is decoded and 0x3c00 added (E/gdv_cutscene.c:2533-2549, 2571-2587, 1312). Frames therefore arrive at exactly the header fps.
- **Quirk:** a sample rate of 0x52b0 (21168 Hz) adds 0x80 to the step, i.e. +0.5 fps (E/gdv_cutscene.c:2309-2310).
- **With audio, the video is slaved to audio.** The pump only refills (E/gdv_cutscene.c:2538). Frames are decoded by `gdv_drain_pending_subframes` while audio has queued at least 2 subframes (E/gdv_cutscene.c:1758-1764).
- **Frames are decoded but not displayed while 6 or more subframes are pending**: the catch-up drop rule (E/gdv_cutscene.c:1745).
- End of playback: the frame counter reaches 0xffff, or the callback latches end of stream (E/gdv_cutscene.c:2968-2974, 1700-1701).

### 3.6 Subtitles during video, and skipping

Subtitles come from the per-frame callback `run_timed_message_sequence`, installed at desc+0x1c (E/gdv_cutscene.c:2647).

**Stream location.** DBASE400 at `rec+0x10`, `rec+0xa` bytes long. It is loaded into `g_message_resource_handle`, but only if flag `0x71384` is set (E/gdv_cutscene.c:2771-2797). That is probably the subtitle option [inferred].

**Entry format:** `{u16 size, u16 timestamp, u8 colour, text}`, padded to 2; the terminator is `{0,0xFFFF}` (P/dbase_400.gd:45-76, 191-206).

**Clock.** Each decoded frame adds samples-per-frame to a counter (E/gdv_cutscene.c:763). Time = `samples/0x89d + 1`, which is tenths of a second at 22050 Hz (E/dialogue_ui.c:1914-1918) [inferred unit].

**Trigger:**
- An entry whose timestamp is reached is drawn if its size is over 6.
- If a subtitle is already on screen, that callback only clears it. The next callback draws the new one (E/dialogue_ui.c:1918-1956).
- The last subtitle stays until the movie ends, because 0xFFFF is never reached.

**Colour.** The colour byte indexes the game palette. It is remapped to the nearest movie-palette entry using the distance 4|dG|+2|dR|+|dB|, with ties going to the later index. Results are cached per index and reset on each palette upload (mode 3) (E/dialogue_ui.c:1899-1908, 1926-1931; E/video_display.c:209-226).

**Placement:**
- y = `screen_h/2 + 0x48`, plus another 0x48 when screen_h ≥ 301;
- centred, at most 2 lines;
- flat colour with no outline (flags 0xc) (E/dialogue_ui.c:1932-1944).

**Skip.** Callback mode 2, at each frame boundary, dequeues **one** key. Only scancode 1 (Esc) ends the movie; other keys are consumed and ignored (E/dialogue_ui.c:1891-1896).

### 3.7 Triggers and file mapping

**DBASE100 op 0x07** (E/dbase100.c:922-942):
1. idx = operand-1, which must be below `base+0x20`.
2. rec = `base+[base+0x24]+idx*0x14`.
3. The name must be non-empty.
4. Call `play_record_gdv_cutscene(rec)`, which:
   - stamps the gallery index and increments `g_cutscenes_seen_count` (E/gdv_cutscene.c:2772-2776);
   - backs up and slides the screen unless `0x83b20==0x4d2` (the boot intro) (:2809-2825);
   - builds the path **`<g_dir_gdv>` + 8-byte name + `.GDV`** (:2827-2831);
   - calls `play_gdv_cutscene` (:2840).

**Other triggers:**
- RAW 0x2b and the boot/new-game/death records reach op 0x07 through §2.2.
- Map changes do not play movies themselves: `cmd_map_transition` only latches the level change (E/raw_commands.c:522-531). The between-level films come from DBASE100 chains [inferred; ROTH_STATE.md:245-247 says the same].
- Gallery replay: menu action 0x800000 (E/menu_hud_ui.c:1766-1780; E/gdv_cutscene.c:3071-3141).
- DBASE300-embedded GDVs are silent inventory close-ups, opened from an existing handle plus an offset (flags 0x1890 | 0x1000) (E/file_config.c:929-940). They belong to the inventory reader, but share this decoder.

**Stream flags** set by `play_gdv_cutscene`:
- 0x48980 normally; 0x8190 (includes 0x10, audio off) when audio init failed, i.e. `0x7675a=0xff` (E/gdv_cutscene.c:2643; E/audio.c:2188);
- |0x10 if there is no digital device (:2651-2652);
- |0x800000 on a 300-line screen (:2659).
- If opening fails with error 0x20, an error box is shown and the user may retry (:2664-2685).

---

## 4. Text and fonts

**DBASE400 record:** see §2.1. Records are padded to 4 bytes (P/dbase_400.gd:4-9, 131-137). Short lines of 0x14 bytes or less read without speech go into a 32-node MRU cache; this is a speed optimisation only (E/dialogue_ui.c:273-334; E/dbase100.c:396-401).

**Font.** The glyphs are a table built into the executable at `0x70f12`. Its header is `ff 00 0b 00 03 00`: max char 255, line step 11, default advance 3 (data/obj3_symbols.h:113). Each glyph offset is at +6+2c. A glyph's advance = low nibble + 1 (E/renderer.c:337-348; E/text_font.c:300-360). No loader writes it [inferred: static data in the EXE].

**Control codes in laid-out text** (E/text_font.c:320-345):
- `0x01 c`: set colour through a 12-step gradient ramp, or flat with flag 8;
- `0x02 lo hi`: x advance;
- `0x0a` / `0x0d`: newline.

**Wrapping** (`layout_timed_message_text`, E/dialogue_ui.c:344-480):
- Width budget is `maxw-12`.
- Lines break at the last space, and `^^` forces a newline (:376).
- After `param_5` wrapped lines, the paragraph is cut with `"..."` (+6 px). The rest of the text continues as the next record (:404-408).
- Each line is centred: `x=(maxw-w)>>1` (:414).
- Line height is 11 (:421).
- **A word wider than the budget with no earlier space makes the whole layout return 0**, and then no window opens (:398-399).

Per-caller limits:
- choice menu: 10 lines (E/dialogue_ui.c:1181);
- monologue: 3 (:1229);
- inspect topics: 16 at width-0x50 (:1096-1097);
- movie subtitles: 2 (:1932-1933).

**Timing of lines:** §2.5 for monologue, §3.6 for movies. The timed message overlay counts down by frame ticks (E/dialogue_ui.c:900-906).

**Positions:**
- monologue: `view_y+view_h+4`, or `panel_y+0x54` when the inventory is open (E/dialogue_ui.c:1021-1028);
- choice panel: vertically centred (E/dialogue_ui.c:814-826).

---

## 5. Speech (DBASE500)

**Clip header** (E/audio.c:1245-1258; P/dbase_500.gd:9-25): at offset `clip*8` there is a 0x2c-byte RIFF-like header with the id `"FFIR"`.

| Offset | Field |
|---|---|
| +0x14 | u16 format; 0x2a (42) means DPCM |
| +0x18 | sample rate |
| +0x20 | block align, used as a divisor |
| +0x28 | data size |

**DPCM.** `pred += T[byte]; out = (s16)pred`, with a 32-bit predictor and output truncated to 16 bits (E/renderer.c:9947-9956). The loop runs at least once even when count ≤ 0.

**Table:** `T[0]=0`, then `c=0x40, s=0x2d` with `acc+=c>>5; c+=s; s+=2`, storing the pairs ±acc, and `T[255]` is a final lone value (E/renderer.c:10115-10132; U:98-112).

**Streaming** (E/audio.c:1126-1146, 1175-1199, 1300-1315): 0x8000-byte ping-pong buffers. DPCM reads half the raw bytes and decodes to twice the size.

**Lookup and start.** Op 0x05 reads the DBASE400 line → `read_next_dialogue_line(flag=1)` → `prime_voice_clip(dword+0)` (E/dbase100.c:393-395; E/audio.c:1208-1292).
- `+0x14` (the sample count used for dwell) is zeroed first (E/dbase100.c:389). A missing clip therefore means an untimed, click-to-continue line.
- **Speech volume below 0x80 skips decoding but keeps the header-derived dwell** (E/audio.c:1130, 1248-1252) [inferred consequence].
- `g_voice_sample_rate` is really the speech *volume* (E/audio.c:1871-1877).

**Stop:**
- `dialogue_voice_force_end` (E/audio.c:1334-1350);
- `dialogue_voice_stop_all` on load or quit (E/audio.c:1355-1366);
- end of stream: the driver callback sets state 2 (E/audio.c:1178), and the pump then resets the text (E/audio.c:1316-1320).

**Sync with subtitles.** Proportional segment dwell, holding the last segment until the audio ends (§2.5).

[open] The playback rate handed to the driver is not taken from the clip header (E/audio.c:1277-1288). Use the header's +0x18 rate, and check that retail block-align makes `size/align` a sample count.

---

## 6. Rules vs presentation

**Port 1:1 (pure logic):**
- The GDV codec, `gdv_decode_video_chunk` and all six formats plus reformat and the LUT prefix. **This is a pure transform from bytes to frames and must be ported as is, byte-exact.**
- Header and chunk parsing math.
- GDV audio sizing and DPCM (**pure, port as is**).
- Speech DPCM plus its table (**pure, port as is**).
- `layout_timed_message_text` (it sets line breaks, the "..." cut and the number of segments, so it drives dwell). **Port as is, using the original font's advance table even if the glyphs are drawn differently.**
- The whole DBASE100 interpreter, action queue, REQUEUE, choice builder, branch walk, `filter_dbase100_active_records`, and op 0x0b RNG use.
- The dwell and segment sequencer (`render_text_ui` state part), busy/gate states (0x6ffff / 0x7ffff), and the pump's sequencing.
- Subtitle schedule (clock, timestamp rule, clear-then-draw, the 2-line cap), the Esc-only skip, and the frame-rate and drop rules.
- Gallery stamping and counter; `0x83b20` / overlay flags as they gate the queue.

**Replace:**
- VESA/Mode-X setup, blitters, geometry, DAC fades, and `write_vga_palette` port I/O. The palette becomes data for `AnimTextures`.
- Dirty rects, framebuffer save and restore, screen slide-in/out, and `present_cutscene_frame`.
- Glyph rasterizing and the colour ramp (but keep the colour *index* semantics).
- The mouse hit-test pixel maths (keep the selection result semantics).
- SOS driver setup, DOS I/O, and the ISR mechanics.

**Engine hookup** (M/movieplayer.cpp):
- **Yes, GDV can be added as another `MoviePlayer`.**
- Implement `Frame(uint64_t clock)` (returns false at the end), `GetTexture()`, and optionally `Start`/`Stop` (M/movieplayer.cpp:45-62).
- Use `AnimTextures` with `SetSize(AnimTexture::Paletted,w,h)` and `SetFrame(pal,pixels)` as SmkPlayer does (M/movieplayer.cpp:750, 784). Expand the palette from 6-bit to 8-bit, and expand half-res frames (flags 0x10/0x20).
- Use `MovieAudioTrack::Start(rate, ch, MusicSamples16bit, cb)` and `GetClockTime` for audio-master sync (M/movieplayer.cpp:88-129).
- Add a magic check `94 19 11 29` in `OpenMovie` next to MVE (M/movieplayer.cpp:831-906).
- Needed beyond the interface:
  - an `OpenMovie` variant taking a FileReader plus offset, for DBASE300;
  - exposing the decoded-sample clock so the subtitle logic (§3.6) can run in a native overlay;
  - a custom skip rule (Esc only). Generic ScreenJob skipping differs;
  - playback that blocks the interpreter, because the original is synchronous (E/gdv_cutscene.c:2694).

---

## 7. Dependencies, size, traps, open questions

**Depends on:**
- DBASE100 loader, flag bitmap and inventory give/remove/count (items reader);
- RAW command dispatcher for 0x23/0x36/0x2b (map-scripts reader);
- RNG `rng_next_index_for_count`;
- music (op 0x1a) and sound effects (op 0x19);
- savegame (the flag bitmap, and gallery bytes if saved [open]);
- input (Esc, Enter, Up/Down, click).

**Estimated size** (C++ lines):

| Part | Lines |
|---|---|
| GDV codec | ~550 |
| Container parse and player | ~450 |
| GDV audio | ~150 |
| Subtitle scheduler | ~150 |
| DBASE100 interpreter and queue (shared with the items reader) | ~600 |
| Choice / monologue state and layout | ~450 |
| Speech | ~250 |
| DBASE400/500 readers | ~150 |
| **Total** | **~2,750** |

**Traps:**
1. Operand-0 records are no-ops (E/dbase100.c:633).
2. The op 0x0d look-ahead into op 8 (:656).
3. A single visible choice auto-runs (:724).
4. The branch ordinal counts hidden choices (:710-712).
5. Queue overflow above 8 is dropped (:439).
6. REQUEUE overwrites slot 0 (:947).
7. A layout returning 0 aborts the window (E/dialogue_ui.c:398).
8. Colour 0 becomes 0x20 (E/dbase100.c:388).
9. The subtitle clock counts decoded frames, not played audio (E/gdv_cutscene.c:763).
10. The display-drop threshold is 6 (:1745).
11. The fmt-8 unary length takes only 8 bits (:449-468).
12. Negative back-references land in the LUT prefix.
13. +0.5 fps at 21168 Hz (:2309).
14. Only Esc skips a movie; one key is consumed per frame.
15. The DPCM do-while runs once when count is 0 (E/renderer.c:9950).
16. The gallery index is written into the in-memory cutscene record (E/gdv_cutscene.c:2774).
17. The playback-loop re-seed bugfix (E/gdv_cutscene.c:2950-2960). Port the fixed behaviour.

**Open questions:**
- (a) `render_text_ui` scales the dwell by `G32(0x82809)` = segment **0**'s length, not the current segment's (E/dialogue_ui.c:1007). This is either a lift bug or original behaviour; check the disassembly at 0x1f0e8.
- (b) The GDV audio mixer bodies 0x4e460/0x4e519 are not staged, including the mono and 8-bit DPCM cases.
- (c) The real tick rates behind the 60 Hz and 70 Hz inferences.
- (d) The meaning of 0x71384 and 0x83e90 (subtitle and text options).
- (e) Whether the gallery stamps persist in savegames.

---

## 8. Side-by-side acceptance tests (port vs ROTH.C)

| # | Test | Pass criterion |
|---|---|---|
| T1 | Codec: decode **all 164** `DATA/GDV/*.GDV` frame by frame with ROTH.C `gdv_decode_video_chunk` (the harness used by E/gdv_cutscene.c:384-386) and with the port | Decode buffer byte-identical on every frame; frames = header+6. Must cover HAWK03A (fmt 5) and INTRO, MOVIE, EPILOGUE, FULLKEEP (fmt 1). |
| T2 | Audio: decode each file's audio | Samples identical; total = frames·rate/fps per channel. |
| T3 | Timing: play INTRO without and with audio | Wall duration = frames/fps ±1 frame. With audio, displayed-frame count equals ROTH.C's under the same injected delay (drop rule ≥6). |
| T4 | Subtitles: boot dialogue record 1 (E/game_core.c:756) → its op 0x07 → cutscene record; parse its DBASE400 stream | For each entry k, the frame where the text appears is the first frame n with `floor(n·spf/2205)+1 ≥ ts_k`, one callback later if a subtitle was already up. Text, colour after remap, and 2-line layout identical. |
| T5 | Skip: inject Esc at frame 10, and 'A' at frame 10 | Esc ends at the frame-10 boundary; 'A' plays to the end. |
| T6 | Choice: for each dialogue record containing op 0x08 (102 such commands, DBASE100_commands.md), seed identical flag bitmap and inventory, then select every visible index (and each flag state of preceding 0x0d/0x8d gates) | Visible-choice count and text identical; resulting flag bitmap, inventory, sequence of spoken DBASE400 offsets, cutscene indices played, and queue contents identical. |
| T7 | Single visible choice: gate all choices but one | No menu is shown and branch 0 runs (E/dbase100.c:728-733). |
| T8 | Random op 0x0b: same RNG state in both | Same sub-chain chosen. |
| T9 | Engine records 1 to 4 (boot, quit, new game, death) | Identical side-effect traces. |
| T10 | Speech: for 20 op-0x05 lines, compare PCM and dwell | PCM identical to `decode_dpcm_block`; dwell ticks = `(size/align)·70/rate`; segment count and switch ticks identical. Also check a line with a missing clip, which waits for a click. |
| T11 | Layout: every DBASE400 string through `layout_timed_message_text` at maxw 320 and 640 with caps 2, 3, 10, 16 | Identical line records and control bytes. |
| T12 | Gallery: play 3 distinct cutscenes | `g_cutscenes_seen_count`=4, `rec+0x13` = 1, 2, 3, and gallery titles are from `rec+0xc`. |
