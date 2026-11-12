// How many linked line portals can one straight line of sight pass through?
//
// r_portal_recursions limits how deep the renderer follows line portals seen through line
// portals. This walks rays through the portal plan of a map, in the Realms plan (before any
// group is moved -- a linked portal is a pure translation, so a straight ray through portals
// is a straight ray in the original plan), and reports the longest chain of cut faces any
// ray crosses while staying inside sectors of the group it is in.
//
// HEIGHTS ARE IGNORED: a ray is stopped by the plan outline only, never by a step, a low
// lintel or a closed door. So the number printed is an UPPER bound on what the eye can do.
// Engine-free, like the planner.
#include "roth_raw.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <map>
#include <set>
#include <algorithm>
#include <string>
#include <vector>
#include "overlap_fn.inc"
#include "roth_portalplan.h"
using namespace roth;
static std::vector<uint8_t> RF(const char*p){std::vector<uint8_t>o;FILE*f=fopen(p,"rb");if(!f)return o;fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);o.resize(n);if(fread(o.data(),1,n,f)!=(size_t)n)o.clear();fclose(f);return o;}
struct Out { std::vector<double> x1,y1,x2,y2; double minx,miny,maxx,maxy; bool ok=false; };
static bool Holds(const Out&o,double x,double y){ if(!o.ok||x<o.minx||x>o.maxx||y<o.miny||y>o.maxy)return false; bool in=false; for(size_t i=0;i<o.x1.size();i++) if((o.y1[i]>y)!=(o.y2[i]>y)){ double xi=o.x1[i]+(y-o.y1[i])*(o.x2[i]-o.x1[i])/(o.y2[i]-o.y1[i]); if(xi>x)in=!in; } return in; }
static bool Cross(double ax,double ay,double bx,double by,double cx,double cy,double dx,double dy){
	auto o=[](double px,double py,double qx,double qy,double rx,double ry){return (qx-px)*(ry-py)-(qy-py)*(rx-px);};
	double d1=o(cx,cy,dx,dy,ax,ay),d2=o(cx,cy,dx,dy,bx,by),d3=o(ax,ay,bx,by,cx,cy),d4=o(ax,ay,bx,by,dx,dy);
	return ((d1>0)!=(d2>0))&&((d3>0)!=(d4>0)); }
