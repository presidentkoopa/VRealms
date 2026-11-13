//
// menudraw_diff -- the pictures of the original's message boxes and dialogue
// text, through THE PORT (roth_menudraw / roth_dlgdraw), against the
// original's framebuffer, byte for byte.
//
//   menudraw_diff <retail DATA folder> <oracle log> <oracle dump dir> [ppm dir]
//
// The oracle log is what menuprobe.c wrote while the script ran in the
// ORIGINAL (ROTH.C, headless). This program reads that log in order and lets
// the port do what the original did at each step:
//   "X va n: ..."   the executable's bytes: the font, the box descriptors
//                   (compared with the port's transcription), the colours
//   "> fill v"      the screen filled with v before a test
//   "> savefile"    a save file the original then read (names, thumbnails)
//   "> snapshot"    g_ui_panel_scratch_handle's picture
//   "O d desc fl"   show_message_box opened (depth d): MessageBoxPainter::Open
//   "L d mark= scroll="  the state one loop pass left: MessageBoxPainter::Pass
//   "C d ..."       the box closed (its save-under put back): the picture dropped
//   "HG ..." "H"    the original's hit test over a grid: the port's, printed alike
//   "D name hash"   the screen dumped: the port's composite of the open pictures
//                   over the fill, compared with <dump dir>/<name>.scr
// The pixels of a box's 4-pixel shadow ring (a blend of whatever lies around
// it, not drawn by the port) are left out of the comparison and counted.
//
// Writes nothing but stdout (and, with a ppm dir, PPM pictures of both sides
// and of the difference -- retail art: never into the repo).
//

#include "../../src/roth/roth_gamecore.h"
#include "../../src/roth/roth_dialogue.h"
#include "../../src/roth/roth_menudraw.h"
#include "../../src/roth/roth_dlgdraw.h"
#include "../../src/roth/roth_install.h"
#include "../../src/roth/roth_das.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace roth;

namespace
{

std::vector<uint8_t> ReadFile(const std::string &path)
{
	std::vector<uint8_t> out;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return out;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0) { out.resize((size_t)n); if (fread(out.data(), 1, (size_t)n, f) != (size_t)n) out.clear(); }
	fclose(f);
	return out;
}

std::vector<uint8_t> Hex(const char *p)
{
	std::vector<uint8_t> v;
	unsigned x;
	int n;
	while (sscanf(p, " %2x%n", &x, &n) == 1) { v.push_back((uint8_t)x); p += n; }
	return v;
}

struct ExeBytes { uint32_t va = 0; std::vector<uint8_t> b; };
std::vector<ExeBytes> g_exe;

const uint8_t *ExeAt(uint32_t va, size_t n)
{
	for (const ExeBytes &e : g_exe)
		if (va >= e.va && va + n <= e.va + e.b.size()) return e.b.data() + (va - e.va);
	return nullptr;
}

uint32_t U32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t U16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// The transcribed descriptors against the original's bytes.
int CheckDescriptors()
{
	int bad = 0, checked = 0;
	for (const MsgBoxDesc &d : MsgBoxDescriptors())
	{
		const uint8_t *p = ExeAt(d.va, 0x14 + d.entries.size() * 0xc);
		if (p == nullptr) { printf("TABLE desc %x: no bytes from the original\n", d.va); bad++; continue; }
		auto cmp = [&](const char *what, uint32_t mine, uint32_t theirs) { checked++; if (mine != theirs) { bad++; printf("TABLE desc %x %s: port %x original %x\n", d.va, what, mine, theirs); } };
		cmp("title", d.title, U32(p));
		cmp("body", d.body, U32(p + 4));
		cmp("icon", d.icon, U16(p + 8));
		cmp("sel", d.sel, U16(p + 0xa));
		cmp("scroll", d.scroll, U16(p + 0xc));
		cmp("range", d.range, U16(p + 0xe));
		cmp("count", (uint32_t)d.entries.size(), U32(p + 0x10));
		for (size_t i = 0; i < d.entries.size(); i++)
		{
			const uint8_t *e = p + 0x14 + i * 0xc;
			cmp("entry text", d.entries[i].text, U32(e));
			cmp("entry alt", d.entries[i].alt, U32(e + 4));
			cmp("entry value", d.entries[i].value, U32(e + 8));
		}
	}
	if (const uint8_t *c = ExeAt(0x7675d, 8))
	{
		checked += 2;
		if (c[0] != 0x40) { bad++; printf("TABLE marker colour differs\n"); }
		if (c[6] != 0xee) { bad++; printf("TABLE default colour [5] differs\n"); }
	}
	if (const uint8_t *f = ExeAt(0x75ef9, 7)) { checked++; if (memcmp(f, "%D: %s", 7) != 0) { bad++; printf("TABLE \"%%D: %%s\" differs\n"); } }
	printf("TABLES checked %d values, %d differ\n", checked, bad);
	return bad;
}

