# REMAROTH texturing and scaling audit: what the code does today

This covers the files staged under `/mnt/user-data/uploads/REMAROTH/`. I only read files and changed nothing. Short names used below:

- `RM` = `src/maploader/rothmap.cpp`
- `RAWH` / `RAWC` = `src/roth/roth_raw.h` / `.cpp`
- `TEXH` / `TEXC` = `src/roth/roth_texture.h` / `.cpp`
- `TOBJ` = `src/roth/roth_texture_object.cpp`
- `DASH` / `DASC` = `src/roth/roth_das.h` / `.cpp`
- `OBJ` = `src/roth/roth_objects.cpp`
- `RT` = `src/roth/roth_runtime.cpp`
- `HWW` = `src/rendering/hwrenderer/scene/hw_walls.cpp`, `HWF` = `hw_flats.cpp`, `HWS` = `hw_sprites.cpp`
- `RDEF` = `src/gamedata/r_defs.h`
- `PY` = `roth_pipeline/tools/build_map.py` (the "Python oracle")

**A caveat on what was staged.** The staged `maploader.cpp`, `p_setup.cpp`, `p_openmap.cpp` and `g_level.cpp` contain no Realms hook: none of them mention `LoadRothMap`, `SpawnPreparedObjects` or `isRoth`. `g_levellocals.h` is not staged either. That means I cannot see the engine side of `ForcedPlayerViewHeight`, `ForcedPlayerHeight` or `ShadeFalloffShift`, the vertex builder that produces base flat UVs, or `FTexCoordInfo`. Wherever a claim depends on those, it is marked NOT VISIBLE.

---

## 1. Floors and ceilings

### How each sector plane is set (RM:453-527, recomputed identically at RT:1969-2002)

**Texture.** `index` is `floorTexture` or `ceilingTexture` (RM:457). The branches:
- If `art.IsSkySurface(index)`, meaning the index equals the pack header word at +0x22 (TEXC:238-245, DASC:53), the plane gets `skyflatnum` (RM:474-480).
- Otherwise it gets `art.World(index)` (RM:475). This is the same registration walls use, so **flats get the same quarter-turn transpose as walls** (TEXC:343-346).
- If that yields nothing (no pack, or a decode failure) it also falls back to `skyflatnum` (RM:480). The stated reason is to avoid hall-of-mirrors.

**Scale.** Two lines do the work:
- `unitsPerTexel = (1 << shift) * 2.0` (RM:515), with `shift = FloorScaleShift()` = `(flags>>4)&3` or `CeilingScaleShift()` = `(flags>>2)&3` (RAWH:101-102).
- `SetXScale` = `SetYScale` = `1/unitsPerTexel` (RM:523-524).

That gives s=0 → 2, s=1 → 4, s=2 → 8, s=3 → 16 world units per texel.

**Offsets.**
- `SetXOffset = shx * unitsPerTexel * 0.5` (RM:525).
- `SetYOffset = -shy * unitsPerTexel * 0.5` (RM:526).
- `shx`/`shy` are **unsigned bytes**, 0..255 (RAWH:73), so they are never negative.
- In GZDoom's flat matrix the offset is added before the scale (HWF:77-95), so in texels the shift comes out as shx/2. That is a half-texel step, independent of s.

**Things the code never sets:**
- Rotation: nothing calls a flat angle setter, so `Angle` stays 0.
- Flips: none. RM:519-522 says "No mirror flips".
- `flags2` high byte: RM:557-561 says it is "Counted either way", but **no counter exists**. That comment describes code that is not there.

**Stated justifications, and how they contradict each other inside RM:**
- **RM:425-431** says "2^s world units per texel … s == 1 is the common case and reduces to the global two-units-per-pixel rule". The code at RM:515 computes 2^(s+1), so s=1 gives 4, not 2. **SELF-CONTRADICTORY.**
- **RM:433-440** says the shift step is "2^s/2 world units". The code's step is `unitsPerTexel/2` = 2^s world units. The comment and code disagree by 2x in world terms.
- **RM:441-446** says flats are world-anchored, citing renderer.c:3415-3416 against 6768-6770. That agrees with Doom's behaviour, and no translation is applied.
- **RM:483-514** withdraws all of the above:
  - It says the scale "IS A PLACEHOLDER, AND THE FIELD IT READS IS THE WRONG ONE".
  - It says the only read of the field is renderer.c:3357 (the sprite/wall setup), that flats go through `draw_floorceil_surface` with per-vertex texture coordinates (renderer.c:11665, 12958-12967), and that "there is no value of this constant that is correct".
  - It says the value "looked closest on STUDY1 … Do not tune it. Do not cite it."
- **RT:1996** cites the same arithmetic as "2^(v+1) -- see the measurement beside rothmap.cpp's copy". RM says it is not a measurement.
- **`tools/oraclelog/flatspans.inc.c:19-26`** says the 2-bit field IS consumed by the span driver (renderer.c:3406/3414, `g_persp_shift = 4 - v`). It calls v "an EXPONENT COUNTING DOWN" and says "Our loader has it counting up, which is the whole defect". That contradicts both RM:483-514 (the field is never read for flats) and RM:425 (it counts up).

