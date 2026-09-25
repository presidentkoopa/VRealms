#!/usr/bin/env python3
"""
One command to convert Realms of the Haunting maps into loadable PK3s.

    python build.py --roth "D:/.../Realms of the Haunting/ROTH" STUDY1
    python build.py --roth "..." --all

Runs the whole chain: read the map, extract the artwork pack it uses, pull out
3D props, and package a PK3. Extraction is cached per pack, so converting a
second map that shares a pack costs nothing extra.

LAYOUT
    tools/       the converters. Nothing here is generated.
    generated/   every output. Safe to delete entirely; this rebuilds it.
    authored/    hand-made data that a rebuild must NEVER touch.

The generated/authored split is the point. Regenerating a map has to be safe to
do forever, so hand-authored work -- grab points, mass, interaction tags,
character positions -- lives in authored/ keyed by the stable IDs each prop
carries (user_roth_sector / user_roth_object), and is merged in rather than
written into the same files the converter owns.
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parent
TOOLS = ROOT / "tools"
GEN = ROOT / "generated"


def run(script: str, *args) -> None:
    cmd = [sys.executable, str(TOOLS / script), *[str(a) for a in args]]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise SystemExit(f"FAILED: {script}")
    return r.stdout


def read_roth_res(roth: Path) -> dict:
    """The game's own manifest of which texture pack each map uses."""
    table, in_maps = {}, False
    for line in (roth / "ROTH.RES").read_text(errors="replace").splitlines():
        line = line.strip().replace("\\", "/")
        if line.startswith("maps"):
            in_maps = True
            continue
        if line.startswith("}"):
            in_maps = False
            continue
        if in_maps and " " in line:
            a, b = line.split()[:2]
            table[Path(a).stem.upper()] = Path(b).stem.upper()
    return table


def ensure_pack(roth: Path, pack: str) -> Path:
    """Extract a DAS pack's textures and meshes once, then reuse."""
    out = GEN / "packs" / pack
    if not (out / "textures" / "meta.json").exists():
        print(f"  extracting pack {pack} ...")
        run("extract_das.py", roth / "M" / f"{pack}.DAS", "-o", out / "textures")
        run("extract_meshes.py", roth / "M" / f"{pack}.DAS", "-o", out / "meshes")
    return out


def build_map(roth: Path, name: str, packs: dict, shared: str) -> None:
    raw = roth / "M" / f"{name}.RAW"
    if not raw.exists():
        print(f"  !! {name}.RAW not found, skipping")
        return

    pack = packs.get(name)
    if not pack:
        print(f"  !! {name} has no entry in ROTH.RES, skipping")
        return

    print(f"{name}  (pack {pack})")
    js = GEN / "maps" / f"{name}.json"
    run("parse_raw.py", raw, "-o", js)

    own = ensure_pack(roth, pack)
    shared_pack = ensure_pack(roth, shared)

    run("render_floorplan.py", js, "-o", GEN / "floorplans" / f"{name}.svg")

    out = run("build_map.py", js,
              "--textures", own / "textures",
              "--sprites", shared_pack / "textures",
              "--meshes", own / "meshes",
              "-o", GEN / "pk3" / f"ROTH_{name}.pk3",
              "--title", name.title())
    for line in out.splitlines():
        if any(k in line for k in ("objects", "linedefs", "sectors", "wall-mounted")):
            print("   " + line.strip())


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("maps", nargs="*", help="map names, e.g. STUDY1")
    ap.add_argument("--roth", type=Path, required=True,
                    help="the game's ROTH directory (containing ROTH.RES and M/)")
    ap.add_argument("--all", action="store_true", help="convert every map")
    args = ap.parse_args()

    for d in ("maps", "packs", "floorplans", "pk3"):
        (GEN / d).mkdir(parents=True, exist_ok=True)

    packs = read_roth_res(args.roth)
    # ADEMO is the shared sprite pack every map draws monsters and props from.
    shared = "ADEMO"

    names = sorted(packs) if args.all else [m.upper() for m in args.maps]
    if not names:
        raise SystemExit("name at least one map, or pass --all")

    for n in names:
        build_map(args.roth, n, packs, shared)

    print(f"\nPK3s in {GEN / 'pk3'}")


if __name__ == "__main__":
    main()
