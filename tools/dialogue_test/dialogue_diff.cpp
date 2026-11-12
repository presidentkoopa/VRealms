//
// dialogue_diff -- run a probe script through THE PORT and print what the
// original prints.
//
//   dialogue_diff <retail DATA folder> <oracle log | -> <script> > port.log
//
// The oracle log is what tools/dialogue_test/dlgprobe.c wrote when the same
// script was run against the ORIGINAL engine (ROTH.C). Its first state block
// seeds the port (flags, inventory, hands), and its ENV line gives the screen
// width and the random generator's words. After that, every command of the
// script is executed against roth::GameCore + roth::DialogueUi +
// roth::InventoryScreen, and the state is printed after each in the probe's
// own format. `diff` of the two logs is the test (run_diff.sh does it and
// counts).
//
// Engine-free: this is the same code the engine links.
//

#include "../../src/roth/roth_gamecore.h"
#include "../../src/roth/roth_dialogue.h"
#include "../../src/roth/roth_invscreen.h"
#include "../../src/roth/roth_icons.h"

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

int g_screenH = 480;

struct Host : DialogueUi, InventorySink
{
	GameCore *core = nullptr;
	InventoryScreen *screen = nullptr;
	FILE *speech = nullptr;
	int voiceMode = 0;
	bool mute = false;      // while the test seeds itself from a new game
	// Globals of systems this test does not have, as plain numbers.
	int32_t l83b20 = 0, health = 0, dead = 0, overlay = 0;
	uint32_t healthMax = 0;

	// ---- interpreter sink: logged exactly as the probe's overrides log ----
	int32_t RunObjectCommandsById(uint16_t id) override { if (!mute) printf("C run_object_commands_by_id %u\n", id); return 0; }
	int32_t ResetCommandChainNoSource(uint16_t idx) override { if (!mute) printf("C reset_command_chain_no_source %u\n", idx); return 0; }
	uint32_t PlaySoundEffect(uint32_t id, uint32_t p) override { if (!mute) printf("C play_sound_effect %u %u\n", id, p); return 0; }
	uint32_t LoadDbase300Chunk(uint32_t idx) override { if (!mute) printf("C load_dbase300_chunk %u\n", idx); return 1; }
	uint32_t ShowFullscreenImage(uint32_t op) override { if (!mute) printf("C show_fullscreen_image %u\n", op); return 0; }
	uint32_t PlayRecordGdvCutscene(int index) override { if (!mute) printf("C play_record_gdv_cutscene %d\n", index); return 0; }
	void ExitCutsceneOverlayMode() override { if (!mute) printf("C exit_cutscene_overlay_mode\n"); }
	void AdvanceClamp8a0f0(uint32_t a, uint32_t m) override
	{
		if (!mute) printf("C advance_clamp_8a0f0 %u %u\n", a, m);
		uint32_t v = (uint32_t)health + a;      // renderer.c:9818-9824
		if (v >= m) v = m;
		health = (int32_t)v;
	}
	void ZeroHealthAndFlagDeath() override { health = 0; dead = 1; }
	uint32_t HealthClampMax() override { return healthMax; }
	void SetLatch83b20() override { l83b20 = 1; }
	void SetLatch80b2c() override { screen->RequestClose(); }
	int32_t CutsceneOverlayActive() override { return overlay; }
	int32_t InventoryPanelOpen() override { return screen->g_inventory_panel_open; }
	int32_t InspectPopupActive() override { return screen->g_inspect_popup_active; }
	void ArmInventoryPanelExit() override { screen->ArmExit(); }

	// ---- speech: the real DBASE500 headers ----
	bool SpeechHeader(uint32_t clip, uint8_t hdr[0x2c]) override
	{
		if (speech == nullptr) return false;
		if (fseek(speech, (long)(clip << 3), SEEK_SET) != 0) return false;
		return fread(hdr, 1, 0x2c, speech) == 0x2c;
	}

