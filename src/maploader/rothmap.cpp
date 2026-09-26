/*
** rothmap.cpp
**
** Builds a level from a Realms of the Haunting map, read straight from the
** player's own installation.
**
** Realms is sector-based 2.5D in the same family as Doom: flat floor and
** ceiling heights per room, vertical walls between them. That makes the
** translation structural rather than a rebuild -- but the two engines differ in
** exactly the ways that matter, and those differences are commented where they
** occur rather than left as magic numbers.
**
** The most important one: Realms stores a two-sided wall as TWO faces that
** point at each other, one owned by each room. Doom stores the same wall as ONE
** line carrying two sides. Sister pairs must therefore be merged; emitting both
** would leave duplicate overlapping lines and wreck node building.
**
** Rules here are taken from ROTH.C, the original engine, via
** ROTH_NATIVE_HANDOFF.md. They are not inferred from how the result looks.
**
**---------------------------------------------------------------------------
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/

#include "maploader.h"
#include "p_setup.h"
#include "g_levellocals.h"
#include "texturemanager.h"
#include "printf.h"
#include "rendering/r_sky.h"
#include "playsim/p_lnspec.h"    // Sector_Outside, for the outside-fog test
#include "playsim/p_3dfloors.h"  // P_Add3DFloor, for Realms' intermediate floors

#include "roth/roth_raw.h"
#include "roth/roth_install.h"
#include "roth/roth_log.h"
#include "roth/roth_texture.h"
#include "roth/roth_objects.h"

//==========================================================================
//
// One Realms world unit is one map unit; the geometry is used as authored.
// Apparent scale problems are a player-size question, not a world one: the
// original doubles the map's recorded player height at load, so a player built
// from the raw value makes the whole world read as twice its intended size.
//
//==========================================================================

void MapLoader::LoadRothMap(MapData *map, FMissingTextureTracker &missingtex)
{
	auto &log = roth::TheLog();

	log.StageBegin("read file");
	auto bytes = roth::Install::ReadWholeFile(map->rothFile.GetChars());
	if (bytes.empty())
	{
		log.Warn("could not read %s", map->rothFile.GetChars());
		log.End();
		I_Error("Could not read %s", map->rothFile.GetChars());
	}

	log.StageBegin("parse");
	roth::Map rm = roth::ParseRaw(bytes.data(), bytes.size());
	if (!rm.ok())
	{
		log.Warn("parse failed: %s", rm.error.c_str());
		log.End();
		I_Error("Realms map %s: %s", map->rothFile.GetChars(), rm.error.c_str());
	}

	log.Section("Contents");
	log.Line("  sectors        %d", (int)rm.sectors.size());
	log.Line("  faces          %d", (int)rm.faces.size());
	log.Line("  vertices       %d", (int)rm.vertices.size());
	log.Line("  texture maps   %d", (int)rm.textureMaps.size());
	log.Line("  mid-platforms  %d", (int)rm.platforms.size());
	int objectCount = 0;
	for (auto &list : rm.objects) objectCount += (int)list.size();
	log.Line("  objects        %d", objectCount);
	log.Line("  player height  %d (%d doubled)", rm.metadata.playerHeight,
		rm.metadata.PlayerHeight());

	// LIGHTING IS DELIBERATELY FLAT FOR NOW. The real model is transcribed in
	// ROTH_LIGHTING.md and is ready to switch on: a sector's byte is a SIGNED
	// offset from 128 that sets how fast darkness closes in, the falloff rate
	// comes from lightAmbience indexing ROTH.C's g_shade_const_table_b, and
	// GZDoom's Build light mode is the same equation. Setting
	// Level->ShadeFalloffShift to that rate and info->lightmode to Build turns
	// it on; the two engine-side pieces it needs are already in place and inert
	// while the shift is zero.
	//
	// It is off because a faithfully dark manor is unusable to work in: you
	// cannot judge a texture or a prop you cannot see. Correct lighting is worth
	// nothing until the things it lights are right.
	//
	// Flat mapping meanwhile: keep the sector's relative ordering but sit it
	// around Doom's normal 160 so the level is readable.
	Level->ShadeFalloffShift = 0;
	log.Line("  lighting       FLAT (readable); Realms' model is off -- see ROTH_LIGHTING.md");

	// Make the player the size Realms says a person is, instead of leaving them
	// at Doom's 56 units in a world built for 144. That difference is why the
	// manor read as enormous: standing in a 154-unit world at 56 units tall,
	// with your eyes at 41 rather than 144, puts you at roughly a third of your
	// proper height and everything towers.
	//
	// The original's collision top is z + player_height + 10 (collision_physics.c:65),
	// so the body occupies height+10 and the eye sits 10 below the crown.
	//
	// THE GEOMETRY MUST NOT BE RESCALED TO COMPENSATE. Every other measurement
	// in the map -- step heights, door widths, how far a table is off the floor
	// -- is correct relative to this player. Rescaling the world corrupts all of
	// them to fix one. We have already been round this loop once.
	{
		const double ph = double(rm.metadata.PlayerHeight());
		Level->ForcedPlayerViewHeight = ph;
		Level->ForcedPlayerHeight = ph + 10.;
		log.Line("  player size    %.0f tall, eye at %.0f (Doom's default is 56 / 41)",
			Level->ForcedPlayerHeight, Level->ForcedPlayerViewHeight);
	}

	// Realms maps carry no BSP, blockmap or reject, so all of it is generated.
	ForceNodeBuild = true;

	//----------------------------------------------------------------------
	// Artwork
	//
	// The map's texture indices name entries in ONE DAS pack, the one ROTH.RES
	// lists against this map. Registering them under a name space of that
	// pack's own is not a tidiness question: TEX0001 in DEMO and TEX0001 in
	// DEMO3 are different pictures, and a shared name space shows one map's art
	// on another map's walls as soon as a second pack is loaded.
	//----------------------------------------------------------------------
	log.StageBegin("artwork");
	log.Section("Artwork");

	roth::TextureSet art;
	bool haveArt = false;
	{
		std::string packFile = roth::TheInstall().PackFile(map->rothPack.GetChars());
		log.Line("  pack file      %s", packFile.c_str());
		haveArt = art.Open(map->rothPack.GetChars(), packFile, &log);
		if (!haveArt)
		{
			// Not fatal: geometry without art is still a loadable, walkable
			// level, and a silent failure here would be much harder to chase.
			log.Warn("no artwork: %s -- surfaces will be blank", art.Error().c_str());
			log.Count("artwork: packs that failed to open");
		}
	}

	// Resolve one stored index. A pack's "draw nothing" index has no texture, so
	// the caller decides what that means for its surface.
	auto worldTex = [&](int index, bool masked = false) -> FTextureID
	{
		if (!haveArt) return FNullTextureID();
		return art.World(index, &log, masked);
	};

	// The sky is named by the MAP, not by the artwork pack -- the pack header's
	// word at +0x22 is something else entirely, and across the 44 retail maps
	// the two disagree on 16 of them. See roth_das.h.
	//
	// A Realms map has no MAPINFO, so without this the level's sky texture stays
	// null, r_sky falls back to "-noflat-", and every sky surface renders solid
	// black. That was the hole in the Study's ceiling.
	FTextureID skyTex = haveArt ? art.Sky(rm.metadata.skyTexture, &log) : FNullTextureID();
	if (skyTex.isValid())
	{
		// NOT set as the level sky any more. Realms' sky is a 256x146 backdrop
		// meant to be drawn as parallax COLUMNS above a wall, not stretched
		// around the world as a Doom sky -- doing that replaced a sky that at
		// least looked deliberate with one that does not. Resolved and kept for
		// when the parallax is modelled; the engine's own sky stands until then.
		(void)skyTex;
		Level->skyspeed1 = Level->skyspeed2 = 0.f;
		log.Line("  sky image      stored index %d", rm.metadata.skyTexture);
	}
	else
	{
		log.Count("artwork: no sky image -- sky surfaces will be black");
	}

	//----------------------------------------------------------------------
	// Vertices
	//----------------------------------------------------------------------
	// Realms gives a sector ONE optional intermediate floor -- a slab with its
	// own top and underside -- as an inline field. That is a tabletop, a shelf,
	// a balcony. GZDoom expresses the same thing as a 3D floor, which needs a
	// CONTROL SECTOR whose own floor and ceiling define the slab, plus a control
	// linedef the renderer reads the slab's side texture from.
	//
	// So every platform costs one sector, one line, one side and two vertices,
	// all of them off in the void where nothing references them. They have to be
	// counted NOW, because the arrays below hand out raw pointers (sd->sector,
	// ld->sidedef) and growing an array later would leave every one of them
	// dangling. Allocate once, exactly.
	// Only slabs with real thickness count: a top at or below its own underside
	// has no volume to render, and reserving space for one would leave a zeroed
	// sector and a zeroed line in the arrays for the node builder to trip over.
	int platformCount = 0;
	for (const auto &rs : rm.sectors)
	{
		if (rs.platformIndex < 0 || rs.platformIndex >= (int)rm.platforms.size()) continue;
		const roth::MidPlatform &mp = rm.platforms[rs.platformIndex];
		if (mp.topZ > mp.undersideZ) platformCount++;
	}

	//----------------------------------------------------------------------
	// Door leaves -- the census, for the same allocate-once reason.
	//
	// A Realms swinging door is NOT the door sector's walls. ROTH.C keeps the
	// swinging leaf as a DETACHED four-point quad living in the door record
	// (setup_door_swing_geometry, doors.c:474: the quad points go to
	// record+0x36 as hinge-relative offsets, and rotate_quad spins them about
	// record[0x14]/[0x16], the hinge vertex). The FS sector geometry is never
	// moved for a swinging door -- tick_swinging_doors only tags the two
	// touched sectors dirty. The door SECTOR is the doorway, a real passage
	// between two rooms; the leaf is a separate object filling it.
	//
	// So we build the leaf as its own geometry and LEAVE THE MAP ALONE. The
	// previous attempt (reverted, e1f7ba66ba) instead tagged the door sector's
	// existing two-sided walls as polyobject lines. That is what tore the map
	// apart, and the mechanism is exact: PO_Init sets SSECF_POLYORG on every
	// subsector holding a polyobject seg (polyobjects.cpp:421), and
	// HWDrawInfo::DoSubsector returns immediately for such a subsector
	// (hw_bsp.cpp:1224) -- it is never rendered again. Those walls were shared
	// with the rooms, so the ROOMS' subsectors were flagged and stopped
	// drawing. That also rules out the zero-delta "build it where it stands"
	// trick for good: building in place puts the discarded origin subsector
	// inside the live map. The Hexen void-room convention is not an avoidable
	// nicety, it is the reason the convention exists -- the sacrificed
	// subsector has to be somewhere nothing is lost.
	//
	// Per leaf, in the void: one sector, eight vertices, eight lines, eight
	// sides -- an annulus. The inner loop of four is the leaf itself (the
	// polyobject); the outer four seal the void room around it so the node
	// builder has a closed, convex container, which is also what
	// FNodeBuilder::FindPolyContainers wants for split avoidance.
	//
	// Only 0xFFFF sectors (roth::Sector::IsDoor) are built: they are the
	// primary pool, closed at load. VERIFIED across all 44 retail maps by a
	// standalone probe over the reader: 141 such sectors, every one with
	// exactly four faces, exactly one hinge, a CLOSED four-vertex loop and a
	// real positive height (218 for almost all of them). 0xFFFD, the
	// secondary pool, NEVER OCCURS in any retail map -- and ROTH.C's
	// tick_secondary_doors shows it would be a ceiling mover, not a swing, so
	// it would not be built here anyway. 0xFFFE (26 sectors game-wide, none in
	// STUDY1) is four-faced too but is not closed at load; whether it should
	// also get a leaf is an OPEN QUESTION, counted below and not guessed at.
	auto leafBuildable = [&](const roth::Sector &rs) -> bool
	{
		if (!rs.IsDoor() || rs.faceCount != 4 || rs.hingeFace < 0) return false;
		if (rs.firstFaceIndex < 0 || rs.firstFaceIndex + 3 >= (int)rm.faces.size()) return false;
		// The hinge must be one of THIS sector's four faces, because the leaf's
		// corner order is cyclic from it.
		const int hingeRel = rs.hingeFace - rs.firstFaceIndex;
		if (hingeRel < 0 || hingeRel > 3) return false;
		// The loop has to close, or the leaf is not a quad.
		for (int j = 0; j < 4; j++)
		{
			const roth::Face &a = rm.faces[rs.firstFaceIndex + j];
			const roth::Face &b = rm.faces[rs.firstFaceIndex + ((j + 1) & 3)];
			if (a.vertex1 < 0 || a.vertex1 >= (int)rm.vertices.size()) return false;
			if (a.vertex2 < 0 || a.vertex2 >= (int)rm.vertices.size()) return false;
			if (a.vertex2 != b.vertex1) return false;
		}
		return true;
	};
	int leafCount = 0;
	for (const auto &rs : rm.sectors)
		if (leafBuildable(rs)) leafCount++;

	log.StageBegin("vertices");
	Level->vertexes.Alloc(rm.vertices.size() + size_t(platformCount) * 4
		+ size_t(leafCount) * 8);
	for (size_t i = 0; i < rm.vertices.size(); i++)
		Level->vertexes[i].set(double(rm.vertices[i].x), double(rm.vertices[i].y));

	// Every vertex needs a matching extra record; the renderer expects one.
	vertexdatas.Clear();
	vertexdatas.Reserve(Level->vertexes.Size());
	memset(&vertexdatas[0], 0, sizeof(vertexdata_t) * vertexdatas.Size());

	//----------------------------------------------------------------------
	// Sectors
	//----------------------------------------------------------------------
	log.StageBegin("sectors");
	Level->sectors.Alloc(rm.sectors.size() + size_t(platformCount) + size_t(leafCount));
	Level->extsectors.Alloc(rm.sectors.size() + size_t(platformCount) + size_t(leafCount));
	memset(&Level->sectors[0], 0, sizeof(sector_t) * Level->sectors.Size());

	int doorCount = 0, doorsWithHinge = 0, flatsToSky = 0, flatFlipsIgnored = 0;
	int darkestLight = 255, brightestLight = 0;
	for (size_t i = 0; i < rm.sectors.size(); i++)
	{
		const roth::Sector &rs = rm.sectors[i];
		sector_t *sec = &Level->sectors[i];

		// Same defaults the UDMF loader applies. A zeroed sector is not a valid
		// one -- several fields mean "unset" only when they hold -1.
		sec->Level = Level;
		sec->e = &Level->extsectors[i];
		sec->sectornum = (int)i;
		sec->lightlevel = 160;
		sec->SetXScale(sector_t::floor, 1.);
		sec->SetYScale(sector_t::floor, 1.);
		sec->SetXScale(sector_t::ceiling, 1.);
		sec->SetYScale(sector_t::ceiling, 1.);
		sec->SetAlpha(sector_t::floor, 1.);
		sec->SetAlpha(sector_t::ceiling, 1.);
		sec->seqType = (Level->flags & LEVEL_SNDSEQTOTALCTRL) ? 0 : -1;
		sec->nextsec = -1;
		sec->prevsec = -1;
		sec->LastDamage = -1;
		// A Realms map carries no compiled behaviour, which is the condition the
		// Doom-format loader applies this under (maploader.cpp:1077): actors
		// standing on a fast-lowering floor ride it down instead of being left
		// hanging. Mid-platforms will need exactly that.
		sec->Flags |= SECF_FLOORDROP;
		sec->heightsec = nullptr;
		sec->damageinterval = 32;
		sec->terrainnum[sector_t::ceiling] = sec->terrainnum[sector_t::floor] = -1;
		sec->ibocount = -1;
		memset(sec->SpecialColors, -1, sizeof(sec->SpecialColors));
		memset(sec->AdditiveColors, 0, sizeof(sec->AdditiveColors));
		sec->gravity = 1.;
		sec->ZoneNumber = 0xFFFF;
		sec->friction = ORIG_FRICTION;
		sec->movefactor = ORIG_FRICTION_FACTOR;

		// A DOOR SECTOR KEEPS ITS REAL HEIGHT. It used to be clamped flat here so
		// doors would not "stand open", and that is backwards: the door sector is
		// the DOORWAY, and what closes it is the leaf built below. Flattening the
		// doorway deletes the passage instead of blocking it.
		//
		// ROTH.C is explicit that the height is real and is the leaf's own extent:
		// setup_door_swing_geometry puts `fs:[key] - fs:[key+2]` -- the sector's
		// ceilingHeight minus its floorHeight -- into every quad point's vertical
		// field (doors.c:545). Measured across all 44 maps: no door sector has
		// zero or negative height; almost all are 218 units tall.
		double floorZ = double(rs.floorHeight);
		double ceilZ = double(rs.ceilingHeight);
		if (rs.IsDoor())
		{
			doorCount++;
			if (rs.hingeFace >= 0) doorsWithHinge++;
		}

		sec->SetPlaneTexZ(sector_t::floor, floorZ);
		sec->SetPlaneTexZ(sector_t::ceiling, ceilZ);
		sec->floorplane.set(0., 0., 1., -floorZ);
		sec->ceilingplane.set(0., 0., -1., ceilZ);

		// Flat and readable while the real model is off: Doom's normal 160,
		// shifted by the sector's own offset from Realms' neutral 128 so the
		// relative light between rooms still reads.
		sec->lightlevel = (short)clamp<int>(160 + (int(rs.light) - 128), 0, 255);
		if (rs.light < darkestLight) darkestLight = rs.light;
		if (rs.light > brightestLight) brightestLight = rs.light;

		//------------------------------------------------------------------
		// Flats.
		//
		// SCALE IS PER SECTOR: 2^s world units per texel, s from the sector
		// flags byte -- bits 4-5 for the floor, 2-3 for the ceiling. Doom's
		// scale is the reciprocal, since one texel covers 1/scale world units.
		// s == 1 is the common case and reduces to the global "two world units
		// per texture pixel" rule; forcing that on every sector instead draws
		// every s == 2 surface at twice its intended density, which makes a rug
		// or a floor pattern tile where it should sit as a single piece.
		//
		// SHIFTS are in HALF-TEXEL steps, so a step is 2^s / 2 world units, and
		// the Y one is subtracted where the X one is added. Both confirmed in
		// ROTH.C: the shift bytes land in the HIGH byte of the texture-origin
		// words (renderer.c:8966, 8985, so x256) and are then shifted by 7 where
		// world coordinates are shifted by 16-s (renderer.c:3415-3416), which
		// makes one step 2^s/2 world units; and renderer.c:3172 adds the X term
		// but subtracts the Y one.
		//
		// FLATS ARE ANCHORED TO THE WORLD ORIGIN, not to anything sector-local.
		// Verified: the flat texture origin adds back the same camera
		// translation that world positions are built with (renderer.c:3415-3416
		// against 6768-6770), and nothing in that path reads a sector bounding
		// box. Doom flats are world-anchored by definition, so this needs no
		// translation -- and roth-editor's sector-corner anchoring is wrong.
		//
		// CAVEAT ON THE SCALE: ROTH.C's flat index also carries a per-texture
		// term derived from the texture width (a bsr-derived shift,
		// renderer.c:3375-3379), which this 2^s model ignores. The direction is
		// right and the selector bits are confirmed, but the exact law is not.
		//------------------------------------------------------------------
		for (int plane = 0; plane < 2; plane++)
		{
			const bool isFloor = (plane == 0);
			const int which = isFloor ? sector_t::floor : sector_t::ceiling;
			const int index = isFloor ? rs.floorTexture : rs.ceilingTexture;
			const int shift = isFloor ? rs.FloorScaleShift() : rs.CeilingScaleShift();
			const int shx = isFloor ? rs.floorShiftX : rs.ceilShiftX;
			const int shy = isFloor ? rs.floorShiftY : rs.ceilShiftY;

			FTextureID tex = worldTex(index);
			// A flat must draw SOMETHING or the sector renders hall of mirrors,
			// so "nothing here" becomes the sky -- which for a ceiling is also
			// what it means.
			sec->SetTexture(which, tex.isValid() ? tex : skyflatnum, false);
			if (!tex.isValid()) flatsToSky++;

			const double unitsPerTexel = double(1 << shift);
			sec->SetXScale(which, 1. / unitsPerTexel);
			sec->SetYScale(which, 1. / unitsPerTexel);
			sec->SetXOffset(which,  shx * unitsPerTexel * 0.5);
			sec->SetYOffset(which, -shy * unitsPerTexel * 0.5);
		}

		//------------------------------------------------------------------
		// Sector colormap. THIS IS NOT OPTIONAL: the memset above leaves
		// Colormap.LightColor as PalEntry(0,0,0), and a black light colour
		// multiplies every surface to black no matter what lightlevel says. The
		// level then renders pitch dark except where a dynamic light, which
		// brings its own colour, happens to reach -- which reads as "textures
		// did not load" when in fact they did.
		//
		// Straight from the Doom-format loader (maploader.cpp:1120-1131),
		// including the outside-fog branch, and deliberately placed AFTER the
		// flats above so the sky-ceiling test sees the real ceiling texture.
		//------------------------------------------------------------------
		sec->Colormap.LightColor = PalEntry(255, 255, 255);
		if (Level->outsidefog != 0xff000000
			&& (sec->GetTexture(sector_t::ceiling) == skyflatnum
				|| (sec->special & 0xff) == Sector_Outside))
		{
			sec->Colormap.FadeColor.SetRGB(Level->outsidefog);
		}
		else if (Level->flags & LEVEL_HASFADETABLE)
		{
			sec->Colormap.FadeColor = 0x939393;
		}
		else
		{
			sec->Colormap.FadeColor.SetRGB(Level->fadeto);
		}

		// The high byte of flags2 is recorded as carrying flat flip bits, but
		// that is UNCONFIRMED: reading ROTH.C found no flat-specific flip decode
		// at all, only a flip of the texture-quad corner assignment in the
		// separate driver used for 3D mesh faces. Counted either way, so if
		// flats do come out mirrored there is a number to reach for.
		if (rs.flags2 & 0xFF00) flatFlipsIgnored++;
	}
	log.Line("  doors closed at load  %d  (%d with a hinge resolved)", doorCount, doorsWithHinge);
	log.Count("doors: no hinge face found", doorCount - doorsWithHinge);
	log.Line("  flats with no art -> sky  %d", flatsToSky);
	// Realms' neutral is 0x80 = 128. A range hugging or exceeding 255 means the
	// mapping has gone wrong again and the level will look flat and overlit.
	log.Line("  sector light  %d .. %d  (Realms neutral is 128)", darkestLight, brightestLight);
	log.Count("flats: flip bits ignored", flatFlipsIgnored);

	//----------------------------------------------------------------------
	// Lines and sides
	//
	// Merging sister faces is the one structural translation that matters.
	//----------------------------------------------------------------------
	log.StageBegin("lines");

	std::vector<bool> consumed(rm.faces.size(), false);
	struct PendingLine { int face, sister; };
	std::vector<PendingLine> pending;
	pending.reserve(rm.faces.size());

	int twoSided = 0, oneSided = 0, orphaned = 0;
	for (size_t i = 0; i < rm.faces.size(); i++)
	{
		if (consumed[i]) continue;
		const roth::Face &f = rm.faces[i];

		if (f.sector < 0)
		{
			orphaned++;                 // counted, never silently dropped
			continue;
		}

		int sister = f.sister;
		if (sister >= 0 && sister < (int)rm.faces.size() && !consumed[sister]
			&& rm.faces[sister].sector >= 0)
		{
			consumed[sister] = true;
			pending.push_back({ (int)i, sister });
			twoSided++;
		}
		else
		{
			pending.push_back({ (int)i, -1 });
			oneSided++;
		}
		consumed[i] = true;
	}

	log.Line("  faces           %d", (int)rm.faces.size());
	log.Line("  -> lines        %d  (%d two-sided, %d one-sided)",
		(int)pending.size(), twoSided, oneSided);
	if (orphaned) log.Warn("%d faces belonged to no sector and were dropped", orphaned);
	log.Count("faces with no sector", orphaned);

	// The tails of both arrays are reserved for the 3D floor control geometry
	// built after this loop -- see the platform count above.
	Level->lines.Alloc(pending.size() + size_t(platformCount) * 4 + size_t(leafCount) * 8);
	Level->sides.Alloc(pending.size() + twoSided + size_t(platformCount) * 4
		+ size_t(leafCount) * 8);   // one per original face, plus the void geometry
	memset(&Level->lines[0], 0, sizeof(line_t) * Level->lines.Size());
	memset(&Level->sides[0], 0, sizeof(side_t) * Level->sides.Size());

	linemap.Clear();
	linemap.Reserve(pending.size());

	// Texturing counters. Everything the artwork path cannot account for is
	// counted rather than skipped in silence, so a gap can never hide inside a
	// "mostly worked".
	int noTextureMap = 0;        // a face pointing at no mapping record
	int midMissing = 0;          // a piece that wanted a mid texture and got none
	int storedExtentDeviates = 0;// stored extent differs from measured length
	int storedExtentUnusable = 0;// no usable stored extent; fell back to 0.5
	int imageFitFaces = 0;       // FF_IMAGE_FIT faces
	int imageFitVerticalUnhandled = 0; // PIECES, not faces: up to 3 per side
	int flippedFaces = 0;        // FF_FLIP_X, approximated by a negative scale
	int shiftedFaces = 0;        // a non-zero shiftX/shiftY was applied
	int edgeMapFaces = 0;        // FF_EDGE_MAP: the outdoor backdrop seen through windows
	int transUpLoFaces = 0;      // FF_TRANS_UPLO banding: not handled
	int extentBitsAbove12 = 0;   // see the note where this is reported

	unsigned sideIndex = 0;
	for (size_t li = 0; li < pending.size(); li++)
	{
		const roth::Face &f = rm.faces[pending[li].face];
		line_t *ld = &Level->lines[li];

		ld->v1 = &Level->vertexes[f.vertex1];
		ld->v2 = &Level->vertexes[f.vertex2];
		ld->alpha = 1.;
		ld->portalindex = UINT_MAX;
		ld->portaltransferred = UINT_MAX;
		ld->special = 0;
		ld->sidedef[0] = ld->sidedef[1] = nullptr;

		// The wall's measured length, which the horizontal texture rules need.
		const double wallLen = (ld->v2->fPos() - ld->v1->fPos()).Length();

		// Anchoring is per-face in Realms but per-LINE in Doom, so the sides
		// accumulate into these and the line takes the result.
		bool hasUpper = false, wantPegBottom = false;

		auto makeSide = [&](const roth::Face &face, bool twoSided, int neighbourSector) -> side_t *
		{
			side_t *sd = &Level->sides[sideIndex++];
			sd->sector = &Level->sectors[face.sector];
			sd->linedef = ld;
			sd->Flags = 0;
			sd->UDMFIndex = (int)(sd - &Level->sides[0]);
			// A zeroed side has zero texture scale, which renders nothing at
			// all. Every part needs an explicit 1.
			for (int part = 0; part < 3; part++)
			{
				sd->SetTextureXScale(part, 1.);
				sd->SetTextureYScale(part, 1.);
				sd->SetTextureXOffset(part, 0.);
				sd->SetTextureYOffset(part, 0.);
			}
			sd->ClearAlpha();

			const roth::TextureMap *tm =
				(face.textureMap >= 0 && face.textureMap < (int)rm.textureMaps.size())
					? &rm.textureMaps[face.textureMap] : nullptr;
			if (tm == nullptr)
			{
				noTextureMap++;
				return sd;
			}

			const uint8_t tf = tm->flags;

			//--------------------------------------------------------------
			// Which stored index goes in which Doom slot.
			//
			// A one-sided wall is one solid piece, so its mid texture is the
			// wall. A two-sided wall has upper and lower pieces filling the
			// height steps against its neighbour, and its mid piece is drawn
			// only when the face says to -- FF_TRANSPARENT marks the
			// see-through decal in the opening. Drawing the mid texture on
			// every two-sided wall would wall every doorway shut.
			//--------------------------------------------------------------
			const int midIndex = tm->midTexture;
			const bool wantMid = !twoSided || (tf & roth::FF_TRANSPARENT) != 0;

			//--------------------------------------------------------------
			// SCALE. Realms addresses wall art in HALF-texels: two world units
			// per texture pixel, where Doom uses one -- so the base scale is
			// 0.5 on both axes. FF_HALF_PIXEL drops that to one unit per texel.
			//
			// Horizontally the face's STORED EXTENT is authoritative and can
			// deliberately differ from the wall's measured length in order to
			// stretch or squash the art, so it is used rather than the geometry.
			// Doom expresses tiling as a scale, so with repeats = stored /
			// (unitsPerTexel * texWidth) and Doom's repeats = wallLen * scale /
			// texWidth, texWidth cancels and
			//
			//     scaleX = stored / (wallLen * unitsPerTexel)
			//
			// which reduces to exactly 0.5 when the stored extent equals the
			// true length. So this only deviates where a map author authored a
			// deviation.
			//
			// Verified in ROTH.C: renderer.c:4734 is the literal `>> 1` that
			// makes a texel two world units; renderer.c:13336 is FF_HALF_PIXEL
			// doubling both extents; renderer.c:13116 feeds the stored extent
			// straight into the u interpolator, and the wall's world length
			// never appears in that path at all.
			//
			// FF_IMAGE_FIT (renderer.c:8272, 13342-13359) takes the OTHER branch
			// entirely: the extents become 2 * texture_dimension, i.e. exactly
			// one copy, the shifts are zeroed, and FF_HALF_PIXEL is never read.
			// So half-pixel does not apply to a fitted face.
			//--------------------------------------------------------------
			const bool imageFit = (tf & roth::FF_IMAGE_FIT) != 0;
			const double unitsPerTexel = (!imageFit && (tf & roth::FF_HALF_PIXEL)) ? 1. : 2.;
			const double baseScale = 1. / unitsPerTexel;

			double scaleX = baseScale;
			const double stored = double(tm->StoredExtent());
			if (imageFit)
			{
				imageFitFaces++;
			}
			else if (stored > 0. && wallLen > 0.)
			{
				scaleX = stored / (wallLen * unitsPerTexel);
				if (fabs(stored - wallLen) > 1.) storedExtentDeviates++;
			}
			else
			{
				storedExtentUnusable++;
			}

			//--------------------------------------------------------------
			// SHIFTS exist only on an extended record, and FF_IMAGE_FIT zeroes
			// them (renderer.c:13343). Both are texel counts, which is what
			// Doom's sidedef offsets are too, so they carry across unchanged.
			//
			// shiftY's SIGN depends on the anchor: by default the piece is top
			// anchored and shiftY moves the art DOWN (renderer.c:5050), but
			// under FF_PIN_BOTTOM the piece is bottom anchored and shiftY is
			// SUBTRACTED instead (renderer.c:5061).
			//--------------------------------------------------------------
			double offX = 0., offY = 0.;
			if (tm->extended && !imageFit)
			{
				offX = double(tm->shiftX);
				offY = double(tm->shiftY);
				if (tf & roth::FF_PIN_BOTTOM) offY = -offY;
				if (offX != 0. || offY != 0.) shiftedFaces++;
			}

			// The height of each piece, so FF_IMAGE_FIT can put exactly one copy
			// down it. Doom resolves these at draw time, but WE ALREADY KNOW
			// BOTH SECTORS here -- we built them -- so every piece's extent is
			// computable at load and none of them has to be given up on.
			//
			// This used to bail on any two-sided wall, which left 903 of
			// STUDY1's pieces silently taking the default scale. That is most of
			// the level, and it is why some walls lined up and others did not:
			// the ones that happened to be one-sided were right.
			const sector_t *ownSec = &Level->sectors[face.sector];
			const double ownFloor = ownSec->GetPlaneTexZ(sector_t::floor);
			const double ownCeil  = ownSec->GetPlaneTexZ(sector_t::ceiling);
			double nbrFloor = 0., nbrCeil = 0.;
			const bool haveNbr = twoSided && neighbourSector >= 0
				&& neighbourSector < (int)rm.sectors.size();
			if (haveNbr)
			{
				const sector_t *ns = &Level->sectors[neighbourSector];
				nbrFloor = ns->GetPlaneTexZ(sector_t::floor);
				nbrCeil  = ns->GetPlaneTexZ(sector_t::ceiling);
			}
			// Which piece is which, in Doom's terms:
			//   upper  = my ceiling down to the neighbour's
			//   lower  = the neighbour's floor down to mine
			//   mid    = the opening between them (the whole sector if solid)
			auto heightOf = [&](int part) -> double
			{
				if (!haveNbr) return ownCeil - ownFloor;
				if (part == side_t::top)    return ownCeil - nbrCeil;
				if (part == side_t::bottom) return nbrFloor - ownFloor;
				return min(ownCeil, nbrCeil) - max(ownFloor, nbrFloor);
			};

			auto setPart = [&](int part, int storedIndex, bool masked)
			{
				FTextureID tex = worldTex(storedIndex, masked);
				if (!tex.isValid()) return false;   // nothing to draw here

				double sx = scaleX, sy = baseScale;
				if (imageFit)
				{
					// One copy across the piece: Doom covers texDim/scale world
					// units with one copy, so scale = texDim / pieceExtent.
					auto *gt = TexMan.GetGameTexture(tex, false);
					double texW = gt ? gt->GetDisplayWidth() : 0.;
					double texH = gt ? gt->GetDisplayHeight() : 0.;
					if (texW > 0. && wallLen > 0.) sx = texW / wallLen;
					const double pieceHeight = heightOf(part);
					if (texH > 0. && pieceHeight > 0.) sy = texH / pieceHeight;
					else if (texH > 0.) imageFitVerticalUnhandled++;
				}
				// FF_FLIP_X mirrors the art, which Doom expresses as a negative
				// horizontal scale. NOT EXACTLY THE SAME THING: ROTH.C mirrors
				// inside the texture's own texel range (renderer.c:4735 computes
				// H - u - 1) AFTER the shift is added, so where a piece is wider
				// than one copy of the art, or carries a shiftX, the two differ.
				// A negative scale is the closest Doom has.
				if (tf & roth::FF_FLIP_X) { sx = -sx; flippedFaces++; }

				sd->SetTexture(part, tex);
				sd->SetTextureXScale(part, sx);
				sd->SetTextureYScale(part, sy);
				sd->SetTextureXOffset(part, offX);
				sd->SetTextureYOffset(part, offY);
				return true;
			};

			// The mid piece of a two-sided wall is the see-through decal in the
			// opening, so it is the one place a hole is definitely wanted.
			if (wantMid && !setPart(side_t::mid, midIndex, twoSided)) midMissing++;
			if (twoSided)
			{
				if (setPart(side_t::top, tm->upperTexture, false)) hasUpper = true;
				setPart(side_t::bottom, tm->lowerTexture, false);
			}

			//--------------------------------------------------------------
			// ANCHORING. Realms hangs a wall piece's art from the TOP of that
			// piece. Doom's upper texture instead aligns its BOTTOM to the
			// lower ceiling, so an upper piece needs ML_DONTPEGTOP to be top
			// anchored. FF_PIN_BOTTOM asks for bottom anchoring, which is
			// ML_DONTPEGBOTTOM.
			//
			// These are LINE flags in Doom but PER-FACE state in Realms, so a
			// sister pair that disagrees cannot be represented at all. They are
			// OR-ed, which is the permissive choice.
			//
			// NOT VERIFIED: ROTH.C's face walk gives every wall piece its own
			// mapping record and shows no separate upper/lower anchoring rule,
			// so the upper-piece case below is reasoned from Doom's defaults plus
			// the verified Realms "anchor at the top" rule, not read out of the
			// original. This is the least-evidenced part of the stage.
			//--------------------------------------------------------------
			if (tf & roth::FF_PIN_BOTTOM) wantPegBottom = true;

			// FF_EDGE_MAP is the outdoor backdrop: the original sets
			// g_parallax_sky_active from this bit (renderer.c:9020) and hands the
			// face to render_parallax_sky_columns, which fills it from the map's
			// sky block and drifts it with the view angle. These are the manor's
			// windows -- in the original you see trees and moonlight through them.
			//
			// We give the face the sky image so it shows the right picture.
			// The DRIFT IS NOT IMPLEMENTED: this is a flat wall of the backdrop,
			// not a parallax layer, so it will not slide as you turn. Counted
			// separately from the faces we do nothing at all with, so the report
			// does not claim more than was done.
			// FF_EDGE_MAP marks a face that can have the outdoor backdrop ABOVE
			// it -- and "above it" is the whole point.
			//
			// ROTH.C sets g_parallax_sky_active from this bit
			// (renderer.c:9020, off the mapping record's flags byte at +8) and
			// then render_parallax_sky_columns fills the open region ABOVE THE
			// WALL TOP, and only where that top is above the horizon. The face
			// still draws its own texture underneath. The sky is what is left
			// over above a wall that does not reach the top of the view.
			//
			// We drew the whole face as sky instead, which is why clouds turned
			// up in hallways and the outside looked wrong: every flagged wall
			// became a sky, including interior ones. Reverted.
			//
			// Doing it properly needs the wall's own top height, which Realms
			// stores per face and our loader does not model yet -- we build every
			// wall floor-to-ceiling, so there IS no region above it to fill. That
			// is the real work, and it is geometry, not texturing.
			if (tf & roth::FF_EDGE_MAP) edgeMapFaces++;
			if (tf & roth::FF_TRANS_UPLO) transUpLoFaces++;
			return sd;
		};

		ld->sidedef[0] = makeSide(f, pending[li].sister >= 0,
			pending[li].sister >= 0 ? rm.faces[pending[li].sister].sector : -1);
		if (pending[li].sister >= 0)
		{
			ld->sidedef[1] = makeSide(rm.faces[pending[li].sister], true, f.sector);
			ld->flags |= ML_TWOSIDED;
		}
		else
		{
			ld->flags |= ML_BLOCKING;
		}

		if (hasUpper) ld->flags |= ML_DONTPEGTOP;
		if (wantPegBottom) ld->flags |= ML_DONTPEGBOTTOM;

		linemap.Push((unsigned)li);
		ld->AdjustLine();
		FinishLoadingLineDef(ld, 255);
	}

	//----------------------------------------------------------------------
	// Mid-platforms -> 3D floors
	//
	// A Realms sector may carry one intermediate floor: a slab with a top you
	// stand on and an underside you see from below, each with its own texture
	// and scale. 16 of them in STUDY1, 2,299 across the 44 maps -- TOWER1 alone
	// has 338. They are tables, shelves, ledges and balconies, and without them
	// a table is four legs and no top.
	//
	// GZDoom says the same thing with a 3D floor, which is described by a
	// CONTROL SECTOR sitting outside the map: its floor plane becomes the slab's
	// underside, its ceiling plane the top, and their textures come with them.
	// The control linedef is not optional -- the renderer reads the slab's SIDE
	// texture straight off master->sidedef[0] with no null check
	// (hw_walls.cpp:2032), so a 3D floor without one crashes.
	//
	// Realms has no side texture for a slab, so the top's art is used for it:
	// a tabletop's edge is the same wood as its surface.
	//----------------------------------------------------------------------
	int platformsBuilt = 0, platformsSkipped = 0;
	if (platformCount > 0)
	{
		log.StageBegin("mid-platforms");

		// Park the control sectors clear of the real map. They are unreachable
		// geometry, but they still go through the node builder and the blockmap,
		// so they must not overlap anything -- an overlapping control sector
		// would carve the BSP of the room it landed in.
		double voidX = 0., voidY = 0.;
		for (const auto &v : rm.vertices)
		{
			if (double(v.x) < voidX) voidX = double(v.x);
			if (double(v.y) < voidY) voidY = double(v.y);
		}
		voidX -= 4096.;
		voidY -= 4096.;

		unsigned ctrlSector = (unsigned)rm.sectors.size();
		unsigned ctrlVertex = (unsigned)rm.vertices.size();
		unsigned ctrlLine   = (unsigned)pending.size();

		for (size_t i = 0; i < rm.sectors.size(); i++)
		{
			const roth::Sector &rs = rm.sectors[i];
			if (rs.platformIndex < 0 || rs.platformIndex >= (int)rm.platforms.size())
				continue;

			const roth::MidPlatform &mp = rm.platforms[rs.platformIndex];
			const double topZ  = double(mp.topZ);
			const double undZ  = double(mp.undersideZ);

			// A slab with no thickness has nothing to render and would give the
			// renderer a zero-height volume. Counted rather than dropped quietly.
			if (topZ <= undZ)
			{
				platformsSkipped++;
				continue;
			}

			sector_t *cs = &Level->sectors[ctrlSector];
			cs->Level = Level;
			cs->e = &Level->extsectors[ctrlSector];
			cs->sectornum = (int)ctrlSector;
			cs->SetXScale(sector_t::floor, 1.);   cs->SetYScale(sector_t::floor, 1.);
			cs->SetXScale(sector_t::ceiling, 1.); cs->SetYScale(sector_t::ceiling, 1.);
			cs->SetAlpha(sector_t::floor, 1.);    cs->SetAlpha(sector_t::ceiling, 1.);
			cs->seqType = -1;
			cs->nextsec = cs->prevsec = -1;
			cs->heightsec = nullptr;
			cs->damageinterval = 32;
			cs->terrainnum[sector_t::ceiling] = cs->terrainnum[sector_t::floor] = -1;
			cs->ibocount = -1;
			memset(cs->SpecialColors, -1, sizeof(cs->SpecialColors));
			memset(cs->AdditiveColors, 0, sizeof(cs->AdditiveColors));
			cs->gravity = 1.;
			cs->ZoneNumber = 0xFFFF;
			cs->friction = ORIG_FRICTION;
			cs->movefactor = ORIG_FRICTION_FACTOR;
			cs->Colormap.LightColor = PalEntry(255, 255, 255);
			cs->Colormap.FadeColor.SetRGB(Level->fadeto);
			// The slab is lit like the room it sits in, not like the void.
			cs->lightlevel = Level->sectors[i].lightlevel;

			// Control floor = the slab's UNDERSIDE, control ceiling = its TOP.
			cs->SetPlaneTexZ(sector_t::floor, undZ);
			cs->SetPlaneTexZ(sector_t::ceiling, topZ);
			cs->floorplane.set(0., 0., 1., -undZ);
			cs->ceilingplane.set(0., 0., -1., topZ);

			FTextureID topTex = worldTex(mp.topTexture);
			FTextureID undTex = worldTex(mp.undersideTexture);
			// NEVER the sky flat here. A control sector's planes become the
			// slab's top and underside, and a sky flat there renders as sky --
			// indoors, inside a table. Missing art borrows the other face's,
			// and a slab with neither draws nothing at all.
			const FTextureID slabTop = topTex.isValid() ? topTex : undTex;
			const FTextureID slabBot = undTex.isValid() ? undTex : topTex;
			cs->SetTexture(sector_t::ceiling, slabTop.isValid() ? slabTop : FNullTextureID(), false);
			cs->SetTexture(sector_t::floor, slabBot.isValid() ? slabBot : FNullTextureID(), false);

			// Scale, exactly as for an ordinary flat: 2^s world units per texel,
			// and Doom's scale is the reciprocal. Bits 4-5 top, 2-3 underside,
			// the same layout the sector flags byte uses.
			const double topScale = 1.0 / double(1 << ((mp.scales >> 4) & 3));
			const double undScale = 1.0 / double(1 << ((mp.scales >> 2) & 3));
			cs->SetXScale(sector_t::ceiling, topScale); cs->SetYScale(sector_t::ceiling, topScale);
			cs->SetXScale(sector_t::floor, undScale);   cs->SetYScale(sector_t::floor, undScale);
			cs->SetXOffset(sector_t::ceiling, double(mp.topShiftX));
			cs->SetYOffset(sector_t::ceiling, double(mp.topShiftY));
			cs->SetXOffset(sector_t::floor, double(mp.undersideShiftX));
			cs->SetYOffset(sector_t::floor, double(mp.undersideShiftY));

			// A CLOSED square, parked in the void well outside the map. It has to
			// be closed: the node builder walks every line, and a lone degenerate
			// one produces "right edge is unconnected" for each and leaves the BSP
			// wrong. Real maps put their control sectors in a sealed void room for
			// exactly this reason; this builds the same thing rather than faking it.
			//
			// Nothing joins these to the level, so they are unreachable and never
			// drawn -- they exist only to give the 3D floor a sector to copy its
			// planes from and a sidedef to take its edge texture from.
			const double cx = voidX + double(platformsBuilt % 64) * 96.;
			const double cy = voidY - double(platformsBuilt / 64) * 96.;
			vertex_t *vv[4] = {
				&Level->vertexes[ctrlVertex + 0], &Level->vertexes[ctrlVertex + 1],
				&Level->vertexes[ctrlVertex + 2], &Level->vertexes[ctrlVertex + 3] };
			vv[0]->set(cx,       cy);
			vv[1]->set(cx + 64., cy);
			vv[2]->set(cx + 64., cy + 64.);
			vv[3]->set(cx,       cy + 64.);

			for (int e = 0; e < 4; e++)
			{
				side_t *cd = &Level->sides[sideIndex];
				cd->sector = cs;
				cd->SetTexture(side_t::mid, topTex.isValid() ? topTex : FNullTextureID());
				cd->SetTextureXScale(side_t::mid, 1.);
				cd->SetTextureYScale(side_t::mid, 1.);
				cd->SetTextureXOffset(side_t::mid, 0.);
				cd->SetTextureYOffset(side_t::mid, 0.);
				cd->Flags = 0;

				line_t *cl = &Level->lines[ctrlLine + e];
				cl->v1 = vv[e];
				cl->v2 = vv[(e + 1) & 3];
				cl->sidedef[0] = cd;
				cl->sidedef[1] = nullptr;
				cl->frontsector = cs;
				cl->backsector = nullptr;
				cl->flags = ML_BLOCKING;
				cl->special = 0;
				cl->alpha = 1.;
				cd->linedef = cl;
				cl->AdjustLine();
				sideIndex++;
			}

			// The first edge is the master: the renderer reads the slab's side
			// texture off master->sidedef[0] with no null check (hw_walls.cpp:2032).
			P_Add3DFloor(&Level->sectors[i], cs, &Level->lines[ctrlLine],
				FF_EXISTS | FF_SOLID | FF_RENDERALL, 255);

			ctrlSector++; ctrlVertex += 4; ctrlLine += 4;
			platformsBuilt++;
		}

		log.Line("  platforms built %d  (%d skipped as zero thickness)",
			platformsBuilt, platformsSkipped);
		log.Count("mid-platforms: zero thickness, skipped", platformsSkipped);
	}

	//----------------------------------------------------------------------
	// Door leaves -> polyobjects
	//
	// See the census above for why the leaf is separate geometry rather than
	// the door sector's own walls, and why it is built in the void.
	//
	// WHAT ROTH.C DOES, and what each piece of this maps to:
	//
	//  * The leaf is a four-point quad stored HINGE-RELATIVE
	//    (setup_door_swing_geometry, doors.c:548-556: each point is
	//    `gs:[vertex+8] - record[0x14]`, `gs:[vertex+0xa] - record[0x16]`,
	//    where record[0x14]/[0x16] are the hinge vertex's x,y).
	//  * rotate_quad (renderer.c:1557) rotates those offsets and adds the
	//    hinge back. At angle 0 it is the identity, so the authored position
	//    IS the closed position. GZDoom's PODOOR_SWING rotates every polyobject
	//    vertex about StartSpot -- the same operation about the same point.
	//  * The angle is a BYTE over a 256-step turn: rotate_quad indexes a
	//    512-entry sine table with `(-2*angle) & 0x1ff`, so one step is
	//    360/256 degrees. tick_swinging_doors clamps the open limit to
	//    +0x40 / -0x40 (doors.c:1084, 1117), i.e. EXACTLY 90 DEGREES. There is
	//    no variable swing extent; see the command-argument note below.
	//  * The four faces are ordered CYCLICALLY FROM THE HINGE (the rol/ror of
	//    the packed offset table 0x24180c00 over the 0xc-byte face stride), and
	//    the hinge face is a THICKNESS edge: the two faces adjacent to it are
	//    the long room-facing ones. Measured over all 44 maps: on 141 of 141
	//    doors those two adjacent faces have two DIFFERENT far sectors, which is
	//    exactly the pair ROTH.C derives as the rooms either side.
	//
	// The surface texture is the face's MID texture: door_corner_tail
	// (doors.c:271) sets the leaf surface's `[out+0xc] = fs:[textureMap+2]`,
	// which is TextureMap::midTexture, unconditionally -- not the upper/lower
	// step pieces a two-sided wall would use.
	//----------------------------------------------------------------------
	struct PolySpot { double sx, sy, ax, ay; int tag; };
	std::vector<PolySpot> polySpots;
	int leavesBuilt = 0, leavesNoTexture = 0, doorCapableNotBuilt = 0;
	for (const auto &rs : rm.sectors)
		if (!rs.IsDoor() && rs.IsDoorCapable()) doorCapableNotBuilt++;

	if (leafCount > 0)
	{
		log.StageBegin("door leaves");
		polySpots.reserve(size_t(leafCount) * 2);

		// The same void-parking rule the 3D-floor control sectors use, on its own
		// band so the two can never overlap. These are OUR bookkeeping numbers for
		// scratch space, not anything read out of Realms: a cell wide enough for the
		// longest leaf measured in the retail data (128 long by 64 thick) with room
		// for the sealing loop around it.
		const double CELL = 512.;
		double voidX = 0., voidY = 0.;
		for (const auto &v : rm.vertices)
		{
			if (double(v.x) < voidX) voidX = double(v.x);
			if (double(v.y) < voidY) voidY = double(v.y);
		}
		voidX -= 4096.;
		voidY -= 4096. + 4096.;   // clear of the platform band above

		unsigned vSector = (unsigned)rm.sectors.size() + (unsigned)platformsBuilt;
		unsigned vVertex = (unsigned)rm.vertices.size() + (unsigned)platformsBuilt * 4;
		unsigned vLine   = (unsigned)pending.size() + (unsigned)platformsBuilt * 4;

		for (size_t i = 0; i < rm.sectors.size(); i++)
		{
			const roth::Sector &rs = rm.sectors[i];
			if (!leafBuildable(rs)) continue;

			const int f0 = rs.firstFaceIndex;
			const int hingeRel = rs.hingeFace - f0;

			// THE HINGE POINT. ROTH.C takes the dword at the hinge face -- its two
			// vertex offsets packed together -- and uses the LOW half, i.e.
			// vertex1, unless the swing is mirrored, in which case `rol ebx,16`
			// selects vertex2 (doors.c:520, 531, 540).
			//
			// UNVERIFIED / KNOWN DIVERGENCE: that mirror choice is made PER
			// ACTIVATION, from which room the player used the door -- the door
			// swings away from whoever opened it. A GZDoom polyobject has one
			// fixed StartSpot, so we pin the hinge to vertex1 (which is also the
			// unconditional choice when the hinge face's faceID is 0xFFFE). A door
			// opened from the mirrored side will therefore pivot about the far end
			// of the hinge edge, a difference of the slab's thickness (8 to 64
			// units in the retail data). Stage 7 can close this properly: it knows
			// which side was used, and it can set po->StartSpot before calling
			// EV_OpenPolyDoor.
			const roth::Face &hf = rm.faces[rs.hingeFace];
			const double hingeX = double(rm.vertices[hf.vertex1].x);
			const double hingeY = double(rm.vertices[hf.vertex1].y);

			// Park this leaf's cell and put its hinge at the cell's centre, so the
			// anchor/spawn delta is exactly the cell offset and nothing else.
			const double ox = voidX + double(leavesBuilt % 32) * CELL;
			const double oy = voidY - double(leavesBuilt / 32) * CELL;
			const double cxc = ox + CELL * 0.5, cyc = oy + CELL * 0.5;
			const double offX = cxc - hingeX, offY = cyc - hingeY;

			//--------------------------------------------------------------
			// The void room: one sector, lit and sized like the doorway it
			// belongs to, so the leaf renders at the right height and
			// brightness once it is translated into place. A one-sided
			// polyobject wall spans its own sidedef sector's floor to ceiling.
			//--------------------------------------------------------------
			sector_t *vs = &Level->sectors[vSector];
			const sector_t *door = &Level->sectors[i];
			vs->Level = Level;
			vs->e = &Level->extsectors[vSector];
			vs->sectornum = (int)vSector;
			vs->SetXScale(sector_t::floor, 1.);   vs->SetYScale(sector_t::floor, 1.);
			vs->SetXScale(sector_t::ceiling, 1.); vs->SetYScale(sector_t::ceiling, 1.);
			vs->SetAlpha(sector_t::floor, 1.);    vs->SetAlpha(sector_t::ceiling, 1.);
			vs->seqType = -1;
			vs->nextsec = vs->prevsec = -1;
			vs->LastDamage = -1;
			vs->heightsec = nullptr;
			vs->damageinterval = 32;
			vs->terrainnum[sector_t::ceiling] = vs->terrainnum[sector_t::floor] = -1;
			vs->ibocount = -1;
			memset(vs->SpecialColors, -1, sizeof(vs->SpecialColors));
			memset(vs->AdditiveColors, 0, sizeof(vs->AdditiveColors));
			vs->gravity = 1.;
			vs->ZoneNumber = 0xFFFF;
			vs->friction = ORIG_FRICTION;
			vs->movefactor = ORIG_FRICTION_FACTOR;
			vs->Colormap.LightColor = PalEntry(255, 255, 255);
			vs->Colormap.FadeColor.SetRGB(Level->fadeto);
			vs->lightlevel = door->lightlevel;
			// The doorway's own heights, so the leaf is exactly as tall as the
			// opening it fills.
			const double lfZ = door->GetPlaneTexZ(sector_t::floor);
			const double lcZ = door->GetPlaneTexZ(sector_t::ceiling);
			vs->SetPlaneTexZ(sector_t::floor, lfZ);
			vs->SetPlaneTexZ(sector_t::ceiling, lcZ);
			vs->floorplane.set(0., 0., 1., -lfZ);
			vs->ceilingplane.set(0., 0., -1., lcZ);
			// NEVER the sky flat in the void: the same trap the 3D-floor control
			// sectors document. Borrow the doorway's own flats; they are never seen.
			vs->SetTexture(sector_t::floor, door->GetTexture(sector_t::floor), false);
			vs->SetTexture(sector_t::ceiling, door->GetTexture(sector_t::ceiling), false);

			const int tag = 1 + leavesBuilt;

			//--------------------------------------------------------------
			// The leaf: four ONE-SIDED lines, fronts facing OUT of the slab.
			// Realms winds a face v1->v2 with its owning sector on the right,
			// which is Doom's own convention and is what the wall loop above
			// already relies on. The door sector owns these faces, so it is on
			// the right -- and the leaf is solid, seen from outside. Reversing
			// each line puts the front where it has to be.
			//--------------------------------------------------------------
			for (int j = 0; j < 4; j++)
			{
				// Cyclically from the hinge, matching ROTH.C's corner ordering.
				const int fi = f0 + ((hingeRel + j) & 3);
				const roth::Face &f = rm.faces[fi];

				vertex_t *va = &Level->vertexes[vVertex + (unsigned)j];
				// Reversed: this line runs face.vertex2 -> face.vertex1.
				va->set(double(rm.vertices[f.vertex2].x) + offX,
				        double(rm.vertices[f.vertex2].y) + offY);
			}
			for (int j = 0; j < 4; j++)
			{
				const int fi = f0 + ((hingeRel + j) & 3);
				const roth::Face &f = rm.faces[fi];

				line_t *ld = &Level->lines[vLine + (unsigned)j];
				side_t *sd = &Level->sides[sideIndex++];

				ld->v1 = &Level->vertexes[vVertex + (unsigned)j];
				ld->v2 = &Level->vertexes[vVertex + (unsigned)((j + 1) & 3)];
				ld->alpha = 1.;
				ld->portalindex = UINT_MAX;
				ld->portaltransferred = UINT_MAX;
				ld->flags = ML_BLOCKING;
				ld->sidedef[0] = sd;
				ld->sidedef[1] = nullptr;
				ld->frontsector = vs;
				ld->backsector = nullptr;
				// Polyobj_ExplicitLine states the lines and their order outright,
				// which suits generated geometry far better than relying on the
				// traversal PO_LINE_START needs. args[1] is the order and MUST be
				// nonzero -- SpawnPolyobj rejects the poly otherwise
				// (polyobjects.cpp:224).
				ld->special = Polyobj_ExplicitLine;
				ld->args[0] = tag;
				ld->args[1] = j + 1;
				ld->args[2] = 0;    // no mirror
				ld->args[3] = 0;    // no sound sequence

				sd->sector = vs;
				sd->linedef = ld;
				sd->Flags = 0;
				sd->UDMFIndex = (int)(sd - &Level->sides[0]);
				for (int part = 0; part < 3; part++)
				{
					sd->SetTextureXScale(part, 1.);
					sd->SetTextureYScale(part, 1.);
					sd->SetTextureXOffset(part, 0.);
					sd->SetTextureYOffset(part, 0.);
				}
				sd->ClearAlpha();

				//------------------------------------------------------
				// The skin. A Realms face is drawn from its OWNING sector's
				// side -- that is the rule the whole wall loop above is built
				// on. This leaf line faces a ROOM, so the picture the player
				// sees on it is the one carried by the face on the room's
				// side: the SISTER. The slab's own face looks the other way,
				// into the doorway.
				//
				// ROTH.C gives the leaf four surfaces, not two, and does
				// exactly this pairing: setup_door_swing_geometry fills two
				// from the slab's own faces (_a, via fs:[face+4]) and two from
				// their sisters (_b, via fs:[fs:[face+8]+4]) -- doors.c:538-545.
				// A one-sided Doom line has one skin, so it takes the
				// outward-facing one.
				//
				// UNVERIFIED: that the _b (sister) surface is the outward one
				// rather than the _a. rotate_quad only produces positions; the
				// surface-to-side association lives in the renderer and was not
				// traced. The owning-sector rule above is what decides it here.
				int skinFace = fi;
				if (f.sister >= 0 && f.sister < (int)rm.faces.size()) skinFace = f.sister;
				const roth::Face &sf = rm.faces[skinFace];
				const roth::TextureMap *tm =
					(sf.textureMap >= 0 && sf.textureMap < (int)rm.textureMaps.size())
						? &rm.textureMaps[sf.textureMap] : nullptr;

				FTextureID tex = tm ? worldTex(tm->midTexture) : FNullTextureID();
				if (!tex.isValid())
				{
					leavesNoTexture++;
				}
				else
				{
					// The same scale law as an ordinary wall: two world units per
					// texture pixel unless FF_HALF_PIXEL, and the stored extent is
					// authoritative horizontally.
					const double unitsPerTexel =
						(tm->flags & roth::FF_HALF_PIXEL) ? 1. : 2.;
					const double len = (ld->v2->fPos() - ld->v1->fPos()).Length();
					const double stored = double(tm->StoredExtent());
					double sx = 1. / unitsPerTexel;
					if (stored > 0. && len > 0.) sx = stored / (len * unitsPerTexel);
					sd->SetTexture(side_t::mid, tex);
					sd->SetTextureXScale(side_t::mid, sx);
					sd->SetTextureYScale(side_t::mid, 1. / unitsPerTexel);
				}

				ld->AdjustLine();
			}

			//--------------------------------------------------------------
			// The seal: four one-sided lines round the cell, fronts facing IN,
			// so the void room is a closed convex container for the leaf. A
			// polyobject's origin subsector is discarded by the renderer
			// (SSECF_POLYORG), which is precisely why it has to be in here and
			// not in the map.
			//--------------------------------------------------------------
			{
				vertex_t *rv[4] = {
					&Level->vertexes[vVertex + 4], &Level->vertexes[vVertex + 5],
					&Level->vertexes[vVertex + 6], &Level->vertexes[vVertex + 7] };
				// Wound so the room's interior is on the right of v1->v2.
				rv[0]->set(ox,        oy);
				rv[1]->set(ox,        oy + CELL);
				rv[2]->set(ox + CELL, oy + CELL);
				rv[3]->set(ox + CELL, oy);

				for (int e = 0; e < 4; e++)
				{
					line_t *ld = &Level->lines[vLine + 4 + (unsigned)e];
					side_t *sd = &Level->sides[sideIndex++];

					ld->v1 = rv[e];
					ld->v2 = rv[(e + 1) & 3];
					ld->alpha = 1.;
					ld->portalindex = UINT_MAX;
					ld->portaltransferred = UINT_MAX;
					ld->flags = ML_BLOCKING;
					ld->special = 0;
					ld->sidedef[0] = sd;
					ld->sidedef[1] = nullptr;
					ld->frontsector = vs;
					ld->backsector = nullptr;

					sd->sector = vs;
					sd->linedef = ld;
					sd->Flags = 0;
					sd->UDMFIndex = (int)(sd - &Level->sides[0]);
					for (int part = 0; part < 3; part++)
					{
						sd->SetTextureXScale(part, 1.);
						sd->SetTextureYScale(part, 1.);
						sd->SetTextureXOffset(part, 0.);
						sd->SetTextureYOffset(part, 0.);
					}
					sd->ClearAlpha();
					// Untextured on purpose: nothing can ever see these, and giving
					// them the sky flat's equivalent would be the trap the 3D-floor
					// control sectors document.
					ld->AdjustLine();
				}
			}

			// The spawn spot goes where the door really is; the anchor goes on the
			// SAME point of the leaf as built, i.e. offset by the cell delta.
			// TranslateToStartSpot moves every vertex by (anchor - spawn), which is
			// exactly that delta, so the leaf lands on its authored position with
			// its hinge on StartSpot -- and PODOOR_SWING then rotates about the
			// hinge, which is what ROTH.C does.
			polySpots.push_back({ hingeX, hingeY, hingeX + offX, hingeY + offY, tag });

			// Tag the DOORWAY sector with the same number so stage 7 can get from
			// the sector the player used to the polyobject that fills it. A tag is
			// the engine's own general handle for this; nothing Realms-specific.
			Level->tagManager.AddSectorTag((int)i, tag);

			vSector++; vVertex += 8; vLine += 8;
			leavesBuilt++;
		}

		// The census and the loop above use the SAME predicate, so these must
		// agree. If they ever do not, the tails of the line and vertex arrays are
		// left zeroed and the node builder will report unconnected edges -- so say
		// so loudly rather than let it look like a geometry bug.
		if (leavesBuilt != leafCount)
			log.Warn("door leaf census %d but built %d -- zeroed lines left in the array",
				leafCount, leavesBuilt);

		log.Line("  door leaves built %d of %d door sectors", leavesBuilt, doorCount);
		log.Line("  polyobject tags   1 .. %d  (the doorway sector carries the same tag)", leavesBuilt);
		log.Count("doors: leaf surface had no artwork", leavesNoTexture);
		log.Count("doors: 0xFFFE door-capable sectors, no leaf built (OPEN QUESTION)",
			doorCapableNotBuilt);
	}

	if (sideIndex < Level->sides.Size())
		Level->sides.Resize(sideIndex);

	//----------------------------------------------------------------------
	// What the artwork path did and did not account for.
	//
	// A SETTLED DISAGREEMENT, kept as a counter because it is cheap: the Python
	// pipeline read an extended record's extent as 15 bits, everything but the
	// extended flag. ROTH.C masks it to 12 (renderer.c:9015,
	// `ES16(newbx) & 0xfff`), which is what roth_raw's StoredExtent() does, so
	// the Python was wrong. This counts the records where that error would have
	// shown, i.e. how much the mistake was worth.
	//----------------------------------------------------------------------
	for (auto &tmr : rm.textureMaps)
		if (tmr.extended && (tmr.fitWord & 0x7000)) extentBitsAbove12++;

	log.Section("Texturing");
	log.Line("  registered images     %d  (%d animated, %d solid colour)",
		art.Registered(), art.Animated(), art.SolidColours());
	log.Line("  stored extent used    %d deviate from measured length, %d unusable",
		storedExtentDeviates, storedExtentUnusable);
	log.Line("  image fit             %d faces; %d PIECES got no vertical fit",
		imageFitFaces, imageFitVerticalUnhandled);
	log.Line("  x-flipped             %d pieces", flippedFaces);
	log.Line("  shifts applied        %d faces", shiftedFaces);
	log.Line("  records the Python's 15-bit extent would have misread  %d",
		extentBitsAbove12);
	log.Count("walls: no texture-map record", noTextureMap);
	log.Count("walls: mid texture wanted but absent", midMissing);
	log.Count("walls: FF_IMAGE_FIT vertical fit not expressible", imageFitVerticalUnhandled);
	log.Count("walls: FF_EDGE_MAP faces -- sky above the wall top not modelled", edgeMapFaces);
	log.Count("walls: FF_TRANS_UPLO banding not handled", transUpLoFaces);
	log.Count("artwork: images that failed to decode", art.Failed());
	log.Count("artwork: stored indices out of every known range", art.OutOfRange());

	//----------------------------------------------------------------------
	// Things
	//
	// Object Z is absolute; the engine wants a height above the floor.
	//----------------------------------------------------------------------
	log.StageBegin("things");
	MapThingsConverted.Clear();

	// Player start. Realms measures the player's facing in 512 units per turn
	// counter-clockwise from +Y, where Doom uses degrees counter-clockwise from
	// +X -- hence the quarter turn. Objects use a DIFFERENT convention; see
	// below. Getting these two mixed up points everything the wrong way.
	{
		FMapThing mt = {};
		mt.pos.X = rm.metadata.startX;
		mt.pos.Y = rm.metadata.startY;
		mt.pos.Z = 0;
		mt.angle = (int)(90 + rm.metadata.rotation * 360.0 / 512.0) % 360;
		mt.EdNum = 1;
		mt.info = DoomEdMap.CheckKey(mt.EdNum);
		mt.flags = MTF_SINGLE | MTF_COOPERATIVE | MTF_DEATHMATCH;
		mt.SkillFilter = 0xffff;
		mt.ClassFilter = 0xffff;
		mt.Gravity = 1;
		mt.RenderStyle = STYLE_Count;
		mt.Alpha = -1;
		mt.Health = 1;
		mt.FloatbobPhase = -1;
		MapThingsConverted.Push(mt);
	}

	//----------------------------------------------------------------------
	// Polyobject spawn spots and anchors for the door leaves.
	//
	// These are not actors -- PO_Init and GetPolySpots pick them out of
	// MapThingsConverted by their editor entry's Special (9301 PolySpawn, 9300
	// PolyAnchor, both in mapinfo/common.txt so every game has them), and read
	// the TAG out of the thing's ANGLE field. They have to be pushed here
	// because the Things stage clears the array.
	//
	// GetPolySpots also hands both to the node builder, which uses the pair to
	// work out where the leaf will end up and protect the loop of segs around
	// its origin from being split.
	//----------------------------------------------------------------------
	for (const auto &ps : polySpots)
	{
		for (int which = 0; which < 2; which++)
		{
			FMapThing mt = {};
			mt.pos.X = which ? ps.ax : ps.sx;
			mt.pos.Y = which ? ps.ay : ps.sy;
			mt.pos.Z = 0;
			mt.angle = ps.tag;          // the polyobject number, not a facing
			mt.EdNum = which ? 9300 : 9301;   // PolyAnchor : PolySpawn
			mt.info = DoomEdMap.CheckKey(mt.EdNum);
			mt.flags = MTF_SINGLE | MTF_COOPERATIVE | MTF_DEATHMATCH;
			mt.SkillFilter = 0xffff;
			mt.ClassFilter = 0xffff;
			mt.Gravity = 1;
			mt.RenderStyle = STYLE_Count;
			mt.Alpha = -1;
			mt.Health = 1;
			mt.FloatbobPhase = -1;
			if (mt.info == nullptr)
				log.Count("doors: no editor entry for the polyobject spot -- leaf will not spawn");
			MapThingsConverted.Push(mt);
		}
	}
	if (!polySpots.empty())
		log.Line("  polyobject spots %d spawn + %d anchor",
			(int)polySpots.size(), (int)polySpots.size());

	log.Line("  player start    (%d, %d) facing %d",
		rm.metadata.startX, rm.metadata.startY,
		(int)(90 + rm.metadata.rotation * 360.0 / 512.0) % 360);
	// Objects. Everything about them -- which artwork, which draw mode, the
	// meshes, the sprite definitions -- is decided and built here; the actors
	// themselves cannot be spawned until the level has a BSP and a blockmap, so
	// that half runs from MapLoader::LoadLevel after SpawnThings. See
	// roth_objects.h.
	roth::PrepareObjects(rm, art, &log);

	log.StageEnd();
	log.Section("Not yet handled");
	// The leaves exist and are closed. NOTHING OPENS THEM yet -- that is the
	// command system, stage 7. When it does, the numbers it needs are these,
	// all read out of ROTH.C rather than guessed:
	//
	//   EV_OpenPolyDoor(Level, nullptr, tag, speed, DAngle::fromDeg(90), delay,
	//                   0, PODOOR_SWING)
	//
	//   * 90 DEGREES, always. tick_swinging_doors clamps the angle byte to
	//     +0x40/-0x40 over a 256-step turn (doors.c:1084, 1117). There is no
	//     variable extent anywhere in the swing.
	//   * THE SIGN is chosen per activation, not stored: the mirror bit
	//     record[2]&1 is set when the face the player used belongs to the room
	//     the hinge's preceding face opens onto (doors.c:527-533), so the door
	//     always swings AWAY from whoever opened it.
	//   * SPEED is the frame step times byte[cmdrec+7] when that is nonzero,
	//     and otherwise the frame step clamped to 8 (doors.c:1044-1047).
	//   * DELAY -- how long it stands open before closing itself -- is
	//     word[cmdrec+0x0a] times byte[cmdrec+7] (spawn_door_instance's
	//     `ecx ? ebx*ecx : ebx` into record[0xa], read back as the dwell
	//     counter at doors.c:1055-1060).
	//
	// CORRECTION TO ROTH_COMMANDS.md, from doors.c: that document calls
	// `+0x0a` x `+0x07` the "swing extent" and `+0x0e`/`+0x10` a "target
	// vector x,y -- the point the wall moves to". Neither holds. The product is
	// the OPEN DWELL TIME as above, and the two words land in record[0x26] and
	// record[0x28], whose only readers in the whole subsystem treat them as
	// SOUND IDS PLUS ONE: record[0x26] is decremented and played when the dwell
	// expires and the door starts closing (doors.c:1057-1063), record[0x28]
	// likewise on full close, positioned at the hinge (doors.c:1142-1145).
	// There is no target point in the swing at all -- the leaf rotates about
	// its hinge by a fixed 90 degrees and nothing else.
	log.Line("  door leaves are built and closed; nothing opens them -- stage 7");
	log.Line("  commands and triggers           -- stage 7");

	Printf("Realms: %s -- %d sectors, %d lines, %d sides. Report: %s\n",
		map->rothFile.GetChars(), (int)Level->sectors.Size(),
		(int)Level->lines.Size(), (int)Level->sides.Size(),
		log.Path().c_str());

	// Close the report here rather than leaving it to the next Begin(). The
	// stage timings and the counter summary are only written by End(), so
	// without this the gap report for the map you are actually standing in never
	// reaches the file -- it appears only after you load a DIFFERENT map, which
	// is the one moment nobody is looking for it.
	log.End();
}
