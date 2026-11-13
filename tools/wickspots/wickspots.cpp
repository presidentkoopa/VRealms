// wickspots.cpp -- where the flames sit on Realms of the Haunting's candle pictures, measured from the
// player's own install, so a VR layer can light (and draw) one wick at a time (docs/WORLD_DESIGN.md 2.3,
// owner's decision 2: "each wick swaps to lit as it catches").
//
// Engine-free; reads the .RAW maps and the .DAS packs through the src/roth readers. Prints numbers only,
// unless asked for pictures (-sheet DIR), which are the retail art and go to /tmp, never into the repo.
//
//   wickspots <DATA>              every candle kind of WORLD_DESIGN.md 2.1: the pairs, the flames, the checks
//   wickspots <DATA> -sheet DIR   also DIR/<unlit>__<lit>.ppm: unlit (aligned) | lit frame 0 | change mask
//                                 (white changed, amber changed and flickering), 4x, with each flame's box
//                                 (green), centroid (blue) and wick point (red), other changes boxed magenta
//
// Build: sh tools/wickspots/build.sh   (writes /tmp/wickspots)
//
// HOW THE PAIRS ARE FOUND (not typed in). For every object click record (category 1, opcode 0x08) in every
// map, the chain is walked (NEXT from the start, into 0x40 sub-chains); a 0x0d record with key 0 changes the
// CLICKED object's art word to word +0x0c (cmd_change_object_texture, IMMEDIATE path, raw_commands.c:4520-
// 4549: obj[4] = word[rec+0xc]). The pair is (art of the objects keyed to the record, that new art).
//
// HOW AN UNLIT AND A LIT PICTURE ARE LINED UP. Both are billboards (the entries are Plain or Animated, not
// directional, and no placement of these kinds is fixed-angle: world_census "fix" 0), drawn bottom-centre on
// the object's anchor (proprender.cpp drawBillboard, the engine's rule): units per texel `upp` = 2, or by the
// IM_HALF_SIZE shift (renderer.c:6512-6525), bottom = z - shift, shift = (modifier & 0x0f) * 2 + the entry's
// anchor y. So a texel row r of a picture H texels tall stands at height (H - r) * upp - shift above the
// anchor, and texel column c at (c + 0.5 - W / 2) * upp across. The unlit picture is mapped into the lit
// picture's texel grid by those two rules; a half-texel horizontal misfit is reported, not hidden.
//
// WHAT "CHANGED" MEANS. A lit texel is changed when, in ANY animation frame, its palette index differs from
// the unlit texel at the same place (transparent counts as a value; outside the unlit picture is
// transparent). Changed texels are grouped 8-connected. For each group: its box, its centroid, and its WICK
// POINT, the middle of the group's lowest row (where a flame meets its wick). Positions are given in lit
// texels (x right, y down from the top-left) and in world units from the anchor (x across, h up).
// The pictures' own animation is reported separately: texels whose index differs between the lit frames.
//
// TRANSPARENCY. Index 0 is read as a hole in every frame, the engine's rule for object art (roth_texture.h,
// the ObjectArt comment). Four lit candles (DEMO[4148], DEMO1[4186], DEMO1[4103], DEMO2[4099]) carry
// image_type 0x13, whose bit 0x02 roth_das.h calls IT_ZERO_OPAQUE; read that way they would be black boxes.
// Which rule ROTH.C's sprite path applies to them was not read for this tool.

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
	gPacks[name] = lp; return lp;
}

static std::string KeyOf(const std::string &pack, int index) { char b[64]; snprintf(b, sizeof b, "%s[%d]", pack.c_str(), index); return b; }

// A picture as the renderer places it: texels row-major (W across, H down), -1 = transparent.
struct Pic
{
	std::string key, kind;
	int W = 0, H = 0, frames = 0;
	double upp = 2.0, shift = 0;
	bool hang = false;
	bool zeroOpaqueBit = false;            // image_type has bit 0x02 (IT_ZERO_OPAQUE) yet is read with 0 as a hole
	std::vector<std::vector<int>> px;      // [frame][y * W + x]
	bool ok() const { return W > 0 && H > 0 && !px.empty(); }
	int At(int f, int x, int y) const { return (x < 0 || y < 0 || x >= W || y >= H) ? -1 : px[f][(size_t)y * W + x]; }
};

