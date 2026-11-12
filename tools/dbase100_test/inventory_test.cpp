//
// Tests for the inventory rules (src/roth/roth_inventory.{h,cpp}) on synthetic
// DBASE100 images built by fixture.h. Each case names the ROTH.C rule it
// checks (inventory.c:LINE unless another file is named).
//
// The first six tests predate the audit against ROTH.C; where the source
// showed the model wrong, the test was corrected and says what the source
// showed ("SOURCE:").
//
// With a retail DBASE100.DAT (argv[1], or the default path below) it also
// runs the retail check: every item given once, and the new-game gives of
// action 3. Reads only.
//

#include "fixture.h"
#include "../../src/roth/roth_dbase100.h"
#include "../../src/roth/roth_inventory.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <map>

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
	int equips = 0, lastEquipSlot = -99, hudDirty = 0, rebuilds = 0, slideResets = 0;
	int tabClears = 0, refreshes = 0, flies = 0;
	int32_t lastEquipId = 0, flyX = 0, flyY = 0, flyZ = 0;
	uint16_t flyId = 0;
	int32_t ammoId = 0;
	bool closeRequested = false;
	// What EvalItemBlock answers, by the first command word of the block.
	std::map<uint32_t, int32_t> evalByFirstWord;
	std::vector<std::string> log;       // every call, in order

	void Note(const char *what, long long a = 0, long long b = 0)
	{
		char buf[96];
		snprintf(buf, sizeof buf, "%s(%lld,%lld)", what, a, b);
		log.push_back(buf);
	}
	// SOURCE: the ammo compare is on the sign-extended id (inventory.c:263-264).
	void UnequipWeapon() override { unequips++; Note("unequip"); }
	void EquipWeapon(int slot, int32_t id) override { equips++; lastEquipSlot = slot; lastEquipId = id; Note("equip", slot, id); }
	void WeaponHudDirty() override { hudDirty++; Note("huddirty"); }
	void RebuildWeaponList() override { rebuilds++; Note("rebuild"); }
	void RightHandChanged(int slot) override { rightChanges++; lastRight = slot; Note("right", slot); }
	void ResetWeaponHud() override { hudResets++; Note("hudreset"); }
	bool IsActiveAmmo(int32_t id) override { return ammoId != 0 && id == ammoId; }
	void AmmoDisplayChanged(int d) override { ammoDelta += d; Note("ammo", d); }
	void HeldItemSlideReset() override { slideResets++; Note("slide"); }
	void ClearTabCursors() override { tabClears++; Note("tabclear"); }
	void RefreshPanel() override { refreshes++; Note("refresh"); }
	bool PanelCloseRequested() override { return closeRequested; }
	void PickupFlyStarted(int32_t x, int32_t y, int32_t z, uint16_t id) override
	{ flies++; flyX = x; flyY = y; flyZ = z; flyId = id; Note("fly", id, z); }
	void ItemMessage(int index, int color, uint32_t a, uint32_t b) override
	{
		char buf[96];
		snprintf(buf, sizeof buf, "msg(%x,%d,%x,%x)", index, color, a, b);
		log.push_back(buf);
	}
	int32_t EvalItemBlock(int32_t itemId, const roth::DbTriggerBlock &b, int mode) override
	{
		Note("eval", itemId, (long long)b.trigger * 16 + mode);
		const uint32_t w = (db != nullptr && b.commandCount > 0) ? db->BlockCommand(b, 0).raw : 0;
		auto it = evalByFirstWord.find(w);
		return it == evalByFirstWord.end() ? 0 : it->second;
	}
	void RunActionRecord(uint32_t id) override { Note("action", id); }

	const roth::Dbase100 *db = nullptr;  // for evalByFirstWord
};

bool LogIs(const Recorder &r, const std::vector<std::string> &want)
{
	if (r.log == want) return true;
	printf("    log was:");
	for (const auto &e : r.log) printf(" %s", e.c_str());
	printf("\n");
	return false;
}

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
	// SOURCE: only bit 0 of the mode is read (inventory.c:1260); 7 is "unhide".
	inv.SetFilter(db, 0, &r);
	inv.SetFilter(db, 7, &r);
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
	CHECK_EQ(inv.RightHand(), b);                  // a hand on the CLICKED entry follows its item (inventory.c:654-657)
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

//==========================================================================
//
// The second fixture: everything give / use / combine / label need.
//
//  1 key (cat 3)            2 potion (stackable, cat 4)   3 gun (weapon: 0x14=6, 0x20=6)
//  4 staff (weapon, OnInspect; 0x14=6, 0x20=0x8002)       5 character (cat 2, topics)
//  6 ammo (stackable)       7 render id 0x150             8 absent
//  9 Dodger (+4&0x80)      10 render 0x217, NOT pickable 11 render 0x217, pickable
// 12 Adam (cat 2, +5&0x10) 13 stackable weapon (0x14=9)  14 weapon 0x94=3, 0x20=0x8000
// 15 self-use -> 16        16, 17 results                 18 self-use -> 17 then 16, effect 5
// 19 effect-only (7)       20 the combiner (recipes below) 21..25 its targets
// 26 plain; 27 has the recipe for 26 (target x held)      28 ChangeName
// 29 OnUse block WITHOUT +5&0x40                          30 render id 0x8234
//
//==========================================================================

enum
{
	KEY = 1, POTION, GUN, STAFF, CHARACTER, AMMO, LOWID, ABSENT, DODGER, DUP_BAD, DUP_GOOD,
	ADAM, STACKGUN, NEGGUN, SELFUSE, RES_A, RES_B, SELFUSE2, EFFECTONLY, COMBINER,
	T21, T22, T23, T24, T25, PLAIN26, HOLDER27, RENAMED, NOFLAG, HIGHID,
	ITEM_COUNT = HIGHID
};

roth::Dbase100 MakeDb2()
{
	fx::Spec s;
	auto item = [&](uint16_t render, uint8_t flags, uint8_t type, std::vector<fx::Block> blocks = {})
	{
		fx::ItemSpec it; it.renderId = render; it.flags = flags; it.itemType = type;
		it.name = 0x1000u + 0x10u * (uint32_t)(s.items.size() + 1);    // name = 0x1000 + 0x10 * id
		it.blocks = blocks;
		s.items.push_back(it);
	};
	const bool N = true;   // the If-NOT bit: 0x34 -> 0xB4
	item(0x210, 0x00, 0x03);                                                    // 1
	item(0x211, 0x00, 0x24);                                                    // 2
	item(0x212, 0x10, 0x01, { { 5, { fx::W(0x14, 6), fx::W(0x20, 6) } } });     // 3
	item(0x213, 0x50, 0x01, { { 4, { fx::W(0x05, 0xF38C) } },
		{ 5, { fx::W(0x14, 6), fx::W(0x20, 0x8002) } },
		{ 5, { fx::W(0x14, 99), fx::W(0x20, 1) } } });                          // 4: the second WeaponAction block is ignored
	item(0x214, 0x08, 0x02, { { 1, { fx::W(0x0f, 0x111) } }, { 1, { fx::W(0x0f, 0x222) } } });   // 5
	item(0x215, 0x00, 0x21);                                                    // 6
	item(0x150, 0x00, 0x03);                                                    // 7
	{ fx::ItemSpec none; none.absent = true; s.items.push_back(none); }         // 8
	item(0x216, 0x80, 0x03);                                                    // 9
	item(0x217, 0x80, 0x03);                                                    // 10
	item(0x217, 0x00, 0x03);                                                    // 11
	item(0x218, 0x00, 0x12);                                                    // 12
	item(0x219, 0x10, 0x21, { { 5, { fx::W(0x14, 9) } } });                     // 13
	item(0x21a, 0x10, 0x01, { { 5, { fx::W(0x14, 3, N), fx::W(0x20, 0x8000) } } });   // 14
	item(0x21b, 0x00, 0x43, { { 0x0b, { fx::W(0x34, RES_A, N) } } });           // 15: no 0x34 word -> target 0
	item(0x21c, 0x00, 0x03);                                                    // 16
	item(0x21d, 0x00, 0x03);                                                    // 17
	item(0x21e, 0x00, 0x43, { { 0x0b, { fx::W(0x34, RES_A, N), fx::W(0x34, RES_B, N), fx::W(0x35, 5) } } });   // 18
	item(0x21f, 0x00, 0x43, { { 0x0b, { fx::W(0x35, 7) } } });                  // 19
	item(0x220, 0x00, 0x43, {                                                   // 20
		{ 0x0b, { fx::W(0x34, KEY), fx::W(0x34, RES_A, N) } },                           // one result
		{ 0x0b, { fx::W(0x34, POTION), fx::W(0x34, COMBINER, N), fx::W(0x34, RES_B, N) } },   // first == held
		{ 0x0b, { fx::W(0x34, T21), fx::W(0x34, RES_B, N), fx::W(0x34, COMBINER, N) } },      // second == held
		{ 0x0b, { fx::W(0x34, T22), fx::W(0x34, RES_A, N), fx::W(0x34, T22, N) } },           // second == target
		{ 0x0b, { fx::W(0x34, T23), fx::W(0x34, T23, N), fx::W(0x34, RES_A, N) } },           // first == target
		{ 0x0b, { fx::W(0x34, T24), fx::W(0x34, RES_A, N), fx::W(0x34, RES_B, N), fx::W(0x35, 9) } },   // neither
		{ 0x0b, { fx::W(0x34, T25), fx::W(0x35, 11) } },                                 // effect only
		{ 0x0b, { fx::W(0x34, PLAIN26), fx::W(0x35, 0x77, N) } } });                     // 0xB5 is not an effect: no recipe
	for (int i = 0; i < 5; i++) item((uint16_t)(0x221 + i), 0x00, 0x03);        // 21..25
	item(0x226, 0x00, 0x03);                                                    // 26
	item(0x227, 0x00, 0x43, { { 0x0b, { fx::W(0x34, PLAIN26), fx::W(0x34, RES_A, N) } } });   // 27
	item(0x228, 0x04, 0x03, { { 2, { fx::W(0x10, 0x600) } }, { 2, { fx::W(0x10, 0x700) } } });   // 28
	item(0x229, 0x00, 0x03, { { 0x0b, { fx::W(0x34, RES_A, N) } } });           // 29
	item(0x8234, 0x00, 0x03);                                                   // 30
	auto b = fx::Build(s);
	roth::Dbase100 db;
	if (!db.Load(b.data(), b.size())) printf("fixture 2 failed to load: %s\n", db.Error().c_str());
	return db;
}

