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
#include "roth/roth_surface.h"
#include "roth/roth_install.h"
#include "roth/roth_log.h"
#include "roth/roth_texture.h"
#include "roth/roth_objects.h"
#include "roth/roth_runtime.h"
#include "c_cvars.h"

//==========================================================================
//
// Realms' depth shading, on or off.
//
// ON is correct and is the default: in Realms the darkness IS the game -- the
// manor is mostly black with a few sources, and an evenly lit one is not the
// same place. But it makes the level hard to WORK on, and the reason this was
// switched off for a stretch is that you cannot see what you are fixing.
//
// So it is a switch rather than an edit. Applied at load, so a change needs the
// map reloading; that is deliberate, because the alternative is recomputing
// every sector's shading on a cvar callback and the two paths then disagree.
//
//==========================================================================
CVAR(Bool, roth_lighting, true, CVAR_ARCHIVE | CVAR_NOINITCALL)

// The flat-screen vertical projection stretch, defined in hw_entrypoint.cpp.
// Realms does not project with square pixels; see where this is set below.
EXTERN_CVAR(Float, r_view_vstretch)
// The world-locked sky cylinder, defined in hw_skyportal.cpp.
EXTERN_CVAR(Bool, r_skyband)
// The player's own field of view. Setting the player's FOV fields is not
// enough: the player SPAWN overwrites both from this cvar
// (p_mobj.cpp:6566, `p->DesiredFOV = p->FOV = QzDoom_GetFOV()`), so a map that
// wants its own field of view has to move the cvar or be silently overridden.
EXTERN_CVAR(Float, fov)

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

	// Realms' lighting, ON. A sector's light byte is a SIGNED offset from 128
	// and does NOT set brightness -- it sets where the darkness starts and how
	// black it can get. The whole model, and where every part of it was read
	// from, is in ROTH_LIGHTING.md:
	//
	//     off  = light - 128
	//     row  = (depth >> shift) - (8 + off)      clamped to 0..31
	//
	// The shift is per map, from lightAmbience indexing ROTH.C's
	// g_shade_const_table_b (data/obj3_owned.c:708, lifted byte-exact from
	// ROTH.EXE): 0 -> 5, 1 -> 6, 2 -> 7.
	//
	// GZDoom's BUILD light mode is the same equation, which is why it is used
	// rather than fog. Its shader computes
	//
	//     shade = (1 - light) * 31 + depth * globvis   clamped to 32 shades
	//
	// -- linear in depth, per-surface offset, 32 shades. Build fixes globvis at
	// 1/64 (hw_drawinfo.cpp:1821); with ShadeFalloffShift set, the loader
	// overrides it to 1/(1<<shift) so the falloff RATE is the original's exactly.
	//
	// This was off for a while so the level could be worked on. It is on now
	// because the goal is 1:1, and in Realms the darkness IS the game: the
	// original is mostly black with a few sources, and a flat, evenly lit manor
	// is not the same place.
	//
	// CORRECTED 2026-10-01 against the oracle (REMAROTH_LIGHTING_ORACLE.md):
	// NORMAL sectors take their shift from metadata +0x10 (shadeLevel), and
	// only LANTERN sectors (sector byte +0x0a bit 1) take +0x14 (lightAmbience)
	// -- render_world_face_list, renderer.c:9190-9197. Lantern sectors add a
	// screen-centred cone term; the whole equation is R_RothShade in main.fp,
	// which this mode now drives instead of Build's.
	int shadeShift = 5, lanternShift = 5;
	{
		static const int kShadeShift[3] = { 5, 6, 7 };
		const int lvl = int(rm.metadata.shadeLevel);
		const int amb = int(rm.metadata.lightAmbience);
		if (lvl >= 0 && lvl < 3) shadeShift = kShadeShift[lvl];
		else log.Count("lighting: shadeLevel outside the shade table", 1);
		if (amb >= 0 && amb < 3) lanternShift = kShadeShift[amb];
		else log.Count("lighting: lightAmbience outside the shade table", 1);

		if (roth_lighting)
		{
			Level->ShadeFalloffShift = shadeShift;
			Level->RothLanternShift = lanternShift;
			Level->RothLighting = true;
			Level->RothLightRng = 0;
			Level->RothLightStep = 0;
			// getRealLightmode takes info->lightmode unconditionally when set
			// (g_level.cpp:163), so this wins over the user's gl_maplightmode.
			if (Level->info != nullptr) Level->info->lightmode = ELightMode::Build;
			log.Line("  lighting       depth >> %d (lantern >> %d), Realms shading",
				shadeShift, lanternShift);
		}
		else
		{
			// Left to the engine's own default rather than forced bright: the
			// point of the switch is to SEE the level, and whatever GZDoom does
			// untouched is the most honest "not Realms' lighting" baseline.
			log.Line("  lighting       OFF (roth_lighting 0) -- engine default shading");
		}
	}

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
	// Drop the previous level's logic before anything registers against this
	// one. BeginLevel cannot do it -- by the time it runs, the loader has
	// already recorded its face and door bindings.
	roth::EndLevel();

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
		// The picture the engine draws wherever a surface resolves to the sky.
		// Named by the MAP (metadata +0x18), never by the pack: TOWER1 asks for
		// entry 72 (an ANIMATED one, so moving cloud), the Raquia levels for
		// 400, the labyrinth for 810.
		//
		// This was briefly removed on the grounds that Realms' sky is meant to
		// be drawn as parallax columns above a wall rather than stretched around
		// the world. Both are true, but the flats come first: 6,208 ceilings and
		// floors across the game sit ON the sky marker, and with no sky texture
		// set they render as whatever the engine defaults to. The parallax layer
		// above walls is still not modelled -- that is the FF_EDGE_MAP counter.
		Level->skytexture1 = Level->skytexture2 = skyTex;
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
	// it would not be built here anyway.
	//
	// 0xFFFE IS A DOOR, and the OPEN QUESTION that stood here is answered:
	// resolve_door_neighbor_sector (doors.c:320) -- the function that finds a
	// door's PARTNER -- accepts a neighbour whose id is >= 0xFFFE, and
	// spawn_door_instance sends anything that is not 0xFFFD to the primary
	// pool, i.e. an ordinary swinging door. So a 0xFFFE sector is the second
	// panel of a DOUBLE DOOR.
	//
	// Leaving them out left a black gap with a vertical seam down the middle of
	// it -- which is exactly where the two panels of a double door meet.
	auto leafBuildable = [&](const roth::Sector &rs) -> bool
	{
		if (!rs.IsDoorCapable() || rs.faceCount != 4 || rs.hingeFace < 0) return false;
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

	int doorCount = 0, doorsWithHinge = 0, flatsToSky = 0, flatFlipsApplied = 0;
	int flatOpaque256 = 0;   // flats taking the 256x256-opaque scale exception
	int pinBottomFaces = 0;  // pieces anchored by DRAW_FROM_BOTTOM
	int skyFlats = 0;
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

		// Equating the two shade equations for the distance at which a surface
		// reaches black gives
		//
		//     lightlevel = 255 * (39 + off) * 2^shift / 1984
		//
		// with 39 = 31 + 8, the full shade range plus the original's head start.
		// off = 0 lands on 160, +20 on 242, -10 on 119. DERIVED from the two
		// equations, not tuned by eye.
		//
		// A sector whose fade would already be over at zero distance goes fully
		// black, which is what the original does and a tool its designers used:
		// the `light != 0` test in renderer.c:9187 means a sector authored at
		// exactly 0 is immune even to the muzzle-flash brightening.
		{
			// Realms shading reads the light BYTE straight out of lightlevel
			// (R_RothShade); the old Build-equivalent mapping saturated at 255
			// for anything brighter than about -23 and lost the difference.
			sec->lightlevel = roth_lighting ? (short)rs.light : (short)160;
		}
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

			// THE SKY MARKER. The pack header names one stored index that means
			// "this surface is the sky" -- see Pack::SkyMarkerIndex. It is a
			// different thing from the map's skyTexture, which says which
			// PICTURE to draw there, and the two were conflated in both
			// directions before this: first by reading the marker as the
			// picture, then by dismissing it entirely.
			//
			// Measured across all 44 retail maps: 36 of them carry flats on
			// their pack's marker, 6,208 in total, from 2 in OPTEMP1 to 594
			// ceilings in TOWER1 -- whose sky picture is an ANIMATED entry, so
			// it is moving cloud. Drawing those as an ordinary flat paints the
			// sky onto the ceiling, which is exactly what TOWER1 looked like.
			const bool isSky = haveArt && art.IsSkySurface(index);
			FTextureID tex = isSky ? FNullTextureID() : worldTex(index);
			if (isSky) skyFlats++;

			// A flat must draw SOMETHING or the sector renders hall of mirrors,
			// so "nothing here" also becomes the sky.
			sec->SetTexture(which, tex.isValid() ? tex : skyflatnum, false);
			if (!isSky && !tex.isValid()) flatsToSky++;

			//----------------------------------------------------------
			// THE FLAT TRANSFORM, and the ONLY place a sector plane gets one.
			//
			// Everything about how a flat is scaled, shifted and mirrored lives
			// in roth_surface. It used to live here AND in roth_runtime.cpp with
			// different numbers, which is why fixes kept landing in one copy and
			// not the other: a platform changed appearance the moment it moved,
			// because the runtime recomputed the scale and silently dropped the
			// mirrors.
			//
			// What this block used to do wrong, for the record, because all of
			// it was believed at the time:
			//   * 2^(s+1) units per texel. It is 2^s (renderer.c:3406 via the
			//     visible pass), so every flat was twice too coarse -- four
			//     times on a 256x256 opaque one, which has its own rule.
			//   * POSITIVE scales on both axes. Both are negative: the engine's
			//     v is already -y/64 at the vertex, so matching ROTH's +y needs
			//     a second negation, and ROTH's -x needs one of its own. Every
			//     flat in the game was mirrored on both axes.
			//   * A mirror negated the scale AND the offset, which double-
			//     negates and leaves the mirrored copy a shift out of place.
			//
			// The encoding is proven by test rather than argued: 665,856 sample
			// points over every scale, shift, mirror and four texture sizes --
			// see tools/rothdiff/test_roth_surface.cpp.
			//----------------------------------------------------------
			roth::FlatSetup fs = {};
			fs.textureWord = (uint16_t)index;
			fs.scaleBits = (uint8_t)shift;
			fs.shiftX = (uint8_t)shx;
			fs.shiftY = (uint8_t)shy;

			// The mirror bits, from sector +0x17. Floor uses bits 0-1, ceiling
			// bits 2-3 (renderer.c:9229 / :9249, VISIBLE pass -- the cursor-pick
			// pass writes a bare fill word with no mirrors in it, and reading
			// rules from that pass is what made this code look wrong once
			// already).
			const uint8_t flipBits = (uint8_t)(rs.flags2 >> 8);
			fs.mirrorX = isFloor ? (flipBits & 0x01) != 0 : (flipBits & 0x04) != 0;
			fs.mirrorY = isFloor ? (flipBits & 0x02) != 0 : (flipBits & 0x08) != 0;
			if (fs.mirrorX || fs.mirrorY) flatFlipsApplied++;

			// 256x256 AND opaque takes the exception (2^(s-1), and a shift unit
			// becomes a whole texel). Translucent 256x256 keeps the normal rule.
			if (haveArt)
			{
				int iw = 0, ih = 0; bool trans = false;
				if (art.ImageShape(index, iw, ih, trans))
				{
					fs.texW = (uint16_t)iw;
					fs.texH = (uint16_t)ih;
					fs.opaque256 = (iw == 256 && ih == 256 && !trans);
					if (fs.opaque256) flatOpaque256++;
				}
			}

			const roth::FlatEngineSetup fe = roth::FlatToEngine(fs);
			sec->SetXScale(which, fe.xScale);
			sec->SetYScale(which, fe.yScale);
			sec->SetXOffset(which, fe.xOffset);
			sec->SetYOffset(which, fe.yOffset);
			// The quarter turn. The texture is registered turned (roth_texture.cpp),
			// so the plane must be turned too or the flat draws transposed. See
			// roth_surface.h, FlatEngineSetup::angle.
			sec->SetAngle(which, DAngle::fromDeg(fe.angle));
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
		// Lantern flag for R_RothShade (sector byte +0x0a bit 1). Desaturation
		// 1/255 is invisible; the shader reads it as a flag.
		sec->Colormap.Desaturation = (roth_lighting && (rs.flags & 2)) ? 1 : 0;

	}
	log.Line("  doors closed at load  %d  (%d with a hinge resolved)", doorCount, doorsWithHinge);
	log.Count("doors: no hinge face found", doorCount - doorsWithHinge);
	log.Line("  flats with no art -> sky  %d", flatsToSky);
	log.Line("  flats ON the sky marker   %d", skyFlats);
	// Realms' neutral is 0x80 = 128. A range hugging or exceeding 255 means the
	// mapping has gone wrong again and the level will look flat and overlit.
	log.Line("  sector light  %d .. %d  (Realms neutral is 128)", darkestLight, brightestLight);
	log.Line("  flat mirror flips %d surface(s) mirrored on one or both axes", flatFlipsApplied);
	log.Line("  flats on the 256x256-opaque scale exception  %d", flatOpaque256);
	log.Line("  DRAW_FROM_BOTTOM pieces  %d  (explicit offset, not a peg flag)", pinBottomFaces);

	//----------------------------------------------------------------------
	// REALMS' FIELD OF VIEW. We were not setting one at all, so every level has
	// rendered at GZDoom's default 90 degrees.
	//
	// ROTH.C builds its projection from a focal length of
	// `view width * 0x7c/256` (ROTH_SURFACES_FIX.md 3.7):
	//
	//     tan(hfov/2) = (w/2) / (w * 124/256) = 128/124 = 1.032258
	//     hfov        = 2 * atan(1.032258)    = 91.82 degrees
	//
	// Set on the player rather than in MAPINFO because this build has no `fov`
	// map option (grep DEFINE_MAP_OPTION), and an unknown MAPINFO key is a
	// fatal parse error raised before the video backend exists -- a silent
	// startup failure of exactly the kind that stalled stage 8.
	//
	// Both fields are set: DesiredFOV is what the player wants and FOV is what
	// is in force, and setting only one lets the next think undo it.
	{
		const float ROTH_FOV_DEGREES = 91.82f;
		// The CVAR as well as the fields. Setting only the fields does not
		// survive: the player spawn overwrites both from the cvar
		// (p_mobj.cpp:6566), so the view quietly reverted to the engine's 90 --
		// about 2% too narrow, which skews EVERY comparison against the
		// original, not only the ones about field of view.
		if (fabs(fov - ROTH_FOV_DEGREES) > 0.001f) fov = ROTH_FOV_DEGREES;
		for (int i = 0; i < MAXPLAYERS; i++)
		{
			players[i].DesiredFOV = ROTH_FOV_DEGREES;
			players[i].FOV = ROTH_FOV_DEGREES;
		}
		log.Line("  field of view         %.2f degrees (Realms; cvar and players)",
			ROTH_FOV_DEGREES);
	}

	//----------------------------------------------------------------------
	// AND THE VERTICAL HALF OF THE SAME PROJECTION, which the field of view
	// above does not cover.
	//
	// Realms does not project with square pixels. Fitted from the original's
	// own output at 640x480 (tools/oracle, flatfit.py): FX = 309.77 across,
	// FY = 355.06 down -- 91.9 degrees and 68.1. The horizontal matches what is
	// set above already; square pixels would put the vertical at 75.4, about 7
	// too wide, and everything would sit at the wrong height on screen.
	//
	// That is not only a look: every check this port makes compares one of our
	// frames against one of the original's, so a vertical mismatch moves every
	// comparison point to the wrong row and reads as a lighting or a surface
	// error when it is neither.
	//
	// NOT `pixelratio`. That scales the view transform, and the VR code reads
	// it to convert headset metres to world units (gl_openvr.cpp:1087), so
	// setting it here would silently resize the world in the headset. The cvar
	// below is applied to the flat-screen PROJECTION only and is ignored in VR,
	// where the lens sets the projection. See hw_entrypoint.cpp.
	{
		const float ROTH_VIEW_VSTRETCH = 355.06f / 309.77f;   // 1.1462
		r_view_vstretch = ROTH_VIEW_VSTRETCH;
		log.Line("  view vertical stretch %.4f  (Realms FY/FX; flat screen only)",
			ROTH_VIEW_VSTRETCH);
	}

	//----------------------------------------------------------------------
	// AND THE SKY, which Realms draws on a world-locked cylinder rather than a
	// dome. Off by default in the engine so no other map changes; a Realms map
	// turns it on here, for the same reason the two values above are set here.
	// See FSkyVertexBuffer::RenderRothSky.
	//----------------------------------------------------------------------
	if (!r_skyband)
	{
		r_skyband = true;
		log.Line("  sky                   world-locked cylinder (r_skyband)");
	}

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
	int storedExtentScaled = 0;  // faces scaled by the stored extent (the real rule)
	int imageFitFaces = 0;       // FF_IMAGE_FIT faces
	int imageFitVerticalUnhandled = 0; // PIECES, not faces: up to 3 per side
	int flippedFaces = 0;        // FF_FLIP_X, approximated by a negative scale
	int shiftedFaces = 0;        // a non-zero shiftX/shiftY was applied
	int edgeMapFaces = 0;        // FF_EDGE_MAP: the outdoor backdrop seen through windows
	int keyWallPieces = 0;       // on the colour key: the original draws nothing there
	int transUpLoFaces = 0;      // FF_TRANS_UPLO with no band (not transparent, or override 0)
	int transUpLoBanded = 0;     // mid pieces cut to the TEXTURE_MAP_OVERRIDE band
	int transUpLoInexact = 0;    // ...whose single copy cannot match the original's wrap
	int extentBitsAbove12 = 0;   // see the note where this is reported
	int doorMidToLeaf = 0;       // doorway mid pieces handed over to a door leaf

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
		bool hasUpper = false;

		auto makeSide = [&](const roth::Face &face, bool twoSided, int neighbourSector) -> side_t *
		{
			side_t *sd = &Level->sides[sideIndex++];
			// A trigger names a FACE; the engine hands us a sidedef. Only this
			// loop knows both, so the pairing is recorded here or the two index
			// spaces never meet. See roth_runtime.
			roth::RegisterFaceSide((int)(&face - &rm.faces[0]), (int)(sd - &Level->sides[0]));
			sd->sector = &Level->sectors[face.sector];
			sd->linedef = ld;
			sd->Flags = WALLF_NOFAKECONTRAST;   // Realms has no fake contrast
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
			bool wantMid = !twoSided || (tf & roth::FF_TRANSPARENT) != 0;

			// A DOOR LEAF TAKES OVER ITS DOORWAY'S MID PIECE.
			//
			// The two long walls of a door sector are two-sided and flagged
			// FF_TRANSPARENT, which is how the closed door's picture gets drawn
			// into the opening. That static mid piece and the swinging leaf built
			// further down are THE SAME SURFACE -- and only one of them can move.
			// Drawing both leaves two copies of the door in the same plane, which
			// is where the coplanar counter came from.
			//
			// So where a leaf is built, the mid piece is dropped and the leaf
			// carries the door instead. This is not a guess about Realms: it is
			// the direct consequence of having moved that surface onto the
			// polyobject, and it is what makes an open door see-through -- the
			// static piece could never have got out of the way.
			if (twoSided && (tf & roth::FF_TRANSPARENT))
			{
				const bool mineIsLeaf = face.sector >= 0
					&& face.sector < (int)rm.sectors.size()
					&& leafBuildable(rm.sectors[face.sector]);
				const bool theirsIsLeaf = neighbourSector >= 0
					&& neighbourSector < (int)rm.sectors.size()
					&& leafBuildable(rm.sectors[neighbourSector]);
				if (mineIsLeaf || theirsIsLeaf)
				{
					wantMid = false;
					doorMidToLeaf++;
				}
			}

			//--------------------------------------------------------------
			// SCALE. Realms addresses wall art in HALF-texels: two world units
			// per texture pixel, where Doom uses one -- so the base scale is
			// 0.5 on both axes. FF_HALF_PIXEL drops that to one unit per texel.
			//
			// Horizontally the scale is the SAME FIXED 0.5, not derived from the
			// face at all. The original's u is `accumulator >> 1` (renderer.c:4734)
			// masked by the texture dimension minus one (13289 sets the mask to
			// texDim - 1; 4356/4437/4734 apply it), and THE WALL'S LENGTH NEVER
			// APPEARS IN THAT PATH. The stored extent says where the coordinate
			// WRAPS, not how far the art is stretched.
			//
			// This used to divide the stored extent by the wall length, which
			// gave every face its own stretch -- so at a corner, two faces
			// sharing a texture stopped continuing each other's pattern. That is
			// what a seam is. The masked wrap is also why the ORIGINAL cannot
			// have seams: a coordinate can only land inside the texture, so a
			// partial tile is structurally impossible.
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
			double stored = double(tm->StoredExtent());
			// The extended record keeps only 12 bits of the extent. Across all 44
			// maps every visible wall's stored extent equals its length modulo
			// that field (oracle survey, 2026-10-01): RAQUIA4 has two 6080- and
			// 6144-long walls stored as 1984 and 2048. Restore the lost high bits
			// rather than drawing those walls three times too wide.
			if (tm->extended && stored > 0. && wallLen > 4096.)
				stored += 4096. * floor((wallLen - stored) / 4096. + 0.5);
			if (imageFit)
			{
				imageFitFaces++;
			}
			else
			{
				// THE STORED EXTENT *IS* THE HORIZONTAL SCALE. Measured, not
				// argued: an instrumented ROTH.C run over STUDY1 logged every
				// wall span's draw flags, and bit 0x100 -- the one that selects
				// the unscaled path -- was CLEAR on all of them:
				//
				//     stored-extent path   0 spans
				//     computed-extent path 34 spans
				//
				// So every wall takes the computed path, where renderer.c:13347
				// builds extent_out = 2 * storedRowLength and renderer.c:4943
				// applies it:
				//
				//     u = alongWall * extent_out / hfit
				//
				// i.e. the stored extent divides the coordinate. Texels across
				// the face = hfit / 2, so
				//
				//     unitsPerTexel = 2 * faceLength / hfit
				//
				// The comment that used to be here claimed the opposite -- that
				// the extent was only a wrap and the scale was a fixed 0.5. That
				// held for the common case ONLY because most faces have
				// hfit == storedRowLength, which collapses the ratio to 2. The
				// probe found one setup with hfit = 16 against a width of 8,
				// where the true density is 1 world unit per texel and the fixed
				// rule draws it at half size -- and that setup was the most drawn
				// of the sample, 16 spans of 34.
				//
				// HALF_PIXEL still halves it (renderer.c:13336 doubles the
				// along-wall coordinate itself).
				if (stored > 0. && wallLen > 0.)
				{
					// texels across = hfit/2, and Doom's scale is texels per
					// world unit.
					scaleX = (stored * 0.5) / wallLen;
					if (tf & roth::FF_HALF_PIXEL) scaleX *= 2.;
					storedExtentScaled++;
				}
				else
				{
					// No usable extent: fall back to the old fixed density
					// rather than divide by zero, and count it so a map full of
					// these is visible rather than silent.
					storedExtentUnusable++;
				}

				// Faces where the extent is NOT simply the wall's length are the
				// ones the old fixed rule drew at the wrong size, so they are
				// worth a number of their own.
				if (stored > 0. && wallLen > 0. && fabs(stored - wallLen) > 1.)
					storedExtentDeviates++;
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
			//--------------------------------------------------------------
			// FF_TRANS_UPLO: THE MID PIECE IS A BAND, NOT THE WHOLE OPENING.
			//
			// compute_face_span_extents (renderer.c:8171) builds a transparent
			// mid piece from the opening -- top = min(ceilings), bottom =
			// max(floors) of the face's own sector (face +6) and its sister's --
			// and then, when the texture map has flag 0x08 AND the face's OWN
			// sector's TEXTURE_MAP_OVERRIDE (sector +0x0c, signed) is non-zero,
			// cuts it to a band 4 * |override| world units tall
			// (renderer.c:8196-8207):
			//
			//   override > 0   bottom = top - 4*override     (hung from the top)
			//   override < 0   top    = bottom + 4*|override| (stood on the floor)
			//
			// The texture is anchored at the band's top (0x852be feeds
			// wrap_reoffset+0x0a). This is how furniture draws its low panels:
			// STUDY1's chair (sectors 421-427, override -10) has a 40-unit band
			// at floor level on its back and arm faces. Ignoring it hung the
			// chair-back art from the CEILING, top-anchored in an opening 384
			// units tall, and stretched the FF_IMAGE_FIT arm faces over the
			// whole height -- the purple slab and brown posts over the desk.
			//--------------------------------------------------------------
			const double openTop = haveNbr ? min(ownCeil, nbrCeil) : ownCeil;
			const double openBot = haveNbr ? max(ownFloor, nbrFloor) : ownFloor;
			double bandTop = openTop, bandBot = openBot;
			bool banded = false;
			if (twoSided && (tf & roth::FF_TRANSPARENT) && (tf & roth::FF_TRANS_UPLO)
				&& face.sector >= 0 && face.sector < (int)rm.sectors.size())
			{
				const int tmo = (int)rm.sectors[face.sector].textureMapOverride;
				if (tmo > 0)      { bandBot = openTop - 4. * tmo; banded = true; }
				else if (tmo < 0) { bandTop = openBot - 4. * tmo; banded = true; }
			}

			auto heightOf = [&](int part) -> double
			{
				if (!haveNbr) return ownCeil - ownFloor;
				if (part == side_t::top)    return ownCeil - nbrCeil;
				if (part == side_t::bottom) return nbrFloor - ownFloor;
				return bandTop - bandBot;
			};

			auto setPart = [&](int part, int storedIndex, bool masked)
			{
				// THE COLOUR KEY MEANS DRAW NOTHING, on a wall as on a flat.
				//
				// renderer.c:9222-9225 is a THREE-way test, and this port had
				// it as two: a negative index is a solid palette colour, an
				// index equal to the pack's key draws NO PIXELS AT ALL, and
				// anything else is textured. The key is a real painted entry in
				// every pack -- DEMO's is an opaque 256x146 picture -- so
				// handing it to World() returns that artwork and we paint a
				// wall across an opening the original leaves open.
				//
				// Measured over all 44 maps: of 59,273 wall pieces that are
				// present, 1,778 sit on the key, and 1,325 of those are MID
				// pieces -- the ones on two-sided lines, where drawing anything
				// at all turns a doorway into a barrier.
				//
				// IsSkySurface is this exact test (index == SkyMarkerIndex);
				// the name is from when the key was thought to mean sky.
				if (haveArt && art.IsSkySurface(storedIndex))
				{
					keyWallPieces++;
					return false;
				}

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
					// ONLY COUNT A PIECE THAT IS ACTUALLY DRAWN. This used to
					// count every case where the fit could not be computed,
					// which included every zero-height step -- and the upper and
					// lower are attempted unconditionally on every two-sided
					// wall, so a wall whose ceilings simply match scored one.
					// That reported 572 "not expressible" fits in STUDY1 alone,
					// all of them pieces Doom draws nothing for. A counter whose
					// number is dominated by non-events is worse than no counter,
					// because it reads as a backlog.
					else if (pieceHeight > 0.) imageFitVerticalUnhandled++;
				}
				// FF_FLIP_X mirrors the art, which Doom expresses as a negative
				// horizontal scale. NOT EXACTLY THE SAME THING: ROTH.C mirrors
				// inside the texture's own texel range (renderer.c:4735 computes
				// H - u - 1) AFTER the shift is added, so where a piece is wider
				// than one copy of the art, or carries a shiftX, the two differ.
				// A negative scale is the closest Doom has.
				if (tf & roth::FF_FLIP_X) { sx = -sx; flippedFaces++; }

				//------------------------------------------------------------
				// DRAW_FROM_BOTTOM, as an EXPLICIT OFFSET rather than a peg flag.
				//
				// Realms anchors each wall piece to a coordinate it computes --
				// the visible pass writes it into wrap_reoff[0x0a]:
				//
				//   upper piece   = clip_top, the sector's own ceiling  R:8664
				//   lower piece   = the piece's own top edge, i.e. the
				//                   top of the step                     R:8685
				//
				// Doom's DEFAULT bottom-part anchor is already the top of the
				// step, so a lower piece needs NO flag at all. Setting
				// ML_DONTPEGBOTTOM there -- which is what FF_PIN_BOTTOM used to
				// do -- re-anchors it to the ceiling and misplaces every one.
				//
				// FF_PIN_BOTTOM (DRAW_FROM_BOTTOM) is a different rule entirely:
				// the texture's BOTTOM sits on the piece's bottom edge
				// (renderer.c:5057-5063). Where the piece is taller than the
				// art that is a downward shift of (pieceHeight - texHeight),
				// expressed in the texel units Doom's offsets use.
				//------------------------------------------------------------
				double pieceOffY = offY;
				if (tf & roth::FF_PIN_BOTTOM)
				{
					auto *gt = TexMan.GetGameTexture(tex, false);
					const double texH = gt ? gt->GetDisplayHeight() : 0.;
					const double pieceHeight = heightOf(part);
					if (texH > 0. && pieceHeight > 0.)
					{
						// sy is texels-per-world-unit for this part, so the
						// piece is pieceHeight * sy texels tall.
						//
						// MEASURED (oracle, SALVAT face 14208, 100% of 248k
						// pixels): texel = W - (z - bottom)/2 - shiftY, i.e. the
						// art's bottom row on the piece's bottom edge. In Doom
						// terms, from the top anchor, that is a row offset of
						// +(texH - pieceHeight*sy). This line used to have the
						// sign the other way round, which misplaced every
						// DRAW_FROM_BOTTOM wall whose height is not a whole
						// number of copies (483 faces, mostly SALVAT/DOMINION).
						pieceOffY += texH - pieceHeight * sy;
						pinBottomFaces++;
					}
				}

				// A banded mid piece: Doom anchors a two-sided mid texture's top
				// at min(ceilings) + rowoffset/scale (hw_walls.cpp DoMidTexture),
				// so move it down to the band's top. pieceOffY so far is the
				// texel row wanted AT the band top.
				if (part == side_t::mid && banded)
				{
					pieceOffY += (bandTop - openTop) * sy;
					transUpLoBanded++;
					auto *gt = TexMan.GetGameTexture(tex, false);
					const double texH = gt ? gt->GetDisplayHeight() : 0.;
					// One copy is drawn. The original wraps inside the band, so a
					// band taller than the art loses its repeats, and a ceiling-
					// hung band shorter than the art spills below it (Doom only
					// clips a mid piece to the opening). Counted, not hidden.
					if (sy > 0. && texH > 0. && fabs(texH / sy - (bandTop - bandBot)) > 0.5
						&& !(bandBot <= openBot && texH / sy > bandTop - bandBot))
						transUpLoInexact++;
				}

				sd->SetTexture(part, tex);
				sd->SetTextureXScale(part, sx);
				sd->SetTextureYScale(part, sy);
				sd->SetTextureXOffset(part, offX);
				sd->SetTextureYOffset(part, pieceOffY);
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
			// NOW VERIFIED, and one half of it was wrong. The visible pass
			// writes each piece's anchor into wrap_reoff[0x0a]:
			//
			//   upper piece  = clip_top, the sector's own ceiling     R:8664
			//   lower piece  = the piece's own top edge, the step top R:8685
			//
			// The upper case matches ML_DONTPEGTOP, so that stays. The LOWER
			// case is already Doom's default for the bottom part, so it needs
			// no flag -- and ML_DONTPEGBOTTOM re-anchors it to the ceiling,
			// which misplaced every lower that carried FF_PIN_BOTTOM.
			//
			// FF_PIN_BOTTOM is not a Doom pegging rule at all: it puts the
			// TEXTURE's bottom on the PIECE's bottom edge (renderer.c:5057-5063)
			// and is now applied as an explicit Y offset in setPart above.
			//--------------------------------------------------------------

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
			if ((tf & roth::FF_TRANS_UPLO) && !banded) transUpLoFaces++;
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
		// NO ML_DONTPEGBOTTOM. Doom's default bottom-part anchor is already the
		// top of the step, which is exactly what Realms does (R:8685); the flag
		// would re-anchor it to the ceiling instead. FF_PIN_BOTTOM is applied as
		// an explicit texture offset in setPart.

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
			cs->Colormap.Desaturation = Level->sectors[i].Colormap.Desaturation;

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

			// THROUGH roth_surface, like every other flat. These slabs are the
			// rugs and table tops, so they are exactly the surfaces people look
			// at and say the textures are wrong.
			//
			// Two things this block used to get wrong beyond the scale:
			//   * the shifts went in RAW, unscaled -- the engine's offsets are
			//     in world units and get multiplied by the scale afterwards, so
			//     a raw shift is off by a factor of units-per-texel;
			//   * the Y offset was not negated, so the two axes disagreed about
			//     which way was up.
			//
			// NO MIRRORS on mid-platforms: their span setup writes a bare
			// 0x38 / 0xb8 with no sector bits ORed in (renderer.c:9339, :9354),
			// unlike sector flats at :9229 / :9249. The scale byte at +0x0C
			// carries both fields -- bits 2-3 underside, 4-5 top.
			auto midFlat = [&](uint16_t texWord, uint8_t s, uint8_t shX, uint8_t shY)
			{
				roth::FlatSetup f = {};
				f.textureWord = texWord;
				f.scaleBits = s;
				f.shiftX = shX;
				f.shiftY = shY;
				// mirrorX / mirrorY stay false: see above.
				if (haveArt)
				{
					int iw = 0, ih = 0; bool trans = false;
					if (art.ImageShape((int)texWord, iw, ih, trans))
					{
						f.texW = (uint16_t)iw;
						f.texH = (uint16_t)ih;
						f.opaque256 = (iw == 256 && ih == 256 && !trans);
					}
				}
				return roth::FlatToEngine(f);
			};

			const roth::FlatEngineSetup top =
				midFlat(mp.topTexture, (uint8_t)((mp.scales >> 4) & 3),
					mp.topShiftX, mp.topShiftY);
			const roth::FlatEngineSetup und =
				midFlat(mp.undersideTexture, (uint8_t)((mp.scales >> 2) & 3),
					mp.undersideShiftX, mp.undersideShiftY);

			cs->SetXScale(sector_t::ceiling, top.xScale);
			cs->SetYScale(sector_t::ceiling, top.yScale);
			cs->SetXOffset(sector_t::ceiling, top.xOffset);
			cs->SetYOffset(sector_t::ceiling, top.yOffset);
			cs->SetXScale(sector_t::floor, und.xScale);
			cs->SetYScale(sector_t::floor, und.yScale);
			cs->SetXOffset(sector_t::floor, und.xOffset);
			cs->SetYOffset(sector_t::floor, und.yOffset);
			cs->SetAngle(sector_t::ceiling, DAngle::fromDeg(top.angle));
			cs->SetAngle(sector_t::floor, DAngle::fromDeg(und.angle));

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

			// So the level LOGIC can move this slab later: several opcodes reach
			// a sector's mid-platform through its +0x18, and on this side that
			// platform IS this control sector's two planes.
			roth::RegisterPlatformControl((int)i, (int)ctrlSector);
			roth::RegisterLightFollower((int)i, (int)ctrlSector);
			roth::RegisterTexture(mp.topTexture, topTex);
			roth::RegisterTexture(mp.undersideTexture, undTex);

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
	// COINCIDENT SURFACES -- A KNOWN, UNRESOLVED RISK, recorded rather than
	// papered over.
	//
	// The leaf is built on the door sector's own four vertices, because that is
	// what ROTH.C does (the quad points are `gs:[vertex+8] - hinge`, with no
	// inset), so when closed the leaf lies EXACTLY on the doorway's own wall
	// faces. Mostly that is harmless: the two long faces are two-sided portals
	// whose only drawn pieces are the upper/lower steps ABOVE and BELOW the
	// leaf's own height, so nothing shares a plane with it. Where it is not
	// harmless is a face that draws a MID texture in the same place -- a
	// one-sided thickness edge, or a two-sided face flagged FF_TRANSPARENT --
	// which will be coplanar with the leaf and can z-fight.
	//
	// The original almost certainly has an answer for this: tick_swinging_doors
	// sets bit 0 of fs:[sector+0x16] on BOTH sectors a door touches
	// (doors.c:1150-1155, cleared again on full close), and +0x16 is
	// Sector::flags2. What that bit makes the renderer do was NOT traced, and
	// "the door quad replaces the sector's own walls" is a plausible reading
	// that I have not confirmed. UNVERIFIED -- so nothing is suppressed here,
	// and the faces that could clash are counted instead. If doors shimmer on
	// screen, this counter is where to start and doors.c's flags2 bit is the
	// thing to trace.
	// Per SIDE, not per leaf: all four sides of the prism are skinned, so a leaf
	// can contribute up to four. Named for what it counts -- the old
	// "leavesNoTexture" read as a leaf count and was never one.
	int leavesBuilt = 0, leafSidesNoTexture = 0, doorCapableNotBuilt = 0;
	int leafCoplanarRisk = 0;
	for (const auto &rs : rm.sectors)
		if (rs.IsDoorCapable() && !leafBuildable(rs)) doorCapableNotBuilt++;

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
			vs->Colormap.Desaturation = door->Colormap.Desaturation;
			roth::RegisterLightFollower((int)i, (int)vSector);
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
			// THE CORNERS. P[k] is corner c_k's OWN first vertex, so P is the
			// sector's corner loop with its start rotated to the hinge. This
			// used to store face.vertex2 and then pair vertex j with vertex
			// j+1, which looks like "the line reversed" and is not: because the
			// ring closes with v2(c_k) == v1(c_(k+1)) (the gate above), that
			// made line j span v1(c_(j+1)) -> v1(c_(j+2)), i.e. face c_(j+1)
			// traversed FORWARD. One shift, two bugs -- the fronts faced INTO
			// the slab, so from outside the player saw the backs, which have no
			// sidedef at all; and the skin below, which textures j == 1 and
			// j == 3, painted the two THICKNESS edges and left the broad faces
			// blank. Keep the corners and the lines in the SAME indexing.
			for (int j = 0; j < 4; j++)
			{
				// Cyclically from the hinge, matching ROTH.C's corner ordering:
				// with the hinge at slot h, c_k is slot (h + k) & 3. FORWARD --
				// see HANDOFF_REMAROTH.md section 12 for why the "backward"
				// reading was wrong and how it was re-derived.
				const int fi = f0 + ((hingeRel + j) & 3);
				const roth::Face &f = rm.faces[fi];

				vertex_t *va = &Level->vertexes[vVertex + (unsigned)j];
				va->set(double(rm.vertices[f.vertex1].x) + offX,
				        double(rm.vertices[f.vertex1].y) + offY);
			}
			auto tmOf = [&](const roth::Face &ff) -> const roth::TextureMap *
			{
				return (ff.textureMap >= 0 && ff.textureMap < (int)rm.textureMaps.size())
					? &rm.textureMaps[ff.textureMap] : nullptr;
			};

			for (int j = 0; j < 4; j++)
			{
				const int fi = f0 + ((hingeRel + j) & 3);
				const roth::Face &f = rm.faces[fi];

				line_t *ld = &Level->lines[vLine + (unsigned)j];
				side_t *sd = &Level->sides[sideIndex++];

				// Line j IS face c_j, REVERSED: from c_j's second corner back
				// to its first. Realms winds a face v1->v2 with its owning
				// sector on the right and a one-sided Doom line's front is on
				// the right of v1->v2, so running it backwards puts the front
				// on the OUTSIDE, which is where the player sees the slab from.
				ld->v1 = &Level->vertexes[vVertex + (unsigned)((j + 1) & 3)];
				ld->v2 = &Level->vertexes[vVertex + (unsigned)j];
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
				sd->Flags = WALLF_NOFAKECONTRAST;   // Realms has no fake contrast
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
				// This was marked UNVERIFIED -- that the _b (sister) surface is the
				// outward one rather than the _a -- and it is now settled, by the
				// extent argument rather than by the renderer. See the mapping
				// table below; the owning-sector rule and ROTH.C agree.
				//
				// WHAT WAS WRONG HERE BEFORE: this said "the leaf is a FLAT PANEL,
				// NOT A BOX". The geometry half of that is refuted by the binary's
				// own door template -- g_door_vertex_template and its body
				// (obj3_owned.c:866-872) are 20 u16 = FOUR quads of five indices,
				// closing cleanly, referencing vertex slots 0x00-0x30 AND
				// 0x40-0x70. Two rings, eight vertices, all four edges present:
				// the slab is a four-sided prism, and the second ring is the quad
				// array a reading had written off as never used.
				//
				// WHICH SURFACE LANDS ON WHICH QUAD -- SETTLED, 2026-10-02. This
				// was the last open point in the door work and the one three
				// readings disagreed on. It is decided not by matching two
				// four-element lists in order, which is what made it look like an
				// inference, but by reading what each argument is USED for
				// (setup_door_corner_surface_a/_b, doors.c:281-297):
				//
				//   _a(ebx, edx):  [out+0x26] = fs:[fs:[edx+4]] & 0xfff
				//                  [out+0x0c] = fs:[fs:[ebx+4] + 2]
				//   _b(ebx):       bx = fs:[ebx+8] FIRST, then both from there
				//
				// +0x00 of a mapping record is the fit word, so `& 0xfff` is the
				// STORED EXTENT, and it comes from edx. +0x02 is the mid texture,
				// so the PICTURE comes from ebx. _b's `+8` is sisterFaceOffset, so
				// _b reads the SISTER's mapping for both. The call order is
				// _a(c3,c2), _a(c1,c0), _b(c3), _b(c1) -- doors.c:553-559.
				//
				// The extent IS the quad's width, so the extent argument is what
				// names the quad. c0 and c2 are the thickness edges (measured:
				// mean 13.9 and 14.0 units against 124.2 and 124.1 for c1 and c3,
				// across all 167 leaves in the retail maps, tools/rothdiff/
				// doorgeom.cpp). Therefore:
				//
				//   quad c0, thickness   width c0          picture c1's OWN
				//   quad c2, thickness   width c2          picture c3's OWN
				//   quad c1, broad       width c1's sister picture c1's SISTER
				//   quad c3, broad       width c3's sister picture c3's SISTER
				//
				// That also settles what was flagged UNVERIFIED just above -- that
				// the _b (sister) surface is the outward one. It is: _b is the form
				// whose extent comes from the broad face, and the broad face is
				// what a room sees. "Each broad face carries two skins" was wrong;
				// each broad face carries ONE, and the second _a surface per side
				// is the thickness edge beside it.
				//
				// WHAT WAS WRONG HERE BEFORE: the thickness edges were left blank,
				// on the reading that c0 and c2 "never contribute a texture". They
				// do not contribute one -- they RECEIVE one, from the broad face
				// that follows them in the ring. Blank, they were 10.4% of every
				// door's surface area drawing nothing, and they are exactly what
				// faces the player when a door stands 90 degrees open, which is
				// where it was noticed.
				const bool isLongFace = (j == 1 || j == 3);

				// texTm carries the picture AND the scale flags -- door_corner_tail
				// reads `ch = fs:[si+8]`, the flag byte, off the TEXTURE source's
				// mapping, not the extent source's.
				const roth::TextureMap *texTm = nullptr;
				const roth::TextureMap *extTm = nullptr;
				if (isLongFace)
				{
					int skinFace = fi;
					if (f.sister >= 0 && f.sister < (int)rm.faces.size()) skinFace = f.sister;
					else leafCoplanarRisk++;   // no sister to take the outward skin from
					texTm = extTm = tmOf(rm.faces[skinFace]);
				}
				else
				{
					// c1 for the hinge edge c0, c3 for the latch edge c2: in both
					// cases the broad face at j + 1, and its OWN mapping, not its
					// sister's.
					texTm = tmOf(rm.faces[f0 + ((hingeRel + j + 1) & 3)]);
					extTm = tmOf(f);
				}

				FTextureID tex = texTm ? worldTex(texTm->midTexture) : FNullTextureID();
				if (!tex.isValid())
				{
					leafSidesNoTexture++;
				}
				else
				{
					// The same scale law as an ordinary wall: two world units per
					// texture pixel unless FF_HALF_PIXEL, and the stored extent is
					// authoritative horizontally.
					const double unitsPerTexel =
						(texTm->flags & roth::FF_HALF_PIXEL) ? 1. : 2.;
					const double len = (ld->v2->fPos() - ld->v1->fPos()).Length();
					const double stored = extTm ? double(extTm->StoredExtent()) : 0.;
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
					sd->Flags = WALLF_NOFAKECONTRAST;   // Realms has no fake contrast
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
			// And tell the runtime, so a command that names this door can find
			// the polyobject that swings without searching the tag table.
			roth::RegisterDoor((int)i, tag);

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

		// leafCount, not doorCount: a double door's second panel is a 0xFFFE
		// sector, which IsDoor() excludes but IsDoorCapable() counts.
		log.Line("  door leaves built %d of %d door sectors  (%d closed at load)",
			leavesBuilt, leafCount, doorCount);
		log.Line("  polyobject tags   1 .. %d  (the doorway sector carries the same tag)", leavesBuilt);
		log.Count("doors: leaf side had no artwork", leafSidesNoTexture);
		log.Count("doors: leaf faces coplanar with a drawn wall piece (UNVERIFIED risk)",
			leafCoplanarRisk);
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
	log.Line("  wall horizontal scale %d face(s) scaled by the stored extent, %d unusable",
		storedExtentScaled, storedExtentUnusable);
	log.Line("    of those, %d have an extent that differs from the wall's length"
		" -- the faces the old fixed 0.5 drew at the wrong size", storedExtentDeviates);
	log.Line("  image fit             %d faces; %d PIECES got no vertical fit",
		imageFitFaces, imageFitVerticalUnhandled);
	log.Line("  x-flipped             %d pieces", flippedFaces);
	log.Line("  shifts applied        %d faces", shiftedFaces);
	log.Line("  records the Python's 15-bit extent would have misread  %d",
		extentBitsAbove12);
	log.Line("  door mid pieces handed to a swinging leaf  %d", doorMidToLeaf);
	log.Count("walls: no texture-map record", noTextureMap);
	log.Count("walls: mid texture wanted but absent", midMissing);
	log.Count("walls: FF_IMAGE_FIT vertical fit not expressible", imageFitVerticalUnhandled);
	log.Count("walls: FF_EDGE_MAP faces -- sky above the wall top not modelled", edgeMapFaces);
	log.Count("walls: pieces on the colour key -- left undrawn, as the original does", keyWallPieces);
	log.Line("  FF_TRANS_UPLO mid pieces banded  %d", transUpLoBanded);
	log.Count("walls: FF_TRANS_UPLO faces with no band (opaque or override 0)", transUpLoFaces);
	log.Count("walls: FF_TRANS_UPLO band not exact (art shorter, or spills below)", transUpLoInexact);
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

	// Textures the LOGIC can ask for, which are not the same set as the textures
	// the geometry already wears: opcode 0x34 repaints a wall to an index that may
	// appear nowhere in the map's own mapping records. The runtime cannot resolve
	// one itself because `art` is a local here and dies with the load, so every
	// index a command record names is resolved now and handed over.
	{
		int registered = 0;
		// Whether a flat index takes the 256x256-opaque scale exception. The
		// runtime needs it because the artwork is gone by the time it runs, and
		// it must apply the SAME rule the loader did -- see RegisterFlat.
		auto flatOpaque256For = [&](int fi)
		{
			if (!haveArt) return false;
			int iw = 0, ih = 0; bool trans = false;
			if (!art.ImageShape(fi, iw, ih, trans)) return false;
			return iw == 256 && ih == 256 && !trans;
		};

		for (const roth::Command &c : rm.commands)
		{
			if ((c.opcode & 0x7f) != 0x34) continue;
			FTextureID tex = worldTex((int)c.aux);
			if (!tex.isValid()) continue;
			roth::RegisterTexture((int)c.aux, tex);
			registered++;
		}

		// Opcode 0x0c swaps a whole wall appearance, so all THREE of its slots
		// can name a texture the map wears nowhere. It also names its face by raw
		// offset rather than by id, alone among the geometry opcodes -- counted
		// here so the reading is checked against the retail data.
		int advSlots = 0, advKeysHit = 0, advKeysMiss = 0;
		for (const roth::Command &c : rm.commands)
		{
			if ((c.opcode & 0x7f) != 0x0C) continue;
			if (rm.faceByOffset.count((uint32_t)c.key) != 0) advKeysHit++;
			else advKeysMiss++;
			const int slots[3] = { (int)c.Word(0x0A), (int)c.Word(0x10), (int)c.Word(0x12) };
			for (int k = 0; k < 3; k++)
			{
				FTextureID t = worldTex(slots[k]);
				if (!t.isValid()) continue;
				roth::RegisterTexture(slots[k], t);
				advSlots++;
			}
		}
		if (advKeysHit + advKeysMiss > 0)
			log.Line("  logic walls      0x0c: %d slot(s); keys on a face %d/%d",
				advSlots, advKeysHit, advKeysHit + advKeysMiss);

		// And the FLATS opcodes 0x0a / 0x0b can swap onto a floor or ceiling,
		// which the record names at +0x0a. A flat index may be the pack's sky
		// marker, so that is registered with it rather than resolved to a
		// texture -- the runtime cannot tell the two apart by itself.
		int flats = 0;
		for (const roth::Command &c : rm.commands)
		{
			const uint8_t op = (uint8_t)(c.opcode & 0x7f);
			if (op != 0x0A && op != 0x0B) continue;
			const int fi = (int)c.Word(0x0A);
			const bool isSky = haveArt && art.IsSkySurface(fi);
			roth::RegisterFlat(fi, isSky ? FNullTextureID() : worldTex(fi), isSky, flatOpaque256For(fi));
			flats++;
		}
		// Every flat the GEOMETRY wears, too: a swap puts the sector's previous
		// appearance into the record, and swapping back has to find it again.
		for (const roth::Sector &rs : rm.sectors)
			for (int pass = 0; pass < 2; pass++)
			{
				const int fi = pass == 0 ? rs.floorTexture : rs.ceilingTexture;
				const bool isSky = haveArt && art.IsSkySurface(fi);
				roth::RegisterFlat(fi, isSky ? FNullTextureID() : worldTex(fi), isSky, flatOpaque256For(fi));
			}
		if (flats > 0)
			log.Line("  logic flats      %d record(s) for opcodes 0x0a/0x0b", flats);
		if (registered > 0)
			log.Line("  logic textures   %d index(es) pre-resolved for opcode 0x34", registered);
	}

	// Hand the level's logic to the runtime. Everything it needs -- the resolved
	// keys, the face-to-sidedef pairing, the door tags -- is in place by now.
	roth::BeginLevel(rm, Level, &log);

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
	log.Line("  door leaves are built, hinged and openable by the level logic");
	log.Line("  commands: the active-effect pool and its tick table -- see ROTH_COMMANDS.md");

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
