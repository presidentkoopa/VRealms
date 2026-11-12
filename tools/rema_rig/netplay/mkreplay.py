#!/usr/bin/env python3
"""Write a vr_replay_play file for one netplay test player.

  mkreplay.py <out.txt> <player 0..3>

Each player gets the same script with its own yaw offset and timing, so every machine has to
receive hands it did not generate. Wire units (d_protocol.h): positions 1/16 map unit in the
player's own frame (fwd, left, up); angles 65536/360 per degree; analogs 0-255.
Columns: rep mh.pos3 mh.ang3 oh.pos3 oh.ang3 head.z head.pitch head.roll
         grip0 grip1 trig0 trig1 bits0 bits1 thumb4 buttons
"""
import sys
P = lambda mu: int(round(mu * 16))
A = lambda deg: int(round(deg * 65536 / 360)) % 65536
A = lambda deg: ((int(round(deg * 65536 / 360)) + 32768) % 65536) - 32768   # signed short
BT_ATTACK, BT_USE, BT_RELOAD, BT_OFFHANDATTACK = 1 << 0, 1 << 1, 1 << 6, 1 << 26
VALID = 1

out, pn = sys.argv[1], int(sys.argv[2])
yo = 15 * pn            # per-player yaw offset, degrees
rows = []
def row(rep, mh, mha, oh, oha, headz=41, hp=0, hr=0, grip=(0, 0), trig=(0, 0), bits=(VALID, VALID), buttons=0):
    rows.append([rep, *map(P, mh), *map(A, mha), *map(P, oh), *map(A, oha), P(headz), A(hp), A(hr),
                 *grip, *trig, *bits, 0, 0, 0, 0, buttons])
rest_m, rest_o = (24, -8, 44), (24, 8, 44)
row(70, rest_m, (yo, 0, 0), rest_o, (yo, 0, 0))                       # 0-69 at rest
for i in range(14):                                                   # 70-139 off hand yaws 0->90, pitches, rolls
    t = i / 13
    row(5, rest_m, (yo, 0, 0), rest_o, (yo + 90 * t, -30 * t, 45 * t))
for i in range(14):                                                   # 140-209 main hand sweeps and rolls
    t = i / 13
    row(5, (24 + 10 * t, -8 - 12 * t, 44 + 6 * t), (yo + 40 * t, 20 * t, -60 * t), rest_o, (yo + 90, -30, 45))
for i in range(8):                                                    # 210-249 grip then trigger squeeze
    row(5, rest_m, (yo, 0, 0), rest_o, (yo, 0, 0), grip=(0, 255), trig=(min(255, 40 * i), 0))
row(10, rest_m, (yo, 0, 0), rest_o, (yo, 0, 0), grip=(0, 255), trig=(255, 0), buttons=BT_ATTACK)   # 250-259 fire
row(20, rest_m, (yo, 0, 0), rest_o, (yo, 0, 0))                       # 260-279
row(40, (20, -8, 30), (yo, 0, 0), (20, 8, 30), (yo, 0, 0), headz=28)  # 280-319 crouch (head low)
row(10, rest_m, (yo, 0, 0), rest_o, (yo, 0, 0), buttons=BT_OFFHANDATTACK)                         # 320-329 off trigger
row(60, rest_m, (yo, 0, 0), rest_o, (yo, 0, 0))                       # 330-389 rest
with open(out, "w") as f:
    f.write("# vrreplay 1\n# netplay test replay, player %d (yaw offset %d)\n" % (pn, yo))
    for r in rows: f.write(" ".join(map(str, r)) + "\n")
print(out, sum(r[0] for r in rows), "tics")
