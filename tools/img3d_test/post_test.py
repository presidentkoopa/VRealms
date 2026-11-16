"""post_test.py -- tools/img3d/post.py on procedural boxes only (make_placeholder.py's), no retail art and no model.

    python3 -I tools/img3d_test/post_test.py <boxes folder> <scratch folder outside the repo>

The Book box is fed to post.py as a generator's raw.glb would come: ten times too big, standing off the origin,
wound INWARD (every face reversed), through a node. post.py must give back: the table's 0.20 m, the base on y = 0,
centred in x and z, faces wound outward (positive signed volume), normals pointing out of the box, the front's
picture still on the +Z face, the grip where items.json says, and a manifest the engine's reader takes (the C++ test
reads the same folder afterwards).
"""
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "img3d"))
import glbio  # noqa: E402
import post  # noqa: E402

import numpy as np  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails += 1


def main():
    boxes, scratch = sys.argv[1], sys.argv[2]
    work, out = os.path.join(scratch, "work"), os.path.join(scratch, "out")
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(os.path.join(work, "17"))
    os.makedirs(os.path.join(work, "16"))
    g = glbio.read_glb(os.path.join(boxes, "17_placeholder.glb"))
    pos = g["positions"] * 10.0 + np.array([3.0, 2.0, -1.0])
    inward = g["indices"][:, ::-1]
    glbio.write_glb(os.path.join(work, "17", "raw.glb"), pos.reshape(-1).tolist(), None, g["uvs"].reshape(-1).tolist(),
                    inward.reshape(-1).tolist(), g["image"], nodes_trs=((0.5, 0.0, 0.0), (0, 0, 0, 1), (1, 1, 1)))
    shutil.copyfile(os.path.join(boxes, "16_placeholder.glb"), os.path.join(work, "16", "raw.glb"))
    done = post.run([17, 16], work, out)
    check(done == [17, 16], "post.py made both items")
    with open(os.path.join(out, "manifest.json")) as f:
        man = json.load(f)
    e = {i["item"]: i for i in man["items"]}
    check(man["version"] == 1 and set(e) == {16, 17}, "the manifest lists 16 and 17")
    r = glbio.read_glb(os.path.join(out, e[17]["file"]))
    v, f, n = r["positions"], r["indices"], r["normals"]
    lo, hi = v.min(0), v.max(0)
    check(abs((hi - lo).max() - 0.20) < 1e-6 and e[17]["size"] == "table", "the Book is its table size, 0.20 m")
    check(abs(lo[1]) < 1e-9 and abs(lo[0] + hi[0]) < 1e-9 and abs(lo[2] + hi[2]) < 1e-9, "base on y = 0, centred in x and z")
    vol = np.einsum("ij,ij->i", v[f[:, 0]], np.cross(v[f[:, 1]], v[f[:, 2]])).sum() / 6.0
    check(vol > 0, "the inward faces were turned outward (signed volume %.6f m^3)" % vol)
    centre = (lo + hi) / 2
    out_ok = all(np.dot(n[i], v[i] - centre) > 0 for i in range(len(v)))
    check(out_ok, "every vertex normal points out of the box")
    front = [i for i in range(len(v)) if v[i][2] > hi[2] - 1e-6]
    check(len(front) >= 4 and all(r["uvs"][i][0] >= 0.5 - 1e-6 for i in front if n[i][2] > 0.9), "the front's green half is still on +Z")
    check(np.allclose(e[17]["grip"], [0.0, -0.025, 0.0], atol=1e-6) and e[17]["hold"] == "palm", "the grip from items.json (base centre, 2.5 cm out of the palm)")
    check(r["image"] is not None and r["image"][:8] == b"\x89PNG\r\n\x1a\n", "the picture is an embedded PNG")
    print("post_test: %s" % ("all passed" if fails == 0 else "%d FAILED" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
