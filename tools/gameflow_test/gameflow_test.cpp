//
// Game flow and persistence, engine-free, against the retail data and against
// dumps taken from the running original.
//
//   gameflow_test "<retail DATA folder>" [options]
//
//   --oracle <file>     a dump written by the ROTH.C probe mod (see README.md):
//                       every "TAG key value" line this port can produce is
//                       produced and compared, and every warp the original
//                       performed is recomputed from the RAW files and compared
//   --sav <file>        an original SAVE<n>.SAV: parsed, imported, and its
//                       global chunks compared byte for byte with this port's
//   --tmp <file>        an original <MAP>.TMP: its sections are measured
//   --census            list every RAW 0x3b record of every map
//   --warp-script <f>   write a probe script that makes the original perform
//                       every distinct (map, key) arrival in the game
//
// Without a retail folder it prints SKIPPED and exits 0. Writes nothing but
// the file named by --warp-script.
//

#include "../../src/roth/roth_gameflow.h"
#include "../../src/roth/roth_raw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <dirent.h>

using namespace roth;

namespace
{

int g_checks = 0, g_failed = 0;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

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

std::string Upper(std::string s) { for (char &c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return s; }

// The interpreter's host: the flow answers its own calls, this records the rest.
struct Host : FlowInterpSink
{
	std::vector<uint32_t> sfx, images;
	uint32_t PlaySoundEffect(uint32_t id, uint32_t) override { sfx.push_back(id); return 0; }
	uint32_t ShowFullscreenImage(uint32_t operand) override { images.push_back(operand); return FlowInterpSink::ShowFullscreenImage(operand); }
};

// One whole game state, as a test holds it.
struct Game
{
	Dbase100 db;
	Host host;
	GameCore core;
	GameFlow flow;

	bool Start(const std::vector<uint8_t> &file)
	{
		if (!db.Load(file.data(), file.size())) return false;
		core.Attach(&db, &host, nullptr);
		flow.Attach(&core, &db, nullptr);
		host.flow = &flow;
		return core.IsAttached() && flow.IsAttached();
	}
};

std::string Hex(const uint8_t *p, size_t n)
{
	static const char *d = "0123456789abcdef";
	std::string s;
	for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
	return s;
}

// The lines of the probe's dump this port can state, in the probe's format.
std::map<std::string, std::string> DumpLines(Game &g)
{
	std::map<std::string, std::string> out;
	char buf[256];
	const std::vector<uint8_t> &bm = g.core.Interp().State().g_dbase100_record_bitmap;
	out["flags"] = Hex(bm.data(), bm.size());
	snprintf(buf, sizeof buf, "%d", g.core.Items().Count());
	out["invcount"] = buf;
	for (int i = 0; i < Inventory::SLOT_COUNT; i++)
	{
		const InventorySlot &s = g.core.Items().Slot(i);
		if (s.id == 0 && s.quantity == 0) continue;
		snprintf(buf, sizeof buf, "id=%u b2=%u b3=%u", s.id, s.quantity & 0xff, s.quantity >> 8);
		out["slot " + std::to_string(i)] = buf;
	}
	snprintf(buf, sizeof buf, "primary=%d secondary=%d", g.core.Items().RightHand(), g.core.Items().LeftHand());
	out["hands"] = buf;
	out["cursorpos"] = Hex(g.flow.CursorPos(), GameFlow::CURSOR_BYTES);
	out["cursorscroll"] = Hex(g.flow.CursorScroll(), GameFlow::CURSOR_BYTES);
	snprintf(buf, sizeof buf, "%u", g.flow.CutscenesSeenCount());
	out["seencount"] = buf;
	snprintf(buf, sizeof buf, "%u", g.flow.MusicChunk());
	out["music"] = buf;
	std::string gal;
	for (int i = 0; i < g.db.CutsceneCount(); i++)
		if (g.db.CutsceneGallery(i)) { gal += " " + std::to_string(i) + ":" + std::to_string(g.db.CutsceneGallery(i)); }
	out["gallery"] = gal.empty() ? "" : gal.substr(1);
	snprintf(buf, sizeof buf, "%d", g.flow.Health());
	out["health"] = buf;
	snprintf(buf, sizeof buf, "%d", g.flow.MaxHealth());
	out["maxhealth"] = buf;
	snprintf(buf, sizeof buf, "%u", g.flow.CurrentMode());
	out["mode"] = buf;
	return out;
}

// The same keys out of the probe's file, for one tag.
std::map<std::string, std::string> OracleLines(const std::vector<std::string> &lines, const std::string &tag)
{
	std::map<std::string, std::string> out;
	for (const std::string &l : lines)
	{
		if (l.compare(0, tag.size() + 1, tag + " ") != 0) continue;
		const std::string rest = l.substr(tag.size() + 1);
		char a[64] = "";
		sscanf(rest.c_str(), "%63s", a);
		const std::string key = a;
		std::string val = rest.size() > key.size() ? rest.substr(key.size() + 1) : "";
		if (key == "slot")
		{
			int n = 0; char v[128] = "";
			sscanf(val.c_str(), "%d %127[^\n]", &n, v);
			out["slot " + std::to_string(n)] = v;
		}
		else if (key == "seencount")
		{
			unsigned seen = 0, music = 0;
			sscanf(val.c_str(), "%u music %u", &seen, &music);
			out["seencount"] = std::to_string(seen);
			out["music"] = std::to_string(music);
		}
		else if (key == "health")
		{
			int h = 0, mx = 0;
			sscanf(val.c_str(), "%d max %d", &h, &mx);
			out["health"] = std::to_string(h);
			out["maxhealth"] = std::to_string(mx);
		}
		else out[key] = val;
	}
	return out;
}

int CompareDump(Game &g, const std::vector<std::string> &oracle, const std::string &tag)
{
	const std::map<std::string, std::string> ours = DumpLines(g);
	const std::map<std::string, std::string> theirs = OracleLines(oracle, tag);
	if (theirs.empty()) { printf("  oracle tag %s: NOT IN THE DUMP\n", tag.c_str()); g_failed++; return 0; }
	int same = 0, diff = 0;
	for (const auto &kv : ours)
	{
		auto it = theirs.find(kv.first);
		const std::string t = it == theirs.end() ? std::string("(absent)") : it->second;
		if (t == kv.second) same++;
		else { diff++; printf("  DIFF %s %s: original \"%s\"  port \"%s\"\n", tag.c_str(), kv.first.c_str(), t.c_str(), kv.second.c_str()); }
	}
	for (const auto &kv : theirs)
		if (kv.first.compare(0, 5, "slot ") == 0 && ours.find(kv.first) == ours.end())
		{ diff++; printf("  DIFF %s %s: original \"%s\"  port (empty)\n", tag.c_str(), kv.first.c_str(), kv.second.c_str()); }
	g_checks++;
	if (diff) g_failed++;
	printf("  oracle tag %-10s %d values equal, %d different\n", tag.c_str(), same, diff);
	return diff;
}

struct LoadedMap { std::string name; Map map; };

} // namespace

int main(int argc, char **argv)
{
	std::string dir = "/mnt/user-data/uploads/Realms of the Haunting/DATA";
	std::string oraclePath, savPath, tmpPath, warpScript;
	bool census = false;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--oracle" && i + 1 < argc) oraclePath = argv[++i];
		else if (a == "--sav" && i + 1 < argc) savPath = argv[++i];
		else if (a == "--tmp" && i + 1 < argc) tmpPath = argv[++i];
		else if (a == "--warp-script" && i + 1 < argc) warpScript = argv[++i];
		else if (a == "--census") census = true;
		else dir = a;
	}

