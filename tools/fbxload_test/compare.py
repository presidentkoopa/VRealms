#!/usr/bin/env python3
"""Diff the ANGLES lines of test_fbxload --angles (the engine's loader, via ufbx) against poses_ref.py
(tools/fbx_hands/poses.py's own reader). Joint angles must agree; Root_joint positions are reported, and
only frames keyed on the controls are held to it (poses.py interpolates the controls' sparse cubic keys
linearly between keys; ufbx evaluates the cubic).

    python3 -I compare.py ours.txt ref.txt
"""
import sys


def load(p):
    out = {}
    for line in open(p):
        if not line.startswith("ANGLES "):
            continue
        parts = line.split()
        key = (parts[1], parts[2])
        d = {}
        for kv in parts[3:]:
            k, v = kv.split("=")
            d[k] = tuple(float(x) for x in v.split(",")) if k == "rootY" else float(v)
        out[key] = d
    return out


# frames keyed on the controls (poses.py --keys: 0, 845 and 1278 are among them); between keys poses.py's linear
# reading of the cubic curves differs from ufbx's, so Root_joint is only held to these
KEYED = ("f0", "f845", "f1278")
ours, ref = load(sys.argv[1]), load(sys.argv[2])
worst_ang, worst_root, worst_turn, fails = 0.0, 0.0, 0.0, 0
for key in sorted(ref):
    if key not in ours:
        print("missing in ours:", key)
        fails += 1
        continue
    a, b = ours[key], ref[key]
    da = max(abs(a[k] - b[k]) for k in b if k not in ("rootY", "Root_joint"))
    droot = abs(a["Root_joint"] - b["Root_joint"])
    dr = max(abs(x - y) for x, y in zip(a["rootY"], b["rootY"]))
    worst_ang = max(worst_ang, da)
    print("%-10s %-6s 21 joints' angles max diff %.4f deg; Root_joint turn %.3f vs %.3f deg, at %s vs %s (max diff %.5f)" % (
        key[0], key[1], da, a["Root_joint"], b["Root_joint"], ",".join("%.4f" % x for x in a["rootY"]), ",".join("%.4f" % x for x in b["rootY"]), dr))
    if key[1] in KEYED:
        worst_root = max(worst_root, dr)
        worst_turn = max(worst_turn, droot)
ok = worst_ang < 0.01 and worst_root < 1e-3 and worst_turn < 0.01 and not fails
print("%s: the 21 joints below Root_joint, angles max diff %.4f deg over %d hand-frames; Root_joint at the control-keyed frames %s:"
      " place max diff %.5f file units, turn max diff %.4f deg"
      % ("PASS" if ok else "FAIL", worst_ang, len(ref), " ".join(KEYED), worst_root, worst_turn))
sys.exit(0 if ok else 1)
