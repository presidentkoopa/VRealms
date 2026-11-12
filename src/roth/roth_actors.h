#pragma once
//
// Realms of the Haunting's ACTORS: the object records of a map once the game is
// running, the sixteen-slot actor pool that makes some of them monsters, the
// sixteen-slot pool of flying things (bullets, and their impact pictures), the
// commands that create, hide, kill and rename objects, and the player's health.
//
// Engine-free, like roth_inventory and roth_dbase100_interp: plain structs and
// integer rules, built and tested on its own (tools/actors_test) against the
// running original. The engine side (roth_actors_engine.cpp) only supplies what
// the rules ask for through ActorHost and shows what they decide.
//
// WHERE EVERY RULE COMES FROM. ROTH.C, roth_c/src/engine/. Abbreviations used
// in the citations: EA = entity_ai.c, RC = raw_commands.c, WC = weapon_combat.c,
// R = renderer.c, DAS = das_assets.c, DB = dbase100.c, INV = inventory.c,
// CP = collision_physics.c, PL = player.c, GC = game_core.c.
//
// THE OBJECT RECORD (16 bytes, RC:317 resolve_command_objects, EA:808
// spawn_entity_at_position, RC:4108). It is roth::Object, field for field:
//
//   +0x00 x            +0x02 y
//   +0x04 id           the art id (textureIndex | textureSource << 8). For a
//                      creature it is ALSO the key its statistics are found by:
//                      the first DBASE100 item whose +2 word equals it (EA:868,
//                      R:10659).
//   +0x06 rotation     256 per turn
//   +0x07 flags        0x80 hidden (never drawn, R:8035)   0x40 frozen/emerging
//                      0x02 set on every flying thing (EA:827)
//   +0x08 light
//   +0x09 renderType   bit 0x80 fixed-angle is authored; the low bits are RUN-TIME:
//                      0x01 has an actor-pool slot   0x02 has a flying-pool slot
//                      0x20 / 0x08 / 0x04 a trigger of type 0x1b / 0x30 / 0x39
//                      names this object's command id (RC:456)
//   +0x0a z            absolute
//   +0x0c link         0, or the pool back-reference: slot*0x22+1 (actor pool,
//                      EA:887) or slot*0x1c+1 (flying pool, EA:824). Zeroed for
//                      every object at map load (RC:3403).
//   +0x0e commandID
//
// The records live in per-sector lists; a list keeps its order, a removed record
// closes the gap and a new one is appended (RC:3703, RC:3799). roth::Map::objects
// is exactly that, so it IS the store here -- there is no second copy to keep in
// step with the one the other commands already read and write.
//
// Because a record changes index when an earlier one in its list is removed, and
// changes list when it crosses into another sector (RC:3849), every record also
// carries a UID that never changes: ObjectUid. The engine side hangs its actor on
// the uid.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <stdint.h>
#include <vector>
#include <string>

namespace roth
{

struct Map;
struct Object;
struct Command;
class Dbase100;

// Never 0. Objects present at load get (sector << 16 | index) + 1 of where they
// started; objects created later get 0x80000000 upward.
typedef uint32_t ObjectUid;

//==========================================================================
//
// The tables the shared art pack (ADEMO.DAS) carries for creatures.
// DAS:367 load_ademo_das_file.
//
//==========================================================================

struct CreatureTables
{
	// The shared pack's FAT: 8 bytes an entry, +6 flags_1 ('$' = 0x24 is a
	// creature), +7 flags_2 (the creature record index). DAS:208-215.
	std::vector<uint8_t> fat;
	// 0x68 bytes a creature: the picture ids for each state (docs/GAME_combat.md
	// 4.2). The original also keeps two run-time words in each record; those are
	// in ActorWorld::CreatureRuntime, not written back here.
	std::vector<uint8_t> records;
	// Collision dimensions by object id, 4 bytes each: +0 height, +2 radius
	// (EA:255-262, R:10709). Ids 0x200.. come from the shared pack (DAS:401);
	// ids below 0x200 from the map's own pack (map_load.c:382).
	std::vector<uint8_t> dims;

