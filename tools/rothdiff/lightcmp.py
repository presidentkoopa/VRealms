#!/usr/bin/env python3
"""lightcmp.py ORIGINAL.png OURS.png [--grid CxR] [--out side.png]

Region-by-region brightness comparison of one original frame (ROTH.C oracle, 640x480,
oracle_shot.sh) against one of ours (rothdiff_shot, any size).

Our frame is mapped onto the original's: the original projects with FX=309.77, FY=355.06
(docs/REMAROTH_MEASURED.md section 1) over a 4:3 screen, and our flat view is the same
projection (rothmap.cpp: 91.82 degrees + r_view_vstretch 1.1462) but Hor+ on a wider window.
So the original's screen is the CENTRAL part of ours whose width is 4/3 * height * FX/FY^-1:
  ours px per tan unit (vertical) = (H/2) / tan(vfov/2) = (H/2) / (240/355.06)
  original's half width in tan units = 320/309.77
The central crop is resized to 640x480 and both are compared on a grid of cells (mean of
Rec.601 luma, 0..255). The HUD corner (x<100, y<40) and the bottom-right icon are masked.

Prints one table per frame: orig / ours / ours-orig per cell, then the whole-frame means and the
median absolute difference. Exit code 0. Pure host tool: reads two PNGs, writes nothing unless
--out is given."""
import sys
import numpy as np
from PIL import Image

FX, FY = 309.77, 355.06

def luma(a):
    a = a.astype(np.float64)
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]

def ours_to_original_frame(img):
    w, h = img.size
    ppt = (h / 2.0) / (240.0 / FY)          # our pixels per tan unit (both axes: same projection)
    half_w = (320.0 / FX) * ppt              # the original's half width, in our pixels
    x0, x1 = w / 2.0 - half_w, w / 2.0 + half_w
    if x0 < -0.5:
        sys.stderr.write("lightcmp: our frame is narrower than 4:3 (%dx%d); comparing what overlaps\n" % (w, h))
    crop = img.crop((int(round(x0)), 0, int(round(x1)), h))
    return crop.resize((640, 480), Image.NEAREST)

def main():
    args = sys.argv[1:]
    grid = (8, 6); out = None
    if '--grid' in args:
        i = args.index('--grid'); c, r = args[i + 1].split('x'); grid = (int(c), int(r)); del args[i:i + 2]
    if '--out' in args:
        i = args.index('--out'); out = args[i + 1]; del args[i:i + 2]
    if len(args) != 2:
        print(__doc__); return 2
    o = Image.open(args[0]).convert('RGB')
    if o.size != (640, 480):
        o = o.resize((640, 480), Image.NEAREST)
    u = ours_to_original_frame(Image.open(args[1]).convert('RGB'))
    lo, lu = luma(np.asarray(o)), luma(np.asarray(u))
    mask = np.ones((480, 640), bool)
    mask[:40, :100] = False          # HUD (original)
    mask[460:, 600:] = False         # inventory icon (original)
    C, R = grid
    cw, ch = 640 // C, 480 // R
    print("cell grid %dx%d (each %dx%d px of the original's 640x480); luma 0..255" % (C, R, cw, ch))
    print("rows: original | ours | ours - original")
    diffs = []
    for name, f in (("orig", lambda a, b: a), ("ours", lambda a, b: b), ("diff", lambda a, b: b - a)):
        print(name)
        for r in range(R):
            line = []
            for c in range(C):
                m = mask[r * ch:(r + 1) * ch, c * cw:(c + 1) * cw]
                a = lo[r * ch:(r + 1) * ch, c * cw:(c + 1) * cw][m]
                b = lu[r * ch:(r + 1) * ch, c * cw:(c + 1) * cw][m]
                if a.size == 0:
                    line.append("   --"); continue
                v = f(a.mean(), b.mean())
                if name == "diff": diffs.append(v)
                line.append("%+5.1f" % v if name == "diff" else "%5.1f" % v)
            print("  " + " ".join(line))
    print("whole frame mean: original %.1f  ours %.1f   median |cell diff| %.1f   max |cell diff| %.1f" % (
        lo[mask].mean(), lu[mask].mean(), float(np.median(np.abs(diffs))), float(np.max(np.abs(diffs)))))
    if out:
        s = Image.new('RGB', (1290, 480), (40, 40, 40)); s.paste(o, (0, 0)); s.paste(u, (650, 0)); s.save(out)
    return 0

if __name__ == '__main__':
    sys.exit(main())
