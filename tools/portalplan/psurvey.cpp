// Survey: plan-overlap -> portal plan, per map. Engine-free.
#include "roth_raw.h"
#include <stdio.h>
#include <stdlib.h>
#include <set>
#include <map>
#include <math.h>
#include <algorithm>
#include <string>
#include "overlap_fn.inc"
#include "roth_portalplan.h"
using namespace roth;
static std::vector<uint8_t> RF(const char*p){std::vector<uint8_t>o;FILE*f=fopen(p,"rb");if(!f)return o;fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);o.resize(n);if(fread(o.data(),1,n,f)!=(size_t)n)o.clear();fclose(f);return o;}
int main(int c,char**v){ bool detail=false; double minArea=64;
 for(int a=1;a<c;a++){ std::string path=v[a]; if(path=="-d"){detail=true;continue;} if(path.rfind("min=",0)==0){minArea=atof(path.c_str()+4);continue;}
  auto d=RF(v[a]); Map m=ParseRaw(d.data(),d.size()); std::string name=path.substr(path.rfind('/')+1); if(!m.ok()){printf("%s ERR\n",name.c_str());continue;}
  std::vector<RothPlanOverlap> ov; RothFindPlanOverlaps(m,{},ov); if(ov.empty())continue;
  std::vector<rothplan::OverlapPair> in; for(auto&p:ov)in.push_back({p.a,p.b,p.area});
  rothplan::Plan pl=rothplan::BuildPlan(m,in,{},(int)m.vertices.size(),minArea);
  int minx=1e9,maxx=-1e9,miny=1e9,maxy=-1e9; for(auto&vx:m.vertices){minx=std::min(minx,(int)vx.x);maxx=std::max(maxx,(int)vx.x);miny=std::min(miny,(int)vx.y);maxy=std::max(maxy,(int)vx.y);}
  std::vector<int> gs(pl.numGroups,0); for(size_t s=0;s<m.sectors.size();s++)gs[pl.group[s]]++;
  int flush=0,step=0,ledge=0,blind=0,pen=0,doorcut=0,trans=0; int climb=m.metadata.MaxClimb();
  for(auto&ct:pl.cuts){ auto&A=m.sectors[m.faces[ct.face].sector];auto&B=m.sectors[m.faces[ct.sister].sector];
    int lo=std::max(A.floorHeight,B.floorHeight),hi=std::min(A.ceilingHeight,B.ceilingHeight);
    if(hi<=lo)blind++; else if(A.floorHeight==B.floorHeight&&A.ceilingHeight==B.ceilingHeight)flush++; else if(abs(A.floorHeight-B.floorHeight)<=climb)step++; else ledge++;
    if(ct.cost>13)pen++; if(A.IsDoorCapable()||B.IsDoorCapable())doorcut++; }
  printf("%-13s sectors %4d x[%d..%d] y[%d..%d] | pairs %3zu slivers %d | groups %d (",name.c_str(),(int)m.sectors.size(),minx,maxx,miny,maxy,ov.size(),pl.ignoredSlivers,pl.numGroups);
  for(int g=0;g<pl.numGroups;g++)printf("%s%d",g?"/":"",gs[g]); printf(") | cuts %zu: flush %d step %d ledge/window %d blind %d | penalised %d door %d | depth %.0f | %s\n",pl.cuts.size(),flush,step,ledge,blind,pen,doorcut,pl.depth,pl.active?"OK":"NOT ACTIVE");
  for(auto&n:pl.notes)printf("     note: %s\n",n.c_str());
  if(detail){ for(int g=1;g<pl.numGroups;g++){printf("     group %d offset (%.0f,%.0f):",g,pl.offX[g],pl.offY[g]); for(size_t s=0;s<m.sectors.size();s++)if(pl.group[s]==g)printf(" %zu",s); printf("\n");}
   for(auto&p:ov)printf("     overlap %d(z %d..%d g%d) x %d(z %d..%d g%d) ~%.0f%s\n",p.a,m.sectors[p.a].floorHeight,m.sectors[p.a].ceilingHeight,pl.group[p.a],p.b,m.sectors[p.b].floorHeight,m.sectors[p.b].ceilingHeight,pl.group[p.b],p.area,pl.group[p.a]==pl.group[p.b]?"  SAME GROUP":"");
   for(auto&ct:pl.cuts){ auto&F=m.faces[ct.face]; auto&A=m.sectors[F.sector];auto&B=m.sectors[m.faces[ct.sister].sector]; auto&V1=m.vertices[F.vertex1];auto&V2=m.vertices[F.vertex2];
    printf("     cut face %d|%d  (%d,%d)-(%d,%d) len %.0f  sector %d g%d z %d..%d cmd %04x | sector %d g%d z %d..%d cmd %04x  cost %d\n",ct.face,ct.sister,V1.x,V1.y,V2.x,V2.y,hypot(V2.x-V1.x,V2.y-V1.y),F.sector,ct.groupFace,A.floorHeight,A.ceilingHeight,A.commandID,m.faces[ct.sister].sector,ct.groupSister,B.floorHeight,B.ceilingHeight,B.commandID,ct.cost); } }
 } }
