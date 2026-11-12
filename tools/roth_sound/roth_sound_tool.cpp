//
// roth_sound_tool -- what Realms of the Haunting's sound data holds, read with
// the SAME code the engine uses (src/roth/roth_sfx.cpp), over the whole retail
// set. It checks the rules roth_sound.cpp relies on and lists what a map will
// ask for. It exports nothing: no sample, song or clip is written anywhere,
// only ids, lengths, rates and hashes are printed.
//
//   roth_sound_tool <folder with ROTH.RES and M> [--samples] [--nodes]
//                   [--commands] [--speech]
//
// Exit code 0 when every check passed, 1 when any failed.
//

#include "roth_sfx.h"
#include "roth_raw.h"
#include "roth_install.h"
#include "roth_dbase100.h"

#include <map>
#include <set>
#include <string>
#include <vector>
#include <string.h>
#include <stdio.h>

using namespace roth;

static int gFailures = 0;
static void Fail(const char *fmt, const char *a = "", int b = 0, int c = 0)
{
	gFailures++;
	printf("  FAIL: ");
	printf(fmt, a, b, c);
	printf("\n");
}

static bool Has(int argc, char **argv, const char *flag)
{
	for (int i = 2; i < argc; i++) if (strcmp(argv[i], flag) == 0) return true;
	return false;
}

static uint32_t U32(const std::vector<uint8_t> &d, size_t o)
{
	if (o + 4 > d.size()) return 0;
	return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}

