//
// sfx_test -- the sound RULES in src/roth/roth_sfx.cpp, checked against cases
// worked by hand from ROTH.C (engine/audio.c). No retail data needed.
// With a folder argument it also walks every retail map's nodes.
//
#include "roth_sfx.h"
#include "roth_raw.h"
#include "roth_install.h"
#include <stdio.h>
#include <string.h>

using namespace roth;

static int gBad = 0;
#define CHECK(cond) do { if (!(cond)) { gBad++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main(int argc, char **argv)
{
	// ---- the volume law, audio.c:65-114 ----
	uint32_t d = 0;
	// At the emitter: full volume, scaled only by the master's 0x7fff/0x8000.
	CHECK(SfxVolume(1000, 0x40, nullptr, 0, 0, 0, 0, 0, 0x7fff, &d) == (int32_t)((0x7fffu * 0x7fffu) >> 15));
	CHECK(d == 0);
	// Half way: 0x7fff - 500 * 0x7fff / 1000 = 0x7fff - 16383 = 16384.
	CHECK(SfxVolume(1000, 0x40, nullptr, 0, 0, 500, 0, 250000, 0x8000, &d) == 16384);
	CHECK(d == 500);
	// Past the range the first result is negative: silence.
	CHECK(SfxVolume(1000, 0x40, nullptr, 0, 0, 1001, 0, 1001 * 1001, 0x7fff, nullptr) == 0);
	// Exactly at the range: 0x7fff - 0x7fff = 0.
	CHECK(SfxVolume(1000, 0x40, nullptr, 0, 0, 1000, 0, 1000000, 0x7fff, nullptr) == 0);
	// Sample volume 0x20 halves it: (16384 * 32) >> 6 = 8192.
	CHECK(SfxVolume(1000, 0x20, nullptr, 0, 0, 500, 0, 250000, 0x8000, nullptr) == 8192);
	// Range 0 and master 0 are silence.
	CHECK(SfxVolume(0, 0x40, nullptr, 0, 0, 0, 0, 0, 0x7fff, nullptr) == 0);
	CHECK(SfxVolume(1000, 0x40, nullptr, 0, 0, 0, 0, 0, 0, nullptr) == 0);

	// Zones: rects are relative to the emitter; the first hit ends the scan.
	SfxZone z;
	z.count = 2;
	z.rects[0] = { 10, 0, -100, -100, 100, 100 };       // plain: hit when OUTSIDE
	z.rects[1] = { 50, 1, -100, -100, 100, 100 };       // inverted: hit when strictly INSIDE
	// Player inside rect 0 (no hit), strictly inside rect 1 (hit): -(50 << 7).
	CHECK(SfxVolume(1000, 0x40, &z, 1000, 1000, 1050, 1000, 2500, 0x8000, nullptr)
		== (0x7fff - (50 * 0x7fff) / 1000) - (50 << 7));
	// Player outside rect 0: that hits first, -(10 << 7), and rect 1 is never tried.
	CHECK(SfxVolume(1000, 0x40, &z, 1000, 1000, 1200, 1000, 40000, 0x8000, nullptr)
		== (0x7fff - (200 * 0x7fff) / 1000) - (10 << 7));
	// On the edge of an inverted rect is not "strictly inside".
	z.count = 1; z.rects[0] = { 50, 1, -100, -100, 100, 100 };
	CHECK(SfxVolume(1000, 0x40, &z, 0, 0, 100, 0, 10000, 0x8000, nullptr) == 0x7fff - (100 * 0x7fff) / 1000);
	// Dampened below zero is silence.
	z.rects[0] = { 255, 1, -100, -100, 100, 100 };
	CHECK(SfxVolume(1000, 0x40, &z, 0, 0, 50, 0, 2500, 0x8000, nullptr) == 0);

	// ---- the slew and the loop's starting volume ----
	CHECK(SfxClampDiff200(0x4000, 0) == 0x200);
	CHECK(SfxClampDiff200(0, 0x4000) == 0x4000 - 0x200);
	CHECK(SfxClampDiff200(0x100, 0) == 0x100);
	CHECK(SfxLoopStartVolume(0x7000, 1000) == (0x7ff - 1000) << 4);     // 1047 << 4 = 16752
	CHECK(SfxLoopStartVolume(0x7000, 3000) == 0x10 << 4);                // floor of 0x10
	CHECK(SfxLoopStartVolume(0x80, 1000) == 0x80);

	// ---- isqrt ----
	CHECK(SfxIsqrt(0) == 0 && SfxIsqrt(1) == 1 && SfxIsqrt(15) == 3 && SfxIsqrt(16) == 4);
	CHECK(SfxIsqrt(1000000) == 1000 && SfxIsqrt(999999) == 999);

	// ---- the node pump, audio.c:605-648 ----
	SfxSection s;
	s.nodes.resize(4);
	s.nodes[0] = { 100, 0, 1, 10, 0x81, 0, 500, 0, 0, 0x40, 0 };        // looping, active
	s.nodes[1] = { 50, 0, 2, 11, 0x01, 0, 500, 0, 0, 0x40, 0 };         // looping, OFF
	s.nodes[2] = { 20, 0, 3, 12, 0x82, 0, 500, 10, 0, 0x40, 0 };        // periodic, active
	s.nodes[3] = { 10, 0, 4, 13, 0x80, 0, 5, 0, 0, 0x40, 0 };           // one-shot, range 5
	s.Setup();
	CHECK(s.nodes[2].timer == 35);                                      // (7 * 10) >> 1
	CHECK(s.nodes[0].timer == 0);
	std::vector<SfxQueryEntry> q = SfxNodePump(s, 0, 0, 2, 0);
	// Node 0 only: 1 is off, 2 has not expired (35 -> 33), 3 is out of range (10 > 5).
	CHECK(q.size() == 1 && q[0].node == 0 && q[0].dist2 == 10000);
	CHECK(s.nodes[2].timer == 33);
	for (int i = 0; i < 16; i++) q = SfxNodePump(s, 0, 0, 2, 0);        // 33 -> 1
	CHECK(s.nodes[2].timer == 1 && q.size() == 1);
	q = SfxNodePump(s, 0, 0, 2, 0);                                     // 1 < 2: expires, reloads
	// timer = (1 - 2) & 0xffff = 0xffff; + 35 carries -> 34. Plays this pass, nearest first.
	CHECK(s.nodes[2].timer == 34);
	CHECK(q.size() == 2 && q[0].node == 2 && q[1].node == 0);
	// At the edge of the range: collected at exactly range, not one beyond.
	q = SfxNodePump(s, 100 + 500, 0, 2, 0);
	CHECK(q.size() == 1 && q[0].node == 0);
	q = SfxNodePump(s, 100 + 501, 0, 2, 0);
	CHECK(q.empty());
	CHECK(s.FindByKey(12) == 2 && s.FindByKey(99) == -1);

	// ---- the speech step table, renderer.c:10115-10132 ----
	int32_t t[256];
	BuildDpcmStepTable(t);
	CHECK(t[0] == 0 && t[1] == 2 && t[2] == -2 && t[3] == 5 && t[4] == -5);
	for (int i = 1; i < 255; i += 2) CHECK(t[i] == -t[i + 1] && t[i] > 0);
	CHECK(t[255] > t[253]);

	// ---- a WAVE image ----
	const uint8_t pcm[4] = { 1, 2, 3, 4 };
	const std::vector<uint8_t> w = MakeWave(pcm, 4, 22050, 16);
	CHECK(w.size() == 48 && memcmp(w.data(), "RIFF", 4) == 0 && memcmp(w.data() + 36, "data", 4) == 0);
	CHECK(w[24] == 0x22 && w[25] == 0x56 && w[34] == 16 && w[40] == 4);

	// ---- every retail node, if a folder was given ----
	if (argc > 1)
	{
		Install install;
		if (!install.Open(argv[1])) { printf("not an install: %s\n", install.Error().c_str()); return 2; }
		int nodes = 0, heardAtCentre = 0, silentBeyond = 0, zoneCut = 0;
		for (const std::string &name : install.MapNames())
		{
			const std::vector<uint8_t> raw = Install::ReadWholeFile(install.MapFile(name.c_str()));
			const Map m = ParseRaw(raw.data(), raw.size());
			SfxSection sec;
			sec.Parse(m.sfxSection.data(), m.sfxSection.size());
			for (const SfxNode &n : sec.nodes)
			{
				nodes++;
				const SfxZone *zone = sec.Zone(n.zone);
				// Standing on the node: heard unless its own volume is 0 or a zone cuts it.
				const int32_t v0 = SfxVolume(n.range, n.volume, nullptr, n.x, n.y, n.x, n.y, 0, 0x7fff, nullptr);
				const int32_t vz = SfxVolume(n.range, n.volume, zone, n.x, n.y, n.x, n.y, 0, 0x7fff, nullptr);
				if (v0 > 0) heardAtCentre++;
				if (vz < v0) zoneCut++;
				// One unit past the range along x: always silent.
				const int32_t r1 = (int32_t)n.range + 1;
				if (SfxVolume(n.range, n.volume, zone, n.x, n.y, n.x + r1, n.y, (uint32_t)r1 * (uint32_t)r1, 0x7fff, nullptr) == 0)
					silentBeyond++;
			}
		}
		printf("retail: %d nodes; %d audible standing on them (the rest have volume 0); %d cut by their own zone there;"
			" %d silent one unit past their range\n", nodes, heardAtCentre, zoneCut, silentBeyond);
		CHECK(silentBeyond == nodes);
	}

	printf("%s\n", gBad == 0 ? "sfx_test: all passed" : "sfx_test: FAILED");
	return gBad == 0 ? 0 : 1;
}
