import os
import sys; sys.path.insert(0,os.environ.get('ORACLE_DIR','.'))
import numpy as np, flatfit as F
exec(open(os.environ.get('ORACLE_DIR','.')+'/common.py').read())
from rawmap import RawMap
prefix,rawp,texl=sys.argv[1:4]; targets=[int(x) for x in sys.argv[4].split(',')]
hdr,valid,tex,col,row,tag=load_all(prefix)
camx,camy,ang=float(np.int16(hdr[3])),float(np.int16(hdr[4])),float(hdr[6]); z=float(np.int16(hdr[5]))
m=RawMap(rawp); info={}
for line in open(texl):
    f,mo,it,w,h=map(int,line.split()[:5]); info[f]=(w,h,mo,it)
for idx in targets:
    so=30+idx*26; S=m.sectors[so]
    for ceil in (False,True):
        t=S['ceilT'] if ceil else S['floorT']; fv=0x38 if ceil else 0xb8
        sel=valid&((tag>>28)==1)&((tag&0xffff)==so)&(((tag>>16)&0xf8)==fv)&(tex==t)
        if sel.sum()<50: continue
        W,Hh=info[t][:2]; py,px=np.nonzero(sel)
        best=[]
        H0=S['ceilH'] if ceil else S['floorH']
        for H in range(H0-64,H0+65,2):
            wx,wy=F.world(px,py,camx,camy,ang,F.EYE+z,H,ceil)
            _,rc,_=F.circ_fit(col[sel].astype(float),-wx/2,W); _,rr,_=F.circ_fit(row[sel].astype(float),wy/2,Hh)
            best.append((rc+rr,H))
        best.sort(); print(idx,'C' if ceil else 'F','raw H',H0,'best H',best[:3], 'n',sel.sum())
