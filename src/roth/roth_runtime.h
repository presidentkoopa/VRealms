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
#include <vector>      // SidesWithOpcode
#include <string>      // DefaultStartMap

#include "textureid.h"
#include "vectors.h"   // DVector3, for ActivateLine's hit point
#include "roth_surface.h"   // WallFace, for RegisterWallFace

struct line_t;
struct sector_t;
class AActor;
struct FLevelLocals;
struct FTraceResults;   // UseFlatAt / Pick -- by reference only, no layout needed
class  player_t;        // declared as a class in d_player.h:348
class  FSerializer;     // SerializeRealmsGlobals
struct event_t;         // FlowResponder

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

// What decides which way a door swings and about which end of its hinge edge
// (setup_door_swing_geometry, doors.c:506-546): the hinge mapping's faceID
// ("marker": 0xFFFE never mirrored, 0xFFFD always, 0xFFFF mirrored when the
// door is opened from the room across corner c3), the Realms sectors across
// the two broad faces c1 and c3, and where vertex1 and vertex2 of the hinge
// face c0 stand in the level. Recorded by the loader, per polyobject tag.
void RegisterDoorSwing(int polyTag, int marker, int sectorAcrossC1, int sectorAcrossC3,
	double v1x, double v1y, double v2x, double v2y);

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

// The X-mirrored registration of the same index (TextureSet::World, flipped),
// for a command that paints it onto a FLIP_X face. See RegisterWallFace.
void RegisterMirroredTexture(int rothIndex, FTextureID tex);

// THE WALL RULE'S INPUTS for one face's sidedef, as the loader handed them to
// roth_surface, and per piece (side_t::top / mid / bottom) the picture's shape in
// the wall's terms and whether the loader drew it from a baked image.
//
// A texture swap (0x34, 0x0c) or a shift change (0x0f) then re-runs the SAME rule
// -- WallPieceRule and WallToEngine, with the record's current slots and shifts and
// the sectors' current heights -- instead of writing scales and offsets of its own,
// which is how the loader and the runtime came to disagree before.
struct WallPieceShape { int dimU = 0, dimV = 0; bool baked = false; };
void RegisterWallFace(int rothFace, const WallFace &wf, int texelLength,
	int ownSector, int nbrSector, const WallPieceShape pieces[3]);

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

// roth_portals: a Realms sector that the loader moved to a region of its own
// (rooms over rooms, joined back by linked line portals). Anything that turns
// a REALMS coordinate known to lie in that sector into an engine position must
// add this: sector centres for warps and arrivals, object positions, a camera
// posed by the diff tools. Nothing is registered, and SectorShift returns false
// with zeros, when the cvar is off or the map has no stacked rooms.
void RegisterSectorShift(int rothSector, double dx, double dy);
bool SectorShift(int rothSector, double &dx, double &dy);
// A Realms x,y to the engine position of the same spot (the pose tools). z,
// when given, picks the storey where rooms are stacked; without it the
// sector that was not moved wins. Returns the Realms sector or -1.
int RealmsToEngineXY(double x, double y, bool haveZ, double z, double &ex, double &ey);

// The player's health 0x8a0f0, its cap 0x7fe44 and the armour factor 0x81e30
// (DBASE100 op 0x37, the interpreter's) and the pain flash 0x89f3b, for the
// actor rules (roth_actors.h). ONE value each: the game flow's health as the
// command core mirrors it, the core's flash, the interpreter's armour.
int32_t PlayerHealth();
void SetPlayerHealth(int32_t v);
int32_t PlayerMaxHealth();
int32_t PlayerArmour();
int32_t PlayerPainFlash();
void SetPlayerPainFlash(int32_t v);

// begin_object_command_chain (raw_commands.c:1717-1729), for the actor rules:
// the object becomes the command source and the chain of the first live 0x1b
// trigger keyed to its command id runs (the "hit by an attack" channel). Used
// when a creature dies and when a shot lands on a shootable object.
bool FireObjectChain(int rothSector, int rothIndex);
// Whether a Realms door sector's leaf is away from its closed place (swinging
// or open) -- what the original's door pool holds (find_record_by_id,
// renderer.c:1394), which decides whether its renderer looks through it.
bool RealmsDoorOpen(int rothSector);
// A 0x3c record's +0x0e sound, played at the object by the chain's tail.
void StageObjectChainSound(uint16_t soundPlusOne, int x, int y);

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
// `hitpos`, where the caller has one, is the point on the line that was
// touched. It decides WHICH BAND OF THE WALL the hit belongs to -- mid, lower
// or upper -- which a face record's +0x06 selects between. Pass nullptr when
// there is no hit point; the band gate is then skipped rather than guessed.
bool ActivateLine(line_t *line, AActor *who, int side, int activationType,
	DVector3 *hitpos = nullptr);

