#!/usr/bin/env python3
"""Every pose on hand_final.fbx beside the same pose on the retargeted Godot hand, rendered alike: a software
rasteriser (the same method as tools/colt_test/rend.py: orthographic, z-buffer, Lambert from the viewer, textured).

    python3 -I render_compare.py DUMPDIR OLD_TEX NEW_TEX OUTDIR

DUMPDIR is check_retarget's output: old_<mesh>_<state>.txt, new_<mesh>_<state>.txt, handframes.txt, poses.txt.
Both hands share the Root (the new file's Root_joint has hand_final's rest transform) and the hand frame at rest, so
one camera per view frames both: the views are in each hand's own frame (X the fingers, Y the back of the hand,
Z = X x Y):
    palm   looking at the palm (camera on the palm side), fingers up
    side   looking along the knuckle line (-Z), fingers up: the curl in profile
    back   looking at the back of the hand, fingers up
Writes OUTDIR/<mesh>_<state>.png (one row: old palm, new palm, old side, new side, old back, new back) and
OUTDIR/sheet_<mesh>_<n>.png contact sheets.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

T = 300   # tile size


def load(p):
    with open(p) as f:
        nv, nt = map(int, f.readline().split())
        V = np.loadtxt(f, max_rows=nv)
        Tr = np.loadtxt(f, max_rows=nt, dtype=int).reshape(-1, 3)
    return V, Tr


def render(V, Tr, tex, vd, up, centre, span):
    vd = vd / np.linalg.norm(vd)
    up = up - vd * up.dot(vd)
    up /= np.linalg.norm(up)
    rt = np.cross(vd, up)
    img = np.zeros((T, T, 3), np.float32) + 0.12
    zb = np.full((T, T), np.inf)
    s = T / span
    cx, cy = centre @ rt, centre @ up
    p = V[:, :3]
    n = V[:, 3:6]
    X = (p @ rt - cx) * s + T / 2
    Y = T / 2 - (p @ up - cy) * s
    Z = p @ vd
    L = -vd + 0.4 * up + 0.3 * rt
    L /= np.linalg.norm(L)
    th, tw = tex.shape[:2]
    for a, b, c in Tr:
        xs = np.array([X[a], X[b], X[c]])
        ys = np.array([Y[a], Y[b], Y[c]])
        x0 = max(int(np.floor(xs.min())), 0)
        x1 = min(int(np.ceil(xs.max())), T - 1)
        y0 = max(int(np.floor(ys.min())), 0)
        y1 = min(int(np.ceil(ys.max())), T - 1)
        if x0 > x1 or y0 > y1:
            continue
        d = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0])
        if abs(d) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w1 = ((gx - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (gy - ys[0])) / d
        w2 = ((xs[1] - xs[0]) * (gy - ys[0]) - (gx - xs[0]) * (ys[1] - ys[0])) / d
        w0 = 1 - w1 - w2
        m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not m.any():
            continue
        z = w0 * Z[a] + w1 * Z[b] + w2 * Z[c]
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        m &= z < sub
        if not m.any():
            continue
        sub[m] = z[m]
        nn = n[a] * w0[..., None] + n[b] * w1[..., None] + n[c] * w2[..., None]
        nn /= np.linalg.norm(nn, axis=-1, keepdims=True) + 1e-9
        sh = np.abs(nn @ L) * 0.8 + 0.2
        uv = V[[a, b, c], 6:8]
        u = w0 * uv[0, 0] + w1 * uv[1, 0] + w2 * uv[2, 0]
        v = w0 * uv[0, 1] + w1 * uv[1, 1] + w2 * uv[2, 1]
        tx = np.clip((u % 1) * (tw - 1), 0, tw - 1).astype(int)
        ty = np.clip((v % 1) * (th - 1), 0, th - 1).astype(int)
        img[y0:y1 + 1, x0:x1 + 1][m] = (tex[ty, tx] * sh[..., None])[m]
    return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))


def main():
    dump, otex, ntex, out = sys.argv[1:5]
    only = set(sys.argv[5].split(',')) if len(sys.argv) > 5 else None
    os.makedirs(out, exist_ok=True)
    texes = []
    for t in (otex, ntex):
        im = Image.open(t).convert('RGB').resize((1024, 1024))
        texes.append(np.asarray(im).astype(np.float32) / 255)
    frames = {}
    for line in open(os.path.join(dump, 'handframes.txt')):
        q = line.split()
        a = np.array(list(map(float, q[1:])))
        frames[q[0]] = (a[0:3], a[3:6], a[6:9], a[9:12])
    states = {}
    for line in open(os.path.join(dump, 'poses.txt')):
        q = line.split()
        states.setdefault(q[0], []).append((q[1], line.strip()))
    for mesh, lst in states.items():
        wrist, X, Y, Z = frames[mesh]
        views = [('palm', Y, X), ('side', -Z, X), ('back', -Y, X)]
        rows = []
        for st, info in lst:
            if only and st not in only:
                continue
            Vo, To = load(os.path.join(dump, 'old_%s_%s.txt' % (mesh, st)))
            Vn, Tn = load(os.path.join(dump, 'new_%s_%s.txt' % (mesh, st)))
            # one frame for the row: both meshes, every view
            P = np.concatenate([Vo[:, :3], Vn[:, :3]])
            centre = P.mean(0)
            span = 0
            for _, vd, up in views:
                vdn = vd / np.linalg.norm(vd)
                u = up - vdn * up.dot(vdn)
                u /= np.linalg.norm(u)
                r = np.cross(vdn, u)
                span = max(span, np.ptp(P @ r), np.ptp(P @ u))
            span *= 1.08
            row = Image.new('RGB', (T * 6, T + 18), (20, 20, 20))
            dr = ImageDraw.Draw(row)
            for k, (vn, vd, up) in enumerate(views):
                row.paste(render(Vo, To, texes[0], vd, up, centre, span), (T * 2 * k, 18))
                row.paste(render(Vn, Tn, texes[1], vd, up, centre, span), (T * (2 * k + 1), 18))
                dr.text((T * 2 * k + 4, 3), 'hand_final %s' % vn, fill=(255, 220, 120))
                dr.text((T * (2 * k + 1) + 4, 3), 'Godot %s' % vn, fill=(140, 220, 255))
            dr.text((T * 5 - 120, 3), st, fill=(255, 255, 255))
            fn = os.path.join(out, '%s_%s.png' % (mesh, st))
            row.save(fn)
            rows.append(row)
            print('wrote', fn, flush=True)
        for i in range(0, len(rows), 8):
            chunk = rows[i:i + 8]
            sheet = Image.new('RGB', (T * 6, (T + 18) * len(chunk)), (20, 20, 20))
            for k, r in enumerate(chunk):
                sheet.paste(r, (0, k * (T + 18)))
            sheet.save(os.path.join(out, 'sheet_%s_%d.png' % (mesh, i // 8)))


if __name__ == '__main__':
    main()
