/*
** p_openmap.cpp
**
** creates the data structures needed to load a map from the resource files.
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2016 Marisa Heit
** Copyright 2005-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "p_setup.h"

#include "cmdlib.h"
#include "filesystem.h"
#include "md5.h"
#include "g_levellocals.h"
#include "cmdlib.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "m_argv.h"
#include "roth/roth_install.h"
#include "roth/roth_log.h"

#define IWAD_ID		MAKE_ID('I','W','A','D')
#define PWAD_ID		MAKE_ID('P','W','A','D')


inline bool P_IsBuildMap(MapData *map)
{
	return false;
}

//===========================================================================
//
// Realms of the Haunting maps are read from the player's own installation.
// `roth_path` points at the folder holding ROTH.RES and M\; when a requested
// map name appears in that manifest it is loaded from there rather than from
// a lump. Nothing derived from the game is generated, cached or shipped.
//
//===========================================================================

FARG(rothpath, "Realms of the Haunting",
	"Path to a Realms of the Haunting installation.",
	"path",
	"Points at the folder containing ROTH.RES and M\\ inside a legitimate copy of"
	" Realms of the Haunting. Its maps and artwork are then read directly from"
	" that installation. Nothing derived from the game is generated or stored;"
	" a copy of the original is required to play.");

CUSTOM_CVAR(String, roth_path, "", CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	if (*self == nullptr || **self == 0)
		return;
	if (!roth::TheInstall().Open(self))
		Printf(TEXTCOLOR_RED "roth_path: %s\n", roth::TheInstall().Error().c_str());
	else
		Printf("Realms of the Haunting: %d maps available\n",
			(int)roth::TheInstall().MapNames().size());
}

// True when this map name belongs to the install. Returns false for every
// ordinary map, so the usual lump lookup is left completely untouched.
static bool P_IsRothMap(const char *mapname, MapData *map)
{
	auto &install = roth::TheInstall();
	static bool reported = false;   // say this once, not once per map lookup

	if (!install.IsOpen())
	{
		// Accept -rothpath on the command line as well as the cvar.
		const char *fromArgs = Args->CheckValue(FArg_rothpath);
		const char *path = (fromArgs && *fromArgs) ? fromArgs
			: ((roth_path && *roth_path) ? (const char *)roth_path : nullptr);

		if (path)
		{
			if (!install.Open(path) && !reported)
			{
				reported = true;
				Printf(TEXTCOLOR_RED "Realms of the Haunting: %s\n",
					install.Error().c_str());
			}
			else if (install.IsOpen() && !reported)
			{
				reported = true;
				Printf("Realms of the Haunting: %d maps found in %s\n",
					(int)install.MapNames().size(), install.Path().c_str());
			}
		}
	}

	if (!install.IsOpen())
		return false;
	if (!install.HasMap(mapname))
		return false;

	map->isRoth = true;
	map->rothFile = install.MapFile(mapname).c_str();
	map->rothPack = install.PackFor(mapname).c_str();

	// Every load gets its own report. Loading a foreign format off someone
	// else's disk fails in ways that are hard to diagnose after the fact.
	auto &log = roth::TheLog();
	log.Begin(mapname);
	log.Section("Source");
	log.Line("  install   %s", install.Path().c_str());
	log.Line("  map file  %s", map->rothFile.GetChars());
	log.Line("  artwork   %s  (shared: %s)", map->rothPack.GetChars(),
		install.SharedPack().c_str());
	return true;
}

//===========================================================================
//
// GetMapIndex
//
// Gets the type of map lump or -1 if invalid or -2 if required and not found.
//
//===========================================================================

struct checkstruct
{
	const char lumpname[9];
	bool  required;
};

static int GetMapIndex(const char *mapname, int lastindex, const char *lumpname, bool needrequired)
{
	static const checkstruct check[] =
	{
		{"",		 true},
		{"THINGS",	 true},
		{"LINEDEFS", true},
		{"SIDEDEFS", true},
		{"VERTEXES", true},
		{"SEGS",	 false},
		{"SSECTORS", false},
		{"NODES",	 false},
		{"SECTORS",	 true},
		{"REJECT",	 false},
		{"BLOCKMAP", false},
		{"BEHAVIOR", false},
		{"LIGHTMAP", false },
		//{"SCRIPTS",	 false},
	};

	if (lumpname==NULL) lumpname="";

	for(size_t i=lastindex+1;i<countof(check);i++)
	{
		if (!strnicmp(lumpname, check[i].lumpname, 8))
			return (int)i;

		if (check[i].required)
		{
			if (needrequired)
			{
				I_Error("'%s' not found in %s\n", check[i].lumpname, mapname);
			}
			return -2;
		}
	}

	return -1;	// End of map reached
}

//===========================================================================
//
// Opens a map for reading
//
//===========================================================================

MapData *P_OpenMapData(const char * mapname, bool justcheck)
{
	MapData * map = new MapData;
	FileReader * wadReader = nullptr;
	bool externalfile = !strnicmp(mapname, "file:", 5);

	// Check the Realms install before anything else: these maps live on disk in
	// their own format and have no lump to find.
	if (!externalfile && P_IsRothMap(mapname, map))
		return map;

	if (externalfile)
	{
		mapname += 5;
		if (!FileExists(mapname))
		{
			delete map;
			return NULL;
		}
		map->resource = FResourceFile::OpenResourceFile(mapname);
		wadReader = map->resource->GetContainerReader();
	}
	else
	{
		FString fmt;
		int lump_wad;
		int lump_map;
		int lump_name = -1;

		// Check for both *.wad and *.map in order to load Build maps
		// as well. The higher one will take precedence.
		// Names with more than 8 characters will only be checked as .wad and .map.
		if (strlen(mapname) <= 8) lump_name = fileSystem.CheckNumForName(mapname);
		fmt.Format("maps/%s.wad", mapname);
		lump_wad = fileSystem.CheckNumForFullName(fmt.GetChars());
		fmt.Format("maps/%s.map", mapname);
		lump_map = fileSystem.CheckNumForFullName(fmt.GetChars());

		if (lump_name > lump_wad && lump_name > lump_map && lump_name != -1)
		{
			int lumpfile = fileSystem.GetFileContainer(lump_name);
			int nextfile = fileSystem.GetFileContainer(lump_name+1);

			map->lumpnum = lump_name;

			if (lumpfile != nextfile)
			{
				// The following lump is from a different file so whatever this is,
				// it is not a multi-lump Doom level so let's assume it is a Build map.
				map->MapLumps[0].Reader = fileSystem.ReopenFileReader(lump_name);
				if (!P_IsBuildMap(map))
				{
					delete map;
					return NULL;
				}
				return map;
			}

			// This case can only happen if the lump is inside a real WAD file.
			// As such any special handling for other types of lumps is skipped.
			map->MapLumps[0].Reader = fileSystem.ReopenFileReader(lump_name);
			strncpy(map->MapLumps[0].Name, fileSystem.GetFileFullName(lump_name), 8);
			map->InWad = true;

			int index = 0;

			if (stricmp(fileSystem.GetFileFullName(lump_name + 1), "TEXTMAP") != 0)
			{
				for(int i = 1;; i++)
				{
					// Since levels must be stored in WADs they can't really have full
					// names and for any valid level lump this always returns the short name.
					const char * lumpname = fileSystem.GetFileFullName(lump_name + i);
					try
					{
						index = GetMapIndex(mapname, index, lumpname, !justcheck);
					}
					catch(...)
					{
						delete map;
						throw;
					}
					if (index == -2)
					{
						delete map;
						return NULL;
					}
					if (index == ML_BEHAVIOR) map->HasBehavior = true;

					// The next lump is not part of this map anymore
					if (index < 0) break;

					map->MapLumps[index].Reader = fileSystem.ReopenFileReader(lump_name + i);
					strncpy(map->MapLumps[index].Name, lumpname, 8);
				}
			}
			else
			{
				map->isText = true;
				map->MapLumps[1].Reader = fileSystem.ReopenFileReader(lump_name + 1);
				for(int i = 2;; i++)
				{
					const char * lumpname = fileSystem.GetFileFullName(lump_name + i);

					if (lumpname == NULL)
					{
						I_Error("Invalid map definition for %s", mapname);
					}
					else if (!stricmp(lumpname, "ZNODES"))
					{
						index = ML_GLZNODES;
					}
					else if (!stricmp(lumpname, "BLOCKMAP"))
					{
						// there is no real point in creating a blockmap but let's use it anyway
						index = ML_BLOCKMAP;
					}
					else if (!stricmp(lumpname, "REJECT"))
					{
						index = ML_REJECT;
					}
					else if (!stricmp(lumpname, "DIALOGUE"))
					{
						index = ML_CONVERSATION;
					}
					else if (!stricmp(lumpname, "BEHAVIOR"))
					{
						index = ML_BEHAVIOR;
						map->HasBehavior = true;
					}
					else if (!stricmp(lumpname, "LIGHTMAP"))
					{
						index = ML_LIGHTMAP;
					}
					else if (!stricmp(lumpname, "ENDMAP"))
					{
						break;
					}
					else continue;
					map->MapLumps[index].Reader = fileSystem.ReopenFileReader(lump_name + i);
					strncpy(map->MapLumps[index].Name, lumpname, 8);
				}
			}
			return map;
		}
		else
		{
			if (lump_map > lump_wad)
			{
				lump_wad = lump_map;
			}
			if (lump_wad == -1)
			{
				delete map;
				return NULL;
			}
			map->lumpnum = lump_wad;
			auto reader = fileSystem.ReopenFileReader(lump_wad);
			map->resource = FResourceFile::OpenResourceFile(fileSystem.GetFileFullName(lump_wad), reader, true);
			wadReader = map->resource->GetContainerReader();
		}
	}
	uint32_t id;

	// Although we're using the resource system, we still want to be sure we're
	// reading from a wad file.
	wadReader->Seek(0, FileReader::SeekSet);
	wadReader->Read(&id, sizeof(id));

	if (id == IWAD_ID || id == PWAD_ID)
	{
		char maplabel[9]="";
		int index=0;

		map->MapLumps[0].Reader = map->resource->GetEntryReader(0, FileSys::READER_SHARED);
		uppercopy(map->MapLumps[0].Name, map->resource->getName(0));

		for(uint32_t i = 1; i < map->resource->EntryCountU(); i++)
		{
			const char* lumpname = map->resource->getName(i);

			if (i == 1 && !strnicmp(lumpname, "TEXTMAP", 8))
			{
				map->isText = true;
				map->MapLumps[ML_TEXTMAP].Reader = map->resource->GetEntryReader(i, FileSys::READER_SHARED);
				strncpy(map->MapLumps[ML_TEXTMAP].Name, lumpname, 8);
				for(int i = 2;; i++)
				{
					lumpname = map->resource->getName(i);
					if (!strnicmp(lumpname, "ZNODES",8))
					{
						index = ML_GLZNODES;
					}
					else if (!strnicmp(lumpname, "BLOCKMAP",8))
					{
						// there is no real point in creating a blockmap but let's use it anyway
						index = ML_BLOCKMAP;
					}
					else if (!strnicmp(lumpname, "REJECT",8))
					{
						index = ML_REJECT;
					}
					else if (!strnicmp(lumpname, "DIALOGUE",8))
					{
						index = ML_CONVERSATION;
					}
					else if (!strnicmp(lumpname, "BEHAVIOR",8))
					{
						index = ML_BEHAVIOR;
						map->HasBehavior = true;
					}
					else if (!strnicmp(lumpname, "LIGHTMAP", 8))
					{
						index = ML_LIGHTMAP;
					}
					else if (!strnicmp(lumpname, "ENDMAP",8))
					{
						return map;
					}
					else continue;
					map->MapLumps[index].Reader = map->resource->GetEntryReader(i, FileSys::READER_SHARED);
					strncpy(map->MapLumps[index].Name, lumpname, 8);
				}
			}

			if (i>0)
			{
				try
				{
					index = GetMapIndex(maplabel, index, lumpname, !justcheck);
				}
				catch(...)
				{
					delete map;
					throw;
				}
				if (index == -2)
				{
					delete map;
					return NULL;
				}
				if (index == ML_BEHAVIOR) map->HasBehavior = true;

				// The next lump is not part of this map anymore
				if (index < 0) break;
			}
			else
			{
				strncpy(maplabel, lumpname, 8);
				maplabel[8]=0;
			}

			map->MapLumps[index].Reader = map->resource->GetEntryReader(i, FileSys::READER_SHARED);
			strncpy(map->MapLumps[index].Name, lumpname, 8);
		}
	}
	else
	{
		// This is a Build map and not subject to WAD consistency checks.
		//map->MapLumps[0].Size = wadReader->GetLength();
		if (!P_IsBuildMap(map))
		{
			delete map;
			return NULL;
		}
	}
	return map;
}

bool P_CheckMapData(const char *mapname)
{
	MapData *mapd = P_OpenMapData(mapname, true);
	if (mapd == NULL) return false;
	delete mapd;
	return true;
}

//===========================================================================
//
// MapData :: GetChecksum
//
// Hashes a map based on its header, THINGS, LINEDEFS, SIDEDEFS, SECTORS,
// and BEHAVIOR lumps. Node-builder generated lumps are not included.
//
//===========================================================================

void MapData::GetChecksum(uint8_t cksum[16])
{
	MD5Context md5;

	if (isRoth)
	{
		// A Realms map has no lumps at all -- it is a file on disk in the
		// player's install. Hash that file's bytes instead; reading the Doom
		// lumps here dereferences readers that were never opened.
		FileReader fr;
		if (fr.OpenFile(rothFile.GetChars()))
			md5Update(fr, md5, (uint32_t)fr.GetLength());
		md5.Final(cksum);
		return;
	}

	if (isText)
	{
		md5Update(Reader(ML_TEXTMAP), md5, Size(ML_TEXTMAP));
	}
	else
	{
		md5Update(Reader(ML_LABEL), md5, Size(ML_LABEL));
		md5Update(Reader(ML_THINGS), md5, Size(ML_THINGS));
		md5Update(Reader(ML_LINEDEFS), md5, Size(ML_LINEDEFS));
		md5Update(Reader(ML_SIDEDEFS), md5, Size(ML_SIDEDEFS));
		md5Update(Reader(ML_SECTORS), md5, Size(ML_SECTORS));
	}
	if (HasBehavior)
	{
		md5Update(Reader(ML_BEHAVIOR), md5, Size(ML_BEHAVIOR));
	}
	md5.Final(cksum);
}
