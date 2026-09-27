# ROTH world-build and draw rules, from ROTH.C

Paths are relative to `roth_c/src/engine/`. "R" = renderer.c, "ML" = map_load.c, "CP" = collision_physics.c, "RW" = render_world.c.
Some claims are inferences rather than direct transcriptions. Those carry the tag **[inferred]**. Some things the staged sources can't settle; those carry the tag **[open]**.

## Headline results

1. **Floors and ceilings are world-anchored.** They are not fitted per sector. World sector floors and ceilings are drawn by a floor-casting span driver. Its texture origin is the camera offset shifted by the sector scale exponent (R:3412-3416, R:9215-9253).
2. **`build_floorceil_vertex_records` does not draw world floors.** It builds 3D-object (DAS mesh) faces. So do its ring-parity texcoords and its 0x1000 "authored UV" variant (R:13299-13311 comment, R:13314-13325, R:3683-3796). No world span record can carry 0x1000 (R:8270-8277, R:9223-9249).
3. **Correction to the project note:** the textureFit nibble byte (span record +0xf, R:13115) is never written by any staged world-face or world-floor emitter (R:8265-8414, R:9147-9372). The nibbles are a 3D-mesh-face field. World floors use the 2-bit sector scale exponent instead. The flats branch of the nibble code (R:3698-3706) is unreachable from both callers, because both clear `+0x23` before the edge-walk (R:3297, R:3320, R:3339, R:3691-3692).
4. **Correction to the note:** the player height is doubled at load. `playerHeight = 2*meta[0x0A]` (=144), `maxClimb = 2*meta[0x0C]+1` (=65), `minFit = 2*meta[0x0E]` (=96) (ML:206-209). These are compared directly against sector Z values (CP:604-612). So "72 units = 6 ft" is wrong by a factor of 2.

---

## 1. Map load

- **Raw copy, no conversion.** The `.RAW` file is read into one heap block and used in place. Selectors are created for the sector/face/metadata block, the vertex block, the commands ("3u"/0x7533 table) and the objects block (ML:417-494). No per-vertex UVs, polygons or floor data are computed.
- **Post-load passes, in full:**
  - `fixup_raw_sectors_after_load` (R:10267-10280). For every sector with `+0x14 == 0xFFFD`: `sec[+0x10] = sec.ceil; sec.ceil = sec.floor`. This initialises a closed door: the open height is saved over the ceiling-shift bytes. Values 0xFFFD/E/F in `+0x14` mark door sectors (R:8718-8726, doors.c:320, 834). So `+0x14` is not just FLOOR_TRIGGER_ID.
  - `mark_geom_sentinel_entries` (R:1637-1648). For every face with `sister == 0xFFFF`, it sets `face[+0xA] |= 0x20` (one-sided marker in the ADD_COLLISION byte).
  - `flag_sectors_with_objects` (ML:133-148). It sets `sector[+0x16] |= 2` if the sector's object-list word is nonzero.
  - Commands: `flag_referenced_object_textures` adds 0x1000 to texture ids referenced by commands whose `+6 & 1` is set (ML:155-191). This is a DAS FAT-range bias (object/sprite block 3 starts at 0x1000). It is not the UV flag. `init_loaded_object_table` resolves links and runs per-type init handlers (ML:1133-1198).
  - Metadata: player start (R:10285-10295). Movement tuning (ML:197-223). Lighting (R:10300-10315).
- **Runtime-only bits written later:** `face[+0xA] |= 0x40` when drawn (automap/"seen") (R:8667, 8864, 8868).

## 2. Wall texture mapping

**Which texture record.** For each face in a sector (stride 0xC from `sector+0xE`), `tm = face+4` points to the texture map (R:9258-9279):

- `fitWord = tm[0]`.
  - If bit 15 is set: `extent = fitWord & 0x0FFF`, `shiftX2 = 2*byte tm[0xA]`, `shiftY = byte tm[0xB]` (R:9273-9276).
  - Otherwise: `extent = fitWord` and there is no shift (R:9278).
  - **Note:** the extended form keeps 12 bits, not 15.
- `flags = byte tm[8]` (R:9271).

