// cmdcore_test.cpp -- the differential test for src/roth/roth_cmdcore.cpp.
//
// The ORIGINAL engine (ROTH.C built headless) is driven by a probe mod that runs
// a small script: call this RAW-command handler on that record, run this chain,
// stage the player here, and after each step print every byte that changed in
// the command table, the map geometry and the object lists, plus the chain
// globals and the particle pool. This tool writes that script for a retail map
// (`gen`) and runs the SAME script through the port (`run`), printing the same
// lines. The two outputs are then compared with diff; see run_diff.sh.
//
// It reads the player's own .RAW files and writes only what it is told to.
//
// Build: g++ -std=c++17 -O1 -I ../../src/roth -o cmdcore_test cmdcore_test.cpp
//   ../../src/roth/roth_cmdcore.cpp ../../src/roth/roth_commands.cpp
//   ../../src/roth/roth_raw.cpp ../../src/roth/roth_install.cpp

#include "roth_cmdcore.h"
#include "roth_raw.h"
#include "roth_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

using namespace roth;

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}

//==========================================================================
// The map as the original holds it in memory: the file's first block, with the
// records at the offsets the reader walked past.
//==========================================================================

struct Layout
{
	std::vector<uint32_t> sectorOff, faceOff, texmapOff, platformOff;
	uint32_t geomSize = 0;
	void Build(const Map &m)
	{
		uint32_t at = 30;
		for (size_t i = 0; i < m.sectors.size(); i++) { sectorOff.push_back(at); at += 0x1a; }
		at += 2;
		for (size_t i = 0; i < m.faces.size(); i++) { faceOff.push_back(at); at += 0x0c; }
		at += 2;
		for (size_t i = 0; i < m.textureMaps.size(); i++) { texmapOff.push_back(at); at += m.textureMaps[i].extended ? 14 : 10; }
		if (m.header.midPlatformsOffset != 0)
		{
			at += 2;
			for (size_t i = 0; i < m.platforms.size(); i++) { platformOff.push_back(at); at += 14; }
		}
		geomSize = m.header.verticesOffset;
	}
};

static void W16(std::vector<uint8_t> &b, uint32_t off, uint16_t v) { if (off + 1 < b.size()) { b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8); } }
static void W8(std::vector<uint8_t> &b, uint32_t off, uint8_t v) { if (off < b.size()) b[off] = v; }

// Only the fields a command can write are laid over the file's own bytes.
static std::vector<uint8_t> GeomImage(const Map &m, const Layout &L, const std::vector<uint8_t> &file)
{
	std::vector<uint8_t> b(file.begin(), file.begin() + std::min<size_t>(L.geomSize, file.size()));
	for (size_t i = 0; i < m.sectors.size(); i++)
	{
		const Sector &s = m.sectors[i]; const uint32_t o = L.sectorOff[i];
		W16(b, o + 0, (uint16_t)s.ceilingHeight); W16(b, o + 2, (uint16_t)s.floorHeight);
		W16(b, o + 6, s.ceilingTexture); W16(b, o + 8, s.floorTexture);
		W8(b, o + 0x0a, s.flags); W8(b, o + 0x0b, s.light);
		W16(b, o + 0x16, s.flags2);
	}
	for (size_t i = 0; i < m.faces.size(); i++) W16(b, L.faceOff[i] + 0x0a, m.faces[i].collisionFlags);
	for (size_t i = 0; i < m.textureMaps.size(); i++)
	{
		const TextureMap &t = m.textureMaps[i]; const uint32_t o = L.texmapOff[i];
		W16(b, o + 2, t.midTexture); W16(b, o + 4, t.upperTexture); W16(b, o + 6, t.lowerTexture);
		W8(b, o + 8, t.flags);
	}
	for (size_t i = 0; i < m.platforms.size(); i++)
	{
		const MidPlatform &p = m.platforms[i]; const uint32_t o = L.platformOff[i];
		W16(b, o + 0, p.undersideTexture); W16(b, o + 6, p.topTexture);
	}
	return b;
}

static std::vector<uint8_t> CmdBytes(const Command &c)
{
	std::vector<uint8_t> b;
	const uint16_t size = (uint16_t)(6 + 2 * c.args.size());
	b.push_back((uint8_t)size); b.push_back((uint8_t)(size >> 8));
	b.push_back(c.modifier); b.push_back(c.opcode);
	b.push_back((uint8_t)c.linkIndex); b.push_back((uint8_t)(c.linkIndex >> 8));
	for (uint16_t a : c.args) { b.push_back((uint8_t)a); b.push_back((uint8_t)(a >> 8)); }
	return b;
}

