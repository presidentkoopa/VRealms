#!/usr/bin/env python3
"""rothdiff -- compare what ROTH.C draws against what REMAROTH draws, per pixel.

WHY THIS EXISTS

Every texture argument on this project so far has been settled by looking at two
screenshots, and every one of those settlements has been wrong at least once. A
screenshot cannot tell you WHICH surface is wrong, only that something looks off,
and it cannot be compared by a machine after the fact.

This compares identity buffers instead: for a given camera pose, both engines
record which surface they drew at every pixel and out of which texture. The
output is a match percentage, the mismatches grouped by cause, and the worst
offending surfaces by pixel count. A regression is an exit code, not an opinion.

FORMAT (.ridb, written by both sides)

    magic  "RIDB"   u32     0x42444952
    version 1       u32
    width           u32
    height          u32
    pose x, y, ang  i32 x3
    w*h records of  u16 fill, u16 flags, u16 id, u16 tex

`fill` is ROTH's span fill word (span record +0x16): bit 0x80 floor / else
ceiling, 0x08 textured / else solid fill, 0x06 the mirror bits. `flags` is
g_world_surface_draw_flags, whose 0x20 / 0x200 bits pick the span driver.

SUB-COMMANDS

    diff A.ridb B.ridb [--baseline b.json] [--json out.json]
    probe A.ridb B.ridb X Y       one pixel, both engines, in full
    poses [--map STUDY1]          print the pose set

EXIT CODES
    0  match, or better than the baseline
    1  worse than the baseline (a regression)
    2  the two buffers cannot be compared at all
"""
import argparse
import json
import os
import struct
import sys
from collections import Counter

MAGIC = 0x42444952
HDR = "<7I"          # magic, version, w, h, then pose as 3 signed read separately
HDR_SIZE = 4 * 7
REC = struct.Struct("<4H")


class Buffer:
    __slots__ = ("path", "w", "h", "pose", "px")

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            raw = f.read()
        if len(raw) < HDR_SIZE:
            raise ValueError(f"{path}: too short to be a .ridb")
        magic, ver, w, h = struct.unpack_from("<4I", raw, 0)
        if magic != MAGIC:
            raise ValueError(f"{path}: not a .ridb (magic {magic:#x})")
        if ver != 1:
            raise ValueError(f"{path}: unsupported .ridb version {ver}")
        px_, py_, pang = struct.unpack_from("<3i", raw, 16)
        self.w, self.h, self.pose = w, h, (px_, py_, pang)
        want = HDR_SIZE + w * h * REC.size
        if len(raw) < want:
            raise ValueError(
                f"{path}: truncated -- header says {w}x{h} "
                f"({want} bytes), file is {len(raw)}"
            )
        self.px = memoryview(raw)[HDR_SIZE:want]

    def at(self, i):
        return REC.unpack_from(self.px, i * REC.size)


OUT_OF_SCOPE = 0xFFFF     # REMAROTH marks a wall pixel with this in `flags`


def surface_kind(fill, flags):
    """ROTH's own classification, read from the words rather than invented.

    Deliberately NOT collapsed into friendly names beyond this: the fill word is
    the original's, and keeping the taxonomy thin means a surprise shows up as a
    surprise rather than being silently bucketed.

    A flat is identified by the high nibble of the fill word -- 0x3x ceiling,
    0xbx floor -- because the low bits carry `textured` (0x08) and the two
    mirror bits (0x06), which vary per surface. Anything else drawn through the
    span path is a wall or a sprite, and rothdiff does not score those yet:
    ROTH's wall fill word has not been read, and guessing one would put an
    invented number into the file this whole tool exists to trust.
    """
    if flags == OUT_OF_SCOPE:
        return "wall"
    if fill == 0 and flags == 0:
        return "unwritten"
    if flags & 0x200:
        return "mesh"
    base = fill & 0xF0
    if base not in (0x30, 0xB0):
        return "wall"
    kind = "floor" if base == 0xB0 else "ceiling"
    if not (fill & 0x08):
        kind += "/solid"
    return kind


