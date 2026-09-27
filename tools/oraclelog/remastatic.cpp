// remastatic.cpp -- REMAROTH's half of the static comparison.
//
// Emits the SAME table oraclelog's static dump emits from the running original,
// so the two can be diffed line for line. Two independent implementations reading
// the same .RAW and agreeing field by field is what "the rig is proven" means;
// the first differing line names the field we read wrongly.
//
// Deliberately uses roth_raw -- the loader's own reader -- and not a fresh parse,
// because the point is to test what REMAROTH actually believes, not to test a
// second opinion written for the occasion.
#include "roth_raw.h"
#include "roth_install.h"
#include <stdio.h>
#include <string>
#include <vector>

static std::vector<uint8_t> ReadFile(const std::string &path)
{
	std::vector<uint8_t> out;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return out;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	out.resize((size_t)n);
	if (n > 0 && fread(out.data(), 1, (size_t)n, f) != (size_t)n) out.clear();
	fclose(f);
	return out;
}

int main(int argc, char **argv)
{
	if (argc < 3) { printf("usage: remastatic <ROTH folder> <MAP>\n"); return 1; }
	if (!roth::TheInstall().Open(argv[1])) { printf("install: bad\n"); return 1; }

	std::vector<uint8_t> data = ReadFile(roth::TheInstall().MapFile(argv[2]));
	if (data.empty()) { printf("no map %s\n", argv[2]); return 1; }
	roth::Map m = roth::ParseRaw(data.data(), data.size());
	if (!m.ok()) { printf("parse: %s\n", m.error.c_str()); return 1; }

	printf("# oracle static dump v1\n");

	// The view block. These are what the loader DERIVES and forces onto the
	// level, so they are the right things to compare against the original's
	// live globals -- not the raw metadata they come from.
	printf("view,center_x,%d\n", 320);      // 640x480 view, as the original set
	printf("view,center_y,%d\n", 240);
	printf("view,player_height,%d\n", (int)m.metadata.playerHeight * 2);
	printf("view,max_climb,%d\n", (int)m.metadata.maxClimb * 2 + 1);
	printf("view,min_fit,%d\n", (int)m.metadata.minFit * 2);

	printf("sec,idx,ceil_h,floor_h,ceil_tex,floor_tex,flags,"
	       "ceil_scale,floor_scale,light,texmap_ovr,faces,"
	       "ceil_shx,ceil_shy,floor_shx,floor_shy,cmd_id\n");
	for (size_t i = 0; i < m.sectors.size(); i++)
	{
		const roth::Sector &s = m.sectors[i];
		printf("sec,%u,%d,%d,%u,%u,%02x,%u,%u,%u,%d,%u,%u,%u,%u,%u,%u\n",
			(unsigned)i,
			(int)s.ceilingHeight, (int)s.floorHeight,
			(unsigned)s.ceilingTexture, (unsigned)s.floorTexture,
			(unsigned)s.flags,
			(unsigned)s.CeilingScaleShift(), (unsigned)s.FloorScaleShift(),
			(unsigned)s.light, (int)s.textureMapOverride,
			(unsigned)s.faceCount,
			(unsigned)s.ceilShiftX, (unsigned)s.ceilShiftY,
			(unsigned)s.floorShiftX, (unsigned)s.floorShiftY,
			(unsigned)s.commandID);
	}

	// Objects. The original walks a per-sector group index, which is exactly how
	// this reader stores them -- objects[i] belongs to sector i -- so the (group,
	// sub) pair lines up without any remapping. Groups with no objects are absent
	// on both sides.
	printf("obj,group,sub,x,y,z,rotation,tex_index,tex_source,"
	       "flags,light,rendertype,cmd_id\n");
	for (size_t g = 0; g < m.objects.size(); g++)
	{
		const std::vector<roth::Object> &list = m.objects[g];
		for (size_t s = 0; s < list.size(); s++)
		{
			const roth::Object &o = list[s];
			printf("obj,%u,%u,%d,%d,%d,%u,%u,%u,%02x,%u,%02x,%u\n",
				(unsigned)g, (unsigned)s,
				(int)o.x, (int)o.y, (int)o.z,
				(unsigned)o.rotation,
				(unsigned)o.textureIndex, (unsigned)o.textureSource,
				(unsigned)o.flags, (unsigned)o.light,
				(unsigned)o.renderType, (unsigned)o.commandID);
		}
	}
	return 0;
}
