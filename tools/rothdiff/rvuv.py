#!/usr/bin/env python3
"""Read and sanity-check .rvuv texel buffers.

A .rvuv is one byte per pixel: the texel coordinate ROTH.C actually sampled
there, obtained by repainting every texture as its own column (U pass) or row
(V pass) and letting the original do the sampling. See uvcapture.inc.c.

    magic "RVUV" u32 | version u32 | width u32 | height u32
    pass u32 (1=U, 2=V) | pose x,y,ang i32 x3
    then w*h bytes
"""
import struct
import sys

MAGIC = 0x56555652
HDR = 32


def load(path):
    with open(path, "rb") as f:
        raw = f.read()
    magic, ver, w, h, p = struct.unpack_from("<5I", raw, 0)
    if magic != MAGIC:
        raise ValueError(f"{path}: not a .rvuv")
    px, py, pang = struct.unpack_from("<3i", raw, 20)
    return {"w": w, "h": h, "pass": p, "pose": (px, py, pang),
            "px": raw[HDR:HDR + w * h]}


def sanity(path):
    b = load(path)
    px = b["px"]
    w, h = b["w"], b["h"]
    name = "U" if b["pass"] == 1 else "V"
    print(f"{path}")
    print(f"  {w}x{h}  pass {name}  pose {b['pose']}")

    # A coordinate buffer should use a lot of distinct values; a broken capture
    # is usually one flat colour or a handful.
    seen = set(px)
    print(f"  distinct byte values: {len(seen)}")

    # Along a horizontal scanline through the middle, a U buffer should mostly
    # step by small amounts; large jumps are surface boundaries.
    mid = h // 2
    row = px[mid * w:(mid + 1) * w]
    steps = [((row[i + 1] - row[i]) % 256) for i in range(w - 1)]
    small = sum(1 for s in steps if s <= 4 or s >= 252)
    print(f"  mid-row: {small}/{len(steps)} adjacent pixels differ by <=4 "
          f"({100.0 * small / len(steps):.1f}% locally smooth)")
    print(f"  mid-row sample: {list(row[w//4:w//4+16])}")
    return b


if __name__ == "__main__":
    bufs = [sanity(p) for p in sys.argv[1:]]
    if len(bufs) == 2:
        a, c = bufs
        same = sum(1 for i in range(len(a["px"])) if a["px"][i] == c["px"][i])
        n = len(a["px"])
        print(f"\nU and V identical on {same}/{n} pixels "
              f"({100.0 * same / n:.2f}%)")
        print("  (they SHOULD differ over most of the picture; near-100% here "
              "would mean the repaint did not take)")
