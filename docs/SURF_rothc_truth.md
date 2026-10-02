# ROTH.C ground truth: how the 1996 engine textures and sizes every surface

All paths are relative to `VRealms/tools/ROTH.C/roth_c/src/engine/`. "R:" means `renderer.c`, "RW:" means `render_world.c`, "GC:" means `game_core.c`, "PL:" means `player.c`. Line numbers come from the staged copy.

Tags: **[code]** means read directly off the transcribed code. **[inferred]** means a short derivation from code I quote. **[open]** means the code does not settle it, usually because it depends on data bytes I could not read.

---

## 0. The single most important correction: which pass draws the screen

There are two face-list passes, and both claim sets cited the wrong one.

| Pass | Entry chain | What it is |
|---|---|---|
| **Visible frame** | GC:352 `tick_ambient_render_and_map` → GC:379 `render_world_view_pass` (0x287b6) → RW:357 `render_scene_body` → RW:327 `render_primary_scene_view` → RW:134 `render_clipped_sector_subscene` → **RW:117 `render_world_face_list` (0x2ad21)** | Draws the picture. |
| **Cursor pick** | GC:578 `run_gameplay_frame` → RW:38 `render_world_view` (0x10c8f) with `eax = mouse_x - view_x, edx = mouse_y - view_y` (RW:40-41) → R:9436 `render_world_scene` → **R:9565 `render_world_face_list_subpass` (0x28dbe)** | A 1-pixel window: `left = X, right = X+1, top = bottom = Y` (R:9493-9497). Every driver stops at "subpass kind != 0" and records the texel under the cursor instead of drawing it (R:3159, R:3545-3548, `sprite_secondary_writeout` R:3261). It returns the hit record (GC:578 comment "hit-record ptr"). |

Claim B's evidence at R:8967 and R:8986, and the "bare literal fill mode", come from the **pick pass**. The visible pass at R:9228-9229 and R:9248-9249 does this:

```c
G16(VA_g_span_fill_mode_word) = (uint16_t)(0x38 | ((ES8(esi + 0x17) & 0xc) >> 1));   /* ceiling, R:9229 */
G16(VA_g_span_fill_mode_word) = (uint16_t)(0xb8 | ((ES8(esi + 0x17) & 3) << 1));      /* floor,   R:9249 */
```

**The struct is the RAW SECTOR record, not a face.** `es` is `[0x852c8]`, the geometry buffer (RW:55, `rec+0x10`). The face-list walks sector offsets, and the fields match RAW.md's 0x1A-byte sector exactly:

- +0 ceiling height, +2 floor height (R:9217, R:9237)
- +6 ceiling texture, +8 floor texture (R:9221, R:9241)
- +0xA flags (R:9228)
- +0xD face count, +0xE first face (R:9212, R:9258)
- +0x10 / +0x12 shift words (R:9226, R:9246)
- +0x17 high byte of the flip word at +0x16
- +0x18 mid-platform offset (R:9320)

The same fields are dumped in `staticdump.inc.c`.

---

## 1. Floors and ceilings (world sectors)

### 1.1 Call chain [code]

1. `render_world_face_list`, block A for the ceiling (R:9216-9233) and block B for the floor (R:9236-9253). A ceiling is drawn only if `eye - ceil_h < 0` (R:9217-9219). A floor is drawn only if `eye - floor_h > 0` (R:9237-9239). Here `eye = [0x852fa]` (section 7).
2. `[0x909fe] = eye - h` is the plane height relative to the eye (R:9220, R:9240).
3. There is a three-way test on the texture word (R:9222-9225, R:9242-9245):
   - **Negative (0xFFxx):** `fill = 0x30 / 0xB0` (no 0x08 textured bit) and `scale = 1`. The span is drawn as a solid colour.
   - **Equal to `[0x90a2c]`:** nothing is emitted.
   - **Otherwise:** textured, as follows:
     - The shift word is sector +0x10 or +0x12. Its low byte goes to `[0x90a09]` and its high byte to `[0x90a0b]` (R:9227, R:9247).
     - Scale is `[0x9098c] = (flags & 0x0C) >> 2` for the ceiling and `(flags & 0x30) >> 4` for the floor (R:9228, R:9248).
     - The flip bits from +0x17 are ORed into the fill word (R:9229, R:9249).
     - After the span is emitted, `G32([0x90a08]) = 0` clears the shift again (R:9231, R:9251).