	bool ok = false;
	std::string error;

	int CreatureCount() const { return (int)(records.size() / 0x68); }
	// -1 unless the id's FAT entry is a creature placeholder.
	int CreatureIndexForId(uint16_t id) const;
	uint16_t CreatureWord(int creature, int off) const;
	uint16_t DimHeight(uint16_t id) const;
	uint16_t DimRadius(uint16_t id) const;
};

// Read the three tables out of ADEMO.DAS held in memory.
CreatureTables LoadCreatureTables(const uint8_t *ademo, size_t size);
// The map pack's 0x800-byte collision section, for ids below 0x200.
void LoadMapDims(CreatureTables &t, const uint8_t *mapDas, size_t size);

//==========================================================================
//
// A creature's statistics: the 0x6c-byte record build_entity_def_record makes
// from an item's AsMonster block (DB:117-180). Kept as the same bytes at the
// same offsets, because the builder appends to its lists without a bound and
// the only faithful model of that is the buffer it overruns into.
//
//==========================================================================

struct ActorDef
{
	uint8_t b[0x6c];
	bool valid = false;        // false: the id has no item, or the item no AsMonster block

	uint16_t W(int off) const { return (uint16_t)(b[off] | (b[off + 1] << 8)); }
	uint32_t D(int off) const { return (uint32_t)W(off) | ((uint32_t)W(off + 2) << 16); }

