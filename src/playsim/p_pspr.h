/*
** p_pspr.h
**
** Sprite animation.
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 id Software
** Copyright 1994-1996 Raven Software
** Copyright 1999-2016 Marisa Heit
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#ifndef __P_PSPR_H__
#define __P_PSPR_H__

#include "renderstyle.h"
#include "palettecontainer.h"
#include "common/utility/TSQueue.h"

// Basic data types.
// Needs fixed point, and BAM angles.

#define WEAPONBOTTOM			128.

#define WEAPONTOP				32.
#define WEAPON_FUDGE_Y			0.375
struct FTranslatedLineTarget;
struct FState;
class player_t;

//
// Overlay psprites are scaled shapes
// drawn directly on the view screen,
// coordinates are given for a 320*200 view screen.
//
enum PSPLayers
{
	PSP_STRIFEHANDS = -1,
	PSP_CALLERID = 0,
	PSP_WEAPON = 1,
	PSP_FLASH = 1000,
	PSP_OFFHANDWEAPON = 1000000,
	PSP_TARGETCENTER = INT_MAX - 2,
	PSP_TARGETLEFT,
	PSP_TARGETRIGHT,
};

enum PSPFlags
{
	PSPF_ADDWEAPON		= 1 << 0,
	PSPF_ADDBOB			= 1 << 1,
	PSPF_POWDOUBLE		= 1 << 2,
	PSPF_CVARFAST		= 1 << 3,
	PSPF_ALPHA			= 1 << 4,
	PSPF_RENDERSTYLE	= 1 << 5,
	PSPF_FLIP			= 1 << 6,
	PSPF_FORCEALPHA		= 1 << 7,
	PSPF_FORCESTYLE		= 1 << 8,
	PSPF_MIRROR			= 1 << 9,
	PSPF_PLAYERTRANSLATED = 1 << 10,
	PSPF_PIVOTPERCENT	= 1 << 11,
	PSPF_INTERPOLATE	= 1 << 12,
};

enum PSPAlign
{
	PSPA_TOP = 0,
	PSPA_CENTER,
	PSPA_BOTTOM,
	PSPA_LEFT = PSPA_TOP,
	PSPA_RIGHT = 2
};

enum EPSPBobType
{
	PSPB_None,
	PSPB_2D,
	PSPB_3D,
};

struct WeaponInterp
{
	FVector2 v[4];
};

struct FPlayerBob
{
	struct FWeaponBobInfo {
		int Tic2D = -1;
		FVector2 Bob2D = {};

		int Tic3D = -1;
		FVector3 Translation = {};
		FVector3 Rotation = {};

		void Clear2D()
		{
			Tic2D = -1;
			Bob2D = {};
		}

		void Clear3D()
		{
			Tic3D = -1;
			Translation = Rotation = {};
		}

		void Clear()
		{
			Clear2D();
			Clear3D();
		}
	};

	FWeaponBobInfo BobInfo = {}, PrevBobInfo = {};

	void SetBob2D(int tic, FVector2 bob)
	{
		BobInfo.Tic2D = tic;
		BobInfo.Bob2D = bob;
		if (PrevBobInfo.Tic2D < 0 || BobInfo.Tic2D - PrevBobInfo.Tic2D != 1)
			PrevBobInfo = BobInfo;
	}

	void SetBob3D(int tic, const FVector3& trans, const FVector3& rot)
	{
		BobInfo.Tic3D = tic;
		BobInfo.Translation = trans;
		BobInfo.Rotation = rot;
		if (PrevBobInfo.Tic3D < 0 || BobInfo.Tic3D - PrevBobInfo.Tic3D != 1)
			PrevBobInfo = BobInfo;
	}

	void UpdateInterpolation(bool sprite)
	{
		if (sprite)
			BobInfo.Clear3D();
		else
			BobInfo.Clear2D();

		PrevBobInfo = BobInfo;
	}

	void ResetInterpolation()
	{
		BobInfo.Clear();
		PrevBobInfo.Clear();
	}

	FVector2 Interpolate2D(double ticFrac) const
	{
		return PrevBobInfo.Bob2D * (1.0 - ticFrac) + BobInfo.Bob2D * ticFrac;
	}

	void Interpolate3D(FVector3& t, FVector3& r, double ticFrac) const
	{
		t = PrevBobInfo.Translation * (1.0 - ticFrac) + BobInfo.Translation * ticFrac;
		r = PrevBobInfo.Rotation * (1.0 - ticFrac) + BobInfo.Rotation * ticFrac;
	}
};

class DPSprite : public DObject
{
	DECLARE_CLASS (DPSprite, DObject)
	HAS_OBJECT_POINTERS
public:
	DPSprite(player_t *owner, AActor *caller, int id);

	static void NewTick();
	void SetState(FState *newstate, bool pending = false);

	int			GetID()							const { return ID; }
	int			GetSprite()						const { return Sprite; }
	int			GetFrame()						const { return Frame; }
	int			GetTics()						const {	return Tics; }
	FTranslationID	GetTranslation()					  { return Translation; }
	FState*		GetState()						const { return State; }
	DPSprite*	GetNext()							  { return Next; }
	AActor*		GetCaller()							  { return Caller; }
	void		SetCaller(AActor *newcaller)		  { Caller = newcaller; }
	// RS fork -- ShiftSurfacePositions() is the per-tic half of display-rate
	// part motion; see SurfOvPos below. NewTick() calls this on every psprite
	// once per tic, before any script has run, which is exactly the moment
	// "where it was" has to be captured.
	void		ResetInterpolation()				  { oldx = x; oldy = y; Prev = Vert; InterpolateTic = false; ShiftSurfacePositions(); }
	void OnDestroy() override;
	std::pair<FRenderStyle, float> GetRenderStyle(FRenderStyle ownerstyle, double owneralpha);
	float GetYAdjust(bool fullscreen);

	int HAlign, VAlign;		// Horizontal and vertical alignment
	DVector2 baseScale;		// Base scale (set by weapon); defaults to (1.0, 1.2) since that's Doom's native aspect ratio
	DAngle rotation;		// How much rotation to apply.
	DVector2 pivot;			// pivot points
	DVector2 scale;			// Dynamic scale (set by A_Overlay functions)
	double x, y, alpha;
	double oldx, oldy;
	bool InterpolateTic;	// One tic interpolation (WOF_INTERPOLATE)
	DVector2 Coord[4];		// Offsets
	WeaponInterp Prev;		// Interpolation
	WeaponInterp Vert;		// Current Position
	bool firstTic;
	int Tics;
	FTranslationID Translation;
	RingBuffer<int, 5> LastPatches;
	FTextureID LastPatch;
	int Flags;
	FRenderStyle Renderstyle;

	// RS FORK -- PER-PSPRITE MODEL TINT. Added 2026-08-08.
	// Tint multiplies the weapon's own texture; Glow is added on top.
	// Both apply to a 3D HUD MODEL, not just a sprite: hw_weapon.cpp
	// sets them on the draw state immediately before RenderHUDModel,
	// and neither hw_models.cpp nor modelrenderer.h touches object or
	// add colour, so the model inherits them.
	//
	// These live on the PSprite rather than the weapon actor so each
	// hand tints independently -- mainhand and offhand are separate
	// PSprite layers (PSP_WEAPON / PSP_OFFHANDWEAPON).
	//
	// Stock GZDoom could not do this at all: the only colour input was
	// the PLAYER's fillcolor, gated behind STYLEF_ColorIsFixed, which
	// forces a stencil style and flattens the model to a silhouette.
	// IN-CLASS INITIALISERS, NOT CONSTRUCTOR-BODY ONES, AND THAT IS LOAD
	// BEARING. DPSprite has a second, PRIVATE default constructor used by
	// savegame deserialisation (`DPSprite () {}` below) which runs none of
	// the public constructor's body. Set only there, these stayed
	// uninitialised on every load, and the serialiser leaves them alone
	// when reading a save written before they existed -- so an old save
	// resumed with whatever garbage was in that memory. Garbage near zero
	// multiplies the weapon by black.
	PalEntry Tint = 0xffffffff;   // multiply, 0xffffffff = untinted
	PalEntry Glow = 0;            // additive, 0x00000000 = none

	// RS FORK -- DIRECT MODEL FRAME ADDRESSING.
	//
	// A HUD model's frame normally arrives through the SPRITE: psp->Frame is
	// a sprite letter index, MODELDEF's FrameIndex maps that letter to a model
	// frame, and FindModelFrame looks the pair up. That channel is one
	// character wide -- MAX_SPRITE_FRAMES is 29, inherited from Doom's
	// 8-character lump names where Boom pushed the frame char as far as ']'.
	//
	// Model meshes have no such limit. Our weapon models run to 75 frames, so
	// most of every reload and fire animation was simply unaddressable: there
	// was no letter left to name it with. Raising MAX_SPRITE_FRAMES does not
	// help -- three more ASCII characters and then lowercase, which lump names
	// case-fold away.
	//
	// So skip the encoding. When ModelFrame >= 0 the HUD model path uses it
	// verbatim instead of the sprite-derived frame, and 75 costs no more than
	// 5. FindModelFrame must still resolve (scale, offsets, skins and flags
	// all come from the FSpriteModelFrame it returns) -- only the frame NUMBER
	// is replaced.
	//
	// ModelFrameNext + ModelFrameLerp drive interpolation explicitly. Stock
	// tweening is derived from state tics and only happens across a state
	// transition, which is useless when one of OUR animations is being played
	// across THEIR state timings: it would snap between poses. Set the lerp
	// and the renderer blends ModelFrame -> ModelFrameNext by it, ignoring
	// gl_interpolate_model_frames and MDL_NOINTERPOLATION.
	//
	// IN-CLASS INITIALISERS FOR THE SAME REASON AS Tint/Glow ABOVE: the
	// private savegame constructor runs no constructor body, and a garbage
	// ModelFrame indexes a model's frame array with a random int.
	//
	// -1 on all three = inactive, stock behaviour.
	int   ModelFrame     = -1;   // model frame to show, bypassing the sprite
	int   ModelFrameNext = -1;   // frame to blend toward
	float ModelFrameLerp = -1.f; // 0..1 blend factor; <0 = use stock timing

	// RS FORK -- PER-PART FRAME ADDRESSING. The MD3 answer to a skeleton.
	//
	// ModelFrame above is ONE number applied to EVERY sub-model in the stack.
	// That is right for a donor mesh playing a single animation across its
	// parts, and wrong for a gun.
	//
	// A MODELDEF block already carries a weapon as SEPARATE models -- Rusted
	// Legacy's pistol is berreta.md3 + berreta_mag.md3 + hand.md3, three
	// stacked models each with their own FrameIndex rows. But one shared frame
	// number means the only way to show "slide back AND magazine out" is a
	// baked frame for that exact combination, so an author ends up hand-
	// authoring the cross product of every part's every position. That
	// explosion is what stopped the mod those models came from: six duplicate
	// MODELDEF blocks per weapon and a state-machine call on every frame line.
	//
	// Addressed per part, each gets its own short frame strip and script drives
	// them independently -- the slide follows the hand racking it while the
	// magazine is already gone. No bones, no rig, no IQM: the same split-mesh
	// technique Quake 3 used for head/torso/legs, reachable from ZScript. It is
	// also the only articulation MD3 can express at all, which matters because
	// the Quest engine will never have the bone API.
	//
	// PER-PART OVERRIDES THE SCALAR, FOR ITS OWN INDEX ONLY. A caller that sets
	// both gets per-part where it asked for it and the scalar everywhere else.
	// ModelSwapper sets only the scalar and is untouched by all of this.
	//
	// ModelPartHidden drops a part from the draw entirely, which is what "the
	// magazine is out of the gun" actually is. Rusted Legacy faked that by
	// pointing the magazine at a junk frame index (its MODELDEF really does say
	// `FrameIndex PISG K 1 99`) and relying on an out-of-range frame drawing
	// nothing. A real switch costs nothing and does not depend on that.
	//
	// IN-CLASS INITIALISERS, SPELLED OUT, for exactly the reason Tint/Glow and
	// ModelFrame above give: the private savegame constructor runs no
	// constructor body, and a garbage frame number indexes a model's frame
	// array with whatever happened to be in that memory.
	//
	// TWELVE is a working ceiling, not a format limit -- FSpriteModelFrame
	// holds modelsAmount in a uint8_t. A gun split into body, slide, magazine,
	// hammer, trigger and two hands is seven. Parts at or past this index fall
	// through to the scalar path rather than failing.
	static constexpr int RS_MODEL_PARTS = 12;

	int   ModelFramePart    [RS_MODEL_PARTS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
	int   ModelFrameNextPart[RS_MODEL_PARTS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
	float ModelFrameLerpPart[RS_MODEL_PARTS] =
		{ -1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f };
	bool  ModelPartHidden   [RS_MODEL_PARTS] = {};   // zero-init is false, i.e. drawn

	// Is ANY per-part field live on this layer.
	//
	// Exists so the two places that need the answer -- CalcModelFrame deciding
	// whether a blend target is required, and the render loop deciding whether
	// to consult the arrays at all -- ask one short question instead of twelve,
	// and so the whole mechanism costs a single test on every weapon that never
	// uses it. Computed rather than cached: a stored flag is one more thing to
	// keep true across a savegame, and the loop is twelve iterations.
	bool AnyModelPartActive() const
	{
		for (int i = 0; i < RS_MODEL_PARTS; i++)
		{
			if (ModelFramePart[i] >= 0 || ModelFrameLerpPart[i] >= 0.f || ModelPartHidden[i])
				return true;
		}
		return false;
	}

	// RS FORK -- PER-SURFACE FRAME ADDRESSING. One level finer than the arrays
	// above, and the level a gun actually needs.
	//
	// ModelFramePart addresses a whole sub-model of a MODELDEF stack, which is
	// right for the pieces an author shipped as their own file -- Rusted
	// Legacy's magazine and hand are separate .md3s and this reaches them. It
	// is NOT enough for the slide, because the slide is a SURFACE inside the
	// pistol's own mesh, sharing the file with the frame, hammer and trigger.
	// So is the shotgun's pump, and so is nearly every mechanical part in the
	// ModelSwapper donor library: 32 of its 33 meshes are multi-surface and
	// every one already animates those surfaces independently. The data was
	// always there; nothing could address it.
	//
	// A SPARSE LIST, NOT A GRID. Addressing (model, surface) as a rectangular
	// array would be 12 models x 64 surfaces of state on every psprite for a
	// feature most weapons never touch. A handful of slots costs nothing when
	// empty, which is the common case, and sixteen driven parts on one gun at
	// one instant is already far past anything real.
	//
	// SLOTS ARE MATCHED ON (model, surface). SurfOvModel < 0 means the slot is
	// unused; there is no ordering requirement and no need to pack them.
	//
	// BY INDEX, and that is forced by the assets rather than chosen. A third
	// of the donor library names its surfaces `Cube`, `Untitled`,
	// `pCylinder10` or `python.004`, and several models repeat a name --
	// `Sights` twice on one shotgun, `Runko` three times on one chaingun. An
	// index is always unambiguous. FModel::GetSurfaceName exists so the
	// meaningful names can still be read off when authoring the map that says
	// which index is the slide.
	static constexpr int RS_SURF_SLOTS = 16;

	int   SurfOvModel  [RS_SURF_SLOTS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
	int   SurfOvSurface[RS_SURF_SLOTS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
	int   SurfOvFrame  [RS_SURF_SLOTS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
	int   SurfOvNext   [RS_SURF_SLOTS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
	float SurfOvLerp   [RS_SURF_SLOTS] =
		{ -1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f };
	bool  SurfOvHidden [RS_SURF_SLOTS] = {};   // zero-init is false, i.e. drawn

	// Does this layer drive any surface at all. Same reason as
	// AnyModelPartActive: the render path asks once instead of sixteen times,
	// and a weapon that never uses the feature pays a single test.
	bool AnySurfaceOverride() const
	{
		for (int i = 0; i < RS_SURF_SLOTS; i++)
			if (SurfOvModel[i] >= 0) return true;
		return false;
	}

	// RS FORK -- DISPLAY-RATE PART MOTION.
	//
	// THE UNIT IS FRAMES. SurfOvPos is a fractional index into the model's own
	// frame array -- 3.5 means "halfway between mesh frame 3 and mesh frame 4".
	// It is not map units, not a 0..1 fraction of anything, not seconds. Stated
	// here because nothing downstream can check it: the compiler will accept
	// any float and the VM will accept any float, and a value handed across
	// this boundary in the wrong unit is a bug that only shows up as motion
	// that looks subtly wrong.
	//
	// WHY IT EXISTS. SurfOvFrame/Next/Lerp above are set by script, and script
	// runs at 35 Hz. The renderer draws at headset rate -- 90, 120 -- and
	// without this it draws the SAME frame pair with the SAME blend two or
	// three times in a row, so a slide being pulled steps instead of gliding.
	// Quake-1 smooth, in a headset, on a part your own hand is moving.
	//
	// So the position is smoothed the way Doom smooths everything that moves:
	// keep where it was at the end of last tic, keep where it is now, and let
	// the renderer interpolate between them by how far through the tic the
	// draw happens. ShiftSurfacePositions() does the keeping, once per tic,
	// from ResetInterpolation(); the blend is in models.cpp at draw time.
	//
	// A SINGLE CONTINUOUS POSITION rather than the frame/next/lerp triple, and
	// that is not a style choice. Interpolating the triple is wrong whenever
	// the frame numbers change between tics: going from (3 -> 4, 0.9) to
	// (4 -> 5, 0.1) means the position advanced from 3.9 to 4.1, but blending
	// the lerp alone runs it 0.9 -> 0.1, backwards through the whole frame.
	// One number has no such seam.
	//
	// NEGATIVE MEANS UNUSED, and the explicit SurfOvFrame/Next/Lerp path is
	// taken instead -- kept for a caller that wants an exact pose pinned with
	// no smoothing at all, which is what setting a slide to locked-back is.
	float SurfOvPos    [RS_SURF_SLOTS] =
		{ -1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f };
	float SurfOvPosPrev[RS_SURF_SLOTS] =
		{ -1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f };

	// A live transform on top of the frame -- the psprite half of the same
	// table DActorModelData carries. Same feature, two homes, one reader.
	// See FModelSurfaceOverride in model.h.
	bool     SurfOvHasXf[RS_SURF_SLOTS] = {};
	FVector3 SurfOvOfs  [RS_SURF_SLOTS] = {};
	FVector4 SurfOvRot  [RS_SURF_SLOTS] = {};

	// Last tic's transform, so the offset path gets the same display-rate
	// smoothing the frame path has. See DActorModelData for why the two moving
	// at different rates is worse than neither moving smoothly.
	FVector3 SurfOvOfsPrev[RS_SURF_SLOTS] = {};
	FVector4 SurfOvRotPrev[RS_SURF_SLOTS] = {};

	// Once per tic, before script runs. Cheap enough to do unconditionally:
	// sixteen float copies against the cost of tracking whether it is needed.
	void ShiftSurfacePositions()
	{
		for (int i = 0; i < RS_SURF_SLOTS; i++)
		{
			SurfOvPosPrev[i] = SurfOvPos[i];
			SurfOvOfsPrev[i] = SurfOvOfs[i];
			SurfOvRotPrev[i] = SurfOvRot[i];
		}
	}

	// RS FORK -- SCRIPT-SUPPRESSED LAYER.
	//
	// Hides this layer in both psprite passes while leaving the weapon itself
	// completely intact -- it keeps its states, damage, ammo and slot, it just
	// is not drawn. That split is what lets a rigged hand stand in for a mod's
	// fist: the fist weapon still does the hitting, the hand is what you see.
	//
	// Alpha cannot do this job. psp->alpha is discarded by
	// DPSprite::GetRenderStyle unless the layer carries PSPF_ALPHA or
	// PSPF_FORCEALPHA, and the draw decision is a `continue` in a render loop
	// no script participates in -- the same wall the flat-overlay fix ran into.
	//
	// In-class initialiser for the same reason as the fields above: the private
	// savegame constructor runs no constructor body.
	bool  NoDraw         = false;

	// RS FORK -- PER-LAYER HAND SELECTION.
	//
	// Which hand this layer is drawn at. <0 keeps today's behaviour exactly:
	// hw_weapon.cpp picks the hand from the layer id (at or above
	// PSP_OFFHANDWEAPON means the off hand) and otherwise from whether the
	// layer's caller IS player->OffhandWeapon. 0 = main hand, 1 = off hand,
	// matching VR_MAINHAND / VR_OFFHAND.
	//
	// Both of those channels are indirect, and one direction cannot be
	// expressed by either: a layer whose caller is the off-hand weapon is
	// ALWAYS drawn at the off hand, and a low id cannot even be created when
	// ReadyWeapon is null (GetPSprite defaults the caller to ReadyWeapon and
	// gives up when that is null). So a gun held in the off hand could draw
	// nothing at the main hand at all -- no magazine carried over to it, no
	// shell, no free hand on the forend.
	//
	// RENDER ONLY, and that is the whole point. A psprite is drawn on one
	// machine and seen by nobody else, so this feeds no playsim decision and
	// cannot desync. It is serialised so a save resumes looking as it looked.
	//
	// In-class initialiser for the same reason as every field above: the
	// private savegame constructor runs no constructor body, and a garbage
	// value here would fling a layer to the wrong hand on an old save.
	int   HandOverride   = -1;

	// RS FORK -- HUD BONE ANCHORING.
	//
	// Draw this layer at a BONE of another layer's model: AnchorLayer names the
	// layer to follow, AnchorBone the bone on it. The layer's own MODELDEF
	// offsets and rotations then apply relative to that bone rather than to the
	// controller.
	//
	// psprite layers are otherwise completely independent -- nothing can follow
	// anything -- so every "put this exactly there" problem (a hand on a grip, a
	// magazine entering its well, a shell at the loading port) had to be solved
	// by hand-tuning offsets per weapon, and re-tuned whenever either model
	// moved. Anchoring makes the target a measured position instead of a guess.
	//
	// The anchored layer must have a HIGHER id than its target: psprites draw in
	// id order and the target's bones are only known once it has been drawn.
	// MDL_FOLLOWBODY: where on the body this layer sits, and which way the body
	// faces. Map units, body axes -- X forward, Y right, Z up, the same
	// convention as AActor::FollowBodyOfs. BodyYaw is supplied by the caller
	// because the renderer's own heading is not visible from script.
	DVector3 BodyOfs      = DVector3(0, 0, 0);
	double   BodyYaw      = 0.0;

	int   AnchorLayer    = -1;
	FName AnchorBone     = NAME_None;

	// WHERE on that bone to sit, and facing which way. Without these, anchoring
	// puts a hand at the bone's origin in the bone's orientation, which is
	// wherever the artist happened to place the tag -- so the gun ends up
	// jammed through the palm rather than sitting in it.
	//
	// Summed into the same MODELDEF offset and rotation calls that the
	// placement sliders use, deliberately rather than applied as extra
	// transforms afterwards: rotations do not commute, so a number here only
	// means the same thing as the equivalent MODELDEF value if it is added at
	// the same point in the chain.
	//
	// Per LAYER rather than per model, because the same hand mesh sits on a
	// pistol grip, a shotgun forend and a charging handle, and those are three
	// different fits of one model.
	DVector3 AnchorOfs    = { 0, 0, 0 };
	DVector3 AnchorAngles = { 0, 0, 0 };   // yaw, pitch, roll


	// Where the anchored bone actually resolved to, as an offset from its
	// weapon's origin in the model's axes, in map units. Written by the
	// renderer into THIS psprite, read by script from THIS psprite.
	//
	// It lands here rather than being looked up from a shared map because
	// script asking the renderer's table directly is a cross-thread read of a
	// TMap while the renderer is writing it -- which crashes, with no message,
	// exactly when a hand reaches for the part. The psprite is already owned by
	// the game side and already the thing that made the request.
	DVector3 AnchorBonePos = { 0, 0, 0 };
	bool     AnchorBoneLive = false;

	// The same bone as a WORLD position, in the frame AttackPos and OffhandPos
	// live in, so a grab test is a plain distance between two world points.
	//
	// AnchorBonePos alone cannot do that job: it is expressed in the weapon
	// model's own axes, so comparing it to a hand position means rebuilding the
	// weapon's basis in script and rotating it by hand -- which is easy to get
	// subtly wrong, and a subtly wrong grab point presents as a grab that never
	// fires rather than as a grab in the wrong place. The renderer already holds
	// the matrix that answers this exactly, so it answers it here.
	DVector3 AnchorBoneWorld = { 0, 0, 0 };

	// The same bone's ORIENTATION, as yaw/pitch/roll in the playsim's own
	// convention, so an actor spawned to replace a bone-driven part can be
	// given the exact rotation that part was drawn at.
	//
	// Position alone is not enough for that: a magazine leaving a weapon has
	// to continue from the angle it was actually at, and reconstructing that
	// in script means rebuilding the weapon's basis by hand -- the same trap
	// AnchorBoneWorld exists to avoid, and one with no good outcome, since
	// ZScript's Quat exposes no vector-rotate to do it with. The renderer
	// already holds the full matrix; it costs nothing to read the rotation
	// out of it at the same moment it reads the translation.
	DVector3 AnchorBoneAngles = { 0, 0, 0 };   // (yaw, pitch, roll), degrees

private:
	DPSprite () {}

	void Serialize(FSerializer &arc);

public:	// must be public to be able to generate the field export tables. Grrr...
	TObjPtr<AActor*> Caller;
	TObjPtr<DPSprite*> Next;
	player_t *Owner;
	FState *State;
	int Sprite;
	int Frame;
	int ID;
	bool processPending; // true: waiting for periodic processing on this tick

	friend class player_t;
	friend void CopyPlayer(player_t *dst, player_t *src, const char *name);
};

void P_NewPspriteTick();
void P_CalcSwing (player_t *player);
void P_SetPsprite(player_t *player, PSPLayers id, FState *state, bool pending = false, AActor *newcaller = nullptr);
void P_BringUpWeapon (player_t *player);
void P_FireWeapon (player_t *player);
void P_BobWeapon(player_t* player);
void P_BobWeapon3D(player_t* player);
DAngle P_BulletSlope (AActor *mo, FTranslatedLineTarget *pLineTarget = NULL, int aimflags = 0);
AActor *P_AimTarget(AActor *mo);

void DoReadyWeaponToBob(AActor *self, int hand = 0);
void DoReadyWeaponToFire(AActor *self, bool primary = true, bool secondary = true, int hand = 0);
void DoReadyWeaponToSwitch(AActor *self, bool switchable = true, int hand = 0);

void A_ReFire(AActor *self, FState *state = NULL);

extern EPSPBobType BobType;
extern FPlayerBob PlayerBob[MAXPLAYERS];

#endif	// __P_PSPR_H__
