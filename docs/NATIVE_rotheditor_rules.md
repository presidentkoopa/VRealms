# roth-editor: how it reads ROTH files and builds the 3D world

Source root: `/mnt/user-data/uploads/VRealms/tools/roth-editor/src/`. All file:line references below are relative to that root. All multi-byte values are little-endian unless marked BE. `Parser.Type`: Byte/Char=u8, Word=u16, DWord=u32, SignedWord=s16, SignedByte=s8, BigEndianWord=u16 BE, String=NUL-terminated ASCII (`parsers/parser.gd:5-17, 65-93`).

---

## PART A: File inventory

### A1. ROTH.RES (install index): `globals/roth.gd:116-148`
- Text file. The parser replaces `\` with `/` and strips quotes. A `key=value` line goes into `res` (for example `version=`, `snd=`, `das2=`). `das2` gets `.das` appended and is the ADEMO pack. Inside `maps { ... }`, each line is `<mapPath> <dasPath>`. The map file is `install_dir/<MAPPATH>.RAW` and the DAS file is `install_dir/../DATA/<DASPATH>.DAS` (`:139-145, 186`).
- `version` selects the exe generation (F1.4/F1.8/F1.14 = old 3.925; Spanish F1/1.8/1.12 = new 3.983) (`:150-164`).
- Vanilla SFX pack is `DATA/DATA/FX22.SFX`, not FXSCRIPT.SFX (`:277-279`). DBASE files are `DATA/DBASE100..500.DAT` (`:246-248`).
- At startup, ADEMO indices 0..292 are preloaded (`:327`).

### A2. Map *.RAW: `parsers/raw.gd` (`parse` at `:228-440`)
Layout in file order:
| Section | Record | Size |
|---|---|---|
| Header `:5-21` | 15×u16: verticesOffset, version(0x70), sectorsOffset, facesOffset, faceTextureMapsOffset, mapMetadataOffset, verticesOffsetRepeat, signature(2 chars, 21079="WR"), midPlatformsSection (0 = none), section7Size, verticesSectionSize, objectsSectionsSize, footerSize, commandSectionSize, sectorCount | 0x1E |
| Sectors `:23-41` | s16 ceilingHeight, s16 floorHeight, u16 unk0x04, u16 ceilingTextureIndex, u16 floorTextureIndex, u8 textureFit, u8 lighting, **s8** textureMapOverride, u8 facesCount, u16 firstFaceOffset (absolute), u8 ceilShiftX, u8 ceilShiftY, u8 floorShiftX, u8 floorShiftY, u16 floorTriggerID, u16 unk0x16 (flags), u16 intermediateFloorOffset (absolute, 0 = none) | 0x1A |
| u16 faceCount, Faces `:43-50` | u16 vertexOffset01, u16 vertexOffset02 (relative to the vertices section), u16 textureMapOffset (absolute), u16 sectorOffset, u16 sisterFaceOffset (0xFFFF = one-sided), u16 addCollision | 0x0C |
| u16 count, TextureMappings `:52-65` | u8 unk0x00 (length low byte), u8 type (bits 0-6 = length high bits, bit 7 = extra 4 bytes follow), u16 mid, u16 upper, u16 lower, u16 unk0x08 (flags); if type≥0x80: u8 shiftTextureX, u8 shiftTextureY, u16 unk0x0C (face ID for commands) (`parser.gd:53-56`) | 0x0A (+4) |
| [u16 count, MidPlatforms] if header.midPlatformsSection≠0 `:67-78` | u16 ceilTex, s16 ceilHeight, u8 ceilShiftX, u8 ceilShiftY, u16 floorTex, s16 floorHeight, u8 floorShiftX, u8 floorShiftY, u8 floorTextureScale, u8 pad | 0x0E |
| Map metadata `:80-95` | s16 initPosX, s16 initPosZ, s16 initPosY, s16 rotation, u16 moveSpeed, playerHeight, maxClimb, minFit, unk0x10, s16 candleGlow, u16 lightAmbience, unk0x16, skyTexture, unk0x1A | 0x1C |
| @verticesOffset: header (u16 size, u16 hdrSize=8, u16 0, u16 count), then vertices: 4×u16 unused, s16 x, s16 y | 8 + 0x0C·n |
| Commands `:113-134, 301-329` | header: 2 chars signature ("3u" = 30003), u16 unk, u16 commandsOffset, u16 count; 15×(u16 categoryOffset, u16 count); count×u16 entry offsets (0 = skipped); commands: u16 size, u8 modifier, u8 base, u16 nextCommandIndex (1-based), (size-6)/2 u16 args | |
| Section 7 `:136-175, 332-346` | u16 sizeA, u16 count, count×0x12 sound effects (s16 x (negated like objects), s16 y, …, u8 zoneIndex @9, u8 volume @0x10); then 0x20-byte zone records until section7Size | |
| Objects `:177-198, 349-374` | u16 size; one u16 per sector, an offset relative to the section start (0 = no objects); at that offset u8 count, u8 count repeat, then count×OBJECT: s16 posX, s16 posY, u8 textureIndex, u8 textureSource, u8 rotation, u8 unk0x07 (flags, "bit 8 start hidden"), u8 lighting, u8 renderType, s16 posZ, u16 unk0x0C, u16 unk0x0E | 0x10 each |
| Footer | 8 bytes (`08 00 08 00 00 00 00 00`, `map.gd:1230`) | |

Offsets are resolved to indices through offset→index maps (`raw.gd:387-417`). Sector faces are contiguous: face j = firstFaceOffset + 0x0C·j. Special case: for vanilla RAQUIA2, vertex y > 20000 gets 65536 subtracted (`map.gd:129-137`).

### A3. DAS texture packs (DEMO*.DAS in `DATA/M`, and ADEMO.DAS, the "das2" pack): `parsers/das.gd`
**Header** (`:69-93`, 68 bytes; the offsets are confirmed by the compiler at `:1210-1235`): "DASP", u16 5, u16 size_fat, u32 img_fat_offset @8, u32 palette_offset @12, u32 unk_0x10_offset @16, u32 filenames_offset @20, u16 filenames_size, u16 directional_table_size @26, u32 directional_table_offset @28, u16 unk_0x20, **u16 sky_index @34**, u32 object_collision_offset @36, u32 monster_mapping_offset @40, u32 monster_mapping_size @44, u16 fat_block_1..4_count @48..54, u32 unk_0x38_offset, u16 unk_0x38_size, u16 unk_0x40_size, u32 unk_0x40_offset.

**FAT** (`:107-112`): entry i is at img_fat_offset + 8·i and holds u32 offset, u16 size, u8 flags_1, u8 flags_2. flags_1 bits (`:4-13`): 1 = SKY, 2 = MONSTER, 5 = DIRECTIONAL. flags_2 indexes the directional or monster table. FAT blocks 1-4 are consecutive. Map objects use textureIndex+4096 (source 0) or +4096+256 (source 1) (`das.gd:1981-1988`, `object_roth.gd:197-208`), so the object block starts at 4096.

**Filenames** (`:95-105, 331-338`): u16 count1, u16 count2, then entries of u16 size, u16 index, cstring name, cstring desc. Only indices that appear here are loaded.

**Palette** (`_parse_palette :869-883`): 768 bytes of 6-bit VGA RGB at palette_offset. It is converted to 8-bit with `(v*259+33)>>6`. If palette_offset == 0, the parser uses `DEFAULT_RAW_PALETTE` / `DEFAULT_PALETTE` (`:48-67`). Shading tables come after the palette: 2 bytes of padding, then **322×256** remap bytes at palette_offset+2+768 (`:886-894`). They are parsed only in the editing path and never used for rendering.

**Directional table** (`:168-178, 323-328`): N = size/20. First N×u16 offsets, then N 18-byte records (u16 header, dir_1..dir_8 FAT indices). Record i is at table_off + 2N + 18i.
**Monster table** (`:180-230, 315-320`): size/104 records of 104 bytes: u32, 8 directions × {flying, walking, attack1, attack2, on_damage} (order back, back_right, right, front_right, front, front_left, left, back_left), then dying_normal, dead_normal, dying_crit, dead_crit, spawn, u16, u32, u32.
**Object collision** (`:831-839`): one u32 per FAT-3 entry (raw).

**Image entry header** (`:114-119`): u8 modifier, u8 image_type, u16 width, u16 height.
- image_type bits (`:26-35`): 0 ANIMATION, 1 PALETTE_ZERO_OPAQUE, 2 TRANSPARENT, 3 MIRROR, 7 OBJECT_DATA.
- modifier bits (`:37-46`): 4 DRAW_DOWNWARD, 6 IMAGE_PACK, 7 HALF_SIZE.
- ADEMO only: each entry is preceded by 4 bytes, two s16 "shift_data" values (`:932-936` and similar). The render path ignores them (it seeks straight to `offset`, `:417-420`).

**Entry-type dispatch for rendering** (`_load_texture_from_file :408-706`, checked in this order):
1. **Monster** (flags_1 has DIRECTIONAL and MONSTER, `:428-436`): `monster_index = monster_mappings[flags_2].walking_front & 0x7FFF`. For ADEMO: `lo | (((hi & 0x7F) - 0x12) << 8)`. Only the walking-front frame is ever shown.
2. **Directional** (DIRECTIONAL only, `:439-446`): `directional_index = dir_5_fat_idx` with the same masking and ADEMO fix. Only direction 5 is used.
3. offset == 0: no image.
4. **Animated** (image_type bit 0, `:453-571`). The header continues with u32 total_block_size, u16 first_image_offset, u16 num_sub_images, u16 0xFFFF, u8, u8 animation_speed (`:121-132`).
   - *Type 1* (num_sub_images ≠ 0xFFFE): a u32 offsets table (editing path `:962-963`), zero padding, then at offset+first_image_offset a 6-byte header and a raw W·H key frame. Each following delta frame modifies the previous buffer:
     - code==0: n=next byte; n==0 ends the whole animation, otherwise the next byte is repeated n times.
     - code>0x80: skip code&0x7F bytes.
     - 0<code<0x80: copy `code` literal bytes.
     - code==0x80: read w=u16; w==0 ends the frame. If w&0x8000: n=w&0x3FFF, then value byte v; v==0 fills n zeros, otherwise the frame ends. Then pos+=n. A plain w is a skip of w bytes (`:486-517`).
     - The editing path maps frames through the offsets table and drops the final frame because it duplicates the first (`:1039-1048`). The render path just plays the deltas in sequence.
   - *Type 2* (0xFFFE): starting at offset+16, a series of 24-byte sub-headers (`:145-157`: u16 type 0x17, u16, u16 buffer_w, u16 buffer_h, u16 num_images, u16 idx, u32 size incl. header, u16 x_off, u16 width, u16 y_off, u16 height), each followed by RLE data. RLE rule: a byte >0xF0 means repeat the next byte (b&0x0F) times; any other byte is a literal. The series continues while num_images stays the same. Frame size is width×height. **x_off/y_off are ignored** (possible approximation).
5. **3D object** (image_type bit 7, `:574-600`). Header is 24 bytes (`:232-243`): u8 mod, u8 type, u16 max_x, u16 max_y, u16, u16, u32, u16 max_z, u32, u16 num_vertices. Each vertex is 18 bytes: s16 x, y, z plus 12 bytes of padding. Next comes a face-mapping header: 4-char signature, u16, **BE u16 faces_array_size**. Faces are read while `size < faces_array_size` with size starting at 4. Each face has a 54-byte header (`:260-285`): texture_fat_index_base is **BE** u16 @0x0C, render_flag_1 @0x16, render_flag_2 @0x17, sub_texture_index @0x1C, edge_count @0x34. It is followed by (edge_count+1)×u16 edges, and **vertex index = edge>>4**.
6. **Image pack** (modifier bit 6 and not SKY, `:604-682`). After the header come u8 size_of_offsets and u8 pack_type.
   - pack_type&0x80, a *directional image pack*: size_of_offsets/2 u16 offsets. Bit 15 marks a mirrored view (editing path `:1113-1120`); unique images are keyed by offset&0x7FF. Images start at offset+32, each a 6-byte header plus W·H bytes, and each is realigned so that `pos&0xF == alignment` (the alignment of offset+32).
   - Otherwise, a *3D-object texture pack*: count the non-zero u16s up to a 0 word (numImgs-1), skip zero bytes, then read (u8 ref, u8 type, u16 w, u16 h, pixels) records with the same 16-byte realignment. These images are indexed by the 3D face's sub_texture_index.
7. **Plain image**: W·H raw 8-bit indices directly after the header (`:685-703`).

**Pixel order.** Wall and sprite images are stored transposed. The stored `width` is the displayed height, and each run of `width` bytes is one displayed column, top to bottom. This is what the UV setup in Part B implies.

**Palette → RGBA** (`utility.gd:115-146`, GPU twin `shaders/convert_palette_image.glsl`):
- `is_transparent = (type & TRANSPARENT) || !(type & PALETTE_ZERO_OPAQUE) || is_fat3`, where is_fat3 means the index is in FAT block 3 (`das.gd:423-425, 698`).
- `is_fully_transparent = image_type & TRANSPARENT`.
- RGB = `(pal6*259+33)>>6`.
- Alpha:
  - Not transparent: RGB only, fully opaque.
  - is_transparent: index 0 → alpha 0, everything else → 255.
  - is_fully_transparent: index 0 → 0, 1..127 → 255, **128..255 → 128** (translucent).
- Each image uses its own pack's palette. ADEMO sprites therefore use ADEMO's palette, or the default palette when ADEMO has none, not the level DAS palette (`das.gd:366`).

### A4. DBASE100.DAT: `parsers/dbase_100.gd`
- Header, 0x34 bytes (`:4-17`): "DBASE100", u32 filesize, u32 unk, then count/offset u32 pairs for inventory, action, cutscene and interface, then u32 unk.
- Cutscenes, 0x14 bytes each (`:19-25`): char[8] name, u16, u16 subtitle length, u32 DBASE400 offset, u32 DBASE400 subtitle offset.
- Interfaces: a u32 DBASE400 offset each.
- Inventory: u32 pointers to records (`:31-40`): u16 len, u16 object_texture_index, u8 closeup_type, u8 item_type, 2 bytes, u32 closeup_video (DBASE300 offset/8), u32 inventory_image (DBASE200 offset/8), u32 DBASE400 offset. Action blocks follow: a 24-bit length plus u8 trigger, then 4-byte commands (24-bit arg, u8 opcode), until length 0 (`:164-205`).
- Actions: u32 pointer → u16 length, u16 unk, (length/4 - 1) commands (`:224-265`).
- Opcode names are in `opcodes.gd`. Text opcodes 5/8/15/16 point to DBASE400. 18/31 point to DBASE200 (/8). 0/14/26 point to DBASE300 (/8).

### A5. DBASE200.DAT (inventory icons and weapon animations): `dbase_200.gd`
"DBASE200" header, then records of u32 size + data, each padded to 8-byte alignment.
- Type 3 (`:11-15, 84-86`): u32 type, u16 w, u16 h, then 0xF0-RLE data (`rle.gd:5-28`).
- Type 0x1E, row-RLE animation (`:17-28, 87-99`): 24-byte frame headers. Each frame has height×(u16 start, u16 run), then the concatenated pixels (`rle.gd:66-82`).

### A6. DBASE300.DAT: `dbase_300.gd`
"DBASE300" header; records are u32 size + payload, 8-byte aligned (`:41-87`). The payload type is chosen by its first u32:
- GDV 0x29111994
- HMP "HMIM"
- MIDI "MThd"
- IMG1: u32 1, u16 w, u16 h, **768-byte 8-bit palette**, RLE
- IMG3: 8-byte header + RLE
- IMG7: 16-byte header (buffer w/h, x_off, w, y_off, h) + RLE (`:17-38, 157-175`)

### A7. DBASE400.DAT (text): `dbase_400.gd`
- Text entry (`:4-9`): u32 dbase500_offset (/8), u16 length_str, u16 font_color, cstring, padded to 4 bytes.
- Subtitle stream (`:45-76`): records of u16 length, u16 timestamp, u8 colour, string of length-5 bytes, padded to 2. timestamp 0xFFFF introduces the title record.

### A8. DBASE500.DAT (speech): `dbase_500.gd`
At 8·offset there is a 44-byte WAV header whose chunkID is "FFIR" (`:9-25`). audioFormat is 42 (DPCM). To decode: `state += delta_table[byte]`, wrapped to s16. The table comes from `utility.gd:98-112`: code=64, step=45, delta += code>>5, code += step, step += 2, filling ±pairs.

### A9. FXSCRIPT.SFX / FX22.SFX: `fxscript.gd`
- Header, 28 bytes (`:4-13`): "0XFS", u16 1, u16, u32 fat_off, u32, u32 fat_size, u32 names_off, u32 names_size.
- FAT, 12 bytes per entry: u32 offset, u32 size, u16 index, u16 type (1 = 16-bit 11025 Hz, 3 = 16-bit 22050 Hz).
- Names: u16 index, cstring name, cstring desc. Terminated by 0xFFFF.
- Samples are s16 LE mono PCM (`:62-74`).

### A10. GDV video (*.GDV, and inside DBASE300): `gdv.gd`
- Header, 24 bytes (`:14-27`): sig, size_id, nb_frames, framerate, sound_flags, playback_frequency, image_type, frame_size, u8, u8 lossyness, w, h. If image_type bit 0 is set, a 768-byte 6-bit palette follows.
- Per frame: an audio chunk of freq/fps bytes, ×2 if stereo, ×2 if 16-bit, then /2 if DPCM. Then the video data: u16 sig, u16 len, u32 type_flags, followed by the data (`:43-97`).
- type_flags: encoding = low 4 bits (1 = palette, 3 = unchanged, 8 = LZ-style; 0/2/5/6 are not implemented), bit 4 hscale, bit 5 vscale, bit 6 intra, bits 8+ pixel_skip. The decoder keeps a 4096-byte prefix dictionary (`:169-200, 270-391`).
- Stereo DPCM audio uses the same delta table (`:414-420`).

### A11. HMP music: `hmp.gd`
u32 size, then the "HMIMIDIP013195" header (`:4-19`), 840 unknown bytes, and num_chunks chunks. Each chunk is u32 number, u32 length, u32 track, then events. The HMP delta is variable-length with the stop bit ≥0x80, least-significant group first (`:136-146`). Meta events use MIDI-style varlen (`:126-133`).

### A12. MELODIC.BNK / DRUM.BNK: `bnk.gd`
AdLib bank format (`:6-47`). Instrument data is 30 bytes at data_offset + 30·index.

### A13. ICONS.ALL: `icon_all.gd`
A table of (u32 offset, u32 size) pairs; the count is first_offset/8. Each entry has an 8-byte header (u16 image_type, u8 x_off, u8 y_off, u16 w, u16 h) followed by 0xF0-RLE data (`:4-63`). No palette.

### A14. BACKDROP.RAW: `backdrop.gd`
Only a compiler is staged (`:5-14`). It writes the same 8-byte header as ICONS.ALL followed by RLE data, so the file is presumably read the same way; that reader is not in the staged files.

---

## PART B: Building the 3D world

### B1. Coordinates and units
- `SCALE_3D_WORLD = 100`, `SCALE_2D_WORLD = 10` (`roth.gd:25-26`).
- 2D vertex: `v = (-rothX, rothY)` (`face.gd:90-91`).
- 3D point: **Godot (X, Y, Z) = (-rothX, height, rothY) / 100** (`face.gd:384`, `sector.gd:513`, `object_roth.gd:309-313`). Object height is posZ.
- Player start, written back when saving: initPosX = -x, initPosY = z, initPosZ = y (`map.gd:827-830`).
- Rotation:
  - Player: `deg = -180 + 90·rot/128`, so 512 units = 360° (`roth.gd:497-503`).
  - Object: 256 units = 360°. The 2D icon uses `rot/256·360 - 90` (`roth.gd:507-513`).
  - 3D rotations are listed in B5.

### B2. Sector floors and ceilings: `sector.gd:438-685`
- **Polygon**: `vertices = Geometry2D.convex_hull(all face endpoints)` (`:76-82`). **Approximation:** a concave sector gets filled as its convex hull. The editor warns separately (`map.gd:666-672`).
  - The hull is triangulated with `triangulate_polygon`. When that fails there is a fallback of [0,1,2,0,2,3] for 5 points or [0,1,2] for 4 points (`:477-487`); the source calls it a "hack".
  - Ceiling triangle order is reversed (`:490-491`).
- Meshes: floor at floorHeight and ceiling at ceilingHeight (`:666-673`). If the sector has a platform, two more: platform floor at platform.floorHeight and platform ceiling at platform.ceilingHeight (reversed) (`:676-685`).
- **Base UV**: u = (x-minX)/(maxX-minX), v = (y-minY)/(maxY-minY), with x = -rothX and y = rothY (`:508-512`). Godot applies `uv' = uv·uv1_scale + uv1_offset`.
- **Texture size**: tw = mapping.width, th = mapping.height. **If tw == 256, both are halved** (`:526-528`). This is an unexplained hack.
- **Scale bits**: A and B come from textureFit (A = bit 2 / B = bit 3 for the ceiling, A = bit 4 / B = bit 5 for the floor, `:10-18, 533-543`). Platforms read the same bit positions from `platform.floorTextureScale` (`:544-554`). **No other textureFit bits are used** (LINK_EXISTS b0, CANDLE b1, LIGHTNING b6 are ignored). The multiplier k is:
  - (A,B) = (0,0): k = 0.5 ("1:2")
  - (1,0): k = 1 ("1:1")
  - (0,1): k = 2 ("2:1")
  - (1,1): k = 4 ("4:1")
