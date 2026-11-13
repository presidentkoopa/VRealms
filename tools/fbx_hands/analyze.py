#!/usr/bin/env python3
"""Structural report of a binary FBX hand rig (standard library only).

  python3 -I analyze.py <file.fbx> [section ...]
  sections: global, models, meshes, materials, skin, pose, anim, curves  (default: all but curves)

Every number printed is read from the file by fbxread.py; nothing is assumed.
"""
import hashlib
import math
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fbxread import read_fbx, props70, split_name  # noqa: E402

KTIME_PER_SEC = 46186158000  # FBX SDK FbxTime: 1 second = 46186158000 ticks


class Scene:
    def __init__(self, path):
        self.path = path
        self.version, self.root = read_fbx(path)
        self.objs = {}
        for n in self.root.find("Objects").children:
            self.objs[n.props[0]] = n
        self.children = defaultdict(list)   # parent id -> [(child id, prop)]
        self.parents = defaultdict(list)    # child id -> [(parent id, prop)]
        for c in self.root.find("Connections").findall("C"):
            kind, child, parent = c.props[0], c.props[1], c.props[2]
            prop = c.props[3] if len(c.props) > 3 else None
            self.children[parent].append((child, prop, kind))
            self.parents[child].append((parent, prop, kind))

    def name(self, oid):
        if oid == 0:
            return "<scene root>"
        n = self.objs.get(oid)
        return split_name(n.props[1])[0] if n is not None else "?%d" % oid

    def cls(self, oid):
        n = self.objs.get(oid)
        return (n.name, n.props[2]) if n is not None else ("?", "?")

    def kids(self, oid, objtype=None, sub=None):
        out = []
        for cid, prop, _ in self.children[oid]:
            n = self.objs.get(cid)
            if n is None:
                continue
            if objtype and n.name != objtype:
                continue
            if sub and n.props[2] != sub:
                continue
            out.append((cid, prop))
        return out

    def ups(self, oid, objtype=None, sub=None):
        out = []
        for pid, prop, _ in self.parents[oid]:
            n = self.objs.get(pid)
            if pid != 0 and n is None:
                continue
            if objtype and (n is None or n.name != objtype):
                continue
            if sub and (n is None or n.props[2] != sub):
                continue
            out.append((pid, prop))
        return out

    def model_parent(self, oid):
        for pid, prop, kind in self.parents[oid]:
            if kind == "OO" and (pid == 0 or (pid in self.objs and self.objs[pid].name == "Model")):
                return pid
        return None


# ---------------------------------------------------------------- matrices (row-major 4x4, column vectors)
def mmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def ident():
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def trans(t):
    m = ident()
    m[0][3], m[1][3], m[2][3] = t
    return m


def scale(s):
    m = ident()
    m[0][0], m[1][1], m[2][2] = s
    return m


def rot_axis(axis, deg):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    m = ident()
    if axis == 0:
        m[1][1], m[1][2], m[2][1], m[2][2] = c, -s, s, c
    elif axis == 1:
        m[0][0], m[0][2], m[2][0], m[2][2] = c, s, -s, c
    else:
        m[0][0], m[0][1], m[1][0], m[1][1] = c, -s, s, c
    return m


# FBX EFbxRotationOrder: 0 XYZ, 1 XZY, 2 YZX, 3 YXZ, 4 ZXY, 5 ZYX.  "XYZ" means X applied first: R = Rz*Ry*Rx.
_ORDERS = {0: (0, 1, 2), 1: (0, 2, 1), 2: (1, 2, 0), 3: (1, 0, 2), 4: (2, 0, 1), 5: (2, 1, 0)}


def euler(e, order=0):
    m = ident()
    for ax in _ORDERS[order]:
        m = mmul(rot_axis(ax, e[ax]), m)
    return m


def fbx_matrix_to_rows(arr):
    """FBX stores 16 doubles column-major (translation at 12,13,14)."""
    return [[arr[c * 4 + r] for c in range(4)] for r in range(4)]


