"""run.py -- the whole pipeline for a list of items: extract -> generate -> post (docs/IMG3D_PIPELINE.md).

    python run.py [items ...] [--steps extract,generate,post] [--backend hunyuan2|trellis2] [--data DIR]
                  [--work DIR] [--out DIR] [--faces 20000] [--no-esrgan] [--engine-dir DIR ...]

No items: items.json's default list (chapters 1 and 2). The log of the run is <work>/run_<time>.log as well as the
console. At the end the folder the engine reads is named in img3d_path.txt beside every doomxr.exe under the repo's
build-dxr (or each --engine-dir), so the engine finds the models with no setting (src/roth/roth_img3d.h).
"""
import argparse
import glob
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import img3d_common as C  # noqa: E402


class Tee:
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8")
        self.out = sys.stdout

    def write(self, s):
        self.out.write(s)
        self.f.write(s)

    def flush(self):
        self.out.flush()
        self.f.flush()


def point_engine(out, dirs):
    if not dirs:
        dirs = [os.path.dirname(p) for p in glob.glob(os.path.join(C.REPO, "build-dxr", "*", "doomxr.exe"))]
    for d in dirs:
        p = os.path.join(d, "img3d_path.txt")
        with open(p, "w", encoding="utf-8") as f:
            f.write(os.path.abspath(out) + "\n")
        C.log("engine: %s names %s" % (p, out))
    if not dirs:
        C.log("engine: no doomxr.exe under build-dxr: start it with +roth_img3d_dir \"%s\"" % out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("items", nargs="*")
    ap.add_argument("--steps", default="extract,generate,post")
    ap.add_argument("--backend", default=os.environ.get("IMG3D_BACKEND", "hunyuan2"), choices=["hunyuan2", "trellis2"])
    ap.add_argument("--data")
    ap.add_argument("--work", default=C.WORK_DIR)
    ap.add_argument("--out", default=C.OUT_DIR)
    ap.add_argument("--exe", default=os.path.join(C.BIN_DIR, "img3d_extract.exe"))
    ap.add_argument("--faces", type=int, default=20000)
    ap.add_argument("--steps-diffusion", type=int, default=50, dest="diffusion")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--no-esrgan", action="store_true")
    ap.add_argument("--engine-dir", action="append", default=[])
    a = ap.parse_args()
    work = C.outside_repo(a.work)
    out = C.outside_repo(a.out)
    os.makedirs(work, exist_ok=True)
    sys.stdout = Tee(os.path.join(work, time.strftime("run_%Y%m%d_%H%M%S.log")))
    items = C.item_list(a.items)
    steps = [s.strip() for s in a.steps.split(",") if s.strip()]
    C.log("img3d: items %s, steps %s, backend %s, work %s, out %s" % (" ".join(map(str, items)), ",".join(steps), a.backend, work, out))
    t0 = time.time()
    if "extract" in steps:
        import extract
        data = C.roth_data(a.data)
        if not data:
            sys.exit("no ROTH DATA folder (--data, IMG3D_ROTH_DATA or tools/claude_bridge/rothpath.txt)")
        items = extract.run(items, data, work, a.exe, not a.no_esrgan)
    if "generate" in steps and items:
        import generate
        items = generate.run(items, work, a.backend, a.faces, a.diffusion, a.seed)
    if "post" in steps and items:
        import post
        items = post.run(items, work, out, a.faces)
        point_engine(out, a.engine_dir)
    C.log("img3d: done in %.0f s: %d item(s) through every step asked: %s" % (time.time() - t0, len(items), " ".join(map(str, items))))
    return 0 if items else 1


if __name__ == "__main__":
    sys.exit(main())