	uint16_t Id() const          { return W(0x04); }
	uint8_t  AttackDelay(int secondary) const { return b[0x06 + (secondary ? 1 : 0)]; }
	uint32_t Health() const      { return D(0x08); }
	uint8_t  MoveRate() const    { return b[0x0c]; }      // only the low byte is read (EA:1290)
	uint16_t TurnLimit() const   { return W(0x0e); }
	uint16_t Bullet(int secondary) const    { return W(0x10 + (secondary ? 2 : 0)); }
	uint16_t AttackSound(int secondary) const { return W(0x14 + (secondary ? 2 : 0)); }
	uint16_t DeathSound() const  { return W(0x60); }
	uint16_t CritDeathSound() const { return W(0x62); }
	uint16_t SpawnSound() const  { return W(0x64); }
	uint8_t  Flash() const       { return b[0x68]; }
	uint8_t  ModeBits() const    { return b[0x69]; }
	uint8_t  AttackRate() const  { return b[0x6a]; }
};

// The 0x0c-byte descriptor init_inventory_item_object fills from an item's
// AsBullet block (INV:147-190).
struct BulletDesc
{
	uint16_t hitAnim = 0;      // +0  the object id an impact turns into
	uint16_t hitSound = 0;     // +2
	uint8_t  speed = 0;        // +4
	uint8_t  ttl = 0;          // +5
	uint16_t damage = 0;       // +6
	uint8_t  width = 0x10;     // +8
	uint8_t  flash = 0;        // +9
	// The function's return: the item's own +2 word (sign-extended in the
	// original), or 0 when the item has no bullet flag or no block.
	int32_t  templateId = 0;
};

//==========================================================================
//
// The two pools. Field names are meanings; the offset each one has in the
// original's record is beside it, and Pack() writes them back out in that
// layout so a test can compare bytes with the running game.
//
//==========================================================================

struct ActorSlot           // 0x22 bytes, 16 of them (EA:857-892)
{
	int       creature = -1;   // +0x00  which 0x68 record (a pointer in the original); -1 none
	ObjectUid object = 0;      // +0x04  0 = the slot is free
	uint8_t   state = 0;       // +0x08  0x01 secondary/critical  0x04  0x08 attacking
	                           //        0x10 in contact  0x20 corpse  0x40 stagger  ==5 special idle
	uint8_t   bits = 0;        // +0x09  0x01 died this tick  0x02/0x04 ranged modes  0x08 steering
	                           //        0x10 steer side  0x20 blocked  0x40  0x80 a shot of mine hit something
	uint8_t   move = 0;        // +0x0a  0x02 off the ground  0x04
	uint8_t   settle = 0;      // +0x0b  facing fraction while alive; settle step once a corpse
	int32_t   health = 0;      // +0x0c  (the killing hit's size once dead, EA:1016)
	uint16_t  fracX = 0;       // +0x10  low halves of the 16.16 position
	uint16_t  fracY = 0;       // +0x12
	uint16_t  fracZ = 0;       // +0x14
	int       sector = -1;     // +0x16
	uint16_t  fallSpeed = 0;   // +0x18
	uint16_t  counter = 0;     // +0x1a
	uint16_t  bullet = 0;      // +0x1c  the item id queued to fire
	uint8_t   fireDelay = 0;   // +0x1e
	uint8_t   stepAccum = 0;   // +0x1f
	uint8_t   attackCount = 0; // +0x20
	uint8_t   pain = 0;        // +0x21
};

struct FlyingSlot          // 0x1c bytes, 16 of them (EA:700-704, 808-850)
{
	ObjectUid object = 0;      // +0x00  0 = free
	uint16_t  angle = 0;       // +0x04
	int       sector = -1;     // +0x06
	uint8_t   mode = 0;        // +0x08  0x01 bullet  0x02 impact picture counting down
	uint8_t   width = 0;       // +0x09
	uint16_t  fracX = 0;       // +0x0a
	uint16_t  fracY = 0;       // +0x0c
	uint16_t  fracZ = 0;       // +0x0e  (CP:1583, 1655)
	int32_t   zVelocity = 0;   // +0x10
	uint16_t  damageType = 0;  // +0x14  the bullet's item id
	uint16_t  damage = 0;      // +0x16
	uint8_t   speed = 0;       // +0x18
	uint8_t   ttl = 0;         // +0x19
	uint16_t  owner = 0;       // +0x1a  the shooter's actor back-reference, 0 = the player;
	                           //        reused as the countdown in mode 2 (EA:1067)
};

//==========================================================================
//
// What the rules need from, and report to, whatever is running them.
//
//==========================================================================

struct PlayerView
{
	int16_t  x = 0, y = 0, z = 0;   // 0x90a8e / 0x90a96 / 0x90a92
	uint16_t zFrac = 0;             // 0x90a90, the low half of the 16.16 height
	uint16_t angle = 0;             // 0x90a8a, 512 per turn
	uint16_t height = 0;            // 0x8c110
	int32_t  pitch = 0;             // 0x90a74, the view pitch
	int32_t  pitchApplied = 0;      // 0x8c108
	int      sector = -1;
};

class ActorHost
{
public:
	virtual ~ActorHost() {}
	// THE PLAYER'S HEALTH IS NOT KEPT HERE. One value, 0x8a0f0, read and
	// written by the frame loop, the DBASE100 interpreter, saves and these
	// rules alike; in the engine it is the game flow's (roth_gameflow.h,
	// GameFlow::Health), and the armour factor 0x81e30 is the interpreter's
	// (Dbase100Interp::g_value_reduction_factor, op 0x37). The rules reach
	// them only through these. The defaults are a private store for host tests.
	virtual int32_t Health() { return mHealth; }
	virtual void SetHealth(int32_t v) { mHealth = v; }
	virtual int32_t MaxHealth() { return mMaxHealth; }
	virtual int32_t Armour() { return mArmour; }
	virtual int32_t PainFlash() { return mFlash; }            // 0x89f3b
	virtual void SetPainFlash(int32_t v) { mFlash = v; }
	int32_t mHealth = 0x800, mMaxHealth = 0x800, mArmour = 0, mFlash = 0;   // the defaults' store
	// play_entity_sound(id, ..., x, y): a sound at a place. `sound` is the
	// stored value minus one, as the original passes it.
	virtual void SoundAt(int sound, int x, int y) { (void)sound; (void)x; (void)y; }
	// An object's record changed in a way that shows: position, art, rotation,
	// hidden bit.
	virtual void ObjectChanged(ObjectUid uid) { (void)uid; }
	virtual void ObjectCreated(ObjectUid uid) { (void)uid; }
	virtual void ObjectDestroyed(ObjectUid uid) { (void)uid; }
	// A sector's object list was edited (a record added, removed or moved to
	// another list), so the (sector, index) of other records may have changed.
	virtual void ListsChanged() {}
	// A creature's shot landed on the player for this much (after armour);
	// the original plays one of Adam's own pain sounds (EA:992).
	virtual void PlayerHurt(int damage) { (void)damage; }
	// The item in the player's right hand (the slot 0x81044 points at), 0 for
	// none. A 0x3c record with no item of its own places THIS (RC:3614).
	virtual int SelectedItem() { return 0; }
	// begin_object_command_chain(obj): run the chains bound to this object's
	// command id (RC:1717). Fired when a creature dies (RC:2815).
	virtual void RunObjectChain(ObjectUid uid) { (void)uid; }
	// The 0x3c record's +0x0e word and the place it was latched for: the
	// post-chain sound and where to play it (RC:1290-1298).
	virtual void ChainSound(uint16_t sound, int16_t x, int16_t y) { (void)sound; (void)x; (void)y; }
};

//==========================================================================
//
// THE ONE THING THE RULES DO NOT OWN: what a moving body runs into.
//
// The original answers that with find_sector_and_collide (CP:1448) for a
// creature's step and sweep_move_with_collision (CP:1556) for a bullet's
// flight. Both are asked for here in the original's own terms -- 16.16
// positions, the same radii and step heights, the same answers -- so that the
// rules above them (steering, gravity, crushing, what a hit does) are the
// original's line for line whichever world answers. tools/actors_test answers
// with the running original's own results; the engine answers from the level
// the player is actually standing in.
//
//==========================================================================

struct CollideQuery            // one creature step (EA:216-268, EA:105-128)
{
	ObjectUid self = 0;
	uint32_t fromX = 0, fromY = 0, fromZ = 0;   // 16.16, where it stands (z: 0x8c124)
	uint32_t toX = 0, toY = 0;                  // 16.16, where it wants to be
	int      sector = -1;                       // the sector it is recorded in
	uint16_t stepHeight = 0;                    // 0x80 walking, 0x260 off the ground
	uint16_t height = 0;                        // the art's height - 0x10 (0x8c0e8)
	uint16_t radius = 0;                        // the art's radius (0x8c134)
	uint32_t budget = 0;                        // squared step length, at least 0x100000
};

struct CollideAnswer
{
	bool     located = false;   // false: the point is in no sector at all (CF set)
	uint32_t x = 0, y = 0;      // 16.16, where it ended up (0x8c120 / 0x8c128)
	uint32_t z = 0;             // 0x8c124 afterwards
	uint32_t backX = 0, backY = 0;   // 0x8c158 / 0x8c15c: the position a blocked step restores
	uint8_t  hit = 0;           // 0x8c1e0: non-zero when something stopped or turned it
	uint16_t floorZ = 0x8000;   // frame word 0: the floor it stands on; high byte 0x80 = none found
	uint16_t ceilZ = 0x7fff;    // frame word 2: the ceiling over it
	int      sector = -1;       // 0x90c12 afterwards
};

struct SweepQuery              // one bullet tick (EA:946-962, CP:1556-1600)
{
	ObjectUid self = 0;
	uint32_t x = 0, y = 0, z = 0;   // 16.16 start
	int      sector = -1;
	int32_t  velX = 0, velY = 0, velZ = 0;   // 16.16 for the whole tick
	uint32_t speed = 0;         // picks 1, 2, 4 or 8 sub-steps (CP:1601-1611)
	uint32_t width = 0;         // 0x90fdc; a target is hit within width + 0x20
	uint16_t owner = 0;         // the shooter's actor back-reference; 0 = the player's own shot
};

struct SweepAnswer
{
	uint32_t x = 0, y = 0, z = 0;   // 16.16 end
	int      sector = -1;
	uint8_t  hit = 0;           // 0x01 a wall, floor or ceiling   0x02 an object or the player
	ObjectUid hitObject = 0;    // with 0x02: the object, or 0 for the player
};

class ActorCollision
{
public:
	virtual ~ActorCollision() {}
	virtual CollideAnswer Collide(const CollideQuery &q) = 0;
	// A player's shot that lands on an object with a 0x1b trigger and no actor
	// fires that object's chain from INSIDE the sweep (CP:1158-1162); whoever
	// implements this owes the same call to ActorHost::RunObjectChain.
	virtual SweepAnswer Sweep(const SweepQuery &q) = 0;
};

// A collision service built from the Realms map alone, for when nothing
// better answers (roth_actor_collide.cpp). NOT the original's algorithm --
// find_sector_and_collide slides a circle along walls (CP:210-372) and this
// only stops at them -- so it is measured, not assumed: tools/actors_test
// "replay ... simple" counts how often its answers equal the original's.
class ActorWorld;
class SimpleCollision : public ActorCollision
{
public:
	explicit SimpleCollision(ActorWorld *w) : mWorld(w) {}
	CollideAnswer Collide(const CollideQuery &q) override;
	SweepAnswer Sweep(const SweepQuery &q) override;
private:
	ActorWorld *mWorld;
	bool Blocked(int sector, int16_t x, int16_t y, int16_t z, uint16_t radius, uint16_t height, uint16_t step) const;
};

// The return values of a command handler, as RunCommand uses them.
enum { ACTCMD_NOTHING = 0, ACTCMD_ACTED = -1 };

class ActorWorld
{
public:
	// `map` is the level's live copy (its objects are edited in place), `db`
	// the game database, `tables` the creature tables. All three must outlive
	// this object. Begin() also does what map load does to the records:
	// zeroes every +0x0c (RC:3403).
	void Begin(Map *map, const Dbase100 *db, const CreatureTables *tables, ActorHost *host);
	void End();
	bool Active() const { return mMap != nullptr; }

