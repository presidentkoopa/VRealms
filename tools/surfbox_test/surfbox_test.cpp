// surfbox_test.cpp -- the port side of tools/surfbox_test: the RAW records' surface box (src/roth/roth_surfbox.h)
// against the ORIGINAL (boxprobe.c in the headless ROTH.C build). Engine-free: g++ and the src/roth readers.
//
//   surfbox_test <DATA> census                          every record of the four boxed channels, all maps
//   surfbox_test <DATA> MAP match-script out [mut]      the match cases of MAP: M lines for boxprobe.c
//   surfbox_test <DATA> MAP match-check log [mut]       those cases through roth_surfbox.h, against the original's MT lines
//   surfbox_test <DATA> MAP texel-script out            poses in front of every boxed face and floor of MAP: P lines
//   surfbox_test <DATA> MAP texel-check log [mut]       every wall / flat pick of the original's PK lines: its +4/+6
//                                                       against WallPickTexel / FlatPickTexel at the pick's hit point
//
// `mut` (a number) turns on one deliberate fault in the port side, to show that the test catches it:
//   1  the box's upper x edge exclusive (x <= x1 becomes x < x1)
//   2  "no box" read from +0x0c alone (ROTH_COMMANDS.md's summary) instead of the dword at +0x0c
//   3  out of the box stops the walk instead of trying the next record
//   4  the wall's +4 not mirrored under FLIP_X
//   5  the flat's +4 / +6 swapped
//
// THE MATCH CASES (checklist 1-8 of roth_surfbox.h). For every map, every target that carries a record with a box
// on one of the four boxed channels -- a face (the faces whose texture map's faceID is the record's key), a
// sector (every sector whose command id is the key: the original matches the id, not one sector) -- and every pick
// type and band the channel reads, the texel points: each box's corners, edges, one in and one out of each edge,
// its middle, and three far points (0,0), (-32768,-32768), (32767,32767). The original answers with
// dispatch_entry_command_trigger and _b on a pick record built in the probe; this side with SurfFirstMatch over
// the same records, in the original's directory order, and roth_commands' WalkChainFlow for the flow pre-pass.
//
// THE TEXEL CASES (checklist 9-13). The original's own cursor pick (render_world_view) over a screen grid from
// poses square-on to each boxed face and above each boxed floor; for every pick on a face or a flat the hit point
// is rebuilt with the original's camera (docs/REMAROTH_MEASURED.md 1: FX 309.77, FY 355.06, X0 320.48,
// Y0 240.54, eye = z + 143.94) and the port's texel computed there. The camera is a fit, so a hit closer than
// EDGE_EPS of a texel's edge (in the texel coordinate) is not a test of the rule: those are counted apart, as
// "at an edge", and every other pick must agree exactly.

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"
#include "roth_surface.h"
#include "roth_surfbox.h"
#include "roth_commands.h"

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

static int g_mut = 0;
static bool g_allEnabled = false;   // the second pass of the match cases: every boxed-channel record's +2 bit 3 cleared

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return b;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	b.resize((size_t)n);
	if (n > 0 && fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
	fclose(f);
	return b;
}
static uint16_t U16(const std::vector<uint8_t> &r, size_t o) { return o + 1 < r.size() ? (uint16_t)(r[o] | (r[o + 1] << 8)) : 0; }

static const char *kMaps[] = { "ABAGATE2","AELF","ANUBIS","AQUA1","AQUA2","CAVERNS","CAVERNS2","CAVERNS3","CHURCH1","DOMINION",
	"DOPPLE","ELOHIM1","GNARL1","GRAVE","LRINTH","LRINTH1","MAS3","MAS4","MAS6","MAS7","MAUSO1EA","MAUSO1EB","MAZE","OPTEMP1",
	"RAQUIA1","RAQUIA2","RAQUIA3","RAQUIA4","RAQUIA5","SALVAT","SOULST2","SOULST3","STUDY1","STUDY2","STUDY3","STUDY4","TEMPLE1",
	"TGATE1F","TGATE1G","TGATE1H","TGATE1I","TOWER1","VICAR","VICAR1" };

// The command header's directories (tools/world_census): category 4 use wall (0x18, hdr+0x18), 5 use floor (0x19,
// +0x1c), 10 examine wall (0x32, +0x30), 11 examine floor (0x31, +0x34).
enum { CAT_USE_WALL = 4, CAT_USE_FLOOR = 5, CAT_EXAM_WALL = 10, CAT_EXAM_FLOOR = 11 };

struct MapData
{
	std::string name;
	std::vector<uint8_t> raw;
	Map m;
	size_t cmdBase = 0;                        // the command section's header in the file (= the object table header)
	std::map<uint32_t, int> recByRef;          // record offset in the table -> 0-based command index
	std::vector<uint32_t> refOf;               // 0-based command index -> record offset in the table
	std::vector<int> cat[15];                  // directory -> 0-based command indices, in the original's order
	std::vector<uint32_t> faceOff;             // face index -> geometry offset
	std::vector<uint8_t> packBytes;
	Pack pack;
	uint16_t key = 0;
	std::map<int, Image> images;
	const Image &Img(int idx)
	{
		auto it = images.find(idx);
		if (it != images.end()) return it->second;
		return images[idx] = pack.ReadImage(idx, false);
	}
	uint32_t SectorOff(int s) const { return 30u + 26u * (uint32_t)s; }
};

static bool LoadMap(const std::string &name, MapData &M)
{
	M.name = name;
	M.raw = ReadFile(TheInstall().MapFile(name.c_str()));
	if (M.raw.empty()) return false;
	M.m = ParseRaw(M.raw.data(), M.raw.size());
	if (!M.m.ok()) return false;
	ApplyLoadTimeHeights(M.m);
	M.packBytes = ReadFile(TheInstall().PackFile(TheInstall().PackFor(name.c_str()).c_str()));
	if (M.packBytes.empty() || !M.pack.Load(M.packBytes.data(), M.packBytes.size())) return false;
	M.key = M.pack.SkyMarkerIndex();
	const size_t base = (size_t)M.m.header.verticesOffset + M.m.header.verticesSectionSize;
	M.cmdBase = base;
	const uint16_t count = U16(M.raw, base + 6);
	size_t at = base + 8 + 60 + 2 * (size_t)count;
	M.refOf.assign(M.m.commands.size(), 0);
	for (int i = 0; i < count; i++)
	{
		M.recByRef[(uint32_t)(at - base)] = i;
		if (i < (int)M.refOf.size()) M.refOf[i] = (uint32_t)(at - base);
		const uint16_t size = U16(M.raw, at);
		if (size < 6) break;
		at += size;
	}
	for (int c = 0; c < 15; c++)
	{
		const uint16_t off = U16(M.raw, base + 8 + 4 * c), n = U16(M.raw, base + 10 + 4 * c);
		for (int k = 0; k < n; k++)
		{
			auto it = M.recByRef.find(U16(M.raw, base + off + 2 * k));
			if (it == M.recByRef.end() || it->second >= (int)M.m.commands.size()) continue;
			M.cat[c].push_back(it->second);
		}
	}
	M.faceOff.resize(M.m.faces.size());
	for (auto &kv : M.m.faceByOffset) if (kv.second >= 0 && kv.second < (int)M.faceOff.size()) M.faceOff[kv.second] = kv.first;
	return true;
}

