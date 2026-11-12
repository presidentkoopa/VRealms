// Standalone harness: Realms faces -> Doom lines exactly as rothmap.cpp's "pending" loop does,
// then GZDoom's own FNodeBuilder (GL nodes), then checks on the result.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <sstream>
#include "doomdata.h"
#include "tarray.h"
#include "r_defs.h"
#include "printf.h"
#include "p_lnspec.h"
#include "roth_raw.h"
#include <algorithm>
#include "overlap_fn.inc"
#include "roth_portalplan.h"
#define private public
#define class struct
#define protected public
#include "nodebuild.h"
#undef private
#undef class
#undef protected
// --- stubs
void I_Error(const char*f,...){va_list a;va_start(a,f);vprintf(f,a);va_end(a);exit(3);}
int Printf(const char*f,...){va_list a;va_start(a,f);vprintf(f,a);va_end(a);return 0;}
int Printf(PrintFlag,const char*f,...){va_list a;va_start(a,f);vprintf(f,a);va_end(a);return 0;}
void *M_Malloc(size_t n){return malloc(n);} void *M_Realloc(void*p,size_t n){return realloc(p,n);} void M_Free(void*p){free(p);}
extern "C" { double c_atan2(double y,double x){return atan2(y,x);} double c_sqrt(double x){return sqrt(x);} }
extern "C" { void fsctor() asm("_ZN7FileSys10FileSystemC1Ev"); void fsctor(){} void fsdtor() asm("_ZN7FileSys10FileSystemD1Ev"); void fsdtor(){} }

using namespace roth;
static std::vector<uint8_t> RF(const char*p){std::vector<uint8_t>o;FILE*f=fopen(p,"rb");if(!f)return o;fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);o.resize(n);if(fread(o.data(),1,n,f)!=(size_t)n)o.clear();fclose(f);return o;}

