"""img3d_common.py -- the folders and the item table every step of tools/img3d shares (docs/IMG3D_PIPELINE.md).

Everything the pipeline makes from the install goes OUTSIDE the repository: the work folder (pictures, raw meshes),
the models' output folder (the GLBs and the manifest the engine reads). Defaults are the owner's layout; every one
can be changed by an environment variable or a command-line option.
"""
import json
import os
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
HERE = os.path.dirname(os.path.abspath(__file__))

ROOT = os.environ.get("IMG3D_ROOT", r"D:\VRealms_img3d")
ENV_DIR = os.environ.get("IMG3D_ENV", os.path.join(ROOT, "img3d_env"))
MODELS_DIR = os.environ.get("IMG3D_MODELS", os.path.join(ROOT, "img3d_models"))
WORK_DIR = os.environ.get("IMG3D_WORK", os.path.join(ROOT, "img3d_work"))
BIN_DIR = os.environ.get("IMG3D_BIN", os.path.join(ROOT, "img3d_bin"))
OUT_DIR = os.environ.get("IMG3D_OUT", r"D:\VRealms_img3d\items")

# EVERYTHING ON D: (owner, 2026-11-23: "everything needs to go on D, I only have 14 GB on C"). Every cache, temp and
# download folder the tools would put under the user's profile on C: is sent under ROOT instead, for this process and
# every process it starts: pip's cache and build temp, torch's hub, Hugging Face, rembg's u2net (the background
# remover hy3dgen uses), CUDA's kernel cache, torch's compiled extensions, and the generic temp / cache folders.
CACHE_DIR = os.path.join(ROOT, "cache")
TEMP_DIR = os.path.join(ROOT, "tmp")
_ON_D = {
    "PIP_CACHE_DIR": os.path.join(CACHE_DIR, "pip"),
    "TORCH_HOME": os.path.join(CACHE_DIR, "torch"),
    "HF_HOME": os.path.join(MODELS_DIR, "hf"),
    "HUGGINGFACE_HUB_CACHE": os.path.join(MODELS_DIR, "hf", "hub"),
    "U2NET_HOME": os.path.join(CACHE_DIR, "u2net"),
    "CUDA_CACHE_PATH": os.path.join(CACHE_DIR, "cuda"),
    "TORCH_EXTENSIONS_DIR": os.path.join(CACHE_DIR, "torch_extensions"),
    "XDG_CACHE_HOME": CACHE_DIR,
    "TEMP": TEMP_DIR,
    "TMP": TEMP_DIR,
    "TMPDIR": TEMP_DIR,
}


def keep_everything_on_root():
    """Point every cache and temp folder under ROOT (os.environ, so child processes inherit it)."""
    for k, v in _ON_D.items():
        os.makedirs(v, exist_ok=True)
        os.environ[k] = v
    import tempfile
    tempfile.tempdir = TEMP_DIR


def free_gb(path):
    import shutil
    drive = os.path.splitdrive(os.path.abspath(path))[0] or path
    try:
        return shutil.disk_usage(drive + os.sep if drive else path).free / 1e9
    except OSError:
        return -1.0


ESRGAN_URL = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth"
ESRGAN_FILE = os.path.join(MODELS_DIR, "upscale", "RealESRGAN_x4plus.pth")


def log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


def roth_data(explicit=None):
    """The retail DATA folder: the option, else IMG3D_ROTH_DATA, else the bridge's rothpath.txt."""
    if explicit:
        return explicit
    if os.environ.get("IMG3D_ROTH_DATA"):
        return os.environ["IMG3D_ROTH_DATA"]
    p = os.path.join(REPO, "tools", "claude_bridge", "rothpath.txt")
    if os.path.exists(p):
        with open(p, encoding="utf-8-sig") as f:
            line = f.readline().strip().strip('"')
            if line:
                return line
    return None


def items_table():
    with open(os.path.join(HERE, "items.json"), encoding="utf-8") as f:
        return json.load(f)


def item_list(args_items):
    """Item ids from the command line ('16,15 21' or none = the table's default list)."""
    ids = []
    for a in args_items or []:
        for p in str(a).replace(",", " ").split():
            ids.append(int(p))
    return ids or list(items_table()["default_items"])


def item_info(item):
    t = items_table()["items"].get(str(item), {})
    d = {"name": "item %d" % item, "metres": None, "hold": "palm", "grip_frac": [0.5, 0.5, 0.5], "grip_offset": [0, 0, 0],
         "grip_rot": [0, 0, 0], "front_yaw": 0, "world_yaw": 0}
    d.update({k: v for k, v in t.items() if v is not None or k == "metres"})
    return d


def outside_repo(path):
    """Refuse to write derived art into the repository (CLAUDE.md)."""
    p = os.path.abspath(path)
    r = REPO
    if os.path.normcase(p) == os.path.normcase(r) or os.path.normcase(p).startswith(os.path.normcase(r) + os.sep):
        sys.exit("refused: %s is inside the repository; everything derived from the install stays outside it" % p)
    return p
