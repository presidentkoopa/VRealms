# ROTH_NATIVE_HANDOFF.md
### For the engine coder: make REMAROTH read Realms of the Haunting natively
*Written 2026-09-25. Supersedes the conversion approach in ROTH_CONVERSION_PLAN.md.*

---

## 0. The job, in one paragraph

REMAROTH must open ROTH's own files (`ROTH.RES`, `M\*.RAW`, `M\*.DAS`) straight from the player's
install at map load and build the level in memory following ROTH's rules. **No Python converter,
no generated `.pk3`, no UDMF text, no Doom data in between.** The engine already has an unused
slot for a third map format; the loader goes there. The VR layer (hands, holsters, weapons,
RS_* mods) sits on top and doesn't change.

**Ground truth order when sources disagree:** ROTH.C (the original engine, byte-verified) beats
roth-editor (approximates in places) beats the Python pipeline and ROTH_CONVERSION_PLAN.md
(which has errors, listed in §2).

**Deep references, all in `docs/`** (every claim there has file:line references):
- `NATIVE_engine_scope.md`: REMAROTH's load and render paths, where ROTH hooks in
- `NATIVE_rothc_rules.md`: the original engine's exact rules (ROTH.C)
- `NATIVE_rotheditor_rules.md`: every ROTH file format, byte layouts, how roth-editor draws them

Source trees on this machine:
- ROTH.C: `E:\VRealms\tools\ROTH.C\roth_c\src\engine\`
- roth-editor: `E:\VRealms\tools\roth-editor\src\`
- beyond-the-ire docs: `E:\VRealms\tools\beyond-the-ire\file_documentation\`
- Install: `D:\SteamLibrary\steamapps\Common\Realms of the Haunting\ROTH\`

---

## 1. Do this first (10 minutes)

1. **VR doesn't start.** `openxr_loader.dll` needs `jsoncpp.dll`, which isn't in the output
   folder, so OpenXR fails to load and the engine falls back to flat mode (see
   `E:\VRealms\doomxr-log.txt`). Copy
   `build-dxr\vcpkg\installed\x64-windows\bin\jsoncpp.dll` → `build-dxr\RelWithDebInfo\`.
   Then add the missing line to `auto-setup-windows-vr.cmd` after line 120:
   `copy /Y "%BUILD%\vcpkg\installed\x64-windows\bin\jsoncpp.dll" "%OUT%\" >nul 2>&1`
2. **Sound fails** ("Sound init failed"). All OpenAL deps are present in the folder; probably
   `MSVCP140_ATOMIC_WAIT.dll` from an old VC++ runtime. Install the current VC++ 2015-2022 x64
   redistributable and retest. Unconfirmed.
3. Work on a branch. Log every engine change in `ROTH_ENGINE_CHANGES.md` under its existing rules
   (name features for what they do, additive, default-off, no per-game special-casing).

---

## 2. Corrections to the existing notes (read before touching anything)

These come from ROTH.C and invalidate parts of ROTH_CONVERSION_PLAN.md and `build_map.py`:

1. **The player is 144 units tall, not 72.** The original engine doubles `playerHeight`,
   `maxClimb` and `minFit` from the map metadata at load. Player: height 144, radius 28, step-up 65,
   minimum gap 96. The pipeline sized the player at 72, which is why the world looked twice too
   big ("doorways three times a person's height"). **Do not rescale the world; the `--scale` /
   SCALE_035 / SCALE_050 experiments are chasing this bug.** World units stay 1:1.
2. **Floors and ceilings are world-anchored**, like Doom flats. The "per-sector stretched floor"
   builder the plan found (`build_floorceil_vertex_records`, the 0x1000 authored-UV flag) draws 3D
   *object* faces, not world floors. `--flat-fit` is wrong; drop it.
3. **The `textureFit` nibbles do not apply to world walls or floors.** They are a 3D-object face
   field. Floors use a 2-bit scale in the sector flags byte (§5.3). Walls use the stored-extent
   path (§5.2). The "largest known gap" in the plan isn't one.
4. In the extended face texture record (top bit set) the stored extent is **12 bits**, not 15.
5. Player and object rotation units differ: **player 512 per turn, objects 256 per turn.** The
   pipeline treats the player start as 256 per turn; that's wrong.

---

## 3. Where it goes in the engine

Line numbers are from the current REMAROTH tree (see `NATIVE_engine_scope.md` for full context).