- **Formulas** (`:557-613`), with Px = 2·k·tw and Py = 2·k·th:
  - `u' = (x - minX + (minX % 1024) + shiftX·k) / Px`
  - `v' = (y - minY + (minY % 1024) - shiftY·k) / Py`
  - When Px divides 1024 this reduces to the world-anchored **u = (-rothX + k·shiftX)/Px, v = (rothY - k·shiftY)/Py**, i.e. 2k world units per texel.
  - The % is a truncating int modulo. Shifts are u8.
- **Flips** use sector.unk0x16 (even for platforms): bit 8 floor X, bit 9 floor Y, bit 10 ceiling X, bit 11 ceiling Y. A flip negates both the scale and the offset on that axis (`:20-24, 616-629`).
- **Sky**: if texture == DAS `sky_index`, the albedo is set to fully transparent with alpha scissor, so the surface is invisible. Nothing is drawn behind it, and map.skyTexture is unused (`:461-463`).
- **Flat colour**: texture ≥ 65280 → `palette[texture-65280]` (8-bit)/256 (`:468-470`). Any other missing index is drawn solid BLUE.
- **Transparency**: only platform surfaces get it. TRANSPARENT → alpha blend; else !PALETTE_ZERO_OPAQUE → alpha scissor (`:452-457`).
- Materials are unshaded with nearest filtering. Animated textures show frame 0 only.

