# REMAROTH: scoping a native ROTH .RAW/.DAS map loader

All paths are relative to `src/`. I only read files, nothing was modified. "Not visible" means the code lives in a file that was not staged.

## 1. Load sequence, format branch, node building, map lookup

**Call chain.** `P_SetupLevel` (p_setup.cpp:625) runs these in order:
1. `P_FreeLevelData()` (p_setup.cpp:670).
2. `P_OpenMapData(Level->MapName, true)` (p_setup.cpp:672).
3. `map->GetChecksum(Level->md5)` (p_setup.cpp:682).
4. `MapLoader loader(Level); loader.LoadLevel(map, ...)` (p_setup.cpp:692-693).

**`MapLoader::LoadLevel`** (maploader/maploader.cpp:2927) runs in this order:
- Behavior or translator selection, and `maptype` (2935-2965).
- `CheckCompatibility(map)` (2967), `T_LoadScripts` (2973), `LoadStrifeConversations` (2993).
- **The format branch** (2997-3021):
  - Binary: `LoadVertexes` → `LoadSectors` → `LoadLineDefs`/`LoadLineDefs2` → `LoadSideDefs2` → `FinishLoadingLineDefs` → `LoadThings`/`LoadThings2`.
  - UDMF: `ParseTextMap` (3020).
- `CalcIndices` (3023), `PostProcessLevel` (3024), `LoopSidedefs(true)` (3026).
- Nodes: load ZNODES, GL, or binary nodes (3031-3092). Otherwise build them with `FNodeBuilder` (3103-3129).
- `CheckNodes` (3158), `LoadBlockMap` (3163), `LoadReject` (3165), `GroupLines` (3166), `FloodZones`, `SetRenderSector`, `FixMinisegReferences`, `FixHoles` (3167-3170).
- `CreateSections` (3182), `SpawnSlopeMakers`/`CopySlopes` (3185-3186), **`Spawn3DFloors`** (3189), `SpawnThings` (3191), `LoadLightmap` (3196).
- `SpawnSpecials` (3213), `InitRenderInfo` (3230), `CreateVBO` (3232), `P_Recalculate3DFloors` (3238), `PO_Init` (3246), `DoomLevelMesh` (3251).

**Where UDMF and binary are told apart.** The choice is made in `P_OpenMapData`. It sets `map->isText` when the lump after the map label is `TEXTMAP` (p_openmap.cpp:163, 195 for lumps in a wad; 278-280 for a map wad). `LoadLevel` only tests `map->isText` and `map->HasBehavior`.

**A precedent for a third format.** There is a dead Build-map hook. `P_IsBuildMap` is a stub that returns false (p_openmap.cpp:37-40). It is reached when a map container is not an IWAD or PWAD (p_openmap.cpp:353-361) and for a single-lump map (147). `P_LoadBuildMap` is a matching stub (maploader.cpp:73-76).

**Cleanest place for a ROTH branch:**
- **Detection.** Replace or extend the `P_IsBuildMap` calls (p_openmap.cpp:147, 357) with a `P_IsRothMap(map)` check. It should test the RAW header, set a new `MapData::isRoth` flag and keep the reader in `MapLumps[0]`. `MapData` is declared in p_setup.h, which is not visible.
- **Loading.** In `LoadLevel`, make 2997-3021 a three-way branch: `if (map->isRoth) LoadRothMap(map, missingtex); else if (!isText) ...; else ParseTextMap(...)`.
- **Model to copy.** Base `LoadRothMap` on `UDMFParser`, not on the binary path. UDMF builds `Parsed*` arrays and then allocates `Level->vertexes`, `sectors` and `extsectors` (udmf.cpp:2633-2643). It creates lines and sides with `ProcessLineDefs` (udmf.cpp:2381-2476), which calls `FinishLoadingLineDef` directly (2468). The binary `FinishLoadingLineDefs` (maploader.cpp:1622) depends on `sidetemp` data that a native loader would not have.
- **Other `!HasBehavior && !isText` branches to guard.** These would otherwise treat a ROTH map as a Doom map: translator load (2941-2960), `LEVEL2_DUMMYSWITCHES` (2984), `TranslateTeleportThings` (3204-3205), and `GetChecksum`, which reads the Doom lumps `ML_LABEL/THINGS/LINEDEFS/...` (p_openmap.cpp:393-397).

