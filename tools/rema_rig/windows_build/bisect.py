#!/usr/bin/env python3
"""Find the mod that makes two machines disagree at tic 0 (or any tic), by bisection.

    python tools\\windows_build\\bisect.py -Zdl C:\\Users\\Command\\Desktop\\remaTest.zdl -Base 5 -Tics 60

Keeps the first -Base files of the load order in every run (the game's base: BD22 and its
hotfix, say) and bisects the rest: each run is nettest.py with -Files, the verdict is its
syncdiff ("WORLD DIVERGES at tic N" / "IN SYNC"). Assumes one culprit; it tests the found
file alone on the base at the end, and everything-but-it, to say whether that holds.
Prints every run's subset and verdict, so a wrong assumption shows. 60 tics is enough for
a tic-0 difference; raise -Tics for one that appears later.
"""
import os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
NETTEST = os.path.join(HERE, 'nettest.py')


def parse(argv):
    o = {'Zdl': '', 'Base': 5, 'Tics': 60, 'Stress': 0}
    for a in argv:
        a = a.strip()
        if not a.startswith('-') or ' ' not in a: continue
        k, v = a[1:].split(' ', 1)
        o[k] = int(v) if k in ('Base', 'Tics', 'Stress') else v.strip()
    return o


def load_order(zdl):
    pairs = []
    for line in open(zdl, errors='replace'):
        m = re.match(r'^file(\d+)=(.+)$', line.strip())
        if m: pairs.append((int(m.group(1)), m.group(2).strip()))
    return [p for _, p in sorted(pairs)]


def run(files, tics, stress):
    """Returns the first diverging tic, or -1 for in sync, or -2 when the run broke."""
    args = [sys.executable, NETTEST, '-Tics %d' % tics, '-Files ' + ';'.join(files)]
    if stress: args.append('-Stress 1')
    out = subprocess.run(args, capture_output=True, text=True, errors='replace').stdout
    m = re.search(r'WORLD DIVERGES at tic (\d+)', out)
    if m: return int(m.group(1))
    if 'IN SYNC' in out: return -1
    return -2


def bisect_with(pinned, cand, o, t0, names):
    """The one file of cand that, with `pinned`, diverges. None when none does alone."""
    step = 0
    while len(cand) > 1:
        step += 1
        half = cand[:len(cand) // 2]
        v = run(pinned + half, o['Tics'], o['Stress'])
        verdict = 'IN SYNC' if v == -1 else ('diverges at tic %d' % v if v >= 0 else 'BROKEN')
        print('  pin step %d: pinned + %d files [%s .. %s]: %s  (%.0f s)' % (step, len(half), os.path.basename(half[0]), os.path.basename(half[-1]), verdict, time.time() - t0), flush=True)
        if v == -2: return None
        if v >= 0: cand = half; continue
        other = cand[len(cand) // 2:]
        v2 = run(pinned + other, o['Tics'], o['Stress'])
        verdict2 = 'IN SYNC' if v2 == -1 else ('diverges at tic %d' % v2 if v2 >= 0 else 'BROKEN')
        print('  pin step %d: pinned + %d files [%s .. %s]: %s  (%.0f s)' % (step, len(other), os.path.basename(other[0]), os.path.basename(other[-1]), verdict2, time.time() - t0), flush=True)
        if v2 >= 0: cand = other
        else:
            print('  neither half of this side diverges with the pin: more than two files are involved, or the difference is not deterministic')
            return None
    return cand[0]


def main(argv):
    o = parse(argv)
    allf = load_order(o['Zdl'])
    base, rest = allf[:o['Base']], allf[o['Base']:]
    names = lambda fs: ', '.join(os.path.basename(f) for f in fs)
    print('bisect: %d files, base %d (%s), %d candidates' % (len(allf), len(base), names(base), len(rest)), flush=True)
    t0 = time.time()
    v = run(base, o['Tics'], o['Stress'])
    print('base alone: %s' % ('IN SYNC' if v == -1 else 'diverges at tic %d' % v if v >= 0 else 'BROKEN'), flush=True)
    if v != -1:
        print('the base itself disagrees -- bisect the base with a smaller -Base'); return 1
    cand = rest
    step = 0
    while len(cand) > 1:
        step += 1
        half = cand[:len(cand) // 2]
        v = run(base + half, o['Tics'], o['Stress'])
        verdict = 'IN SYNC' if v == -1 else ('diverges at tic %d' % v if v >= 0 else 'BROKEN')
        print('step %d: base + %d files [%s .. %s]: %s  (%.0f s)' % (step, len(half), os.path.basename(half[0]), os.path.basename(half[-1]), verdict, time.time() - t0), flush=True)
        if v == -2: return 2
        if v >= 0: cand = half
        else:
            # the culprit is in the other half (or needs something from both: checked at the end)
            other = cand[len(cand) // 2:]
            v2 = run(base + other, o['Tics'], o['Stress'])
            verdict2 = 'IN SYNC' if v2 == -1 else ('diverges at tic %d' % v2 if v2 >= 0 else 'BROKEN')
            print('step %d: base + %d files [%s .. %s]: %s  (%.0f s)' % (step, len(other), os.path.basename(other[0]), os.path.basename(other[-1]), verdict2, time.time() - t0), flush=True)
            if v2 >= 0: cand = other
            else:
                # AN INTERACTION: it takes a file from each half. Pin A whole and bisect B for its
                # member; then pin that member and bisect A for the other. Both get named.
                print('NEITHER HALF DIVERGES ALONE: it takes files from both halves. Pinning A, bisecting B...', flush=True)
                b = bisect_with(base + half, other, o, t0, names)
                if not b: return 3
                print('B MEMBER: ' + b + '. Pinning it, bisecting A...', flush=True)
                a = bisect_with(base + [b], half, o, t0, names)
                if not a: return 3
                print('PAIR: %s + %s' % (a, b), flush=True)
                v = run(base + [a, b], o['Tics'], o['Stress'])
                print('base + the pair alone: %s' % ('IN SYNC' if v == -1 else 'diverges at tic %d' % v if v >= 0 else 'BROKEN'))
                without = [f for f in allf if f != b]
                v = run(without, o['Tics'], o['Stress'])
                print('everything but %s: %s' % (os.path.basename(b), 'IN SYNC' if v == -1 else 'diverges at tic %d' % v if v >= 0 else 'BROKEN'))
                print('done in %.0f s' % (time.time() - t0))
                return 0
    culprit = cand[0]
    print('CULPRIT: ' + culprit, flush=True)
    v = run(base + [culprit], o['Tics'], o['Stress'])
    print('base + culprit alone: %s' % ('IN SYNC' if v == -1 else 'diverges at tic %d' % v if v >= 0 else 'BROKEN'))
    without = [f for f in allf if f != culprit]
    v = run(without, o['Tics'], o['Stress'])
    print('everything but the culprit: %s' % ('IN SYNC' if v == -1 else 'diverges at tic %d' % v if v >= 0 else 'BROKEN'))
    print('done in %.0f s' % (time.time() - t0))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
