// item_dossier.cpp -- prints the DATA HALF of every item's design dossier (docs/ITEM_DESIGN.md),
// read from the player's own install through the engine's readers (src/roth). Engine-free; writes nothing.
//
//   tools/item_dossier/build.sh /tmp/idw
//   /tmp/idw/item_dossier "<ROTH DATA folder>"              every item, in the dossier's priority order
//   /tmp/idw/item_dossier "<ROTH DATA folder>" --item N     one item
//   /tmp/idw/item_dossier "<ROTH DATA folder>" --order      the priority order only, one line per item
//
// Per item it prints what the dossier's "obtained / used / original behaviour" half needs:
//   - the record: id, name, category (tab), flags, render id (and which other records share it), stackable, weapon;
//   - the close-up film (DBASE300 GDV): frames, fps, size; the turntable to look at;
//   - obtained: starting item (action 3), placed in maps (objects whose render id resolves to this record by the
//     ascending-first search give_item_by_dbase_id makes, inventory.c:38-70), given by RAW 0x29 (map, count),
//     given by DBASE100 op 0x11 in actions and item blocks, produced by another item's OnUse (0xB4);
//   - used: RAW 0x27 "if not item" tests (map, count, the 0x27 mode bits, the trigger kind and the chain's
//     opcodes), RAW 0x2a removes, DBASE100 op 0x91 removes, op 0x02/0x03 count gates, other items' OnUse that
//     target it (0x34);
//   - behaviour: the OnInspect lines (quoted SHORT), OnUse recipes (target 0 = self; results; effect action and
//     its opcodes), document page count, information topic titles, WeaponAction / AsBullet / AsMonster blocks.
//
// Quotes are cut at 60 characters: this prints from the player's install and nothing it prints is committed
// beyond short names and quotes (CLAUDE.md; docs/ITEM_DESIGN.md).
//
// Priority order (docs/ITEM_DESIGN.md): (1) the starting items given by action 3, Adam's own record last;
// (2) every item a RAW 0x27 tests, by test count (descending) then id; (3) every item placed in a map, by
// placement count then id; (4) the rest, by id.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "roth_raw.h"
#include "roth_install.h"
#include "roth_dbase100.h"
#include "roth_log.h"

using namespace roth;

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

static std::vector<uint8_t> g_text;   // DBASE400
static std::vector<uint8_t> g_d300;   // DBASE300

// DBASE400 entry: u32 voice, u16 length, u8 colour, u8, then text (docs/GAME_inventory.md 2.4).
static std::string Text(uint32_t off, size_t cut = 60)
{
	if (g_text.empty() || off == 0 || (uint64_t)off + 8 > g_text.size()) return "";
	const uint8_t *p = g_text.data() + off;
	unsigned len = (unsigned)(p[4] | (p[5] << 8));
	std::string s;
	for (unsigned i = 0; i < len && off + 8 + i < g_text.size(); i++)
	{
		uint8_t c = p[8 + i];
		if (c == 0) break;
		s += (c == '\n' || c == '\r') ? ' ' : ((c >= 0x20 && c < 0x7f) ? (char)c : '?');
	}
	if (s.size() > cut) s = s.substr(0, cut) + "...";
	return s;
}

static const char *CatName(int c)
{
	switch (c)
	{
	case 0: return "0 (general items)";
	case 1: return "1 (weapons and ammunition)";
	case 2: return "2 (characters and important info; never in a hand)";
	case 3: return "3 (artifacts)";
	case 4: return "4 (documents)";
	default: return "?";
	}
}