4. `emit_world_span_unclipped(_indexed)` (R:8302, R:8363) builds a screen-space polygon. Its vertices are the projected wall endpoints of the sector: screen X plus the Y already projected at that plane. It then calls `rasterize_world_spans_scanline` (R:8256).
5. **`[0x84f2e]` "fill mode word" IS the span record's draw-flag word at +0x16.** `VA_g_world_span_record = 0x84f18` (g_names.h:365), and 0x84f18 + 0x16 = 0x84f2e (g_names.h:366). The rasterizer copies it straight into the draw flags: `G16(VA_g_world_surface_draw_flags) = HP16(esi_rec + 0x16)` (R:13100-13101). So `[0x9093c]` is 0x38 or 0xB8, ORed with bits 2 and 4.
6. Dispatch (R:13314-13325): `if (flags & 0x20) draw_scaled_sprite_spans` comes **first**. Both 0x38 and 0xB8 contain 0x20, so **every world floor and ceiling goes to `draw_scaled_sprite_spans` (0x39610)**. Bit 0x200 is never set by any face-list writer. `classify_surface_floorceil` always returns 0 (R:4294). So `draw_floorceil_surface` (0x3a84e) is **never reached by world floors**.
7. `draw_scaled_sprite_spans` (R:3286) calls the edge walker `rasterize_floorceil_polygon` (R:3198) only for per-scanline X extents and shading. It first sets `[0x90a23] = 0` (R:3297, R:3320, R:3339), so `build_floorceil_vertex_records` takes the no-texcoord `records:` path (R:3692). The texture coordinates are computed by **floor casting**, described next.

### 1.2 The texel formula [code, derivation inferred but mechanical]

**Setup, per surface (R:3354-3421).** `d98c = [0x9098c]`, the 2-bit scale `s`. For the normal (non-256) case:

```
g_persp_shift = 4 - s                                             (R:3371)
U0 = (u16)[0x90a04] << (0x10 - s)  +  (u16)[0x90a08] << 7          (R:3415)
V0 = (u16)[0x90a06] << (0x10 - s)  +  (u16)[0x90a0a] << 7          (R:3416)
```

- `[0x90a04]` and `[0x90a06]` are `-player_x` and `-player_y`. They are written at PL:181-182 from the camera record, which PL:76 and PL:78 fill with `0 - player_x` and `0 - player_y`. transform_world_vertices confirms this: `X' = worldX + [0x90a04]` (R:6767).
- `[0x90a08]` is `shiftX << 8` and `[0x90a0a]` is `shiftY << 8`. The low bytes are 0 because of the clear at R:9231. So `word << 7 = shift << 15`, which is **half a texel per shift unit** in 16.16.

**Per scanline (R:3074-3077, R:3141-3151).**

- `persp = ((|eye - h| << (4 - s)) * f) / rows_from_horizon`, where `f = [0x90998]` is the vertical focal length (R:3471). This equals `dist * 2^(4-s)`.
- `sin` and `cos` come from `sincos_pair(-viewAngle)` (R:3442), so `sin = -sinA` and `cos = cosA`. The table amplitude is 2^14: the first entries are 0, 0xC9, 0x192, ..., data/obj3_symbols.h:173.
- The column accumulator `v = persp*sin>>2 + stepU*scr + U0`. The row accumulator `v2 = persp*cos>>2 + stepU2*scr - V0`. Here `scr` is screen x minus centre (R:3143-3145).
- `if (fl & 4) { v2 = -v2; u2 = -u2 }` and `if (fl & 2) { v = -v; u = -u }` (R:3149-3151).

**Texel fetch (R:2716-2735).** `index = (((v2 & 0xffff00ff) | (v' & 0xff00)) >> (0x18 - bsr)) & (W*H - 1)`, where `v' = (v << (0x10 - bsr)) >> 8` and `bsr = 8 + log2(W)` (R:3376-3383). Working through the bit positions: **column = int(v) mod W, row = int(v2) mod H, and texel = pixels[row*W + col]**. W is the image width at block +0xC (DAS IMAGE_WIDTH) and pixels start at block +0x10. The loader reads the file block to block +0xA (das_assets.c:606), so block +0xA/+0xC/+0xE/+0x10 are file offsets +0/+2/+4/+6.

Putting it together, the camera terms cancel and the world point `P = cam + dist*(sinA, cosA)` gives the result below.

> **col = ( −Px / 2^s + shiftX/2 ) mod W,  row = ( +Py / 2^s − shiftY/2 ) mod H,  texel = img[row·W + col]**
>
> - `s` = sector `(+0xA & 0x0C) >> 2` for the ceiling, `(+0xA & 0x30) >> 4` for the floor. **2^s world units per texel**: s = 0 → 1 unit, s = 1 → 2 units, s = 2 → 4 units, s = 3 → 8 units.
> - `shiftX` = +0x10 (ceiling) / +0x12 (floor). `shiftY` = +0x11 (ceiling) / +0x13 (floor). Unsigned bytes, half-texel steps.
> - **Flip, byte +0x17:**
>   - Ceiling: bit 2 → draw-flag 2 → `col = −col`. Bit 3 → draw-flag 4 → `row = −row`.
>   - Floor: bit 0 → draw-flag 2 (col). Bit 1 → draw-flag 4 (row).
>   - Negation applies to the whole coordinate including the shift, so the flip mirrors about world origin and stays world-anchored.

