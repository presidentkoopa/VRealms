#pragma once
//
// Placing Realms of the Haunting's objects -- the furniture, the decoration, the
// suit of armour, the chairs and the desks -- into a level the engine reads
// straight out of the player's own installation.
//
// NOTHING IS WRITTEN TO DISK. The pictures, the 3D meshes and the actors that
// carry them are all built in memory while the level loads. The retired Python
// pipeline did this with generated sprite lumps, DECORATE and MODELDEF; none of
// that exists here and none of it may.
//
// TWO PHASES, and they cannot be one. LoadRothMap runs before the level has a
// BSP or a blockmap, and an actor cannot be spawned into a world it cannot be
// linked to. So Prepare decides what every object becomes and builds all of its
// artwork -- which is where every interesting decision is, and therefore where
// all the logging is -- and Spawn puts the actors in once the level is ready.
// The two are called from opposite ends of MapLoader::LoadLevel.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

struct FLevelLocals;

namespace roth
{

struct Map;
class Log;
class TextureSet;

// Work out what each object in `rm` becomes and build its artwork. `levelArt`
// is the map's own pack, already open; the shared sprite pack is opened here
// because only objects use it. Everything is reported to `log`, which may be
// null. Safe to call for a map with no objects.
void PrepareObjects(const Map &rm, TextureSet &levelArt, Log *log);

// Spawn what Prepare decided. Must run after the level is linkable -- after
// SpawnThings -- and does nothing at all when Prepare was not called, so it is
// harmless on an ordinary Doom map.
void SpawnPreparedObjects(FLevelLocals *Level);

} // namespace roth
