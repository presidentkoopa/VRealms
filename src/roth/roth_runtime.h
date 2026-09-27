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
// WHAT A HANDLER IS, IN THE ORIGINAL. Read this before adding one. The 0x30780
// dispatch table (transcribed at platform/boot.c:189-205) splits into two very
// different kinds of entry:
//
//   IMMEDIATE handlers do the thing and return -- 0x2f open a door, 0x17 toggle
//   another command, 0x26 set a flag, 0x3b change map.
//
//   REGISTRARS do not do the thing at all. They allocate an ACTIVE EFFECT
//   record and return; the visible behaviour lives in a separate per-frame TICK
//   handler reached through a second table (0x3088c, boot.c:215-229). Every
//   light, texture, height and sector-move opcode is one of these.
//
// So "implement the lighting opcode" is not one function: it is the active-effect
// pool plus its tick. That machinery is NOT built yet, and the registrar opcodes
// are counted as unimplemented rather than approximated. The load report carries
// a per-opcode census saying which is which.
//

#include <stdint.h>

#include "textureid.h"

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
//
// The progress FLAGS are deliberately NOT forgotten: they are global and
// persistent in the original (ROTH_COMMANDS.md, "flags, items and dialogue all
// live outside the map") and savegame chunk 6 writes the whole bitmap. Use
// ResetProgressFlags for a new game.
void EndLevel();

// A door panel's polyobject tag, recorded by the loader as it builds them, so a
// command that names a door can find the thing that swings. Keyed by the Realms
// SECTOR index of the doorway.
void RegisterDoor(int rothSector, int polyTag);

// Which engine sidedef a Realms face became. Only the loader knows both, and a
// trigger names a FACE while the engine hands us a sidedef, so without this the
// two index spaces never meet.
void RegisterFaceSide(int rothFace, int sideIndex);

// A Realms texture index and the engine texture the loader built for it.
//
// The loader's TextureSet is a local that does not outlive the load, so an
// opcode that repaints a wall at run time cannot resolve an index itself. The
// loader registers every index its command records can name, which is a few per
// map, and the runtime looks up rather than resolving. Call before BeginLevel.
void RegisterTexture(int rothIndex, FTextureID tex);

// The same, for a FLAT the logic can swap onto a floor or ceiling (0x0a, 0x0b).
// Separate from RegisterTexture because a flat index can be the pack's SKY
// MARKER, which resolves to no texture at all and means "draw the sky here" --
// a distinction the runtime cannot make for itself and must not lose.
void RegisterFlat(int rothIndex, FTextureID tex, bool isSky);

// Which engine sector became the 3D-floor CONTROL sector for a Realms sector's
// mid-platform. Only the loader knows the pairing, and the level logic needs it:
// several opcodes reach a sector's mid-platform through its +0x18 to move the
// slab's heights or swap its faces, and on this side a mid-platform IS a control
// sector's two planes. Without this those paths have nowhere to write.
void RegisterPlatformControl(int rothSector, int ctrlSector);

// The player used a wall. Returns true if a Realms chain fired, in which case
// the engine's own line activation should not also run.
//
// Hooked at the top of P_ActivateLine: Realms triggers are not Doom line
// specials and must be tested before the engine decides the line is inert.
bool ActivateLine(line_t *line, AActor *who, int side);

// Fire the SECTOR-keyed triggers bound to a sector -- the 0x19 and 0x31 marks.
//
// NOTHING CALLS THIS YET, DELIBERATELY. In the original these bits do not mean
// "the player is in this sector": they are a fast-reject gate inside
// dispatch_entry_command_trigger (raw_commands.c:3058), which runs from the USE
// and cursor-probe paths and matches the sector against the object-table refs
// this port does not build yet. There is no movement callback to hook it to.
//
// It is left here rather than deleted because it is the correct half -- the
// binding and the firing are right, only the caller is missing. Wiring it to a
// sector-entry event would fire these triggers on walking through a doorway,
// which the original does not do; see IsSectorTrigger for the trace.
void FireSectorTriggers(sector_t *sec, AActor *who);

// One WORLD STEP of Realms' own level logic: the delay countdowns and the
// deferred command queue the original drives from tick_world_effects
// (raw_commands.c:3288). Inert when no Realms level is loaded.
void TickLevelLogic(FLevelLocals *level);

// Wipe the persistent progress flags. A new game, not a level change.
void ResetProgressFlags();

} // namespace roth