**Engine changes for flats: none found.**
- `hw_SetPlaneTextureRotation` (HWF:68-99) is the stock order: scale → translate(offs/texsize) → scale(64/texsize) → rotate.
- A grep across HWF, HWW, HWS, `hw_drawstructs.h` and RDEF for Realms, `explicitUV`, `TexelLength` or similar finds nothing flat-related.

**Is there a per-vertex UV mechanism? No.** In every staged engine and loader file, flats get only the Doom world-grid `FTransform`: `xOffs`/`yOffs`/`xScale`/`yScale`/`Angle` (RDEF:587-597). The only per-vertex texcoords anywhere are:
- the oracle *probe* that samples ROTH.C's run-list U/V (flatspans.inc.c:51-56, 136-156), which is measurement only;
- the unit-square UVs on 3D meshes (see section 5).

### Sky and solid-colour flats

- The pack marker check is RM:474.
- The map's sky picture is `art.Sky(metadata.skyTexture)`, which is simply `World(index)` (TEXC:247-250). It is therefore **transposed like a wall** and opaque. It is assigned to `Level->skytexture1/2` with `skyspeed` 0 (RM:196-213). No reason is given for transposing the sky picture specifically.
- Indices from 0xFF00 or 0x8000 upward become an 8×8 solid palette-colour texture (TEXC:415-431, 467-470). TEXH:234-237 says this rule is NOT VERIFIED from ROTH.C and rests on range evidence only.
- In the staged STUDY1 dump, many door-sector ceilings use 65400 or 65436, and sector 16 uses 65280 (palette 0) on both planes (`sample_static_STUDY1.csv`).

### Runtime flat swaps

- RT:2094-2259 swaps texture, shift, scale bits and "flip bits" together.
- RT:2186-2232 treats `flags2` bits 0x03/0x0C of the high byte as flat flip bits. The loader ignores those bits.
- RT:2194-2199 also re-uses the placeholder scale.

---

## 2. Mid-platforms (3D floors), RM:970-1147

- **Control sector.** One per slab. Its floor is the underside Z and its ceiling is the top Z (RM:1054-1057).
- **Textures.** `worldTex(topTexture)` and `worldTex(undersideTexture)`, both transposed like walls. Either one borrows the other if missing, and neither ever becomes sky (RM:1059-1068).
- **Scale.**
  - `topScale = 1/(2^((scales>>4)&3) * 2)` and `undScale = 1/(2^((scales>>2)&3) * 2)` (RM:1074-1075). The comment calls this the "Same 2^(v+1) base", while RM:1070 says "2^s world units per texel".
  - **RT:2053-2054 (ApplyPlatform) uses `1/2^s` instead.** That is half the loader's value, so any runtime re-apply doubles the texture density. **SELF-CONTRADICTORY.**
- **Offsets.**
  - The raw bytes are used directly as world units: `SetXOffset(ceiling, topShiftX)` and so on, with **no ×unitsPerTexel×0.5 and no Y negation** (RM:1078-1081; RT:2057-2060 is the same).
  - The ordinary sector flats *do* get the half-texel conversion and the Y negation, so the two paths disagree (compare RM:525-526).
- **Flip bits.** The `pad` byte is ignored by the loader. RT:2172-2173 writes flip bits into it.
- **Side of the slab.**
  - All four control lines carry `topTex` as their mid texture, at scale **1.0 on both axes** with offset 0 (RM:1106-1110). The justification given is "Realms has no side texture for a slab" (RM:986-987), which is unverified.
  - The renderer takes the texture and scale from `mastersd` (HWW:2079-2082). Vertically it is anchored to the model's top plane (`rover->top.model->GetPlaneTexZ`, HWW:2096-2098), and U runs along the target seg (HWW:2090-2092).
  - So a slab edge is drawn at **1 world unit per texel**: 2× denser than walls and 4-16× denser than its own top.
- **Flags.** `FF_EXISTS|FF_SOLID|FF_RENDERALL` (RM:1130-1131). A 3D floor covers the **whole** target sector.

---

## 3. Walls, RM:644-968

**Slots.**
- A one-sided wall gets its mid texture only. A two-sided wall gets upper and lower, plus a mid only if `FF_TRANSPARENT` is set (RM:708-709).
- A two-sided mid adjoining a door-leaf sector is dropped (RM:725-738).
- A two-sided mid uses the **masked** variant, where index 0 is a hole (RM:890 → `masked=twoSided`). Everything else is opaque unless the image carries `IT_TRANSLUCENT` (TEXC:324-326).

