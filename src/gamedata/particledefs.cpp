/*
** particledefs.cpp
**
** [PARTICLEDEFS] GPU particle definitions: the PARTICLEDEFS reader, the table,
** the inline cache and the `particles` CCMD. See particledefs.h and
** "Engine docs/GPU_PARTICLES_STAGE2_PLAN.md" 2b.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** THE LUMP (any number of blocks; every key is optional)
**
**   particle flame_puff
**   {
**       size     = 6 @0, 28 @0.5, 40 @1          // value @ life fraction; up to 8 keys a ramp
**       color    = 255 245 200 @0, 120 40 10 @1  // 0..255, multiplies the spawn's tint
**       alpha    = 0 @0, 0.6 @0.2, 0 @1          // occlusion 0..1 (drawn from 2d; 0 = additive)
**       emissive = 3 @0, 0 @1                    // light it adds
**       lit      = 0                             // 0 unlit .. 1 lit by the lights in view (2d)
**       gravity  = -80                           // map units/s^2 downward; negative rises
**       drag     = 0.6                           // 1/s
**       maxsize  = 48                            // diameter cap; r_gpuparticles_maxsize still caps it
**       soft     = 8                             // map units (2d; until then r_gpuparticles_soft)
**       texture  = "RSSKA0", 6, 12, loop         // first frame, frames, fps, loop | once (2c)
**       orient   = billboard                     // billboard | streak | flake
**       stretch  = 0                             // streak: seconds of travel drawn
**       spin     = -90, 90                       // deg/s, picked per particle by its seed
**       collide  = plane                         // none | plane | level: SpawnParticles' surface and floor, or the whole level (below)
**       fade     = none                          // none | smooth: the stage 1 fade over the last 40% of life
**       look     = none                          // none | dust | fire: a generated shape (below); not with texture
**       mesh     = "models/debris/chip1.md3"     // draw each particle as this small 3D model (below)
**       restitution = 0.3                        // debris that bounces and stays: the debris pool (below)
**       friction = 0.5                           // debris: 0 slides on and on .. 1 stops at once
**       restlife = 60                            // debris: seconds it lies still before fading; 0 = never rests
**       restfade = 0.5                           // debris: seconds of that fade
**   }
**
** [LOOKS] GENERATED LOOKS ("Engine docs/GPU_PARTICLE_LOOKS_PLAN.md", build A). `look`
** draws the particle from noise generated on the GPU in place of the round dot or a
** flipbook, so no two particles are alike. Every other key still applies. The noise
** lives in the particle's own space and is world-sized, so both eyes see one shape.
**
**   look      = dust | fire   // smoke, flash and spark are later builds and refused here
**   roughness = 0.6           // how torn the edge is, 0..1                   (dust, fire)
**   churn     = 0.6           // how fast the shape turns over, cells a second, 0..16
**   detail    = 3             // noise octaves at the default quality, 1..4 (whole)
**   heat      = 1, 0.3        // fire: heat at birth, at death, 0 soot .. 1 white-hot
**   rise      = 24            // fire: map units a second the shape streams upward, 0..1000
**
** dust draws a lit body only (alpha; it never glows, whatever `emissive` says) and is
** shaded by the direction of the lights in view. fire glows along a fixed heat ramp
** (soot, deep red, orange, yellow, white) that the colour ramp multiplies. `look` and
** `texture` together, a key the look does not use, or a look key with no look refuses
** the definition. Left off: dust roughness 0.6, churn 0.6, detail 3; fire roughness
** 0.6, churn 1.5, detail 3, heat 1, 0.3, rise 24. r_gpuparticles_looks sets the
** quality (0 = every look as the plain round dot).
**
** [MESHPARTICLES] MESH PARTICLES ("Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #10). `mesh`
** draws each particle as a small 3D model -- a chip, a shard, a casing -- instanced, lit once
** for the whole chunk, in the opaque pass. Every other key still applies, and describes the
** billboard it draws as while "Mesh particles" (r_meshparticles) is off.
**
**   mesh = "<md3 path>"[, "<skin>"[, <frame>]]
**     path   the md3's full path inside its package, in quotes
**     skin   a file beside the md3, a full path or a texture name ("" or left off: the md3's own)
**     frame  a whole number, 0 when left off; the mesh is static at that frame
**
** Refused (the usual line, the rest of the lump still loads): not an MD3; not exactly ONE
** surface; no triangles or more than 64; more than 3 vertices a triangle; a triangle naming a
** vertex that is not there; a frame the file does not have; a skin that is not found; a frame
** whose vertices all sit at the origin.
** With a mesh, `size` is the model's SCALE: 1 (the default) draws it as modelled, 1 md3 unit to
** 1 map unit. It is stored as scale x the mesh's diameter, so the billboard it falls back to is as
** wide as the chunk. `spin` = min, max is its tumble, degrees a second about a random axis (the
** seed picks the axis, the rate and a random starting turn). With `collide = plane` (or `level`,
** which a ring mesh treats as `plane`; with `restitution` it is a debris piece, below) it rests ON
** surfaces, lands on the floor, settles flat and slides to a stop. `lit` and `color` light and
** colour it; `emissive` is glow added on top (0 for anything not hot); `alpha` does not apply,
** a mesh is opaque. It turns about its origin, so author it centred.
**
** [LEVELFIELD] COLLISION WITH THE WHOLE LEVEL ("Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #8).
** `collide = level` keeps a particle out of every floor, ceiling and wall near the player as they are
** NOW -- an open door is open, a lift carries what lies on it -- through the level collision field
** ("Particle collision", r_particlecollision, on by default):
**   - closer to a surface than its radius (half its drawn size) it is pushed out and slides along it;
**     a streak turns along the surface;
**   - falling onto a floor it lands and skids at that height;
**   - into a wall deeper than its radius (at least 1, at most 2 map units) it fades out and stays gone:
**     it never pops out of the far side;
**   - where the field has no answer (switched off, not built yet, out of its reach) it does what
**     `plane` does with SpawnParticles' surface and floor.
** Stateless like every ring particle: it does not bounce. Give the definition `restitution` and its
** particles go to the debris pool instead (below). A ring mesh with `collide = level` keeps the `plane`
** landing. r_particlecollision_test makes `plane` definitions use the field too, to judge it.
**
** [DEBRISPOOL] DEBRIS THAT STAYS ("Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #9, "Engine docs/
** DEBRIS_9_IMPL_NOTES.md"). A definition with `restitution` is DEBRIS: SpawnParticles sends its bursts to
** the debris pool ("Debris that stays", r_debris, on by default), where every piece is simulated each tic --
** it bounces off the level (the collision field where it answers, SpawnParticles' surface and floor where
** it does not), slides, rolls, comes to rest, stays, and is thrown again by PushEffectImpulse or by a floor
** that moves under it. Everything else about it is the definition's usual keys (a `mesh` piece settles
** flat on a face). Where the pool cannot take a burst (GL, the switch off) the same particles are drawn as
** ring particles, exactly as without these keys.
**
**   restitution = 0.3   // 0..1, the share of its speed into a surface a piece keeps when it bounces
**   friction    = 0.5   // 0..1, how fast it stops sliding (0.5 when left off)
**   restlife    = 60    // 0..600 seconds it lies still before fading; 0 or left off = NEVER RESTS: it
**                       // bounces and slides until its life ends and dies (sparks)
**   restfade    = 0.5   // 0..10 seconds of fading at the end of the rest (0.5 when left off)
**
** Its ramps run over `life` from its spawn and hold their last key after it. A piece that rests starts its
** rest when it lies still or when its life ends, whichever is first, stays `restlife` seconds and fades
** over `restfade` (a mesh shrinks away); `fade = smooth` does not apply to it. "Debris time on the ground"
** (r_debris_life, 60 by default) scales every restlife of 5 seconds or more by r_debris_life / 60; shorter
** rests (embers cooling on the floor) stay as written. A blast that throws a resting piece does not restart
** its rest. `friction`, `restlife` and `restfade` need `restitution`; `restfade` needs a `restlife`;
** `restitution` needs `collide = plane` or `level`.
**
** [DEBRISSOUNDS] LANDING SOUNDS ("Engine docs/DEBRIS_SOUNDS_11_IMPL_NOTES.md"). A debris definition may name
** the sound a GROUP of its pieces makes landing: one SpawnParticles burst the pool takes is one group (groups
** of the same sounds landing within 3 tics and 64 map units of each other are one), heard once, where and when
** its first piece is seen to land -- predicted as the burst goes into the pool, never read back.
**
**   landsound  = "rsb/debris/chips"                            // the sound, a SNDINFO name ($random is best)
**   landsound  = "rsb/debris/chips", "rsb/debris/rubble"       // ...and the one a group of MORE than 12 makes
**   landsound  = "rsb/debris/chips", "rsb/debris/rubble", 20   // ...more than 20 (a whole number, 1 .. 4096)
**   landvolume = 1           // 0..1, a group's volume at that count; fewer pieces get sqrt(pieces / count) of
**                            // it, never under a fifth (1 when left off)
**   landpitch  = 0.94, 1.06  // the pitch range each landing picks from, 0.25 .. 4 or one value (0.94, 1.06)
**
** They need `restitution`; `landvolume` and `landpitch` need `landsound`. A name SNDINFO does not have is not
** refused: the renderer says so once and those pieces land silently. SNDINFO $pitchshift and $pitchset do not
** apply (landpitch does); give each landing sound a `$limit` -- the engine caps how many start too
** (hw_debrislanding.h). "Debris landing sounds" (r_debris_sounds) and its volume are the player's.
**
** [PARTICLELIGHTS] LIGHT ("Engine docs/EFFECT_LIGHTS_LC_IMPL_NOTES.md", "Engine docs/LIGHTS_20_21_22_PLAN.md" 2e). A definition
** with `light` throws light: each of its particles that carries one -- a ring particle or a debris piece -- is an effect light
** ("Effect lights", r_effectlights) that flies with it and lights the walls, floors, models, sprites, smoke and other particles
** around it over its life. Where many crowd together they blend into one glow; past the light budget the small, far ones stop
** lighting first. Vulkan only. A definition without these keys throws no light.
**
**   light         = 48, 1.5        // radius in map units, 0 .. 1024 (0: no light); intensity 0 .. 16 (1 when left off)
**   lightcolor    = 255 170 80     // 0..255 (or 255, 170, 80); the colour ramp's first key when left off. The spawn's tint multiplies it
**   lightramp     = 2 @0, 0 @1     // its brightness over life, 0 .. 16, a ramp like `emissive`; left off: 1, fading as fade = smooth
**   lightshare    = 0.5            // the share of each burst's particles that carry a light, 0..1, picked by a hash of each (1)
**   lightmax      = 16             // at most this many lights a burst, a whole number 1 .. 4096 (16 when left off)
**   lightline     = 1              // a streak lights as a line along its drawn length (1, the default for orient = streak) or a point (0)
**   lighthold     = 1.5            // where it lands it keeps lighting, then cools, for a hashed share (a quarter to all) of this
**                                  // many seconds, 0 .. 60; 0 or left off: the light goes out where it lands
**   lightpriority = 1              // 0 low, 1 normal (left off), 2 important: ranks with flashes and tracers, which always light
**
** A ring particle lands where its flight first comes down onto its floor or a floor-like plane; a debris piece where its first
** landing is predicted (the landing sound's prediction). The spawn's `intensity` scales its light too. The other keys need `light`
** with a radius above 0; `lighthold` needs collide = plane or level; `lightline` needs orient = streak. Walls block a point light;
** a streak's line light they do not. "Particle light test" (r_particlelights_test) lights glowing definitions without keys.
**
** A ramp given one value with no '@' is constant. With several, every value needs
** '@t', 0 <= t <= 1, increasing; it holds its first value before the first key and
** its last after the last. Size, color, alpha and emissive SHARE up to 8 time keys
** on the GPU: the times of all four are merged, and each ramp is sampled at every
** merged time. For size, alpha and emissive that is exact (a straight line sampled
** on itself); a colour sampled at a time only another ramp uses is rounded to the
** nearest 1/255, because colours are stored 8 bits a channel.
**
** Defaults: size 4, color 255 255 255, alpha 0, emissive 1, lit 0, gravity 0,
** drag 0, maxsize 0 (no cap of its own), soft unset, no texture, billboard,
** stretch 0, spin 0, collide none, fade none.
**
** A bad block is refused -- one console line naming lump, line and definition --
** and the rest of the lump still loads (defblocks.cpp).
**
** [2c] FLIPBOOK FRAMES ("Engine docs/GPU_PARTICLES_STAGE2_PLAN.md" 2c, approved
** 2026-09-13). `texture` names the FIRST frame and a count; the later frames are
** found by name:
**   - A six-character SPRITE frame (four-letter sprite, frame, rotation) steps its
**     frame character and keeps the rotation: "RSSKA0", 6 is RSSKA0 .. RSSKF0,
**     over the engine's frames A-Z, then [ \ ] -- a count running past ']' is
**     refused, and so is an eight-character mirrored pair (RSBTA2A8) as a first
**     frame.
**   - Any other name ends in a decimal number that counts up, keeping its zero
**     padding and growing only when it must: SMOKE01 .. SMOKE06, PUFF8 PUFF9
**     PUFF10, SMOKE99 SMOKE100. The name is looked up as a sprite first, so SMOKE1
**     counts as a number only when no sprite of that name exists.
**   - One frame (or no count) is just that texture.
**   - A frame that is not there refuses that ONE definition, naming the frame.
** `loop` plays at fps; `once` spreads the frames over the particle's life; adjacent
** frames are blended. The frames go into the particle atlas, at most
** ParticleDefinitionBuffer::ATLAS_LAYERS layers across every definition (identical
** runs share layers); a definition that would pass that is refused. Inline
** definitions (SpawnGpuParticles) never have a texture.
**
*/

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include "particledefs.h"
#include "defblocks.h"
#include "hw_particledefbuffer.h"
#include "filesystem.h"
#include "printf.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "v_video.h"
#include "g_levellocals.h"
#include "texturemanager.h"	// [2c] flipbook frames are found by name
#include "hw_meshparticles.h"	// [MESHPARTICLES] ParticleMeshDefinition, MAX_TRIANGLES, the render state in `particles`
#include "model.h"	// [MESHPARTICLES] LoadSkin, the skin lookup the model loader uses
#include "m_swap.h"	// [MESHPARTICLES] LittleLong / LittleShort for the md3 check