def in_scope(kind):
    """rothdiff v1 scores FLATS. Everything else is reported, not scored."""
    return kind.startswith("floor") or kind.startswith("ceiling")


def classify(a, b):
    """Why do these two pixels disagree? First matching cause wins, most
    specific first, so 'texture' never absorbs a surface-kind mismatch."""
    af, afl, aid, atex = a
    bf, bfl, bid, btex = b
    ak, bk = surface_kind(af, afl), surface_kind(bf, bfl)
    if ak == "unwritten" and bk != "unwritten":
        return "missing-in-A"
    if bk == "unwritten" and ak != "unwritten":
        return "missing-in-B"
    if ak != bk:
        return f"kind {ak} vs {bk}"
    if atex != btex:
        return "texture"
    if (af ^ bf) & 0x06:
        return "mirror bits"
    if aid != bid:
        return "surface id"
    return "other"


def cmd_diff(args):
    try:
        A, B = Buffer(args.a), Buffer(args.b)
    except ValueError as e:
        print(f"rothdiff: {e}", file=sys.stderr)
        return 2

    if (A.w, A.h) != (B.w, B.h):
        print(
            f"rothdiff: size mismatch {A.w}x{A.h} vs {B.w}x{B.h}. "
            "Capture both engines at the SAME resolution -- comparing "
            "different ones is how a 16% field-of-view difference got read "
            "as a projection defect.",
            file=sys.stderr,
        )
        return 2
    if A.pose != B.pose:
        print(
            f"rothdiff: pose mismatch {A.pose} vs {B.pose}; "
            "the buffers are of different places.",
            file=sys.stderr,
        )
        return 2

    n = A.w * A.h
    scored = same = 0
    skipped = 0
    causes = Counter()
    worst = Counter()          # keyed by (kind, texture, id) on the ROTH side
    for i in range(n):
        pa, pb = A.at(i), B.at(i)
        ka = surface_kind(pa[0], pa[1])
        kb = surface_kind(pb[0], pb[1])

        # Neither engine claims a flat here: out of scope, and NOT counted as a
        # match. Scoring agreement on pixels the tool cannot judge would inflate
        # the percentage with exactly the surfaces it is blind to.
        if not in_scope(ka) and not in_scope(kb):
            skipped += 1
            continue

        scored += 1
        if pa == pb:
            same += 1
            continue
        causes[classify(pa, pb)] += 1
        worst[(ka, pa[3], pa[2])] += 1

    pct = 100.0 * same / scored if scored else 0.0
    out = {
        "pose": list(A.pose),
        "size": [A.w, A.h],
        "scope": "flats",
        "pixels": n,
        "scored": scored,
        "out_of_scope": skipped,
        "match_pct": round(pct, 4),
        "mismatched": scored - same,
        "causes": dict(causes.most_common()),
        "worst_surfaces": [
            {"kind": k, "texture": t, "id": i_, "pixels": c}
            for (k, t, i_), c in worst.most_common(10)
        ],
    }

    baseline_pct = None
    if args.baseline and os.path.exists(args.baseline):
        with open(args.baseline) as f:
            baseline_pct = json.load(f).get("match_pct")
        out["baseline_pct"] = baseline_pct
        out["delta_pct"] = round(pct - baseline_pct, 4)

    text = json.dumps(out, indent=2)
    if args.json:
        with open(args.json, "w") as f:
            f.write(text + "\n")
    print(text)

    if baseline_pct is not None and pct < baseline_pct - 1e-9:
        print(
            f"rothdiff: REGRESSION {baseline_pct:.4f}% -> {pct:.4f}%",
            file=sys.stderr,
        )
        return 1
    return 0


