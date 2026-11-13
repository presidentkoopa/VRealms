#!/usr/bin/env python3
"""hand_final.fbx -> hand_r.iqm + hand_l.iqm for the Realms game package. Build time; no Blender.

    python3 -I tools/fbx_hands/fbx2iqm.py [--fbx PATH] [--tex PATH] [--out DIR] [--keep-tips] [--tails blender|mesh]

Reads ONLY the FBX (tools/fbx_hands/fbxread.py) and its skin texture. No existing IQM hand is read.

Writes (default --out vrealms_iwad/models/hands):
    hand_r.iqm   right hand: mesh Hand_Low,  skeleton under GLOBAL_MAIN_CONTROL_R,  its own take
    hand_l.iqm   left hand:  mesh Hand_Low1, skeleton under GLOBAL_MAIN_CONTROL_R1, its own take,
                 reflected as data (the FBX makes it a left hand with a -1 X scale on its top control)
    lambert1_Base_Color.png   the FBX's base colour texture, copied byte for byte
and, beside this script: hand_frames.txt (frame constants) and fbx2iqm_build.log (every solved number).

FILE FRAME (the IQM's own coordinates; the engine loads file Z as up: models_iqm.cpp LoadPosition
stores v.x, v.z, v.y and CalculateBones conjugates by swapYZ; Doom's actor-local frame is X forward,
Y left, Z up):
    +X  forward: wrist (Root_joint) towards the middle fingertip, at the bind pose
    +Y  left:    perpendicular to X in the knuckle line; the RIGHT hand's thumb side, the LEFT hand's little-finger side
    +Z  up:      the back of the hand. The palm faces -Z on both hands (fingers curl towards -Z)
    origin       Root_joint (the wrist), at every frame
UNITS: map units. 0.19 m from the wrist to the middle fingertip (mesh) at Realms' 93.5 units/m
(src/maploader/rothmap.cpp:573-578: 34 * (144 + 10) / 56). One model unit is one map unit on the
follow-hand path (src/r_data/models.cpp:375-381, ObjectToWorldMatrix divides VR_UnitsPerMeter out).

FRAMES (RS_Hands' map, hand_frames.txt):
    0-10       the 11 baked poses (handrecipe.py, a port of RS_Hands tools_export_poses.py): an authored
               take frame where the script's picker finds one, the curl table where it does not
    11-1288    FBX Take 001 frames 0-1277, this hand's own curves
    1289-1297  hold_round hold_shell insert hold_slide hold_mag hold_foregrip hold_forend reach support
               (picked or solved the same way)
    1298       reach_claw = row 241 verbatim, 1299 salute = row 5 with MIDDLE_F_* rotations from row 0
               (RS_Hands tools/iqm_append_pose.py)
    Frames 0-10 and 1289-1299 are made ONCE, on the rig that skins mesh Hand_Low (GLOBAL_MAIN_CONTROL_R,
    the armature tools_export_poses.py reads [TEP:75-76]), and BOTH hands take them (the left reflected):
    the owner's set is that one hand, mirrored for the other side. Row 1298 is therefore FBX take frame
    230 of that rig in both files. Frames 11-1288 stay each hand's own take.
Root_joint and HANDPALM_joint (the wrist) hold their bind transform in EVERY frame: the controller owns
the wrist. (The owner's own reach_claw keeps 38 degrees of authored wrist, iqm_append_pose.py:46-55;
here it is zeroed like every other row. Whether the wrist may ever be animated is the owner's open
question.) Only the finger joints move (rotations; the FBX's own small finger-joint translation keys
are kept as authored).

SKIN: each UP_TOP joint's weight is merged into its TOP joint (the take never rotates UP_TOP), then each
vertex keeps its 4 largest weights, renormalised to ubytes summing to 255. --keep-tips skips the merge.
The left hand keeps Hand_Low1's polygons and weights and takes Hand_Low's normals, UVs and tangents
(see build_hand), all reflected with the geometry.
"""
import argparse
import json
import math
import os
import shutil
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from analyze import Scene, skin_data, mesh_of_model  # noqa: E402
from poses import Hand, FRAME  # noqa: E402
import handrecipe as HR  # noqa: E402

DEFAULT_FBX = "/home/claude/dxr/models/hands/hand_final.fbx"
TEXTURE_NAME = "lambert1_Base_Color.png"

# Engine scale. Realms sets VR_UnitsPerMeter from the map's standing height (rothmap.cpp:573-578):
# 34 * standingHeight / 56 with standingHeight = PlayerHeight() + 10. PlayerHeight() is 144 on 41 of
# the 44 retail maps (= 93.5 units/m) and 128 on ABAGATE2, AQUA1 and DOPPLE (= 83.93 units/m).
UNITS_PER_METRE = 34.0 * (144.0 + 10.0) / 56.0
HAND_LENGTH_M = 0.19

TAKE_FRAMES = 1278
SOURCE_BASE = 11
HOLD_BASE = SOURCE_BASE + TAKE_FRAMES          # 1289
HOLD_NAMES = ["hold_round", "hold_shell", "insert", "hold_slide", "hold_mag", "hold_foregrip",
              "hold_forend", "reach", "support", "reach_claw", "salute"]
REACH_CLAW_ROW = 241                           # iqm_append_pose.py:43
SALUTE_BASE_ROW, SALUTE_GRAFT_ROW = 5, 0       # iqm_append_pose.py:58
FIST_AUTHORED = 513                            # render_wired_poses.py:47 (a take frame, FBX 502)

