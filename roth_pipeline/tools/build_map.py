#!/usr/bin/env python3
"""
Turn a parsed ROTH map (parse_raw.py JSON) + extracted textures into a
GZDoom-loadable PK3 containing a UDMF map.

Key structural translations:

  ROTH sector  -> UDMF sector      (floor/ceiling height + textures + light)
  ROTH face    -> UDMF linedef     (+ one sidedef per side)
  ROTH vertex  -> UDMF vertex
  ROTH object  -> UDMF thing       (placeholder type for now)

The one genuinely non-obvious bit: ROTH represents a two-sided wall as TWO faces that
point at each other via sisterFaceIndex, one owned by each neighbouring sector.
Doom represents the same wall as ONE linedef carrying two sidedefs. So sister
pairs are merged — emitting both would leave duplicate overlapping linedefs,
which wrecks node building.

Usage:
    python build_map.py out/STUDY1.json --textures out/tex_demo -o out/roth_study1.pk3
"""

import argparse
import json
import struct
import zipfile
from pathlib import Path

# Texture index sentinels, per face.gd:166-172 — these mean "flat palette
# colour", not a texture lookup. 65535 resolves to palette[255]; anything else
# at or above 32768 resolves to palette[index - 32768].
SENTINEL_EXACT = 65535
SENTINEL_MIN = 32768
NO_TEX = "-"      # UDMF's "nothing here"
SKY_TEX = "F_SKY1"  # Doom's sky marker; ROTH renders its sky_index see-through

# ROTH addresses wall textures in HALF-texels: two world units per texture
# pixel, where Doom uses one. Confirmed independently from both the original
# engine's rasteriser (extents are 2*texW/2*texH, V is >>1 before use) and from
# roth-editor's mesh builder (the `* 2` in every UV denominator).
#
# ROTH uses 2 world units per texture pixel; Doom uses 1. So every ROTH texture
# should appear twice its Doom-default size, which is a scale of 0.5 on both
# walls and flats. Both fields run in the SAME direction: below 1 = larger.
#
# Do NOT try to "fix" a perceived wall/floor mismatch by making these differ.
# That was tried (walls 2.0) and made walls visibly worse. Apparent differences
# in a screenshot are dominated by perspective -- a floor two metres from the
# camera is magnified enormously versus a wall ten metres away. Verify scale by
# computing expected tile counts from map geometry, not by eye.
ROTH_FLAT_SCALE = 0.5   # floors and ceilings
# CONFIRMED CORRECT by A/B test against the original game, 2026-09-25.
# ROTH stores wall textures rotated 90 degrees relative to Doom's convention:
# the along-wall axis indexes texture ROWS, not columns. Transposing every wall
# texture was a large, immediately visible improvement. Flats are NOT rotated.
ROTATE_WALLS = True
# Floors transpose the same way walls do (handoff 5.3). Direction is stated
# but the signs are flagged unconfirmed, so --no-transpose-flats can A/B it.
TRANSPOSE_FLATS = True
TEX_DIMS: dict = {}   # texture index -> (w, h), filled in main()
# Per-sector flat fitting matches the original engine's model but currently
# produces implausible scales (41% of sectors above 8x, max 1152x), so it is
# OFF by default until the discrepancy is understood. --flat-fit enables it.
FLAT_FIT = False
ROTH_WALL_SCALE = 0.5   # sidedefs

# Filled in by main() from the extracted pack's meta.json / palette.json.
SKY_INDEX = 0
PALETTE: list = []
_used_colours: set = set()


def texname(index: int, context: str = "wall") -> str:
    """Map a ROTH texture index to a Doom texture name.

    ROTH renders `index == sky_index` as transparent (face.gd). What that means
    in Doom depends on the surface: a wall drawing nothing is the ordinary
    "no height step against my neighbour" case ("-"), whereas a ceiling drawing
    nothing means the sector is open to the sky (F_SKY1). A floor must always
    have *some* flat or it renders HOM, so it gets the sky too.
    """
    if index == SKY_INDEX:
        return NO_TEX if context == "wall" else SKY_TEX
    # Both walls and flats store their art transposed: the along-wall axis picks
    # texture rows, and for floors world X drives rows too (handoff 5.1, 5.3).
    # So every world surface uses the transposed copy. Object sprites and model
    # skins do NOT -- they are stored the normal way round.
    if ROTATE_WALLS and 0 < index < SENTINEL_MIN:
        if context == "wall" or TRANSPOSE_FLATS:
            return f"RTX{index:04d}"
    if index == SENTINEL_EXACT:
        return palette_tex(255)
    if index >= SENTINEL_MIN:
        return palette_tex(index - SENTINEL_MIN)
    return f"TEX{index:04d}"


def palette_tex(colour_index: int) -> str:
    """Name a solid-colour texture, recording it so we generate the PNG."""
    if not (0 <= colour_index < 256):
        return NO_TEX
    _used_colours.add(colour_index)
    return f"PAL{colour_index:03d}"


