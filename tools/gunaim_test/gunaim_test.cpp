//
// gunaim_test -- ActorWorld::HandOriginSector (roth_actors.h), the check that
// lets the VR hand's shot start at the hand only when the original's map lets
// a shot from the player's own start reach it (docs/VR_CHANGES_LEDGER.md row
// 33). Engine-free: the port's map reader and actor rules, on the retail maps.
//
//   gunaim_test <DATA dir> [MAP ...]      (default: every map in DATA/M)
//
// For every face of every sector, a point 4 units inside its sector, beside
// the face's middle, and the point 4 units across it:
//   W  one-sided face (a wall): the walk must refuse (-1).
//   O  two-sided face, open at the crossing height on both sides: the walk
//      must arrive in the far face's sector.
//   C  two-sided face, closed at that height (a step taller than the point, a
//      closed door): the walk must refuse.
//   S  the same point (no move): its own sector.
//   R  every O case walked back the other way arrives in the first sector.
// Points the original's own test (InsideSector) does not put in the expected
// sector (thin or odd sectors, corners), and points within a unit of another
// face of their sector (on a boundary either way out is right), are skipped
// and counted.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
#include "roth_actors.h"
#include "roth_raw.h"
#include "roth_dbase100.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <dirent.h>
#include <strings.h>

using namespace roth;

static std::vector<uint8_t> Slurp(const std::string &path)
{
	std::vector<uint8_t> v;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return v;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n > 0) { v.resize((size_t)n); if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear(); }
	fclose(f);
	return v;
}

static std::string FindFile(const std::string &dir, const std::string &name)
{
	DIR *d = opendir(dir.c_str());
	if (!d) return dir + "/" + name;
	std::string out = dir + "/" + name;
	while (dirent *e = readdir(d)) if (!strcasecmp(e->d_name, name.c_str())) { out = dir + "/" + e->d_name; break; }
	closedir(d);
	return out;
}

struct Count { long w = 0, wbad = 0, o = 0, obad = 0, c = 0, cbad = 0, s = 0, sbad = 0, r = 0, rbad = 0, skipped = 0; };

