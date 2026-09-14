/*
** damagedefs.cpp
**
** [SURFACEDAMAGE] The surface damage brushes and looks. See damagedefs.h for the lump.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include <algorithm>
#include <cmath>
#include <cstring>

#include "damagedefs.h"
#include "defblocks.h"
#include "hw_surfacedamageframe.h"
#include "bitmap.h"
#include "image.h"
#include "filesystem.h"
#include "printf.h"
#include "i_time.h"
#include "cmdlib.h"
#include "name.h"
#include "tarray.h"
#include "zstring.h"

namespace
{
	enum class EMaskChannels { Rgba, Alpha, Luminance };

	// One layer's pixels while the lumps load: a whole mip chain, largest first.
	struct LayerChain
	{
		TArray<uint8_t> Chain;
	};

	struct LoadState
	{
		SurfaceDamageDefinitionSet Set;
		TArray<LayerChain> BrushLayers;		// by atlas layer
		TArray<LayerChain> DetailLayers;	// by detail layer
		TMap<int, int> BrushByName;			// FName index -> brush
		TMap<int, int> LookByName;			// FName index -> look (NAME_None -> 0)
	};

	LoadState &State()
	{
		static LoadState state;
		return state;
	}

	// FNV-1a over the name's text, ASCII lower case, so the salt is the same on every machine whatever order names were made in.
	uint32_t NameSalt(const char *text)
	{
		uint32_t h = 2166136261u;
		for (const char *p = text; *p != 0; p++)
		{
			char c = *p;
			if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
			h = (h ^ (uint8_t)c) * 16777619u;
		}
		return h;
	}

	// A whole mip chain from level 0 (side x side RGBA8 at the chain's start): each level the average of 2 x 2 texels of the
	// one above, rounded.
	void BuildMips(uint8_t *chain, int side, int levels)
	{
		uint8_t *level = chain;
		int levelSide = side;
		for (int m = 1; m < levels && levelSide > 1; m++)
		{
			const int nextSide = levelSide >> 1;
			uint8_t *next = level + (size_t)levelSide * (size_t)levelSide * 4;
			for (int y = 0; y < nextSide; y++)
			{
				for (int x = 0; x < nextSide; x++)
				{
					const uint8_t *a = level + ((size_t)(y * 2) * (size_t)levelSide + (size_t)(x * 2)) * 4;
					const uint8_t *b = a + 4;
					const uint8_t *c = a + (size_t)levelSide * 4;
					const uint8_t *d = c + 4;
					uint8_t *out = next + ((size_t)y * (size_t)nextSide + (size_t)x) * 4;
					for (int ch = 0; ch < 4; ch++)
						out[ch] = (uint8_t)((a[ch] + b[ch] + c[ch] + d[ch] + 2) >> 2);
				}
			}
			level = next;
			levelSide = nextSide;
		}
	}

	// A separable tent filter from `sourceSide` pixels onto `side`: radius one texel of the coarser side, so it magnifies
	// smoothly and minifies without aliasing (vk_texture.cpp's particle atlas filter, clamped at the edges).
	void TentWeights(int side, int sourceSide, TArray<int> &index, TArray<float> &weight, TArray<int> &first)
	{
		index.Clear();
		weight.Clear();
		first.Resize((unsigned)side + 1);
		const double scale = (double)sourceSide / side;
		const double radius = std::max(1.0, scale);
		for (int d = 0; d < side; d++)
		{
			first[(unsigned)d] = (int)index.Size();
			const double centre = (d + 0.5) * scale;
			const int lo = (int)std::floor(centre - radius);
			const int hi = (int)std::ceil(centre + radius);
			double total = 0.0;
			const unsigned start = index.Size();
			for (int s = lo; s <= hi; s++)
			{
				const double w = 1.0 - std::fabs(s + 0.5 - centre) / radius;
				if (w <= 0.0) continue;
				index.Push(std::clamp(s, 0, sourceSide - 1));
				weight.Push((float)w);
				total += w;
			}
			for (unsigned t = start; t < index.Size(); t++)
				weight[t] = (float)(weight[t] / std::max(total, 1e-9));
		}
		first[(unsigned)side] = (int)index.Size();
	}

	// An image lump into a side x side chain in the engine's channels. False when the lump is missing or not an image.
	bool DecodeLayer(const FString &lumpName, EMaskChannels channels, int side, int levels, TArray<uint8_t> &chain, FString &error)
	{
		int lump = fileSystem.CheckNumForFullName(lumpName.GetChars());
		if (lump < 0) lump = fileSystem.CheckNumForName(lumpName.GetChars());
		if (lump < 0)
		{
			error.Format("no lump \"%s\"", lumpName.GetChars());
			return false;
		}
		FImageSource *image = FImageSource::GetImage(lump, false);
		if (image == nullptr)
		{
			error.Format("\"%s\" is not an image the engine reads", lumpName.GetChars());
			return false;
		}
		FBitmap bitmap = image->GetCachedBitmap(nullptr, 0);
		const int width = bitmap.GetWidth();
		const int height = bitmap.GetHeight();
		const int pitch = bitmap.GetPitch();
		const uint8_t *pixels = bitmap.GetPixels();
		if (pixels == nullptr || width <= 0 || height <= 0)
		{
			error.Format("\"%s\" has no pixels", lumpName.GetChars());
			return false;
		}

		// B G R A in, the engine's four masks out.
		TArray<float> source;
		source.Resize((unsigned)width * (unsigned)height * 4);
		for (int y = 0; y < height; y++)
		{
			const uint8_t *row = pixels + (size_t)y * (size_t)pitch;
			for (int x = 0; x < width; x++)
			{
				const float b = row[x * 4 + 0] / 255.f;
				const float g = row[x * 4 + 1] / 255.f;
				const float r = row[x * 4 + 2] / 255.f;
				const float a = row[x * 4 + 3] / 255.f;
				float *out = &source[((unsigned)y * (unsigned)width + (unsigned)x) * 4];
				switch (channels)
				{
				case EMaskChannels::Rgba:
					out[0] = r; out[1] = g; out[2] = b; out[3] = a;
					break;
				case EMaskChannels::Alpha:
					out[0] = out[1] = out[2] = out[3] = a;
					break;
				case EMaskChannels::Luminance:
					out[0] = out[1] = out[2] = out[3] = (0.299f * r + 0.587f * g + 0.114f * b) * a;
					break;
				}
			}
		}

		TArray<int> ix, iy, fx, fy;
		TArray<float> wx, wy;
		TentWeights(side, width, ix, wx, fx);
		TentWeights(side, height, iy, wy, fy);
		TArray<float> across;
		across.Resize((unsigned)side * (unsigned)height * 4);
		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < side; x++)
			{
				float sum[4] = { 0.f, 0.f, 0.f, 0.f };
				for (int t = fx[(unsigned)x]; t < fx[(unsigned)x + 1]; t++)
				{
					const float *in = &source[((unsigned)y * (unsigned)width + (unsigned)ix[(unsigned)t]) * 4];
					for (int c = 0; c < 4; c++) sum[c] += in[c] * wx[(unsigned)t];
				}
				memcpy(&across[((unsigned)y * (unsigned)side + (unsigned)x) * 4], sum, sizeof(sum));
			}
		}
		chain.Resize((unsigned)SurfaceDamageChainBytes(side, levels));
		memset(chain.Data(), 0, chain.Size());
		for (int y = 0; y < side; y++)
		{
			for (int x = 0; x < side; x++)
			{
				float sum[4] = { 0.f, 0.f, 0.f, 0.f };
				for (int t = fy[(unsigned)y]; t < fy[(unsigned)y + 1]; t++)
				{
					const float *in = &across[((unsigned)iy[(unsigned)t] * (unsigned)side + (unsigned)x) * 4];
					for (int c = 0; c < 4; c++) sum[c] += in[c] * wy[(unsigned)t];
				}
				uint8_t *out = chain.Data() + ((size_t)y * (size_t)side + (size_t)x) * 4;
				for (int c = 0; c < 4; c++)
					out[c] = (uint8_t)std::clamp((int)std::lround(sum[c] * 255.f), 0, 255);
			}
		}
		BuildMips(chain.Data(), side, levels);
		return true;
	}

	// THE BUILT-IN BRUSH `round`: a soft bowl. Depth is a smooth pit filling 70% of the brush; soot and wet fall off to its
	// edge; heat sits in the middle. A mod's own brushes replace it only by name.
	void MakeRoundBrush(TArray<uint8_t> &chain)
	{
		const int side = SURFACE_DAMAGE_BRUSH_TEXELS;
		chain.Resize((unsigned)SurfaceDamageChainBytes(side, SURFACE_DAMAGE_BRUSH_MIPS));
		memset(chain.Data(), 0, chain.Size());
		const auto smooth = [](double e0, double e1, double x)
		{
			const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
			return t * t * (3.0 - 2.0 * t);
		};
		for (int y = 0; y < side; y++)
		{
			for (int x = 0; x < side; x++)
			{
				const double dx = (x + 0.5) / side * 2.0 - 1.0;
				const double dy = (y + 0.5) / side * 2.0 - 1.0;
				const double d = std::sqrt(dx * dx + dy * dy);
				const double soot = smooth(1.0, 0.15, d);
				const double depth = smooth(0.72, 0.25, d);
				const double heat = smooth(0.8, 0.0, d);
				const double wet = smooth(1.0, 0.55, d);
				uint8_t *out = chain.Data() + ((size_t)y * (size_t)side + (size_t)x) * 4;
				out[0] = (uint8_t)std::lround(soot * 255.0);
				out[1] = (uint8_t)std::lround(depth * 255.0);
				out[2] = (uint8_t)std::lround(heat * 255.0);
				out[3] = (uint8_t)std::lround(wet * 255.0);
			}
		}
		BuildMips(chain.Data(), side, SURFACE_DAMAGE_BRUSH_MIPS);
	}

	// THE ENGINE'S GRAIN (detail layer 0), tiling: r fine grain, g crack lines, b streaks along u, a speckle. Integer hashing
	// of a lattice that wraps at the layer's side, so every value is the same on every machine and the layer tiles.
	uint32_t LatticeHash(int x, int y, int period, uint32_t seed)
	{
		x = ((x % period) + period) % period;
		y = ((y % period) + period) % period;
		uint32_t h = seed * 0x9E3779B1u ^ (uint32_t)x * 0x85EBCA77u;
		h = (h ^ (h >> 15)) * 0x2C1B3C6Du ^ (uint32_t)y * 0xC2B2AE3Du;
		h = (h ^ (h >> 13)) * 0x297A2D39u;
		return h ^ (h >> 16);
	}

	double ValueNoise(double u, double v, int periodX, int periodY, uint32_t seed)
	{
		const double fx = u * periodX, fy = v * periodY;
		const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
		const double tx = fx - x0, ty = fy - y0;
		const double sx = tx * tx * (3.0 - 2.0 * tx), sy = ty * ty * (3.0 - 2.0 * ty);
		const auto at = [&](int x, int y)
		{
			// Two lattices, one per axis period, so a stretched noise still wraps on both axes.
			const uint32_t h = LatticeHash(((x % periodX) + periodX) % periodX, ((y % periodY) + periodY) % periodY, std::max(periodX, periodY), seed);
			return (h >> 8) / 16777216.0;
		};
		const double a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
		return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
	}

	void MakeEngineGrain(TArray<uint8_t> &chain)
	{
		const int side = SURFACE_DAMAGE_DETAIL_TEXELS;
		chain.Resize((unsigned)SurfaceDamageChainBytes(side, SURFACE_DAMAGE_DETAIL_MIPS));
		memset(chain.Data(), 0, chain.Size());
		for (int y = 0; y < side; y++)
		{
			for (int x = 0; x < side; x++)
			{
				const double u = (x + 0.5) / side, v = (y + 0.5) / side;
				const double grain = 0.55 * ValueNoise(u, v, 16, 16, 11u) + 0.3 * ValueNoise(u, v, 64, 64, 12u) + 0.15 * ValueNoise(u, v, 128, 128, 13u);
				const double ridge = 1.0 - std::fabs(2.0 * (0.65 * ValueNoise(u, v, 8, 8, 21u) + 0.35 * ValueNoise(u, v, 32, 32, 22u)) - 1.0);
				const double cracks = std::pow(ridge, 6.0);
				const double streaks = 0.7 * ValueNoise(u, v, 4, 96, 31u) + 0.3 * ValueNoise(u, v, 16, 128, 32u);
				const double speck = (LatticeHash(x, y, side, 41u) >> 8) / 16777216.0 > 0.94 ? 1.0 : 0.0;
				uint8_t *out = chain.Data() + ((size_t)y * (size_t)side + (size_t)x) * 4;
				out[0] = (uint8_t)std::clamp((int)std::lround(grain * 255.0), 0, 255);
				out[1] = (uint8_t)std::clamp((int)std::lround(cracks * 255.0), 0, 255);
				out[2] = (uint8_t)std::clamp((int)std::lround(streaks * 255.0), 0, 255);
				out[3] = (uint8_t)std::lround(speck * 255.0);
			}
		}
		BuildMips(chain.Data(), side, SURFACE_DAMAGE_DETAIL_MIPS);
	}

	void SetLook(SurfaceDamageLook &look, const char *name, float rim[4], float inside[4], float detailScale, float detailStrength, float rimWidth,
		float wetShine, float bend, float depth)
	{
		look.Surface = FName(name);
		if (look.Surface.GetIndex() == FName("default").GetIndex())
			look.Surface = NAME_None;
		memcpy(look.Rim, rim, sizeof(look.Rim));
		memcpy(look.Inside, inside, sizeof(look.Inside));
		look.Detail[0] = 0.f;
		look.Detail[1] = detailScale;
		look.Detail[2] = detailStrength;
		look.Detail[3] = rimWidth;
		look.Finish[0] = wetShine;
		look.Finish[1] = 0.f;
		look.Finish[2] = bend;
		look.Finish[3] = depth;
	}

	// The built-in looks (plan: concrete a dusty grey rim, metal bright dent edges, wood splintered edges).
	void AddBuiltInLooks(LoadState &state)
	{
		struct BuiltIn { const char *Name; float Rim[4]; float Inside[4]; float DetailScale, DetailStrength, RimWidth, WetShine, Bend, Depth; };
		static const BuiltIn looks[] =
		{
			{ "default", { 0.62f, 0.60f, 0.57f, 0.55f }, { 0.20f, 0.19f, 0.18f, 0.85f }, 32.f, 0.6f, 5.f, 0.6f, 1.0f, 1.5f },
			{ "metal",   { 0.95f, 0.93f, 0.90f, 0.75f }, { 0.10f, 0.10f, 0.11f, 0.90f }, 24.f, 0.35f, 7.f, 1.2f, 1.2f, 1.0f },
			{ "wood",    { 0.78f, 0.62f, 0.42f, 0.65f }, { 0.22f, 0.15f, 0.09f, 0.85f }, 20.f, 0.8f, 5.f, 0.5f, 0.9f, 1.5f },
			{ "dirt",    { 0.42f, 0.34f, 0.25f, 0.35f }, { 0.35f, 0.29f, 0.22f, 0.75f }, 40.f, 0.7f, 3.f, 0.4f, 0.6f, 1.0f },
			{ "glass",   { 0.92f, 0.95f, 0.97f, 0.70f }, { 0.55f, 0.58f, 0.60f, 0.60f }, 16.f, 0.5f, 8.f, 1.5f, 1.4f, 0.0f },
			{ "liquid",  { 0.00f, 0.00f, 0.00f, 0.00f }, { 1.00f, 1.00f, 1.00f, 0.00f }, 32.f, 0.0f, 0.f, 2.0f, 0.3f, 0.0f },
		};
		for (const BuiltIn &b : looks)
		{
			SurfaceDamageLook look;
			float rim[4], inside[4];
			memcpy(rim, b.Rim, sizeof(rim));
			memcpy(inside, b.Inside, sizeof(inside));
			SetLook(look, b.Name, rim, inside, b.DetailScale, b.DetailStrength, b.RimWidth, b.WetShine, b.Bend, b.Depth);
			state.LookByName.Insert(look.Surface.GetIndex(), (int)state.Set.Looks.Size());
			state.Set.Looks.Push(look);
		}
	}

	bool NumberItem(const FDefBlockEntry &entry, int count, double lo, double hi, double *out, FString &error, int &errorLine)
	{
		// Either one item of `count` numbers (rim = 1 0.9 0.8) or `count` items of one number each (rim = 1, 0.9, 0.8).
		TArray<double> values;
		for (const FDefBlockItem &item : entry.Items)
		{
			if (item.HasAt)
			{
				error.Format("'%s' takes no '@'", entry.Key.GetChars());
				errorLine = item.Line;
				return false;
			}
			for (const FDefBlockAtom &atom : item.Atoms)
			{
				if (atom.Kind != FDefBlockAtom::Number)
				{
					error.Format("'%s' takes numbers, found '%s'", entry.Key.GetChars(), atom.Text.GetChars());
					errorLine = item.Line;
					return false;
				}
				values.Push(atom.Value);
			}
		}
		if ((int)values.Size() != count)
		{
			error.Format("'%s' takes %d number%s, found %u", entry.Key.GetChars(), count, count == 1 ? "" : "s", values.Size());
			errorLine = entry.Line;
			return false;
		}
		for (int i = 0; i < count; i++)
		{
			if (!(values[(unsigned)i] >= lo && values[(unsigned)i] <= hi))
			{
				error.Format("'%s' must be %g .. %g, found %g", entry.Key.GetChars(), lo, hi, values[(unsigned)i]);
				errorLine = entry.Line;
				return false;
			}
			out[i] = values[(unsigned)i];
		}
		return true;
	}

	bool OneWord(const FDefBlockEntry &entry, FString &word, FString &error, int &errorLine)
	{
		if (entry.Items.Size() != 1 || entry.Items[0].Atoms.Size() != 1 || entry.Items[0].HasAt || entry.Items[0].Atoms[0].Kind == FDefBlockAtom::Number)
		{
			error.Format("'%s' takes one word", entry.Key.GetChars());
			errorLine = entry.Line;
			return false;
		}
		word = entry.Items[0].Atoms[0].Text;
		return true;
	}

	bool ReadBrush(LoadState &state, const FDefBlock &block, FString &error, int &errorLine)
	{
		SurfaceDamageBrush brush;
		brush.Name = FName(block.Name.GetChars());
		brush.Salt = NameSalt(block.Name.GetChars());
		brush.Lump = block.LumpName;
		brush.Line = block.Line;
		if (block.Name.IsEmpty())
		{
			error = "a brush needs a name";
			return false;
		}

		TArray<FString> masks;
		int masksLine = block.Line;
		EMaskChannels channels = EMaskChannels::Rgba;
		for (const FDefBlockEntry &entry : block.Entries)
		{
			if (entry.Key.CompareNoCase("masks") == 0)
			{
				masksLine = entry.Line;
				for (const FDefBlockItem &item : entry.Items)
				{
					for (const FDefBlockAtom &atom : item.Atoms)
					{
						if (atom.Kind == FDefBlockAtom::Number || item.HasAt)
						{
							error = "'masks' takes image names (quote a path)";
							errorLine = item.Line;
							return false;
						}
						masks.Push(atom.Text);
					}
				}
			}
			else if (entry.Key.CompareNoCase("channels") == 0)
			{
				FString word;
				if (!OneWord(entry, word, error, errorLine)) return false;
				if (word.CompareNoCase("rgba") == 0) channels = EMaskChannels::Rgba;
				else if (word.CompareNoCase("alpha") == 0) channels = EMaskChannels::Alpha;
				else if (word.CompareNoCase("luminance") == 0) channels = EMaskChannels::Luminance;
				else
				{
					error.Format("'channels' is rgba, alpha or luminance, found '%s'", word.GetChars());
					errorLine = entry.Line;
					return false;
				}
			}
			else if (entry.Key.CompareNoCase("turn") == 0 || entry.Key.CompareNoCase("flip") == 0)
			{
				FString word;
				if (!OneWord(entry, word, error, errorLine)) return false;
				bool hashed;
				if (word.CompareNoCase("hashed") == 0) hashed = true;
				else if (word.CompareNoCase("none") == 0) hashed = false;
				else
				{
					error.Format("'%s' is none or hashed, found '%s'", entry.Key.GetChars(), word.GetChars());
					errorLine = entry.Line;
					return false;
				}
				if (entry.Key.CompareNoCase("turn") == 0) brush.TurnHashed = hashed;
				else brush.FlipHashed = hashed;
			}
			else
			{
				error.Format("unknown key '%s' (a brush has masks, channels, turn, flip)", entry.Key.GetChars());
				errorLine = entry.Line;
				return false;
			}
		}
		if (masks.Size() == 0)
		{
			error = "a brush needs 'masks'";
			return false;
		}
		if (masks.Size() > (unsigned)SURFACE_DAMAGE_BRUSH_VARIANTS)
		{
			error.Format("a brush has at most %d masks, found %u", SURFACE_DAMAGE_BRUSH_VARIANTS, masks.Size());
			errorLine = masksLine;
			return false;
		}

		// Decode every mask before anything is kept, so a refused brush adds nothing.
		TArray<LayerChain> chains;
		chains.Resize(masks.Size());
		for (unsigned i = 0; i < masks.Size(); i++)
		{
			if (!DecodeLayer(masks[i], channels, SURFACE_DAMAGE_BRUSH_TEXELS, SURFACE_DAMAGE_BRUSH_MIPS, chains[i].Chain, error))
			{
				errorLine = masksLine;
				return false;
			}
		}

		const int *existing = state.BrushByName.CheckKey(brush.Name.GetIndex());
		// A replaced brush's layers stay in the atlas unused; the budget counts them.
		if ((int)state.BrushLayers.Size() + (int)chains.Size() > SURFACE_DAMAGE_BRUSH_LAYERS)
		{
			error.Format("the brush atlas already holds %u of its %d layers", state.BrushLayers.Size(), SURFACE_DAMAGE_BRUSH_LAYERS);
			errorLine = masksLine;
			return false;
		}
		brush.FirstLayer = (int)state.BrushLayers.Size();
		brush.Variants = (int)chains.Size();
		for (unsigned i = 0; i < chains.Size(); i++)
			state.BrushLayers.Push(std::move(chains[i]));

		if (existing != nullptr)
		{
			const SurfaceDamageBrush &earlier = state.Set.Brushes[(unsigned)*existing];
			Printf("SurfaceDamage: brush '%s' at %s, line %d replaces the one at %s, line %d\n", block.Name.GetChars(), block.LumpName.GetChars(), block.Line,
				earlier.Lump.IsEmpty() ? "(built in)" : earlier.Lump.GetChars(), earlier.Line);
			state.Set.Brushes[(unsigned)*existing] = brush;
		}
		else
		{
			state.BrushByName.Insert(brush.Name.GetIndex(), (int)state.Set.Brushes.Size());
			state.Set.Brushes.Push(brush);
		}
		return true;
	}

	bool ReadLook(LoadState &state, const FDefBlock &block, FString &error, int &errorLine)
	{
		if (block.Name.IsEmpty())
		{
			error = "a look needs a surface name (or default)";
			return false;
		}
		FName surface(block.Name.GetChars());
		if (surface.GetIndex() == FName("default").GetIndex())
			surface = NAME_None;

		// Keys left out keep the built-in look of this name, or `default`'s.
		const int *existing = state.LookByName.CheckKey(surface.GetIndex());
		SurfaceDamageLook look = state.Set.Looks[existing != nullptr ? (unsigned)*existing : 0u];
		look.Surface = surface;
		look.Lump = block.LumpName;
		look.Line = block.Line;

		for (const FDefBlockEntry &entry : block.Entries)
		{
			double v[3];
			const FString &key = entry.Key;
			if (key.CompareNoCase("rim") == 0 || key.CompareNoCase("inside") == 0)
			{
				if (!NumberItem(entry, 3, 0.0, 1.0, v, error, errorLine)) return false;
				float *target = key.CompareNoCase("rim") == 0 ? look.Rim : look.Inside;
				for (int i = 0; i < 3; i++) target[i] = (float)v[i];
			}
			else if (key.CompareNoCase("rimstrength") == 0 || key.CompareNoCase("insidestrength") == 0 || key.CompareNoCase("detailstrength") == 0)
			{
				if (!NumberItem(entry, 1, 0.0, 1.0, v, error, errorLine)) return false;
				if (key.CompareNoCase("rimstrength") == 0) look.Rim[3] = (float)v[0];
				else if (key.CompareNoCase("insidestrength") == 0) look.Inside[3] = (float)v[0];
				else look.Detail[2] = (float)v[0];
			}
			else if (key.CompareNoCase("rimwidth") == 0)
			{
				if (!NumberItem(entry, 1, 0.0, 32.0, v, error, errorLine)) return false;
				look.Detail[3] = (float)v[0];
			}
			else if (key.CompareNoCase("detailscale") == 0)
			{
				if (!NumberItem(entry, 1, 1.0, 1024.0, v, error, errorLine)) return false;
				look.Detail[1] = (float)v[0];
			}
			else if (key.CompareNoCase("wetshine") == 0 || key.CompareNoCase("bend") == 0)
			{
				if (!NumberItem(entry, 1, 0.0, 4.0, v, error, errorLine)) return false;
				if (key.CompareNoCase("wetshine") == 0) look.Finish[0] = (float)v[0];
				else look.Finish[2] = (float)v[0];
			}
			else if (key.CompareNoCase("depth") == 0)
			{
				if (!NumberItem(entry, 1, 0.0, 8.0, v, error, errorLine)) return false;
				look.Finish[3] = (float)v[0];
			}
			else if (key.CompareNoCase("detail") == 0)
			{
				if (entry.Items.Size() != 1 || entry.Items[0].Atoms.Size() != 1 || entry.Items[0].Atoms[0].Kind == FDefBlockAtom::Number || entry.Items[0].HasAt)
				{
					error = "'detail' takes one image name (quote a path)";
					errorLine = entry.Line;
					return false;
				}
				look.DetailTexture = entry.Items[0].Atoms[0].Text;
			}
			else
			{
				error.Format("unknown key '%s' (a look has rim, rimstrength, rimwidth, inside, insidestrength, detail, detailscale, detailstrength, wetshine, bend, depth)", key.GetChars());
				errorLine = entry.Line;
				return false;
			}
		}

		if (look.DetailTexture.IsNotEmpty())
		{
			LayerChain chain;
			if (!DecodeLayer(look.DetailTexture, EMaskChannels::Rgba, SURFACE_DAMAGE_DETAIL_TEXELS, SURFACE_DAMAGE_DETAIL_MIPS, chain.Chain, error))
				return false;
			look.Detail[0] = (float)state.DetailLayers.Size();
			state.DetailLayers.Push(std::move(chain));
		}
		else
		{
			look.Detail[0] = 0.f;
		}

		if (existing != nullptr)
		{
			state.Set.Looks[(unsigned)*existing] = look;
		}
		else
		{
			if (state.Set.Looks.Size() >= (unsigned)SURFACE_DAMAGE_LOOKS)
			{
				error.Format("there are already %d looks, the most the renderer holds", SURFACE_DAMAGE_LOOKS);
				return false;
			}
			state.LookByName.Insert(surface.GetIndex(), (int)state.Set.Looks.Size());
			state.Set.Looks.Push(look);
		}
		return true;
	}

	// Layer chains into the upload order: level 0 of every layer, then level 1 of every layer, ...
	void Assemble(const TArray<LayerChain> &layers, int side, int levels, TArray<uint8_t> &out)
	{
		out.Clear();
		const size_t chainBytes = SurfaceDamageChainBytes(side, levels);
		out.Resize((unsigned)(chainBytes * layers.Size()));
		size_t cursor = 0;
		size_t levelOffset = 0;
		for (int m = 0; m < levels; m++)
		{
			const size_t levelBytes = (size_t)(side >> m) * (size_t)(side >> m) * 4;
			for (unsigned i = 0; i < layers.Size(); i++)
			{
				memcpy(out.Data() + cursor, layers[i].Chain.Data() + levelOffset, levelBytes);
				cursor += levelBytes;
			}
			levelOffset += levelBytes;
		}
	}
}

void LoadSurfaceDamageDefinitions()
{
	const uint64_t startNs = I_nsTime();
	LoadState &state = State();
	const uint64_t generation = state.Set.Generation + 1;
	state.Set = SurfaceDamageDefinitionSet();
	state.Set.Generation = generation;
	state.BrushLayers.Clear();
	state.DetailLayers.Clear();
	state.BrushByName.Clear();
	state.LookByName.Clear();

	// Built in: the brush `round` and the six looks.
	{
		LayerChain round;
		MakeRoundBrush(round.Chain);
		state.BrushLayers.Push(std::move(round));
		SurfaceDamageBrush brush;
		brush.Name = FName("round");
		brush.Salt = NameSalt("round");
		brush.FirstLayer = 0;
		brush.Variants = 1;
		state.BrushByName.Insert(brush.Name.GetIndex(), 0);
		state.Set.Brushes.Push(brush);

		LayerChain grain;
		MakeEngineGrain(grain.Chain);
		state.DetailLayers.Push(std::move(grain));
		AddBuiltInLooks(state);
	}

	FDefBlockStats stats;
	const FDefBlockHandler handler = [&state](const FDefBlock &block, FString &error, int &errorLine) -> bool
	{
		if (block.Kind.CompareNoCase("brush") == 0)
			return ReadBrush(state, block, error, errorLine);
		if (block.Kind.CompareNoCase("look") == 0)
			return ReadLook(state, block, error, errorLine);
		error.Format("unknown kind '%s' (DAMAGEDEFS has brush and look)", block.Kind.GetChars());
		return false;
	};

	int lastLump = 0;
	int lump;
	while ((lump = fileSystem.FindLumpFullName("DAMAGEDEFS", &lastLump, true)) != -1)
	{
		state.Set.Lumps++;
		ReadDefinitionBlocks(lump, handler, stats);
	}
	state.Set.Blocks = stats.Blocks;
	state.Set.Refused = stats.Refused;

	Assemble(state.BrushLayers, SURFACE_DAMAGE_BRUSH_TEXELS, SURFACE_DAMAGE_BRUSH_MIPS, state.Set.BrushPixels);
	state.Set.BrushLayers = (int)state.BrushLayers.Size();
	Assemble(state.DetailLayers, SURFACE_DAMAGE_DETAIL_TEXELS, SURFACE_DAMAGE_DETAIL_MIPS, state.Set.DetailPixels);
	state.Set.DetailLayers = (int)state.DetailLayers.Size();
	state.Set.LoadMs = (double)(I_nsTime() - startNs) / 1e6;

	int variants = 0;
	for (const SurfaceDamageBrush &b : state.Set.Brushes) variants += b.Variants;
	Printf("SurfaceDamage: %u brush%s (%d variant%s) and %u look%s from %u DAMAGEDEFS lump%s -- %d refused (%.1f ms)\n",
		state.Set.Brushes.Size(), state.Set.Brushes.Size() == 1 ? "" : "es", variants, variants == 1 ? "" : "s",
		state.Set.Looks.Size(), state.Set.Looks.Size() == 1 ? "" : "s", state.Set.Lumps, state.Set.Lumps == 1 ? "" : "s",
		state.Set.Refused, state.Set.LoadMs);
}

const SurfaceDamageDefinitionSet& SurfaceDamageDefinitions()
{
	return State().Set;
}

int SurfaceDamageFindBrush(FName name)
{
	const int *index = State().BrushByName.CheckKey(name.GetIndex());
	return index != nullptr ? *index : -1;
}

int SurfaceDamageLookFor(FName surface)
{
	const int *index = State().LookByName.CheckKey(surface.GetIndex());
	return index != nullptr ? *index : 0;
}
