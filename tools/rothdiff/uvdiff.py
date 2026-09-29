#!/usr/bin/env python3
"""Compare what the two engines SAMPLED, texel by texel.

Both engines are made to paint every world texture with the number we want to
read -- the linear texel index, and which texture it belongs to -- so an
ordinary frame from either one carries that number directly. Nothing either
engine computes about coordinates sits between the sample and the pixel, which
is the point: a comparison that asks our own code what it sampled only proves
our arithmetic agrees with itself.

Four passes per camera spot per engine, because a pixel carries eight bits and
neither a texel index nor a texture id fits in eight:

    ilo  texel index, low byte      ihi  texel index, high byte
    tlo  texture id,  low byte      thi  texture id,  high byte

A pixel is SCORED where the original sampled a real texture, and it MATCHES
when both engines name the same texture and the same texel inside it. That is
a strictly harder question than "is the same surface there", which is what the
identity-buffer comparison asked and why it could not see a wrong scale, a
wrong shift or a wrong mirror even when it worked.

    python uvdiff.py <spot> [--dir captures]
    python uvdiff.py --all [--dir captures]
"""
import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PASSES = ("ilo", "ihi", "tlo", "thi")


def load_rvuv(path):
    """The oracle's frame: a 32-byte header then one byte per pixel."""
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < 32:
        raise ValueError(f"{path}: too short for a .rvuv")
    magic, ver, w, h, _pass, px, py, pang = struct.unpack_from("<5I3i", raw, 0)
    if magic != 0x56555652:
        raise ValueError(f"{path}: not a .rvuv (magic {magic:#x})")
    want = 32 + w * h
    if len(raw) < want:
        raise ValueError(f"{path}: truncated, header says {w}x{h}")
    a = np.frombuffer(raw[32:want], dtype=np.uint8).reshape(h, w)
    return a, (px, py, pang)


def load_shot(path):
    """Our frame: an ordinary screenshot.

    The pattern renders through a grey identity translation, so palette index i
    arrives as RGB(i, i, i) and any channel is the value. Red is taken, and the
    other two are checked: a pixel where they disagree has been through
    something -- a filter, a light, a postprocess -- and cannot be read back, so
    it is reported rather than quietly scored.
    """
    im = Image.open(path).convert("RGB")
    a = np.asarray(im)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    tainted = int(np.count_nonzero((r != g) | (g != b)))
    return r, tainted


def spot_files(d, spot, side):
    ext = "rvuv" if side == "roth" else "png"
    return {p: os.path.join(d, f"{spot}.{side}.{p}.{ext}") for p in PASSES}


def compare(d, spot):
    ro = spot_files(d, spot, "roth")
    re = spot_files(d, spot, "rema")
    missing = [p for p in PASSES if not os.path.exists(ro[p])] \
            + [p for p in PASSES if not os.path.exists(re[p])]
    if missing:
        return {"spot": spot, "error": f"not captured ({len(missing)} pass file(s) missing)"}

    R = {}
    for p in PASSES:
        R[p], _pose = load_rvuv(ro[p])
    E = {}
    tainted = 0
    for p in PASSES:
        E[p], t = load_shot(re[p])
        tainted += t

    if R["ilo"].shape != E["ilo"].shape:
        return {"spot": spot,
                "error": f"size mismatch {R['ilo'].shape[::-1]} vs {E['ilo'].shape[::-1]}"
                         " -- capture both at the same resolution"}

    rt = R["tlo"].astype(np.uint16) | (R["thi"].astype(np.uint16) << 8)
    et = E["tlo"].astype(np.uint16) | (E["thi"].astype(np.uint16) << 8)
    ri = R["ilo"].astype(np.uint16) | (R["ihi"].astype(np.uint16) << 8)
    ei = E["ilo"].astype(np.uint16) | (E["ihi"].astype(np.uint16) << 8)

    # Scored where the ORIGINAL sampled a texture at all. Anywhere it drew
    # nothing -- sky, the menu, an unpainted fill -- there is no texel to be
    # right or wrong about, and counting those would inflate the number with
    # pixels the test never reached.
    scored = rt != 0
    n = int(np.count_nonzero(scored))
    if n == 0:
        return {"spot": spot, "error": "the original sampled no texture here"}

    same_tex = (rt == et) & scored
    same_texel = same_tex & (ri == ei)
    matched = int(np.count_nonzero(same_texel))

    wrong_tex = n - int(np.count_nonzero(same_tex))
    wrong_texel = int(np.count_nonzero(same_tex)) - matched

    return {
        "spot": spot,
        "pixels": int(scored.size),
        "scored": n,
        "matched": matched,
        "match_pct": 100.0 * matched / n,
        "wrong_texture": wrong_tex,
        "wrong_texel": wrong_texel,
        "tainted": tainted,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("spot", nargs="?")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--dir", default=os.path.join(HERE, "captures"))
    a = ap.parse_args()

    if a.all:
        seen = sorted({f.split(".")[0] for f in os.listdir(a.dir)
                       if f.endswith(".rvuv")})
    elif a.spot:
        seen = [a.spot]
    else:
        ap.error("give a spot name or --all")

    print(f"{'spot':16s} {'scored':>9s} {'match':>8s}  {'wrong tex':>9s} {'wrong texel':>11s}")
    print("-" * 62)
    worst = 100.0
    for s in seen:
        r = compare(a.dir, s)
        if "error" in r:
            print(f"{s:16s} {'--':>9s} {'--':>8s}  {r['error']}")
            continue
        print(f"{s:16s} {r['scored']:9d} {r['match_pct']:7.2f}%  "
              f"{r['wrong_texture']:9d} {r['wrong_texel']:11d}"
              + ("   TAINTED PIXELS -- filtering or lighting is still on"
                 if r["tainted"] else ""))
        worst = min(worst, r["match_pct"])
    return 0 if worst >= 99.99 else 1


if __name__ == "__main__":
    sys.exit(main())
