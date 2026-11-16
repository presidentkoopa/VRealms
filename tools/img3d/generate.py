"""generate.py -- STEP 2: the image-to-3D model over each item's input.png -> <work>/<id>/raw.glb (textured).

    python generate.py [--backend hunyuan2|trellis2] [--work DIR] [--faces 20000] [--steps 50] [--seed 1234] [items ...]
    python generate.py --selftest OUT.glb        a procedural picture (no retail art) through the whole backend

PRIMARY: Tencent Hunyuan3D-2 (2.0; github.com/Tencent/Hunyuan3D-2, weights tencent/Hunyuan3D-2 on Hugging Face,
Tencent Hunyuan 3D 2.0 Community License -- docs/IMG3D_PIPELINE.md 2). Two stages, never both on the GPU at once, which
is how it fits 12 GB: (1) every item's SHAPE with Hunyuan3D-DiT v2-0 (about 6 GB; the upstream README: "6 GB VRAM for
shape generation and 16 GB for shape and texture generation in total"), floaters and degenerate faces removed and the
mesh reduced to --faces BEFORE texturing, so the texture is baked onto the final mesh; the shape model is then freed;
(2) every item's TEXTURE with Hunyuan3D-Paint v2-0 (turbo) and its delight model under enable_model_cpu_offload (the
upstream gradio app's --low_vram_mode path). The input's alpha is the cut-out: the background remover is not run on
an RGBA picture. The models load from IMG3D_MODELS (HY3DGEN_MODELS / HF_HOME point there; SETUP_IMG3D.bat set them).

FALLBACK: Microsoft TRELLIS.2-4B (MIT; github.com/microsoft/TRELLIS.2). Upstream wants Linux and 24 GB; on this PC
it runs only through a Windows build with low-VRAM changes (github.com/IgorAherne/TRELLIS.2-stableprojectorz, "fit
better into 8GB gpus") installed by its own installer. --backend trellis2 runs trellis2_run.py with THAT build's
python (IMG3D_TRELLIS_PYTHON) in its code folder (IMG3D_TRELLIS_CODE). Not set up by SETUP_IMG3D.bat.

Each item's log goes to <work>/<id>/generate.log; a failed item does not stop the others.
"""
import argparse
import gc
import json
import os
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import img3d_common as C  # noqa: E402

HY_REPO = "tencent/Hunyuan3D-2"
HY_SHAPE = "hunyuan3d-dit-v2-0"
HY_PAINT = "hunyuan3d-paint-v2-0-turbo"
HY_PAINT_PLAIN = "hunyuan3d-paint-v2-0"


def env_for_models():
    os.environ.setdefault("HY3DGEN_MODELS", C.MODELS_DIR)
    os.environ.setdefault("HF_HOME", os.path.join(C.MODELS_DIR, "hf"))
    os.environ.setdefault("PYTORCH_CUDA_ALLOC_CONF", "expandable_segments:True")


def vram():
    import torch
    if not torch.cuda.is_available():
        return "no CUDA"
    free, total = torch.cuda.mem_get_info()
    return "%.1f of %.1f GB free, peak %.1f GB" % (free / 2**30, total / 2**30, torch.cuda.max_memory_allocated() / 2**30)


def hunyuan_shapes(jobs, faces, steps, seed):
    import torch
    from PIL import Image
    from hy3dgen.shapegen import Hunyuan3DDiTFlowMatchingPipeline, FloaterRemover, DegenerateFaceRemover, FaceReducer
    C.log("shape: loading %s/%s" % (HY_REPO, HY_SHAPE))
    pipe = Hunyuan3DDiTFlowMatchingPipeline.from_pretrained(HY_REPO, subfolder=HY_SHAPE, use_safetensors=True)
    done = []
    for item, d in jobs:
        t0 = time.time()
        try:
            img = Image.open(os.path.join(d, "input.png")).convert("RGBA")
            torch.cuda.reset_peak_memory_stats()
            mesh = pipe(image=img, num_inference_steps=steps, octree_resolution=380, num_chunks=20000,
                        generator=torch.manual_seed(seed), output_type="trimesh")[0]
            mesh = FloaterRemover()(mesh)
            mesh = DegenerateFaceRemover()(mesh)
            mesh = FaceReducer()(mesh, max_facenum=faces)
            mesh.export(os.path.join(d, "shape.glb"))
            C.log("item %d: shape %d faces in %.0f s (%s)" % (item, len(mesh.faces), time.time() - t0, vram()))
            done.append((item, d))
        except Exception:  # noqa: BLE001 -- one item's failure is logged and the batch goes on
            C.log("item %d: SHAPE FAILED\n%s" % (item, traceback.format_exc()))
    del pipe
    gc.collect()
    torch.cuda.empty_cache()
    return done