uint32_t NameOf(int id) { return 0x1000u + 0x10u * (uint32_t)id; }

void TestGive(const roth::Dbase100 &db)
{
	g_test = "give_item: a plain item gets quantity 0 and the right hand (:377, :413-414, :433-434)";
	{
		roth::Inventory inv; Recorder r;
		CHECK_EQ(inv.GiveItem(db, KEY, nullptr, &r), KEY);
		CHECK_EQ(inv.Count(), 1);
		CHECK_EQ(inv.Slot(0).id, KEY);
		CHECK_EQ(inv.Slot(0).quantity, 0);
		CHECK_EQ(inv.RightHand(), 0);
		CHECK_EQ(inv.LeftHand(), -1);
		CHECK(!inv.PickupLockArmed());                 // no context, no lock
		CHECK(LogIs(r, { "right(0,0)" }));
		// A second one is a second slot; the hand moves to it.
		CHECK_EQ(inv.GiveItem(db, KEY, nullptr, &r), KEY);
		CHECK_EQ(inv.Count(), 2);
		CHECK_EQ(inv.Slot(1).id, KEY);
		CHECK_EQ(inv.RightHand(), 1);
		CHECK_EQ(inv.Query(db, KEY, 0), 2);
	}

	g_test = "give_item: only the low 16 bits of the index count (:383)";
	{
		roth::Inventory inv;
		CHECK_EQ(inv.GiveItem(db, 0x10000 + KEY), KEY);
		CHECK_EQ(inv.Slot(0).id, KEY);
		CHECK_EQ(inv.GiveItem(db, 0xFFFF), 0);         // -1 as 16 bits: refused [unverified]
		CHECK_EQ(inv.GiveItem(db, -5), 0);
		CHECK_EQ(inv.Count(), 1);
	}

	g_test = "give_item: range (:384 is `jg`, so index == count is valid)";
	{
		roth::Inventory inv;
		CHECK_EQ(db.ItemCount(), ITEM_COUNT);
		CHECK_EQ(inv.GiveItem(db, ITEM_COUNT), ITEM_COUNT);
		CHECK_EQ(inv.GiveItem(db, ITEM_COUNT + 1), 0);
		CHECK_EQ(inv.GiveItem(db, 0x7fff), 0);
		CHECK_EQ(inv.Count(), 1);
		CHECK_EQ(inv.GiveItem(db, 0), 0);              // index 0 without a context [unverified]
	}

	g_test = "give_item: stackable (:398-403)";
	{
		roth::Inventory inv; Recorder r;
		CHECK_EQ(inv.GiveItem(db, KEY, nullptr, &r), KEY);
		CHECK_EQ(inv.GiveItem(db, POTION, nullptr, &r), POTION);
		CHECK_EQ(inv.Slot(1).quantity, 1);             // a new stack starts at 1 (:399)
		CHECK_EQ(inv.RightHand(), 1);
		inv.SetRightHand(0);
		CHECK_EQ(inv.GiveItem(db, POTION, nullptr, &r), POTION);
		CHECK_EQ(inv.Count(), 2);                      // one slot (GAME_inventory §10 test 5)
		CHECK_EQ(inv.Slot(1).quantity, 2);
		CHECK_EQ(inv.RightHand(), 1);                  // the stack is re-selected (:433)
		CHECK_EQ(inv.Query(db, POTION, 0), 2);
		CHECK_EQ(roth::ItemLabelQuantity(db, inv.Slot(1)), 2);
		// There is no cap: the word wraps.
		auto bytes = inv.Serialize();
		bytes[1 * 4 + 2] = 0xff; bytes[1 * 4 + 3] = 0xff;
		inv.Deserialize(bytes.data(), bytes.size());
		CHECK_EQ(inv.GiveItem(db, POTION), POTION);
		CHECK_EQ(inv.Slot(1).quantity, 0);
		CHECK_EQ(inv.Count(), 2);
	}

	g_test = "give_item: a hidden stack is not found, a second slot is made (renderer.c:10054)";
	{
		roth::Inventory inv;
		inv.GiveItem(db, POTION);
		inv.SetFilter(db, 0);
		CHECK_EQ(inv.Slot(0).id, 0x8000 | POTION);
		CHECK_EQ(inv.GiveItem(db, POTION), POTION);
		CHECK_EQ(inv.Count(), 2);
		CHECK_EQ(inv.Slot(1).id, POTION);
		CHECK_EQ(inv.Slot(1).quantity, 1);
	}

	g_test = "give_item: weapon ammo from WeaponAction attrs+4 (:392-396)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, KEY, nullptr, &r);
		r.log.clear();
		CHECK_EQ(inv.GiveItem(db, GUN, nullptr, &r), GUN);
		CHECK_EQ(inv.Slot(1).id, GUN);
		CHECK_EQ(inv.Slot(1).quantity, 6);             // op 0x14 = 6, op 0x20 without bit 15
		CHECK_EQ(inv.LeftHand(), 1);                   // equipped (:418-426)
		CHECK_EQ(inv.RightHand(), 0);                  // the right hand is left alone
		CHECK_EQ(r.lastEquipId, GUN);
		CHECK(LogIs(r, { "equip(1,3)", "huddirty(0,0)", "rebuild(0,0)" }));   // :421-427, :458-459

		CHECK_EQ(inv.GiveItem(db, STAFF, nullptr, &r), STAFF);
		CHECK_EQ(inv.Slot(2).quantity, 6 << 8);        // op 0x20 = 0x8002: shifted (GAME_inventory §10 test 2)
		CHECK_EQ(inv.LeftHand(), 2);
		CHECK_EQ(roth::ItemLabelQuantity(db, inv.Slot(2)), 6);   // shown as qty >> 8 (:1582)
		CHECK_EQ(roth::ItemLabelQuantity(db, inv.Slot(1)), 6);
		CHECK_EQ(roth::WeaponGiveQuantity(db, *db.Item(STAFF)), 0x600);
		CHECK(roth::WeaponQuantityIsShifted(db, *db.Item(STAFF)));
		CHECK(!roth::WeaponQuantityIsShifted(db, *db.Item(GUN)));
		CHECK_EQ(roth::WeaponGiveQuantity(db, *db.Item(KEY)), 0);

		// 0x94 = 3 is -3 (weapon_combat.c:388-391); shifted it is 0xFFFFFD00 and
		// the slot keeps 0xFD00. The label shift is LOGICAL on the sign-extended
		// word (:1582), hence the large number.
		CHECK_EQ(inv.GiveItem(db, NEGGUN, nullptr, &r), NEGGUN);
		CHECK_EQ(inv.Slot(3).quantity, 0xFD00);
		CHECK_EQ(roth::WeaponGiveQuantity(db, *db.Item(NEGGUN)), -768);
		CHECK_EQ(roth::ItemLabelQuantity(db, inv.Slot(3)), 0x00FFFFFD);
	}

	g_test = "give_item: a stackable weapon gets 1, not its ammo (:398-399 after :392)";
	{
		roth::Inventory inv; Recorder r;
		CHECK_EQ(inv.GiveItem(db, STACKGUN, nullptr, &r), STACKGUN);
		CHECK_EQ(inv.Slot(0).quantity, 1);
		CHECK_EQ(inv.LeftHand(), 0);
		inv.SetLeftHand(-1);
		r.log.clear();
		CHECK_EQ(inv.GiveItem(db, STACKGUN, nullptr, &r), STACKGUN);
		CHECK_EQ(inv.Count(), 1);
		CHECK_EQ(inv.Slot(0).quantity, 2);
		CHECK_EQ(inv.LeftHand(), 0);                   // the existing stack is equipped again
		CHECK(LogIs(r, { "equip(0,13)", "huddirty(0,0)", "rebuild(0,0)" }));
	}

	g_test = "give_item: category 2 (:407-409, :429-430)";
	{
		roth::Inventory inv; Recorder r;
		CHECK_EQ(inv.GiveItem(db, CHARACTER, nullptr, &r), CHARACTER);
		CHECK_EQ(inv.Count(), 1);
		CHECK_EQ(inv.Slot(0).quantity, 0);
		CHECK_EQ(inv.RightHand(), -1);                 // never put in a hand
		CHECK_EQ(inv.LeftHand(), -1);
		CHECK(r.log.empty());
		// Already held: the early return gives back THE INDEX, and nothing else
		// happens -- with a context too: no fly-in, no lock.
		roth::GiveContext ctx; ctx.id = 0x214;
		r.ammoId = CHARACTER;
		CHECK_EQ(inv.GiveItem(db, CHARACTER, &ctx, &r), CHARACTER);
		CHECK_EQ(inv.Count(), 1);
		CHECK(r.log.empty());
		CHECK(!inv.PickupLockArmed());
		// A hidden... category 2 is never hidden; but once the pack is full the
		// full test comes first and even the duplicate answers 0 (:381).
		for (int i = 1; i < roth::Inventory::SLOT_COUNT; i++) inv.GiveItem(db, KEY);
		CHECK_EQ(inv.Count(), 256);
		CHECK_EQ(inv.GiveItem(db, CHARACTER, nullptr, &r), 0);
	}

	g_test = "give_item: inventory full (:381; GAME_inventory §10 test 6)";
	{
		roth::Inventory inv; Recorder r;
		for (int i = 0; i < roth::Inventory::SLOT_COUNT; i++) CHECK_EQ(inv.GiveItem(db, KEY), KEY);
		CHECK_EQ(inv.Count(), 256);
		CHECK_EQ(inv.RightHand(), 255);
		CHECK_EQ(inv.GiveItem(db, KEY, nullptr, &r), 0);
		CHECK_EQ(inv.GiveItem(db, POTION, nullptr, &r), 0);
		CHECK_EQ(inv.GiveItem(db, GUN, nullptr, &r), 0);
		roth::GiveContext ctx; ctx.id = 0x210;
		CHECK_EQ(inv.GiveItem(db, 0, &ctx, &r), 0);
		CHECK_EQ(inv.GiveItemByRenderId(db, 0x210, &ctx, &r), 0);
		CHECK(r.log.empty());
		CHECK(!inv.PickupLockArmed());
		// A stack that is already there is refused too: the full test is first.
		roth::Inventory inv2;
		inv2.GiveItem(db, POTION);
		for (int i = 1; i < roth::Inventory::SLOT_COUNT; i++) inv2.GiveItem(db, KEY);
		CHECK_EQ(inv2.GiveItem(db, POTION), 0);
		CHECK_EQ(inv2.Slot(0).quantity, 1);
	}

	g_test = "give_item: a zero table entry still gives a bare slot (:390 -> L_8d, :433)";
	{
		roth::Inventory inv; Recorder r;
		CHECK_EQ(inv.GiveItem(db, ABSENT, nullptr, &r), ABSENT);
		CHECK_EQ(inv.Slot(0).id, ABSENT);
		CHECK_EQ(inv.Slot(0).quantity, 0);
		CHECK_EQ(inv.RightHand(), 0);                  // no record: selected
		CHECK(LogIs(r, { "right(0,0)" }));
	}

	g_test = "give_item: the ammo display follows the index given (:436-443)";
	{
		roth::Inventory inv; Recorder r; r.ammoId = AMMO;
		inv.GiveItem(db, AMMO, nullptr, &r);
		inv.GiveItem(db, AMMO, nullptr, &r);
		inv.GiveItem(db, KEY, nullptr, &r);
		CHECK_EQ(r.ammoDelta, 2);
		CHECK(LogIs(r, { "right(0,0)", "ammo(1,0)", "right(0,0)", "ammo(1,0)", "right(1,0)" }));
	}

	g_test = "give_item: the fly-in and the pickup lock (:444-457)";
	{
		roth::Inventory inv; Recorder r;
		roth::GiveContext ctx; ctx.x = 10; ctx.y = -20; ctx.z = 5; ctx.id = 0x999;
		// The record's own render id (>= 0x200) replaces the context's.
		CHECK_EQ(inv.GiveItem(db, KEY, &ctx, &r), KEY);
		CHECK(inv.PickupLockArmed());
		CHECK_EQ(inv.PickupLockFlags(), 5);            // renderer.c:9882
		CHECK_EQ(r.flyId, 0x210);
		CHECK_EQ(r.flyX, 10); CHECK_EQ(r.flyY, -20); CHECK_EQ(r.flyZ, 5 + 0x18);
		CHECK(LogIs(r, { "right(0,0)", "fly(528,29)" }));     // hand first, then the fly-in
		// A record id below 0x200 leaves the context's id.
		inv.ResetPickupLock(); r = Recorder();
		ctx.id = 0x345;
		CHECK_EQ(inv.GiveItem(db, LOWID, &ctx, &r), LOWID);
		CHECK_EQ(r.flyId, 0x345);
		CHECK(inv.PickupLockArmed());
		// ctx+7 & 1 hides the context's id; with a low record id nothing flies.
		inv.ResetPickupLock(); r = Recorder();
		ctx.flags7 = 1;
		CHECK_EQ(inv.GiveItem(db, LOWID, &ctx, &r), LOWID);
		CHECK_EQ(r.flies, 0);
		CHECK(!inv.PickupLockArmed());
		// ...but a record id >= 0x200 still does.
		CHECK_EQ(inv.GiveItem(db, KEY, &ctx, &r), KEY);
		CHECK_EQ(r.flyId, 0x210);
		// No record: the context's id.
		inv.ResetPickupLock(); r = Recorder(); ctx.flags7 = 0;
		CHECK_EQ(inv.GiveItem(db, ABSENT, &ctx, &r), ABSENT);
		CHECK_EQ(r.flyId, 0x345);
		// A context id of 0 (cmd_give_item's drop struct) and a low record id: nothing.
		inv.ResetPickupLock(); r = Recorder(); ctx.id = 0;
		CHECK_EQ(inv.GiveItem(db, LOWID, &ctx, &r), LOWID);
		CHECK_EQ(r.flies, 0);
		// A record id with bit 15 is "negative" and still passes the unsigned test (:449).
		CHECK_EQ(inv.GiveItem(db, HIGHID, &ctx, &r), HIGHID);
		CHECK_EQ(r.flyId, 0x8234);
		// A weapon with a context: equip, fly, then the list.
		inv.ResetPickupLock(); r = Recorder(); r.ammoId = GUN;
		CHECK_EQ(inv.GiveItem(db, GUN, &ctx, &r), GUN);
		CHECK(LogIs(r, { "equip(7,3)", "huddirty(0,0)", "ammo(1,0)", "fly(530,29)", "rebuild(0,0)" }));
	}

	g_test = "give_item: index 0 resolves through the context (:385-386)";
	{
		roth::Inventory inv; Recorder r;
		roth::GiveContext ctx; ctx.id = 0x210;
		CHECK_EQ(inv.GiveItem(db, 0, &ctx, &r), KEY);
		CHECK_EQ(inv.Slot(0).id, KEY);
		CHECK(inv.PickupLockArmed());
		ctx.id = 0x150;
		CHECK_EQ(inv.GiveItem(db, 0, &ctx, &r), 0);    // below 0x200
		ctx.id = 0x8234;                                // zero-extended: matches nothing
		CHECK_EQ(inv.GiveItem(db, 0, &ctx, &r), 0);
		CHECK_EQ(inv.Count(), 1);
	}
}

