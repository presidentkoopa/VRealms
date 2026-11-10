//
// dbase100_dump -- read the player's own DBASE100.DAT through the same reader
// the engine uses (src/roth/roth_dbase100.cpp) and print what it found.
// Writes nothing.
//
// It exists to settle, against the retail file, the things this sandbox could
// only build from the docs: the counts, every anomaly, the open questions the
// reader notes, the contents of records 1-4 (ROTH_GAME_PORT.md §7), and the
// spot values the specs quote (the Staff, the Dodger).
//
// Usage:
//   dbase100_dump <retail DATA dir | path\to\DBASE100.DAT> [options]
//
//   (no option)     summary: header, notes, anomalies, histograms, records 1-4,
//                   spot checks
//   --items         one line per inventory record, plus its blocks
//   --actions       every action record with its decoded commands
//   --cutscenes     every cutscene entry (and whether DATA\GDV has the file)
//   --interface     every interface entry
//   --all           all of the above
//   --item N        one inventory record in full (1-based id)
//   --action N      one action record in full (1-based id)
//
// When DBASE400.DAT sits beside DBASE100.DAT, names and lines are printed
// from it (dump only: the DBASE400 reading here is a convenience for reading
// the output and is NOT the engine's text reader -- that is step 4).
//

#include "../../src/roth/roth_dbase100.h"
#include "../../src/roth/roth_install.h"
#include "../../src/roth/roth_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <map>

