#!/usr/bin/env python3
"""The reference side of test_fbxload's S checks, with nothing the engine uses: tools/fbx_hands' own FBX reader (no
ufbx) and libpng (through OpenCV) instead of stb_image.

    python3 -I maps_ref.py <hand_final.fbx> <maps dir written by test_fbxload --maps> <base colour PNG beside the model>

  M1  every map test_fbxload extracted is, byte for byte, the Video content the FBX connects to aiStandardSurface1 on
      the same property (Material -> Texture on the property -> Video Content), read by fbxread.py;
  M2  the engine's decode (fbx_image.h: stb_image, 16-bit channels rounded) equals libpng's decode with the same
      rounding, every texel; and the 16-bit rounding against libpng's own 16-bit values;
  M3  the base colour file beside the model (what the engine draws) is the embedded base colour, every texel;
  M4  the left hand's corner UVs as the loader writes them (mirrorskin) against the RIGHT hand's UVs from fbxread.py at
      the same polygon and control point (an independent mirror check: no ufbx on this side);
  M5  the normal map's green runs up (OpenGL), measured: the slopes it encodes are curl-free only when read that way;
  and writes normal_decoded.png / roughness_decoded.png (the engine's decoded texels) for render.py.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "fbx_hands"))
from analyze import Scene, mesh_of_model  # noqa: E402
from fbxread import split_name  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print("  %s  %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        fails += 1


def video_bytes(sc, mat_name):
    """{property: (texture file, content bytes)} for the material named mat_name."""
    out = {}
    for oid, n in sc.objs.items():
        if n.name != "Material" or sc.name(oid) != mat_name:
            continue
        for tid, prop in sc.kids(oid, "Texture"):
            tn = sc.objs[tid]
            rfn = tn.find("RelativeFilename").props[0] if tn.find("RelativeFilename") else ""
            data = b""
            for vid, _ in sc.kids(tid, "Video"):
                c = sc.objs[vid].find("Content")
                if c is not None and c.props and len(c.props[0]):
                    data = bytes(c.props[0])
            out[prop] = (rfn.replace("\\", "/").split("/")[-1], data)
    return out


def uv_by_corner(sc, name):
    """{(polygon, control point): (u, v)} from the FBX's own LayerElementUV (v as stored, up)."""
    for mid, n in sc.objs.items():
        if n.name == "Model" and n.props[2] == "Mesh" and sc.name(mid) == name:
            g = sc.objs[mesh_of_model(sc, mid)]
    le = [c for c in g.children if c.name == "LayerElementUV"][0]
    uv = np.array(le.find("UV").props[0]).reshape(-1, 2)
    idx = le.find("UVIndex")
    per = uv[np.array(idx.props[0])] if idx is not None else uv
    pvi = np.array(g.find("PolygonVertexIndex").props[0])
    out = {}
    poly = 0
    for k, x in enumerate(pvi):
        cp = int(~x) if x < 0 else int(x)
        out[(poly, cp)] = (float(per[k][0]), float(per[k][1]))
        if x < 0:
            poly += 1
    poly_of = []
    poly = 0
    for x in pvi:
        poly_of.append(poly)
        if x < 0:
            poly += 1
    return out, poly_of