**Scale.**
- `unitsPerTexel` is 1 if `FF_HALF_PIXEL` is set and `FF_IMAGE_FIT` is not, otherwise 2. `scaleX = scaleY = 1/unitsPerTexel` (RM:764-768, 859).
- Horizontal scale is fixed and does not depend on the wall's length. The citation is renderer.c:4734, 13289, 4356, 4437 (RM:745-762, 776-795).

**Stored extent.**
- Parsed as `fitWord & 0x0FFF` when the record is extended, otherwise the whole `fitWord` (RAWH:127).
- **Not used** for walls. It is only counted (RM:796-799). `extentBitsAbove12` is also only counted (RM:1553-1554).
- **The door leaf does the opposite:** `sx = stored / (len * unitsPerTexel)` (RM:1436-1438), with the comment "the stored extent is authoritative horizontally" (RM:1430-1432). That is the rule RM:776-791 says caused the seams. **SELF-CONTRADICTORY.**

**U origin.**
- Nothing is set beyond the Doom default: each sidedef's U starts at 0 at its own start vertex, and `TexelLength` comes from `FinishLoadingLineDef`, i.e. the rounded line length (NOT VISIBLE; used at HWW:1378, 2092).
- RM:755-757 claims the original "cannot have seams" because of the masked wrap. With per-line U origins, the Doom build *will* show a pattern break at every face whose length is not a multiple of `texW / scale` (2·texW). Nothing makes U continuous across faces.

**Shifts.**
- Only on extended records, and zeroed under image-fit. `offX = shiftX`, `offY = shiftY` in texels, with `offY` negated under `FF_PIN_BOTTOM` (RM:812-819). The citation is renderer.c:5050/5061. Bytes are unsigned.
- GZDoom treats sidedef offsets as texels unless `LEVEL3_FORCEWORLDPANNING` is set (HWW:1593), and the loader never sets that flag.
- The runtime scroll/swap re-apply writes the same shift to all three parts and does not re-check image-fit (RT:2304-2327).

**Image-fit.**
- `sx = texW/wallLen` and `sy = texH/pieceHeight`, which stretches one copy across the whole piece (RM:860-871).
- The piece heights are computed from the two sectors (RM:830-852).
- The comment's own citation says image-fit makes "the extents become 2 * texture_dimension" (RM:759-762). Read with the fixed-`>>1` U path described in the same block, that describes a fixed 2 units/texel wrap at one copy, **not** a stretch to the wall's length. The citation and the code may disagree; the other reader should check renderer.c:13342-13359.

**Flip.**
- `FF_FLIP_X` sets `sx = -sx` (RM:878). The comment admits this is not what ROTH.C does.
- **TEXH:114-118 and TEXC:352-373 implement the "correct" in-pixel mirror (`World(…, flipped=true)`), but RM never passes `flipped`.** The worldTex lambda at RM:183-187 has no flip argument. So the pixel-mirror path is dead code, and the loader uses the method its own header calls wrong. **SELF-CONTRADICTORY.**
- Whether GZDoom renders a negative sidedef scale as a mirror lives in `FTexCoordInfo`, which is NOT VISIBLE.

**Pegging.**
- If either side has an upper piece, the line gets `ML_DONTPEGTOP` (RM:962). That anchors the upper piece's top to the front ceiling.
- If either face has `FF_PIN_BOTTOM`, the line gets `ML_DONTPEGBOTTOM`, OR-ed across both faces (RM:914, 963).
- RM:908-912 admits this is "NOT VERIFIED … least-evidenced".
- **A concrete defect follows from the engine code.** For a *lower* piece, GZDoom passes ceiling ref = back floor and `v_offset = frefz - crefz` (HWW:2855-2859). With peg set (HWW:1646-1647), the texture top lands at front-ceiling + H. So a pinned *lower* piece is anchored to the **front sector's ceiling**, not to the piece's bottom. The two only agree when the distance is a multiple of the texture height.
- For one-sided and two-sided mid pieces, `DONTPEGBOTTOM` does give bottom anchoring.
- Two-sided mids never get `ML_WRAP_MIDTEX`, so a transparent decal is drawn once and does not tile vertically.

**Fields that are ignored:**

| Field | Where it is ignored |
|---|---|
| Sector `textureMapOverride` (per-sector transparent-mid size and anchor) | RAWH:61-70; used only by an RT effect at RT:2783 |
| `FF_TRANS_UPLO` | counted only, RM:946 |
| `FF_EDGE_MAP` (parallax sky above wall) | counted only, RM:916-945 |
| The textureFit "repeat nibbles" | nothing reads them; ROTH_STATE.md:284-290 says they are unapplied and their source field is unknown |
| `TextureMap::unk` | never read |

**Engine edits in hw_walls*.cpp and r_defs.h** (none of them touch wall UVs):

