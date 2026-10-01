import os
import numpy as np, struct
exec(open(os.environ.get('ORACLE_DIR','.')+'/decode.py').read().split("if __name__")[0])
W,H=640,480
def load_all(prefix='cap'):
    hdr,valid,tex,col,row=decode(prefix)
    d=open(f'{prefix}_0.bin','rb').read()
    tag=np.frombuffer(d[32+W*H:32+W*H+4*W*H],np.uint32).reshape(H,W)
    return hdr,valid,tex,col,row,tag