static const char *RawOpName(int op)
{
	switch (op)
	{
	case 0x02: return "light-switch";
	case 0x03: return "modify-sector";
	case 0x07: return "change-height";
	case 0x08: return "TRIG object-click";
	case 0x09: return "move-sector";
	case 0x0a: return "floor-texture";
	case 0x0c: return "wall-texture-x";
	case 0x0d: return "object-texture";
	case 0x0e: return "scroll-sector";
	case 0x0f: return "scroll-wall";
	case 0x10: return "sound-node";
	case 0x11: return "flash-lights";
	case 0x12: return "delay";
	case 0x13: return "TRIG sector-edge";
	case 0x14: return "sync-walls";
	case 0x15: return "counter";
	case 0x16: return "spawn-object";
	case 0x17: return "toggle-command";
	case 0x18: return "TRIG wall-use";
	case 0x19: return "TRIG floor-use";
	case 0x1a: return "TRIG wall-contact";
	case 0x1b: return "TRIG object-touch";
	case 0x1c: return "cycle-texture";
	case 0x1d: return "change-lighting";
	case 0x1e: return "counter";
	case 0x1f: return "loop";
	case 0x20: return "cycle-object-texture";
	case 0x21: return "animate-walls";
	case 0x22: return "counter";
	case 0x23: return "object-height";
	case 0x24: return "rotate-object";
	case 0x26: return "set-flag";
	case 0x27: return "if-not-item";
	case 0x28: return "if-not-flag";
	case 0x29: return "give-item";
	case 0x2a: return "remove-item";
	case 0x2b: return "dialogue";
	case 0x2d: return "particles";
	case 0x2e: return "smash-wall";
	case 0x2f: return "open-door";
	case 0x30: return "TRIG examine-object";
	case 0x31: return "TRIG examine-floor";
	case 0x32: return "TRIG examine-wall";
	case 0x33: return "damage";
	case 0x34: return "wall-texture";
	case 0x35: return "wall-damage";
	case 0x36: return "if-dialogue";
	case 0x38: return "jump-if-next-fails";
	case 0x3a: return "object-id";
	case 0x3b: return "map-transition";
	case 0x3c: return "spawn-object-x";
	case 0x3d: return "TRIG timer";
	case 0x3f: return "rotate-player";
	case 0x40: return "run-command";
	case 0x41: return "slow-player";
	case 0x42: return "inventory-filter";
	default: return nullptr;
	}
}

static std::string RawOp(int op)
{
	char b[48];
	const char *n = RawOpName(op);
	if (n) snprintf(b, sizeof b, "%s", n);
	else snprintf(b, sizeof b, "op%02x", op);
	return b;
}

static const char *DbOpName(int op)
{
	switch (op)
	{
	case 0x01: return "if-flag";
	case 0x02: return "if-count>";
	case 0x03: return "if-count==";
	case 0x04: return "set-flag";
	case 0x05: return "say";
	case 0x07: return "cutscene";
	case 0x08: return "choice";
	case 0x0d: return "if-flag-next";
	case 0x0e: return "fullscreen";
	case 0x11: return "give";
	case 0x19: return "sfx";
	case 0x1a: return "music";
	case 0x1d: return "jump";
	case 0x23: return "raw-by-index";
	case 0x26: return "health";
	case 0x2d: return "ui-latch";
	case 0x36: return "raw-by-id";
	default: return nullptr;
	}
}

struct Uses
{
	std::map<std::string, int> placed;            // map -> objects
	std::map<std::string, int> rawGive, rawRemove; // map -> records
	std::map<std::string, int> rawTest;            // map -> 0x27 records
	std::map<std::string, std::set<std::string>> testCtx;   // map -> "trigger: chain" strings
	std::set<int> actGive, actRemove, actGate;     // action ids
	std::set<int> itemGive, itemRemove, itemGate;  // item ids whose blocks do it
	std::set<int> usedOnBy;                        // items whose OnUse targets this
	std::set<int> producedBy;                      // items whose OnUse yields this
	bool start = false;
	int Tests() const { int t = 0; for (auto &p : rawTest) t += p.second; return t; }
	int Placed() const { int t = 0; for (auto &p : placed) t += p.second; return t; }
};

static std::string Join(const std::set<int> &s)
{
	std::string o;
	for (int v : s) { if (!o.empty()) o += ", "; o += std::to_string(v); }
	return o;
}

