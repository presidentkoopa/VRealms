//
// gdvtool -- decode a Realms GDV movie with src/roth/roth_gdv and print one
// line per record: the CRC-32 of the decode buffer, of the movie palette in
// the header buffer, of the DAC as the screen shows it, of the record's
// decoded audio, and the subtitle machine's events. The same lines come out
// of the running original through gdvprobe.c; compare.py diffs the two.
//
// It writes no pictures and no sound anywhere: the movies are the player's
// retail files.
//
//   gdvtool <file.GDV> [DBASE100.DAT DBASE400.DAT]   (subtitles need both)
//
// GDV_GAMEPAL=<file> holds the game palette (g_palette_rgb_ptr, 0x300 bytes
// as hex: the probe's P line): with it the S lines carry the movie palette
// index the original draws each subtitle in.
// GDV_ORDER=imagefree runs the palette writes of a frame's settle before its
// subtitle call, as ROTH.C's image-free host does (traps.c,
// host_gdv_publish_frame), instead of after it as the original blitter does.
//
// GDV_LOOK=<frame>:<file.ppm> writes that one frame as the screen shows it,
// for a person to look at; give it a path outside the repository.
//
#include "../../src/roth/roth_gdv.h"
#include "../../src/roth/roth_dialogue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

using namespace roth;

static uint32_t Crc32(const void *data, size_t n, uint32_t c = 0)
{
	const uint8_t *p = (const uint8_t *)data;
	c = ~c;
	for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
	return ~c;
}

