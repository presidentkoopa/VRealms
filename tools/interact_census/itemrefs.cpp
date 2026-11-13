// itemrefs.cpp -- which inventory items the level scripts test "in hand" (RAW opcode 0x27), per map. Build as census.cpp.
#include <cstdio>
#include <map>
#include "roth_raw.h"
#include "roth_install.h"
#include "roth_dbase100.h"
#include "roth_log.h"
using namespace roth;
static std::vector<uint8_t> ReadAll(const std::string &p){std::vector<uint8_t> b;FILE*f=fopen(p.c_str(),"rb");if(!f)return b;fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);b.resize(n);if(fread(b.data(),1,n,f)!=(size_t)n)b.clear();fclose(f);return b;}
int main(int, char**argv){TheInstall().Open(argv[1]);Dbase100 db;Log log;LoadDbase100FromInstall(TheInstall(),db,&log);
 std::map<int,int> uses; std::map<int,std::string> where;
 for(auto&name:TheInstall().MapNames()){auto raw=ReadAll(TheInstall().MapFile(name.c_str()));if(raw.empty())continue;Map m=ParseRaw(raw.data(),raw.size());if(!m.ok())continue;
  for(auto&c:m.commands) if(!c.isTrigger && c.opcode==0x27){uses[c.key]++; if(where[c.key].find(name)==std::string::npos) where[c.key]+=name+" ";}}
 printf("RAW 0x27 (test the item in hand) by item: %zu distinct items, %d tests\n",uses.size(),[&]{int t=0;for(auto&u:uses)t+=u.second;return t;}());
 for(auto&u:uses){const DbItem*it=db.Item(u.first); printf("  item %3d x%-3d %s\n",u.first,u.second,where[u.first].c_str());}
}
