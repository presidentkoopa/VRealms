//
// invdraw_diff -- run an invprobe script through THE PORT (roth_invscreen's
// rules + roth_invdraw's painter) and compare the panel picture, byte for
// byte, with the original's framebuffer dumps.
//
//   invdraw_diff <retail DATA folder> <oracle log> <script> <oracle dump dir> [png dir]
//
// The oracle log is what invprobe.c wrote when the same script ran in the
// ORIGINAL (ROTH.C, headless). Its "S initial" block seeds the port (flags,
// carried slots, hands), its "X" lines are the executable's bytes (the font,
// for this test only, and the tables roth_invdraw.cpp transcribes, which are
// compared), its "TABLES" line the hashes of the pack tables the original
// loaded (compared with the ones read here from the map's pack). Then every
// command runs on the port, and at each "dump" the picture is compared with
// <oracle dump dir>/<name>.pan and the drawing state, label and hotspots are
// printed in the probe's own format (run.sh diffs those lines).
//
// Writes nothing but stdout (and, with a png dir, PPM pictures of both sides
// and of the difference -- retail art: never into the repo).
//

#include "../../src/roth/roth_gamecore.h"
#include "../../src/roth/roth_dialogue.h"
#include "../../src/roth/roth_invscreen.h"
#include "../../src/roth/roth_invdraw.h"
#include "../../src/roth/roth_install.h"
#include "../../src/roth/roth_das.h"
#include "../../src/roth/roth_invmouse.h"
#include "../../src/roth/roth_invhand.h"
#include "../../src/roth/roth_panelmap.h"

#include <stdio.h>
#include <memory>
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
	InventoryPanelPainter painter;
};

std::vector<uint8_t> Hex(const char *p)
{
	std::vector<uint8_t> v;
	unsigned x;
	int n;
	while (sscanf(p, " %2x%n", &x, &n) == 1) { v.push_back((uint8_t)x); p += n; }
	return v;
}

// The probe's "X va n: bytes" lines.
struct ExeBytes { uint32_t va = 0; std::vector<uint8_t> b; };

bool Seed(World &w, const std::string &oraclePath, std::vector<ExeBytes> &exe, std::string &tables, int &ax, int &ay)
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
		if (!strncmp(p, "TABLES ", 7)) { tables = p; continue; }
		if (!strncmp(p, "ENV ", 4))
		{
			int pitch = 0, h = 0;
			if (sscanf(p, "ENV pitch=%d h=%d", &pitch, &h) == 2) { ax = (pitch - 0x130) >> 1; ay = h >> 1; }   // compute_viewport_half_extents, renderer.c:9617
			continue;
		}
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

// The transcribed tables against the running original's bytes.
int CheckTables(const std::vector<ExeBytes> &exe)
{
	int bad = 0, checked = 0;
	auto u16 = [](const uint8_t *p) { return (unsigned)(p[0] | (p[1] << 8)); };
	auto u32 = [](const uint8_t *p) { return (unsigned)(p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24)); };
	if (const uint8_t *p = ExeAt(exe, 0x71208, 13 * 4))
		for (int i = 0; i < 13; i++, checked++)
			if (u16(p + i * 4) != InvPanelTables::slotXY[i][0] || u16(p + i * 4 + 2) != InvPanelTables::slotXY[i][1]) { printf("TABLE slotXY[%d] differs\n", i); bad++; }
	if (const uint8_t *p = ExeAt(exe, 0x712da, 40))
		for (int i = 0; i < 5; i++, checked++)
			if (u32(p + i * 8) != InvPanelTables::tabDas[i][0] || u32(p + i * 8 + 4) != InvPanelTables::tabDas[i][1]) { printf("TABLE tabDas[%d] differs\n", i); bad++; }
	if (const uint8_t *p = ExeAt(exe, 0x712d5, 5))
		for (int i = 0; i < 5; i++, checked++) if (p[i] != InvPanelTables::tabX[i]) { printf("TABLE tabX[%d] differs\n", i); bad++; }
	if (const uint8_t *p = ExeAt(exe, 0x7123c, 5))
		for (int i = 0; i < 5; i++, checked++) if (p[i] != InvPanelTables::tabCategory[i]) { printf("TABLE tabCategory[%d] differs\n", i); bad++; }
	if (const uint8_t *p = ExeAt(exe, 0x71255, 128))
		for (int i = 0; i < 32; i++, checked++) if (u32(p + i * 4) != InvPanelTables::cursorCycle[i]) { printf("TABLE cursorCycle[%d] differs\n", i); bad++; }
	if (const uint8_t *p = ExeAt(exe, 0x7675d, 12))
	{
		checked += 4;
		if (p[0] != InvPanelTables::markerSelected) { printf("TABLE markerSelected differs\n"); bad++; }
		if (p[1] != InvPanelTables::leftHandColor) { printf("TABLE leftHandColor differs\n"); bad++; }
		if (p[6] != InvPanelTables::rightHandColor) { printf("TABLE rightHandColor differs\n"); bad++; }
		if (p[11] != InvPanelTables::labelColor) { printf("TABLE labelColor differs\n"); bad++; }
	}
	if (const uint8_t *p = ExeAt(exe, 0x75e2c, 8))
	{
		checked++;
		if (memcmp(p, InvPanelTables::labelFormat, 8) != 0) { printf("TABLE labelFormat differs\n"); bad++; }
	}
	printf("TABLES checked %d values, %d differ\n", checked, bad);
	return bad;
}

