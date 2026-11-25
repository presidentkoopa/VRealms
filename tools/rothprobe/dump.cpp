#include "roth_raw.h"
#include <cstdio>
#include <vector>
int main(int argc,char**argv){
  for(int a=1;a<argc;a++){
  FILE*f=fopen(argv[a],"rb"); if(!f) continue; std::vector<uint8_t> b; int c; while((c=fgetc(f))!=EOF) b.push_back((uint8_t)c); fclose(f);
  roth::Map m=roth::ParseRaw(b.data(),b.size());
  if(m.categories[8].empty()) continue;
  printf("%s:",argv[a]);
  for(uint16_t i:m.categories[8]){ const auto&r=m.commands[i-1]; printf(" #%u(op %02x key %u ff %02x mod %02x link %u)",i,r.opcode,r.key,r.fireFlags,r.modifier,r.linkIndex);} printf("\n");}
}
