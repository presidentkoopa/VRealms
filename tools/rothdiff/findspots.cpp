// findspots.cpp -- choose camera spots from the MAP DATA, not by eye.
//
// The ten spots this rig used until now were picked by hand before any of it
// ran, and they turned out not to be chosen to prove anything: the spot named
// for standing on a carpet is 20% floor, and the one named for looking up
// cannot look up. A comparison is only worth the spots it is taken from.
//
// So this asks the map which sectors actually carry each property under test,
// and places a level camera in them. It reads the player's own .RAW and .DAS
// through the same reader the loader uses, launches nothing, and writes only
// the spot list.
//
//   1  big plain floor, s=1, no shift, no mirror        baseline
//   2  floor with a non-zero shift                      the shift signs
//   3  floor with a mirror bit, one spot per axis       the mirror bits
//   4  ceiling, s=1                                     the ceiling signs
//   5  256x256 opaque flat                              the scale exception
//   6  two adjacent sectors, same texture, diff shift   the seam
//   7  mid-platform top                                 the platform path
//
// Spots 8-10 are walls and are deliberately not here yet: flats go to ~100%
// first.
//
// Build:
//   cl /nologo /std:c++17 /O1 /EHsc /I ..\..\src\roth /Fe:findspots.exe
//      findspots.cpp ..\..\src\roth\roth_raw.cpp ..\..\src\roth\roth_install.cpp
//      ..\..\src\roth\roth_das.cpp ..\..\src\roth\roth_palette.cpp
//      ..\..\src\roth\roth_surface.cpp

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"
#include "roth_surface.h"

#include <stdio.h>
#include <math.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}

// ROTH's angle unit: 512 per turn, 0 along +Y and 128 along +X. Given a
// direction, the angle that looks along it.
static int AngleTowards(double dx, double dy)
{
	double a = atan2(dx, dy) / (2.0 * 3.14159265358979323846) * 512.0;
	int ia = (int)lround(a) % 512;
	if (ia < 0) ia += 512;
	return ia;
}

struct SectorGeom
{
	double cx = 0, cy = 0;     // centroid
	double area = 0;           // absolute polygon area
	double span = 0;           // longest vertex-to-centroid reach
	bool ok = false;
};

// A sector's outline, from the vertices its own faces name. Realms stores a
// face per edge, so walking the sector's face range gives the ring.
static SectorGeom Outline(const roth::Map &m, const roth::Sector &s)
{
	SectorGeom g;
	if (s.firstFaceIndex < 0 || s.faceCount < 3) return g;

	std::vector<std::pair<double, double>> pts;
	for (int i = 0; i < (int)s.faceCount; i++)
	{
		const int fi = s.firstFaceIndex + i;
		if (fi < 0 || fi >= (int)m.faces.size()) return g;
		const roth::Face &f = m.faces[fi];
		if (f.vertex1 < 0 || f.vertex1 >= (int)m.vertices.size()) return g;
		pts.push_back({ (double)m.vertices[f.vertex1].x, (double)m.vertices[f.vertex1].y });
	}
	if (pts.size() < 3) return g;

	double a2 = 0, cx = 0, cy = 0;
	for (size_t i = 0; i < pts.size(); i++)
	{
		const auto &p = pts[i];
		const auto &q = pts[(i + 1) % pts.size()];
		const double cross = p.first * q.second - q.first * p.second;
		a2 += cross;
		cx += (p.first + q.first) * cross;
		cy += (p.second + q.second) * cross;
	}
	if (fabs(a2) < 1e-6)
	{
		// Degenerate ring: fall back to the vertex average so the sector is
		// still usable rather than silently dropped.
		for (const auto &p : pts) { cx += p.first; cy += p.second; }
		g.cx = cx / (double)pts.size();
		g.cy = cy / (double)pts.size();
		g.area = 0;
	}
	else
	{
		g.cx = cx / (3.0 * a2);
		g.cy = cy / (3.0 * a2);
		g.area = fabs(a2) * 0.5;
	}
	for (const auto &p : pts)
	{
		const double d = hypot(p.first - g.cx, p.second - g.cy);
		if (d > g.span) g.span = d;
	}
	g.ok = true;
	return g;
}

struct Spot
{
	std::string name;
	int x = 0, y = 0, ang = 0;
	std::string why;
};