static Pic Load(const std::string &pack, int index)
{
	Pic p; p.key = KeyOf(pack, index);
	LoadedPack *lp = GetPack(pack);
	if (!lp->ok) return p;
	const FatEntry *e = lp->pack.Entry(index);
	if (!e) return p;
	p.kind = e->kind == EntryKind::Plain ? "picture" : e->kind == EntryKind::Animated ? "anim" : e->kind == EntryKind::Indirection ? "DIRECTIONAL" : "other";
	Image im = lp->pack.ReadImage(index, true, true);
	if (!im.ok()) return p;
	// Stored column-major: frames[f][col * im.width + row]; the picture is im.height wide (world_census WritePPM).
	p.W = im.height; p.H = im.width; p.frames = (int)im.frames.size();
	if (im.modifier & IM_HALF_SIZE) { const int s = (im.imageType >> 5) & 3; p.upp = s == 0 ? 1.0 : 2.0 * (1 << s); }
	p.hang = (im.modifier & IM_HANG) != 0;
	p.shift = double((int16_t)(uint16_t)((im.modifier & 0x0f) * 2 + (uint16_t)lp->pack.ReadAnchor(index).y));
	for (const auto &fr : im.frames)
	{
		if (fr.size() < (size_t)p.W * p.H) continue;
		std::vector<int> v((size_t)p.W * p.H);
		for (int x = 0; x < p.W; x++)
			for (int y = 0; y < p.H; y++)
			{
				const uint8_t ci = fr[(size_t)x * im.width + y];
				// Index 0 is a hole in OBJECT art whatever image_type says: the engine's object rule
				// (roth_texture.h, ObjectArt comment). Some lit candles carry image_type 0x13, whose bit 0x02
				// roth_das.h names IT_ZERO_OPAQUE; read that way their background is a black box.
				v[(size_t)y * p.W + x] = ci == 0 ? -1 : ci;
				if (ci == 0 && !im.ZeroIsTransparent()) p.zeroOpaqueBit = true;
			}
		p.px.push_back(std::move(v));
	}
	p.frames = (int)p.px.size();
	return p;
}

struct Group { int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; double cx = 0, cy = 0; double wx = 0; int wn = 0; int r = 0, g = 0, b = 0; };

static std::vector<Group> Groups(const std::vector<uint8_t> &mask, int W, int H, const Pic &lit, const std::vector<Colour> &pal, std::vector<int> *labOut = nullptr)
{
	std::vector<int> lab((size_t)W * H, -1);
	std::vector<Group> out;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			if (!mask[(size_t)y * W + x] || lab[(size_t)y * W + x] >= 0) continue;
			const int id = (int)out.size(); out.push_back({});
			std::vector<std::pair<int, int>> st = { { x, y } }; lab[(size_t)y * W + x] = id;
			std::vector<std::pair<int, int>> mem;
			while (!st.empty())
			{
				auto [cx, cy] = st.back(); st.pop_back(); mem.push_back({ cx, cy });
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++)
					{
						const int nx = cx + dx, ny = cy + dy;
						if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
						if (!mask[(size_t)ny * W + nx] || lab[(size_t)ny * W + nx] >= 0) continue;
						lab[(size_t)ny * W + nx] = id; st.push_back({ nx, ny });
					}
			}
			Group &g = out[id];
			for (auto [mx, my] : mem)
			{
				g.n++; g.x0 = std::min(g.x0, mx); g.x1 = std::max(g.x1, mx); g.y0 = std::min(g.y0, my); g.y1 = std::max(g.y1, my);
				g.cx += mx + 0.5; g.cy += my + 0.5;
				const int ci = lit.At(0, mx, my);
				if (ci >= 0 && ci < (int)pal.size()) { g.r += pal[ci].r; g.g += pal[ci].g; g.b += pal[ci].b; }
			}
			for (auto [mx, my] : mem) if (my == g.y1) { g.wx += mx + 0.5; g.wn++; }
			g.cx /= g.n; g.cy /= g.n; g.wx /= g.wn;
		}
	if (labOut) { *labOut = lab; return out; }   // unsorted, so the labels index `out`
	std::sort(out.begin(), out.end(), [](const Group &a, const Group &b) { return a.x0 != b.x0 ? a.x0 < b.x0 : a.y0 < b.y0; });
	return out;
}

