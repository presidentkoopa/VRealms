/*
** volumedefs.cpp
**
** [EMISSIVEVOLUMES] Emissive volume definitions: the VOLUMEDEFS reader, the table and the `emissivevolumes` command. See
** volumedefs.h and "Engine docs/VOLUMETRIC_FLASH_15_PLAN.md" 2j.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** THE LUMP (any number of VOLUMEDEFS lumps, any number of blocks; every key is optional; the shared reader, defblocks.h)
**
**   emissive rsb_flash_star
**   {
**       class       = gun                  // gun | biggun | blast: which "Volumetric flashes" setting draws it
**       shape       = 0, 6, 6              // body: length along the axis, base radius, tip radius[, weight 0..4]
**       offset      = 0                    // the base point along the axis (map units)
**       petals      = 5, 14, 4, 30         // count 0..16, length, width, spread from the axis (degrees)[, weight]
**       petaljitter = 0.35                 // 0..1: each petal's length (and half as much its width) varies by this
**       ring        = 0, 0, 0              // a ring around the base: radius, thickness, weight
**       softness    = 0.5                  // 0..1: the edge falloff, a share of each part's thickness
**       noise       = 0.08, 0.5, 2         // cells per map unit, amount 0..1, octaves 1..2
**       billow      = 40                   // map units a second the noise flows outward
**       churn       = 1                    // noise cells a second it changes in place
**       grow        = 0.4, 0.3             // start scale, share of life it takes to reach full size
**       expand      = 0                    // map units a second the bound grows after that
**       life        = 0.06                 // seconds (0.005..10); "Flash length" may shorten or stretch it (not a blast's)
**       heat        = 1.0, 0.3             // core heat at birth, at death, 0..1 on the engine's heat ramp
**       heatfalloff = 0.5                  // 0..4: temperature = core heat x density ^ heatfalloff
**       tint        = 255 255 255          // multiplies the heat ramp: plasma blue, BFG green
**       intensity   = 4 @0, 0 @1           // brightness over life, up to 8 keys, 0..64 (above 1 blooms)
**       absorption  = 0                    // 0..4: how much the gas hides what is behind it (0 = a pure glow)
**       flicker     = 0.25, 30             // amount 0..1, rate (Hz)
**       drag        = 4                    // per second, on the spawn's velocity
**       motion      = trail                // trail | stay | ride: how it moves when the followed hand moves
**       followtip   = 0.3                  // 0..1: trail: the share of the hand's motion the tip keeps
**       light       = 0, 0                 // its effect light: radius (0 = none, else 1..1024), intensity 0..16
**       lightline   = 0                    // 0 a point at the emission centre, 1 a line along the axis
**       lightflags  = surfaces, smoke, particles   // what the light lights (any of the three)
**       signature   = 0                    // 0 every shot varies; 1 one fixed shape every shot
**       wobble      = 0.12                 // 0..0.5: how much each shot's size varies
**   }
**
** Defaults make a plain soft ball of radius 8 that never lights. A block of another kind, an unknown key, a key given twice
** or a value out of range refuses the block with one console line; the rest of the lump loads.
**
*/

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include "volumedefs.h"
#include "defblocks.h"
#include "filesystem.h"
#include "printf.h"
#include "c_dispatch.h"
#include "v_text.h"
#include "tarray.h"
#include "zstring.h"

using namespace EmissiveVolumeCore;

namespace
{
	const unsigned kMaxDefinitions = 1024;

	struct NamedDefinition
	{
		FString Name;
		FString Lump;
		int Line = 0;
		int Handle = 0;
		Definition Def;
	};

	struct DefinitionTable
	{
		TArray<NamedDefinition> Named;
		TMap<int, int> ByHandle;			// handle -> index in Named
		TMap<int, bool> ReportedHandles;	// unknown handles already reported
		int Lumps = 0;
		int Replaced = 0;
		FDefBlockStats Stats;
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

