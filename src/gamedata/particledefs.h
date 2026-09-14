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
	float look[4];			// x lit 0..1 (2d)  y soft map units (2d; -1 = not set)  z collide 0 none 1 plane  w flags (PDF_*)
	float flipbook[4];		// x first atlas layer (-1 = no texture; 2c)  y frames  z fps  w 0 loop, 1 once
	float spare[2][4];		// zero; room for later steps without a layout change
};

enum
{
	// The stage 1 fade: emissive (and, from 2d, alpha) times 1 - smoothstep(0.6, 1, t).
	// `fade = smooth` in a lump; every inline definition has it.
	PDF_FADE_SMOOTH = 1,
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
