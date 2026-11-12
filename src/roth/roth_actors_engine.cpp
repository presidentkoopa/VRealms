//
// The engine side of Realms' actors. The rules (roth_actors.h) decide
// everything; this file feeds them the player, asks them to tick, routes the
// map's commands to them, and moves the engine's actors to where the rules
// put each object -- in the engine's coordinates, which are the Realms ones
// plus the shift of the object's sector when roth_portals has moved it.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_actors_engine.h"
#include "roth_actors.h"
#include "roth_runtime.h"
#include "roth_objects.h"
#include "roth_raw.h"
#include "roth_dbase100.h"
#include "roth_install.h"
#include "roth_log.h"
#include "roth_sound.h"
#include "roth_texture.h"
#include "roth_das.h"

#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "playsim/p_local.h"
#include "printf.h"
#include "c_dispatch.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <math.h>
#include <stdio.h>

namespace roth
{

namespace
{

std::vector<uint8_t> ReadWhole(const std::string &path)
{
	std::vector<uint8_t> v;
	if (path.empty()) return v;
	FILE *f = fopen(path.c_str(), "rb");                 // read only; nothing is ever written
	if (f == nullptr) return v;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0)
	{
		v.resize((size_t)n);
		if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear();
	}
	fclose(f);
	return v;
}

AActor *ThePlayer(FLevelLocals *level)
{
	if (level == nullptr) return nullptr;
	for (int i = 0; i < MAXPLAYERS; i++)
		if (level->PlayerInGame(i) && level->Players[i]->mo != nullptr) return level->Players[i]->mo;
	return nullptr;
}

// THE PICTURES THE ACTORS WEAR, by das id: the map's pack below 0x1200, the
// shared pack from there (Pack::ResolveDasId). Every frame its own texture
// (TextureSet::Frames), because the frame is the game's choice, not a clock's.
struct ArtPacks
{
	std::unique_ptr<TextureSet> shared, level;
	const TextureSet::PictureFrames *Frames(uint16_t das)
	{
		int index = 0;
		const bool fromShared = Pack::ResolveDasId(das & 0x7fff, index);
		TextureSet *t = fromShared ? shared.get() : level.get();
		if (t == nullptr || !t->IsOpen()) return nullptr;
		const TextureSet::PictureFrames &pf = t->Frames(index, nullptr);
		return pf.ok ? &pf : nullptr;
	}
	const Pack *PackFor(uint16_t das, int &index)
	{
		const bool fromShared = Pack::ResolveDasId(das & 0x7fff, index);
		TextureSet *t = fromShared ? shared.get() : level.get();
		return (t != nullptr && t->IsOpen()) ? t->PackData() : nullptr;
	}
};
ArtPacks g_art;

class EngineHost : public ActorHost
{
public:
	ActorWorld *world = nullptr;
	std::map<ObjectUid, AActor *> engineActors;
	std::map<std::string, int> counts;

	int32_t Health() override { return PlayerHealth(); }
	void SetHealth(int32_t v) override { SetPlayerHealth(v); }
	int32_t MaxHealth() override { return PlayerMaxHealth(); }
	int32_t Armour() override { return PlayerArmour(); }
	bool DoorInPool(int sector) override { return RealmsDoorOpen(sector); }
	PictureTiming Timing(uint16_t id) override
	{
		PictureTiming t;
		const TextureSet::PictureFrames *pf = g_art.Frames(id);
		if (pf == nullptr || pf->frames.empty()) return t;
		t.ok = true;
		t.rle = pf->rle;
		t.ticksPerFrame = (uint16_t)pf->ticksPerFrame;
		t.frameCount = (uint16_t)pf->frames.size();
		t.rate = pf->rate;
		return t;
	}
	int32_t PainFlash() override { return PlayerPainFlash(); }
	void SetPainFlash(int32_t v) override { SetPlayerPainFlash(v); }

	void SoundAt(int sound, int x, int y) override
	{
		if (sound >= 0) SoundPlayEntity((uint32_t)sound, (int16_t)x, (int16_t)y);
	}
	void ChainSound(uint16_t sound, int16_t x, int16_t y) override { StageObjectChainSound(sound, x, y); }
	void RunObjectChain(ObjectUid uid) override
	{
		int s = -1, i = -1;
		if (world->Find(uid, &s, &i) != nullptr) FireObjectChain(s, i);
	}
	void PlayerHurt(int damage) override
	{
		counts["player hit by a creature's shot"]++;
		Printf(TEXTCOLOR_RED "Realms: hit for %d, health %d\n", damage, (int)PlayerHealth());
	}