static_assert(sizeof(ParticleDefinitionGpu) == ParticleDefinitionBuffer::RECORD_BYTES,
	"ParticleDefinitionGpu must be sixteen vec4s -- see ParticleDefinitionData in vk_shader.cpp");
static_assert(offsetof(ParticleDefinitionGpu, keyColor) == 128 && offsetof(ParticleDefinitionGpu, motion) == 160 &&
	offsetof(ParticleDefinitionGpu, shape) == 176 && offsetof(ParticleDefinitionGpu, look) == 192 &&
	offsetof(ParticleDefinitionGpu, flipbook) == 208 && offsetof(ParticleDefinitionGpu, spare) == 224,
	"ParticleDefinitionGpu offsets must match the std430 layout of ParticleDefinitionData in vk_shader.cpp");
// [2d] The bytes ParticleDefinitionBuffer reads to tell what a definition's particles need
// from the draw -- the premultiplied blend, the view lights, the read-only depth pass
// (ParticleDefinitionBuffer::GetSlotLooks).
static_assert(offsetof(ParticleDefinitionGpu, key) == 0 &&
	sizeof(ParticleDefinitionGpu::key) / sizeof(ParticleDefinitionGpu::key[0]) == ParticleDefinitionBuffer::KEYS &&
	sizeof(ParticleDefinitionGpu::key[0]) == ParticleDefinitionBuffer::KEY_STRIDE &&
	ParticleDefinitionBuffer::KEY_ALPHA_OFFSET == 2 * sizeof(float) &&
	ParticleDefinitionBuffer::KEY_COUNT_OFFSET == offsetof(ParticleDefinitionGpu, motion) + 3 * sizeof(float) &&
	ParticleDefinitionBuffer::LOOK_OFFSET == offsetof(ParticleDefinitionGpu, look),
	"ParticleDefinitionBuffer's look offsets must match ParticleDefinitionGpu: key[i].z alpha, motion.w key count, look.x lit, look.y soft");

namespace
{
	const unsigned kNamedSlots = ParticleDefinitionBuffer::NAMED_SLOTS;
	const unsigned kInlineSlots = ParticleDefinitionBuffer::INLINE_SLOTS;
	const unsigned kSlots = ParticleDefinitionBuffer::SLOTS;
	const unsigned kMaxKeys = 8;
	const double kNoUpperLimit = 1e30;

	// Seconds past an inline definition's last particle's death before its slot
	// may be rewritten: frames already recorded can still draw that particle with a
	// slightly older level time.
	const double kInlineReuseMargin = 0.5;

	const char *const kOrientNames[] = { "billboard", "streak", "flake" };
	// [LEVELFIELD] "level" (look.z 2, #8) appended; a value's number never changes.
	const char *const kCollideNames[] = { "none", "plane", "level" };
	const char *const kFadeNames[] = { "none", "smooth" };
	const char *const kModeNames[] = { "loop", "once" };
	// [LOOKS] In EParticleLook order (particledefs.h).
	const char *const kLookNames[] = { "none", "dust", "smoke", "fire", "flash", "spark" };
	static_assert(sizeof(kLookNames) / sizeof(kLookNames[0]) == PDL_COUNT, "kLookNames must name every EParticleLook");

	// [LOOKS] Defaults for a look's keys when a definition leaves them off.
	const double kLookRoughness = 0.6;
	const double kDustChurn = 0.6;
	const double kFireChurn = 1.5;
	const double kLookDetail = 3.0;
	const double kFireHeatStart = 1.0;
	const double kFireHeatEnd = 0.3;
	const double kFireRise = 24.0;

	// [DEBRISPOOL] Defaults and limits for a debris definition's keys (restitution itself is what makes it debris, so it
	// has no default).
	const double kDebrisFriction = 0.5;
	const double kDebrisRestFade = 0.5;
	const double kDebrisMaxRestLife = 600.0;
	const double kDebrisMaxRestFade = 10.0;

	// [DEBRISSOUNDS] Defaults and limits for a debris definition's landing sound keys (hw_debrislanding.h; the big count's
	// default is DebrisLanding::kDefaultBigCount).
	const int kLandBigCount = 12;
	const double kLandMaxBigCount = 4096.0;
	const double kLandVolume = 1.0;
	const double kLandPitchMin = 0.94;
	const double kLandPitchMax = 1.06;
	const double kLandLowestPitch = 0.25;
	const double kLandHighestPitch = 4.0;

	// [PARTICLELIGHTS] Limits and defaults of a definition's light keys (hw_particlelights.h resolves with the same).
	const double kLightMaxRadius = 1024.0;
	const double kLightMaxIntensity = 16.0;
	const double kLightMaxBrightness = 16.0;
	const double kLightShare = 1.0;
	const double kLightMax = 16.0;
	const double kLightMostPerBurst = 4096.0;
	const double kLightMaxHold = 60.0;
	const double kLightPriority = 1.0;

	struct NamedInfo
	{
		FString Name;
		FString Lump;
		int Line = 0;
		unsigned KeyCount = 0;
		FString Texture;	// the first frame as written ("" = no texture)
		int TextureLine = 0;		// [2c] the line of the `texture` key, for refusals
		TArray<FTextureID> Frames;	// [2c] the flipbook's frames, found by name at load (empty = no texture)
		int FirstLayer = -1;		// [2c] its first atlas layer once AssignAtlasLayers has run, -1 = none
		int Handle = 0;		// ParticleDefinitionHandle(Name)
		bool HasMesh = false;		// [MESHPARTICLES] the definition names a `mesh`
		int MeshLine = 0;			// [MESHPARTICLES] the line of the `mesh` key
		FString MeshSkinName;		// [MESHPARTICLES] the skin as written ("" = the md3's own)
		ParticleMeshDefinition Mesh;	// [MESHPARTICLES] checked at load; Slot filled when the list is built
		bool HasDebris = false;			// [DEBRISPOOL] the definition has `restitution`
		ParticleDebrisDefinition Debris;	// [DEBRISPOOL] its keys; Slot filled when the list is built
		bool HasLight = false;			// [PARTICLELIGHTS] the definition throws light (`light` with a radius above 0)
		ParticleLightDefinition Light;	// [PARTICLELIGHTS] its keys; Slot filled when the list is built
	};

	struct InlineInfo
	{
		bool Used = false;
		uint32_t Hash = 0;
		float Tuple[6] = {};
		uint64_t LevelSerial = 0;
		double Expiry = 0.0;	// level seconds; free once past, or once the level changes
		uint64_t Spawns = 0;
	};

	struct DefinitionTable
	{
		ParticleDefinitionGpu Gpu[kSlots];
		uint64_t SlotGeneration[kSlots];
		uint64_t Generation = 0;	// never goes backwards, even across a reload

		NamedInfo Named[kNamedSlots];
		unsigned NamedCount = 0;
		TMap<int, int> ByHandle;			// handle -> named slot
		TMap<int, bool> ReportedHandles;	// unknown handles already reported, so each prints once

		InlineInfo Inline[kInlineSlots];
		int LastInlineHit = -1;
		uint64_t WarnedFullSerial = 0;

		unsigned Lumps = 0;
		unsigned Replaced = 0;
		FDefBlockStats Stats;

		// [2c] The particle atlas layer list (AssignAtlasLayers) and its generation,
		// which never goes backwards, so a renderer's copy always notices a reload.
		TArray<ParticleAtlasLayer> AtlasLayers;
		uint64_t AtlasGeneration = 0;

		// [MESHPARTICLES] The definitions that name a mesh, in slot order (AssignMeshList), and the
		// list's generation, which never goes backwards, so the renderer's copy notices a reload.
		TArray<ParticleMeshDefinition> Meshes;
		uint64_t MeshGeneration = 0;

		// [DEBRISPOOL] The definitions with `restitution`, in slot order (AssignDebrisList), and the list's generation,
		// which never goes backwards.
		TArray<ParticleDebrisDefinition> Debris;
		uint64_t DebrisGeneration = 0;

		// [PARTICLELIGHTS] The definitions that throw light, in slot order (AssignLightList), and the list's generation, which never
		// goes backwards.
		TArray<ParticleLightDefinition> Lights;
		uint64_t LightGeneration = 0;

		DefinitionTable()
		{
			memset(Gpu, 0, sizeof(Gpu));
			memset(SlotGeneration, 0, sizeof(SlotGeneration));
		}

		void Stamp(unsigned slot)
		{
			Generation++;
			SlotGeneration[slot] = Generation;
		}
	};

	DefinitionTable &Table()
	{
		static DefinitionTable table;
		return table;
	}

	bool Fail(FString &error, int &errorLine, int line, const char *fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		error.VFormat(fmt, ap);
		va_end(ap);
		errorLine = line;
		return false;
	}

	//==========================================================================
	//
	// Values
	//
	//==========================================================================

	bool ReadNumber(const FDefBlockEntry &e, double lo, double hi, double &out, FString &error, int &errorLine)
	{
		if (e.Items.Size() != 1 || e.Items[0].Atoms.Size() != 1 || e.Items[0].HasAt || e.Items[0].Atoms[0].Kind != FDefBlockAtom::Number)
			return Fail(error, errorLine, e.Line, "'%s' takes one number", e.Key.GetChars());

		const double v = e.Items[0].Atoms[0].Value;
		if (!(v >= lo && v <= hi))
		{
			if (hi >= kNoUpperLimit)
				return Fail(error, errorLine, e.Line, "'%s' = %g must be %g or more", e.Key.GetChars(), v, lo);
			return Fail(error, errorLine, e.Line, "'%s' = %g is outside %g .. %g", e.Key.GetChars(), v, lo, hi);
		}
		out = v;
		return true;
	}

	// A single word from `choices`; its index in `out`.
	bool ReadChoice(const FDefBlockEntry &e, const char *const *choices, int count, int &out, FString &error, int &errorLine)
	{
		FString list;
		for (int i = 0; i < count; i++)
			list.AppendFormat("%s%s", i > 0 ? " | " : "", choices[i]);

		if (e.Items.Size() != 1 || e.Items[0].Atoms.Size() != 1 || e.Items[0].HasAt || e.Items[0].Atoms[0].Kind != FDefBlockAtom::Word)
			return Fail(error, errorLine, e.Line, "'%s' takes one of: %s", e.Key.GetChars(), list.GetChars());

		const FString &word = e.Items[0].Atoms[0].Text;
		for (int i = 0; i < count; i++)
		{
			if (word.CompareNoCase(choices[i]) == 0)
			{
				out = i;
				return true;
			}
		}
		return Fail(error, errorLine, e.Line, "'%s' = %s -- expected one of: %s", e.Key.GetChars(), word.GetChars(), list.GetChars());
	}

	// [LOOKS] `a, b` -- or one value for both -- each in lo .. hi, and whole numbers
	// only when `whole`. `usage` finishes the sentence "'<key>' is ...".
	bool ReadPair(const FDefBlockEntry &e, double lo, double hi, bool whole, const char *usage, double out[2], FString &error, int &errorLine)
	{
		const unsigned n = e.Items.Size();
		if (n < 1 || n > 2)
			return Fail(error, errorLine, e.Line, "'%s' is %s", e.Key.GetChars(), usage);
		double v[2] = { 0.0, 0.0 };
		for (unsigned k = 0; k < n; k++)
		{
			const FDefBlockItem &item = e.Items[k];
			if (item.Atoms.Size() != 1 || item.HasAt || item.Atoms[0].Kind != FDefBlockAtom::Number)
				return Fail(error, errorLine, item.Line, "'%s' is %s", e.Key.GetChars(), usage);
			const double x = item.Atoms[0].Value;
			if (!(x >= lo && x <= hi))
				return Fail(error, errorLine, item.Line, "'%s' value %g is outside %g .. %g", e.Key.GetChars(), x, lo, hi);
			if (whole && x != std::floor(x))
				return Fail(error, errorLine, item.Line, "'%s' value %g must be a whole number", e.Key.GetChars(), x);
			v[k] = x;
		}
		out[0] = v[0];
		out[1] = n == 2 ? v[1] : v[0];
		return true;
	}

	struct RampKey
	{
		double T = 0.0;
		double V[3] = { 0.0, 0.0, 0.0 };
	};

	struct Ramp
	{
		TArray<RampKey> Keys;
		bool Constant = true;	// one value: held for the whole life, adds no time key

		void SetConstant(double a, double b = 0.0, double c = 0.0)
		{
			RampKey key;
			key.V[0] = a; key.V[1] = b; key.V[2] = c;
			Keys.Clear();
			Keys.Push(key);
			Constant = true;
		}

		// Piecewise linear, held flat past both ends; exact at its own keys.
		double Sample(double t, int channel) const
		{
			if (Keys.Size() == 1 || t <= Keys[0].T) return Keys[0].V[channel];
			for (unsigned i = 1; i < Keys.Size(); i++)
			{
				const RampKey &b = Keys[i];
				if (t == b.T) return b.V[channel];
				if (t < b.T)
				{
					const RampKey &a = Keys[i - 1];
					const double f = (t - a.T) / (b.T - a.T);
					return a.V[channel] + (b.V[channel] - a.V[channel]) * f;
				}
			}
			return Keys.Last().V[channel];
		}
	};

