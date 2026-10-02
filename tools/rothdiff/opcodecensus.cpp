// opcodecensus.cpp -- how much of Realms' level logic this port can actually run,
// counted over all 44 retail maps, offline.
//
// WHY THIS EXISTS
//
// roth_runtime.cpp runs a command chain by dispatching on each record's opcode.
// It already knows which opcodes it implements (`IsImplemented`), which are
// genuine no-ops in the original (`IsVerifiedNop`), and it counts whatever is
// left in `g.unhandledOps` -- but it only reports that for the ONE map that is
// loaded, which means seeing the whole picture costs 44 launches.
//
// The launch budget is the scarcest thing on this project, so this does it from
// the files. The question it answers is the one that decides when the game layer
// can start: of every command record in the shipping game, what share can this
// port execute today, which opcodes are the biggest holes, and which map is the
// most and least ready.
//
// A WARNING ABOUT THE TWO LISTS BELOW
//
// They MIRROR roth_runtime.cpp's `IsImplemented` and `IsVerifiedNop`, which are
// in an anonymous namespace in an engine translation unit and cannot be linked
// to from here. That is a duplication and it can drift. Two things guard it:
// this tool PRINTS both lists every run, so a disagreement is visible rather
// than silent, and the counts are reported as "by this tool's table" rather
// than as ground truth. If you change the dispatcher, change these.
//
// It reads the player's own .RAW files through the same reader the loader uses,
// and writes nothing.
//
// Build:
//   cl /nologo /EHsc /std:c++17 /O2 /I ..\..\src\roth /Fe:opcodecensus.exe \
//       /Fo:obj_oc\ opcodecensus.cpp ../../src/roth/roth_raw.cpp \
//       ../../src/roth/roth_install.cpp

#include "roth_raw.h"
#include "roth_install.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}

// Mirrors roth_runtime.cpp IsImplemented(). See the warning at the top.
static const uint8_t kImplemented[] = {
	0x02, 0x03, 0x06, 0x07, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	0x11, 0x12, 0x17, 0x1D, 0x23, 0x24, 0x26, 0x28, 0x2F, 0x34,
	0x36, 0x38, 0x3B, 0x3E, 0x40,
	// Reached only through RunCommandWithChain, so it is dispatched but is not
	// in IsImplemented's list -- counted here because the player gets it.
	0x2B,
};

// Mirrors roth_runtime.cpp IsVerifiedNop(). The triggers in it are not "missing":
// a trigger's INSTRUCTION slot is the nop in the original too, and the trigger
// itself is registered at load. Counting them as holes would overstate the gap.
static const uint8_t kNopReserved[] = {
	0x00, 0x01, 0x04, 0x05, 0x2C, 0x37, 0x43, 0x44, 0x47,
};
static const uint8_t kNopTrigger[] = {
	0x08, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x25, 0x30, 0x31, 0x32, 0x39, 0x3D,
};

static bool In(const uint8_t *a, size_t n, uint8_t v)
{
	for (size_t i = 0; i < n; i++) if (a[i] == v) return true;
	return false;
}
#define INSET(arr, v) In(arr, sizeof(arr), v)