// Every sidedef carrying a trigger of one opcode, for aiming a test at one.
// The load report prints the same list, but a capture script cannot read it:
// `+exec` runs before the deferred `map`, so a cfg would have to carry sidedef
// numbers copied by hand out of a previous run's log.
std::vector<int> SidesWithOpcode(uint8_t opcode);

// roth_at (a test tool, see roth_runtime.cpp): true while any timed console
// command is still waiting for its tic. rothdiff checks it so a capture batch
// that empties early does not quit before the test's later commands have run.
bool TimedCommandsPending();

// Report where each door leaf ended up after PO_Init, and whether it is sitting
// in a subsector the renderer skips. Call it right after PO_Init; a console
// command cannot do this job, because `+exec` runs before the deferred `map`.
void ReportPolyobjects(FLevelLocals *level);

// Register one line of a door leaf, by sidedef, against the polyobject tag of
// the panel it belongs to.
//
// A leaf is not a Realms FACE -- the loader generates its four lines, so there
// is no face id and no entry in the face-to-sidedef pairing -- but it IS what
// the player clicks to open the door (GAME_core.md §4.5: a type-6 door goes
// straight to toggle_door_open_state, with no trigger and no command chain).
// Without this the leaf is bare one-sided geometry that silently eats the use
// ray, and nothing behind it can be reached either.
//
// roomFace is the Realms face on the ROOM's side of a broad leaf side, or -1
// for a thickness edge. While a door is closed the original has no door
// object at all: what the player sees and clicks is that room face, so a use
// on the leaf is passed on to it and its own trigger opens the door.
void RegisterDoorLeafSide(int sideIndex, int polyTag, int roomFace = -1);

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

// WHAT FIRES A REALMS TRIGGER, as opposed to what it is bound to.
//
// The two are independent, and three opcodes keyed by a SECTOR are fired by
// three different things (GAME_core.md §5.2 rows 4, 6, 12). Collapsing them is
// what made "wire a caller for FireSectorTriggers" look like a sensible next
// step; it would have fired two floor-click triggers on walking through a
// doorway. Full derivation at EventForOpcode in roth_runtime.cpp.
enum class TrigEvent : uint8_t
{
	None = 0,
	Use,          // left-click: 0x18 on a wall face, 0x19 on a floor
	Examine,      // right-click: 0x32 on a wall face, 0x31 on a floor/ceiling
	Impact,       // an attack or projectile hits a face: 0x1a
	SectorEnter,  // crossing into or out of a sector: 0x13
};

// Fire the sector-keyed triggers on one sector that are waiting for ONE event.
//
// 0x13 wants SectorEnter, 0x19 wants Use (a click on the floor), 0x31 wants
// Examine (a right-click on floor, ceiling or platform). Passing the event is
// mandatory precisely because they share a key space and nothing else.
//
// WHICH CALLERS EXIST: none yet for any of the three. Use and Examine need a
// ray that lands on a FLAT, which P_UseLines structurally cannot provide -- it
// tests lines. SectorEnter needs a sector-transition event. Until those exist
// the bindings are built and inert, and the load report says so per opcode
// rather than letting a bound-but-unreachable trigger read as a working one.
void FireSectorTriggers(sector_t *sec, AActor *who, TrigEvent want);

// Fire the floor-click triggers under the player's aim: 0x19 for a use, 0x31
// for an examine. Returns true if a chain acted, so the caller can treat the
// use as consumed.
//
// This exists because P_UseLines structurally cannot reach these: its path is
// two-dimensional (start and end are DVector2, walking lines out of the
// blockmap), so there is no flat in it to hit and no pitch to ask about. That
// is why 12 of STUDY1's triggers had no caller, and why "wire FireSectorTriggers
// to sector entry" looked like the only way to reach them -- it was the only way
// through that path.
//
// Call it BEFORE the line path, not after. P_UseTraverse returns true for "can't
// use through a wall", so an obstructed ray reports the use as consumed and a
// `!used` guard skips this in the very case it is needed. It claims the use only
// when the ray really hits a flat carrying a trigger that wants it, so it does
// not steal uses from doors.
bool UseFlat(AActor *who, TrigEvent want);

