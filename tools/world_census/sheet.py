#!/usr/bin/env python3
# sheet.py -- contact sheets of the object kinds and wall-trigger pictures that world_census -dump wrote.
#
#   world_census <DATA> > census.txt
#   world_census <DATA> -dump /tmp/wc/art          (then convert the .ppm files to .png, or leave them)
#   python3 -I tools/world_census/sheet.py census.txt /tmp/wc/art /tmp/wc/sheets
#
# The pictures are the retail art: write sheets to /tmp only, never into the repo.
# One cell per kind, labelled "PACK[index] xN what" with the click/switch/examine/item flags, in the census's
# order (most placed first). Meshes have no picture here (render them with tools/propcheck/proprender).
import os, re, sys
from PIL import Image, ImageDraw

census, artdir, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(outdir, exist_ok=True)
CELL, LABEL, COLS = 150, 34, 8

def load(stem):
    for ext in ('.png', '.ppm'):
        p = os.path.join(artdir, stem + ext)
        if os.path.exists(p):
            im = Image.open(p).convert('RGB')
            px = im.load()
            # magenta = transparent in the dump: show it as dark grey
            for y in range(im.height):
                for x in range(im.width):
                    if px[x, y] == (255, 0, 255):
                        px[x, y] = (40, 40, 40)
            return im
    return None

def cells_from(section, rx):
    rows, on = [], False
    for line in open(census, encoding='latin-1'):
        if line.startswith(section):
            on = True
            continue
        if on and not line.strip():
            break
        if on:
            m = rx.match(line)
            if m:
                rows.append((m, line))
    return rows

def sheet(rows, stem_of, label_of, name):
    if not rows:
        return
    per = COLS * 6
    for page in range(0, len(rows), per):
        chunk = rows[page:page + per]
        n_rows = (len(chunk) + COLS - 1) // COLS
        out = Image.new('RGB', (COLS * CELL, n_rows * (CELL + LABEL)), (20, 20, 20))
        d = ImageDraw.Draw(out)
        for i, (m, line) in enumerate(chunk):
            cx, cy = (i % COLS) * CELL, (i // COLS) * (CELL + LABEL)
            im = load(stem_of(m))
            if im is not None:
                s = min((CELL - 6) / im.width, (CELL - 6) / im.height, 4.0)
                im = im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.NEAREST)
                out.paste(im, (cx + (CELL - im.width) // 2, cy + (CELL - im.height) // 2))
            else:
                d.text((cx + 40, cy + 60), '(no picture)', fill=(120, 120, 120))
            for k, t in enumerate(label_of(m, line)):
                d.text((cx + 3, cy + CELL + 1 + 11 * k), t[:26], fill=(230, 230, 160) if k == 0 else (190, 190, 190))
        p = os.path.join(outdir, '%s_%02d.png' % (name, page // per + 1))
        out.save(p)
        print(p)

obj_rx = re.compile(r'^(\w+)\[(\d+)\]\s+(\S+)\s+(\S+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+) \|\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)')
def obj_label(m, line):
    flags = []
    for name, g in (('click', 9), ('SWITCH', 10), ('exam', 11), ('hit', 12), ('walk', 13)):
        if int(m.group(g)):
            flags.append('%s%s' % (name, m.group(g)))
    item = re.search(r'ITEM (\d+) (\w+) "([^"]*)"', line)
    third = ('item %s %s' % (item.group(1), item.group(3))) if item else ''
    return ['%s[%s] x%s' % (m.group(1), m.group(2), m.group(5)), '%s %s %s' % (m.group(3), m.group(4), ' '.join(flags)), third]

rows = cells_from('art ', obj_rx)
rows = [r for r in rows if r[0].group(3) not in ('mesh', 'creature', 'empty')]
sheet(rows, lambda m: 'obj_%s_%s' % (m.group(1), m.group(2)), obj_label, 'objects')

face_rx = re.compile(r'^(\w+)\[(\d+)\]\s+(\w+)\s+(\S+) \|\s+(\d+)\s+(\d+)\s+(\d+) \| (\S+)')
def face_label(m, line):
    return ['%s[%s] %s' % (m.group(1), m.group(2), m.group(3)), 'use%s exam%s hit%s' % (m.group(5), m.group(6), m.group(7)), m.group(8)]
rows = cells_from('picture ', face_rx)
sheet(rows, lambda m: 'tex_%s_%s' % (m.group(1), m.group(2)), face_label, 'walls')