	void QueueTimedMessageColor(const std::string &msg, uint8_t color) override
	{ if (!mute) printf("C queue_timed_message_color %u \"%s\"\n", color, msg.c_str()); DialogueUi::QueueTimedMessageColor(msg, color); }
	uint32_t PrimeVoiceClip(uint32_t clip) override
	{ if (!mute) printf("C prime_voice_clip %u\n", clip); return DialogueUi::PrimeVoiceClip(clip); }

	// ---- inventory sink ----
	void PickupFlyStarted(int32_t x, int32_t y, int32_t z, uint16_t renderId) override
	{ if (!mute) printf("C begin_item_pickup_lock %d %d %d %u\n", x, y, z, renderId); }
	void ItemMessage(int interfaceIndex, int colorIndex, uint32_t nameA, uint32_t nameB) override
	{ DialogueUi::ItemMessage(interfaceIndex, colorIndex, nameA, nameB); }
	void RefreshPanel() override { screen->RefreshPanel(); }
	void ClearTabCursors() override { screen->ClearTabCursors(); }
	bool PanelCloseRequested() override { return screen->PanelCloseRequested(); }
	void EquipWeapon(int slot, int32_t itemId) override
	{
		// activate_weapon_item goes on to count the weapon's ammo with
		// query_player_inventory(ammo id, 0), which clears the autoselect flag
		// (weapon_combat.c:450). Weapons are another system; the one effect of
		// that call on THIS state is reproduced when the ammo id is known.
		(void)slot; (void)itemId;
	}
};

void QueueTimedMessageLogged(Host &h, const std::string &msg, uint8_t color);

struct World
{
	Dbase100 db;
	GameCore core;
	Host host;
	InventoryScreen screen;
};