	// Items that are one number each (no '@'): between minItems and maxItems of them, item k in lo[k] .. hi[k], whole where
	// whole[k]. Items not written keep what `out` held.
	bool ReadNumbers(const FDefBlockEntry &e, unsigned minItems, unsigned maxItems, const double lo[], const double hi[], const bool whole[],
		const char *usage, double out[], FString &error, int &errorLine)
	{
		const unsigned n = e.Items.Size();
		if (n < minItems || n > maxItems)
			return Fail(error, errorLine, e.Line, "'%s' is %s", e.Key.GetChars(), usage);
		for (unsigned k = 0; k < n; k++)
		{
			const FDefBlockItem &item = e.Items[k];
			if (item.Atoms.Size() != 1 || item.HasAt || item.Atoms[0].Kind != FDefBlockAtom::Number)
				return Fail(error, errorLine, item.Line, "'%s' is %s", e.Key.GetChars(), usage);
			const double v = item.Atoms[0].Value;
			if (!(v >= lo[k] && v <= hi[k]))
				return Fail(error, errorLine, item.Line, "'%s' value %g is outside %g .. %g", e.Key.GetChars(), v, lo[k], hi[k]);
			if (whole != nullptr && whole[k] && v != std::floor(v))
				return Fail(error, errorLine, item.Line, "'%s' value %g must be a whole number", e.Key.GetChars(), v);
			out[k] = v;
		}
		return true;
	}

	bool ReadNumber(const FDefBlockEntry &e, double lo, double hi, double &out, FString &error, int &errorLine)
	{
		double v = out;
		if (!ReadNumbers(e, 1, 1, &lo, &hi, nullptr, "one number", &v, error, errorLine))
			return false;
		out = v;
		return true;
	}

	bool ReadWhole(const FDefBlockEntry &e, int lo, int hi, int &out, FString &error, int &errorLine)
	{
		const double dlo = lo, dhi = hi;
		const bool whole = true;
		double v = out;
		if (!ReadNumbers(e, 1, 1, &dlo, &dhi, &whole, "one whole number", &v, error, errorLine))
			return false;
		out = (int)v;
		return true;
	}

	// One word from `choices`; its index in `out`.
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

	// `tint = 255 255 255`: one item of three numbers 0..255 (or three items of one number each).
	bool ReadTint(const FDefBlockEntry &e, float out[3], FString &error, int &errorLine)
	{
		double v[3] = { 0.0, 0.0, 0.0 };
		int got = 0;
		for (unsigned k = 0; k < e.Items.Size(); k++)
		{
			const FDefBlockItem &item = e.Items[k];
			if (item.HasAt)
				return Fail(error, errorLine, item.Line, "'%s' takes no '@'", e.Key.GetChars());
			for (unsigned a = 0; a < item.Atoms.Size(); a++)
			{
				if (item.Atoms[a].Kind != FDefBlockAtom::Number || got >= 3)
					return Fail(error, errorLine, item.Line, "'%s' is three numbers, red green blue 0..255", e.Key.GetChars());
				const double x = item.Atoms[a].Value;
				if (!(x >= 0.0 && x <= 255.0))
					return Fail(error, errorLine, item.Line, "'%s' value %g is outside 0 .. 255", e.Key.GetChars(), x);
				v[got++] = x;
			}
		}
		if (got != 3)
			return Fail(error, errorLine, e.Line, "'%s' is three numbers, red green blue 0..255", e.Key.GetChars());
		for (int c = 0; c < 3; c++)
			out[c] = (float)(std::round(v[c]) / 255.0);
		return true;
	}

