#!/usr/bin/env python3
"""THE GODOT XR TOOLS HANDS WITH THE OWNER'S POSES: an ASCII FBX that the engine reads exactly as it reads
hand_final.fbx -- the same two meshes (Hand_Low = right, Hand_Low1 = left), the same 22 joint names, the same take
("Take 001", 1,279 frames at 60 fps) -- whose every frame is hand_final.fbx's frame of the same number, carried onto
the Godot hand as JOINT ANGLES IN ANATOMICAL FINGER FRAMES. vr_handpose_math.h (kPoseSources, kOwnerPoses, the hold
and colt code) reads it with no change.

    python3 -I retarget_godot.py [--no-thumb-contact] TAKE.json GODOT_HANDS_DIR SKIN.png OUT.fbx [REPORT.json]

TAKE.json   tools/fbxload_test/dump_take's dump of hand_final.fbx: the engine's own loader output (fbx_skinned.cpp):
            per hand the joints, the rest and every frame's local TRS, the rest mesh with its 4 bones/weights.
GODOT_HANDS_DIR  godot-xr-tools addons/godot-xr-tools/hands (model/hand_r.gltf, model/hand_l.gltf; CC0).
SKIN.png    the base colour the materials name (only its file name is written; the engine finds it beside the FBX).

THE CARRY (per hand, per frame t). Everything is done in each hand's ROOT-LOCAL space ("RL": the Root_joint's frame;
the left hand's -1 X scale lives on its Root, so below the Root both hands are right hands, as in hand_final.fbx,
where the two skeletons' rest locals are equal -- fbxload_test H1).
  1. hand_final: each finger joint's canonical frame F (vr_handpose_math.h MeasureFingerFrames: X to the child, Z =
     X x palm, Y = Z x X, measured on the rest = frame 0) and the hand frame HF (HandFrameFromJoints: X wrist ->
     middle knuckle, Y = -palm normal, Z = X x Y). World canonical frames W_j(t) = R_j(t) F_j; the palm's
     W_p(t) = R_p(t) R_p(0)^T HF (the hand frame carried by HANDPALM_joint).
  2. the ANGLES: A_palm(t) = HF^T W_p(t); a finger's first joint A(t) = W_p(t)^T W_j(t); every other joint
     A(t) = W_parent(t)^T W_j(t). Flexion, abduction and twist, each in its own rig's anatomical frame.
  3. the Godot hand (glTF bind, a right hand; the left glTF mirrored in x is one, to 0.63 mm), in RL so that its hand
     frame at rest IS HF and its wrist is the Root: its own bone lengths, knuckle and thumb positions; the joints'
     local axes ARE their canonical frames (X along the bone), so the engine measures F = identity on it.
  4. the same angles on it: W'_p = HF A_palm, W'_first = W'_p A, W'_j = W'_parent A. The engine's pose rotation
     (PoseRotation: q = F^T L(0)^T L(t) F) is then A(0)^T A(t) on both rigs: equal by construction (checked after
     load by check_retarget.cpp).
  5. THE THUMB WHERE IT TOUCHES THE FINGERS (thumb_contact, below): hand_final's thumb pivots at mid-palm on a short
     first bone, the Godot thumb at the wrist on a long metacarpal; with the angles alone the Godot thumb stands off the
     fist, the point and the cup (seen in render_compare.py). Only where hand_final's thumb IP joint or tip is within
     32 mm of a finger's middle or end bone, its three outer points are carried with the finger bones near them and the
     Godot thumb is fitted to them (least squares, held near the carried angles). Every other finger, and the thumb
     everywhere else, keeps the angles exactly. `--no-thumb-contact` writes the angles alone.
  JOINTS: Root_joint = Wrist; INDEX/MIDDLE_F/RING/PINK BASE, MID, TOP, UP_TOP = Proximal, Intermediate, Distal, Tip;
  THUMB BASE, MID, TOP, UP_TOP = Metacarpal, Proximal, Distal, Tip; HANDPALM_joint has no Godot joint: it is placed
  where hand_final's is, in hand proportions (along the hand by wrist->middle-knuckle length, across by knuckle width).
  The four finger METACARPALS (and Palm, unweighted) fold into HANDPALM; their skin weight and the Wrist's are split
  between Root_joint and HANDPALM_joint along the hand as hand_final's own skin splits them (its Root/HANDPALM weight
  ratio by the fraction of the way from the wrist to the middle knuckle), so the palm bends where hand_final's does.
SIZE: scaled so that Root_joint to the farthest middle-fingertip vertex (largest weight MIDDLE_F_TOP/UP_TOP) at rest is
hand_final's 1.712605 units: the MODELDEF Scale 0.11094212 (the owner's 0.19 m) stays.
REST: frame 0 (hand_final's open frame, carried) is the bind pose: the Godot mesh is skinned into it once here (linear
blend, the glTF's own weights) and written so; the clusters bind it there.
"""
import base64
import json
import math
import sys

import numpy as np

# --------------------------------------------------------------------------- small math

def qmat(q):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w)
    x, y, z, w = x / n, y / n, z / n, w / n
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def trs(t):
    M = np.eye(4)
    M[:3, :3] = qmat(t[3:7]) * np.array(t[7:10])[None, :]
    M[:3, 3] = t[0:3]
    return M


def unit(v):
    n = np.linalg.norm(v)
    if n < 1e-12:
        raise ValueError('zero vector')
    return v / n


def ortho(M):
    U, _, Vt = np.linalg.svd(M)
    R = U @ Vt
    if np.linalg.det(R) < 0:
        raise ValueError('reflection where a rotation was expected')
    return R