namespace
{

std::vector<uint8_t> g_text;   // DBASE400.DAT, when present

bool EndsWithDat(const std::string &s)
{
	if (s.size() < 4) return false;
	std::string t = s.substr(s.size() - 4);
	for (auto &c : t) c = (char)toupper((unsigned char)c);
	return t == ".DAT";
}

bool FileExists(const std::string &p)
{
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

std::string Dir(const std::string &path)
{
	size_t s = path.find_last_of("\\/");
	return s == std::string::npos ? std::string(".") : path.substr(0, s);
}

std::string FindSibling(const std::string &dir, const char *upper)
{
	std::string lower = upper;
	for (auto &c : lower) c = (char)tolower((unsigned char)c);
	for (const std::string &n : { std::string(upper), lower })
	{
		std::string p = dir + "/" + n;
		if (FileExists(p)) return p;
	}
	return "";
}

// DBASE400 text entry: u32 voice clip, u16 length, u8 colour, (u8), then the
// text (docs/GAME_dialogue.md §2.1). Printed up to the first NUL, the stated
// length, or 72 bytes. Dump-only.
std::string Text400(uint32_t offset)
{
	if (g_text.empty() || offset == 0) return "";
	if ((uint64_t)offset + 8 > g_text.size()) return "<past end of DBASE400>";
	const uint8_t *p = g_text.data() + offset;
	const unsigned len = (unsigned)(p[4] | (p[5] << 8));
	std::string out = "\"";
	for (unsigned i = 0; i < len && i < 72 && offset + 8 + i < g_text.size(); i++)
	{
		uint8_t c = p[8 + i];
		if (c == 0) break;
		if (c == '\n' || c == '\r') { out += ' '; continue; }
		out += (c >= 0x20 && c < 0x7f) ? (char)c : '?';
	}
	return out + "\"";
}

// Names for the DBASE100 interpreter opcodes, from docs/GAME_dialogue.md §2.3
// and docs/GAME_core.md §5.4. Anything else is printed as a number.
const char *OpName(uint8_t op)
{
	switch (op)
	{
	case 0x00: return "doc-page";
	case 0x01: return "if-flag";
	case 0x02: return "if-count>";
	case 0x03: return "if-count==";
	case 0x04: return "set-flag";
	case 0x05: return "say";
	case 0x07: return "cutscene";
	case 0x08: return "choice";
	case 0x09: return "branch";
	case 0x0a: return "end";
	case 0x0b: return "random";
	case 0x0c: return "random-sep";
	case 0x0d: return "if-flag-next";
	case 0x0e: return "fullscreen";
	case 0x0f: return "topic";
	case 0x10: return "count/name";
	case 0x11: return "give";
	case 0x19: return "sfx";
	case 0x1a: return "music";
	case 0x1b: return "topic-stage";
	case 0x1c: return "caller-gate";
	case 0x1d: return "jump";
	case 0x23: return "raw-by-index";
	case 0x26: return "health";
	case 0x2d: return "ui-latch";
	case 0x34: return "use-target";
	case 0x35: return "use-effect";
	case 0x36: return "raw-by-id";
	case 0x37: return "armour";
	default:   return "";
	}
}

void PrintCommand(const roth::DbCommand &c, const char *indent)
{
	printf("%s%08x  %c%02x %-13s %6u (0x%06x)", indent, c.raw, c.ifNot ? '!' : ' ',
		c.opcode, OpName(c.opcode), c.operand, c.operand);
	// Text operands: say, choice, topic (GAME_dialogue.md, opcodes.gd: 5/8/15/16).
	if (c.opcode == 0x05 || c.opcode == 0x08 || c.opcode == 0x0f || c.opcode == 0x10)
		printf("  %s", Text400(c.operand).c_str());
	printf("\n");
}

void PrintItem(const roth::Dbase100 &db, int id, bool full)
{
	const roth::DbItem *it = db.ItemSlot(id);
	if (!it) { printf("  item %3d  (none)\n", id); return; }
	printf("  item %3d  @0x%06x len %4u  render 0x%03x  flags 0x%02x  type 0x%02x (cat %u)  +6 0x%02x  +7 0x%02x"
		"  closeup 0x%x  icon 0x%x  name 0x%x%s  %s\n",
		id, it->offset, it->length, it->renderId, it->flags, it->itemType, it->Category(),
		it->unk06, it->runtimeAtLoad, it->closeup, it->icon, it->name,
		it->ok ? "" : "  ** MALFORMED **", Text400(it->name).c_str());
	for (const auto &b : it->blocks)
	{
		printf("      block %-13s (0x%02x) %3d cmds @0x%06x\n", roth::DbTriggerName(b.trigger),
			b.trigger, b.commandCount, b.offset);
		if (full)
			for (const auto &c : db.Commands(b)) PrintCommand(c, "        ");
	}
}

void PrintAction(const roth::Dbase100 &db, int id)
{
	if (id <= 0 || id > db.ActionCount()) { printf("  action %d out of range\n", id); return; }
	const roth::DbAction *a = db.Action(id);
	if (!a) { printf("  action %3d  (none or malformed)\n", id); return; }
	printf("  action %3d  @0x%06x len %4u  +2 0x%04x  %d cmds\n", id, a->offset, a->length,
		a->unk02, a->commandCount);
	for (const auto &c : db.Commands(*a)) PrintCommand(c, "      ");
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("usage: dbase100_dump <retail DATA dir | DBASE100.DAT> [--items] [--actions]"
			" [--cutscenes] [--interface] [--all] [--item N] [--action N]\n");
		return 2;
	}
	const std::string arg = argv[1];
	bool showItems = false, showActions = false, showCuts = false, showIface = false;
	std::vector<int> oneItems, oneActions;
	for (int i = 2; i < argc; i++)
	{
		const std::string o = argv[i];
		if (o == "--items") showItems = true;
		else if (o == "--actions") showActions = true;
		else if (o == "--cutscenes") showCuts = true;
		else if (o == "--interface") showIface = true;
		else if (o == "--all") showItems = showActions = showCuts = showIface = true;
		else if (o == "--item" && i + 1 < argc) oneItems.push_back(atoi(argv[++i]));
		else if (o == "--action" && i + 1 < argc) oneActions.push_back(atoi(argv[++i]));
		else { printf("unknown option %s\n", o.c_str()); return 2; }
	}

	roth::Dbase100 db;
	std::string dbPath, dataDir;
	if (EndsWithDat(arg))
	{
		dbPath = arg;
		dataDir = Dir(arg);
		auto bytes = roth::Install::ReadWholeFile(dbPath);
		if (bytes.empty()) { printf("FAIL: cannot read %s\n", dbPath.c_str()); return 1; }
		if (!db.Load(std::move(bytes))) { printf("FAIL: %s\n", db.Error().c_str()); return 1; }
	}
	else
	{
		// The engine's own path: Install, then the adapter.
		dataDir = arg;
		roth::Install install;
		if (!install.Open(arg.c_str()))
		{
			printf("FAIL: %s (point this at the folder holding ROTH.RES, or pass the .DAT itself)\n",
				install.Error().c_str());
			return 1;
		}
		dbPath = install.DataFile("DBASE100.DAT");
		roth::Log log;   // not begun: the adapter's report goes nowhere, we print our own
		if (!roth::LoadDbase100FromInstall(install, db, &log))
		{
			printf("FAIL: %s\n", db.Error().empty() ? "DBASE100.DAT not found or unreadable"
				: db.Error().c_str());
			return 1;
		}
	}

	const std::string textPath = FindSibling(dataDir, "DBASE400.DAT");
	if (!textPath.empty()) g_text = roth::Install::ReadWholeFile(textPath);

	const roth::DbHeader &h = db.Header();
	printf("file          %s (%u bytes)\n", dbPath.c_str(), (unsigned)db.Size());
	printf("text          %s\n", g_text.empty() ? "(no DBASE400.DAT beside it; names not shown)" : textPath.c_str());
	printf("\nheader\n");
	printf("  +0x08 file size   %10u%s\n", h.fileSize, h.fileSize == db.Size() ? "" : "  ** differs from the real size **");
	printf("  +0x0C unknown     %10u (0x%x)\n", h.unk0C, h.unk0C);
	printf("  +0x10 inventory   %10u  table @0x%x\n", h.inventoryCount, h.inventoryTable);
	printf("  +0x18 action      %10u  table @0x%x\n", h.actionCount, h.actionTable);
	printf("  +0x20 cutscene    %10u  table @0x%x\n", h.cutsceneCount, h.cutsceneTable);
	printf("  +0x28 interface   %10u  table @0x%x\n", h.interfaceCount, h.interfaceTable);
	printf("  +0x30             %10u\n", h.field30);

	// Counts the docs quote (ROTH_COMMANDS.md "their own numbering").
	printf("\nexpected by the docs\n");
	printf("  inventory  %4u  expected 281  %s\n", h.inventoryCount, h.inventoryCount == 281 ? "OK" : "MISMATCH");
	printf("  action     %4u  expected 694  %s\n", h.actionCount, h.actionCount == 694 ? "OK" : "MISMATCH");
	printf("  +0x30      %4u  expected   2  %s\n", h.field30, h.field30 == 2 ? "OK" : "MISMATCH");

	printf("\nflag bitmap size by candidate count (docs: 433 records -> 56 bytes)\n");
	const struct { const char *n; uint32_t v; } cands[] = {
		{ "+0x0C", h.unk0C }, { "inventory", h.inventoryCount }, { "action", h.actionCount },
		{ "cutscene", h.cutsceneCount }, { "interface", h.interfaceCount } };
	for (const auto &c : cands)
		printf("  %-10s %6u -> %4d bytes%s\n", c.n, c.v, roth::Dbase100::FlagBitmapBytes(c.v),
			c.v == 433 ? "   <-- 433" : "");

	printf("\nnotes\n");
	for (const auto &n : db.Notes()) printf("  %s\n", n.c_str());

	printf("\nanomalies (%d)\n", db.AnomalyTotal());
	if (db.Anomalies().empty()) printf("  none\n");
	for (const auto &kv : db.Anomalies()) printf("  %6d  %s\n", kv.second, kv.first.c_str());

	// Histograms.
	std::map<int, int> trig, cat, actOps, blockOps;
	int outOfRange = 0, zeroOperand = 0;
	for (int id = 1; id <= db.ItemCount(); id++)
	{
		const roth::DbItem *it = db.Item(id);
		if (!it) continue;
		cat[it->Category()]++;
		for (const auto &b : it->blocks)
		{
			trig[b.trigger]++;
			for (const auto &c : db.Commands(b)) blockOps[c.FullOpcode()]++;
		}
	}
	for (int id = 1; id <= db.ActionCount(); id++)
	{
		const roth::DbAction *a = db.Action(id);
		if (!a) continue;
		for (const auto &c : db.Commands(*a))
		{
			actOps[c.FullOpcode()]++;
			// execute_dbase100_chain dispatches 1..0x37 only (dbase100.c:641-652).
			if (c.opcode < 1 || c.opcode > 0x37) outOfRange++;
			if (c.operand == 0) zeroOperand++;
		}
	}
	printf("\nitem categories (low nibble of +5)\n ");
	for (auto &kv : cat) printf("  cat %d: %d", kv.first, kv.second);
	printf("\n\ntrigger blocks by code\n");
	for (auto &kv : trig) printf("  0x%02x %-13s %5d\n", kv.first, roth::DbTriggerName((uint8_t)kv.first), kv.second);
	printf("\naction opcodes (full byte, ! = If-NOT): %d outside the interpreter's 1..0x37, %d with operand 0 (skipped)\n",
		outOfRange, zeroOperand);
	for (auto &kv : actOps)
		printf("  %c%02x %-13s %6d\n", (kv.first & 0x80) ? '!' : ' ', kv.first & 0x7f,
			OpName((uint8_t)(kv.first & 0x7f)), kv.second);
	printf("\nitem block opcodes (full byte)\n");
	for (auto &kv : blockOps)
		printf("  %c%02x %-13s %6d\n", (kv.first & 0x80) ? '!' : ' ', kv.first & 0x7f,
			OpName((uint8_t)(kv.first & 0x7f)), kv.second);

	// ROTH_GAME_PORT.md §7: "What DBASE100 records 1-4 contain (dump them)."
	printf("\nrecords 1-4 (boot, quit, new game, death -- game_core.c:757, 802, 825, 971)\n");
	for (int id = 1; id <= 4; id++) PrintAction(db, id);

	// Spot values quoted by docs/GAME_inventory.md §10.
	printf("\nspot checks (docs/GAME_inventory.md §10)\n");
	{
		const int staff = db.FindItemByRenderId(0x294);
		printf("  render id 0x294 (Creator's Staff, quoted as \"index 11\"): id %d%s\n", staff,
			staff == 11 ? "  -> the quoted index is 1-based" : staff == 12 ? "  -> the quoted index is 0-based" : "");
		if (staff) PrintItem(db, staff, true);
		printf("    expect: OnInspect says DBASE400 0xF38C; a 0x20 operand 0x8002 (SetAmmoCap 6)\n");
		int nNotPick = 0;
		printf("  records with +4 == 0x80 exactly (the Dodger is one):");
		for (int id = 1; id <= db.ItemCount(); id++)
		{
			const roth::DbItem *it = db.Item(id);
			if (it && it->flags == 0x80) { printf(" %d", id); nNotPick++; }
		}
		printf("  (%d)\n", nNotPick);
		int adam = 0;
		for (int id = 1; id <= db.ItemCount() && !adam; id++)
		{
			const roth::DbItem *it = db.Item(id);
			if (it && (it->itemType & roth::DBT_ADAM)) adam = id;
		}
		printf("  first record with +5&0x10 (Adam): %d\n", adam);
	}

	if (showItems)
	{
		printf("\ninventory\n");
		for (int id = 1; id <= db.ItemCount(); id++) PrintItem(db, id, false);
	}
	for (int id : oneItems) { printf("\n"); PrintItem(db, id, true); }
	if (showActions)
	{
		printf("\nactions\n");
		for (int id = 1; id <= db.ActionCount(); id++) PrintAction(db, id);
	}
	for (int id : oneActions) { printf("\n"); PrintAction(db, id); }
	if (showCuts)
	{
		printf("\ncutscenes (op 0x07 plays operand-1, i.e. these indices)\n");
		int missing = 0;
		for (int i = 0; i < db.CutsceneCount(); i++)
		{
			const roth::DbCutscene *c = db.Cutscene(i);
			const bool have = !FindSibling(dataDir + "/GDV", (c->name + ".GDV").c_str()).empty();
			if (!have) missing++;
			printf("  %3d  %-8s +8 0x%04x  subs %5d @0x%06x  title 0x%06x  gallery %u  %s  %s\n",
				i, c->name.c_str(), c->unk08, c->subtitleSize, c->subtitleText, c->titleText,
				c->galleryAtLoad, have ? "GDV ok " : "GDV ?? ", Text400(c->titleText).c_str());
		}
		printf("  %d of %d names have no GDV\\<name>.GDV beside this file\n", missing, db.CutsceneCount());
	}
	if (showIface)
	{
		printf("\ninterface (0-based raw; whether resolve_dbase100_text biases the index is UNVERIFIED)\n");
		for (int i = 0; i < db.InterfaceCount(); i++)
		{
			uint32_t v = 0;
			db.InterfaceEntry(i, v);
			printf("  %3d (0x%02x)  0x%06x  %s\n", i, i, v, Text400(v).c_str());
		}
	}

	printf("\n%s\n", db.AnomalyTotal() == 0 ? "LOADED CLEAN" : "LOADED WITH ANOMALIES (see above)");
	return db.AnomalyTotal() == 0 ? 0 : 3;
}
