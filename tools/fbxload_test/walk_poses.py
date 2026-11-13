#!/usr/bin/env python3
"""THE TAKE WALKED, both hands, every frame: which authored frame of each hand's own take is each controller state.

    python3 -I walk_poses.py TAKE.json OUT_DIR

TAKE.json is dump_take's (the engine's loader: fbx_skinned.cpp + ufbx). Each frame's joints are composed and taken back
into the hand's REST placement (Root_joint's rest . Root_joint(frame)^-1), so the hand's own motion through the take
is removed and frames compare (the engine draws the hand the same way: the controller owns the wrist).

Per frame, per finger (all lengths / the hand length L = wrist to the middle fingertip joint at rest):
  straight  tip-to-knuckle / chain length (1 = a straight finger, whatever its angle to the palm)
  ext       the same along the hand's finger axis f (wrist -> middle knuckle): 1 = straight AND in line with the palm
  dpalm     the fingertip (UP_TOP) to HANDPALM_joint's rest place
  side      that offset along the palm normal n (the side the fingers fold to)
  tf tk tn  the fingertip relative to HANDPALM_joint's rest place along f, k (pinky -> index knuckle) and n
  jang      each joint's local rotation away from frame 0 (deg)
and the palm joint's local rotation away from frame 0.

Writes OUT_DIR/walk_<hand>.csv (every frame) and OUT_DIR/holds_<hand>.txt (the take's held poses: runs of frames where
no finger joint turns more than 0.05 deg a frame), and prints the state search (see STATES below).

The engine's table (vr_handpose_math.h kPoseSources) is then read and checked against the walk: every source frame
is what its role needs (CHECKS below), and the extended finger / raised thumb is the take's best held one or within a
stated margin of it. With --sheet TEXTURE, OUT_DIR/poses_<hand>.png: the six states as the table composes them, the
owner's frames and the close candidates, each drawn with the wrist at rest.

    python3 -I walk_poses.py [--sheet TEXTURE.png] [--owner fbx_poses.json] TAKE.json OUT_DIR vr_handpose_math.h

With --owner (UZDXREMA tools/avatar/fbx_poses.json) the engine's held-object / weapon-grip table (kOwnerPoses) is checked
too (check_hold_table): every entry equal to the owner's file, each hand's own frames measured, a pose one hand authored
shown to be absent from the other hand's take (its nearest frame there is further from it than the two hands' shared
frames are from each other), and the palm rule.
"""
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))	# render.py beside this file (-I drops the script's dir)

FINGERS = ["thumb", "index", "middle", "ring", "pinky"]
JN = {
    "thumb": ["THUMB_BASE_joint", "THUMB_MID_joint", "THUMB_TOP_joint", "THUMB_UP_TOP_joint"],
    "index": ["INDEX_BASE_joint", "INDEX_MID_joint", "INDEX_TOP_joint", "INDEX_UP_TOP_joint"],
    "middle": ["MIDDLE_F_BASE_joint", "MIDDLE_F_MID_joint", "MIDDLE_F_TOP_joint", "MIDDLE_F_UP_TOP_joint"],
    "ring": ["RING_BASE_joint", "RING_MID_joint", "RING_TOP_joint", "RING_UP_TOP_joint"],
    "pinky": ["PINK_BASE_joint", "PINK_MID_joint", "PINK_TOP_joint", "PINK_UP_TOP_joint"],
}


def trs_mat(a):
    t, q, s = a[0:3], a[3:7], a[7:10]
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w)
    x, y, z, w = x / n, y / n, z / n, w / n
    R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    M = np.eye(4)
    M[:3, :3] = R * np.array(s)[None, :]
    M[:3, 3] = t
    return M


def compose(parents, locs):
    W = [None] * len(parents)
    for j, p in enumerate(parents):
        M = trs_mat(locs[j])
        W[j] = M if p < 0 else W[p] @ M
    return W


def qang(a, b):
    d = abs(sum(a[k] * b[k] for k in range(4))) / (math.sqrt(sum(x * x for x in a)) * math.sqrt(sum(x * x for x in b)))
    return 2 * math.degrees(math.acos(min(1.0, d)))