### B3. Walls: `face.gd`
**Pieces** (`_initialize_meshes :376-515`). Each quad has vertex order [v1@bottom, v1@top, v2@top, v2@bottom].
- One-sided: a mid quad from floor to ceiling using midTextureIndex (mid=true).
- Two-sided (sister exists):
  - Mid, only if flags bit 0 TRANSPARENT: from `max(floorA,floorB)` to `min(ceilA,ceilB)`, with mid=true and transparency allowed.
  - Lower, if floor < sister.floor: from floor to sister.floor, using the lower texture.
  - Upper, if ceil > sister.ceil: from sister.ceil to ceil, using the upper texture.
  - Each side draws only its own pieces. The sister face draws the opposite side.
- Invisible walls: if `sector.floorTriggerID == 65534` and the face is two-sided, every piece is transparent (`:150-152`).

**UV** (`:181-198`): v1-bottom (1,0), v1-top (0,0), v2-top (0,1), v2-bottom (1,1).
- U is vertical: 0 at the top, rising toward the bottom.
- V is horizontal: 0 at v1, 1 at v2.
- This is the column-major storage described in Part A, so **tw_h = mapping.height (texels along the wall) and tw_v = mapping.width (texels vertically)** (`:221-226`, default 128). HALF_PIXEL (bit 5) halves both.