//---------------------------------------------------------------------------
// The port's walk, with the mutations
//---------------------------------------------------------------------------
static bool BoxAdmitsMut(const SurfBox &b, const SurfTexel &t)
{
	if (g_mut == 1)
	{
		if (!SurfBoxSet(b)) return true;
		return (int16_t)b.x0 <= t.x && t.x < (int16_t)b.x1 && (int16_t)b.y0 <= t.y && t.y <= (int16_t)b.y1;
	}
	if (g_mut == 2)
	{
		if (b.x0 == 0) return true;
		return (int16_t)b.x0 <= t.x && t.x <= (int16_t)b.x1 && (int16_t)b.y0 <= t.y && t.y <= (int16_t)b.y1;
	}
	return SurfBoxAdmits(b, t);
}
static int FirstMatchMut(const std::vector<SurfCandidate> &list, SurfChannel ch, uint8_t type, uint8_t band, const SurfTexel &t)
{
	if (g_mut != 1 && g_mut != 2 && g_mut != 3) return SurfFirstMatch(list, ch, type, band, t);
	for (size_t i = 0; i < list.size(); i++)
	{
		const SurfCandidate &c = list[i];
		if (c.modifier & 0x08) continue;
		if (!SurfFlagsAdmit(ch, c.fireFlags, type, band)) continue;
		if (!BoxAdmitsMut(c.box, t)) { if (g_mut == 3) return -1; continue; }
		return (int)i;
	}
	return -1;
}

// The records of one directory that name this target, in the directory's order: a face's by its texture map's
// faceID (word[texmap + 0xc], RC:3019-3026), a sector's by its command id (word[cell + 0x14], RC:3062-3065).
static std::vector<int> TargetRecords(const MapData &M, int cat, int face, int sector)
{
	std::vector<int> out;
	uint16_t key = 0;
	if (face >= 0)
	{
		const int tmi = M.m.faces[face].textureMap;
		if (tmi < 0 || !M.m.textureMaps[tmi].extended) return out;
		key = M.m.textureMaps[tmi].faceID;
	}
	else key = M.m.sectors[sector].commandID;
	for (int ci : M.cat[cat]) if (M.m.commands[ci].key == key) out.push_back(ci);
	return out;
}

static SurfCandidate Cand(const MapData &M, int ci)
{
	const Command &c = M.m.commands[ci];
	SurfCandidate s;
	s.record = (uint16_t)(ci + 1);
	s.modifier = g_allEnabled ? (uint8_t)(c.modifier & ~0x08) : c.modifier;
	s.fireFlags = c.fireFlags;
	s.box = RecordSurfBox(c);
	return s;
}

// The marker bytes the original's dispatch tests before it walks (set at load by mark_geometry_records_by_id: the
// face's texture map +9 bit 0 for 0x18, bit 2 for 0x32; the sector's +0x16 bit 4 for 0x19, bit 5 for 0x31). The
// port keeps no marker: a target is marked when a record of that channel names it (roth_runtime.cpp binds
// them; HasEvent). That equivalence is part of what this test checks.
static uint8_t TexmapMarker(const MapData &M, int face)
{
	return (uint8_t)((TargetRecords(M, CAT_USE_WALL, face, -1).empty() ? 0 : 1) | (TargetRecords(M, CAT_EXAM_WALL, face, -1).empty() ? 0 : 4));
}
static uint8_t SectorMarker(const MapData &M, int s)
{
	return (uint8_t)((TargetRecords(M, CAT_USE_FLOOR, -1, s).empty() ? 0 : 0x10) | (TargetRecords(M, CAT_EXAM_FLOOR, -1, s).empty() ? 0 : 0x20));
}

// What the port answers for one case: the record offset in the table of the use match and of the examine match.
struct Answer { uint32_t use = 0, exa = 0; };
static Answer PortMatch(const MapData &M, uint8_t type, int face, int sector, uint8_t bandIdx, const SurfTexel &t)
{
	Answer a;
	auto pick = [&](int cat, SurfChannel ch, uint8_t band) -> uint32_t
	{
		std::vector<int> recs = TargetRecords(M, cat, face, sector);
		std::vector<SurfCandidate> list;
		for (int ci : recs) list.push_back(Cand(M, ci));
		const int at = FirstMatchMut(list, ch, type, band, t);
		if (at < 0) return 0;
		const Command &c = M.m.commands[recs[at]];
		if (WalkChainFlow(M.m, c.chainStart) != 0) return 0;      // the flow pre-pass gates: no match, nothing later tried
		return M.refOf[recs[at]];
	};
	const uint8_t band = SurfBandBit(bandIdx);
	if (type == 3)
	{
		if (TexmapMarker(M, face) & 1) a.use = pick(CAT_USE_WALL, SurfChannel::UseFace, band);
		if (TexmapMarker(M, face) & 4) { a.exa = pick(CAT_EXAM_WALL, SurfChannel::ExamineFace, band); return a; }
	}
	if ((type == 2 || type == 8) && (SectorMarker(M, sector) & 0x10)) a.use = pick(CAT_USE_FLOOR, SurfChannel::UseFlat, 0);
	if (type != 7 && type != 4 && (SectorMarker(M, sector) & 0x20))
	{
		const int s = sector;
		if (s >= 0) { int f = face; face = -1; a.exa = pick(CAT_EXAM_FLOOR, SurfChannel::ExamineFlat, 0); face = f; }
	}
	return a;
}

