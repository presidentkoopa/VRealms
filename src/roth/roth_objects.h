#pragma once

#include <stdint.h>
#include <utility>
#include <vector>
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
class AActor;
class PClassActor;
struct FMemoryMeshData;
class FTextureID;

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

// WHICH ACTOR A REALMS OBJECT BECAME, and the angle convention it was spawned
// with. Both exist because the level LOGIC can move an object after it has
// spawned -- rotate it, raise it, repaint it -- and a command names the object
// by its place in the map while the world only knows the actor.
//
// Addressed by the Realms sector that owns the object and the object's index
// within that sector, which is how the map itself stores them, so no third
// numbering has to be invented or kept in step.
//
// Null when the object never spawned: an object outside the pack's art, or one
// whose spawn failed, is simply absent rather than an error at every use.
AActor *FindObjectActor(int rothSector, int rothIndex);

// AND THE OTHER WAY ROUND: which Realms object an actor is. The counterpart of
// FindObjectActor, needed by anything that starts from the WORLD rather than
// from the map -- a ray that hits a prop, a hand that closes on one -- because
// all of those know an actor and need the object the logic addresses.
//
// False, with both outputs untouched, when the actor is not a Realms object.
// That is the ordinary answer for every Doom actor in the level, so callers
// test it rather than treating it as a fault.
bool ObjectForActor(const AActor *mo, int &rothSector, int &rothIndex);

// Replace both indexes with `current`: ((sector, index), actor) for every
// object that has an actor now. For the actor rules, whose list edits move
// records (roth_actors_engine.cpp).
void RekeyObjectActors(const std::vector<std::pair<std::pair<int, int>, AActor *>> &current);

// After the engine replaced every thinker (a hub snapshot coming back, a
// savegame loading), find the props again. Each carries its (sector, index)
// in `special2`, set at spawn, which the actor serializer keeps.
void RebindObjectActors(FLevelLocals *Level);

// A sprite for a texture the LOGIC can ask for, looked up by the packed texture
// word a command record carries -- the object's +0x04 pair, texture index in the
// low byte and source in the high one.
//
// Built at load for the same reason wall textures are: opcode 0x0d repaints a
// prop to a texture that may appear on no object in the map, and the art pack is
// a loader local that does not outlive the load, so nothing can be resolved
// later. Negative when the loader never built one.
int FindLogicSprite(uint16_t textureWord);

// A SPRITE FRAME FOR A MESH SOME OTHER PART OF THE LOADER BUILT, bound to an
// actor class the way prop meshes are bound to LoaderProp -- in memory, nothing
// written anywhere. roth_platforms uses it for the boxes a moving sector
// becomes. `key` names the geometry: the same key reuses the model already
// registered, so reloading a map does not grow the model table.
//
// Returns the sprite index to put in AActor::sprite (frame 0), or -1. Must be
// called after PrepareObjects in the same load, because that is where the
// sprite table and the added model frames are reset; `data` is consumed.
int BindMemoryMesh(const char *key, FMemoryMeshData &&data, PClassActor *cls, Log *log);

// The rotation byte -> yaw conversion the objects were built with, exported so a
// later rotation lands on the same convention as the original spawn instead of a
// second derivation that can drift from it.
double ObjectYaw(uint8_t rotation);

// And its inverse, for a command that turns an object to face something in the
// world: the bearing arrives in degrees and the map holds a byte.
uint8_t RotationFromYaw(double deg);

// The yaw the ACTOR of an object wants for a rotation byte. Not ObjectYaw: a
// mesh and a fixed-angle picture each sit at a fixed turn from the object's
// facing (roth_objects.cpp, YawOffsetMesh / YawOffsetFixedQuad), and a command
// that turns one must keep that turn.
double ObjectActorYaw(const AActor *mo, uint8_t rotation);

// A PARTICLE (opcode 0x2d, roth_cmdcore.h): an actor wearing the logic sprite
// for `textureWord` at a Realms-absolute position already moved into the
// engine's frame. Null when no sprite was built for the word at load. The
// light is the combined level the caller worked out, or -1 for the sector's.
AActor *SpawnLogicSpriteActor(FLevelLocals *Level, uint16_t textureWord, double x, double y, double z, int lightLevel);
// Move one to where its particle now is, with the same anchor shift.
bool MoveLogicSpriteActor(AActor *mo, uint16_t textureWord, double x, double y, double z);

// AN OBJECT THE LOADER DID NOT SPAWN, given an actor at run time by the actor
// rules' presentation (roth_actors_engine.cpp): a creature placeholder, a prop
// hidden at load and later shown, a record a command or a shot created. The
// actor is findable by (sector, index) like a loaded one -- FindObjectActor,
// ObjectForActor, a restore -- and wears whatever sprite its caller gives it.
AActor *SpawnObjectActor(FLevelLocals *Level, int rothSector, int rothIndex, double x, double y, double z);
// A sprite definition of one frame, `views` 1 or 8 pictures (each 8-view
// picture fills two of the sixteen rotation slots, as for directional props);
// `flip` may be null. -1 when the sprite table is full. The table is rebuilt
// on every map load, so a caller's memo of these must be dropped with it.
int MakeObjectSprite(const FTextureID *tex, const bool *flip, int views, Log *log);

} // namespace roth
