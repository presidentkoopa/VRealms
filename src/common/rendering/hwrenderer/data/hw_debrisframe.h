/*
** hw_debrisframe.h
**
** [DEBRISPOOL] The debris pool's records, and what its CPU side hands its GPU side each frame.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: "debris that stays" ("Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #9, "Engine docs/
** DEBRIS_9_IMPL_NOTES.md"). A ring particle is stateless -- its record holds starting conditions and the
** vertex shader works out where it is now -- so it cannot bounce off a wall, come to rest, or be thrown
** again by a blast. Debris needs state. The pool is a GPU buffer of pieces, each simulated one step per
** world tic by a compute shader (shaders/compute/debris_step.comp) against the level collision field (#8),
** and drawn as a billboard (gpuparticles.vp) or a mesh (meshparticles.vp) like any particle.
**
** Three homes, the level field's arrangement:
**   - the CPU side (src/rendering/hwrenderer/hw_debrispool.cpp) reads the level -- the SpawnParticles
**     bursts of debris definitions, the blast pushes, moving sectors -- decides slots and steps, and fills
**     DebrisPoolFrame;
**   - the GPU side (vk_debrispool.cpp) owns the buffers and records the uploads and the steps;
**   - this header is what both agree on. Every struct marked GPU is std430 with no padding and must match
**     its GLSL twin in debris_step.comp, gpuparticles.vp and meshparticles.vp (the static_asserts below and
**     the SPIR-V readback in the notes' checks hold them together).
**
** Presentation only: nothing here is ever read back into the playsim.
**
*/

#pragma once

#include <cstddef>
#include <cstdint>

// THE POOL'S SIZE (r_debris_pool): a power of two from 4,096 to 65,536 pieces, 16,384 by default
// (owner, 2026-09-14). A size between the steps is rounded down to one.
inline constexpr int DEBRIS_POOL_MIN = 4096;
inline constexpr int DEBRIS_POOL_DEFAULT = 16384;
inline constexpr int DEBRIS_POOL_MAX = 65536;

inline int DebrisPoolSizeFor(int requested)
{
	int size = DEBRIS_POOL_MIN;
	while (size < DEBRIS_POOL_MAX && size * 2 <= requested)
		size *= 2;
	return size;
}

// Only named definitions (slots 0..255) can be debris; the definition table below is indexed by that slot.
inline constexpr int DEBRIS_DEFINITION_SLOTS = 256;

// One simulation step per world tic, at most this many a frame, so a hitch does not become a burst (SH4).
inline constexpr int DEBRIS_STEPS_PER_FRAME = 2;
inline constexpr float DEBRIS_STEP_SECONDS = 1.0f / 35.0f;	// TICRATE

// What one frame may hand over: new pieces (the rest wait for the next frame), blast pushes and wake boxes
// (more wake boxes than this in a step wake every resting piece instead).
inline constexpr int DEBRIS_SPAWNS_PER_FRAME = 8192;
inline constexpr int DEBRIS_IMPULSES_PER_FRAME = 512;
inline constexpr int DEBRIS_WAKES_PER_FRAME = 128;

// A pool mesh instance's draw starts its instances here, so meshparticles.vp can tell it from a ring instance
// (gl_InstanceIndex includes firstInstance; a ring's instance list is at most 1,048,576 long).
inline constexpr int DEBRIS_MESH_INSTANCE_BASE = 1 << 24;

// "Debris time on the ground" (r_debris_life) scales every rest of DEBRIS_REST_SCALE_FROM seconds or more by
// r_debris_life / DEBRIS_REST_REFERENCE; shorter rests are part of an effect's look (an ember cooling) and stay
// as the definition wrote them.
inline constexpr float DEBRIS_REST_SCALE_FROM = 5.0f;
inline constexpr float DEBRIS_REST_REFERENCE = 60.0f;

// GPU: ONE PIECE, eleven vec4s, 176 bytes (set 1 binding 10, DebrisPoolSSO). Shader space: y is up.
//   Spawn[0..4]   the particle ring's own record (FLevelLocals::GpuParticleRecord), written once at spawn:
//                 a xyz spawn position, w birth (level seconds); b xyz launch velocity, w life (seconds);
//                 c rgb tint, w intensity; d x definition slot, y size scale, z ambient light, w seed;
//                 e xy the spawn's plane (octahedral; x = 2 none), z its offset, w the floor (-32768 none)
//   Position      xyz where the piece is after the last step; w state: 0 free, 1 flying, 2 resting
//   Previous      xyz where it was one step earlier; w the level time of Position, seconds
//   Velocity      xyz map units a second; w the level time its rest clock started (-1: not started)
//   Turn          its orientation, a unit quaternion (xyz, w)
//   PreviousTurn  the orientation one step earlier
//   Spin          xyz angular velocity, radians a second (world axes); w steps spent slow on a floor
struct DebrisPieceGpu
{
	float Spawn[5][4];
	float Position[4];
	float Previous[4];
	float Velocity[4];
	float Turn[4];
	float PreviousTurn[4];
	float Spin[4];
};

