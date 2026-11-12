/*
** roth_platforms.cpp
**
** Moving sectors as solid platform actors. See roth_platforms.h for the shape
** of the thing and for whose work the platform physics are (FishyClock's
** "3D Platform", GPL-3.0, https://github.com/FishyClock/3Dplatform).
**
** NOT COMPILED BY THE LANE THAT WROTE IT beyond g++ -fsyntax-only against the
** engine's headers (tools/roth_syntaxcheck.sh), and never run. Every claim
** about what it looks like in a headset is a prediction.
**
**---------------------------------------------------------------------------
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/

#include "roth_platforms.h"
#include "roth_platformplan.h"   // rothplat::PieceKind
#include "roth_objects.h"        // BindMemoryMesh
#include "roth_log.h"

#include "actor.h"
#include "info.h"
#include "g_levellocals.h"
#include "texturemanager.h"
#include "gametexture.h"
#include "printf.h"
#include "vm.h"
#include "model_memorymesh.h"

#include <map>
#include <math.h>
#include <stdio.h>
#include <string>

namespace roth
{

namespace
{

std::vector<PlatformPiece> gPieces;

// What the level logic needs to find and steer a moving sector's actors.
struct SectorEntry
{
	int tid = 0;                     // every actor of this sector carries it
	bool hasSlab = false;
	double loadSlabBottom = 0;       // the slab's underside at load: its Z follows
	double nbFloor = 0, nbCeil = 0;  // where a floor block stands / a ceiling block hangs
	bool ownsFloor = false, ownsCeiling = false;
};
std::map<int, SectorEntry> gSectors;

PClassActor *PlatformClass()
{
	// Declared in the engine's own zscript on top of the library's base class:
	// wadsrc/static/zscript/actors/loaderplatform.zs.
	static PClassActor *cls = nullptr;
	if (cls == nullptr) cls = PClass::FindActor("LoaderPlatform");
	return cls;
}

//==========================================================================
//
// Where a flat's texture lands on a world point, as the hardware renderer
// computes it -- so a box's top samples exactly the texel the sector plane it
// replaces sampled.
//
// The renderer gives a flat vertex (x/64, -y/64) (hw_vertexbuilder.cpp:176) and
// then a texture matrix built in this order (hw_flats.cpp:91-95):
//
//     scale(xscale, yscale) * translate(xoffs/w, yoffs/h)
//         * scale(64/w, 64/h) * rotate(-angle)
//
// which applies to the coordinate right to left. The same arithmetic, written
// out. The texture then RIDES the box as it slides, where a real flat is pinned
// to the world -- and that is what the original does too: every 0x09 record
// that is built here folds its per-frame delta into the flat's shift bytes
// (raw_commands.c:5917-5929), which is precisely "keep the picture on the
// moving sector".
//
//==========================================================================

FVector2 FlatUV(const PlatformFlat &f, double x, double y)
{
	double w = 64., h = 64.;
	if (FGameTexture *gt = TexMan.GetGameTexture(f.tex, true))
	{
		if (gt->GetDisplayWidth() > 0)  w = gt->GetDisplayWidth();
		if (gt->GetDisplayHeight() > 0) h = gt->GetDisplayHeight();
	}
	const double a = -f.angle * (3.14159265358979323846 / 180.);
	const double co = cos(a), si = sin(a);
	const double u0 = x / 64., v0 = -y / 64.;
	double u = u0 * co - v0 * si;
	double v = u0 * si + v0 * co;
	u = u * (64. / w) + f.xOffset / w;
	v = v * (64. / h) + f.yOffset / h;
	return FVector2(float(u * f.xScale), float(v * f.yScale));
}

//==========================================================================
//
// The box. Model space puts the vertical in the MIDDLE (model_memorymesh.h),
// and the origin is the first solid box's centre at the piece's bottom, which
// is the actor that wears the mesh.
//
// Both windings are emitted. The winding the model path culls by was not
// established for this geometry, and a platform that is invisible from above is
// a worse failure than twelve extra triangles.
//
// The SIDES have no art of their own for a slab (rothmap.cpp makes the same
// call for the 3D floor's edge and uses the top's). They are mapped two world
// units to the texel, Realms' ordinary wall density, from the top edge down.
// That density is a presentation choice here, not something read out of ROTH.C.
//
//==========================================================================

FMemoryMeshData BuildBox(const PlatformPiece &p, double ox, double oy)
{
	FMemoryMeshData data;
	const double xs[2] = { p.x0, p.x1 }, ys[2] = { p.y0, p.y1 }, zs[2] = { p.z0, p.z1 };

	// Corner i: bit 0 = x1, bit 1 = y1, bit 2 = top.
	data.vertices.Resize(8);
	for (int i = 0; i < 8; i++)
		data.vertices[i] = FVector3(float(xs[i & 1] - ox), float(zs[(i >> 2) & 1] - p.z0),
			float(ys[(i >> 1) & 1] - oy));

	auto flatFace = [&](const PlatformFlat &f, int a, int b, int c, int d)
	{
		if (!f.tex.isValid()) return;
		FMemoryMeshFace face;
		face.count = 4;
		const int idx[4] = { a, b, c, d };
		for (int k = 0; k < 4; k++)
		{
			face.vertex[k] = idx[k];
			face.uv[k] = FlatUV(f, xs[idx[k] & 1], ys[(idx[k] >> 1) & 1]);
		}
		face.skin = f.tex;
		data.faces.Push(face);
	};
	flatFace(p.top, 4, 5, 7, 6);
	flatFace(p.bottom, 0, 2, 3, 1);

	// south (y0), east (x1), north (y1), west (x0): bottom pair, then top pair.
	static const int sideCorners[4][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 } };
	const double height = p.z1 - p.z0;
	for (int s = 0; s < 4; s++)
	{
		if (!p.side[s].isValid()) continue;
		double w = 64., h = 64.;
		if (FGameTexture *gt = TexMan.GetGameTexture(p.side[s], true))
		{
			if (gt->GetDisplayWidth() > 0)  w = gt->GetDisplayWidth();
			if (gt->GetDisplayHeight() > 0) h = gt->GetDisplayHeight();
		}
		const int a = sideCorners[s][0], b = sideCorners[s][1];
		const double len = fabs(xs[b & 1] - xs[a & 1]) + fabs(ys[(b >> 1) & 1] - ys[(a >> 1) & 1]);
		const float u1 = float(len / (2. * w)), v1 = float(height / (2. * h));

		FMemoryMeshFace face;
		face.count = 4;
		face.vertex[0] = a + 4;  face.uv[0] = FVector2(0.f, 0.f);
		face.vertex[1] = a;      face.uv[1] = FVector2(0.f, v1);
		face.vertex[2] = b;      face.uv[2] = FVector2(u1, v1);
		face.vertex[3] = b + 4;  face.uv[3] = FVector2(u1, 0.f);
		face.skin = p.side[s];
		data.faces.Push(face);
	}

	data.doubleSided = true;
	return data;
}

// Everything that decides what the mesh looks like, so that the same piece on
// a reload finds the model it registered last time (BindMemoryMesh).
std::string MeshKey(const PlatformPiece &p, double ox, double oy)
{
	char buf[512];
	snprintf(buf, sizeof buf,
		"platform/%g,%g,%g,%g,%g,%g@%g,%g/t%d:%g,%g,%g,%g,%g/b%d:%g,%g,%g,%g,%g/s%d,%d,%d,%d",
		p.x0, p.y0, p.x1, p.y1, p.z0, p.z1, ox, oy,
		p.top.tex.GetIndex(), p.top.xScale, p.top.yScale, p.top.xOffset, p.top.yOffset, p.top.angle,
		p.bottom.tex.GetIndex(), p.bottom.xScale, p.bottom.yScale, p.bottom.xOffset,
		p.bottom.yOffset, p.bottom.angle,
		p.side[0].GetIndex(), p.side[1].GetIndex(), p.side[2].GetIndex(), p.side[3].GetIndex());
	return buf;
}

} // namespace

//==========================================================================
//
//
//
//==========================================================================

static bool gEnabled = false;

void ResetPlatforms(bool enabled)
{
	gPieces.clear();
	gSectors.clear();
	gEnabled = enabled;
}

bool PlatformsEnabled()
{
	return gEnabled;
}

void AddPlatformPiece(const PlatformPiece &piece)
{
	if (piece.rothSector < 0 || piece.centres.empty() || piece.radius <= 0) return;
	if (piece.z1 <= piece.z0) return;
	gPieces.push_back(piece);

	// Recorded NOW, not at spawn: the level logic asks PlatformOwnsPlane while
	// the map is still loading (the load-time lights-out pass, BeginLevel).
	SectorEntry &se = gSectors[piece.rothSector];
	if (piece.kind == rothplat::PIECE_SLAB)
	{
		se.hasSlab = true;
		se.loadSlabBottom = piece.z0;
	}
	else if (piece.kind == rothplat::PIECE_FLOORBLOCK)
	{
		se.ownsFloor = true;
		se.nbFloor = piece.z0;
	}
	else if (piece.kind == rothplat::PIECE_CEILBLOCK)
	{
		se.ownsCeiling = true;
		se.nbCeil = piece.z1;
	}
}

bool SectorIsPlatform(int rothSector)
{
	return gSectors.find(rothSector) != gSectors.end();
}

bool PlatformOwnsPlane(int rothSector, bool ceiling)
{
	auto it = gSectors.find(rothSector);
	if (it == gSectors.end()) return false;
	return ceiling ? it->second.ownsCeiling : it->second.ownsFloor;
}

//==========================================================================
//
// SpawnPreparedPlatforms
//
// One actor per solid box. Every actor of one moving sector shares a TID, and
// when there is more than one they name that TID as their group
// (FishyPlatform's args[2]), which is how the library makes several boxes one
// rigid body: its PostBeginPlay reads the argument on the first tick.
//
// args[1] are the library's option flags, and both matter:
//     1   linear path. The driver hands over one straight step at a time.
//     32  do not clip against geometry or other platforms. Realms never tests
//         a moving sector against anything -- its neighbours stretch out of the
//         way by construction -- and the box sits exactly wall to wall in its
//         channel, where a clipping test can only ever refuse wrongly.
//
// The mesh is worn by the FIRST box of a piece and the rest are invisible, so a
// rectangle covered by two overlapping squares is still drawn once.
//
//==========================================================================

void SpawnPreparedPlatforms(FLevelLocals *Level)
{
	if (gPieces.empty() || Level == nullptr) return;

	PClassActor *cls = PlatformClass();
	if (cls == nullptr)
	{
		Printf(TEXTCOLOR_RED "Realms: roth_platforms is on but the LoaderPlatform actor class "
			"is missing -- no moving sector was built\n");
		gPieces.clear();
		gSectors.clear();
		return;
	}

	// How many actors each sector will have, to know who needs a group.
	std::map<int, int> actorsOf;
	for (const PlatformPiece &p : gPieces) actorsOf[p.rothSector] += (int)p.centres.size();

	int spawned = 0, failed = 0, meshless = 0, nextTid = 29000;
	for (const PlatformPiece &p : gPieces)
	{
		SectorEntry &se = gSectors[p.rothSector];
		if (se.tid == 0)
		{
			se.tid = Level->FindUniqueTID(nextTid, 3000, false);
			if (se.tid == 0) { failed += (int)p.centres.size(); continue; }
			nextTid = se.tid + 1;
		}

		const double ox = p.centres[0].first, oy = p.centres[0].second;
		const std::string key = MeshKey(p, ox, oy);
		const int sprite = BindMemoryMesh(key.c_str(), BuildBox(p, ox, oy), cls, &TheLog());
		if (sprite < 0) meshless++;

		for (size_t i = 0; i < p.centres.size(); i++)
		{
			AActor *mo = AActor::StaticSpawn(Level, cls,
				DVector3(p.centres[i].first, p.centres[i].second, p.z0), NO_REPLACE);
			if (mo == nullptr) { failed++; continue; }

			FLinkContext ctx;
			mo->UnlinkFromWorld(&ctx);
			mo->radius = p.radius;
			mo->Height = p.z1 - p.z0;
			mo->LinkToWorld(&ctx);
			// StaticSpawn may have clamped Z to the floor; the map's is absolute.
			mo->SetZ(p.z0);
			// Where it was at load. The driver's displacement is added to this,
			// and it is saved with the actor.
			mo->SpawnPoint = DVector3(p.centres[i].first, p.centres[i].second, p.z0);

			mo->args[0] = 0;
			mo->args[1] = 1 | 32;
			mo->args[2] = actorsOf[p.rothSector] > 1 ? se.tid : 0;
			mo->args[3] = 0;
			mo->args[4] = 0;
			mo->SetTID(se.tid);

			// Which piece this is and how tall it was, for DrivePlatform. Neither
			// field is used by the library.
			mo->special1 = p.kind;
			mo->specialf1 = p.z1 - p.z0;

			if (i == 0 && sprite >= 0)
			{
				mo->sprite = sprite;
				mo->frame = 0;
				// The mesh spans the whole footprint, which is wider than this
				// one box whenever the footprint is not square.
				const double rx = (p.x1 - p.x0), ry = (p.y1 - p.y0);
				mo->renderradius = sqrt(rx * rx + ry * ry);
			}
			else
			{
				mo->renderflags |= RF_INVISIBLE;
			}
			if (p.lightLevel >= 0) mo->LightLevel = (int16_t)p.lightLevel;
			spawned++;
		}
	}

	const std::string &path = TheLog().Path();
	if (!path.empty())
	{
		if (FILE *f = fopen(path.c_str(), "a"))
		{
			fprintf(f, "\nMoving-sector platforms spawned (roth_platforms)\n"
				"------------------------------------------------\n");
			fprintf(f, "  moving sectors        %d\n", (int)gSectors.size());
			fprintf(f, "  pieces                %d\n", (int)gPieces.size());
			fprintf(f, "  actors spawned        %d\n", spawned);
			fprintf(f, "  spawns that failed    %d\n", failed);
			fprintf(f, "  pieces with no mesh   %d   (solid but invisible)\n", meshless);
			for (const auto &kv : gSectors)
				fprintf(f, "  sector %4d  tid %d%s%s%s\n", kv.first, kv.second.tid,
					kv.second.hasSlab ? "  slab" : "",
					kv.second.ownsFloor ? "  floor-block" : "",
					kv.second.ownsCeiling ? "  ceiling-block" : "");
			fclose(f);
		}
	}
	Printf("Realms: %d platform actor(s) for %d moving sector(s)%s\n", spawned,
		(int)gSectors.size(), failed ? FStringf(", %d failed", failed).GetChars() : "");

	// The descriptions are done with; gSectors is what the level logic uses.
	gPieces.clear();
}

//==========================================================================
//
// DrivePlatform
//
// Tell a moving sector's actors where the Realms records say it is.
//
// The sideways move goes through the library, by one member of the group: its
// LoaderDrive (loaderplatform.zs) asks FishyPlatform for a one-tic straight
// move to SpawnPoint + displacement and runs it at once, so passengers ride,
// things in the way are pushed, and the other boxes of the group come along.
// It is only called when that member is not already where it should be -- so an
// idle platform costs one hash lookup a tic, and one that was blocked is asked
// again every tic until it gets there.
//
// HEIGHTS are applied here directly. A slab keeps its thickness and its Z comes
// with the group move (0x03 sinks RAQUIA3's slabs). A floor block's TOP is the
// sector's floor, so when 0x07 raises that floor the box grows from the ground
// and its mesh is stretched to match (Scale.Y), which is what a rising pillar's
// stretched sides look like in the original. A ceiling block likewise.
//
// KNOWN LIMIT, stated rather than papered over: the vertical part of the group
// move is the slab's alone. A sector with BOTH a slab and a floor block whose
// slab changes height would lift the floor block with it. No retail map has
// that combination (tools/platforms).
//
//==========================================================================

void DrivePlatform(FLevelLocals *Level, int rothSector, double dx, double dy,
	const PlatformHeights &now, int lightLevel)
{
	if (Level == nullptr) return;
	auto it = gSectors.find(rothSector);
	if (it == gSectors.end() || it->second.tid == 0) return;
	const SectorEntry &se = it->second;

	PClassActor *cls = PlatformClass();
	if (cls == nullptr) return;

	const double dz = se.hasSlab ? now.slabBottom - se.loadSlabBottom : 0.;

	AActor *lead = nullptr;
	bool astray = false;
	auto iter = Level->GetActorIterator(se.tid);
	while (AActor *mo = iter.Next())
	{
		if (!mo->IsKindOf(cls)) continue;
		if (lead == nullptr)
		{
			// Only the lead is tested. The library moves a group all-or-nothing
			// (a member that cannot follow sends the whole group back), and the
			// other members' positions come out of its own group arithmetic, so
			// they need not equal SpawnPoint + displacement to the last bit.
			// The tolerance is far below anything visible and far above that.
			lead = mo;
			const DVector3 want = mo->SpawnPoint + DVector3(dx, dy, dz);
			astray = (mo->Pos() - want).LengthSquared() > 1e-6;
		}

		if (lightLevel >= 0 && mo->LightLevel != lightLevel)
			mo->LightLevel = (int16_t)lightLevel;

		double wantH = mo->Height;
		switch (mo->special1)
		{
		case rothplat::PIECE_SLAB:       wantH = now.slabTop - now.slabBottom; break;
		case rothplat::PIECE_FLOORBLOCK: wantH = now.floorZ - se.nbFloor;      break;
		case rothplat::PIECE_CEILBLOCK:  wantH = se.nbCeil - now.ceilingZ;     break;
		}
		if (wantH < 1.) wantH = 1.;          // an actor cannot have no height
		if (wantH != mo->Height)
		{
			mo->Height = wantH;
			if (mo->specialf1 > 0.) mo->Scale.Y = wantH / mo->specialf1;
		}
	}

	if (lead == nullptr || !astray) return;

	IFVM(LoaderPlatform, LoaderDrive)
	{
		VMValue params[] = { lead, dx, dy, dz };
		VMCall(func, params, 4, nullptr, 0);
	}
}

} // namespace roth
