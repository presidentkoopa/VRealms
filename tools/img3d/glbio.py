"""glbio.py -- the GLB the engine reads (src/roth/roth_img3d_core.h), written with the standard library only, and a
PNG encoder. Used by post.py (the pipeline's last step) and make_placeholder.py (the test boxes).

The file: glTF 2.0 binary, one mesh, one triangle primitive, POSITION + NORMAL + TEXCOORD_0 (float32) and uint32
indices, one material whose baseColorTexture is an embedded PNG. Metres, +Y up, +Z the item's front. UVs are glTF's
(top-left origin, v down).
"""
import json
import struct
import zlib
from array import array


def png_rgba(width, height, rgba):
    """Encode `rgba` (bytes, row-major, top row first, 4 bytes a pixel) as a PNG."""
    if len(rgba) != width * height * 4:
        raise ValueError("png_rgba: %d bytes for %dx%d" % (len(rgba), width, height))
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def _pad4(b, fill=b"\x00"):
    return b + fill * ((4 - len(b) % 4) % 4)


def write_glb(path, positions, normals, uvs, indices, image_bytes, image_mime="image/png", name="item", nodes_trs=None):
    """positions / normals: flat sequences of floats (3 a vertex); uvs: 2 a vertex (or None); indices: ints, 3 a
    triangle. `nodes_trs` (tests only): (translation, rotation quaternion xyzw, scale) put on the mesh's node."""
    n = len(positions) // 3
    if n == 0 or len(positions) != n * 3:
        raise ValueError("write_glb: positions must be 3 floats a vertex")
    if normals is not None and len(normals) != n * 3:
        raise ValueError("write_glb: normals count")
    if uvs is not None and len(uvs) != n * 2:
        raise ValueError("write_glb: uv count")
    if len(indices) % 3 != 0 or max(indices) >= n or min(indices) < 0:
        raise ValueError("write_glb: bad indices")

    blobs = []
    views = []
    accessors = []

    def add_view(data, target=None):
        off = sum(len(b) for b in blobs)
        blobs.append(_pad4(data))
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target is not None:
            v["target"] = target
        views.append(v)
        return len(views) - 1

    def add_float(vals, comps, minmax=False):
        data = array("f", [float(v) for v in vals]).tobytes()
        acc = {"bufferView": add_view(data, 34962), "componentType": 5126, "count": len(vals) // comps,
               "type": {2: "VEC2", 3: "VEC3"}[comps]}
        if minmax:
            acc["min"] = [min(vals[k::comps]) for k in range(comps)]
            acc["max"] = [max(vals[k::comps]) for k in range(comps)]
        accessors.append(acc)
        return len(accessors) - 1

    attrs = {"POSITION": add_float(positions, 3, True)}
    if normals is not None:
        attrs["NORMAL"] = add_float(normals, 3)
    if uvs is not None:
        attrs["TEXCOORD_0"] = add_float(uvs, 2)
    idata = array("I", [int(i) for i in indices])
    if idata.itemsize != 4:
        idata = array("L", [int(i) for i in indices])
    accessors.append({"bufferView": add_view(idata.tobytes(), 34963), "componentType": 5125, "count": len(indices), "type": "SCALAR"})
    prim = {"attributes": attrs, "indices": len(accessors) - 1, "mode": 4}
    doc = {"asset": {"version": "2.0", "generator": "VRealms tools/img3d glbio.py"},
           "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": name}],
           "meshes": [{"name": name, "primitives": [prim]}]}
    if nodes_trs is not None:
        t, r, s = nodes_trs
        doc["nodes"][0].update({"translation": list(t), "rotation": list(r), "scale": list(s)})
    if image_bytes:
        img_view = add_view(bytes(image_bytes))
        doc["images"] = [{"bufferView": img_view, "mimeType": image_mime}]
        doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}]
        doc["textures"] = [{"source": 0, "sampler": 0}]
        doc["materials"] = [{"name": name, "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                                                     "metallicFactor": 0.0, "roughnessFactor": 1.0}}]
        prim["material"] = 0
    binary = b"".join(blobs)
    doc["buffers"] = [{"byteLength": len(binary)}]
    doc["bufferViews"] = views
    doc["accessors"] = accessors
    js = _pad4(json.dumps(doc, separators=(",", ":")).encode("utf-8"), b" ")
    total = 12 + 8 + len(js) + 8 + len(binary)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(binary), 0x004E4942) + binary)
    return total


