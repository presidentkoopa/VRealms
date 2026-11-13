//
// popdraw_diff -- run a popprobe script through THE PORT (roth_invscreen's
// rules, roth_invdraw's panel, roth_popupdraw's popup) and compare the popup's
// picture, byte for byte, with the original's framebuffer dumps.
//
//   popdraw_diff <retail DATA folder> <oracle log> <script> <oracle dump dir> [ppm dir]
//
// The oracle log is what popprobe.c wrote when the same script ran in the
// ORIGINAL (ROTH.C, headless). Its "S initial" block seeds the port, its "X"
// lines give the font (for this test only), "EX" the frame clock the popup
// opened on. Then the script runs on the port, the queue handed out in the
// same order as the original's loops read it (one entry per key read: a film
// boundary's, or each read of the modal loop's drain), and at each dump the
// popup's rectangle is compared with the original's screen: "qd" the window
// as the back buffer holds it, "qp" the picture the screen shows (with the
// topic menu). The popup's state and the hit tests are printed in the probe's
// format (run.sh diffs them).
//
// Writes nothing but stdout (and, with a ppm dir, PPM pictures -- retail art:
// never into the repo).
//

#include "../../src/roth/roth_gamecore.h"
#include "../../src/roth/roth_dialogue.h"
#include "../../src/roth/roth_invscreen.h"
#include "../../src/roth/roth_invdraw.h"
#include "../../src/roth/roth_popupdraw.h"
#include "../../src/roth/roth_install.h"
#include "../../src/roth/roth_das.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

struct Host : DialogueUi, InventorySink
{
	InventoryScreen *screen = nullptr;
	void RefreshPanel() override { screen->RefreshPanel(); }
	void ClearTabCursors() override { screen->ClearTabCursors(); }
	bool PanelCloseRequested() override { return screen->PanelCloseRequested(); }
	void SetLatch80b2c() override { screen->RequestClose(); }
	int32_t InventoryPanelOpen() override { return screen->g_inventory_panel_open; }
	int32_t InspectPopupActive() override { return screen->g_inspect_popup_active; }
	void ArmInventoryPanelExit() override { screen->ArmExit(); }
};

struct World
{
	Dbase100 db;
	GameCore core;
	Host host;
	InventoryScreen screen;
	InventoryPanelPainter panel;
	InspectPopupView view;
};

std::vector<uint8_t> Hex(const char *p)
{
	std::vector<uint8_t> v;
	unsigned x;
	int n;
	while (sscanf(p, " %2x%n", &x, &n) == 1) { v.push_back((uint8_t)x); p += n; }
	return v;
}

struct ExeBytes { uint32_t va = 0; std::vector<uint8_t> b; };
struct Ex { unsigned tick = 0, last = 0; };

