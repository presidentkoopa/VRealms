#!/usr/bin/env python3
"""
Read a ROTH .DAS texture pack: survey its contents, or extract images as PNGs.

Port of the structural parts of roth-editor's src/resources/parsers/das.gd.

Images are 8-bit palette-indexed, so they're written as indexed-colour PNGs
(PNG colour type 3 + PLTE) — the original palette survives byte-for-byte, which
keeps the 1996 look intact and is a format GZDoom ingests directly. No upscaling,
no resampling, no dependencies.

Usage:
    python extract_das.py DEMO.DAS --survey
    python extract_das.py DEMO.DAS -o out/textures_demo
"""

import argparse
import struct
import zlib
from pathlib import Path

# image_type bits
ANIMATION = 1 << 0
PALETTE_ZERO_OPAQUE = 1 << 1
TRANSPARENT = 1 << 2
OBJECT_DATA = 1 << 7
# modifier bits
IMAGE_PACK = 1 << 6
# flags_1 bits
F1_MONSTER = 1 << 2
F1_DIRECTIONAL = 1 << 5


class R:
    def __init__(self, d, p=0):
        self.d, self.p = d, p

    def u8(self):
        v = self.d[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]; self.p += 4; return v

    def s(self, n):
        v = self.d[self.p:self.p + n]; self.p += n; return v


def parse_header(d: bytes) -> dict:
    r = R(d)
    magic = r.s(4)
    if magic != b"DASP":
        raise ValueError(f"not a DAS file (magic={magic!r})")
    h = {
        "das_id_num": r.u16(), "size_fat": r.u16(),
        "img_fat_offset": r.u32(), "palette_offset": r.u32(),
        "unk_0x10_offset": r.u32(), "filenames_offset": r.u32(),
        "filenames_size": r.u16(), "directional_object_table_size": r.u16(),
        "directional_object_table_offset": r.u32(),
        "unk_0x20": r.u16(), "sky_index": r.u16(),
        "object_collision_section_offset": r.u32(),
        "monster_mapping_section_offset": r.u32(),
        "monster_mapping_section_size": r.u32(),
        "fat_block_1_count": r.u16(), "fat_block_2_count": r.u16(),
        "fat_block_3_count": r.u16(), "fat_block_4_count": r.u16(),
    }
    h["image_count"] = sum(h[f"fat_block_{i}_count"] for i in (1, 2, 3, 4))
    return h


def parse_palette(d: bytes, offset: int):
    """256 RGB triples stored as VGA 6-bit (0-63), expanded to 8-bit.

    `palette_offset == 0` is not an error -- it means "use the engine's built-in
    palette", which ADEMO.DAS (the shared sprite pack for every map) does. That
    table is reproduced in default_palette.py.
    """
    if offset == 0:
        from default_palette import DEFAULT_RAW_PALETTE as P
        return [tuple((P[i * 3 + c] * 259 + 33) >> 6 for c in range(3))
                for i in range(256)]
    out = []
    for i in range(256):
        r, g, b = d[offset + i * 3: offset + i * 3 + 3]
        out.append(tuple((v * 259 + 33) >> 6 for v in (r, g, b)))
    return out


def parse_fat(d: bytes, h: dict):
    base = h["img_fat_offset"]
    fat = []
    for i in range(h["image_count"]):
        off, size, f1, f2 = struct.unpack_from("<IHBB", d, base + i * 8)
        fat.append({"index": i, "offset": off, "size": size, "flags_1": f1, "flags_2": f2})
    return fat


def classify(d: bytes, e: dict) -> str:
    """What kind of entry is this? Determines whether we can decode it simply."""
    if e["size"] == 0 or e["offset"] == 0 or e["offset"] + 6 > len(d):
        return "empty"
    if e["flags_1"] & F1_MONSTER:
        return "monster-ref"
    if e["flags_1"] & F1_DIRECTIONAL:
        return "directional-ref"
    modifier, image_type = d[e["offset"]], d[e["offset"] + 1]
    if image_type & OBJECT_DATA:
        return "3d-object"
    if image_type & ANIMATION:
        return "animated"
    if modifier & IMAGE_PACK:
        return "image-pack"
    return "plain"


def read_animated(d: bytes, e: dict):
    """First frame of an animated image.

    Layout (type 1, `num_sub_images != 0xFFFE`):
        IMAGE_COMPRESSED_1_HEADER   18 bytes
        num_sub_images x uint32     frame offsets
        zero padding                up to start + first_image_offset
        mini header                 modifier, image_type, width, height (6 bytes)
        width*height bytes          FIRST FRAME, UNCOMPRESSED  <-- what we want
        ... delta-compressed frames after that

    Only the first frame is taken. Animation would need the delta decoder and a
    multi-frame sprite; for a static prop this is the right frame anyway.

    Packs using the built-in palette prefix each entry with 4 bytes of shift
    data, so the header may start at +0 or +4. Both are tried and the one giving
    plausible dimensions wins.
    """
    for base in (e["offset"], e["offset"] + 4):
        if base + 18 > len(d):
            continue
        try:
            (_mod, _it, w, h, _total, first_off,
             nsub) = struct.unpack_from("<BBHHIHH", d, base)
        except struct.error:
            continue
        if not (0 < w <= 4096 and 0 < h <= 4096) or nsub == 0xFFFE:
            continue
        img_at = base + first_off
        if img_at + 6 + w * h > len(d):
            continue
        w2, h2 = struct.unpack_from("<HH", d, img_at + 2)
        if not (0 < w2 <= 4096 and 0 < h2 <= 4096):
            continue
        px = d[img_at + 6: img_at + 6 + w2 * h2]
        if len(px) != w2 * h2:
            continue
        return {"w": w2, "h": h2, "px": px, "modifier": 0, "image_type": 0}
    return None