### 3.1 Detection: `src/p_openmap.cpp`
- There is a dead Build-map hook: `P_IsBuildMap` is a stub returning false (**37-40**), called at
  **147** and **357**. Add `P_IsRothMap(map)` beside it: test the RAW header (signature u16
  21079 at offset 0x0E, version 0x70 at 0x02), set a new `MapData::isRoth` flag (MapData is in
  `p_setup.h`), keep the reader.
- Map lookup today: lump named `mapname` (≤8 chars, **129**), `maps/<name>.wad` (**130**),
  `maps/<name>.map` (**132**). The `file:` prefix already opens a path on disk (**106-118**), but
  **117** dereferences `GetContainerReader()` without a null check; fix that.
- Recommended: a `roth_path` cvar (or `-rothpath`) pointing at the install's `ROTH\` folder. On
  `map STUDY1`, if the name is in `ROTH.RES`, open `<roth_path>\M\STUDY1.RAW` directly and
  remember its DAS pack name. Put this before the IWAD/PWAD id check (**259-266**).
- `GetChecksum` (**393-397**) reads Doom lumps; give ROTH maps an MD5 of the RAW file instead.

### 3.2 Loading: `src/maploader/maploader.cpp`
- The format branch is **2997-3021** (binary vs `ParseTextMap`). Make it three-way:
  `if (map->isRoth) LoadRothMap(map, missingtex); else if (!map->isText) ...; else ParseTextMap(...)`.
  `P_LoadBuildMap` (**73-76**) is a matching stub; replace it or add alongside.
- Put the loader in a new file, `src/maploader/rothmap.cpp`, and the pure file readers (no engine
  types) in `src/roth/` so they can be unit-tested outside the engine.
- **Model it on the UDMF loader, not the binary one.** UDMF fills arrays then allocates
  `Level->vertexes/sectors/extsectors` (`udmf.cpp` **2633-2643**) and builds lines/sides in
  `ProcessLineDefs` (**2381-2476**), calling `FinishLoadingLineDef` directly (**2468**). The binary
  path's `FinishLoadingLineDefs` (**1622**) needs `sidetemp` data you won't have.
- Nodes: set `ForceNodeBuild = true`. `FNodeBuilder` needs only vertexes, sides (with `sidenum`)
  and lines (**3097-3124**). Blockmap is generated when missing (**2581-2588**). Missing REJECT
  is fine (**2783-2797**).
- **Guard the Doom-only branches** keyed on `!HasBehavior && !isText` so ROTH maps skip them:
  translator (**2941-2960**), `LEVEL2_DUMMYSWITCHES` (**2984**), teleport TIDs (**3204-3205**),
  `SECF_FLOORDROP` (**1077**). Check `CheckCompatibility` (**2967**), `T_LoadScripts` (**2973**),
  `LoadStrifeConversations` (**2993**), `CheckNodes` (**3158**), `LoadLightmap` (**3196**) are
  empty-safe with no Doom lumps.

### 3.3 What the loader must fill (`src/gamedata/r_defs.h`)
- **Sector** (`sector_t`, **624**): `memset(0)` first (the RS light-trim fields at **741-761**
  depend on zero). Then copy the defaults from `maploader.cpp` **1051-1142** / `udmf.cpp`
  **1666-1700**: heights via `SetPlaneTexZ` + `floorplane.set(0,0,1,-h)` /
  `ceilingplane.set(0,0,-1,h)`; flats via `SetTexture(pos, FTextureID)`; `lightlevel`; scales and
  alpha = 1; `gravity=1`; `ZoneNumber=0xFFFF`; `terrainnum=-1`; `nextsec/prevsec=-1`;
  `SpecialColors=-1`; `friction/movefactor`; `ibocount=-1`; `heightsec=nullptr`;
  `e=&extsectors[i]`; `sectornum`.
- **Line** (`line_t`, **1700**): `v1`, `v2`, `sidedef[0/1]`, `ML_TWOSIDED` when two-sided,
  `alpha=1`, `portalindex=portaltransferred=UINT_MAX`, `special=0`. Push `linemap` for every line
  (`FinishLoadingLineDef` indexes it, **1555**). Call `AdjustLine()` then `FinishLoadingLineDef`.
- **Side** (`side_t`, **1355**): `sector`, `linedef`, textures via `side->SetTexture(part, FTextureID)`,
  offsets, **`SetTextureX/YScale(1)` for every part** (memset leaves 0, which breaks rendering),
  `ClearAlpha()`, `Flags`, `UDMFIndex`.