void TestGiveByRenderId(const roth::Dbase100 &db)
{
	g_test = "give_item_by_dbase_id: search order (:343-364)";
	roth::Inventory inv;
	CHECK_EQ(inv.GiveItemByRenderId(db, 0x210), KEY);
	CHECK_EQ(inv.GiveItemByRenderId(db, 0x212), GUN);
	// Two records carry 0x217; the first is not pickable. The search walks ON
	// past it and gives the second -- while is_item_id_pickable stops at the first.
	CHECK_EQ(inv.GiveItemByRenderId(db, 0x217), DUP_GOOD);
	CHECK(!roth::IsItemIdPickable(db, 0x217));
	CHECK_EQ(inv.GiveItemByRenderId(db, 0x216), 0);    // +4 & 0x80
	CHECK_EQ(inv.GiveItemByRenderId(db, 0x214), 0);    // category 2
	CHECK_EQ(inv.GiveItemByRenderId(db, 0x218), 0);    // Adam is category 2
	CHECK_EQ(inv.GiveItemByRenderId(db, 0x150), 0);    // below 0x200 even with a record
	CHECK_EQ(inv.GiveItemByRenderId(db, 0x1ff), 0);
	CHECK_EQ(inv.GiveItemByRenderId(db, 0x2ff), 0);    // no record
	CHECK_EQ(inv.GiveItemByRenderId(db, -1), 0);       // signed `jl` (:348)
	// Record 30 carries 0x8234. The record side is sign-extended, so neither
	// spelling of the id can reach it through here.
	CHECK_EQ(inv.GiveItemByRenderId(db, 0x8234), 0);
	CHECK_EQ(inv.GiveItemByRenderId(db, (int16_t)0x8234), 0);
	CHECK_EQ(inv.Count(), 3);
	// is_item_id_pickable's test is unsigned (:48), so the sign-extended
	// spelling DOES match there.
	CHECK(!roth::IsItemIdPickable(db, 0x8234));
	CHECK(roth::IsItemIdPickable(db, (int16_t)0x8234));
	CHECK(roth::IsItemIdPickable(db, 0x210));
	CHECK(!roth::IsItemIdPickable(db, 0x218));
}