inline constexpr int DEBRIS_PIECE_BYTES = 176;
static_assert(sizeof(DebrisPieceGpu) == DEBRIS_PIECE_BYTES, "DebrisPieceGpu must be eleven vec4s -- see DebrisPiece in debris_step.comp, gpuparticles.vp and meshparticles.vp");
static_assert(offsetof(DebrisPieceGpu, Position) == 80 && offsetof(DebrisPieceGpu, Previous) == 96 && offsetof(DebrisPieceGpu, Velocity) == 112 &&
	offsetof(DebrisPieceGpu, Turn) == 128 && offsetof(DebrisPieceGpu, PreviousTurn) == 144 && offsetof(DebrisPieceGpu, Spin) == 160,
	"DebrisPieceGpu's offsets must match the std430 layout of DebrisPiece");

// The piece states (Position[3]).
inline constexpr float DEBRIS_STATE_FREE = 0.0f;
inline constexpr float DEBRIS_STATE_FLYING = 1.0f;
inline constexpr float DEBRIS_STATE_RESTING = 2.0f;

// GPU: ONE DEBRIS DEFINITION, five vec4s, 80 bytes, indexed by the named definition slot.
//   Physics    x restitution 0..1, y friction 0..1, z rest life (seconds, as written; 0 = never rests), w rest fade
//   Motion     x gravity (map units/s^2, down), y drag (1/s), z collide (1 plane, 2 level), w 1 = a debris
//              definition (0: pieces of this slot are freed -- the definition went away)
//   Body       x the first size key (map units; for a mesh, scale x diameter, as particledefs.cpp stores it),
//              y 1 = drawn as a mesh when meshes draw, z the mesh's diameter (0 for a billboard), w 0
//   BoundsMin  xyz the mesh's bounds (model axes, y up, md3 units), w 0
//   BoundsMax  xyz, w 0
struct DebrisDefinitionGpu
{
	float Physics[4];
	float Motion[4];
	float Body[4];
	float BoundsMin[4];
	float BoundsMax[4];
};

inline constexpr int DEBRIS_DEFINITION_BYTES = 80;
static_assert(sizeof(DebrisDefinitionGpu) == DEBRIS_DEFINITION_BYTES, "DebrisDefinitionGpu must be five vec4s -- see DebrisDefinition in the shaders");

// GPU: THE DEFINITIONS BUFFER (set 1 binding 11, DebrisDefinitionSSO; the step reads it too), laid out as the shaders
// declare it:
//   0                              vec4 info: x the rest scale (r_debris_life / 60), y DEBRIS_REST_SCALE_FROM,
//                                  z the pool's capacity, w the mesh instances uploaded
//   16                             DEBRIS_DEFINITION_SLOTS x DebrisDefinitionGpu
//   DEBRIS_MESH_INSTANCES_OFFSET   uint pool slots, one per pool mesh instance, grouped by definition
inline constexpr int DEBRIS_DEFINITIONS_HEADER_BYTES = 16;
inline constexpr int DEBRIS_MESH_INSTANCES_OFFSET = DEBRIS_DEFINITIONS_HEADER_BYTES + DEBRIS_DEFINITION_SLOTS * DEBRIS_DEFINITION_BYTES;	// 20,496
inline constexpr int DEBRIS_MESH_INSTANCE_BYTES = 4;

// GPU: ONE BLAST PUSH for a step (SH3's PushEffectImpulse, converted): Sphere xyz its centre (shader axes), w its
// radius; Push x its strength at the centre (map units a second, negative pulls in), y the step it belongs to.
struct DebrisImpulseGpu
{
	float Sphere[4];
	float Push[4];
};

// GPU: ONE WAKE BOX for a step: a sector under it moved (SH2). Min xyz (shader axes), w the step; Max xyz, w 0.
struct DebrisWakeGpu
{
	float Min[4];
	float Max[4];
};

