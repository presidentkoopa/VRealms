#!/usr/bin/env python3
"""The Colt fitted in each hand (colt_fit's dumps), from the gun's left side, its right side and an oblique (above,
behind, from the left), with the landmarks (web, beavertail, pad, trigger, the hole's centres, the socket, the muzzle).

    python3 -I fitviews.py OUTDIR TAG HANDTEX     -> OUTDIR/TAG_<hand>_<view>.png and OUTDIR/TAG_all.png
"""
import os
import sys, subprocess, numpy as np
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
from PIL import Image
O, T, TEX = sys.argv[1:4]
fits = {}
for line in open(O + '/colt_fit.txt'):
    q = line.split()
    if q and q[0].startswith('fit_'):
        R = np.array(list(map(float, q[2:11]))).reshape(3, 3)
        fits['Hand_Low' if q[0] == 'fit_right' else 'Hand_Low1'] = R
ims = []
for H in ('Hand_Low', 'Hand_Low1'):
    R = fits[H]
    gx, gy, gz = R[:, 0], R[:, 1], R[:, 2]     # the gun's right, muzzle, up in the hand frame
    views = [('gunleft', gx, gz), ('gunright', -gx, gz), ('oblique', (gx * 0.6 - gz * 0.55 + gy * 0.45), gz)]
    for n, vd, up in views:
        f = lambda v: '%f,%f,%f' % tuple(v)
        out = '%s/%s_%s_%s.png' % (O, T, H, n)
        subprocess.run(['python3', '-I', os.path.join(ROOT, 'tools/colt_test/rend.py'), out, '700', '560', 'view=' + f(vd), 'up=' + f(up),
                        'mesh=%s/%s_gun.txt:%s/vrealms_iwad/models/weapons/colt1911/colt1911_diffuse.jpg' % (O, H, ROOT),
                        'mesh=%s/%s_hand_ready.txt:%s' % (O, H, TEX), 'pts=%s/%s_pts.txt' % (O, H)], check=True)
        ims.append(Image.open(out))
s = Image.new('RGB', (2100, 1120))
for i, im in enumerate(ims):
    s.paste(im, ((i % 3) * 700, (i // 3) * 560))
s.save('%s/%s_all.png' % (O, T))