	// ---- objects ----
	Object *Find(ObjectUid uid, int *sector = nullptr, int *index = nullptr);
	ObjectUid UidAt(int sector, int index) const;
	// resolve_command_objects (RC:317): every object whose command id is `key`,
	// in list order, capped; key 0 is the command source object alone.
	std::vector<ObjectUid> ResolveCommandObjects(uint16_t key, int cap = 0xc8);
	// The object a chain is running for (0x8a100). Set by whoever fires an
	// object's trigger, cleared when a chain starts from anything else.
	void SetCommandSource(ObjectUid uid) { mSource = uid; mSourceSynthetic = false; }
	ObjectUid CommandSource() const { return mSourceSynthetic ? 0 : mSource; }

	// ---- the three commands ----
	int CmdSpawnObject(Command &rec);        // 0x16  RC:622
	int CmdChangeObjectId(Command &rec);     // 0x3a  RC:479
	int CmdSpawnObjectAdv(Command &rec);     // 0x3c  RC:4108
	// Set by a handler that wants the chain stopped (0x8a268 = 2).
	uint8_t chainInterrupt = 0;
	// The id counter cmd_change_object_id mints from (0x8a0c0) and its mirror.
	uint32_t nextObjectId = 0, lastMintedId = 0;

	// ---- pools ----
	ActorSlot  actors[16];
	FlyingSlot flying[16];
	int actorCount = 0;        // 0x91e00
	int flyingCount = 0;       // 0x90fe0
	// The original keeps two run-time values in each shared creature record:
	// +0x00 the statistics pointer and +0x60 the id they were looked up by.
	struct CreatureRuntime { int def = -1; uint16_t defId = 0; };
	std::vector<CreatureRuntime> creatureRt;