class Hand:
    def __init__(self, name, d, right):
        self.name, self.right = name, right
        self.names, self.parents = d["names"], d["parents"]
        self.idx = {n: i for i, n in enumerate(self.names)}
        self.rest = d["rest"]
        self.frames = d["frames"]
        self.n = len(self.frames)
        self.Wrest = compose(self.parents, self.rest)
        P = lambda nm: self.Wrest[self.idx[nm]][:3, 3]
        self.root = self.idx["Root_joint"]
        self.palm = self.idx["HANDPALM_joint"]
        self.palmC = P("HANDPALM_joint")
        f = P("MIDDLE_F_BASE_joint") - P("Root_joint")
        self.f = f / np.linalg.norm(f)
        kr = P("INDEX_BASE_joint") - P("PINK_BASE_joint")
        k = kr - self.f * (kr @ self.f)
        self.k = k / np.linalg.norm(k)
        n = np.cross(self.k, self.f) if right else np.cross(self.f, self.k)
        self.nrm = n / np.linalg.norm(n)
        self.L = np.linalg.norm(P("MIDDLE_F_UP_TOP_joint") - P("Root_joint"))
        self._W = {}
        self.verts = np.array(d["verts"], dtype=np.float64) if "verts" in d else None
        self.tris = np.array(d["tris"], dtype=np.int64).reshape(-1, 3) if "tris" in d else None

    def world(self, fr):
        """joint globals of frame fr, the hand's own motion removed (rest placement of Root_joint)."""
        if fr not in self._W:
            W = compose(self.parents, self.frames[fr])
            back = self.Wrest[self.root] @ np.linalg.inv(W[self.root])
            self._W[fr] = [back @ m for m in W]
        return self._W[fr]

    def world_of(self, locs):
        W = compose(self.parents, locs)
        back = self.Wrest[self.root] @ np.linalg.inv(W[self.root])
        return [back @ m for m in W]

    def finger_metrics(self, W, fn):
        js = [self.idx[n] for n in JN[fn]]
        p = [W[j][:3, 3] for j in js]
        chain = sum(np.linalg.norm(p[i + 1] - p[i]) for i in range(3))
        tip = p[3] - self.palmC
        return dict(
            straight=np.linalg.norm(p[3] - p[0]) / chain,
            ext=((p[3] - p[0]) @ self.f) / chain,
            dpalm=np.linalg.norm(tip) / self.L,
            side=(tip @ self.nrm) / self.L,
            tf=(tip @ self.f) / self.L, tk=(tip @ self.k) / self.L, tn=(tip @ self.nrm) / self.L,
            tip=p[3],
        )

    def jang(self, fr, j, ref=0):
        return qang(self.frames[fr][j][3:7], self.frames[ref][j][3:7])


def main():
    args = sys.argv[1:]
    tex = None
    owner = None
    while args and args[0] in ("--sheet", "--owner"):
        if args[0] == "--sheet":
            tex = args[1]
        else:
            owner = args[1]
        args = args[2:]
    take, out, header = args[0], args[1], args[2]
    d = json.load(open(take))
    hands = [Hand("Hand_Low", d["Hand_Low"], True), Hand("Hand_Low1", d["Hand_Low1"], False)]
    write_csv(hands, out)
    search(hands, out)
    fails = check_table(hands, header)
    if owner:
        fails += check_hold_table(hands, header, owner)
    if tex:
        for h in hands:
            contact_sheet(h, read_table(header)[h.name], tex, "%s/poses_%s.png" % (out, h.name))
    print("\n%s: %d failure(s)" % ("FAIL" if fails else "PASS", fails))
    sys.exit(1 if fails else 0)


def write_csv(hands, out):
    for h in hands:
        rows = []
        for fr in range(h.n):
            W = h.world(fr)
            row = {"frame": fr, "palm": h.jang(fr, h.palm)}
            for fn in FINGERS:
                m = h.finger_metrics(W, fn)
                for key in ("straight", "ext", "dpalm", "side", "tf", "tk", "tn"):
                    row["%s_%s" % (fn, key)] = m[key]
                for i, jn in enumerate(JN[fn][:3]):
                    row["%s_j%d" % (fn, i + 1)] = h.jang(fr, h.idx[jn])
            # motion: the largest finger-joint turn since the previous frame
            if fr > 0:
                row["motion"] = max(h.jang(fr, h.idx[jn], fr - 1) for fn in FINGERS for jn in JN[fn][:3] + ["HANDPALM_joint"])
            else:
                row["motion"] = 0.0
            rows.append(row)
        keys = list(rows[0].keys())
        with open("%s/walk_%s.csv" % (out, h.name), "w") as f:
            f.write(",".join(keys) + "\n")
            for r in rows:
                f.write(",".join("%.5f" % r[k] if isinstance(r[k], float) else str(r[k]) for k in keys) + "\n")
        print("wrote %s/walk_%s.csv (%d frames)" % (out, h.name, h.n))


# ---------------------------------------------------------------------------------------------------------------------
# THE STATES. Thresholds name what a finger IS in a frame; they choose nothing by themselves -- every choice below is a
# ranking over the whole take, printed with its numbers.
#   extended finger (index..pinky): straight >= 0.97 and ext >= 0.85      closed finger: ext < 0 (tip behind the knuckle)
#   thumb closed: its tip on the pinky side of the index knuckle (tk_ix < 0), across the fingers
#   thumb up: straight >= 0.95 and its direction (base -> tip) along k (pinky -> index knuckle) >= 0.5: out of the
#             top of a closed fist
#   thumb out (finger gun): straight >= 0.95 and its tip beyond the index knuckle along k (tk_ix > 0.1)
# ---------------------------------------------------------------------------------------------------------------------

STRAIGHT, EXT = 0.97, 0.85


def thumb_geo(h, W):
    b = W[h.idx["THUMB_BASE_joint"]][:3, 3]
    t = W[h.idx["THUMB_UP_TOP_joint"]][:3, 3]
    v = (t - b) / np.linalg.norm(t - b)
    ib = h.Wrest[h.idx["INDEX_BASE_joint"]][:3, 3]
    rel = (t - ib) / h.L
    return dict(dir_f=v @ h.f, dir_k=v @ h.k, dir_n=v @ h.nrm, tk_ix=rel @ h.k, tn_ix=rel @ h.nrm)