static std::vector<uint8_t> ObjBytes(const Object &o)
{
	std::vector<uint8_t> b(16);
	b[0] = (uint8_t)o.x; b[1] = (uint8_t)((uint16_t)o.x >> 8); b[2] = (uint8_t)o.y; b[3] = (uint8_t)((uint16_t)o.y >> 8);
	b[4] = o.textureIndex; b[5] = o.textureSource; b[6] = o.rotation; b[7] = o.flags; b[8] = o.light; b[9] = o.renderType;
	b[10] = (uint8_t)o.z; b[11] = (uint8_t)((uint16_t)o.z >> 8); b[12] = (uint8_t)o.unk0C; b[13] = (uint8_t)(o.unk0C >> 8);
	b[14] = (uint8_t)o.commandID; b[15] = (uint8_t)(o.commandID >> 8);
	return b;
}

//==========================================================================
// The port, set up as the engine sets it up, with a host that needs no engine.
//==========================================================================

struct Port : public CmdHost
{
	Map map;
	Layout L;
	std::vector<uint8_t> file;
	CmdCore core;
	uint32_t crng = 0xcc61;
	uint16_t pending = 0;
	int16_t px = 0, py = 0;
	uint16_t angle = 0;
	bool autoSel = false;
	std::vector<uint16_t> queue;          // the deferred queue's front buffer: chain indices
	std::set<int> flags;                  // DBASE100 flag bits that are set
	uint16_t nextActive = 0;
	int lastResult = 0;
	std::vector<std::string> notes;

	// snapshots
	std::vector<std::vector<uint8_t>> snapC, snapO;
	std::vector<uint8_t> snapG;

	bool ItemAutoSelected() override { return autoSel; }
	void RunPendingRecord(uint16_t idx) override { notes.push_back("pending record " + std::to_string(idx) + " would run"); }
	bool QueueDeferred(uint16_t chain) override { if (queue.size() >= 16) return false; queue.push_back(chain); return true; }
	void PlayerXY(int16_t &x, int16_t &y) override { x = px; y = py; }
	uint16_t PlayerAngle() override { return angle; }
	void SetPlayerAngle(uint16_t a) override { angle = a; }

	void Snap()
	{
		snapC.clear(); for (const Command &c : map.commands) snapC.push_back(CmdBytes(c));
		snapG = GeomImage(map, L, file);
		snapO.clear();
		for (auto &list : map.objects) for (const Object &o : list) snapO.push_back(ObjBytes(o));
	}
	void Diff(const char *tag, FILE *out);
	int RunOne(Command &rec, int index, Handlers &h);
	void Chain(uint16_t index);
};

static int Tri(int v) { return v == 0 ? 0 : (v & 1) ? 1 : 2; }

void Port::Diff(const char *tag, FILE *out)
{
	fprintf(out, "D %s\n", tag);
	for (size_t i = 0; i < map.commands.size(); i++)
	{
		const std::vector<uint8_t> now = CmdBytes(map.commands[i]);
		for (size_t k = 0; k < now.size(); k++)
			if (snapC[i][k] != now[k]) fprintf(out, "C %u %u %02x %02x\n", (unsigned)(i + 1), (unsigned)k, snapC[i][k], now[k]);
	}
	const std::vector<uint8_t> g = GeomImage(map, L, file);
	for (size_t k = 0; k < g.size(); k++) if (snapG[k] != g[k]) fprintf(out, "G %u %02x %02x\n", (unsigned)k, snapG[k], g[k]);
	size_t n = 0;
	for (size_t s = 0; s < map.objects.size(); s++)
		for (size_t j = 0; j < map.objects[s].size(); j++, n++)
		{
			const std::vector<uint8_t> now = ObjBytes(map.objects[s][j]);
			for (size_t k = 0; k < 16; k++)
				if (snapO[n][k] != now[k]) fprintf(out, "O %u %u %u %02x %02x\n", (unsigned)s, (unsigned)j, (unsigned)k, snapO[n][k], now[k]);
		}
	fprintf(out, "S int=%u mode=%u delta=%u pend=%u res=%d next=%u ang=%u hp=%d spd=%u crng=%08x prng=%04x flash=%d q=%u",
		core.interrupt, core.stepMode, core.stepDelta, pending, Tri(lastResult), nextActive, angle, core.health,
		core.speedRequest, crng, core.particleRng, core.damageFlash, (unsigned)queue.size());
	for (uint16_t c : queue) fprintf(out, " [%u,0]", c);
	fprintf(out, "\n");
	for (const std::string &s : notes) fprintf(out, "N %s\n", s.c_str());
	notes.clear();
	Snap();
}

