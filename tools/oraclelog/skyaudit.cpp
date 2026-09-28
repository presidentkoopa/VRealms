// skyaudit.cpp -- the sky/colour-key data check.
//
// ROTH_SURFACES_FIX.md 3.6 settles from code that the DAS header word at +0x22
// is the COLOUR KEY, and that the original draws NOTHING on a surface carrying
// it (R:9225/9245 for flats, R:8666/8687/8732 for walls). The sky is a 2D band
// drawn only above EDGE_MAP walls; it is never a ceiling texture.
//
// That contradicts nothing in our data, but it reinterprets all of it: the
// "6,208 sky flats" this loader counted are 6,208 surfaces that draw NOTHING.
// So the number has to be split by surface kind, and any key ceiling with no
// EDGE_MAP face anywhere around it has to be listed -- because that is a room
// where mapping the key to engine sky would show sky with nothing to justify it.
//
// Reads the retail files through the loader's own reader. Writes nothing.
#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"
#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <set>

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); o.resize((size_t)n);
	if (n>0 && fread(o.data(),1,(size_t)n,f)!=(size_t)n) o.clear();
	fclose(f); return o;
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: skyaudit <ROTH folder>\n"); return 1; }
	if (!roth::TheInstall().Open(argv[1])) { printf("install: bad\n"); return 1; }

	long long totFloor = 0, totCeil = 0, totWall = 0;
	long long totKeyCeilNoEdge = 0, totEdgeFaces = 0;
	int mapsWithKey = 0;

	printf("map,key,floors,ceilings,walls,edge_faces,key_ceil_no_edge_in_sector,key_ceil_no_edge_in_map\n");

	std::vector<std::string> orphanReport;

	for (const std::string &name : roth::TheInstall().MapNames())
	{
		std::vector<uint8_t> d = ReadFile(roth::TheInstall().MapFile(name.c_str()));
		if (d.empty()) continue;
		roth::Map m = roth::ParseRaw(d.data(), d.size());
		if (!m.ok()) continue;

		// the pack's colour key
		std::vector<uint8_t> pd = ReadFile(roth::TheInstall().PackFile(
				roth::TheInstall().PackFor(name.c_str()).c_str()));
		if (pd.empty()) continue;
		roth::Pack pack;
		if (!pack.Load(pd.data(), pd.size())) continue;
		const uint16_t key = pack.SkyMarkerIndex();

		// which sectors own an EDGE_MAP face
		std::set<int> sectorsWithEdge;
		long long edgeFaces = 0, walls = 0;
		for (const roth::Face &f : m.faces)
		{
			if (f.textureMap < 0 || f.textureMap >= (int)m.textureMaps.size()) continue;
			const roth::TextureMap &tm = m.textureMaps[(size_t)f.textureMap];
			if (tm.flags & roth::FF_EDGE_MAP) { edgeFaces++; if (f.sector >= 0) sectorsWithEdge.insert(f.sector); }
			if (tm.midTexture == key || tm.upperTexture == key || tm.lowerTexture == key) walls++;
		}

		long long floors = 0, ceils = 0, orphanSector = 0;
		for (size_t i = 0; i < m.sectors.size(); i++)
		{
			const roth::Sector &s = m.sectors[i];
			if (s.floorTexture == key) floors++;
			if (s.ceilingTexture == key)
			{
				ceils++;
				if (sectorsWithEdge.find((int)i) == sectorsWithEdge.end()) orphanSector++;
			}
		}

		const long long orphanMap = sectorsWithEdge.empty() ? ceils : 0;
		if (floors || ceils || walls)
		{
			mapsWithKey++;
			printf("%s,%u,%lld,%lld,%lld,%lld,%lld,%lld\n",
				name.c_str(), (unsigned)key, floors, ceils, walls,
				edgeFaces, orphanSector, orphanMap);
			if (orphanMap > 0)
			{
				char buf[160];
				snprintf(buf, sizeof buf,
					"  %-10s %lld key ceiling(s) and NOT ONE EDGE_MAP face in the whole map",
					name.c_str(), ceils);
				orphanReport.push_back(buf);
			}
		}
		totFloor += floors; totCeil += ceils; totWall += walls;
		totEdgeFaces += edgeFaces; totKeyCeilNoEdge += orphanSector;
	}

	printf("\n# TOTALS across %d maps carrying the key\n", mapsWithKey);
	printf("# key floors            %lld\n", totFloor);
	printf("# key ceilings          %lld\n", totCeil);
	printf("# key wall pieces       %lld\n", totWall);
	printf("# key flats (fl+ceil)   %lld\n", totFloor + totCeil);
	printf("# EDGE_MAP faces        %lld\n", totEdgeFaces);
	printf("# key ceilings whose OWN sector has no EDGE_MAP face: %lld\n", totKeyCeilNoEdge);
	printf("\n# Maps with key ceilings but NO EDGE_MAP face anywhere:\n");
	if (orphanReport.empty()) printf("#   (none)\n");
	for (const std::string &s : orphanReport) printf("#%s\n", s.c_str());
	return 0;
}
