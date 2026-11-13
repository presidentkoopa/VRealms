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
#include "roth_das.h"
#include "roth_raw.h"
#include "roth_dbase100.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <ctype.h>
#include <algorithm>
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
	std::vector<int> doors;    // the original's door pool, by sector index
	bool DoorInPool(int sector) override { return std::find(doors.begin(), doors.end(), sector) != doors.end(); }
	int selected = 0;
	int SelectedItem() override { return selected; }

	// The weapon's outside world, from the oracle's INVS / WPN lines: the
	// inventory slots (id, quantity), the flash byte, Adam's item.
	ActorWorld *world = nullptr;
	const Dbase100 *db = nullptr;
	std::map<int, int> slotIds;
	std::vector<uint8_t> dbase200;
	int adam = 0, hooks = 0;
	void RemoveItem(int16_t id) override
	{
		hooks++;
		for (auto &e : slotIds)
			if (e.second == id)
			{
				if (mSlotQty[e.first] > 1) mSlotQty[e.first]--;
				else { e.second = 0; mSlotQty[e.first] = 0; }
				return;
			}
	}
	void NoAmmoMessage(int msg) override { printf("NOAMMO %d\n", msg); }
	void EquipFirstUsableWeapon() override
	{
		hooks++;
		std::vector<ActorWorld::CarriedSlot> carried;
		for (const auto &e : slotIds) if (e.second != 0) carried.push_back({ e.first, (uint16_t)e.second, (uint16_t)mSlotQty[e.first] });
		auto count = [&](int16_t want) { int n = 0; for (const auto &e : slotIds) if (e.second == want) n += mSlotQty[e.first]; return n; };
		int id = 0;
		const int slot = world->FindUsableWeapon(carried, count, id);
		if (slot < 0) { world->EquipWeapon(adam, -1); return; }      // reset_weapon_hud
		if (slot == world->weapon.heldSlot) return;
		world->ResetWeaponFireTiming();
		const int16_t ammo = world->EquipWeapon(id, slot);
		if (ammo != 0) world->SetWeaponAmmo(count(ammo));
	}
	void ConsumeHeldWeapon() override
	{
		hooks++;
		const int s = world->weapon.heldSlot;
		if (s >= 0) { slotIds[s] = 0; mSlotQty[s] = 0; }
		world->EquipWeapon(adam, -1);
	}
	int WeaponFrames(int32_t sprite) override
	{
		// load_dbase200_sprite_cached (das_assets.c:489-516): at sprite * 8 a
		// size dword, then the frame records.
		const size_t at = (size_t)(uint32_t)sprite * 8;
		if (sprite == 0 || at + 4 > dbase200.size()) return 0;
		const uint32_t size = (uint32_t)dbase200[at] | ((uint32_t)dbase200[at + 1] << 8) | ((uint32_t)dbase200[at + 2] << 16) | ((uint32_t)dbase200[at + 3] << 24);
		if (size == 0 || at + 4 + size > dbase200.size()) return 0;
		const uint8_t *fr = dbase200.data() + at + 4;
		return RunLengthRepairedCount(fr, size, 0, size, fr[8] | (fr[9] << 8));
	}
	std::string Invs() const
	{
		std::string o;
		int n = 0;
		for (const auto &e : slotIds) if (e.second != 0) n++;
		char b[64];
		snprintf(b, sizeof b, "INVS n=%d acc=%u rn=%d :", n, world->weapon.rechargeAccum, (int)world->weapon.recharge.size());
		o = b;
		for (const auto &e : slotIds) if (e.second != 0)
		{
			auto q = mSlotQty.find(e.first);
			snprintf(b, sizeof b, " %d:%d:%d", e.first, e.second, q == mSlotQty.end() ? 0 : q->second);
			o += b;
		}
		o += " |";
		for (const auto &r : world->weapon.recharge) { snprintf(b, sizeof b, " %d:%u:%u", r.slot, r.full, r.rate); o += b; }
		return o;
	}
};