bool Seed(World &w, const std::string &oraclePath, std::vector<ExeBytes> &exe, std::string &tables, std::vector<Ex> &ex)
{
	FILE *f = fopen(oraclePath.c_str(), "r");
	if (!f) return false;
	std::vector<char> line(1 << 16);
	bool inInitial = false, seeded = false;
	Dbase100InterpState &s = w.core.Interp().State();
	Inventory &inv = w.core.Items();
	inv.Reset();
	while (fgets(line.data(), (int)line.size(), f))
	{
		const char *p = line.data();
		if (!strncmp(p, "S initial", 9)) { inInitial = true; continue; }
		if (!strncmp(p, "X ", 2))
		{
			ExeBytes e; unsigned va = 0, n = 0;
			if (sscanf(p, "X %x %u:", &va, &n) == 2) { e.va = va; e.b = Hex(strchr(p, ':') + 1); exe.push_back(e); }
			continue;
		}
		if (!strncmp(p, "EX ", 3)) { Ex e; sscanf(p, "EX tick=%u last=%u", &e.tick, &e.last); ex.push_back(e); continue; }
		if (!strncmp(p, "TABLES ", 7)) { tables = p; continue; }
		if (!inInitial) continue;
		if (p[0] == 'E' || p[0] == '>') { inInitial = false; continue; }
		if (!strncmp(p, " flags=", 7))
		{
			p += 7; size_t i = 0;
			while (p[0] && p[1] && p[0] != '\n' && i < s.g_dbase100_record_bitmap.size())
			{ unsigned v = 0; sscanf(p, "%2x", &v); s.g_dbase100_record_bitmap[i++] = (uint8_t)v; p += 2; }
			seeded = true;
		}
		else if (!strncmp(p, " inv=", 5))
		{
			const char *q = strchr(p, ':'); q = q ? q + 1 : p;
			int idx; unsigned id, qty; int used = 0;
			std::vector<uint8_t> raw(Inventory::SAVE_BYTES, 0);
			while (sscanf(q, " %d:%u.%u%n", &idx, &id, &qty, &used) == 3)
			{
				raw[(size_t)idx * 4] = (uint8_t)id; raw[(size_t)idx * 4 + 1] = (uint8_t)(id >> 8);
				raw[(size_t)idx * 4 + 2] = (uint8_t)qty; raw[(size_t)idx * 4 + 3] = (uint8_t)(qty >> 8);
				q += used;
			}
			inv.Deserialize(raw.data(), raw.size(), nullptr);
		}
		else if (!strncmp(p, " hands", 6))
		{
			int L, R, as, ai, au, last; unsigned lock; int lt;
			if (sscanf(p, " hands L=%d R=%d adam=%d/%d auto=%d last=%d lock=%x/%d", &L, &R, &as, &ai, &au, &last, &lock, &lt) == 8)
			{
				inv.SetLeftHand(L);
				inv.SetRightHand(R, nullptr);
				inv.RestoreActiveHeldItem(w.db, nullptr);
				inv.SetLastItem(last);
			}
		}
	}
	fclose(f);
	return seeded;
}

const uint8_t *ExeAt(const std::vector<ExeBytes> &exe, uint32_t va, size_t n)
{
	for (const ExeBytes &e : exe)
		if (va >= e.va && va + n <= e.va + e.b.size()) return e.b.data() + (va - e.va);
	return nullptr;
}

uint32_t Fnv(const uint8_t *p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }

void WritePpm(const std::string &path, const uint8_t *px, int w, int h, const std::vector<Colour> &pal)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) { const Colour &c = pal[px[i]]; fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
	fclose(f);
}

// The queue, as popprobe.c holds it.
enum { QK, QDUMP, QPRES, QMOUSE, QHIT, QWAIT, QBTN, QSTATE, QHOLD };
struct Qe { int kind = 0; int a = 0, b = 0, c = 0; std::string name; };

struct Run
{
	World *w = nullptr;
	std::string dumps, ppm;
	std::vector<Colour> pal;
	std::vector<Qe> q;
	size_t qi = 0;
	int held = 0, mx = 0, my = 0;
	std::string presentPending;
	uint32_t presentSeen = 0;
	int compared = 0, bad = 0;
	long pixels = 0, differing = 0;
	std::vector<uint8_t> lastClosed;            // the original's screen at the last dump with no popup
	int restoredChecks = 0, restoredBad = 0;
	uint16_t tick = 0;

	void State(const char *why);
	void Compare(const std::string &name, const UiCanvas &c);
	void HitGrid(int step);
	uint8_t Dequeue();
};

