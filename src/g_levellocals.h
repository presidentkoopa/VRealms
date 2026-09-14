/*
** g_levellocals.h
**
** The static data for a level
**
**---------------------------------------------------------------------------
**
** Copyright 1998-2016 Marisa Heit
** Copyright 2005-2017 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "doomdata.h"
#include "g_level.h"
#include "r_defs.h"
#include "r_sky.h"
#include "portal.h"
#include "p_blockmap.h"
#include "p_local.h"
#include "po_man.h"
#include "p_acs.h"
#include "p_tags.h"
#include "p_spec.h"
#include "actor.h"
#include "b_bot.h"
#include "p_effect.h"
#include "d_player.h"
#include "p_destructible.h"
#include "r_data/r_sections.h"
#include "r_data/r_canvastexture.h"
#include "r_data/r_interpolate.h"
#include "doom_aabbtree.h"
#include "doom_levelmesh.h"
#include "p_visualthinker.h"
#include "particledefs.h"	// [PARTICLEDEFS] InlineParticleDefinition, ResolveParticleDefinitionHandle
#include <memory>

EXTERN_CVAR(Bool, sv_autocompat)

struct FGlobalDLightLists
{
	//TODO add TSet and switch from TMap to TSet
	TArray<TMap<FDynamicLight*, std::unique_ptr<FLightNode>>> flat_dlist;
	TArray<TMap<FDynamicLight*, std::unique_ptr<FLightNode>>> wall_dlist;
};

//============================================================================
//
// This is used to mark processed portals for some collection functions.
//
//============================================================================

struct FPortalBits
{
	TArray<uint32_t> data;

	void setSize(int num)
	{
		data.Resize((num + 31) / 32);
		clear();
	}

	void clear()
	{
		memset(&data[0], 0, data.Size() * sizeof(uint32_t));
	}

	void setBit(int group)
	{
		data[group >> 5] |= (1 << (group & 31));
	}

	int getBit(int group)
	{
		return data[group >> 5] & (1 << (group & 31));
	}
};

class DACSThinker;
class DFraggleThinker;
class DSpotState;
class DSeqNode;
struct FStrifeDialogueNode;
class DAutomapBase;
struct wbstartstruct_t;
class DSectorMarker;
struct FTranslator;
struct EventManager;

typedef TMap<int, int> FDialogueIDMap;				// maps dialogue IDs to dialogue array index (for ACS)
typedef TMap<FName, int> FDialogueMap;				// maps actor class names to dialogue array index
typedef TMap<int, FUDMFKeys> FUDMFKeyMap;
class DIntermissionController;

// [BB] What a billboard draws. The shader owns the shapes; what a payload
// number MEANS in a given card or readout is a mod/ZScript decision.
enum EBillboardPayload
{
	BB_PANEL   = 0,  // rounded-rect backing; data byte0 = corner radius, byte1 = border width
	BB_TEXTURE = 1,  // arbitrary TextureID on the quad; data = TextureID.GetIndex()
	BB_DIGITS  = 2,  // one integer, printed; data = the value
	BB_GLYPH   = 3,  // one character; data = its code
	BB_RING    = 4,  // progress ring; data = progress (low byte, 0-255) | style bits above
	BB_BAR     = 5,  // progress bar; data = progress (low byte, 0-255) | style bits above
	BB_TEXT    = 6,  // arbitrary string; reads FBillboard::text, ignores data
	BB_SEGMENT = 7,  // same string, drawn as a 16-segment display -- no atlas
	BB_SEGLCD  = 8,  // as BB_SEGMENT but inverted: lit plate, digits punched out
	BB_SEAM    = 9,  // a glowing slit; widen it with ResizeBillboard to open it
	// [BB] GITD's kill badge, transcribed rather than approximated. ONE quad:
	// the lozenge plate and its digits are drawn in a single pass, which is
	// what lets the digits punch to black out of the plate. data = the number.
	// Drive `progress` to open it. Digits only; letters are BB_SEGMENT.
	BB_WG13    = 10,
	// [BB] BB_PANEL's job, done as a distance field instead of a sampled
	// texture. Same rounded rect, but the edge is solved per pixel, so it is
	// crisp at any size and -- the actual reason it exists -- it can take
	// SetBillboardGlow, because a halo needs a field to read past the edge and
	// a sampled plate has none.
	//
	// Deliberately a SECOND payload rather than a change to BB_PANEL: the two
	// look different, one is cheaper, and a caller should get to pick. Numbered
	// above BB_TEXT so it lands inside the `payload >= BB_TEXT` gate that packs
	// the halo -- that gate is what stopped the sampled payloads carrying a
	// glow into a shader with no idea what one is.
	//
	// data byte0 = corner radius 0-255 across the half-extent, byte1 = border
	// width in the same units; 0 border draws a plain filled plate.
	BB_SDFPANEL = 11,

	// [BB] A HEXAGON, for tessellation. Same field, same halo, same border and
	// the same shape nibbles as BB_SDFPANEL -- only the distance function is
	// different.
	//
	// Its own payload rather than a shape flag on the panel, because a comb is
	// a different problem from a card: cells SHARE EDGES, and a shared edge is
	// the one place a sampled shape cannot hide. Two neighbours each half a
	// pixel soft do not meet, they seam, and forty of them read as a mesh of
	// grey lines instead of tiles.
	//
	// Pointy-top. Numbered above BB_TEXT like every other field payload, so it
	// lands inside the `payload >= BB_TEXT` gate that packs the halo.
	BB_SDFHEX  = 12,

	// [BB] AN N-POINTED STAR POLYGON -- the symbol a chart draws, not a point
	// of light. Filled, or stroked as an outline of a given width.
	//
	// The distinction is the whole point of the payload: a glyph has an
	// INSIDE, and a chart wants to put something there or hang a leader off
	// it. A light has no middle to write in.
	//
	// STROKE OR FILL IS A HIERARCHY, cheaply: one payload draws a chart's
	// named stars (hollow, large) and the hundreds of specks around them
	// (solid, small), and they match because they are literally the same
	// shape at different sizes.
	//
	// Its own payload and not a nibble on BB_SDFPANEL because the panel's
	// whole structure is edge-then-fill of a rounded rect. Cranking that
	// radius to maximum gives a superellipse, whose four surviving corners
	// are plainly visible at the size a star is drawn -- it reads as a
	// lozenge. There is no rectangle setting that is a star.
	//
	// data byte0 = stroke width 0-15, where 0 means FILLED; byte1 = number of
	// points, under 3 meaning the default 5. The same two nibbles in the same
	// places as the panel and the hex, with different meanings, because a
	// star has no corner radius to describe.
	//
	// The shape is fitted well inside its quad rather than to it, so the halo
	// has transparent margin to fall off in -- fit it to the quad and every
	// glow clips square at the edge. The caller pays in (transparent) quad
	// area, which is the correct trade for anything that glows.
	BB_SDFSTAR = 13,
};

// [BB] How a billboard decides which way it points. Facing is a MODE, not
// the definition of the primitive: a quad that always turns to the camera
// cannot be hinged to another at a fixed angle, because once both turn
// independently the angle between them stops meaning anything and a hinged
// assembly collapses into parallel planes. Hinge solving stays in ZScript;
// the engine only ever consumes the yaw/tilt it is handed.
enum EBillboardFacing
{
	BBF_FIXED      = 0,  // use my own yaw/tilt verbatim
	BBF_CAMERAYAW  = 1,  // turn to the viewer, stay upright (tilt preserved)
	BBF_CAMERA     = 2,  // turn to the viewer including tilt
};

// [BB] Billboard flag bits.
enum EBillboardFlags
{
	BBFL_PERSISTENT = 1,  // lives until RemoveBillboard(); ignores lifetime
	BBFL_ATTACHED   = 2,  // follows attachedTo; dies when that actor does
	BBFL_NODEPTH    = 4,  // skip depth test; draws over world geometry
	BBFL_VIEWLOCKED = 8,  // pos is an offset from the viewer, not a world point
	BBFL_FOLLOWANGLE = 16, // attached only: yaw is relative to the actor's facing, so faces turn with it

	// [BB] Decoration. Drawn like anything else, but never returned by
	// AimBillboard/TouchBillboard/SweepBillboard.
	//
	// A composed panel is not one quad -- it is forty small ones, a bar's
	// track and fill and every glyph of every label, each a billboard with
	// its own handle. The queries return the NEAREST hit, so without this
	// flag the panel's own face is permanently masked by the text drawn on
	// it: a pointer aimed at a row comes back holding the handle of a
	// letter, and no caller can map that to a row. Flag the decoration and
	// the one quad that means something is the one that answers.
	BBFL_NOHIT      = 32,

	// [BB] BB_SEAM only: the opening is a HOLE, not a lit panel. Dark
	// interior, bright rim. Without it a seam is a glowing slab and anything
	// stepping out of it reads as standing in front of a light rather than
	// emerging from somewhere.
	BBFL_VOID       = 64,
};

// [BB] A world-anchored quad: real depth-tested geometry, not a HUD overlay
// and not a surface-shader term. Extent is per-axis (width/height) rather
// than one radius because these back rectangular panels, and orientation is
// stored in design space -- mYaw is which way the face points, mTilt is how
// far the top leans, 0 being vertical. Converting that to whatever the
// renderer wants is the draw path's job, not the caller's.
//
// Lifetime is one of three: transient (expires by lifetime), persistent
// (until removed), or attached (until its actor dies). See
// FLevelLocals::TickBillboards().
struct FBillboard
{
	int      id = 0;               // handle for Update/Move/Remove; 0 = transient, no handle issued
	DVector3 pos;                  // world position; recomputed each tic while attached
	double   width = 32.0;
	double   height = 32.0;
	double   yaw = 0.0;            // design space: which way the face points
	double   tilt = 0.0;           // design space: 0 = vertical, + leans the top toward the viewer

	// [BB] Spin in the quad's own plane, degrees, clockwise seen from in front.
	// Independent of facing: a BBF_CAMERA billboard still rolls, because the
	// camera solve decides where the face POINTS and roll decides which way is
	// up on it. See BillboardBasis.
	double   roll = 0.0;
	int      facing = BBF_FIXED;   // EBillboardFacing
	int      payload = 0;          // EBillboardPayload
	int      data = 0;             // payload-specific packed int

	// [BB] Which typeface, for the distance-field payloads. 0 is the default
	// face and is what every existing call site gets by not setting this;
	// 1..N index the rolled roster. See FSDFFontRoster in hw_sdffont.h --
	// the roster is SHUFFLED PER GAME, so a slot names a ROLE ("the display
	// face") and is never a promise about which typeface answers to it.
	int      font = 0;

	// [BB] BB_TEXT's string, and only BB_TEXT's.
	//
	// It lives here rather than inside `data` because `data` is one int and
	// text is not a number. BB_DIGITS works by packing a value into that int,
	// and there is no equivalent trick for a string: 0-9 plus A-Z is 36
	// symbols, six bits each, and an int safely carries four characters before
	// it runs out. "B0002" is five and "CG B0001" is eight, so the packing
	// cannot represent the names this is for. Empty on every other payload,
	// which costs one empty FString per billboard and nothing else.
	FString  text;

	// [BB] Neon, for the payloads drawn from a distance field.
	//
	// glowRadius is how far past the letter's edge the halo reaches, as a
	// fraction of the atlas's spread: 1.0 uses the whole field, 0 is off.
	// It CANNOT usefully exceed 1 -- past the spread there is no field left to
	// read and the halo clips to a hard square at the glyph's cell boundary.
	// Measured before the shader existed, in tools/sdffont/sdfpreview.ps1.
	//
	// Ignored by every payload that is not distance-field text, because a
	// glow needs an edge to fall away from and a plain quad has no such thing.
	// [BB] How far through its reveal a payload is, 0..1. THE ANIMATED HALF OF
	// GITD's wgType 13 lived here and was the point of it: its plate is a thin
	// slit that opens vertically into a full ellipse, and the number only
	// appears once it is more than half open. Drawing the end state and
	// skipping the reveal throws away the effect.
	//
	// 1.0 by default, so anything that never sets it draws fully formed.
	double   progress = 1.0;

	double   glowRadius = 0.0;
	double   glowStrength = 0.0;   // 0 = off, 1 = halo as bright as the core

	PalEntry color;

	// [BB] Second colour, for payloads that draw a gradient. Alpha 0 means
	// "no gradient" and the payload uses `color` flat -- which is why this
	// defaults to 0 rather than to white: a white second colour would wash
	// every existing billboard the moment the field appeared.
	PalEntry color2 = 0;

	double   alpha = 1.0;          // 0 = invisible, 1 = opaque; the fade handle
	int      flags = 0;            // EBillboardFlags
	double   lifetime = 0.0;       // seconds; <= 0 = permanent. Moot once persistent/attached.
	int      spawntic = 0;         // level.maptime at creation, for transient expiry

	// [BB] Which group's transform this rides, 0 for none. See
	// FBillboardGroup -- the short version is that a composed panel is forty
	// quads and scaling it is one number, not eighty setter calls.
	int      group = 0;

	// A raw AActor* would dangle across a GC sweep. TObjPtr does not, and
	// does not itself keep the actor alive -- "attached billboards die with
	// their actor" means exactly that: once this resolves null the billboard
	// is dropped, never the other way round.
	TObjPtr<AActor*> attachedTo;
	DVector3 attachOffset;

	// Where this actually ended up last time it was drawn. View-locked
	// billboards have no fixed world position -- theirs is resolved per
	// frame against the interpolated viewpoint -- so aiming and touching
	// have to test against what was drawn rather than against pos, or the
	// pointer would disagree with what the player sees. Written by the
	// renderer, read by the aim/touch queries; not serialized, since the
	// first frame after a load rewrites it.
	DVector3 drawPos;
};

// [BB] A SHARED TRANSFORM FOR A COMPOSED PANEL.
//
// A panel is not a quad, it is forty of them: a shell, a face, every rule,
// every glyph of every label. Scaling that as one object means scaling each
// member's SIZE and its OFFSET FROM THE PANEL'S CENTRE together -- shrink the
// quads without shrinking the gaps and you get forty tiny elements in the
// original layout, which is not a smaller panel, it is a broken one.
//
// Script could do that: ResizeBillboard and MoveBillboard both exist. It
// would be eighty calls per step, each an O(n) scan of the billboard array,
// and -- the part that actually matters -- it would step at 35Hz, because
// that is when script runs. A UI element scaling in twelve visible jumps in
// front of someone's face is worse than not animating it at all.
//
// So the transform lives here and resolves in the renderer, at frame rate,
// from a start tic and a duration. Script says "grow from 0 to 1 over ten
// tics" ONCE and never touches it again. Same argument BBFL_VIEWLOCKED makes
// for position: anything welded to the eye that updates at tic rate reads as
// lag, and in a headset lag reads as nausea.
//
// The origin is in the MEMBERS' OWN SPACE, whatever that is for them -- an
// offset from the viewer for BBFL_VIEWLOCKED, an offset from the actor for
// BBFL_ATTACHED, a world point otherwise. A group whose members do not all
// share a space is a caller error and draws as nonsense; there is no cheap
// way to detect it and no attempt is made.
struct FBillboardGroup
{
	int      id = 0;               // handle; 0 is never issued
	DVector3 origin;               // the point members scale about, in their own space

	// The animation, as a declaration rather than a state machine. durTics 0
	// means settled and the scale is simply `to`, which is also how a plain
	// SetBillboardGroupScale is stored.
	double   from = 1.0;
	double   to = 1.0;
	int      startTic = 0;         // level.maptime when it began
	int      durTics = 0;          // 0 = settled
};

// [BB] THE ONE TRUE BILLBOARD BASIS. Renderer, aim ray, touch test and sweep
// all resolve their orientation HERE and nowhere else.
//
// This used to be three hand-copied pairs of lines, and on 2026-08-08 all
// three were wrong in the same way at once: `right` was the viewer's LEFT, so
// every billboard texture drew mirrored and BB_DIGITS laid multi-digit numbers
// out backwards (120 read as 021). Correcting three copies is three chances to
// correct only two of them, and a pointer that lands somewhere other than
// where the panel draws is invisible until someone notices a row is clickable
// half a panel away from where it looks. So there is one copy now, and
// "all three must always agree" is structural rather than a comment.
//
//   face   F = ( cos y,  sin y, 0)
//   right  R = (-sin y,  cos y, 0)   the viewer's right
//   up     U tilts toward -F, so positive tilt leans the top toward the viewer
//   normal N = R x U, which reduces to F at zero tilt
//
// `bpos` is where the billboard actually is -- drawPos for a view-locked one,
// pos otherwise. `eye` is whatever the caller looks FROM: the viewpoint in the
// renderer, the ray origin or the touching point in a query. A pointer and a
// viewpoint are not the same thing in VR, but resolving a camera-facing
// billboard against the thing doing the asking is what a player expects.
//
// tiltBias and scale are the bb_tiltbias / bb_scale cvars, passed in rather
// than read here so this header stays free of cvar dependencies. EVERY caller
// must pass the live values: they are comfort dials that change what is DRAWN,
// so a query that ignored them would put the clickable region somewhere other
// than the picture. That was a real defect -- the queries used bb.width
// unscaled while the renderer scaled it, so raising bb_scale to make a panel
// readable grew the panel and left its new edges dead.
// yawBias is the VIEW-LOCKED half of the same idea as tiltBias, and it is not
// optional for BBFL_VIEWLOCKED. That flag resolves POSITION against the
// viewpoint; without this it left ORIENTATION in world space, so a head-locked
// panel followed you around the room while permanently facing world-east. You
// could walk around your own HUD. Off-axis it foreshortened until it was a
// third of its authored width, and from behind it drew its back -- which reads
// as every glyph mirrored, and cost an afternoon chasing bb_flipu.
//
// A BIAS rather than "face the camera", deliberately. BBF_CAMERAYAW makes each
// quad yaw about its OWN position, which bows a composed panel into a cylinder
// and breaks hinged assemblies. Adding the view yaw to the STORED yaw keeps
// every element's angle RELATIVE to every other, so a flat panel stays flat, a
// hinge stays hinged, and the whole assembly turns with the head as one rigid
// object -- which is what view-locked was always supposed to mean.
inline void BillboardBasis(const FBillboard &bb, const DVector3 &bpos, const DVector3 &eye,
	double tiltBias, double scale,
	DVector3 &right, DVector3 &up, DVector3 &normal, double &halfw, double &halfh,
	double yawBias = 0.0)
{
	const double DEG2RAD = 0.01745329251994329576923690768489;
	const double RAD2DEG = 57.29577951308232087679815481410517;

	double useYaw = bb.yaw + yawBias;
	double useTilt = bb.tilt;

	// An attached billboard can hold its yaw relative to the actor it rides,
	// so a thing that turns takes its faces with it instead of sliding around
	// still pointing whichever way it was born facing.
	if ((bb.flags & BBFL_FOLLOWANGLE) && (bb.flags & BBFL_ATTACHED) && bb.attachedTo != nullptr)
	{
		useYaw += bb.attachedTo->Angles.Yaw.Degrees();
	}

	if (bb.facing == BBF_CAMERAYAW || bb.facing == BBF_CAMERA)
	{
		double dx = eye.X - bpos.X;
		double dy = eye.Y - bpos.Y;
		useYaw = atan2(dy, dx) * RAD2DEG;

		if (bb.facing == BBF_CAMERA)
		{
			double dz = eye.Z - bpos.Z;
			useTilt = atan2(dz, sqrt(dx * dx + dy * dy)) * RAD2DEG;
		}
	}

	double yawRad = useYaw * DEG2RAD;
	double tiltRad = (useTilt + tiltBias) * DEG2RAD;
	double cy = cos(yawRad), sy = sin(yawRad);
	double ct = cos(tiltRad), st = sin(tiltRad);

	right = DVector3(-sy, cy, 0.0);
	up = DVector3(-cy * st, -sy * st, ct);
	normal = right ^ up;

	// [BB] ROLL -- the third axis, spin in the quad's own plane.
	//
	// Yaw and tilt aim a billboard; neither can turn its face. A card that
	// tumbles as it arrives, a dial, a readout that rotates to stay level --
	// all of them wanted an angle that did not exist.
	//
	// Applied here rather than in the vertex builder ON PURPOSE. This function
	// is the single basis shared by the renderer AND by AimBillboard,
	// TouchBillboard and SweepBillboard, so a rolled billboard is pointable
	// exactly where it is drawn. Rolling in the renderer alone would have made
	// every rolled panel silently un-hittable at its visible corners -- the
	// same class of bug as the group transform in section 22.
	//
	// A rotation about the normal leaves the normal alone, so it is untouched.
	if (bb.roll != 0.0)
	{
		const double rollRad = bb.roll * DEG2RAD;
		const double cr = cos(rollRad), sr = sin(rollRad);
		const DVector3 r0 = right, u0 = up;
		right = r0 * cr + u0 * sr;
		up    = u0 * cr - r0 * sr;
	}

	double g = scale > 0.01 ? scale : 0.01;
	halfw = bb.width * 0.5 * g;
	halfh = bb.height * 0.5 * g;
}

struct FLevelLocals
{
	void *level;
	void *Level;	// bug catchers.
	FLevelLocals();
	~FLevelLocals();

	void *operator new(size_t blocksize)
	{
		// Null the allocated memory before running the constructor.
		// If we later allocate secondary levels they need to behave exactly like a global variable, i.e. start nulled.
		auto block = ::operator new(blocksize);
		memset(block, 0, blocksize);
		return block;
	}


	friend class MapLoader;

	DIntermissionController* CreateIntermission();
	void Tick();
	void Mark();
	void AddScroller(int secnum);
	void SetInterMusic(const char *nextmap);
	void SetMusicVolume(float v);
	void ClearLevelData(bool fullgc = true);
	void ClearPortals();
	bool CheckIfExitIsGood(AActor *self, level_info_t *newmap);
	void FormatMapName(FString &mapname, const char *mapnamecolor);
	void ClearAllSubsectorLinks();
	void TranslateLineDef (line_t *ld, maplinedef_t *mld, int lineindexforid = -1);
	int TranslateSectorSpecial(int special);
	bool IsTIDUsed(int tid, bool clientside);
	int FindUniqueTID(int start_tid, int limit, bool clientside);
	int GetConversation(int conv_id);
	int GetConversation(FName classname);
	void SetConversation(int convid, PClassActor *Class, int dlgindex);
	int FindNode (const FStrifeDialogueNode *node);
	int GetInfighting();
	void SetCompatLineOnSide(bool state);
	ELevelCompatFlags GetCompatibility(ELevelCompatFlags mask);
	ELevelCompatFlags2 GetCompatibility2(ELevelCompatFlags2 mask);
	void ApplyCompatibility();
	void ApplyCompatibility2();
	AActor* SelectActorFromTID(int tid, size_t index, bool clientSide, AActor* defactor);

	void Init();

private:
	bool ShouldDoIntermission(cluster_info_t* nextcluster, cluster_info_t* thiscluster);
	line_t *FindPortalDestination(line_t *src, int tag, int matchtype = -1);
	void BuildPortalBlockmap();
	void UpdatePortal(FLinePortal *port);
	void CollectLinkedPortals();
	void CreateLinkedPortals();
	bool ChangePortalLine(line_t *line, int destid);
	void AddDisplacementForPortal(FSectorPortal *portal);
	void AddDisplacementForPortal(FLinePortal *portal);
	bool ConnectPortalGroups();

	void SerializePlayers(FSerializer &arc, bool skipload);
	void CopyPlayer(player_t *dst, player_t *src, const char *name);
	void ReadOnePlayer(FSerializer &arc, bool fromHub);
	void ReadMultiplePlayers(FSerializer &arc, int numPlayers, bool fromHub);
	void SerializeSounds(FSerializer &arc);
	void PlayerSpawnPickClass (int playernum);

public:
	void SnapshotLevel();
	void UnSnapshotLevel(bool hubLoad);

	void FinalizePortals();
	bool ChangePortal(line_t *ln, int thisid, int destid);
	unsigned GetSkyboxPortal(AActor *actor);
	unsigned GetPortal(int type, int plane, sector_t *orgsec, sector_t *destsec, const DVector2 &displacement);
	unsigned GetStackPortal(AActor *point, int plane);
	DVector2 GetPortalOffsetPosition(double x, double y, double dx, double dy);
	bool CollectConnectedGroups(int startgroup, const DVector3 &position, double upperz, double checkradius, FPortalGroupArray &out);

	void ActivateInStasisPlat(int tag);
	bool CreateCeiling(sector_t *sec, DCeiling::ECeiling type, line_t *line, int tag, double speed, double speed2, double height, int crush, int silent, int change, DCeiling::ECrushMode hexencrush);
	bool ActivateInStasisCeiling(int tag);
	bool CreateFloor(sector_t *sec, DFloor::EFloor floortype, line_t *line, double speed, double height, int crush, int change, bool hexencrush, bool hereticlower);
	void DoDeferedScripts();
	void AdjustPusher(int tag, int magnitude, int angle, bool wind);
	int Massacre(bool baddies = false, FName cls = NAME_None);
	AActor *SpawnMapThing(FMapThing *mthing, int position);
	AActor *SpawnMapThing(int index, FMapThing *mt, int position);
	AActor *SpawnPlayer(FPlayerStart *mthing, int playernum, int flags = 0);
	void StartLightning();
	void ForceLightning(int mode, FSoundID tempSound = NO_SOUND);
	void ClearDynamic3DFloorData();
	void WorldDone(void);
	void AirControlChanged();
	AActor *SelectTeleDest(int tid, int tag, bool norandom, bool isPlayer);
	bool AlignFlat(int linenum, int side, int fc);
	void ReplaceTextures(const char *fromname, const char *toname, int flags);

	bool EV_Thing_Spawn(int tid, AActor *source, int type, DAngle angle, bool fog, int newtid);
	bool EV_Thing_Move(int tid, AActor *source, int mapspot, bool fog);
	bool EV_Thing_Projectile(int tid, AActor *source, int type, const char *type_name, DAngle angle,
		double speed, double vspeed, int dest, AActor *forcedest, int gravity, int newtid, bool leadTarget);
	int EV_Thing_Damage(int tid, AActor *whofor0, int amount, FName type);

	bool EV_DoPlat(int tag, line_t *line, DPlat::EPlatType type, double height, double speed, int delay, int lip, int change);
	void EV_StopPlat(int tag, bool remove);
	bool EV_DoPillar(DPillar::EPillar type, line_t *line, int tag, double speed, double height, double height2, int crush, bool hexencrush);
	bool EV_DoDoor(DDoor::EVlDoor type, line_t *line, AActor *thing, int tag, double speed, int delay, int lock, int lightTag, bool boomgen = false, int topcountdown = 0);
	bool EV_SlidingDoor(line_t *line, AActor *thing, int tag, int speed, int delay, DAnimatedDoor::EADType type);
	bool EV_DoCeiling(DCeiling::ECeiling type, line_t *line, int tag, double speed, double speed2, double height, int crush, int silent, int change, DCeiling::ECrushMode hexencrush = DCeiling::ECrushMode::crushDoom);
	bool EV_CeilingCrushStop(int tag, bool remove);
	bool EV_StopCeiling(int tag, line_t *line);
	bool EV_BuildStairs(int tag, DFloor::EStair type, line_t *line, double stairsize, double speed, int delay, int reset, int igntxt, int usespecials);
	bool EV_DoFloor(DFloor::EFloor floortype, line_t *line, int tag, double speed, double height, int crush, int change, bool hexencrush, bool hereticlower = false);
	bool EV_FloorCrushStop(int tag, line_t *line);
	bool EV_StopFloor(int tag, line_t *line);
	bool EV_DoDonut(int tag, line_t *line, double pillarspeed, double slimespeed);
	bool EV_DoElevator(line_t *line, DElevator::EElevator type, double speed, double height, int tag);
	bool EV_StartWaggle(int tag, line_t *line, int height, int speed, int offset, int timer, bool ceiling);
	bool EV_DoChange(line_t *line, EChange changetype, int tag);

	void EV_StartLightFlickering(int tag, int upper, int lower);
	void EV_StartLightStrobing(int tag, int upper, int lower, int utics, int ltics);
	void EV_StartLightStrobing(int tag, int utics, int ltics);
	void EV_TurnTagLightsOff(int tag);
	void EV_LightTurnOn(int tag, int bright);
	void EV_LightTurnOnPartway(int tag, double frac);
	void EV_LightChange(int tag, int value);
	void EV_StartLightGlowing(int tag, int upper, int lower, int tics);
	void EV_StartLightFading(int tag, int value, int tics);
	void EV_StopLightEffect(int tag);

	bool EV_Teleport(int tid, int tag, line_t *line, int side, AActor *thing, int flags);
	bool EV_SilentLineTeleport(line_t *line, int side, AActor *thing, int id, INTBOOL reverse);
	bool EV_TeleportOther(int other_tid, int dest_tid, bool fog);
	bool EV_TeleportGroup(int group_tid, AActor *victim, int source_tid, int dest_tid, bool moveSource, bool fog);
	bool EV_TeleportSector(int tag, int source_tid, int dest_tid, bool fog, int group_tid);

	void RecalculateDrawnSubsectors();
	FSerializer &SerializeSubsectors(FSerializer &arc, const char *key);
	void SpawnExtraPlayers();
	void Serialize(FSerializer &arc, bool hubload);
	DThinker *FirstThinker (int statnum);
	DThinker* FirstClientSideThinker(int statnum);

	// g_Game
	void PlayerReborn (int player);
	bool CheckSpot (int playernum, FPlayerStart *mthing);
	void DoReborn (int playernum, bool force = false);
	void QueueBody (AActor *body);
	double PlayersRangeFromSpot (FPlayerStart *spot);
	FPlayerStart *SelectFarthestDeathmatchSpot (size_t selections);
	FPlayerStart *SelectRandomDeathmatchSpot (int playernum, unsigned int selections);
	void DeathMatchSpawnPlayer (int playernum);
	FPlayerStart *PickPlayerStart(int playernum, int flags = 0);
	bool DoCompleted(FString nextlevel, wbstartstruct_t &wminfo);
	void StartTravel();
	void AddToTravellingList(DThinker* th);
	void MoveTravellers();
	int FinishTravel();
	void UnlinkActorFromLevel(AActor& mo);
	void LinkActorToLevel(AActor& mo);
	void ChangeLevel(const char *levelname, int position, int flags, int nextSkill = -1);
	const char *GetSecretExitMap();
	void ExitLevel(int position, bool keepFacing);
	void SecretExitLevel(int position);
	void DoLoadLevel(const FString &nextmapname, int position, bool autosave, bool newGame);

	void DeleteAllAttachedLights();
	void RecreateAllAttachedLights();


private:
	// Work data for CollectConnectedGroups.
	FPortalBits processMask;
	TArray<FLinePortal*> foundPortals;
	TArray<int> groupsToCheck;

public:

	FSectorTagIterator GetSectorTagIterator(int tag)
	{
		return FSectorTagIterator(tagManager, tag);
	}
	FSectorTagIterator GetSectorTagIterator(int tag, line_t *line)
	{
		return FSectorTagIterator(tagManager, tag, line);
	}
	FLineIdIterator GetLineIdIterator(int tag)
	{
		return FLineIdIterator(tagManager, tag);
	}
	template<class T> TThinkerIterator<T> GetThinkerIterator(FName subtype = NAME_None, int statnum = MAX_STATNUM+1)
	{
		if (subtype == NAME_None) return TThinkerIterator<T>(this, statnum, false);
		else return TThinkerIterator<T>(this, subtype, statnum, false);
	}
	template<class T> TThinkerIterator<T> GetThinkerIterator(FName subtype, int statnum, AActor *prev)
	{
		return TThinkerIterator<T>(this, subtype, statnum, prev, false);
	}
	template<class T> TThinkerIterator<T> GetClientSideThinkerIterator(FName subtype = NAME_None, int statnum = MAX_STATNUM + 1)
	{
		if (subtype == NAME_None) return TThinkerIterator<T>(this, statnum, true);
		else return TThinkerIterator<T>(this, subtype, statnum, true);
	}
	template<class T> TThinkerIterator<T> GetClientSideThinkerIterator(FName subtype, int statnum, AActor* prev)
	{
		return TThinkerIterator<T>(this, subtype, statnum, prev, true);
	}
	FActorIterator GetActorIterator(int tid)
	{
		return FActorIterator(TIDHash, tid);
	}
	FActorIterator GetActorIterator(int tid, AActor *start)
	{
		return FActorIterator(TIDHash, tid, start);
	}
	NActorIterator GetActorIterator(FName type, int tid)
	{
		return NActorIterator(TIDHash, type, tid);
	}
	FActorIterator GetClientSideActorIterator(int tid)
	{
		return FActorIterator(ClientSideTIDHash, tid);
	}
	FActorIterator GetClientSideActorIterator(int tid, AActor* start)
	{
		return FActorIterator(ClientSideTIDHash, tid, start);
	}
	NActorIterator GetClientSideActorIterator(FName type, int tid)
	{
		return NActorIterator(ClientSideTIDHash, type, tid);
	}
	AActor *SingleActorFromTID(int tid, bool clientSide, AActor *defactor)
	{
		return tid == 0 ? defactor : (clientSide ? GetClientSideActorIterator(tid).Next() : GetActorIterator(tid).Next());
	}

	bool SectorHasTags(sector_t *sector)
	{
		return tagManager.SectorHasTags(sector);
	}
	bool SectorHasTag(sector_t *sector, int tag)
	{
		return tagManager.SectorHasTag(sector, tag);
	}
	bool SectorHasTag(int sector, int tag)
	{
		return tagManager.SectorHasTag(sector, tag);
	}
	int GetFirstSectorTag(const sector_t *sect) const
	{
		return tagManager.GetFirstSectorTag(sect);
	}
	int GetFirstSectorTag(int i) const
	{
		return tagManager.GetFirstSectorTag(i);
	}
	int GetFirstLineId(const line_t *sect) const
	{
		return tagManager.GetFirstLineID(sect);
	}

	bool LineHasId(int line, int tag)
	{
		return tagManager.LineHasID(line, tag);
	}
	bool LineHasId(line_t *line, int tag)
	{
		return tagManager.LineHasID(line, tag);
	}

	int FindFirstSectorFromTag(int tag)
	{
		auto it = GetSectorTagIterator(tag);
		return it.Next();
	}

	int FindFirstLineFromID(int tag)
	{
		auto it = GetLineIdIterator(tag);
		return it.Next();
	}

	int isFrozen()
	{
		return frozenstate;
	}

private:	// The engine should never ever access subsectors of the game nodes. This is only needed for actually implementing PointInSector.
	subsector_t *PointInSubsector(double x, double y);
public:
	sector_t *PointInSectorBuggy(double x, double y);
	subsector_t *PointInRenderSubsector (fixed_t x, fixed_t y);

	sector_t *PointInSector(const DVector2 &pos)
	{
		return PointInSubsector(pos.X, pos.Y)->sector;
	}

	sector_t* PointInSector(const DVector3& pos)
	{
		return PointInSubsector(pos.X, pos.Y)->sector;
	}

	sector_t *PointInSector(double x, double y)
	{
		return PointInSubsector(x, y)->sector;
	}

	subsector_t *PointInRenderSubsector (const DVector2 &pos)
	{
		return PointInRenderSubsector(FloatToFixed(pos.X), FloatToFixed(pos.Y));
	}

	subsector_t* PointInRenderSubsector(const DVector3& pos)
	{
		return PointInRenderSubsector(FloatToFixed(pos.X), FloatToFixed(pos.Y));
	}

	FPolyObj *GetPolyobj (int polyNum)
	{
		auto index = Polyobjects.FindEx([=](const auto &poly) { return poly.tag == polyNum; });
		return index == Polyobjects.Size()? nullptr : &Polyobjects[index];
	}


	void ClearTIDHashes ()
	{
		memset(TIDHash, 0, sizeof(TIDHash));
		memset(ClientSideTIDHash, 0, sizeof(ClientSideTIDHash));
	}


	bool CheckReject(sector_t *s1, sector_t *s2)
	{
		if (rejectmatrix.Size() > 0)
		{
			int pnum = int(s1->Index()) * sectors.Size() + int(s2->Index());
			return !(rejectmatrix[pnum >> 3] & (1 << (pnum & 7)));
		}
		return true;
	}

	DThinker *CreateThinker(PClass *cls, int statnum = STAT_DEFAULT)
	{
		DThinker *thinker = static_cast<DThinker*>(cls->CreateNew());
		assert(thinker->IsKindOf(RUNTIME_CLASS(DThinker)));
		thinker->ObjectFlags |= OF_JustSpawned;
		Thinkers.Link(thinker, statnum);
		thinker->Level = this;
		return thinker;
	}

	template<typename T, typename... Args>
	T* CreateThinker(Args&&... args)
	{
		auto thinker = static_cast<T*>(CreateThinker(RUNTIME_CLASS(T), T::DEFAULT_STAT));
		thinker->Construct(std::forward<Args>(args)...);
		return thinker;
	}

	DThinker* CreateClientSideThinker(PClass* cls, int statnum = STAT_DEFAULT)
	{
		DThinker* thinker = static_cast<DThinker*>(cls->CreateNew());
		assert(thinker->IsKindOf(RUNTIME_CLASS(DThinker)));
		thinker->ObjectFlags |= OF_JustSpawned | OF_ClientSide | OF_Transient | OF_NoRollback;
		ClientSideThinkers.Link(thinker, statnum);
		thinker->Level = this;
		return thinker;
	}

	template<typename T, typename... Args>
	T* CreateClientSideThinker(Args&&... args)
	{
		auto thinker = static_cast<T*>(CreateClientSideThinker(RUNTIME_CLASS(T), T::DEFAULT_STAT));
		thinker->Construct(std::forward<Args>(args)...);
		return thinker;
	}

	void SetMusic();

	TArray<vertex_t> vertexes;
	TArray<sector_t> sectors;
	TArray<extsector_t> extsectors; // container for non-trivial sector information. sector_t must be trivially copyable for *_fakeflat to work as intended.
	TArray<line_t*> linebuffer;	// contains the line lists for the sectors.
	TArray<subsector_t*> subsectorbuffer;	// contains the subsector lists for the sectors.
	TArray<line_t> lines;
	TArray<side_t> sides;
	TArray<seg_t *> segbuffer;	// contains the seg links for the sidedefs.
	TArray<seg_t> segs;
	TArray<subsector_t> subsectors;
	TArray<node_t> nodes;
	TArray<subsector_t> gamesubsectors;
	TArray<node_t> gamenodes;
	node_t *headgamenode;
	TArray<uint8_t> rejectmatrix;
	TArray<zone_t>	Zones;
	TArray<FPolyObj> Polyobjects;

	TArray<FSectorPortal> sectorPortals;
	TArray<FLinePortal> linePortals;

	// Lightmaps
	TArray<LightmapSurface> LMSurfaces;
	TArray<float> LMTexCoords;
	int LMTextureCount = 0;
	int LMTextureSize = 0;
	TArray<uint16_t> LMTextureData;
	TArray<LightProbe> LightProbes;
	int LPMinX = 0;
	int LPMinY = 0;
	int LPWidth = 0;
	int LPHeight = 0;
	static const int LPCellSize = 32;
	TArray<LightProbeCell> LPCells;

	// Portal information.
	FDisplacementTable Displacements;
	FPortalBlockmap PortalBlockmap;
	TArray<FLinePortal*> linkedPortals;	// only the linked portals, this is used to speed up looking for them in P_CollectConnectedGroups.
	TArray<FSectorPortalGroup *> portalGroups;
	TArray<FLinePortalSpan> linePortalSpans;
	FSectionContainer sections;
	FCanvasTextureInfo canvasTextureInfo;
	EventManager *localEventManager = nullptr;
	DoomLevelAABBTree* aabbTree = nullptr;
	DoomLevelMesh* levelMesh = nullptr;

	// [ZZ] Destructible geometry information
	TMap<int, FHealthGroup> healthGroups;

	FBlockmap blockmap;
	TArray<polyblock_t *> PolyBlockMap;
	FUDMFKeyMap UDMFKeys[4];

	// These are copies of the loaded map data that get used by the savegame code to skip unaltered fields
	// Without such a mechanism the savegame format would become too slow and large because more than 80-90% are normally still unaltered.
	TArray<sector_t>	loadsectors;
	TArray<line_t>	loadlines;
	TArray<side_t>	loadsides;

	// Maintain single and multi player starting spots.
	TArray<FPlayerStart> deathmatchstarts;
	FPlayerStart		playerstarts[MAXPLAYERS];
	TArray<FPlayerStart> AllPlayerStarts;

	FBehaviorContainer Behaviors;
	AActor *TIDHash[128];
	AActor* ClientSideTIDHash[128];

	TArray<FStrifeDialogueNode *> StrifeDialogues;
	FDialogueIDMap DialogueRoots;
	FDialogueMap ClassRoots;
	FCajunMaster BotInfo;

	ELevelCompatFlags ii_compatflags = 0;
	ELevelCompatFlags2 ii_compatflags2 = 0;
	ELevelBugCompatFlags ib_compatflags = 0;
	ELevelCompatFlags i_compatflags = 0;
	ELevelCompatFlags2 i_compatflags2 = 0;

	DSectorMarker *SectorMarker;

	uint8_t		md5[16];			// for savegame validation. If the MD5 does not match the savegame won't be loaded.
	int			time;			// time in the hub
	int			maptime;			// time in the map
	int			totaltime;		// time in the game
	int			starttime;
	int			partime;
	int			sucktime;
	uint32_t	spawnindex;

	level_info_t *info;
	int			cluster;
	int			clusterflags;
	int			levelnum;
	int			lumpnum;
	FString		LevelName;
	FString		MapName;			// the lump name (E1M1, MAP01, etc)
	FString		NextMap;			// go here when using the regular exit
	FString		NextSecretMap;		// map to go to when used secret exit
	FString		AuthorName;
	FString		F1Pic;
	FTranslator *Translator;
	EMapType	maptype;
	FTagManager tagManager;
	FInterpolator interpolator;

	uint64_t	ShaderStartTime = 0;	// tell the shader system when we started the level (forces a timer restart)

	static const int BODYQUESIZE = 32;
	TObjPtr<AActor*> bodyque[BODYQUESIZE];
	TObjPtr<DAutomapBase*> automap = MakeObjPtr<DAutomapBase*>(nullptr);
	int bodyqueslot;

	// For now this merely points to the global player array, but with this in place, access to this array can be moved over to the level.
	// As things progress each level needs to be able to point to different players, even if they are just null if the second level is merely a skybox or camera target.
	// But even if it got a real player, the level will not own it - the player merely links to the level.
	// This should also be made a real object eventually.
	player_t *Players[MAXPLAYERS];

	// This is to allow refactoring without refactoring the data right away.
	bool PlayerInGame(int pnum)
	{
		return playeringame[pnum];
	}

	// This needs to be done better, but for now it should be good enough.
	bool PlayerInGame(player_t *player)
	{
		for (unsigned int i = 0; i < MAXPLAYERS; i++)
		{
			if (player == Players[i]) return PlayerInGame(i);
		}
		return false;
	}

	int PlayerNum(player_t *player)
	{
		for (unsigned int i = 0; i < MAXPLAYERS; i++)
		{
			if (player == Players[i]) return i;
		}
		return -1;
	}

	bool isPrimaryLevel() const
	{
		return true;
	}

	// Gets the console player without having the calling code be aware of the level's state.
	player_t *GetConsolePlayer() const
	{
		return isPrimaryLevel()? Players[consoleplayer] : nullptr;
	}

	bool isConsolePlayer(AActor *mo) const
	{
		auto p = GetConsolePlayer();
		if (!p) return false;
		return p->mo == mo;
	}

	bool isCamera(AActor *mo) const
	{
		auto p = GetConsolePlayer();
		if (!p) return false;
		return p->camera == mo;
	}

	int NumMapSections;

	uint32_t		flags;
	uint32_t		flags2;
	uint32_t		flags3;

	uint32_t		fadeto;					// The color the palette fades to (usually black)
	uint32_t		outsidefog;				// The fog for sectors with sky ceilings

	uint32_t		hazardcolor;			// what color strife hazard blends the screen color as
	uint32_t		hazardflash;			// what color strife hazard flashes the screen color as

	FString		LightningSound = "world/thunder";
	FString		Music;
	int			musicorder;
	int			cdtrack;
	unsigned int cdid;
	FTextureID	skytexture1;
	FTextureID	skytexture2;
	FTextureID	skymisttexture;

	float		skyspeed1;				// Scrolling speed of sky textures, in pixels per ms
	float		skyspeed2;
	float		skymistspeed;
	float		skymistyscale;			// Y-scale for skymist layer. Scales from horizon as midpoint. Doesn't tile.

	double		sky1pos, sky2pos;
	float		hw_sky1pos, hw_sky2pos, hw_skymistpos, hw_skymistyscale;
	bool		skystretch;
	uint32_t	globalcolormap;

	int			total_secrets;
	int			found_secrets;

	int			total_items;
	int			found_items;

	int			total_monsters;
	int			killed_monsters;

	struct VelocityMeasurer {
		int total = 0;
		double cur_velocity = 0.0;
		double max_velocity = 0.0;
		double avg_velocity = 0.0;

		void SetVelocity(double spd)
		{
			cur_velocity = spd;
			if (spd > max_velocity)
				max_velocity = spd;
			avg_velocity += (spd - avg_velocity) / ++total;
		}

		void Clear()
		{
			total = 0;
			cur_velocity = max_velocity = avg_velocity = 0.0;
		}
	};

	VelocityMeasurer velocities[MAXPLAYERS] = {};

	void ClearVelocities()
	{
		for (auto& vel : velocities)
			vel.Clear();
	}

	double		gravity;
	double		aircontrol;
	double		airfriction;
	int			airsupply;
	int			DefaultEnvironment;		// Default sound environment.

	DSeqNode *SequenceListHead;

	// [RH] particle globals
	uint32_t			OldestParticle; // [MC] Oldest particle for replacing with SPF_REPLACE
	uint32_t			ActiveParticles;
	uint32_t			InactiveParticles;
	TArray<particle_t>	Particles;
	TArray<uint16_t>	ParticlesInSubsec;
	FThinkerCollection Thinkers;
	FThinkerCollection ClientSideThinkers;
	TArray<DThinker*> TravellingThinkers;

	TArray<DVector2>	Scrolls;		// NULL if no DScrollers in this level

	int8_t		WallVertLight;			// Light diffs for vert/horiz walls
	int8_t		WallHorizLight;

	bool		FromSnapshot;			// The current map was restored from a snapshot
	bool		HasHeightSecs;			// true if some Transfer_Heights effects are present in the map. If this is false, some checks in the renderer can be shortcut.
	bool		HasDynamicLights;		// Another render optimization for maps with no lights at all.
	int		frozenstate;

	double		teamdamage;

	TArray<FString> savedModelFiles;

	// former OpenGL-exclusive properties that should also be usable by the true color software renderer.
	int fogdensity;
	int outsidefogdensity;
	int skyfog;

	FName		deathsequence;
	float		pixelstretch;
	float		MusicVolume;

	// Hardware render stuff that can either be set via CVAR or MAPINFO
	bool		brightfog;
	bool		lightadditivesurfaces;
	bool		notexturefill;
	int			ImpactDecalCount;
	float		thickfogdistance;
	float		thickfogmultiplier;

	int                LocalWorldTimer = 0;	// For client-sided actions that are still bound to world processing.
	int                LocalTimer = 0;		// For client-sided actions independent of any world state.
	FGlobalDLightLists lightlists;

	FDynamicLight *lights;
	// [round2 B1] The dynamic lights held in a tracked pose (FDynamicLight::
	// PoseAnchor), so hw_entrypoint.cpp can re-pose them every frame without
	// walking `lights`. A light leaves it in FDynamicLight::ReleaseLight, which
	// every free passes through -- NOT UnlinkLight, which LinkLight calls on every
	// relink -- and ClearLevelData empties it. Main thread only.
	TArray<FDynamicLight*> PoseAnchoredLights;
	DVisualThinker* VisualThinkerHead = nullptr;

	// [BB] Billboards: world-anchored oriented quads backing the in-world
	// panel system. Set-and-forget, unlike anything rebuilt per tic.
	TArray<FBillboard> Billboards;
	int NextBillboardID = 1;
	void TickBillboards();

	// [BB] Shared transforms for composed panels. See FBillboardGroup.
	TArray<FBillboardGroup> BillboardGroups;
	int NextBillboardGroupID = 1;

	FBillboardGroup *FindBillboardGroupByID(int gid)
	{
		if (gid == 0) return nullptr;
		for (auto &g : BillboardGroups) if (g.id == gid) return &g;
		return nullptr;
	}

	// [BB] The group's scale RIGHT NOW, eased, at render resolution.
	//
	// ticFrac is the renderer's fraction through the current tic, so this
	// returns a different number on every drawn frame while an animation is
	// running -- which is the whole point of the group living in the engine
	// rather than in script.
	//
	// TWO CURVES, chosen by direction, because growing and collapsing are not
	// the same gesture. Growth overshoots slightly and settles: a panel that
	// arrives at exactly its final size and stops reads as a texture being
	// swapped in, whereas a few percent past and back reads as an object
	// arriving. A collapse does the opposite -- it accelerates away, because a
	// thing leaving should not linger and should certainly not bounce.
	//
	// Returns `to` and does no work at all once the animation is spent, so a
	// settled group costs one comparison per billboard per frame.
	double BillboardGroupScale(int gid, double ticFrac, DVector3 *origin = nullptr)
	{
		const FBillboardGroup *g = FindBillboardGroupByID(gid);
		if (!g) return 1.0;
		if (origin) *origin = g->origin;

		if (g->durTics <= 0) return g->to;

		double elapsed = (double(maptime) + ticFrac) - double(g->startTic);
		double t = elapsed / double(g->durTics);
		if (t <= 0.0) return g->from;
		if (t >= 1.0) return g->to;

		double e;
		if (g->to >= g->from)
		{
			// Ease-out back. c is the classic 1.70158 taken down to about a
			// third: full strength overshoots ~10% and on a panel a foot from
			// someone's eyes that is a wobble, not a flourish.
			const double c = 0.6;
			const double u = t - 1.0;
			e = 1.0 + (c + 1.0) * u * u * u + c * u * u;
		}
		else
		{
			e = t * t;		// ease-in quad: leaves faster than it arrived
		}

		double s = g->from + (g->to - g->from) * e;
		return s > 0.0 ? s : 0.0;
	}

	// [BB] VOLUMETRIC BEAMS -- lit air rather than lit surfaces.
	//
	// THIRTY-TWO SLOTS (MAX_VOL_BEAMS below), and it used to be one. A
	// singleton meant every caller was really the same caller: the weapon
	// wheel's laser, RS_Lance and anything else all wrote the same fields, so
	// whoever set it last won and whoever finished first called Clear and took
	// everyone else's light out with it. A flashlight was impossible to add for
	// exactly that reason -- open the wheel and your torch would go dark.
	//
	// (It was four for a while; see the note on the constant for why it grew.)
	// Each LIVE beam is a raymarch. They cost nothing when off-screen -- the
	// pass bounds every one with an analytic ray/cone intersection and returns
	// black in a few dot products -- but N beams lighting the same corridor is
	// N marches over the same pixels.
	//
	// Slot 0 is what a caller that never heard of slots gets, so every existing
	// call site keeps working unchanged.
	// 32, AND IT MUST MATCH PPVolumetricBeam::MAX_BEAMS EXACTLY.
	//
	// hw_drawinfo.cpp iterates to THIS constant and hands each slot to
	// PPVolumetricBeam::AddBeam, which guards with `if (count < MAX_BEAMS)`.
	// If the two disagree, every slot above the LOWER value is dropped on the
	// floor -- no error, no log line, no warning. The cone simply never renders
	// and there is nothing anywhere to say why. Change one, change the other.
	//
	// Raised from 4 because a dual-wielding VR loadout already fills it at rest:
	// flashlight, wheel laser, and a muzzle flash per hand is four, with no
	// headroom for anything added later.
	//
	// The ceiling is an ARRAY BOUND, not a per-frame cost. PPVolumetricBeam::
	// Render early-returns on count <= 0 and loops to `count` -- the number of
	// beams actually published this frame -- so unused slots cost nothing to
	// draw. Thirteen arrays of 32 on FLevelLocals is under 4KB per level.
	//
	// NOT FOR LASER SIGHTS. A laser is a LINE and belongs in the separate
	// 128-slot system (MAX_BEAMS / BeamStart / BeamEnd below). These are CONES:
	// flashlights and muzzle flashes, things that light the air in a volume.
	static const int MAX_VOL_BEAMS = 32;

	bool     VolBeamActive[MAX_VOL_BEAMS] = {};
	DVector3 VolBeamPos[MAX_VOL_BEAMS] = {};
	DVector3 VolBeamDir[MAX_VOL_BEAMS] = {};
	PalEntry VolBeamColor[MAX_VOL_BEAMS] = {};
	double   VolBeamInner[MAX_VOL_BEAMS] = {};   // degrees, full brightness inside
	double   VolBeamOuter[MAX_VOL_BEAMS] = {};   // degrees, faded out by here
	double   VolBeamLength[MAX_VOL_BEAMS] = {};
	double   VolBeamDensity[MAX_VOL_BEAMS] = {};
	double   VolBeamFalloff[MAX_VOL_BEAMS] = {};
	double   VolBeamDust[MAX_VOL_BEAMS] = {};    // 0 clean, 1 heavily mottled
	double   VolBeamDustScale[MAX_VOL_BEAMS] = {};
	double   VolBeamDustDrift[MAX_VOL_BEAMS] = {};

	// RS FORK -- WHERE A VOLUMETRIC BEAM IS HELD, per slot.
	//
	//   0  the pos/dir script gave SetVolumetricBeam, as always
	//   1  the MAIN hand: AttackPos, aimed along AttackAngle/AttackPitch
	//   2  the OFF hand: OffhandPos, OffhandAngle/OffhandPitch
	//   3  the HEAD: HmdPos/HmdYaw/HmdPitch, or the view when no headset
	//      pose has been written (HmdPos zero)
	//
	// The cone version of BeamAnchor (below) and for the same reason: script
	// publishes at 35Hz, a tracked hand moves at 90Hz+, so a hand torch posed
	// from WorldTick holds each pose for 2-3 frames and then jumps. Anchored,
	// hw_drawinfo.cpp (ResolveVolBeamPose) reads the pose the VR backend wrote
	// THIS frame. Unlike a line beam's anchor it moves the DIRECTION too -- a
	// cone has no world-fixed far end to keep.
	//
	// VolBeamAnchorOffset is (forward, right, up) in map units in the pose's
	// yaw/pitch frame. Zero by default; SetVolumetricBeam resets both when it
	// claims a slot that was not live, and ClearVolumetricBeam resets the mode.
	int      VolBeamAnchor[MAX_VOL_BEAMS] = {};
	DVector3 VolBeamAnchorOffset[MAX_VOL_BEAMS] = {};

	// The first live beam that actually emits light, or -1. The fog reads a
	// single torch cone (it has one set of mFogBeam uniforms), so it takes the
	// lowest live slot rather than silently taking whichever happened to be
	// written last. A beam at density 0 (Brightness 0, or a flicker dip) is
	// skipped: the air pass already draws nothing for it, and the fog glow
	// must not keep a full-strength torch the beam itself no longer has.
	int FirstVolBeam() const
	{
		for (int i = 0; i < MAX_VOL_BEAMS; i++)
			if (VolBeamActive[i] && VolBeamDensity[i] > 0.0) return i;
		return -1;
	}

	// [BB] Sweep: a thin band of light at a fixed distance from an origin,
	// measured in WORLD space and tested on every surface. Because the test
	// is world-space rather than per-surface, the band wraps continuously
	// across floor, wall and ceiling on its own -- a cylinder expanding from
	// a point slices all three at the same radius, and a plane travelling
	// down a corridor draws an unbroken rectangle around it.
	//
	// This is not a sector property and deliberately not one of the four
	// lanes: it is a single world-space overlay, so it costs one set of
	// uniforms per frame rather than anything per sector.
	//
	// [round2 SW-21] This header sat stranded above the volumetric beams with a
	// shape list that stopped at 4. Both now live here, beside the fields.
	//
	// Up to eight bands travel at once, so a train of them can chase each
	// other with their own colours and spacing.
	static const int MAX_SWEEP_BANDS = 8;
	// The shared shape, and each band's own SweepBandMode below:
	//   0 off, 1 ring (cylinder from origin), 2 bar along X (both sides),
	//   3 bar along Y (both sides), 4 sphere from origin, 5 rise (signed, Z),
	//   6 +X, 7 +Y, 8 -X, 9 -Y (the signed crossings). SweepShapeDist in main.fp.
	int SweepMode = 0;
	DVector3 SweepOrigin;
	int SweepCount = 0;
	double SweepRadius[MAX_SWEEP_BANDS] = {};     // where each band sits
	double SweepThickness[MAX_SWEEP_BANDS] = {};  // band width, map units
	double SweepSoftness[MAX_SWEEP_BANDS] = {};   // 1 linear, higher = tighter core
	PalEntry SweepColor[MAX_SWEEP_BANDS] = {};
	double SweepIntensity[MAX_SWEEP_BANDS] = {};
	double SweepTrail = 0;                        // wake length, signed
	// Per band, so eight sweeps need not agree about where the centre of the
	// world is. Seeded from SweepOrigin/SweepMode; overridden per band by
	// SetSweepBandAt. Shape 0 means the band is off.
	DVector3 SweepBandOrigin[MAX_SWEEP_BANDS];
	int SweepBandMode[MAX_SWEEP_BANDS] = {};
	// What each band does to the pixels it covers: 0 default (add), 1 add,
	// 2 lift, 3 crush, 4 recolour. Uploaded with the band's fill and passed bit
	// as drawmode + 16*fill + 256*passed (FRenderState::SetSweepBandDraw).
	// [round2 SW-21] This used to stop at 3.
	int SweepBandDraw[MAX_SWEEP_BANDS] = {};

	// [BB] WHAT IS *INSIDE* A BAND.
	//
	// A band knows, for every pixel it covers, both how strongly it covers it
	// AND where that pixel is in the world. It used to throw the second away
	// and blend one flat colour weighted by the first -- so a band could only
	// ever be a wash.
	//
	// But every shape that defines a distance also implies two TANGENT
	// coordinates, and a pattern is just a function of those two. A bar
	// sweeping a corridor has (height, across); a ring has (height, arc). So
	// the same code that draws a lattice in a hallway draws a cage on an
	// expanding cylinder, with no per-shape special casing beyond picking the
	// two axes.
	//
	// PER BAND: only the fill MODE, packed into the draw mode's spare bits --
	// see SetSweepBandDraw. That is what lets a train be a solid wall, then a
	// grid, then a band of travelling darkness.
	//
	// SHARED: the style. Spacing, width, softness, rotation and the rest are
	// frame-global, because putting them per band would mean another
	// vec4[8] in StreamData -- and that buffer's size divides 64KB into
	// MAX_STREAM_DATA draws, so it would cost draw batching in every frame of
	// the game to let band 3 have a different line width from band 4.
	int      SweepBandFill[MAX_SWEEP_BANDS] = {};
	double   SweepFillSpacingU = 64;   // 0 = no lines in this axis
	double   SweepFillSpacingV = 64;
	double   SweepFillWidth = 3;       // world units, so it does not shimmer
	double   SweepFillSoft = 1.5;      // hard laser vs glowing filament
	double   SweepFillRotate = 0;      // degrees, in the band's own plane
	double   SweepFillDrift = 0;       // pattern sliding as the band travels
	double   SweepFillMajor = 0;       // every Nth line emphasised; 0 = off
	double   SweepFillMajorBoost = 2;  // how much wider a major line is
	double   SweepFillJitter = 0;      // emitters, not a texture
	double   SweepFillFlicker = 0;     // individual lines dropping out
	double   SweepFillGrad = 0;        // fade along one axis
	int      SweepFillGradAxis = 0;    // 0 = along V (height), 1 = along U
	double   SweepFillGap = 0;         // how much of the band colour fills the
	                                   // gaps. 0 = only the lines are lit and
	                                   // you see the room between them, which
	                                   // is what reads as actual lasers.
	PalEntry SweepFillColor = 0xFFFFFF;

	// [BB] THE ROOM THE LATTICE IS ALLOWED TO STAND IN. See the note beside
	// mSweepRoomMin in hw_viewpointuniforms.h for why an infinite plane needed
	// this at all. Caller-published from script, because "which sectors count
	// as one room" is a judgement the renderer has no business making --
	// SweepRoomSoft <= 0 means unbounded, which is what every map that never
	// calls it gets.
	DVector3 SweepRoomMin = DVector3(0, 0, 0);
	DVector3 SweepRoomMax = DVector3(0, 0, 0);
	double   SweepRoomSoft = 0;   // fade distance in units; 0 = no bound at all
	// How strongly the lattice is drawn IN THE AIR inside the band, rather
	// than only on the surfaces the band lands on. 0 = the old behaviour.
	double   SweepFillAir = 0;

	// [round2 B2] THE PASSED REGION: a look on everything a band's front has
	// already crossed, graded per pixel, so it follows the line exactly instead
	// of flipping a whole sector when its centre crosses. "Passed", not "wake":
	// SweepTrail is the wake. The test is the band light's own measure --
	// SweepShapeDist < radius -- which is behind the front for every shape.
	//
	// Look only: it grades the room's light in main.fp's getLightColor, not the
	// glow, bands, beams or stamps, the same line darkness draws. It lasts while
	// the band is live. Gameplay that must outlive the sweep stays per sector in
	// the mod. ClearSweep and ClearLevelData zero the per-band bits; the look
	// settings are left alone, per ClearLevelData's convention.
	int      SweepBandPassed[MAX_SWEEP_BANDS] = {};   // 0 off, 1 grade this band's passed side
	PalEntry SweepPassedTint = 0xFFFFFF;
	double   SweepPassedTintMix = 0;   // 0..1, multiply the light toward the tint
	double   SweepPassedDarken = 0;    // 0..1 of the light taken away
	double   SweepPassedDesat = 0;     // 0..1 toward grey
	double   SweepPassedSoft = 32;     // map units the look fades in over behind the front

	// [BB] REAL BEAMS.
	//
	// A laser in Doom is usually a sprite, or a chain of puffs spawned close
	// enough together to read as a line. Both are fakes and both show it: the
	// sprite does not light anything, and the chain stitches, gaps at long
	// range, and costs an actor per segment.
	//
	// A beam is a SEGMENT, and the honest way to draw one is the same way a
	// sweep band is drawn -- light every pixel by its distance from the thing.
	// The only difference is which distance:
	//
	//   sweep band   distance from a POINT     length(p - origin)
	//   beam         distance from a SEGMENT   length(p - closest(a,b))
	//
	// Everything else follows for free, because it is per pixel in world
	// space: the beam wraps across floor, wall and ceiling as one unbroken
	// object, it is continuous at any length, and the surfaces near it
	// brighten because they ARE near it rather than because something also
	// spawned a dynamic light.
	//
	// Not a sweep band, though, and deliberately so: a band's radius is a
	// distance that grows, and a beam does not travel. It simply is.
	// (How many: see MAX_BEAMS. [round2 SW-22])
// [BB] SHAPES. More than eight (16 when written, MAX_SHAPES = 128 now) -- eight was the beam budget, chosen for a
	// system where every slot costs a segment solve per fragment. A shape is a
	// couple of ALU behind an early reject, so the old cap was being copied
	// rather than reasoned about.
	static const int MAX_SHAPES = 128;
	DVector3 ShapePos[MAX_SHAPES];
	double   ShapeSize[MAX_SHAPES] = {};      // 0 = the slot is free
	int      ShapeKind[MAX_SHAPES] = {};      // 0 off, see hw_viewpointuniforms.h
	int      ShapeOrient[MAX_SHAPES] = {};    // 0 floor, 1 wall, 2 any, 3 standing (StandingShapesAt() in main.fp)
	double   ShapeAngle[MAX_SHAPES] = {};     // in-plane rotation for 0-2; plane-facing rotation around world-up for 3
	double   ShapeThick[MAX_SHAPES] = {};
	double   ShapeSeam[MAX_SHAPES] = {};      // 0 closed, 1 fully split
	PalEntry ShapeColor[MAX_SHAPES] = {};
	double   ShapeIntensity[MAX_SHAPES] = {};
	// Lifetime, resolved at render rate so a seam opens smoothly rather than
	// in 35Hz steps -- same reason the disturbances resolve their age there.
	double   ShapeBirth[MAX_SHAPES] = {};
	double   ShapeLife[MAX_SHAPES] = {};       // 0 = it never expires
	double   ShapeGrow[MAX_SHAPES] = {};       // size added per second
	double   ShapeSeamRate[MAX_SHAPES] = {};   // seam opened per second

	// Repeat: one slot drawing a formation. Mode 0 is a single shape.
	int      ShapeRepeat[MAX_SHAPES] = {};     // 0 single, 1 radial, 2 grid
	double   ShapeRepCount[MAX_SHAPES] = {};   // radial: how many. grid: extent
	double   ShapeRepSpace[MAX_SHAPES] = {};   // radial: orbit. grid: spacing
	double   ShapeRepSpin[MAX_SHAPES] = {};    // deg/sec, or drift units/sec

	// FULL 3D ORIENTATION AND LINKING, for orient 3 (standing) shapes only --
	// see StandingShapesAt() in main.fp. ShapeAngle above is the base YAW;
	// these add pitch and roll, and a rate for all three so a shape can spin
	// or tumble without a per-tic ZScript call, resolved the same way
	// ShapeGrow/ShapeSeamRate already are (base + rate * age, once per frame,
	// natively -- the shader only ever sees the final resolved values).
	double   ShapePitch[MAX_SHAPES] = {};
	double   ShapeRoll[MAX_SHAPES] = {};
	double   ShapeYawRate[MAX_SHAPES] = {};    // deg/sec
	double   ShapePitchRate[MAX_SHAPES] = {};  // deg/sec
	double   ShapeRollRate[MAX_SHAPES] = {};   // deg/sec

	// LINKING: a shape's world position and orientation compose with its
	// parent's, so moving or rotating the parent carries every shape linked
	// to it -- how a compound 3D object gets built out of flat panels.
	//
	// CALLER-MANAGED, LIKE THE BEAM INDEX SPACE: ShapeParent MUST name a
	// slot with a SMALLER index than its own, so a single forward pass over
	// the array resolves every parent before the child that reads it. There
	// is no cycle check and no topological sort -- a parent index >= your
	// own, or a cycle, is undefined and will not be caught.
	//
	// Local offset composes along the PARENT's resolved basis, not world
	// axes: X along the parent's own facing, Y along its right, Z along its
	// up -- so "64 units out, turned 90 degrees" means the same thing
	// however the parent itself is currently oriented.
	//
	// Local yaw/pitch/roll ADD to the parent's resolved yaw/pitch/roll.
	// This is Euler addition, not true rotation composition -- exact for a
	// pure-yaw chain (a fan of panels around one vertical hinge, which is
	// most of what "build a box or a fan out of panels" needs), and an
	// approximation once pitch and roll are combined at the same joint.
	// Documented rather than hidden: a wrong answer that says so is a
	// starting point, a silently wrong one is the bug this project keeps
	// finding.
	int      ShapeParent[MAX_SHAPES] = {};     // -1 = no parent
	DVector3 ShapeLocalPos[MAX_SHAPES] = {};
	double   ShapeLocalYaw[MAX_SHAPES] = {};
	double   ShapeLocalPitch[MAX_SHAPES] = {};
	double   ShapeLocalRoll[MAX_SHAPES] = {};

	double   ShapeSoft = 2.0;
	double   ShapeHeightFade = 24.0;
	double   ShapeReach = 0.0;
	PalEntry ShapeUnder = 0xffff2610;

	// 128. Raised from 8 so a real firefight can have bolts crossing in both
	// directions. The arrays below are vec4, which is the one case where
	// std140 and the C++ layout agree naturally, so growing them keeps the
	// shader offsets aligned. 3 arrays x 128 x 16B = 6KB per viewpoint, x2
	// viewpoints = 12KB against a 64KB uniform range.
	//
	// COST IS PER ACTIVE BEAM, NOT PER SLOT. Both shader loops break as soon
	// as i reaches the live count, and each surviving beam gets a cheap
	// bounding-sphere reject before the real solve. An empty slot costs
	// nothing.
	// [STAMP] Surface stamps. An impact publishes one and forgets it; the
	// engine ages it and lets it go. Parallel arrays rather than a struct
	// array, matching the beams below, so the upload is a straight copy.
	//
	// A stamp is an EVENT with a life, where a beam is a SLOT a script owns --
	// so unlike the beams these are not addressed by index from script. Spawn
	// takes the oldest slot when all are busy, which is the right thing
	// to lose in a firefight: the stamp that has been fading longest.
	//
	// 64 (raised from 16, which ran out in a firefight) to match the shader and
	// both GLSL viewpoint blocks and HWViewpointUniforms::mStamp*. FIVE places
	// say this number. See the note on MAX_SURFACE_STAMPS in
	// func_surfacestamps.fp; a mismatch is silent corruption, not an error.
	static const int MAX_SURFACE_STAMPS = 64;

	DVector3 StampPos[MAX_SURFACE_STAMPS] = {};
	DVector3 StampAxis[MAX_SURFACE_STAMPS] = {};   // world space; shader projects it
	double   StampRadius[MAX_SURFACE_STAMPS] = {};
	PalEntry StampColor[MAX_SURFACE_STAMPS] = {};
	int      StampShape[MAX_SURFACE_STAMPS] = {};
	int      StampTex[MAX_SURFACE_STAMPS] = {};    // 0 = no second layer
	double   StampTexStrength[MAX_SURFACE_STAMPS] = {};
	int      StampAge[MAX_SURFACE_STAMPS] = {};    // tics since spawn
	int      StampLife[MAX_SURFACE_STAMPS] = {};   // tics total; 0 = slot free

	// Publish one. Fire and forget: no handle comes back because there is
	// nothing to keep writing to -- P_Ticker ages it and drops it. Takes the
	// oldest slot when all are busy, which in a firefight is the right thing to
	// lose: the stamp that has been fading longest is the one nobody is looking
	// at, and evicting the NEWEST would mean the shot you just fired is the one
	// that fails to register.
	//
	// axis is a world direction for the oriented shapes and may be zero for the
	// rest; the shader projects it onto whichever surface it lands on. tex 0
	// means no second layer. life is in tics.
	//
	// Inline and on the level rather than a static helper in vmthunks.cpp,
	// because more than script publishes these -- the `stamp` CCMD does too,
	// and native gameplay code is the obvious next caller.
	void SpawnSurfaceStamp(int shape, const DVector3 &pos, double radius,
		PalEntry color, int life, const DVector3 &axis, int tex, double texStrength)
	{
		if (radius <= 0.0 || life <= 0) return;

		int slot = -1;
		for (int i = 0; i < MAX_SURFACE_STAMPS; i++)
		{
			if (StampLife[i] <= 0) { slot = i; break; }
		}
		if (slot < 0)
		{
			double worst = -1.0;
			for (int i = 0; i < MAX_SURFACE_STAMPS; i++)
			{
				const double prog = StampAge[i] / (double)StampLife[i];
				if (prog > worst) { worst = prog; slot = i; }
			}
		}

		StampShape[slot] = shape;
		StampPos[slot] = pos;
		StampAxis[slot] = axis;
		StampRadius[slot] = radius;
		StampColor[slot] = color;
		StampTex[slot] = tex;
		StampTexStrength[slot] = texStrength;
		StampAge[slot] = 0;
		StampLife[slot] = life;
	}

	void ClearSurfaceStamps()
	{
		for (int i = 0; i < MAX_SURFACE_STAMPS; i++)
		{
			StampLife[i] = 0;
			StampRadius[i] = 0.0;
		}
	}

	// [GPUPARTICLES] STATELESS GPU PARTICLES -- the renderer-agnostic half.
	//
	// Every other procedural effect here is a field; this is matter. A record
	// is a particle's STARTING conditions, and the vertex shader works out where
	// it is now from level time, so nothing is stepped per tic and nothing is
	// re-uploaded per frame. This ring and the two members below are what a
	// renderer rebuild keeps; GpuParticleBuffer (hw_gpuparticlebuffer.h), the
	// shaders and the draw call are what it replaces.
	//
	// See "Engine docs/GPU_PARTICLES_PLAN.md".
	//
	// Record, SHADER space (y up), five vec4s, 80 bytes, std430 with no padding.
	// Must match GpuParticleBuffer::RECORD_BYTES and the GpuParticle struct in
	// vk_shader.cpp's prolog.
	//
	// [PARTICLEDEFS] THE STAGE 2 LAYOUT ("Engine docs/GPU_PARTICLES_STAGE2_PLAN.md"
	// 2b). What a particle looks like over its life lives in a particle definition
	// (gamedata/particledefs.h, set 1 binding 7); the record keeps what differs per
	// particle. Same size, same ring, same sync rule as stage 1.
	struct GpuParticleRecord
	{
		float a[4];   // xyz spawn position,               w birth, level seconds
		float b[4];   // xyz initial velocity, units/s,    w life, seconds (0 = free slot)
		float c[4];   // rgb tint 0..1 (times the definition's colour),  w intensity scale
		float d[4];   // x definition slot, y size scale, z ambient light at spawn 0..1, w seed 0..1
		float e[4];   // xy surface normal, octahedral (x = 2: no plane), z plane offset, w floor height (-32768: none)
	};
	// e.x of a record with no collision plane. An octahedral normal's x is in [-1, 1].
	static constexpr float GPUPARTICLE_NO_PLANE = 2.f;
	// e.w of a record with no floor.
	static constexpr float GPUPARTICLE_NO_FLOOR = -32768.f;
	// [2b] LEGACY records, written only while r_gpuparticles_legacy is on (the
	// bring-up A/B), keep the stage 1 layout -- c rgb colour and intensity; d size
	// start, size end, gravity, drag; e.y stretch -- with e.x = this + orient
	// (-16..-14), which no stage 2 record can hold. gpuparticles.vp draws them with
	// the stage 1 code. Goes when the A/B does.
	static constexpr float GPUPARTICLE_LEGACY_TAG = -16.f;

	TArray<GpuParticleRecord> GpuParticles;   // empty until the first spawn this process
	// Every record ever written this level; the write cursor is Written % size.
	uint64_t GpuParticleWritten = 0;
	// Per-level serial from a global counter, NOT this object's address: a new
	// level can be allocated where the last one was, and the renderer uses the
	// serial to know it must re-upload the whole ring.
	uint64_t GpuParticleSerial = 0;

	static uint64_t GpuParticleNewSerial()
	{
		static uint64_t counter = 0;
		return ++counter;
	}

	// Local integer hash. ALL spawn jitter comes from this -- never random(),
	// never FRandom, never any playsim RNG stream -- so a spawn is netplay-safe
	// by construction, even when the caller is gated on the local player.
	static uint32_t GpuParticleHash(uint32_t x)
	{
		x ^= x >> 16; x *= 0x7feb352dU;
		x ^= x >> 15; x *= 0x846ca68bU;
		x ^= x >> 16;
		return x;
	}

	static double GpuParticleRand(uint32_t seed, uint32_t index, uint32_t stream)
	{
		return GpuParticleHash(seed ^ GpuParticleHash(index * 8U + stream + 0x68bc21ebU)) / 4294967296.0;
	}

	// Sized on first use from the same latched capacity the GPU ring uses, so
	// the two can never disagree within a run. Kept allocated across levels.
	void EnsureGpuParticleRing()
	{
		if (GpuParticles.Size() > 0) return;
		extern int GpuParticleRingCapacity();   // hw_cvars.cpp, r_gpuparticles_ringsize
		GpuParticles.Resize((unsigned)GpuParticleRingCapacity());
		for (auto &r : GpuParticles) r = GpuParticleRecord();
		if (GpuParticleSerial == 0) GpuParticleSerial = GpuParticleNewSerial();
	}

	// Write `count` records at the cursor. NEVER REFUSES: the oldest records are
	// overwritten, for FogDisturb's reason -- a refusal makes the hundredth
	// spark in a firefight silently do nothing, at the one moment the effect
	// exists for.
	//
	// Game space in, shader space out, for position AND velocity. spread is the
	// cone's half-angle in degrees around dir. speed in map units per second,
	// life in seconds, gravity in map units/s^2, drag in 1/s. seed 0 derives one
	// from the cursor.
	//
	// Inline and on the level, like SpawnSurfaceStamp, so native gameplay code
	// can publish these as well as script.
	//
	// [PARTICLEDEFS] Since stage 2b the look is an inline particle definition
	// (InlineParticleDefinition) and the colour and intensity ride in the record;
	// the look on screen is stage 1's. Returns nothing: particles are presentation,
	// and nothing in the simulation may read them back.
	void SpawnGpuParticles(const DVector3 &pos, const DVector3 &dir, int count,
		double spread, double speed, double speedJitter,
		PalEntry color, double intensity, double life, double lifeJitter,
		double sizeStart, double sizeEnd, double gravity, double drag,
		int orient, double stretch, int seed)
	{
		if (count <= 0 || life <= 0.0) return;

		EnsureGpuParticleRing();
		const unsigned size = GpuParticles.Size();
		if (size == 0) return;
		if ((unsigned)count > size) count = (int)size;

		const uint32_t s = seed != 0 ? (uint32_t)seed
			: GpuParticleHash((uint32_t)GpuParticleWritten ^ (uint32_t)(GpuParticleWritten >> 32) ^ 0x9e3779b9U);

		// Cone axis and a basis around it, in game space.
		const double kPi = 3.14159265358979323846;
		double ax = dir.X, ay = dir.Y, az = dir.Z;
		const double al = sqrt(ax * ax + ay * ay + az * az);
		if (al < 1e-9) { ax = 0.0; ay = 0.0; az = 1.0; }
		else { ax /= al; ay /= al; az /= al; }

		const double sx = (az < 0.9 && az > -0.9) ? 0.0 : 1.0;
		const double sz = (az < 0.9 && az > -0.9) ? 1.0 : 0.0;
		double t1x = -sz * ay, t1y = sz * ax - sx * az, t1z = sx * ay;   // seed x axis
		const double tl = sqrt(t1x * t1x + t1y * t1y + t1z * t1z);
		t1x /= tl; t1y /= tl; t1z /= tl;
		const double t2x = ay * t1z - az * t1y, t2y = az * t1x - ax * t1z, t2z = ax * t1y - ay * t1x;

		const double spreadDeg = spread < 0.0 ? 0.0 : (spread > 180.0 ? 180.0 : spread);
		const double cosMax = cos(spreadDeg * kPi / 180.0);

		// Birth on the tic clock, the same basis as FogDisturb. Sub-tic
		// smoothness comes from uLevelTime including TicFrac.
		const float birth = (float)(maptime / (double)TICRATE);
		const int mode = orient < 0 ? 0 : (orient > 2 ? 2 : orient);

		// [PARTICLEDEFS] Where the look goes (stage 2b). By default the look --
		// sizes, gravity, drag, orient, stretch -- becomes an inline particle
		// definition (gamedata/particledefs.h), shared by every burst with the same
		// tuple. Colour and intensity stay in the record as its tint and intensity
		// scale, so one look is one definition whatever its colour or brightness.
		// With r_gpuparticles_legacy on (the bring-up A/B) the stage 1 record is
		// written instead, tagged, and gpuparticles.vp draws it with stage 1's code.
		extern bool GpuParticlesLegacyPath();   // hw_cvars.cpp, r_gpuparticles_legacy
		const bool legacy = GpuParticlesLegacyPath();
		int definition = 0;
		if (!legacy)
		{
			// The longest life any record below can get, so the definition's slot
			// is kept until the last of them has died.
			double longestLife = life * (1.0 + fabs(lifeJitter));
			if (longestLife < 1e-3) longestLife = 1e-3;
			definition = InlineParticleDefinition((float)sizeStart, (float)sizeEnd, (float)gravity, (float)drag,
				mode, (float)stretch, GpuParticleSerial, birth, longestLife);
		}

		for (int i = 0; i < count; i++)
		{
			const double u1 = GpuParticleRand(s, (uint32_t)i, 0);
			const double u2 = GpuParticleRand(s, (uint32_t)i, 1);
			const double u3 = GpuParticleRand(s, (uint32_t)i, 2);
			const double u4 = GpuParticleRand(s, (uint32_t)i, 3);

			// Uniform over the spherical cap.
			const double cosT = 1.0 - u1 * (1.0 - cosMax);
			const double sin2 = 1.0 - cosT * cosT;
			const double sinT = sin2 > 0.0 ? sqrt(sin2) : 0.0;
			const double phi = u2 * 2.0 * kPi;
			const double cp = cos(phi) * sinT, sp = sin(phi) * sinT;
			const double vx = ax * cosT + t1x * cp + t2x * sp;
			const double vy = ay * cosT + t1y * cp + t2y * sp;
			const double vz = az * cosT + t1z * cp + t2z * sp;

			const double spd = speed * (1.0 + speedJitter * (2.0 * u3 - 1.0));
			double lf = life * (1.0 + lifeJitter * (2.0 * u4 - 1.0));
			if (lf < 1e-3) lf = 1e-3;

			GpuParticleRecord &r = GpuParticles[(unsigned)(GpuParticleWritten % size)];
			// Game (x, y, z) -> shader (x, z, y): y is up in shader space.
			r.a[0] = (float)pos.X;      r.a[1] = (float)pos.Z;      r.a[2] = (float)pos.Y;      r.a[3] = birth;
			r.b[0] = (float)(vx * spd); r.b[1] = (float)(vz * spd); r.b[2] = (float)(vy * spd); r.b[3] = (float)lf;
			r.c[0] = color.r / 255.f;   r.c[1] = color.g / 255.f;   r.c[2] = color.b / 255.f;   r.c[3] = (float)intensity;
			if (legacy)
			{
				// [2b] Stage 1 layout, e.x tagged.
				r.d[0] = (float)sizeStart;  r.d[1] = (float)sizeEnd;    r.d[2] = (float)gravity;    r.d[3] = (float)drag;
				r.e[0] = GPUPARTICLE_LEGACY_TAG + (float)mode;  r.e[1] = (float)stretch;  r.e[2] = 0.f;  r.e[3] = 0.f;
			}
			else
			{
				// [PARTICLEDEFS] Stage 2 layout: the inline definition, size scale 1,
				// ambient 1 (inline definitions are unlit), a seed; no plane, no floor.
				r.d[0] = (float)definition; r.d[1] = 1.f; r.d[2] = 1.f; r.d[3] = (float)GpuParticleRand(s, (uint32_t)i, 4);
				r.e[0] = GPUPARTICLE_NO_PLANE; r.e[1] = 0.f; r.e[2] = 0.f; r.e[3] = GPUPARTICLE_NO_FLOOR;
			}
			GpuParticleWritten++;
		}
	}

	// [PARTICLEDEFS] Emission directions in game space, for SpawnParticles. A basis
	// around `dir` (unit axis a, across t1 and t2), then a direction from two
	// uniform numbers. The cone is SpawnGpuParticles' own maths, line for line;
	// SpawnGpuParticles keeps its inline copy until the stage 2b A/B has proven the
	// definitions path, and can call these after.
	struct GpuParticleBasis
	{
		double ax, ay, az;
		double t1x, t1y, t1z;
		double t2x, t2y, t2z;
	};

	static GpuParticleBasis GpuParticleMakeBasis(const DVector3 &dir)
	{
		GpuParticleBasis B;
		B.ax = dir.X; B.ay = dir.Y; B.az = dir.Z;
		const double al = sqrt(B.ax * B.ax + B.ay * B.ay + B.az * B.az);
		if (al < 1e-9) { B.ax = 0.0; B.ay = 0.0; B.az = 1.0; }
		else { B.ax /= al; B.ay /= al; B.az /= al; }

		const double sx = (B.az < 0.9 && B.az > -0.9) ? 0.0 : 1.0;
		const double sz = (B.az < 0.9 && B.az > -0.9) ? 1.0 : 0.0;
		B.t1x = -sz * B.ay; B.t1y = sz * B.ax - sx * B.az; B.t1z = sx * B.ay;
		const double tl = sqrt(B.t1x * B.t1x + B.t1y * B.t1y + B.t1z * B.t1z);
		B.t1x /= tl; B.t1y /= tl; B.t1z /= tl;
		B.t2x = B.ay * B.t1z - B.az * B.t1y; B.t2y = B.az * B.t1x - B.ax * B.t1z; B.t2z = B.ax * B.t1y - B.ay * B.t1x;
		return B;
	}

	// Uniform over the spherical cap within acos(cosMax) of the axis.
	static DVector3 GpuParticleConeDirection(const GpuParticleBasis &B, double cosMax, double u1, double u2)
	{
		const double kPi = 3.14159265358979323846;
		const double cosT = 1.0 - u1 * (1.0 - cosMax);
		const double sin2 = 1.0 - cosT * cosT;
		const double sinT = sin2 > 0.0 ? sqrt(sin2) : 0.0;
		const double phi = u2 * 2.0 * kPi;
		const double cp = cos(phi) * sinT, sp = sin(phi) * sinT;
		return DVector3(B.ax * cosT + B.t1x * cp + B.t2x * sp,
			B.ay * cosT + B.t1y * cp + B.t2y * sp,
			B.az * cosT + B.t1z * cp + B.t2z * sp);
	}

	// A disc across the axis, lifted toward it by up to liftMax radians: fire
	// splashing along the wall it hit (FLAME_ENGINE_PLAN F4).
	static DVector3 GpuParticleDiscDirection(const GpuParticleBasis &B, double liftMax, double u1, double u2)
	{
		const double kPi = 3.14159265358979323846;
		const double lift = u1 * liftMax;
		const double cl = cos(lift), sl = sin(lift);
		const double phi = u2 * 2.0 * kPi;
		const double cp = cos(phi) * cl, sp = sin(phi) * cl;
		return DVector3(B.ax * sl + B.t1x * cp + B.t2x * sp,
			B.ay * sl + B.t1y * cp + B.t2y * sp,
			B.az * sl + B.t1z * cp + B.t2z * sp);
	}

	// [PARTICLEDEFS] SPAWN FROM A NAMED DEFINITION (stage 2b). `definition` is a
	// handle from LevelLocals.ParticleDefinition(name) (ParticleDefinitionHandle).
	// The look over life -- size, colour, occlusion and light ramps, gravity, drag,
	// spin, collision -- is the definition's; this call gives what differs per
	// burst: where, which way, how many, how fast, how long, and a tint, a
	// brightness and a size scale on top.
	//
	//   shape          0 cone: `spread` is the half-angle around dir, as SpawnGpuParticles
	//                  1 disc: across dir, lifted toward it by up to `spread` degrees (0..90)
	//   surfacePoint,  a plane the particles stay in front of, when the definition says
	//   surfaceNormal  `collide = plane`; a zero normal is no plane (FLAME_ENGINE_PLAN F4)
	//   floorZ         a floor they skid along (same condition); -32768 is none
	//
	// NEVER REFUSES for space: the ring overwrites its oldest, as SpawnGpuParticles.
	// A handle with no definition on this machine spawns nothing and says so once.
	// NETPLAY: returns nothing, and what happens here only ever changes this
	// machine's pixels (particledefs.h). All jitter is GpuParticleHash, never
	// playsim RNG. The ambient light at spawn (for lit definitions, drawn from 2d)
	// is the sector light at pos, read once per call from map data.
	void SpawnParticles(int definition, const DVector3 &pos, const DVector3 &dir, int count,
		double spread, double speed, double speedJitter, double life, double lifeJitter,
		PalEntry tint, double intensity, double sizeScale, int seed,
		int shape, const DVector3 &surfacePoint, const DVector3 &surfaceNormal, double floorZ)
	{
		if (count <= 0 || life <= 0.0) return;

		const int defSlot = ResolveParticleDefinitionHandle(definition, true);
		if (defSlot < 0) return;

		EnsureGpuParticleRing();
		const unsigned size = GpuParticles.Size();
		if (size == 0) return;
		if ((unsigned)count > size) count = (int)size;

		const uint32_t s = seed != 0 ? (uint32_t)seed
			: GpuParticleHash((uint32_t)GpuParticleWritten ^ (uint32_t)(GpuParticleWritten >> 32) ^ 0x9e3779b9U);

		const double kPi = 3.14159265358979323846;
		const GpuParticleBasis basis = GpuParticleMakeBasis(dir);
		const bool disc = shape == 1;
		const double cosMax = cos(clamp(spread, 0.0, 180.0) * kPi / 180.0);
		const double liftMax = clamp(spread, 0.0, 90.0) * kPi / 180.0;

		// Birth on the tic clock, as SpawnGpuParticles.
		const float birth = (float)(maptime / (double)TICRATE);

		float ambient = 1.f;
		if (sector_t *sec = PointInSector(pos))
			ambient = (float)clamp(sec->lightlevel / 255.0, 0.0, 1.0);

		// The plane in SHADER space (game x, y, z -> shader x, z, y) as an octahedral
		// normal with shader z as the pole -- gpuparticles.vp's OctahedralDecode --
		// and its offset along that normal.
		float planeX = GPUPARTICLE_NO_PLANE, planeY = 0.f, planeOffset = 0.f;
		const double normalLength = surfaceNormal.Length();
		if (normalLength > 1e-6)
		{
			const double nx = surfaceNormal.X / normalLength;
			const double ny = surfaceNormal.Z / normalLength;
			const double nz = surfaceNormal.Y / normalLength;
			const double sum = fabs(nx) + fabs(ny) + fabs(nz);
			double ox = nx / sum, oy = ny / sum;
			if (nz < 0.0)
			{
				const double fx = (1.0 - fabs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
				const double fy = (1.0 - fabs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
				ox = fx;
				oy = fy;
			}
			planeX = (float)ox;
			planeY = (float)oy;
			planeOffset = (float)(nx * surfacePoint.X + ny * surfacePoint.Z + nz * surfacePoint.Y);
		}
		const float floorHeight = floorZ > -32768.0 ? (float)floorZ : GPUPARTICLE_NO_FLOOR;

		for (int i = 0; i < count; i++)
		{
			const double u1 = GpuParticleRand(s, (uint32_t)i, 0);
			const double u2 = GpuParticleRand(s, (uint32_t)i, 1);
			const double u3 = GpuParticleRand(s, (uint32_t)i, 2);
			const double u4 = GpuParticleRand(s, (uint32_t)i, 3);
			const double u5 = GpuParticleRand(s, (uint32_t)i, 4);

			const DVector3 v = disc ? GpuParticleDiscDirection(basis, liftMax, u1, u2) : GpuParticleConeDirection(basis, cosMax, u1, u2);
			const double spd = speed * (1.0 + speedJitter * (2.0 * u3 - 1.0));
			double lf = life * (1.0 + lifeJitter * (2.0 * u4 - 1.0));
			if (lf < 1e-3) lf = 1e-3;

			GpuParticleRecord &r = GpuParticles[(unsigned)(GpuParticleWritten % size)];
			// Game (x, y, z) -> shader (x, z, y): y is up in shader space.
			r.a[0] = (float)pos.X;        r.a[1] = (float)pos.Z;        r.a[2] = (float)pos.Y;        r.a[3] = birth;
			r.b[0] = (float)(v.X * spd);  r.b[1] = (float)(v.Z * spd);  r.b[2] = (float)(v.Y * spd);  r.b[3] = (float)lf;
			r.c[0] = tint.r / 255.f;      r.c[1] = tint.g / 255.f;      r.c[2] = tint.b / 255.f;      r.c[3] = (float)intensity;
			r.d[0] = (float)defSlot;      r.d[1] = (float)sizeScale;    r.d[2] = ambient;             r.d[3] = (float)u5;
			r.e[0] = planeX;              r.e[1] = planeY;              r.e[2] = planeOffset;         r.e[3] = floorHeight;
			GpuParticleWritten++;
		}
	}

	// Zero every life and push the cursor on by a whole ring, which the
	// renderer's sync rule reads as "upload everything" on the next scene.
	void ClearGpuParticles()
	{
		const unsigned size = GpuParticles.Size();
		if (size == 0) return;
		for (auto &r : GpuParticles) r.b[3] = 0.f;
		GpuParticleWritten += size;
	}

	// Map change and savegame load (ClearLevelData). A new serial, so the
	// renderer re-uploads the emptied ring before anything draws.
	void ResetGpuParticles()
	{
		for (auto &r : GpuParticles) r = GpuParticleRecord();
		GpuParticleWritten = 0;
		GpuParticleSerial = GpuParticleNewSerial();
	}

	// [round2 SW-22] 128 beam slots. The note for this used to read "eight,
	// because that is enough for a weapon beam plus a tripwire grid, and the
	// per-fragment cost is eight cheap segment tests", orphaned above the
	// SHAPES block long after the count grew. It must match mBeamA[128] and
	// mBeamLook[128] in hw_viewpointuniforms.h and both GLSL copies; the upload
	// carries the live count in mBeamParams.x.
	static const int MAX_BEAMS = 128;
	int      BeamCount = 0;
	// RS FORK -- WHERE A BEAM'S ORIGIN COMES FROM, per slot.
	//
	//   0  the world point in BeamStart, as always
	//   1  the MAIN hand, resolved at DRAW rate
	//   2  the OFF hand
	//
	// A laser sight starts at the weapon, and a weapon in VR moves at head
	// tracking rate -- 90Hz and up. Script runs at 35. So a beam whose origin
	// was written from script sampled AttackPos once per tic and held it, and no
	// amount of interpolating between two such samples recovers the motion
	// between them: the beam steps against a gun that glides. Reported as "the
	// laser is jittery when I move around", and it gets worse with speed
	// because the disagreement grows with it.
	//
	// Anchored instead, the draw path reads the hand's CURRENT position -- the
	// same value hw_vrmodes.cpp refreshed this frame -- so the beam leaves the
	// muzzle and stays there. The far END is still the interpolated world point,
	// correctly, because a hit location genuinely only changes once a tic.
	//
	// This is the beam equivalent of AActor::FollowHandMode and exists for the
	// identical reason. Zero by default: every existing caller is untouched.
	int      BeamAnchor[MAX_BEAMS] = {};

	DVector3 BeamStart[MAX_BEAMS] = {};
	DVector3 BeamEnd[MAX_BEAMS] = {};
	double   BeamThick[MAX_BEAMS] = {};   // the hot core, world units
	double   BeamSoft[MAX_BEAMS] = {};    // how far the halo reaches past it
	PalEntry BeamColor[MAX_BEAMS] = {};
	double   BeamIntensity[MAX_BEAMS] = {};

	// LAST TIC'S ENDPOINTS, so the renderer can interpolate.
	//
	// Script writes beams once per tic at 35Hz, but the renderer uploads them
	// every frame. Without these the beam teleports 35 times a second while
	// everything around it moves smoothly, which reads as a strobing beam at
	// any framerate above the tic rate. P_Ticker copies current -> prev at the
	// top of each tic, at the same instant AActor::Prev is taken, so a beam
	// anchored to an actor interpolates in lockstep with that actor.
	//
	// PrevBeamIntensity AND PrevBeamCount ARE BOTH LOAD-BEARING, not spare
	// diagnostics. Every beam user in RS_Lance releases a slot by writing
	// (0,0,0)->(0,0,0) with intensity 0, so on the tic a beam switches back on
	// prev is the map origin. Interpolating that sweeps the beam across the
	// entire level for one tic, lighting everything it crosses. The renderer
	// tests both fields and snaps instead of lerping whenever a slot was dark
	// or out of range last tic. Slots can also go live purely by BeamCount
	// growing, which is why the count is remembered too.
	DVector3 PrevBeamStart[MAX_BEAMS] = {};
	DVector3 PrevBeamEnd[MAX_BEAMS] = {};
	double   PrevBeamIntensity[MAX_BEAMS] = {};
	int      PrevBeamCount = 0;
	double   BeamGlow = 0.35;             // halo strength relative to the core
	double   BeamFogScatter = 1.0;        // how much a beam lights fog it crosses

	// The beam seen IN THE AIR rather than the light it casts on surfaces.
	// 0 means it only lights what it touches, which is a spotlight; turn it up
	// and the beam is an object hanging in space. Depth-correct for free --
	// see BeamAirGlow in main.fp.
	double   BeamAirGlow = 1.0;
	double   BeamScrollSpeed = 6.0;       // energy travelling muzzle to impact
	double   BeamScrollDepth = 0.25;      // 0 = a smooth beam
	double   BeamTaper = 0.35;            // thinner at the muzzle than the hit
	double   BeamFlare = 1.5;             // brightness boost where it lands

	// [BEAMLINES] CLAIMED SLOTS AND A LOOK PER LINE.
	// See "Engine docs/BEAM_LINES_PLAN.md".
	//
	// BeamCount and the SetBeamLook values are single numbers for the scene, so
	// every SetBeam caller fights over them (lance.zs and wr_constellation.zs
	// both say so in their own comments). These let a line opt out of that
	// fight. All of it is inert until ClaimBeam or SetBeamStyle is called:
	//
	//   BeamClaimed[i]   ClaimBeam handed this slot out. A claimed slot renders
	//                    whatever BeamCount says and ClearBeams leaves it alone.
	//                    Cleared on map change (ClearLevelData); never
	//                    serialized, so a caller holding one across a load
	//                    checks IsBeamClaimed and claims again.
	//   BeamHasStyle[i]  SetBeamStyle gave the slot its own air glow / halo /
	//                    taper / flare. Without it the renderer uploads the
	//                    scene values, so a line that never sets a style reaches
	//                    the shader with exactly the numbers it always did.
	//   PrevBeamLive[i]  was the slot live -- below BeamCount, or claimed -- at
	//                    the last tic snapshot. The per-slot form of
	//                    PrevBeamCount, which it agrees with exactly while
	//                    nothing is claimed. The renderer's lerp test and the
	//                    snapshot's forget-the-anchor rule both read it.
	//
	// Scroll speed/depth and fog scatter stay scene-wide: one user each so far.
	bool     BeamClaimed[MAX_BEAMS] = {};
	// The actor a claim belongs to, if the claimer named one (ClaimBeam(owner)).
	// P_Ticker releases the slot on the tic that actor is destroyed -- the same
	// tic on every machine -- so a caller that never calls ReleaseBeam cannot
	// leak slots. BeamClaimOwned says an owner was given; the pointer is only
	// read while it is set, and FLevelLocals::Mark keeps it GC-safe.
	TObjPtr<AActor*> BeamClaimOwner[MAX_BEAMS];
	bool     BeamClaimOwned[MAX_BEAMS] = {};
	bool     BeamHasStyle[MAX_BEAMS] = {};
	double   BeamStyleAirGlow[MAX_BEAMS] = {};
	double   BeamStyleHalo[MAX_BEAMS] = {};
	double   BeamStyleTaper[MAX_BEAMS] = {};
	double   BeamStyleFlare[MAX_BEAMS] = {};
	bool     PrevBeamLive[MAX_BEAMS] = {};
	bool     BeamClaimFullLogged = false;   // ClaimBeam's "no free slot" line: once per map
	bool     BeamCountOverClaimLogged = false;   // SetBeamCount grew over a claim: once per map

	// A slot draws if it is below BeamCount (the legacy rule) or claimed.
	bool BeamSlotLive(int i) const
	{
		return i < BeamCount || BeamClaimed[i];
	}

	// Searches from the TOP down, and never below BeamCount: legacy callers use
	// low fixed indices, so a claim stays out of their way. The slot is handed
	// over blank -- dark, unanchored, unstyled, and with no history, so the
	// first SetBeam snaps instead of lerping from whatever the slot last held.
	//
	// It cannot protect against a legacy caller RAISING BeamCount over a claimed
	// slot later. Both would then write the same slot.
	int ClaimBeam()
	{
		for (int i = MAX_BEAMS - 1; i >= 0 && i >= BeamCount; i--)
		{
			if (BeamClaimed[i]) continue;
			BeamClaimed[i] = true;
			BeamIntensity[i] = 0.0;
			PrevBeamIntensity[i] = 0.0;
			BeamAnchor[i] = 0;
			BeamHasStyle[i] = false;
			return i;
		}
		return -1;
	}

	// Only a CLAIMED slot is released, so a stray call cannot blank a legacy
	// caller's line. Intensity 0 means the next claimer snaps rather than lerps.
	void ReleaseBeam(int index)
	{
		if (index < 0 || index >= MAX_BEAMS || !BeamClaimed[index]) return;
		BeamClaimed[index] = false;
		BeamClaimOwned[index] = false;
		BeamIntensity[index] = 0.0;
		BeamAnchor[index] = 0;
		BeamHasStyle[index] = false;
	}

	bool IsBeamClaimed(int index) const
	{
		return index >= 0 && index < MAX_BEAMS && BeamClaimed[index];
	}

	// Any slot, claimed or not.
	void SetBeamStyle(int index, double airGlow, double halo, double taper, double flare)
	{
		if (index < 0 || index >= MAX_BEAMS) return;
		BeamHasStyle[index] = true;
		BeamStyleAirGlow[index] = airGlow;
		BeamStyleHalo[index] = halo;
		BeamStyleTaper[index] = taper;
		BeamStyleFlare[index] = flare;
	}

	void ClearBeamStyle(int index)
	{
		if (index < 0 || index >= MAX_BEAMS) return;
		BeamHasStyle[index] = false;
	}

	// [DRAWNLINES] MANY GLOWING LINES -- the renderer-agnostic half.
	//
	// The beam slots above are lit PER PIXEL: every fragment of every surface
	// runs the closest-approach solve for every live slot. That is what lets
	// them light walls, and it is why they cap at 128 and cost pixels x lines.
	// A tracer storm or a laser grid needs thousands.
	//
	// A drawn line has the same look -- main.fp's air-glow maths, run in the
	// fragment shader of a box around the line -- so it costs only the pixels
	// near it. What it gives up is lighting surfaces; drawnlines.fp lists every
	// place the two cannot match.
	//
	// This array and the calls below are what a renderer rebuild keeps;
	// DrawnLineBuffer (hw_drawnlinebuffer.h), drawnlines.vp/.fp and the draw in
	// HWDrawInfo::RenderTranslucent are what it replaces. r_beams_drawn routes
	// the beam slots above through the same draw, for A/B comparison.
	//
	// The index space is CALLER-MANAGED, 0 .. DrawnLineCapacity()-1, like
	// SetBeam's. Game space in; the renderer swizzles to shader space.
	struct DrawnLine
	{
		DVector3 Start{ 0., 0., 0. };
		DVector3 End{ 0., 0., 0. };
		DVector3 PrevStart{ 0., 0., 0. };
		DVector3 PrevEnd{ 0., 0., 0. };
		double   Thick = 0.;             // the hot core, as SetBeam's thick
		double   Soft = 0.;              // how far the halo reaches, as SetBeam's soft
		PalEntry Color = {};
		double   Intensity = 0.;
		double   PrevIntensity = 0.;
		// The look. Defaults are the beam system's own declared defaults
		// (BeamAirGlow, BeamGlow, BeamTaper, BeamFlare, BeamScrollSpeed) except
		// scroll depth: 0, a smooth line -- the scroll is the beading RS_Lance
		// switched off.
		double   AirGlow = 1.0;
		double   Halo = 0.35;
		double   Taper = 0.35;
		double   Flare = 1.5;
		double   ScrollSpeed = 6.0;
		double   ScrollDepth = 0.0;
		// [F1] Two OPT-IN looks over the one above ("Engine docs/FLAME_ENGINE_PLAN.md"
		// F1), off until SetDrawnLineGradient / SetDrawnLineTurbulence. Off, the
		// renderer sends look flags 0 and drawnlines.vp/.fp run exactly the code they
		// ran before these existed (hw_drawinfo.cpp, WriteDrawnLineLooks).
		// GRADIENT: the colour runs from Color at the start to ColorEnd at the end,
		// and the halo's reach grows toward the end by Swell (1 none, below 1 it
		// narrows). A gradient to the line's own colour with Swell 1 is no gradient.
		bool     HasGradient = false;
		PalEntry ColorEnd = {};
		double   Swell = 1.0;
		// TURBULENCE: world-space value noise, rising, pushes the glow's edge in and
		// out and its brightness up and down, so the edges lick. Strength 0 is off.
		double   TurbulenceStrength = 0.0;
		double   TurbulenceScale = 0.05;     // noise cells per map unit
		double   TurbulenceSpeed = 1.5;      // noise cells per second, rising
		int      Anchor = 0;             // as BeamAnchor: 0 world, 1 main hand, 2 off hand
		// Whose hand. -1 is the console player -- BeamAnchor's rule, which in
		// netplay puts everyone's line at each viewer's own hand. A player number
		// rather than an actor pointer, so nothing can dangle.
		int      AnchorPlayer = -1;
		bool     Live = false;           // written by SetDrawnLine since the last clear
		bool     PrevLive = false;       // ...as of the last tic snapshot
	};

	TArray<DrawnLine> DrawnLines;        // empty until the first write this process
	int DrawnLineHigh = 0;               // one past the highest slot written; loops stop here

	// Sized on first use from the latched capacity, kept allocated across maps.
	void EnsureDrawnLines()
	{
		if (DrawnLines.Size() > 0) return;
		extern int DrawnLineCapacity();   // hw_cvars.cpp, a fixed engine number
		DrawnLines.Resize((unsigned)DrawnLineCapacity());
		for (auto &l : DrawnLines) l = DrawnLine();
	}

	DrawnLine *DrawnLineForWrite(int index)
	{
		if (index < 0) return nullptr;
		EnsureDrawnLines();
		if ((unsigned)index >= DrawnLines.Size()) return nullptr;
		if (index >= DrawnLineHigh) DrawnLineHigh = index + 1;
		return &DrawnLines[index];
	}

	void SetDrawnLine(int index, const DVector3 &start, const DVector3 &end, PalEntry col, double intensity, double thick, double soft)
	{
		DrawnLine *l = DrawnLineForWrite(index);
		if (l == nullptr) return;
		if (!l->Live) l->PrevIntensity = 0.0;   // a line coming on has no history to lerp from
		l->Start = start;
		l->End = end;
		l->Color = col;
		l->Intensity = intensity;
		l->Thick = thick;
		l->Soft = soft;
		l->Live = true;
	}

	void SetDrawnLineLook(int index, double airGlow, double halo, double taper, double flare, double scrollSpeed, double scrollDepth)
	{
		DrawnLine *l = DrawnLineForWrite(index);
		if (l == nullptr) return;
		l->AirGlow = airGlow;
		l->Halo = halo;
		l->Taper = taper;
		l->Flare = flare;
		l->ScrollSpeed = scrollSpeed;
		l->ScrollDepth = scrollDepth;
	}

	// [F1] The two opt-in looks (see DrawnLine). Setters only: nothing hands a
	// line's look back to script, so gameplay code cannot branch on it.
	// Swell 0..16 is the halo reach at the far end over the start's. The line's
	// own colour with swell 1 is no gradient, and the renderer sends it as off.
	void SetDrawnLineGradient(int index, PalEntry colorEnd, double swell)
	{
		DrawnLine *l = DrawnLineForWrite(index);
		if (l == nullptr) return;
		l->HasGradient = true;
		l->ColorEnd = colorEnd;
		l->Swell = clamp(swell, 0.0, 16.0);
	}

	// Strength 0..2 (0 off; 1 moves an edge by up to its own distance from the
	// line), scale 0..1 noise cells per map unit, speed -100..100 cells per
	// second of level time (positive rises).
	void SetDrawnLineTurbulence(int index, double strength, double scale, double speed)
	{
		DrawnLine *l = DrawnLineForWrite(index);
		if (l == nullptr) return;
		l->TurbulenceStrength = clamp(strength, 0.0, 2.0);
		l->TurbulenceScale = clamp(scale, 0.0, 1.0);
		l->TurbulenceSpeed = clamp(speed, -100.0, 100.0);
	}

	void SetDrawnLineAnchor(int index, int mode, int playerNum = -1)
	{
		DrawnLine *l = DrawnLineForWrite(index);
		if (l == nullptr) return;
		l->Anchor = (mode < 0 || mode > 2) ? 0 : mode;
		l->AnchorPlayer = (playerNum >= 0 && playerNum < MAXPLAYERS) ? playerNum : -1;
	}

	// Clearing FORGETS the slot, look and anchor included, for the reason a dark
	// beam slot forgets its anchor: slots are reused, and the next writer should
	// not inherit a hand or a look it never asked for.
	void ClearDrawnLine(int index)
	{
		if (index < 0 || (unsigned)index >= DrawnLines.Size()) return;
		DrawnLines[index] = DrawnLine();
	}

	// Also the map-change and savegame-load reset (ClearLevelData).
	void ClearDrawnLines()
	{
		for (int i = 0; i < DrawnLineHigh; i++) DrawnLines[i] = DrawnLine();
		DrawnLineHigh = 0;
	}

	// P_Ticker, beside the beam snapshot and for the same reason: script writes
	// at 35Hz, the renderer interpolates between this and the next write.
	void SnapshotDrawnLines()
	{
		for (int i = 0; i < DrawnLineHigh; i++)
		{
			DrawnLine &l = DrawnLines[i];
			if (l.Live)
			{
				l.PrevStart = l.Start;
				l.PrevEnd = l.End;
				l.PrevIntensity = l.Intensity;
			}
			else
			{
				l.PrevIntensity = 0.0;
			}
			l.PrevLive = l.Live;
		}
	}

	// [BB] GLOW WAVE -- the missing axis.
	//
	// A glow already varies per pixel VERTICALLY: the fragment's distance
	// from the plane is what makes coverage and falloff smooth up a wall.
	// Horizontally it could not vary at all, because reach arrives as one
	// number for the whole surface -- so a wall faded beautifully top to
	// bottom and had a dead straight top edge from one end to the other.
	//
	// This is a world-space wave that modulates that reach per fragment, so
	// the edge itself rises and falls along the surface. It can drive the
	// brightness and the two-colour boundary as well, which are the same
	// wave read into different terms and look nothing like each other.
	//
	// Measured with the SAME five shapes as the sweep, from its own origin,
	// so a wave along the floor and a band crossing the room can be given
	// the same shape and made to agree. Two systems that measure the world
	// differently can never be lined up; two that share one distance
	// function line up by construction.
	//
	// Scene-global, so unlike the sweep this does not ride StreamData -- it
	// goes in the viewpoint block, which is written a handful of times a
	// frame rather than once per draw. MAX_STREAM_DATA stays at 34.
	double GlowWaveLength = 0;      // world units per cycle; 0 = off
	double GlowWaveSpeed = 0;       // radians per second
	double GlowWaveSharp = 1;       // pow() on the crest; 1 = plain sine
	int    GlowWaveShape = 1;       // same vocabulary as SweepMode
	double GlowWaveReach = 0;       // how far the edge moves, 0-1
	double GlowWaveBright = 0;      // how much the brightness swings, 0-1
	double GlowWaveColour = 0;      // how far the near/far boundary slides
	double GlowWaveDetune = 0;      // second sine, offset from the first
	double GlowWaveSeed = 0;        // per-room phase scatter, 0 = all as one
	double GlowWavePhase[4] = {};   // wall top, wall bottom, floor, ceiling
	DVector3 GlowWaveOrigin;

	// [BB] DARKNESS, PER FRAGMENT.
	//
	// The same four curves a mod would otherwise run per sector per tic,
	// handed to the shader to run against each fragment's own light. Mode 0
	// is off and costs one compare.
	//
	// Adjust is the curve's input, pre-multiplied by the caller (32 x a 0-8
	// dial, in the original) so the shader never has to know what a "preset"
	// is. MinLight floors the result and PostGain lifts it, both after the
	// curve, exactly where they were.
	int    DarkMode = 0;            // 0 off, 1 subtract, 2 compress,
	                                // 3 cap brightest, 4 deepen shadows
	double DarkAdjust = 0;
	double DarkMinLight = 0;
	double DarkPreGain = 0;
	double DarkPostGain = 0;

	// The two a sector cannot express.
	double DarkDistDepth = 0;       // how much darker at DistRange away
	double DarkDistRange = 2048;
	double DarkHeightDepth = 0;     // how much darker below HeightRef
	double DarkHeightRef = 0;       // world Z the pooling starts from
	double DarkHeightRange = 256;

	// [round2 B4] WHERE DarkHeightRef COMES FROM (SetDarknessHeightFollow).
	//   0 absolute: DarkHeightRef as the caller wrote it, as before
	//   1 the viewer's feet: the camera's interpolated Z at draw rate, plus
	//     DarkHeightOffset. A mod writing pmo.pos.z from WorldTick holds each
	//     value for 2-3 frames, so the pool edge stepped on lifts and stairs.
	// ClearDarkness and ClearLevelData set it back to 0.
	int    DarkHeightFollow = 0;
	double DarkHeightOffset = 0;

	// [BB] HOW MUCH OF THE DARKNESS ACTORS ARE SPARED, 0 to 1.
	//
	// The darkness pass takes the whole scene down together, which takes the
	// monsters with it -- correct for a wall and wrong for the thing walking
	// towards you, which simply stops existing. 0 is the old behaviour, 1
	// leaves actors at full brightness, and the useful settings are between.
	//
	// Per level rather than per actor: it is a property of how dark the ROOM
	// has been made, not of any one thing standing in it.
	double DarkActorExempt = 0;

	// [BB] FOG SLAB -- fog with a TOP.
	//
	// Sector fog is a distance tint on surfaces: the further a wall is, the
	// more it blends toward the fog colour. Nothing is simulated in the air,
	// which is why it has no shape -- no ceiling, no thickness you can stand
	// in, and no way to be brighter where a light passes through it.
	//
	// This is a horizontal slab of participating medium with a world-space
	// top. It is solved ANALYTICALLY rather than raymarched: for a flat-topped
	// slab the answer is closed-form -- work out how much of the eye-to-pixel
	// ray passed below the ceiling and fog by that length. No marching, no
	// loop, exact.
	//
	// SCATTER is what makes it more than coloured haze. The flashlight cone is
	// already described to the renderer (VolBeam* above), so fog inside that
	// cone can be brightened without any extra tracing -- the mist lights up
	// where the torch sweeps it.
	//
	// WAKE is a single point that lags behind the player on a spring. Inside
	// its radius the slab is thinned and its top is disturbed, so walking
	// leaves a trail that settles. One point rather than a history buffer,
	// because a trail that fades IS a point that follows you slowly.
	bool     FogSlabActive = false;
	double   FogSlabTop = 0;        // world Z of the mist's surface
	// The layer's BOTTOM. Far below any map by default, which is a half-space
	// and the old behaviour. Raise it for ceiling fog or a floating band.
	double   FogSlabBottom = -32768;
	// VERTICAL HOLD. With a period set the layer repeats up the room and the
	// whole stack rolls -- the old television fault. One mod() from the single
	// layer case, and it costs the same, because a repeating thing is
	// arithmetic rather than a loop.
	double   FogSlabPeriod = 0;
	double   FogSlabRoll = 0;

	// [BB] A tornado -- the same fog, shaped into a funnel you can stand in.
	// Density 0 is off, and it is tested first: this is the most expensive
	// thing in the fragment shader, because unlike a knee-high layer it does
	// not early out for most of the screen when you are looking at one.
	DVector2 TornadoPos;
	double   TornadoBase = 0;
	double   TornadoTop = 512;
	double   TornadoRadBase = 48;
	double   TornadoRadTop = 320;
	double   TornadoDensity = 0;
	double   TornadoSwirl = 0.5;
	double   TornadoSpin = 2.0;
	double   TornadoTwist = 8.0;
	double   TornadoLean = 0;
	double   TornadoLeanPeriod = 6.0;
	// Its own colour and its own torch response, so a red funnel can stand in
	// blue ground mist without either being a tint of the other.
	PalEntry TornadoColor = 0xff8c99b3;
	double   TornadoScatter = 1.2;

	// [BB] DISTURBANCES. One primitive, five effects -- see the note beside
	// mFogDisturbA in hw_viewpointuniforms.h. A ring buffer rather than an
	// allocation: a disturbance is a short-lived event, and the oldest slot is
	// always the right one to reuse when the array is already full.
	//
	// RAISED FROM 8 TO 32, deliberately not matched to MAX_SHAPES/MAX_BEAMS'
	// 128: both loops that read this array (main.fp, the glow feed and the
	// density calc) run over every fragment inside the fog volume, which on a
	// screen-filling bank of mist is a much bigger multiplier per slot than a
	// sparse decal or a segment test. 32 clears a busy firefight's worth of
	// simultaneous gunfire, deaths and explosions without quietly turning the
	// fog pass into a 16x-heavier one for slots that are usually empty.
	static const int MAX_FOG_DISTURB = 32;
	DVector3 FogDisturbPos[MAX_FOG_DISTURB];
	double   FogDisturbRadius[MAX_FOG_DISTURB] = {};
	double   FogDisturbBirth[MAX_FOG_DISTURB] = {};   // level time in seconds
	double   FogDisturbLife[MAX_FOG_DISTURB] = {};    // 0 = slot is free
	double   FogDisturbStrength[MAX_FOG_DISTURB] = {};
	double   FogDisturbSpeed[MAX_FOG_DISTURB] = {};
	int      FogDisturbMode[MAX_FOG_DISTURB] = {};
	// NOT a ring cursor, whatever it was meant to be: FogDisturb takes the
	// first free slot or the oldest, and never reads this. Only
	// ClearFogDisturb writes it. Left in place as inert state.
	int      FogDisturbNext = 0;

	double   FogNoiseScale = 0.004;
	double   FogNoiseDepth = 0;      // 0 = uniform density, as before
	DVector2 FogNoiseDrift;

	double   FogTendrilSpacing = 96;
	double   FogTendrilRadius = 10;
	double   FogTendrilHeight = 96;
	double   FogTendrilDensity = 0;  // 0 = off
	double   FogTendrilRise = 0.6;
	double   FogTendrilSpread = 1.0;
	double   FogTendrilLean = 6.0;
	double   FogTendrilTaper = 1.6;

	DVector2 FogWakeVel;
	double   FogWakeStretch = 0;     // 0 = a plain disc, as before

	double   FogBowStrength = 0;     // 0 = the sweep does not touch the mist
	double   FogBowWidth = 64;
	double   FogBowThin = 0.6;

	// Which reference each fog edge follows, and how gently. 0 is absolute
	// world Z; positive follows the FLOOR by that fraction, negative follows
	// the CEILING. The magnitude is what turns a staircase into a slope.
	// [RS fork] The edge sits at its value PLUS the full floor (ceiling)
	// height; the magnitude blends the eye's floor toward each fragment's
	// floor. It used to scale absolute floor Z. See FogSlabAt in main.fp.
	double   FogFollowTop = 0;
	double   FogFollowBottom = 0;

	// [BB] FOG BY ROOM TYPE. A sky ceiling is outdoors and anything else is
	// indoors -- the marker every Doom map already carries, so this needs no new
	// mapping work and no new flag. Both default to 1, so a level that never asks
	// is fogged exactly as it was.
	double   FogIndoorScale = 1.0;
	double   FogOutdoorScale = 1.0;

	// [BB] What survives the colour drain. Threshold 0 = the old all-or-nothing
	// behaviour, exactly.
	double   DesatKeep = 0;
	double   DesatKeepSoft = 0.15;
	int      DesatKeepHue = 0;   // 0 any, 1 red, 2 green, 3 blue

	// [BB] THE DRAIN, as a scene-global rather than per sector.
	//
	// DesatKeep above decides what SURVIVES desaturation. This is the amount
	// of desaturation there is to survive, and until now the only way to set
	// it was the sector's own colormap byte -- which meant a mod wanting a
	// monochrome world had to walk every sector in the map and mutate it,
	// exactly the way DarkDoomZ had to walk every sector to darken one.
	//
	// Same fix as SetDarkness: one number for the frame, applied per fragment.
	// max()'d against the per-sector factor in the shader rather than
	// replacing it, so a sector a mapper deliberately drained harder stays
	// drained harder.
	double   DesatGlobal = 0;    // 0..1

	PalEntry FogColor2 = 0xffb38059;
	double   FogColor2Mix = 0;       // 0 = one colour, as before

	// [round2 B3] A TRANSIENT GRADIENT TINT THAT WINS OVER THE STANDING ONE
	// (SetFogGradientOverride / ClearFogGradientOverride), on the fog slab
	// override's pattern. A fog mod re-pushes SetFogGradient every tic, so a
	// passing caller's tint (a sweep's) written there never reached a frame.
	// While active the renderer uploads these instead; FogColor2/FogColor2Mix are
	// never touched, so clearing hands the view straight back. One override,
	// not a stack. ClearLevelData ends it.
	bool     FogColor2OverrideActive = false;
	PalEntry FogColor2OverrideColor = 0;
	double   FogColor2OverrideMix = 0;

	// [RS fork] The colour an IGNITE disturbance burns (SetFogIgniteColor).
	// Ignite used FogColor2, a colour chosen for the top of the layer and left
	// black by most presets, so explosions added black light. Unset keeps
	// that old behaviour. Uploaded packed in mFogWake2.w.
	PalEntry FogIgniteColor = 0;
	bool     FogIgniteColorSet = false;

	// [BB] Texture inside the glow -- see GlowTextureAt in main.fp. The wave
	// varies a glow's EDGE and has nothing to say once coverage saturates;
	// these happen within the lit area instead. All off at 0.
	double   GlowTexNoise = 0;
	double   GlowTexScale = 0.02;
	double   GlowTexDrift = 1.0;
	double   GlowTexContrast = 1.0;
	double   GlowFlow = 0;
	double   GlowFlowSpacing = 64;
	double   GlowFlowSpeed = 0.4;
	double   GlowFlowSharp = 2.0;
	double   GlowCell = 0;
	double   GlowCellScale = 96;
	double   GlowCellSpeed = 1.2;
	double   GlowCellWidth = 0.08;
	double   GlowReact = 0;      // the walls take the disturbance array too
	double   GlowPulse = 0;      // depth of the state pulse
	double   GlowPulseLevel = 0; // and how alarmed the room currently is
	// [RS fork] HOW FAST THE THROB BEATS, a multiplier on the rate the level
	// implies (main.fp: rate = 1 + 6*level). The level set depth AND speed, so
	// a bright alarm was always a fast one and RS_GlowInTheDark's Red Alert
	// strobed. 1 is the old rate exactly. Uploaded in mGlowTex4.w.
	double   GlowPulseRate = 1.0;

	// [BB] THE HEATMAP.
	//
	// Where the fighting happened, drawn on the floor and accumulated over the
	// whole life of a map. This is emphatically NOT the disturbance array: a
	// disturbance is a handful of short-lived events and lives in uniforms, and
	// a heatmap is hundreds of permanent deposits that have to be summed. Eight
	// slots cannot express it and neither can eighty.
	//
	// So it is a coarse GRID over the map's own extent, stamped on the CPU when
	// something dies and sampled per fragment. A grid is the right shape for
	// this because the question a heatmap answers -- "how much happened near
	// here" -- is a spatial sum, and a sum wants a bucket, not a list. Adding
	// the thousandth death costs exactly what the first one cost.
	//
	// Resolution is fixed rather than exposed: 256 squared over a map's bounding
	// box is a handful of world units per cell on anything Doom-sized, the
	// texture is a megabyte, and the eye cannot use more from something this
	// deliberately blurry.
	static const int HEAT_RES = 256;
	TArray<float> HeatIntensity;    // HEAT_RES * HEAT_RES, accumulated
	TArray<float> HeatHeight;       // world Z of the deposits in that cell
	bool     HeatDirty = false;     // needs re-uploading to the GPU
	bool     HeatEverUsed = false;  // nothing allocated until first asked for

	double   HeatScale = 0;         // 0 = off
	double   HeatDecay = 0;         // units of intensity lost per second
	double   HeatTolerance = 96;    // how far off in Z before a floor is
	                                // considered a different storey
	PalEntry HeatColorLow = 0xff2040ff;
	PalEntry HeatColorHigh = 0xffff2000;
	double   HeatCeiling = 8.0;     // intensity that maps to the high colour
	double   FogSlabDensity = 0;    // per 1000 units of travel below the top
	double   FogSlabSoft = 24;      // how many units the top edge fades over
	double   FogSlabScatter = 0;    // 0 = flat haze, 1 = torch lights it
	PalEntry FogSlabColor = 0xFF3018;
	double   FogSlabWakeStrength = 0;
	DVector3 FogSlabWakePos;
	double   FogSlabWakeRadius = 0;
	// How much of the surface behind it the mist takes on. Without this the
	// slab is a flat colour laid over the scene and reads as a filter rather
	// than a substance -- mist in front of a red glowing wall should be red.
	double   FogSlabPickup = 0;
	// The surface itself, animated. Amplitude 0 leaves the top flat.
	double   FogSurfAmp = 0;
	double   FogSurfLen = 256;
	double   FogSurfSpeed = 1.0;
	double   FogSurfCross = 0.6;

	// [RS fork] A TRANSIENT SLAB THAT OVERRIDES THE STANDING ONE
	// (SetFogSlabOverride / ClearFogSlabOverride).
	//
	// One slab slot and two kinds of caller made them fight: RS_Fog re-pushes
	// its standing fog every tic and replaced the weapon wheel's mist, and the
	// wheel's ClearFogSlab wiped RS_Fog's fog when it closed. A caller whose
	// mist is temporary sets this instead. While it is active the renderer
	// draws it in place of the FogSlab* values above, which stay untouched, so
	// clearing it brings the standing fog straight back. Self-contained and in
	// absolute world Z: its own bottom, no stack, no swell, no follow. Wake and
	// pickup are still the standing slab's. Density <= 0 clears it.
	bool     FogSlabOverrideActive = false;
	double   FogSlabOverrideTop = 0;
	double   FogSlabOverrideDensity = 0;
	double   FogSlabOverrideSoft = 24;
	double   FogSlabOverrideScatter = 0;
	PalEntry FogSlabOverrideColor = 0xFF3018;
	double   FogSlabOverrideBottom = -32768;

	// links to global game objects
	TArray<DBehavior*> ActorBehaviors, ClientSideActorBehaviors;
	TArray<TObjPtr<AActor *>> CorpseQueue;
	TObjPtr<DFraggleThinker *> FraggleScriptThinker = MakeObjPtr<DFraggleThinker*>(nullptr);
	TObjPtr<DACSThinker*> ACSThinker = MakeObjPtr<DACSThinker*>(nullptr);
	TObjPtr<DACSThinker*> ClientSideACSThinker = MakeObjPtr<DACSThinker*>(nullptr);

	TObjPtr<DSpotState *> SpotState = MakeObjPtr<DSpotState*>(nullptr);

	//==========================================================================
	//
	//
	//==========================================================================

	void AddActorBehavior(DBehavior& b)
	{
		if (b.Level == nullptr)
		{
			b.Level = this;
			if (b.IsClientSide())
				ClientSideActorBehaviors.Push(&b);
			else
				ActorBehaviors.Push(&b);
		}
	}

	void RemoveActorBehavior(DBehavior& b)
	{
		if (b.Level == this)
		{
			b.Level = nullptr;
			if (b.IsClientSide())
				ClientSideActorBehaviors.Delete(ClientSideActorBehaviors.Find(&b));
			else
				ActorBehaviors.Delete(ActorBehaviors.Find(&b));
		}
	}

	//==========================================================================
	//
	//
	//==========================================================================

	bool IsJumpingAllowed() const
	{
		if (dmflags & DF_NO_JUMP)
			return false;
		if (dmflags & DF_YES_JUMP)
			return true;
		return !(flags & LEVEL_JUMP_NO);
	}

	//==========================================================================
	//
	//
	//==========================================================================

	bool IsCrouchingAllowed() const
	{
		if (dmflags & DF_NO_CROUCH)
			return false;
		if (dmflags & DF_YES_CROUCH)
			return true;
		return !(flags & LEVEL_CROUCH_NO);
	}

	//==========================================================================
	//
	//
	//==========================================================================

	bool IsFreelookAllowed() const
	{
		if (dmflags & DF_NO_FREELOOK)
			return false;
		if (dmflags & DF_YES_FREELOOK)
			return true;
		return !(flags & LEVEL_FREELOOK_NO);
	}

	bool MissileShouldClip() const
	{
		return (i_compatflags & COMPATF_MISSILECLIP) ||
			(sv_autocompat && (gameinfo.gametype & GAME_DoomChex) && maptype == MAPTYPE_DOOM);
	}

	node_t		*HeadNode() const
	{
		return nodes.Size() == 0 ? nullptr : &nodes[nodes.Size() - 1];
	}
	node_t		*HeadGamenode() const
	{
		return headgamenode;
	}

	// Returns true if level is loaded from saved game or is being revisited as a part of a hub
	bool		IsReentering() const
	{
		return savegamerestore
			|| (info != nullptr && info->Snapshot.mBuffer != nullptr && info->isValid());
	}
};


extern FLevelLocals level;
extern FLevelLocals *primaryLevel;	// level for which to display the user interface. This will always be the one the current consoleplayer is in.
extern FLevelLocals *currentVMLevel;

inline FSectorPortal *line_t::GetTransferredPortal()
{
	auto Level = GetLevel();
	return portaltransferred >= Level->sectorPortals.Size() ? (FSectorPortal*)nullptr : &Level->sectorPortals[portaltransferred];
}

inline FSectorPortal *sector_t::GetPortal(int plane)
{
	return &Level->sectorPortals[Portals[plane]];
}

inline double sector_t::GetPortalPlaneZ(int plane)
{
	return Level->sectorPortals[Portals[plane]].mPlaneZ;
}

inline DVector2 sector_t::GetPortalDisplacement(int plane)
{
	return Level->sectorPortals[Portals[plane]].mDisplacement;
}

inline int sector_t::GetPortalType(int plane)
{
	return Level->sectorPortals[Portals[plane]].mType;
}

inline int sector_t::GetOppositePortalGroup(int plane)
{
	return Level->sectorPortals[Portals[plane]].mDestination->PortalGroup;
}

inline bool sector_t::PortalBlocksView(int plane)
{
	if (GetPortalType(plane) != PORTS_LINKEDPORTAL) return false;
	return !!(planes[plane].Flags & (PLANEF_NORENDER | PLANEF_DISABLED | PLANEF_OBSTRUCTED));
}

inline bool sector_t::PortalBlocksSight(int plane)
{
	return PLANEF_LINKED != (planes[plane].Flags & (PLANEF_NORENDER | PLANEF_NOPASS | PLANEF_DISABLED | PLANEF_OBSTRUCTED | PLANEF_LINKED));
}

inline bool sector_t::PortalBlocksMovement(int plane)
{
	return PLANEF_LINKED != (planes[plane].Flags & (PLANEF_NOPASS | PLANEF_DISABLED | PLANEF_OBSTRUCTED | PLANEF_LINKED));
}

inline bool sector_t::PortalBlocksSound(int plane)
{
	return PLANEF_LINKED != (planes[plane].Flags & (PLANEF_BLOCKSOUND | PLANEF_DISABLED | PLANEF_OBSTRUCTED | PLANEF_LINKED));
}

inline bool sector_t::PortalIsLinked(int plane)
{
	return (GetPortalType(plane) == PORTS_LINKEDPORTAL);
}

inline FLevelLocals *line_t::GetLevel() const
{
	return frontsector->Level;
}
inline FLinePortal *line_t::getPortal() const
{
	return portalindex == UINT_MAX && portalindex >= GetLevel()->linePortals.Size() ? (FLinePortal*)nullptr : &GetLevel()->linePortals[portalindex];
}

// returns true if the portal is crossable by actors
inline bool line_t::isLinePortal() const
{
	return portalindex == UINT_MAX && portalindex >= GetLevel()->linePortals.Size() ? false : !!(GetLevel()->linePortals[portalindex].mFlags & PORTF_PASSABLE);
}

// returns true if the portal needs to be handled by the renderer
inline bool line_t::isVisualPortal() const
{
	return portalindex == UINT_MAX && portalindex >= GetLevel()->linePortals.Size() ? false : !!(GetLevel()->linePortals[portalindex].mFlags & PORTF_VISIBLE);
}

inline line_t *line_t::getPortalDestination() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? (line_t*)nullptr : GetLevel()->linePortals[portalindex].mDestination;
}

inline int line_t::getPortalFlags() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? 0 : GetLevel()->linePortals[portalindex].mFlags;
}

inline int line_t::getPortalAlignment() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? 0 : GetLevel()->linePortals[portalindex].mAlign;
}

inline int line_t::getPortalType() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? 0 : GetLevel()->linePortals[portalindex].mType;
}

inline DVector2 line_t::getPortalDisplacement() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? DVector2(0., 0.) : GetLevel()->linePortals[portalindex].mDisplacement;
}

inline DAngle line_t::getPortalAngleDiff() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? DAngle::fromDeg(0.) : GetLevel()->linePortals[portalindex].mAngleDiff;
}

inline bool line_t::hitSkyWall(AActor* mo) const
{
	return backsector &&
		backsector->GetTexture(sector_t::ceiling) == skyflatnum &&
		mo->Z() >= backsector->ceilingplane.ZatPoint(mo->PosRelative(this).XY());
}

// This must later be extended to return an array with all levels.
// It is meant for code that needs to iterate over all levels to make some global changes, e.g. configuation CCMDs.
inline TArrayView<FLevelLocals *> AllLevels()
{
	return TArrayView<FLevelLocals *>(&primaryLevel, 1);
}

ELightMode getRealLightmode(FLevelLocals* Level, bool for3d);