//---------------------------------------------------------------------------
// census
//---------------------------------------------------------------------------
static int Census(const char *data)
{
	(void)data;
	int tot = 0, boxed = 0, xOnly = 0, x1Only = 0, dirs = 0, dirsOutOfOrder = 0;
	for (const char *mn : kMaps)
	{
		MapData M;
		if (!LoadMap(mn, M)) { printf("%s: not loaded\n", mn); continue; }
		const int cats[4] = { CAT_USE_WALL, CAT_USE_FLOOR, CAT_EXAM_WALL, CAT_EXAM_FLOOR };
		const char *cn[4] = { "use-wall", "use-floor", "exam-wall", "exam-floor" };
		// The engine walks its bindings in RECORD order (roth_runtime.cpp binds the command table in order); the
		// original walks the directory. They are the same walk only if each directory lists its records in order.
		for (int k = 0; k < 4; k++)
		{
			dirs++;
			if (!std::is_sorted(M.cat[cats[k]].begin(), M.cat[cats[k]].end())) { dirsOutOfOrder++; printf("%s %s: the directory is NOT in record order\n", mn, cn[k]); }
		}
		for (int k = 0; k < 4; k++)
			for (int ci : M.cat[cats[k]])
			{
				const Command &c = M.m.commands[ci];
				const SurfBox b = RecordSurfBox(c);
				tot++;
				if (!SurfBoxSet(b)) continue;
				boxed++;
				if (b.x0 == 0) xOnly++;
				if (b.x1 == 0) x1Only++;
				printf("%s %s #%d op=%02x mod=%02x f6=%02x key=%u box x %d..%d y %d..%d\n", mn, cn[k], ci + 1, c.opcode, c.modifier,
					c.fireFlags, c.key, (int16_t)b.x0, (int16_t)b.x1, (int16_t)b.y0, (int16_t)b.y1);
			}
	}
	printf("records %d, boxed %d (x0 == 0 in %d of them, x1 == 0 in %d); directories %d, %d not in record order\n", tot, boxed, xOnly, x1Only,
		dirs, dirsOutOfOrder);
	return 0;
}

//---------------------------------------------------------------------------
// match cases
//---------------------------------------------------------------------------
struct Case { uint8_t type; int face, sector; uint8_t band; SurfTexel t; };

static std::vector<Case> MatchCases(const MapData &M)
{
	std::vector<Case> out;
	std::set<int> faces, sectors;
	auto boxedIn = [&](int cat, std::set<int> &into, bool isFace)
	{
		for (int ci : M.cat[cat])
		{
			const Command &c = M.m.commands[ci];
			if (!SurfBoxSet(RecordSurfBox(c))) continue;
			if (isFace)
			{
				for (size_t f = 0; f < M.m.faces.size(); f++)
				{
					const int tmi = M.m.faces[f].textureMap;
					if (tmi >= 0 && M.m.textureMaps[tmi].extended && M.m.textureMaps[tmi].faceID == c.key) into.insert((int)f);
				}
			}
			else
				for (size_t s = 0; s < M.m.sectors.size(); s++) if (M.m.sectors[s].commandID == c.key) into.insert((int)s);
		}
	};
	boxedIn(CAT_USE_WALL, faces, true); boxedIn(CAT_EXAM_WALL, faces, true);
	boxedIn(CAT_USE_FLOOR, sectors, false); boxedIn(CAT_EXAM_FLOOR, sectors, false);

	auto points = [&](const std::vector<int> &recs, std::vector<SurfTexel> &pts)
	{
		std::set<std::pair<int, int>> seen;
		auto add = [&](int x, int y)
		{
			x = std::max(-32768, std::min(32767, x)); y = std::max(-32768, std::min(32767, y));
			if (!seen.insert({ x, y }).second) return;
			SurfTexel t; t.have = true; t.x = (int16_t)x; t.y = (int16_t)y; pts.push_back(t);
		};
		add(0, 0); add(-32768, -32768); add(32767, 32767);
		for (int ci : recs)
		{
			const SurfBox b = RecordSurfBox(M.m.commands[ci]);
			if (!SurfBoxSet(b)) continue;
			const int x0 = (int16_t)b.x0, x1 = (int16_t)b.x1, y0 = (int16_t)b.y0, y1 = (int16_t)b.y1;
			const int xs[7] = { x0 - 1, x0, x0 + 1, (x0 + x1) / 2, x1 - 1, x1, x1 + 1 };
			const int ys[7] = { y0 - 1, y0, y0 + 1, (y0 + y1) / 2, y1 - 1, y1, y1 + 1 };
			for (int x : xs) for (int y : ys) add(x, y);
		}
	};
	for (int f : faces)
	{
		std::vector<int> recs = TargetRecords(M, CAT_USE_WALL, f, -1), r2 = TargetRecords(M, CAT_EXAM_WALL, f, -1);
		recs.insert(recs.end(), r2.begin(), r2.end());
		std::vector<SurfTexel> pts; points(recs, pts);
		for (uint8_t band = 0; band < 4; band++)
			for (const SurfTexel &t : pts) out.push_back(Case{ 3, f, M.m.faces[f].sector, band, t });
	}
	for (int s : sectors)
	{
		std::vector<int> recs = TargetRecords(M, CAT_USE_FLOOR, -1, s), r2 = TargetRecords(M, CAT_EXAM_FLOOR, -1, s);
		recs.insert(recs.end(), r2.begin(), r2.end());
		std::vector<SurfTexel> pts; points(recs, pts);
		const uint8_t types[4] = { 2, 8, 1, 7 };
		for (uint8_t ty : types)
			for (const SurfTexel &t : pts) out.push_back(Case{ ty, -1, s, 0, t });
	}
	return out;
}

static int MatchScript(MapData &M, const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) return 2;
	const std::vector<Case> cs = MatchCases(M);
	// Twice: as loaded, then with every record of the four directories enabled (X), which reaches the records a
	// chain enables later (0x17) and the walk's fall-through past them.
	for (int pass = 0; pass < 2; pass++)
	{
		if (pass == 1) fprintf(f, "X\n");
		for (size_t i = 0; i < cs.size(); i++)
		{
			const Case &c = cs[i];
			fprintf(f, "M %u %u %u %u %d %d %c%zu\n", c.type, c.face >= 0 ? M.faceOff[c.face] : 0u, c.sector >= 0 ? M.SectorOff(c.sector) : 0u,
				c.band, c.t.x, c.t.y, pass ? 'e' : 'c', i);
		}
	}
	fprintf(f, "Q\n");
	fclose(f);
	printf("%s: %zu match cases -> %s\n", M.name.c_str(), cs.size(), path);
	return 0;
}

