#!/usr/bin/env python3
"""The VR hands on the converter's real IQMs, outside the engine.

    python3 -I tools/hands_test/test_iqm.py [--models DIR] [--exe test_handpose] [--out DIR] [--upm 107.4]

1. FINGERS. Writes the finger joints' rotations for the five target frames (0 open, 1 point, 3 fist,
   7 grip_tu, 8 ready_td) and the wrist/knuckle bind positions of each hand, runs the C++ test
   (tools/hands_test/test_handpose.cpp, which compiles the renderer's own vr_handpose_math.h) on them,
   then rebuilds each converged hand as the engine does -- frame 0's locals, the finger joints'
   rotations replaced (vr_handpose.cpp VRHandPose_Apply) -- skins it, and compares every vertex with
   the baked frame that input should reproduce (open->0, fist/grab->6 gripfire, point->1,
   thumbup->10 fire_tu, fingergun->7 grip_tu, trigger->2, ready->8). Also: an analog sweep is
   monotone, and the step response matches 1 - 0.75^n.

2. PLACEMENT. Replays the engine's matrix chain for the flat test's grip (vr_handpose.cpp TestGrip ->
   DefaultHandFit -> MODELDEF Scale -> ModelSeat, models.cpp ObjectToWorldMatrix order) with the seat
   the C++ test computed, and checks in visual map space (pixel stretch undone): the hand is 0.19 m at
   the given units per metre, the grip socket lands on the grip, the fingers point up, the palm faces
   the camera, each thumb is on its own outer side, and nothing is mirrored (det > 0). Then a natural
   controller hold (OpenXR grip -Z up-forward 45 degrees) for the orientation a headset would show.

3. PICTURES (reasoning aids, not evidence of the engine): each test pose drawn from the camera, flat
   shaded, written to --out. The engine's own capture is tools/claude_bridge/cfg/hands_flat.cfg.
"""
import argparse
import math
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from iqm import IQM, quat_to_mat  # noqa: E402

FINGERS = [("THUMB", 0), ("INDEX", 1), ("MIDDLE_F", 2), ("RING", 3), ("PINK", 4)]
SEG = ["BASE", "MID", "TOP"]
POSES = [0, 1, 3, 7, 8]
FRAME_JOINTS = ["Root_joint", "INDEX_BASE_joint", "MIDDLE_F_BASE_joint", "RING_BASE_joint", "PINK_BASE_joint"]
MODELDEF_SCALE = 0.0106952          # vrealms_iwad/MODELDEF.txt
CONVERTER_UPM = 93.5                # the converter's own units per metre (fbx2iqm.py header)

fails = 0
passes = 0


def check(cond, msg):
    global fails, passes
    if cond:
        passes += 1
    else:
        fails += 1
        print("FAIL", msg)


# ---- VSMatrix, as matrix.cpp: column vectors, every op multiplies on the RIGHT ----
class M:
    def __init__(self):
        self.m = np.eye(4)

    def translate(self, x, y, z):
        t = np.eye(4); t[:3, 3] = (x, y, z); self.m = self.m @ t

    def scale(self, x, y, z):
        self.m = self.m @ np.diag([x, y, z, 1.0])

    def rotate(self, deg, x, y, z):
        v = np.array([x, y, z], float); v /= np.linalg.norm(v)
        c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
        X, Y, Z = v
        r = np.eye(4)
        r[0, 0] = c + X * X * (1 - c); r[0, 1] = X * Y * (1 - c) - Z * s; r[0, 2] = X * Z * (1 - c) + Y * s
        r[1, 0] = X * Y * (1 - c) + Z * s; r[1, 1] = c + Y * Y * (1 - c); r[1, 2] = Y * Z * (1 - c) - X * s
        r[2, 0] = X * Z * (1 - c) - Y * s; r[2, 1] = Y * Z * (1 - c) + X * s; r[2, 2] = c + Z * Z * (1 - c)
        self.m = self.m @ r

    def mult_colmajor(self, a16):
        self.m = self.m @ np.array(a16, float).reshape(4, 4).T

    def mult_quat(self, q):
        r = np.eye(4); r[:3, :3] = quat_to_mat(q); self.m = self.m @ r


SWAP = np.array([[1, 0, 0], [0, 0, 1], [0, 1, 0]], float)     # file <-> render (models_iqm.cpp LoadPosition)

# vr_handpose.cpp TestGrip: grip axes in the camera-local frame (+x right, +y up, +z back), column-major
TEST_AXES = {0: [0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, 0, 0, 0, 0, 1],
             1: [0, 0, 1, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1]}
TEST_FWD, TEST_SIDE, TEST_UP = 0.40, 0.11, -0.04


