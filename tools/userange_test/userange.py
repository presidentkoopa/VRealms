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
  userange.py targets RAW targets.txt dist             A (use) and E (examine) steps at every target targets.cpp lists
                                                       (FACE, DOOR, ITEM), stood dist units off, facing it: examines
                                                       first (they change nothing), then uses
  userange.py l1diff RAW oracle.log engine.log [-v]    L1 against the original (docs/VR_INTERACTION.md 6.1): the
                                                       same eye rays (the original's PK lines at pitch 0, through
                                                       roth_l1diff in the engine): record, class, use and examine

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

def angle_to(fx, fy):
    t = math.atan2(-fx, fy)
    return int(round(t * 512 / (2 * math.pi))) % 512

def cmd_targets(raw, tfile, dist):
    m = RawMap(raw); dist = float(dist); steps = []
    FY = 355.06
    def stand(x, y, tx, ty, label, aimz=None, extra=None):
        c = sectors_at(m, (round(x), round(y)))
        if not c: return
        c.sort(key=lambda o: m.sectors[o]['floorH'])
        so = c[0]; fl = m.sectors[so]['floorH']
        a = angle_to(tx - x, ty - y)
        dy = 0
        if aimz is not None:
            d = math.hypot(tx - x, ty - y)
            dy = int(round(FY * ((fl + 144) - aimz) / max(d, 1)))
            dy = max(-235, min(235, dy))
        steps.append((round(x), round(y), a, so, fl, label, dy))
    for ln in open(tfile):
        p = ln.split()
        if not p: continue
        if p[0] == 'FACE' or p[0] == 'DFACE':
            kv = dict(x.split('=', 1) for x in p if '=' in x)
            (x1, y1), (x2, y2) = [tuple(map(int, kv[k].split(','))) for k in ('v1', 'v2')]
            nx, ny = map(float, kv['n'].split(','))
            mx, my = (x1 + x2) / 2, (y1 + y2) / 2
            sgn = 1 if p[0] == 'FACE' else -1     # a door face: from outside the door sector
            fi = p[-4] if p[0] == 'FACE' else p[1]
            idx = p[5] if p[0] == 'FACE' else p[1]
            stand(mx + sgn * nx * dist, my + sgn * ny * dist, mx, my, '%s%s' % ('f' if p[0] == 'FACE' else 'd', idx))
        elif p[0] == 'ITEM':
            kv = dict(x.split('=', 1) for x in p if '=' in x)
            x, y, z = int(kv['x']), int(kv['y']), int(kv['z'])
            # stand on the side away from the sector's walls: try the four ways, the first inside a sector
            for ang in (0, 90, 180, 270):
                t = math.radians(ang)
                sx, sy = x - math.cos(t) * dist, y - math.sin(t) * dist
                if sectors_at(m, (round(sx), round(sy))):
                    stand(sx, sy, x, y, 'i' + p[1], aimz=z + 16)
                    break
    seen = set(); out = []
    for st in steps:
        if st[5] in seen: continue
        seen.add(st[5]); out.append(st)
    for op in ('E', 'A'):
        for x, y, a, so, fl, label, dy in out:
            print('%s %d %d %d %d %d 0 %s%s | s 0 %d' % (op, x, y, a, so, fl, op.lower(), label, dy))
    print('Q')

def cls_of(itype, shape, depth):
    # roth_vrrules.h ClassOf
    if itype == 3 and depth < 600: return 'usable'
    if itype == 5: return 'blocked'
    if shape in (0x398, 0x3a0): return 'too far'
    if itype == 3: return 'too far'
    return 'nothing'

def parse_l1(log):
    rows = {}
    for ln in open(log):
        p = ln.split()
        if not p or p[0] != 'L1': continue
        r = dict(label=p[1], px=int(p[2]), py=int(p[3]), pa=int(p[4]), dx=int(p[5]), dy=int(p[6]))
        last = None
        for kv in p[7:]:
            if '=' not in kv and last: r[last] += ' ' + kv; continue   # a class name with a space ("too far")
            k, v = kv.split('=', 1); r[k] = v; last = k
        rows[(r['label'], r['dx'], r['dy'])] = r
    return rows

