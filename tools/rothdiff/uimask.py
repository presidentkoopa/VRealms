"""Build a mask of the HUD overlays so a pair of frames can be compared on
world pixels only. LANE_START step 1 item 4.

The original draws things over the picture that are not the picture: a health
bar top left, a hand cursor, and a held-item icon bottom right. Ours draws none
of them. Counting them as world pixels makes every comparison slightly wrong and
makes the regions they sit in badly wrong.

HOW THEY ARE FOUND

  STATIC   A pixel that is IDENTICAL across two DIFFERENT views of the same
           engine is not the world, because the world changes completely
           between poses. Restricted to pixels bright enough to matter: unlit
           black agrees with unlit black everywhere and would swallow the frame.

Dark textured wallpaper still throws up a couple of hundred coincidental
agreements, scattered one and two at a time. An overlay is a solid object, so
components smaller than MIN_COMPONENT are dropped before the mask is grown by a
pixel to catch edge blending.

TWO THINGS THIS DELIBERATELY DOES NOT DO

  THE GREEN CROSSHAIR IS NOT AN OVERLAY. LANE_START item 4 calls it "a green
  reticle" carried by both frames. It is world-anchored: it sits at the left
  edge at pose A and the right edge at pose C, where a screen-fixed HUD element
  would be in the same place in both, and our engine contains no reticle code
  while still drawing it. Both engines draw it in the same place, so it cancels
  in a comparison. Masking it would delete real geometry.

  COLOUR CANNOT FIND IT ANYWAY. An earlier version of this file masked
  green-dominant pixels. Measured on STUDY1: the crosshair uses rgb(0,81,40),
  (0,93,49), (0,113,61); the green-jewelled item on the pose C wall uses
  rgb(0,85,40) and (0,93,49). (0,93,49) is THE SAME PALETTE ENTRY in both.
  There is no threshold that separates them. Do not reintroduce a hue rule.

  THE HAND CURSOR IS NOT CAUGHT. It is opaque, but it moves between poses -- it
  is over the wainscot at pose A and out of frame at pose C -- so the static
  rule cannot see it. Mask it with --rect when a frame carries it; at pose A it
  is x 388-405, y 268-291.

Usage:
    python uimask.py <poseA.png> <poseB.png> [--rect x0,y0,x1,y1] [--out m.png]

Import `build_mask` to use it from a comparison script.
"""

import sys
import os

try:
    from PIL import Image
except ImportError:
    sys.exit("uimask: needs Pillow (pip install pillow)")


# A pixel must be at least this bright before "identical across two views"
# counts as evidence of an overlay. Below it, agreement is shared darkness.
STATIC_MIN_LEVEL = 24

# Smallest run of touching pixels that can be an overlay rather than a
# coincidence. The health bar and the held item are both hundreds.
MIN_COMPONENT = 12


def _load(path):
    im = Image.open(path).convert("RGB")
    return im, im.load(), im.size


def build_mask(path_a, path_b, rects=()):
    """True where a pixel is overlay, not world. The two frames must be the
    same size and show DIFFERENT views of the SAME engine."""
    _, pa, (w, h) = _load(path_a)
    _, pb, size_b = _load(path_b)
    if size_b != (w, h):
        raise ValueError("frames differ in size: %s vs %s" % ((w, h), size_b))

    mask = [[False] * w for _ in range(h)]
    static = 0
    for y in range(h):
        row = mask[y]
        for x in range(w):
            ca = pa[x, y]
            if ca == pb[x, y] and max(ca) >= STATIC_MIN_LEVEL:
                row[x] = True
                static += 1

    kept, dropped = _drop_small(mask, w, h)

    forced = 0
    for x0, y0, x1, y1 in rects:
        for y in range(max(0, y0), min(h, y1)):
            for x in range(max(0, x0), min(w, x1)):
                if not kept[y][x]:
                    forced += 1
                kept[y][x] = True

    grown = _grow(kept, w, h)
    total = sum(sum(1 for v in row if v) for row in grown)
    return grown, {"static": static, "speckle": dropped, "rects": forced,
                   "total": total, "w": w, "h": h}


def _drop_small(mask, w, h):
    """Remove components below MIN_COMPONENT. Iterative flood fill: the frames
    are small, but a recursive one still blows the stack on a long thin run."""
    seen = [[False] * w for _ in range(h)]
    out = [[False] * w for _ in range(h)]
    dropped = 0
    for y0 in range(h):
        for x0 in range(w):
            if not mask[y0][x0] or seen[y0][x0]:
                continue
            stack = [(x0, y0)]
            seen[y0][x0] = True
            comp = []
            while stack:
                x, y = stack.pop()
                comp.append((x, y))
                for dy in (-1, 0, 1):
                    yy = y + dy
                    if yy < 0 or yy >= h:
                        continue
                    for dx in (-1, 0, 1):
                        xx = x + dx
                        if 0 <= xx < w and mask[yy][xx] and not seen[yy][xx]:
                            seen[yy][xx] = True
                            stack.append((xx, yy))
            if len(comp) >= MIN_COMPONENT:
                for x, y in comp:
                    out[y][x] = True
            else:
                dropped += len(comp)
    return out, dropped


def _grow(mask, w, h):
    out = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if not mask[y][x]:
                continue
            for dy in (-1, 0, 1):
                yy = y + dy
                if yy < 0 or yy >= h:
                    continue
                for dx in (-1, 0, 1):
                    xx = x + dx
                    if 0 <= xx < w:
                        out[yy][xx] = True
    return out


def _parse_rects(argv):
    rects = []
    for i, a in enumerate(argv):
        if a == "--rect":
            v = [int(n) for n in argv[i + 1].split(",")]
            if len(v) != 4:
                sys.exit("--rect wants x0,y0,x1,y1")
            rects.append(tuple(v))
    return rects


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    a, b = argv[1], argv[2]
    mask, stats = build_mask(a, b, _parse_rects(argv))
    pct = 100.0 * stats["total"] / (stats["w"] * stats["h"])
    print("%s + %s" % (os.path.basename(a), os.path.basename(b)))
    print("  static across views : %d px" % stats["static"])
    print("  dropped as speckle  : %d px" % stats["speckle"])
    print("  added by --rect     : %d px" % stats["rects"])
    print("  masked after grow   : %d px  (%.2f%% of frame)"
          % (stats["total"], pct))

    if "--out" in argv:
        out = argv[argv.index("--out") + 1]
        im = Image.open(a).convert("RGB")
        px = im.load()
        for y in range(stats["h"]):
            for x in range(stats["w"]):
                if mask[y][x]:
                    px[x, y] = (255, 0, 255)
        im.save(out)
        print("  preview             : %s (masked pixels in magenta)" % out)


if __name__ == "__main__":
    main(sys.argv)