	// spawn_entity_into_state_pool_a (EA:857): make `uid` an actor of creature
	// record `creature`. Returns the back-reference, 0 when the pool is full.
	int SpawnActor(int creature, ObjectUid uid);
	// The renderer's first-draw rule (R:5826-5840): an object whose art is a
	// creature placeholder and which has no slot yet gets one. Returns true
	// when a slot was taken. The CALLER decides "is being drawn"; in VR that
	// must be once a tic for both eyes together, never once an eye.
	bool WakeOnFirstDraw(ObjectUid uid);
	// THE RENDERER'S ONE GAME RULE (select_surface_anim_frame, render_world.c
	// :392-430, with decode_das_anim_frame, das_assets.c:1503-1521). When an
	// object with a pool slot is DRAWN wearing an animated picture, and the
	// slot is attacking, dying, staggering (actor state & 0x6c) or an impact
	// picture (flying mode & 2), the picture's frame is counter / ticksPerFrame;
	// a frame past the picture's last one sets the counter to 0xffff. That is
	// what ends an attack after its animation (19 frames x 4 ticks for the
	// STUDY1 ghoul: 76 ticks) instead of after the 257 the tick alone counts,
	// and what clears an impact picture early. An actor nobody sees keeps the
	// full 257.
	//
	// `picture` names the animated picture; the original keeps the last frame
	// requested in the picture itself (+0x18), shared by every object wearing
	// it, and a request equal to it is not clamped (das_assets.c:1509-1511).
	// Call once per drawn object per TIC, never once per eye: a second call in
	// the same tic is a no-op by that same rule, so per-eye calls would be
	// harmless only by accident.
	void AnimationDrawn(ObjectUid uid, uint32_t picture, uint16_t ticksPerFrame, uint16_t frameCount);

