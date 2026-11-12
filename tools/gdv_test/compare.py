#!/usr/bin/env python3
"""compare.py <original.trace> <port lines...> -- diff gdvprobe.c's trace (the original) against
gdvtool's output (the port), movie by movie: every record's decode-buffer, movie-palette, DAC and
audio CRC, every subtitle event, and the totals. Prints one summary line per movie and the first
differences. Exit 0 when everything matches."""
import sys, re

def parse(lines):
    movies = {}; cur = None
    for ln in lines:
        ln = ln.rstrip('\n')
        if ln.startswith('M '):
            cur = ln.split()[1]; movies[cur] = {'F': {}, 'S': [], 'T': None}
        elif cur is None: continue
        elif ln.startswith('F '):
            p = ln.split(); k = int(p[1]); movies[cur]['F'][k] = dict(x.split('=') for x in p[2:])
        elif ln.startswith('S '):
            p = ln.split(); k = int(p[1])
            if p[2] == 'clear': movies[cur]['S'].append((k, 'clear', None, None))
            else:
                kv = dict(x.split('=', 1) for x in p[3:] if '=' in x and x.split('=')[0] in ('col', 'crc', 'x0', 'x1', 'h'))
                movies[cur]['S'].append((k, 'show', kv.get('col'), kv.get('crc') + ' x0=%s x1=%s h=%s' % (kv.get('x0'), kv.get('x1'), kv.get('h'))))
        elif ln.startswith('T '):
            movies[cur]['T'] = dict(x.split('=') for x in ln.split()[2:])
    return movies

orig = parse(open(sys.argv[1]))
port = {}
for fn in sys.argv[2:]:
    if not fn.startswith('--'): port.update(parse(open(fn)))
bad = 0
for name, o in orig.items():
    p = port.get(name)
    if p is None: print('%-9s no port output' % name); bad += 1; continue
    keys = sorted(set(o['F']) | set(p['F']))
    cnt = {'pix': 0, 'pal': 0, 'dac': 0, 'flags': 0, 'aud': 0}; diffs = []
    for k in keys:
        a, b = o['F'].get(k), p['F'].get(k)
        if a is None or b is None: diffs.append('record %d missing on the %s side' % (k, 'original' if a is None else 'port')); continue
        for f in cnt:
            if a.get(f) == b.get(f): cnt[f] += 1
            elif len(diffs) < 8: diffs.append('record %d %s: original %s port %s' % (k, f, a.get(f), b.get(f)))
    n = len(keys)
    # subtitles: frame, show/clear, text CRC, and the colour index when the port was given the seed
    # (GDV_SEED); without it the port prints the entry's raw colour byte and col is not compared
    withcol = '--col' in sys.argv
    so = [(k, t, c, col if withcol else None) for k, t, col, c in o['S']]; sp = [(k, t, c, col if withcol else None) for k, t, col, c in p['S']]
    smatch = sum(1 for x, y in zip(so, sp) if x == y) if len(so) == len(sp) else sum(1 for x, y in zip(so, sp) if x == y)
    ok = all(v == n for v in cnt.values()) and so == sp and o['T'] and p['T'] and \
         o['T'].get('audio_samples') == p['T'].get('audio_samples') and o['T'].get('audio_crc') == p['T'].get('audio_crc')
    print('%-9s records %d/%d  pixels %d  movie-palette %d  DAC %d  flags %d  audio blocks %d  | samples %s/%s crc %s/%s | subtitle events %d/%d match  %s' % (
        name, len(p['F']), len(o['F']), cnt['pix'], cnt['pal'], cnt['dac'], cnt['flags'], cnt['aud'],
        p['T'] and p['T'].get('audio_samples'), o['T'] and o['T'].get('audio_samples'),
        p['T'] and p['T'].get('audio_crc'), o['T'] and o['T'].get('audio_crc'), smatch, len(so), 'MATCH' if ok else 'DIFFER'))
    for d in diffs: print('    ' + d)
    if so != sp:
        for i, (x, y) in enumerate(zip(so + [None] * 9, sp + [None] * 9)):
            if x != y: print('    subtitle event %d: original %s port %s' % (i, x, y)); break
    if not ok: bad += 1
sys.exit(1 if bad else 0)
