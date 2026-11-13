#!/usr/bin/env python3
"""The synthetic hand poses of RS_Hands, rebuilt on hand_final.fbx's own rig, without Blender.

A port of the curl-angle half of the owner's tools/tools_export_poses.py (RS_Hands; read from
/home/claude/rs_hands_ref/tools/tools_export_poses.py, 1,274 lines). Every constant and rule below
carries the source's line numbers as  [TEP:nnn].  What is NOT ported, and why, is listed at the end
of this docstring and again in the build log.

How a pose is applied here, and why it is the same thing Blender did
---------------------------------------------------------------------
Blender: pose_world(bone) = pose_world(parent) @ rest_local(bone) @ basis(bone), with the basis a
rotation about the bone's head in the bone's own rest axes. Here: W[j] = W[parent] @ Lrest[j] @ R[j],
Lrest[j] = TL[parent]^-1 @ TL[j] from the FBX skin clusters' bind matrices (TransformLink), R[j] a
3x3 rotation in joint j's own rest axes. Blender's default FBX import (primary bone axis Y, secondary
X, no automatic orientation) keeps the FBX node axes as the bone axes [reasoning, not seen: from
memory of Blender's importer], so the axis searches below sample the same direction grids.

What a "tail" is
----------------
The script measures bone TAILS: where a finger ENDS [TEP:35]. A tail is a Blender import artefact (the
FBX has joints only), and on this Maya rig Blender's tails point along each joint's local Y, which is
SIDEWAYS on most of these joints (the bones run along local X). The port therefore uses the geometric
meaning of "where the finger ends":
  * tip(finger)  = the bind-mesh vertex furthest along TOP->UP_TOP among the vertices whose largest
                   skin weight is that finger's TOP or UP_TOP joint, carried rigidly by UP_TOP;
  * TOP tail     = the UP_TOP joint's head (the next joint down the finger).
This is a declared deviation from the Blender run: solved numbers (axes, adduction, ceilings, pinch,
holds) can differ from the RS_Hands build's.

Not ported (no effect on the curl-table poses, or overridden by the owner's order):
  * [TEP:801-1075] the authored-frame picker (POSE_SOURCE / HOLD_SOURCE). The owner ordered frames 0-10
    and 1289-1297 "from the curl table"; the README's table and render_wired_poses.py's header call
    0-10 synthetic. Reported, not applied.
  * [TEP:579-630] the palm thickness mesh scan: printed only, its result (PALM_HALF) is never used.
  * [TEP:400-410, 1141-1169] fold-check and splay diagnostics: the fold check IS ported, as a print.
  * [TEP:101-125] repivot / -90 X rotation: rigid, so no distance, dot product or angle changes.
"""
import math
import numpy as np

FINGERS = ["INDEX", "MIDDLE_F", "RING", "PINK", "THUMB"]          # [TEP:26]
SEGS = ["BASE", "MID", "TOP", "UP_TOP"]                             # [TEP:33]
TIPSEG = "UP_TOP"                                                   # [TEP:36]
TAPER = {"BASE": 1.0, "MID": 1.2, "TOP": 0.78, "UP_TOP": 0.35}      # [TEP:50]
FINGER4 = ("INDEX", "MIDDLE_F", "RING", "PINK")                     # [TEP:317]

#            index middle ring pink thumb                            [TEP:53-73]
POSES = [
    ("open",     [0, 0, 0, 0, 0]),
    ("point",    [0, 85, 90, 90, 14]),
    ("trigger",  [80, 0, 0, 0, 0]),
    ("fist",     [85, 85, 90, 90, 30]),
    ("pinch",    [-1, 85, 90, 90, -1]),     # solved [TEP:520-533]
    ("thumbout", [85, 85, 90, 90, 0]),
    ("gripfire", [85, 85, 90, 90, 25]),
    ("grip_tu",  [0, 85, 90, 90, 0]),
    ("ready_td", [30, 85, 90, 90, 25]),
    ("ready_tu", [30, 85, 90, 90, 0]),
    ("fire_tu",  [85, 85, 90, 90, 0]),
]

