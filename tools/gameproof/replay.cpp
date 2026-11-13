//
// replay.cpp -- the PORT's side of the differential test against the original.
//
//   replay "<retail DATA folder>" <oracle.trace> <out.trace> [rawrecs.txt]
//
// Runs the plan gameprobe.c runs inside the original (ROTH.C), on roth::GameCore,
// and writes the same lines. Each action, inventory call and RAW record starts
// from the restored post-new-game state, as the probe's does. The stub answers
// (PROOF_MODE) and the handful of values of other systems the interpreter reads
// are taken from the oracle trace's ENV line, so both sides answer alike.
//
// Everything the harness has to MODEL because it is neither interpreter nor
// inventory cites the ROTH.C lines it models: activate_weapon_item's ammo rule,
// voice_stream_pump's tail, dialogue_voice_force_end, read_next_dialogue_line,
// advance_clamp_8a0f0. If the engine side ever grows those systems, they must
// agree with these lines too.
//
// Engine-free: the C++ standard library and src/roth's engine-free files.
// Reads retail data; writes nothing but <out.trace>, which must not be committed.
//

#include "../../src/roth/roth_gamecore.h"
#include "../../src/roth/roth_itemcmds.h"
#include "../../src/roth/roth_handuse.h"
#include "../../src/roth/roth_sfx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

using namespace roth;

