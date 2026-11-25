import subprocess,sys,os
from PIL import Image, ImageChops
D=os.environ.get("ROTH_DATA","/mnt/user-data/uploads/Realms of the Haunting/DATA")+"/M/"
BIN=os.environ.get("ROTHPROBE_BIN","/tmp/rothprobe")
ORACLE=os.environ.get("ORACLE_DIR","/home/claude/oracle_build")
def sec(m,s):
    o=subprocess.run([os.path.join(BIN,"sec"),D+m+".RAW",str(s)],capture_output=True,text=True).stdout.split()
    cx,cy=o[3].strip("()").split(","); return int(cx),int(cy),int(o[-3]),int(o[-1])
def at(m,x,y):
    o=subprocess.run([os.path.join(BIN,"pt"),D+m+".RAW",str(x),str(y)],capture_output=True,text=True).stdout.split("\n")
    return [(int(l.split()[1]),int(l.split()[3]),int(l.split()[5])) for l in o if l]
def shot(m,x,y,a,n,out):
    env=dict(os.environ,ORACLE_FRAMES=str(n))
    subprocess.run(["./oracle_shot.sh",m,str(x),str(y),str(a),out],cwd=ORACLE,env=env,capture_output=True,timeout=300)
m=sys.argv[1]; s=int(sys.argv[2]); wy=int(sys.argv[3]) if len(sys.argv)>3 else 0
cx,cy,f,c=sec(m,s); res=[]
for dx,dy,ang in [(-1,0,384),(1,0,128),(0,-1,0),(0,1,256)]:
    for d in (500,800,1100):
        x,y=cx+dx*d,cy+dy*d; r=at(m,x,y)
        if len(r)!=1 or r[0][0]==s or r[0][2]-r[0][1]<120: continue
        a=f"/tmp/rothprobe_fp_{m}_{s}_{x}_{y}_a.png"; b=a.replace("_a.png","_b.png")
        shot(m,x,y+wy,ang,200,a); shot(m,x,y+wy,ang,260,b)
        try:
            ia,ib=Image.open(a).convert('L'),Image.open(b).convert('L')
            d_=ImageChops.difference(ia,ib).point(lambda v:255 if v>24 else 0)
            frac=sum(d_.histogram()[255:])/(ia.size[0]*ia.size[1])
        except Exception as e: frac=-1
        res.append((frac,x,y+wy,ang,r[0])); print(m,s,x,y+wy,ang,r[0],round(frac,3),flush=True)
print("BEST",max(res) if res else None)