void TestAudit(const roth::Dbase100 &db)
{
	g_test = "audit: reset_inventory clears slots and count only (renderer.c:10256-10261)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, GUN); inv.GiveItem(db, KEY);
		inv.Query(db, KEY, 1);
		inv.Reset();
		CHECK_EQ(inv.Count(), 0);
		CHECK_EQ(inv.Slot(0).id, 0);
		CHECK_EQ(inv.Slot(0).quantity, 0);
		CHECK_EQ(inv.LeftHand(), 0);                   // the hand pointers stand
		CHECK_EQ(inv.RightHand(), 1);
	}

	g_test = "audit: query clears the autoselect flag on every call (:477)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, KEY); inv.GiveItem(db, T21);  // T21 ends in the right hand
		CHECK_EQ(inv.Query(db, KEY, 1, &r), 0);
		CHECK(inv.AutoSelected());
		CHECK_EQ(inv.Query(db, KEY, 0, &r), 1);        // a count-mode call clears it
		CHECK(!inv.AutoSelected());
		inv.SetRightHand(1);
		inv.Query(db, KEY, 1, &r);
		CHECK(inv.AutoSelected());
		inv.Query(db, 999, 1, &r);                     // so does a failed select
		CHECK(!inv.AutoSelected());
	}

	g_test = "audit: the slide timer resets on the right-hand path only (:486 vs :491)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, GUN); inv.GiveItem(db, KEY);
		CHECK_EQ(inv.Query(db, KEY, 1, &r), 1);
		CHECK_EQ(r.slideResets, 1);
		CHECK_EQ(inv.Query(db, GUN, 1, &r), 1);
		CHECK_EQ(r.slideResets, 1);
	}

	g_test = "audit: count mode gives the quantity only when it is above 1 (:518-529)";
	{
		roth::Inventory inv;
		Put(inv, POTION, 0);                           // a stackable slot with a zero quantity word
		CHECK_EQ(inv.Query(db, POTION, 0), 1);         // SOURCE: 1, the match count -- not 0
		roth::Inventory inv1; Put(inv1, POTION, 1);
		CHECK_EQ(inv1.Query(db, POTION, 0), 1);
		roth::Inventory inv2; Put(inv2, POTION, 0x8005);   // signed: negative is not > 1
		CHECK_EQ(inv2.Query(db, POTION, 0), 1);
		roth::Inventory inv3; Put(inv3, GUN, 6);       // not stackable: the count
		CHECK_EQ(inv3.Query(db, GUN, 0), 1);
		roth::Inventory inv4; Put(inv4, POTION, 7); Put(inv4, POTION, 9);   // two slots: the count
		CHECK_EQ(inv4.Query(db, POTION, 0), 2);
	}

	g_test = "audit: remove clears the id word only; the quantity stays (:275)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, GUN);
		CHECK_EQ(inv.RemoveItem(db, GUN, &r), -1);
		CHECK_EQ(inv.Slot(0).id, 0);
		CHECK_EQ(inv.Slot(0).quantity, 6);             // stale, and saved raw in chunk 7
		CHECK_EQ(inv.Serialize()[2], 6);
		CHECK_EQ(inv.Count(), 0);
		// The next give overwrites it.
		inv.GiveItem(db, KEY);
		CHECK_EQ(inv.Slot(0).quantity, 0);
	}

	g_test = "audit: remove_item rebuilds the weapon list only when nothing matched (:288, :318)";
	{
		roth::Inventory inv; Recorder r;
		CHECK_EQ(inv.RemoveItem(db, KEY, &r), 0);      // empty pack
		CHECK_EQ(r.rebuilds, 1);
		inv.GiveItem(db, KEY);
		CHECK_EQ(inv.RemoveItem(db, POTION, &r), 0);   // no match
		CHECK_EQ(r.rebuilds, 2);
		CHECK_EQ(inv.RemoveItem(db, KEY, &r), -1);     // a removal does not
		CHECK_EQ(r.rebuilds, 2);
	}

	g_test = "audit: removing one of a held stack still empties the hand (:259-262 before :268)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, POTION); inv.GiveItem(db, POTION);
		CHECK_EQ(inv.RightHand(), 0);
		CHECK_EQ(inv.RemoveItem(db, POTION, &r), -1);
		CHECK_EQ(inv.Slot(0).quantity, 1);
		CHECK_EQ(inv.RightHand(), -1);
		CHECK(LogIs(r, { "right(-1,0)" }));
	}

	g_test = "audit: remove_inventory_item order: unequip, right hand, ammo (:250-267)";
	{
		roth::Inventory inv; Recorder r; r.ammoId = GUN;
		inv.GiveItem(db, GUN);
		inv.SetRightHand(0);                           // the same slot in both hands
		CHECK_EQ(inv.RemoveItem(db, GUN, &r), -1);
		CHECK(LogIs(r, { "unequip(0,0)", "right(-1,0)", "ammo(-1,0)" }));
		CHECK_EQ(inv.LeftHand(), -1);
	}

	g_test = "audit: consume_held_item (:325-338)";
	{
		roth::Inventory inv; Recorder r;
		CHECK_EQ(inv.ConsumeSlot(db, 0, &r), 0);       // empty pack
		CHECK_EQ(inv.ConsumeSlot(db, -1, &r), 0);      // null slot
		inv.GiveItem(db, KEY); inv.GiveItem(db, POTION); inv.GiveItem(db, POTION);
		r = Recorder();
		CHECK_EQ(inv.ConsumeSlot(db, 1, &r), 1);
		CHECK_EQ(inv.Slot(1).quantity, 1);
		CHECK(LogIs(r, { "right(-1,0)", "rebuild(0,0)" }));
		// The original does not look whether the slot is used: the count drops anyway.
		CHECK_EQ(inv.Count(), 2);
		CHECK_EQ(inv.ConsumeSlot(db, 200, &r), 1);
		CHECK_EQ(inv.Count(), 1);
	}

	g_test = "audit: the 0x42 filter reads bit 0 only, and its order (:1255-1288)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, KEY); inv.GiveItem(db, CHARACTER); inv.GiveItem(db, GUN); inv.GiveItem(db, ADAM);
		inv.SetFilter(db, 2, &r);                      // bit 0 clear: hide
		CHECK_EQ(inv.Slot(0).id, 0x8000 | KEY);
		CHECK_EQ(inv.Slot(1).id, CHARACTER);
		CHECK_EQ(inv.Slot(2).id, 0x8000 | GUN);
		CHECK_EQ(inv.Slot(3).id, ADAM);                // category 2 with the Adam bit
		CHECK_EQ(inv.Slot(2).quantity, 6);
		CHECK_EQ(inv.LeftHand(), -1);
		CHECK_EQ(inv.RightHand(), -1);
		// No activate_weapon_item(0,0): the pointers are zeroed directly.
		CHECK(LogIs(r, { "tabclear(0,0)", "right(-1,0)", "hudreset(0,0)", "rebuild(0,0)" }));
		inv.SetFilter(db, 0, &r);                      // hiding twice changes nothing
		CHECK_EQ(inv.Slot(0).id, 0x8000 | KEY);
		r = Recorder();
		inv.SetFilter(db, 3, &r);                      // bit 0 set: unhide
		CHECK_EQ(inv.Slot(0).id, KEY);
		CHECK_EQ(inv.Slot(2).id, GUN);
		CHECK(LogIs(r, { "tabclear(0,0)", "rebuild(0,0)" }));
		CHECK_EQ(inv.Count(), 4);
	}

	g_test = "audit: swap moves only a hand on the CLICKED entry (:650-664)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, GUN); inv.GiveItem(db, KEY); inv.GiveItem(db, T21);
		inv.SetLeftHand(0); inv.SetRightHand(1);
		inv.SwapSlots(0, 2, &r);                       // clicked = the gun's entry
		CHECK_EQ(inv.Slot(2).id, GUN);
		CHECK_EQ(inv.Slot(2).quantity, 6);
		CHECK_EQ(inv.Slot(0).id, T21);
		CHECK_EQ(inv.LeftHand(), 2);                   // followed its item
		CHECK_EQ(inv.RightHand(), 1);
		CHECK(LogIs(r, { "rebuild(0,0)" }));
		// SOURCE: a hand on the HELD entry (the second argument) is not moved.
		inv.SwapSlots(0, 1, &r);                       // held = slot 1, which is the right hand
		CHECK_EQ(inv.Slot(0).id, KEY);
		CHECK_EQ(inv.RightHand(), 1);                  // still slot 1, which now holds T21
		CHECK_EQ(inv.Slot(1).id, T21);
	}

	g_test = "save chunk 2: hand offsets, and slot 0 saves as none (savegame.c:152-157, 187-192)";
	{
		roth::Inventory inv;
		inv.GiveItem(db, KEY); inv.GiveItem(db, GUN);
		inv.SetRightHand(0); inv.SetLeftHand(1);
		uint32_t l = 99, rr = 99;
		inv.SaveHands(l, rr);
		CHECK_EQ(l, 4);
		CHECK_EQ(rr, 0);                               // slot 0: pointer - base == 0 == "none"
		roth::Inventory back;
		auto bytes = inv.Serialize();
		Recorder r;
		back.LoadHands(l, rr);
		CHECK(back.Deserialize(bytes.data(), bytes.size(), &r));
		CHECK_EQ(r.rebuilds, 1);                       // savegame.c:1416
		CHECK_EQ(back.LeftHand(), 1);
		CHECK_EQ(back.RightHand(), -1);                // lost, as in the original
		back.LoadHands(0x3fc, 6);
		CHECK_EQ(back.LeftHand(), 255);
		CHECK_EQ(back.RightHand(), -1);                // not a slot boundary [unverified]
	}
}

struct FixedRng : roth::InventoryRng
{
	int32_t value = 0, lastCount = -1, calls = 0;
	int32_t NextIndexForCount(int32_t count) override { lastCount = count; calls++; return value; }
};

void TestAutoselect(const roth::Dbase100 &db)
{
	g_test = "rng_next_index_for_count (renderer.c:11130-11147)";
	{
		// Expected values come from an independent model of the routine, from
		// the EXE's initial state: LCG 0xB785, rng 0, countdown 0.
		roth::RothIndexRng rng;
		const int want3[10] = { 2, 0, 0, 2, 0, 1, 0, 0, 2, 1 };
		for (int i = 0; i < 10; i++) CHECK_EQ(rng.NextIndexForCount(3), want3[i]);
		const int want24[6] = { 21, 14, 3, 11, 5, 2 };
		for (int i = 0; i < 6; i++) CHECK_EQ(rng.NextIndexForCount(24), want24[i]);
		CHECK_EQ(rng.seed, 0x1f64);
		CHECK_EQ(rng.rng, 0x641f);
		CHECK_EQ(rng.countdown, 8);
		for (int n = 1; n <= 24; n++)
			for (int i = 0; i < 50; i++)
			{
				const int32_t v = rng.NextIndexForCount(n);
				CHECK(v >= 0 && v < n);
			}
	}

	g_test = "find_or_autoselect: count mode (:1441-1451)";
	{
		roth::Inventory inv; FixedRng rng;
		inv.GiveItem(db, KEY); inv.GiveItem(db, T21); inv.GiveItem(db, KEY); inv.GiveItem(db, T22);
		const int32_t ids[3] = { KEY, T22, 999 };
		CHECK_EQ(inv.FindOrAutoselect(ids, 3, 0, rng), 3);
		CHECK_EQ(inv.LastItem(), T22);                 // the last hit, in slot order
		const int32_t dup[2] = { KEY, KEY };           // a repeated id counts twice per slot
		CHECK_EQ(inv.FindOrAutoselect(dup, 2, 0, rng), 4);
		CHECK_EQ(inv.FindOrAutoselect(ids, 0, 0, rng), 0);
		CHECK_EQ(rng.calls, 0);
	}

	g_test = "find_or_autoselect: select mode (:1405-1439)";
	{
		roth::Inventory inv; FixedRng rng; Recorder r;
		inv.GiveItem(db, GUN);                         // slot 0, left hand
		inv.GiveItem(db, KEY);                         // 1
		inv.GiveItem(db, T21);                         // 2
		inv.GiveItem(db, KEY);                         // 3
		inv.GiveItem(db, T22);                         // 4, right hand
		const int32_t ids[2] = { KEY, T21 };
		const int32_t held[2] = { 999, T22 };
		const int32_t gun[1] = { GUN };
		// In the right hand: 1, slide reset, last item.
		CHECK_EQ(inv.FindOrAutoselect(held, 2, 1, rng, &r), 1);
		CHECK_EQ(r.slideResets, 1);
		CHECK_EQ(inv.LastItem(), T22);
		// In the left hand: 1, no slide reset.
		CHECK_EQ(inv.FindOrAutoselect(gun, 1, 1, rng, &r), 1);
		CHECK_EQ(r.slideResets, 1);
		CHECK_EQ(inv.LastItem(), GUN);
		CHECK_EQ(rng.calls, 0);
		// Neither: slots 1, 2, 3 are collected in order and the rng picks.
		rng.value = 2;
		CHECK_EQ(inv.FindOrAutoselect(ids, 2, 1, rng, &r), 0);   // 0, like the single-item query
		CHECK_EQ(rng.lastCount, 3);
		CHECK_EQ(inv.RightHand(), 3);
		CHECK(inv.AutoSelected());
		CHECK_EQ(r.lastRight, 3);
		// Only the low 16 bits of the draw are used (:1432).
		inv.SetRightHand(4);
		rng.value = 0x30001;
		CHECK_EQ(inv.FindOrAutoselect(ids, 2, 1, rng, &r), 0);
		CHECK_EQ(inv.RightHand(), 2);
		// flags & 2, and the easy flag: no scan, no draw, flag clear.
		inv.SetRightHand(4);
		const int calls = rng.calls;
		CHECK_EQ(inv.FindOrAutoselect(ids, 2, 3, rng, &r), 0);
		CHECK_EQ(inv.RightHand(), 4);
		CHECK(!inv.AutoSelected());
		inv.SetEasySelect(true);
		CHECK_EQ(inv.FindOrAutoselect(ids, 2, 1, rng, &r), 0);
		CHECK_EQ(inv.RightHand(), 4);
		inv.SetEasySelect(false);
		CHECK_EQ(rng.calls, calls);
		// Nothing carried: 0 and no draw.
		const int32_t none[1] = { 999 };
		CHECK_EQ(inv.FindOrAutoselect(none, 1, 1, rng, &r), 0);
		CHECK_EQ(rng.calls, calls);
		CHECK(!inv.AutoSelected());
		// Hidden items are negative ids and match nothing.
		inv.SetFilter(db, 0);
		CHECK_EQ(inv.FindOrAutoselect(ids, 2, 1, rng, &r), 0);
		CHECK_EQ(inv.RightHand(), -1);
		CHECK_EQ(inv.FindOrAutoselect(ids, 2, 0, rng, &r), 0);
	}

	g_test = "find_or_autoselect: at most 24 candidates (:1429)";
	{
		roth::Inventory inv; FixedRng rng;
		for (int i = 0; i < 40; i++) inv.GiveItem(db, KEY);
		inv.SetRightHand(-1);
		const int32_t ids[1] = { KEY };
		rng.value = 23;
		CHECK_EQ(inv.FindOrAutoselect(ids, 1, 1, rng), 0);
		CHECK_EQ(rng.lastCount, 24);
		CHECK_EQ(inv.RightHand(), 23);
		// With the real generator the pick is always one of the 24.
		roth::RothIndexRng real;
		for (int i = 0; i < 100; i++)
		{
			inv.SetRightHand(-1);
			inv.FindOrAutoselect(ids, 1, 1, real);
			CHECK(inv.RightHand() >= 0 && inv.RightHand() < 24);
		}
	}
}

