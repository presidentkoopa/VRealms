#!/usr/bin/env python3
# plan.py -- a plan view of an `openables -area ... -poly FILE` dump, to see what the walk reached.
#
#   /tmp/openables "$ROTH_DATA" -area STUDY1 -stop 29 -have 1,15,16,68 -poly /tmp/plan.txt > /tmp/area.txt
#   python3 -I tools/openables/plan.py /tmp/plan.txt /tmp/plan.png [x0,x1,y0,y1] [scale]
#
# Geometry only (sector outlines, record numbers), no retail art; still write it to /tmp. Colours: green = walked,
# blue = touchable (one or two portals off the walk), orange = a door sector walked through, dark red = a door not
# opened, near black = a closed sector (ceiling at the floor). Yellow dots = objects with records (magenta = hidden at
# load), cyan lines = faces with use/examine/attack-hit records, red ring = the map's start. Labels: s<sector> and
# its floor height. Map y grows upward.
import sys
from PIL import Image, ImageDraw

poly, out = sys.argv[1], sys.argv[2]
bbox = [int(v) for v in sys.argv[3].split(',')] if len(sys.argv) > 3 and sys.argv[3] else None
sc = float(sys.argv[4]) if len(sys.argv) > 4 else 0.25
S, O, F, P = [], [], [], None
for line in open(poly):
    t = line.split()
    if not t:
        continue
    if t[0] == 'S':
        idx, st, cid, fl, ce, li, door = map(int, t[1:8])
        pts = list(map(int, t[8:]))
        S.append((idx, st, cid, fl, ce, door, [(pts[i], pts[i + 1]) for i in range(0, len(pts), 2)]))
    elif t[0] == 'O':
        O.append(t[1:])
    elif t[0] == 'F':
        F.append(t[1:])
    elif t[0] == 'P':
        P = (int(t[1]), int(t[2]))
if bbox:
    x0, x1, y0, y1 = bbox
else:
    xs = [x for s in S if s[1] for x, y in s[6]]
    ys = [y for s in S if s[1] for x, y in s[6]]
    x0, x1, y0, y1 = min(xs) - 100, max(xs) + 100, min(ys) - 100, max(ys) + 100
W, H = int((x1 - x0) * sc), int((y1 - y0) * sc)
im = Image.new('RGB', (W, H), (20, 20, 20))
d = ImageDraw.Draw(im)

def tp(x, y):
    return ((x - x0) * sc, (y1 - y) * sc)

for idx, st, cid, fl, ce, door, pts in S:
    if len(pts) < 3:
        continue
    col = (55, 55, 55)
    if st == 2:
        col = (160, 80, 40) if door else (60, 100, 60)
    elif st == 1:
        col = (60, 60, 120)
    elif door:
        col = (110, 50, 50)
    if ce <= fl:
        col = (35, 35, 35)
    d.polygon([tp(x, y) for x, y in pts], fill=col, outline=(120, 120, 120))
for idx, st, cid, fl, ce, door, pts in S:
    if len(pts) < 3:
        continue
    cx = sum(p[0] for p in pts) / len(pts)
    cy = sum(p[1] for p in pts) / len(pts)
    if x0 < cx < x1 and y0 < cy < y1:
        d.text(tp(cx, cy), f"s{idx}\n{fl}", fill=(200, 200, 200))
for s, ax, ay, bx, by, fi, recs in F:
    d.line([tp(int(ax), int(ay)), tp(int(bx), int(by))], fill=(0, 220, 255), width=3)
    d.text(tp((int(ax) + int(bx)) / 2, (int(ay) + int(by)) / 2), recs, fill=(0, 255, 255))
for s, x, y, cid, word, hid, recs in O:
    if recs != '-':
        X, Y = tp(int(x), int(y))
        d.ellipse([X - 3, Y - 3, X + 3, Y + 3], fill=(255, 200, 0) if hid == '0' else (255, 0, 255))
        d.text((X + 4, Y - 4), recs, fill=(255, 230, 120))
if P:
    X, Y = tp(*P)
    d.ellipse([X - 6, Y - 6, X + 6, Y + 6], outline=(255, 0, 0), width=2)
im.save(out)
print(out, W, H)
