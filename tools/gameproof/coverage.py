#!/usr/bin/env python3
"""coverage.py <trace> -- what a trace actually exercised: event and state counts per section.
A differential test proves only what it reached; read this next to compare.py's numbers."""
import sys, collections
EVENTS = ('give', 'remove', 'query', 'find', 'filter', 'rng', 'evalblock', 'weapon', 'weaponlist', 'weaponhud', 'pickupfly',
          'icon', 'tabclear', 'line', 'voice', 'window', 'windowalt', 'pick', 'pump', 'forceend', 'gdv', 'image', 'sfx',
          'db300', 'seqfinal', 'seqevent', 'seqprocess', 'exitoverlay', 'resetchain', 'runobj', 'health',
          'DRIVE-LIMIT', 'RUNAWAY', 'UNMODELLED')
c = collections.Counter(); sec = '-'
for line in open(sys.argv[1], errors='replace'):
    if line[:2] in ('A ', 'B ', 'Q ', 'I ', 'R '): sec = line[0]; c[(sec, '(tests)')] += 1; continue
    w = line.strip().split(' ')[0]
    if w in EVENTS and not line.startswith('S'): c[(sec, w)] += 1
    if line.startswith('S dlg') and 'qcount=0' not in line: c[(sec, 'state:queue-nonempty')] += 1
    if line.startswith('S flags '): c[(sec, 'state:flags-changed')] += 1
    if line.startswith('S ext'):
        if 'death=1' in line: c[(sec, 'state:death')] += 1
        if not line.rstrip().endswith('img'): c[(sec, 'state:image-changed')] += 1
    if line.startswith('S inv') and 'auto=1' in line: c[(sec, 'state:autoselected')] += 1
for s in 'ABQIR':
    row = ['%s=%d' % (k[1], v) for k, v in sorted(c.items()) if k[0] == s]
    if row: print(s, ' '.join(row))