	// `intensity = 4 @0, 0 @1`: a defblocks ramp of one number a key, 0..64. One value is constant; more need '@ <share of
	// life>' in 0..1, increasing.
	bool ReadIntensity(const FDefBlockEntry &e, Definition &d, FString &error, int &errorLine)
	{
		const unsigned n = e.Items.Size();
		if (n == 0)
			return Fail(error, errorLine, e.Line, "'%s' needs a value", e.Key.GetChars());
		if (n > (unsigned)INTENSITY_KEYS)
			return Fail(error, errorLine, e.Line, "'%s' has %u keys; a ramp holds at most %d", e.Key.GetChars(), n, INTENSITY_KEYS);
		for (unsigned i = 0; i < n; i++)
		{
			const FDefBlockItem &item = e.Items[i];
			if (item.Atoms.Size() != 1 || item.Atoms[0].Kind != FDefBlockAtom::Number)
				return Fail(error, errorLine, item.Line, "each '%s' value is one number", e.Key.GetChars());
			const double v = item.Atoms[0].Value;
			if (!(v >= 0.0 && v <= 64.0))
				return Fail(error, errorLine, item.Line, "'%s' value %g is outside 0 .. 64", e.Key.GetChars(), v);
			double t = 0.0;
			if (n > 1)
			{
				if (!item.HasAt)
					return Fail(error, errorLine, item.Line, "'%s' has %u values, so each needs '@ <life fraction>'", e.Key.GetChars(), n);
				if (!(item.At >= 0.0 && item.At <= 1.0))
					return Fail(error, errorLine, item.Line, "'%s' time @%g is outside 0 .. 1", e.Key.GetChars(), item.At);
				if (i > 0 && !(item.At > (double)d.IntensityTime[i - 1]))
					return Fail(error, errorLine, item.Line, "'%s' times must increase: @%g follows @%g", e.Key.GetChars(), item.At, (double)d.IntensityTime[i - 1]);
				t = item.At;
			}
			d.IntensityTime[i] = (float)t;
			d.IntensityValue[i] = (float)v;
		}
		for (int i = (int)n; i < INTENSITY_KEYS; i++)
		{
			d.IntensityTime[i] = 0.f;
			d.IntensityValue[i] = 0.f;
		}
		d.IntensityKeys = (int)n;
		return true;
	}

	// `lightflags = surfaces, smoke, particles`: any of the three words, each once.
	bool ReadLightFlags(const FDefBlockEntry &e, int &out, FString &error, int &errorLine)
	{
		static const char *const words[] = { "surfaces", "smoke", "particles" };
		int flags = 0;
		if (e.Items.Size() == 0)
			return Fail(error, errorLine, e.Line, "'%s' takes any of: surfaces, smoke, particles", e.Key.GetChars());
		for (unsigned k = 0; k < e.Items.Size(); k++)
		{
			const FDefBlockItem &item = e.Items[k];
			if (item.Atoms.Size() != 1 || item.HasAt || item.Atoms[0].Kind != FDefBlockAtom::Word)
				return Fail(error, errorLine, item.Line, "'%s' takes any of: surfaces, smoke, particles", e.Key.GetChars());
			int bit = -1;
			for (int w = 0; w < 3; w++)
			{
				if (item.Atoms[0].Text.CompareNoCase(words[w]) == 0)
					bit = 1 << w;
			}
			if (bit < 0)
				return Fail(error, errorLine, item.Line, "'%s' = %s -- expected any of: surfaces, smoke, particles", e.Key.GetChars(), item.Atoms[0].Text.GetChars());
			if (flags & bit)
				return Fail(error, errorLine, item.Line, "'%s' names %s twice", e.Key.GetChars(), item.Atoms[0].Text.GetChars());
			flags |= bit;
		}
		out = flags;
		return true;
	}

