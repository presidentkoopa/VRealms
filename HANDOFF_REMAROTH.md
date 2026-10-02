# REMAROTH — handoff

**Written 2026-09-28.** For a new lane taking over REMAROTH. Read this, then
`ROTH_STATE.md` (steps 1–8) and `ROTH_SURFACES_FIX.md` (the surface spec, which
is authoritative and was written by the owner's designer, not by us).

---

## 0. What this project is

Realms of the Haunting (1996) running inside a private GZDoom VR fork, reading
the player's own retail install at run time. **ROTH.C** — a C lift of the
original DOS binary, at `E:\VRealms\tools\ROTH.C` — is the specification.

**The mission, in the owner's words (2026-09-28):**

> REMAROTH recreates Realms of the Haunting 1:1, exactly as ROTH.C does: same
> world, same look, same rules. We use the DoomVR engine only because it carries
> the VR systems. ROTH.C is the specification. Where our engine disagrees with
> ROTH.C, the engine changes. Nothing is "done" until it matches ROTH.C in a
> test.

**Engine work is authorised** (owner, 2026-09-28: *"I authorize any and all
engine work to make our engine load ROTH 1:1"*). Scope it and name the files
first; the owner reads C++ and decides.

### Hard constraints

- **Nothing derived from the game is written to disk, cached, converted or
  shipped.** The retail install stays read-only — this is achieved with junction
  mirrors (`E:\DOOMWork\_oracle`, `E:\DOOMWork\_croot`).
- **ROTH.C's tree must not be modified.** Editing the reference costs it its
  authority as an oracle. Instrument it with the mod SDK instead.
- **Launches are cleared with the owner.** They are often in a VR headset and a
  window appearing is disruptive. Headless runs still count as launches.
- The fork is never for distribution.

---

## 1. THE ONE LESSON THAT MATTERS

**Every time this project measured, it got the answer right the first time.
Every time it reasoned from lifted x86, it was wrong.**

Wrong by reasoning: three different flat-scale constants; deleting correct
mirror code; an entire afternoon on a per-vertex-UV theory built on the wrong
render pass; a claim that pixelstretch is unused by the hardware renderer.

Right by measuring: the sky/colour-key audit; the flat-span capture; the wall
path probe.

**And beware tests that cannot fail.** The flat comparison reports
`MATCH 100.0000%` over 2.47M sample points — but both sides of it are our own
code. It proves the *encoding* is faithful to `FlatTexel`; it says nothing about
whether `FlatTexel`'s rule is right. A ceiling regression shipped behind that
number on 2026-09-28.

---

## 2. THE TRAP THAT HAS COST THE MOST TIME

**ROTH.C walks the world face list TWICE and the two passes differ.**

```
render_world_face_list          0x2ad21  renderer.c:9147   DRAWS THE PICTURE
    callers: render_world.c:117, :155, :187
render_world_face_list_subpass  0x28dbe  renderer.c:8903   CURSOR PICK
    one caller: renderer.c:9565, inside the fn that sets a ONE-PIXEL window
```

They look almost identical. The difference that matters:

```c
visible ceiling:  fill = 0x38 | ((sector[0x17] & 0x0c) >> 1)    R:9229
visible floor:    fill = 0xb8 | ((sector[0x17] & 0x03) << 1)    R:9249
pick    ceiling:  fill = 0x38                                   R:8968
pick    floor:    fill = 0xb8                                   R:8987
```

Read the pick pass and you conclude flats have no mirrors. They do. That
conclusion reached the loader and correct code was deleted on the strength of
it.

**Every rule must be traced from `render_world_face_list` to the texel fetch.**
Not the pick subpass. Not the 3D-mesh face driver (`draw_floorceil_surface`,
0x3a84e) — world flats never reach that; the dispatch at `renderer.c:13314`
tests `& 0x20` before `& 0x200`, and flats carry `0x20`.

---

## 3. What is DONE and verified

| Area | State |
|---|---|
| Readers, geometry, objects, mid-platforms, standalone boot | steps 1,2,4,5,8 done |
| Flats: scale, shift, mirror, signs | wired through `roth_surface`, 100% offline over 44 maps |
| 256×256-opaque exception | wired; mechanism confirmed (see §5) |
| Mid-platforms (rugs, table tops) | through the same module |
| Wall horizontal scale | wired 2026-09-28 from the measured rule (see §5) |
| Lower-wall anchors | explicit offsets, not Doom peg flags |
| FOV 91.82° horizontal | set on the player at load |
| Vertical projection | **fixed 2026-10-01.** Realms is NOT square-pixel — see §5 |
| Lighting | transcribed and verified on **2943 of 2943** vertices, incl. the candle mode |
| Lights out at load | `InitLightSwitchesAtLoad` reproduces `tick_cmd_45`; 139 sectors in STUDY1, same as the original |
| Light followers | control sectors under mid-platforms and void sectors behind door leaves track their parent |
| Furniture bands | `TRANS_UPLO` mid-texture bands; the study chair and ~8000 faces across all maps |
| Palette shading | `roth_palette_shading`, default on. The original's real 32-row table lookup on the GPU (`roth_palshade`, `func_roth.fp`). Walls and flats; **not sprites yet** |
| Directional sprites | placed 2026-09-28; frame tables 208/208 offline over 44 maps, 17/17 in STUDY2. **View ORDER unverified** — see §7 |

**Debug views:** `roth_shade_debug` 1 unshaded, 2 light byte, 3 Z, 4 wall/flat
marker, 5 lantern flag, 6 row, 7 per-pixel row.

### Key files

```
src/roth/roth_surface.{h,cpp}     THE single source of truth for surface rules
src/maploader/rothmap.cpp         the loader
src/roth/roth_runtime.cpp         runtime re-application (platforms, opcodes)
src/roth/roth_objects.cpp         object placement and facing
src/roth/roth_texture_object.cpp  object art registration
tools/rothdiff/                   comparison tooling
tools/oraclelog/                  the ROTH.C instrumentation mod
```

**`roth_surface` exists because the flat scale had been implemented five times
in five places with different numbers.** Nothing else may compute a scale,
offset, flip or anchor. If you find a second copy, that is the bug.

---

## 4. What is NOT done

1. **Which way round the directional views go.** The art is placed and rotating
   (§7), but nothing has confirmed that view *i* faces the direction ROTH.C
   draws it facing. If it is wrong the props are all rotated by a constant, or
   mirrored front-to-back. Needs a two-engine look, not a look at ours alone.
   This also settles `ANGLE_SENSE` (`roth_objects.cpp:102`, `+1` in code, the
   designer says `-1`).
2. **Sprites generally** — size table and vertical nibble (`ROTH_SURFACES_FIX.md`
   §3.4) untouched. The owner's words on seeing STUDY2, 2026-09-28: *"those
   pillars are too tall"*.
3. **Colour-key ceilings** — the three-way test exists in `roth_surface` but is
   not wired for ceilings/walls. This is the black side-aisle ceilings.
4. **The sky.** Realms paints a flat strip above outdoor-flagged walls; we draw
   GZDoom's dome, which pinches shut into a grey disc overhead. The rule, read
   twice on 2026-10-01 by independent readers and not broken by either:

   - **Column** `= (2 * (a(x) - viewAngle)) & 0xFF`, where `a(x)` is the ray
     angle through screen column x in 1/512 turn, from the projection LUT at
     `0x8c484`: `x = centre_x - hscale * tan(2*pi*a/512)`. **An arctangent of
     screen x, not linear in it.** One source column is 1/1024 turn, so a
     256-wide sky spans 90 degrees and tiles four times per revolution.

     As the code actually computes it (`renderer.c:5440-5469`), it is
     `(lut[screenX] - 2*viewAngle) & 0xFF` — the LUT byte is already the
     doubled angle, and `2*viewAngle` wraps the byte four times per revolution,
     which is the same statement. Worth having in this form because of what it
     rules out: **there is no column bias anywhere in the original's
     arithmetic.** The index is `u16[0x90958] + u16[0x8a30e]`, which looks like
     a hidden offset and is not — `0x8a30e` is just `max(0, view_bound_left -
     span_left)` (`renderer.c:4896-4898`, zeroed at `:4782`), a LEFT-CLIP
     amount, so the index is simply the first VISIBLE screen column and the
     lookup is on absolute screen x.

     So the suspected **one-column offset in our sky is not a missing term from
     ROTH.C** — it can only be our cylinder's texel-centre convention against
     the LUT's integer sampling. Do NOT "fix" it by nudging u on a guess: the
     magnitude is one column either way and the direction is not derivable, so
     it needs an oracle frame. (It was nearly nudged blind on 2026-10-01.)
   - **Row** starts at `viewport_top_margin - (pitch * vscale >> 7)`, so it
     **SHEARS 1:1 WITH PITCH** — it does NOT stand still, and it **clamps and
     smears at the bottom** rather than wrapping.
   - Blitted **once per wall span, not per column**. The lift's own comment at
     `renderer.c:5375` says per-column and is wrong.
   - Never paints in the cursor-pick subpass (`renderer.c:5412` bails when
     `0x90a48 != 0`).
   - The image is **256 wide, row-major**, fetched `[row<<8 | col]`.

   This replaces an earlier "no pitch term, clamp pitch so the zenith is never
   visible" plan, which came from the two now-corrected lines in
   `ROTH_BETTER.md`. `ROTH_BETTER.md` §9 settles that the faithful band comes
   first and a
   real VR sky is phase 2. The rotated-dome attempt in `6b3d12f` does NOT work
   and is superseded.
5. **Lighting, the parts not yet applied** — muzzle-flash brightening, object
   light bytes (231 in STUDY1), the glow table (face flag 0x40 with the
   storm/lightning phase counter at `0x8a355`), and tint ramp selection
   (metadata `+0x16`).
6. **Palette shading on sprites.** A user shader forces `CLAMP_NONE`, so it
   needs care before it goes on.
7. **The rest of the `0x30998` load-time init table** — moving floors and
   ceilings, geometry effects, re-runs. Only type `0x02` is implemented.
8. **Doors** — hinges verified 141/141 across 44 maps. Nothing opens yet, but
   the host is already built and the rule is now settled. See §12.

   **TWO CLAIMS THAT WERE IN THIS SECTION ARE FALSE. Both were believed earlier
   on 2026-10-01 and both were corrected the same day by reading the tree and
   the binary rather than the docs.**

   - *"The polyobject build was reverted (`e1f7ba66ba`), so the next job is to
     build four one-sided lines."* **No — it is LIVE IN HEAD.** `e1f7ba66ba` is
     an ancestor, but the leaf builder was rebuilt after it: `rothmap.cpp:1720`
     emits `Polyobj_ExplicitLine` and `roth_runtime.cpp:394` calls
     `EV_OpenPolyDoor` with `PODOOR_SWING`. The four one-sided lines in a parked
     void cell already exist (`rothmap.cpp:1676-1735`). `ROTH_STATE.md` §6 is
     stale on this and this file inherited it. **Checking `git log` is not
     enough — check the tree.**
   - *"GZDoom cannot render two-sided polyobject lines, and that is why."*
     Too strong, and irrelevant either way. GZDoom does handle them
     (`hw_bsp.cpp:669`, `:704-707`, `hw_walls.cpp:2454`, `:2508`), and the port
     does not need it: one-sided lines never raise the question. **No engine
     change is needed for doors.**

   What `6061695bab` recorded that IS true: a Realms slab's faces are shared
   with the rooms either side, so the slab cannot be made a polyobject by
   TAGGING existing faces — hence the parked void cell, which is the right
   shape and is already there.
9. **Level logic handlers** — 1,937 chains parse and execute; no handlers.
10. **The game layer** — `ROTH_GAME_PORT.md`. Not started.

### Known discrepancies, not yet explained

- **The left bay window pane in the study.** The original draws it at row 31
  through mapper 9 (masked, flat colormap); the glass takes one shade value per
  screen column, maxed across the whole pane. The pane's left edge is clipped by
  the view bound at x=181. Next step: instrument ROTH.C's wall-corner values —
  `wd_project` is static in the code ROTH.C was lifted to, so hook
  `compute_wall_column_source_offset`, or tag the mappers as is already done.
- **Corridor pose L1** is about 9 rows brighter than the original, and our Z
  there is about 8% short. Some of that may have been the vertical projection
  error fixed on 2026-10-01 — worth re-measuring before chasing it as lighting.
- **The courtyard trees render as near-black silhouettes** while the grass
  beside them is lit green (STUDY1, camera `1128 1560 128`, captured
  2026-10-01, `tools/rothdiff/captures/m_fan4123_side.png`). The trees are the
  `IT_OBJECT_DATA` fans of §13, drawn as models.

  **THIS IS AN OBSERVATION, NOT A DIAGNOSIS.** It may simply be night. An A/B of
  `roth_palette_shading` 1 against 0 at the same pose produced **byte-identical
  PNGs**, which would be a striking result — except the `logfile` line in the
  exec script never produced a console log, so there is no evidence the CVAR was
  applied at all. **The test is INCONCLUSIVE and must be redone** with the
  setting on the command line (`+roth_palette_shading 0`) and the console output
  actually captured, before anyone reasons from it. Recorded so the next session
  does not re-run the same inconclusive experiment and believe it.
- **`DEMO[4102]` sits 16 units high**, with `modifier = 0x88` — nibble 8, HANG
  clear — so the shift should be −16 and the nibble looks like it is not
  arriving. Two things now ruled out by measurement rather than argument
  (`tools/rothdiff/prefixcheck.cpp`):
  - it carries **no anchor offset** (flags_1 is 0x80, bit 3 clear, and no entry
    in any map pack has one), so the "0x84aba second term" is not the
    explanation and the two faults are independent;
  - `0x88` also sets `IM_HALF_SIZE`, and with image_type `0x10` the exponent is
    0, so this entry draws at **1.0 units per pixel, not 2.0**. The original
    scales the sprite's extents by that modifier but does **not** scale the
    nibble shift, which stays in raw world units (`renderer.c:6550-6555`
    computes `edx` with no `cl` applied). Worth confirming the port agrees,
    since a half-size entry is exactly where a size and a shift could get
    multiplied together by mistake.

  Settling it needs the oracle: `rothdiff_sprites` emits each prop's screen
  rectangle in the same form as `ORACLE_WALLLOG`'s billboard lines, so the two
  can be compared number for number. Both halves need a run.

---

## 5. Rules established from ROTH.C (cite these, don't re-derive)

**THE PROJECTION IS NOT SQUARE-PIXEL.** Realms uses a different focal length
across and down. Fitted from its own output at 640×480 (`tools/oracle`,
`flatfit.py`):

```
FX = 309.77   X0 = 320.48      horizontal FOV 91.9°
FY = 355.06   Y0 = 240.54      vertical   FOV 68.1°
EYE = 143.94  (playerZ + 144 = 2 × playerHeight)
FY / FX = 1.1462
```

A square-pixel projection at the same horizontal field gives **75.4°** down,
about 7° too wide — which is what this port drew until 2026-10-01. That is not
cosmetic: every check here compares one of our frames against one of the
original's, so a vertical mismatch moves every comparison point to the wrong
row and reads as a lighting or a surface error when it is neither.

- The horizontal rule (`focal = view width × 0x7c/256`) is confirmed: it
  predicts 310.00 against the fitted 309.77.
- **There is no matching constant in the source for the vertical.** `0x8E`
  (which would give exactly 355) does not appear anywhere, and the aspect term
  at `render_target_buffer+0x1c` (`renderer.c:12075`) works out to 0.956, so it
  is not the vertical focal. Two plausible source leads, both dead. Use the
  measured 1.1462.
- **Do NOT use MAPINFO `pixelratio` for this.** It scales the view *transform*,
  and the VR code reads it to convert headset metres to world units
  (`gl_openvr.cpp:1087`, `:1098`) — setting it silently resizes the world in
  the headset. It is applied instead to the flat-screen *projection*,
  `r_view_vstretch` in `hw_entrypoint.cpp`, gated on `!IsVR()` and set per map
  by `rothmap.cpp`. In VR the lens sets the projection and none of it applies.
- Verified by rendering one pose before and after: vertical scale **1.1480**
  measured against 1.1462 asked for, horizontal **1.0000**. A first measurement
  said 1.193 and was wrong because it assumed the viewport was centred in the
  window; the centre is ~60 rows off and a wrong centre reads as a wrong scale.
  1.193 is close enough to the engine's old 1.2 default to be believed, so fit
  the centre as a free parameter.

**Flats are world-anchored**, Doom-style. Per surface: a scale, a shift, a
mirror.

```
col = ( -x / 2^s + shiftX/2 ) mod W
row = ( +y / 2^s - shiftY/2 ) mod H
```

- `2^s` world units per texel, `s` = 0..3. The project shipped `2^(s+1)` and
  argued for `2^(s-1)` and `2^(s+2)` before measuring it.
- **The two signs are `[derived]`, not read.** They are implemented so the test
  can check them; they have never been confirmed against ROTH.C's output. If
  flats are mirrored in-game, suspect these first.
- **256×256 AND opaque** takes `2^(s-1)`, and a shift unit becomes a whole
  texel. Branch at `renderer.c:3367` (`width == height && (uint8_t)width == 0`),
  then `span_textured_mode_flag`, which selects the opaque loop `0x3a220` over
  the translucent `0x3a100` (`renderer.c:3341`). That flag is **bit 26 of the
  texture block's +8 dword** (`renderer.c:13281`) = this pack's
  `IT_TRANSLUCENT`. 6,504 surfaces take this exception; 434 are mirrored.

**Mirrors** come from sector `+0x17`: floor bits 0–1, ceiling bits 2–3
(`renderer.c:9229`, `:9249`, VISIBLE pass).

**Mid-platforms** use the same formula, scale byte `+0x0C` (bits 2–3 underside,
4–5 top), and have **no mirrors** (bare `0x38`/`0xb8` at `renderer.c:9339`,
`:9354`).

**Walls, horizontal.** Measured with an instrumented run over STUDY1: bit
`0x100` of the draw flags — the unscaled path — was **clear on every span**
(0 stored-extent, 34 computed-extent). So every wall takes the computed path:

```
renderer.c:13347   extent_out = 2 * storedRowLength
renderer.c:4943    u = alongWall * extent_out / hfit
=> unitsPerTexel   = 2 * faceLength / hfit
```

HALF_PIXEL halves it (`renderer.c:13336` doubles the along-wall coordinate).
Most faces have `hfit == storedRowLength`, which collapses to the old fixed
`0.5` — which is why that wrong rule survived. One sampled setup had `hfit = 16`
against width 8 (true density 1 unit/texel, drawn at half size), and it was the
most-drawn of the sample.

**Wall vertical anchors** are explicit coordinates, not peg flags
(`wrap_reoff[0x0a]`):

| piece | anchor | source |
|---|---|---|
| upper | the sector's own ceiling | R:8664 |
| lower | the piece's own top edge (the step top) | R:8685 |

Doom's **default** bottom-part anchor is already the step top, so lowers need no
flag — `ML_DONTPEGBOTTOM` re-anchors them to the ceiling and misplaces them.
`FF_PIN_BOTTOM` is a different rule: the texture's bottom on the piece's bottom
edge (`renderer.c:5057-5063`), applied as an explicit offset.

**The three-way texture test** (`renderer.c:9222-9225`), which this port had as
two:

```
negative (0xFFxx)        -> a SOLID palette colour, index xx
== the colour key        -> DRAW NOTHING AT ALL
otherwise                -> textured
```

The colour key is the DAS header word at `+0x22` — the same word this codebase
called the "sky marker". **They are the same thing.** Audit over all 44 maps:

```
key floors      263      key ceilings   5,945      key flats  6,208
key wall pieces 33,255   EDGE_MAP faces 4,304
```

33,255 key *wall* pieces is what kills the "it means sky" reading. Sky is a 2D
band drawn only above EDGE_MAP walls; it is never a flat. Mapping key ceilings
to engine sky is a fair approximation — except in six maps with key ceilings and
no EDGE_MAP face anywhere: MAS3 (2), MAS4 (3), MAS7 (24), MAUSO1EB (2),
OPTEMP1 (1), SALVAT (1).

---

## 6. Lighting

**Already implemented and on.** `rothmap.cpp:~85-125`, model in
`ROTH_LIGHTING.md`:

```
off = light - 128
row = (depth >> shift) - (8 + off)     clamped 0..31
shift from lightAmbience: 0->5, 1->6, 2->7
```

Mapped onto GZDoom's BUILD light mode with `globvis` overridden to
`1/(1<<shift)` so the falloff *rate* matches. `roth_lighting 0` turns it off for
working on the level (applied at load; needs a map reload).

`pixelstretch` **is** consumed by the hardware renderer — in the **view** matrix
(`hw_drawinfo.cpp:3027, 3034-3035`), which scales world Y. An earlier claim that
it was unused came from checking only `GetProjection` and stopping.

### The missing dynamic term

`0x853f6` is a **global additive light offset**, not a dynamic light:

```
player.c:459-462       boost -= g_frame_time_scale       decays every frame
weapon_combat.c:752    boost = max(boost, weaponDef[+0x44])
entity_ai.c:1218       boost = max(boost, entityDef[+0x68])
```

added to sector ambient in the viewmodel (`collision_physics.c:1771`) **and in
world face shading** (`renderer.c:9187`). Every weapon shot and enemy attack
brightens the whole room, then fades. Per-def values, so each weapon and
creature lights differently. **We have none of this.** It is the owner's
"constantly changing" light.

---

## 7. Directional sprites — DONE 2026-09-28, except the view order

**Was:** "everything facing the same way", because directional art was dropped
entirely, not mis-faced.

**Now:** placed. `Pack::ReadDirectional` resolves the frame table and
`roth_objects.cpp` builds a real GZDoom rotation set. Evidence, both independent
of each other:

- **offline, all 44 maps** (`tools/rothdiff/dircheck.exe <ROTH folder>`): 26
  directional entries, all eight-view, 0 unresolved; 208 frames named, 208
  resolve to real pictures, 44 of them mirrored; 53 placed objects want them.
- **in engine, STUDY2**: `directional 17 placed from 136 view frames, 0
  unresolved`. 17 × 8 = 136, and 17 is what the offline audit predicted.

**Three things §7 originally got wrong, all found by reading the oracle:**

1. There are **three** frame-picking paths, not one, keyed off the DAS status
   byte: `0xfe` the per-map directional table (`renderer.c:5884`), `0xfc` the
   creature table (`:5844`), `<0xfc` the resident block's own table (`:5705`).
   Same 8/16-way rule and same mirror bit in all three.
2. The directional frame table is **its own DAS section**, not part of the FAT:
   header `+0x1c` file offset, `+0x1a` size (`map_load.c:367`, `:385-390`). The
   record is found by the FAT entry's **flags_2** byte, not its index
   (`das_assets.c:888` stamps it into the status word's high byte).
3. Frame ids are **global across both packs**: `id >= 0x1200` means the shared
   pack at `id - 0x1200` (`select_das_fat_entry`, `renderer.c:730-733`). Without
   this a fifth of the frames look like they point off the end of the world.
   Now `Pack::ResolveDasId`, which also fills a gap `roth_texture_object.cpp`
   had flagged in a comment and left unimplemented.

**Measured, so it need not be asked again:** Realms ships **no** sixteen-view
art — all 26 entries are eight-view. And of the 53 placed directional objects,
**none** also carries the fixed-angle flag or the x-flip, so those cases do not
arise.

**The view ORDER — SETTLED 2026-10-02; the derivation holds.** Realms picks its
frame with `((2*rot + 0x120 - viewAngle) >> 6) & 7`, a turn being 512 units, so
its offset is 202.5° — which is exactly GZDoom's own `45.0/2*9` rounding offset
(`hw_sprites.cpp:1436`). On that basis view *i* was mapped straight to rotation
*i*. That was a derivation, and §1 is blunt about derivations, so it was
measured rather than trusted.

**The two-engine look.** `ROTH_DEMO_O04358` in STUDY2 at (−1236, 2576) — DEMO
entry 4125, whose eight frames are all DIFFERENT and none mirrored, so the view
drawn is read off the picture instead of inferred. The camera orbits it at
radius 300 in 45° steps and both engines capture the same eight poses
(`captures/s2orbit.cfg` and `poses_study2_orbit.csv`; they must agree exactly).
`tools/rothdiff/viewmatch.py` scores each of the original's views against all
eight of ours on a mean-removed crop of the prop alone, because the backgrounds
legitimately differ.

**Result: the diagonal wins 8 of 8**, each time by a clear margin — 0.40, 0.49,
0.76, 0.71, 0.46, 0.72, 0.76, 0.74 against runners-up of 0.26 to 0.60. Offset +0
on every view. So view *i* → rotation *i*, and **`ANGLE_SENSE` is right as coded
(`+1`)**; the designer's `-1` would have broken the diagonal.

**The test could have failed**, which is the part worth keeping. This statue's
own yaw is not zero: it resolves to rotation 0 from a bearing of 110.7°, putting
its yaw near 270–313°, so negating the sense would have moved the chosen frame
by about five views. Across STUDY2's 17 directional props our engine picks
rotations 0, 1, 3, 5 and 7, so they genuinely face different ways.

**Seen while comparing, both separate open items and neither a sprite fault:**
the courtyard trees draw as green vertical streaks in ours where the original
has full canopies (§4's inconclusive tree A/B), and orbit views 0 and 7 show a
grey-blue smear where the original has a building — suspect the sky band
(§4 item 4) before blaming geometry.

### The original brief, kept for the rule

**The rule** (`renderer.c:5705-5727`) — note there are **two** directional
modes; `ROTH_SURFACES_FIX.md` §3.4 only documents the 8-way one:

```c
if (!(cw & 0x8000))   ci = table lookup;                              /* fixed */
else if (cw & 0x2000) t = rot*2 + 0x110; t -= viewAngle; t -= cdiv;
                      ci = (t >> 4) & 0x1e;                           /* 16-way */
else                  t = rot*2 + 0x120; t -= viewAngle; t -= cdiv;
                      ci = (t >> 5) & 0xe;                            /* 8-way  */

bw = block[ci + 0x12];
if (bw & 0x8000) { draw_flags ^= 2; idx = bw & 0x7fff; }   /* MIRROR this frame */
block += idx << 4;
```

The frame table at `block + 0x12` with a mirror bit is the **grouped block**
kind (`blk+0x0a & 0x40`), whose children `free_das_cache_entry` (0x41413) walks
from the same offset. Two views of one structure.

**The work — no engine change needed**, Doom does 8/16-way rotations with
per-rotation mirroring natively:

| file | change |
|---|---|
| `roth_das.{h,cpp}` | Follow `FAT_DIRECTIONAL`. Parse the `+0x12` frame table, resolve child sub-blocks, carry the `0x8000` mirror bit, expose 8-way vs 16-way from `cw & 0x2000`. |
| `roth_texture_object.cpp` | Register children as Doom sprite **rotations**. Today `:227-237` calls `TexAnim.AddSimpleAnim` — that is for animation frames, and rotations are not animation. |
| `roth_objects.cpp` | Place directional objects instead of skipping. Reserve `ObjectYaw` for genuinely fixed-angle ones. |

**Also unverified:** `ANGLE_SENSE = +1.0` (`roth_objects.cpp:102`). The designer
says it should be `-1` (`Doom angle = 90 - rot*360/256`). The file itself says
the sense is the first thing to distrust. It only affects fixed-angle objects.
Test on an asymmetric one at `rot = 64`.

---

## 8. The comparison tool — READ THIS BEFORE REBUILDING IT

> **USE `tools/oracle` (2026-10-01). Everything below it is the older rig.**
>
> `tools/oracle` runs ROTH.C headless, skips every GDV and answers the main menu
> with "new game" so it boots straight into a map — **no savegame needed**, and
> `mkgame.sh` builds a symlinked game dir whose `ROTH.RES` lists whichever map
> you want first. It paints each loaded DAS image with a 6-bit slice of
> `col | row<<8 | fatIndex<<16` across five passes, makes shade and remap tables
> identity, tags flat spans with sector and fill word, pins the camera, and
> dumps the indexed frame plus tags.
>
> It also **fits the rule rather than only scoring it**: `flatfit.py` derives
> ROTH's flat mapping — axis, sign, scale, offset — per sector from the pixels,
> and `walls.py` assigns wall pixels to faces by ray cast and fits the wall
> mapping. That answers *what the rule is*, which a match percentage cannot.
>
> Its own gotchas, found the hard way, are in `tools/oracle/README.md`: run
> captures one at a time (the host framebuffer `/roth_fb` is global and parallel
> runs corrupt each other); the paint hook must only paint blocks the call
> actually loaded; images wider than 256 lose column bit 8, so compare the low
> 8 bits; and the original sometimes nudges the player after a pose is set, so
> fit the camera rather than trusting the header.
>
> **Note the rig lives in a Linux workspace.** There are no captures on the
> Windows tree, and `capture.sh` / `mkgame.sh` are bash.
>
> `tools/rothdiff/` below still holds two things worth keeping: `findspots`,
> which chooses camera spots from the map data by property, and the saved-game
> handling — both are independent of which oracle you use. Its own identity
> buffer comparison is superseded: it compared *surface identity*, which cannot
> see a wrong scale, shift or mirror even when working.

**The older rig, for context.** Getting a number was the highest-value work in
the project, because without it every fix is a guess with a confident test
wrapped round it.

### What works

`tools/oraclelog/` and `tools/rothdiff/` instrument ROTH.C through its mod SDK.
Proven working:

- **Headless capture from a save.** F10 is quickload (scancode `0x44`, read from
  the game's own keymap table at `0x7093d`; F9 saves, Esc is the quit/menu
  prompt). The owner has a save in the Study.
- **Span-level logging** filtered to the visible pass (`0x90a48 == 0`).
- **The static comparison** — every sector and object, field for field, ROTH.C
  against our reader: **zero diff**.
- `flatcheck.exe` — offline flat comparison over all 44 maps. **But see §1: it
  cannot fail.**

### What does NOT work, and why

The per-pixel texel comparison. **Ten attempts on 2026-09-28, no result.**

The approach was: repaint every texture with a coordinate ramp so the
framebuffer *is* the (u,v) buffer — no arithmetic of ours in the path, immune to
the fills' self-modified shifts. **The technique itself works** — the world
visibly turns into ramps, confirmed on screen.

What defeated it:

- Painting at the span **driver** paints the *previous* surface's texture;
  `g_render_source_base_ptr` is re-resolved per texture inside the span path
  (`renderer.c:13332`). Painting at the **fill** is correct.
- Captures kept landing on the frame *before* the paint took effect, or on a
  black fade after the quickload.
- Walking the DAS cache to paint everything up front **crashes the game** when a
  menu draws. Menu art is a different block kind and writing `width*height`
  bytes into it corrupts memory. Skipping `blk+0x0a & 0x40` was not enough.

**Do not simply resume this.** Writing into the game's texture memory using
dimension fields that are only valid for some block kinds is unsafe by
construction.

**The safe alternative, not yet built:** a read-only comparison. Record per span,
from both engines, the texture id and the scale/shift/mirror actually used, and
diff those. Per-surface rather than per-pixel, cannot crash, and it still catches
scale, shift and mirror — which is the entire live problem.

### Useful addresses

```
0x90a48  subpass kind          0 = visible pass; 0xff / 1..8 = cursor pick
0x8528c  current surface record  (0x90a42 is PICK-ONLY -- do not use)
0x84f2e  span fill word        = span record +0x16; also the draw flags
0x9093c  g_world_surface_draw_flags   0x20 -> sprite/flat driver, 0x200 -> mesh
0x90978  texture width (valid only after the texture is resolved)
0x9097c  +0x0c height, +0x10 the scale nibble
0x84980  g_render_source_base_ptr   pixel data; re-resolved per texture
0x86d30  DAS status table, 0x1600 x 2 bytes; low byte < 0xfc = loaded
0x89930  DAS cache slots, 6 bytes each; first dword = pool handle
         blk+0x0a kind (0x40 = grouped), +0x0c width, +0x0e height, +0x10 pixels
0x90bcc  tick counter (70 Hz)
0x90a8c / 0x90a90 / 0x90a94   player X / height / Y, 16.16
```

---

## 9. Practical notes that cost hours

- **`-rothpath` must point at the `ROTH` subfolder**, not the install root. And
  PowerShell's `Start-Process -ArgumentList` does not re-quote array elements
  containing spaces — pass the whole command line as **one string** or the path
  silently splits and no maps are found.
- **`-stdout` sets `batchrun`**, which takes an early `norun` exit. Use
  `+logfile` or a `logfile` line as the first line of an `+exec` script.
- **`+exec` runs before `map`** — `map` is deferred, `exec` is not. A `quit` in
  an exec script fires at startup. `roth_diff.cpp` queues console-requested
  captures until a level exists, drained from `P_Ticker`.
- **LNK1103 "debugging information corrupt" = DELETE `doomxr.iobj`.**

  ```
  del build-dxr\src\zdoom.dir\RelWithDebInfo\doomxr.iobj
  del build-dxr\src\zdoom.dir\RelWithDebInfo\doomxr.ipdb
  ```

  That is the INCREMENTAL LTCG CACHE, and when it goes stale the linker rejects
  a freshly compiled object. The tell is in the build log, just above the error:

  ```
  1 of 123325 functions (<0.1%) were compiled, the rest were copied from
  previous compilation.
  ```

  It is 1.45 GB, it lives in `zdoom.dir` beside the objects rather than in the
  output directory, and **it survives a "clean" that only removes `*.obj`,
  `*.pdb` and `*.idb`** — which is why this note was wrong twice and cost most
  of 2026-10-01. Earlier editions blamed, in order: leftover `doomxr.iobj` in
  the *output* directory (right file, wrong directory), a parallel-build PDB
  race, and a bad code construct in one translation unit. The last of those came
  from `rothmap.cpp` appearing to be fixed by rewriting a `FindCVar` call;
  almost certainly that edit just perturbed enough code to invalidate the cache.

  A serial build (no `-m`) is still worth having for legibility, but it is not
  the fix.

- **DO NOT DELETE `doomxr.iobj` PROPHYLACTICALLY.** Deleting it forces a full
  LTCG pass — the log says so outright: *"All 123333 functions were compiled
  because no usable IPDB/IOBJ from previous compilation was found."* That is
  about **20 minutes of linking** on this machine against a couple of minutes
  incrementally. On 2026-10-01 it was deleted before every build "to be safe"
  and cost most of an evening. Delete it when LNK1103 actually appears, and not
  before.

- **NEVER RUN TWO BUILDS AT ONCE.** They fight over the build directory and both
  die partway, with the log ending mid-library and no error line anywhere — the
  failure looks like a mystery rather than a collision. Before starting a build,
  check nothing is running:

  ```
  Get-Process cmake,MSBuild,cl,link -ErrorAction SilentlyContinue
  ```

  and prefer ONE invocation left completely alone until it finishes. Killing a
  build mid-LTCG also orphans a `link.exe` that keeps running for a while
  afterwards and will be mistaken for the next build making progress.

- **A build log redirected to a fixed filename can silently fail to open** if a
  previous build still holds the handle — the batch then reports an exit code
  for a command that never ran, while the exe sits untouched at its old
  timestamp. Use a per-run log name, and confirm progress by the TIMESTAMP ON
  THE EXE rather than by an exit code.

- **The Debug output directory has no DLLs.** `openvr_api.dll`,
  `openxr_loader.dll`, `zmusic.dll`, `OpenAL32.dll` only sit beside
  RelWithDebInfo. Build RelWithDebInfo, or copy them.
- **`vrealms.pk3` goes stale.** It is built from `vrealms_iwad/`; rebuild it
  (it is a plain zip) or you will test yesterday's package against today's code.
- **Do not use shell heredocs to write or patch files** in this environment.
  They mangle backslashes (a `\n` in a C string silently became a real newline
  and broke a format string, costing a cleared launch) and break on quotes. Use
  the Write/Edit tools.
- **OpenXR initialises even with `vr_mode 0`** — the loader just loads the DLL
  (`oxr_loader.cpp:58`). There is no `-novr`. Screenshots taken in VR capture
  the stereo composition, not a flat view.
- **`roth_<MAP>.log` is truncated every time a map loads.** It is opened
  `fopen(path, "w")` (`roth_log.cpp:39`), so a SECOND load of the same map wipes
  the complete report the first one wrote. On 2026-09-28 this read as "the
  loader dies after the Source section" — a 227-byte file — when in fact 8,158
  bytes had been written and then wiped. If you poll that file, poll fast and
  **keep the largest version you see**; do not trust the version that is there
  when the process ends. The report writer already flushes per line, so a short
  file means truncation, not a crash.
- **Most maps are not in MAPINFO.** Only `STUDY1` was declared; `map STUDY2`
  silently has nothing to load until an entry exists. Every map loads through
  the same path, so an entry is all it takes. STUDY2 is now declared because it
  carries 17 directional objects, the most in the game.
- **A launch needs `-iwad vrealms.pk3`** or it stops at "Cannot find a game
  IWAD" before anything else happens.

---

## 10. THE ORDER OF WORK

**This section is the priority order. §4 is an unordered inventory of
everything outstanding — do not read its numbering as a plan.**

Done, and struck out rather than deleted so nobody re-opens them:

- ~~Directional sprites~~ — 2026-09-28 (§7), bar the view order.
- ~~The comparison rig~~ — `tools/oracle`, 2026-10-01 (§8).
- ~~Paletted rendering and the shade table~~ — 2026-10-01, `roth_palshade`.
- ~~The lighting model~~ — 2026-10-01, verified 2943/2943 vertices.
- ~~The vertical projection~~ — 2026-10-01 (§5).
- ~~Colour-key walls~~ — 2026-10-01. 1,778 wall pieces across the 44 maps that
  drew artwork over the original's openings now draw nothing.
- ~~The sky~~ — 2026-10-01. A world-locked cylinder, `r_skyband`. The zenith
  disc is gone and the drift measures 0.88-0.94x world-locked. NOT yet compared
  against an oracle frame, so the texture's orientation and the exact vertical
  placement are unconfirmed.

Outstanding, in the order to do them:

1. **Sprite size and vertical placement** (`ROTH_SURFACES_FIX.md` §3.4). The
   owner's "those pillars are too tall". Measure against the oracle before
   implementing — the documented rule, applied as written, sinks 68 of STUDY1's
   props through the floor they are standing on.

   - ~~the nibble shift~~ — done (`8bc39fa`). The burial is real in the
     original; both alternatives are plainly worse.
   - ~~the art entry's own anchor~~ — done. This was the "0x84aba second term",
     and it is not a mystery global: every write to it is a 32-bit store to
     `0x84ab8` of `dword[block+4]`, and that dword is the four bytes sitting
     **immediately before the entry in the file**, present only when flags_1
     bit 3 is set (`das_assets.c:934-944` seeks to `fat.offset - 4` for exactly
     those). The payload still lands at `fat.offset`, so the picture reader was
     never affected by any of this. See `roth::AnchorOffset`.

     The lift calls the dword a "size prefix"; that is a misnomer taken from
     the read's shape rather than its use. **MEASURED: 0 of the 321 prefixed
     entries hold the FAT size, size + 4, or width × height.** It is a pair of
     signed words used for two different axes two instructions apart — the low
     one a lateral slide in VIEW space, the high one the vertical term. Only
     the shared pack carries them: 321 of its 778 entries, and **none at all in
     DEMO, DEMO1, DEMO2, DEMO3 or DEMO4**, which is why omitting it looked
     harmless for so long. Barely any *placed* object reaches one (1 of 4,973);
     they arrive through the directional frame tables instead.
   - **THE PILLARS ARE NOT TOO TALL, AND NOTHING EVER MEASURED THAT THEY WERE.**
     See §13. `DEMO[4123]` and `DEMO[4128]` are `IT_OBJECT_DATA` meshes, the
     mesh scale chain has a gain of **exactly 1.000000**, and **542 is their
     true rendered height**. The "3.5x" came from this port's own warning at
     `roth_objects.cpp:888` — `if (ey > 3.f * 154.f && log)` — which fires on
     anything over 462 units and logs "542 tall"; `542 / 154 = 3.519`. A
     warning fired and was read as a measurement. **Close this as a scale
     question.** Whatever the owner saw is something else, and §13 says where
     to look.
   - **a per-view lateral anchor** is missing, and it is a structural gap rather
     than an oversight. A directional object gets ONE `SpriteInfo` — view 0's —
     but each view is a different art entry with its own anchor and its own
     mirror bit, which the original negates and applies per view. MEASURED over
     all 44 maps (`tools/rothdiff/prefixcheck.cpp`), of the 9 directional
     entries whose every frame carries an anchor: the **vertical** anchor is
     identical across views for 9 of 9, so taking it from view 0 is sound; the
     **lateral** anchor differs across views for **8 of 9**; and all 9 mirror
     some views and not others. So the lateral half is applied on the plain path
     and counted-but-skipped on the directional one. Closing it means carrying a
     per-view offset on the sprite definition beside the per-view flip already
     there — a real change, deliberately not guessed at.

     **NEEDS NO CODE — it is inert in every retail map. Measured 2026-10-02,
     the same way the tint ramp closed (`dbec850`).** All ten maps carrying
     directional objects report `0 applied, 0 with one on ANY view, 0 whose
     views DISAGREE`.

     **The first zero was not trustworthy and was fixed before being believed.**
     The counter tested `info.lateralOffset`, which is *view 0's*, so an entry
     whose view 0 is zero and whose other views are not would have read as
     "nothing here" — and 59 of the 321 prefixes carry `x = 0`. The view loop
     already fetched `frameInfo` for every view and discarded all but the first,
     so it now tracks whether ANY view carries one and whether the views
     DISAGREE. Still zero everywhere.

     **Why it is zero, which is the part that makes it safe to close.** The 9
     prefixed directional entries are all in the SHARED pack, and **no retail
     map places them**: every placed directional prop is map-pack art, which
     carries no prefixes at all (`prefixcheck`: none in DEMO, DEMO1, DEMO2,
     DEMO3 or DEMO4). Confirmed on the art itself in two maps — STUDY2's 17 are
     `ROTH_DEMO_*`, MAZE's 8 are `ROTH_DEMO4_*`. The structural change would be
     dead code against the retail data.

     Build it only if something ever places a shared-pack directional entry. The
     counter will say so.
2. **The remaining lighting terms.** Re-derived 2026-10-01 with three readers
   and a skeptic each; all four survived, against none on the first attempt.

   - ~~object light bytes~~ — done (`06ef8ef`). A SIGNED OFFSET on the sector's
     light, 0x80 neutral, no radius and no distance term. 231 of STUDY1's
     objects carry one.
   - ~~the glow table~~ — done (`d9f7a79`). `screen = glow_table[texel]`: no depth, no
     sector light, no flash. A flat palette remap that ignores lighting
     entirely. Gated on sector flags byte +0x0a bit 6 AND `(phase & 0x49) != 0`
     where phase is the byte at `0x8a355`. The table is 256 bytes at
     `palOff + 0x14402` in the map DAS. **618 sectors carry the bit, 335 of them
     in STUDY2** — it is the storm in the courtyard. The phase is stepped at a
     **fixed 35 Hz**, not once per rendered frame, so the flicker does not change
     speed with the framerate. NOT YET SEEN ON SCREEN: it builds and the data is
     measured, but no launch has confirmed the flicker looks right.
     Touches `roth_palshade` / `func_roth.fp` / `hw_drawinfo`.
   - ~~tint ramp selection~~ — **NO CODE NEEDED, measured 2026-10-01.** Map
     metadata `+0x16` is not a ramp index, it is a per-map boolean deciding
     whether the tint ramp exists at all, and when it is zero the engine aliases
     the tint pointer back onto the WORLD ramp. **It is zero on all 44 retail
     maps** (checked directly, not inferred), so rows 32-62 are never read by
     the world renderer in the shipping game. What the sector's bit 1 actually
     does — swap the shade-pair index from metadata `+0x10` to `+0x14`, and
     perspective-correct mapping — is the lantern/candle path we already have.
   - **muzzle flash** — rule is solid (decays 120 units/second, raised by MAX
     not ADD from three sites, added 8-bit WRAPPING to the sector light only
     when that light is non-zero). **BLOCKED, not pending**: it is raised only
     by weapon fire and creature attacks, so nothing can move it until the game
     layer exists. Do not schedule it before then.
3. ~~**Doors**~~ — **THEY OPEN, 2026-10-01 (`537b2928`).** STUDY1: 30 of 30
   leaves built, hinged and swinging, `roth_door: 30 opened, 0 refused`.
   Polyobjects were the right host after all; see §12 for the settled rule and
   the three defects fixed. What is left on doors is narrow:

   - **the swing DIRECTION is unverified.** `roth_door` calls `SwingDoor` with
     no player context and GZDoom's `PODOOR_SWING` uses a fixed `swingdir`,
     while the original decides the mirror from which room the player used the
     door from (`doors.c:525`). The rule is derived (§12) but **not wired into
     `OpenDoor`**. Nothing will exercise it properly until a trigger opens a
     door with a player standing somewhere.
   - ~~the swung leaf's visible face reads flat and pale~~ — **DONE, 2026-10-02
     (`09c664cf`).** It was an unskinned thickness edge, exactly as suspected,
     and the §12 question it was tied to is settled with it. The two thin sides
     were blank while carrying artwork in the files (166/167 and 162/167), which
     is 10.4% of every door's surface drawing nothing. Seen on screen, closed /
     mid-swing / open.
   - closing, blocking and the re-close on a second use are untested: nothing
     has ever closed one either.
   - **NEW, and not a door bug**: the mesh `DEMO[4123]`, ~1700 units beyond
     STUDY1's door 9, **draws in front of the closed leaf** and over the
     swinging one. Found by the mid-swing control, not by looking for it. It is
     the same object at the same screen position in all three frames, which is
     what first made it look like a door texture failing to move. **Whether
     meshes also draw through ORDINARY walls has not been tested**, and that
     test decides whether this is a general depth fault in the mesh path or
     something specific to the polyobject leaf. See §13.
4. **Level logic handlers**, then **the game layer** (`ROTH_GAME_PORT.md`).

   `ROTH_GAME_PORT.md` §1 says "fix before anything else: the trigger wiring is
   wrong in code". **That is stale — checked 2026-10-01 and the fix landed in
   2026-09**, before this item was ever reached. The code now binds 0x18 / 0x1a
   / 0x32 as face-keyed and 0x19 / 0x31 as sector-keyed, with 0x13 as the third
   kind, which is exactly what the spec's own "what it really is" column asks
   for; only its "runtime currently treats it as" column is out of date. A
   status banner is now on that section. **So nothing blocks starting here.**

   The real remaining gap on triggers is narrower and is already written down in
   `roth_runtime.cpp` above `IsFaceTrigger`: the **direction mask and bounding
   box** on the face channel are not implemented, because they live in
   object-table refs this port does not build yet. A face trigger therefore
   fires from any approach rather than only the authored one — a known
   over-fire, reported at load rather than hidden.

   **HOW FAR THE LEVEL LOGIC ACTUALLY IS, counted over all 44 retail maps
   (`tools/rothdiff/opcodecensus.cpp`, no launch needed).** 5,531 command
   records in the shipping game. 1,877 are trigger slots whose instruction is
   the nop in the original too, and 66 are reserved nops, so **3,588 records
   have to do something — and 2,250 of them already run: 62.7%.** The dispatcher
   handles 26 opcodes; 1,338 records across 21 opcodes fall through.

   Those 21 are not scattered. Grouped by the system each belongs to in
   `GAME_core.md` §5.3, **two systems are 70% of everything missing**:

   | System | Opcodes | Records | What it is |
   |---|---|---|---|
   | **Actors / spawning** | 0x3c (256), 0x16 (153), 0x3a (72) | **481** | `cmd_spawn_object_adv`, `cmd_spawn_object`, `cmd_change_object_id` |
   | **Inventory** | 0x29 (239), 0x27 (161), 0x2a (56), 0x42 (2) | **458** | `cmd_give_item`, `cmd_if_not_item`, `cmd_remove_item`, the display filter |
   | Ambient sound | 0x10 (94) | 94 | `cmd_activate_sfx_node` |
   | Particles | 0x2d (76) | 76 | `cmd_particle_effect` |
   | Loop counters | 0x15 (37), 0x1e (42), 0x1f (25), 0x22 (1) | 105 | the `cmd_count` family — pure flow, no new subsystem |
   | Player effects | 0x33 (44), 0x3f (19), 0x35 (15), 0x41 (3) | 81 | damage, forced rotation, hazard walls, slowdown |
   | Moving sectors | 0x09 (32) | 32 | `cmd_move_sector` — moves a sector's VERTICES, unlike 0x07 |
   | Texture odds | 0x2e (9), 0x1c (1), 0x20 (1) | 11 | smash / cycle variants |

   **So the honest answer to "when can the game go in" is: now, and the first
   two things to build are inventory and actor spawning.** Neither is loader
   work — both are squarely the game layer, which is what this item is. The
   `cmd_count` family is worth doing early and cheaply: 105 records of pure
   control flow that needs no new subsystem at all.

   Per map, the least ready are DOPPLE 41.9%, VICAR 42.2%, CAVERNS 42.9%; the
   most are CAVERNS2 and ABAGATE2 at 100%. **CHURCH1 is the one that matters**:
   451 records, 260 of them live, and 120 unhandled — the largest single
   concentration of missing logic in the game.

   Caveat on the numbers: the census mirrors `IsImplemented` / `IsVerifiedNop`
   by hand, because they are in an anonymous namespace in an engine translation
   unit. The tool prints both tables every run so drift shows up instead of
   lying; if you change the dispatcher, change them.

**Before building any of 1-3, re-derive the rule.** All three were extracted
from ROTH.C on 2026-10-01 and all three were REFUTED by an independent reader:
the sprite spec had the two face-list passes exactly backwards (the §2 trap),
the lighting spec parsed the Sector record at a 24-byte stride when it is 26,
and the doors spec had the hinge corner ordering inverted. Treat any numbers
circulating on those three as unsafe.

---

## 11. Working with the owner

- They are frequently in a VR headset. **Clear every launch.** Say what will
  appear on screen and roughly for how long.
- They will tell you plainly when something looks wrong, and they are reliably
  right about *that something is wrong* — trust the observation, verify the
  cause.
- **Do not report a fix without evidence.** "Parser counts alone don't count."
- When a rule is contested, do not argue it — measure it. That has resolved
  every dispute in this project, usually in one run.

---

## 12. DOORS — the settled rule, 2026-10-01

Re-derived by three independent readings, each attacked by its own skeptic.
**All six agree on the corner ordering**, which is the detail that sank the
previous attempt. Confidence: high.

**CAVEAT ON LINE NUMBERS.** All three skeptics made the same complaint: a number
of citations land roughly 11 lines early, inside a function's prose header
comment rather than its code. The *rules* survived independent re-derivation;
the *line numbers* below should be confirmed by eye before being quoted onward.

### The corner ordering — FORWARD, not backward

`pk = 0x24180c00` (`doors.c:491`). Its bytes from the least significant are
`0x00, 0x0c, 0x18, 0x24`, and the Face stride is `0x0C`, so those are face
slots 0/1/2/3 of the sector.

The previous attempt's "walking BACKWARD (ror)" was inverted, and the diagnosis
is specific: **the hinge SEARCH loop and the corner STORE loop use different
rotations, and the search's was mistaken for the store's.**

- Search (`doors.c:500`) uses `rol32` — byte3 into byte0, i.e. slot −1, so it
  probes 0, 3, 2, 1: descending. It `break`s on a hit **before** the rol, so on
  exit `pk & 0xff` is the hinge's own offset. That positioning is what makes the
  store loop work.
- Store (`doors.c:511`) uses `ror32` — byte1 into byte0, i.e. slot +1, so the
  low byte ascends.

**With the hinge at sector face slot `h`: `c_k = slot (h + k) & 3`.** `c0` is the
hinge face; `c1` and `c3` are the two broad faces; `c2` is the far thickness
edge. Independent corroboration from the consumer: the mirror decision
(`doors.c:525`) compares the sector reached through `c3` against the room the
player used the door from, which is the "swing away from the player" rule only
under forward ordering.

### The motion

A **pure 2D rotation about the hinge vertex**. No translation, no easing, and a
single byte of state at `rec+0x03`.

- `g_sincos_table` is 512 × int16 Q14 — settled by reading its actual bytes
  (`tab[128] = 0x4000 = 16384`, `obj3_owned.c:786-800`), which makes the lift's
  swapped sin/cos *names* irrelevant. **Implement from the indices:** `tab[i]`
  is sine, `tab[(i + 0x80) & 0x1ff]` cosine.
- `rotate_quad` gives exactly `out = pivot + [[cos,−sin],[sin,cos]] · (px,py)`.
- **1 angle unit = 1.40625° exactly. Full travel = 64 units = 90°**, so the
  `DAngle::fromDeg(90.)` already in `roth_runtime.cpp:394` is correct.
- **Time base is 70 Hz (70.31), not 120.** The lift's "120 Hz divisor" comment
  at `dos_runtime.c:277-280` is arithmetically self-refuting: `1000/3863 =
  0.2589 = 18.2/70.3`, not `18.2/120 = 0.1517`. Corroborated by the driver timer
  registered at rate `0x46` = 70 (`audio.c:1835-1842`) and by `GAME_core.md:18`.
- Speed: `step = dt × M`, or `min(dt, 8)` when `M == 0`. **`M == 0` is
  equivalent to `M == 1`** above 8.75 fps — a per-frame overshoot cap, *not*
  `M = 8`. Full open at `M=1` is 64 ticks = 0.914 s.
- `dt` is whole ISR ticks since the previous rendered frame, clamped to 45, with
  **no lower clamp** — `dt` can be 0 and then no door moves that frame.

### What moves

**Nothing in the shared map.** The slab is a four-point quad living inside the
door-pool record. `rotate_quad` rewrites only the world-space corner array at
`[rec+0x2e]+0x82`. The single map write is bit 0 of each adjoining sector's
`+0x16` flags byte, and only at open/close boundaries — it must be kept in
lockstep with the record's existence, including re-setting it on every survivor
after the pool compacts.

### The command record — both earlier hypotheses were wrong

| Field | What it actually is |
|---|---|
| `+0x07` byte | **the swing speed multiplier `M`**, angle units per 70 Hz tick |
| `+0x08` word | the resolve key: a FACE id; 0 = "the wall the player just used" |
| `+0x0a` word | **the open dwell reload `A`**; the stored value is `(uint16)(A × M)` when `M != 0` |
| `+0x0e` / `+0x10` | **not a target point.** Stashed by `register_door_swing` for other use |

### The skin — PARTLY OPEN, do not guess

Verified by reading `doors.c:553-559`: there are exactly four surface
sub-structs, and **all four are built from `c1` and `c3` only**, each twice —
`_a(ebx=c3, edx=c2)`, `_a(ebx=c1, edx=c0)`, `_b(ebx=c3)`, `_b(ebx=c1)`. In `_a`,
`ebx` is the texture source and `edx` only contributes a stored extent, so
**`c0` and `c2` are never a texture source.** Each broad face carrying two skins
(its own and its sister's) is what "two-sided" actually meant.

Also verified, from the binary: `g_door_vertex_template` + its body
(`obj3_owned.c:866-872`) is 20 × u16 = **four quads of five indices**, closing
cleanly, referencing vertex slots `0x00-0x30` **and** `0x40-0x70`. So array B is
the slab's second ring and all four edges exist as geometry — and array B is not
"never read" as one reading claimed.

### Which surface lands on which quad — SETTLED, 2026-10-02

This was the last open point in the door work and the single thing the three
readings disagreed on. It was stuck because it was being attacked as a
list-matching problem: four surfaces, four quads, both in a fixed order, so any
pairing looks as good as any other and all of them are inferences.

It is not a list-matching problem. **Read what each argument is used for**
(`setup_door_corner_surface_a`/`_b`, doors.c:281-297):

```
_a(ebx, edx):   [out+0x26] = fs:[ fs:[edx+4] ] & 0xfff
                [out+0x0c] = fs:[ fs:[ebx+4] + 2 ]
_b(ebx):        bx = fs:[ebx+8]  FIRST, then both of the above from there
```

`+0x00` of a mapping record is the fit word, so `& 0xfff` is the **stored
extent** — and it comes from `edx`. `+0x02` is the mid texture, so the
**picture** comes from `ebx`. `_b`'s `+8` is `sisterFaceOffset`, so `_b` reads
the **sister's** mapping for both. The call order is `_a(c3,c2)`, `_a(c1,c0)`,
`_b(c3)`, `_b(c1)` — doors.c:553-559.

**The extent is the quad's width, so the extent argument names the quad.** And
which corners are the narrow ones is now measured rather than assumed
(`tools/rothdiff/doorgeom.cpp`, all 167 leaves in the retail maps):

```
j == 0  (thickness)  mean  13.9   min  2.0   max 64.0
j == 1  (broad)      mean 124.2   min 42.0   max 192.0
j == 2  (thickness)  mean  14.0   min  2.0   max 69.9
j == 3  (broad)      mean 124.1   min 42.0   max 192.0

long pair is {1,3}  167 / 167  (100.0%)   median aspect 10.67
```

So:

| quad | width from | picture from |
|---|---|---|
| c0 — hinge thickness edge | c0 | **c1's own** mapping |
| c2 — latch thickness edge | c2 | **c3's own** mapping |
| c1 — broad face | c1's sister | c1's **sister** |
| c3 — broad face | c3's sister | c3's **sister** |

Two consequences beyond the table:

- **The `_b` (sister) surface is the outward one**, which this section had
  carried as UNVERIFIED. `_b` is the form whose extent comes from the broad
  face, and the broad face is what a room sees. The broad faces were already
  skinned correctly.
- **"Each broad face carries two skins" was wrong.** Each broad face carries
  one; the second `_a` surface per side is the *thickness edge beside it*.

Note what the measurement does NOT settle: it confirms the broad faces sit at
the ODD slots, which is true under either traversal direction, because the hinge
is at a short edge and short/long alternate around the ring. The `ror`-vs-`rol`
direction is settled separately, above, and decides only which broad face is c1
and which is c3 — i.e. which way round the door's front and back art go. For the
70.7% of leaves where `skin(c1) == skin(c3)` that is invisible; for the other
29.3% it is not, and it has not been seen on screen.

### What was fixed on 2026-10-02

**The thickness edges were blank and should never have been.** They were left
untextured on the reading that c0 and c2 "never contribute a texture" — true,
but they *receive* one, from the broad face that follows them in the ring. The
cost of the error, measured before fixing it:

```
painted (j 1,3)                         12769696 sq units
blanked but textured in file (0,2)       1330659 sq units   (10.4% of painted)
has a mid texture in the file:  j0  166/167     j2  162/167
```

So 10.4% of every door's surface drew nothing, on sides that have artwork in the
player's own files — and the latch edge is exactly what faces you when a door
stands 90° open, which is where the owner spotted it ("the texture looks bad").
`leavesNoTexture` was also renamed `leafSidesNoTexture`, because it counts sides
and can now count up to four per leaf.

### Mapping it onto GZDoom — the conversion is exactly 16

No engine change. `EV_OpenPolyDoor`'s swing branch already does

```
m_Speed = speed * (90./64) / 8      // degrees per 35 Hz tic, po_man.cpp:674
```

and **`90/64 = 1.40625` is exactly Realms' degrees per angle unit** — Hexen's
polyobject doors inherited the same 64-step quarter turn from the same era, so
the two engines already agree on the unit. Equating the rates:

```
Realms   M * 1.40625 * 70        = 98.4375 * M   deg/s
GZDoom   speed * 1.40625 / 8 * 35 = 6.15234 * speed deg/s
=>       speed = 16 * M
```

Check: `M = 1` gives `m_Speed` = 2.8125 deg/tic, so 90° takes 32 tics = 0.914 s,
against the original's 64 ticks at 70 Hz = 0.914 s. **Exact, not fitted.**

**Dwell is `A` ticks whatever the speed.** The reload is `A × M`, but the counter
is decremented by the same step as the swing — `M` per tick — so it runs for
`A × M / M = A` ticks and does *not* scale with speed. `A` is in 70 Hz ticks and
`m_WaitTics` is in 35 Hz tics, so `delay = A / 2`. A first pass of this code
scaled the dwell by `M`, which would have made fast doors wait proportionally
longer; the error is only visible by reading both engines' units side by side.

### What was fixed on 2026-10-01

All loader/runtime side, no engine change:

1. **`rothmap.cpp` corner/line pairing.** It stored `face.vertex2` and then
   paired vertex `j` with `j+1`, which reads like "the line reversed" and is
   not: with the ring closing as `v2(c_k) == v1(c_(k+1))`, line `j` spanned
   `v1(c_(j+1)) -> v1(c_(j+2))` — face `c_(j+1)` traversed **forward**. One
   shift, two bugs: the one-sided fronts faced INTO the slab (so from outside
   the player saw backs with no sidedef), and the `j == 1 || j == 3` skin
   painted the two THICKNESS edges and left the broad faces blank. Now `P[k]`
   is `c_k`'s own `vertex1` and line `j` runs `P[j+1] -> P[j]`.
2. **The "flat panel, not a box" comment**, refuted by the binary's own
   template. Replaced with what is verified and what is open.
3. **`roth_runtime.cpp` speed field.** It read the speed from `args[3]` (the
   command's `+0x0c`) while reading the dwell multiplier from `+0x07` — one
   value cannot live at two offsets. Both are `+0x07`. The `M == 0` case also
   used `8.0`, eight times too fast.

**NONE OF THIS HAS BEEN SEEN.** It builds; no launch has confirmed a door is
visible, let alone that one swings the right way. The first run should check, in
this order: are all 25 of STUDY1's leaves present and solid; do the broad faces
carry a texture; does the slab swing away from the player; does it take about
0.9 s at `M = 1`.

---

## 13. MESHES — the scale is 1.0, and the "too tall" premise is dead

Derived 2026-10-01 by three independent readings, each attacked by its own
skeptic. All three survived, and — the trap that was set for exactly this — none
of them reasoned backwards from the 3.5x symptom. Confidence: high.

### The scale chain: gain exactly 1.000000

**One stored int16 = one world unit = one map unit.** There is ONE shift in the
whole path (`>> 14`, horizontal only) and it is exactly unity. No `>>2`, no
`>>8`, no 256-grid factor, no missing or double shift.

```
vx = (int16) u16[v+0x00]   vup = (int16) u16[v+0x02]   vy = (int16) u16[v+0x04]
ang = g_sprite_view_angle - 2 * (u8)record[+0x06]
rx  = (vx*cos - vy*sin) >> 14      ry = (vx*sin + vy*cos) >> 14
```

The unity was **proven twice, by measurement rather than inference**: the
512-entry `g_sincos_table` was extracted and computed — `T[128] = +16384`,
`T[384] = -16384`, amplitude exactly `0x4000 = 2^14`, max deviation from
`round(16384*sin(2*pi*i/512))` across all 512 entries = 1 — so
`(unit * 2^14) >> 14 = unit`. Independently, `floorceil_rotation_sincos`
early-returns the point *unchanged* at angle 0 while its body uses the same
table and the same `>>14`, which forces `T[0] = 0` and `T[0x80] = 0x4000`.

Corroborated from the other side: `rwss_sprite_side_entry` has to feed a mesh
vertex into the *wall* projector and converts by `<< 8` on lateral and depth and
nothing at all on height (`renderer.c:6597-6602`) — mesh view coords are x1,
wall coords are x256. The port's existing "one mesh unit is one world unit"
note was right all along.

### So where did "3.5x too tall" come from

**From this port's own log line, not from any measurement.**
`roth_objects.cpp:888` is `if (ey > 3.f * 154.f && log)`, under a comment
reasoning that Realms' player is 154 units so a prop over three times that is
"either genuinely architectural or a scale we have got wrong". The threshold is
462. A 542-unit mesh trips it and logs `542 tall`. And `542 / 154 = 3.519`.

A session reading that log could manufacture "3.5x too tall" out of the prop's
own ratio against the threshold's divisor. **Nothing measured an error — a
warning fired.** Treat that log line as a prompt to measure, never as a finding.

### What the two entries actually are

`DEMO[4123]` and `DEMO[4128]` are **six-blade radial fans**, byte-identical in
geometry, differing ONLY in texture ids. 14 vertices = 7 `(x,y)` positions each
paired at `up = 0` and `up = 542`: a hub at `(0,2)` and six outer points at
radius ~230 and bearings 0.5°, 65.4°, 121.9°, 179.5°, 245.9°, 306.9°. All six
blades share the two axis vertices. `min(up) = 0`, so **the model stands on its
origin**. 12 faces in 6 complementary pairs (`f0 = [0,1,2,3]`, `f1 = [1,0,3,2]`
— the same quad pre-reversed), and the engine draws exactly 6 of the 12 from any
viewpoint because faces are **one-sided, backface-culled in screen space, and
depth-sorted painter-style**. Footprint 460 x 420 x 542.

### The mesh path is bigger than anyone assumed

MEASURED (`tools/rothdiff/meshcheck.cpp`): **935 of the 4,973 placed objects
across all 44 maps are meshes — 19%**, spread over 33 distinct entries. 47 mesh
entries exist in the five map packs and **none at all in the shared pack**.
STUDY1 places one, `DEMO[4109]` at `(816, 264, 400)`, so meshes can be checked
in the same map as the doors.

### A port bug that is real but UNREACHABLE

`DEMO.DAS[4097]` (56 verts, 47 faces) is the only entry in all five packs tagged
**`EXPL`** rather than `EXP2`. The EXPL variant stores a face's texture id as a
zero-extended BYTE at `+0x0c`, where the normal kind stores a byteswapped word
(`renderer.c:817-821`). `roth_das.cpp:494` reads `RdU16BE(+0x0C)`
unconditionally, so for an EXPL mesh every face would resolve to a huge id,
which `MeshFace` treats as a flat colour — all 47 faces painted solid.

**It is placed in NONE of the 44 retail maps** (measured, not assumed). So the
bug cannot reach the screen and does not need fixing now. Do not spend time on
it; do add the tag check if the mesh reader is touched for another reason.

### Still open

- **How a mesh face's texture maps to pixels — now the largest gap, and it GREW
  during review.** All three readings asserted `face+0x24`/`+0x26` are the
  face's world-unit extents and therefore the UV rule; the measured pair
  contradicts that. Do not implement mesh UVs from those two fields.
- The absolute world sense of the yaw. The *relative* rule is determinate (bias
  0, rotation sign opposite to the view sign); which way `R(+a)` turns in
  GZDoom's own convention is not settled.
- Whether an `IT_OBJECT_DATA` entry ever reaches the **second** entry point,
  `rwss_type04` (`renderer.c:6422-6453`), which feeds the same projector from a
  different record whose rotation byte is at `+0x03`, not `+0x06`.
- One skeptic challenged §2's visible-pass/subpass assignment, saying the lift's
  own header for `0x28dbe` describes it as *building* the per-frame draw list.
  §2's rule was established by measurement and stands until re-measured — but
  the disagreement is recorded rather than buried.

---

## 14. INVENTORY — the spec is sound; here is what it is missing

Inventory is the largest single missing system: **458 of the 1,338 unhandled
records** (§10 item 4). `docs/GAME_inventory.md` was audited against ROTH.C on
2026-10-01 by three independent readers, each attacked by its own skeptic.

**READ THIS BEFORE ACTING ON ANY CLAIM THAT THE SPEC IS WRONG.** All three
audits INVENTED errors in that document, and two of the three were refuted
outright on exactly that ground. The spec is substantially correct and the
failure mode here is not "the spec is stale", it is "a reader misreads it and
reports a fault that is not there". A false accusation against it is worse than
silence, because someone will act on it. Every item below was confirmed by
opening both sides.

### The one omission that matters most, verified by hand

**The in-memory table pointer is pre-biased by minus four, so inventory ids are
1-BASED.** `game_core.c:412-414`:

```
g_dbase100_inventory_table = ptr + (*(base + 0x14) - 4)
g_dbase100_dialogue_table  = ptr + (*(base + 0x1c) - 4)
```

So `table[id]` at a 4-byte stride reads file offset entry `id - 1`, and the
consumers index with the raw id (`inventory.c:389`,
`esi = base + table[di * 4]`). The file-level rule is
`record(id) = base + fileInventoryOffsets[id - 1]`, ids 1..281, with the
`id <= 0` and `offset == 0` guards the original has. The spec never states the
bias. **Everything downstream is off by one without it.**

### The second, also verified by hand

**Weapon ammo is set BEFORE the stackable test, not after** (`inventory.c:392-397`
then `:398`). `give_item` parses the WeaponAction attributes and writes
`quantity` from them, then the stackable branch overwrites `quantity = 1`.
Reversing the order costs every weapon its ammo. (Measured by the audit: no
retail record is both a weapon and stackable, so the two never collide — but
the order is still what makes the weapon path right.)

### A REAL BUG IN CODE THAT ALREADY "WORKS", found on the way

`RunIndexedCommand` — opcode 0x40, 76 records across 10 maps — dispatched its
sub-chain through `RunCommand` instead of `RunCommandWithChain`. The five
opcodes that need the chain (**0x28, 0x36, 0x38, 0x2b and a nested 0x40**)
therefore did nothing at all inside a 0x40 sub-chain and were counted as
unhandled. The original has no such split: its 0x40 loop dispatches through the
same `0x30780` table as the top level (`raw_commands.c:3968-3975`). **Fixed
2026-10-01**, with a nesting depth guard, since a sub-chain can now recurse
rather than merely loop.

### The implementation order

Steps 0-2 and 5-6 and 8 need no new file reading; the rest wait on a
DBASE100.DAT reader, **which the port does not have at all** — verified, the
only occurrences of the name in `src/` are comments.

| # | Step | Needs DBASE100? |
|---|---|---|
| 0 | Fix the shared chain state: the autoselect flag and the two shared tails | no |
| 1 | The slot array and its five primitives. `find_free_inventory_slot` **increments the count itself** (`renderer.c:10035`, the only increment in the program) — `give_item` must not also | no |
| 2 | ~~Close the 0x40 divergence~~ — **done, see above** | no |
| 3 | **A DBASE100.DAT reader sized to four fields.** The gate; do it early | — |
| 4 | `0x29` give, key != 0 — **187 of 239 records, 34 maps, the largest single win** | yes |
| 5 | `0x27` if-not-item, single-query and compare-last — 132 of 161 | no, but after 4 |
| 6 | `0x2a` remove — 56 records, ~15 lines | no |
| 7 | `0x42` filter — 2 records, ~10 lines | mode 0 only |
| 8 | `0x27`'s list forms — 29 records, 7 real decision points | no |
| 9 | The 52 key-0 `0x29` records | **blocked on the object-click trigger channel**, not on inventory |
| 10 | The examine data path (DBASE200/300/400) | yes |

Step 9 is worth noting: those 52 records are blocked on `g_command_active_chain`
(`0x8a134`), the clicked-world-object latch — the object table and the
object-click trigger channel, which is the same missing machinery the face
trigger's direction mask needs (§10 item 4).
