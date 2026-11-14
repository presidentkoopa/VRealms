#!/usr/bin/env python3
# Scratch (not repo): a textured (vertex-coloured) MESH from the carved hull, coloured from the 4x frames.
# usage: mesh.py <dumpdir/id> <outdir with model.npz> [height_m]
import sys, math, struct, json
SRC, OUT, SCALE = sys.argv[1], sys.argv[2], 4
HEIGHT_M = float(sys.argv[3]) if len(sys.argv) > 3 else 0.20
exec(open('/tmp/vxhq/prelude.py').read())
from skimage import measure
from scipy.spatial import cKDTree

d = np.load(OUT + "/model.npz")
shape = tuple(d['shape']); h = float(d['h']); x0, y0, z0 = d['origin']
FG = np.unpackbits(d['solid'])[:np.prod(shape)].reshape(shape).astype(bool)
sm = ndi.gaussian_filter(FG.astype(np.float32), sigma=1.1)
sm = np.pad(sm, 2)
del FG
verts, faces, nrm, _ = measure.marching_cubes(sm, 0.5, step_size=2, allow_degenerate=False)
del sm
verts -= 2
P = np.stack([x0 + (verts[:, 0] + 0.5) * h, y0 + (verts[:, 1] + 0.5) * h, z0 + (verts[:, 2] + 0.5) * h], 1)
N = nrm.copy()  # skimage: these already point out of the solid (measured)
N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
# winding: make faces counter-clockwise seen from outside
a, b, c = P[faces[:, 0]], P[faces[:, 1]], P[faces[:, 2]]
fn = np.cross(b - a, c - a)
if (np.einsum('ij,ij->i', fn, N[faces[:, 0]]) < 0).mean() > 0.5:
    faces = faces[:, [0, 2, 1]]
log("mesh", len(P), "vertices,", len(faces), "triangles")

