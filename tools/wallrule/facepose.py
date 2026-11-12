import sys, math
sys.argv_=sys.argv
import os
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'oracle_cmp.py')).read().split("def fit")[0])
faces,secs=load_pieces(sys.argv[1]); D=float(sys.argv[2])
for fi in map(int,sys.argv[3:]):
    f=faces[fi]; ax,ay=f['a']; bx,by=f['b']; ex,ey=bx-ax,by-ay; L=math.hypot(ex,ey); nx,ny=ey/L,-ex/L
    cx,cy=(ax+bx)/2+nx*D,(ay+by)/2+ny*D
    ang=round(math.atan2(nx,-ny)*512/(2*math.pi))%512   # looking along -n
    print(f"f{fi} {round(cx)} {round(cy)} {ang}")