	bool ReadRamp(const FDefBlockEntry &e, int channels, double lo, double hi, Ramp &ramp, FString &error, int &errorLine)
	{
		const unsigned n = e.Items.Size();
		if (n == 0)
			return Fail(error, errorLine, e.Line, "'%s' needs a value", e.Key.GetChars());
		if (n > kMaxKeys)
			return Fail(error, errorLine, e.Line, "'%s' has %u keys; a ramp holds at most %u", e.Key.GetChars(), n, kMaxKeys);

		ramp.Keys.Clear();
		ramp.Constant = (n == 1);

		for (unsigned i = 0; i < n; i++)
		{
			const FDefBlockItem &item = e.Items[i];
			if ((int)item.Atoms.Size() != channels)
			{
				if (channels == 3)
					return Fail(error, errorLine, item.Line, "each '%s' value is three numbers, red green blue 0..255 (found %u)", e.Key.GetChars(), item.Atoms.Size());
				return Fail(error, errorLine, item.Line, "each '%s' value is one number (found %u)", e.Key.GetChars(), item.Atoms.Size());
			}

			RampKey key;
			for (int c = 0; c < channels; c++)
			{
				const FDefBlockAtom &atom = item.Atoms[c];
				if (atom.Kind != FDefBlockAtom::Number)
					return Fail(error, errorLine, item.Line, "'%s' expects numbers, found '%s'", e.Key.GetChars(), atom.Text.GetChars());
				const double v = atom.Value;
				if (!(v >= lo && v <= hi))
				{
					if (hi >= kNoUpperLimit)
						return Fail(error, errorLine, item.Line, "'%s' value %g must be %g or more", e.Key.GetChars(), v, lo);
					return Fail(error, errorLine, item.Line, "'%s' value %g is outside %g .. %g", e.Key.GetChars(), v, lo, hi);
				}
				// Colours are stored 8 bits a channel.
				key.V[c] = channels == 3 ? std::round(v) : v;
			}

			if (n == 1)
			{
				// One value is constant whether or not it has a time.
				key.T = 0.0;
			}
			else
			{
				if (!item.HasAt)
					return Fail(error, errorLine, item.Line, "'%s' has %u values, so each needs '@ <life fraction>'", e.Key.GetChars(), n);
				if (!(item.At >= 0.0 && item.At <= 1.0))
					return Fail(error, errorLine, item.Line, "'%s' time @%g is outside 0 .. 1", e.Key.GetChars(), item.At);
				if (i > 0 && !(item.At > ramp.Keys.Last().T))
					return Fail(error, errorLine, item.Line, "'%s' times must increase: @%g follows @%g", e.Key.GetChars(), item.At, ramp.Keys.Last().T);
				key.T = item.At;
			}
			ramp.Keys.Push(key);
		}
		return true;
	}

	//==========================================================================
	//
	// [2c] Flipbook frames: finding them by name, and packing them into the
	// particle atlas's layer list
	//
	//==========================================================================

	// The engine's sprite frame characters: 'A'..'Z', then '[', '\', ']' -- 29 of
	// them, MAX_SPRITE_FRAMES in r_data/sprites.h.
	const int kSpriteFrames = 29;
#ifdef MAX_SPRITE_FRAMES
	static_assert(kSpriteFrames == MAX_SPRITE_FRAMES, "the flipbook sprite frame rule must cover exactly the engine's sprite frames");
#endif

	// A sprite frame character's index 0..28, or -1. Lower case counts as upper, as
	// in TexMan's name lookup.
	int SpriteFrameIndex(char ch)
	{
		if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
		const int index = ch - 'A';
		return (index >= 0 && index < kSpriteFrames) ? index : -1;
	}

	FTextureID NoTexture()
	{
		FTextureID id;
		id.SetInvalid();
		return id;
	}

	// A SPRITE of this name (TexMan's sprite use type, no alias), or an invalid id.
	FTextureID FindSpriteFrame(const char *name)
	{
		const FTextureID id = TexMan.CheckForTexture(name, ETextureType::Sprite, FTextureManager::TEXMAN_NoAlias);
		if (!id.isValid()) return NoTexture();
		FGameTexture *tex = TexMan.GetGameTexture(id);
		return (tex != nullptr && tex->GetUseType() == ETextureType::Sprite) ? id : NoTexture();
	}

	// Any texture of this name, as TexMan's any-type lookup finds it -- walls, flats,
	// patches, graphics, sprites, TEXTURES definitions -- or an invalid id. Never the
	// null texture.
	FTextureID FindAnyTexture(const char *name)
	{
		const FTextureID id = TexMan.CheckForTexture(name, ETextureType::Any, FTextureManager::TEXMAN_TryAny);
		return (id.isValid() && TexMan.GetGameTexture(id) != nullptr) ? id : NoTexture();
	}

	// THE APPROVED FRAME RULE (see the top of this file). Fills `frames` with `count`
	// textures starting at `first`, or returns false with the reason -- naming the
	// frame that is missing -- in `error`. Reads TexMan only; changes nothing.
	bool FindFlipbookFrames(const FString &first, int count, TArray<FTextureID> &frames, FString &error)
	{
		frames.Clear();
		const char *name = first.GetChars();
		const size_t length = first.Len();
		if (count < 1) count = 1;

		// Looked up as a sprite first, so a six-character texture such as SMOKE1 counts
		// as a number only when no sprite of that name exists.
		const FTextureID sprite = FindSpriteFrame(name);
		if (sprite.isValid() && length == 8 && SpriteFrameIndex(name[4]) >= 0 && SpriteFrameIndex(name[6]) >= 0)
		{
			error.Format("texture \"%s\" is a mirrored sprite pair (two rotations in one lump) and cannot be a first frame -- name a six-character sprite frame", name);
			return false;
		}

		if (sprite.isValid() && length == 6 && SpriteFrameIndex(name[4]) >= 0)
		{
			// RULE 1 -- a sprite frame name: step the frame character, keep the rotation.
			const int firstFrame = SpriteFrameIndex(name[4]);
			if (firstFrame + count > kSpriteFrames)
			{
				error.Format("texture \"%s\", %d runs past ']', the last sprite frame (frames go A-Z, then [ \\ ]) -- from '%c' there are at most %d",
					name, count, name[4], kSpriteFrames - firstFrame);
				return false;
			}

			char frameName[7];
			memcpy(frameName, name, 6);
			frameName[6] = 0;
			frames.Push(sprite);
			for (int i = 1; i < count; i++)
			{
				frameName[4] = (char)('A' + firstFrame + i);
				const FTextureID id = FindSpriteFrame(frameName);
				if (!id.isValid())
				{
					error.Format("sprite frame \"%s\" was not found (frame %d of %d, stepping the frame letter from \"%s\")", frameName, i + 1, count, name);
					return false;
				}
				frames.Push(id);
			}
			return true;
		}

		// RULE 2 -- any other name: its trailing decimal number counts up.
		const FTextureID firstId = sprite.isValid() ? sprite : FindAnyTexture(name);
		if (!firstId.isValid())
		{
			error.Format("texture \"%s\" was not found", name);
			return false;
		}
		frames.Push(firstId);
		if (count == 1) return true;

		size_t digitsAt = length;
		while (digitsAt > 0 && name[digitsAt - 1] >= '0' && name[digitsAt - 1] <= '9') digitsAt--;
		const int digits = (int)(length - digitsAt);
		if (digits == 0)
		{
			error.Format("texture \"%s\", %d: a flipbook's first frame must be a six-character sprite frame (such as \"RSSKA0\") or end in a number (such as \"SMOKE01\")", name, count);
			return false;
		}
		if (digits > 9)
		{
			error.Format("texture \"%s\": its frame number has %d digits, and at most 9 are counted", name, digits);
			return false;
		}

		const FString prefix = first.Left(digitsAt);
		const int firstNumber = atoi(name + digitsAt);
		for (int i = 1; i < count; i++)
		{
			// The zero padding as written, growing only when the number needs more
			// digits: 09 -> 10, 99 -> 100.
			FString number;
			number.Format("%d", firstNumber + i);
			FString frameName = prefix;
			for (int pad = (int)number.Len(); pad < digits; pad++) frameName += "0";
			frameName += number;

			const FTextureID id = FindAnyTexture(frameName.GetChars());
			if (!id.isValid())
			{
				error.Format("texture frame \"%s\" was not found (frame %d of %d, counting up from \"%s\")", frameName.GetChars(), i + 1, count, name);
				return false;
			}
			frames.Push(id);
		}
		return true;
	}

	// Identical runs -- the same first frame and the same count, and so the same
	// frames -- share their atlas layers.
	uint64_t FlipbookRunKey(const TArray<FTextureID> &frames)
	{
		return ((uint64_t)(uint32_t)frames[0].GetIndex() << 16) | (uint64_t)frames.Size();
	}

	// The atlas layers the named table uses, leaving out slot `skipSlot` (-1: none)
	// and adding `extra` (null: nothing). With skipSlot = the slot a definition
	// replaces and extra = its frames, this is exactly what the table would use
	// once it is accepted -- which is what the load-time budget check needs.
	unsigned AtlasLayersInUse(const DefinitionTable &table, int skipSlot, const TArray<FTextureID> *extra)
	{
		TMap<uint64_t, bool> runs;
		unsigned layers = 0;
		auto addRun = [&](const TArray<FTextureID> &frames)
		{
			if (frames.Size() == 0) return;
			const uint64_t key = FlipbookRunKey(frames);
			if (runs.CheckKey(key) != nullptr) return;
			runs.Insert(key, true);
			layers += frames.Size();
		};
		for (unsigned i = 0; i < table.NamedCount; i++)
		{
			if ((int)i != skipSlot) addRun(table.Named[i].Frames);
		}
		if (extra != nullptr) addRun(*extra);
		return layers;
	}

	// One flipbook's layers, appended. One scale for the whole run -- its largest
	// frame side (display size, so a high-resolution replacement frame keeps its
	// place) fills the layer -- and each frame centred in its layer.
	void AppendFlipbookLayers(TArray<ParticleAtlasLayer> &layers, const TArray<FTextureID> &frames)
	{
		double side = 0.0;
		for (unsigned i = 0; i < frames.Size(); i++)
		{
			FGameTexture *tex = TexMan.GetGameTexture(frames[i]);
			if (tex == nullptr) continue;
			side = std::max(side, (double)std::max(tex->GetDisplayWidth(), tex->GetDisplayHeight()));
		}

		for (unsigned i = 0; i < frames.Size(); i++)
		{
			ParticleAtlasLayer layer;
			layer.Texture = frames[i];
			layer.Left = 0.f;
			layer.Top = 0.f;
			layer.Width = 1.f;
			layer.Height = 1.f;
			FGameTexture *tex = TexMan.GetGameTexture(frames[i]);
			if (tex != nullptr && side > 0.0)
			{
				const double w = tex->GetDisplayWidth() / side;
				const double h = tex->GetDisplayHeight() / side;
				layer.Width = (float)w;
				layer.Height = (float)h;
				layer.Left = (float)((1.0 - w) * 0.5);
				layer.Top = (float)((1.0 - h) * 0.5);
			}
			layers.Push(layer);
		}
	}

	// Gives every textured named definition its first atlas layer, in slot order,
	// once every lump has loaded -- so a definition replaced by a later lump leaves
	// no orphaned layers -- and rebuilds the layer list the renderer builds the atlas
	// from. Returns how many definitions have a texture.
	unsigned AssignAtlasLayers(DefinitionTable &table)
	{
		TMap<uint64_t, int> runStart;
		table.AtlasLayers.Clear();
		unsigned textured = 0;
		for (unsigned i = 0; i < table.NamedCount; i++)
		{
			NamedInfo &n = table.Named[i];
			int firstLayer = -1;
			if (n.Frames.Size() > 0)
			{
				textured++;
				const uint64_t key = FlipbookRunKey(n.Frames);
				if (const int *start = runStart.CheckKey(key))
				{
					firstLayer = *start;
				}
				else if (table.AtlasLayers.Size() + n.Frames.Size() <= ParticleDefinitionBuffer::ATLAS_LAYERS)
				{
					firstLayer = (int)table.AtlasLayers.Size();
					runStart.Insert(key, firstLayer);
					AppendFlipbookLayers(table.AtlasLayers, n.Frames);
				}
				else
				{
					// Unreachable: the handler refuses a definition that would not fit, and
					// counts exactly what is live once it is accepted.
					Printf(TEXTCOLOR_ORANGE "ParticleAtlas: '%s' (%s, line %d) did not fit in the atlas and draws untextured\n",
						n.Name.GetChars(), n.Lump.GetChars(), n.Line);
				}
			}
			n.FirstLayer = firstLayer;
			if (table.Gpu[i].flipbook[0] != (float)firstLayer)
			{
				table.Gpu[i].flipbook[0] = (float)firstLayer;
				table.Stamp(i);
			}
		}
		table.AtlasGeneration++;
		return textured;
	}

	//==========================================================================
	//
	// [MESHPARTICLES] The `mesh` key: the md3 is read and checked here, at load,
	// so a mesh the renderer cannot draw well is refused with the lump's usual
	// line instead of drawing wrong. The renderer loads the same path through the
	// model loader later (MeshParticleBuffer): Models is emptied by InitModels
	// after this runs, so no model index is kept here.
	//
	//==========================================================================

	// The most triangles a mesh particle may have (the cost target depends on it), and the most
	// vertices such a surface can sensibly use: three a triangle.
	const unsigned kMeshMaxTriangles = MeshParticleBuffer::MAX_TRIANGLES;
	const unsigned kMeshMaxVertices = 3 * MeshParticleBuffer::MAX_TRIANGLES;

	// The MD3 structures FMD3Model reads (models_md3.cpp), 4-byte packed, little-endian.
	const uint64_t kMd3HeaderBytes = 108;		// ... Num_Frames 76, Num_Surfaces 84, Ofs_Surfaces 100
	const uint64_t kMd3SurfaceBytes = 108;		// ... Num_Shaders 76, Num_Verts 80, Num_Triangles 84, Ofs_Triangles 88,
												//     Ofs_Shaders 92, Ofs_Texcoord 96, Ofs_XYZNormal 100 (offsets from the surface)
	const uint64_t kMd3ShaderBytes = 68;		// char Name[64], index
	const uint64_t kMd3TriangleBytes = 12;		// three vertex indices
	const uint64_t kMd3TexcoordBytes = 8;		// s, t
	const uint64_t kMd3VertexBytes = 8;			// short x, y, z (1/64 units), packed normal