def euler_xyz(R):
    """FBX eEulerXYZ: R = Rz(c) Ry(b) Rx(a); degrees (a, b, c)."""
    sb = -R[2, 0]
    sb = max(-1.0, min(1.0, sb))
    b = math.asin(sb)
    if abs(sb) < 0.9999999:
        a = math.atan2(R[2, 1], R[2, 2])
        c = math.atan2(R[1, 0], R[0, 0])
    else:
        a = math.atan2(-R[1, 2], R[1, 1])
        c = 0.0
    return np.degrees([a, b, c])


def rot_from_euler(e):
    a, b, c = np.radians(e)
    Rx = np.array([[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]])
    Ry = np.array([[math.cos(b), 0, math.sin(b)], [0, 1, 0], [-math.sin(b), 0, math.cos(b)]])
    Rz = np.array([[math.cos(c), -math.sin(c), 0], [math.sin(c), math.cos(c), 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


def unwrap(prev, e):
    """The Euler triple equal to e (as a rotation) nearest prev: +-360 per angle, and the (a+180, 180-b, c+180) twin."""
    best = None
    for cand in (np.array(e), np.array([e[0] + 180, 180 - e[1], e[2] + 180])):
        c = cand.copy()
        for k in range(3):
            c[k] += 360.0 * round((prev[k] - c[k]) / 360.0)
        d = np.abs(c - prev).sum()
        if best is None or d < best[0]:
            best = (d, c)
    return best[1]

# --------------------------------------------------------------------------- the rigs

FINGERS = ['THUMB', 'INDEX', 'MIDDLE_F', 'RING', 'PINK']
SEGS = ['BASE', 'MID', 'TOP', 'UP_TOP']
GODOT_FINGER = {'THUMB': ('Thumb', ['Metacarpal', 'Proximal', 'Distal', 'Tip']),
                'INDEX': ('Index', ['Proximal', 'Intermediate', 'Distal', 'Tip']),
                'MIDDLE_F': ('Middle', ['Proximal', 'Intermediate', 'Distal', 'Tip']),
                'RING': ('Ring', ['Proximal', 'Intermediate', 'Distal', 'Tip']),
                'PINK': ('Little', ['Proximal', 'Intermediate', 'Distal', 'Tip'])}
# the joint order of the new file: parents first, hand_final's own order (dump_take)
ORDER = ['Root_joint', 'HANDPALM_joint']
for f in ['INDEX', 'MIDDLE_F', 'RING', 'PINK', 'THUMB']:
    ORDER += ['%s_%s_joint' % (f, s) for s in SEGS]
PARENT = {'Root_joint': None, 'HANDPALM_joint': 'Root_joint'}
for f in FINGERS:
    prev = 'HANDPALM_joint'
    for s in SEGS:
        n = '%s_%s_joint' % (f, s)
        PARENT[n] = prev
        prev = n
DRIVEN = ['%s_%s_joint' % (f, s) for f in FINGERS for s in SEGS[:3]]   # the 15 joints the engine turns
TARGET_MEASURE = 1.712605   # hand_final: Root_joint to the farthest middle-fingertip vertex (vrealms_iwad/MODELDEF.txt)


def old_hand(H):
    names = H['names']
    idx = {n: i for i, n in enumerate(names)}
    par = H['parents']
    frames = H['frames']

    def rl_worlds(locs):
        W = [None] * len(names)
        for j, p in enumerate(par):
            W[j] = np.eye(4) if p < 0 else W[p] @ trs(locs[j])
        return W
    rest = rl_worlds(frames[0])
    pos = lambda W, n: W[idx[n]][:3, 3]
    # MeasurePalm / HandFrameFromJoints, right-handed below the Root (RL)
    h, ix, md, rg, pk = (pos(rest, n) for n in ['Root_joint', 'INDEX_BASE_joint', 'MIDDLE_F_BASE_joint', 'RING_BASE_joint', 'PINK_BASE_joint'])
    HF = hand_frame(h, ix, md, pk)
    palm = -HF[:, 1]
    # MeasureFingerFrames: in each joint's own (local) space
    F = {}
    for f in FINGERS:
        for s in SEGS[:3]:
            n = '%s_%s_joint' % (f, s)
            j = idx[n]
            kids = [c for c in range(len(names)) if par[c] == j]
            best = max(kids, key=lambda c: np.linalg.norm(frames[0][c][0:3]))   # first of equals, as max()
            X = unit(np.array(frames[0][best][0:3]))
            G = rest[j][:3, :3]
            pl = unit(G.T @ palm)
            Z = unit(np.cross(X, pl))
            u = unit(np.cross(X, Z))
            X = unit(np.cross(Z, u))
            Y = np.cross(Z, X)
            assert Y @ pl > 0.05, n
            F[n] = np.column_stack([X, Y, Z])
    return dict(names=names, idx=idx, par=par, frames=frames, rl=rl_worlds, rest=rest, HF=HF, F=F, H=H)


def hand_frame(wrist, ix, md, pk):
    f = unit(md - wrist)
    kr = ix - pk
    k = unit(kr - f * (kr @ f))
    palm = unit(np.cross(k, f))   # right hand
    X = f
    Y = -palm
    Z = np.cross(X, Y)
    return np.column_stack([X, Y, Z])


def angles(old, t):
    """hand_final frame t as anatomical angles: A[name] (3x3), and the palm's."""
    W = old['rl'](old['frames'][t])
    idx = old['idx']
    Rp = ortho(W[idx['HANDPALM_joint']][:3, :3])
    Rp0 = ortho(old['rest'][idx['HANDPALM_joint']][:3, :3])
    Wp = Rp @ Rp0.T @ old['HF']
    A = {'HANDPALM_joint': old['HF'].T @ Wp}
    for f in FINGERS:
        prevW = Wp
        for s in SEGS[:3]:
            n = '%s_%s_joint' % (f, s)
            Wj = ortho(W[idx[n]][:3, :3]) @ old['F'][n]
            A[n] = prevW.T @ Wj
            prevW = Wj
    return A

# --------------------------------------------------------------------------- the glTF

def load_gltf(path):
    g = json.load(open(path))
    buf = base64.b64decode(g['buffers'][0]['uri'].split(',', 1)[1])

    def acc(i):
        a = g['accessors'][i]
        bv = g['bufferViews'][a['bufferView']]
        ct = {5126: 'f', 5123: 'H', 5121: 'B', 5125: 'I'}[a['componentType']]
        nc = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
        off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        arr = np.frombuffer(buf, dtype=np.dtype(ct).newbyteorder('<'), count=a['count'] * nc, offset=off)
        return arr.reshape(a['count'], nc) if nc > 1 else arr
    meshes = [i for i, n in enumerate(g['nodes']) if 'mesh' in n]
    assert len(meshes) == 1 and len(g['meshes']) == 1 and len(g['meshes'][0]['primitives']) == 1
    prim = g['meshes'][0]['primitives'][0]
    at = prim['attributes']
    mesh = dict(pos=acc(at['POSITION']).astype(float), nrm=acc(at['NORMAL']).astype(float), uv=acc(at['TEXCOORD_0']).astype(float),
                jnt=acc(at['JOINTS_0']).astype(int), wts=acc(at['WEIGHTS_0']).astype(float), idx=acc(prim['indices']).astype(int))
    nodes = g['nodes']
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get('children', []):
            parent[c] = i

    def local(i):
        n = nodes[i]
        if 'matrix' in n:
            return np.array(n['matrix']).reshape(4, 4).T
        M = np.eye(4)
        M[:3, :3] = qmat(n.get('rotation', [0, 0, 0, 1])) * np.array(n.get('scale', [1, 1, 1]))[None, :]
        M[:3, 3] = n.get('translation', [0, 0, 0])
        return M

    def world(i):
        M = local(i)
        while i in parent:
            i = parent[i]
            M = local(i) @ M
        return M
    skin = g['skins'][0]
    names = [nodes[j]['name'] for j in skin['joints']]
    jpos = {names[k]: world(j)[:3, 3] for k, j in enumerate(skin['joints'])}
    mw = world(meshes[0])
    assert np.abs(mw - np.eye(4)).max() < 1e-9, 'the mesh node is not at the origin'
    # the bind: the inverse bind matrices are the joints' world inverses (checked)
    ibm = acc(skin['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
    for k, j in enumerate(skin['joints']):
        assert np.abs(world(j) @ ibm[k] - np.eye(4)).max() < 1e-5, names[k]
    return dict(mesh=mesh, names=names, jpos=jpos)


def godot_name(n, side):
    sfx = '_R' if side == 0 else '_L'
    if n == 'Root_joint':
        return 'Wrist' + sfx
    for f in FINGERS:
        for k, s in enumerate(SEGS):
            if n == '%s_%s_joint' % (f, s):
                gf, gs = GODOT_FINGER[f]
                return '%s_%s%s' % (gf, gs[k], sfx)
    return None

# --------------------------------------------------------------------------- one hand

# --------------------------------------------------------------------------- the thumb's contacts
#
# WHY. The angles carry every finger's shape (render_compare.py), but not the thumb's where it TOUCHES the fingers: hand_final's
# thumb pivots at mid-palm on a 19 mm first bone, the Godot thumb at the wrist on a 40 mm metacarpal, and the fingers it rests
# on are 25-40% longer. With hand_final's angles the Godot thumb stands off the fist, the cup and the curled index (seen:
# fbx_fist, ctl_point, fbx_cup renders). So, only where hand_final's thumb tip is AT a finger (within CONTACT_FULL of a finger
# bone's centre line; none beyond CONTACT_NONE, a smooth blend between), the Godot thumb tip is sent to where that finger
# part went: the old tip plus the displacement (new - old joint positions, this frame) of the finger bones near it, weighted
# by 1 / (distance^4 + eps^4). Reached by (a) one common flexion change at the thumb's MID and TOP joints (about their own
# flexion axes) for the reach from the thumb's base, then (b) the least rotation of the whole thumb about its base joint for
# the direction. Twist is untouched. Everywhere else, and in every other finger, the angles stand as carried.
CONTACT_FULL = 0.016 / 0.11094212   # 16 mm (hand file units)
CONTACT_NONE = 0.032 / 0.11094212   # 32 mm
CONTACT_EPS = 0.004 / 0.11094212
FOUR = ['INDEX', 'MIDDLE_F', 'RING', 'PINK']
NO_CONTACT = '--no-thumb-contact' in sys.argv   # the angles alone (for comparison)


def _seg_closest(p, a, b):
    ab = b - a
    t = np.clip((p - a) @ ab / max(ab @ ab, 1e-18), 0.0, 1.0)
    return t, np.linalg.norm(p - (a + t * ab))


def _rot_z(deg):
    c, s_ = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[c, -s_, 0], [s_, c, 0], [0, 0, 1]])


def _min_rot(u, v):
    u = unit(u)
    v = unit(v)
    ax = np.cross(u, v)
    sn = np.linalg.norm(ax)
    cs = u @ v
    if sn < 1e-12:
        return np.eye(3)
    ax /= sn
    K = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]])
    ang = math.atan2(sn, cs)
    return np.eye(3) + math.sin(ang) * K + (1 - math.cos(ang)) * K @ K