	// spawn_object_from_das_resource (DAS:208).
	bool SpawnFromResource(ObjectUid uid, uint16_t id, int variant);
	// destroy_dynamic_entity (EA:739).
	void DestroyObject(ObjectUid uid);

	// spawn_entity_at_position (EA:808). Returns the flying slot or -1.
	int SpawnFlying(uint16_t x, uint16_t y, uint16_t z, uint16_t id, uint32_t angleSource, int32_t zVelocity);
	// spawn_object_projectile_at_player (WC:682) / the flagged variant (WC:726).
	// `src` is a 16-byte object record (a real object or a command's copy).
	int FireFrom(uint16_t item, const Object &src, int zOffset, bool playerFire);
	// spawn_projectile_from_aim (WC:616-675) for a shot with no screen aim
	// (key fire): straight along the player's angle, from just under his eye,
	// climbing with the view pitch. `yawOffset` is the 512-per-turn offset a
	// screen or hand aim adds (WC:620-634); 0 for the key. Owner 0 = the
	// player's own shot, which can never hit him.
	int FirePlayer(uint16_t item, int32_t yawOffset = 0);

	// ---- the tick ----
	// One pass of the loop in tick_dynamic_entities (EA:1358-1377): every
	// flying thing in slot order, then every actor in slot order. The caller
	// runs it once per 70 Hz tick. Does nothing without a collision service.
	void Tick();
	void SetCollision(ActorCollision *c) { mCollision = c; }
	// What move_entity_with_collision / sweep_move_with_collision leave in
	// the block at 0x911a4, which outlives the call and is read by the next
	// rule (EA:346-470). One block, shared by both, as in the original.
	struct MoveBlock
	{
		uint32_t x = 0, y = 0;     // +0x00, +0x04
		uint32_t targetZ = 0;      // +0x08
		uint32_t z = 0;            // +0x0c
		uint16_t floor = 0;        // +0x10
		uint16_t ceil = 0;         // +0x12
		uint16_t height = 0;       // +0x14
		uint16_t standZ = 0;       // +0x16
		int      sector = -1;      // +0x18
		uint8_t  hit = 0;          // +0x1a
		ObjectUid hitObject = 0;   // +0x1c
	} moveBlock;
	// Move a record to another sector's list (RC:3849-3876).
	void MoveObjectToSector(ObjectUid uid, int newSector);

	// ---- statistics ----
	// entity_def_cache_lookup (EA:645): the ten-entry cache, most recent first.
	// Returns a cache node index or -1.
	int LookupDef(uint32_t id);
	const ActorDef *Def(int node) const { return node >= 0 && node < 10 ? &mDefNodes[node] : nullptr; }
	// build_entity_def_record for the first item whose +2 word is `id`
	// (R:10659 + DB:117).
	ActorDef BuildDef(uint32_t id) const;
	// init_inventory_item_object (INV:147) for item `item` (a 1-based id).
	BulletDesc BulletFor(uint32_t item) const;
	// resolve_object_template_record (RC:3612): 0, or the item's +2 word when
	// it is 0x200 or more. `flags` receives the item's +4 dword when found.
	// Item 0 means the item in the player's right hand.
	int32_t ResolveTemplate(uint32_t item, uint32_t &flags) const;
	mutable uint32_t lastItemRecord = 0;   // 0x89fa8: the item the last resolve settled on