	bool Md3Read32(const uint8_t *bytes, size_t length, uint64_t offset, uint32_t &out)
	{
		if (offset + 4 > length) return false;
		uint32_t value;
		memcpy(&value, bytes + offset, sizeof(value));
		out = LittleLong(value);
		return true;
	}

	// `mesh = "<path>"[, "<skin>"[, <frame>]]`, checked: an MD3 of exactly one surface, 1 .. 64
	// triangles, vertex indices in range, the frame there, the skin found, a frame with some size.
	// Fills `mesh` (all but Slot) with the drawn frame's bounds in the model vertex buffer's axes.
	bool ReadParticleMesh(const FString &path, const FString &skinName, int frame, ParticleMeshDefinition &mesh, FString &error)
	{
		mesh = ParticleMeshDefinition();

		const int lump = fileSystem.CheckNumForFullName(path.GetChars());
		if (lump < 0)
		{
			error.Format("mesh \"%s\" was not found -- give the md3's full path inside its package, such as \"models/debris/concrete/chip1.md3\"", path.GetChars());
			return false;
		}
		const auto data = fileSystem.ReadFile(lump);
		const uint8_t *bytes = (const uint8_t *)data.data();
		const size_t length = data.size();
		if (bytes == nullptr || length < kMd3HeaderBytes || memcmp(bytes, "IDP3", 4) != 0)
		{
			error.Format("mesh \"%s\" is not an MD3 model -- mesh particles draw md3s", path.GetChars());
			return false;
		}

		uint32_t numFrames = 0, numSurfaces = 0, surfaceOffset = 0;
		Md3Read32(bytes, length, 76, numFrames);
		Md3Read32(bytes, length, 84, numSurfaces);
		Md3Read32(bytes, length, 100, surfaceOffset);
		if (numSurfaces != 1)
		{
			error.Format("mesh \"%s\" has %u surfaces -- a mesh particle is ONE surface with one skin (merge its parts)", path.GetChars(), numSurfaces);
			return false;
		}
		if (numFrames < 1)
		{
			error.Format("mesh \"%s\" has no frames", path.GetChars());
			return false;
		}
		if ((uint32_t)frame >= numFrames)
		{
			error.Format("mesh \"%s\" has no frame %d -- it has %u (0 .. %u)", path.GetChars(), frame, numFrames, numFrames - 1);
			return false;
		}

		const uint64_t surface = surfaceOffset;
		uint32_t numShaders = 0, numVertices = 0, numTriangles = 0, triangleOffset = 0, shaderOffset = 0, texcoordOffset = 0, vertexOffset = 0;
		if (surface + kMd3SurfaceBytes > length ||
			!Md3Read32(bytes, length, surface + 76, numShaders) || !Md3Read32(bytes, length, surface + 80, numVertices) ||
			!Md3Read32(bytes, length, surface + 84, numTriangles) || !Md3Read32(bytes, length, surface + 88, triangleOffset) ||
			!Md3Read32(bytes, length, surface + 92, shaderOffset) || !Md3Read32(bytes, length, surface + 96, texcoordOffset) ||
			!Md3Read32(bytes, length, surface + 100, vertexOffset))
		{
			error.Format("mesh \"%s\" is cut short: its surface lies past the end of the file", path.GetChars());
			return false;
		}
		if (numTriangles < 1 || numTriangles > kMeshMaxTriangles)
		{
			error.Format("mesh \"%s\" has %u triangles -- a mesh particle has 1 .. %u (thousands are drawn at once)", path.GetChars(), numTriangles, kMeshMaxTriangles);
			return false;
		}
		if (numVertices < 3 || numVertices > kMeshMaxVertices)
		{
			error.Format("mesh \"%s\" has %u vertices -- its %u triangles can use 3 .. %u", path.GetChars(), numVertices, numTriangles, kMeshMaxVertices);
			return false;
		}
		if (surface + triangleOffset + (uint64_t)numTriangles * kMd3TriangleBytes > length ||
			surface + texcoordOffset + (uint64_t)numVertices * kMd3TexcoordBytes > length ||
			surface + vertexOffset + (uint64_t)numVertices * numFrames * kMd3VertexBytes > length)
		{
			error.Format("mesh \"%s\" is cut short: its triangles, texture coordinates or vertices run past the end of the file", path.GetChars());
			return false;
		}
		for (uint32_t tri = 0; tri < numTriangles; tri++)
		{
			for (uint32_t corner = 0; corner < 3; corner++)
			{
				uint32_t index = 0;
				Md3Read32(bytes, length, surface + triangleOffset + (uint64_t)tri * kMd3TriangleBytes + corner * 4, index);
				if (index >= numVertices)
				{
					error.Format("mesh \"%s\": triangle %u uses vertex %u, and the surface has %u", path.GetChars(), tri, index, numVertices);
					return false;
				}
			}
		}

		// The drawn frame's bounds and reach, in the axes FMD3Model::BuildVertexBuffer gives the vertex
		// buffer: md3 (x, y, z) as (x, z, y), so y is up, in md3 units -- map units at scale 1.
		float boundsMin[3] = { 1.0e30f, 1.0e30f, 1.0e30f };
		float boundsMax[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
		double farthest = 0.0;
		const uint64_t frameVertices = surface + vertexOffset + (uint64_t)frame * numVertices * kMd3VertexBytes;
		for (uint32_t v = 0; v < numVertices; v++)
		{
			int16_t raw[3];
			memcpy(raw, bytes + frameVertices + (uint64_t)v * kMd3VertexBytes, sizeof(raw));
			const float x = LittleShort(raw[0]) / 64.f;
			const float y = LittleShort(raw[1]) / 64.f;
			const float z = LittleShort(raw[2]) / 64.f;
			const float axes[3] = { x, z, y };
			for (int a = 0; a < 3; a++)
			{
				boundsMin[a] = std::min(boundsMin[a], axes[a]);
				boundsMax[a] = std::max(boundsMax[a], axes[a]);
			}
			farthest = std::max(farthest, std::sqrt((double)x * x + (double)y * y + (double)z * z));
		}
		if (!(farthest > 0.0))
		{
			error.Format("mesh \"%s\": every vertex of frame %d is at the origin, so there is nothing to draw", path.GetChars(), frame);
			return false;
		}

		// The skin: as written -- a file beside the md3, a full path, or a texture name -- else the md3's
		// own (its surface's first shader name), looked up the way FMD3Model::Load looks it up.
		FString modelDirectory;
		const auto slash = path.LastIndexOf('/');
		if (slash >= 0) modelDirectory = path.Left((size_t)slash + 1);
		FTextureID skin;
		skin.SetInvalid();
		if (!skinName.IsEmpty())
		{
			skin = LoadSkin(modelDirectory.GetChars(), skinName.GetChars());
			if (!skin.isValid()) skin = LoadSkin("", skinName.GetChars());
			if (!skin.isValid())
			{
				error.Format("mesh \"%s\": its skin \"%s\" was not found (beside the md3, as a full path, or as a texture name)", path.GetChars(), skinName.GetChars());
				return false;
			}
		}
		else
		{
			FString own;
			if (numShaders >= 1 && surface + shaderOffset + kMd3ShaderBytes <= length)
			{
				char shaderName[65];
				memcpy(shaderName, bytes + surface + shaderOffset, 64);
				shaderName[64] = 0;
				own = shaderName;
				own.ReplaceChars('\\', '/');
			}
			if (own.IsEmpty())
			{
				error.Format("mesh \"%s\" names no skin of its own -- give one: mesh = \"%s\", \"<skin>\"", path.GetChars(), path.GetChars());
				return false;
			}
			skin = LoadSkin("", own.GetChars());
			if (!skin.isValid()) skin = LoadSkin(modelDirectory.GetChars(), own.GetChars());
			if (!skin.isValid())
			{
				error.Format("mesh \"%s\": its own skin \"%s\" was not found -- give one: mesh = \"%s\", \"<skin>\"", path.GetChars(), own.GetChars(), path.GetChars());
				return false;
			}
		}

		mesh.Path = path;
		mesh.Skin = skin;
		mesh.Frame = frame;
		mesh.Frames = numFrames;
		mesh.Vertices = numVertices;
		mesh.Triangles = numTriangles;
		for (int a = 0; a < 3; a++)
		{
			mesh.BoundsMin[a] = boundsMin[a];
			mesh.BoundsMax[a] = boundsMax[a];
		}
		mesh.Diameter = (float)(2.0 * farthest);
		return true;
	}

	// The mesh list the renderer syncs from, in slot order, once every lump has loaded (so a
	// definition replaced by a later lump hands over only its replacement). Returns its size.
	unsigned AssignMeshList(DefinitionTable &table)
	{
		table.Meshes.Clear();
		for (unsigned i = 0; i < table.NamedCount; i++)
		{
			NamedInfo &n = table.Named[i];
			if (!n.HasMesh) continue;
			n.Mesh.Slot = (int)i;
			table.Meshes.Push(n.Mesh);
		}
		table.MeshGeneration++;
		return table.Meshes.Size();
	}

	// [DEBRISPOOL] The debris list the renderer syncs from, in slot order, once every lump has loaded. Returns its size.
	unsigned AssignDebrisList(DefinitionTable &table)
	{
		table.Debris.Clear();
		for (unsigned i = 0; i < table.NamedCount; i++)
		{
			NamedInfo &n = table.Named[i];
			if (!n.HasDebris) continue;
			n.Debris.Slot = (int)i;
			table.Debris.Push(n.Debris);
		}
		table.DebrisGeneration++;
		return table.Debris.Size();
	}

	// [PARTICLELIGHTS] The light list the renderer syncs from, in slot order, once every lump has loaded. Returns its size.
	unsigned AssignLightList(DefinitionTable &table)
	{
		table.Lights.Clear();
		for (unsigned i = 0; i < table.NamedCount; i++)
		{
			NamedInfo &n = table.Named[i];
			if (!n.HasLight) continue;
			n.Light.Slot = (int)i;
			table.Lights.Push(n.Light);
		}
		table.LightGeneration++;
		return table.Lights.Size();
	}

	//==========================================================================
	//
	// One 'particle' block -> one GPU definition
	//
	//==========================================================================

	bool BuildDefinition(const FDefBlock &b, ParticleDefinitionGpu &gpu, NamedInfo &info, FString &error, int &errorLine)
	{
		if (b.Kind.CompareNoCase("particle") != 0)
			return Fail(error, errorLine, b.Line, "unknown block '%s'; PARTICLEDEFS holds 'particle <name> { ... }' blocks", b.Kind.GetChars());

		Ramp size, color, alpha, emissive;
		size.SetConstant(4.0);
		color.SetConstant(255.0, 255.0, 255.0);
		alpha.SetConstant(0.0);
		emissive.SetConstant(1.0);

		double lit = 0.0, gravity = 0.0, drag = 0.0, maxsize = 0.0, soft = -1.0, stretch = 0.0;
		double spinMin = 0.0, spinMax = 0.0;
		int orient = 0, collide = 0, fade = 0, mode = 0;
		FString texture;
		int textureLine = 0;	// [2c]
		double frames = 0.0, fps = 0.0;

		// [LOOKS] Read as written; checked against the look, and defaulted, after the loop.
		// A key's line stays 0 when the block leaves it off.
		int look = PDL_NONE;
		int lookLine = 0;
		double roughness = 0.0, churn = 0.0, detail = 0.0, rise = 0.0;
		double heat[2] = { 0.0, 0.0 }, prongs[2] = { 0.0, 0.0 };
		int roughnessLine = 0, churnLine = 0, detailLine = 0, riseLine = 0, heatLine = 0, prongsLine = 0;

		// [MESHPARTICLES] Read as written; the md3 is read and checked after the loop. sizeLine stays 0
		// when the block leaves `size` off, which for a mesh means scale 1.
		FString meshPath, meshSkin;
		int meshFrame = 0, meshLine = 0, sizeLine = 0;

		// [DEBRISPOOL] Read as written; checked after the loop. A key's line stays 0 when the block leaves it off.
		double restitution = 0.0, friction = kDebrisFriction, restLife = 0.0, restFade = kDebrisRestFade;
		int restitutionLine = 0, frictionLine = 0, restLifeLine = 0, restFadeLine = 0;
		// [DEBRISSOUNDS] Read as written; checked after the loop.
		FString landSound, landSoundBig;
		int landBigCount = kLandBigCount;
		double landVolume = kLandVolume;
		double landPitch[2] = { kLandPitchMin, kLandPitchMax };
		int landSoundLine = 0, landVolumeLine = 0, landPitchLine = 0;
		// [PARTICLELIGHTS] Read as written; checked after the loop. A key's line stays 0 when the block leaves it off.
		double lightRadius = 0.0, lightIntensity = 1.0, lightShare = kLightShare, lightMax = kLightMax, lightlineValue = 0.0, lightHold = 0.0;
		double lightPriority = kLightPriority;
		double lightColor[3] = { 255.0, 255.0, 255.0 };
		Ramp lightRamp;
		int lightKeyLine = 0, lightColorLine = 0, lightRampLine = 0, lightShareLine = 0, lightMaxLine = 0, lightlineLine = 0, lightHoldLine = 0;
		int lightPriorityLine = 0;

		for (unsigned i = 0; i < b.Entries.Size(); i++)
		{
			const FDefBlockEntry &e = b.Entries[i];
			for (unsigned j = 0; j < i; j++)
			{
				if (b.Entries[j].Key.CompareNoCase(e.Key) == 0)
					return Fail(error, errorLine, e.Line, "'%s' is given twice (first at line %d)", e.Key.GetChars(), b.Entries[j].Line);
			}

			bool ok = true;
			if (e.Key.CompareNoCase("size") == 0) { ok = ReadRamp(e, 1, 0.0, kNoUpperLimit, size, error, errorLine); sizeLine = e.Line; }
			else if (e.Key.CompareNoCase("color") == 0) ok = ReadRamp(e, 3, 0.0, 255.0, color, error, errorLine);
			else if (e.Key.CompareNoCase("alpha") == 0) ok = ReadRamp(e, 1, 0.0, 1.0, alpha, error, errorLine);
			else if (e.Key.CompareNoCase("emissive") == 0) ok = ReadRamp(e, 1, 0.0, kNoUpperLimit, emissive, error, errorLine);
			else if (e.Key.CompareNoCase("lit") == 0) ok = ReadNumber(e, 0.0, 1.0, lit, error, errorLine);
			else if (e.Key.CompareNoCase("gravity") == 0) ok = ReadNumber(e, -kNoUpperLimit, kNoUpperLimit, gravity, error, errorLine);
			else if (e.Key.CompareNoCase("drag") == 0) ok = ReadNumber(e, 0.0, kNoUpperLimit, drag, error, errorLine);
			else if (e.Key.CompareNoCase("maxsize") == 0) ok = ReadNumber(e, 0.0, kNoUpperLimit, maxsize, error, errorLine);
			else if (e.Key.CompareNoCase("soft") == 0) ok = ReadNumber(e, 0.0, kNoUpperLimit, soft, error, errorLine);
			else if (e.Key.CompareNoCase("stretch") == 0) ok = ReadNumber(e, 0.0, kNoUpperLimit, stretch, error, errorLine);
			else if (e.Key.CompareNoCase("orient") == 0) ok = ReadChoice(e, kOrientNames, 3, orient, error, errorLine);
			else if (e.Key.CompareNoCase("collide") == 0) ok = ReadChoice(e, kCollideNames, 3, collide, error, errorLine);
			else if (e.Key.CompareNoCase("fade") == 0) ok = ReadChoice(e, kFadeNames, 2, fade, error, errorLine);
			// [LOOKS] The generated look and its keys.
			else if (e.Key.CompareNoCase("look") == 0) { ok = ReadChoice(e, kLookNames, PDL_COUNT, look, error, errorLine); lookLine = e.Line; }
			else if (e.Key.CompareNoCase("roughness") == 0) { ok = ReadNumber(e, 0.0, 1.0, roughness, error, errorLine); roughnessLine = e.Line; }
			else if (e.Key.CompareNoCase("churn") == 0) { ok = ReadNumber(e, 0.0, 16.0, churn, error, errorLine); churnLine = e.Line; }
			else if (e.Key.CompareNoCase("rise") == 0) { ok = ReadNumber(e, 0.0, 1000.0, rise, error, errorLine); riseLine = e.Line; }
			else if (e.Key.CompareNoCase("detail") == 0)
			{
				ok = ReadNumber(e, 1.0, 4.0, detail, error, errorLine);
				if (ok && detail != std::floor(detail))
					return Fail(error, errorLine, e.Line, "'detail' = %g must be a whole number of noise octaves, 1 .. 4", detail);
				detailLine = e.Line;
			}
			else if (e.Key.CompareNoCase("heat") == 0)
			{
				ok = ReadPair(e, 0.0, 1.0, false, "start, end on the heat ramp, each 0 (soot) .. 1 (white-hot), or one value for both", heat, error, errorLine);
				heatLine = e.Line;
			}
			else if (e.Key.CompareNoCase("prongs") == 0)
			{
				ok = ReadPair(e, 2.0, 16.0, true, "min, max whole numbers 2 .. 16, or one value for both", prongs, error, errorLine);
				if (prongs[1] < prongs[0]) std::swap(prongs[0], prongs[1]);
				prongsLine = e.Line;
			}
			// [DEBRISPOOL] Debris that stays: the pool's keys.
			else if (e.Key.CompareNoCase("restitution") == 0) { ok = ReadNumber(e, 0.0, 1.0, restitution, error, errorLine); restitutionLine = e.Line; }
			else if (e.Key.CompareNoCase("friction") == 0) { ok = ReadNumber(e, 0.0, 1.0, friction, error, errorLine); frictionLine = e.Line; }
			else if (e.Key.CompareNoCase("restlife") == 0) { ok = ReadNumber(e, 0.0, kDebrisMaxRestLife, restLife, error, errorLine); restLifeLine = e.Line; }
			else if (e.Key.CompareNoCase("restfade") == 0) { ok = ReadNumber(e, 0.0, kDebrisMaxRestFade, restFade, error, errorLine); restFadeLine = e.Line; }
			// [DEBRISSOUNDS] The sound a group of these pieces makes landing.
			else if (e.Key.CompareNoCase("landsound") == 0)
			{
				// landsound = "<sound>"[, "<sound for a bigger group>"[, <pieces a group has more than to be bigger>]]
				const unsigned n = e.Items.Size();
				const char *usage = "'landsound' is \"<sound>\"[, \"<sound for a bigger group>\"[, <more pieces than this make a group bigger, 1 .. 4096>]]";
				if (n < 1 || n > 3)
					return Fail(error, errorLine, e.Line, "%s", usage);
				for (unsigned k = 0; k < n; k++)
				{
					if (e.Items[k].Atoms.Size() != 1 || e.Items[k].HasAt)
						return Fail(error, errorLine, e.Items[k].Line, "%s", usage);
				}
				for (unsigned k = 0; k < n && k < 2; k++)
				{
					const FDefBlockAtom &nameAtom = e.Items[k].Atoms[0];
					if (nameAtom.Kind != FDefBlockAtom::String || nameAtom.Text.IsEmpty())
						return Fail(error, errorLine, e.Items[k].Line, "%s -- sound names go in quotes, as SNDINFO names them", usage);
				}
				landSound = e.Items[0].Atoms[0].Text;
				landSoundBig = n >= 2 ? e.Items[1].Atoms[0].Text : FString();
				landBigCount = kLandBigCount;
				if (n >= 3)
				{
					const FDefBlockAtom &countAtom = e.Items[2].Atoms[0];
					if (countAtom.Kind != FDefBlockAtom::Number || !(countAtom.Value >= 1.0 && countAtom.Value <= kLandMaxBigCount) || countAtom.Value != std::floor(countAtom.Value))
						return Fail(error, errorLine, e.Items[2].Line, "'landsound' group size must be a whole number, 1 .. %g", kLandMaxBigCount);
					landBigCount = (int)countAtom.Value;
				}
				landSoundLine = e.Line;
			}
			else if (e.Key.CompareNoCase("landvolume") == 0) { ok = ReadNumber(e, 0.0, 1.0, landVolume, error, errorLine); landVolumeLine = e.Line; }
			else if (e.Key.CompareNoCase("landpitch") == 0)
			{
				ok = ReadPair(e, kLandLowestPitch, kLandHighestPitch, false, "min, max pitch, 0.25 .. 4 (1 = as recorded), or one value for both", landPitch, error, errorLine);
				if (landPitch[1] < landPitch[0]) std::swap(landPitch[0], landPitch[1]);
				landPitchLine = e.Line;
			}
			// [PARTICLELIGHTS] The light it throws.
			else if (e.Key.CompareNoCase("light") == 0)
			{
				// light = <radius>[, <intensity>]
				const unsigned n = e.Items.Size();
				const char *usage = "'light' is <radius>[, <intensity>] -- a radius 0 .. 1024 map units (0: no light), an intensity 0 .. 16";
				if (n < 1 || n > 2)
					return Fail(error, errorLine, e.Line, "%s", usage);
				for (unsigned k = 0; k < n; k++)
				{
					const FDefBlockItem &item = e.Items[k];
					if (item.Atoms.Size() != 1 || item.HasAt || item.Atoms[0].Kind != FDefBlockAtom::Number)
						return Fail(error, errorLine, item.Line, "%s", usage);
				}
				lightRadius = e.Items[0].Atoms[0].Value;
				if (!(lightRadius >= 0.0 && lightRadius <= kLightMaxRadius))
					return Fail(error, errorLine, e.Items[0].Line, "'light' radius %g is outside 0 .. %g", lightRadius, kLightMaxRadius);
				lightIntensity = n == 2 ? e.Items[1].Atoms[0].Value : 1.0;
				if (!(lightIntensity >= 0.0 && lightIntensity <= kLightMaxIntensity))
					return Fail(error, errorLine, e.Items[n - 1].Line, "'light' intensity %g is outside 0 .. %g", lightIntensity, kLightMaxIntensity);
				lightKeyLine = e.Line;
			}
			else if (e.Key.CompareNoCase("lightcolor") == 0)
			{
				// lightcolor = <red> <green> <blue> -- one colour, as `color` writes one -- or <red>, <green>, <blue>; 0 .. 255 each.
				const unsigned n = e.Items.Size();
				const char *usage = "'lightcolor' is one colour, red green blue 0 .. 255 -- a light's colour does not ramp (lightramp is its brightness over life)";
				if (n != 1 && n != 3)
					return Fail(error, errorLine, e.Line, "%s", usage);
				unsigned channel = 0;
				for (unsigned k = 0; k < n; k++)
				{
					const FDefBlockItem &item = e.Items[k];
					if (item.HasAt || item.Atoms.Size() != (n == 1 ? 3u : 1u))
						return Fail(error, errorLine, item.Line, "%s", usage);
					for (unsigned m = 0; m < item.Atoms.Size(); m++)
					{
						const FDefBlockAtom &atom = item.Atoms[m];
						if (atom.Kind != FDefBlockAtom::Number)
							return Fail(error, errorLine, item.Line, "%s", usage);
						if (!(atom.Value >= 0.0 && atom.Value <= 255.0))
							return Fail(error, errorLine, item.Line, "'lightcolor' value %g is outside 0 .. 255", atom.Value);
						lightColor[channel++] = atom.Value;
					}
				}
				lightColorLine = e.Line;
			}
			else if (e.Key.CompareNoCase("lightramp") == 0) { ok = ReadRamp(e, 1, 0.0, kLightMaxBrightness, lightRamp, error, errorLine); lightRampLine = e.Line; }
			else if (e.Key.CompareNoCase("lightshare") == 0) { ok = ReadNumber(e, 0.0, 1.0, lightShare, error, errorLine); lightShareLine = e.Line; }
			else if (e.Key.CompareNoCase("lightmax") == 0)
			{
				ok = ReadNumber(e, 1.0, kLightMostPerBurst, lightMax, error, errorLine);
				if (ok && lightMax != std::floor(lightMax))
					return Fail(error, errorLine, e.Line, "'lightmax' = %g must be a whole number of lights, 1 .. %g", lightMax, kLightMostPerBurst);
				lightMaxLine = e.Line;
			}
			else if (e.Key.CompareNoCase("lightline") == 0)
			{
				ok = ReadNumber(e, 0.0, 1.0, lightlineValue, error, errorLine);
				if (ok && lightlineValue != std::floor(lightlineValue))
					return Fail(error, errorLine, e.Line, "'lightline' is 0 (the light is a point) or 1 (a line along the streak)");
				lightlineLine = e.Line;
			}
			else if (e.Key.CompareNoCase("lighthold") == 0) { ok = ReadNumber(e, 0.0, kLightMaxHold, lightHold, error, errorLine); lightHoldLine = e.Line; }
			else if (e.Key.CompareNoCase("lightpriority") == 0)
			{
				ok = ReadNumber(e, 0.0, 2.0, lightPriority, error, errorLine);
				if (ok && lightPriority != std::floor(lightPriority))
					return Fail(error, errorLine, e.Line, "'lightpriority' is 0 (low), 1 (normal) or 2 (important)");
				lightPriorityLine = e.Line;
			}
			else if (e.Key.CompareNoCase("mesh") == 0)
			{
				// [MESHPARTICLES] mesh = "<md3 path>"[, "<skin>"[, <frame>]]. The file is read and checked
				// once every key is read (ReadParticleMesh, below the loop).
				const unsigned n = e.Items.Size();
				const char *usage = "'mesh' is \"<md3 path>\"[, \"<skin>\"[, <frame>]]";
				if (n < 1 || n > 3)
					return Fail(error, errorLine, e.Line, "%s", usage);
				for (unsigned k = 0; k < n; k++)
				{
					if (e.Items[k].Atoms.Size() != 1 || e.Items[k].HasAt)
						return Fail(error, errorLine, e.Items[k].Line, "%s", usage);
				}
				const FDefBlockAtom &pathAtom = e.Items[0].Atoms[0];
				if (pathAtom.Kind != FDefBlockAtom::String || pathAtom.Text.IsEmpty())
					return Fail(error, errorLine, e.Line, "'mesh' needs the md3's path first, in quotes, such as mesh = \"models/debris/concrete/chip1.md3\"");
				meshPath = pathAtom.Text;
				meshSkin = "";
				meshFrame = 0;
				if (n >= 2)
				{
					const FDefBlockAtom &skinAtom = e.Items[1].Atoms[0];
					if (skinAtom.Kind == FDefBlockAtom::Number)
						return Fail(error, errorLine, e.Items[1].Line, "'mesh' takes the skin second (in quotes; \"\" for the md3's own), then the frame");
					meshSkin = skinAtom.Text;
				}
				if (n >= 3)
				{
					const FDefBlockAtom &frameAtom = e.Items[2].Atoms[0];
					if (frameAtom.Kind != FDefBlockAtom::Number || !(frameAtom.Value >= 0.0 && frameAtom.Value <= 65535.0) || frameAtom.Value != std::floor(frameAtom.Value))
						return Fail(error, errorLine, e.Items[2].Line, "'mesh' frame must be a whole number, 0 or more");
					meshFrame = (int)frameAtom.Value;
				}
				meshLine = e.Line;
			}
			else if (e.Key.CompareNoCase("spin") == 0)
			{
				// spin = min, max -- or one value for both.
				const unsigned n = e.Items.Size();
				double v[2] = { 0.0, 0.0 };
				if (n < 1 || n > 2)
					return Fail(error, errorLine, e.Line, "'spin' is min, max in degrees a second (or one value)");
				for (unsigned k = 0; k < n; k++)
				{
					const FDefBlockItem &item = e.Items[k];
					if (item.Atoms.Size() != 1 || item.HasAt || item.Atoms[0].Kind != FDefBlockAtom::Number)
						return Fail(error, errorLine, item.Line, "'spin' is min, max in degrees a second (or one value)");
					v[k] = item.Atoms[0].Value;
				}
				spinMin = v[0];
				spinMax = n == 2 ? v[1] : v[0];
				if (spinMax < spinMin) std::swap(spinMin, spinMax);
			}
			else if (e.Key.CompareNoCase("texture") == 0)
			{
				// texture = "<first frame>", frames, fps, loop | once. Frames, fps and
				// mode may be left off (1, 0, loop): one still texture. The frames are
				// found once every key is read (FindFlipbookFrames, below the loop).
				const unsigned n = e.Items.Size();
				const char *usage = "'texture' is \"<first frame>\", frames, fps, loop | once";
				if (n < 1 || n > 4)
					return Fail(error, errorLine, e.Line, "%s", usage);
				for (unsigned k = 0; k < n; k++)
				{
					if (e.Items[k].Atoms.Size() != 1 || e.Items[k].HasAt)
						return Fail(error, errorLine, e.Items[k].Line, "%s", usage);
				}
				const FDefBlockAtom &name = e.Items[0].Atoms[0];
				if (name.Kind == FDefBlockAtom::Number || name.Text.IsEmpty())
					return Fail(error, errorLine, e.Line, "'texture' needs the first frame's name first");
				texture = name.Text;
				textureLine = e.Line;
				frames = 1.0;
				fps = 0.0;
				mode = 0;
				if (n >= 2)
				{
					const FDefBlockAtom &a = e.Items[1].Atoms[0];
					if (a.Kind != FDefBlockAtom::Number || a.Value < 1.0 || a.Value > 256.0 || a.Value != std::floor(a.Value))
						return Fail(error, errorLine, e.Items[1].Line, "'texture' frames must be a whole number 1 .. 256");
					frames = a.Value;
				}
				if (n >= 3)
				{
					const FDefBlockAtom &a = e.Items[2].Atoms[0];
					if (a.Kind != FDefBlockAtom::Number || !(a.Value >= 0.0 && a.Value <= 1000.0))
						return Fail(error, errorLine, e.Items[2].Line, "'texture' fps must be a number 0 .. 1000");
					fps = a.Value;
				}
				if (n >= 4)
				{
					const FDefBlockAtom &a = e.Items[3].Atoms[0];
					if (a.Kind != FDefBlockAtom::Word)
						return Fail(error, errorLine, e.Items[3].Line, "'texture' ends with loop or once");
					if (a.Text.CompareNoCase("loop") == 0) mode = 0;
					else if (a.Text.CompareNoCase("once") == 0) mode = 1;
					else return Fail(error, errorLine, e.Items[3].Line, "'texture' ends with loop or once, not '%s'", a.Text.GetChars());
				}
			}
			else
			{
				return Fail(error, errorLine, e.Line, "unknown key '%s'", e.Key.GetChars());
			}
			if (!ok) return false;
		}

		// [LOOKS] What a look allows, checked before the flipbook's frames are looked up. Each
		// refusal names the key's line; the rest of the lump still loads.
		if (look != PDL_NONE && !texture.IsEmpty())
			return Fail(error, errorLine, lookLine, "'look' and 'texture' cannot both be given (texture at line %d): a look is drawn in place of the flipbook", textureLine);
		if (look == PDL_SMOKE || look == PDL_FLASH || look == PDL_SPARK)
			return Fail(error, errorLine, lookLine, "look = %s is not in this engine yet (particle looks build B); this build draws dust and fire", kLookNames[look]);
		if (look == PDL_NONE)
		{
			const struct { const char *Key; int Line; } lookKeys[] = {
				{ "roughness", roughnessLine }, { "churn", churnLine }, { "detail", detailLine },
				{ "heat", heatLine }, { "rise", riseLine }, { "prongs", prongsLine } };
			for (const auto &k : lookKeys)
			{
				if (k.Line != 0)
					return Fail(error, errorLine, k.Line, "'%s' needs a look (look = dust | fire)", k.Key);
			}
		}
		else
		{
			const bool hot = look == PDL_FIRE || look == PDL_FLASH || look == PDL_SPARK;
			const bool rises = look == PDL_SMOKE || look == PDL_FIRE;
			if (heatLine != 0 && !hot)
				return Fail(error, errorLine, heatLine, "'heat' has no effect on look = %s -- it is for fire, flash and spark", kLookNames[look]);
			if (riseLine != 0 && !rises)
				return Fail(error, errorLine, riseLine, "'rise' has no effect on look = %s -- it is for smoke and fire", kLookNames[look]);
			if (prongsLine != 0 && look != PDL_FLASH)
				return Fail(error, errorLine, prongsLine, "'prongs' has no effect on look = %s -- it is for flash", kLookNames[look]);
		}

		// [MESHPARTICLES] The mesh, read and checked now that every key is known. Any failed check refuses
		// this definition alone, at the `mesh` line. `size` becomes the model's scale: 1 when left off, and
		// stored as scale x diameter, so the billboard fallback covers the chunk and the mesh shader can
		// divide it back out.
		ParticleMeshDefinition mesh;
		if (meshLine != 0)
		{
			if (!ReadParticleMesh(meshPath, meshSkin, meshFrame, mesh, error))
			{
				errorLine = meshLine;
				return false;
			}
			if (sizeLine == 0)
				size.SetConstant(1.0);
			for (RampKey &key : size.Keys)
				key.V[0] *= mesh.Diameter;
		}

		// [DEBRISPOOL] What the debris keys allow. Each refusal names the key's line; the rest of the lump still loads.
		if (restitutionLine == 0)
		{
			const struct { const char *Key; int Line; } debrisKeys[] = {
				{ "friction", frictionLine }, { "restlife", restLifeLine }, { "restfade", restFadeLine } };
			for (const auto &k : debrisKeys)
			{
				if (k.Line != 0)
					return Fail(error, errorLine, k.Line, "'%s' needs 'restitution' -- restitution = 0..1 makes the definition debris that bounces and stays", k.Key);
			}
			// [DEBRISSOUNDS] The landing sound keys likewise.
			const struct { const char *Key; int Line; } landKeys[] = {
				{ "landsound", landSoundLine }, { "landvolume", landVolumeLine }, { "landpitch", landPitchLine } };
			for (const auto &k : landKeys)
			{
				if (k.Line != 0)
					return Fail(error, errorLine, k.Line, "'%s' needs 'restitution' -- a landing sound is made by debris the debris pool simulates (restitution = 0..1)", k.Key);
			}
		}
		else
		{
			if (collide == 0)
				return Fail(error, errorLine, restitutionLine, "'restitution' needs something to bounce off: collide = plane or collide = level");
			if (restFadeLine != 0 && !(restLife > 0.0))
				return Fail(error, errorLine, restFadeLine, "'restfade' needs a restlife above 0 -- a piece that never rests does not fade at rest");
			// [DEBRISSOUNDS]
			if (landSoundLine == 0 && (landVolumeLine != 0 || landPitchLine != 0))
				return Fail(error, errorLine, landVolumeLine != 0 ? landVolumeLine : landPitchLine,
					"'%s' needs 'landsound' -- the sound a group of these pieces makes landing", landVolumeLine != 0 ? "landvolume" : "landpitch");
		}

		// [PARTICLELIGHTS] What the light keys allow. Each refusal names the key's line; the rest of the lump still loads.
		const bool throwsLight = lightKeyLine != 0 && lightRadius > 0.0;
		if (!throwsLight)
		{
			const struct { const char *Key; int Line; } lightKeys[] = {
				{ "lightcolor", lightColorLine }, { "lightramp", lightRampLine }, { "lightshare", lightShareLine }, { "lightmax", lightMaxLine },
				{ "lightline", lightlineLine }, { "lighthold", lightHoldLine }, { "lightpriority", lightPriorityLine } };
			for (const auto &k : lightKeys)
			{
				if (k.Line != 0)
					return Fail(error, errorLine, k.Line, "'%s' needs a light -- light = <radius>[, <intensity>] with a radius above 0 makes the definition throw one", k.Key);
			}
		}
		else
		{
			if (lightHoldLine != 0 && collide == 0)
				return Fail(error, errorLine, lightHoldLine, "'lighthold' needs something to land on: collide = plane or collide = level -- a light holds where its particle lands");
			if (lightlineLine != 0 && orient != 1)
				return Fail(error, errorLine, lightlineLine, "'lightline' has no effect on orient = %s -- only a streak has a length to light along", kOrientNames[orient]);
		}

		// [2c] The flipbook's frames, found by name now that the count is known. A
		// frame that is not there refuses this definition alone, naming the frame; the
		// rest of the lump still loads.
		TArray<FTextureID> frameIds;
		if (!texture.IsEmpty() && !FindFlipbookFrames(texture, (int)frames, frameIds, error))
		{
			errorLine = textureLine;
			return false;
		}

		// The shared time keys: every time any non-constant ramp was written with.
		TArray<double> times;
		for (const Ramp *r : { &size, &color, &alpha, &emissive })
		{
			if (r->Constant) continue;
			for (const RampKey &key : r->Keys) times.Push(key.T);
		}
		if (times.Size() > 1) std::sort(&times[0], &times[0] + times.Size());
		TArray<double> keyTimes;
		for (double t : times)
		{
			if (keyTimes.Size() == 0 || t != keyTimes.Last()) keyTimes.Push(t);
		}
		if (keyTimes.Size() == 0) keyTimes.Push(0.0);
		if (keyTimes.Size() > kMaxKeys)
		{
			return Fail(error, errorLine, b.Line, "size, color, alpha and emissive share %u time keys, and these ramps use %u different times",
				kMaxKeys, keyTimes.Size());
		}

		memset(&gpu, 0, sizeof(gpu));
		const unsigned count = keyTimes.Size();
		for (unsigned i = 0; i < kMaxKeys; i++)
		{
			// Keys past the count repeat the last one; the shader never reads them.
			const double t = keyTimes[i < count ? i : count - 1];
			gpu.key[i][0] = (float)t;
			gpu.key[i][1] = (float)size.Sample(t, 0);
			gpu.key[i][2] = (float)alpha.Sample(t, 0);
			gpu.key[i][3] = (float)emissive.Sample(t, 0);

			const int red = clamp((int)std::lround(color.Sample(t, 0)), 0, 255);
			const int green = clamp((int)std::lround(color.Sample(t, 1)), 0, 255);
			const int blue = clamp((int)std::lround(color.Sample(t, 2)), 0, 255);
			gpu.keyColor[i / 4][i % 4] = (float)((red << 16) | (green << 8) | blue);
		}

		gpu.motion[0] = (float)gravity;
		gpu.motion[1] = (float)drag;
		gpu.motion[2] = (float)maxsize;
		gpu.motion[3] = (float)count;

		gpu.shape[0] = (float)orient;
		gpu.shape[1] = (float)stretch;
		gpu.shape[2] = (float)spinMin;
		gpu.shape[3] = (float)spinMax;

		gpu.look[0] = (float)lit;
		gpu.look[1] = (float)soft;
		gpu.look[2] = (float)collide;
		gpu.look[3] = (float)(fade ? PDF_FADE_SMOOTH : 0);

		gpu.flipbook[0] = -1.f;	// [2c] no atlas layer until AssignAtlasLayers, once every lump has loaded
		gpu.flipbook[1] = (float)frames;
		gpu.flipbook[2] = (float)fps;
		gpu.flipbook[3] = (float)mode;

		// [LOOKS] spare[0..1] (particledefs.h), with the look's defaults for keys left off.
		// No look leaves all eight zero -- the definition's bytes are exactly as before looks.
		if (look != PDL_NONE)
		{
			const bool fire = look == PDL_FIRE;
			gpu.spare[0][0] = (float)look;
			gpu.spare[0][1] = (float)(roughnessLine != 0 ? roughness : kLookRoughness);
			gpu.spare[0][2] = (float)(churnLine != 0 ? churn : (fire ? kFireChurn : kDustChurn));
			gpu.spare[0][3] = (float)(detailLine != 0 ? detail : kLookDetail);
			gpu.spare[1][0] = (float)(heatLine != 0 ? heat[0] : (fire ? kFireHeatStart : 0.0));
			gpu.spare[1][1] = (float)(heatLine != 0 ? heat[1] : (fire ? kFireHeatEnd : 0.0));
			gpu.spare[1][2] = (float)(prongsLine != 0 ? prongs[0] * 32.0 + prongs[1] : 0.0);
			gpu.spare[1][3] = (float)(riseLine != 0 ? rise : (fire ? kFireRise : 0.0));
		}

		info.Name = b.Name;
		info.Lump = b.LumpName;
		info.Line = b.Line;
		info.KeyCount = count;
		info.Texture = texture;
		info.TextureLine = textureLine;
		info.Frames = frameIds;
		info.HasMesh = meshLine != 0;	// [MESHPARTICLES]
		info.MeshLine = meshLine;
		info.MeshSkinName = meshSkin;
		info.Mesh = mesh;
		info.HasDebris = restitutionLine != 0;	// [DEBRISPOOL]
		if (info.HasDebris)
		{
			info.Debris.Restitution = (float)restitution;
			info.Debris.Friction = (float)friction;
			info.Debris.RestLife = (float)restLife;
			info.Debris.RestFade = (float)restFade;
			// [DEBRISSOUNDS]
			info.Debris.LandSound = landSound;
			info.Debris.LandSoundBig = landSoundBig;
			info.Debris.LandBigCount = landBigCount;
			info.Debris.LandVolume = (float)landVolume;
			info.Debris.LandPitchMin = (float)landPitch[0];
			info.Debris.LandPitchMax = (float)landPitch[1];
		}
		info.HasLight = throwsLight;	// [PARTICLELIGHTS]
		if (throwsLight)
		{
			ParticleLightDefinition &light = info.Light;
			light.Radius = (float)lightRadius;
			light.Intensity = (float)lightIntensity;
			light.HasColor = lightColorLine != 0;
			for (int c = 0; c < 3; c++)
				light.Color[c] = (float)(lightColor[c] / 255.0);
			light.RampKeys = lightRampLine != 0 ? (int)lightRamp.Keys.Size() : 0;
			for (unsigned k = 0; k < (unsigned)light.RampKeys; k++)
			{
				light.RampTime[k] = (float)lightRamp.Keys[k].T;
				light.RampValue[k] = (float)lightRamp.Keys[k].V[0];
			}
			light.Share = (float)lightShare;
			light.Max = (int)lightMax;
			light.Line = lightlineLine != 0 ? (int)lightlineValue : -1;
			light.Hold = (float)lightHold;
			light.Priority = (int)lightPriority;
		}
		return true;
	}
}

//==========================================================================
//
// Loading
//
//==========================================================================

void LoadParticleDefinitions()
{
	DefinitionTable &table = Table();

	for (unsigned i = 0; i < table.NamedCount; i++)
	{
		memset(&table.Gpu[i], 0, sizeof(ParticleDefinitionGpu));
		table.Stamp(i);
		table.Named[i] = NamedInfo();
	}
	table.NamedCount = 0;
	table.ByHandle.Clear();
	table.ReportedHandles.Clear();
	table.Lumps = 0;
	table.Replaced = 0;
	table.Stats = FDefBlockStats();

	const FDefBlockHandler handler = [&table](const FDefBlock &block, FString &error, int &errorLine) -> bool
	{
		ParticleDefinitionGpu gpu;
		NamedInfo info;
		if (!BuildDefinition(block, gpu, info, error, errorLine)) return false;

		const int handle = ParticleDefinitionHandle(block.Name.GetChars());
		const int *existing = table.ByHandle.CheckKey(handle);
		if (existing != nullptr)
		{
			const NamedInfo &earlier = table.Named[*existing];
			if (earlier.Name.CompareNoCase(block.Name.GetChars()) != 0)
			{
				// Two different names, one 31-bit hash. Refused here, at load, so a
				// handle always means one name; it only ever costs this machine pixels.
				error.Format("its name has the same handle (%d) as '%s' at %s, line %d -- rename one of them",
					handle, earlier.Name.GetChars(), earlier.Lump.GetChars(), earlier.Line);
				return false;
			}
		}
		else if (table.NamedCount >= kNamedSlots)
		{
			error.Format("the table already holds %u named definitions, the most it can", kNamedSlots);
			return false;
		}

		// [2c] THE ATLAS BUDGET. Counts the layers the table would use with this
		// definition accepted (the one it replaces left out, identical runs shared).
		// Every acceptance keeps that at or under the cap, so the layer assignment
		// after the last lump always fits. Nothing is changed before this check.
		if (info.Frames.Size() > 0)
		{
			const int replacing = existing != nullptr ? *existing : -1;
			if (AtlasLayersInUse(table, replacing, &info.Frames) > ParticleDefinitionBuffer::ATLAS_LAYERS)
			{
				const unsigned inUse = AtlasLayersInUse(table, replacing, nullptr);
				error.Format("its flipbook \"%s\" needs %u particle atlas layers and only %u of %u are free",
					info.Texture.GetChars(), info.Frames.Size(), ParticleDefinitionBuffer::ATLAS_LAYERS - inUse, ParticleDefinitionBuffer::ATLAS_LAYERS);
				errorLine = info.TextureLine;
				return false;
			}
		}

		int index;
		if (existing != nullptr)
		{
			index = *existing;
			const NamedInfo &earlier = table.Named[index];
			Printf("ParticleDefinitions: '%s' at %s, line %d replaces the one at %s, line %d\n",
				block.Name.GetChars(), block.LumpName.GetChars(), block.Line,
				earlier.Lump.GetChars(), earlier.Line);
			table.Replaced++;
		}
		else
		{
			index = (int)table.NamedCount++;
			table.ByHandle.Insert(handle, index);
		}

		info.Handle = handle;
		table.Gpu[index] = gpu;
		table.Stamp((unsigned)index);
		table.Named[index] = info;
		return true;
	};

	int lastLump = 0;
	int lump;
	while ((lump = fileSystem.FindLumpFullName("PARTICLEDEFS", &lastLump, true)) != -1)
	{
		table.Lumps++;
		ReadDefinitionBlocks(lump, handler, table.Stats);
	}

	// [2c] Atlas layers for the definitions that survived every lump.
	const unsigned textured = AssignAtlasLayers(table);
	// [MESHPARTICLES] And the mesh list, the same way.
	const unsigned meshed = AssignMeshList(table);
	// [DEBRISPOOL] And the debris list.
	const unsigned debris = AssignDebrisList(table);
	// [PARTICLELIGHTS] And the light list.
	const unsigned lit = AssignLightList(table);

	Printf("ParticleDefinitions: %u named definition%s from %u PARTICLEDEFS lump%s -- %d refused, %u replaced by a later one\n",
		table.NamedCount, table.NamedCount == 1 ? "" : "s", table.Lumps, table.Lumps == 1 ? "" : "s",
		table.Stats.Refused, table.Replaced);
	Printf("ParticleDefinitions: %u textured definition%s using %u of %u particle atlas layers (the renderer builds the atlas on its next frame; Vulkan only)\n",
		textured, textured == 1 ? "" : "s", table.AtlasLayers.Size(), ParticleDefinitionBuffer::ATLAS_LAYERS);
	Printf("ParticleDefinitions: %u definition%s name%s a mesh (drawn as instanced 3D meshes while r_meshparticles is on; Vulkan only)\n",
		meshed, meshed == 1 ? "" : "s", meshed == 1 ? "s" : "");
	Printf("ParticleDefinitions: %u definition%s %s debris (restitution: simulated in the debris pool while r_debris is on; Vulkan only)\n",
		debris, debris == 1 ? "" : "s", debris == 1 ? "is" : "are");
	// [DEBRISSOUNDS] And how many of those make a landing sound.
	unsigned landing = 0;
	for (const ParticleDebrisDefinition &d : table.Debris)
	{
		if (!d.LandSound.IsEmpty()) landing++;
	}
	Printf("ParticleDefinitions: %u debris definition%s name%s a landing sound (landsound: one sound per group where it lands, while r_debris_sounds is on; Vulkan only)\n",
		landing, landing == 1 ? "" : "s", landing == 1 ? "s" : "");
	// [PARTICLELIGHTS]
	Printf("ParticleDefinitions: %u definition%s throw%s a light (light: an effect light per particle that carries one, while r_effectlights is on; Vulkan only)\n",
		lit, lit == 1 ? "" : "s", lit == 1 ? "s" : "");
}

//==========================================================================
//
// Handles. NETPLAY: the handle is a pure function of the name's text, so what
// script holds is identical on every machine whatever each one's lumps did; only
// the render-side spawn asks the table what a handle stands for.
//
//==========================================================================

int ParticleDefinitionHandle(const char *name)
{
	uint32_t hash = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)name; p != nullptr && *p != 0; p++)
	{
		// ASCII lower case only, so the result never depends on a locale. FName is
		// case-insensitive and keeps whichever spelling a machine saw first.
		unsigned char ch = *p;
		if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
		hash ^= ch;
		hash *= 16777619u;
	}
	const int handle = (int)(hash & 0x7fffffffu);
	return handle != 0 ? handle : 1;
}