	// Put the engine's actor where the record says, in engine coordinates.
	void ObjectChanged(ObjectUid uid) override
	{
		auto it = engineActors.find(uid);
		if (it == engineActors.end() || it->second == nullptr) { counts["object with no engine actor changed"]++; return; }
		int s = -1, i = -1;
		const Object *o = world->Find(uid, &s, &i);
		if (o == nullptr) return;
		AActor *mo = it->second;
		double dx = 0., dy = 0.;
		SectorShift(s, dx, dy);
		mo->SetOrigin(DVector3(o->x + dx, o->y + dy, (double)o->z), true);
		mo->Angles.Yaw = DAngle::fromDeg(ObjectActorYaw(mo, o->rotation));
		if (o->flags & 0x80) mo->renderflags |= RF_INVISIBLE;     // hidden (renderer.c:8035)
		else mo->renderflags &= ~RF_INVISIBLE;
	}
	void ObjectCreated(ObjectUid uid) override
	{
		// A new record (0x3c, a bullet, an impact picture): the presentation
		// pass gives it an actor and its picture at the end of this tic.
		(void)uid;
		counts["object created at run time"]++;
	}
	void ObjectDestroyed(ObjectUid uid) override
	{
		auto it = engineActors.find(uid);
		if (it == engineActors.end()) return;
		if (it->second != nullptr) it->second->Destroy();
		engineActors.erase(it);
	}
	bool listsChanged = false;
	void ListsChanged() override { listsChanged = true; }