def png_transpose(data: bytes) -> bytes:
    """Transpose an indexed PNG (swap rows and columns).

    The ROTH.C investigation reports that ROTH stores wall textures effectively
    rotated 90 degrees relative to Doom's convention -- the along-wall axis
    indexes texture ROWS rather than columns. If that is right, transposing the
    image is the whole fix, and it is far cheaper to test than to re-derive.

    Only handles what we ourselves wrote: 8-bit indexed, filter type 0.
    """
    import zlib as _z
    pos, w, h, plte, idat = 8, 0, 0, b"", b""
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if tag == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif tag == b"PLTE":
            plte = body
        elif tag == b"IDAT":
            idat += body
        pos += 12 + ln

    raw = _z.decompress(idat)
    # strip the per-scanline filter byte (we always write filter 0)
    rows = [raw[y * (w + 1) + 1: y * (w + 1) + 1 + w] for y in range(h)]
    out_px = bytearray(w * h)
    for y in range(h):
        row = rows[y]
        for x in range(w):
            out_px[x * h + y] = row[x]      # (x,y) -> (y,x); new width = h

    nw, nh = h, w
    new_raw = b"".join(b"\x00" + bytes(out_px[y * nw:(y + 1) * nw]) for y in range(nh))

    def chunk(tag: bytes, body: bytes) -> bytes:
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", _z.crc32(tag + body) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", nw, nh, 8, 3, 0, 0, 0))
            + chunk(b"PLTE", plte)
            + chunk(b"IDAT", _z.compress(new_raw, 9))
            + chunk(b"IEND", b""))


def solid_png(rgb: tuple) -> bytes:
    """A 64x64 single-colour indexed PNG, for flat-colour surfaces."""
    import zlib as _z
    w = h = 64
    raw = b"".join(b"\x00" + bytes([0]) * w for _ in range(h))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", _z.crc32(tag + data) & 0xFFFFFFFF))

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
    out += chunk(b"PLTE", bytes(rgb))
    out += chunk(b"IDAT", _z.compress(raw, 9))
    out += chunk(b"IEND", b"")
    return out


# Texture-mapping flag bits, from roth-editor's face.gd:6-14. These live on the
# mapping's `unk0x08` field.
F_TRANSPARENT = 1 << 0
F_FLIP_X = 1 << 1
F_IMAGE_FIT = 1 << 2
F_TRANSPARENT_UPPER_LOWER = 1 << 3
F_NO_REFLECT = 1 << 4      # confirmed by both investigations: no texturing effect
F_HALF_PIXEL = 1 << 5
F_EDGE_MAP = 1 << 6        # confirmed by both investigations: no texturing effect
F_PIN_BOTTOM = 1 << 7


def fit_nibbles(fit: int) -> tuple:
    """ROTH's texture-fit byte is TWO NIBBLES, not a 4-way ratio selector.

    Confirmed in the original engine in two independent places (renderer.c
    :3700-3706 for flats, :3345-3359 for walls): high nibble -> horizontal
    repeats (1+h), low nibble -> vertical repeats (1+l), integers 1..16 per
    axis. A byte of 0 means no scaling at all *and* disables wrapping.

    roth-editor reads only bits 2-5 and models this as four fixed ratios, which
    is why it mis-renders the ~600 sectors in the retail maps whose fit byte
    sets bit 6. Do not copy the editor here.
    """
    if fit == 0:
        return 1, 1
    return 1 + ((fit >> 4) & 0xF), 1 + (fit & 0xF)


def wall_u_repeats(tm: dict, wall_len: float, tex_w: int) -> float:
    """How many times the texture repeats along a wall, per ROTH.

    ROTH stores an explicit 15-bit horizontal extent per face -- `unk0x00`
    plus the low 7 bits of `type`. It is AUTHORITATIVE: the original engine has
    a dedicated stored-extents path (renderer.c:13334-13341), the editor exposes
    it as a hand-editable field independent of geometry, and retail maps carry
    values that deliberately differ from the wall's true length in order to
    stretch or squash a texture.

    Doom derives wall tiling from the measured wall length and has no way to
    express this, so we convert: ROTH repeats = stored / (2 * tex_w), and Doom
    gives repeats = wall_len * scalex / tex_w, hence

        scalex = stored / (2 * wall_len)

    Note tex_w cancels -- and when the stored value equals the true length this
    reduces to 0.5, which is the plain half-texel default. So this only deviates
    where the map author actually authored a deviation.
    """
    stored = tm.get("unk0x00", 0) | ((tm.get("type", 0) & 0x7F) << 8)
    if stored <= 0 or wall_len <= 0:
        return ROTH_WALL_SCALE
    return stored / (2.0 * wall_len)


# Sector `floorTriggerID` sentinels (ROTH_NATIVE_HANDOFF.md 4 and 5.6).
TRIGGER_DOOR = (0xFFFD, 0xFFFF)   # a door; the original closes these at load
TRIGGER_INVISIBLE = 0xFFFE        # two-sided walls in this sector aren't drawn

# Doom linedef special 1: "open, wait, close" on use, retriggerable. The closest
# native equivalent to a ROTH door, and it needs no scripting.
DOOR_SPECIAL = 1


def is_door(sec: dict) -> bool:
    return sec.get("floorTriggerID", 0) in TRIGGER_DOOR


