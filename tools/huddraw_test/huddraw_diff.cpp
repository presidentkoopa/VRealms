//
// huddraw_diff -- run a hudprobe script through THE PORT (roth_huddraw's
// painter, driven by our own ported game logic: GameCore's inventory,
// ActorWorld's weapon and damage) and compare the screen, byte for byte, with
// the original's framebuffer dumps.
//
//   huddraw_diff <retail DATA folder> <oracle log> <script> <oracle dump dir> [ppm dir]
//
// The oracle log is what hudprobe.c wrote when the same script ran in the
// ORIGINAL (ROTH.C, headless). Its first INV line and its START line seed the
// port (the carried slots, Adam's item, the hands, the health, the corner and
// slide timers), its "X" lines are the executable's bytes (the font, for this
// test only, and the tables roth_huddraw.cpp transcribes, which are
// compared). Then every command runs on the port; every HUD routine the
// port's logic reaches prints an "HC" line in the probe's format, every give
// and remove an "INV" line, every dump a "D" state line, and run.sh diffs
// them with the original's. At each dump the screen (the "world" pattern
// with the HUD composed over it, HudPainter::ComposeOnto) is compared with
// <oracle dump dir>/<name>.scr.
//
// Writes nothing but stdout (and, with a ppm dir, PPM pictures of both sides
// and of the difference -- retail art: never into the repo).
//

#include "../../src/roth/roth_gamecore.h"
#include "../../src/roth/roth_huddraw.h"
#include "../../src/roth/roth_actors.h"
#include "../../src/roth/roth_install.h"
#include "../../src/roth/roth_das.h"
#include "../../src/roth/roth_dbase100.h"

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

std::vector<uint8_t> Hex(const char *p)
{
	std::vector<uint8_t> v;
	unsigned x;
	int n;
	while (sscanf(p, " %2x%n", &x, &n) == 1) { v.push_back((uint8_t)x); p += n; }
	return v;
}

struct ExeBytes { uint32_t va = 0; std::vector<uint8_t> b; };
const uint8_t *ExeAt(const std::vector<ExeBytes> &exe, uint32_t va, size_t n)
{
	for (const ExeBytes &e : exe)
		if (va >= e.va && va + n <= e.va + e.b.size()) return e.b.data() + (va - e.va);
	return nullptr;
}

uint32_t Fnv(const uint8_t *p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }

struct Test;

// The inventory's outside world: what the engine's RuntimeInvSink does
// (roth_interface.cpp) for the weapon and the HUD.
struct InvHost : InventorySink
{
	Test *t = nullptr;
	void UnequipWeapon() override;
	void EquipWeapon(int slot, int32_t itemId) override;
	void RebuildWeaponList() override;
	bool IsActiveAmmo(int32_t itemId) override;
	void AmmoDisplayChanged(int delta) override;
	void RightHandChanged(int slot) override;
	void HeldItemSlideReset() override;
};

// The weapon's outside world: what the engine's EngineHost does
// (roth_actors_engine.cpp), the health a plain store here.
struct WpnHost : ActorHost
{
	Test *t = nullptr;
	int32_t Health() override;
	void SetHealth(int32_t v) override;
	int SlotQuantity(int slot) override;
	void SetSlotQuantity(int slot, int q) override;
	void RemoveItem(int16_t id) override;
	void NoAmmoMessage(int msg) override { printf("NOAMMO %d\n", msg); }
	void EquipFirstUsableWeapon() override;
	void HudWeaponView(int arg) override;
	void HudWeaponPanel() override;
};