static std::vector<uint8_t> ReadFile(const char *path)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(path, "rb");
	if (!f) return b;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	b.resize((size_t)n);
	if (fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: gdvtool file.GDV [DBASE100.DAT DBASE400.DAT]\n"); return 2; }
	std::vector<uint8_t> movie = ReadFile(argv[1]);
	if (movie.empty()) { fprintf(stderr, "gdvtool: cannot read %s\n", argv[1]); return 1; }
	std::string stem = argv[1];
	if (stem.find_last_of("/\\") != std::string::npos) stem = stem.substr(stem.find_last_of("/\\") + 1);
	if (stem.find('.') != std::string::npos) stem = stem.substr(0, stem.find('.'));

	GdvDecoder d;
	if (!d.Open(movie.data(), movie.size())) { fprintf(stderr, "gdvtool: %s\n", d.Error().c_str()); return 1; }
	printf("M %s %dx%d frames=%d fps=%d rate=%d ver=%d spf=%d abytes=%u dpcm=%d\n", stem.c_str(), d.Width(), d.Height(),
		d.FrameCount(), d.Fps(), d.HeaderRate(), d.Version(), d.SamplesPerFrame(), d.AudioBytesPerRecord(), d.AudioIsDpcm() ? 1 : 0);

	// The subtitle stream, as play_record_gdv_cutscene loads it.
	GdvSubtitles subs;
	if (argc >= 4)
	{
		std::vector<uint8_t> d100 = ReadFile(argv[2]), d400 = ReadFile(argv[3]);
		if (d100.size() > 0x28)
		{
			const uint32_t cnt = *(const uint32_t *)(d100.data() + 0x20), tab = *(const uint32_t *)(d100.data() + 0x24);
			for (uint32_t i = 0; i < cnt; i++)
			{
				const uint8_t *r = d100.data() + tab + i * 0x14;
				char name[9] = {}; memcpy(name, r, 8);
				if (strcasecmp(name, stem.c_str()) != 0) continue;
				const int16_t sz = (int16_t)(r[0xa] | (r[0xb] << 8));
				const uint32_t off = (*(const uint32_t *)(r + 0x10)) & 0xffffff;
				if (sz > 0 && off + (uint32_t)sz <= d400.size()) subs.Start(d400.data() + off, (size_t)sz);
				printf("U entry=%u size=%d offset=0x%x\n", i, (int)sz, off);
				break;
			}
		}
	}

	uint8_t gamePal[0x300] = {}; bool haveGamePal = false;
	if (const char *e = getenv("GDV_GAMEPAL"))
		if (FILE *f = fopen(e, "r"))
		{
			unsigned v; int i = 0;
			while (i < 0x300 && fscanf(f, "%2x", &v) == 1) gamePal[i++] = (uint8_t)v;
			haveGamePal = i == 0x300;
			fclose(f);
		}
	const bool imageFreeOrder = getenv("GDV_ORDER") && strcmp(getenv("GDV_ORDER"), "imagefree") == 0;
	int look = -1; const char *lookPath = nullptr;
	if (const char *e = getenv("GDV_LOOK")) { look = atoi(e); lookPath = strchr(e, ':'); if (lookPath) lookPath++; }
	uint32_t audioCrc = 0; uint64_t audioSamples = 0; int shownFrames = 0;
	while (d.NextFrame())
	{
		const int k = d.FrameIndex();
		const std::vector<int16_t> &a = d.AudioBlock();
		const uint32_t ac = Crc32(a.data(), a.size() * 2);
		audioCrc = Crc32(a.data(), a.size() * 2, audioCrc);
		audioSamples += a.size();
		if (d.FrameShown()) shownFrames++;
		if (look >= 0 && k == look && lookPath)
		{
			std::vector<uint8_t> pic;
			d.ExpandPicture(pic);
			if (FILE *f = fopen(lookPath, "wb"))
			{
				fprintf(f, "P6 %d %d 255\n", d.Width(), d.Height());
				for (uint8_t i : pic) { const uint8_t *c = d.Dac() + i * 3; const uint8_t rgb[3] = { (uint8_t)(c[0] << 2), (uint8_t)(c[1] << 2), (uint8_t)(c[2] << 2) }; fwrite(rgb, 1, 3, f); }
				fclose(f);
			}
		}
		printf("F %d pix=%08x pal=%08x dac=%08x flags=%02x aud=%08x n=%u\n", k, Crc32(d.Pixels(), d.PixelBytes()),
			Crc32(d.MoviePalette(), 0x300), Crc32(d.Dac(), 0x300), d.ChunkFlags(), ac, (unsigned)a.size());
		// The subtitle machine, in the original's order (roth_gdv.h).
		for (int n = d.DecodeWrites(); n > 0; n--) subs.PaletteWritten();
		if (imageFreeOrder) for (int n = d.EmitWrites(); n > 0; n--) subs.PaletteWritten();
		if (subs.Active() && subs.Frame(d.SampleClock()))
		{
			if (subs.Showing())
			{
				const unsigned col = haveGamePal ? GdvSubtitles::NearestIndex(gamePal + subs.ColorIndex() * 3, d.MoviePalette()) : subs.ColorIndex();
				// The layout the original draws (layout_timed_message_text at the
				// screen pitch, two lines; dialogue_ui.c:1932-1933).
				std::vector<TextLine> rec;
				std::string z = subs.Text(); z.push_back('\0');
				LayoutTimedMessageText(rec, (const uint8_t *)z.data(), 640, 2, (uint8_t)col);
				printf("S %d show col=%u crc=%08x x0=%d x1=%d h=%d %s\n", k, col, Crc32(subs.Text().data(), subs.Text().size()),
					rec.empty() ? 0 : rec[0].x0, rec.empty() ? 0 : rec[0].x1, rec.empty() ? 0 : rec[0].height, subs.Text().c_str());
			}
			else printf("S %d clear\n", k);
		}
		if (!imageFreeOrder) for (int n = d.EmitWrites(); n > 0; n--) subs.PaletteWritten();
	}
	if (!d.Error().empty()) { printf("E %s\n", d.Error().c_str()); }
	printf("T %s frames=%d shown=%d audio_samples=%llu audio_crc=%08x seconds=%.3f\n", stem.c_str(), d.FrameIndex() + 1, shownFrames,
		(unsigned long long)audioSamples, audioCrc, (double)(d.FrameIndex() + 1) * d.SamplesPerFrame() / GDV_OUTPUT_RATE);
	return d.Error().empty() ? 0 : 1;
}