struct MapRow
{
	std::string name;
	long long total = 0, impl = 0, nop = 0, trig = 0, miss = 0;
	double Ready() const
	{
		const long long live = total - nop - trig;
		return live > 0 ? 100.0 * double(impl) / double(live) : 100.0;
	}
};

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: opcodecensus <ROTH folder> [-v]\n"); return 2; }
	const bool verbose = (argc > 2 && std::string(argv[2]) == "-v");
	if (!roth::TheInstall().Open(argv[1])) { printf("install: bad\n"); return 2; }

	std::map<int, long long> perOp;             // every opcode, how many records
	std::map<int, std::set<std::string>> opMaps; // ... and in how many maps
	std::vector<MapRow> rows;
	long long total = 0, impl = 0, nopR = 0, trig = 0, miss = 0;
	long long disabled = 0, triggerRecs = 0, chains = 0;

	for (const std::string &name : roth::TheInstall().MapNames())
	{
		std::vector<uint8_t> d = ReadFile(roth::TheInstall().MapFile(name.c_str()));
		if (d.empty()) continue;
		roth::Map m = roth::ParseRaw(d.data(), d.size());
		if (!m.ok()) { printf("  %-10s PARSE FAILED\n", name.c_str()); continue; }

		MapRow row;
		row.name = name;
		for (const roth::Command &c : m.commands)
		{
			// The 0x80 bit is editor metadata the exec loop masks off
			// (raw_commands.c:1256).
			const uint8_t op = (uint8_t)(c.opcode & 0x7f);
			perOp[op]++;
			opMaps[op].insert(name);
			row.total++;
			total++;
			if (c.disabled) disabled++;
			if (c.isTrigger) { triggerRecs++; if (c.chainStart != 0) chains++; }

			if (INSET(kImplemented, op))     { row.impl++; impl++; }
			else if (INSET(kNopTrigger, op)) { row.trig++; trig++; }
			else if (INSET(kNopReserved, op)){ row.nop++;  nopR++; }
			else                             { row.miss++; miss++; }
		}
		rows.push_back(row);
	}

	printf("command records across %u maps\n", (unsigned)rows.size());
	printf("  total                     %8lld\n", total);
	printf("  trigger records           %8lld  (%lld naming a chain)\n", triggerRecs, chains);
	printf("  disabled (modifier 0x08)  %8lld\n", disabled);
	printf("\nby what the dispatcher does with them\n");
	printf("  IMPLEMENTED               %8lld\n", impl);
	printf("  trigger slots (nop in the original too)   %8lld\n", trig);
	printf("  reserved / cmd_default_nop               %8lld\n", nopR);
	printf("  UNHANDLED                 %8lld\n", miss);
	{
		const long long live = total - trig - nopR;
		printf("\n  of the %lld records that must actually DO something,"
			" %lld run: %.1f%%\n", live, impl, live > 0 ? 100.0 * double(impl) / double(live) : 100.0);
	}

	printf("\nthe holes, biggest first  (opcode, records, maps affected)\n");
	{
		std::vector<std::pair<long long, int>> v;
		for (const auto &kv : perOp)
			if (!INSET(kImplemented, (uint8_t)kv.first) &&
				!INSET(kNopTrigger, (uint8_t)kv.first) &&
				!INSET(kNopReserved, (uint8_t)kv.first))
				v.push_back({ kv.second, kv.first });
		std::sort(v.begin(), v.end(), [](const std::pair<long long, int> &a,
			const std::pair<long long, int> &b) { return a.first > b.first; });
		if (v.empty()) printf("    none -- every opcode in the retail data is handled\n");
		for (const auto &p : v)
			printf("    0x%02x  %6lld records  %2u maps\n",
				p.second, p.first, (unsigned)opMaps[p.second].size());
	}

	printf("\nwhat IS implemented, by weight\n");
	{
		std::vector<std::pair<long long, int>> v;
		for (const auto &kv : perOp)
			if (INSET(kImplemented, (uint8_t)kv.first)) v.push_back({ kv.second, kv.first });
		std::sort(v.begin(), v.end(), [](const std::pair<long long, int> &a,
			const std::pair<long long, int> &b) { return a.first > b.first; });
		for (const auto &p : v)
			printf("    0x%02x  %6lld records  %2u maps\n",
				p.second, p.first, (unsigned)opMaps[p.second].size());
	}

	// Which maps are ready and which are not, because "start the game layer"
	// means starting on SOME map and the first one should be a ready one.
	printf("\nper map, least ready first  (of records that must do something)\n");
	{
		std::vector<MapRow> s = rows;
		std::sort(s.begin(), s.end(), [](const MapRow &a, const MapRow &b)
			{ return a.Ready() < b.Ready(); });
		for (size_t i = 0; i < s.size(); i++)
		{
			if (!verbose && i >= 10 && i + 5 < s.size()) continue;
			printf("    %-10s %5.1f%%   %4lld run / %4lld live   %4lld unhandled"
				"   (%lld records)\n", s[i].name.c_str(), s[i].Ready(),
				s[i].impl, s[i].total - s[i].trig - s[i].nop, s[i].miss, s[i].total);
		}
		if (!verbose && s.size() > 15) printf("    ... -v for all %u\n", (unsigned)s.size());
	}

	// The tables this run used, so drift from the dispatcher is visible.
	printf("\nthe tables this tool used (mirror roth_runtime.cpp -- see the header)\n");
	printf("  implemented  :");
	for (size_t i = 0; i < sizeof(kImplemented); i++) printf(" %02x", kImplemented[i]);
	printf("\n  trigger nops :");
	for (size_t i = 0; i < sizeof(kNopTrigger); i++) printf(" %02x", kNopTrigger[i]);
	printf("\n  reserved nops:");
	for (size_t i = 0; i < sizeof(kNopReserved); i++) printf(" %02x", kNopReserved[i]);
	printf("\n");
	return 0;
}