def side(sector: int, tm: dict, top: str = NO_TEX, bottom: str = NO_TEX,
         mid: str = NO_TEX, wall_len: float = 0.0) -> str:
    """One UDMF sidedef, translating ROTH's texture placement onto Doom's.

    ANCHORING. ROTH hangs every wall piece's texture from the TOP of that piece
    (the upper vertex carries v=0). Doom's defaults differ per slot:

      upper  : Doom aligns the texture's BOTTOM to the lower ceiling by default.
               ROTH wants the section's top -> Doom's "upper unpegged"
               (dontpegtop).
      lower  : Doom already aligns to the top of the section. Matches.
      middle : Doom already aligns to the ceiling. Matches.

    PIN_BOTTOM flips a piece to bottom-anchored == Doom's dontpegbottom.

    OFFSETS are in texels on both sides, so they carry across 1:1.
    """
    flags = tm.get("unk0x08", 0)
    sc = ROTH_WALL_SCALE
    # HALF_PIXEL halves the texture dimensions, doubling the repeat rate --
    # one world unit per texel instead of two (face.gd:228-231).
    if flags & F_HALF_PIXEL:
        sc *= 2
    sx = -sc if flags & F_FLIP_X else sc

    # Explicit pixel offsets exist only when the mapping's high bit is set, and
    # IMAGE_FIT suppresses them entirely (face.gd:303-314).
    off_x = off_y = 0
    meta = tm.get("additionalMetadata")
    if meta and (tm.get("type", 0) & 0x80) and not (flags & F_IMAGE_FIT):
        off_x = meta.get("shiftTextureX", 0)
        off_y = meta.get("shiftTextureY", 0)

    fields = [f"sector = {sector}"]
    for slot, tex in (("top", top), ("bottom", bottom), ("mid", mid)):
        if tex == NO_TEX:
            continue
        fields.append(f'texture{"middle" if slot == "mid" else slot} = "{tex}"')
        fields.append(f"scalex_{slot} = {sx}")
        fields.append(f"scaley_{slot} = {sc}")
        if off_x:
            fields.append(f"offsetx_{slot} = {off_x}")
        if off_y:
            fields.append(f"offsety_{slot} = {off_y}")

    if top != NO_TEX:
        fields.append("dontpegtop = true")
    if flags & F_PIN_BOTTOM:
        fields.append("dontpegbottom = true")

    return "sidedef { " + "; ".join(fields) + "; }"



def sector_bbox(sec: dict, faces: list, verts: list) -> tuple:
    """Bounding box of a sector, from the vertices of its own face loop."""
    xs, ys = [], []
    for j in range(sec["facesCount"]):
        f = faces[sec["firstFaceIndex"] + j]
        for k in ("vertexIndex01", "vertexIndex02"):
            v = verts[f[k]]
            xs.append(v["x"]); ys.append(v["y"])
    if not xs:
        return 0.0, 1.0, 0.0, 1.0
    return float(min(xs)), float(max(xs)), float(min(ys)), float(max(ys))


def flat_fit_scale(tex_index: int, bx0, bx1, by0, by1, rep_u, rep_v, scale):
    """Scale that fits (rep_u x rep_v) copies of a flat across a sector's bbox.

    Doom covers `tex_w / xscale` world units with one copy, so to fit `rep_u`
    copies across a span of `w` units: xscale = tex_w * rep_u / w.
    """
    dims = TEX_DIMS.get(tex_index)
    if not dims:
        return ROTH_FLAT_SCALE, ROTH_FLAT_SCALE
    tw, th = dims
    w = max(1.0, (bx1 - bx0) * scale)
    h = max(1.0, (by1 - by0) * scale)
    return (tw * rep_u) / w, (th * rep_v) / h