KNUCKLE_SPAN_CM = 7.5        # [TEP:146]
TEST_DEG = 85.0              # [TEP:167]
LATERAL_W = 3.0              # [TEP:168]
PALM_CLEAR_RADII = 2.4       # [TEP:626]
SAMPLES = (("MID", "head"), ("TOP", "head"), ("TOP", "tail"))   # [TEP:277]
PALM_CONTACT = "MIDDLE_F_BASE_joint"                              # [TEP:730]


def axis_angle(axis, deg):
    """mathutils.Quaternion(axis, angle) as a 3x3 matrix (axis normalised, right-hand rule)."""
    a = np.asarray(axis, dtype=np.float64)
    n = np.linalg.norm(a)
    if n < 1e-12 or deg == 0.0:
        return np.eye(3)
    x, y, z = a / n
    t = math.radians(deg)
    c, s = math.cos(t), math.sin(t)
    C = 1.0 - c
    return np.array([[c + x * x * C, x * y * C - z * s, x * z * C + y * s],
                     [y * x * C + z * s, c + y * y * C, y * z * C - x * s],
                     [z * x * C - y * s, z * y * C + x * s, c + z * z * C]])


def unit(v):
    v = np.asarray(v, dtype=np.float64)
    return v / np.linalg.norm(v)


def sphere_axes(step_deg, around=None, span_deg=None):
    """[TEP:170-195], same order of generation (ties keep the first)."""
    out = []
    if around is None:
        for th in range(0, 181, step_deg):
            st, ct = math.sin(math.radians(th)), math.cos(math.radians(th))
            for ph in range(0, 360, step_deg):
                v = np.array([st * math.cos(math.radians(ph)), ct, st * math.sin(math.radians(ph))])
                if np.linalg.norm(v) > 1e-6:
                    out.append(unit(v))
                if th in (0, 180):
                    break
    else:
        up = np.array([0.0, 0.0, 1.0]) if abs(around[2]) < 0.9 else np.array([1.0, 0.0, 0.0])
        e1 = unit(np.cross(around, up))
        e2 = unit(np.cross(around, e1))
        r = math.radians(span_deg)
        n = max(2, int(span_deg / step_deg))
        for i in range(-n, n + 1):
            for j in range(-n, n + 1):
                v = around + e1 * (r * i / n) + e2 * (r * j / n)
                if np.linalg.norm(v) > 1e-6:
                    out.append(unit(v))
    return out


CARDINALS = [np.array(v, dtype=np.float64) for v in
             ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1))]   # [TEP:212]


