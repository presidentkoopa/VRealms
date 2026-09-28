#!/usr/bin/env python3
"""Drive both engines over a pose set and report the match percentages.

This is the thing to run after every loader change. It prints a table of
before/after match percentages and exits non-zero if any pose regressed against
the saved baseline, so "the fix works" stops being a claim and becomes a number.

    python run_rothdiff.py --capture-roth      # ROTH.C, one headless run per pose
    python run_rothdiff.py --capture-remaroth  # REMAROTH, one run, all poses
    python run_rothdiff.py                     # diff whatever has been captured
    python run_rothdiff.py --save-baseline     # bless the current numbers

Captures land in tools/rothdiff/captures/<pose>.{roth,rema}.ridb and are not
committed: they are outputs, and a stale one silently comparing against new code
is exactly the trap this tool exists to remove.
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CAPTURES = os.path.join(HERE, "captures")
BASELINE = os.path.join(HERE, "baseline.json")

sys.path.insert(0, HERE)
from rothdiff import POSES  # noqa: E402

ORACLE_DIR = r"E:\DOOMWork\_oracle"
CROOT_DIR = r"E:\DOOMWork\_croot"
ROTHC_EXE = os.path.join(ORACLE_DIR, "rothc.exe")


def capture_roth(poses, settle, hold):
    """ONE headless ROTH.C run for the WHOLE pose set.

    Loading the map is the expensive part and the camera is only being moved
    between captures, so the plugin walks the pose list inside a single run.
    An earlier version launched the game once per pose -- ten launches to
    produce ten buffers -- which was purely a defect in how this was written.

    The intro film and the menus are skipped by LOADING A SAVE: the plugin
    presses F10 (quickload, scancode 0x44, read out of the game's own keymap
    table) a few seconds in. Do NOT use --skip-gdv for this -- it makes the
    game ask for the boot CD.

    The save only has to be in the right MAP. The camera spots are absolute
    world coordinates, so where the save stands inside the level is irrelevant.
    """
    os.makedirs(CAPTURES, exist_ok=True)
    posefile = os.path.join(CAPTURES, "poses.csv")
    with open(posefile, "w") as f:
        for name, x, y, a in poses:
            f.write(f"{name},{x},{y},{a}\n")

    env = dict(os.environ)
    env["ROTHDIFF_POSEFILE"] = posefile
    env["ROTHDIFF_OUTDIR"] = CAPTURES
    env["ROTHDIFF_SETTLE"] = str(settle)
    env["ROTHDIFF_HOLD"] = str(hold)
    env["ROTHDIFF_QUICKLOAD"] = "1"

    print(f"  ROTH.C: one run, quickload, {len(poses)} camera spot(s) ...")
    r = subprocess.run(
        [ROTHC_EXE, "--headless",
         "--game-dir", ORACLE_DIR, "--c-root", CROOT_DIR],
        cwd=ORACLE_DIR, env=env, capture_output=True, timeout=600,
    )
    err = r.stderr.decode("latin-1", "replace")
    for line in err.splitlines():
        if "[rothdiff]" in line:
            print("   ", line.strip())

    missing = [n for n, *_ in poses
               if not os.path.exists(os.path.join(CAPTURES, f"{n}.roth.ridb"))]
    if missing:
        print(f"  MISSING {len(missing)}: {', '.join(missing)}")
        for line in err.strip().splitlines()[-3:]:
            print(f"      {line}")


def emit_remaroth_script(poses, w, h, path):
    """REMAROTH captures every pose in ONE run, because loading the level is the
    expensive part and the camera is just moved between dumps."""
    os.makedirs(CAPTURES, exist_ok=True)
    with open(path, "w") as f:
        # First, so that anything the run says afterwards is on disk. +logfile
        # on the command line is too late when the failure is early.
        f.write(f"logfile {os.path.join(CAPTURES, 'rema.log')}\n".replace("\\", "/"))
        for name, x, y, a in poses:
            out = os.path.join(CAPTURES, f"{name}.rema.ridb").replace("\\", "/")
            f.write(f"rothdiff_dump {x} {y} {a} {w} {h} {out}\n")
        # NO `quit` HERE. Console commands from +exec are not deferred, but
        # `map` is, so a quit in this script fires before a level ever exists
        # and kills the process with nothing captured. The engine quits itself
        # once it has drained the queued dumps.
    return path


def do_diff(poses, baseline, save):
    base = {}
    if os.path.exists(baseline):
        with open(baseline) as f:
            base = json.load(f)

    rows, worst_regression = [], 0.0
    results = {}
    for name, x, y, a in poses:
        ra = os.path.join(CAPTURES, f"{name}.roth.ridb")
        rb = os.path.join(CAPTURES, f"{name}.rema.ridb")
        if not (os.path.exists(ra) and os.path.exists(rb)):
            rows.append((name, None, base.get(name), "not captured"))
            continue
        p = subprocess.run(
            [sys.executable, os.path.join(HERE, "rothdiff.py"), "diff", ra, rb],
            capture_output=True, text=True,
        )
        if p.returncode == 2:
            rows.append((name, None, base.get(name), p.stderr.strip().splitlines()[0]))
            continue
        d = json.loads(p.stdout)
        pct = d["match_pct"]
        results[name] = pct
        was = base.get(name)
        note = ""
        if was is not None:
            delta = pct - was
            note = f"{delta:+.2f}"
            worst_regression = min(worst_regression, delta)
        rows.append((name, pct, was, note))

    w1 = max(len(r[0]) for r in rows) if rows else 10
    print(f"\n{'pose':<{w1}}  {'before':>8}  {'after':>8}  delta")
    print("-" * (w1 + 30))
    for name, pct, was, note in rows:
        b = f"{was:.2f}%" if was is not None else "--"
        a_ = f"{pct:.2f}%" if pct is not None else "--"
        print(f"{name:<{w1}}  {b:>8}  {a_:>8}  {note}")

    if save:
        with open(baseline, "w") as f:
            json.dump(results, f, indent=2)
        print(f"\nbaseline saved: {baseline}")
        return 0

    if worst_regression < -1e-9:
        print(f"\nREGRESSION: worst pose dropped {worst_regression:.2f} points")
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", default="STUDY1")
    ap.add_argument("--capture-roth", action="store_true")
    ap.add_argument("--capture-remaroth", action="store_true",
                    help="write the console script REMAROTH should +exec")
    ap.add_argument("--settle", type=int, default=40,
                    help="ticks before the first capture, for the map to settle")
    ap.add_argument("--hold", type=int, default=3,
                    help="ticks to hold each later pose; the level is already up")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=200)
    ap.add_argument("--save-baseline", action="store_true")
    args = ap.parse_args()

    poses = POSES.get(args.map)
    if not poses:
        print(f"no pose set for {args.map}", file=sys.stderr)
        return 2

    if args.capture_roth:
        capture_roth(poses, args.settle, args.hold)
    if args.capture_remaroth:
        p = emit_remaroth_script(poses, args.width, args.height,
                                 os.path.join(CAPTURES, "rothdiff.cfg"))
        print(f"wrote {p}\nrun REMAROTH with:  +exec {p}")
        return 0

    return do_diff(poses, BASELINE, args.save_baseline)


if __name__ == "__main__":
    sys.exit(main())
