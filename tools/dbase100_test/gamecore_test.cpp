//
// The game core end to end, on the retail file: the interpreter and the
// inventory joined, with nothing stubbed between them.
//
//   gamecore_test "<retail DATA folder>"
//
// Without a retail folder it prints SKIPPED and exits 0: every rule here is
// already covered on fixtures by interp_test and inventory_test. What this
// adds is the join.
//

#include "../../src/roth/roth_gamecore.h"

#include <stdio.h>
#include <string>
#include <vector>

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

// The host: everything that is not interpreter or inventory. It records.
struct Host : roth::Dbase100Sink
{
	std::vector<int> movies;
	std::vector<uint32_t> music, sfx;
	uint32_t PlayRecordGdvCutscene(int index) override { movies.push_back(index); return 0; }
	uint32_t LoadDbase300Chunk(uint32_t index) override { music.push_back(index); return 1; }
	uint32_t PlaySoundEffect(uint32_t id, uint32_t) override { sfx.push_back(id); return 0; }
};

} // namespace

int main(int argc, char **argv)
{
	std::string dir = argc > 1 ? argv[1] : "/mnt/user-data/uploads/Realms of the Haunting/DATA";
	std::vector<uint8_t> file = ReadFile(dir + "/DBASE100.DAT");
	if (file.empty()) { printf("gamecore_test: SKIPPED (no DBASE100.DAT in %s)\n", dir.c_str()); return 0; }

	roth::Dbase100 db;
	CHECK(db.Load(file.data(), file.size()));
	CHECK(db.AnomalyTotal() == 0);

	Host host;
	roth::GameCore core;
	core.Attach(&db, &host, nullptr);
	CHECK(core.IsAttached());

	// ---- new game: action 3 through the real interpreter into the real inventory
	core.NewGame();
	roth::Inventory &inv = core.Items();
	CHECK(host.movies.size() == 1 && host.movies[0] == 150);    // CHPTR1
	CHECK(host.music.size() == 1 && host.music[0] == 0x293cad);
	CHECK(inv.Count() == 4);
	CHECK(inv.Slot(0).id == 68);    // Adam Randall
	CHECK(inv.Slot(1).id == 1);     // Shards and Seals
	CHECK(inv.Slot(2).id == 15);    // Lamp
	CHECK(inv.Slot(3).id == 16);    // Matchbox
	CHECK(inv.RightHand() == 3);
	printf("new game: %d items:", inv.Count());
	for (int i = 0; i < inv.Count(); i++) printf(" %u", inv.Slot(i).id);
	printf("  right hand slot %d, left hand slot %d\n", inv.RightHand(), inv.LeftHand());

	// ---- the interpreter's count ops see what the inventory holds
	CHECK(inv.Query(db, 15, 0) == 1);
	CHECK(inv.Query(db, 9, 0) == 0);

	// ---- a second new game starts clean
	core.Interp().Dbase100BitmapTestSet(5);
	CHECK(core.Interp().TestDbase100RecordFlag(5) != 0);
	core.NewGame();
	CHECK(core.Items().Count() == 4);
	CHECK(core.Interp().TestDbase100RecordFlag(5) == 0);

	// ---- death: action 4 plays its sound once, then flag 430 silences it
	host.sfx.clear();
	core.RunAction(roth::GameCore::ACTION_DEATH);
	CHECK(host.sfx.size() == 1);
	core.Interp().Dbase100BitmapTestSet(430);
	core.RunAction(roth::GameCore::ACTION_DEATH);
	CHECK(host.sfx.size() == 1);

	// ---- every action, joined: nothing may read outside the image except
	//      action 543, whose one word is past the end of the file.
	core.NewGame();
	int ran = 0;
	for (int id = 1; id <= db.ActionCount(); id++)
	{
		if (db.Action(id) == nullptr) continue;
		core.RunAction((uint32_t)id);
		core.Interp().ResetDialogueState();
		ran++;
	}
	printf("ran %d actions joined; inventory now holds %d; out-of-image reads %d\n",
		ran, core.Items().Count(), (int)core.Interp().OutOfImageReads());
	CHECK(core.Interp().OutOfImageReads() <= 1);

	printf("gamecore_test: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