def hunyuan_paint(jobs):
    import torch
    import trimesh
    from PIL import Image
    from hy3dgen.texgen import Hunyuan3DPaintPipeline
    pipe = None
    for sub in (HY_PAINT, HY_PAINT_PLAIN, None):
        try:
            C.log("texture: loading %s/%s with CPU offload" % (HY_REPO, sub or "(hy3dgen's default paint model)"))
            pipe = Hunyuan3DPaintPipeline.from_pretrained(HY_REPO, subfolder=sub) if sub else Hunyuan3DPaintPipeline.from_pretrained(HY_REPO)
            break
        except Exception as e:  # noqa: BLE001 -- a hy3dgen without `subfolder`, or a folder not downloaded: the next
            C.log("texture: %s did not load (%s)" % (sub, e))
    if pipe is None:
        C.log("texture: NO paint model loaded: nothing textured")
        return []
    pipe.enable_model_cpu_offload()
    done = []
    for item, d in jobs:
        t0 = time.time()
        try:
            img = Image.open(os.path.join(d, "input.png")).convert("RGBA")
            mesh = trimesh.load(os.path.join(d, "shape.glb"), force="mesh")
            torch.cuda.reset_peak_memory_stats()
            mesh = pipe(mesh, image=img)
            mesh.export(os.path.join(d, "raw.glb"))
            C.log("item %d: textured in %.0f s (%s) -> raw.glb" % (item, time.time() - t0, vram()))
            done.append(item)
        except Exception:  # noqa: BLE001
            C.log("item %d: TEXTURE FAILED\n%s" % (item, traceback.format_exc()))
    del pipe
    gc.collect()
    torch.cuda.empty_cache()
    return done


def trellis2(jobs, faces, seed):
    py = os.environ.get("IMG3D_TRELLIS_PYTHON")
    code = os.environ.get("IMG3D_TRELLIS_CODE")
    if not py or not code or not os.path.exists(py):
        sys.exit("trellis2: set IMG3D_TRELLIS_PYTHON (the TRELLIS.2 build's python.exe) and IMG3D_TRELLIS_CODE (its code folder)")
    done = []
    helper = os.path.join(C.HERE, "trellis2_run.py")
    for item, d in jobs:
        r = subprocess.run([py, helper, os.path.join(d, "input.png"), os.path.join(d, "raw.glb"), str(faces), str(seed)], cwd=code)
        if r.returncode == 0 and os.path.exists(os.path.join(d, "raw.glb")):
            C.log("item %d: TRELLIS.2 -> raw.glb" % item)
            done.append(item)
        else:
            C.log("item %d: TRELLIS.2 FAILED (exit %d)" % (item, r.returncode))
    return done


def run(items, work, backend="hunyuan2", faces=20000, steps=50, seed=1234):
    env_for_models()
    work = C.outside_repo(work)
    jobs = []
    for item in items:
        d = os.path.join(work, str(item))
        if os.path.exists(os.path.join(d, "input.png")):
            jobs.append((item, d))
        else:
            C.log("item %d: no input.png (run extract first)" % item)
    if not jobs:
        return []
    t0 = time.time()
    if backend == "trellis2":
        done = trellis2(jobs, faces, seed)
    else:
        shaped = hunyuan_shapes(jobs, faces, steps, seed)
        done = hunyuan_paint(shaped) if shaped else []
    for item, d in jobs:
        with open(os.path.join(d, "generated.json"), "w", encoding="utf-8") as f:
            json.dump({"backend": backend, "ok": item in done, "faces": faces, "steps": steps, "seed": seed,
                       "shape": HY_SHAPE if backend == "hunyuan2" else "TRELLIS.2-4B",
                       "paint": HY_PAINT if backend == "hunyuan2" else "TRELLIS.2-4B"}, f, indent=2)
    C.log("generate: %d of %d item(s) in %.0f s" % (len(done), len(jobs), time.time() - t0))
    return done


def selftest(out):
    """A procedural picture (a red cylinder with a yellow band, drawn here) through the whole backend."""
    from PIL import Image, ImageDraw
    work = os.path.join(C.WORK_DIR, "_selftest")
    os.makedirs(work, exist_ok=True)
    im = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im)
    dr.rectangle((362, 300, 662, 760), fill=(190, 40, 30, 255))
    dr.ellipse((362, 240, 662, 360), fill=(230, 80, 60, 255))
    dr.ellipse((362, 700, 662, 820), fill=(190, 40, 30, 255))
    dr.rectangle((362, 480, 662, 540), fill=(240, 200, 40, 255))
    os.makedirs(os.path.join(work, "0"), exist_ok=True)
    im.save(os.path.join(work, "0", "input.png"))
    done = run([0], work, "hunyuan2", 8000, 30, 1234)
    if done != [0]:
        return 1
    import shutil
    shutil.copyfile(os.path.join(work, "0", "raw.glb"), out)
    C.log("SELFTEST OK: %s (%d bytes)" % (out, os.path.getsize(out)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("items", nargs="*")
    ap.add_argument("--backend", default=os.environ.get("IMG3D_BACKEND", "hunyuan2"), choices=["hunyuan2", "trellis2"])
    ap.add_argument("--work", default=C.WORK_DIR)
    ap.add_argument("--faces", type=int, default=20000)
    ap.add_argument("--steps", type=int, default=50)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--selftest", metavar="OUT.glb")
    a = ap.parse_args()
    if a.selftest:
        return selftest(a.selftest)
    done = run(C.item_list(a.items), a.work, a.backend, a.faces, a.steps, a.seed)
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