void TestHands(const roth::Dbase100 &db)
{
	g_test = "cursor: picking a hand's cell empties the hand (dialogue_ui.c:1773-1787)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, GUN); inv.GiveItem(db, KEY); inv.GiveItem(db, T21);
		inv.SetRightHand(1);
		inv.PickUpToCursor(2, &r);
		CHECK_EQ(inv.Held(), 2);
		CHECK_EQ(inv.LeftHand(), 0); CHECK_EQ(inv.RightHand(), 1);
		CHECK(r.log.empty());
		inv.PickUpToCursor(0, &r);                     // the equipped weapon
		CHECK_EQ(inv.LeftHand(), -1);
		CHECK(LogIs(r, { "unequip(0,0)" }));
		inv.PickUpToCursor(1, &r);                     // the selected item: no icon refresh
		CHECK_EQ(inv.RightHand(), -1);
		CHECK_EQ(r.rightChanges, 0);
		CHECK_EQ(inv.Held(), 1);
		inv.DropHeld();
		CHECK_EQ(inv.Held(), -1);
	}

	g_test = "cursor: hand boxes (dialogue_ui.c:1693-1695, 1721-1723)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, KEY); inv.GiveItem(db, GUN);
		inv.SetLeftHand(-1); inv.SetRightHand(-1);
		inv.PickUpToCursor(1, &r);
		inv.HeldToLeftHand(&r);
		CHECK_EQ(inv.LeftHand(), 1);
		CHECK_EQ(inv.Held(), -1);
		CHECK(LogIs(r, { "equip(1,3)" }));
		// Nothing tests for a weapon: a key goes to the left hand the same way.
		inv.PickUpToCursor(0, &r);
		inv.HeldToLeftHand(&r);
		CHECK_EQ(inv.LeftHand(), 0);
		CHECK_EQ(r.lastEquipId, KEY);
		inv.PickUpToCursor(1, &r);
		r = Recorder();
		inv.HeldToRightHand(&r);
		CHECK_EQ(inv.RightHand(), 1);
		CHECK_EQ(inv.Held(), -1);
		CHECK(r.log.empty());                          // the icon is not refreshed here
		inv.HeldToRightHand(&r);                       // nothing held: nothing
		CHECK_EQ(inv.RightHand(), 1);
		inv.HeldToLeftHand(&r);
		CHECK_EQ(inv.LeftHand(), 0);
	}

	g_test = "commit_held_cursor_item and Enter (:2178-2196, :1710-1720)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, KEY); inv.GiveItem(db, GUN); inv.GiveItem(db, CHARACTER);
		inv.SetLeftHand(-1); inv.SetRightHand(-1);
		inv.CommitHeld(0, &r);                         // nothing held
		CHECK(r.log.empty());
		inv.PickUpToCursor(1, &r);
		inv.CommitHeld(2, &r);                         // list 2: nothing
		CHECK_EQ(inv.LeftHand(), -1); CHECK_EQ(inv.RightHand(), -1);
		inv.CommitHeld(1, &r);                         // list 1: equip
		CHECK_EQ(inv.LeftHand(), 1);
		CHECK(LogIs(r, { "equip(1,3)" }));
		CHECK_EQ(inv.Held(), 1);                       // commit does not let go; the panel close does
		inv.PickUpToCursor(0, &r);
		r = Recorder();
		inv.CommitHeld(roth::TabCategory(2), &r);      // any other list: select + icon
		CHECK_EQ(inv.RightHand(), 0);
		CHECK(LogIs(r, { "right(0,0)" }));

		roth::Inventory e; Recorder r2;
		e.GiveItem(db, KEY); e.GiveItem(db, GUN);
		e.SetLeftHand(-1); e.SetRightHand(-1);
		e.EnterOnSlot(2, 0, &r2);
		CHECK_EQ(e.RightHand(), -1);
		e.EnterOnSlot(1, 1, &r2);
		CHECK_EQ(e.LeftHand(), 1);
		e.EnterOnSlot(3, 0, &r2);
		CHECK_EQ(e.RightHand(), 0);
		e.EnterOnSlot(3, -1, &r2);                     // cursor past the end
		CHECK_EQ(e.RightHand(), 0);
		CHECK(LogIs(r2, { "equip(1,3)", "right(0,0)" }));
	}

	g_test = "restore_active_held_item (:2204-2244)";
	{
		roth::Inventory inv; Recorder r;
		inv.RestoreActiveHeldItem(db, &r);             // empty pack: nothing at all
		CHECK(r.log.empty());
		CHECK_EQ(inv.AdamSlot(), -1);
		inv.GiveItem(db, KEY); inv.GiveItem(db, ABSENT); inv.GiveItem(db, ADAM); inv.GiveItem(db, GUN);
		inv.SetLeftHand(-1);
		inv.RestoreActiveHeldItem(db, &r);
		CHECK_EQ(inv.AdamSlot(), 2);
		CHECK_EQ(inv.AdamItemId(), ADAM);
		CHECK(LogIs(r, { "unequip(0,0)" }));           // no weapon equipped: fists
		inv.SetLeftHand(3);
		r = Recorder();
		inv.RestoreActiveHeldItem(db, &r);
		CHECK(r.log.empty());                          // a weapon is equipped: left alone
		// No Adam carried: the old latch stands.
		roth::Inventory none; none.GiveItem(db, KEY);
		none.RestoreActiveHeldItem(db, &r);
		CHECK_EQ(none.AdamSlot(), -1);
		// Swap keeps the Adam slot on its item, in both directions (:658-664).
		inv.SwapSlots(0, 2);
		CHECK_EQ(inv.AdamSlot(), 0);
		inv.SwapSlots(0, 1);
		CHECK_EQ(inv.AdamSlot(), 1);
	}
}

