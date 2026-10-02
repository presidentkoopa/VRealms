"""Mean grey in vertical strips across a region, for both engines side by side.

The shape of a lighting difference says what kind it is. A candle sector's shade
carries a SCREEN-POSITION term -- the original adds 1024 * max(|tx|,|ty|) to the
depth, tx and ty being the screen offsets from the view centre over the focal
length -- so a candle mismatch darkens towards the edges of the frame and shows
as a gradient. A sector whose light byte or lights-out state is wrong is flat
across the strip instead. Those want completely different fixes.

    python strips.py <orig.png> <ours.png> x0 y0 x1 y1 [nstrips]
"""

import sys
import os

try:
    from PIL import Image
except ImportError:
    sys.exit("strips: needs Pillow")


def mean(im, box):
    px = list(im.crop(box).convert("L").getdata())
    return sum(px) / len(px) if px else 0.0


def main(argv):
    if len(argv) < 7:
        sys.exit(__doc__)
    a = Image.open(argv[1])
    b = Image.open(argv[2])
    x0, y0, x1, y1 = (int(v) for v in argv[3:7])
    n = int(argv[7]) if len(argv) > 7 else 6
    if a.size != b.size:
        sys.exit("frames differ in size: %s vs %s" % (a.size, b.size))

    cx = a.size[0] / 2.0
    w = (x1 - x0) / float(n)
    print("%-16s %8s %8s %8s %8s" % ("strip", "|x-cx|", "ORIG", "OURS", "diff"))
    for i in range(n):
        sx0 = int(round(x0 + i * w))
        sx1 = int(round(x0 + (i + 1) * w))
        box = (sx0, y0, sx1, y1)
        ma, mb = mean(a, box), mean(b, box)
        mid = (sx0 + sx1) / 2.0
        print("x %4d-%-4d    %8.0f %8.1f %8.1f %+8.1f"
              % (sx0, sx1, abs(mid - cx), ma, mb, mb - ma))
    print()
    print("%s vs %s" % (os.path.basename(argv[1]), os.path.basename(argv[2])))
    print("A FLAT diff down the column points at the sector (light byte or")
    print("lights-out state). A diff that GROWS with |x-cx| points at the")
    print("candle cone's screen-position term.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