# Joint order: parents first, breadth first (the order tools/fbx2iqm/hand_*.json expects).
JOINT_ORDER = ["Root_joint", "HANDPALM_joint"]
for _s in ("BASE", "MID", "TOP", "UP_TOP"):
    for _f in ("INDEX", "MIDDLE_F", "RING", "PINK", "THUMB"):
        JOINT_ORDER.append(f"{_f}_{_s}_joint")

D_MIRROR = np.diag([1.0, -1.0, 1.0])           # left hand: reflect across the file's XZ plane


# --------------------------------------------------------------------------------------- math
def rot_axis_np(axis, deg):
    a = np.radians(deg)
    c, s = np.cos(a), np.sin(a)
    n = len(a)
    m = np.zeros((n, 3, 3))
    i, j = [(1, 2), (2, 0), (0, 1)][axis]
    m[:, axis, axis] = 1.0
    m[:, i, i] = c; m[:, i, j] = -s
    m[:, j, i] = s; m[:, j, j] = c
    return m


def euler_xyz(e):
    """FBX eEulerXYZ: X applied first, R = Rz @ Ry @ Rx. e: (N,3) degrees."""
    e = np.atleast_2d(np.asarray(e, dtype=np.float64))
    return rot_axis_np(2, e[:, 2]) @ rot_axis_np(1, e[:, 1]) @ rot_axis_np(0, e[:, 0])


def m4(R=None, t=None):
    M = np.eye(4)
    if R is not None:
        M[:3, :3] = R
    if t is not None:
        M[:3, 3] = t
    return M


def local_mats(mp, R, T):
    """FBX SDK local transform for N samples (analyze.py local_matrix, vectorised):
    T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1."""
    if mp["order"] != 0:
        raise SystemExit("rotation order %d not handled" % mp["order"])
    R = np.atleast_2d(R); T = np.atleast_2d(T)
    n = len(R)
    pre = m4(euler_xyz([mp["Pre"]])[0])
    post_inv = m4(euler_xyz([mp["Post"]])[0].T)
    left = m4(t=mp["RotOff"]) @ m4(t=mp["RotPiv"]) @ pre
    right = post_inv @ m4(t=-np.asarray(mp["RotPiv"])) @ m4(t=mp["SclOff"]) @ m4(t=mp["SclPiv"]) \
        @ m4(np.diag(mp["S"])) @ m4(t=-np.asarray(mp["SclPiv"]))
    out = np.zeros((n, 4, 4))
    Rm = euler_xyz(R)
    for k in range(n):
        out[k] = m4(t=T[k]) @ left @ m4(Rm[k]) @ right
    return out


def mat_to_quat(R):
    """3x3 rotation -> (x, y, z, w)."""
    m = R
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        w = 0.25 * s; x = (m[2, 1] - m[1, 2]) / s; y = (m[0, 2] - m[2, 0]) / s; z = (m[1, 0] - m[0, 1]) / s
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        w = (m[2, 1] - m[1, 2]) / s; x = 0.25 * s; y = (m[0, 1] + m[1, 0]) / s; z = (m[0, 2] + m[2, 0]) / s
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        w = (m[0, 2] - m[2, 0]) / s; x = (m[0, 1] + m[1, 0]) / s; y = 0.25 * s; z = (m[1, 2] + m[2, 1]) / s
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        w = (m[1, 0] - m[0, 1]) / s; x = (m[0, 2] + m[2, 0]) / s; y = (m[1, 2] + m[2, 1]) / s; z = 0.25 * s
    q = np.array([x, y, z, w])
    return q / np.linalg.norm(q)


def unit(v):
    v = np.asarray(v, dtype=np.float64)
    return v / np.linalg.norm(v)