// The oracle's WPN line into the port's weapon state (a sync), and its
// comparable part (everything before " fl=").
static void ParseWpn(const char *l, ActorWorld &w, TestHost &host)
{
	ActorWorld::PlayerWeapon &p = w.weapon;
	unsigned ctr, spr, end, timer, rng, bob, fl; int held, qty, hs, idx;
	sscanf(l, "WPN lock=%d done=%d ctr=%x anim=%d lower=%d timer=%u def=%d spr=%x rate=%d end=%u act=%d n=%d item=%d ammo=%d cap=%d scale=%d rng=%x xo=%d yo=%d sway=%d bob=%u held=%d qty=%d hs=%d fl=%u idx=%d adam=%d w4=%d",
		&p.lock, &p.shotDone, &ctr, &p.anim, &p.lower, &timer, &p.pendingDef, &spr, &p.rate, &end, &p.active, &p.slots, &p.item, &p.ammo, &p.ammoCap, &p.shotScale, &rng,
		&p.xoff, &p.yoff, &p.sway, &bob, &held, &qty, &hs, &fl, &idx, &host.adam, &p.word8a0f4);
	p.counter = (uint16_t)ctr; p.sprite = (int32_t)spr; p.cycleEnd = end; p.timer = timer; p.rng = (uint16_t)rng; p.bob = bob;
	p.held = held != 0; p.heldSlot = hs;
	w.lightFlash = (uint8_t)fl;
	if (idx > 0) { const int keep = p.slots; ParseWeaponActions(*host.db, idx, p.actions, 4); p.slots = keep; }
}
static std::string WpnHead(const std::string &l) { const size_t k = l.find(" idx="); return k == std::string::npos ? l : l.substr(0, k); }
static void ParseInvs(const char *l, ActorWorld &w, TestHost &host)
{
	unsigned acc; int n, rn, used = 0;
	if (sscanf(l, "INVS n=%d acc=%u rn=%d :%n", &n, &acc, &rn, &used) < 3) return;
	host.slotIds.clear(); host.mSlotQty.clear();
	w.weapon.rechargeAccum = acc;
	w.weapon.recharge.clear();
	const char *q = l + used;
	bool second = false;
	while (*q)
	{
		while (*q == ' ') q++;
		if (*q == '|') { second = true; q++; continue; }
		if (!*q) break;
		int a, b, c, k = 0;
		if (sscanf(q, "%d:%d:%d%n", &a, &b, &c, &k) != 3) break;
		if (!second) { host.slotIds[a] = b; host.mSlotQty[a] = c; }
		else w.weapon.recharge.push_back({ a, (uint16_t)b, (uint16_t)c });
		q += k;
	}
}

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
	if (!strcmp(argv[3], "--posegen") || !strcmp(argv[3], "--posegen-creatures"))
	{
		const bool nearCreatures = !strcmp(argv[3], "--posegen-creatures");
		std::vector<std::pair<int, int>> cre;
		for (const auto &list : map.objects)
			for (const Object &o : list)
				if (!(o.flags & 0x80) && tables.CreatureIndexForId((uint16_t)(o.textureIndex | (o.textureSource << 8))) >= 0)
					cre.push_back(std::make_pair((int)o.x, (int)o.y));
		if (nearCreatures && cre.empty()) { fprintf(stderr, "no visible creatures\n"); return 1; }
		// Random camera poses inside random sectors, one per frame, for the
		// visibility differential: "<frame> pose x y angle sectorOffset".
		ActorWorld pw; pw.Begin(&map, &db, &tables, nullptr);
		uint32_t rng = 12345;
		auto rnd = [&]() { rng = rng * 1103515245u + 12345u; return (rng >> 8) & 0xffffff; };
		int made = 0, frame = 3;
		printf("1 vistrace 1\n1 ticktrace 1\n");
		for (int tries = 0; made < 150 && tries < 200000; tries++)
		{
			const int sct = (int)(rnd() % map.sectors.size());
			const Sector &sc = map.sectors[sct];
			if (sc.firstFaceIndex < 0 || sc.ceilingHeight - sc.floorHeight < 100) continue;
			int minx = 32767, maxx = -32768, miny = 32767, maxy = -32768;
			for (int k = 0; k < sc.faceCount; k++)
			{
				const Vertex &v = map.vertices[map.faces[sc.firstFaceIndex + k].vertex1];
				minx = std::min(minx, (int)v.x); maxx = std::max(maxx, (int)v.x); miny = std::min(miny, (int)v.y); maxy = std::max(maxy, (int)v.y);
			}
			if (maxx - minx < 40 || maxy - miny < 40) continue;
			int x = minx + 16 + (int)(rnd() % (unsigned)(maxx - minx - 32)), y = miny + 16 + (int)(rnd() % (unsigned)(maxy - miny - 32));
			if (nearCreatures)
			{
				// within 1200 units of a visible creature
				const std::pair<int, int> &c = cre[rnd() % cre.size()];
				if (abs(x - c.first) > 1200 || abs(y - c.second) > 1200) continue;
			}
			if (pw.LocateSector((int16_t)x, (int16_t)y, sc.floorHeight, -1) != sct) continue;
			printf("%d pose %d %d %d %x\n", frame, x, y, (int)(rnd() % 512), pw.SectorOffset(sct));
			frame += 2; made++;
		}
		printf("%d quit\n", frame + 2);
		return 0;
	}
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
	host.db = &db;
	host.dbase200 = Slurp(FindFile(data, "DBASE200.DAT"));
	ActorWorld w;
	w.Begin(&map, &db, &tables, &host);
	host.world = &w;

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
			if (in && (!strncmp(l, "TICK ", 5) || !strncmp(l, "FRAME", 5) || !strncmp(l, "CMD ", 4) ||
				!strncmp(l, "PT ", 3) || !strncmp(l, "RC ", 3) || !strncmp(l, "TRIG ", 5) || !strcmp(l, "RWL"))) break;
			if (!in) continue;
			if (!strncmp(l, "WPN ", 4)) { ParseWpn(l, w, host); continue; }
			if (!strncmp(l, "INVS ", 5)) { ParseInvs(l, w, host); continue; }
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
		// " weapon": the port runs its own weapon (WeaponFrame / TriggerFire /
		// RechargeFrame / RebuildWeaponList at the PT / TRIG / RC / RWL lines)
		// and its shots are its own; the oracle's WPN and INVS lines after each
		// are compared with the port's, and its SHOT lines are not injected.
		const bool weaponMode = lab.size() > 7 && lab.compare(lab.size() - 7, 7, " weapon") == 0;
		if (weaponMode) lab.resize(lab.size() - 7);
		int wpnLines = 0, wpnBad = 0, wpnGarbled = 0, invGarbled = 0, invLines = 0, invBad = 0, trigs = 0;
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
		auto applyWeapon = [&](const std::string &e) -> bool
		{
			const char *c = e.c_str();
			if (!strncmp(c, "PT fts=", 7)) { w.WeaponFrame(atoi(c + 7)); return true; }
			if (!strncmp(c, "RC fts=", 7)) { w.RechargeFrame(atoi(c + 7)); return true; }
			if (!strncmp(c, "TRIG ", 5)) { int a = 0, b = 0; sscanf(c, "TRIG %d %d", &a, &b); w.TriggerFire(a, b); trigs++; return true; }
			if (!strcmp(c, "RWL"))
			{
				std::vector<std::pair<int, uint16_t>> used;
				for (const auto &u : host.slotIds) if (u.second != 0) used.push_back(std::make_pair(u.first, (uint16_t)u.second));
				w.RebuildWeaponList(used);
				return true;
			}
			if (!strncmp(c, "WPN ", 4))
			{
				std::string mine = w.DumpWeapon();
				mine.erase(mine.find_last_not_of("\n") + 1);
				wpnLines++;
				// A line the probe's log garbled (two writers interleaved: a field
				// with no digits) is counted apart, not compared.
				bool garbled = false;
				{
					auto split = [](const std::string &x) { std::vector<std::string> v; size_t a = 0; while (a < x.size()) { size_t b = x.find(' ', a); if (b == std::string::npos) b = x.size(); v.push_back(x.substr(a, b - a)); a = b + 1; } return v; };
					const std::vector<std::string> A = split(WpnHead(e)), B = split(mine);
					if (A.size() != B.size()) garbled = true;
					for (size_t k = 0; !garbled && k < A.size(); k++)
					{
						const size_t ea = A[k].find('='), eb = B[k].find('=');
						if (ea != eb || A[k].compare(0, ea == std::string::npos ? 0 : ea, B[k], 0, eb == std::string::npos ? 0 : eb) != 0 || ea + 1 >= A[k].size()) garbled = true;
						else if ((A[k].compare(0, 4, "ctr=") == 0 || A[k].compare(0, 4, "rng=") == 0) && A[k].size() != 8) garbled = true;
					}
				}
				if (garbled) { wpnGarbled++; return true; }
				if (WpnHead(e) != mine) { if (wpnBad++ < 6) printf("WPN differs at tick %d\n  orig %s\n  port %s\n", tick, WpnHead(e).c_str(), mine.c_str()); }
				return true;
			}
			if (!strncmp(c, "INVS ", 5))
			{
				invLines++;
				{
					// garbled in the log: the same shape token by token or it is not a reading
					const std::string mine = host.Invs();
					auto shape = [](const std::string &x) { std::string o; for (char ch : x) if (ch == ' ' || ch == ':' || ch == '=' || ch == '|') o += ch; return o; };
					bool bad = shape(mine) != shape(e) && e.size() != mine.size();
					for (size_t k = 0; k + 1 < e.size(); k++)
						if ((e[k] == ':' || e[k] == '=') && !(isdigit((unsigned char)e[k + 1]) || e[k + 1] == '-') && !(e[k] == ':' && e[k + 1] == ' ')) bad = true;
					if (bad) { invGarbled++; return true; }
				}
				if (host.Invs() != e) { if (invBad++ < 6) printf("INVS differs at tick %d\n  orig %s\n  port %s\n", tick, c, host.Invs().c_str()); }
				return true;
			}
			if (!strncmp(c, "SHOT ", 5) || !strncmp(c, "FIRE ", 5)) return true;
			return false;
		};
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
					if (weaponMode && applyWeapon(e)) continue;
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
					if (weaponMode && applyWeapon(e)) continue;
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
			if (weaponMode && (!strncmp(l, "PT ", 3) || !strncmp(l, "RC ", 3) || !strncmp(l, "TRIG ", 5) || !strcmp(l, "RWL") ||
				!strncmp(l, "WPN ", 4) || !strncmp(l, "INVS ", 5) || !strncmp(l, "SHOT ", 5))) { pending.push_back(l); continue; }
		}
		if (f) fclose(f);
		printf("REPLAY ticks=%d question-mismatches=%d\n", tick, rp.qmis);
		if (weaponMode) printf("WEAPON triggers=%d, weapon states compared %d (differ %d, garbled in the log %d), inventory states compared %d (differ %d, garbled %d), inventory hooks %d\n",
			trigs, wpnLines - wpnGarbled, wpnBad, wpnGarbled, invLines - invGarbled, invBad, invGarbled, host.hooks);
		if (rp.judge) printf("JUDGE simple collision agreed with the original on %d of %d answers\n", rp.agreed, rp.judged);
		w.SetCollision(nullptr);
	};

	static char l2[1 << 17];
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
		if (!strcmp(op, "wdump")) { fputs(w.DumpWeapon().c_str(), stdout); puts(host.Invs().c_str()); continue; }
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
		else if (!strcmp(op, "viscmp"))
		{
			// Every main-view tree the original logged (VIS lines of the oracle
			// log in argv[4]): the same view through RenderView, sectors compared.
			FILE *f = fopen(argv[4], "r"); static char l[1 << 17];
			int frames = 0, same = 0, skipped = 0;
			while (f && fgets(l, sizeof l, f))
			{
				if (!strncmp(l, "SECH ", 5))
				{
					// the original's live heights at this frame
					const char *q = strchr(l, ':');
					for (size_t i = 0; q && i < map.sectors.size(); i++)
					{
						int c2, f2, k;
						if (sscanf(q + 1, " %d,%d%n", &c2, &f2, &k) != 2) break;
						map.sectors[i].ceilingHeight = (int16_t)c2; map.sectors[i].floorHeight = (int16_t)f2;
						q += k;
					}
					continue;
				}
				if (!strncmp(l, "DOORS ", 6))
				{
					host.doors.clear();
					for (const char *q = strchr(l, ':') + 1; *q; ) { unsigned v; int k; if (sscanf(q, " %x%n", &v, &k) != 1) break; host.doors.push_back((int)((v - map.header.sectorsOffset) / 0x1a)); q += k; }
					continue;
				}
				if (strncmp(l, "VIS ", 4)) continue;
				unsigned psec, root, ang; int px, py, wl, wr, vox, voy, xs, xc, n;
				const char *p = strstr(l, "px=");
				if (!p || sscanf(p, "px=%d py=%d pa=%*u psec=%x tree=%*d root=%x win=%d,%d ang=%u vo=%d,%d xs=%d xc=%d n=%d", &px, &py, &psec, &root, &wl, &wr, &ang, &vox, &voy, &xs, &xc, &n) != 12) continue;
				std::vector<unsigned> want;
				for (const char *q = strchr(l, ':') + 1; *q; ) { unsigned v; int k; if (sscanf(q, " %x%n", &v, &k) != 1) break; want.push_back(v); q += k; }
				ActorWorld::RothViewParams vp;
				vp.camX = (int16_t)-vox; vp.camY = (int16_t)-voy; vp.viewAngle = (uint16_t)ang; vp.xscale = xs; vp.xcenter = xc;
				vp.left = (int16_t)wl; vp.right = (int16_t)wr; vp.rootSector = (int)((root - map.header.sectorsOffset) / 0x1a);
				std::vector<std::string> tr;
				if (getenv("VISTRACE")) vp.trace = &tr;
				// Only views whose camera stands inside its root sector, as in play.
				// (A pose written straight into the original's memory can leave the
				// player outside the sector recorded for him for a frame.)
				if (vp.rootSector < 0 || (size_t)vp.rootSector >= map.sectors.size()) { skipped++; continue; }   // a sector the original adds at run time (a door leaf)
				if (!getenv("VISALL") && w.LocateSector(vp.camX, vp.camY, map.sectors[vp.rootSector].floorHeight, vp.rootSector) != vp.rootSector) { skipped++; continue; }
				const ActorWorld::RothViewResult r = w.RenderView(vp);
				for (const std::string &t : tr) printf("  %s\n", t.c_str());
				std::vector<unsigned> got;
				for (int s : r.visits) { const unsigned o = w.SectorOffset(s); if (std::find(got.begin(), got.end(), o) == got.end()) got.push_back(o); }
				std::sort(got.begin(), got.end());
				frames++;
				if (got == want) same++;
				else if (frames - same <= 5)
				{
					printf("VISDIFF root=%04x win=%d,%d ang=%u cam=%d,%d got %zu want %zu:", root, wl, wr, ang, -vox, -voy, got.size(), want.size());
					for (unsigned v : want) if (std::find(got.begin(), got.end(), v) == got.end()) printf(" -%04x", v);
					for (unsigned v : got) if (std::find(want.begin(), want.end(), v) == want.end()) printf(" +%04x", v);
					printf("\n");
				}
			}
			if (f) fclose(f);
			printf("VISCMP %d of %d trees identical (%d skipped: camera outside its root sector)\n", same, frames, skipped);
		}
		else if (!strcmp(op, "wakecmp"))
		{
			// First wake of every creature: the original's (WAKE lines) against the
			// port's, the port waking from each main view the original rendered.
			FILE *f = fopen(argv[4], "r");
			std::map<std::string, int> orig, port;
			int frame = 0, lastPortFrame = -1; unsigned envSec = 0;
			while (f && fgets(l2, sizeof l2, f))
			{
				int s2, i2, fr;
				if (!strncmp(l2, "ENV", 3)) { const char *q = strstr(l2, "psec="); if (q) sscanf(q, "psec=%x", &envSec); continue; }
				if (sscanf(l2, "WAKE %d.%d", &s2, &i2) == 2)
				{
					char k[32]; snprintf(k, sizeof k, "%d.%d", s2, i2);
					if (!orig.count(k)) orig[k] = frame;
					continue;
				}
				if (!strncmp(l2, "SECH ", 5))
				{
					const char *q = strchr(l2, ':');
					for (size_t i = 0; q && i < map.sectors.size(); i++)
					{ int c2, f2, k; if (sscanf(q + 1, " %d,%d%n", &c2, &f2, &k) != 2) break; map.sectors[i].ceilingHeight = (int16_t)c2; map.sectors[i].floorHeight = (int16_t)f2; q += k; }
					continue;
				}
				if (!strncmp(l2, "DOORS ", 6))
				{
					host.doors.clear();
					for (const char *q = strchr(l2, ':') + 1; *q; ) { unsigned v; int k; if (sscanf(q, " %x%n", &v, &k) != 1) break; host.doors.push_back((int)((v - map.header.sectorsOffset) / 0x1a)); q += k; }
					continue;
				}
				unsigned root, ang; int wl, wr, vox, voy;
				const char *p = strstr(l2, "root=");
				if (strncmp(l2, "VIS ", 4) || !p || sscanf(l2, "VIS frame=%d", &fr) != 1) continue;
				frame = fr;
				if (sscanf(p, "root=%x win=%d,%d ang=%u vo=%d,%d", &root, &wl, &wr, &ang, &vox, &voy) != 6) continue;
				ActorWorld::RothViewParams vp;
				vp.camX = (int16_t)-vox; vp.camY = (int16_t)-voy; vp.viewAngle = (uint16_t)ang; vp.left = (int16_t)wl; vp.right = (int16_t)wr;
				vp.rootSector = (int)((root - map.header.sectorsOffset) / 0x1a);
				if ((size_t)vp.rootSector >= map.sectors.size()) continue;
				if (!getenv("WAKE_ALLTREES") && (wl != 0 || wr != 639)) continue;   // the main view only
				std::vector<ObjectUid> res;
				if (getenv("WAKE_PORTROOTS"))
				{
					// the port's whole pipeline: its own roots from the player
					int ppx, ppy; unsigned ps; sscanf(strstr(l2, "px="), "px=%d py=%d", &ppx, &ppy); sscanf(strstr(l2, "psec="), "psec=%x", &ps);
					if (ppx != vp.camX || ppy != vp.camY || fr == lastPortFrame) continue;   // once a frame, the main view's camera
					lastPortFrame = fr;
					if (envSec) ps = envSec;   // the sector the player was in when the frame was drawn
					const int psi = (int)((ps - map.header.sectorsOffset) / 0x1a);
					if (psi < 0 || (size_t)psi >= map.sectors.size()) continue;
					for (const auto &rp2 : w.ViewRoots(vp.camX, vp.camY, vp.viewAngle, psi))
						for (ObjectUid u : w.RenderView(rp2).resolved) res.push_back(u);
				}
				else res = w.RenderView(vp).resolved;
				for (ObjectUid u : res)
				{
					int s3, i3; const Object *o = w.Find(u, &s3, &i3);
					if (!o || tables.CreatureIndexForId((uint16_t)(o->textureIndex | (o->textureSource << 8))) < 0) continue;
					char k[32]; snprintf(k, sizeof k, "%d.%d", s3, i3);
					if (!port.count(k)) port[k] = frame;
				}
			}
			if (f) fclose(f);
			int same = 0, total = 0;
			std::map<std::string, int> all = orig; for (auto &e : port) all[e.first] = 0;
			for (auto &e : all)
			{
				total++;
				const int a = orig.count(e.first) ? orig[e.first] : -1, b = port.count(e.first) ? port[e.first] : -1;
				// The original wakes at the render of frame F and reports it before the next tick.
				// A wake before the first traced view (the map's opening frames) is
				// compared with the port's first traced view.
				const bool ok = (a < 0 && b < 0) || (a >= 0 && b >= 0 && (abs(a - b) <= 1 || (a <= 1 && b <= 3)));
				if (ok) same++;
				printf("WAKECMP %s original %d port %d %s\n", e.first.c_str(), a, b, ok ? "ok" : "DIFF");
			}
			printf("WAKECMP %d of %d creatures woke on the same frame\n", same, total);
		}
		else if (!strcmp(op, "rootcmp"))
		{
			// The original's roots and windows per frame against ViewRoots.
			FILE *f = fopen(argv[4], "r");
			int frames = 0, same = 0, curFrame = -1;
			std::vector<std::string> want; int px = 0, py = 0; unsigned psec = 0, ang = 0;
			auto finish = [&]()
			{
				if (curFrame < 0 || want.empty()) return;
				const int ps = (int)((psec - map.header.sectorsOffset) / 0x1a);
				if (ps < 0 || (size_t)ps >= map.sectors.size()) { want.clear(); return; }
				std::vector<std::string> got;
				for (const auto &r : w.ViewRoots((int16_t)px, (int16_t)py, (uint16_t)ang, ps))
				{ char b[64]; snprintf(b, sizeof b, "%04x:%d,%d", w.SectorOffset(r.rootSector), r.left, r.right); got.push_back(b); }
				std::sort(got.begin(), got.end()); std::sort(want.begin(), want.end());
				want.erase(std::unique(want.begin(), want.end()), want.end());
				frames++;
				if (got == want) same++;
				else if (frames - same <= 8)
				{
					printf("ROOTDIFF frame %d want", curFrame); for (auto &x : want) printf(" %s", x.c_str());
					printf("  got"); for (auto &x : got) printf(" %s", x.c_str()); printf("\n");
				}
				want.clear();
			};
			while (f && fgets(l2, sizeof l2, f))
			{
				int fr, wl, wr, vox, voy; unsigned root, a2;
				if (strncmp(l2, "VIS ", 4) || sscanf(l2, "VIS frame=%d", &fr) != 1) continue;
				const char *p = strstr(l2, "root=");
				if (!p || sscanf(p, "root=%x win=%d,%d ang=%u vo=%d,%d", &root, &wl, &wr, &a2, &vox, &voy) != 6) continue;
				if (fr != curFrame) { finish(); curFrame = fr; }
				const char *q = strstr(l2, "psec="); sscanf(q, "psec=%x", &psec);
				// the main view's trees: camera at the player (not a mirror's)
				px = -vox; py = -voy; ang = a2;
				int ppx, ppy; sscanf(strstr(l2, "px="), "px=%d py=%d", &ppx, &ppy);
				if (ppx != px || ppy != py) continue;
				char b[64]; snprintf(b, sizeof b, "%04x:%d,%d", root, wl, wr); want.push_back(b);
			}
			finish();
			if (f) fclose(f);
			printf("ROOTCMP %d of %d frames: same roots and windows\n", same, frames);
		}
		else if (!strcmp(op, "wakecheck"))
		{
			// The creatures the player's view would wake: "x y angle512".
			int x, y, a; sscanf(args, "%d %d %d", &x, &y, &a);
			w.player.x = (int16_t)x; w.player.y = (int16_t)y; w.player.angle = (uint16_t)a;
			w.player.sector = w.LocateSector((int16_t)x, (int16_t)y, -32768, -1);
			ActorWorld::RothViewParams vp; vp.camX = (int16_t)x; vp.camY = (int16_t)y; vp.viewAngle = (uint16_t)(0 - a); vp.rootSector = w.player.sector;
			const ActorWorld::RothViewResult r = w.RenderView(vp);
			printf("WAKECHECK %d %d %d sector %d visits %zu creatures:", x, y, a, w.player.sector, r.visits.size());
			for (ObjectUid u : r.resolved)
			{
				int s2, i2; const Object *o = w.Find(u, &s2, &i2);
				if (o && tables.CreatureIndexForId((uint16_t)(o->textureIndex | (o->textureSource << 8))) >= 0) printf(" %d.%d", s2, i2);
			}
			printf("\n");
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
		else if (!strcmp(op, "facecmp"))
		{
			// Every FACE line of the oracle log (the probe's trace of
			// resolve_face_surface_id, renderer.c:12700): the slot's state,
			// flags and pain, the object's facing, the view angle, the
			// object's view x and depth, the view index the original passed
			// and the picture and pain byte it came back with. The port must
			// make the same view index from the same inputs, and the same
			// picture and pain byte from the same slot.
			if (argc <= 4) { printf("facecmp needs the oracle log\n"); continue; }
			FILE *f = fopen(argv[4], "r");
			char l[600]; int n = 0, badCi = 0, badPic = 0, badPain = 0;
			while (f && fgets(l, sizeof l, f))
			{
				int s2, i2, mon; unsigned st, f9, fa, pain, ci, rot, va, pic, pain2; int xv, dep;
				if (sscanf(l, "FACE obj=%d.%d mon=%d st=%x f9=%x fa=%x pain=%x ci=%u rot=%x va=%x xv=%d dep=%d -> %x pain=%x",
					&s2, &i2, &mon, &st, &f9, &fa, &pain, &ci, &rot, &va, &xv, &dep, &pic, &pain2) != 14) continue;
				const ObjectUid uid = w.UidAt(s2, i2);
				Object *o = w.Find(uid);
				if (o == nullptr) continue;
				if (o->unk0C == 0 || !(o->renderType & 1)) { o->flags &= 0x7f; w.WakeOnFirstDraw(uid); }
				if (o->unk0C == 0) continue;
				ActorSlot &a = w.actors[(o->unk0C - 1) / 0x22];
				a.creature = mon; a.state = (uint8_t)st; a.bits = (uint8_t)f9; a.move = (uint8_t)fa; a.pain = (uint8_t)pain;
				const int myCi = ActorWorld::ViewIndex((uint8_t)rot, (uint16_t)va, dep, xv);
				const uint16_t myPic = w.CreaturePicture(uid, (int)ci, true);
				n++;
				if (myCi != (int)ci) { if (badCi++ < 5) printf("FACE ci: %s   port %d\n", l, myCi); }
				if (myPic != pic) { if (badPic++ < 5) printf("FACE picture: %s   port %04x\n", l, myPic); }
				if (a.pain != pain2) { if (badPain++ < 5) printf("FACE pain: %s   port %02x\n", l, a.pain); }
			}
			if (f) fclose(f);
			printf("FACECMP %d draws: view index %d differ, picture %d differ, pain byte %d differ\n", n, badCi, badPic, badPain);
		}
		else if (!strcmp(op, "advcmp"))
		{
			// Every ADV line of the oracle log (the probe's trace of
			// advance_das_sprite_animation_frame, das_assets.c:1450): the
			// picture's frame and timer before and after one drawn step,
			// its rate and frame count and the frame's tick scale. The
			// port's StepPictureClock must make the same step.
			if (argc <= 4) { printf("advcmp needs the oracle log\n"); continue; }
			FILE *f = fopen(argv[4], "r");
			char l[600]; int n = 0, bad = 0, gated = 0;
			while (f && fgets(l, sizeof l, f))
			{
				unsigned blk, count, rate, t0, t1, fts, gate = 0, now = 1; int f0, f1;
				if (sscanf(l, "ADV blk=%x count=%u rate=%x frame %d->%d timer %x->%x fts=%u gate=%u now=%u", &blk, &count, &rate, &f0, &f1, &t0, &t1, &fts, &gate, &now) < 8) continue;
				// The block's own once-a-frame gate (das_assets.c:1466-1468): a
				// second draw of the same picture in one frame changes nothing.
				// The port steps each picture once a frame (StepDrawnPictures),
				// which is this gate; the gated calls are counted, not stepped.
				if (f0 >= 0 && rate != 0xff && gate == now) { gated++; if (f1 != f0 || t1 != t0) bad++; continue; }
				if (count == 0xfffe) continue;                     // the static kind returns at once
				ActorWorld::PictureClock c; c.frame = f0 < 0 ? -1 : f0; c.timer = (uint8_t)t0;
				const bool fresh = f0 < 0;
				if (f0 < -1) continue;                             // a held frame: republished as is
				ActorWorld::StepPictureClock(c, (uint8_t)rate, (int)count, (int)fts);
				n++;
				if (c.frame != f1 || (!fresh && c.timer != (uint8_t)t1) || (fresh && !(rate & 0x80) && c.timer != (uint8_t)t1))
					if (bad++ < 8) printf("ADV differs: %s   port frame %d timer %02x\n", l, c.frame, c.timer);
			}
			if (f) fclose(f);
			printf("ADVCMP %d steps (and %d repeat draws in the same frame), %d differ\n", n, gated, bad);
		}
		else if (!strcmp(op, "weaponcmp"))
		{
			// Every WEAP line of the oracle log: the four-slot table the
			// original's apply_weapon_action_attributes filled for an item,
			// from a buffer prefilled with 0xaa. ParseWeaponActions must fill
			// the same bytes from the same prefill.
			if (argc <= 4) { printf("weaponcmp needs the oracle log\n"); continue; }
			FILE *f = fopen(argv[4], "r");
			static char l[4096]; int n = 0, bad = 0, slots = 0, soundKeyFields = 0, beyond = 0;
			while (f && fgets(l, sizeof l, f))
			{
				int idx; unsigned cnt; int used = 0;
				if (sscanf(l, "WEAP %d n=%u%n", &idx, &cnt, &used) != 2) continue;
				// Past the end of DBASE100's item table the original reads on
				// into whatever memory follows it; no data names such an item.
				if (idx > db.ItemCount()) { beyond++; continue; }
				WeaponAction t[4];
				for (auto &a : t) memset(a.b, 0xaa, sizeof a.b);
				const int mine = ParseWeaponActions(db, idx, t, 4);
				// Ops 0x19 and 0x21 are SOUND KEYS: at start-up the original
				// rewrites them in DBASE100, in place, with the sample row each
				// resolves to (resolve_dbase100_sound_ids, audio.c:1896-1919);
				// the engine's sound lane does the same to TheDbase100(). The
				// headless oracle has its own sample table, so the two fields
				// those ops fill (+0x10, +0x34) are taken from it and counted,
				// and everything else must match byte for byte.
				{
					const char *q = l + used;
					for (int k = 0; k < (int)cnt && k < 4; k++)
					{
						while (*q == ' ') q++;
						for (int off : { 0x10, 0x34 })
							for (int j = 0; j < 4; j++)
							{
								unsigned byte = 0; sscanf(q + 2 * (off + j), "%2x", &byte);
								if (t[k].b[off + j] != (uint8_t)byte) soundKeyFields++;
								t[k].b[off + j] = (uint8_t)byte;
							}
						q += 0xa0;
					}
				}
				std::string hex;
				for (int k = 0; k < 4; k++) { hex += ' '; for (int j = 0; j < 0x50; j++) { char h[3]; snprintf(h, sizeof h, "%02x", t[k].b[j]); hex += h; } }
				std::string theirs = l + used; theirs.erase(theirs.find_last_not_of("\r\n") + 1);
				n++; slots += (int)cnt;
				if (mine != (int)cnt || hex != theirs) { if (bad++ < 3) printf("WEAP %d differs: count %u vs port %d\n  orig%s\n  port%s\n", idx, cnt, mine, theirs.c_str(), hex.c_str()); }
			}
			if (f) fclose(f);
			printf("WEAPONCMP %d weapon items (%d action slots), %d differ (sound-key bytes taken from the original's resolution: %d; indices past the item table skipped: %d)\n", n, slots, bad, soundKeyFields, beyond);
		}
		else if (!strcmp(op, "creaturerec"))
		{
			// The picture words of one creature record (R:12700-12745 reads
			// them): eight-view tables at +0x04 +0x14 +0x24 +0x34 +0x44, single
			// pictures from +0x54.
			int c = 0; sscanf(args, "%d", &c);
			printf("REC %d", c);
			for (int off = 0; off < 0x68; off += 2) printf("%s%02x:%04x", off % 16 ? " " : "\n  ", off, tables.CreatureWord(c, off));
			printf("\n");
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