class Rig:
    """One hand's skeleton in FBX bind space, posed by per-joint basis rotations."""

    def __init__(self, names, parents, TL, tips, log=print):
        self.names = list(names)
        self.idx = {n: i for i, n in enumerate(self.names)}
        self.parents = list(parents)
        self.TL = [np.asarray(m, dtype=np.float64) for m in TL]
        self.Lrest = []
        for i, p in enumerate(self.parents):
            self.Lrest.append(self.TL[i] if p < 0 else np.linalg.inv(self.TL[p]) @ self.TL[i])
        # fingertip points in each UP_TOP joint's local frame
        self.tip_local = {}
        for f, pt in tips.items():
            j = self.idx[f"{f}_{TIPSEG}_joint"]
            self.tip_local[f] = (np.linalg.inv(self.TL[j]) @ np.append(pt, 1.0))[:3]
        self.basis = [np.eye(3) for _ in self.names]
        self.log = log
        self._W = None
        # recipe state
        self.AXES, self.ADDUCT, self.ADDUCT_AXES, self.MAX_CURL = {}, {}, {}, {}

    # ---------------------------------------------------------------- forward kinematics
    def fk(self):
        if self._W is not None:
            return self._W
        W = [None] * len(self.names)
        for i, p in enumerate(self.parents):
            R4 = np.eye(4)
            R4[:3, :3] = self.basis[i]
            W[i] = (self.Lrest[i] if p < 0 else W[p] @ self.Lrest[i]) @ R4
        self._W = W
        return W

    def set_basis(self, name, R):
        self.basis[self.idx[name]] = R
        self._W = None

    def rest_pose(self):                       # [TEP:264-267]
        self.basis = [np.eye(3) for _ in self.names]
        self._W = None

    def head(self, name):
        return self.fk()[self.idx[name]][:3, 3].copy()

    def tip(self, f):
        """tail of the finger's UP_TOP bone: the fingertip (see module docstring)."""
        W = self.fk()[self.idx[f"{f}_{TIPSEG}_joint"]]
        return (W @ np.append(self.tip_local[f], 1.0))[:3]

    def tail(self, f, seg):
        if seg == TIPSEG:
            return self.tip(f)
        nxt = SEGS[SEGS.index(seg) + 1]
        return self.head(f"{f}_{nxt}_joint")

    def worlds(self):
        return [w.copy() for w in self.fk()]

    # ---------------------------------------------------------------- the recipe
    def set_finger(self, fing, deg, clamp=True):        # [TEP:238-262]
        if clamp and deg > 0 and fing in self.MAX_CURL:
            deg = min(deg, self.MAX_CURL[fing])
        for sg in SEGS:
            n = f"{fing}_{sg}_joint"
            if n not in self.idx:
                continue
            q = axis_angle(self.AXES[n], deg * TAPER[sg])
            if sg == "BASE" and fing in self.ADDUCT:
                q = q @ axis_angle(self.ADDUCT_AXES[fing], self.ADDUCT[fing] * deg / 90.0)
            self.set_basis(n, q)
        return deg

    def solve(self):
        log = self.log
        self.rest_pose()
        # [TEP:135-139] palm frame
        kn = unit(self.head("PINK_BASE_joint") - self.head("INDEX_BASE_joint"))
        midv = unit(self.tip("MIDDLE_F") - self.head("MIDDLE_F_BASE_joint"))
        pn = unit(np.cross(kn, midv))
        self.pn = pn
        OPPOSE_TARGET = self.head("MIDDLE_F_BASE_joint")
        # [TEP:146-150]
        span = np.linalg.norm(self.head("INDEX_BASE_joint") - self.head("PINK_BASE_joint"))
        CM = span / KNUCKLE_SPAN_CM
        self.CM = CM
        log("knuckle span %.5f units = %.1fcm, so 1cm = %.5f units" % (span, KNUCKLE_SPAN_CM, CM))

        def score_axis(jn, tipf, axis, rest, lat, oppose):     # [TEP:197-208]
            self.set_basis(jn, axis_angle(axis, TEST_DEG))
            now = self.tip(tipf)
            self.set_basis(jn, np.eye(3))
            if oppose:
                return float(np.linalg.norm(now - OPPOSE_TARGET))
            d = now - rest
            return float(d @ pn + LATERAL_W * abs(d @ lat))

        def free_axis(jn, tipf, lat, oppose=False, cardinal_only=False):   # [TEP:215-228]
            rest = self.tip(tipf)
            best = None
            for axis in (CARDINALS if cardinal_only else sphere_axes(15)):
                sc = score_axis(jn, tipf, axis, rest, lat, oppose)
                if best is None or sc < best[0]:
                    best = (sc, axis)
            if not cardinal_only:
                for axis in sphere_axes(3, around=best[1], span_deg=15):
                    sc = score_axis(jn, tipf, axis, rest, lat, oppose)
                    if sc < best[0]:
                        best = (sc, axis)
            return best[1], best[0]

        def tip_spread():                                       # [TEP:279-291]
            out = []
            for seg, end in SAMPLES:
                pts = [(self.tail(f, seg) if end == "tail" else self.head(f"{f}_{seg}_joint")) for f in FINGER4]
                out += [float(np.linalg.norm(pts[i + 1] - pts[i])) for i in range(3)]
            return out

        def spread_at_full_curl():                              # [TEP:293-300]
            self.rest_pose()
            for f in FINGER4:
                self.set_finger(f, 90)
            g = tip_spread()
            self.rest_pose()
            return g

        # [TEP:318-338] solve every joint both ways
        free_ax, card_ax, thumb_free, thumb_card = {}, {}, {}, {}
        log("=== solving flexion axes ===")
        for f in FINGERS:
            base = self.head(f"{f}_BASE_joint")
            fdir = unit(self.tip(f) - base)
            lat = unit(np.cross(fdir, pn))
            opp = (f == "THUMB")
            for sg in SEGS:
                jn = f"{f}_{sg}_joint"
                a_free, _ = free_axis(jn, f, lat, oppose=opp)
                a_card, _ = free_axis(jn, f, lat, oppose=opp, cardinal_only=True)
                (thumb_free if opp else free_ax)[jn] = a_free
                (thumb_card if opp else card_ax)[jn] = a_card

        def pinch_reach(thumb_axes):                            # [TEP:340-353]
            self.AXES.update(thumb_axes)
            best = None
            for idx in range(0, 95, 5):
                for th in range(0, 95, 5):
                    self.set_finger("INDEX", idx)
                    self.set_finger("THUMB", th)
                    d = float(np.linalg.norm(self.tip("INDEX") - self.tip("THUMB")))
                    if best is None or d < best:
                        best = d
            self.rest_pose()
            return best

        # [TEP:355-378]
        self.AXES.update(free_ax); self.AXES.update(thumb_free)
        g_free = spread_at_full_curl()
        self.AXES.update(card_ax)
        g_card = spread_at_full_curl()
        log("  fingertip splay, free axes      %.5f %.5f %.5f  total %.5f" % (g_free[0], g_free[1], g_free[2], sum(g_free)))
        log("  fingertip splay, cardinal axes  %.5f %.5f %.5f  total %.5f" % (g_card[0], g_card[1], g_card[2], sum(g_card)))
        if sum(g_free) <= sum(g_card):
            self.AXES.update(free_ax); log("  -> fingers: FREE axes (tighter fist)")
            self.finger_axes_kind = "free"
        else:
            self.AXES.update(card_ax); log("  -> fingers: CARDINAL axes (tighter fist)")
            self.finger_axes_kind = "cardinal"
        self.AXES.update(free_ax if sum(g_free) <= sum(g_card) else card_ax)
        r_free = pinch_reach(thumb_free)
        r_card = pinch_reach(thumb_card)
        log("  thumb reach, free axes     %.5f" % r_free)
        log("  thumb reach, cardinal axes %.5f" % r_card)
        if r_free <= r_card:
            self.AXES.update(thumb_free); log("  -> thumb: FREE axes (closer pinch)")
            self.thumb_axes_kind = "free"
        else:
            self.AXES.update(thumb_card); log("  -> thumb: CARDINAL axes (closer pinch)")
            self.thumb_axes_kind = "cardinal"
        self.rest_pose()

        # [TEP:400-410] fold check (diagnostic)
        log("=== fold check: tip travel at 90 degrees ===")
        self.fold = {}
        for f in FINGER4 + ("THUMB",):
            self.rest_pose()
            a = self.tip(f)
            self.set_finger(f, 90)
            b = self.tip(f)
            self.rest_pose()
            self.fold[f] = (float(np.linalg.norm(a - b) / CM), float(((a - b) @ pn) / CM))
            log("  %-10s travels %5.2fcm, %+5.2fcm of it palmward" % (f, *self.fold[f]))

        # [TEP:412-441] adduction axes
        log("=== solving knuckle adduction ===")
        for f in FINGER4:
            jn = f"{f}_BASE_joint"
            base = self.head(jn)
            fdir = unit(self.tip(f) - base)
            lat = unit(np.cross(fdir, pn))
            rest = self.tip(f)
            best = None
            for axis in sphere_axes(10):
                self.set_basis(jn, axis_angle(axis, 30.0))
                d = self.tip(f) - rest
                self.set_basis(jn, np.eye(3))
                sc = -abs(d @ lat) + abs(d @ pn)
                if best is None or sc < best[0]:
                    best = (sc, axis, float(d @ lat))
            toward = 1.0 if f in ("INDEX",) else -1.0
            if f == "MIDDLE_F":
                toward = 0.0
            ax = best[1]
            if best[2] * toward < 0:
                ax = -ax
            self.ADDUCT_AXES[f] = ax
            self.ADDUCT[f] = 0.0
        self.rest_pose()

        def total_spread():                                     # [TEP:443-450]
            self.rest_pose()
            for f in FINGER4:
                self.set_finger(f, 90)
            g = tip_spread()
            self.rest_pose()
            return sum(g), g

        KNUCKLE_GAPS = []                                       # [TEP:463-468]
        for a, b in (("INDEX", "MIDDLE_F"), ("MIDDLE_F", "RING"), ("RING", "PINK")):
            KNUCKLE_GAPS.append(float(np.linalg.norm(self.head(f"{a}_BASE_joint") - self.head(f"{b}_BASE_joint"))))
        log("  knuckle spacing (the target)     %.5f %.5f %.5f" % tuple(KNUCKLE_GAPS))

        def spread_error(gaps):                                 # [TEP:470-482]
            err = 0.0
            for si in range(len(SAMPLES)):
                for i in range(3):
                    d = gaps[si * 3 + i] - KNUCKLE_GAPS[i]
                    err += (-d * 3.0) if d < 0.0 else d
            return err

        base_total, base_gaps = total_spread()
        log("  before error %.5f" % spread_error(base_gaps))
        for _ in range(3):                                      # [TEP:495-510]
            for f in FINGER4:
                if f == "MIDDLE_F":
                    continue
                keep = self.ADDUCT[f]
                best = (spread_error(total_spread()[1]), keep)
                for deg in range(-16, 17, 2):
                    self.ADDUCT[f] = float(deg)
                    t = spread_error(total_spread()[1])
                    if t < best[0]:
                        best = (t, float(deg))
                self.ADDUCT[f] = best[1]
        fin_total, fin_gaps = total_spread()
        for f in FINGER4:
            log("  %-10s adduct %+5.1f deg at full curl" % (f, self.ADDUCT[f]))
        be, fe = spread_error(base_gaps), spread_error(fin_gaps)
        log("  after  error %.5f   (%+.1f%%)" % (fe, 100.0 * (be - fe) / be if be else 0.0))
        self.rest_pose()

        # [TEP:520-533] pinch
        best = None
        for idx in range(0, 95, 5):
            for th in range(0, 95, 5):
                self.set_finger("INDEX", idx)
                self.set_finger("THUMB", th)
                d = float(np.linalg.norm(self.tip("INDEX") - self.tip("THUMB")))
                if best is None or d < best[0]:
                    best = (d, idx, th)
        self.set_finger("INDEX", 0); self.set_finger("THUMB", 0)
        gap, pinch_idx, pinch_th = best
        self.poses = [(n, list(c)) for n, c in POSES]
        self.poses[4] = ("pinch", [pinch_idx, 85, 90, 90, pinch_th])
        self.pinch_gap_cm = gap / CM
        log("pinch solved: index %d thumb %d, gap %.5f (%.2fcm)" % (pinch_idx, pinch_th, gap, gap / CM))
        self.rest_pose()

        # [TEP:553-659] how far each finger may close
        PALM_A = self.head("HANDPALM_joint")
        PALM_B = self.head("MIDDLE_F_BASE_joint")

        def tip_height(f):
            tp = self.tip(f)
            ab = PALM_B - PALM_A
            t = max(0.0, min(1.0, float((tp - PALM_A) @ ab / (ab @ ab))))
            return float(np.linalg.norm(tp - (PALM_A + ab * t)))

        FINGER_RADIUS = KNUCKLE_GAPS[0] * 0.5
        CLEARANCE = FINGER_RADIUS * PALM_CLEAR_RADII
        log("=== how far each finger may close ===  fingertips stop %.2fcm from the palm centre line" % (CLEARANCE / CM))
        for f in FINGER4:
            self.rest_pose()
            limit = 110
            for deg in range(0, 115, 5):
                self.set_finger(f, deg)
                if tip_height(f) < CLEARANCE:
                    limit = max(0, deg - 5)
                    break
            self.rest_pose()
            self.set_finger(f, limit)
            h = tip_height(f)
            self.rest_pose()
            self.MAX_CURL[f] = limit
            note = "" if limit >= 110 else "   <- cannot reach a full fist"
            log("  %-10s ceiling %3d deg, tip %5.2fcm above the palm%s" % (f, limit, h / CM, note))
        self.rest_pose()

        # [TEP:689-777] hold solvers
        def tip_gap():
            return float(np.linalg.norm(self.tip("INDEX") - self.tip("THUMB")))

        def solve_pinch(diam_cm, others):
            want = diam_cm * CM
            for fi, f in enumerate(["MIDDLE_F", "RING", "PINK"]):
                self.set_finger(f, others[fi], clamp=False)
            best = None
            for idx in range(0, 95, 5):
                for th in range(0, 95, 5):
                    self.set_finger("INDEX", idx, clamp=False)
                    self.set_finger("THUMB", th, clamp=False)
                    err = abs(tip_gap() - want)
                    if best is None or (err, -(idx + th)) < (best[0], -(best[1] + best[2])):
                        best = (err, idx, th)
            self.rest_pose()
            _, idx, th = best
            note = "  (rig minimum, target unreachable)" if best[0] / CM > 0.3 else ""
            log("  pinch %scm -> index %d thumb %d  off by %.2fcm%s" % (diam_cm, idx, th, best[0] / CM, note))
            return [idx, others[0], others[1], others[2], th]

        def wrap_gap():
            return float(np.linalg.norm(self.tip("INDEX") - self.head(PALM_CONTACT)))

        def solve_wrap(diam_cm):
            want = diam_cm * CM
            best = None
            for c in range(0, 100, 5):
                for f in FINGER4:
                    self.set_finger(f, c, clamp=False)
                err = abs(wrap_gap() - want)
                if best is None or err < best[0]:
                    best = (err, c)
            _, c = best
            for f in FINGER4:
                self.set_finger(f, c, clamp=False)
            tbest = None
            for th in range(0, 70, 5):
                self.set_finger("THUMB", th, clamp=False)
                err = abs(tip_gap() - want)
                if tbest is None or err < tbest[0]:
                    tbest = (err, th)
            _, th = tbest
            self.rest_pose()
            note = "  (rig minimum, target unreachable)" if best[0] / CM > 0.5 else ""
            log("  wrap  %scm -> fingers %d thumb %d  off by %.2fcm palm, %.2fcm thumb%s"
                % (diam_cm, c, th, best[0] / CM, tbest[0] / CM, note))
            return [c, c, c, c, th]

        log("=== solving interaction poses ===")
        HOLD = [                                                 # [TEP:1078-1127]
            ("hold_round", solve_pinch(1.0, [80, 80, 80])),
            ("hold_shell", solve_pinch(2.0, [55, 50, 45])),
            ("insert", None),
            ("hold_slide", solve_pinch(2.6, [40, 35, 30])),
            ("hold_mag", solve_wrap(2.4)),
            ("hold_foregrip", solve_wrap(3.2)),
            ("hold_forend", solve_wrap(4.5)),
            ("reach", [-12, -12, -12, -12, -15]),
            ("support", [70, 75, 75, 75, 8]),
        ]
        _shell = HOLD[1][1]                                       # [TEP:1128-1129]
        HOLD[2] = ("insert", [_shell[0], _shell[1], _shell[2], _shell[3], _shell[4] + 18])
        _mag = HOLD[4][1]                                         # [TEP:1137-1138]
        HOLD[4] = ("hold_mag", [_mag[0], _mag[1], _mag[2], _mag[3], 12])
        self.holds = HOLD
        self.rest_pose()

    # ---------------------------------------------------------------- write-out helpers
    def pose_worlds(self, curls, clamp=True):
        """[TEP:1203-1210] / [TEP:1231-1246]: rest, then every finger set; returns world matrices
        and the curl each finger actually got (after the ceiling)."""
        self.rest_pose()
        got = []
        for fi, fing in enumerate(FINGERS):
            got.append(self.set_finger(fing, curls[fi], clamp=clamp))
        W = self.worlds()
        basis = [b.copy() for b in self.basis]
        self.rest_pose()
        return W, got, basis