def sample(img, px, py):
    n = len(px); C = 1024; r = (n + C - 1) // C
    mx = np.zeros(r * C, np.float32); my = np.zeros(r * C, np.float32); mx[:n] = px; my[:n] = py
    o = cv2.remap(img, mx.reshape(r, C), my.reshape(r, C), cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    return o.reshape(-1, img.shape[2])[:n]

# visibility: depth buffer from the triangles' own points (vertices + face centres), 4x the film
cen = P[faces].mean(1)
allpts = np.concatenate([P, cen])
acc = np.zeros((len(P), 3)); wsum = np.zeros(len(P))
for k in KS:
    u, v, dd = project(k, allpts[:, 0], allpts[:, 1], allpts[:, 2])
    iu = np.clip(np.floor(u * S).astype(np.int64), 0, W * S - 1); iv = np.clip(np.floor(v * S).astype(np.int64), 0, H * S - 1)
    zb = np.full(W * S * H * S, -1e9, np.float32)
    np.maximum.at(zb, iv * (W * S) + iu, dd.astype(np.float32))
    zb = ndi.maximum_filter(zb.reshape(H * S, W * S), size=3)
    uu, vv, dv = u[:len(P)], v[:len(P)], dd[:len(P)]
    vis = dv >= zb[iv[:len(P)], iu[:len(P)]] - 2.5 * h
    cosv = N @ viewdir(k)
    w = np.where(vis & (cosv > 0.2), np.clip(cosv, 0, 1) ** 8, 0.0)
    col = sample(UP[k], uu * S - 0.5, vv * S - 0.5)
    acc += col * w[:, None]; wsum += w
has = wsum > 0
C = np.zeros((len(P), 3), np.float32)
C[has] = acc[has] / wsum[has, None]
if (~has).any():
    t = cKDTree(P[has]); _, j = t.query(P[~has])
    C[~has] = C[has][j] * 0.6
log("coloured;", int((~has).sum()), "vertices no frame sees")

# ---- GLB (Y up, metres, standing on y = 0, the turntable axis at x = z = 0)
ymin = P[:, 1].min(); ext = np.ptp(P, 0).max()   # HEIGHT_M = the longest side
m_per = HEIGHT_M / ext
pos = np.stack([P[:, 0], P[:, 1] - ymin, P[:, 2]], 1).astype(np.float32) * m_per
col8 = (np.clip(C, 0, 1) * 255 + 0.5).astype(np.uint8)
col4 = np.concatenate([col8, np.full((len(col8), 1), 255, np.uint8)], 1)
idx = faces.astype(np.uint32).ravel()
def pad4(b): return b + b'\0' * ((4 - len(b) % 4) % 4)
bp, bn, bc, bi = pad4(pos.tobytes()), pad4(N.astype(np.float32).tobytes()), pad4(col4.tobytes()), pad4(idx.tobytes())
off = [0, len(bp), len(bp) + len(bn), len(bp) + len(bn) + len(bc)]
gl = {
    "asset": {"version": "2.0", "generator": "vrealms closeup reconstruction (scratch)"},
    "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "COLOR_0": 2}, "indices": 3, "material": 0}]}],
    "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0, "roughnessFactor": 0.8}, "doubleSided": False}],
    "buffers": [{"byteLength": off[3] + len(bi)}],
    "bufferViews": [{"buffer": 0, "byteOffset": off[0], "byteLength": pos.nbytes, "target": 34962},
                    {"buffer": 0, "byteOffset": off[1], "byteLength": N.size * 4, "target": 34962},
                    {"buffer": 0, "byteOffset": off[2], "byteLength": col4.nbytes, "target": 34962},
                    {"buffer": 0, "byteOffset": off[3], "byteLength": idx.nbytes, "target": 34963}],
    "accessors": [{"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": pos.min(0).tolist(), "max": pos.max(0).tolist()},
                  {"bufferView": 1, "componentType": 5126, "count": len(pos), "type": "VEC3"},
                  {"bufferView": 2, "componentType": 5121, "normalized": True, "count": len(pos), "type": "VEC4"},
                  {"bufferView": 3, "componentType": 5125, "count": len(idx), "type": "SCALAR"}],
}
js = pad4(json.dumps(gl).encode()).replace(b'\0', b' ')
binb = bp + bn + bc + bi
glb = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binb)) + struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(binb), 0x004E4942) + binb
open(OUT + "/model.glb", "wb").write(glb)
log("glb", len(glb) // 1024, "KB")
np.savez_compressed(OUT + "/mesh.npz", P=P, N=N, C=C, F=faces)

# ---- preview: a real triangle rasteriser (numpy, per-pixel barycentric over each triangle's box), flat-lit by the baked colour
def raster(Pu, Pv, Pd, Cv, Nv, w, hgt, bg=(0.09, 0.09, 0.16), shade=None):
    img = np.zeros((hgt, w, 3), np.float32) + np.array(bg, np.float32)
    zb = np.full((hgt, w), -1e9, np.float32)
    tri = faces
    u0, u1, u2 = Pu[tri[:, 0]], Pu[tri[:, 1]], Pu[tri[:, 2]]
    v0, v1, v2 = Pv[tri[:, 0]], Pv[tri[:, 1]], Pv[tri[:, 2]]
    area = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0)
    keep = area < 0   # front faces (screen v down)
    sel = np.flatnonzero(keep & (np.abs(area) > 1e-9))
    xmin = np.floor(np.minimum(np.minimum(u0, u1), u2)).astype(int); xmax = np.ceil(np.maximum(np.maximum(u0, u1), u2)).astype(int)
    ymin_ = np.floor(np.minimum(np.minimum(v0, v1), v2)).astype(int); ymax = np.ceil(np.maximum(np.maximum(v0, v1), v2)).astype(int)
    bw = (xmax - xmin + 1); bh = (ymax - ymin_ + 1)
    for sz in np.unique(np.maximum(bw[sel], bh[sel])):
        g = sel[np.maximum(bw[sel], bh[sel]) == sz]
        if len(g) == 0 or sz > 64: continue
        ox, oy = np.meshgrid(np.arange(sz), np.arange(sz))
        X = xmin[g][:, None] + ox.ravel()[None] + 0.5; Y = ymin_[g][:, None] + oy.ravel()[None] + 0.5
        A = area[g][:, None]
        l0 = ((u1[g][:, None] - X) * (v2[g][:, None] - Y) - (u2[g][:, None] - X) * (v1[g][:, None] - Y)) / A
        l1 = ((u2[g][:, None] - X) * (v0[g][:, None] - Y) - (u0[g][:, None] - X) * (v2[g][:, None] - Y)) / A
        l2 = 1 - l0 - l1
        ins = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
        Xi = X.astype(int); Yi = Y.astype(int)
        ins &= (Xi >= 0) & (Yi >= 0) & (Xi < w) & (Yi < hgt)
        ti, pi = np.nonzero(ins)
        gi = g[ti]
        L0, L1, L2 = l0[ti, pi], l1[ti, pi], l2[ti, pi]
        dz = L0 * Pd[tri[gi, 0]] + L1 * Pd[tri[gi, 1]] + L2 * Pd[tri[gi, 2]]
        xx, yy = Xi[ti, pi], Yi[ti, pi]
        cc = L0[:, None] * Cv[tri[gi, 0]] + L1[:, None] * Cv[tri[gi, 1]] + L2[:, None] * Cv[tri[gi, 2]]
        if shade is not None:
            nn = L0[:, None] * Nv[tri[gi, 0]] + L1[:, None] * Nv[tri[gi, 1]] + L2[:, None] * Nv[tri[gi, 2]]
            cc = cc * (0.75 + 0.25 * np.clip(nn @ shade, 0, 1))[:, None]
        o = np.argsort(dz)  # far first; later writes (nearer) win
        flat = yy[o] * w + xx[o]
        zf = zb.ravel(); imf = img.reshape(-1, 3)
        # resolve: keep the nearest per pixel
        _, last = np.unique(flat[::-1], return_index=True)
        last = len(flat) - 1 - last
        pix = flat[last]; dzz = dz[o][last]
        better = dzz > zf[pix]
        zf[pix[better]] = dzz[better]; imf[pix[better]] = cc[o][last][better]
    return (np.clip(img, 0, 1) * 255).astype(np.uint8)