void WritePpm(const std::string &path, const uint8_t *px, int w, int h, const std::vector<Colour> &pal)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) { const Colour &c = pal[px[i]]; fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
	fclose(f);
}

// The test's synthetic pictures (menuprobe.c: savefile, snapshot): a DAS
// picture with flags 2 (no palette), plain rows of a seeded sequence.
std::vector<uint8_t> Synthetic(int w, int h, int seed)
{
	std::vector<uint8_t> img(8 + (size_t)w * h, 0);
	img[0] = 2; img[4] = (uint8_t)w; img[5] = (uint8_t)(w >> 8); img[6] = (uint8_t)h; img[7] = (uint8_t)(h >> 8);
	uint32_t r = (uint32_t)seed * 2654435761u;
	for (int i = 0; i < w * h; i++) { r = r * 1103515245u + 12345u; img[8 + (size_t)i] = (uint8_t)((r >> 16) & 0xff); }
	return img;
}

struct SaveFile { bool present = false; std::string name; bool hasName = false; int w = 0, h = 0, seed = 0; };

// The dialogue state a "dtext" logged before its dump: the globals render_text_ui
// reads and the records (g_dialogue_segments, g_timed_message_lines) as the
// original holds them (tools/dialogue_test proves roth_dialogue's records equal).
struct TextState
{
	bool pending = false;
	unsigned gate = 0;
	int ctx = 0, seg = 0, mode = 0, mode10 = 0x28, inv = 0, ay = 0, vy = 0, vh = 0, tm = 0, timer = 0, busy = 0;
	std::vector<TextLine> segs, tml;
};