	bool BuildDefinition(const FDefBlock &block, Definition &d, FString &error, int &errorLine)
	{
		d = Definition();
		d.NameHash = NameHash(block.Name.GetChars());

		for (unsigned i = 0; i < block.Entries.Size(); i++)
		{
			const FDefBlockEntry &e = block.Entries[i];
			for (unsigned j = 0; j < i; j++)
			{
				if (block.Entries[j].Key.CompareNoCase(e.Key.GetChars()) == 0)
					return Fail(error, errorLine, e.Line, "'%s' is given twice (first at line %d)", e.Key.GetChars(), block.Entries[j].Line);
			}
			const char *key = e.Key.GetChars();
			bool ok = true;
			if (e.Key.CompareNoCase("class") == 0)
			{
				static const char *const choices[] = { "gun", "biggun", "blast" };
				ok = ReadChoice(e, choices, 3, d.Class, error, errorLine);
			}
			else if (e.Key.CompareNoCase("shape") == 0)
			{
				const double lo[] = { 0, 0, 0, 0 }, hi[] = { 8192, 4096, 4096, 4 };
				double v[4] = { d.Length, d.BaseRadius, d.TipRadius, d.BodyWeight };
				ok = ReadNumbers(e, 3, 4, lo, hi, nullptr, "length, base radius, tip radius[, weight]", v, error, errorLine);
				d.Length = v[0]; d.BaseRadius = v[1]; d.TipRadius = v[2]; d.BodyWeight = v[3];
			}
			else if (e.Key.CompareNoCase("offset") == 0) ok = ReadNumber(e, -4096, 4096, d.Offset, error, errorLine);
			else if (e.Key.CompareNoCase("petals") == 0)
			{
				const double lo[] = { 0, 0, 0, 0, 0 }, hi[] = { PETALS_MAX, 8192, 4096, 180, 4 };
				const bool whole[] = { true, false, false, false, false };
				double v[5] = { (double)d.PetalCount, d.PetalLength, d.PetalWidth, d.PetalSpread, d.PetalWeight };
				ok = ReadNumbers(e, 4, 5, lo, hi, whole, "count, length, width, spread (degrees)[, weight]", v, error, errorLine);
				d.PetalCount = (int)v[0]; d.PetalLength = v[1]; d.PetalWidth = v[2]; d.PetalSpread = v[3]; d.PetalWeight = v[4];
			}
			else if (e.Key.CompareNoCase("petaljitter") == 0) ok = ReadNumber(e, 0, 1, d.PetalJitter, error, errorLine);
			else if (e.Key.CompareNoCase("ring") == 0)
			{
				const double lo[] = { 0, 0, 0 }, hi[] = { 4096, 4096, 4 };
				double v[3] = { d.RingRadius, d.RingThickness, d.RingWeight };
				ok = ReadNumbers(e, 3, 3, lo, hi, nullptr, "radius, thickness, weight", v, error, errorLine);
				d.RingRadius = v[0]; d.RingThickness = v[1]; d.RingWeight = v[2];
			}
			else if (e.Key.CompareNoCase("softness") == 0) ok = ReadNumber(e, 0, 1, d.Softness, error, errorLine);
			else if (e.Key.CompareNoCase("noise") == 0)
			{
				const double lo[] = { 0, 0, 1 }, hi[] = { 16, 1, 2 };
				const bool whole[] = { false, false, true };
				double v[3] = { d.NoiseScale, d.NoiseAmount, (double)d.NoiseOctaves };
				ok = ReadNumbers(e, 2, 3, lo, hi, whole, "scale (cells per map unit), amount 0..1[, octaves 1..2]", v, error, errorLine);
				d.NoiseScale = v[0]; d.NoiseAmount = v[1]; d.NoiseOctaves = (int)v[2];
			}
			else if (e.Key.CompareNoCase("billow") == 0) ok = ReadNumber(e, 0, 4096, d.Billow, error, errorLine);
			else if (e.Key.CompareNoCase("churn") == 0) ok = ReadNumber(e, 0, 64, d.Churn, error, errorLine);
			else if (e.Key.CompareNoCase("grow") == 0)
			{
				const double lo[] = { 0, 0 }, hi[] = { 4, 1 };
				double v[2] = { d.GrowStart, d.GrowShare };
				ok = ReadNumbers(e, 2, 2, lo, hi, nullptr, "start scale, share of life", v, error, errorLine);
				d.GrowStart = v[0]; d.GrowShare = v[1];
			}
			else if (e.Key.CompareNoCase("expand") == 0) ok = ReadNumber(e, 0, 65536, d.Expand, error, errorLine);
			else if (e.Key.CompareNoCase("life") == 0) ok = ReadNumber(e, LIFE_MIN, LIFE_MAX, d.Life, error, errorLine);
			else if (e.Key.CompareNoCase("heat") == 0)
			{
				const double lo[] = { 0, 0 }, hi[] = { 1, 1 };
				double v[2] = { d.HeatStart, d.HeatEnd };
				ok = ReadNumbers(e, 2, 2, lo, hi, nullptr, "heat at birth, at death (0..1)", v, error, errorLine);
				d.HeatStart = v[0]; d.HeatEnd = v[1];
			}
			else if (e.Key.CompareNoCase("heatfalloff") == 0) ok = ReadNumber(e, 0, 4, d.HeatFalloff, error, errorLine);
			else if (e.Key.CompareNoCase("tint") == 0) ok = ReadTint(e, d.Tint, error, errorLine);
			else if (e.Key.CompareNoCase("intensity") == 0) ok = ReadIntensity(e, d, error, errorLine);
			else if (e.Key.CompareNoCase("absorption") == 0) ok = ReadNumber(e, 0, 4, d.Absorption, error, errorLine);
			else if (e.Key.CompareNoCase("flicker") == 0)
			{
				const double lo[] = { 0, 0 }, hi[] = { 1, 240 };
				double v[2] = { d.Flicker, d.FlickerRate };
				ok = ReadNumbers(e, 1, 2, lo, hi, nullptr, "amount 0..1[, rate (Hz)]", v, error, errorLine);
				d.Flicker = v[0]; d.FlickerRate = v[1];
			}
			else if (e.Key.CompareNoCase("drag") == 0) ok = ReadNumber(e, 0, 64, d.Drag, error, errorLine);
			else if (e.Key.CompareNoCase("motion") == 0)
			{
				static const char *const choices[] = { "trail", "stay", "ride" };
				ok = ReadChoice(e, choices, 3, d.Motion, error, errorLine);
			}
			else if (e.Key.CompareNoCase("followtip") == 0) ok = ReadNumber(e, 0, 1, d.FollowTip, error, errorLine);
			else if (e.Key.CompareNoCase("light") == 0)
			{
				const double lo[] = { 0, 0 }, hi[] = { 1024, 16 };
				double v[2] = { d.LightRadius, d.LightIntensity };
				ok = ReadNumbers(e, 2, 2, lo, hi, nullptr, "radius (0 = none, else 1..1024), intensity 0..16", v, error, errorLine);
				if (ok && v[0] > 0.0 && v[0] < 1.0)
					ok = Fail(error, errorLine, e.Line, "'light' radius %g: 0 for no light, else 1 .. 1024", v[0]);
				d.LightRadius = v[0]; d.LightIntensity = v[1];
			}
			else if (e.Key.CompareNoCase("lightline") == 0) ok = ReadWhole(e, 0, 1, d.LightLine, error, errorLine);
			else if (e.Key.CompareNoCase("lightflags") == 0) ok = ReadLightFlags(e, d.LightFlags, error, errorLine);
			else if (e.Key.CompareNoCase("signature") == 0) ok = ReadWhole(e, 0, 1, d.Signature, error, errorLine);
			else if (e.Key.CompareNoCase("wobble") == 0) ok = ReadNumber(e, 0, 0.5, d.Wobble, error, errorLine);
			else
			{
				return Fail(error, errorLine, e.Line, "unknown key '%s' (VOLUMEDEFS keys: class shape offset petals petaljitter ring softness noise "
					"billow churn grow expand life heat heatfalloff tint intensity absorption flicker drag motion followtip light lightline "
					"lightflags signature wobble)", key);
			}
			if (!ok)
				return false;
		}
		return true;
	}
}

//==========================================================================
//
// Loading
//
//==========================================================================

void LoadEmissiveVolumeDefinitions()
{
	DefinitionTable &table = Table();
	table.Named.Clear();
	table.ByHandle.Clear();
	table.ReportedHandles.Clear();
	table.Lumps = 0;
	table.Replaced = 0;
	table.Stats = FDefBlockStats();

	const FDefBlockHandler handler = [&table](const FDefBlock &block, FString &error, int &errorLine) -> bool
	{
		if (block.Kind.CompareNoCase("emissive") != 0)
		{
			error.Format("unknown kind '%s' (VOLUMEDEFS has emissive)", block.Kind.GetChars());
			return false;
		}
		Definition d;
		if (!BuildDefinition(block, d, error, errorLine))
			return false;

		const int handle = EmissiveVolumeDefinitionHandle(block.Name.GetChars());
		const int *existing = table.ByHandle.CheckKey(handle);
		if (existing != nullptr)
		{
			NamedDefinition &earlier = table.Named[*existing];
			if (earlier.Name.CompareNoCase(block.Name.GetChars()) != 0)
			{
				// Two different names, one 31-bit hash: refused at load, so a handle always means one name.
				error.Format("its name has the same handle (%d) as '%s' at %s, line %d -- rename one of them",
					handle, earlier.Name.GetChars(), earlier.Lump.GetChars(), earlier.Line);
				return false;
			}
			Printf("EmissiveVolumes: '%s' at %s, line %d replaces the one at %s, line %d\n", block.Name.GetChars(),
				block.LumpName.GetChars(), block.Line, earlier.Lump.GetChars(), earlier.Line);
			earlier.Lump = block.LumpName;
			earlier.Line = block.Line;
			earlier.Def = d;
			table.Replaced++;
			return true;
		}
		if (table.Named.Size() >= kMaxDefinitions)
		{
			error.Format("the table already holds %u definitions, the most it can", kMaxDefinitions);
			return false;
		}
		NamedDefinition named;
		named.Name = block.Name;
		named.Lump = block.LumpName;
		named.Line = block.Line;
		named.Handle = handle;
		named.Def = d;
		table.ByHandle.Insert(handle, (int)table.Named.Size());
		table.Named.Push(named);
		return true;
	};

	int lastLump = 0;
	int lump;
	while ((lump = fileSystem.FindLumpFullName("VOLUMEDEFS", &lastLump, true)) != -1)
	{
		table.Lumps++;
		ReadDefinitionBlocks(lump, handler, table.Stats);
	}

	Printf("EmissiveVolumes: %u definition%s from %d VOLUMEDEFS lump%s -- %d refused, %d replaced by a later one (drawn on Vulkan only)\n",
		table.Named.Size(), table.Named.Size() == 1 ? "" : "s", table.Lumps, table.Lumps == 1 ? "" : "s", table.Stats.Refused, table.Replaced);
}

//==========================================================================
//
// Handles. NETPLAY: a pure function of the name's text; only the renderer and the presentation query ask what it stands for.
//
//==========================================================================

int EmissiveVolumeDefinitionHandle(const char *name)
{
	return HandleFor(name);
}

const Definition *FindEmissiveVolumeDefinition(int handle, bool report)
{
	DefinitionTable &table = Table();
	if (const int *index = table.ByHandle.CheckKey(handle))
		return &table.Named[*index].Def;

	if (report && handle != 0 && table.ReportedHandles.CheckKey(handle) == nullptr && table.ReportedHandles.CountUsed() < 64)
	{
		table.ReportedHandles.Insert(handle, true);
		Printf(TEXTCOLOR_ORANGE "EmissiveVolumes: SpawnEmissiveVolume was given handle %d, which no VOLUMEDEFS definition on this machine has -- "
			"nothing drawn (see `emissivevolumes` for the loaded names; this only ever changes this machine's pixels)\n", handle);
	}
	return nullptr;
}

int EmissiveVolumeDefinitionCount()
{
	return (int)Table().Named.Size();
}

CCMD(emissivevolumes)
{
	DefinitionTable &table = Table();
	static const char *const classes[] = { "gun", "biggun", "blast" };
	static const char *const motions[] = { "trail", "stay", "ride" };
	Printf("Emissive volume definitions: %u, from %d VOLUMEDEFS lump%s (%d refused, %d replaced)\n", table.Named.Size(), table.Lumps,
		table.Lumps == 1 ? "" : "s", table.Stats.Refused, table.Replaced);
	for (unsigned i = 0; i < table.Named.Size(); i++)
	{
		const NamedDefinition &n = table.Named[i];
		const Definition &d = n.Def;
		const Extent extent = ExtentOf(d, LengthsAt(d, 1.0));
		Printf("  %-24s handle %10d  %-6s life %.3fs  bound %.1f  petals %d  ring %s  absorption %.2f  motion %s  light %s  (%s, line %d)\n",
			n.Name.GetChars(), n.Handle, classes[std::clamp(d.Class, 0, 2)], d.Life, extent.Radius, d.PetalCount,
			d.RingWeight > 0.0 && d.RingThickness > 0.0 ? "yes" : "no", d.Absorption, motions[std::clamp(d.Motion, 0, 2)],
			d.LightRadius >= 1.0 && d.LightIntensity > 0.0 ? (d.LightLine ? "line" : "point") : "none", n.Lump.GetChars(), n.Line);
	}
}
