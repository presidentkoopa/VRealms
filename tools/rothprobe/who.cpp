#include "roth_raw.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); std::vector<uint8_t> b; int c; while((c=fgetc(f))!=EOF) b.push_back((uint8_t)c); fclose(f);
  roth::Map m=roth::ParseRaw(b.data(),b.size()); int k=atoi(argv[2]);
  for(size_t i=0;i<m.commands.size();i++){auto&r=m.commands[i]; if(r.linkIndex==k) printf("#%zu op %02x mod %02x key %u ff %02x sf %02x sector %d faces %zu trig %d\n",i+1,r.opcode,r.modifier,r.key,r.fireFlags,r.subFlags,r.sector,r.faces.size(),(int)roth::IsTriggerOpcode(r.opcode));}
}