def test_grip(side, upm, ps, eye, yaw, turn, axes=None):
    g = M()
    g.translate(eye[0], eye[2], eye[1])
    g.scale(upm, upm, -upm)
    g.scale(1, 1 / ps, 1)
    g.rotate(-90 + yaw, 0, 1, 0)
    g.translate(TEST_SIDE if side == 0 else -TEST_SIDE, TEST_UP, -TEST_FWD)
    g.rotate(-turn if side == 0 else turn, 0, 1, 0)
    if axes is None:
        g.mult_colmajor(TEST_AXES[side])
    else:
        g.m = g.m @ axes
    return g


def object_to_world(grip, fit16, seat16):
    """models.cpp ObjectToWorldMatrix on the grip path: hand frame, then step 3 scale (MODELDEF
    Scale: x, z, y = all the same), offsets/rotations/pivot all zero, then the seat; NoPixelStretch."""
    o = M(); o.m = grip.m.copy()
    o.mult_colmajor(fit16)
    o.scale(MODELDEF_SCALE, MODELDEF_SCALE, MODELDEF_SCALE)
    o.mult_colmajor(seat16)
    return o.m


def to_visual_map(p_render, eye, ps):
    """render (x, height, y) -> map (x, y, z), the pixel stretch undone about the eye."""
    x, h, y = p_render[..., 0], p_render[..., 1], p_render[..., 2]
    z = eye[2] + (h - eye[2]) * ps
    return np.stack([x, y, z], -1)