- **Vertices:** `vertex_t::set(x,y)` (**117-126**), plus one zeroed `vertexdata_t` each
  (`udmf.cpp` **2617**).
- **Things:** `MapThingsConverted` of `FMapThing`, fields as in `maploader.cpp` **1305-1322,
  1400-1409** (pos, angle, EdNum, info, flags, SkillFilter, ClassFilter=0xffff, Gravity=1,
  RenderStyle=STYLE_Count, Alpha=-1, Health=1, FloatbobPhase=-1). Or spawn actors directly.
- **Use texture IDs, not names.** Name lookup truncates to 8 chars (**180-186**, **2194-2196**)
  and falls back to a default with warnings (**141-165**).

### 3.4 Textures from DAS, in memory
- Not in the staged files: look in `src/common/textures/` (`texturemanager.*`, `gametexture.h`,
  `image.h`, `imagetexture.cpp`). Upstream GZDoom lets a custom `FImageSource` subclass hold 8-bit
  indexed pixels + palette and be registered with `TexMan.AddGameTexture(MakeGameTexture(...))`
  without any lump. Verify in this tree.
- Wall art is stored rotated 90° (§5.1). Rotate it once inside the image source, then the renderer
  needs no change for that.
- Palette-colour surfaces: generate one small solid texture per palette index used.
- Name textures uniquely per pack (`DEMO:0012` vs `DEMO3:0012` are different images) or keep
  them ID-only.

### 3.5 Walls in the renderer (`src/rendering/hwrenderer/scene/hw_walls.cpp`)
- U: `SetWallCoordinates` (**1365**): `l_ul = TextureOffset`, `texlength = sidedef->TexelLength`
  (**1378-1381**), `u = l_ul + texlength * frac` (**1489-1490**). V is world-Z anchored
  (**1415-1416, 1459-1460**).
- **ROTH's stored extent fits without renderer changes:** set `side->TexelLength` (uint16,
  r_defs **1423**) to ROTH's stored extent *after* `FinishLoadingLineDef` overwrites it. Bake 2
  units per texel, shifts and flips into the per-part scale/offset (r_defs **1398-1401**).
- Only if that can't express something (e.g. DRAW_FROM_BOTTOM with world-Z anchoring when planes
  move): add a default-off explicit-UV hook. Narrowest version: `bool explicitUV; float u[2], v[2];`
  on `side_t::part`; pass the part index into `SetWallCoordinates` (declaration
  `hw_drawstructs.h` **262**, callers **1649** and **1893**); override after **1490**; skip
  `CheckTexturePosition` (**1525-1590**). `BuildFFBlock` (**2088-2106**) needs the same for
  platform sides.

### 3.6 Floors (`hw_flats.cpp`)
- Flat UVs come from a per-sector transform (offset, scale, angle, texture size relative to 64)
  built at **68-98**, set by `SetPlaneTextureRotation` (**511, 540**). ROTH floors are
  world-anchored with per-sector scale, shift and flip, so this should be enough. The one known
  risk: ROTH maps world X to image rows (a transpose). If the existing transform can't express it,
  pre-transpose flat images in the image source (same trick as walls).

### 3.7 Mid-platforms → 3D floors
- In memory: `extsector_t::XFloor.ffloors` (r_defs **575-581**). Created by `Spawn3DFloors` /
  `Set3DFloor` (maploader.h **238-239**, called at maploader.cpp **3189**), defined in
  `p_3dfloors.cpp` (upstream has `P_Add3DFloor(sec, model, master, flags, alpha)`).
- ROTH platforms cover their whole sector, so one 3D floor per sector with a platform maps 1:1.
- Each needs a **model sector** (an extra sector with no lines, added to `Level->sectors` before
  `CreateVBO`) and a **master line with a sidedef**: the renderer dereferences
  `rover->master->sidedef[0]` unconditionally (hw_walls **2032**). Create them after
  `Spawn3DFloors` and before `CreateVBO` / `P_Recalculate3DFloors` (**3232-3239**).

### 3.8 Sprites (`hw_sprites.cpp`, `HWSprite::Process` at 1022)
- Size: actor `Scale` (**1099, 1577**). ROTH = 2 units per pixel → Scale 2, square pixels.
- Vertical anchor: `z1 = z - r.top` (**1588-1589**), `r` from the texture's sprite offsets. A top
  offset of 0 hangs the sprite down from its anchor.
