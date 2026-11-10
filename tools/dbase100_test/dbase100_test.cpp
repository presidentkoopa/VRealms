//
// Tests for the DBASE100.DAT reader (src/roth/roth_dbase100.{h,cpp}) against
// synthetic files built byte by byte in fixture.h. No retail data is needed or
// used. Build and run with tools/dbase100_test/build.sh (or build.cmd).
//
// Every malformed case asserts two things: the reader does not crash or read
// out of bounds (the sanitizer build proves that), and the problem is VISIBLE
// -- either the load fails with an Error(), or a named anomaly is counted.
//

#include "fixture.h"
#include "../../src/roth/roth_dbase100.h"
#include "../../src/roth/roth_install.h"
#include "../../src/roth/roth_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <random>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

namespace
{

int g_checks = 0, g_failed = 0;
const char *g_test = "";

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
	printf("  FAIL [%s] %s:%d: %s\n", g_test, __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long _a = (long long)(a), _b = (long long)(b); \
	if (_a != _b) { g_failed++; printf("  FAIL [%s] %s:%d: %s == %s (%lld vs %lld)\n", \
	g_test, __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

int Anom(const roth::Dbase100 &db, const char *name)
{
	auto it = db.Anomalies().find(name);
	return it == db.Anomalies().end() ? 0 : it->second;
}

bool HasNote(const roth::Dbase100 &db, const std::string &needle)
{
	for (const auto &n : db.Notes()) if (n.find(needle) != std::string::npos) return true;
	return false;
}

// A small but complete database: three items (one absent), three actions
// (one absent), two cutscenes, four interface texts.
fx::Spec GoodSpec()
{
	using fx::W;
	fx::Spec s;
	s.unk0C = 433;          // so the flag-bitmap note has something to find
	s.field30 = 2;

	fx::ItemSpec staff;     // shaped after the Creator's Staff (GAME_inventory.md §10 test 2)
	staff.renderId = 0x294;
	staff.flags = 0x40 | 0x08;              // OnInspect, OnInformation
	staff.itemType = 0x03;                  // category 3
	staff.unk06 = 0x5a;
	staff.runtime = 0x00;
	staff.closeup = 0x1234;
	staff.icon = 0x55;
	staff.name = 0xabcdef;
	staff.blocks.push_back({ 0x04, { W(0x05, 0x001000) } });                  // OnInspect: speak
	staff.blocks.push_back({ 0x01, { W(0x0f, 0x2000), W(0x01, 17, true) } }); // topic, gate
	staff.blocks.push_back({ 0x01, { W(0x0f, 0x2100) } });
	s.items.push_back(staff);

	fx::ItemSpec none; none.absent = true;
	s.items.push_back(none);

	fx::ItemSpec gun;
	gun.renderId = 0x2a0;
	gun.flags = 0x10 | 0x20;                // weapon, bullet
	gun.itemType = 0x21;                    // stackable, category 1
	gun.blocks.push_back({ 0x05, { W(0x13, 7), W(0x14, 8) } });
	gun.blocks.push_back({ 0x06, { W(0x20, 3) } });
	gun.lengthMode = 1;
	s.items.push_back(gun);

	fx::ActionSpec boot;
	boot.unk02 = 0x7777;
	boot.words = { W(0x07, 1), W(0x04, 12), W(0x04, 13, true), W(0x11, 5, true), W(0x0a, 0xffffff) };
	s.actions.push_back(boot);
	fx::ActionSpec gap; gap.absent = true;
	s.actions.push_back(gap);
	fx::ActionSpec jump;
	jump.words = { W(0x1d, 1) };
	s.actions.push_back(jump);

	fx::CutsceneSpec c0; c0.name = "INTRO"; c0.unk08 = 9; c0.subtitleSize = -2;
	c0.titleText = 0x400; c0.subtitleText = 0x123456; c0.gallery = 0;
	fx::CutsceneSpec c1; c1.name = "ABCDEFGH"; c1.subtitleSize = 300; c1.gallery = 7;
	s.cutscenes = { c0, c1 };

	s.interface = { 0x10, 0x20, 0x30, 0 };
	return s;
}

void TestGoodFile()
{
	g_test = "good file";
	fx::Layout L;
	auto bytes = fx::Build(GoodSpec(), &L);
	roth::Dbase100 db;
	CHECK(db.Load(bytes.data(), bytes.size()));
	CHECK(db.IsLoaded());
	CHECK(db.Error().empty());
	CHECK_EQ(db.AnomalyTotal(), 0);
	for (auto &kv : db.Anomalies()) printf("    unexpected anomaly: %s x%d\n", kv.first.c_str(), kv.second);

	const roth::DbHeader &h = db.Header();
	CHECK_EQ(h.fileSize, bytes.size());
	CHECK_EQ(h.unk0C, 433);
	CHECK_EQ(h.inventoryCount, 3);
	CHECK_EQ(h.inventoryTable, L.inventoryTable);
	CHECK_EQ(h.actionCount, 3);
	CHECK_EQ(h.actionTable, L.actionTable);
	CHECK_EQ(h.cutsceneCount, 2);
	CHECK_EQ(h.cutsceneTable, L.cutsceneTable);
	CHECK_EQ(h.interfaceCount, 4);
	CHECK_EQ(h.interfaceTable, L.interfaceTable);
	CHECK_EQ(h.field30, 2);

	// Items: 1-based.
	CHECK_EQ(db.ItemCount(), 3);
	CHECK(db.Item(0) == nullptr);
	CHECK(db.Item(-1) == nullptr);
	CHECK(db.Item(4) == nullptr);
	CHECK(db.Item(2) == nullptr);           // table entry 0
	CHECK(db.ItemSlot(2) == nullptr);

	const roth::DbItem *staff = db.Item(1);
	CHECK(staff != nullptr);
	if (staff)
	{
		CHECK_EQ(staff->id, 1);
		CHECK_EQ(staff->offset, L.itemOffsets[0]);
		CHECK_EQ(staff->renderId, 0x294);
		CHECK_EQ(staff->flags, 0x48);
		CHECK_EQ(staff->itemType, 0x03);
		CHECK_EQ(staff->Category(), 3);
		CHECK(!staff->IsWeapon());
		CHECK(!staff->IsStackable());
		CHECK_EQ(staff->unk06, 0x5a);
		CHECK_EQ(staff->runtimeAtLoad, 0);
		CHECK_EQ(staff->closeup, 0x1234);
		CHECK_EQ(staff->icon, 0x55);
		CHECK_EQ(staff->name, 0xabcdef);
		CHECK(staff->terminated);
		CHECK_EQ(staff->blocks.size(), 3);
		CHECK_EQ(staff->length, staff->endOffset - staff->offset);
		const roth::DbTriggerBlock *insp = staff->FindBlock(roth::DBTRIG_INSPECT);
		CHECK(insp != nullptr);
		if (insp)
		{
			CHECK_EQ(insp->size, 8);
			CHECK_EQ(insp->commandCount, 1);
			auto cmds = db.Commands(*insp);
			CHECK_EQ(cmds.size(), 1);
			if (!cmds.empty())
			{
				CHECK_EQ(cmds[0].opcode, 0x05);
				CHECK(!cmds[0].ifNot);
				CHECK_EQ(cmds[0].operand, 0x001000);
			}
		}
		// FindBlock returns the FIRST OnInformation block.
		const roth::DbTriggerBlock *info = staff->FindBlock(roth::DBTRIG_INFORMATION);
		CHECK(info != nullptr);
		if (info)
		{
			CHECK_EQ(info->commandCount, 2);
			roth::DbCommand gate = db.BlockCommand(*info, 1);
			CHECK_EQ(gate.opcode, 0x01);
			CHECK(gate.ifNot);
			CHECK_EQ(gate.FullOpcode(), 0x81);
			CHECK_EQ(gate.operand, 17);
			CHECK_EQ(db.BlockCommand(*info, 2).raw, 0);   // past the end: empty word
		}
		CHECK(staff->FindBlock(roth::DBTRIG_ON_USE) == nullptr);
	}

	const roth::DbItem *gun = db.Item(3);
	CHECK(gun != nullptr);
	if (gun)
	{
		CHECK(gun->IsWeapon());
		CHECK(gun->IsStackable());
		CHECK_EQ(gun->Category(), 1);
		CHECK_EQ(gun->blocks.size(), 2);
		CHECK_EQ(gun->length + 4u, gun->endOffset - gun->offset);   // lengthMode 1
	}

	// Render-id lookup.
	CHECK_EQ(db.FindItemByRenderId(0x294), 1);
	CHECK_EQ(db.FindItemByRenderId(0x2a0), 3);
	CHECK_EQ(db.FindItemByRenderId(0x999), 0);
	CHECK_EQ(db.DuplicateRenderIds(), 0);

	// Actions.
	CHECK_EQ(db.ActionCount(), 3);
	CHECK(db.Action(0) == nullptr);
	CHECK(db.Action(2) == nullptr);
	CHECK(db.Action(4) == nullptr);
	const roth::DbAction *boot = db.Action(1);
	CHECK(boot != nullptr);
	if (boot)
	{
		CHECK_EQ(boot->offset, L.actionOffsets[0]);
		CHECK_EQ(boot->length, 4 + 5 * 4);
		CHECK_EQ(boot->unk02, 0x7777);
		CHECK_EQ(boot->commandCount, 5);
		CHECK_EQ(boot->firstCommand, boot->offset + 4);
		auto cmds = db.Commands(*boot);
		CHECK_EQ(cmds.size(), 5);
		if (cmds.size() == 5)
		{
			CHECK_EQ(cmds[0].opcode, 0x07); CHECK_EQ(cmds[0].operand, 1);
			CHECK_EQ(cmds[2].FullOpcode(), 0x84);
			CHECK_EQ(cmds[3].FullOpcode(), 0x91);   // remove item
			CHECK_EQ(cmds[3].opcode, 0x11);
			CHECK_EQ(cmds[4].operand, 0xffffff);    // marker operand
		}
	}
	const roth::DbAction *jump = db.Action(3);
	CHECK(jump != nullptr && jump->commandCount == 1);

	// Cutscenes: 0-based.
	CHECK_EQ(db.CutsceneCount(), 2);
	const roth::DbCutscene *c0 = db.Cutscene(0);
	CHECK(c0 != nullptr);
	if (c0)
	{
		CHECK(c0->name == "INTRO");
		CHECK_EQ(c0->unk08, 9);
		CHECK_EQ(c0->subtitleSize, -2);
		CHECK_EQ(c0->titleText, 0x400);
		CHECK_EQ(c0->subtitleText, 0x123456);
		CHECK_EQ(c0->galleryAtLoad, 0);
	}
	const roth::DbCutscene *c1 = db.Cutscene(1);
	CHECK(c1 != nullptr);
	if (c1)
	{
		CHECK(c1->name == "ABCDEFGH");      // all 8 bytes, no NUL
		CHECK_EQ(c1->subtitleSize, 300);
		CHECK_EQ(c1->galleryAtLoad, 7);
		CHECK_EQ(c1->subtitleText, 0);      // gallery byte NOT folded into the offset
	}
	CHECK(db.Cutscene(2) == nullptr);
	CHECK(db.Cutscene(-1) == nullptr);

	// Interface: 0-based, raw.
	CHECK_EQ(db.InterfaceCount(), 4);
	uint32_t v = 99;
	CHECK(db.InterfaceEntry(0, v) && v == 0x10);
	CHECK(db.InterfaceEntry(3, v) && v == 0);
	CHECK(!db.InterfaceEntry(4, v));
	CHECK(!db.InterfaceEntry(-1, v));

	// Runtime bytes write into the image.
	CHECK_EQ(db.ItemRuntime(1), 0);
	db.SetItemRuntime(1, roth::DBI_RUNTIME_EXAMINED);
	CHECK_EQ(db.ItemRuntime(1), 0x80);
	CHECK_EQ(db.Image()[L.itemOffsets[0] + 7], 0x80);
	CHECK_EQ(db.Item(1)->runtimeAtLoad, 0);
	CHECK_EQ(db.CutsceneGallery(1), 7);
	db.SetCutsceneGallery(0, 3);
	CHECK_EQ(db.CutsceneGallery(0), 3);
	CHECK_EQ(db.Cutscene(0)->subtitleText, 0x123456);
	// The caller's buffer is untouched (Load copies).
	CHECK_EQ(bytes[L.itemOffsets[0] + 7], 0);

	// Notes.
	CHECK(HasNote(db, "header fields equal to the documented 433: +0x0C"));
	CHECK(HasNote(db, "inventory: 3 entries, 1 empty, 0 malformed"));
	CHECK(HasNote(db, "1 equal the span through the end marker, 1 the span before it, 0 neither"));
	CHECK(HasNote(db, "+4&0x40 OnInspect"));
	CHECK(HasNote(db, "actions: 3 entries, 1 empty, 0 malformed"));
	CHECK(HasNote(db, "header +0x30 = 2"));
}

void TestCommandDecode()
{
	g_test = "command word";
	roth::DbCommand c = roth::DecodeDbCommand(0x91000005u);
	CHECK_EQ(c.opcode, 0x11); CHECK(c.ifNot); CHECK_EQ(c.operand, 5);
	c = roth::DecodeDbCommand(0x34ffffffu);
	CHECK_EQ(c.opcode, 0x34); CHECK(!c.ifNot); CHECK_EQ(c.operand, 0xffffff);
	c = roth::DecodeDbCommand(0xB4010002u);
	CHECK_EQ(c.opcode, 0x34); CHECK(c.ifNot); CHECK_EQ(c.operand, 0x010002);
	CHECK_EQ(c.FullOpcode(), 0xB4);
	c = roth::DecodeDbCommand(0);
	CHECK_EQ(c.opcode, 0); CHECK_EQ(c.operand, 0);
}

void TestBitmapSize()
{
	g_test = "flag bitmap size";
	// docs/GAME_core.md §6: 433 -> 56 bytes, 448 bits.
	CHECK_EQ(roth::Dbase100::FlagBitmapBytes(433), 56);
	CHECK_EQ(roth::Dbase100::FlagBitmapBytes(0), 4);
	CHECK_EQ(roth::Dbase100::FlagBitmapBytes(31), 4);
	CHECK_EQ(roth::Dbase100::FlagBitmapBytes(32), 8);
	CHECK_EQ(roth::Dbase100::FlagBitmapBytes(281), 36);
	CHECK_EQ(roth::Dbase100::FlagBitmapBytes(694), 88);
}

void TestStructuralFailures()
{
	g_test = "structural";
	roth::Dbase100 db;

	CHECK(!db.Load(nullptr, 0));
	CHECK(!db.Error().empty());

	auto good = fx::Build(GoodSpec());
	CHECK(!db.Load(good.data(), 0x33));
	CHECK(db.Error().find("smaller than") != std::string::npos);

	auto bad = good; bad[0] = 'X';
	CHECK(!db.Load(bad.data(), bad.size()));
	CHECK(db.Error().find("signature") != std::string::npos);
	CHECK_EQ(db.ItemCount(), 0);
	CHECK(!db.IsLoaded());

	// Each table pushed past the end of the file must fail the load.
	const size_t countAt[] = { 0x10, 0x18, 0x20, 0x28 };
	for (size_t k = 0; k < 4; k++)
	{
		auto t = good;
		fx::Put32(t, countAt[k], 0x00100000u);
		CHECK(!db.Load(t.data(), t.size()));
		CHECK(db.Error().find("outside") != std::string::npos);
		CHECK_EQ(db.ItemCount() + db.ActionCount() + db.CutsceneCount() + db.InterfaceCount(), 0);
	}
	// A table offset inside the header is also refused.
	{
		auto t = good;
		fx::Put32(t, 0x14, 0x10);
		CHECK(!db.Load(t.data(), t.size()));
	}
	// Counts that overflow 32 bits when multiplied must not wrap into range.
	{
		auto t = good;
		fx::Put32(t, 0x10, 0x40000001u);
		CHECK(!db.Load(t.data(), t.size()));
	}
	// A failed load after a good one leaves nothing behind.
	CHECK(db.Load(good.data(), good.size()));
	CHECK(!db.Load(bad.data(), bad.size()));
	CHECK(db.Item(1) == nullptr);
	CHECK(db.Action(1) == nullptr);
}

void TestRecordAnomalies()
{
	using fx::W;
	roth::Dbase100 db;

	g_test = "file size mismatch";
	{
		fx::Spec s = GoodSpec(); s.wrongFileSize = true;
		auto b = fx::Build(s);
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "header file size differs from the real file size"), 1);
		CHECK(db.Item(1) != nullptr);
	}

	g_test = "item offset out of file";
	{
		fx::Layout L;
		auto b = fx::Build(GoodSpec(), &L);
		fx::Put32(b, L.inventoryTable + 0, (uint32_t)b.size() - 8);
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "item record lies outside the file"), 1);
		CHECK(db.Item(1) == nullptr);
		CHECK(db.ItemSlot(1) != nullptr && !db.ItemSlot(1)->ok);
		CHECK(db.Item(3) != nullptr);       // the others survive
	}

