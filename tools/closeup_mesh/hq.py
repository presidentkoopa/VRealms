#!/usr/bin/env python3
# Scratch (not repo): high-definition voxel model of a Realms close-up turntable.
#  1. upscale every frame S x (de-dither inside the item's mask, cubic, sharpen; mask with sub-pixel edges)
#  2. carve the visual hull at 1/S film px (coarse 1x hull first, then only inside it)
#  3. clip the unseen underside at the lowest point any frame sees
#  4. colour each surface voxel from the frames that see it, weighted hard toward the most frontal one
# usage: hq.py <dumpdir/id> <outdir> [S]
import sys, os, math, time
import numpy as np, cv2
from scipy import ndimage as ndi

src, out = sys.argv[1], sys.argv[2]
S = int(sys.argv[3]) if len(sys.argv) > 3 else 4
os.makedirs(out, exist_ok=True)
T0 = time.time()
def log(*a): print(f"[{time.time()-T0:6.1f}s]", *a, flush=True)

meta = {}
for line in open(src + "/meta.txt"):
    p = line.split()
    if p: meta[p[0]] = p[1:]
W, H, F = int(meta['w'][0]), int(meta['h'][0]), int(meta['frames'][0])
cam = dict(period=float(meta['period'][0]), dir=int(meta['direction'][0]), elev=float(meta['elevation'][0]),
           ax=float(meta['axisX'][0]), ay=float(meta['axisY'][0]), roll=float(meta['roll'][0]),
           invD=float(meta['invDistance'][0]), first=int(meta['firstFrame'][0]), last=int(meta['lastFrame'][0]))
aoff = [float(x) for x in meta.get('angleOffset', [])]
last = cam['last'] if cam['last'] >= 0 else F - 1
KS = list(range(cam['first'], last + 1))
frames = np.fromfile(src + "/frames.bin", np.uint8).reshape(F, H, W)
pal = np.fromfile(src + "/palette.bin", np.uint8).reshape(256, 3).astype(np.float32) / 255.0
masks = np.fromfile(src + "/masks.bin", np.uint8).reshape(F, H, W)

def angle(k):
    return cam['dir'] * 2 * math.pi * (k - cam['first']) / cam['period'] + (aoff[k] if k < len(aoff) else 0.0)

def project(k, x, y, z, elev=None, ang=None):
    th = angle(k) if ang is None else ang
    e = cam['elev'] if elev is None else elev
    c, s = math.cos(th), math.sin(th)
    xr = x * c - z * s; zr = x * s + z * c
    cp, sp = math.cos(e), math.sin(e)
    up = y * cp - zr * sp
    depth = y * sp + zr * cp
    right = xr
    if cam['invD'] != 0 and elev is None:
        sc = 1.0 / np.maximum(0.05, 1.0 - depth * cam['invD'])
        right = right * sc; up = up * sc
    cr, sr = math.cos(cam['roll']), math.sin(cam['roll'])
    u = cam['ax'] + right * cr - up * sr
    v = cam['ay'] - (right * sr + up * cr)
    return u, v, depth

def viewdir(k):
    th = angle(k); c, s = math.cos(th), math.sin(th)
    cp, sp = math.cos(cam['elev']), math.sin(cam['elev'])
    return np.array([s * cp, sp, c * cp], np.float32)

# ---------------------------------------------------------------- 1. upscale
def upscale(k):
    m = masks[k].astype(np.float32)
    rgb = pal[frames[k]]
    # de-dither: mask-normalised blur, so the black ground never bleeds in
    sg = 0.65
    num = cv2.GaussianBlur(rgb * m[..., None], (0, 0), sg)
    den = cv2.GaussianBlur(m, (0, 0), sg)
    dd = num / np.maximum(den, 1e-4)[..., None]
    # outside the mask: the nearest inside colour, so the cubic kernel sees no black at the edge
    if (m > 0).any():
        idx = ndi.distance_transform_edt(m == 0, return_distances=False, return_indices=True)
        dd = dd[idx[0], idx[1]]
    up = cv2.resize(dd, (W * S, H * S), interpolation=cv2.INTER_CUBIC)
    up = up + 0.6 * (up - cv2.GaussianBlur(up, (0, 0), 1.2 * S / 4))   # unsharp
    up = np.clip(up, 0, 1)
    mb = cv2.GaussianBlur(m, (0, 0), 0.55)
    mu = cv2.resize(mb, (W * S, H * S), interpolation=cv2.INTER_CUBIC) > 0.5
    return up.astype(np.float32), mu

