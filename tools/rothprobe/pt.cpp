#include "roth_raw.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char**argv){FILE*f=fopen(argv[1],"rb");std::vector<uint8_t> b;int c;while((c=fgetc(f))!=EOF)b.push_back((uint8_t)c);fclose(f);
 roth::Map m=roth::ParseRaw(b.data(),b.size()); double px=atof(argv[2]),py=atof(argv[3]);
 for(size_t s=0;s<m.sectors.size();s++){int cross=0; for(auto&fa:m.faces){ if(fa.sector!=(int)s) continue; auto&a=m.vertices[fa.vertex1];auto&bb=m.vertices[fa.vertex2];
   if(((a.y>py)!=(bb.y>py)) && (px < (bb.x-a.x)*(py-a.y)/(double)(bb.y-a.y)+a.x)) cross++;}
   if(cross&1) printf("sector %zu floor %d ceil %d\n",s,m.sectors[s].floorHeight,m.sectors[s].ceilingHeight);}
}