**Span flag word** (R:8270-8277):
`F = (flags & 0x83) | (flags&4 ? 0 : 0x100) | (flags&0x10 ? 0 : 0x40) | 0x18`.

| Bit | Meaning |
|---|---|
| 1 | TRANSPARENT: palette index 0 is skipped |
| 2 | FLIP_X |
| 0x80 | DRAW_FROM_BOTTOM |
| 0x100 | Stored-extent path. Set when IMAGE_FIT (bit 2) is clear. |
| 0x40 | Depth-shade bias adjust (R:4824-4846). Set when texmap bit 4 is clear. **[open: meaning]** |

- HALF_PIXEL = flags bit 5 (R:13336).
- EDGE_MAP = flags bit 6 turns on the parallax sky above the wall (R:9281, R:4777-4795, R:5104-5111). The sky texture is `meta[0x18]` (R:10306).

**Texture dimensions.** The resolved texture has `rw = +0xC` (the stored row length, which is the vertical pixel count on screen) and `h = +0xE` (the number of rows, which is the horizontal extent) (R:13288-13289). Along-wall position picks a row (R:4353-4357). That is the 90-degree storage.

**Horizontal (U).** Perspective-correct `t ∈ [0,1]` across the face (N/D accumulators, R:4878-4913, R:5125-5135):
```
Ux2   = t*extX + shiftX2                  (R:4352-4353, shift added R:5113)
U     = Ux2 >> 1                          (2 world units per texel)
if F&2:  U = (h-1) - U                    (R:4355)
U    &= (h-1), or no mask when the range fits (R:4356, R:13338/13347)
```
`extX` has two sources:

- **Stored path** (`F&0x100`): `extX = extent` (×2 if HALF_PIXEL). So `U = t*extent/2 + shiftX` (R:13334-13340). When `extent == wall length`, that is 2 units per texel.
- **Computed path** (IMAGE_FIT): `extX = 2*h*(1+lo)` and `extY = 2*rw*(1+hi)`, from nibble byte `fit` (R:13342-13360). The shifts are zeroed (R:13343). For world faces `fit` is never written (see headline 3), so the result is exactly one copy across the face. **[open: static value of 0x84f27, presumed 0]**
- **[inferred]** `t=0` at the face's left screen end, which is VERTEX_01 for a front-facing face.

**Vertical (V)**, texels down from the anchor (R:4934-4945, R:4376-4381, R:4358):
```
stored path:   V = (anchorZ - z)/2 + rowOff
IMAGE_FIT:     V = (anchorZ - z) * rw_ext/H   (one copy over the piece height H) [per-row step scale not traced]
rowOff = shiftY                                          (R:5050)
DRAW_FROM_BOTTOM: rowOff = ((2*rw - extY)>>1) - shiftY   (bottom-aligned; shift SUBTRACTED) (R:5057-5063)
HALF_PIXEL (stored path): extX, extY and the anchor term are doubled, so 1 unit/texel (R:13336)
```

**Anchors and pieces** (`[0x90986]` = camZ − anchorZ):

- **One-sided:** the mid texture `tm+2` spans the sector's floor to ceiling, anchored at the ceiling (R:8835, R:8869).
- **Two-sided upper:** `tm+4`, from this ceiling down to the neighbour's ceiling, anchored at this ceiling (R:8660-8676).
- **Two-sided lower:** `tm+6`, from the neighbour's floor down to this floor, anchored at the neighbour's floor (the top of the piece) (R:8680-8696).
- **Transparent mid** (TRANSPARENT and the overlay pass): `tm+2` in the opening `[max(floors), min(ceilings)]`, anchored at the opening top (R:8708-8741, R:8171-8204).
  - With TRANSPARENT_FIXED_SIZE (bit 3) and `ovr = (int8)sector[+0xC]` nonzero, the size is `4*|ovr|` world units.
  - `ovr > 0` hangs from the opening top.
  - `ovr < 0` stands on the opening bottom (R:8206-8216).
- A texture index of `0xFFxx` or higher is drawn as the flat colour `xx` (R:13103-13110). A key colour skips the piece (R:8666, 8687).

## 3. Floor and ceiling mapping (world sectors)

