#pragma once
//
// Realms of the Haunting's level logic, running.
//
// roth_commands is the engine-free SPINE -- it walks and executes chains but has
// no idea what a command DOES. This is the other half: it keeps the level's
// parsed logic alive for the session, binds triggers to the geometry they watch,
// and implements the handlers against the live world.
//
// The decoded opcode set is in ROTH_COMMANDS.md; the door behaviour this drives
// is read from doors.c and cited where it is used.
//

#include <stdint.h>

struct line_t;
struct sector_t;
class AActor;
struct FLevelLocals;

namespace roth
{

struct Map;
class Log;

// Hand the level's logic to the runtime. Copies what it needs, so the loader's
// Map can go out of scope. Call once per load, before the player spawns.
void BeginLevel(const Map &map, FLevelLocals *level, Log *log);

// Forget it again. Safe to call when no level is active.
void EndLevel();

// A door panel's polyobject tag, recorded by the loader as it builds them, so a
// command that names a door can find the thing that swings. Keyed by the Realms
// SECTOR index of the doorway.
void RegisterDoor(int rothSector, int polyTag);

// Which engine sidedef a Realms face became. Only the loader knows both, and a
// trigger names a FACE while the engine hands us a sidedef, so without this the
// two index spaces never meet.
void RegisterFaceSide(int rothFace, int sideIndex);

// The player used a wall. Returns true if a Realms chain fired, in which case
// the engine's own line activation should not also run.
//
// Hooked at the top of P_ActivateLine: Realms triggers are not Doom line
// specials and must be tested before the engine decides the line is inert.
bool ActivateLine(line_t *line, AActor *who, int side);

// The player entered a sector. Fires the enter-sector triggers bound to it.
void CrossSector(sector_t *sec, AActor *who);

} // namespace roth