UP, MU = {}, {}
for k in KS:
    UP[k], MU[k] = upscale(k)
log("upscaled", len(KS), "frames to", W * S, "x", H * S)
# carving masks: a little slack (sub-pixel misregistration of the fitted camera)
SLACK = max(1, S // 2)
ker = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * SLACK + 1, 2 * SLACK + 1))
MC = {k: cv2.dilate(MU[k].astype(np.uint8), ker) for k in KS}
MC1 = {k: cv2.dilate(masks[k], np.ones((3, 3), np.uint8)) for k in KS}

# ---------------------------------------------------------------- 2. carve
def carve_test(x, y, z, fine):
    keep = np.ones(x.shape, bool)
    for k in KS:
        u, v, _ = project(k, x, y, z)
        if fine:
            iu = np.floor(u * S).astype(np.int64); iv = np.floor(v * S).astype(np.int64); mk = MC[k]; ww, hh = W * S, H * S
        else:
            iu = np.floor(u).astype(np.int64); iv = np.floor(v).astype(np.int64); mk = MC1[k]; ww, hh = W, H
        ok = (iu >= 0) & (iv >= 0) & (iu < ww) & (iv < hh)
        inside = np.zeros(x.shape, bool)
        inside[ok] = mk[iv[ok], iu[ok]] > 0
        keep &= inside
    return keep

R = int(max(W, H) * 0.8) + 4
xs1 = np.arange(-R, R) + 0.5
ys1 = np.arange(-2 * H, 2 * H) + 0.5
G1 = np.zeros((len(xs1), len(ys1), len(xs1)), bool)
for i0 in range(0, len(xs1), 16):
    X, Y, Z = np.meshgrid(xs1[i0:i0 + 16], ys1, xs1, indexing='ij')
    G1[i0:i0 + 16] = carve_test(X.ravel(), Y.ravel(), Z.ravel(), False).reshape(X.shape)
nz = np.argwhere(G1)
lo, hi = nz.min(0) - 2, nz.max(0) + 3
G1 = G1[lo[0]:hi[0], lo[1]:hi[1], lo[2]:hi[2]]
x0, y0, z0 = xs1[lo[0]] - 0.5, ys1[lo[1]] - 0.5, xs1[lo[2]] - 0.5
log("coarse hull", G1.shape, int(G1.sum()), "cells")
cand1 = ndi.binary_dilation(G1, iterations=1)

h = 1.0 / S
NX, NY, NZ = (np.array(G1.shape) * S)
FG = np.zeros((NX, NY, NZ), bool)
sub = (np.arange(S) + 0.5) * h
ci = np.argwhere(cand1)
B = 4000
for b in range(0, len(ci), B):
    c = ci[b:b + B]
    # every fine voxel of these coarse cells
    I = (c[:, None, None, None, 0] * S + np.arange(S)[None, :, None, None]) + 0 * np.arange(S)[None, None, :, None] + 0 * np.arange(S)[None, None, None, :]
    J = (c[:, None, None, None, 1] * S + np.arange(S)[None, None, :, None]) + 0 * I
    L = (c[:, None, None, None, 2] * S + np.arange(S)[None, None, None, :]) + 0 * I
    I, J, L = I.ravel(), J.ravel(), L.ravel()
    keep = carve_test(x0 + (I + 0.5) * h, y0 + (J + 0.5) * h, z0 + (L + 0.5) * h, True)
    FG[I[keep], J[keep], L[keep]] = True
log("fine hull", FG.shape, int(FG.sum()), "voxels")
# keep the big parts only (specks from slack), fill closed cavities
lab, n = ndi.label(FG)
if n > 1:
    sizes = ndi.sum(FG, lab, range(1, n + 1))
    FG = np.isin(lab, 1 + np.flatnonzero(sizes >= 0.002 * sizes.max()))
FG = ndi.binary_fill_holes(FG)