def cmd_l1diff(raw, olog, elog, verbose=False):
    m = RawMap(raw)
    face0 = 30 + m.nsec * 26 + 2
    eng = parse_l1(elog)
    acts = {}
    for ln in open(olog):
        p = ln.split()
        if p and p[0] in ('ACT', 'EXA', 'USE'): acts.setdefault(p[1], []).append(ln.strip())
    n = rec_ok = cls_yaw_ok = cls_d2_ok = centre = centre_d2_ok = 0
    by = {}
    bad = []
    for r in parse(olog):
        if r['pitch'] != 0: continue
        e = eng.get((r['label'], r['dx'], r['dy']))
        if e is None: bad.append(('missing', r['label'], r['dx'], r['dy'])); continue
        n += 1
        ot = r['type'] if r['type'] != 5 else 0          # the parallax sky (5) is no Realms thing: the port has none
        et = int(e['type'])
        if ot in (1, 2, 7, 8): want = ('sec', (r['sec'] - 30) // 26)
        elif ot == 3: want = ('face', (r['face'] - face0) // 12)
        elif ot == 4: want = ('obj', (r.get('ox'), r.get('oy'), r.get('oz')))
        elif ot == 6: want = ('sec', (r['sec'] - 30) // 26)
        else: want = ('none', None)
        if want[0] == 'sec': got = int(e['sec'])
        elif want[0] == 'face': got = int(e['face'])
        elif want[0] == 'obj': got = tuple(int(x) for x in e['objxyz'].split(','))
        else: got = None
        same_rec = ot == et and got == want[1]
        k = by.setdefault(ot, [0, 0]); k[0] += 1
        if same_rec: rec_ok += 1; k[1] += 1
        oc = cls_of(r['itype'] if r['cursor'] >= 0 else 1, r['cursor'] if r['cursor'] >= 0 else 0x240, r['depth'])
        it, sh, nm = e['cy'].split('/', 2)
        ecy = (int(it), int(sh, 16))
        ocur = (r['itype'], r['cursor']) if r['cursor'] >= 0 else (1, 0x240)
        same_yaw = same_rec and ecy == ocur
        if same_yaw: cls_yaw_ok += 1
        it2, sh2, nm2 = e['c2'].split('/', 2)
        same_d2 = same_rec and cls_of(int(it2), int(sh2, 16), float(e['d2'])) == oc
        if same_d2: cls_d2_ok += 1
        if r['dx'] == 0:
            centre += 1
            if same_d2: centre_d2_ok += 1
        if not (same_rec and same_yaw):
            bad.append(('rec' if not same_rec else 'cls', r['label'], r['dx'], r['dy'], 'orig type %d %s %s depth %d cursor %d/%x (%s)' % (r['type'], want[0], want[1], r['depth'], ocur[0], ocur[1], oc),
                        'L1 type %d %s depth(yaw) %s cursor %s depth(D2) %s %s hit %s' % (et, got, e['yaw'], e['cy'], e['d2'], e['c2'], e['hit'])))
    # THE RESULT: what the original's activate / examine reached at an A / E step (its HOOK lines), against what L1's
    # use and examine dispatchers match there (read only: the record, an object's pickup, a door)
    res_n = res_ok = 0; res_by = {}
    cur = None; hooks = []
    for ln in open(olog):
        p = ln.split()
        if not p: continue
        if p[0] == 'PK': cur = (p[1], int(p[7]), int(p[8])); hooks = []
        elif p[0] == 'HOOK': hooks.append(p[1:])
        elif p[0] in ('ACT', 'EXA') and cur is not None:
            e = eng.get(cur)
            if e is None: continue
            res_n += 1
            if p[0] == 'ACT':
                o = ('nothing',)
                for h in hooks:
                    if h[0].startswith('fire_command_contact_trigger'): o = ('chain', int(h[1], 16))
                    elif h[0] == 'give_item_by_dbase_id': o = ('give',)
                    elif h[0] == 'toggle_door_open_state': o = ('door',)
                et = int(e['type']); ur, uo, uk = [int(x, 16) if i == 1 else int(x) for i, x in enumerate(e['use'].split(':'))]
                within = float(e['yaw']) < 600
                if not within or et == 0: g = ('nothing',)
                elif ur: g = ('chain', uk)
                elif et == 4 and int(e['pickup']) or (et == 4 and int(e['item']) > 0): g = ('give',)
                elif et == 6: g = ('door',)
                else: g = ('nothing',)
            else:
                o = ('nothing',)
                for h in hooks:
                    if h[0].startswith('examine_world_object'): o = ('examine', int(h[1], 16))
                    elif h[0].startswith('eval_dialogue_record_condition'): o = ('inspect',)
                et = int(e['type']); er, eo, ek = [int(x, 16) if i == 1 else int(x) for i, x in enumerate(e['exa'].split(':'))]
                if er: g = ('examine', ek)
                elif et == 4 and int(e['ex2']) > 0: g = ('inspect',)
                else: g = ('nothing',)
            k = res_by.setdefault((p[0], o[0]), [0, 0]); k[0] += 1
            if o == g: res_ok += 1; k[1] += 1
            else: bad.append(('result', cur[0], cur[1], cur[2], 'orig %s %s' % (p[0], o), 'L1 %s (type %s, use %s, exa %s, yaw depth %s)' % (g, e['type'], e['use'], e['exa'], e['yaw'])))
    print('L1 vs the original, %s: %d eye rays at pitch 0' % (os.path.basename(raw), n))
    print('  record (type and face / sector / object) the same: %d / %d' % (rec_ok, n))
    for t in sorted(by): print('    original type %d: %d / %d' % (t, by[t][1], by[t][0]))
    print('  class with the original\'s own measure (z along the view yaw): cursor type and shape the same: %d / %d' % (cls_yaw_ok, n))
    print('  class by D2 (horizontal distance) the same as the original\'s: %d / %d (centre column, where D2 = the original\'s z: %d / %d)' % (cls_d2_ok, n, centre_d2_ok, centre))
    if res_n:
        print('  result (what a use / an examine there reaches) the same: %d / %d' % (res_ok, res_n))
        for k in sorted(res_by): print('    original %s %s: %d / %d' % (k[0], k[1], res_by[k][1], res_by[k][0]))
    if verbose or len(bad) <= 60:
        for b in bad: print('  DIFF', *b)
    else:
        for b in bad[:60]: print('  DIFF', *b)
        print('  ... %d more' % (len(bad) - 60))

if __name__ == '__main__':
    c = sys.argv[1]; a = sys.argv[2:]
    if c == 'random': cmd_random(a[0], a[1], a[2], a[3] if len(a) > 3 else 0, a[4:] or ('c',))
    elif c == 'line': cmd_line(*a[:9], samples=a[9:] or ('c',))
    elif c == 'pose': cmd_pose(*a[:7], samples=a[7:])
    elif c == 'walls': cmd_walls(a[0], a[1])
    elif c == 'fit': cmd_fit(a[0], a[1:])
    elif c == 'objs': cmd_objs(a[0])
    elif c == 'targets': cmd_targets(a[0], a[1], a[2])
    elif c == 'l1diff': cmd_l1diff(a[0], a[1], a[2], '-v' in a[3:])
    else: sys.exit(__doc__)