struct Test
{
	Dbase100 db;
	GameCore core;
	ActorWorld world;
	HudPainter hud;
	InvHost inv;
	WpnHost wpn;
	int32_t hp = 0x800, maxhp = 0x800;
	int lastEquips = 0;
	HudWeapon W() const
	{
		HudWeapon h;
		h.item = world.weapon.item;
		h.ammo = world.weapon.ammo;
		h.ammoCap = world.weapon.ammoCap;
		h.panel = world.weapon.actions[0].D(0x48);
		return h;
	}
	// render_weapon_hud(1, &attrs), with its write-back and its tail
	// (rebuild_weapon_inventory_list, WC:1057, only on the full path).
	void RenderWeaponHud()
	{
		const HudWeapon w = W();
		const int32_t p = hud.RenderWeaponHud(w);
		if (p != w.panel) world.weapon.actions[0].SetD(0x48, p);
		if (p != 0) inv.RebuildWeaponList();
	}
	void State()
	{
		const Inventory &i = core.Items();
		printf(" item=%d ammo=%d cap=%d panel=%d cell=%d f80=%d f7c=%d hp=%d max=%d L=%d R=%d peek=%d slide=%d icon=%d\n",
			(int)world.weapon.item, (int)world.weapon.ammo, (int)world.weapon.ammoCap, (int)world.weapon.actions[0].D(0x48),
			(int)hud.CellCounter(), (int)hud.g_weapon_panel_dirty, (int)hud.g_health_bar_dirty, (int)hp, (int)maxhp,
			i.LeftHand(), i.RightHand(), (int)hud.g_corner_peek, (int)hud.g_held_slide, (int)hud.HeldIconId());
	}
	void Inv()
	{
		const Inventory &i = core.Items();
		printf("INV n=%d:", i.Count());
		for (int s = 0; s < Inventory::SLOT_COUNT; s++)
			if (i.Slot(s).id != 0) printf(" %d:%u.%u", s, (unsigned)i.Slot(s).id, (unsigned)i.Slot(s).quantity);
		printf(" adam=%d/%d\n", i.AdamSlot(), (int)i.AdamItemId());
	}
	int SlotOf(int id)
	{
		for (int s = 0; s < Inventory::SLOT_COUNT; s++) if (core.Items().Slot(s).id == (uint16_t)id) return s;
		return -1;
	}
};

void InvHost::UnequipWeapon() { t->world.EquipWeapon((int)t->core.Items().AdamItemId(), -1); }
void InvHost::EquipWeapon(int slot, int32_t itemId)
{
	const int16_t ammo = t->world.EquipWeapon((int)itemId, slot);
	if (ammo != 0) t->world.SetWeaponAmmo(t->core.Items().Query(t->db, (uint16_t)ammo, 0, this));
}
void InvHost::RebuildWeaponList()
{
	std::vector<std::pair<int, uint16_t>> used;
	const Inventory &i = t->core.Items();
	for (int s = 0; s < Inventory::SLOT_COUNT && (int)used.size() < i.Count(); s++)
		if (i.Slot(s).id != 0) used.push_back(std::make_pair(s, i.Slot(s).id));
	t->world.RebuildWeaponList(used);
}
bool InvHost::IsActiveAmmo(int32_t itemId) { return itemId == t->world.weapon.actions[0].D(0x30); }   // id == [0x811e4]
void InvHost::AmmoDisplayChanged(int delta) { t->hud.g_weapon_panel_dirty = 1; t->world.weapon.ammo += delta; }
void InvHost::RightHandChanged(int slot)
{
	t->hud.UpdateSelectedItemIcon(slot < 0 ? 0 : (int16_t)t->core.Items().Slot(slot).id);
}
void InvHost::HeldItemSlideReset() { t->hud.g_held_slide = 0; }