**Consequences:**

- **World-anchored, like Doom flats.** Adjacent sectors with the same texture, scale, shift and flip line up seamlessly. [inferred, high]
- A floor and a ceiling with identical settings map the same (x, y) to the same texel. The ceiling is not implicitly mirrored: same formula, only the scanline direction differs (R:3458-3464, R:3511-3521).
- Axis order: **world +Y walks down the stored image rows. World +X walks the stored columns backwards.** If a tool first transposes DAS images for walls (the "90° rotation", DAS.md:186), then in that transposed image world X runs down the rows. That is the only sense in which claim A's "world X drives image rows" is true.

### 1.3 Special cases [code]

- **256×256** (`W == H` and the low byte of W is 0, R:3367):
  - **Opaque** (`[0x8a352] == 0`, R:3385-3393): shift becomes `5 - s` and U0/V0 use `<< (0x11 - s)` and `shift << 16`. That gives **2^(s-1) world units per texel and a full texel per shift unit.** A 256² floor covers the same world area as a 128² texture: the double-density path. The reversed packed-byte loop (R:2739-2773) keeps the same row*256 + col orientation.
  - **Translucent 256²** (R:3394-3410): scale stays 2^s, shift is a full texel per unit.
- **Negative texture (0xFFxx):** a solid colour, palette index = low byte (R:13103-13110). It is shaded by distance and sector light through the colormap (R:3294-3314, loops at R:2573/R:2601). **It is not sky.** The ceiling textures in STUDY1's dump (sample_static_STUDY1.csv: 65400 = 0xFF78, 65436, 65280) are solid-colour fills.
- **Colour key:** `[0x90a2c] = [0x89eec + 0x1A]` (R:9393). init_render_struct copies that from `VA_g_das_unk_0x22` (R:1518), which is **DAS header word +0x22** (map_load.c:363). Surfaces with this id are skipped: flats at R:9225/R:9245, walls at R:8841, R:8666, R:8687, R:8732. Its value, and whether levels use it for "open sky" ceilings, is **[open]**: dump `[0x90cbe]` per map.
- **Image flags** (DAS block modifier = low byte of block +0xA, copied to `[0x8a2a4]` at R:13226-13227):
  - If modifier bit 0x80 is set, the plane height is forced to ±0x4B0 (R:3429-3433).
  - If modifier bit 0x10 is set, U0 and V0 drop the camera term. The texture then moves with the view: screen-anchored (R:3412-3420).
  - Both only matter if a floor texture has those bits. DAS.md:214 says 0x80-0x9F are object sprites. **[open]:** confirm FAT-block-1 floor textures have modifier bits 0x90 clear.
- **Translucent textures** (`[0x8a352] = BLOCK_TYPE & 4`, R:13281-13282) take loops 0x3a100 and 0x3a700. Texel 0 is transparent and texels ≥ 0x80 are blended (R:2669-2672, R:3000-3003).

### 1.4 Verdicts on the claims

| Claim | Verdict | Evidence |
|---|---|---|
| A: floor-cast driver, world-anchored | **TRUE** | R:13314 → R:3286; R:3415-3416 use -cam; R:3471 |
| A: 2-bit scale, 2^s units per texel, bit positions | **TRUE** | R:9228/9248, R:3415 |
| A: shift bytes +0x10/+0x11 ceiling, +0x12/+0x13 floor, half-texel steps | **TRUE** (full texel on 256²) | R:9226-9227, R:9246-9247, R:3415, R:3390 |
| A: flips from +0x17, OR'd into the fill word, then negated | **TRUE, in the visible pass** | R:9229, R:9249, R:3149-3151 |
| A: world X drives image rows | **FALSE as stated** (true only for the transposed image) | derivation in §1.2 |
| A: 256² opaque double-density path | **TRUE** | R:3385-3393 |
| A: `build_floorceil_vertex_records` UVs only for 3D objects | **TRUE** | `[0x90a23]` is only set to 0xFF at R:12867, inside `draw_floorceil_surface`, which needs flag 0x200 |
| B: floors are per-vertex-UV polygons | **FALSE** for world flats | §1.1 steps 6-7 |
| B: scale is per FACE | **FALSE**: it is the sector record | §0 |
| B: scale read only at R:3357, so floors never use it | **FALSE**: R:3357 is inside the floor driver | R:3314 |
| B: floors reach `draw_floorceil_surface` via 0x200 | **FALSE**: 0x20 is dispatched first and 0x200 is never set | R:13314-13320 |
| B: R:11665 index, R:12958-12967 `<<2` UVs | TRUE but irrelevant: this is the mesh-face driver | — |
| B: "mirror bit is really textured" / bare literals | **FALSE** in the visible pass. True only in the pick pass and for mid-platforms. | R:9229 vs R:8968 |
| B: three-way texture test | **TRUE** | R:9222-9225 |
| B: shift is a U/V byte pair | **TRUE** (U = low byte → column, V = high byte → row) | R:9227 |

