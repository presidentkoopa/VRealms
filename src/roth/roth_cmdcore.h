#pragma once
//
// The RAW map commands that are pure game state, engine-free.
//
// Counters and loops (0x15 0x1e 0x1f 0x21 0x22), the texture oddments (0x14 0x1c
// 0x20 0x2e), the player effects (0x33 0x35 0x3f 0x41) and the particle burst
// (0x2d) with its pool. Every rule is taken from ROTH.C's raw_commands.c and the
// line is cited on the function in the .cpp.
//
// NO ENGINE HEADERS. The same code is run by tools/cmdcore_test against the
// retail maps and compared, record by record, with the original engine driven by
// a probe mod. What the engine has to do about a change (repaint a wall, turn
// the camera, draw a particle) arrives through CmdHost.
//
// The state lives where the original keeps it: the command RECORD is the
// counter (word +0x0a), the map's own geometry is the texture state, and the
// handful of globals the count family shares with the chain executor are the
// public members below.
//

#include <stdint.h>
#include <vector>
#include "roth_raw.h"
#include "roth_commands.h"

namespace roth
{

// A handler result that is not zero and has bit 0 clear. Three handlers return a
// POINTER in the original (the latched 0x36 record, raw_commands.c:181-189; the
// active wall, :4891). The executor only ORs results together; "non-zero" arms a
// trigger's one-shot bit (:1646) and bit 0 alone lets the post-chain sound play
// (:1296). Records and geometry sit at even addresses, so such a result arms
// the first and not the second.
enum { CMD_LATCHED = 2 };

// One slot of the particle pool: 0x20 slots of 0x34 bytes at 0x911e4
// (alloc_particle, raw_commands.c:3640). Offsets +8..+0x13 are laid out as an
// OBJECT record, because the renderer draws the list through the object path
// (renderer.c:8010-8011): position, texture word, rotation, flags, light,
// render type, z.
struct CmdParticle
{
	bool     busy = false;         // +0x30 bit 0
	int      sector = -1;          // +0x04, as a sector index; -1 is the original's 0
	int16_t  x = 0, y = 0;         // +0x08 +0x0a
	uint16_t texture = 0;          // +0x0c  index low, source high: the command's key
	uint8_t  rotation = 0;         // +0x0e
	uint8_t  flags = 0;            // +0x0f
	uint8_t  light = 0;            // +0x10
	uint8_t  renderType = 0;       // +0x11
	int16_t  z = 0;                // +0x12
	int32_t  zvel = 0, xvel = 0, yvel = 0;   // +0x18 +0x1c +0x20
	int32_t  xacc = 0, yacc = 0, zacc = 0;   // +0x24 +0x28 +0x2c, 16.16
	uint8_t  isStatic = 0;         // +0x31
	uint16_t life = 0;             // +0x32
	// Not in the original: counts allocations of this slot, so whoever draws the
	// pool can tell a slot that was freed and reused from one that lived on.
	uint32_t serial = 0;
};

// What the commands need from the rest of the game.
class CmdHost
{
public:
	virtual ~CmdHost() {}
	// g_item_autoselected_flag, 0x89f60.
	virtual bool ItemAutoSelected() { return false; }
	// run_command_dbase100_record on the record 0x36 latched (1-based index).
	virtual void RunPendingRecord(uint16_t recordIndex) { (void)recordIndex; }
	// Append {chain, 0} to the deferred queue. False when it already holds 16.
	virtual bool QueueDeferred(uint16_t chain) { (void)chain; return false; }
	// g_player_x / g_player_y, map units.
	virtual void PlayerXY(int16_t &x, int16_t &y) { x = 0; y = 0; }
	// g_player_angle, 512 to the turn; the original keeps all 16 bits.
	virtual uint16_t PlayerAngle() { return 0; }
	virtual void SetPlayerAngle(uint16_t a) { (void)a; }
	// The map was written; tell whatever shows it.
	virtual void TexmapChanged(int textureMap) { (void)textureMap; }
	virtual void SectorFlatChanged(int sector) { (void)sector; }
	virtual void PlatformChanged(int platform) { (void)platform; }
	virtual void ObjectChanged(int sector, int index) { (void)sector; (void)index; }
	// apply_damage_to_player took `amount` off the player's health.
	virtual void PlayerDamaged(int32_t amount) { (void)amount; }
};

class CmdCore
{
public:
	// `commandRng` is g_command_rng (0x71f48), which cmd_delay_timer and the
	// flash-lights tick also draw from, so it is shared and not owned here.
	// `pendingRecord` is g_pending_command_record (0x8a0dc) as a 1-based index.
	void Attach(Map *map, CmdHost *host, uint32_t *commandRng, uint16_t *pendingRecord);
	// A level starts: the particle pool empties and the chain globals clear.
	// Health is the player's and survives a level change.
	void BeginLevel();
	// A new game: g_player_health = 0x800 (game_core.c:828-830).
	void NewGame();

	// ---- the globals the count family shares with the executor ----------
	uint8_t  interrupt = 0;        // g_command_chain_interrupt 0x8a268: 0, 1 stop, 2 re-run
	uint32_t stepMode = 0;         // g_anim_step_mode 0x8a104
	uint32_t stepDelta = 0;        // 0x8a108
	uint8_t  damageScale = 0;      // 0x8a26a: set while a repeating sector trigger's chain runs

