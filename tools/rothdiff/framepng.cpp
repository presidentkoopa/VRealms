// framepng.cpp -- turn an oracle frame dump into a picture you can look at.
//
// WHY THIS EXISTS
//
// The whole-frame comparison needs ROTH's frame and ours side by side at the
// same camera. Our half already lands as a PNG (`rothdiff_shot`). ROTH's half
// comes out of the rothdiff plugin as a .pgm of RAW PALETTE INDICES -- the byte
// at each pixel is the index the original selected, deliberately NOT put through
// a palette, because every measurement built on those dumps so far wanted the
// number rather than the colour.
//
// For a whole-frame comparison we want the colour. The palette is the MAP PACK's
// own, read here through the same reader the loader uses, so the picture is the
// one the original would have shown rather than an approximation of it.
//
// This is the piece the handoff called "decoding the oracle framebuffer dump to
// an image on Windows". The Linux path wrote a PPM to /tmp and went no further.
//
// It reads the player's own .DAS, reads a .pgm, and writes a .png. It writes
// nothing into the retail install and modifies nothing in ROTH.C's tree.
//
// Build:
//   cl /nologo /EHsc /std:c++17 /O2 /I ..\..\src\roth /Fe:framepng.exe \
//       /Fo:obj_fp\ framepng.cpp ../../src/roth/roth_das.cpp \
//       ../../src/roth/roth_install.cpp ../../src/roth/roth_palette.cpp
//
// Usage:
//   framepng <ROTH folder> <in.pgm> <MAPNAME> <out.png>
//   framepng <ROTH folder> <in.pgm> --shared <out.png>

#include "roth_das.h"
#include "roth_install.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}

//--------------------------------------------------------------------------
// A minimal PNG writer. No zlib: the IDAT payload is a zlib stream whose
// deflate blocks are all STORED (BTYPE 00), which is legal, universally
// readable, and about 30 lines. A frame is 640x480 and written once per
// capture, so the lost compression costs nothing worth a dependency.
//--------------------------------------------------------------------------
static uint32_t Crc32(const uint8_t *d, size_t n, uint32_t crc = 0xffffffffu)
{
	static uint32_t tab[256];
	static bool built = false;
	if (!built)
	{
		for (uint32_t i = 0; i < 256; i++)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
			tab[i] = c;
		}
		built = true;
	}
	for (size_t i = 0; i < n; i++) crc = tab[(crc ^ d[i]) & 0xff] ^ (crc >> 8);
	return crc;
}

static uint32_t Adler32(const uint8_t *d, size_t n)
{
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
	return (b << 16) | a;
}

static void PutBE32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
	v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

static void Chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
	PutBE32(out, (uint32_t)data.size());
	std::vector<uint8_t> tc;
	tc.insert(tc.end(), type, type + 4);
	tc.insert(tc.end(), data.begin(), data.end());
	out.insert(out.end(), tc.begin(), tc.end());
	PutBE32(out, Crc32(tc.data(), tc.size()) ^ 0xffffffffu);
}

static bool WritePng(const char *path, int w, int h, const std::vector<uint8_t> &rgb)
{
	// Raw scanlines, each prefixed with filter type 0 (None).
	std::vector<uint8_t> raw;
	raw.reserve((size_t)h * (1 + (size_t)w * 3));
	for (int y = 0; y < h; y++)
	{
		raw.push_back(0);
		const uint8_t *row = rgb.data() + (size_t)y * (size_t)w * 3;
		raw.insert(raw.end(), row, row + (size_t)w * 3);
	}

	std::vector<uint8_t> z;
	z.push_back(0x78); z.push_back(0x01);          // zlib header, no preset dict
	size_t at = 0;
	while (at < raw.size())
	{
		const size_t n = (raw.size() - at > 65535) ? 65535 : (raw.size() - at);
		const bool last = (at + n >= raw.size());
		z.push_back(last ? 1 : 0);                 // BFINAL, BTYPE = 00 stored
		z.push_back((uint8_t)(n & 0xff));  z.push_back((uint8_t)(n >> 8));
		const uint16_t nl = (uint16_t)~(uint16_t)n;
		z.push_back((uint8_t)(nl & 0xff)); z.push_back((uint8_t)(nl >> 8));
		z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
		at += n;
	}
	PutBE32(z, Adler32(raw.data(), raw.size()));

	std::vector<uint8_t> out;
	const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	out.insert(out.end(), sig, sig + 8);

	std::vector<uint8_t> ihdr;
	PutBE32(ihdr, (uint32_t)w); PutBE32(ihdr, (uint32_t)h);
	ihdr.push_back(8);      // bit depth
	ihdr.push_back(2);      // colour type 2 = truecolour
	ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
	Chunk(out, "IHDR", ihdr);
	Chunk(out, "IDAT", z);
	Chunk(out, "IEND", std::vector<uint8_t>());

	FILE *f = fopen(path, "wb");
	if (f == nullptr) return false;
	const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
	fclose(f);
	return ok;
}

