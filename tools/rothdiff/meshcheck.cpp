// meshcheck.cpp -- where the IT_OBJECT_DATA meshes are, and whether the two
// known mesh faults can actually reach the screen.
//
// WHY THIS EXISTS
//
// Two sessions were spent treating "those pillars are too tall" as a SPRITE
// scaling bug. The offenders, DEMO[4123] and DEMO[4128], are meshes, and the
// mesh scale chain was re-derived on 2026-10-01 and came out at a gain of
// EXACTLY 1.0 -- there is no scale error. So the question is no longer "what is
// the scale" but "where are these things and what do they look like", which
// needs a camera pointed at one.
//
// This prints the world position of every placed mesh so a camera can be aimed,
// and answers two questions the derivation left open:
//
//   1. Does ANY map place DEMO.DAS[4097]? It is the only entry in all five
//      packs tagged "EXPL" rather than "EXP2", and the EXPL variant stores a
//      face's texture id as a zero-extended BYTE at +0x0c instead of a
//      byteswapped word (renderer.c:817-821). roth_das.cpp reads BE16 at +0x0c
//      unconditionally, so for an EXPL mesh every face would resolve to a huge
//      id -- which MeshFace treats as a flat colour. If nothing places it, the
//      bug is unreachable and does not need fixing now.
//   2. How many placed objects are meshes at all, and which entries, so the
//      size of the mesh path is a number rather than an impression.
//
// It reads the player's own .RAW and .DAS through the same reader the loader
// uses, and writes nothing.
//
// Build:
//   cl /nologo /EHsc /std:c++17 /O2 /I ..\..\src\roth /Fe:meshcheck.exe \
//       /Fo:obj_mc\ meshcheck.cpp ../../src/roth/roth_raw.cpp \
//       ../../src/roth/roth_install.cpp ../../src/roth/roth_das.cpp \
//       ../../src/roth/roth_palette.cpp

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"

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
static uint16_t RdU16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

// The four-byte tag the engine tests. It sits 8 bytes before the face chain,
// which the block header locates as (u16[+0x12] << 4) + u16[+0x10] -- the same
// paragraph:offset far pointer the vertex table uses (renderer.c:796-805).
// Returned as a readable string, or "none" when it cannot be located.
static std::string MeshTag(const std::vector<uint8_t> &raw, uint32_t entryOff, size_t size)
{
	// block + N == entry + (N - 10), so the header words the engine reads at
	// block+0x10/+0x12 are at entry+6/entry+8.
	if ((size_t)entryOff + 10 > raw.size()) return "oob";
	const uint32_t lo = RdU16(raw.data() + entryOff + 6);
	const uint32_t hi = RdU16(raw.data() + entryOff + 8);
	const uint32_t faceOff = (hi << 4) + lo;        // relative to block
	if (faceOff < 10 + 8) return "none";
	const size_t at = (size_t)entryOff + (faceOff - 10) - 8;
	if (at + 4 > raw.size()) return "oob";
	std::string s;
	for (int i = 0; i < 4; i++)
	{
		const uint8_t c = raw[at + i];
		s += (c >= 32 && c < 127) ? (char)c : '.';
	}
	return s;
}