**Node building.** Yes, the node builder runs on its own when nodes are missing:
- With no ZNODES and empty SEGS/SSECTORS/NODES, it tries GL nodes. If that fails it sets `ForceNodeBuild = true` (maploader.cpp:3083-3089).
- A ROTH branch can simply set `ForceNodeBuild = true`.
- `FNodeBuilder` needs only vertexes, sides (with `sidenum`) and lines (3097-3124).
- The blockmap is generated when the lump is empty or `ForceNodeBuild` is set (`LoadBlockMap`, maploader.cpp:2581-2588).
- A missing REJECT just resets the reject matrix (2783-2797).

**Finding a map by name.** `P_OpenMapData` (p_openmap.cpp:102) tries these:
- A lump named `mapname`, only if the name is 8 characters or fewer (129).
- `maps/<name>.wad` (130-131).
- `maps/<name>.map` (132-133). The newest of these wins (135, 244).

**Opening a file on disk.** The `file:` prefix already opens a path outside any wad or pk3 (106-118) through `FResourceFile::OpenResourceFile`.
- Risk: the code then calls `map->resource->GetContainerReader()` without a null check (117).
- Whether `OpenResourceFile` accepts a non-archive `.RAW`, for example as a single-lump file, is not visible here.
- The `maps/*.map` path passes `containeronly=true` (255), which may reject a raw blob.
- The ROTH detector should therefore sit before the IWAD/PWAD id check (259-266), and the null check must be added.

## 2. Fields a native loader must fill

The references are the binary defaults (`LoadSectors`, maploader.cpp:1051-1142) and the UDMF defaults (udmf.cpp:1666-1700).

**Sector** (`sector_t`, r_defs.h:624):
- Start with `memset(0)`. This is required: the RS light-trim fields depend on zero (r_defs.h:741-761).
- Setup: `e = &extsectors[i]`, `Level`, `sectornum`.
- Heights: `SetPlaneTexZ(floor/ceiling)` plus `floorplane.set(0,0,1,-h)` and `ceilingplane.set(0,0,-1,h)` (maploader.cpp:1078-1081).
- Flats: `sector->SetTexture(pos, FTextureID)` (via `MapLoader::SetTexture`, maploader.cpp:178-208).
- Light: `lightlevel`.
- Defaults to set:
  - Scale and alpha to 1 (1100-1105).
  - `gravity=1`, `ZoneNumber=0xFFFF`, `terrainnum=-1`.
  - `seqType`, `nextsec/prevsec=-1`.
  - `SpecialColors` = -1, `AdditiveColors` = 0.
  - `Colormap.LightColor` / `FadeColor` (1116-1130).
  - `friction` / `movefactor`, `ibocount=-1`, `heightsec=nullptr`.
- Tag: `Level->tagManager.AddSectorTag`.
- The sector's `Lines` list is built later by `GroupLines`.

**Two-sided wall** (`line_t`, r_defs.h:1700; `side_t`, r_defs.h:1355):
- Line fields: `v1`, `v2`, `sidedef[0]`, `sidedef[1]`, `flags |= ML_TWOSIDED`, `alpha=1`, `portalindex = portaltransferred = UINT_MAX`, `special=0` (udmf.cpp:861-866).
- Then call `AdjustLine()` and `FinishLoadingLineDef`. That sets `frontsector/backsector` and `side->TexelLength` (maploader.cpp:1547-1570).
- **Push `linemap`** for every line. `FinishLoadingLineDef` indexes it (1555).
- Each side needs:
  - `sector` and `linedef`.
  - `textures[top/mid/bottom].texture` via `side->SetTexture` (maploader.cpp:169).
  - `SetTextureX/YOffset`.
  - **`SetTextureX/YScale(1)`**: memset would leave these at 0.
  - `ClearAlpha()`, `Flags`, `UDMFIndex` (maploader.cpp:2170-2177).

**Blocking one-sided wall:** the same, with `sidedef[1]=nullptr`, a mid texture and optionally `ML_BLOCKING`. A null back sector already blocks movement.

