import os
# End-to-end: ROTH.C oracle pixels vs a model of GZDoom's flat path fed by REMAROTH's FlatToEngine.
import sys, subprocess, numpy as np
sys.path.insert(0,os.environ.get('ORACLE_DIR','.'))
import flatfit as F
exec(open(os.environ.get('ORACLE_DIR','.')+'/common.py').read())
from rawmap import RawMap
def gz_texel(e, W, H, wx, wy):
    xs,ys,xo,yo,ang=e
    th=np.deg2rad(-ang); c,s=np.cos(th),np.sin(th)
    u0,v0=wx,-wy
    ru=u0*c-v0*s; rv=u0*s+v0*c
    u=xs*(xo+ru); v=ys*(yo+rv)          # texels along display width / height
    # quarter-turned texture: display width = stored H (u indexes stored ROW), display height = stored W
    row=np.floor(u+1e-6)%H; col=np.floor(v+1e-6)%W
    return col,row
def run(prefix, rawp, texl, fte):
    hdr,valid,tex,col,row,tag=load_all(prefix)
    camx,camy,ang=float(np.int16(hdr[3])),float(np.int16(hdr[4])),float(hdr[6]); z=float(np.int16(hdr[5]))
    m=RawMap(rawp); info={}
    for line in open(texl):
        f,mo,it,w,h=map(int,line.split()[:5]); info[f]=(w,h,mo,it)
    kind=tag>>28; sec=tag&0xffff; fill=(tag>>16)&0xf8
    tot=hit=0; per=[]
    for so in np.unique(sec[kind==1]):
        S=m.sectors.get(int(so))
        if S is None: continue
        for ceil in (False,True):
            t=S['ceilT'] if ceil else S['floorT']
            sel=valid&(kind==1)&(sec==so)&(fill==(0x38 if ceil else 0xb8))&(tex==t)
            if sel.sum()<20 or t not in info: continue
            W,Hh,mo,it=info[t]
            s=((S['fit']&0x0c)>>2) if ceil else ((S['fit']&0x30)>>4)
            mir=((S['b17']>>2)&3) if ceil else (S['b17']&3)
            shx,shy=(S['cshx'],S['cshy']) if ceil else (S['fshx'],S['fshy'])
            o256=int(W==256 and Hh==256 and not (it&4))
            e=list(map(float,subprocess.run([fte],input=f"{W} {Hh} {s} {shx} {shy} {mir&1} {(mir>>1)&1} {o256}\n",capture_output=True,text=True).stdout.split()))
            py,px=np.nonzero(sel)
            H=S['ceilH'] if ceil else S['floorH']
            wx,wy=F.world(px,py,camx,camy,ang,F.EYE+z,H,ceil)
            gc,gr=gz_texel(e,W,Hh,wx,wy)
            dc=np.abs(((col[sel]-gc)+W/2)%W-W/2); dr=np.abs(((row[sel]-gr)+Hh/2)%Hh-Hh/2)
            ok=(dc<=1)&(dr<=1)
            tot+=sel.sum(); hit+=ok.sum(); per.append((S['idx'],'C' if ceil else 'F',int(sel.sum()),ok.mean()))
    return hit/tot, per, tot
if __name__=='__main__':
    for prefix,rawp,texl in [('cap','/home/claude/work/game/M/STUDY1.RAW','tex_0.txt'),('L211b','/home/claude/work/game/M/LRINTH1.RAW','L211b_tex.txt'),('L210','/home/claude/work/game/M/LRINTH1.RAW','L210_tex.txt')]:
        for name in ('old','new'):
            r,per,tot=run(os.environ.get('ORACLE_DIR','.')+'/'+prefix,rawp,os.environ.get('ORACLE_DIR','.')+'/'+texl,os.environ.get('ORACLE_DIR','.')+'/e2e/fte_'+name)
            print(f'{prefix:6s} {name}: {r*100:6.2f}% of {tot} flat pixels within 1 texel of ROTH.C')
