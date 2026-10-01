import os
import numpy as np, sys
def load(p):
    d=open(p,'rb').read(); hdr=np.frombuffer(d[:32],np.uint32); w,h=hdr[0],hdr[1]
    return hdr, np.frombuffer(d[32:32+w*h],np.uint8).reshape(h,w)
def decode(prefix):
    caps=[load(f'{prefix}_{k}.bin') for k in range(5)]
    hdr=caps[0][0]; planes=[c[1].astype(np.int64) for c in caps]
    valid=np.ones(planes[0].shape,bool); word=np.zeros(planes[0].shape,np.int64)
    for k,pl in enumerate(planes):
        valid &= (pl>=1)&(pl<=64)
        word |= ((pl-1)&63) << (6*k)
    col=word&0xff; row=(word>>8)&0xff; tex=(word>>16)&0x1fff
    return hdr, valid, tex, col, row
if __name__=='__main__':
    hdr,valid,tex,col,row=decode(sys.argv[1])
    texinfo={}
    for line in open(sys.argv[2]):
        f,m,it,w,h=map(int,line.split()); texinfo[f]=(w,h,m,it)
    ok=valid.copy()
    bad=0
    ts,cs=np.unique(tex[valid],return_counts=True)
    for t,c in zip(ts,cs):
        if t not in texinfo: print('unknown tex',t,c); continue
        w,h,m,it=texinfo[t]; sel=valid&(tex==t)
        b=((col[sel]>=w)|(row[sel]>=h)).sum(); bad+=b
        print(f'tex {t:5d} {w}x{h} mod={m:#x} it={it:#x} px={c} out_of_range={b}')
    print('valid px',valid.sum(),'of',valid.size,'bad',bad)