int32_t WpnHost::Health() { return t->hp; }
void WpnHost::SetHealth(int32_t v) { t->hp = v; }
int WpnHost::SlotQuantity(int slot) { return slot >= 0 ? t->core.Items().Slot(slot).quantity : 0; }
void WpnHost::SetSlotQuantity(int slot, int q) { t->core.Items().SetQuantity(slot, (uint16_t)q); }
void WpnHost::RemoveItem(int16_t id) { t->core.Items().RemoveItem(t->db, (uint16_t)id, &t->inv); }
void WpnHost::EquipFirstUsableWeapon()
{
	// equip_first_usable_weapon (WC:1379-1437), as roth_actors_engine.cpp
	// runs it, with its render_weapon_hud (WC:1429) and reset_weapon_hud's (WC:935).
	Inventory &i = t->core.Items();
	std::vector<ActorWorld::CarriedSlot> carried;
	for (int s = 0; s < Inventory::SLOT_COUNT && (int)carried.size() < i.Count(); s++)
		if (i.Slot(s).id != 0) carried.push_back({ s, i.Slot(s).id, i.Slot(s).quantity });
	auto count = [&](int16_t id) { return i.Query(t->db, (uint16_t)id, 0, nullptr); };
	int id = 0;
	const int slot = t->world.FindUsableWeapon(carried, count, id);
	if (slot < 0) { i.SetLeftHand(-1); t->world.EquipWeapon((int)i.AdamItemId(), -1); t->RenderWeaponHud(); return; }
	if (slot == t->world.weapon.heldSlot) return;
	if (slot == i.RightHand()) i.SetRightHand(-1, &t->inv);
	t->world.ResetWeaponFireTiming();
	i.SetLeftHand(slot);
	const int16_t ammo = t->world.EquipWeapon(id, slot);
	if (ammo != 0) t->world.SetWeaponAmmo(i.Query(t->db, (uint16_t)ammo, 0, &t->inv));
	t->RenderWeaponHud();
}
void WpnHost::HudWeaponView(int arg) { t->hud.RenderWeaponView(arg, t->W()); }
void WpnHost::HudWeaponPanel() { t->hud.RedrawWeaponHudPanel(t->W()); }

uint32_t g_rng = 1;
uint32_t Xs() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

void WritePpm(const std::string &path, const uint8_t *px, int w, int h, const std::vector<Colour> &pal)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) { const Colour &c = pal[px[i]]; fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
	fclose(f);
}