int main(int argc,char**argv)
{
	auto d=RF(argv[1]); Map rm=ParseRaw(d.data(),d.size()); if(!rm.ok()){printf("ERR\n");return 1;}
	std::set<int> skip, watch; bool dumpsubs=false, verbose=false, full=false, nopoly=false, portals=false, doorcuts=false;
	for(int a=2;a<argc;a++){ std::string s=argv[a]; if(s.rfind("skip=",0)==0){const char*c=s.c_str()+5;while(*c){skip.insert(atoi(c));while(*c&&*c!=',')c++;if(*c)c++;}}
		else if(s.rfind("watch=",0)==0){const char*c=s.c_str()+6;while(*c){watch.insert(atoi(c));while(*c&&*c!=',')c++;if(*c)c++;}} else if(s=="-v")verbose=true; else if(s=="full")full=true; else if(s=="subs")dumpsubs=true; else if(s=="nopoly")nopoly=true; else if(s=="portals")portals=true; else if(s=="doorcuts")doorcuts=true; }
	// pending lines, as rothmap.cpp:967-1003
	std::vector<bool> consumed(rm.faces.size(),false); struct PL{int face,sister;}; std::vector<PL> pend;
	for(size_t i=0;i<rm.faces.size();i++){ if(consumed[i])continue; const Face&f=rm.faces[i];
		if(skip.count(f.sector)||(f.sister>=0&&skip.count(rm.faces[f.sister].sector))){consumed[i]=true;if(f.sister>=0)consumed[f.sister]=true;continue;}
		if(f.sector<0)continue; int s=f.sister;
		if(s>=0&&s<(int)rm.faces.size()&&!consumed[s]&&rm.faces[s].sector>=0){consumed[s]=true;pend.push_back({(int)i,s});}
		else pend.push_back({(int)i,-1});
		consumed[i]=true; }
	// census as rothmap.cpp:454-533
	int platformCount=0, leafCount=0;
	auto leafBuildable=[&](const Sector&rs)->bool{ if(!rs.IsDoorCapable()||rs.faceCount!=4||rs.hingeFace<0)return false; if(rs.firstFaceIndex<0||rs.firstFaceIndex+3>=(int)rm.faces.size())return false; int hr=rs.hingeFace-rs.firstFaceIndex; if(hr<0||hr>3)return false;
		for(int j=0;j<4;j++){const Face&a=rm.faces[rs.firstFaceIndex+j];const Face&b=rm.faces[rs.firstFaceIndex+((j+1)&3)]; if(a.vertex1<0||a.vertex1>=(int)rm.vertices.size())return false; if(a.vertex2<0||a.vertex2>=(int)rm.vertices.size())return false; if(a.vertex2!=b.vertex1)return false;} return true; };
	if(full){ for(auto&rs:rm.sectors){ if(rs.platformIndex>=0&&rs.platformIndex<(int)rm.platforms.size()){auto&mp=rm.platforms[rs.platformIndex]; if(mp.topZ>mp.undersideZ)platformCount++;} if(leafBuildable(rs))leafCount++; } }
	int realV=rm.vertices.size(), realL=pend.size(), realS=rm.sectors.size();
	int nv=realV+platformCount*4+leafCount*8, nl=realL+platformCount*4+leafCount*8, ns=realS+platformCount+leafCount;
	rothplan::Plan plan; plan.group.assign(realS,0); plan.offX.assign(1,0.); plan.offY.assign(1,0.);
	if(portals){ std::vector<RothPlanOverlap> ov; std::vector<int> sk(skip.begin(),skip.end()); RothFindPlanOverlaps(rm,sk,ov); std::vector<rothplan::OverlapPair> in; for(auto&p:ov)in.push_back({p.a,p.b,p.area});
		plan=rothplan::BuildPlan(rm,in,sk,nv,64.,!doorcuts); for(auto&n:plan.notes)printf("PLAN %s\n",n.c_str());
		if(plan.active){ for(int g=1;g<plan.numGroups;g++)printf("PLAN group %d offset (%.0f,%.0f)\n",g,plan.offX[g],plan.offY[g]); } }
	const int xV=nv, xL=nl, xS=ns;   // first extra vertex / line / sector
	if(plan.active){ nv+=plan.extraVerts.size(); nl+=plan.cuts.size(); }   // one more line per cut; no sectors
	vertex_t*V=(vertex_t*)calloc(nv,sizeof(vertex_t)); line_t*L=(line_t*)calloc(nl,sizeof(line_t)); side_t*S=(side_t*)calloc(nl*2,sizeof(side_t)); sector_t*SEC=(sector_t*)calloc(ns,sizeof(sector_t));
	for(int i=0;i<realV;i++){V[i].set((double)rm.vertices[i].x,(double)rm.vertices[i].y);} for(int i=0;i<nv;i++)V[i].vertexnum=i;
	if(plan.active)for(size_t i=0;i<plan.extraVerts.size();i++)V[xV+i].set(plan.extraVerts[i].x,plan.extraVerts[i].y);
	for(int i=0;i<ns;i++)SEC[i].sectornum=i;
	int si=0;
	int xl=xL;
	for(int i=0;i<realL;i++){ const Face&f=rm.faces[pend[i].face]; const int gf=plan.GroupOf(f.sector);
		L[i].v1=&V[plan.VertexFor(gf,f.vertex1)];L[i].v2=&V[plan.VertexFor(gf,f.vertex2)];L[i].linenum=i;
		S[si].sector=&SEC[f.sector];S[si].linedef=&L[i];S[si].sidenum=si;L[i].sidedef[0]=&S[si];L[i].frontsector=&SEC[f.sector];si++;
		if(pend[i].sister>=0){const Face&g=rm.faces[pend[i].sister]; const int gg=plan.GroupOf(g.sector);
			if(g.vertex1!=f.vertex2||g.vertex2!=f.vertex1) printf("LOADER: line %d face %d sister %d do not share reversed vertices (%d,%d) vs (%d,%d)\n",i,pend[i].face,pend[i].sister,f.vertex1,f.vertex2,g.vertex1,g.vertex2);
			auto ct=plan.active?plan.cutByFace.find(pend[i].face):plan.cutByFace.end();
			if(ct==plan.cutByFace.end()){
				if(gf!=gg){printf("PLAN BUG: line %d joins groups %d and %d without a cut\n",i,gf,gg);exit(4);}
				S[si].sector=&SEC[g.sector];S[si].linedef=&L[i];S[si].sidenum=si;L[i].sidedef[1]=&S[si];L[i].backsector=&SEC[g.sector];si++;
			} else { const rothplan::Cut&c=plan.cuts[ct->second]; if(c.face!=pend[i].face||c.sister!=pend[i].sister){printf("PLAN BUG: cut pairing differs from the loader's\n");exit(4);}
				// the primary face keeps this line, ONE-SIDED: the linked portal stands in for what was behind it
				// the sister face gets a line of its own, in its own group's space, one-sided too
				line_t*l2=&L[xl]; l2->v1=&V[plan.VertexFor(gg,g.vertex1)]; l2->v2=&V[plan.VertexFor(gg,g.vertex2)]; l2->linenum=xl; xl++;
				S[si].sector=&SEC[g.sector];S[si].linedef=l2;S[si].sidenum=si;l2->sidedef[0]=&S[si];l2->frontsector=&SEC[g.sector];si++;
				if(l2->v1->fX()-L[i].v2->fX()!=plan.offX[gg]-plan.offX[gf]||l2->v2->fX()-L[i].v1->fX()!=plan.offX[gg]-plan.offX[gf]||l2->v1->fY()-L[i].v2->fY()!=plan.offY[gg]-plan.offY[gf]){printf("PLAN BUG: portal pair not an exact translation\n");exit(4);}
				printf("PORTAL line %d (%.0f,%.0f)-(%.0f,%.0f) sector %d <-> line %d (%.0f,%.0f)-(%.0f,%.0f) sector %d  displacement (%.0f,%.0f)\n",i,L[i].v1->fX(),L[i].v1->fY(),L[i].v2->fX(),L[i].v2->fY(),f.sector,(int)(l2-L),l2->v1->fX(),l2->v1->fY(),l2->v2->fX(),l2->v2->fY(),g.sector,l2->v2->fX()-L[i].v1->fX(),l2->v2->fY()-L[i].v1->fY());
			}
		} }
	if(plan.active){
		if(xl!=nl){printf("PLAN BUG: %d lines emitted, %d allocated\n",xl,nl);exit(4);}
		printf("PLAN geometry: +%zu moved vertices, +%zu portal lines (one-sided pairs), no buffer sectors\n",plan.extraVerts.size(),plan.cuts.size()); }
	TArray<FNodeBuilder::FPolyStart> ps,an;
	if(full){
		// platform control squares, rothmap.cpp:1638-1842
		double voidX=0,voidY=0; for(auto&v:rm.vertices){ if(v.x<voidX)voidX=v.x; if(v.y<voidY)voidY=v.y; }
		double pvx=voidX-4096., pvy=voidY-4096.; int cs=realS, cv=realV, cl=realL, built=0;
		for(int i=0;i<realS;i++){ auto&rs=rm.sectors[i]; if(rs.platformIndex<0||rs.platformIndex>=(int)rm.platforms.size())continue; auto&mp=rm.platforms[rs.platformIndex]; if(mp.topZ<=mp.undersideZ)continue;
			double cx=pvx+double(built%64)*96., cy=pvy-double(built/64)*96.;
			V[cv+0].set(cx,cy);V[cv+1].set(cx+64.,cy);V[cv+2].set(cx+64.,cy+64.);V[cv+3].set(cx,cy+64.);
			for(int e=0;e<4;e++){ S[si].sector=&SEC[cs];S[si].linedef=&L[cl+e];S[si].sidenum=si; L[cl+e].v1=&V[cv+e];L[cl+e].v2=&V[cv+((e+1)&3)];L[cl+e].sidedef[0]=&S[si];L[cl+e].frontsector=&SEC[cs];L[cl+e].linenum=cl+e; si++; }
			cs++;cv+=4;cl+=4;built++; }
		// door leaves, rothmap.cpp:1911-2302
		const double CELL=512.; double lvx=voidX-4096., lvy=voidY-(4096.+4096.); int leaves=0;
		for(int i=0;i<realS;i++){ auto&rs=rm.sectors[i]; if(!leafBuildable(rs))continue; int f0=rs.firstFaceIndex, hr=rs.hingeFace-f0; const Face&hf=rm.faces[rs.hingeFace];
			double hx=rm.vertices[hf.vertex1].x, hy=rm.vertices[hf.vertex1].y; double ox=lvx+double(leaves%32)*CELL, oy=lvy-double(leaves/32)*CELL; double offX=ox+CELL*0.5-hx, offY=oy+CELL*0.5-hy; int tag=1+leaves;
			for(int j=0;j<4;j++){ const Face&f=rm.faces[f0+((hr+j)&3)]; V[cv+j].set(double(rm.vertices[f.vertex1].x)+offX,double(rm.vertices[f.vertex1].y)+offY); }
			for(int j=0;j<4;j++){ line_t*ld=&L[cl+j]; ld->v1=&V[cv+((j+1)&3)]; ld->v2=&V[cv+j]; ld->sidedef[0]=&S[si]; ld->frontsector=&SEC[cs]; ld->linenum=cl+j; ld->special=Polyobj_ExplicitLine; ld->args[0]=tag; ld->args[1]=j+1; S[si].sector=&SEC[cs];S[si].linedef=ld;S[si].sidenum=si; si++; }
			V[cv+4].set(ox,oy);V[cv+5].set(ox,oy+CELL);V[cv+6].set(ox+CELL,oy+CELL);V[cv+7].set(ox+CELL,oy);
			for(int e=0;e<4;e++){ line_t*ld=&L[cl+4+e]; ld->v1=&V[cv+4+e]; ld->v2=&V[cv+4+((e+1)&3)]; ld->sidedef[0]=&S[si]; ld->frontsector=&SEC[cs]; ld->linenum=cl+4+e; S[si].sector=&SEC[cs];S[si].linedef=ld;S[si].sidenum=si; si++; }
			if(!nopoly){ FNodeBuilder::FPolyStart a; a.polynum=tag; a.x=FLOAT2FIXED(hx+plan.offX[plan.GroupOf(i)]); a.y=FLOAT2FIXED(hy+plan.offY[plan.GroupOf(i)]); ps.Push(a); a.x=FLOAT2FIXED(hx+offX); a.y=FLOAT2FIXED(hy+offY); an.Push(a); }
			cs++;cv+=8;cl+=8;leaves++; }
		printf("void geometry: %d platform control squares, %d door leaves (%u spawn spots)\n",built,leaves,ps.Size());
	}
	printf("lines %d sides %d verts %d sectors %d\n",nl,si,nv,ns);
	FNodeBuilder::FLevel lev={V,nv,S,si,L,nl,0,0,0,0}; lev.FindMapBounds();
	FNodeBuilder nb(lev,ps,an,true);
	int nvo=nb.Vertices.Size(); vertex_t*OV=(vertex_t*)calloc(nvo,sizeof(vertex_t));
	for(int i=0;i<nvo;i++){OV[i].set(nb.Vertices[i].x,nb.Vertices[i].y);}
	TArray<FNodeBuilder::glseg_t> segs(nb.Segs.Size()*5/4);
	unsigned nsub=nb.Subsectors.Size(); std::vector<int> first(nsub),num(nsub);
	for(unsigned i=0;i<nsub;i++){num[i]=nb.CloseSubsector(segs,i,OV);first[i]=segs.Size()-num[i];}
	printf("nodes %u subsectors %u glsegs %u (privsegs %u) verts %d\n",nb.Nodes.Size(),nsub,segs.Size(),nb.Segs.Size(),nvo);
	// Realms sector areas (shoelace over faces; owning sector on the right => clockwise => negative)
	std::vector<double> want(ns,0),got(ns,0); 
	for(int s=0;s<realS;s++){ if(skip.count(s))continue; const Sector&rs=rm.sectors[s]; double a=0; for(int k=0;k<rs.faceCount;k++){const Face&f=rm.faces[rs.firstFaceIndex+k]; auto&A=rm.vertices[f.vertex1];auto&B=rm.vertices[f.vertex2]; a+=(double)A.x*B.y-(double)B.x*A.y;} want[s]=-a/2; }
	int bad=0,lt3=0,nonconvex=0,open=0,nosec=0,tiny=0,mixed=0;
	for(unsigned i=0;i<nsub;i++){
		int sec=-1; bool mix=false; for(int k=0;k<num[i];k++){auto&g=segs[first[i]+k]; if(g.sidedef){int s2=g.sidedef->sector->sectornum; if(sec<0)sec=s2; else if(sec!=s2)mix=true;}}
		double a=0; bool op=false, ncv=false; 
		for(int k=0;k<num[i];k++){auto&g=segs[first[i]+k];auto&h=segs[first[i]+(k+1)%num[i]]; if(g.v2!=h.v1)op=true; a+=g.v1->fX()*h.v1->fY()-h.v1->fX()*g.v1->fY();
			auto&j=segs[first[i]+(k+2)%num[i]]; double cr=(h.v1->fX()-g.v1->fX())*(j.v1->fY()-h.v1->fY())-(h.v1->fY()-g.v1->fY())*(j.v1->fX()-h.v1->fX()); if(cr>1e-6)ncv=true; }
		a=-a/2; // fan area, as hw_vertexbuilder builds it from firstline[k].v1
		if(sec>=0)got[sec]+=a; else nosec++;
		if(num[i]<3)lt3++; if(op)open++; if(ncv)nonconvex++; if(mix)mixed++; if(num[i]>=3&&fabs(a)<1.0)tiny++;
		bool w = sec>=0&&watch.count(sec)&&dumpsubs;
		if(w||(verbose&&(op||ncv||mix||num[i]<3))){ printf("SUB %u sector %d nsegs %d fanarea %.3f%s%s%s\n",i,sec,num[i],a,op?" OPEN":"",ncv?" NONCONVEX":"",mix?" MIXED":"");
			for(int k=0;k<num[i];k++){auto&g=segs[first[i]+k]; printf("   (%.4f,%.4f)->(%.4f,%.4f) %s line %d sec %d\n",g.v1->fX(),g.v1->fY(),g.v2->fX(),g.v2->fY(),g.linedef?"seg ":"mini",g.linedef?(int)(g.linedef-L):-1,g.sidedef?g.sidedef->sector->sectornum:-1);} }
	}
	printf("subsectors: <3 segs %d, open loops %d, nonconvex %d, mixed-sector %d, no-sector %d, area<1 %d\n",lt3,open,nonconvex,mixed,nosec,tiny);
	for(int s=0;s<realS;s++){ if(skip.count(s))continue; if(fabs(want[s]-got[s])>0.51){bad++; printf("DEF %d %.0f %.0f\n",s,want[s],got[s]); if(bad<=40||watch.count(s))printf("AREA sector %d realms %.1f subsectors %.1f diff %.1f\n",s,want[s],got[s],got[s]-want[s]);} }
	printf("sectors whose subsector area != polygon area: %d of %d\n",bad,realS);
	// ---- emulate SetRenderSector / FixMinisegReferences / FixHoles (glnodes.cpp:1286, renderinfo.cpp:763,815)
	{
		struct ES{int v1,v2,sec,partner,sub;bool mini;}; std::vector<ES> E(segs.Size());
		for(unsigned i=0;i<segs.Size();i++){auto&g=segs[i]; E[i].v1=g.v1-OV;E[i].v2=g.v2-OV;E[i].sec=g.sidedef?g.sidedef->sector->sectornum:-1;E[i].mini=!g.sidedef; E[i].partner=-1; if(g.Partner!=UINT_MAX){unsigned st=nb.Segs[g.Partner].storedseg; if(st!=UINT_MAX)E[i].partner=st;} }
		for(unsigned i=0;i<nsub;i++)for(int k=0;k<num[i];k++)E[first[i]+k].sub=i;
		for(auto&e:E){ if(e.partner>=0&&!e.mini&&E[e.partner].mini){E[e.partner].partner=-1;e.partner=-1;} }
		for(size_t i=0;i<E.size();i++){ if(E[i].partner>=0&&E[E[i].partner].partner!=(int)i)E[i].partner=-1; }
		struct SS{int first,num,rsec;bool hole;}; std::vector<SS> SUB(nsub); std::vector<int> und;
		for(unsigned i=0;i<nsub;i++){ SUB[i]={first[i],num[i],-1,false}; for(int k=0;k<num[i];k++){auto&e=E[first[i]+k]; if(!e.mini&&(e.partner<0||(!E[e.partner].mini&&e.sec!=E[e.partner].sec))){SUB[i].rsec=e.sec;break;}} if(SUB[i].rsec<0)und.push_back(i); }
		while(!und.empty()){ bool del=false; for(int i=(int)und.size()-1;i>=0;i--){ auto&ss=SUB[und[i]]; for(int k=0;k<ss.num;k++){auto&e=E[ss.first+k]; if(e.partner>=0){int b=SUB[E[e.partner].sub].rsec; if(b>=0){ss.rsec=b;und.erase(und.begin()+i);del=true;break;}}} }
			if(!del){ for(int u:und){ for(int k=0;k<SUB[u].num;k++) if(E[SUB[u].first+k].sec>=0){SUB[u].rsec=E[SUB[u].first+k].sec;break;} } break; } }
		// FixMinisegReferences
		{ std::vector<int> b; for(size_t i=0;i<E.size();i++) if(E[i].mini&&E[i].partner<0)b.push_back(i);
		  for(size_t i=0;i<b.size();i++){ int pick=-1; for(size_t j=i+1;j<b.size();j++){ if(E[b[i]].v1==E[b[j]].v2&&E[b[j]].v1==E[b[i]].v2&&SUB[E[b[i]].sub].rsec==SUB[E[b[j]].sub].rsec){pick=j;break;} } if(pick>=0){E[b[pick]].partner=b[i];E[b[i]].partner=b[pick];b.erase(b.begin()+pick);b.erase(b.begin()+i);i--;} } }
		// FixHoles
		std::vector<std::vector<int>> loops; int unpartnered=0;
		{ std::vector<int> b; for(size_t i=0;i<E.size();i++) if(E[i].mini&&E[i].partner<0)b.push_back(i); unpartnered=b.size();
		  loops.emplace_back(); int li=0; int start=-1,check=-1;
		  while(!b.empty()){ bool found=false; if(loops[li].empty()){start=b.back();b.pop_back();loops[li].push_back(start);check=start;}
		    for(size_t i=0;i<b.size();i++){ int s1=b[i]; if(E[s1].v1==E[check].v2&&SUB[E[s1].sub].rsec==SUB[E[check].sub].rsec){ found=true; loops[li].push_back(s1); b.erase(b.begin()+i); i--; check=s1; if(E[s1].v2==E[start].v1){loops.emplace_back();li=loops.size()-1;} } }
		    if(!found){ if(!loops[li].empty()&&E[loops[li][0]].v1!=E[loops[li].back()].v2) loops[li].clear(); } }
		}
		int nholes=0; std::vector<std::vector<std::pair<double,double>>> holepoly; std::vector<int> holesec;
		for(auto&l:loops){ if(l.empty())continue; nholes++; std::vector<std::pair<double,double>> p; for(int sidx:l)p.push_back({OV[E[sidx].v2].fX(),OV[E[sidx].v2].fY()}); holepoly.push_back(p); holesec.push_back(SUB[E[l[0]].sub].rsec); }
		printf("after SetRenderSector/FixMinisegReferences: unpartnered minisegs %d; FixHoles adds %d hole subsectors\n",unpartnered,nholes);
		// what the flat pass would actually put down, per Realms sector, sampled on a 2-unit lattice
		auto inTri=[&](double px,double py,double ax,double ay,double bx,double by,double cx,double cy){ double d1=(px-bx)*(ay-by)-(ax-bx)*(py-by),d2=(px-cx)*(by-cy)-(bx-cx)*(py-cy),d3=(px-ax)*(cy-ay)-(cx-ax)*(py-ay); bool neg=(d1<0)||(d2<0)||(d3<0),pos=(d1>0)||(d2>0)||(d3>0); return !(neg&&pos); };
		auto inPoly=[&](const std::vector<std::pair<double,double>>&p,double x,double y){ bool in=false; for(size_t i=0,j=p.size()-1;i<p.size();j=i++){ if((p[i].second>y)!=(p[j].second>y)){ double xi=p[j].first+(y-p[j].second)*(p[i].first-p[j].first)/(p[i].second-p[j].second); if(xi>x)in=!in; } } return in; };
		for(int ws:watch){ if(ws>=realS)continue; const Sector&rs=rm.sectors[ws]; std::vector<std::pair<double,double>> a,b2; double mnx=1e9,mny=1e9,mxx=-1e9,mxy=-1e9; std::vector<double> ex1,ey1,ex2,ey2;
			for(int k=0;k<rs.faceCount;k++){const Face&f=rm.faces[rs.firstFaceIndex+k];auto&A=rm.vertices[f.vertex1];auto&B=rm.vertices[f.vertex2]; ex1.push_back(A.x);ey1.push_back(A.y);ex2.push_back(B.x);ey2.push_back(B.y); mnx=std::min(mnx,(double)A.x);mxx=std::max(mxx,(double)A.x);mny=std::min(mny,(double)A.y);mxy=std::max(mxy,(double)A.y);}
			auto inSec=[&](double x,double y){bool in=false; for(size_t i=0;i<ex1.size();i++){ if((ey1[i]>y)!=(ey2[i]>y)){double xi=ex1[i]+(y-ey1[i])*(ex2[i]-ex1[i])/(ey2[i]-ey1[i]); if(xi>x)in=!in;}} return in;};
			long tot=0,cov=0,covOther=0; const double G=2; int cell=16; int W=(int)ceil((mxx-mnx)/cell),H=(int)ceil((mxy-mny)/cell); std::vector<int> ct(W*H,0),cc(W*H,0);
			for(double y=mny+0.37;y<mxy;y+=G)for(double x=mnx+0.61;x<mxx;x+=G){ if(!inSec(x,y))continue; tot++; bool c=false; const double X0=x,Y0=y; x+=plan.offX[plan.GroupOf(ws)]; y+=plan.offY[plan.GroupOf(ws)];
				for(unsigned i=0;i<nsub&&!c;i++){ if(SUB[i].rsec!=ws||SUB[i].num<3)continue; auto&v0=*segs[SUB[i].first].v1; for(int k=2;k<SUB[i].num&&!c;k++){auto&v1=*segs[SUB[i].first+k-1].v1;auto&v2=*segs[SUB[i].first+k].v1; if(inTri(x,y,v0.fX(),v0.fY(),v1.fX(),v1.fY(),v2.fX(),v2.fY()))c=true;} }
				for(size_t h=0;h<holepoly.size()&&!c;h++){ if(holesec[h]==ws&&holepoly[h].size()>=3&&inPoly(holepoly[h],x,y))c=true; }
				x=X0;y=Y0; int cx=(int)((x-mnx)/cell),cy=(int)((y-mny)/cell); ct[cy*W+cx]++; if(c){cov++;cc[cy*W+cx]++;} }
			printf("FLAT COVERAGE sector %d: %.1f%% of its plan gets a floor/ceiling (%ld of %ld samples)\n",ws,tot?100.0*cov/tot:0.0,cov,tot);
			if(verbose){ printf("  map of sector %d, x %g..%g left to right, y %g (top) .. %g, %d-unit cells: #=drawn .=HOLE +=partly\n",ws,mnx,mxx,mxy,mny,cell); for(int y=H-1;y>=0;y--){ printf("  "); for(int x=0;x<W;x++){ int t=ct[y*W+x],c2=cc[y*W+x]; putchar(t==0?' ':c2==t?'#':c2==0?'.':'+'); } printf("\n"); } }
		}
	}
	// every real line side must be covered by segs
	std::map<int,double> cov; for(unsigned i=0;i<segs.Size();i++){auto&g=segs[i]; if(g.sidedef){cov[g.sidedef->sidenum]+=hypot(g.v2->fX()-g.v1->fX(),g.v2->fY()-g.v1->fY());}}
	int uncovered=0; for(int i=0;i<si;i++){line_t*l=S[i].linedef; if(l-L>=realL)continue; double len=hypot(rm.vertices[rm.faces[pend[l-L].face].vertex2].x-rm.vertices[rm.faces[pend[l-L].face].vertex1].x,rm.vertices[rm.faces[pend[l-L].face].vertex2].y-rm.vertices[rm.faces[pend[l-L].face].vertex1].y); if(fabs(cov[i]-len)>0.01){uncovered++; if(uncovered<=30)printf("SIDE %d (line %d, sector %d) length %.3f covered by segs %.3f\n",i,(int)(l-L),S[i].sector->sectornum,len,cov[i]);}}
	printf("sides not exactly covered by segs: %d of %d\n",uncovered,si);
	return 0;
}
