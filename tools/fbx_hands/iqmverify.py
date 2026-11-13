#!/usr/bin/env python3
"""Check hand_r.iqm / hand_l.iqm against hand_final.fbx, from the written bytes.

    python3 -I tools/fbx_hands/iqmverify.py [--models DIR] [--fbx PATH] [--quick]

Written separately from fbx2iqm.py and imports nothing from it: the IQM is decoded here from raw bytes
(its own header, joint, pose, frame, vertex-array parsers), and the FBX side is sampled with poses.py
(Hand.sample) and analyze.py (local_matrix), the pure-Python evaluator of the FBX report.

  V0  structure: what the engine's loader (src/common/models/models_iqm.cpp) accepts
  V1  every take frame (1278 per hand): IQM joint local rotation vs the FBX, max angle error per joint;
      translations too; Root_joint and HANDPALM_joint must hold the bind in every frame
  V2  frames 0-10 and 1289-1299: an authored pose's finger joints must equal the FBX curves of the slot
      rig (recipe_solved.json "slots_rig", sampled here) at its take frame, wrist at the bind; a
      curl-table pose's finger joints must match the recipe (axes, adduction, ceilings, applied curls);
      1298 must equal the slot rig's FBX take frame 230 the same way
  V2X frames 0-10 and 1289-1299 of hand_l.iqm are the reflection of hand_r.iqm's (both hands take the
      slots of the one rig, as the owner mirrors his one hand)
  V3  wrist (origin) to middle fingertip at frame 0 from the skinned mesh, in map units
  V5  handedness and the file axes: thumb side, palm side (from take frame 500, the FBX's held fist), forward
  TX  a textured render of frame 0 (palm and back) from the IQM's own UVs -> sheets/texture_check.png
"""
import argparse
import json
import math
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from analyze import Scene, local_matrix  # noqa: E402
from poses import Hand  # noqa: E402

UPM = 34.0 * 154.0 / 56.0
TAKE0, NTAKE = 11, 1278
DM = np.diag([1.0, -1.0, 1.0])


# ------------------------------------------------------------------ an IQM reader of its own
class IqmFile:
    def __init__(self, path):
        b = open(path, "rb").read()
        self.size = len(b)
        assert b[:16] == b"INTERQUAKEMODEL\0", "magic"
        h = struct.unpack_from("<27I", b, 16)
        (self.version, self.filesize, self.flags, n_text, o_text, n_mesh, o_mesh, n_va, self.nv, o_va,
         self.ntri, o_tri, o_adj, self.nj, o_j, self.np_, o_p, self.na, o_a, self.nf, self.nfc, o_f,
         o_b, n_c, o_c, n_x, o_x) = h
        txt = b[o_text:o_text + n_text]

        def s(o):
            return txt[o:txt.index(b"\0", o)].decode()
        self.meshes = [(s(m[0]), s(m[1])) + m[2:] for m in
                       (struct.unpack_from("<6I", b, o_mesh + 24 * i) for i in range(n_mesh))]
        self.arrays = {}
        for i in range(n_va):
            t, fl, fmt, size, off = struct.unpack_from("<5I", b, o_va + 20 * i)
            dt = {1: np.uint8, 7: np.float32}[fmt]
            self.arrays[t] = (fmt, size, np.frombuffer(b, dt, self.nv * size, off).reshape(self.nv, size))
        self.tris = np.frombuffer(b, np.uint32, self.ntri * 3, o_tri).reshape(-1, 3)
        self.adj = np.frombuffer(b, np.uint32, self.ntri * 3, o_adj).reshape(-1, 3)
        self.jname, self.jpar, self.jt, self.jq, self.js = [], [], [], [], []
        for i in range(self.nj):
            r = struct.unpack_from("<Ii3f4f3f", b, o_j + 48 * i)
            self.jname.append(s(r[0])); self.jpar.append(r[1])
            self.jt.append(np.array(r[2:5])); self.jq.append(np.array(r[5:9])); self.js.append(np.array(r[9:12]))
        self.pose = [struct.unpack_from("<iI10f10f", b, o_p + 88 * i) for i in range(self.np_)]
        self.anims = [(s(r[0]),) + r[1:] for r in (struct.unpack_from("<3IfI", b, o_a + 20 * i) for i in range(self.na))]
        raw = np.frombuffer(b, np.uint16, self.nf * self.nfc, o_f).reshape(self.nf, self.nfc).astype(np.float64)
        # decode every channel of every frame
        self.ch = np.zeros((self.nf, self.np_, 10))
        k = 0
        for j, p in enumerate(self.pose):
            mask, off, scl = p[1], p[2:12], p[12:22]
            for c in range(10):
                self.ch[:, j, c] = off[c]
                if mask & (1 << c):
                    self.ch[:, j, c] += raw[:, k] * scl[c]
                    k += 1
        assert k == self.nfc
        self.bounds = np.frombuffer(b, np.float32, self.nf * 8, o_b).reshape(self.nf, 8)
        self.comment = b[o_c:o_c + n_c].rstrip(b"\0").decode() if n_c else ""

    @staticmethod
    def qmat(q):
        x, y, z, w = q / np.linalg.norm(q)
        return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                         [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                         [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])

    def local(self, f, j):
        c = self.ch[f, j]
        M = np.eye(4); M[:3, :3] = self.qmat(c[3:7]) * c[7:10][None, :]; M[:3, 3] = c[0:3]
        return M

    def bind_local(self, j):
        M = np.eye(4); M[:3, :3] = self.qmat(self.jq[j]) * self.js[j][None, :]; M[:3, 3] = self.jt[j]
        return M

    def worlds(self, f=None):
        W = []
        for j, p in enumerate(self.jpar):
            L = self.bind_local(j) if f is None else self.local(f, j)
            W.append(L if p < 0 else W[p] @ L)
        return W

    def skin(self, f):
        B, P = self.worlds(None), self.worlds(f)
        S = [P[j] @ np.linalg.inv(B[j]) for j in range(self.nj)]
        pos = self.arrays[0][2].astype(np.float64)
        bi = self.arrays[4][2].astype(int)
        bw = self.arrays[5][2].astype(np.float64) / 255.0
        out = np.zeros_like(pos)
        for k in range(4):
            M = np.stack([S[j] for j in bi[:, k]])
            out += (np.einsum("vij,vj->vi", M[:, :3, :3], pos) + M[:, :3, 3]) * bw[:, k:k + 1]
        return out


