import json, sys, numpy as np, csv
sys.path.insert(0,'tools/fbxload_test')
import walk_poses as wp, render as R
from PIL import Image, ImageDraw
d=json.load(open('/tmp/fbxload_test_out/walk/take.json'))
tex=np.array(Image.open('vrealms_iwad/models/hands/lambert1_Base_Color.png').convert('RGB'))
step=int(sys.argv[1]); out=sys.argv[2]
for name,isR in (("Hand_Low",True),("Hand_Low1",False)):
    h=wp.Hand(name,d[name],isR)
    rows=list(csv.DictReader(open(f'/tmp/fbxload_test_out/walk/walk_{name}.csv')))
    mot=[float(r['motion']) for r in rows]
    frames=sorted(set(list(range(0,1279,step))+[311,693]))
    P0,_,_,_=wp.skin(h,[list(x) for x in h.rest]); pad=0.25*(P0[:,2].max()-P0[:,2].min())
    b=(P0[:,0].min()-pad,P0[:,0].max()+pad,P0[:,2].min()-pad,P0[:,2].max()+pad)
    tiles=[]
    for fr in frames:
        P,N,UV,T=wp.skin(h,wp.locals_for(h,wp.whole(fr)))
        im=R.render(P,N,UV,T,tex,+1,bounds=b)
        tiles.append((fr,Image.fromarray(im).resize((180,180))))
    cols=12; TW=180; rws=(len(tiles)+cols-1)//cols
    img=Image.new('RGB',(cols*TW,rws*(TW+16)),(0,0,0)); dr=ImageDraw.Draw(img)
    for i,(fr,im) in enumerate(tiles):
        x,y=(i%cols)*TW,(i//cols)*(TW+16); img.paste(im,(x,y+16))
        dr.text((x+3,y+2),"frame %d (Blender %d)%s"%(fr,fr+1," held" if mot[fr]==0 else ""),fill=(255,255,0) if fr in (311,693) else (220,220,220))
    img.save(f'{out}_{"right" if isR else "left"}.png'); print('wrote',out,len(tiles))