	const std::vector<uint8_t> dbFile = ReadFile(dir + "/DBASE100.DAT");
	if (dbFile.empty()) { printf("gameflow_test: SKIPPED (no DBASE100.DAT in %s)\n", dir.c_str()); return 0; }

	std::vector<std::string> oracle;
	if (!oraclePath.empty())
	{
		const std::vector<uint8_t> o = ReadFile(oraclePath);
		std::string line;
		for (uint8_t c : o) { if (c == '\n') { oracle.push_back(line); line.clear(); } else if (c != '\r') line += (char)c; }
		if (!line.empty()) oracle.push_back(line);
		printf("oracle dump: %s, %d lines\n", oraclePath.c_str(), (int)oracle.size());
	}

	// ------------------------------------------------------------------
	// 1. The byte stream refuses to read out of bounds.
	// ------------------------------------------------------------------
	{
		SaveWriter w;
		w.U8(0x12); w.U16(0x3456); w.U32(0x789abcde); w.I32(-5); w.Str("STUDY1");
		const size_t m = w.BeginChunk(7); w.U32(1); w.U32(2); w.EndChunk(m);
		SaveReader r(w.Data().data(), w.Data().size());
		CHECK(r.U8() == 0x12 && r.U16() == 0x3456 && r.U32() == 0x789abcde && r.I32() == -5);
		CHECK(r.Str() == "STUDY1");
		uint16_t id = 0; SaveReader c(nullptr, 0);
		CHECK(r.NextChunk(id, c) && id == 7 && c.Remaining() == 8 && c.U32() == 1 && c.U32() == 2);
		CHECK(r.AtEnd() && r.Ok());
		CHECK(r.U32() == 0 && !r.Ok());
		const uint8_t bad[] = { 0xff, 0xff, 0xff, 0x7f, 'x' };      // a length far past the end
		SaveReader b(bad, sizeof bad);
		CHECK(b.Str().empty() && !b.Ok());
	}

	// ------------------------------------------------------------------
	// 2. Boot and new game, against the original's dump.
	// ------------------------------------------------------------------
	Game g;
	CHECK(g.Start(dbFile));
	{
		// Nothing in the retail file starts "examined" or in the gallery, so
		// ResetForMenu's "back to the file" is "clear the bit".
		int preset = 0;
		for (int id = 1; id <= g.db.ItemCount(); id++) if (g.db.ItemRuntime(id) & 0x80) preset++;
		for (int i = 0; i < g.db.CutsceneCount(); i++) if (g.db.CutsceneGallery(i)) preset++;
		CHECK(preset == 0);
		CHECK(g.db.Header().field30 == 2);      // or op 0x07 plays nothing (dbase100.c:936)
	}

	g.flow.Boot();
	printf("boot: movies");
	for (const std::string &m : g.flow.MovieLog()) printf(" %s", m.c_str());
	printf(", music %u, health %d, mode %u\n", g.flow.MusicChunk(), g.flow.Health(), g.flow.CurrentMode());
	CHECK(g.flow.MovieLog().size() == 2 && g.flow.MovieLog()[0] == "GREMLOGO" && g.flow.MovieLog()[1] == "INTRO");
	CHECK(g.db.CutsceneGallery(54) == 1 && g.db.CutsceneGallery(53) == 2);
	CHECK(g.flow.CutscenesSeenCount() == 3);
	CHECK(g.flow.MusicChunk() == 0x294411);
	CHECK(g.flow.Health() == 0x800);
	CHECK(g.flow.CurrentMode() == GameFlow::MODE_BETWEEN);
	CHECK(g.flow.CutsceneOverlayActive() == 0);     // finish_dialogue_record_eval took the overlay down

