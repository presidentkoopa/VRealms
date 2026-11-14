// move_test: replay a moveprobe log of the ORIGINAL (tools/move_test/oracle) through the
// engine-free port (src/roth/roth_playermove) and diff every tick and frame.
//
//   move_test MAP.RAW oracle.log [-v] [-liveimage]
//
// What it does, in order:
//   1. the tables the movement reads (jump, keyboard directions, walk bob, sine) against the
//      running original's (TAB lines);
//   2. the image: es / gs / ob built from the RAW file by PlayerMove::BuildImage, byte for byte
//      against the running original's memory (ES / GS / OB dumps), counting differences by field;
//   3. the run: the port starts from the INIT state, gets the live heights the engine would
//      give it (sector +0/+2, platform +2/+8, the door bit), then replays every OP, TK and FR
//      line in order and compares its state with the original's after each one.
// -liveimage runs on the original's own es/ob instead of the built ones (isolates the rules).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roth_playermove.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>

using namespace roth;

namespace
{
std::vector<uint8_t> ReadFile(const char *p)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(p, "rb");
	if (!f) return b;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	b.resize((size_t)n);
	if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
	fclose(f);
	return b;
}

struct Host : PlayerMoveHost
{
	PlayerMove *pm = nullptr;
	int32_t flash = 0;
	int damages = 0, sounds = 0, contacts = 0;
	// apply_direct_damage_to_player (weapon_combat.c:29-43)
	void DamageDirect(uint32_t dmg) override
	{
		damages++;
		uint32_t eax = dmg + 0x20;
		if ((int32_t)eax < 0x35c6 && (int32_t)eax >= flash)
		{
			eax >>= 1;
			uint32_t pain = (uint32_t)flash + eax;
			flash = (int32_t)pain;
			if (pain >= 0x164) flash = 0x164;
		}
		uint32_t h = (uint32_t)pm->health - dmg;
		pm->health = (int32_t)h;
		if ((int32_t)h < 0) pm->health = 0;
	}
	void LandingSound(uint32_t) override { sounds++; }
	void ContactTrigger(uint32_t) override { contacts++; }
};

std::string State(const PlayerMove &p, const Host &h)
{
	char b[512];
	snprintf(b, sizeof b, "x=%08x y=%08x z=%08x a=%04x vx=%08x vy=%08x rc=%04x sa=%08x ta=%04x ib=%04x ra=%u qs=%04x qa=%u qb=%u lf=%02x"
		" ab=%08x,%08x,%08x,%08x,%08x h=%04x cam=%04x hp=%08x sec=%04x pa=%08x bob=%08x fl=%08x",
		p.x, p.y, p.z, p.angle, (uint32_t)p.velX, (uint32_t)p.velY, p.rateCounter, (uint32_t)p.speedAccum, p.turnAccum, p.inputBits,
		(uint32_t)p.runActive, p.queueSelect, p.queueA.count, p.queueB.count, p.locoFlags,
		(uint32_t)p.jumpPhase, (uint32_t)p.fallState, (uint32_t)p.landPending, (uint32_t)p.crouchMode, (uint32_t)p.fallAccum,
		p.height, p.camHeight, (uint32_t)p.health, p.sector, (uint32_t)p.pitchApplied, p.bobPhase, (uint32_t)h.flash);
	return b;
}
// "key=hex ..." -> map
std::map<std::string, std::string> Fields(const std::string &s)
{
	std::map<std::string, std::string> m;
	std::istringstream is(s);
	std::string tok;
	while (is >> tok) { size_t e = tok.find('='); if (e != std::string::npos) m[tok.substr(0, e)] = tok.substr(e + 1); }
	return m;
}
uint32_t H(const std::map<std::string, std::string> &m, const char *k) { auto it = m.find(k); return it == m.end() ? 0 : (uint32_t)strtoul(it->second.c_str(), 0, 16); }

