"""Read a rothdiff identity buffer and say WHAT is at a region of the picture.

Every pixel carries the fill word, the draw flags, the surface id and the
texture the engine used, so "is this the same surface in both engines" and
"which face is that wall" stop being questions answered by looking at a picture.

    python ridb.py <file.ridb> [x0 y0 x1 y1] ...

With no region it summarises the whole frame. Regions may be repeated; each is
reported separately, most common surface first.

Layout (roth_diff.cpp, RIDB_MAGIC/RIDB_VERSION; must match idbuffer.inc.c):
    u32 magic 'RIDB', u32 version, u32 w, u32 h, i32 x, i32 y, i32 angle
    then w*h of { u16 fill, u16 flags, u16 id, u16 tex }
"""

import struct
import sys
import os
from collections import Counter

MAGIC = 0x42444952


def load(path):
    with open(path, "rb") as f:
        blob = f.read()
    magic, ver, w, h = struct.unpack_from("<4I", blob, 0)
    if magic != MAGIC:
        sys.exit("%s: not a RIDB (magic %08x)" % (path, magic))
    px, py, pang = struct.unpack_from("<3i", blob, 16)
    need = 28 + w * h * 8
    if len(blob) < need:
        sys.exit("%s: short file, want %d bytes, have %d" % (path, need, len(blob)))
    return blob, ver, w, h, (px, py, pang)


def region(blob, w, h, box):
    x0, y0, x1, y1 = box
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(w, x1), min(h, y1)
    c = Counter()
    for y in range(y0, y1):
        base = 28 + (y * w) * 8
        for x in range(x0, x1):
            fill, flags, sid, tex = struct.unpack_from("<4H", blob, base + x * 8)
            c[(fill, flags, sid, tex)] += 1
    return c, (x1 - x0) * (y1 - y0)


def report(name, c, total):
    print("  %-26s %d px" % (name, total))
    if total == 0:
        return
    for (fill, flags, sid, tex), n in c.most_common(8):
        print("      fill 0x%04x flags 0x%04x  id %-6d tex %-6d  %7d px  %5.1f%%"
              % (fill, flags, sid, tex, n, 100.0 * n / total))


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    path = argv[1]
    blob, ver, w, h, pose = load(path)
    print("%s  %dx%d  v%d  pose (%d, %d, %d)"
          % (os.path.basename(path), w, h, ver, pose[0], pose[1], pose[2]))

    nums = [int(v) for v in argv[2:]]
    if not nums:
        c, t = region(blob, w, h, (0, 0, w, h))
        report("whole frame", c, t)
        return 0
    if len(nums) % 4 != 0:
        sys.exit("regions come in fours: x0 y0 x1 y1")
    for i in range(0, len(nums), 4):
        box = tuple(nums[i:i + 4])
        c, t = region(blob, w, h, box)
        report("x %d-%d y %d-%d" % (box[0], box[2], box[1], box[3]), c, t)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