def read_glb(path):
    """The reading half (numpy): every triangle primitive the default scene reaches, node transforms applied, joined.
    Returns dict(positions Nx3, normals Nx3 or None, uvs Nx2 (glTF's, top-left origin) or None, indices Mx3,
    image bytes or None, mime). The same rules as the engine's reader (src/roth/roth_img3d_core.cpp)."""
    import numpy as np
    with open(path, "rb") as f:
        data = f.read()
    magic, version, total = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2:
        raise ValueError("%s: not a glTF 2 GLB" % path)
    js, binary, at = None, b"", 12
    while at + 8 <= min(total, len(data)):
        ln, typ = struct.unpack_from("<II", data, at)
        chunk = data[at + 8:at + 8 + ln]
        if typ == 0x4E4F534A and js is None:
            js = json.loads(chunk.decode("utf-8"))
        elif typ == 0x004E4942 and not binary:
            binary = chunk
        at += 8 + ln
    if js is None:
        raise ValueError("%s: no JSON chunk" % path)
    if js.get("extensionsRequired"):
        raise ValueError("%s: requires %s" % (path, js["extensionsRequired"]))
    ctype = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
    ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}

    def accessor(i):
        a = js["accessors"][i]
        dt = np.dtype(ctype[a["componentType"]])
        n, k = a["count"], ncomp[a["type"]]
        if "bufferView" not in a:
            return np.zeros((n, k), np.float64)
        bv = js["bufferViews"][a["bufferView"]]
        off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        stride = bv.get("byteStride", 0) or dt.itemsize * k
        rows = [np.frombuffer(binary, dt, k, off + stride * r) for r in range(n)] if stride != dt.itemsize * k else None
        arr = np.vstack(rows) if rows is not None else np.frombuffer(binary, dt, n * k, off).reshape(n, k)
        arr = arr.astype(np.float64)
        if a.get("normalized"):
            arr = arr / float(np.iinfo(dt).max)
        return arr

    def node_matrix(nd):
        if "matrix" in nd:
            return np.array(nd["matrix"], np.float64).reshape(4, 4).T
        t = nd.get("translation", [0, 0, 0])
        x, y, z, w = nd.get("rotation", [0, 0, 0, 1])
        s = nd.get("scale", [1, 1, 1])
        r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                      [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                      [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        m = np.eye(4)
        m[:3, :3] = r * np.array(s)[None, :]
        m[:3, 3] = t
        return m

    out = {"positions": [], "normals": [], "uvs": [], "indices": [], "image": None, "mime": None}
    count = [0]

    def primitive(p, m):
        if p.get("mode", 4) != 4:
            return
        at = p["attributes"]
        pos = accessor(at["POSITION"])
        pos = (np.c_[pos, np.ones(len(pos))] @ m.T)[:, :3]
        nrm = accessor(at["NORMAL"]) if "NORMAL" in at else None
        if nrm is not None:
            nrm = nrm @ m[:3, :3].T
            nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
        uv = accessor(at["TEXCOORD_0"]) if "TEXCOORD_0" in at else None
        idx = accessor(p["indices"]).astype(np.int64).reshape(-1) if "indices" in p else np.arange(len(pos))
        base = sum(len(a) for a in out["positions"])
        out["positions"].append(pos)
        out["normals"].append(nrm)
        out["uvs"].append(uv)
        out["indices"].append(idx.reshape(-1, 3) + base)
        if count[0] == 0 and "material" in p and out["image"] is None:
            try:
                tex = js["materials"][p["material"]]["pbrMetallicRoughness"]["baseColorTexture"]["index"]
                img = js["images"][js["textures"][tex]["source"]]
                bv = js["bufferViews"][img["bufferView"]]
                o = bv.get("byteOffset", 0)
                out["image"] = binary[o:o + bv["byteLength"]]
                out["mime"] = img.get("mimeType")
            except (KeyError, IndexError):
                pass
        count[0] += 1

    def node(i, parent, depth=0):
        nd = js["nodes"][i]
        m = parent @ node_matrix(nd)
        if "mesh" in nd:
            for p in js["meshes"][nd["mesh"]]["primitives"]:
                primitive(p, m)
        for c in nd.get("children", []):
            node(c, m, depth + 1)

    if js.get("scenes"):
        for n in js["scenes"][js.get("scene", 0)].get("nodes", []):
            node(n, np.eye(4))
    else:
        for mesh in js.get("meshes", []):
            for p in mesh["primitives"]:
                primitive(p, np.eye(4))
    if not out["positions"]:
        raise ValueError("%s: no triangles" % path)
    res = {"positions": np.vstack(out["positions"]), "indices": np.vstack(out["indices"]), "image": out["image"], "mime": out["mime"]}
    res["normals"] = np.vstack(out["normals"]) if all(n is not None for n in out["normals"]) else None
    res["uvs"] = np.vstack(out["uvs"]) if all(u is not None for u in out["uvs"]) else None
    return res