**Vertices:** `vertex_t::set(x,y)` (r_defs.h:117-126). Push a zeroed `vertexdata_t` per vertex, as UDMF does (udmf.cpp:2617).

**Things:** fill `MapThingsConverted` with `FMapThing` records.
- Fields to set:
  - `pos`, `angle`, `EdNum`.
  - `info = DoomEdMap.CheckKey(EdNum)`.
  - `flags` (MTF_SINGLE/COOP/DM), `SkillFilter`, `ClassFilter=0xffff`.
  - `Gravity=1`, `RenderStyle=STYLE_Count`, `Alpha=-1`, `Health=1`, `FloatbobPhase=-1` (maploader.cpp:1305-1322, 1400-1409).
- A player start is an `EdNum` that DoomEdNums maps to player 1 start.
- `SpawnThings` (1420) calls `Level->SpawnMapThing`. That function is not visible here.

## 3. Registering textures at runtime

The texture manager and texture classes are **not visible in these files**. The only calls seen are lookups: `TexMan.CheckForTexture` (maploader.cpp:141, 187; p_setup.cpp:260), `TexMan.GetGameTexture` and `TexMan.GetRawTexture` (hw_walls.cpp:2749-2750).

Look in `src/common/textures/`:
- `texturemanager.h/.cpp`: `FTextureManager::AddGameTexture`, `CheckForTexture`.
- `gametexture.h`: `FGameTexture`, `MakeGameTexture`, `SetOffsets`, `SetScale`.
- `image.h` / `imagetexture.cpp`: `FImageSource` and the pixel methods such as `CreatePalettedPixels` and `CopyPixels`.

In upstream GZDoom, a custom `FImageSource` subclass holding 8-bit indexed data plus a palette can be wrapped and registered at load time without any lump. Treat that as the approach to verify, not a confirmed fact about this tree.

Two useful consequences:
- Wall textures stored rotated 90 degrees can be rotated back once, inside that image source, so the renderer needs no change.
- Flat palette-colour surfaces can be 1x1 generated textures, one per palette index. An untextured flat is dropped (hw_flats.cpp:602-603). An untextured wall only becomes `RENDERWALL_COLOR` in one path (hw_walls.cpp:2143).

## 4. Wall UVs

**Where U and V are computed.** `HWWall::SetWallCoordinates` (hw_walls.cpp:1365):
- U: `l_ul = FloatToTexU(TextureOffset(t_ofs))`, then `texlength = FloatToTexU(sidedef->TexelLength)` (1378-1381). Then `tcs[..].u = l_ul + texlength * glseg.fracleft/fracright` (1489-1490).
- V: `FloatToTexV(texturetop - z)` per corner (1415-1416, 1459-1460). V is anchored to world Z.

**Scale per part.** `GetTexCoordInfo` passes per-part `xScale/yScale` into `FTexCoordInfo::GetFromTexture` (1594-1597). `FTexCoordInfo` itself is not visible.

**Pegging and offsets by wall part:**
- Top, bottom and one-sided: `DoTexture` computes `floatceilingref = ceilingref + RowOffset(yoffset)`, plus the pegging adjustment (1646-1647).
- Two-sided mid: `DoMidTexture` handles `ML_DONTPEGBOTTOM` (1722-1731) and the wrap/clamp decisions (1841-1870).
- Peg flags are decided in `Process` (2482-2483).
- 3D-floor sides compute their own UVs in `BuildFFBlock` (2088-2106).
- `CheckTexturePosition` renormalizes by whole texture units (1525-1590).

**Vertex upload.** hw_walls_vertex.cpp copies the four corner `tcs` and linearly interpolates both u and v along edges and height splits (35-58, 74-97, 120-173, 190-193). **Arbitrary corner UVs therefore survive, including a rotated mapping.**

**ROTH rules that need no renderer change:**
- Setting `side->TexelLength` to ROTH's stored extent, after `FinishLoadingLineDef` overwrites it, gives an extent independent of wall length. It is a `uint16_t` (r_defs.h:1423).
- "textureFit" repeat counts and 2 units per pixel can be baked into the per-part `xScale/yScale/xOffset/yOffset` (r_defs.h:1398-1401) at load.
- V stays world-anchored, so moving planes will slide the texture.
- A two-sided mid texture does not tile vertically unless `ML_WRAP_MIDTEX` is set.

