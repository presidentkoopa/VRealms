// tools/colt_test/test_colt.cpp -- THE COLT'S REAL-PARTS RELOAD AGAINST THE ORIGINAL'S RULES, engine-free.
//
// The port's own weapon logic (ActorWorld: ParseWeaponActions, TriggerFire, WeaponFrame -- proven against the running
// original by tools/actors_test, and for the Colt's reload by tools/colt_test/oracle_reload.txt, 0 differ) with the
// retail DBASE100/200 and STUDY1, driven tic by tic exactly as roth_actors_engine.cpp WeaponTick drives it (2 Realms
// ticks a tic; the seat's reload pulled at the next tic's start, the button's pull through PullReachesOriginal, then
// WeaponFrame, then roth_colt::Step with the tic's events), and the inventory as a host with remove_item's hook
// (inventory.c:263-267: the active ammo item taken lowers g_active_weapon_ammo). Scripted hands play the reload.
//
//   test_colt DATA
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roth_colt.h"
#include "roth_actors.h"
#include "roth_raw.h"
#include "roth_dbase100.h"
#include "roth_das.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <vector>

using namespace roth;
using namespace roth::colt;

static int gFails = 0, gPass = 0;
static void Check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void Check(bool ok, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	printf("%s ", ok ? "PASS" : "FAIL");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	if (ok) gPass++; else gFails++;
}

static std::vector<uint8_t> Slurp(const std::string &p)
{
	std::vector<uint8_t> d;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return d;
	fseek(f, 0, SEEK_END); d.resize(ftell(f)); fseek(f, 0, SEEK_SET);
	if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
	fclose(f);
	return d;
}
static std::string FindFile(const std::string &dir, const std::string &name)
{
	DIR *d = opendir(dir.c_str());
	std::string out = dir + "/" + name;
	if (!d) return out;
	while (dirent *e = readdir(d)) if (!strcasecmp(e->d_name, name.c_str())) { out = dir + "/" + e->d_name; break; }
	closedir(d);
	return out;
}

// The inventory: slot 4 the Colt (its word), slot 5 the magazines (i51). remove_item with its ammo hook.
struct Host : ActorHost
{
	ActorWorld *world = nullptr;
	std::vector<uint8_t> dbase200;
	int noAmmo = 0, equipFirst = 0, removed51 = 0, sounds = 0;
	void RemoveItem(int16_t id) override
	{
		if (world->weapon.actions[0].D(0x30) == (int32_t)id) world->weapon.ammo--;   // inventory.c:263-267
		if (id == kMagazine && mSlotQty[5] > 0) { mSlotQty[5]--; removed51++; }
	}
	void NoAmmoMessage(int msg) override { noAmmo = msg; }
	void EquipFirstUsableWeapon() override { equipFirst++; }
	void WeaponSound(int) override { sounds++; }
	int WeaponFrames(int32_t sprite) override
	{
		const size_t at = (size_t)(uint32_t)sprite * 8;
		if (sprite == 0 || at + 4 > dbase200.size()) return 0;
		const uint32_t size = (uint32_t)dbase200[at] | ((uint32_t)dbase200[at + 1] << 8) | ((uint32_t)dbase200[at + 2] << 16) | ((uint32_t)dbase200[at + 3] << 24);
		if (size == 0 || at + 4 + size > dbase200.size()) return 0;
		const uint8_t *fr = dbase200.data() + at + 4;
		return RunLengthRepairedCount(fr, size, 0, size, fr[8] | (fr[9] << 8));
	}
};

