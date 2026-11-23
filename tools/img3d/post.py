"""post.py -- STEP 3: each raw.glb cleaned, sized, pivoted and written where the engine reads it, with the manifest.

    python post.py [--work DIR] [--out DIR] [--faces 20000] [--tex 1024] [items ...]

Per item (<work>/<id>/raw.glb -> <out>/<id>_<name>.glb):
  1. one mesh (every primitive of the scene joined, glbio.read_glb: the engine reader's rules), its base colour
     picture and UVs; refused if it has none, or more faces than --faces (generate.py reduces before texturing, so this
     only catches a different generator);
  2. turned by items.json front_yaw (degrees about +Y) when the generator's front is not +Z for this item;
  3. the faces turned outward if the signed volume says they are wound inward; smooth (area-weighted) vertex normals;
  4. the picture made opaque RGB and no larger than --tex (Lanczos), PNG;
  5. sized: the longest side = items.json "metres" where known ("size": "table"); else 0.10 m and "size": "picture"
     (the engine then sizes the lying item to its picture and the held one to its card);
  6. the base on y = 0, centred in x and z (the lying pivot); the grip point = items.json grip_frac of the box plus
     grip_offset, in metres (the hand pivot);
  7. written with glbio.py (one primitive, float positions / normals / UVs, uint32 indices, the PNG embedded), read back
     and compared; manifest.json in <out> updated for these items (others kept).
The output folder must be outside the repository.
"""
import argparse
import io
import json
import math
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glbio  # noqa: E402
import img3d_common as C  # noqa: E402

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402


def slug(s):
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_") or "item"


def outward(v, f):
    """Turn every face round when the mesh's signed volume is negative: a generated mesh is wound one way
    throughout (marching cubes), so the only question is which way."""
    a, b, c = v[f[:, 0]], v[f[:, 1]], v[f[:, 2]]
    vol = float(np.einsum("ij,ij->i", a, np.cross(b, c)).sum()) / 6.0
    return (f[:, ::-1].copy(), True) if vol < 0 else (f, False)


def vertex_normals(v, f):
    """Area-weighted face normals summed per vertex (a vertex split at a UV seam keeps its own)."""
    fn = np.cross(v[f[:, 1]] - v[f[:, 0]], v[f[:, 2]] - v[f[:, 0]])
    n = np.zeros_like(v)
    for k in range(3):
        np.add.at(n, f[:, k], fn)
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    n = np.where(ln > 1e-20, n / np.maximum(ln, 1e-20), np.array([0.0, 1.0, 0.0]))
    return n


def process(item, work, out, faces_max, tex_max):
    info = C.item_info(item)
    d = os.path.join(work, str(item))
    raw = os.path.join(d, "raw.glb")
    if not os.path.exists(raw):
        C.log("item %d: no raw.glb (run generate first)" % item)
        return None
    g = glbio.read_glb(raw)
    if g["uvs"] is None or not g["image"]:
        C.log("item %d: REFUSED -- raw.glb has no base colour picture with UVs" % item)
        return None
    v, faces, uvs = g["positions"], g["indices"].astype(np.int64), g["uvs"].copy()
    if len(faces) > faces_max:
        C.log("item %d: REFUSED -- %d faces, over the %d budget (generate with --faces %d)" % (item, len(faces), faces_max, faces_max))
        return None
    if info.get("front_yaw"):
        a = math.radians(float(info["front_yaw"]))
        c, s = math.cos(a), math.sin(a)
        v = np.stack([v[:, 0] * c + v[:, 2] * s, v[:, 1], -v[:, 0] * s + v[:, 2] * c], 1)
    faces, flipped = outward(v, faces)

    lo, hi = v.min(0), v.max(0)
    longest = float((hi - lo).max())
    if longest <= 0:
        C.log("item %d: REFUSED -- the mesh has no size" % item)
        return None
    metres = info.get("metres")
    size = "table" if metres else "picture"
    scale = float(metres or 0.10) / longest
    centre = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
    v = (v - centre) * scale
    normals = vertex_normals(v, faces)
    lo, hi = v.min(0), v.max(0)
    frac = np.array(info.get("grip_frac", [0.5, 0.5, 0.5]), np.float64)
    grip = lo + frac * (hi - lo) + np.array(info.get("grip_offset", [0, 0, 0]), np.float64)

    pic = Image.open(io.BytesIO(g["image"])).convert("RGB")
    if max(pic.size) > tex_max:
        s = tex_max / float(max(pic.size))
        pic = pic.resize((max(1, int(pic.width * s)), max(1, int(pic.height * s))), Image.LANCZOS)
    buf = io.BytesIO()
    pic.save(buf, "PNG", optimize=True)

    name = "%d_%s.glb" % (item, slug(info["name"]))
    path = os.path.join(out, name)
    glbio.write_glb(path, v.reshape(-1).tolist(), normals.reshape(-1).tolist(), uvs.reshape(-1).tolist(),
                    faces.reshape(-1).tolist(), buf.getvalue(), name=info["name"])
    back = glbio.read_glb(path)
    if len(back["indices"]) != len(faces) or len(back["positions"]) != len(v) or not np.allclose(back["positions"], v, atol=1e-6):
        C.log("item %d: REFUSED -- %s did not read back as written" % (item, name))
        return None
    ext = hi - lo
    C.log("item %d %s: %s -- %d faces, %d vertices%s, %.3f x %.3f x %.3f m (%s), picture %dx%d, grip (%.3f %.3f %.3f), hold %s"
          % (item, info["name"], name, len(faces), len(v), " (faces turned outward)" if flipped else "", ext[0], ext[1], ext[2],
             "its real size" if size == "table" else "sized later to its picture", pic.width, pic.height, grip[0], grip[1], grip[2],
             info.get("hold", "palm")))
    src = None
    if os.path.exists(os.path.join(d, "chosen.json")):
        with open(os.path.join(d, "chosen.json"), encoding="utf-8") as f:
            src = json.load(f)
    return {"item": item, "name": info["name"], "file": name, "metres": round(float(ext.max()), 5), "size": size,
            "grip": [round(float(x), 5) for x in grip], "grip_rot": info.get("grip_rot", [0, 0, 0]),
            "hold": info.get("hold", "palm"), "world_yaw": info.get("world_yaw", 0), "faces": int(len(faces)), "source": src}