//--------------------------------------------------------------------------
// Binary PGM (P5). The plugin writes the header as "P5\n<w> <h>\n255\n" with
// no comments, but comment lines are skipped anyway so a hand-made file works.
//--------------------------------------------------------------------------
static bool ReadPgm(const std::vector<uint8_t> &d, int *w, int *h, std::vector<uint8_t> *px)
{
	size_t p = 0;
	auto skipWs = [&]()
	{
		for (;;)
		{
			while (p < d.size() && (d[p] == ' ' || d[p] == '\n' || d[p] == '\r' || d[p] == '\t')) p++;
			if (p < d.size() && d[p] == '#') { while (p < d.size() && d[p] != '\n') p++; continue; }
			break;
		}
	};
	auto num = [&]() -> int
	{
		skipWs();
		int v = 0; bool any = false;
		while (p < d.size() && d[p] >= '0' && d[p] <= '9') { v = v * 10 + (d[p] - '0'); p++; any = true; }
		return any ? v : -1;
	};
	if (d.size() < 2 || d[0] != 'P' || d[1] != '5') return false;
	p = 2;
	*w = num(); *h = num();
	const int maxv = num();
	if (*w <= 0 || *h <= 0 || maxv != 255) return false;
	p++;                                    // exactly one whitespace byte follows maxval
	const size_t need = (size_t)*w * (size_t)*h;
	if (p + need > d.size()) return false;
	px->assign(d.begin() + p, d.begin() + p + need);
	return true;
}

int main(int argc, char **argv)
{
	if (argc < 5)
	{
		printf("usage: framepng <ROTH folder> <in.pgm> <MAPNAME|--shared> <out.png>\n");
		return 2;
	}
	if (!roth::TheInstall().Open(argv[1])) { printf("install: bad\n"); return 2; }

	const std::vector<uint8_t> pgm = ReadFile(argv[2]);
	if (pgm.empty()) { printf("cannot read %s\n", argv[2]); return 2; }
	int w = 0, h = 0;
	std::vector<uint8_t> idx;
	if (!ReadPgm(pgm, &w, &h, &idx)) { printf("%s is not a binary P5 pgm\n", argv[2]); return 2; }

	// The palette belongs to the MAP's pack. Getting this wrong does not fail,
	// it just produces a plausible picture in the wrong colours -- so say which
	// pack was used, every time.
	const std::string which = argv[3];
	const std::string pack = (which == "--shared")
		? roth::TheInstall().SharedPack()
		: roth::TheInstall().PackFor(which.c_str());
	const std::vector<uint8_t> pd = ReadFile(roth::TheInstall().PackFile(pack.c_str()));
	roth::Pack p;
	if (pd.empty() || !p.Load(pd.data(), pd.size()))
	{
		printf("cannot load pack %s\n", pack.c_str());
		return 2;
	}
	const std::vector<roth::Colour> &pal = p.Palette();
	if (pal.size() < 256) { printf("pack %s has no palette\n", pack.c_str()); return 2; }

	std::vector<uint8_t> rgb((size_t)w * (size_t)h * 3);
	// How much of the frame is index 0. A dump taken on a black fade, or before
	// the world pass ran, is almost entirely 0 -- and it is better to be told
	// that than to be handed a black picture and left to wonder.
	long long zero = 0;
	for (size_t i = 0; i < idx.size(); i++)
	{
		const roth::Colour c = pal[idx[i]];
		rgb[i * 3 + 0] = c.r; rgb[i * 3 + 1] = c.g; rgb[i * 3 + 2] = c.b;
		if (idx[i] == 0) zero++;
	}

	if (!WritePng(argv[4], w, h, rgb)) { printf("cannot write %s\n", argv[4]); return 2; }
	printf("%s  %dx%d  palette from %s  index 0 is %.1f%% of the frame\n",
	       argv[4], w, h, pack.c_str(), 100.0 * double(zero) / double(idx.size()));
	if (zero * 10 > (long long)idx.size() * 9)
		printf("  WARNING: nearly the whole frame is index 0 -- this is very "
		       "likely a fade or a pre-world frame, not a picture.\n");
	return 0;
}