def _new_worlds(loc, loc_t):
    out = {}
    for n in ORDER:
        p = PARENT[n]
        M = np.eye(4)
        M[:3, :3] = loc[n]
        M[:3, 3] = loc_t[n]
        out[n] = M if p is None else out[p] @ M
    return out


def _rotvec(v):
    th = np.linalg.norm(v)
    if th < 1e-12:
        return np.eye(3)
    k = v / th
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3) + math.sin(th) * K + (1 - math.cos(th)) * K @ K


def _rot_y(deg):
    c, s_ = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[c, 0, s_], [0, 1, 0], [-s_, 0, c]])


THUMB3 = ('THUMB_MID_joint', 'THUMB_TOP_joint', 'THUMB_UP_TOP_joint')
FIT_WEIGHT = {'THUMB_MID_joint': 0.5, 'THUMB_TOP_joint': 1.0, 'THUMB_UP_TOP_joint': 1.0}
FIT_REG = 0.01   # file units per radian of departure from the carried angles (~1.1 mm a radian): the fit leads


def _thumb_apply(loc, x):
    """x = base rotation vector (radians, in the palm's axes, before the carried base), MCP flexion and abduction,
    IP flexion (degrees about the joints' own Z / Y): a copy of loc with the thumb changed."""
    l2 = dict(loc)
    l2['THUMB_BASE_joint'] = _rotvec(x[0:3]) @ loc['THUMB_BASE_joint']
    l2['THUMB_MID_joint'] = loc['THUMB_MID_joint'] @ _rot_z(x[3]) @ _rot_y(x[4])
    l2['THUMB_TOP_joint'] = loc['THUMB_TOP_joint'] @ _rot_z(x[5])
    return l2