	g.flow.ClearMovieLog();
	g.flow.NewGame();
	printf("new game: movies");
	for (const std::string &m : g.flow.MovieLog()) printf(" %s", m.c_str());
	printf(", music %u, %d items, mode %u\n", g.flow.MusicChunk(), g.core.Items().Count(), g.flow.CurrentMode());
	CHECK(g.flow.MovieLog().size() == 1 && g.flow.MovieLog()[0] == "CHPTR1");
	CHECK(g.db.CutsceneGallery(150) == 3 && g.flow.CutscenesSeenCount() == 4);
	CHECK(g.flow.MusicChunk() == 0x293cad);
	CHECK(g.core.Items().Count() == 4 && g.core.Items().RightHand() == 3 && g.core.Items().LeftHand() == -1);
	CHECK(g.flow.CurrentMode() == GameFlow::MODE_PLAY);
	CHECK(g.flow.Health() == 0x800 && !g.flow.NoDeathWait());
	if (!oracle.empty()) CompareDump(g, oracle, "NEWGAME");

	// ------------------------------------------------------------------
	// 3. The start map, and every map's list entry.
	// ------------------------------------------------------------------
	std::vector<LoadedMap> maps;
	{
		const std::vector<uint8_t> res = ReadFile(dir + "/ROTH.RES");
		const std::vector<MapListEntry> list = ParseMapList(res.data(), res.size());
		CHECK(!list.empty());
		if (!list.empty())
		{
			printf("ROTH.RES: %d maps, the first is %s with pack %s\n", (int)list.size(), list[0].map.c_str(), list[0].pack.c_str());
			CHECK(list[0].map == "STUDY1" && list[0].pack == "DEMO");
		}
		// The RAW files actually present.
		std::map<std::string, std::string> files;
		if (DIR *d = opendir((dir + "/M").c_str()))
		{
			while (dirent *e = readdir(d))
			{
				const std::string n = Upper(e->d_name);
				if (n.size() > 4 && n.substr(n.size() - 4) == ".RAW") files[n.substr(0, n.size() - 4)] = dir + "/M/" + e->d_name;
			}
			closedir(d);
		}
		int listed = 0;
		for (const MapListEntry &e : list) if (files.count(e.map)) listed++;
		printf("ROTH.RES: %d of its %d maps have a RAW file; %d RAW files in M\n", listed, (int)list.size(), (int)files.size());
		CHECK(listed == (int)list.size());
		for (const auto &kv : files)
		{
			const std::vector<uint8_t> d = ReadFile(kv.second);
			LoadedMap lm;
			lm.name = kv.first;
			lm.map = ParseRaw(d.data(), d.size());
			if (!lm.map.ok()) { printf("  %s: PARSE FAILED\n", kv.first.c_str()); g_failed++; continue; }
			maps.push_back(lm);
		}
		CHECK(maps.size() == 44);
	}
	auto findMap = [&](const std::string &n) -> const Map *
	{
		for (const LoadedMap &m : maps) if (m.name == n) return &m.map;
		return nullptr;
	};

	// The start position the original reports for a new game is the first
	// map's own metadata (init_player_position_from_metadata, map_load.c:657).
	if (const Map *study = findMap("STUDY1"))
	{
		printf("STUDY1 metadata start: x %d y %d z %d rotation %d\n", study->metadata.startX, study->metadata.startY,
			study->metadata.startZ, study->metadata.rotation);
		for (const std::string &l : oracle)
		{
			int x, xf, y, yf, z, zf; unsigned ang, sec;
			if (sscanf(l.c_str(), "NEWGAME pos x=%d.%x y=%d.%x z=%d.%x angle=%u sector=%u", &x, &xf, &y, &yf, &z, &zf, &ang, &sec) == 8)
			{
				CHECK(x == study->metadata.startX && y == study->metadata.startY);
				CHECK((int)ang == (study->metadata.rotation & 0x1ff));
				const int off = (int)sec - (int)study->header.sectorsOffset;
				CHECK(off >= 0 && off % 0x1a == 0);
				printf("  original new game: x %d y %d z %d angle %u, sector record %u = sector index %d\n", x, y, z, ang, sec, off / 0x1a);
			}
		}
	}