int ResolveParticleDefinitionHandle(int handle, bool report)
{
	DefinitionTable &table = Table();
	if (const int *index = table.ByHandle.CheckKey(handle))
		return *index;

	if (report && handle != 0 && table.ReportedHandles.CheckKey(handle) == nullptr && table.ReportedHandles.CountUsed() < 64)
	{
		table.ReportedHandles.Insert(handle, true);
		Printf(TEXTCOLOR_ORANGE "ParticleDefinitions: SpawnParticles was given handle %d, which no PARTICLEDEFS definition on this machine has -- "
			"nothing drawn (see `particles` for the loaded names; this only ever changes this machine's pixels)\n", handle);
	}
	return -1;
}

//==========================================================================
//
// The inline cache
//
//==========================================================================

int InlineParticleDefinition(float sizeStart, float sizeEnd, float gravity, float drag, int orient, float stretch,
	uint64_t levelSerial, double birth, double longestLife)
{
	DefinitionTable &table = Table();
	const float tuple[6] = { sizeStart, sizeEnd, gravity, drag, (float)orient, stretch };

	// FNV-1a over the tuple's bits: equal looks are equal bits, since every caller
	// casts the same doubles to float the same way.
	uint32_t hash = 2166136261u;
	const uint8_t *bytes = (const uint8_t *)tuple;
	for (size_t i = 0; i < sizeof(tuple); i++)
	{
		hash ^= bytes[i];
		hash *= 16777619u;
	}

	const double expiry = birth + longestLife + kInlineReuseMargin;

	auto matches = [&](int i) -> bool
	{
		const InlineInfo &s = table.Inline[i];
		return s.Used && s.Hash == hash && memcmp(s.Tuple, tuple, sizeof(tuple)) == 0;
	};

	auto use = [&](int i) -> int
	{
		InlineInfo &s = table.Inline[i];
		if (s.LevelSerial != levelSerial || expiry > s.Expiry) s.Expiry = expiry;
		s.LevelSerial = levelSerial;
		s.Spawns++;
		table.LastInlineHit = i;
		return (int)(kNamedSlots + i);
	};

	if (table.LastInlineHit >= 0 && matches(table.LastInlineHit)) return use(table.LastInlineHit);
	for (int i = 0; i < (int)kInlineSlots; i++)
	{
		if (matches(i)) return use(i);
	}

	// A new look. Take the first slot whose particles are all gone -- never used,
	// from another level (the ring was reset), or past its expiry.
	int pick = -1;
	int soonest = -1;
	for (int i = 0; i < (int)kInlineSlots; i++)
	{
		const InlineInfo &s = table.Inline[i];
		if (!s.Used || s.LevelSerial != levelSerial || birth >= s.Expiry)
		{
			pick = i;
			break;
		}
		if (soonest < 0 || s.Expiry < table.Inline[soonest].Expiry) soonest = i;
	}
	if (pick < 0)
	{
		// Every slot holds live particles. A spawn never refuses, so the look that
		// dies soonest is replaced and its remaining particles change look.
		pick = soonest;
		if (table.WarnedFullSerial != levelSerial)
		{
			table.WarnedFullSerial = levelSerial;
			Printf(TEXTCOLOR_ORANGE "ParticleDefinitions: inline cache full -- %u different SpawnGpuParticles looks are alive at once; "
				"the one expiring soonest was replaced, so its live particles change look\n", kInlineSlots);
		}
	}

	// The stage 1 look as a definition: size start -> end over life, emissive 1
	// (the record's intensity scale carries the brightness), white (the record's
	// tint carries the colour), the stage 1 smoothstep fade, no cap of its own.
	ParticleDefinitionGpu &g = table.Gpu[kNamedSlots + pick];
	memset(&g, 0, sizeof(g));
	g.key[0][0] = 0.f; g.key[0][1] = sizeStart; g.key[0][2] = 0.f; g.key[0][3] = 1.f;
	g.key[1][0] = 1.f; g.key[1][1] = sizeEnd;   g.key[1][2] = 0.f; g.key[1][3] = 1.f;
	for (unsigned i = 2; i < kMaxKeys; i++) memcpy(g.key[i], g.key[1], sizeof(g.key[1]));
	for (unsigned i = 0; i < kMaxKeys; i++) g.keyColor[i / 4][i % 4] = (float)0xFFFFFF;
	g.motion[0] = gravity; g.motion[1] = drag;    g.motion[2] = 0.f;  g.motion[3] = 2.f;
	g.shape[0] = (float)orient; g.shape[1] = stretch; g.shape[2] = 0.f; g.shape[3] = 0.f;
	g.look[0] = 0.f;    g.look[1] = -1.f;  g.look[2] = 0.f;  g.look[3] = (float)PDF_FADE_SMOOTH;
	g.flipbook[0] = -1.f;
	table.Stamp(kNamedSlots + pick);

	InlineInfo &s = table.Inline[pick];
	s.Used = true;
	s.Hash = hash;
	memcpy(s.Tuple, tuple, sizeof(tuple));
	s.LevelSerial = levelSerial;
	s.Expiry = expiry;
	s.Spawns = 0;
	return use(pick);
}