	// After the rules edited the lists: tell roth_objects where every actor's
	// record is now, so the runtime's own commands (rotate, raise, repaint)
	// still find the right actor by (sector, index).
	void RekeyIfNeeded()
	{
		if (!listsChanged) return;
		listsChanged = false;
		std::vector<std::pair<std::pair<int, int>, AActor *>> cur;
		for (const auto &e : engineActors)
		{
			int s = -1, i = -1;
			if (e.second != nullptr && world->Find(e.first, &s, &i) != nullptr)
				cur.push_back(std::make_pair(std::make_pair(s, i), e.second));
		}
		RekeyObjectActors(cur);
	}
};

struct State
{
	bool active = false;
	FLevelLocals *level = nullptr;
	ActorWorld world;
	CreatureTables tables;
	EngineHost host;
	SimpleCollision *collision = nullptr;
	std::vector<uint8_t> sharedPack;
	uint32_t ticks = 0;
	// The presentation: the sprite made for each set of
	// eight views, the art word each loaded actor was built with, and which
	// objects' actors the presentation dresses.
	std::map<std::vector<int>, int> spriteForViews;
	std::map<ObjectUid, uint16_t> loadedArt;
	std::set<ObjectUid> dressed;
	// What each dressed object showed from the player's view last tic, for
	// roth_actors: the das id (bit 15 the mirror) and its frame.
	std::map<ObjectUid, std::pair<uint16_t, int>> wearing;
};

State g_actors;

// The sprite for eight views (or one), each a texture and a mirror bit.
int SpriteFor(const FTextureID *tex, const bool *flip, int views)
{
	std::vector<int> key;
	key.push_back(views);
	for (int v = 0; v < views; v++) { key.push_back(tex[v].GetIndex()); key.push_back(flip[v] ? 1 : 0); }
	auto it = g_actors.spriteForViews.find(key);
	if (it != g_actors.spriteForViews.end()) return it->second;
	const int sn = MakeObjectSprite(tex, flip, views, nullptr);
	g_actors.spriteForViews[key] = sn;
	return sn;
}

// What one object wears now: eight das ids (bit 15 the mirror), one for each
// view, or a single one. False when it is nothing this pass can draw (a 3D
// mesh, a directional record the file does not resolve, no picture at all).
bool PicturesOf(ObjectUid uid, const Object &o, uint16_t ids[8], int &views)
{
	ActorWorld &w = g_actors.world;
	// A creature placeholder, slotted or not (renderer.c:5826-5864, 12700).
	(void)uid;
	if (w.CreaturePictureOf(o, 0, false) != 0)
	{
		for (int v = 0; v < 8; v++) ids[v] = w.CreaturePictureOf(o, v * 2, false);
		views = 8;
		return true;
	}
	const uint16_t das = (uint16_t)(0x1000 + ((o.textureIndex | (o.textureSource << 8)) & 0x3ff));
	int index = 0;
	const Pack *pack = g_art.PackFor(das, index);
	if (pack == nullptr) return false;
	const FatEntry *e = pack->Entry(index);
	if (e == nullptr) return false;
	if (e->kind == EntryKind::Indirection)
	{
		// A directional record: its eight views (renderer.c:5885-5925).
		if (pack->Indirect(index) != IndirectKind::Directional) return false;
		const Directional dir = pack->ReadDirectional(index);
		if (!dir.ok() || dir.count != 8) return false;
		for (int v = 0; v < 8; v++) ids[v] = (uint16_t)((dir.frames[v].entry & 0x7fff) | (dir.frames[v].mirror ? 0x8000 : 0));
		views = 8;
		return true;
	}
	if (e->kind == EntryKind::Object3D || e->kind == EntryKind::Empty) return false;
	ids[0] = das;
	views = 1;
	return true;
}

// Dress one object's actor: the sprite of its current pictures at their
// current frames, its size, and whether it shows.
void Dress(ObjectUid uid, const Object &o, AActor *mo, int playerView)
{
	uint16_t ids[8] = {};
	int views = 0;
	if (!PicturesOf(uid, o, ids, views)) { g_actors.host.counts["object with no picture this pass can draw"]++; return; }
	FTextureID tex[8];
	bool flip[8] = {};
	const TextureSet::PictureFrames *main = nullptr;
	for (int v = 0; v < views; v++)
	{
		const TextureSet::PictureFrames *pf = g_art.Frames(ids[v]);
		if (pf == nullptr || pf->frames.empty()) { g_actors.host.counts["state picture missing"]++; return; }
		const int f = g_actors.world.PictureFrame(o, ids[v]);
		tex[v] = pf->frames[f >= 0 && f < (int)pf->frames.size() ? f : 0];
		flip[v] = (ids[v] & 0x8000) != 0;
		if (v == (views == 8 ? (playerView >> 1) : 0)) { main = pf; g_actors.wearing[uid] = std::make_pair(ids[v], f); }
	}
	const int sn = SpriteFor(tex, flip, views);
	if (sn < 0) return;
	mo->sprite = sn;
	mo->frame = 0;
	if (main != nullptr) mo->Scale = DVector2(main->info.unitsPerPixel, main->info.unitsPerPixel);
	if (o.flags & 0x80) mo->renderflags |= RF_INVISIBLE;     // hidden (renderer.c:8035)
	else mo->renderflags &= ~RF_INVISIBLE;
}

// THE FRAME'S DRAW, once a tic after the tic's rules, whatever the number of
// eyes (ActorWorld::DrawnPass): the original's own visibility walk from the
// player's pose wakes the creatures its renderer would have resolved
// (renderer.c:5826-5840), clears the pain byte the way its resolver does and
// lets the drawn attack, death and impact pictures end their counters
// (AnimationDrawn). Then every picture drawn this frame takes its step, and
// every object the presentation owns is dressed.
void DrawAndPresent(int ticks)
{
	ActorWorld &w = g_actors.world;
	int woke = 0;
	const std::vector<ActorWorld::DrawnObject> drawn = w.DrawnPass(&woke);
	if (woke > 0) Printf("Realms: %d creature(s) woke (the original's view from Realms %d,%d angle %d)\n",
		woke, w.player.x, w.player.y, w.player.angle);

	w.StepDrawnPictures(drawn, ticks);
	std::map<ObjectUid, int> viewOf;
	for (const ActorWorld::DrawnObject &d : drawn) viewOf[d.uid] = d.viewIndex;

	const Map *map = w.MapPtr();
	for (size_t s = 0; s < map->objects.size(); s++)
		for (size_t i = 0; i < map->objects[s].size(); i++)
		{
			const Object &o = map->objects[s][i];
			const ObjectUid uid = w.UidAt((int)s, (int)i);
			auto have = g_actors.host.engineActors.find(uid);
			AActor *mo = have != g_actors.host.engineActors.end() ? have->second : nullptr;
			const bool creature = w.CreaturePictureOf(o, 0, false) != 0;
			if (mo == nullptr)
			{
				// Nothing the loader spawned: a creature placeholder, a prop
				// hidden at load, a record made at run time. It gets an actor
				// the first time it is not hidden.
				if (o.flags & 0x80) continue;
				uint16_t probe[8]; int pv = 0;
				if (!PicturesOf(uid, o, probe, pv)) continue;       // a mesh, or no art: nothing to show
				double dx = 0., dy = 0.;
				SectorShift((int)s, dx, dy);
				mo = SpawnObjectActor(g_actors.level, (int)s, (int)i, o.x + dx, o.y + dy, (double)o.z);
				if (mo == nullptr) { g_actors.host.counts["object actor could not be spawned"]++; continue; }
				mo->Angles.Yaw = DAngle::fromDeg(ObjectActorYaw(mo, o.rotation));
				const uint8_t L = o.light;
				const uint8_t S = (uint8_t)map->sectors[s].light;
				mo->LightLevel = (int16_t)((L != 0 && S != 0) ? (int)(uint8_t)((uint8_t)(L - 0x80) + S) : (int)L);
				g_actors.host.engineActors[uid] = mo;
				g_actors.dressed.insert(uid);
				g_actors.host.counts["object given an actor at run time"]++;
			}
			else if (!g_actors.dressed.count(uid))
			{
				// A loaded actor keeps the loader's art until the record's art
				// changes (0x3a, a death) or it becomes a creature.
				auto la = g_actors.loadedArt.find(uid);
				const uint16_t now = (uint16_t)(o.textureIndex | (o.textureSource << 8));
				if (!creature && la != g_actors.loadedArt.end() && la->second == now) continue;
				g_actors.dressed.insert(uid);
			}
			auto vo = viewOf.find(uid);
			Dress(uid, o, mo, vo != viewOf.end() ? vo->second : 0);
		}
}

void SyncPlayer()
{
	AActor *pmo = ThePlayer(g_actors.level);
	PlayerView &p = g_actors.world.player;
	if (pmo == nullptr) return;
	int sector = pmo->Sector != nullptr ? pmo->Sector->Index() : -1;
	const Map *map = g_actors.world.MapPtr();
	if (sector >= (int)map->sectors.size()) sector = -1;
	double dx = 0., dy = 0.;
	if (sector >= 0) SectorShift(sector, dx, dy);
	p.x = (int16_t)lround(pmo->X() - dx);
	p.y = (int16_t)lround(pmo->Y() - dy);
	const double z = pmo->Z();
	p.z = (int16_t)floor(z);
	p.zFrac = (uint16_t)((z - floor(z)) * 65536.);
	p.angle = (uint16_t)((int)lround((pmo->Angles.Yaw.Degrees() - 90.0) * (512.0 / 360.0)) & 511);
	p.height = pmo->player != nullptr ? (uint16_t)pmo->player->viewheight : 0;
	p.sector = sector;
	p.pitch = p.pitchApplied = 0;                            // key fire (WC:672); hand aim belongs to the weapon work
}

} // namespace

