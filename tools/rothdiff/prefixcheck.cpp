// prefixcheck.cpp -- the offline audit of the DAS entry PREFIX DWORD.
//
// WHAT THIS IS ABOUT
//
// ROTH's sprite vertical base has an asymmetry the port does not reproduce.
// The two projection paths compute the same quantity differently:
//
//   linear  (renderer.c:6550-6555)
//       edx = (int16)( (uint16)((mod & 0xf) * 2) + (uint16)[0x84aba] )
//   rotated (renderer.c:6129-6134)
//       edx =                     (mod & 0xf) * 2
//
// so the linear path carries a term the rotated one does not -- flagged in the
// lift itself as "the rotated path OMITS the linear-only [0x84aba] term ...
// real asymmetries, disasm-confirmed, do NOT fix" (renderer.c:6089).
//
// [0x84aba] is never written on its own. Every write is a 32-bit store to
// 0x84ab8 of `dword[block+4]` (renderer.c:5666, :5676, :5692, :5731, :5976,
// :6015; zeroed at :6663 and :7703), so the vertical term is that dword's HIGH
// word. Its LOW word is read two instructions later, sign-extended, shifted
// left 8 and added to the lateral position -- negated when the x-flip bit is
// set (renderer.c:5667-5670). That pairing is why this looks like an ANCHOR
// OFFSET and not a size, and this tool is here to decide which it is from the
// files rather than from the reading.
//
// WHERE THE DWORD LIVES
//
// The loader is das_assets.c:934-944. When the FAT entry's word at +6 has bit 3
// set it seeks to `fileoff - 4` and reads `size + 4` bytes to `block + 6`, then
// copies the leading dword down to `block + 4`. Otherwise it seeks to `fileoff`,
// reads `size` bytes to `block + 0xa`, and sets `block + 4` to zero.
//
// Either way the payload lands at `block + 0xa`, so the image header still
// begins exactly at `fat.offset` and roth_das.cpp's reader is unaffected. The
// prefix is the FOUR BYTES IMMEDIATELY BEFORE the entry in the file, and only
// when flags_1 bit 3 is set.
//
// WHAT IT MEASURES
//
//   - how many entries carry the prefix, per pack;
//   - whether the dword behaves like a SIZE (tracking width*height or the FAT
//     size word) or like a PAIR OF SIGNED OFFSETS (two small values near zero).
//     These two readings make opposite predictions and the files decide;
//   - whether bit 3 ever coincides with the other flags_1 values, so the bit
//     can be read on its own;
//   - how many PLACED objects in the 44 retail maps resolve to a prefixed
//     entry, which is the size of what is wrong on screen today;
//   - the specific entries a measurement has already been taken against.
//
// It reads the player's own .RAW and .DAS through the same reader the loader
// uses, and writes nothing.
//
// Build:
//   i686-w64-mingw32-g++ -std=c++17 -O1 -static -I ../../src/roth \
//       -o prefixcheck.exe prefixcheck.cpp \
//       ../../src/roth/roth_raw.cpp ../../src/roth/roth_install.cpp \
//       ../../src/roth/roth_das.cpp ../../src/roth/roth_palette.cpp

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"

#include <stdio.h>
#include <stdlib.h>
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

static uint32_t RdU32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t RdU16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

struct Totals
{
	long long entries = 0, prefixed = 0, notPrefixed = 0;
	long long prefixUnreadable = 0;     // offset < 4: the prefix would be off the front
	long long prefixZero = 0;
	// the SIZE reading
	long long matchesFatSize = 0, matchesPixelCount = 0, matchesEntryPlusFour = 0;
	// the OFFSET-PAIR reading
	long long xZero = 0, yZero = 0, bothZero = 0;
	long long xSmall = 0, ySmall = 0;   // |v| <= 512, i.e. plausible as a world offset
	long long xHuge = 0, yHuge = 0;     // |v| > 4096, implausible as either
	int xMin = 32767, xMax = -32768, yMin = 32767, yMax = -32768;
	long long xNeg = 0, yNeg = 0;
	long long objectsTotal = 0, objectsPrefixed = 0;
};