- Fixed-angle (wall-mounted): `RF_WALLSPRITE` uses the actor's yaw (**1635-1644**).
- Art without sprite-name lumps: set `thing->picnum`; it overrides sprite lookup (**1036, 1424**).

---

## 4. ROTH file formats (summary; full layouts in `NATIVE_rotheditor_rules.md` Part A)

All little-endian unless noted.

- **ROTH.RES** (text): `key=value` lines (`version`, `snd`, `das2=m\ademo` = the shared sprite
  pack), then `maps { <map> <daspack> ... }`, e.g. `m\study1 m\demo`. Paths are relative to the
  `ROTH\` folder; in this install both RAW and DAS files live in `ROTH\M\`. 44 maps use 5 packs
  (DEMO, DEMO1-4); ADEMO is loaded for every map.
- **RAW header** (0x1E): 15 u16: verticesOffset, version (0x70), sectorsOffset, facesOffset,
  faceTextureMapsOffset, mapMetadataOffset, verticesOffsetRepeat, signature (21079),
  midPlatformsOffset (0 = none), section7Size, verticesSectionSize, objectsSectionSize, footerSize,
  commandSectionSize, sectorCount.
- **Sector** (0x1A): s16 ceilingHeight, s16 floorHeight, u16 unk, u16 ceilingTex, u16 floorTex,
  u8 flags (`textureFit` in older notes; holds floor/ceiling scale, candle, lightning), u8 light,
  s8 overrideHeight, u8 faceCount, u16 firstFaceOffset (absolute), u8 ceilShiftX, ceilShiftY,
  floorShiftX, floorShiftY, u16 floorTriggerID (0xFFFD/E/F = door, 65534 = invisible walls),
  u16 flags2 (flips), u16 platformOffset (0 = none).
- **Face** (0x0C): u16 v1Offset, u16 v2Offset (relative to vertices), u16 textureMapOffset,
  u16 sectorOffset, u16 sisterFaceOffset (0xFFFF = one-sided), u16 collisionFlags.
- **Face texture map** (0x0A, +4 if extended): u16 fitWord (bit 15 = extended), u16 midTex,
  u16 upperTex, u16 lowerTex, u8 flags, u8 unk; extended adds u8 shiftX, u8 shiftY, u16 faceID
  (used by commands).
- **Mid-platform** (0x0E): u16 undersideTex, s16 undersideZ, u8 shiftX, shiftY, u16 topTex,
  s16 topZ, u8 shiftX, shiftY, u8 scales (bits 2-3 underside, 4-5 top), u8 pad.
- **Metadata** (0x1C): s16 startX, startZ, startY, rotation; u16 moveSpeed, playerHeight,
  maxClimb, minFit, shadeLevel(unk0x10); s16 candleGlow; u16 lightAmbience, tintFlag(unk0x16),
  skyTexture, unk.
- **Vertices:** 8-byte header (u16 size, 8, 0, count), then 0x0C each: 4 unused u16, s16 x, s16 y.
  (roth-editor subtracts 65536 from y > 20000 on RAQUIA2 only; check whether that's needed.)
- **Objects:** u16 section size, one u16 offset per sector (0 = none); at each: u8 count, u8 count,
  then 0x10 each: s16 x, s16 y, u8 texIndex, u8 texSource, u8 rotation, u8 flags, u8 light,
  u8 renderType, s16 z, u16, u16.
- **Commands:** "3u" header, 15 trigger-category (offset, count) pairs, entry points, commands of
  u16 size, u8 modifier, u8 opcode, u16 next (1-based), args.
- **DAS:** "DASP" header (68 bytes; `sky_index` at byte 34, palette offset at 12, FAT at 8);
  FAT entries u32 offset, u16 size, u8 flags1, u8 flags2; palette = 768 bytes of **6-bit** VGA
  (`(v*259+33)>>6` to 8-bit), then 2 bytes, then **322 shade tables × 256** (needed for lighting).
  Palette offset 0 = built-in default palette (ADEMO uses it; roth-editor `das.gd` 48-67).
  Image header: u8 modifier (bit 4 hang, bit 6 image pack, bit 7 half size), u8 imageType
  (bit 0 animated, 1 palette-zero-opaque, 2 translucent, 3 mirror, 7 3D object), u16 w, u16 h.
  ADEMO entries have a 4-byte prefix before the header. Entry types (plain, animated type 1 & 2,
  image pack, directional, monster, 3D object) and their decoding: `NATIVE_rotheditor_rules.md` A3.
  Map objects index art at `texIndex + 4096` (source 0), `+4096+256` (1), ADEMO `texIndex` (2),
  ADEMO `+256` (3).
- **Transparency:** index 0 is transparent unless palette-zero-opaque; with the translucent bit,
  indices 128-255 are blended.

---

## 5. ROTH's rules the engine must follow (from ROTH.C; formulas in `NATIVE_rothc_rules.md`)

### 5.1 Coordinates and units
- +X right, +Y forward (away) at view angle 0, Z up. Right-handed, same top-down layout as Doom.
  roth-editor's X negation is a Godot artefact; don't copy it.
- Player angle: 512 per turn, counter-clockwise, 0 = facing +Y. Doom angle ≈
  `90 + A*360/512`. **Verify on STUDY1's start.**
- Object rotation: 256 per turn, doubled to the 512 frame; facing measured clockwise from +Y.
  Doom angle ≈ `90 - rot*360/256`. **Verify on a fixed-angle picture.**
- Player: height 144, radius 28, step 65, min gap 96 (metadata values doubled).
- Wall art stored transposed: the stored "width" is the on-screen height; along-wall picks rows.

### 5.2 Walls
- Pieces: one-sided = mid texture floor→ceiling, anchored at the ceiling. Two-sided upper = upper
  texture, this ceiling→neighbour ceiling, anchored at this ceiling. Two-sided lower = lower
  texture, neighbour floor→this floor, anchored at the top of the piece. Transparent mid (flag
  bit 0) = mid texture in the opening, anchored at the opening top; with bit 3 and a nonzero
  sector `overrideHeight`, it's `4*|ovr|` tall, hanging from the top if ovr > 0, standing on the
  bottom if ovr < 0.
- U (stored-extent path, IMAGE_FIT clear): `U = t*extent/2 + shiftX` texels across the face, i.e.
  2 world units per texel when extent = wall length. Extent = `fitWord & 0x0FFF` if bit 15 set
  (shifts present), else `fitWord`. FLIP_X (bit 1) mirrors. HALF_PIXEL (bit 5) = 1 unit per texel.
- V: `(anchorZ - z)/2 + shiftY`. DRAW_FROM_BOTTOM (bit 7) aligns to the bottom, shift subtracted.
- IMAGE_FIT (bit 2): exactly one copy of the texture across the piece, shifts ignored.
- EDGE_MAP (bit 6): parallax sky drawn above the wall (sky texture = metadata skyTexture).
- Texture index == DAS `sky_index`: draw nothing on walls; open sky on ceilings.
- Flat colour: ROTH.C draws index `0xFFxx` as palette colour `xx`. roth-editor uses
  `65535 → 255, ≥32768 → value-32768`. Follow ROTH.C; check where they disagree.
- Sector `floorTriggerID == 65534`: two-sided walls in it are invisible.

### 5.3 Floors and ceilings
- World-anchored. Scale `s` = sector flags bits 2-3 (ceiling), 4-5 (floor); `2^s` world units per
  texel (s=1 is the normal case = 2 units).
- Shifts: one step = half a texel. Flips: flags2 high byte bits 0-1 floor X/Y, 2-3 ceiling X/Y;
  mirror about the world axis.
- World X drives image rows, world Y drives columns (a transpose). Signs unconfirmed: A/B in DOSBox.
- 256×256 opaque textures may draw at twice the density. Unconfirmed: A/B in DOSBox.

### 5.4 Mid-platforms
- Underside drawn with ceiling rules, top with floor rules. Side walls: a face whose neighbour has
  a platform draws its mid texture between the neighbour's underside and top.
- Collision: pass under if `underside − floor ≥ minFit`; step onto if `top − feet ≤ maxClimb` and
  `ceiling − top ≥ minFit`.

### 5.5 Objects
- Z is **absolute** (convert to height above floor for the engine).
- Sprite size: 2 world units per pixel. Half size when modifier bit 7 (with an optional power-of-two
  multiplier; see §5 of the ROTH.C report).
- Standing: bottom at `z − 2*(mod & 0xF) − yoff`. Hanging (modifier bit 4): top at
  `z + 2*(mod & 0xF) + yoff`.
- Flags bit 4 (object `+7 & 0x10`): horizontal flip.
- renderType bit 7: fixed plane perpendicular to the object's facing (wall-mounted). Otherwise a
  billboard turning around the vertical axis.
- Multi-angle sprites (8 or 16 views): ROTH's frame-selection formula is in the ROTH.C report §5.
  Use it rather than RAW.md's direction numbering.
- Object light: `light ? light − 0x80 + sectorLight : 0` (0x80 = same as sector).
- 3D props: vertex stride 0x10 (x, up, y), 1 mesh unit = 1 world unit, rotated by the object's
  rotation; per-face texture from the pack (packs pick a sub-image), `≥ 65280` = palette colour.
- Collision size from the DAS table at header offset 0x24, indexed by texture.

### 5.6 Doors (needed early because they change geometry at load)
- At load the original runs `fixup_raw_sectors_after_load`: every sector with `+0x14 == 0xFFFD`
  gets its ceiling saved and set to its floor (door closed). Door logic: ROTH.C `doors.c`.

### 5.7 Lighting
- Pixel = `shadeTable[shade][texel]`, shade 0-31 (31 = black), tables from the DAS palette block.
  Sector light byte 0x80 = neutral. Candle sectors (flags bit 1) and lightning (bit 6) have their
  own ramps. First pass: map sector light to engine lightlevel. Faithful pass: use the DAS shade
  tables as a colormap.

---

## 6. Build order, with "done when" tests

Reference numbers from the Python survey (`roth_pipeline/tools/survey_all.py`), which stays useful
as a test oracle: **44 maps, 16,906 sectors, 82,210 faces, 5,324 objects, 5,531 commands, 2,299
mid-platforms.** STUDY1: 1,076 vertices, 507 sectors, 2,454 faces = 1,608 lines (846 sister pairs
merged), 274 objects.

| Phase | Build | Done when |
|---|---|---|
| **1. Readers** | `src/roth/`: RES, RAW, DAS readers, no engine types. Small test exe. | Parses all 44 maps; totals match the numbers above exactly. |
| **2. Geometry** | Detection + `LoadRothMap`: vertices, sectors, lines (merge sister faces: one line, two sides), player start, 144-unit player. Flat grey textures. | `map STUDY1` loads from the install with no pk3 and no crash; all 44 maps load; line count for STUDY1 = 1,608. Walk around; door heights look human-scale. |
| **3. Textures** | DAS image source, walls rotated back, palette-colour textures, wall rules §5.2, floor rules §5.3, sky. | STUDY1 side by side with DOSBox (same spot, same angle): walls and floors line up. |
| **4. Objects** | Sprites (size, hang, flip, fixed-angle), 3D props, collision sizes. | The Study's furniture and paintings sit where DOSBox has them, right size, right facing. |
| **5. Platforms** | Mid-platforms as 3D floors. | TOWER1 and TGATE1F (338 / 251 platforms) walkable. |
| **6. Doors + lighting** | Door fixup + `doors.c` logic; sector light, candle, lightning. | Doors open; STUDY1 light matches DOSBox roughly. |
| **7. Commands** | Port `raw_commands.c` (6,000+ lines, opcode semantics in beyond-the-ire `RAW_commands.md`, roth-editor `opcodes.gd`). | Triggers in STUDY1 fire (doors, dialogue, spawns). |
| **8. Standalone** | Minimal base game definition so the engine boots without `doom2.wad`. | Runs with only REMAROTH files + the player's ROTH install. |

After phase 3, retire `build_map.py` for geometry. Keep `parse_raw.py`, `extract_das.py` and
`survey_all.py` as cross-checks.

---

## 7. Don'ts

- Don't translate to UDMF or any Doom format as an intermediate step.
- Don't port ROTH.C's renderer: it's a 1996 software renderer and can't do stereo VR. Port its rules.
- Don't copy roth-editor where it approximates: convex-hull floors, halving 256-wide floor
  textures, `textureFit` bits on walls, single frozen frame for animated/directional art, 3D prop
  faces textured from the level pack even for ADEMO objects.
- Don't rescale the world to fix scale; fix the player (§2.1).
- Don't ship or cache anything derived from the game files outside the player's machine.
- Don't hardcode 72/144: read `playerHeight` per map (three maps use 64: ABAGATE2, AQUA1, DOPPLE)
  and double it.

## 8. Still open (settle by A/B against the original in DOSBox)

- Floor texture axis signs, and whether 256×256 opaque floors draw at double density.
- Exact Doom angle conversions for the player start and object facing (§5.1).
- What reads `candleGlow`, and the enemy collision mask (not in ROTH.C's staged files).
- Wall flat-colour thresholds where ROTH.C and roth-editor disagree.
