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
4. **The sky.** Realms paints a flat strip above outdoor-flagged walls, drifting
   with yaw and never with pitch; we draw GZDoom's dome, which pinches shut into
   a grey disc overhead. Settled approach: a screen-space quad with no pitch
   term, pitch clamped to the original's shear range so the zenith is never in
   view. `ROTH_BETTER.md` §9 settles that the faithful band comes first and a
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
8. **Doors** — hinges verified 141/141 across 44 maps; the polyobject build was
   reverted (`e1f7ba66ba`) because Realms slabs have two-sided faces and GZDoom
   cannot render two-sided polyobject lines. Nothing opens.
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

**What is still open: the view ORDER.** Realms picks its frame with
`((2*rot + 0x120 - viewAngle) >> 6) & 7`, a turn being 512 units, so its offset
is 202.5° — which is exactly GZDoom's own `45.0/2*9` rounding offset
(`hw_sprites.cpp:1436`). On that basis view *i* is mapped straight to rotation
*i*. **That is a derivation, and §1 is blunt about derivations here.** If it is
wrong every directional prop is rotated by a constant or reversed. One
two-engine look settles it, and settles `ANGLE_SENSE` with it.

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
- **LNK1103 "debugging information corrupt" — SUSPECT THE CODE, NOT THE BUILD.**

  This note has now been wrong twice, so here is the whole history rather than
  another confident rule:

  1. It first prescribed deleting `doomxr.iobj` / `doomxr.ipdb` and the
     offending `.obj`. That sometimes worked.
  2. On 2026-09-29 it was rewritten to blame a parallel-build PDB race, because
     dropping `-m` fixed it that day.
  3. On 2026-10-01 a **clean serial rebuild failed anyway**, repeatedly, on one
     object (`rothmap.obj`), while 627 others linked. Killing `mspdbsrv`,
     clearing every object and every PDB, and rebuilding from scratch did not
     help.

  **It was a specific construct in that one translation unit.** The file had
  been edited to set a cvar with `FindCVar()` + `SetGenericRep()`. Rewriting
  that as an `EXTERN_CVAR` declaration and a direct assignment — the idiomatic
  form — linked first try with no other change.

  So: when one object fails and the rest link, **bisect the source of that
  object** before touching the build. A serial build (no `-m`) is still
  advisable and still cheap:

  ```
  cmake --build . --config RelWithDebInfo --target zdoom -- -verbosity:minimal
  ```
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

## 10. Suggested order

1. ~~Directional sprites~~ — **done 2026-09-28** (§7), bar the view order.
2. **Colour-key ceilings** (§5). Small; the rule is already in `roth_surface`.
   The owner on seeing STUDY2's courtyard, 2026-09-28: *"skybox is all fucked
   up"* — STUDY2 is the EDGE_MAP-wall sky case, so it is the map to work on.
3. **The read-only comparison** (§8). Until this exists nothing can be *proven*.
4. **Sprite size and vertical placement** (`ROTH_SURFACES_FIX.md` §3.4).
5. **The global light term** (§6) — small, fully specified, and visible.
6. Then the bigger engine work the owner has authorised: the shade table applied
   exactly, and paletted rendering, which is what "pixel perfect" ultimately
   requires.
7. Doors, then `ROTH_GAME_PORT.md`.

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