int main(int c,char**v){ double step=4; int ndir=720, npt=17; double maxlen=40000;
 for(int a=1;a<c;a++){ std::string path=v[a]; if(path.rfind("step=",0)==0){step=atof(path.c_str()+5);continue;} if(path.rfind("dirs=",0)==0){ndir=atoi(path.c_str()+5);continue;}
  auto d=RF(v[a]); Map m=ParseRaw(d.data(),d.size()); ApplyLoadTimeHeights(m); /* as the loader does, before planning */ std::string name=path.substr(path.rfind('/')+1); if(!m.ok())continue;
  std::vector<RothPlanOverlap> ov; RothFindPlanOverlaps(m,{},ov); if(ov.empty())continue;
  std::vector<rothplan::OverlapPair> in; for(auto&p:ov)in.push_back({p.a,p.b,p.area});
  rothplan::Plan pl=rothplan::BuildPlan(m,in,{},(int)m.vertices.size());
  if(!pl.active||pl.cuts.empty()){ printf("%-13s cuts %zu: no portal is ever seen through\n",name.c_str(),pl.cuts.size()); continue; }
  const int ns=m.sectors.size(), nf=m.faces.size(), nv=m.vertices.size();
  std::vector<Out> out(ns); double X0=1e18,Y0=1e18,X1=-1e18,Y1=-1e18;
  for(int s=0;s<ns;s++){ const Sector&rs=m.sectors[s]; if(rs.faceCount<3||rs.firstFaceIndex<0||rs.firstFaceIndex+rs.faceCount>nf)continue; Out&o=out[s]; bool bad=false,first=true;
   for(int k=0;k<rs.faceCount;k++){ const Face&f=m.faces[rs.firstFaceIndex+k]; if(f.vertex1<0||f.vertex2<0||f.vertex1>=nv||f.vertex2>=nv){bad=true;break;}
    double ax=m.vertices[f.vertex1].x,ay=m.vertices[f.vertex1].y,bx=m.vertices[f.vertex2].x,by=m.vertices[f.vertex2].y; o.x1.push_back(ax);o.y1.push_back(ay);o.x2.push_back(bx);o.y2.push_back(by);
    if(first){o.minx=o.maxx=ax;o.miny=o.maxy=ay;first=false;} o.minx=std::min({o.minx,ax,bx});o.maxx=std::max({o.maxx,ax,bx});o.miny=std::min({o.miny,ay,by});o.maxy=std::max({o.maxy,ay,by}); }
   o.ok=!bad&&o.maxx>o.minx&&o.maxy>o.miny; if(o.ok){X0=std::min(X0,o.minx);Y0=std::min(Y0,o.miny);X1=std::max(X1,o.maxx);Y1=std::max(Y1,o.maxy);} }
  // a coarse grid of sector lists per group
  const double CELL=128; const int GW=(int)((X1-X0)/CELL)+2, GH=(int)((Y1-Y0)/CELL)+2;
  std::vector<std::vector<std::vector<int>>> grid(pl.numGroups, std::vector<std::vector<int>>((size_t)GW*GH));
  for(int s=0;s<ns;s++){ if(!out[s].ok)continue; int g=pl.group[s]; for(int gy=(int)((out[s].miny-Y0)/CELL);gy<=(int)((out[s].maxy-Y0)/CELL);gy++)for(int gx=(int)((out[s].minx-X0)/CELL);gx<=(int)((out[s].maxx-X0)/CELL);gx++)grid[g][(size_t)gy*GW+gx].push_back(s); }
  auto inGroup=[&](int g,double x,double y){ if(x<X0||y<Y0||x>X1||y>Y1)return false; for(int s:grid[g][(size_t)((int)((y-Y0)/CELL))*GW+(int)((x-X0)/CELL)]) if(Holds(out[s],x,y))return true; return false; };
  struct CS{double ax,ay,bx,by;int gRight,gLeft;}; std::vector<CS> cs;
  for(auto&ct:pl.cuts){ const Face&F=m.faces[ct.face]; cs.push_back({(double)m.vertices[F.vertex1].x,(double)m.vertices[F.vertex1].y,(double)m.vertices[F.vertex2].x,(double)m.vertices[F.vertex2].y,ct.groupFace,ct.groupSister}); }
  int best=0; double bx0=0,by0=0,bang=0; std::vector<int> hist(16,0); long rays=0;
  for(size_t a0=0;a0<cs.size();a0++) for(int ip=0;ip<npt;ip++){ double t=(ip+0.5)/npt; double px=cs[a0].ax+(cs[a0].bx-cs[a0].ax)*t, py=cs[a0].ay+(cs[a0].by-cs[a0].ay)*t;
   for(int id=0;id<ndir;id++){ double ang=2*M_PI*(id+0.37)/ndir, dx=cos(ang), dy=sin(ang);
    // which way across the first cut: the owning sector is on the RIGHT of v1->v2
    double ex=cs[a0].bx-cs[a0].ax, ey=cs[a0].by-cs[a0].ay; double side=ex*dy-ey*dx; if(fabs(side)<1e-9)continue;
    int g = side>0 ? cs[a0].gLeft : cs[a0].gRight;   // moving to the left of the face -> into the sister's group
    int from = side>0 ? cs[a0].gRight : cs[a0].gLeft;
    // the ray must be able to ARRIVE: a point just before the cut must be inside the group it comes from
    if(!inGroup(from,px-dx*1.0,py-dy*1.0))continue;
    rays++; int depth=1; double x=px+dx*0.01,y=py+dy*0.01, len=0; size_t last=a0;
    while(len<maxlen){ double nx=x+dx*step, ny=y+dy*step; bool crossed=false;
     for(size_t k=0;k<cs.size();k++){ if(k==last)continue; if(Cross(x,y,nx,ny,cs[k].ax,cs[k].ay,cs[k].bx,cs[k].by)){ double kx=cs[k].bx-cs[k].ax,ky=cs[k].by-cs[k].ay; double sd=kx*dy-ky*dx; int ng=sd>0?cs[k].gLeft:cs[k].gRight, og=sd>0?cs[k].gRight:cs[k].gLeft; if(og!=g)continue; g=ng; depth++; last=k; crossed=true; break; } }
     x=nx;y=ny;len+=step; if(!crossed&&!inGroup(g,x,y))break; }
    if(depth<16)hist[depth]++; if(depth>best){best=depth;bx0=px;by0=py;bang=ang*180/M_PI;} } }
  printf("%-13s cuts %2zu | rays %ld | longest chain of portals on one ray: %d (first found from (%.0f,%.0f) heading %.1f deg) | rays by chain length:",name.c_str(),cs.size(),rays,best,bx0,by0,bang);
  for(int k=1;k<16;k++)if(hist[k])printf(" %d:%d",k,hist[k]); printf("\n");
 } }