struct Sim
{
	ActorWorld &w;
	Host &h;
	Rig rig;
	bool reloadDue = false;
	int tic = 0;
	std::vector<Act> acts;
	int pulls = 0, dry = 0, shots = 0;
	Sim(ActorWorld &ww, Host &hh) : w(ww), h(hh) {}
	Game Now(uint32_t ev) { return GameOf(w, h.SlotQuantity(w.weapon.heldSlot), w.weapon.sprite != 0 ? h.WeaponFrames(w.weapon.sprite) : 0, ev); }
	// one engine tic, as WeaponTick + ColtAfterWeapon
	Event Tic(bool button, const Hands &hands)
	{
		w.weaponEvents = 0;
		const Game g0 = Now(0);
		if (reloadDue)
		{
			reloadDue = false;
			if (g0.colt && g0.qty == 0 && g0.mags > 0 && g0.lock == 0) w.TriggerFire(0, 0);
		}
		else if (button)
		{
			if (PullReachesOriginal(rig, g0, hands.vr)) { w.TriggerFire(0, 0); pulls++; }
			else dry++;
		}
		w.WeaponFrame(2, 0);
		const Game g = Now(w.weaponEvents);
		if (g.shotLeft) shots++;
		const Event e = Step(rig, g, hands, 180 * 0.000271900171);
		if (e.act == ACT_SEAT && g.qty == 0) reloadDue = true;
		if (e.act != ACT_NONE) acts.push_back(e.act);
		tic++;
		return e;
	}
	void Idle(int n, const Hands &hd) { for (int i = 0; i < n; i++) Tic(false, hd); }
	// the button held one tic, then released until the cycle (and lock) ends
	void Shoot(const Hands &hd) { Tic(true, hd); for (int i = 0; i < 60 && w.weapon.lock != 0; i++) Tic(false, hd); Idle(2, hd); }
	int Qty() { return h.SlotQuantity(4); }
	int Mags() { return h.SlotQuantity(5); }
};