def resize(item, out):
    """--resize: the item's model already in <out> sized again to items.json "metres" (and its grip found again), the
    mesh and its picture untouched -- for a size set after the model was made. Not for a model of several parts
    (film_build.py makes those, and sizes them itself)."""
    info = C.item_info(item)
    old = {}
    mp = os.path.join(out, "manifest.json")
    if os.path.exists(mp):
        with open(mp, encoding="utf-8") as f:
            old = {e.get("item"): e for e in json.load(f).get("items", [])}
    name = old.get(item, {}).get("file") or "%d_%s.glb" % (item, slug(info["name"]))   # the file it was made as
    path = os.path.join(out, name)
    if not os.path.exists(path):
        C.log("item %d: no %s to resize" % (item, name))
        return None
    with open(path, "rb") as f:
        data = f.read()
    jlen = struct.unpack("<I", data[12:16])[0]
    if len(json.loads(data[20:20 + jlen].decode("utf-8")).get("materials", [])) > 1:
        C.log("item %d: %s has several parts -- resized by film_build.py, not here" % (item, name))
        return None
    g = glbio.read_glb(path)
    v, faces = g["positions"], g["indices"].astype(np.int64)
    lo, hi = v.min(0), v.max(0)
    metres = info.get("metres")
    scale = float(metres or 0.10) / float((hi - lo).max())
    centre = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
    v = (v - centre) * scale
    lo, hi = v.min(0), v.max(0)
    grip = lo + np.array(info.get("grip_frac", [0.5, 0.5, 0.5]), np.float64) * (hi - lo) + np.array(info.get("grip_offset", [0, 0, 0]), np.float64)
    glbio.write_glb(path, v.reshape(-1).tolist(), g["normals"].reshape(-1).tolist(), g["uvs"].reshape(-1).tolist(),
                    faces.reshape(-1).tolist(), g["image"], name=info["name"])
    ext = hi - lo
    C.log("item %d %s: resized to %.3f x %.3f x %.3f m (%s), grip (%.3f %.3f %.3f), hold %s" % (item, info["name"], ext[0], ext[1], ext[2],
          "its real size" if metres else "sized later to its picture", grip[0], grip[1], grip[2], info.get("hold", "palm")))
    e = dict(old.get(item, {}))
    e.update({"item": item, "name": info["name"], "file": name, "metres": round(float(ext.max()), 5), "size": "table" if metres else "picture",
              "grip": [round(float(x), 5) for x in grip], "grip_rot": info.get("grip_rot", [0, 0, 0]), "hold": info.get("hold", "palm"),
              "world_yaw": info.get("world_yaw", 0), "faces": int(len(faces))})
    return e


def write_manifest(out, entries):
    mp = os.path.join(out, "manifest.json")
    old = {"version": 1, "items": []}
    if os.path.exists(mp):
        try:
            with open(mp, encoding="utf-8") as f:
                old = json.load(f)
        except ValueError:
            C.log("manifest.json did not parse: written fresh")
    keep = [e for e in old.get("items", []) if e.get("item") not in {n["item"] for n in entries}]
    doc = {"version": 1, "generator": "VRealms tools/img3d (post.py)", "items": sorted(keep + entries, key=lambda e: e["item"])}
    tmp = mp + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
    os.replace(tmp, mp)
    C.log("manifest: %s, %d item(s)" % (mp, len(doc["items"])))


def run(items, work, out, faces_max=20000, tex_max=1024):
    out = C.outside_repo(out)
    work = C.outside_repo(work)
    os.makedirs(out, exist_ok=True)
    entries = []
    for item in items:
        try:
            e = process(item, work, out, faces_max, tex_max)
        except Exception as ex:  # noqa: BLE001 -- one item's failure is logged and the rest go on
            C.log("item %d: POST FAILED: %r" % (item, ex))
            e = None
        if e:
            entries.append(e)
    if entries:
        write_manifest(out, entries)
    return [e["item"] for e in entries]


def main():
    C.keep_everything_on_root()   # every cache and temp folder under IMG3D_ROOT (owner: everything on D:)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("items", nargs="*")
    ap.add_argument("--work", default=C.WORK_DIR)
    ap.add_argument("--out", default=C.OUT_DIR)
    ap.add_argument("--faces", type=int, default=20000)
    ap.add_argument("--tex", type=int, default=1024)
    ap.add_argument("--resize", action="store_true", help="only size the models already made again (items.json metres)")
    a = ap.parse_args()
    if a.resize:
        out = C.outside_repo(a.out)
        entries = [e for e in (resize(i, out) for i in C.item_list(a.items)) if e]
        if entries:
            write_manifest(out, entries)
        return 0 if entries else 1
    done = run(C.item_list(a.items), a.work, a.out, a.faces, a.tex)
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
