import os
ALIAS={}
import sys, struct, numpy as np
sys.path.insert(0,os.environ.get('ORACLE_DIR','.'))
import flatfit as F
exec(open(os.environ.get('ORACLE_DIR','.')+'/common.py').read())
from rawmap import RawMap
def load_faces(m):
    out=[]
    for fo,f in m.faces.items():
        a=np.array(m.vertex(f['v1']),float); b=np.array(m.vertex(f['v2']),float)
        tm=m.maps.get(f['tm']); S=m.sectors[f['sec']]
        sis=m.faces.get(f['sister']); N=m.sectors[sis['sec']] if sis else None
        fw=struct.unpack_from('<H',m.d,tm['off'])[0]
        ext=(fw&0xfff) if fw&0x8000 else fw
        out.append(dict(off=fo,a=a,b=b,len=float(np.hypot(*(b-a))),tm=tm,ext=ext,S=S,N=N,flags=tm['flags']&0xff))
    return out
def analyse(prefix, rawp, texl, verbose=True):
    hdr,valid,tex,col,row,tag=load_all(prefix)
    for a_,b_ in ALIAS.items(): tex=np.where(tex==a_,b_,tex)
    camx,camy,ang=float(np.int16(hdr[3])),float(np.int16(hdr[4])),float(hdr[6]); z=float(np.int16(hdr[5])); eye=F.EYE+z
    m=RawMap(rawp); faces=load_faces(m)
    info={}
    for line in open(texl):
        f,mo,it,w,h=map(int,line.split()[:5]); info[f]=(w,h,mo,it)
    th=np.pi/2+ang*2*np.pi/512; fw=np.array([np.cos(th),np.sin(th)]); rt=np.array([np.sin(th),-np.cos(th)])
    cam=np.array([camx,camy])
    # per-pixel assignment
    H,W=valid.shape
    assign={}  # face idx -> list of (px,py,dist_along,zworld,piece)
    walltex=set()
    for fi,f in enumerate(faces):
        for k in ('mid','up','lo'):
            walltex.add(f['tm'][k])
    cand=valid&np.isin(tex,list(walltex))
    for px in range(W):
        ys=np.nonzero(cand[:,px])[0]
        if len(ys)==0: continue
        d=fw+((px+0.5-F.X0)/F.FX)*rt
        hits=[]
        for fi,f in enumerate(faces):
            a,b=f['a'],f['b']; e=b-a
            den=d[0]*(-e[1])-d[1]*(-e[0])
            if abs(den)<1e-9: continue
            rhs=a-cam
            t=(rhs[0]*(-e[1])-rhs[1]*(-e[0]))/den
            s=(d[0]*rhs[1]-d[1]*rhs[0])/den
            if t<=1 or s<0 or s>1: continue
            # front-facing: camera on the sector side; ROTH faces: test sign
            cr=e[0]*(cam[1]-a[1])-e[1]*(cam[0]-a[0])
            hits.append((t,s,fi,cr))
        hits.sort()
        for py in ys:
            T=tex[py,px]
            for t,s,fi,cr in hits:
                f=faces[fi]; zw=eye-(py+0.5-F.Y0)*t/F.FY
                S,N=f['S'],f['N']; tm=f['tm']
                piece=None
                if N is None:
                    if tm['mid']==T and S['floorH']<=zw<=S['ceilH']: piece='mid'
                else:
                    if tm['up']==T and N['ceilH']<=zw<=S['ceilH']: piece='up'
                    elif tm['lo']==T and S['floorH']<=zw<=N['floorH']: piece='lo'
                    elif tm['mid']==T and max(S['floorH'],N['floorH'])<=zw<=min(S['ceilH'],N['ceilH']): piece='mid'
                if piece:
                    assign.setdefault((fi,piece),[]).append((px,py,s*f['len'],zw,cr))
                    break
                # occlusion: if this face is solid at this height stop
                if N is None and S['floorH']<=zw<=S['ceilH'] and cr>0: break
    res=[]
    for (fi,piece),L in assign.items():
        if len(L)<40: continue
        f=faces[fi]; tm=f['tm']; T=tm[piece]; Wt,Ht=info[T][:2]
        A=np.array(L); px=A[:,0].astype(int); py=A[:,1].astype(int)
        c=col[py,px].astype(float); r=row[py,px].astype(float)
        dist=A[:,2]; zw=A[:,3]
        best={}
        for nm,x,obs,n in (('row~dist',dist,r,Ht),('col~z',zw,c,Wt)):
            bb=None
            for sg in (1,-1):
                for sc in (2.0,1.0,0.5,0.25,0.125):
                    o,rms,ok=F.circ_fit(obs,sg*sc*x,n)
                    if bb is None or rms<bb[3]: bb=(sg,sc,o,rms,ok)
            best[nm]=bb
        res.append(dict(face=f['off'],sec=f['S']['idx'],piece=piece,tex=T,W=Wt,H=Ht,n=len(L),len=f['len'],ext=f['ext'],flags=f['flags'],shx=tm['shx'],shy=tm['shy'],best=best,front=np.median(A[:,4])>0))
        if verbose:
            b1,b2=best['row~dist'],best['col~z']
            print(f"face@{f['off']} sec{f['S']['idx']} {piece} tex{T} {Wt}x{Ht} n={len(L)} len={f['len']:.0f} ext={f['ext']} fl={f['flags']:#04x} sh=({tm['shx']},{tm['shy']}) | row={b1[0]*b1[1]:+.3f}*d {b1[2]:6.2f} rms {b1[3]:.2f} | col={b2[0]*b2[1]:+.3f}*z {b2[2]:6.2f} rms {b2[3]:.2f}")
    return res
if __name__=='__main__':
    analyse(sys.argv[1],sys.argv[2],sys.argv[3])
