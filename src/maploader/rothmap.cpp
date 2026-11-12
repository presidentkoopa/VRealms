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
#include <map>   // the door-leaf skin tally in the load report
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
#include "hw_vrmodes.h"   // VR_SetUnitsPerMeterOverride
#include "roth/roth_runtime.h"
#include "roth/roth_interface.h"
#include "c_cvars.h"
#include "roth_portalplan.h"   // rooms over rooms: groups, cuts, the translation
#include "roth/roth_platformplan.h"  // moving sectors: which become platform actors
#include "roth/roth_platforms.h"
#include "playsim/portal.h"    // FLinePortal, PORTT_LINKED

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

// One line per wall in the load report, so an engine line index from rothdiff
// can be turned back into the Realms face it was built from. NOT archived: it
// is a diagnostic for one load, and 1,908 extra lines in every report is not
// something anyone should inherit by accident.
CVAR(Bool, roth_logwalls, false, CVAR_NOINITCALL)

// A comma list of Realms sector numbers to leave out of the geometry, so that
// "is this the geometry that breaks the picture" can be answered by rendering
// the same pose with and without it instead of by argument. Not archived: a
// build with sectors missing is a test, never a load anyone should keep.
CVAR(String, roth_skip_sectors, "", CVAR_NOINITCALL)

// "sector=height" pairs, comma separated, forcing a sector's CEILING. The
// sibling of roth_skip_sectors: skipping answers "is this geometry the
// problem", and this answers "is it the geometry or the 2-unit band on it",
// which skipping alone cannot separate. Not archived, same reason.
CVAR(String, roth_sector_ceiling, "", CVAR_NOINITCALL)

// ROOMS OVER ROOMS, REPAIRED WITH LINKED PORTALS. ON.
//
// 1: every stack of sectors that overlaps in plan is separated -- the smaller
// side is moved to a region of its own -- and each face the move cut is rebuilt
// as a pair of lines joined by a linked line portal, so the two regions render,
// collide and trace as if they were still stacked. The plan (which sectors,
// which faces) is made by roth_portalplan.h, which the offline node-builder
// harness runs too. 0: the map is built exactly as before.
//
// Each half of a cut face is a ONE-SIDED line carrying a linked portal: the
// engine takes what is behind it from the far line's front sector, live
// (line_t::getPortalBeyondSector, r_defs.h). There are no buffer sectors.
//
// On since 2026-11-12. With it off, the 11 retail maps that stack rooms lose
// floor and ceiling in 54 sectors (STUDY1's upper corridor among them) and the
// far walls behind them; with it on the offline node-builder check finds no
// sector short of its floor in any of the 44 maps (tools/portalplan/allmaps.sh).
// 0 is kept for comparison shots, not for play. Not archived. Applied at load;
// change it and reload the map.
CVAR(Bool, roth_portals, true, CVAR_NOINITCALL)

// A door leaf is a polyobject, and a polyobject exists in one region of the
// map only: the part of a leaf that swings through a portal line is neither
// drawn nor solid beyond it. So by default (0) the planner never cuts a face a
// leaf can reach, and leaves a stack alone if it cannot be separated otherwise.
// 1 lets such a cut be made (and names it in the load report) -- for looking at
// what a leaf at a seam does, not for playing. Applied at load, with roth_portals.
CVAR(Bool, roth_portals_doorcuts, false, CVAR_NOINITCALL)

// Moving sectors as solid platform actors.
//
// RAW opcode 0x09 slides a sector sideways by moving its vertices, which a
// static BSP cannot do. 1: every moving sector that roth_platformplan.h can
// show as "a slab or a block travelling along a level channel" is taken out of
// the static map and built as a platform actor instead (roth/roth_platforms.h;
// the platform physics are FishyClock's "3D Platform", GPL-3.0), and the level
// logic runs the original's 0x09 tick to drive it. 0: the map is built exactly
// as before and 0x09 stays unimplemented and counted.
//
// Not archived, for the same reason as roth_portals: nothing behind it has
// been compiled into a running engine by the lane that wrote it, let alone
// seen in a headset. Applied at load; change it and reload the map.
CVAR(Bool, roth_platforms, false, CVAR_NOINITCALL)

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
// PLAN OVERLAP -- rooms stacked over rooms.
//
// Realms draws by walking from the camera's sector through faces into the
// neighbour each one names (ROTH.C renderer.c:7113 walk_visible_sectors,
// "Mode 0: portal walk"; :7319 build_sector_render_tree_recursive, which takes
// the neighbour straight from the face's sister at +8). Nothing in that walk
// asks where a sector lies in plan, so two sectors may occupy the same x,y at
// different heights and the retail maps do exactly that: STUDY1's upper
// corridor (sectors 368/404/405, z 400..720) lies directly over the ground-floor
// hall (358/359/360/380, z 60..380/460).
//
// A Doom BSP gives every point of the plan to exactly ONE subsector, and so to
// one sector. Where two sectors overlap, one of them cannot have its floor and
// ceiling there at all, whatever the node builder does. MEASURED, by linking
// this tree's own FNodeBuilder standalone against the loader's line
// construction for all 44 retail maps (2026-11-12): 54 sectors come out of the
// build with less subsector area than their outline; ALL 54 are plan-overlapped,
// none of the 547 sectors thinner than 4 units is among them, and the 33 maps
// with no overlap lose nothing. That is the "black wedges in the floor and
// ceiling" of RENDERING_NEXT_STEP.md -- not the 2-unit sectors 56/57, which
// each build into one exact 4-seg subsector; leaving them out only reshuffles
// which splitters are picked and so where the holes fall.
//
// This function only FINDS the overlaps so the load report can name them. It
// does not repair anything: that needs the storeys moved apart and joined by
// line portals, or folded into 3D floors, and is not attempted here.
//
// Sampled, not exact: each sector is probed on a lattice (4 units, finer for a
// narrow one) at points that can never lie on an integer edge, with an even-odd
// test against its own outline and then against every other sector whose
// bounding box it touches. A hit is a real point inside both outlines; the area
// is hits times the lattice cell, so treat it as an estimate.
//
//==========================================================================
namespace
{

struct RothPlanOverlap
{
	int a, b;        // Realms sector indices, a < b
	double area;     // estimated shared plan area, square map units
};

struct RothOutline
{
	std::vector<double> x1, y1, x2, y2;
	double minx = 0., miny = 0., maxx = 0., maxy = 0.;
	bool usable = false;
};

bool RothOutlineHolds(const RothOutline &o, double x, double y)
{
	if (x < o.minx || x > o.maxx || y < o.miny || y > o.maxy) return false;
	bool in = false;
	for (size_t i = 0; i < o.x1.size(); i++)
	{
		if ((o.y1[i] > y) != (o.y2[i] > y))
		{
			const double xi = o.x1[i] + (y - o.y1[i]) * (o.x2[i] - o.x1[i]) / (o.y2[i] - o.y1[i]);
			if (xi > x) in = !in;
		}
	}
	return in;
}

void RothFindPlanOverlaps(const roth::Map &rm, const std::vector<int> &skipSectors,
	std::vector<RothPlanOverlap> &out)
{
	const int ns = (int)rm.sectors.size();
	std::vector<RothOutline> outline((size_t)ns);
	for (int s = 0; s < ns; s++)
	{
		bool skipped = false;
		for (int sx : skipSectors) if (sx == s) skipped = true;
		const roth::Sector &rs = rm.sectors[s];
		if (skipped || rs.faceCount < 3 || rs.firstFaceIndex < 0
			|| rs.firstFaceIndex + rs.faceCount > (int)rm.faces.size()) continue;
		RothOutline &o = outline[s];
		bool first = true, bad = false;
		for (int k = 0; k < rs.faceCount; k++)
		{
			const roth::Face &f = rm.faces[rs.firstFaceIndex + k];
			if (f.vertex1 < 0 || f.vertex2 < 0 || f.vertex1 >= (int)rm.vertices.size()
				|| f.vertex2 >= (int)rm.vertices.size()) { bad = true; break; }
			const double ax = rm.vertices[f.vertex1].x, ay = rm.vertices[f.vertex1].y;
			const double bx = rm.vertices[f.vertex2].x, by = rm.vertices[f.vertex2].y;
			o.x1.push_back(ax); o.y1.push_back(ay); o.x2.push_back(bx); o.y2.push_back(by);
			if (first) { o.minx = o.maxx = ax; o.miny = o.maxy = ay; first = false; }
			o.minx = ax < o.minx ? ax : o.minx;  o.maxx = ax > o.maxx ? ax : o.maxx;
			o.minx = bx < o.minx ? bx : o.minx;  o.maxx = bx > o.maxx ? bx : o.maxx;
			o.miny = ay < o.miny ? ay : o.miny;  o.maxy = ay > o.maxy ? ay : o.maxy;
			o.miny = by < o.miny ? by : o.miny;  o.maxy = by > o.maxy ? by : o.maxy;
		}
		o.usable = !bad && o.maxx > o.minx && o.maxy > o.miny;
	}

	// Each member of a pair probes it on its own lattice; first = the estimate
	// from the lower-numbered sector's probes, second = from the higher one's.
	std::map<std::pair<int, int>, std::pair<double, double>> found;
	std::vector<int> nearby;
	for (int s = 0; s < ns; s++)
	{
		const RothOutline &o = outline[s];
		if (!o.usable) continue;
		nearby.clear();
		for (int t = 0; t < ns; t++)
		{
			const RothOutline &p = outline[t];
			if (t == s || !p.usable) continue;
			if (p.maxx <= o.minx || p.minx >= o.maxx || p.maxy <= o.miny || p.miny >= o.maxy) continue;
			nearby.push_back(t);
		}
		if (nearby.empty()) continue;

		// 4 units, or a third of the narrow side for a sliver, never under 1.
		double step = 4.;
		const double narrow = (o.maxx - o.minx) < (o.maxy - o.miny) ? (o.maxx - o.minx) : (o.maxy - o.miny);
		if (narrow / 3. < step) step = narrow / 3.;
		if (step < 1.) step = 1.;
		// Offsets that are not multiples of anything a vertex can be, so no
		// probe sits on an edge and the even-odd test is never ambiguous.
		for (double y = o.miny + step * 0.4371; y < o.maxy; y += step)
		{
			for (double x = o.minx + step * 0.3913; x < o.maxx; x += step)
			{
				if (!RothOutlineHolds(o, x, y)) continue;
				for (int t : nearby)
				{
					if (!RothOutlineHolds(outline[t], x, y)) continue;
					std::pair<double, double> &est =
						found[std::pair<int, int>(s < t ? s : t, s < t ? t : s)];
					(s < t ? est.first : est.second) += step * step;
				}
			}
		}
	}
	for (const auto &kv : found)
	{
		RothPlanOverlap po;
		po.a = kv.first.first;
		po.b = kv.first.second;
		po.area = kv.second.first > kv.second.second ? kv.second.first : kv.second.second;
		out.push_back(po);
	}
}

} // namespace

//==========================================================================
//
// One Realms world unit is one map unit; the geometry is used as authored.
// Apparent scale problems are a player-size question, not a world one: the
// original doubles the map's recorded player height at load, so a player built
// from the raw value makes the whole world read as twice its intended size.
//
//==========================================================================

