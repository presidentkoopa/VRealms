#!/usr/bin/env python3
"""A small software rasteriser for the meshes test_fbxload --dump writes: what the loader's output looks like,
textured, from the front and the back, without the engine. Not the engine's renderer: it shows the mesh,
its UVs, its normals and its winding as the loader leaves them, nothing about how the engine lights them.

    python3 -I render.py DUMPDIR TEXTURE.png OUT.png name [name...]

Each name is a dump file stem (Hand_Low_rest, Hand_Low1_500, ...). One row per name: view from -Y (the file's
front, +Z up), then from +Y; each tile lit by a light from the viewer with the vertex normals, textured with
the given texture at the vertex UVs (v already flipped by the loader). Back faces (by the IQM winding, which
is clockwise from outside) are culled, so an inside-out mesh shows its far side.
"""
import sys

import numpy as np
from PIL import Image, ImageDraw

TILE = 360


def load(p):
    with open(p) as f:
        nv, nt = map(int, f.readline().split())
        v = np.array([list(map(float, f.readline().split())) for _ in range(nv)])
        t = np.array([list(map(int, f.readline().split())) for _ in range(nt)])
    return v[:, :3], v[:, 3:6], v[:, 6:8], t


def render(P, N, UV, T, tex, view):
    # view: +1 looks along +Y (from -Y), -1 along -Y (from +Y). Screen x = world x * view, y = world z.
    sx = P[:, 0] * view
    sy = P[:, 2]
    depth = P[:, 1] * view  # smaller is nearer
    lo = np.array([sx.min(), sy.min()])
    hi = np.array([sx.max(), sy.max()])
    s = (TILE - 20) / max(hi - lo)
    X = (sx - lo[0]) * s + 10 + (TILE - 20 - (hi[0] - lo[0]) * s) / 2
    Y = TILE - ((sy - lo[1]) * s + 10)
    img = np.zeros((TILE, TILE, 3), np.float32) + 0.15
    zb = np.full((TILE, TILE), np.inf)
    th, tw = tex.shape[:2]
    light = np.array([0, -view, 0.3])
    light /= np.linalg.norm(light)
    for a, b, c in T:
        # IQM winding: (b-a)x(c-a) points INTO the surface, so a face toward the viewer is clockwise on screen
        ax, ay, bx, by, cx, cy = X[a], Y[a], X[b], Y[b], X[c], Y[c]
        area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
        if area <= 0:   # with screen y down and x mirrored for the back view, area > 0 is a face toward the viewer
            continue
        x0, x1 = int(max(0, np.floor(min(ax, bx, cx)))), int(min(TILE - 1, np.ceil(max(ax, bx, cx))))
        y0, y1 = int(max(0, np.floor(min(ay, by, cy)))), int(min(TILE - 1, np.ceil(max(ay, by, cy))))
        if x1 < x0 or y1 < y0:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((bx - xs) * (cy - ys) - (by - ys) * (cx - xs)) / area
        w1 = ((cx - xs) * (ay - ys) - (cy - ys) * (ax - xs)) / area
        w2 = 1 - w0 - w1
        m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not m.any():
            continue
        z = w0 * depth[a] + w1 * depth[b] + w2 * depth[c]
        iy, ix = (ys - 0.5).astype(int), (xs - 0.5).astype(int)
        m &= z < zb[iy, ix]
        if not m.any():
            continue
        iy, ix, z = iy[m], ix[m], z[m]
        ww = np.stack([w0[m], w1[m], w2[m]], 1)
        uv = ww @ UV[[a, b, c]]
        n = ww @ N[[a, b, c]]
        n /= np.linalg.norm(n, axis=1)[:, None] + 1e-12
        lam = np.clip(n @ light, 0, 1) * 0.8 + 0.2
        tu = np.clip((uv[:, 0] % 1.0) * tw, 0, tw - 1).astype(int)
        tv = np.clip((uv[:, 1] % 1.0) * th, 0, th - 1).astype(int)
        col = tex[tv, tu, :3] / 255.0
        zb[iy, ix] = z
        img[iy, ix] = col * lam[:, None]
    return (np.clip(img, 0, 1) * 255).astype(np.uint8)


def main():
    d, texp, out = sys.argv[1], sys.argv[2], sys.argv[3]
    names = sys.argv[4:]
    tex = np.array(Image.open(texp).convert("RGB"))
    rows = []
    for nm in names:
        P, N, UV, T = load("%s/%s.txt" % (d, nm))
        a = Image.fromarray(render(P, N, UV, T, tex, +1))
        b = Image.fromarray(render(P, N, UV, T, tex, -1))
        row = Image.new("RGB", (TILE * 2, TILE + 18), (0, 0, 0))
        row.paste(a, (0, 18))
        row.paste(b, (TILE, 18))
        ImageDraw.Draw(row).text((4, 2), "%s: from -Y (front) | from +Y (back)" % nm, fill=(255, 255, 0))
        rows.append(row)
    sheet = Image.new("RGB", (TILE * 2, sum(r.height for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.height
    sheet.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
