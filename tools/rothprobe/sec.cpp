#include "roth_raw.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); std::vector<uint8_t> b; int c; while((c=fgetc(f))!=EOF) b.push_back((uint8_t)c); fclose(f);
  roth::Map m=roth::ParseRaw(b.data(),b.size());
  for(int a=2;a<argc;a++){int s=atoi(argv[a]); double sx=0,sy=0;int n=0; int x0=1e9,x1=-1e9,y0=1e9,y1=-1e9;
   for(auto&fa:m.faces) if(fa.sector==s){for(int v:{fa.vertex1,fa.vertex2}){auto&p=m.vertices[v]; sx+=p.x;sy+=p.y;n++; if(p.x<x0)x0=p.x; if(p.x>x1)x1=p.x; if(p.y<y0)y0=p.y; if(p.y>y1)y1=p.y;}}
   printf("sector %d centre (%.0f,%.0f) box x %d..%d y %d..%d floor %d ceil %d\n",s,sx/n,sy/n,x0,x1,y0,y1,m.sectors[s].floorHeight,m.sectors[s].ceilingHeight);}
}
