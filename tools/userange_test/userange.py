#!/usr/bin/env python3
"""userange.py -- scripts for rangeprobe.c and the analysis of its logs.

  userange.py random RAW N seed [pitch] [samples...]   random poses inside the sectors of RAW (one script line each)
  userange.py line RAW x0 y0 angle d0 d1 step pitch label [samples...]
                                                       poses backing away along -forward from (x0,y0): the
                                                       eye at distance d from (x0,y0) for d in d0..d1
  userange.py pose RAW OP x y angle pitch label [samples...]
                                                       one script line (OP = P, A or E) with the sector and floor of (x,y)
  userange.py walls RAW log                            wall picks: depth against the hit's distance along the view
  userange.py fit RAW log...                           wall centre picks of the logs: depth against t, 2t+64 residuals and the fit
  userange.py objs log                                 object picks: depth against the object's view distance
  userange.py floors RAW log                           floor picks of column scans: depth at the sector edges

Conventions (measured, see docs/REMAROTH_MEASURED.md): angle512 0 = +y, 128 = -x (CCW from +Y), so
forward = (-sin, cos). Record words: +8 = the face's file offset, +0xa = the sector's file offset.
"""
import math, os, random, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'oracle'))
from rawmap import RawMap, sectors_at, sector_edges

def fwd(a):
    t = a * 2 * math.pi / 512
    return -math.sin(t), math.cos(t)

def parse(log):
    rows = []
    for ln in open(log):
        p = ln.split()
        if not p or p[0] != 'PK': continue
        r = dict(label=p[1], px=int(p[2]), py=int(p[3]), pz=int(p[4]), pa=int(p[5]), pitch=int(p[6]), dx=int(p[7]), dy=int(p[8]),
                 kind=int(p[9]), type=int(p[10]), das=int(p[11], 16), tv=int(p[12], 16), tu=int(p[13], 16), face=int(p[14], 16),
                 sec=int(p[15], 16), defp=int(p[16], 16), door=int(p[17], 16), depth=int(p[18]), cursor=int(p[19], 16) if p[19] != 'ffffffff' else -1,
                 itype=int(p[20]), flags=int(p[21], 16), cnt=int(p[22]), latch=int(p[23]))
        if len(p) > 27: r.update(ox=int(p[25]), oy=int(p[26]), oz=int(p[27]))
        rows.append(r)
    return rows

def ray_face(m, r, face):
    f = m.faces.get(face)
    if not f: return None
    (x1, y1), (x2, y2) = m.vertex(f['v1']), m.vertex(f['v2'])
    fx, fy = fwd(r['pa'])
    # solve p + t*f = v1 + u*(v2-v1)
    ex, ey = x2 - x1, y2 - y1
    den = fx * (-ey) - fy * (-ex)
    if abs(den) < 1e-9: return None
    bx, by = x1 - r['px'], y1 - r['py']
    t = (bx * (-ey) - by * (-ex)) / den
    u = (fx * by - fy * bx) / den
    return t, u

def cmd_random(raw, n, seed, pitch=0, samples=('c',)):
    m = RawMap(raw); rnd = random.Random(int(seed)); out = []
    secs = [o for o, s in m.sectors.items() if s['mid'] == 0 and s['trig'] == 0 and s['ceilH'] - s['floorH'] >= 200 and sector_edges(m, o)]
    while len(out) < int(n):
        so = rnd.choice(secs); e = sector_edges(m, so)
        xs = [p[0] for a in e for p in a]; ys = [p[1] for a in e for p in a]
        x, y = rnd.randint(min(xs), max(xs)), rnd.randint(min(ys), max(ys))
        c = sectors_at(m, (x, y))
        if c != [so]: continue
        # keep the eye 30 units clear of every edge of its sector
        if any(seg_dist((x, y), a, b) < 30 for a, b in e): continue
        a = rnd.randrange(512)
        out.append('P %d %d %d %d %d %d r%d | %s' % (x, y, a, so, m.sectors[so]['floorH'], int(pitch), len(out), ' '.join(samples)))
    print('\n'.join(out)); print('Q')

def seg_dist(p, a, b):
    (px, py), (ax, ay), (bx, by) = p, a, b
    dx, dy = bx - ax, by - ay; L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)