def angle_deg(Ra, Rb):
    c = (np.trace(Ra.T @ Rb) - 1.0) / 2.0
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def axis_angle(axis, deg):
    a = np.asarray(axis, float); a = a / np.linalg.norm(a)
    t = math.radians(deg); c, s = math.cos(t), math.sin(t); C = 1 - c; x, y, z = a
    return np.array([[c + x * x * C, x * y * C - z * s, x * z * C + y * s],
                     [y * x * C + z * s, c + y * y * C, y * z * C - x * s],
                     [z * x * C - y * s, z * y * C + x * s, c + z * z * C]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default=os.path.join(REPO, "vrealms_iwad", "models", "hands"))
    ap.add_argument("--fbx", default="/home/claude/dxr/models/hands/hand_final.fbx")
    ap.add_argument("--quick", action="store_true", help="V1 on every 10th take frame only")
    a = ap.parse_args()
    solved = json.load(open(os.path.join(HERE, "recipe_solved.json")))
    taper = solved["taper"]
    sc = Scene(a.fbx)
    fails = []
    logf = open(os.path.join(HERE, "iqmverify.log"), "w")

    def log(s=""):
        print(s); logf.write(s + "\n")

    def check(ok, what):
        log(("  PASS " if ok else "  FAIL ") + what)
        if not ok:
            fails.append(what)

    results = {}
    files = {}
    slot_top = solved["slots_rig"]
    slot_id = [o for o, n in sc.objs.items() if n.name == "Model" and sc.name(o) == slot_top][0]
    slot_hand = Hand(sc, slot_top, slot_id)
    for side, fname, top in (("right", "hand_r.iqm", "GLOBAL_MAIN_CONTROL_R"), ("left", "hand_l.iqm", "GLOBAL_MAIN_CONTROL_R1")):
        path = os.path.join(a.models, fname)
        m = IqmFile(path)
        files[side] = m
        left = side == "left"
        log("=" * 20 + " %s (%d bytes)" % (path, m.size))
        # ---------------------------------------------------------------- V0
        log("V0 structure")
        check(m.version == 2 and m.filesize == m.size, "IQM v2, filesize field = file size")
        top_id = [o for o, n in sc.objs.items() if n.name == "Model" and sc.name(o) == top][0]
        hand = Hand(sc, top, top_id)
        check(sorted(m.jname) == sorted(hand.joints) and m.nj == 22, "22 joints, names exactly the FBX's: %s" % ", ".join(m.jname))
        check(all(p < j for j, p in enumerate(m.jpar)), "every parent precedes its child (models_iqm.cpp:156-165)")
        par_ok = all((m.jpar[j] < 0 and n == "Root_joint") or
                     (m.jpar[j] >= 0 and sc.name(sc.model_parent(hand.joints[n])) == m.jname[m.jpar[j]])
                     for j, n in enumerate(m.jname))
        check(par_ok, "parent links equal the FBX's")
        fmts = {t: (f, s_) for t, (f, s_, _) in m.arrays.items()}
        check(fmts.get(0) == (7, 3) and fmts.get(1) == (7, 2) and fmts.get(2) == (7, 3) and fmts.get(4) == (1, 4)
              and fmts.get(5) == (1, 4), "vertex arrays the loader takes: position f3, texcoord f2, normal f3, "
              "blendindexes ub4, blendweights ub4 (+ tangent f4 %s, ignored by the loader)" % (fmts.get(3),))
        bw = m.arrays[5][2].astype(int); bi = m.arrays[4][2].astype(int)
        check((bw.sum(1) == 255).all() and (bi < 22).all(), "weights sum to 255 on all %d vertices; indices < 22" % m.nv)
        check(int(m.tris.max()) < m.nv and all((x == 0xFFFFFFFF) or x < m.ntri for x in m.adj.flatten()),
              "%d triangles in range; adjacency in range" % m.ntri)
        check(all(np.allclose(sv, 1.0) for sv in m.js) and np.allclose(m.ch[:, :, 7:10], 1.0), "every joint scale is exactly 1 (bind and frames)")
        check(m.nf == 1300, "1300 frames (%d channels per frame)" % m.nfc)
        log("  anims: " + ", ".join("%s@%d+%d" % (n, f0, nn) for n, f0, nn, _, _ in m.anims))
        P = m.arrays[0][2].astype(float); N = m.arrays[2][2].astype(float)
        A, B, C = P[m.tris[:, 0]], P[m.tris[:, 1]], P[m.tris[:, 2]]
        agree = ((np.cross(B - A, C - A) * (N[m.tris[:, 0]] + N[m.tris[:, 1]] + N[m.tris[:, 2]])).sum(1) > 0).mean()
        check(agree < 0.01, "winding: %.4f of triangles have (b-a)x(c-a) along the normal (engine's IQMs: 0.000-0.001)" % agree)
        # ---------------------------------------------------------------- V1
        log("V1 take frames vs the FBX (poses.py / analyze.py evaluator)")
        J = {n: i for i, n in enumerate(m.jname)}
        # scale from bind translations: |IQM bind t| / |FBX rest t| on finger joints
        ratios = []
        for n in m.jname:
            if n in ("Root_joint", "HANDPALM_joint"):
                continue
            ft = np.array(local_matrix(hand.mp[hand.joints[n]]))[:3, 3]
            ratios.append(np.linalg.norm(m.jt[J[n]]) / np.linalg.norm(ft))
        s = float(np.mean(ratios))
        log("  scale from bind translations: %.6f (spread %.2e)" % (s, np.ptp(ratios)))
        frames = range(0, NTAKE, 10) if a.quick else range(NTAKE)
        maxerr = {n: 0.0 for n in m.jname}
        maxterr = 0.0
        wrist_fbx = 0.0
        Dm = DM if left else np.eye(3)
        for f in frames:
            for n in m.jname:
                j = J[n]; fj = hand.joints[n]
                R = hand.sample(fj, "Lcl Rotation", f); T = hand.sample(fj, "Lcl Translation", f)
                Lf = np.array(local_matrix(hand.mp[fj], R, T))
                Li = m.local(TAKE0 + f, j)
                if n in ("Root_joint", "HANDPALM_joint"):
                    # stripped: must equal the bind
                    e = angle_deg(m.bind_local(j)[:3, :3], Li[:3, :3])
                    maxerr[n] = max(maxerr[n], e)
                    if n == "HANDPALM_joint":
                        wrist_fbx = max(wrist_fbx, angle_deg(np.array(local_matrix(hand.mp[fj]))[:3, :3], Lf[:3, :3]))
                    continue
                exp = Dm @ Lf[:3, :3] @ Dm
                maxerr[n] = max(maxerr[n], angle_deg(exp, Li[:3, :3]))
                maxterr = max(maxterr, np.linalg.norm(s * (Dm @ Lf[:3, 3]) - Li[:3, 3]))
        worst = max((v, k) for k, v in maxerr.items() if k not in ("Root_joint", "HANDPALM_joint"))
        log("  %d frames x 20 finger joints: max rotation error %.5f deg (%s); max translation error %.5f map units"
            % (len(frames), worst[0], worst[1], maxterr))
        for n in m.jname:
            log("    %-22s max %.5f deg" % (n, maxerr[n]))
        check(worst[0] < 0.05, "take: every finger joint local rotation within 0.05 deg of the FBX")
        check(maxerr["Root_joint"] < 1e-3 and maxerr["HANDPALM_joint"] < 1e-3,
              "Root_joint and HANDPALM_joint hold the bind in every take frame (the FBX wrist turns up to %.1f deg there; stripped)" % wrist_fbx)
        root_t = max(np.abs(m.ch[f, J["Root_joint"], 0:3]).max() for f in range(m.nf))
        check(root_t < 1e-4 and np.abs(m.jt[J["Root_joint"]]).max() < 1e-4, "Root_joint at the origin in the bind and in all 1300 frames (max |t| %.2e)" % root_t)
        results[side] = {"take_max_rot_err_deg": worst, "take_max_t_err": maxterr}

        def authored_err(m_, J_, row, fbx_frame, Dm=Dm, s=s):
            """(max finger-joint rotation error, max wrist-vs-bind error) in degrees, and the max
            finger-joint translation error, of IQM row `row` against the slot rig's FBX take frame."""
            rmax, wmax, tmax = 0.0, 0.0, 0.0
            for n in m_.jname:
                j = J_[n]
                Li = m_.local(row, j)
                if n in ("Root_joint", "HANDPALM_joint"):
                    wmax = max(wmax, angle_deg(m_.bind_local(j)[:3, :3], Li[:3, :3]))
                    continue
                fj = slot_hand.joints[n]
                R = slot_hand.sample(fj, "Lcl Rotation", fbx_frame); T = slot_hand.sample(fj, "Lcl Translation", fbx_frame)
                Lf = np.array(local_matrix(slot_hand.mp[fj], R, T))
                rmax = max(rmax, angle_deg(Dm @ Lf[:3, :3] @ Dm, Li[:3, :3]))
                tmax = max(tmax, np.linalg.norm(s * (Dm @ Lf[:3, 3]) - Li[:3, 3]))
            return (rmax, wmax), tmax
        # ---------------------------------------------------------------- V2
        log("V2 baked and hold poses vs the recipe (recipe_solved.json)")
        rs = solved[side]
        AX, AAX, ADD = rs["AXES"], rs["ADDUCT_AXES"], rs["ADDUCT"]
        fingers = ["INDEX", "MIDDLE_F", "RING", "PINK", "THUMB"]
        worst2 = 0.0
        n_auth = 0
        for fr in rs["frames"]:
            f, name, applied = fr["frame"], fr["name"], fr["applied"]
            if fr.get("source_frame_1based") is not None:
                # [TEP:1192-1201 / 1235-1241] an authored frame, verbatim, pose_only: every finger joint
                # equals the slot rig's FBX curves at that take frame; Root_joint and HANDPALM_joint
                # hold the bind.
                ra, ta = authored_err(m, J, f, fr["fbx_take_frame"])
                n_auth += 1
                log("  %4d %-13s AUTHORED: %s FBX take frame %d; max finger-joint rotation error %.5f deg, "
                    "translation %.5f map units; wrist vs bind %.5f deg" % (f, name, slot_top, fr["fbx_take_frame"], ra[0], ta, ra[1]))
                worst2 = max(worst2, ra[0], ra[1], 0.0 if ta < 1e-3 else 999.0)
                continue
            cells = []
            for fi, fg in enumerate(fingers):
                c = applied[fi]
                for sg in ("BASE", "MID", "TOP", "UP_TOP"):
                    n = "%s_%s_joint" % (fg, sg)
                    exp = axis_angle(AX[n], c * taper[sg])
                    if sg == "BASE" and fg in ADD:
                        exp = exp @ axis_angle(AAX[fg], ADD[fg] * c / 90.0)
                    exp = Dm @ exp @ Dm
                    rel = m.bind_local(J[n])[:3, :3].T @ m.local(f, J[n])[:3, :3]
                    err = angle_deg(exp, rel)
                    worst2 = max(worst2, err)
                    cells.append("%s%s %5.1f/%5.1f" % (fg[0] if fg != "THUMB" else "T", sg[0] if sg != "UP_TOP" else "U",
                                                      angle_deg(np.eye(3), exp), angle_deg(np.eye(3), rel)))
            log("  %4d %-13s table %-22s applied %-22s" % (f, name, fr["table"], applied))
            log("        expected/measured deg: " + "  ".join(cells))
        check(worst2 < 0.05, "frames 0-10, 1289-1297: %d authored rows within 0.05 deg of their FBX take frames (wrist at bind); every finger joint of the other %d within 0.05 deg of the recipe's rotation (max %.5f)" % (n_auth, len(rs["frames"]) - n_auth, worst2))
        # 1298, 1299
        ra, ta = authored_err(m, J, 1298, solved[side]["reach_claw_fbx_take_frame"])
        check(max(ra) < 0.05 and ta < 1e-3, "1298 reach_claw = %s FBX take frame %d (the owner's row 241): finger joints "
              "within %.5f deg / %.5f map units, wrist at the bind (%.5f deg)"
              % (slot_top, solved[side]["reach_claw_fbx_take_frame"], ra[0], ta, ra[1]))
        e98 = np.abs(m.ch[1298] - m.ch[241]).max()
        sal = m.ch[5].copy()
        for n in m.jname:
            if n.startswith("MIDDLE_F"):
                sal[J[n], 3:7] = m.ch[0, J[n], 3:7]
        e99 = np.abs(m.ch[1299] - sal).max()
        if not left:
            check(e98 < 1e-9, "1298 reach_claw = row 241 channel for channel (max diff %.1e)" % e98)
        else:
            log("  (left: row 241 is this hand's own take; 1298 is the slot rig's frame, checked above; differs from row 241 by %.3f)" % e98)
        check(e99 < 1e-9, "1299 salute = row 5 with MIDDLE_F_* rotation channels of row 0 (max diff %.1e)" % e99)
        results[side]["recipe_max_err_deg"] = worst2
        # ---------------------------------------------------------------- V3
        log("V3 wrist to middle fingertip at frame 0, skinned")
        v0 = m.skin(0)
        dom = [m.jname[bi[i, int(np.argmax(bw[i]))]] for i in range(m.nv)]
        tipv = [i for i in range(m.nv) if dom[i] in ("MIDDLE_F_TOP_joint", "MIDDLE_F_UP_TOP_joint")]
        L = max(np.linalg.norm(v0[i]) for i in tipv)
        far = max(tipv, key=lambda i: np.linalg.norm(v0[i]))
        check(abs(L - 0.19 * UPM) < 0.02, "wrist (origin = Root_joint) to middle fingertip %.4f map units = %.4f m at %.1f u/m (target 0.19 m = %.4f)"
              % (L, L / UPM, UPM, 0.19 * UPM))
        log("  that vertex at %s; mesh bbox %s .. %s" % (np.round(v0[far], 3), np.round(v0.min(0), 2), np.round(v0.max(0), 2)))
        results[side]["length"] = L
        # ---------------------------------------------------------------- V5
        log("V5 handedness and axes")
        W0, W3 = m.worlds(0), m.worlds(3)

        def tip(i_, v):
            return v[i_]
        thumbv = [i for i in range(m.nv) if dom[i] in ("THUMB_TOP_joint", "THUMB_UP_TOP_joint")]
        thumb_tip = v0[max(thumbv, key=lambda i: np.linalg.norm(v0[i]))]
        fwd = v0[far] / np.linalg.norm(v0[far])
        # The closing direction, from the FBX's own held fist (take frame 500: FBX report, frames
        # 319-685), not from a pose slot the picker may fill with any frame: how the middle knuckles
        # (the MID joints) move from open to that fist, across the hand.
        Wf = m.worlds(TAKE0 + 500)
        mids = [Wf[J["%s_MID_joint" % fg]][:3, 3] - W0[J["%s_MID_joint" % fg]][:3, 3]
                for fg in ("INDEX", "MIDDLE_F", "RING", "PINK")]
        palm_dir = np.mean(mids, 0)
        palm_dir -= fwd * (palm_dir @ fwd)
        palm_dir /= np.linalg.norm(palm_dir)
        knuck = np.mean([W0[J[n]][:3, 3] for n in ("INDEX_BASE_joint", "MIDDLE_F_BASE_joint", "RING_BASE_joint", "PINK_BASE_joint")], 0)
        side_v = thumb_tip - knuck
        side_v -= fwd * (side_v @ fwd)
        h = float(np.cross(side_v, fwd) @ palm_dir)
        log("  forward (middle tip) %s; thumb tip %s; middle knuckles close (open -> take fist, across the hand) towards %s" % (np.round(fwd, 3), np.round(thumb_tip, 2), np.round(palm_dir, 3)))
        log("  chirality (thumb side x forward) . (curl direction) = %+.3f  (+ = right hand, - = left)" % h)
        check((h > 0) == (not left), "%s hand is a %s hand" % (fname, "left" if left else "right"))
        check(fwd[0] > 0.98, "fingers point +X (forward)")
        check((thumb_tip[1] > 0) == (not left), "thumb on the %s side (y %s 0)" % ("+Y" if not left else "-Y", ">" if not left else "<"))
        check(palm_dir[2] < -0.5, "fingers close towards -Z: the palm faces -Z, +Z is the back of the hand")
        idx_y = W0[J["INDEX_BASE_joint"]][1, 3]; pink_y = W0[J["PINK_BASE_joint"]][1, 3]
        check((idx_y > pink_y) == (not left), "index knuckle on the thumb side of the little finger (y %.2f vs %.2f)" % (idx_y, pink_y))
        results[side]["chirality"] = h
        # ---------------------------------------------------------------- TX
        results[side]["uv"] = m.arrays[1][2].astype(float)
        results[side]["v0"] = v0
        results[side]["tris"] = m.tris
    # ---------------------------------------------------------------- V2X
    log("V2X hand_l.iqm's recipe rows vs hand_r.iqm's, reflected")
    mr, ml = files["right"], files["left"]
    Jr = {n: i for i, n in enumerate(mr.jname)}
    rows = list(range(0, 11)) + list(range(1289, 1300))
    wr, wt = 0.0, 0.0
    for f in rows:
        for n in mr.jname:
            Lr, Ll = mr.local(f, Jr[n]), ml.local(f, ml.jname.index(n))
            wr = max(wr, angle_deg(DM @ Lr[:3, :3] @ DM, Ll[:3, :3]))
            wt = max(wt, np.linalg.norm(DM @ Lr[:3, 3] - Ll[:3, 3]))
    check(wr < 0.01 and wt < 1e-3, "frames 0-10 and 1289-1299: left = right reflected across XZ, every joint "
          "(max %.5f deg, %.6f map units; 16-bit channel quantisation)" % (wr, wt))
    # textured render of frame 0 from the IQMs' own UVs
    try:
        from PIL import Image, ImageDraw
        tex = np.asarray(Image.open(os.path.join(a.models, "lambert1_Base_Color.png")).convert("RGB"))
        Ht, Wt = tex.shape[:2]
        tiles = []
        for side in ("right", "left"):
            r = results[side]
            for view, lab in ((np.array([[0, 1, 0], [1, 0, 0], [0, 0, -1.0]]), "palm"),
                              (np.array([[0, -1, 0], [1, 0, 0], [0, 0, 1.0]]), "back")):
                p = r["v0"] @ view.T          # screen x, screen y (fingers up), depth (+ towards viewer)
                mn, mx = p.min(0), p.max(0); c = (mn + mx) / 2; sc_ = (mx - mn)[:2].max()
                S = 360
                img = Image.new("RGB", (S, S + 24), (20, 20, 24)); d = ImageDraw.Draw(img)
                order = np.argsort(p[r["tris"]][:, :, 2].mean(1))
                for t in order:
                    tri = r["tris"][t]
                    uv = r["uv"][tri].mean(0)
                    col = tex[int(np.clip(uv[1] * Ht, 0, Ht - 1)), int(np.clip(uv[0] * Wt, 0, Wt - 1))]
                    d.polygon([((p[k][0] - c[0]) / sc_ * S * 0.9 + S / 2, -(p[k][1] - c[1]) / sc_ * S * 0.9 + S / 2) for k in tri],
                              fill=tuple(int(x) for x in col))
                d.text((6, S + 4), "%s hand, %s side (frame 0)" % (side, lab), fill=(230, 230, 230))
                tiles.append(img)
        out = Image.new("RGB", (360 * 4, 384))
        for i, t in enumerate(tiles):
            out.paste(t, (360 * i, 0))
        os.makedirs(os.path.join(HERE, "sheets"), exist_ok=True)
        tp = os.path.join(HERE, "sheets", "texture_check.png")
        out.save(tp)
        log("TX wrote %s (UVs as written: u, v with rows running down)" % tp)
    except Exception as e:  # noqa: BLE001
        log("TX texture render failed: %r" % e)
    log("")
    log("FAILURES: %d" % len(fails))
    for f in fails:
        log("  " + f)
    logf.close()
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