	// ------------------------------------------------------------------
	// 4. Every RAW 0x3b record in the game: what it does and where it lands.
	// ------------------------------------------------------------------
	struct Dest { std::string map; uint16_t key; };
	std::vector<Dest> dests;
	{
		int total = 0, nothing = 0, inPlace = 0, sameNoMove = 0, change = 0, changeKey0 = 0;
		int noSuchMap = 0, noSuchSector = 0, oneShot = 0;
		std::set<std::string> seen;
		for (const LoadedMap &lm : maps)
		{
			for (size_t ci = 0; ci < lm.map.commands.size(); ci++)
			{
				const Command &c = lm.map.commands[ci];
				if ((c.opcode & 0x7f) != 0x3b) continue;
				total++;
				WarpRequest req;
				for (int k = 0; k < 8; k++) req.name[k] = (char)c.Byte(0x0a + k);       // raw_commands.c:527-528
				req.name[8] = 0;
				for (int k = 0; k < 8; k++) if (req.name[k] == 0) { for (int j = k; j < 8; j++) req.name[j] = 0; break; }
				req.key = c.key;                                                         // :530
				if (c.fireFlags & 0x10) oneShot++;
				const WarpKind kind = ClassifyWarp(req, lm.name.c_str());
				const std::string target = kind == WarpKind::ChangeMap ? WarpTargetName(req) : lm.name;
				const Map *tm = findMap(target);
				Arrival a;
				bool found = false;
				switch (kind)
				{
				case WarpKind::Nothing: nothing++; break;
				case WarpKind::SameMapNoMove: sameNoMove++; break;
				case WarpKind::RelocateInPlace:
					inPlace++;
					found = FindArrivalSector(lm.map, req.key, -1, a);
					if (!found) noSuchSector++;
					break;
				case WarpKind::ChangeMap:
					change++;
					if (tm == nullptr) { noSuchMap++; break; }
					if (req.key == 0) { changeKey0++; break; }
					found = FindArrivalSector(*tm, req.key, -1, a);
					if (!found) noSuchSector++;
					break;
				}
				if (census)
					printf("  %-9s #%-4d -> %-8s key %-5u %s%s", lm.name.c_str(), (int)ci + 1, req.name[0] ? req.name : "(same)", req.key,
						kind == WarpKind::Nothing ? "nothing" : kind == WarpKind::SameMapNoMove ? "same map, no move"
						: kind == WarpKind::RelocateInPlace ? "relocate in place" : "change map", (c.fireFlags & 0x10) ? " one-shot" : "");
				if (census && found) printf("  sector %d centre %d,%d floor %d", a.sector, a.x, a.y, a.floor);
				if (census && kind == WarpKind::ChangeMap && tm == nullptr) printf("  NO SUCH MAP");
				if (census) printf("\n");
				if (found || (kind == WarpKind::ChangeMap && tm != nullptr && req.key == 0))
				{
					const std::string k = target + ":" + std::to_string(req.key);
					if (seen.insert(k).second) dests.push_back({ target, req.key });
				}
			}
		}
		printf("RAW 0x3b: %d records. change map %d (of them key 0: %d, unknown map: %d), relocate in place %d, "
			"same map no move %d, nothing %d; one-shot %d; arrival key that names no sector %d; distinct arrivals %d\n",
			total, change, changeKey0, noSuchMap, inPlace, sameNoMove, nothing, oneShot, noSuchSector, (int)dests.size());
		CHECK(total > 0);
	}

	if (!warpScript.empty())
	{
		FILE *f = fopen(warpScript.c_str(), "w");
		if (f)
		{
			fprintf(f, "wait 20\n");
			for (const Dest &d : dests) fprintf(f, "warp %s %u\nwait 3\n", d.map.c_str(), d.key);
			fprintf(f, "quit\n");
			fclose(f);
			printf("wrote %s: %d warps\n", warpScript.c_str(), (int)dests.size());
		}
	}