// Flicker groups whose boxes intersect are one flame (a spark drawn a texel off its flame). Recomputed from the
// texels, so the box, centroid and wick point are the merged flame's.
static std::vector<Group> Flames(const std::vector<uint8_t> &mask, int W, int H, const Pic &lit, const std::vector<Colour> &pal)
{
	std::vector<int> lab;
	std::vector<Group> g = Groups(mask, W, H, lit, pal, &lab);
	std::vector<int> root(g.size());
	for (size_t i = 0; i < g.size(); i++) root[i] = (int)i;
	auto find = [&](int i) { while (root[i] != i) i = root[i] = root[root[i]]; return i; };
	for (size_t i = 0; i < g.size(); i++)
		for (size_t j = i + 1; j < g.size(); j++)
			if (g[i].x0 <= g[j].x1 && g[j].x0 <= g[i].x1 && g[i].y0 <= g[j].y1 && g[j].y0 <= g[i].y1) root[find((int)i)] = find((int)j);
	std::vector<uint8_t> merged((size_t)W * H, 0);
	std::map<int, std::vector<size_t>> by;
	for (size_t i = 0; i < lab.size(); i++) if (lab[i] >= 0) by[find(lab[i])].push_back(i);
	std::vector<Group> out;
	for (auto &kv : by)
	{
		std::vector<uint8_t> one((size_t)W * H, 0);
		for (size_t i : kv.second) one[i] = 1;
		// one merged flame: its texels need not be connected, so build its numbers directly
		Group G;
		for (size_t i : kv.second)
		{
			const int x = (int)(i % W), y = (int)(i / W);
			G.n++; G.x0 = std::min(G.x0, x); G.x1 = std::max(G.x1, x); G.y0 = std::min(G.y0, y); G.y1 = std::max(G.y1, y);
			G.cx += x + 0.5; G.cy += y + 0.5;
			const int ci = lit.At(0, x, y);
			if (ci >= 0 && ci < (int)pal.size()) { G.r += pal[ci].r; G.g += pal[ci].g; G.b += pal[ci].b; }
		}
		for (size_t i : kv.second) if ((int)(i / W) == G.y1) { G.wx += (int)(i % W) + 0.5; G.wn++; }
		G.cx /= G.n; G.cy /= G.n; G.wx /= G.wn;
		out.push_back(G);
	}
	std::sort(out.begin(), out.end(), [](const Group &a, const Group &b) { return a.x0 != b.x0 ? a.x0 < b.x0 : a.y0 < b.y0; });
	return out;
}