// The opcodes a test chain may contain besides the core's own. These few are
// re-stated here from ROTH.C, for the test only, so a chain can be walked end to
// end without the engine: 0x17 (:541), 0x26 (:592), 0x28 (:4665), 0x38 (:41),
// 0x40 (:3955), 0x12 (:4420, its registration half), 0x06/0x3e (:499/:506).
int Port::RunOne(Command &rec, int index, Handlers &h)
{
	const uint8_t op = rec.opcode & 0x7f;
	int r = 0;
	core.interrupt = (uint8_t)h.interrupt;
	if (core.Dispatch(op, rec, r)) { h.interrupt = (Interrupt)core.interrupt; return r; }
	auto consume = [&](int ret) { if (rec.Byte(6) & 0x10) { rec.modifier |= 8; } pending = 0; return ret; };
	auto ifnext = [&]() { h.interrupt = Interrupt::Rerun; int ret = 0; if (pending != 0) { if (!autoSel) { RunPendingRecord(pending); pending = 0; } else ret = CMD_LATCHED; } return ret; };
	switch (op)
	{
	case 0x06: case 0x3e: return CMD_ACTED;
	case 0x17:
	{
		const uint16_t idx = rec.Word(8);
		if (idx == 0 || idx > map.commands.size()) return 0;
		Command &t = map.commands[idx - 1];
		const uint8_t bit = (t.opcode == 0x06 || t.opcode == 0x2c) ? 0x02 : 0x08;
		t.modifier ^= bit;
		const uint8_t fl = rec.Byte(6);
		if (fl & 0x06) { t.modifier |= bit; if (fl & 0x02) t.modifier &= (uint8_t)~bit; }
		if (!(fl & 0x20)) rec.modifier |= 8;
		return CMD_ACTED;
	}
	case 0x26:
	{
		const int id = rec.Word(8); const uint8_t fl = rec.Byte(6);
		const bool set = flags.count(id) != 0;
		const bool doSet = (fl & 2) ? !set : (fl & 1) ? false : true;
		if (doSet == set) return 0;
		if (doSet) flags.insert(id); else flags.erase(id);
		return consume(CMD_ACTED);
	}
	case 0x28:
	{
		const bool set = flags.count(rec.Word(8)) != 0; const int f1 = rec.Byte(6) & 1;
		if (set) return f1 ? consume(0) : ifnext();
		return f1 ? ifnext() : consume(CMD_ACTED);
	}
	case 0x38: h.nextActive = rec.Word(8); return 0;
	case 0x40:
	{
		uint16_t ax = rec.Byte(6) | (rec.Byte(7) << 8);
		int last = 0;
		while (ax != 0 && ax <= map.commands.size())
		{
			Command &c = map.commands[ax - 1];
			if (!(c.modifier & 8)) { last = RunOne(c, ax, h); lastResult |= last; if (h.interrupt != Interrupt::None) break; }
			if (c.opcode == 0x12) break;
			if (c.linkIndex == 0) break;
			ax = c.linkIndex;
		}
		return last;
	}
	case 0x12:
	{
		const uint8_t m = rec.modifier;
		if (!(m & 4) && (m & 0x21)) return 0;
		if (m & 2) crng = crng * 0x5e5u + 0x29u;
		if (!(rec.modifier & 4)) rec.modifier |= 0x20;
		return CMD_ACTED;
	}
	default: return 0;      // the verified no-ops and the trigger slots
	}
	(void)index;
}

// reset_command_chain_state (raw_commands.c:1323-1333) over the port's spine.
void Port::Chain(uint16_t index)
{
	Handlers h;
	Handlers *hp = &h;
	lastResult = 0;
	h.Run = [this, hp](const Map &, const Command &c, int idx)
	{
		const int r = RunOne(map.commands[idx - 1], idx, *hp);
		(void)c;
		return r;
	};
	lastResult |= ExecChain(map, index, h);
	core.interrupt = (uint8_t)h.interrupt;
	nextActive = h.nextActive;
}

//==========================================================================
// gen: the script for one map.
//==========================================================================

static const std::set<int> kChainSafe = { 0x00, 0x01, 0x04, 0x05, 0x06, 0x08, 0x12, 0x13, 0x14, 0x15, 0x17, 0x18, 0x19, 0x1a,
	0x1b, 0x1c, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x25, 0x26, 0x28, 0x2c, 0x2e, 0x30, 0x31, 0x32, 0x33, 0x37, 0x38, 0x39,
	0x3d, 0x3e, 0x3f, 0x40, 0x41 };