**Per-sector setup** (R:9215-9253):

| | Ceiling | Floor |
|---|---|---|
| Drawn when | `camZ < ceil` | `camZ > floor` |
| Texture | `+6` | `+8` |
| Shift bytes | `+0x10/+0x11` | `+0x12/+0x13` |
| Scale `s` | `(sec[+0xA]&0x0C)>>2` | `(sec[+0xA]&0x30)>>4` |
| Flip bits | `(byte sec[+0x17]&0x0C)>>1` | `(byte sec[+0x17]&3)<<1` |
| Span flags | `0x38` or'd with flips | `0xB8` or'd with flips |

Bit 0x20 of the span flags sends the surface to the floor-cast driver (R:13314-13316).

**Driver** (R:3354-3420, R:3441-3453, R:3122-3152). Accumulators carry 16 fractional bits per texel:
```
k = 2^(16-s)
colAcc(screen x, row) = relX(row,x)*k + ( offX*k + shiftX*2^15 )     (R:3415; offX = g_view_offset_x)
rowAcc(screen x, row) = relY(row,x)*k - ( offY*k + shiftY*2^15 )     (R:3416, R:3145)
texel = tex[(rowAcc>>16) & (h-1)][(colAcc>>16) & (rw-1)]            (R:2725-2733, mask R:3411)
```

- The relative terms come from the view sin/cos and `distance = camHeightAboveSurface*focal*2^(4-s)/rowsFromHorizon` (R:3471, R:3141-3145).
- So **`2^s` world units per texel**: s=0 → 1, s=1 → 2 (the "full" default), s=2 → 4, s=3 → 8. The shift unit is 0.5 texel.
- The origin is tied to the camera offset, so the texture is anchored to world coordinates. **[inferred]** `offX = -camX` (it is added to vertices in `transform_world_vertices`, R:6767-6770), which gives a pure world mapping, possibly mirrored.
- Axes, in the natural upright image (W=`+0xE`, H=`+0xC`): world **X drives image rows**, world **Y drives image columns**. So flats are transposed relative to u=X, v=Y. Signs: **[open]**, needs an A/B test.
- **Flips:** flip bit 2 negates the column stepper and base. Flip bit 4 negates the row stepper and base (R:3148-3152). The mirror is about the world axis, not about the sector.
- **256×256 textures** (`rw == h`, low byte 0) with an opaque texture take a different path: base shift `17-s` and perspective shift `5-s` (R:3385-3393, reversed walker R:2739-2772). By arithmetic that is **`2^(s-1)` units per texel**, twice as dense. **[inferred; verify in DOSBox]**
- **Texture-record flags** `[0x8a2a4]` (texture `+0xA`):
  - bit 0x10 drops the camera term, leaving shift only (R:3417-3419).
  - bit 0x80 fixes the plane height at ±0x4B0 (R:3429-3433).

## 4. Mid-platforms

- **Layout:** `sector[+0x18]` points to a record, as in RAW.md: `+0` underside texture, `+2` underside Z, `+4/+5` underside shift, `+6` top texture, `+8` top Z, `+0xA/+0xB` top shift, `+0xC` scales. Bits 2-3 of `+0xC` are the underside `s` and bits 4-5 are the top `s` (R:9338, 9353).
- **Draw** (R:9310-9362):
  - If `camZ < underside`, draw the underside with ceiling rules (flags 0x38, no flips).
  - Else if `camZ > top`, draw the top with floor rules (0xB8).
  - Sides: a face whose neighbour has a platform draws its mid texture over the band `[nbr.underside, nbr.top]`, clipped to its own floor and ceiling and around its own platform (R:8583-8607, R:8784-8792).
- **Collision:** the platform splits the vertical window. You pass under it if `under − max(floor) ≥ minFit`, or over it if `top − feetZ ≤ maxClimb` and `min(ceil) − top ≥ minFit` (CP:614-635, CP:1022-1029).
- **Ledges:** a face whose flags have bit 0x80 (under the collision mask) and whose texmap has bit 3 set, in a sector with `ovr<0`, becomes walkable at `floor + 4*|ovr|` (CP:715-720).

## 5. Objects

