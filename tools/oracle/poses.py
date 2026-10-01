import os
import sys, numpy as np
sys.path.insert(0,os.environ.get('ORACLE_DIR','.'))
import walls as Wm
from rawmap import RawMap, sectors_at
def pose_for_face(m, f, dist=220):
    a,b=f['a'],f['b']; mid=(a+b)/2; e=(b-a)/np.hypot(*(b-a))
    for nrm in (np.array([-e[1],e[0]]), np.array([e[1],-e[0]])):
        for dd in (dist, dist*0.7, dist*0.5, dist*0.35, 60):
            p=mid+nrm*dd
            ss=sectors_at(m,(p[0],p[1]))
            if f['S']['off'] in ss:
                look=-nrm; th=np.degrees(np.arctan2(look[1],look[0]))
                ang=int(round((th-90)*512/360))%512
                S=f['S']
                return (int(round(p[0]))&0xffff, int(round(p[1]))&0xffff, ang, S['off'], S['floorH']&0xffff)
    return None