	// Every warp the original performed, recomputed from the RAW files.
	if (!oracle.empty())
	{
		int compared = 0, equal = 0, liveFloorMoved = 0, heightsChecked = 0;
		char cur[64] = "", dest[64] = "";
		int key = 0, bx = 0, by = 0, bz = 0; unsigned bsec = 0;
		bool have = false;
		// The floors the ORIGINAL had in memory around the warp. A map's
		// load-time effects can move a floor before the player arrives
		// (DOMINION's first sector is 4 in the file and 2 in memory), so the
		// height rule is checked against these, and the file only against
		// x, y and the sector.
		int liveOld = 0x7fff, latch = 0, liveNew = 0x7fff;
		bool haveOld = false;
		struct Pending { bool on = false; WarpKind kind = WarpKind::Nothing; int bz = 0, ez = 0, fileNew = 0; bool relocated = false; unsigned bsec = 0; std::string what; } pend;
		for (const std::string &l : oracle)
		{
			if (sscanf(l.c_str(), "EVENT warp_floor old=%d latch76754=%d", &liveOld, &latch) == 2) { haveOld = true; continue; }
			if (sscanf(l.c_str(), "EVENT warp_floor new=%d", &liveNew) == 1)
			{
				if (pend.on && pend.relocated && haveOld)
				{
					// ml_relocate_found, map_load.c:518-520, in 16-bit words: the
					// old floor is the player's sector's, or -- with no sector,
					// which only this test's own key-0 hops produce -- whatever
					// the latch 0x76754 still holds.
					const int effOld = (pend.bsec != 0 && liveOld != 0x7fff) ? liveOld : latch;
					const int want = (int16_t)(uint16_t)(pend.bz - (int16_t)(uint16_t)(effOld - liveNew));
					heightsChecked++;
					if (want != pend.ez) { g_failed++; printf("  HEIGHT DIFF %s: original z %d, rule gives %d (old floor %d, new floor %d)\n", pend.what.c_str(), pend.ez, want, effOld, liveNew); }
					if (liveNew != pend.fileNew) liveFloorMoved++;
				}
				pend.on = false; haveOld = false;
				continue;
			}
			if (sscanf(l.c_str(), "EVENT warp_begin dest=%63s key=%d cur=%63s x=%d y=%d z=%d sec=%u", dest, &key, cur, &bx, &by, &bz, &bsec) == 7)
			{ have = true; continue; }
			if (sscanf(l.c_str(), "EVENT warp_begin dest= key=%d cur=%63s x=%d y=%d z=%d sec=%u", &key, cur, &bx, &by, &bz, &bsec) == 6)
			{ dest[0] = 0; have = true; continue; }
			unsigned ret = 0, ang = 0, esec = 0; int ex = 0, ey = 0, ez = 0; char after[64] = "";
			if (!have || sscanf(l.c_str(), "EVENT warp_end ret=%u cur=%63s x=%d y=%d z=%d ang=%u sec=%u", &ret, after, &ex, &ey, &ez, &ang, &esec) != 7) continue;
			have = false;
			if (key == -1) continue;             // the first load of a session, not a 0x3b
			// The loaded map's base name, from "m\study1.RAW".
			std::string curBase = cur;
			{ size_t p = curBase.find_last_of("\\/"); if (p != std::string::npos) curBase = curBase.substr(p + 1); }
			{ size_t p = curBase.find('.'); if (p != std::string::npos) curBase = curBase.substr(0, p); }
			curBase = Upper(curBase);
			std::string afterBase = after;
			{ size_t p = afterBase.find_last_of("\\/"); if (p != std::string::npos) afterBase = afterBase.substr(p + 1); }
			{ size_t p = afterBase.find('.'); if (p != std::string::npos) afterBase = afterBase.substr(0, p); }
			afterBase = Upper(afterBase);

			WarpRequest req;
			strncpy(req.name, dest, 8);
			req.key = (uint16_t)key;
			const WarpKind kind = ClassifyWarp(req, curBase.c_str());
			const Map *from = findMap(curBase);
			std::string target = curBase;
			if (kind == WarpKind::ChangeMap) target = WarpTargetName(req);
			const Map *to = findMap(target);
			compared++;
			bool ok = (target == afterBase) && from != nullptr && to != nullptr;
			int px = bx, py = by, pz = bz, psec = -1;
			if (ok)
			{
				// The sector the player stood in, from the original's record offset.
				const int fromIdx = bsec >= from->header.sectorsOffset ? (int)(bsec - from->header.sectorsOffset) / 0x1a : -1;
				const int oldFloor = (fromIdx >= 0 && fromIdx < (int)from->sectors.size() && bsec != 0) ? from->sectors[(size_t)fromIdx].floorHeight : 0;
				Arrival a;
				if (kind == WarpKind::RelocateInPlace)
				{
					if (FindArrivalSector(*from, req.key, fromIdx, a)) { px = a.x; py = a.y; pz = ArrivalHeight(bz, oldFloor, a.floor); psec = a.sector; }
				}
				else if (kind == WarpKind::ChangeMap && req.key != 0)
				{
					if (FindArrivalSector(*to, req.key, -1, a)) { px = a.x; py = a.y; pz = ArrivalHeight(bz, oldFloor, a.floor); psec = a.sector; }
				}
				ok = px == ex && py == ey;
				if (psec >= 0) ok = ok && (unsigned)(to->header.sectorsOffset + psec * 0x1a) == esec;
				else ok = ok && pz == ez;                 // nothing relocated: the height stands too
				pend.on = true; pend.kind = kind; pend.bz = bz; pend.ez = ez; pend.relocated = psec >= 0;
				pend.fileNew = psec >= 0 ? to->sectors[(size_t)psec].floorHeight : 0; pend.bsec = bsec;
				pend.what = curBase + " -> " + target + " key " + std::to_string(key);
			}
			if (ok) equal++;
			else printf("  WARP DIFF %s -> %s key %d: original %s %d,%d z %d sec %u; port %s %d,%d z %d sector %d\n", curBase.c_str(), dest, key,
				afterBase.c_str(), ex, ey, ez, esec, target.c_str(), px, py, pz, psec);
		}
		printf("  oracle warps: %d compared, %d equal (map, x, y, sector; z where nothing relocates)\n", compared, equal);
		printf("  oracle warps: %d relocations checked against the height rule with the original's live floors; "
			"%d arrival floors had moved from the file's value by load-time effects\n", heightsChecked, liveFloorMoved);
		g_checks++;
		if (compared != equal) g_failed++;
	}

	// ------------------------------------------------------------------
	// 5. The name compare, on the cases the original's loop distinguishes.
	// ------------------------------------------------------------------
	{
		CHECK(WarpNamesMatch("", "STUDY1"));
		CHECK(WarpNamesMatch("study1", "STUDY1"));
		CHECK(WarpNamesMatch("STUDY1", "m\\study1.RAW"));
		CHECK(!WarpNamesMatch("STUDY2", "STUDY1"));
		CHECK(!WarpNamesMatch("STUDY", "STUDY1"));       // the request ends first: bytes differ
		CHECK(!WarpNamesMatch("STUDY10", "STUDY1"));     // the loaded name ends first, request does not
		CHECK(WarpNamesMatch("MAUSO1EA", "MAUSO1EA"));   // eight characters: the count runs out
		WarpRequest r;
		CHECK(ClassifyWarp(r, "STUDY1") == WarpKind::Nothing);
		r.key = 5;
		CHECK(ClassifyWarp(r, "STUDY1") == WarpKind::RelocateInPlace);
		strcpy(r.name, "STUDY1"); r.key = 0;
		CHECK(ClassifyWarp(r, "STUDY1") == WarpKind::SameMapNoMove);
		strcpy(r.name, "VICAR");
		CHECK(ClassifyWarp(r, "STUDY1") == WarpKind::ChangeMap && WarpTargetName(r) == "VICAR");
		CHECK(ArrivalHeight(100, 40, -20) == 40);        // 60 above the old floor, 60 above the new
	}