static int MatchCheck(MapData &M, const char *log)
{
	const std::vector<Case> cs = MatchCases(M);
	FILE *f = fopen(log, "r");
	if (!f) { printf("no log %s\n", log); return 2; }
	char ln[256];
	size_t n = 0, same = 0, differ = 0, inBox = 0, outBox = 0, anyMatch = 0;
	std::set<uint32_t> recsSeen;
	while (fgets(ln, sizeof ln, f))
	{
		char lab[64]; unsigned u = 0, e = 0;
		if (sscanf(ln, "MT %63s %u %u", lab, &u, &e) != 3 || (lab[0] != 'c' && lab[0] != 'e')) continue;
		const size_t i = (size_t)atol(lab + 1);
		if (i >= cs.size()) continue;
		const Case &c = cs[i];
		g_allEnabled = lab[0] == 'e';
		const Answer a = PortMatch(M, c.type, c.face, c.sector, c.band, c.t);
		g_allEnabled = false;
		n++;
		if (a.use || a.exa) anyMatch++;
		if (a.use) recsSeen.insert(a.use);
		if (a.exa) recsSeen.insert(a.exa);
		if (a.use == u && a.exa == e) { same++; continue; }
		if (differ++ < 25)
			printf("  DIFFER %s: type %u face %d sector %d band %u texel (%d,%d): original use %u exa %u, port use %u exa %u\n", lab, c.type,
				c.face, c.sector, c.band, c.t.x, c.t.y, u, e, a.use, a.exa);
	}
	fclose(f);
	(void)inBox; (void)outBox;
	// Which boxed records were reached at all, and why the others were not (read off the same data).
	int boxed = 0, reached = 0, noTarget = 0, spent = 0, gated = 0, other = 0;
	const int cats[4] = { CAT_USE_WALL, CAT_USE_FLOOR, CAT_EXAM_WALL, CAT_EXAM_FLOOR };
	for (int cat : cats)
		for (int ci : M.cat[cat])
		{
			const Command &c = M.m.commands[ci];
			if (!SurfBoxSet(RecordSurfBox(c))) continue;
			boxed++;
			if (recsSeen.count(M.refOf[ci])) { reached++; continue; }
			bool target = false;
			const bool face = cat == CAT_USE_WALL || cat == CAT_EXAM_WALL;
			if (face) { for (const Case &k : cs) if (k.face >= 0) { const std::vector<int> r = TargetRecords(M, cat, k.face, -1); if (std::count(r.begin(), r.end(), ci)) { target = true; break; } } }
			else { for (const Case &k : cs) if (k.face < 0) { const std::vector<int> r = TargetRecords(M, cat, -1, k.sector); if (std::count(r.begin(), r.end(), ci)) { target = true; break; } } }
			if (!target) noTarget++;
			else if (c.modifier & 0x08) spent++;
			else if (WalkChainFlow(M.m, c.chainStart) != 0) gated++;
			else { other++; printf("  boxed record #%d (op %02x key %u) never matched\n", ci + 1, c.opcode, c.key); }
		}
	printf("%s: boxed records %d: %d matched in some case; not matched: %d name no geometry, %d disabled at load, %d gated by their flow, %d other (shadowed)\n",
		M.name.c_str(), boxed, reached, noTarget, spent, gated, other);
	printf("%s: match cases (as loaded, then all enabled) %zu of %zu run, %zu identical, %zu differ; %zu cases matched a record (%zu distinct records)\n", M.name.c_str(),
		n, 2 * cs.size(), same, differ, anyMatch, recsSeen.size());
	return (differ == 0 && n == 2 * cs.size()) ? 0 : 1;
}

//---------------------------------------------------------------------------
// texel cases
//---------------------------------------------------------------------------
static double FX = 309.77, FY = 355.06, X0 = 320.48, Y0 = 240.54, EYE = 143.94, PITCH_PX = 2.777;

// The face's wall rule, as rothmap.cpp builds it (the loader's own fields; tools/wallrule checks the loader).
static bool FaceRule(MapData &M, int fi, WallFace &wf, int dimU[3], int dimV[3], int tex[3])
{
	const Map &m = M.m;
	const Face &f = m.faces[fi];
	if (f.sector < 0 || f.vertex1 < 0 || f.vertex2 < 0 || f.textureMap < 0) return false;
	const TextureMap &tm = m.textureMaps[f.textureMap];
	const Vertex &a = m.vertices[f.vertex1], &b = m.vertices[f.vertex2];
	wf = WallFace{};
	wf.flags = tm.flags;
	wf.extended = tm.extended;
	wf.extent = (int)tm.StoredExtent();
	wf.shiftX = tm.extended ? tm.shiftX : 0;
	wf.shiftY = tm.extended ? tm.shiftY : 0;
	wf.length = hypot((double)b.x - a.x, (double)b.y - a.y);
	const int ns = (f.sister >= 0 && f.sister < (int)m.faces.size()) ? m.faces[f.sister].sector : -1;
	wf.twoSided = ns >= 0;
	wf.ownFloor = m.sectors[f.sector].floorHeight; wf.ownCeil = m.sectors[f.sector].ceilingHeight;
	wf.ownOverride = m.sectors[f.sector].textureMapOverride;
	auto platOf = [&](int si, int &under, int &top) -> bool
	{
		under = top = 0;
		if (si < 0) return false;
		const int pi = m.sectors[si].platformIndex;
		if (pi < 0 || pi >= (int)m.platforms.size()) return false;
		under = m.platforms[pi].undersideZ; top = m.platforms[pi].topZ;
		return true;
	};
	if (wf.twoSided)
	{
		wf.nbrFloor = m.sectors[ns].floorHeight; wf.nbrCeil = m.sectors[ns].ceilingHeight;
		int nu, nt, ou, ot;
		const bool nh = platOf(ns, nu, nt), oh = platOf(f.sector, ou, ot);
		wf.overlayFollows = WallOverlayFollows(tm.flags, tm.midTexture == M.key, wf.ownFloor, wf.ownCeil, nh, nu, nt, oh, ou, ot);
	}
	const int slot[3] = { tm.upperTexture, tm.midTexture, tm.lowerTexture };
	for (int k = 0; k < 3; k++)
	{
		tex[k] = slot[k]; dimU[k] = dimV[k] = 0;
		if ((int16_t)slot[k] < 0 || slot[k] == M.key) continue;
		const Image &im = M.Img(slot[k]);
		if (!im.ok()) continue;
		dimU[k] = im.height; dimV[k] = im.width;     // RE:13288-13289: along the wall = block +0xe, down it = +0xc
	}
	return true;
}

// The flat the pick names, as rothmap.cpp builds it: a sector's floor (type 2) or ceiling (1) with the sector's
// scale bits, shifts and flip bits; a mid-platform's top (8) or underside (7) with the platform's, no mirrors.
static bool FlatRule(MapData &M, int si, uint8_t type, FlatSetup &fs, double &planeZ)
{
	const Sector &rs = M.m.sectors[si];
	fs = FlatSetup{};
	if (type == 1 || type == 2)
	{
		const bool floor_ = type == 2;
		fs.textureWord = floor_ ? rs.floorTexture : rs.ceilingTexture;
		fs.scaleBits = (uint8_t)(floor_ ? rs.FloorScaleShift() : rs.CeilingScaleShift());
		fs.shiftX = floor_ ? rs.floorShiftX : rs.ceilShiftX;
		fs.shiftY = floor_ ? rs.floorShiftY : rs.ceilShiftY;
		const uint8_t flip = (uint8_t)(rs.flags2 >> 8);
		fs.mirrorX = floor_ ? (flip & 1) != 0 : (flip & 4) != 0;
		fs.mirrorY = floor_ ? (flip & 2) != 0 : (flip & 8) != 0;
		planeZ = floor_ ? rs.floorHeight : rs.ceilingHeight;
	}
	else
	{
		if (rs.platformIndex < 0 || rs.platformIndex >= (int)M.m.platforms.size()) return false;
		const MidPlatform &mp = M.m.platforms[rs.platformIndex];
		const bool top = type == 8;
		fs.textureWord = top ? mp.topTexture : mp.undersideTexture;
		fs.scaleBits = (uint8_t)(top ? (mp.scales >> 4) & 3 : (mp.scales >> 2) & 3);
		fs.shiftX = top ? mp.topShiftX : mp.undersideShiftX;
		fs.shiftY = top ? mp.topShiftY : mp.undersideShiftY;
		planeZ = top ? mp.topZ : mp.undersideZ;
	}
	if ((int16_t)fs.textureWord < 0 || fs.textureWord == M.key) return false;
	const Image &im = M.Img(fs.textureWord);
	if (!im.ok()) return false;
	fs.texW = (uint16_t)im.width; fs.texH = (uint16_t)im.height;
	fs.opaque256 = im.width == 256 && im.height == 256 && !(im.imageType & IT_TRANSLUCENT);
	return true;
}