- **`WALLF2_SKYWALL`** (RDEF:1355-1365, `side_t::Flags2` at RDEF:1448). HWW:2600-2607 draws a one-sided wall carrying it with `SkyLine`. **No staged code sets it.** RM:937-939 says drawing whole faces as sky was reverted.
- **Per-wall glow** (`side_t::glow`, RDEF:1368-1382; `ApplyWallOwnGlow`, HWW:40-81). This is lighting, and nothing Realms-related sets it.
- The wall UV code has the same shape and line positions the earlier engine report listed:
  - `SetWallCoordinates` (HWW:1365-1381, 1489-1490);
  - `DoTexture` pegging (HWW:1644-1650);
  - FF block (HWW:2086-2108);
  - `GetTexCoordInfo` (HWW:1593-1596).
  
  I found no Realms modification there. `hw_walls_vertex.cpp` has no Realms references.

---

## 4. Textures: from DAS image to engine texture

**Decoding.**
- The header is modifier, image_type, a width word and a height word, followed by w·h bytes (DASC:202-215).
- Animated entries take their flags from the entry, not the frame (DASC:281-282). Deltas go through the per-frame table (DASC:290-298), capped at 63 frames.
- An image pack yields **only one** sub-image (DASC:303-319). `IM_IMAGE_PACK` sub-selection is not implemented.

**Transpose.** Every path registers with `w = img.height`, `h = img.width` and column-major bytes (TEXC:345-346, TOBJ:202-203), which amounts to a transpose (a diagonal mirror, not a pure rotation). That applies to:
- walls;
- flats (the same `World()`);
- the sky;
- solid colours, where it makes no difference;
- sprites and mesh skins (`Sprite()`).

**The provenance contradicts itself:**
- TEXC:13-20 says "Wall and flat art is stored rotated … Sprites and model skins are NOT stored rotated".
- TEXH:135-147 and TOBJ:12-17 say sprites ARE rotated, and that "the handoff is wrong".
- ROTH_STATE.md:95-96 says "Flats transpose too. Sprites and model skins do not."
- **PY:53-56 says "Flats are NOT rotated."** The Python oracle's screenshots were taken with untransposed flats.
- The edge screenshots (section 7) show floorboards 90° off.

**Dimensions.** The texture is width = stored height and height = stored width. Nothing is rescaled. Solid colours are 8×8.

**Palette.**
- The pack palette, or a built-in default, is expanded from 6 to 8 bits with `(v*259+33)>>6` (DASC:26-27, 77-85). The comment calls it "the same expansion the original uses", which is an uncited claim.
- There are three translations per pack (TEXC:135-158, 191-208):

| Variant | Behaviour | Used by |
|---|---|---|
| Opaque | every index is written | default world texture |
| Keyed | index 0 has alpha 0 | two-sided mids, all sprites |
| Translucent | index 0 is a hole and indices ≥0x80 get alpha 128 | any image with `IT_TRANSLUCENT` |

- The 0.50 blend figure is *measured* from the DAS blend LUT (TEXC:102-107).
- Whether GZDoom *blends* alpha-128 texels on walls and flats, rather than alpha-testing them, is NOT VISIBLE.

**Colour key.**
- **`IT_ZERO_OPAQUE` is never consulted anywhere** (grep). `Image::ZeroIsTransparent()` (DASH:71) is dead code.
- The world path is opaque by default and keyed only by the caller's `masked` hint. TEXC:317-322 admits "Strictly, ROTH.C keys masking off the image alone and the hint should go".

**Sky.** The picture is the map's `skyTexture` (metadata +0x18). The marker is the pack header word at +0x22 (DASH:105-128). The loader never models the parallax behaviour (RM:207-209, 941-945). Both the 44-map census figures and the renderer.c citations in these comments are claims I cannot check.

**Animation.** 229 ms per frame (TEXC:69-76, TOBJ:68-70). It is explicitly described as UNVERIFIED and taken from the Python pipeline.

---

## 5. Sprites, objects and meshes (OBJ)

**Skipped objects:**
- `flags & 0x80` (OBJ:504-509, cited renderer.c:8035);
- `FAT_MONSTER` and `FAT_DIRECTIONAL` indirections, so **directional art is never drawn** (OBJ:548-565);
- empty entries.

**Art source.** `textureSource` & 3 → +4096 / +4352 in the map pack, or +0 / +256 in the shared pack (DASC:392-401).

**Scale.**
- `unitsPerPixel` is 2 by default. With modifier bit 0x80, s = `(imageType>>5)&3` gives s=0 → 1 and s=1,2,3 → 4, 8, 16 (TOBJ:99-108).
- The actor gets `Scale = (upp, upp)` (OBJ:622, 821). The cited source is renderer.c:6512-6525.

**Vertical placement.**
- The actor Z is the absolute object Z, re-set after spawning (OBJ:535, 828).
- Texture offsets are `(w/2, h)` to stand on Z, or `(w/2, 0)` for `IM_HANG` (TOBJ:217). The cited source is renderer.c:6549-6561.
- The modifier's low-nibble shift, `2*(mod&0xf)`, is computed but **deliberately not applied**, because applying it sank 68 STUDY1 props into the floor (OBJ:626-648, TEXH:53-61).
- The light byte is not applied (OBJ:712, 722).
- Pixel stretch is only applied to sprites carrying the ROLL or SQUAREPIXELS flags (HWS:1578-1583).