bool ActorsActive() { return g_actors.active; }

int ActorsEquipWeapon(int itemIndex)
{
	ActorWorld &w = g_actors.world;
	const int16_t ammoItem = w.EquipWeapon(itemIndex);
	Printf("Realms: weapon %d equipped: %d action(s), %s\n", itemIndex, w.weapon.slots,
		ammoItem != 0 ? FStringf("ammo item %d", (int)ammoItem).GetChars() : "needs no ammo");
	return ammoItem;
}

void ActorsSetWeaponAmmo(int count)
{
	g_actors.world.SetWeaponAmmo(count);
	Printf("Realms: weapon ammo %d\n", count);
}

void ActorsUnequipWeapon()
{
	// activate_weapon_item(0, 0) (WC:430-441). NOT YET FAITHFUL: with no
	// record the original parses the item at 0x81054 (the primary
	// selection's +0x10) instead; here it is fists with no actions.
	g_actors.world.EquipWeapon(0);
	Printf("Realms: weapon put away (fists)\n");
}
void ActorsDrainConsole();

void ActorsBeginLevel(Map &map, FLevelLocals *level, Log *log)
{
	ActorsEndLevel();
	const Install &inst = TheInstall();
	if (g_actors.sharedPack.empty())
		g_actors.sharedPack = ReadWhole(inst.PackFile(inst.SharedPack().c_str()));
	g_actors.tables = LoadCreatureTables(g_actors.sharedPack.data(), g_actors.sharedPack.size());
	if (!g_actors.tables.ok)
	{
		if (log) log->Warn("actors: creature tables unavailable (%s) -- no actors this level", g_actors.tables.error.c_str());
		return;
	}
	{
		const std::string packName = level != nullptr ? inst.PackFor(level->MapName.GetChars()) : std::string();
		const std::vector<uint8_t> mapPack = ReadWhole(inst.PackFile(packName.c_str()));
		LoadMapDims(g_actors.tables, mapPack.data(), mapPack.size());
	}
	g_actors.level = level;
	g_actors.host = EngineHost();
	g_actors.host.world = &g_actors.world;
	g_actors.world.Begin(&map, &TheDbase100(), &g_actors.tables, &g_actors.host);
	delete g_actors.collision;
	g_actors.collision = new SimpleCollision(&g_actors.world);
	g_actors.world.SetCollision(g_actors.collision);

	// The pictures the actors wear, from both packs (ArtPacks).
	g_actors.world.pictureClocks.clear();
	g_actors.spriteForViews.clear();
	g_actors.loadedArt.clear();
	g_actors.dressed.clear();
	g_actors.wearing.clear();
	g_art.shared.reset(new TextureSet());
	g_art.level.reset(new TextureSet());
	if (!g_art.shared->Open(inst.SharedPack().c_str(), inst.PackFile(inst.SharedPack().c_str()), log) && log)
		log->Warn("actors: the shared pack did not open for creature art: %s", g_art.shared->Error().c_str());
	if (level != nullptr)
	{
		const std::string packName = inst.PackFor(level->MapName.GetChars());
		if (!g_art.level->Open(packName.c_str(), inst.PackFile(packName.c_str()), log) && log)
			log->Warn("actors: the map pack did not open for object art: %s", g_art.level->Error().c_str());
	}

	int props = 0, creatures = 0;
	for (size_t s = 0; s < map.objects.size(); s++)
		for (size_t i = 0; i < map.objects[s].size(); i++)
		{
			const ObjectUid uid = g_actors.world.UidAt((int)s, (int)i);
			if (AActor *mo = FindObjectActor((int)s, (int)i))
			{
				g_actors.host.engineActors[uid] = mo; props++;
				g_actors.loadedArt[uid] = (uint16_t)(map.objects[s][i].textureIndex | (map.objects[s][i].textureSource << 8));
			}
			const Object &o = map.objects[s][i];
			if (g_actors.tables.CreatureIndexForId((uint16_t)(o.textureIndex | (o.textureSource << 8))) >= 0) creatures++;
		}
	g_actors.active = true;
	g_actors.ticks = 0;
	if (log) log->Line("actors: %d creature placeholders, %d objects bound to engine actors, %d creature records",
		creatures, props, g_actors.tables.CreatureCount());
}