void HexBytes(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", p[i]); }

void Dump(World &w, const char *tag)
{
	Dbase100InterpState &s = w.core.Interp().State();
	Inventory &inv = w.core.Items();
	printf("S %s\n flags=", tag);
	HexBytes(s.g_dbase100_record_bitmap.data(), s.g_dbase100_record_bitmap.size());
	printf("\n inv=%d:", inv.Count());
	for (int i = 0; i < 256; i++) if (inv.Slot(i).id) printf(" %d:%u.%u", i, inv.Slot(i).id, inv.Slot(i).quantity);
	printf("\n hands L=%d R=%d adam=%d/%d auto=%d last=%d lock=%x/%d\n", inv.LeftHand(), inv.RightHand(), inv.AdamSlot(), (int)inv.AdamItemId(),
		inv.AutoSelected() ? 1 : 0, (int)inv.LastItem(), inv.PickupLockFlags(), (int)inv.PickupLockTimer());
	printf(" q=%d:", s.g_dialogue_action_queue_count);
	for (int i = 0; i < s.g_dialogue_action_queue_count && i < 8; i++)
		printf(" [%x,%d,%x]", s.g_dialogue_action_queue[i].chain, s.g_dialogue_action_queue[i].count, s.g_dialogue_action_queue[i].flags);
	// The original's panel drawing reads the cursor item's label and leaves its
	// colour in 0x82040: a draw-side leftover, not compared for the screen verbs.
	const std::string t = tag;
	const bool screenVerb = t == "tab" || t == "hold" || t == "cell" || t == "handbox" || t == "commit" || t == "useself" || t == "combine";
	printf("\n dlg busy=%d ctx=%d gate=%x seg=%d dwell=%d div=%d vstate=%d vsamp=%d qa=%d topic=%x col=",
		s.g_dialogue_busy_flag, s.g_active_dialogue_context, (uint32_t)s.g_move_freeze_gate, w.host.g_dialogue_segment_index,
		w.host.g_dialogue_line_dwell, w.host.g_dialogue_dwell_divisor, s.g_voice_stream_state, w.host.g_voice_sample_count,
		s.g_dbase100_queue_active, s.g_dbase100_pending_topic);
	if (screenVerb) printf("-\n"); else printf("%u\n", w.host.g_timed_message_color);
	printf(" choice cur=%x rem=%d mode=%x count=%d sel=%d saved=%d idx=", s.g_dbase100_choice_cursor, s.g_dbase100_choice_remaining,
		s.g_dbase100_choice_mode, s.g_dbase100_choice_count, s.g_choice_selected_index, s.g_choice_saved_index);
	for (int i = 0; i < 17; i++) printf("%d,", s.g_dbase100_choice_record_indices[i]);
	printf("\n misc l83b20=%d l80b2c=%d health=%d dead=%d overlay=%d red=%d tm=%d/%d panel=%d popup=%d\n", w.host.l83b20, w.screen.g_ui_close_request,
		w.host.health, w.host.dead, w.host.overlay, s.g_value_reduction_factor, w.host.TimedMessageLineCount(), w.host.TimedMessageTimer(),
		w.screen.g_inventory_panel_open, w.screen.g_inspect_popup_active);
	const std::vector<TextLine> &seg = w.host.Segments();
	for (int i = 0; i < s.g_active_dialogue_context && i < 16 && i < (int)seg.size(); i++)
	{
		printf(" seg %d x0=%d x1=%d h=%d len=%d text=", i, seg[i].x0, seg[i].x1, seg[i].height, seg[i].len);
		HexBytes(seg[i].text.data(), seg[i].text.size() > 600 ? 600 : seg[i].text.size());
		printf("\n");
	}
	const std::vector<TextLine> &tm = w.host.TimedMessageLines();
	for (int i = 0; i < w.host.TimedMessageLineCount() && i < 14 && i < (int)tm.size(); i++)
	{
		printf(" tml %d x0=%d x1=%d h=%d len=%d text=", i, tm[i].x0, tm[i].x1, tm[i].height, tm[i].len);
		HexBytes(tm[i].text.data(), tm[i].text.size() > 600 ? 600 : tm[i].text.size());
		printf("\n");
	}
}

void DumpEntries(World &w)
{
	const InventoryEntryList &l = w.screen.Entries();
	const int n = (int)l.slots.size();
	printf("E count=%d list=%d held=%d lh=%d rh=%d dl=%d dr=%d adam=%d:", n, w.screen.ActiveList(), w.screen.HeldEntry(),
		l.leftEntry, l.rightEntry, l.shownLeft >= 0 ? 1 : 0, l.shownRight >= 0 ? 1 : 0, l.adamEntry);
	Inventory &inv = w.core.Items();
	for (int i = 0; i < n; i++) printf(" %d/%d", l.slots[(size_t)i], (int)(int16_t)inv.Slot(l.slots[(size_t)i]).id);
	printf("\n");
}

// The first state block of the oracle log and its ENV line.
bool Seed(World &w, const std::string &oraclePath, int &pitch)
{
	FILE *f = fopen(oraclePath.c_str(), "r");
	if (!f) return false;
	std::vector<char> line(1 << 16);
	bool inInitial = false, seeded = false;
	Dbase100InterpState &s = w.core.Interp().State();
	Inventory &inv = w.core.Items();
	inv.Reset();
	while (fgets(line.data(), (int)line.size(), f))
	{
		const char *p = line.data();
		if (!strncmp(p, "S initial", 9)) { inInitial = true; continue; }
		if (!strncmp(p, "ENV ", 4))
		{
			unsigned seed = 0, rng = 0; int cd = 0, rate = 0, decodeb = 0, sub = 0; unsigned base300 = 0, hmax = 0; unsigned mc[7] = {}; int mx = 0, my = 0, sh = 0;
			sscanf(p, "ENV pitch=%d rate=%d rng=%x,%x,%d decodeb=%d sub=%d base300=%x hmax=%u mc=%u,%u,%u,%u,%u,%u,%u mouse=%d,%d screenh=%d", &pitch, &rate, &seed, &rng, &cd, &decodeb, &sub,
				&base300, &hmax, &mc[0], &mc[1], &mc[2], &mc[3], &mc[4], &mc[5], &mc[6], &mx, &my, &sh);
			if (sh > 0) g_screenH = sh;
			w.host.SetPointer(mx + my, -1);
			w.core.Rng().seed = (uint16_t)seed; w.core.Rng().rng = (uint16_t)rng; w.core.Rng().countdown = cd;
			w.host.healthMax = hmax;
			w.host.subtitlesOff = sub != 0;
			if (strstr(p, " mc=")) for (int i = 0; i < 7; i++) w.host.defaultMessageColor[i] = (uint8_t)mc[i];
			continue;
		}
		if (!inInitial) continue;
		if (p[0] == '>' || (p[0] == 'S' && p[1] == ' ')) { inInitial = false; continue; }
		if (!strncmp(p, " flags=", 7))
		{
			p += 7; size_t i = 0;
			while (p[0] && p[1] && p[0] != '\n' && i < s.g_dbase100_record_bitmap.size())
			{ unsigned v = 0; sscanf(p, "%2x", &v); s.g_dbase100_record_bitmap[i++] = (uint8_t)v; p += 2; }
			seeded = true;
		}
		else if (!strncmp(p, " inv=", 5))
		{
			const char *q = strchr(p, ':'); q = q ? q + 1 : p;
			int idx; unsigned id, qty; int used = 0;
			std::vector<uint8_t> raw(Inventory::SAVE_BYTES, 0);
			while (sscanf(q, " %d:%u.%u%n", &idx, &id, &qty, &used) == 3)
			{
				raw[(size_t)idx * 4] = (uint8_t)id; raw[(size_t)idx * 4 + 1] = (uint8_t)(id >> 8);
				raw[(size_t)idx * 4 + 2] = (uint8_t)qty; raw[(size_t)idx * 4 + 3] = (uint8_t)(qty >> 8);
				q += used;
			}
			inv.Deserialize(raw.data(), raw.size(), nullptr);
		}
		else if (!strncmp(p, " hands", 6))
		{
			int L, R, as, ai, au, last; unsigned lock; int lt;
			if (sscanf(p, " hands L=%d R=%d adam=%d/%d auto=%d last=%d lock=%x/%d", &L, &R, &as, &ai, &au, &last, &lock, &lt) == 8)
			{
				inv.SetLeftHand(L);
				inv.SetRightHand(R, nullptr);
				inv.RestoreActiveHeldItem(w.db, nullptr);      // re-latches the Adam record from the slots
				inv.SetLastItem(last);
			}
		}
		else if (!strncmp(p, " dlg", 4))
		{
			const char *c = strstr(p, "col=");
			if (c) w.host.g_timed_message_color = (uint8_t)atoi(c + 4);
		}
		else if (!strncmp(p, " misc", 5))
		{
			const char *c = strstr(p, "health=");
			if (c) w.host.health = atoi(c + 7);
			c = strstr(p, "l83b20=");
			if (c) w.host.l83b20 = atoi(c + 7);
		}
	}
	fclose(f);
	return seeded;
}

struct Snapshot
{
	Dbase100InterpState interp;
	Inventory inv;
	RothIndexRng rng;
	InventoryScreen screen;
	bool valid = false;
	// The DialogueUi's own state, field by field (it holds pointers).
	int32_t seg, dwell, div, ramp, pcol, lastpos, tmcount, tmtimer, vsamp, vrate, mode, lines, pending, vclock;
	uint8_t col;
	int32_t l83b20, health, dead;
};

} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "dialogue_diff <DATA folder> <oracle log> <script>\n"); return 2; }
	const std::string dir = argv[1];
	World w;
	std::vector<uint8_t> file = ReadFile(dir + "/DBASE100.DAT");
	if (file.empty() || !w.db.Load(file.data(), file.size())) { fprintf(stderr, "no DBASE100.DAT in %s\n", dir.c_str()); return 2; }
	w.host.text = ReadFile(dir + "/DBASE400.DAT");
	w.host.speech = fopen((dir + "/DBASE500.DAT").c_str(), "rb");
	w.host.core = &w.core;
	w.host.screen = &w.screen;
	w.core.Attach(&w.db, &w.host, &w.host);
	w.host.Bind(&w.core.Interp(), &w.db);
	w.screen.Bind(&w.core, &w.host, &w.db);
	w.core.Interp().State().g_choice_selected_index = -1;       // 0x71370's initialised value

	int pitch = 640;
	if (!strcmp(argv[2], "-"))
	{
		// No oracle log: the state a new game leaves, which is the state the
		// original is in when the probe starts (compare any oracle log's first
		// block): action 3's items, the Matchbox selected, full health.
		w.host.mute = true;
		w.core.NewGame();
		w.host.mute = false;
		w.host.l83b20 = 0;              // the game loop has consumed the intro latch by then
		w.host.health = 0x800;          // game_core.c:828-830
		w.host.healthMax = 0x800;
		w.host.g_timed_message_color = 0x67;
		w.core.Interp().State().g_choice_selected_index = -1;
	}
	else if (!Seed(w, argv[2], pitch)) { fprintf(stderr, "could not seed from %s\n", argv[2]); return 2; }
	w.host.screenPitch = pitch;

	printf("# dlgprobe\n");
	Dump(w, "initial");

	Snapshot snap;
	bool latch = false;         // 0x8a26c
	bool quiet = false;
	const int screenH = g_screenH;      // g_screen_pitch + 4 (0x8549c)

	FILE *f = fopen(argv[3], "r");
	if (!f) { fprintf(stderr, "no script %s\n", argv[3]); return 2; }
	char line[1024];
	while (fgets(line, sizeof line, f))
	{
		char cmd[64]; int a[8] = {};
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#') continue;
		sscanf(line, "%63s %i %i %i %i %i %i %i %i", cmd, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]);
		printf("> %s\n", line);
		const std::string c = cmd;
		Dbase100InterpState &s = w.core.Interp().State();
		Inventory &inv = w.core.Items();

		if (c == "adv") { printf("ADV"); for (int ch = 0; ch < 256; ch++) printf(" %d", FontCharAdvance((uint8_t)ch)); printf("\n"); continue; }
		else if (c == "env") { printf("ENV\n"); continue; }
		else if (c == "pitch") { w.host.screenPitch = a[0]; continue; }
		else if (c == "alllines")
		{
			for (int i = 1; i <= w.db.ActionCount(); i++)
			{
				const DbAction *act = w.db.Action(i);
				if (act == nullptr || !act->ok) continue;
				for (int k = 0; k < act->commandCount; k++)
				{
					const DbCommand cm = w.db.ActionCommand(*act, k);
					if (cm.opcode != 5 && cm.opcode != 8) continue;
					std::vector<uint8_t> ibuf(0x800, 0);
					const uint32_t len = w.host.ReadNextDialogueLine(ibuf.data(), 0x3fc, cm.operand, 0);
					std::vector<TextLine> out;
					const int ctx = LayoutTimedMessageText(out, ibuf.data(), w.host.screenPitch, a[0], w.host.g_timed_message_color);
					printf("L %d %d %u %x n=%u col=%u ctx=%d:", i, k, (unsigned)cm.opcode, cm.operand, len, w.host.g_timed_message_color, ctx);
					for (int sg = 0; sg < ctx && sg < 64 && sg < (int)out.size(); sg++)
					{
						uint32_t h = 2166136261u;
						for (uint8_t b : out[(size_t)sg].text) h = (h ^ b) * 16777619u;
						printf(" %d,%d,%d,%d,%08x", out[(size_t)sg].x0, out[(size_t)sg].x1, out[(size_t)sg].height, out[(size_t)sg].len, h);
					}
					printf("\n");
				}
			}
			continue;
		}
		else if (c == "snap")
		{
			snap.interp = s; snap.inv = inv; snap.rng = w.core.Rng(); snap.screen = w.screen; snap.valid = true;
			snap.seg = w.host.g_dialogue_segment_index; snap.dwell = w.host.g_dialogue_line_dwell; snap.div = w.host.g_dialogue_dwell_divisor;
			snap.ramp = w.host.g_dialogue_reveal_ramp; snap.pcol = w.host.g_dialogue_panel_color; snap.lastpos = w.host.g_choice_last_pos;
			snap.tmcount = w.host.g_timed_message_line_count; snap.tmtimer = w.host.g_timed_message_timer; snap.vsamp = w.host.g_voice_sample_count;
			snap.vrate = w.host.g_voice_rate; snap.mode = w.host.g_choice_interaction_mode; snap.lines = w.host.g_choice_line_count;
			snap.pending = w.host.g_pending_choice_accept_index; snap.vclock = w.host.voiceClockTicks; snap.col = w.host.g_timed_message_color;
			snap.l83b20 = w.host.l83b20; snap.health = w.host.health; snap.dead = w.host.dead;
			continue;
		}
		else if (c == "reset")
		{
			if (snap.valid)
			{
				s = snap.interp; inv = snap.inv; w.core.Rng() = snap.rng; w.screen = snap.screen;
				w.host.g_dialogue_segment_index = snap.seg; w.host.g_dialogue_line_dwell = snap.dwell; w.host.g_dialogue_dwell_divisor = snap.div;
				w.host.g_dialogue_reveal_ramp = snap.ramp; w.host.g_dialogue_panel_color = snap.pcol; w.host.g_choice_last_pos = snap.lastpos;
				w.host.g_timed_message_line_count = snap.tmcount; w.host.g_timed_message_timer = snap.tmtimer; w.host.g_voice_sample_count = snap.vsamp;
				w.host.g_voice_rate = snap.vrate; w.host.g_choice_interaction_mode = snap.mode; w.host.g_choice_line_count = snap.lines;
				w.host.g_pending_choice_accept_index = snap.pending; w.host.voiceClockTicks = snap.vclock; w.host.g_timed_message_color = snap.col;
				w.host.l83b20 = snap.l83b20; w.host.health = snap.health; w.host.dead = snap.dead;
				latch = false;
			}
		}
		else if (c == "voice") { w.host.voiceMode = a[0]; w.host.speechAvailable = a[0] == 2; continue; }
		else if (c == "mapcalls") { continue; }
		else if (c == "quiet") { quiet = a[0] != 0; continue; }
		else if (c == "act") { const uint32_t r = w.core.RunAction((uint32_t)a[0]); printf("R %d\n", (int)r); }
		else if (c == "qact")
		{
			if (!latch) { latch = true; s.g_dbase100_queue_active = 1; }       // raw_commands.c:4701-4704
			const uint32_t r = w.core.Interp().EvalDialogueRecordById((uint32_t)a[0]);
			printf("R %d\n", (int)r);
		}
		else if (c == "fin") { w.core.Interp().FinishDialogueRecordEval(); latch = false; inv.ClearAutoSelected(); }   // raw_commands.c:1313-1315
		else if (c == "pump") { w.host.VoiceStreamPump(); }
		else if (c == "tick") { w.host.TickTextUi(a[0]); }
		else if (c == "frames") { for (int i = 0; i < a[0]; i++) { w.host.VoiceStreamPump(); w.host.TickTextUi(a[1] ? a[1] : 1); } }
		else if (c == "vend") { w.host.VoiceEnded(); }
		else if (c == "next") { w.host.ChoiceSelectNext(); }
		else if (c == "prev") { w.host.ChoiceSelectPrev(); }
		else if (c == "accept") { w.host.ChoiceAcceptSelected(); }
		else if (c == "enter") { w.host.UseEnterKey(); }
		else if (c == "skip") { const uint32_t r = w.host.TryInterruptDialogueVoice(); printf("R %d\n", (int)r); }
		else if (c == "forceend") { w.host.DialogueVoiceForceEnd(0, 0); }
		else if (c == "stopall") { w.host.DialogueVoiceStopAll(); }
		else if (c == "clickrow")
		{
			// The pixel the probe clicks, rebuilt from the port's own line records
			// with the original's arithmetic (dialogue_ui.c:255-268, 1190-1212).
			const std::vector<TextLine> &seg = w.host.Segments();
			const int ctx = s.g_active_dialogue_context;
			int panelH = 0;
			for (int i = 0; i < ctx && i < (int)seg.size(); i++) panelH += seg[(size_t)i].height + 4;
			panelH -= 4;
			uint32_t u = (uint32_t)(screenH - panelH) >> 1;
			int x = -1, y = -1;
			for (int i = 0; i < ctx && i < (int)seg.size(); i++)
			{
				if (i == a[0]) { x = seg[(size_t)i].x0 - 0x28 + 0x28 + 1; y = (int)u + 1; break; }
				u += (uint32_t)seg[(size_t)i].height + 4;
			}
			printf("X %d %d\n", x, y);
			w.host.PointerClick(x + y, (a[0] >= 0 && a[0] < ctx) ? a[0] : -1);
		}
		else if (c == "give") { const int32_t r = w.core.Give(a[0], nullptr); printf("R %d\n", (int)r); }
		else if (c == "givectx")
		{
			GiveContext ctx; ctx.id = (uint16_t)a[1]; ctx.x = (int16_t)a[2]; ctx.y = (int16_t)a[3]; ctx.z = (int16_t)a[4]; ctx.flags7 = (uint8_t)a[5];
			const int32_t r = w.core.Give(a[0], &ctx); printf("R %d\n", (int)r);
		}
		else if (c == "remove") { const int r = w.core.Remove((uint16_t)a[0]); printf("R %d\n", r); }
		else if (c == "query") { const int r = w.core.Query((uint16_t)a[0], a[1]); printf("R %d\n", r); }
		else if (c == "list")
		{
			int32_t ids[16] = {}; const int cnt = a[1];
			for (int i = 0; i < cnt && i < 6; i++) ids[i] = a[2 + i];
			const int r = w.core.FindOrAutoselect(ids, cnt, a[0]); printf("R %d\n", r);
		}
		else if (c == "filter") { w.core.SetFilter(a[0]); }
		else if (c == "easy") { inv.SetEasySelect(a[0] != 0); continue; }
		else if (c == "flagset") { printf("R %d\n", (int)w.core.Interp().Dbase100BitmapTestSet((uint32_t)a[0])); }
		else if (c == "flagclr") { printf("R %d\n", (int)w.core.Interp().Dbase100BitmapTestClear((uint32_t)a[0])); }
		else if (c == "tmsg") { QueueTimedMessageLogged(w.host, strchr(line, ' ') + 1, 0x20); }
		else if (c == "examine")
		{
			const DbTriggerBlock *blk = nullptr;
			FindInspectByRenderId(w.db, a[0], &blk);
			printf("R %d\n", blk ? (int)blk->offset : 0);
			if (blk) w.core.Interp().EvalDialogueRecordConditionWithCleanup(blk->offset, 1);
		}
		else if (c == "pickup")
		{
			GiveContext ctx; ctx.id = (uint16_t)a[0]; ctx.x = (int16_t)a[1]; ctx.y = (int16_t)a[2]; ctx.z = (int16_t)a[3]; ctx.flags7 = (uint8_t)a[4];
			const bool pk = IsItemIdPickable(w.db, a[0]);
			const int32_t r = inv.GiveItemByRenderId(w.db, a[0], &ctx, w.core.InvSink());
			printf("R %d pickable=%d\n", (int)r, pk ? 1 : 0);
		}
		else if (c == "locktick") { inv.TickPickupLock(a[0]); }
		else if (c == "tab") { w.screen.g_cursor_active_list = a[0]; w.screen.RefreshPanel(); DumpEntries(w); }
		else if (c == "hold") { w.screen.g_inventory_panel_open = 1; w.screen.g_cursor_scroll_offsets[w.screen.ActiveList()] = 0; w.screen.ClickCell(a[0], InventoryScreen::SECONDARY); DumpEntries(w); }
		else if (c == "cell") { w.screen.g_cursor_scroll_offsets[w.screen.ActiveList()] = 0; w.screen.ClickCell(a[0], a[1]); DumpEntries(w); }
		else if (c == "handbox") { w.screen.ClickHandBox(a[0], a[1]); DumpEntries(w); }
		else if (c == "commit") { inv.CommitHeld(w.screen.ActiveList(), w.core.InvSink()); DumpEntries(w); }
		else if (c == "useself") { inv.UseHeldOnSelf(w.db, w.core.InvSink()); DumpEntries(w); }
		else if (c == "combine") { inv.CombineHeldWith(w.db, w.screen.Entries().slots.size() > (size_t)a[0] ? w.screen.Entries().slots[(size_t)a[0]] : -1, w.core.InvSink()); DumpEntries(w); }
		else if (c == "topics" || c == "topic")
		{
			if (c == "topics") printf("R %d\n", (int)w.host.BuildAvailableChoiceMenu(a[0]));
			else w.host.ActivateSelectedChoiceRecord((uint32_t)a[0]);
			const std::vector<TextLine> &cl = w.host.TopicLines();
			printf("T mode=%d lines=%d pending=%d\n", w.host.g_choice_interaction_mode, w.host.g_choice_line_count, w.host.g_pending_choice_accept_index);
			for (int i = 0; i < w.host.g_choice_line_count && i < 16 && i < (int)cl.size(); i++)
			{
				printf(" cl %d x0=%d x1=%d h=%d len=%d text=", i, cl[(size_t)i].x0, cl[(size_t)i].x1, cl[(size_t)i].height, cl[(size_t)i].len);
				HexBytes(cl[(size_t)i].text.data(), cl[(size_t)i].text.size() > 600 ? 600 : cl[(size_t)i].text.size());
				printf("\n");
			}
		}
		else if (c == "icons")
		{
			const std::vector<uint8_t> d2 = ReadFile(dir + "/DBASE200.DAT");
			for (int i = 1; i <= w.db.ItemCount(); i++)
			{
				const DbItem *rec = w.db.Item(i);
				if (rec == nullptr || !rec->ok || rec->icon == 0) continue;
				const DasImage im = DecodeDbase200Resource(d2, rec->icon);
				uint32_t h = 2166136261u;
				for (uint8_t b : im.pixels) h = (h ^ b) * 16777619u;
				printf("I %d %d %d %08x\n", i, im.width, im.height, h);
			}
			continue;
		}
		else if (c == "label")
		{
			const DbItem *rec = w.db.Item(a[0]);
			const uint32_t shown = ItemLabelText(w.db, a[0], w.core.InvSink());
			const uint32_t raw = rec ? rec->name : 0;
			printf("R %x %x tabidx=%d choice=%d\n", raw, shown != raw ? shown : 0, ItemTabIndex(w.db, a[0]), ItemHasAvailableChoice(w.db, a[0], w.core.InvSink()) ? 1 : 0);
			continue;
		}
		else { printf("? unknown command\n"); continue; }
		if (!quiet) Dump(w, cmd);
	}
	fclose(f);
	return 0;
}

namespace
{
void QueueTimedMessageLogged(Host &h, const std::string &msg, uint8_t color)
{
	h.QueueTimedMessageColor(msg, color);
}
}
