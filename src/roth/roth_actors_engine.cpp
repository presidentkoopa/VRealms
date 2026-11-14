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
#include "roth_interface.h"
#include "roth_inventory.h"

#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "playsim/p_local.h"
#include "printf.h"
#include "c_dispatch.h"
#include "d_event.h"
#include "r_data/models.h"
#include "model_memorymesh.h"

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
std::vector<uint8_t> g_dbase200;   // read only; the weapon pictures (load_dbase200_sprite_cached)

// The frame records of DBASE200 picture `sprite` (das_assets.c:495-512: at
// sprite * 8 a size dword, then the payload), or null.
const uint8_t *WeaponPicture(int32_t sprite, uint32_t &size)
{
	size = 0;
	const size_t at = (size_t)(uint32_t)sprite * 8;
	if (sprite <= 0 || at + 4 > g_dbase200.size()) return nullptr;
	const uint8_t *b = g_dbase200.data() + at;
	size = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	if (size == 0 || at + 4 + size > g_dbase200.size()) { size = 0; return nullptr; }
	return b + 4;
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
	bool DoorInPool(int sector) override { return RealmsDoorOpen(sector); }
	// ---- the weapon's outside world: the game core's inventory ----
	int SlotQuantity(int slot) override
	{
		Inventory *inv = InterfaceInventory();
		return (inv != nullptr && slot >= 0 && slot < Inventory::SLOT_COUNT) ? inv->Slot(slot).quantity : 0;
	}
	void SetSlotQuantity(int slot, int q) override
	{
		if (Inventory *inv = InterfaceInventory()) inv->SetQuantity(slot, (uint16_t)q);
	}
	void RemoveItem(int16_t id) override
	{
		if (Inventory *inv = InterfaceInventory()) inv->RemoveItem(TheDbase100(), (uint16_t)id, InterfaceInventorySink());
	}
	void NoAmmoMessage(int msg) override
	{
		// show_no_ammo_message (WC:466): DBASE100 text 5 or 6 as a timed line.
		Printf("Realms: %s\n", msg == 5 ? "the weapon is out of charge" : "no ammunition");
	}
	void EquipFirstUsableWeapon() override;
	void ConsumeHeldWeapon() override
	{
		// player.c:506-509: the held item used up, then activate_weapon_item(0, 0).
		Inventory *inv = InterfaceInventory();
		if (inv == nullptr) return;
		const int slot = world->weapon.heldSlot;
		if (slot >= 0) inv->ConsumeSlot(TheDbase100(), slot, InterfaceInventorySink());
		inv->SetLeftHand(-1);
		ActorsUnequipWeapon((int)inv->AdamItemId());
	}
	void WeaponSound(int id) override { SoundPlayEffect((uint32_t)id, 0xfcff); }   // WC:885
	// The weapon panel's redraws (WC:812, 820, 874, 979): the HUD's.
	void HudWeaponView(int arg) override { InterfaceHudWeaponView(arg); }
	void HudWeaponPanel() override { InterfaceHudWeaponPanel(); }
	int WeaponFrames(int32_t sprite) override
	{
		uint32_t size = 0;
		const uint8_t *fr = WeaponPicture(sprite, size);
		if (fr == nullptr) return 0;
		return RunLengthRepairedCount(fr, size, 0, size, fr[8] | (fr[9] << 8));   // das_assets.c:510
	}

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
	// The held weapon's actor (RealmsHeld) and the cards built for weapons with
	// no model: (picture, frame, vr) -> sprite bound to a card mesh.
	AActor *held = nullptr;
	std::map<std::pair<std::pair<int32_t, int>, std::pair<bool, const PClass *>>, int> cards;
	int lastPull = -1, lastShot = -1, lastAmmo = -1, lastReload = -1, lastNoAmmo = -1, lastEquip = -1, lastCycleEnd = -1;
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

// THE WEAPON, once a tic after the tic's ticks (the original's frame runs
// update_player_tick after tick_dynamic_entities): the trigger from the fire
// button (BT_ATTACK -- in VR the main-hand trigger), the frame step with the
// shot aimed from the hand ray in VR and from the view in flat, the recharge.
// Everything it reads comes through the usercmd: the button and, in VR,
// AttackAngle/AttackPitch, which the engine rebuilds from it on every peer.
// THE VR HANDS AND THE ATTACK (owner, 2026-11-13: the trigger examines on either
// hand and never sends attack unless that hand holds a drawn weapon; "physical
// melee ... physical only"). The fire button (BT_ATTACK, in VR the main hand's
// trigger through the binding) reaches the weapon only while it is allowed
// (roth_vrpick.cpp VRPickTick: always in flat; in VR only while the main hand
// holds a weapon). The hands' own attack is A FIST THAT LANDS (roth_gestures.cpp,
// RSQ Melee's punch reader): where the original's cursor on what it met is an
// attack target, the original's secondary click there -- trigger_weapon_fire
// aimed at the cursor (game_core.c:673-685; roth_handclick.h) -- with the fist's
// CONTACT as the aim: one TriggerFire at the next weapon tick, and the shot's
// aim (PunchAimAt, roth_actors.h) held on that point until the shot leaves or
// the fire cycle ends, as the original latches its aim at the trigger
// (WC:893-896, the aim words read at the shot).
static bool g_fireButtonAllowed = true;
struct FireAt { bool pending = false, holding = false; double x = 0, y = 0, z = 0; };
static FireAt g_fireAt;

void WeaponTick(int ticks)
{
	ActorWorld &w = g_actors.world;
	AActor *pmo = ThePlayer(g_actors.level);
	if (pmo == nullptr || pmo->player == nullptr) return;
	const bool vr = pmo->OverrideAttackPosDir;
	const DAngle aimYaw = vr ? pmo->AttackAngle : pmo->Angles.Yaw;
	const DAngle aimPitch = vr ? pmo->AttackPitch : pmo->Angles.Pitch;
	// The original's view pitch: p looks up at atan(p / 128), +-126 (0x90a74).
	double t = -aimPitch.Tan() * 128.0;
	if (t > 126.0) t = 126.0;
	if (t < -126.0) t = -126.0;
	w.player.pitch = (int32_t)lround(t);
	// spawn_projectile_from_aim takes its screen aim as an offset subtracted
	// from the player's angle (WC:620-634); the hand's own yaw, as that offset.
	const int handAngle = (int)lround((aimYaw.Degrees() - 90.0) * (512.0 / 360.0)) & 511;
	int32_t yawOffset = ((int)w.player.angle - handAngle) & 511;
	if (yawOffset >= 256) yawOffset -= 512;
	const bool button = (pmo->player->cmd.buttons & BT_ATTACK) && g_fireButtonAllowed;
	// A FIST THAT LANDED (ActorsRequestFireAt): the original's aimed attack, its aim the contact.
	if (g_fireAt.pending)
	{
		g_fireAt.pending = false;
		const bool ready = w.weapon.lock == 0 && w.weapon.slots != 0;   // TriggerFire's own first gates (WC:781-783)
		w.TriggerFire(0, 0);
		g_fireAt.holding = ready && w.weapon.lock != 0;
		Printf("[roth_gestures] the fist's attack: trigger_weapon_fire %s\n", g_fireAt.holding ? "pulled, aimed at the contact"
			: "refused by the original's weapon (still in its last swing, or no hands)");
	}
	else if (button) w.TriggerFire(0, 0);                  // key_fire_weapon (WC:912)
	if (g_fireAt.holding)
	{
		double dx = 0, dy = 0;
		if (w.player.sector >= 0) SectorShift(w.player.sector, dx, dy);
		const PunchAim a = PunchAimAt(w.player, (int16_t)lround(g_fireAt.x - dx), (int16_t)lround(g_fireAt.y - dy), (int32_t)lround(g_fireAt.z));
		yawOffset = a.yawOffset;
		w.player.pitch = a.pitch;
	}
	// This frame's own events (the tic's may already hold some): set aside, read, given back.
	const uint32_t evBefore = w.weaponEvents;
	w.weaponEvents = 0;
	w.WeaponFrame(ticks, yawOffset);
	const uint32_t evFrame = w.weaponEvents;
	w.weaponEvents |= evBefore;
	if (g_fireAt.holding && ((evFrame & (ActorWorld::WE_SHOT | ActorWorld::WE_CYCLE_END)) || w.weapon.lock == 0))
	{
		g_fireAt.holding = false;
		Printf("[roth_gestures] the fist's attack: %s\n", (evFrame & ActorWorld::WE_SHOT) ? "the shot left, aimed at the contact" : "the swing ended");
	}
	w.RechargeFrame(ticks);
	w.player.pitch = 0;
}

// A card for a weapon with no model: the original's picture, frame `frame`,
// as a quad mesh bound to `cls`. In VR centred on the picture (it rides the
// hand); flat, laid out on the original's 320 x 200 screen around its centre,
// so the card in front of the eye stands where the original drew it.
int WeaponCard(const PClass *cls, int32_t sprite, int frame, bool vr)
{
	const auto key = std::make_pair(std::make_pair(sprite, frame), std::make_pair(vr, cls));
	auto it = g_actors.cards.find(key);
	if (it != g_actors.cards.end()) return it->second;
	int sn = -1;
	uint32_t size = 0;
	const uint8_t *fr = WeaponPicture(sprite, size);
	ViewmodelFrame vf;
	if (fr != nullptr) vf = DecodeViewmodelFrame(fr, size, frame);
	if (vf.ok && g_art.level && g_art.level->IsOpen())
	{
		const FString texName = FStringf("ROTH_WPN_%X_%02d", (unsigned)sprite, frame);
		const FTextureID tex = g_art.level->LoosePicture(texName.GetChars(), vf.pixels, vf.w, vf.h);
		if (tex.isValid())
		{
			FMemoryMeshData m;
			const double u = vr ? 0.06 : 0.1;                // world units a texel
			const double cx = vr ? vf.x + vf.w * 0.5 : 160.0, cy = vr ? vf.y + vf.h * 0.5 : 100.0;
			const double x0 = vf.x - cx, x1 = vf.x + vf.w - cx, y0 = vf.y - cy, y1 = vf.y + vf.h - cy;
			// Model space: x, y up, z. VR: the hand's frame is X right, Y up,
			// forward -Z, so the card stands in x-y. Flat: the actor faces the
			// view, model +x is forward and +z is left, so the card stands in y-z.
			auto V = [&](double px, double py) {
				return vr ? FVector3((float)(px * u), (float)(-py * u), 0.f)
				          : FVector3(0.f, (float)(-py * u), (float)(-px * u));
			};
			m.vertices.Push(V(x0, y0)); m.vertices.Push(V(x1, y0)); m.vertices.Push(V(x1, y1)); m.vertices.Push(V(x0, y1));
			FMemoryMeshFace f;
			f.count = 4;
			f.vertex[0] = 0; f.vertex[1] = 1; f.vertex[2] = 2; f.vertex[3] = 3;
			f.uv[0] = FVector2(0.f, 0.f); f.uv[1] = FVector2(1.f, 0.f); f.uv[2] = FVector2(1.f, 1.f); f.uv[3] = FVector2(0.f, 1.f);
			f.skin = tex;
			m.faces.Push(f);
			m.doubleSided = true;
			const FString mkey = FStringf("weaponcard/%s/%X/%d/%d", cls->TypeName.GetChars(), (unsigned)sprite, frame, vr ? 1 : 0);
			sn = BindMemoryMesh(mkey.GetChars(), std::move(m), const_cast<PClassActor *>(static_cast<const PClassActor *>(cls)), nullptr);
		}
	}
	g_actors.cards[key] = sn;
	return sn;
}

// THE HELD WEAPON IN THE WORLD. One RealmsHeld actor (RealmsWeapon<index>
// when that class exists), told every tic what the original's weapon is
// doing, and drawn at the main hand in VR, in front of the eye in flat.
void PresentHeld()
{
	ActorWorld &w = g_actors.world;
	AActor *pmo = ThePlayer(g_actors.level);
	if (pmo == nullptr || g_actors.level == nullptr) return;
	const ActorWorld::WeaponPresentation pr = w.PresentWeapon();
	const uint32_t ev = w.weaponEvents;
	w.weaponEvents = 0;
	const int now = g_actors.level->maptime;
	if (ev & ActorWorld::WE_PULL) g_actors.lastPull = now;
	if (ev & ActorWorld::WE_SHOT) g_actors.lastShot = now;
	if (ev & ActorWorld::WE_AMMO) g_actors.lastAmmo = now;
	if (ev & ActorWorld::WE_RELOAD) g_actors.lastReload = now;
	if (ev & ActorWorld::WE_NOAMMO) g_actors.lastNoAmmo = now;
	if (ev & ActorWorld::WE_EQUIP) g_actors.lastEquip = now;
	if (ev & ActorWorld::WE_CYCLE_END) g_actors.lastCycleEnd = now;

	PClassActor *cls = PClass::FindActor(FName(FStringf("RealmsWeapon%d", (int)pr.item).GetChars()));
	if (cls == nullptr) cls = PClass::FindActor("RealmsHeld");
	if (cls == nullptr) return;
	AActor *&mo = g_actors.held;
	if (mo != nullptr && (mo->ObjectFlags & OF_EuthanizeMe)) mo = nullptr;
	if (mo != nullptr && mo->GetClass() != cls) { mo->Destroy(); mo = nullptr; }
	if (mo == nullptr) mo = AActor::StaticSpawn(g_actors.level, cls, pmo->Pos(), NO_REPLACE);
	if (mo == nullptr) return;

	mo->IntVar(FName("RealmsItem")) = pr.item;
	mo->IntVar(FName("RealmsState")) = pr.state;
	mo->IntVar(FName("RealmsFrame")) = pr.frame;
	mo->IntVar(FName("RealmsFrameCount")) = pr.frameCount;
	mo->IntVar(FName("RealmsStateTics")) = (int)pr.ticksInState;
	mo->IntVar(FName("RealmsStateLength")) = (int)pr.stateLength;
	mo->FloatVar(FName("RealmsProgress")) = pr.progress;
	mo->IntVar(FName("RealmsCharge")) = pr.charge;
	mo->IntVar(FName("RealmsChargeFull")) = pr.chargeFull;
	mo->IntVar(FName("RealmsPullTic")) = g_actors.lastPull;
	mo->IntVar(FName("RealmsShotTic")) = g_actors.lastShot;
	mo->IntVar(FName("RealmsAmmoTic")) = g_actors.lastAmmo;
	mo->IntVar(FName("RealmsReloadTic")) = g_actors.lastReload;
	mo->IntVar(FName("RealmsNoAmmoTic")) = g_actors.lastNoAmmo;
	mo->IntVar(FName("RealmsEquipTic")) = g_actors.lastEquip;
	mo->IntVar(FName("RealmsCycleEndTic")) = g_actors.lastCycleEnd;

	const bool vr = pmo->OverrideAttackPosDir;
	const bool hidden = pr.state == ActorWorld::WS_AWAY || pr.item == 0 || pr.sprite == 0 || pmo->health <= 0;
	if (hidden) { mo->renderflags |= RF_INVISIBLE; return; }
	mo->renderflags &= ~RF_INVISIBLE;

	// The modder's mesh when there is one: the class's own sprite, frame letter
	// = the original's frame. Otherwise the card.
	const int frame = pr.frame < 0 ? 0 : (pr.frame > 28 ? 28 : pr.frame);
	const AActor *def = GetDefaultByType(cls);
	const int stock = (def != nullptr && def->SpawnState != nullptr) ? def->SpawnState->sprite : -1;
	if (stock >= 0 && FindModelFrame(cls, stock, frame, false) != nullptr) { mo->sprite = stock; mo->frame = (uint8_t)frame; }
	else
	{
		const int card = WeaponCard(cls, pr.sprite, pr.frame, vr);
		if (card < 0) { mo->renderflags |= RF_INVISIBLE; g_actors.host.counts["held weapon with no model and no picture"]++; return; }
		mo->sprite = card;
		mo->frame = 0;
	}

	if (vr)
	{
		// FollowHandMode 1: the model rides the main controller at draw rate.
		// The actor stands at the shooting point so it is culled with the hand.
		mo->FollowHandMode = 1;
		mo->SetOrigin(pmo->AttackPos, true);
		mo->Angles.Yaw = pmo->AttackAngle;
	}
	else
	{
		// In front of the eye, where the original's 320 x 200 picture stands:
		// the card is laid out about the screen's centre, and the view's bob
		// and sway (the original's draw point) move it.
		mo->FollowHandMode = 0;
		const double u = 0.1, dist = 16.0;
		const DAngle yaw = pmo->Angles.Yaw, pitch = pmo->Angles.Pitch;
		const DVector3 fwd(yaw.Cos() * pitch.Cos(), yaw.Sin() * pitch.Cos(), -pitch.Sin());
		const DVector3 left(-yaw.Sin(), yaw.Cos(), 0.0);
		const DVector3 up = fwd ^ left;
		const double eyeZ = pmo->player->viewz;
		const DVector3 eye(pmo->X(), pmo->Y(), eyeZ);
		const DVector3 at = eye + fwd * dist - left * (pr.x * u) - up * (pr.y * u);
		mo->SetOrigin(at, true);
		mo->Angles.Yaw = yaw;
		mo->Angles.Pitch = pitch;
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
uint32_t ActorsLastMintedId() { return g_actors.world.lastMintedId; }

int ActorsEquipWeapon(int slot, int itemIndex)
{
	ActorWorld &w = g_actors.world;
	const int16_t ammoItem = w.EquipWeapon(itemIndex, slot);
	Printf("Realms: weapon %d equipped from slot %d: %d action(s), %s\n", itemIndex, slot, w.weapon.slots,
		ammoItem != 0 ? FStringf("ammo item %d", (int)ammoItem).GetChars() : "no ammo item");
	return ammoItem;
}

void ActorsSetWeaponAmmo(int count)
{
	g_actors.world.SetWeaponAmmo(count);
}

void ActorsUnequipWeapon(int adamItem)
{
	// activate_weapon_item(0, 0) (WC:430-441): no slot, and the item parsed is
	// the one latched at 0x81054 -- Adam's own, whose actions are his hands.
	g_actors.world.EquipWeapon(adamItem, -1);
	Printf("Realms: weapon put away (Adam's item %d, %d action(s))\n", adamItem, g_actors.world.weapon.slots);
}

void ActorsHudWeapon(int32_t &item, int32_t &ammo, int32_t &ammoCap, int32_t &panel, int &equips)
{
	const ActorWorld::PlayerWeapon &w = g_actors.world.weapon;
	item = w.item;
	ammo = w.ammo;
	ammoCap = w.ammoCap;
	panel = w.actions[0].D(0x48);
	equips = w.equips;
}

void ActorsSetHudPanel(int32_t panel) { g_actors.world.weapon.actions[0].SetD(0x48, panel); }
int32_t ActorsActiveAmmoId() { return g_actors.world.weapon.actions[0].D(0x30); }
void ActorsAddWeaponAmmo(int delta) { g_actors.world.weapon.ammo += delta; }

// Outside the anonymous namespace: declared in roth_actors_engine.h (the MSVC link trap).
void ActorsSetFireButtonAllowed(bool allowed) { g_fireButtonAllowed = allowed; }
void ActorsRequestFireAt(double x, double y, double z) { g_fireAt.pending = true; g_fireAt.x = x; g_fireAt.y = y; g_fireAt.z = z; }
int ActorsTickCount() { return g_actors.ticks; }

bool ActorsHeldWeaponDrawn(int32_t &item, const char *&cls)
{
	item = 0;
	cls = nullptr;
	AActor *mo = g_actors.held;
	if (mo == nullptr || (mo->ObjectFlags & OF_EuthanizeMe) || (mo->renderflags & RF_INVISIBLE) || mo->FollowHandMode != 1) return false;
	item = mo->IntVar(FName("RealmsItem"));
	cls = mo->GetClass()->TypeName.GetChars();
	return item != 0;
}

void ActorsRebuildWeaponList()
{
	Inventory *inv = InterfaceInventory();
	if (inv == nullptr) return;
	std::vector<std::pair<int, uint16_t>> used;
	for (int i = 0; i < Inventory::SLOT_COUNT && (int)used.size() < inv->Count(); i++)
		if (inv->Slot(i).id != 0) used.push_back(std::make_pair(i, inv->Slot(i).id));
	g_actors.world.RebuildWeaponList(used);
}

// equip_first_usable_weapon (WC:1379-1435).
void EngineHost::EquipFirstUsableWeapon()
{
	Inventory *inv = InterfaceInventory();
	if (inv == nullptr) return;
	std::vector<ActorWorld::CarriedSlot> carried;
	for (int i = 0; i < Inventory::SLOT_COUNT && (int)carried.size() < inv->Count(); i++)
		if (inv->Slot(i).id != 0) carried.push_back({ i, inv->Slot(i).id, inv->Slot(i).quantity });
	auto count = [&](int16_t id) { return inv->Query(TheDbase100(), (uint16_t)id, 0, nullptr); };
	int id = 0;
	const int slot = world->FindUsableWeapon(carried, count, id);
	if (slot < 0) { inv->SetLeftHand(-1); ActorsUnequipWeapon((int)inv->AdamItemId()); InterfaceHudRenderWeaponHud(); return; }   // reset_weapon_hud (WC:933-935)
	if (slot == world->weapon.heldSlot) return;
	if (slot == inv->RightHand()) inv->SetRightHand(-1, InterfaceInventorySink());
	world->ResetWeaponFireTiming();
	inv->SetLeftHand(slot);                                  // 0x81038 = the slot (WC:1427)
	const int ammo = ActorsEquipWeapon(slot, id);
	if (ammo != 0) ActorsSetWeaponAmmo(inv->Query(TheDbase100(), (uint16_t)ammo, 0, InterfaceInventorySink()));
	InterfaceHudRenderWeaponHud();                           // WC:1429
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
	if (g_dbase200.empty())
	{
		const std::string pics = inst.DataFile("DBASE200.DAT");
		if (!pics.empty()) g_dbase200 = ReadWhole(pics);
	}
	g_actors.cards.clear();
	g_actors.held = nullptr;
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
	g_actors.cards.clear();
	g_actors.held = nullptr;
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
	WeaponTick(ticks);
	g_actors.host.RekeyIfNeeded();
	// After the frame's ticks: what is drawn this frame.
	DrawAndPresent(ticks);
	PresentHeld();
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
struct PendingConsole { bool dump = false; int shoot = 0; int atTic = 0; int give = 0; bool trigger = false; bool weapon = false; };
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
		if (c.give != 0)
		{
			Inventory *inv = InterfaceInventory();
			const int32_t r = inv != nullptr ? inv->GiveItem(TheDbase100(), c.give, nullptr, InterfaceInventorySink()) : 0;
			Printf("roth_give %d -> %s (tic %d)\n", c.give, r ? "given" : "not given", now);
			continue;
		}
		if (c.trigger)
		{
			g_actors.world.TriggerFire(0, 0);
			Printf("roth_trigger (tic %d): %s", now, g_actors.world.DumpWeapon().c_str());
			continue;
		}
		if (c.weapon)
		{
			const ActorWorld::WeaponPresentation pr = g_actors.world.PresentWeapon();
			static const char *names[] = { "Away", "Raise", "Ready", "Fire", "Reload", "Lower" };
			Printf("roth_weapon (tic %d): item %d %s frame %d/%d picture %x, %u of %u ticks (%.2f), drawn at %d,%d, charge %d/%d\n  %s",
				now, (int)pr.item, names[pr.state], pr.frame, pr.frameCount, (unsigned)pr.sprite, pr.ticksInState, pr.stateLength,
				pr.progress, pr.x, pr.y, pr.charge, pr.chargeFull, g_actors.world.DumpWeapon().c_str());
			if (g_actors.held != nullptr)
				Printf("  held actor %s sprite %d frame %d at %.0f,%.0f,%.0f%s%s\n", g_actors.held->GetClass()->TypeName.GetChars(),
					g_actors.held->sprite, g_actors.held->frame, g_actors.held->X(), g_actors.held->Y(), g_actors.held->Z(),
					(g_actors.held->renderflags & RF_INVISIBLE) ? " (hidden)" : "", g_actors.held->FollowHandMode == 1 ? " (rides the main hand)" : "");
			continue;
		}
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

// roth_give <DBASE100 index> [tic]: give_item, as the original's scripts do
// (a weapon equips itself). roth_trigger [tic]: pull the trigger once.
// roth_weapon [tic]: what the weapon is doing and what is drawn for it.
CCMD(roth_give)
{
	if (argv.argc() < 2 || atoi(argv[1]) <= 0) { Printf("usage: roth_give <DBASE100 index> [level tic]\n"); return; }
	roth::PendingConsole c; c.give = atoi(argv[1]);
	if (argv.argc() >= 3) c.atTic = atoi(argv[2]);
	roth::g_consoleQueue.push_back(c);
}
CCMD(roth_trigger)
{
	roth::PendingConsole c; c.trigger = true;
	if (argv.argc() >= 2) c.atTic = atoi(argv[1]);
	roth::g_consoleQueue.push_back(c);
}
CCMD(roth_weapon)
{
	roth::PendingConsole c; c.weapon = true;
	if (argv.argc() >= 2) c.atTic = atoi(argv[1]);
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