	g_test = "block size not whole words";
	{
		fx::Layout L;
		auto b = fx::Build(GoodSpec(), &L);
		const uint32_t blk = L.itemOffsets[0] + 0x14;
		fx::Put32(b, blk, 6u | (4u << 24));
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "item trigger block size is not a whole number of words"), 1);
		CHECK(db.Item(1) == nullptr);
		CHECK(db.Item(3) != nullptr);
	}

	g_test = "block size below header";
	{
		fx::Layout L;
		auto b = fx::Build(GoodSpec(), &L);
		fx::Put32(b, L.itemOffsets[0] + 0x14, 4u << 24);
		// size 0 with trigger 4 -> end marker with a non-zero trigger byte
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "item end marker carries a non-zero trigger byte"), 1);
		CHECK(db.Item(1) != nullptr && db.Item(1)->blocks.empty());
	}

	g_test = "block runs past end";
	{
		fx::Layout L;
		auto b = fx::Build(GoodSpec(), &L);
		fx::Put32(b, L.itemOffsets[0] + 0x14, 0x00100000u | (4u << 24));
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "item trigger block runs past the end of the file"), 1);
		CHECK(db.Item(1) == nullptr);
	}

	g_test = "no end marker";
	{
		fx::Spec s;
		fx::ItemSpec it; it.renderId = 0x200;
		it.blocks.push_back({ 0x04, { W(5, 1) } });
		it.noTerminator = true;
		s.items.push_back(it);      // last thing in the file: the walk hits EOF
		auto b = fx::Build(s);
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "item trigger list reaches the end of the file with no end marker"), 1);
		CHECK(db.Item(1) == nullptr);
		CHECK(db.ItemSlot(1) && db.ItemSlot(1)->blocks.size() == 1);   // kept for the dump
	}

	g_test = "unknown trigger code";
	{
		fx::Spec s;
		fx::ItemSpec it; it.renderId = 0x201;
		it.blocks.push_back({ 0x09, { W(5, 1) } });
		s.items.push_back(it);
		auto b = fx::Build(s);
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "item trigger block with an unknown trigger code"), 1);
		CHECK(db.Item(1) != nullptr);       // still served: the walk is well-formed
		CHECK(!roth::IsKnownDbTrigger(9));
		CHECK(std::string(roth::DbTriggerName(9)) == "unknown");
		CHECK(std::string(roth::DbTriggerName(0x0b)) == "OnUse");
	}

	g_test = "item header truncated";
	{
		fx::Spec s;
		fx::ItemSpec it; it.renderId = 0x202; s.items.push_back(it);
		auto b = fx::Build(s);
		b.resize(b.size() - 8);     // cut into the 0x14-byte fixed part
		fx::Put32(b, 0x08, (uint32_t)b.size());
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "item record lies outside the file"), 1);
		CHECK(db.Item(1) == nullptr);
	}

	g_test = "item length mismatch is a note, not an anomaly";
	{
		fx::Spec s;
		fx::ItemSpec it; it.renderId = 0x203; it.lengthMode = 2; it.lengthOverride = 0x99;
		s.items.push_back(it);
		auto b = fx::Build(s);
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(db.AnomalyTotal(), 0);
		CHECK(HasNote(db, "0 equal the span through the end marker, 0 the span before it, 1 neither"));
	}

	g_test = "action anomalies";
	{
		fx::Spec s;
		fx::ActionSpec a1; a1.overrideLength = true; a1.length = 2;            // shorter than its length word
		fx::ActionSpec a2; a2.words = { W(1, 1), W(2, 2) }; a2.overrideLength = true; a2.length = 14; // not whole words
		fx::ActionSpec a3; a3.words = { W(1, 1) }; a3.overrideLength = true; a3.length = 0x4000;      // past EOF
		s.actions = { a1, a2, a3 };
		auto b = fx::Build(s);
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "action record shorter than its own length word"), 1);
		CHECK_EQ(Anom(db, "action record length is not a whole number of words"), 1);
		CHECK_EQ(Anom(db, "action record runs past the end of the file"), 1);
		CHECK(db.Action(1) == nullptr);
		// 14/4 - 1 = 2: ROTH.C's integer arithmetic, served with the anomaly counted.
		CHECK(db.Action(2) != nullptr && db.Action(2)->commandCount == 2);
		CHECK(db.Action(3) == nullptr);
		CHECK(HasNote(db, "actions: 3 entries, 0 empty, 2 malformed"));
	}

	g_test = "action offset out of file";
	{
		fx::Layout L;
		auto b = fx::Build(GoodSpec(), &L);
		fx::Put32(b, L.actionTable + 8, (uint32_t)b.size() - 2);
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "action record lies outside the file"), 1);
		CHECK(db.Action(3) == nullptr);
		CHECK(db.Action(1) != nullptr);
	}

	g_test = "cutscene name";
	{
		fx::Spec s;
		fx::CutsceneSpec c; c.name = std::string("AB\x01", 3); s.cutscenes.push_back(c);
		auto b = fx::Build(s);
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(Anom(db, "cutscene name holds a non-printable byte"), 1);
	}

	g_test = "shared offsets and duplicate render ids";
	{
		fx::Layout L;
		fx::Spec s = GoodSpec();
		s.items[2].renderId = 0x294;        // same as item 1
		auto b = fx::Build(s, &L);
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(db.DuplicateRenderIds(), 2);
		CHECK_EQ(db.FindItemByRenderId(0x294), 1);   // ascending-first
		fx::Put32(b, L.inventoryTable + 4, L.itemOffsets[0]);
		CHECK(db.Load(b.data(), b.size()));
		CHECK(HasNote(db, "2 sharing a record offset"));
		CHECK(db.Item(2) != nullptr && db.Item(2)->offset == db.Item(1)->offset);
	}

	g_test = "empty database";
	{
		fx::Spec s;
		auto b = fx::Build(s);
		CHECK(db.Load(b.data(), b.size()));
		CHECK_EQ(db.ItemCount(), 0);
		CHECK_EQ(db.AnomalyTotal(), 0);
		CHECK(db.Item(1) == nullptr);
		CHECK_EQ(db.FindItemByRenderId(0), 0);
	}
}

