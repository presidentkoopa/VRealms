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
//
// `opaque256` travels with the registration because the runtime cannot see the
// artwork -- the loader's TextureSet does not outlive the load -- yet it needs
// the SAME scale rule the loader used. 256x256 opaque flats take an exception
// (2^(s-1) units per texel, and a whole-texel shift unit), and a platform
// rescaled by a different rule than the one it was built with changes
// appearance the moment it moves. That is precisely how the loader's and the
// runtime's copies of this code drifted apart before roth_surface existed.
void RegisterFlat(int rothIndex, FTextureID tex, bool isSky, bool opaque256 = false);

// Which engine sector became the 3D-floor CONTROL sector for a Realms sector's
// mid-platform. Only the loader knows the pairing, and the level logic needs it:
// several opcodes reach a sector's mid-platform through its +0x18 to move the
// slab's heights or swap its faces, and on this side a mid-platform IS a control
// sector's two planes. Without this those paths have nowhere to write.
void RegisterPlatformControl(int rothSector, int ctrlSector);

// An engine sector that must stay lit exactly like a Realms sector: a
// mid-platform's control sector, a door leaf's void room. The loader copies
// light at build time; this keeps run-time light changes (switches, fades,
// the load-time lights-out pass) reaching them too.
void RegisterLightFollower(int rothSector, int engineSector);

// The player touched a wall or a door leaf. Returns true if this line belongs
// to Realms, in which case the engine's own line activation must not also run.
//
// Hooked at the top of P_ActivateLine: Realms triggers are not Doom line
// specials and must be tested before the engine decides the line is inert.
//
// `activationType` is the engine's own SPAC_* for the event that arrived, and it
// MATTERS: one Realms line can be reached by a use, a walk-over, a bullet and a
// shove, and GAME_core.md §5.2 gives exactly one of those per opcode. This used
// to take the event and discard it, so every binding answered all four.
bool ActivateLine(line_t *line, AActor *who, int side, int activationType);

// Register one line of a door leaf, by sidedef, against the polyobject tag of
// the panel it belongs to.
//
// A leaf is not a Realms FACE -- the loader generates its four lines, so there
// is no face id and no entry in the face-to-sidedef pairing -- but it IS what
// the player clicks to open the door (GAME_core.md §4.5: a type-6 door goes
// straight to toggle_door_open_state, with no trigger and no command chain).
// Without this the leaf is bare one-sided geometry that silently eats the use
// ray, and nothing behind it can be reached either.
void RegisterDoorLeafSide(int sideIndex, int polyTag);

// Give every Realms line a non-zero `special` and its activation bits, so the
// engine's own dispatch sites offer the line to ActivateLine instead of
// deciding it is inert. Returns how many lines it newly marked.
//
// CALL THIS AFTER ANY ENGINE LOAD PASS THAT WRITES `line->special`. BeginLevel
// runs it once, but PO_Init later zeroes the special of every line it collected
// into a polyobject (polyobjects.cpp:431-433) -- which is every door leaf in
// the map -- and the leaves cannot be marked before then either, because
// SpawnPolyobj needs that same special to find them (:221). Idempotent, so
// calling it again is always safe.
int MarkTriggerLines(FLevelLocals *level);

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
//
// DO NOT "FIX" THIS BY WIRING IT TO SECTOR ENTRY. A session handoff proposed
// exactly that; it is wrong, and the authoritative spec agrees with this
// comment rather than with the handoff. GAME_core.md §5.2 rows 6 and 12 give
// 0x19 as "left-click floor / platform top" and 0x31 as "right-click
// floor/ceiling/platform". They are CLICKS ON A FLOOR, matched by the sector's
// command id. What this port is actually missing is a floor-click dispatch --
// a use or examine ray that lands on a flat rather than a wall.
//
// The real enter/leave-sector trigger is 0x13 (§5.2 row 4), which the loader
// does not classify or bind at all. That one needs a sector-transition event;
// these two do not.
void FireSectorTriggers(sector_t *sec, AActor *who);

// One WORLD STEP of Realms' own level logic: the delay countdowns and the
// deferred command queue the original drives from tick_world_effects
// (raw_commands.c:3288). Inert when no Realms level is loaded.
void TickLevelLogic(FLevelLocals *level);

// Wipe the persistent progress flags. A new game, not a level change.
void ResetProgressFlags();

} // namespace roth
