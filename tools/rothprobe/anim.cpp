#include "roth_raw.h"
#include "roth_das.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <map>
static std::vector<uint8_t> rd(const char*p){FILE*f=fopen(p,"rb");std::vector<uint8_t> b;int c;while((c=fgetc(f))!=EOF)b.push_back((uint8_t)c);fclose(f);return b;}
int main(int argc,char**argv){
  auto rb=rd(argv[1]); auto db=rd(argv[2]); roth::Map m=roth::ParseRaw(rb.data(),rb.size()); roth::Pack p; p.Load(db.data(),db.size());
  int sx=m.metadata.startX, sy=m.metadata.startY; printf("start %d %d rot %d\n",sx,sy,m.metadata.rotation);
  std::map<int,int> fr; auto frames=[&](int t){ if(t<=0||t>=0x8000) return 0; auto it=fr.find(t); if(it!=fr.end()) return it->second; int n=(int)p.ReadImage(t,true,false).frames.size(); fr[t]=n; return n;};
  for(size_t i=0;i<m.faces.size();i++){auto&fa=m.faces[i]; if(fa.textureMap<0) continue; auto&t=m.textureMaps[fa.textureMap];
    for(int tx:{(int)t.midTexture,(int)t.upperTexture,(int)t.lowerTexture}){int n=frames(tx); if(n>1){auto&v=m.vertices[fa.vertex1];auto&w=m.vertices[fa.vertex2]; double d=hypot((v.x+w.x)/2.0-sx,(v.y+w.y)/2.0-sy); if(d<1500) printf("face %zu sector %d tex %d frames %d (%d,%d)-(%d,%d) dist %.0f\n",i,fa.sector,tx,n,v.x,v.y,w.x,w.y,d);}}}
}