// Every truncation of a good file: no crash, and the damage is always visible.
void TestTruncationSweep()
{
	g_test = "truncation sweep";
	auto good = fx::Build(GoodSpec());
	int silent = 0;
	for (size_t n = 0; n < good.size(); n++)
	{
		roth::Dbase100 db;
		const bool ok = db.Load(good.data(), n);
		if (ok && db.AnomalyTotal() == 0) silent++;
	}
	CHECK_EQ(silent, 0);
}

// Random corruption: the sanitizer build is what makes this meaningful.
void TestFuzz()
{
	g_test = "fuzz";
	auto good = fx::Build(GoodSpec());
	std::mt19937 rng(12345);
	int loaded = 0;
	for (int iter = 0; iter < 20000; iter++)
	{
		auto b = good;
		const int flips = 1 + (int)(rng() % 8);
		for (int k = 0; k < flips; k++)
			b[rng() % b.size()] = (uint8_t)rng();
		if (rng() % 4 == 0) b.resize(rng() % (b.size() + 1));
		roth::Dbase100 db;
		if (db.Load(b.data(), b.size()))
		{
			loaded++;
			// Walk everything the accessors serve.
			for (int id = 1; id <= db.ItemCount(); id++)
				if (const roth::DbItem *it = db.Item(id))
					for (const auto &blk : it->blocks) (void)db.Commands(blk);
			for (int id = 1; id <= db.ActionCount(); id++)
				if (const roth::DbAction *a = db.Action(id)) (void)db.Commands(*a);
			for (int i = 0; i < db.CutsceneCount(); i++) (void)db.CutsceneGallery(i);
			for (int id = 1; id <= db.ItemCount(); id++) (void)db.ItemRuntime(id);
		}
	}
	CHECK(loaded > 0);
	printf("  fuzz: 20000 corrupted images, %d still loaded\n", loaded);
}