def classify(h, W):
    m = {fn: h.finger_metrics(W, fn) for fn in FINGERS}
    tg = thumb_geo(h, W)
    c = {}
    for fn in FINGERS[1:]:
        c[fn] = "ext" if (m[fn]["straight"] >= STRAIGHT and m[fn]["ext"] >= EXT) else ("closed" if m[fn]["ext"] < 0 else "-")
    t = []
    if tg["tk_ix"] < 0:
        t.append("closed")
    if m["thumb"]["straight"] >= 0.95 and tg["dir_k"] >= 0.5:
        t.append("up")
    if m["thumb"]["straight"] >= 0.95 and tg["tk_ix"] > 0.1:
        t.append("out")
    c["thumb"] = t
    return c, m, tg


STATE_RULES = {
    "open": lambda c: all(c[f] == "ext" for f in FINGERS[1:]) and "closed" not in c["thumb"],
    "fist": lambda c: all(c[f] == "closed" for f in FINGERS[1:]) and "closed" in c["thumb"],
    "point": lambda c: c["index"] == "ext" and all(c[f] == "closed" for f in FINGERS[2:]) and "closed" in c["thumb"],
    "thumbup": lambda c: all(c[f] == "closed" for f in FINGERS[1:]) and "up" in c["thumb"],
    "fingergun": lambda c: c["index"] == "ext" and all(c[f] == "closed" for f in FINGERS[2:]) and "out" in c["thumb"],
    "trigger": lambda c: c["index"] == "closed" and all(c[f] == "ext" for f in FINGERS[2:]),
}

# the owner's table (UZDXREMA tools/avatar/fbx_poses.json), Blender numbers: take frame = number - 1
OWNER = {
    "Hand_Low": {"open": 1, "relaxed": 154, "halfcurl": 193, "curl": 206, "fist": 319, "crossed": 304, "twofinger_point": 762,
                 "fingergun": 803, "twohand_gun": 985},
    "Hand_Low1": {"open": 1, "relaxed": 154, "halfcurl": 193, "curl": 206, "fist": 319, "crossed": 304, "wrap_half": 820,
                  "wrap_tight": 837, "cup": 985, "thumb_out": 944, "twohand_support": 985},
}


def locals_for(h, src):
    """A hand's locals: every joint at rest, each finger in `src` (finger -> take frame) taken WHOLE (its three joints and
    the tip) from that frame, HANDPALM_joint from src['palm'] (None: rest). Root_joint stays at rest: the hand's own
    motion through the take is not part of a pose."""
    locs = [list(x) for x in h.rest]
    for fn in FINGERS:
        fr = src.get(fn)
        if fr is None:
            continue
        for jn in JN[fn]:
            j = h.idx[jn]
            locs[j] = list(h.frames[fr][j])
    if src.get("palm") is not None:
        locs[h.palm] = list(h.frames[src["palm"]][h.palm])
    return locs


def whole(fr):
    return {"thumb": fr, "index": fr, "middle": fr, "ring": fr, "pinky": fr, "palm": fr}