---

## 2. Mid-platforms (top and underside)

**Chain:** the sector's `+0x18` points to the mid-platform record `geo` (R:9320). There is no flip anywhere on this path (see the last bullet).

**Record layout** matches RAW.md:

| Offset | Field |
|---|---|
| +0 | underside texture |
| +2 | underside height |
| +4 | underside shift word |
| +6 | top texture |
| +8 | top height |
| +0xA | top shift word |
| +0xC | scale byte |

**Rules:**

- **Underside** is drawn only if `eye - geo[+2] < 0`, i.e. the eye is below the underside (R:9324-9341). It uses `(geo[+0xC] & 0x0C) >> 2`.
- **Top** is drawn only if `eye - geo[+8] > 0` (R:9342-9356). It uses `(geo[+0xC] & 0x30) >> 4`.
  - RAW.md calls +0xC "FLOOR_TEXTURE_SCALE", but it holds **both** 2-bit fields, laid out like sector +0xA. [code]
- The emitters are the clipped versions (R:8434, R:8493). They take the sector outline and project each vertex's Y from `[0x909fe]` through its own depth (`clip_project_emit`, R:8421-8427). They then run the same floor-cast driver, so the texel formula of §1.2 applies with these fields. It is **world-anchored**.
- The three-way negative / colour-key / textured test is identical (R:9332-9335, R:9347-9350).
- **Differences from sector flats [code]:**
  - Fill is the bare literal 0x38 or 0xB8, so **no flips** (R:9339, R:9354).
  - The shift is **not cleared** after the emit: there is no `G32([0x90a08]) = 0` on this path, unlike R:9231. That only affects a later stale reader (§5).
- Underside and top are drawn after the sector's walls, inside the secondary-surface block (R:9293-9363).

---

## 3. Walls

### 3.1 Chain [code]

`render_world_face_list` inner loop, R:9262-9291:

- `newbx = es:[face+4]` is the RAW texture-map record.
- Render flags `[0x90a2e]` = texmap +8, the TEXTURE_FLAGS byte (R:9271).
- If HORIZONTAL_FIT is negative (top bit set): `[0x909a8] = 2*SHIFT_X` (texmap +0xA), `[0x909aa] = SHIFT_Y` (+0xB), and `hfit = HFIT & 0xFFF`. Otherwise `hfit = HFIT` (R:9272-9279).
- A face with fs flag 0x20 (a two-sided face) goes to `draw_world_face_clipped_spans`. Others go to `draw_world_face_projected_spans` (R:9282-9286).
- Both then call `emit_world_span_record` (R:8265), which builds the flag word from TEXTURE_FLAGS (R:8270-8277):

```c
ax = cl;  if (!(cl & 4)) ax |= 0x100;   ax &= 0x183;   if (!(cl & 0x10)) ax |= 0x40;   ax |= 0x18;
```

Span record +0x24 is the vertical extent: `ceil - floor` of the piece (R:8869, R:8996). +0x26 is `hfit` (R:9276). `rasterize_world_spans_scanline` then goes to the wall path (R:13327-13361) and on to `wall_body_36b68` (R:4764).

The TEXTURE_FLAGS bits are confirmed by their use in the code:

| Bit | Name | Evidence |
|---|---|---|
| 0x01 | TRANSPARENT | overlay multipass, R:8771 |
| 0x02 | FLIP_X | R:4735 |
| 0x04 | IMAGE_FIT | R:8272 |
| 0x08 | TRANSPARENT_FIXED_SIZE | R:8206 |
| 0x10 | NO_REFLECT | R:8274 |
| 0x20 | HALF_PIXEL | R:13336 |
| 0x40 | EDGE_MAP | R:9281 → sky |
| 0x80 | DRAW_FROM_BOTTOM | R:5057 |

### 3.2 Mapping rules

- **IMAGE_FIT clear → stored-extent path.** Flag 0x100 is set (R:13334-13340):
  - Horizontal extent = `hfit`. Vertical extent = piece height in world units.
  - The column coordinate is taken in half-texel units: `ax >> 1` (R:4734).
  - Result: **1 texel = 2 world units vertically. Horizontally, texels across the face = hfit / 2**, which is 2 units per texel when hfit equals the face length, as RAW.md says is usual. The texture tiles. [code + inferred]
- **IMAGE_FIT set → computed-extent path** (R:13342-13361):
  - Extents are `2*W` (vertical) and `2*H` (horizontal), each multiplied by `(1 + nibble)` from `[0x90971]` = span record byte +0xF.
  - So the image fits the face exactly `(1+n)` times each way.
