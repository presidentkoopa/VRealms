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

src, out = SRC, OUT
S = SCALE
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

