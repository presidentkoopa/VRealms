#!/usr/bin/env python3
"""REMA in-hand mesh checker (docs/plans/REMA_INHAND_MESH_STANDARD.md).

    python rema_meshcheck.py <mesh> [--class pistol|smg|rifle|shotgun|heavy|melee] [--units m|cm|in|mapunits]

Reads GLB/glTF, MD3 and OBJ (+ <name>.points.json). Prints PASS/WARN/FAIL per rule, each failure
with its fix. Exit 0 = ready, 1 = warnings only, 2 = a failure. Standard library only.
"""
import json, math, os, struct, sys

CLASSES = {  # length range along the barrel, metres
    "pistol": (0.15, 0.30), "smg": (0.30, 0.70), "rifle": (0.70, 1.20),
    "shotgun": (0.70, 1.30), "heavy": (0.80, 1.80), "melee": (0.20, 1.80),
}
UNITS = {"m": 1.0, "cm": 0.01, "in": 0.0254, "mapunits": 1.0 / 32.0}  # 32 map units ~ 1 m (REMA vr_vunits_per_meter default family)
POINTS = ["muzzle", "support", "magwell", "eject", "sight", "stock", "grip"]
TRI_WARN, TRI_FAIL, TEX_WARN = 25000, 100000, 2048


# ---------------------------------------------------------------- readers
# Every reader returns: verts [(x,y,z)] in the file's units, in REMA's frame (+X barrel, +Y left, +Z up),
# tris (int), points {name: (x,y,z)}, textures [(name, w, h)], materials (int), notes [str]

def read_md3(path):
    d = open(path, "rb").read()
    if d[:4] != b"IDP3":
        raise ValueError("not an MD3 (no IDP3 header)")
    (_, _, _, _, nframes, ntags, nsurf, _, ofs_frames, ofs_tags, ofs_surf, _) = struct.unpack_from("<4si64siiiiiiiii", d, 0)
    pts = {}
    for t in range(ntags):  # frame 0's tags
        o = ofs_tags + t * 112
        name = d[o:o + 64].split(b"\0")[0].decode("latin-1").lower()
        x, y, z = struct.unpack_from("<3f", d, o + 64)
        key = name[4:] if name.startswith("tag_") else name
        pts[key] = (x, y, z)
    verts, tris, shaders = [], 0, []
    o = ofs_surf
    for _ in range(nsurf):
        (_flags, _nfr, nsh, nv, ntr, _ofs_tri, ofs_sh, _ofs_st, ofs_xyz, ofs_end) = struct.unpack_from("<10i", d, o + 68)
        tris += ntr
        for s in range(nsh):
            shaders.append(d[o + ofs_sh + s * 68:o + ofs_sh + s * 68 + 64].split(b"\0")[0].decode("latin-1"))
        for v in range(nv):  # frame 0
            x, y, z = struct.unpack_from("<3h", d, o + ofs_xyz + v * 8)
            verts.append((x / 64.0, y / 64.0, z / 64.0))
        o += ofs_end
    return verts, tris, pts, [(s, 0, 0) for s in shaders], max(1, len(set(shaders))), []


def read_obj(path):
    verts, tris, mats = [], 0, set()
    for line in open(path, encoding="latin-1"):
        p = line.split()
        if not p:
            continue
        if p[0] == "v":
            x, y, z = map(float, p[1:4])
            verts.append((-z, -x, y))  # OBJ is usually Y-up, -Z forward (Blender default export): to +X fwd, +Y left, +Z up
        elif p[0] == "f":
            tris += max(0, len(p) - 3)
        elif p[0] == "usemtl":
            mats.add(p[1] if len(p) > 1 else "")
    pts = {}
    side = os.path.splitext(path)[0] + ".points.json"
    notes = ["OBJ axes assumed Y-up, -Z forward (Blender's default export)."]
    if os.path.exists(side):
        for k, v in json.load(open(side)).items():
            x, y, z = v
            pts[k.lower()] = (-z, -x, y)
    else:
        notes.append(f"no {os.path.basename(side)}: named points can't be read from an OBJ without it.")
    return verts, tris, pts, [], max(1, len(mats)), notes