def thumb_contact(old, t, loc, loc_t):
    """Edits loc (this frame's local rotations) in place; returns (contact weight, tip move in file units)."""
    Wo = old['rl'](old['frames'][t])
    op = lambda n: Wo[old['idx'][n]][:3, 3]
    Wn = _new_worlds(loc, loc_t)
    np_ = lambda n: Wn[n][:3, 3]
    # contact: the thumb's IP joint or tip at a finger's middle or end bone (hand_final, this frame)
    contact_segs, field = [], []
    for f in FOUR:
        ch = ['%s_%s_joint' % (f, s) for s in SEGS]
        for k, (a, b) in enumerate(zip(ch[:-1], ch[1:])):
            field.append((a, b))
            if k >= 1:
                contact_segs.append((a, b))
        field.append(('HANDPALM_joint', ch[0]))
        field.append(('Root_joint', ch[0]))
    dmin = 1e9
    for n in THUMB3[1:]:
        for a, b in contact_segs:
            dmin = min(dmin, _seg_closest(op(n), op(a), op(b))[1])
    c = float(np.clip((CONTACT_NONE - dmin) / (CONTACT_NONE - CONTACT_FULL), 0.0, 1.0))
    c = c * c * (3 - 2 * c)
    if c <= 0:
        return 0.0, 0.0
    # each of the thumb's three outer points, carried with the hand around it: hand_final's point plus the displacement
    # (new - old, this frame) of the finger and palm bones near it, weighted 1 / (d^2 + eps^2)
    targets = {}
    for n in THUMB3:
        acc = np.zeros(3)
        wsum = 0.0
        for a, b in field:
            u, d = _seg_closest(op(n), op(a), op(b))
            disp = (1 - u) * (np_(a) - op(a)) + u * (np_(b) - op(b))
            w = 1.0 / (d * d + CONTACT_EPS * CONTACT_EPS)
            acc += w * disp
            wsum += w
        targets[n] = (1 - c) * np_(n) + c * (op(n) + acc / wsum)

    def resid(x):
        W2 = _new_worlds(_thumb_apply(loc, x), loc_t)
        r = [FIT_WEIGHT[n] * (W2[n][:3, 3] - targets[n]) for n in THUMB3]
        r.append(FIT_REG * x[0:3])
        r.append(FIT_REG * np.radians(x[3:6]))
        return np.concatenate([np.ravel(v) for v in r])
    x = np.zeros(6)
    lam = 1e-3
    r = resid(x)
    for _ in range(40):
        J = np.zeros((len(r), 6))
        for k in range(6):
            h = 1e-6 if k < 3 else 1e-4
            dx = np.zeros(6)
            dx[k] = h
            J[:, k] = (resid(x + dx) - r) / h
        step = np.linalg.solve(J.T @ J + lam * np.diag(np.diag(J.T @ J) + 1e-12), -J.T @ r)
        r2 = resid(x + step)
        if r2 @ r2 < r @ r:
            x, r = x + step, r2
            lam *= 0.3
            if np.abs(step).max() < 1e-7:
                break
        else:
            lam *= 10
    l2 = _thumb_apply(loc, x)
    W3 = _new_worlds(l2, loc_t)
    moved = float(np.linalg.norm(W3['THUMB_UP_TOP_joint'][:3, 3] - np_('THUMB_UP_TOP_joint')))
    for n in ('THUMB_BASE_joint', 'THUMB_MID_joint', 'THUMB_TOP_joint'):
        loc[n] = l2[n]
    return c, moved


