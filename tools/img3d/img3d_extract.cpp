// img3d_extract.cpp -- THE ITEM PICTURES FOR THE IMAGE-TO-3D PIPELINE (docs/IMG3D_PIPELINE.md, step 1 of 3), read
// from the player's own install through the engine's readers (src/roth) and the parked close-up reader
// (tools/voxelize_parked). Engine-free. Writes ONLY into the folder it is given, which must be outside the repository:
// everything it writes is derived from retail art (CLAUDE.md).
//
//   img3d_extract "<ROTH DATA folder>" "<out folder>" <item> [<item> ...]
//
// Per item, <out>/<id>/ gets:
//   icon.tga        the inventory icon (DBASE200, the item's +0x0C), the game palette of STUDY1's pack, index 0 a hole
//   world.tga       the picture the item lies in the world as (its +0x02 render id through the object art rule,
//                   Pack::ResolveObjectArt; the shared pack or STUDY1's), index 0 a hole unless the entry says opaque;
//                   absent for a directional, 3D or missing entry
//   film_NNN.tga    every frame of the close-up film (DBASE300, the item's +0x08), its own palette, the background
//                   cut away by the film's ground mask (tools/voxelize_parked/roth_voxelize.h VoxBuildMasks)
//   info.json       the item's id, name, render id, the sizes, and per film frame the item's pixel count and whether
//                   it touches the frame's edge -- what tools/img3d/extract.py chooses the view by
// TGA: 32-bit BGRA, top-left origin, alpha 0 or 255. Nothing else is written; nothing is printed beyond progress.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "roth_install.h"
#include "roth_dbase100.h"
#include "roth_icons.h"
#include "roth_das.h"
#include "roth_voxelize.h"

using namespace roth;
namespace fs = std::filesystem;

static std::vector<uint8_t> ReadAll(const std::string &p)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return b;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b.resize(n > 0 ? (size_t)n : 0);
	if (n > 0 && fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
	fclose(f);
	return b;
}

// Row-major RGBA, top row first.
static bool WriteTga(const std::string &path, int w, int h, const std::vector<uint8_t> &rgba)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	uint8_t hd[18] = {};
	hd[2] = 2;                       // uncompressed true colour
	hd[12] = (uint8_t)(w & 255); hd[13] = (uint8_t)(w >> 8);
	hd[14] = (uint8_t)(h & 255); hd[15] = (uint8_t)(h >> 8);
	hd[16] = 32;
	hd[17] = 0x28;                   // 8 alpha bits, top-left origin
	fwrite(hd, 1, 18, f);
	std::vector<uint8_t> bgra(rgba.size());
	for (size_t i = 0; i + 3 < rgba.size(); i += 4) { bgra[i] = rgba[i + 2]; bgra[i + 1] = rgba[i + 1]; bgra[i + 2] = rgba[i]; bgra[i + 3] = rgba[i + 3]; }
	const bool ok = fwrite(bgra.data(), 1, bgra.size(), f) == bgra.size();
	fclose(f);
	return ok;
}

// DBASE400 entry: u32 voice, u16 length, u8 colour, u8, then the text (docs/GAME_inventory.md 2.4).
static std::string Text(const std::vector<uint8_t> &t, uint32_t off)
{
	if (t.empty() || off == 0 || (uint64_t)off + 8 > t.size()) return "";
	const uint8_t *p = t.data() + off;
	unsigned len = (unsigned)(p[4] | (p[5] << 8));
	std::string s;
	for (unsigned i = 0; i < len && off + 8 + i < t.size() && s.size() < 60; i++)
	{
		uint8_t c = p[8 + i];
		if (c == 0) break;
		s += (c == '\n' || c == '\r') ? ' ' : ((c >= 0x20 && c < 0x7f) ? (char)c : '?');
	}
	return s;
}

static std::string JsonStr(const std::string &s)
{
	std::string o = "\"";
	for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
	return o + "\"";
}

static std::vector<uint8_t> Paletted(const std::vector<uint8_t> &idx, const std::vector<Colour> &pal, bool zeroHole)
{
	std::vector<uint8_t> out(idx.size() * 4);
	for (size_t i = 0; i < idx.size(); i++)
	{
		const uint8_t v = idx[i];
		const Colour c = v < pal.size() ? pal[v] : Colour{ 0, 0, 0 };
		out[i * 4] = c.r; out[i * 4 + 1] = c.g; out[i * 4 + 2] = c.b;
		out[i * 4 + 3] = (zeroHole && v == 0) ? 0 : 255;
	}
	return out;
}