def model_props(sc, oid):
    p = props70(sc.objs[oid])

    def v3(k, d):
        return tuple(p[k][3:6]) if k in p else d

    return {
        "T": v3("Lcl Translation", (0.0, 0.0, 0.0)),
        "R": v3("Lcl Rotation", (0.0, 0.0, 0.0)),
        "S": v3("Lcl Scaling", (1.0, 1.0, 1.0)),
        "Pre": v3("PreRotation", (0.0, 0.0, 0.0)),
        "Post": v3("PostRotation", (0.0, 0.0, 0.0)),
        "RotOff": v3("RotationOffset", (0.0, 0.0, 0.0)),
        "RotPiv": v3("RotationPivot", (0.0, 0.0, 0.0)),
        "SclOff": v3("ScalingOffset", (0.0, 0.0, 0.0)),
        "SclPiv": v3("ScalingPivot", (0.0, 0.0, 0.0)),
        "order": p["RotationOrder"][3] if "RotationOrder" in p else 0,
        "inherit": p["InheritType"][3] if "InheritType" in p else 0,
        "active": p["RotationActive"][3] if "RotationActive" in p else 0,
        "raw": p,
    }


def local_matrix(mp, R=None, T=None):
    """FBX SDK local transform: T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1."""
    R = mp["R"] if R is None else R
    T = mp["T"] if T is None else T
    neg = lambda v: tuple(-x for x in v)  # noqa: E731
    post = euler(mp["Post"], 0)
    post_inv = [[post[j][i] for j in range(4)] for i in range(4)]
    m = trans(T)
    for x in (trans(mp["RotOff"]), trans(mp["RotPiv"]), euler(mp["Pre"], 0), euler(R, mp["order"]), post_inv,
              trans(neg(mp["RotPiv"])), trans(mp["SclOff"]), trans(mp["SclPiv"]), scale(mp["S"]), trans(neg(mp["SclPiv"]))):
        m = mmul(m, x)
    return m


def world_matrix(sc, oid, overrides=None, cache=None):
    if cache is not None and oid in cache:
        return cache[oid]
    mp = model_props(sc, oid)
    R = T = None
    if overrides and oid in overrides:
        R = overrides[oid].get("R")
        T = overrides[oid].get("T")
    m = local_matrix(mp, R, T)
    par = sc.model_parent(oid)
    if par:
        m = mmul(world_matrix(sc, par, overrides, cache), m)
    if cache is not None:
        cache[oid] = m
    return m


def fmt3(v, p=3):
    return "(" + ", ".join(("%." + str(p) + "f") % x for x in v) + ")"


# ---------------------------------------------------------------- sections
def sec_global(sc):
    print("== GLOBAL")
    print("file", sc.path, os.path.getsize(sc.path), "bytes")
    print("FBX binary version", sc.version)
    hdr = sc.root.find("FBXHeaderExtension")
    cr = sc.root.find("Creator")
    print("Creator", cr.props[0] if cr else None, "| CreationTime", sc.root.find("CreationTime").props[0])
    si = hdr.find("SceneInfo")
    if si:
        for k, v in props70(si).items():
            if k.startswith(("Original|", "LastSaved|", "DocumentUrl", "SrcDocumentUrl")):
                print("  SceneInfo", k, v[3:])
    for k, v in props70(sc.root.find("GlobalSettings")).items():
        print("  GlobalSettings", k, v[3:])
    print("  object counts:", dict(Counter((n.name + ":" + n.props[2]) for n in sc.objs.values())))
    print("  connections:", len(sc.root.find("Connections").children))


def sec_models(sc):
    print("== MODEL TREE (Model nodes, parent via OO connections)")
    models = [oid for oid, n in sc.objs.items() if n.name == "Model"]

    def walk(oid, d):
        n = sc.objs[oid]
        mp = model_props(sc, oid)
        w = world_matrix(sc, oid)
        extra = ""
        if n.props[2] == "LimbNode":
            na = sc.kids(oid, "NodeAttribute")
            sz = props70(sc.objs[na[0][0]]).get("Size", (0, 0, 0, None))[3] if na else None
            extra = " size=%s" % sz
        print("  " * d + "%s [%s] T=%s R=%s S=%s Pre=%s ord=%d inh=%d world=%s%s" % (
            sc.name(oid), n.props[2], fmt3(mp["T"]), fmt3(mp["R"], 2), fmt3(mp["S"]), fmt3(mp["Pre"], 2),
            mp["order"], mp["inherit"], fmt3((w[0][3], w[1][3], w[2][3])), extra))
        for cid, _ in sc.kids(oid, "Model"):
            if sc.model_parent(cid) == oid:
                walk(cid, d + 1)

    for oid in models:
        if sc.model_parent(oid) == 0:
            walk(oid, 1)