static const std::set<int> kMine = { 0x14, 0x15, 0x1c, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x2d, 0x2e, 0x33, 0x35, 0x3f, 0x41 };

// Everything a chain can reach, and whether all of it is in the safe set.
static bool Reach(const Map &m, uint16_t start, std::set<int> &seen, std::set<int> &flagIds, bool &hasMine)
{
	std::vector<uint16_t> todo{ start };
	bool ok = true;
	while (!todo.empty())
	{
		uint16_t i = todo.back(); todo.pop_back();
		if (i == 0 || i > m.commands.size() || seen.count(i)) continue;
		seen.insert(i);
		const Command &c = m.commands[i - 1];
		const int op = c.opcode & 0x7f;
		if (!kChainSafe.count(op)) ok = false;
		if (kMine.count(op)) hasMine = true;
		if (op == 0x26 || op == 0x28) flagIds.insert(c.Word(8));
		if (op == 0x40) todo.push_back((uint16_t)(c.Byte(6) | (c.Byte(7) << 8)));
		if (op == 0x38 || op == 0x1e) todo.push_back(c.Word(8));
		// 0x17 can re-enable a record anywhere; its target is only toggled, not run.
		todo.push_back(c.linkIndex);
	}
	return ok;
}

static void Gen(const Map &m, const Layout &L, FILE *o)
{
	int step = 0;
	auto diff = [&](const char *what, size_t rec) { fprintf(o, "diff %s_%zu_%d\n", what, rec, step++); };
	fprintf(o, "set32 85324 2\nset32 8a104 0\nset32 8a108 0\nset8 8a268 0\nset8 8a26a 0\nset16 90a8a 77\nset32 71f48 0x1234\nset16 7276c 0x3f3\nset32 8a0f0 2048\nset32 89f3b 0\nset32 89f54 0\nset32 89f60 0\nset32 8a138 0\npend 0\nsrc 65535 0\nface 0\nclearq\nsnap\ndiff start\n");

	// A face trigger's faces, for the key-0 forms.
	std::vector<int> useFaces;
	for (size_t fi = 0; fi < m.faces.size() && useFaces.size() < 3; fi++)
	{
		const Face &f = m.faces[fi];
		if (f.textureMap >= 0 && m.textureMaps[f.textureMap].extended && m.textureMaps[f.textureMap].faceID != 0) useFaces.push_back((int)fi);
	}
	for (size_t fi = 0; fi < m.faces.size(); fi++)
		if (m.faces[fi].sister >= 0 && m.faces[fi].textureMap >= 0 && !m.textureMaps[m.faces[fi].textureMap].extended) { useFaces.push_back((int)fi); break; }
	// An object, for the source forms.
	int objS = -1, objJ = -1;
	for (size_t s = 0; s < m.objects.size() && objS < 0; s++) if (!m.objects[s].empty()) { objS = (int)s; objJ = 0; }

	for (size_t i = 0; i < m.commands.size(); i++)
	{
		const Command &c = m.commands[i];
		const int op = c.opcode & 0x7f;
		const size_t n = i + 1;
		static const std::map<int, const char *> countFn = { { 0x15, "cmd_count" }, { 0x1f, "cmd_texture_change_count" },
			{ 0x22, "cmd_count_addl_arg" }, { 0x21, "cmd_animate_facegroup_texture" } };
		if (countFn.count(op))
		{
			const char *fn = countFn.at(op);
			int steps = std::min<int>(c.Word(8) + 3, 10);
			for (int k = 0; k < steps; k++) { fprintf(o, "h %s %zu\n", fn, n); diff("count", n); }
			// every step the map's own 0x1e records can ask of it
			std::set<std::pair<int, int>> modes;
			for (const Command &e : m.commands)
				if ((e.opcode & 0x7f) == 0x1e && (e.Word(8) == n || e.Word(8) == 0)) modes.insert({ (e.Byte(6) & 3) + 1, e.Word(0x0a) });
			int tried = 0;
			for (auto &md : modes)
			{
				if (tried++ >= 6) break;
				for (int k = 0; k < 3; k++)
				{
					fprintf(o, "set32 8a104 %d\nset32 8a108 %d\nh %s %zu\n", md.first, md.second, fn, n);
					diff("countm", n);
				}
			}
			fprintf(o, "pend %zu\nh %s %zu\n", n, fn, n); diff("countpend", n);     // a latched record, suppressed
			fprintf(o, "pend 0\n");
		}
		else if (op == 0x1e) { for (int k = 0; k < 4; k++) { fprintf(o, "h cmd_modify_count %zu\n", n); diff("modify", n); } fprintf(o, "clearq\nset32 8a104 0\n"); }
		else if (op == 0x1c) { for (int k = 0; k < 3; k++) { fprintf(o, "h cmd_cycle_texture %zu\n", n); diff("cycle", n); } }
		else if (op == 0x14) { for (int k = 0; k < 3; k++) { fprintf(o, "h cmd_sync_facegroup_texture %zu\n", n); diff("sync", n); } }
		else if (op == 0x20)
		{
			if (c.Word(8) == 0 && objS >= 0) fprintf(o, "src %d %d\n", objS, objJ);
			for (int k = 0; k < 3; k++) { fprintf(o, "h cmd_cycle_object_texture %zu\n", n); diff("cycleobj", n); }
			fprintf(o, "src 65535 0\n");
		}
		else if (op == 0x2e)
		{
			if (c.Word(8) != 0) { for (int k = 0; k < 3; k++) { fprintf(o, "h cmd_smash_face_texture %zu\n", n); diff("smash", n); } }
			else
			{
				fprintf(o, "h cmd_smash_face_texture %zu\n", n); diff("smashnone", n);
				for (int fi : useFaces) { fprintf(o, "face %u\n", L.faceOff[fi]); for (int k = 0; k < 2; k++) { fprintf(o, "h cmd_smash_face_texture %zu\n", n); diff("smash0", n); } }
				fprintf(o, "face 0\nset8 8a268 0\n");
			}
		}
		else if (op == 0x3f)
		{
			static const int angs[3] = { 0, 100, 500 };
			for (int a : angs) { fprintf(o, "set16 90a8a %d\nset32 71f48 0x1234\nh cmd_player_rotation %zu\n", a, n); diff("rot", n); }
			fprintf(o, "rec8 %zu 2 0\n", n);
		}
		else if (op == 0x41) { fprintf(o, "h cmd_set_player_speed_reduction %zu\n", n); diff("slow", n); fprintf(o, "set32 89f54 0\n"); }
		else if (op == 0x33)
		{
			const int r = c.Word(0x0a);
			struct Pos { int dx, dy; };
			std::vector<Pos> pos = { { 0, 0 } };
			if (r != 0) pos = { { 0, 0 }, { r, 0 }, { 0, r + r / 2 }, { 2 * r - 1, 0 }, { 2 * r, 0 }, { 3 * r, 3 * r } };
			for (int scale = 0; scale < 2; scale++)
			{
				// from an object
				if (objS >= 0)
					for (const Pos &p : pos)
					{
						const Object &ob = m.objects[objS][objJ];
						fprintf(o, "set32 8a0f0 2048\nset32 89f3b 0\nset8 8a26a %d\nsrc %d %d\nface 0\nset16 90a8e %d\nset16 90a96 %d\nrec8 %zu 2 0\nh cmd_apply_damage %zu\n",
							scale, objS, objJ, (uint16_t)(ob.x + p.dx - m.wrapShiftX), (uint16_t)(ob.y + p.dy - m.wrapShiftY), n, n);
						diff("dmgobj", n);
					}
				// from a wall
				if (!useFaces.empty())
					for (const Pos &p : pos)
					{
						const Face &f = m.faces[useFaces[0]];
						const Vertex &a = m.vertices[f.vertex1], &b = m.vertices[f.vertex2];
						fprintf(o, "set32 8a0f0 2048\nset32 89f3b 0\nset8 8a26a %d\nsrc 65535 0\nface %u\nset16 90a8e %d\nset16 90a96 %d\nrec8 %zu 2 0\nh cmd_apply_damage %zu\n",
							scale, L.faceOff[useFaces[0]], (uint16_t)((a.x + b.x) / 2 + p.dx - m.wrapShiftX), (uint16_t)((a.y + b.y) / 2 + p.dy - m.wrapShiftY), n, n);
						diff("dmgwall", n);
					}
				// from nothing at all
				fprintf(o, "set32 8a0f0 2048\nset32 89f3b 0\nset8 8a26a %d\nsrc 65535 0\nface 0\nrec8 %zu 2 0\nh cmd_apply_damage %zu\n", scale, n, n);
				diff("dmgnone", n);
			}
			// the flash and a low health
			fprintf(o, "set32 8a0f0 5\nset32 89f3b 300\nset8 8a26a 0\nrec8 %zu 2 0\nh cmd_apply_damage %zu\n", n, n); diff("dmglow", n);
			fprintf(o, "set32 8a0f0 2048\nset32 89f3b 0\nrec8 %zu 2 0\n", n);
		}
		else if (op == 0x35)
		{
			// the group's first wall, the player at a spread of distances from its middle
			int tm = -1;
			for (size_t t = 0; t < m.textureMaps.size(); t++) if (m.textureMaps[t].extended && m.textureMaps[t].faceID == c.Word(8) && c.Word(8) != 0) { tm = (int)t; break; }
			int face = -1;
			if (tm >= 0) for (size_t fi = 0; fi < m.faces.size(); fi++) if (m.faces[fi].textureMap == tm) { face = (int)fi; break; }
			const uint8_t mod0 = c.modifier;
			for (int pass = 0; pass < 2; pass++)       // as authored, then with the armed bits cleared
			{
				if (pass == 1) fprintf(o, "rec8 %zu 2 0\n", n);
				if (face >= 0)
				{
					const Vertex &a = m.vertices[m.faces[face].vertex1], &b = m.vertices[m.faces[face].vertex2];
					const double mx = (a.x + b.x) / 2.0, my = (a.y + b.y) / 2.0;
					double nx = -(b.y - a.y), ny = (b.x - a.x); const double len = sqrt(nx * nx + ny * ny); if (len > 0) { nx /= len; ny /= len; }
					const int reach = c.Word(0x0a) * 2 + 0x1c;
					static const double frac[6] = { 0.0, 0.5, 0.95, 1.05, 2.0, -0.5 };
					for (double fr : frac)
					{
						fprintf(o, "set32 8a0f0 2048\nset32 89f3b 0\nset16 90a8e %d\nset16 90a96 %d\nh cmd_face_emits_damage %zu\n",
							(uint16_t)((int)lround(mx + nx * reach * fr) - m.wrapShiftX), (uint16_t)((int)lround(my + ny * reach * fr) - m.wrapShiftY), n);
						diff("walldmg", n);
						if (pass == 1) fprintf(o, "rec8 %zu 2 0\n", n);
					}
				}
				else { fprintf(o, "h cmd_face_emits_damage %zu\n", n); diff("walldmg0", n); }
			}
			fprintf(o, "rec8 %zu 2 %u\nset32 8a0f0 2048\nset32 89f3b 0\n", n, mod0);
		}
		else if (op == 0x2d)
		{
			fprintf(o, "clearparts\nset16 7276c 0x3f3\nsrc 65535 0\nface 0\nh cmd_particle_effect %zu\n", n); diff("partnone", n); fprintf(o, "parts\n");
			if (objS >= 0)
			{
				fprintf(o, "clearparts\nset16 7276c 0x3f3\nsrc %d %d\nface 0\nh cmd_particle_effect %zu\n", objS, objJ, n); diff("partobj", n); fprintf(o, "parts\n");
				for (int k = 0; k < 4; k++) fprintf(o, "call0 tick_particles\n");
				fprintf(o, "parts\n");
			}
			if (!useFaces.empty())
			{
				fprintf(o, "clearparts\nset16 7276c 0x4a11\nsrc 65535 0\nface %u\nh cmd_particle_effect %zu\n", L.faceOff[useFaces[0]], n); diff("partwall", n); fprintf(o, "parts\n");
				for (int k = 0; k < 4; k++) fprintf(o, "call0 tick_particles\n");
				fprintf(o, "parts\n");
			}
			fprintf(o, "clearparts\nsrc 65535 0\nface 0\n");
		}
	}

	// Whole chains, from every trigger, where every reachable record is one the
	// test can run on both sides. Fired three times each: counters and one-shot
	// bits make the second and third runs different from the first.
	fprintf(o, "set32 8a104 0\nset32 8a108 0\nset8 8a268 0\nset8 8a26a 0\npend 0\nsrc 65535 0\nface 0\nclearq\nset32 8a0f0 2048\nset32 89f3b 0\nset16 90a8a 77\ndiff prechain\n");
	std::set<uint16_t> starts;
	for (const Command &c : m.commands) if (c.isTrigger && c.chainStart != 0) starts.insert(c.chainStart);
	for (uint16_t s : starts)
	{
		std::set<int> seen, flagIds; bool hasMine = false;
		if (!Reach(m, s, seen, flagIds, hasMine) || !hasMine) continue;
		for (int id : flagIds) fprintf(o, "callv dbase100_bitmap_test_clear %d\n", id);
		for (int k = 0; k < 3; k++)
		{
			fprintf(o, "chain %u\n", s); diff("chain", s);
			fprintf(o, "drain\n"); diff("drain", s);
			fprintf(o, "drain\n"); diff("drain2", s);
		}
		fprintf(o, "clearq\nset8 8a268 0\nset32 8a104 0\n");
	}
}

