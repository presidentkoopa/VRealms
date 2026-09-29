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
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CAPTURES = os.path.join(HERE, "captures")
BASELINE = os.path.join(HERE, "baseline.json")

# The external watchdog. See capture_roth for why it cannot live in the plugin.
# FIRST_GRACE_S covers the intro, the menus and the quickload on each launch;
# STALL_S is how long a single camera spot may go without producing a buffer
# before the run is declared wedged on it. A good spot takes well under a
# second, so STALL_S is generous by two orders of magnitude on purpose.
FIRST_GRACE_S = 180
STALL_S = 40

# The four pattern passes, in the order roth_pattern numbers them.
PASS_NAME = {1: "ilo", 2: "ihi", 3: "tlo", 4: "thi"}

sys.path.insert(0, HERE)
from rothdiff import POSES  # noqa: E402

ORACLE_DIR = r"E:\DOOMWork\_oracle"
CROOT_DIR = r"E:\DOOMWork\_croot"
ROTHC_EXE = os.path.join(ORACLE_DIR, "rothc.exe")


SAVES = os.path.join(HERE, "saves")
LIVE_SAVE = os.path.join(ORACLE_DIR, "savegame", "SAVE0.SAV")


def install_save(mapname):
    """Put the right map's quicksave in slot 0 before capturing it.

    ROTH.C skips its intro by quickloading, so whatever map the save holds is
    the only map that can be captured. There is one quicksave key (F9, slot 0),
    so saving in a second map used to destroy the first -- which is not a
    workable way to run a rig that has 44 maps to get through.

    They are just files (`SAVE%D.SAV`), so instead we keep one per map in
    tools/rothdiff/saves as SAVE0.<MAP>.SAV and copy the right one into slot 0
    here. Nothing is ever overwritten: to add a map, quicksave in it once and
    file the result.

    A live slot 0 that is not yet filed gets filed before it is replaced, so a
    save made by hand is never silently thrown away.
    """
    want = os.path.join(SAVES, f"SAVE0.{mapname.upper()}.SAV")
    if not os.path.exists(want):
        print(f"  no saved game for {mapname} -- expected {want}")
        print(f"  make one: warp to {mapname} in ROTH.C (--devmode, W) and "
              f"press F9, then file SAVE0.SAV here under that name")
        return False

    os.makedirs(SAVES, exist_ok=True)
    if os.path.exists(LIVE_SAVE):
        import filecmp
        filed = [f for f in os.listdir(SAVES)
                 if filecmp.cmp(LIVE_SAVE, os.path.join(SAVES, f), shallow=False)]
        if not filed:
            stamp = time.strftime("%Y%m%d_%H%M%S")
            keep = os.path.join(SAVES, f"SAVE0.UNFILED_{stamp}.SAV")
            shutil.copy2(LIVE_SAVE, keep)
            print(f"  slot 0 held an unfiled save -- kept it as "
                  f"{os.path.basename(keep)}")

    shutil.copy2(want, LIVE_SAVE)
    print(f"  slot 0 <- {os.path.basename(want)}")
    return True


