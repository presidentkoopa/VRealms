#!/usr/bin/env python3
"""The reference side of tools/fbxload_test: tools/fbx_hands/poses.py's own FBX reader and sampler (no ufbx)
prints, for the given frames, every joint's rotation from its rest (degrees; Root_joint: its world rotation
from frame 0, the controls included) and Root_joint's world position,
in the line format test_fbxload --angles prints, so compare.py can diff the two.

    python3 -I poses_ref.py <hand_final.fbx> f1,f2,...

Hand_Low is GLOBAL_MAIN_CONTROL_R's rig and Hand_Low1 GLOBAL_MAIN_CONTROL_R1's (docs/FBX_HANDS_REPORT.md fact 21;
test_fbxload T1 checks the same from ufbx).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "fbx_hands"))
from analyze import Scene  # noqa: E402
from poses import Hand, root_world, rot_angle  # noqa: E402

MESH = {"GLOBAL_MAIN_CONTROL_R": "Hand_Low", "GLOBAL_MAIN_CONTROL_R1": "Hand_Low1"}


def main():
    sc = Scene(sys.argv[1])
    frames = [int(x) for x in sys.argv[2].split(",")]
    for oid, n in sc.objs.items():
        if n.name == "Model" and sc.model_parent(oid) == 0 and sc.name(oid).startswith("GLOBAL_MAIN_CONTROL"):
            h = Hand(sc, sc.name(oid), oid)
            _, m0 = root_world(sc, h, 0)
            for f in frames:
                _, ang = h.pose(f)
                rp, m = root_world(sc, h, f)
                # Root_joint in the engine model carries the controls above it (its local is its world), so its
                # angle here is the world rotation from frame 0 (= the scene's rest, test_fbxload's reference)
                r = [[sum(m0[k][i] * m[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
                ang["Root_joint"] = rot_angle(r)
                print("ANGLES %s f%d %s rootY=%.5f,%.5f,%.5f" % (
                    MESH[h.name], f, " ".join("%s=%.3f" % (k, ang[k]) for k in sorted(ang)), *rp))


if __name__ == "__main__":
    main()
