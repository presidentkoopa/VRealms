#include "roth_raw.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char**argv){FILE*f=fopen(argv[1],"rb");std::vector<uint8_t> b;int c;while((c=fgetc(f))!=EOF)b.push_back((uint8_t)c);fclose(f);
 roth::Map m=roth::ParseRaw(b.data(),b.size());
 for(int a=2;a<argc;a++){int s=atoi(argv[a]); auto&sc=m.sectors[s]; printf("sector %d floor %d ceil %d\n",s,sc.floorHeight,sc.ceilingHeight);
  for(size_t i=0;i<m.faces.size();i++){auto&fa=m.faces[i]; bool mine=fa.sector==s; bool other=fa.sister>=0&&m.faces[fa.sister].sector==s; if(!mine) continue; auto&v=m.vertices[fa.vertex1];auto&w=m.vertices[fa.vertex2]; int nb=fa.sister>=0?m.faces[fa.sister].sector:-1;
   printf("  face %zu (%d,%d)-(%d,%d) sister-sector %d %s\n",i,v.x,v.y,w.x,w.y,nb,nb>=0?"":"ONE-SIDED");
   if(nb>=0) printf("     nbr floor %d ceil %d\n",m.sectors[nb].floorHeight,m.sectors[nb].ceilingHeight);}}}
