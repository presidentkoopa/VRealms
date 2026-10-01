import os
import numpy as np, sys, itertools
sys.path.insert(0,os.environ.get('ORACLE_DIR','.'))
exec(open(os.environ.get('ORACLE_DIR','.')+'/common.py').read())
from rawmap import RawMap
FX,FY,X0,Y0,EYE=309.77,355.06,320.48,240.54,143.94
def world(px,py,camx,camy,ang,eyez,H,ceil):
    th=np.pi/2+ang*2*np.pi/512
    if ceil: D=(H-eyez)*FY/(Y0-(py+0.5))
    else:    D=(eyez-H)*FY/((py+0.5)-Y0)
    L=(px+0.5-X0)*D/FX
    fx,fy=np.cos(th),np.sin(th); rx,ry=np.sin(th),-np.cos(th)
    return camx+D*fx+L*rx, camy+D*fy+L*ry
def circ_fit(vals, pred, W):
    d=(vals-pred)%W; ang=d*2*np.pi/W
    m=np.angle(np.mean(np.exp(1j*ang)))
    c=(m*W/(2*np.pi))%W
    res=((d-c+W/2)%W)-W/2
    return c, np.sqrt(np.mean(res**2)), np.mean(np.abs(res)<0.75)
def analyse(prefix, rawpath, texlog, eyez_extra=0.0, verbose=True):
    hdr,valid,tex,col,row,tag=load_all(prefix)
    camx,camy,ang=float(np.int16(hdr[3])),float(np.int16(hdr[4])),float(hdr[6])
    m=RawMap(rawpath)
    info={}
    for line in open(texlog):
        f,mo,it,w,h=map(int,line.split()[:5]); info[f]=(w,h,mo,it)
    kind=tag>>28; sec=tag&0xffff; fill=(tag>>16)&0xff
    out=[]
    for so in np.unique(sec[kind==1]):
        S=m.sectors.get(int(so))
        if S is None: continue
        for fv in (0x38,0xb8):
            ceil = fv==0x38
            t = S['ceilT'] if ceil else S['floorT']
            sel=valid&(kind==1)&(sec==so)&((fill&0xf8)==fv)&(tex==t)
            n=sel.sum()
            if n<50 or t not in info: continue
            W,Hh=info[t][0],info[t][1]
            py,px=np.nonzero(sel)
            H = S['ceilH'] if ceil else S['floorH']
            wx,wy=world(px,py,camx,camy,ang,EYE+float(np.int16(hdr[5]))+eyez_extra,H,ceil)
            c=col[sel].astype(float); r=row[sel].astype(float)
            best=None
            mags=[2.0,1.0,0.5,0.25,0.125]
            for tr in (0,1):
                for sc,sr in itertools.product((1,-1),(1,-1)):
                    for mc in mags:
                        a = (wx if not tr else wy)*sc*mc
                        cc,rmsc,okc=circ_fit(c,a,W)
                        if best is None or rmsc<best['c'][1]: pass
                        for mr in mags:
                            b = (wy if not tr else wx)*sr*mr
                            cr,rmsr,okr=circ_fit(r,b,Hh)
                            score=rmsc+rmsr
                            if best is None or score<best['score']:
                                best=dict(score=score,tr=tr,sc=sc,sr=sr,mc=mc,mr=mr,c=(cc,rmsc,okc),r=(cr,rmsr,okr))
            fit=S['fit']; s = ((fit&0x0c)>>2) if ceil else ((fit&0x30)>>4)
            mir = (S['b17']>>2)&3 if ceil else S['b17']&3
            shx,shy=(S['cshx'],S['cshy']) if ceil else (S['fshx'],S['fshy'])
            rec=dict(sec=int(so),idx=S['idx'],ceil=ceil,tex=t,W=W,H=Hh,trans=bool(info[t][3]&4),n=int(n),s=s,mir=mir,shx=shx,shy=shy,**best)
            out.append(rec)
            if verbose:
                axis_c='x' if not best['tr'] else 'y'; axis_r='y' if not best['tr'] else 'x'
                print(f"sec{S['idx']:4d} {'C' if ceil else 'F'} tex{t:4d} {W}x{Hh} {'T' if rec['trans'] else 'O'} n={n:6d} s={s} mir={mir} sh=({shx},{shy}) | col={'+' if best['sc']>0 else '-'}{axis_c}*{best['mc']} +{best['c'][0]:7.2f} rms={best['c'][1]:.2f} ok={best['c'][2]:.2f} | row={'+' if best['sr']>0 else '-'}{axis_r}*{best['mr']} +{best['r'][0]:7.2f} rms={best['r'][1]:.2f} ok={best['r'][2]:.2f}")
    return out
if __name__=='__main__':
    analyse(sys.argv[1],sys.argv[2],sys.argv[3])