def read_image_pack(d: bytes, e: dict):
    """First image of an image-pack entry.

    Two variants, both after the 6-byte standard header:

    A) directional pack (`pack_type & 0x80`): a byte count and a pack type, then
       uint16 offsets whose low 11 bits identify unique sub-images; the images
       themselves start at entry+32.
    B) 3D-object texture pack: a zero-terminated run of uint16s gives the image
       count, then the images follow.

    Both store each sub-image as modifier/type/width/height followed by raw
    palette bytes, padded to a 16-byte alignment established by the first one.
    Only the first sub-image is taken -- enough for a static prop or a texture
    on a model face.
    """
    base = e["offset"]
    if base + 8 > len(d):
        return None
    pack_type = d[base + 7]

    def read_sub(p: int):
        if p + 6 > len(d):
            return None
        w, h = struct.unpack_from("<HH", d, p + 2)
        if not (0 < w <= 4096 and 0 < h <= 4096) or p + 6 + w * h > len(d):
            return None
        return {"w": w, "h": h, "px": d[p + 6: p + 6 + w * h],
                "modifier": d[p], "image_type": d[p + 1]}

    if pack_type & 0x80:
        return read_sub(base + 32)

    # Variant B: walk the uint16 table to its terminator, then skip zero padding.
    p = base + 4
    while p + 2 <= len(d) and struct.unpack_from("<H", d, p)[0] != 0:
        p += 2
    p += 2
    while p < len(d) and d[p] == 0:
        p += 1
    return read_sub(p - 1)


def read_plain(d: bytes, e: dict):
    r = R(d, e["offset"])
    modifier, image_type = r.u8(), r.u8()
    w, h = r.u16(), r.u16()
    if w <= 0 or h <= 0 or w > 4096 or h > 4096:
        return None
    px = r.s(w * h)
    if len(px) != w * h:
        return None
    return {"w": w, "h": h, "px": px, "modifier": modifier, "image_type": image_type}


def png_indexed(w: int, h: int, px: bytes, palette, transparent_zero: bool) -> bytes:
    raw = b"".join(b"\x00" + px[y * w:(y + 1) * w] for y in range(h))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
    out += chunk(b"PLTE", b"".join(bytes(c) for c in palette))
    if transparent_zero:
        out += chunk(b"tRNS", b"\x00")  # index 0 fully transparent
    out += chunk(b"IDAT", zlib.compress(raw, 9))
    out += chunk(b"IEND", b"")
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("das", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--survey", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    d = args.das.read_bytes()
    h = parse_header(d)
    fat = parse_fat(d, h)
    palette = parse_palette(d, h["palette_offset"])

    kinds = {}
    for e in fat:
        e["kind"] = classify(d, e)
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1

    print(f"{args.das.name}  ({len(d) / 1e6:.1f} MB)")
    print(f"  images in FAT ...... {h['image_count']}")
    print(f"  block counts ....... {h['fat_block_1_count']} / {h['fat_block_2_count']}"
          f" / {h['fat_block_3_count']} / {h['fat_block_4_count']}")
    print(f"  palette ............ {chr(39)}embedded{chr(39)} if h[chr(39)]palette_offset{chr(39)}] else {chr(39)}built-in default{chr(39)}")
    print(f"  sky index .......... {h['sky_index']}")
    print("  entry kinds:")
    for k, n in sorted(kinds.items(), key=lambda kv: -kv[1]):
        print(f"      {k:16} {n}")

    if args.survey or not args.out:
        sizes = []
        for e in fat:
            if e["kind"] == "plain":
                img = read_plain(d, e)
                if img:
                    sizes.append((img["w"], img["h"]))
        if sizes:
            common = {}
            for s in sizes:
                common[s] = common.get(s, 0) + 1
            top = sorted(common.items(), key=lambda kv: -kv[1])[:8]
            print("  most common plain-image sizes:")
            for (w, ht), n in top:
                print(f"      {w}x{ht:<6} {n}")
        return

    if palette is None:
        print("  !! embedded palette absent; default-palette fallback not implemented yet")
        return

    args.out.mkdir(parents=True, exist_ok=True)

    # The palette and sky index are needed downstream to resolve flat-colour
    # sentinels and sky surfaces, so publish them alongside the images.
    import json
    (args.out / "palette.json").write_text(json.dumps(palette))
    (args.out / "meta.json").write_text(json.dumps({
        "sky_index": h["sky_index"],
        "image_count": h["image_count"],
        "source": args.das.name,
    }))

    written = skipped = 0
    for e in fat:
        if e["kind"] == "plain":
            img = read_plain(d, e)
        elif e["kind"] == "animated":
            img = read_animated(d, e)       # first frame only
        elif e["kind"] == "image-pack":
            img = read_image_pack(d, e)     # first sub-image only
        else:
            skipped += 1
            continue
        if not img:
            skipped += 1
            continue
        transparent = not (img["image_type"] & PALETTE_ZERO_OPAQUE)
        png = png_indexed(img["w"], img["h"], img["px"], palette, transparent)
        (args.out / f"TEX{e['index']:04d}.png").write_bytes(png)
        written += 1
        if args.limit and written >= args.limit:
            break

    print(f"\n  wrote {written} PNGs to {args.out}  (skipped {skipped} non-plain entries)")


if __name__ == "__main__":
    main()
