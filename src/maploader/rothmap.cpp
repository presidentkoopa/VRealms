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

#include "roth/roth_raw.h"
#include "roth/roth_install.h"
#include "roth/roth_log.h"
#include "roth/roth_texture.h"

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

	//----------------------------------------------------------------------
	// Vertices
	//----------------------------------------------------------------------
	log.StageBegin("vertices");
	Level->vertexes.Alloc(rm.vertices.size());
	for (size_t i = 0; i < rm.vertices.size(); i++)
		Level->vertexes[i].set(double(rm.vertices[i].x), double(rm.vertices[i].y));

	// Every vertex needs a matching extra record; the renderer expects one.
	vertexdatas.Clear();
	vertexdatas.Reserve(rm.vertices.size());
	memset(&vertexdatas[0], 0, sizeof(vertexdata_t) * vertexdatas.Size());

	//----------------------------------------------------------------------
	// Sectors
	//----------------------------------------------------------------------
	log.StageBegin("sectors");
	Level->sectors.Alloc(rm.sectors.size());
	Level->extsectors.Alloc(rm.sectors.size());
	memset(&Level->sectors[0], 0, sizeof(sector_t) * Level->sectors.Size());

	int doorCount = 0, flatsToSky = 0, flatFlipsIgnored = 0;
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

		// A door is stored at full height and closed by the engine at load.
		// Without this every door in the game stands permanently open.
		double floorZ = double(rs.floorHeight);
		double ceilZ = double(rs.IsDoor() ? rs.floorHeight : rs.ceilingHeight);
		if (rs.IsDoor()) doorCount++;

		sec->SetPlaneTexZ(sector_t::floor, floorZ);
		sec->SetPlaneTexZ(sector_t::ceiling, ceilZ);
		sec->floorplane.set(0., 0., 1., -floorZ);
		sec->ceilingplane.set(0., 0., -1., ceilZ);

		// Realms' light byte is centred on 0x80. Mapping is provisional until
		// the shade tables are used properly; see the handoff, section 5.7.
		sec->lightlevel = (short)clamp<int>(rs.light * 2, 0, 255);

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
	log.Line("  doors closed at load  %d", doorCount);
	log.Line("  flats with no art -> sky  %d", flatsToSky);
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

	Level->lines.Alloc(pending.size());
	Level->sides.Alloc(pending.size() + twoSided);   // one per original face
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
	int edgeMapFaces = 0;        // FF_EDGE_MAP parallax sky: not handled
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

		auto makeSide = [&](const roth::Face &face, bool twoSided) -> side_t *
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

			// The piece height, where it is well defined. Only a one-sided wall
			// has one: its mid piece spans the whole sector. An upper or lower
			// piece depends on the neighbour's planes, which Doom resolves at
			// draw time, so a vertical fit cannot be expressed for those.
			const sector_t *ownSec = &Level->sectors[face.sector];
			const double pieceHeight = twoSided ? 0.
				: ownSec->GetPlaneTexZ(sector_t::ceiling) - ownSec->GetPlaneTexZ(sector_t::floor);

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

			// Flags that do something the loader does not yet do, counted so
			// they stay visible. FF_EDGE_MAP is the parallax sky above a wall
			// (renderer.c:9020); FF_TRANS_UPLO clips the mid piece to a band.
			if (tf & roth::FF_EDGE_MAP) edgeMapFaces++;
			if (tf & roth::FF_TRANS_UPLO) transUpLoFaces++;
			return sd;
		};

		ld->sidedef[0] = makeSide(f, pending[li].sister >= 0);
		if (pending[li].sister >= 0)
		{
			ld->sidedef[1] = makeSide(rm.faces[pending[li].sister], true);
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
	log.Count("walls: FF_EDGE_MAP parallax sky not handled", edgeMapFaces);
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

	log.Line("  player start    (%d, %d) facing %d",
		rm.metadata.startX, rm.metadata.startY,
		(int)(90 + rm.metadata.rotation * 360.0 / 512.0) % 360);
	log.Count("objects not yet spawned", objectCount);

	log.StageEnd();
	log.Section("Not yet handled");
	log.Line("  sprites and model skins         -- stage 4 (walls and flats done)");
	log.Line("  objects and 3D props            -- stage 4");
	log.Line("  mid-platforms as 3D floors      -- stage 5");
	log.Line("  door logic and lighting         -- stage 6");
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
