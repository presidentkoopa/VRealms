# REMAROTH — start here

> **A LANE RAN OVERNIGHT INTO 2026-10-03. Read
> `HANDOFF_SESSION_2026-10-03.md` before anything else in this file.** The use
> key opens doors, and all four reachable trigger channels are verified firing
> — including 23 enter-sector triggers that had been bound to nothing at all.
>
> **Before your first run**, read its §3: two rig faults were corrupting results
> all session (the window pauses when it loses focus, and a launch steals the
> owner's keyboard), and `-config tools/rothdiff/captures/capture.ini` is not
> optional.
>
> **The previous evening's handoff is wrong about the door**, and its "THE EXACT
> NEXT STEP" would have introduced a bug. Corrected in place at that section.

> **A SECOND LANE RAN ON 2026-10-02 (evening). Read
> `HANDOFF_SESSION_2026-10-02_evening.md` before this file.** Most of Step 1 and
> all of Step 2's sprites are done; the VR scale is fixed; the oracle now runs
> unattended in any map.
>
> **A THIRD LANE RAN LATER THE SAME EVENING. The use key opens doors.** Six of
> six in STUDY1. The cause was not the trigger layer: a door leaf is one-sided
> blocking geometry with no line special, so it swallowed the use ray and
> shadowed every trigger line behind it. `docs/TRIGGERS_the_use_key_problem.md`
> §2.4, numbers in `docs/REMAROTH_MEASURED.md` §9.
>
> **Two documents mislead on this, and both are corrected in place rather than
> deleted.** The evening handoff's "THE EXACT NEXT STEP" and
> `TRIGGERS_the_use_key_problem.md` §2.2 both say to wire a caller for
> `FireSectorTriggers`. **Do not** -- it has no caller deliberately, and
> `GAME_core.md` §5.2 agrees with the code comment that says so. §2.3's
> classification table is also narrower than it reads.
>
> **THE §5.2 CLASSIFICATION IS NOW DONE, AND THE TRIGGER LAYER RUNS.** Every
> trigger carries its event; `0x13` is bound and firing; `0x19` fires through a
> ray that can hit a flat; `0x1a` is off the use path. Entering STUDY1's sector
> 174 **warps the map to STUDY3** -- the first Realms scripted map transition
> this port has run from play. `docs/REMAROTH_MEASURED.md` §11 has the per-opcode
> table, the ROTH.C citations for the facing mask, and the list of what is
> deliberately not built.
>
> **`0x13` had been bound to NOTHING: 23 records in STUDY1, 35% of its
> triggers.** It matched neither classifier and fell through the binding loop in
> silence. "12 sector-keyed", in every handoff including this file's own §5, was
> really 35. If a count in an older document looks authoritative, check whether
> anything was counting the thing it claims to count.
>
> The live work now: the four items under §11's "Open" and "What ROTH.C says and
> the port does not do" -- sector 409's `0x19`, the water-variant Z test, the
> leave refire (`+0x06 & 0x40`), and the platform-top flag.
>
> Two things in this file are now actively wrong:
> `doorgeom -cam` and `meshcheck -v` print angles in the PRE-FIX convention
> (negate them), and the neutral post-processing list is missing
> `vid_fixgamma 0`, `vid_blackpoint 0`, `vid_whitepoint 0` -- without which
> every picture comparison is wrong by a gamma of about 1.47.
>
> **BEFORE YOUR FIRST RUN, read `docs/REMAROTH_MEASURED.md` §12.** Always pass
> `-config tools/rothdiff/captures/capture.ini`, which is a COPY of the owner's
> ini with `vr_mode` forced to 0, so a run cannot read or write their settings.
> Two attempts at writing a minimal one instead cost the owner a disrupted
> session each -- one took over the headset while they were wearing it. Also:
> a `logfile` line in a capture cfg **does not work** and loses the run's output
> (read `doomxr-log.txt`); `+exec` runs BEFORE the deferred `map`, so a console
> command in a cfg finds no level; and the engine exits `0xC0000409` on shutdown
> even on a wholly successful run, so **an exit code cannot tell you whether a
> run worked.** §10 and §12.

**For a lane picking this up on 2026-10-02.** Most of this project is already
worked out and written down. This page says which document holds what, which
parts of those documents are out of date, and what to do in what order. It
repeats a rule only where two documents disagree and you need to know which wins.

- Repo: `E:\DOOMWork\REMAROTH`
- The original, as C: `E:\VRealms\tools\ROTH.C` — **read it, never edit it**
- The original, runnable: `E:\DOOMWork\_oracle` (game dir) and `E:\DOOMWork\_croot`
- Tree state: clean, **22 commits unpushed**. Only the owner can push. This page
  and `docs/REMAROTH_MEASURED.md` are new and uncommitted.

---

## 1. Read these first, in this order

| # | File | Read | It gives you |
|---|---|---|---|
| 1 | `HANDOFF_REMAROTH.md` | §0, §1, §2, §10, §11 properly; the rest once | The mission and hard constraints (§0). The two mistakes that cost the most (§1, §2). **The order of work (§10).** How to work with the owner (§11). |
| 2 | `HANDOFF_SESSION_2026-10-02.md` | all | What the last lane changed, and where it stopped (§3). |
| 3 | `docs/REMAROTH_MEASURED.md` | all | Everything measured against the running original, including two things found today that the handoffs do not know: §1 (pose angles) and §4 (the start-pose lighting difference). |
| 4 | `ROTH_SURFACES_FIX.md` | §0, §1, §3, §7 | The surface rules, written by the owner's designer. Authoritative except where §2 below says otherwise. |

Stop there and start work. Everything else is reference; open it from §3 below
when the task calls for it.

`HANDOFF_REMAROTH.md` itself tells you to read `ROTH_STATE.md` next. Don't read
it front to back: it was written 2026-09-25/27 and half of it is superseded. Use
only the parts listed in §2.

---

## 2. Which document to believe

Newest to oldest. When two disagree, the higher one wins. A measurement beats
all of them (`HANDOFF_REMAROTH.md` §1).

**And a document written yesterday is still a document.** The rule above is
about distance from the running game, not about age: "our documents" in
`HANDOFF_REMAROTH.md` §1 includes the one a lane wrote three hours ago. On
2026-10-02 a page's own conclusion was quoted into a handoff as the exact next
step, and it was wrong in a way that would have introduced a bug — while a code
comment that disagreed with it was right. Where a page says "the likely reason",
read an open question with a candidate attached. Where it reports a number,
trust the number.

| Document | Trust it for | Do NOT trust it for |
|---|---|---|
| `docs/REMAROTH_MEASURED.md` §9, §10, §11, §12 | the door/use numbers, the per-opcode trigger table, the ROTH.C citations for the facing mask and the three sector variants, the rig and config rules. **Newest, and measured.** | — |
| `docs/TRIGGERS_the_use_key_problem.md` | §2.1 and §2.4 (the real cause), §3.1, §3.3, §3.7 | **§2.2's "wire a caller for `FireSectorTriggers`"** — would introduce a bug; it is uncalled deliberately. **§2.3's table** — overstates the disagreement; the key-space rows are not one. **§4's original verdict** that §2.3 was "the likely reason" a door would not open. All corrected in place. |
| `HANDOFF_SESSION_2026-10-02_evening.md` | §1 what is settled, §3 what cost the most (the `vid_fixgamma` and stale-angle traps), §4 the instruments | **§2.1's "THE EXACT NEXT STEP"** — all of it is now done, and its last instruction ("wire a caller for `FireSectorTriggers`") would have introduced a bug. Its "all 12 sector-keyed" is an undercount of 35. And none of its three reasons was why a door would not open. Corrected in place at that section. |
| `HANDOFF_SESSION_2026-10-02.md` | doors §1, the mesh §2, the capture rules and build notes §3, owner rules §5 | **"The two engines AGREE on a pose"** — true only of pose A. Poses B and C look in opposite directions. `docs/REMAROTH_MEASURED.md` §1. |
| `HANDOFF_REMAROTH.md` §10, §12, §13, §14 | the plan, doors, meshes, inventory. These are the current sections. | §12's closing "NONE OF THIS HAS BEEN SEEN": it has, see §10 item 3. |
| `HANDOFF_REMAROTH.md` §3, §5, §7, §9 | what is done, the rules to cite, directional sprites, build and launch traps | — |
| `HANDOFF_REMAROTH.md` §4 | the **list** of what is outstanding, and the "known discrepancies" at its end | its numbering as an order (§10 is the order), and items 2, 3, 4, 5, 8, which §10 marks done or part-done |
| `HANDOFF_REMAROTH.md` §6 | the muzzle-flash term at `0x853f6` | the shade formula: it predates the measured model. Use `docs/REMAROTH_MEASURED.md` §4. |
| `HANDOFF_REMAROTH.md` §8 | the useful addresses; why the texture-repaint approach must not be resumed | "USE `tools/oracle`": that rig is bash, ran in a Linux workspace and has no captures on this machine. On Windows the path is `tools/rothdiff` + the `oraclelog` mod, session handoff §3. |
| `ROTH_SURFACES_FIX.md` | §3.1–3.3 flats, mid-platforms, walls; §7 working rules | §3.7 "pixels nearly square": measured otherwise (`HANDOFF_REMAROTH.md` §5). §3.4 sprites and §3.6 sky: use `HANDOFF_REMAROTH.md` §10 item 1, §4 item 4 and the measured doc. |
| `ROTH_LIGHTING.md` | the idea (a light byte sets how fast dark closes in), the table layout in the DAS, the flash | which metadata field sets the shift (STUDY1 is `>> 7`, not `>> 5`); it has no candle mode at all; "the engine needs a new light mode" and "lighting is off" are both long done. Tint ramp: `HANDOFF_REMAROTH.md` §10 item 2. |
| `ROTH_COMMANDS.md` | record layout, key lookup, the opcode tables, the active-effect pool and its 15 ticks, "what is still open" | **the trigger table** ("0x18 enters a sector" and the rest). Wrong. The right one is `ROTH_GAME_PORT.md` §1 and `docs/GAME_core.md` §5.2. |
| `ROTH_GAME_PORT.md` | the architecture and build order of the game layer | §1's body: the banner at its top says the wiring is fixed |
| `ROTH_STATE.md` | §1 how the loader is laid out and the reader self-test; §3 format facts; §6b why standalone boot needs an episode and a skill; §8 the two link errors | §5, §6 (doors reverted — they are live and open), §7 (lighting off, sky, the "massive armour", `roth_objectangle`) |
| `ROTH_BETTER.md` | deliberate differences from the original, and their status. **Append to it.** | §7 "interpreter not built" |
| `README.md` | — | "start with ROTH_STATE.md", and `-iwad <any iwad>`: a launch needs `-iwad vrealms.pk3` |

**History only — do not steer by these:** `STATUS_2026-09-28.md`,
`ROTH_NATIVE_HANDOFF.md`, `ROTH_NATIVE_PLAN.md`, `ROTH_CONVERSION_PLAN.md`,
`ROTH_BRIEF.md`, `ROTH_README.md`, `ROTH_ENGINE_CHANGES.md` (it still says no
engine changes were made), `roth_pipeline/`, `tools/rothdiff/README.md` (its
identity-buffer comparison is superseded; the tools beside it are not).

**Not this project:** `ENGINE_DELTA.md`, `FORK_CHANGES.md`,
`CAPABILITY_MANIFEST.md`, `VR_INTERACTION_PLAN.md`, `BILLBOARDS.md` and the
other engine-fork documents in the repo root.

---

## 3. Where to look, by subject

| Subject | Rule | Evidence / detail | Code |
|---|---|---|---|
| Projection, FOV | `HANDOFF_REMAROTH.md` §5 | measured doc §1 | `rothmap.cpp` (`ROTH_FOV_DEGREES`, `ROTH_VIEW_VSTRETCH`), `hw_entrypoint.cpp` (`r_view_vstretch`) |
| Flats, mid-platforms | `ROTH_SURFACES_FIX.md` §3.1–3.2, `HANDOFF_REMAROTH.md` §5 | measured doc §2; `docs/SURF_rothc_truth.md` §1–2 | `src/roth/roth_surface.*` — the only place a scale, shift, flip or anchor may be computed |
| Walls | `ROTH_SURFACES_FIX.md` §3.3, `HANDOFF_REMAROTH.md` §5 | measured doc §3; `docs/SURF_rothc_truth.md` §3 | `roth_surface.*`, `rothmap.cpp` |
| Colour key, solid colours | `HANDOFF_REMAROTH.md` §5 (the three-way test) | `tools/oraclelog/skyaudit.cpp` | `roth_surface.*` |
| Lighting | measured doc §4 | `ROTH_LIGHTING.md` for the tables and the flash | `main.fp` (`R_RothShade`), `main.vp`, `hw_drawinfo.cpp` (the packed word), `roth_runtime.cpp` (`InitLightSwitchesAtLoad`, light followers) |
| Palette shading, glow | `HANDOFF_REMAROTH.md` §3 table, §10 item 2 | `src/roth/roth_palshade.h` header comment | `roth_palshade.*`, `func_roth.fp` |
| Sky | `HANDOFF_REMAROTH.md` §4 item 4 | measured doc §5; `ROTH_BETTER.md` §9 | `hw_skydome.cpp` (`RenderRothSky`), cvar `r_skyband` |
| Sprites, placement | `HANDOFF_REMAROTH.md` §10 item 1 | measured doc §6; `tools/rothdiff/prefixcheck.cpp` | `roth_objects.cpp`, `roth_texture_object.cpp` |
| Directional sprites | `HANDOFF_REMAROTH.md` §7 | `tools/rothdiff/dircheck.cpp` | `roth_das.*` (`ReadDirectional`, `ResolveDasId`), `roth_objects.cpp` |
| Doors | `HANDOFF_REMAROTH.md` §12 | session handoff §1; `tools/rothdiff/doorgeom.cpp` | `rothmap.cpp` (leaf builder), `roth_runtime.cpp` (`SwingDoor`, console command `roth_door`) |
| Meshes | `HANDOFF_REMAROTH.md` §13 | session handoff §2; `tools/rothdiff/meshcheck.cpp` | `roth_das.cpp`, `roth_objects.cpp` |
| Level logic | `ROTH_COMMANDS.md` (not its trigger table) | `HANDOFF_REMAROTH.md` §10 item 4; `tools/rothdiff/opcodecensus.cpp` | `roth_runtime.cpp`, `roth_commands.*` |
| Inventory | `docs/GAME_inventory.md` | `HANDOFF_REMAROTH.md` §14 — read it before believing the spec is wrong | not started; no DBASE100 reader exists |
| The game layer | `ROTH_GAME_PORT.md` | `docs/GAME_core.md`, `GAME_combat.md`, `GAME_dialogue.md`, `GAME_audio_save_menu.md`, `GAME_engine_hooks.md` | not started |
| File formats | `ROTH_STATE.md` §3 | `docs/NATIVE_rothc_rules.md`, `docs/NATIVE_rotheditor_rules.md` (roth-editor approximates; ROTH.C wins) | `roth_raw.*`, `roth_das.*` |
| Build and launch traps | `HANDOFF_REMAROTH.md` §9 | session handoff "Build notes"; `ROTH_STATE.md` §8 | `auto-setup-windows-vr.cmd` |
| Building and running ROTH.C | `tools/oraclelog/README.md` | session handoff §3 | `tools/oraclelog/`, `tools/rothdiff/rothdiff_plugin.c` |

Every load writes `roth_<MAP>.log` beside the exe
(`build-dxr\RelWithDebInfo\`). It says what was built, what was not, and every
counter. Read it after each run; it is truncated on the next load
(`HANDOFF_REMAROTH.md` §9).

---

## 4. Running the two engines

Pieced together from the last lane's own files. Check each against the file
named before relying on it.

**Ours.** `tools/rothdiff/captures/pair.cfg` is the capture script.

```
build-dxr\RelWithDebInfo\doomxr.exe -iwad vrealms.pk3 -rothpath E:\DOOMWork\_oracle +map STUDY1 +exec E:/DOOMWork/REMAROTH/tools/rothdiff/captures/pair.cfg
```

`rothdiff_shot <x> <y> <angle512> <file> [pitch]` queues a screenshot and quits
after the last one. `rothdiff_sprites <x> <y> <angle512> <file>` writes every
prop's screen rectangle. `roth_door <tag|all> [speedM] [dwell] [afterTics]`
swings a door. `vrealms_iwad/MAPINFO` declares STUDY1, STUDY2 and LRINTH1; for
any other map see `HANDOFF_REMAROTH.md` §9, "Most maps are not in MAPINFO".

**The original.** Both mods must be installed in `E:\DOOMWork\_oracle\mods\`:
`oraclelog` skips the movies and presses Play; `rothdiff` pins the pose and
writes the frame. `captures/oracle2.err` is the log of a good run.

**Use `tools/rothdiff/run_oracle.ps1`. Do not hand-roll the command line.**

```
.\run_oracle.ps1 -Map STUDY2 -PoseFile poses_study2.csv
```

It runs unattended: no intro movies, no "insert CD", no main menu, and **no
quicksave in the target map**. Three things it gets right that cost most of
2026-10-02 to learn, all of which look like a frozen or broken game:

- **Never pass `--skip-gdv`.** It HIDES the `.GDV` files (`dos.c:152`), the open
  then fails, and a failed open raises the CD-swap retry prompt
  (`file_config.c:913`) that nothing headless can answer. Answering it "retry"
  re-opens a file that is still hidden — that is the 163,973-call loop an
  earlier lane recorded against the message box in general. The movies are
  skipped by `oraclelog` refusing to PLAY the cutscene instead, so nothing is
  hidden and no prompt is ever raised.
- **The main menu blocks.** Forging mode=1 does not leave the menu's own loop.
  `oraclelog` answers that box with 1 = Play, bounded to before gameplay and to
  four boxes so it cannot spin.
- **`-Map` replaces the quicksave entirely.** `rothdiff_plugin.c` drives the
  game's own warp path (see `VA_WARP_DEST`), so any of the 44 maps can be
  captured with nobody at the keyboard. The single quicksave slot used to mean
  that making a save in a new map destroyed the previous map's capturability.

**`-Quickload` is opt-in and currently HANGS**, so only reach for it if the
capture genuinely needs a saved game's state. Arming the savegame request leaves
the game rendering but not advancing the `0x90bcc` tick, and `on_frame_game` is
deduped on that tick, so the rig stops being called and nothing is captured. It
looks exactly like a frozen game. Not diagnosed further; `-Map` made it
unnecessary.

The run writes `<pose>.frame.pgm`. Then
`framepng <ROTH folder> <in.pgm> <MAP> <out.png>` and `sidebyside.ps1`.

Rebuild either mod with its own `build_plugin.cmd` (32-bit MSVC; there is no
mingw on this machine, and `rothc.exe` itself therefore cannot be rebuilt here —
it needs `i686-w64-mingw32-gcc`).

**Three settings stick between runs** because they are archived:
`roth_lighting`, `roth_pattern`, `roth_palette_shading`. An old capture script
(`captures/rothdiff.cfg`) sets `roth_lighting 0` and `roth_pattern 1`. Confirm
the load report's `lighting` line reads `Realms shading` before judging a
picture, and pass `+roth_pattern 0` on the command line.

---

## 5. What to do, in order

### Step 0 — before any launch

Ask the owner. One approval is one run, headless included. Never capture while
they are in the headset. Say what will appear and for how long.

### Step 1 — finish the comparison instrument

This is where the last lane stopped (session handoff §3) and it is what the
owner chose over starting the game layer. Nothing below can be judged until two
frames of the same view can be laid over each other.

1. ~~**Make both tools agree on the angle.**~~ **Code change made, NOT yet
   proven.** `RothAngleToDoom` (`src/roth/roth_diff.cpp:86`) now adds the angle,
   matching the player start. All three of its callers are the capture camera,
   no object path uses it, and `doorgeom` has no conversion of its own, so
   nothing needs regenerating. The claim was re-confirmed twice before touching
   it: by reading the two conversions, and by pairing `ORACLE_poseC` against
   `REMA_poseC` from the frames already on disk — the original is in the long
   hall, ours faces a panelled wall. **PROVEN 11:47** — `REMA2_poseC.png` shows
   the chandelier, the colonnade, the blue carpet and the far door between its
   two sconces, as the original does. Pose A, the control, is unchanged.
2. ~~**Capture ours at 640×480.**~~ **Done, no engine change.** `vid_setsize
   640 480` in the cfg is enough; `captures/pair2.cfg` has it. Beware the
   `Resolution: 3840 x 2137` line in the console — it is printed at `V_Init`,
   BEFORE the cfg runs, and does not describe the captured frames. Check the
   PNG itself. The load report read `field of view 91.82` and `view vertical
   stretch 1.1462`.
3. ~~**Neutral post-processing.**~~ **Done** in `captures/pair2.cfg`, with the
   full §8 list, not only the four cvars named here.
4. ~~**Deal with the overlays.**~~ **Done — `tools/rothdiff/uimask.py`.** Masks
   0.82% of the original's frame and 0.50% of ours. **This item's premise was
   half wrong and the correction matters:** the green reticle is NOT an overlay.
   It is world-anchored — left edge at pose A, right edge at pose C, where a
   screen-fixed element would be in the same place in both — our engine has no
   reticle code and draws it anyway, and both engines put it in the same place,
   so it cancels. Masking it deletes real geometry. Nor can colour find it: the
   crosshair and the green-jewelled item on the pose C wall share the palette
   entry rgb(0,93,49) exactly. A hue rule was written, caught the world item,
   and was removed; do not reintroduce one. The real overlays are the original's
   three HUD pieces, and ours draws none of them.
5. ~~**Run the control.**~~ **Done, and it passes.** The original's own three
   regions reproduced to the decimal through our reader, so the instrument is
   sound. Ours: pillar 42.9 against 44.2, far wall 14.0 against 13.9 — and the
   right-hand wall 19.2 against 1.7, which is measured doc §4's open difference
   reproducing on a second platform at the correct frame size. The instrument is
   right; that one is the port.

### Step 2 — point it at what is waiting, in the plan's own order

The order is `HANDOFF_REMAROTH.md` §10. Each line is a check that has never been
made, or a measured miss that has not been fixed.

**Sprites (§10 item 1)**
1. ~~`DEMO[4102]` sits 16 units high.~~ **FIXED 2026-10-02, and it was not a
   placement bug.** Prop Z was not stable after load: a prop placed below its
   floor by the modifier nibble satisfies `Z() != floorz`, so `P_ZMovement` ran
   on it every tic and snapped it back up, `NOGRAVITY` notwithstanding. Fixed by
   `+NOINTERACTION` on `LoaderProp`. Measured doc §6.
2. ~~The shared-pack snake, `ADEMO 45`, about 3.5 units high.~~ **Same fault,
   fixed by the same line.** It was drifting +4, not misplaced. Worst vertical
   error over all four reference rectangles is now 1.4 px.

   **Read this before trusting any prop number:** the same prop measured −0.2 px
   in a run's first capture and −29.3 px in its second. A placement error and a
   drift look identical in one frame. `tools/rothdiff/sprcheck.py` with
   `captures/sprites.cfg` re-runs the whole table in one launch.
3. ~~Directional view order, and `ANGLE_SENSE` with it.~~ **SETTLED 2026-10-02:
   view *i* → rotation *i*, and `ANGLE_SENSE` is right as coded (`+1`).** The
   diagonal wins 8 of 8 on an orbit of `ROTH_DEMO_O04358` in STUDY2, and the
   statue's yaw is far from zero so the test could have failed.
   `HANDOFF_REMAROTH.md` §7; `tools/rothdiff/viewmatch.py` re-runs the scoring,
   `dirprops.py` finds the props and emits the orbit.
4. ~~The per-view lateral anchor.~~ **NEEDS NO CODE — inert in every retail
   map, measured 2026-10-02.** All ten maps with directional objects report
   `0 applied, 0 with one on ANY view, 0 whose views DISAGREE`. The 9 prefixed
   directional entries are shared-pack and nothing places them; every placed
   directional prop is map-pack art, which carries no prefixes at all. The old
   counter only looked at view 0 and was corrected before being believed.
   `HANDOFF_REMAROTH.md` §10 item 1.

**Lighting (§10 item 2)**
5. The start-pose difference: the right-hand wall and the door leaf
   (measured doc §4). Found today, unexplained.
6. The storm glow has never been seen on screen. STUDY2.
7. Re-measure corridor L1 and the left bay pane now the vertical projection is
   fixed (`HANDOFF_REMAROTH.md` §4, known discrepancies).
8. The courtyard trees: redo the inconclusive A/B with
   `+roth_palette_shading 0` on the command line (same section).
9. Palette shading on sprites (`HANDOFF_REMAROTH.md` §4 item 6).

**Sky (§10, struck out but unconfirmed)**
10. Orientation, vertical placement and the one-column remainder against an
    oracle frame. Do not nudge `u` without one (`HANDOFF_REMAROTH.md` §4 item 4).

**Doors and meshes (§10 item 3, §13)**
11. Does a mesh draw through an ordinary wall? One test classifies the fault
    (session handoff §2).
12. Front and back art on the 29.3% of leaves where they differ
    (`HANDOFF_REMAROTH.md` §12).
13. Closing, blocking, the second use. **Half of this is now live:** opening by
    the use key works (the leaf calls `SwingDoor`), so the second use has a
    known caller waiting for it. Closing needs the door's own state word —
    `toggle_door_open_state` toggles, and the port only opens.
14. Swing direction: rule derived, not wired, probably an engine change
    (session handoff §1). Name the files and ask.
15. Mesh face UVs: do not implement from `face+0x24`/`+0x26`
    (`HANDOFF_REMAROTH.md` §13).

**Then:** the rest of the `0x30998` load-time table (`HANDOFF_REMAROTH.md` §4
item 7).

### Step 3 — the game layer: NOT YET

The owner has deferred it and will need convincing. Do not raise it. When they
say go, the order is already written: `HANDOFF_REMAROTH.md` §10 item 4 (the
census), §14 (the inventory steps), `ROTH_GAME_PORT.md` §4. Muzzle flash waits
for it too.

---

## 6. Rules

**Hard constraints** — `HANDOFF_REMAROTH.md` §0. Nothing derived from the game
is written to disk, cached, converted or shipped. ROTH.C's tree is never
modified. Every launch is cleared with the owner. The fork is never distributed.
Engine work is allowed, but scope it and name the files first.

**Evidence** — `HANDOFF_REMAROTH.md` §1, §2, §11.
1. The running original beats ROTH.C's code, which beats its comments, which
   beat our documents.
2. Trace every render rule from `render_world_face_list`, never the pick
   subpass.
3. Before building on a rule that was only read, re-derive it. Three specs were
   refuted on a second reading (§10, last paragraph).
4. Run the control before the experiment. A test that cannot fail proves nothing.
5. A warning in a log is a prompt to measure, not a finding (§13).
6. When a measurement contradicts a document, fix the document the same day.

**Things not to do**
- No MAPINFO `pixelratio` for the projection; the VR code reads it.
- No pushing. No two builds at once. No deleting `doomxr.iobj` unless LNK1103
  has actually appeared. No shell heredocs to write files.
  (`HANDOFF_REMAROTH.md` §9.)
- No resuming the texture-repaint capture (`HANDOFF_REMAROTH.md` §8).
- No claiming a "first" about the original.

**The owner** — `HANDOFF_REMAROTH.md` §11, session handoff §5. One short
question with short options. Yes or no first. No fix reported without evidence.
They are right that something is wrong; verify what.

---

## 7. Decisions in force

| Decision | State |
|---|---|
| Target | PC VR, Vulkan. Flat play exists to compare against the original. |
| 1:1 first | Anything "better in VR" is recorded in `ROTH_BETTER.md`, not built. |
| RAQUIA2 | Re-centred at load. |
| Props sinking into the floor | **On**, as the original does it. The owner's earlier instinct was to leave it off; it is one line to flip. `ROTH_BETTER.md` §10. Ask before changing it. |
| The sky | The faithful band first; a real VR sky is phase 2. `ROTH_BETTER.md` §9. |
| The game layer | Deferred by the owner. |

---

## 8. When you stop

Write `HANDOFF_SESSION_<date>.md` in the shape of yesterday's: what changed,
what is in flight, the exact next step, what was learned the hard way. Correct
`HANDOFF_REMAROTH.md` wherever a rule changed, and `docs/REMAROTH_MEASURED.md`
wherever a number did.
