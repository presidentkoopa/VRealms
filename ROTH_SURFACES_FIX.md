# ROTH_SURFACES_FIX.md
### Why textures and scaling are wrong, and the fix
*2026-09-28. Built from two independent reads: ROTH.C (what the original does) and REMAROTH
(what we do). Full evidence in `docs/SURF_rothc_truth.md` and `docs/SURF_remaroth_audit.md`.
The deciding ROTH.C lines were re-checked by hand. Line numbers: R = `renderer.c`,
RW = `render_world.c`, PL = `player.c`, all in ROTH.C `roth_c/src/engine/`.*

---

## 0. Verdict

**Realms floors and ceilings are world-anchored flats with a per-sector scale, shift and mirror.
GZDoom's flat transform can express that exactly. No engine change is needed for flats. Stop the
per-plane-UV engine work.**

The per-vertex-UV finding came from reading the wrong code path. ROTH.C runs the world renderer
twice:

| Pass | Entry | Purpose |
|---|---|---|
| **Visible frame** | RW:117 → `render_world_face_list` (0x2ad21, R:9147) | draws the picture |
| **Cursor pick** | `render_world_view` → `render_world_face_list_subpass` (0x28dbe, R:8903) | a **1-pixel** window (R:9493-9497) that records what the mouse is over |

- The lines cited for "scale is per face" (R:8967, R:8986) are in the **pick** pass. The pick pass
  sets a bare fill word with no flip bits (R:8968).
- The visible pass reads the **sector** record and ORs its flip bits in (R:9229, R:9249).
- `draw_floorceil_surface` (0x3a84e) and its per-vertex UVs (R:11665, R:12958-12967) are the
  **3D-mesh face** driver. World flats never reach it. Dispatch tests flag 0x20 first
  (R:13314-13320), the visible pass's fill word is 0x38/0xB8 (bit 0x20 set), and that word *is* the
  span record's flag field (`VA_g_span_fill_mode_word` = 0x84f2e = record 0x84f18 + 0x16, read as
  draw flags at R:13100-13101).
- The scale read at R:3357 is inside `draw_scaled_sprite_spans` (R:3286). That function is the
  floor-cast driver world flats use, so floors *do* use the scale.

---

## 1. Why there are so many bugs: five root causes

1. **Rules taken from the wrong code path:**
   - the cursor-pick pass, for floor flags;
   - the 3D-mesh face driver, for floor UVs.

   Every rule must be traced from the **visible** entry (`render_world_face_list`) to the texel
   fetch.
2. **One rule implemented in several places with different numbers.** The audit found:
   - flat scale in **five** versions;
   - mid-platform scale 2× different between the loader and the runtime;
   - two wall horizontal-scale rules;
   - conflicting transpose rules, object facing, player height and door models.

   Every fix so far has landed in one copy and not the others.
3. **Engine defaults nobody set:**
   - **Pixel stretch.** The engine's default vertical stretch of 1.2 is never overridden. That makes
     rooms about 20% taller, which is the likely cause of "ceilings look too tall".
   - **Field of view.**
4. **Rig errors:**
   - The oracle's flat hooks also fire during the pick pass.
   - It reads the face id from a pick-only global.
   - It reads texture size before the texture is resolved.
   - The ROTH.C and REMAROTH screenshots were captured at different resolutions (640×480 vs
     320×200, about 16% FOV difference).
   - Screenshots don't record which build made them.
5. **Status reported on counts and eyeballing, not per-texel comparison.**

The fix is one surface module, the correct rules, and a test that checks texels rather than
pictures.

---

## 2. The architecture fix: one source of truth

Create **`src/roth/roth_surface.{h,cpp}`**: pure functions, no engine types. For every surface kind,
given the map data (plus a world point where needed), return what ROTH.C would sample:

- `FlatTexel(sectorOrPlatform, plane, worldX, worldY)` → `(texture, col, row)`
- `WallTexel(face, piece, alongWallT, worldZ)` → `(texture, u, v)`
- `SpriteQuad(object)` → world size, bottom/top Z, facing
- `SurfaceKind(textureWord)` → textured / solid palette colour / colour key (draw nothing)

Then:

- **The loader, the runtime (moving platforms, texture-change opcodes, doors) and the tests all call
  this module.** Nothing else computes a scale, offset, flip or anchor. Delete the other copies.
