#!/usr/bin/env python3
"""stackscan.py minidump.mdmp doomxr.exe -- the crashing thread's stack, scanned for return addresses.

Reads the minidump's exception stream (crashing thread id), its thread list (RSP) and memory list (the stack
bytes), and prints every qword on the stack that points into doomxr.exe just after a CALL instruction, as an
image-relative VA for llvm-symbolizer (--obj=doomxr.exe). A heuristic scan, not an unwind: stale frames can appear,
but every real caller is in the list, in order."""
import struct, sys
md = open(sys.argv[1], 'rb').read(); exe = open(sys.argv[2], 'rb').read()
sig, ver, nstreams, dirrva = struct.unpack_from('<IIII', md, 0)
streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from('<III', md, dirrva + 12 * i); streams.setdefault(t, []).append((size, rva))
# ModuleList (4): doomxr base and size
size, rva = streams[4][0]; n = struct.unpack_from('<I', md, rva)[0]; base = None
for i in range(n):
    off = rva + 4 + 108 * i
    b, sz = struct.unpack_from('<QI', md, off); namerva = struct.unpack_from('<I', md, off + 20)[0]
    ln = struct.unpack_from('<I', md, namerva)[0]; name = md[namerva + 4: namerva + 4 + ln].decode('utf-16le')
    if name.lower().endswith('doomxr.exe'): base, msize = b, sz
# Exception (6): thread id, context location
size, rva = streams[6][0]; tid = struct.unpack_from('<I', md, rva)[0]
ctx_size, ctx_rva = struct.unpack_from('<II', md, rva + 8 + 152)
rsp = struct.unpack_from('<Q', md, ctx_rva + 0x98)[0]; rip = struct.unpack_from('<Q', md, ctx_rva + 0xF8)[0]
# ThreadList (3): the stack memory of that thread
size, rva = streams[3][0]; n = struct.unpack_from('<I', md, rva)[0]; stack = None
for i in range(n):
    off = rva + 4 + 48 * i
    t = struct.unpack_from('<I', md, off)[0]
    if t == tid:
        start, dsz, drva = struct.unpack_from('<QII', md, off + 24); stack = (start, md[drva:drva + dsz])
# PE: preferred image base and sections (to read code bytes before a candidate return address)
pe = struct.unpack_from('<I', exe, 0x3c)[0]; nsec = struct.unpack_from('<H', exe, pe + 6)[0]
optsz = struct.unpack_from('<H', exe, pe + 20)[0]; imagebase = struct.unpack_from('<Q', exe, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + 40 * i
    vsz, va, rsz, rptr = struct.unpack_from('<IIII', exe, o + 8); ch = struct.unpack_from('<I', exe, o + 36)[0]
    secs.append((va, vsz, rptr, rsz, ch))
def code(rva_, k):
    for va, vsz, rptr, rsz, ch in secs:
        if ch & 0x20 and va <= rva_ - k and rva_ <= va + rsz: return exe[rptr + rva_ - va - k: rptr + rva_ - va]
    return None
def after_call(r):
    b = code(r, 7)
    if not b: return False
    return b[-5] == 0xE8 or (b[-2] == 0xFF and (b[-1] >> 3) & 7 == 2) or (b[-3] == 0xFF and (b[-2] >> 3) & 7 == 2) \
        or (b[-6] == 0xFF and (b[-5] >> 3) & 7 == 2) or (b[-7] == 0xFF and (b[-6] >> 3) & 7 == 2)
print('0x%x' % (rip - base + imagebase))
start, data = stack
for o in range(max(0, rsp - start), len(data) - 7, 8):
    q = struct.unpack_from('<Q', data, o)[0]
    if base <= q < base + msize and after_call(q - base): print('0x%x' % (q - base + imagebase))