- **textureFit nibble on world walls:** `[0x90971]` comes from `HP8(esi_rec + 0xf)` (R:13115). For world walls that byte is 0x84f27. **No writer of 0x84f26 or 0x84f27 exists** (grep: none, and obj3_symbols.h:659 lists "no G-macro sites"). So the nibble is its static initial value, **presumably 0**, and IMAGE_FIT is a single fit. **[inferred]** The nibble is real for 3D-mesh wall-type faces, from face byte +0xF (§5).
- **90° stored rotation: TRUE [code].**
  - The horizontal texel index selects the image row: `ax *= [0x90978]`, the row width W (R:4737).
  - The vertical position indexes within the row (R:4738).
  - So each stored row (W bytes) is one vertical wall column. **W is the vertical texel count and H is the horizontal texel count.**
- **FLIP_X:** `u = ~u + H` (R:4735), i.e. mirror across the face.
- **Shifts:**
  - SHIFT_X (texmap +0xA): `[0x909a8] = 2*shiftX` is added to the horizontal start in half-texel units (R:5113). That is **1 texel per unit, horizontal (U)**.
  - SHIFT_Y (texmap +0xB) becomes the vertical start texel `[0x8a33c]` (R:5050, added at R:4738). That is **1 texel per unit, vertical (V)**.
  - Both exist only when the top bit of HORIZONTAL_FIT is set (R:9273).
- **HALF_PIXEL:** doubles both extents and the vertical origin (R:13336). The result is **1 world unit per texel**.
- **Vertical anchor:**
  - v = 0 at the projected **top edge of the piece**. The per-pixel step is `ext_v / (2*npix - 1)` (R:4739-4740) and the column starts at the top corner Y (R:5101).
  - One-sided wall: the top is the sector ceiling (R:8835, R:8832-8836).
  - Upper piece: the top is the **own sector's ceiling** (`clip_top = [0x90a30]`, R:8661-8665). The texture is UPPER, texmap +4.
  - Lower piece: the top is the **neighbour's floor**, the top of the step (R:8680-8686). The texture is LOWER, texmap +6.
  - Transparent mid (overlay, R:8727-8741): the top is `min(ceilA, ceilB)`, the top of the opening (`compute_face_span_extents`, R:8192-8203). With TRANSPARENT_FIXED_SIZE and sector +0xC ≠ 0: `> 0` pins to the top with height `4*val`; `< 0` pins to the bottom with height `4*|val|` (R:8206-8215). **4 world units per step** [code].
- **DRAW_FROM_BOTTOM (0x80):** the vertical start texel is `W - ext_v/2 - shiftY`, wrapped with W-1 if negative (R:5057-5063). The texture's bottom row lands on the piece's bottom edge. [code]
- **Wrap:** the horizontal wrap mask is H-1, or 0xFFFF when no wrap is needed (R:13337-13338). The vertical wrap uses the "wrapped" mapper when `(W - start)*2 < ext_v` (R:5065-5069). Both are correct only for power-of-two sizes.
- **[open]:** which face vertex gets u = 0 (left on screen, presumably VERTEX_01). The per-column u starts at the left projected end (R:4893-4915) and is not tied back to vertex order in the code I read.

---

## 4. Sprites and billboard objects

**Chain.** Sector objects become secondary surfaces (`build_secondary_surface_list`, R:9306). `render_world_secondary_surface` (R:6613) dispatches on type: 2, 3 and 0xFF take the main path and go to `rwss_shared_tail` (R:6471). That resolves the image (`emit_world_face_spans`, R:5740) and draws the sprite **as a vertical wall quad** through the wall driver.

**The object record is `subrec`:**

| Offset | Field | Evidence |
|---|---|---|
| +4 | texture index/source; texture id = `word + 0x1000` | R:5748 |
| +6 | rotation byte | R:6097 via [0x84ab6] |
| +7 | flags; 0x10 = mirror | R:6669 |
| +8 | light | R:6659 |
| +9 | render type; 0x80 = fixed-angle | R:6664 |
| +0xA | Z (world units) | R:6665 |

**Size [code, R:6505-6525, R:6538-6561].** Let W = block +0xC (vertical texels) and H = block +0xE (horizontal texels).

- The half-width, in <<8 view units, is `e = (2H) << cl`. That is **half-width = H·2^(cl-7) world units**.
- The height is `[0x90980] = 2W`. Then, based on the block's `+0xA` word:

| Block modifier and type | cl | Height | Width | World units per texel |
|---|---|---|---|---|
| Modifier bit 0x80 clear (wall-type image) | 7 | 2W | 2H | **2** |
| Modifier 0x80, BLOCK_TYPE bits 0x60 = 0 (the 0x1x "standard") | 6 | W (`>>1`) | H | **1** |
| Modifier 0x80, `n = (BLOCK_TYPE >> 5) & 3`: 0x2x → n = 1, 0x4x → n = 2 | 7+n | `2W << n` | `2H·2^n` | **2^(n+1)** = 4 or 8 |