def read_gltf(path):
    if path.lower().endswith(".glb"):
        d = open(path, "rb").read()
        magic, _, _ = struct.unpack_from("<III", d, 0)
        if magic != 0x46546C67:
            raise ValueError("not a GLB")
        o, js, binc = 12, None, b""
        while o < len(d):
            ln, ty = struct.unpack_from("<II", d, o)
            chunk = d[o + 8:o + 8 + ln]
            if ty == 0x4E4F534A: js = json.loads(chunk)
            elif ty == 0x004E4942: binc = chunk
            o += 8 + ln
        buffers = [binc]
    else:
        js = json.load(open(path))
        base = os.path.dirname(path)
        buffers = []
        for b in js.get("buffers", []):
            uri = b.get("uri", "")
            if uri.startswith("data:"):
                import base64
                buffers.append(base64.b64decode(uri.split(",", 1)[1]))
            else:
                buffers.append(open(os.path.join(base, uri), "rb").read())

    def mat_of(node):
        t = node.get("translation", [0, 0, 0]); r = node.get("rotation", [0, 0, 0, 1]); s = node.get("scale", [1, 1, 1])
        if "matrix" in node:
            m = node["matrix"]; return [[m[c * 4 + r_] for c in range(4)] for r_ in range(4)]
        x, y, z, w = r
        R = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
             [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
             [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
        return [[R[i][0] * s[0], R[i][1] * s[1], R[i][2] * s[2], t[i]] for i in range(3)] + [[0, 0, 0, 1]]

    def mul(a, b):
        return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]

    def app(m, p):
        return tuple(m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] for i in range(3))

    def toREMA(p):  # glTF: +Y up, -Z forward, +X right  ->  +X forward, +Y left, +Z up
        return (-p[2], -p[0], p[1])

    nodes = js.get("nodes", [])
    verts, tris, pts, notes = [], 0, {}, []
    scenes = js.get("scenes", [{"nodes": list(range(len(nodes)))}])
    roots = scenes[js.get("scene", 0)]["nodes"] if scenes else []
    ident = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]

    def accessor(i):
        a = js["accessors"][i]; bv = js["bufferViews"][a["bufferView"]]
        buf = buffers[bv.get("buffer", 0)]
        off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        stride = bv.get("byteStride", 12)
        return [struct.unpack_from("<3f", buf, off + k * stride) for k in range(a["count"])]

    def walk(ni, parent):
        n = nodes[ni]; m = mul(parent, mat_of(n))
        name = n.get("name", "").lower()
        for key in POINTS:
            if name == key or name == "tag_" + key or name.endswith("_" + key) and key not in pts:
                pts[key] = toREMA(app(m, (0, 0, 0)))
        if "mesh" in n:
            for prim in js["meshes"][n["mesh"]].get("primitives", []):
                if "POSITION" in prim.get("attributes", {}):
                    for p in accessor(prim["attributes"]["POSITION"]):
                        verts.append(toREMA(app(m, p)))
                if "indices" in prim:
                    tris += js["accessors"][prim["indices"]]["count"] // 3
                else:
                    tris += js["accessors"][prim["attributes"]["POSITION"]]["count"] // 3
        for c in n.get("children", []):
            walk(c, m)

    for r in roots:
        walk(r, ident)
    tex = [(img.get("name") or img.get("uri", "embedded"), 0, 0) for img in js.get("images", [])]
    return verts, tris, pts, tex, max(1, len(js.get("materials", []))), notes


# ---------------------------------------------------------------- checks