# --------------------------------------------------------------------------------------- FBX side
class FbxHand:
    """One of the two rigs in hand_final.fbx, in FBX bind space (unmirrored)."""

    def __init__(self, sc, top_name, log):
        top = [o for o, n in sc.objs.items() if n.name == "Model" and sc.name(o) == top_name]
        if len(top) != 1:
            raise SystemExit("no unique %s" % top_name)
        self.top_name = top_name
        self.h = Hand(sc, top_name, top[0])
        if sorted(self.h.joints) != sorted(JOINT_ORDER):
            raise SystemExit("%s: joints %s" % (top_name, sorted(self.h.joints)))
        self.names = list(JOINT_ORDER)
        ids = [self.h.joints[n] for n in self.names]
        self.parents = []
        for n, j in zip(self.names, ids):
            p = sc.model_parent(j)
            self.parents.append(ids.index(p) if p in ids else -1)
        assert self.parents[0] == -1 and all(p >= 0 for p in self.parents[1:])
        # the mesh skinned to exactly these joints
        self.mesh_name = None
        for mid, n in sc.objs.items():
            if n.name == "Model" and n.props[2] == "Mesh":
                gid = mesh_of_model(sc, mid)
                sd = skin_data(sc, gid)
                if set(sd) == set(ids):
                    self.mesh_name, self.gid, self.sd = sc.name(mid), gid, sd
        if self.mesh_name is None:
            raise SystemExit("%s: no mesh skinned to it" % top_name)
        self.TL = [np.array(self.sd[j][3]) for j in ids]
        tl_tr = max(np.abs(np.array(self.sd[j][3]) @ np.array(self.sd[j][2]) - np.eye(4)).max() for j in ids)
        log("%s: mesh %s; max |TransformLink * Transform - I| = %.2e (mesh bind = identity)" % (top_name, self.mesh_name, tl_tr))
        if tl_tr > 1e-6:
            raise SystemExit("cluster Transform is not TransformLink^-1")
        self.mp = [self.h.mp[j] for j in ids]
        # rest chain against the bind matrices
        Wr = [None] * 22
        for i, p in enumerate(self.parents):
            L = local_mats(self.mp[i], [self.mp[i]["R"]], [self.mp[i]["T"]])[0]
            Wr[i] = self.TL[0] if p < 0 else Wr[p] @ L
        dev = max(np.abs(Wr[i] - self.TL[i]).max() for i in range(22))
        log("  rest (Lcl) chain from Root_joint vs bind matrices: max deviation %.2e" % dev)
        self.ids = ids
        self.sc = sc
        self._read_mesh()

    def _read_mesh(self):
        g = self.sc.objs[self.gid]
        self.cp = np.array(g.find("Vertices").props[0], dtype=np.float64).reshape(-1, 3)
        pvi = list(g.find("PolygonVertexIndex").props[0])
        polys, cur = [], []
        for k, x in enumerate(pvi):
            if x < 0:
                cur.append((~x, k)); polys.append(cur); cur = []
            else:
                cur.append((x, k))
        self.polys = polys
        self.npv = len(pvi)

        def layer(name):
            le = g.find(name)
            return le
        ln = layer("LayerElementNormal")
        assert ln.find("MappingInformationType").props[0] == "ByVertice" and ln.find("ReferenceInformationType").props[0] == "Direct"
        self.cp_normal = np.array(ln.find("Normals").props[0]).reshape(-1, 3)
        lu = layer("LayerElementUV")
        assert lu.find("MappingInformationType").props[0] == "ByPolygonVertex" and lu.find("ReferenceInformationType").props[0] == "IndexToDirect"
        self.uv = np.array(lu.find("UV").props[0]).reshape(-1, 2)
        self.uvidx = np.array(lu.find("UVIndex").props[0])
        lt = layer("LayerElementTangent"); lb = layer("LayerElementBinormal")
        for le in (lt, lb):
            assert le.find("MappingInformationType").props[0] == "ByPolygonVertex" and le.find("ReferenceInformationType").props[0] == "Direct"
        self.pv_tan = np.array(lt.find("Tangents").props[0]).reshape(-1, 3)
        self.pv_bin = np.array(lb.find("Binormals").props[0]).reshape(-1, 3)
        # skin weights per control point, by joint index
        self.weights = [dict() for _ in range(len(self.cp))]
        for ji, j in enumerate(self.ids):
            idx, wts = self.sd[j][0], self.sd[j][1]
            for i, w in zip(idx, wts):
                if w > 0:
                    self.weights[i][ji] = self.weights[i].get(ji, 0.0) + w

    def dominant(self, i):
        d = self.weights[i]
        return max(d, key=lambda k: (d[k], -k))

    def fingertips(self):
        """Per finger: the bind-mesh vertex furthest along TOP->UP_TOP among the vertices whose
        largest weight is that finger's TOP or UP_TOP joint."""
        out = {}
        for f in HR.FINGERS:
            jt, ju = self.names.index(f"{f}_TOP_joint"), self.names.index(f"{f}_UP_TOP_joint")
            pt, pu = self.TL[jt][:3, 3], self.TL[ju][:3, 3]
            d = unit(pu - pt)
            cand = [i for i in range(len(self.cp)) if self.dominant(i) in (jt, ju)]
            best = max(cand, key=lambda i: self.cp[i] @ d)
            out[f] = self.cp[best].copy()
        return out

    def take_worlds(self):
        """Per take frame: 22 world matrices in bind space; Root and HANDPALM at bind (stripped)."""
        n = TAKE_FRAMES
        locs = []
        for i, j in enumerate(self.ids):
            ch = {}
            for pr in ("Lcl Rotation", "Lcl Translation"):
                cv = self.h.curves.get((j, pr))
                if cv is None:
                    ch[pr] = np.tile(self.mp[i]["R" if pr == "Lcl Rotation" else "T"], (n, 1))
                    continue
                cols = []
                for ax in "XYZ":
                    kt, kv, _ = cv[ax]
                    if len(kt) != n or any(kt[k] != k * FRAME for k in range(n)):
                        raise SystemExit("%s %s %s: not baked one key per frame" % (self.names[i], pr, ax))
                    cols.append(np.array(kv, dtype=np.float64))
                ch[pr] = np.stack(cols, 1)
            locs.append(local_mats(self.mp[i], ch["Lcl Rotation"], ch["Lcl Translation"]))
        frames = []
        rest = [self.TL[0]] + [np.linalg.inv(self.TL[p]) @ self.TL[i] for i, p in enumerate(self.parents) if p >= 0]
        for f in range(n):
            W = [None] * 22
            for i, p in enumerate(self.parents):
                if self.names[i] in ("Root_joint", "HANDPALM_joint"):
                    L = rest[i]                       # controller owns these: bind, every frame
                else:
                    L = locs[i][f]
                W[i] = L if p < 0 else W[p] @ L
            frames.append(W)
        return frames, locs


# --------------------------------------------------------------------------------------- IQM writer
IQM_POSITION, IQM_TEXCOORD, IQM_NORMAL, IQM_TANGENT, IQM_BLENDINDEXES, IQM_BLENDWEIGHTS = 0, 1, 2, 3, 4, 5
IQM_UBYTE, IQM_FLOAT = 1, 7


class Text:
    def __init__(self):
        self.buf = bytearray(b"\0")
        self.at = {"": 0}

    def add(self, s):
        if s not in self.at:
            self.at[s] = len(self.buf)
            self.buf += s.encode("ascii") + b"\0"
        return self.at[s]