**Narrowest default-off hook:**
- Add `bool explicitUV; float u[2], v[2];` to `side_t::part`, holding UVs at the part's left/right and top/bottom.
- Give `SetWallCoordinates` a trailing `int texpos = -1` (declaration at hw_drawstructs.h:262). Pass it from `DoTexture` (1649) and `DoMidTexture` (1893). `type` is set after the call in `DoMidTexture` (1903-1915), so it cannot be used.
- After line 1490, when set, overwrite U with `lerp(u0,u1,frac)` and V with the corner's fraction between the unclipped part top and bottom.
- Skip `CheckTexturePosition` for these walls.
- `BuildFFBlock` needs the same override for mid-platform sides.

## 5. Flat UVs

- Flats draw indexed triangles from a pre-built vertex buffer: `state.DrawIndexed(..., iboindex + section->vertexindex, section->vertexcount)` (hw_flats.cpp:293).
- The texture placement is a texture matrix built from the sector's `FTransform` (offset, scale, angle) and the texture size relative to 64 (hw_flats.cpp:68-98). It is set by `SetPlaneTextureRotation` (511, 540).
- The per-vertex base UVs are written by `CreateVBO` (maploader.cpp:3232 → hw_vertexbuilder.cpp) into `FFlatVertex` (flatvertices.h). **Neither file is visible.** In upstream GZDoom the base UV is world XY divided by 64.
- The triangulation comes from `FSection`s (`CreateSections`, maploader.cpp:3182; hw_sections.cpp, not visible). Those are built after BSP from subsector segs.

**Implication.** Triangle vertices include node-builder split points, and a sector may be cut into several sections. So storing UVs on the original polygon's vertices is not enough. The vertex builder needs a per-sector UV **function** that it can evaluate at any point.

**Two cases:**
- If ROTH's per-vertex UVs are affine within a sector, which one stretched copy (for example fitting the bounding box) usually is: store a 2x3 matrix per plane and evaluate it in the vertex builder, default off. An axis-aligned stretch may already fit the existing `xScale/yScale/xOffs/yOffs` with no engine change, but the fixed matrix order (scale, translate, 64/size scale, rotate; 89-93) cannot express skew.
- If the UVs are non-affine (four arbitrary corner UVs), a custom triangulation using only the original vertices would be needed.

Keep in mind that 3D-floor planes reuse the target sector's vertices with the model sector's transform (`SetFrom3DFloor`, hw_flats.cpp:635-661).

## 6. 3D floors (mid-platforms)

**In memory:**
- `extsector_t::XFloor.ffloors` (a `TDeletingArray<F3DFloor*>`), plus `lightlist` and `attached` (r_defs.h:575-581).
- `F3DFloor` itself is not visible (p_3dfloors.h).
- From its uses: `master` (a line whose `sidedef[0]` mid texture skins the sides) (hw_walls.cpp:2032, 2083), `top/bottom.model` (control sectors for height, texture and light) (hw_walls.cpp:2101; hw_flats.cpp:637-654), `target`, `flags` (FF_EXISTS, FF_RENDERPLANES, FF_UPPERTEXTURE, FF_LOWERTEXTURE, FF_FOG), and `alpha`.

**Creation:**
- `MapLoader::Spawn3DFloors` and `Set3DFloor(line_t*, ...)` (maploader.h:238-239), called at maploader.cpp:3189. Both are defined in p_3dfloors.cpp, which is not visible.
- Upstream exposes a static `P_Add3DFloor(sec, model, master, flags, alpha)` there.

**Could a native loader create them directly? Yes, with conditions:**
- It needs a model `sector_t` per platform. This can be an extra sector with no lines, but it must be in `Level->sectors` before `CreateVBO`.
- It needs a `master` line with a sidedef. The renderer dereferences `rover->master->sidedef[0]` unconditionally (hw_walls.cpp:2032).
- It must call something like `P_Add3DFloor` after `Spawn3DFloors`, before `CreateVBO` and `P_Recalculate3DFloors` (3232-3239).