int EntryOf(const InventoryEntryList &l, int extra)
{
	// The appended hand entries (inventory.c:1353-1369) sit after the grid ones.
	(void)l;
	return extra;
}

void PrintHot(const std::vector<InvHotspot> &v, int ax, int ay)
{
	for (const InvHotspot &h : v) printf(" [%d,%d,%u,%u,%d]", h.x + ax, h.y + ay, h.w, h.h, h.action);
	printf(" end\n");
}

void WritePpm(const std::string &path, const uint8_t *px, int w, int h, const std::vector<Colour> &pal)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) { const Colour &c = pal[px[i]]; fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
	fclose(f);
}

// THE VR HANDS' PATH to a click (the "ptr" commands): the panel standing in the world as
// roth_interface.cpp stands it (AddWorldPicture: 0.14 map units a pixel, yaw = the player's
// + 180, BillboardBasis' axes with no tilt), a hand at 107.4 units a metre, its fingertip
// and finger made to point at the pixel's centre, its gestures run tic by tic through
// roth_invhand.h, the mouse it makes fed to roth_invmouse.h -- the same InvMouse the engine
// gives every pointer on the panel. The original runs the same panel points through its own
// handle_cursor_click (invprobe.c "ptr").
struct VRPath
{
	PanelPlane plane;
	InvHand hand;
	InvMouse mouse;
	double upm = 107.4;
	int optionsOpened = 0;
	int mapWrong = 0, events = 0;
	VRPath()
	{
		const double PXW = 0.1 * 1.4, yaw = 27.0 + 180.0, r = yaw * M_PI / 180.0;
		plane.right = PanelV3(-sin(r), cos(r), 0.0);
		plane.up = PanelV3(0, 0, 1);
		plane.front = PanelV3(cos(r), sin(r), 0.0);              // right x up: toward the player
		plane.halfw = 0x120 * PXW * 0.5; plane.halfh = 0x50 * PXW * 0.5;
		plane.centre = PanelV3(-311.0, 2044.5, 61.0);
		plane.left = 0; plane.top = 0; plane.w = 0x120; plane.h = 0x50;
		mouse.openOptions = [this] { optionsOpened = 1; };
	}
	std::unique_ptr<InvMouseDialogueUi> dlg;
	void BindDialogue(World &w)
	{
		dlg.reset(new InvMouseDialogueUi(w.host, w.core.Interp().State()));
		mouse.dlg = dlg.get();
	}
	void Print(World &w, int x, int y, int code, uint16_t shape)
	{
		Inventory &inv = w.core.Items();
		const Dbase100InterpState &st = w.core.Interp().State();
		printf("P %d %d %d shape=%x latch=%d list=%d pos=%d scroll=%d count=%d held=%d L=%d R=%d insp=%d close=%d opt=%d busy=%d"
			" gate=%x dev4=%d lastpos=%d sel=%d\n",
			x, y, code, shape, mouse.g_hover_latch, w.screen.ActiveList(), w.screen.CursorEntry(), w.screen.ScrollOffset(),
			w.screen.EntryCount(), w.screen.HeldEntry(), inv.LeftHand(), inv.RightHand(), w.screen.g_inventory_inspect_request,
			w.screen.g_ui_close_request, optionsOpened, (int)st.g_dialogue_busy_flag, (unsigned)st.g_move_freeze_gate,
			mouse.g_dev_mode_flag_4, (int)w.host.g_choice_last_pos, (int)st.g_choice_selected_index);
	}
	// The S (use false) / U key: a click with code 8 at the mouse (InvMouse::KeyClick).
	void Key(World &w, bool use)
	{
		const uint16_t shape = mouse.KeyClick(w.screen, use);
		Print(w, mouse.mouseX, mouse.mouseY, 8, shape);
	}

