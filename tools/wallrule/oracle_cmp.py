# orcmp.py <pieces export> <capture prefix> [ruleset 0=ROTH] : original's painted texels vs the rule, per pixel
import sys, os, numpy as np, math
os.environ.setdefault('ORACLE_DIR', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'oracle'))
sys.path.insert(0, os.environ['ORACLE_DIR'])
exec(open(os.environ['ORACLE_DIR']+'/common.py').read())
FX,FY,X0,Y0,EYE=309.77,355.06,320.48,240.54,143.94
def load_pieces(path):
    faces={}; cur=None; secs={}
    for ln in open(path):
        t=ln.split()
        if t[0]=='S': secs[int(t[1])]=(int(t[2]),int(t[3]))
        elif t[0]=='F':
            cur=dict(i=int(t[1]),a=(int(t[2]),int(t[3])),b=(int(t[4]),int(t[5])),sec=int(t[6]),nsec=int(t[7]),P={})
            faces[cur['i']]=cur
        elif t[0]=='P':
            k,tier=int(t[1]),int(t[2])
            cur['P'][(k,tier)]=dict(tex=int(t[3]),dimU=int(t[4]),dimV=int(t[5]),zTop=float(t[6]),zBot=float(t[7]),u0=float(t[8]),su=float(t[9]),
                mirror=int(t[10]),uMode=int(t[11]),zAnc=float(t[12]),v0=float(t[13]),sv=float(t[14]),vMode=int(t[15]))
    return faces,secs
def sample(p,d,z):
    c=math.floor(p['u0']+p['su']*d+1e-7)
    if p['mirror']: c=p['dimU']-1-c
    if p['uMode']==1: c%=p['dimU']
    elif p['uMode']==2: c&=p['dimU']-1
    elif p['uMode']==3: c=0
    r=math.floor(p['v0']+p['sv']*(p['zAnc']-z)+1e-7)
    if p['vMode']==1: r%=p['dimV']
    elif p['vMode']==2: r&=p['dimV']-1
    elif p['vMode']==3 and not (0<=r<p['dimV']): return None
    return c,r
def run(pieces,prefix,K=0,verbose=True,off=(0,0),stride=1,quiet=False):
    faces,secs=load_pieces(pieces)
    hdr,valid,tex,col,row,tag=load_all(prefix)
    camx,camy,ang=float(np.int16(hdr[3])),float(np.int16(hdr[4])),float(hdr[6]); z0=float(np.int16(hdr[5])); eye=EYE+z0
    th=np.pi/2+ang*2*np.pi/512; fw=np.array([np.cos(th),np.sin(th)]); rt=np.array([np.sin(th),-np.cos(th)])
    cam=np.array([camx+off[0],camy+off[1]]); H,W=valid.shape
    fl=[f for f in faces.values() if any(k==K for (k,t) in f['P'])]
    A=np.array([f['a'] for f in fl],float); B=np.array([f['b'] for f in fl],float); E=B-A
    stats={}  # (face,tier) -> [n, exact, within1, sum dc, sum dr]
    unassigned=0; total=0
    for px in range(0,W,stride):
        ys=np.nonzero(valid[:,px])[0][::stride]
        if len(ys)==0: continue
        d=fw+((px+0.5-X0)/FX)*rt
        den=d[0]*(-E[:,1])-d[1]*(-E[:,0])
        rhs=A-cam
        with np.errstate(divide='ignore',invalid='ignore'):
            t=(rhs[:,0]*(-E[:,1])-rhs[:,1]*(-E[:,0]))/den
            s=(d[0]*rhs[:,1]-d[1]*rhs[:,0])/den
        cr=E[:,0]*(cam[1]-A[:,1])-E[:,1]*(cam[0]-A[:,0])   # <0: camera on the right of a->b (the face's own side)
        ok=(np.abs(den)>1e-9)&(t>1)&(s>=0)&(s<=1)&(cr<0)
        idx=np.nonzero(ok)[0]; idx=idx[np.argsort(t[idx])]
        for py in ys:
            T=int(tex[py,px]); total+=1; done=False
            for i in idx:
                f=fl[i]; zw=eye-(py+0.5-Y0)*t[i]/FY
                for tier in (0,1,2):
                    p=f['P'].get((K,tier))
                    if not p or p['tex']!=T or not (p['zBot']-1<=zw<=p['zTop']+1): continue
                    L=math.hypot(*E[i]); sm=sample(p,s[i]*L,min(max(zw,p['zBot']+1e-3),p['zTop']-1e-3))
                    if sm is None: continue
                    c,r=sm
                    # the capture: 'row' = stored row = along the wall; 'col' = stored column = down it
                    dc=((int(row[py,px])-c+128)&255)-128; dr=((int(col[py,px])-r+128)&255)-128
                    st=stats.setdefault((f['i'],tier),[0,0,0,0,0,T])
                    st[0]+=1; st[1]+= (dc==0 and dr==0); st[2]+= (abs(dc)<=1 and abs(dr)<=1); st[3]+=dc; st[4]+=dr
                    done=True; break
                if done: break
            if not done: unassigned+=1
    n=sum(v[0] for v in stats.values()); ex=sum(v[1] for v in stats.values()); w1=sum(v[2] for v in stats.values())
    if quiet: return w1/max(n,1), n
    print(f'{prefix}: cam offset {off} painted px {total}, on a wall piece of the rule {n} ({unassigned} not: flats, sprites, objects), exact {ex} ({100*ex/max(n,1):.1f}%), within 1 texel {w1} ({100*w1/max(n,1):.1f}%)')
    if verbose:
        for (fi,tier),v in sorted(stats.items(), key=lambda kv:-kv[1][0]):
            if v[0]<40: continue
            flag='' if v[2]>=0.97*v[0] else '   <<<<'
            print(f'  face {fi:5d} {["upper","mid","lower"][tier]:5s} tex {v[5]:4d} px {v[0]:6d} within1 {100*v[2]/v[0]:5.1f}% mean dcol {v[3]/v[0]:+7.2f} drow {v[4]/v[0]:+7.2f}{flag}')
    return stats
def fit(pieces,prefix,K=0):
    best=(run(pieces,prefix,K,off=(0,0),stride=9,quiet=True)[0],(0,0))
    for step in (8,2,1):
        b0=best[1]
        for dx in (-2,-1,0,1,2):
            for dy in (-2,-1,0,1,2):
                o=(b0[0]+dx*step,b0[1]+dy*step)
                sc=run(pieces,prefix,K,off=o,stride=9,quiet=True)[0]
                if sc>best[0]+1e-9: best=(sc,o)
    return best[1]
if __name__=='__main__':
    K=int(sys.argv[3]) if len(sys.argv)>3 else 0
    o=fit(sys.argv[1],sys.argv[2],0) if len(sys.argv)>4 else (0,0)
    run(sys.argv[1],sys.argv[2],K,off=o)
