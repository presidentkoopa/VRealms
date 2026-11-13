#!/usr/bin/env python3
"""measure_ref.py -- UZDXREMA's measure_avatar.py, ITS OWN LINES, run on the DXR hands' rest joints, against the
engine's port (vr_handpose_math.h MeasurePalm / MeasureFingerFrames).

    python3 -I measure_ref.py <rest.json from test_fingers> <UZDXREMA checkout>

measure_avatar.py measures a whole body from an .mdl or .iqm and refuses anything without arms, legs and a head, so it
cannot be run on hand_final.fbx as a program. Its hand-frame block and its finger-frame block (lines 362-556: the
FRIK hand frame and palm normal per side, then the canonical bone frame of every finger joint) are cut out of the file
UNEDITED, dedented, and executed here with the names they read supplied: `model.joints` (parent, t, r, s from the
engine's own FBX loader, rest pose), `G` from measure_avatar's own global_transforms(), `pos`/`rot`/`jid` as main()
defines them (:312-319), and the roles mkfbxposes.py uses for this file (hand = Root_joint, finger 1/2/3 =
BASE/MID/TOP). The right hand (Hand_Low) is side R, the left (Hand_Low1) side L, as one skeleton of 44 joints.
The cut is pinned by the text of its first and last lines.
"""
import json
import math
import os
import sys
import textwrap


def main():
    rest = json.load(open(sys.argv[1]))
    uz = sys.argv[2]
    tools = os.path.join(uz, "tools", "avatar")
    src = os.path.join(tools, "measure_avatar.py")
    lines = open(src, encoding="utf-8").read().split("\n")
    first, last = lines[361].strip(), lines[555].strip()
    if not first.startswith("# ---- the FRIK hand frame at rest, per side") or not last.startswith("finger_frames[side] = per_finger"):
        sys.exit("FAIL: measure_avatar.py:362/556 read %r / %r -- the source moved; re-pin the cut" % (first, last))
    block = textwrap.dedent("\n".join(lines[361:556]))

    sys.path.insert(0, tools)
    import measure_avatar as MA  # noqa: E402  (the source, read-only)

    class J:
        pass

    class Model:
        pass

    model = Model()
    model.joints = []
    names = {}
    for side, mesh in (("R", "Hand_Low"), ("L", "Hand_Low1")):
        base = len(model.joints)
        for i, j in enumerate(rest[mesh]["joints"]):
            o = J()
            o.parent = j["parent"] + base if j["parent"] >= 0 else -1
            o.t, o.r, o.s = tuple(j["t"]), tuple(j["r"]), tuple(j["s"])
            o.index = base + i
            model.joints.append(o)
            names[(side, j["name"])] = base + i
    roles = {}
    fb = {"thumb": "THUMB", "index": "INDEX", "middle": "MIDDLE_F", "ring": "RING", "pinky": "PINK"}
    seg = {1: "BASE", 2: "MID", 3: "TOP"}
    for side in ("R", "L"):
        roles["hand_%s" % side] = names[(side, "Root_joint")]
        for f, b in fb.items():
            for k in (1, 2, 3):
                roles["%s_%d_%s" % (f, k, side)] = names[(side, "%s_%s_joint" % (b, seg[k]))]

    G = MA.global_transforms(model)
    ns = dict(vars(MA))
    ns.update(model=model, G=G, tier="Full",
              jid=lambda role: roles[role],
              pos=lambda role: G[roles[role]][1],
              rot=lambda role: G[roles[role]][0])
    del MA.ERRORS[:]
    try:
        exec(compile(block, "measure_avatar.py:362-556", "exec"), ns)
    except SystemExit as e:
        sys.exit("FAIL: measure_avatar.py's own lines refused the hands: %s" % e)
    if MA.ERRORS:
        sys.exit("FAIL: measure_avatar.py's own lines reported: %s" % "; ".join(MA.ERRORS))

    worst_f = worst_p = 0.0
    bad = 0
    for side, mesh in (("R", "Hand_Low"), ("L", "Hand_Low1")):
        ours = rest[mesh]["ours"]
        palm = ns["hand_frames"][side]["palm"]
        dp = max(abs(a - b) for a, b in zip(palm, ours["palm"]))
        worst_p = max(worst_p, dp)
        for fr in ours["frames"]:
            ref = [j for j in ns["finger_frames"][side][fr["finger"]] if j["role"].startswith("%s_%d_" % (fr["finger"], fr["joint"]))][0]
            d = max(max(abs(a - b) for a, b in zip(ref[k], fr[k])) for k in ("X", "Y", "Z"))
            if ref["borrowed"] != fr["borrowed"]:
                bad += 1
            worst_f = max(worst_f, d)
        print("measure_avatar.py :362-556 on %s (side %s): palm normal %s, ours %s" % (
            mesh, side, " ".join("%+.6f" % v for v in palm), " ".join("%+.6f" % v for v in ours["palm"])))
    print("measure_avatar.py's own lines vs the engine's port, both hands, 30 finger joints: palm normal max diff %.3g, "
          "frame axes max diff %.3g, tipaxis disagreements %d" % (worst_p, worst_f, bad))
    if worst_p > 1e-9 or worst_f > 1e-9 or bad:
        print("FAIL: the port's canonical frames differ from measure_avatar.py's")
        return 1
    print("PASS: identical to measure_avatar.py (< 1e-9)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