- Each loader conversion becomes "choose engine values so the engine's (u,v) equals
  `roth_surface`'s answer". The static test (§5) proves it.

---

## 3. The rules (authoritative; from the visible pass)

### 3.1 Floors and ceilings: world-anchored
Sector record (0x1A stride):

| Field | Ceiling | Floor | Source |
|---|---|---|---|
| texture word | +6 | +8 | R:9222-9225 |
| scale bits `s` | `(+0xA & 0x0C) >> 2` | `(+0xA & 0x30) >> 4` | R:9228, R:9248 |
| shift X / Y | +0x10 / +0x11 | +0x12 / +0x13 | R:9227, R:9247 |
| mirror X / Y (byte +0x17) | bit 2 / bit 3 | bit 0 / bit 1 | R:9229, R:9249 |

**The three-way texture test** (R:9222-9225):
- Negative (0xFFxx): **solid palette colour** xx.
- Equal to the DAS colour key (header word +0x22): **draw nothing**.
- Otherwise: textured.

**The texel formula** (R:3371, R:3415-3416, R:3471, R:3141-3151; fetch R:2716-2735), for world
point (x, y):

```
col = ( -x / 2^s  + shiftX/2 ) mod W        W = the image's stored row length (block +0xC)
row = ( +y / 2^s  - shiftY/2 ) mod H        H = block +0xE
texel = image[row * W + col]                the RAW stored layout, NOT transposed
mirror X / Y negates that axis INCLUDING its shift term (mirror about the world origin)
```

- **2^s world units per texel** (s = 0, 1, 2, 3 → 1, 2, 4, 8). Shifts are unsigned, in half-texel
  steps.
- **[derived]** The scale, the shift units and the mirror bits are read directly from the code. The
  two **signs** (−x, +y) come from a short derivation in which the camera terms cancel. Confirm them
  with the live sampler (§5) before relying on them.
- **256×256 opaque textures** (R:3385-3393): 2^(s−1) units per texel, and one shift unit is a
  **full** texel. 256×256 translucent textures keep the normal rule.
- Adjacent sectors with the same texture and settings are seamless. Where the designers changed
  shift or mirror between rooms, the seam is intentional, and we must reproduce it.
- **[open]** Image modifier bits 0x80 (fixed plane height ±0x4B0) and 0x10 (drop camera term)
  (R:3412-3433). Confirm they're clear on floor textures.

### 3.2 Mid-platforms
- Same formula.
- The scale byte at +0xC holds both fields: `& 0x0C` = underside, `& 0x30` = top.
- **No mirror flips:** the fill word is a bare 0x38/0xB8 (R:9339, R:9354).
- Underside drawn only when the eye is below it; top only when above it (R:9324-9356).

### 3.3 Walls
- **Storage.** Art is stored rotated 90°: the along-wall index selects the image row (R:4737).
  Transposing wall images once at load is correct.
- **Horizontal (stored-extent path, IMAGE_FIT clear).**
  - **Texels across the face = extent / 2**, where extent comes from the face texture record: the
    low 12 bits of the fit word if its top bit is set, otherwise the whole word (R:13334-13340).
  - This is **not** "0.5 × wall length". It only equals that when the stored extent equals the
    length.
  - HALF_PIXEL (bit 5): 1 world unit per texel, i.e. extent texels across (R:13336).
- **Vertical:** 2 world units per texel; HALF_PIXEL makes it 1.
- **IMAGE_FIT (bit 2):** one copy stretched over the piece. The repeat nibble is never written for
  world walls, so ×1.
- **Shifts** exist only on extended records (fit-word top bit):
  - shiftX = +1 texel along U per unit (R:5113);
  - shiftY = +1 texel along V per unit (R:5050).