def cmd_line(raw, x0, y0, a, d0, d1, step, pitch, label, samples=('c',)):
    m = RawMap(raw); fx, fy = fwd(int(a)); out = []
    for d in range(int(d0), int(d1) + 1, int(step)):
        x, y = round(int(x0) - fx * d), round(int(y0) - fy * d)
        c = sectors_at(m, (x, y))
        if not c: continue
        c.sort(key=lambda o: m.sectors[o]['floorH'])
        out.append('P %d %d %d %d %d %d %s_%d | %s' % (x, y, int(a), c[0], m.sectors[c[0]]['floorH'], int(pitch), label, d, ' '.join(samples)))
    print('\n'.join(out)); print('Q')

def cmd_pose(raw, op, x, y, a, pitch, label, samples=()):
    m = RawMap(raw); c = sectors_at(m, (int(x), int(y)))
    if not c: sys.exit('no sector at %s,%s' % (x, y))
    c.sort(key=lambda o: m.sectors[o]['floorH'])
    line = '%s %d %d %d %d %d %d %s' % (op, int(x), int(y), int(a), c[0], m.sectors[c[0]]['floorH'], int(pitch), label)
    print(line + (' | ' + ' '.join(samples) if samples else ''))

def cmd_walls(raw, log):
    m = RawMap(raw)
    for r in parse(log):
        if r['type'] != 3 or r['dx'] != 0 or r['dy'] != 0: continue
        h = ray_face(m, r, r['face'])
        if not h: print('NOFACE', r['label'], hex(r['face'])); continue
        t, u = h
        print('W %s face=%04x t=%.1f u=%.3f depth=%d pitch=%d das=%d' % (r['label'], r['face'], t, u, r['depth'], r['pitch'], r['das']))

def cmd_fit(raw, logs):
    m = RawMap(raw); pts = []
    for log in logs:
        for r in parse(log):
            if r['type'] != 3 or r['dx'] or r['dy']: continue
            h = ray_face(m, r, r['face'])
            if h and h[0] > 0 and -0.01 <= h[1] <= 1.01: pts.append((h[0], r['depth']))
    res = sorted(d - (2 * t + 64) for t, d in pts)
    inl = [(t, d) for t, d in pts if abs(d - (2 * t + 64)) < 10]
    n = len(inl); st = sum(t for t, d in inl); sd = sum(d for t, d in inl); stt = sum(t * t for t, d in inl); std = sum(t * d for t, d in inl)
    k = (n * std - st * sd) / (n * stt - st * st); b = (sd - k * st) / n
    print('%d wall centre picks; residual of depth - (2t+64): median %.1f; |r|<4: %d; |r|<10: %d' % (len(pts), res[len(res) // 2],
          sum(abs(x) < 4 for x in res), n))
    print('fit over |r|<10: depth = %.4f*t + %.2f -> depth 600 at t=%.1f, 2000 at t=%.1f, 5000 at t=%.1f' % (k, b, (600 - b) / k, (2000 - b) / k, (5000 - b) / k))

def cmd_objs(log):
    for r in parse(log):
        if r['type'] != 4 or 'ox' not in r: continue
        fx, fy = fwd(r['pa']); vx, vy = r['ox'] - r['px'], r['oy'] - r['py']
        z = vx * fx + vy * fy; e = math.hypot(vx, vy)
        print('O %s dx=%d dy=%d z=%.1f euclid=%.1f oz-pz=%d depth=%d cursor=%x itype=%d pitch=%d' % (r['label'], r['dx'], r['dy'], z, e, r['oz'] - r['pz'], r['depth'], r['cursor'], r['itype'], r['pitch']))

if __name__ == '__main__':
    c = sys.argv[1]; a = sys.argv[2:]
    if c == 'random': cmd_random(a[0], a[1], a[2], a[3] if len(a) > 3 else 0, a[4:] or ('c',))
    elif c == 'line': cmd_line(*a[:9], samples=a[9:] or ('c',))
    elif c == 'pose': cmd_pose(*a[:7], samples=a[7:])
    elif c == 'walls': cmd_walls(a[0], a[1])
    elif c == 'fit': cmd_fit(a[0], a[1:])
    elif c == 'objs': cmd_objs(a[0])
    else: sys.exit(__doc__)
