#!/usr/bin/env python3
"""Sample the baked joint curves of hand_final.fbx frame by frame and describe the hand poses.

  python3 -I poses.py <file.fbx> [--timeline] [--holds] [--frames f1,f2,...] [--keys]

All joint metrics are measured in the hand's own Root_joint frame, so the root/arm motion
(GLOBAL_MAIN_CONTROL_*, GLOBAL_cntrl) is removed from the finger numbers and reported separately.

Metrics per finger (I M R P T):
  ext  = |tip - knuckle| now / |tip - knuckle| at rest   (1.0 = as straight as the bind pose, small = curled)
  bend = sum of the joint rotation angles (deg) of BASE, MID, TOP relative to the rest pose (R = 0)
Thumb extra:
  t2i  = distance thumb tip -> index MID joint, in hand lengths (small = thumb on the index side, pinch/trigger)
  t2p  = distance thumb tip -> palm centre (mean of the four BASE joints), in hand lengths
  base = rotation angle (deg) of the BASE (knuckle) joint alone
  reach = |tip - Root_joint| now / at rest  (includes the knuckle bend that ext does not see)
  IM.spread = index tip to middle tip distance / rest (V-sign spread > 1, fingers together ~1)
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import Scene, anim_index, curve, model_props, local_matrix, mmul  # noqa: E402

FINGERS = [("I", "INDEX_BASE_joint", "INDEX_MID_joint", "INDEX_TOP_joint", "INDEX_UP_TOP_joint"),
           ("M", "MIDDLE_F_BASE_joint", "MIDDLE_F_MID_joint", "MIDDLE_F_TOP_joint", "MIDDLE_F_UP_TOP_joint"),
           ("R", "RING_BASE_joint", "RING_MID_joint", "RING_TOP_joint", "RING_UP_TOP_joint"),
           ("P", "PINK_BASE_joint", "PINK_MID_joint", "PINK_TOP_joint", "PINK_UP_TOP_joint"),
           ("T", "THUMB_BASE_joint", "THUMB_MID_joint", "THUMB_TOP_joint", "THUMB_UP_TOP_joint")]
FRAME = 769769300  # 1/60 s in FBX ticks


def rot_angle(m):
    tr = m[0][0] + m[1][1] + m[2][2]
    return math.degrees(math.acos(max(-1.0, min(1.0, (tr - 1) / 2))))


def dist(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


class Hand:
    def __init__(self, sc, top_name, top_id):
        self.sc = sc
        self.top = top_id
        self.name = top_name
        # collect LimbNodes under this top control
        self.joints = {}
        stack = [top_id]
        while stack:
            x = stack.pop()
            for cid, _ in sc.kids(x, "Model"):
                if sc.model_parent(cid) != x:
                    continue
                if sc.cls(cid)[1] == "LimbNode":
                    self.joints[sc.name(cid)] = cid
                stack.append(cid)
        self.root = self.joints["Root_joint"]
        self.curves = {}
        for sid, lid, cn, m, pr, cv in anim_index(sc):
            if pr in ("Lcl Rotation", "Lcl Translation"):
                self.curves[(m, pr)] = {ax[-1]: curve(sc, c) for ax, c in cv.items()}
        self.mp = {j: model_props(sc, j) for j in self.joints.values()}

    def sample(self, m, pr, frame):
        cv = self.curves.get((m, pr))
        if not cv:
            return None
        out = []
        t = frame * FRAME
        for ax in "XYZ":
            kt, kv, d = cv[ax]
            # baked curves: one key per frame; take the key at or before t
            lo, hi = 0, len(kt) - 1
            if t <= kt[0]:
                out.append(kv[0]); continue
            if t >= kt[-1]:
                out.append(kv[-1]); continue
            while hi - lo > 1:
                mid = (lo + hi) // 2
                if kt[mid] <= t:
                    lo = mid
                else:
                    hi = mid
            a = (t - kt[lo]) / (kt[hi] - kt[lo])
            out.append(kv[lo] + (kv[hi] - kv[lo]) * a)  # linear between keys (only matters for non-baked curves)
        return tuple(out)

    def pose(self, frame):
        """{joint name: matrix in Root_joint space} and {joint name: local rotation angle vs rest}."""
        loc = {}
        ang = {}
        for n, j in self.joints.items():
            R = self.sample(j, "Lcl Rotation", frame) if frame is not None else None
            T = self.sample(j, "Lcl Translation", frame) if frame is not None else None
            mp = self.mp[j]
            loc[n] = local_matrix(mp, R, T)
            rest = local_matrix(mp)
            # rotation relative to the rest local transform
            rel = mmul([[rest[c][r] for c in range(3)] + [0] for r in range(3)] + [[0, 0, 0, 1]], loc[n])
            ang[n] = rot_angle(rel)
        world = {}

        def w(n):
            if n in world:
                return world[n]
            j = self.joints[n]
            par = self.sc.model_parent(j)
            pn = self.sc.name(par)
            if n == "Root_joint":
                world[n] = [[1.0 if i == k else 0.0 for k in range(4)] for i in range(4)]
            else:
                world[n] = mmul(w(pn), loc[n])
            return world[n]

        for n in self.joints:
            w(n)
        pos = {n: (m[0][3], m[1][3], m[2][3]) for n, m in world.items()}
        return pos, ang

    def metrics(self, frame, rest=None):
        pos, ang = self.pose(frame)
        rest = rest or self.pose(None)[0]
        hl = dist(rest["Root_joint"], rest["MIDDLE_F_UP_TOP_joint"])
        out = {}
        for f, b, m, t, tip in FINGERS:
            out[f + ".ext"] = dist(pos[tip], pos[b]) / dist(rest[tip], rest[b])
            out[f + ".bend"] = ang[b] + ang[m] + ang[t]
            out[f + ".base"] = ang[b]
            out[f + ".reach"] = dist(pos[tip], pos["Root_joint"]) / dist(rest[tip], rest["Root_joint"])
        out["IM.spread"] = dist(pos["INDEX_UP_TOP_joint"], pos["MIDDLE_F_UP_TOP_joint"]) / dist(rest["INDEX_UP_TOP_joint"], rest["MIDDLE_F_UP_TOP_joint"])
        palm = tuple(sum(pos[n][i] for n in ("INDEX_BASE_joint", "MIDDLE_F_BASE_joint", "RING_BASE_joint", "PINK_BASE_joint")) / 4 for i in range(3))
        out["T.t2i"] = dist(pos["THUMB_UP_TOP_joint"], pos["INDEX_MID_joint"]) / hl
        out["T.t2p"] = dist(pos["THUMB_UP_TOP_joint"], palm) / hl
        out["wrist"] = ang["HANDPALM_joint"]
        return out, pos


def root_world(sc, hand, frame):
    """World position of Root_joint including the animated controls above it (for root-motion size)."""
    chain = []
    x = hand.root
    while x:
        chain.append(x)
        x = sc.model_parent(x)
    m = [[1.0 if i == k else 0.0 for k in range(4)] for i in range(4)]
    for j in reversed(chain):
        mp = model_props(sc, j)
        R = hand.sample(j, "Lcl Rotation", frame)
        T = hand.sample(j, "Lcl Translation", frame)
        m = mmul(m, local_matrix(mp, R, T))
    return (m[0][3], m[1][3], m[2][3]), m


def main():
    path = sys.argv[1]
    args = sys.argv[2:]
    sc = Scene(path)
    hands = []
    for oid, n in sc.objs.items():
        if n.name == "Model" and sc.model_parent(oid) == 0 and sc.name(oid).startswith("GLOBAL_MAIN_CONTROL"):
            hands.append(Hand(sc, sc.name(oid), oid))
    nframes = 1278
    rests = {h.name: h.pose(None)[0] for h in hands}
    for h in hands:
        r = rests[h.name]
        print("hand %s: %d joints; rest hand length Root->middle tip %.4f file units" % (
            h.name, len(h.joints), dist(r["Root_joint"], r["MIDDLE_F_UP_TOP_joint"])))

    if "--palm" in args:
        # which side of the hand do the fingertips curl towards?  compare tip motion with the palm frame
        for h in hands:
            r = rests[h.name]
            # rest frame vectors in Root_joint space
            up = [r["MIDDLE_F_BASE_joint"][i] - r["Root_joint"][i] for i in range(3)]
            side = [r["INDEX_BASE_joint"][i] - r["PINK_BASE_joint"][i] for i in range(3)]
            # find the frame with the smallest mean extension (most curled)
            best = min(range(nframes), key=lambda f: sum(h.metrics(f, r)[0][k + ".ext"] for k in "IMRP"))
            m, pos = h.metrics(best, r)
            d = [pos["MIDDLE_F_UP_TOP_joint"][i] - r["MIDDLE_F_UP_TOP_joint"][i] for i in range(3)]
            n = [up[1] * side[2] - up[2] * side[1], up[2] * side[0] - up[0] * side[2], up[0] * side[1] - up[1] * side[0]]
            print("%s: most-curled frame %d; up(root->middle base)=%s index-minus-pinky=%s; up x side=%s; middle tip moved %s; dot(move, up x side)=%.3f" % (
                h.name, best, [round(x, 3) for x in up], [round(x, 3) for x in side], [round(x, 3) for x in n], [round(x, 3) for x in d],
                sum(d[i] * n[i] for i in range(3))))

    if "--timeline" in args or "--holds" in args:
        rows = []
        prev = {}
        for f in range(nframes):
            row = {"f": f}
            for h in hands:
                m, pos = h.metrics(f, rests[h.name])
                rp, _ = root_world(sc, h, f)
                vel = 0.0
                if h.name in prev:
                    pm, pp = prev[h.name]
                    vel = max(abs(m[k] - pm[k]) for k in m if k.endswith(".bend"))
                prev[h.name] = (m, rp)
                row[h.name] = (m, rp, vel)
            rows.append(row)
        if "--timeline" in args:
            step = 10
            for f in range(0, nframes, step):
                line = "%4d %6.2fs" % (f, f / 60)
                for h in hands:
                    m, rp, vel = rows[f][h.name]
                    line += " | %s ext I%.2f M%.2f R%.2f P%.2f T%.2f t2i%.2f wr%3.0f root(%5.2f,%5.2f,%5.2f)" % (
                        h.name[-2:], m["I.ext"], m["M.ext"], m["R.ext"], m["P.ext"], m["T.ext"], m["T.t2i"], m["wrist"], *rp)
                print(line)
        if "--holds" in args:
            thr = 0.25  # deg/frame on every finger's summed bend
            for h in hands:
                print("HOLDS for %s (all finger bends change < %.2f deg/frame, >= 6 frames)" % (h.name, thr))
                start = None
                for f in range(1, nframes + 1):
                    still = f < nframes and rows[f][h.name][2] < thr
                    if still and start is None:
                        start = f - 1
                    if not still and start is not None:
                        if f - start >= 6:
                            mid = (start + f - 1) // 2
                            m, rp, _ = rows[mid][h.name]
                            r0 = rows[start][h.name][1]
                            r1 = rows[f - 1][h.name][1]
                            print("  frames %4d-%4d (%5.2f-%5.2fs) ext I%.2f M%.2f R%.2f P%.2f T%.2f | bend I%3.0f M%3.0f R%3.0f P%3.0f T%3.0f | t2i %.2f t2p %.2f wrist %3.0f | root moved %.3f" % (
                                start, f - 1, start / 60, (f - 1) / 60, m["I.ext"], m["M.ext"], m["R.ext"], m["P.ext"], m["T.ext"],
                                m["I.bend"], m["M.bend"], m["R.bend"], m["P.bend"], m["T.bend"], m["T.t2i"], m["T.t2p"], m["wrist"], dist(r0, r1)))
                        start = None

    if "--frames" in args:
        fr = [int(x) for x in args[args.index("--frames") + 1].split(",")]
        for h in hands:
            for f in fr:
                m, pos = h.metrics(f, rests[h.name])
                print("%s f%d %s" % (h.name, f, " ".join("%s=%.2f" % (k, v) for k, v in m.items())))

    if "--keys" in args:
        keys = set()
        for (m, pr), cv in hands[0].curves.items():
            if sc.cls(m)[1] == "NurbsCurve":
                for ax in cv.values():
                    keys.update(int(round(t / FRAME)) for t in ax[0])
        print("authored key frames on the rig controls (union over all controls, both hands): %d" % len(keys))
        print(sorted(keys))


if __name__ == "__main__":
    main()
