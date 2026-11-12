#!/usr/bin/env python3
"""Compare vr_synclog files from the machines of one netgame.

  syncdiff.py [--from TIC] <log0> <log1> [<log2> ...]

--from TIC compares only tics >= TIC: for a game that was healed by a resync (a host leaving),
the question is whether the survivors agree AFTER it.

Aligns on gametic. Reports the first tic where the world hash differs, and the first tic and
field where any player line differs, naming the field. Exit 0 = in sync, 1 = diverged.
"""
import sys
HDR_P = ("pnum x y z yaw pitch health crouch viewh weapon | mh.fwd mh.left mh.up mh.yaw mh.pitch mh.roll "
         "oh.fwd oh.left oh.up oh.yaw oh.pitch oh.roll head.z head.pitch head.roll head.roomdx head.roomdy head.turn grip0 grip1 trig0 trig1 "
         "bits0 bits1 thumb0 thumb1 thumb2 thumb3 buttons weaponyaw weaponpitch | handaim playinvr override "
         "apos.x apos.y apos.z ayaw apitch aroll mainroll opos.x opos.y opos.z oyaw opitch oroll "
         "hmd.x hmd.y hmd.z hmdyaw hmdpitch hmdroll | ammo ready.ammo1 ready.ammo2 offweapon off.ammo1 off.ammo2 "
         "| torso torso.valid torso.yaw torso.rel torso.frozen neck.x neck.y neck.z chest.x chest.y chest.z "
         "pelvis.x pelvis.y pelvis.z standeye "
         "| gear h0.at h0.heading h0.closeness h0.rail h1.at h1.heading h1.closeness h1.rail mounts").split()
def hdr(i):
    return HDR_P[i] if i < len(HDR_P) else "field%d" % i
def load(path):
    tics = {}
    cur = None
    for line in open(path):
        if line.startswith("T "):
            f = line.split(); cur = int(f[1])
            tics[cur] = {"hash": f[4], "actors": f[5], "rng": f[6], "P": {}}
        elif line.startswith("P ") and cur is not None:
            f = line.split(); tics[cur]["P"][int(f[1])] = f[1:]
    return tics
args = sys.argv[1:]; start = 0
if args and args[0] == "--from": start = int(args[1]); args = args[2:]
logs = [load(p) for p in args]
common = sorted(t for t in set.intersection(*[set(l) for l in logs]) if t >= start)
if not common: print("no common tics"); sys.exit(2)
print("tics compared: %d (%d..%d)" % (len(common), common[0], common[-1]))
bad = 0
for t in common:
    a = logs[0][t]
    for k, l in enumerate(logs[1:], 1):
        b = l[t]
        if set(a["P"]) != set(b["P"]):
            pn = sorted(set(a["P"]) ^ set(b["P"]))[0]
            print("tic %d: player %d has left on one machine; comparison ends here (clean through tic %d)" % (t, pn, t - 1))
            print("IN SYNC" if bad == 0 else "DIVERGED (%d differences)" % bad); sys.exit(0 if bad == 0 else 1)
        if a["hash"] != b["hash"] or a["actors"] != b["actors"] or a["rng"] != b["rng"]:
            print("WORLD DIVERGES at tic %d (log0 vs log%d): hash %s/%s actors %s/%s rng %s/%s" %
                  (t, k, a["hash"], b["hash"], a["actors"], b["actors"], a["rng"], b["rng"]))
            bad += 1
        for pn in sorted(set(a["P"]) | set(b["P"])):
            pa, pb = a["P"].get(pn), b["P"].get(pn)
            if pa is None or pb is None:
                print("tic %d: player %d has left on one machine; comparison ends here (clean through tic %d)" % (t, pn, t - 1))
                print("IN SYNC" if bad == 0 else "DIVERGED (%d differences)" % bad); sys.exit(0 if bad == 0 else 1)
            for i, (x, y) in enumerate(zip(pa, pb)):
                if x != y and hdr(i) != "|":
                    print("tic %d player %d field %-10s log0=%s log%d=%s" % (t, pn, hdr(i), x, k, y)); bad += 1
        if bad >= 40: print("... (stopping at 40 differences)"); sys.exit(1)
print("IN SYNC" if bad == 0 else "DIVERGED (%d differences)" % bad)
sys.exit(0 if bad == 0 else 1)