std::vector<uint8_t> Hex(const std::vector<std::string> &rows)
{
	std::vector<uint8_t> b;
	for (const std::string &r : rows)
		for (size_t i = 0; i + 1 < r.size(); i += 2) b.push_back((uint8_t)strtoul(r.substr(i, 2).c_str(), 0, 16));
	return b;
}
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: move_test MAP.RAW oracle.log [-v] [-liveimage]\n"); return 2; }
	bool verbose = false, liveImage = false;
	for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "-v")) verbose = true; if (!strcmp(argv[i], "-liveimage")) liveImage = true; }
	const std::vector<uint8_t> raw = ReadFile(argv[1]);
	std::ifstream lf(argv[2]);
	if (raw.empty() || !lf) { fprintf(stderr, "cannot read inputs\n"); return 2; }

	std::vector<std::string> lines;
	std::map<std::string, std::vector<std::string>> dumps;
	{
		std::string ln;
		while (std::getline(lf, ln))
		{
			// hex dumps: "ES n" then "ES+ off hex"
			size_t sp = ln.find(' ');
			std::string tag = ln.substr(0, sp);
			if (tag == "ES+" || tag == "GS+" || tag == "OB+" || tag == "DIM+")
			{
				std::istringstream is(ln); std::string t, off, hex; is >> t >> off >> hex;
				dumps[tag.substr(0, tag.size() - 1)].push_back(hex);
				continue;
			}
			lines.push_back(ln);
		}
	}
	PlayerMove pm;
	Host host; host.pm = &pm; pm.host = &host;
	int fails = 0;

	// ---- 1. tables ----
	{
		int diff = 0, n = 0;
		for (const std::string &l : lines)
		{
			if (l.compare(0, 4, "TAB ") != 0) continue;
			std::istringstream is(l.substr(4)); std::string name; is >> name;
			std::vector<std::string> v; std::string t; while (is >> t) v.push_back(t);
			const MoveTables &T = MoveTables::Original();
			if (name == "jump") for (size_t i = 0; i < v.size() && i < 0x15; i++) { n++; diff += atoi(v[i].c_str()) != T.jump[i]; }
			if (name == "dir")  for (size_t i = 0; i < v.size() && i < 16; i++) { n++; diff += strtoul(v[i].c_str(), 0, 16) != T.dir[i]; }
			if (name == "bob")  for (size_t i = 0; i < v.size() && i < 32; i++) { n++; diff += atoi(v[i].c_str()) != T.bob[i]; }
			if (name == "sin")  for (size_t i = 0; i < v.size() && i < 512; i++) { n++; diff += (uint16_t)strtoul(v[i].c_str(), 0, 16) != (uint16_t)ActorSin((uint32_t)i * 2); }
			if (name == "keys")
				for (size_t i = 0; i < v.size() && i < MK_COUNT; i++) { n++; diff += (uint8_t)strtoul(v[i].c_str(), 0, 16) != kMoveKeyScancode[i]; }
		}
		printf("tables: %d values compared, %d differ\n", n, diff);
		if (diff) fails++;
	}

	// ---- 2. the image ----
	if (!pm.BuildImage(raw.data(), raw.size())) { printf("BuildImage failed\n"); return 1; }
	pm.InitTuning();
	const std::vector<uint8_t> oES = Hex(dumps["ES"]), oGS = Hex(dumps["GS"]), oOB = Hex(dumps["OB"]), oDIM = Hex(dumps["DIM"]);
	{
		// es by field: sectors (0x1a) / faces (0xc) / the rest
		const uint32_t secFirst = pm.SectorOffset(0), secEnd = secFirst + pm.SectorCount() * 0x1a;
		std::map<std::string, int> byField;
		int diff = 0;
		for (size_t i = 0; i < oES.size() && i < pm.es.size(); i++)
		{
			if (oES[i] == pm.es[i]) continue;
			diff++;
			char k[64];
			if (i >= secFirst && i < secEnd) snprintf(k, sizeof k, "sector+%02x", (unsigned)((i - secFirst) % 0x1a));
			else snprintf(k, sizeof k, "other");
			byField[k]++;
		}
		printf("image es: %zu bytes (original %zu), %d differ", pm.es.size(), oES.size(), diff);
		for (auto &kv : byField) printf("  %s:%d", kv.first.c_str(), kv.second);
		printf("\n");
		int gd = 0; for (size_t i = 0; i < oGS.size() && i < pm.gs.size(); i++) gd += oGS[i] != pm.gs[i];
		printf("image gs: %zu bytes (original %zu), %d differ\n", pm.gs.size(), oGS.size(), gd);
		int od = 0; for (size_t i = 0; i < oOB.size() && i < pm.ob.size(); i++) od += oOB[i] != pm.ob[i];
		printf("image ob: %zu bytes (original %zu), %d differ\n", pm.ob.size(), oOB.size(), od);
	}
	// the live parts the engine supplies: sector and platform heights, the door bit, the objects
	for (int s = 0; s < pm.SectorCount(); s++)
	{
		const uint32_t o = pm.SectorOffset(s);
		if (o + 0x1a > oES.size()) break;
		auto W = [&](uint32_t a) { return (int16_t)(oES[a] | (oES[a + 1] << 8)); };
		pm.SetSectorHeights(s, W(o), W(o + 2));
		const uint16_t l = (uint16_t)(oES[o + 0x18] | (oES[o + 0x19] << 8));
		if (l != 0 && l + 10u <= oES.size()) pm.SetPlatformHeights(s, W(l + 2), W(l + 8));
		pm.SetSectorDoorFlag(s, (oES[o + 0x16] & 1) != 0);
		pm.SetSectorTextureOverride(s, (int8_t)oES[o + 0xc]);
	}
	{
		// THE BYTES THE COLLISION READS (roth_playermove.cpp): sector +0 +2 +0xc +0xd +0xe +0x14 +0x18 and
		// bits 0-1 of +0x16; face +0..+9 and bits 0x81 of +0xa; a face's mapping record bit 0x08 of +8;
		// platform +2 +8; vertex +8..+0xb. Everything else in these images is the renderer's or the
		// scripts' and nothing in the movement reads it.
		auto R16 = [](const std::vector<uint8_t> &b, uint32_t o) { return (uint16_t)(o + 1 < b.size() ? b[o] | (b[o + 1] << 8) : 0); };
		int n = 0, diff = 0; std::map<std::string, int> byField;
		auto cmp = [&](const char *what, uint32_t a, uint8_t mask, const std::vector<uint8_t> &o, const std::vector<uint8_t> &p)
		{
			n++;
			if (a < o.size() && a < p.size() && ((o[a] ^ p[a]) & mask)) { diff++; byField[what]++; }
		};
		const uint32_t faces = R16(pm.es, 6);
		for (int s = 0; s < pm.SectorCount(); s++)
		{
			const uint32_t o = pm.SectorOffset(s);
			const int k[] = { 0, 1, 2, 3, 0xc, 0xd, 0xe, 0xf, 0x14, 0x15, 0x18, 0x19 };
			for (int i : k) { char nm[32]; snprintf(nm, sizeof nm, "sector+%02x", i); cmp(nm, o + i, 0xff, oES, pm.es); }
			cmp("sector+16&3", o + 0x16, 3, oES, pm.es);
			const uint16_t l = R16(pm.es, o + 0x18);
			if (l) { cmp("platform+2", l + 2, 0xff, oES, pm.es); cmp("platform+3", l + 3, 0xff, oES, pm.es); cmp("platform+8", l + 8, 0xff, oES, pm.es); cmp("platform+9", l + 9, 0xff, oES, pm.es); }
			const uint16_t w0 = R16(pm.es, o + 0xe);
			for (int w = 0; w < pm.es[o + 0xd]; w++)
			{
				const uint32_t f = (uint16_t)(w0 + w * 0xc);
				for (int i = 0; i < 0xa; i++) cmp("face+0..9", f + i, 0xff, oES, pm.es);
				cmp("face+a&81", f + 0xa, 0x81, oES, pm.es);
				cmp("mapping+8&8", (uint16_t)(R16(pm.es, f + 4) + 8), 8, oES, pm.es);
				for (int vi = 0; vi < 2; vi++) { const uint16_t v = R16(pm.es, f + vi * 2); for (int i = 8; i < 12; i++) cmp("vertex+8..b", (uint16_t)(v + i), 0xff, oGS, pm.gs); }
			}
		}
		(void)faces;
		printf("image, the bytes the collision reads, with the live heights, door bits and +0xc from the engine: %d compared, %d differ", n, diff);
		for (auto &kv : byField) printf("  %s:%d", kv.first.c_str(), kv.second);
		printf("\n");
		if (diff) fails++;
	}
	if (liveImage) { pm.es = oES; pm.ob = oOB; }
	else pm.ob = oOB;     // the objects are the host's live records (the actors port owns them)
	pm.dims = oDIM;

	// ---- 3. the run ----
	bool started = false;
	uint32_t held = 0; bool shift = false;
	int ticks = 0, frames = 0, tickDiff = 0, frameDiff = 0, shown = 0;
	std::map<std::string, int> fieldDiffs;
	for (const std::string &l : lines)
	{
		if (l.compare(0, 5, "INIT ") == 0)
		{
			auto m = Fields(l);
			pm.movementMode = (uint8_t)H(m, "mode"); pm.cursorType = (uint8_t)H(m, "cur"); pm.mouseRelative = (uint8_t)H(m, "rel");
			pm.runToggle = H(m, "rt"); pm.moveSpeed = (uint16_t)H(m, "msi"); pm.restHeight = (int32_t)H(m, "rest");
			pm.maxClimb = (uint16_t)H(m, "climb"); pm.minFit = (uint16_t)H(m, "fit"); pm.speedShift = H(m, "shift");
			pm.viewClearance = (int32_t)H(m, "clear"); pm.viewPitch = (int32_t)H(m, "vpitch");
			pm.dialogueBusy = H(m, "busy"); pm.freezeGate = H(m, "gate"); pm.landingSound = (uint16_t)H(m, "thud");
			if (H(m, "doors") != 0) printf("NOTE: %u doors in the original's pool at start; the test gives the port none\n", H(m, "doors"));
			if (H(m, "pool") != 0) printf("NOTE: %u creatures in the pool at start; the test gives the port none\n", H(m, "pool"));
			continue;
		}
		if (l.compare(0, 6, "INITQ ") == 0)
		{
			std::istringstream is(l.substr(6)); std::string q;
			for (int k = 0; k < 2 && (is >> q); k++)
			{
				PlayerMove::Queue &Q = k ? pm.queueB : pm.queueA;
				Q.count = (uint16_t)atoi(q.c_str());
				const char *p = strchr(q.c_str(), ':') + 1;
				for (int i = 0; i < 16; i++) { Q.d[i][0] = (int32_t)strtoul(p, (char **)&p, 16); p++; Q.d[i][1] = (int32_t)strtoul(p, (char **)&p, 16); p++; }
			}
			continue;
		}
		if (l.compare(0, 6, "INITS ") == 0)
		{
			auto m = Fields(l);
			pm.x = H(m, "x"); pm.y = H(m, "y"); pm.z = H(m, "z"); pm.angle = (uint16_t)H(m, "a");
			pm.velX = (int32_t)H(m, "vx"); pm.velY = (int32_t)H(m, "vy"); pm.rateCounter = (uint16_t)H(m, "rc");
			pm.speedAccum = (int32_t)H(m, "sa"); pm.turnAccum = (uint16_t)H(m, "ta"); pm.inputBits = (uint16_t)H(m, "ib");
			pm.runActive = (int32_t)H(m, "ra"); pm.queueSelect = (uint16_t)H(m, "qs"); pm.locoFlags = (uint8_t)H(m, "lf");
			{
				unsigned a[5] = {}; sscanf(m["ab"].c_str(), "%x,%x,%x,%x,%x", &a[0], &a[1], &a[2], &a[3], &a[4]);
				pm.jumpPhase = (int32_t)a[0]; pm.fallState = (int32_t)a[1]; pm.landPending = (int32_t)a[2]; pm.crouchMode = (int32_t)a[3]; pm.fallAccum = (int32_t)a[4];
			}
			pm.height = (uint16_t)H(m, "h"); pm.camHeight = (uint16_t)H(m, "cam"); pm.health = (int32_t)H(m, "hp");
			pm.sector = (uint16_t)H(m, "sec"); pm.pitchApplied = (int32_t)H(m, "pa"); pm.bobPhase = H(m, "bob"); host.flash = (int32_t)H(m, "fl");
			pm.sectorCache = pm.sector;
			started = true;
			continue;
		}
		if (!started) continue;
		if (l.compare(0, 3, "OP ") == 0)
		{
			std::istringstream is(l.substr(3)); int t; std::string op, a1, a2; is >> t >> op;
			std::string rest; std::getline(is, rest);
			if (op == "keys")
			{
				held = 0; shift = false;
				if (rest.find('-') == std::string::npos)
				{
					const char *p = rest.c_str();
					while (*p) { while (*p == ' ' || *p == ',') p++; if (!*p) break; const unsigned sc = strtoul(p, (char **)&p, 16);
						if (sc == 0x36) shift = true;
						for (int k = 0; k < MK_COUNT; k++) if (kMoveKeyScancode[k] == sc) held |= 1u << k; }
				}
			}
			else if (op == "w8" || op == "w16" || op == "w32")
			{
				unsigned a = 0, v = 0; sscanf(rest.c_str(), "%x %x", &a, &v);
				switch (a)
				{
				case 0x7e8dc: pm.runToggle = v; break;
				case 0x83aea: pm.dialogueBusy = v; break;
				case 0x83125: pm.freezeGate = v; break;
				case 0x89f58: pm.speedShift = v; break;
				case 0x8a0f0: pm.health = (int32_t)v; break;
				case 0x12570: pm.moveSpeed = (uint16_t)v; break;
				case 0x89f54: break;
				case 0x7f560: break;   // the dev flag: the original's door opener reads it, not the movement   // the 0x41 request: the latched shift comes from each TK line (sh=)
				default: printf("NOTE: poke %x not modelled\n", a);
				}
			}
			else if (op == "pin" || op == "unpin") { }   // the dialogue freeze: read from each TK line
			else if (op == "sech")
			{
				int i, c, f; if (sscanf(rest.c_str(), "%d %d %d", &i, &c, &f) == 3) pm.SetSectorHeights(i, (int16_t)c, (int16_t)f);
			}
			else if (op == "pose")
			{
				int x, y, a, sc, z; if (sscanf(rest.c_str(), "%d %d %d %d %d", &x, &y, &a, &sc, &z) == 5)
				{ pm.x = (uint32_t)x << 16; pm.y = (uint32_t)y << 16; pm.z = (uint32_t)z << 16; pm.angle = (uint16_t)a; pm.sector = (uint16_t)sc; }
			}
			continue;
		}
		if (l.compare(0, 6, "DOORS ") == 0)
		{
			// the door pool before this frame: the doors are the renderer's to swing (doors.c), inputs here
			pm.doors.clear();
			std::istringstream is(l.substr(6)); std::string f, d; is >> f;
			while (is >> d)
			{
				MoveDoor md; int v[9] = {}; unsigned key = 0;
				sscanf(d.c_str(), "%x,%d,%d,%d,%d,%d,%d,%d,%d", &key, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
				md.sector = (uint16_t)key;
				for (int k = 0; k < 4; k++) { md.x[k] = (int16_t)v[k * 2]; md.y[k] = (int16_t)v[k * 2 + 1]; }
				pm.doors.push_back(md);
			}
			continue;
		}
		if (l.compare(0, 2, "DF") == 0)
		{
			std::istringstream is(l.substr(2)); int i; while (is >> i) pm.SetSectorDoorFlag(i, true);
			continue;
		}
		const bool isTk = l.compare(0, 3, "TK ") == 0, isFr = l.compare(0, 3, "FR ") == 0;
		if (!isTk && !isFr) continue;
		auto m = Fields(l);
		if (isTk)
		{
			// 0x89f58 is latched from the 0x41 request by tick_world_effects (raw_commands.c:3290): an input here
			if (m.count("sh")) pm.speedShift = H(m, "sh");
			if (m.count("bz")) { pm.dialogueBusy = H(m, "bz"); pm.freezeGate = H(m, "gt"); }   // the dialogue's, inputs here
			MoveInput in; in.held = held; in.shift = shift; pm.Tick(in); ticks++;
		}
		else
		{
			// 0x8497c is the renderer's (compute_floor_clearance_for_render, R:7036): an input here
			if (m.count("clr")) pm.viewClearance = (int32_t)H(m, "clr");
			pm.Frame((int)strtoul(m["fts"].c_str(), 0, 10)); frames++;
		}
		// the original's line, without the frame's extra fields
		std::string want;
		{
			size_t p = l.find(" x=");
			want = l.substr(p + 1);
		}
		// The pain flash is the frame loop's (it decays before the movement runs, game_core.c:481-487):
		// not compared; the fall damage is, through the health. The host follows the original's value.
		host.flash = (int32_t)H(m, "fl");
		const std::string got = State(pm, host);
		if (got != want)
		{
			if (isTk) tickDiff++; else frameDiff++;
			auto gm = Fields(got), wm = Fields(want);
			for (auto &kv : wm) if (gm[kv.first] != kv.second) fieldDiffs[kv.first]++;
			if (shown < (verbose ? 40 : 5))
			{
				shown++;
				printf("DIFF %s\n  orig %s\n  port %s\n", l.substr(0, l.find(" x=")).c_str(), want.c_str(), got.c_str());
			}
		}
		// update_view_transform_params runs after the probe logged the frame (update_player_movement, PL:410-425)
		if (isFr) pm.ClampViewPitch();
	}
	printf("run: %d ticks, %d frames compared; %d ticks and %d frames differ", ticks, frames, tickDiff, frameDiff);
	for (auto &kv : fieldDiffs) printf("  %s:%d", kv.first.c_str(), kv.second);
	printf("\nexercised: %u wall push-outs, %u endpoint slides, %u portal passes, %u object push-outs, %u object height windows, %u door clips (%u pushing), %u sector changes",
		pm.stats.wallPush, pm.stats.wallSlide, pm.stats.portalRecurse, pm.stats.objectPush, pm.stats.objectZNarrow, pm.stats.doorClip, pm.stats.doorPush, pm.stats.sectorChange);
	printf("\nhost: %d fall damages, %d landing sounds, %d contact triggers\n", host.damages, host.sounds, host.contacts);
	if (tickDiff || frameDiff) fails++;
	return fails ? 1 : 0;
}