// The same thing, from a trace somebody else already ran. UseFlat above is now
// a wrapper that traces from the eye and calls this -- split out so a pick made
// from a VR hand can reuse the flat rules without a second trace down a
// different ray, which is how the eye and the hand came to disagree about what
// the player was pointing at.
bool UseFlatAt(AActor *who, const FTraceResults &res, TrigEvent want);

//==========================================================================
//
// POINTING AT THINGS
//
// ONE RAY, ASKED ONCE, AND IT MAY BE A HAND'S.
//
// Everything above reaches Realms geometry down a ray the engine chose for its
// own reasons: P_UseLines walks a two-dimensional path out of the blockmap, and
// UseFlat traces from the eye. Neither can answer "what is this controller
// pointing at", because a hand ray has a pitch and those do not.
//
// WHY A PLAIN STOP-AT-FIRST TRACE IS NOT ENOUGH, measured: the loader gives a
// Realms face with a sister ML_TWOSIDED and no ML_BLOCKING (rothmap.cpp), and
// Trace() never reports a two-sided line the ray passes THROUGH. So a wall
// trigger on the mid band of any interior wall is invisible to a stop-at-first
// trace at every mask value -- no mask fixes it. P_UseTraverse found them only
// because it offers every line it crosses. This uses Trace's ETraceStatus
// callback with TRACE_Continue to get the same offer, which p_trace.cpp already
// does for SPAC_Impact.
//
// The band comes from FTraceResults::Tier, which the tracer has worked out
// properly, rather than from re-deriving it off a hit height.
//
//==========================================================================

struct Pick
{
	enum Kind : uint8_t
	{
		Nothing,
		WallFace,   // a Realms face -- line + side, with a band
		Flat,       // a floor, a ceiling, or a mid-platform's top or underside
		DoorLeaf,   // a hinged door slab; swings rather than dispatching
		Prop,       // a Realms object -- the 0x08 / 0x30 channel
	};

	Kind      kind   = Nothing;
	line_t   *line   = nullptr;   // WallFace, DoorLeaf
	int       side   = 0;         // which side of it was met
	sector_t *sector = nullptr;   // Flat
	AActor   *actor  = nullptr;   // Prop
	int       leafTag = 0;        // DoorLeaf: the polyobject tag to swing
	DVector3  hitPos = DVector3(0, 0, 0);
	uint8_t   band   = 0;         // mid 0x01, lower 0x02, upper 0x04; 0 = unknown
	double    dist   = 0;
	// Flat only. The original's pick record types (GAME_core.md 4.4): a sector
	// floor is 2, a ceiling 1, a mid-platform top 8 and its underside 7. The
	// trace tells them apart by which plane it met and by its 3D floor.
	bool      ceiling = false;
	const void *ffloor = nullptr; // the trace's F3DFloor*, non-null on a mid-platform
};

// Trace one ray and say what Realms thing is at the end of it. Nearest wins,
// and a hit with no Realms meaning is passed over rather than stopping the
// search. False when the ray meets nothing this port can act on.
bool PickFrom(AActor *who, const DVector3 &start, const DVector3 &dir,
	double range, Pick &out);

// The box a Realms object's actor is picked and reached by: its sprite's
// rectangle (width and height times its scale, hung from its top offset), or
// for a mesh its radius and height. The original picks opaque texels; this is
// coarser and says so (PickFrom).
void ObjectPickBox(AActor *mo, DVector3 &lo, DVector3 &hi);

// The running level's object record (sector, index) as the logic holds it now -- its art word, its id --
// for presentation that must read the original's state (roth_candles.cpp). Null outside a level or range.
struct Object;
const Object *RuntimeObject(int rothSector, int rothIndex);

// A HAND LAID ON A WALL: the nearest Realms wall face or door leaf within
// `radius` of the point `p` (a palm), measured in the map's plane, with `p`
// inside that wall's height. Independent of which way the hand faces, which a
// ray from the palm is not: a palm pressed on a door is level with it.
bool PickTouchSurface(AActor *who, const DVector3 &p, double radius, Pick &out);

