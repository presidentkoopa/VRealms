#!/usr/bin/env python3
"""Join voxelize_test's per-film pictures into one contact sheet.

    contact_sheet.py <pictures dir> <out.png> <table.txt> [item,item,...]

<pictures dir> holds film_<item>.ppm from `voxelize_test --pictures`; the
table is voxelize_test's stdout (for the labels). The pictures are made from
the player's retail art: keep both outside the repository.
Needs Pillow.
"""
import os
import sys

from PIL import Image, ImageDraw


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    pics, out, table = sys.argv[1], sys.argv[2], sys.argv[3]
    labels = {}
    for line in open(table, encoding='latin-1'):
        if line.startswith('#'):
            continue
        f = line.rstrip('\n').split('|')
        if len(f) < 15:
            continue
        first = f[0].split(',')[0]
        labels[first] = '%s %s  P=%s dir=%s elev=%s  IoU %s (min %s)' % (f[0][:20], f[1], f[5], f[6], f[7].split(' ')[0], f[12], f[13])
    if len(sys.argv) > 4:
        items = sys.argv[4].split(',')
    else:
        items = sorted((n[5:-4] for n in os.listdir(pics) if n.startswith('film_') and n.endswith('.ppm')), key=int)
    tiles = []
    for it in items:
        p = os.path.join(pics, 'film_%s.ppm' % it)
        if not os.path.exists(p):
            continue
        im = Image.open(p).convert('RGB')
        s = min(1.0, 900.0 / im.width)
        im = im.resize((int(im.width * s), int(im.height * s)))
        lab = Image.new('RGB', (im.width, 16), (0, 0, 0))
        ImageDraw.Draw(lab).text((4, 2), labels.get(it, it), fill=(255, 255, 0))
        t = Image.new('RGB', (im.width, im.height + 16))
        t.paste(lab, (0, 0))
        t.paste(im, (0, 16))
        tiles.append(t)
    if not tiles:
        print('no pictures')
        return 1
    cols = 2
    rows = (len(tiles) + cols - 1) // cols
    cw = max(t.width for t in tiles) + 6
    rh = [max(t.height for t in tiles[r * cols:(r + 1) * cols]) + 6 for r in range(rows)]
    sheet = Image.new('RGB', (cw * cols, sum(rh)), (60, 60, 60))
    y = 0
    for r in range(rows):
        for c in range(cols):
            i = r * cols + c
            if i < len(tiles):
                sheet.paste(tiles[i], (c * cw, y))
        y += rh[r]
    sheet.save(out)
    print('wrote %s (%d films)' % (out, len(tiles)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