// The row a RAW record's "plus one" sound word names; reports a bad one.
static void CheckRow(const SfxBank &bank, const char *map, int rec, const char *what, uint16_t plusOne, int &bad)
{
	if (plusOne == 0 || !bank.IsOpen()) return;
	const SfxRow *r = bank.Row((int)plusOne - 1);
	if (r == nullptr || !r->Loadable())
	{
		bad++;
		printf("  FAIL: %s record %d %s names row %d, which the bank does not have\n", map, rec, what, (int)plusOne - 1);
		gFailures++;
	}
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("roth_sound_tool <folder with ROTH.RES and M> [--samples] [--nodes] [--commands] [--speech]\n");
		return 2;
	}
	const std::string root = argv[1];
	const bool listSamples = Has(argc, argv, "--samples");
	const bool listNodes = Has(argc, argv, "--nodes");
	const bool listCommands = Has(argc, argv, "--commands");
	const bool listSpeech = Has(argc, argv, "--speech");

	Install install;
	if (!install.Open(root.c_str()))
	{
		printf("not an install: %s\n", install.Error().c_str());
		return 2;
	}

	// ---- the effect bank ------------------------------------------------
	printf("== EFFECT BANK ==\n");
	std::string how, tried;
	const std::string bankPath = FindEffectBank(install.Path(), &how, &tried);
	SfxBank bank;
	if (bankPath.empty()) { printf("  not found. Looked in:%s\n", tried.c_str()); gFailures++; }
	else if (!bank.Open(bankPath)) { printf("  %s: %s\n", bankPath.c_str(), bank.Error().c_str()); gFailures++; }
	else
	{
		printf("  file    %s\n  chosen  %s\n", bankPath.c_str(), how.c_str());
		std::map<int, int> flagHist;
		int empty = 0, keyIsRowPlus1 = 0;
		uint64_t bytes = 0, frames70 = 0;
		std::set<uint16_t> keys;
		for (int i = 0; i < bank.Count(); i++)
		{
			const SfxRow *r = bank.Row(i);
			flagHist[r->flags]++;
			if (!r->Loadable()) empty++;
			if (r->key == i + 1) keyIsRowPlus1++;
			keys.insert(r->key);
			bytes += r->size;
			frames70 += r->Ticks70();
			const std::vector<uint8_t> raw = bank.Read(i);
			if (r->Loadable() && raw.size() != r->size) Fail("row %s%d could not be read in full", "", i);
			if (listSamples)
				printf("  row %3d key %3u flags %02x  %5d Hz %2d-bit  %8u bytes %7u samples %6.2f s  hash %08x\n",
					i, (unsigned)r->key, (unsigned)r->flags, r->Rate(), r->Is16Bit() ? 16 : 8,
					(unsigned)r->size, (unsigned)r->Frames(), double(r->Frames()) / r->Rate(),
					(unsigned)SfxHash(raw.data(), raw.size()));
			const std::vector<uint8_t> wav = bank.ReadAsWave(i);
			if (r->Loadable() && (wav.size() < 44 || memcmp(wav.data(), "RIFF", 4) != 0))
				Fail("row %s%d did not become a WAVE image", "", i);
		}
		printf("  rows    %d (%d unloadable), %llu sample bytes, %.1f minutes of sound\n", bank.Count(), empty,
			(unsigned long long)bytes, double(frames70) / 70. / 60.);
		printf("  flags  ");
		for (auto &kv : flagHist) printf(" 0x%02x x%d", kv.first, kv.second);
		printf("   (bit0 16-bit, bit1 22050 Hz)\n");
		printf("  keys    %d distinct; key == row + 1 on %d of %d rows\n", (int)keys.size(), keyIsRowPlus1, bank.Count());
		if ((int)keys.size() != bank.Count()) Fail("duplicate keys in the bank%s", "");
	}

	// ---- DBASE100: sound opcodes and music ---------------------------------
	printf("\n== DBASE100 ==\n");
	Dbase100 db;
	std::vector<uint8_t> dbBytes = Install::ReadWholeFile(install.DataFile("DBASE100.DAT"));
	std::map<int, std::vector<uint32_t>> musicByAction;      // action id -> chunk indices (op 0x1a)
	std::set<uint32_t> songs;
	if (dbBytes.empty() || !db.Load(dbBytes.data(), dbBytes.size())) { printf("  DBASE100.DAT unreadable\n"); gFailures++; }
	else
	{
		// Action records: op 0x19 carries a row + 1 as it stands.
		int act19 = 0, actBad = 0;
		for (int id = 1; id <= db.ActionCount(); id++)
		{
			const DbAction *a = db.Action(id);
			if (a == nullptr) continue;
			for (const DbCommand &c : db.Commands(*a))
			{
				if (c.opcode == 0x19)
				{
					act19++;
					const int row = (int)(c.operand & 0x7fff) - 1;
					if (bank.IsOpen() && (bank.Row(row) == nullptr)) { actBad++; printf("  action %d: op 0x19 row %d not in the bank\n", id, row); }
					if (listCommands) printf("  action %d: play_sound_effect row %d\n", id, row);
				}
				if (c.opcode == 0x1a) { musicByAction[id].push_back(c.operand); songs.insert(c.operand); }
			}
		}
		printf("  action records: %d play a sound effect (op 0x19), %d name a row the bank lacks\n", act19, actBad);
		if (actBad) gFailures++;

		// Item records: the keys, resolved exactly as the engine will.
		std::vector<uint8_t> image = dbBytes;
		std::map<int, int> opHist;
		int unresolved = 0;
		for (int id = 1; id <= db.ItemCount(); id++)
		{
			const DbItem *it = db.Item(id);
			if (it == nullptr) continue;
			for (const DbTriggerBlock &b : it->blocks)
				for (const DbCommand &c : db.Commands(b))
				{
					if (c.opcode == 0x19 || c.opcode == 0x21 || c.opcode == 0x29 || c.opcode == 0x2a
						|| c.opcode == 0x31 || c.opcode == 0x32)
					{
						opHist[c.opcode]++;
						if (bank.IsOpen() && bank.IndexForKey((uint16_t)c.operand) == 0) unresolved++;
					}
					if (c.opcode == 0x1a) songs.insert(c.operand);
				}
		}
		int total = 0;
		printf("  item records: sound-key opcodes");
		for (auto &kv : opHist) { printf(" 0x%02x x%d", kv.first, kv.second); total += kv.second; }
		printf(" = %d words\n", total);
		if (bank.IsOpen())
		{
			const int rewritten = bank.ResolveDbase100(image.data(), image.size());
			printf("  resolve_dbase100_sound_ids rewrote %d words (%d keys had no row and became 0)\n", rewritten, unresolved);
			if (rewritten != total) Fail("the resolve pass and the record walk disagree%s (%d vs %d)", "", rewritten, total);
		}
	}

	// ---- music -------------------------------------------------------------
	printf("\n== MUSIC (DBASE300, DBASE100 op 0x1a) ==\n");
	std::string triedSong;
	const std::string songPath = FindRetailFile(install.Path(), "DBASE300.DAT", &triedSong);
	if (songPath.empty()) { printf("  DBASE300.DAT not found. Looked in:%s\n", triedSong.c_str()); gFailures++; }
	else
	{
		printf("  file    %s\n", songPath.c_str());
		for (uint32_t idx : songs)
		{
			if (idx == 0) { printf("  chunk 0: music off\n"); continue; }
			std::string why;
			const std::vector<uint8_t> chunk = ReadDbase300Chunk(songPath, idx, &why);
			if (chunk.empty()) { printf("  chunk %u: %s\n", (unsigned)idx, why.c_str()); gFailures++; continue; }
			const bool hmp = chunk.size() >= 0x40 && memcmp(chunk.data(), "HMIMIDIP", 8) == 0;
			std::string users;
			for (auto &kv : musicByAction)
				for (uint32_t c : kv.second) if (c == idx) { users += " " + std::to_string(kv.first); break; }
			printf("  chunk %7u: %6u bytes, %s, %u tracks, hash %08x; DBASE100 actions:%s\n", (unsigned)idx,
				(unsigned)chunk.size(), hmp ? std::string((const char *)chunk.data(), 14).c_str() : "NOT HMP",
				hmp ? (unsigned)U32(chunk, 0x30) : 0u, (unsigned)SfxHash(chunk.data(), chunk.size()),
				users.empty() ? " (item records only)" : users.c_str());
			if (!hmp) gFailures++;
		}
		printf("  %d distinct songs are asked for by scripts\n", (int)songs.size() - (songs.count(0) ? 1 : 0));
	}

	// ---- maps --------------------------------------------------------------
	printf("\n== MAPS ==\n");
	int examineAux = 0, examineOut = 0;
	int totalNodes = 0, totalZones = 0, total10 = 0, layoutBad = 0, rowBad = 0, keyMiss10 = 0, linkMiss = 0;
	std::map<int, int> modeHist, sndByOp;
	for (const std::string &name : install.MapNames())
	{
		const std::vector<uint8_t> raw = Install::ReadWholeFile(install.MapFile(name.c_str()));
		Map m = ParseRaw(raw.data(), raw.size());
		if (!m.ok()) { printf("%s: %s\n", name.c_str(), m.error.c_str()); gFailures++; continue; }
		SfxSection sec;
		if (!sec.Parse(m.sfxSection.data(), m.sfxSection.size()) || !sec.error.empty())
		{ layoutBad++; Fail("%s: section 7 layout", name.c_str()); }
		const size_t zoneBytes = m.sfxSection.size() < 4 ? 0 : m.sfxSection.size() - (4 + sec.nodes.size() * 0x12);
		if (zoneBytes % 0x20 != 0) { layoutBad++; Fail("%s: zone list is not whole records", name.c_str()); }

		int active = 0, mode[4] = { 0, 0, 0, 0 }, nodeBad = 0;
		for (size_t i = 0; i < sec.nodes.size(); i++)
		{
			const SfxNode &n = sec.nodes[i];
			if (n.Active()) active++;
			mode[n.Mode() > 3 ? 3 : n.Mode()]++;
			modeHist[n.Mode() * 2 + (n.Active() ? 1 : 0)]++;
			if (bank.IsOpen() && (bank.Row(n.sound) == nullptr || !bank.Row(n.sound)->Loadable())) nodeBad++;
			if (n.zone > sec.zones.size()) { layoutBad++; Fail("%s: node %d names a zone past the list", name.c_str(), (int)i); }
			if (n.range == 0) Fail("%s: node %d has range 0", name.c_str(), (int)i);
			if (listNodes)
				printf("  %s node %3d at (%6d,%6d) row %3u key %5u mode %d %-6s range %5u period %5u vol %3u zone %u\n",
					name.c_str(), (int)i, n.x, n.y, (unsigned)n.sound, (unsigned)n.key, n.Mode(),
					n.Active() ? "active" : "off", (unsigned)n.range, (unsigned)n.period, (unsigned)n.volume, (unsigned)n.zone);
		}
		if (nodeBad) { rowBad += nodeBad; Fail("%s: %d nodes name a row the bank lacks", name.c_str(), nodeBad); }
		totalNodes += (int)sec.nodes.size();
		totalZones += (int)sec.zones.size();

		// The records that ask for sound.
		int trig = 0, door = 0, sw = 0, lift = 0, c10 = 0, c2b = 0;
		std::set<int> actions;
		for (size_t i = 0; i < m.commands.size(); i++)
		{
			const Command &c = m.commands[i];
			const uint8_t op = (uint8_t)(c.opcode & 0x7f);
			const int rec = (int)i + 1;
			if (c.isTrigger && op != 0x02)
			{
				// An EXAMINE record's +0x0a is the DBASE100 record it shows, not
				// a sound (input.c:953-969; fire_trigger_on_interact clears the
				// latch, raw_commands.c:1683). Counted apart, as the evidence.
				if (op == 0x30 || op == 0x31 || op == 0x32)
				{
					if (c.aux != 0) { examineAux++; if (bank.IsOpen() && bank.Row((int)c.aux - 1) == nullptr) examineOut++; }
				}
				else if (c.aux != 0)
				{
					trig++; sndByOp[op]++;
					CheckRow(bank, name.c_str(), rec, "trigger +0x0a", c.aux, rowBad);
					if (listCommands) printf("  %s rec %4d trigger 0x%02x: chain sound row %d\n", name.c_str(), rec, op, c.aux - 1);
				}
			}
			else if (op == 0x02)
			{
				if (c.Word(0x0E) != 0)
				{
					sw++; sndByOp[op]++;
					CheckRow(bank, name.c_str(), rec, "light switch +0x0e", c.Word(0x0E), rowBad);
					if (listCommands) printf("  %s rec %4d light switch: row %d\n", name.c_str(), rec, c.Word(0x0E) - 1);
				}
			}
			else if (op == 0x2F)
			{
				const uint16_t a = c.Word(0x0C), b = c.Word(0x0E), d = c.Word(0x10);
				if (a | b | d)
				{
					door++; sndByOp[op]++;
					CheckRow(bank, name.c_str(), rec, "door open +0x0c", a, rowBad);
					CheckRow(bank, name.c_str(), rec, "door close +0x0e", b, rowBad);
					CheckRow(bank, name.c_str(), rec, "door shut +0x10", d, rowBad);
					if (listCommands) printf("  %s rec %4d door: open row %d, close row %d, shut row %d (-1 = silent)\n",
						name.c_str(), rec, a - 1, b - 1, d - 1);
				}
			}
			else if (op == 0x07 || op == 0x03)
			{
				const uint16_t key = c.Word(op == 0x07 ? 0x10 : 0x14);
				const uint16_t end = c.Word(op == 0x07 ? 0x12 : 0x0C);
				if (key != 0)
				{
					lift++; sndByOp[op]++;
					const int node = sec.FindByKey(key);
					if (node < 0) linkMiss++;
					else CheckRow(bank, name.c_str(), rec, "mover end sound", end, rowBad);
					if (listCommands) printf("  %s rec %4d mover 0x%02x: node key %u -> node %d, end sound row %d\n",
						name.c_str(), rec, op, (unsigned)key, node, end - 1);
				}
			}
			else if (op == 0x10)
			{
				c10++; total10++;
				int matched = 0;
				for (const SfxNode &n : sec.nodes) if (n.key == c.key) matched++;
				if (matched == 0) keyMiss10++;
				if (listCommands) printf("  %s rec %4d 0x10: key %u flags %02x -> %d node(s)\n", name.c_str(), rec,
					(unsigned)c.key, (unsigned)c.fireFlags, matched);
			}
			else if (op == 0x2B)
			{
				c2b++;
				actions.insert((int)c.key);
			}
		}
		std::string music;
		for (int a : actions)
		{
			auto it = musicByAction.find(a);
			if (it == musicByAction.end()) continue;
			for (uint32_t ch : it->second) music += " " + std::to_string(ch) + "(action " + std::to_string(a) + ")";
		}
		printf("%-9s nodes %3d (active %3d; mode0 %2d mode1 %3d mode2 %d mode3+ %3d) zones %2d | sound in: %2d triggers,"
			" %2d doors, %d switches, %d movers, %2d node commands | music%s\n",
			name.c_str(), (int)sec.nodes.size(), active, mode[0], mode[1], mode[2], mode[3], (int)sec.zones.size(),
			trig, door, sw, lift, c10, music.empty() ? " none asked for by this map's own records" : music.c_str());
	}
	printf("\n  totals: %d nodes, %d zones, %d node commands (0x10); layout failures %d; rows missing %d;\n"
		"          0x10 records whose key matches no node %d; movers whose node key matches none %d\n",
		totalNodes, totalZones, total10, layoutBad, rowBad, keyMiss10, linkMiss);
	printf("  nodes by mode: ");
	for (auto &kv : modeHist) printf(" mode %d %s x%d;", kv.first / 2, (kv.first & 1) ? "active" : "off", kv.second);
	printf("\n  records with sound, by opcode:");
	for (auto &kv : sndByOp) printf(" 0x%02x x%d", kv.first, kv.second);
	printf("\n  examine records (0x30/0x31/0x32) with a non-zero +0x0a: %d, of which %d are past the bank's last row\n"
		"  (so that word is not a sound on those opcodes; it is the DBASE100 record the examine shows)\n",
		examineAux, examineOut);

	// ---- speech ------------------------------------------------------------
	printf("\n== SPEECH (DBASE500, named by DBASE400 lines) ==\n");
	std::string triedVoice;
	const std::string voicePath = FindRetailFile(install.Path(), "DBASE500.DAT", &triedVoice);
	const std::vector<uint8_t> text = Install::ReadWholeFile(install.DataFile("DBASE400.DAT"));
	if (voicePath.empty()) printf("  DBASE500.DAT not found. Looked in:%s\n", triedVoice.c_str());
	else if (text.size() < 16) printf("  DBASE400.DAT unreadable\n");
	else
	{
		printf("  file    %s\n", voicePath.c_str());
		// DBASE400 after its 8-byte signature: { u32 clip, u16 length, u8
		// colour, u8 } then the text, the next line on the following 4-byte
		// boundary. (A sequential walk is this tool's, to find the clips; the
		// engine only ever seeks to an offset a script gives it.)
		std::set<uint32_t> clips;
		int lines = 0;
		for (size_t o = 8; o + 8 <= text.size(); )
		{
			const uint32_t clip = U32(text, o);
			const uint32_t len = (uint32_t)text[o + 4] | ((uint32_t)text[o + 5] << 8);
			lines++;
			if (clip != 0) clips.insert(clip);
			o = (o + 8 + len + 3) & ~(size_t)3;
		}
		int good = 0, dpcm = 0, other = 0;
		std::map<int, int> rates;
		uint64_t samples = 0;
		for (uint32_t clip : clips)
		{
			const VoiceClip v = ReadDbase500Clip(voicePath, clip, listSpeech);
			if (!v.ok) { other++; continue; }
			good++;
			if (v.dpcm) dpcm++;
			rates[v.rate]++;
			samples += v.dpcm ? v.rawSize : v.rawSize / 2;
			if (listSpeech)
			{
				int peak = 0;
				for (int16_t s : v.pcm) { const int a = s < 0 ? -s : s; if (a > peak) peak = a; }
				printf("  clip %8u: %s %5d Hz %8u samples %6.2f s  peak %5d  hash %08x\n", (unsigned)clip,
					v.dpcm ? "DPCM" : "PCM ", v.rate, (unsigned)v.pcm.size(), double(v.pcm.size()) / v.rate, peak,
					(unsigned)SfxHash((const uint8_t *)v.pcm.data(), v.pcm.size() * 2));
			}
		}
		printf("  %d text lines walked, %d name a clip; %d clip headers read cleanly (%d DPCM), %d did not\n",
			lines, (int)clips.size(), good, dpcm, other);
		printf("  rates:");
		for (auto &kv : rates) printf(" %d Hz x%d", kv.first, kv.second);
		printf("; %.1f minutes of speech\n", double(samples) / 22050. / 60.);
	}

	printf("\n%s (%d failure%s)\n", gFailures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", gFailures, gFailures == 1 ? "" : "s");
	return gFailures == 0 ? 0 : 1;
}