**Constraint:**
- A 3D floor covers the **whole** target sector, so a platform smaller than its sector needs its own sector footprint.
- Side walls get either one texture from the master line or the per-seg top/bottom texture (`FF_UPPERTEXTURE`/`FF_LOWERTEXTURE`, 2067-2078). They do not get a separate texture per face.

## 7. Sprites

Everything below is in `HWSprite::Process` (hw_sprites.cpp:1022):
- **World size:** `sprscale = thing->InterpolatedScale()` (actor `Scale`) (1099). Then `r.Scale(sprscale.X, sprscale.Y*isoscaleY)` (1577). The level `pixelstretch` is applied for roll or square-pixel sprites (1579-1584) and in `CalculateVertices` (665, 721, 762-766). At 2 world units per pixel, use Scale 2 and square pixels.
- **Vertical anchor:** `z = pos.Z + WorldOffset.Z` (1377), with `Floorclip` subtracted for face sprites (1379). Then `z1 = z - r.top`, `z2 = z1 - r.height` (1588-1589). `r` comes from the texture's `GetSpritePositioning().GetSpriteRect()` (1522, 1531), that is, the texture's top/left offsets. A top offset of 0 makes the sprite hang downward from its anchor. `GetSpriteOffset` (1524-1525) and `SpriteOffset` also apply.
- **Orientation:** `spritetype = renderflags & RF_SPRITETYPEMASK` (1375).
  - `RF_WALLSPRITE` uses the actor's `Angles.Yaw` (1635-1644; 653-700). This is the fixed-angle, wall-mounted case.
  - `RF_FLATSPRITE` is at 1619-1633 and 582+.
  - Angles come from `thing->Angles` (1358-1360).
- **Texture without sprite-name lumps:** `thing->picnum`, when valid, overrides the sprite lookup (1036, 1424) and disables model lookup (1162). This fits runtime-registered ROTH object textures.
- **Meshes:** go through `modelframe` / skins (1162, 1238-1266). Per-face textures would need `surfaceskinIDs` (1266).

## 8. Risks: code that assumes Doom data

- **Doom-format branches keyed on `!HasBehavior && !isText`.** These are the translator/xlat, `LEVEL2_DUMMYSWITCHES`, teleport TID, `SECF_FLOORDROP` (1077) and the checksum over Doom lumps (p_openmap.cpp:393-397).
- **`CheckCompatibility(map)` and `PostProcessLevel`.** These key on the map MD5 and lumps (2967, 3024). Bodies are not visible.
- **Other functions that take `MapData*`.** `T_LoadScripts`, `LoadStrifeConversations`, `GetPolySpots`, `CheckNodes` (node caching) and `LoadLightmap` all read lumps; the ROTH map needs empty-safe readers.
- **Name length limits:**
  - Lump lookup by map name is limited to 8 characters (p_openmap.cpp:129).
  - `GetMapIndex` uses `strnicmp(...,8)` (80).
  - Sector flat names are truncated to 8 when `truncate=true` (maploader.cpp:180-186).
  - Binary sidedef names are copied as 8 characters (2194-2196).
  - Use UDMF-style untruncated calls, or `side->SetTexture(pos, FTextureID)` directly.
- **Name-based texture lookup with fallback.** `CheckForTexture` falls back to `GetDefaultTexture` and prints warnings (141-165, 187-201). Runtime textures need unique names, or should be assigned by ID.
- **Line specials.** `FinishLoadingLineDef` consumes `TranslucentLine` (1573-1608), `ProcessSideTextures` interprets specials (2036), and `Line_Mirror` is checked in `DoTexture` (1652). Keep `special=0`.
- **Required bookkeeping.** Skipping any of these crashes or misrenders: `linemap`, nonzero side scales, `extsectors`, `sidenum`, and `vertexdatas`.
- **Walls are anchored to world Z and tiled.** ROTH's stretch rule must be baked into scales, or needs the explicit-UV hook in section 4.
- **Line-based renderer and physics.** True 3D mesh props and non-whole-sector platforms do not fit; see section 6.
- **Doom actor tables.** `DoomEdMap` and player-start `EdNum`s assume Doom-style actor tables, so ROTH objects need DoomEdNums or direct actor spawning.