// Act on a pick: faces go to the face dispatch WITH the hit position (which is
// what finally lets the wall-band gate run), flats to UseFlatAt, leaves to
// SwingDoor. Returns true if something acted, so the caller can treat the
// press as consumed.
bool Activate(const Pick &p, AActor *who, TrigEvent want);

// Act on a Realms object: find the record whose key matches the object's own
// id, then do what the original does with it -- run its chain, or, for a light
// switch, throw the switch, play its sound and flip the object's state bit.
//
// This is the channel that did not exist. A light switch authored as an OBJECT
// could not work at all, while one authored as a wall face could, and nothing
// about that asymmetry was visible from outside the engine.
bool ActivateObject(AActor *mo, TrigEvent want);

// Pick from a player's main hand, then from the off hand, and act on the first
// that finds something. The hand pose is AttackPos/AttackPitch/AttackAngle and
// its offhand pair, which is where the VR backend writes the controller every
// frame. Inert unless a Realms level is loaded and roth_pick is set.
bool UseByHand(player_t *player, TrigEvent want);

//==========================================================================
//
// THE ORIGINAL'S CURSOR, ON A PICK (roth_vrpick.cpp is its VR caller)
//
// classify_cursor_target_object (E/input.c:1111-1245) decides, every frame
// with no button down, what the thing under the cursor offers: the cursor
// TYPE (1 nothing, 3 usable, 5 blocked or keyed) and the cursor SHAPE, banded
// by the pick's depth against 600 (use), 2000 (too far) and 5000 (the
// examine probe). run_gameplay_frame (E/game_core.c:515-732) then lets a
// click act only on what that classification allows:
//   examine  (primary)   type 3 or 5, and something to examine (count != 0)
//   use      (secondary) type 3; activate_targeted_object gates depth < 600
//
// THE DEPTH is the original's renderer metric (+0x20 of the pick record).
// MEASURED on the running original (docs/REMAROTH_MEASURED.md 13): depth =
// 2z + 64, z the HORIZONTAL distance from the player's x,y to the picked point
// along the view yaw -- for every kind of target, at every pitch, and planar
// off the view centre too. So the bands are z < 268 (use), < 968 (too far),
// < 2468 (the examine probe), and they are applied here as the original
// applies them: the depth 2z + 64 against 600 / 2000 / 5000.
//
// FOR A HAND, z is measured from the player's x,y along the HAND RAY's yaw.
// The original's yaw is the camera's because its pointer is the camera's
// cursor: z is depth along the axis the pointer looks down. A hand's pointer
// looks down its own ray, and measuring along the head's yaw instead would let
// a hand pointed sideways reach without limit (z = 0 at 90 degrees). From the
// player's x,y, not the hand's, because the original's reach is the body's:
// the player's position is where its camera stands.
//
//==========================================================================

struct CursorClass
{
	uint8_t  recType = 0;     // the pick record's type: 1 ceiling, 2 floor, 3 face, 4 object, 6 door, 7/8 platform under/top; 0 none
	uint8_t  type = 1;        // 0x7e932: 1 nothing, 3 usable, 5 blocked / keyed; 0 a path that writes none (no click acts)
	uint16_t shape = 0x240;   // set_cursor_shape's id
	uint8_t  flags = 0;       // 0x7fdac: bit 1 an examine trigger matched, bit 2 blocked / keyed
	int      examinable = 0;  // 0x7fdb0: examine triggers + an OnInspect block
	bool     outOfRange = false; // 0x7fdb4
	double   depth = 0;       // +0x20, as 2z + 64 (see above)

	// What a click may do, by run_gameplay_frame's rules (roth_cursor.h):
	// activate_targeted_object's own gate is depth < 0x258 (E/input.c:1031).
	bool CanExamine() const { return (type == 3 || type == 5) && examinable != 0; }
	bool CanUse() const { return type == 3 && depth < 0x258; }
};

// z for a pick (see above): from the player's x,y along the ray's yaw (the
// player's own yaw when the ray is nearly vertical).
double PickPlanarZ(AActor *who, const DVector3 &hit, const DVector3 &rayDir);
// The pick record's depth for that z: 2z + 64 (REMAROTH_MEASURED.md 13).
inline double PickDepthOfZ(double z) { return 2.0 * z + 64.0; }
// How long a ray in `rayDir` must be traced to reach the examine band's planar
// edge (z 2468) from wherever on the player it starts.
double PickTraceRange(const DVector3 &rayDir);

