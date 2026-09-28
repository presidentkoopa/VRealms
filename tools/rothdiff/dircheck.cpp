// dircheck.cpp -- the offline directional-art audit, over all 44 maps.
//
// Directional art is the art whose picture depends on the angle you view the
// object from. REMAROTH does not draw it at all: roth_objects.cpp classifies
// the entry as an Indirection and drops it. This tool answers, without an
// engine and without a window, the questions that decide how it gets drawn:
//
//   - which packs carry a directional block, and how big it is;
//   - whether flags_1 == 0x20 / 0x24 really are the only indirection values
//     (the original tests the whole byte, not single bits -- das_assets.c:886);
//   - how many directional entries resolve, into eight views or sixteen;
//   - whether the frames they name are real pictures, and how many are
//     mirrored;
//   - how many placed objects in the retail maps actually reference one, which
//     is the size of what is missing from the screen today.
//
// It reads the player's own .RAW and .DAS through the same reader the loader
// uses, and writes nothing.
//
// Build:
//   i686-w64-mingw32-g++ -std=c++17 -O1 -static -I ../../src/roth \
//       -o dircheck.exe dircheck.cpp \
//       ../../src/roth/roth_raw.cpp ../../src/roth/roth_install.cpp \
//       ../../src/roth/roth_das.cpp ../../src/roth/roth_palette.cpp

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <set>

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}

struct Totals
{
	long long entriesDirectional = 0, entriesCreature = 0;
	long long resolved8 = 0, resolved16 = 0;
	long long unresolvedFixed = 0, unresolvedRange = 0;
	long long framesTotal = 0, framesMirrored = 0, framesDistinct = 0;
	long long framesPicture = 0, framesNotPicture = 0, framesOutOfPack = 0;
	long long objectsDirectional = 0, objectsCreature = 0, objectsTotal = 0;
	long long objectsDirectionalFixed = 0, objectsDirectionalFlipped = 0;
};

// Every value flags_1 actually takes, so the 0x20 / 0x24 reading is checked
// against the files rather than assumed from the lifted code.
static std::map<int, long long> gFlags1;
static std::map<std::string, long long> perMapDirectional;