	static PanelV3 Unit(const PanelV3 &v) { return v * (1.0 / v.Length()); }
	// via 0: the laser from 35 cm in front and 10 cm to the right; 1: the fingertip pressed
	// onto the point; 2: the hand pointing away from the panel. The hand points with the
	// index straight and the grip open (the owner's scheme: the trigger is the original's
	// PRIMARY button, the grip its SECONDARY).
	void Aim(InvHandInput &in, int x, int y, int via, double metres)
	{
		const PanelV3 at = PanelWorldOf(plane, x + 0.5, y + 0.5);
		in.valid = true; in.haveRay = true; in.fromFinger = true;
		in.grip = 0.f; in.trigger = 0.f; in.triggerTouch = false;
		if (via == 1) { in.tip = at + plane.front * (metres * upm); in.dir = plane.front * -1.0; }
		else
		{
			in.tip = at + plane.front * (0.35 * upm) + plane.right * (0.10 * upm) - plane.up * (0.05 * upm);
			in.dir = via == 2 ? plane.front : Unit(at - in.tip);
		}
	}
	// One tic of the hand: its mouse onto the panel (the hover), and its click.
	uint16_t Tic(World &w, const InvHandInput &in, int x, int y, int via, int &clicks, uint16_t shape)
	{
		InvHandInput t = in;
		t.haveRay = in.haveRay && hand.WantsRay(in.valid, in.triggerTouch, in.trigger);   // as the engine reads the finger
		const InvHandOutput o = hand.Tick(plane, t, upm);
		if (o.pointer)
		{
			if (o.px != x || o.py != y) { mapWrong++; printf("MAP hand's mouse at %d,%d for panel point %d,%d\n", o.px, o.py, x, y); }
			shape = mouse.Hover(w.painter, w.screen, o.px, o.py);
		}
		if (o.click)
		{
			events++;
			const int ex = via == 2 ? PANEL_NOWHERE : x, ey = via == 2 ? PANEL_NOWHERE : y;
			if (o.cx != ex || o.cy != ey) { mapWrong++; printf("MAP click at %d,%d for panel point %d,%d\n", o.cx, o.cy, ex, ey); }
			clicks++;
			shape = mouse.Click(w.painter, w.screen, o.cx, o.cy, o.click);
			printf("V %s: button %d at %d,%d -> action 0x%x\n", InvHandHowName(o.how), o.click, o.cx, o.cy, mouse.lastAction);
		}
		return shape;
	}
	// The original's "ptr x y code": code 0 the hover, 1 / 2 a primary / secondary click.
	// The hand's: 1 the trigger (the index comes down, then pulls), 2 the grip squeezed;
	// with the fingertip (via 1): 1 a touch that backs off, 2 the grip while it touches.
	void Ptr(World &w, int x, int y, int code, int via)
	{
		hand.Reset();
		InvHandInput in;
		int clicks = 0;
		uint16_t shape = 0;
		if (code == 0 || via == 0 || via == 2)
		{
			Aim(in, x, y, via, 0);
			shape = Tic(w, in, x, y, via, clicks, shape);           // pointing: the mouse comes onto the point
			shape = Tic(w, in, x, y, via, clicks, shape);
			if (code == 1)
			{
				in.triggerTouch = true;                             // the index onto the trigger
				in.dir = Unit(in.dir - plane.up * 0.4);             // the curling finger swings the ray
				shape = Tic(w, in, x, y, via, clicks, shape);
				in.trigger = 0.5f; shape = Tic(w, in, x, y, via, clicks, shape);
				in.trigger = 1.0f; shape = Tic(w, in, x, y, via, clicks, shape);   // pressed
			}
			else if (code == 2)
			{
				in.grip = 0.5f; shape = Tic(w, in, x, y, via, clicks, shape);
				in.grip = 1.0f; shape = Tic(w, in, x, y, via, clicks, shape);      // closed
			}
		}
		else
		{
			Aim(in, x, y, 1, 0.10);
			shape = Tic(w, in, x, y, via, clicks, shape);           // the fingertip 10 cm in front
			Aim(in, x, y, 1, 0.01);
			shape = Tic(w, in, x, y, via, clicks, shape);           // on the panel
			shape = Tic(w, in, x, y, via, clicks, shape);
			if (code == 1) Aim(in, x, y, 1, 0.10);                  // a touch: backed off
			else in.grip = 1.0f;                                    // the grip while it touches
			shape = Tic(w, in, x, y, via, clicks, shape);
		}
		if (code != 0 && clicks != 1) { mapWrong++; printf("MAP %d click(s) for one gesture\n", clicks); }
		Print(w, x, y, code, shape);
	}
};

} // namespace

