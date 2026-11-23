"""extract.py -- STEP 1: each item's best picture, cut out, cleaned and upscaled, ready for the image-to-3D model.

    python extract.py [--data "<ROTH DATA>"] [--work DIR] [--exe img3d_extract.exe] [--no-esrgan] [items ...]

1. img3d_extract (C++, the engine's own readers; build_extract.cmd builds it) writes every picture source of the item
   into <work>/<id>/: icon.tga (the inventory icon), world.tga (the picture it lies in the world as), film_NNN.tga
   (every frame of its close-up film), each cut out by the original's own transparency (index 0 / the film's ground
   mask), and info.json.
2. The view: <work>/<id>/choose.txt if the owner wrote one (a file name, e.g. film_004.tga); else the close-up
   frame with the most item pixels that does not touch the frame's edge (any frame if all do), unless the icon has
   more pixels; else the icon; else the world picture. The film's frames are the largest and most detailed pictures
   the install has of an item; the largest silhouette is the turntable's most open view of it.
3. Cleaned: cropped to the item; the transparent border filled with the nearest item colour (so neither the
   upscaler nor the image-to-3D model sees a black halo); upscaled 4x by Real-ESRGAN x4plus (BSD-3-Clause; the weights
   SETUP_IMG3D.bat downloads) until the long side is at least 384 px, then Lanczos to 768 px; the alpha by Lanczos
   then a soft threshold. Without the weights (or with --no-esrgan) Lanczos alone, and the log says so.
4. Written: <work>/<id>/input.png (1024 x 1024 RGBA, the item centred, 12 % margin) and views.png (every candidate
   side by side, the chosen one framed) for the owner to check the choice. Both stay in the work folder: they are
   derived from retail art and never go into the repository.
"""
import argparse
import glob
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import img3d_common as C  # noqa: E402

import numpy as np  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

_esrgan = None


def esrgan_model(want):
    """Real-ESRGAN x4plus through spandrel (MIT), on the GPU when there is one. None: not available."""
    global _esrgan
    if not want:
        return None
    if _esrgan is not None:
        return _esrgan or None
    _esrgan = False
    if not os.path.exists(C.ESRGAN_FILE):
        C.log("upscaler: %s missing (SETUP_IMG3D.bat downloads it): Lanczos only" % C.ESRGAN_FILE)
        return None
    try:
        import torch
        import spandrel
        m = spandrel.ModelLoader().load_from_file(C.ESRGAN_FILE)
        dev = "cuda" if torch.cuda.is_available() else "cpu"
        m.to(dev).eval()
        _esrgan = (m, dev)
        C.log("upscaler: Real-ESRGAN x4plus on", dev)
    except Exception as e:  # noqa: BLE001 -- any failure here means "no upscaler", said once
        C.log("upscaler: Real-ESRGAN could not load (%s): Lanczos only" % e)
    return _esrgan or None


def esrgan_x4(rgb, model):
    import torch
    m, dev = model
    t = torch.from_numpy(rgb.astype(np.float32) / 255.0).permute(2, 0, 1)[None].to(dev)
    with torch.no_grad():
        o = m(t)
    return (o[0].permute(1, 2, 0).clamp(0, 1).cpu().numpy() * 255.0 + 0.5).astype(np.uint8)


