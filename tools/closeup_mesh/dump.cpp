// Scratch: dump close-up films (indexed frames, palette, masks) and the fitted turntable camera.
#include "roth_voxelize.h"
#include "roth_dbase100.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <sys/stat.h>
using namespace roth;
static std::vector<uint8_t> ReadFile(const std::string &p){std::vector<uint8_t> b;FILE*f=fopen(p.c_str(),"rb");if(!f)return b;fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);b.resize(n);fread(b.data(),1,n,f);fclose(f);return b;}
static void W(const std::string&p,const void*d,size_t n){FILE*f=fopen(p.c_str(),"wb");fwrite(d,1,n,f);fclose(f);}
int main(int argc,char**argv){
  std::string dir=argv[1], out=argv[2];
  auto d100=ReadFile(dir+"/DBASE100.DAT"), d300=ReadFile(dir+"/DBASE300.DAT");
  Dbase100 db; if(!db.Load(d100.data(),d100.size())){fprintf(stderr,"db\n");return 1;}
  for(int a=3;a<argc;a++){
    int id=atoi(argv[a]); const DbItem*it=db.Item(id); if(!it||!it->closeup){printf("%d: no film\n",id);continue;}
    const uint8_t*g;size_t gs; if(!VoxCloseupBytes(d300.data(),d300.size(),it->closeup,g,gs)){printf("%d bad\n",id);continue;}
    VoxFilm film; std::string err; if(!VoxDecodeFilm(g,gs,film,&err)){printf("%d %s\n",id,err.c_str());continue;}
    std::vector<uint8_t> masks; VoxBuildMasks(film,masks);
    VoxGeometry G=VoxEstimateTurntable(film,masks);
    std::string d=out+"/"+std::to_string(id); mkdir(d.c_str(),0755);
    W(d+"/frames.bin",film.pixels.data(),film.pixels.size()); W(d+"/palette.bin",film.palette,768); W(d+"/masks.bin",masks.data(),masks.size());
    FILE*f=fopen((d+"/meta.txt").c_str(),"w");
    const VoxCamera&c=G.camera;
    fprintf(f,"w %d\nh %d\nframes %d\nturntable %d\nperiod %.9f\ndirection %d\nelevation %.9f\naxisX %.9f\naxisY %.9f\nroll %.9f\ninvDistance %.12f\nfirstFrame %d\nlastFrame %d\nmeanIoU %.4f\nminIoU %.4f\nelevOpen %d\n",
      film.width,film.height,film.frames,(int)G.turntable,c.period,c.direction,c.elevation,c.axisX,c.axisY,c.roll,c.invDistance,c.firstFrame,c.lastFrame,G.meanIoU,G.minFrameIoU,(int)G.elevationOpen);
    fprintf(f,"angleOffset"); for(double o:c.angleOffset) fprintf(f," %.9f",o); fprintf(f,"\n");
    fprintf(f,"ground"); for(int i=0;i<256;i++) if(film.groundSet[i]) fprintf(f," %d",i); fprintf(f,"\nnote %s\n",G.note.c_str());
    fclose(f);
    printf("%d: %dx%d x%d turntable %d iou %.3f elev %.1f note %s\n",id,film.width,film.height,film.frames,(int)G.turntable,G.meanIoU,c.elevation*57.2958,G.note.c_str());
    fflush(stdout);
  }
}
