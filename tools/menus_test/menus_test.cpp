//
// The Realms menus and what their choices do, against the retail data and
// against dumps of the running original (the flow probe, see
// tools/gameflow_test).
//
//   menus_test "<retail DATA folder>" [--oracle <dump>]...
//
// Without a retail folder it prints SKIPPED and exits 0.
//

#include "../../src/roth/roth_menus.h"
#include "../../src/roth/roth_gameflow.h"

#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

using namespace roth;

namespace
{

int g_checks = 0, g_failed = 0;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

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

Dbase100 *g_db;
std::vector<uint8_t> g_text;

// resolve_dbase100_text (dbase100.c:408-422) + read_next_dialogue_line's
// header (dbase100.c:371-403).
std::string Text(uint32_t id)
{
	uint32_t off = 0;
	if (!g_db->InterfaceEntry((int)id, off) || off == 0) return "Missing Text";
	if ((size_t)off + 8 > g_text.size()) return "?";
	const uint32_t len = g_text[off + 4] | (g_text[off + 5] << 8);
	std::string s;
	for (uint32_t i = 0; i < len && off + 8 + i < g_text.size() && g_text[off + 8 + i] != 0; i++) s += (char)g_text[off + 8 + i];
	return s;
}

std::string Show(const RealmsMenus &m)
{
	std::string s = Text(m.TitleId());
	if (m.BodyId()) s += " / " + Text(m.BodyId());
	s += ":";
	for (const RealmsMenus::Row &r : m.Rows()) s += " [" + (r.textId ? Text(r.textId) : r.label) + (r.enabled ? "" : " (off)") + "]";
	return s;
}

struct Host : FlowInterpSink {};

struct Game
{
	Dbase100 db;
	Host host;
	GameCore core;
	GameFlow flow;
	bool Start(const std::vector<uint8_t> &file)
	{
		if (!db.Load(file.data(), file.size())) return false;
		core.Attach(&db, &host, nullptr);
		flow.Attach(&core, &db, nullptr);
		host.flow = &flow;
		return true;
	}
};

std::string Hex(const uint8_t *p, size_t n)
{
	static const char *d = "0123456789abcdef";
	std::string s;
	for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
	return s;
}

// The probe's lines this port can state (see gameflow_test).
std::map<std::string, std::string> Ours(Game &g)
{
	std::map<std::string, std::string> o;
	const std::vector<uint8_t> &bm = g.core.Interp().State().g_dbase100_record_bitmap;
	o["flags"] = Hex(bm.data(), bm.size());
	o["invcount"] = std::to_string(g.core.Items().Count());
	for (int i = 0; i < Inventory::SLOT_COUNT; i++)
	{
		const InventorySlot &s = g.core.Items().Slot(i);
		if (s.id == 0 && s.quantity == 0) continue;
		char b[64]; snprintf(b, sizeof b, "id=%u b2=%u b3=%u", s.id, s.quantity & 0xff, s.quantity >> 8);
		o["slot " + std::to_string(i)] = b;
	}
	char b[64];
	snprintf(b, sizeof b, "primary=%d secondary=%d", g.core.Items().RightHand(), g.core.Items().LeftHand());
	o["hands"] = b;
	o["seencount"] = std::to_string(g.flow.CutscenesSeenCount());
	std::string gal;
	for (int i = 0; i < g.db.CutsceneCount(); i++) if (g.db.CutsceneGallery(i)) gal += " " + std::to_string(i) + ":" + std::to_string(g.db.CutsceneGallery(i));
	o["gallery"] = gal.empty() ? "" : gal.substr(1);
	o["health"] = std::to_string(g.flow.Health());
	return o;
}

std::map<std::string, std::string> Theirs(const std::vector<std::string> &lines, const std::string &tag)
{
	std::map<std::string, std::string> o;
	for (const std::string &l : lines)
	{
		if (l.compare(0, tag.size() + 1, tag + " ") != 0) continue;
		const std::string rest = l.substr(tag.size() + 1);
		char k[64] = ""; sscanf(rest.c_str(), "%63s", k);
		const std::string key = k;
		const std::string val = rest.size() > key.size() ? rest.substr(key.size() + 1) : "";
		if (key == "slot") { int n = 0; char v[128] = ""; sscanf(val.c_str(), "%d %127[^\n]", &n, v); o["slot " + std::to_string(n)] = v; }
		else if (key == "seencount") { unsigned s = 0; sscanf(val.c_str(), "%u", &s); o["seencount"] = std::to_string(s); }
		else if (key == "health") { int h = 0; sscanf(val.c_str(), "%d", &h); o["health"] = std::to_string(h); }
		else if (key == "flags" || key == "invcount" || key == "hands" || key == "gallery") o[key] = val;
	}
	return o;
}

void Compare(Game &g, const std::vector<std::string> &oracle, const std::string &tag)
{
	const auto a = Ours(g), b = Theirs(oracle, tag);
	if (b.empty()) return;
	int same = 0, diff = 0;
	for (const auto &kv : b)
	{
		auto it = a.find(kv.first);
		if (it != a.end() && it->second == kv.second) same++;
		else { diff++; printf("  DIFF %s %s: original \"%s\" port \"%s\"\n", tag.c_str(), kv.first.c_str(), kv.second.c_str(), it == a.end() ? "(absent)" : it->second.c_str()); }
	}
	for (const auto &kv : a) if (kv.first.compare(0, 5, "slot ") == 0 && !b.count(kv.first)) { diff++; printf("  DIFF %s %s: original (empty)\n", tag.c_str(), kv.first.c_str()); }
	g_checks++;
	if (diff) g_failed++;
	printf("  oracle %-10s %d equal, %d different\n", tag.c_str(), same, diff);
}

} // namespace