// classify_cursor_target_object on a pick (its decision is roth_cursor.h, engine-free and
// tested against the original's own text by tools/vrpick_test). `depth` is 2z + 64 (above).
CursorClass ClassifyPick(const Pick &p, AActor *who, double depth);

// examine_object_under_cursor (E/input.c:979-1015): the right-click. The
// examine trigger the pick matches (dispatch_entry_command_trigger_b) has its
// DBASE100 record evaluated and its chain run (examine_world_object,
// fire_trigger_on_interact); with none, an object's OnInspect block. True when
// something was examined.
bool ExaminePick(const Pick &p, AActor *who);

// Whether two picks name the same thing (a press that began on one target and
// ended on another acts on neither). Compares identities, never dereferences.
bool SamePickTarget(const Pick &a, const Pick &b);

// A short name for a log line.
const char *PickKindName(const Pick &p);

// TEST INSTRUMENT (roth_vrpicktest seek): the nearest thing of a kind the
// player can act on -- "item" a pickable object, "examine" an object the
// original's cursor lets a click examine, "door" a door leaf's broad side,
// "face" a wall face carrying a use trigger -- that the test hand can
// reach from a spot `metresOff` in front of it, and that spot (on its floor).
// The hand is placed relative to the eye (ahead, right, up, in units). With
// clearRadius > 0 no other object may be within it of the hand (a grip test).
// "needs": an object or a wall face whose USE chain tests item `needsItem`
// with RAW 0x27 (the item-on-the-world test).
// grabRadius > 0: a GRIP test on an object (the picker's palm reach, kGrabRadius): the hand is put in
// line with the target at metresOff and raised or lowered to it; liftMetres is set to the lift the test
// grip needs for that (VRHandTest_SetLift). upm: map units per metre.
struct PickTestHand
{
	double ahead = 0, right = 0, up = 0, metresOff = 0, clearRadius = 0, grabRadius = 0, upm = 34.0;
	int needsItem = 0;
	double liftMetres = 0;   // out
};
bool PickTestTarget(const char *kind, AActor *who, PickTestHand &hand, DVector3 &aim, DVector3 &stand);

// Notice a player crossing into a new sector, and fire that sector's 0x13
// triggers. Call once per tic per player; it compares against the sector seen
// last time and does nothing when it has not changed.
//
// This is the dispatch for the largest unreachable trigger category in the
// game: 23 of STUDY1's 66 bound triggers are 0x13 and none of them could fire
// before. A per-tic comparison is the original's own mechanism -- twe_link_state
// polls the player's position from the per-frame world tick -- so this is not a
// stand-in for a callback that exists somewhere.
//
// Fires on ENTER only, and nothing on the first sighting after a load. Both
// limits are explained at the definition; both are about data not yet read, not
// about the intended design.
void NotifyPlayerSector(int playerNum, AActor *mo);

// One WORLD STEP of Realms' own level logic: the delay countdowns and the
// deferred command queue the original drives from tick_world_effects
// (raw_commands.c:3288). Inert when no Realms level is loaded.
void TickLevelLogic(FLevelLocals *level);

// Wipe the persistent progress flags. A new game, not a level change.
void ResetProgressFlags();

// ---- game flow (roth_gameflow.h holds the rules) -------------------------

// The map a new game starts in: the first entry of ROTH.RES's `maps {` list
// (game_core.c:1062-1068), STUDY1 in the retail game. False when this is not
// the Realms game or its install is not open.
bool DefaultStartMap(std::string &out);

// The engine has just rebuilt a level from a hub snapshot or a savegame.
// Called at the end of FLevelLocals::Serialize when reading.
void LevelRestored(FLevelLocals *level);

// The Realms state a savegame carries beside the level: flags, inventory,
// health, music, the movie gallery, and every visited map's logic. Called
// beside P_WriteACSVars / P_ReadACSVars.
void SerializeRealmsGlobals(FSerializer &arc);

// The menus and the death wait take Esc while one is up (sysCallbacks.WantEscape),
// and their keys -- Esc, Space in the death wait, arrows, Enter, the pad's A/B
// and d-pad -- plus F9/F10 (quicksave, quickload) in play (G_Responder).
bool FlowWantsEscape();
bool FlowResponder(const event_t *ev);
// A dead player asked to respawn (FLevelLocals::DoReborn). True while the
// Realms death wait owns that press: it is a click on the death box instead.
bool FlowRebornRequested(player_t *player);

} // namespace roth