void TestUse(const roth::Dbase100 &db)
{
	g_test = "resolve_item_use_action (:551-594)";
	{
		roth::UseAction a = roth::ResolveItemUseAction(db, db.Item(SELFUSE), 0);
		CHECK_EQ(a.packed, RES_A); CHECK_EQ(a.effect, 0);
		a = roth::ResolveItemUseAction(db, db.Item(SELFUSE2), 0);
		CHECK_EQ(a.packed, ((uint32_t)RES_A << 16) | RES_B);   // newest result is "second"
		CHECK_EQ(a.effect, 5);
		a = roth::ResolveItemUseAction(db, db.Item(EFFECTONLY), 0);
		CHECK_EQ(a.packed, 0xffffffffu); CHECK_EQ(a.effect, 7);
		a = roth::ResolveItemUseAction(db, db.Item(SELFUSE), KEY);     // wrong target
		CHECK_EQ(a.packed, 0);
		a = roth::ResolveItemUseAction(db, db.Item(NOFLAG), 0);        // the block is there, +5&0x40 is not
		CHECK_EQ(a.packed, 0);
		a = roth::ResolveItemUseAction(db, db.Item(KEY), 0);
		CHECK_EQ(a.packed, 0);
		a = roth::ResolveItemUseAction(db, nullptr, 0);
		CHECK_EQ(a.packed, 0);
		a = roth::ResolveItemUseAction(db, db.Item(COMBINER), T24);
		CHECK_EQ(a.packed, ((uint32_t)RES_A << 16) | RES_B); CHECK_EQ(a.effect, 9);
		a = roth::ResolveItemUseAction(db, db.Item(COMBINER), T25);
		CHECK_EQ(a.packed, 0xffffffffu); CHECK_EQ(a.effect, 11);
		// 0xB5 is not an effect (:583): the matching block has neither result
		// nor effect, the scan runs to the end, the answer is 0.
		a = roth::ResolveItemUseAction(db, db.Item(COMBINER), PLAIN26);
		CHECK_EQ(a.packed, 0); CHECK_EQ(a.effect, 0);
		// No block matches: 0, and the effect of the LAST OnUse block scanned
		// is left behind (here that block has none).
		a = roth::ResolveItemUseAction(db, db.Item(COMBINER), 0);
		CHECK_EQ(a.packed, 0);
		a = roth::ResolveItemUseAction(db, db.Item(SELFUSE2), KEY);    // one block, effect 5, wrong target
		CHECK_EQ(a.packed, 0); CHECK_EQ(a.effect, 5);
	}

	g_test = "use_item_on_self: no recipe (:716-733)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, KEY);
		inv.UseHeldOnSelf(db, &r);                     // nothing held [unverified]: nothing
		CHECK(r.log.empty());
		inv.PickUpToCursor(0);
		inv.UseHeldOnSelf(db, &r);
		CHECK(LogIs(r, { "msg(2a,0,1010,0)" }));
		CHECK_EQ(inv.Held(), 0);                       // still on the cursor
		CHECK_EQ(inv.Count(), 1);
	}

	g_test = "use_item_on_self: one result (:744-759)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, KEY); inv.GiveItem(db, SELFUSE);
		inv.PickUpToCursor(1);                         // empties the right hand silently
		inv.UseHeldOnSelf(db, &r);
		CHECK_EQ(inv.Slot(1).id, RES_A);               // the freed slot is reused
		CHECK_EQ(inv.Count(), 2);
		CHECK_EQ(inv.RightHand(), 1);
		CHECK_EQ(inv.Held(), -1);
		// consume (rebuild), give (select), THEN the message, then the refresh.
		CHECK(LogIs(r, { "rebuild(0,0)", "right(1,0)", "msg(2d,5,10f0,0)", "refresh(0,0)" }));
	}

	g_test = "use_item_on_self: two results, low word first; effect before refresh (:746-775)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, SELFUSE2);
		inv.PickUpToCursor(0);
		inv.UseHeldOnSelf(db, &r);
		CHECK_EQ(inv.Slot(0).id, RES_B);               // "second" (the low word) is given first
		CHECK_EQ(inv.Slot(1).id, RES_A);
		CHECK_EQ(inv.RightHand(), 1);
		CHECK(LogIs(r, { "rebuild(0,0)", "right(0,0)", "right(1,0)", "msg(2d,5,1120,0)",
			"action(5,0)", "refresh(0,0)" }));
		// A close request suppresses the refresh (:774).
		roth::Inventory inv2; Recorder r2; r2.closeRequested = true;
		inv2.GiveItem(db, SELFUSE);
		inv2.PickUpToCursor(0);
		inv2.UseHeldOnSelf(db, &r2);
		CHECK_EQ(r2.refreshes, 0);
	}

	g_test = "use_item_on_self: effect-only is NOT consumed (:744)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, EFFECTONLY);
		inv.PickUpToCursor(0);
		inv.UseHeldOnSelf(db, &r);
		CHECK_EQ(inv.Slot(0).id, EFFECTONLY);
		CHECK_EQ(inv.Count(), 1);
		CHECK_EQ(inv.Held(), -1);
		CHECK(LogIs(r, { "msg(2d,5,1130,0)", "action(7,0)", "refresh(0,0)" }));
	}

	g_test = "use_item_on_self: a full pack swallows the result (give after consume)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, SELFUSE);
		for (int i = 1; i < roth::Inventory::SLOT_COUNT; i++) inv.GiveItem(db, KEY);
		CHECK_EQ(inv.Count(), 256);
		inv.PickUpToCursor(0);
		inv.UseHeldOnSelf(db, &r);                     // consume frees a slot, so the give fits
		CHECK_EQ(inv.Slot(0).id, RES_A);
		CHECK_EQ(inv.Count(), 256);
	}
}

// Sets up: slot 0 = COMBINER (held), slot 1 = the target, slot 2 = a bystander.
// The set-up gives are not recorded; the log starts at the combine.
void Combine(const roth::Dbase100 &db, roth::Inventory &inv, Recorder &r, int target)
{
	inv.GiveItem(db, COMBINER); inv.GiveItem(db, target); inv.GiveItem(db, T21 == target ? T22 : T21);
	inv.PickUpToCursor(0);
	inv.CombineHeldWith(db, 1, &r);
}

void TestCombine(const roth::Dbase100 &db)
{
	const std::string ok = "msg(28,5,1140,";       // "combined", held name first

	g_test = "combine: no recipe (:826-843)";
	{
		roth::Inventory inv; Recorder r;
		Combine(db, inv, r, RES_B);                    // RES_B is in no recipe
		CHECK(LogIs(r, { "msg(29,0,1140,1110)" }));
		CHECK_EQ(inv.Held(), 0);                       // nothing changes; still held
		CHECK_EQ(inv.Count(), 3);
		// A recipe whose only content is a 0xB5 word is no recipe.
		roth::Inventory inv2; Recorder r2;
		Combine(db, inv2, r2, PLAIN26);
		CHECK(LogIs(r2, { "msg(29,0,1140,11a0)" }));
		// Nothing held [unverified]: nothing.
		roth::Inventory inv3; Recorder r3;
		inv3.GiveItem(db, COMBINER); inv3.GiveItem(db, KEY);
		inv3.CombineHeldWith(db, 1, &r3);
		CHECK(r3.log.empty());
		CHECK_EQ(inv3.Count(), 2);
	}

	g_test = "combine: one result -> consume held, consume target, give (:888-891)";
	{
		roth::Inventory inv; Recorder r;
		Combine(db, inv, r, KEY);
		CHECK_EQ(inv.Slot(0).id, RES_A);
		CHECK_EQ(inv.Slot(1).id, 0);
		CHECK_EQ(inv.Count(), 2);
		CHECK_EQ(inv.Held(), -1);
		// The message comes BEFORE the dispatch here (unlike use-on-self).
		CHECK(LogIs(r, { ok + "1010)", "rebuild(0,0)", "rebuild(0,0)", "right(0,0)", "refresh(0,0)" }));
	}

	g_test = "combine: first == held -> consume target, give second (:872-875)";
	{
		roth::Inventory inv; Recorder r;
		Combine(db, inv, r, POTION);
		CHECK_EQ(inv.Slot(0).id, COMBINER);            // the held item stays
		CHECK_EQ(inv.Slot(1).id, RES_B);
		CHECK_EQ(inv.Count(), 3);
		CHECK_EQ(inv.Held(), -1);                      // "kept" does not survive the refresh (:1317)
	}

	g_test = "combine: second == held -> consume target, give first (:868-871)";
	{
		roth::Inventory inv; Recorder r;
		Combine(db, inv, r, T21);
		CHECK_EQ(inv.Slot(0).id, COMBINER);
		CHECK_EQ(inv.Slot(1).id, RES_B);
		CHECK_EQ(inv.Count(), 3);
		CHECK_EQ(inv.Held(), -1);
	}

	g_test = "combine: second == target -> consume held, give first (:876-878)";
	{
		roth::Inventory inv; Recorder r;
		Combine(db, inv, r, T22);
		CHECK_EQ(inv.Slot(0).id, RES_A);
		CHECK_EQ(inv.Slot(1).id, T22);                 // the target stays
		CHECK_EQ(inv.Count(), 3);
	}

	g_test = "combine: first == target -> consume held, give second (:879-881)";
	{
		roth::Inventory inv; Recorder r;
		Combine(db, inv, r, T23);
		CHECK_EQ(inv.Slot(0).id, RES_A);
		CHECK_EQ(inv.Slot(1).id, T23);
		CHECK_EQ(inv.Count(), 3);
	}

	g_test = "combine: neither -> consume both, give second THEN first; effect after refresh (:882-927)";
	{
		roth::Inventory inv; Recorder r;
		Combine(db, inv, r, T24);
		CHECK_EQ(inv.Slot(0).id, RES_B);               // second (the newest 0xB4) lands first
		CHECK_EQ(inv.Slot(1).id, RES_A);
		CHECK_EQ(inv.Count(), 3);
		CHECK_EQ(inv.RightHand(), 1);
		CHECK(LogIs(r, { ok + "1180)", "rebuild(0,0)", "rebuild(0,0)", "right(0,0)", "right(1,0)",
			"refresh(0,0)", "action(9,0)" }));
	}

	g_test = "combine: effect-only consumes nothing (:864, :893)";
	{
		roth::Inventory inv; Recorder r;
		Combine(db, inv, r, T25);
		CHECK_EQ(inv.Slot(0).id, COMBINER);
		CHECK_EQ(inv.Slot(1).id, T25);
		CHECK_EQ(inv.Held(), -1);
		CHECK(LogIs(r, { ok + "1190)", "refresh(0,0)", "action(11,0)" }));
	}

	g_test = "combine: the recipe may sit on the target (:808-809)";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, PLAIN26); inv.GiveItem(db, HOLDER27);
		inv.PickUpToCursor(0);
		inv.CombineHeldWith(db, 1, &r);
		CHECK_EQ(inv.Slot(0).id, RES_A);
		CHECK_EQ(inv.Count(), 1);
		// The names are still held first, target second.
		CHECK(!r.log.empty() && r.log[0] == "msg(28,5,11a0,11b0)");
		// The held item is tried first: COMBINER held on HOLDER27 has no recipe
		// for 27, and 27 has none for COMBINER.
		roth::Inventory inv2; Recorder r2;
		Combine(db, inv2, r2, HOLDER27);
		CHECK(!r2.log.empty() && r2.log.back() == "msg(29,0,1140,11b0)");
	}

	g_test = "combine: a hidden target is a negative id and matches no recipe";
	{
		roth::Inventory inv; Recorder r;
		inv.GiveItem(db, COMBINER); inv.GiveItem(db, KEY);
		inv.SetFilter(db, 0);
		inv.SetFilter(db, 1);
		auto bytes = inv.Serialize();
		bytes[1 * 4 + 1] |= 0x80;                      // hide the key only
		inv.Deserialize(bytes.data(), bytes.size());
		inv.PickUpToCursor(0);
		inv.CombineHeldWith(db, 1, &r);
		CHECK(LogIs(r, { "msg(29,0,1140,0)" }));       // no record for a negative id: name 0 [unverified]
	}
}