**Facing.**
- The object's yaw is `90 + roth_objectangle + (+1)·rot·360/256` (OBJ:102, 136, 147-154). The citation is renderer.c:6096 (`2*(rot+0x40) - view`).
- `renderType & 0x80` → `RF_WALLSPRITE`, whose quad spans yaw−90°, so its normal is the yaw (HWS:1635-1644). `flags & 0x10` → `RF_XFLIP`.
- Contradictions:
  - OBJ:77-82 and ROTH_STATE.md:101 say the object sense is *opposite* to the player's. The code uses +1.
  - OBJ:94-100 says the observed error was "a uniform 180", fixed by the quarter turn. OBJ:129-135 records that disagreement.
  - ROTH_STATE.md:280-283 still says to dial the angle in with the cvar.
- The +90 is cited only for the flat-quad path (renderer.c:6096). **It is also applied to meshes** (OBJ:536 goes to every `Pending`), with no mesh-specific citation. OBJ:67-69 cites render_world.c:637 only for the doubling.

**Directional frames.** All 16 rotations are the same texture (OBJ:268-270).

**Meshes.**
- Vertices are read as `(x, up, y)` into `FVector3(x, up, y)` at 1:1, with model scale 1 (OBJ:305-314, 423).
- OBJ:288-293 cites render_world.c:653-676 for the lack of scaling. **ROTH_STATE.md:291-296 says the 1:1 claim came from roth-editor, not ROTH.C**, and records two props (`DEMO[4123]`, `DEMO[4128]`) that are 542 units tall.
- UVs are a fixed unit square per face in corner order: U {0,0,1,1}, V {0,1,1,0}, with `flipV` applied (OBJ:330-335). OBJ:295-301 says outright that this cannot be verified: "Expect the texture ORIENTATION on a face to be wrong somewhere".
- `subTexture` is read but ignored (DASC:363).
- Faces are double-sided (OBJ:345).
- Mesh skins go through `Sprite()`, so they are transposed and keyed.
- Mesh texture ids at or above 0x1200 that match no colour base become palette colour 0 (TOBJ:156-167, e.g. DEMO mesh 4097 → 9600). The shared-pack fallback is not implemented.

---

## 6. Player and view

- `ForcedPlayerViewHeight = 2·playerHeight` (144 for 72) and `ForcedPlayerHeight = that + 10` (154) (RM:138-144; RAWH:150). So the **eye sits at the full doubled height**.
  - The citation is collision_physics.c:65.
  - The consumer of both fields is NOT VISIBLE in the staged files.
- PY:589-600 used height 72 with view `72·41/56` = 53. That is a different model.
- The player start uses Z = 0, so metadata `startZ` is ignored (RM:1592). The angle is `90 + rot·360/512` (RM:1593); the README confirms 128 → 180.
- `MaxClimb()` and `MinFit()` are defined but unused (RAWH:151-152). Step height stays the engine default.
- `remastatic.cpp:47` prints `maxClimb*2+1`, while RAWH:151 has `*2`.
- **FOV.** Nothing sets it, so it is the `fov` cvar default of 90 (p_user.cpp:199), or the HMD in VR.
- **Pixel aspect.** The loader never sets `pixelstretch` (g_level.cpp:2041 copies it from `info`).
  - Upstream GZDoom's default is 1.2, and the hardware view matrix stretches the world vertically by it.
  - That code, `hw_drawinfo`, is NOT VISIBLE, and so is the vrealms MAPINFO (ROTH_STATE.md:201-203). This is an **unresolved candidate 1.2× vertical distortion**.
  - `remastatic.cpp:44-45` hard-codes the "640x480 view" centre (320/240); REMAROTH itself sets nothing of the kind.
- **Lighting,** which affects the brightness comparisons:
  - Build light mode is forced on (RM:110-123).
  - `lightlevel = 255·(39+off)·2^shift/1984` (RM:399-418). The comment says it is derived; it ignores the original's `cap` term (ROTH_LIGHTING.md:37).
  - It does **not** use the DAS's own 32×256 shading table or the tint table (ROTH_LIGHTING.md:59-65).
  - The flash is not implemented.
  - ROTH_STATE.md:297-301 says lighting is "deliberately OFF". The code has it ON.

---

## 7. Visual defect catalogue

**Capture conditions.**
- The REMAROTH captures are 320×200 (16:10). The `cmp_*`, `edge_*` and `rug3_*` REMAROTH crops are 2× nearest-neighbour upscales of that, with visible 2-pixel blocks.
- The ROTHC captures are 640×480 (4:3), point-sampled, and much darker. I brightened copies to read them.
- `after_rug` and `after_scale` were not staged.
- Nothing staged records which code revision produced BEFORE, AFTER or X2.

### `ref_STUDY1_start` (ROTHC vs REMAROTH)

