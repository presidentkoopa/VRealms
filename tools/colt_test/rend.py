#!/usr/bin/env python3
# rend.py OUT.png W H  view=dx,dy,dz up=ux,uy,uz [mesh=FILE:tex|rgb ...] [pts=FILE] [scale=s] [center=x,y,z]
# orthographic; mesh files: "nv nt" then rows px py pz nx ny nz u v [bone], then tris
import sys, numpy as np
from PIL import Image, ImageDraw
args=dict(); meshes=[]; ptsf=[]
out=sys.argv[1]; W=int(sys.argv[2]); H=int(sys.argv[3])
for a in sys.argv[4:]:
    k,v=a.split('=',1)
    if k=='mesh': meshes.append(v)
    elif k=='pts': ptsf.append(v)
    else: args[k]=v
vd=np.array(list(map(float,args['view'].split(','))));vd/=np.linalg.norm(vd)
up=np.array(list(map(float,args['up'].split(','))));up-=vd*up.dot(vd);up/=np.linalg.norm(up)
rt=np.cross(vd,up)
def load(p):
    with open(p) as f:
        nv,nt=map(int,f.readline().split())
        V=np.loadtxt(f,max_rows=nv)
        T=np.loadtxt(f,max_rows=nt,dtype=int).reshape(-1,3)
    return V,T
data=[]
allp=[]
for m in meshes:
    fn,col=m.split(':',1)
    V,T=load(fn)
    if col.endswith('.jpg') or col.endswith('.png'):
        im=Image.open(col).convert('RGB'); im=im.resize((1024,1024)); tex=np.asarray(im).astype(np.float32)/255
        c=None
    else:
        tex=None; c=np.array(list(map(float,col.split(','))))
    data.append((V,T,tex,c)); allp.append(V[:,:3])
P=np.concatenate(allp)
sx=P@rt; sy=P@up
if 'center' in args:
    cc=np.array(list(map(float,args['center'].split(',')))); cx,cy=cc@rt,cc@up
else:
    cx,cy=(sx.min()+sx.max())/2,(sy.min()+sy.max())/2
span=float(args['span']) if 'span' in args else max(sx.max()-sx.min(),sy.max()-sy.min())*1.08
s=min(W,H)/span
img=np.zeros((H,W,3),np.float32)+0.12; zb=np.full((H,W),np.inf)
L=-vd+0.4*up+0.3*rt; L/=np.linalg.norm(L)
for V,T,tex,c in data:
    p=V[:,:3]; n=V[:,3:6]
    X=(p@rt-cx)*s+W/2; Y=H/2-(p@up-cy)*s; Z=p@vd
    for a,b,cc in T:
        xs=np.array([X[a],X[b],X[cc]]); ys=np.array([Y[a],Y[b],Y[cc]])
        x0=max(int(np.floor(xs.min())),0); x1=min(int(np.ceil(xs.max())),W-1)
        y0=max(int(np.floor(ys.min())),0); y1=min(int(np.ceil(ys.max())),H-1)
        if x0>x1 or y0>y1: continue
        d=(xs[1]-xs[0])*(ys[2]-ys[0])-(xs[2]-xs[0])*(ys[1]-ys[0])
        if abs(d)<1e-9: continue
        gx,gy=np.meshgrid(np.arange(x0,x1+1)+0.5,np.arange(y0,y1+1)+0.5)
        w1=((gx-xs[0])*(ys[2]-ys[0])-(xs[2]-xs[0])*(gy-ys[0]))/d
        w2=((xs[1]-xs[0])*(gy-ys[0])-(gx-xs[0])*(ys[1]-ys[0]))/d
        w0=1-w1-w2
        m=(w0>=0)&(w1>=0)&(w2>=0)
        if not m.any(): continue
        z=w0*Z[a]+w1*Z[b]+w2*Z[cc]
        sub=zb[y0:y1+1,x0:x1+1]; m&=z<sub
        if not m.any(): continue
        sub[m]=z[m]
        nn=n[a]*w0[...,None]+n[b]*w1[...,None]+n[cc]*w2[...,None]
        nn/=np.linalg.norm(nn,axis=-1,keepdims=True)+1e-9
        sh=np.abs(nn@L)*0.8+0.2
        if tex is not None:
            uv=V[[a,b,cc],6:8]; u=w0*uv[0,0]+w1*uv[1,0]+w2*uv[2,0]; v=w0*uv[0,1]+w1*uv[1,1]+w2*uv[2,1]
            tx=np.clip((u%1)*1023,0,1023).astype(int); ty=np.clip((v%1)*1023,0,1023).astype(int)
            col=tex[ty,tx]
        else: col=np.broadcast_to(c,(m.shape[0],m.shape[1],3))
        img[y0:y1+1,x0:x1+1][m]=(col*sh[...,None])[m]
im=Image.fromarray((np.clip(img,0,1)*255).astype(np.uint8)); dr=ImageDraw.Draw(im)
for pf in ptsf:
    for line in open(pf):
        q=line.split()
        if len(q)<4: continue
        p=np.array(list(map(float,q[1:4]))); col=q[4] if len(q)>4 else 'red'
        x=(p@rt-cx)*s+W/2; y=H/2-(p@up-cy)*s
        dr.ellipse([x-4,y-4,x+4,y+4],outline=col,width=2); dr.text((x+5,y-5),q[0],fill=col)
# scale bar text
dr.text((5,H-15),"span %.3f"%span,fill='white')
im.save(out)
