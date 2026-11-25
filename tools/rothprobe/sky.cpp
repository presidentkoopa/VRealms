#include "roth_raw.h"
#include <cstdio>
#include <vector>
int main(int argc,char**argv){for(int a=1;a<argc;a++){FILE*f=fopen(argv[a],"rb");std::vector<uint8_t> b;int c;while((c=fgetc(f))!=EOF)b.push_back((uint8_t)c);fclose(f);
 roth::Map m=roth::ParseRaw(b.data(),b.size()); printf("%s sky %u ambience %u tint %u unk %u\n",argv[a],m.metadata.skyTexture,m.metadata.lightAmbience,m.metadata.tintFlag,m.metadata.unk);}}
