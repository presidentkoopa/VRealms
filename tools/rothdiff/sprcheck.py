"""Compare rothdiff_sprites output against the original's measured rectangles.

The reference numbers are the original's own billboard rectangles, recorded in
docs/REMAROTH_MEASURED.md section 6. Negative dy means ours is HIGHER on screen.

    python sprcheck.py            # uses captures/spr_*.txt

Capture them with captures/sprites.cfg. Angles are passed as written; the
(512 - angle) compensation section 6 used to need is gone with the
RothAngleToDoom fix.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "captures")

# file, sprite name, object x, y, then the ORIGINAL's x0 x1 y0 y1 depth
REF = [
    ("spr_256.txt", "ROTH_DEMO_O04102", "904.0", "3606.0", 231, 304, 218, 482, 234),
    ("spr_320.txt", "ROTH_DEMO_O04102", "904.0", "3606.0", 495, 583, 213, 533, 193),
    ("spr_081.txt", "ROTH_ADEMO_O00045", "-560.0", "-380.0", 181, 241, 442, 525, 184),
    ("spr_081.txt", "ROTH_ADEMO_O00045", "-672.0", "-232.0", 303, 333, 343, 386, 358),
]


def find(fname, sprite, ox, oy):
    path = os.path.join(CAP, fname)
    if not os.path.exists(path):
        return None
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 10 and p[0] == "S" and p[6] == sprite \
                    and p[7] == ox and p[8] == oy:
                return [float(v) for v in (p[1], p[2], p[3], p[4], p[5])] + [float(p[9])]
    return None


def main():
    print("%-20s %-10s %9s %9s %9s %9s %7s"
          % ("prop", "pose", "dx0", "dx1", "dytop", "dybot", "z"))
    worst = 0.0
    for fname, sprite, ox, oy, X0, X1, Y0, Y1, D in REF:
        got = find(fname, sprite, ox, oy)
        pose = fname.replace("spr_", "").replace(".txt", "")
        if got is None:
            print("%-20s %-10s  NOT FOUND" % (sprite.replace("ROTH_", ""), pose))
            continue
        gx0, gx1, gy0, gy1, gd, gz = got
        d = (gx0 - X0, gx1 - X1, gy0 - Y0, gy1 - Y1)
        worst = max(worst, abs(d[2]), abs(d[3]))
        flag = "" if max(abs(d[2]), abs(d[3])) < 2.0 else "   <-- off"
        print("%-20s %-10s %+9.1f %+9.1f %+9.1f %+9.1f %7.1f%s"
              % (sprite.replace("ROTH_", ""), pose, d[0], d[1], d[2], d[3], gz, flag))
    print()
    print("worst vertical error: %.1f px" % worst)
    return 0 if worst < 2.0 else 1


if __name__ == "__main__":
    sys.exit(main())