struct PK { std::string label; int px, py, pz, pa, pitch, dx, dy; uint8_t r[0x24]; int drawnPitch = 0; };
static bool ParsePK(const char *ln, PK &p)
{
	char lab[64], hex[0x24 * 2 + 4];
	if (sscanf(ln, "PK %63s %d %d %d %d %d %d %d %80s %d", lab, &p.px, &p.py, &p.pz, &p.pa, &p.pitch, &p.dx, &p.dy, hex, &p.drawnPitch) < 9) return false;
	if (strlen(hex) != 0x48) return false;
	p.label = lab;
	for (int i = 0; i < 0x24; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); p.r[i] = (uint8_t)v; }
	return true;
}
static uint16_t W(const PK &p, int o) { return (uint16_t)(p.r[o] | (p.r[o + 1] << 8)); }

struct Ray { double cx, cy, eye, hx, hy; double vy; };   // forward-distance t: point (cx,cy)+t*(hx,hy), z = eye - vy*t
static double g_eyeAdd = 0.0;   // the map's eye height over STUDY1's: twice its metadata player height, less 144 (README)
static double g_foot = 1.0;    // the pixel footprint tested, in pixels either way of the ray (README)
static double g_subX = 0.0, g_subY = 0.5, g_fsubX = 1.0, g_fsubY = 0.5;   // where in the pixel the ray goes: the best fit over the STUDY1 poses (README)
static Ray MakeRay(const PK &p, double subX = NAN, double subY = NAN)
{
	Ray r;
	const double th = M_PI / 2 + p.pa * 2 * M_PI / 512;
	const double fwx = cos(th), fwy = sin(th), rtx = sin(th), rty = -cos(th);
	if (std::isnan(subX)) subX = g_subX;
	if (std::isnan(subY)) subY = g_subY;
	const double sx = 320 + p.dx + subX, sy = 240 + p.dy + subY;
	r.cx = p.px; r.cy = p.py; r.eye = p.pz + EYE + g_eyeAdd;
	r.hx = fwx + ((sx - X0) / FX) * rtx; r.hy = fwy + ((sx - X0) / FX) * rty;
	r.vy = (sy - (Y0 + PITCH_PX * p.drawnPitch)) / FY;   // the pitch is a shear of the view's rows (README)
	return r;
}

// IS THE PICK SEEN DIRECTLY? The original also draws through mirrors (the reflect subpass, renderer.c:9569-9592),
// where the face a pixel shows is not where this camera's ray meets it. A pick whose rebuilt ray is stopped before
// the hit -- by a one-sided face, or by a two-sided face whose opening the ray passes above or below -- is not
// seen directly and is counted apart. A uniform grid of faces keeps it cheap.
struct FaceGrid
{
	static const int CELL = 128;
	std::map<std::pair<int, int>, std::vector<int>> cells;
	void Build(const Map &m)
	{
		cells.clear();
		for (size_t i = 0; i < m.faces.size(); i++)
		{
			const Face &f = m.faces[i];
			if (f.vertex1 < 0 || f.vertex2 < 0 || f.sector < 0) continue;
			const Vertex &a = m.vertices[f.vertex1], &b = m.vertices[f.vertex2];
			const int x0 = (int)floor(std::min(a.x, b.x) / (double)CELL), x1 = (int)floor(std::max(a.x, b.x) / (double)CELL);
			const int y0 = (int)floor(std::min(a.y, b.y) / (double)CELL), y1 = (int)floor(std::max(a.y, b.y) / (double)CELL);
			for (int x = x0; x <= x1; x++) for (int y = y0; y <= y1; y++) cells[{ x, y }].push_back((int)i);
		}
	}
};

static bool Blocked(const MapData &M, const FaceGrid &G, const Ray &r, double tHit, int hitFace)
{
	const Map &m = M.m;
	std::set<int> seen;
	const double len = hypot(r.hx, r.hy);
	const double step = FaceGrid::CELL / 2.0 / len;
	for (double t = 0; t <= tHit + step; t += step)
	{
		const int cx = (int)floor((r.cx + r.hx * t) / FaceGrid::CELL), cy = (int)floor((r.cy + r.hy * t) / FaceGrid::CELL);
		for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++)
		{
			auto it = G.cells.find({ cx + dx, cy + dy });
			if (it == G.cells.end()) continue;
			for (int fi : it->second)
			{
				if (fi == hitFace || !seen.insert(fi).second) continue;
				const Face &f = m.faces[fi];
				const Vertex &a = m.vertices[f.vertex1], &b = m.vertices[f.vertex2];
				const double ex = (double)b.x - a.x, ey = (double)b.y - a.y;
				const double den = r.hx * (-ey) - r.hy * (-ex);
				if (fabs(den) < 1e-12) continue;
				const double bx = a.x - r.cx, by = a.y - r.cy;
				const double tt = (bx * (-ey) - by * (-ex)) / den, ss = (r.hx * by - r.hy * bx) / den;
				if (ss < 0 || ss > 1 || tt <= 1 || tt >= tHit - 0.5) continue;
				const double z = r.eye - r.vy * tt;
				const int ns = (f.sister >= 0 && f.sister < (int)m.faces.size()) ? m.faces[f.sister].sector : -1;
				if (ns < 0) return true;
				const Sector &o = m.sectors[f.sector], &n = m.sectors[ns];
				if (z > std::min(o.ceilingHeight, n.ceilingHeight) || z < std::max(o.floorHeight, n.floorHeight)) return true;
			}
		}
	}
	return false;
}

static double FracEdge(double v) { const double f = v - floor(v); return std::min(f, 1 - f); }
static const double EDGE_EPS = 0.08;