	// ------------------------------------------------------------------
	// 6. Save and load: carried item, flags, gallery, hands, cursors, health.
	// ------------------------------------------------------------------
	{
		// The owner's check, in state terms: an item carried and a flag set
		// survive a save, a "quit" (a fresh process: a new Game), and a load.
		g.core.Give(9, nullptr);                         // the Colt
		g.core.Interp().Dbase100BitmapTestSet(77);
		g.core.Interp().Dbase100BitmapTestSet(430);
		if (!oracle.empty()) CompareDump(g, oracle, "CHANGED");
		{
			// The probe's own sequence: save, change things, load in the same
			// session. Everything changed after the save must be gone.
			PlayerRecord p0;
			SaveWriter s0;
			g.flow.SaveGlobals(s0, p0, "STUDY1");
			g.core.Remove(9);
			g.core.Give(24, nullptr);
			g.core.Interp().Dbase100BitmapTestSet(100);
			if (!oracle.empty() && !OracleLines(oracle, "DIVERGED").empty()) CompareDump(g, oracle, "DIVERGED");
			SaveReader r0(s0.Data().data(), s0.Data().size());
			CHECK(g.flow.LoadGlobals(r0));
			CHECK(g.core.Query(9, 0) == 1 && g.core.Query(24, 0) == 0);
			CHECK(g.core.Interp().TestDbase100RecordFlag(100) == 0);
			if (!oracle.empty() && !OracleLines(oracle, "LOADED").empty()) CompareDump(g, oracle, "LOADED");
		}
		g.flow.CursorPos()[3] = 2; g.flow.CursorScroll()[7] = 1;
		g.flow.SetHealth(0x432);
		g.core.Interp().State().g_value_reduction_factor = 37;

		PlayerRecord pr;
		pr.x = 864 << 16; pr.y = 3840 << 16; pr.z = 5 << 16; pr.angle = 128; pr.sector = 6972;
		SaveWriter w;
		g.flow.SaveGlobals(w, pr, "STUDY1");
		printf("save: global state is %d bytes\n", (int)w.Data().size());

		Game h;
		CHECK(h.Start(dbFile));
		h.flow.Boot();                                   // a fresh process has booted before any load
		SaveReader r(w.Data().data(), w.Data().size());
		PlayerRecord back; std::string mapBack;
		CHECK(h.flow.LoadGlobals(r, &back, &mapBack));
		CHECK(mapBack == "STUDY1");
		CHECK(back.x == pr.x && back.y == pr.y && back.z == pr.z && back.angle == 128 && back.sector == 6972);
		CHECK(back.health == 0x432 && back.valueReduction == 37);
		CHECK(DumpLines(h) == DumpLines(g));
		CHECK(h.core.Query(9, 0) == 1);
		CHECK(h.core.Interp().TestDbase100RecordFlag(77) != 0 && h.core.Interp().TestDbase100RecordFlag(430) != 0);
		CHECK(h.core.Interp().TestDbase100RecordFlag(78) == 0);
		CHECK(h.flow.Health() == 0x432 && h.core.Interp().State().g_value_reduction_factor == 37);
		CHECK(h.core.Items().AdamSlot() == 0);
		// Saving what was loaded gives the same bytes.
		SaveWriter w2;
		h.flow.SaveGlobals(w2, back, mapBack);
		CHECK(w2.Data() == w.Data());

		// A stream cut short anywhere is refused and changes nothing.
		Game k;
		CHECK(k.Start(dbFile));
		k.flow.Boot(); k.flow.NewGame();
		const std::map<std::string, std::string> before = DumpLines(k);
		int refused = 0, accepted = 0;
		for (size_t cut = 0; cut < w.Data().size(); cut++)
		{
			SaveReader t(w.Data().data(), cut);
			if (k.flow.LoadGlobals(t)) accepted++; else refused++;
			if (DumpLines(k) != before) { accepted++; break; }
		}
		printf("save: %d truncated streams refused, %d accepted\n", refused, accepted);
		CHECK(accepted == 0);

	}

	// ------------------------------------------------------------------
	// 7. One map's logic state round-trips.
	// ------------------------------------------------------------------
	{
		MapLogicState s;
		s.mapName = "STUDY1";
		uint32_t x = 12345;
		auto rnd = [&]() { x = x * 1103515245u + 12345u; return (x >> 8) & 0xffff; };
		for (int i = 0; i < 661; i++)
		{
			SavedCommand c;
			c.modifier = (uint8_t)rnd(); c.disabled = (uint8_t)(rnd() & 1);
			for (uint32_t k = rnd() % 12; k > 0; k--) c.args.push_back((uint16_t)rnd());
			s.commands.push_back(c);
		}
		for (int i = 0; i < 5; i++) s.delays.push_back({ (int32_t)rnd() - 300, (uint16_t)rnd(), rnd() });
		for (int i = 0; i < 3; i++) s.deferred.push_back({ (uint16_t)rnd(), rnd() });
		for (int i = 0; i < 9; i++)
		{
			SavedEffect e;
			e.tick = (uint8_t)rnd(); e.record = (uint16_t)rnd(); e.payload = (uint16_t)rnd();
			for (uint32_t k = rnd() % 6; k > 0; k--) { e.sectors.push_back((int32_t)rnd()); e.baseLight.push_back((uint8_t)rnd()); }
			e.sfxNode = (int32_t)(rnd() % 40) - 1; e.sfxStarted = (uint8_t)(rnd() & 1);
			e.finished = (uint8_t)(rnd() & 1); e.hold = (uint16_t)rnd(); e.holding = (uint8_t)(rnd() & 1); e.flags5 = (uint8_t)rnd();
			for (uint32_t k = (rnd() % 4) * 2; k > 0; k--) e.objects.push_back((int32_t)rnd());
			for (int k = 0; k < 4; k++) e.moveVerts[k] = (int32_t)rnd() - 5;
			s.effects.push_back(e);
		}
		s.pendingRecord = 17; s.activeFace = 4; s.activeSector = -1; s.activeObjectSector = 9; s.activeObjectIndex = 2;
		s.movers = { 5, -64, 0, 9, 0, 128 };
		s.lastSectorByPlayer = { 0, 268 };
		s.dialogueLatch = 1;

		SaveWriter w;
		s.Save(w);
		MapLogicState t;
		SaveReader r(w.Data().data(), w.Data().size());
		CHECK(t.Load(r) && r.AtEnd());
		CHECK(t == s);
		SaveWriter w2;
		t.Save(w2);
		CHECK(w2.Data() == w.Data());
		printf("map state: 661 commands, 9 effects -> %d bytes, round trip equal\n", (int)w.Data().size());

		int refused = 0, wrong = 0;
		for (size_t cut = 0; cut < w.Data().size(); cut += 7)
		{
			MapLogicState u;
			u.mapName = "UNTOUCHED";
			SaveReader c(w.Data().data(), cut);
			if (u.Load(c)) wrong++; else refused++;
			if (u.mapName != "UNTOUCHED") wrong++;
		}
		CHECK(wrong == 0);
		// Random damage must never read out of bounds (run under ASan/UBSan).
		for (int k = 0; k < 2000; k++)
		{
			std::vector<uint8_t> d = w.Data();
			for (int j = 0; j < 4; j++) d[rnd() % d.size()] = (uint8_t)rnd();
			MapLogicState u;
			SaveReader c(d.data(), d.size());
			u.Load(c);
		}
		printf("map state: %d truncated streams refused, 2000 damaged streams read without a fault\n", refused);
	}