def surface_of(G):
    e = ndi.binary_erosion(G, structure=ndi.generate_binary_structure(3, 1), border_value=0)
    return G & ~e

def centres(idx):
    return x0 + (idx[:, 0] + 0.5) * h, y0 + (idx[:, 1] + 0.5) * h, z0 + (idx[:, 2] + 0.5) * h

def visibility(SV):
    """per frame: bool visible for every surface voxel, plus u,v"""
    x, y, z = centres(SV)
    vis, uv = {}, {}
    for k in KS:
        u, v, d = project(k, x, y, z)
        iu = np.clip(np.floor(u * S).astype(np.int64), 0, W * S - 1); iv = np.clip(np.floor(v * S).astype(np.int64), 0, H * S - 1)
        zb = np.full(W * S * H * S, -1e9, np.float32)
        pix = iv * (W * S) + iu
        np.maximum.at(zb, pix, d.astype(np.float32))
        # a 3x3 max of the depth buffer: no holes between splats
        zb2 = ndi.maximum_filter(zb.reshape(H * S, W * S), size=3).ravel()
        vis[k] = d >= np.minimum(zb[pix], zb2[pix]) - 1.6 * h
        vis[k] &= d >= zb2[pix] - 2.5 * h
        uv[k] = (u, v)
    return vis, uv

# ---------------------------------------------------------------- 3. clip the unseen underside
SV = np.argwhere(surface_of(FG))
vis, uv = visibility(SV)
seen = np.zeros(len(SV), bool)
for k in KS: seen |= vis[k]
ylow = SV[seen, 1].min()
if cam['elev'] > math.radians(3):
    FG[:, :ylow, :] = False
    log("clipped the underside below fine row", ylow, f"(y = {y0 + ylow * h:.1f} film px)")
nz = np.argwhere(FG)
lo, hi = nz.min(0), nz.max(0) + 1
FG = FG[lo[0]:hi[0], lo[1]:hi[1], lo[2]:hi[2]]
x0, y0, z0 = x0 + lo[0] * h, y0 + lo[1] * h, z0 + lo[2] * h
SURF = surface_of(FG)
SV = np.argwhere(SURF)
log("model", FG.shape, int(FG.sum()), "voxels,", len(SV), "surface")

def sample(img, px, py):
    n = len(px); C = 1024; r = (n + C - 1) // C
    mx = np.zeros(r * C, np.float32); my = np.zeros(r * C, np.float32); mx[:n] = px; my[:n] = py
    o = cv2.remap(img, mx.reshape(r, C), my.reshape(r, C), cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    return o.reshape(-1, img.shape[2])[:n]

# ---------------------------------------------------------------- 4. colour
sm = ndi.gaussian_filter(FG.astype(np.float32), sigma=0.9 * S / 2 + 0.5)
P = np.pad(sm, 1, mode='edge')
i, j, l = SV[:, 0] + 1, SV[:, 1] + 1, SV[:, 2] + 1
N = -np.stack([P[i + 1, j, l] - P[i - 1, j, l], P[i, j + 1, l] - P[i, j - 1, l], P[i, j, l + 1] - P[i, j, l - 1]], 1)
N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-6)
del sm, P
vis, uv = visibility(SV)
acc = np.zeros((len(SV), 3), np.float64); wsum = np.zeros(len(SV))
for k in KS:
    vk = vis[k]
    w = np.clip(N @ viewdir(k), 0, 1) ** 6 * vk
    u, v = uv[k]
    col = sample(UP[k], u * S - 0.5, v * S - 0.5)
    acc += col * w[:, None]; wsum += w
has = wsum > 1e-6
colr = np.zeros((len(SV), 3), np.float32)
colr[has] = (acc[has] / wsum[has, None])
# voxels no frame sees (the cut base): the nearest seen voxel's colour
if (~has).any():
    grid = np.full(FG.shape, -1, np.int64)
    grid[SV[has, 0], SV[has, 1], SV[has, 2]] = np.flatnonzero(has)
    _, ind = ndi.distance_transform_edt(grid < 0, return_indices=True)
    q = SV[~has]
    src_i = grid[ind[0][q[:, 0], q[:, 1], q[:, 2]], ind[1][q[:, 0], q[:, 1], q[:, 2]], ind[2][q[:, 0], q[:, 1], q[:, 2]]]
    colr[~has] = colr[src_i] * 0.55