// The original's live geometry (boxprobe's GEOM line) over the file's, so heights moved at load or since, swapped
// slots and shifts are the ones it drew with.
static bool OverlayGeometry(MapData &M, const char *log)
{
	FILE *f = fopen(log, "r");
	if (!f) return false;
	std::string line; char buf[65536]; bool geom = false;
	while (!geom && fgets(buf, sizeof buf, f))
	{
		line += buf;
		if (line.empty() || line.back() != '\n') continue;
		if (!line.compare(0, 5, "GEOM "))
		{
			unsigned n = 0; sscanf(line.c_str() + 5, "%u", &n);
			const char *hex = strchr(line.c_str() + 5, ' ');
			if (hex && n <= M.raw.size() && strlen(hex + 1) >= 2 * (size_t)n)
			{
				for (unsigned i = 0; i < n; i++) { unsigned v; sscanf(hex + 1 + 2 * i, "%2x", &v); M.raw[i] = (uint8_t)v; }
				geom = true;
			}
		}
		line.clear();
	}
	fclose(f);
	if (geom) M.m = ParseRaw(M.raw.data(), M.raw.size());
	return geom && M.m.ok();
}

static int TexelCheck(MapData &M, const char *log)
{
	g_eyeAdd = 2.0 * M.m.metadata.playerHeight - 144.0;
	if (!OverlayGeometry(M, log)) printf("%s: no GEOM line in the log: the file's geometry is used\n", M.name.c_str());
	FILE *f = fopen(log, "r");
	if (!f) { printf("no log %s\n", log); return 2; }
	std::map<uint32_t, int> faceByOff;
	for (size_t i = 0; i < M.faceOff.size(); i++) faceByOff[M.faceOff[i]] = (int)i;
	FaceGrid grid; grid.Build(M.m);
	long indirect = 0, stuckWall = 0, offCentre = 0;
	std::map<std::string, long> differBy, sameBy;   // per face / flat class: the differences and the identical picks
	static char ln[1 << 20];
	long nWall = 0, okWall = 0, edgeWall = 0, badWall = 0, nFlat = 0, okFlat = 0, edgeFlat = 0, badFlat = 0, skipped = 0;
	while (fgets(ln, sizeof ln, f))
	{
		PK p;
		if (strncmp(ln, "PK ", 3)) continue;
		if (!ParsePK(ln, p)) continue;
		const uint8_t type = p.r[1];
		// THE CENTRE OF THE VIEW ONLY: toward the sides the original's columns drift off a pinhole camera by more
		// than a pixel, and on the first and last rows a span clipped by the view's edge starts a texel off
		// (measured: STUDY1, README). Neither says anything about the texel rule.
		if (abs(p.dx) > 128 || abs(p.dy) > 220) { offCentre++; continue; }
		const Ray ray = MakeRay(p);
		const int16_t ox = (int16_t)W(p, 4), oy = (int16_t)W(p, 6);
		if (type == 3)
		{
			auto it = faceByOff.find(W(p, 8));
			if (it == faceByOff.end()) { skipped++; continue; }
			const int fi = it->second;
			WallFace wf; int dimU[3], dimV[3], tex[3];
			if (!FaceRule(M, fi, wf, dimU, dimV, tex)) { skipped++; continue; }
			// which piece: the one whose picture the pick names (+2, the DAS entry)
			const int das = W(p, 2);
			const Face &fc = M.m.faces[fi];
			const Vertex &a = M.m.vertices[fc.vertex1], &b = M.m.vertices[fc.vertex2];
			const double ex = (double)b.x - a.x, ey = (double)b.y - a.y;
			const double den = ray.hx * (-ey) - ray.hy * (-ex);
			if (fabs(den) < 1e-9) { skipped++; continue; }
			const double bx = a.x - ray.cx, by = a.y - ray.cy;
			const double t = (bx * (-ey) - by * (-ex)) / den, s = (ray.hx * by - ray.hy * bx) / den;
			const double d = s * wf.length, z = ray.eye - ray.vy * t;
			if (s < -0.01 || s > 1.01 || t <= 0 || Blocked(M, grid, ray, t, fi)) { indirect++; continue; }
			const uint8_t bandBit = SurfBandBit(p.r[0x1a]);
			const int tier = bandBit == 0x04 ? WALL_UPPER : bandBit == 0x02 ? WALL_LOWER : WALL_MID;
			if (tex[tier] != das || dimU[tier] <= 0) { skipped++; continue; }
			const WallPiece wp = WallPieceRule(wf, (WallTier)tier, dimU[tier], dimV[tier]);
			WallPiece wq = wp;
			if (g_mut == 4) wq.mirror = false;
			const double zc = std::min(std::max(z, (double)wp.bot), (double)wp.top);
			const SurfTexel pt = WallPickTexel(wq, dimU[tier], dimV[tier], d, zc);
			nWall++;
			const double uf = wp.u0 + wp.su * d, vf = wp.v0 + wp.sv * (wp.anchor - zc);
			// THE PIXEL'S FOOTPRINT: the camera is a fit, so the texel is only known to within what the pixel covers.
			// A pick whose pixel straddles a texel edge (or the piece's top or bottom) is counted "at an edge".
			bool edge = FracEdge(uf) < EDGE_EPS || FracEdge(vf) < EDGE_EPS || z > wp.top + 0.5 || z < wp.bot - 0.5;
			for (int corner = 0; corner < 4 && !edge; corner++)
			{
				const Ray rc = MakeRay(p, g_subX + ((corner & 1) ? g_foot : -g_foot), g_subY + ((corner & 2) ? g_foot : -g_foot));
				const double dn = rc.hx * (-ey) - rc.hy * (-ex);
				if (fabs(dn) < 1e-9) { edge = true; break; }
				const double tc = (bx * (-ey) - by * (-ex)) / dn, sc = (rc.hx * by - rc.hy * bx) / dn;
				const double zz = rc.eye - rc.vy * tc;
				const double uc = wp.u0 + wp.su * sc * wf.length, vc = wp.v0 + wp.sv * (wp.anchor - zz);
				if (floor(uc + 1e-7) != floor(uf + 1e-7) || floor(vc + 1e-7) != floor(vf + 1e-7) || zz > wp.top || zz < wp.bot) edge = true;
			}
			if (getenv("SB_DUMP")) printf("WD %s %d %d %d %d %d %d %.4f %.4f %d %d %d %d %d %d %d %d %02x %.3f %.3f\n", p.label.c_str(), p.px, p.py, p.pz, p.pa, p.dx, p.dy, uf, vf, ox, oy, pt.x, pt.y, (int)edge, fi, tier, (int)wf.twoSided, wf.flags, t, z);
			// The wrapping row mapper on an image whose row count is not a power of two keeps a running index
			// that sticks (RE:1929-1935; roth_surface.cpp WrapRow): its row is not a function of the point, and
			// only the column is compared there.
			const bool stuck = wp.vMasked && (dimV[tier] & (dimV[tier] - 1)) != 0;
			if (stuck) { if (pt.have && pt.x == ox) stuckWall++; else if (edge) edgeWall++; else if (badWall++ < 20) printf("  WALL DIFFER (column, stuck rows) %s face %d: original %d, port %d\n", p.label.c_str(), fi, ox, pt.x); continue; }
			char cls[96];
			snprintf(cls, sizeof cls, "face %d %s flags %02x %s%dx%d", fi, tier == WALL_UPPER ? "upper" : tier == WALL_LOWER ? "lower" : "mid",
				wf.flags, wf.twoSided ? "two-sided " : "", dimU[tier], dimV[tier]);
			if (pt.have && pt.x == ox && pt.y == oy) { okWall++; if (!edge) sameBy[std::string("wall flags ") + (char)('0' + ((wf.flags >> 4) & 0xf)) + (char)('0' + (wf.flags & 0xf))]++; }
			else if (edge) edgeWall++;
			else if (differBy[cls]++, badWall++ < 20)
				printf("  WALL DIFFER %s (%d,%d) face %d tier %d tex %d dim %dx%d flags %02x: original (%d,%d), port (%d,%d) at d %.2f z %.2f u %.3f v %.3f\n",
					p.label.c_str(), p.dx, p.dy, fi, tier, das, dimU[tier], dimV[tier], wf.flags, ox, oy, pt.x, pt.y, d, z, uf, vf);
		}
		else if (type == 1 || type == 2 || type == 7 || type == 8)
		{
			const uint32_t so = W(p, 0xa);
			if (so < 30 || (so - 30) % 26 || (int)((so - 30) / 26) >= (int)M.m.sectors.size()) { skipped++; continue; }
			const int si = (int)((so - 30) / 26);
			FlatSetup fs; double planeZ;
			if (!FlatRule(M, si, type, fs, planeZ) || W(p, 2) != fs.textureWord) { skipped++; continue; }
			const Ray fray = MakeRay(p, g_fsubX, g_fsubY);
			if (fabs(fray.vy) < 1e-9) { skipped++; continue; }
			const double t = (fray.eye - planeZ) / fray.vy;
			const double x = fray.cx + t * fray.hx, y = fray.cy + t * fray.hy;
			if (t <= 0 || Blocked(M, grid, fray, t, -1)) { indirect++; continue; }
			FlatSetup fq = fs;
			const SurfTexel pt = FlatPickTexel(fq, x, y);
			double cf, rf; FlatPickCoords(fs, x, y, cf, rf);
			nFlat++;
			bool edge = FracEdge(cf) < EDGE_EPS || FracEdge(rf) < EDGE_EPS;
			for (int corner = 0; corner < 4 && !edge; corner++)
			{
				const Ray rc = MakeRay(p, g_fsubX + ((corner & 1) ? g_foot : -g_foot), g_fsubY + ((corner & 2) ? g_foot : -g_foot));
				if (fabs(rc.vy) < 1e-9) { edge = true; break; }
				const double tc = (rc.eye - planeZ) / rc.vy;
				double cc, rr; FlatPickCoords(fs, rc.cx + tc * rc.hx, rc.cy + tc * rc.hy, cc, rr);
				if (tc <= 0 || floor(cc + 1e-7) != floor(cf + 1e-7) || floor(rr + 1e-7) != floor(rf + 1e-7)) edge = true;
			}
			if (getenv("SB_DUMP")) printf("FD %u %d %u %u %u %u %u %u %d %d %d %.4f %.4f %d %d %.4f %.4f %d %d %d %d\n", type, si, fs.textureWord, fs.texW, fs.texH, fs.scaleBits,
				fs.shiftX, fs.shiftY, fs.mirrorX, fs.mirrorY, fs.opaque256, x, y, ox, oy, cf, rf, p.dx, p.dy, (int)planeZ, (int)edge);
			SurfTexel q = pt;
			if (g_mut == 5) std::swap(q.x, q.y);
			char fcls[96];
			snprintf(fcls, sizeof fcls, "flat type %u sector %d %ux%u s%u%s%s%s", type, si, fs.texW, fs.texH, fs.scaleBits, fs.opaque256 ? " opaque256" : "",
				fs.mirrorX ? " mirrorX" : "", fs.mirrorY ? " mirrorY" : "");
			if (q.have && q.x == ox && q.y == oy) { okFlat++; if (!edge) { char k[64]; snprintf(k, sizeof k, "flat type %u %ux%u s%u%s%s%s", type, fs.texW, fs.texH, fs.scaleBits, fs.opaque256 ? " opaque256" : "", fs.mirrorX ? " mirrorX" : "", fs.mirrorY ? " mirrorY" : ""); sameBy[k]++; } }
			else if (edge) edgeFlat++;
			else if (differBy[fcls]++, badFlat++ < 30)
				printf("  FLAT DIFFER %s (%d,%d) type %u sector %d tex %u %ux%u s%u sh %u,%u mir %d%d o256 %d: original (%d,%d), port (%d,%d) col %.3f row %.3f\n",
					p.label.c_str(), p.dx, p.dy, type, si, fs.textureWord, fs.texW, fs.texH, fs.scaleBits, fs.shiftX, fs.shiftY, fs.mirrorX, fs.mirrorY,
					fs.opaque256, ox, oy, q.x, q.y, cf, rf);
		}
	}
	fclose(f);
	{
		std::vector<std::pair<long, std::string>> v;
		for (auto &kv : differBy) v.push_back({ kv.second, kv.first });
		std::sort(v.rbegin(), v.rend());
		for (size_t i = 0; i < v.size() && i < 15; i++) printf("  differ %6ld  %s\n", v[i].first, v[i].second.c_str());
		for (auto &kv : sameBy) printf("  identical (clear of an edge) %7ld  %s\n", kv.second, kv.first.c_str());
	}
	printf("%s: wall picks %ld: %ld identical, %ld column identical on a stuck-row piece, %ld at a texel edge (camera fit), %ld differ; flat picks %ld: %ld identical, %ld at an edge, %ld differ; %ld not on a known piece, %ld not seen directly (a mirror), %ld outside the centre of the view\n",
		M.name.c_str(), nWall, okWall, stuckWall, edgeWall, badWall, nFlat, okFlat, edgeFlat, badFlat, skipped, indirect, offCentre);
	return (badWall == 0 && badFlat == 0) ? 0 : 1;
}