void TestListsAndLabels(roth::Dbase100 &db)
{
	g_test = "tabs (0x7123c = 0,1,3,4,2; :2153-2161)";
	CHECK_EQ(roth::TabCategory(0), 0);
	CHECK_EQ(roth::TabCategory(1), 1);
	CHECK_EQ(roth::TabCategory(2), 3);
	CHECK_EQ(roth::TabCategory(3), 4);
	CHECK_EQ(roth::TabCategory(4), 2);
	CHECK_EQ(roth::ItemTabIndex(db, GUN), 1);
	CHECK_EQ(roth::ItemTabIndex(db, KEY), 2);
	CHECK_EQ(roth::ItemTabIndex(db, POTION), 3);
	CHECK_EQ(roth::ItemTabIndex(db, CHARACTER), 4);
	CHECK_EQ(roth::ItemTabIndex(db, 0), 0);
	CHECK_EQ(roth::ItemTabIndex(db, ABSENT), 0);       // [unverified]

	g_test = "build_inventory_entry_list (:1314-1368)";
	{
		roth::Inventory inv; Recorder r; r.db = &db;
		inv.GiveItem(db, KEY);         // 0
		inv.GiveItem(db, GUN);         // 1  left
		inv.GiveItem(db, CHARACTER);   // 2
		inv.GiveItem(db, T21);         // 3
		inv.GiveItem(db, ADAM);        // 4
		inv.GiveItem(db, T22);         // 5  right
		inv.RestoreActiveHeldItem(db);
		inv.PickUpToCursor(3);
		roth::InventoryEntryList l = inv.BuildEntryList(db, 3, &r);
		CHECK_EQ(inv.Held(), -1);                      // :1317
		CHECK_EQ(l.slots.size(), 3u);
		CHECK_EQ(l.slots[0], 0); CHECK_EQ(l.slots[1], 3); CHECK_EQ(l.slots[2], 5);
		CHECK_EQ(l.rightEntry, 2);
		CHECK_EQ(l.leftEntry, -1);                     // the gun is on another tab
		CHECK_EQ(l.shownLeft, 1); CHECK_EQ(l.shownRight, 5);   // the hand boxes show on every tab
		l = inv.BuildEntryList(db, 1, &r);
		CHECK_EQ(l.slots.size(), 1u);
		CHECK_EQ(l.leftEntry, 0);
		// Category 2: Adam always lists (+5&0x10); the character only with an
		// available topic (dialogue_ui.c:74-97).
		l = inv.BuildEntryList(db, 2, &r);
		CHECK_EQ(l.slots.size(), 1u);
		CHECK_EQ(l.slots[0], 4);
		CHECK_EQ(l.adamEntry, 0);
		r.evalByFirstWord[fx::W(0x0f, 0x222)] = 1;     // the second topic's condition holds
		r.log.clear();
		l = inv.BuildEntryList(db, 2, &r);
		CHECK_EQ(l.slots.size(), 2u);
		CHECK_EQ(l.slots[0], 2);
		CHECK_EQ(l.adamEntry, 1);
		CHECK(LogIs(r, { "eval(5,18)", "eval(5,18)" }));   // both topics scanned, mode 2
		// Hidden slots are passed over; category 2 is never hidden.
		inv.SetFilter(db, 0);
		l = inv.BuildEntryList(db, 3, &r);
		CHECK_EQ(l.slots.size(), 0u);
		CHECK_EQ(l.shownLeft, -1); CHECK_EQ(l.shownRight, -1);
		l = inv.BuildEntryList(db, 2, &r);
		CHECK_EQ(l.slots.size(), 2u);
	}

	g_test = "label text: ChangeName (:73-97, :1031-1039)";
	{
		Recorder r; r.db = &db;
		CHECK_EQ(roth::ItemLabelText(db, KEY, &r), NameOf(KEY));
		CHECK(r.log.empty());                          // no +4&0x04: nothing is evaluated
		CHECK_EQ(roth::ItemLabelText(db, RENAMED, &r), NameOf(RENAMED));   // both conditions false
		CHECK(LogIs(r, { "eval(28,32)", "eval(28,32)" }));                 // mode 0, each block in turn
		r.evalByFirstWord[fx::W(0x10, 0x700)] = 1;
		CHECK_EQ(roth::ItemLabelText(db, RENAMED, &r), 0x700);
		r.evalByFirstWord[fx::W(0x10, 0x600)] = -1;    // any non-zero result is true
		r.log.clear();
		CHECK_EQ(roth::ItemLabelText(db, RENAMED, &r), 0x600);
		CHECK_EQ(r.log.size(), 1u);                    // the first true block ends the walk
		CHECK_EQ(roth::ItemLabelText(db, 0, &r), 0);
		CHECK_EQ(roth::ItemLabelText(db, RENAMED, nullptr), NameOf(RENAMED));
	}

	g_test = "examine: first view sets +7 bit 0x80, then runs OnInspect (file_config.c:962-967)";
	{
		Recorder r; r.db = &db;
		CHECK_EQ(db.ItemRuntime(STAFF) & 0x80, 0);
		CHECK(roth::ExamineFirstView(db, STAFF, &r));
		CHECK_EQ(db.ItemRuntime(STAFF) & 0x80, 0x80);
		CHECK(LogIs(r, { "eval(4,64)" }));             // trigger 4, mode 0
		CHECK(!roth::ExamineFirstView(db, STAFF, &r)); // second view: nothing
		CHECK_EQ(r.log.size(), 1u);
		roth::ReplayInspect(db, STAFF, &r);            // the magnifier replays it
		CHECK_EQ(r.log.size(), 2u);
		// No OnInspect: the bit is still set, nothing runs.
		r.log.clear();
		CHECK(roth::ExamineFirstView(db, KEY, &r));
		CHECK_EQ(db.ItemRuntime(KEY), 0x80);
		CHECK(r.log.empty());
		roth::ReplayInspect(db, KEY, &r);
		CHECK(r.log.empty());
		CHECK(!roth::ExamineFirstView(db, ABSENT, &r));
		CHECK(!roth::ExamineFirstView(db, 0, &r));
		db.SetItemRuntime(STAFF, 0); db.SetItemRuntime(KEY, 0);
	}

	g_test = "world examine: OnInspect by render id (dialogue_ui.c:518-539)";
	{
		const roth::DbTriggerBlock *b = nullptr;
		CHECK_EQ(roth::FindInspectByRenderId(db, 0x213, &b), STAFF);
		CHECK(b != nullptr && b->trigger == 4);
		CHECK_EQ(roth::FindInspectByRenderId(db, 0x210, &b), 0);   // no +4&0x40
		CHECK(b == nullptr);
		CHECK_EQ(roth::FindInspectByRenderId(db, 0x150, &b), 0);
		CHECK_EQ(roth::FindInspectByRenderId(db, 0x2ff, nullptr), 0);
		CHECK(roth::FindInspectBlock(*db.Item(KEY)) == nullptr);
	}

	g_test = "available choice (dialogue_ui.c:74-97)";
	{
		Recorder r; r.db = &db;
		CHECK(roth::ItemHasAvailableChoice(db, ADAM, &r));
		CHECK(r.log.empty());                          // +5&0x10 answers at once
		CHECK(!roth::ItemHasAvailableChoice(db, KEY, &r));   // +4&0x08 clear
		CHECK(!roth::ItemHasAvailableChoice(db, CHARACTER, &r));
		r.evalByFirstWord[fx::W(0x0f, 0x111)] = 1;
		r.log.clear();
		CHECK(roth::ItemHasAvailableChoice(db, CHARACTER, &r));
		CHECK_EQ(r.log.size(), 1u);                    // the first true topic ends the walk
		CHECK(!roth::ItemHasAvailableChoice(db, CHARACTER, nullptr));
	}
}

void TestPickupLock(const roth::Dbase100 &db)
{
	g_test = "pickup lock (:32-37, :106-139; renderer.c:9880-9887)";
	roth::Inventory inv;
	inv.TickPickupLock(5);                             // unarmed: the timer does not move
	CHECK_EQ(inv.PickupLockTimer(), 0);
	roth::GiveContext ctx; ctx.id = 0x210;
	inv.GiveItem(db, KEY, &ctx);
	CHECK(inv.PickupLockArmed());
	inv.TickPickupLock(0x1b);
	CHECK(inv.PickupLockArmed());                      // 0x1b < 0x1c
	inv.TickPickupLock(1);
	CHECK(!inv.PickupLockArmed());                     // disarmed at 0x1c
	CHECK_EQ(inv.PickupLockTimer(), 0);
	CHECK_EQ(inv.PickupLockFlags(), 0);
	// give_item itself does not test the lock: a scripted give goes through.
	inv.GiveItem(db, KEY, &ctx);
	CHECK_EQ(inv.GiveItem(db, KEY, &ctx), KEY);
	// A reset clears the flags but not the timer, so a re-arm carries on.
	inv.TickPickupLock(0x10);
	inv.ResetPickupLock();
	CHECK(!inv.PickupLockArmed());
	CHECK_EQ(inv.PickupLockTimer(), 0x10);
	inv.BeginPickupLock();
	inv.TickPickupLock(0x0c);
	CHECK(!inv.PickupLockArmed());
	// One big step disarms at once.
	inv.BeginPickupLock();
	inv.TickPickupLock(100);
	CHECK(!inv.PickupLockArmed());
}

//==========================================================================
//
// The retail check. Not a test of expectations: it reports what the retail
// database does under these rules, and fails only on a contradiction of the
// rules themselves.
//
//==========================================================================

std::vector<uint8_t> ReadFile(const std::string &path)
{
	std::vector<uint8_t> out;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return out;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
	fclose(f);
	return out;
}