static std::map<int, long long> gFlags1All;        // every flags_1 value seen
static std::map<int, long long> gFlags1Prefixed;   // ... among prefixed entries
static std::map<int, long long> gKindPrefixed;     // which EntryKinds carry it
static std::map<std::string, long long> gPerPackPrefixed;
static std::map<std::string, long long> gPerMapObjects;
static std::map<int, long long> gYHistogram;       // the vertical term's values

static void AuditPack(const roth::Pack &pack, const std::vector<uint8_t> &raw,
	const char *label, Totals &t, std::set<std::string> &seen, bool verbose)
{
	if (!seen.insert(label).second) return;

	long long here = 0;
	int shown = 0;
	for (int i = 0; i < pack.Count(); i++)
	{
		const roth::FatEntry *e = pack.Entry(i);
		if (e == nullptr || e->size == 0) continue;
		t.entries++;
		gFlags1All[e->flags1]++;

		if ((e->flags1 & 8) == 0) { t.notPrefixed++; continue; }

		t.prefixed++;
		here++;
		gFlags1Prefixed[e->flags1]++;
		gKindPrefixed[(int)e->kind]++;

		if (e->offset < 4 || (size_t)e->offset > raw.size())
		{
			t.prefixUnreadable++;
			continue;
		}
		const uint32_t dw = RdU32(raw.data() + e->offset - 4);
		if (dw == 0) { t.prefixZero++; }

		// --- the SIZE reading ---
		// If the dword is a size it should track one of these. The loader reads
		// `size + 4` bytes, so `size + 4` is the candidate a byte count would
		// most plausibly take.
		if (dw == (uint32_t)e->size) t.matchesFatSize++;
		if (dw == (uint32_t)e->size + 4u) t.matchesEntryPlusFour++;
		if ((size_t)e->offset + 6 <= raw.size())
		{
			const uint32_t w = RdU16(raw.data() + e->offset + 2);
			const uint32_t h = RdU16(raw.data() + e->offset + 4);
			if (w > 0 && h > 0 && dw == w * h) t.matchesPixelCount++;
		}

		// --- the OFFSET-PAIR reading ---
		const int16_t x = (int16_t)(uint16_t)(dw & 0xffff);
		const int16_t y = (int16_t)(uint16_t)(dw >> 16);
		if (x == 0) t.xZero++;
		if (y == 0) t.yZero++;
		if (x == 0 && y == 0) t.bothZero++;
		if (x < 0) t.xNeg++;
		if (y < 0) t.yNeg++;
		if (abs((int)x) <= 512) t.xSmall++;
		if (abs((int)y) <= 512) t.ySmall++;
		if (abs((int)x) > 4096) t.xHuge++;
		if (abs((int)y) > 4096) t.yHuge++;
		if (x < t.xMin) t.xMin = x;
		if (x > t.xMax) t.xMax = x;
		if (y < t.yMin) t.yMin = y;
		if (y > t.yMax) t.yMax = y;
		gYHistogram[y]++;

		if (verbose && shown < 12)
		{
			printf("    %-10s entry %5d  flags1 0x%02x flags2 %3d  size %5u  "
				"prefix %08x  x %6d  y %6d\n",
				label, i, e->flags1, e->flags2, e->size, dw, (int)x, (int)y);
			shown++;
		}
	}
	gPerPackPrefixed[label] = here;
	printf("  %-10s %5d entries, %5lld prefixed\n", label, pack.Count(), here);
}

