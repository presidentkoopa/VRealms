/*
** particledefs.h
**
** [PARTICLEDEFS] GPU particle definitions -- the CPU table. What a particle
** looks like over its life (size, colour, occlusion and light ramps, motion,
** spin, collision, flipbook) is held here once per definition; each record in the
** particle ring carries only an index into this table plus what differs per
** particle. See "Engine docs/GPU_PARTICLES_STAGE2_PLAN.md" 2b.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** TWO KINDS OF DEFINITION, ONE TABLE OF 512 SLOTS
**
**   0..255    NAMED. Read from every PARTICLEDEFS lump at startup (any mod may ship
**             one; a later definition of the same name replaces an earlier one in
**             load order). Script gets a HANDLE for a name with
**             LevelLocals.ParticleDefinition and spawns with LevelLocals.SpawnParticles.
**
**   256..511  INLINE. SpawnGpuParticles' look -- sizes, gravity, drag, orient,
**             stretch -- becomes a definition the first time that tuple is used,
**             cached here. A slot is only rewritten once every particle that used
**             it has died, so live particles never change look.
**
** NETPLAY: particles are presentation only. The playsim decides an event, particles
** draw it, and nothing reads them back -- so script never learns whether a
** definition exists on this machine. It addresses one by a handle worked out from
** the name's text alone (ParticleDefinitionHandle), which is the same number on
** every machine whether that machine's lumps defined the name, refused it or never
** had it. Only the renderer-side spawn resolves a handle, and a handle that
** resolves to nothing changes nothing but this machine's pixels.
**
** This half survives a renderer rebuild. ParticleDefinitionBuffer
** (hw_particledefbuffer.h) is the GPU copy, synced from HWDrawInfo::ProcessScene.
**
** [2c] FLIPBOOKS. A named definition's `texture` names its first frame and a count;
** the frames are found by name at load (particledefs.cpp, FindFlipbookFrames) and
** listed here as particle atlas layers, which the renderer builds the atlas from
** (ParticleDefinitionBuffer::SyncAtlasLayers, VkTextureManager::CreateParticleAtlas).
** The frame list survives a renderer rebuild; the atlas image does not.
**
*/

#pragma once

#include <cstdint>

#include "zstring.h"	// [DEBRISSOUNDS] ParticleDebrisDefinition's sound names

// One definition as the GPU reads it: sixteen vec4s, 256 bytes, std430 with no
// padding. Must match ParticleDefinitionData in vk_shader.cpp's prolog and
// ParticleDefinitionBuffer::RECORD_BYTES (particledefs.cpp asserts both size and
// offsets).
//
// Ramps share their time keys: `key[i].x` is the life fraction of key i for every
// channel. The parser merges the times each ramp was written with, so a lump may
// give size and colour different times (up to 8 distinct times in all).
struct ParticleDefinitionGpu
{
	float key[8][4];		// x t (life fraction 0..1)  y size (diameter, map units)  z alpha 0..1  w emissive
	float keyColor[2][4];	// per key, 0xRRGGBB as an exact float (keys 0-3, then 4-7); multiplies the record's tint
	float motion[4];		// x gravity u/s^2 (down)  y drag 1/s  z maxsize (0 = r_gpuparticles_maxsize alone)  w key count 1..8
	float shape[4];			// x orient 0 billboard 1 streak 2 flake  y stretch (s)  z spin min  w spin max (deg/s, picked by seed)
	float look[4];			// x lit 0..1 (2d)  y soft map units (2d; -1 = not set)  z collide 0 none 1 plane 2 level (#8)  w flags (PDF_*)
	float flipbook[4];		// x first atlas layer (-1 = no texture; 2c)  y frames  z fps  w 0 loop, 1 once
	// [LOOKS] The generated look ("Engine docs/GPU_PARTICLE_LOOKS_PLAN.md"), in what was spare room, so no
	// layout change -- the member keeps its name because the prolog's copy of this struct is shared by every
	// Vulkan shader; gpuparticles.vp/.fp name the fields with local #defines. All zero = no look (every
	// inline definition, and every named one without a `look`), which draws exactly as before looks.
	//   [0]  x kind (EParticleLook)  y roughness 0..1  z churn, noise cells a second  w detail, octaves 1..4
	//   [1]  x heat start 0..1  y heat end 0..1  z prongs, min * 32 + max  w rise, map units a second
	float spare[2][4];
};

enum
{
	// The stage 1 fade: emissive (and, from 2d, alpha) times 1 - smoothstep(0.6, 1, t).
	// `fade = smooth` in a lump; every inline definition has it.
	PDF_FADE_SMOOTH = 1,
	// [ATLASBC7] The flipbook's frames are in the COMPRESSED particle atlas (BC7, fixed set binding 10) rather than the
	// uncompressed one (binding 4); flipbook[0] counts layers within the atlas this names. Set only by the atlas layout
	// (particledefs.cpp, AssignAtlasLayers) -- no lump key writes it. gpuparticles.fp reads it; every other reader of these
	// flags tests PDF_FADE_SMOOTH alone.
	PDF_ATLAS_COMPRESSED = 2,
};

