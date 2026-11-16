"""make_placeholder.py -- PROCEDURAL test boxes (no retail art) and their manifest, to prove the engine's load and
draw path (src/roth/roth_img3d.cpp) before any real model exists. Standard library only: runs here and on the PC.

    python -I tools/img3d/make_placeholder.py <out folder> [--trs]

Writes <out>/manifest.json and one box per item:
    16 Matchbox  5.5 x 1.6 x 3.6 cm  hold palm   orange / brown checks, FRONT (+Z) face solid green
    21 Key       6.0 x 0.6 x 2.0 cm  hold fist   yellow / grey checks,  front solid green
    17 Book      20 x 4 x 13 cm      hold palm   blue / white checks,   front solid green
Each box stands on y = 0, centred in x and z, and is drawn with a 64 x 64 texture: the left half checks, the right half
the front's green, so a capture shows which way the item faces. --trs writes the 17 box through a node transform
(the reader's node path; tools/img3d_test checks it lands where the plain one does).
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glbio  # noqa: E402

BOXES = [
    # item, name, file, (x, y, z) metres, hold, grip, colour a, colour b
    (16, "Matchbox (placeholder box)", "16_placeholder.glb", (0.055, 0.016, 0.036), "palm", [0.0, -0.025, 0.0], (230, 120, 30), (90, 50, 20)),
    (21, "Key (placeholder box)", "21_placeholder.glb", (0.060, 0.006, 0.020), "fist", [0.0, 0.003, 0.0], (230, 210, 40), (120, 120, 120)),
    (17, "Book (placeholder box)", "17_placeholder.glb", (0.200, 0.040, 0.130), "palm", [0.0, -0.025, 0.0], (40, 80, 230), (240, 240, 240)),
]
FRONT = (40, 200, 60)


def texture(a, b):
    w = h = 64
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            c = FRONT if x >= 32 else (a if ((x // 8) + (y // 8)) % 2 == 0 else b)
            i = (y * w + x) * 4
            px[i:i + 4] = bytes((c[0], c[1], c[2], 255))
    return glbio.png_rgba(w, h, bytes(px))


def box(sx, sy, sz):
    """24 vertices (4 a face), outward normals, counter-clockwise from outside; base on y = 0."""
    hx, hz = sx / 2.0, sz / 2.0
    faces = [  # normal, four corners (counter-clockwise seen from outside), uv half (0 left, 1 right)
        ((0, 0, 1), [(-hx, 0, hz), (hx, 0, hz), (hx, sy, hz), (-hx, sy, hz)], 1),       # front +Z
        ((0, 0, -1), [(hx, 0, -hz), (-hx, 0, -hz), (-hx, sy, -hz), (hx, sy, -hz)], 0),  # back
        ((1, 0, 0), [(hx, 0, hz), (hx, 0, -hz), (hx, sy, -hz), (hx, sy, hz)], 0),       # +X (the item's left)
        ((-1, 0, 0), [(-hx, 0, -hz), (-hx, 0, hz), (-hx, sy, hz), (-hx, sy, -hz)], 0),  # -X
        ((0, 1, 0), [(-hx, sy, hz), (hx, sy, hz), (hx, sy, -hz), (-hx, sy, -hz)], 0),   # top
        ((0, -1, 0), [(-hx, 0, -hz), (hx, 0, -hz), (hx, 0, hz), (-hx, 0, hz)], 0),      # bottom
    ]
    pos, nrm, uv, idx = [], [], [], []
    for n, corners, half in faces:
        base = len(pos) // 3
        u0 = 0.5 * half
        for k, c in enumerate(corners):
            pos += list(c)
            nrm += list(n)
            uv += [u0 + (0.5 if k in (1, 2) else 0.0), 1.0 if k in (0, 1) else 0.0]
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nrm, uv, idx


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    trs = "--trs" in sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    items = []
    for item, name, fn, size, hold, grip, a, b in BOXES:
        pos, nrm, uv, idx = box(*size)
        node = None
        if trs and item == 17:
            # The same box through a node: the vertices turned -90 degrees about Y and moved, the node turning them
            # back (+90 about Y) and moving them home. The reader must land every vertex where the plain box has it.
            q = (0.0, math.sin(math.pi / 4), 0.0, math.cos(math.pi / 4))
            moved = []
            for i in range(0, len(pos), 3):
                x, y, z = pos[i] - 0.0, pos[i + 1] - 0.5, pos[i + 2] - 0.0
                moved += [-z, y, x]   # -90 about Y: (x, z) -> (-z, x)
            pos = moved
            nn = []
            for i in range(0, len(nrm), 3):
                nn += [-nrm[i + 2], nrm[i + 1], nrm[i]]
            nrm = nn
            node = ((0.0, 0.5, 0.0), q, (1.0, 1.0, 1.0))
        glbio.write_glb(os.path.join(out, fn), pos, nrm, uv, idx, texture(a, b), name=name, nodes_trs=node)
        items.append({"item": item, "name": name, "file": fn, "metres": max(size), "size": "table",
                      "grip": grip, "grip_rot": [0, 0, 0], "hold": hold, "world_yaw": 0})
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump({"version": 1, "generator": "tools/img3d/make_placeholder.py (procedural boxes, no retail art)",
                   "items": items}, f, indent=2)
    print("placeholder boxes for items %s written to %s" % (", ".join(str(i["item"]) for i in items), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