// The entries a measurement has already been taken against, so this run says
// something about them directly rather than only in aggregate.
static void ShowNamed(const roth::Pack &pack, const std::vector<uint8_t> &raw,
	const char *label, const int *ids, int n)
{
	for (int k = 0; k < n; k++)
	{
		const int i = ids[k];
		const roth::FatEntry *e = pack.Entry(i);
		if (e == nullptr) { printf("  %s[%d]  no entry\n", label, i); continue; }
		printf("  %s[%d]  flags1 0x%02x  flags2 %3d  size %5u  kind %d",
			label, i, e->flags1, e->flags2, e->size, (int)e->kind);
		if (e->size != 0 && (size_t)e->offset + 6 <= raw.size())
		{
			const uint8_t mod = raw[e->offset];
			const uint8_t ity = raw[e->offset + 1];
			printf("  mod 0x%02x  type 0x%02x  %ux%u", mod, ity,
				RdU16(raw.data() + e->offset + 2), RdU16(raw.data() + e->offset + 4));
			printf("  nibble %d  hang %d", mod & 0xf, (mod & 0x10) ? 1 : 0);
		}
		if ((e->flags1 & 8) && e->offset >= 4)
		{
			const uint32_t dw = RdU32(raw.data() + e->offset - 4);
			printf("  PREFIX %08x  x %d  y %d", dw,
				(int)(int16_t)(uint16_t)(dw & 0xffff), (int)(int16_t)(uint16_t)(dw >> 16));
		}
		else
		{
			printf("  no prefix");
		}
		printf("\n");
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: prefixcheck <ROTH folder> [-v]\n"); return 2; }
	const bool verbose = (argc > 2 && std::string(argv[2]) == "-v");
	if (!roth::TheInstall().Open(argv[1])) { printf("install: bad\n"); return 2; }

	Totals t;
	std::set<std::string> seen;

	printf("packs\n");
	std::vector<uint8_t> sd = ReadFile(roth::TheInstall().PackFile(
		roth::TheInstall().SharedPack().c_str()));
	roth::Pack shared;
	const bool haveShared = !sd.empty() && shared.Load(sd.data(), sd.size());
	if (haveShared) AuditPack(shared, sd, "SHARED", t, seen, verbose);
	else printf("  SHARED      not opened\n");

	// Keep the map packs' bytes alive: the audit reads behind each entry.
	std::map<std::string, std::vector<uint8_t>> packBytes;
	for (const std::string &name : roth::TheInstall().MapNames())
	{
		const std::string pk = roth::TheInstall().PackFor(name.c_str());
		if (seen.count(pk)) continue;
		std::vector<uint8_t> pd = ReadFile(roth::TheInstall().PackFile(pk.c_str()));
		if (pd.empty()) continue;
		roth::Pack pack;
		if (!pack.Load(pd.data(), pd.size())) continue;
		AuditPack(pack, pd, pk.c_str(), t, seen, verbose);
		packBytes[pk] = std::move(pd);
	}

	// How many PLACED objects reach a prefixed entry. This is the number that
	// says how much of the screen is affected, rather than how much of the file.
	for (const std::string &name : roth::TheInstall().MapNames())
	{
		std::vector<uint8_t> d = ReadFile(roth::TheInstall().MapFile(name.c_str()));
		if (d.empty()) continue;
		roth::Map m = roth::ParseRaw(d.data(), d.size());
		if (!m.ok()) continue;

		const std::string pk = roth::TheInstall().PackFor(name.c_str());
		auto it = packBytes.find(pk);
		if (it == packBytes.end()) continue;
		roth::Pack level;
		if (!level.Load(it->second.data(), it->second.size())) continue;

		for (const std::vector<roth::Object> &group : m.objects)
			for (const roth::Object &o : group)
			{
				if (o.flags & 0x80) continue;   // never drawn
				t.objectsTotal++;
				int idx = 0;
				const bool useShared = roth::Pack::ResolveObjectArt(
					o.textureIndex, o.textureSource, idx);
				if (useShared && !haveShared) continue;
				const roth::Pack &p = useShared ? shared : level;
				const roth::FatEntry *e = p.Entry(idx);
				if (e == nullptr || e->size == 0) continue;
				if (e->flags1 & 8) { t.objectsPrefixed++; gPerMapObjects[name]++; }
			}
	}

	printf("\nentries\n");
	printf("  total          %8lld\n", t.entries);
	printf("  prefixed       %8lld  (flags_1 bit 3)\n", t.prefixed);
	printf("  not prefixed   %8lld\n", t.notPrefixed);
	printf("  unreadable     %8lld  (offset < 4)\n", t.prefixUnreadable);
	printf("  prefix == 0    %8lld\n", t.prefixZero);

	printf("\nthe SIZE reading -- does the dword track a byte count?\n");
	printf("  == FAT size       %8lld / %lld\n", t.matchesFatSize, t.prefixed);
	printf("  == FAT size + 4   %8lld / %lld\n", t.matchesEntryPlusFour, t.prefixed);
	printf("  == width*height   %8lld / %lld\n", t.matchesPixelCount, t.prefixed);

	printf("\nthe OFFSET-PAIR reading -- two signed words?\n");
	printf("  x range     %6d .. %6d   zero %lld  negative %lld  |v|<=512 %lld  |v|>4096 %lld\n",
		t.xMin, t.xMax, t.xZero, t.xNeg, t.xSmall, t.xHuge);
	printf("  y range     %6d .. %6d   zero %lld  negative %lld  |v|<=512 %lld  |v|>4096 %lld\n",
		t.yMin, t.yMax, t.yZero, t.yNeg, t.ySmall, t.yHuge);
	printf("  both zero   %6lld\n", t.bothZero);

	printf("\nthe vertical term's values (high word), most common first\n");
	{
		std::vector<std::pair<long long, int>> v;
		for (const auto &kv : gYHistogram) v.push_back({ kv.second, kv.first });
		std::sort(v.begin(), v.end(), [](const std::pair<long long, int> &a,
			const std::pair<long long, int> &b) { return a.first > b.first; });
		for (size_t i = 0; i < v.size() && i < 20; i++)
			printf("    y %6d  x%lld\n", v[i].second, v[i].first);
		printf("    (%u distinct values)\n", (unsigned)v.size());
	}

	printf("\nflags_1 values, all entries\n");
	for (const auto &kv : gFlags1All)
		printf("    0x%02x  x%-8lld %s\n", kv.first, kv.second,
			(kv.first & 8) ? "<- prefixed" : "");

	printf("\nEntryKind of the prefixed entries (0 Empty 1 Plain 2 Animated 3 ImagePack 4 Object3D 5 Indirection)\n");
	for (const auto &kv : gKindPrefixed) printf("    kind %d  x%lld\n", kv.first, kv.second);

	printf("\nplaced objects\n");
	printf("  total        %8lld\n", t.objectsTotal);
	printf("  prefixed art %8lld\n", t.objectsPrefixed);
	{
		std::vector<std::pair<long long, std::string>> v;
		for (const auto &kv : gPerMapObjects) v.push_back({ kv.second, kv.first });
		std::sort(v.begin(), v.end(), [](const std::pair<long long, std::string> &a,
			const std::pair<long long, std::string> &b) { return a.first > b.first; });
		for (size_t i = 0; i < v.size() && i < 12; i++)
			printf("    %-10s %lld\n", v[i].second.c_str(), v[i].first);
	}

	// WHO ACTUALLY READS A PREFIXED ENTRY.
	//
	// Almost no PLACED object does, so the placed-object count above is not the
	// impact. The prefixed entries are overwhelmingly Animated and live only in
	// the shared pack, which is where a DIRECTIONAL entry's frames point. So the
	// consumer to measure is the frame tables: every directional entry in every
	// pack, resolved, with each frame checked for the prefix.
	if (haveShared)
	{
		long long dirEntries = 0, frames = 0, framesPrefixed = 0;
		long long dirWithAny = 0, dirWithAll = 0;
		std::set<int> prefixedFramesHit;
		std::map<int, long long> yAmongHit;
		// roth_objects.cpp takes ONE SpriteInfo for a directional object -- the
		// one belonging to view 0 -- and applies its anchor to the whole actor.
		// That is only sound if every view of an entry agrees on the anchor, so
		// count the entries whose views disagree instead of assuming they do.
		long long dirUniformY = 0, dirVariedY = 0, dirUniformX = 0, dirVariedX = 0;
		long long dirMirrorMixed = 0;

		auto auditFrames = [&](const roth::Pack &pack)
		{
			for (int i = 0; i < pack.Count(); i++)
			{
				if (pack.Indirect(i) != roth::IndirectKind::Directional) continue;
				const roth::Directional d = pack.ReadDirectional(i);
				if (!d.ok()) continue;
				dirEntries++;
				int hit = 0, mirrors = 0;
				std::set<int> ysHere, xsHere;
				for (int f = 0; f < d.count; f++)
				{
					frames++;
					if (d.frames[f].mirror) mirrors++;
					int fidx = 0;
					const bool fromShared = roth::Pack::ResolveDasId(d.frames[f].entry, fidx);
					const roth::Pack *fp = fromShared ? &shared : &pack;
					const roth::FatEntry *fe = fp->Entry(fidx);
					if (fe == nullptr || fe->size == 0) continue;
					if ((fe->flags1 & 8) == 0) continue;
					framesPrefixed++;
					hit++;
					if (fromShared)
					{
						prefixedFramesHit.insert(fidx);
						if (fe->offset >= 4 && (size_t)fe->offset <= sd.size())
						{
							const uint32_t dw = RdU32(sd.data() + fe->offset - 4);
							const int fy = (int)(int16_t)(uint16_t)(dw >> 16);
							const int fx = (int)(int16_t)(uint16_t)(dw & 0xffff);
							yAmongHit[fy]++;
							ysHere.insert(fy);
							xsHere.insert(fx);
						}
					}
				}
				if (hit > 0) dirWithAny++;
				if (hit == d.count) dirWithAll++;
				if (hit > 0)
				{
					if (ysHere.size() <= 1) dirUniformY++; else dirVariedY++;
					if (xsHere.size() <= 1) dirUniformX++; else dirVariedX++;
					if (mirrors != 0 && mirrors != d.count) dirMirrorMixed++;
				}
			}
		};

		auditFrames(shared);
		for (auto &kv : packBytes)
		{
			roth::Pack pack;
			if (pack.Load(kv.second.data(), kv.second.size())) auditFrames(pack);
		}

		printf("\ndirectional frames -- the real consumer\n");
		printf("  directional entries resolved  %8lld\n", dirEntries);
		printf("    at least one prefixed frame %8lld\n", dirWithAny);
		printf("    every frame prefixed        %8lld\n", dirWithAll);
		printf("  frames total                  %8lld\n", frames);
		printf("  frames on a prefixed entry    %8lld\n", framesPrefixed);
		printf("  distinct prefixed entries hit %8u / %lld in the files\n",
			(unsigned)prefixedFramesHit.size(), t.prefixed);
		printf("  of those entries, do the views agree?\n");
		printf("    same vertical anchor across views  %8lld  differing %lld\n",
			dirUniformY, dirVariedY);
		printf("    same lateral anchor across views   %8lld  differing %lld\n",
			dirUniformX, dirVariedX);
		printf("    views mirrored inconsistently      %8lld\n", dirMirrorMixed);
		printf("  the vertical terms they carry, most common first\n");
		{
			std::vector<std::pair<long long, int>> v;
			for (const auto &kv : yAmongHit) v.push_back({ kv.second, kv.first });
			std::sort(v.begin(), v.end(), [](const std::pair<long long, int> &a,
				const std::pair<long long, int> &b) { return a.first > b.first; });
			for (size_t i = 0; i < v.size() && i < 12; i++)
				printf("      y %6d  x%lld\n", v[i].second, v[i].first);
		}
	}

	// Parity of the vertical term. MEASURED 315 even, 6 odd -- so it is NOT a
	// doubled half-unit like the (mod & 0xf) * 2 it is added to. It is a plain
	// signed world-unit offset that merely happens to be mostly even.
	{
		long long odd = 0, even = 0;
		for (const auto &kv : gYHistogram) { if (kv.first & 1) odd += kv.second; else even += kv.second; }
		printf("\nparity of the vertical term: %lld even, %lld odd\n", even, odd);
	}

	// The entries a prop measurement has already been taken against. DEMO's own
	// pack carries no prefix at all, so the nibble-8 discrepancy on DEMO[4102]
	// is a SEPARATE fault from this one -- worth printing so the two do not get
	// conflated again.
	for (const std::string &name : roth::TheInstall().MapNames())
	{
		const std::string pk = roth::TheInstall().PackFor(name.c_str());
		if (pk != "DEMO") continue;
		auto it = packBytes.find(pk);
		if (it == packBytes.end()) break;
		roth::Pack pack;
		if (!pack.Load(it->second.data(), it->second.size())) break;
		printf("\nthe measured entries, map %s -> pack %s\n", name.c_str(), pk.c_str());
		const int ids[] = { 4102, 4123, 4128 };
		ShowNamed(pack, it->second, pk.c_str(), ids, 3);
		break;
	}
	return 0;
}
