// doorgeom.cpp -- the shape of a door leaf, measured instead of assumed.
//
// WHY THIS EXISTS
//
// rothmap.cpp builds a swinging door leaf as a four-sided prism and skins only
// TWO of the four sides, on the stated grounds that j == 1 and j == 3 are the
// "broad faces" and j == 0 and j == 2 are "thickness edges" that ROTH.C never
// sources a texture from. The ROTH.C half of that is read out of
// setup_door_swing_geometry (doors.c:553-559) and is solid. The GEOMETRY half --
// that c0 and c2 really are short edges, so leaving them blank costs nothing --
// has never been measured. If a Realms doorway is roughly square in plan there
// are no thin edges at all, and the loader is leaving two full-size faces of
// every door in the game untextured.
//
// The owner reports the open door's visible surface "looks bad", which is what
// either fault would look like. This tells them apart before anything is
// changed.
//
// WHAT IT PRINTS
//
//   1. The four edge lengths of every door leaf, in the loader's own j order
//      (cyclic from the hinge), so "which j is long" is a number.
//   2. Whether the two longest are {1,3} -- the loader's assumption -- or some
//      other pair, per door and as a total.
//   3. For each j, whether a texture is even available: the face's own mid
//      texture and its sister's. A blank side that HAS art in the file is a
//      visible hole; one that has none never had a picture to draw.
//   4. The aspect ratio long:short, so "thickness" is a measured claim.
//
// It reads the player's own .RAW files through the same parser the loader uses,
// and writes nothing.
//
// Build:
//   cl /nologo /EHsc /std:c++17 /O2 /I ..\..\src\roth /Fe:doorgeom.exe \
//       /Fo:obj_dg\ doorgeom.cpp ../../src/roth/roth_raw.cpp \
//       ../../src/roth/roth_install.cpp

#include "roth_raw.h"
#include "roth_install.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}

// Mirrors rothmap.cpp's leafBuildable so this measures the SAME population the
// loader actually builds. Kept deliberately verbatim rather than shared: this
// tool must not be able to change loader behaviour. If leafBuildable changes,
// this drifts -- the same hazard opcodecensus.cpp carries and documents.
static bool LeafBuildable(const roth::Map &m, const roth::Sector &rs)
{
	if (!rs.IsDoorCapable() || rs.faceCount != 4 || rs.hingeFace < 0) return false;
	if (rs.firstFaceIndex < 0 || rs.firstFaceIndex + 4 > (int)m.faces.size()) return false;
	const int hingeRel = rs.hingeFace - rs.firstFaceIndex;
	if (hingeRel < 0 || hingeRel > 3) return false;
	// The ring must close: v2(c_k) == v1(c_(k+1)). The loader relies on this to
	// treat its corner array as the sector's corner loop.
	for (int k = 0; k < 4; k++)
	{
		const roth::Face &a = m.faces[rs.firstFaceIndex + k];
		const roth::Face &b = m.faces[rs.firstFaceIndex + ((k + 1) & 3)];
		if (a.vertex2 < 0 || b.vertex1 < 0) return false;
		if (a.vertex2 != b.vertex1) return false;
	}
	return true;
}

static double EdgeLen(const roth::Map &m, const roth::Face &f)
{
	if (f.vertex1 < 0 || f.vertex2 < 0) return -1.;
	if ((size_t)f.vertex1 >= m.vertices.size() || (size_t)f.vertex2 >= m.vertices.size()) return -1.;
	const double dx = double(m.vertices[f.vertex2].x) - double(m.vertices[f.vertex1].x);
	const double dy = double(m.vertices[f.vertex2].y) - double(m.vertices[f.vertex1].y);
	return std::sqrt(dx * dx + dy * dy);
}

// The mid texture the loader would reach for on side j: the SISTER's when there
// is one, else the face's own. Returns 0 when there is no picture to draw.
static uint16_t SkinTexture(const roth::Map &m, const roth::Face &f, bool *usedSister)
{
	int sk = -1;
	*usedSister = false;
	if (f.sister >= 0 && (size_t)f.sister < m.faces.size()) { sk = f.sister; *usedSister = true; }
	else sk = (int)(&f - m.faces.data());
	const roth::Face &sf = m.faces[sk];
	if (sf.textureMap < 0 || (size_t)sf.textureMap >= m.textureMaps.size()) return 0;
	return m.textureMaps[sf.textureMap].midTexture;
}