**Camera.**
- It is the same spot and heading: the long hall, a pillar pair left and right, the far door centred.
- The lateral position matches. The near-pillar offsets from screen centre are left:right ≈ 2.44 (ROTHC) against ≈ 2.49 (REMAROTH).
- **REMAROTH's horizontal FOV is about 16% wider**: the pillar offsets are ~0.86× ROTHC's.
- Vertically, the far door sits roughly symmetric about centre in ROTHC. In REMAROTH its bottom is further below centre than its top is above, which suggests a higher eye or a different vertical centre. The door's height:width is ≈ 2.2 in ROTHC against ≈ 1.7 in raw REMAROTH pixels. **These are eyeball measurements.**

**Brightness and colour.**
- ROTHC is mostly black: the side rooms beyond the pillars are unlit, and the shadows carry saturated purple/blue from the shading table.
- REMAROTH is evenly lit and brown-neutral. The side walls, wainscot and paintings are fully visible.
- REMAROTH textures are bilinear-smoothed; ROTHC's are hard pixels.

**Far door.**
- ROTHC draws a panelled double door with a pentagram.
- REMAROTH shows a black opening with only a thin vertical bar, so the door leaf is not visibly textured.

**Rug.**
- ROTHC has a green area and a blue area split by one long straight diagonal, plus a patterned border strip on the right edge.
- REMAROTH's rug is uniform navy with no diagonal and no border.

**Wood floor (right).**
- ROTHC's boards run *along* the hall.
- REMAROTH's run *across* it, a 90° orientation difference.

**Ceiling.** See `cmp_ceiling` below. There are black rectangles top-left and top-right.

**Chandelier.** ROTHC's is larger, lower and red-brown. REMAROTH's is smaller and gold.

**Statues.** Present in both. They are nearly invisible in ROTHC.

### `cmp_ceiling`

- **ROTHC** (top 288 rows of the same frame):
  - The central ceiling carries a fine fret/lattice pattern of roughly 7-8 cells across the aisle, with a circular rose under the chandelier.
  - Both side aisles show sloped purple-and-orange patterned surfaces receding to the vanishing point.
- **REMAROTH:**
  - The central ceiling shows ~1-2 large rectangular panels and one small square medallion. The pattern is **several times coarser**, consistent with ≥4 units/texel.
  - A light cross-beam spans the top.
  - The side-aisle regions are **solid black rectangles** (≈x20-135, y35-100 and x400-510, y35-240 in the 640-wide crop), exactly where ROTHC has textured surfaces.
- I cannot tell from the image whether those are sky surfaces, solid-colour flats (`0xFF00+n`), or geometry that is missing because walls are built floor-to-ceiling and `FF_EDGE_MAP` is unmodelled. The static dump shows door sectors with `0xFF78`/`0xFF9C` ceilings and one sector at `0xFF00` (palette 0).

### `cmp_rug` and `rug3_*` (same framing)

Dot pitch near the bottom rows, measured by eye with 10-px ticks:

| Capture | Dot pitch (px) | Grid |
|---|---|---|
| ROTHC | ≈21 | staggered |
| BEFORE | ≈17-18 | staggered |
| X2 | ≈35 | |
| AFTER | ≈65-70 | cells enclose square tiles |

- Correcting BEFORE for REMAROTH's ~16% wider FOV gives ≈20-21. **BEFORE's floor density matches the original. X2 is about 2× too coarse and AFTER about 4× too coarse.**
- If BEFORE was the Python-equivalent 2 units/texel and the rug sector's floor has s=2 (STUDY1 hall sectors with floor tex 120 are s=2 in the sample dump), then the current code's 2^(s+1) = 8 units/texel is the AFTER look. That is an inference, not something recorded.
- None of the REMAROTH versions reproduces:
  - ROTHC's green/blue diagonal split;
  - the border strip;
  - ROTHC's darkening toward the top.
- REMAROTH's near pillars sit about 25 px closer to centre in the crop, which is consistent with the FOV difference.

### `edge` (rug/wood boundary, bottom right)

- **ROTHC:** the boards run along the hall and converge on the vanishing point. A narrow patterned grey/orange border runs along the rug edge.
- **REMAROTH:**
  - The boards run horizontally across the view, i.e. **rotated 90°** (flats transposed).
  - There is no border strip. There is one isolated blue-and-orange patterned patch at about x130-200, y225-250: a fragment of a different texture at the sector edge, consistent with a thin border texture drawn at too coarse a scale or with a mismatched origin.
  - The wainscot wall at top right is lit; in ROTHC it is black.
- The pillar and rug-edge positions differ by the FOV change, so the camera is the same but the framing is not.

---

## 8. Every texturing and scaling decision

Verdict key:
- **CODE** = DERIVED-FROM-CODE-WITH-CITATION (a ROTH.C line is cited; the citation itself is unchecked)
- **MEAS** = MEASURED
- **GUESS** = GUESSED-OR-PROVISIONAL
- **CONTRA** = SELF-CONTRADICTORY