def build_udmf(m: dict, scale: float = 1.0, things: list | None = None) -> str:
    sectors = m["sectorsSection"]["sectors"]
    faces = m["facesSection"]["faces"]
    verts = m["verticesSection"]["vertices"]
    mappings = m["faceTextureMappingSection"]["mappings"]
    meta = m["mapMetadataSection"]

    out = ['namespace = "zdoom";', ""]

    for v in verts:
        out.append(f"vertex {{ x = {v['x'] * scale:.3f}; y = {v['y'] * scale:.3f}; }}")
    out.append("")

    for s in sectors:
        # ROTH fits flats PER SECTOR, not across the world. Verified against the
        # original engine: build_floorceil_vertex_records assigns texture-space
        # corners to the sector's OWN vertex ring by parity, and world position
        # never enters the texture coordinate. So each sector gets one copy of
        # its texture (times the fit nibbles' repeat count) stretched over its
        # outline, and adjacent sectors do NOT line up.
        #
        # Doom flats are world-anchored by definition and cannot express that.
        # The closest the map format allows is scaling the texture to the
        # sector's bounding box, which is EXACT for a rectangular sector
        # (corners map to corners) and an approximation otherwise. Doing this
        # properly needs per-polygon UVs, i.e. engine work.
        bx0, bx1, by0, by1 = sector_bbox(s, faces, verts)
        rep_u, rep_v = fit_nibbles(s.get("textureFit", 0))

        # A door sector is stored with its ceiling at full height, and the
        # original engine closes it at load (fixup_raw_sectors_after_load drops
        # the ceiling to the floor). Without this every door in the game stands
        # permanently open.
        ceil_h = s["floorHeight"] if is_door(s) else s["ceilingHeight"]

        fields = [
            f'heightfloor = {int(s["floorHeight"] * scale)}',
            f'heightceiling = {int(ceil_h * scale)}',
            f'texturefloor = "{texname(s["floorTextureIndex"], "floor")}"',
            f'textureceiling = "{texname(s["ceilingTextureIndex"], "ceiling")}"',
            f'lightlevel = {min(255, max(0, s.get("lighting", 160)))}',
        ]
        for plane, tex_idx, shx, shy in (
                ("floor", s["floorTextureIndex"],
                 s.get("floorTextureShiftX", 0), s.get("floorTextureShiftY", 0)),
                ("ceiling", s["ceilingTextureIndex"],
                 s.get("ceilingTextureShiftX", 0), s.get("ceilingTextureShiftY", 0))):
            # Per-sector scale, from the sector flags byte: bits 4-5 for the
            # floor, 2-3 for the ceiling, giving 2^s world units per texel
            # (ROTH_NATIVE_HANDOFF.md 5.3). Doom's scale is its reciprocal.
            #
            # Forcing one value on every sector -- which is what a flat 0.5 does
            # -- draws every s=2 surface at twice its intended density, so
            # rugs and floor patterns tile instead of sitting as one piece.
            bits = 4 if plane == "floor" else 2
            s_exp = (s.get("textureFit", 0) >> bits) & 3
            flat_scale = 1.0 / (2 ** s_exp) / scale

            if FLAT_FIT:
                fx, fy = flat_fit_scale(tex_idx, bx0, bx1, by0, by1, rep_u, rep_v, scale)
                fields.append(f"xscale{plane} = {fx:.5f}")
                fields.append(f"yscale{plane} = {fy:.5f}")
                # Anchor the stretched copy at the sector's own corner instead
                # of the world origin -- that is what per-sector fitting means.
                fields.append(f"xpanning{plane} = {(-bx0 * scale) + shx:.3f}")
                fields.append(f"ypanning{plane} = {(by1 * scale) - shy:.3f}")
            else:
                fields.append(f"xscale{plane} = {flat_scale:.5f}")
                fields.append(f"yscale{plane} = {flat_scale:.5f}")
                # Shift steps are half a texel each (handoff 5.3).
                if shx:
                    fields.append(f"xpanning{plane} = {shx * (2 ** s_exp) / 2:.3f}")
                if shy:
                    fields.append(f"ypanning{plane} = {-shy * (2 ** s_exp) / 2:.3f}")
        out.append("sector { " + "; ".join(fields) + "; }")
    out.append("")

    # Sidedefs are emitted as we walk faces; linedefs reference them by index.
    sidedefs: list[str] = []
    linedefs: list[str] = []
    emitted: set[int] = set()

    for i, f in enumerate(faces):
        if i in emitted:
            continue
        sister_i = f.get("sisterFaceIndex")
        two_sided = sister_i is not None and sister_i < len(faces)

        tm = mappings[f["textureMappingIndex"]]
        front = len(sidedefs)
        if two_sided:
            # Upper/lower fill the height steps against the neighbour; mid is
            # usually empty or a see-through decal.
            sidedefs.append(side(f["sectorIndex"], tm,
                                 top=texname(tm["upperTextureIndex"]),
                                 bottom=texname(tm["lowerTextureIndex"]),
                                 mid=texname(tm["midTextureIndex"]) if tm["type"] & 0x01 else NO_TEX))
            sf = faces[sister_i]
            stm = mappings[sf["textureMappingIndex"]]
            back = len(sidedefs)
            sidedefs.append(side(sf["sectorIndex"], stm,
                                 top=texname(stm["upperTextureIndex"]),
                                 bottom=texname(stm["lowerTextureIndex"]),
                                 mid=texname(stm["midTextureIndex"]) if stm["type"] & 0x01 else NO_TEX))
            emitted.add(sister_i)
            # If exactly one side is a door sector, this line is the door's
            # face: give it the use-to-open special, pointed at the door side.
            # A line with doors on both sides isn't a threshold, so skip it.
            a_door = is_door(sectors[f["sectorIndex"]])
            b_door = is_door(sectors[sf["sectorIndex"]])
            extra = ""
            if a_door != b_door:
                extra = (f" special = {DOOR_SPECIAL}; playeruse = true; "
                         "repeatspecial = true; playercross = false;")
                # The special acts on the sector behind the line, so the door
                # sector must be on the BACK. Swap the sides if it isn't.
                if a_door:
                    front, back = back, front
            linedefs.append(
                f"linedef {{ v1 = {f['vertexIndex01']}; v2 = {f['vertexIndex02']}; "
                f"sidefront = {front}; sideback = {back}; twosided = true;{extra} }}"
            )
        else:
            sidedefs.append(side(f["sectorIndex"], tm, mid=texname(tm["midTextureIndex"])))
            linedefs.append(
                f"linedef {{ v1 = {f['vertexIndex01']}; v2 = {f['vertexIndex02']}; "
                f"sidefront = {front}; blocking = true; }}"
            )
        emitted.add(i)

    out.extend(linedefs)
    out.append("")
    out.extend(sidedefs)
    out.append("")

    # Player 1 start, from the map's own recorded start position.
    out.append(
        f"thing {{ x = {meta['initPosX'] * scale:.3f}; y = {meta['initPosY'] * scale:.3f}; "
        # The PLAYER's angle uses 512 units per turn, counter-clockwise, with 0
        # facing +Y -- a different convention from objects, which use 256. Doom
        # measures counter-clockwise from +X, hence the 90 degree offset.
        f"angle = {int(90 + meta['rotation'] * 360.0 / 512.0) % 360}; type = 1; skill1 = true; "
        "skill2 = true; skill3 = true; skill4 = true; skill5 = true; single = true; }"
    )

    out.extend(things or [])

    return "\n".join(out) + "\n"


