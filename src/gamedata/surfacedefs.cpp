/*
** surfacedefs.cpp
**
** [SURFACEMATERIALS] What textures are made of. See surfacedefs.h for the lump,
** the priority order and the netplay notes.
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

#include <cstring>
#include <string>

#include "surfacedefs.h"
#include "defblocks.h"
#include "doomtype.h"			// PClassActor, which p_terrain.h names
#include "cmdlib.h"
#include "p_terrain.h"			// TerrainTypes, Terrains, FTerrainDef
#include "textures.h"			// FTexture, then FGameTexture (gametexture.h, which needs FTexture complete)
#include "textureid.h"			// ETextureType, FTextureID
#include "texturemanager.h"		// TexMan
#include "filesystem.h"
#include "printf.h"
#include "c_dispatch.h"
#include "c_commandline.h"		// FCommandLine
#include "engineerrors.h"		// CRecoverableError
#include "i_time.h"
#include "name.h"
#include "tarray.h"
#include "zstring.h"

namespace
{
	enum class ESurfaceList : uint8_t { Walls, Flats, Any };
	const char *const kListNames[] = { "walls", "flats", "any" };

	// FString, not std::string, in anything a TArray holds: TArray grows with
	// realloc, which only relocatable types survive.
	struct SurfaceRule
	{
		FName Surface = NAME_None;
		ESurfaceList List = ESurfaceList::Any;
		bool Literal = false;		// no * or ?: the whole pattern is its prefix
		FString Pattern;			// upper-case (ASCII)
		unsigned PrefixLen = 0;		// the literal run before the first * or ?
		FString Lump;
		int Line = 0;
	};

	enum class EOrigin : uint8_t { None, Rule, Tag, Terrain };

	// What decided a texture's answer, for the `surface` command and for TERRAIN's
	// "unless a rule or tag already decided". A rule that says `none` decides too.
	struct SurfaceOrigin
	{
		EOrigin Kind = EOrigin::None;
		int Index = -1;				// Rule: Rules. Tag: Tags. Terrain: Terrains.
	};

	struct SurfaceTag
	{
		FString Source;
		int Line = 0;
	};

	struct SurfaceTable
	{
		TArray<SurfaceRule> Rules;
		TArray<SurfaceOrigin> Origins;	// by texture index
		TArray<SurfaceTag> Tags;
		unsigned Lumps = 0;
		FDefBlockStats Stats;
		bool Ready = false;				// rules loaded: the added-texture hook classifies
	};

	SurfaceTable &Table()
	{
		static SurfaceTable table;
		return table;
	}

	// ASCII only, never the C locale's toupper: a name must fold the same way on
	// every machine (netplay).
	inline char UpperAscii(char c)
	{
		return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
	}

	// * matches any run (empty too), ? any one character. Both sides are already
	// upper-case. The same walk as RS_Ballistics' RSB_MaterialDef.Glob, so a list
	// moved from RSBDEFS classifies exactly as the script prototype did.
	bool GlobMatch(const char *pat, size_t pn, const char *text, size_t sn)
	{
		size_t pi = 0, si = 0, star = 0, mark = 0;
		bool haveStar = false;
		while (si < sn)
		{
			if (pi < pn && (pat[pi] == '?' || pat[pi] == text[si]))
			{
				pi++;
				si++;
			}
			else if (pi < pn && pat[pi] == '*')
			{
				star = pi++;
				haveStar = true;
				mark = si;
			}
			else if (haveStar)
			{
				pi = star + 1;
				si = ++mark;
			}
			else
			{
				return false;
			}
		}
		while (pi < pn && pat[pi] == '*') pi++;
		return pi == pn;
	}

	bool ListCovers(ESurfaceList list, ETextureType type)
	{
		switch (list)
		{
		case ESurfaceList::Walls: return type == ETextureType::Wall || type == ETextureType::WallPatch || type == ETextureType::Override;
		case ESurfaceList::Flats: return type == ETextureType::Flat;
		case ESurfaceList::Any: return true;
		}
		return false;
	}

	// The name a texture is matched by, upper-case: what TexMan.GetName gives
	// script -- the texture's own name, or for a full-path texture the path of the
	// lump it was made from. Empty for a nameless texture, which no rule matches.
	std::string ClassifiedName(FGameTexture *tex)
	{
		std::string name;
		if (tex->GetName().IsNotEmpty())
		{
			name = tex->GetName().GetChars();
		}
		else if (tex->GetTexture() != nullptr)
		{
			const int lump = tex->GetSourceLump();
			if (lump >= 0 && TexMan.GetLinkedTexture(lump) == tex)
			{
				const char *full = fileSystem.GetFileFullName(lump);
				if (full != nullptr) name = full;
			}
		}
		for (char &c : name) c = UpperAscii(c);
		return name;
	}

	void ClassifyTexture(SurfaceTable &table, FGameTexture *tex, int index)
	{
		if (tex == nullptr || index < 0) return;
		if ((unsigned)index >= table.Origins.Size()) table.Origins.Resize((unsigned)index + 1);

		SurfaceOrigin origin;
		FName surface = NAME_None;
		// No SURFACES lump loaded (the default): nothing to match, no name to build.
		const std::string name = table.Rules.Size() > 0 ? ClassifiedName(tex) : std::string();
		if (!name.empty())
		{
			const ETextureType type = tex->GetUseType();
			for (int r = (int)table.Rules.Size() - 1; r >= 0; r--)
			{
				const SurfaceRule &rule = table.Rules[r];
				if (!ListCovers(rule.List, type)) continue;
				// The literal-prefix fast path: nearly every rule fails on its first letters.
				if (name.size() < rule.PrefixLen || memcmp(name.data(), rule.Pattern.GetChars(), rule.PrefixLen) != 0) continue;
				const bool matches = rule.Literal
					? name.size() == rule.PrefixLen
					: GlobMatch(rule.Pattern.GetChars(), rule.Pattern.Len(), name.data(), name.size());
				if (matches)
				{
					surface = rule.Surface;
					origin.Kind = EOrigin::Rule;
					origin.Index = r;
					break;
				}
			}
		}
		tex->SetSurface(surface);
		table.Origins[index] = origin;
	}

	// FTextureManager::GameTextureAddedHook: a texture created after load is
	// classified as the load pass would have classified it.
	void OnGameTextureAdded(FGameTexture *tex)
	{
		SurfaceTable &table = Table();
		if (table.Ready && tex != nullptr) ClassifyTexture(table, tex, tex->GetID().GetIndex());
	}

	// One `surface <name> { walls = ..., flats = ..., any = ... }` block. Nothing is
	// added unless the whole block is good.
	bool ReadSurfaceBlock(SurfaceTable &table, const FDefBlock &block, FString &error, int &errorLine)
	{
		if (block.Kind.CompareNoCase("surface") != 0)
		{
			error.Format("a SURFACES lump holds only surface blocks, not '%s' -- surface <name> { walls = \"PATTERN*\", ... }", block.Kind.GetChars());
			return false;
		}
		if (block.Name.IsEmpty())
		{
			error = "a surface needs a name, e.g. surface metal { ... }";
			return false;
		}
		// `none`, in any case, is NAME_None: a later lump un-tags what an earlier one tagged.
		const FName surface(block.Name.GetChars());

		TArray<SurfaceRule> rules;
		for (const FDefBlockEntry &entry : block.Entries)
		{
			ESurfaceList list = ESurfaceList::Any;
			if (entry.Key.CompareNoCase("walls") == 0) list = ESurfaceList::Walls;
			else if (entry.Key.CompareNoCase("flats") == 0) list = ESurfaceList::Flats;
			else if (entry.Key.CompareNoCase("any") == 0) list = ESurfaceList::Any;
			else
			{
				error.Format("unknown key '%s' -- the keys are walls, flats and any", entry.Key.GetChars());
				errorLine = entry.Line;
				return false;
			}

			for (const FDefBlockItem &item : entry.Items)
			{
				const int line = item.Line > 0 ? item.Line : entry.Line;
				if (item.HasAt)
				{
					error.Format("'@' has no meaning in a texture list (%s)", entry.Key.GetChars());
					errorLine = line;
					return false;
				}
				for (const FDefBlockAtom &atom : item.Atoms)
				{
					if (atom.Kind == FDefBlockAtom::Number)
					{
						error.Format("%s: '%s' reads as a number -- put texture names and patterns in quotes, e.g. \"NAME\", \"PATTERN*\"",
							entry.Key.GetChars(), atom.Text.GetChars());
						errorLine = line;
						return false;
					}
					if (atom.Text.IsEmpty())
					{
						error.Format("%s: an empty texture name", entry.Key.GetChars());
						errorLine = line;
						return false;
					}

					std::string upper = atom.Text.GetChars();
					for (char &c : upper) c = UpperAscii(c);
					const size_t wild = upper.find_first_of("*?");

					SurfaceRule rule;
					rule.Surface = surface;
					rule.List = list;
					rule.Literal = wild == std::string::npos;
					rule.Pattern = FString(upper.c_str(), upper.size());
					rule.PrefixLen = (unsigned)(rule.Literal ? upper.size() : wild);
					rule.Lump = block.LumpName;
					rule.Line = line;
					rules.Push(rule);
				}
			}
		}

		if (rules.Size() == 0)
		{
			error = "it lists no textures -- give it walls, flats or any";
			return false;
		}
		for (const SurfaceRule &rule : rules) table.Rules.Push(rule);
		return true;
	}

	const char *UseTypeName(ETextureType type)
	{
		switch (type)
		{
		case ETextureType::Any: return "any";
		case ETextureType::Wall: return "wall";
		case ETextureType::Flat: return "flat";
		case ETextureType::Sprite: return "sprite";
		case ETextureType::WallPatch: return "wall patch";
		case ETextureType::Build: return "build";
		case ETextureType::SkinSprite: return "skin sprite";
		case ETextureType::Decal: return "decal";
		case ETextureType::MiscPatch: return "graphic";
		case ETextureType::FontChar: return "font character";
		case ETextureType::Override: return "override";
		case ETextureType::Autopage: return "automap page";
		case ETextureType::SkinGraphic: return "skin graphic";
		case ETextureType::Null: return "null";
		case ETextureType::FirstDefined: return "first defined";
		case ETextureType::Special: return "special";
		case ETextureType::SWCanvas: return "canvas";
		}
		return "unknown";
	}

	void PrintTextureSurface(int index)
	{
		SurfaceTable &table = Table();
		FGameTexture *tex = TexMan.GameByIndex(index);
		if (tex == nullptr) return;

		const SurfaceOrigin origin = (unsigned)index < table.Origins.Size() ? table.Origins[index] : SurfaceOrigin();
		FString why;
		switch (origin.Kind)
		{
		case EOrigin::Rule:
			if ((unsigned)origin.Index < table.Rules.Size())
			{
				const SurfaceRule &rule = table.Rules[origin.Index];
				why.Format("SURFACES %s \"%s\", %s line %d", kListNames[(int)rule.List], rule.Pattern.GetChars(), rule.Lump.GetChars(), rule.Line);
			}
			break;
		case EOrigin::Tag:
			if ((unsigned)origin.Index < table.Tags.Size())
			{
				const SurfaceTag &tag = table.Tags[origin.Index];
				why.Format("GLDEFS surface, %s line %d", tag.Source.GetChars(), tag.Line);
			}
			break;
		case EOrigin::Terrain:
			if ((unsigned)origin.Index < Terrains.Size())
				why.Format("TERRAIN '%s'", Terrains[origin.Index].Name.GetChars());
			break;
		case EOrigin::None:
			why = table.Ready ? "no rule, tag or terrain names it" : "SURFACES has not loaded";
			break;
		}

		const std::string name = ClassifiedName(tex);
		Printf("%s (%s, texture %d): %s -- %s\n", name.empty() ? "(nameless)" : name.c_str(), UseTypeName(tex->GetUseType()), index,
			tex->GetSurface().GetChars(), why.GetChars());
	}
}

//==========================================================================
//
// Loading
//
//==========================================================================

void LoadSurfaceDefinitions()
{
	SurfaceTable &table = Table();
	const double started = I_msTimeF();

	table.Ready = false;
	table.Rules.Clear();
	table.Origins.Clear();
	table.Tags.Clear();
	table.Lumps = 0;
	table.Stats = FDefBlockStats();

	const FDefBlockHandler handler = [&table](const FDefBlock &block, FString &error, int &errorLine) -> bool
	{
		return ReadSurfaceBlock(table, block, error, errorLine);
	};

	// Full name, extension ignored: SURFACES, SURFACES.txt... at a pk3's root, or a
	// WAD lump (the name is exactly 8 characters). Load order = lump order.
	int lastLump = 0;
	int lump;
	while ((lump = fileSystem.FindLumpFullName("SURFACES", &lastLump, true)) != -1)
	{
		table.Lumps++;
		try
		{
			ReadDefinitionBlocks(lump, handler, table.Stats);
		}
		catch (const CRecoverableError &err)
		{
			// The reader itself never throws, but FScanner raises a script error on a
			// character it has no token for (a stray $ or \ outside quotes). Blocks
			// before that point are kept; the game still starts.
			Printf(TEXTCOLOR_RED "%s: SURFACES not read past this point -- %s\n", fileSystem.GetFileFullPath(lump).c_str(), err.GetMessage());
			table.Stats.Refused++;
		}
	}

	const int count = TexMan.NumTextures();
	table.Origins.Resize((unsigned)count);
	int tagged = 0;
	for (int i = 0; i < count; i++)
	{
		FGameTexture *tex = TexMan.GameByIndex(i);
		if (tex == nullptr) continue;
		ClassifyTexture(table, tex, i);
		if (tex->GetSurface() != NAME_None) tagged++;
	}

	table.Ready = true;
	TexMan.AddGameTextureAddedHook(OnGameTextureAdded);

	Printf("Surfaces: %u rule%s from %u lump%s, %d texture%s tagged, %d refused (%.1f ms)\n",
		table.Rules.Size(), table.Rules.Size() == 1 ? "" : "s", table.Lumps, table.Lumps == 1 ? "" : "s",
		tagged, tagged == 1 ? "" : "s", table.Stats.Refused, I_msTimeF() - started);
}

void TagTextureSurface(FGameTexture *tex, FName surface, const char *source, int line)
{
	if (tex == nullptr) return;
	SurfaceTable &table = Table();

	tex->SetSurface(surface);

	const int index = tex->GetID().GetIndex();
	if (index < 0) return;
	if ((unsigned)index >= table.Origins.Size()) table.Origins.Resize((unsigned)index + 1);

	SurfaceTag tag;
	tag.Source = source != nullptr ? source : "";
	tag.Line = line;
	table.Origins[index].Kind = EOrigin::Tag;
	table.Origins[index].Index = (int)table.Tags.Push(tag);
}

void ApplyTerrainSurfaces()
{
	SurfaceTable &table = Table();
	const unsigned textures = (unsigned)TexMan.NumTextures();
	const unsigned count = TerrainTypes.Types.Size() < textures ? TerrainTypes.Types.Size() : textures;

	int tagged = 0;
	for (unsigned i = 0; i < count; i++)
	{
		const uint16_t terrain = TerrainTypes.Types[i];
		if (terrain == 0xffff || terrain >= Terrains.Size()) continue;	// no `floor` line maps it
		const FName surface = Terrains[terrain].SurfaceName;
		if (surface == NAME_None) continue;

		FGameTexture *tex = TexMan.GameByIndex((int)i);
		if (tex == nullptr) continue;
		if (i >= table.Origins.Size()) table.Origins.Resize(i + 1);

		SurfaceOrigin &origin = table.Origins[i];
		if (origin.Kind != EOrigin::None) continue;	// a rule or an exact tag decided it, `none` included

		tex->SetSurface(surface);
		origin.Kind = EOrigin::Terrain;
		origin.Index = terrain;
		tagged++;
	}

	if (tagged > 0)
	{
		Printf("Surfaces: %d texture%s tagged by TERRAIN\n", tagged, tagged == 1 ? "" : "s");
	}
}

//==========================================================================
//
// Console commands, for the desk (the headset proof needs no typing).
//
// `surface <texture>` -- what every texture of that name is made of, and which
// rule, tag or terrain said so. A full-path name is looked up, never created.
//
//==========================================================================

CCMD(surface)
{
	if (argv.argc() < 2)
	{
		Printf("surface <texture> -- what a texture is made of, and which SURFACES rule, GLDEFS tag or TERRAIN said so\n");
		return;
	}

	TArray<FTextureID> list;
	TexMan.ListTextures(argv[1], list, true);
	if (list.Size() == 0)
	{
		const FTextureID id = TexMan.CheckForTexture(argv[1], ETextureType::Any,
			FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_DontCreate);
		if (id.GetIndex() > 0) list.Push(id);
	}
	if (list.Size() == 0)
	{
		Printf("surface: no texture named '%s'\n", argv[1]);
		return;
	}
	for (const FTextureID &id : list)
	{
		PrintTextureSurface(id.GetIndex());
	}
}

//==========================================================================
//
// `surfaces` -- how many textures each surface name has, and from where.
//
//==========================================================================

CCMD(surfaces)
{
	SurfaceTable &table = Table();
	const int count = TexMan.NumTextures();

	Printf("Surfaces: %u rules from %u SURFACES lump%s (%d blocks accepted, %d refused), %u GLDEFS tags, %d textures\n",
		table.Rules.Size(), table.Lumps, table.Lumps == 1 ? "" : "s", table.Stats.Accepted, table.Stats.Refused,
		table.Tags.Size(), count);

	TArray<FName> names;
	TArray<int> totals, byRule, byTag, byTerrain;
	int tagged = 0;
	for (int i = 0; i < count; i++)
	{
		FGameTexture *tex = TexMan.GameByIndex(i);
		if (tex == nullptr || tex->GetSurface() == NAME_None) continue;
		tagged++;

		const FName surface = tex->GetSurface();
		unsigned j = names.Find(surface);
		if (j == names.Size())
		{
			names.Push(surface);
			totals.Push(0);
			byRule.Push(0);
			byTag.Push(0);
			byTerrain.Push(0);
		}
		totals[j]++;
		const EOrigin kind = (unsigned)i < table.Origins.Size() ? table.Origins[i].Kind : EOrigin::None;
		if (kind == EOrigin::Rule) byRule[j]++;
		else if (kind == EOrigin::Tag) byTag[j]++;
		else if (kind == EOrigin::Terrain) byTerrain[j]++;
	}

	for (unsigned j = 0; j < names.Size(); j++)
	{
		Printf("  %-16s %5d  (SURFACES %d, GLDEFS %d, TERRAIN %d)\n", names[j].GetChars(), totals[j], byRule[j], byTag[j], byTerrain[j]);
	}
	Printf("  %d of %d textures tagged\n", tagged, count);
}