	// ------------------------------------------------------------------
	// 8. Death, and the end of the game.
	// ------------------------------------------------------------------
	{
		Game d;
		CHECK(d.Start(dbFile));
		d.flow.Boot(); d.flow.NewGame();

		// Killed: action 4 plays its sound, then the wait.
		d.flow.SetHealth(0);
		CHECK(d.flow.BeginDeath() == GameFlow::DeathStep::Waiting);
		printf("death: action 4 asked for %d sound(s), the first is %u\n", (int)d.host.sfx.size(), d.host.sfx.empty() ? 0u : d.host.sfx[0]);
		CHECK(d.host.sfx.size() == 1);
		CHECK(d.flow.CurrentMode() == GameFlow::MODE_DEAD && d.core.Items().LeftHand() == -1);
		int passes = 0;
		GameFlow::DeathStep st = GameFlow::DeathStep::Waiting;
		while (st == GameFlow::DeathStep::Waiting && passes < 1000) { st = d.flow.TickDeath(2, 0); passes++; }
		printf("death: the wait ends after %d passes at frame scale 2 (%d units)\n", passes, d.flow.DeathUnits());
		CHECK(st == GameFlow::DeathStep::Restart && passes == 150);

		// Esc ends it at once; 0x39 with no slot ever used ends it too; with a
		// slot it asks for the load.
		d.flow.NewGame(); d.flow.SetHealth(0); d.flow.BeginDeath();
		CHECK(d.flow.TickDeath(2, 0) == GameFlow::DeathStep::Waiting);
		CHECK(d.flow.TickDeath(2, 1) == GameFlow::DeathStep::Restart);
		d.flow.NewGame(); d.flow.SetHealth(0); d.flow.BeginDeath();
		CHECK(d.flow.LastSlot() == -1);
		CHECK(d.flow.TickDeath(2, 0x39) == GameFlow::DeathStep::Restart);
		d.flow.NewGame(); d.flow.SetHealth(0); d.flow.BeginDeath();
		d.flow.SetLastSlot(0);
		CHECK(d.flow.TickDeath(2, 0x39) == GameFlow::DeathStep::QuickLoad);
		CHECK(d.flow.DeathLoadFailed() == GameFlow::DeathStep::Restart);

		// Back at the menu everything the game wrote is gone.
		d.core.Interp().Dbase100BitmapTestSet(12);
		d.flow.ResetForMenu();
		CHECK(d.core.Interp().TestDbase100RecordFlag(12) == 0);
		CHECK(d.flow.CutscenesSeenCount() == 1 && d.db.CutsceneGallery(150) == 0);
		d.flow.ClearMovieLog();
		d.flow.NewGame();
		CHECK(d.db.CutsceneGallery(150) == 1 && d.flow.CutscenesSeenCount() == 2);     // CHPTR1 is now the first movie seen
		CHECK(d.core.Items().Count() == 4 && d.flow.Health() == 0x800);

		// The end of the game: action 614.
		d.host.sfx.clear(); d.flow.ClearMovieLog();
		d.core.RunAction(614);
		printf("the end (action 614): movies");
		for (const std::string &m : d.flow.MovieLog()) printf(" %s", m.c_str());
		printf(", images %d, health %d, no-wait latch %d, music %u\n", (int)d.host.images.size(), d.flow.Health(),
			d.flow.NoDeathWait() ? 1 : 0, d.flow.MusicChunk());
		CHECK(d.flow.MovieLog().size() == 2 && d.flow.MovieLog()[1] == "ECREDITS");
		CHECK(d.flow.Health() == 0 && d.flow.NoDeathWait());
		// The chain's last word, music 0x294411, does NOT run: the original
		// ends with the game music still current (measured: the probe's
		// AFTER614 line), and so does this.
		CHECK(d.flow.MusicChunk() == 0x293cad);
		CHECK(d.host.images.size() == 1 && d.host.images[0] == 0x29f544);
		if (!oracle.empty() && !OracleLines(oracle, "AFTER614").empty())
		{
			// Bring a game to the same point the probe was at: boot, new game, 614.
			Game e;
			CHECK(e.Start(dbFile));
			e.flow.Boot(); e.flow.NewGame();
			e.core.RunAction(614);
			CompareDump(e, oracle, "AFTER614");
			CHECK(e.flow.BeginDeath() == GameFlow::DeathStep::Restart);
			e.flow.ResetForMenu();
			e.flow.NewGame();
			CompareDump(e, oracle, "LATER");
		}
		CHECK(d.flow.BeginDeath() == GameFlow::DeathStep::Restart);    // no wait: straight to the menu

		// Which RAW records start it, and where.
		int callers = 0;
		for (const LoadedMap &lm : maps)
			for (size_t ci = 0; ci < lm.map.commands.size(); ci++)
			{
				const Command &c = lm.map.commands[ci];
				if ((c.opcode & 0x7f) == 0x2b && c.key == 614) { callers++; printf("  action 614 is run by %s record %d\n", lm.name.c_str(), (int)ci + 1); }
			}
		printf("  action 614 has %d RAW 0x2b caller(s)\n", callers);
	}