The jump from 1 unit (standard) to 4 units (0x2x) is what the code says. **Verify with the oracle** before trusting it visually.

**Vertical anchor [code, R:6550-6561].**

- `Yv = eye - Z` (positive is down) and `nib = modifier & 0xF`.
- Modifier bit 0x10 clear (0x8x): the sprite **bottom** is at world `Z - 2*nib - hi(block+4)`, and it extends up by the height.
- Modifier bit 0x10 set (0x9x, **hanging**): the sprite **top** is at world `Z + 2*nib + ...`, and it hangs down by the height.
  - `hi(block+4)` is the high word of block +4, `[0x84aba]` (R:6553). Its value is **[open]**.
- The low word of block +4, shifted `<<8`, is added to view X as a horizontal offset, negated when mirrored (R:5691-5696). Its value is **[open]**.

**Billboard orientation.** The linear path puts both edges at the object's depth, `x ± half-width` (R:6541-6547), so the sprite always faces the camera.

**Fixed-angle objects** (`subrec+9 & 0x80` → `rwss_rotated_tail`, R:6093) [code + inferred]:

- `φ = (2·(rot + 0x40) − viewAngle) & 0x1FF` (R:6097).
- The two edges are at view offsets `(x ± sinφ·HW, depth ± cosφ·HW)` (R:6100-6116).
- In world space the plane runs along direction angle **θ = 2·rot + 128**, where angle a is the direction `(sin a, cos a)`: 0 = +Y and 128 = +X, 512 per turn. Its **normal points at angle 2·rot** (±256).
- Width uses the same `cl` rule as above. When the edges swap (left > right), draw-flag bit 2 is toggled, which mirrors the image (R:6120-6126).

**Directional frame selection** (block modifier bit 0x40 set: 0xC0 directional or 0x40 folder, R:5703-5729):

- 8-way: `t = 2·rot + 0x120 − viewAngle − cdiv`, `idx = (t >> 5) & 0xE`.
  - `cdiv = (viewX << 6) / depth` corrects for the object's bearing off the screen centre.
- 16-way when block +0x10 has bit 0x2000: `t = 2·rot + 0x110 − ...`, `idx = (t >> 4) & 0x1E`.
- The reference word at `block + 0x12 + idx` (DAS DIR_n) selects the image. Its bit 15 toggles the mirror (R:5727).
- If block +0x10 bit 15 is clear, a fixed frame is used instead (R:5706-5707).

---

## 5. 3D mesh objects

**Chain:** `draw_world_sprite_billboard` (R:6691) → `project_sprite_to_render_queue` (RW:622) → queue → `render_world_sprite` (RW:699) → the rasterizer mid-entry (R:13065).

- **Position:** the object at (X, Y) = record +0/+2, Z = +0xA − eye (R:6698-6701).
- **Vertices:** DAS vertex +0 → vx, **+2 → height (up)**, +4 → vy (RW:653-656).
  - The ×2^14 sine amplitude cancels (`>> 14`, RW:658-659), so units are **1:1 world units**.
  - Screen: `sx = rx·e / depth + cx`, `sy = −h·f / depth + cy` (RW:666-673).
- **Rotation:** the view angle minus `2·OBJ_ROTATION` (RW:645, R:6706). Inverting the transform gives world `dx = vx·cos2r + vy·sin2r`, `dy = −vx·sin2r + vy·cos2r` [inferred]. At r = 0, mesh +0 → world +X and mesh +4 → world +Y. The rotation sense is the same as the view angle.
- **Face texturing.** The span record *is* the DAS face record: +0xC texture id, +0x16 flags, +0x1C sub-index, +0x30/+0x34/+0x36 vertices (R:13100, R:13264, R:4283-4285). The branches, taken in dispatch order:
  - **flags & 0x20** ("render as floor"): floor-cast driver (§1.2). The plane height is the first vertex's eye-relative height (RW:718-722).
    - `[0x9098c]` (scale) and the shift words are **not set** on this path. They are whatever the last world flat left: the shifts are 0 after a sector flat, but the scale is stale, and mid-platforms leave both stale. [code/inferred]
  - **flags & 0x200:** `draw_floorceil_surface` with per-vertex UV and per-vertex W perspective (R:12855, R:12956-12971).
    - UVs come from `build_floorceil_vertex_records` (R:3683) and form the texture's corner quad in texel<<6 units:
      - v0 = (0, H'), v1 = (0, 0), v2 = (W', 0), v3 = (W', H'), where `X' = X·64 − 1` (R:3708-3739).
      - Flag 2 swaps the H axis (mirror). Flag 0x20 applies `(1+nibble)` repeat multipliers.
    - **Flag 0x1000:** explicit per-vertex byte texcoords: U = face[+0x20+k], V = face[+0x1C+k], ×64 (R:3766-3795). **[inferred]**
  - **Otherwise:** wall driver on the classify-projected quad (R:4275).
    - Flag 0x100 (RENDER_FLAG_2 bit 0, "repeating") → stored extents from face +0x24 (vertical) and +0x26 (horizontal) in half-texels.
    - Else fit ×(1 + nibble), where the nibble is face byte +0xF.