| # | Decision | Where | Stated source | Verdict |
|---|---|---|---|---|
| 1 | Flat scale = 1/(2^s·2) | RM:515,523-524; RT:1997-1999 | "placeholder … do not cite" (RM:483-514) vs "2^s" (RM:425) vs "measurement" (RT:1996) | CONTRA |
| 2 | Flat scale selector bits: floor 4-5, ceiling 2-3 | RAWH:101-102 | renderer.c:8967/8986; flatspans says it counts down | CODE / CONTRA |
| 3 | Flat offset ±sh·upt·0.5, Y negated | RM:525-526 | renderer.c:8966, 8985, 3415, 3172 | CODE; comment's world step is off by 2× (CONTRA) |
| 4 | Flats world-anchored, no rotation, no flip | RM:441-446, 519-522 | renderer.c:3415 vs 6768, 8962-8986 | CODE |
| 5 | Flat flags2 flip bits ignored at load but used at runtime | RM:557-561 vs RT:2186-2232 | "counted" (no counter exists) | CONTRA |
| 6 | Flats transposed like walls | TEXC:343-346 via RM:475 | ROTH_STATE.md:96; PY:55 says not rotated | CONTRA |
| 7 | Sky-marker index = pack header +0x22 → skyflatnum | RM:474-480; TEXC:238-245 | map_load.c:363, renderer.c:1518; 44-map census | CODE+MEAS |
| 8 | Missing flat art → sky | RM:480 | avoid HOM | GUESS |
| 9 | Sky picture = World(metadata+0x18), transposed, skyspeed 0 | RM:196-213; TEXC:247-250 | map_load.c:214, renderer.c:5414; transpose not justified | CODE / GUESS |
| 10 | Platform scale 1/(2^s·2) at load | RM:1074-1075 | "same as flats" | GUESS (inherits #1) |
| 11 | Platform scale 1/2^s at runtime | RT:2053-2054 | "rothmap.cpp:1018 is the original" | CONTRA with #10 |
| 12 | Platform shifts as raw world units, no Y negate | RM:1078-1081; RT:2057-2060 | none | GUESS / CONTRA with #3 |
| 13 | Platform side = top texture at scale 1.0 | RM:1106-1108 | "Realms has no side texture" | GUESS |
| 14 | Wall scale fixed 0.5, or 1.0 with half-pixel | RM:765-768 | renderer.c:4734, 13289 | CODE |
| 15 | Stored extent ignored for walls | RM:776-799 | same, plus PY dead code | CODE |
| 16 | Door-leaf sx = stored/(len·upt) | RM:1433-1441 | "stored extent authoritative" | CONTRA with #15 |
| 17 | Wall U origin = per-line Doom default | implicit; HWW:1378 | "original cannot have seams" | GUESS |
| 18 | Image-fit stretches one copy to piece W×H | RM:860-871 | renderer.c:8272, 13342-13359 ("extents = 2·texDim") | CONTRA? (citation reads as a fixed wrap) |
| 19 | Half-pixel ignored under image-fit | RM:765 | renderer.c:13342-13359 | CODE |
| 20 | Wall shifts = texels, offY negated under PIN_BOTTOM, zeroed under fit | RM:812-819 | renderer.c:5050, 5061, 13343 | CODE |
| 21 | FLIP_X → negative scale; pixel-mirror path unused | RM:878 vs TEXH:114-118, TEXC:352-373 | renderer.c:4735 | CONTRA |
| 22 | Upper piece → ML_DONTPEGTOP | RM:962 | "reasoned from Doom defaults" | GUESS |
| 23 | PIN_BOTTOM → ML_DONTPEGBOTTOM, OR-ed per line (a lower piece anchors to the front ceiling) | RM:914, 963 | "NOT VERIFIED" | GUESS |
| 24 | Two-sided mid only when FF_TRANSPARENT; masked; no wrap | RM:709, 890 | none cited | GUESS |
| 25 | Door-adjacent transparent mid dropped | RM:725-738 | consequence of the leaf model | GUESS |
| 26 | textureMapOverride, TRANS_UPLO, EDGE_MAP, repeat nibbles unapplied | RAWH:61-70; RM:945-946 | counted | GUESS (omissions) |
| 27 | WALLF2_SKYWALL engine flag exists but is never set | RDEF:1355-1365; HWW:2600 | reverted (RM:937-939) | (dead) |
| 28 | Opaque default; keyed only via the caller hint | TEXC:317-326 | "hint should go" | CONTRA (self-admitted) |
| 29 | IT_TRANSLUCENT → hole + alpha 128 for ≥0x80 | TEXC:80-121 | renderer.c:6288, 13278; blend LUT fit 0.50 | CODE+MEAS |
| 30 | IT_ZERO_OPAQUE never read | DASH:22, 71 | none | GUESS (omission) |
| 31 | Solid colour: 0xFF00/0x8000 bases → 8×8 | TEXC:415-431, 467-470; TEXH:221-239 | range census; "NOT VERIFIED" | MEAS |
| 32 | Palette 6→8 bit via (v·259+33)>>6 | DASC:26-27 | "same as original" (uncited) | GUESS |
| 33 | Animation frame time 229 ms | TEXC:76; TOBJ:70 | Python "looked right" | GUESS |
| 34 | Sprites and mesh skins transposed | TOBJ:200-203 | renderer.c:6505-6561 plus visual check | CONTRA (TEXC:19-20, ROTH_STATE.md:96) |
| 35 | Sprite index 0 always a hole | TOBJ:191-197 | "IT_ZERO_OPAQUE unused in retail objects" | MEAS |
| 36 | Sprite world scale 2 (or 1/4/8/16 by modifier) | TOBJ:99-108; OBJ:622 | renderer.c:6512-6525 | CODE |
| 37 | Sprite anchor bottom-centre / top for HANG | TOBJ:217 | renderer.c:6549-6561 | CODE |
| 38 | Nibble vertical shift not applied | OBJ:626-648 | STUDY1 Z statistics | MEAS (contradicts cited code) |
| 39 | Object yaw = 90 + rot·360/256, sense +1 | OBJ:102, 136, 147-154 | renderer.c:6096 | CODE; CONTRA with ROTH_STATE.md:101 and OBJ:94-100 |
| 40 | Same +90 applied to meshes | OBJ:536, 820 | none mesh-specific | GUESS |
| 41 | Fixed-angle → RF_WALLSPRITE; flags&0x10 → XFLIP | OBJ:655-664 | renderer.c:6095-6147 | CODE |
| 42 | Directional and monster art not drawn; 16 identical rotations | OBJ:548-565, 268-270 | not followed | GUESS (omission) |
| 43 | Mesh 1:1 units, (x, up, y), no mirror | OBJ:288-293; DASH:88-89 | render_world.c:653-676 vs ROTH_STATE.md:296 (from roth-editor) | CONTRA (provenance) |
| 44 | Mesh UVs = unit square per face; subTexture ignored | OBJ:330-335 | "cannot be verified" | GUESS |
| 45 | Image pack → first sub-image only | DASC:303-319 | none | GUESS |
| 46 | Eye = 2·playerHeight (144), body 154 | RM:138-144 | collision_physics.c:65 | CODE; PY uses 72/53 |
| 47 | Start Z = 0; MaxClimb/MinFit unused | RM:1592; RAWH:151-152 | none | GUESS (omission) |
| 48 | FOV / pixelstretch untouched | (absent) | none | GUESS (omission) |
| 49 | Build light, lightlevel formula | RM:110-123, 399-418 | renderer.c, obj3_owned.c:708; ignores cap and DAS table | CODE (partial) |

### Values that appear in two places with different numbers

- **Flat units per texel:**
  - 2^s: RM:425-426, RM:1070, RAWH:100, ROTH_STATE.md:102
  - 2^(s+1): RM:515, RM:1074-1075, RT:1997
  - 2^s: RT:2053-2054 (platforms)
  - fixed 2: PY:51
  - "counting down, 4−v": flatspans.inc.c:21-26
- **Flat shift step:** RM:433-440 says 2^s/2 world units; the code at RM:525 gives 2^s. Platforms use 1 world unit per shift unit (RM:1078).
- **Wall horizontal scale:** fixed 0.5 (RM:765) vs `stored/(len·upt)` for door leaves (RM:1438) vs `stored/(2·wall_len)` (PY:204-209, dead code).
- **Sprite and flat transpose:** yes (TOBJ, TEXH:135) vs no (TEXC:19-20; ROTH_STATE.md:96 for sprites; PY:55 for flats).
- **Object facing sense:** +1 (OBJ:102) vs opposite to the player (ROTH_STATE.md:101; OBJ:77-82).
- **Object origin:** 90° (OBJ:136) vs "uniform 180 error, fixed" (OBJ:94-100) vs a cvar to dial in (ROTH_STATE.md:280-283).
- **Player height/eye:** 154/144 (RM:140-141) vs 72/53 (PY:589-600). The oversize checks use 154 (OBJ:587, 679).
- **Max climb:** `maxClimb*2` (RAWH:151) vs `*2+1` (remastatic.cpp:47, sample dump 65).
- **Lighting state:** ON (RM:106-120) vs "deliberately OFF" (ROTH_STATE.md:297) and "placeholder, one-to-one" (ROTH_LIGHTING.md:187-191).
- **Door model:** a fixed 90° swing with no target point (RM:1165-1169, 1753-1762) vs "swing to a target point" (ROTH_STATE.md:104-105, 129-130).
- **0xFFFE sectors:** "the two-sided wall is not drawn" (RAWH:29) vs "second panel of a double door, leaf built" (RM:285-290). The log label at RM:1536 still says "OPEN QUESTION".