	// ------------------------------------------------------------------
	// 9. An original save file.
	// ------------------------------------------------------------------
	if (!savPath.empty())
	{
		const std::vector<uint8_t> sav = ReadFile(savPath);
		const OriginalSave s = ParseOriginalSave(sav.data(), sav.size());
		printf("original save %s: %d bytes, chunks", savPath.c_str(), (int)sav.size());
		for (uint16_t id : s.order) printf(" %x", id);
		printf("\n  map %s, %d per-map state file(s):", s.mapName.c_str(), (int)s.maps.size());
		for (const OriginalSave::MapState &m : s.maps) printf(" %s (%d bytes)", m.file.c_str(), (int)m.bytes.size());
		printf("\n  player x %d y %d z %d angle %u sector %u health %d hands secondary %u primary %u\n", s.player.x >> 16, s.player.y >> 16,
			s.player.z >> 16, s.player.angle, s.player.sector, s.player.health, s.player.secondary, s.player.primary);
		CHECK(s.ok && s.hasPlayer);
		CHECK(s.inventory.size() == 0x400 && s.cursorPos.size() == 0x14 && s.cursorScroll.size() == 0x14);
		CHECK((int)s.gallery.size() == g.db.CutsceneCount());
		CHECK(s.flags.size() == g.core.Interp().State().g_dbase100_record_bitmap.size());
		for (const OriginalSave::MapState &m : s.maps)
		{
			const OriginalMapState o = MeasureOriginalMapState(m.bytes.data(), m.bytes.size());
			printf("  %s: geometry %s %u bytes (%u changed), objects %u, command patch %u, effects %u, entities %u, sound nodes %u, EXIT %s\n",
				m.file.c_str(), o.geometryIsDelta ? "delta" : "literal", o.geometryBytes, o.geometryChanged, o.objectBytes,
				o.commandPatchBytes, o.effectBytes, o.entityBytes, o.soundNodeBytes, o.ok ? "yes" : "NO");
			CHECK(o.ok);
		}

		// Import it into a fresh game, and compare with the port's own state
		// after the same inputs (g, above: new game + the Colt + flags 77, 430).
		Game im;
		CHECK(im.Start(dbFile));
		im.flow.Boot();
		CHECK(im.flow.ImportOriginal(s));
		// Byte for byte: what this port would put in each global chunk.
		const std::vector<uint8_t> inv = g.core.Items().Serialize();
		CHECK(inv == s.inventory);
		CHECK(g.core.Interp().State().g_dbase100_record_bitmap == s.flags);
		std::vector<uint8_t> gal;
		for (int i = 0; i < g.db.CutsceneCount(); i++) gal.push_back(g.db.CutsceneGallery(i));
		CHECK(gal == s.gallery);
		CHECK(g.flow.MusicChunk() == s.musicChunk);
		PlayerRecord mine;
		g.flow.FillPlayerRecord(mine);
		CHECK(mine.primary == s.player.primary && mine.secondary == s.player.secondary);
		int diffs = 0;
		{
			const std::map<std::string, std::string> a = DumpLines(im), b = DumpLines(g);
			for (const auto &kv : a)
			{
				if (kv.first == "health" || kv.first == "cursorpos" || kv.first == "cursorscroll") continue;   // set by hand in section 6
				auto it = b.find(kv.first);
				if (it == b.end() || it->second != kv.second) { diffs++; printf("  SAV DIFF %s: original \"%s\" port \"%s\"\n", kv.first.c_str(), kv.second.c_str(), it == b.end() ? "(absent)" : it->second.c_str()); }
			}
		}
		printf("  imported: %d differences from this port's own state after the same inputs\n", diffs);
		CHECK(diffs == 0);
		CHECK(s.mapName == "STUDY1");
	}

	if (!tmpPath.empty())
	{
		const std::vector<uint8_t> t = ReadFile(tmpPath);
		const OriginalMapState o = MeasureOriginalMapState(t.data(), t.size());
		printf("original map state %s: %d bytes: geometry %s %u bytes (%u changed), objects %u, command patch %u, effects %u, entities %u, sound nodes %u, EXIT %s\n",
			tmpPath.c_str(), (int)t.size(), o.geometryIsDelta ? "delta" : "literal", o.geometryBytes, o.geometryChanged, o.objectBytes,
			o.commandPatchBytes, o.effectBytes, o.entityBytes, o.soundNodeBytes, o.ok ? "yes" : "NO");
		CHECK(o.ok);
	}

	printf("gameflow_test: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
