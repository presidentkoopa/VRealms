#!/usr/bin/env python3
"""Per-player console script for run.sh: locomotion, weapon switches and a fight that the
replay cannot drive (it only sets hands and buttons). `wait N` is in tics from script start.
  mkexec.py <out.cfg> <player>"""
import sys
out, pn = sys.argv[1], int(sys.argv[2])
L = ["wait 60", "+forward", "wait 50", "-forward", "+moveleft" if pn % 2 else "+moveright", "wait 30",
     "-moveleft" if pn % 2 else "-moveright", "slot %d" % (2 + pn % 3), "wait 25", "+attack", "wait 20", "-attack",
     "slot 1", "wait 20", "+back", "wait 40", "-back", "+left", "wait 30", "-left", "+attack", "wait 60", "-attack",
     "+forward", "wait 120", "-forward", "+jump", "wait 5", "-jump", "+attack", "wait 60", "-attack"]
open(out, "w").write("; ".join(L) + "\n")