struct Placed
{
	std::string map, pack;
	int index = 0;
	int x = 0, y = 0, z = 0;
	int rot = 0;
	bool shared = false;
};

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: meshcheck <ROTH folder> [-v]\n"); return 2; }
	const bool verbose = (argc > 2 && std::string(argv[2]) == "-v");
	if (!roth::TheInstall().Open(argv[1])) { printf("install: bad\n"); return 2; }

	std::vector<uint8_t> sd = ReadFile(roth::TheInstall().PackFile(
		roth::TheInstall().SharedPack().c_str()));
	roth::Pack shared;
	const bool haveShared = !sd.empty() && shared.Load(sd.data(), sd.size());

	// Every mesh entry in every pack, with its tag and its decoded size.
	printf("mesh entries per pack  (tag, vertices, faces)\n");
	std::map<std::string, std::vector<uint8_t>> packBytes;
	std::map<std::string, std::map<std::string, int>> tagCount;   // pack -> tag -> n
	std::set<std::string> seen;
	long long meshEntries = 0, explEntries = 0;

	auto auditPack = [&](const roth::Pack &pack, const std::vector<uint8_t> &raw, const char *label)
	{
		if (!seen.insert(label).second) return;
		int n = 0, bad = 0;
		for (int i = 0; i < pack.Count(); i++)
		{
			const roth::FatEntry *e = pack.Entry(i);
			if (e == nullptr || e->kind != roth::EntryKind::Object3D || e->size == 0) continue;
			n++;
			meshEntries++;
			const std::string tag = MeshTag(raw, e->offset, e->size);
			tagCount[label][tag]++;
			if (tag == "EXPL") explEntries++;
			const roth::Mesh m = pack.ReadMesh(i);
			if (!m.ok()) bad++;
			if (verbose || tag == "EXPL")
				printf("    %-8s [%4d]  tag %-4s  %3u verts  %3u faces  size %5u%s\n",
					label, i, tag.c_str(), (unsigned)m.vertices.size(),
					(unsigned)m.faces.size(), e->size,
					tag == "EXPL" ? "   <- EXPL, the port's face fields are wrong for this kind" : "");
		}
		printf("  %-8s %d mesh entries, %d that the reader could not decode\n", label, n, bad);
	};

	if (haveShared) auditPack(shared, sd, "SHARED");
	for (const std::string &name : roth::TheInstall().MapNames())
	{
		const std::string pk = roth::TheInstall().PackFor(name.c_str());
		if (packBytes.count(pk)) continue;
		std::vector<uint8_t> pd = ReadFile(roth::TheInstall().PackFile(pk.c_str()));
		if (pd.empty()) continue;
		roth::Pack pack;
		if (!pack.Load(pd.data(), pd.size())) continue;
		auditPack(pack, pd, pk.c_str());
		packBytes[pk] = std::move(pd);
	}

	printf("\n  tags seen: ");
	for (const auto &pk : tagCount)
		for (const auto &t : pk.second) printf("%s/%s=%d  ", pk.first.c_str(), t.first.c_str(), t.second);
	printf("\n");

	// Every PLACED mesh, so a camera can be aimed at one.
	std::vector<Placed> placed;
	std::map<std::string, long long> perEntry;
	long long objTotal = 0;

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
				objTotal++;
				int idx = 0;
				const bool useShared = roth::Pack::ResolveObjectArt(
					o.textureIndex, o.textureSource, idx);
				if (useShared && !haveShared) continue;
				const roth::Pack &p = useShared ? shared : level;
				const roth::FatEntry *e = p.Entry(idx);
				if (e == nullptr || e->kind != roth::EntryKind::Object3D) continue;

				Placed pl;
				pl.map = name; pl.pack = useShared ? "SHARED" : pk;
				pl.index = idx; pl.x = o.x; pl.y = o.y; pl.z = o.z; pl.rot = o.rotation;
				pl.shared = useShared;
				placed.push_back(pl);
				char key[64];
				snprintf(key, sizeof key, "%s[%d]", pl.pack.c_str(), idx);
				perEntry[key]++;
			}
	}

	printf("\nplaced objects %lld, of which MESHES %u\n", objTotal, (unsigned)placed.size());
	printf("\nby entry, most placed first\n");
	{
		std::vector<std::pair<long long, std::string>> v;
		for (const auto &kv : perEntry) v.push_back({ kv.second, kv.first });
		std::sort(v.begin(), v.end(), [](const std::pair<long long, std::string> &a,
			const std::pair<long long, std::string> &b) { return a.first > b.first; });
		for (size_t i = 0; i < v.size() && (verbose || i < 20); i++)
			printf("    %-16s x%lld\n", v[i].second.c_str(), v[i].first);
		printf("    (%u distinct mesh entries placed)\n", (unsigned)v.size());
	}

	// A CAMERA SPOT FOR EACH OF THE TWO MEASURED ENTRIES. ROTH angles are
	// 1/512 of a turn; 0 looks along +X. Standing back along -X and looking
	// at +X puts the prop in front of the camera.
	printf("\nwhere the two measured fans are -- aim a camera here\n");
	for (const Placed &pl : placed)
	{
		if (pl.index != 4123 && pl.index != 4128) continue;
		printf("    %-10s %s[%d]  at (%d, %d, %d) rot %d"
			"   ->  rothdiff_shot %d %d 0\n",
			pl.map.c_str(), pl.pack.c_str(), pl.index, pl.x, pl.y, pl.z, pl.rot,
			pl.x - 600, pl.y);
	}

	// And the first few of any mesh, in case neither of those two is placed.
	printf("\nthe first placed meshes in each map that has one\n");
	{
		std::set<std::string> done;
		for (const Placed &pl : placed)
		{
			if (!done.insert(pl.map).second) continue;
			printf("    %-10s %s[%d] at (%d, %d, %d)  ->  rothdiff_shot %d %d 0\n",
				pl.map.c_str(), pl.pack.c_str(), pl.index, pl.x, pl.y, pl.z,
				pl.x - 600, pl.y);
		}
	}
	return 0;
}