pick = [KS[int(round(t * (len(KS) - 1) / 4))] for t in range(4)]
row_film, row_model = [], []
for k in pick:
    row_film.append(cv2.resize((pal[frames[k]] * 255).astype(np.uint8), (W * S, H * S), interpolation=cv2.INTER_NEAREST))
    u, v, dd = project(k, P[:, 0], P[:, 1], P[:, 2])
    row_model.append(raster(u * S, v * S, dd, C, N, W * S, H * S))
TS = max(W, H) * S
ext3 = np.ptp(P, 0).max()
cy0 = (P[:, 1].max() + P[:, 1].min()) / 2
novel = []
for e, ang in [(math.radians(25), 0.7), (math.radians(25), 2.6), (math.radians(-20), 1.3), (math.radians(75), 0.3)]:
    c_, s_ = math.cos(ang), math.sin(ang); cp, sp = math.cos(e), math.sin(e)
    xr = P[:, 0] * c_ - P[:, 2] * s_; zr = P[:, 0] * s_ + P[:, 2] * c_
    up = (P[:, 1] - cy0) * cp - zr * sp; dd = (P[:, 1] - cy0) * sp + zr * cp
    sc = TS * 0.85 / ext3
    novel.append(raster(TS / 2 + xr * sc, TS / 2 - up * sc, dd, C, N, TS, TS))
def hcat(ims, hh):
    ims = [cv2.resize(i, (int(i.shape[1] * hh / i.shape[0]), hh), interpolation=cv2.INTER_AREA) for i in ims]
    return np.concatenate([np.pad(i, ((2, 2), (2, 2), (0, 0)), constant_values=40) for i in ims], 1)
rows = [hcat(row_film, 400), hcat(row_model, 400), hcat(novel, 400)]
wmax = max(r.shape[1] for r in rows)
sheet = np.concatenate([np.pad(r, ((0, 0), (0, wmax - r.shape[1]), (0, 0)), constant_values=40) for r in rows], 0)
cv2.imwrite(OUT + "/mesh_sheet.png", sheet[..., ::-1])
log("mesh sheet", OUT + "/mesh_sheet.png")
