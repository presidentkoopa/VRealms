// census.cpp -- how many things in Realms of the Haunting the player can interact with, per map and in total,
// read from the player's own install (the .RAW maps and DBASE100.DAT) through the engine's own readers.
// Engine-free; writes nothing.
//
//   g++ -std=c++17 -O1 -I src/roth -o census tools/interact_census/census.cpp src/roth/roth_raw.cpp \
//       src/roth/roth_install.cpp src/roth/roth_dbase100.cpp src/roth/roth_dbase100_install.cpp src/roth/roth_log.cpp
//   ./census "<ROTH DATA folder>"
//
// What is counted (docs/GAME_core.md 4.5, docs/GAME_inventory.md 4):
//   objects          every placed object
//   item objects     objects whose art (textureIndex | source << 8) is an inventory record's +2 render id
//                    (what give_item_by_dbase_id would resolve; 162 records share a render id, so this is an upper bound)
//   pickable         ... of those, records without DBI_NOT_PICKABLE and not category 2
//   triggers         command-section TRIGGER records by opcode: 0x08 object click chain, 0x18 wall use (left click),
//                    0x19 floor/platform use, 0x30/0x31/0x32 examine (right click) object / floor / wall
//   candle sectors   sectors with the lights-out bit (sector byte +0x0a bit 1): the original calls it CANDLE; it is a LIGHTING mode, not candle objects
#include <cstdio>
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
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	b.resize((size_t)n);
	if (n > 0 && fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: census <ROTH DATA folder>\n"); return 2; }
	if (!TheInstall().Open(argv[1])) { printf("install: %s\n", TheInstall().Error().c_str()); return 2; }
	Dbase100 db;
	Log log;
	if (!LoadDbase100FromInstall(TheInstall(), db, &log)) { printf("DBASE100 not loaded\n"); return 2; }

	// The items.
	int cat[16] = {}, pick = 0, closeup = 0, inspect = 0, onuse = 0, doc = 0, info = 0, weapon = 0;
	for (int id = 1; id <= db.ItemCount(); id++)
	{
		const DbItem *it = db.Item(id);
		if (!it || !it->ok) continue;
		cat[it->Category()]++;
		if (!(it->flags & DBI_NOT_PICKABLE) && it->Category() != DB_CATEGORY_INFO) pick++;
		if (it->closeup) closeup++;
		if (it->FindBlock(DBTRIG_INSPECT)) inspect++;
		if (it->FindBlock(DBTRIG_ON_USE)) onuse++;
		if (it->FindBlock(DBTRIG_DOCUMENT)) doc++;
		if (it->FindBlock(DBTRIG_INFORMATION)) info++;
		if (it->IsWeapon()) weapon++;
	}
	printf("DBASE100: %d inventory records; by category (tab):", db.ItemCount());
	for (int c = 0; c < 16; c++) if (cat[c]) printf(" %d:%d", c, cat[c]);
	printf("\n  pickable (not flagged unpickable, not category 2) %d; weapons %d; with a close-up film %d;"
		" OnInspect comment %d; OnUse/combine %d; document pages %d; information topics %d\n",
		pick, weapon, closeup, inspect, onuse, doc, info);

	const int ops[] = { 0x08, 0x18, 0x19, 0x30, 0x31, 0x32 };
	std::map<int, int> totalOps;
	int tObj = 0, tItem = 0, tPick = 0, tCandle = 0, maps = 0;
	std::set<int> itemsPlaced;
	printf("\n%-10s %7s %7s %7s | %5s %5s %5s %5s %5s %5s | %6s\n", "map", "objects", "items", "pickbl",
		"0x08", "0x18", "0x19", "0x30", "0x31", "0x32", "candle");
	for (const std::string &name : TheInstall().MapNames())
	{
		std::vector<uint8_t> raw = ReadAll(TheInstall().MapFile(name.c_str()));
		if (raw.empty()) continue;
		Map m = ParseRaw(raw.data(), raw.size());
		if (!m.ok()) continue;
		maps++;
		int nObj = 0, nItem = 0, nPick = 0, nCandle = 0;
		for (const auto &grp : m.objects)
			for (const Object &o : grp)
			{
				nObj++;
				const uint16_t rid = (uint16_t)(o.textureIndex | (o.textureSource << 8));
				const int id = db.FindItemByRenderId(rid);
				if (id > 0)
				{
					nItem++;
					itemsPlaced.insert(id);
					const DbItem *it = db.Item(id);
					if (it && !(it->flags & DBI_NOT_PICKABLE) && it->Category() != DB_CATEGORY_INFO) nPick++;
				}
			}
		for (const Sector &s : m.sectors) if (s.flags & 2) nCandle++;
		std::map<int, int> n;
		for (const Command &c : m.commands) if (c.isTrigger) n[c.opcode]++;
		printf("%-10s %7d %7d %7d |", name.c_str(), nObj, nItem, nPick);
		for (int op : ops) { printf(" %5d", n[op]); totalOps[op] += n[op]; }
		printf(" | %6d\n", nCandle);
		tObj += nObj; tItem += nItem; tPick += nPick; tCandle += nCandle;
	}
	printf("%-10s %7d %7d %7d |", "ALL", tObj, tItem, tPick);
	for (int op : ops) printf(" %5d", totalOps[op]);
	printf(" | %6d\n", tCandle);
	printf("\n%d maps; %zu distinct inventory records placed as objects\n", maps, itemsPlaced.size());
	return 0;
}
