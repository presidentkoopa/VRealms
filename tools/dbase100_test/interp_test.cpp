//
// Tests for the DBASE100 command interpreter
// (src/roth/roth_dbase100_interp.{h,cpp}).
//
// Part 1 builds chains byte by byte with fixture.h and checks each rule
// against what ROTH.C's roth_c/src/engine/dbase100.c does ("dbase100.c:LINE"
// below). Part 2 is a smoke test on the retail DBASE100.DAT when it is
// present: every action and every item trigger block is run under a recording
// sink, and the calls are counted.
//
//   interp_test                 synthetic tests, then retail at the default path
//   interp_test <DBASE100.DAT>  retail file given explicitly
//
// The sanitizer build is what proves "no out-of-bounds read"; the checks on
// OutOfImageReads() prove the interpreter never asked for one.
//

#include "fixture.h"
#include "../../src/roth/roth_dbase100.h"
#include "../../src/roth/roth_dbase100_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace
{

int g_checks = 0, g_failed = 0;
const char *g_test = "";

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
	printf("  FAIL [%s] %s:%d: %s\n", g_test, __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long _a = (long long)(a), _b = (long long)(b); \
	if (_a != _b) { g_failed++; printf("  FAIL [%s] %s:%d: %s == %s (%lld vs %lld)\n", \
	g_test, __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; std::string _a = (a), _b = (b); \
	if (_a != _b) { g_failed++; printf("  FAIL [%s] %s:%d: %s\n      got  \"%s\"\n      want \"%s\"\n", \
	g_test, __FILE__, __LINE__, #a, _a.c_str(), _b.c_str()); } } while (0)

using fx::W;
typedef std::vector<uint32_t> Words;
const uint32_t NEG1 = 0xffffffffu;

std::string F(const char *fmt, long long a = 0, long long b = 0, long long c = 0)
{
	char buf[160];
	snprintf(buf, sizeof(buf), fmt, a, b, c);
	return buf;
}

//==========================================================================
//
// A sink that writes every call into a log and answers from settable values.
//
//==========================================================================

class RecSink : public roth::Dbase100Sink
{
public:
	std::vector<std::string> log;
	std::map<std::string, int> counts;

	std::map<int32_t, uint32_t> inventory;          // id -> count
	std::map<uint32_t, std::string> text;           // DBASE400 offset -> bytes
	bool anyText = false;                           // every offset has a line
	uint32_t openWindow = 1, openWindowAlt = 1;
	uint32_t loadChunk = 1;
	int32_t audioSeqState = 0;
	uint32_t chunkId = 0;
	uint8_t volume = 0;
	int32_t overlay = 0, panelOpen = 0, popup = 0;
	uint32_t healthMax = 0;
	std::deque<int32_t> rng;                        // injected RNG results; empty -> 0
	bool keepLog = true;
	std::string lastWindowText;
	roth::Dbase100Interp *forceEndCallsBack = nullptr;

	void Note(const char *name, const std::string &line)
	{
		counts[name]++;
		if (keepLog) log.push_back(line);
	}
	std::string Joined() const
	{
		std::string s;
		for (const auto &l : log) { if (!s.empty()) s += ' '; s += l; }
		return s;
	}
	int Count(const char *name) const
	{
		auto it = counts.find(name);
		return it == counts.end() ? 0 : it->second;
	}
	void Clear() { log.clear(); counts.clear(); }

	uint32_t GiveItem(int32_t id, uint32_t ctx) override { Note("GiveItem", F("Give(%lld,%lld)", id, ctx)); return 0; }
	uint32_t RemoveItem(int32_t id) override { Note("RemoveItem", F("Remove(%lld)", id)); return 0; }
	uint32_t QueryPlayerInventory(int32_t id, uint32_t flags) override
	{
		Note("QueryPlayerInventory", F("Query(%lld,%lld)", id, flags));
		auto it = inventory.find(id);
		return it == inventory.end() ? 0 : it->second;
	}
	uint32_t ReadNextDialogueLine(uint8_t *dest, uint32_t maxlen, uint32_t off, uint32_t flag) override
	{
		Note("ReadNextDialogueLine", F("Read(0x%llx,%lld,%lld)", off, maxlen, flag));
		std::string s;
		auto it = text.find(off);
		if (it != text.end()) s = it->second;
		else if (anyText) s = std::string("line", 5);   // "line\0"
		if (s.size() > maxlen) s.resize(maxlen);        // dbase100.c:390
		memcpy(dest, s.data(), s.size());
		return (uint32_t)s.size();
	}
	uint32_t Dbase100OpenDialogueWindow(const uint8_t *t, uint32_t bytes) override
	{
		lastWindowText.assign((const char *)t, bytes);
		Note("Dbase100OpenDialogueWindow", F("OpenMenu(%lld)", bytes));
		return openWindow;
	}
	uint32_t Dbase100OpenDialogueWindowAlt(const uint8_t *t, uint32_t bytes) override
	{
		lastWindowText.assign((const char *)t, bytes);
		Note("Dbase100OpenDialogueWindowAlt", F("OpenLine(%lld)", bytes));
		return openWindowAlt;
	}
	void DialogueVoiceForceEnd(uint32_t a, uint32_t d) override
	{
		Note("DialogueVoiceForceEnd", F("ForceEnd(%lld,%lld)", a, d));
		if (forceEndCallsBack) forceEndCallsBack->CloseDialogueAndRunBranch();
	}
	uint32_t PlayRecordGdvCutscene(int index) override { Note("PlayRecordGdvCutscene", F("PlayGdv(%lld)", index)); return 1; }
	uint32_t ShowFullscreenImage(uint32_t op) override { Note("ShowFullscreenImage", F("Image(0x%llx)", op)); return 1; }
	void ExitCutsceneOverlayMode() override { Note("ExitCutsceneOverlayMode", "ExitOverlay"); overlay = 0; }
	uint32_t PlaySoundEffect(uint32_t id, uint32_t param) override { Note("PlaySoundEffect", F("Sfx(%lld,0x%llx)", id, param)); return 0; }
	uint32_t LoadDbase300Chunk(uint32_t index) override { Note("LoadDbase300Chunk", F("LoadChunk(0x%llx)", index)); return loadChunk; }
	uint32_t FinalizeAudioSequenceRef() override { Note("FinalizeAudioSequenceRef", "FinalizeRef"); return 0; }
	uint32_t EmitMusicSequenceEvent(uint32_t e) override { Note("EmitMusicSequenceEvent", F("Emit(%lld)", e)); return 0; }
	void ProcessAudioSequenceChunk() override { Note("ProcessAudioSequenceChunk", "Process"); }
	int32_t ResetCommandChainNoSource(uint16_t i) override { Note("ResetCommandChainNoSource", F("RawIndex(%lld)", i)); return -1; }
	int32_t RunObjectCommandsById(uint16_t i) override { Note("RunObjectCommandsById", F("RawId(%lld)", i)); return -1; }
	void AdvanceClamp8a0f0(uint32_t amount, uint32_t max) override { Note("AdvanceClamp8a0f0", F("Heal(%lld,%lld)", amount, max)); }
	int32_t RngNextIndexForCount(int32_t count) override
	{
		int32_t r = 0;
		if (!rng.empty()) { r = rng.front(); rng.pop_front(); }
		Note("RngNextIndexForCount", F("Rng(%lld)", count));
		return r;
	}

	int32_t AudioSeqState() override { return audioSeqState; }
	void SetAudioSeqState(int32_t v) override { Note("SetAudioSeqState", F("SeqState=%lld", v)); audioSeqState = v; }
	uint32_t CurrentDbase300ChunkId() override { return chunkId; }
	uint8_t MusicMasterVolume() override { return volume; }
	int32_t CutsceneOverlayActive() override { return overlay; }
	int32_t InventoryPanelOpen() override { return panelOpen; }
	int32_t InspectPopupActive() override { return popup; }
	void ArmInventoryPanelExit() override { Note("ArmInventoryPanelExit", "ArmPanelExit"); }
	void SetLatch83b20() override { Note("SetLatch83b20", "Latch83b20"); }
	void SetLatch80b2c() override { Note("SetLatch80b2c", "Latch80b2c"); }
	void ZeroHealthAndFlagDeath() override { Note("ZeroHealthAndFlagDeath", "Kill"); }
	uint32_t HealthClampMax() override { return healthMax; }
};

//==========================================================================
//
// A database made of the given action records (ids 1..n, laid out one after
// another in the image, so "the word after a record" is the next record's
// header), two cutscenes and one item, with an interpreter attached.
//
//==========================================================================

struct World
{
	fx::Spec spec;
	fx::Layout L;
	roth::Dbase100 db;
	RecSink sink;
	roth::Dbase100Interp in;

	explicit World(const std::vector<Words> &actions)
	{
		spec.unk0C = 433;
		fx::CutsceneSpec c0; c0.name = "INTRO";
		fx::CutsceneSpec c1;                        // no name: op 0x07 ignores it
		fx::CutsceneSpec c2; c2.name = "MOVIE";
		spec.cutscenes = { c0, c1, c2 };
		for (const auto &w : actions)
		{
			fx::ActionSpec a;
			a.words = w;
			spec.actions.push_back(a);
		}
	}
	bool Go()
	{
		auto bytes = fx::Build(spec, &L);
		if (!db.Load(bytes.data(), bytes.size())) { printf("  fixture did not load: %s\n", db.Error().c_str()); return false; }
		in.Attach(&db, &sink);
		return true;
	}
	// The image offset of command `index` of action `id`.
	uint32_t Cmd(int id, int index = 0) const { return L.actionOffsets[(size_t)id - 1] + 4 + (uint32_t)index * 4; }
	uint32_t Run(int id) { return in.EvalDialogueRecordById((uint32_t)id); }
	bool Flag(uint32_t i) { return in.TestDbase100RecordFlag(i) != 0; }
	void Clean()
	{
		CHECK_EQ(in.AnomalyTotal(), 0);
		CHECK_EQ(in.OutOfImageReads(), 0);
		for (const auto &kv : in.Anomalies()) printf("    unexpected anomaly: %s x%d\n", kv.first.c_str(), kv.second);
	}
};

int Unhandled(const roth::Dbase100Interp &in, int op)
{
	auto it = in.Unhandled().find(op);
	return it == in.Unhandled().end() ? 0 : it->second;
}

int Anom(const roth::Dbase100Interp &in, const char *name)
{
	auto it = in.Anomalies().find(name);
	return it == in.Anomalies().end() ? 0 : it->second;
}

//==========================================================================
//
// The flag bitmap
//
//==========================================================================

void TestBitmap()
{
	g_test = "flag bitmap";
	roth::Dbase100Interp bare;
	// No bitmap: ROTH.C's pointer-0 case (raw_commands.c:82, renderer.c:11531).
	CHECK_EQ(bare.TestDbase100RecordFlag(3), -1);
	CHECK_EQ(bare.Dbase100BitmapTestSet(3), NEG1);
	CHECK_EQ(bare.Dbase100BitmapTestClear(3), NEG1);

	World w({ { W(0x0a, 1) } });
	if (!w.Go()) { CHECK(false); return; }
	// 433 records -> ((433 + 0x20) & ~0x1f) >> 3 = 56 bytes (game_core.c:420-424).
	CHECK_EQ(w.in.State().g_dbase100_record_bitmap.size(), 56);
	for (uint8_t b : w.in.State().g_dbase100_record_bitmap) CHECK_EQ(b, 0);

	CHECK_EQ(w.in.TestDbase100RecordFlag(10), 0);
	CHECK_EQ(w.in.Dbase100BitmapTestSet(10), NEG1);     // was clear: set, -1
	CHECK_EQ(w.in.Dbase100BitmapTestSet(10), 0);        // already set: 0
	CHECK_EQ(w.in.TestDbase100RecordFlag(10), -1);
	// byte = idx >> 3, mask = 1 << (idx & 7)
	CHECK_EQ(w.in.State().g_dbase100_record_bitmap[1], 0x04);
	CHECK_EQ(w.in.Dbase100BitmapTestClear(10), NEG1);   // was set: clear, -1
	CHECK_EQ(w.in.Dbase100BitmapTestClear(10), 0);      // already clear: 0
	CHECK_EQ(w.in.State().g_dbase100_record_bitmap[1], 0);
	w.in.Dbase100BitmapTestSet(447);
	CHECK_EQ(w.in.State().g_dbase100_record_bitmap[55], 0x80);
	w.Clean();

	// Past the bitmap: counted, never stored.
	CHECK_EQ(w.in.TestDbase100RecordFlag(448), 0);
	CHECK_EQ(w.in.Dbase100BitmapTestSet(448), 0);
	CHECK_EQ(w.in.Dbase100BitmapTestClear(0xffffff), 0);
	CHECK_EQ(Anom(w.in, "flag index outside the bitmap"), 3);

	// A different count sizes a different bitmap.
	World w2({ { W(0x0a, 1) } });
	w2.spec.unk0C = 31;
	if (!w2.Go()) { CHECK(false); return; }
	CHECK_EQ(w2.in.State().g_dbase100_record_bitmap.size(), 4);
	World w3({ { W(0x0a, 1) } });
	w3.spec.unk0C = 32;
	if (!w3.Go()) { CHECK(false); return; }
	CHECK_EQ(w3.in.State().g_dbase100_record_bitmap.size(), 8);
}

//==========================================================================
//
// The loop: operand 0, the opcode bound, the count, the return value
//
//==========================================================================

void TestLoopBasics()
{
	g_test = "loop basics";
	{
		// op 0x04 set and clear; op 0x10 counts; the accumulator is returned.
		World w({ { W(0x04, 7), W(0x04, 9), W(0x04, 7, true), W(0x10, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 4);
		CHECK(!w.Flag(7));
		CHECK(w.Flag(9));
		w.Clean();
	}
	{
		// Operand 0: skipped whatever the opcode (dbase100.c:633).
		World w({ { W(0x04, 0), W(0x0a, 0), W(0x11, 0), W(0x7f, 0), W(0x04, 3) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 1);
		CHECK_EQ(w.in.OperandZeroSkips(), 4);
		CHECK(!w.Flag(0));
		CHECK(w.Flag(3));
		CHECK(w.sink.log.empty());
		CHECK(w.in.Unhandled().empty());
		w.Clean();
	}
	{
		// The bound: opcodes 1..0x37 dispatch, 0 and 0x38..0x7f do not
		// (dbase100.c:641). In-range opcodes with no handler are skipped too
		// (:651). All are counted.
		World w({ { 0x00000005u, W(0x38, 5), W(0x7f, 5), W(0x09, 0xffffff), W(0x0c, 0xffffff),
			W(0x0f, 5), W(0x34, 5), W(0x06, 5), W(0x37, 5), W(0x04, 2) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 1);
		CHECK_EQ(Unhandled(w.in, 0x00), 1);
		CHECK_EQ(Unhandled(w.in, 0x38), 1);
		CHECK_EQ(Unhandled(w.in, 0x7f), 1);
		CHECK_EQ(Unhandled(w.in, 0x09), 1);
		CHECK_EQ(Unhandled(w.in, 0x0c), 1);
		CHECK_EQ(Unhandled(w.in, 0x0f), 1);
		CHECK_EQ(Unhandled(w.in, 0x34), 1);
		CHECK_EQ(Unhandled(w.in, 0x06), 1);
		CHECK_EQ(Unhandled(w.in, 0x37), 0);                 // the last handled opcode
		CHECK_EQ(w.in.State().g_value_reduction_factor, 5);
		CHECK(w.Flag(2));
		CHECK(roth::Dbase100OpcodeName(0x37) != nullptr);
		CHECK(roth::Dbase100OpcodeName(0x38) == nullptr);
		CHECK(roth::Dbase100OpcodeName(0x09) == nullptr);
		CHECK(roth::IsDbase100MarkerOpcode(0x09));
		CHECK(roth::IsDbase100MarkerOpcode(0x0c));
		CHECK(!roth::IsDbase100MarkerOpcode(0x0f));
		w.Clean();
	}
	{
		// The loop is do-while: a count of 1, 0 or -1 all run exactly one
		// record (dbase100.c:625-637).
		for (int count = -1; count <= 2; count++)
		{
			World w({ { W(0x04, 1), W(0x04, 2), W(0x04, 3) } });
			if (!w.Go()) { CHECK(false); return; }
			const uint32_t r = w.in.ExecuteDbase100Chain(w.Cmd(1), count, 0);
			const int ran = count <= 1 ? 1 : count;
			CHECK_EQ(r, ran);
			CHECK_EQ(w.Flag(1), true);
			CHECK_EQ(w.Flag(2), ran >= 2);
			CHECK_EQ(w.Flag(3), false);
			w.Clean();
		}
	}
	{
		// A record whose length word is 0: count = 0/4 - 1 = -1
		// (dbase100.c:433-436), and the word after the header still runs.
		World w({ { W(0x04, 5), W(0x04, 6) }, { W(0x04, 8) } });
		w.spec.actions[0].overrideLength = true;
		w.spec.actions[0].length = 0;
		if (!w.Go()) { CHECK(false); return; }
		CHECK(w.db.Action(1) != nullptr && w.db.Action(1)->bodiless);
		CHECK_EQ(w.Run(1), 1);
		CHECK(w.Flag(5));
		CHECK(!w.Flag(6));
		w.Clean();
	}
	{
		// A length of 4 (header only): count 0. The word that runs is the
		// NEXT record's header: here 0x0400 over a length of 8, which decodes
		// as op 0x04 operand 8.
		World w({ {}, { W(0x04, 30) } });
		w.spec.actions[1].unk02 = 0x0400;
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 1);
		CHECK(w.Flag(8));
		CHECK(!w.Flag(30));
		w.Clean();
	}
	{
		// Reads outside the image give 0 and are counted, not performed.
		World w({ { W(0x04, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.in.ExecuteDbase100Chain((uint32_t)w.db.Size() - 2, 3, 0), 0);
		CHECK_EQ(w.in.OutOfImageReads(), 3);
		CHECK_EQ(w.in.OperandZeroSkips(), 3);
		CHECK_EQ(w.in.ExecuteDbase100Chain(0xfffffffcu, 1, 0), 0);
		CHECK_EQ(w.in.OutOfImageReads(), 4);
	}
	{
		// Not attached: nothing runs.
		roth::Dbase100Interp bare;
		CHECK_EQ(bare.EvalDialogueRecordById(1), 0);
		CHECK_EQ(bare.ExecuteDbase100Chain(0x40, 1, 0), 0);
		CHECK_EQ(Anom(bare, "interpreter run with no database attached"), 1);
	}
}

//==========================================================================
//
// Gates: 0x01, 0x02, 0x03, 0x1c, 0x0d
//
//==========================================================================

void TestGates()
{
	g_test = "op 0x01";
	{
		// If: flag set -> go on. Flag clear -> end with 0, not the
		// accumulator (dbase100.c:803-808).
		World w({ { W(0x04, 1), W(0x01, 1), W(0x04, 2), W(0x01, 50), W(0x04, 3) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 0);
		CHECK(w.Flag(2));
		CHECK(!w.Flag(3));
		w.Clean();
	}
	{
		// If-NOT: flag clear -> go on; set -> end with 0.
		World w({ { W(0x01, 50, true), W(0x04, 2), W(0x04, 50), W(0x01, 50, true), W(0x04, 3) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 0);
		CHECK(w.Flag(2));
		CHECK(!w.Flag(3));
		w.Clean();
	}
	{
		// A gate that holds to the end returns the accumulator.
		World w({ { W(0x04, 1), W(0x01, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 1);
	}

	g_test = "op 0x02";
	{
		// Item = low 16 bits (signed), threshold = operand >> 16. If: go on
		// when count > threshold (dbase100.c:811-818).
		World w({ { W(0x02, 0x020007), W(0x04, 1) }, { W(0x02, 0x020007, true), W(0x04, 2) },
			{ W(0x02, 0x00ffff), W(0x04, 3) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.inventory[7] = 2;
		CHECK_EQ(w.Run(1), 0);              // 2 > 2 fails: end
		CHECK(!w.Flag(1));
		CHECK_EQ(w.Run(2), 1);              // If-NOT: 2 <= 2 goes on
		CHECK(w.Flag(2));
		w.sink.inventory[7] = 3;
		CHECK_EQ(w.Run(1), 1);
		CHECK(w.Flag(1));
		w.in.Dbase100BitmapTestClear(2);
		CHECK_EQ(w.Run(2), 0);              // If-NOT: 3 <= 2 fails
		CHECK(!w.Flag(2));
		// 0xffff is item -1 after the sign extension (dbase100.c:813).
		w.sink.Clear();
		w.sink.inventory[-1] = 1;
		CHECK_EQ(w.Run(3), 1);
		CHECK_STR(w.sink.Joined(), "Query(-1,0)");
		w.Clean();
	}

	g_test = "op 0x03";
	{
		// If: EQUAL ends the chain with 0; unequal goes on. If-NOT: equal
		// goes on; unequal ends (dbase100.c:821-832).
		World w({ { W(0x04, 9), W(0x03, 0x010007), W(0x04, 1) },
			{ W(0x04, 9), W(0x03, 0x010007, true), W(0x04, 2) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.inventory[7] = 1;
		CHECK_EQ(w.Run(1), 0);
		CHECK(!w.Flag(1));
		CHECK_EQ(w.Run(2), 2);
		CHECK(w.Flag(2));
		w.sink.inventory[7] = 5;
		w.in.Dbase100BitmapTestClear(2);
		CHECK_EQ(w.Run(1), 2);
		CHECK(w.Flag(1));
		CHECK_EQ(w.Run(2), 0);
		CHECK(!w.Flag(2));
		// These query even in scan mode: no scan check on 0x02 / 0x03.
		w.sink.Clear();
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1, 1), 1, 2), 0);
		CHECK_EQ(w.sink.Count("QueryPlayerInventory"), 1);
		w.Clean();
	}

	g_test = "op 0x1c";
	{
		// If: go on when caller flag bit 0 is CLEAR, end with 0 when set.
		// If-NOT the reverse. Scan mode steps over it (dbase100.c:867-871).
		World w({ { W(0x04, 9), W(0x1c, 1), W(0x04, 1) }, { W(0x04, 9), W(0x1c, 1, true), W(0x04, 2) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 3, 0), 2);
		CHECK(w.Flag(1));
		w.in.Dbase100BitmapTestClear(1);
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 3, 1), 0);
		CHECK(!w.Flag(1));
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(2), 3, 0), 0);
		CHECK(!w.Flag(2));
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(2), 3, 1), 2);
		CHECK(w.Flag(2));
		// Scan mode: the gate itself is skipped, in both polarities.
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1, 1), 1, 3), 0);
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(2, 1), 1, 2), 0);
		w.Clean();
	}

	g_test = "op 0x0d";
	{
		// If: flag set -> the next record runs; clear -> it is stepped over.
		// If-NOT the reverse. Nothing is counted for the gate itself
		// (dbase100.c:655-662).
		World w({ { W(0x0d, 40), W(0x04, 1), W(0x04, 2) }, { W(0x0d, 40, true), W(0x04, 3), W(0x04, 4) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 1);
		CHECK(!w.Flag(1));
		CHECK(w.Flag(2));
		CHECK_EQ(w.Run(2), 2);
		CHECK(w.Flag(3));
		CHECK(w.Flag(4));
		w.in.Dbase100BitmapTestSet(40);
		w.in.Dbase100BitmapTestClear(2);
		w.in.Dbase100BitmapTestClear(3);
		w.in.Dbase100BitmapTestClear(4);
		CHECK_EQ(w.Run(1), 2);
		CHECK(w.Flag(1));
		CHECK(w.Flag(2));
		CHECK_EQ(w.Run(2), 1);
		CHECK(!w.Flag(3));
		CHECK(w.Flag(4));
		w.Clean();
	}
	{
		// Stepping over the next record also takes it off the count: a gate
		// that fails on the last-but-one record ends the chain.
		World w({ { W(0x0d, 40), W(0x04, 1) }, { W(0x04, 2) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 0);
		CHECK(!w.Flag(1));
		CHECK(!w.Flag(2));
		w.Clean();
	}
	{
		// The look-ahead reads the next word's top BYTE and wants exactly 8
		// (dbase100.c:656). 0x88 (op 8 with If-NOT) is not 8: the gate is an
		// ordinary one and the 0x88 record is what it steps over.
		World w({ { W(0x0d, 40), W(0x08, 0x100, true), W(0x04, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.anyText = true;
		CHECK_EQ(w.Run(1), 1);
		CHECK(w.Flag(1));
		CHECK_EQ(w.sink.Count("ReadNextDialogueLine"), 0);
		w.Clean();
	}
	{
		// The look-ahead on a record's LAST command reads the next record's
		// header. Its top byte is the high byte of the header's upper word:
		// with 8 there, the choice builder is entered -- it re-reads the gate,
		// runs out of records and returns 1 (dbase100.c:716). With anything
		// else the gate is ordinary and the chain ends with the accumulator.
		for (int pass = 0; pass < 2; pass++)
		{
			World w({ { W(0x0d, 40) }, { W(0x04, 2) } });
			w.spec.actions[1].unk02 = pass == 0 ? 0x0800 : 0x0300;
			if (!w.Go()) { CHECK(false); return; }
			CHECK_EQ(w.Run(1), pass == 0 ? 1 : 0);
			CHECK(!w.Flag(2));
			CHECK_EQ(w.in.State().g_dbase100_choice_cursor, 0);
			w.Clean();
		}
	}
}

//==========================================================================
//
// Effects: 0x04 scan, 0x11, 0x19, 0x23, 0x36, 0x26, 0x2d, 0x37, 0x0a
//
//==========================================================================

void TestEffects()
{
	g_test = "op 0x11";
	{
		World w({ { W(0x11, 68), W(0x11, 5, true), W(0x11, 0xfffe), W(0x11, 0x12ffff, true) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 4);
		// The id is the low 16 bits sign-extended; give's ctx is 0
		// (dbase100.c:770-771).
		CHECK_STR(w.sink.Joined(), "Give(68,0) Remove(5) Give(-2,0) Remove(-1)");
		w.Clean();
	}

	g_test = "op 0x19";
	{
		World w({ { W(0x19, 281), W(0x19, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 2);
		CHECK_STR(w.sink.Joined(), "Sfx(280,0xfcfe) Sfx(0,0xfcfe)");     // dbase100.c:849
		w.Clean();
	}

	g_test = "op 0x23 / 0x36";
	{
		// The LOW 16 bits must be non-zero; neither is counted
		// (dbase100.c:835-844).
		World w({ { W(0x23, 12), W(0x23, 0x010000), W(0x36, 0x02abcd), W(0x36, 0x030000) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 0);
		CHECK_STR(w.sink.Joined(), "RawIndex(12) RawId(43981)");
		w.Clean();
	}

	g_test = "op 0x26";
	{
		// Signed low 16 bits: positive heals up to the cap at 0x7fe44; zero
		// or negative kills. Not counted (dbase100.c:901-907).
		World w({ { W(0x26, 25), W(0x26, 0x010000), W(0x26, 0xffff), W(0x26, 0x7fff) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.healthMax = 0x800;
		CHECK_EQ(w.Run(1), 0);
		CHECK_STR(w.sink.Joined(), "Heal(25,2048) Kill Kill Heal(32767,2048)");
		w.Clean();
	}

	g_test = "op 0x2d";
	{
		// Low byte: below 4 nothing, 4 sets 0x83b20, 7 sets 0x80b2c only
		// while the inventory panel is open, anything else nothing
		// (dbase100.c:888-898).
		World w({ { W(0x2d, 1), W(0x2d, 3), W(0x2d, 4), W(0x2d, 5), W(0x2d, 7), W(0x2d, 0x104), W(0x2d, 8) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 0);
		CHECK_STR(w.sink.Joined(), "Latch83b20 Latch83b20");
		w.sink.Clear();
		w.sink.panelOpen = 1;
		CHECK_EQ(w.Run(1), 0);
		CHECK_STR(w.sink.Joined(), "Latch83b20 Latch80b2c Latch83b20");
		w.Clean();
	}

	g_test = "op 0x37";
	{
		// Signed 16-bit add, not counted (dbase100.c:782-784).
		World w({ { W(0x37, 100), W(0x37, 0xfff6), W(0x37, 0x010003) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 0);
		CHECK_EQ(w.in.State().g_value_reduction_factor, 93);
		w.Clean();
	}

	g_test = "op 0x0a";
	{
		// Ends the chain with 1, whatever the accumulator (dbase100.c:740-742).
		World w({ { W(0x04, 1), W(0x04, 2), W(0x0a, 0xffffff), W(0x04, 3) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 1);
		CHECK(w.Flag(2));
		CHECK(!w.Flag(3));
		w.Clean();
	}
}

//==========================================================================
//
// Scan mode (flags bit 1), and which returns run finalize
//
//==========================================================================

void TestScanAndFinalize()
{
	g_test = "scan mode";
	{
		// Each of these returns 1 at once in scan mode and does nothing
		// (dbase100.c:666, 762, 769, 854, 911, 923).
		const uint32_t effectful[] = { W(0x04, 5), W(0x04, 5, true), W(0x11, 5), W(0x11, 5, true),
			W(0x05, 0x100), W(0x07, 1), W(0x08, 0x100), W(0x0e, 5) };
		for (uint32_t word : effectful)
		{
			World w({ { W(0x10, 1), W(0x10, 1), word, W(0x04, 9) } });
			if (!w.Go()) { CHECK(false); return; }
			w.sink.anyText = true;
			w.in.State().g_dbase100_pending_topic = 77;
			CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 4, 2), 1);
			CHECK(!w.Flag(5));
			CHECK(!w.Flag(9));
			CHECK(w.sink.log.empty());
			// The scan return does NOT run finalize: the pending topic stays.
			CHECK_EQ(w.in.State().g_dbase100_pending_topic, 77);
			CHECK_EQ(w.in.State().g_dialogue_action_queue_count, 0);
			w.Clean();
		}
	}
	{
		// An 0x0d that hands over to the choice builder scans as a choice.
		World w({ { W(0x0d, 40), W(0x08, 0x100), W(0x09, 0xffffff), W(0x0a, 0xffffff) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 4, 2), 1);
		CHECK(w.sink.log.empty());
	}
	{
		// These are stepped over in scan mode, uncounted: 0x1a, 0x1b, 0x1c.
		// A chain of only those scans to 0.
		World w({ { W(0x1a, 9), W(0x1b, 1), W(0x1c, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.chunkId = 5;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 3, 2), 0);
		CHECK(w.sink.log.empty());
		CHECK_EQ(w.in.State().g_dbase100_pending_topic, 0);
		w.Clean();
	}
	{
		// And these ACT in scan mode, as ROTH.C has them: 0x19 (counted),
		// 0x23, 0x36, 0x26, 0x2d, 0x37 (not counted), 0x10 (counted).
		World w({ { W(0x19, 3), W(0x23, 4), W(0x36, 5), W(0x26, 6), W(0x2d, 4), W(0x37, 7), W(0x10, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 7, 2), 2);
		CHECK_STR(w.sink.Joined(), "Sfx(2,0xfcfe) RawIndex(4) RawId(5) Heal(6,0) Latch83b20");
		CHECK_EQ(w.in.State().g_value_reduction_factor, 7);
		w.Clean();
	}
	{
		// A gate fails first: scan returns 0 -- "this topic is not available".
		World w({ { W(0x01, 40), W(0x05, 0x100) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 2, 2), 0);
		w.in.Dbase100BitmapTestSet(40);
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 2, 2), 1);
	}

	g_test = "finalize";
	{
		// finalize_dbase100_chain (dbase100.c:305-320), reached by the normal
		// end, op 0x0a and the failed gates. A fresh load with the music
		// faded out (state 2): state 0, finalize ref, emit(volume), process.
		const Words chains[] = { { W(0x10, 1) }, { W(0x0a, 1) }, { W(0x01, 40) } };
		const uint32_t want[] = { 1, 1, 0 };
		for (int i = 0; i < 3; i++)
		{
			World w({ chains[i] });
			if (!w.Go()) { CHECK(false); return; }
			w.sink.loadChunk = 0;
			w.sink.audioSeqState = 2;
			w.sink.volume = 99;
			w.in.State().g_dbase100_pending_topic = 77;
			CHECK_EQ(w.Run(1), want[i]);
			CHECK_STR(w.sink.Joined(), "LoadChunk(0x4d) SeqState=0 FinalizeRef Emit(99) Process");
			CHECK_EQ(w.in.State().g_dbase100_pending_topic, 0);
			w.Clean();
		}
	}
	{
		// Fresh load, music not faded: process only. Not fresh: nothing but
		// the load. The topic is cleared either way. No topic: no calls.
		World w({ { W(0x10, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.loadChunk = 0;
		w.in.State().g_dbase100_pending_topic = 3;
		CHECK_EQ(w.in.FinalizeDbase100Chain(1234), 1234);
		CHECK_STR(w.sink.Joined(), "LoadChunk(0x3) Process");
		w.sink.Clear();
		w.sink.loadChunk = 1;
		w.sink.audioSeqState = 2;
		w.in.State().g_dbase100_pending_topic = 3;
		CHECK_EQ(w.in.FinalizeDbase100Chain(5), 5);
		CHECK_STR(w.sink.Joined(), "LoadChunk(0x3)");
		CHECK_EQ(w.in.State().g_dbase100_pending_topic, 0);
		CHECK_EQ(w.sink.audioSeqState, 2);
		w.sink.Clear();
		CHECK_EQ(w.in.FinalizeDbase100Chain(6), 6);
		CHECK(w.sink.log.empty());
	}
}

//==========================================================================
//
// Music: 0x1a, 0x1b
//
//==========================================================================

void TestMusic()
{
	g_test = "op 0x1a";
	{
		// Faded out: finalize ref then state 0. Then the load; a fresh one
		// is processed. Counted either way (dbase100.c:794-800).
		World w({ { W(0x1a, 0x294411) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.audioSeqState = 2;
		w.sink.loadChunk = 0;
		CHECK_EQ(w.Run(1), 1);
		CHECK_STR(w.sink.Joined(), "FinalizeRef SeqState=0 LoadChunk(0x294411) Process");
		w.sink.Clear();
		w.sink.loadChunk = 1;
		CHECK_EQ(w.Run(1), 1);
		CHECK_STR(w.sink.Joined(), "LoadChunk(0x294411)");
		w.Clean();
	}
	{
		// Busy: REQUEUE-B. Slot 0 gets the count and the record's own offset
		// but NOT the flags (dbase100.c:789-792); the count becomes 1; -1.
		World w({ { W(0x04, 1), W(0x1a, 9), W(0x04, 2) } });
		if (!w.Go()) { CHECK(false); return; }
		w.in.State().g_dialogue_busy_flag = 1;
		w.in.State().g_dialogue_action_queue[0].flags = 0x55;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 3, 1), NEG1);
		CHECK(w.Flag(1));
		CHECK(!w.Flag(2));
		CHECK_EQ(w.in.State().g_dialogue_action_queue_count, 1);
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].chain, w.Cmd(1, 1));
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].count, 2);
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].flags, 0x55);
		CHECK(w.sink.log.empty());
		// A count that is already non-zero is left alone (dbase100.c:951).
		w.in.State().g_dialogue_action_queue_count = 3;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 3, 1), NEG1);
		CHECK_EQ(w.in.State().g_dialogue_action_queue_count, 3);
		w.Clean();
	}

	g_test = "op 0x1b";
	{
		// Stages the current DBASE300 chunk id as the pending topic and is
		// counted (dbase100.c:775-779); finalize then commits it at the end
		// of the same chain.
		World w({ { W(0x1b, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.chunkId = 0x1234;
		CHECK_EQ(w.Run(1), 1);
		CHECK_STR(w.sink.Joined(), "LoadChunk(0x1234)");
		CHECK_EQ(w.in.State().g_dbase100_pending_topic, 0);
		w.Clean();
	}
}

//==========================================================================
//
// Jump: 0x1d
//
//==========================================================================

void TestJump()
{
	g_test = "op 0x1d";
	{
		// Carries on in the target record; the accumulator is kept, the rest
		// of the jumping record is abandoned (dbase100.c:874-885).
		World w({ { W(0x04, 1), W(0x1d, 3), W(0x04, 2) }, { W(0x04, 3) }, { W(0x04, 4), W(0x04, 5) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 3);
		CHECK(w.Flag(1));
		CHECK(!w.Flag(2));
		CHECK(!w.Flag(3));
		CHECK(w.Flag(4));
		CHECK(w.Flag(5));
		w.Clean();
	}
	{
		// id == count is valid (`operand > count` is the test, :877); past
		// it, or an empty table entry, the jump is a no-op and the chain
		// goes on.
		World w({ { W(0x1d, 4), W(0x1d, 2), W(0x04, 1), W(0x1d, 3), W(0x04, 2) }, {}, { W(0x04, 6) } });
		w.spec.actions[1].absent = true;
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 2);
		CHECK(w.Flag(1));
		CHECK(!w.Flag(2));
		CHECK(w.Flag(6));
		w.Clean();
	}
	{
		// A jump to a zero-length record: count = 0 - 1, the chain ends
		// there without reading on.
		World w({ { W(0x04, 1), W(0x1d, 2), W(0x04, 2) }, { W(0x04, 3) } });
		w.spec.actions[1].overrideLength = true;
		w.spec.actions[1].length = 0;
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 1);
		CHECK(!w.Flag(2));
		CHECK(!w.Flag(3));
		w.Clean();
	}
	{
		// A record that jumps to itself hangs ROTH.C. Here the step budget
		// stops it and says so.
		World w({ { W(0x1d, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 0);
		CHECK_EQ(Anom(w.in, "chain ran past the step budget"), 1);
	}
}

//==========================================================================
//
// Lines, movies, images: 0x05, 0x07, 0x0e, and REQUEUE-A
//
//==========================================================================

void TestSay()
{
	g_test = "op 0x05";
	{
		// Reads the line with voice (flag 1, room 0x3fc), opens the window,
		// goes busy; counted (dbase100.c:853-864).
		World w({ { W(0x05, 0xf38c), W(0x04, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.text[0xf38c] = std::string("Staff\0", 6);
		CHECK_EQ(w.Run(1), 2);
		CHECK_STR(w.sink.Joined(), "Read(0xf38c,1020,1) OpenLine(6)");
		CHECK_STR(w.sink.lastWindowText, std::string("Staff\0", 6));
		CHECK_EQ(w.in.State().g_dialogue_busy_flag, 1);
		CHECK(w.Flag(1));           // the chain itself goes on after opening
		w.Clean();
	}
	{
		// No text: no window, not busy, still counted. Window refused: same.
		World w({ { W(0x05, 0x100) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 1);
		CHECK_STR(w.sink.Joined(), "Read(0x100,1020,1)");
		CHECK_EQ(w.in.State().g_dialogue_busy_flag, 0);
		w.sink.Clear();
		w.sink.anyText = true;
		w.sink.openWindowAlt = 0;
		CHECK_EQ(w.Run(1), 1);
		CHECK_STR(w.sink.Joined(), "Read(0x100,1020,1) OpenLine(5)");
		CHECK_EQ(w.in.State().g_dialogue_busy_flag, 0);
		w.Clean();
	}
	{
		// A second line while the first is up: REQUEUE-A parks it in slot 0
		// with all three fields and returns -1 (dbase100.c:859, 946-952).
		// No finalize on the way out.
		World w({ { W(0x05, 0x100), W(0x05, 0x200), W(0x04, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.anyText = true;
		w.in.State().g_dbase100_pending_topic = 0;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 3, 1), NEG1);
		CHECK_STR(w.sink.Joined(), "Read(0x100,1020,1) OpenLine(5)");
		CHECK_EQ(w.in.State().g_dialogue_action_queue_count, 1);
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].chain, w.Cmd(1, 1));
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].count, 2);
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].flags, 1);
		CHECK(!w.Flag(1));

		// The pump: busy clears, advance runs the head. The second line
		// opens and the rest of the chain runs; the entry is popped and
		// finish_dialogue_record_eval runs (dbase100.c:484-497).
		w.sink.Clear();
		w.in.AdvanceDialogueActionQueue();              // still busy: nothing
		CHECK(w.sink.log.empty());
		w.in.State().g_dialogue_busy_flag = 0;
		w.in.State().g_dbase100_queue_active = 1;
		w.in.AdvanceDialogueActionQueue();
		CHECK_STR(w.sink.Joined(), "Read(0x200,1020,1) OpenLine(5)");
		CHECK(w.Flag(1));
		CHECK_EQ(w.in.State().g_dialogue_action_queue_count, 0);
		CHECK_EQ(w.in.State().g_dbase100_queue_active, 0);
		w.Clean();
	}
	{
		// finish_dialogue_record_eval before the read (dbase100.c:860): with
		// the overlay up, the queue empty and no choice parked it tears the
		// overlay down (dbase100.c:291-296).
		World w({ { W(0x05, 0x100) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.overlay = 1;
		w.in.State().g_dbase100_queue_active = 1;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 1, 0), 1);
		CHECK_STR(w.sink.Joined(), "ExitOverlay Read(0x100,1020,1)");
		CHECK_EQ(w.in.State().g_dbase100_queue_active, 0);
		// Not with a queued entry, and not with a choice parked.
		w.sink.Clear();
		w.sink.overlay = 1;
		w.in.State().g_dbase100_choice_cursor = w.Cmd(1);
		w.in.FinishDialogueRecordEval();
		CHECK(w.sink.log.empty());
		w.in.State().g_dbase100_choice_cursor = 0;
		w.in.State().g_dialogue_action_queue_count = 1;
		w.in.FinishDialogueRecordEval();
		CHECK(w.sink.log.empty());
		w.in.State().g_dialogue_action_queue_count = 0;
		w.in.FinishDialogueRecordEval();
		CHECK_STR(w.sink.Joined(), "ExitOverlay");
		w.Clean();
	}
	{
		// While run_dialogue_action_queue drains, a line is counted and
		// skipped without being read (dbase100.c:855).
		World w({ { W(0x05, 0x100), W(0x04, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.anyText = true;
		w.in.State().g_dbase100_choice_record_indices[roth::Dbase100InterpState::QUEUE_DRAINING] = 1;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 2, 0), 2);
		CHECK(w.sink.log.empty());
		CHECK(w.Flag(1));
		w.Clean();
	}

	g_test = "op 0x07";
	{
		// Plays cutscene operand - 1, then sets 0x83b20; counted
		// (dbase100.c:932-942).
		World w({ { W(0x07, 1), W(0x07, 3) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 2);
		CHECK_STR(w.sink.Joined(), "PlayGdv(0) Latch83b20 PlayGdv(2) Latch83b20");
		w.Clean();
	}
	{
		// Past the table, or an entry with no name: counted, nothing played,
		// no latch (dbase100.c:935, 938).
		World w({ { W(0x07, 4), W(0x07, 2), W(0x07, 0xffffff) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 3);
		CHECK(w.sink.log.empty());
		w.Clean();
	}
	{
		// Header +0x30 is not 2: nothing plays (dbase100.c:936).
		World w({ { W(0x07, 1) } });
		w.spec.field30 = 1;
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 1);
		CHECK(w.sink.log.empty());
		w.Clean();
	}
	{
		// Busy: REQUEUE-A. An inspect popup over an open inventory panel: it
		// is told to close, and the movie is re-queued too -- and that check
		// runs even when busy already blocks (dbase100.c:927-931).
		World w({ { W(0x04, 1), W(0x07, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.in.State().g_dialogue_busy_flag = 1;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 2, 0), NEG1);
		CHECK(w.sink.log.empty());
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].chain, w.Cmd(1, 1));
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].count, 1);
		CHECK_EQ(w.in.State().g_dialogue_action_queue_count, 1);

		w.in.ResetDialogueState();
		w.sink.panelOpen = 1;
		w.sink.popup = 1;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 2, 0), NEG1);
		CHECK_STR(w.sink.Joined(), "ArmPanelExit");
		CHECK_EQ(w.in.State().g_dialogue_action_queue_count, 1);

		w.in.ResetDialogueState();
		w.sink.Clear();
		w.in.State().g_dialogue_busy_flag = 1;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 2, 0), NEG1);
		CHECK_STR(w.sink.Joined(), "ArmPanelExit");

		// Panel open but the popup in another state: not blocked.
		w.in.ResetDialogueState();
		w.sink.Clear();
		w.sink.popup = 2;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 2, 0), 2);
		CHECK_STR(w.sink.Joined(), "PlayGdv(0) Latch83b20");
		w.Clean();
	}
	{
		// A text context up with the queue marked active: the queue is
		// drained FIRST, then the movie plays (dbase100.c:924-926).
		World w({ { W(0x07, 1) }, { W(0x04, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		roth::Dbase100InterpState &s = w.in.State();
		s.g_active_dialogue_context = 3;
		s.g_dbase100_queue_active = 1;
		s.g_dialogue_action_queue_count = 1;
		s.g_dialogue_action_queue[0].chain = w.Cmd(2);
		s.g_dialogue_action_queue[0].count = 1;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 1, 0), 1);
		CHECK(w.Flag(1));
		CHECK_EQ(s.g_dialogue_action_queue_count, 0);
		CHECK_STR(w.sink.Joined(), "PlayGdv(0) Latch83b20");
		w.Clean();
	}

	g_test = "op 0x0e";
	{
		// Shows the image with the whole 24-bit operand, counted FIRST
		// (dbase100.c:917-918). Busy: REQUEUE-A.
		World w({ { W(0x0e, 0x123456), W(0x04, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 2);
		CHECK_STR(w.sink.Joined(), "Image(0x123456)");
		w.sink.Clear();
		w.in.State().g_dialogue_busy_flag = 1;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 2, 4), NEG1);
		CHECK(w.sink.log.empty());
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].chain, w.Cmd(1));
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].count, 2);
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].flags, 4);
		w.in.ResetDialogueState();
		w.sink.panelOpen = 1;
		w.sink.popup = 1;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 2, 0), NEG1);
		CHECK_STR(w.sink.Joined(), "ArmPanelExit");
		w.Clean();
	}
}

//==========================================================================
//
// The choice menu: 0x08 / 0x09 / 0x0a
//
//==========================================================================

// Three choices, the second gated on flag 40, three branches setting flags
// 1, 2, 3.
Words Menu()
{
	return { W(0x04, 20),
		W(0x08, 0x100), W(0x0d, 40), W(0x08, 0x200), W(0x08, 0x300),
		W(0x09, 0xffffff), W(0x04, 1), W(0x0a, 0xffffff),
		W(0x09, 0xffffff), W(0x04, 2), W(0x0a, 0xffffff),
		W(0x09, 0xffffff), W(0x04, 3), W(0x0a, 0xffffff) };
}

void SetMenuText(RecSink &s)
{
	s.text[0x100] = std::string("Yes\0", 4);
	s.text[0x200] = std::string("Maybe\0", 6);
	s.text[0x300] = std::string("No\0", 3);
}

void TestChoices()
{
	g_test = "choice menu";
	{
		// All three visible. The text is each line with its last byte made
		// '\n' (dbase100.c:704); the menu opens, busy = 1, return 1, and the
		// cursor parks on the first 0x09 with its count (dbase100.c:720-727).
		World w({ Menu() });
		if (!w.Go()) { CHECK(false); return; }
		SetMenuText(w.sink);
		w.in.Dbase100BitmapTestSet(40);
		w.in.State().g_dbase100_queue_active = 1;
		w.in.State().g_dbase100_pending_topic = 9;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 14, 1), 1);
		// Room shrinks by what each line took: 0x800, then -4, then -6.
		CHECK_STR(w.sink.Joined(), "Read(0x100,2048,0) Read(0x200,2044,0) Read(0x300,2038,0) OpenMenu(13)");
		CHECK_STR(w.sink.lastWindowText, "Yes\nMaybe\nNo\n");
		const roth::Dbase100InterpState &s = w.in.State();
		CHECK_EQ(s.g_dialogue_busy_flag, 1);
		CHECK_EQ(s.g_dbase100_choice_count, 3);
		CHECK_EQ(s.g_dbase100_choice_cursor, w.Cmd(1, 5));
		CHECK_EQ(s.g_dbase100_choice_remaining, 9);
		CHECK_EQ(s.g_dbase100_choice_mode, 1);
		CHECK_EQ(s.g_dbase100_choice_record_indices[0], 0);
		CHECK_EQ(s.g_dbase100_choice_record_indices[1], 1);
		CHECK_EQ(s.g_dbase100_choice_record_indices[2], 2);
		CHECK_EQ(s.g_dbase100_queue_active, 0);             // dbase100.c:673
		CHECK_EQ(s.g_dbase100_pending_topic, 9);            // no finalize on this return
		CHECK(w.Flag(20));
		CHECK(!w.Flag(1));

		// Pick the third: close_dialogue_and_run_branch (dbase100.c:532-540).
		w.sink.Clear();
		w.in.State().g_choice_saved_index = 2;
		w.in.State().g_choice_selected_index = 2;
		w.in.State().g_active_dialogue_context = 3;
		w.in.State().g_move_freeze_gate = 0x6ffff;
		w.in.CloseDialogueAndRunBranch();
		CHECK(!w.Flag(1));
		CHECK(!w.Flag(2));
		CHECK(w.Flag(3));
		CHECK_EQ(s.g_dbase100_choice_cursor, 0);
		CHECK_EQ(s.g_dialogue_busy_flag, 0);
		CHECK_EQ(s.g_active_dialogue_context, 0);
		CHECK_EQ(s.g_move_freeze_gate, 0);
		CHECK_EQ(s.g_choice_selected_index, -1);
		CHECK_EQ(s.g_dbase100_pending_topic, 0);            // the branch's 0x0a finalized
		// No choice parked any more: a second accept does nothing but
		// return the index (dbase100.c:504).
		CHECK_EQ(w.in.ExecuteDialogueBranch(1), 1);
		CHECK(!w.Flag(2));
		w.Clean();
	}
	{
		// The gated choice hidden: two visible. Its sub-record index is
		// still used up (dbase100.c:696, 710-712), so visible choice 1 maps
		// to sub-record 2 and runs the THIRD branch.
		World w({ Menu() });
		if (!w.Go()) { CHECK(false); return; }
		SetMenuText(w.sink);
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 14, 0), 1);
		CHECK_STR(w.sink.lastWindowText, "Yes\nNo\n");
		CHECK_EQ(w.in.State().g_dbase100_choice_count, 2);
		CHECK_EQ(w.in.State().g_dbase100_choice_record_indices[0], 0);
		CHECK_EQ(w.in.State().g_dbase100_choice_record_indices[1], 2);
		CHECK_EQ(w.in.ExecuteDialogueBranch(1), 0);
		CHECK(w.Flag(3));
		CHECK(!w.Flag(2));
		w.Clean();
	}
	{
		// The gate as If-NOT, and a gate FIRST: the chain reaches the menu
		// through op 0x0d's look-ahead (dbase100.c:656) and the builder
		// re-reads the gate.
		World w({ { W(0x0d, 40, true), W(0x08, 0x100), W(0x08, 0x300),
			W(0x09, 0xffffff), W(0x04, 1), W(0x0a, 1), W(0x09, 0xffffff), W(0x04, 2), W(0x0a, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		SetMenuText(w.sink);
		CHECK_EQ(w.Run(1), 1);
		CHECK_STR(w.sink.lastWindowText, "Yes\nNo\n");
		CHECK_EQ(w.in.State().g_dbase100_choice_cursor, w.Cmd(1, 3));
		CHECK_EQ(w.in.State().g_dbase100_choice_remaining, 6);

		// Flag set: the If-NOT gate hides "Yes"; ONE choice is left, so no
		// menu opens and its branch runs at once -- sub-record 1, the
		// second branch. Returns the choice count, 1 (dbase100.c:728-733).
		w.in.ResetDialogueState();
		w.sink.Clear();
		w.in.Dbase100BitmapTestSet(40);
		w.in.State().g_dialogue_busy_flag = 0;
		w.in.State().g_move_freeze_gate = 5;
		w.in.State().g_active_dialogue_context = 5;
		CHECK_EQ(w.Run(1), 1);
		CHECK_STR(w.sink.Joined(), "Read(0x300,2048,0)");
		CHECK(!w.Flag(1));
		CHECK(w.Flag(2));
		CHECK_EQ(w.in.State().g_dialogue_busy_flag, 0);
		CHECK_EQ(w.in.State().g_move_freeze_gate, 0);
		CHECK_EQ(w.in.State().g_active_dialogue_context, 0);
		CHECK_EQ(w.in.State().g_dbase100_choice_cursor, 0);
		w.Clean();
	}
	{
		// No visible choice (no text at all): counted, and the chain carries
		// on FROM THE STOP RECORD -- the 0x09 is skipped by the main loop and
		// the first branch runs (dbase100.c:719).
		World w({ Menu() });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 14, 0), 1);    // the branch's 0x0a
		CHECK(w.Flag(1));
		CHECK(!w.Flag(2));
		CHECK_EQ(w.in.State().g_dbase100_choice_cursor, 0);
		CHECK_EQ(w.sink.Count("Dbase100OpenDialogueWindow"), 0);
		CHECK_EQ(Unhandled(w.in, 0x09), 1);
		w.Clean();
	}
	{
		// The window refuses to open: the choice record stays published, not
		// busy, counted, and the chain carries on from the stop record
		// (dbase100.c:725).
		World w({ { W(0x08, 0x100), W(0x08, 0x300), W(0x09, 0xffffff), W(0x04, 1), W(0x10, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		SetMenuText(w.sink);
		w.sink.openWindow = 0;
		CHECK_EQ(w.Run(1), 3);
		CHECK(w.Flag(1));
		CHECK_EQ(w.in.State().g_dialogue_busy_flag, 0);
		CHECK_EQ(w.in.State().g_dbase100_choice_cursor, w.Cmd(1, 2));
		CHECK_EQ(w.in.State().g_dbase100_choice_count, 2);
		w.Clean();
	}
	{
		// The record ends inside the builder: return 1, nothing published,
		// no window -- even with choices collected (dbase100.c:716).
		World w({ { W(0x04, 1), W(0x08, 0x100), W(0x08, 0x300) }, { W(0x04, 2) } });
		if (!w.Go()) { CHECK(false); return; }
		SetMenuText(w.sink);
		w.in.State().g_dbase100_pending_topic = 9;
		CHECK_EQ(w.Run(1), 1);
		CHECK_EQ(w.sink.Count("ReadNextDialogueLine"), 2);
		CHECK_EQ(w.sink.Count("Dbase100OpenDialogueWindow"), 0);
		CHECK_EQ(w.in.State().g_dbase100_choice_cursor, 0);
		CHECK_EQ(w.in.State().g_dbase100_pending_topic, 9);
		CHECK(!w.Flag(2));
		w.Clean();
	}
	{
		// Busy when the menu is reached: REQUEUE-A on the 0x08 record itself.
		World w({ Menu() });
		if (!w.Go()) { CHECK(false); return; }
		SetMenuText(w.sink);
		w.in.State().g_dialogue_busy_flag = 1;
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 14, 0), NEG1);
		CHECK(w.sink.log.empty());
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].chain, w.Cmd(1, 1));
		CHECK_EQ(w.in.State().g_dialogue_action_queue[0].count, 13);
		CHECK_EQ(w.in.State().g_dialogue_action_queue_count, 1);
		// Later, not busy: the queued entry builds the menu.
		w.in.State().g_dialogue_busy_flag = 0;
		w.in.AdvanceDialogueActionQueue();
		CHECK_STR(w.sink.lastWindowText, "Yes\nNo\n");
		CHECK_EQ(w.in.State().g_dialogue_action_queue_count, 0);
		CHECK_EQ(w.in.State().g_dbase100_choice_cursor, w.Cmd(1, 5));
		w.Clean();
	}
	{
		// A nested menu inside an earlier branch: its 0x08 lines add to the
		// target, its 0x09s take off again, so the outer branch is still
		// found (dbase100.c:515-524).
		World w({ { W(0x08, 0x100), W(0x08, 0x300),
			W(0x09, 0xffffff), W(0x08, 0x100), W(0x08, 0x300),
				W(0x09, 0xffffff), W(0x04, 11), W(0x0a, 1),
				W(0x09, 0xffffff), W(0x04, 12), W(0x0a, 1),
			W(0x09, 0xffffff), W(0x04, 2), W(0x0a, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		SetMenuText(w.sink);
		CHECK_EQ(w.Run(1), 1);
		CHECK_EQ(w.in.ExecuteDialogueBranch(1), 0);
		CHECK(w.Flag(2));
		CHECK(!w.Flag(11));
		CHECK(!w.Flag(12));
		w.Clean();
	}
	{
		// A branch index with no matching 0x09: the walk runs off the end of
		// the record and returns what was left of the target; nothing runs,
		// and the walk stops at the record's end (dbase100.c:525).
		World w({ { W(0x08, 0x100), W(0x08, 0x300), W(0x09, 0xffffff), W(0x04, 1), W(0x0a, 1) }, { W(0x04, 2) } });
		if (!w.Go()) { CHECK(false); return; }
		SetMenuText(w.sink);
		CHECK_EQ(w.Run(1), 1);
		w.in.State().g_dbase100_choice_record_indices[1] = 3;   // as if two more choices had been hidden
		CHECK_EQ(w.in.ExecuteDialogueBranch(1), 2);
		CHECK(!w.Flag(1));
		CHECK(!w.Flag(2));
		CHECK_EQ(w.in.State().g_dbase100_choice_cursor, 0);
		w.Clean();
	}
	{
		// The room is 0x800 bytes: a line longer than what is left is cut to
		// it by the reader, and a line with no room reads as 0 and is not a
		// choice (dbase100.c:670, 698-699).
		World w({ { W(0x08, 0x100), W(0x08, 0x200), W(0x08, 0x300), W(0x09, 0xffffff), W(0x0a, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.text[0x100] = std::string(0x7fe, 'a');
		w.sink.text[0x200] = std::string("bcdef\0", 6);
		w.sink.text[0x300] = std::string("g\0", 2);
		CHECK_EQ(w.Run(1), 1);
		CHECK_STR(w.sink.Joined(), "Read(0x100,2048,0) Read(0x200,2,0) Read(0x300,0,0) OpenMenu(2048)");
		CHECK_EQ(w.in.State().g_dbase100_choice_count, 2);
		CHECK_EQ(w.sink.lastWindowText.size(), 0x800);
		CHECK_EQ(w.sink.lastWindowText[0x7fd], '\n');
		CHECK_EQ(w.sink.lastWindowText[0x7fe], 'b');
		CHECK_EQ(w.sink.lastWindowText[0x7ff], '\n');
		w.Clean();
	}
	{
		// ROTH.C writes the index table with no bound. The 17th visible
		// choice lands on the queue-draining flag at +0x40 -- kept, because
		// it is what the original does; past the global's extent the write
		// is refused and counted.
		Words many;
		for (int i = 0; i < 21; i++) many.push_back(W(0x08, 0x100));
		many.push_back(W(0x09, 0xffffff));
		many.push_back(W(0x0a, 1));
		World w({ many });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.anyText = true;
		CHECK_EQ(w.Run(1), 1);
		CHECK_EQ(w.in.State().g_dbase100_choice_count, 21);
		CHECK_EQ(w.in.State().g_dbase100_choice_record_indices[15], 15);
		CHECK_EQ(w.in.State().g_dbase100_choice_record_indices[roth::Dbase100InterpState::QUEUE_DRAINING], 16);
		CHECK_EQ(Anom(w.in, "more visible choices than the choice table's extent"), 2);
		CHECK_EQ(w.in.ExecuteDialogueBranch(25), 0);
		CHECK_EQ(Anom(w.in, "dialogue branch index outside the choice table"), 1);
	}
	{
		// dialogue_voice_force_end may tail-call close_dialogue_and_run_branch
		// (audio.c:1343-1347): the interpreter is re-entered from the sink
		// while run_dialogue_action_queue is on the stack.
		World w({ Menu(), { W(0x04, 30) } });
		if (!w.Go()) { CHECK(false); return; }
		SetMenuText(w.sink);
		w.in.Dbase100BitmapTestSet(40);
		CHECK_EQ(w.Run(1), 1);
		CHECK_EQ(w.in.State().g_dialogue_busy_flag, 1);
		w.in.State().g_choice_saved_index = 1;
		w.sink.forceEndCallsBack = &w.in;
		w.in.State().g_dbase100_queue_active = 1;
		w.in.State().g_dialogue_action_queue_count = 1;
		w.in.State().g_dialogue_action_queue[0].chain = w.Cmd(2);
		w.in.State().g_dialogue_action_queue[0].count = 1;
		w.in.RunDialogueActionQueue();
		CHECK(w.Flag(2));               // the branch, from inside the sink
		CHECK(w.Flag(30));              // then the queued entry
		CHECK_EQ(w.in.State().g_dialogue_action_queue_count, 0);
		w.Clean();
	}
}

//==========================================================================
//
// Random: 0x0b / 0x0c
//
//==========================================================================

void TestRandom()
{
	g_test = "filter";
	{
		// Segments end at a KEEP opcode; the scan stops after an 0x0c. The
		// entry after the last closed one is the open segment
		// (dbase100.c:41-100).
		World w({ { W(0x0b, 0xffffff),
			W(0x05, 0x100),
			W(0x01, 40), W(0x04, 1), W(0x05, 0x200),
			W(0x11, 3),
			W(0x04, 2),
			W(0x0c, 0xffffff),
			W(0x04, 9) } });
		if (!w.Go()) { CHECK(false); return; }
		uint32_t cur = w.Cmd(1, 1);
		std::vector<roth::Dbase100Interp::Segment> segs;
		int32_t active = -1;
		CHECK_EQ(w.in.FilterDbase100ActiveRecords(cur, 8, segs, active), 1);
		CHECK_EQ(active, 3);
		CHECK_EQ(cur, w.Cmd(1, 8));                     // just past the 0x0c
		CHECK_EQ(segs[0].chain, w.Cmd(1, 1)); CHECK_EQ(segs[0].count, 1);
		CHECK_EQ(segs[1].chain, w.Cmd(1, 2)); CHECK_EQ(segs[1].count, 3);
		CHECK_EQ(segs[2].chain, w.Cmd(1, 5)); CHECK_EQ(segs[2].count, 1);
		CHECK_EQ(segs[3].chain, w.Cmd(1, 6)); CHECK_EQ(segs[3].count, 2);   // open: 0x04, 0x0c
		// Every KEEP opcode closes a segment.
		const uint8_t keep[] = { 0x05, 0x07, 0x0e, 0x11, 0x1a, 0x1d, 0x23, 0x26, 0x2d, 0x36, 0x37 };
		for (uint8_t op : keep)
		{
			World k({ { W(op, 1), W(0x0c, 1) } });
			if (!k.Go()) { CHECK(false); return; }
			uint32_t c = k.Cmd(1);
			CHECK_EQ(k.in.FilterDbase100ActiveRecords(c, 2, segs, active), 0);
			CHECK_EQ(active, 1);
		}
		// The count runs out before any 0x0c: the scan stops there.
		cur = w.Cmd(1, 1);
		CHECK_EQ(w.in.FilterDbase100ActiveRecords(cur, 2, segs, active), 0);
		CHECK_EQ(active, 1);
		CHECK_EQ(cur, w.Cmd(1, 3));
		w.Clean();
	}
	{
		// A failed 0x0d gate drops the segment it sits in -- and, because
		// nothing ever clears the flag again (dbase100.c:68-72), every
		// segment after it.
		World w({ { W(0x05, 0x100), W(0x0d, 40), W(0x05, 0x200), W(0x05, 0x300), W(0x0c, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		uint32_t cur = w.Cmd(1);
		std::vector<roth::Dbase100Interp::Segment> segs;
		int32_t active = -1;
		w.in.FilterDbase100ActiveRecords(cur, 5, segs, active);
		CHECK_EQ(active, 1);
		CHECK_EQ(segs[0].count, 1);
		CHECK_EQ(segs[1].chain, w.Cmd(1, 4));           // restarted after each dropped KEEP
		// With the flag set the gate holds and all three are kept; the gate
		// record is counted into its segment.
		w.in.Dbase100BitmapTestSet(40);
		cur = w.Cmd(1);
		w.in.FilterDbase100ActiveRecords(cur, 5, segs, active);
		CHECK_EQ(active, 3);
		CHECK_EQ(segs[1].chain, w.Cmd(1, 1)); CHECK_EQ(segs[1].count, 2);
		// If-NOT gate: fails when the flag is set.
		World n({ { W(0x0d, 40, true), W(0x05, 0x200), W(0x0c, 1) } });
		if (!n.Go()) { CHECK(false); return; }
		cur = n.Cmd(1);
		n.in.FilterDbase100ActiveRecords(cur, 3, segs, active);
		CHECK_EQ(active, 1);
		n.in.Dbase100BitmapTestSet(40);
		cur = n.Cmd(1);
		n.in.FilterDbase100ActiveRecords(cur, 3, segs, active);
		CHECK_EQ(active, 0);
		w.Clean();
	}

	g_test = "op 0x0b";
	{
		// Three one-record segments; the RNG picks; the pick runs as its own
		// chain; then the outer chain carries on after the 0x0c. The 0x0b
		// itself is counted, the sub-chain's result is dropped
		// (dbase100.c:745-758).
		const Words chain = { W(0x0b, 0xffffff), W(0x11, 1), W(0x11, 2), W(0x11, 3), W(0x0c, 0xffffff), W(0x04, 9) };
		for (int pick = 0; pick < 3; pick++)
		{
			World w({ chain });
			if (!w.Go()) { CHECK(false); return; }
			w.sink.rng.push_back(pick);
			CHECK_EQ(w.Run(1), 2);
			CHECK_STR(w.sink.Joined(), F("Rng(3) Give(%lld,0)", pick + 1));
			CHECK(w.Flag(9));
			w.Clean();
		}
		{
			// Only the low 16 bits of the RNG result are used (dbase100.c:753).
			World w({ chain });
			if (!w.Go()) { CHECK(false); return; }
			w.sink.rng.push_back(0x70002);
			CHECK_EQ(w.Run(1), 2);
			CHECK_STR(w.sink.Joined(), "Rng(3) Give(3,0)");
		}
		{
			// An index outside the list is refused and counted.
			World w({ chain });
			if (!w.Go()) { CHECK(false); return; }
			w.sink.rng.push_back(3);
			CHECK_EQ(w.Run(1), 2);
			CHECK_STR(w.sink.Joined(), "Rng(3)");
			CHECK_EQ(Anom(w.in, "RngNextIndexForCount returned an index outside the list"), 1);
		}
	}
	{
		// A multi-record segment runs whole, gates included; the caller's
		// flags are passed down (scan mode here: the sub-chain returns 1 at
		// its give and the outer chain still goes on).
		World w({ { W(0x0b, 1), W(0x01, 40), W(0x04, 1), W(0x11, 5), W(0x04, 2), W(0x11, 6), W(0x0c, 1), W(0x10, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.in.Dbase100BitmapTestSet(40);
		CHECK_EQ(w.Run(1), 2);
		CHECK_STR(w.sink.Joined(), "Rng(2) Give(5,0)");
		CHECK(w.Flag(1));
		CHECK(!w.Flag(2));
		w.sink.Clear();
		w.in.Dbase100BitmapTestClear(1);
		w.sink.rng.push_back(1);
		CHECK_EQ(w.in.ExecuteDbase100Chain(w.Cmd(1), 8, 2), 2);
		CHECK_STR(w.sink.Joined(), "Rng(2)");
		CHECK(!w.Flag(2));
		w.Clean();
	}
	{
		// Nothing after the 0x0b (count 0): counted, no scan, no RNG. No
		// active segment: counted, no RNG.
		World w({ { W(0x10, 1), W(0x0b, 1) }, { W(0x0b, 1), W(0x04, 1), W(0x0c, 1), W(0x10, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 2);
		CHECK_EQ(w.Run(2), 2);
		CHECK(!w.Flag(1));              // the unkept records were scanned over, not run
		CHECK(w.sink.log.empty());
		w.Clean();
	}
	{
		// No 0x0c: the scan takes the rest of the record, and the chain ends
		// after the pick.
		World w({ { W(0x0b, 1), W(0x11, 1), W(0x11, 2) }, { W(0x04, 1) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.rng.push_back(1);
		CHECK_EQ(w.Run(1), 1);
		CHECK_STR(w.sink.Joined(), "Rng(2) Give(2,0)");
		CHECK(!w.Flag(1));
		w.Clean();
	}
}

//==========================================================================
//
// The queue
//
//==========================================================================

void TestQueue()
{
	g_test = "queue";
	{
		// Empty queue: the record runs now. Anything queued: the record is
		// appended {chain, count, flag} and 1 is returned; with 8 queued it
		// is dropped and 0 is returned (dbase100.c:437-446).
		std::vector<Words> acts;
		for (int i = 0; i < 12; i++) acts.push_back({ W(0x04, (uint32_t)(100 + i)), W(0x10, 1) });
		World w(acts);
		if (!w.Go()) { CHECK(false); return; }
		roth::Dbase100InterpState &s = w.in.State();
		CHECK_EQ(w.Run(1), 2);
		CHECK(w.Flag(100));

		s.g_dialogue_action_queue_count = 1;        // something is parked in slot 0
		s.g_dialogue_action_queue[0].chain = w.Cmd(2);
		s.g_dialogue_action_queue[0].count = 2;
		for (int id = 3; id <= 9; id++)
		{
			CHECK_EQ(w.in.EvalOrQueueDialogueRecordCommands(w.L.actionOffsets[(size_t)id - 1], (uint32_t)id * 4), 1);
			CHECK(!w.Flag((uint32_t)(100 + id - 1)));
			CHECK_EQ(s.g_dialogue_action_queue_count, id - 1);
			CHECK_EQ(s.g_dialogue_action_queue[id - 2].chain, w.Cmd(id));
			CHECK_EQ(s.g_dialogue_action_queue[id - 2].count, 2);
			CHECK_EQ(s.g_dialogue_action_queue[id - 2].flags, id * 4);
		}
		CHECK_EQ(s.g_dialogue_action_queue_count, 8);
		CHECK_EQ(w.Run(10), 0);                     // full: dropped
		CHECK_EQ(w.Run(11), 0);
		CHECK_EQ(s.g_dialogue_action_queue_count, 8);

		// advance: one entry per call, in order; -1 would leave it.
		w.in.AdvanceDialogueActionQueue();
		CHECK(w.Flag(101));
		CHECK(!w.Flag(102));
		CHECK_EQ(s.g_dialogue_action_queue_count, 7);
		CHECK_EQ(s.g_dialogue_action_queue[0].chain, w.Cmd(3));
		CHECK_EQ(s.g_dialogue_action_queue[6].chain, w.Cmd(9));

		// With room again a new record queues behind the rest.
		CHECK_EQ(w.Run(12), 1);
		CHECK_EQ(s.g_dialogue_action_queue_count, 8);
		CHECK_EQ(s.g_dialogue_action_queue[7].chain, w.Cmd(12));

		// run: drains everything in order, with the draining flag up while
		// it does and down after (dbase100.c:454, 479).
		w.in.RunDialogueActionQueue();
		CHECK_EQ(s.g_dialogue_action_queue_count, 0);
		for (uint32_t f = 102; f <= 108; f++) CHECK(w.Flag(f));
		CHECK(!w.Flag(109));
		CHECK(!w.Flag(110));
		CHECK(w.Flag(111));
		CHECK_EQ(s.g_dbase100_choice_record_indices[roth::Dbase100InterpState::QUEUE_DRAINING], 0);
		w.Clean();
	}
	{
		// Drain order, seen through the sink.
		World w({ { W(0x11, 1) }, { W(0x11, 2) }, { W(0x11, 3) }, { W(0x11, 4) } });
		if (!w.Go()) { CHECK(false); return; }
		roth::Dbase100InterpState &s = w.in.State();
		s.g_dialogue_action_queue_count = 1;
		s.g_dialogue_action_queue[0].chain = w.Cmd(1);
		s.g_dialogue_action_queue[0].count = 1;
		CHECK_EQ(w.Run(3), 1);
		CHECK_EQ(w.Run(2), 1);
		CHECK(w.sink.log.empty());
		// eval with the queue marked active drains it BEFORE looking at the
		// new record, which then finds the queue empty and runs at once
		// (dbase100.c:431-438).
		s.g_dbase100_queue_active = 1;
		CHECK_EQ(w.Run(4), 1);
		CHECK_STR(w.sink.Joined(), "Give(1,0) Give(3,0) Give(2,0) Give(4,0)");
		CHECK_EQ(s.g_dialogue_action_queue_count, 0);
		w.Clean();
	}
	{
		// run with a line up: the voice is force-ended, queue-active drops,
		// and unless the stream is still in state 1 the busy / context /
		// gate globals are cleared -- before the loop and after each entry
		// (dbase100.c:455-459, 472-476).
		World w({ { W(0x05, 0x100), W(0x04, 1) }, { W(0x04, 2) } });
		if (!w.Go()) { CHECK(false); return; }
		w.sink.anyText = true;
		roth::Dbase100InterpState &s = w.in.State();
		s.g_dialogue_busy_flag = 1;
		s.g_active_dialogue_context = 2;
		s.g_move_freeze_gate = 0x7ffff;
		s.g_voice_stream_state = 2;
		s.g_dbase100_queue_active = 1;
		s.g_dialogue_action_queue_count = 2;
		s.g_dialogue_action_queue[0].chain = w.Cmd(1);
		s.g_dialogue_action_queue[0].count = 2;
		s.g_dialogue_action_queue[1].chain = w.Cmd(2);
		s.g_dialogue_action_queue[1].count = 1;
		w.in.RunDialogueActionQueue();
		// The queued line is NOT spoken: op 0x05 is skipped while draining.
		CHECK_STR(w.sink.Joined(), "ForceEnd(0,0)");
		CHECK(w.Flag(1));
		CHECK(w.Flag(2));
		CHECK_EQ(s.g_dialogue_busy_flag, 0);
		CHECK_EQ(s.g_active_dialogue_context, 0);
		CHECK_EQ(s.g_move_freeze_gate, 0);
		CHECK_EQ(s.g_voice_stream_state, 0);
		CHECK_EQ(s.g_dbase100_queue_active, 0);
		CHECK_EQ(s.g_dialogue_action_queue_count, 0);

		// Stream still in state 1 after the force-end: busy is left alone.
		s.g_dialogue_busy_flag = 1;
		s.g_voice_stream_state = 1;
		s.g_active_dialogue_context = 2;
		w.sink.Clear();
		w.in.RunDialogueActionQueue();
		CHECK_STR(w.sink.Joined(), "ForceEnd(0,0)");
		CHECK_EQ(s.g_dialogue_busy_flag, 1);
		CHECK_EQ(s.g_active_dialogue_context, 2);
		w.Clean();
	}
	{
		// A head entry that re-queues itself is not popped by advance
		// (dbase100.c:493) ...
		World w({ { W(0x0e, 5), W(0x04, 1) }, { W(0x04, 2) } });
		if (!w.Go()) { CHECK(false); return; }
		roth::Dbase100InterpState &s = w.in.State();
		w.sink.panelOpen = 1;
		w.sink.popup = 1;
		s.g_dialogue_action_queue_count = 2;
		s.g_dialogue_action_queue[0].chain = w.Cmd(1);
		s.g_dialogue_action_queue[0].count = 2;
		s.g_dialogue_action_queue[1].chain = w.Cmd(2);
		s.g_dialogue_action_queue[1].count = 1;
		w.in.AdvanceDialogueActionQueue();
		CHECK_EQ(s.g_dialogue_action_queue_count, 2);
		CHECK_EQ(s.g_dialogue_action_queue[0].chain, w.Cmd(1));
		CHECK(!w.Flag(1));
		// ... and when the popup has gone, it runs and the queue moves on.
		w.sink.popup = 0;
		w.in.AdvanceDialogueActionQueue();
		CHECK(w.Flag(1));
		CHECK_EQ(s.g_dialogue_action_queue_count, 1);
		CHECK_EQ(s.g_dialogue_action_queue[0].chain, w.Cmd(2));
		w.in.AdvanceDialogueActionQueue();
		CHECK(w.Flag(2));
		CHECK_EQ(s.g_dialogue_action_queue_count, 0);
		w.in.AdvanceDialogueActionQueue();              // empty: nothing
		w.Clean();

		// run_dialogue_action_queue on an entry that can never get through
		// loops for ever in ROTH.C; the guard stops it and says so.
		w.sink.popup = 1;
		s.g_dialogue_action_queue_count = 1;
		s.g_dialogue_action_queue[0].chain = w.Cmd(1);
		s.g_dialogue_action_queue[0].count = 2;
		w.in.RunDialogueActionQueue();
		CHECK_EQ(Anom(w.in, "run_dialogue_action_queue did not drain"), 1);
	}
}

//==========================================================================
//
// The entries: by id, and item trigger blocks
//
//==========================================================================

void TestEntries()
{
	g_test = "eval by id";
	{
		World w({ { W(0x04, 1) }, {}, { W(0x04, 3) } });
		w.spec.actions[1].absent = true;
		if (!w.Go()) { CHECK(false); return; }
		CHECK_EQ(w.Run(1), 1);
		CHECK_EQ(w.Run(2), 0);                  // table entry 0 (dbase100.c:552)
		CHECK_EQ(w.Run(3), 1);                  // id == count is the last record
		CHECK_EQ(w.Run(4), 0);                  // id > count (dbase100.c:549)
		CHECK_EQ(w.in.EvalDialogueRecordById(0x7fffffff), 0);
		w.Clean();
		// The compare is signed and there is no lower bound: id 0 reads the
		// dword BEFORE the table. In this fixture that is the last cutscene
		// entry's +0x10 word, 0: no record. Counted.
		CHECK_EQ(w.Run(0), 0);
		CHECK_EQ(Anom(w.in, "eval_dialogue_record_by_id called with an id below 1"), 1);
		CHECK_EQ(w.in.EvalDialogueRecordById(0x80000000u), 0);
		CHECK_EQ(w.in.OutOfImageReads(), 1);
	}

	g_test = "trigger blocks";
	{
		// An item trigger block is run from its header, exactly like an
		// action record: the low 16 bits of the header are the byte length
		// (dbase100.c:433-436; the callers are dialogue_ui.c:74-98,
		// input.c:1010, inventory.c:90).
		World w({ { W(0x0a, 1) } });
		fx::ItemSpec it;
		it.renderId = 0x294;
		it.blocks.push_back({ 0x04, { W(0x05, 0x100), W(0x04, 1) } });              // OnInspect
		it.blocks.push_back({ 0x01, { W(0x0f, 0x2000), W(0x01, 40), W(0x05, 0x200) } });   // OnInformation
		it.blocks.push_back({ 0x0b, { W(0x34, 7), W(0x1c, 1, true), W(0x04, 2) } });      // OnUse
		w.spec.items.push_back(it);
		if (!w.Go()) { CHECK(false); return; }
		w.sink.anyText = true;
		const roth::DbItem *item = w.db.Item(1);
		CHECK(item != nullptr);
		if (!item) return;

		// Examine (flag 1): the line opens; cleanup clears queue-active.
		w.in.State().g_dbase100_queue_active = 1;
		CHECK_EQ(w.in.EvalBlock(item->blocks[0], 1), 2);
		CHECK_STR(w.sink.Joined(), "Read(0x100,1020,1) OpenLine(5)");
		CHECK(w.Flag(1));
		CHECK_EQ(w.in.State().g_dbase100_queue_active, 0);

		// "Is this topic available": scan (flag 2). Gate closed -> 0; open
		// -> 1 at the line, which is not spoken.
		w.in.ResetDialogueState();
		w.sink.Clear();
		CHECK_EQ(w.in.EvalBlock(item->blocks[1], 2), 0);
		w.in.Dbase100BitmapTestSet(40);
		CHECK_EQ(w.in.EvalBlock(item->blocks[1], 2), 1);
		CHECK(w.sink.log.empty());
		CHECK_EQ(Unhandled(w.in, 0x0f), 2);     // the topic word is not the interpreter's

		// OnUse: 0x34 is skipped; the If-NOT caller gate lets flag bit 0
		// through and stops a caller without it.
		CHECK_EQ(w.in.EvalBlock(item->blocks[2], 0), 0);
		CHECK(!w.Flag(2));
		CHECK_EQ(w.in.EvalBlock(item->blocks[2], 1), 1);
		CHECK(w.Flag(2));
		w.Clean();

		// The last block's last command is followed by the end marker: a
		// do-while read past the block lands on a 0 word.
		const roth::DbTriggerBlock &last = item->blocks[2];
		CHECK_EQ(w.in.ExecuteDbase100Chain(last.firstCommand + (uint32_t)last.commandCount * 4, 0, 0), 0);
		CHECK_EQ(w.in.OutOfImageReads(), 0);
	}
}

//==========================================================================
//
// Random chains under the sanitizers: whatever the words, nothing is read
// outside the image and nothing hangs.
//
//==========================================================================

void TestFuzz()
{
	g_test = "fuzz";
	uint32_t seed = 0x1234567u;
	auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
	const uint8_t ops[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
		0x10, 0x11, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x23, 0x26, 0x2d, 0x36, 0x37, 0x0f, 0x00, 0x40 };
	std::map<std::string, int> anomalies;
	for (int round = 0; round < 400; round++)
	{
		std::vector<Words> acts;
		const int nacts = 1 + (int)(next() % 6);
		for (int a = 0; a < nacts; a++)
		{
			Words ws;
			const int n = (int)(next() % 14);
			for (int i = 0; i < n; i++)
			{
				const uint8_t op = ops[next() % sizeof(ops)];
				uint32_t operand = next() % 8;
				if (op == 0x1d) operand = 1 + next() % (uint32_t)(nacts + 1);
				if (next() % 16 == 0) operand = next() & 0xffffff;
				ws.push_back(W(op, operand, next() % 3 == 0));
			}
			acts.push_back(ws);
		}
		World w(acts);
		w.spec.actions[next() % (uint32_t)nacts].unk02 = (uint16_t)next();
		if (!w.Go()) { CHECK(false); return; }
		w.sink.keepLog = false;
		w.sink.anyText = next() % 2 == 0;
		w.sink.loadChunk = next() % 2;
		w.sink.panelOpen = (int32_t)(next() % 2);
		w.sink.popup = (int32_t)(next() % 2);
		for (int i = 0; i < 64; i++) w.sink.rng.push_back((int32_t)(next() % 4));
		for (int i = 0; i < 8; i++) if (next() % 2) w.in.Dbase100BitmapTestSet(next() % 8);
		for (int step = 0; step < 12; step++)
		{
			roth::Dbase100InterpState &s = w.in.State();
			switch (next() % 6)
			{
			case 0: w.in.EvalDialogueRecordById(1 + next() % (uint32_t)nacts); break;
			case 1: w.in.EvalDialogueRecordConditionWithCleanup(w.L.actionOffsets[next() % (uint32_t)nacts], next() % 4); break;
			case 2: s.g_dialogue_busy_flag = 0; s.g_active_dialogue_context = 0; w.in.AdvanceDialogueActionQueue(); break;
			case 3: s.g_choice_saved_index = (int32_t)(next() % 4); w.in.CloseDialogueAndRunBranch(); break;
			case 4: s.g_dbase100_queue_active = 1; break;
			default: w.in.RunDialogueActionQueue(); break;
			}
		}
		// A random script may legitimately run off the end of the image
		// (the last record's do-while read), name a flag past the bitmap or
		// loop for ever; each of those must come out as a counted anomaly,
		// never as a real out-of-bounds access. The sanitizer build is the
		// proof of that; here the names are only totalled.
		for (const auto &kv : w.in.Anomalies()) anomalies[kv.first] += kv.second;
		CHECK(w.in.State().g_dialogue_action_queue_count >= 0
			&& w.in.State().g_dialogue_action_queue_count <= roth::Dbase100InterpState::QUEUE_SLOTS);
	}
	printf("  fuzz: 400 random databases; anomalies raised (all counted, none a real overrun):\n");
	for (const auto &kv : anomalies) printf("    %-62s x%d\n", kv.first.c_str(), kv.second);
}

//==========================================================================
//
// Retail smoke test
//
//==========================================================================

bool ReadFile(const char *path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
	fclose(f);
	return true;
}

struct Totals
{
	std::map<std::string, int> calls;
	std::map<int, int> unhandled;
	std::map<std::string, int> anomalies;
	std::map<long long, int> returns;
	int runs = 0, outOfImage = 0, operandZero = 0;

	void Add(const RecSink &s, const roth::Dbase100Interp &in, uint32_t ret)
	{
		runs++;
		for (const auto &kv : s.counts) calls[kv.first] += kv.second;
		for (const auto &kv : in.Unhandled()) unhandled[kv.first] += kv.second;
		for (const auto &kv : in.Anomalies()) anomalies[kv.first] += kv.second;
		outOfImage += in.OutOfImageReads();
		operandZero += in.OperandZeroSkips();
		returns[(long long)(int32_t)ret]++;
	}
	void Print(const char *title) const
	{
		printf("  %s: %d runs\n", title, runs);
		printf("    returns:");
		int shown = 0;
		for (const auto &kv : returns) { if (shown++ < 12) printf(" %lld x%d", kv.first, kv.second); }
		if (returns.size() > 12) printf(" ...");
		printf("\n    sink calls:\n");
		for (const auto &kv : calls) printf("      %-32s %d\n", kv.first.c_str(), kv.second);
		printf("    words not executed (operand 0: %d):", operandZero);
		for (const auto &kv : unhandled)
			printf(" %02x%s x%d", kv.first, roth::IsDbase100MarkerOpcode((uint8_t)kv.first) ? "(marker)" : "", kv.second);
		printf("\n    out-of-image reads: %d   anomalies:", outOfImage);
		if (anomalies.empty()) printf(" none");
		printf("\n");
		for (const auto &kv : anomalies) printf("      %s x%d\n", kv.first.c_str(), kv.second);
	}
};

// Lets everything parked play out, the way the game's per-frame pump and a
// player picking each choice in turn would, so branches and queued tails run
// too.
void PlayOut(roth::Dbase100Interp &in, RecSink &sink, int pick)
{
	(void)sink;
	for (int guard = 0; guard < 64; guard++)
	{
		roth::Dbase100InterpState &s = in.State();
		if (s.g_dbase100_choice_cursor != 0)
		{
			s.g_choice_saved_index = s.g_dbase100_choice_count > 0 ? pick % s.g_dbase100_choice_count : 0;
			in.CloseDialogueAndRunBranch();
			continue;
		}
		if (s.g_dialogue_action_queue_count == 0 && s.g_dialogue_busy_flag == 0) break;
		s.g_dialogue_busy_flag = 0;                 // voice_stream_pump, audio.c:1320-1326
		s.g_active_dialogue_context = 0;
		in.AdvanceDialogueActionQueue();
	}
}

void TestRetail(const char *path)
{
	g_test = "retail";
	std::vector<uint8_t> bytes;
	if (!ReadFile(path, bytes))
	{
		printf("retail: %s not found -- smoke test SKIPPED\n", path);
		return;
	}
	roth::Dbase100 db;
	CHECK(db.Load(std::move(bytes)));
	if (!db.IsLoaded()) { printf("  retail file did not load: %s\n", db.Error().c_str()); return; }
	printf("retail: %s\n  %d items, %d actions, %d cutscenes, header +0x0C = %u\n", path,
		db.ItemCount(), db.ActionCount(), db.CutsceneCount(), (unsigned)db.Header().unk0C);
	CHECK_EQ(db.ActionCount(), 694);
	CHECK_EQ(db.ItemCount(), 281);

	RecSink sink;
	roth::Dbase100Interp in;
	in.Attach(&db, &sink);
	CHECK_EQ(in.State().g_dbase100_record_bitmap.size(), 56);

	// ---- the four engine hooks, exact (game_core.c:757, 802, 825, 971) ----
	{
		// 1, boot: two movies and the music.
		CHECK_EQ(in.EvalDialogueRecordById(1), 3);
		CHECK_STR(sink.Joined(), "PlayGdv(54) Latch83b20 PlayGdv(53) Latch83b20 LoadChunk(0x294411)");
		CHECK(db.Cutscene(54) != nullptr && db.Cutscene(53) != nullptr);
		if (db.Cutscene(54) && db.Cutscene(53))
			printf("  action 1 (boot): movies %s, %s, music chunk 0x294411\n",
				db.Cutscene(54)->name.c_str(), db.Cutscene(53)->name.c_str());
		// 3, new game: a movie, the music, four items.
		sink.Clear(); in.ResetDialogueState();
		CHECK_EQ(in.EvalDialogueRecordById(3), 6);
		CHECK_STR(sink.Joined(), "PlayGdv(150) Latch83b20 LoadChunk(0x293cad) Give(68,0) Give(1,0) Give(15,0) Give(16,0)");
		if (db.Cutscene(150)) printf("  action 3 (new game): movie %s, music chunk 0x293cad, give 68 1 15 16\n", db.Cutscene(150)->name.c_str());
		// 4, death: If-NOT flag 430 gates the sound; the 0x2d sub-code 1 does nothing.
		sink.Clear(); in.ResetDialogueState();
		CHECK_EQ(in.EvalDialogueRecordById(4), 1);
		CHECK_STR(sink.Joined(), "Sfx(280,0xfcfe)");
		in.Dbase100BitmapTestSet(430);
		sink.Clear();
		CHECK_EQ(in.EvalDialogueRecordById(4), 0);
		CHECK(sink.log.empty());
		in.Dbase100BitmapTestClear(430);
		printf("  action 4 (death): sfx 280 unless flag 430\n");
		// 2, quit: a zero-length record. The one word the do-while loop
		// reads is reported, whatever it is.
		sink.Clear(); in.ResetDialogueState(); in.ClearDiagnostics();
		const roth::DbAction *quit = db.Action(2);
		CHECK(quit != nullptr && quit->bodiless);
		const uint32_t r2 = in.EvalDialogueRecordById(2);
		if (quit)
			printf("  action 2 (quit): zero length; the word read after its header is 0x%08x -> returns %u, sink calls: %s\n",
				(unsigned)db.U32At(quit->firstCommand), (unsigned)r2, sink.log.empty() ? "none" : sink.Joined().c_str());
		CHECK_EQ(in.OutOfImageReads(), 0);
		CHECK_EQ(in.AnomalyTotal(), 0);
	}

	// ---- every action and every trigger block, three ways ----
	// pass 0: a silent world (no text, nothing opens), flags all clear
	// pass 1: a talking world (every line has text, windows open, each
	//         choice and queued tail is played out), flags all clear
	// pass 2: the talking world with every flag set
	static const char *const passName[] = { "silent, flags clear", "talking, flags clear", "talking, flags set" };
	sink.keepLog = false;
	for (int pass = 0; pass < 3; pass++)
	{
		Totals actions, blocks;
		std::map<int, int> blockTriggerRuns;
		sink.anyText = pass > 0;
		sink.loadChunk = pass > 0 ? 0 : 1;
		sink.healthMax = 0x800;
		auto fresh = [&](int salt)
		{
			in.ResetDialogueState();
			in.ClearDiagnostics();
			sink.Clear();
			sink.rng.clear();
			for (int i = 0; i < 16; i++) sink.rng.push_back((salt + i) % 2);
			auto &bm = in.State().g_dbase100_record_bitmap;
			for (auto &b : bm) b = pass == 2 ? 0xff : 0;
		};
		std::string loopers, pastEnd;
		int unexplainedReads = 0;
		for (int id = 1; id <= db.ActionCount(); id++)
		{
			for (int pick = 0; pick < (pass > 0 ? 4 : 1); pick++)
			{
				fresh(id + pick);
				const uint32_t r = in.EvalDialogueRecordById((uint32_t)id);
				in.FinishDialogueRecordEval();
				if (pass > 0) PlayOut(in, sink, pick);
				actions.Add(sink, in, r);
				if (pick != 0) continue;
				if (Anom(in, "chain ran past the step budget") > 0) loopers += F(" %lld", id);
				if (in.OutOfImageReads() > 0)
				{
					// The only read past the file a retail action may make: a
					// zero-length record that is the LAST thing in the file,
					// whose one do-while word lies at EOF (dbase100.c:625-637).
					const roth::DbAction *a = db.Action(id);
					const bool atEof = a != nullptr && a->bodiless && a->firstCommand == db.Size();
					if (atEof && in.OutOfImageReads() == 1) pastEnd += F(" %lld", id);
					else unexplainedReads++;
				}
			}
		}
		for (int id = 1; id <= db.ItemCount(); id++)
		{
			const roth::DbItem *it = db.Item(id);
			if (!it) continue;
			for (const auto &b : it->blocks)
			{
				for (uint32_t flag = 0; flag < 3; flag++)
				{
					fresh(id + (int)flag);
					const uint32_t r = in.EvalBlock(b, flag);
					if (pass > 0) PlayOut(in, sink, (int)flag);
					blocks.Add(sink, in, r);
					blockTriggerRuns[b.trigger]++;
				}
			}
		}
		printf(" pass %d (%s)\n", pass, passName[pass]);
		actions.Print("actions");
		blocks.Print("item trigger blocks (each with caller flags 0, 1, 2)");
		printf("  zero-length actions at EOF, whose one word is past the file:%s\n", pastEnd.empty() ? " none" : pastEnd.c_str());
		printf("  actions stopped by the step budget (ROTH.C would not return):%s\n", loopers.empty() ? " none" : loopers.c_str());
		CHECK_EQ(unexplainedReads, 0);
		CHECK_EQ(blocks.outOfImage, 0);
		CHECK(blocks.anomalies.empty());
		CHECK_EQ(actions.runs, db.ActionCount() * (pass > 0 ? 4 : 1));
		// A retail ACTION may raise only those two, and may meet no word the
		// interpreter does not know other than the two markers.
		for (const auto &kv : actions.anomalies)
			CHECK(kv.first == "chain ran past the step budget" || kv.first == "command word read outside the image");
		for (const auto &kv : actions.unhandled) CHECK(roth::IsDbase100MarkerOpcode((uint8_t)kv.first));
		// With text and windows and a fresh flag bitmap -- the nearest of the
		// three to a real game -- nothing loops. (The silent world loops in
		// the hub menus: no text means no visible choice, the chain falls
		// into the first branch and that jumps back to the menu.)
		if (pass == 1) CHECK(loopers.empty());
		if (pass == 0)
		{
			// Silent, flags clear: the boot / new-game calls must be in there.
			CHECK(actions.calls["PlayRecordGdvCutscene"] >= 3);
			CHECK(actions.calls["LoadDbase300Chunk"] >= 2);
			CHECK(actions.calls["GiveItem"] >= 4);
		}
	}
}

} // namespace

int main(int argc, char **argv)
{
	const char *retail = "/mnt/user-data/uploads/Realms of the Haunting/DATA/DBASE100.DAT";
	if (argc > 1) retail = argv[1];

	TestBitmap();
	TestLoopBasics();
	TestGates();
	TestEffects();
	TestScanAndFinalize();
	TestMusic();
	TestJump();
	TestSay();
	TestChoices();
	TestRandom();
	TestQueue();
	TestEntries();
	TestFuzz();
	const int synthetic = g_checks, syntheticFailed = g_failed;
	printf("synthetic: %d checks, %d failed\n", synthetic, syntheticFailed);

	TestRetail(retail);
	printf("interp_test: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