---

## 6. Sky

- **Identification.** A wall whose TEXTURE_FLAGS has **0x40 (EDGE_MAP)** sets `[0x90a2a]` (R:9281). The wall driver then calls `render_parallax_sky_columns` for the region **from the viewport top down to the wall's top edge** in each of the wall's columns (R:5104-5111). If the whole wall is below the view, the degenerate band path is used (R:4777-4796).
- **Ceilings are never sky.**
  - Ceiling texture == colour key → not drawn (R:9225).
  - 0xFFxx → solid palette colour.
  - Anything else → textured.
  - In painter's order, near ceilings overwrite far sky. [inferred]
- **Sky image:** `[0x89f34]` = map metadata +0x18 SKY_TEXTURE (R:5378, R:5414).
- **Column** = `(LUT[screen_x] − 2·viewAngle) & 0xFF` (R:5466-5468).
  - `LUT[x]` is the low byte of the 512-unit ray angle through column x: `x = cx − tan(a)·e` (R:12000-12008).
  - So the sky tiles 256 columns per 180° of spread, but scrolls 2 columns per angle unit when turning. That is faithful and odd.
- **Row** starts at `[0x909f6] + block[0]` (R:5433). `[0x909f6]` = viewport top margin − `pitch·f >> 7` (RW:315-319), so the sky shears 1:1 with pitch. The row then steps 1 per screen line (2 lines per row in double-scanline mode, R:5453). Fetch is `fs:[row<<8 | col]`, so the sky image is **256 wide, row-major** (R:5466-5491).

---

## 7. Projection

**Camera record** (PL:68-96):

- `rec+0 = −x` and `rec+4 = −y`.
- `rec+2 = −z − [0x8c112]`.
- So **eye = player_z + [0x8c112]** (PL:185). `[0x8c112]` = `g_player_height` (= 2 × metadata +0x0A, 144 by default, map_load.c:206-207) + walk bob. It is clamped to `[floor + 24, ceil − 24]` (PL:371-383). [code; player_z = foot level, inferred]

**Focal lengths** (R:12073-12100, R:9401-9412):

- `aspect = [0x854a4] · 0x132 >> 8`.
- **Horizontal focal** `e = view_w · 0x7C >> 8`, when `view_w·aspect ≥ view_h << 16`. This gives **HFOV = 2·atan(128/124) ≈ 91.8°** at any width.
- **Vertical focal** `f = e · aspect >> 16`.
- Walls and sprites use `[0x8527c] = e` horizontally and `[0x85288] = f << 8` vertically (rwss_proj R:5350; fcs_proj_y R:8558).
- **320×200** (`[0x854a4] = 0xCCCC`, video_display.c:881):
  - aspect = 0.9563, e = 155, f = 148, so **f/e = 0.956**.
  - The code assumes nearly square pixels. On a 4:3 CRT (pixel aspect 1.2) the vertical therefore appears about 1.15× taller than geometrically correct.
  - VFOV at 200 lines ≈ 2·atan(100/148) ≈ 68°. [computed]
- **VESA modes** take `[0x854a4]` from table 0x146e8 (video_display.c:781). The values are **[open]**. The oraclelog projection dump (staticdump.inc.c) should read them directly.

**Pitch is a y-shear**, not a rotation:

- `cy = view_center_y + (pitch · f) >> 7` (RW:66-71). Pitch is clamped to ±0x7E (PL:93-94), roughly ±44.5°.
- Floor casting uses `rows_from_horizon` measured from this shifted cy (R:3458-3521), so floors stay consistent under shear.
- Positive pitch moves the horizon down, i.e. looks up. [inferred]

---

## 8. Notes on the team's oraclelog instrumentation

- `ov_span_floor` and `ov_span_ceiling` hook emitters that both passes call. Rows from the pick pass (`[0x90a48] != 0`) have no flip bits and the wrong window. **Filter on `[0x90a48] == 0`.**
- `VA_FACE_ID` (0x90a42) is written only in the pick pass (R:8953). In the visible pass the sector offset is at 0x8528c (R:9179).
- `width` and `height` (0x90978 / 0x90988) are read **before** `roth_next_…` runs, but the texture is resolved later, inside the rasterizer (R:13288-13289). The values logged are therefore the **previous** surface's.
- The `draw_floorceil_surface` counter will only count mesh faces with 0x200. Zero world-flat entries is the expected result, not a failure.
- The comment in `flatspans.inc.c` saying "larger v = fewer world units per texel" is **false**. U0 = X·2^(16-v), so texel = X / 2^v.
- The comment saying "no sector bits are ever OR'd into [the fill word]" is **false** for the visible pass.