def capture_roth(poses, settle, hold, mapname="STUDY1"):
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

    ONE WEDGED SPOT MUST NOT COST THE WHOLE SET.

    Some camera spots wedge the original solidly: STUDY1's `study_doorway` and
    `corridor_along` both do. The game stops inside its own render loop, so the
    70 Hz tick never advances and no plugin hook is ever called again -- which
    means the watchdog INSIDE the plugin cannot fire. A watchdog running on the
    clock it is watching is no watchdog at all. Measured 2026-09-29: seven and a
    half minutes on one spot, no progress, process still alive.

    So it lives out here, where it has a wall clock and, more to the point, can
    kill the process. If no new buffer appears for `STALL_S`, the run is stuck
    on the first spot it has not produced; that spot is dropped and the game
    relaunched with what is left, until the set is done.

    The first capture of each launch gets a much longer grace period, because it
    includes the intro, the menus and the quickload. A load slower than the
    stall timeout would otherwise look exactly like a wedge and throw away a
    good spot -- an in-plugin version of this did precisely that to
    `study_start`.
    """
    os.makedirs(CAPTURES, exist_ok=True)
    if not install_save(mapname):
        return

    def have():
        return {n for n, *_ in poses
                if os.path.exists(os.path.join(CAPTURES, f"{n}.roth.ridb"))}

    remaining = [p for p in poses if p[0] not in have()]
    wedged = []

    # Bounded: every pass either captures something or drops exactly one spot.
    for attempt in range(len(poses) + 1):
        remaining = [p for p in remaining if p[0] not in have()]
        if not remaining:
            break

        posefile = os.path.join(CAPTURES, "poses.csv")
        with open(posefile, "w") as f:
            for name, x, y, a in remaining:
                f.write(f"{name},{x},{y},{a}\n")

        env = dict(os.environ)
        env["ROTHDIFF_POSEFILE"] = posefile
        env["ROTHDIFF_OUTDIR"] = CAPTURES
        env["ROTHDIFF_SETTLE"] = str(settle)
        env["ROTHDIFF_HOLD"] = str(hold)
        env["ROTHDIFF_QUICKLOAD"] = "1"

        logpath = os.path.join(CAPTURES, f"roth_attempt{attempt}.log")
        print(f"  ROTH.C run {attempt + 1}: quickload, "
              f"{len(remaining)} camera spot(s) ...")

        with open(logpath, "wb") as log:
            proc = subprocess.Popen(
                [ROTHC_EXE, "--headless",
                 "--game-dir", ORACLE_DIR, "--c-root", CROOT_DIR],
                cwd=ORACLE_DIR, env=env, stdout=log, stderr=subprocess.STDOUT,
            )

            seen = have()
            deadline = time.time() + FIRST_GRACE_S
            while proc.poll() is None:
                time.sleep(1.0)
                now = have()
                if now != seen:
                    for n in sorted(now - seen):
                        print(f"      captured {n}")
                    seen = now
                    deadline = time.time() + STALL_S
                if time.time() > deadline:
                    break

            stalled = proc.poll() is None
            if stalled:
                proc.kill()
                proc.wait(timeout=20)

        remaining = [p for p in remaining if p[0] not in have()]
        if stalled and remaining:
            bad = remaining[0]          # the first it never produced
            wedged.append(bad[0])
            remaining = remaining[1:]
            print(f"      WEDGED on {bad[0]} -- killed it, dropping that spot "
                  f"and retrying the other {len(remaining)}")
        elif not stalled and remaining:
            print(f"      exited on its own with {len(remaining)} spot(s) "
                  f"unmade -- see {os.path.basename(logpath)}")
            break

    missing = [n for n, *_ in poses if n not in have()]
    if wedged:
        print(f"  WEDGED {len(wedged)}: {', '.join(wedged)}")
    if missing:
        print(f"  MISSING {len(missing)}: {', '.join(missing)}")
        print(f"      per-run output is in {os.path.basename(CAPTURES)}"
              f"/roth_attempt*.log")


# Everything that must be OFF before a pattern capture, and why.
#
# The pattern puts a number on the screen and the screenshot reads it back. Any
# stage that changes a pixel between those two points corrupts the measurement
# while leaving a picture that looks fine -- which is the exact failure this
# whole rig exists to stop. uvdiff checks R==G==B per pixel and reports
# "TAINTED" when something here has been missed.
#
# Unknown names are harmless: the console says so and carries on, which is
# better than pruning the list to whatever this build happens to have.
PATTERN_OFF = [
    "roth_lighting 0",           # the Realms shade model; ours, and it shades
    "gl_texture_filter 0",       # nearest, or a texel is blended with its neighbour
    "gl_texture_filter_anisotropic 1",
    "r_mipmap 0",                # a distant surface must not sample a smaller copy
    "gl_bloom 0", "gl_ssao 0", "gl_fxaa 0", "gl_tonemap 0",
    "gl_lens 0", "gl_blendcolormaps 0",
    "vid_brightness 0", "vid_contrast 1", "vid_gamma 1",
    "gl_global_fade 0",
    "r_drawplayersprites 0",     # the weapon would cover part of the frame
    "hud_althud 0", "screenblocks 12", "crosshair 0",
]


def emit_remaroth_script(poses, w, h, path, pattern=0):
    """REMAROTH captures every pose in ONE run, because loading the level is the
    expensive part and the camera is just moved between dumps.

    With `pattern` set this emits a PATTERN pass: the textures are replaced by
    their own coordinates and each spot is an ordinary screenshot, which is the
    method the comparison uses now. Without it, the older identity-buffer dump.

    roth_pattern is read when a texture is built, so it has to be set before the
    level loads -- which it is, because console commands from +exec run
    immediately while `map` is deferred.
    """
    os.makedirs(CAPTURES, exist_ok=True)
    with open(path, "w") as f:
        # First, so that anything the run says afterwards is on disk. +logfile
        # on the command line is too late when the failure is early.
        f.write(f"logfile {os.path.join(CAPTURES, 'rema.log')}\n".replace("\\", "/"))
        if pattern:
            for line in PATTERN_OFF:
                f.write(line + "\n")
            f.write(f"roth_pattern {pattern}\n")
        for name, x, y, a in poses:
            if pattern:
                out = os.path.join(
                    CAPTURES, f"{name}.rema.{PASS_NAME[pattern]}.png").replace("\\", "/")
                f.write(f"rothdiff_shot {x} {y} {a} {out}\n")
                continue
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
    ap.add_argument("--pattern", type=int, default=0, choices=[0, 1, 2, 3, 4],
                    help="capture a PATTERN pass instead of an identity buffer: "
                         "1 texel index low, 2 index high, 3 texture id low, 4 id high")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=200)
    ap.add_argument("--save-baseline", action="store_true")
    args = ap.parse_args()

    poses = POSES.get(args.map)
    if not poses:
        print(f"no pose set for {args.map}", file=sys.stderr)
        return 2

    if args.capture_roth:
        capture_roth(poses, args.settle, args.hold, args.map)
    if args.capture_remaroth:
        p = emit_remaroth_script(poses, args.width, args.height,
                                 os.path.join(CAPTURES, "rothdiff.cfg"),
                                 args.pattern)
        print(f"wrote {p}\nrun REMAROTH with:  +exec {p}")
        if args.pattern:
            print(f"  pattern pass {args.pattern} ({PASS_NAME[args.pattern]}); "
                  f"all four passes are needed before uvdiff can score a spot")
        return 0

    return do_diff(poses, BASELINE, args.save_baseline)


if __name__ == "__main__":
    sys.exit(main())
