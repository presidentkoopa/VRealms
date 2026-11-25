#include "roth_raw.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); std::vector<uint8_t> b; int c; while((c=fgetc(f))!=EOF) b.push_back((uint8_t)c); fclose(f);
  roth::Map m=roth::ParseRaw(b.data(),b.size());
  for(int a=2;a<argc;a++){int fi=atoi(argv[a]); auto&fa=m.faces[fi]; auto&t=m.textureMaps[fa.textureMap]; printf("face %d tmap %d off %u: mid %u up %u lo %u flags %02x ext %d faceID %u coll %04x\n",fi,fa.textureMap,fa.textureMapOffset,t.midTexture,t.upperTexture,t.lowerTexture,t.flags,t.extended,t.faceID,fa.collisionFlags);}
}