// The install adapter: a fake DATA directory with ROTH.RES and a lower-case
// dbase100.dat, as a CD copy on a case-sensitive file system might have.
void TestInstallAdapter(const char *tmpRoot)
{
	g_test = "install adapter";
	std::string dir = std::string(tmpRoot) + "/dbase100_test_install";
	MKDIR(dir.c_str());
	{
		FILE *f = fopen((dir + "/ROTH.RES").c_str(), "wb");
		CHECK(f != nullptr);
		if (!f) return;
		fputs("das2=m\\ademo.das\nmaps\n{\nm\\study1 m\\demo\n}\n", f);
		fclose(f);
	}
	remove((dir + "/dbase100.dat").c_str());

	roth::Install install;
	CHECK(install.Open(dir.c_str()));
	roth::Dbase100 db;
	roth::Log log;
	log.Begin("dbase100_test", (dir + "/").c_str());
	CHECK(!roth::LoadDbase100FromInstall(install, db, &log));   // no file yet
	CHECK_EQ(log.Warnings(), 1);

	auto bytes = fx::Build(GoodSpec());
	{
		FILE *f = fopen((dir + "/dbase100.dat").c_str(), "wb");
		CHECK(f != nullptr);
		if (!f) return;
		fwrite(bytes.data(), 1, bytes.size(), f);
		fclose(f);
	}
	CHECK(!install.DataFile("DBASE100.DAT").empty());
	CHECK(roth::LoadDbase100FromInstall(install, db, &log));
	CHECK(db.Item(1) != nullptr);
	CHECK_EQ(log.Warnings(), 1);   // a clean file adds no warning
	log.End();

	// A corrupt file fails loudly.
	bytes[0] = 'Q';
	{
		FILE *f = fopen((dir + "/dbase100.dat").c_str(), "wb");
		if (f) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
	}
	roth::Log log2;
	CHECK(!roth::LoadDbase100FromInstall(install, db, &log2));
	CHECK_EQ(log2.Warnings(), 1);

	roth::Install closed;
	CHECK(!roth::LoadDbase100FromInstall(closed, db, nullptr));
	CHECK(closed.DataFile("DBASE100.DAT").empty());
}

} // namespace

int main(int argc, char **argv)
{
	// `dbase100_test --write-fixture FILE` writes the good synthetic database,
	// so the dump tool can be exercised without retail data.
	if (argc > 2 && strcmp(argv[1], "--write-fixture") == 0)
	{
		auto b = fx::Build(GoodSpec());
		FILE *f = fopen(argv[2], "wb");
		if (!f) return 1;
		fwrite(b.data(), 1, b.size(), f);
		fclose(f);
		return 0;
	}
	const char *tmp = argc > 1 ? argv[1] : "/tmp";
	TestCommandDecode();
	TestBitmapSize();
	TestGoodFile();
	TestStructuralFailures();
	TestRecordAnomalies();
	TestTruncationSweep();
	TestFuzz();
	TestInstallAdapter(tmp);
	printf("%d checks, %d failed\n", g_checks, g_failed);
	printf("%s\n", g_failed == 0 ? "ALL DBASE100 TESTS PASS" : "*** DBASE100 TESTS FAILED ***");
	return g_failed == 0 ? 0 : 1;
}