def main():
    fbx, mapsdir, basefile = sys.argv[1], sys.argv[2], sys.argv[3]
    sc = Scene(fbx)
    print("== the maps against fbxread.py (no ufbx) and libpng (OpenCV %s)" % cv2_version())
    vids = video_bytes(sc, "aiStandardSurface1")
    props = {"basecolor": "Maya|baseColor", "normal": "Maya|normalCamera", "roughness": "Maya|specularRoughness"}
    lines = [l.split() for l in open(os.path.join(mapsdir, "maps.txt"))]
    decoded = {}
    for slot, fname, prop, w, h, ch, bits in lines:
        w, h, ch, bits = int(w), int(h), int(ch), int(bits)
        ours = open(os.path.join(mapsdir, slot + ".bin"), "rb").read()
        rfile, ref = vids.get(props[slot], ("", b""))
        check(prop == props[slot] and fname == rfile and ours == ref and len(ref) > 0,
              "M1 %s: %s on %s, %d bytes, byte for byte the FBX's Video content" % (slot, fname, prop, len(ours)))
        rgba = np.fromfile(os.path.join(mapsdir, slot + ".rgba"), np.uint8).reshape(h, w, 4)
        decoded[slot] = rgba
        lib = libpng_decode(ours)
        if lib is None:
            print("  SKIP  M2 %s: OpenCV is not available here; the decode is NOT checked independently" % slot)
            continue
        if lib.dtype == np.uint16:
            exp8 = ((lib.astype(np.uint32) * 255 + 32767) // 65535).astype(np.uint8)
            trunc = (lib >> 8).astype(np.uint8)
            note = "16-bit, rounded; truncating (stb's own 16 -> 8) would differ on %.1f%% of the values" % (100.0 * (exp8 != trunc).mean())
        else:
            exp8 = lib
            note = "8-bit"
        if exp8.ndim == 2:
            exp8 = np.repeat(exp8[:, :, None], 3, 2)
        d = np.abs(rgba[:, :, :3].astype(int) - exp8[:, :, :3].astype(int)).max()
        check(d == 0 and (rgba[:, :, 3] == 255).all(), "M2 %s: the engine's decode equals libpng's, every texel (max diff %d; %s; alpha 255)" % (slot, d, note))
    # M3: the base colour beside the model
    lib_file = libpng_decode(open(basefile, "rb").read())
    if lib_file is not None and "basecolor" in decoded:
        d = np.abs(lib_file[:, :, :3].astype(int) - decoded["basecolor"][:, :, :3].astype(int)).max()
        check(d == 0, "M3 %s (what the engine draws as the base colour) equals the FBX's embedded base colour, every texel (max diff %d)" % (os.path.basename(basefile), d))
    # M5: which way the normal map's green runs, measured: a normal map is the slope of a height field, so the
    # slopes it gives must be curl-free; read with the wrong green the field has a curl. (The file's name says
    # OpenGL: green up. The engine's shader reads green up: main.fp WITH_NORMALMAP_GREEN_UP.)
    nb = open(os.path.join(mapsdir, "normal.bin"), "rb").read()
    lib = libpng_decode(nb)
    if lib is not None:
        a = lib.astype(np.float64) / (65535.0 if lib.dtype == np.uint16 else 255.0) * 2 - 1
        nx, ny, nz = a[..., 0], a[..., 1], np.maximum(a[..., 2], 0.3)
        ok = (a[..., 2] > 0.3) & ~((np.abs(a[..., 0]) < 0.002) & (np.abs(a[..., 1]) < 0.002))
        okc = ok[1:, 1:] & ok[:-1, :-1] & ok[1:, :-1] & ok[:-1, 1:]
        hx = -nx / nz
        res = {}
        for name, hr in (("up", ny / nz), ("down", -ny / nz)):   # slope along the rows (rows run down the image)
            c = (hx[1:, :-1] - hx[:-1, :-1]) - (hr[:-1, 1:] - hr[:-1, :-1])
            res[name] = np.sqrt((c[okc] ** 2).mean())
        check(res["up"] < 0.75 * res["down"], "M5 the normal map is green-up (OpenGL), as its name says and as the engine reads it: rms curl %.5f read green-up, "
              "%.5f read green-down, over %d texels" % (res["up"], res["down"], int(okc.sum())))
    # M4: the left hand's corners
    right, _ = uv_by_corner(sc, "Hand_Low")
    _, poly_of_left = uv_by_corner(sc, "Hand_Low1")
    worst = 0.0
    n = 0
    for line in open(os.path.join(mapsdir, "Hand_Low1_mirrorskin_uv.txt")):
        idx, cp, u, v = line.split()
        key = (poly_of_left[int(idx)], int(cp))
        ru, rv = right[key]
        worst = max(worst, abs(float(u) - ru), abs(float(v) - (1.0 - rv)))
        n += 1
    check(worst < 1e-6, "M4 the left hand's %d corners (mirrorskin) sample at the right hand's UV of the same polygon and control point, "
          "read by fbxread.py (max diff %.2g)" % (n, worst))
    # for render.py: the engine's decoded maps as PNG
    from PIL import Image
    for slot in ("normal", "roughness"):
        if slot in decoded:
            Image.fromarray(decoded[slot][:, :, :3]).save(os.path.join(mapsdir, slot + "_decoded.png"))
    print("\n%s: %d failure(s)" % ("PASS" if not fails else "FAIL", fails))
    return 1 if fails else 0


def cv2_version():
    try:
        import cv2
        return cv2.__version__
    except ImportError:
        return "NOT AVAILABLE"


def libpng_decode(data):
    try:
        import cv2
    except ImportError:
        return None
    a = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_UNCHANGED)
    if a is None:
        return None
    if a.ndim == 3:
        a = a[:, :, ::-1] if a.shape[2] == 3 else a[:, :, [2, 1, 0, 3]]   # BGR(A) -> RGB(A)
    return a


if __name__ == "__main__":
    sys.exit(main())