def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("mesh")
    ap.add_argument("--class", dest="cls", choices=sorted(CLASSES))
    ap.add_argument("--units", choices=sorted(UNITS), default="m")
    a = ap.parse_args(argv)
    ext = os.path.splitext(a.mesh)[1].lower()
    reader = {".md3": read_md3, ".obj": read_obj, ".glb": read_gltf, ".gltf": read_gltf}.get(ext)
    if ext == ".fbx":
        print("FAIL  format: FBX isn't read directly. Fix: open it in Blender, keep the empties, File > Export > glTF 2.0 (.glb).")
        return 2
    if reader is None:
        print(f"FAIL  format: {ext} is not one of .glb .gltf .md3 .obj"); return 2
    verts, tris, pts, tex, mats, notes = reader(a.mesh)
    if not verts:
        print("FAIL  no vertices found"); return 2
    k = UNITS[a.units]
    verts = [(x * k, y * k, z * k) for x, y, z in verts]
    pts = {n: (p[0] * k, p[1] * k, p[2] * k) for n, p in pts.items()}

    res = []  # (level, rule, text)
    def R(level, rule, text): res.append((level, rule, text))

    lo = [min(v[i] for v in verts) for i in range(3)]; hi = [max(v[i] for v in verts) for i in range(3)]
    ext3 = [hi[i] - lo[i] for i in range(3)]
    axes = "XYZ"
    longest = max(range(3), key=lambda i: ext3[i])
    print(f"{os.path.basename(a.mesh)}: {len(verts)} verts, {tris} tris, size X {ext3[0]:.3f}  Y {ext3[1]:.3f}  Z {ext3[2]:.3f} m (units {a.units})")
    for n in notes: print("note  " + n)

    # frame: barrel along +X
    if longest == 0:
        R("PASS", "frame", "the longest side runs along X (the barrel axis)")
    else:
        rot = {1: "turn it 90 deg about Z (up) so the barrel points along +X",
               2: "turn it 90 deg about Y so the barrel points along +X (it is standing on end)"}[longest]
        R("FAIL", "frame", f"the longest side runs along {axes[longest]}, not X. Fix: {rot}.")

    # size
    L = ext3[0] if longest == 0 else ext3[longest]
    if a.cls:
        mn, mx = CLASSES[a.cls]
        if mn <= L <= mx:
            R("PASS", "size", f"length {L:.3f} m is inside {a.cls} {mn:.2f}-{mx:.2f} m")
        else:
            target = (mn + mx) / 2
            R("FAIL", "size", f"length {L:.3f} m is outside {a.cls} {mn:.2f}-{mx:.2f} m. Fix: scale by {target / L:.3f} (to {target:.2f} m), "
              f"or check --units (in cm it would be {L / k * 0.01:.3f} m, in inches {L / k * 0.0254:.3f} m).")
    else:
        R("WARN", "size", f"length {L:.3f} m; pass --class to check it against a real size range")

    # origin = grip
    inside = all(lo[i] - 1e-6 <= 0.0 <= hi[i] + 1e-6 for i in range(3))
    if "grip" in pts and max(abs(c) for c in pts["grip"]) > 0.005:
        g = pts["grip"]
        R("FAIL", "origin", f"a grip point is named at ({g[0]:.3f}, {g[1]:.3f}, {g[2]:.3f}) but the origin isn't on it. Fix: move the mesh by ({-g[0]:.3f}, {-g[1]:.3f}, {-g[2]:.3f}).")
    elif not inside:
        R("FAIL", "origin", f"the origin is outside the gun (bounds X {lo[0]:.3f}..{hi[0]:.3f}, Y {lo[1]:.3f}..{hi[1]:.3f}, Z {lo[2]:.3f}..{hi[2]:.3f}). "
          "Fix: put the origin at the centre of the main hand's grip on the handle.")
    else:
        fx = (0 - lo[0]) / ext3[0] if ext3[0] > 0 else 0.5
        fz = (0 - lo[2]) / ext3[2] if ext3[2] > 0 else 0.5
        if fx > 0.7 or fz > 0.8:
            R("WARN", "origin", f"the origin is {fx:.0%} of the way along the gun and {fz:.0%} up it; a grip is usually in the rear half and the lower part. Check it is on the handle.")
        else:
            R("PASS", "origin", f"the origin is inside the gun, {fx:.0%} along and {fz:.0%} up (handle area)")

    # roll / bore plane
    cy = (lo[1] + hi[1]) / 2
    if ext3[1] > 0 and abs(cy) > 0.25 * ext3[1] + 0.01:
        R("WARN", "frame", f"the gun's middle is {cy:+.3f} m off the origin sideways; the bore should pass over the grip (no roll, no side offset).")

    # named points
    if "muzzle" in pts:
        m = pts["muzzle"]
        if m[0] > 0 and m[0] >= hi[0] - 0.15 * ext3[0]:
            R("PASS", "points", f"muzzle at ({m[0]:.3f}, {m[1]:.3f}, {m[2]:.3f}), at the front")
        else:
            R("FAIL", "points", f"muzzle at ({m[0]:.3f}, {m[1]:.3f}, {m[2]:.3f}) is not at the front (+X) end. Fix: move the muzzle point to the barrel's mouth, or turn the gun to face +X.")
    else:
        R("FAIL", "points", "no 'muzzle' point. Fix: add an empty (glTF), a tag_muzzle (MD3) or a points.json entry (OBJ) at the barrel's mouth.")
    if a.cls in ("rifle", "shotgun", "smg", "heavy") and "support" not in pts:
        R("WARN", "points", "two-handed class with no 'support' point: the off hand will have nowhere to hold.")
    for n in ("support", "magwell", "eject", "sight", "stock"):
        if n in pts:
            p = pts[n]; R("PASS", "points", f"{n} at ({p[0]:.3f}, {p[1]:.3f}, {p[2]:.3f})")

    # budget
    if tris > TRI_FAIL: R("FAIL", "budget", f"{tris} triangles (limit {TRI_FAIL}). Fix: decimate in Blender (Decimate modifier).")
    elif tris > TRI_WARN: R("WARN", "budget", f"{tris} triangles (aim for {TRI_WARN} or fewer).")
    else: R("PASS", "budget", f"{tris} triangles")
    if mats > 4: R("WARN", "budget", f"{mats} materials; four or fewer is better")

    worst = 0
    for level, rule, text in res:
        print(f"{level:5} {rule:7} {text}")
        worst = max(worst, {"PASS": 0, "WARN": 1, "FAIL": 2}[level])
    print({0: "READY for REMA in-hand.", 1: "USABLE, with warnings.", 2: "NOT READY: fix the FAIL lines."}[worst])
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
