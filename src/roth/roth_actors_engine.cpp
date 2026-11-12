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

#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "playsim/p_local.h"
#include "printf.h"
#include "c_dispatch.h"

#include <map>
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
		// A new record (0x3c, a bullet, an impact picture). The objects
		// loader builds art only for what the map places, so a record whose
		// picture it never built cannot be drawn yet. Counted, never hidden.
		(void)uid;
		counts["object created with no engine art yet"]++;
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
};

State g_actors;

// The renderer's first-draw wake (renderer.c:5826-5840) evaluated ONCE A TIC
// for the player, never once an eye. The original wakes a creature the first
// time its picture is rasterised. The engine has no per-object "was
// rasterised"; the nearest it keeps is the subsector's drawn mark (set by the
// BSP walk the moment the subsector is first rendered, hw_bsp.cpp:614-634),
// which is per subsector and never cleared. So: a creature wakes on the first
// tic its subsector has been drawn AND it stands inside the player's field of
// view. That is the original's rule at the engine's resolution; where it can
// differ is a creature inside a drawn subsector but hidden behind a pillar.
void WakeVisibleCreatures()
{
	AActor *pmo = ThePlayer(g_actors.level);
	if (pmo == nullptr || pmo->player == nullptr) return;
	Map *map = g_actors.world.MapPtr();
	const double fov = pmo->player->FOV > 0 ? pmo->player->FOV : 90.;
	const double half = fov * 0.5 + 10.;                   // the picture's own width
	for (size_t s = 0; s < map->objects.size(); s++)
		for (size_t i = 0; i < map->objects[s].size(); i++)
		{
			const Object &o = map->objects[s][i];
			if ((o.flags & 0x80) || o.unk0C != 0) continue;
			const uint16_t id = (uint16_t)(o.textureIndex | (o.textureSource << 8));
			if (g_actors.tables.CreatureIndexForId(id) < 0) continue;
			double dx = 0., dy = 0.;
			SectorShift((int)s, dx, dy);
			const DVector2 pos(o.x + dx, o.y + dy);
			subsector_t *ss = g_actors.level->PointInRenderSubsector(pos);
			if (ss == nullptr || !(ss->flags & SSECMF_DRAWN)) continue;
			const DVector2 d = pos - pmo->Pos().XY();
			const double ang = fabs(deltaangle(pmo->Angles.Yaw, d.Angle()).Degrees());
			if (ang > half) continue;
			if (g_actors.world.WakeOnFirstDraw(g_actors.world.UidAt((int)s, (int)i)))
				Printf("Realms: creature %zu.%zu (art 0x%04x) woke\n", s, i, id);
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

	int props = 0, creatures = 0;
	for (size_t s = 0; s < map.objects.size(); s++)
		for (size_t i = 0; i < map.objects[s].size(); i++)
		{
			const ObjectUid uid = g_actors.world.UidAt((int)s, (int)i);
			if (AActor *mo = FindObjectActor((int)s, (int)i)) { g_actors.host.engineActors[uid] = mo; props++; }
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
	g_actors.host.RekeyIfNeeded();
	// After the frame's ticks: what is drawn this frame.
	WakeVisibleCreatures();
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

// roth_actors: the actor pools, for the console and the bridge's logs.
CCMD(roth_actors)
{
	using namespace roth;
	if (!g_actors.active) { Printf("roth_actors: no Realms level\n"); return; }
	Printf("roth_actors: tick %u, health %d/%d, armour %d\n", g_actors.ticks,
		(int)PlayerHealth(), (int)PlayerMaxHealth(), (int)PlayerArmour());
	Printf("%s", g_actors.world.DumpTick().c_str());
	for (const auto &c : g_actors.host.counts) Printf("  %6d  %s\n", c.second, c.first.c_str());
}

// roth_shoot <item>: fire a DBASE100 bullet item from the player straight
// ahead, as the original's key fire does (weapon_combat.c:616-675 with no
// screen aim). A test of the bullet rules until the weapons are bound.
CCMD(roth_shoot)
{
	using namespace roth;
	if (!g_actors.active || argv.argc() < 2) { Printf("usage: roth_shoot <bullet item id>\n"); return; }
	SyncPlayer();
	const int slot = g_actors.world.FirePlayer((uint16_t)atoi(argv[1]));
	Printf("roth_shoot %s -> %s\n", argv[1], slot >= 0 ? "fired" : "no bullet (not a bullet item, or the pool is full)");
}