//==========================================================================
//
// For the renderer
//
//==========================================================================

const ParticleDefinitionGpu *ParticleDefinitionTableData() { return Table().Gpu; }
const uint64_t *ParticleDefinitionSlotGenerations() { return Table().SlotGeneration; }
unsigned ParticleDefinitionSlotCount() { return kSlots; }
uint64_t ParticleDefinitionGeneration() { return Table().Generation; }

// [2c] For the renderer's atlas sync (ParticleDefinitionBuffer::SyncAtlasLayers).
const ParticleAtlasLayer *ParticleAtlasLayerData() { return Table().AtlasLayers.Size() > 0 ? &Table().AtlasLayers[0] : nullptr; }
unsigned ParticleAtlasLayerCount() { return Table().AtlasLayers.Size(); }
uint64_t ParticleAtlasGeneration() { return Table().AtlasGeneration; }

// [MESHPARTICLES] For the renderer's mesh particles (MeshParticleBuffer::SyncDefinitions).
const ParticleMeshDefinition *ParticleMeshDefinitionData() { return Table().Meshes.Size() > 0 ? &Table().Meshes[0] : nullptr; }
unsigned ParticleMeshDefinitionCount() { return Table().Meshes.Size(); }
uint64_t ParticleMeshGeneration() { return Table().MeshGeneration; }