---

## 9. Summary table

| # | Rule | Evidence | Confidence |
|---|---|---|---|
| 1 | The visible frame is drawn by `render_world_face_list` 0x2ad21. 0x28dbe is the 1-pixel cursor pick. | RW:117, RW:40, R:9493-9497 | High |
| 2 | Flat fields come from the RAW sector record (0x1A stride) | R:9212-9258 | High |
| 3 | World floors/ceilings use the floor-cast driver 0x39610 (draw flag 0x20); never 0x3a84e | R:13100, R:13314 | High |
| 4 | col = (−X/2^s + shX/2) mod W, row = (Y/2^s − shY/2) mod H, texel = img[row·W + col] | R:3415-3416, R:3141-3147, R:2725 | High (fetch bit-math derived) |
| 5 | s = ceiling (+0xA & 0xC) >> 2, floor (+0xA & 0x30) >> 4; 2^s units per texel | R:9228, R:9248 | High |
| 6 | Shift = sector +0x10/11 (ceiling), +0x12/13 (floor), unsigned, half texel | R:9227, R:9247, R:3415 | High |
| 7 | Flip: +0x17 ceiling bit 2 → X, bit 3 → Y; floor bit 0 → X, bit 1 → Y (mirror about origin) | R:9229, R:9249, R:3149-3151 | High |
| 8 | Flats are world-anchored and continuous across sectors | §1.2 | High |
| 9 | 256² opaque: 2^(s−1) units per texel, 1-texel shift | R:3385-3393 | High |
| 10 | Texture < 0 → solid palette colour (not sky); == DAS hdr +0x22 → skipped | R:9222-9225, R:13103, map_load.c:363 | High / key value open |
| 11 | Mid-platform: +0xC holds both scales; no flips; underside only if eye below, top only if eye above | R:9324-9356 | High |
| 12 | Wall stored path (no IMAGE_FIT): 2 units per texel vertically, hfit/2 texels across | R:13334-13340, R:4734 | High |
| 13 | IMAGE_FIT: image fits the face once (world-wall nibble has no writer, so presumably 0) | R:13346-13349 | Med-high |
| 14 | Wall image transposed: stored row = wall column; W vertical, H horizontal | R:4737-4738 | High |
| 15 | SHIFT_X = +1 texel along U; SHIFT_Y = +1 texel along V; only when HFIT bit 15 is set | R:9273-9275, R:5113, R:5050 | High |
| 16 | FLIP_X: u → H−1−u; HALF_PIXEL: 1 unit per texel; DRAW_FROM_BOTTOM: bottom-aligned | R:4735, R:13336, R:5057-5063 | High |
| 17 | V anchor: top of the piece (own ceiling / neighbour floor for lower / opening top for mid); fixed-size mid = 4 units × override | R:8661-8686, R:8206-8215 | Med-high |
| 18 | Sprite: 2 units per texel (plain); modifier 0x80 + 0x1x → 1; 0x2x → 4; 0x4x → 8 | R:6509-6525 | Med (code clear; verify visually) |
| 19 | Sprite anchor: bottom at Z − 2·nib; 0x9x hangs, top at Z + 2·nib | R:6550-6561 | Med-high |
| 20 | Fixed-angle plane: along angle 2·rot + 128, normal 2·rot (512 per turn, 0 = +Y, 128 = +X) | R:6097-6116 | Med-high |
| 21 | Directional frame: idx = ((2·rot + 0x120 − view − bearing) >> 5) & 7; 16-way variant | R:5709-5727 | High |
| 22 | Mesh: vertex (+0, +4) horizontal, +2 up, 1:1 units, rotation 2·rot | RW:645-673 | High |
| 23 | Mesh face: 0x20 → floor-cast (stale scale); 0x200 → per-vertex corner UV; else wall driver (0x100 = stored extents) | R:13314-13361, R:3683-3795 | Med-high |
| 24 | Sky: only above EDGE_MAP walls; col = LUT[x] − 2·view; row 1:1 from pitch-shifted top; 256-wide | R:5104-5111, R:5466-5491 | High |
| 25 | HFOV ≈ 91.8°; f = e·aspect; 320×200: e = 155, f = 148 | R:12085-12100, video_display.c:881 | High (VESA values open) |
| 26 | Pitch = y-shear, cy += pitch·f/128, clamp ±126 | RW:66-71, PL:93-94 | High |
| 27 | Eye = player_z + 2·meta[0x0A] (+ bob), clamped ±24 from floor/ceiling | PL:76-81, PL:371-383, map_load.c:206 | High |