def cmd_probe(args):
    """One pixel, both engines, everything about it. For when the diff says a
    surface is wrong and the next question is 'wrong how'."""
    try:
        A, B = Buffer(args.a), Buffer(args.b)
    except ValueError as e:
        print(f"rothdiff: {e}", file=sys.stderr)
        return 2
    if not (0 <= args.x < A.w and 0 <= args.y < A.h):
        print(f"rothdiff: ({args.x},{args.y}) outside {A.w}x{A.h}", file=sys.stderr)
        return 2
    i = args.y * A.w + args.x
    pa, pb = A.at(i), B.at(i)
    rows = [
        ("fill",  f"{pa[0]:#06x}", f"{pb[0]:#06x}"),
        ("flags", f"{pa[1]:#06x}", f"{pb[1]:#06x}"),
        ("id",    pa[2], pb[2]),
        ("tex",   pa[3], pb[3]),
        ("kind",  surface_kind(pa[0], pa[1]), surface_kind(pb[0], pb[1])),
    ]
    print(f"pixel ({args.x},{args.y}) at pose {A.pose}")
    print(f"{'':8} {'ROTH.C':>18} {'REMAROTH':>18}")
    for name, x, y in rows:
        flag = "" if str(x) == str(y) else "   <-- differs"
        print(f"{name:8} {str(x):>18} {str(y):>18}{flag}")
    if pa != pb:
        print(f"\ncause: {classify(pa, pb)}")
    return 0


# Ten poses in STUDY1. Placed at the landmarks the port keeps getting wrong --
# the carpet and its fringe, the ceiling, a doorway, the staircase -- rather
# than spread evenly, because an even spread mostly samples blank wall.
POSES = {
    "STUDY1": [
        ("study_start",      864, 3840,   0),
        ("study_carpet",     864, 3720,   0),
        ("study_fringe_n",   864, 3600, 128),
        ("study_ceiling_up", 864, 3840,  64),
        ("study_corner_sw",  700, 3600, 192),
        ("study_doorway",    980, 3840,   0),
        ("corridor_in",     1100, 3840,   0),
        ("corridor_along",  1100, 4100, 128),
        ("stairs_foot",     1100, 4400, 128),
        ("stairs_look_up",  1100, 4400,  96),
    ],
    # LRINTH1 -- chosen by tools/rothdiff/findspots, which asks the map which
    # sectors carry each property under test rather than picking spots by eye.
    # This map is used for the flats pass because it is one of only four that
    # carry EVERY property in one place; STUDY1 has no mirrored floor at all,
    # so it cannot test the mirror bits, which are the rule the docs mark
    # [derived] and never confirmed.
    "LRINTH1": [
        ("flat_plain",   -4180, -3506,   0),   # 1 baseline, s=1, no shift/mirror
        ("flat_shifted", -6656,  -256,   0),   # 2 the shift signs
        ("flat_mirx",    -1536,  -384,   0),   # 3a mirrored in X
        ("flat_miry",    -4032,  2240,   0),   # 3b mirrored in Y
        ("ceil_plain",   -7282,  -264,   0),   # 4 the ceiling signs
        ("flat_256",     -4096, -1600,   0),   # 5 the 256x256 opaque exception
        ("flat_seam",    -3808,  1069, 256),   # 6 same texture, different shift
        ("midplat_top",  -4096, -1408,   0),   # 7 the mid-platform path
    ],
}



def cmd_poses(args):
    for name, x, y, a in POSES.get(args.map, []):
        print(f"{name},{x},{y},{a}")
    return 0


def main():
    ap = argparse.ArgumentParser(prog="rothdiff", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("diff", help="compare two identity buffers")
    d.add_argument("a", help="ROTH.C buffer")
    d.add_argument("b", help="REMAROTH buffer")
    d.add_argument("--baseline", help="JSON from a previous run; exit 1 if worse")
    d.add_argument("--json", help="write the report here as well as to stdout")
    d.set_defaults(func=cmd_diff)

    p = sub.add_parser("probe", help="one pixel, both engines")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("x", type=int)
    p.add_argument("y", type=int)
    p.set_defaults(func=cmd_probe)

    q = sub.add_parser("poses", help="print the pose set")
    q.add_argument("--map", default="STUDY1")
    q.set_defaults(func=cmd_poses)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