	// ---- geometry the rules themselves own ----
	// locate_sector_at_position (CP:1003). Sector indices; -1 = none.
	int LocateSector(int16_t x, int16_t y, int16_t z, int hint) const;

	// ---- the player ----
	PlayerView player;
	bool    damagedThisFrame = false;   // 0x83e7c
	uint8_t lightFlash = 0;    // 0x853f6

	// ---- the player's health (docs/GAME_combat.md 3, each checked below) ----
	// The maximum a new game or restart gives (GC:766-772, 825-830): the
	// Health of the statistics built from Adam's own item, 0x800 when that is
	// 0. The game flow owns the value; this is only the rule that computes it.
	int32_t AdamMaxHealth(uint32_t adamItem) const;
	// apply_direct_damage_to_player (WC:29-43): fall damage, and the common
	// tail of every other path. Raises the pain flash, takes the health down,
	// stops at 0.
	void DamageDirect(uint32_t damage);
	// apply_reduced_damage_to_player (WC:50-61): a creature's shot. Armour
	// takes damage*armour>>11 off; 0x35c6 and above is not reduced.
	void DamageReduced(uint32_t damage);
	// apply_damage_to_player (R:9999-10026): commands and hazards. 0 kills;
	// above 200 it is (d-199)<<7; perTick multiplies by the tick count of the
	// frame; falloff takes d*falloff>>8 off. Armour does not apply.
	void DamageScripted(uint32_t damage, bool perTick, uint32_t falloff, uint32_t ticksThisFrame);
	// Once a frame (GC:334-340): the pain flash fades by 4 a tick.
	void DecayPainFlash(uint32_t ticksThisFrame);
	// The fall rule (PL:274-292). The original counts the ticks of a fall in
	// an accumulator that also sets the speed: each tick the player drops by
	// min(distance left, accumulator) and the accumulator grows by one. On
	// landing, an accumulator above 0x16 hurts: (acc - 0x16)^2, at most 0x1f40.
	static uint32_t FallDamage(int32_t accumulator);
	// The accumulator a clean drop of `height` units ends with.
	static int32_t FallAccumulatorForDrop(int32_t height);

	// 0x33 -- cmd_apply_damage (RC:4245-4275). `activeFace` is the face the
	// chain was fired from (0x8a0fc), -1 none; `perTick` is the firing
	// trigger's "every tick" flag (0x8a26a, RC:2887).
	int CmdApplyDamage(Command &rec, int activeFace, bool perTick, uint32_t ticksThisFrame);
	// 0x35 -- cmd_face_emits_damage (RC:4282-4297), run from a chain.
	int CmdFaceEmitsDamage(Command &rec, int activeFace, uint32_t ticksThisFrame);
	// The per-frame half of 0x35 (WC:72-106): every record that was armed at
	// load (modifier & 0x21, RC:5511) hurts the player each frame he is
	// within reach of one of its faces.
	void TickDamageEmitters(uint32_t ticksThisFrame);
	// point_to_wall_distance_sq (RC:4226) for a face of the map.
	uint32_t PlayerToFaceDistanceSq(int face, uint16_t radius) const;

	// The three generators (docs/GAME_combat.md 4.3). The values are the ones
	// the original has at the first gameplay frame of a new game, read out of
	// the running game by tools/actors_test's probe.
	uint16_t rngDamage = 0x04fd;        // 0x7272c
	uint32_t rngWander = 0x00007e15;    // 0x72730
	uint32_t rngAnim = 0x00007e15;      // 0x72734
	uint32_t rngAnimSeed = 0x00007e15;  // 0x72738
	int32_t  animScheduler = 0;         // 0x7273c