def search(hands, out):
    report = {}
    for h in hands:
        nm = h.name
        own = OWNER[nm]
        print("\n==================== %s (%s hand), %d frames at %.0f fps; L (wrist to middle tip) %.4f file units" % (
            nm, "RIGHT" if h.right else "LEFT", h.n, 60, h.L))
        dr = max(abs(a - b) for j in range(len(h.rest)) for a, b in zip(h.rest[j], h.frames[0][j]))
        print("  frame 0 vs the rest pose: largest TRS difference %.3g (frame 0 IS the rest the model draws)" % dr)
        # 1. whole authored frames that are each state
        cls = [classify(h, h.world(fr)) for fr in range(h.n)]
        print("  1. WHOLE FRAMES of the take that are each state (rules above):")
        found = {}
        for st, rule in STATE_RULES.items():
            frs = [fr for fr in range(h.n) if rule(cls[fr][0])]
            found[st] = frs
            runs = []
            for fr in frs:
                if runs and fr == runs[-1][1] + 1:
                    runs[-1][1] = fr
                else:
                    runs.append([fr, fr])
            print("     %-9s %4d frames %s" % (st, len(frs), " ".join("%d-%d" % (a, b) if a != b else "%d" % a for a, b in runs[:12])))
        # what each finger is in the owner's frames
        print("  the owner's frames (take = Blender - 1), what each finger is (index..pinky ext/closed/-; thumb closed/up/out):")
        for k, b in own.items():
            c, m, tg = cls[b - 1]
            print("     %-16s take %4d: index %-6s middle %-6s ring %-6s pinky %-6s thumb %-12s palm %5.1f deg" % (
                k, b - 1, c["index"], c["middle"], c["ring"], c["pinky"], ",".join(c["thumb"]) or "-", h.jang(b - 1, h.palm)))

        # 2. the fist: least sum of the four fingertips to the palm, thumb closed
        def fist_score(fr):
            m = cls[fr][1]
            return sum(m[f]["dpalm"] for f in FINGERS[1:])
        fist_frames = found["fist"]
        best = min(fist_frames, key=fist_score)
        print("  2. FIST: among the %d fist frames, least sum of index..pinky tip-to-palm: frame %d (%.4f, motion %.2f deg/frame); "
              "owner's take %d: %.4f (+%.1f%%), motion %.2f; the held fist 360: %.4f" % (
                  len(fist_frames), best, fist_score(best), h_motion(h, best), own["fist"] - 1, fist_score(own["fist"] - 1),
                  100 * (fist_score(own["fist"] - 1) / fist_score(best) - 1), h_motion(h, own["fist"] - 1), fist_score(360)))
        # 3. open
        def open_score(fr):
            m = cls[fr][1]
            return min(m[f]["ext"] for f in FINGERS[1:])
        ob = max(found["open"], key=open_score) if found["open"] else None
        print("  3. OPEN: %d open frames; the most open (the least open finger's ext) frame %s (%.4f, palm %.1f deg); owner's take 0: %.4f "
              "(and frame 0 is the rest)" % (len(found["open"]), ob, open_score(ob), h.jang(ob, h.palm), open_score(0)))

        # 4. per-finger sources for the compositions, each finger on the OPEN palm (the palm is not driven where a finger is
        # straight -- section 5), its own three joints and tip from one frame
        idx_rank, thumb_rank = [], []
        for fr in range(h.n):
            W = h.world_of(locals_for(h, {"index": fr, "thumb": fr}))
            mi = h.finger_metrics(W, "index")
            mt = h.finger_metrics(W, "thumb")
            tg = thumb_geo(h, W)
            idx_rank.append((min(mi["straight"], mi["ext"]), fr, mi["straight"], mi["ext"]))
            ang = math.degrees(math.acos(max(-1.0, min(1.0, tg["dir_f"])))) if (mt["straight"] >= 0.95 and tg["tk_ix"] > 0) else -1.0
            thumb_rank.append((ang, fr, mt["straight"], tg["dir_k"], tg["dir_f"], tg["dir_n"], tg["tk_ix"]))
        idx_rank.sort(reverse=True)
        thumb_rank.sort(reverse=True)
        print("  4a. EXTENDED INDEX, its own joints on the open palm: the straightest-and-in-line (min(straight, ext)), best first:")
        seen = []
        for v, fr, st, ex in idx_rank:
            if any(abs(fr - s) < 8 for s in seen):
                continue
            seen.append(fr)
            print("       take %4d  min %.4f  straight %.4f ext %.4f   index motion %.2f%s" % (fr, v, st, ex, finger_motion(h, "index", fr), "" if held(h, "index", fr) else "  (moving)"))
            if len(seen) >= 6:
                break
        for k in ("fingergun", "twofinger_point", "open"):
            if k in own:
                fr = own[k] - 1
                v = [x for x in idx_rank if x[1] == fr][0]
                print("       owner's %-15s take %4d: min %.4f straight %.4f ext %.4f (rank %d of %d)" % (k, fr, v[0], v[2], v[3], idx_rank.index(v) + 1, h.n))
        print("  4b. THUMB UP, its own joints on the open palm: the angle off the finger axis f (straight >= 0.95, tip not across the fingers), best first:")
        seen = []
        for v, fr, st, dk, df, dn, tk in thumb_rank:
            if any(abs(fr - s) < 8 for s in seen):
                continue
            seen.append(fr)
            print("       take %4d  %5.1f deg off f  dir k %+.3f f %+.3f n %+.3f  straight %.4f  tip beyond the index knuckle %+.3f  thumb motion %.2f%s" % (
                fr, v, dk, df, dn, st, tk, finger_motion(h, "thumb", fr), "" if held(h, "thumb", fr) else "  (moving)"))
            if len(seen) >= 6:
                break
        for k in ("fingergun", "twohand_gun", "thumb_out", "curl", "relaxed", "open"):
            if k in own:
                fr = own[k] - 1
                v = [x for x in thumb_rank if x[1] == fr][0]
                print("       owner's %-15s take %4d: %5.1f deg off f, dir k %+.3f f %+.3f n %+.3f straight %.4f tip beyond index %+.3f, thumb motion %.2f (rank %d)" % (
                    k, fr, v[0], v[3], v[4], v[5], v[2], v[6], finger_motion(h, "thumb", fr), thumb_rank.index(v) + 1))
        report[nm] = dict(idx_rank=idx_rank, thumb_rank=thumb_rank, cls=cls, found=found)
    return report


