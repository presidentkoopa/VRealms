"""film_build.py -- item models built from the item's own close-up film, for the items the image-to-3D generator
cannot make: cut gems (it rounds every facet off) and glass (it paints the glass opaque, with whatever was behind
it baked in). Owner, 2026-11-23, of the chapter 1-2 batch: "the green vial is supposed to be glass, half full of a
glowing green liquid ... the red gem needs tightened up ... all the crystals seem bad".

THE SOURCE. extract.py has already cut every frame of the item's close-up film (DBASE300, 20 frames) into
img3d_work/<item>/film_NNN.tga, the original's ground (index 0) black. Most of these films turn the item a full
circle about the vertical in 20 frames, 18 degrees a frame (seen: frame k and frame k+10 are each other's mirror
about one column, the half-turn check below). Three recipes (items.json "film"):

  "hull"   the 20 outlines carve the item's shape (a visual hull: a voxel stays only where every frame shows the
           item). The film's pixel colours sort the pixels into the item's parts ("parts": glass, liquid, cork, ...);
           each part is carved from its own pixels, so the liquid is the liquid's own shape, seen through the glass.
           "gem": true flattens the carved shape into cut facets (below).
  "crystal" a long natural crystal: a prism of `sides` faces with pointed ends, its length and thickness measured
           from the film's frames (the longest and the thickest outline). For a film that tumbles the item.
  "cut"    a cut gem from ONE outline (a film that does not turn): the outline's corners give the girdle (`sides`,
           the corners left once nearly straight corners are merged), the table and pavilion a standard cut's.

FACETS ("gem"): the carved shape's convex hull, its faces grouped by direction (within `facet_deg`), each group one
flat plane touching the hull; the gem is where every plane's inside meets. A cut gem is convex, so nothing of its
shape is lost but the carving's stair-steps.

THE MATERIALS (glbio.write_glb_parts; the engine: roth_img3d_core.h THE PARTS): each part's colour is the median
of its own pixels (the brighter half for a gem, which the film darkens with its single light); "blend" parts are
drawn see-through at the colour's alpha, "emissive" parts glow (fullbright). Opacities and the glow are TUNING
NUMBERS (items.json), not the game's: the original's film is a pre-rendered picture and has no materials.

Output: <out>/<item>_<name>.glb and the item's manifest entry (post.py's fields: the same "metres" / grip rules), the
manifest merged as post.py merges it. Writes a preview (front, side, above) beside the work folder's frames.

  python film_build.py 37 103 35 249 143 206
"""
import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image

import img3d_common as C
import glbio

STEP_DEG = 18.0            # 20 frames, a full turn (the half-turn check)


def slug(s):
    import re
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_") or "item"   # as post.py names its files


# --------------------------------------------------------------------------- the film

def load_film(work, item):
    d = os.path.join(work, str(item))
    frames = []
    for k in range(64):
        p = os.path.join(d, "film_%03d.tga" % k)
        if not os.path.exists(p):
            break
        frames.append(np.asarray(Image.open(p).convert("RGB")).astype(np.float32))
    if not frames:
        raise SystemExit("film_build: item %d has no film frames in %s (run extract.py first)" % (item, d))
    return frames


