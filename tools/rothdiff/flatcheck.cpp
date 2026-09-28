// flatcheck.cpp -- the offline flat comparison, over every sector in all 44 maps.
//
// For every floor and ceiling in the retail install, at a spread of world
// points, this asks two questions and compares the answers:
//
//   roth_surface::FlatTexel   what ROTH.C samples there
//   roth_surface::EngineTexel what GZDoom samples there, given the values the
//                             loader chose via FlatToEngine
//
// A surface passes only if every sample agrees. No rendering, no window, no
// engine: it reads the player's own .RAW and .DAS through the loader's reader
// and answers in seconds.
//
// What it does NOT cover, so a high number is not mistaken for more than it is:
// occlusion, projection, eye height, field of view, lighting, and walls. It
// covers the scale, the shift, the mirror and the two signs -- which is the
// class of defect the flats have.
//
// Build:
//   i686-w64-mingw32-g++ -std=c++17 -O1 -static -I ../../src/roth \
//       -o flatcheck.exe flatcheck.cpp \
//       ../../src/roth/roth_surface.cpp ../../src/roth/roth_raw.cpp \
//       ../../src/roth/roth_install.cpp ../../src/roth/roth_das.cpp \
//       ../../src/roth/roth_palette.cpp

#include "roth_surface.h"
#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <map>

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}

// A spread of world points, including negatives and far-from-origin values.
// Flats are world-anchored, so a sign error only shows up off the origin.
static const int32_t PTS[] = { 0, 1, 3, 64, 1024, 3840, -2, -65, -1025, -3841 };
static const int NPTS = (int)(sizeof PTS / sizeof PTS[0]);

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: flatcheck <ROTH folder>\n"); return 2; }
	if (!roth::TheInstall().Open(argv[1])) { printf("install: bad\n"); return 2; }

	long long surfaces = 0, surfacesOk = 0;
	long long samples = 0, samplesOk = 0;
	long long skippedNoArt = 0, drawsNothing = 0, opaque256 = 0, mirrored = 0;
	std::map<std::string, std::pair<long long, long long>> perMap;   // ok / total

	for (const std::string &name : roth::TheInstall().MapNames())
	{
		std::vector<uint8_t> d = ReadFile(roth::TheInstall().MapFile(name.c_str()));
		if (d.empty()) continue;
		roth::Map m = roth::ParseRaw(d.data(), d.size());
		if (!m.ok()) continue;

		std::vector<uint8_t> pd = ReadFile(roth::TheInstall().PackFile(
			roth::TheInstall().PackFor(name.c_str()).c_str()));
		if (pd.empty()) continue;
		roth::Pack pack;
		if (!pack.Load(pd.data(), pd.size())) continue;
		const uint16_t key = pack.SkyMarkerIndex();

		// Image shapes, read once per index rather than per surface.
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

		for (const roth::Sector &rs : m.sectors)
		{
			for (int plane = 0; plane < 2; plane++)
			{
				const bool isFloor = (plane == 0);
				const int index = isFloor ? rs.floorTexture : rs.ceilingTexture;

				roth::FlatSetup fs = {};
				fs.textureWord = (uint16_t)index;
				fs.scaleBits = (uint8_t)(isFloor ? rs.FloorScaleShift() : rs.CeilingScaleShift());
				fs.shiftX = isFloor ? rs.floorShiftX : rs.ceilShiftX;
				fs.shiftY = isFloor ? rs.floorShiftY : rs.ceilShiftY;

				const uint8_t flip = (uint8_t)(rs.flags2 >> 8);
				fs.mirrorX = isFloor ? (flip & 0x01) != 0 : (flip & 0x04) != 0;
				fs.mirrorY = isFloor ? (flip & 0x02) != 0 : (flip & 0x08) != 0;
				if (fs.mirrorX || fs.mirrorY) mirrored++;

				auto sh = shapeOf(index);
				fs.texW = (uint16_t)sh.first.first;
				fs.texH = (uint16_t)sh.first.second;
				fs.opaque256 = (sh.first.first == 256 && sh.first.second == 256 && !sh.second);
				if (fs.opaque256) opaque256++;

				// A surface that draws nothing has no texel to compare, and
				// counting it as a pass would inflate the number with the
				// surfaces the rule does not reach.
				const roth::SurfaceClass sc = roth::ClassifySurface(fs.textureWord, key);
				if (sc.kind != roth::SurfaceKind::Textured) { drawsNothing++; continue; }
				if (fs.texW == 0 || fs.texH == 0) { skippedNoArt++; continue; }

				const roth::FlatEngineSetup fe = roth::FlatToEngine(fs);

				surfaces++;
				bool allOk = true;
				for (int i = 0; i < NPTS; i++)
					for (int j = 0; j < NPTS; j++)
					{
						const roth::Texel want = roth::FlatTexel(fs, PTS[i], PTS[j], key);
						const roth::Texel got = roth::EngineTexel(fe, fs.texW, fs.texH, PTS[i], PTS[j]);
						samples++;
						if (want.col == got.col && want.row == got.row) samplesOk++;
						else allOk = false;
					}
				if (allOk) surfacesOk++;
				perMap[name].second++;
				if (allOk) perMap[name].first++;
			}
		}
	}

	printf("flat comparison -- ROTH.C rule vs the engine values the loader chooses\n");
	printf("  maps                 %d\n", (int)perMap.size());
	printf("  surfaces compared    %lld\n", surfaces);
	printf("  sample points        %lld\n", samples);
	printf("  drew nothing         %lld  (colour key or solid colour)\n", drawsNothing);
	printf("  no art               %lld\n", skippedNoArt);
	printf("  mirrored             %lld\n", mirrored);
	printf("  256x256 opaque       %lld\n", opaque256);

	int bad = 0;
	for (auto &kv : perMap)
		if (kv.second.first != kv.second.second)
		{
			if (bad < 10)
				printf("  MISMATCH %-10s %lld of %lld surfaces disagree\n",
					kv.first.c_str(),
					kv.second.second - kv.second.first, kv.second.second);
			bad++;
		}
	if (bad > 10) printf("  ... and %d more map(s)\n", bad - 10);

	const double sp = samples ? 100.0 * (double)samplesOk / (double)samples : 0.0;
	const double su = surfaces ? 100.0 * (double)surfacesOk / (double)surfaces : 0.0;
	printf("\n  MATCH  %.4f%% of sample points   %.4f%% of surfaces\n", sp, su);
	return (samplesOk == samples) ? 0 : 1;
}