int main(int argc, char **argv)
{
	if (argc < 5) { fprintf(stderr, "invdraw_diff <DATA folder> <oracle log> <script> <oracle dump dir> [ppm dir]\n"); return 2; }
	const std::string dir = argv[1];
	const std::string dumps = argv[4];
	const std::string ppm = argc > 5 ? argv[5] : "";
	World w;
	std::vector<uint8_t> d100 = ReadFile(dir + "/DBASE100.DAT");
	if (d100.empty() || !w.db.Load(d100.data(), d100.size())) { fprintf(stderr, "no DBASE100.DAT in %s\n", dir.c_str()); return 2; }
	std::vector<uint8_t> d200 = ReadFile(dir + "/DBASE200.DAT");
	std::vector<uint8_t> icons = ReadFile(dir + "/DATA/ICONS.ALL");
	if (icons.empty()) { fprintf(stderr, "no DATA/ICONS.ALL in %s\n", dir.c_str()); return 2; }
	w.host.text = ReadFile(dir + "/DBASE400.DAT");
	w.host.screen = &w.screen;
	w.core.Attach(&w.db, &w.host, &w.host);
	w.host.Bind(&w.core.Interp(), &w.db);
	w.screen.Bind(&w.core, &w.host, &w.db);
	w.core.Interp().State().g_choice_selected_index = -1;

	// The map's pack, for its palette and its shade and blend tables: STUDY1's.
	Install inst;
	if (!inst.Open(dir.c_str())) { fprintf(stderr, "install: %s\n", inst.Error().c_str()); return 2; }
	std::vector<uint8_t> packBytes = ReadFile(inst.PackFile(inst.PackFor("STUDY1").c_str()));
	Pack pack;
	if (packBytes.empty() || !pack.Load(packBytes.data(), packBytes.size()) || pack.ShadeTables() == nullptr || pack.BlendTable() == nullptr)
	{ fprintf(stderr, "STUDY1's pack has no shade/blend tables\n"); return 2; }

	std::vector<ExeBytes> exe;
	std::string tables;
	int ax = 168, ay = 240;
	if (!Seed(w, argv[2], exe, tables, ax, ay)) { fprintf(stderr, "could not seed from %s\n", argv[2]); return 2; }
	int bad = CheckTables(exe);
	{
		char mine[96];
		snprintf(mine, sizeof mine, "TABLES blend=%08x ramp=%08x\n", Fnv(pack.BlendTable(), 0x10000), Fnv(pack.ShadeTables(), 0x4000));
		const bool same = tables == mine;
		printf("PACK tables %s (original: %s", same ? "identical" : "DIFFER", tables.c_str());
		printf(" port: %s)\n", mine);
		if (!same) bad++;
	}
	RothFont font;
	if (const uint8_t *f = ExeAt(exe, 0x70f12 - (uint32_t)ROTH_FONT_BEFORE, ROTH_FONT_BEFORE + ROTH_FONT_AFTER))
	{
		font.bytes.assign(f, f + ROTH_FONT_BEFORE + ROTH_FONT_AFTER);
		font.desc = ROTH_FONT_BEFORE;
		const uint32_t h = RothFontHash(font.bytes.data(), font.bytes.size());
		printf("FONT hash %08x (ROTH_FONT_HASH %08x: %s)\n", h, ROTH_FONT_HASH, h == ROTH_FONT_HASH ? "matches" : "DIFFERS");
		// FindRothExeFont on the same bytes, as if they were the whole executable.
		RothFont found; std::string err;
		printf("FONT FindRothExeFont on the original's bytes: %s\n", FindRothExeFont(font.bytes, found, err) && found.bytes == font.bytes ? "found" : err.c_str());
	}

	InvPanelAssets a;
	a.iconsAll = &icons;
	a.dbase200 = &d200;
	a.shade = pack.ShadeTables();
	a.blend = pack.BlendTable();
	a.font = font.ok() ? &font : nullptr;
	a.readLine = [&w](uint8_t *dest, uint32_t maxlen, uint32_t off) { return w.host.ReadNextDialogueLine(dest, maxlen, off, 0); };
	a.labelText = [&w](int32_t id) { return ItemLabelText(w.db, id, w.core.InvSink()); };
	w.painter.Bind(&w.screen, &w.core, &w.db, a, false);

	std::vector<Colour> pal = pack.Palette();
	VRPath vr;
	vr.BindDialogue(w);

	FILE *sf = fopen(argv[3], "r");
	if (!sf) { fprintf(stderr, "no script %s\n", argv[3]); return 2; }
	char line[1024];
	int dumpsCompared = 0, dumpsBad = 0;
	long pixels = 0, differing = 0;
	while (fgets(line, sizeof line, sf))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#') continue;
		char cmd[64] = {}; int x[8] = {};
		if (sscanf(line, "%63s %i %i %i %i %i %i %i %i", cmd, &x[0], &x[1], &x[2], &x[3], &x[4], &x[5], &x[6], &x[7]) < 1) continue;
		const std::string c = cmd;
		printf("> %s\n", line);
		Inventory &inv = w.core.Items();
		if (c == "env" || c == "exe" || c == "fill" || c == "font") {}
		else if (c == "hires") w.painter.Bind(&w.screen, &w.core, &w.db, a, true);
		else if (c == "give") w.core.Give(x[0], nullptr);
		else if (c == "remove") w.core.Remove((uint16_t)x[0]);
		else if (c == "adam") inv.RestoreActiveHeldItem(w.db, w.core.InvSink());
		else if (c == "open") w.screen.Open();
		else if (c == "close") w.screen.Close();
		else if (c == "frame") { w.painter.RedrawInventoryCursorCell((uint16_t)x[0]); w.screen.Post(); }
		else if (c == "cursorcell") w.painter.RedrawInventoryCursorCell((uint16_t)x[0]);
		else if (c == "key")
		{
			switch (x[0])
			{
			case 0x01: case 0x17: w.screen.KeySelect(); break;
			case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: w.screen.KeyTab(x[0] - 2); break;
			case 0x0f: w.screen.KeyNextTab(); break;
			case 0x1c: w.screen.KeyEnter(); break;
			case 0x39: w.screen.KeyExamine(); break;
			case 0x4b: w.screen.KeyLeft(); break;
			case 0x4d: w.screen.KeyRight(); break;
			case 0x50: w.screen.KeyDown(); break;
			case 0x48: w.screen.KeyUp(); break;
			default: printf("? key %#x not mapped\n", x[0]); break;
			}
			w.screen.Post();
		}
		else if (c == "hitgrid")
		{
			const int st = x[0] > 0 ? x[0] : 3;
			for (int y = -3; y <= 0x53; y += st) for (int xx = -3; xx <= 0x123; xx += st)
				printf("H %d %d %d\n", xx, y, w.painter.HitTest(xx, y));
		}
		else if (c == "tabclick") w.screen.ClickTab(x[0]);
		else if (c == "cell") w.screen.ClickCell(x[0], x[1]);
		else if (c == "handbox") w.screen.ClickHandBox(x[0], x[1]);
		else if (c == "arrow") { if (x[0]) w.screen.ScrollDown(); else w.screen.ScrollUp(); }
		else if (c == "ptr") vr.Ptr(w, x[0], x[1], x[2], x[3]);
		else if (c == "ptrsweep")
		{
			const int st = x[0] > 0 ? x[0] : 5;
			for (int y = -3; y <= 0x53; y += st) for (int xx = -3; xx <= 0x123; xx += st) vr.Ptr(w, xx, y, 0, 0);
		}
		else if (c == "noinspect") w.screen.g_inventory_inspect_request = -1;
		else if (c == "dlg")
		{
			Dbase100InterpState &st = w.core.Interp().State();
			st.g_dialogue_busy_flag = x[0]; st.g_move_freeze_gate = x[1]; st.g_dialogue_action_queue_count = x[2];
		}
		else if (c == "skey") vr.Key(w, x[0] != 0);
		else if (c == "dump")
		{
			const char *n = strchr(line, ' ');
			const std::string name = n ? n + 1 : "unnamed";
			const std::vector<uint8_t> o = ReadFile(dumps + "/" + name + ".pan");
			const std::vector<uint8_t> &p = w.painter.Pixels();
			const int pw = w.painter.Width(), ph = w.painter.Height();
			long diff = -1;
			if (o.size() == 16 + p.size())
			{
				diff = 0;
				for (size_t i = 0; i < p.size(); i++) if (o[16 + i] != p[i]) diff++;
			}
			dumpsCompared++;
			pixels += (long)p.size();
			if (diff != 0) dumpsBad++;
			differing += diff < 0 ? (long)p.size() : diff;
			printf("PIX %s %ld/%ld differ%s\n", name.c_str(), diff < 0 ? (long)p.size() : diff, (long)p.size(), diff < 0 ? " (no oracle dump of that size)" : "");
			if (diff > 0)
			{
				int shown = 0;
				for (int y = 0; y < ph && shown < 12; y++)
					for (int xx = 0; xx < pw && shown < 12; xx++)
					{
						const size_t i = (size_t)y * pw + xx;
						if (o[16 + i] != p[i]) { printf("  at %d,%d original %u port %u\n", xx, y, o[16 + i], p[i]); shown++; }
					}
			}
			if (!ppm.empty())
			{
				WritePpm(ppm + "/" + name + ".port.ppm", p.data(), pw, ph, pal);
				if (o.size() == 16 + p.size())
				{
					WritePpm(ppm + "/" + name + ".orig.ppm", o.data() + 16, pw, ph, pal);
					std::vector<uint8_t> dm(p.size());
					for (size_t i = 0; i < p.size(); i++) dm[i] = o[16 + i] != p[i] ? 0xff : 0;
					std::vector<Colour> bw(256, Colour{ 0, 0, 0 }); bw[0xff] = Colour{ 255, 0, 255 };
					WritePpm(ppm + "/" + name + ".diff.ppm", dm.data(), pw, ph, bw);
				}
			}
			// The drawing state, in the probe's format.
			const InventoryEntryList &l = w.screen.Entries();
			const int count = w.screen.EntryCount();
			const int dl = l.shownLeft >= 0 ? count : -1;
			const int dr = l.shownRight >= 0 ? count + (l.shownLeft >= 0 ? 1 : 0) : -1;
			const int total = count + (l.shownLeft >= 0) + (l.shownRight >= 0);
			printf(" state list=%d tab=%d pos=%d scroll=%d count=%d total=%d held=%d lh=%d rh=%d dl=%d dr=%d adam=%d/%d dirty=%u arrows=%x lab=%x/%d labx=%d\n",
				w.screen.ActiveList(), w.screen.ActiveTab(), w.screen.CursorEntry(), w.screen.ScrollOffset(), count, total,
				w.screen.HeldEntry(), l.leftEntry, l.rightEntry, EntryOf(l, dl), EntryOf(l, dr), inv.AdamSlot(), (int)inv.AdamItemId(),
				w.screen.g_inventory_dirty_flags, w.painter.g_inventory_arrow_state, w.painter.g_label_text, (int)w.painter.g_label_qty, (int)w.painter.g_label_x);
			printf(" hands L=%d R=%d entries:", inv.LeftHand(), inv.RightHand());
			for (int s : l.slots) printf(" %d/%d", s, (int)(int16_t)inv.Slot(s).id);
			if (l.shownLeft >= 0) printf(" %d/%d", l.shownLeft, (int)(int16_t)inv.Slot(l.shownLeft).id);
			if (l.shownRight >= 0) printf(" %d/%d", l.shownRight, (int)(int16_t)inv.Slot(l.shownRight).id);
			printf("\n label=\"");
			for (int i = 2; i < 0x64 && w.painter.g_label[i]; i++) putchar(w.painter.g_label[i]);
			printf("\"\n hot0");
			PrintHot(w.painter.TabHotspots(), ax, ay);
			printf(" hot1");
			PrintHot(w.painter.PanelHotspots(), ax, ay);
		}
		else printf("? unknown command\n");
	}
	fclose(sf);
	for (const std::string &pr : w.painter.Problems()) printf("PROBLEM %s\n", pr.c_str());
	printf("SUMMARY dumps %d (%d with differences), pixels %ld compared, %ld differ; table/font checks %d bad\n",
		dumpsCompared, dumpsBad, pixels, differing, bad);
	if (vr.events > 0 || vr.mapWrong > 0)
		printf("PIX vr hands: %d click(s) made by the hand's gestures, %d landed off their panel point\n", vr.events, vr.mapWrong);
	return (dumpsBad == 0 && bad == 0 && vr.mapWrong == 0) ? 0 : 1;
}