static void RunMap(const std::string &data, const std::string &name, Count &total)
{
	std::vector<uint8_t> raw = Slurp(FindFile(data + "/M", name + ".RAW"));
	if (raw.empty()) { printf("%s: no map\n", name.c_str()); return; }
	Map map = ParseRaw(raw.data(), raw.size());
	if (!map.ok()) { printf("%s: %s\n", name.c_str(), map.error.c_str()); return; }
	ActorWorld w;
	w.Begin(&map, nullptr, nullptr, nullptr);
	Count c;
	int shown = 0;
	auto bad = [&](const char *kind, int s, int fi, int got, int want) {
		if (shown++ < 5) printf("  %s %s sector %d face %d: got %d, want %d\n", name.c_str(), kind, s, fi, got, want);
	};
	for (int s = 0; s < (int)map.sectors.size(); s++)
	{
		const Sector &sec = map.sectors[(size_t)s];
		if (sec.firstFaceIndex < 0 || sec.ceilingHeight - sec.floorHeight < 8) continue;
		for (int k = 0; k < sec.faceCount; k++)
		{
			const int fi = sec.firstFaceIndex + k;
			const Face &f = map.faces[(size_t)fi];
			if (f.vertex1 < 0 || f.vertex2 < 0) continue;
			const Vertex &a = map.vertices[(size_t)f.vertex1], &b = map.vertices[(size_t)f.vertex2];
			const double ex = b.x - a.x, ey = b.y - a.y, len = sqrt(ex * ex + ey * ey);
			if (len < 24) continue;
			const double mx = (a.x + b.x) * 0.5, my = (a.y + b.y) * 0.5;
			// The side of the face the sector lies on: try both normals.
			double nx = -ey / len, ny = ex / len;
			int16_t ix = (int16_t)lround(mx + nx * 4), iy = (int16_t)lround(my + ny * 4);
			if (!w.InsideSectorPublic(s, ix, iy)) { nx = -nx; ny = -ny; ix = (int16_t)lround(mx + nx * 4); iy = (int16_t)lround(my + ny * 4); }
			if (!w.InsideSectorPublic(s, ix, iy)) { c.skipped++; continue; }
			const int16_t ox = (int16_t)lround(mx - nx * 4), oy = (int16_t)lround(my - ny * 4);
			// A point lying on (within a unit of) another face of its sector is
			// on a boundary the walk may leave by either face: not a case.
			auto nearOther = [&](int sector, int skipFace, int16_t px, int16_t py) {
				const Sector &q = map.sectors[(size_t)sector];
				for (int j = 0; j < q.faceCount; j++)
				{
					const int fj = q.firstFaceIndex + j;
					if (fj == skipFace) continue;
					const Face &g = map.faces[(size_t)fj];
					if (g.vertex1 < 0 || g.vertex2 < 0) continue;
					const Vertex &p1 = map.vertices[(size_t)g.vertex1], &p2 = map.vertices[(size_t)g.vertex2];
					const double gx = p2.x - p1.x, gy = p2.y - p1.y, gl = gx * gx + gy * gy;
					double u = gl > 0 ? ((px - p1.x) * gx + (py - p1.y) * gy) / gl : 0.0;
					u = u < 0 ? 0 : u > 1 ? 1 : u;
					const double dx = p1.x + gx * u - px, dy = p1.y + gy * u - py;
					if (dx * dx + dy * dy < 1.0) return true;
				}
				return false;
			};
			if (nearOther(s, fi, ix, iy)) { c.skipped++; continue; }
			const int16_t z = (int16_t)(sec.floorHeight + (sec.ceilingHeight - sec.floorHeight) / 2);
			c.s++;
			const int same = w.HandOriginSector(s, ix, iy, z, ix, iy, z);
			if (same != s) { c.sbad++; bad("S", s, fi, same, s); }
			const int got = w.HandOriginSector(s, ix, iy, z, ox, oy, z);
			if (f.sister < 0)
			{
				c.w++;
				if (got != -1) { c.wbad++; bad("W", s, fi, got, -1); }
				continue;
			}
			const int ns = map.faces[(size_t)f.sister].sector;
			if (ns < 0 || !w.InsideSectorPublic(ns, ox, oy) || nearOther(ns, f.sister, ox, oy)) { c.skipped++; continue; }
			const Sector &o = map.sectors[(size_t)ns];
			const bool open = z >= o.floorHeight && z <= o.ceilingHeight && o.ceilingHeight > o.floorHeight;
			if (open)
			{
				c.o++;
				if (got != ns) { c.obad++; bad("O", s, fi, got, ns); }
				c.r++;
				const int back = w.HandOriginSector(ns, ox, oy, z, ix, iy, z);
				if (back != s) { c.rbad++; bad("R", s, fi, back, s); }
			}
			else
			{
				c.c++;
				if (got != -1) { c.cbad++; bad("C", s, fi, got, -1); }
			}
		}
	}
	printf("%-10s walls %ld (%ld wrong)  open %ld (%ld wrong)  closed %ld (%ld wrong)  same %ld (%ld wrong)  back %ld (%ld wrong)  skipped %ld\n",
		name.c_str(), c.w, c.wbad, c.o, c.obad, c.c, c.cbad, c.s, c.sbad, c.r, c.rbad, c.skipped);
	total.w += c.w; total.wbad += c.wbad; total.o += c.o; total.obad += c.obad; total.c += c.c; total.cbad += c.cbad;
	total.s += c.s; total.sbad += c.sbad; total.r += c.r; total.rbad += c.rbad; total.skipped += c.skipped;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: gunaim_test DATA [MAP ...]\n"); return 2; }
	const std::string data = argv[1];
	std::vector<std::string> maps;
	for (int i = 2; i < argc; i++) maps.push_back(argv[i]);
	if (maps.empty())
	{
		DIR *d = opendir((data + "/M").c_str());
		if (d) { while (dirent *e = readdir(d)) { std::string n = e->d_name; if (n.size() > 4 && !strcasecmp(n.c_str() + n.size() - 4, ".RAW")) maps.push_back(n.substr(0, n.size() - 4)); } closedir(d); }
	}
	Count t;
	for (const std::string &m : maps) RunMap(data, m, t);
	const long wrong = t.wbad + t.obad + t.cbad + t.sbad + t.rbad;
	printf("TOTAL %zu map(s): %ld cases, %ld wrong (walls %ld/%ld, open %ld/%ld, closed %ld/%ld, same %ld/%ld, back %ld/%ld), %ld skipped\n",
		maps.size(), t.w + t.o + t.c + t.s + t.r, wrong, t.wbad, t.w, t.obad, t.o, t.cbad, t.c, t.sbad, t.s, t.rbad, t.r, t.skipped);
	return wrong ? 1 : 0;
}