	// ---- for tests and logs ----
	// The original's back-reference for a slot, and a slot's bytes in the
	// original's layout with pointers replaced by indices.
	static int ActorBackref(int slot)  { return slot * 0x22 + 1; }
	static int FlyingBackref(int slot) { return slot * 0x1c + 1; }
	uint16_t SectorOffset(int sector) const;   // the geometry offset the original names a sector by
	std::string Dump() const;                  // tools/actors_test compares this with the probe's
	std::string DumpTick() const;              // the same without the object lists, plus the pools' own objects

	Map *MapPtr() { return mMap; }
	const CreatureTables *Tables() const { return mTables; }
	ActorHost *Host() { return mHost; }

private:
	Map *mMap = nullptr;
	const Dbase100 *mDb = nullptr;
	const CreatureTables *mTables = nullptr;
	mutable ActorHost *mHost = nullptr;
	mutable ActorHost mNullHost;

	std::vector<std::vector<ObjectUid>> mUids;   // parallel to Map::objects
	uint32_t mNextUid = 0x80000000u;

	// The command source: an object, or the record cmd 0x3c builds at the
	// player's feet when its key is 0xffff (RC:4114-4122).
	ObjectUid mSource = 0;
	bool mSourceSynthetic = false;

	ActorDef mDefNodes[10];
	int mDefNext[10];          // the list link; -1 ends it
	int mDefHead = -1;
	int mDefCount = 0;

	ActorCollision *mCollision = nullptr;

	// the tick's parts (roth_actor_tick.cpp)
	uint16_t DamageRoll();                                   // EA:721
	bool Revalidate(int creature);                           // R:10739
	const ActorDef *CreatureDef(int creature) const;
	void TickFlying();                                       // EA:1053
	void FlyBullet(int slot);                                // EA:946
	void FlyWalker(int slot);                                // EA:899
	bool BulletSpent(int slot);                              // WC:175
	uint32_t HitDamage(int actor, uint16_t type, uint16_t base);   // R:10754
	void TickActor(int slot);                                // EA:1311
	void ActorDied(int slot);                                // RC:2809 + EA:80
	void ActorCorpse(int slot);                              // EA:1092
	bool ActorStagger(int slot);                             // EA:1143
	bool ActorAttackRecover(int slot);                       // EA:1204
	void ActorThink(int slot);                               // EA:1281
	void ActorAmbient(int slot, const ActorDef &def);        // EA:1243
	void ActorLocomotion(int slot, uint8_t moveRate);        // EA:574
	void ActorAim(int slot, uint32_t limit);                 // EA:490
	void ActorMove(int slot, int32_t dx, int32_t dy);        // EA:216 + EA:105
	void ActorVertical(int slot, int32_t dx, int32_t dy);    // EA:357
	bool ActorTouchesPlayer(int slot) const;                 // R:10689
	void BeginAttack(int slot);                              // WC:248
	void LaunchAttack(int slot);                             // WC:275
	void AttackApply(int slot, const ActorDef &def, bool secondary);   // WC:216

	std::vector<int> mEmitters;
	std::vector<std::pair<uint32_t, uint16_t>> mPictureLast;   // picture -> its +0x18            // command indices (0-based) armed at load, newest first

	ObjectUid AppendObject(int sector, const Object &o);     // alloc_object_record_slot
	void RemoveObject(int sector, int index);                // remove_secondary_state_record
	void MarkTriggerLinks(Object &o, uint16_t id) const;     // RC:456
	uint8_t PlayerBearing(int16_t x, int16_t y) const;       // RC:300
	bool InsideSector(int sector, int16_t qx, int16_t qy) const;   // CP:91
	friend class ActorTicker;
};

// The integer trigonometry the rules use. Exposed for the tests, which check
// both against the tables in the running original.
int32_t ActorSin(uint32_t byteOffset);                 // sincos_lookup, R:113
uint32_t ActorBearing(int16_t x1, int16_t y2, int16_t y1, int16_t x2);   // atan2_bearing, R:614
int32_t ActorIsqrt(uint32_t v);                        // isqrt_fixed, R:169

} // namespace roth
