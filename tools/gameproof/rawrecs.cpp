//
// rawrecs.cpp -- list the RAW command records the game core answers, from the
// retail maps: opcodes 0x27, 0x29, 0x2a, 0x2b, 0x42.
//
//   rawrecs "<retail DATA folder>/M" > rawrecs.txt
//
// One line per DISTINCT record: "op modifier fireFlags key pendingKey count"
// (hex hex hex dec dec dec). pendingKey is the action id of the 0x36 record
// that precedes a 0x27 in its map (0x36 latches itself as the pending record,
// raw_commands.c:514; the if-next tail then runs it as a 0x2b, :4653), 0 for
// none. Each 0x27 is listed both with and without it. The output is derived
// from retail data: keep it out of the repo.
//

#include "../../src/roth/roth_raw.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <tuple>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: rawrecs <folder of .RAW maps>\n"); return 2; }
	std::vector<std::string> names;
	if (DIR *d = opendir(argv[1]))
	{
		while (dirent *e = readdir(d))
		{
			const size_t n = strlen(e->d_name);
			if (n > 4 && strcasecmp(e->d_name + n - 4, ".RAW") == 0) names.push_back(e->d_name);
		}
		closedir(d);
	}
	std::map<std::tuple<int, int, int, int, int>, int> seen;
	int maps = 0;
	for (const std::string &name : names)
	{
		std::vector<uint8_t> data;
		if (FILE *f = fopen((std::string(argv[1]) + "/" + name).c_str(), "rb"))
		{
			fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
			if (n > 0) { data.resize((size_t)n); if (fread(data.data(), 1, (size_t)n, f) != (size_t)n) data.clear(); }
			fclose(f);
		}
		roth::Map m = roth::ParseRaw(data.data(), data.size());
		if (!m.ok()) { fprintf(stderr, "rawrecs: %s did not parse\n", name.c_str()); continue; }
		maps++;
		int lastPending = 0;
		for (const roth::Command &c : m.commands)
		{
			if (c.opcode == 0x36) lastPending = c.key;
			if (c.opcode != 0x27 && c.opcode != 0x29 && c.opcode != 0x2a && c.opcode != 0x2b && c.opcode != 0x42) continue;
			seen[std::make_tuple((int)c.opcode, (int)c.modifier, (int)c.fireFlags, (int)c.key, 0)]++;
			if (c.opcode == 0x27 && lastPending != 0)
				seen[std::make_tuple((int)c.opcode, (int)c.modifier, (int)c.fireFlags, (int)c.key, lastPending)]++;
		}
	}
	for (const auto &kv : seen)
		printf("%02x %02x %02x %d %d %d\n", std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first),
			std::get<3>(kv.first), std::get<4>(kv.first), kv.second);
	fprintf(stderr, "rawrecs: %d maps, %d distinct records\n", maps, (int)seen.size());
	return maps > 0 ? 0 : 1;
}
