#include "roth_raw.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); std::vector<uint8_t> b; int c; while((c=fgetc(f))!=EOF) b.push_back((uint8_t)c); fclose(f);
  roth::Map m=roth::ParseRaw(b.data(),b.size());
  for(int a=2;a<argc;a++){int i=atoi(argv[a]); for(int n=0;n<8&&i>0&&i<=(int)m.commands.size();n++){auto&r=m.commands[i-1]; printf("#%d op %02x mod %02x key %u aux %u link %u args",i,r.opcode,r.modifier,r.key,r.aux,r.linkIndex); for(auto w:r.args) printf(" %u",w); printf(" sector %d faces %zu\n",r.sector,r.faces.size()); if(r.nextIndex==0||r.nextIndex==i) break; i=r.nextIndex;} printf("--\n");}
  for(size_t i=0;i<m.commands.size();i++){auto&r=m.commands[i]; for(auto w:r.args) if(w==762){printf("record #%zu op %02x has 762\n",i+1,r.opcode);break;}}
}