void Run::State(const char *why)
{
	InspectPopupView &v = w->view;
	InspectPopupPainter &p = v.painter;
	const Dbase100InterpState &s = w->core.Interp().State();
	const Host &h = w->host;
	// The geometry words stay in the original's globals after the popup; the
	// port's painter keeps them too.
	int wd[12];
	p.Words(wd);
	const bool ever = wd[6] != 0;
	printf("P %s active=%d state=%d content=%d w=%d h=%d words", why, w->screen.g_inspect_popup_active, v.State(),
		ever ? wd[1] * 640 + wd[0] : -1, wd[2], wd[3]);
	for (int i = 0; i < 12; i++) printf(" %d", wd[i]);
	printf(" latch=* marks=%d mode=%d sel=%d lines=%d origin=%d,%d bbox=%d,%d,%d reveal=%d alt=%d page=%d/%d pend=%d info=%d exit=%d busy=%d ctx=%d gate=%x\n",
		w->screen.PopupMarks(), h.g_choice_interaction_mode, s.g_choice_selected_index, h.g_choice_line_count, p.ChoiceX(), p.ChoiceY(),
		p.ChoiceX0(), p.ChoiceX1(), p.ChoiceHeight(), p.choiceReveal, h.TopicColor(), w->screen.g_inspect_current_page, p.PageCount(), p.PagePending(),
		v.DragState(), w->screen.g_popup_exit_request, s.g_dialogue_busy_flag, s.g_active_dialogue_context, (unsigned)s.g_move_freeze_gate);
	printf(" cells");
	for (int i = 0; i < 6; i++) printf(" [%u,%u,%u,%u]", p.Cells()[i].y, p.Cells()[i].h, p.Cells()[i].x, p.Cells()[i].w);
	printf("\n film boundaries=%d decodes=%d\n", v.Boundaries(), v.film.Decodes());
	const std::vector<TextLine> &tl = h.TopicLines();
	for (int i = 0; i < h.g_choice_line_count && i < (int)tl.size(); i++)
	{
		printf(" TL %d %d %d %d %d ", i, tl[(size_t)i].x0, tl[(size_t)i].x1, tl[(size_t)i].height, tl[(size_t)i].len);
		for (int k = 0; k < tl[(size_t)i].len && k < (int)tl[(size_t)i].text.size(); k++) printf("%02x", tl[(size_t)i].text[(size_t)k]);
		printf("\n");
	}
	printf(" label ");
	for (int i = 0; i < 0x40; i++) printf("%02x", w->panel.g_label[i]);
	printf("\n");
	{
		const int rh = w->core.Items().RightHand();
		printf(" RH slot=%d id=%d dirty=%u\n", rh, rh >= 0 ? (int)(int16_t)w->core.Items().Slot(rh).id : 0, w->screen.g_inventory_dirty_flags);
	}
}

void Run::Compare(const std::string &name, const UiCanvas &c)
{
	const std::vector<uint8_t> o = ReadFile(dumps + "/" + name + ".scr");
	if (o.size() < 16) { printf("PIX %s no oracle dump\n", name.c_str()); bad++; compared++; return; }
	const int pitch = (int)(o[4] | (o[5] << 8)), rows = (int)(o[8] | (o[9] << 8));
	const uint8_t *scr = o.data() + 16;
	if (!w->view.painter.IsOpen())
	{
		// No popup on the port's side. The original's screen: the inventory
		// panel's rectangle must be the port's panel picture (the panel is
		// redrawn after a popup, inventory.c:1841-1844), and everything else
		// what it was at the last dump before the popup opened
		// (free_inspect_popup_and_redraw put it back).
		std::vector<uint8_t> now(scr, scr + (size_t)pitch * rows);
		const int ax = (pitch - 0x130) >> 1, ay = rows >> 1;
		long dp = 0, dr = 0, np = 0;
		const std::vector<uint8_t> &pp = w->panel.Pixels();
		const bool panelUp = w->screen.IsOpen();
		for (int y = 0; y < rows; y++)
			for (int x = 0; x < pitch; x++)
			{
				const size_t i = (size_t)y * pitch + x;
				const bool inPanel = panelUp && x >= ax && x < ax + 0x120 && y >= ay && y < ay + 0x50;
				if (inPanel) { np++; if (pp[(size_t)(y - ay) * 0x120 + (x - ax)] != now[i]) dp++; }
				else if (!lastClosed.empty() && now[i] != lastClosed[i]) dr++;
			}
		restoredChecks++;
		if (dp != 0 || dr != 0) restoredBad++;
		printf("RESTORED %s: no popup; the panel %ld/%ld px differ from the port's panel picture; outside it %ld px differ from the screen before the popup%s\n",
			name.c_str(), dp, np, dr, lastClosed.empty() ? " (no earlier screen)" : "");
		lastClosed = now;
		return;
	}
	const int x0 = c.Left(), y0 = c.Top(), cw = c.Width(), ch = c.Height();
	long diff = 0, n = 0;
	int shown = 0;
	std::vector<uint8_t> orig((size_t)cw * ch, 0), dm((size_t)cw * ch, 0);
	for (int y = 0; y < ch; y++)
		for (int x = 0; x < cw; x++)
		{
			const int sx = x0 + x, sy = y0 + y;
			if (sx < 0 || sy < 0 || sx >= pitch || sy >= rows) continue;
			const size_t i = (size_t)y * cw + x;
			const uint8_t ov = scr[(size_t)sy * pitch + sx];
			orig[i] = ov;
			n++;
			if (c.Mask()[i] != 1 || c.Pixels()[i] != ov)
			{
				diff++; dm[i] = 0xff;
				if (shown < 10) { printf("  at %d,%d original %u port %u%s\n", sx, sy, ov, c.Pixels()[i], c.Mask()[i] != 1 ? " (not drawn)" : ""); shown++; }
			}
		}
	compared++;
	pixels += n;
	differing += diff;
	if (diff != 0) bad++;
	printf("PIX %s %ld/%ld differ (window %d,%d %dx%d)\n", name.c_str(), diff, n, x0, y0, cw, ch);
	if (!ppm.empty())
	{
		WritePpm(ppm + "/" + name + ".port.ppm", c.Pixels().data(), cw, ch, pal);
		WritePpm(ppm + "/" + name + ".orig.ppm", orig.data(), cw, ch, pal);
		std::vector<Colour> bw(256, Colour{ 0, 0, 0 }); bw[0xff] = Colour{ 255, 0, 255 };
		WritePpm(ppm + "/" + name + ".diff.ppm", dm.data(), cw, ch, bw);
	}
}

