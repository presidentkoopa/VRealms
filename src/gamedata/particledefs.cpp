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
**       collide  = plane                         // none | plane: honour SpawnParticles' surface and floor
**       fade     = none                          // none | smooth: the stage 1 fade over the last 40% of life
**   }
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

static_assert(sizeof(ParticleDefinitionGpu) == ParticleDefinitionBuffer::RECORD_BYTES,
	"ParticleDefinitionGpu must be sixteen vec4s -- see ParticleDefinitionData in vk_shader.cpp");
static_assert(offsetof(ParticleDefinitionGpu, keyColor) == 128 && offsetof(ParticleDefinitionGpu, motion) == 160 &&
	offsetof(ParticleDefinitionGpu, shape) == 176 && offsetof(ParticleDefinitionGpu, look) == 192 &&
	offsetof(ParticleDefinitionGpu, flipbook) == 208 && offsetof(ParticleDefinitionGpu, spare) == 224,
	"ParticleDefinitionGpu offsets must match the std430 layout of ParticleDefinitionData in vk_shader.cpp");

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
	const char *const kCollideNames[] = { "none", "plane" };
	const char *const kFadeNames[] = { "none", "smooth" };
	const char *const kModeNames[] = { "loop", "once" };

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

		for (unsigned i = 0; i < b.Entries.Size(); i++)
		{
			const FDefBlockEntry &e = b.Entries[i];
			for (unsigned j = 0; j < i; j++)
			{
				if (b.Entries[j].Key.CompareNoCase(e.Key) == 0)
					return Fail(error, errorLine, e.Line, "'%s' is given twice (first at line %d)", e.Key.GetChars(), b.Entries[j].Line);
			}

			bool ok = true;
			if (e.Key.CompareNoCase("size") == 0) ok = ReadRamp(e, 1, 0.0, kNoUpperLimit, size, error, errorLine);
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
			else if (e.Key.CompareNoCase("collide") == 0) ok = ReadChoice(e, kCollideNames, 2, collide, error, errorLine);
			else if (e.Key.CompareNoCase("fade") == 0) ok = ReadChoice(e, kFadeNames, 2, fade, error, errorLine);
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

		info.Name = b.Name;
		info.Lump = b.LumpName;
		info.Line = b.Line;
		info.KeyCount = count;
		info.Texture = texture;
		info.TextureLine = textureLine;
		info.Frames = frameIds;
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

	Printf("ParticleDefinitions: %u named definition%s from %u PARTICLEDEFS lump%s -- %d refused, %u replaced by a later one\n",
		table.NamedCount, table.NamedCount == 1 ? "" : "s", table.Lumps, table.Lumps == 1 ? "" : "s",
		table.Stats.Refused, table.Replaced);
	Printf("ParticleDefinitions: %u textured definition%s using %u of %u particle atlas layers (the renderer builds the atlas on its next frame; Vulkan only)\n",
		textured, textured == 1 ? "" : "s", table.AtlasLayers.Size(), ParticleDefinitionBuffer::ATLAS_LAYERS);
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
		const int collide = clamp((int)g.look[2], 0, 1);
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
}