// [LOOKS] What spare[0][0] holds: the shape gpuparticles.fp generates for the particle
// instead of the round dot or a flipbook frame. The numbers are the plan's order and never
// change -- a later build adds a look by implementing its number, not by renumbering. Named
// for what they draw; any definition may use any of them.
enum EParticleLook
{
	PDL_NONE = 0,	// the round dot, or the flipbook (`texture`)
	PDL_DUST = 1,	// build A: a lit cloud that billows, tears and thins; never glows
	PDL_SMOKE = 2,	// build B
	PDL_FIRE = 3,	// build A: tongues on the heat ramp
	PDL_FLASH = 4,	// build B
	PDL_SPARK = 5,	// build B
	PDL_COUNT
};

// Reads every PARTICLEDEFS lump (d_main.cpp, after the textures). Named slots are
// cleared and refilled; the inline cache is left alone.
void LoadParticleDefinitions();

// The handle for a definition name: FNV-1a of the name lower-cased, 31 bits, never
// 0 and never negative. A pure function of the text -- it does NOT look at the
// table, so it cannot tell anyone whether the definition loaded. 0 is never a
// handle, so a zeroed script field means "no definition".
int ParticleDefinitionHandle(const char *name);

// The named slot a handle stands for on THIS machine, or -1 (no definition of that
// name loaded here, or handle 0). Render-side only: the answer can differ between
// machines, so nothing that affects the simulation may depend on it. With `report`,
// an unknown handle prints one console line, once per handle. Inline slots are
// never returned: they are recycled.
int ResolveParticleDefinitionHandle(int handle, bool report);

// SpawnGpuParticles' inline definition for this tuple, created or found. Never
// fails: with all 256 slots holding live particles, the one expiring soonest is
// replaced (and one warning is printed per level). `birth` is the spawn's level
// time in seconds, `longestLife` the longest life any of its records can have,
// `levelSerial` the spawning level's GpuParticleSerial.
int InlineParticleDefinition(float sizeStart, float sizeEnd, float gravity, float drag, int orient, float stretch,
	uint64_t levelSerial, double birth, double longestLife);

// For the renderer's sync (ParticleDefinitionBuffer::Sync).
const ParticleDefinitionGpu *ParticleDefinitionTableData();
const uint64_t *ParticleDefinitionSlotGenerations();
unsigned ParticleDefinitionSlotCount();
uint64_t ParticleDefinitionGeneration();

// [2c] For the renderer's atlas sync (ParticleDefinitionBuffer::SyncAtlasLayers):
// one entry per particle atlas layer, in layer order -- every frame a named
// definition's flipbook uses, a flipbook's frames on consecutive layers, identical
// runs shared -- and the list's generation, bumped each time LoadParticleDefinitions
// rebuilds it. A definition's first layer is its flipbook[0].
struct ParticleAtlasLayer;
const ParticleAtlasLayer *ParticleAtlasLayerData();
unsigned ParticleAtlasLayerCount();
uint64_t ParticleAtlasGeneration();

// [ATLASBC7] THE COMPRESSED PARTICLE ATLAS ("Engine docs/PARTICLE_ATLAS_COMPRESSED_IMPL_NOTES.md"). A flipbook whose every
// frame is a premultiplied BC7 DDS of one power-of-two side goes to a second, BC7 atlas (ParticleDefinitionBuffer::
// SyncAtlasLayers' compressed list, VkTextureManager::ParticleAtlasCompressed): its layer list in layer order, the side every
// layer has (0 = no compressed layers) and its own generation, which never goes backwards. A definition whose frames are
// there has PDF_ATLAS_COMPRESSED.
struct ParticleCompressedAtlasLayer;
const ParticleCompressedAtlasLayer *ParticleCompressedAtlasLayerData();
unsigned ParticleCompressedAtlasLayerCount();
int ParticleCompressedAtlasSide();
uint64_t ParticleCompressedAtlasGeneration();

// [ATLASBC7] Lays every flipbook's frames into the two atlases again when `policy` -- the renderer's atlas settings and what the
// device can hold (hw_particledefbuffer.h) -- differs from the one they were laid out with; the first call always does. Called by
// HWDrawInfo::ProcessScene every scene, so a menu setting applies with the menu open; a compare otherwise. CPU only: TexMan
// lookups, no pixels. A list's generation moves, and a definition is stamped, only when that list or that definition changed.
// Render-side only: the layout can differ between machines and changes nothing but this machine's pixels.
struct ParticleAtlasPolicy;
void RefreshParticleAtlasLayout(const ParticleAtlasPolicy &policy);