// One copy of a picture across a span, for DOOR LEAVES only (the wall loop now
// goes through roth_surface's WallToEngine and does not use this). The engine
// measures the span rounded to whole units, and the last texel must not wrap.
// Seen on screen 2026-11-12 against the original's frames (doors tag 1, 3, 5).
static double OneCopyScale(double texels, double span, bool mirrored)
{
	const double whole = floor(span + 0.5);
	if (whole < 1.) return texels / span;
	return texels / (whole + (mirrored ? 0.5 : -0.5));
}

void MapLoader::LoadRothMap(MapData *map, FMissingTextureTracker &missingtex)
{
	auto &log = roth::TheLog();

	// ONE REPORT PER REAL LOAD, OPENED HERE AND NOWHERE ELSE.
	//
	// Loading a foreign format off someone else's disk fails in ways that are
	// hard to diagnose after the fact, so every load writes a full report.
	// This is the first point reached ONLY when the map is really being
	// loaded, which is why the report starts here instead of in P_IsRothMap:
	// P_OpenMapData is also how the engine asks whether a map merely EXISTS
	// (P_CheckMapData, from menus, MAPINFO redirects and the secret-exit
	// lookup), and Log::Begin opens with "w". Opening it there stamped a 0 ms
	// "(nothing counted)" report over every map that was asked about -- one
	// run produced a stub for all 44 -- and destroyed the real report whenever
	// a probe arrived after the load. That is why the per-map report could not
	// be trusted as written.
	auto &install = roth::TheInstall();
	log.Begin(Level->MapName.GetChars());
	log.Section("Source");
	log.Line("  install   %s", install.Path().c_str());
	log.Line("  map file  %s", map->rothFile.GetChars());
	log.Line("  artwork   %s  (shared: %s)", map->rothPack.GetChars(),
		install.SharedPack().c_str());

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

	// THE HEIGHTS THE MAP STARTS WITH. The original writes each 0x07 record's
	// first limit into its sectors at load (roth_raw.h, ApplyLoadTimeHeights;
	// ROTH.C raw_commands.c:5243, map_load.c:1183-1196). Done here, to the map
	// data, before one plane, wall piece or texture offset is made from it.
	{
		std::vector<roth::LoadHeightChange> moved;
		const int n = roth::ApplyLoadTimeHeights(rm, &moved);
		log.Section("Heights set at load by 0x07 records");
		log.Line("  sector heights changed  %d", n);
		for (const roth::LoadHeightChange &ch : moved)
			log.Line("    sector %-4d %s %d -> %d  (command %d)", ch.sector,
				ch.ceiling ? "ceiling" : "floor  ", (int)ch.from, (int)ch.to, ch.command);
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

		// THE COLLISION HEIGHT IS g_min_fit, NOT THE EYE HEIGHT. They are two
		// different numbers in the original and this used to use one for both.
		//
		// What the original actually tests to let you through an opening is a
		// SPAN THRESHOLD: move_player_with_collision loads it from g_min_fit
		// (E/collision_physics.c:1693) and every passage check is the same
		// shape -- "if (ceiling - floor) < span, blocked" (:1048, and the
		// portal tests at :604-633). g_min_fit is 2*meta[0x0e] = 96, while the
		// eye sits at 144. So a Realms player walks through a 96-unit gap
		// standing up; he does not have to be 96 units tall to do it.
		//
		// The old `ph + 10` was not a Realms measurement at all -- it was the
		// eye height plus a guess -- and using it as the collision height made
		// every opening between 96 and 154 impassable that the original walks
		// straight through.
		//
		// The original ALSO squashes the player under a low ceiling rather than
		// stopping him: g_player_height is dynamic, clamped to (ceiling - pos)
		// and eased back toward its resting target at 4 a tic (E/player.c:313,
		// :318-326). GZDoom's own viewz clamp in P_CalcHeight gives the same
		// duck for free once the collision height is the span.
		Level->ForcedPlayerHeight = double(rm.metadata.MinFit());
		log.Line("  player size    %.0f eye, %.0f collision (Doom's is 41 / 56)"
			" -- collision is g_min_fit = 2*meta[0x0e], the span the original"
			" tests to let you through", Level->ForcedPlayerViewHeight,
			Level->ForcedPlayerHeight);

		// AND THE STAIRS, which is the same fact a third time. A person 154
		// units tall has stairs built to match; Doom's step-up limit is 24 and
		// the map's own is g_max_climb = 2*meta[0x0C]+1 (GAME_core.md 2.1,
		// :208). Left at 24 the manor's staircases simply cannot be walked up
		// -- the geometry is right and the player is wrong, exactly as with the
		// height above. ROADMAP.md section 2 puts step height on the "these are
		// rules, port them" side of the movement split.
		//
		// NOT ported here: the crouched limit, which the original drops to 16
		// (GAME_core.md 5, 0x819cd). That needs the crouch state, so it belongs
		// with the rest of the movement rules rather than in the loader.
		Level->ForcedPlayerMaxStepHeight = double(rm.metadata.MaxClimb());
		log.Line("  step up        %.0f units (Doom's default is 24) -- map says"
			" meta[0x0c]=%d, doubled +1", Level->ForcedPlayerMaxStepHeight,
			(int)rm.metadata.maxClimb);

		// AND HOW WIDE HE IS. Not from the map: the original hardcodes the
		// player's collision dimensions, unlike height and climb which the map
		// states. move_player_with_collision (E/collision_physics.c:1687-1692)
		// writes the whole block as fixed player dims.
		//
		// IT IS 0x1c = 28, NOT the 0x70 that docs/GAME_core.md 3.3 quotes as
		// "radius". 0x70 is the derived 4r. The same function builds every
		// other entity's block from one radius r (:1592-1600) as
		//
		//     box_max = r,  box_min = -r,  radius = 4r,
		//     radius_sq = 64r^2,  radius_sq+4 = 65r^2
		//
		// and the player's hardcoded values satisfy all five at r = 28:
		// 0x1c = 28, 0x70 = 4*28, 0xc400 = 64*28^2, 0xc710 = 65*28^2. Setting
		// the radius to 0x70 would make him four times too wide.
		//
		// No doubling: that applies to the three METADATA fields at map_load.c
		// :206-209, and this is not one of them.
		const double ROTH_PLAYER_RADIUS = 28.;
		Level->ForcedPlayerRadius = ROTH_PLAYER_RADIUS;
		log.Line("  radius         %.0f units (Doom's default is 16) -- fixed by"
			" the original at 0x1c; 0x70 is its 4r", Level->ForcedPlayerRadius);

		// AND HOW FAR HE CAN REACH. PROVISIONAL -- read the next paragraph
		// before changing it, and before trusting it.
		//
		// The original gates "use" on the renderer's PICK DEPTH, not on world
		// distance: < 0x258 (600) is use range, < 0x7d0 is the "too far"
		// cursor, < 0x1388 is the examine probe (E/input.c:1031, :1112-1258;
		// docs/GAME_core.md 4.4). Those depth units are marked [open] in the
		// spec, which says in as many words that the 600 has to be recalibrated
		// to world units by measuring in ROTH.C. GAME_core.md 5.2 step 8 is the
		// measurement: find the depth where the cursor changes, confirm 599
		// fires and 601 does not.
		//
		// THE 600 BELOW IS THAT BAND NUMBER READ AS WORLD UNITS. It is NOT the
		// calibrated answer and must not be quoted as one. It is here because
		// the alternative is worse: Doom's 64 is a 56-unit player's arm, and at
		// 93.5 units to the metre it gives this 154-unit player 0.68 m of
		// reach, while UseFlat and the hand pick BOTH read this field and the
		// hand pick is documented to let the beam "cross rooms". 600 is 6.4 m,
		// which does. Reach is interface, which ROTH_BETTER.md 13 leaves free;
		// WHICH records a press may fire is logic, and that is untouched.
		const double ROTH_USE_RANGE_PROVISIONAL = 600.;
		Level->ForcedPlayerUseRange = ROTH_USE_RANGE_PROVISIONAL;
		log.Line("  use range      %.0f units PROVISIONAL (Doom's default is 64"
			" = 0.68 m here) -- the original's band is a PICK DEPTH of 600,"
			" units still uncalibrated: GAME_core.md 5.2 step 8",
			Level->ForcedPlayerUseRange);

		// AND THE SAME FACT AGAIN, FOR THE HEADSET. Making the player the right
		// number of UNITS tall fixes the flat-screen view, and does nothing at
		// all in VR: there the world's size is set by how many units the engine
		// calls a metre, and vr_vunits_per_meter is 34 because Doom's player is
		// 56 units. Left alone, a 154-unit player stands 4.5 m tall and the
		// manor reads as giant from inside the headset -- the same complaint the
		// comment above is about, in the one place the fix above cannot reach.
		//
		// Derived, not tuned: keep the player's REAL-WORLD height what the cvar
		// already implies for Doom, and scale by how much bigger Realms' units
		// are. 34 * 154/56 = 93.5, which puts the eye at 144 units = 1.54 m.
		//
		// NOT the cvar itself. It is CVAR_ARCHIVE | CVAR_GLOBALCONFIG -- writing
		// it would resize every other game this engine runs, permanently, from
		// loading one Realms map. The override is per level and is cleared by
		// P_SetupLevel. See VR_UnitsPerMeter in hw_vrmodes.h.
		// FROM THE STANDING HEIGHT, NOT ForcedPlayerHeight. Those were the same
		// number until the collision height became g_min_fit (96) above, and
		// reading the field here would quietly rescale the whole world from
		// 93.5 to 58.3 units a metre -- the headset's idea of how big the manor
		// is, changed by a collision fix. How tall the player STANDS is the
		// human-scale anchor, so it is spelled out here and the two uses can no
		// longer drift into each other.
		const double standingHeight = ph + 10.;
		const double DOOM_PLAYER_HEIGHT = 56.0, DOOM_UNITS_PER_METRE = 34.0;
		const double upm = DOOM_UNITS_PER_METRE
			* (standingHeight / DOOM_PLAYER_HEIGHT);
		VR_SetUnitsPerMeterOverride((float)upm);
		log.Line("  VR world scale %.1f units/metre (Realms; the cvar's %.0f is"
			" Doom's, and is untouched)", upm, DOOM_UNITS_PER_METRE);
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
	// And the previous level's platform pieces, latching the cvar for this load.
	roth::ResetPlatforms(roth_platforms);

	log.Section("Artwork");

	roth::TextureSet art;
	bool haveArt = false;
	{
		std::string packFile = roth::TheInstall().PackFile(map->rothPack.GetChars());
		log.Line("  pack file      %s", packFile.c_str());
		haveArt = art.Open(map->rothPack.GetChars(), packFile, &log);
		// The dialogue and inventory panels draw in this map's palette.
		if (haveArt) roth::InterfaceSetPalette(art);
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
		// NAMED AND MEASURED, not just "there is one". A sky that resolves to a
		// black or degenerate picture is indistinguishable from a hole: every
		// sky surface in the map, floor and ceiling alike, draws it.
		if (auto *gt = TexMan.GetGameTexture(skyTex))
			log.Line("  sky image      stored index %d -> '%s'  %dx%d",
				rm.metadata.skyTexture, gt->GetName().GetChars(),
				(int)gt->GetDisplayWidth(), (int)gt->GetDisplayHeight());
		else
			log.Line("  sky image      stored index %d -> NO GAME TEXTURE",
				rm.metadata.skyTexture);
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
	//
	// roth_platforms: a slab that belongs to a MOVING sector (RAW opcode 0x09)
	// is not a 3D floor at all in that mode -- it becomes a platform actor, built
	// further down -- so it is planned first and left out of this count. The
	// plan is made from the map data alone (roth_platformplan.h), which is why
	// it can be made this early.
	std::vector<rothplat::Plan> movePlans;
	std::vector<char> slabIsActor(rm.sectors.size(), 0);
	if (roth_platforms)
	{
		movePlans = rothplat::PlanMovingSectors(rm);
		for (const rothplat::Plan &mpn : movePlans)
		{
			if (!mpn.supported) continue;
			for (const rothplat::Piece &pc : mpn.pieces)
				if (pc.kind == rothplat::PIECE_SLAB) slabIsActor[mpn.sector] = 1;
		}
	}

	int platformCount = 0;
	for (size_t si = 0; si < rm.sectors.size(); si++)
	{
		const roth::Sector &rs = rm.sectors[si];
		if (rs.platformIndex < 0 || rs.platformIndex >= (int)rm.platforms.size()) continue;
		if (slabIsActor[si]) continue;
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

	//----------------------------------------------------------------------
	// Rooms over rooms (roth_portals). Planned HERE, before anything is
	// allocated, because the arrays below are allocated once and exactly, and
	// the plan adds to the vertices (moved copies) and changes how many lines
	// and sides the faces make (a cut pair is two one-sided lines instead of
	// one two-sided line). The vertices it adds go at the very END of the
	// array, after the slab controls and the door leaves, so no index the
	// rest of this function computes moves. It adds no sectors.
	//----------------------------------------------------------------------
	rothplan::Plan portalPlan;
	portalPlan.group.assign(rm.sectors.size(), 0);
	if (roth_portals)
	{
		log.StageBegin("portal plan");
		std::vector<int> planSkip;
		for (const char *sk = roth_skip_sectors; sk != nullptr && *sk != 0; )
		{
			while (*sk == ' ' || *sk == ',') sk++;
			if (*sk == 0) break;
			planSkip.push_back(atoi(sk));
			while (*sk != 0 && *sk != ',') sk++;
		}
		std::vector<RothPlanOverlap> found;
		RothFindPlanOverlaps(rm, planSkip, found);
		std::vector<rothplan::OverlapPair> pairsIn;
		for (const RothPlanOverlap &po : found) pairsIn.push_back({ po.a, po.b, po.area });
		portalPlan = rothplan::BuildPlan(rm, pairsIn, planSkip,
			(int)(rm.vertices.size() + size_t(platformCount) * 4 + size_t(leafCount) * 8),
			0., !roth_portals_doorcuts);

		log.Section("Portals (roth_portals 1)");
		for (const std::string &n : portalPlan.notes) log.Line("  %s", n.c_str());
		if (!portalPlan.active)
		{
			log.Line("  nothing separated: the map is built as it always was");
		}
		else
		{
			for (int g = 1; g < portalPlan.numGroups; g++)
			{
				FString members;
				int count = 0;
				for (size_t si = 0; si < rm.sectors.size(); si++)
					if (portalPlan.group[si] == g)
					{
						if (count++ < 64) members.AppendFormat("%d ", (int)si);
						roth::RegisterSectorShift((int)si, portalPlan.offX[g], portalPlan.offY[g]);
					}
				log.Line("  group %d: %d sector(s) moved by (%.0f, %.0f): %s%s", g, count,
					portalPlan.offX[g], portalPlan.offY[g], members.GetChars(),
					count > 64 ? "..." : "");
			}
			log.Line("  cut face pairs %d (each a pair of one-sided linked portal lines;"
				" no buffer sectors), moved vertices %d",
				(int)portalPlan.cuts.size(), (int)portalPlan.extraVerts.size());
			log.Count("portals: cuts within reach of a door leaf (roth_portals_doorcuts 1)",
				portalPlan.doorReachCuts);
			log.Count("portals: cut faces that paid a hazard penalty (door or slab"
				" nearby, trigger face, short face)", portalPlan.penalisedCuts);
			log.Count("portals: overlap stacks that could not be separated",
				portalPlan.unresolvedStacks);
		}
	}
	const size_t planVerts = portalPlan.active ? portalPlan.extraVerts.size() : 0;
	const size_t planCuts = portalPlan.active ? portalPlan.cuts.size() : 0;
	const unsigned planVertexBase = (unsigned)(rm.vertices.size() + size_t(platformCount) * 4
		+ size_t(leafCount) * 8);

	log.StageBegin("vertices");
	Level->vertexes.Alloc(rm.vertices.size() + size_t(platformCount) * 4
		+ size_t(leafCount) * 8 + planVerts);
	for (size_t i = 0; i < planVerts; i++)
		Level->vertexes[planVertexBase + (unsigned)i].set(portalPlan.extraVerts[i].x,
			portalPlan.extraVerts[i].y);
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

		// roth_sector_ceiling overrides, for geometry bisection only.
		{
			const char *s = roth_sector_ceiling;
			while (s != nullptr && *s != 0)
			{
				while (*s == ' ' || *s == ',') s++;
				if (*s == 0) break;
				const int which = atoi(s);
				const char *eq = strchr(s, '=');
				if (eq != nullptr && which == (int)i)
				{
					ceilZ = atof(eq + 1);
					log.Line("  SECTOR CEILING OVERRIDE  sector %d -> %.0f"
						" (was %d) -- geometry test, not a shippable load",
						(int)i, ceilZ, (int)rs.ceilingHeight);
				}
				while (*s != 0 && *s != ',') s++;
			}
		}
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

			// WHICH SECTORS, not just how many. A sky flat indoors is how a
			// black hole appears in a floor or a ceiling: the surface resolves
			// to the sky, and whatever the sky picture is gets drawn there --
			// solid black if it is dark or unset. Naming the sector turns "the
			// ceiling has a hole in it" into a lookup.
			if (isSky)
				log.Line("  sky flat       sector %d %s", (int)i,
					which == sector_t::ceiling ? "ceiling" : "floor");

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

	// EVERY BROKEN SECTOR AS THE ENGINE ENDED UP WITH IT, when roth_logwalls
	// is on. The wall dump prints the REALMS heights a line was built from;
	// this prints what the engine will actually render, which is the only
	// thing that can explain looking through a two-sided line into nothing. A
	// sector whose ceiling is not above its floor, or whose flats resolved to
	// no texture, draws nothing at all -- and through a portal that is
	// indistinguishable from a hole in the world.
	if (roth_logwalls)
	{
		int broken = 0;
		for (unsigned si = 0; si < rm.sectors.size() && si < Level->sectors.Size(); si++)
		{
			sector_t &es = Level->sectors[si];
			const double ef = es.floorplane.ZatPoint(es.centerspot);
			const double ec = es.ceilingplane.ZatPoint(es.centerspot);
			const bool degenerate = ec <= ef;
			const bool noFloor = !es.GetTexture(sector_t::floor).isValid();
			const bool noCeil = !es.GetTexture(sector_t::ceiling).isValid();
			if (degenerate || noFloor || noCeil) broken++;
			log.Line("  sector %-4d engine z %.0f..%.0f%s%s%s  (roth z %d..%d)",
				(int)si, ef, ec,
				degenerate ? "  DEGENERATE" : "",
				noFloor ? "  NO FLOOR TEX" : "",
				noCeil ? "  NO CEILING TEX" : "",
				(int)rm.sectors[si].floorHeight,
				(int)rm.sectors[si].ceilingHeight);
		}
		log.Line("  sectors that render nothing: %d of %d", broken,
			(int)rm.sectors.size());
	}

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
	// portalPeer: set on the two halves of a face pair the portal plan cut.
	// Each half is then a ONE-SIDED line of its own, in its own group's space,
	// whose far side is reached through the linked portal that joins it to
	// the other half. Its front side is still built against the real
	// neighbour (portalPeer's sector), so it has that wall's upper and lower
	// pieces; the engine reads the neighbour's heights through the portal.
	struct PendingLine { int face, sister; int portalPeer = -1; };
	std::vector<PendingLine> pending;
	pending.reserve(rm.faces.size());
	std::vector<std::pair<int, int>> portalPending;   // the two line indices of each cut

	// BISECTING GEOMETRY BY SECTOR. roth_skip_sectors is a comma list of Realms
	// sector numbers whose faces are left out of the build entirely, on either
	// side. It exists because "is this geometry what is breaking the picture"
	// is otherwise answered by argument, and on this map it has been answered
	// wrongly more than once: the only honest test is to remove the suspect and
	// render the same pose again. Empty by default and not archived.
	std::vector<int> skipSectors;
	{
		const char *s = roth_skip_sectors;
		while (s != nullptr && *s != 0)
		{
			while (*s == ' ' || *s == ',') s++;
			if (*s == 0) break;
			skipSectors.push_back(atoi(s));
			while (*s != 0 && *s != ',') s++;
		}
		if (!skipSectors.empty())
		{
			FString list;
			for (int sx : skipSectors) list.AppendFormat("%d ", sx);
			log.Line("  SKIPPING SECTORS  %s -- this build is a geometry test,"
				" not a shippable load", list.GetChars());
		}
	}
	auto sectorSkipped = [&](int sec)
	{
		for (int sx : skipSectors) if (sx == sec) return true;
		return false;
	};

	int twoSided = 0, oneSided = 0, orphaned = 0, skippedFaces = 0;
	for (size_t i = 0; i < rm.faces.size(); i++)
	{
		if (consumed[i]) continue;
		const roth::Face &f = rm.faces[i];

		if (!skipSectors.empty()
			&& (sectorSkipped(f.sector)
				|| (f.sister >= 0 && sectorSkipped(rm.faces[f.sister].sector))))
		{
			skippedFaces++;
			consumed[i] = true;
			if (f.sister >= 0) consumed[f.sister] = true;
			continue;
		}

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
			auto cutIt = portalPlan.active ? portalPlan.cutByFace.find((int)i)
				: portalPlan.cutByFace.end();
			if (cutIt != portalPlan.cutByFace.end()
				&& portalPlan.cuts[cutIt->second].face == (int)i
				&& portalPlan.cuts[cutIt->second].sister == sister)
			{
				PendingLine a = { (int)i, -1 };
				a.portalPeer = sister;
				PendingLine b = { sister, -1 };
				b.portalPeer = (int)i;
				portalPending.push_back({ (int)pending.size(), (int)pending.size() + 1 });
				pending.push_back(a);
				pending.push_back(b);
			}
			else
			{
				pending.push_back({ (int)i, sister });
			}
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
	if (skippedFaces > 0)
		log.Line("  faces left out by roth_skip_sectors  %d", skippedFaces);

	//----------------------------------------------------------------------
	// ROOMS OVER ROOMS, NAMED. See RothFindPlanOverlaps above for why a Doom
	// BSP cannot hold two sectors in the same plan position and what was
	// measured. This only reports; the geometry below is built exactly as
	// before. Sectors left out by roth_skip_sectors are not counted, so the
	// report reads zero once a test build has removed one of the storeys.
	//
	// THE TEST THIS MAKES ONE COMMAND: the two lists are ready to paste into
	// roth_skip_sectors. In STUDY1, leaving out the lower list
	// (358,359,360,380) gives the upper corridor back its whole floor and
	// ceiling -- the standalone node-builder run puts sectors 368, 404 and 405
	// at 100% of their plan against 38%, 72% and 35% with everything present
	// -- at the cost of the hall underneath, which is why it is a test and not
	// a fix.
	//----------------------------------------------------------------------
	{
		std::vector<RothPlanOverlap> overlaps;
		RothFindPlanOverlaps(rm, skipSectors, overlaps);
		if (!overlaps.empty())
		{
			// A sector is "lower" when it is the lower-floored member of every
			// pair it is in, "upper" when it is the higher one in every pair;
			// a sector that is both (three storeys) goes in neither list.
			std::vector<char> isLower(rm.sectors.size(), 0), isUpper(rm.sectors.size(), 0);
			for (const RothPlanOverlap &po : overlaps)
			{
				const bool aLower = rm.sectors[po.a].floorHeight <= rm.sectors[po.b].floorHeight;
				isLower[aLower ? po.a : po.b] = 1;
				isUpper[aLower ? po.b : po.a] = 1;
			}
			int members = 0;
			FString lowerList, upperList;
			for (size_t si = 0; si < rm.sectors.size(); si++)
			{
				if (!isLower[si] && !isUpper[si]) continue;
				members++;
				if (isLower[si] && !isUpper[si])
					lowerList.AppendFormat("%s%d", lowerList.IsEmpty() ? "" : ",", (int)si);
				if (isUpper[si] && !isLower[si])
					upperList.AppendFormat("%s%d", upperList.IsEmpty() ? "" : ",", (int)si);
			}
			log.Warn("%d sectors share plan area with another sector (%d pairs) --"
				" rooms over rooms. One BSP cannot give both their floors and"
				" ceilings; expect holes in the flats there", members, (int)overlaps.size());
			int shown = 0;
			for (const RothPlanOverlap &po : overlaps)
			{
				if (shown++ >= 48)
				{
					log.Line("    ... and %d more pairs", (int)overlaps.size() - 48);
					break;
				}
				log.Line("    sector %-4d z %d..%d  and sector %-4d z %d..%d  share ~%.0f sq units",
					po.a, (int)rm.sectors[po.a].floorHeight, (int)rm.sectors[po.a].ceilingHeight,
					po.b, (int)rm.sectors[po.b].floorHeight, (int)rm.sectors[po.b].ceilingHeight,
					po.area);
			}
			log.Line("    test, lower rooms out:  roth_skip_sectors \"%s\"", lowerList.GetChars());
			log.Line("    test, upper rooms out:  roth_skip_sectors \"%s\"", upperList.GetChars());
			log.Count("sectors overlapping another in plan (not representable in one BSP)", members);
		}
		else
		{
			log.Line("  plan overlap    none -- no sector lies over another");
		}
	}

	// The tails of both arrays are reserved for the 3D floor control geometry
	// built after this loop -- see the platform count above.
	// roth_portals: a cut pair is two pending lines and so is already counted
	// in pending.size(); each has ONE side, which is what pending.size() +
	// twoSided already allows for (the pair was counted as one two-sided line).
	if (portalPending.size() != planCuts)
	{
		// The planner pairs faces by the same rule as the loop above. If the
		// two ever disagree a group would be left joined to nothing: stop.
		log.Warn("portal plan named %d cut(s) but the line loop found %d",
			(int)planCuts, (int)portalPending.size());
		log.End();
		I_Error("Realms map %s: portal plan and line pairing disagree (roth_portals)",
			map->rothFile.GetChars());
	}
	Level->lines.Alloc(pending.size() + size_t(platformCount) * 4 + size_t(leafCount) * 8);
	Level->sides.Alloc(pending.size() + twoSided + size_t(platformCount) * 4
		+ size_t(leafCount) * 8);    // one per original face, plus the void geometry
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
	int flippedFaces = 0;        // FF_FLIP_X: drawn from the X-mirrored registration of the image
	int shiftedFaces = 0;        // a non-zero shiftX/shiftY was applied
	int edgeMapFaces = 0;        // FF_EDGE_MAP: the outdoor backdrop seen through windows
	int keyWallPieces = 0;       // on the colour key: the original draws nothing there
	int transUpLoFaces = 0;      // FF_TRANS_UPLO with no band (not transparent, or override 0)
	int transUpLoBanded = 0;     // mid pieces cut to the TEXTURE_MAP_OVERRIDE band
	int extentBitsAbove12 = 0;   // see the note where this is reported
	struct WallShape { int w = 0, h = 0; bool ok = false; };
	std::map<int, WallShape> wallShape;   // stored index -> the picture's stored shape
	int bakedWallPieces = 0;     // pieces drawn from a baked image (roth_surface WallToEngine)
	int bakeTooLarge = 0;        // ...that would have needed one larger than 4096 texels a side
	int fittedPieces = 0;        // pieces laid down as exactly one copy
	int fitClearedPieces = 0;    // upper/lower pieces of a fitted face the original draws unfitted
	int doorMidToLeaf = 0;       // doorway mid pieces handed over to a door leaf

	unsigned sideIndex = 0;
	for (size_t li = 0; li < pending.size(); li++)
	{
		const roth::Face &f = rm.faces[pending[li].face];
		line_t *ld = &Level->lines[li];

		// A face of a moved sector uses that group's moved copy of each vertex
		// (roth_portals); with no plan VertexFor returns the vertex itself.
		{
			const int faceGroup = portalPlan.GroupOf(f.sector);
			ld->v1 = &Level->vertexes[portalPlan.VertexFor(faceGroup, f.vertex1)];
			ld->v2 = &Level->vertexes[portalPlan.VertexFor(faceGroup, f.vertex2)];
		}
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

		// The stored extent of each side's face, to become that side's
		// TexelLength once FinishLoadingLineDef has run (it overwrites the
		// field). -1 leaves the engine's own rounded line length in place.
		// See the SCALE block in makeSide.
		int wantTexelLength[2] = { -1, -1 };
		int makingSide = 0;

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
			// WHERE EVERY TEXEL OF THIS FACE GOES is decided in ONE place,
			// roth_surface (WallPieceRule: what ROTH.C samples; WallToEngine:
			// the engine values that sample the same), and nothing here
			// computes a scale, an offset, a flip or an anchor of its own.
			// roth_surface.h cites the renderer.c lines for each rule; this
			// block only collects what the rule needs to know about the face
			// and pushes the answer into the sidedef. tools/wallrule runs the
			// same two functions over all 44 maps against an independent
			// transcription of ROTH.C, piece by piece and join by join, and
			// tools/oracle confirms the rule against the original's own
			// pixels.
			//
			// What used to be here and was wrong (tools/wallrule, HEAD column):
			//   * FF_FLIP_X as a NEGATIVE horizontal scale. With the offset at
			//     0 the engine takes the run 0..-1 for "inside one copy" and
			//     clamps it (hw_walls.cpp SetWallCoordinates), so the piece is
			//     one smeared column: 2,777 pieces. The mirror now lives in the
			//     image (TextureSet::World, flipped), as it does in the
			//     original (renderer.c:4735).
			//   * FF_IMAGE_FIT stretched over the UPPER and LOWER pieces of a
			//     see-through face. The original builds those two with the
			//     flag cleared (renderer.c:8773, 8785): 429 pieces.
			//   * a lower piece under a neighbour floor ABOVE this ceiling was
			//     anchored at that floor; the original anchors it at the
			//     piece's own top (renderer.c:8682-8685).
			//   * a see-through mid piece was one clipped copy whatever the
			//     original did: no tiling, rows the band should hide, a start
			//     row beyond the image. 1,717 pieces.
			//   * the shifts of an extended record were dropped from every
			//     piece of a fitted face, including the unfitted ones.
			//   * a 12-bit stored extent was "restored" from the wall's length
			//     on two RAQUIA4 faces. ROTH.C reads 12 bits (renderer.c:9276)
			//     and draws what they say.
			//--------------------------------------------------------------
			const bool imageFit = (tf & roth::FF_IMAGE_FIT) != 0;
			const sector_t *ownSec = &Level->sectors[face.sector];
			const bool haveNbr = twoSided && neighbourSector >= 0
				&& neighbourSector < (int)rm.sectors.size();

			roth::WallFace wf;
			wf.flags = tf;
			wf.extended = tm->extended;
			wf.extent = (int)tm->StoredExtent();
			wf.shiftX = tm->extended ? (int)tm->shiftX : 0;
			wf.shiftY = tm->extended ? (int)tm->shiftY : 0;
			wf.length = wallLen;
			wf.twoSided = haveNbr;
			wf.ownFloor = (int)floor(ownSec->GetPlaneTexZ(sector_t::floor) + 0.5);
			wf.ownCeil  = (int)floor(ownSec->GetPlaneTexZ(sector_t::ceiling) + 0.5);
			wf.ownOverride = (face.sector >= 0 && face.sector < (int)rm.sectors.size())
				? (int)rm.sectors[face.sector].textureMapOverride : 0;
			if (haveNbr)
			{
				const sector_t *ns = &Level->sectors[neighbourSector];
				wf.nbrFloor = (int)floor(ns->GetPlaneTexZ(sector_t::floor) + 0.5);
				wf.nbrCeil  = (int)floor(ns->GetPlaneTexZ(sector_t::ceiling) + 0.5);
				// Does the mid overlay follow the upper and lower? (That is what
				// clears IMAGE_FIT on them.) The platform slabs are the map's.
				auto platOf = [&](int si, int &under, int &top) -> bool
				{
					under = top = 0;
					if (si < 0 || si >= (int)rm.sectors.size()) return false;
					const int pi = rm.sectors[si].platformIndex;
					if (pi < 0 || pi >= (int)rm.platforms.size()) return false;
					under = rm.platforms[pi].undersideZ; top = rm.platforms[pi].topZ;
					return true;
				};
				int nu, nt, ou, ot;
				const bool nh = platOf(neighbourSector, nu, nt);
				const bool oh = platOf(face.sector, ou, ot);
				wf.overlayFollows = roth::WallOverlayFollows(tf,
					haveArt && art.IsSkySurface(tm->midTexture),
					wf.ownFloor, wf.ownCeil, nh, nu, nt, oh, ou, ot);
			}

			// The side's TexelLength: the stored extent is the number of half
			// texels ROTH.C lays across the face (renderer.c:9276-9279, 13335).
			// Applied after FinishLoadingLineDef, which overwrites the field.
			const int ruleTexelLength = roth::WallTexelLength(wf);
			wantTexelLength[makingSide] = ruleTexelLength;
			const int sideTexelLength = ruleTexelLength >= 0 ? ruleTexelLength : (int)(wallLen + 0.5f);

			if (imageFit) imageFitFaces++;
			else if (wf.extent > 0) storedExtentScaled++;
			else storedExtentUnusable++;
			if (!imageFit && wf.extent > 0 && fabs(double(wf.extent) - wallLen) > 1.) storedExtentDeviates++;
			if (wf.shiftX != 0 || wf.shiftY != 0) shiftedFaces++;
			if (tf & roth::FF_FLIP_X) flippedFaces++;
			const bool banded = haveNbr && (tf & roth::FF_TRANSPARENT) && (tf & roth::FF_TRANS_UPLO)
				&& wf.ownOverride != 0;

			roth::WallPieceShape pieceShape[3];   // for RegisterWallFace
			auto setPart = [&](int part, int storedIndex, bool masked)
			{
				// THE COLOUR KEY MEANS DRAW NOTHING, on a wall as on a flat.
				//
				// renderer.c:9222-9225 is a THREE-way test: a negative index is
				// a solid palette colour, an index equal to the pack's key
				// draws NO PIXELS AT ALL, and anything else is textured. The
				// key is a real painted entry in every pack -- DEMO's is an
				// opaque 256x146 picture -- so handing it to World() returns
				// that artwork and paints a wall across an opening the original
				// leaves open. IsSkySurface is this exact test (index ==
				// SkyMarkerIndex); the name is from when the key was thought to
				// mean sky.
				if (haveArt && art.IsSkySurface(storedIndex))
				{
					keyWallPieces++;
					return false;
				}
				if (!haveArt) return false;

				// The picture's shape, in the wall's terms: its stored HEIGHT
				// runs along the wall, its stored row length down it
				// (renderer.c:13288-13289).
				int storedW = 0, storedH = 0;
				bool picture = false;
				if (storedIndex >= 0 && storedIndex < (int)roth::TEX_PICTURE_LIMIT)
				{
					// ImageShape decodes the image, and a map asks for the same
					// few hundred pictures thousands of times.
					auto known = wallShape.find(storedIndex);
					if (known == wallShape.end())
					{
						WallShape ws; bool translucent = false;
						ws.ok = art.ImageShape(storedIndex, ws.w, ws.h, translucent);
						known = wallShape.emplace(storedIndex, ws).first;
					}
					picture = known->second.ok;
					storedW = known->second.w; storedH = known->second.h;
				}
				const int dimU = storedH, dimV = storedW;

				roth::WallPiece wp;
				roth::WallEngineSetup we;
				if (picture)
				{
					wp = roth::WallPieceRule(wf, (roth::WallTier)part, dimU, dimV);
					we = roth::WallToEngine(wf, (roth::WallTier)part, wp, dimU, dimV, sideTexelLength);
				}

				FTextureID tex;
				if (we.drawn && we.baked)
				{
					// An image of exactly the columns and rows the original
					// draws on this piece, in order. See WallToEngine.
					static uint16_t colMap[4096], rowMap[4096];
					int cols = 0, rows = 0;
					roth::WallBakeMaps(wp, we, dimU, dimV, colMap, cols, rowMap, rows, 4096);
					if (cols > 0 && rows > 0)
					{
						tex = art.WorldBaked(storedIndex, &log, masked, colMap, cols, rowMap, rows);
						bakedWallPieces++;
					}
					else
					{
						// Too large to bake (no retail piece is): the plain
						// image, tiled by the engine. Counted, not hidden.
						if (we.bakeV) we.yOffset += wp.v0;   // the start row the bake would have held
						we.baked = we.bakeU = we.bakeV = false;
						we.mirrored = wp.mirror;
						we.xOffset = wp.u0;
						bakeTooLarge++;
					}
				}
				if (!tex.isValid())
					tex = art.World(storedIndex, &log, masked, picture && (tf & roth::FF_FLIP_X) != 0);
				if (!tex.isValid()) return false;   // nothing to draw here

				double sx, sy, ox, oy;
				if (we.drawn)
				{
					sx = we.xScale; sy = we.yScale; ox = we.xOffset; oy = we.yOffset;
					if (wp.fit) fittedPieces++;
					else if (imageFit) fitClearedPieces++;
					if ((tf & roth::FF_PIN_BOTTOM) && !wp.fit) pinBottomFaces++;
					if (part == side_t::mid && banded) transUpLoBanded++;
				}
				else
				{
					// A piece with no height at load (the two sectors meet), or
					// a solid colour. Nothing is drawn now; if a moving sector
					// opens the piece later it starts from the unfitted rule's
					// plain terms. (ROTH.C re-derives every piece from the live
					// heights each frame; this loader does it once. See the
					// wallrule report, "what remains".)
					sx = sy = (tf & roth::FF_HALF_PIXEL) ? 1. : 0.5;
					ox = wf.shiftX; oy = wf.shiftY;
				}

				pieceShape[part].dimU = picture ? dimU : 0;
				pieceShape[part].dimV = picture ? dimV : 0;
				pieceShape[part].baked = we.drawn && we.baked;
				sd->SetTexture(part, tex);
				sd->SetTextureXScale(part, sx);
				sd->SetTextureYScale(part, sy);
				sd->SetTextureXOffset(part, ox);
				sd->SetTextureYOffset(part, oy);
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
			// The same inputs, for the runtime's texture swaps and scrolls, which
			// re-run this rule rather than keeping a copy of it (ReapplyWallFace).
			roth::RegisterWallFace((int)(&face - &rm.faces[0]), wf, sideTexelLength,
				face.sector, haveNbr ? neighbourSector : -1, pieceShape);

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

		const int portalPeer = pending[li].portalPeer;
		ld->sidedef[0] = makeSide(f, pending[li].sister >= 0 || portalPeer >= 0,
			pending[li].sister >= 0 ? rm.faces[pending[li].sister].sector
				: portalPeer >= 0 ? rm.faces[portalPeer].sector : -1);
		if (pending[li].sister >= 0)
		{
			makingSide = 1;
			ld->sidedef[1] = makeSide(rm.faces[pending[li].sister], true, f.sector);
			ld->flags |= ML_TWOSIDED;
		}
		else if (portalPeer >= 0)
		{
			// One half of a cut pair: a ONE-SIDED line that is not a wall. The
			// front side above was built exactly as it would have been against
			// the real neighbour -- same pieces, same heights, because makeSide
			// reads the neighbour's own sector -- and there is no back side:
			// the linked portal made further down stands in for it, and the
			// engine measures the opening against the neighbour itself, as it
			// is at that moment (line_t::getPortalBeyondSector). So no
			// ML_TWOSIDED (there is no second side to look at) and no
			// ML_BLOCKING (the line must not stop anyone the opening lets by).
		}
		else
		{
			ld->flags |= ML_BLOCKING;
		}

		// EVERY WALL, NAMED, when roth_logwalls is on.
		//
		// rothdiff answers "which surface is wrong" with an ENGINE line index,
		// and nothing in the tree could turn that number back into the Realms
		// face it came from -- so a per-pixel diff that correctly blamed two
		// linedefs still left a hunt through 1,908 of them. One line per wall
		// closes that loop: index, the face behind it, its sector and the
		// texture-mapping flags that decide how (and whether) it draws.
		if (roth_logwalls)
		{
			const roth::TextureMap *tmL =
				(f.textureMap >= 0 && f.textureMap < (int)rm.textureMaps.size())
					? &rm.textureMaps[f.textureMap] : nullptr;
			// BOTH sides' heights. An engine line only draws an upper or a
			// lower band where the two sectors' ceilings or floors disagree,
			// so the pair of spans is what says whether a two-sided line can
			// put anything across a ceiling or a floor at all.
			const int sis = pending[li].sister;
			const int ssec = sis >= 0 ? rm.faces[sis].sector : -1;
			// IS THE PAIRING MUTUAL? A sister link that the other face does not
			// return is not a shared wall between two rooms -- it is a portal
			// to somewhere else in the map, and the renderer shows the far
			// room through it like a hole in the world.
			const bool mutualSis = sis >= 0 && sis < (int)rm.faces.size()
				&& rm.faces[sis].sister == pending[li].face;
			log.Line("  wall line %-5d (%d,%d)-(%d,%d) len %.0f | face %-5d"
				" sector %-4d z %d..%d |"
				" sister %-5d sector %-4d z %s%s | map %-5d flags 0x%02x",
				(int)li,
				(int)ld->v1->fX(), (int)ld->v1->fY(),
				(int)ld->v2->fX(), (int)ld->v2->fY(),
				(ld->v2->fPos() - ld->v1->fPos()).Length(),
				pending[li].face, f.sector,
				(int)rm.sectors[f.sector].floorHeight,
				(int)rm.sectors[f.sector].ceilingHeight,
				sis, ssec,
				ssec >= 0
					? [&]{ static char b[32];
						snprintf(b, sizeof(b), "%d..%d",
							(int)rm.sectors[ssec].floorHeight,
							(int)rm.sectors[ssec].ceilingHeight);
						return b; }()
					: "-",
				(sis >= 0 && !mutualSis) ? "  NOT MUTUAL" : "",
				f.textureMap, tmL ? (unsigned)tmL->flags : 0u);

			// AND THE FAR SIDE'S OWN MAPPING. Either side of a two-sided wall
			// can ask for a mid piece, and only the near one was being printed
			// -- so a line whose BACK face wants a see-through panel looked
			// blameless here while drawing nothing at all in the world.
			if (sis >= 0)
			{
				const roth::Face &sf = rm.faces[sis];
				const roth::TextureMap *tmS =
					(sf.textureMap >= 0 && sf.textureMap < (int)rm.textureMaps.size())
						? &rm.textureMaps[sf.textureMap] : nullptr;
				log.Line("      back face %-5d map %-5d flags 0x%02x  mid %u"
					" upper %u lower %u", sis, sf.textureMap,
					tmS ? (unsigned)tmS->flags : 0u,
					tmS ? (unsigned)tmS->midTexture : 0u,
					tmS ? (unsigned)tmS->upperTexture : 0u,
					tmS ? (unsigned)tmS->lowerTexture : 0u);
			}
		}

		if (hasUpper) ld->flags |= ML_DONTPEGTOP;
		// NO ML_DONTPEGBOTTOM. Doom's default bottom-part anchor is already the
		// top of the step, which is exactly what Realms does (R:8685); the flag
		// would re-anchor it to the ceiling instead. FF_PIN_BOTTOM is applied as
		// an explicit texture offset in setPart.

		linemap.Push((unsigned)li);
		ld->AdjustLine();
		FinishLoadingLineDef(ld, 255);

		// FinishLoadingLineDef has just set TexelLength to the rounded line
		// length. roth_surface's WallTexelLength replaces it with the stored
		// extent -- the number of half-texels ROTH.C lays across the face
		// (renderer.c:9276-9279, 13335) -- and every scale makeSide set was
		// computed against that number.
		for (int s = 0; s < 2; s++)
			if (ld->sidedef[s] != nullptr && wantTexelLength[s] >= 0)
				ld->sidedef[s]->TexelLength = (uint16_t)wantTexelLength[s];
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
			// roth_platforms: this slab moves, and is an actor. Same test as
			// the census above, which is what keeps the allocation exact.
			if (slabIsActor[i]) continue;

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

			// EVERY SLAB, NAMED. A slab that resolves to no texture is drawn as
			// a solid black quad hanging in the room -- the "black holes in the
			// floor and ceiling" an owner reports, which look like missing
			// geometry and are actually a mid-platform with no art. Logging the
			// stored indices makes that a lookup instead of a hunt.
			if (!slabTop.isValid() || !slabBot.isValid())
			{
				log.Line("  platform sector %d  top idx %u %s, underside idx %u %s"
					"  z %.0f..%.0f  <-- DRAWS BLACK",
					(int)i, (unsigned)mp.topTexture,
					topTex.isValid() ? "ok" : "UNRESOLVED",
					(unsigned)mp.undersideTexture,
					undTex.isValid() ? "ok" : "UNRESOLVED", undZ, topZ);
				log.Count("mid-platforms: no art on either face -- draws black",
					1);
			}

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
				// These lines came out of the memset with portalindex 0, and this
				// engine's line_t::getPortal (g_levellocals.h) only treats UINT_MAX
				// as "no portal". Harmless while a Realms map had no line portals
				// and nothing asked; with roth_portals it would make every slab
				// control line a copy of portal 0.
				cl->portalindex = UINT_MAX;
				cl->portaltransferred = UINT_MAX;
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
	// Moving sectors -> platform actors   (roth_platforms)
	//
	// RAW opcode 0x09 slides a sector by adding a delta to its four vertices
	// every frame, and its neighbours stretch to follow. Nothing in a static BSP
	// can do that. Where roth_platformplan.h finds that the four neighbours are
	// LEVEL with each other, though, "the sector slides" and "its slab, or the
	// block of floor it stands proud by, slides along a level channel" are the
	// same picture -- and a block sliding along a channel is an actor.
	//
	// So for each such sector, two things happen here:
	//
	//   * its solid parts are DESCRIBED to roth_platforms as boxes -- the
	//     mid-platform slab, the floor above the neighbours' floor, the ceiling
	//     below the neighbours' ceiling -- each with the engine's own plane
	//     parameters for the flats it wears, so the box's top samples exactly
	//     what the plane sampled. The actors are spawned later, when the level
	//     can be linked into (MapLoader::LoadLevel, beside the props);
	//
	//   * its floor and ceiling in the STATIC map are set to the neighbours',
	//     flats included, so what is left behind is the channel. Its slab was
	//     already kept out of the 3D floors above.
	//
	// The Realms sector record is NOT changed: the level logic still holds the
	// sector's own heights, and the platform follows those.
	//
	// The side faces of a box take the wall art the original shows on the step:
	// the neighbour's face looking at the block names it, as its lower texture
	// for a floor block and its upper for a ceiling block. A slab has no edge
	// art in Realms; it borrows its top's, as the 3D floors do.
	//----------------------------------------------------------------------
	if (!movePlans.empty())
	{
		log.StageBegin("moving sectors");
		log.Section("Moving sectors (roth_platforms)");
		int built = 0, pieces = 0, notBuilt = 0;

		// A sector plane's texturing as the engine holds it right now.
		auto planeOf = [&](sector_t *sx, int which) -> roth::PlatformFlat
		{
			roth::PlatformFlat f;
			f.tex = sx->GetTexture(which);
			if (f.tex == skyflatnum) f.tex = FNullTextureID();   // never draw sky on a box
			f.xScale = sx->GetXScale(which);
			f.yScale = sx->GetYScale(which);
			f.xOffset = sx->GetXOffset(which);
			f.yOffset = sx->GetYOffset(which, false);
			f.angle = sx->GetAngle(which, false).Degrees();
			return f;
		};
		// A mid-platform face, through roth_surface exactly as the 3D floors.
		auto slabFlat = [&](uint16_t texWord, uint8_t sbits, uint8_t shX, uint8_t shY)
		{
			roth::FlatSetup f = {};
			f.textureWord = texWord;
			f.scaleBits = sbits;
			f.shiftX = shX;
			f.shiftY = shY;
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
			const roth::FlatEngineSetup e = roth::FlatToEngine(f);
			roth::PlatformFlat out;
			out.tex = worldTex(texWord);
			out.xScale = e.xScale;   out.yScale = e.yScale;
			out.xOffset = e.xOffset; out.yOffset = e.yOffset;
			out.angle = e.angle;
			return out;
		};
		auto copyPlane = [&](sector_t *to, sector_t *from, int which)
		{
			const double z = from->GetPlaneTexZ(which);
			to->SetPlaneTexZ(which, z);
			if (which == sector_t::floor) to->floorplane.set(0., 0., 1., -z);
			else                          to->ceilingplane.set(0., 0., -1., z);
			to->SetTexture(which, from->GetTexture(which), false);
			to->SetXScale(which, from->GetXScale(which));
			to->SetYScale(which, from->GetYScale(which));
			to->SetXOffset(which, from->GetXOffset(which));
			to->SetYOffset(which, from->GetYOffset(which, false));
			to->SetAngle(which, from->GetAngle(which, false));
		};

		for (const rothplat::Plan &mpn : movePlans)
		{
			FString recs;
			for (int r : mpn.records) recs.AppendFormat("%s%d", recs.IsEmpty() ? "" : ",", r);

			FString why = mpn.reason.c_str();
			bool ok = mpn.supported;
			// A geometry test that removed this sector or the neighbour it
			// borrows from removed the thing this would be built against.
			if (ok && (sectorSkipped(mpn.sector) || sectorSkipped(mpn.nbSector)))
			{ ok = false; why = "sector or neighbour left out by roth_skip_sectors"; }
			// roth_portals moves whole sectors to another region. A platform
			// whose sector and channel ended up in different regions would have
			// to cross a portal the moment it moved; that has not been looked
			// at, so it is declined rather than guessed.
			double offX = 0., offY = 0.;
			if (ok)
			{
				roth::SectorShift(mpn.sector, offX, offY);
				const roth::Sector &ms = rm.sectors[mpn.sector];
				for (int k = 0; k < 4 && ok; k++)
				{
					const roth::Face &f = rm.faces[ms.firstFaceIndex + k];
					if (f.sister < 0) continue;
					double nx = 0., ny = 0.;
					roth::SectorShift(rm.faces[f.sister].sector, nx, ny);
					if (nx != offX || ny != offY)
					{ ok = false; why = "roth_portals put it and a neighbour in different regions"; }
				}
			}
			if (!ok)
			{
				log.Line("  sector %4d  record(s) %s  NOT BUILT: %s", mpn.sector,
					recs.GetChars(), why.GetChars());
				notBuilt++;
				continue;
			}

			const roth::Sector &ms = rm.sectors[mpn.sector];
			sector_t *sec = &Level->sectors[mpn.sector];
			sector_t *nb = &Level->sectors[mpn.nbSector];

			// Which neighbour face looks at each side of the box.
			// south (y0), east (x1), north (y1), west (x0).
			int sideFace[4] = { -1, -1, -1, -1 };
			for (int k = 0; k < 4; k++)
			{
				const roth::Face &f = rm.faces[ms.firstFaceIndex + k];
				const roth::Vertex &a = rm.vertices[f.vertex1], &b = rm.vertices[f.vertex2];
				int edge = -1;
				if (a.y == b.y) edge = (a.y == mpn.y0) ? 0 : 2;
				else if (a.x == b.x) edge = (a.x == mpn.x1) ? 1 : 3;
				if (edge >= 0) sideFace[edge] = f.sister;
			}
			auto stepTex = [&](int edge, bool upper, FTextureID fallback) -> FTextureID
			{
				const int sf = sideFace[edge];
				if (sf >= 0 && rm.faces[sf].textureMap >= 0
					&& (size_t)rm.faces[sf].textureMap < rm.textureMaps.size())
				{
					const roth::TextureMap &tm = rm.textureMaps[rm.faces[sf].textureMap];
					const FTextureID t = worldTex(upper ? tm.upperTexture : tm.lowerTexture);
					if (t.isValid()) return t;
				}
				return fallback;
			};

			FString desc;
			for (const rothplat::Piece &pc : mpn.pieces)
			{
				roth::PlatformPiece pp;
				pp.rothSector = mpn.sector;
				pp.kind = pc.kind;
				pp.x0 = double(mpn.x0) + offX;  pp.x1 = double(mpn.x1) + offX;
				pp.y0 = double(mpn.y0) + offY;  pp.y1 = double(mpn.y1) + offY;
				pp.z0 = double(pc.z0);          pp.z1 = double(pc.z1);
				pp.radius = mpn.radius;
				for (const auto &c : mpn.centres)
					pp.centres.push_back({ c.first + offX, c.second + offY });
				pp.lightLevel = sec->lightlevel;

				if (pc.kind == rothplat::PIECE_SLAB)
				{
					const roth::MidPlatform &mp = rm.platforms[ms.platformIndex];
					pp.top = slabFlat(mp.topTexture, (uint8_t)((mp.scales >> 4) & 3),
						mp.topShiftX, mp.topShiftY);
					pp.bottom = slabFlat(mp.undersideTexture, (uint8_t)((mp.scales >> 2) & 3),
						mp.undersideShiftX, mp.undersideShiftY);
					// Missing art borrows the other face's, as for the 3D floors.
					if (!pp.top.tex.isValid()) pp.top = pp.bottom;
					if (!pp.bottom.tex.isValid()) pp.bottom = pp.top;
					for (int e = 0; e < 4; e++) pp.side[e] = pp.top.tex;
					desc.AppendFormat(" slab %d..%d", pc.z0, pc.z1);
				}
				else if (pc.kind == rothplat::PIECE_FLOORBLOCK)
				{
					// Its top is the sector's own floor; it stands on the ground.
					pp.top = planeOf(sec, sector_t::floor);
					for (int e = 0; e < 4; e++) pp.side[e] = stepTex(e, false, pp.top.tex);
					desc.AppendFormat(" floor-block %d..%d", pc.z0, pc.z1);
				}
				else
				{
					// Its underside is the sector's own ceiling; it hangs flush.
					pp.bottom = planeOf(sec, sector_t::ceiling);
					for (int e = 0; e < 4; e++) pp.side[e] = stepTex(e, true, pp.bottom.tex);
					desc.AppendFormat(" ceiling-block %d..%d", pc.z0, pc.z1);
				}
				roth::AddPlatformPiece(pp);
				pieces++;
			}

			// What is left behind is the channel. AFTER the pieces were
			// described, because they read this sector's own planes.
			if (mpn.lowerFloor) copyPlane(sec, nb, sector_t::floor);
			if (mpn.raiseCeil)  copyPlane(sec, nb, sector_t::ceiling);

			log.Line("  sector %4d  record(s) %s  %s:%s  footprint %dx%d as %d box(es) r %.0f%s%s",
				mpn.sector, recs.GetChars(), mpn.reason.c_str(), desc.GetChars(),
				mpn.x1 - mpn.x0, mpn.y1 - mpn.y0, (int)mpn.centres.size(), mpn.radius,
				mpn.lowerFloor ? "; static floor -> neighbours'" : "",
				mpn.raiseCeil ? "; static ceiling -> neighbours'" : "");
			built++;
		}
		log.Line("  %d moving sector(s) built as platforms (%d piece(s)), %d not built",
			built, pieces, notBuilt);
		log.Count("moving sectors: 0x09 sectors with no platform (still static)", notBuilt);
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
	// WHAT ART THE LEAVES ACTUALLY GOT, by name and count.
	//
	// The owner reports doors looking untextured while this loader reports
	// `leaf side had no artwork 0` -- the two cannot both be describing the same
	// thing, and a lit screenshot cannot separate them because Realms is dark
	// enough that a door 48 units away photographs as near-black. The texture is
	// known HERE, at the moment it is chosen, where no lighting can hide it.
	//
	// Counted on the colour key as well: the wall path skips a piece whose
	// stored index is the key, because the original draws nothing there
	// (IsSkySurface, :1118), and the leaf path has no such test. A leaf whose
	// skin is on the key would therefore be handed to World() and painted with
	// whatever that returns -- or be the thing that is not appearing.
	std::map<FString, int> leafTexNames;
	int leafSidesOnColourKey = 0;
	std::map<int, int> leafLightHist;   // doorway sector lightlevel -> how many leaves
	std::map<int, int> leafOverrideHist;// door sector TEXTURE_MAP_OVERRIDE (+0x0c)
	std::map<int, int> leafSpanHist;    // the full span a leaf is currently given
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
			// SETTLED 2026-11-12 (tools/doorcheck). The mirror is decided by the
			// hinge mapping's faceID, the "marker" (doors.c:506, 523-531):
			//   0xFFFE  never mirrored: pivot vertex1, angle byte 0 -> +0x40
			//   0xFFFD  always mirrored: pivot vertex2, angle byte 0 -> -0x40
			//   0xFFFF  mirrored only when opened from the room across c3
			// 160 of the 167 retail doors carry FFFD or FFFE, so their pivot is
			// fixed by the map and is set here; the 7 FFFF doors start on
			// vertex1 and roth::SwingDoor moves the pivot when one is opened
			// from the other side. Pinning every door to vertex1 put 74 doors'
			// pivot one slab thickness out.
			const roth::Face &hf = rm.faces[rs.hingeFace];
			const roth::TextureMap *hingeTm =
				(hf.textureMap >= 0 && hf.textureMap < (int)rm.textureMaps.size())
					? &rm.textureMaps[hf.textureMap] : nullptr;
			const int hingeMarker = hingeTm ? (int)hingeTm->faceID : 0xFFFE;
			const int hingeVertex = (hingeMarker == 0xFFFD) ? hf.vertex2 : hf.vertex1;
			const double hingeX = double(rm.vertices[hingeVertex].x);
			const double hingeY = double(rm.vertices[hingeVertex].y);

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
			// WHAT LIGHT A LEAF ACTUALLY INHERITS.
			//
			// Door leaves render BLACK while the wall around them draws fine,
			// and the art is not the cause -- every leaf side is given real
			// named Realms door art. R_RothShade cannot be the cause either:
			// its light-0 case returns 0.0, which is "unshaded, raw texels",
			// i.e. BRIGHT. So a black leaf points at the light this line
			// copies, and the doorway sector is a thin slot in a wall, which
			// is exactly the kind of sector that may carry no light of its own.
			// Tallied rather than assumed.
			leafLightHist[(int)door->lightlevel]++;
			leafOverrideHist[(int)rs.textureMapOverride]++;
			vs->Colormap.Desaturation = door->Colormap.Desaturation;
			roth::RegisterLightFollower((int)i, (int)vSector);
			const double doorFullTop = door->GetPlaneTexZ(sector_t::ceiling);
			const double doorFullBot = door->GetPlaneTexZ(sector_t::floor);
			const int ovr = (int)rs.textureMapOverride;
			// THE LEAF'S VERTICAL EXTENT, from ROTH.C and no longer a candidate.
			//
			// The leaf is a box mesh standing on the door sector's FLOOR:
			// rwss_type04 passes `es:[sector+2] - view z`, the floor, as the
			// mesh's base (renderer.c:6436-6438), ring B of its eight vertices
			// is at height 0 and ring A at the height word (doors.c:561-571).
			// The height word is the door sector's ceiling minus its floor --
			// UNLESS corner c1's own mapping carries both TRANSPARENT and
			// TRANS_UPLO, in which case it is -4 * TEXTURE_MAP_OVERRIDE
			// (doors.c:534-538). Times four, and only under that flag pair.
			//
			// What stood here took the override unscaled and unconditionally,
			// which made sector 23 of STUDY1 (override 13, no flag pair) a strip
			// 13 units tall. It never showed, because nothing read this sector's
			// height: a one-sided polyobject wall was drawn floor to ceiling of
			// whatever sector it stood IN (see WALLF2_OWNHEIGHT below).
			// tools/doorcheck: 36 of 167 leaves take the override, none comes
			// out zero or negative.
			double leafHeight = doorFullTop - doorFullBot;
			{
				const roth::Face &c1f = rm.faces[f0 + ((hingeRel + 1) & 3)];
				if (c1f.textureMap >= 0 && c1f.textureMap < (int)rm.textureMaps.size())
				{
					const uint8_t c1flags = rm.textureMaps[c1f.textureMap].flags;
					if ((c1flags & roth::FF_TRANSPARENT) && (c1flags & roth::FF_TRANS_UPLO))
						leafHeight = -4. * double(ovr);
				}
			}
			const double lfZ = doorFullBot;
			const double lcZ = doorFullBot + (leafHeight > 0. ? leafHeight : 0.);
			leafSpanHist[(int)(lcZ - lfZ)]++;
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

				// THE LEAF IS WHAT THE PLAYER CLICKS, so the level logic has to
				// be able to get from this line back to the panel it swings.
				// A leaf has no Realms face id -- these four lines are generated
				// here, not read from the map -- so RegisterFaceSide cannot
				// carry it and this pairing is by polyobject tag instead.
				// GAME_core.md §4.5: a type-6 door goes straight to
				// toggle_door_open_state, with no trigger and no chain.
				// The ROOM face behind a broad side goes with it: a closed door in
				// the original is that face, so a use on the leaf is a use on it.
				roth::RegisterDoorLeafSide((int)(sd - &Level->sides[0]), tag,
					((j == 1 || j == 3) && f.sister >= 0) ? f.sister : -1);
				// The leaf has a height of its own: this sidedef's sector, the
				// void room sized above. See WALLF2_OWNHEIGHT (r_defs.h).
				sd->Flags2 |= WALLF2_OWNHEIGHT;
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

				// WHAT THE SURFACE'S DRAW WORD SAYS (door_corner_tail,
				// doors.c:263-276, from the TEXTURE source's flag byte):
				//   TRANSPARENT (bit 0) is kept -- palette index 0 is a hole.
				//     44 leaf sides in the retail maps wear art that has holes
				//     (the glazed cabinet doors of STUDY1, texture 748).
				//   TRANS_UPLO without TRANSPARENT clears the textured bit.
				//     No retail leaf does (tools/doorcheck), so it is honoured
				//     by drawing nothing rather than by guessing a fill.
				// And the colour key draws nothing on a leaf as on a wall: it is
				// the same rasteriser (renderer.c:13100-13160).
				const uint8_t skinFlags = texTm ? texTm->flags : 0;
				const bool skinMasked = (skinFlags & roth::FF_TRANSPARENT) != 0;
				const bool skinBlank = texTm == nullptr
					|| ((skinFlags & roth::FF_TRANS_UPLO) && !skinMasked)
					|| (haveArt && art.IsSkySurface(texTm->midTexture));
				FTextureID tex = skinBlank ? FNullTextureID()
					: worldTex(texTm->midTexture, skinMasked);

				// Record what was chosen, before anything can hide it.
				if (texTm && haveArt && art.IsSkySurface(texTm->midTexture))
					leafSidesOnColourKey++;
				{
					auto *gt = tex.isValid()
						? TexMan.GetGameTexture(tex, true) : nullptr;
					FString nm;
					if (gt != nullptr && gt->GetName().GetChars() != nullptr
						&& gt->GetName().GetChars()[0] != '\0')
						nm = gt->GetName().GetChars();
					else
						nm.Format("(index %u, no name)",
							texTm ? (unsigned)texTm->midTexture : 0u);
					leafTexNames[nm]++;
				}

				// THE LINE'S LENGTH, AS THE ENGINE MEASURES IT. This is the line
				// that was missing, and it is why every door looked untextured:
				// the leaf's lines are built here and never went through
				// FinishLoadingLineDef, so side_t::TexelLength stayed at the
				// zero the array was cleared to. HWWall::SetWallCoordinates
				// spans the texture over `TexelLength` texels from one end of
				// the wall to the other (hw_walls.cpp:1378-1381), so with 0
				// every column of the wall sampled the SAME texel column -- a
				// door-coloured smear with no picture in it.
				const double len = (ld->v2->fPos() - ld->v1->fPos()).Length();
				sd->TexelLength = (uint16_t)min((int)(len + 0.5), 65535);

				if (!tex.isValid())
				{
					leafSidesNoTexture++;
				}
				else
				{
					// THE SAME RASTERISER AS A WALL, entered with the surface's
					// own record (renderer.c:13100-13117, 13334-13361):
					//
					//   IMAGE_FIT clear (draw bit 0x100): the extents are the
					//     surface's stored ones -- +0x26, the extent mapping's
					//     fit word & 0xfff, across; +0x24, the leaf height,
					//     down. Half-texels, so two world units per texel both
					//     ways, hung from the top of the leaf.
					//   IMAGE_FIT set: both extents are twice the picture's own
					//     size, i.e. exactly one copy across the quad and one
					//     down it.
					//
					// Shifts and HALF_PIXEL are NOT part of a leaf surface: the
					// draw word is masked with 0x103 (doors.c:267) and the
					// record has no shift fields. The original reads both from
					// globals left by whatever wall it drew last; this draws
					// them as zero and off, which is what a leaf's own data says.
					//
					// This used to scale every side as tiled, IMAGE_FIT or not
					// (294 of 664 leaf sides are fitted), and to fold the extent
					// into the scale -- the fault the wall loop documents at its
					// own TexelLength.
					auto *gt = TexMan.GetGameTexture(tex, false);
					const double texW = gt ? gt->GetDisplayWidth() : 0.;
					const double texH = gt ? gt->GetDisplayHeight() : 0.;
					const bool flipX = (skinFlags & roth::FF_FLIP_X) != 0;
					double sx = 0.5, sy = 0.5;
					if (skinFlags & roth::FF_IMAGE_FIT)
					{
						if (texW > 0. && len > 0.) sx = OneCopyScale(texW, len, flipX);
						if (texH > 0. && lcZ > lfZ) sy = OneCopyScale(texH, lcZ - lfZ, false);
					}
					else
					{
						const int stored = extTm ? int(extTm->fitWord & 0x0FFF) : 0;
						if (stored > 0) sd->TexelLength = (uint16_t)stored;
					}
					if (flipX) sx = -sx;   // as the wall loop does it
					sd->SetTexture(side_t::mid, tex);
					sd->SetTextureXScale(side_t::mid, sx);
					sd->SetTextureYScale(side_t::mid, sy);
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
			// The SPAWN spot follows the doorway if its sector was moved
			// (roth_portals); the anchor and the leaf stay in the void cell.
			const int doorGroup = portalPlan.GroupOf((int)i);
			const double shiftX = portalPlan.active ? portalPlan.offX[doorGroup] : 0.;
			const double shiftY = portalPlan.active ? portalPlan.offY[doorGroup] : 0.;
			polySpots.push_back({ hingeX + shiftX, hingeY + shiftY,
				hingeX + offX, hingeY + offY, tag });

			// Tag the DOORWAY sector with the same number so stage 7 can get from
			// the sector the player used to the polyobject that fills it. A tag is
			// the engine's own general handle for this; nothing Realms-specific.
			Level->tagManager.AddSectorTag((int)i, tag);
			// And tell the runtime, so a command that names this door can find
			// the polyobject that swings without searching the tag table.
			roth::RegisterDoor((int)i, tag);
			// What decides the swing, for the runtime: the marker, the rooms
			// across c1 and c3, and both ends of the hinge edge where the leaf
			// really stands (doors.c:516-546).
			{
				const roth::Face &c1f = rm.faces[f0 + ((hingeRel + 1) & 3)];
				const roth::Face &c3f = rm.faces[f0 + ((hingeRel + 3) & 3)];
				const int secAcrossC1 = (c1f.sister >= 0 && c1f.sister < (int)rm.faces.size())
					? rm.faces[c1f.sister].sector : -1;
				const int secAcrossC3 = (c3f.sister >= 0 && c3f.sister < (int)rm.faces.size())
					? rm.faces[c3f.sister].sector : -1;
				roth::RegisterDoorSwing(tag, hingeMarker, secAcrossC1, secAcrossC3,
					double(rm.vertices[hf.vertex1].x) + shiftX, double(rm.vertices[hf.vertex1].y) + shiftY,
					double(rm.vertices[hf.vertex2].x) + shiftX, double(rm.vertices[hf.vertex2].y) + shiftY);
			}

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
		log.Count("doors: leaf side whose skin sits on the COLOUR KEY"
			" (the wall path skips these; the leaf path does not)",
			leafSidesOnColourKey);
		log.Line("  door leaf skins actually assigned, by name:");
		for (auto &kv : leafTexNames)
			log.Line("    %-24s x%d", kv.first.GetChars(), kv.second);
		log.Line("  light each leaf inherits from its DOORWAY sector:");
		for (auto &kv : leafLightHist)
			log.Line("    lightlevel %-4d x%d", kv.first, kv.second);
		log.Line("  leaf height (doors.c:534-538: door ceiling - floor, or"
			" -4 * override under TRANSPARENT+TRANS_UPLO on c1):");
		for (auto &kv : leafSpanHist)
			log.Line("    span %-5d x%d", kv.first, kv.second);
		log.Line("  door sector TEXTURE_MAP_OVERRIDE (+0x0c):");
		for (auto &kv : leafOverrideHist)
			log.Line("    override %-5d x%d", kv.first, kv.second);
		log.Count("doors: leaf faces coplanar with a drawn wall piece (UNVERIFIED risk)",
			leafCoplanarRisk);
		log.Count("doors: 0xFFFE door-capable sectors, no leaf built (OPEN QUESTION)",
			doorCapableNotBuilt);
	}

	//----------------------------------------------------------------------
	// roth_portals: the portals themselves.
	//----------------------------------------------------------------------
	if (portalPlan.active)
	{
		log.StageBegin("portals");
		// Two portals per cut, each naming the other, exactly as the engine's
		// own Eternity-style definition makes them (MapLoader::SpawnLinePortal,
		// specials.cpp, the PORTT_LINKEDEE branch): type LINKED, no z offset,
		// interactive. FinalizePortals (after PO_Init) then measures the
		// displacement from the vertices and floods the groups. A linked pair
		// must be an exact translation of each other with reversed direction;
		// the plan moves whole groups by whole units, so it is -- checked here
		// rather than trusted, because the engine demotes a mismatched pair to
		// a teleporter without stopping -- and then, both lines being one-sided,
		// to a picture nobody can walk into (FLevelLocals::ValidateOneSidedPortals).
		int made = 0, bad = 0;
		for (const std::pair<int, int> &pp : portalPending)
		{
			line_t *la = &Level->lines[pp.first];
			line_t *lb = &Level->lines[pp.second];
			const DVector2 d1 = lb->v2->fPos() - la->v1->fPos();
			const DVector2 d2 = lb->v1->fPos() - la->v2->fPos();
			const bool exact = d1.X == d2.X && d1.Y == d2.Y
				&& la->frontsector != nullptr && lb->frontsector != nullptr
				&& la->backsector == nullptr && lb->backsector == nullptr;
			if (!exact) bad++;
			line_t *ends[2] = { la, lb };
			for (int k = 0; k < 2 && exact; k++)
			{
				ends[k]->portalindex = Level->linePortals.Reserve(1);
				FLinePortal *port = &Level->linePortals.Last();
				port->Clear();
				port->mOrigin = ends[k];
				port->mDestination = ends[1 - k];
				port->mType = PORTT_LINKED;
				port->mAlign = PORG_ABSOLUTE;
				port->mDefFlags = PORTF_TYPEINTERACTIVE;
			}
			if (exact) made++;
			log.Line("  portal  line %-5d (%.0f,%.0f)-(%.0f,%.0f) sector %-4d  <->  line %-5d"
				" (%.0f,%.0f)-(%.0f,%.0f) sector %-4d  displacement (%.0f, %.0f)%s",
				pp.first, la->v1->fX(), la->v1->fY(), la->v2->fX(), la->v2->fY(),
				la->frontsector ? la->frontsector->sectornum : -1,
				pp.second, lb->v1->fX(), lb->v1->fY(), lb->v2->fX(), lb->v2->fY(),
				lb->frontsector ? lb->frontsector->sectornum : -1, d1.X, d1.Y,
				exact ? "" : "  NOT AN EXACT PAIR -- left as a wall");
			if (!exact)
			{
				// With no portal a one-sided line must be the wall it looks like.
				la->flags |= ML_BLOCKING;
				lb->flags |= ML_BLOCKING;
			}
		}
		log.Line("  linked line portal pairs  %d", made);
		log.Count("portals: cut pairs that were not an exact translation (no portal made)", bad);
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
	log.Line("  wall horizontal extent %d unfitted face(s) carry a stored extent, %d carry none"
		" (one column repeated, as the original draws it)",
		storedExtentScaled, storedExtentUnusable);
	log.Line("    of those, %d have an extent that differs from the wall's length", storedExtentDeviates);
	log.Line("  image fit             %d faces: %d pieces fitted, %d upper/lower pieces left"
		" unfitted (the original clears the flag on them)",
		imageFitFaces, fittedPieces, fitClearedPieces);
	log.Line("  x-flipped             %d faces (mirrored image, positive scale)", flippedFaces);
	log.Line("  baked wall images     %d pieces (non-power-of-two masks, see-through bands)", bakedWallPieces);
	log.Count("walls: piece too large to bake -- drawn from the plain image", bakeTooLarge);
	log.Line("  shifts applied        %d faces", shiftedFaces);
	log.Line("  records the Python's 15-bit extent would have misread  %d",
		extentBitsAbove12);
	log.Line("  door mid pieces handed to a swinging leaf  %d", doorMidToLeaf);
	log.Count("walls: no texture-map record", noTextureMap);
	log.Count("walls: mid texture wanted but absent", midMissing);
	log.Count("walls: FF_EDGE_MAP faces -- sky above the wall top not modelled", edgeMapFaces);
	log.Count("walls: pieces on the colour key -- left undrawn, as the original does", keyWallPieces);
	log.Line("  FF_TRANS_UPLO mid pieces banded  %d", transUpLoBanded);
	log.Count("walls: FF_TRANS_UPLO faces with no band (opaque or override 0)", transUpLoFaces);
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
		if (portalPlan.active)
		{
			// The start is a Realms coordinate; if it stands in a moved sector
			// it moves with it. Where two sectors share the spot, the height
			// decides.
			const int startSector = rothplan::SectorAt(rm, double(rm.metadata.startX),
				double(rm.metadata.startY), double(rm.metadata.startZ));
			const int sg = portalPlan.GroupOf(startSector);
			mt.pos.X += portalPlan.offX[sg];
			mt.pos.Y += portalPlan.offY[sg];
			log.Line("  player start    in Realms sector %d, group %d", startSector, sg);
		}
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

		// A swapped-in picture on a FLIP_X face is drawn from the X-mirrored
		// registration (roth_surface WallToEngine), which only a picture has.
		auto registerMirrored = [&](int index)
		{
			if (!haveArt || index < 0 || index >= (int)roth::TEX_PICTURE_LIMIT || art.IsSkySurface(index)) return;
			roth::RegisterMirroredTexture(index, art.World(index, &log, false, true));
		};

		for (const roth::Command &c : rm.commands)
		{
			if ((c.opcode & 0x7f) != 0x34) continue;
			FTextureID tex = worldTex((int)c.aux);
			if (!tex.isValid()) continue;
			roth::RegisterTexture((int)c.aux, tex);
			registerMirrored((int)c.aux);
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
				registerMirrored(slots[k]);
				advSlots++;
			}
		}
		if (advKeysHit + advKeysMiss > 0)
			log.Line("  logic walls      0x0c: %d slot(s); keys on a face %d/%d",
				advSlots, advKeysHit, advKeysHit + advKeysMiss);

		// The counter and breakage opcodes paint numbers the map need not wear
		// anywhere (roth_cmdcore.cpp): 0x1f writes count - 1 + word[+0x10] as
		// its count runs from 0 up to its limit (raw_commands.c:686-698), and
		// 0x2e flips bit 0 of a slot -- Realms keeps a wall's whole and broken
		// pictures as an even/odd pair (:361-389) -- on the walls its key names,
		// or with key 0 on whichever wall was used.
		int countTex = 0;
		for (const roth::Command &c : rm.commands)
		{
			const uint8_t op = (uint8_t)(c.opcode & 0x7f);
			if (op == 0x1F)
			{
				const int top = (c.Word(0x08) > c.Word(0x0C) ? c.Word(0x08) : c.Word(0x0C)) + 1;
				for (int k = 0; k <= top && k < 0x100; k++)
				{
					const int ti = (uint16_t)(k - 1 + c.Word(0x10));
					FTextureID t = worldTex(ti);
					if (!t.isValid()) continue;
					roth::RegisterTexture(ti, t);
					countTex++;
				}
			}
			else if (op == 0x2E)
			{
				static const int dirSlot[4] = { 0, 2, 1, 0 };   // {2,6,4,2}: mid, lower, upper, mid
				const int which = dirSlot[c.Word(0x06) & 3];
				for (const roth::TextureMap &tm : rm.textureMaps)
				{
					if (c.key != 0 && (!tm.extended || tm.faceID != c.key)) continue;
					const uint16_t v = which == 0 ? tm.midTexture : which == 1 ? tm.upperTexture : tm.lowerTexture;
					for (int pair = 0; pair < 2; pair++)
					{
						const int ti = pair == 0 ? (v & ~1) : (v | 1);
						FTextureID t = worldTex(ti);
						if (!t.isValid()) continue;
						roth::RegisterTexture(ti, t);
						countTex++;
					}
				}
			}
			else if (op == 0x1C)
			{
				// cmd_cycle_texture: every sector the key names takes the
				// reference sector's floor (or ceiling, flag 1) plus one, once
				// per run (:973-1027). Sixteen runs ahead are made ready.
				const bool ceiling = (c.Word(0x06) & 1) != 0;
				for (const roth::Sector &rs : rm.sectors)
				{
					if (rs.commandID != c.key && rs.commandID != c.Word(0x0A)) continue;
					const int base = ceiling ? rs.ceilingTexture : rs.floorTexture;
					for (int k = 1; k <= 16; k++)
					{
						const int fi = base + k;
						const bool isSky = haveArt && art.IsSkySurface(fi);
						roth::RegisterFlat(fi, isSky ? FNullTextureID() : worldTex(fi), isSky, flatOpaque256For(fi));
					}
				}
			}
		}
		if (countTex > 0)
			log.Line("  logic counters   %d texture(s) for opcodes 0x1f/0x2e", countTex);

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
