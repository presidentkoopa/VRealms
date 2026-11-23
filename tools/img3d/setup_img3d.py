"""setup_img3d.py -- ONE-TIME SETUP of the image-to-3D pipeline on the owner's PC (SETUP_IMG3D.bat runs this).
Windows, an NVIDIA GPU (written for the RTX 3080 Ti, 12 GB), Visual Studio 2022 (the engine's own compiler).

Everything goes OUTSIDE the repository, under IMG3D_ROOT (default D:\\VRealms_img3d):
  img3d_env\\      the Python venv (Python 3.10 preferred; 3.11 / 3.12 accepted) with CUDA torch
  img3d_models\\   Hunyuan3D-2's code (git clone), its weights (Hugging Face), Real-ESRGAN x4plus, the HF cache
  img3d_bin\\      img3d_extract.exe (tools/img3d/img3d_extract.cpp, built here with MSVC)
  img3d_work\\     the pictures and raw meshes, per item
Steps (each says what it does; a step already done is skipped; a failure stops with what to do):
   1  the licence: Hunyuan3D-2's community licence does NOT apply in the EU, the UK or South Korea; you confirm
   2  a Python 3.10-3.12 for the venv
   3  the GPU (nvidia-smi) and the CUDA compiler nvcc 12.x (the texture stage's rasterizer is compiled)
   4  Visual Studio's x64 compiler (vswhere -> vcvars64.bat), for the rasterizer and the extractor
   5  the venv, pip, torch 2.5.1 + CUDA 12.4 (upstream's tested pair)
   6  Hunyuan3D-2 cloned, its requirements, hy3dgen installed, its commit written down
   7  the texture stage's two native modules built (custom_rasterizer with nvcc, differentiable_renderer)
   8  spandrel (the upscaler's loader) and Real-ESRGAN x4plus's weights
   9  the weights: tencent/Hunyuan3D-2's hunyuan3d-dit-v2-0, hunyuan3d-paint-v2-0(-turbo), hunyuan3d-delight-v2-0
  10  img3d_extract.exe
  11  the SELF-TEST: a picture drawn here (no game art) through shape + texture -> img3d_work\\selftest.glb
    python setup_img3d.py [--yes-licence] [--skip-selftest]
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import img3d_common as C  # noqa: E402

HY_GIT = "https://github.com/Tencent/Hunyuan3D-2.git"
HY_CODE = os.path.join(C.MODELS_DIR, "Hunyuan3D-2")
TORCH = ["torch==2.5.1", "torchvision==0.20.1", "--index-url", "https://download.pytorch.org/whl/cu124"]
# The turbo paint folder's name is the model zoo's "Paint-v2-0-Turbo" by the repo's naming (not seen in a listing):
# both paint folders are fetched, so generate.py can fall back to the plain one.
WEIGHTS = ["hunyuan3d-dit-v2-0/*", "hunyuan3d-paint-v2-0-turbo/*", "hunyuan3d-paint-v2-0/*", "hunyuan3d-delight-v2-0/*"]
STEPS = 11


class _Tee:
    """Everything printed also goes to IMG3D_ROOT\\setup_log.txt, so a stopped setup can be read without a screenshot."""
    def __init__(self, stream, log):
        self.stream, self.log = stream, log

    def write(self, text):
        self.stream.write(text)
        try:
            self.log.write(text)
            self.log.flush()
        except (OSError, ValueError):
            pass

    def flush(self):
        self.stream.flush()


def step(n, what):
    print("\n==== [%d/%d] %s" % (n, STEPS, what), flush=True)


def die(msg):
    print("\nSETUP STOPPED: " + msg, flush=True)
    sys.exit(1)


def run(cmd, env=None, cwd=None, check=True):
    print("  > " + (cmd if isinstance(cmd, str) else " ".join('"%s"' % c if " " in c else c for c in cmd)), flush=True)
    # streamed through print, so the command's own output lands in setup_log.txt too
    p = subprocess.Popen(cmd, env=env, cwd=cwd, shell=isinstance(cmd, str), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, errors="replace", bufsize=1)
    for line in p.stdout:
        sys.stdout.write(line)
    rc = p.wait()
    if check and rc != 0:
        die("the command above failed (exit %d)" % rc)
    return rc


def venv_python():
    return os.path.join(C.ENV_DIR, "Scripts", "python.exe")


def find_python():
    cands = [["py", "-3.10"], ["py", "-3.11"], ["py", "-3.12"], ["python"]]
    for c in cands:
        try:
            out = subprocess.run(c + ["-c", "import sys;print('%d.%d' % sys.version_info[:2]);print(sys.executable)"],
                                 capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if out.returncode == 0:
            ver, exe = out.stdout.split()[:2]
            if ver in ("3.10", "3.11", "3.12"):
                return exe, ver
    return None, None


def vcvars_env():
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        return None
    vs = subprocess.run([vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                         "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
    bat = os.path.join(vs, "VC", "Auxiliary", "Build", "vcvars64.bat")
    if not vs or not os.path.exists(bat):
        return None
    out = subprocess.run('"%s" >nul 2>&1 && set' % bat, capture_output=True, text=True, shell=True).stdout
    env = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k.upper()] = v   # one PATH, whatever case `set` printed it in (Windows names are case-blind)
    return env if "INCLUDE" in env else None


def model_env(base=None):
    env = {k.upper(): v for k, v in (base or os.environ).items()}
    env["HY3DGEN_MODELS"] = C.MODELS_DIR
    env["HF_HOME"] = os.path.join(C.MODELS_DIR, "hf")
    env["PYTHONUTF8"] = "1"
    return env


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--yes-licence", action="store_true")
    ap.add_argument("--skip-selftest", action="store_true")
    a = ap.parse_args()
    for d in (C.ROOT, C.MODELS_DIR, C.BIN_DIR, C.WORK_DIR):
        C.outside_repo(d)
        os.makedirs(d, exist_ok=True)
    log = open(os.path.join(C.ROOT, "setup_log.txt"), "a", encoding="utf-8", errors="replace")
    log.write("\n\n######## SETUP_IMG3D %s ########\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
    sys.stdout = _Tee(sys.stdout, log)
    sys.stderr = _Tee(sys.stderr, log)
    C.keep_everything_on_root()
    print("Everything goes under %s: caches, temp and downloads too (%s)." % (C.ROOT, ", ".join(sorted(C._ON_D))), flush=True)
    print("Free space: %s %.1f GB, C: %.1f GB." % (os.path.splitdrive(C.ROOT)[0], C.free_gb(C.ROOT), C.free_gb("C:\\")), flush=True)
    if 0 <= C.free_gb(C.ROOT) < 35:
        die("%s needs about 35 GB free for the setup (25 GB of models plus room to unpack)." % os.path.splitdrive(C.ROOT)[0])
    t0 = time.time()

    step(1, "the licence of the primary model (Tencent Hunyuan3D-2)")
    print("  Tencent Hunyuan 3D 2.0 Community License Agreement (huggingface.co/tencent/Hunyuan3D-2, LICENSE):\n"
          "    'THIS LICENSE AGREEMENT DOES NOT APPLY IN THE EUROPEAN UNION, UNITED KINGDOM AND SOUTH KOREA'\n"
          "    Territory = 'the worldwide territory, excluding the territory of the European Union, United Kingdom\n"
          "    and South Korea'. Outputs: 'Tencent claims no rights in Outputs You generate.' Over 1 million monthly\n"
          "    active users needs Tencent's own licence. Outputs may not be used to improve other AI models.\n"
          "  If you are in the EU, the UK or South Korea, do NOT continue: use the MIT-licensed fallback (TRELLIS.2),\n"
          "  docs/IMG3D_PIPELINE.md section 5.")
    if not a.yes_licence:
        ans = input("  Type AGREE if you are outside those territories and accept that licence: ").strip()
        if ans != "AGREE":
            die("the licence was not accepted; nothing was installed")

    step(2, "a Python 3.10-3.12 for the venv")
    if os.path.exists(venv_python()):
        print("  the venv exists: " + venv_python())
    else:
        exe, ver = find_python()
        if not exe:
            die("no Python 3.10, 3.11 or 3.12 found (py launcher or PATH). Install one, e.g.\n"
                "    winget install -e --id Python.Python.3.10\n  then run SETUP_IMG3D.bat again.")
        print("  using Python %s: %s" % (ver, exe))

    step(3, "the GPU and the CUDA compiler")
    if shutil.which("nvidia-smi"):
        run(["nvidia-smi", "--query-gpu=name,memory.total,driver_version", "--format=csv,noheader"], check=False)
    else:
        print("  nvidia-smi not on PATH (the driver installs it): the GPU could not be listed")
    nvcc = shutil.which("nvcc") or (os.path.join(os.environ["CUDA_PATH"], "bin", "nvcc.exe") if os.environ.get("CUDA_PATH") else None)
    if not nvcc or not os.path.exists(nvcc):
        die("no CUDA compiler (nvcc). The texture stage's rasterizer is compiled with it. Install the CUDA Toolkit 12.4:\n"
            "    https://developer.nvidia.com/cuda-12-4-1-download-archive  (Windows, x86_64, exe local)\n"
            "  then open a NEW window and run SETUP_IMG3D.bat again.")
    v = subprocess.run([nvcc, "--version"], capture_output=True, text=True).stdout
    print("  " + (v.strip().splitlines()[-1] if v.strip() else nvcc))
    if "release 12." not in v:
        die("nvcc is not CUDA 12.x (torch here is built for CUDA 12.4; the extension must match its major version)")
    cuda_home = os.path.dirname(os.path.dirname(nvcc))

    step(4, "Visual Studio's x64 compiler")
    vc = vcvars_env()
    if vc is None:
        die("Visual Studio 2022 with 'Desktop development with C++' was not found (vswhere / vcvars64.bat)")
    cl = shutil.which("cl", path=vc.get("PATH", ""))
    if not cl:
        die("vcvars64.bat ran but cl.exe is not on its PATH")
    print("  vcvars64: ok, " + cl)

    step(5, "the venv and CUDA torch")
    if not os.path.exists(venv_python()):
        exe, _ = find_python()
        run([exe, "-m", "venv", C.ENV_DIR])
    py = venv_python()
    run([py, "-m", "pip", "install", "--upgrade", "pip", "wheel", "setuptools<70"])
    have = subprocess.run([py, "-c", "import torch;print(torch.__version__, torch.cuda.is_available())"], capture_output=True, text=True).stdout
    if "2.5.1+cu124 True" in have:
        print("  torch " + have.strip())
    else:
        run([py, "-m", "pip", "install"] + TORCH)
        run([py, "-c", "import torch;print('torch', torch.__version__, 'CUDA', torch.cuda.is_available(), torch.cuda.get_device_name(0));"
                       "assert torch.cuda.is_available()"])

    step(6, "Hunyuan3D-2's code")
    if not os.path.exists(os.path.join(HY_CODE, ".git")):
        if not shutil.which("git"):
            die("git is not on PATH (Git for Windows)")
        run(["git", "clone", "--depth", "1", HY_GIT, HY_CODE])
    commit = subprocess.run(["git", "-C", HY_CODE, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    with open(os.path.join(C.MODELS_DIR, "hunyuan3d2_commit.txt"), "w") as f:
        f.write(commit + "\n")
    print("  Hunyuan3D-2 at commit " + commit)
    run([py, "-m", "pip", "install", "-r", os.path.join(HY_CODE, "requirements.txt")])
    run([py, "-m", "pip", "install", "-e", HY_CODE])

    step(7, "the texture stage's native modules (custom_rasterizer with nvcc, differentiable_renderer)")
    benv = model_env(vc)
    benv["CUDA_HOME"] = cuda_home
    benv["CUDA_PATH"] = cuda_home
    benv["DISTUTILS_USE_SDK"] = "1"
    benv["PATH"] = os.path.join(cuda_home, "bin") + os.pathsep + benv.get("PATH", "")
    # CUDA 12.4's nvcc accepts Visual Studio 2017-2022 only; the owner's PC has Visual Studio 2026 (MSVC 14.52), and
    # setup_log.txt (2026-11-23) stopped here on host_config.h's version check. nvcc's own override flag lets it use
    # the newer compiler, and the STL's matching check is waived the same way. CUDA 12.4 stays: torch is cu124, and
    # an extension must be built with the toolkit torch was built with. NVCC_APPEND_FLAGS reaches every nvcc call.
    benv["NVCC_APPEND_FLAGS"] = (benv.get("NVCC_APPEND_FLAGS", "") +
                                 " -allow-unsupported-compiler -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH").strip()
    benv["CL"] = (benv.get("CL", "") + " /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH").strip()
    print("  nvcc with Visual Studio 2026: NVCC_APPEND_FLAGS=%s" % benv["NVCC_APPEND_FLAGS"], flush=True)
    probe = subprocess.run([py, "-c", "import custom_rasterizer_kernel, mesh_processor"], capture_output=True, env=benv)
    if probe.returncode == 0:
        print("  both already built")
    else:
        for sub in ("custom_rasterizer", "differentiable_renderer"):
            run([py, "setup.py", "install"], env=benv, cwd=os.path.join(HY_CODE, "hy3dgen", "texgen", sub))
        run([py, "-c", "import torch, custom_rasterizer_kernel; import mesh_processor; print('native modules: ok')"], env=benv)

    step(8, "the upscaler (Real-ESRGAN x4plus through spandrel)")
    run([py, "-m", "pip", "install", "spandrel", "pillow", "numpy"])
    if not os.path.exists(C.ESRGAN_FILE):
        os.makedirs(os.path.dirname(C.ESRGAN_FILE), exist_ok=True)
        print("  downloading " + C.ESRGAN_URL)
        urllib.request.urlretrieve(C.ESRGAN_URL, C.ESRGAN_FILE + ".part")
        os.replace(C.ESRGAN_FILE + ".part", C.ESRGAN_FILE)
    print("  %s (%d bytes)" % (C.ESRGAN_FILE, os.path.getsize(C.ESRGAN_FILE)))

    step(9, "the weights (tencent/Hunyuan3D-2: shape v2-0, paint v2-0 and its turbo, delight v2-0; roughly 20 GB the first time)")
    target = os.path.join(C.MODELS_DIR, "tencent", "Hunyuan3D-2")
    run([py, "-c", "from huggingface_hub import snapshot_download as s; import sys; "
                   "p = s(repo_id='tencent/Hunyuan3D-2', local_dir=sys.argv[1], allow_patterns=sys.argv[2:]); print('weights in', p)",
         target] + WEIGHTS, env=model_env())

    step(10, "img3d_extract.exe (the engine's readers; MSVC)")
    src = os.path.join(C.REPO, "src", "roth")
    vox = os.path.join(C.REPO, "tools", "voxelize_parked")
    objdir = os.path.join(C.BIN_DIR, "obj")
    os.makedirs(objdir, exist_ok=True)
    files = [os.path.join(HERE, "img3d_extract.cpp")] + [os.path.join(src, f) for f in (
        "roth_install.cpp", "roth_dbase100.cpp", "roth_icons.cpp", "roth_das.cpp", "roth_palette.cpp", "roth_gdv.cpp", "roth_log.cpp")] + \
        [os.path.join(vox, "roth_voxelize.cpp")]
    run([cl, "/nologo", "/std:c++17", "/EHsc", "/O2", "/W3", "/D_CRT_SECURE_NO_WARNINGS", "/I", src, "/I", vox,
         "/Fo:" + objdir + "\\", "/Fd:" + os.path.join(objdir, "extract.pdb"), "/Fe:" + os.path.join(C.BIN_DIR, "img3d_extract.exe")] + files, env=vc)

    step(11, "the self-test: a picture drawn here (no game art) through shape and texture")
    if a.skip_selftest:
        print("  skipped (--skip-selftest)")
    else:
        out = os.path.join(C.WORK_DIR, "selftest.glb")
        run([py, os.path.join(HERE, "generate.py"), "--selftest", out], env=model_env(benv))
        print("  open %s in Windows' 3D Viewer: a red can with a yellow band" % out)
    print("\nSETUP DONE in %.0f min. Next: RUN_IMG3D.bat (docs/IMG3D_PIPELINE.md)." % ((time.time() - t0) / 60.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