def mesh_of_model(sc, mid):
    g = sc.kids(mid, "Geometry", "Mesh")
    return g[0][0] if g else None


def sec_meshes(sc):
    print("== MESHES")
    for mid, n in sc.objs.items():
        if n.name != "Model" or n.props[2] != "Mesh":
            continue
        gid = mesh_of_model(sc, mid)
        g = sc.objs[gid]
        verts = g.find("Vertices").props[0]
        pvi = g.find("PolygonVertexIndex").props[0]
        sizes = Counter()
        k = 0
        npoly = 0
        for i, x in enumerate(pvi):
            k += 1
            if x < 0:
                sizes[k] += 1
                npoly += 1
                k = 0
        tris = sum((s - 2) * c for s, c in sizes.items())
        nv = len(verts) // 3
        xs, ys, zs = verts[0::3], verts[1::3], verts[2::3]
        w = world_matrix(sc, mid)
        print("Mesh model '%s' geometry %d: control points %d, polygons %d (sizes %s), triangles after fan %d, polygon-vertex refs %d" % (
            sc.name(mid), gid, nv, npoly, dict(sizes), tris, len(pvi)))
        print("   local bbox x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]" % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
        print("   model world translation %s" % fmt3((w[0][3], w[1][3], w[2][3])))
        mp = model_props(sc, mid)
        print("   model Lcl T=%s R=%s S=%s" % (fmt3(mp["T"]), fmt3(mp["R"]), fmt3(mp["S"])))
        for le in g.children:
            if le.name.startswith("LayerElement"):
                nm = le.find("Name").props[0] if le.find("Name") else ""
                mi = le.find("MappingInformationType").props[0] if le.find("MappingInformationType") else ""
                ri = le.find("ReferenceInformationType").props[0] if le.find("ReferenceInformationType") else ""
                arrs = {c.name: len(c.props[0]) for c in le.children if c.props and hasattr(c.props[0], "tobytes")}
                extra = ""
                if le.name == "LayerElementMaterial":
                    extra = " material ids used: %s" % sorted(set(le.find("Materials").props[0]))
                print("   %s idx=%s name=%r mapping=%s ref=%s arrays=%s%s" % (le.name, le.props[0], nm, mi, ri, arrs, extra))
        for c in g.children:
            if c.name in ("Edges",):
                print("   Edges", len(c.props[0]))
        mats = sc.kids(mid, "Material")
        print("   materials (connection order = material index):", [sc.name(m) for m, _ in mats])
        print("   deformers:", [(sc.cls(d)[1], d) for d, _ in sc.kids(gid, "Deformer")])


def sec_materials(sc):
    print("== MATERIALS / TEXTURES / VIDEO")
    texdir = os.path.join(os.path.dirname(sc.path), "textures")
    disk = {}
    if os.path.isdir(texdir):
        for f in sorted(os.listdir(texdir)):
            with open(os.path.join(texdir, f), "rb") as fh:
                disk[hashlib.sha1(fh.read()).hexdigest()] = f
    for oid, n in sc.objs.items():
        if n.name == "Material":
            users = [sc.name(u) for u, _ in sc.ups(oid, "Model")]
            p = props70(n)
            print("Material '%s' shading=%s used by models %s" % (sc.name(oid), n.find("ShadingModel").props[0], users))
            for k in ("DiffuseColor", "Diffuse", "Maya|baseColor", "Maya|base", "Maya|specularRoughness", "Maya|metalness"):
                if k in p:
                    print("    %s = %s" % (k, p[k][3:]))
            for tid, prop in sc.kids(oid, "Texture"):
                print("    texture '%s' on property %r" % (sc.name(tid), prop))
    for oid, n in sc.objs.items():
        if n.name == "Texture":
            fn = n.find("FileName").props[0] if n.find("FileName") else ""
            rfn = n.find("RelativeFilename").props[0] if n.find("RelativeFilename") else ""
            mats = [(sc.name(m), pr) for m, pr in sc.ups(oid, "Material")]
            vids = [v for v, _ in sc.kids(oid, "Video")]
            print("Texture '%s' FileName=%r Relative=%r -> materials %s, video %s" % (sc.name(oid), fn, rfn, mats, vids))
    for oid, n in sc.objs.items():
        if n.name == "Video":
            c = n.find("Content")
            data = c.props[0] if c and c.props else b""
            h = hashlib.sha1(data).hexdigest() if data else None
            dims = None
            if data[:8] == b"\x89PNG\r\n\x1a\n":
                import struct
                dims = struct.unpack(">II", data[16:24]) + (data[24], data[25])
            print("Video %d '%s' file=%r embedded=%d bytes png(w,h,bitdepth,colortype)=%s same-bytes-as-disk=%s" % (
                oid, sc.name(oid), n.find("RelativeFilename").props[0], len(data), dims, disk.get(h)))


def skin_data(sc, gid):
    """Return {bone model id: (indexes, weights, Transform rows, TransformLink rows)} for a mesh geometry."""
    out = {}
    for did, _ in sc.kids(gid, "Deformer", "Skin"):
        for cid, _ in sc.kids(did, "Deformer", "Cluster"):
            cl = sc.objs[cid]
            bones = [b for b, _ in sc.kids(cid, "Model")]
            idx = cl.find("Indexes").props[0] if cl.find("Indexes") else []
            wts = cl.find("Weights").props[0] if cl.find("Weights") else []
            tr = fbx_matrix_to_rows(cl.find("Transform").props[0])
            tl = fbx_matrix_to_rows(cl.find("TransformLink").props[0])
            out[bones[0]] = (idx, wts, tr, tl, cid)
    return out


def sec_skin(sc):
    print("== SKIN")
    for mid, n in sc.objs.items():
        if n.name != "Model" or n.props[2] != "Mesh":
            continue
        gid = mesh_of_model(sc, mid)
        nv = len(sc.objs[gid].find("Vertices").props[0]) // 3
        sd = skin_data(sc, gid)
        for did, _ in sc.kids(gid, "Deformer", "Skin"):
            p = sc.objs[did]
            st = p.find("SkinningType").props[0] if p.find("SkinningType") else None
            acc = p.find("Link_DeformAcuracy").props[0] if p.find("Link_DeformAcuracy") else None
            print("mesh '%s': Skin deformer %d SkinningType=%s DeformAccuracy=%s clusters=%d" % (sc.name(mid), did, st, acc, len(sd)))
        per = defaultdict(list)
        for b, (idx, wts, tr, tl, cid) in sd.items():
            for i, w in zip(idx, wts):
                if w > 0:
                    per[i].append((w, b))
        cnt = Counter(len(v) for v in per.values())
        sums = [sum(w for w, _ in v) for v in per.values()]
        print("   influences per vertex histogram %s; vertices with no weight %d of %d" % (dict(sorted(cnt.items())), nv - len(per), nv))
        print("   weight sum min %.5f max %.5f" % (min(sums), max(sums)))
        skel = set(sd.keys())
        print("   cluster bones (%d):" % len(sd))
        for b, (idx, wts, tr, tl, cid) in sd.items():
            print("     %-22s verts %5d  TransformLink.t=%s" % (sc.name(b), sum(1 for w in wts if w > 0), fmt3((tl[0][3], tl[1][3], tl[2][3]))))
        # which skeleton root do the bones share
        roots = set()
        for b in skel:
            x = b
            while sc.model_parent(x) and sc.cls(sc.model_parent(x))[1] == "LimbNode":
                x = sc.model_parent(x)
            roots.add((x, sc.name(x)))
        print("   skeleton roots of the cluster bones:", roots)


def sec_pose(sc):
    print("== BIND POSES")
    for oid, n in sc.objs.items():
        if n.name != "Pose":
            continue
        nodes = n.findall("PoseNode")
        print("Pose '%s' type=%s nodes=%d" % (sc.name(oid), n.props[2], len(nodes)))
        for pn in nodes:
            nid = pn.find("Node").props[0]
            m = fbx_matrix_to_rows(pn.find("Matrix").props[0])
            w = world_matrix(sc, nid) if sc.objs[nid].name == "Model" else None
            dev = max(abs(m[i][j] - w[i][j]) for i in range(4) for j in range(4)) if w else None
            print("   %-22s %-8s bind t=%s  |bind - rest(Lcl) world| max=%.4g" % (
                sc.name(nid), sc.cls(nid)[1], fmt3((m[0][3], m[1][3], m[2][3])), dev))


def anim_index(sc):
    """[(stack, layer, curvenode, model, prop, {axis: curve id})]"""
    out = []
    for sid, n in sc.objs.items():
        if n.name != "AnimationStack":
            continue
        for lid, _ in sc.kids(sid, "AnimationLayer"):
            for cnid, _ in sc.kids(lid, "AnimationCurveNode"):
                targets = [(m, pr) for m, pr in sc.ups(cnid) if m != lid]
                curves = {pr: c for c, pr in sc.kids(cnid, "AnimationCurve")}
                for m, pr in targets:
                    out.append((sid, lid, cnid, m, pr, curves))
    return out


def curve(sc, cid):
    c = sc.objs[cid]
    t = c.find("KeyTime").props[0]
    v = c.find("KeyValueFloat").props[0]
    d = c.find("Default").props[0] if c.find("Default") else None
    return t, v, d


def sec_anim(sc):
    print("== ANIMATION")
    for sid, n in sc.objs.items():
        if n.name == "AnimationStack":
            p = props70(n)
            print("AnimationStack '%s':" % sc.name(sid), {k: (v[3] if len(v) > 3 else v) for k, v in p.items()})
            for k in ("LocalStart", "LocalStop", "ReferenceStart", "ReferenceStop"):
                if k in p:
                    print("   %s = %d ticks = %.4f s" % (k, p[k][3], p[k][3] / KTIME_PER_SEC))
            for lid, _ in sc.kids(sid, "AnimationLayer"):
                print("   layer '%s' curve nodes %d" % (sc.name(lid), len(sc.kids(lid, "AnimationCurveNode"))))
    tk = sc.root.find("Takes")
    if tk:
        for t in tk.findall("Take"):
            lt = t.find("LocalTime").props
            print("Takes/Take %r LocalTime %s -> %.4f..%.4f s" % (t.props[0], lt, lt[0] / KTIME_PER_SEC, lt[1] / KTIME_PER_SEC))
    idx = anim_index(sc)
    byprop = Counter((sc.cls(m)[1], pr) for _, _, _, m, pr, _ in idx)
    print("curve node targets by (model type, property):", dict(byprop))
    times = set()
    keycounts = Counter()
    animated = defaultdict(set)
    allcurves = 0
    for sid, lid, cnid, m, pr, curves in idx:
        for ax, cid in curves.items():
            t, v, d = curve(sc, cid)
            allcurves += 1
            keycounts[len(t)] += 1
            times.update(t)
            if len(v) and (max(v) - min(v)) > 1e-4:
                animated[sc.name(m) + "#" + str(m)].add(pr + ax[-1])
    ts = sorted(times)
    print("curves attached: %d; key count histogram %s" % (allcurves, dict(keycounts)))
    if ts:
        steps = Counter(ts[i + 1] - ts[i] for i in range(len(ts) - 1))
        print("distinct key times %d, first %.4f s last %.4f s; key spacing histogram (ticks): %s" % (
            len(ts), ts[0] / KTIME_PER_SEC, ts[-1] / KTIME_PER_SEC, dict(steps.most_common(5))))
        st = steps.most_common(1)[0][0]
        print("most common spacing %d ticks = 1/%.4f s" % (st, KTIME_PER_SEC / st))
    print("channels that actually change value (max-min > 1e-4):")
    for k in sorted(animated):
        print("   %-34s %s" % (k, sorted(animated[k])))


def main():
    path = sys.argv[1]
    secs = sys.argv[2:] or ["global", "models", "meshes", "materials", "skin", "pose", "anim"]
    sc = Scene(path)
    for s in secs:
        globals()["sec_" + s](sc)


if __name__ == "__main__":
    main()