// [DEBRISPOOL] For the renderer's debris pool (DebrisPool, hw_debrispool.cpp) and SpawnParticles' hook.
const ParticleDebrisDefinition *ParticleDebrisDefinitionData() { return Table().Debris.Size() > 0 ? &Table().Debris[0] : nullptr; }
unsigned ParticleDebrisDefinitionCount() { return Table().Debris.Size(); }
uint64_t ParticleDebrisGeneration() { return Table().DebrisGeneration; }

// [PARTICLELIGHTS] For the renderer's particle and debris lights (GpuParticleBuffer::SpawnRecordLights, DebrisPool::SyncLights).
const ParticleLightDefinition *ParticleLightDefinitionData() { return Table().Lights.Size() > 0 ? &Table().Lights[0] : nullptr; }
unsigned ParticleLightDefinitionCount() { return Table().Lights.Size(); }
uint64_t ParticleLightGeneration() { return Table().LightGeneration; }

bool ParticleDefinitionIsDebris(int slot)
{
	const DefinitionTable &table = Table();
	return slot >= 0 && (unsigned)slot < table.NamedCount && table.Named[slot].HasDebris;
}

// [DEBRISPOOL] The pool's state for `particles` (hw_debrispool.cpp): declared here, as SpawnGpuParticles declares
// GpuParticlesLegacyPath, so the definitions table does not include the renderer's pool.
extern FString DebrisPoolReport();