//---------------------------------------------------------------------------
// texel poses
//---------------------------------------------------------------------------
static std::vector<int> SectorsAt(const MapData &M, double x, double y)
{
	std::vector<int> out;
	std::vector<char> in(M.m.sectors.size(), 0);
	for (const Face &f : M.m.faces)
	{
		if (f.sector < 0 || f.vertex1 < 0 || f.vertex2 < 0) continue;
		const Vertex &a = M.m.vertices[f.vertex1], &b = M.m.vertices[f.vertex2];
		if ((a.y > y) != (b.y > y))
		{
			const double xi = a.x + (y - a.y) * (double)(b.x - a.x) / (double)(b.y - a.y);
			if (x < xi) in[f.sector] ^= 1;
		}
	}
	for (size_t s = 0; s < in.size(); s++) if (in[s]) out.push_back((int)s);
	return out;
}
static int Angle512(double dx, double dy)
{
	double a = (atan2(dy, dx) - M_PI / 2) * 512 / (2 * M_PI);
	int i = (int)floor(a + 0.5);
	return ((i % 512) + 512) % 512;
}

static int TexelScript(MapData &M, const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) return 2;
	std::set<int> faces, sectors;
	const std::vector<Case> cs = MatchCases(M);
	for (const Case &c : cs) { if (c.face >= 0) faces.insert(c.face); else sectors.insert(c.sector); }
	int poses = 0;
	// Clear of every face by 64 units: the original keeps the player out of walls by its own collision radius,
	// and a pose inside it is drawn from somewhere else than the pose says.
	auto clear = [&](double x, double y)
	{
		for (const Face &fc : M.m.faces)
		{
			if (fc.vertex1 < 0 || fc.vertex2 < 0) continue;
			const Vertex &a = M.m.vertices[fc.vertex1], &b = M.m.vertices[fc.vertex2];
			const double ex = b.x - a.x, ey = b.y - a.y, l2 = ex * ex + ey * ey;
			double u = l2 > 0 ? ((x - a.x) * ex + (y - a.y) * ey) / l2 : 0;
			u = std::max(0.0, std::min(1.0, u));
			if (hypot(a.x + u * ex - x, a.y + u * ey - y) < 64) return false;
		}
		return true;
	};
	auto pose = [&](double x, double y, double dx, double dy, const char *tag, int id, const char *grid, int pitch = 0)
	{
		const std::vector<int> at = SectorsAt(M, x, y);
		if (at.size() != 1 || !clear(x, y)) return false;
		const Sector &s = M.m.sectors[at[0]];
		if (s.ceilingHeight - s.floorHeight < 160) return false;
		fprintf(f, "P %d %d %d %u %d %d %s%d_%d | %s\n", (int)floor(x + 0.5), (int)floor(y + 0.5), Angle512(dx, dy), M.SectorOff(at[0]),
			s.floorHeight, pitch, tag, id, poses, grid);
		poses++;
		return true;
	};
	for (int fi : faces)
	{
		const Face &fc = M.m.faces[fi];
		const Vertex &a = M.m.vertices[fc.vertex1], &b = M.m.vertices[fc.vertex2];
		const double mx = (a.x + b.x) / 2.0, my = (a.y + b.y) / 2.0, len = hypot((double)b.x - a.x, (double)b.y - a.y);
		if (len < 1) continue;
		double nx = -(b.y - a.y) / len, ny = (b.x - a.x) / len;
		{
			const std::vector<int> at = SectorsAt(M, mx + nx * 4, my + ny * 4);
			if (std::find(at.begin(), at.end(), fc.sector) == at.end()) { nx = -nx; ny = -ny; }
		}
		int made = 0;
		// Square-on, and only where the face runs along an axis: the original's own view of a turned pose is
		// measurably off the pinhole camera this side rebuilds the ray with (tools/surfbox_test/README.md).
		if (fabs(nx) > 1e-9 && fabs(ny) > 1e-9) continue;
		for (double d : { 64.0, 110.0, 170.0 })
			if (pose(mx + nx * d, my + ny * d, -nx, -ny, "f", fi, "-160 160 5 -230 230 5")) made++;
		(void)made;
	}
	for (int si : sectors)
	{
		double cx = 0, cy = 0; int n = 0;
		for (const Face &fc : M.m.faces) if (fc.sector == si && fc.vertex1 >= 0) { cx += M.m.vertices[fc.vertex1].x; cy += M.m.vertices[fc.vertex1].y; n++; }
		if (!n) continue;
		cx /= n; cy /= n;
		// Looking DOWN (pitch -126, the original's limit: player.c:92-94), so the floor is near and a pixel covers
		// little of it: from the floor's middle, and from 60 and 120 units back along each axis.
		int made = 0;
		for (double d : { 0.0, 60.0, 120.0 })
			for (int k = 0; k < 4 && made < 5; k++)
			{
				if (d == 0 && k > 0) break;
				const double th = k * M_PI / 2, dx = cos(th), dy = sin(th);
				const double x = cx - dx * d, y = cy - dy * d;
				const std::vector<int> at = SectorsAt(M, x, y);
				if (at.size() != 1 || M.m.sectors[at[0]].floorHeight > M.m.sectors[si].floorHeight + 40) continue;
				if (pose(x, y, dx, dy, "s", si, "-128 128 4 -200 238 4", -126)) made++;
			}
	}
	fprintf(f, "Q\n");
	fclose(f);
	printf("%s: %d poses for %zu faces and %zu sectors -> %s\n", M.name.c_str(), poses, faces.size(), sectors.size(), path);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: see the header\n"); return 2; }
	if (!TheInstall().Open(argv[1])) { fprintf(stderr, "install: %s\n", TheInstall().Error().c_str()); return 2; }
	if (!strcmp(argv[2], "census")) return Census(argv[1]);
	if (argc < 5) { fprintf(stderr, "usage: see the header\n"); return 2; }
	if (argc > 5) g_mut = atoi(argv[5]);
	if (getenv("SB_SUBX")) g_subX = atof(getenv("SB_SUBX"));
	if (getenv("SB_SUBY")) g_subY = atof(getenv("SB_SUBY"));
	if (getenv("SB_FSUBX")) g_fsubX = atof(getenv("SB_FSUBX"));
	if (getenv("SB_FSUBY")) g_fsubY = atof(getenv("SB_FSUBY"));
	if (getenv("SB_FX")) FX = atof(getenv("SB_FX"));
	if (getenv("SB_PITCH")) PITCH_PX = atof(getenv("SB_PITCH"));
	if (getenv("SB_FOOT")) g_foot = atof(getenv("SB_FOOT"));
	if (getenv("SB_X0")) X0 = atof(getenv("SB_X0"));
	if (getenv("SB_FY")) FY = atof(getenv("SB_FY"));
	if (getenv("SB_Y0")) Y0 = atof(getenv("SB_Y0"));
	MapData M;
	if (!LoadMap(argv[2], M)) { fprintf(stderr, "map %s not loaded\n", argv[2]); return 2; }
	const std::string mode = argv[3];
	if (mode == "match-script") return MatchScript(M, argv[4]);
	if (mode == "match-check") return MatchCheck(M, argv[4]);
	if (mode == "texel-check") return TexelCheck(M, argv[4]);
	if (mode == "texel-script") return TexelScript(M, argv[4]);
	fprintf(stderr, "unknown mode %s\n", argv[3]);
	return 2;
}
