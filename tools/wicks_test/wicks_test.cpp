// wicks_test.cpp -- engine-free tests of src/roth/roth_wicks (a candle's wicks, one by one), against the
// player's own install. Prints numbers only; nothing derived from the retail art is written anywhere.
//
//   wicks_test <DATA> compose      every object whose click chain swaps its own picture, in every map: the
//                                  pairs that qualify as candles, and for each: the full composite against
//                                  the lit picture (every frame, every texel), the empty composite against
//                                  the unlit picture, every changed texel owned by exactly one wick, the
//                                  assignment (by column / by nearest / ties) and where the nearest ones went
//   wicks_test state               the per-wick state machine (WickLighting): every order of 1..5 wicks, with
//                                  refused first uses, late and early touches, the catch, the light's growth
//   wicks_test <DATA> scripts DIR  the oracle scripts of tools/candles_test/run_diff.sh: one reference (one
//                                  click) and one per wick-by-wick scenario, the clicks being exactly the uses
//                                  WickLighting asks for
// Exit 0 = every check held.
//
// Build: sh tools/wicks_test/build.sh   (writes /tmp/wicks_test)

#include "roth_wicks.h"
#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace roth;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}
struct LoadedPack { std::vector<uint8_t> bytes; Pack pack; bool ok = false; };
static std::map<std::string, LoadedPack *> gPacks;
static LoadedPack *GetPack(const std::string &name)
{
	auto it = gPacks.find(name);
	if (it != gPacks.end()) return it->second;
	auto *lp = new LoadedPack;
	lp->bytes = ReadFile(TheInstall().PackFile(name.c_str()));
	lp->ok = !lp->bytes.empty() && lp->pack.Load(lp->bytes.data(), lp->bytes.size());
	gPacks[name] = lp;
	return lp;
}
static std::string Resolve(const std::string &mapPack, uint16_t word, int &idx)
{
	const bool sh = Pack::ResolveObjectArt((uint8_t)(word & 0xff), (uint8_t)(word >> 8), idx);
	return sh ? TheInstall().SharedPack() : mapPack;
}
static WickPicture Pic(const std::string &pack, int idx, bool rle = true)
{
	LoadedPack *lp = GetPack(pack);
	if (!lp->ok) return WickPicture();
	return WickPictureFromImage(lp->pack.ReadImage(idx, true, rle), lp->pack.ReadAnchor(idx));
}