// -cam MAP: emit rothdiff_shot lines aimed square at each leaf's broad face c1,
// so a door can be photographed without hand-hunting a camera. The map vertex
// coordinates ARE the Doom world coordinates -- the loader applies no offset to
// the main map (rothmap.cpp's offX/offY exist only to translate a leaf into its
// void cell) -- and PlaceCamera takes world x/y directly.
static int EmitCameras(const char *mapName, int dist)
{
	std::vector<uint8_t> d = ReadFile(roth::TheInstall().MapFile(mapName));
	if (d.empty()) { printf("// %s: no such map\n", mapName); return 2; }
	roth::Map m = roth::ParseRaw(d.data(), d.size());
	if (!m.error.empty()) { printf("// %s: %s\n", mapName, m.error.c_str()); return 2; }

	int tag = 0;
	for (size_t si = 0; si < m.sectors.size(); si++)
	{
		const roth::Sector &rs = m.sectors[si];
		if (!LeafBuildable(m, rs)) continue;
		tag++;                                  // leaves are tagged 1..N in load order

		const int f0 = rs.firstFaceIndex;
		const int hingeRel = rs.hingeFace - f0;
		const roth::Face &c1 = m.faces[f0 + ((hingeRel + 1) & 3)];
		const double x1 = double(m.vertices[c1.vertex1].x) + m.wrapShiftX;
		const double y1 = double(m.vertices[c1.vertex1].y) + m.wrapShiftY;
		const double x2 = double(m.vertices[c1.vertex2].x) + m.wrapShiftX;
		const double y2 = double(m.vertices[c1.vertex2].y) + m.wrapShiftY;
		const double dx = x2 - x1, dy = y2 - y1;
		const double len = std::sqrt(dx * dx + dy * dy);
		if (len <= 0.) continue;
		const double mx = (x1 + x2) * .5, my = (y1 + y2) * .5;

		// A face is wound with its OWNING sector on the right, and the owner here
		// is the doorway. So the ROOM is on the left: (-dy, dx). Stand there and
		// look back along (dy, -dx).
		const double camX = mx + (-dy / len) * double(dist);
		const double camY = my + ( dx / len) * double(dist);
		const double faceX = dy / len, faceY = -dx / len;

		// ROTH angle: 512 per turn, 0 along +Y, 128 at +X (roth_diff.cpp:83-88),
		// so a512 = (90 - doomDegrees) * 512/360.
		const double doomDeg = std::atan2(faceY, faceX) * 180. / 3.14159265358979323846;
		int a512 = (int)std::lround((90. - doomDeg) * (512. / 360.));
		a512 = ((a512 % 512) + 512) % 512;

		printf("rothdiff_shot %d %d %d <OUT>/door%02d.png   // sector %zu cid %04x\n",
		       (int)std::lround(camX), (int)std::lround(camY), a512, tag, si, rs.commandID);
	}
	printf("// %d leaves in %s, camera %d units out from broad face c1\n", tag, mapName, dist);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: doorgeom <ROTH folder> [-v | -cam MAP [dist]]\n"); return 2; }
	const bool verbose = (argc > 2 && std::string(argv[2]) == "-v");
	if (!roth::TheInstall().Open(argv[1])) { printf("install: bad\n"); return 2; }

	if (argc > 3 && std::string(argv[2]) == "-cam")
		return EmitCameras(argv[3], argc > 4 ? atoi(argv[4]) : 220);

	long long leaves = 0;
	long long longPair13 = 0, longPair02 = 0, longPairOther = 0, squarish = 0;
	long long skinnedSideHasArt[4] = {0, 0, 0, 0};
	long long sideBlankInFile[4] = {0, 0, 0, 0};
	long long sideHasSister[4] = {0, 0, 0, 0};
	double sumLen[4] = {0., 0., 0., 0.};
	double minLen[4] = {1e9, 1e9, 1e9, 1e9}, maxLenA[4] = {-1., -1., -1., -1.};
	std::vector<double> aspects;
	// How much texture would be lost by blanking j == 0 and j == 2: total wall
	// area on those sides that HAS a picture in the file.
	double blankedAreaWithArt = 0., paintedArea = 0.;
	std::map<std::string, int> perMapLeaves;
	// THE ONE OPEN QUESTION, reduced to a count. ROTH.C sources all FOUR leaf
	// surfaces from c1 and c3 only (doors.c:553-559), so a thickness quad must
	// wear one of those two pictures -- but which surface lands on which quad is
	// the point three readings disagreed on. If c1 and c3 carry the SAME texture,
	// the disagreement cannot change a pixel and the question is moot.
	long long skin13Same = 0, skin13Differ = 0;
	// And the alternative reading: that a thickness edge wears its OWN stored
	// texture (the doorjamb's). Counted so the two candidates can be compared
	// rather than argued.
	long long edgeOwnMatchesBroad = 0, edgeOwnDiffers = 0;

	for (const std::string &name : roth::TheInstall().MapNames())
	{
		std::vector<uint8_t> d = ReadFile(roth::TheInstall().MapFile(name.c_str()));
		if (d.empty()) continue;
		roth::Map m = roth::ParseRaw(d.data(), d.size());
		if (!m.error.empty()) { printf("%-10s parse: %s\n", name.c_str(), m.error.c_str()); continue; }

		for (size_t si = 0; si < m.sectors.size(); si++)
		{
			const roth::Sector &rs = m.sectors[si];
			if (!LeafBuildable(m, rs)) continue;
			leaves++;
			perMapLeaves[name]++;

			const int f0 = rs.firstFaceIndex;
			const int hingeRel = rs.hingeFace - f0;
			const double height = double(rs.ceilingHeight) - double(rs.floorHeight);

			double L[4];
			uint16_t T[4];
			bool sis[4];
			bool bad = false;
			for (int j = 0; j < 4; j++)
			{
				const roth::Face &f = m.faces[f0 + ((hingeRel + j) & 3)];
				L[j] = EdgeLen(m, f);
				if (L[j] < 0.) { bad = true; break; }
				T[j] = SkinTexture(m, f, &sis[j]);
			}
			if (bad) continue;

			for (int j = 0; j < 4; j++)
			{
				sumLen[j] += L[j];
				minLen[j] = std::min(minLen[j], L[j]);
				maxLenA[j] = std::max(maxLenA[j], L[j]);
				if (sis[j]) sideHasSister[j]++;
				if (T[j] != 0) skinnedSideHasArt[j]++; else sideBlankInFile[j]++;
			}

			if (T[1] == T[3]) skin13Same++; else skin13Differ++;
			for (int j = 0; j <= 2; j += 2)
			{
				// The edge's own stored mid texture, NOT the sister-preferring
				// choice above -- that is the competing candidate.
				const roth::Face &ef = m.faces[f0 + ((hingeRel + j) & 3)];
				uint16_t own = 0;
				if (ef.textureMap >= 0 && (size_t)ef.textureMap < m.textureMaps.size())
					own = m.textureMaps[ef.textureMap].midTexture;
				if (own != 0 && (own == T[1] || own == T[3])) edgeOwnMatchesBroad++;
				else edgeOwnDiffers++;
			}

			// Which pair is long? Rank the four and see which two indices top it.
			int order[4] = {0, 1, 2, 3};
			std::sort(order, order + 4, [&](int a, int b) { return L[a] > L[b]; });
			const int hiA = std::min(order[0], order[1]), hiB = std::max(order[0], order[1]);
			const double longAvg = (L[order[0]] + L[order[1]]) * .5;
			const double shortAvg = (L[order[2]] + L[order[3]]) * .5;
			const double aspect = shortAvg > 0. ? longAvg / shortAvg : 0.;
			aspects.push_back(aspect);

			// "Squarish" = the short pair is more than 60% of the long pair, i.e.
			// there is no meaningful thickness direction at all.
			if (aspect < 1.0 / 0.6) squarish++;

			if (hiA == 1 && hiB == 3) longPair13++;
			else if (hiA == 0 && hiB == 2) longPair02++;
			else longPairOther++;

			// Area accounting on the loader's own choice.
			for (int j = 0; j < 4; j++)
			{
				const double area = L[j] * (height > 0. ? height : 0.);
				if (j == 1 || j == 3) paintedArea += area;
				else if (T[j] != 0) blankedAreaWithArt += area;
			}

			if (verbose)
			{
				printf("%-10s sec %3zu cid %04x hingeRel %d  L = %7.1f %7.1f %7.1f %7.1f"
				       "  long {%d,%d} asp %5.2f  tex %04x %04x %04x %04x  h %5.1f\n",
				       name.c_str(), si, rs.commandID, hingeRel,
				       L[0], L[1], L[2], L[3], hiA, hiB, aspect,
				       T[0], T[1], T[2], T[3], height);
			}
		}
	}

	printf("\n================ door leaf geometry, measured ================\n");
	printf("leaves the loader builds            %lld   (across %zu maps)\n",
	       leaves, perMapLeaves.size());
	if (leaves == 0) { printf("nothing to measure\n"); return 1; }

	printf("\nedge length by j (cyclic from the hinge)\n");
	for (int j = 0; j < 4; j++)
	{
		printf("  j == %d%-18s mean %7.1f   min %7.1f   max %7.1f\n", j,
		       (j == 1 || j == 3) ? "  (loader SKINS)" : "  (loader blanks)",
		       sumLen[j] / double(leaves), minLen[j], maxLenA[j]);
	}

	printf("\nwhich pair is the LONG pair\n");
	printf("  {1,3}  -- the loader's assumption   %lld  (%.1f%%)\n",
	       longPair13, 100. * double(longPair13) / double(leaves));
	printf("  {0,2}  -- exactly inverted          %lld  (%.1f%%)\n",
	       longPair02, 100. * double(longPair02) / double(leaves));
	printf("  other  -- not an opposite pair      %lld  (%.1f%%)\n",
	       longPairOther, 100. * double(longPairOther) / double(leaves));

	std::sort(aspects.begin(), aspects.end());
	printf("\naspect ratio long:short  (1.0 == perfectly square in plan)\n");
	printf("  min %5.2f   p25 %5.2f   median %5.2f   p75 %5.2f   max %5.2f\n",
	       aspects.front(), aspects[aspects.size() / 4], aspects[aspects.size() / 2],
	       aspects[aspects.size() * 3 / 4], aspects.back());
	printf("  leaves with NO thickness direction (short > 60%% of long)  %lld  (%.1f%%)\n",
	       squarish, 100. * double(squarish) / double(leaves));

	printf("\nis there art for each side in the file at all\n");
	for (int j = 0; j < 4; j++)
	{
		printf("  j == %d%-18s has mid texture %5lld   blank %5lld   has sister %5lld\n", j,
		       (j == 1 || j == 3) ? "  (loader SKINS)" : "  (loader blanks)",
		       skinnedSideHasArt[j], sideBlankInFile[j], sideHasSister[j]);
	}

	printf("\narea the loader paints vs area it blanks THAT HAS ART\n");
	printf("  painted (j 1,3)                     %12.0f sq units\n", paintedArea);
	printf("  blanked but textured in file (0,2)  %12.0f sq units   (%.1f%% of painted)\n",
	       blankedAreaWithArt,
	       paintedArea > 0. ? 100. * blankedAreaWithArt / paintedArea : 0.);

	printf("\nthe open question: does it matter which broad skin a thickness quad wears\n");
	printf("  skin(c1) == skin(c3)   -- choice is MOOT    %lld  (%.1f%%)\n",
	       skin13Same, 100. * double(skin13Same) / double(leaves));
	printf("  skin(c1) != skin(c3)   -- choice is visible  %lld  (%.1f%%)\n",
	       skin13Differ, 100. * double(skin13Differ) / double(leaves));
	printf("\ncompeting candidate: the thickness edge wears its OWN stored texture\n");
	printf("  own texture equals a broad skin      %lld  / %lld edges\n",
	       edgeOwnMatchesBroad, leaves * 2);
	printf("  own texture is something else        %lld  / %lld edges\n",
	       edgeOwnDiffers, leaves * 2);

	printf("\nleaves per map\n");
	for (const auto &kv : perMapLeaves) printf("  %-10s %3d\n", kv.first.c_str(), kv.second);
	return 0;
}
