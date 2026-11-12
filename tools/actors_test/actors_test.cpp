//
// actors_test -- runs roth_actors (the port) through a script and prints its
// state in the format tools/actors_test/actprobe.c prints the ORIGINAL's, so
// the two logs can be compared with diff.
//
//   actors_test <DATA dir> <MAP> <script> [oracle.log]
//
// The oracle log, when given, supplies the inputs that are not rules: the
// player's position (its ENV lines) -- so both sides are fed the same numbers.
//
// Script lines are "<tick> <op> [args]", the probe's own format:
//   dump LABEL | glb LABEL | cmd INDEX | src SECTOR INDEX | wake SECTOR INDEX |
//   w32 VA VALUE (a few known addresses) | tick N | trig (table self-check)
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
#include "roth_actors.h"
#include "roth_raw.h"
#include "roth_dbase100.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <strings.h>
#include <dirent.h>

using namespace roth;

static std::vector<uint8_t> Slurp(const std::string &path)
{
	std::vector<uint8_t> v;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return v;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	v.resize((size_t)n);
	if (n > 0 && fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear();
	fclose(f);
	return v;
}

// Case-insensitive file lookup inside a directory.
static std::string FindFile(const std::string &dir, const std::string &name)
{
	DIR *d = opendir(dir.c_str());
	if (!d) return "";
	std::string out;
	while (dirent *e = readdir(d))
		if (!strcasecmp(e->d_name, name.c_str())) { out = dir + "/" + e->d_name; break; }
	closedir(d);
	return out;
}

struct TestHost : ActorHost
{
	void SoundAt(int sound, int x, int y) override { printf("SND %d at %d,%d\n", sound, (int16_t)x, (int16_t)y); }
	void RunObjectChain(ObjectUid) override { printf("CHAIN\n"); }
	int selected = 0;
	int SelectedItem() override { return selected; }
};

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: actors_test DATA MAP script [oracle.log]\n"); return 2; }
	const std::string data = argv[1], mapName = argv[2];

	std::vector<uint8_t> raw = Slurp(FindFile(data + "/M", mapName + ".RAW"));
	if (raw.empty()) { fprintf(stderr, "no map %s\n", mapName.c_str()); return 1; }
	Map map = ParseRaw(raw.data(), raw.size());
	if (!map.ok()) { fprintf(stderr, "map: %s\n", map.error.c_str()); return 1; }

	Dbase100 db;
	{
		std::vector<uint8_t> d = Slurp(FindFile(data, "DBASE100.DAT"));
		if (d.empty() || !db.Load(std::move(d))) { fprintf(stderr, "DBASE100: %s\n", db.Error().c_str()); return 1; }
	}
	std::vector<uint8_t> ademo = Slurp(FindFile(data + "/M", "ADEMO.DAS"));
	CreatureTables tables = LoadCreatureTables(ademo.data(), ademo.size());
	if (!tables.ok) { fprintf(stderr, "ADEMO: %s\n", tables.error.c_str()); return 1; }

	// --gen: write the command script for this map -- every 0x16 / 0x3c / 0x3a
	// record in index order, each with a known command source, each followed
	// by a full dump.
	if (!strcmp(argv[3], "--gen") || !strcmp(argv[3], "--census"))
	{
		int n16 = 0, n3c = 0, n3a = 0, creatures = 0, fs = -1;
		for (size_t s = 0; s < map.objects.size(); s++)
		{
			if (fs < 0 && !map.objects[s].empty()) fs = (int)s;
			for (const Object &o : map.objects[s])
				if (tables.CreatureIndexForId((uint16_t)(o.textureIndex | (o.textureSource << 8))) >= 0) creatures++;
		}
		const bool gen = !strcmp(argv[3], "--gen");
		if (gen) printf("1 w8 8a268 0\n1 dump initial\n1 sync initial\n");
		for (size_t i = 0; i < map.commands.size(); i++)
		{
			const uint8_t o = map.commands[i].opcode & 0x7f;
			if (o == 0x33 && gen)
			{
				printf("1 w32 8a0f0 800\n1 src %d 0\n1 cmd %d\n1 dump after%d\n1 w32 8a0f0 800\n", fs, (int)i + 1, (int)i + 1);
				continue;
			}
			if (o == 0x33 || o == 0x35)
			{
				if (!gen) printf("DMGREC %s %zu op=%02x mod=%02x f6=%02x dmg=%u key=%04x rad=%u\n", mapName.c_str(), i + 1, o,
					map.commands[i].modifier, map.commands[i].Byte(6), map.commands[i].Byte(7), map.commands[i].key, map.commands[i].Word(0x0a));
				continue;
			}
			if (o != 0x16 && o != 0x3c && o != 0x3a) continue;
			(o == 0x16 ? n16 : o == 0x3c ? n3c : n3a)++;
			if (!gen) continue;
			if (o == 0x3a || map.commands[i].key == 0) printf("1 src %d 0\n", fs); else printf("1 src -1\n");
			printf("1 cmd %d\n1 dump after%d\n", (int)i + 1, (int)i + 1);
		}
		if (gen) printf("2 quit\n");
		else printf("%-9s 0x16=%3d 0x3c=%3d 0x3a=%3d creatures=%3d start=%d,%d\n", mapName.c_str(), n16, n3c, n3a, creatures, map.metadata.startX, map.metadata.startY);
		return 0;
	}

	TestHost host;
	ActorWorld w;
	w.Begin(&map, &db, &tables, &host);

	// Player inputs from the oracle's log.
	if (argc > 4)
	{
		FILE *f = fopen(argv[4], "r");
		char ln[600];
		while (f && fgets(ln, sizeof ln, f))
		{
			int px, py, pz, pa, ph; unsigned psec;
			const char *p = strstr(ln, "sel=");
			if (!strncmp(ln, "ENV", 3) && p) host.selected = atoi(p + 4);
			p = strstr(ln, "px=");
			if (!strncmp(ln, "ENV", 3) && p && sscanf(p, "px=%d py=%d pz=%d pa=%d ph=%d psec=%x", &px, &py, &pz, &pa, &ph, &psec) == 6)
			{
				w.player.x = (int16_t)px; w.player.y = (int16_t)py; w.player.z = (int16_t)pz;
				w.player.angle = (uint16_t)pa; w.player.height = (uint16_t)ph;
				w.player.sector = psec ? (int)((psec - map.header.sectorsOffset) / 0x1a) : -1;
				break;
			}
		}
		if (f) fclose(f);
	}

	// Load the original's state from the block of its log headed "== label":
	// objects, actor slots, flying slots, generators. Used to start the port
	// from exactly where the original stands, so that what follows tests the
	// rules being replayed and nothing that happened before them.
	auto sync = [&](const char *label)
	{
		if (argc <= 4) { printf("SYNC needs the oracle log\n"); return; }
		FILE *f = fopen(argv[4], "r");
		char l[600];
		bool in = false;
		std::string want = std::string("== ") + label;
		while (f && fgets(l, sizeof l, f))
		{
			l[strcspn(l, "\n")] = 0;
			if (!strncmp(l, "== ", 3)) { if (in) break; in = (want == l); continue; }
			if (in && (!strncmp(l, "TICK ", 5) || !strncmp(l, "FRAME", 5) || !strncmp(l, "CMD ", 4))) break;
			if (!in) continue;
			if (!strncmp(l, "GLB", 3))
			{
				unsigned rd, ra, rs1, rs2, na, np; int sched, hp, mhp, arm, fl;
				sscanf(l, "GLB hp=%d maxhp=%d arm=%d flash=%d rngd=%x rnga=%x rngs=%x,%x sched=%d", &hp, &mhp, &arm, &fl, &rd, &ra, &rs1, &rs2, &sched);
				const char *p = strstr(l, "na="); sscanf(p, "na=%u np=%u", &na, &np);
				host.mHealth = hp; host.mMaxHealth = mhp; host.mArmour = arm; host.mFlash = fl;
				w.rngDamage = (uint16_t)rd; w.rngWander = ra; w.rngAnim = rs1; w.rngAnimSeed = rs2; w.animScheduler = sched;
				w.actorCount = (int)na; w.flyingCount = (int)np;
			}
			else if (!strncmp(l, "OBJ", 3))
			{
				int s, i, x, y, z; unsigned id, rot, f7, lt, f9, c, cmd;
				sscanf(l, "OBJ %d.%d x=%d y=%d id=%x rot=%x f7=%x lt=%x f9=%x z=%d c=%x cmd=%x", &s, &i, &x, &y, &id, &rot, &f7, &lt, &f9, &z, &c, &cmd);
				if ((size_t)s >= map.objects.size() || (size_t)i >= map.objects[s].size()) { printf("SYNC object %d.%d missing\n", s, i); continue; }
				Object &o = map.objects[s][i];
				o.x = (int16_t)x; o.y = (int16_t)y; o.textureIndex = (uint8_t)id; o.textureSource = (uint8_t)(id >> 8);
				o.rotation = (uint8_t)rot; o.flags = (uint8_t)f7; o.light = (uint8_t)lt; o.renderType = (uint8_t)f9;
				o.z = (int16_t)z; o.unk0C = (uint16_t)c; o.commandID = (uint16_t)cmd;
			}
			else if (!strncmp(l, "ACT", 3))
			{
				int i, mon, s, oi, hp; unsigned def, st, f9, fa, fb, lx, ly, lz, sec, vel, ctr, proj, dly, acc, atk, pain;
				char ob[32];
				sscanf(l, "ACT %d mon=%d def=%x obj=%31s st=%x f9=%x fa=%x fb=%x hp=%d lo=%x,%x,%x sec=%x vel=%x ctr=%x proj=%x dly=%x acc=%x atk=%x pain=%x",
					&i, &mon, &def, ob, &st, &f9, &fa, &fb, &hp, &lx, &ly, &lz, &sec, &vel, &ctr, &proj, &dly, &acc, &atk, &pain);
				ActorSlot &a = w.actors[i];
				a.creature = mon;
				a.object = (sscanf(ob, "%d.%d", &s, &oi) == 2) ? w.UidAt(s, oi) : 0;
				a.state = (uint8_t)st; a.bits = (uint8_t)f9; a.move = (uint8_t)fa; a.settle = (uint8_t)fb; a.health = hp;
				a.fracX = (uint16_t)lx; a.fracY = (uint16_t)ly; a.fracZ = (uint16_t)lz;
				a.sector = sec ? (int)((sec - map.header.sectorsOffset) / 0x1a) : -1;
				a.fallSpeed = (uint16_t)vel; a.counter = (uint16_t)ctr; a.bullet = (uint16_t)proj; a.fireDelay = (uint8_t)dly;
				a.stepAccum = (uint8_t)acc; a.attackCount = (uint8_t)atk; a.pain = (uint8_t)pain;
				if (mon >= 0 && (size_t)mon < w.creatureRt.size() && def != 0)
				{ w.creatureRt[mon].def = w.LookupDef(def); w.creatureRt[mon].defId = (uint16_t)def; }
			}
			else if (!strncmp(l, "PRJ", 3))
			{
				int i, s, oi, zv; unsigned ang, sec, mode, wd, fx, fy, fz, type, dmg, spd, ttl, own;
				char ob[32];
				sscanf(l, "PRJ %d obj=%31s ang=%x sec=%x mode=%x w=%x fx=%x fy=%x fz=%x zv=%d type=%x dmg=%x spd=%x ttl=%x own=%x",
					&i, ob, &ang, &sec, &mode, &wd, &fx, &fy, &fz, &zv, &type, &dmg, &spd, &ttl, &own);
				FlyingSlot &p = w.flying[i];
				p.object = (sscanf(ob, "%d.%d", &s, &oi) == 2) ? w.UidAt(s, oi) : 0;
				p.angle = (uint16_t)ang; p.sector = sec ? (int)((sec - map.header.sectorsOffset) / 0x1a) : -1;
				p.mode = (uint8_t)mode; p.width = (uint8_t)wd; p.fracX = (uint16_t)fx; p.fracY = (uint16_t)fy; p.fracZ = (uint16_t)fz;
				p.zVelocity = zv; p.damageType = (uint16_t)type; p.damage = (uint16_t)dmg; p.speed = (uint8_t)spd; p.ttl = (uint8_t)ttl; p.owner = (uint16_t)own;
			}
		}
		if (f) fclose(f);
	};

	// REPLAY: run the port tick by tick beside the original's trace. Each
	// "TICK n" block of the trace is the original's state before its tick n;
	// the port prints its own state at the same point, and every collision
	// question the port asks during the tick is answered with the original's
	// own answer from the COL / SWP lines that follow the block -- after
	// checking that the port asked the same question.
	struct Replayer : ActorCollision
	{
		std::vector<std::string> col, swp;
		size_t ci = 0, si = 0;
		int qmis = 0, judged = 0, agreed = 0;
		ActorCollision *judge = nullptr;   // when set: compare its answer with the original's
		ActorWorld *w = nullptr;
		Map *map = nullptr;
		int SecIdx(unsigned off) const { return off ? (int)((off - map->header.sectorsOffset) / 0x1a) : -1; }
		CollideAnswer Collide(const CollideQuery &q) override
		{
			CollideAnswer a;
			if (ci >= col.size()) { printf("COLQ none left\n"); qmis++; a.x = q.toX; a.y = q.toY; a.located = true; a.sector = q.sector; return a; }
			const char *l = col[ci++].c_str();
			char self[32]; unsigned fx, fy, fz, tx, ty, sec, st, hg, rd, bd, loc, x, y, z, bx, by, hit, fzz, cz, sec2;
			sscanf(l, "COL self=%31s from=%x,%x,%x to=%x,%x sec=%x step=%x hgt=%x rad=%x bud=%x -> loc=%u x=%x y=%x z=%x back=%x,%x hit=%x fz=%x cz=%x sec=%x",
				self, &fx, &fy, &fz, &tx, &ty, &sec, &st, &hg, &rd, &bd, &loc, &x, &y, &z, &bx, &by, &hit, &fzz, &cz, &sec2);
			if (fx != q.fromX || fy != q.fromY || fz != q.fromZ || tx != q.toX || ty != q.toY || SecIdx(sec) != q.sector ||
				st != q.stepHeight || hg != q.height || rd != q.radius || bd != q.budget)
			{
				printf("COLQ MISMATCH want %s\n     got from=%08x,%08x,%08x to=%08x,%08x sec=%04x step=%04x hgt=%04x rad=%04x bud=%08x\n", l,
					q.fromX, q.fromY, q.fromZ, q.toX, q.toY, w->SectorOffset(q.sector), q.stepHeight, q.height, q.radius, q.budget);
				qmis++;
			}
			a.located = loc != 0; a.x = x; a.y = y; a.z = z; a.backX = bx; a.backY = by; a.hit = (uint8_t)hit;
			a.floorZ = (uint16_t)fzz; a.ceilZ = (uint16_t)cz; a.sector = SecIdx(sec2);
			if (judge)
			{
				const CollideAnswer j = judge->Collide(q);
				judged++;
				if (j.x == a.x && j.y == a.y && (j.hit != 0) == (a.hit != 0) && j.floorZ == a.floorZ && j.sector == a.sector) agreed++;
				else printf("JUDGE COL orig x=%08x y=%08x hit=%u fz=%04x sec=%d  simple x=%08x y=%08x hit=%u fz=%04x sec=%d\n",
					a.x, a.y, a.hit, a.floorZ, a.sector, j.x, j.y, j.hit, j.floorZ, j.sector);
			}
			return a;
		}
		SweepAnswer Sweep(const SweepQuery &q) override
		{
			SweepAnswer a;
			if (si >= swp.size()) { printf("SWPQ none left\n"); qmis++; a.x = q.x; a.y = q.y; a.z = q.z; a.sector = q.sector; return a; }
			const char *l = swp[si++].c_str();
			char self[32], ho[32]; unsigned x, y, z, sec, spd, wd, own, ex, ey, ez, esec, hit; int vx, vy, vz;
			sscanf(l, "SWP self=%31s x=%x y=%x z=%x sec=%x vx=%d vy=%d vz=%d spd=%u w=%u own=%x -> x=%x y=%x z=%x sec=%x hit=%x ho=%31s",
				self, &x, &y, &z, &sec, &vx, &vy, &vz, &spd, &wd, &own, &ex, &ey, &ez, &esec, &hit, ho);
			if (x != q.x || y != q.y || z != q.z || SecIdx(sec) != q.sector || vx != q.velX || vy != q.velY || vz != q.velZ ||
				spd != q.speed || wd != q.width || own != q.owner)
			{
				printf("SWPQ MISMATCH want %s\n     got x=%08x y=%08x z=%08x sec=%04x vx=%d vy=%d vz=%d spd=%u w=%u own=%04x\n", l,
					q.x, q.y, q.z, w->SectorOffset(q.sector), q.velX, q.velY, q.velZ, q.speed, q.width, q.owner);
				qmis++;
			}
			a.x = ex; a.y = ey; a.z = ez; a.sector = SecIdx(esec); a.hit = (uint8_t)hit;
			int s, i;
			a.hitObject = (sscanf(ho, "%d.%d", &s, &i) == 2) ? w->UidAt(s, i) : 0;
			if (judge)
			{
				const SweepAnswer j = judge->Sweep(q);
				judged++;
				if (j.x == a.x && j.y == a.y && j.hit == a.hit && (!(a.hit & 2) || j.hitObject == a.hitObject)) agreed++;
				else printf("JUDGE SWP orig x=%08x y=%08x z=%08x hit=%u  simple x=%08x y=%08x z=%08x hit=%u\n", a.x, a.y, a.z, a.hit, j.x, j.y, j.z, j.hit);
			}
			return a;
		}
	};
	auto replay = [&](const char *startLabel)
	{
		std::string lab = startLabel;
		if (lab.size() > 7 && lab.compare(lab.size() - 7, 7, " simple") == 0) lab.resize(lab.size() - 7);
		sync(lab.c_str());
		Replayer rp; rp.w = &w; rp.map = &map;
		w.SetCollision(&rp);
		SimpleCollision simple(&w);
		rp.judge = strstr(startLabel, " simple") ? &simple : nullptr;
		FILE *f = fopen(argv[4], "r");
		char l[700];
		bool started = false;
		std::vector<std::string> pending;   // WAKE / FIRE lines before a TICK
		std::vector<std::string> block;
		int tick = -1;
		auto runTick = [&]()
		{
			if (tick < 0) return;
			// the original's state before this tick, in the port's terms
			printf("TICK %d\n", tick);
			fputs(w.DumpTick().c_str(), stdout);
			w.Tick();
		};
		while (f && fgets(l, sizeof l, f))
		{
			l[strcspn(l, "\n")] = 0;
			if (!started) { if (!strncmp(l, "== ", 3) && lab == l + 3) started = true; continue; }
			if (!strncmp(l, "== ", 3))
			{
				// A full dump the original took mid-run (before tick n): finish
				// tick n-1 first, apply what happened between, then print ours.
				rp.col.swap(block); block.clear(); rp.ci = 0;
				runTick();
				rp.col.clear(); rp.swp.clear(); rp.si = 0;
				tick = -1;
				for (const std::string &e : pending)
				{
					int s, i;
					if (sscanf(e.c_str(), "WAKE %d.%d", &s, &i) == 2) w.WakeOnFirstDraw(w.UidAt(s, i));
					else if (!strncmp(e.c_str(), "FIRE ", 5)) w.FirePlayer((uint16_t)atoi(e.c_str() + 5));
					else if (!strncmp(e.c_str(), "ANIM obj=", 9))
					{
						unsigned key, tpf, cnt;
						if (sscanf(e.c_str(), "ANIM obj=%d.%d key=%x tpf=%u cnt=%u", &s, &i, &key, &tpf, &cnt) == 5)
							w.AnimationDrawn(w.UidAt(s, i), key, (uint16_t)tpf, (uint16_t)cnt);
					}
					else if (!strncmp(e.c_str(), "FRAME fts=", 10)) w.DecayPainFlash((uint32_t)atoi(e.c_str() + 10));
				}
				pending.clear();
				printf("%s\n", l);
				fputs(w.Dump().c_str(), stdout);
				continue;
			}
			if (!strncmp(l, "TICK ", 5))
			{
				// finish the previous tick with the answers gathered for it
				rp.col.swap(block); block.clear(); rp.ci = 0;
				runTick();
				rp.col.clear(); rp.swp.clear(); rp.si = 0;
				for (const std::string &e : pending)
				{
					int s, i;
					if (sscanf(e.c_str(), "WAKE %d.%d", &s, &i) == 2) w.WakeOnFirstDraw(w.UidAt(s, i));
					else if (!strncmp(e.c_str(), "FIRE ", 5)) w.FirePlayer((uint16_t)atoi(e.c_str() + 5));
					else if (!strncmp(e.c_str(), "ANIM obj=", 9))
					{
						unsigned key, tpf, cnt;
						if (sscanf(e.c_str(), "ANIM obj=%d.%d key=%x tpf=%u cnt=%u", &s, &i, &key, &tpf, &cnt) == 5)
							w.AnimationDrawn(w.UidAt(s, i), key, (uint16_t)tpf, (uint16_t)cnt);
					}
					else if (!strncmp(e.c_str(), "FRAME fts=", 10)) w.DecayPainFlash((uint32_t)atoi(e.c_str() + 10));
				}
				pending.clear();
				tick = atoi(l + 5);
				continue;
			}
			if (!strncmp(l, "ENV", 3))
			{
				int px, py, pz, pa, ph, pzf, pit, pita; unsigned psec;
				const char *p = strstr(l, "px=");
				if (p && sscanf(p, "px=%d py=%d pz=%d pa=%d ph=%d psec=%x", &px, &py, &pz, &pa, &ph, &psec) == 6)
				{
					w.player.x = (int16_t)px; w.player.y = (int16_t)py; w.player.z = (int16_t)pz;
					w.player.angle = (uint16_t)pa; w.player.height = (uint16_t)ph;
					w.player.sector = psec ? (int)((psec - map.header.sectorsOffset) / 0x1a) : -1;
				}
				if ((p = strstr(l, "pzf=")) && sscanf(p, "pzf=%d pitch=%d pitcha=%d", &pzf, &pit, &pita) == 3)
				{ w.player.zFrac = (uint16_t)pzf; w.player.pitch = pit; w.player.pitchApplied = pita; }
				continue;
			}
			if (!strncmp(l, "COL ", 4)) { block.push_back(l); continue; }
			if (!strncmp(l, "SWP ", 4)) { rp.swp.push_back(l); continue; }
			if (!strncmp(l, "WAKE ", 5) || !strncmp(l, "FIRE ", 5) || !strncmp(l, "FRAME", 5) || !strncmp(l, "ANIM ", 5)) { pending.push_back(l); continue; }
		}
		if (f) fclose(f);
		printf("REPLAY ticks=%d question-mismatches=%d\n", tick, rp.qmis);
		if (rp.judge) printf("JUDGE simple collision agreed with the original on %d of %d answers\n", rp.agreed, rp.judged);
		w.SetCollision(nullptr);
	};

	FILE *sf = fopen(argv[3], "r");
	if (!sf) { fprintf(stderr, "no script\n"); return 1; }
	char ln[256];
	while (fgets(ln, sizeof ln, sf))
	{
		if (ln[0] == '#' || ln[0] == '\n') continue;
		int tick; char op[16], args[200] = "";
		if (sscanf(ln, "%d %15s %199[^\n]", &tick, op, args) < 2) continue;
		if (!strcmp(op, "sync")) { sync(args); continue; }
		if (!strcmp(op, "replay")) { replay(args); continue; }
		if (!strcmp(op, "unhide")) { int s = 0, i = 0; sscanf(args, "%d %d", &s, &i); if (Object *o = w.Find(w.UidAt(s, i))) o->flags &= 0x7f; continue; }
		if (!strcmp(op, "dump")) { printf("== %s\n", args); fputs(w.Dump().c_str(), stdout); }
		else if (!strcmp(op, "glb"))
		{
			printf("== %s\n", args);
			std::string d = w.Dump();
			size_t p = 0;
			while (p < d.size()) { size_t e = d.find('\n', p); if (d.compare(p, 3, "OBJ")) fwrite(d.data() + p, 1, e - p + 1, stdout); p = e + 1; }
		}
		else if (!strcmp(op, "w32"))
		{
			unsigned a, v; sscanf(args, "%x %x", &a, &v);
			if (a == 0x8a0f0) host.mHealth = (int32_t)v;
		}
		else if (!strcmp(op, "src")) { int s = -1, i = 0; sscanf(args, "%d %d", &s, &i); w.SetCommandSource(s < 0 ? 0 : w.UidAt(s, i)); }
		else if (!strcmp(op, "wake")) { int s = -1, i = 0; sscanf(args, "%d %d", &s, &i); printf("WAKE %d.%d %d\n", s, i, (int)w.WakeOnFirstDraw(w.UidAt(s, i))); }
		else if (!strcmp(op, "cmd"))
		{
			const int idx = atoi(args);
			if (idx <= 0 || (size_t)idx > map.commands.size()) { printf("CMD %d norec\n", idx); continue; }
			Command &c = map.commands[(size_t)idx - 1];
			const uint8_t o = c.opcode & 0x7f;
			w.chainInterrupt = 0;
			int r;
			if (o == 0x16) r = w.CmdSpawnObject(c);
			else if (o == 0x3c) r = w.CmdSpawnObjectAdv(c);
			else if (o == 0x3a) r = w.CmdChangeObjectId(c);
			else if (o == 0x33) r = w.CmdApplyDamage(c, -1, false, 1);
			else { printf("CMD %d op=%02x unsupported\n", idx, o); continue; }
			char b[32] = "-";
			int s, i;
			if (w.CommandSource() && w.Find(w.CommandSource(), &s, &i)) snprintf(b, sizeof b, "%d.%d", s, i);
			printf("CMD %d op=%02x ret=%d intr=%u mod=%02x src=%s\n", idx, o, r, w.chainInterrupt, c.modifier, b);
		}
		else if (!strcmp(op, "trig"))
		{
			// The sine and arctangent tables, for comparison with the original's memory.
			printf("SIN"); for (int i = 0; i < 512; i++) printf(" %04x", (uint16_t)ActorSin((uint32_t)i * 2)); printf("\n");
			printf("ATN"); for (int i = 0; i < 512; i += 1) printf(" %03x", ActorBearing(0, (int16_t)(ActorSin((uint32_t)((i * 2 + 0x100) & 0x3fe)) / 16), 0, (int16_t)(ActorSin((uint32_t)i * 2) / 16))); printf("\n");
			printf("SQR"); for (uint32_t v = 1; v < 0x40000000u; v = v * 3 + 7) printf(" %x", ActorIsqrt(v)); printf("\n");
		}
		else if (!strcmp(op, "loc"))
		{
			int x, y, z, h = -1; sscanf(args, "%d %d %d %d", &x, &y, &z, &h);
			printf("LOC %d %d %d %d = %04x\n", x, y, z, h, w.SectorOffset(w.LocateSector((int16_t)x, (int16_t)y, (int16_t)z, h)));
		}
		else if (!strcmp(op, "creatures"))
		{
			for (size_t si = 0; si < map.objects.size(); si++)
				for (size_t oi = 0; oi < map.objects[si].size(); oi++)
				{
					const Object &o = map.objects[si][oi];
					const uint16_t id = (uint16_t)(o.textureIndex | (o.textureSource << 8));
					const int c = tables.CreatureIndexForId(id);
					if (c < 0) continue;
					const ActorDef d = w.BuildDef(id);
					printf("CRE %zu.%zu x=%d y=%d z=%d id=%04x mon=%d flags=%02x hp=%u rate=%u turn=%u b1=%04x b2=%04x atk=%u modes=%02x h=%u r=%u valid=%d\n", si, oi, o.x, o.y, o.z, id, c, o.flags,
						d.Health(), d.MoveRate(), d.TurnLimit(), d.Bullet(0), d.Bullet(1), d.AttackRate(), d.ModeBits(), tables.DimHeight(id), tables.DimRadius(id), (int)d.valid);
				}
		}
		else if (!strcmp(op, "bullets"))
		{
			for (int i = 1; i <= db.ItemCount(); i++)
			{
				const BulletDesc d = w.BulletFor((uint32_t)i);
				if (d.templateId == 0) continue;
				printf("BUL %d tmpl=%x dmg=%u spd=%u ttl=%u w=%u hit=%x snd=%u flash=%u\n", i, d.templateId, d.damage, d.speed, d.ttl, d.width, d.hitAnim, d.hitSound, d.flash);
			}
		}
		else if (!strcmp(op, "fall"))
		{
			int fl, ce, st; sscanf(args, "%d %d %d", &fl, &ce, &st);
			const int32_t acc = ActorWorld::FallAccumulatorForDrop(st - fl);
			uint32_t d = ActorWorld::FallDamage(acc);
			if ((int32_t)d > host.mHealth) d = (uint32_t)host.mHealth;   // the original stops at 0
			printf("FALL drop=%d acc=%d dmg=%u\n", st - fl, acc, d);
		}
		else if (!strcmp(op, "tmpl"))
		{
			uint32_t fl = 0; const int item = atoi(args); const int32_t t = w.ResolveTemplate((uint32_t)item, fl);
			printf("TMPL %d = %x flags %08x\n", item, t, fl);
		}
		else if (!strcmp(op, "quit")) break;
	}
	fclose(sf);
	return 0;
}