**Scale** (`:233-235`), applied when `!(IMAGE_FIT) || (TRANSPARENT && !mid)`:
- `scale.V = storedLen / (2·tw_h)`, where storedLen = unk0x00 + ((type & 0x7F) << 8). This is the stored extent, not the geometric length. The editor writes ceil(length) into it (`:117-120`).
- `scale.U = pieceHeight / (2·tw_v)`.
- Result: **2 world units per texel**, or 1 with HALF_PIXEL.
- IMAGE_FIT (bit 2) leaves the scale at 1, so the texture is stretched once. Upper and lower pieces of TRANSPARENT faces always tile.

**Vertical anchor**:
- Default: texel column 0 sits at the top of the piece (the ceiling for mid and upper pieces, the sister's floor for lower pieces).
- PIN_BOTTOM (bit 7) uses the alternate UVs (0,0)/(1,0)/(1,1)/(0,1) and then negates the scale and offset on U (`:201-218, 319-321`). This anchors the texture's last column at the bottom edge.

**Shifts** (`:303-314`), only when type bit 7 is set and IMAGE_FIT is clear:
- `offset.V = shiftTextureX / tw_h`
- `offset.U = shiftTextureY / tw_v`
- Units are **texels**, not world units. The negative branches (+256) can never run because the fields are read unsigned.

**Other flags**:
- FLIP_X (bit 1) negates the V scale and offset (`:323-325`).
- TRANSPARENT mid pieces with a negative U scale get offset.U += 1 (`:328-330`).
- **textureMapOverride** (s8), applied to a mid piece when two-sided and flag bit 3 TRANSPARENT_UPPER_LOWER is set (`:239-299`):
  - If >0: the bottom edge is raised, so the quad covers only the top `override·4` units. scale.U = override·2/tw_v.
  - If <0: the top edge is lowered to bottom + |override|·4.
- Bit 4 NO_REFLECT and bit 6 EDGE_MAP are defined but unused.
- **Colour faces**:
  - 65535 → palette[255] (`:167-169`, which computes 65535-65280).
  - ≥32768 → palette[tex-32768] (`:170-172`). This threshold differs from the sector rule.
  - Any other missing index → WHITE.
  - FLIP_X on a colour face means alpha 128 (translucent) (`:177-178`); the flag is overloaded here.
- **Transparency** (mid pieces of two-sided TRANSPARENT faces only, `:144-148`): texture TRANSPARENT (or colour + bit 1) → ALPHA_DEPTH_PRE_PASS; else !PALETTE_ZERO_OPAQUE → ALPHA_SCISSOR.
- Sky index → invisible (`:154-156`).
- Image packs use image[0].
- Two-sided faces also get an invisible collision strip 5 units wide (`:453-497`).

### B4. Mid-platforms
- Horizontal surfaces are covered in B2. `platform.floorHeight` is the top surface and `ceilingHeight` is the underside.
- Side walls (`face.gd:499-515`): if the **sister's** sector has a platform and this face is not TRANSPARENT, the face draws a quad from `platform.ceilingHeight` to `platform.floorHeight` using **midTextureIndex**, provided `floorHeight - ceilingHeight > 0`.

### B5. Objects: `object_roth.gd`
**Texture source** (`:195-212`):
- 0 → map DAS index+4096
- 1 → map DAS index+4096+256
- 2 → ADEMO index
- 3 → ADEMO index+256
- Otherwise, or when the entry is invalid, a purple sphere of radius 0.125.

**Sprite** (`:221-336`):
- Half-width `w = tex.height/100`, half-height `h = tex.width/100` (transposed storage).
- The quad spans x ∈ [-w, +w] and y ∈ [0, 2h], i.e. **2 units per pixel**, with the **bottom at posZ**.
- HALF_SIZE, `modifier` bit 7, halves both. For directional entries the child's modifier is used (`:254`).
- DRAW_DOWNWARD, `texture.modifier` bit 4 (the parent entry's), shifts the quad down by 2h so that its top is at posZ (hanging) (`:272-274`).
- `flags_1` bit 3 was tried and commented out, so it has no effect (`:267-270`).
- Monster entries show the walking_front texture's image. Directional entries show dir_5's image. Image packs and animations show frame 0 (`:240-254`). **Directional and animation state is not reproduced.**
- UVs, after the edits at `:285-293` (assuming Godot's QuadMesh vertex order): (+w,bottom) (1,1), (-w,bottom) (1,0), (+w,top) (0,1), (-w,top) (0,0). **unk0x07 bit 4** swaps these to (1,0), (1,1), (0,0), (0,1), which mirrors the sprite horizontally (`:295-299`).
- **Billboard vs fixed**:
  - renderType bit 7 clear: `BILLBOARD_FIXED_Y`, a Y-axis billboard (`:228-231`).
  - bit 7 set: fixed, with `rotation_degrees.y -= rotation/256·360 - 180` (`:334-336`).
- Cull disabled. TRANSPARENT → alpha blend, otherwise alpha scissor. grow 0.001.
- Other flags: unk0x07 bit 0 only changes the highlight material (`:615`). The "bit 8 start hidden" flag (comment at `raw.gd:192`) is not rendered.

**3D mesh objects** (`_initialize_3d_object :339-425`):
- Each face vertex is `vertices[edge>>4]` with **x negated**, then /100. It is placed at (-posX, posZ, posY)/100 with `rotation_degrees.y -= rotation/256·360` (no -180, unlike sprites).
- **Triangulation** by edge count:
  - 5 edges (a closed quad) → tris 0,1,2 / 0,2,3, UVs (0,0)(0,1)(1,1) / (0,0)(1,1)(1,0).
  - 4 edges → one triangle (0,0)(0,1)(1,1).
  - Any other count draws nothing.
  - The whole texture is stretched over each face.
- render_flag_1 bit 1 negates the V scale and offset.
- **Texture**: `texture_fat_index_base` (BE); +0x1000 only when the face has 0 vertices, which looks like a quirk. It is looked up in **map.das**, even for ADEMO objects (probable bug). An array image uses `image[sub_texture_index]`.
- ≥65280 → palette colour `palette[idx-65280]`. Any other missing index → purple.

### B6. Lighting
None. Every sector, face and object material is `SHADING_MODE_UNSHADED`. sector.lighting, object.lighting, lightAmbience, candleGlow, textureFit CANDLE/LIGHTNING and the 322 DAS shading tables are parsed and saved, but never applied to rendering (grep: they appear only in the parser/compiler, `raw.gd:30,90-91,193`, `map.gd:978,1065-1066,1220`, `das.gd:733,886-894`). Colours are full-bright palette RGB.

### Approximations to watch
- Floors: convex hull instead of the true polygon; the 256-width halving hack; only textureFit bits 2-5 are used; platforms reuse the sector's flip bits.
- Walls: the horizontal scale comes from the stored length; shifts are in texels with dead signed branches; bit 1 is used both as a flip and as colour translucency; face colours use a 32768 threshold where sectors use 65280.
- Sprites and objects: directional, monster and animation state is frozen to one frame; ADEMO shift_data and type-2 frame x/y offsets are ignored; the sprite rotation offset (-180) differs from the 3D-object one (0); 3D faces take their textures from the map DAS.