def posed_globals(model, rot_by_joint):
    """Frame 0's locals with the given joints' rotations replaced (VRHandPose_Apply)."""
    t = model.frame_t[0].copy(); q = model.frame_q[0].copy(); s = model.frame_s[0].copy()
    for j, r in rot_by_joint.items():
        q[j] = r
    return model.globals_from_locals(t, q, s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default=None)
    ap.add_argument("--exe", default="/tmp/test_handpose")
    ap.add_argument("--out", default="/tmp/hands_test_out")
    ap.add_argument("--upm", type=float, default=107.4)
    ap.add_argument("--no-pictures", action="store_true")
    a = ap.parse_args()
    cands = [a.models] if a.models else [os.path.join(HERE, "..", "..", "vrealms_iwad", "models", "hands"),
                                         "/home/claude/wt_fbx2iqm/vrealms_iwad/models/hands"]
    mdir = next((c for c in cands if c and os.path.exists(os.path.join(c, "hand_r.iqm"))), None)
    if mdir is None:
        sys.exit("no hand_r.iqm in %s" % cands)
    os.makedirs(a.out, exist_ok=True)
    print("models:", os.path.abspath(mdir))

    results = {}
    for side, fn in ((0, "hand_r.iqm"), (1, "hand_l.iqm")):
        model = IQM(os.path.join(mdir, fn))
        jidx = {(f, k): model.joint("%s_%s_joint" % (name, SEG[k])) for name, f in FINGERS for k in range(3)}
        pf = os.path.join(a.out, "poses_%d.txt" % side)
        of = os.path.join(a.out, "out_%d.txt" % side)
        with open(pf, "w") as fp:
            for p in POSES:
                for (f, k), j in jidx.items():
                    q = model.frame_q[p, j]
                    fp.write("q %d %d %d %.9f %.9f %.9f %.9f\n" % (p, f, k, *q))
            for k, n in enumerate(FRAME_JOINTS):
                fp.write("p %d %.9f %.9f %.9f\n" % (k, *model.base[model.joint(n)][:3, 3]))
        r = subprocess.run([a.exe, pf, of], capture_output=True, text=True)
        print(fn, "C++:", r.stdout.strip().splitlines()[-1])
        check(r.returncode == 0, "%s: test_handpose exit %d" % (fn, r.returncode))

        runs, expect, steps, seat, fit, sock = {}, {}, [], {}, {}, {}
        for line in open(of):
            w = line.split()
            if w[0] == "r":
                runs.setdefault(w[1], {})[(int(w[2]), int(w[3]))] = np.array(list(map(float, w[4:8])))
            elif w[0] == "e":
                expect[w[1]] = int(w[2])
            elif w[0] == "step":
                steps.append((int(w[1]), float(w[2]), float(w[3])))
            elif w[0] == "seat":
                seat[int(w[1])] = list(map(float, w[2:18]))
            elif w[0] == "fit":
                fit[int(w[1])] = list(map(float, w[2:18]))
            elif w[0] == "socket":
                sock[int(w[1])] = np.array(list(map(float, w[2:5])))

        # 1. every named input reproduces its baked frame
        mm = 1000.0 / CONVERTER_UPM     # file units -> millimetres
        verts = {}
        for name, fr in expect.items():
            g = posed_globals(model, {jidx[k]: q for k, q in runs[name].items()})
            got = model.skin(g)
            want = model.skin(model.frame_globals(fr))
            d = np.linalg.norm(got - want, axis=1).max() * mm
            print("  %-9s -> frame %2d: largest vertex difference %.4f mm" % (name, fr, d))
            check(d < 0.05, "%s %s: %.4f mm from frame %d" % (fn, name, d, fr))
            verts[name] = got
        # analog: the middle finger's tip moves monotonically from open to fist as grip rises
        tipj = model.joint("MIDDLE_F_UP_TOP_joint")
        open_tip = model.frame_globals(0)[tipj][:3, 3]
        fist_tip = model.frame_globals(3)[tipj][:3, 3]
        prev = -1
        for name in ("analog0.25", "analog0.50", "analog0.75"):
            g = posed_globals(model, {jidx[k]: q for k, q in runs[name].items()})
            frac = np.linalg.norm(g[tipj][:3, 3] - open_tip) / np.linalg.norm(fist_tip - open_tip)
            print("  %s: middle tip %.0f%% of the way open -> fist" % (name, frac * 100))
            check(frac > prev, "%s: analog sweep not monotone" % fn)
            prev = frac
        # step response = 1 - 0.75^n
        worst = max(abs(g_ - e) for _, g_, e in steps)
        print("  step response open->fist at 90 Hz, frames 1..12: %s (worst |got - (1-0.75^n)| = %.4f)"
              % (" ".join("%.2f" % g_ for _, g_, _ in steps), worst))
        check(worst < 0.01, "%s: step response off by %.4f" % (fn, worst))

        # 2. placement on the flat test's grip
        upm, ps, eye, yaw = a.upm, 1.2, np.array([0.0, 0.0, 0.0]), 90.0   # camera faces north (+y)
        grip = test_grip(side, upm, ps, eye, yaw, 0.0)
        o2w = object_to_world(grip, fit[side], seat[side])
        lin = o2w[:3, :3]
        det_render = np.linalg.det(lin)
        # file -> render (swap) -> o2w -> map (swap): the handedness that reaches the screen
        det_total = np.linalg.det(SWAP @ lin @ SWAP)
        check(det_total > 0, "%s: mirrored (det %g)" % (fn, det_total))
        culled_right = (det_render > 0)      # BeginDrawModel: positive scales -> the world path's culling
        check(culled_right, "%s: render det %g < 0 with positive MODELDEF scales -> inside out" % (fn, det_render))

        def to_map(v_file):
            v_r = v_file @ SWAP.T
            p = (o2w @ np.c_[v_r, np.ones(len(v_r))].T).T[:, :3]
            return to_visual_map(p, eye, ps)

        g0 = model.frame_globals(0)
        wrist = to_map(g0[model.joint("Root_joint")][:3, 3][None])[0]
        mtip = to_map(g0[tipj][:3, 3][None])[0]
        ttip = to_map(g0[model.joint("THUMB_UP_TOP_joint")][:3, 3][None])[0]
        back = to_map(np.array([[0, 0, 1.0]]))[0] - to_map(np.array([[0, 0, 0.0]]))[0]   # file +Z = back of the hand
        socket = to_map(sock[side][None])[0]
        fingers = (mtip - wrist) / np.linalg.norm(mtip - wrist)
        # 0.19 m is wrist to the middle FINGERTIP of the mesh (the converter's contract), not to the tip
        # joint: the furthest skinned vertex along the fingers.
        mesh0 = to_map(model.skin(g0))
        length_m = ((mesh0 - wrist) @ fingers).max() / upm
        back /= np.linalg.norm(back)
        want_socket = np.array([TEST_SIDE if side == 0 else -TEST_SIDE, TEST_FWD, TEST_UP]) * upm
        sname = "right" if side == 0 else "left"
        print("  placement (%s, %.1f units/m): wrist->middle fingertip (mesh) %.4f m; fingers (%.2f %.2f %.2f); back of hand "
              "(%.2f %.2f %.2f); thumb tip x %.1f vs wrist x %.1f; socket (%.1f %.1f %.1f) want (%.1f %.1f %.1f); det %.3g"
              % (sname, upm, length_m, *fingers, *back, ttip[0], wrist[0], *socket, *want_socket, det_total))
        check(abs(length_m - 0.19) < 0.002, "%s: hand %.4f m, not 0.19" % (fn, length_m))
        check(np.linalg.norm(socket - want_socket) < 0.01 * upm, "%s: socket not on the grip" % fn)
        check(fingers[2] > 0.9, "%s: fingers not up" % fn)
        check(back[1] > 0.9, "%s: back of the hand not away from the camera" % fn)
        check((ttip[0] - wrist[0]) * (1 if side == 0 else -1) > 0, "%s: thumb on the wrong side" % fn)

        # the same at another metre: still 0.19 m (the live roth_vr_units_per_metre case)
        for upm2 in (93.5, 34.0):
            o2 = object_to_world(test_grip(side, upm2, ps, eye, yaw, 0.0), fit[side], seat[side])
            vv = np.r_[[g0[0][:3, 3]], model.skin(g0)] @ SWAP.T
            p2 = to_visual_map((o2 @ np.c_[vv, np.ones(len(vv))].T).T[:, :3], eye, ps)
            l2 = ((p2[1:] - p2[0]) @ fingers).max() / upm2
            check(abs(l2 - 0.19) < 0.002, "%s at %.1f units/m: %.4f m" % (fn, upm2, l2))
            print("  at %.1f units/m: wrist->middle fingertip %.4f m" % (upm2, l2))

        # a natural hold: OpenXR grip -Z up-forward 45 deg, +X right (into the right palm / out of the left)
        c45 = math.sqrt(0.5)
        Zg = np.array([0, -c45, c45])                       # camera-local (+x right, +y up, +z back)
        Xg = np.array([1.0, 0, 0])
        Yg = np.cross(Zg, Xg)
        R = np.eye(4); R[:3, 0], R[:3, 1], R[:3, 2] = Xg, Yg, Zg
        o3 = object_to_world(test_grip(side, upm, ps, eye, yaw, 0.0, axes=R), fit[side], seat[side])

        def m3(v):
            return to_visual_map((o3 @ np.r_[SWAP @ v, 1.0])[None, :3], eye, ps)[0]
        w3, t3, th3 = m3(g0[0][:3, 3]), m3(g0[tipj][:3, 3]), m3(g0[model.joint("THUMB_UP_TOP_joint")][:3, 3])
        b3 = m3(np.array([0, 0, 1.0])) - m3(np.zeros(3)); b3 /= np.linalg.norm(b3)
        f3 = (t3 - w3) / np.linalg.norm(t3 - w3); h3 = (th3 - w3) / np.linalg.norm(th3 - w3)
        print("  natural hold (%s): fingers (%.2f %.2f %.2f), back of hand (%.2f %.2f %.2f), wrist->thumb tip "
              "(%.2f %.2f %.2f)  [map: x right, y forward, z up]" % (sname, *f3, *b3, *h3))
        results[side] = dict(model=model, verts=verts, seat=seat[side], fit=fit[side])

    # 3. pictures
    if not a.no_pictures:
        draw(results, a)
    print("test_iqm: %d passed, %d failed" % (passes, fails))
    return 1 if fails else 0