def build_hand(old, gl, side, scale, report, probe=False):
    """The new hand in RL: rest joint positions, local translations, the per-frame local rotations, and the mesh
    skinned into frame 0. Returns a dict, and the measure (Root to farthest middle-fingertip vertex)."""
    mirror = np.diag([-1.0, 1.0, 1.0]) if side == 1 else np.eye(3)   # the left glTF, mirrored in x: a right hand
    gp = {n: mirror @ gl['jpos'][godot_name(n, side)] for n in ORDER if godot_name(n, side)}
    wrist = gp['Root_joint']
    HFg = hand_frame(wrist, gp['INDEX_BASE_joint'], gp['MIDDLE_F_BASE_joint'], gp['PINK_BASE_joint'])
    HF = old['HF']
    to_rl = lambda p: HF @ (HFg.T @ (scale * (p - wrist)))
    P = {n: to_rl(p) for n, p in gp.items()}   # the Godot joints in RL at its bind

    # HANDPALM_joint where hand_final's is, in hand proportions
    rest = old['rest']
    idx = old['idx']
    op = lambda n: rest[idx[n]][:3, 3]
    c = HF.T @ op('HANDPALM_joint')   # wrist = origin in RL
    lo_len = np.linalg.norm(op('MIDDLE_F_BASE_joint'))
    ln_len = np.linalg.norm(P['MIDDLE_F_BASE_joint'])
    lo_w = np.linalg.norm(op('INDEX_BASE_joint') - op('PINK_BASE_joint'))
    ln_w = np.linalg.norm(P['INDEX_BASE_joint'] - P['PINK_BASE_joint'])
    ratio = np.array([ln_len / lo_len, 0.5 * (ln_len / lo_len + ln_w / lo_w), ln_w / lo_w])
    P['HANDPALM_joint'] = HF @ (c * ratio)
    report['palm_pivot_fraction_along'] = float(c[0] / lo_len)

    # the joints' local translations (constant): in the parent's local axes at the Godot bind = the canonical frames
    # there (BindCanon); a finger joint's child lies along its X.
    palm_n = -HF[:, 1]

    def canon(n, Xdir):
        X = unit(Xdir)
        Z = unit(np.cross(X, palm_n))
        Y = np.cross(Z, X)
        return np.column_stack([X, Y, Z])
    bindR = {'Root_joint': np.eye(3), 'HANDPALM_joint': HF.copy()}
    for f in FINGERS:
        for s_i, s in enumerate(SEGS):
            n = '%s_%s_joint' % (f, s)
            if s_i < 3:
                child = '%s_%s_joint' % (f, SEGS[s_i + 1])
                bindR[n] = canon(n, P[child] - P[n])
            else:
                bindR[n] = bindR['%s_%s_joint' % (f, SEGS[2])]
    loc_t = {}
    for n in ORDER:
        p = PARENT[n]
        if p is None:
            loc_t[n] = np.zeros(3)
        elif p == 'HANDPALM_joint' or p == 'Root_joint':
            loc_t[n] = bindR[p].T @ (P[n] - P[p])
        else:
            L = np.linalg.norm(P[n] - P[p])
            loc_t[n] = np.array([L, 0.0, 0.0])
            report.setdefault('bone_lengths', {})[n] = float(L)

    # every frame: the local rotations (relative to the parent's axes) and the RL worlds
    nfr = 1 if probe else len(old['frames'])
    loc_r = {n: np.zeros((nfr, 3, 3)) for n in ORDER}

    def pose(t):
        A = angles(old, t)
        Wr = {'Root_joint': np.eye(3), 'HANDPALM_joint': HF @ A['HANDPALM_joint']}
        loc = {'Root_joint': np.eye(3), 'HANDPALM_joint': Wr['HANDPALM_joint']}
        for f in FINGERS:
            prev = 'HANDPALM_joint'
            for s in SEGS[:3]:
                n = '%s_%s_joint' % (f, s)
                Wr[n] = Wr[prev] @ A[n]
                loc[n] = A[n]
                prev = n
            n = '%s_UP_TOP_joint' % f
            Wr[n] = Wr[prev]
            loc[n] = np.eye(3)
        return Wr, loc
    thumb_log = []
    for t in range(nfr):
        _, loc = pose(t)
        if not probe and not NO_CONTACT:
            thumb_log.append(thumb_contact(old, t, loc, loc_t))
        elif not probe:
            thumb_log.append((0.0, 0.0))
        for n in ORDER:
            loc_r[n][t] = loc[n]
    if not probe:
        corr = np.array([x[0] for x in thumb_log])
        moved = np.array([x[1] for x in thumb_log])
        report['thumb_contact'] = {'frames_corrected': int((corr > 0).sum()), 'frames_fully': int((corr >= 1).sum()),
                                   'largest_tip_move_mm': float(moved.max() * 0.11094212 * 1000),
                                   'frame0_weight': float(corr[0])}
        report['thumb_contact_per_frame'] = [[float(a), float(b)] for a, b in thumb_log]

    def worlds(t):
        out = {}
        for n in ORDER:
            p = PARENT[n]
            M = np.eye(4)
            M[:3, :3] = loc_r[n][t]
            M[:3, 3] = loc_t[n]
            out[n] = M if p is None else out[p] @ M
        return out
    W0 = worlds(0)

    # the mesh: Godot bind (mirrored for the left) -> RL -> skinned into frame 0
    m = gl['mesh']
    pos = np.array([to_rl(mirror @ p) for p in m['pos']])
    nrm = np.array([unit(HF @ (HFg.T @ (mirror @ v))) for v in m['nrm']])
    # weights onto the new joints
    gidx = {nm: k for k, nm in enumerate(gl['names'])}
    target = {}
    for n in ORDER:
        gn = godot_name(n, side)
        if gn:
            target[gidx[gn]] = n
    sfx = '_R' if side == 0 else '_L'
    group = [gidx['Wrist' + sfx], gidx['Palm' + sfx]] + [gidx['%s_Metacarpal%s' % (g, sfx)] for g in ['Index', 'Middle', 'Ring', 'Little']]
    # hand_final's own Root/HANDPALM split along the hand
    V = np.array(old['H']['verts'])
    oroot = np.linalg.inv(trs(old['H']['rest'][0]))   # file -> RL
    opos = (oroot[:3, :3] @ V[:, 0:3].T).T + oroot[:3, 3]
    xo = opos @ HF[:, 0] / lo_len
    wr = np.zeros(len(V))
    wp = np.zeros(len(V))
    for k in range(4):
        b = V[:, 8 + k].astype(int)
        w = V[:, 12 + k] / 255.0
        wr += np.where(b == idx['Root_joint'], w, 0)
        wp += np.where(b == idx['HANDPALM_joint'], w, 0)
    bins = np.arange(-0.4, 1.3, 0.025)
    prof_x, prof_r = [], []
    for b0 in bins:
        s = (xo >= b0) & (xo < b0 + 0.025) & (wr + wp > 1e-6)
        if s.sum() >= 3:
            prof_x.append(b0 + 0.0125)
            prof_r.append(wr[s].sum() / (wr[s] + wp[s]).sum())
    prof_x, prof_r = np.array(prof_x), np.array(prof_r)
    report['root_palm_split'] = [[float(a), float(b)] for a, b in zip(prof_x, prof_r)]
    xn = pos @ HF[:, 0] / ln_len
    rsplit = np.interp(xn, prof_x, prof_r)
    nv = len(pos)
    jw = np.zeros((nv, len(ORDER)))
    jn = {n: k for k, n in enumerate(ORDER)}
    for k in range(4):
        gj = m['jnt'][:, k]
        w = m['wts'][:, k]
        for v in range(nv):
            if w[v] <= 0:
                continue
            if gj[v] in group:
                jw[v, jn['Root_joint']] += w[v] * rsplit[v]
                jw[v, jn['HANDPALM_joint']] += w[v] * (1 - rsplit[v])
            else:
                jw[v, jn[target[gj[v]]]] += w[v]
    jw /= jw.sum(1, keepdims=True)
    # skin into frame 0: M_j = World0_j . BindCanon_j^-1 (the Godot bind, as canonical frames at its joints)
    skinM = {}
    for n in ORDER:
        B = np.eye(4)
        B[:3, :3] = bindR[n]
        B[:3, 3] = P[n]
        skinM[n] = W0[n] @ np.linalg.inv(B)
    pos0 = np.zeros_like(pos)
    nrm0 = np.zeros_like(nrm)
    for k, n in enumerate(ORDER):
        w = jw[:, k]
        if not w.any():
            continue
        M = skinM[n]
        pos0 += w[:, None] * ((M[:3, :3] @ pos.T).T + M[:3, 3])
        nrm0 += w[:, None] * (M[:3, :3] @ nrm.T).T
    nrm0 = nrm0 / np.linalg.norm(nrm0, axis=1, keepdims=True)
    # the measure: Root (RL origin) to the farthest vertex whose largest weight is MIDDLE_F_TOP / UP_TOP
    dom = np.array(ORDER)[jw.argmax(1)]
    tip = np.isin(dom, ['MIDDLE_F_TOP_joint', 'MIDDLE_F_UP_TOP_joint'])
    measure = float(np.linalg.norm(pos0[tip], axis=1).max())
    return dict(P=P, loc_t=loc_t, loc_r=loc_r, W0=W0, pos=pos0, nrm=nrm0, uv=m['uv'], tris=m['idx'].reshape(-1, 3),
                jw=jw, measure=measure, ratio=ratio)