int main(int argc, char **argv)
{
	std::string dir = "/mnt/user-data/uploads/Realms of the Haunting/DATA";
	std::vector<std::string> oracle;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--oracle" && i + 1 < argc)
		{
			const std::vector<uint8_t> o = ReadFile(argv[++i]);
			std::string line;
			for (uint8_t c : o) { if (c == '\n') { oracle.push_back(line); line.clear(); } else if (c != '\r') line += (char)c; }
		}
		else dir = a;
	}
	const std::vector<uint8_t> dbFile = ReadFile(dir + "/DBASE100.DAT");
	g_text = ReadFile(dir + "/DBASE400.DAT");
	if (dbFile.empty() || g_text.empty()) { printf("menus_test: SKIPPED (no DBASE100/400 in %s)\n", dir.c_str()); return 0; }
	Dbase100 db;
	CHECK(db.Load(dbFile.data(), dbFile.size()));
	g_db = &db;

	// ---- the boxes show the original's words ---------------------------------
	RealmsMenus m;
	m.OpenMain();
	printf("main:    %s\n", Show(m).c_str());
	CHECK(Show(m) == "Options / Options menu: [Play the game] [Load a game] [Replay Intro] [Settings] [Quit to DOS]");
	m.OpenOptions();
	printf("options: %s\n", Show(m).c_str());
	CHECK(Show(m) == "Options / Options menu: [New game] [Load a game] [Save a game] [Settings] [Quit to DOS]");
	m.Choose(0);
	printf("         %s\n", Show(m).c_str());
	CHECK(Show(m) == "New game: [OK to start a new game.] [CANCEL]");
	m.Back();
	m.Choose(4);
	CHECK(Show(m) == "Quit to DOS: [Quit to DOS] [CANCEL]");
	m.Back(); m.Choose(3);
	printf("         %s\n", Show(m).c_str());
	CHECK(m.Rows().size() == 4);
	m.OpenMain(); m.Choose(3);
	CHECK(m.Rows().size() == 3);                           // the front end's reduced box
	m.OpenMain(); m.Choose(2);
	printf("         %s\n", Show(m).c_str());
	CHECK(Show(m) == "Movie clips: [Intro] [Making of] [Credits]");

	// ---- slot lists: 9 slots, 5 rows, empty load rows are off ----------------
	std::vector<std::string> names(9);
	names[2] = "STUDY1 one";
	m.SetSlotNames(names);
	m.SetDefaultSaveName("The Study");
	m.OpenMain(); m.Choose(1);
	printf("load:    %s\n", Show(m).c_str());
	CHECK(Show(m) == "Load a game / Select file to load: [Not available (off)] [Not available (off)] [STUDY1 one] [Not available (off)] [Not available (off)]");
	m.Choose(0);
	CHECK(!m.HasOutcome());                                // an empty slot cannot be loaded
	m.Choose(2);
	MenuOutcome o = m.Take();
	CHECK(o.kind == MenuOutcome::LOAD && o.slot == 3 && !m.IsOpen());
	m.OpenOptions(); m.Choose(2);
	for (int i = 0; i < 9; i++) m.ScrollDown();
	CHECK(m.Scroll() == 4);
	m.Choose(4);
	o = m.Take();
	CHECK(o.kind == MenuOutcome::SAVE && o.slot == 9 && o.name == "The Study");
	m.OpenOptions(); m.Choose(2);
	CHECK(m.Scroll() == 4);                                // the scroll stays where it was (word[0xc], menu_hud_ui.c:1289, 2060)
	for (int i = 0; i < 9; i++) m.ScrollUp();
	m.Choose(2);                                           // slot 3 holds a save: replace?
	CHECK(m.Current() == RealmsMenus::OVERWRITE_CONFIRM && !m.HasOutcome());
	printf("         %s\n", Show(m).c_str());
	CHECK(Show(m) == "Save a game: [OK to replace] [CANCEL]");
	m.Choose(0);
	o = m.Take();
	CHECK(o.kind == MenuOutcome::SAVE && o.slot == 3);
	m.OpenOptions(); m.Choose(2); m.Choose(2); m.Choose(1);   // (scroll 0 kept from above)
	CHECK(m.Take().kind == MenuOutcome::RESUME);

	// ---- Esc, as each box reads "cancelled" -------------------------------------
	m.OpenMain(); m.Back();
	CHECK(m.Take().kind == MenuOutcome::PLAY);            // menu_hud_ui.c:2092-2093
	m.OpenOptions(); m.Back();
	CHECK(m.Take().kind == MenuOutcome::RESUME);
	m.OpenMain(); m.Choose(4); m.Back();
	CHECK(m.Current() == RealmsMenus::MAIN);
	m.Choose(4); m.Choose(0);
	CHECK(m.Take().kind == MenuOutcome::QUIT);
	m.OpenOptions(); m.Choose(0); m.Choose(0);
	CHECK(m.Take().kind == MenuOutcome::NEW_GAME);
	m.OpenMain(); m.Choose(2); m.Choose(1);
	o = m.Take();
	CHECK(o.kind == MenuOutcome::RUN_ACTION && o.slot == 6 && m.Current() == RealmsMenus::MOVIES);
	m.Down(); m.Down(); m.Enter();                         // from row 1: 2, then round to 0 (menu_hud_ui.c:1649-1656)
	CHECK(m.Take().slot == 5);
	m.Up(); m.Enter();                                     // from row 0 up to the last row
	CHECK(m.Take().slot == 7);

	// ---- the selection a box opens with, and the pointer (show_message_box's loop) ----
	m = RealmsMenus();                                     // the words as the EXE image has them at boot
	m.OpenOptions(); m.Choose(4);
	CHECK(m.Current() == RealmsMenus::QUIT_CONFIRM && m.Selected() == 1);   // 0x71a00's word[0xa]: CANCEL
	m.Enter();
	CHECK(m.Current() == RealmsMenus::OPTIONS && m.Selected() == 4);        // shown again on the row it closed on
	m.PointerAt(2);                                        // the pointer on row 2 (hit 2): selection 1
	CHECK(m.Selected() == 1);
	m.PointerAt(0);                                        // off the rows, the pointer in charge: no selection
	CHECK(m.Selected() == -2);
	m.Enter();                                             // Enter with none: cancelled
	CHECK(m.Take().kind == MenuOutcome::RESUME);
	m.OpenOptions(); m.PointerClick(-1);                   // the close box: cancelled
	CHECK(m.Take().kind == MenuOutcome::RESUME);
	m.OpenMain();
	CHECK(m.ShowFlags() == 0x201);                         // run_main_menu's first show (menu_hud_ui.c:2082)
	m.Choose(4); m.Back();
	CHECK(m.Current() == RealmsMenus::MAIN && m.ShowFlags() == 1);

	// ---- the death screen -----------------------------------------------------------
	// No box and no rows: two keys (game_core.c:986-1000).
	m.OpenDeath(false);
	CHECK(m.Rows().empty());
	m.DeathKey(true);                                      // Space with no last slot: the wait ends
	CHECK(m.Take().kind == MenuOutcome::DEATH_END);
	m.OpenDeath(true); m.DeathKey(false);                  // Esc: the wait ends
	CHECK(m.Take().kind == MenuOutcome::DEATH_END);
	m.OpenDeath(true); m.DeathKey(true);                   // Space with a last slot: load it
	CHECK(m.Take().kind == MenuOutcome::DEATH_LOAD);

	// ---- what the choices do to the game, against the original ----------------------
	{
		// Options -> New game -> OK: reset_and_start_new_game, then the new-game
		// block with no main menu (game_core.c:936, 1006-1012, outer loop with
		// ebx 3). Probe: o_m2 (OPTNEW).
		Game g;
		CHECK(g.Start(dbFile));
		g.flow.Boot(); g.flow.NewGame();
		g.core.Give(9); g.core.Interp().Dbase100BitmapTestSet(77);
		g.flow.ResetForMenu();
		g.flow.NewGame();
		printf("options New game: %d items, seen %u\n", g.core.Items().Count(), g.flow.CutscenesSeenCount());
		CHECK(g.core.Items().Count() == 4 && g.flow.CutscenesSeenCount() == 2);
		Compare(g, oracle, "OPTNEW");
	}
	{
		// Main menu -> Load: the boot ran, the new-game block did NOT
		// (game_play_loop L17af5). Probe: o_m1a saved slot 3 after a give and a
		// flag; o_m1b loaded it from the main menu (MENULOADED).
		Game a;
		CHECK(a.Start(dbFile));
		a.flow.Boot(); a.flow.NewGame();
		a.core.Give(9); a.core.Interp().Dbase100BitmapTestSet(77);
		Compare(a, oracle, "SAVEDSTATE");
		SaveWriter w;
		a.flow.SaveGlobals(w, PlayerRecord(), "STUDY1");
		Game b;
		CHECK(b.Start(dbFile));
		b.flow.Boot();
		SaveReader r(w.Data().data(), w.Data().size());
		CHECK(b.flow.LoadGlobals(r));
		Compare(b, oracle, "MENULOADED");
	}
	{
		// F9 (0x43) saves slot 0 and F10 (0x44) loads it (keymap 0x7093d:
		// 0x43 -> key_quicksave 0x14ce5, 0x44 -> key_quickload 0x14cc9;
		// input.c:545-559). Probe: o_m5 (AFTERF10): the give and the flag after
		// the F9 are gone.
		Game g;
		CHECK(g.Start(dbFile));
		g.flow.Boot(); g.flow.NewGame();
		SaveWriter w;
		g.flow.SaveGlobals(w, PlayerRecord(), "STUDY1");
		g.core.Give(9); g.core.Interp().Dbase100BitmapTestSet(78);
		SaveReader r(w.Data().data(), w.Data().size());
		CHECK(g.flow.LoadGlobals(r));
		Compare(g, oracle, "AFTERF10");
	}
	{
		// The death screen's Esc and the menu's Play: everything reset, the
		// new-game block (probe death runs: AFTERDEATH).
		Game g;
		CHECK(g.Start(dbFile));
		g.flow.Boot(); g.flow.NewGame();
		g.core.Give(9); g.core.Interp().Dbase100BitmapTestSet(55);
		g.flow.SetHealth(0);
		CHECK(g.flow.BeginDeath() == GameFlow::DeathStep::Waiting);
		CHECK(g.flow.TickDeath(2, 1) == GameFlow::DeathStep::Restart);
		g.flow.ResetForMenu();
		g.flow.NewGame();
		Compare(g, oracle, "AFTERDEATH");
	}

	printf("menus_test: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