def write_iqm(path, mesh_name, material, joint_names, parents, bind_locals, frame_locals, verts, tris,
              adjacency, anims, comment):
    """bind_locals: list of (t, q). frame_locals: [frame][joint] (t, q). verts: dict of arrays."""
    nf, nj = len(frame_locals), len(joint_names)
    text = Text()
    mesh_rec = struct.pack("<6I", text.add(mesh_name), text.add(material), 0, len(verts["pos"]), 0, len(tris))
    joints = b""
    for i in range(nj):
        t, q = bind_locals[i]
        joints += struct.pack("<Ii3f4f3f", text.add(joint_names[i]), parents[i], *t, *q, 1.0, 1.0, 1.0)
    # channels: 0-2 translate, 3-6 quaternion, 7-9 scale
    vals = np.zeros((nf, nj, 10))
    for f in range(nf):
        for j in range(nj):
            t, q = frame_locals[f][j]
            vals[f, j, 0:3] = t
            vals[f, j, 3:7] = q
            vals[f, j, 7:10] = 1.0
    poses = b""
    mask_list, offs, scls = [], np.zeros((nj, 10)), np.zeros((nj, 10))
    for j in range(nj):
        mask = 0
        for c in range(10):
            lo, hi = vals[:, j, c].min(), vals[:, j, c].max()
            offs[j, c] = lo
            if hi - lo > 1e-7:
                mask |= 1 << c
                scls[j, c] = (hi - lo) / 65535.0
        mask_list.append(mask)
        poses += struct.pack("<iI10f10f", parents[j], mask, *offs[j], *scls[j])
    nfc = sum(bin(m).count("1") for m in mask_list)
    fd = np.zeros((nf, nfc), dtype=np.uint16)
    k = 0
    for j in range(nj):
        for c in range(10):
            if mask_list[j] & (1 << c):
                fd[:, k] = np.clip(np.round((vals[:, j, c] - offs[j, c]) / scls[j, c]), 0, 65535).astype(np.uint16)
                k += 1
    anim_recs = b""
    for name, first, num, rate in anims:
        anim_recs += struct.pack("<3IfI", text.add(name), first, num, rate, 0)
    # bounds per frame from the skinned mesh
    bounds = b""
    for f in range(nf):
        v = skin_cpu(verts, parents, bind_locals, frame_locals[f])
        mn, mx = v.min(0), v.max(0)
        bounds += struct.pack("<8f", *mn, *mx, float(np.sqrt(v[:, 0] ** 2 + v[:, 1] ** 2).max()),
                              float(np.sqrt((v ** 2).sum(1)).max()))
    cbytes = comment.encode("ascii") + b"\0"

    # vertex arrays
    nv = len(verts["pos"])
    arrays = [
        (IQM_POSITION, IQM_FLOAT, 3, verts["pos"].astype("<f4").tobytes()),
        (IQM_TEXCOORD, IQM_FLOAT, 2, verts["uv"].astype("<f4").tobytes()),
        (IQM_NORMAL, IQM_FLOAT, 3, verts["nrm"].astype("<f4").tobytes()),
        (IQM_TANGENT, IQM_FLOAT, 4, verts["tan"].astype("<f4").tobytes()),
        (IQM_BLENDINDEXES, IQM_UBYTE, 4, verts["bi"].astype(np.uint8).tobytes()),
        (IQM_BLENDWEIGHTS, IQM_UBYTE, 4, verts["bw"].astype(np.uint8).tobytes()),
    ]

    out = bytearray(124)

    def align():
        while len(out) % 4:
            out.append(0)
    ofs = {}
    ofs["text"] = len(out); out += text.buf; align()
    ofs["meshes"] = len(out); out += mesh_rec
    va_hdr_at = len(out); out += bytes(20 * len(arrays))
    va = b""
    for typ, fmt, size, data in arrays:
        align()
        va += struct.pack("<5I", typ, 0, fmt, size, len(out))
        out += data
    out[va_hdr_at:va_hdr_at + len(va)] = va
    align()
    ofs["tri"] = len(out); out += np.asarray(tris, dtype="<u4").tobytes()
    ofs["adj"] = len(out); out += np.asarray(adjacency, dtype="<u4").tobytes()
    ofs["joints"] = len(out); out += joints
    ofs["poses"] = len(out); out += poses
    ofs["anims"] = len(out); out += anim_recs
    ofs["frames"] = len(out); out += fd.astype("<u2").tobytes(); align()
    ofs["bounds"] = len(out); out += bounds
    ofs["comment"] = len(out); out += cbytes; align()
    hdr = b"INTERQUAKEMODEL\0" + struct.pack(
        "<27I", 2, len(out), 0, len(text.buf), ofs["text"], 1, ofs["meshes"], len(arrays), nv, va_hdr_at,
        len(tris), ofs["tri"], ofs["adj"], nj, ofs["joints"], nj, ofs["poses"], len(anims), ofs["anims"],
        nf, nfc, ofs["frames"], ofs["bounds"], len(cbytes), ofs["comment"], 0, 0)
    assert len(hdr) == 124
    out[0:124] = hdr
    with open(path, "wb") as fh:
        fh.write(out)
    return len(out), nfc, mask_list


def quat_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                     [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                     [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])


def compose(parents, locals_):
    W = []
    for j, p in enumerate(parents):
        t, q = locals_[j]
        L = m4(quat_mat(q), t)
        W.append(L if p < 0 else W[p] @ L)
    return W


