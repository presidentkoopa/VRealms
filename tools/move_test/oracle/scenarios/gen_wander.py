#!/usr/bin/env python3
"""gen_wander.py SEED TICKS > wander.txt: a held-key sequence that walks, runs, turns, strafes, jumps and
crouches around the start room for TICKS ticks -- into its walls, corners and furniture."""
import random, sys
seed, ticks = int(sys.argv[1]), int(sys.argv[2])
r = random.Random(seed)
moves = ['48', '50', '33', '34', '48,33', '48,34', '50,33', '50,34', '-']
turns = ['', '4b', '4d']
extra = ['', '', '', '', '1e', '2c', '36']
t = 5
print('# generated: gen_wander.py %d %d' % (seed, ticks))
run = 0
while t < ticks:
    if r.random() < 0.15:
        run ^= 1; print('%d w32 7e8dc %d' % (t, run)); t += 1
    k = [x for x in (r.choice(moves), r.choice(turns), r.choice(extra)) if x and x != '-']
    print('%d keys %s' % (t, ','.join(k) if k else '-'))
    t += r.randint(3, 40)
print('%d quit' % ticks)
