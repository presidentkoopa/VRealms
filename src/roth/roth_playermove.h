#pragma once
//
// THE ORIGINAL'S PLAYER MOVEMENT, engine-free.
//
// Ported from ROTH.C (the original engine, lifted to C) and not from any summary:
//   renderer.c   :192-210 move_input_*, :241 key_z_crouch, :280-315 turn_input_*,
//                :212-231 look_pitch_*, :513-523 look_pitch_recenter_up,
//                :592-605 key_a_jump, :969-978 carry_player_span,
//                :1044-1103 player_movement_tick, :1150-1231 apply_player_movement_input,
//                :9606-9612 reset_movement_velocity_queues, :9688 clear_819c0_bits,
//                :9911-9916 approach_value, :10905-10912 reset_player_locomotion_state,
//                :1390-1406 find_record_by_id
//   player.c     :205-329 update_player_vertical_physics, :340-383 update_player_view_bob
//   collision_physics.c :91-162 scan_sector_edges_at_y, :210-371 clip_query_circle_to_edge,
//                :373-398 clip_locate_query_to_object, :595-647 crw2_portal_narrow,
//                :674-900 collide_point_walls_recursive, :906-1000 the sector search,
//                :1030-1068 collide_sector_walls, :1180-1400 the object push-out,
//                :1402-1434 resolve_collisions_in_sector, :1448-1503 find_sector_and_collide /
//                collide_substep_track_sector, :1677-1796 move_player_with_collision
//   doors.c      :347-451 collect_doors_near_query / door_gather_sector / gather_nearby_doors
//   weapon_combat.c :29-43 apply_direct_damage_to_player (through the host)
// The checklist with every part, constant and line is docs/PLAYER_MOVEMENT_PORT.md;
// tools/move_test diffs this against the running original, tick by tick.
//
// THE ORIGINAL'S MEMORY, KEPT AS ITS MEMORY. The collision walks the map through 16-bit
// offsets into two byte images (the geometry section, es = 0x90aa8, and the vertex section,
// gs = 0x90aac) and pushes off objects through the objects buffer (0x90aa4). Those three are
// held here byte for byte as the original holds them (BuildImage: the RAW file's own bytes
// and the two load fixups that touch fields the collision reads), so every offset, wrap and
// sign of the original's arithmetic is the same arithmetic here. The host keeps the live
// parts current: sector heights and platform heights (SetSectorHeights, SetPlatformHeights),
// object records (ob), the creature pool, the door pool.
//
// Units are the original's: positions 16.16 in the original's map space (NOT the engine's,
// which may be wrap-shifted or room-shifted -- the wiring converts), angles 512 per turn.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace roth
{

// The held-key table at 0x707f1, in the order dispatch_held_key_actions walks it
// (input.c:208-226; dumped from the running original by tools/move_test).
enum MoveKey
{
	MK_BACK = 0,          // 0x50 Down   move_input_backward
	MK_FORWARD,           // 0x48 Up     move_input_forward
	MK_TURN_RIGHT,        // 0x4d Right  turn_input_right
	MK_TURN_LEFT,         // 0x4b Left   turn_input_left
	MK_STRAFE_LEFT,       // 0x33 ,      move_input_strafe_left
	MK_STRAFE_RIGHT,      // 0x34 .      move_input_strafe_right
	MK_PITCH_CENTRE_DOWN, // 0x51 PgDn   look_pitch_recenter_down
	MK_PITCH_CENTRE_UP,   // 0x49 PgUp   look_pitch_recenter_up
	MK_PITCH_UP,          // 0x47 Home   look_pitch_up
	MK_PITCH_DOWN,        // 0x4f End    look_pitch_down
	MK_JUMP,              // 0x1e A      key_a_jump
	MK_CROUCH,            // 0x2c Z      key_z_crouch
	MK_COUNT
};
extern const uint8_t kMoveKeyScancode[MK_COUNT];

// What the original's ISR tick reads besides the keys.
struct MoveInput
{
	uint32_t held = 0;          // 1 << MoveKey for each key held this tick
	bool     shift = false;     // g_key_modifier_flags & 0x40 (the run key inverts the toggle)
	int32_t  mouseDx = 0, mouseDy = 0;   // g_mouse_dx / g_mouse_dy (the mouse-steer path only)
};

// The original's movement tables (the data image; tools/move_test compares them with the
// running original's): jump 0x71304, keyboard direction offsets 0x707c9, walk bob 0x724f0.
struct MoveTables
{
	int32_t  jump[0x15];
	uint16_t dir[16];
	int16_t  bob[32];
	static const MoveTables &Original();
};

// A door in the door pool (0x8b3f8, stride 0x1f6): the sector it was spawned for (its record's
// first word) and its leaf as the collision clips it, the four points at *(rec+0x2e)+0x82
// (stride 0x10, x at +0, y at +4) in the original's map space.
struct MoveDoor
{
	uint16_t sector = 0;
	int16_t  x[4] = {}, y[4] = {};
};

class PlayerMoveHost
{
public:
	virtual ~PlayerMoveHost() {}
	// apply_direct_damage_to_player (weapon_combat.c:29-43), the fall damage.
	virtual void DamageDirect(uint32_t damage) = 0;
	// start_persistent_looping_sound(sound) for the landing thud (player.c:292-295).
	virtual void LandingSound(uint32_t sound) { (void)sound; }
	// fire_tracked_object_trigger (raw_commands.c:2974) for the object at `obOffset` in the
	// objects image, from inside the push-out (collision_physics.c:1283-1287).
	virtual void ContactTrigger(uint32_t obOffset) { (void)obOffset; }
};

class PlayerMove
{
public:
	// ---- the original's memory -------------------------------------------------
	std::vector<uint8_t> es;     // 0x90aa8: RAW bytes [0, header+0): header, sectors, faces, mappings, platforms
	std::vector<uint8_t> gs;     // 0x90aac: the vertex section (header+0x14 bytes)
	std::vector<uint8_t> ob;     // 0x90aa4: the objects section (header+0x16 bytes, plus room)
	std::vector<uint8_t> dims;   // 0x85c50: the art's collision dimensions, 4 bytes an object id
	std::vector<uint32_t> pool;  // the creature pool's objects (0x91e04 +4), as offsets into ob, slot order; 0 = empty slot
	std::vector<MoveDoor> doors; // the door pool, pool order

	// RAW file -> es, gs, ob, with fixup_raw_sectors_after_load (renderer.c:10267) and
	// mark_geom_sentinel_entries (renderer.c:1637), and flag_sectors_with_objects
	// (map_load.c:133). Returns false on a malformed file.
	bool BuildImage(const uint8_t *raw, size_t size);
	uint16_t SectorOffset(int index) const;      // geometry offset of sector `index`
	int SectorIndex(uint16_t offset) const;      // and back; -1 if not a sector record
	int SectorCount() const;
	void SetSectorHeights(int index, int16_t ceiling, int16_t floor);
	void SetPlatformHeights(int index, int16_t undersideZ, int16_t topZ);   // via the sector's +0x18
	void SetSectorDoorFlag(int index, bool on);
	void SetSectorTextureOverride(int index, int8_t v);   // byte +0x0c (the stacked-sector alt-Z reads it, CP:722-726)  // byte +0x16 bit 0 (setup_door_swing_geometry, doors.c:464)

	// ---- the map's tuning (init_movement_tuning_from_first_map, map_load.c:197-209) ----
	void InitTuning();                           // reads the metadata block of es
	uint16_t moveSpeed = 5;      // 0x12570 imm16, the velocity half-life period
	uint16_t maxClimb = 0x41;    // 0x8c148 (2*meta+0xc + 1)
	uint16_t minFit = 0x60;      // 0x90be0 (2*meta+0xe)
	int32_t  restHeight = 0x90;  // 0x8c114

	// ---- the player's state (named by the original's addresses) -----------------
	uint32_t x = 0, z = 0, y = 0;   // 0x90a8c / 0x90a90 / 0x90a94, 16.16
	uint16_t angle = 0;          // 0x90a8a, 512 a turn
	uint16_t sector = 0;         // 0x90c12, geometry offset (0 = none)
	uint16_t sectorCache = 0;    // 0x8c1d6
	int32_t  velX = 0, velY = 0; // 0x90a80 / 0x90a84
	uint16_t rateCounter = 0;    // 0x90bda
	int32_t  speedAccum = 0;     // 0x7e8d8
	uint16_t turnAccum = 0;      // 0x90bd8
	uint16_t turnMirror = 0;     // 0x7e8f4 (0x7e8e8 + 0xc), the turn tail's mirror
	uint16_t inputBits = 0;      // 0x7e920
	uint32_t runToggle = 0;      // 0x7e8dc
	int32_t  runActive = 0;      // 0x7e8e0
	uint16_t queueSelect = 0;    // 0x90bd6
	struct Queue { uint16_t count = 0; int32_t d[16][2] = {}; } queueA, queueB;   // 0x90abe / 0x90b42
	uint32_t speedShift = 0;     // 0x89f58 (latched by the command core from 0x41)
	uint8_t  locoFlags = 0;      // 0x819c0
	int32_t  jumpPhase = 0;      // 0x819c1
	int32_t  fallState = 0;      // 0x819c5
	int32_t  landPending = 0;    // 0x819c9
	int32_t  crouchMode = 0;     // 0x819cd
	int32_t  fallAccum = 0;      // 0x819d1
	uint16_t height = 0x90;      // 0x8c110
	uint16_t camHeight = 0x90;   // 0x8c112
	int16_t  bobOffset = 0;      // 0x8c1d4
	int32_t  health = 0x800;     // 0x8a0f0 (the host owns it; read for the forced crouch)
	int32_t  viewPitch = 0;      // 0x90a74
	int32_t  pitchApplied = 0;   // 0x8c108
	uint32_t bobPhase = 0;       // 0x8c10c
	int32_t  viewClearance = 0;  // 0x8497c
	uint8_t  movementMode = 1;   // 0x7674a
	uint8_t  cursorType = 0;     // 0x7e932
	uint8_t  mouseRelative = 0;  // 0x7e8d0
	int32_t  mouseSmooth = 0, mouseLimit = 0;   // 0x7e8f8 / 0x7e8fc
	int32_t  mouseDxIn = 0, mouseDyIn = 0;      // 0x76870 / 0x76874 (g_mouse_dx / g_mouse_dy)
	uint32_t dialogueBusy = 0, freezeGate = 0;  // 0x83aea / 0x83125
	uint16_t landingSound = 0;   // 0x7fe50 (0x7fe38 + 0x18), sound + 1
	uint8_t  lightOffset = 0;    // 0x853f6 (render_sector_walk_mode + 0x23)
	uint8_t  viewShade = 0;      // 0x8c1de viewmodel shade, written after the queue
	uint16_t sectorCacheFlag = 0;// 0x8c1d8
	// the last collision's outputs
	uint8_t  hitFlags = 0;       // 0x8c1e0
	uint32_t entityCount = 0;    // 0x8c12c
	// counters for the tests (not the original's): how often each response ran
	struct Stats { uint32_t wallPush = 0, wallSlide = 0, portalRecurse = 0, objectPush = 0, objectZNarrow = 0, doorClip = 0, doorPush = 0, sectorChange = 0; } stats;

	// ---- the tick (70 Hz ISR) and the frame ----------------------------------------
	void Tick(const MoveInput &in);   // player_movement_tick
	void Frame(int fts);              // move_player_with_collision(0), then update_player_view_bob
	void ClampViewPitch();            // update_view_transform_params' pitch clamp, run after Frame
	// the held-key handlers, callable on their own
	void KeyForward(); void KeyBackward(); void KeyStrafeLeft(); void KeyStrafeRight();
	void KeyTurnLeft(); void KeyTurnRight(); void KeyJump(); void KeyCrouch();
	void KeyPitchUp(); void KeyPitchDown(); void KeyPitchCentreDown(); void KeyPitchCentreUp();
	void ResetLocomotion();           // reset_player_locomotion_state
	void ResetQueues();               // reset_movement_velocity_queues
	// carry_player_span (renderer.c:969-978) for a moving floor at `height` that moved by d*.
	void CarryPlayerSpan(uint16_t heightNow, int32_t dx, int32_t dy, int32_t dz);
	// update_player_vertical_physics alone (player.c:205-329), for tests.
	uint32_t VerticalPhysics(uint32_t floorZ, uint32_t ceilRaw, uint32_t pos);
	// locate_sector_at_position (collision_physics.c:1003): the sector holding (x,y,z), 0 = none.
	uint16_t Locate(int16_t px, int16_t py, int16_t pz, uint16_t hint);
	int16_t SectorFloor(uint16_t off) const { return (int16_t)E16(off + 2); }
	// NOT THE ORIGINAL'S: a VR roomscale step (owner, 2026-11-13: the headset's own walking moves the
	// player), put in the queue the next Frame consumes as one more entry, so it goes through the
	// original's collision like a tick's. False when that queue is full (keep it for the next frame).
	bool AddExtraDelta(int32_t dx, int32_t dy);

	PlayerMoveHost *host = nullptr;
	const MoveTables *tables = &MoveTables::Original();

private:
	// collision scratch, the original's globals
	uint32_t qX = 0, qY = 0, qZ = 0;          // 0x8c120 / 0x8c128 / 0x8c124 (16.16)
	uint32_t restoreX = 0, restoreY = 0;      // 0x90ab4 / 0x90ab8
	uint32_t savedX = 0, savedZ = 0, savedY = 0;   // 0x8c160 / 0x8c164 / 0x8c168
	uint16_t savedSector = 0;                 // 0x8c170
	uint16_t portalContinue = 0;              // 0x8c190
	uint8_t  stepActive = 0;                  // 0x8c1e2
	uint8_t  flagMask = 0;                    // 0x8c1e4
	uint8_t  portalKind = 0;                  // 0x8c1e5
	uint8_t  fireGate = 0;                    // 0x8c1e6
	int32_t  altZ = 0;                        // 0x8c100
	uint16_t stackCount = 0;                  // 0x8c192
	uint16_t stack[0x1e] = {};                // 0x8c194
	uint16_t bestSector = 0;                  // 0x8c1d0
	int32_t  boxMax = 0x1c, boxMin = -0x1c;   // 0x8c130 / 0x8c134
	int32_t  radiusSq = 0xc400, radiusSq2 = 0xc710, cornerSq = 0xc440, radius = 0x70;   // 0x8c138 / 0x8c13c / 0x8c140 / 0x8c144
	uint16_t span = 0;                        // 0x8c0e8
	uint16_t maxStep = 0;                     // 0x90bde
	int32_t  segThreshold = 0;                // 0x8c14c
	int32_t  push154 = 0, push158 = 0, push15c = 0;   // 0x8c154 / 0x8c158 / 0x8c15c
	uint8_t  pushed = 0, retries = 0;         // 0x8c1da / 0x8c1dc
	uint32_t blocker = 0;                     // 0x8c16c (an ob offset + 1, 0 = none)
	int16_t  edgeAx = 0, edgeAy = 0, edgeBx = 0, edgeBy = 0;   // 0x8c1ef.. (the clip's edge)
	struct WorkItem { uint32_t flag; uint32_t ref; };   // {0, ob offset} or {1, door index}
	std::vector<WorkItem> work;               // 0x8498c

	void ApplyMovementInput();
	void Enqueue(int32_t vx, int32_t vy);
	void Collide(uint8_t *frame);             // move_player_with_collision's queue walk
	void ViewBob(const uint8_t *frame, int fts);
	int  ScanSectorEdgesAtY(uint16_t sec, uint8_t *frame);
	int  SearchFromHint(uint32_t hint, uint8_t *frame, uint32_t *out);
	int  SearchNeighbours(uint32_t sec, uint8_t *frame, uint32_t *out);
	int  SearchGlobal(uint8_t *frame, uint32_t *out);
	void FindQuerySector(uint32_t hint, uint8_t *frame, uint32_t *out);
	int  FindSectorAndCollide(uint8_t *frame);
	void ResolveInSector(uint32_t sec, uint8_t *frame);
	void CollideSectorWalls(uint32_t sec, uint8_t *frame);
	void CollidePointWalls(uint32_t sec, uint8_t *frame);
	int  PortalNarrow(uint8_t *frame);
	int  ClipCircleToEdge(uint8_t *frame);
	void ClipToDoor(const MoveDoor &d, uint8_t *frame);
	int  ResolveObjects(uint8_t *frame);
	void PushOut(uint32_t obj, int32_t pushX, int32_t pushY);
	void GatherNearby();
	bool DoorRecordExists(uint16_t sec) const;   // find_record_by_id

	uint8_t  E8(uint32_t o) const;
	uint16_t E16(uint32_t o) const;
	int16_t  VX(uint16_t off) const;
	int16_t  VY(uint16_t off) const;
};

// The original's trig, as the actors port has it (roth_actors.cpp, checked against the
// running original's table): sincos_lookup (a byte offset into the 512-word table),
// atan2_bearing, isqrt_fixed.
int32_t ActorSin(uint32_t byteOffset);
uint32_t ActorBearing(int16_t x1, int16_t y2, int16_t y1, int16_t x2);
int32_t ActorIsqrt(uint32_t v);

} // namespace roth