log("coloured;", int((~has).sum()), "surface voxels unseen")
np.savez_compressed(out + "/model.npz", solid=np.packbits(FG.ravel()), shape=np.array(FG.shape), sv=SV, col=colr,
                    origin=np.array([x0, y0, z0]), h=h, S=S)

# ---------------------------------------------------------------- previews
def render(elev, ang, scale, w, hgt, cx=None, cy=None, film_k=None):
    x, y, z = centres(SV)
    if film_k is not None:
        u, v, d = project(film_k, x, y, z); u = u * S; v = v * S
    else:
        th = ang; c, s = math.cos(th), math.sin(th)
        xr = x * c - z * s; zr = x * s + z * c
        cp, sp = math.cos(elev), math.sin(elev)
        up = y * cp - zr * sp; d = y * sp + zr * cp
        u = cx + xr * scale; v = cy - up * scale
    img = np.zeros((hgt, w, 3), np.float32) + np.array([0.09, 0.09, 0.16], np.float32)
    zb = np.full(hgt * w, -1e9, np.float32)
    rad = 2 if film_k is not None else max(2, int(math.ceil(scale * h)) + 1)
    order = np.argsort(d)
    for oy in range(rad + 1):
        for ox in range(rad + 1):
            iu = np.floor(u + ox - rad / 2).astype(np.int64); iv = np.floor(v + oy - rad / 2).astype(np.int64)
            ok = (iu >= 0) & (iv >= 0) & (iu < w) & (iv < hgt)
            pix = (iv * w + iu)[ok]; dd = d[ok]
            np.maximum.at(zb, pix, dd.astype(np.float32))
    zb2 = zb.reshape(hgt, w)
    for oy in range(rad + 1):
        for ox in range(rad + 1):
            iu = np.floor(u + ox - rad / 2).astype(np.int64); iv = np.floor(v + oy - rad / 2).astype(np.int64)
            ok = (iu >= 0) & (iv >= 0) & (iu < w) & (iv < hgt)
            sel = ok.copy(); sel[ok] = d[ok] >= zb2[iv[ok], iu[ok]] - 1e-6
            img[iv[sel], iu[sel]] = colr[sel]
    return (np.clip(img, 0, 1) * 255).astype(np.uint8)

tiles = []
pick = [KS[int(round(t * (len(KS) - 1) / 4))] for t in range(4)] if len(KS) >= 4 else KS
row_film, row_model = [], []
for k in pick:
    f = (np.where(MU[k][..., None], UP[k], np.array([0, 0, 0], np.float32)) * 255).astype(np.uint8)
    raw = cv2.resize((pal[frames[k]] * 255).astype(np.uint8), (W * S, H * S), interpolation=cv2.INTER_NEAREST)
    row_film.append(raw)
    row_model.append(render(None, None, None, W * S, H * S, film_k=k))
TS = max(W, H) * S
novel = []
ext = max(FG.shape) * h
for e, a in [(math.radians(30), 0.6), (math.radians(30), 2.4), (math.radians(-25), 1.2), (math.radians(80), 0.3)]:
    novel.append(render(e, a, TS * 0.8 / ext, TS, TS, cx=TS / 2, cy=TS / 2 + (y0 + FG.shape[1] * h / 2) * TS * 0.8 / ext))
def hcat(ims, hh):
    ims = [cv2.resize(i, (int(i.shape[1] * hh / i.shape[0]), hh), interpolation=cv2.INTER_AREA) for i in ims]
    return np.concatenate([np.pad(i, ((2, 2), (2, 2), (0, 0)), constant_values=40) for i in ims], 1)
hh = 360
rows = [hcat(row_film, hh), hcat(row_model, hh), hcat(novel, hh)]
wmax = max(r.shape[1] for r in rows)
sheet = np.concatenate([np.pad(r, ((0, 0), (0, wmax - r.shape[1]), (0, 0)), constant_values=40) for r in rows], 0)
cv2.imwrite(out + "/sheet.png", sheet[..., ::-1])
log("sheet written", out + "/sheet.png")