//==========================================================================
// run: the same script through the port.
//==========================================================================

static void Run(Port &P, FILE *in, FILE *out)
{
	P.core.Attach(&P.map, &P, &P.crng, &P.pending);
	P.core.BeginLevel();
	P.core.NewGame();
	fprintf(out, "MAP ncmd=%u geom=%u\n", (unsigned)P.map.commands.size(), P.L.geomSize);
	P.Snap();
	char line[256], a[64], b[64];
	while (fgets(line, sizeof line, in))
	{
		unsigned x = 0, y = 0; int z = 0; a[0] = b[0] = 0;
		if (sscanf(line, "%63s", a) != 1 || a[0] == '#') continue;
		const std::string cmd = a;
		if (cmd == "h")
		{
			sscanf(line, "%*s %63s %u", b, &x);
			if (x == 0 || x > P.map.commands.size()) { fprintf(out, "R h %s %u -> BAD\n", b, x); continue; }
			Command &rec = P.map.commands[x - 1];
			static const std::map<std::string, int> fn = { { "cmd_count", 0x15 }, { "cmd_texture_change_count", 0x1f },
				{ "cmd_count_addl_arg", 0x22 }, { "cmd_animate_facegroup_texture", 0x21 }, { "cmd_modify_count", 0x1e },
				{ "cmd_cycle_texture", 0x1c }, { "cmd_sync_facegroup_texture", 0x14 }, { "cmd_cycle_object_texture", 0x20 },
				{ "cmd_smash_face_texture", 0x2e }, { "cmd_player_rotation", 0x3f }, { "cmd_set_player_speed_reduction", 0x41 },
				{ "cmd_apply_damage", 0x33 }, { "cmd_face_emits_damage", 0x35 }, { "cmd_particle_effect", 0x2d } };
			int r = 0;
			if (!fn.count(b) || !P.core.Dispatch((uint8_t)fn.at(b), rec, r)) { fprintf(out, "R h %s %u -> BAD\n", b, x); continue; }
			fprintf(out, "R h %s %u -> %d\n", b, x, r == 0 ? 0 : (r & 1) ? -1 : 2);
		}
		else if (cmd == "chain") { sscanf(line, "%*s %u", &x); P.Chain((uint16_t)x); fprintf(out, "R chain %u\n", x); }
		else if (cmd == "drain")
		{
			// process_deferred_command_queue (raw_commands.c:1587-1620)
			std::vector<uint16_t> batch; batch.swap(P.queue);
			for (uint16_t c : batch) { P.core.sourceSector = P.core.sourceIndex = -1; P.core.activeFace = P.core.activeTexmap = -1; P.Chain(c); }
			fprintf(out, "R drain\n");
		}
		else if (cmd == "call0") { sscanf(line, "%*s %63s", b); if (!strcmp(b, "tick_particles")) P.core.TickParticles(); fprintf(out, "R call0 %s\n", b); }
		else if (cmd == "callv") { sscanf(line, "%*s %63s %u", b, &x); if (!strcmp(b, "dbase100_bitmap_test_clear")) P.flags.erase((int)x); else if (!strcmp(b, "dbase100_bitmap_test_set")) P.flags.insert((int)x); fprintf(out, "R callv %s %u\n", b, x); }
		else if (cmd == "set8" || cmd == "set16" || cmd == "set32")
		{
			sscanf(line, "%*s %x %i", &x, &z);
			switch (x)
			{
			case 0x85324: P.core.frameTimeScale = z; break;
			case 0x8a104: P.core.stepMode = (uint32_t)z; break;
			case 0x8a108: P.core.stepDelta = (uint32_t)z; break;
			case 0x8a268: P.core.interrupt = (uint8_t)z; break;
			case 0x8a26a: P.core.damageScale = (uint8_t)z; break;
			case 0x90a8e: P.px = (int16_t)(z + P.map.wrapShiftX); break;
			case 0x90a96: P.py = (int16_t)(z + P.map.wrapShiftY); break;
			case 0x90a8a: P.angle = (uint16_t)z; break;
			case 0x8a0f0: P.core.health = z; break;
			case 0x8a138: P.lastResult = z; break;
			case 0x89f3b: P.core.damageFlash = z; break;
			case 0x89f54: P.core.speedRequest = (uint32_t)z; break;
			case 0x71f48: P.crng = (uint32_t)z; break;
			case 0x7276c: P.core.particleRng = (uint16_t)z; break;
			case 0x89f60: P.autoSel = z != 0; break;
			default: fprintf(out, "!! set of unknown address %x\n", x);
			}
		}
		else if (cmd == "rec8") { sscanf(line, "%*s %u %u %i", &x, &y, &z); if (x && x <= P.map.commands.size()) { Command &c = P.map.commands[x - 1]; if (y == 2) { c.modifier = (uint8_t)z; c.disabled = (z & 8) != 0; } else c.SetByte((int)y, (uint8_t)z); } }
		else if (cmd == "rec16") { sscanf(line, "%*s %u %u %i", &x, &y, &z); if (x && x <= P.map.commands.size()) P.map.commands[x - 1].SetWord((int)y, (uint16_t)z); }
		else if (cmd == "pend") { sscanf(line, "%*s %u", &x); P.pending = (uint16_t)x; }
		else if (cmd == "src") { sscanf(line, "%*s %u %u", &x, &y); if (x == 65535) P.core.sourceSector = P.core.sourceIndex = -1; else { P.core.sourceSector = (int)x; P.core.sourceIndex = (int)y; } }
		else if (cmd == "face")
		{
			sscanf(line, "%*s %u", &x);
			P.core.activeFace = P.core.activeTexmap = -1;
			if (x != 0) for (size_t i = 0; i < P.L.faceOff.size(); i++) if (P.L.faceOff[i] == x) { P.core.activeFace = (int)i; P.core.activeTexmap = P.map.faces[i].textureMap; }
		}
		else if (cmd == "clearq") P.queue.clear();
		else if (cmd == "snap") P.Snap();
		else if (cmd == "diff") { sscanf(line, "%*s %63s", b); P.Diff(b, out); }
		else if (cmd == "parts")
		{
			for (int i = 0; i < CmdCore::PARTICLES; i++)
			{
				const CmdParticle &p = P.core.particles[i];
				if (!p.busy) continue;
				fprintf(out, "P %d sec=%u x=%d y=%d tex=%04x rot=%u fl=%02x lt=%02x rt=%02x z=%d zv=%d xv=%d yv=%d xa=%d ya=%d za=%d st=%u life=%u\n", i,
					p.sector >= 0 ? P.L.sectorOff[p.sector] : 0u, (int16_t)(p.x - P.map.wrapShiftX), (int16_t)(p.y - P.map.wrapShiftY), p.texture, p.rotation, p.flags, p.light, p.renderType, p.z,
					p.zvel, p.xvel, p.yvel, (int32_t)((uint32_t)p.xacc - ((uint32_t)P.map.wrapShiftX << 16)), (int32_t)((uint32_t)p.yacc - ((uint32_t)P.map.wrapShiftY << 16)), p.zacc, p.isStatic, p.life);
			}
			fprintf(out, "P end\n");
		}
		else if (cmd == "clearparts") { for (CmdParticle &p : P.core.particles) p.busy = false; }
		else if (cmd == "dump16")
		{
			sscanf(line, "%*s %x %u", &x, &y);
			fprintf(out, "W %x", x);
			for (unsigned i = 0; i < y; i++) fprintf(out, " %d", x == 0x72080 ? (int)CmdCore::Sine512(i) : 0);
			fprintf(out, "\n");
		}
		else if (cmd == "flag") { sscanf(line, "%*s %u", &x); fprintf(out, "F %u %u\n", x, (unsigned)P.flags.count((int)x)); }
		else fprintf(out, "!! bad line %s", line);
	}
	fprintf(out, "END\n");
}

int main(int argc, char **argv)
{
	if (argc < 5) { printf("usage: cmdcore_test gen|run <retail DATA folder> <MAP> <script> [out]\n"); return 2; }
	if (!TheInstall().Open(argv[2])) { printf("install: bad\n"); return 2; }
	Port P;
	P.file = ReadFile(TheInstall().MapFile(argv[3]));
	if (P.file.empty()) { printf("no such map %s\n", argv[3]); return 2; }
	P.map = ParseRaw(P.file.data(), P.file.size());
	if (!P.map.ok()) { printf("parse failed: %s\n", P.map.error.c_str()); return 2; }
	P.L.Build(P.map);
	if (!strcmp(argv[1], "gen"))
	{
		FILE *o = fopen(argv[4], "w"); if (!o) return 2;
		Gen(P.map, P.L, o); fclose(o); return 0;
	}
	FILE *in = fopen(argv[4], "r"); if (!in) { printf("no script\n"); return 2; }
	FILE *out = argc > 5 ? fopen(argv[5], "w") : stdout;
	Run(P, in, out);
	return 0;
}