def skin_cpu(verts, parents, bind_locals, frame_locals):
    B = compose(parents, bind_locals)
    P = compose(parents, frame_locals)
    S = np.stack([P[j] @ np.linalg.inv(B[j]) for j in range(len(B))])
    pos = np.concatenate([verts["pos"], np.ones((len(verts["pos"]), 1))], 1)
    w = verts["bw"].astype(np.float64) / 255.0
    out = np.zeros((len(pos), 3))
    for k in range(4):
        M = S[verts["bi"][:, k]]
        out += np.einsum("vij,vj->vi", M[:, :3, :], pos) * w[:, k:k + 1]
    return out


# --------------------------------------------------------------------------------------- build
def recipe_slots(fh, log, tails):
    """The 20 recipe slots (poses 0-10, holds 1289-1297) and reach_claw's source row, in FBX bind
    space, from ONE rig: tools_export_poses.py works on the armature of mesh Hand_Low [TEP:75-76]
    (here GLOBAL_MAIN_CONTROL_R) and the owner mirrors that one hand for both sides (README: both
    model blocks use hand_left.iqm with Scale -1). So both of our hands take these slots; the left
    hand reflects them as data like everything else. The two rigs' bind matrices are identical
    (checked in main), so a slot's bind-space worlds are valid on either rig."""
    rig = HR.Rig(fh.names, fh.parents, fh.TL, fh.fingertips(), log=log, tails=tails)
    rig.solve()
    take, _ = fh.take_worlds()
    rig.pick_sources(take)               # [TEP:780-1075]
    # [TEP:1188-1213] / [TEP:1228-1249]: an authored frame beats the curl table, every finger
    # joint verbatim (pose_only: Root_joint and HANDPALM_joint stay at the bind).
    frames, pose_info = [], []
    for name, curls in rig.poses:
        src = rig.POSE_SOURCE.get(name)
        if src is not None:
            frames.append([w.copy() for w in take[src - 1]])
            pose_info.append((name, curls, None, None, True, src))
            continue
        W, got, basis = rig.pose_worlds(curls, clamp=True)
        frames.append(W)
        pose_info.append((name, curls, got, basis, True, None))
    for name, curls in rig.holds:
        src = rig.HOLD_SOURCE.get(name)
        if src is not None:
            frames.append([w.copy() for w in take[src - 1]])
            pose_info.append((name, curls, None, None, name == "reach", src))
            continue
        W, got, basis = rig.pose_worlds(curls, clamp=(name == "reach"))
        frames.append(W)
        pose_info.append((name, curls, got, basis, name == "reach", None))
    assert len(frames) == 20
    # 1298 reach_claw: the owner's row 241 = source frame 231 = FBX take frame 230 OF THIS RIG
    # (iqm_append_pose.py:41-55). Root_joint and HANDPALM_joint at the bind, as in every row here.
    claw = [w.copy() for w in take[REACH_CLAW_ROW - SOURCE_BASE]]
    return dict(rig=rig, poses=frames[:11], holds=frames[11:], pose_info=pose_info, claw=claw,
                top=fh.top_name, claw_take_frame=REACH_CLAW_ROW - SOURCE_BASE)