//==========================================================================
//
// `particles` -- the definitions and how full the inline cache is, to the
// console and log. Developer options has a Command row for it (no typing).
//
//==========================================================================

CCMD(particles)
{
	DefinitionTable &table = Table();

	Printf("Particle definitions: %u named of %u, from %u PARTICLEDEFS lump%s (%d refused, %u replaced); table generation %llu\n",
		table.NamedCount, kNamedSlots, table.Lumps, table.Lumps == 1 ? "" : "s",
		table.Stats.Refused, table.Replaced, (unsigned long long)table.Generation);

	if (screen != nullptr && screen->mParticleDefinitions != nullptr)
	{
		Printf("  GPU copy: synced to generation %llu, %llu slot uploads since it was created\n",
			(unsigned long long)screen->mParticleDefinitions->GetSyncedGeneration(),
			(unsigned long long)screen->mParticleDefinitions->GetUploadedSlots());
	}
	else
	{
		Printf("  GPU copy: none (GPU particles are drawn on Vulkan only)\n");
	}

	for (unsigned i = 0; i < table.NamedCount; i++)
	{
		const NamedInfo &n = table.Named[i];
		const ParticleDefinitionGpu &g = table.Gpu[i];
		const int orient = clamp((int)g.shape[0], 0, 2);
		const int collide = clamp((int)g.look[2], 0, 2);
		const int flags = (int)g.look[3];

		FString soft;
		if (g.look[1] < 0.f) soft = "unset";
		else soft.Format("%g", g.look[1]);

		FString texture;
		if (n.Texture.IsEmpty()) texture = "none";
		else if (n.FirstLayer >= 0)
			texture.Format("\"%s\" x%d at %g fps, %s -- atlas layers %d..%d", n.Texture.GetChars(), (int)g.flipbook[1], g.flipbook[2], g.flipbook[3] > 0.5f ? "once" : "loop",
				n.FirstLayer, n.FirstLayer + (int)n.Frames.Size() - 1);
		else
			texture.Format("\"%s\" x%d -- not in the atlas", n.Texture.GetChars(), (int)g.flipbook[1]);

		Printf("  #%u %s (handle %d) -- %s, line %d\n", i, n.Name.GetChars(), n.Handle, n.Lump.GetChars(), n.Line);
		Printf("      %u time keys, maxsize %g, %s, stretch %g, spin %g..%g, gravity %g, drag %g, fade %s, collide %s, lit %g, soft %s, texture %s\n",
			n.KeyCount, g.motion[2], kOrientNames[orient], g.shape[1], g.shape[2], g.shape[3], g.motion[0], g.motion[1],
			(flags & PDF_FADE_SMOOTH) ? "smooth" : "none", kCollideNames[collide], g.look[0], soft.GetChars(), texture.GetChars());

		// [LOOKS] The generated look, when it has one.
		const int look = clamp((int)g.spare[0][0], 0, (int)PDL_COUNT - 1);
		if (look != PDL_NONE)
		{
			FString lookText;
			lookText.Format("look %s -- roughness %g, churn %g cells/s, detail %d octaves", kLookNames[look], g.spare[0][1], g.spare[0][2], (int)g.spare[0][3]);
			if (look == PDL_FIRE || look == PDL_FLASH || look == PDL_SPARK)
				lookText.AppendFormat(", heat %g -> %g", g.spare[1][0], g.spare[1][1]);
			if (look == PDL_SMOKE || look == PDL_FIRE)
				lookText.AppendFormat(", rise %g u/s", g.spare[1][3]);
			if (look == PDL_FLASH)
				lookText.AppendFormat(", prongs %d..%d", (int)g.spare[1][2] / 32, (int)g.spare[1][2] % 32);
			Printf("      %s\n", lookText.GetChars());
		}

		// [MESHPARTICLES] The mesh, when it has one, and how this machine draws it.
		if (n.HasMesh)
		{
			const ParticleMeshDefinition &mesh = n.Mesh;
			FGameTexture *skinTexture = TexMan.GetGameTexture(mesh.Skin);
			const double centreX = 0.5 * ((double)mesh.BoundsMin[0] + mesh.BoundsMax[0]);
			const double centreY = 0.5 * ((double)mesh.BoundsMin[1] + mesh.BoundsMax[1]);
			const double centreZ = 0.5 * ((double)mesh.BoundsMin[2] + mesh.BoundsMax[2]);
			const double offCentre = std::sqrt(centreX * centreX + centreY * centreY + centreZ * centreZ);
			Printf("      mesh \"%s\" -- frame %d of %u, %u triangles, %u vertices, skin %s%s; diameter %g map units at size 1, bounds (%g %g %g) .. (%g %g %g)%s\n",
				mesh.Path.GetChars(), mesh.Frame, mesh.Frames, mesh.Triangles, mesh.Vertices,
				skinTexture != nullptr ? skinTexture->GetName().GetChars() : "?", n.MeshSkinName.IsEmpty() ? " (the md3's own)" : "",
				mesh.Diameter, mesh.BoundsMin[0], mesh.BoundsMin[1], mesh.BoundsMin[2], mesh.BoundsMax[0], mesh.BoundsMax[1], mesh.BoundsMax[2],
				offCentre > 0.1 * mesh.Diameter ? " -- it turns about its origin, which is off its centre" : "");
			if (screen != nullptr && screen->mMeshParticles != nullptr)
				Printf("      %s\n", screen->mMeshParticles->DescribeSlot((int)i).GetChars());
			else
				Printf("      drawn as its billboard (mesh particles are drawn on Vulkan only)\n");
		}

		// [DEBRISPOOL] The debris keys, when it has them.
		if (n.HasDebris)
		{
			const ParticleDebrisDefinition &debris = n.Debris;
			FString rest;
			if (debris.RestLife > 0.f)
				rest.Format("rests %g s%s, then fades over %g s", debris.RestLife, debris.RestLife >= 5.f ? " (times r_debris_life / 60)" : "", debris.RestFade);
			else
				rest = "never rests (it bounces until its life ends)";
			Printf("      debris -- restitution %g, friction %g, %s\n", debris.Restitution, debris.Friction, rest.GetChars());
			// [DEBRISSOUNDS] Its landing sound, when it has one.
			if (!debris.LandSound.IsEmpty())
			{
				FString big;
				if (!debris.LandSoundBig.IsEmpty())
					big.Format("; a group of more than %d: \"%s\"", debris.LandBigCount, debris.LandSoundBig.GetChars());
				Printf("      lands with \"%s\"%s -- volume %g, pitch %g .. %g\n", debris.LandSound.GetChars(), big.GetChars(),
					debris.LandVolume, debris.LandPitchMin, debris.LandPitchMax);
			}
		}

		// [PARTICLELIGHTS] The light it throws, when it throws one.
		if (n.HasLight)
		{
			const ParticleLightDefinition &light = n.Light;
			FString colour, brightness, hold;
			if (light.HasColor)
				colour.Format("%d %d %d", (int)std::lround(light.Color[0] * 255.0), (int)std::lround(light.Color[1] * 255.0), (int)std::lround(light.Color[2] * 255.0));
			else
				colour = "the colour ramp's first key";
			if (light.RampKeys == 0)
				brightness = "1, fading as fade = smooth";
			for (int k = 0; k < light.RampKeys; k++)
				brightness.AppendFormat("%s%g @%g", k > 0 ? ", " : "", light.RampValue[k], light.RampTime[k]);
			if (collide == 0)
				hold = "never lands";
			else if (light.Hold > 0.f)
				hold.Format("where it lands it holds a hashed share (a quarter to all) of %g s", light.Hold);
			else
				hold = "goes out where it lands";
			Printf("      light -- radius %g, intensity %g, colour %s, brightness %s; %g of each burst, at most %d; %s; %s; priority %d\n",
				light.Radius, light.Intensity, colour.GetChars(), brightness.GetChars(), light.Share, light.Max,
				orient == 1 && light.Line != 0 ? "a line along the streak" : "a point", hold.GetChars(), light.Priority);
		}
	}

	// [2c] The particle atlas: what the named flipbooks use, and what the renderer built.
	Printf("Particle atlas: %u of %u layers used by named flipbooks (list generation %llu)\n",
		table.AtlasLayers.Size(), ParticleDefinitionBuffer::ATLAS_LAYERS, (unsigned long long)table.AtlasGeneration);
	if (screen != nullptr && screen->mParticleDefinitions != nullptr)
	{
		const ParticleDefinitionBuffer *defs = screen->mParticleDefinitions;
		if (defs->GetAtlasBuiltLayers() > 0)
		{
			Printf("  GPU atlas: %u layers of %d x %d with mips, %.2f MiB (from list generation %llu)\n",
				defs->GetAtlasBuiltLayers(), defs->GetAtlasBuiltSize(), defs->GetAtlasBuiltSize(),
				defs->GetAtlasBuiltBytes() / (1024.0 * 1024.0), (unsigned long long)defs->GetAtlasGeneration());
		}
		else
		{
			Printf("  GPU atlas: only the 1 x 1 placeholder%s\n",
				table.AtlasLayers.Size() > 0 ? " -- the atlas is built on the next drawn frame" : " (no definition names a texture)");
		}
	}

	FLevelLocals *level = primaryLevel;
	const uint64_t serial = level != nullptr ? level->GpuParticleSerial : 0;
	const double now = level != nullptr ? level->maptime / (double)TICRATE : 0.0;

	unsigned used = 0, alive = 0;
	for (const InlineInfo &s : table.Inline)
	{
		if (!s.Used) continue;
		used++;
		if (s.LevelSerial == serial && now < s.Expiry) alive++;
	}
	Printf("Inline cache (SpawnGpuParticles looks): %u of %u slots used, %u holding live particles in this level\n", used, kInlineSlots, alive);

	for (unsigned i = 0; i < kInlineSlots; i++)
	{
		const InlineInfo &s = table.Inline[i];
		if (!s.Used) continue;
		const bool live = s.LevelSerial == serial && now < s.Expiry;
		Printf("  #%u size %g -> %g, gravity %g, drag %g, %s, stretch %g -- %llu spawn call%s, %s\n",
			kNamedSlots + i, s.Tuple[0], s.Tuple[1], s.Tuple[2], s.Tuple[3], kOrientNames[clamp((int)s.Tuple[4], 0, 2)], s.Tuple[5],
			(unsigned long long)s.Spawns, s.Spawns == 1 ? "" : "s", live ? "live" : "free to reuse");
	}

	FBaseCVar *legacy = FindCVar("r_gpuparticles_legacy", nullptr);
	const bool legacyOn = legacy != nullptr && legacy->GetGenericRep(CVAR_Int).Int != 0;
	Printf("r_gpuparticles_legacy %s: SpawnGpuParticles writes %s\n",
		legacy == nullptr ? "(not in this build)" : (legacyOn ? "1" : "0"),
		legacyOn ? "stage 1 records (the legacy path)" : "inline-definition records");

	// [LOOKS] The quality every `look` draws at (hw_particledefbuffer.cpp).
	static const char *const kLooksQuality[] = { "every look as the plain round dot", "2 octaves, no slope lighting", "3 octaves with slope lighting", "4 octaves with slope lighting" };
	FBaseCVar *looks = FindCVar("r_gpuparticles_looks", nullptr);
	if (looks == nullptr)
	{
		Printf("r_gpuparticles_looks (not in this build)\n");
	}
	else
	{
		const int looksValue = clamp(looks->GetGenericRep(CVAR_Int).Int, 0, 3);
		Printf("r_gpuparticles_looks %d: %s\n", looksValue, kLooksQuality[looksValue]);
	}

	// [MESHPARTICLES] The mesh switch (hw_meshparticles.cpp) and the mesh list.
	FBaseCVar *meshSwitch = FindCVar("r_meshparticles", nullptr);
	const bool meshesOn = meshSwitch != nullptr && meshSwitch->GetGenericRep(CVAR_Int).Int != 0;
	Printf("r_meshparticles %s: %u definition%s name%s a mesh -- %s\n",
		meshSwitch == nullptr ? "(not in this build)" : (meshesOn ? "1" : "0"),
		table.Meshes.Size(), table.Meshes.Size() == 1 ? "" : "s", table.Meshes.Size() == 1 ? "s" : "",
		meshesOn ? "drawn as instanced 3D meshes" : "drawn as their billboards");
	if (screen != nullptr && screen->mMeshParticles != nullptr)
		Printf("  mesh instances uploaded: %u of %u\n", screen->mMeshParticles->GetUploadedInstances(), screen->mMeshParticles->GetInstanceCapacity());

	// [DEBRISPOOL] The debris definitions and the pool (hw_debrispool.cpp).
	Printf("Debris definitions (restitution): %u\n", table.Debris.Size());
	// [PARTICLELIGHTS] The light definitions and the test (hw_gpuparticlebuffer.cpp).
	FBaseCVar *lightTest = FindCVar("r_particlelights_test", nullptr);
	Printf("Light definitions (light): %u; r_particlelights_test %s\n", table.Lights.Size(),
		lightTest == nullptr ? "(not in this build)" : (lightTest->GetGenericRep(CVAR_Int).Int != 0 ? "1: glowing definitions without keys light too" : "0"));
	Printf("%s\n", DebrisPoolReport().GetChars());
}