def draw(results, a):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("no PIL: no pictures")
        return
    W, H = 640, 480
    upm, ps, eye, yaw = a.upm, 1.2, np.array([0.0, 0.0, 0.0]), 90.0
    f = (W / 2) / math.tan(math.radians(45))
    names = ["open", "fist", "point", "thumbup", "fingergun", "trigger", "ready"]
    sheet = Image.new("RGB", (W * len(names) // 2, H * 2 // 2), (40, 40, 48))
    for turn in (0, 90):
        for ni, name in enumerate(names):
            img = Image.new("RGB", (W, H), (40, 40, 48))
            dr = ImageDraw.Draw(img)
            tris = []
            for side, r in results.items():
                model = r["model"]
                o2w = object_to_world(test_grip(side, upm, ps, eye, yaw, turn), r["fit"], r["seat"])
                v = r["verts"][name] @ SWAP.T
                p = to_visual_map((o2w @ np.c_[v, np.ones(len(v))].T).T[:, :3], eye, ps)
                for t in model.triangles:
                    a3 = p[t]
                    n = np.cross(a3[1] - a3[0], a3[2] - a3[0])
                    nl = np.linalg.norm(n)
                    if nl == 0:
                        continue
                    c = a3.mean(0)
                    if c[1] <= 1:
                        continue
                    shade = abs(n @ (c / np.linalg.norm(c))) / nl
                    col = (205, 160, 130) if side == 0 else (175, 150, 205)
                    pts = [(W / 2 + q[0] / q[1] * f, H / 2 - q[2] / q[1] * f) for q in a3]
                    tris.append((c[1], pts, tuple(int(k * (0.25 + 0.75 * shade)) for k in col)))
            for _, pts, col in sorted(tris, key=lambda x: -x[0]):
                dr.polygon(pts, fill=col)
            dr.text((8, 8), "%s  turn %d  (right hand tan, left violet)" % (name, turn), fill=(255, 255, 255))
            fn = os.path.join(a.out, "hands_%s_t%d.png" % (name, turn))
            img.save(fn)
            small = img.resize((W // 2, H // 2))
            sheet.paste(small, ((ni % len(names)) * W // 2, (0 if turn == 0 else 1) * H // 2))
    sheet.save(os.path.join(a.out, "hands_sheet.png"))
    print("pictures:", os.path.abspath(a.out))


if __name__ == "__main__":
    sys.exit(main())