def build_hand(fh, left, frame_xf, log, merge_tips, slots, attr=None):
    """frame_xf = (Q, r, s): bind space -> file frame. slots: recipe_slots() of the right rig.
    Returns everything write_iqm needs."""
    Q, r, s = frame_xf
    D = D_MIRROR if left else np.eye(3)
    QD = D @ Q

    def to_file_world(W):
        out = []
        for M in W:
            R = D @ (Q @ M[:3, :3]) @ D
            t = s * (QD @ (M[:3, 3] - r))
            out.append(m4(R, t))
        return out

    def locals_of(Wf, ref=None):
        res = []
        for j, p in enumerate(fh.parents):
            if p < 0:
                L = Wf[j]
            else:
                L = np.linalg.inv(Wf[p]) @ Wf[j]
            q = mat_to_quat(L[:3, :3])
            if ref is not None and np.dot(q, ref[j][1]) < 0:
                q = -q
            res.append((L[:3, 3].copy(), q))
        return res

    bind_W = to_file_world([fh.TL[i].copy() for i in range(22)])
    bind_locals = locals_of(bind_W)
    for j in range(22):
        if bind_locals[j][1][3] < 0:
            bind_locals[j] = (bind_locals[j][0], -bind_locals[j][1])

    take, take_locals = fh.take_worlds()
    frames_bindspace = slots["poses"] + take + slots["holds"]
    assert len(frames_bindspace) == HOLD_BASE + 9
    pose_info = slots["pose_info"]

    frame_locals = [locals_of(to_file_world(W), bind_locals) for W in frames_bindspace]
    # 1298 reach_claw: the owner's row 241 verbatim (iqm_append_pose.py:41-55, grafts=[]), i.e. FBX take
    # frame 230 of the slot rig, wrist at the bind (see HANDOFF note: animating the wrist is open)
    frame_locals.append(locals_of(to_file_world(slots["claw"]), bind_locals))
    # 1299 salute: row 5, MIDDLE_F* rotation channels (TRS_ROT = 3..6) from row 0 (iqm_append_pose.py:56-66)
    sal = [(t.copy(), q.copy()) for t, q in frame_locals[SALUTE_BASE_ROW]]
    for j, n in enumerate(fh.names):
        if n.startswith("MIDDLE_F"):
            sal[j] = (sal[j][0], frame_locals[SALUTE_GRAFT_ROW][j][1].copy())
    frame_locals.append(sal)
    assert len(frame_locals) == HOLD_BASE + 11 == 1300

    # ---- mesh
    # Surface attributes (normal, UV, tangent frame) come from `attr` (Hand_Low for both hands).
    # Hand_Low1 has the same control points and the same polygons in reverse order, but its normals
    # are stored already reflected in X (1667/1667 equal diag(-1,1,1) * Hand_Low's) and its UVs are
    # u ~ 1-u of Hand_Low's (max deviation 0.0028): the FBX gives Hand_Low1 an untextured material
    # (blinn1), and with these UVs the skin texture lands on the wrong islands (nails on the palm;
    # checked in a textured render). So the left hand keeps Hand_Low1's polygon order and skin
    # weights and takes the surface attributes of the identical Hand_Low surface, then reflects.
    attr = attr or fh
    pv_attr = {}
    for pi, poly in enumerate(attr.polys):
        for cp, k in poly:
            pv_attr[(pi, cp)] = k
    keyed, pv_to_v = {}, []
    vpos, vnrm, vuv, vtan, vcp = [], [], [], [], []
    for pi, poly in enumerate(fh.polys):
        for cp, k in poly:
            ka = pv_attr[(pi, cp)]
            key = (cp, int(attr.uvidx[ka]), tuple(np.round(attr.pv_tan[ka], 5)), tuple(np.round(attr.pv_bin[ka], 5)))
            if key not in keyed:
                keyed[key] = len(vpos)
                vpos.append(fh.cp[cp]); vnrm.append(attr.cp_normal[cp])
                u, v = attr.uv[attr.uvidx[ka]]
                vuv.append((u, 1.0 - v))                   # FBX v runs up; texture rows run down
                vtan.append((attr.pv_tan[ka], attr.pv_bin[ka]))
                vcp.append(cp)
            pv_to_v.append(keyed[key])
    if attr is not fh:
        assert np.abs(attr.cp - fh.cp).max() == 0.0
        log("  surface attributes (normals, UVs, tangents) from %s; %s's own normals = diag(-1,1,1) x %s's: %d/%d"
            % (attr.mesh_name, fh.mesh_name, attr.mesh_name,
               int(((fh.cp_normal * (attr.cp_normal * [-1, 1, 1])).sum(1) > 0.999).sum()), len(fh.cp)))
    nv = len(vpos)
    pos = np.array([s * (QD @ (p - r)) for p in vpos])
    nrm = np.array([unit(QD @ n) for n in vnrm])
    tan = np.zeros((nv, 4))
    for i, (t, b) in enumerate(vtan):
        tt = QD @ t
        tt = unit(tt - nrm[i] * (tt @ nrm[i]))
        bb = QD @ b
        tan[i, :3] = tt
        tan[i, 3] = 1.0 if np.dot(np.cross(nrm[i], tt), bb) >= 0 else -1.0
    # triangles: fan per polygon
    tris, tri_cp = [], []
    for poly in fh.polys:
        vs = [pv_to_v[k] for _, k in poly]
        cps = [cp for cp, _ in poly]
        for i in range(1, len(vs) - 1):
            tris.append((vs[0], vs[i], vs[i + 1]))
            tri_cp.append((cps[0], cps[i], cps[i + 1]))
    tris = np.array(tris); tri_cp = np.array(tri_cp)
    A, B, C = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    agree = ((np.cross(B - A, C - A) * (nrm[tris[:, 0]] + nrm[tris[:, 1]] + nrm[tris[:, 2]])).sum(1) > 0).mean()
    # The engine's IQMs (Blender iqm_export) wind each triangle with (b-a)x(c-a) AGAINST the outward
    # normal (checked on dxr's marine_novr, Shotgun, icehook: 0.0 / 0.001 / 0.0 agree).
    if agree > 0.5:
        tris = tris[:, [0, 2, 1]]; tri_cp = tri_cp[:, [0, 2, 1]]
    A, B, C = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    agree_after = ((np.cross(B - A, C - A) * (nrm[tris[:, 0]] + nrm[tris[:, 1]] + nrm[tris[:, 2]])).sum(1) > 0).mean()
    log("  mesh %s: %d control points -> %d vertices, %d triangles; winding vs normal: FBX order %.3f agree, written %.3f agree (want 0)"
        % (fh.mesh_name, len(fh.cp), nv, len(tris), agree, agree_after))
    # adjacency over welded control points
    edges = {}
    for ti, (a, b, c) in enumerate(tri_cp):
        for e in ((a, b), (b, c), (c, a)):
            edges.setdefault((min(e), max(e)), []).append(ti)
    adj = np.full((len(tris), 3), 0xFFFFFFFF, dtype=np.uint64)
    for ti, (a, b, c) in enumerate(tri_cp):
        for k, e in enumerate(((a, b), (b, c), (c, a))):
            o = [x for x in edges[(min(e), max(e))] if x != ti]
            if len(o) == 1:
                adj[ti, k] = o[0]
    # ---- weights: 4 per vertex
    tips = {fh.names.index(f"{f}_UP_TOP_joint"): fh.names.index(f"{f}_TOP_joint") for f in HR.FINGERS}
    bi = np.zeros((nv, 4), dtype=np.int64)
    bw = np.zeros((nv, 4), dtype=np.int64)
    max_drop, n_drop, max_q = 0.0, 0, 0.0
    for i in range(nv):
        d = dict(fh.weights[vcp[i]])
        if merge_tips:
            m = {}
            for jj, w in d.items():
                jj = tips.get(jj, jj)
                m[jj] = m.get(jj, 0.0) + w
            d = m
        items = sorted(d.items(), key=lambda kv: (-kv[1], kv[0]))
        drop = sum(w for _, w in items[4:])
        if len(items) > 4:
            n_drop += 1
            max_drop = max(max_drop, drop)
        items = items[:4]
        tot = sum(w for _, w in items)
        exact = [w / tot * 255.0 for _, w in items]
        q = [int(math.floor(x)) for x in exact]
        rem = 255 - sum(q)
        order = sorted(range(len(items)), key=lambda k: -(exact[k] - q[k]))
        for k in order[:rem]:
            q[k] += 1
        max_q = max(max_q, max(abs(q[k] - exact[k]) / 255.0 for k in range(len(items))))
        for k, (jj, _) in enumerate(items):
            bi[i, k] = jj
            bw[i, k] = q[k]
    log("  weights: %d vertices had more than 4 influences%s; largest weight sum dropped %.5f; "
        "ubyte rounding error max %.5f; every vertex sums to 255"
        % (n_drop, " after merging UP_TOP into TOP" if merge_tips else "", max_drop, max_q))
    verts = {"pos": pos, "nrm": nrm, "uv": np.array(vuv), "tan": tan, "bi": bi, "bw": bw}
    return dict(rig=slots["rig"], bind_locals=bind_locals, frame_locals=frame_locals, verts=verts, tris=tris,
                adj=adj.astype(np.uint32), pose_info=pose_info, take_locals=take_locals, max_drop=max_drop)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fbx", default=DEFAULT_FBX)
    ap.add_argument("--tex", default=None)
    ap.add_argument("--out", default=os.path.join(REPO, "vrealms_iwad", "models", "hands"))
    ap.add_argument("--tails", choices=("mesh", "blender"), default="blender",
                    help="where the recipe says a finger ends: the mesh fingertip, or Blender's import "
                         "bone tail (emulated; see handrecipe.Rig)")
    ap.add_argument("--keep-tips", action="store_true",
                    help="do NOT merge each UP_TOP joint's weight into its TOP joint before cutting to 4. "
                         "Merging is the default (the take never rotates UP_TOP). The recipe's poses do "
                         "rotate UP_TOP (TAPER 0.35); measured, merging moves no vertex of any checked "
                         "frame by more than 0.073 map units (0.8 mm).")
    a = ap.parse_args()
    a.merge_tips = not a.keep_tips
    tex = a.tex or os.path.join(os.path.dirname(a.fbx), "textures", TEXTURE_NAME)
    os.makedirs(a.out, exist_ok=True)
    logf = open(os.path.join(HERE, "fbx2iqm_build.log"), "w")

    def log(s=""):
        print(s)
        logf.write(s + "\n")

    sc = Scene(a.fbx)
    log("fbx %s (%d bytes)" % (a.fbx, os.path.getsize(a.fbx)))
    right = FbxHand(sc, "GLOBAL_MAIN_CONTROL_R", log)
    left = FbxHand(sc, "GLOBAL_MAIN_CONTROL_R1", log)
    bind_diff = max(np.abs(right.TL[i] - left.TL[i]).max() for i in range(22))
    log("bind matrices, hand A vs hand B: max difference %.2e" % bind_diff)
    if bind_diff > 1e-12:
        raise SystemExit("the two rigs' binds differ: the right rig's recipe slots cannot be shared")

    # ---- the file frame, from the right hand's bind pose
    TL = right.TL
    jn = right.names.index
    r = TL[jn("Root_joint")][:3, 3].copy()
    x = unit(TL[jn("MIDDLE_F_UP_TOP_joint")][:3, 3] - r)
    y0 = TL[jn("INDEX_BASE_joint")][:3, 3] - TL[jn("PINK_BASE_joint")][:3, 3]
    y = unit(y0 - x * (y0 @ x))
    z = np.cross(x, y)
    Q = np.stack([x, y, z])
    # the hand's length, wrist to the middle fingertip ON THE MESH
    jt, ju = jn("MIDDLE_F_TOP_joint"), jn("MIDDLE_F_UP_TOP_joint")
    tipverts = [i for i in range(len(right.cp)) if right.dominant(i) in (jt, ju)]
    L_mesh = max(np.linalg.norm(right.cp[i] - r) for i in tipverts)
    L_joint = np.linalg.norm(TL[ju][:3, 3] - r)
    target = HAND_LENGTH_M * UNITS_PER_METRE
    s = target / L_mesh
    log("file frame: X = Root->MIDDLE_F_UP_TOP %s, Y = index-minus-pinky knuckles %s, Z %s (FBX bind axes)"
        % (np.round(x, 4), np.round(y, 4), np.round(z, 4)))
    log("hand length: mesh %.5f FBX units (joint-to-joint %.5f); target %.3f m x %.4f units/m = %.4f map units; scale %.6f"
        % (L_mesh, L_joint, HAND_LENGTH_M, UNITS_PER_METRE, target, s))

    solved = {"units_per_metre": UNITS_PER_METRE, "hand_length_m": HAND_LENGTH_M, "scale": s,
              "Q_rows_fbx_to_file": Q.tolist(), "root_fbx": r.tolist(), "mesh_length_fbx": L_mesh,
              "joint_length_fbx": L_joint, "merge_tips": a.merge_tips, "taper": HR.TAPER, "tails": a.tails}
    log("")
    log("=" * 30 + " RECIPE on %s / %s (TEP's armature: the one skinning mesh Hand_Low [TEP:75-76])"
        % (right.top_name, right.mesh_name))
    slots = recipe_slots(right, log, a.tails)
    solved["slots_rig"] = right.top_name
    for side, fh, is_left, fname in (("RIGHT", right, False, "hand_r.iqm"), ("LEFT", left, True, "hand_l.iqm")):
        log("")
        log("=" * 30 + " %s hand: %s / %s" % (side, fh.top_name, fh.mesh_name))
        b = build_hand(fh, is_left, (Q, r, s), log, a.merge_tips, slots, attr=right)
        rig = b["rig"]
        log("  frames 0-10 and 1289-1299 from the recipe on %s%s; frames 11-1288 this hand's own take"
            % (slots["top"], ", reflected" if is_left else ""))
        log("  poses (authored take frame of %s, or table curls -> applied after the ceiling):" % slots["top"])
        for k, (name, curls, got, basis, cl, src) in enumerate(b["pose_info"]):
            fr = k if k < 11 else HOLD_BASE + (k - 11)
            if src is not None:
                log("    %4d %-13s AUTHORED source frame %d = FBX take frame %d (table %s not used)" % (fr, name, src, src - 1, curls))
            else:
                log("    %4d %-13s table %s -> applied %s%s" % (fr, name, curls, got, "" if cl else "  (unclamped)"))
        log("  axes: fingers %s, thumb %s; adduction %s; ceilings %s; pinch gap %.2fcm"
            % (rig.finger_axes_kind, rig.thumb_axes_kind, {k: v for k, v in rig.ADDUCT.items()}, rig.MAX_CURL, rig.pinch_gap_cm))
        anims = [(n, i, 1, 60.0) for i, (n, _) in enumerate(HR.POSES)]
        anims.append(("take", SOURCE_BASE, TAKE_FRAMES, 60.0))
        anims += [(n, HOLD_BASE + i, 1, 60.0) for i, n in enumerate(HOLD_NAMES)]
        anims.append(("fist_authored", FIST_AUTHORED, 1, 60.0))
        comment = ("hand_final.fbx %s (%s) -> tools/fbx_hands/fbx2iqm.py. File frame: +X wrist->middle fingertip, "
                   "+Y left, +Z back of hand (palm -Z), origin Root_joint, map units (0.19 m at %.1f u/m). "
                   "Frames 0-10 and 1289-1299 RS_Hands tools_export_poses.py slots (authored take frames of %s where "
                   "its picker matched, curl table otherwise; 1298-1299 as iqm_append_pose.py), 11-1288 FBX Take 001 "
                   "0-1277 of this hand. Root and wrist at bind in every frame. Weights cut to 4%s."
                   % (fh.mesh_name, side.lower(), UNITS_PER_METRE, slots["top"], ", UP_TOP merged into TOP" if a.merge_tips else ""))
        solved[side.lower()] = {
            "fbx_top": fh.top_name, "mesh": fh.mesh_name, "mirrored": is_left,
            "slots_rig": slots["top"], "reach_claw_fbx_take_frame": slots["claw_take_frame"],
            "finger_axes": rig.finger_axes_kind, "thumb_axes": rig.thumb_axes_kind,
            "AXES": {k: list(map(float, v)) for k, v in rig.AXES.items()},
            "ADDUCT_AXES": {k: list(map(float, v)) for k, v in rig.ADDUCT_AXES.items()},
            "ADDUCT": rig.ADDUCT, "MAX_CURL": rig.MAX_CURL, "cm_units": rig.CM, "pinch_gap_cm": rig.pinch_gap_cm,
            "fold_check_cm": rig.fold,
            "tails": rig.tails, "blender_bone_lengths": dict(zip(fh.names, rig.blen)),
            "frames": [{"frame": (k if k < 11 else HOLD_BASE + (k - 11)), "name": name, "table": curls,
                        "applied": got, "clamped": cl, "source_frame_1based": src,
                        "fbx_take_frame": (src - 1 if src is not None else None)}
                       for k, (name, curls, got, basis, cl, src) in enumerate(b["pose_info"])],
        }
        path = os.path.join(a.out, fname)
        size, nfc, masks = write_iqm(path, fh.mesh_name, TEXTURE_NAME, fh.names, fh.parents, b["bind_locals"],
                                     b["frame_locals"], b["verts"], b["tris"], b["adj"], anims, comment)
        log("  wrote %s: %d bytes, %d vertices, %d triangles, 22 joints, %d frames x %d channels, %d anims"
            % (path, size, len(b["verts"]["pos"]), len(b["tris"]), len(b["frame_locals"]), nfc, len(anims)))
        moving = [fh.names[j] for j in range(22) if masks[j] & 0x78]
        log("  joints with rotation channels: %d (%s)" % (len(moving), ", ".join(moving)))
        log("  joints with translation channels: %s" % [fh.names[j] for j in range(22) if masks[j] & 0x7])

    with open(os.path.join(HERE, "recipe_solved.json"), "w") as jf:
        json.dump(solved, jf, indent=1)
    log("wrote %s" % os.path.join(HERE, "recipe_solved.json"))
    shutil.copyfile(tex, os.path.join(a.out, TEXTURE_NAME))
    log("")
    log("copied %s -> %s" % (tex, os.path.join(a.out, TEXTURE_NAME)))
    with open(os.path.join(HERE, "hand_frames.txt"), "w") as fh:
        fh.write("const SOURCE_BASE  = %d;\n" % SOURCE_BASE)
        fh.write("const SOURCE_COUNT = %d;\n" % TAKE_FRAMES)
        fh.write("const HOLD_BASE    = %d;\n" % HOLD_BASE)
        for i, n in enumerate(HOLD_NAMES):
            fh.write("const POSE_%-13s = HOLD_BASE + %d;\n" % (n.upper(), i))
    log("wrote %s" % os.path.join(HERE, "hand_frames.txt"))
    logf.close()


if __name__ == "__main__":
    main()