# --------------------------------------------------------------------------- the FBX (ASCII 7.4)

TICKS_PER_FRAME = 46186158000 // 60   # FBX time, 60 fps: exact


def fmt(a, f='%.9g'):
    return ','.join(f % x for x in a)


class Writer:
    def __init__(self):
        self.objects = []
        self.conns = []
        self.next = 1000000

    def uid(self):
        self.next += 1
        return self.next

    def obj(self, s):
        self.objects.append(s)

    def oo(self, child, parent):
        self.conns.append('\tC: "OO",%d,%d' % (child, parent))

    def op(self, child, parent, prop):
        self.conns.append('\tC: "OP",%d,%d,"%s"' % (child, parent, prop))


def arr(name, values, f='%.9g', ints=False):
    vals = ','.join(str(int(v)) for v in values) if ints else fmt(values, f)
    return '\t\t%s: *%d {\n\t\t\ta: %s\n\t\t} \n' % (name, len(values), vals)


def mat_cols(M):
    return [M[r, c] for c in range(4) for r in range(4)]   # column-major


def write_hand(w, mesh_name, hand, root_trs, side, nfr, layer, mat_id):
    # Root_joint's own transform: hand_final's Root world at rest (Z up), the left's -1 X scale with it
    RootM = trs(root_trs)
    S = np.array(root_trs[7:10])
    Rroot = RootM[:3, :3] / S[None, :]
    files = {}
    for n in ORDER:
        files[n] = RootM @ hand['W0'][n]
    ids = {}
    for n in ORDER:
        mid, aid = w.uid(), w.uid()
        ids[n] = mid
        if n == 'Root_joint':
            t, e, s = root_trs[0:3], euler_xyz(Rroot), S
        else:
            t, e, s = hand['loc_t'][n], euler_xyz(hand['loc_r'][n][0]), np.ones(3)
        w.obj('\tNodeAttribute: %d, "NodeAttribute::%s", "LimbNode" {\n\t\tTypeFlags: "Skeleton"\n\t}\n' % (aid, n))
        w.obj('\tModel: %d, "Model::%s", "LimbNode" {\n\t\tVersion: 232\n\t\tProperties70:  {\n'
              '\t\t\tP: "InheritType", "enum", "", "",1\n'
              '\t\t\tP: "Lcl Translation", "Lcl Translation", "", "A",%s\n'
              '\t\t\tP: "Lcl Rotation", "Lcl Rotation", "", "A",%s\n'
              '\t\t\tP: "Lcl Scaling", "Lcl Scaling", "", "A",%s\n'
              '\t\t}\n\t\tShading: Y\n\t\tCulling: "CullingOff"\n\t}\n' % (mid, n, fmt(t, '%.12g'), fmt(e, '%.12g'), fmt(s, '%.12g')))
        w.oo(aid, mid)
    for n in ORDER:
        w.oo(ids[n], 0 if PARENT[n] is None else ids[PARENT[n]])

    # the take: rotation curves on HANDPALM and the 15 driven joints (every frame, Euler unwrapped)
    for n in ['HANDPALM_joint'] + DRIVEN:
        eul = np.zeros((nfr, 3))
        prev = euler_xyz(hand['loc_r'][n][0])
        for t in range(nfr):
            e = unwrap(prev, euler_xyz(hand['loc_r'][n][t]))
            eul[t] = e
            prev = e
        cn = w.uid()
        w.obj('\tAnimationCurveNode: %d, "AnimCurveNode::R", "" {\n\t\tProperties70:  {\n'
              '\t\t\tP: "d|X", "Number", "", "A",%.9g\n\t\t\tP: "d|Y", "Number", "", "A",%.9g\n\t\t\tP: "d|Z", "Number", "", "A",%.9g\n'
              '\t\t}\n\t}\n' % ((cn,) + tuple(eul[0])))
        w.oo(cn, layer)
        w.op(cn, ids[n], 'Lcl Rotation')
        times = [t * TICKS_PER_FRAME for t in range(nfr)]
        for k, ax in enumerate('XYZ'):
            cv = w.uid()
            w.obj('\tAnimationCurve: %d, "AnimCurve::", "" {\n\t\tDefault: %.9g\n\t\tKeyVer: 4009\n' % (cv, eul[0, k])
                  + arr('KeyTime', times, ints=True) + arr('KeyValueFloat', eul[:, k], '%.10g')
                  + arr('KeyAttrFlags', [260], ints=True) + arr('KeyAttrDataFloat', [0, 0, 255790911, 0], ints=True)
                  + arr('KeyAttrRefCount', [nfr], ints=True) + '\t}\n')
            w.op(cv, cn, 'd|' + ax)

    # the mesh: control points welded by position, in the file's space (the Root's transform applied)
    pos = (RootM[:3, :3] @ hand['pos'].T).T + RootM[:3, 3]
    Nm = np.linalg.inv(RootM[:3, :3]).T
    nrm = (Nm @ hand['nrm'].T).T
    nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)
    key = {}
    cp_of = np.zeros(len(pos), int)
    cps = []
    for v, p in enumerate(pos):
        k = tuple(np.round(p, 7))
        if k not in key:
            key[k] = len(cps)
            cps.append(v)
        cp_of[v] = key[k]
    # a welded point must carry one set of weights
    for v in range(len(pos)):
        assert np.abs(hand['jw'][v] - hand['jw'][cps[cp_of[v]]]).max() < 1e-6, 'welded vertices with different weights'
    tris = hand['tris']
    pvi, nrms, uvi = [], [], []
    for tri in tris:
        for c, v in enumerate(tri):
            pvi.append(cp_of[v] if c < 2 else -cp_of[v] - 1)
            nrms.extend(nrm[v])
            uvi.append(v)
    uv = hand['uv'].copy()
    uv[:, 1] = 1.0 - uv[:, 1]   # glTF v runs down, FBX v up
    gid, mid = w.uid(), w.uid()
    w.obj('\tGeometry: %d, "Geometry::%s", "Mesh" {\n' % (gid, mesh_name)
          + arr('Vertices', pos[cps].ravel(), '%.9g') + arr('PolygonVertexIndex', pvi, ints=True)
          + '\t\tGeometryVersion: 124\n'
          + '\t\tLayerElementNormal: 0 {\n\t\t\tVersion: 102\n\t\t\tName: ""\n\t\t\tMappingInformationType: "ByPolygonVertex"\n'
            '\t\t\tReferenceInformationType: "Direct"\n' + arr('Normals', nrms, '%.7g') + '\t\t}\n'
          + '\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: "map1"\n\t\t\tMappingInformationType: "ByPolygonVertex"\n'
            '\t\t\tReferenceInformationType: "IndexToDirect"\n' + arr('UV', uv.ravel(), '%.8g') + arr('UVIndex', uvi, ints=True) + '\t\t}\n'
          + '\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: ""\n\t\t\tMappingInformationType: "AllSame"\n'
            '\t\t\tReferenceInformationType: "IndexToDirect"\n' + arr('Materials', [0], ints=True) + '\t\t}\n'
          + '\t\tLayer: 0 {\n\t\t\tVersion: 100\n'
            '\t\t\tLayerElement:  {\n\t\t\t\tType: "LayerElementNormal"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n'
            '\t\t\tLayerElement:  {\n\t\t\t\tType: "LayerElementUV"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n'
            '\t\t\tLayerElement:  {\n\t\t\t\tType: "LayerElementMaterial"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t}\n\t}\n')
    w.obj('\tModel: %d, "Model::%s", "Mesh" {\n\t\tVersion: 232\n\t\tProperties70:  {\n\t\t\tP: "InheritType", "enum", "", "",1\n'
          '\t\t}\n\t\tShading: T\n\t\tCulling: "CullingOff"\n\t}\n' % (mid, mesh_name))
    w.oo(mid, 0)
    w.oo(gid, mid)
    w.oo(mat_id, mid)
    # the skin: one cluster per joint (every joint, weighted or not: the skeleton is the clusters' bones)
    sk = w.uid()
    w.obj('\tDeformer: %d, "Deformer::Skin %s", "Skin" {\n\t\tVersion: 101\n\t\tLink_DeformAcuracy: 50\n\t}\n' % (sk, mesh_name))
    w.oo(sk, gid)
    for k, n in enumerate(ORDER):
        cl = w.uid()
        wv = hand['jw'][cps, k]
        sel = np.nonzero(wv > 1e-7)[0]
        w.obj('\tDeformer: %d, "SubDeformer::Cluster %s %s", "Cluster" {\n\t\tVersion: 100\n\t\tUserData: "", ""\n' % (cl, mesh_name, n)
              + (arr('Indexes', sel, ints=True) + arr('Weights', wv[sel], '%.9g') if len(sel) else '')
              + arr('Transform', mat_cols(np.linalg.inv(files[n])), '%.12g') + arr('TransformLink', mat_cols(files[n]), '%.12g') + '\t}\n')
        w.oo(cl, sk)
        w.oo(ids[n], cl)
    return ids