void ActorsEndLevel()
{
	g_actors.world.SetCollision(nullptr);
	g_actors.world.End();
	delete g_actors.collision;
	g_actors.collision = nullptr;
	g_actors.host.engineActors.clear();
	g_actors.spriteForViews.clear();
	g_actors.dressed.clear();
	g_actors.wearing.clear();
	g_actors.active = false;
	g_actors.level = nullptr;
}

void ActorsTick(FLevelLocals *level, int ticks)
{
	if (!g_actors.active || level != g_actors.level) return;
	SyncPlayer();
	// Once a frame, before the frame's ticks: the hazard faces hurt
	// (damage_player_from_emitter, weapon_combat.c:72, from tick_world_effects,
	// raw_commands.c:3297). The pain flash's decay is the command core's
	// (CmdCore::DecayDamageFlash), on the same shared value.
	g_actors.world.TickDamageEmitters((uint32_t)ticks);
	for (int t = 0; t < ticks; t++) { g_actors.world.Tick(); g_actors.ticks++; }
	g_actors.world.frameClock = g_actors.ticks;
	g_actors.host.RekeyIfNeeded();
	// After the frame's ticks: what is drawn this frame.
	DrawAndPresent(ticks);
	g_actors.host.RekeyIfNeeded();
	ActorsDrainConsole();
}