- **Record** (RAW 0x10 bytes): x `+0`, y `+2`, texture word `+4`, rotation byte `+6`, flags `+7`, light `+8`, renderType `+9`, Z `+0xA`.
- **Z is absolute.** It is compared against the camera Z (R:6665-6666, RW:700-701) and against collision Z windows (CP:1264).
- **Rotation:** the byte is in 256-per-turn units and is doubled into the 512-per-turn view angle (R:6097, R:5862, RW:706). Collision swaps its box axes when `((rot+0x20)&0x40)` is set, i.e. 64 = 90° (CP:1318).
- **Sprite size** (R:6505-6525, R:6538-6561):
  - Width = `2*(+0xE)` world units, centred.
  - Height = `2*(+0xC)`.
  - Scale: if texture `+0xA & 0x80`, bits 13-14 give an exponent n and the size is ×2^n; with no exponent the size is ×0.5.
  - Lateral hotspot `+4` and vertical offset `+6` of the frame, in world units (R:5975-5980, R:6553). The lateral offset is negated when the sprite is flipped.
- **Vertical anchor** (low byte `d` of texture `+0xA`, the DAS 0x8X/0x9X modifier; R:6552-6555, R:6130-6136):
  - `d&0x10` clear (stands): spans `[z − 2(d&0xF) − yoff, … + H]`.
  - `d&0x10` set (**hangs**): spans `[z + 2(d&0xF) + yoff − H, z + 2(d&0xF) + yoff]`.
  - The rotated path omits `yoff`.
- **x-flip:** object `+7 & 0x10` (R:6669).
- **Billboard vs fixed:** `+9 & 0x80` selects a fixed plane oriented at view-relative angle `2*(rot+0x40) − V` (perpendicular to the facing `2*rot`), with half-width `(+0xE)` (R:6664, R:6095-6143). Otherwise the sprite always faces the camera.
- **Directional frames** (texture `+0xA & 0x40`, selector word `+0x10 & 0x8000`) (R:5703-5729):
  ```
  off = (lateral<<6)/depth                       (≈ 64*x/z, 64 = 45°)
  8-dir : k = ((2*rot + 0x120 − V − off) >> 5) & 0xE     -> word index
  16-dir (+0x10 & 0x2000): k = ((2*rot + 0x110 − V − off) >> 4) & 0x1E
  frame = word[+0x12 + k]; bit15 = mirror; (frame&0x7FFF)<<4 = byte offset
  ```
  Frame 0 is shown when the camera looks opposite the object's facing. RAW.md numbers directions differently ("dir 1 = back"). Use the formula.
- **Lighting:** `shade = light ? light − 0x80 + sectorLight : 0`. 0x80 means "same as the sector" (R:6659-6661).
- **3D meshes** (texture `+0xA & 0x8000`; RW:622-676):
  - Vertex stride 0x10: `+0` x, `+2` **up**, `+4` depth-y. Units are 1:1 world.
  - Rotated by `(V − 2*rot)`: `x' = x cos − y sin`, `y' = x sin + y cos`.
  - Height = `vtx+2 + (z − camZ)`.
  - Faces go through `rasterize_world_spans_scanline` with face `+0x16` flags (R:13314-13361):
    - 0x20: floor-cast, world-anchored.
    - 0x200: polygon with corner texcoords `(0|W, 0|H)` by ring parity (R:3726-3739).
    - 0x1000: authored per-vertex texel UVs, bytes `+0x1C+i` (V) and `+0x20+i` (U) (R:3766-3794).
    - Otherwise: the wall-column path. Fit byte `+0xF` gives repeats `(1+hi)×(1+lo)`, or stored extents `+0x24/+0x26`.
    - Graphics-folder sub-image comes from `+0x1C` (R:13263-13266).
- **Collision** (CP:1248-1323). The table is DAS `[hdr+0x24]` (ML:381-382), indexed by object `+4` word ×4.
  - Normal: Chebyshev half-width `tbl[+2]` plus radius, height `tbl[+0]`.
  - `+7 & 1`: box `[shift, h, w⊥, w∥]<<shift`.
  - `+7 & 8`: occupies `[z−h, z]` (hanging); otherwise `[z, z+h]`.