int main(int argc, char **argv)
{
	const char *mapName = (argc > 2) ? argv[2] : "STUDY1";
	if (argc < 2) { printf("usage: findspots <ROTH folder> [MAP]\n"); return 2; }
	if (!roth::TheInstall().Open(argv[1])) { printf("install: bad\n"); return 2; }

	std::vector<uint8_t> d = ReadFile(roth::TheInstall().MapFile(mapName));
	if (d.empty()) { printf("no map %s\n", mapName); return 2; }
	roth::Map m = roth::ParseRaw(d.data(), d.size());
	if (!m.ok()) { printf("parse: %s\n", m.error.c_str()); return 2; }

	std::vector<uint8_t> pd = ReadFile(roth::TheInstall().PackFile(
		roth::TheInstall().PackFor(mapName).c_str()));
	roth::Pack pack;
	if (pd.empty() || !pack.Load(pd.data(), pd.size()))
	{ printf("no pack for %s\n", mapName); return 2; }
	const uint16_t key = pack.SkyMarkerIndex();

	// Image shape per texture index, read once.
	std::map<int, std::pair<std::pair<int,int>, bool>> shape;
	auto shapeOf = [&](int idx) {
		auto it = shape.find(idx);
		if (it != shape.end()) return it->second;
		roth::Image img = pack.ReadImage(idx, false);
		std::pair<std::pair<int,int>, bool> s =
			{ { img.ok() ? img.width : 0, img.ok() ? img.height : 0 },
			  img.ok() && (img.imageType & roth::IT_TRANSLUCENT) != 0 };
		shape[idx] = s;
		return s;
	};

	std::vector<SectorGeom> geom(m.sectors.size());
	for (size_t i = 0; i < m.sectors.size(); i++) geom[i] = Outline(m, m.sectors[i]);

	auto drawn = [&](int texWord) {
		return roth::ClassifySurface((uint16_t)texWord, key).kind
			== roth::SurfaceKind::Textured;
	};

	std::vector<Spot> spots;

	// The camera stands at the sector's centroid, level, facing the sector's
	// longest reach so the most of that sector is in frame. Level is a
	// deliberate choice: 20-56% of a frame being flat is enough to score, and
	// the original's look up/down is a shear rather than a rotation, so pitch
	// would mean changing the capture format on both sides.
	auto placeInSector = [&](int si, const char *name, const std::string &why) {
		const SectorGeom &g = geom[si];
		if (!g.ok) return false;
		Spot s;
		s.name = name;
		s.x = (int)lround(g.cx);
		s.y = (int)lround(g.cy);
		s.ang = 0;
		s.why = why;
		spots.push_back(s);
		return true;
	};

	// --- 1..5: one sector each, biggest first so the flat fills the frame ---
	struct Cand { int si; double area; };
	auto pick = [&](const char *name, const char *why,
	                bool (*test)(const roth::Sector &, int, int, bool, bool),
	                int wantScale, bool wantMirrorX, bool wantMirrorY,
	                bool ceilingSide, bool want256) {
		(void)test;
		std::vector<Cand> cands;
		for (size_t i = 0; i < m.sectors.size(); i++)
		{
			const roth::Sector &rs = m.sectors[i];
			if (!geom[i].ok || geom[i].area < 4096.0) continue;

			const int tex = ceilingSide ? rs.ceilingTexture : rs.floorTexture;
			if (!drawn(tex)) continue;

			const int sc = ceilingSide ? rs.CeilingScaleShift() : rs.FloorScaleShift();
			const uint8_t flip = (uint8_t)(rs.flags2 >> 8);
			const bool mx = ceilingSide ? (flip & 0x04) != 0 : (flip & 0x01) != 0;
			const bool my = ceilingSide ? (flip & 0x08) != 0 : (flip & 0x02) != 0;
			const int shx = ceilingSide ? rs.ceilShiftX : rs.floorShiftX;
			const int shy = ceilingSide ? rs.ceilShiftY : rs.floorShiftY;

			auto sh = shapeOf(tex);
			const bool is256 = (sh.first.first == 256 && sh.first.second == 256 && !sh.second);

			if (wantScale >= 0 && sc != wantScale) continue;
			if (want256 != is256) continue;
			if (mx != wantMirrorX || my != wantMirrorY) continue;

			// name-specific extra conditions
			const std::string n(name);
			if (n == "flat_plain"   && (shx != 0 || shy != 0)) continue;
			if (n == "flat_shifted" && (shx == 0 && shy == 0)) continue;

			cands.push_back({ (int)i, geom[i].area });
		}
		if (cands.empty()) { printf("  (none) %-16s %s\n", name, why); return; }
		std::sort(cands.begin(), cands.end(),
			[](const Cand &a, const Cand &b) { return a.area > b.area; });
		placeInSector(cands[0].si, name,
			std::string(why) + "  sector " + std::to_string(cands[0].si)
			+ ", area " + std::to_string((long long)cands[0].area));
	};

	printf("map %s -- %d sectors, %d faces\n\n",
		mapName, (int)m.sectors.size(), (int)m.faces.size());

	pick("flat_plain",   "1 baseline floor, s=1, no shift, no mirror",
	     nullptr, 1, false, false, false, false);
	pick("flat_shifted", "2 floor with a non-zero shift",
	     nullptr, 1, false, false, false, false);
	pick("flat_mirx",    "3a floor mirrored in X",
	     nullptr, -1, true, false, false, false);
	pick("flat_miry",    "3b floor mirrored in Y",
	     nullptr, -1, false, true, false, false);
	pick("ceil_plain",   "4 ceiling, s=1",
	     nullptr, 1, false, false, true, false);
	pick("flat_256",     "5 256x256 opaque flat",
	     nullptr, -1, false, false, false, true);

	// --- 6: a seam. Two sectors sharing a face, same floor texture, different
	// shift. The camera stands in one and looks across the shared edge. ---
	{
		int bestA = -1, bestB = -1; double bestArea = 0; int bestFace = -1;
		for (size_t fi = 0; fi < m.faces.size(); fi++)
		{
			const roth::Face &f = m.faces[fi];
			if (f.sister < 0 || f.sector < 0) continue;
			if (f.sister >= (int)m.faces.size()) continue;
			const int sb = m.faces[f.sister].sector;
			if (sb < 0 || sb == f.sector) continue;
			if (f.sector >= (int)m.sectors.size() || sb >= (int)m.sectors.size()) continue;

			const roth::Sector &A = m.sectors[f.sector];
			const roth::Sector &B = m.sectors[sb];
			if (A.floorTexture != B.floorTexture || !drawn(A.floorTexture)) continue;
			if (A.floorShiftX == B.floorShiftX && A.floorShiftY == B.floorShiftY) continue;
			if (!geom[f.sector].ok || !geom[sb].ok) continue;

			const double a = geom[f.sector].area + geom[sb].area;
			if (a > bestArea) { bestArea = a; bestA = f.sector; bestB = sb; bestFace = (int)fi; }
		}
		if (bestA >= 0)
		{
			// Stand in A, look at the shared edge's midpoint, so the seam runs
			// across the middle of the picture.
			const roth::Face &f = m.faces[bestFace];
			const double mx = 0.5 * (m.vertices[f.vertex1].x + m.vertices[f.vertex2].x);
			const double my = 0.5 * (m.vertices[f.vertex1].y + m.vertices[f.vertex2].y);
			Spot s;
			s.name = "flat_seam";
			s.x = (int)lround(geom[bestA].cx);
			s.y = (int)lround(geom[bestA].cy);
			s.ang = AngleTowards(mx - geom[bestA].cx, my - geom[bestA].cy);
			s.why = "6 seam: sectors " + std::to_string(bestA) + " and "
			      + std::to_string(bestB) + ", same texture, different shift";
			spots.push_back(s);
		}
		else printf("  (none) %-16s %s\n", "flat_seam", "6 seam");
	}

	// --- 7: a mid-platform top (a rug, a table top). ---
	{
		int best = -1; double bestArea = 0;
		for (size_t i = 0; i < m.sectors.size(); i++)
		{
			if (m.sectors[i].platformIndex < 0) continue;
			if (!geom[i].ok) continue;
			const roth::MidPlatform &p = m.platforms[m.sectors[i].platformIndex];
			if (!drawn(p.topTexture)) continue;
			if (geom[i].area > bestArea) { bestArea = geom[i].area; best = (int)i; }
		}
		if (best >= 0)
			placeInSector(best, "midplat_top",
				"7 mid-platform top, sector " + std::to_string(best));
		else printf("  (none) %-16s %s\n", "midplat_top", "7 mid-platform");
	}

	// --- output ---
	printf("\n%-16s %8s %8s %6s   %s\n", "spot", "x", "y", "angle", "why");
	for (const Spot &s : spots)
		printf("%-16s %8d %8d %6d   %s\n",
			s.name.c_str(), s.x, s.y, s.ang, s.why.c_str());

	printf("\n# paste into rothdiff.py POSES\n    \"%s\": [\n", mapName);
	for (const Spot &s : spots)
		printf("        (\"%s\",%*s%d, %d, %d),\n", s.name.c_str(),
			(int)(18 - s.name.size()), "", s.x, s.y, s.ang);
	printf("    ],\n");

	printf("\n%d spot(s)\n", (int)spots.size());
	return spots.empty() ? 1 : 0;
}
