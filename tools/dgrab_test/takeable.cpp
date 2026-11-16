// tools/dgrab_test/takeable.cpp -- WHICH REALMS OBJECTS RSQ DISTANCE GRAB MAY TAKE, per map, by the rule of
// src/roth/roth_runtime.cpp ObjectIsPickupItem (after ROTH.C activate_targeted_object, input.c:1026-1066), re-stated
// over the engine's own readers (roth_raw, DBASE100, IsItemIdPickable). Engine-free. It restates the rule: it is a
// check that the rule picks the original's items, not a test of the engine code itself.
//   tools/dgrab_test/run.sh [MAP ...]   (ROTH_DATA = the install's DATA folder)
#include <cstdio>
#include <string>
#include <vector>
#include "roth_raw.h"
#include "roth_install.h"
#include "roth_dbase100.h"
#include "roth_inventory.h"
#include "roth_log.h"
using namespace roth;
static std::vector<uint8_t> ReadAll(const std::string &p){std::vector<uint8_t> b;FILE*f=fopen(p.c_str(),"rb");if(!f)return b;fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);b.resize(n);if(fread(b.data(),1,n,f)!=(size_t)n)b.clear();fclose(f);return b;}
int main(int argc,char**argv){
 TheInstall().Open(argv[1]); Dbase100 &db=TheDbase100(); Log log; LoadDbase100FromInstall(TheInstall(),db,&log);
 std::vector<uint8_t> raw=ReadAll(TheInstall().MapFile(argv[2])); Map m=ParseRaw(raw.data(),raw.size());
 int cand=0,take=0;
 for(size_t s=0;s<m.objects.size();s++)for(size_t i=0;i<m.objects[s].size();i++){const Object&o=m.objects[s][i];
  int32_t rid=(int32_t)(uint16_t)(o.textureIndex|(o.textureSource<<8));
  if(o.renderType&0x12)continue; if(!IsItemIdPickable(db,rid))continue; cand++;
  int item=db.FindItemByRenderId((uint16_t)rid); const DbItem*it=db.Item(item);
  bool bound=false,useAny=false,gives=false;
  if(o.commandID)for(size_t k=0;k<m.commands.size();k++){const Command&c=m.commands[k];if(!c.isTrigger||c.key!=o.commandID||c.disabled)continue;
   if(c.opcode==0x30){bound=true;continue;}
   if((c.opcode==0x08||c.opcode==0x02)&&c.chainStart){bound=true;useAny=true;int g=0;for(uint16_t q=c.chainStart;q&&g<64;g++){if(q>m.commands.size())break;const Command&x=m.commands[q-1];if(g>0&&x.isTrigger)break;if(x.opcode==0x29&&(x.key==0||x.key==item))gives=true;q=x.nextIndex;}}}
  bool ok; const char*why;
  if(!bound){ ok=!(o.renderType&1)&&!(o.renderType&0x10); why="no record: the type-4 pickup"; }
  else { ok=gives; why=gives?"its use chain gives it":(useAny?"its use chain gives no such item":"examine record only"); }
  if(ok)take++;
  printf("%-4s %zu/%zu item %3d id %4u b9 0x%02x  %s\n",ok?"TAKE":"no",s,i,item,o.commandID,o.renderType,why);
 }
 printf("%s: %d item objects, %d takeable\n",argv[2],cand,take);}
