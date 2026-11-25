#include "roth_das.h"
#include <cstdio>
#include <vector>
#include <cstdlib>
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); std::vector<uint8_t> b; int c; while((c=fgetc(f))!=EOF) b.push_back((uint8_t)c); fclose(f);
  roth::Pack p; p.Load(b.data(),b.size());
  for(int a=3;a<argc;a++){ int i=atoi(argv[a]); roth::Image im=p.ReadImage(i,true,true);
   printf("%d: %dx%d frames %zu rle %d timer %u rate %u tpf %d\n",i,im.width,im.height,im.frames.size(),im.rle,im.timer,im.rate,im.ticksPerFrame);
   for(size_t fr=0;fr<im.frames.size();fr++){ char fn[256]; snprintf(fn,256,"%s/%d_%zu.ppm",argv[2],i,fr); FILE*o=fopen(fn,"wb"); fprintf(o,"P6 %d %d 255\n",im.width,im.height);
     for(int k=0;k<im.width*im.height;k++){auto col=p.Palette()[im.frames[fr][k]]; fputc(col.r,o);fputc(col.g,o);fputc(col.b,o);} fclose(o);} }
}