static void AuditPack(const roth::Pack &pack, const char *label, Totals &t,
	std::set<std::string> &packsSeen, bool verbose,
	const roth::Pack *shared)
{
	if (!packsSeen.insert(label).second) return;

	long long dirHere = 0;
	for (int i = 0; i < pack.Count(); i++)
	{
		const roth::FatEntry *e = pack.Entry(i);
		if (e == nullptr || e->size == 0) continue;
		gFlags1[e->flags1]++;

		const roth::IndirectKind k = pack.Indirect(i);
		if (k == roth::IndirectKind::Creature) { t.entriesCreature++; continue; }
		if (k != roth::IndirectKind::Directional) continue;

		t.entriesDirectional++;
		dirHere++;

		const roth::Directional d = pack.ReadDirectional(i);
		if (!d.ok())
		{
			// Either a fixed frame the file does not resolve, or a record
			// reaching outside the block. Told apart by whether the pack has a
			// block at all and whether the index fits.
			if (!pack.HasDirectional()) t.unresolvedRange++;
			else t.unresolvedFixed++;
			continue;
		}
		if (d.count == 16) t.resolved16++; else t.resolved8++;

		std::set<uint16_t> distinct;
		for (int f = 0; f < d.count; f++)
		{
			t.framesTotal++;
			if (d.frames[f].mirror) t.framesMirrored++;
			distinct.insert(d.frames[f].entry);

			// A frame is named by a GLOBAL das id, so it may belong to the
			// other pack. This is the same resolution the original's
			// select_das_fat_entry does, and without it a fifth of the frames
			// look like they point off the end of the world.
			int fidx = 0;
			const bool fromShared = roth::Pack::ResolveDasId(d.frames[f].entry, fidx);
			const roth::Pack *fp = fromShared ? shared : &pack;
			const roth::FatEntry *fe = fp ? fp->Entry(fidx) : nullptr;
			if (fe == nullptr)
			{
				t.framesOutOfPack++;
				if (verbose && t.framesOutOfPack <= 24)
					printf("    out of pack: entry %5d view %2d -> id %5d "
						"(%s index %d)\n", i, f, d.frames[f].entry,
						fromShared ? "shared" : "own", fidx);
				continue;
			}
			// A frame must be a picture: if these come back as indirections or
			// empties the record layout is wrong.
			if (fe->kind == roth::EntryKind::Plain ||
				fe->kind == roth::EntryKind::Animated ||
				fe->kind == roth::EntryKind::ImagePack)
				t.framesPicture++;
			else
				t.framesNotPicture++;
		}
		t.framesDistinct += (long long)distinct.size();

		if (verbose && dirHere <= 3)
		{
			printf("    entry %5d  flags2 %3d  %2d views :", i, e->flags2, d.count);
			for (int f = 0; f < d.count && f < 8; f++)
				printf(" %d%s", d.frames[f].entry, d.frames[f].mirror ? "M" : "");
			printf("%s\n", d.count > 8 ? " ..." : "");
		}
	}

	printf("  %-10s  directional block %-3s  entries %4lld\n",
		label, pack.HasDirectional() ? "yes" : "no", dirHere);
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: dircheck <ROTH folder> [-v]\n"); return 2; }
	const bool verbose = (argc > 2 && std::string(argv[2]) == "-v");
	if (!roth::TheInstall().Open(argv[1])) { printf("install: bad\n"); return 2; }

	Totals t;
	std::set<std::string> packsSeen;

	// The shared pack first: object art routes to it for two of the four
	// texture sources, so it carries the props that appear in every map.
	std::vector<uint8_t> sd = ReadFile(roth::TheInstall().PackFile(
		roth::TheInstall().SharedPack().c_str()));
	roth::Pack shared;
	const bool haveShared = !sd.empty() && shared.Load(sd.data(), sd.size());
	printf("packs\n");
	if (haveShared) AuditPack(shared, "SHARED", t, packsSeen, verbose, &shared);
	else printf("  SHARED      not opened\n");

	for (const std::string &name : roth::TheInstall().MapNames())
	{
		std::vector<uint8_t> pd = ReadFile(roth::TheInstall().PackFile(
			roth::TheInstall().PackFor(name.c_str()).c_str()));
		if (pd.empty()) continue;
		roth::Pack pack;
		if (!pack.Load(pd.data(), pd.size())) continue;
		AuditPack(pack, roth::TheInstall().PackFor(name.c_str()).c_str(), t,
			packsSeen, verbose, haveShared ? &shared : nullptr);
	}

	// How many placed objects reference directional art. This is the number
	// that says what is missing from the screen, rather than what is in the
	// files.
	for (const std::string &name : roth::TheInstall().MapNames())
	{
		std::vector<uint8_t> d = ReadFile(roth::TheInstall().MapFile(name.c_str()));
		if (d.empty()) continue;
		roth::Map m = roth::ParseRaw(d.data(), d.size());
		if (!m.ok()) continue;

		std::vector<uint8_t> pd = ReadFile(roth::TheInstall().PackFile(
			roth::TheInstall().PackFor(name.c_str()).c_str()));
		if (pd.empty()) continue;
		roth::Pack level;
		if (!level.Load(pd.data(), pd.size())) continue;

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
				const roth::IndirectKind k = p.Indirect(idx);
				if (k == roth::IndirectKind::Directional)
				{
					t.objectsDirectional++;
					perMapDirectional[name]++;
					// renderType bit 7 draws a picture as a fixed-angle quad
					// rather than turning it to face the camera. That would
					// contradict a view-dependent frame, so whether the two
					// ever coincide decides a real branch in roth_objects.cpp.
					if (o.FixedAngle()) t.objectsDirectionalFixed++;
					if (o.HorizontalFlip()) t.objectsDirectionalFlipped++;
				}
				else if (k == roth::IndirectKind::Creature) t.objectsCreature++;
			}
	}

	printf("\nflags_1 values seen across every pack\n");
	for (const auto &kv : gFlags1)
		printf("  0x%02x  %8lld%s\n", kv.first, kv.second,
			kv.first == 0x20 ? "   <- directional" :
			kv.first == 0x24 ? "   <- creature" : "");

	printf("\nentries\n");
	printf("  directional (flags_1 0x20)   %8lld\n", t.entriesDirectional);
	printf("  creature    (flags_1 0x24)   %8lld\n", t.entriesCreature);
	printf("  resolved, eight views        %8lld\n", t.resolved8);
	printf("  resolved, sixteen views      %8lld\n", t.resolved16);
	printf("  unresolved, fixed frame      %8lld\n", t.unresolvedFixed);
	printf("  unresolved, out of range     %8lld\n", t.unresolvedRange);

	printf("\nframes named by the resolved entries\n");
	printf("  total                        %8lld\n", t.framesTotal);
	printf("  distinct pictures            %8lld\n", t.framesDistinct);
	printf("  mirrored (frame bit 15)      %8lld\n", t.framesMirrored);
	printf("  are a picture                %8lld\n", t.framesPicture);
	printf("  are NOT a picture            %8lld\n", t.framesNotPicture);
	printf("  outside the pack             %8lld\n", t.framesOutOfPack);

	printf("\nplaced objects in the retail maps\n");
	printf("  drawn objects total          %8lld\n", t.objectsTotal);
	printf("  want directional art         %8lld\n", t.objectsDirectional);
	printf("  want creature art            %8lld\n", t.objectsCreature);
	printf("  of the directional: also fixed-angle %lld, also x-flipped %lld\n",
		t.objectsDirectionalFixed, t.objectsDirectionalFlipped);

	printf("\nmaps carrying directional objects\n");
	for (const auto &kv : perMapDirectional)
		printf("  %-12s %4lld\n", kv.first.c_str(), kv.second);

	// The audit is only meaningful if the frames really are pictures. Say so
	// plainly rather than leaving it to be read off the numbers.
	const bool sane = t.framesTotal > 0 && t.framesNotPicture == 0 &&
		t.framesOutOfPack == 0;
	printf("\n%s\n", sane
		? "RECORD LAYOUT HOLDS: every frame named resolves to a picture."
		: "RECORD LAYOUT SUSPECT: frames do not all resolve to pictures.");
	return sane ? 0 : 1;
}