static std::string MapList(const std::map<std::string, int> &m)
{
	std::string o;
	for (auto &p : m) { if (!o.empty()) o += ", "; o += p.first + " x" + std::to_string(p.second); }
	return o;
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: item_dossier <ROTH DATA folder> [--item N | --order]\n"); return 2; }
	int only = 0;
	bool orderOnly = false;
	for (int i = 2; i < argc; i++)
	{
		if (!strcmp(argv[i], "--item") && i + 1 < argc) only = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--order")) orderOnly = true;
	}
	if (!TheInstall().Open(argv[1])) { printf("install: %s\n", TheInstall().Error().c_str()); return 2; }
	Dbase100 db;
	Log log;
	if (!LoadDbase100FromInstall(TheInstall(), db, &log)) { printf("DBASE100 not loaded\n"); return 2; }
	g_text = ReadAll(TheInstall().DataFile("DBASE400.DAT"));
	g_d300 = ReadAll(TheInstall().DataFile("DBASE300.DAT"));

	const int N = db.ItemCount();
	std::vector<Uses> U(N + 1);

	// Starting items: action 3 (game_core.c:825).
	if (const DbAction *a = db.Action(3))
		for (const DbCommand &c : db.Commands(*a))
			if (c.opcode == 0x11 && !c.ifNot && (int)c.operand <= N) U[c.operand].start = true;

	// DBASE100 actions.
	for (int id = 1; id <= db.ActionCount(); id++)
	{
		const DbAction *a = db.Action(id);
		if (!a) continue;
		for (const DbCommand &c : db.Commands(*a))
		{
			const int item = (int)(c.operand & 0xffff);
			if (item < 1 || item > N) continue;
			if (c.opcode == 0x11) (c.ifNot ? U[item].actRemove : U[item].actGive).insert(id);
			if (c.opcode == 0x02 || c.opcode == 0x03) U[item].actGate.insert(id);
		}
	}
	// Item blocks: gives, removes, gates, OnUse.
	for (int id = 1; id <= N; id++)
	{
		const DbItem *it = db.Item(id);
		if (!it) continue;
		for (const DbTriggerBlock &b : it->blocks)
		{
			for (const DbCommand &c : db.Commands(b))
			{
				const int item = (int)(c.operand & 0xffff);
				if (b.trigger == DBTRIG_ON_USE)
				{
					if (c.opcode == 0x34 && !c.ifNot && item >= 1 && item <= N) U[item].usedOnBy.insert(id);
					if (c.opcode == 0x34 && c.ifNot && (int)c.operand >= 1 && (int)c.operand <= N) U[c.operand].producedBy.insert(id);
					continue;
				}
				if (b.trigger == DBTRIG_WEAPON_ACTION || b.trigger == DBTRIG_AS_BULLET || b.trigger == DBTRIG_AS_MONSTER)
					continue;
				if (item < 1 || item > N) continue;
				if (c.opcode == 0x11) (c.ifNot ? U[item].itemRemove : U[item].itemGive).insert(id);
				if (c.opcode == 0x02 || c.opcode == 0x03) U[item].itemGate.insert(id);
			}
		}
	}
	// Maps.
	for (const std::string &name : TheInstall().MapNames())
	{
		std::vector<uint8_t> raw = ReadAll(TheInstall().MapFile(name.c_str()));
		if (raw.empty()) continue;
		Map m = ParseRaw(raw.data(), raw.size());
		if (!m.ok()) continue;
		for (const auto &grp : m.objects)
			for (const Object &o : grp)
			{
				// Ids below 0x200 are never items (is_item_id_pickable, inventory.c:47): such art is scenery.
				const uint16_t rid = (uint16_t)(o.textureIndex | (o.textureSource << 8));
				const int id = rid >= 0x200 ? db.FindItemByRenderId(rid) : 0;
				if (id > 0) U[id].placed[name]++;
			}
		const int nc = (int)m.commands.size();
		for (int i = 0; i < nc; i++)
		{
			const Command &c = m.commands[i];
			if (c.isTrigger) continue;
			const int item = c.key;
			if (item < 1 || item > N) continue;
			if (c.opcode == 0x29) U[item].rawGive[name]++;
			if (c.opcode == 0x2a) U[item].rawRemove[name]++;
			if (c.opcode == 0x27) U[item].rawTest[name]++;
		}
		// The 0x27 tests in context: every trigger whose chain reaches one.
		for (int t = 0; t < nc; t++)
		{
			const Command &tr = m.commands[t];
			if (!tr.isTrigger || tr.chainStart == 0) continue;
			std::vector<int> chain;
			std::set<int> seen;
			for (int k = tr.chainStart; k >= 1 && k <= nc && !seen.count(k) && chain.size() < 40; k = m.commands[k - 1].nextIndex)
			{
				seen.insert(k);
				chain.push_back(k);
				if (m.commands[k - 1].isTrigger) break;
			}
			for (size_t j = 0; j < chain.size(); j++)
			{
				const Command &c = m.commands[chain[j] - 1];
				if (c.opcode != 0x27 || c.isTrigger || c.key < 1 || c.key > N) continue;
				std::string s = RawOp(tr.opcode) + ":";
				for (size_t q = 0; q < chain.size(); q++)
				{
					const Command &d = m.commands[chain[q] - 1];
					s += " " + RawOp(d.opcode);
					if (d.opcode == 0x27 || d.opcode == 0x29 || d.opcode == 0x2a) s += "(" + std::to_string(d.key) + ")";
					if (d.opcode == 0x27)
					{
						// cmd_if_not_item (raw_commands.c:4760-4789): f6&2 select mode (the item must be in a hand),
						// f6&0x20 adds 2 (no auto-select); else count mode (carried anywhere). f6&1 = branch polarity,
						// f6&8 / f6&4 = list mode / compare the last item.
						char f[48];
						const char *m = (d.fireFlags & 8) ? "list" : (d.fireFlags & 4) ? "last-item"
							: !(d.fireFlags & 2) ? "carried" : (d.fireFlags & 0x20) ? "in-hand,no-autoselect" : "in-hand";
						snprintf(f, sizeof f, "[%s f6=%02x]", m, d.fireFlags);
						s += f;
					}
				}
				U[c.key].testCtx[name].insert(s);
			}
		}
	}

	// Priority order.
	std::vector<int> order;
	std::set<int> done;
	auto add = [&](int id) { if (id >= 1 && id <= N && !done.count(id)) { order.push_back(id); done.insert(id); } };
	int adam = 0;
	for (int id = 1; id <= N; id++)
	{
		const DbItem *it = db.Item(id);
		if (it && U[id].start) { if (it->itemType & DBT_ADAM) adam = id; else add(id); }
	}
	// The starting items in the order the brief names them: Lamp, Matchbox, then the rest of action 3.
	{
		std::vector<int> s(order.begin(), order.end());
		std::stable_sort(s.begin(), s.end(), [&](int a, int b) {
			auto rank = [&](int x) { const DbItem *it = db.Item(x); std::string n = it ? Text(it->name) : "";
				return n == "Lamp" ? 0 : n == "Matchbox" ? 1 : 2; };
			return rank(a) < rank(b);
		});
		order = s;
	}
	add(adam);
	std::vector<int> g2, g3, g4;
	for (int id = 1; id <= N; id++) if (!done.count(id) && U[id].Tests()) g2.push_back(id);
	std::stable_sort(g2.begin(), g2.end(), [&](int a, int b) { return U[a].Tests() > U[b].Tests(); });
	for (int id : g2) add(id);
	for (int id = 1; id <= N; id++) if (!done.count(id) && U[id].Placed()) g3.push_back(id);
	std::stable_sort(g3.begin(), g3.end(), [&](int a, int b) { return U[a].Placed() > U[b].Placed(); });
	for (int id : g3) add(id);
	for (int id = 1; id <= N; id++) add(id);

	if (orderOnly)
	{
		int rank = 0;
		for (int id : order)
		{
			const DbItem *it = db.Item(id);
			const char *grp = U[id].start ? "start" : U[id].Tests() ? "tested" : U[id].Placed() ? "placed" : "rest";
			printf("%3d  item %3d  %-6s tests %2d placed %3d  cat %d  %s\n", ++rank, id, grp, U[id].Tests(),
				U[id].Placed(), it ? it->Category() : -1, it ? Text(it->name).c_str() : "?");
		}
		return 0;
	}

	for (int id : order)
	{
		if (only && id != only) continue;
		const DbItem *it = db.Item(id);
		if (!it) { printf("### item %d: no record\n\n", id); continue; }
		const Uses &u = U[id];
		printf("### %d %s\n", id, Text(it->name).c_str());
		printf("- record: category %s; flags 0x%02x; type 0x%02x%s%s%s%s\n", CatName(it->Category()), it->flags,
			it->itemType, it->IsStackable() ? "; stackable" : "", it->IsWeapon() ? "; WEAPON" : "",
			(it->flags & DBI_NOT_PICKABLE) ? "; not pickable" : "", (it->itemType & DBT_ADAM) ? "; Adam's record" : "");
		{
			std::string shared;
			for (int j = 1; j <= N; j++)
			{
				const DbItem *o = db.Item(j);
				if (j != id && o && o->renderId == it->renderId && it->renderId >= 0x200)
					shared += (shared.empty() ? "" : ", ") + std::to_string(j);
			}
			const int first = it->renderId >= 0x200 ? db.FindItemByRenderId(it->renderId) : 0;
			printf("- render id 0x%03x%s%s%s\n", it->renderId, it->renderId < 0x200 ? " (below 0x200: never a world pickup)" : "",
				shared.empty() ? "" : ("; shared with " + shared).c_str(),
				(first && first != id) ? ("; a placed object with this art resolves to item " + std::to_string(first)).c_str() : "");
		}
		if (it->closeup)
		{
			const uint64_t o = (uint64_t)it->closeup * 8 + 4;
			if (o + 0x18 <= g_d300.size())
			{
				const uint8_t *g = g_d300.data() + o;
				auto w16 = [&](int k) { return (unsigned)(g[k] | (g[k + 1] << 8)); };
				printf("- close-up film: DBASE300 0x%x (GDV at +4): %u frames at %u fps, %ux%u%s\n", (unsigned)(it->closeup * 8),
					w16(6), w16(8), w16(0x14), w16(0x16), (it->flags & DBI_CLOSEUP_HOLD) ? "; hold mode (flags bit 0)" : "");
			}
		}
		else printf("- close-up film: none\n");

		// Obtained.
		printf("- obtained:");
		bool any = false;
		if (u.start) { printf(" starting item (action 3);"); any = true; }
		if (!u.placed.empty()) { printf(" placed (pickable art) in %s;", MapList(u.placed).c_str()); any = true; }
		if (!u.rawGive.empty()) { printf(" RAW 0x29 give in %s;", MapList(u.rawGive).c_str()); any = true; }
		if (!u.actGive.empty()) { printf(" DBASE100 give (op 0x11) in actions %s;", Join(u.actGive).c_str()); any = true; }
		if (!u.itemGive.empty()) { printf(" give in the blocks of items %s;", Join(u.itemGive).c_str()); any = true; }
		if (!u.producedBy.empty()) { printf(" result of the OnUse of items %s;", Join(u.producedBy).c_str()); any = true; }
		printf("%s\n", any ? "" : " no source found (not placed, not given)");

		// Used.
		printf("- used by scripts:");
		any = false;
		if (u.Tests()) { printf(" RAW 0x27 in-hand test x%d in %s;", u.Tests(), MapList(u.rawTest).c_str()); any = true; }
		if (!u.rawRemove.empty()) { printf(" RAW 0x2a remove in %s;", MapList(u.rawRemove).c_str()); any = true; }
		if (!u.actRemove.empty()) { printf(" DBASE100 remove (op 0x91) in actions %s;", Join(u.actRemove).c_str()); any = true; }
		if (!u.itemRemove.empty()) { printf(" remove in the blocks of items %s;", Join(u.itemRemove).c_str()); any = true; }
		if (!u.actGate.empty()) { printf(" count gate (op 0x02/0x03) in actions %s;", Join(u.actGate).c_str()); any = true; }
		if (!u.itemGate.empty()) { printf(" count gate in the blocks of items %s;", Join(u.itemGate).c_str()); any = true; }
		if (!u.usedOnBy.empty()) { printf(" target of the OnUse of items %s;", Join(u.usedOnBy).c_str()); any = true; }
		printf("%s\n", any ? "" : " none");
		for (auto &p : u.testCtx)
			for (auto &s : p.second) printf("  - %s: %s\n", p.first.c_str(), s.c_str());

		// Behaviour.
		int pages = 0;
		std::vector<std::string> topics;
		for (const DbTriggerBlock &b : it->blocks)
		{
			std::vector<DbCommand> cs = db.Commands(b);
			if (b.trigger == DBTRIG_INSPECT)
			{
				printf("- OnInspect:");
				for (const DbCommand &c : cs)
					if (c.opcode == 0x05) printf(" \"%s\"", Text(c.operand).c_str());
					else if (c.opcode == 0x07) printf(" [cutscene %u]", c.operand);
					else if (c.opcode == 0x11) printf(" [%s item %u]", c.ifNot ? "remove" : "give", c.operand);
					else if (c.opcode == 0x1d) printf(" [jump to action %u]", c.operand);
					else if (c.opcode == 0x36) printf(" [raw-by-id %u]", c.operand);
				printf("\n");
			}
			if (b.trigger == DBTRIG_ON_USE)
			{
				// resolve_item_use_action (inventory.c:551-594): 0x34 = the target this recipe matches (none = 0 =
				// use on self), every 0xB4 = a result item, 0x35 = the effect action.
				int target = 0, eff = 0;
				std::vector<int> res;
				for (const DbCommand &c : cs)
				{
					if (c.opcode == 0x34 && !c.ifNot) target = (int)c.operand;
					if (c.opcode == 0x34 && c.ifNot) res.push_back((int)c.operand);
					if (c.opcode == 0x35 && !c.ifNot) eff = (int)c.operand;
				}
				printf("- OnUse:");
				if (target == 0) printf(" on self");
				else { const DbItem *t = db.Item(target); printf(" with item %d %s", target, t ? Text(t->name).c_str() : "?"); }
				for (size_t k = 0; k < res.size(); k++)
				{
					const DbItem *t = db.Item(res[k]);
					printf(" %s item %d %s", k ? "+" : "->", res[k], t ? Text(t->name).c_str() : "?");
				}
				if (res.empty()) printf(" (effect only: use_item_on_self does not consume it)");
				if (eff)
				{
					printf("; effect action %d:", eff);
					if (const DbAction *a = db.Action(eff))
						for (const DbCommand &c : db.Commands(*a))
						{
							const char *n = DbOpName(c.opcode);
							printf(" %s%s", c.ifNot ? "!" : "", n ? n : "op");
							if (!n) printf("%02x", c.opcode);
							if (c.opcode == 0x11 || c.opcode == 0x36 || c.opcode == 0x07) printf("(%u)", c.operand);
							if (c.opcode == 0x05) printf("(\"%s\")", Text(c.operand, 40).c_str());
						}
				}
				printf("\n");
			}
			if (b.trigger == DBTRIG_DOCUMENT) for (const DbCommand &c : cs) if (c.opcode == 0x00) pages++;
			if (b.trigger == DBTRIG_INFORMATION)
				for (const DbCommand &c : cs) if (c.opcode == 0x0f) { topics.push_back(Text(c.operand, 40)); break; }
		}
		if (pages) printf("- document: %d page%s\n", pages, pages == 1 ? "" : "s");
		if (!topics.empty())
		{
			std::map<std::string, int> t;
			for (auto &s : topics) t[s]++;
			printf("- information topics (%zu blocks):", topics.size());
			for (auto &p : t) printf(" \"%s\"%s", p.first.c_str(), p.second > 1 ? (" x" + std::to_string(p.second)).c_str() : "");
			printf("\n");
		}
		int wa = 0, ab = 0, am = 0, cn = 0;
		for (const DbTriggerBlock &b : it->blocks)
		{
			wa += b.trigger == DBTRIG_WEAPON_ACTION;
			ab += b.trigger == DBTRIG_AS_BULLET;
			am += b.trigger == DBTRIG_AS_MONSTER;
			cn += b.trigger == DBTRIG_CHANGE_NAME;
		}
		if (wa) printf("- WeaponAction blocks: %d (docs/REALMS_WEAPONS.md)\n", wa);
		if (ab) printf("- AsBullet block: %d (this record is a projectile)\n", ab);
		if (am) printf("- AsMonster block: %d (this record is a creature)\n", am);
		if (cn) printf("- ChangeName block: the label changes with a condition\n");
		if (it->blocks.empty()) printf("- no trigger blocks at all: no comment, no use, no topics\n");
		printf("\n");
	}
	return 0;
}
