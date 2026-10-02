"""List the DIRECTIONAL props in a rothdiff_sprites sweep, and propose an orbit.

A directional prop is one the dump reports with rots > 1. Everything else
repeats rotation 0, so its picture cannot say anything about the view order.

    python dirprops.py captures/s2_q*.txt

Prints each distinct directional prop once, with its position, and for the
closest one emits the eight rothdiff_shot lines of an orbit around it -- one per
view, 45 degrees apart, which is what settles HANDOFF_REMAROTH.md section 7.
"""

import sys
import glob
import math
import os

# S xl xr yt yb depth NAME x y z rots=N rot=N view=NAME
I_DEPTH, I_NAME, I_X, I_Y, I_Z = 5, 6, 7, 8, 9


def parse(paths):
    props = {}
    for p in paths:
        with open(p) as f:
            for line in f:
                t = line.split()
                if len(t) < 13 or t[0] != "S":
                    continue
                kv = {}
                for tok in t[10:]:
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        kv[k] = v
                rots = int(kv.get("rots", "1"))
                if rots <= 1:
                    continue
                key = (t[I_NAME], t[I_X], t[I_Y], t[I_Z])
                props.setdefault(key, {"rots": rots, "views": set()})
                props[key]["views"].add(kv.get("view", "-"))
    return props


def main(argv):
    paths = []
    for a in argv[1:]:
        paths.extend(glob.glob(a))
    if not paths:
        sys.exit("usage: dirprops.py <sprite dump files>")

    props = parse(paths)
    if not props:
        print("no directional props (rots > 1) in %d file(s)" % len(paths))
        print("If that is a surprise, check the rots= field is being written at")
        print("all -- an eight-view sprite fills GZDoom's sixteen rotation slots")
        print("PAIRWISE, so Texture[0] == Texture[1] on a directional prop too.")
        return 1

    print("%-24s %10s %10s %8s %6s %s" % ("prop", "x", "y", "z", "rots", "views seen"))
    for (name, x, y, z), d in sorted(props.items()):
        print("%-24s %10s %10s %8s %6d %d"
              % (name, x, y, z, d["rots"], len(d["views"])))
    print("\n%d directional prop(s)" % len(props))

    # An orbit around the first one, at a radius that keeps it comfortably in
    # frame. ROTH angles are 512 to the turn, so a view is 64 units.
    name, x, y, z = sorted(props)[0]
    cx, cy = float(x), float(y)
    r = 300.0
    print("\n// orbit of %s at (%.0f, %.0f) -- paste into a cfg" % (name, cx, cy))
    for i in range(8):
        # Stand at 45*i degrees around it, looking back at it. ROTH's player
        # angle is counter-clockwise from +Y, so facing = 90 + a*360/512.
        th = math.radians(45.0 * i)
        px, py = cx + r * math.cos(th), cy + r * math.sin(th)
        look = math.degrees(math.atan2(cy - py, cx - px))      # from +X, ccw
        a512 = int(round(((look - 90.0) % 360.0) * 512.0 / 360.0)) % 512
        print('rothdiff_shot %d %d %d <OUT>/dir_%d.png' % (round(px), round(py), a512, i))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