def skin(h, locs):
    """the mesh posed by `locs` (the hand's own motion removed), as the engine skins it: 4 byte weights through the
    palette W(posed) . W(rest)^-1. Returns P, N, UV, T for render.py."""
    W = h.world_of(locs)
    pal = np.stack([W[j] @ np.linalg.inv(h.Wrest[j]) for j in range(len(W))])
    V = h.verts
    pos = np.c_[V[:, 0:3], np.ones(len(V))]
    P = np.zeros((len(V), 3))
    N = np.zeros((len(V), 3))
    for k in range(4):
        b = V[:, 8 + k].astype(int)
        w = V[:, 12 + k] / 255.0
        M = pal[b]
        P += w[:, None] * np.einsum("nij,nj->ni", M[:, :3, :], pos)
        N += w[:, None] * np.einsum("nij,nj->ni", M[:, :3, :3], V[:, 3:6])
    return P, N, V[:, 6:8], h.tris


def sheet(h, entries, texpath, out, cols=2):
    """one tile pair (front, back) per entry, every tile framed alike (the rest hand's extent, padded), labelled."""
    from PIL import Image, ImageDraw
    import render as R
    tex = np.array(Image.open(texpath).convert("RGB"))
    P0, _, _, _ = skin(h, [list(x) for x in h.rest])
    pad = 0.25 * (P0[:, 2].max() - P0[:, 2].min())
    b = (P0[:, 0].min() - pad, P0[:, 0].max() + pad, P0[:, 2].min() - pad, P0[:, 2].max() + pad)
    tiles = []
    for label, locs in entries:
        P, N, UV, T = skin(h, locs)
        im = [R.render(P, N, UV, T, tex, +1, bounds=b), R.render(P, N, UV, T, tex, -1, bounds=b)]
        tiles.append((label, im))
    TW = R.TILE
    rows = (len(tiles) + cols - 1) // cols
    img = Image.new("RGB", (cols * 2 * TW, rows * (TW + 30)), (0, 0, 0))
    d = ImageDraw.Draw(img)
    for i, (label, im) in enumerate(tiles):
        x, y = (i % cols) * 2 * TW, (i // cols) * (TW + 30)
        for k in range(2):
            img.paste(Image.fromarray(im[k]), (x + k * TW, y + 30))
        d.text((x + 4, y + 2), "%s" % label, fill=(255, 255, 0))
        d.text((x + 4, y + 15), "front | back", fill=(160, 160, 160))
    img.save(out)
    print("wrote", out)


def h_motion(h, fr):
    if fr <= 0:
        return 0.0
    return max(h.jang(fr, h.idx[jn], fr - 1) for fn in FINGERS for jn in JN[fn][:3] + ["HANDPALM_joint"])


# ---------------------------------------------------------------------------------------------------------------------
# THE ENGINE'S TABLE, read from vr_handpose_math.h, checked against the walk
# ---------------------------------------------------------------------------------------------------------------------

POSES = ["fist", "point", "thumbup", "fingergun"]


def read_table(header):
    import re
    txt = open(header).read()
    blk = txt[txt.index("kPoseSources[SIDE_COUNT][POSE_COUNT]"):]
    blk = blk[:blk.index("};")]
    rows = re.findall(r"\{\s*\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\},\s*(-?\d+)\s*\}", blk)
    assert len(rows) == 8, "kPoseSources: expected 8 rows, read %d" % len(rows)
    t = {}
    for hi, name in enumerate(["Hand_Low", "Hand_Low1"]):
        t[name] = {}
        for pi, pn in enumerate(POSES):
            r = [int(x) for x in rows[hi * 4 + pi]]
            t[name][pn] = {"thumb": r[0], "index": r[1], "middle": r[2], "ring": r[3], "pinky": r[4], "palm": r[5] if r[5] >= 0 else None}
    return t


def states_of(tab):
    """the six controller states at full input as compositions (finger -> frame, palm -> frame or None)"""
    st = {"open": whole(0), "trigger": {"thumb": 0, "index": tab["fist"]["index"], "middle": 0, "ring": 0, "pinky": 0, "palm": None}}
    st["open"]["palm"] = None
    for p in POSES:
        st[p] = dict(tab[p])
    return st


def finger_motion(h, fn, fr):
    if fr <= 0 or fr >= h.n:
        return 0.0
    return max(h.jang(fr, h.idx[jn], fr - 1) for jn in JN[fn][:3])


def held(h, fn, fr):
    return finger_motion(h, fn, fr) < 0.05 and finger_motion(h, fn, fr + 1) < 0.05


def on_open_palm(h, fn, fr):
    W = h.world_of(locals_for(h, {fn: fr}))
    return h.finger_metrics(W, fn), thumb_geo(h, W)


def thumb_angle(h, fr):
    m, tg = on_open_palm(h, "thumb", fr)
    ok = m["straight"] >= 0.95 and tg["tk_ix"] > 0
    return (math.degrees(math.acos(max(-1.0, min(1.0, tg["dir_f"])))) if ok else -1.0), m["straight"], tg


def check_table(hands, header):
    tab = read_table(header)
    fails = 0

    def check(ok, msg):
        nonlocal fails
        print("  %s  %s" % ("PASS" if ok else "FAIL", msg))
        if not ok:
            fails += 1

    print("\n== THE ENGINE'S TABLE (%s kPoseSources) against the walk" % header)
    for h in hands:
        t = tab[h.name]
        print(" ", h.name, "(RIGHT)" if h.right else "(LEFT)")
        for p in POSES:
            print("    %-9s thumb %4d index %4d middle %4d ring %4d pinky %4d palm %s" % (p, t[p]["thumb"], t[p]["index"], t[p]["middle"], t[p]["ring"], t[p]["pinky"],
                                                                                    t[p]["palm"] if t[p]["palm"] is not None else "not driven"))
        cls = lambda fr: classify(h, h.world(fr))[0]
        # the fist: a whole fist frame, its palm driven
        f = t["fist"]
        fr = f["index"]
        check(all(f[k] == fr for k in FINGERS) and f["palm"] == fr and STATE_RULES["fist"](cls(fr)),
              "%s fist: every finger and the palm from one frame (%d), and that frame is a fist (index..pinky closed, thumb across)" % (h.name, fr))
        fist = fr
        # the closed fingers of every pose are the fist's, or closed in their own frame
        for p in POSES:
            for fn in ("middle", "ring", "pinky") + (("index",) if p in ("fist", "thumbup") else ()) + (("thumb",) if p in ("fist", "point") else ()):
                src = t[p][fn]
                c = cls(src)
                closed = ("closed" in c["thumb"]) if fn == "thumb" else (c[fn] == "closed")
                check(closed, "%s %s: %s from frame %d is closed there" % (h.name, p, fn, src))
        # the extended index (point, fingergun): held, extended, the straightest held index within 1%
        best = max((min(m["straight"], m["ext"]), fr) for fr in range(h.n) if held(h, "index", fr) for m in [on_open_palm(h, "index", fr)[0]])
        for p in ("point", "fingergun"):
            src = t[p]["index"]
            m, _ = on_open_palm(h, "index", src)
            v = min(m["straight"], m["ext"])
            check(held(h, "index", src) and m["straight"] >= STRAIGHT and m["ext"] >= EXT and v >= best[0] * 0.99,
                  "%s %s: the index from frame %d is held and extended on the open palm (straight %.4f, ext %.4f; the take's best held index %.4f at %d)" % (
                      h.name, p, src, m["straight"], m["ext"], best[0], best[1]))
        # the raised thumb (thumbup): held, straight, not across the fingers, within 2 degrees of the take's best held
        bestT = max((thumb_angle(h, fr)[0], fr) for fr in range(h.n) if held(h, "thumb", fr))
        src = t["thumbup"]["thumb"]
        a, st, tg = thumb_angle(h, src)
        check(held(h, "thumb", src) and a >= bestT[0] - 2.0,
              "%s thumbup: the thumb from frame %d is held, straight %.3f, %.1f deg off the finger axis (the take's best held thumb %.1f at %d)" % (
                  h.name, src, st, a, bestT[0], bestT[1]))
        # the finger gun's thumb: held, straight, OUT (its tip beyond the index knuckle along the knuckle line)
        src = t["fingergun"]["thumb"]
        a, st, tg = thumb_angle(h, src)
        check(held(h, "thumb", src) and st >= 0.95 and tg["tk_ix"] > 0.1,
              "%s fingergun: the thumb from frame %d is held, straight %.3f, out: its tip %.3f hand lengths beyond the index knuckle (%.1f deg off the finger axis)" % (
                  h.name, src, st, tg["tk_ix"], a))
        # the palm: driven exactly where no finger of the composition is straight
        for p in POSES:
            W = h.world_of(locals_for(h, dict(t[p], palm=None)))
            straight = [fn for fn in FINGERS if h.finger_metrics(W, fn)["straight"] >= (0.95 if fn == "thumb" else STRAIGHT)]
            # what the fist's palm would do to each straight finger
            Wc = h.world_of(locals_for(h, dict(t[p], palm=fist)))
            carry = ", ".join("%s %.1f mm" % (fn, np.linalg.norm(Wc[h.idx[JN[fn][3]]][:3, 3] - W[h.idx[JN[fn][3]]][:3, 3]) * MM) for fn in straight)
            check((t[p]["palm"] is not None) == (len(straight) == 0),
                  "%s %s: palm %s; straight fingers: %s%s" % (h.name, p, "driven" if t[p]["palm"] is not None else "not driven",
                                                             ", ".join(straight) or "none", (" (the fist's palm would carry them: %s)" % carry) if straight else ""))
    return fails


MM = 0.11094212 * 1000  # file units -> mm: the hand's 0.19 m over its wrist-to-fingertip length (vrealms_iwad/MODELDEF.txt)


def contact_sheet(h, tab, tex, out):
    st = states_of(tab)
    own = OWNER[h.name]
    E = []
    for name in ("open", "fist", "point", "thumbup", "fingergun", "trigger"):
        c = st[name]
        lab = "%s: thumb %d index %d middle %d ring %d pinky %d, palm %s" % (name.upper(), c["thumb"], c["index"], c["middle"], c["ring"], c["pinky"],
                                                                           c["palm"] if c["palm"] is not None else "rest")
        E.append((lab, locals_for(h, c)))
    for k, b in own.items():
        if k in ("open", "fist"):
            continue
        E.append(("owner's %s (Blender %d = take %d), whole" % (k, b, b - 1), locals_for(h, whole(b - 1))))
    fist = tab["fist"]["index"]
    if h.right:
        cands = [("point cand.: index 802 (owner's fingergun, mid-fire)", dict(st["point"], index=802)),
                 ("point cand.: index 0 (open)", dict(st["point"], index=0)),
                 ("thumbup cand.: thumb 984 (owner's twohand_gun, twitch)", dict(st["thumbup"], thumb=984)),
                 ("thumbup cand.: thumb 808 (finger gun hold)", dict(st["thumbup"], thumb=808)),
                 ("thumbup cand.: thumb 0 (open)", dict(st["thumbup"], thumb=0)),
                 ("fingergun cand.: 802 whole but middle from fist", dict(st["fingergun"], thumb=802, index=802, ring=802, pinky=802)),
                 ("point with the fist's palm (the swing)", dict(st["point"], palm=fist))]
    else:
        cands = [("thumbup cand.: thumb 943 (owner's thumb_out)", dict(st["thumbup"], thumb=943)),
                 ("thumbup cand.: thumb 259 (best held angle)", dict(st["thumbup"], thumb=259)),
                 ("thumbup cand.: thumb 1136 (cup hold)", dict(st["thumbup"], thumb=1136)),
                 ("thumbup cand.: thumb 0 (open)", dict(st["thumbup"], thumb=0)),
                 ("point cand.: index 186 (straightest, moving)", dict(st["point"], index=186)),
                 ("fingergun cand.: thumb 943", dict(st["fingergun"], thumb=943)),
                 ("point with the fist's palm (the swing)", dict(st["point"], palm=fist))]
    for lab, c in cands:
        E.append((lab, locals_for(h, c)))
    sheet(h, E, tex, out, cols=2)


# ---------------------------------------------------------------------------------------------------------------------
# THE HELD-OBJECT AND WEAPON-GRIP TABLE (vr_handpose_math.h kOwnerPoses) against the owner's fbx_poses.json and the takes
# ---------------------------------------------------------------------------------------------------------------------

def read_owner_table(header):
    import re
    txt = open(header).read()
    blk = txt[txt.index("inline constexpr OwnerPose kOwnerPoses[] ="):]
    blk = blk[:blk.index("};")]
    hands = {"HANDS_BOTH": "both", "HANDS_A": "A", "HANDS_B": "B"}
    out = {}
    order = []
    for line in blk.splitlines():
        m = re.match(r'\s*\{\s*"([^"]+)",\s*(\d+),\s*(HANDS_\w+|0),\s*(\{[^}]*\}|VRHP_ALL\("[^"]+"\)),\s*(nullptr|"[^"]+")\s*\},', line)
        if not m:
            continue
        name, blender, hs, fing, palm = m.groups()
        palm = None if palm == "nullptr" else palm.strip('"')
        if int(blender) > 0:
            out[name] = {"raw": [int(blender), hands[hs]], "palm": palm}
        else:
            a = re.match(r'VRHP_ALL\("([^"]+)"\)', fing)
            fl = [a.group(1)] * 5 if a else re.findall(r'"([^"]+)"', fing)
            assert len(fl) == 5, line
            out[name] = {"fingers": dict(zip(FINGERS, fl)), "palm": palm}
        order.append(name)
    return out, order


def tip_sig(h, fr):
    W = h.world(fr)
    v = []
    for fn in FINGERS:
        m = h.finger_metrics(W, fn)
        v += [m["tf"], m["tk"], m["tn"]]
    return np.array(v)


def check_hold_table(hands, header, owner_path):
    fails = 0

    def check(ok, msg):
        nonlocal fails
        print("  %s  %s" % ("PASS" if ok else "FAIL", msg))
        if not ok:
            fails += 1

    owner = json.load(open(owner_path))
    tab, order = read_owner_table(header)
    print("\n== THE HELD-OBJECT AND WEAPON-GRIP TABLE (%s kOwnerPoses) against %s and both takes" % (header, owner_path))
    want = [k for k in owner if not k.startswith("//")]
    check(order == want, "the table has the owner's %d poses, in his order (%d in the engine)" % (len(want), len(order)))
    same = 0
    for k in want:
        o, t = owner[k], tab.get(k)
        if t is None:
            check(False, "%s is missing from the engine's table" % k)
            continue
        if isinstance(o, list):
            ok = "raw" in t and t["raw"] == [o[0], o[1]]
        else:
            ok = "fingers" in t and all(t["fingers"][fn] == o["fingers"][fn] for fn in FINGERS)
        if ok:
            same += 1
        else:
            check(False, "%s differs: owner %s, engine %s" % (k, o, t))
    check(same == len(want), "every entry equals the owner's (raw: Blender frame and hand; composed: each finger's pose): %d of %d" % (same, len(want)))

    H = {"R": hands[0], "L": hands[1]}
    authored = lambda hs, s: hs == "both" or (hs == "A" and s == "R") or (hs == "B" and s == "L")
    raw = {k: v["raw"] for k, v in tab.items() if "raw" in v}

    # the hands' own frames, measured
    print("  each raw pose in each hand's own take (take = Blender - 1): index..pinky ext/closed/-, thumb closed/up/out, palm, motion deg/frame")
    for k, (b, hs) in raw.items():
        for s in ("R", "L"):
            h = H[s]
            fr = b - 1
            c, m, tg = classify(h, h.world(fr))
            print("     %-20s %s take %4d %-9s: index %-6s middle %-6s ring %-6s pinky %-6s thumb %-10s palm %5.1f motion %.2f" % (
                k, s, fr, "authored" if authored(hs, s) else "NOT ITS", c["index"], c["middle"], c["ring"], c["pinky"],
                ",".join(c["thumb"]) or "-", h.jang(fr, h.palm), h_motion(h, fr)))

    # a pose one hand authored is absent from the other hand's take: its nearest frame there is further from it than the
    # two hands' shared frames are from each other (fingertips in each hand's own palm frame, hand lengths)
    shared = [b - 1 for k, (b, hs) in raw.items() if hs == "both"]
    tol = max(np.linalg.norm(tip_sig(H["R"], fr) - tip_sig(H["L"], fr)) for fr in shared)
    print("  the two hands' shared frames (%s) differ from each other by up to %.3f (fingertips in the hand's own frame, hand lengths): "
          "a frame further than that from a pose is not that pose" % (", ".join(str(x) for x in sorted(set(shared))), tol))
    sigs = {s: [tip_sig(H[s], g) for g in range(H[s].n)] for s in ("R", "L")}
    for k, (b, hs) in raw.items():
        if hs == "both":
            continue
        src = "R" if hs == "A" else "L"
        dst = "L" if src == "R" else "R"
        target = tip_sig(H[src], b - 1)
        d = [(np.linalg.norm(sigs[dst][g] - target), g) for g in range(H[dst].n)]
        d.sort()
        best, g = d[0]
        used = sorted(n for n, v in tab.items() if "fingers" in v and k in v["fingers"].values())
        if not used:
            print("  NOTE  %s (Blender %d, hand %s): no composed pose uses it; the %s take's nearest frame %d is %.3f away (tolerance %.3f), %s -- "
                  "not mirrored, and not substituted" % (k, b, hs, "right" if dst == "R" else "left", g, best, tol,
                                                         "held" if held(H[dst], "index", g) and held(H[dst], "middle", g) else "moving"))
            continue
        check(best > tol, "%s (Blender %d, hand %s): absent from the %s take -- its nearest frame there, %d, is %.3f away (> %.3f), %s" % (
            k, b, hs, "right" if dst == "R" else "left", g, best, tol,
            "held" if held(H[dst], "index", g) and held(H[dst], "middle", g) else "moving") + "; used by " + ", ".join(used))

    # the palm rule (checklist item 38, by its reason): the palm takes the middle finger's frame exactly when every straight
    # finger of the pose comes from that frame (no straight finger rides a palm from another frame)
    def palm_rule(h, src):
        W = h.world_of(locals_for(h, dict(src, palm=None)))
        straight = [fn for fn in FINGERS if h.finger_metrics(W, fn)["straight"] >= (0.95 if fn == "thumb" else STRAIGHT)]
        return all(src[fn] == src["middle"] for fn in straight), straight

    # the same rule gives item 38's table for the controller poses (kPoseSources)
    for hi, nm in enumerate(["Hand_Low", "Hand_Low1"]):
        t = read_table(header)[nm]
        for pn in POSES:
            src = {fn: t[pn][fn] for fn in FINGERS}
            drive, straight = palm_rule(hands[hi], src)
            check(drive == (t[pn]["palm"] is not None) and (not drive or t[pn]["palm"] == src["middle"]),
                  "%s %s (kPoseSources): the rule says palm %s, the table has %s; straight fingers %s" % (
                      nm, pn, ("from %d" % src["middle"]) if drive else "not driven",
                      t[pn]["palm"] if t[pn]["palm"] is not None else "not driven",
                      ", ".join("%s (%d)" % (fn, src[fn]) for fn in straight) or "none"))
    for k in order:
        t = tab[k]
        fing = {fn: k for fn in FINGERS} if "raw" in t else t["fingers"]
        for s in ("R", "L"):
            h = H[s]
            if not all(authored(raw[fing[fn]][1], s) for fn in FINGERS):
                continue
            src = {fn: raw[fing[fn]][0] - 1 for fn in FINGERS}
            drive, straight = palm_rule(h, src)
            driven = t["palm"] is not None
            ok = driven == drive and (not driven or raw[t["palm"]][0] - 1 == src["middle"])
            if not ok or k in ("hold_magazine", "hold_round", "hold_forend", "hold_foregrip", "hold_inserting", "fbx_curl", "gun_grip", "grip_rocket_fire", "pointing"):
                check(ok, "%s %s: palm %s; straight fingers: %s" % (k, s, ("driven from %d" % (raw[t["palm"]][0] - 1)) if driven else "not driven",
                                                                    ", ".join("%s (%d)" % (fn, src[fn]) for fn in straight) or "none"))
    print("  (the palm rule held for every other pose on every hand that has it)")
    return fails


if __name__ == "__main__":
    main()