static void PutPPM(const std::string &path, const std::vector<Colour> &img, int W, int H)
{
	FILE *f = fopen(path.c_str(), "wb"); if (!f) return;
	fprintf(f, "P6 %d %d 255\n", W, H);
	for (const Colour &c : img) { fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
	fclose(f);
}

// The flames of a lit picture: one line each, texels and world units.
static void PrintFlames(const std::vector<Group> &fl, const Pic &lit)
{
	printf("  FLAMES (groups of texels that flicker between the lit frames), left to right. Texels: x right, y down from the lit\n"
		"  picture's top-left. World: units from the anchor, x across (right as drawn), h up.\n");
	printf("    #  box x0..x1 y0..y1   size  texels   centroid (x, y)   wick point (x, y)   wick world (x, h)\n");
	int k = 0;
	for (const Group &g : fl)
	{
		// a smaller flickering group starting within 2 rows under another, sharing its columns, is that flame's
		// glow on its cup or candle top, not a flame of its own
		std::string tag;
		for (size_t j = 0; j < fl.size(); j++)
			if (&fl[j] != &g && fl[j].n > g.n && g.x0 <= fl[j].x1 && fl[j].x0 <= g.x1 && g.y0 > fl[j].y1 && g.y0 <= fl[j].y1 + 2)
			{ char b[64]; snprintf(b, sizeof b, "   GLOW under group %zu, not a flame", j + 1); tag = b; }
		const double wxw = (g.wx - lit.W / 2.0) * lit.upp, whw = (lit.H - (g.y1 + 1)) * lit.upp - lit.shift;
		printf("   %2d  %3d..%-3d %3d..%-3d  %2dx%-2d  %4d     (%5.1f, %5.1f)     (%5.1f, %3d)        (%+6.1f, %6.1f)%s\n", ++k, g.x0, g.x1, g.y0, g.y1,
			g.x1 - g.x0 + 1, g.y1 - g.y0 + 1, g.n, g.cx, g.cy, g.wx, g.y1 + 1, wxw, whw, tag.c_str());
	}
}

// One unlit/lit pair, measured and printed.
static void Measure(const Pic &un, const Pic &lit, const std::vector<Colour> &pal, const std::string &sheetDir, const std::string &how)
{
	printf("\nPAIR %s -> %s   (%s)\n", un.key.c_str(), lit.key.c_str(), how.c_str());
	printf("  unlit %-12s %-11s %3d x %3d texels, %d frame(s), %.0f units/texel, shift %+.0f%s -> %.0f x %.0f units\n", un.key.c_str(), un.kind.c_str(),
		un.W, un.H, un.frames, un.upp, un.shift, un.hang ? " HANGS" : "", un.W * un.upp, un.H * un.upp);
	printf("  lit   %-12s %-11s %3d x %3d texels, %d frame(s), %.0f units/texel, shift %+.0f%s -> %.0f x %.0f units%s\n", lit.key.c_str(), lit.kind.c_str(),
		lit.W, lit.H, lit.frames, lit.upp, lit.shift, lit.hang ? " HANGS" : "", lit.W * lit.upp, lit.H * lit.upp,
		lit.zeroOpaqueBit ? "  (image_type bit 0x02 set; index 0 read as a hole, the object rule)" : "");
	if (!un.ok() || !lit.ok()) { printf("  (a picture did not decode)\n"); return; }
	if (un.upp != lit.upp || un.hang || lit.hang) printf("  NOTE: different scale or a hanging picture: the mapping below assumes the same scale and bottom anchoring\n");
	// unlit texel (ux, uy) -> lit texel (ux + dx, uy + dy)
	const double dxf = lit.W / 2.0 - un.W / 2.0;
	const double dyf = (lit.H - un.H) + (un.shift - lit.shift) / lit.upp;
	const int dx = (int)floor(dxf), dy = (int)lround(dyf);
	printf("  alignment: unlit texel (x, y) lies on lit texel (x %+d, y %+d)%s\n", dx, dy,
		dxf != dx ? "   ** half-texel horizontal misfit: the widths differ by an odd number; floor taken **" : "");
	auto unAt = [&](int x, int y) { return un.At(0, x - dx, y - dy); };
	// Unlit pictures that leave the lit picture's area (would be lost by compositing on the lit picture).
	int outside = 0;
	for (int y = 0; y < un.H; y++) for (int x = 0; x < un.W; x++) if (un.At(0, x, y) >= 0 && (x + dx < 0 || y + dy < 0 || x + dx >= lit.W || y + dy >= lit.H)) outside++;
	printf("  unlit texels that fall outside the lit picture: %d\n", outside);

	std::vector<uint8_t> chg((size_t)lit.W * lit.H, 0), chg0((size_t)lit.W * lit.H, 0), anim((size_t)lit.W * lit.H, 0);
	int nChg = 0, nChg0 = 0, nAnim = 0, appear = 0, vanish = 0, recolour = 0;
	for (int y = 0; y < lit.H; y++)
		for (int x = 0; x < lit.W; x++)
		{
			const int u = unAt(x, y);
			bool any = false;
			for (int f = 0; f < lit.frames; f++)
			{
				const int l = lit.At(f, x, y);
				if (l != u) { any = true; if (f == 0) { chg0[(size_t)y * lit.W + x] = 1; nChg0++; } }
				if (f > 0 && l != lit.At(0, x, y)) anim[(size_t)y * lit.W + x] = 1;
			}
			if (anim[(size_t)y * lit.W + x]) nAnim++;
			if (!any) continue;
			chg[(size_t)y * lit.W + x] = 1; nChg++;
			const int l0 = lit.At(0, x, y);
			if (u < 0 && l0 >= 0) appear++; else if (u >= 0 && l0 < 0) vanish++; else if (u != l0) recolour++;
		}
	printf("  changed texels: %d in any frame (%d in frame 0: %d appear where the unlit picture is clear, %d vanish, %d recoloured); texels animated between lit frames: %d\n",
		nChg, nChg0, appear, vanish, recolour, nAnim);
	// changed texels not animated, and animated texels not changed
	int chgNotAnim = 0, animNotChg = 0;
	for (size_t i = 0; i < chg.size(); i++) { if (chg[i] && !anim[i]) chgNotAnim++; if (anim[i] && !chg[i]) animNotChg++; }
	printf("  changed but never animated: %d   animated but not changed from unlit: %d\n", chgNotAnim, animNotChg);

	// FLAMES are the groups of texels that flicker (differ between the lit frames). Each flame's wick point is the
	// middle of its lowest flickering row, one texel down: where the flame meets the wick.
	std::vector<Group> fl = Flames(anim, lit.W, lit.H, lit, pal);
	PrintFlames(fl, lit);
	// Every changed group that holds no flickering texel is an OTHER change, which per-flame compositing from the
	// lit picture would also have to carry. Each is placed against the flames: "under flame k" when it shares
	// columns with flame k and starts within 12 rows below its wick.
	std::vector<uint8_t> other(chg);
	for (size_t i = 0; i < other.size(); i++) if (anim[i]) other[i] = 0;
	// drop texels 8-connected to a flame (the flame's own steady core): flood from the flame texels through chg
	{
		std::vector<std::pair<int, int>> st;
		for (int y = 0; y < lit.H; y++) for (int x = 0; x < lit.W; x++) if (anim[(size_t)y * lit.W + x]) st.push_back({ x, y });
		std::vector<uint8_t> seen(anim);
		while (!st.empty())
		{
			auto [x, y] = st.back(); st.pop_back();
			for (int dy = -1; dy <= 1; dy++) for (int dx2 = -1; dx2 <= 1; dx2++)
			{
				const int nx = x + dx2, ny = y + dy;
				if (nx < 0 || ny < 0 || nx >= lit.W || ny >= lit.H) continue;
				const size_t i = (size_t)ny * lit.W + nx;
				if (seen[i] || !chg[i]) continue;
				// the flame's core: only upward of (or level with) the flame's own bottom
				bool inFlameRows = false;
				for (const Group &f : fl) if (nx >= f.x0 && nx <= f.x1 && ny <= f.y1) inFlameRows = true;
				if (!inFlameRows) continue;
				seen[i] = 1; other[i] = 0; st.push_back({ nx, ny });
			}
		}
	}
	std::vector<Group> og = Groups(other, lit.W, lit.H, lit, pal);
	int nOther = 0; for (const Group &g : og) nOther += g.n;
	printf("  OTHER changed texels (not part of a flame): %d in %zu group(s)\n", nOther, og.size());
	for (const Group &g : og)
	{
		std::string where = "elsewhere";
		for (size_t k = 0; k < fl.size(); k++)
			if (g.x1 >= fl[k].x0 - 1 && g.x0 <= fl[k].x1 + 1 && g.y0 >= fl[k].y1 && g.y0 <= fl[k].y1 + 13)
			{ char b[64]; snprintf(b, sizeof b, "under flame %zu (+%d rows)", k + 1, g.y0 - fl[k].y1 - 1); where = b; break; }
		const int cnt = std::max(1, g.n);
		printf("    box x %2d..%-2d y %3d..%-3d %4d texels, mean rgb %3d %3d %3d, %s\n", g.x0, g.x1, g.y0, g.y1, g.n, g.r / cnt, g.g / cnt, g.b / cnt, where.c_str());
	}
	if (!sheetDir.empty())
	{
		const int S = 4, PW = lit.W * S, gap = 8, TW = PW * 3 + gap * 2, TH = lit.H * S;
		std::vector<Colour> img((size_t)TW * TH, Colour{ 40, 40, 48 });
		auto put = [&](int X, int Y, Colour c) { if (X >= 0 && Y >= 0 && X < TW && Y < TH) img[(size_t)Y * TW + X] = c; };
		for (int y = 0; y < lit.H; y++)
			for (int x = 0; x < lit.W; x++)
				for (int sy = 0; sy < S; sy++)
					for (int sx = 0; sx < S; sx++)
					{
						const int u = unAt(x, y), l = lit.At(0, x, y);
						if (u >= 0) put(x * S + sx, y * S + sy, pal[u]);
						if (l >= 0) put(PW + gap + x * S + sx, y * S + sy, pal[l]);
						const size_t i = (size_t)y * lit.W + x;
						put(2 * (PW + gap) + x * S + sx, y * S + sy, chg[i] ? (anim[i] ? Colour{ 255, 200, 60 } : Colour{ 255, 255, 255 }) : (l >= 0 ? Colour{ 70, 70, 80 } : Colour{ 40, 40, 48 }));
					}
		auto box = [&](int ox, const Group &g, Colour c)
		{
			for (int X = g.x0 * S; X <= (g.x1 + 1) * S - 1; X++) { put(ox + X, g.y0 * S, c); put(ox + X, (g.y1 + 1) * S - 1, c); }
			for (int Y = g.y0 * S; Y <= (g.y1 + 1) * S - 1; Y++) { put(ox + g.x0 * S, Y, c); put(ox + (g.x1 + 1) * S - 1, Y, c); }
		};
		for (int panel = 0; panel < 3; panel++)
		{
			const int ox = panel * (PW + gap);
			for (const Group &g : og) box(ox, g, { 230, 0, 230 });
			for (const Group &g : fl)
			{
				box(ox, g, { 0, 220, 0 });
				const int cx = (int)(g.cx * S), cy = (int)(g.cy * S), wx = (int)(g.wx * S), wy = (g.y1 + 1) * S;
				for (int d = -2; d <= 2; d++) { put(ox + cx + d, cy, { 60, 120, 255 }); put(ox + cx, cy + d, { 60, 120, 255 }); }
				for (int d = -4; d <= 4; d++) { put(ox + wx + d, wy, { 255, 0, 0 }); put(ox + wx, wy + d, { 255, 0, 0 }); }
			}
		}
		std::string a = un.key, b = lit.key;
		for (std::string *s : { &a, &b }) for (char &c : *s) if (c == '[' || c == ']') c = '_';
		PutPPM(sheetDir + "/" + a + "_" + b + ".ppm", img, TW, TH);
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: wickspots <ROTH DATA> [-sheet DIR]\n"); return 2; }
	if (!TheInstall().Open(argv[1])) { printf("install: %s\n", TheInstall().Error().c_str()); return 2; }
	std::string sheetDir;
	for (int a = 2; a + 1 < argc; a++) if (!strcmp(argv[a], "-sheet")) sheetDir = argv[a + 1];
	const std::vector<Colour> &pal = GetPack("DEMO")->pack.Palette();

	// The candle kinds of docs/WORLD_DESIGN.md 2.1 (looked at on contact sheets there).
	static const char *unlitKinds[] = { "DEMO[4098]", "DEMO[4186]", "DEMO[4124]", "DEMO1[4185]" };
	static const char *litOnly[] = { "DEMO[4148]", "DEMO1[4103]", "DEMO2[4099]", "DEMO1[4104]", "DEMO4[4215]", "DEMO4[4258]", "DEMO[4135]" };
	static const char *unlitDeco[] = { "DEMO[4117]", "DEMO1[4131]", "DEMO4[4232]", "DEMO[4189]" };

	// 1. The pairs, from the maps' click chains.
	std::map<std::pair<std::string, std::string>, std::set<std::string>> pairs;   // (unlit, lit) -> "MAP #rec"
	std::map<std::string, int> placedFixed, placed;
	for (const std::string &name : TheInstall().MapNames())
	{
		std::vector<uint8_t> raw = ReadFile(TheInstall().MapFile(name.c_str()));
		if (raw.empty()) continue;
		Map m = ParseRaw(raw.data(), raw.size());
		if (!m.ok()) continue;
		const std::string pack = TheInstall().PackFor(name.c_str());
		auto art = [&](uint16_t w) { int idx = 0; const bool sh = Pack::ResolveObjectArt((uint8_t)(w & 0xff), (uint8_t)(w >> 8), idx); return KeyOf(sh ? TheInstall().SharedPack() : pack, idx); };
		std::map<int, std::set<std::string>> artsById;
		for (const auto &grp : m.objects)
			for (const Object &o : grp)
			{
				const std::string a = art((uint16_t)(o.textureIndex | (o.textureSource << 8)));
				placed[a]++;
				if (o.renderType & 0x80) placedFixed[a]++;
				if (o.commandID) artsById[o.commandID].insert(a);
			}
		for (size_t ci = 0; ci < m.commands.size(); ci++)
		{
			const Command &t = m.commands[ci];
			if (t.opcode != 0x08 || !t.isTrigger) continue;
			std::set<int> seen;
			std::vector<int> todo = { t.chainStart };
			while (!todo.empty())
			{
				int i = todo.back(); todo.pop_back();
				for (int steps = 0; i > 0 && i <= (int)m.commands.size() && steps < 128 && seen.insert(i).second; steps++)
				{
					const Command &c = m.commands[i - 1];
					if (c.opcode == 0x40) todo.push_back(c.Word(6));
					if (c.opcode == 0x0d && c.key == 0)
						for (const std::string &u : artsById[t.key])
							pairs[{ u, art(c.Word(0x0c)) }].insert(name + " #" + std::to_string(ci + 1) + "/#" + std::to_string(i));
					if (c.isTrigger) break;
					i = c.nextIndex;
				}
			}
		}
	}
	printf("WICKSPOTS: candle pictures, unlit against lit\n");
	printf("Pairs found in the maps' click chains (0x0d with key 0 on the clicked object):\n");
	for (const auto &kv : pairs)
	{
		bool candle = false;
		for (const char *k : unlitKinds) if (kv.first.first == k) candle = true;
		if (!candle) continue;
		std::string w; for (const std::string &s : kv.second) w += " " + s;
		printf("  %-12s -> %-12s by%s\n", kv.first.first.c_str(), kv.first.second.c_str(), w.c_str());
	}
	printf("Placements drawn fixed-angle (renderType & 0x80) per kind; 0 = every placement is a billboard:");
	for (const char *k : unlitKinds) printf(" %s %d/%d", k, placedFixed[k], placed[k]);
	for (const char *k : litOnly) printf(" %s %d/%d", k, placedFixed[k], placed[k]);
	for (const char *k : unlitDeco) printf(" %s %d/%d", k, placedFixed[k], placed[k]);
	printf("\n");

	auto parse = [](const std::string &key, std::string &pk, int &idx) { const size_t lb = key.find('['); pk = key.substr(0, lb); idx = atoi(key.c_str() + lb + 1); };
	for (const auto &kv : pairs)
	{
		bool candle = false;
		for (const char *k : unlitKinds) if (kv.first.first == k) candle = true;
		if (!candle) continue;
		std::string pu, pl; int iu, il; parse(kv.first.first, pu, iu); parse(kv.first.second, pl, il);
		Measure(Load(pu, iu), Load(pl, il), pal, sheetDir, "scripted swap");
	}

	// 2. The kinds with no scripted partner: their own animation, and the closest same-pack picture.
	printf("\nKINDS WITH NO SCRIPTED PARTNER: what animates in the lit ones, and the nearest same-size picture in the same pack\n");
	auto nearest = [&](const Pic &p, const std::string &pk, int self, bool wantStill) -> std::pair<int, double>
	{
		LoadedPack *lp = GetPack(pk);
		int best = -1; double bestF = 2;
		for (int i = 0; i < lp->pack.Count(); i++)
		{
			if (i == self) continue;
			const FatEntry *e = lp->pack.Entry(i);
			if (!e || (e->kind != EntryKind::Plain && e->kind != EntryKind::Animated)) continue;
			if (wantStill != (e->kind == EntryKind::Plain)) continue;
			Image im = lp->pack.ReadImage(i, false);
			if (!im.ok() || im.height != p.W) continue;
			Pic q = Load(pk, i);
			if (!q.ok()) continue;
			// bottoms aligned, centres aligned; fraction of the union of opaque texels that differ
			const int dx = (int)floor(p.W / 2.0 - q.W / 2.0), dy = (int)lround((p.H - q.H) + (q.shift - p.shift) / p.upp);
			int uni = 0, diff = 0;
			for (int y = 0; y < p.H; y++)
				for (int x = 0; x < p.W; x++)
				{
					const int a = p.At(0, x, y), b = q.At(0, x - dx, y - dy);
					if (a < 0 && b < 0) continue;
					uni++; if (a != b) diff++;
				}
			const double fr = uni ? double(diff) / uni : 1;
			if (fr < bestF) { bestF = fr; best = i; }
		}
		return { best, bestF };
	};
	for (const char *k : litOnly)
	{
		std::string pk; int idx; parse(k, pk, idx);
		Pic p = Load(pk, idx);
		std::vector<uint8_t> anim((size_t)p.W * p.H, 0);
		int na = 0;
		for (int y = 0; y < p.H; y++) for (int x = 0; x < p.W; x++) for (int f = 1; f < p.frames; f++) if (p.At(f, x, y) != p.At(0, x, y)) { if (!anim[(size_t)y * p.W + x]) na++; anim[(size_t)y * p.W + x] = 1; }
		std::vector<Group> ga = Flames(anim, p.W, p.H, p, pal);
		auto nb = nearest(p, pk, idx, true);
		printf("\n  %-12s %-7s %3d x %3d texels, %2d frames, %.0f units/texel, shift %+.0f -> %.0f x %.0f units; flickering texels %d\n", k, p.kind.c_str(),
			p.W, p.H, p.frames, p.upp, p.shift, p.W * p.upp, p.H * p.upp, na);
		PrintFlames(ga, p);
		if (nb.first >= 0) printf("  nearest still picture of the same width in %s: [%d], %.0f%% of opaque texels differ (no unlit partner)\n", pk.c_str(), nb.first, nb.second * 100);
		if (!sheetDir.empty())
		{
			// lit frame 0, 4x, flames boxed (green), centroid (blue), wick point (red)
			const int S = 4, TW = p.W * S, TH = p.H * S;
			std::vector<Colour> img((size_t)TW * TH, Colour{ 40, 40, 48 });
			auto put = [&](int X, int Y, Colour c) { if (X >= 0 && Y >= 0 && X < TW && Y < TH) img[(size_t)Y * TW + X] = c; };
			for (int y = 0; y < p.H; y++) for (int x = 0; x < p.W; x++) { const int l = p.At(0, x, y); if (l >= 0) for (int sy = 0; sy < S; sy++) for (int sx = 0; sx < S; sx++) put(x * S + sx, y * S + sy, pal[l]); }
			for (const Group &g : ga)
			{
				for (int X = g.x0 * S; X <= (g.x1 + 1) * S - 1; X++) { put(X, g.y0 * S, { 0, 220, 0 }); put(X, (g.y1 + 1) * S - 1, { 0, 220, 0 }); }
				for (int Y = g.y0 * S; Y <= (g.y1 + 1) * S - 1; Y++) { put(g.x0 * S, Y, { 0, 220, 0 }); put((g.x1 + 1) * S - 1, Y, { 0, 220, 0 }); }
				const int cx = (int)(g.cx * S), cy = (int)(g.cy * S), wx = (int)(g.wx * S), wy = (g.y1 + 1) * S;
				for (int d = -2; d <= 2; d++) { put(cx + d, cy, { 60, 120, 255 }); put(cx, cy + d, { 60, 120, 255 }); }
				for (int d = -4; d <= 4; d++) { put(wx + d, wy, { 255, 0, 0 }); put(wx, wy + d, { 255, 0, 0 }); }
			}
			std::string a = k; for (char &c : a) if (c == '[' || c == ']') c = '_';
			PutPPM(sheetDir + "/litonly_" + a + ".ppm", img, TW, TH);
		}
	}
	for (const char *k : unlitDeco)
	{
		std::string pk; int idx; parse(k, pk, idx);
		Pic p = Load(pk, idx);
		auto nb = nearest(p, pk, idx, false);
		printf("  %-12s %-7s %3d x %3d texels, %2d frame(s), %.0f u/t, shift %+.0f", k, p.kind.c_str(), p.W, p.H, p.frames, p.upp, p.shift);
		if (nb.first >= 0) printf("; nearest ANIMATED picture of the same width in %s: [%d], %.0f%% of opaque texels differ", pk.c_str(), nb.first, nb.second * 100);
		printf("\n");
	}
	return 0;
}