// [MESHPARTICLES] For the renderer's mesh particles (MeshParticleBuffer::SyncDefinitions,
// hw_meshparticles.h): one entry per named definition that names a `mesh`, in slot order, each
// checked when its lump loaded (an md3 of one surface and at most 64 triangles, the frame, the
// skin), and the list's generation, bumped each time LoadParticleDefinitions rebuilds it.
struct ParticleMeshDefinition;
const ParticleMeshDefinition *ParticleMeshDefinitionData();
unsigned ParticleMeshDefinitionCount();
uint64_t ParticleMeshGeneration();

// [DEBRISPOOL] DEBRIS THAT STAYS ("Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #9, "Engine docs/
// DEBRIS_9_IMPL_NOTES.md"). A named definition with a `restitution` key is a DEBRIS definition:
// FLevelLocals::SpawnParticles sends its bursts to the renderer's debris pool (hw_debrispool.h),
// where every piece is simulated -- it bounces, slides, comes to rest and stays -- instead of
// writing stateless ring records. Its keys live here, not in ParticleDefinitionGpu (whose 256
// bytes are all used), and reach the renderer as this list, the mesh list's way. Everything else
// about the piece's look is its ParticleDefinitionGpu slot.
struct ParticleDebrisDefinition
{
	int Slot = -1;				// the named definition slot, 0 .. 255
	float Restitution = 0.f;	// 0..1: the share of its speed into a surface a piece keeps when it bounces
	float Friction = 0.5f;		// 0..1: how much of a contact's push is taken off its sliding speed
	float RestLife = 0.f;		// seconds it stays once at rest; 0 = never rests (bounces until its life ends)
	float RestFade = 0.5f;		// seconds of fading out at the end of that rest

	// [DEBRISSOUNDS] Its landing sound ("Engine docs/DEBRIS_SOUNDS_11_IMPL_NOTES.md", hw_debrislanding.h): a group of these
	// pieces makes LandSound once, where and when its first piece lands; a group of more than LandBigCount makes
	// LandSoundBig instead when there is one. SNDINFO names, resolved by the renderer. Empty = silent (no `landsound`).
	FString LandSound;
	FString LandSoundBig;
	int LandBigCount = 12;
	float LandVolume = 1.f;			// 0..1: a group's volume at LandBigCount pieces or more
	float LandPitchMin = 0.94f;		// the range each landing's pitch is picked from (a client RNG)
	float LandPitchMax = 1.06f;
};

// One entry per named definition with `restitution`, in slot order, and the list's generation,
// bumped each time LoadParticleDefinitions rebuilds it.
const ParticleDebrisDefinition *ParticleDebrisDefinitionData();
unsigned ParticleDebrisDefinitionCount();
uint64_t ParticleDebrisGeneration();

// Whether this named slot's definition has `restitution` on this machine. False for -1, for inline
// slots and for every slot without the key. Render-side only, like ResolveParticleDefinitionHandle:
// the answer can differ between machines, so nothing that affects the simulation may depend on it.
bool ParticleDefinitionIsDebris(int slot);

// [PARTICLELIGHTS] A DEFINITION THAT THROWS LIGHT ("Engine docs/EFFECT_LIGHTS_LC_IMPL_NOTES.md"; "Engine docs/
// LIGHTS_20_21_22_PLAN.md" 2e). A named definition with a `light` radius above 0 makes its particles -- ring particles and debris
// pieces alike -- throw effect lights (hw_effectlights.h): of each burst a hashed `lightshare`, at most `lightmax`, each a short
// light that flies with its particle and, where it collides, lands and holds (hw_particlelights.h). Its keys live here, not in
// ParticleDefinitionGpu (whose 256 bytes are all used), and reach the renderer as this list, the debris list's way.
struct ParticleLightDefinition
{
	int Slot = -1;					// the named definition slot, 0 .. 255
	float Radius = 0.f;				// light = <radius>: map units, above 0 .. 1024
	float Intensity = 1.f;			// light = <radius>, <intensity>: 0 .. 16
	bool HasColor = false;			// lightcolor given; else the colour ramp's first key
	float Color[3] = { 1.f, 1.f, 1.f };	// lightcolor, 0..1 a channel
	int RampKeys = 0;				// lightramp: 0 = left off (1, fading as `fade = smooth`); else its keys of (life fraction, brightness)
	float RampTime[8] = {};
	float RampValue[8] = {};			// 0 .. 16
	float Share = 1.f;				// lightshare: the share of each burst's particles that carry a light, 0..1
	int Max = 16;					// lightmax: at most this many lights a burst, 1 .. 4096
	int Line = -1;					// lightline: -1 left off (a streak lights as a line), 0 a point, 1 a line along the streak
	float Hold = 0.f;				// lighthold: the most seconds a landed light keeps lighting, 0 .. 60
	int Priority = 1;				// lightpriority: 0 low, 1 normal, 2 important (ranks with flashes and tracers)
};

// One entry per named definition that throws light, in slot order, and the list's generation, bumped each time
// LoadParticleDefinitions rebuilds it.
const ParticleLightDefinition *ParticleLightDefinitionData();
unsigned ParticleLightDefinitionCount();
uint64_t ParticleLightGeneration();
