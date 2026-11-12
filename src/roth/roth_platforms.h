#pragma once
//
// Moving sectors as solid platform ACTORS -- cvar roth_platforms.
//
// Realms of the Haunting's RAW opcode 0x09 slides a sector sideways by moving
// its four vertices every frame. A Doom BSP is static, so that sector cannot
// slide here. What CAN slide is an actor: roth_platformplan.h decides which
// moving sectors are, visibly, "a slab or a block travelling along a level
// channel", the map loader takes that slab or block out of the static map, and
// this file puts it back as a platform actor -- a box mesh built in memory from
// the sector's own flats, and a row of solid boxes that carry whatever stands on
// them.
//
// THE PLATFORM PHYSICS ARE NOT OURS. The actor class is LoaderPlatform, a small
// subclass of FishyPlatform from "3D Platform" by FishyClock (GPL-3.0,
// https://github.com/FishyClock/3Dplatform), shipped intact under
// wadsrc/static/zscript/fishy/. Carrying passengers, pushing what is in the
// way, grouping several boxes into one rigid body and crossing portals are all
// that library's work.
//
// THE GAME LOGIC IS NOT THE LIBRARY'S. Where the piece is on a given tic is
// decided by the transcription of tick_moving_sector in roth_runtime.cpp, in
// Realms' own 16-bit vertex coordinates, exactly as ROTH.C computes it. The
// actor is told where to be and follows; if something solid holds it up, the
// logic does not wait for it and the actor catches up when it can.
//
// NOTHING IS WRITTEN TO DISK, as everywhere else in this loader: the mesh is a
// FMemoryMeshModel and its textures are the ones the map already registered.
//
// THREE PHASES, for the reason roth_objects.h gives: the loader describes the
// pieces while there is no BSP to link an actor into (AddPlatformPiece), the
// actors are spawned once the level is linkable (SpawnPreparedPlatforms, called
// beside SpawnPreparedObjects), and the level logic drives them every world
// step (DrivePlatform).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <utility>
#include <vector>
#include "textureid.h"

struct FLevelLocals;

namespace roth
{

// How one horizontal face is textured, in the ENGINE's plane terms -- the same
// five numbers the loader gives a sector plane (rothmap.cpp, FlatToEngine), so
// the box's top draws texel for texel what the floor it replaces drew.
struct PlatformFlat
{
	FTextureID tex = FNullTextureID();
	double xScale = 1., yScale = 1., xOffset = 0., yOffset = 0., angle = 0.;
};

// One vertical piece of one moving sector. Kinds are rothplat::PieceKind.
struct PlatformPiece
{
	int rothSector = -1;
	int kind = 0;
	double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // footprint at load, engine coordinates
	double z0 = 0, z1 = 0;                   // vertical extent at load
	PlatformFlat top, bottom;                // an invalid texture leaves that face out
	FTextureID side[4];                      // south, east, north, west
	// The solid boxes: square footprints of one radius, centred here.
	std::vector<std::pair<double, double>> centres;
	double radius = 0;
	int lightLevel = -1;                     // the moving sector's own, at load
};

// Forget everything, and latch whether this load builds platforms at all (the
// value of roth_platforms when the load began). Called at the start of every
// Realms load, so the level logic and the loader cannot disagree mid-level if
// the cvar is changed in the console afterwards.
void ResetPlatforms(bool enabled);
bool PlatformsEnabled();

// Loader: describe one piece. Nothing is spawned yet.
void AddPlatformPiece(const PlatformPiece &piece);

// Spawn what was described. After SpawnPreparedObjects, because that is where
// the sprite table is reset for this load. Does nothing when nothing was added,
// so it is inert on every other map and whenever roth_platforms is 0.
void SpawnPreparedPlatforms(FLevelLocals *Level);

// Has this Realms sector been turned into a platform?
bool SectorIsPlatform(int rothSector);

// Did the loader take this sector's floor (or ceiling) out of the static map
// and give it to a platform? The level logic must then leave the ENGINE plane
// alone when it changes the Realms height or flat: the static plane is the
// neighbours' now, and the platform follows the Realms values by itself.
bool PlatformOwnsPlane(int rothSector, bool ceiling);

// The vertical state of a moving sector as the Realms records hold it NOW.
struct PlatformHeights
{
	double floorZ = 0, ceilingZ = 0;         // the sector's own +0x02 and +0x00
	double slabBottom = 0, slabTop = 0;      // its mid-platform, when it has one
};

// Level logic, once per world step for each platform sector: where the sector
// has slid to (a displacement from where it was at load), its heights and its
// light. Cheap when nothing changed.
void DrivePlatform(FLevelLocals *Level, int rothSector, double dx, double dy,
	const PlatformHeights &now, int lightLevel);

} // namespace roth
