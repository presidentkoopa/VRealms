#!/usr/bin/env python3
"""The Colt held in two hands (support_fit's dumps, gun metres): from the off side (as the owner's picture's panels: the
gun's left side for a right-handed shooter, the muzzle to the left), the shooting side, the top, an oblique (below, in
front, from the off side) and the shooter's eye (behind and above). Each pairing: OUT/<tag>_<pair>_<view>.png and one
sheet OUT/<tag>_<pair>_all.png.

    python3 -I supportviews.py OUTDIR TAG HANDTEX [pair ...]
"""
import os
import sys, subprocess
from PIL import Image
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
O, T, TEX = sys.argv[1:4]
pairs = [int(p) for p in sys.argv[4:]] or [0, 1]
SUPTEX = os.environ.get('SUPPORT_SKIN') and TEX or '0.55,0.75,0.95'   # the support hand blue (to tell the hands apart), or skinned
GUNTEX = '%s/vrealms_iwad/models/weapons/colt1911/colt1911_diffuse.jpg' % ROOT
for p in pairs:
    off = -1.0 if p == 0 else 1.0        # pair 0: the right hand shoots, the off side is the gun's -x
    views = [('offside', (-off, 0, 0), (0, 0, 1)), ('shootside', (off, 0, 0), (0, 0, 1)), ('top', (0, 0, -1), (0, 1, 0)),
             ('below', (-off * 0.5, -0.6, 0.6), (0, 0, 1)), ('eye', (0, 0.85, -0.5), (0, 0.5, 0.85))]
    ims = []
    for n, vd, up in views:
        f = lambda v: '%f,%f,%f' % tuple(v)
        out = '%s/%s_%d_%s.png' % (O, T, p, n)
        subprocess.run(['python3', '-I', os.path.join(ROOT, 'tools/colt_test/rend.py'), out, '640', '520', 'view=' + f(vd), 'up=' + f(up),
                        'mesh=%s/sup%d_gun.txt:%s' % (O, p, GUNTEX), 'mesh=%s/sup%d_main.txt:%s' % (O, p, TEX),
                        'mesh=%s/sup%d_sup.txt:%s' % (O, p, SUPTEX), 'span=0.24', 'center=0,%f,%f' % (-0.035, -0.01)], check=True)
        ims.append(Image.open(out))
    s = Image.new('RGB', (640 * 3, 520 * 2))
    for i, im in enumerate(ims):
        s.paste(im, ((i % 3) * 640, (i // 3) * 520))
    s.save('%s/%s_%d_all.png' % (O, T, p))