// DBASE400 entry: u32 voice, u16 length, u8 colour, u8, text (dbase100.c:382-392).
// For reading the report only.
std::string TextAt(const std::vector<uint8_t> &t, uint32_t off)
{
	if (t.empty() || (uint64_t)off + 8 > t.size()) return "?";
	size_t len = (size_t)t[off + 4] | ((size_t)t[off + 5] << 8);
	if (off + 8 + len > t.size()) len = t.size() - off - 8;
	std::string s((const char *)t.data() + off + 8, len);
	while (!s.empty() && (s.back() == 0 || s.back() == '\n' || s.back() == '\r')) s.pop_back();
	for (auto &c : s) if ((unsigned char)c < 0x20) c = ' ';
	return s;
}

void RetailCheck(const std::string &dir)
{
	std::vector<uint8_t> bytes = ReadFile(dir + "/DBASE100.DAT");
	if (bytes.empty())
	{
		printf("retail check: no DBASE100.DAT at %s -- skipped\n", dir.c_str());
		return;
	}
	const std::vector<uint8_t> text = ReadFile(dir + "/DBASE400.DAT");
	roth::Dbase100 db;
	if (!db.Load(bytes.data(), bytes.size()))
	{
		printf("retail check: load failed: %s\n", db.Error().c_str());
		g_failed++;
		return;
	}
	printf("\nRETAIL CHECK: %s\n", dir.c_str());
	printf("  %d items, %d actions, %d anomalies\n", db.ItemCount(), db.ActionCount(), db.AnomalyTotal());

	g_test = "retail (a): every item given once into a fresh inventory";
	struct Tally { int items = 0, given = 0, right = 0, left = 0, noHand = 0, absent = 0; std::map<int, int> qty; };
	std::map<int, Tally> byCat;     // -1 = no record
	int weapons = 0, shifted = 0, stackable = 0;
	std::string weaponLines;
	for (int id = 1; id <= db.ItemCount(); id++)
	{
		roth::Inventory inv; Recorder r;
		const roth::DbItem *rec = db.Item(id);
		const int cat = rec ? rec->Category() : -1;
		Tally &t = byCat[cat];
		t.items++;
		const int32_t got = inv.GiveItem(db, id, nullptr, &r);
		CHECK_EQ(got, id);
		CHECK_EQ(inv.Count(), 1);
		CHECK_EQ(inv.Slot(0).id, id);
		if (got == id) t.given++;
		if (inv.RightHand() == 0) t.right++;
		else if (inv.LeftHand() == 0) t.left++;
		else t.noHand++;
		t.qty[inv.Slot(0).quantity]++;
		if (rec == nullptr) { t.absent++; continue; }
		// The rules, restated against each record.
		const bool weapon = (rec->flags & roth::DBI_WEAPON) != 0;
		const bool stack = (rec->itemType & roth::DBT_STACKABLE) != 0;
		if (stack) stackable++;
		CHECK_EQ(inv.LeftHand() == 0, weapon);
		CHECK_EQ(inv.RightHand() == 0, !weapon && cat != 2);
		if (stack) CHECK_EQ(inv.Slot(0).quantity, 1);
		else if (!weapon) CHECK_EQ(inv.Slot(0).quantity, 0);
		// A second give: one more for a stack, refused-with-index for category 2.
		const int32_t again = inv.GiveItem(db, id, nullptr, &r);
		CHECK_EQ(again, id);
		if (stack) { CHECK_EQ(inv.Count(), 1); CHECK_EQ(inv.Slot(0).quantity, 2); }
		else if (cat == 2) CHECK_EQ(inv.Count(), 1);
		else CHECK_EQ(inv.Count(), 2);
		if (weapon)
		{
			weapons++;
			const bool sh = roth::WeaponQuantityIsShifted(db, *rec);
			if (sh) shifted++;
			char line[200];
			snprintf(line, sizeof line, "    %3d %-28.28s render %04x  attrs+4 %d%s -> slot quantity 0x%04x%s\n",
				id, TextAt(text, rec->name).c_str(), rec->renderId,
				stack ? 0 : (sh ? roth::WeaponGiveQuantity(db, *rec) >> 8 : roth::WeaponGiveQuantity(db, *rec)),
				sh ? " <<8" : "", stack ? 1 : (unsigned)(uint16_t)roth::WeaponGiveQuantity(db, *rec),
				stack ? " (stackable: 1)" : "");
			weaponLines += line;
		}
	}
	printf("  (a) give every item once, fresh inventory each time:\n");
	printf("      cat  items given  right-hand left-hand no-hand  quantities\n");
	for (const auto &kv : byCat)
	{
		const Tally &t = kv.second;
		if (kv.first < 0) printf("      none");
		else printf("      %-4d", kv.first);
		printf(" %5d %5d  %10d %9d %7d  ", t.items, t.given, t.right, t.left, t.noHand);
		for (const auto &q : t.qty) printf("%dx q=0x%x  ", q.second, q.first);
		printf("\n");
	}
	printf("      %d weapons (%d with quantity = rounds << 8), %d stackable\n", weapons, shifted, stackable);
	printf("%s", weaponLines.c_str());

	// Pickability of every render id, and whether give-by-render-id agrees.
	int pickable = 0, shared = 0, sharedWorld = 0, disagree = 0;
	std::map<int32_t, int> seenRender;
	for (int id = 1; id <= db.ItemCount(); id++)
	{
		const roth::DbItem *rec = db.Item(id);
		if (!rec) continue;
		const int32_t rid = rec->renderId;
		if (seenRender[rid]++) { shared++; if (rid >= 0x200) sharedWorld++; continue; }
		roth::Inventory inv;
		const bool can = roth::IsItemIdPickable(db, rid);
		const int32_t got = inv.GiveItemByRenderId(db, rid);
		if (can) pickable++;
		// When the first record is pickable the two must agree on it.
		if (can) CHECK_EQ(got, id);
		// When it is not, give-by-id walks on and may still find a later record.
		if (!can && got != 0) disagree++;
	}
	printf("      render ids: %d distinct, %d pickable; %d records repeat an earlier id (%d of them >= 0x200);\n"
		"      %d id(s) where the first record is unpickable but give_item_by_dbase_id finds a later one\n",
		(int)seenRender.size(), pickable, shared, sharedWorld, disagree);

	g_test = "retail (b): the new-game gives of action 3";
	const roth::DbAction *a3 = db.Action(3);
	if (a3 == nullptr)
	{
		printf("  (b) action 3 is missing\n");
		g_failed++;
		return;
	}
	printf("  (b) action 3, %d commands:", a3->commandCount);
	roth::Inventory inv; Recorder r;
	inv.Reset();                                       // game_core.c:822 reset_inventory
	int others = 0;
	for (int i = 0; i < a3->commandCount; i++)
	{
		const roth::DbCommand c = db.ActionCommand(*a3, i);
		printf(" %02x:%x", c.FullOpcode(), c.operand);
		if (c.opcode != 0x11) { others++; continue; }
		// dbase100.c:770-771: 0x11 gives (int16)operand, 0x91 removes it.
		const int32_t arg = (int32_t)(int16_t)(uint16_t)c.operand;
		if (!c.ifNot) CHECK_EQ(inv.GiveItem(db, arg, nullptr, &r), arg);
		else inv.RemoveItem(db, (uint16_t)arg, &r);
	}
	printf("\n");
	if (others) printf("      NOTE: %d command(s) are not 0x11/0x91 and were not run (no interpreter here)\n", others);
	inv.RestoreActiveHeldItem(db, &r);                 // game_core.c:825
	inv.SetLeftHand(-1); r.UnequipWeapon();            // game_core.c:830 activate_weapon_item(0,0)
	printf("      inventory after reset + action 3 + restore_active_held_item + activate_weapon_item(0,0):\n");
	for (int i = 0; i < roth::Inventory::SLOT_COUNT; i++)
	{
		const roth::InventorySlot &s = inv.Slot(i);
		if (s.id == 0) continue;
		const roth::DbItem *rec = db.Item(s.id);
		printf("      slot %d: item %d qty %d  cat %d  tab %d  \"%s\"%s%s%s\n", i, s.id, s.quantity,
			rec ? rec->Category() : -1, roth::ItemTabIndex(db, s.id),
			rec ? TextAt(text, roth::ItemLabelText(db, s.id, nullptr)).c_str() : "(no record)",
			i == inv.RightHand() ? "  [RIGHT HAND]" : "", i == inv.LeftHand() ? "  [LEFT HAND]" : "",
			i == inv.AdamSlot() ? "  [ADAM]" : "");
	}
	printf("      count %d, left hand %d, right hand %d, Adam slot %d (item %d)\n",
		inv.Count(), inv.LeftHand(), inv.RightHand(), inv.AdamSlot(), (int)inv.AdamItemId());
	printf("      sink calls:");
	for (const auto &e : r.log) printf(" %s", e.c_str());
	printf("\n");
}

} // namespace

int main(int argc, char **argv)
{
	roth::Dbase100 db = MakeDb();
	if (!db.IsLoaded()) return 1;
	TestPickable(db);
	TestFreeSlot();
	TestQuery(db);
	TestRemove(db);
	TestFilter(db);
	TestSwapAndSave(db);

	roth::Dbase100 db2 = MakeDb2();
	if (!db2.IsLoaded()) return 1;
	if (db2.AnomalyTotal() != 0) { printf("fixture 2 has anomalies\n"); return 1; }
	TestGive(db2);
	TestGiveByRenderId(db2);
	TestAudit(db2);
	TestAutoselect(db2);
	TestHands(db2);
	TestUse(db2);
	TestCombine(db2);
	TestListsAndLabels(db2);
	TestPickupLock(db2);
	const int fixtureChecks = g_checks, fixtureFailed = g_failed;
	printf("%d fixture checks, %d failed\n", fixtureChecks, fixtureFailed);

	RetailCheck(argc > 1 ? argv[1] : "/mnt/user-data/uploads/Realms of the Haunting/DATA");
	if (g_checks != fixtureChecks)
		printf("%d retail checks, %d failed\n", g_checks - fixtureChecks, g_failed - fixtureFailed);

	printf("%d checks, %d failed\n", g_checks, g_failed);
	printf("%s\n", g_failed == 0 ? "ALL INVENTORY TESTS PASS" : "*** INVENTORY TESTS FAILED ***");
	return g_failed == 0 ? 0 : 1;
}