def half_turn_axis(masks):
    """The column the film turns about: frame k mirrored about it is frame k+10 (a half turn). Returns (axis, the
    mismatch: 0 = every pair a perfect mirror)."""
    H, W = masks[0].shape
    half = len(masks) // 2
    best = None
    xs = np.arange(W)
    for a2 in range(W // 2, 3 * W // 2):
        a = a2 / 2.0   # half-pixel steps over the middle half of the frame
        xm = np.round(2 * a - xs).astype(int)
        ok = (xm >= 0) & (xm < W)
        err = n = 0
        for k in range(half):
            A, B = masks[k], masks[k + half]
            Bm = np.zeros_like(B)
            Bm[:, ok] = B[:, xm[ok]]
            err += (A ^ Bm).sum()
            n += (A | Bm).sum()
        r = err / max(n, 1)
        if best is None or r < best[1]:
            best = (a, r)
    return best


def carve(masks, axis, elev_deg, res=1.0):
    """The visual hull: voxels (x across, y the image row, z toward the viewer at frame 0) every frame shows."""
    H, W = masks[0].shape
    R = max(axis, W - axis) + 1
    n = int(2 * R / res) + 1
    xs = np.arange(n) * res - R
    ys = np.arange(0, H, res)
    X, Y, Z = np.meshgrid(xs, ys, xs, indexing="ij")
    keep = np.ones(X.shape, bool)
    e = math.radians(elev_deg)
    cy = H / 2.0
    for k, m in enumerate(masks):
        t = math.radians(STEP_DEG * k)
        xr = X * math.cos(t) + Z * math.sin(t)
        zr = -X * math.sin(t) + Z * math.cos(t)
        u = np.round(axis + xr).astype(int)
        v = np.round(cy + (Y - cy) * math.cos(e) + zr * math.sin(e)).astype(int)
        ok = (u >= 0) & (u < W) & (v >= 0) & (v < H)
        hit = np.zeros(X.shape, bool)
        hit[ok] = m[v[ok], u[ok]]
        keep &= hit
    return keep, xs, ys


def to_item_axes(P, H):
    """Carve coordinates (x across, y image row DOWN, z toward the viewer) -> glTF (x right, +y up, +z front)."""
    Q = np.empty_like(P)
    Q[:, 0] = P[:, 0]
    Q[:, 1] = H - P[:, 1]
    Q[:, 2] = P[:, 2]
    return Q


# --------------------------------------------------------------------------- meshes

def surface(keep, xs, ys, smooth=1.0):
    """A smooth closed surface round the carved voxels (marching cubes on the blurred volume)."""
    from scipy.ndimage import gaussian_filter
    from skimage.measure import marching_cubes
    vol = np.pad(keep.astype(np.float32), 2)
    if smooth > 0:
        vol = gaussian_filter(vol, smooth)
    verts, faces, normals, _ = marching_cubes(vol, 0.5)
    res = xs[1] - xs[0]
    verts = (verts - 2) * res
    P = np.c_[xs[0] + verts[:, 0], ys[0] + verts[:, 1], xs[0] + verts[:, 2]]
    return P, faces.astype(np.int64)


def facets(points, facet_deg):
    """A cut gem round `points`: the convex hull's faces grouped into planes (within facet_deg of the group's
    direction, largest faces first), the planes' common inside. Returns (vertices, triangles, flat normals) with
    every facet's corners its own (flat shading)."""
    from scipy.spatial import ConvexHull, HalfspaceIntersection
    hull = ConvexHull(points)
    centre = points[hull.vertices].mean(0)
    tri = hull.simplices
    a, b, c = points[tri[:, 0]], points[tri[:, 1]], points[tri[:, 2]]
    cr = np.cross(b - a, c - a)
    area = np.linalg.norm(cr, axis=1) * 0.5
    nrm = cr / (2 * area[:, None] + 1e-12)
    flip = ((a - centre) * nrm).sum(1) < 0
    nrm[flip] *= -1
    cos_t = math.cos(math.radians(facet_deg))
    groups = []                       # [normal sum (area weighted), area]
    for f in np.argsort(-area):
        for g in groups:
            gn = g[0] / np.linalg.norm(g[0])
            if gn @ nrm[f] >= cos_t:
                g[0] += nrm[f] * area[f]
                g[1] += area[f]
                break
        else:
            groups.append([nrm[f] * area[f], area[f]])
    planes = []
    hv = points[hull.vertices]
    for gsum, _ in groups:
        n = gsum / np.linalg.norm(gsum)
        d = (hv @ n).max()
        planes.append((n, d))
    hs = np.array([[n[0], n[1], n[2], -d] for n, d in planes])
    inter = HalfspaceIntersection(hs, centre)
    V = inter.intersections
    verts, tris, norms = [], [], []
    scale = np.ptp(points, axis=0).max()
    for n, d in planes:
        on = np.abs(V @ n - d) < 1e-4 * scale
        F = V[on]
        if len(F) < 3:
            continue
        # unique corners, ordered round the facet
        F = np.unique(np.round(F / (1e-6 * scale)).astype(np.int64), axis=0) * (1e-6 * scale)
        if len(F) < 3:
            continue
        m = F.mean(0)
        u = F[0] - m
        u /= np.linalg.norm(u)
        w = np.cross(n, u)
        ang = np.arctan2((F - m) @ w, (F - m) @ u)
        F = F[np.argsort(ang)]
        base = len(verts)
        verts.extend(F.tolist())
        norms.extend([n.tolist()] * len(F))
        for k in range(1, len(F) - 1):
            tris.append([base, base + k, base + k + 1])
    return np.array(verts), np.array(tris, np.int64), np.array(norms), len(planes)


def smooth_normals(P, T):
    N = np.zeros_like(P)
    cr = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    for k in range(3):
        np.add.at(N, T[:, k], cr)
    N /= np.linalg.norm(N, axis=1, keepdims=True) + 1e-12
    return N


def outward(P, T):
    """Wind every triangle counter-clockwise seen from outside (the engine culls the backs of see-through parts)."""
    c = P.mean(0)
    cr = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    mid = (P[T[:, 0]] + P[T[:, 1]] + P[T[:, 2]]) / 3
    bad = ((mid - c) * cr).sum(1) < 0
    T = T.copy()
    T[bad] = T[bad][:, [0, 2, 1]]
    return T


def flat_wind(V, T, N):
    cr = np.cross(V[T[:, 1]] - V[T[:, 0]], V[T[:, 2]] - V[T[:, 0]])
    bad = (cr * N[T[:, 0]]).sum(1) < 0
    T = T.copy()
    T[bad] = T[bad][:, [0, 2, 1]]
    return T


# --------------------------------------------------------------------------- the recipes

def classify(frame, part):
    """The pixels of `part` (items.json "parts"[k]["pixels"]): a list of rules over r g b, all must hold."""
    r, g, b = frame[..., 0], frame[..., 1], frame[..., 2]
    m = (r + g + b) > 0
    env = {"r": r, "g": g, "b": b, "np": np}
    for rule in part.get("pixels", []):
        m &= eval(rule, {"__builtins__": {}}, env)
    return m


def part_colour(frames, masks, brighter=False):
    px = np.concatenate([f[m] for f, m in zip(frames, masks)])
    if len(px) == 0:
        return [0.5, 0.5, 0.5]
    if brighter:
        lum = px.sum(1)
        px = px[lum >= np.median(lum)]
    return (np.median(px, axis=0) / 255.0).round(4).tolist()


def build_hull(item, frames, spec, log):
    from scipy.ndimage import binary_closing, binary_fill_holes, binary_erosion
    # The item's outline: everything not the film's ground, holes filled (dark glass and its black reflections read
    # as the ground's black, and would carve rings into the glass).
    all_masks = [binary_fill_holes(f.sum(2) > 0) for f in frames]
    axis, mis = half_turn_axis(all_masks)
    log("item %d: the film turns about column %.1f (half-turn mismatch %.3f: 0 = perfect mirrors)" % (item, axis, mis))
    elev = float(spec.get("elevation_deg", 10.0))
    H = frames[0].shape[0]
    parts = []
    res = float(spec.get("voxel", 1.0))
    outer = None
    for p in spec["parts"]:
        masks = [binary_closing(classify(f, p), iterations=1) if p.get("pixels") else m for f, m in zip(frames, all_masks)]
        keep, xs, ys = carve(masks, axis, elev, res=res)
        if keep.sum() < 20:
            log("item %d: part %s: nothing carved -- left out" % (item, p["name"]))
            continue
        if outer is None:
            outer = keep
        if p.get("fill"):
            # A liquid (or what fills a bottle) is the bottle's inside up to a level: its own carving gives the level
            # (the highest few percent of it, so a stray pixel does not raise it), the glass's inside the shape, so
            # the surface is flat and the liquid touches the glass, as the film shows it.
            rows = np.nonzero(keep.any(axis=(0, 2)))[0]
            counts = keep.sum(axis=(0, 2))[rows]
            cum = np.cumsum(counts) / counts.sum()
            top = rows[np.searchsorted(cum, float(p.get("level_pct", 0.03)))]
            bottom = rows.max()
            inside = binary_erosion(outer, iterations=max(1, int(round(float(p.get("wall_px", 1.5)) / res))))
            fill = np.zeros_like(keep)
            fill[:, top:bottom + 1, :] = inside[:, top:bottom + 1, :]
            log("item %d: part %s: filled to %.0f%% of the bottle's height (row %.0f of %.0f..%.0f)" % (item, p["name"],
                100.0 * (bottom - top) / max(1, np.ptp(np.nonzero(outer.any(axis=(0, 2)))[0])), ys[top],
                ys[np.nonzero(outer.any(axis=(0, 2)))[0].min()], ys[np.nonzero(outer.any(axis=(0, 2)))[0].max()]))
            keep = fill
        X, Y, Z = np.meshgrid(xs, ys, xs, indexing="ij")
        pts = np.c_[X[keep], Y[keep], Z[keep]]
        if p.get("color"):
            colour = p["color"]
        elif p.get("colour_of") == "rest":
            # the part's own colour from the pixels no other part claims (the glass: what is not liquid or cork)
            others = [q for q in spec["parts"] if q is not p and q.get("pixels")]
            rest = [(f.sum(2) > 0) & ~np.any([classify(f, q) for q in others], axis=0) if others else (f.sum(2) > 0) for f in frames]
            colour = part_colour(frames, rest, brighter=True)
        else:
            colour = part_colour(frames, masks, brighter=bool(spec.get("gem")))
        if spec.get("gem") and p.get("lift"):
            mx = max(colour) or 1.0
            colour = [round(c * float(p["lift"]) / mx, 4) for c in colour]
        if p.get("tint"):
            # see-through glass: the film's dark glass colour lifted so its brightest channel is `tint`
            mx = max(colour) or 1.0
            colour = [round(c * float(p["tint"]) / mx, 4) for c in colour]
        if spec.get("gem"):
            Ps, _ = surface(keep, xs, ys, smooth=0.6)
            V, T, N, nplanes = facets(Ps, float(spec.get("facet_deg", 18)))
            V = to_item_axes(V, H)
            N = N * np.array([1, -1, 1])
            T = flat_wind(V, T, N)
            log("item %d: part %s: %d voxels -> %d facets" % (item, p["name"], keep.sum(), nplanes))
        else:
            V, T = surface(keep, xs, ys, smooth=float(p.get("smooth", 1.0)))
            V = to_item_axes(V, H)
            T = outward(V, T)
            N = smooth_normals(V, T)
            log("item %d: part %s: %d voxels -> %d triangles" % (item, p["name"], keep.sum(), len(T)))
        parts.append(dict(V=V, T=T, N=N, mat=material(p, colour)))
    return parts


def lift(p, colour):
    """A gem's colour: the film lights it with one dim lamp; `lift` raises its brightest channel to that value."""
    if not p.get("lift"):
        return colour
    mx = max(colour) or 1.0
    return [round(c * float(p["lift"]) / mx, 4) for c in colour]


def material(p, colour):
    a = float(p.get("opacity", 1.0))
    glow = float(p.get("glow", 0.0))
    return {"name": p["name"], "color": [colour[0], colour[1], colour[2], a], "blend": a < 1.0,
            "emissive": [colour[0] * glow, colour[1] * glow, colour[2] * glow] if glow > 0 else [0, 0, 0],
            "roughness": float(p.get("roughness", 1.0))}


def outline_long_thick(mask):
    ys, xs = np.nonzero(mask)
    P = np.c_[xs, ys].astype(float)
    c = P.mean(0)
    _, _, Vt = np.linalg.svd(P - c, full_matrices=False)
    pr = (P - c) @ Vt.T
    return np.ptp(pr[:, 0]), np.ptp(pr[:, 1])


def build_crystal(item, frames, spec, log):
    masks = [f.sum(2) > 0 for f in frames]
    lt = [outline_long_thick(m) for m in masks]
    L = max(l for l, _ in lt)
    Wd = float(np.median([t for l, t in lt if l > 0.9 * L]))
    sides = int(spec.get("sides", 6))
    tips = spec.get("tips", [0.16, 0.10])          # each end's point, a fraction of the length
    flat = float(spec.get("flatten", 0.85))        # the cross-section's height over its width
    log("item %d: crystal %d sides, %.0f px long, %.0f px thick (the film's longest and thickest outline)" % (item, sides, L, Wd))
    rng = np.random.RandomState(item)
    pts = []
    for zf in (-0.5 + tips[0], 0.5 - tips[1]):
        for k in range(sides):
            a = 2 * math.pi * (k + 0.5) / sides
            j = 1.0 + rng.uniform(-0.06, 0.06)
            pts.append([0.5 * Wd * math.cos(a) * j, 0.5 * Wd * flat * math.sin(a) * j, zf * L])
    pts.append([0.08 * Wd, 0.05 * Wd, -0.5 * L])
    pts.append([-0.05 * Wd, -0.04 * Wd, 0.5 * L])
    P = np.array(pts)
    # lying along x (the film shows it long and level), up +y
    P = P[:, [2, 1, 0]]
    V, T, N, nplanes = facets(P, 1.0)
    T = flat_wind(V, T, N)
    colour = lift(spec["parts"][0], spec.get("color") or part_colour(frames, masks, brighter=True))
    return [dict(V=V, T=T, N=N, mat=material(spec["parts"][0], colour))]


def build_cut(item, frames, spec, log):
    from scipy.spatial import ConvexHull
    m = frames[0].sum(2) > 0
    ys, xs = np.nonzero(m)
    P = np.c_[xs, ys].astype(float)
    hv = P[ConvexHull(P).vertices]
    corners = merge_corners(hv, float(spec.get("corner_deg", 25)))
    sides = len(corners)
    log("item %d: the outline has %d corners" % (item, sides))
    c = corners.mean(0)
    rel = corners - c
    # The film looks down on it at a slant: stretch the outline's height until its corners are most even round the
    # middle (a cut gem's girdle is regular).
    best = None
    for s in np.linspace(1.0, 4.0, 61):
        q = rel * np.array([1, s])
        r = np.linalg.norm(q, axis=1)
        sc = r.std() / r.mean()
        if best is None or sc < best[0]:
            best = (sc, s)
    girdle = rel * np.array([1, best[1]])
    log("item %d: seen from %.0f degrees above (the slant that makes the %d corners even)" % (item, math.degrees(math.asin(min(1.0, 1.0 / best[1]))), sides))
    Dm = 2 * np.linalg.norm(girdle, axis=1).mean()
    cut = spec.get("cut", {})
    table, crown, pav, gird = cut.get("table", 0.6), cut.get("crown", 0.14), cut.get("pavilion", 0.43), cut.get("girdle", 0.02)
    pts = []
    for (x, y) in girdle:
        pts.append([x, gird * Dm / 2, y])
        pts.append([x, -gird * Dm / 2, y])
        pts.append([x * table, crown * Dm, y * table])
        pts.append([x * 0.55, -0.55 * pav * Dm, y * 0.55])
    pts.append([0, -pav * Dm, 0])
    P = np.array(pts)
    V, T, N, nplanes = facets(P, 1.0)
    T = flat_wind(V, T, N)
    colour = lift(spec["parts"][0], spec.get("color") or part_colour(frames, [m], brighter=True))
    log("item %d: cut gem, %d facets, %.0f px across" % (item, nplanes, Dm))
    return [dict(V=V, T=T, N=N, mat=material(spec["parts"][0], colour))]


def merge_corners(hv, deg):
    V = [np.array(p) for p in hv]
    while len(V) > 3:
        n = len(V)
        best = None
        for i in range(n):
            a, b, c = V[i - 1], V[i], V[(i + 1) % n]
            u, w = b - a, c - b
            if np.linalg.norm(u) < 1e-9 or np.linalg.norm(w) < 1e-9:
                best = (0, i)
                break
            ang = math.degrees(math.acos(np.clip(u @ w / np.linalg.norm(u) / np.linalg.norm(w), -1, 1)))
            if best is None or ang < best[0]:
                best = (ang, i)
        if best[0] > deg:
            break
        V.pop(best[1])
    return np.array(V)


# --------------------------------------------------------------------------- out

def finish(item, info, parts, out, log):
    allV = np.concatenate([p["V"] for p in parts])
    lo, hi = allV.min(0), allV.max(0)
    centre = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
    longest = (hi - lo).max()
    metres = info.get("metres")
    k = float(metres or 0.10) / longest
    for p in parts:
        p["V"] = (p["V"] - centre) * k
    ext = (hi - lo) * k
    gf = info.get("grip_frac", [0.5, 0.0, 0.5])
    go = info.get("grip_offset", [0, -0.025, 0])
    grip = [(-0.5 + gf[0]) * ext[0] + go[0], gf[1] * ext[1] + go[1], (-0.5 + gf[2]) * ext[2] + go[2]]
    name = "%d_%s.glb" % (item, slug(info["name"]))
    glb = []
    for p in parts:
        glb.append({"positions": p["V"].ravel().tolist(), "normals": p["N"].ravel().tolist(), "uvs": None,
                    "indices": p["T"].ravel().tolist(), "material": p["mat"]})
    glbio.write_glb_parts(os.path.join(out, name), glb, name=info["name"])
    log("item %d: %s, %.3f m (%s), parts %s" % (item, name, float(ext.max()), "table" if metres else "picture",
        ", ".join("%s%s%s" % (p["mat"]["name"], " see-through %.2f" % p["mat"]["color"][3] if p["mat"]["blend"] else "",
                               " glowing" if any(p["mat"]["emissive"]) else "") for p in parts)))
    return {"item": item, "name": info["name"], "file": name, "metres": round(float(ext.max()), 5),
            "size": "table" if metres else "picture", "grip": [round(g, 5) for g in grip],
            "grip_rot": info.get("grip_rot", [0, 0, 0]), "hold": info.get("hold", "palm"),
            "world_yaw": info.get("world_yaw", 0), "faces": int(sum(len(p["T"]) for p in parts)),
            "source": {"view": "film_000.tga .. film_019.tga", "why": "film_build.py (%s)" % info["film"]["recipe"],
                       "prepared": "carved from the close-up film"}}


def preview(parts, path):
    """Front, side, above: flat shaded, see-through parts drawn over the rest."""
    W = 300
    views = [np.array([0, 0, -1.0]), np.array([-1.0, 0, 0]), np.array([-0.7, -0.6, -0.7])]
    out = Image.new("RGB", (W * 3, W), (32, 32, 32))
    allV = np.concatenate([p["V"] for p in parts])
    span = np.ptp(allV, axis=0).max() * 1.15
    order = sorted(range(len(parts)), key=lambda i: parts[i]["mat"]["blend"])
    for vi, vd in enumerate(views):
        vd = vd / np.linalg.norm(vd)
        up = np.array([0, 1.0, 0]) - vd * vd[1]
        up /= np.linalg.norm(up)
        rt = np.cross(vd, up)
        img = np.full((W, W, 3), 0.125, np.float32)
        zb = np.full((W, W), np.inf)
        c = allV.mean(0)
        L = -vd + 0.5 * up + 0.3 * rt
        L /= np.linalg.norm(L)
        for pi in order:
            p = parts[pi]
            col = np.array(p["mat"]["color"][:3])
            a = p["mat"]["color"][3]
            glow = any(p["mat"]["emissive"])
            V, T, N = p["V"], p["T"], p["N"]
            X = ((V - c) @ rt) / span * W + W / 2
            Y = W / 2 - ((V - c) @ up) / span * W
            Z = (V - c) @ vd
            for t in T:
                xs, ys = X[t], Y[t]
                x0, x1 = max(int(xs.min()), 0), min(int(xs.max()) + 1, W - 1)
                y0, y1 = max(int(ys.min()), 0), min(int(ys.max()) + 1, W - 1)
                if x0 > x1 or y0 > y1:
                    continue
                d = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0])
                if abs(d) < 1e-9:
                    continue
                n = N[t].mean(0)
                if p["mat"]["blend"] and n @ vd > 0:
                    continue   # the engine culls a see-through part's backs
                gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
                w1 = ((gx - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (gy - ys[0])) / d
                w2 = ((xs[1] - xs[0]) * (gy - ys[0]) - (gx - xs[0]) * (ys[1] - ys[0])) / d
                w0 = 1 - w1 - w2
                m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
                z = w0 * Z[t[0]] + w1 * Z[t[1]] + w2 * Z[t[2]]
                sub = zb[y0:y1 + 1, x0:x1 + 1]
                m &= z < sub
                if not m.any():
                    continue
                sh = 1.0 if glow else abs(n @ L) * 0.8 + 0.25
                region = img[y0:y1 + 1, x0:x1 + 1]
                if p["mat"]["blend"]:
                    region[m] = region[m] * (1 - a) + col * sh * a
                else:
                    region[m] = col * sh
                    sub[m] = z[m]
        out.paste(Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)), (vi * W, 0))
    out.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("items", nargs="+", type=int)
    ap.add_argument("--work", default=C.WORK_DIR)
    ap.add_argument("--out", default=C.OUT_DIR)
    a = ap.parse_args()
    C.keep_everything_on_root()
    a.out = C.outside_repo(a.out)
    a.work = C.outside_repo(a.work)
    os.makedirs(a.out, exist_ok=True)
    log = print
    entries = []
    for item in a.items:
        info = C.item_info(item)
        if "film" not in info:
            print("film_build: item %d has no \"film\" recipe in items.json -- skipped" % item)
            continue
        spec = info["film"]
        src = spec.get("same_as", item)
        frames = load_film(a.work, src)
        recipe = spec["recipe"]
        parts = {"hull": build_hull, "crystal": build_crystal, "cut": build_cut}[recipe](item, frames, spec, log)
        if not parts:
            print("film_build: item %d: nothing built" % item)
            continue
        e = finish(item, info, parts, a.out, log)
        preview(parts, os.path.join(a.work, str(item), "film_build.png"))
        entries.append(e)
    if entries:
        mpath = os.path.join(a.out, "manifest.json")
        man = json.load(open(mpath, encoding="utf-8")) if os.path.exists(mpath) else {"version": 1, "items": []}
        keep = [x for x in man.get("items", []) if x.get("item") not in {e["item"] for e in entries}]
        man["items"] = sorted(keep + entries, key=lambda x: x["item"])
        man["generator"] = man.get("generator", "tools/img3d")
        with open(mpath, "w", encoding="utf-8") as f:
            json.dump(man, f, indent=1)
        print("manifest: %s, %d item(s)" % (mpath, len(man["items"])))


if __name__ == "__main__":
    main()
