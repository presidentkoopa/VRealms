#!/usr/bin/env python3
# Writes a standalone test of FSeamFilter::Hides to stdout.
#
# The function decides which lines a thing straddling a linked portal on a one-sided line must NOT
# collide with (src/playsim/p_maputl.cpp). It is pure geometry, so its body is lifted out of the
# engine source verbatim and run against hand-made cases -- the engine itself cannot be run here.
# build.sh compiles and runs the result.
import sys
src = open(sys.argv[1]).read()
a = src.index("bool FSeamFilter::Hides(const line_t *line, const FBoundingBox &bbox) const")
b = src.index("//===========================================================================", a)
body = src[a:b].replace("bool FSeamFilter::Hides(const line_t *line, const FBoundingBox &bbox) const",
                        "bool HiddenBehindSeam(const line_t *line) const")
print(r'''#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "r_defs.h"
#include "m_bbox.h"
void *M_Malloc(size_t n){return malloc(n);} void *M_Realloc(void*p,size_t n){return realloc(p,n);} void M_Free(void*p){free(p);}
struct Iter {
	enum { MAX_SEAMS = 4 };
	line_t *seams[MAX_SEAMS]; int numseams = 0; FBoundingBox bbox;
	''' + body + r'''
};
static vertex_t V[128]; static int nv=0; static sector_t SA, SB;
static line_t *mk(double x1,double y1,double x2,double y2,bool twosided=false){ line_t*l=new line_t(); memset((void*)l,0,sizeof(*l)); V[nv].set(x1,y1); V[nv+1].set(x2,y2); l->v1=&V[nv]; l->v2=&V[nv+1]; nv+=2; l->setDelta(x2-x1,y2-y1); l->frontsector=&SA; l->backsector=twosided?&SB:nullptr; return l; }
static int fails=0;
static void expect(const char*name,bool got,bool want){ printf("%-74s %s\n",name,got==want?"ok":"FAIL"); if(got!=want)fails++; }
int main(){
	// Portal P runs north along x=256 from (256,396) to (256,556); its FRONT (right of v1->v2) is x>256, its back x<256.
	Iter it; line_t*P=mk(256,396,256,556); it.seams[0]=P; it.numseams=1;
	// a thing of radius 16 at (262,470): its box reaches x=246, 10 units behind the portal
	it.bbox.setBox(262,470,16);
	expect("the portal line itself is never hidden", it.HiddenBehindSeam(P), false);
	expect("a wall 6 units behind, parallel, spanning the box", it.HiddenBehindSeam(mk(250,300,250,700)), true);
	expect("a wall of another storey exactly under the portal (faces away)", it.HiddenBehindSeam(mk(256,556,256,396)), true);
	expect("a two-sided line inside the hidden room, ending on the plane", it.HiddenBehindSeam(mk(256,470,200,470,true)), true);
	expect("a wall in FRONT of the portal", it.HiddenBehindSeam(mk(270,300,270,700)), false);
	expect("a wall crossing the portal plane (front to back)", it.HiddenBehindSeam(mk(300,470,200,470)), false);
	// near the portal's north end: the box spans y 546..566, past the end at 556, and x 252..272
	it.bbox.setBox(262,556,10);
	expect("collinear neighbour wall of the near room at the end (same facing)", it.HiddenBehindSeam(mk(256,556,256,700)), false);
	expect("a wall behind, but beyond the portal's end only", it.HiddenBehindSeam(mk(254,560,254,700)), false);
	expect("a wall behind spanning past the end (partly outside the strip)", it.HiddenBehindSeam(mk(254,300,254,700)), false);
	expect("near room's own wall running back from the end, facing outward", it.HiddenBehindSeam(mk(256,556,200,556)), false);
	expect("hidden room's side wall running back from the end, facing inward", it.HiddenBehindSeam(mk(200,556,256,556)), true);
	// two portals side by side in one opening
	line_t*P2=mk(256,556,256,716); it.seams[1]=P2; it.numseams=2;
	expect("two adjacent portals: a wall behind spanning both strips", it.HiddenBehindSeam(mk(254,300,254,800)), true);
	expect("two adjacent portals: two-sided line between them running back", it.HiddenBehindSeam(mk(256,556,200,556,true)), true);
	// a diagonal portal from (0,0) to (100,100): front is the right side (x>y), back is x<y
	Iter d; line_t*D=mk(0,0,100,100); d.seams[0]=D; d.numseams=1; d.bbox.setBox(52,48,12);
	expect("diagonal: wall parallel, 5 behind", d.HiddenBehindSeam(mk(0-3.5355,0+3.5355,100-3.5355,100+3.5355)), true);
	expect("diagonal: wall parallel, 5 in front", d.HiddenBehindSeam(mk(0+3.5355,0-3.5355,100+3.5355,100-3.5355)), false);
	expect("a line that misses the box altogether", d.HiddenBehindSeam(mk(500,500,600,600)), false);
	printf("%d failure(s)\n",fails); return fails!=0;
}''')