## 6. Lighting

- **Colormap lookup:** `pixel = cmap[(shade<<8) | texel]`, with `shade` in 0..31 (31 is black). Tables come from the DAS palette block (ML:375-379, R:2651, 2670, R:4337-4340).
- **Sector light `L`** = `sec[+0xB]`, plus the global light offset if `L≠0` (R:9186-9188). Per surface (R:4812-4821):
  ```
  bias = 8 + (L − 0x80);  maxShade = 0x20 − (max(L−0x80+4,0)>>2)
  shade = clamp((depthTerm >> k) − bias − viewBias, 0, maxShade); ≥31 → black
  ```
  `k` is set by the shade level (L0=3, L1=2, L2=1) through `patch_span_driver_shade` (R:9127-9139, R:4987).
- **Shade level per sector** (R:9190-9213):
  - Normal sectors use `meta[0x10]`.
  - **CANDLE** (`sec[+0xA]&2`) sectors use `meta[0x14]` (LIGHT_AMBIENCE), the alternate ramp and tint table, and radial falloff from screen centre (R:5005-5017).
  - The tint table equals the normal one unless `meta[0x16]≠0` (R:10307-10314).
  - While the player stands in a CANDLE sector, `viewBias` is 13 lower, so the scene is brighter (CP:1773-1778, R:9388-9390).
- **LIGHTNING** (`sec[+0xA]&0x40`) plus runtime flash bits `&0x49` switch to the glow table at full bright (R:9200-9205).
- **CANDLE_GLOW** (`meta[0x12]`) is stored (R:10303), but its consumer is not in the staged files. **[open]**
- **Transparency:** texel 0 is skipped if span bit 1 is set. Texel `&0x80` is blended through the 64K table when the texture is translucent (R:2669-2672, R:13281-13285).

## 7. Coordinates

- **Angles:** 512 per turn (sin/cos masks 0x1FF, R:1380-1388, R:12204-12217). The sine table has 2^14 amplitude **[inferred]**.
- **Camera** (R:6738-6778, R:1278-1291):
  ```
  rel = world + viewOffset;  depth = (y*cosV + x*sinV);  screenX ∝ (x*cosV − y*sinV)
  ```
  At V=0 the camera looks along **+Y** with screen-right = **+X**. Z is up (a larger Z projects higher: R:5352-5356 with `v = camZ − z`). The frame is right-handed.
- **Bearings:** `atan2_bearing` gives +Y→0 and +X→128 (R:614-646), so V increases clockwise when +Y is drawn up.
- **Player angle** A: forward `= (sin(−A), cos(−A))` (R:1180-1204). A is counter-clockwise. **[inferred]** `V = −A`; the conversion code (`apply_view_camera_params`) is not staged.
- **Scale:** the player is 144 units tall with radius 28. Vertex coordinates are world units. Textures are 2 units per texel by default.

## 8. Collision

- **Player** (CP:1686-1706):
  - Radius 0x1C (box ±28, `perp² < 64·28²`).
  - `stepMax = maxClimb` (65), or 16 in the special airborne mode.
  - Span threshold `minFit` (96).
  - Wall mask 0x81.
  - Z reference is the feet (CP:1704, CP:65).
- **Portal crossing** (CP:604-612). Blocked if:
  - `nbr.floor − feetZ > stepMax`, or
  - `min(ceil) − max(floor) < minFit`, or
  - `feetZ − nbr.ceil > minFit`.
- **Face flags** `f = face[+0xA]`:
  - **Point mode** (player): `m = f & mask`. `m=0` passes. `m≠0` without 0x80 is solid. 0x80 means special platform or ledge (CP:709-735).
  - **Ray mode** (projectiles): passes if `f&2` or `(f&3)==0`. `(f&3)==1` is solid unless 0x80 (CP:446-470).
  - The enemy mask is set outside the staged files. **[open]**
- Door sectors (`+0x14 ≥ 0xFFFE`) block unless the door record says open (CP:454-457, CP:731-733).
- **Other entities** use radius `g_projectile_collision_width` (CP:1591-1603) and sub-step at velocity magnitudes 0xC/0x18/0x30.
