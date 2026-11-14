//
// voxelize_test -- run the close-up voxelizer (src/roth/roth_voxelize) over
// every inventory item of an install and report, per film:
//   the items using it, the film's size and frames, the turntable estimate
//   (frames per turn, direction, elevation and how well it is pinned, axis
//   column, roll, perspective), the silhouette consistency (mean and worst
//   IoU of the model's projection against each frame's mask), the grid, the
//   voxel counts, the KVX size, the photometric error against the film, and
//   the item's world sprite size (its DAS picture in the shared pack).
//
// Pictures (only with --pictures DIR, which must be OUTSIDE the repository:
// they are made from the player's retail art): per film, a PPM with the film's
// frames at four angles over the model drawn with the film's camera, and the
// model from new angles. tools/voxelize_test/contact_sheet.py joins them.
//
//   voxelize_test <retail DATA dir> [--items 15,16,20] [--pictures /tmp/dir]
//                 [--voxel 1.0] [--mode front|median|mean] [--tol 0]
//                 [--kvx /tmp/dir]   (writes each model's KVX there, for the round trip)
//
// Writes the table to stdout. Nothing goes into the repository.
//
#include "../../src/roth/roth_voxelize.h"
#include "../../src/roth/roth_dbase100.h"
#include "../../src/roth/roth_das.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace roth;

static std::vector<uint8_t> ReadFile(const std::string &path)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return b;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b.resize((size_t)n);
	if (n > 0 && fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
	fclose(f);
	return b;
}

static std::string Name400(const std::vector<uint8_t> &t, uint32_t off)
{
	if (off == 0 || (size_t)off + 8 > t.size()) return "?";
	const unsigned len = (unsigned)(t[off + 4] | (t[off + 5] << 8));
	std::string s;
	for (unsigned i = 0; i < len && off + 8 + i < t.size() && i < 40; i++)
	{
		const uint8_t c = t[off + 8 + i];
		if (c == 0) break;
		s += (c >= 0x20 && c < 0x7f) ? (char)c : '_';
	}
	return s;
}

// Text for a C++ line comment: no backslash (it would continue the line).
static std::string Comment(std::string t)
{
	for (char &c : t) if (c == '\\') c = '/';
	return t;
}

static void WritePpm(const std::string &path, int w, int h, const std::vector<uint8_t> &rgb)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	fprintf(f, "P6 %d %d 255\n", w, h);
	fwrite(rgb.data(), 1, rgb.size(), f);
	fclose(f);
}