- **FLIP_X (bit 1):** u → H−1−u, applied **after** the shift (R:4735).
- **Vertical anchor** (v = 0 at the anchor):

  | Piece | Anchor | Source |
  |---|---|---|
  | one-sided wall | own ceiling | |
  | upper | own ceiling | R:8661-8665 |
  | lower | top of the step (the neighbour's floor) | R:8680-8686 |
  | transparent mid | top of the opening (lower of the two ceilings) | R:8192-8203 |
  | fixed-size mid (bit 3) | 4 × sector +0xC tall; positive = hangs from the top, negative = stands on the bottom | R:8206-8215 |
  | DRAW_FROM_BOTTOM (bit 7) | the texture's bottom sits on the piece's **bottom** edge | R:5057-5063 |

- **[open]** Which end of the face has u = 0. Settle it with the static test (§5).
- Each face starts its own U at 0. ROTH does **not** carry U across neighbouring faces, so don't add
  continuity.

### 3.4 Sprites and objects
- **Size:** 2 world units per pixel, adjusted by the art's size modifier (R:6509-6525):

  | Art's size modifier | World units per pixel |
  |---|---|
  | none | 2 |
  | 0x80 with block type 0x1x | 1 |
  | 0x80 with 0x2x | 4 |
  | 0x80 with 0x4x | 8 |

  Check the unusual 1 → 4 jump on one example in the oracle.
- **Stored transposed** like walls: vertical texels = the stored row length.
- **Vertical position** (R:6550-6561):
  - normal: **bottom at Z − 2·(modifier & 0xF)**;
  - hanging (modifier bit 4): **top at Z + 2·(modifier & 0xF)**.

  Block +4 adds offsets **[open]**.
- **Fixed-angle objects** (object +9 bit 0x80): the plane's normal is 2·rot in 512 units per turn,
  where **0 = +Y and 128 = +X** (clockwise with +Y up) (R:6093-6126).
  - Doom angles run counter-clockwise from +X, so **Doom angle = 90° − rot·360/256** (as long as the
    loader doesn't mirror X).
  - The current loader uses **+**. Verify on an asymmetric fixed-angle object at rot = 64.
- **Directional frames:** 8-way index `((2·rot + 0x120 − viewAngle − bearing) >> 5) & 7`. Bit 15 of
  the frame reference mirrors (R:5709-5727).

### 3.5 3D meshes
- Vertex +0/+4 horizontal and +2 up, **1 mesh unit = 1 world unit**, rotated by 2·rot in 512 units,
  same sense as the view (RW:645-673).
- Faces are textured by the mesh-face driver (R:3708-3739; per-vertex UVs at the texture corners,
  flag 2 mirrors V). **This** is where the per-vertex-UV code belongs.

### 3.6 Sky, colour key and solid colours
- **Ceilings are never sky.**
- The sky is drawn only **above walls flagged EDGE_MAP** (0x40), from the top of the view down to
  the wall's top edge. It uses the map's sky texture (R:5104-5111).
- A ceiling equal to the colour key draws nothing, so the sky drawn by nearby EDGE_MAP walls shows
  through. Mapping colour-key ceilings to engine sky is a fair approximation of that.
- **0xFFxx ceilings are solid palette colours, not sky.** STUDY1 has 0xFF78, 0xFF9C and 0xFF00
  ceilings. Check the black side-aisle ceilings against this rule first.

### 3.7 Projection and view
- **Horizontal field of view ≈ 91.8°**: focal = view width × 0x7C/256.
- **Looking up and down is a vertical shear, not a rotation:** the picture shifts, ±126 limit
  (RW:66-71, PL:93-94). Irrelevant for VR, relevant for flat comparisons.
- **Eye height = player Z + 2 × metadata +0x0A** (144 on most maps), clamped 24 units from floor and
  ceiling (PL:76-81, PL:371-383).
- **Pixel aspect.** ROTH treats pixels as nearly square in world terms. For a geometrically true
  world (and VR requires one), **REMAROTH must render with pixel stretch 1.0**, not the engine
  default 1.2.

---

## 4. What to change in REMAROTH (defect → cause → change)

| # | Defect you see | Cause (audit) | Change |
|---|---|---|---|
| 1 | Floorboards rotated 90° | Flats are transposed like walls | **Don't transpose flat images.** Register the raw stored layout, and set the flat transform so u = −x/2^s + shiftX/2 and v = +y/2^s − shiftY/2 (texels) |
| 2 | Flat patterns 2× (or 4×) too coarse | Scale is 1/(2^s·2), i.e. 4 units per texel at s=1 | **2^s units per texel**; the 256×256 opaque exception |
| 3 | Rug border and edges misaligned, blocky seams | Wrong scale, wrong axes, flips ignored, shift conversion | Shift in half-texel steps with the signs above; **mirror via negative scale on that axis, applied to the shift too**; floor flips from +0x17 bits 0-1, ceiling bits 2-3 |
| 4 | Platform surfaces wrong, and they change when a platform moves | Loader and runtime use different scales; platform shifts in raw units with no Y negation | Both go through `roth_surface`; +0xC split into underside and top; no flips |
| 5 | Walls stretched or squashed where ROTH's aren't | Fixed 0.5 horizontal scale; stored extent ignored except on doors | **Texels across = extent/2** on every wall (HALF_PIXEL: extent) |
| 6 | Pinned lower walls and some uppers misplaced | DRAW_FROM_BOTTOM mapped to Doom's unpegged-bottom flag (wrong anchor on lowers); upper anchor left to Doom's default | Compute each piece's **explicit** vertical offset from the anchors in §3.3; don't rely on Doom's peg flags |
| 7 | Rooms feel too tall | Engine default pixel stretch 1.2 never overridden | Set **pixel stretch 1.0** in the game definition or the loader |
| 8 | Black side-aisle ceilings | 0xFFxx / colour-key handling | Apply the three-way test (§3.6). 0xFFxx = solid colour, key = open (sky approximation) |
| 9 | Hanging and standing objects slightly off vertically | Size modifier's vertical nibble read but not applied | Apply §3.4 |
| 10 | Some fixed-angle objects face wrong | Sense +1 against ROTH's clockwise convention | Check against the oracle at rot 0, 64, 128, 192; expect 90 − rot·1.40625 |
| 11 | Far door has no texture; mesh skins [check] | Transpose applied to all art | Walls and sprites: transpose. Flats: **no**. Mesh skins: verify with one sample |

---

## 5. How it's proven (fix the rig first)

**Fix the rig before trusting any new number:**
- Only log when **`[0x90a48] == 0`** (the visible pass).
- Take the sector from `0x8528c`, not `0x90a42` (that one is pick-only).
- Read texture width and height **after** the texture is resolved.
- Correct the two wrong comments in `flatspans.inc.c`:
  - "larger v = fewer units per texel" is backwards;
  - "no sector bits are ORed" is false for the visible pass.
- A zero count for `draw_floorceil_surface` on world floors is **correct**, not a failure.
- Capture ROTH.C and REMAROTH screenshots at the **same resolution and aspect**, and write the build
  id into each file name.

**Then three levels of proof:**
1. **Static texel test**, no rendering. For every sector, platform and wall piece in STUDY1, pick 3
   world points:
   - `roth_surface` gives (texture, col, row);
   - the engine's own flat or wall transform, as the loader set it up, gives (u, v);
   - they must be equal modulo the texture size.

   Run it on all 44 maps; it's cheap.
2. **Live texel sampler** at the reference views: ROTH.C's visible pass (fixed rig) against
   REMAROTH, per screen pixel: surface, texture and (u, v). Mismatches get grouped by cause.
3. **Same-resolution screenshots**, for everything numbers can't catch (lighting, framing).

---

## 6. Order of work (each step proven before the next)

1. Fix the rig (§5).
2. Create `roth_surface` with the §3 rules and the static texel test. Delete every duplicate
   computation.
3. Flats: no transpose, 2^s scale, the 256 rule, signs, shifts, mirrors. Static test passes.
4. Mid-platforms, loader and runtime through the same module.
5. Pixel stretch 1.0, then recheck framing and room height against ROTH.C at the same resolution.
6. Walls: stored extent everywhere, explicit anchors, DRAW_FROM_BOTTOM, find the u = 0 end.
7. Sprites: size table, vertical nibble, facing sign.
8. Colour key, solid colours, black ceilings.
9. Then the doors, then the game layer (ROTH_GAME_PORT.md).

## 7. Working rules going forward

- Every rule is traced from **`render_world_face_list` (0x2ad21)** to the texel fetch. Not the pick
  subpass, not the mesh driver.
- "ROTH.C doesn't do X" must come with the searches that were run.
- Investigate → implement → prove, as separate steps. No provisional value ships without a
  `// PROVISIONAL` tag and an entry in ROTH_STATE.md.