def build_wad(mapname: str, textmap: str) -> bytes:
    """Minimal PWAD holding one UDMF map. Nodes are left for GZDoom to build."""
    lumps = [(mapname, b""), ("TEXTMAP", textmap.encode("utf-8")), ("ENDMAP", b"")]
    data, directory, offset = b"", b"", 12
    for name, payload in lumps:
        data += payload
        directory += struct.pack("<II", offset, len(payload)) + name.upper()[:8].ljust(8, "\0").encode("ascii")
        offset += len(payload)
    header = b"PWAD" + struct.pack("<II", len(lumps), 12 + len(data))
    return header + data + directory


B36 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
MESH_SPRITE = "RMSH"   # dummy sprite a model actor still needs
MESHES: dict = {}      # fat index -> mesh info, loaded from --meshes
FIRST_DOOMEDNUM = 20000   # 30000 is already claimed by this fork; engine mapinfo tops out at 14165 + 32000


def sprite_name(n: int) -> str:
    """Doom sprite names are exactly 4 characters. 'RT' + base36 gives 1296."""
    return "RT" + B36[n // 36] + B36[n % 36]


def resolve_object_texture(o: dict) -> tuple:
    """Map an object's texture reference to (fat_index, which_pack).

    ROTH indirects object art through `textureSource` (object_roth.gd:194-219):
        0 -> this level's pack, index + 4096
        1 -> this level's pack, index + 4096 + 256
        2 -> the shared "ademo" pack, index
        3 -> the shared "ademo" pack, index + 256
    """
    t, s = o["textureIndex"], o["textureSource"]
    return {0: (t + 4096, "level"),
            1: (t + 4096 + 256, "level"),
            2: (t, "ademo"),
            3: (t + 256, "ademo")}[s]


def build_modeldef(used_meshes: dict, tex_for: dict) -> str:
    """MODELDEF entries so ROTH's true-3D props render as models, not sprites.

    Each face group in the OBJ becomes one surface, and Skin N binds the texture
    that group used. Sentinel indices (>= 32768) are flat palette colours, which
    we already generate as PAL### textures.
    """
    out = ["// Generated by roth_pipeline/build_map.py -- do not hand-edit.\n"]
    for idx, info in sorted(used_meshes.items()):
        skins = "\n".join(
            f'    Skin {i} "{tex_for(t)}"' for i, t in enumerate(info["skins"]))
        out.append(
            f"Model RothMesh{idx}\n"
            "{\n"
            '    Path "models"\n'
            f'    Model 0 "OBJ{idx:04d}.obj"\n'
            f"{skins}\n"
            "    Scale 1.0 1.0 1.0\n"
            f"    FrameIndex {MESH_SPRITE} A 0 0\n"
            "}\n")
    return "\n".join(out)


def build_objects(m: dict, available: set, scale: float):
    """Turn ROTH's placed objects into Doom things + the actors they need.

    Doom has no way to place "a sprite" directly -- a thing references an actor
    class by number. So we synthesise one actor per distinct sprite, then place
    things pointing at them.

    Returns (things, decorate, sprite_map, skipped) where sprite_map is
    {fat_index: sprite_name} for the caller to write out.
    """
    sectors = m["sectorsSection"]["sectors"]

    wanted, skipped = [], {"ademo": 0, "no_art": 0, "mesh": 0}
    for si, s in enumerate(sectors):
        for oi, o in enumerate(s["objectInformation"]):
            idx, pack = resolve_object_texture(o)
            # Props with real geometry (tables, chairs, chests) come through as
            # models rather than sprites.
            if pack == "level" and idx in MESHES:
                wanted.append((si, oi, o, ("mesh", idx), False))
                skipped["mesh"] += 1
                continue
            if idx not in available.get(pack, ()):
                skipped["no_art"] += 1
                continue
            # renderType bit 7 means "fixed angle" -- the object hangs at its own
            # orientation rather than turning to face the player. Pictures on
            # walls, signs, anything flat-mounted. Doom sprites billboard by
            # default, which makes these swivel as you walk past; +WALLSPRITE
            # pins them.
            wanted.append((si, oi, o, (pack, idx), bool(o["renderType"] & 0x80)))

    # One sprite per (pack, index); one actor per (pack, index, facing mode),
    # since the same artwork can appear both fixed and billboarded.
    sprites = sorted({k for _, _, _, k, _ in wanted if k[0] != "mesh"})
    sprite_map = {key: sprite_name(i) for i, key in enumerate(sprites)}
    actors = sorted({(k, fx) for _, _, _, k, fx in wanted})
    ednum = {a: FIRST_DOOMEDNUM + i for i, a in enumerate(actors)}

    decorate = ["// Generated by roth_pipeline/build_map.py -- do not hand-edit.\n"]
    for (key, fixed) in actors:
        pack, idx = key
        if pack == "mesh":
            # A model actor still needs a sprite frame for its state; the model
            # replaces it visually.
            decorate.append(
                f"ACTOR RothMesh{idx} {ednum[(key, fixed)]}\n"
                "{\n"
                "    Radius 20\n"
                "    Height 56\n"
                "    +SOLID\n"
                "    States\n"
                "    {\n"
                "    Spawn:\n"
                f"        {MESH_SPRITE} A -1\n"
                "        Stop\n"
                "    }\n"
                "}\n")
            continue
        decorate.append(
            f"ACTOR RothObj{pack.capitalize()}{idx}{'Fixed' if fixed else ''} {ednum[(key, fixed)]}\n"
            "{\n"
            "    Radius 16\n"
            "    Height 32\n"
            "    +NOGRAVITY\n"
            "    +NOBLOCKMAP\n"
            + ("    +WALLSPRITE\n" if fixed else "")
            + "    States\n"
            "    {\n"
            "    Spawn:\n"
            # DECORATE state lines take NO semicolons -- that's ZScript syntax,
            # and using it here fails with "Invalid parameter ';'".
            f"        {sprite_map[key]} A -1\n"
            "        Stop\n"
            "    }\n"
            "}\n"
        )

    things = []
    for si, oi, o, key, fixed in wanted:
        # ROTH stores an absolute Z; UDMF wants height above the sector floor.
        z = int(o["posZ"] * scale) - int(sectors[si]["floorHeight"] * scale)
        # OBJECT facing is 256 units per turn measured CLOCKWISE from +Y, which
        # is the opposite sense to the player's 512-unit counter-clockwise
        # angle. Doom measures counter-clockwise from +X.
        ang = int(90 - o["rotation"] * 360.0 / 256.0) % 360
        things.append(
            f'thing {{ x = {o["posX"] * scale:.3f}; y = {o["posY"] * scale:.3f}; '
            f"height = {z}; angle = {ang}; type = {ednum[(key, fixed)]}; "
            "skill1 = true; skill2 = true; skill3 = true; skill4 = true; "
            "skill5 = true; single = true; coop = true; dm = true; "
            # Stable identity back to the source object, so hand-authored data
            # (grab points, mass, interaction tags) can be keyed to this exact
            # prop and survive the pipeline being re-run.
            f"user_roth_sector = {si}; user_roth_object = {oi}; }}"
        )

    skipped["fixed"] = sum(1 for _, _, _, _, f in wanted if f)
    used_meshes = {idx for _, _, _, (p, idx), _ in wanted if p == "mesh"}
    return things, "\n".join(decorate), sprite_map, skipped, used_meshes



def build_player_decorate(meta: dict) -> str:
    """A player actor sized the way the original engine sizes it.

    THE ORIGINAL DOUBLES THESE. `playerHeight`, `maxClimb` and `minFit` are
    stored halved in the map metadata and doubled by ROTH.C at load, giving a
    144-unit player with a 65-unit step and a 96-unit minimum gap (per
    ROTH_NATIVE_HANDOFF.md section 2.1, verified against ROTH.C).

    This matters more than it looks. Sizing the player at the raw 72 makes the
    entire world read as twice its intended size -- doorways appearing three
    times a person's height -- which is a player bug, NOT a world-scale bug.
    Do not "fix" it by shrinking the geometry; world units are 1:1.

    Read per map rather than hardcoding: three maps (ABAGATE2, AQUA1, DOPPLE)
    store 64 instead of 72.
    """
    h = int(meta.get("playerHeight", 72)) * 2
    step = int(meta.get("maxClimb", 32)) * 2
    view = round(h * 41.0 / 56.0)      # Doom's own eye-to-height ratio
    return f"""// Generated by roth_pipeline/build_map.py -- do not hand-edit.
// Sized from this map's metadata, doubled as the original engine does:
// playerHeight {h // 2}*2 = {h}, maxClimb {step // 2}*2 = {step}.

ACTOR RothPlayer : DoomPlayer
{{
    Height {h}
    Radius 28
    MaxStepHeight {step}
    Player.ViewHeight {view}
    Player.AttackZOffset {view - 8}
}}
"""

def build_animdefs(anims: dict, used: set, tics: int = 8) -> str:
    """ANIMDEFS so ROTH's animated textures actually cycle.

    Frames were written beside the first as TEXnnnn_1, _2 ... Walls and flats
    use the transposed copies, so the animation is declared on those names and
    the extra frames are transposed to match.
    """
    out = ["// Generated by roth_pipeline/tools/build_map.py -- do not hand-edit.\n"]
    prefix = "RTX" if ROTATE_WALLS else "TEX"
    for idx, count in sorted(anims.items()):
        if idx not in used:
            continue
        lines = [f'texture {prefix}{idx:04d}']
        lines.append(f"    pic {prefix}{idx:04d} tics {tics}")
        for f in range(1, count):
            lines.append(f"    pic {prefix}{idx:04d}_{f} tics {tics}")
        out.append("\n".join(lines) + "\n")
    return "\n".join(out)


def build_mapinfo(mapname: str, title: str) -> str:
    """MAPINFO so the pk3 boots straight into this map.

    `clearepisodes` plus a single episode means New Game goes directly here with
    no episode picker, and defining the map by name means `map <NAME>` and the
    automap title work properly instead of showing "Unnamed".
    """
    return f"""// Generated by roth_pipeline/build_map.py -- do not hand-edit.

map {mapname} "{title}"
{{
    levelnum = 1
    cluster = 1
    next = "{mapname}"
    secretnext = "{mapname}"
    sky1 = "F_SKY1"
    music = ""
    nointermission
    noinventorybar
}}

gameinfo
{{
    PlayerClasses = "RothPlayer"
}}

clearepisodes
episode {mapname}
{{
    name = "Realms of the Haunting"
    key = "r"
    noskillmenu
}}
"""


def build_textures_lump(names: list[tuple[str, int, int]]) -> str:
    """TEXTURES lump so GZDoom exposes each PNG as a usable wall/flat texture."""
    lines = []
    for name, w, h in names:
        lines.append(f"Texture \"{name}\", {w}, {h}\n{{\n    Patch \"{name}\", 0, 0\n}}\n")
    return "\n".join(lines)


def png_size(path: Path) -> tuple[int, int]:
    d = path.read_bytes()[16:24]
    w, h = struct.unpack(">II", d)
    return w, h


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("json", type=Path)
    ap.add_argument("--textures", type=Path, required=True,
                    help="this level's own extracted pack (from its DAS)")
    ap.add_argument("--meshes", type=Path,
                    help="directory of extracted 3D props (extract_meshes.py)")
    ap.add_argument("--sprites", type=Path,
                    help="the shared ADEMO pack, for objects whose art lives there")
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--mapname", default=None)
    ap.add_argument("--title", default=None, help="level title shown in-game")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--no-transpose-flats", action="store_true",
                    help="stop transposing floor/ceiling art (A/B for handoff 5.3)")
    ap.add_argument("--flat-fit", action="store_true",
                    help="fit flats per-sector (matches the original engine's model; "
                         "currently produces extreme scales, so off by default)")
    ap.add_argument("--no-rotate-walls", action="store_true",
                    help="disable the 90-degree wall transpose (it is correct; for A/B only)")
    ap.add_argument("--grid", action="store_true",
                    help="calibration mode: point every texture at a measurable grid "
                         "instead of the real artwork, so alignment error can be counted")
    args = ap.parse_args()

    global SKY_INDEX, PALETTE, ROTATE_WALLS, TEX_DIMS, FLAT_FIT, MESHES, TRANSPOSE_FLATS
    # Texture scale is tied to geometry scale: shrinking the world by N means a
    # texel covers N times fewer world units, so the texture scale must rise by
    # N to keep the same on-screen size. Keeping these in lockstep means --scale
    # can be changed without every surface silently resizing.
    global ROTH_WALL_SCALE, ROTH_FLAT_SCALE
    ROTH_WALL_SCALE = 0.5 / args.scale
    ROTH_FLAT_SCALE = 0.5 / args.scale
    ROTATE_WALLS = not args.no_rotate_walls
    TRANSPOSE_FLATS = not args.no_transpose_flats
    FLAT_FIT = args.flat_fit
    mapname = (args.mapname or args.json.stem).upper()[:8]
    m = json.loads(args.json.read_text())

    meta_path = args.textures / "meta.json"
    pal_path = args.textures / "palette.json"
    if meta_path.exists():
        SKY_INDEX = json.loads(meta_path.read_text())["sky_index"]
    if pal_path.exists():
        PALETTE = json.loads(pal_path.read_text())
    if not PALETTE:
        raise SystemExit("palette.json missing — re-run extract_das.py to emit it")

    pngs = sorted(args.textures.glob("TEX*.png"))
    ademo_pngs = sorted(args.sprites.glob("TEX*.png")) if args.sprites else []
    available = {
        "level": {int(p.stem[3:]) for p in pngs},
        "ademo": {int(p.stem[3:]) for p in ademo_pngs},
    }
    sprite_src = {"level": args.textures, "ademo": args.sprites}
    global TEX_DIMS
    TEX_DIMS = {int(p.stem[3:]): png_size(p) for p in pngs}
    global MESHES
    if args.meshes and (args.meshes / 'meshes.json').exists():
        MESHES = {int(k): v for k, v in
                  json.loads((args.meshes / 'meshes.json').read_text()).items()}
    things, decorate, sprite_map, obj_skipped, used_meshes = build_objects(
        m, available, args.scale)

    textmap = build_udmf(m, scale=args.scale, things=things)   # populates _used_colours
    wad = build_wad(mapname, textmap)
    tex_defs = [(p.stem, *png_size(p)) for p in pngs]
    tex_defs += [(f"PAL{c:03d}", 64, 64) for c in sorted(_used_colours)]

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(f"maps/{mapname}.wad", wad)
        z.writestr("MAPINFO.txt", build_mapinfo(mapname, args.title or mapname))
        z.writestr("DECORATE.txt",
                   build_player_decorate(m["mapMetadataSection"]) + "\n" + decorate)
        if sprite_map:
            for (pack, idx), spr in sprite_map.items():
                # Doom sprite lump naming: 4-char name + frame letter + rotation.
                z.writestr(f"sprites/{spr}A0.png",
                           (sprite_src[pack] / f"TEX{idx:04d}.png").read_bytes())

        if used_meshes:
            # A model actor still needs a real sprite lump for its spawn frame,
            # even though the model is what gets drawn.
            z.writestr(f"sprites/{MESH_SPRITE}A0.png",
                       solid_png(tuple(PALETTE[0])))
            # Models use the UNROTATED artwork -- the 90-degree transpose is a
            # wall-rendering correction and must not apply to model skins.
            def skin_for(t: int) -> str:
                # Model faces use a different flat-colour base from walls:
                # object_roth.gd resolves >= 65280 as palette[t - 65280].
                if t >= 65280:
                    return palette_tex(t - 65280)
                if t >= SENTINEL_MIN:
                    return palette_tex((t - SENTINEL_MIN) & 0xFF)
                if t in TEX_DIMS:
                    return f"TEX{t:04d}"
                return palette_tex(255)   # art missing: flat colour beats a crash
            z.writestr("MODELDEF.txt",
                       build_modeldef({i: MESHES[i] for i in used_meshes}, skin_for))
            for i in sorted(used_meshes):
                obj = (args.meshes / f"OBJ{i:04d}.obj").read_text()
                # The mesh extractor emits placeholder material names because it
                # has no idea how texture indices resolve. GZDoom insists every
                # usemtl names a real texture even when MODELDEF supplies skins,
                # so bind them here where the resolution is known.
                for n, t in enumerate(MESHES[i]["skins"]):
                    obj = obj.replace(f"usemtl ROTHSKIN{n}\n",
                                      f"usemtl {skin_for(t)}\n")
                z.writestr(f"models/OBJ{i:04d}.obj", obj)

        if args.grid:
            # Calibration build: every texture name resolves to the SAME 64x64
            # grid, declared at 64x64 regardless of what the real texture's
            # dimensions were. Keeping the original dimensions here would make
            # the grid render at a different density on every surface, which
            # defeats the entire point of a calibration texture.
            import make_grid
            g = make_grid.SIZE
            z.writestr("textures/GRID.png",
                       make_grid.png_indexed(g, g, make_grid.grid_pixels(), make_grid.PALETTE))
            lump = "".join(
                f'Texture "{n}", {g}, {g}\n{{\n    Patch "GRID", 0, 0\n}}\n\n'
                for n, _w, _h in tex_defs
            )
            z.writestr("TEXTURES.txt", lump)
        else:
            defs = list(tex_defs)
            if ROTATE_WALLS:
                for p in pngs:
                    idx = int(p.stem[3:])
                    rot = png_transpose(p.read_bytes())
                    z.writestr(f"textures/RTX{idx:04d}.png", rot)
                    w, h = struct.unpack(">II", rot[16:24])
                    defs.append((f"RTX{idx:04d}", w, h))

            # Animation frames, transposed to match the surfaces that use them.
            anim_path = args.textures / "anims.json"
            anims = ({int(k): v for k, v in json.loads(anim_path.read_text()).items()}
                     if anim_path.exists() else {})
            used_idx = {i for i in TEX_DIMS}
            for idx, count in anims.items():
                for f in range(1, count):
                    fp = args.textures / f"TEX{idx:04d}_{f}.png"
                    if not fp.exists():
                        continue
                    raw = fp.read_bytes()
                    img = png_transpose(raw) if ROTATE_WALLS else raw
                    name = f"{'RTX' if ROTATE_WALLS else 'TEX'}{idx:04d}_{f}"
                    z.writestr(f"textures/{name}.png", img)
                    w, h = struct.unpack(">II", img[16:24])
                    defs.append((name, w, h))
            if anims:
                z.writestr("ANIMDEFS.txt", build_animdefs(anims, used_idx))

            z.writestr("TEXTURES.txt", build_textures_lump(defs))
            for p in pngs:
                z.writestr(f"textures/{p.name}", p.read_bytes())
            for c in sorted(_used_colours):
                z.writestr(f"textures/PAL{c:03d}.png", solid_png(tuple(PALETTE[c])))

    # Report
    n_lines = textmap.count("linedef {")
    n_sides = textmap.count("sidedef {")
    n_sect = textmap.count("sector {")
    n_vert = textmap.count("vertex {")
    print(f"{args.out}")
    print(f"  map name ....... {mapname}")
    print(f"  vertices ....... {n_vert}")
    print(f"  linedefs ....... {n_lines}   (from {len(m['facesSection']['faces'])} ROTH faces"
          f" — sister pairs merged)")
    print(f"  sidedefs ....... {n_sides}")
    print(f"  sectors ........ {n_sect}")
    print(f"  textures ....... {len(pngs)} image + {len(_used_colours)} flat-colour")
    print(f"  sky index ...... {SKY_INDEX} -> {SKY_TEX}")
    print(f"  sky surfaces ... {textmap.count(SKY_TEX)}")
    print(f"  no-texture ..... {textmap.count(chr(34) + NO_TEX + chr(34))}")
    print(f"  objects placed . {len(things)}  ({len(sprite_map)} distinct sprites)")
    print(f"     {obj_skipped.get('fixed', 0)} wall-mounted, "
          f"{obj_skipped.get('mesh', 0)} 3D props, "
          f"{obj_skipped['no_art']} skipped for missing art")
    print(f"  TEXTMAP size ... {len(textmap) / 1024:.0f} KB")


if __name__ == "__main__":
    main()