static void Blit(std::vector<uint8_t> &dst, int dw, int dh, int x0, int y0, const std::vector<uint8_t> &src, int sw, int sh)
{
	for (int y = 0; y < sh; y++)
		for (int x = 0; x < sw; x++)
		{
			const int X = x0 + x, Y = y0 + y;
			if (X < 0 || Y < 0 || X >= dw || Y >= dh) continue;
			memcpy(&dst[(Y * dw + X) * 3], &src[(y * sw + x) * 3], 3);
		}
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: voxelize_test <DATA dir> [--items a,b] [--pictures dir] [--voxel px] [--mode front|median|mean] [--tol n] [--kvx dir]\n");
		return 2;
	}
	const std::string dir = argv[1];
	std::set<int> only;
	std::string pictures, kvxDir, emitTable;
	bool useTable = false, estimateOnly = false, includeInfo = false;
	VoxCarveOptions carve;
	for (int i = 2; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--items" && i + 1 < argc)
		{
			std::string s = argv[++i];
			size_t p = 0;
			while (p < s.size()) { only.insert(atoi(s.c_str() + p)); p = s.find(',', p); if (p == std::string::npos) break; p++; }
		}
		else if (a == "--pictures" && i + 1 < argc) pictures = argv[++i];
		else if (a == "--kvx" && i + 1 < argc) kvxDir = argv[++i];
		else if (a == "--emit-table" && i + 1 < argc) emitTable = argv[++i];
		else if (a == "--use-table") useTable = true;
		else if (a == "--estimate-only") estimateOnly = true;
		else if (a == "--info") includeInfo = true;
		else if (a == "--no-photo-carve") carve.photoCarve = false;
		else if (a == "--voxel" && i + 1 < argc) carve.voxelPx = atof(argv[++i]);
		else if (a == "--tol" && i + 1 < argc) carve.missTolerance = atoi(argv[++i]);
		else if (a == "--mode" && i + 1 < argc)
		{
			const std::string m = argv[++i];
			carve.colourMode = m == "front" ? VoxColourMode::Front : m == "mean" ? VoxColourMode::Mean : VoxColourMode::Median;
		}
		else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
	}

	std::vector<uint8_t> d100 = ReadFile(dir + "/DBASE100.DAT"), d300 = ReadFile(dir + "/DBASE300.DAT"), d400 = ReadFile(dir + "/DBASE400.DAT");
	std::vector<uint8_t> ademo = ReadFile(dir + "/M/ADEMO.DAS");
	if (d100.empty() || d300.empty()) { fprintf(stderr, "cannot read DBASE100/300 in %s\n", dir.c_str()); return 1; }
	Dbase100 db;
	if (!db.Load(d100.data(), d100.size())) { fprintf(stderr, "DBASE100: %s\n", db.Error().c_str()); return 1; }
	Pack shared;
	const bool haveShared = !ademo.empty() && shared.Load(ademo.data(), ademo.size());

	// Films are shared between records: group the items by close-up offset.
	std::map<uint32_t, std::vector<int>> films;
	int withFilm = 0;
	for (int id = 1; id <= db.ItemCount(); id++)
	{
		const DbItem *it = db.Item(id);
		if (!it || !it->closeup) continue;
		withFilm++;
		if (!only.empty() && !only.count(id)) continue;
		films[it->closeup].push_back(id);
	}
	printf("# %d inventory records, %d with a close-up film, %zu distinct films in this run\n", db.ItemCount(), withFilm, films.size());
	printf("# voxel %.2f film px, colour %s, miss tolerance %d\n", carve.voxelPx,
		carve.colourMode == VoxColourMode::Front ? "front" : carve.colourMode == VoxColourMode::Mean ? "mean" : "median", carve.missTolerance);
	printf("# items|name|film WxH|frames|turn|period|dir|elev deg [lo..hi]|axis column,row|roll deg|persp D px|search IoU mean/min|IoU mean|IoU min|photo err|grid|voxel px|voxels|surface|kvx bytes|world sprite: drawn WxH texels x units/texel = world units -> world units per film px|runner-up|note\n");

	int nOk = 0, nNot = 0, nInfo = 0;
	std::vector<std::pair<VoxFittedFilm, std::string>> fitted;
	double sumIoU = 0;
	for (auto &f : films)
	{
		const std::vector<int> &ids = f.second;
		const DbItem *it = db.Item(ids[0]);
		std::string idList;
		for (int id : ids) idList += (idList.empty() ? "" : ",") + std::to_string(id);
		const std::string name = Name400(d400, it->name);

		// Information records (category 2: people, places, topics) have
		// close-up films too, but of portraits and scenes, not objects.
		if (it->Category() == DB_CATEGORY_INFO && !includeInfo)
		{
			nInfo++;
			printf("%s|%s|information record (category 2): a portrait or a place, not an object; skipped\n", idList.c_str(), name.c_str());
			continue;
		}
		const uint8_t *gdv = nullptr;
		size_t gsz = 0;
		if (!VoxCloseupBytes(d300.data(), d300.size(), f.first, gdv, gsz)) { printf("%s|%s|bad offset\n", idList.c_str(), name.c_str()); continue; }
		const auto t0 = std::chrono::steady_clock::now();
		VoxFilm film;
		std::string err;
		if (!VoxDecodeFilm(gdv, gsz, film, &err)) { printf("%s|%s|decode: %s\n", idList.c_str(), name.c_str(), err.c_str()); continue; }
		std::vector<uint8_t> masks;
		VoxBuildMasks(film, masks);
		const uint32_t crc = VoxFilmCrc(gdv, gsz);
		const VoxFittedFilm *fit = useTable ? VoxFindFitted(crc, (uint32_t)gsz) : nullptr;
		VoxGeometry G;
		if (fit)
		{
			G.turntable = true;
			G.camera = VoxCameraFromFitted(*fit);
			G.elevationOpen = fit->elevationOpen != 0;
			G.elevationLow = G.elevationHigh = G.camera.elevation;
			G.note = "camera from the fitted table; ";
		}
		else G = VoxEstimateTurntable(film, masks);
		if (G.turntable && !emitTable.empty()) fitted.push_back({ VoxFittedFromCamera(crc, (uint32_t)gsz, G.camera, G.elevationOpen), Comment(idList + " " + name) });
		const auto t1 = std::chrono::steady_clock::now();

		// The world sprite: the item's render id in the shared pack.
		std::string sprite = "-";
		double spriteW = 0, spriteH = 0;
		if (haveShared && it->renderId >= 0x200)
		{
			int idx = 0;
			if (Pack::ResolveObjectArt((uint8_t)(it->renderId & 0xff), (uint8_t)(it->renderId >> 8), idx))
			{
				const Image img = shared.ReadImage(idx);
				if (img.ok())
				{
					double upp = 2.0;
					if (img.modifier & IM_HALF_SIZE) { const int s = (img.imageType >> 5) & 3; upp = s == 0 ? 1.0 : 2.0 * (1 << s); }
					// Object art is stored with the picture's columns as its rows
					// (roth_das.cpp, THE AXES): drawn width = img.height.
					const int dw = img.height, dh = img.width;
					spriteW = dw * upp; spriteH = dh * upp;
					char t[96];
					snprintf(t, sizeof t, "%dx%d x%g = %gx%g", dw, dh, upp, spriteW, spriteH);
					sprite = t;
				}
			}
		}
		else if (it->renderId != 0) sprite = "map pack";

		// The film's widest horizontal extent over the turn and its tallest
		// vertical one, in film pixels: what the world sprite's size is set
		// against for a scale (units per film pixel).
		int maxW = 0, maxH = 0;
		for (int k = 0; k < film.frames; k++)
		{
			int x0 = film.width, x1 = -1, y0 = film.height, y1 = -1;
			for (int y = 0; y < film.height; y++)
				for (int x = 0; x < film.width; x++)
					if (masks[((size_t)k * film.height + y) * film.width + x]) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
			if (x1 >= 0) { maxW = std::max(maxW, x1 - x0 + 1); maxH = std::max(maxH, y1 - y0 + 1); }
		}
		if (spriteW > 0 && maxW > 0)
		{
			char t[96];
			snprintf(t, sizeof t, " -> %.3f u/px by width, %.3f by height", spriteW / maxW, spriteH / maxH);
			sprite += t;
		}
		const VoxCamera &c = G.camera;
		char head[512];
		snprintf(head, sizeof head, "%s|%s|%dx%d|%d|%d-%d|%.1f|%+d|%.1f [%.1f..%.1f]|%.1f,%.1f|%.2f|%s|%.3f/%.3f",
			idList.c_str(), name.c_str(), film.width, film.height, film.frames, c.firstFrame, c.lastFrame, c.period, c.direction,
			c.elevation * 180 / M_PI, G.elevationLow * 180 / M_PI, G.elevationHigh * 180 / M_PI, c.axisX, c.axisY, c.roll * 180 / M_PI,
			c.invDistance > 0 ? std::to_string((int)lround(1.0 / c.invDistance)).c_str() : "ortho", G.meanIoU, G.minFrameIoU);
		if (!G.turntable)
		{
			nNot++;
			printf("%s|-|-|-|-|-|-|-|-|-|%s|%s|NOT A TURNTABLE: %s\n", head, sprite.c_str(), G.runnerUp.c_str(), G.note.c_str());
			fflush(stdout);
			continue;
		}
		if (estimateOnly)
		{
			nOk++;
			printf("%s|-|-|-|-|-|-|-|-|%s|%s|%s t=%.1fs\n", head, sprite.c_str(), G.runnerUp.c_str(), G.note.c_str(), std::chrono::duration<double>(t1 - t0).count());
			fflush(stdout);
			continue;
		}
		VoxModel m;
		if (!VoxCarve(film, masks, c, carve, m)) { printf("%s|carve failed\n", head); continue; }
		const auto t2 = std::chrono::steady_clock::now();
		const std::vector<double> iou = VoxReprojectIoU(m, c, masks, film.width, film.height, film.frames);
		double mean = 0, mn = 1;
		int cnt = 0;
		for (double v : iou) if (v >= 0) { mean += v; mn = std::min(mn, v); cnt++; }
		mean /= std::max(1, cnt);
		const double photo = VoxPhotometricError(m, c, film, masks);
		std::string kerr;
		const std::vector<uint8_t> kvx = VoxBuildKvx(m, &kerr);
		nOk++;
		sumIoU += mean;
		printf("%s|%.3f|%.3f|%.1f|%dx%dx%d|%.2f|%d|%d|%s|%s|%s|%s%s evals=%d t=%.1fs+%.1fs\n", head, mean, mn, photo, m.sx, m.sy, m.sz, m.voxelPx,
			m.Count(), m.SurfaceCount(), kvx.empty() ? ("FAIL " + kerr).c_str() : std::to_string(kvx.size()).c_str(), sprite.c_str(),
			G.runnerUp.c_str(), (G.note + (m.photoCarved ? "photo-carved " + std::to_string(m.photoCarved) + "; " : "")).c_str(), film.paletteChanged ? "palette changes; " : "", G.evaluations,
			std::chrono::duration<double>(t1 - t0).count(), std::chrono::duration<double>(t2 - t1).count());
		fflush(stdout);

		if (!kvxDir.empty() && !kvx.empty())
		{
			FILE *o = fopen((kvxDir + "/item_" + std::to_string(ids[0]) + ".kvx").c_str(), "wb");
			if (o) { fwrite(kvx.data(), 1, kvx.size(), o); fclose(o); }
		}
		if (!pictures.empty())
		{
			// Row 1: the film at four angles of the turn. Row 2: the model with
			// the film's camera at the same frames. Row 3: the model from a
			// camera 20 degrees up at 0/90/180/270 degrees, and from straight
			// above, at the same scale.
			const int W = film.width, H = film.height;
			const int turn = c.lastFrame - c.firstFrame + 1;
			int ks[4];
			for (int j = 0; j < 4; j++) ks[j] = c.firstFrame + std::min(turn - 1, (int)lround(c.period * j / 4.0) % std::max(1, turn));
			const int cw = std::max(W, H) + 8;
			const int SW = cw * 5, SH = (H + 8) * 2 + cw;
			std::vector<uint8_t> sheet((size_t)SW * SH * 3, 40);
			const uint8_t bg[3] = { 0, 0, 0 }, bg2[3] = { 24, 24, 48 };
			std::vector<uint8_t> rgb;
			for (int j = 0; j < 4; j++)
			{
				rgb.resize((size_t)W * H * 3);
				const uint8_t *fr = film.Frame(ks[j]);
				for (int i = 0; i < W * H; i++) memcpy(&rgb[i * 3], film.palette + fr[i] * 3, 3);
				Blit(sheet, SW, SH, j * cw + 4, 4, rgb, W, H);
				VoxRenderFrame(m, c, ks[j], W, H, rgb, bg);
				Blit(sheet, SW, SH, j * cw + 4, H + 12, rgb, W, H);
			}
			const double scale = 1.0;
			for (int j = 0; j < 5; j++)
			{
				const double ang = j < 4 ? j * M_PI / 2 + M_PI / 8 : 0;
				const double el = j < 4 ? 20 * M_PI / 180 : 89 * M_PI / 180;
				VoxRender(m, ang, el, scale * m.voxelPx, cw - 4, cw - 4, rgb, bg2);
				Blit(sheet, SW, SH, j * cw + 2, (H + 8) * 2 + 2, rgb, cw - 4, cw - 4);
			}
			WritePpm(pictures + "/film_" + std::to_string(ids[0]) + ".ppm", SW, SH, sheet);
		}
	}
	if (!emitTable.empty())
	{
		FILE *o = fopen(emitTable.c_str(), "w");
		if (o)
		{
			fprintf(o, "// The close-up voxelizer's fitted turntable numbers, one row per retail item\n"
				"// film (roth_voxelize.h, THE FITTED-NUMBERS TABLE). Numbers only: no pixels.\n"
				"// Written by tools/voxelize_test (voxelize_test <DATA> --emit-table FILE)\n"
				"// from the player's install; included by roth_voxelize.cpp only.\n//\n"
				"// crc, size, period, direction, elevationOpen, firstFrame, lastFrame,\n"
				"// elevationDeg, axisX, axisY, rollDeg, distance (0 = orthographic)\n"
				"static const VoxFittedFilm VOX_FITTED[] =\n{\n");
			for (auto &f : fitted)
			{
				const VoxFittedFilm &v = f.first;
				fprintf(o, "\t{ 0x%08x, %u, %.4ff, %d, %d, %d, %d, %.4ff, %.4ff, %.4ff, %.4ff, %.2ff },   // %s\n", v.crc, v.size, v.period,
					v.direction, v.elevationOpen, v.firstFrame, v.lastFrame, v.elevationDeg, v.axisX, v.axisY, v.rollDeg, v.distance, f.second.c_str());
			}
			fprintf(o, "\t{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },   // end\n};\n");
			fclose(o);
		}
	}
	printf("# films: %d modelled (mean IoU %.3f), %d not turntables, %d information records skipped\n", nOk, nOk ? sumIoU / nOk : 0, nNot, nInfo);
	return 0;
}