int ActorsCommand(uint8_t op, Command &rec, int &activeObjectSector, int &activeObjectIndex,
	int activeFace, bool &interrupt)
{
	interrupt = false;
	if (!g_actors.active) return ACTCMD_NOTHING;
	ActorWorld &w = g_actors.world;
	SyncPlayer();
	w.SetCommandSource(w.UidAt(activeObjectSector, activeObjectIndex));
	w.chainInterrupt = 0;
	int r = ACTCMD_NOTHING;
	switch (op & 0x7f)
	{
	case 0x16: r = w.CmdSpawnObject(rec); break;
	case 0x3a: r = w.CmdChangeObjectId(rec); break;
	case 0x3c: r = w.CmdSpawnObjectAdv(rec); break;
	default: break;
	}
	interrupt = w.chainInterrupt == 2;
	g_actors.host.RekeyIfNeeded();
	// The source object the command left behind is the runtime's active object.
	int s = -1, i = -1;
	if (w.CommandSource() != 0 && w.Find(w.CommandSource(), &s, &i) != nullptr)
	{
		activeObjectSector = s;
		activeObjectIndex = i;
	}
	return r;
}

} // namespace roth

// The console commands. A command typed (or +exec'd) before a Realms level
// is running is QUEUED and run from the level tick, like roth_dialogue: a
// bridge script's commands arrive before the map exists.
namespace roth { namespace {
struct PendingConsole { bool dump = false; int shoot = 0; int atTic = 0; };
std::vector<PendingConsole> g_consoleQueue;
}
void ActorsDrainConsole()
{
	if (!g_actors.active) return;
	std::vector<PendingConsole> q, later;
	q.swap(g_consoleQueue);
	const int now = g_actors.level != nullptr ? g_actors.level->maptime : 0;
	for (const PendingConsole &c : q)
	{
		if (c.atTic > now) { later.push_back(c); continue; }
		if (c.dump)
		{
			Printf("roth_actors: tick %u, health %d/%d, armour %d\n", g_actors.ticks,
				(int)PlayerHealth(), (int)PlayerMaxHealth(), (int)PlayerArmour());
			Printf("%s", g_actors.world.DumpTick().c_str());
			for (const auto &e : g_actors.wearing)
			{
				int s = -1, i = -1;
				if (g_actors.world.Find(e.first, &s, &i) == nullptr) continue;
				Printf("  object %d.%d wears picture 0x%04x%s frame %d\n", s, i, e.second.first & 0x7fff,
					(e.second.first & 0x8000) ? " (mirrored)" : "", e.second.second);
			}
			for (const auto &e : g_actors.host.counts) Printf("  %6d  %s\n", e.second, e.first.c_str());
		}
		else
		{
			SyncPlayer();
			const int slot = g_actors.world.FirePlayer((uint16_t)c.shoot);
			Printf("roth_shoot %d -> %s (tic %d, from Realms %d,%d angle %d)\n", c.shoot,
				slot >= 0 ? "fired" : "no bullet (not a bullet item, or the flying pool is full)",
				now, g_actors.world.player.x, g_actors.world.player.y, g_actors.world.player.angle);
		}
	}
	for (const PendingConsole &c : later) g_consoleQueue.push_back(c);
}
} // namespace roth

// roth_actors [tic]: the actor pools, for the console and the bridge's logs;
// with a tic, at that tic of the level.
CCMD(roth_actors)
{
	roth::PendingConsole c; c.dump = true;
	if (argv.argc() >= 2) c.atTic = atoi(argv[1]);         // roth_actors [level tic]
	roth::g_consoleQueue.push_back(c);
}

// roth_shoot <item> [tic]: fire a DBASE100 bullet item from the player
// straight ahead, as the original's key fire does (weapon_combat.c:616-675
// with no screen aim). A test of the bullet rules. With a tic, not before
// that tic of the level (level maptime), so a bridge script can time it.
CCMD(roth_shoot)
{
	if (argv.argc() < 2 || atoi(argv[1]) <= 0) { Printf("usage: roth_shoot <bullet item id> [level tic]  (for example: roth_shoot 61)\n"); return; }
	roth::PendingConsole c; c.shoot = atoi(argv[1]);
	if (argv.argc() >= 3) c.atTic = atoi(argv[2]);
	roth::g_consoleQueue.push_back(c);
}