static_assert(sizeof(DebrisImpulseGpu) == 32 && sizeof(DebrisWakeGpu) == 32, "DebrisImpulseGpu and DebrisWakeGpu must be two vec4s each");

// GPU: THE STEP'S EVENTS BUFFER (the step's own set, binding 2): the frame's impulses, then its wake boxes.
inline constexpr int DEBRIS_STEP_EVENT_BYTES = DEBRIS_IMPULSES_PER_FRAME * 32 + DEBRIS_WAKES_PER_FRAME * 32;	// 20,480

// CPU: one new piece this frame, copied into pool slot Slot before step Step (-1: after this frame's steps).
struct DebrisSpawnUpload
{
	uint32_t Slot = 0;
	int32_t Step = -1;
	DebrisPieceGpu Piece = {};
};

// [DEBRISPOOL] THIS FRAME'S POOL, decided on the CPU (hw_debrispool.cpp). The backend does it in this order:
// allocate or free; Clear; the definitions and the mesh instance list when their generations moved, and the info
// vec4 every frame; then for each step, that step's spawns, then the step; then the spawns for no step.
struct DebrisPoolFrame
{
	// Raised by every PrepareFrame; the backend acts on a frame once.
	uint64_t Serial = 0;

	// The pool should exist: Vulkan, r_gpuparticles, r_debris, and a debris burst on this map within the linger,
	// or pieces still alive. False frees it.
	bool Active = false;
	int Capacity = DEBRIS_POOL_DEFAULT;

	// Every piece freed: a new map, a savegame load, ClearGpuParticles, or a new allocation's first frame.
	bool Clear = false;

	// Steps this frame, 0..DEBRIS_STEPS_PER_FRAME, and the level time (seconds) each one ends at.
	int Steps = 0;
	float StepTime[DEBRIS_STEPS_PER_FRAME] = {};

	// Pieces [0, HighWater) are stepped; no slot at or past it has ever been used since the last Clear.
	int HighWater = 0;

	const DebrisSpawnUpload* Spawns = nullptr;
	int SpawnCount = 0;
	const DebrisImpulseGpu* Impulses = nullptr;
	int ImpulseCount = 0;
	const DebrisWakeGpu* Wakes = nullptr;
	int WakeCount = 0;
	bool WakeAll[DEBRIS_STEPS_PER_FRAME] = {};

	// Renderer-read every frame: r_debris_life / 60 and r_gpuparticles_sizescale.
	float RestScale = 1.0f;
	float SizeScale = 1.0f;

	// DEBRIS_DEFINITION_SLOTS records, rewritten when DefinitionGeneration moves.
	const DebrisDefinitionGpu* Definitions = nullptr;
	uint64_t DefinitionGeneration = 0;

	// The pool mesh instance list, rewritten when MeshInstanceGeneration moves.
	const uint32_t* MeshInstances = nullptr;
	int MeshInstanceCount = 0;
	uint64_t MeshInstanceGeneration = 0;
};

// [DEBRISPOOL] What the backend did, for the CPU side's next frame and the draw. Renderer-internal (written by
// vk_debrispool.cpp and vk_descriptorset.cpp, read by hw_debrispool.cpp); never the playsim. Epoch rises with every
// allocation, so the CPU side knows its slot record no longer holds. Bound: set 1 bindings 10 and 11 hold the real
// buffers in THIS frame's set (VkDescriptorSetManager::UpdateHWBufferSet), so a draw may read them.
struct DebrisPoolBackendStatus
{
	bool Allocated = false;
	int Capacity = 0;
	int RefusedCapacity = 0;	// a capacity this device refused until the pool stops being asked for; 0 = none
	uint64_t Epoch = 0;
	uint64_t DefinitionGeneration = 0;		// the definitions the buffer holds
	uint64_t MeshInstanceGeneration = 0;	// the mesh instance list the buffer holds
	bool Bound = false;
};

inline DebrisPoolBackendStatus& DebrisPoolStatus()
{
	static DebrisPoolBackendStatus status;
	return status;
}

// "Engine docs/DEBRIS_9_IMPL_NOTES.md", deviation 1: the frame travels through here rather than as a member of
// FrameComputeInput (hw_framecompute.h), so this step stands without that file. Filled by DebrisPool::PrepareFrame
// right before RunFrameCompute; read by VkComputeManager::RunFrame.
inline DebrisPoolFrame& DebrisPoolFrameForBackend()
{
	static DebrisPoolFrame frame;
	return frame;
}