namespace
{

FILE *g_out = nullptr;
int g_depth = 0;

void Ev(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// gameprobe.c cuts a test that logs more than EVENT_LIMIT events (a script that
// never ends); so does this, at the same event.
const int EVENT_LIMIT = 4000;
int g_events = 0;
void Ev(const char *fmt, ...)
{
	if (g_out == nullptr) return;
	// Past the limit nothing more is printed; the port's own step guard
	// (Dbase100Interp::MAX_STEPS) then ends the call, and the test's state is not dumped.
	if (++g_events > EVENT_LIMIT) { if (g_events == EVENT_LIMIT + 1) fprintf(g_out, "RUNAWAY\n"); return; }
	va_list ap; va_start(ap, fmt);
	for (int i = 0; i < g_depth; i++) fputc(' ', g_out);
	vfprintf(g_out, fmt, ap); fputc('\n', g_out);
	va_end(ap);
}

uint32_t Crc32(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
	return ~c;
}

std::vector<uint8_t> ReadFile(const std::string &path)
{
	std::vector<uint8_t> out;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return out;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n > 0) { out.resize((size_t)n); if (fread(out.data(), 1, (size_t)n, f) != (size_t)n) out.clear(); }
	fclose(f);
	return out;
}

// The ENV line of the oracle trace.
struct Env
{
	int mode = 0, actions = 0, items = 0, bitmap = 0, seq = 0, overlay = 0, panel = 0, popup = 0;
	unsigned chunk = 0, musvol = 0, hpmax = 0;
};

// What other systems hold, as far as the two ported systems can change or read it.
struct World
{
	int32_t l83b20 = 0, l80b2c = 0, panel = 0, panelx = 0, hp = 0, death = 0, seq = 0, hud = 0;
	int32_t ammoId = 0, ammo = 0, ammoFlag = 0;
};

Env g_env;
World g_world;
Dbase100 g_db;
std::vector<uint8_t> g_text;        // DBASE400.DAT
GameCore g_core;

// attrs+0x30 of the record's first WeaponAction block: apply_weapon_action_attributes,
// weapon_combat.c:338-420 (op 0x20 stores the 24-bit operand at +0x30; the record must
// carry flag +4 & 0x10, :349; only block code 5 is parsed, :355).
uint32_t WeaponAttr30(int32_t idx)
{
	if (idx <= 0) return 0;                                              // :344
	const DbItem *rec = g_db.Item(idx);
	if (rec == nullptr || !rec->IsWeapon()) return 0;                    // :346-349
	const DbTriggerBlock *b = rec->FindBlock(5);
	if (b == nullptr) return 0;
	uint32_t v = 0;
	for (const DbCommand &c : g_db.Commands(*b)) if ((c.opcode & 0x7f) == 0x20) v = c.operand;
	return v;
}

struct Observer : GameCoreObserver
{
	void Enter(const char *what, int32_t a, int32_t b) override
	{
		if (!strcmp(what, "give")) Ev("give id=%d ctx=%d {", (int)(int16_t)a, b);
		else if (!strcmp(what, "remove")) Ev("remove id=%d {", a);
		else if (!strcmp(what, "query")) Ev("query id=%d flags=%u {", a, (unsigned)b);
		else if (!strcmp(what, "find")) Ev("find n=%u flags=%u {", (unsigned)a, (unsigned)b);
		else if (!strcmp(what, "filter")) Ev("filter mode=%u {", (unsigned)a);
		else if (!strcmp(what, "evalblock")) Ev("evalblock off=%u mode=%u {", (unsigned)a, (unsigned)b);
		else if (!strcmp(what, "rng")) { mCount = a; return; }
		g_depth++;
	}
	void Leave(const char *what, int32_t ret) override
	{
		if (!strcmp(what, "rng")) { Ev("rng count=%d ret=%d", mCount, ret); return; }
		g_depth--;
		if (!strcmp(what, "give")) Ev("} give ret=%d", ret);
		else if (!strcmp(what, "remove")) Ev("} remove ret=%d", ret);
		else if (!strcmp(what, "query")) Ev("} query ret=%u", (unsigned)ret);
		else if (!strcmp(what, "find")) Ev("} find ret=%u", (unsigned)ret);
		else if (!strcmp(what, "filter")) Ev("} filter");
		else if (!strcmp(what, "evalblock")) Ev("} evalblock ret=%d", ret);
	}
	int32_t mCount = 0;
};

struct InvHost : InventorySink
{
	// activate_weapon_item, weapon_combat.c:426-459, as far as it touches what the
	// inventory rules read: the ammo id (0x811e4 = attrs+0x30), the ammo count
	// (0x83e6c) and the equip counter (0x810c4).
	void Activate(int slot, int32_t id)
	{
		Ev("weapon slot=%d id=%d {", slot, id); g_depth++;
		const int32_t idx = slot < 0 ? g_core.Items().AdamItemId() : id;             // :430
		const uint32_t a30 = WeaponAttr30(idx);
		g_world.ammoId = (int32_t)a30;
		if (a30 & 0x8000) g_world.ammo = 0;                                           // :443-445
		else if (g_world.ammoId != 0) g_world.ammo = g_core.Query((uint16_t)(int16_t)g_world.ammoId, 0);   // :448-452
		else g_world.ammo = 0;                                                        // :454
		g_world.hud++;                                                                // :458
		g_depth--; Ev("} weapon");
	}
	void UnequipWeapon() override { Activate(-1, 0); }
	void EquipWeapon(int slot, int32_t itemId) override { Activate(slot, itemId); }
	void WeaponHudDirty() override { g_world.hud++; }
	void RebuildWeaponList() override { Ev("weaponlist"); }
	// reset_weapon_hud, weapon_combat.c:922-935: activate_weapon_item(0, 0), then a redraw.
	void ResetWeaponHud() override { Ev("weaponhud {"); g_depth++; Activate(-1, 0); g_depth--; Ev("} weaponhud"); }
	bool IsActiveAmmo(int32_t itemId) override { return itemId == g_world.ammoId; }
	void AmmoDisplayChanged(int delta) override { g_world.ammoFlag = 1; g_world.ammo += delta; }
	void RightHandChanged(int) override { Ev("icon"); }
	void ClearTabCursors() override { Ev("tabclear"); }
	void RefreshPanel() override { Ev("panelrefresh"); }
	bool PanelCloseRequested() override { return g_world.l80b2c != 0; }
	void PickupFlyStarted(int32_t x, int32_t y, int32_t z, uint16_t renderId) override
	{ Ev("pickupfly x=%d y=%d z=%d id=%u", x, y, z, renderId); }
};

struct InterpHost : Dbase100Sink
{
	// read_next_dialogue_line, dbase100.c:371-403, minus the timed-message cache
	// (it returns what the read returns) and with the voice clip only logged.
	uint32_t ReadNextDialogueLine(uint8_t *dest, uint32_t maxlen, uint32_t off, uint32_t flag) override
	{
		uint32_t n = 0;
		if ((uint64_t)off + 8 <= g_text.size())
		{
			const uint8_t *h = g_text.data() + off;
			const uint32_t clip = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
			n = (uint32_t)h[4] | ((uint32_t)h[5] << 8);                                  // :383
			if (n > maxlen) n = maxlen;                                                  // :388
			const uint64_t avail = g_text.size() - ((uint64_t)off + 8);
			if (n > avail) n = (uint32_t)avail;
			memcpy(dest, h + 8, n);
			if (flag != 0 && clip != 0) { g_depth++; Ev("voice clip=%u", clip); g_depth--; }   // :392-394
		}
		Ev("line off=%u max=%u flag=%u ret=%u", off, maxlen, flag, n);
		return n;
	}
	uint32_t Dbase100OpenDialogueWindow(const uint8_t *t, uint32_t bytes) override
	{
		const uint32_t n = (uint32_t)strnlen((const char *)t, bytes), r = (g_env.mode & 1) ? 0 : 1;
		Ev("window len=%u crc=%08x ret=%u", n, Crc32(t, n), r); return r;
	}
	uint32_t Dbase100OpenDialogueWindowAlt(const uint8_t *t, uint32_t bytes) override
	{
		const uint32_t n = (uint32_t)strnlen((const char *)t, bytes), r = (g_env.mode & 1) ? 0 : 1;
		Ev("windowalt len=%u crc=%08x ret=%u", n, Crc32(t, n), r); return r;
	}
	// dialogue_voice_force_end, audio.c:1335-1350.
	void DialogueVoiceForceEnd(uint32_t a, uint32_t d) override
	{
		Ev("forceend %u %u", a, d);
		Dbase100InterpState &s = g_core.Interp().State();
		if (s.g_voice_stream_state == 1)                                 // :1337-1342
		{
			s.g_voice_stream_state = 2;
			if (s.g_move_freeze_gate < 0x6ffff) s.g_active_dialogue_context = 0;
			s.g_move_freeze_gate = 0;
			return;
		}
		if (s.g_move_freeze_gate == 0x6ffff) { Ev("UNMODELLED: choice highlight (audio.c:1343-1347)"); return; }
		s.g_move_freeze_gate = 0;                                        // :1349
	}
	uint32_t PlayRecordGdvCutscene(int index) override { Ev("gdv index=%d", index); return 0; }
	uint32_t ShowFullscreenImage(uint32_t op) override { Ev("image op=%u", op); return 0; }
	void ExitCutsceneOverlayMode() override { Ev("exitoverlay"); }
	uint32_t PlaySoundEffect(uint32_t id, uint32_t param) override { Ev("sfx id=%u param=%u", id, param); return 0; }
	uint32_t LoadDbase300Chunk(uint32_t index) override
	{ const uint32_t r = (g_env.mode & 2) ? 0 : 1; Ev("db300 index=%u ret=%u", index, r); return r; }
	uint32_t FinalizeAudioSequenceRef() override { Ev("seqfinal"); return 0; }
	uint32_t EmitMusicSequenceEvent(uint32_t e) override { Ev("seqevent %u", e); return 0; }
	void ProcessAudioSequenceChunk() override { Ev("seqprocess"); }
	int32_t ResetCommandChainNoSource(uint16_t i) override { Ev("resetchain %u", i); return 0; }
	int32_t RunObjectCommandsById(uint16_t i) override { Ev("runobj %u", i); return 0; }
	// advance_clamp_8a0f0, renderer.c:9818-9824.
	void AdvanceClamp8a0f0(uint32_t amount, uint32_t max) override
	{
		Ev("health amount=%u max=%u", amount, max);
		uint32_t v = (uint32_t)g_world.hp + amount;
		if (v >= max) v = max;
		g_world.hp = (int32_t)v;
	}
	int32_t AudioSeqState() override { return g_world.seq; }
	void SetAudioSeqState(int32_t v) override { g_world.seq = v; }
	uint32_t CurrentDbase300ChunkId() override { return g_env.chunk; }
	uint8_t MusicMasterVolume() override { return (uint8_t)g_env.musvol; }
	int32_t CutsceneOverlayActive() override { return g_env.overlay; }
	int32_t InventoryPanelOpen() override { return g_world.panel; }
	int32_t InspectPopupActive() override { return g_env.popup; }
	void ArmInventoryPanelExit() override { g_world.panel = 2; g_world.panelx = 1; }
	void SetLatch83b20() override { g_world.l83b20 = 1; }
	void SetLatch80b2c() override { g_world.l80b2c = 1; }
	void ZeroHealthAndFlagDeath() override { g_world.hp = 0; g_world.death = 1; }
	uint32_t HealthClampMax() override { return g_env.hpmax; }
};

Observer g_observer;
InvHost g_invHost;
InterpHost g_interpHost;

// ---- snapshot ---------------------------------------------------------------

struct Snapshot
{
	Dbase100InterpState interp;
	Inventory items;
	RothIndexRng rng;
	World world;
	std::vector<uint8_t> image;
};
Snapshot g_s0, g_base[8];
bool g_baseMade[8] = {};
const Snapshot *g_cur = &g_s0;

void Take(Snapshot &s)
{
	s.interp = g_core.Interp().State(); s.items = g_core.Items(); s.rng = g_core.Rng(); s.world = g_world;
	s.image.assign(g_db.Image(), g_db.Image() + g_db.Size());
}
void Restore(const Snapshot &s)
{
	g_core.Interp().State() = s.interp; g_core.Items() = s.items; g_core.Rng() = s.rng; g_world = s.world;
	memcpy(g_db.MutableImage(), s.image.data(), s.image.size());
}

void SetupInventory(int k);

// gameprobe.c begin(): starting state `b`, bits 0-1 = inventory setup, bit 2 = every flag set.
void Begin(int b)
{
	g_depth = 0; g_events = 0;
	if (!g_baseMade[b])
	{
		Restore(g_s0); g_cur = &g_s0;
		SetupInventory(b & 3);
		if (b & 4) for (uint8_t &x : g_core.Interp().State().g_dbase100_record_bitmap) x = 0xff;
		if (g_env.mode & 4) g_world.seq = 2;
		Take(g_base[b]); g_baseMade[b] = true;
	}
	g_cur = &g_base[b]; Restore(*g_cur); g_events = 0;
}

void DumpState(const char *tag, bool full)
{
	const Dbase100InterpState &s = g_core.Interp().State();
	const Inventory &inv = g_core.Items();
	fprintf(g_out, "%s flags", tag);
	for (size_t i = 0; i < s.g_dbase100_record_bitmap.size() * 8; i++)
	{
		const int now = (s.g_dbase100_record_bitmap[i >> 3] >> (i & 7)) & 1;
		const int was = full ? 0 : (g_cur->interp.g_dbase100_record_bitmap[i >> 3] >> (i & 7)) & 1;
		if (now != was) fprintf(g_out, " %c%u", now ? '+' : '-', (unsigned)i);
	}
	fprintf(g_out, "\n%s inv count=%d left=%d right=%d adam=%d/%d auto=%u easy=%u last=%d pickup=%u/%d slide=%d slots", tag,
		inv.Count(), inv.LeftHand(), inv.RightHand(), inv.AdamSlot(), (int)inv.AdamItemId(), inv.AutoSelected() ? 1u : 0u,
		inv.EasySelect() ? 1u : 0u, (int)inv.LastItem(), inv.PickupLockFlags(), (int)inv.PickupLockTimer(), 0);
	for (int i = 0; i < Inventory::SLOT_COUNT; i++)
	{
		const InventorySlot &now = inv.Slot(i), &was = g_cur->items.Slot(i);
		if (full ? (now.id || now.quantity) : (now.id != was.id || now.quantity != was.quantity))
			fprintf(g_out, " %d:%04x:%u", i, now.id, now.quantity);
	}
	fprintf(g_out, "\n%s dlg active=%d qcount=%d", tag, s.g_dbase100_queue_active, s.g_dialogue_action_queue_count);
	for (int i = 0; i < 8 && i < s.g_dialogue_action_queue_count; i++)
		fprintf(g_out, " q%d=%u/%d/%u", i, s.g_dialogue_action_queue[i].chain, s.g_dialogue_action_queue[i].count, s.g_dialogue_action_queue[i].flags);
	fprintf(g_out, " ch=%u/%d/%u/%d topic=%u idx", s.g_dbase100_choice_cursor, s.g_dbase100_choice_remaining, s.g_dbase100_choice_mode,
		s.g_dbase100_choice_count, s.g_dbase100_pending_topic);
	{ int n = s.g_dbase100_choice_cursor ? s.g_dbase100_choice_count : 0; if (n > 16) n = 16;
	  for (int i = 0; i < n; i++) fprintf(g_out, " %d", s.g_dbase100_choice_record_indices[i]); }
	fprintf(g_out, " drain=%d busy=%d ctx=%d freeze=%d voice=%d sel=%d valred=%d\n",
		s.g_dbase100_choice_record_indices[Dbase100InterpState::QUEUE_DRAINING], s.g_dialogue_busy_flag, s.g_active_dialogue_context,
		s.g_move_freeze_gate, s.g_voice_stream_state, s.g_choice_selected_index, s.g_value_reduction_factor);
	const RothIndexRng &r = g_core.Rng();
	fprintf(g_out, "%s rng seed=%04x rng=%04x cd=%d\n", tag, r.seed, r.rng, r.countdown);
	fprintf(g_out, "%s ext l83b20=%d l80b2c=%d panel=%d/%d hp=%d death=%d seq=%d hud=%d ammo=%d/%d/%d img", tag,
		g_world.l83b20, g_world.l80b2c, g_world.panel, g_world.panelx, g_world.hp, g_world.death, g_world.seq,
		g_world.hud - g_cur->world.hud, g_world.ammoId, g_world.ammo, g_world.ammoFlag);
	{ int shown = 0;
	  for (size_t i = 0; i < g_db.Size() && shown < 24; i++)
		if (g_db.Image()[i] != g_cur->image[i]) { fprintf(g_out, " %u:%02x", (unsigned)i, g_db.Image()[i]); shown++; } }
	fputc('\n', g_out);
}

// ---- the drive: gameprobe.c drive() ---------------------------------------------

void Drive(int pick)
{
	Dbase100Interp &in = g_core.Interp();
	Dbase100InterpState &s = in.State();
	for (int step = 0; step < 200 && g_events <= EVENT_LIMIT; step++)
	{
		if (s.g_dbase100_choice_cursor != 0)
		{
			const int n = s.g_dbase100_choice_count, k = pick < n ? pick : 0; pick = 0;
			Ev("pick %d of %d {", k, n); g_depth++;
			s.g_choice_saved_index = k;
			in.CloseDialogueAndRunBranch();
			g_depth--; Ev("}");
		}
		else if (s.g_dialogue_busy_flag != 0 || s.g_dialogue_action_queue_count != 0)
		{
			Ev("pump {"); g_depth++;
			// voice_stream_pump's tail, audio.c:1316-1326. No voice file is open here
			// (the probe stubs prime_voice_clip), so the refill half (:1301-1315) is idle.
			if (s.g_voice_stream_state == 2) { s.g_voice_stream_state = 0; s.g_move_freeze_gate = 0; }
			bool held = false;
			if (s.g_dialogue_busy_flag != 0)
			{
				if (s.g_voice_stream_state != 0 || s.g_active_dialogue_context != 0) held = true;
				else s.g_dialogue_busy_flag = 0;
			}
			if (!held) in.AdvanceDialogueActionQueue();
			g_depth--; Ev("}");
		}
		else return;
	}
	Ev("DRIVE-LIMIT");
}

// ---- section A ---------------------------------------------------------------------

const int kVarBase[4] = { 0, 4, 2, 7 };

void SectionActions(int lo, int hi)
{
	for (int id = lo; id <= hi; id++) for (int var = 0; var < 4; var++)
	{
		int variants = 1;
		for (int v = 0; v < variants; v++)
		{
			Begin(kVarBase[var]);
			fprintf(g_out, "A %d var=%d pick=%d\n", id, var, v);
			const uint32_t r = g_core.Interp().EvalDialogueRecordById((uint32_t)id);
			Ev("= %d", (int)r);
			const Dbase100InterpState &s = g_core.Interp().State();
			if (v == 0 && g_events <= EVENT_LIMIT && s.g_dbase100_choice_cursor != 0) { variants = s.g_dbase100_choice_count; if (variants > 16) variants = 16; }
			g_core.Interp().FinishDialogueRecordEval();
			Drive(v);
			if (g_events <= EVENT_LIMIT) DumpState("S", false);
		}
	}
}

// ---- section B ----------------------------------------------------------------------

void SectionBlocks()
{
	const int n = (int)g_db.Header().inventoryCount;
	for (int id = 1; id <= n; id++)
	{
		const DbItem *it = g_db.ItemSlot(id);
		if (it == nullptr || it->offset == 0) continue;
		// Walked as the probe walks them: from record + 0x14, by each header's own size.
		for (uint32_t blk = it->offset + 0x14; ; )
		{
			const uint32_t hdr = g_db.U32At(blk), size = hdr & 0xffffff;
			if (size < 4) break;
			for (int mode = 0; mode < 3; mode++) for (int b = 0; b < 8; b += 7)
			{
				Begin(b);
				fprintf(g_out, "B item=%d block=%u trigger=%u mode=%d base=%d\n", id, blk, hdr >> 24, mode, b);
				DbTriggerBlock tb; tb.offset = blk; tb.size = size; tb.trigger = (uint8_t)(hdr >> 24);
				tb.firstCommand = blk + 4; tb.commandCount = (int)((size - 4) / 4);
				Ev("evalblock off=%u mode=%u {", blk, (unsigned)mode); g_depth++;
				const uint32_t r = g_core.Interp().EvalBlock(tb, (uint32_t)mode);
				g_depth--; Ev("} evalblock ret=%d", (int)r);
				Ev("= %d", (int)r);
				Drive(0);
				if (g_events <= EVENT_LIMIT) DumpState("S", false);
			}
			blk += (size >> 2) << 2;
		}
	}
}

// ---- section I -----------------------------------------------------------------------

void SetupInventory(int k)
{
	FILE *keep = g_out; g_out = nullptr;
	const int n = g_db.Header().inventoryCount;
	if (k == 1) { for (int id = 1; id <= n; id += 3) g_core.Give(id); }
	if (k == 2) { for (int id = n; id >= 1; id -= 2) g_core.Give(id); for (int id = 2; id <= n; id += 5) g_core.Give(id); }
	if (k == 3) { for (int rep = 0; rep < 4 && g_core.Items().Count() < 0x100; rep++) for (int id = 1; id <= n && g_core.Items().Count() < 0x100; id++) g_core.Give(id); }
	g_out = keep;
}

void SectionInventory()
{
	const int n = (int)g_db.Header().inventoryCount;
	static const char *const opname[] = { "give", "query0", "query1", "query2", "query3", "remove", "find0", "find1", "give+remove", "filter0", "filter1" };
	for (int k = 0; k < 4; k++)
	{
		Begin(k);
		fprintf(g_out, "I setup=%d\n", k); DumpState("S", true);
		for (int id = 0; id <= n + 1; id++) for (int op = 0; op < 11; op++)
		{
			if (op >= 9 && id > 1) continue;
			Begin(k);
			fprintf(g_out, "I setup=%d id=%d op=%s\n", k, id, opname[op]);
			const int32_t list[3] = { id, id + 1, id + 7 };
			switch (op)
			{
			case 0: if (id == 0) { Ev("skipped: give 0 reads its context"); break; }
				Ev("= %d", (int)g_core.Give(id)); break;
			case 1: case 2: case 3: case 4: Ev("= %d", g_core.Query((uint16_t)id, op - 1)); break;
			case 5: Ev("= %d", g_core.Remove((uint16_t)id)); break;
			case 6: case 7: Ev("= %d", g_core.FindOrAutoselect(list, 3, op - 6)); break;
			case 8: if (id == 0) { Ev("skipped: give 0 reads its context"); break; }
				Ev("= %d", (int)g_core.Give(id));
				Ev("= %d", (int)g_core.Give(id));
				Ev("= %d", g_core.Remove((uint16_t)id));
				Ev("= %d", g_core.Query((uint16_t)id, 1));
				Ev("= %d", g_core.Remove((uint16_t)id));
				Ev("= %d", g_core.Remove((uint16_t)id)); break;
			case 9: case 10:
				g_core.SetFilter((op - 9) ^ id);
				Ev("= %d", g_core.Query(1, 1));
				g_core.SetFilter(1); break;
			}
			if (g_events <= EVENT_LIMIT) DumpState("S", false);
		}
	}
}

// ---- section Q ----------------------------------------------------------------------

void SectionQueue(int lo, int hi)
{
	const int n = g_db.ActionCount();
	for (int id = lo; id <= hi; id++) for (int b = 0; b < 8; b += 7)
	{
		Begin(b);
		ItemCmdState st;
		fprintf(g_out, "Q %d base=%d\n", id, b);
		for (int j = 0; j < 3; j++)
		{
			ItemCmdRecord rec; rec.fireFlags = (uint8_t)(j == 1 ? 0x10 : 0); rec.key = (uint16_t)((id - 1 + j) % n + 1);
			const int32_t r = RunCommandDbase100Record(g_core, st, rec);
			Ev("= %d mod=%02x latch=%u", (int)r, rec.modifier, st.dialogueLatch ? 1u : 0u);
			if (j == 0)             // the chain tail, raw_commands.c:1313-1315
			{
				g_core.Interp().FinishDialogueRecordEval(); st.dialogueLatch = false; g_core.Items().ClearAutoSelected();
				Ev("chain end");
			}
		}
		g_core.Interp().FinishDialogueRecordEval();
		Drive(0);
		if (g_events <= EVENT_LIMIT) DumpState("S", false);
	}
}

// ---- section R -------------------------------------------------------------------------

struct PendingHost : ItemCmdHost
{
	ItemCmdRecord pend;
	ItemCmdRecord *PendingRecord(uint32_t handle) override { return handle == 1 ? &pend : nullptr; }
};

void SectionRaw(const char *path)
{
	FILE *f = path ? fopen(path, "r") : nullptr;
	if (!f) { fprintf(g_out, "R none\n"); return; }
	char line[256];
	while (fgets(line, sizeof line, f))
	{
		unsigned op, mod, f6, key, pendkey, n;
		if (sscanf(line, "%x %x %x %u %u %u", &op, &mod, &f6, &key, &pendkey, &n) != 6) continue;
		for (int withctx = 0; withctx < (op == 0x29 ? 2 : 1); withctx++)
		for (int k = 0; k < 4; k++)
		{
			if (op == 0x29 && key == 0 && !withctx) continue;
			Begin(k);
			ItemCmdRecord rec; rec.modifier = (uint8_t)mod; rec.fireFlags = (uint8_t)f6; rec.key = (uint16_t)key;
			PendingHost host; host.pend.modifier = 0; host.pend.fireFlags = 0x10; host.pend.key = (uint16_t)pendkey;
			ItemCmdState st;
			st.pendingRecord = pendkey ? 1 : 0;
			GiveContext ctx; ctx.x = 100; ctx.y = -200; ctx.z = 64; ctx.flags7 = 0;
			ctx.id = g_db.ItemSlot(15) != nullptr ? g_db.ItemSlot(15)->renderId : 0;
			if (op == 0x27 && (f6 & 8) && (f6 & 4)) { st.list[0] = (int32_t)key; st.list[1] = (int32_t)key + 1; st.listCount = 2; }
			fprintf(g_out, "R op=%02x mod=%02x f6=%02x key=%u pend=%u ctx=%d setup=%d\n", op, mod, f6, key, pendkey, withctx, k);
			int32_t r = 0;
			switch (op)
			{
			case 0x27: r = CmdIfNotItem(g_core, st, rec, host); break;
			case 0x29: r = CmdGiveItem(g_core, st, rec, withctx ? &ctx : nullptr); break;
			case 0x2a: r = CmdRemoveItem(g_core, st, rec); break;
			case 0x42: r = CmdSetInventoryFilter(g_core, rec); break;
			case 0x2b: r = RunCommandDbase100Record(g_core, st, rec); break;
			}
			Ev("= %d mod=%02x pendmod=%02x pending=%d interrupt=%u latch=%u listn=%d", r == 0 ? 0 : (r == -1 ? -1 : 1), rec.modifier,
				host.pend.modifier, st.pendingRecord != 0 ? 1 : 0, (unsigned)st.interrupt, st.dialogueLatch ? 1u : 0u, st.listCount);
			g_core.Interp().FinishDialogueRecordEval();
			Drive(0);
			if (g_events <= EVENT_LIMIT) DumpState("S", false);
		}
	}
	fclose(f);
}

// ---- section H -------------------------------------------------------------------------
//
// gameprobe.c section_hands. The original is run with the right hand as the VR hand leaves it
// (r0 for the item hand, nothing for the other); here every test starts from r0 and the VR scope
// (roth_handuse.h HandUseBegin) makes the difference, so equal traces mean the other hand's use IS
// the original's use with nothing selected. HandUseEnd (the restore) runs after the state is
// printed; it has no counterpart in the original, so it is checked here against its own rule and
// counted on stderr.

int g_handFail = 0;
int g_handAfter[5] = {};

int FirstSlotWith(uint16_t id)
{
	if (id == 0) return -1;
	for (int i = 0; i < Inventory::SLOT_COUNT; i++) if (g_core.Items().Slot(i).id == id) return i;
	return -1;
}

void SectionHands(const char *path)
{
	FILE *f = path ? fopen(path, "r") : nullptr;
	if (!f) { fprintf(g_out, "H none\n"); return; }
	static const char *const r0name[3] = { "own", "item", "none" };
	char line[256];
	while (fgets(line, sizeof line, f))
	{
		unsigned op, mod, f6, key, pendkey, n;
		if (sscanf(line, "%x %x %x %u %u %u", &op, &mod, &f6, &key, &pendkey, &n) != 6 || op != 0x27) continue;
		for (int k = 0; k < 4; k++) for (int easy = 0; easy < 2; easy++)
		for (int rk = 0; rk < 3; rk++) for (int hand = 0; hand < 2; hand++)
		{
			Begin(k);
			Inventory &inv = g_core.Items();
			const int r0 = rk == 0 ? inv.RightHand() : rk == 1 ? FirstSlotWith((uint16_t)key) : -1;
			if (rk == 1 && r0 < 0) continue;
			inv.SetEasySelect(easy != 0);
			inv.SetRightHand(r0);                                  // the probe writes 0x81044 directly: no icon call
			const HandUse scope = HandUseBegin(inv, hand == 0);
			ItemCmdRecord rec; rec.modifier = (uint8_t)mod; rec.fireFlags = (uint8_t)f6; rec.key = (uint16_t)key;
			ItemCmdRecord rem; rem.key = (uint16_t)key;
			PendingHost host; host.pend.modifier = 0; host.pend.fireFlags = 0x10; host.pend.key = (uint16_t)pendkey;
			ItemCmdState st;
			st.pendingRecord = pendkey ? 1 : 0;
			if ((f6 & 8) && (f6 & 4)) { st.list[0] = (int32_t)key; st.list[1] = (int32_t)key + 1; st.listCount = 2; }
			fprintf(g_out, "H op=27 mod=%02x f6=%02x key=%u pend=%u setup=%d easy=%d r0=%s:%d hand=%s\n", mod, f6, key, pendkey,
				k, easy, r0name[rk], r0, hand == 0 ? "item" : "other");
			const int32_t r = CmdIfNotItem(g_core, st, rec, host);
			Ev("= %d mod=%02x pendmod=%02x pending=%d interrupt=%u latch=%u listn=%d right=%d auto=%u", r == 0 ? 0 : (r == -1 ? -1 : 1),
				rec.modifier, host.pend.modifier, st.pendingRecord != 0 ? 1 : 0, (unsigned)st.interrupt, st.dialogueLatch ? 1u : 0u,
				st.listCount, inv.RightHand(), inv.AutoSelected() ? 1u : 0u);
			const int32_t r2 = CmdRemoveItem(g_core, st, rem);
			Ev("remove = %d right=%d", r2 == 0 ? 0 : (r2 == -1 ? -1 : 1), inv.RightHand());
			g_core.Interp().FinishDialogueRecordEval();
			Drive(0);
			if (g_events <= EVENT_LIMIT) DumpState("S", false);

			// THE RESTORE, by its own rule (roth_handuse.h).
			const int during = inv.RightHand();
			const uint16_t idThen = r0 >= 0 ? inv.Slot(r0).id : 0;
			const HandUseAfter after = HandUseEnd(inv, scope);
			g_handAfter[(int)after]++;
			bool ok;
			switch (after)
			{
			case HandUseAfter::ItemHand:     ok = hand == 0 && inv.RightHand() == during; break;
			case HandUseAfter::NothingHeld:  ok = hand == 1 && r0 < 0 && inv.RightHand() == during; break;
			case HandUseAfter::Restored:     ok = hand == 1 && during < 0 && inv.RightHand() == r0 && idThen == scope.savedId; break;
			case HandUseAfter::GameSelected: ok = hand == 1 && r0 >= 0 && during >= 0 && inv.RightHand() == during; break;
			case HandUseAfter::ItemGone:     ok = hand == 1 && during < 0 && inv.RightHand() < 0 && idThen != scope.savedId; break;
			default: ok = false;
			}
			if (!ok && g_handFail++ < 10)
				fprintf(stderr, "replay H: restore rule broken: key %u setup %d r0 %d hand %d during %d now %d (%s)\n",
					key, k, r0, hand, during, inv.RightHand(), HandUseAfterName(after));
		}
	}
	fclose(f);
	fprintf(stderr, "replay H: after the use -- item hand %d, nothing held %d, restored %d, the use selected %d, item gone %d;"
		" restore rule broken %d\n", g_handAfter[0], g_handAfter[1], g_handAfter[2], g_handAfter[3], g_handAfter[4], g_handFail);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: replay <retail DATA folder> <oracle.trace> <out.trace> [rawrecs.txt]\n"); return 2; }
	const std::string dir = argv[1];
	std::vector<uint8_t> file = ReadFile(dir + "/DBASE100.DAT");
	g_text = ReadFile(dir + "/DBASE400.DAT");
	if (file.empty() || g_text.empty() || !g_db.Load(file.data(), file.size())) { fprintf(stderr, "replay: no DBASE100/DBASE400 in %s\n", dir.c_str()); return 1; }

	// The plan and the stub answers come from the oracle trace.
	char sections[16] = "ABQIRH"; int lo = 1, hi = 0;
	std::vector<std::pair<uint32_t, uint32_t>> patches; bool patchUnknown = false;
	{
		FILE *o = fopen(argv[2], "r");
		if (!o) { fprintf(stderr, "replay: cannot read %s\n", argv[2]); return 1; }
		char line[4096]; bool got = false;
		while (fgets(line, sizeof line, o))
			if (sscanf(line, "ENV mode=%d actions=%d items=%d bitmap=%d seq=%d chunk=%u musvol=%u overlay=%d panel=%d popup=%d hpmax=%u plan=%15s %d %d",
				&g_env.mode, &g_env.actions, &g_env.items, &g_env.bitmap, &g_env.seq, &g_env.chunk, &g_env.musvol, &g_env.overlay,
				&g_env.panel, &g_env.popup, &g_env.hpmax, sections, &lo, &hi) == 14) { got = true; break; }
		// The loader's rewrite of the DBASE100 image (gameprobe.c, PATCH): the original's
		// resolve_dbase100_sound_ids, audio.c:1898-1924. Applied here as the original left it.
		unsigned off, val;
		while (fgets(line, sizeof line, o))
		{
			if (sscanf(line, "PATCH %u %x", &off, &val) == 2) patches.push_back(std::make_pair(off, val));
			else if (!strncmp(line, "PATCH-UNKNOWN", 13)) patchUnknown = true;
			else break;
		}
		fclose(o);
		if (!got) { fprintf(stderr, "replay: no ENV line in %s\n", argv[2]); return 1; }
	}
	g_out = fopen(argv[3], "w");
	if (!g_out) { fprintf(stderr, "replay: cannot write %s\n", argv[3]); return 1; }

	fprintf(g_out, "BOOT\n");
	g_core.Attach(&g_db, &g_interpHost, &g_invHost);
	g_core.SetObserver(&g_observer);
	g_world.seq = g_env.seq; g_world.panel = g_env.panel;
	// The boot hook, game_core.c:755-758, then the new-game block, :821-833.
	g_core.RunAction(GameCore::ACTION_BOOT);
	g_core.NewGame();
	g_world.hp = (int32_t)g_env.hpmax;                                   // game_core.c:828-830
	g_world.l83b20 = 0;                                                  // game_core.c:833
	fprintf(g_out, "ENV mode=%d actions=%d items=%d bitmap=%d seq=%d chunk=%u musvol=%u overlay=%d panel=%d popup=%d hpmax=%u plan=%s %d %d\n",
		g_env.mode, g_db.ActionCount(), g_db.ItemCount(), (int)g_core.Interp().State().g_dbase100_record_bitmap.size(), g_world.seq,
		g_env.chunk, g_env.musvol, g_env.overlay, g_world.panel, g_env.popup, g_env.hpmax, sections, lo, hi);
	// The loader's rewrite of the image, resolve_dbase100_sound_ids (audio.c:1898-1924), done the
	// way the engine does it: the sound lane's SfxBank::ResolveDbase100 over DATA/FXSCRIPT.SFX.
	// The rewritten words are printed like the probe's PATCH lines, so S0 diffs them against
	// what the original's loader wrote. Without the bank the oracle's words are applied as given.
	{
		std::vector<uint8_t> before(g_db.Image(), g_db.Image() + g_db.Size());
		SfxBank bank;
		const bool own = bank.Open(dir + "/DATA/FXSCRIPT.SFX") || bank.Open(dir + "/FXSCRIPT.SFX");
		if (own) bank.ResolveDbase100(g_db.MutableImage(), g_db.Size());
		else
			for (const auto &pt : patches)
				if ((size_t)pt.first + 4 <= g_db.Size())
					for (int i = 0; i < 4; i++) g_db.MutableImage()[pt.first + i] = (uint8_t)(pt.second >> (8 * i));
		for (size_t off = 0; off + 4 <= g_db.Size(); off += 4)
			if (memcmp(before.data() + off, g_db.Image() + off, 4) != 0)
				fprintf(g_out, "PATCH %u %08x\n", (unsigned)off, g_db.U32At((uint32_t)off));
		if (!own) fprintf(stderr, "replay: no FXSCRIPT.SFX; the sound-id rewrite is taken from the oracle, not checked\n");
	}
	if (patchUnknown) fprintf(g_out, "PATCH-UNKNOWN (DBASE100.DAT not readable from the game folder)\n");
	Take(g_s0);
	DumpState("S0", true);
	if (strchr(sections, 'A')) SectionActions(lo, hi);
	if (strchr(sections, 'B')) SectionBlocks();
	if (strchr(sections, 'I')) SectionInventory();
	if (strchr(sections, 'Q')) SectionQueue(lo, hi);
	if (strchr(sections, 'R')) SectionRaw(argc > 4 ? argv[4] : nullptr);
	if (strchr(sections, 'H')) SectionHands(argc > 4 ? argv[4] : nullptr);
	fprintf(g_out, "END\n");
	fclose(g_out);
	return g_handFail != 0 ? 3 : 0;
}
