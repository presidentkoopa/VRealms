//
// Standalone check for the ROTH file readers. Builds and runs WITHOUT the
// engine, so the parsers can be proven before anything is wired into the
// map loader.
//
// It diffs against counts already validated by the Python pipeline across all
// 44 retail maps, so a regression shows up as a number mismatch rather than as
// something odd on screen later.
//
//   cl /std:c++17 /EHsc /Fe:roth_selftest.exe roth_selftest.cpp roth_raw.cpp
//   roth_selftest.exe "D:\...\Realms of the Haunting\ROTH"
//

#include "roth_raw.h"
#include "roth_commands.h"
#include "roth_das.h"
#include "roth_install.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <map>

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
	if (n > 0)
	{
		out.resize((size_t)n);
		if (fread(out.data(), 1, (size_t)n, f) != (size_t)n) out.clear();
	}
	fclose(f);
	return out;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("usage: roth_selftest <path to the game's ROTH folder>\n");
		return 2;
	}
	std::string rothDir = argv[1];

	// Exercise the same Install class the engine uses, rather than a private
	// copy of the manifest parser.
	roth::Install install;
	if (!install.Open(rothDir.c_str()))
	{
		printf("FAIL: %s\n", install.Error().c_str());
		return 1;
	}
	printf("install      %s\n", install.Path().c_str());
	printf("shared pack  %s      maps listed  %d\n\n",
		install.SharedPack().c_str(), (int)install.MapNames().size());

	std::map<std::string, std::string> maps;
	for (auto &n : install.MapNames())
		maps[n] = install.PackFor(n.c_str());

	// Totals the Python pipeline already produced for the retail game.
	const int EXPECT_MAPS = 44;
	const int EXPECT_SECTORS = 16906;
	// The Python oracle's totals for the command section, over all 44 maps.
	const int EXPECT_COMMANDS = 5531;
	const int EXPECT_TRIGGERS = 1876;
	const int EXPECT_DEAD_KEYS = 19;   // see the note by the print below
	const int EXPECT_ENTRIES  = 1937;   // entry POINTS, not chains -- ROTH_COMMANDS.md mislabelled this
	const int EXPECT_FACES = 82210;
	const int EXPECT_OBJECTS = 5324;
	const int EXPECT_PLATFORMS = 2299;

	int nMaps = 0, nSectors = 0, nFaces = 0, nObjects = 0, nPlatforms = 0;
	int nOpenLoops = 0, nFailed = 0;
	int nCommands = 0, nTriggers = 0, nEntries = 0;
	// Geometry opcodes only -- flags, items and dialogue key off DBASE100 ids,
	// and 0x17/0x38/0x40 take a command index, so none of those should resolve.
	int nGeomKeys = 0, nGeomResolved = 0;
	// Every entry point in every map is walked and executed, to prove the spine
	// terminates on real data and to see which opcodes the game actually reaches.
	int nChainsRun = 0, nChainsGated = 0, nStepsRun = 0;
	int opSeen[256] = {0};

	for (auto &entry : maps)
	{
		const std::string &name = entry.first;
		auto bytes = ReadFile(install.MapFile(name.c_str()));
		if (bytes.empty()) continue;      // listed but not shipped

		roth::Map m = roth::ParseRaw(bytes.data(), bytes.size());
		if (!m.ok())
		{
			printf("  %-10s PARSE FAILED: %s\n", name.c_str(), m.error.c_str());
			nFailed++;
			continue;
		}

		nMaps++;
		nSectors += (int)m.sectors.size();
		nCommands += (int)m.commands.size();
		for (const auto &c : m.commands) if (c.isTrigger) nTriggers++;
		nEntries += (int)m.entryPoints.size();
		for (uint16_t ep : m.entryPoints)
		{
			if (roth::WalkChainFlow(m, ep) != 0) { nChainsGated++; continue; }
			roth::Handlers h;
			h.Run = [&](const roth::Map &, const roth::Command &c, int) -> int
			{
				opSeen[c.opcode & 0x7f]++;
				nStepsRun++;
				return roth::CMD_NOTHING;
			};
			roth::ExecChain(m, ep, h);
			nChainsRun++;
		}
		for (const auto &c : m.commands)
		{
			switch (c.opcode)
			{
			case 0x18: case 0x32: case 0x13: case 0x19: case 0x1A: case 0x31:
			case 0x2F: case 0x34: case 0x07: case 0x1D: case 0x09: case 0x0A:
				if (c.key == 0) break;          // resolved at runtime
				nGeomKeys++;
				if (c.sector >= 0 || !c.faces.empty()) nGeomResolved++;
				break;
			default: break;
			}
		}
		nFaces += (int)m.faces.size();
		nPlatforms += (int)m.platforms.size();
		for (auto &list : m.objects) nObjects += (int)list.size();

		// Structural check: a sector's faces must form a closed loop, i.e. every
		// vertex it touches is used an even number of times. This is what makes
		// the geometry expressible as sectors at all.
		for (auto &s : m.sectors)
		{
			std::map<int, int> used;
			for (int j = 0; j < s.faceCount; j++)
			{
				int fi = s.firstFaceIndex + j;
				if (fi < 0 || fi >= (int)m.faces.size()) continue;
				used[m.faces[fi].vertex1]++;
				used[m.faces[fi].vertex2]++;
			}
			for (auto &kv : used)
				if (kv.second % 2) { nOpenLoops++; break; }
		}
	}

	// ---- artwork packs ----------------------------------------------------
	struct PackCheck { const char *name; int entries, images, meshes; };
	// Counts the Python extractor produced for these packs.
	const PackCheck packChecks[] = {
		{ "DEMO",  4382, 813, 16 },
		{ "ADEMO",  778, 420,  0 },
	};
	int packFails = 0;

	for (auto &pc : packChecks)
	{
		auto bytes = ReadFile(install.PackFile(pc.name));
		roth::Pack pack;
		if (bytes.empty() || !pack.Load(bytes.data(), bytes.size()))
		{
			printf("  %-6s FAILED TO LOAD: %s\n", pc.name, pack.Error().c_str());
			packFails++;
			continue;
		}

		int images = 0, meshes = 0;
		for (int i = 0; i < pack.Count(); i++)
		{
			const roth::FatEntry *e = pack.Entry(i);
			if (e->kind == roth::EntryKind::Object3D)
			{
				if (pack.ReadMesh(i).ok()) meshes++;
			}
			else if (e->kind != roth::EntryKind::Empty
				&& e->kind != roth::EntryKind::Indirection)
			{
				if (pack.ReadImage(i).ok()) images++;
			}
		}

		bool ok = pack.Count() == pc.entries && images == pc.images
			&& meshes == pc.meshes;
		printf("  %-6s entries %5d/%-5d  images %4d/%-4d  meshes %3d/%-3d  %s\n",
			pc.name, pack.Count(), pc.entries, images, pc.images,
			meshes, pc.meshes, ok ? "OK" : "MISMATCH");
		if (!ok) packFails++;
	}

	printf("\n");
	printf("maps parsed     %6d   expected %6d  %s\n", nMaps, EXPECT_MAPS,
		nMaps == EXPECT_MAPS ? "OK" : "MISMATCH");
	printf("sectors         %6d   expected %6d  %s\n", nSectors, EXPECT_SECTORS,
		nSectors == EXPECT_SECTORS ? "OK" : "MISMATCH");
	printf("commands        %6d   expected %6d  %s\n", nCommands, EXPECT_COMMANDS,
		nCommands == EXPECT_COMMANDS ? "OK" : "MISMATCH");
	{
		int distinct = 0;
		for (int i = 0; i < 256; i++) if (opSeen[i]) distinct++;
		printf("chains run      %6d   gated %6d   steps %6d   opcodes reached %d\n",
			nChainsRun, nChainsGated, nStepsRun, distinct);
	}
	printf("triggers        %6d   expected %6d  %s\n", nTriggers, EXPECT_TRIGGERS,
		nTriggers == EXPECT_TRIGGERS ? "OK" : "MISMATCH");
	printf("entry points    %6d   expected %6d  %s\n", nEntries, EXPECT_ENTRIES,
		nEntries == EXPECT_ENTRIES ? "OK" : "MISMATCH");
	// 19 of the retail maps' geometry keys match no sector and no wall, and that
	// is FAITHFUL rather than a gap. Opcode 0x13's init handler in the original
	// is mark_geometry_records_by_id (raw_commands.c:4994): it scans sectors for
	// commandID == key and ORs a flag into the matches. No match means nothing
	// gets marked and the trigger is inert -- ROTH.C resolves these to nothing
	// either. They are dead keys left behind by level editing: 15 of the 19 are
	// opcode 0x13, two are already flagged disabled, and one carries key 0xFFFC,
	// sitting right beside the door sentinels.
	//
	// Pinned so the number cannot quietly grow. If it does, the resolver broke.
	printf("geometry keys   %6d   resolved %6d   dead %3d (expect %d)  %s\n",
		nGeomKeys, nGeomResolved, nGeomKeys - nGeomResolved, EXPECT_DEAD_KEYS,
		nGeomKeys - nGeomResolved == EXPECT_DEAD_KEYS ? "OK" : "CHANGED");
	printf("faces           %6d   expected %6d  %s\n", nFaces, EXPECT_FACES,
		nFaces == EXPECT_FACES ? "OK" : "MISMATCH");
	printf("objects         %6d   expected %6d  %s\n", nObjects, EXPECT_OBJECTS,
		nObjects == EXPECT_OBJECTS ? "OK" : "MISMATCH");
	printf("mid-platforms   %6d   expected %6d  %s\n", nPlatforms, EXPECT_PLATFORMS,
		nPlatforms == EXPECT_PLATFORMS ? "OK" : "MISMATCH");
	printf("open loops      %6d   expected      0  %s\n", nOpenLoops,
		nOpenLoops == 0 ? "OK" : "MISMATCH");
	printf("parse failures  %6d   expected      0  %s\n", nFailed,
		nFailed == 0 ? "OK" : "MISMATCH");

	printf("pack checks     %6s   expected     ok  %s\n",
		packFails ? "failed" : "ok", packFails == 0 ? "OK" : "MISMATCH");

	bool pass = nMaps == EXPECT_MAPS && nSectors == EXPECT_SECTORS
		&& nFaces == EXPECT_FACES && nObjects == EXPECT_OBJECTS
		&& nPlatforms == EXPECT_PLATFORMS && nOpenLoops == 0 && nFailed == 0
		&& packFails == 0
		&& nCommands == EXPECT_COMMANDS && nTriggers == EXPECT_TRIGGERS
		&& nEntries == EXPECT_ENTRIES
		&& nGeomKeys - nGeomResolved == EXPECT_DEAD_KEYS;
	printf("\n%s\n", pass ? "ALL CHECKS PASS" : "*** CHECKS FAILED ***");
	return pass ? 0 : 1;
}
