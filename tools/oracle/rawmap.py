import os
import struct
class RawMap:
    def __init__(s, path):
        d=s.d=open(path,'rb').read()
        u16=lambda o:struct.unpack_from('<H',d,o)[0]
        s.vtxoff=u16(0); s.secoff=u16(4); s.nsec=u16(28)
        s.sectors={}
        for i in range(s.nsec):
            o=30+i*26
            s.sectors[o]=dict(idx=i,off=o,ceilH=struct.unpack_from('<h',d,o)[0],floorH=struct.unpack_from('<h',d,o+2)[0],
                unk4=u16(o+4),ceilT=u16(o+6),floorT=u16(o+8),fit=d[o+10],light=d[o+11],tmo=d[o+12],nfaces=d[o+13],firstFace=u16(o+14),
                cshx=d[o+16],cshy=d[o+17],fshx=d[o+18],fshy=d[o+19],trig=u16(o+20),u16_=u16(o+22),mid=u16(o+24),b17=d[o+0x17])
        p=30+s.nsec*26; s.nface=u16(p); p+=2; s.faces={}
        for i in range(s.nface):
            s.faces[p]=dict(off=p,v1=u16(p),v2=u16(p+2),tm=u16(p+4),sec=u16(p+6),sister=u16(p+8),col=u16(p+10)); p+=12
        s.nmap=u16(p); p+=2; s.maps={}
        for i in range(s.nmap):
            typ=d[p+1]; m=dict(off=p,unk0=d[p],type=typ,mid=u16(p+2),up=u16(p+4),lo=u16(p+6),flags=u16(p+8))
            if typ>=0x80: m.update(shx=d[p+10],shy=d[p+11],u0c=u16(p+12)); p+=14
            else: m.update(shx=0,shy=0); p+=10
            s.maps[p- (14 if typ>=0x80 else 10)]=m
        s.midstart=p
        vh=s.vtxoff; s.nvtx=u16(vh+6)
    def vertex(s,off):
        # vertex offsets are relative to vertices section header
        o=s.vtxoff+off
        return struct.unpack_from('<hh',s.d,o+8)

def _pip(pt, poly):
    x,y=pt; inside=False
    for i in range(len(poly)):
        x1,y1=poly[i]; x2,y2=poly[(i+1)%len(poly)]
        if (y1>y)!=(y2>y):
            xi=x1+(y-y1)*(x2-x1)/(y2-y1)
            if x<xi: inside=not inside
    return inside
def sector_edges(m, so):
    return [(m.vertex(f['v1']), m.vertex(f['v2'])) for f in m.faces.values() if f['sec']==so]
def sector_poly(m, so):
    return [e[0] for e in sector_edges(m,so)]
def sectors_at(m, pt):
    out=[]
    for so in m.sectors:
        e=sector_edges(m,so)
        if not e: continue
        # even-odd over all edges (handles holes / multiple loops)
        x,y=pt; inside=False
        for (x1,y1),(x2,y2) in e:
            if (y1>y)!=(y2>y):
                xi=x1+(y-y1)*(x2-x1)/(y2-y1)
                if x<xi: inside=not inside
        if inside: out.append(so)
    return out