void Run::HitGrid(int step)
{
	InspectPopupPainter &p = w->view.painter;
	if (step <= 0) step = 3;
	const int x0 = p.WinX() - 6, y0 = p.WinY() - 6, x1 = x0 + (p.WinW() + 1) + 12, y1 = y0 + p.WinH() + 12;
	const Dbase100InterpState &s = w->core.Interp().State();
	const Host &h = w->host;
	printf("HG %d %d %d %d step=%d\n", x0, y0, x1, y1, step);
	for (int y = y0; y <= y1; y += step)
		for (int x = x0; x <= x1; x += step)
		{
			int row = -1;
			const int r = p.HitTest(x, y, h.g_choice_interaction_mode, s.g_dialogue_busy_flag != 0, false, h.TopicLines(), h.g_choice_line_count, &row);
			int mk = -1;
			if (h.g_choice_interaction_mode == 1 && s.g_dialogue_busy_flag == 0 && row >= 0 && h.TopicLines()[(size_t)row].Color() != w->host.markerSelected) mk = row;
			if (r) printf("H %d %d %d %d\n", x, y, r, mk);
		}
}

uint8_t Run::Dequeue()
{
	while (qi < q.size())
	{
		Qe &e = q[qi];
		if (e.kind == QK) { qi++; printf("K %d at boundary %d decodes %d\n", e.a, w->view.Boundaries(), w->view.film.Decodes()); return (uint8_t)e.a; }
		if (e.kind == QDUMP) { qi++; printf("D %s\n", e.name.c_str()); Compare(e.name, w->view.painter); State(e.name.c_str()); continue; }
		if (e.kind == QPRES) { qi++; presentPending = e.name; presentSeen = w->view.painter.Presents(); continue; }
		if (e.kind == QSTATE) { qi++; State("q"); continue; }
		if (e.kind == QMOUSE) { qi++; mx = e.a; my = e.b; w->view.SetPointer(mx, my, held); continue; }
		if (e.kind == QBTN) { qi++; w->view.Press(e.a == 1 ? 1 : 2); return 0; }
		if (e.kind == QHOLD) { qi++; held = e.a; w->view.SetPointer(mx, my, held); continue; }
		if (e.kind == QHIT) { qi++; HitGrid(e.a); continue; }
		if (e.kind == QWAIT)
		{
			if (e.c == 0) { e.c = 1; e.b = e.a; }
			if (e.b <= 0) { qi++; continue; }
			e.b--;
			return 0;
		}
		qi++;
	}
	return 1;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 5) { fprintf(stderr, "popdraw_diff <DATA folder> <oracle log> <script> <oracle dump dir> [ppm dir]\n"); return 2; }
	const std::string dir = argv[1];
	World w;
	std::vector<uint8_t> d100 = ReadFile(dir + "/DBASE100.DAT");
	if (d100.empty() || !w.db.Load(d100.data(), d100.size())) { fprintf(stderr, "no DBASE100.DAT in %s\n", dir.c_str()); return 2; }
	std::vector<uint8_t> d200 = ReadFile(dir + "/DBASE200.DAT");
	std::vector<uint8_t> d300 = ReadFile(dir + "/DBASE300.DAT");
	std::vector<uint8_t> icons = ReadFile(dir + "/DATA/ICONS.ALL");
	if (icons.empty() || d300.empty()) { fprintf(stderr, "no DATA/ICONS.ALL or DBASE300.DAT in %s\n", dir.c_str()); return 2; }
	w.host.text = ReadFile(dir + "/DBASE400.DAT");
	w.host.screen = &w.screen;
	w.core.Attach(&w.db, &w.host, &w.host);
	w.host.Bind(&w.core.Interp(), &w.db);
	w.screen.Bind(&w.core, &w.host, &w.db);
	w.view.Bind(&w.screen, &w.host, &w.core);
	w.core.Interp().State().g_choice_selected_index = -1;

	Install inst;
	if (!inst.Open(dir.c_str())) { fprintf(stderr, "install: %s\n", inst.Error().c_str()); return 2; }
	std::vector<uint8_t> packBytes = ReadFile(inst.PackFile(inst.PackFor("STUDY1").c_str()));
	Pack pack;
	if (packBytes.empty() || !pack.Load(packBytes.data(), packBytes.size()) || pack.ShadeTables() == nullptr || pack.BlendTable() == nullptr)
	{ fprintf(stderr, "STUDY1's pack has no shade/blend tables\n"); return 2; }

	std::vector<ExeBytes> exe;
	std::string tables;
	std::vector<Ex> ex;
	if (!Seed(w, argv[2], exe, tables, ex)) { fprintf(stderr, "could not seed from %s\n", argv[2]); return 2; }
	int checks = 0;
	{
		char mine[96];
		snprintf(mine, sizeof mine, "TABLES blend=%08x ramp=%08x", Fnv(pack.BlendTable(), 0x10000), Fnv(pack.ShadeTables(), 0x4000));
		const bool same = tables.compare(0, strlen(mine), mine) == 0;
		printf("PACK tables %s\n", same ? "identical" : "DIFFER");
		if (!same) checks++;
	}
	RothFont font;
	if (const uint8_t *f = ExeAt(exe, 0x70f12 - (uint32_t)ROTH_FONT_BEFORE, ROTH_FONT_BEFORE + ROTH_FONT_AFTER))
	{
		font.bytes.assign(f, f + ROTH_FONT_BEFORE + ROTH_FONT_AFTER);
		font.desc = ROTH_FONT_BEFORE;
		const uint32_t h = RothFontHash(font.bytes.data(), font.bytes.size());
		printf("FONT hash %08x: %s\n", h, h == ROTH_FONT_HASH ? "matches" : "DIFFERS");
		if (h != ROTH_FONT_HASH) checks++;
	}
	// The executable bytes the popup reads: the marker colour.
	if (const uint8_t *m = ExeAt(exe, 0x7675d, 1))
	{
		printf("EXE marker colour %02x (port %02x)\n", m[0], w.view.painter.markerSelected);
		if (m[0] != w.view.painter.markerSelected) checks++;
	}

	InvPanelAssets a;
	a.iconsAll = &icons;
	a.dbase200 = &d200;
	a.shade = pack.ShadeTables();
	a.blend = pack.BlendTable();
	a.font = font.ok() ? &font : nullptr;
	a.readLine = [&w](uint8_t *dest, uint32_t maxlen, uint32_t off) { return w.host.ReadNextDialogueLine(dest, maxlen, off, 0); };
	a.labelText = [&w](int32_t id) { return ItemLabelText(w.db, id, w.core.InvSink()); };
	w.panel.Bind(&w.screen, &w.core, &w.db, a, false);
	{
		UiAssets ua;
		ua.iconsAll = &icons; ua.shade = pack.ShadeTables(); ua.blend = pack.BlendTable(); ua.font = font.ok() ? &font : nullptr;
		w.view.painter.SetAssets(ua);
	}
	w.view.painter.dbase300 = [&d300](uint32_t res) {
		std::vector<uint8_t> out;
		const uint64_t off = (uint64_t)res << 3;
		if (off + 4 > d300.size()) return out;
		const uint32_t n = d300[off] | (d300[off + 1] << 8) | (d300[off + 2] << 16) | ((uint32_t)d300[off + 3] << 24);
		if (off + 4 + n > d300.size()) return out;
		out.assign(d300.begin() + (long)off + 4, d300.begin() + (long)(off + 4 + n));
		return out;
	};
	w.view.textUi = [&w](int scale) { w.host.TickTextUi(scale); };

	Run run;
	run.w = &w;
	run.dumps = argv[4];
	run.ppm = argc > 5 ? argv[5] : "";
	run.pal = pack.Palette();
	size_t exIndex = 0;

	FILE *sf = fopen(argv[3], "r");
	if (!sf) { fprintf(stderr, "no script %s\n", argv[3]); return 2; }
	char line[1024];
	while (fgets(line, sizeof line, sf))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#') continue;
		char cmd[64] = {}; int x[8] = {};
		if (sscanf(line, "%63s %i %i %i %i %i %i %i %i", cmd, &x[0], &x[1], &x[2], &x[3], &x[4], &x[5], &x[6], &x[7]) < 1) continue;
		const std::string c = cmd;
		printf("> %s\n", line);
		const char *rest = strchr(line, ' ');
		const std::string name = rest ? rest + 1 : "unnamed";
		if (c == "env" || c == "exe" || c == "fill" || c == "font") {}
		else if (c == "give") w.core.Give(x[0], nullptr);
		else if (c == "examined")
		{
			const uint8_t r = w.db.ItemRuntime(x[0]);
			w.db.SetItemRuntime(x[0], x[1] ? (uint8_t)(r | 0x80) : (uint8_t)(r & 0x7f));
		}
		else if (c == "open") w.screen.Open();
		else if (c == "tab") { for (int t = 0; t < 5; t++) if (TabCategory(t) == x[0]) w.screen.ClickTab(t); }
		else if (c == "dump") { printf("D %s\n", name.c_str()); run.Compare(name, w.view.painter); run.State(name.c_str()); }
		else if (c == "state") run.State("cmd");
		else if (c == "q") { Qe e; e.kind = QK; e.a = x[0]; run.q.push_back(e); }
		else if (c == "qd") { Qe e; e.kind = QDUMP; e.name = name; run.q.push_back(e); }
		else if (c == "qp") { Qe e; e.kind = QPRES; e.name = name; run.q.push_back(e); }
		else if (c == "qs") { Qe e; e.kind = QSTATE; run.q.push_back(e); }
		else if (c == "qm") { Qe e; e.kind = QMOUSE; e.a = x[0]; e.b = x[1]; run.q.push_back(e); }
		else if (c == "qb") { Qe e; e.kind = QBTN; e.a = x[0]; run.q.push_back(e); }
		else if (c == "qhold") { Qe e; e.kind = QHOLD; e.a = x[0]; run.q.push_back(e); }
		else if (c == "qh") { Qe e; e.kind = QHIT; e.a = x[0]; run.q.push_back(e); }
		else if (c == "qw") { Qe e; e.kind = QWAIT; e.a = x[0]; run.q.push_back(e); }
		else if (c == "cell") w.screen.ClickCell(x[0], InventoryScreen::PRIMARY);   // dispatch_dialogue_ui_action(0x1c + cell, primary)
		else if (c == "examine")
		{
			// The frame update_inventory_screen runs next: its tail opens the popup.
			run.mx = x[0]; run.my = x[1]; run.held = 0;
			Ex e = exIndex < ex.size() ? ex[exIndex++] : Ex();
			run.tick = (uint16_t)e.tick;
			w.screen.Post();
			if (w.screen.PopupUp())
			{
				const int32_t id = w.screen.PopupItem();
				const DbItem *rec = w.db.Item(id);
				const uint64_t off = (uint64_t)rec->closeup << 3;
				const uint32_t n = d300[off] | (d300[off + 1] << 8) | (d300[off + 2] << 16) | ((uint32_t)d300[off + 3] << 24);
				const uint8_t *icon = nullptr; size_t iconSize = 0;
				if (rec->icon != 0)
				{
					const uint64_t io = (uint64_t)rec->icon << 3;
					const uint32_t in = d200[io] | (d200[io + 1] << 8) | (d200[io + 2] << 16) | ((uint32_t)d200[io + 3] << 24);
					icon = d200.data() + io + 4; iconSize = in;
				}
				w.view.SetPointer(run.mx, run.my, 0);
				w.view.Begin(d300.data() + off + 4, n, icon, iconSize, w.panel.g_label, sizeof w.panel.g_label, (uint16_t)e.last);
				// The film: one timer tick and one boundary per key read.
				bool eos = false;
				while (w.view.InFilm())
				{
					run.tick++;
					if (!w.view.FilmBoundary(run.tick)) { eos = true; break; }
					if (!run.presentPending.empty() && w.view.painter.Presents() != run.presentSeen)
					{ printf("D %s\n", run.presentPending.c_str()); run.Compare(run.presentPending, w.view.painter.Presented()); run.State(run.presentPending.c_str()); run.presentPending.clear(); }
					if (w.view.KeyWanted() && !w.view.FilmKey(run.Dequeue())) { eos = true; break; }
					if (w.view.FilmRanOut()) break;
				}
				printf("F end eos=%d boundaries %d decodes %d state %d\n", eos ? 1 : 0, w.view.Boundaries(), w.view.film.Decodes(), w.view.State());
				w.view.FinishFilm(eos, run.tick);
				while (w.view.InModal())
				{
					run.tick++;
					w.view.ModalPassBegin(run.tick);
					if (!run.presentPending.empty() && w.view.painter.Presents() != run.presentSeen)
					{ printf("D %s\n", run.presentPending.c_str()); run.Compare(run.presentPending, w.view.painter.Presented()); run.State(run.presentPending.c_str()); run.presentPending.clear(); }
					while (w.view.ModalKey(run.Dequeue())) {}
					w.view.ModalPassEnd();
				}
			}
			// update_inventory_screen's close test (inventory.c:1837-1840).
			if (w.screen.g_ui_close_request != 0 || w.screen.g_inventory_panel_open == 0) w.screen.Close();
			printf("R examine queue %d/%d active=%d\n", (int)run.qi, (int)run.q.size(), w.screen.g_inspect_popup_active);
			run.q.clear(); run.qi = 0;
			run.State("after");
		}
		else printf("? unknown command\n");
	}
	fclose(sf);
	for (const std::string &pr : w.view.painter.Problems()) printf("PROBLEM %s\n", pr.c_str());
	printf("SUMMARY dumps %d (%d with differences), pixels %ld compared, %ld differ; restored %d (%d differ); checks %d bad\n",
		run.compared, run.bad, run.pixels, run.differing, run.restoredChecks, run.restoredBad, checks);
	return (run.bad == 0 && run.restoredBad == 0 && checks == 0) ? 0 : 1;
}