int main(int argc, char **argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "usage: img3d_extract \"<ROTH DATA folder>\" \"<out folder>\" <item> [<item> ...]\n");
		return 2;
	}
	const std::string dir = argv[1], out = argv[2];
	Install inst;
	if (!inst.Open(dir.c_str())) { fprintf(stderr, "install: %s\n", inst.Error().c_str()); return 2; }
	auto data = [&](const char *n) { const std::string p = inst.DataFile(n); return p.empty() ? std::vector<uint8_t>() : ReadAll(p); };
	std::vector<uint8_t> d100 = data("DBASE100.DAT"), d200 = data("DBASE200.DAT"), d300 = data("DBASE300.DAT"), d400 = data("DBASE400.DAT");
	Dbase100 db;
	if (d100.empty() || !db.Load(d100.data(), d100.size())) { fprintf(stderr, "no readable DBASE100.DAT in %s\n", dir.c_str()); return 2; }

	// The packs: STUDY1's (its palette is the game palette chapters 1 and 2 are drawn in) and the shared one.
	std::vector<uint8_t> levelBytes = ReadAll(inst.PackFile(inst.PackFor("STUDY1").c_str()));
	std::vector<uint8_t> sharedBytes = ReadAll(inst.PackFile(inst.SharedPack().c_str()));
	Pack level, shared;
	const bool haveLevel = !levelBytes.empty() && level.Load(levelBytes.data(), levelBytes.size());
	const bool haveShared = !sharedBytes.empty() && shared.Load(sharedBytes.data(), sharedBytes.size());
	if (!haveLevel) { fprintf(stderr, "STUDY1's pack does not load: no game palette\n"); return 2; }
	const std::vector<Colour> pal = level.Palette();

	int bad = 0;
	for (int a = 3; a < argc; a++)
	{
		const int id = atoi(argv[a]);
		const DbItem *it = db.Item(id);
		if (it == nullptr || !it->ok) { fprintf(stderr, "item %d: no record\n", id); bad++; continue; }
		const std::string name = Text(d400, it->name);
		const fs::path idir = fs::path(out) / std::to_string(id);
		std::error_code ec;
		fs::create_directories(idir, ec);
		if (ec) { fprintf(stderr, "item %d: cannot make %s\n", id, idir.string().c_str()); return 2; }
		std::string json = "{\n  \"item\": " + std::to_string(id) + ",\n  \"name\": " + JsonStr(name) + ",\n  \"render_id\": " + std::to_string(it->renderId);
		int views = 0;

		// The inventory icon.
		if (it->icon != 0 && !d200.empty())
		{
			DasImage ic = DecodeDbase200Resource(d200, it->icon);
			if (ic.ok && WriteTga((idir / "icon.tga").string(), ic.width, ic.height, Paletted(ic.pixels, pal, true)))
			{
				json += ",\n  \"icon\": [" + std::to_string(ic.width) + ", " + std::to_string(ic.height) + "]";
				views++;
			}
		}

		// The picture it lies in the world as.
		{
			int artIndex = 0;
			const bool useShared = Pack::ResolveObjectArt((uint8_t)(it->renderId & 0xff), (uint8_t)(it->renderId >> 8), artIndex);
			const Pack *pk = useShared ? (haveShared ? &shared : nullptr) : &level;
			const FatEntry *e = pk ? pk->Entry(artIndex) : nullptr;
			if (e != nullptr && e->kind != EntryKind::Empty && e->kind != EntryKind::Indirection && e->kind != EntryKind::Object3D)
			{
				Image img = pk->ReadImage(artIndex, false);
				if (img.ok())
				{
					// Stored a quarter turn: column-major with the shown width = img.height (roth_texture_object.cpp Sprite).
					const int W = img.height, H = img.width;
					const std::vector<uint8_t> &f = img.frames[0];
					std::vector<uint8_t> rows((size_t)W * H);
					for (int x = 0; x < W; x++)
						for (int y = 0; y < H; y++) rows[(size_t)y * W + x] = (size_t)x * H + y < f.size() ? f[(size_t)x * H + y] : 0;
					if (WriteTga((idir / "world.tga").string(), W, H, Paletted(rows, pal, img.ZeroIsTransparent())))
					{
						json += ",\n  \"world\": [" + std::to_string(W) + ", " + std::to_string(H) + "],\n  \"world_pack\": " +
							JsonStr(useShared ? inst.SharedPack() : inst.PackFor("STUDY1")) + ",\n  \"world_entry\": " + std::to_string(artIndex);
						views++;
					}
				}
			}
		}

		// The close-up film.
		if (it->closeup != 0 && !d300.empty())
		{
			const uint8_t *g = nullptr;
			size_t gs = 0;
			VoxFilm film;
			std::string err;
			if (VoxCloseupBytes(d300.data(), d300.size(), it->closeup, g, gs) && VoxDecodeFilm(g, gs, film, &err) && film.frames > 0)
			{
				std::vector<uint8_t> masks;
				VoxBuildMasks(film, masks);
				const int W = film.width, H = film.height;
				json += ",\n  \"film\": [" + std::to_string(W) + ", " + std::to_string(H) + ", " + std::to_string(film.frames) + "],\n  \"frames\": [";
				for (int k = 0; k < film.frames; k++)
				{
					const uint8_t *fr = film.Frame(k);
					const uint8_t *mk = masks.data() + (size_t)k * W * H;
					std::vector<uint8_t> rgba((size_t)W * H * 4);
					int area = 0;
					bool edge = false;
					for (int y = 0; y < H; y++)
						for (int x = 0; x < W; x++)
						{
							const size_t i = (size_t)y * W + x;
							const uint8_t *c = film.palette + fr[i] * 3;
							rgba[i * 4] = c[0]; rgba[i * 4 + 1] = c[1]; rgba[i * 4 + 2] = c[2];
							rgba[i * 4 + 3] = mk[i] ? 255 : 0;
							if (mk[i]) { area++; if (x == 0 || y == 0 || x == W - 1 || y == H - 1) edge = true; }
						}
					char fn[32];
					snprintf(fn, sizeof fn, "film_%03d.tga", k);
					WriteTga((idir / fn).string(), W, H, rgba);
					json += std::string(k ? ", " : "") + "[" + std::to_string(area) + ", " + (edge ? "1" : "0") + "]";
				}
				json += "]";
				views++;
			}
			else fprintf(stderr, "item %d: close-up film %u does not decode (%s)\n", id, it->closeup, err.c_str());
		}
		json += "\n}\n";
		if (FILE *jf = fopen((idir / "info.json").string().c_str(), "wb")) { fwrite(json.data(), 1, json.size(), jf); fclose(jf); }
		printf("item %d %s: %d picture source(s) -> %s\n", id, name.c_str(), views, idir.string().c_str());
		if (views == 0) bad++;
	}
	return bad ? 1 : 0;
}