int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: test_colt DATA\n"); return 2; }
	const std::string data = argv[1];
	std::vector<uint8_t> raw = Slurp(FindFile(data + "/M", "STUDY1.RAW"));
	Map map = ParseRaw(raw.data(), raw.size());
	if (!map.ok()) { printf("map: %s\n", map.error.c_str()); return 2; }
	Dbase100 db;
	if (!db.Load(Slurp(FindFile(data, "DBASE100.DAT")))) { printf("DBASE100: %s\n", db.Error().c_str()); return 2; }
	std::vector<uint8_t> ademo = Slurp(FindFile(data + "/M", "ADEMO.DAS"));
	CreatureTables tables = LoadCreatureTables(ademo.data(), ademo.size());

	// ---- the Colt's record, as the port reads it ----
	WeaponAction a[4];
	const int n = ParseWeaponActions(db, kColt, a, 4);
	printf("i9: %d WeaponAction block(s): picture %x, bullet i%d, shot frame %d, rate %d, cap %d, ammo item 0x%x, reload picture %x rate %d\n",
		n, (unsigned)a[0].D(0), a[0].D(8), a[0].D(0x0c), a[0].D(0x3c), a[0].D(4), (unsigned)a[0].D(0x30), (unsigned)a[0].D(0x2c), a[0].D(0x40));
	Check(n == 1 && a[0].D(4) == 12 && a[0].D(0x30) == kMagazine && a[0].D(0x2c) != 0,
		"the Colt (i9): one block, magazine 12, ammo item i51, a reload picture (the record the rules below assume)");

	auto setup = [&](ActorWorld &w, Host &h, int qty, int mags) {
		h.dbase200 = Slurp(FindFile(data, "DBASE200.DAT"));
		w.Begin(&map, &db, &tables, &h);
		h.world = &w;
		w.player.x = -864; w.player.y = 3412; w.player.angle = 466;
		w.player.sector = w.LocateSector(-864, 3412, 0, -1);
		h.mSlotQty[4] = qty;
		h.mSlotQty[5] = mags;
		w.ResetWeaponFireTiming();
		const int16_t ammo = w.EquipWeapon(kColt, 4);
		w.SetWeaponAmmo(ammo == kMagazine ? mags : 0);
		w.weapon.anim = 3;   // raised (the original's weapon up: player.c:470-479)
		w.weapon.lower = 0;
	};
	Hands vr; vr.vr = true;
	Hands flat;

	// ==== 1. FLAT: the original's automatic reload on the pull (1:1) ====
	{
		ActorWorld w; Host h; setup(w, h, 12, 3);
		Sim s(w, h);
		for (int i = 0; i < 12; i++) s.Shoot(flat);
		Check(s.Qty() == 0 && s.Mags() == 3 && s.shots == 12, "flat: 12 shots empty the word (12 -> 0), no magazine used (i51 %d, shots %d)", s.Mags(), s.shots);
		s.Tic(true, flat);
		Check(s.Qty() == 12 && s.Mags() == 2 && w.weapon.ammo == 2 && w.weapon.pendingDef == 0 && w.weapon.lock != 0,
			"flat: the 13th pull is the original's reload: one i51 (3 -> %d), the word 0 -> %d, g_active_weapon_ammo %d, no shot (pending %d), the reload picture's lock (%d)",
			s.Mags(), s.Qty(), w.weapon.ammo, w.weapon.pendingDef, w.weapon.lock);
		int lockTics = 0; while (w.weapon.lock != 0 && lockTics < 400) { s.Tic(false, flat); lockTics++; }
		// player.c:488-521: armed, counter = rate + fts; then + fts a frame; the cycle ends when counter / rate reaches the
		// picture's frames: (29 x 8 - (8 + 2)) / 2 = 111 tics after the arming tic, 2 Realms ticks a tic.
		const int frames = h.WeaponFrames(a[0].D(0x2c)), rate = a[0].D(0x40), want = (frames * rate - (rate + 2) + 1) / 2 + 1;
		Check(lockTics == want, "flat: the reload picture (%d frames x rate %d) holds the trigger %d tics after the pull (the original's count: %d)", frames, rate, lockTics, want);
	}

	// ==== 2. VR: fire to empty; the slide locks; the dry pull; the real-parts reload ====
	{
		ActorWorld w; Host h; setup(w, h, 12, 3);
		Sim s(w, h);
		Hands hd = vr;
		// R1: a release with rounds in the magazine is refused
		hd.reloadInput = true; const Event e1 = s.Tic(false, hd); hd.reloadInput = false;
		Check(e1.act == ACT_REFUSE_DROP && s.rig.magIn && s.Qty() == 12, "R1: the release with 12 in the word is refused (%s); the magazine stays, the word 12", e1.why);
		for (int i = 0; i < 12; i++) s.Shoot(hd);
		Check(s.Qty() == 0 && s.Mags() == 3 && s.shots == 12, "VR: 12 shots, the word 0, i51 still 3");
		Check(s.rig.slideLocked && !s.rig.chambered, "R7: the slide locked back after the shot that emptied the word");
		const int pullsWas = s.pulls;
		s.Tic(true, hd); s.Idle(3, hd);
		Check(s.pulls == pullsWas && s.dry >= 1 && s.Qty() == 0 && s.Mags() == 3 && w.weapon.lock == 0 && h.removed51 == 0,
			"R5: the 13th pull is a dry click -- trigger_weapon_fire NOT called, no reload (word 0, i51 3, lock 0, removed %d)", h.removed51);
		// R2: no magazine taken while the gun has one
		hd.offAtPouch = true; hd.offGrip = true; const Event e2 = s.Tic(false, hd); hd.offGrip = false; s.Tic(false, hd);
		Check(e2.act == ACT_REFUSE_TAKE, "R2: a magazine from the pouch refused while the gun has one (%s)", e2.why);
		// R1: the release on the empty magazine drops it
		hd.offAtPouch = false;
		hd.offAtMagButton = true; hd.magButtonPress = true; const Event e3 = s.Tic(false, hd); hd.magButtonPress = false; hd.offAtMagButton = false;
		Check(e3.act == ACT_DROP_MAG && !s.rig.magIn && s.Qty() == 0 && s.Mags() == 3, "R1: the release with the word 0 drops the magazine (presentation only: word 0, i51 3)");
		// R3: taken and let go before it seats: back to the pouch
		hd.offAtPouch = true; hd.offGrip = true; const Event e4 = s.Tic(false, hd);
		Check(e4.act == ACT_TAKE_MAG && s.rig.magInHand && s.Mags() == 3, "R2: a magazine taken from the pouch (i51 still %d: the inventory unchanged)", s.Mags());
		hd.offAtPouch = false; hd.offGrip = false; const Event e5 = s.Tic(false, hd);
		Check(e5.act == ACT_RETURN_MAG && !s.rig.magInHand && s.Mags() == 3 && s.Qty() == 0, "R3: let go before it seated: back to the pouch, nothing consumed");
		// taken again, carried to the well, seated: the original's reload, once
		hd.offAtPouch = true; hd.offGrip = true; s.Tic(false, hd); hd.offAtPouch = false;
		s.Idle(5, hd);
		hd.magAtWell = true; const Event e6 = s.Tic(false, hd); hd.magAtWell = false;
		Check(e6.act == ACT_SEAT && s.reloadDue, "R4: the magazine seated: the original's reload due at the next tic");
		s.Tic(false, hd);
		Check(s.Mags() == 2 && s.Qty() == 12 && w.weapon.ammo == 2 && h.removed51 == 1 && w.weapon.pendingDef == 0 && w.weapon.lock != 0,
			"R4: the seat pulled trigger_weapon_fire once: remove_item(i51) (3 -> %d, removed %d), the word 0 -> %d (+ the cap 12), g_active_weapon_ammo %d, no shot, the reload picture's lock %d",
			s.Mags(), h.removed51, s.Qty(), w.weapon.ammo, w.weapon.lock);
		hd.offGrip = false; s.Tic(false, hd);
		// the slide is still back: a pull is a dry click even with 12 in the word (R6); the original's own lock also holds
		const int p2 = s.pulls; s.Tic(true, hd);
		Check(s.pulls == p2 && s.Qty() == 12, "R6: with the slide still locked back the pull is a dry click (the word stays 12)");
		// rack: the off hand on the serrations pulls past 90 % and lets go
		hd.offAtSlide = true; hd.offGrip = true; hd.slideAlong = 0; s.Tic(false, hd);
		hd.slideAlong = 0.05; s.Tic(false, hd);
		const bool back = s.rig.rackedBack;
		hd.offGrip = false; const Event e8 = s.Tic(false, hd); hd.offAtSlide = false;
		Check(e8.act == ACT_SLIDE_FORWARD && !s.rig.slideLocked && s.rig.chambered && s.Qty() == 12 && s.Mags() == 2,
			"R7: racked (pulled back %d, then let go: %s): the slide forward, a round chambered; the game untouched (word 12, i51 2)", (int)back, e8.why);
		// the reload picture's lock (the original's) must end before the next shot
		for (int i = 0; i < 200 && w.weapon.lock != 0; i++) s.Tic(false, hd);
		s.Shoot(hd);
		Check(s.Qty() == 11 && s.Mags() == 2 && s.shots == 13, "VR: fire again after the reload: the word 12 -> %d, i51 %d", s.Qty(), s.Mags());
		// THE SAME NUMBERS AS THE ORIGINAL: after 12 shots and its reload the original holds the word 12 and i51 2
		// (tools/colt_test/oracle_reload.txt: INVS 4:9:12 5:51:2), and its 13th shot leaves 11.
		Check(h.removed51 == 1, "one i51 consumed in the whole VR sequence, as the original's one reload");
	}

	// ==== 3. VR with no magazine carried: the pull on the empty word IS the original's (message 6, the next weapon) ====
	{
		ActorWorld w; Host h; setup(w, h, 1, 0);
		Sim s(w, h);
		s.Shoot(vr);
		Check(s.Qty() == 0, "VR, no i51: the last round fired");
		s.Tic(true, vr);
		Check(h.noAmmo == 6 && h.equipFirst == 1 && s.Mags() == 0, "R5: with no i51 carried the pull reaches the original: show_no_ammo_message(6) and equip_first_usable_weapon (%d, %d)", h.noAmmo, h.equipFirst);
		// R2: nothing to take
		Hands hd = vr;
		hd.reloadInput = true; s.Tic(false, hd); hd.reloadInput = false;
		hd.offAtPouch = true; hd.offGrip = true; const Event e = s.Tic(false, hd);
		Check(e.act == ACT_REFUSE_TAKE && !s.rig.magInHand, "R2: no Colt-45 magazine carried: nothing to take (%s)", e.why);
	}

	// ==== 4. the parts: the take frames the original's frames choose ====
	{
		Rig r; Game g; g.colt = true; g.qty = 5; g.frames = 8; g.shotFrame = 3; g.firing = true;
		g.framePos = 0; Parts p = PartsFor(r, g);
		Check(p.trigger == kTakeReady && p.slide == kTakeReady, "parts: at the pull the trigger and slide at the ready frame (%.1f)", p.trigger);
		g.framePos = 1.5; p = PartsFor(r, g);
		Check(p.trigger > kTakeReady && p.trigger < kTakeHammerDown && p.slide == kTakeReady, "parts: before the shot frame the trigger presses (take %.2f), the slide still", p.trigger);
		g.framePos = 3 + 5 * 0.4; p = PartsFor(r, g);
		Check(fabs(p.slide - (kTakeHammerDown + 10 * 0.4)) < 1e-9 && !p.chamberShown, "parts: after the shot the slide follows the take 115 -> 125 (%.2f at 40 %%); the round gone", p.slide);
		g.qty = 0; g.framePos = 3 + 5 * 0.9; p = PartsFor(r, g);
		Check(p.slide == kTakeSlideLock, "parts: the last round's cycle stops the slide on the slide stop (take %.0f)", p.slide);
		r.slideLocked = true; g.firing = false; p = PartsFor(r, g);
		Check(p.slide == kTakeSlideLock && !p.chamberShown && !p.topRoundShown, "parts: locked back, empty: slide on the stop, no rounds drawn");
		r.slideLocked = false; r.magIn = false; g.qty = 0; p = PartsFor(r, g);
		Check(!p.magShown, "parts: the magazine out of the grip is not drawn in it");
	}

	// ==== 5. R8: the shooting hand's button through a whole reload (the original's numbers throughout) ====
	{
		ActorWorld w; Host h; setup(w, h, 12, 3);
		Sim s(w, h);
		Hands hd = vr;
		hd.mainButton = true; const Event b1 = s.Tic(false, hd); hd.mainButton = false;
		Check(b1.act == ACT_REFUSE_DROP && s.rig.magIn && s.Qty() == 12, "R8/R1: the button on a full magazine, slide forward: the release, refused (%s); the magazine stays", b1.why);
		for (int i = 0; i < 12; i++) s.Shoot(hd);
		Check(s.rig.slideLocked && s.Qty() == 0, "R8: 12 shots, the slide locked back on the empty magazine");
		hd.mainButton = true; const Event b2 = s.Tic(false, hd); hd.mainButton = false;
		Check(b2.act == ACT_DROP_MAG && !s.rig.magIn && s.rig.slideLocked && s.Qty() == 0 && s.Mags() == 3,
			"R8: the button with the slide locked on the EMPTY magazine is the release: dropped (%s), the slide stays back, word 0, i51 3", b2.why);
		hd.mainButton = true; const Event b3 = s.Tic(false, hd); hd.mainButton = false;
		Check(b3.act == ACT_BUTTON_NONE && s.rig.slideLocked, "R8: the button with no magazine in does nothing (%s)", b3.why);
		hd.offAtPouch = true; hd.offGrip = true; s.Tic(false, hd); hd.offAtPouch = false;
		hd.magAtWell = true; const Event b4 = s.Tic(false, hd); hd.magAtWell = false;
		s.Tic(false, hd); hd.offGrip = false; s.Tic(false, hd);
		Check(b4.act == ACT_SEAT && s.Qty() == 12 && s.Mags() == 2 && s.rig.slideLocked, "R8: a magazine seated: the original's reload (word 12, i51 2), the slide still back");
		const int p0 = s.pulls; s.Tic(true, hd);
		Check(s.pulls == p0, "R6: a pull before the slide closes is a dry click");
		for (int i = 0; i < 200 && w.weapon.lock != 0; i++) s.Tic(false, hd);
		hd.mainButton = true; const Event b5 = s.Tic(false, hd); hd.mainButton = false;
		Check(b5.act == ACT_SLIDE_FORWARD && !s.rig.slideLocked && s.rig.chambered && s.Qty() == 12 && s.Mags() == 2,
			"R8: the button with the slide locked on a magazine WITH rounds is the slide stop: forward, chambered (%s); the game untouched", b5.why);
		s.Shoot(hd);
		Check(s.Qty() == 11 && h.removed51 == 1, "R8: the next pull fires (word 12 -> %d), one i51 consumed in all, as the original's one reload", s.Qty());
		hd.mainButton = true; const Event b6 = s.Tic(false, hd); hd.mainButton = false;
		Check(b6.act == ACT_REFUSE_DROP && s.rig.magIn && s.Qty() == 11, "R8/R1: the button on 11 rounds, slide forward: refused, nothing moves (word 11)");
		Check(MainButton(true, false, 0) == BUTTON_MAG_RELEASE && MainButton(true, true, 0) == BUTTON_MAG_RELEASE && MainButton(true, true, 7) == BUTTON_SLIDE_STOP
			&& MainButton(false, true, 0) == BUTTON_NOTHING && MainButton(false, false, 5) == BUTTON_NOTHING, "R8: the button's table (slide stop only on a locked slide with rounds)");
	}

	// ==== 6. R9: the off hand on the grip (the latch) ====
	{
		const double want[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.02, 0.0, 0.03, 1 };
		auto at = [&](double off, double deg) {   // the hand `off` metres from the place, turned `deg` about Z
			const double r = deg * 3.14159265358979323846 / 180;
			double m[16] = { cos(r), sin(r), 0, 0, -sin(r), cos(r), 0, 0, 0, 0, 1, 0, 0.02 + off, 0.0, 0.03, 1 };
			return SupportPlaceOf(want, m);
		};
		const SupportPlace p0 = at(0, 0), p1 = at(0.04, 30), p2 = at(0.08, 0), p3 = at(0.12, 0), p4 = at(0.0, 60), p5 = at(0, 80);
		Check(fabs(p0.dist) < 1e-12 && fabs(p0.deg) < 1e-6 && fabs(p1.dist - 0.04) < 1e-12 && fabs(p1.deg - 30) < 1e-6, "R9: the place: distance and turn from the fitted frame (%.3f m %.1f deg)", p1.dist, p1.deg);
		SupportGrip g;
		Check(g.Feed(p0, false, 3, false, true) == 0 && !g.engaged, "R9 hold mode: at the grip with the grip open: not taken");
		{ const int ev = g.Feed(p1, true, 3, false, true); Check(ev == 1 && g.engaged, "R9 hold mode: 4 cm and 30 deg off with the grip closed: taken (%s)", g.why); }
		Check(g.Feed(p2, true, 3, false, true) == 0 && g.engaged, "R9: 8 cm off (inside the exit): kept");
		Check(g.Feed(p4, true, 3, false, true) == 0 && g.engaged, "R9: turned 60 deg (inside the exit): kept");
		{ const int ev = g.Feed(p3, true, 3, false, true); Check(ev == -1 && !g.engaged, "R9: 12 cm off: let go (%s)", g.why); }
		Check(g.Feed(p2, true, 3, false, true) == 0 && !g.engaged, "R9: back to 8 cm (outside the entry): not taken again");
		Check(g.Feed(p0, true, 3, false, true) == 1, "R9: at the place again, the grip still closed: taken");
		{ const int ev = g.Feed(p0, false, 3, false, true); Check(ev == -1 && !g.engaged, "R9 hold mode: the grip opens: let go (%s)", g.why); }
		g.Feed(p0, true, 3, false, true);
		{ const int ev = g.Feed(p0, true, 3, true, true); Check(ev == -1 && !g.engaged && g.rearmRequired, "R9: a reload action lets go AT ONCE (%s)", g.why); }
		Check(g.Feed(p0, true, 3, false, true) == 0 && !g.engaged, "R9: after a reload action, not retaken while the hand stays at the grip");
		g.Feed(p3, true, 3, false, true);
		Check(g.Feed(p0, true, 3, false, true) == 1, "R9: the hand left and came back: taken again");
		{ const int ev = g.Feed(p5, true, 3, false, true); Check(ev == -1, "R9: turned 80 deg: let go (%s)", g.why); }
		g.Reset(); g.Feed(p0, true, 3, false, true);
		{ const int ev = g.Feed(p0, true, 3, false, false); Check(ev == -1 && !g.engaged, "R9: the gun or the hand no longer free (a magazine in it, the gun put away): let go (%s)", g.why); }
		SupportGrip t;
		t.Feed(p0, false, 4, false, true);
		Check(t.Feed(p0, true, 4, false, true) == 1 && t.Feed(p0, false, 4, false, true) == 0 && t.Feed(p0, true, 4, false, true) == -1, "R9 toggle mode (vr_support_mode 4): press takes, release keeps, press lets go");
		SupportGrip z;
		Check(z.Feed(p0, true, 0, false, true) == 0 && !z.engaged, "R9: vr_support_mode 0: never taken");
		SupportGrip a1;
		Check(a1.Feed(p0, false, 1, false, true) == 1 && a1.Feed(p3, false, 1, false, true) == -1, "R9 auto mode (1): taken at the place with no grip, let go away from it");
	}

	printf("\n%s: %d passed, %d failed\n", gFails ? "FAIL" : "PASS", gPass, gFails);
	return gFails ? 1 : 0;
}
