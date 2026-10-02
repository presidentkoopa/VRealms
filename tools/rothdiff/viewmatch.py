"""Settle the directional view ORDER by matching pictures, not by derivation.

For each of the original's eight views of one directional prop, score it against
ALL eight of ours and report which fits best. If the best match is the diagonal
-- original view i against our view i -- then view i maps to rotation i and
HANDOFF_REMAROTH.md section 7's derivation holds. A constant offset down the
diagonal means every directional prop is rotated by that many views; a reversed
diagonal means the sense is flipped, which is what ANGLE_SENSE controls.

Only a tight crop around the prop is scored. The backgrounds legitimately differ
(trees, sky, lighting are separate open items) and would swamp the signal.

    python viewmatch.py [captures-dir]
"""

import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("viewmatch: needs Pillow")

# The camera looks straight at the prop in every orbit shot, so it lands in the
# middle. Tight enough to exclude the trees and the sky.
BOX = (265, 170, 375, 380)
N = 8


def load(path):
    im = Image.open(path).convert("L").crop(BOX)
    px = list(im.getdata())
    n = len(px)
    mean = sum(px) / n
    # Mean-removed, so a brightness difference between the engines (a known
    # open item of its own) cannot decide which view matched.
    return [v - mean for v in px]


def ncc(a, b):
    num = sum(x * y for x, y in zip(a, b))
    da = sum(x * x for x in a) ** 0.5
    db = sum(y * y for y in b) ** 0.5
    return num / (da * db) if da > 0 and db > 0 else 0.0


def main(argv):
    cap = argv[1] if len(argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "captures")

    orig, ours = [], []
    for i in range(N):
        po = os.path.join(cap, "ORACLE_dir%d.png" % i)
        pm = os.path.join(cap, "REMA_dir%d.png" % i)
        if not (os.path.exists(po) and os.path.exists(pm)):
            sys.exit("missing %s or %s -- capture the orbit first" % (po, pm))
        orig.append(load(po))
        ours.append(load(pm))

    print("correlation of ORIGINAL view (rows) against OURS (columns)\n")
    print("        " + "".join("  ours%d" % j for j in range(N)))
    offsets = {}
    for i in range(N):
        scores = [ncc(orig[i], ours[j]) for j in range(N)]
        best = max(range(N), key=lambda j: scores[j])
        offsets[i] = (best - i) % N
        row = "".join(("%7.2f" % s) if j != best else ("%6.2f*" % s)
                      for j, s in enumerate(scores))
        print("orig%d  %s   best=ours%d" % (i, row, best))

    print()
    tally = {}
    for v in offsets.values():
        tally[v] = tally.get(v, 0) + 1
    for off, n in sorted(tally.items(), key=lambda kv: -kv[1]):
        print("  offset %+d : %d of %d views" % (off, n, N))

    best_off, best_n = max(tally.items(), key=lambda kv: kv[1])
    print()
    if best_off == 0 and best_n == N:
        print("VERDICT: view i -> rotation i on all %d views. The derivation holds." % N)
        return 0
    if best_off == 0:
        print("VERDICT: diagonal on %d of %d. Check the %d that disagree before"
              " trusting it." % (best_n, N, N - best_n))
        return 0
    print("VERDICT: the views are offset by %+d (%d of %d agree on it)."
          " Every directional prop is rotated by that much." % (best_off, best_n, N))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