def main():
    sys.argv = [a for a in sys.argv if a != '--no-thumb-contact']
    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(2)
    take = json.load(open(sys.argv[1]))
    gdir = sys.argv[2].rstrip('/')
    skin = sys.argv[3].replace('\\', '/').split('/')[-1]
    out = sys.argv[4]
    report = {}
    hands = []
    for side, (mesh_name, gfile) in enumerate([('Hand_Low', 'hand_r.gltf'), ('Hand_Low1', 'hand_l.gltf')]):
        old = old_hand(take[mesh_name])
        gl = load_gltf(gdir + '/model/' + gfile)
        rep = report.setdefault(mesh_name, {})
        probe = build_hand(old, gl, side, 1.0, {}, probe=True)
        scale = TARGET_MEASURE / probe['measure']
        hand = build_hand(old, gl, side, scale, rep)
        rep['scale_glTF_metres_to_file_units'] = scale
        rep['measure'] = hand['measure']
        rep['frames'] = len(old['frames'])
        hands.append((mesh_name, hand, take[mesh_name]['rest'][0], side, len(old['frames'])))
        print('%s: glTF metre -> %.6f file units (x%.6f the Godot size at 0.11094212 m a unit); measure %.6f; palm pivot %.3f of wrist->knuckle'
              % (mesh_name, scale, scale * 0.11094212, hand['measure'], rep['palm_pivot_fraction_along']))
    w = Writer()
    stack, layer = w.uid(), w.uid()
    nfr = hands[0][4]
    stop = (nfr - 1) * TICKS_PER_FRAME
    w.obj('\tAnimationStack: %d, "AnimStack::Take 001", "" {\n\t\tProperties70:  {\n\t\t\tP: "LocalStop", "KTime", "Time", "",%d\n'
          '\t\t\tP: "ReferenceStop", "KTime", "Time", "",%d\n\t\t}\n\t}\n' % (stack, stop, stop))
    w.obj('\tAnimationLayer: %d, "AnimLayer::BaseLayer", "" {\n\t}\n' % layer)
    w.oo(layer, stack)
    tex = w.uid()
    w.obj('\tTexture: %d, "Texture::%s", "" {\n\t\tType: "TextureVideoClip"\n\t\tVersion: 202\n\t\tTextureName: "Texture::%s"\n'
          '\t\tFileName: "%s"\n\t\tRelativeFilename: "%s"\n\t}\n' % (tex, skin, skin, skin, skin))
    for mesh_name, hand, root_trs, side, n in hands:
        mat = w.uid()
        w.obj('\tMaterial: %d, "Material::skin_%s", "" {\n\t\tVersion: 102\n\t\tShadingModel: "lambert"\n\t\tMultiLayer: 0\n'
              '\t\tProperties70:  {\n\t\t\tP: "DiffuseColor", "Color", "", "A",1,1,1\n\t\t}\n\t}\n' % (mat, mesh_name))
        w.op(tex, mat, 'DiffuseColor')
        write_hand(w, mesh_name, hand, root_trs, side, n, layer, mat)
    with open(out, 'w', newline='\n') as f:
        f.write('; FBX 7.4.0 project file\n; The Godot XR Tools hands (CC0, DigitalN8m4r3, made with MakeHuman CE) with the poses of hand_final.fbx,\n'
                '; written by VRealms tools/hand_retarget/retarget_godot.py\n')
        f.write('FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: "VRealms retarget_godot.py"\n}\n')
        f.write('GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n'
                '\t\tP: "UpAxis", "int", "Integer", "",2\n\t\tP: "UpAxisSign", "int", "Integer", "",1\n'
                '\t\tP: "FrontAxis", "int", "Integer", "",1\n\t\tP: "FrontAxisSign", "int", "Integer", "",-1\n'
                '\t\tP: "CoordAxis", "int", "Integer", "",0\n\t\tP: "CoordAxisSign", "int", "Integer", "",1\n'
                '\t\tP: "OriginalUpAxis", "int", "Integer", "",2\n\t\tP: "OriginalUpAxisSign", "int", "Integer", "",1\n'
                '\t\tP: "UnitScaleFactor", "double", "Number", "",1\n\t\tP: "OriginalUnitScaleFactor", "double", "Number", "",1\n'
                '\t\tP: "TimeMode", "enum", "", "",3\n\t\tP: "TimeSpanStart", "KTime", "Time", "",0\n'
                '\t\tP: "TimeSpanStop", "KTime", "Time", "",%d\n\t\tP: "CustomFrameRate", "double", "Number", "",60\n\t}\n}\n' % stop)
        f.write('Objects:  {\n')
        for o in w.objects:
            f.write(o)
        f.write('}\nConnections:  {\n')
        f.write('\n'.join(w.conns))
        f.write('\n}\nTakes:  {\n\tCurrent: "Take 001"\n\tTake: "Take 001" {\n\t\tFileName: "Take_001.tak"\n'
                '\t\tLocalTime: 0,%d\n\t\tReferenceTime: 0,%d\n\t}\n}\n' % (stop, stop))
    if len(sys.argv) > 5:
        json.dump(report, open(sys.argv[5], 'w'), indent=1)
    print('wrote', out)


if __name__ == '__main__':
    main()
