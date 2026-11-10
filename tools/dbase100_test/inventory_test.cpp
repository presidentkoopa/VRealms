//
// Tests for the inventory data model (src/roth/roth_inventory.{h,cpp}) on a
// synthetic DBASE100 built by fixture.h. Each case names the rule from
// docs/GAME_inventory.md it checks.
//

#include "fixture.h"
#include "../../src/roth/roth_dbase100.h"
#include "../../src/roth/roth_inventory.h"

#include <stdio.h>

namespace
{

int g_checks = 0, g_failed = 0;
const char *g_test = "";

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
	printf("  FAIL [%s] %s:%d: %s\n", g_test, __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long _a = (long long)(a), _b = (long long)(b); \
	if (_a != _b) { g_failed++; printf("  FAIL [%s] %s:%d: %s == %s (%lld vs %lld)\n", \
	g_test, __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

// ids: 1 key (cat 3), 2 potion (stackable, cat 4), 3 gun (weapon, cat 1),
// 4 Dodger (+4 = 0x80), 5 a character (cat 2), 6 ammo (stackable),
// 7 low render id (0x150), 8 absent.
roth::Dbase100 MakeDb()
{
	fx::Spec s;
	auto item = [&](uint16_t render, uint8_t flags, uint8_t type)
	{
		fx::ItemSpec it; it.renderId = render; it.flags = flags; it.itemType = type;
		s.items.push_back(it);
	};
	item(0x210, 0x00, 0x03);
	item(0x211, 0x00, 0x24);
	item(0x212, 0x10, 0x01);
	item(0x213, 0x80, 0x03);
	item(0x214, 0x00, 0x02);
	item(0x215, 0x00, 0x21);
	item(0x150, 0x00, 0x03);
	fx::ItemSpec none; none.absent = true; s.items.push_back(none);
	auto b = fx::Build(s);
	roth::Dbase100 db;
	if (!db.Load(b.data(), b.size())) printf("fixture failed to load: %s\n", db.Error().c_str());
	return db;
}

struct Recorder : roth::InventorySink
{
	int unequips = 0, rightChanges = 0, lastRight = -99, hudResets = 0, ammoDelta = 0;
	uint16_t ammoId = 0;
	void UnequipWeapon() override { unequips++; }
	void RightHandChanged(int slot) override { rightChanges++; lastRight = slot; }
	void ResetWeaponHud() override { hudResets++; }
	bool IsActiveAmmo(uint16_t id) override { return ammoId != 0 && id == ammoId; }
	void AmmoDisplayChanged(int d) override { ammoDelta += d; }
};

// Puts an item in a fresh slot the way give_item will have to: through
// FindFreeSlot, which bumps the count itself.
int Put(roth::Inventory &inv, uint16_t id, uint16_t qty)
{
	int s = inv.FindFreeSlot();
	if (s >= 0)
	{
		auto v = inv.Serialize();
		v[s * 4] = (uint8_t)id; v[s * 4 + 1] = (uint8_t)(id >> 8);
		v[s * 4 + 2] = (uint8_t)qty; v[s * 4 + 3] = (uint8_t)(qty >> 8);
		const int count = inv.Count();
		inv.Deserialize(v.data(), v.size());
		if (inv.Count() != count) printf("  Put: count drifted\n");
	}
	return s;
}

void TestPickable(const roth::Dbase100 &db)
{
	g_test = "is_item_id_pickable";
	CHECK(roth::IsItemIdPickable(db, 0x210));
	CHECK(roth::IsItemIdPickable(db, 0x212));      // a weapon is pickable
	CHECK(!roth::IsItemIdPickable(db, 0x213));     // +4&0x80 (the Dodger, §10 test 3)
	CHECK(!roth::IsItemIdPickable(db, 0x214));     // category 2
	CHECK(!roth::IsItemIdPickable(db, 0x150));     // below 0x200, even with a record
	CHECK(!roth::IsItemIdPickable(db, 0x1ff));
	CHECK(!roth::IsItemIdPickable(db, 0x2ff));     // no record
	CHECK(!roth::IsItemIdPickable(db, 0));
}

void TestFreeSlot()
{
	g_test = "find_free_inventory_slot";
	roth::Inventory inv; inv.Reset();
	CHECK_EQ(inv.Count(), 0);
	CHECK_EQ(inv.FindFreeSlot(), 0);
	CHECK_EQ(inv.Count(), 1);                      // it increments the count itself
	// Slot 0 is still id 0, so the first-empty rule finds it again.
	CHECK_EQ(inv.FindFreeSlot(), 0);
	inv.Reset();
	for (int i = 0; i < roth::Inventory::SLOT_COUNT; i++) CHECK_EQ(Put(inv, 1, 0), i);
	CHECK_EQ(inv.Count(), 256);
	CHECK_EQ(inv.FindFreeSlot(), -1);
	CHECK_EQ(inv.Count(), 256);
}

void TestQuery(const roth::Dbase100 &db)
{
	g_test = "query count mode";
	roth::Inventory inv; inv.Reset();
	CHECK_EQ(inv.Query(db, 1, 0), 0);
	Put(inv, 1, 0);
	CHECK_EQ(inv.Query(db, 1, 0), 1);
	Put(inv, 2, 5);                                // stackable, one slot: its quantity
	CHECK_EQ(inv.Query(db, 2, 0), 5);
	Put(inv, 1, 0);                                // two plain slots: the slot count
	CHECK_EQ(inv.Query(db, 1, 0), 2);
	CHECK_EQ(inv.Query(db, 0, 0), 0);

	g_test = "query select mode (the Study key, §10 test 4)";
	inv.Reset();
	Recorder r;
	const int keySlot = Put(inv, 1, 0);
	CHECK_EQ(inv.Query(db, 1, 1, &r), 0);          // carried, not held: auto-select and STILL 0
	CHECK_EQ(inv.RightHand(), keySlot);
	CHECK(inv.AutoSelected());
	CHECK_EQ(r.lastRight, keySlot);
	inv.ClearAutoSelected();
	CHECK_EQ(inv.Query(db, 1, 1, &r), 1);          // second click: held
	CHECK(!inv.AutoSelected());

	inv.SetRightHand(-1);
	inv.SetEasySelect(true);                       // easy flag: never auto-select
	CHECK_EQ(inv.Query(db, 1, 1, &r), 0);
	CHECK_EQ(inv.RightHand(), -1);
	CHECK(!inv.AutoSelected());
	inv.SetEasySelect(false);
	CHECK_EQ(inv.Query(db, 1, 3, &r), 0);          // flags&2: never auto-select
	CHECK_EQ(inv.RightHand(), -1);

	const int gunSlot = Put(inv, 3, 6);
	inv.SetLeftHand(gunSlot);
	CHECK_EQ(inv.Query(db, 3, 1, &r), 1);          // left hand counts as held
	CHECK_EQ(inv.Query(db, 9, 1, &r), 0);          // not carried
	CHECK_EQ(inv.RightHand(), -1);
}

void TestRemove(const roth::Dbase100 &db)
{
	g_test = "remove_item: stack (§10 test 5)";
	roth::Inventory inv; inv.Reset();
	Recorder r;
	const int potion = Put(inv, 2, 2);
	CHECK_EQ(inv.RemoveItem(db, 2, &r), -1);
	CHECK_EQ(inv.Slot(potion).quantity, 1);        // decremented, slot kept
	CHECK_EQ(inv.Count(), 1);
	CHECK_EQ(inv.RemoveItem(db, 2, &r), -1);
	CHECK_EQ(inv.Slot(potion).id, 0);              // quantity 1: slot cleared
	CHECK_EQ(inv.Count(), 0);
	CHECK_EQ(inv.RemoveItem(db, 2, &r), 0);        // nothing left

	g_test = "remove_item: non-stackable with a quantity";
	inv.Reset();
	const int gun = Put(inv, 3, 6);
	CHECK_EQ(inv.RemoveItem(db, 3, &r), -1);
	CHECK_EQ(inv.Slot(gun).id, 0);                 // not stackable: cleared outright
	CHECK_EQ(inv.Count(), 0);

	g_test = "remove_item: hands first";
	inv.Reset();
	r = Recorder();
	const int k1 = Put(inv, 1, 0);
	const int k2 = Put(inv, 1, 0);
	inv.SetRightHand(k2);
	CHECK_EQ(inv.RemoveItem(db, 1, &r), -1);
	CHECK_EQ(inv.Slot(k2).id, 0);                  // the held copy goes, not the first
	CHECK_EQ(inv.Slot(k1).id, 1);
	CHECK_EQ(inv.RightHand(), -1);
	CHECK_EQ(r.lastRight, -1);

	const int g = Put(inv, 3, 4);
	inv.SetLeftHand(g);
	CHECK_EQ(inv.RemoveItem(db, 3, &r), -1);
	CHECK_EQ(inv.LeftHand(), -1);
	CHECK_EQ(r.unequips, 1);                       // back to fists

	g_test = "remove_item: active ammo display";
	inv.Reset();
	r = Recorder(); r.ammoId = 6;
	Put(inv, 6, 3);
	CHECK_EQ(inv.RemoveItem(db, 6, &r), -1);
	CHECK_EQ(r.ammoDelta, -1);

	g_test = "remove_item: id 0 and hidden items";
	CHECK_EQ(inv.RemoveItem(db, 0, &r), 0);
	inv.Reset();
	Put(inv, 1 | 0x8000, 0);
	CHECK_EQ(inv.RemoveItem(db, 1, &r), 0);        // full-u16 compare: hidden is invisible
}

void TestFilter(const roth::Dbase100 &db)
{
	g_test = "0x42 filter (§10 test 9)";
	roth::Inventory inv; inv.Reset();
	Recorder r;
	const int key = Put(inv, 1, 0);
	const int chr = Put(inv, 5, 0);
	const int gun = Put(inv, 3, 6);
	inv.SetLeftHand(gun);
	inv.SetRightHand(key);
	inv.SetFilter(db, 0, &r);
	CHECK_EQ(inv.Slot(key).id, 0x8001);
	CHECK_EQ(inv.Slot(chr).id, 5);                 // category 2 stays visible
	CHECK_EQ(inv.Slot(gun).id, 0x8003);
	CHECK_EQ(inv.LeftHand(), -1);
	CHECK_EQ(inv.RightHand(), -1);
	CHECK_EQ(r.hudResets, 1);
	CHECK_EQ(inv.Query(db, 1, 0), 0);              // hidden: invisible to queries
	CHECK_EQ(inv.Count(), 3);                      // still occupying slots
	CHECK_EQ(inv.FindFreeSlot(), 3);
	inv.SetFilter(db, 1, &r);
	CHECK_EQ(inv.Slot(key).id, 1);
	CHECK_EQ(inv.Slot(gun).id, 3);
	CHECK_EQ(inv.Query(db, 1, 0), 1);
	inv.SetFilter(db, 7, &r);                      // unknown mode: nothing
	CHECK_EQ(inv.Slot(key).id, 1);
}

void TestSwapAndSave(const roth::Dbase100 &db)
{
	g_test = "swap";
	roth::Inventory inv; inv.Reset();
	const int a = Put(inv, 1, 0);
	const int b = Put(inv, 2, 3);
	inv.SetRightHand(a);
	inv.SwapSlots(a, b);
	CHECK_EQ(inv.Slot(a).id, 2);
	CHECK_EQ(inv.Slot(b).id, 1);
	CHECK_EQ(inv.RightHand(), b);                  // the hand follows its item
	inv.SwapSlots(a, 999);                         // out of range: nothing
	CHECK_EQ(inv.Slot(a).id, 2);

	g_test = "save chunk 7 (§10 test 10)";
	inv.SetFilter(db, 0);
	auto bytes = inv.Serialize();
	CHECK_EQ(bytes.size(), 0x400);
	CHECK_EQ(bytes[b * 4 + 1], 0x80);              // hidden bit saved raw
	roth::Inventory back; back.Reset();
	back.SetRightHand(42);
	CHECK(back.Deserialize(bytes.data(), bytes.size()));
	CHECK_EQ(back.Count(), 2);                     // recomputed
	CHECK(back.Serialize() == bytes);              // byte-identical
	CHECK_EQ(back.RightHand(), 42);                // hands are chunk 2, untouched
	CHECK(!back.Deserialize(bytes.data(), 0x3ff));
	CHECK(!back.Deserialize(nullptr, 0x400));
}

} // namespace

int main()
{
	roth::Dbase100 db = MakeDb();
	if (!db.IsLoaded()) return 1;
	TestPickable(db);
	TestFreeSlot();
	TestQuery(db);
	TestRemove(db);
	TestFilter(db);
	TestSwapAndSave(db);
	printf("%d checks, %d failed\n", g_checks, g_failed);
	printf("%s\n", g_failed == 0 ? "ALL INVENTORY TESTS PASS" : "*** INVENTORY TESTS FAILED ***");
	return g_failed == 0 ? 0 : 1;
}