def fill_border(rgb, alpha):
    """Every transparent pixel takes the colour of the nearest item pixel (a breadth-first grow, 4-neighbour)."""
    h, w = alpha.shape
    out = rgb.copy()
    known = alpha > 0
    if not known.any():
        return out
    frontier = known.copy()
    while not known.all():
        grown = np.zeros_like(known)
        acc = np.zeros(out.shape, np.float32)
        cnt = np.zeros((h, w), np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            src = np.roll(np.roll(frontier, dy, 0), dx, 1)
            col = np.roll(np.roll(out, dy, 0), dx, 1)
            # np.roll wraps: drop the wrapped row / column
            if dy == 1:
                src[0, :] = False
            if dy == -1:
                src[-1, :] = False
            if dx == 1:
                src[:, 0] = False
            if dx == -1:
                src[:, -1] = False
            m = src & ~known
            acc[m] += col[m]
            cnt[m] += 1
            grown |= m
        if not grown.any():
            break
        out[grown] = (acc[grown] / cnt[grown][:, None] + 0.5).astype(np.uint8)
        known |= grown
        frontier = grown
    return out


def enclosed(alpha):
    """Transparent pixels the image's border cannot reach through transparent pixels (holes inside the item)."""
    clear = alpha == 0
    reach = np.zeros_like(clear)
    reach[0, :] = clear[0, :]
    reach[-1, :] = clear[-1, :]
    reach[:, 0] |= clear[:, 0]
    reach[:, -1] |= clear[:, -1]
    while True:
        g = reach.copy()
        g[1:, :] |= reach[:-1, :]
        g[:-1, :] |= reach[1:, :]
        g[:, 1:] |= reach[:, :-1]
        g[:, :-1] |= reach[:, 1:]
        g &= clear
        if (g == reach).all():
            return clear & ~reach
        reach = g


def prepare(path, use_esrgan, fill_holes=False):
    im = Image.open(path).convert("RGBA")
    a = np.array(im)
    if fill_holes:
        # The item's own pixels under its holes (the film keeps the colour under a cleared alpha): a lamp's dark glass
        # reads as the film's black ground, but it is glass, not a hole (items.json "fill_holes").
        a[:, :, 3][enclosed(a[:, :, 3])] = 255
    alpha = a[:, :, 3]
    ys, xs = np.nonzero(alpha)
    if len(xs) == 0:
        return None, "no item pixels"
    pad = 2
    x0, x1 = max(0, xs.min() - pad), min(a.shape[1], xs.max() + 1 + pad)
    y0, y1 = max(0, ys.min() - pad), min(a.shape[0], ys.max() + 1 + pad)
    a = a[y0:y1, x0:x1]
    rgb = fill_border(a[:, :, :3], a[:, :, 3])
    alpha = a[:, :, 3]
    model = esrgan_model(use_esrgan)
    how = []
    while model is not None and max(rgb.shape[:2]) < 384:
        rgb = esrgan_x4(rgb, model)
        how.append("ESRGAN x4")
    target = 768
    h, w = rgb.shape[:2]
    s = target / float(max(w, h))
    nw, nh = max(1, int(round(w * s))), max(1, int(round(h * s)))
    rgb_im = Image.fromarray(rgb).resize((nw, nh), Image.LANCZOS)
    al = np.array(Image.fromarray(alpha).resize((nw, nh), Image.LANCZOS)).astype(np.float32)
    al = np.clip((al - 96.0) * (255.0 / 64.0), 0, 255).astype(np.uint8)   # a soft threshold round the half
    how.append("Lanczos to %dx%d" % (nw, nh))
    out = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
    item = Image.merge("RGBA", (*rgb_im.split(), Image.fromarray(al)))
    if max(nw, nh) > 900:
        item = item.resize((int(nw * 900 / max(nw, nh)), int(nh * 900 / max(nw, nh))), Image.LANCZOS)
    out.paste(item, ((1024 - item.width) // 2, (1024 - item.height) // 2), item)
    return out, ", ".join(how)


def choose(d, info):
    override = os.path.join(d, "choose.txt")
    if os.path.exists(override):
        with open(override, encoding="utf-8") as f:
            name = f.readline().strip()
        if os.path.exists(os.path.join(d, name)):
            return name, "choose.txt"
        C.log("  choose.txt names %s, which is not there: choosing myself" % name)
    icon_area = 0
    if os.path.exists(os.path.join(d, "icon.tga")):
        icon_area = int((np.array(Image.open(os.path.join(d, "icon.tga")).convert("RGBA"))[:, :, 3] > 0).sum())
    frames = info.get("frames", [])
    best, best_area = None, -1
    for clean in (True, False):
        for k, (area, edge) in enumerate(frames):
            if clean and edge:
                continue
            if area > best_area:
                best, best_area = "film_%03d.tga" % k, area
        if best is not None:
            break
    if best is not None and best_area > icon_area:
        return best, "the close-up frame with the most item pixels (%d)%s" % (best_area, "" if frames[int(best[5:8])][1] == 0 else ", touching the edge")
    if icon_area > 0:
        return "icon.tga", "the inventory icon (%d item pixels)" % icon_area
    if os.path.exists(os.path.join(d, "world.tga")):
        return "world.tga", "the world picture (no icon, no film)"
    return None, "no picture"


def contact_sheet(d, chosen):
    files = ["icon.tga", "world.tga"] + sorted(os.path.basename(p) for p in glob.glob(os.path.join(d, "film_*.tga")))
    files = [f for f in files if os.path.exists(os.path.join(d, f))]
    cells = []
    for f in files:
        im = Image.open(os.path.join(d, f)).convert("RGBA")
        s = 160.0 / max(im.width, im.height)
        im = im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.NEAREST)
        cell = Image.new("RGB", (170, 186), (40, 40, 40) if f != chosen else (200, 160, 0))
        cell.paste(im, ((170 - im.width) // 2, 5), im)
        ImageDraw.Draw(cell).text((5, 170), f, fill=(255, 255, 255))
        cells.append(cell)
    cols = 8
    rows = (len(cells) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 170, max(1, rows) * 186), (0, 0, 0))
    for i, c in enumerate(cells):
        sheet.paste(c, ((i % cols) * 170, (i // cols) * 186))
    sheet.save(os.path.join(d, "views.png"))


def run(items, data, work, exe, use_esrgan=True):
    work = C.outside_repo(work)
    os.makedirs(work, exist_ok=True)
    if not exe or not os.path.exists(exe):
        sys.exit("extract: the extractor %s is not built (build_extract.cmd)" % exe)
    C.log("extract: %s -> %s, items %s" % (data, work, " ".join(map(str, items))))
    r = subprocess.run([exe, data, work] + [str(i) for i in items], capture_output=True, text=True)
    for line in (r.stdout + r.stderr).splitlines():
        C.log("  " + line)
    ok = []
    for item in items:
        d = os.path.join(work, str(item))
        ip = os.path.join(d, "info.json")
        if not os.path.exists(ip):
            C.log("item %d: nothing extracted" % item)
            continue
        with open(ip, encoding="utf-8") as f:
            info = json.load(f)
        name, why = choose(d, info)
        if name is None:
            C.log("item %d %s: no picture to use" % (item, info.get("name", "")))
            continue
        fill = bool(C.item_info(item).get("fill_holes", False))
        img, how = prepare(os.path.join(d, name), use_esrgan, fill)
        if fill:
            how = "holes inside the item filled (items.json), " + how
        if img is None:
            C.log("item %d: %s has %s" % (item, name, how))
            continue
        img.save(os.path.join(d, "input.png"))
        contact_sheet(d, name)
        with open(os.path.join(d, "chosen.json"), "w", encoding="utf-8") as f:
            json.dump({"view": name, "why": why, "prepared": how}, f, indent=2)
        C.log("item %d %s: %s -- %s; %s -> input.png" % (item, info.get("name", ""), name, why, how))
        ok.append(item)
    return ok


def main():
    C.keep_everything_on_root()   # every cache and temp folder under IMG3D_ROOT (owner: everything on D:)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("items", nargs="*")
    ap.add_argument("--data")
    ap.add_argument("--work", default=C.WORK_DIR)
    ap.add_argument("--exe", default=os.path.join(C.BIN_DIR, "img3d_extract.exe"))
    ap.add_argument("--no-esrgan", action="store_true")
    a = ap.parse_args()
    data = C.roth_data(a.data)
    if not data:
        sys.exit("no ROTH DATA folder (--data, IMG3D_ROTH_DATA or tools/claude_bridge/rothpath.txt)")
    ok = run(C.item_list(a.items), data, a.work, a.exe, not a.no_esrgan)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