// ---------------------------------------------------------------- compose
static int Compose()
{
	struct Kind { std::string unlit, lit; std::set<std::string> where; int item = 0; };
	std::map<std::pair<std::string, std::string>, Kind> kinds;
	int objects = 0;
	for (const std::string &name : TheInstall().MapNames())
	{
		std::vector<uint8_t> raw = ReadFile(TheInstall().MapFile(name.c_str()));
		if (raw.empty()) continue;
		Map m = ParseRaw(raw.data(), raw.size());
		if (!m.ok()) continue;
		const std::string pack = TheInstall().PackFor(name.c_str());
		for (const LightableObject &L : FindLightableObjects(m))
		{
			int iu = 0, il = 0;
			const std::string pu = Resolve(pack, L.unlitWord, iu), pl = Resolve(pack, L.litWord, il);
			char ku[48], kl[48];
			snprintf(ku, sizeof ku, "%s[%d]", pu.c_str(), iu);
			snprintf(kl, sizeof kl, "%s[%d]", pl.c_str(), il);
			Kind &k = kinds[{ ku, kl }];
			k.unlit = ku; k.lit = kl; k.item = L.item;
			char w[64]; snprintf(w, sizeof w, "%s %d/%d #%u", name.c_str(), L.sector, L.index, (unsigned)L.trigger);
			k.where.insert(w);
			objects++;
		}
	}
	printf("COMPOSE: %d objects in %zu (unlit, lit) pairs swap their own picture from their click chain (0x0d, key 0)\n", objects, kinds.size());
	int candles = 0;
	for (auto &kv : kinds)
	{
		const Kind &k = kv.second;
		auto parse = [](const std::string &key, std::string &pk, int &idx) { const size_t lb = key.find('['); pk = key.substr(0, lb); idx = atoi(key.c_str() + lb + 1); };
		std::string pu, pl; int iu, il;
		parse(k.unlit, pu, iu); parse(k.lit, pl, il);
		const WickPicture U = Pic(pu, iu), L = Pic(pl, il);
		const WickModel M = BuildWickModel(U, L);
		printf("\nPAIR %s -> %s  (%zu object(s): %s%s; item tested %d)\n", k.unlit.c_str(), k.lit.c_str(), k.where.size(), k.where.begin()->c_str(),
			k.where.size() > 1 ? ", ..." : "", k.item);
		if (!M.ok) { printf("  not a candle: %s\n", M.why.c_str()); continue; }
		candles++;
		// The engine registers object art from ReadImage(index, true) (TextureSet::Sprite) and the state pictures
		// from ReadImage(index, true, true) (TextureSet::Frames): the lit picture must be the same both ways.
		const WickPicture L2 = Pic(pl, il, false);
		CHECK(L2.frames == L.frames, "%s decodes differently with and without run-length frames", k.lit.c_str());
		printf("  %d x %d texels, %.0f u/t, %d lit frames; unlit texel (x, y) = lit (x %+d, y %+d); %d changed texels, %d flicker; %d wick(s)\n",
			M.W, M.H, M.upp, M.frames, M.dx, M.dy, M.changed, M.flicker, M.Wicks());
		for (int f = 0; f < M.Wicks(); f++)
		{
			const WickFlame &F = M.flames[f];
			printf("    wick %d: flame box x %d..%d y %d..%d (%d texels), wick point (%.1f, %d) lit texels = %+.1f across, %.0f up from the actor's z;"
				" owns %d changed texels\n", f + 1, F.x0, F.x1, F.y0, F.y1, F.texels, F.wx, F.wy, F.across, F.up, M.perFlame[f]);
		}
		printf("  assignment: %d texels inside one flame's columns, %d to the nearest wick across (%d of them a tie across, settled vertically or leftmost)\n",
			M.inColumn, M.byNearest, M.ties);
		// Where the nearest-across texels went, as the groups they form.
		{
			std::vector<uint8_t> amb((size_t)M.W * M.H, 0);
			for (size_t i = 0; i < amb.size(); i++)
			{
				if (M.owner[i] < 0) continue;
				const int x = (int)(i % M.W);
				const WickFlame &F = M.flames[M.owner[i]];
				if (!(x >= F.x0 && x <= F.x1)) amb[i] = 1;
			}
			// rows of runs, per owner
			std::map<int, std::vector<std::string>> runs;
			for (int y = 0; y < M.H; y++)
				for (int x = 0; x < M.W; )
				{
					const size_t i = (size_t)y * M.W + x;
					if (!amb[i]) { x++; continue; }
					const int o = M.owner[i];
					int x2 = x;
					while (x2 + 1 < M.W && amb[(size_t)y * M.W + x2 + 1] && M.owner[(size_t)y * M.W + x2 + 1] == o) x2++;
					char b[40]; snprintf(b, sizeof b, "y%d x%d..%d", y, x, x2);
					runs[o].push_back(b);
					x = x2 + 1;
				}
			for (auto &r : runs)
			{
				printf("    to wick %d by nearest across:", r.first + 1);
				for (const std::string &s : r.second) printf(" %s", s.c_str());
				printf("\n");
			}
		}
		// Every changed texel owned once; nothing unchanged owned.
		int bad = 0;
		for (size_t i = 0; i < M.owner.size(); i++)
		{
			bool ch = false;
			for (int f = 0; f < M.frames; f++) if (M.lit[f][i] != M.unlit[i]) ch = true;
			if (ch != (M.owner[i] >= 0) || M.owner[i] >= M.Wicks()) bad++;
		}
		CHECK(bad == 0, "%d texels owned wrongly", bad);
		int sum = 0; for (int n : M.perFlame) sum += n;
		CHECK(sum == M.changed, "the owned texels (%d) are not the changed ones (%d)", sum, M.changed);
		// The full composite IS the lit picture, every frame; the empty one IS the unlit picture.
		std::vector<uint8_t> c;
		long diffFull = 0, diffZero = 0, padZero = 0;
		for (int f = 0; f < M.frames; f++)
		{
			ComposeWicks(M, M.FullMask(), f, c);
			for (size_t i = 0; i < c.size(); i++) diffFull += c[i] != L.frames[f][i];
			ComposeWicks(M, 0, f, c);
			for (int y = 0; y < M.H; y++)
				for (int x = 0; x < M.W; x++)
				{
					const int ux = x - M.dx, uy = y - M.dy;
					const bool inside = ux >= 0 && uy >= 0 && ux < U.W && uy < U.H;
					const uint8_t v = c[(size_t)y * M.W + x];
					if (inside) diffZero += v != U.At(0, ux, uy);
					else padZero += v != 0;
				}
		}
		CHECK(diffFull == 0, "full composite differs from %s in %ld texels", k.lit.c_str(), diffFull);
		CHECK(diffZero == 0 && padZero == 0, "empty composite differs from %s in %ld texels (%ld outside)", k.unlit.c_str(), diffZero, padZero);
		// Each single wick adds exactly its own texels; the masks are additive.
		long extra = 0;
		for (uint32_t mask = 0; mask <= M.FullMask(); mask++)
			for (int f = 0; f < M.frames; f++)
			{
				ComposeWicks(M, mask, f, c);
				for (size_t i = 0; i < c.size(); i++)
				{
					const int o = M.owner[i];
					const uint8_t want = (o >= 0 && (mask >> o & 1)) ? L.frames[f][i] : M.unlit[i];
					extra += c[i] != want;
				}
			}
		CHECK(extra == 0, "%ld texels of the %u masks are not their wicks' own", extra, M.FullMask() + 1);
		printf("  checked: full composite = %s in %d frame(s) x %d texels: %ld differ; empty composite = %s (%d x %d at %+d,%+d): %ld differ, %ld"
			" padding texels not clear; %u masks x %d frames, each texel its own wick's: %ld differ\n",
			k.lit.c_str(), M.frames, M.W * M.H, diffFull, k.unlit.c_str(), U.W, U.H, M.dx, M.dy, diffZero, padZero, M.FullMask() + 1, M.frames, extra);
	}
	printf("\nCOMPOSE: %d candle pair(s); %s\n", candles, g_fail ? "FAILURES above" : "every check held");
	return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------- state
// A scenario: (tic, wick) touches; the use asked on a wick starts the lighting unless `refuse` > 0 (the first
// `refuse` uses are refused, as the item test would without the item in the hand).
struct Touch { int tic; int wick; };
struct Outcome { int uses = 0; uint32_t mask = 0; int caughtAt = -1, allAt = -1, startedAt = -1; };
static Outcome Play(int wicks, const std::vector<Touch> &touches, int refuse, int tics, std::vector<int> *useTics = nullptr)
{
	WickLighting w;
	w.Reset(wicks);
	Outcome o;
	size_t ti = 0;
	for (int t = 0; t < tics; t++)
	{
		uint32_t near = 0;
		while (ti < touches.size() && touches[ti].tic == t) { near |= 1u << touches[ti].wick; ti++; }
		const int k = w.Flames(near);
		if (k >= 0)
		{
			o.uses++;
			if (useTics) useTics->push_back(t);
			if (refuse > 0) refuse--;
			else { w.Started(k); o.startedAt = t; }
		}
		w.Tick();
		if (w.Caught() && o.caughtAt < 0) o.caughtAt = t;
		if (w.AllLit() && o.allAt < 0) o.allAt = t;
	}
	o.mask = w.Mask();
	return o;
}

static int State()
{
	printf("STATE: WickLighting\n");
	int scenarios = 0;
	// Every order of n wicks, touched 20 tics apart, one tic each.
	for (int n = 1; n <= 5; n++)
	{
		std::vector<int> order(n);
		for (int i = 0; i < n; i++) order[i] = i;
		do
		{
			std::vector<Touch> t;
			for (int i = 0; i < n; i++) t.push_back({ 10 + 20 * i, order[i] });
			const Outcome o = Play(n, t, 0, 700);
			scenarios++;
			CHECK(o.uses == 1, "n=%d: %d uses for one wick-by-wick lighting", n, o.uses);
			CHECK(o.mask == (1u << n) - 1, "n=%d: not all lit", n);
			CHECK(o.caughtAt < 0, "n=%d: caught although every wick was lit", n);
			CHECK(o.allAt == 10 + 20 * (n - 1), "n=%d: all lit at %d", n, o.allAt);
		} while (std::next_permutation(order.begin(), order.end()));
	}
	// Only one wick lit: the catch, 15 s (525 tics) after the first.
	{
		const Outcome o = Play(5, { { 30, 2 } }, 0, 700);
		scenarios++;
		CHECK(o.uses == 1 && o.startedAt == 30, "single wick: uses %d started %d", o.uses, o.startedAt);
		CHECK(o.caughtAt == 30 + WickLighting::CATCH_TICS - 1 || o.caughtAt == 30 + WickLighting::CATCH_TICS, "catch at %d", o.caughtAt);
		CHECK(o.mask == 31, "after the catch the mask is %x", o.mask);
		printf("  one wick lit at tic 30: the others catch at tic %d (%d tics = %.1f s later)\n", o.caughtAt, o.caughtAt - 30, (o.caughtAt - 30) / 35.0);
	}
	// Touching the same wick again and again, a hand resting on a wick, every wick at once.
	{
		std::vector<Touch> t;
		for (int k = 0; k < 200; k++) t.push_back({ 5 + k, 1 });            // resting: one edge
		for (int k = 0; k < 10; k++) t.push_back({ 300 + 3 * k, 1 });      // tapping: many edges on a lit wick
		std::sort(t.begin(), t.end(), [](const Touch &a, const Touch &b) { return a.tic < b.tic; });
		const Outcome o = Play(5, t, 0, 400);
		scenarios++;
		CHECK(o.uses == 1 && o.mask == (1u << 1), "resting and tapping one wick: uses %d mask %x", o.uses, o.mask);
		WickLighting w; w.Reset(5);
		const int k = w.Flames(31);   // every wick at once
		scenarios++;
		CHECK(k == 0 && w.UsesAsked() == 1, "every wick at once: wick %d, %d uses", k, w.UsesAsked());
		w.Started(k);
		CHECK(w.Mask() == 1, "started on wick 0: mask %x", w.Mask());
		w.Flames(31);
		CHECK(w.Mask() == 31 && w.UsesAsked() == 1, "the wicks a flame stays at light on the next tic, no use: mask %x uses %d", w.Mask(), w.UsesAsked());
	}
	// Refused uses (the matchbox not in the hand): each new edge asks again; the one that is not refused starts.
	{
		const Outcome o = Play(5, { { 10, 0 }, { 20, 0 }, { 30, 3 }, { 40, 4 }, { 50, 1 } }, 2, 700);
		scenarios++;
		CHECK(o.uses == 3 && o.startedAt == 30, "two refused: uses %d started %d", o.uses, o.startedAt);
		CHECK(o.mask == 31 && o.caughtAt >= 0, "two refused: mask %x caught %d (wicks 0 and 2 catch)", o.mask, o.caughtAt);
	}
	// Order independence: the final state of every order is the same, and the uses always one.
	{
		std::set<uint32_t> masks;
		std::set<int> uses;
		srand(1234);
		for (int r = 0; r < 2000; r++)
		{
			std::vector<int> order = { 0, 1, 2, 3, 4 };
			for (int i = 4; i > 0; i--) std::swap(order[i], order[rand() % (i + 1)]);
			const int n = 1 + rand() % 5;
			std::vector<Touch> t;
			int tic = 1 + rand() % 50;
			for (int i = 0; i < n; i++) { t.push_back({ tic, order[i] }); tic += 1 + rand() % 200; }
			const Outcome o = Play(5, t, 0, tic + 600);
			masks.insert(o.mask);
			uses.insert(o.uses);
			scenarios++;
		}
		CHECK(masks.size() == 1 && *masks.begin() == 31, "random orders end in %zu different masks", masks.size());
		CHECK(uses.size() == 1 && *uses.begin() == 1, "random orders asked %zu different use counts", uses.size());
	}
	// The light grows with each lit wick (ramps complete, the flicker at its own value).
	{
		WickModel m;
		m.ok = true;
		for (int k = 0; k < 5; k++) { WickFlame f; f.flameAcross = -20 + 10 * k; f.flameUp = 150; m.flames.push_back(f); }
		double prevR = 0, prevI = 0;
		for (int n = 1; n <= 5; n++)
		{
			double age[kMaxWicks];
			for (int k = 0; k < kMaxWicks; k++) age[k] = k < n ? 10.0 : -1.0;
			CandleLightOut out;
			const bool on = CandleLight(m, age, 3.25, out);
			scenarios++;
			CHECK(on && out.lit == n && out.radius > prevR && out.intensity > prevI, "%d wicks: radius %.1f intensity %.2f", n, out.radius, out.intensity);
			printf("  light with %d wick(s) lit: radius %.1f units, intensity %.2f x %.2f, at %+.1f across %.1f up\n", n, out.radius, out.intensity, kLightPerWick,
				out.across, out.up);
			prevR = out.radius; prevI = out.intensity;
		}
		double lo = 1, hi = 0;
		for (int i = 0; i < 7000; i++) { const double f = WickFlicker(i / 70.0, i % 5); lo = std::min(lo, f); hi = std::max(hi, f); }
		CHECK(lo >= 0.78 && hi <= 1.0, "flicker %.3f..%.3f", lo, hi);
		printf("  flicker over 100 s: %.3f .. %.3f\n", lo, hi);
	}
	printf("STATE: %d scenarios; %s\n", scenarios, g_fail ? "FAILURES above" : "every check held");
	return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------- scripts
// For tools/candles_test/run_diff.sh. Frames are the original's game frames; one tic here is one frame there
// (the scripts only need the ORDER and the spacing of the uses, which WickLighting decides).
static int Scripts(const char *dir)
{
	struct Scenario { const char *name; std::vector<Touch> touches; };
	const int at = 50;   // the first wick, the frame of the one click
	std::vector<Scenario> sc = {
		{ "order_3_5_1_4_2", { { at, 2 }, { at + 6, 4 }, { at + 11, 0 }, { at + 19, 3 }, { at + 25, 1 } } },
		{ "order_5_4_3_2_1", { { at, 4 }, { at + 3, 3 }, { at + 4, 2 }, { at + 30, 1 }, { at + 31, 0 } } },
		{ "one_wick_then_catch", { { at, 0 } } },
		{ "all_at_once", { { at, 0 }, { at, 1 }, { at, 2 }, { at, 3 }, { at, 4 } } },
		{ "tapping_one_wick", { { at, 1 }, { at + 2, 1 }, { at + 4, 1 }, { at + 6, 1 }, { at + 8, 1 }, { at + 10, 1 } } },
	};
	auto head = [](FILE *f) { fprintf(f, "40 give 16\n40 right 16\n40 snap\n"); };
	auto tail = [](FILE *f) { fprintf(f, "%d diff end\n%d quit\n", 50 + 620, 50 + 621); };
	{
		const std::string p = std::string(dir) + "/ref.script";
		FILE *f = fopen(p.c_str(), "w");
		if (!f) { printf("cannot write %s\n", p.c_str()); return 2; }
		head(f);
		fprintf(f, "%d click 211 0\n", at);
		tail(f);
		fclose(f);
	}
	for (const Scenario &s : sc)
	{
		std::vector<int> useTics;
		const Outcome o = Play(5, s.touches, 0, 700, &useTics);
		const std::string p = std::string(dir) + "/" + s.name + ".script";
		FILE *f = fopen(p.c_str(), "w");
		if (!f) return 2;
		head(f);
		for (int t : useTics) fprintf(f, "%d click 211 0\n", t);
		tail(f);
		fclose(f);
		printf("SCRIPT %s: %zu touches -> %d use(s) at frame(s)", s.name, s.touches.size(), o.uses);
		for (int t : useTics) printf(" %d", t);
		printf("; all lit at %d, caught at %d\n", o.allAt, o.caughtAt);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "state")) return State();
	if (argc < 3) { printf("usage: wicks_test <DATA> compose | wicks_test state | wicks_test <DATA> scripts DIR\n"); return 2; }
	if (!strcmp(argv[2], "scripts")) return argc >= 4 ? Scripts(argv[3]) : 2;
	if (!TheInstall().Open(argv[1])) { printf("install: %s\n", TheInstall().Error().c_str()); return 2; }
	if (!strcmp(argv[2], "compose")) return Compose();
	return 2;
}
