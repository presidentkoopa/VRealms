#!/usr/bin/env python3
"""compare.py <oracle.trace> <port.trace> [--show N]
Diff the original's trace (gameprobe.c) against the port's (replay.cpp), test by test.
A test is everything from one 'A ...' / 'I ...' / 'R ...' / 'H ...' header line to the next.
Prints, per section, how many tests match exactly and, for each mismatch, the first
divergent line of both sides. Exit status 0 only when every test matches."""
import sys, collections

def tests(path):
    out, order, cur = {}, [], None
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        if line[:2] in ('A ', 'B ', 'Q ', 'I ', 'R ', 'H ') or line in ('BOOT', 'END') or line.startswith('ENV '):
            cur = line if not line.startswith('ENV ') else 'ENV'
            if cur == 'ENV':
                out['ENV'] = [line]; order.append('ENV'); cur = 'S0'; out['S0'] = []; order.append('S0'); continue
            if cur in out: cur += ' #dup'
            out[cur] = []; order.append(cur)
        elif cur is not None:
            out[cur].append(line)
    return out, order

def main():
    a, order = tests(sys.argv[1]); b, _ = tests(sys.argv[2])
    show = int(sys.argv[sys.argv.index('--show') + 1]) if '--show' in sys.argv else 40
    stats = collections.OrderedDict(); bad = []
    for name in order:
        if name in ('BOOT', 'END'): continue
        sec = name.split(' ')[0]
        st = stats.setdefault(sec, [0, 0]); st[1] += 1
        la, lb = a[name], b.get(name)
        if lb is not None and la == lb: st[0] += 1; continue
        first = None
        if lb is None: first = ('<test missing in the port trace>', '', 0)
        else:
            for i in range(max(len(la), len(lb))):
                x = la[i] if i < len(la) else '<end>'; y = lb[i] if i < len(lb) else '<end>'
                if x != y: first = (x, y, i); break
        bad.append((name, first))
    extra = [n for n in b if n not in a]
    for sec, (ok, n) in stats.items(): print('%-3s %d / %d match exactly' % (sec, ok, n))
    if extra: print('port trace has %d tests the oracle trace lacks' % len(extra))
    # distinct A ids
    aid = collections.OrderedDict()
    for name in order:
        if name.startswith('A '): aid.setdefault(name.split()[1], True)
    for name, _ in bad:
        if name.startswith('A '): aid[name.split()[1]] = False
    if aid: print('actions: %d of %d ids match in every variant' % (sum(1 for v in aid.values() if v), len(aid)))
    for name, (x, y, i) in bad[:show]:
        print('MISMATCH %s  (line %d of the test)\n   original: %s\n   port    : %s' % (name, i + 1, x[:300], y[:300]))
    if len(bad) > show: print('... and %d more mismatches' % (len(bad) - show))
    return 1 if bad or extra else 0

sys.exit(main())
