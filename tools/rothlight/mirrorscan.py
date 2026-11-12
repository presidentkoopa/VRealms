#!/usr/bin/env python3
"""mirrorscan.py <retail DATA dir>

Lists every Realms MIRROR face in the 44 retail maps.

THE RULE (ROTH.C): a face is a mirror when the picture on it carries image_type bit 3
(roth_das.h IT_MIRROR): the wall rasterizer tests `ah & 8` on the art block's word at +0xa
(renderer.c:13229-13246, rasterize_world_spans_scanline 0x368b9) and, instead of drawing the
picture, registers the face for a reflected view (add_reflection_view_entry, renderer.c:12642).
`ah & 4` (IT_TRANSLUCENT) then decides whether the picture is ALSO drawn over the reflection
(renderer.c:13245: jne srcptr) or not drawn at all (:13246 return).

The reflected view (render_reflection_subviews, render_world.c:203) reflects the camera across
the face along ONE AXIS -- whichever of |dx|,|dy| the face spans more
(reflect_view_across_mirror_plane, renderer.c:12669) -- so the tool also reports whether each
mirror face is axis-aligned, which is what makes a general plane mirror the same picture.

Prints one line per mirror face and a summary; reads only."""
import os, re, struct, sys, glob

def u16(d, o): return struct.unpack_from('<H', d, o)[0]
def s16(d, o): return struct.unpack_from('<h', d, o)[0]
def u32(d, o): return struct.unpack_from('<I', d, o)[0]

class Das:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        assert d[:4] == b'DASP', path
        fat = u32(d, 8)
        n = sum(u16(d, 0x30 + 2 * i) for i in range(4))
        self.fat = [(u32(d, fat + 8 * i), u16(d, fat + 8 * i + 4), d[fat + 8 * i + 6], d[fat + 8 * i + 7]) for i in range(n)]
    def image_type(self, idx):
        if idx < 0 or idx >= len(self.fat): return None
        off, size, f1, f2 = self.fat[idx]
        if off == 0 or size < 6 or f1 & (4 | 0x20): return None   # indirections are not pictures
        return self.d[off + 1]

def map_pack_names(data_dir):
    s = open(os.path.join(data_dir, 'ROTH.RES'), 'rb').read().decode('latin-1')
    out = {}
    for m in re.finditer(r'(?im)^m\\(\w+)[ \t]+m\\(\w+)', s):
        out[m.group(1).upper()] = m.group(2).upper()
    return out

def faces(raw):
    d = raw
    secoff = u16(d, 4); nsec = u16(d, 28)
    p = 30 + nsec * 26
    nface = u16(d, p); p += 2
    fl = []
    for i in range(nface):
        fl.append(dict(off=p, v1=u16(d, p), v2=u16(d, p + 2), tm=u16(d, p + 4), sec=u16(d, p + 6), sister=u16(d, p + 8)))
        p += 12
    vtx = u16(d, 0)
    def vert(o): return struct.unpack_from('<hh', d, vtx + o + 8)
    return fl, vert

def tmap(d, off):
    typ = d[off + 1]
    return dict(type=typ, mid=u16(d, off + 2), up=u16(d, off + 4), lo=u16(d, off + 6), flags=u16(d, off + 8))

def main():
    data = sys.argv[1] if len(sys.argv) > 1 else "/mnt/user-data/uploads/Realms of the Haunting/DATA"
    packs = map_pack_names(data)
    total = 0; axis = 0; drawn = 0; onesided = 0
    cache = {}
    for raw in sorted(glob.glob(os.path.join(data, 'M', '*.RAW'))):
        name = os.path.basename(raw)[:-4].upper()
        pk = packs.get(name)
        if not pk: continue
        pp = os.path.join(data, 'M', pk + '.DAS')
        if not os.path.exists(pp):
            cand = [f for f in os.listdir(os.path.join(data, 'M')) if f.upper() == pk + '.DAS']
            if not cand: continue
            pp = os.path.join(data, 'M', cand[0])
        das = cache.get(pp) or cache.setdefault(pp, Das(pp))
        d = open(raw, 'rb').read()
        fl, vert = faces(d)
        for f in fl:
            if f['tm'] == 0xffff or f['tm'] + 10 > len(d): continue
            t = tmap(d, f['tm'])
            for part in ('mid', 'up', 'lo'):
                tid = t[part]
                if tid == 0xffff or tid >= 0x1200: continue
                it = das.image_type(tid)
                if it is None or not (it & 8): continue
                a, b = vert(f['v1']), vert(f['v2'])
                al = (a[0] == b[0]) or (a[1] == b[1])
                total += 1; axis += al; drawn += bool(it & 4); onesided += (f['sister'] == 0xffff)
                print("%-9s face@%-6d %-3s tex %-5d image_type %02x  %s  (%d,%d)-(%d,%d)  %s  %s" % (
                    name, f['off'], part, tid, it, 'glass drawn over' if it & 4 else 'no glass',
                    a[0], a[1], b[0], b[1], 'axis-aligned' if al else 'DIAGONAL',
                    'one-sided' if f['sister'] == 0xffff else 'two-sided'))
    print("mirror faces: %d  axis-aligned %d  glass drawn over %d  one-sided %d" % (total, axis, drawn, onesided))

if __name__ == '__main__':
    main()