bool ParseRecord(const char *p, TextLine &t)
{
	int i = 0, len = 0;
	char hex[4096] = {};
	if (sscanf(p, "%*s %d %d %d %d %d %4095s", &i, &t.x0, &t.x1, &t.height, &len, hex) < 5) return false;
	t.len = len;
	t.text = Hex(hex);
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "menudraw_diff <DATA folder> <oracle log> <oracle dump dir> [ppm dir]\n"); return 2; }
	const std::string dir = argv[1];
	const std::string dumps = argv[3];
	const std::string ppm = argc > 4 ? argv[4] : "";
	Dbase100 db;
	std::vector<uint8_t> d100 = ReadFile(dir + "/DBASE100.DAT");
	if (d100.empty() || !db.Load(d100.data(), d100.size())) { fprintf(stderr, "no DBASE100.DAT in %s\n", dir.c_str()); return 2; }
	std::vector<uint8_t> icons = ReadFile(dir + "/DATA/ICONS.ALL");
	if (icons.empty()) { fprintf(stderr, "no DATA/ICONS.ALL in %s\n", dir.c_str()); return 2; }
	GameCore core;
	DialogueUi dlg;
	dlg.text = ReadFile(dir + "/DBASE400.DAT");
	core.Attach(&db, &dlg, nullptr);
	dlg.Bind(&core.Interp(), &db);

	Install inst;
	if (!inst.Open(dir.c_str())) { fprintf(stderr, "install: %s\n", inst.Error().c_str()); return 2; }
	std::vector<uint8_t> packBytes = ReadFile(inst.PackFile(inst.PackFor("STUDY1").c_str()));
	Pack pack;
	if (packBytes.empty() || !pack.Load(packBytes.data(), packBytes.size()) || pack.ShadeTables() == nullptr || pack.BlendTable() == nullptr)
	{ fprintf(stderr, "STUDY1's pack has no shade/blend tables\n"); return 2; }
	const std::vector<Colour> pal = pack.Palette();

	FILE *lf = fopen(argv[2], "r");
	if (!lf) { fprintf(stderr, "no oracle log %s\n", argv[2]); return 2; }
	std::vector<std::string> log;
	{
		std::vector<char> line(1 << 18);
		while (fgets(line.data(), (int)line.size(), lf)) { std::string s = line.data(); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); log.push_back(s); }
		fclose(lf);
	}
	// The executable's bytes first (they come before the commands that use them).
	for (const std::string &l : log)
	{
		if (l.compare(0, 2, "X ") != 0) continue;
		ExeBytes e; unsigned va = 0, n = 0;
		if (sscanf(l.c_str(), "X %x %u:", &va, &n) == 2) { e.va = va; e.b = Hex(strchr(l.c_str(), ':') + 1); g_exe.push_back(e); }
	}
	int bad = CheckDescriptors();
	RothFont font;
	if (const uint8_t *f = ExeAt(0x70f12 - (uint32_t)ROTH_FONT_BEFORE, ROTH_FONT_BEFORE + ROTH_FONT_AFTER))
	{
		font.bytes.assign(f, f + ROTH_FONT_BEFORE + ROTH_FONT_AFTER);
		font.desc = ROTH_FONT_BEFORE;
		const uint32_t h = RothFontHash(font.bytes.data(), font.bytes.size());
		printf("FONT hash %08x (%s)\n", h, h == ROTH_FONT_HASH ? "the original's" : "DIFFERS");
		if (h != ROTH_FONT_HASH) bad++;
	}
	else { printf("FONT no font bytes in the log\n"); bad++; }
	UiAssets ua;
	ua.iconsAll = &icons;
	ua.shade = pack.ShadeTables();
	ua.blend = pack.BlendTable();
	ua.font = font.ok() ? &font : nullptr;

	int pitch = 640, height = 480, ax = 0, ay = 0, tickstep = 1;
	bool hires = false;
	int fill = 0;
	std::map<int, SaveFile> saves;
	std::vector<uint8_t> snapshot;
	std::vector<std::unique_ptr<MessageBoxPainter>> boxes;     // index = depth - 1
	std::vector<std::string> problems;
	TextState ts;
	DialogueTextPainter choicePainter;                          // the last choice panel drawn (its hit areas)
	bool haveChoice = false;
	int dumpsCompared = 0, dumpsBad = 0;
	long pixels = 0, differing = 0, excludedTotal = 0;

	auto slotNames = [&]() {
		// read_savegame_slot_names (savegame.c:1262-1294) over the files the
		// probe wrote: {0, '~'} without a file; the name record; 0 0 when the
		// name is empty.
		std::vector<uint8_t> n(0x1b0, 0);
		for (int s = 1; s <= 9; s++)
		{
			uint8_t *cell = n.data() + (s - 1) * 0x30;
			cell[0] = 0; cell[1] = 0x7e;
			auto it = saves.find(s);
			if (it == saves.end() || !it->second.present || !it->second.hasName) continue;
			const std::string &nm = it->second.name;
			for (size_t i = 0; i <= nm.size() && i < 0x30; i++) cell[i] = i < nm.size() ? (uint8_t)nm[i] : 0;
			if (cell[0] == 0) cell[1] = 0;
		}
		return n;
	};

	for (size_t li = 0; li < log.size(); li++)
	{
		const std::string &l = log[li];
		const char *p = l.c_str();
		if (!strncmp(p, "ENV ", 4))
		{
			int hr = 0;
			sscanf(p, "ENV pitch=%d h=%d hires=%d ax=%d ay=%d", &pitch, &height, &hr, &ax, &ay);
			hires = hr != 0;
			printf("%s\n", p);
			continue;
		}
		if (!strncmp(p, "TABLES ", 7))
		{
			unsigned blend = 0, ramp = 0, world = 0; int off = 1;
			sscanf(p, "TABLES blend=%x ramp=%x world=%x worldoff=%d", &blend, &ramp, &world, &off);
			auto fnv = [](const uint8_t *q, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) h = (h ^ q[i]) * 16777619u; return h; };
			const bool same = blend == fnv(pack.BlendTable(), 0x10000) && ramp == fnv(pack.ShadeTables(), 0x4000) && world == fnv(pack.ShadeTables(), 0x2000) && off == 0;
			printf("PACK tables %s\n", same ? "identical (and [0x86d28] is [0x86d14])" : "DIFFER");
			if (!same) bad++;
			continue;
		}
		if (!strncmp(p, "> ", 2))
		{
			printf("%s\n", p);
			char cmd[64] = {}; int a[8] = {};
			sscanf(p + 2, "%63s %i %i %i %i %i %i %i %i", cmd, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]);
			const std::string c = cmd;
			if (c == "fill") fill = a[0];
			else if (c == "tickstep") tickstep = a[0];
			else if (c == "rmsaves") saves.clear();
			else if (c == "savefile")
			{
				SaveFile s; s.present = true; s.w = a[1]; s.h = a[2]; s.seed = a[3];
				const char *n = p + 2; for (int k = 0; k < 5 && n; k++) n = strchr(n + 1, ' ');
				s.name = n ? n + 1 : "-";
				s.hasName = s.name != "-";
				if (!s.hasName) s.name.clear();
				saves[a[0]] = s;
			}
			else if (c == "snapshot") snapshot = a[0] > 0 ? Synthetic(a[0], a[1], a[2]) : std::vector<uint8_t>();
			continue;
		}
		if (p[0] == 'O' && p[1] == ' ')
		{
			int depth = 0; unsigned va = 0, flags = 0; int sel = 0, scroll = 0, range = 0;
			sscanf(p, "O %d %x %x sel=%d scroll=%d range=%d", &depth, &va, &flags, &sel, &scroll, &range);
			const MsgBoxDesc *d = FindMsgBoxDesc(va);
			printf("%s\n", p);
			if (d == nullptr) { printf("? no descriptor %x in the port\n", va); bad++; continue; }
			std::unique_ptr<MessageBoxPainter> b(new MessageBoxPainter());
			b->SetAssets(ua);
			b->screenPitch = pitch; b->screenHeight = height; b->hires = hires;
			b->anchorX = ax; b->anchorY = ay;
			b->resolveText = [&dlg](uint8_t *dest, uint32_t maxlen, uint32_t id) { return dlg.ResolveDbase100TextBytes(dest, maxlen, (int)id, 0); };
			b->slotNames = slotNames();
			b->thumbnail = [&saves](uint32_t slot) {
				auto it = saves.find((int)slot + 1);
				if (it == saves.end() || !it->second.present || it->second.w <= 0) return std::vector<uint8_t>();
				return Synthetic(it->second.w, it->second.h, it->second.seed);
			};
			b->panelSnapshot = snapshot;
			MsgBoxDesc dd = *d;
			dd.range = (uint16_t)range;
			b->Open(dd, flags, sel, scroll);
			while ((int)boxes.size() >= depth) boxes.pop_back();
			boxes.push_back(std::move(b));
			continue;
		}
		if (p[0] == 'L' && p[1] == ' ')
		{
			int depth = 0, mark = -1, scroll = 0;
			if (sscanf(p, "L %d mark=%d scroll=%d", &depth, &mark, &scroll) == 3 && depth >= 1 && depth <= (int)boxes.size())
			{
				MessageBoxPainter &b = *boxes[(size_t)depth - 1];
				b.Pass(mark >= 0 ? mark : -2, scroll != b.Scroll() ? scroll : -1, (uint32_t)tickstep);
			}
			continue;
		}
		if (p[0] == 'C' && p[1] == ' ')
		{
			int depth = 0;
			sscanf(p, "C %d", &depth);
			printf("%s\n", p);
			while ((int)boxes.size() >= depth && !boxes.empty())
			{
				for (const std::string &pr : boxes.back()->Problems()) problems.push_back(pr);
				boxes.pop_back();
			}
			continue;
		}
		if (p[0] == 'T' && p[1] == ' ')
		{
			ts = TextState();
			sscanf(p, "T gate=%x ctx=%d seg=%d mode=%d mode10=%d inv=%d ay=%d vy=%d vh=%d tm=%d timer=%d busy=%d", &ts.gate, &ts.ctx, &ts.seg, &ts.mode,
				&ts.mode10, &ts.inv, &ts.ay, &ts.vy, &ts.vh, &ts.tm, &ts.timer, &ts.busy);
			ts.pending = true;
			continue;
		}
		if ((p[0] == 'T' && (p[1] == 'S' || p[1] == 'T')) && p[2] == ' ')
		{
			TextLine t;
			if (ParseRecord(p, t)) (p[1] == 'S' ? ts.segs : ts.tml).push_back(t);
			continue;
		}
		if (!strncmp(p, "CHG", 3))
		{
			int step = 3;
			const char *st = strstr(p, "step=");
			if (st) step = atoi(st + 5);
			if (!haveChoice || ts.busy == 0 || ts.gate != 0x6ffff) { printf("CHG none\n"); continue; }
			const DialogueTextPainter &c = choicePainter;
			const int x0 = c.RegionX() - 8, y0 = c.RegionY() - 8, x1 = c.RegionX() + c.RegionW() - 1 + 8, y1 = c.RegionY() + c.RegionH() + 8;
			printf("CHG %d %d %d %d step=%d\n", x0, y0, x1, y1, step);
			for (int y = y0; y <= y1; y += step)
				for (int x = x0; x <= x1; x += step)
				{
					const int r = c.ChoiceRowAt(x, y);
					if (r >= 0) printf("CH %d %d %d\n", x, y, r);
				}
			continue;
		}
		if (!strncmp(p, "HG", 2))
		{
			int step = 3;
			const char *s = strstr(p, "step=");
			if (s) step = atoi(s + 5);
			if (boxes.empty()) { printf("HG none\n"); continue; }
			const MessageBoxPainter &b = *boxes.back();
			printf("HG box=%d,%d rows=%d listy=%d flags=%x step=%d\n", b.BoxX(), b.BoxY(), b.Rows(), b.BoxY() + 0x10, b.Flags(), step);
			const int x0 = b.BoxX() - 6, y0 = b.BoxY() - 6, x1 = b.BoxX() + 0x130, y1 = b.BoxY() + 0x10 + b.Rows() * 0xb + 0x50;
			for (int y = y0; y <= y1; y += step)
				for (int x = x0; x <= x1; x += step)
				{
					const int r = b.HitTest(x, y);
					if (r) printf("H %d %d %d\n", x, y, r);
				}
			continue;
		}
		if (p[0] == 'D' && p[1] == ' ')
		{
			char name[128] = {};
			sscanf(p, "D %127s", name);
			const std::vector<uint8_t> o = ReadFile(dumps + "/" + name + ".scr");
			const int rows = hires ? height * 2 : height;
			const size_t n = (size_t)pitch * rows;
			std::vector<uint8_t> port(n, (uint8_t)fill), excl(n, 0);
			std::vector<const UiCanvas *> layers;
			std::vector<std::unique_ptr<DialogueTextPainter>> texts;
			if (ts.pending)
			{
				// render_text_ui (dialogue_ui.c:971-1047) at frame scale 0: the timed
				// message first, then the choice panel or the current segment.
				ts.pending = false;
				haveChoice = false;
				if (ts.tm != 0)
				{
					std::unique_ptr<DialogueTextPainter> t(new DialogueTextPainter());
					t->SetAssets(ua); t->screenPitch = pitch; t->screenHeight = height; t->hires = hires;
					if (ts.timer >= 1) t->RenderActiveTimedMessage(ts.tml, ts.tm, ts.inv != 0, ts.ay, ts.vy);
					texts.push_back(std::move(t));
				}
				if (ts.ctx != 0)
				{
					std::unique_ptr<DialogueTextPainter> t(new DialogueTextPainter());
					t->SetAssets(ua); t->screenPitch = pitch; t->screenHeight = height; t->hires = hires;
					if (ts.gate == 0x6ffff)
					{
						t->RenderDialogueTextPanel(ts.segs, ts.ctx, ts.mode10);
						choicePainter = *t;
						haveChoice = true;
					}
					else
					{
						if (ts.mode == 1) { printf("? the topic menu (render_choice_text_panel) is not ported\n"); bad++; }
						if (ts.seg >= 0 && ts.seg < (int)ts.segs.size()) t->RenderTextUiSegment(ts.segs[(size_t)ts.seg], ts.inv != 0, ts.ay, ts.vy, ts.vh);
					}
					texts.push_back(std::move(t));
				}
				for (const auto &t : texts) { layers.push_back(t.get()); for (const std::string &pr : t->Problems()) problems.push_back(pr); }
			}
			for (const UiCanvas *b : layers)
			{
				const std::vector<uint8_t> &px = b->Pixels(), &mk = b->Mask();
				for (int y = 0; y < b->Height(); y++)
					for (int x = 0; x < b->Width(); x++)
					{
						const int X = b->Left() + x, Y = (hires ? b->Top() * 2 : b->Top()) + y;
						if (X < 0 || X >= pitch || Y < 0 || Y >= rows) continue;
						const size_t i = (size_t)y * b->Width() + x;
						if (mk[i] == 1) port[(size_t)Y * pitch + X] = px[i];
					}
			}
			for (const auto &bp : boxes)
			{
				const MessageBoxPainter &b = *bp;
				int sx, sy, sw, sh;
				b.ShadowRect(sx, sy, sw, sh);
				if (sw > 0)
					for (int y = sy - 4; y < sy + sh + 4; y++)
						for (int x = sx - 4; x < sx + sw + 4; x++)
						{
							if (x >= sx && x < sx + sw && y >= sy && y < sy + sh) continue;
							if (x < 0 || x >= pitch || y < 0 || y >= height) continue;
							for (int k = 0; k < (hires ? 2 : 1); k++) excl[(size_t)(hires ? y * 2 + k : y) * pitch + x] = 1;
						}
				const std::vector<uint8_t> &px = b.Pixels(), &mk = b.Mask();
				for (int y = 0; y < b.Height(); y++)
					for (int x = 0; x < b.Width(); x++)
					{
						const int X = b.Left() + x, Y = (hires ? b.Top() * 2 : b.Top()) + y;
						if (X < 0 || X >= pitch || Y < 0 || Y >= rows) continue;
						const size_t i = (size_t)y * b.Width() + x, j = (size_t)Y * pitch + X;
						if (mk[i] == 1) { port[j] = px[i]; excl[j] = 0; }
						else if (mk[i] >= 0x20) port[j] = pack.ShadeTables()[((size_t)(mk[i] - 0x20) << 8) | port[j]];
					}
			}
			long diff = -1, excluded = 0;
			if (o.size() == 16 + n)
			{
				diff = 0;
				for (size_t i = 0; i < n; i++) { if (excl[i]) { excluded++; continue; } if (o[16 + i] != port[i]) diff++; }
			}
			dumpsCompared++;
			pixels += (long)n - excluded;
			excludedTotal += excluded;
			if (diff != 0) dumpsBad++;
			differing += diff < 0 ? (long)n : diff;
			printf("PIX %s %ld/%ld differ (%ld shadow pixels not compared)%s\n", name, diff < 0 ? (long)n : diff, (long)n - excluded, excluded,
				diff < 0 ? " (no oracle dump of that size)" : "");
			if (diff > 0)
			{
				int shown = 0;
				for (int y = 0; y < rows && shown < 12; y++)
					for (int x = 0; x < pitch && shown < 12; x++)
					{
						const size_t i = (size_t)y * pitch + x;
						if (!excl[i] && o[16 + i] != port[i]) { printf("  at %d,%d original %u port %u\n", x, y, o[16 + i], port[i]); shown++; }
					}
			}
			if (!ppm.empty() && o.size() == 16 + n)
			{
				WritePpm(ppm + "/" + name + ".port.ppm", port.data(), pitch, rows, pal);
				WritePpm(ppm + "/" + name + ".orig.ppm", o.data() + 16, pitch, rows, pal);
				std::vector<uint8_t> dm(n);
				for (size_t i = 0; i < n; i++) dm[i] = excl[i] ? 1 : (o[16 + i] != port[i] ? 0xff : 0);
				std::vector<Colour> bw(256, Colour{ 0, 0, 0 }); bw[1] = Colour{ 40, 40, 90 }; bw[0xff] = Colour{ 255, 0, 255 };
				WritePpm(ppm + "/" + name + ".diff.ppm", dm.data(), pitch, rows, bw);
			}
			continue;
		}
		if (p[0] == 'H' && p[1] == ' ') continue;            // the original's hit points: run.sh diffs them
		if (!strncmp(p, "SAVEFILE", 8) || !strncmp(p, "R ", 2) || !strncmp(p, "STR", 3) || !strncmp(p, "SNAPSHOT", 8)) { printf("%s\n", p); continue; }
	}
	for (const auto &b : boxes) for (const std::string &pr : b->Problems()) problems.push_back(pr);
	{
		std::vector<std::string> seen;
		for (const std::string &pr : problems)
		{
			bool dup = false;
			for (const std::string &q : seen) if (q == pr) dup = true;
			if (!dup) { seen.push_back(pr); printf("PROBLEM %s\n", pr.c_str()); }
		}
	}
	printf("SUMMARY dumps %d (%d with differences), pixels %ld compared, %ld differ, %ld shadow pixels not compared; table/font checks %d bad\n",
		dumpsCompared, dumpsBad, pixels, differing, excludedTotal, bad);
	return (dumpsBad == 0 && bad == 0) ? 0 : 1;
}
