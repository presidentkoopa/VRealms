#!/usr/bin/env python3
"""
Batch-parse every ROTH map and report a coverage matrix.

The point is not to convert anything — it's to find out, before we invest in
conversion logic, how much the 44 maps actually vary. Anything the parser
doesn't understand is COUNTED, never silently dropped, so "we handled 97% of
STUDY1" can't hide "we handled 3% of CAVERNS".

Reads ROTH.RES for the authoritative map -> texture-pack table.

Usage:
    python survey_all.py --roth "D:/.../Realms of the Haunting/ROTH"
"""

import argparse
import traceback
from collections import Counter
from pathlib import Path

import parse_raw

SENTINEL_MIN = 32768


def read_roth_res(roth_dir: Path) -> dict:
    """map name (upper) -> das pack name (upper), from the game's own manifest."""
    res = roth_dir / "ROTH.RES"
    table, in_maps = {}, False
    for line in res.read_text(errors="replace").splitlines():
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


def survey(path: Path) -> dict:
    m = parse_raw.parse(path.read_bytes())
    sectors = m["sectorsSection"]["sectors"]
    faces = m["facesSection"]["faces"]
    maps_ = m["faceTextureMappingSection"]["mappings"]
    meta = m["mapMetadataSection"]
    verts = m["verticesSection"]["vertices"]

    tex_kind = Counter()
    for s in sectors:
        for k in ("floorTextureIndex", "ceilingTextureIndex"):
            v = s[k]
            tex_kind["sentinel" if v >= SENTINEL_MIN else "image"] += 1
    for t in maps_:
        for k in ("midTextureIndex", "upperTextureIndex", "lowerTextureIndex"):
            v = t[k]
            tex_kind["sentinel" if v >= SENTINEL_MIN else "image"] += 1

    # Structural health: do sector face loops close?
    open_loops = 0
    for s in sectors:
        used = Counter()
        for j in range(s["facesCount"]):
            f = faces[s["firstFaceIndex"] + j]
            used[f["vertexIndex01"]] += 1
            used[f["vertexIndex02"]] += 1
        if used and any(c % 2 for c in used.values()):
            open_loops += 1

    xs = [v["x"] for v in verts]
    ys = [v["y"] for v in verts]

    return {
        "sectors": len(sectors),
        "faces": len(faces),
        "twosided": sum(1 for f in faces if "sisterFaceIndex" in f),
        "verts": len(verts),
        "objects": sum(len(s["objectInformation"]) for s in sectors),
        "commands": len(m["commandsSection"]["allCommands"]),
        "midplat": len(m.get("midPlatformsSection", {}).get("platforms", [])),
        "open_loops": open_loops,
        "playerHeight": meta["playerHeight"],
        "maxClimb": meta["maxClimb"],
        "sentinel_pct": 100 * tex_kind["sentinel"] / max(1, sum(tex_kind.values())),
        "extent": max(max(xs) - min(xs), max(ys) - min(ys)),
        # Texture mappings with type >= 0x80 carry extra shift metadata — these
        # are the ones whose alignment we can't fake.
        "tm_extended": sum(1 for t in maps_ if "additionalMetadata" in t),
        "tm_total": len(maps_),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--roth", type=Path, required=True,
                    help="the game's ROTH directory (containing ROTH.RES and M/)")
    args = ap.parse_args()

    packs = read_roth_res(args.roth)
    mdir = args.roth / "M"
    raws = sorted(mdir.glob("*.RAW"))

    print(f"{'MAP':<10} {'PACK':<7} {'SECT':>5} {'FACE':>5} {'2SID':>5} {'OBJ':>4} "
          f"{'CMD':>4} {'MIDP':>4} {'OPEN':>4} {'PH':>3} {'SENT%':>5} {'EXTENT':>6} {'EXT-TM':>6}")
    print("-" * 96)

    ok = fail = 0
    heights, all_packs, totals = Counter(), Counter(), Counter()
    failures = []

    for raw in raws:
        name = raw.stem.upper()
        pack = packs.get(name, "?")
        try:
            s = survey(raw)
        except Exception as e:
            fail += 1
            failures.append((name, f"{type(e).__name__}: {e}"))
            print(f"{name:<10} {pack:<7} {'  !! PARSE FAILED':<40}")
            continue
        ok += 1
        heights[s["playerHeight"]] += 1
        all_packs[pack] += 1
        for k in ("sectors", "faces", "objects", "commands", "midplat", "open_loops"):
            totals[k] += s[k]
        print(f"{name:<10} {pack:<7} {s['sectors']:>5} {s['faces']:>5} {s['twosided']:>5} "
              f"{s['objects']:>4} {s['commands']:>4} {s['midplat']:>4} {s['open_loops']:>4} "
              f"{s['playerHeight']:>3} {s['sentinel_pct']:>4.0f}% {s['extent']:>6} "
              f"{s['tm_extended']:>6}")

    print("-" * 96)
    print(f"parsed OK: {ok}/{len(raws)}    failed: {fail}")
    print(f"totals: {totals['sectors']} sectors, {totals['faces']} faces, "
          f"{totals['objects']} objects, {totals['commands']} commands, "
          f"{totals['midplat']} mid-platforms")
    print(f"UNCLOSED sector loops across ALL maps: {totals['open_loops']}")
    print(f"playerHeight values seen: {dict(heights)}"
          f"   <- if this is a single value, ONE global scale works everywhere")
    print(f"texture packs in use: {dict(all_packs)}")
    if failures:
        print("\nfailures:")
        for n, e in failures:
            print(f"  {n}: {e}")


if __name__ == "__main__":
    main()
