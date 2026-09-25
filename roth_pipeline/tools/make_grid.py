#!/usr/bin/env python3
"""
Generate a calibration grid texture, and a TEXTURES lump that points every
texture name at it.

Why: debugging texture alignment against actual wood-panelling and stone is
guesswork — "that looks a bit off" isn't measurable. Against a regular grid with
a marked origin, misalignment becomes countable: you can read the offset error
straight off the squares and work back to the formula.

The grid is 64x64 (one Doom "flat" unit) with:
  - a 1px border so tile boundaries are obvious
  - crosshair lines at the halfway point
  - a bright corner marker so rotation/flip is visible at a glance

Usage:
    python make_grid.py -o out/grid
"""

import argparse
import struct
import zlib
from pathlib import Path

SIZE = 64

# A small deliberately garish palette — nothing in the real game looks like this,
# so grid surfaces are unmistakable.
PALETTE = [
    (18, 18, 24),     # 0 background (dark)
    (235, 235, 235),  # 1 grid lines (white)
    (232, 60, 60),    # 2 origin corner (red)
    (60, 200, 255),   # 3 centre crosshair (cyan)
]


def grid_pixels() -> bytes:
    px = bytearray(SIZE * SIZE)  # all index 0
    for y in range(SIZE):
        for x in range(SIZE):
            i = y * SIZE + x
            on_border = x == 0 or y == 0 or x == SIZE - 1 or y == SIZE - 1
            on_eighth = (x % 8 == 0) or (y % 8 == 0)
            on_centre = x == SIZE // 2 or y == SIZE // 2
            if x < 8 and y < 8:
                px[i] = 2          # origin marker block
            elif on_centre:
                px[i] = 3
            elif on_border:
                px[i] = 1
            elif on_eighth:
                px[i] = 1
    return bytes(px)


def png_indexed(w: int, h: int, px: bytes, palette) -> bytes:
    raw = b"".join(b"\x00" + px[y * w:(y + 1) * w] for y in range(h))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    pal = b"".join(bytes(c) for c in palette)
    pal += b"\x00" * (768 - len(pal))
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
    out += chunk(b"PLTE", pal)
    out += chunk(b"IDAT", zlib.compress(raw, 9))
    out += chunk(b"IEND", b"")
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "GRID.png").write_bytes(png_indexed(SIZE, SIZE, grid_pixels(), PALETTE))
    print(f"wrote {args.out / 'GRID.png'}  ({SIZE}x{SIZE})")


if __name__ == "__main__":
    main()
