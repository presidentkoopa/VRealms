#!/usr/bin/env python3
# gen_face_scenario.py pc_MAP.txt out.txt -- a camera-pose script (from actors_test --posegen) plus,
# every third frame, the pain byte and the state/move bytes of all sixteen actor slots forced through a
# cycle (0x08 0x09 0x20 0x21 0x40 0x41 0x05 0x80 0x00 0x01), so that resolve_face_surface_id
# (renderer.c:12700) is traced from every state, view and pain case. The probe's FACE, ANIM and ADV
# lines are what actors_test's facecmp / advcmp compare.
import sys
cyc = [0x08, 0x09, 0x20, 0x21, 0x40, 0x41, 0x05, 0x80, 0x00, 0x01]
L = [l for l in open(sys.argv[1]).read().splitlines() if 'vistrace' not in l and 'quit' not in l]
for f in range(4, 300, 3):
    for i in range(16):
        base = 0x91e04 + i * 0x22
        if f % 2 == 0: L.append(f"{f} w8 {base + 0x21:x} 1")
        else:
            L.append(f"{f} w8 {base + 8:x} {cyc[(f // 3 + i) % len(cyc)]:x}")
            L.append(f"{f} w8 {base + 10:x} {2 if (f // 3) % 3 == 0 else 0}")
L.append("320 quit")
L.sort(key=lambda l: int(l.split()[0]))
open(sys.argv[2], 'w').write("\n".join(L) + "\n")