	// ---- "what the player just used", for a key of 0 ----------------------
	int activeTexmap = -1;         // g_active_object 0x8a0f8: a texture-map record
	int activeFace = -1;           // g_active_object_secondary 0x8a0fc: a face
	int sourceSector = -1;         // g_command_source_object 0x8a100: an object,
	int sourceIndex = -1;          //   as (sector, index within the sector)

	// ---- the player ------------------------------------------------------
	int32_t  health = 0x800;       // g_player_health 0x8a0f0
	int32_t  damageFlash = 0;      // g_damage_flash_level 0x89f3b
	bool     hudDirty = false;     // 0x83e7c
	uint32_t speedRequest = 0;     // g_player_speed_reduction_request 0x89f54
	uint32_t speedShift = 0;       // g_player_speed_reduction_shift 0x89f58
	int32_t  frameTimeScale = 2;   // g_frame_time_scale 0x85324

	// ---- particles ---------------------------------------------------------
	enum { PARTICLES = 0x20 };
	CmdParticle particles[PARTICLES];
	uint16_t particleRng = 0x3f3;  // g_rng_state 0x7276c

	// ---- the handlers: -1 acted, 0 nothing, CMD_LATCHED ---------------------
	int Count(Command &rec);                   // 0x15
	int TextureChangeCount(Command &rec);      // 0x1f
	int AnimateFacegroupTexture(Command &rec); // 0x21
	int CountAddlArg(Command &rec);            // 0x22
	int ModifyCount(Command &rec);             // 0x1e
	int SyncFacegroupTexture(Command &rec);    // 0x14
	int CycleTexture(Command &rec);            // 0x1c
	int CycleObjectTexture(Command &rec);      // 0x20
	int SmashFaceTexture(Command &rec);        // 0x2e
	int ApplyDamage(Command &rec);             // 0x33
	int FaceEmitsDamage(Command &rec);         // 0x35
	int PlayerRotation(Command &rec);          // 0x3f
	int SetPlayerSpeedReduction(Command &rec); // 0x41
	int ParticleEffect(Command &rec);          // 0x2d
	// True and `result` set when `opcode` (already masked with 0x7f) is one of
	// the fourteen above.
	bool Dispatch(uint8_t opcode, Command &rec, int &result);

	// ---- per frame ---------------------------------------------------------
	// tick_world_effects step 1: latch and clear the speed request
	// (raw_commands.c:3290-3293).
	void LatchSpeedReduction();
	// tick_particles (raw_commands.c:5762).
	void TickParticles();
	// The damage flash's decay, once per frame (game_core.c:337-342).
	void DecayDamageFlash();

	// ---- shared leaves, public so the host test can check them ------------
	void ApplyDamageToPlayer(uint32_t amount, uint8_t scaleFlag, uint32_t falloff);
	void AdvanceClampHealth(uint32_t amount, uint32_t max);
	std::vector<int> TexmapsById(uint16_t key) const;
	std::vector<int> GatherFaces(uint16_t id) const;
	std::vector<int> SectorsById(uint16_t key) const;
	std::vector<int> ConnectedSectors(uint16_t key);
	struct ObjRef { int sector; int index; };
	std::vector<ObjRef> ResolveObjects(uint16_t id) const;
	bool FindObjectByKey(uint16_t key, ObjRef &out) const;
	int FindFaceByTexmap(int textureMap) const;
	uint32_t PointToWallDistanceSq(int face, uint16_t radius);
	static uint32_t PointSegmentDistanceSq(uint16_t radius, int16_t ax, int16_t ay,
		int16_t bx, int16_t by, int16_t px, int16_t py);
	// The 512-entry sine table at 0x72080, amplitude 16384.
	static int16_t Sine512(unsigned index);
	// locate_sector_at_position (collision_physics.c:1003): the sector holding a
	// point, as an index, or -1. `hintSector` -1 for none.
	int LocateSector(int x, int y, int z, int hintSector) const;
	bool PointInSector(int sector, int16_t x, int16_t y) const;
	int SpawnParticle(int16_t x, uint16_t texture, int16_t y, int16_t z);
	int SpawnParticleOnEdge(int face, uint16_t texture);

private:
	Map *mMap = nullptr;
	CmdHost *mHost = nullptr;
	CmdHost mNullHost;
	uint32_t *mCommandRng = nullptr;
	uint16_t *mPending = nullptr;
	uint32_t mOwnRng = 0xcc61;
	uint16_t mOwnPending = 0;

	enum Paint { PaintNone, PaintTexmaps, PaintFaces, PaintObjects };
	int CountFamily(Command &rec, Paint paint);
	int StepCount(Command &rec, uint16_t seed, Paint paint);
	int FlushPending();
	int Done();
	void PaintStep(Command &rec, uint16_t nv, Paint paint);
	uint16_t TexmapWordC(int tmi) const;
	uint16_t *TexmapSlot(int tmi, unsigned dirOff);
	uint16_t ParticleRngNext();
	int AllocParticle();
	void Disable(Command &rec) { rec.modifier |= 0x08; rec.disabled = true; }
};

} // namespace roth
