#include "roth_raw.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); std::vector<uint8_t> b; int c; while((c=fgetc(f))!=EOF) b.push_back((uint8_t)c); fclose(f);
  roth::Map m=roth::ParseRaw(b.data(),b.size()); int k=atoi(argv[2]);
  for(size_t i=0;i<m.textureMaps.size();i++){auto&t=m.textureMaps[i]; if(t.midTexture==k||t.upperTexture==k||t.lowerTexture==k) printf("tmap %zu mid %u up %u lo %u\n",i,t.midTexture,t.upperTexture,t.lowerTexture);}
  for(size_t i=0;i<m.faces.size();i++){auto&fa=m.faces[i]; if(fa.textureMap<0) continue; auto&t=m.textureMaps[fa.textureMap]; if(t.midTexture==k||t.upperTexture==k||t.lowerTexture==k){auto&v=m.vertices[fa.vertex1]; auto&w=m.vertices[fa.vertex2]; printf("face %zu sector %d (%d,%d)-(%d,%d)\n",i,fa.sector,v.x,v.y,w.x,w.y);}}
  for(size_t i=0;i<m.sectors.size();i++){auto&s=m.sectors[i]; if(s.floorTexture==k||s.ceilingTexture==k) printf("sector %zu floor %u ceil %u\n",i,s.floorTexture,s.ceilingTexture);}
  for(size_t i=0;i<m.objects.size();i++) for(auto&o:m.objects[i]) if(o.textureIndex+256*o.textureSource==k||o.textureIndex==k%256) printf("obj sector %zu at (%d,%d,%d) tex %u src %u id %u\n",i,o.x,o.y,o.z,o.textureIndex,o.textureSource,o.commandID);
}