int CheckTables(const std::vector<ExeBytes> &exe, const std::vector<uint32_t> &wt)
{
	int bad = 0, checked = 0;
	auto u32 = [](const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); };
	if (const uint8_t *p = ExeAt(exe, 0x71864, 84))
		for (int i = 0; i < 21; i++, checked++) if (u32(p + i * 4) != HudTables::health[i]) { printf("TABLE health[%d] differs\n", i); bad++; }
	for (int k = 0; k < 15 && k < (int)wt.size(); k++)
	{
		checked++;
		if (wt[(size_t)k] != HudTables::weaponPanelAt[k]) { printf("TABLE weaponPanelAt[%d] differs\n", k); bad++; }
		if (const uint8_t *p = ExeAt(exe, wt[(size_t)k], 84))
			for (int i = 0; i < 21; i++, checked++) if (u32(p + i * 4) != HudTables::weaponPanel[k][i]) { printf("TABLE weaponPanel[%d][%d] differs\n", k, i); bad++; }
	}
	if (const uint8_t *p = ExeAt(exe, 0x7675e, 1)) { checked++; if (p[0] != HudTables::messageColor) { printf("TABLE messageColor differs\n"); bad++; } }
	if (const uint8_t *p = ExeAt(exe, 0x75ef0, 4)) { checked++; if (memcmp(p, HudTables::ammoFormat, 4) != 0) { printf("TABLE ammoFormat differs\n"); bad++; } }
	if (const uint8_t *p = ExeAt(exe, 0x708ec, 0x48))
	{
		const uint8_t *mine[6] = { HudTables::edgeTL, HudTables::edgeBR, HudTables::cornerTL, HudTables::cornerTR, HudTables::cornerBL, HudTables::cornerBR };
		const int off[6] = { 0, 4, 8, 0x18, 0x28, 0x38 }, len[6] = { 4, 4, 16, 16, 16, 16 };
		for (int k = 0; k < 6; k++, checked++) if (memcmp(p + off[k], mine[k], (size_t)len[k]) != 0) { printf("TABLE shadow %d differs\n", k); bad++; }
	}
	printf("TABLES checked %d values, %d differ\n", checked, bad);
	if (checked < 21 + 15 * 22 + 2 + 6) { printf("TABLES the oracle log lacks some of them\n"); bad++; }
	return bad;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 5) { fprintf(stderr, "huddraw_diff <DATA folder> <oracle log> <script> <oracle dump dir> [ppm dir]\n"); return 2; }
	const std::string dir = argv[1], dumps = argv[4], ppm = argc > 5 ? argv[5] : "";
	static Test t;
	t.inv.t = &t;
	t.wpn.t = &t;
	std::vector<uint8_t> d100 = ReadFile(dir + "/DBASE100.DAT");
	if (d100.empty() || !t.db.Load(d100.data(), d100.size())) { fprintf(stderr, "no DBASE100.DAT in %s\n", dir.c_str()); return 2; }
	std::vector<uint8_t> d200 = ReadFile(dir + "/DBASE200.DAT");
	std::vector<uint8_t> icons = ReadFile(dir + "/DATA/ICONS.ALL");
	if (icons.empty() || d200.empty()) { fprintf(stderr, "no DATA/ICONS.ALL or DBASE200.DAT in %s\n", dir.c_str()); return 2; }
	t.core.Attach(&t.db, nullptr, &t.inv);
	t.world.Begin(nullptr, &t.db, nullptr, &t.wpn);

	Install inst;
	if (!inst.Open(dir.c_str())) { fprintf(stderr, "install: %s\n", inst.Error().c_str()); return 2; }
	std::vector<uint8_t> packBytes = ReadFile(inst.PackFile(inst.PackFor("STUDY1").c_str()));
	Pack pack;
	if (packBytes.empty() || !pack.Load(packBytes.data(), packBytes.size()) || pack.ShadeTables() == nullptr || pack.BlendTable() == nullptr)
	{ fprintf(stderr, "STUDY1's pack has no shade/blend tables\n"); return 2; }

	// ---- the original's log: tables, font, the starting state ----
	std::vector<ExeBytes> exe;
	std::vector<uint32_t> wt;
	std::string tables;
	int startL = -1, startR = -1, startPeek = 0, startSlide = 0, startIcon = 0;
	bool haveInv = false, haveStart = false;
	{
		FILE *f = fopen(argv[2], "r");
		if (!f) { fprintf(stderr, "no oracle log %s\n", argv[2]); return 2; }
		std::vector<char> line(1 << 16);
		while (fgets(line.data(), (int)line.size(), f))
		{
			const char *p = line.data();
			unsigned va = 0, n = 0; int k = 0;
			if (!strncmp(p, "X ", 2) && sscanf(p, "X %x %u:", &va, &n) == 2) { ExeBytes e; e.va = va; e.b = Hex(strchr(p, ':') + 1); exe.push_back(e); }
			else if (!strncmp(p, "WT ", 3) && sscanf(p, "WT %d %x", &k, &va) == 2) wt.push_back(va);
			else if (!strncmp(p, "TABLES ", 7)) tables = p;
			else if (!haveInv && !strncmp(p, "INV n=", 6))
			{
				haveInv = true;
				const char *q = strchr(p, ':'); q = q ? q + 1 : p;
				int idx, used = 0; unsigned id, qty;
				std::vector<uint8_t> raw(Inventory::SAVE_BYTES, 0);
				while (sscanf(q, " %d:%u.%u%n", &idx, &id, &qty, &used) == 3)
				{
					raw[(size_t)idx * 4] = (uint8_t)id; raw[(size_t)idx * 4 + 1] = (uint8_t)(id >> 8);
					raw[(size_t)idx * 4 + 2] = (uint8_t)qty; raw[(size_t)idx * 4 + 3] = (uint8_t)(qty >> 8);
					q += used;
				}
				t.core.Items().Deserialize(raw.data(), raw.size(), nullptr);
			}
			else if (!haveStart && !strncmp(p, "START", 5))
			{
				haveStart = true;
				const char *q = strstr(p, "hp="); if (q) sscanf(q, "hp=%d max=%d L=%d R=%d peek=%d slide=%d icon=%d", &t.hp, &t.maxhp, &startL, &startR, &startPeek, &startSlide, &startIcon);
			}
		}
		fclose(f);
	}
	if (!haveInv || !haveStart) { fprintf(stderr, "the oracle log has no INV / START line\n"); return 2; }
	Inventory &inv = t.core.Items();
	inv.RestoreActiveHeldItem(t.db, nullptr);
	inv.SetLeftHand(startL);
	inv.SetRightHand(startR, nullptr);
	t.world.EquipWeapon((int)inv.AdamItemId(), -1);   // the original's state: activate_weapon_item(0, 0) done long ago
	t.lastEquips = t.world.weapon.equips;
	t.hud.g_corner_peek = startPeek;
	t.hud.g_held_slide = startSlide;

	int bad = CheckTables(exe, wt);
	{
		char mine[96];
		snprintf(mine, sizeof mine, "TABLES blend=%08x ramp=%08x\n", Fnv(pack.BlendTable(), 0x10000), Fnv(pack.ShadeTables(), 0x4000));
		const bool same = tables == mine;
		printf("PACK tables %s\n", same ? "identical" : "DIFFER");
		if (!same) bad++;
	}
	RothFont font;
	if (const uint8_t *f = ExeAt(exe, 0x70f12 - (uint32_t)ROTH_FONT_BEFORE, ROTH_FONT_BEFORE + ROTH_FONT_AFTER))
	{
		font.bytes.assign(f, f + ROTH_FONT_BEFORE + ROTH_FONT_AFTER);
		font.desc = ROTH_FONT_BEFORE;
		const uint32_t h = RothFontHash(font.bytes.data(), font.bytes.size());
		printf("FONT hash %08x (%s)\n", h, h == ROTH_FONT_HASH ? "matches" : "DIFFERS");
		if (h != ROTH_FONT_HASH) bad++;
	}

	HudAssets a;
	a.ui.iconsAll = &icons;
	a.ui.shade = pack.ShadeTables();
	a.ui.blend = pack.BlendTable();
	a.ui.font = font.ok() ? &font : nullptr;
	a.dbase200 = &d200;
	a.db = &t.db;
	t.hud.Bind(a);
	t.hud.g_held_icon_enabled = 1;          // game_core.c:841, the game's start
	t.hud.UpdateSelectedItemIcon(startIcon);   // the held icon the original has cached (0x8130c)
	t.hud.g_held_slide = startSlide;
	t.hud.onRoutine = [](const char *name, int arg) { printf("HC %s %d", name, arg); t.State(); };
	t.Inv();
	const std::vector<Colour> pal = pack.Palette();

	std::vector<uint8_t> worldPix((size_t)HudPainter::SCREEN_W * HudPainter::SCREEN_H, 0);
	int mouseX = 0, mouseY = 0;
	int dumpsCompared = 0, dumpsBad = 0;
	long pixels = 0, differing = 0, hudPixels = 0;

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
		if (c == "env" || c == "exe" || c == "font" || c == "wtable" || c == "fts") {}
		else if (c == "inv") t.Inv();
		else if (c == "world")
		{
			g_rng = x[0] ? (uint32_t)x[0] : 1u;
			for (uint8_t &v : worldPix) v = (uint8_t)(Xs() >> 24);
		}
		else if (c == "give") { t.core.Give(x[0], nullptr); t.Inv(); }
		else if (c == "remove") { t.core.Remove((uint16_t)x[0]); t.Inv(); }
		else if (c == "health") t.hp = x[0];
		else if (c == "maxhealth") t.maxhp = x[0];
		else if (c == "damage")
		{
			t.world.damagedThisFrame = false;
			t.world.DamageDirect((uint32_t)x[0]);
			if (t.world.damagedThisFrame) t.hud.g_health_bar_dirty = 1;   // WC:32
		}
		else if (c == "layout") t.hud.RefreshHudLayout(t.hp, t.maxhp);
		else if (c == "equip")
		{
			const int s = t.SlotOf(x[0]);
			if (s >= 0) { inv.SetLeftHand(s); t.inv.EquipWeapon(s, x[0]); }
		}
		else if (c == "unequip") { inv.SetLeftHand(-1); t.inv.UnequipWeapon(); }
		else if (c == "equipfirst") t.wpn.EquipFirstUsableWeapon();
		else if (c == "weaponhud")
		{
			if (t.world.weapon.equips != t.lastEquips) { t.lastEquips = t.world.weapon.equips; t.RenderWeaponHud(); }
		}
		else if (c == "select")
		{
			const int s = t.SlotOf(x[0]);
			inv.SetRightHand(s, nullptr);
			t.hud.UpdateSelectedItemIcon(s < 0 ? 0 : x[0]);
		}
		else if (c == "deselect") { inv.SetRightHand(-1, nullptr); t.hud.UpdateSelectedItemIcon(0); }
		else if (c == "fire") { t.world.weapon.lock = 0; t.world.TriggerFire(0, 0); }
		else if (c == "recharge") t.world.RechargeFrame(x[0]);
		else if (c == "mouse") { mouseX = x[0]; mouseY = x[1]; }
		else if (c == "peek") t.hud.CornerPeekTimer(mouseX < 0x20 && mouseY < 0x20, x[0]);
		else if (c == "hud")
		{
			const int fts = x[0];
			t.hud.DrawFrame(fts, t.hp, t.maxhp, [] { return t.W(); }, (int32_t)inv.AdamItemId(),
				[fts] { t.world.RechargeFrame(fts); });   // draw_active_ui_panels' first call (MH:751)
		}
		else if (c == "dump")
		{
			const char *n = strchr(line, ' ');
			const std::string name = n ? n + 1 : "unnamed";
			std::vector<uint8_t> scr = worldPix;
			t.hud.ComposeOnto(scr.data());
			const std::vector<uint8_t> o = ReadFile(dumps + "/" + name + ".scr");
			long diff = -1;
			if (o.size() == 16 + scr.size())
			{
				diff = 0;
				for (size_t i = 0; i < scr.size(); i++) if (o[16 + i] != scr[i]) diff++;
			}
			long hudHere = 0;
			for (size_t i = 0; i < scr.size(); i++) if (scr[i] != worldPix[i] || (o.size() == 16 + scr.size() && o[16 + i] != worldPix[i])) hudHere++;
			dumpsCompared++;
			pixels += (long)scr.size();
			hudPixels += hudHere;
			if (diff != 0) dumpsBad++;
			differing += diff < 0 ? (long)scr.size() : diff;
			printf("PIX %s %ld/%ld differ, %ld HUD pixels (not the world's) on either side%s\n", name.c_str(), diff < 0 ? (long)scr.size() : diff,
				(long)scr.size(), hudHere, diff < 0 ? " (no oracle dump of that size)" : "");
			if (diff > 0)
			{
				int shown = 0;
				for (int y = 0; y < HudPainter::SCREEN_H && shown < 12; y++)
					for (int xx = 0; xx < HudPainter::SCREEN_W && shown < 12; xx++)
					{
						const size_t i = (size_t)y * HudPainter::SCREEN_W + xx;
						if (o[16 + i] != scr[i]) { printf("  at %d,%d original %u port %u world %u\n", xx, y, o[16 + i], scr[i], worldPix[i]); shown++; }
					}
			}
			if (!ppm.empty())
			{
				WritePpm(ppm + "/" + name + ".port.ppm", scr.data(), HudPainter::SCREEN_W, HudPainter::SCREEN_H, pal);
				if (o.size() == 16 + scr.size())
				{
					WritePpm(ppm + "/" + name + ".orig.ppm", o.data() + 16, HudPainter::SCREEN_W, HudPainter::SCREEN_H, pal);
					std::vector<uint8_t> dm(scr.size());
					for (size_t i = 0; i < scr.size(); i++) dm[i] = o[16 + i] != scr[i] ? 0xff : 0;
					std::vector<Colour> bw(256, Colour{ 0, 0, 0 }); bw[0xff] = Colour{ 255, 0, 255 };
					WritePpm(ppm + "/" + name + ".diff.ppm", dm.data(), HudPainter::SCREEN_W, HudPainter::SCREEN_H, bw);
				}
			}
			printf(" state"); t.State();
		}
		else printf("? unknown command\n");
	}
	fclose(sf);
	for (const std::string &pr : t.hud.Problems()) { printf("PROBLEM %s\n", pr.c_str()); bad++; }
	printf("SUMMARY dumps %d (%d with differences), pixels %ld compared, %ld differ, %ld HUD pixels; table/font/problem checks %d bad\n",
		dumpsCompared, dumpsBad, pixels, differing, hudPixels, bad);
	return (dumpsBad == 0 && bad == 0) ? 0 : 1;
}
