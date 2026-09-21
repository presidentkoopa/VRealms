/*
** actor.zs
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 id Software
** Copyright 1999-2016 Marisa Heit
** Copyright 2006-2017 Christoph Oelckers
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

struct FCheckPosition
{
	// in
	native Actor		thing;
	native Vector3		pos;

	// out
	native Sector		cursector;
	native double		floorz;
	native double		ceilingz;
	native double		dropoffz;
	native TextureID	floorpic;
	native int			floorterrain;
	native Sector		floorsector;
	native TextureID	ceilingpic;
	native Sector		ceilingsector;
	native bool			touchmidtex;
	native bool			abovemidtex;
	native bool			floatok;
	native bool			FromPMove;
	native line			ceilingline;
	native Actor		stepthing;
	native bool			DoRipping;
	native bool			portalstep;
	native int			portalgroup;

	native int			PushTime;

	// These are internal helpers to properly initialize an object of this type.
	private native void _Constructor();
	private native void _Destructor();

	native void ClearLastRipped();
}

struct FLineTraceData
{
	enum ETraceResult
	{
		TRACE_HitNone,
		TRACE_HitFloor,
		TRACE_HitCeiling,
		TRACE_HitWall,
		TRACE_HitActor,
		TRACE_CrossingPortal,
		TRACE_HasHitSky
	};

	native Actor HitActor;
	native Line HitLine;
	native Sector HitSector;
	native F3DFloor Hit3DFloor;
	native TextureID HitTexture;
	native Vector3 HitLocation;
	native Vector3 HitDir;
	native double Distance;
	native int NumPortals;
	native int LineSide;
	native int LinePart;
	native int SectorPlane;
	native int HitType;
}

struct LinkContext
{
	readonly voidptr sector_list;	// really msecnode but that's not exported yet.
	readonly voidptr render_list;
}

class ViewPosition native
{
	native readonly Vector3 Offset;
	native readonly int Flags;
}

class Behavior native play abstract version("4.15.1")
{
	native readonly Actor Owner;
	native readonly LevelLocals Level;

	virtual void Initialize() {}
	virtual void Reinitialize() {}
	virtual void TransferredOwner(Actor oldOwner) {}
	virtual void Tick() {}
}

class BehaviorIterator native abstract final version("4.15.1")
{
	native static BehaviorIterator CreateFrom(Actor mobj, class<Behavior> type = null);
	native static BehaviorIterator Create(class<Behavior> type = null, class<Actor> ownerType = null, bool clientSide = false);

	native Behavior Next();
	native void Reinit();
}

struct TRS
{
	FVector3 translation;
	FQuat rotation;
	FVector3 scaling;
}

class AnimationFrame native abstract sealed(PrecalculatedAnimationFrame, InterpolatedFrame) {}

class PrecalculatedAnimationFrame : AnimationFrame native final
{
	native Array<TRS> frameData; //hacky but required
}

class InterpolatedFrame : AnimationFrame native final
{
	native float inter;	// = -1.0f;
	native int frame1;		// = -1;
	native int frame2;		// = -1;
}

enum EModelAnimFlags
{
	MODELANIM_NONE			= 1 << 0, // no animation
	MODELANIM_LOOP			= 1 << 1, // animation loops, otherwise it stays on the last frame once it ends
};

class AnimationSequence native final
{
	native int firstFrame;			// = 0;
	native int lastFrame;			// = 0;
	native int loopFrame;			// = 0;
	native float framerate;			// = 0;
	native double startFrame;		// = 0;
	native int flags;				// = MODELANIM_NONE;
	native double startTic;			// = 0; // when the current animation started (changing framerates counts as restarting) (or when animation starts if interpolating from previous animation)
	native double switchOffset;		// = 0; // when the animation was changed -- where to interpolate the switch from

	AnimationSequence Init()
	{
		flags = MODELANIM_NONE;
		return self;
	}
}

class AnimationLayer native final
{
	native AnimationSequence curAnim;
	native AnimationFrame prevAnim;
}

class Actor : Thinker native
{
	const DEFAULT_HEALTH = 1000;
	const ONFLOORZ = -2147483648.0;
	const ONCEILINGZ = 2147483647.0;
	const STEEPSLOPE = (46342./65536.);	// [RH] Minimum floorplane.c value for walking
	const FLOATRANDZ = ONCEILINGZ-1;
	const TELEFRAG_DAMAGE = 1000000;
	const MinVel = double.equal_epsilon;
	const LARGE_MASS = 10000000;	// not INT_MAX on purpose
	const ORIG_FRICTION = (0xE800/65536.);	// original value
	const ORIG_FRICTION_FACTOR = (2048/65536.);	// original value
	const DEFMORPHTICS = 40 * TICRATE;
	const MELEEDELTA = 20;

	// flags are not defined here, the native fields for those get synthesized from the internal tables.

	// for some comments on these fields, see their native representations in actor.h.
	native readonly Actor snext;	// next in sector list.
	native PlayerInfo Player;
	native readonly ViewPosition ViewPos; // Will be null until A_SetViewPos() is called for the first time.
	native readonly vector3 Pos;
	native vector3 Prev;
	native uint ThruBits;
	native uint lineBlockBits;	// [BLOCKBITS] lines whose blockBits share a bit block this actor (GZSelaco 14d9255578)
	native vector2 SpriteOffset;
	native vector3 WorldOffset;
	native double spriteAngle;
	native double spriteRotation;
	native float VisibleStartAngle;
	native float VisibleStartPitch;
	native float VisibleEndAngle;
	native float VisibleEndPitch;
	native double Angle;
	native double Pitch;
	native double Roll;
	native double AngledRollOffset;
	native vector3 Vel;
	native double Speed;
	native double FloatSpeed;
	native SpriteID sprite;
	native uint8 frame;
	// RS fork -- true when this class appears in MODELDEF at all. Set on the
	// CLASS DEFAULTS by the modeldef parser, and it is the same flag
	// FindModelFrameRaw gates on. Read it off GetDefaultByType, never off a
	// live actor: A_ChangeModel sets it on the instance as a side effect, so
	// an instance read reports true for anything already model-swapped.
	native readonly bool hasmodel;
	native vector2 Scale;
	native TextureID picnum;
	native double Alpha;
	native readonly color fillcolor;	// must be set with SetShade to initialize correctly.
	native Sector CurSector;
	native double CeilingZ;
	native double FloorZ;
	native double DropoffZ;
	native Sector floorsector;
	native TextureID floorpic;
	native int floorterrain;
	native Sector ceilingsector;
	native TextureID ceilingpic;
	native double Height;
	native readonly double Radius;
		native readonly double RenderRadius;
	native double projectilepassheight;
	native int tics;
	native readonly State CurState;
	native readonly int Damage;
	native int projectilekickback;
	native int special1;
	native int special2;
	native double specialf1;
	native double specialf2;
	native int weaponspecial;
	native int Health;
	native uint8 movedir;
	native int8 visdir;
	native int16 movecount;
	native int16 strafecount;
	native Actor Target;
	native Actor Master;
	native Actor Tracer;
	native readonly Actor DamageSource;
	native Actor LastHeard;
	native Actor LastEnemy;
	native Actor LastLookActor;
	native int ReactionTime;
	native int Threshold;
	native readonly int DefThreshold;
	native vector3 SpawnPoint;
	native uint16 SpawnAngle;
	native int StartHealth;
	native uint8 WeaveIndexXY;
	native uint8 WeaveIndexZ;
	native uint16 skillrespawncount;
	native int Args[5];
	native int Mass;
	native int Special;
	native readonly int TID;
	native readonly int TIDtoHate;
	native readonly int WaterLevel;
	native readonly double WaterDepth;
	native int Score;
	native int Accuracy;
	native int Stamina;
	native double MeleeRange;
	native int PainThreshold;
	native double Gravity;
	native double FloorClip;
	native name DamageType;
	native name DamageTypeReceived;
	native uint8 FloatBobPhase;
	native double FloatBobStrength;
	native double FloatBobFactor;
	native int RipperLevel;
	native int RipLevelMin;
	native int RipLevelMax;
	native name Species;
	native Actor Alternative;
	native Actor goal;
	native uint8 MinMissileChance;
	native double MissileChanceMult;
	native int8 LastLookPlayerNumber;
	native uint SpawnFlags;
	native double meleethreshold;
	native double maxtargetrange;
	native double bouncefactor;
	native double wallbouncefactor;
	native int bouncecount;
	native double friction;
	native int FastChaseStrafeCount;
	native double pushfactor;
	native int lastpush;
	native int activationtype;
	native int lastbump;
	native int DesignatedTeam;
	native Actor BlockingMobj;
	native Line BlockingLine;
	native Line MovementBlockingLine;
	native Sector Blocking3DFloor;
	native Sector BlockingCeiling;
	native Sector BlockingFloor;
	native int PoisonDamage;
	native name PoisonDamageType;
	native int PoisonDuration;
	native int PoisonPeriod;
	native int PoisonDamageReceived;
	native name PoisonDamageTypeReceived;
	native int PoisonDurationReceived;
	native int PoisonPeriodReceived;
	native Actor Poisoner;
	native int MinRespawnTics;
	native int RespawnDice;

	native Inventory Inv;
	native uint8 smokecounter;
	native uint8 FriendPlayer;
	native TranslationID Translation;
	native sound AttackSound;
	native sound DeathSound;
	native sound SeeSound;
	native sound PainSound;
	native sound ActiveSound;
	native sound UseSound;
	native sound BounceSound;
	native sound WallBounceSound;
	native sound CrushPainSound;
	native double MaxDropoffHeight;
	native double MaxStepHeight;
	native double MaxSlopeSteepness;
	native int16 PainChance;
	native name PainType;
	native name DeathType;
	native double DamageFactor;
	native double DamageMultiply;
	native Class<Actor> TelefogSourceType;
	native Class<Actor> TelefogDestType;
	native readonly State SpawnState;
	native readonly State SeeState;
	native State MeleeState;
	native State MissileState;
	native DecalBase DecalGenerator;
	native uint8 fountaincolor;
	native double CameraHeight;	// Height of camera when used as such
	native double CameraFOV;
	native double ViewAngle, ViewPitch, ViewRoll;
	native double RadiusDamageFactor;		// Radius damage factor
	native double SelfDamageFactor;
	native double ShadowAimFactor, ShadowPenaltyFactor;
	native double StealthAlpha;
	native int WoundHealth;		// Health needed to enter wound state
	native readonly color BloodColor;
	native readonly TranslationID BloodTranslation;
	native int RenderHidden;
	native int RenderRequired;
	native int FriendlySeeBlocks;
	native int16 lightlevel;
	native readonly int SpawnTime;
	private native int InventoryID;	// internal counter.
	native uint freezetics;
	native Vector2 AutomapOffsets;
	native double LandingSpeed;
	// Draw this actor as its voxel if it has one, outranking any model, unless
	// r_voxels_mode is 2 (no voxels at all). With a voxel pack loaded, that
	// cvar's auto default draws ONLY actors with this set as voxels. A frame
	// that exists only as a voxel always keeps its voxel. Set it on grab and
	// clear it on release to make a held object a real 3D thing you can turn
	// over; a billboard cannot be. Costs a null check on actors that have no
	// voxel. See FindModelFrame. [round2 X3: this said "ignoring r_drawvoxels"]
	native bool VoxelOverride;
	native bool ForceModelAngles;
	native int HardpointButtons;
	// Draw this actor in the player's body frame -- head position and yaw, read
	// at DRAW rate -- with FollowBodyOfs as its seat in that frame (X forward,
	// Y right, Z up, map units). Mode 0 is off and is the default. Per-actor
	// rather than a MODELDEF flag because a dozen props share one class and each
	// sits somewhere different on the body.
	// 1 = the renderer's own heading. 2 = FollowBodyYaw below.
	// Use 2 whenever the caller filters its own heading: the renderer's heading
	// is not readable from here and is NOT the same as HmdYaw.
	native int FollowBodyMode;
	native double FollowBodyYaw;
	// Draw FollowBodyYaw at display rate: last tic's heading turned toward this
	// tic's by the shortest way, as Angles are drawn. false, the default, draws the
	// value as written. Not saved: set it every tic with the heading. A snap turn
	// stays a snap if ClearInterpolation() is called after writing it.
	native bool FollowBodyYawInterp;
	native Vector3 FollowBodyOfs;

	// RS FORK -- HELD IN A HAND, PLACED AT DRAW RATE. The hand-frame twin of
	// FollowBodyMode/FollowBodyOfs above.
	//
	// FollowHandMode overrides WHICH controller this model rides, regardless of
	// what its MODELDEF says:
	//     0  the MODELDEF decides (FollowMainHand / FollowOffHand). The default.
	//     1  the main hand.    2  the off hand.
	//
	// The case it exists for: an off hand reaching for a pistol's slide. The
	// slide belongs to the gun and the gun rides the MAIN controller, so the
	// hand has to be drawn in the MAIN frame to touch it -- while the player's
	// real off hand stays somewhere the two controllers do not collide.
	//
	// FollowHandOfs is where in that frame it sits. It is ADDED TO the model's
	// own placement offsets, so it is in the same units and axes as MODELDEF
	// Offset and the _ofs_x/_ofs_y/_ofs_z placement cvars: whatever number moves
	// a slider one unit moves this one unit. Zero changes nothing.
	native int FollowHandMode;
	native Vector3 FollowHandOfs;

	// A TURN OF THE HAND'S FRAME, in degrees, about the hand itself:
	//   X yaw   + turns the model left (like Angle)
	//   Y pitch + tips the muzzle down (like Pitch)
	//   Z roll  + tips its top to the right, seen from behind
	// It turns the model's whole seat -- Offset, FollowHandOfs, the sliders -- so a
	// gun swings about the grip, and anything riding it turns with it. Unlike
	// FollowHandOfs it reads the same in either hand: the renderer undoes the
	// off-hand mirror. Script-owned animation (recoil, a draw flick, a catch
	// settling); no slider writes it. Drawn interpolated between tics, so set it
	// once a tic. Zero changes nothing.
	native Vector3 FollowHandRot;

	// WHOSE PLACEMENT SLIDERS THIS ACTOR USES RIGHT NOW.
	//
	// A MODELDEF names a PlacementCVars prefix for the whole class. This names
	// one for this actor, this moment, and the RENDERER reads it -- so the six
	// values under <prefix>_ofs_x/_y/_z, _yaw/_pitch/_roll (and _scale) move the
	// model WHILE THE MENU IS OPEN. Nothing script-driven can do that: the
	// playsim is frozen behind a menu, so a script-read number only lands once
	// you close it, which looks exactly like a dead slider.
	//
	// 'None' (the default) uses the MODELDEF's own prefix and changes nothing.
	//
	// Pair it with FollowHandOfs, not against it: this prefix is the TUNING,
	// owned by the player and never written by script; FollowHandOfs is the
	// ANIMATION, owned by script and never touched by a slider. The renderer
	// adds them. One writer each.
	native name PlacementPrefix;

	// RS FORK -- DRAWN INSIDE ANOTHER ACTOR'S MODEL, AT DRAW RATE.
	//
	// The model rides FollowActor's model as it is drawn this frame -- its seat,
	// its own follow mode and its live placement sliders included -- so it moves
	// with the parent even while a menu has the game paused. Not the parent's
	// scale, and not its MODELDEF base orientation.
	//
	// FollowActorOfs is the seat in that frame: X forward, Y left, Z up, map
	// units -- the axes GetModelWorldOffset answers in. This actor's own angles
	// apply on top, RELATIVE to the frame.
	//
	// FollowActorSlot -1 (the default) follows the whole model; 0..15 follows
	// that surface slot of the parent as it is drawn (a set part transform or a
	// live hand drive). Null is off. Keep the actor near the parent: its own
	// position still decides whether it is drawn at all. See actor.h.
	native Actor FollowActor;
	native int FollowActorSlot;
	native Vector3 FollowActorOfs;

	// RS FORK -- OR A JOINT OF THE PARENT'S MODEL, AS DRAWN (Engine docs/MODEL_JOINT_DRIVE_PLAN.md piece E). Name a joint
	// and this rides it on the parent's model index FollowActorJointModel, in FollowActorSlot's place: its animation,
	// joint poses, joint offsets and drives, reach chains and aims all in -- a hand seat on a slide the other hand drags,
	// a sight on tag_rail_attach, a wrist display on an animated gauntlet. Rigid: the joint's origin and its rotation,
	// never its scale. The identity at the bind pose, so a FollowActorOfsInModel point is a point on the BIND-POSE mesh
	// (the IQM file's vertex, as (x, z, y)). Read from the parent's last draw: one frame late when this is drawn first,
	// the same for both eyes. The picture only: ModelPointToWorld, ModelFollowFrameToWorld, GetBonePosition and every
	// other script query see the whole model's frame. No such joint, a parent not drawn yet, or a hidden joint rides
	// the whole model and says so ([FOLLOWJOINT]). On a DECOUPLEDANIMATIONS or MODELSAREATTACHMENTS parent the joint is
	// taken from its one palette. 'None' (the default) is off. See actor.h.
	native name FollowActorJoint;
	native int FollowActorJointModel;

	// Read FollowActorOfs as a POINT ON THE PARENT'S MESH instead of a seat in
	// the follow frame. Give it the same (x, y, z) you would hand the parent's
	// ModelPointToWorld: the renderer's model space, y up, so an MD3-file point
	// (x, y, z) goes in as (x, z, y). The renderer carries it through the
	// parent's scale, mirror and MODELDEF orientation, so a point read off the
	// mesh lands on the mesh, and with FollowActorSlot it rides that part.
	// Position only: this actor still turns with the follow frame, and
	// FollowActorOfsCVar/2 still add on top in the frame's axes and units. False,
	// the default, is the old meaning. See actor.h.
	native bool FollowActorOfsInModel;

	// A SEAT YOU CAN TUNE WITH THE MENU OPEN. Names a placement set; the
	// RENDERER adds <prefix>_ofs_x/_ofs_y/_ofs_z to FollowActorOfs every frame,
	// in the follow frame's axes and units, so a slider moves this while the
	// playsim is frozen. Not divided by this actor's scale, so one set of cvars
	// serves children of different sizes. 'None' adds nothing. See actor.h.
	native name FollowActorOfsCVar;

	// A second set, summed with the first: one seat moved by two sliders that
	// cannot share a name -- a correction for a whole family of children and a
	// nudge for this one. Both live. See actor.h.
	native name FollowActorOfsCVar2;

	// Trace this actor in neon, from its own sprite. See func_spriteoutline.fp
	// and the note in actor.h. OutlineMode 0 is off and is the default; 1 keeps
	// the body and adds a glowing edge, 2 erases the body and leaves the wire
	// figure, 3 flattens the body to the tint with the edge bright over it.
	// Works on any actor, names no monster, and is unaffected by stairs -- the
	// outline IS the sprite.
	native color OutlineColorA;
	native color OutlineColorB;
	native double OutlineStrength;
	native double OutlineThickness;
	native double OutlineThreshold;
	native double OutlineGlow;
	native double OutlinePulse;
	native int OutlineMode;

	// [SCENEMASK] What this actor's pixels ARE, for a post-process shader that asks.
	//
	// The scene pass stamps this byte into a post-process-readable attachment for every
	// pixel this actor draws (its sprite or its model). A shader that declares the scene
	// mask -- a GLDEFS postprocess block with `Texture SceneMask "SceneMask"` -- then reads
	// it per pixel and decides: keep these in colour while the rest greys out, outline
	// those, show only the warm ones, keep this marker legible through a grade.
	//
	// 0 is "nothing special" and is the default, so nothing changes for anything that does
	// not opt in. What 1..255 mean is up to the mod: the engine never learns, so two mods
	// may use different numbers and a third may read both. Agree the numbers between the
	// mod that sets them and the shader that reads them.
	//
	// Presentation only: nothing in the playsim reads it, so setting it on one machine and
	// not another cannot desync anything. It is saved with the actor.
	//
	// Vulkan only, and only while some loaded post-process shader declares the mask --
	// otherwise setting it costs nothing at all.
	native uint8 PostMask;

	// Draw this actor only while the named cvar is above zero. The RENDERER
	// reads it every frame, so it answers even while a menu has the playsim
	// frozen: a tuning page lights what it is editing by setting one cvar from
	// its UI code. 'None' (the default) always draws. See actor.h.
	native name VisibleCVar;

	// Drawn at the fade this cvar says, read by the RENDERER every frame, so a
	// fade slider answers while a menu has the playsim frozen. 'None' (the
	// default) keeps this actor's own Alpha. See actor.h.
	native name AlphaCVar;

	// Drawn at the size this cvar says, while it is above zero -- read by the
	// RENDERER every frame, so a size slider answers behind a paused menu. At
	// zero, or with no such cvar, this actor's own Scale is used. ScaleCVarUnit
	// is what one of that cvar means as a scale, so a slider in map units can
	// drive a mesh of any radius in a frame of any units. See actor.h.
	native name ScaleCVar;
	native double ScaleCVarUnit;

	// Breathes: above zero, this actor fades in and out PulseHz times a second,
	// down to PulseDepth of its alpha and back. Driven by the RENDERER from the
	// wall clock, so it keeps breathing while a menu has the playsim frozen --
	// which is when a "this is the one you are editing" mark has to be visible.
	// A sine, never a step. Zero (the default) is off. See actor.h.
	native double PulseHz;
	native double PulseDepth;

	// Multiplies the colour this actor is drawn in, whatever its render style --
	// the texture stays, tinted, unlike SetShade, which only a Stencil or Shaded
	// style uses. With +BRIGHT it glows in that colour. 0 (the default) is off.
	// See actor.h.
	native color TintColor;

	// Three sizes for a model, because Scale has only two and a mesh has three.
	// Multiplies the model's own scale per axis, in the mesh's own space; zero or
	// less on an axis leaves that axis alone, so (0,0,0) -- the default -- draws
	// as before. What turns a drawn reach sphere into the oval it really is.
	native vector3 ScaleAxes;

	// [BB] A SWEEP FRONT JUST REACHED THIS ACTOR.
	//
	// Called once, at the moment a travelling band's front crosses it -- not
	// while it is inside the band, and not when the band was fired.
	//
	// EMPTY HERE ON PURPOSE, and the engine never calls it. This exists so the
	// thing that DRAWS a sweep never has to know what a monster is, and the
	// thing that knows what a monster is never has to know a sweep exists. A
	// lighting mod calls it on whatever it crosses; a monster mod overrides it
	// and re-tiers. Neither has to be loaded for the other to compile, which is
	// the whole point -- an earlier draft put the monster tier table inside the
	// lighting mod and made it fail to load without a monster mod present.
	//
	// The tint is the band's colour AT THAT MOMENT, so a band carrying a second
	// colour hands out a different one at the near edge than at the far edge --
	// which is what lets one sweep leave a gradient behind it rather than one
	// flat answer.
	virtual void OnSweepCrossed(Vector3 origin, double front, Color tint) {}
	// WRITABLE FROM SCRIPT, 2026-08-30. These six were readonly, which was fine
	// while the only thing that ever set them was the VR backend writing the
	// controller pose -- but a WRIST-MOUNTED weapon fires from the mount along
	// the arm, not from wherever the hand is pointing, and the attack origin and
	// direction are exactly these fields. A mod that cannot write them can only
	// spray wherever the palm happens to face.
	//
	// Still rewritten from the controller every frame, so a script write lasts
	// one tic by design -- which is what a single shot wants. Nothing that does
	// not write them is affected.
	native vector3 AttackPos;
	native double AttackPitch;
	native readonly double AttackRoll;
	native double AttackAngle;

	// The main hand's REAL wrist roll. AttackRoll is zeroed every tic by the
	// playsim -- the usercmd has no weaponroll to rebuild it from, so it must
	// read the same on every peer -- which left script believing the wrist was
	// level while the held model rolled with it. Use this for anything welded
	// to the weapon; use AttackRoll for anything that must agree over a net.
	// OffhandRoll is already true and needs no counterpart.
	native readonly double MainHandRoll;

	// REAL controller velocity, from OpenXR's own sensor fusion -- not
	// inferred by differencing two AttackPos samples 28ms apart in script,
	// which amplifies tracking jitter and throws away everything the render
	// thread saw between tics. Renderer-owned like AttackPos itself: written
	// every frame, never serialised, reads (0,0,0) on any frame the runtime
	// didn't report a valid velocity rather than holding a stale value.
	// Linear is map-units/second in the same frame AttackPos lives in;
	// angular is radians/second about the MAP axes (X, Y, Z up), NOT the
	// hand's own -- spin about the barrel is its projection onto the hand's
	// forward axis. What the tip of a swung weapon is doing that linear
	// velocity alone can't say.
	native readonly vector3 AttackVel;
	native readonly vector3 AttackAngularVel;

	// RS fork -- address a model frame directly, bypassing the sprite letter
	// table. The hand rig's poses live at 0-10 and 1289-1297 and no sprite
	// letter can name frame 1293, so this is the only way to reach them on a
	// world actor. Set ModelFrame to -1 to go back to normal resolution.
	// ModelFrameLerp in 0..1 blends the BONES from ModelFrame toward
	// ModelFrameNext, so a grip closes instead of snapping shut.
	native int ModelFrame;
	native int ModelFrameNext;
	native float ModelFrameLerp;
	native vector3 OffhandPos;
	native double OffhandPitch;
	native readonly double OffhandRoll;
	native double OffhandAngle;

	// Same as AttackVel/AttackAngularVel above, off hand.
	native readonly vector3 OffhandVel;
	native readonly vector3 OffhandAngularVel;
	native readonly bool OverrideAttackPosDir;

	// Real headset position/orientation in map units, world space -- where the
	// player's head actually is, not an aim ray. For body-relative UI such as
	// holsters that need to reason about the player's physical pose.
	native readonly vector3 HmdPos;
	native readonly double HmdYaw;
	native readonly double HmdPitch;
	native readonly double HmdRoll;

	// Two-hand stabilize reach for the ready weapon, inches. Write this from
	// ReadyWeapon.StabilizeDistance each tic; native reads it in place of a
	// fixed constant. 0 = use vr_stabilize_distance_inches, negative = disabled.
	native double StabilizeReach;

	// Set true for a hand while it is claimed by body-relative UI (a holster
	// reach) this frame. Native gives a holster claim priority over stabilize
	// and the grip-modifier layer for that hand -- one hand, one meaning.
	native bool HolsterClaimMain;
	native bool HolsterClaimOff;

	// Set true for a hand while it has something it could take hold of -- in
	// reach, or picked out by a targeting cone. Script-owned, same claim /
	// arbitrate split as HolsterClaim* above.
	//
	// It outranks the grip modifier layer. The dominant grip is the shift layer
	// while vr_secondary_button_mappings is on, and that layer stands analog
	// turning down -- so without this, reaching for anything stops you turning.
	// Claim it and that hand's grip means grabbing for as long as there is
	// something to grab, and goes back to being the modifier when there is not.
	native bool GrabClaimMain;
	native bool GrabClaimOff;

	// Set true for a hand while it is at a body HARDPOINT. Separate from
	// HolsterClaim* deliberately: both are body-anchored volumes, but they are
	// driven by different mods and a single shared flag meant each one erased
	// the other's answer every tic, silently.
	//
	// Own your own field and write it every frame. Do not write another
	// system's.
	native bool HardpointClaimMain;
	native bool HardpointClaimOff;

	// What the grip arbiter decided each hand's grip MEANS this frame.
	// 0 none, 1 holster, 2 stabilize, 3 modifier, 4 plain (EGripContext).
	// Engine-owned and the mirror of HolsterClaim* above: script says what it
	// wants to claim, this reports what actually won.
	// Capacitive finger contact. Bit 0 = thumb resting on a surface, bit 1 =
	// index resting on the trigger. Contact, not press -- an index can rest on
	// the trigger without firing, which is the pose that reads as gun handling.
	native readonly int FingerTouchMain;
	native readonly int FingerTouchOff;
	native readonly int GripContextMain;
	native readonly int GripContextOff;

	// The RAW squeeze, per hand. Engine-owned.
	//
	// GripContext above is published even while the grip is NOT held -- a hand
	// keeps holding a magazine when you relax your fingers -- so once anything
	// claims a subject it latches to GRIPCTX_Object and `GripContext != 0`
	// reads as a grip held forever. That is correct for POSE and useless for
	// EDGES: a toggle can never see its own release. Test this for the button.
	native readonly bool GripHeldMain;
	native readonly bool GripHeldOff;

	// HOW FAR the trigger and the squeeze are pulled, 0..1, per hand.
	//
	// GripHeld* above is this same control after the runtime applied its own
	// threshold and threw the rest away. Read the bool when you want a button
	// -- it carries the runtime's idea of "pressed", which should not be
	// re-derived per mod -- and read these when you want the travel: a trigger
	// with a real break and reset, a pull that stops short of the shot, a grip
	// that tightens on a part instead of merely closing on it.
	//
	// 0 or 1 with nothing in between on controllers whose squeeze is a click
	// rather than an axis (Vive wand, WMR). That is what that hardware has.
	native readonly double TriggerValueMain;
	native readonly double TriggerValueOff;
	native readonly double GripValueMain;
	native readonly double GripValueOff;

	// Thumbstick position per hand, each axis -1..1, centred at (0,0).
	//
	// FingerTouch* says a thumb is resting somewhere; this says WHERE along a
	// range, which is what a thumb sliding a fire selector or stepping a sight
	// dial needs and a touch bit cannot express.
	native readonly Vector2 ThumbPosMain;
	native readonly Vector2 ThumbPosOff;

	// What each hand is closed ON, as opposed to what its grip MEANS above.
	// EGripSubject, declared in constants.zs so every mod can name the values
	// rather than writing bare integers.
	//
	// Claim / result, the same pair as HolsterClaim* and GripContext*. Write
	// GripClaim* to say what this hand has taken hold of -- only script can
	// know that -- and read GripSubject* for what the arbiter settled on, which
	// is the claim unless the hand is inside a holster.
	//
	// A non-zero claim also stands two-hand stabilize down for that hand.
	// Stabilize is proximity between the controllers and nothing more, and
	// reloading is exactly the gesture that brings the hands together, so
	// without this every reload reads as gripping the weapon two-handed.
	// More than one mod can write these, so the convention is: SET your subject
	// while you hold it, and CLEAR it only when the current value is one of
	// yours. A mod that blanks the field whenever it has nothing to say erases
	// everyone else's claim, and handler order is not something any of them
	// controls.
	native int GripClaimMain;
	native int GripClaimOff;
	native readonly int GripSubjectMain;
	native readonly int GripSubjectOff;

	// True while the off hand is actually ON the main hand's weapon -- its grip,
	// forend or foregrip -- rather than merely near it. It does not move the
	// weapon, deliberately.
	//
	// LOCAL, NOT PLAYSIM. This is written by the VR backend from controller
	// proximity, for the console player's own pawn, on the machine that has the
	// headset. On every other machine it is FALSE for that same pawn, and for a
	// desktop player it is false always.
	//
	// SO DO NOT DECIDE A SHOT WITH IT. This comment used to say "read it to
	// tighten a weapon's spread", and that was wrong: spread is a hit decision
	// and it draws from the playsim RNG, so a weapon following that advice
	// disagreed with its own peers on the first shot and left the RNG stream out
	// of phase for the rest of the session.
	//
	// To make a second hand on the weapon actually buy something, send the
	// DECISION and apply it everywhere:
	//
	//   local side:  EventHandler.SendNetworkEvent("mymod_grip", hand, held);
	//   play side:   override void NetworkProcess(ConsoleEvent e)
	//                { /* set replicated state on players[e.Player]'s weapon */ }
	//
	// Reading it to DRAW something -- a pose, a hand, a HUD cue -- is fine and is
	// what it is for.
	native readonly bool TwoHandedHold;

	// Accumulated CONTROLLER-driven yaw (snap + stick turn), degrees. HmdYaw is
	// physical head yaw PLUS this. Body-relative anchors must follow this part
	// 1:1 -- it rotates the whole virtual body -- while only the physical
	// remainder should get a neck deadzone.
	native readonly double VRTurnYaw;

	// What the laser sight's own beam trace is resting on, per hand, updated
	// every render frame. Engine-owned -- this is the SAME trace the beam is
	// drawn from, not a second one, so a mod checking "is this a headshot"
	// against it always agrees with what the player sees.
	native readonly Actor LaserTraceTargetMain;
	native readonly Actor LaserTraceTargetOff;
	native readonly vector3 LaserTraceHitPosMain;
	native readonly vector3 LaserTraceHitPosOff;

	// Write true here when the point above is a headshot -- the engine has no
	// idea which classes have heads, that is gameplay-mod data. The laser
	// sight reacts to this (vr_laser_headshot_react and friends) on whichever
	// hand it is set for.
	native bool LaserHeadshotLinedUpMain;
	native bool LaserHeadshotLinedUpOff;

	meta String Obituary;		// Player was killed by this actor
	meta String HitObituary;		// Player was killed by this actor in melee
	meta String SelfObituary;	// Player killed himself using this actor
	meta double DeathHeight;	// Height on normal death
	meta double BurnHeight;		// Height on burning death
	meta int GibHealth;			// Negative health below which this monster dies an extreme death
	meta Sound HowlSound;		// Sound being played when electrocuted or poisoned
	meta Name BloodType;		// Blood replacement type
	meta Name BloodType2;		// Bloopsplatter replacement type
	meta Name BloodType3;		// AxeBlood replacement type
	meta bool DontHurtShooter;
	meta double ExplosionRadius;
	meta int ExplosionDamage;
	meta int MeleeDamage;
	meta Sound MeleeSound;
	meta Sound RipSound;
	meta double MissileHeight;
	meta Name MissileName;
	meta double FastSpeed;		// speed in fast mode
	meta Sound PushSound;		// Sound being played when pushed by something

	// todo: implement access to native meta properties.
	// native meta int infighting_group;
	// native meta int projectile_group;
	// native meta int splash_group;

	Property prefix: none;
	Property Obituary: Obituary;
	Property HitObituary: HitObituary;
	Property SelfObituary: SelfObituary;
	Property MeleeDamage: MeleeDamage;
	Property MeleeSound: MeleeSound;
	Property MissileHeight: MissileHeight;
	Property MissileType: MissileName;
	Property DontHurtShooter: DontHurtShooter;
	Property ExplosionRadius: ExplosionRadius;
	Property ExplosionDamage: ExplosionDamage;
	//Property BloodType: BloodType, BloodType2, BloodType3;
	Property FastSpeed: FastSpeed;
	Property HowlSound: HowlSound;
	Property GibHealth: GibHealth;
	Property DeathHeight: DeathHeight;
	Property BurnHeight: BurnHeight;
	property Health: health;
	property WoundHealth: WoundHealth;
	property ReactionTime: reactiontime;
	property PainThreshold: PainThreshold;
	property DamageMultiply: DamageMultiply;
	property ProjectileKickback: ProjectileKickback;
	property Speed: speed;
	property FloatSpeed: FloatSpeed;
	property Radius: radius;
	property RenderRadius: RenderRadius;
	property Height: height;
	property ProjectilePassHeight: ProjectilePassHeight;
	property Mass: mass;
	property XScale: ScaleX;
	property YScale: ScaleY;
	property SeeSound: SeeSound;
	property AttackSound: AttackSound;
	property BounceSound: BounceSound;
	property WallBounceSound: WallBounceSound;
	property PainSound: PainSound;
	property DeathSound: DeathSound;
	property ActiveSound: ActiveSound;
	property CrushPainSound: CrushPainSound;
	property PushSound: PushSound;
	property Alpha: Alpha;
	property MaxTargetRange: MaxTargetRange;
	property MeleeThreshold: MeleeThreshold;
	property MeleeRange: MeleeRange;
	property PushFactor: PushFactor;
	property BounceCount: BounceCount;
	property WeaveIndexXY: WeaveIndexXY;
	property WeaveIndexZ: WeaveIndexZ;
	property MinMissileChance: MinMissileChance;
	property MissileChanceMult: MissileChanceMult;
	property MaxStepHeight: MaxStepHeight;
	property MaxDropoffHeight: MaxDropoffHeight;
	property MaxSlopeSteepness: MaxSlopeSteepness;
	property PoisonDamageType: PoisonDamageType;
	property RadiusDamageFactor: RadiusDamageFactor;
	property SelfDamageFactor: SelfDamageFactor;
	property StealthAlpha: StealthAlpha;
	property CameraHeight: CameraHeight;
	property CameraFOV: CameraFOV;
	property VSpeed: velz;
	property SpriteRotation: SpriteRotation;
	property VisibleAngles: VisibleStartAngle, VisibleEndAngle;
	property VisiblePitch: VisibleStartPitch, VisibleEndPitch;
	property Species: Species;
	property Accuracy: accuracy;
	property Stamina: stamina;
	property TelefogSourceType: TelefogSourceType;
	property TelefogDestType: TelefogDestType;
	property Ripperlevel: RipperLevel;
	property RipLevelMin: RipLevelMin;
	property RipLevelMax: RipLevelMax;
	property RipSound: RipSound;
	property RenderHidden: RenderHidden;
	property RenderRequired: RenderRequired;
	property FriendlySeeBlocks: FriendlySeeBlocks;
	property ThruBits: ThruBits;
	property LineBlockBits: lineBlockBits;	// [BLOCKBITS]
	property LightLevel: LightLevel;
	property ShadowAimFactor: ShadowAimFactor;
	property ShadowPenaltyFactor: ShadowPenaltyFactor;
	property AutomapOffsets : AutomapOffsets;
	property LandingSpeed: LandingSpeed;
	property MinRespawnTics: MinRespawnTics;
	property RespawnDice: RespawnDice;

	// need some definition work first
	//FRenderStyle RenderStyle;
	native private int RenderStyle;	// This is kept private until its real type has been implemented into the VM. But some code needs to copy this.
	//int ConversationRoot; // THe root of the current dialogue

	// deprecated things.
	native readonly deprecated("2.3", "Use Pos.X instead") double X;
	native readonly deprecated("2.3", "Use Pos.Y instead") double Y;
	native readonly deprecated("2.3", "Use Pos.Z instead") double Z;
	native readonly deprecated("2.3", "Use Vel.X instead") double VelX;
	native readonly deprecated("2.3", "Use Vel.Y instead") double VelY;
	native readonly deprecated("2.3", "Use Vel.Z instead") double VelZ;
	native readonly deprecated("2.3", "Use Vel.X instead") double MomX;
	native readonly deprecated("2.3", "Use Vel.Y instead") double MomY;
	native readonly deprecated("2.3", "Use Vel.Z instead") double MomZ;
	native deprecated("2.3", "Use Scale.X instead") double ScaleX;
	native deprecated("2.3", "Use Scale.Y instead") double ScaleY;

	//FStrifeDialogueNode *Conversation; // [RH] The dialogue to show when this actor is used.;

	Default
	{
		LightLevel -1;
		Scale 1;
		Health DEFAULT_HEALTH;
		Reactiontime 8;
		Radius 20;
		RenderRadius 0;
		Height 16;
		Mass 100;
		RenderStyle 'Normal';
		Alpha 1;
		MinMissileChance 200;
		MissileChanceMult 1.0;
		MeleeRange 64 - MELEEDELTA;
		MaxDropoffHeight 24;
		MaxStepHeight 24;
		MaxSlopeSteepness STEEPSLOPE;
		BounceFactor 0.7;
		WallBounceFactor 0.75;
		BounceCount -1;
		FloatSpeed 4;
		FloatBobPhase -1;	// randomly initialize by default
		FloatBobStrength 1.0;
		FloatBobFactor 1.0;
		Gravity 1;
		Friction 1;
		DamageFactor 1.0;		// damage multiplier as target of damage.
		DamageMultiply 1.0;		// damage multiplier as source of damage.
		PushFactor 0.25;
		WeaveIndexXY 0;
		WeaveIndexZ 16;
		DesignatedTeam 255;
		PainType "Normal";
		DeathType "Normal";
		TeleFogSourceType "TeleportFog";
		TeleFogDestType 'TeleportFog';
		RipperLevel 0;
		RipLevelMin 0;
		RipLevelMax 0;
		RipSound "misc/ripslop";
		DefThreshold 100;
		BloodType "Blood", "BloodSplatter", "AxeBlood";
		ExplosionDamage 128;
		ExplosionRadius -1.0;	// i.e. use ExplosionDamage value
		MissileHeight 32;
		SpriteAngle 0;
		SpriteRotation 0;
		AngledRollOffset 0;
		StencilColor "00 00 00";
		VisibleAngles 0, 0;
		VisiblePitch 0, 0;
		DefaultStateUsage SUF_ACTOR|SUF_OVERLAY;
		CameraHeight int.min;
		CameraFOV 90.f;
		FastSpeed -1;
		RadiusDamageFactor 1;
		SelfDamageFactor 1;
		ShadowAimFactor 1;
		ShadowPenaltyFactor 1;
		AutomapOffsets (0,0);
		StealthAlpha 0;
		WoundHealth 6;
		GibHealth int.min;
		DeathHeight -1;
		BurnHeight -1;
		RenderHidden 0;
		RenderRequired 0;
		FriendlySeeBlocks 10; // 10 (blocks) * 128 (one map unit block)
		LandingSpeed -8; // landing speed from a jump with normal gravity (squats the player's view)
		MinRespawnTics 0; //0 is default value defined in SKILLP_Respawn, negative is seconds
		RespawnDice 4;
	}

	// Functions

	// 'parked' global functions.
	native clearscope static double deltaangle(double ang1, double ang2);
	native clearscope static double absangle(double ang1, double ang2);
	native clearscope static Vector2 AngleToVector(double angle, double length = 1);
	native clearscope static Vector2 RotateVector(Vector2 vec, double angle);
	native clearscope static double Normalize180(double ang);

	virtual void MarkPrecacheSounds()
	{
		MarkSound(SeeSound);
		MarkSound(AttackSound);
		MarkSound(PainSound);
		MarkSound(DeathSound);
		MarkSound(ActiveSound);
		MarkSound(UseSound);
		MarkSound(BounceSound);
		MarkSound(WallBounceSound);
		MarkSound(CrushPainSound);
		MarkSound(HowlSound);
		MarkSound(MeleeSound);
		MarkSound(PushSound);
	}

	bool IsPointerEqual(int ptr_select1, int ptr_select2)
	{
		return GetPointer(ptr_select1) == GetPointer(ptr_select2);
	}

	clearscope static double BobSin(double fb)
	{
		return sin(fb * (180./32)) * 8;
	}

	native version("4.15.1") clearscope Behavior FindBehavior(class<Behavior> type) const;
	native version("4.15.1") bool RemoveBehavior(class<Behavior> type);
	native version("4.15.1") Behavior AddBehavior(class<Behavior> type);
	native version("4.15.1") void TickBehaviors();
	native version("4.15.1") void ClearBehaviors(class<Behavior> type = null);
	native version("4.15.1") void MoveBehaviors(Actor from);

	native clearscope bool isFrozen() const;
	virtual native void BeginPlay();
	virtual native void Activate(Actor activator);
	virtual native void Deactivate(Actor activator);
	virtual native int DoSpecialDamage (Actor target, int damage, Name damagetype, int flags = 0, double angle = 0);
	virtual native int TakeSpecialDamage (Actor inflictor, Actor source, int damage, Name damagetype, int flags = 0, double angle = 0);
	virtual native void Die(Actor source, Actor inflictor, int dmgflags = 0, Name MeansOfDeath = 'none');
	virtual native bool Slam(Actor victim);
	virtual void Touch(Actor toucher) {}
	virtual native void FallAndSink(double grav, double oldfloorz);
	native bool MorphInto(Actor morph);
	native ui void DisplayNameTag();
	native clearscope void DisableLocalRendering(uint playerNum, bool disable);
	native ui bool ShouldRenderLocally(); // Only clients get to check this, never the playsim.

	// Called when the Actor is being used within a PSprite. This happens before potentially changing PSprite
	// state so that any custom actions based on things like player input can be done before moving to the next
	// state of something like a weapon.
	virtual void PSpriteTick(PSprite psp) {}

	// Called by inventory items to see if this actor is capable of touching them.
	// If true, the item will attempt to be picked up. Useful for things like
	// allowing morphs to pick up limited items such as keys while preventing
	// them from picking other items up.
	virtual bool CanTouchItem(Inventory item)
	{
		return true;
	}

	// [AA] Called by inventory items in CallTryPickup to see if this actor needs
	// to process them in some way before they're received. Gets called before
	// the item's TryPickup, allowing fully customized handling of all items.
	virtual bool CanReceive(Inventory item)
	{
		return true;
	}

	// [AA] Called by inventory items at the end of CallTryPickup to let actors
	// do something with the items they've received. 'Item' might be null for
	// items that disappear on pickup.
	// 'itemcls' is passed unconditionally, so it can still be read even if
	// 'item' is null due to being destroyed with GoAwayAndDie() on pickup.
	virtual void HasReceived(Inventory item, class<Inventory> itemcls = null) {}

	// Called in TryMove if the mover ran into another Actor. This isn't called on players
	// if they're currently predicting. Guarantees collisions unlike CanCollideWith.
	virtual void CollidedWith(Actor other, bool passive) {}

	// Called by PIT_CheckThing to check if two actors actually can collide.
	virtual bool CanCollideWith(Actor other, bool passive)
	{
		return true;
	}

	// Called by PIT_CheckLine to check if an actor can cross a line.
	virtual bool CanCrossLine(Line crossing, Vector3 next)
	{
		return true;
	}

	// Called by revival/resurrection to check if one can resurrect the other.
	// "other" can be null when not passive.
	virtual bool CanResurrect(Actor other, bool passive)
	{
		return true;
	}

	// Called after an Actor has been resurrected.
	version("4.15.1") virtual void OnRevive() {}

	// Called when an actor is to be reflected by a disc of repulsion.
	// Returns true to continue normal blast processing.
	virtual bool SpecialBlastHandling (Actor source, double strength)
	{
		return true;
	}

	// This is called before a missile gets exploded.
	virtual int SpecialMissileHit (Actor victim)
	{
		return MHIT_DEFAULT;
	}

	// This is called when a missile bounces off something.
	virtual int SpecialBounceHit(Actor bounceMobj, Line bounceLine, readonly<SecPlane> bouncePlane, bool is3DFloor = false)
	{
		return MHIT_DEFAULT;
	}

	// Called when the player presses 'use' and an actor is found, except if the
	// UseSpecial flag is set. Use level.ExecuteSpecial to call action specials
	// instead.
	virtual bool Used(Actor user)
	{
		return false;
	}

	virtual class<Actor> GetBloodType(int type)
	{
		Class<Actor> bloodcls;
		if (type == 0)
		{
			bloodcls = BloodType;
		}
		else if (type == 1)
		{
			bloodcls = BloodType2;
		}
		else if (type == 2)
		{
			bloodcls = BloodType3;
		}
		else
		{
			return NULL;
		}

		if (bloodcls != NULL)
		{
			bloodcls = GetReplacement(bloodcls);
		}
		return bloodcls;
	}

	virtual int GetGibHealth()
	{
		if (GibHealth != int.min)
		{
			return -abs(GibHealth);
		}
		else
		{
			return -int(GetSpawnHealth() * gameinfo.gibfactor);
		}
	}

	virtual double GetDeathHeight()
	{
		// [RH] Allow the death height to be overridden using metadata.
		double metaheight = -1;
		if (DamageType == 'Fire')
		{
			metaheight = BurnHeight;
		}
		if (metaheight < 0)
		{
			metaheight = DeathHeight;
		}
		if (metaheight < 0)
		{
			return Height * 0.25;
		}
		else
		{
			return MAX(metaheight, 0);
		}
	}

	virtual String GetObituary(Actor victim, Actor inflictor, Name mod, bool playerattack)
	{
		if (mod == 'Telefrag')
		{
			return "$OB_MONTELEFRAG";
		}
		else if (mod == 'Melee' && HitObituary.Length() > 0)
		{
			return HitObituary;
		}
		return Obituary;
	}

	virtual String GetSelfObituary(Actor inflictor, Name mod)
	{
		return SelfObituary;
	}

	virtual int OnDrain(Actor victim, int damage, Name dmgtype)
	{
		return damage;
	}

	// called on getting a secret, return false to disable default "secret found" message/sound
	virtual bool OnGiveSecret(bool printmsg, bool playsound) { return true; }

	// called before and after triggering a teleporter
	// return false in PreTeleport() to cancel the action early
	virtual bool PreTeleport( Vector3 destpos, double destangle, int flags ) { return true; }
	virtual void PostTeleport( Vector3 destpos, double destangle, int flags ) {}

	native virtual bool OkayToSwitchTarget(Actor other);
	native clearscope static class<Actor> GetReplacement(class<Actor> cls);
	native clearscope static class<Actor> GetReplacee(class<Actor> cls);
	native static int GetSpriteIndex(name sprt);
	// [BB] This actor's current sprite frame as a texture, so callers can ask
	// TexMan.GetSize() how big the ARTWORK is. Sizing anything off Height
	// instead measures the collision cylinder, which is a different number and
	// wrong by a lot on tall monsters. rotation is the view angle 0-15.
	native TextureID GetSpriteTextureID(int rotation = 0);
	native clearscope static double GetDefaultSpeed(class<Actor> type);
	native static class<Actor> GetSpawnableType(int spawnnum);
	native clearscope static int ApplyDamageFactors(class<Inventory> itemcls, Name damagetype, int damage, int defdamage);
	native void RemoveFromHash();
	native void ChangeTid(int newtid);
	native bool ActivateSpecial(Actor activator, bool death = false);
	deprecated("3.8", "Use Level.FindUniqueTid() instead") static int FindUniqueTid(int start = 0, int limit = 0)
	{
		return level.FindUniqueTid(start, limit);
	}
	native void SetShade(color col);
	native clearscope int GetRenderStyle() const;
	native clearscope bool CheckKeys(int locknum, bool remote, bool quiet = false);
	protected native void CheckPortalTransition(bool linked = true);
	native clearscope bool HasConversation() const;
	native clearscope bool CanTalk() const;
	native bool StartConversation(Actor player, bool faceTalker = true, bool saveAngle = true, bool rumble = true);

	native clearscope string GetTag(string defstr = "") const;
	native clearscope string GetCharacterName() const;
	native void SetTag(string defstr = "");
	native clearscope double GetBobOffset(double frac = 0) const;
	native void ClearCounters();
	native bool GiveBody (int num, int max=0);
	native bool HitFloor();
	native virtual bool Grind(bool items);
	native clearscope bool isTeammate(Actor other) const;
	native clearscope int PlayerNumber() const;
	native void SetFriendPlayer(PlayerInfo player);
	native void SoundAlert(Actor target, bool splash = false, double maxdist = 0);
	native void ClearBounce();
	native TerrainDef GetFloorTerrain();
	native bool CheckLocalView(int consoleplayer = -1 /* parameter is not used anymore but needed for backward compatibility. */);
	native bool CheckNoDelay();
	native bool UpdateWaterLevel (bool splash = true);
	native bool IsZeroDamage();
	native void ClearInterpolation();
	native void ClearFOVInterpolation();
	native clearscope Vector3 PosRelative(sector sec) const;
	native void RailAttack(FRailParams p);
	native clearscope Name GetDecalName() const;

	native void HandleSpawnFlags();
	native void ExplodeMissile(line lin = null, Actor target = null, bool onsky = false);
	native void RestoreDamage();
	native clearscope int SpawnHealth() const;
	virtual clearscope int GetMaxHealth(bool withupgrades = false) const	// this only exists to make checks for player health easier so they can avoid the type check and just call this method.
	{
		return SpawnHealth();
	}
	native void SetDamage(int dmg);
	native clearscope double Distance2D(Actor other) const;
	native clearscope double Distance3D(Actor other) const;
	native clearscope double Distance2DSquared(Actor other) const;
	native clearscope double Distance3DSquared(Actor other) const;
	native void SetOrigin(vector3 newpos, bool moving);
	native void SetXYZ(vector3 newpos);

	native clearscope Actor GetPointer(int aaptr);
	native double BulletSlope(out FTranslatedLineTarget pLineTarget = null, int aimflags = 0);
	native void CheckFakeFloorTriggers (double oldz, bool oldz_has_viewheight = false);
	native bool CheckFor3DFloorHit(double z, bool trigger);
	native bool CheckFor3DCeilingHit(double z, bool trigger);
	native int CheckMonsterUseSpecials(Line blocking = null);

	native bool CheckMissileSpawn(double maxdist);
	native bool CheckPosition(Vector2 pos, bool actorsonly = false, FCheckPosition tm = null);
	native bool TestMobjLocation();
	native static Actor Spawn(class<Actor> type, vector3 pos = (0,0,0), int replace = NO_REPLACE);
	native static clearscope Actor SpawnClientSide(class<Actor> type, vector3 pos = (0,0,0), int replace = NO_REPLACE);
	native Actor SpawnMissile(Actor dest, class<Actor> type, Actor owner = null);
	native Actor SpawnMissileXYZ(Vector3 pos, Actor dest, Class<Actor> type, bool checkspawn = true, Actor owner = null);
	native Actor SpawnMissileZ (double z, Actor dest, class<Actor> type);
	native Actor SpawnMissileAngleZSpeed (double z, class<Actor> type, double angle, double vz, double speed, Actor owner = null, bool checkspawn = true, int aimflags = 0);
	native Actor SpawnMissileZAimed (double z, Actor dest, Class<Actor> type);
	native Actor SpawnSubMissile(Class<Actor> type, Actor target, int aimflags = 0, double angle = 1e37);
	native Actor, Actor SpawnPlayerMissile(class<Actor> type, double angle = 1e37, double x = 0, double y = 0, double z = 0, out FTranslatedLineTarget pLineTarget = null, bool nofreeaim = false, bool noautoaim = false, int aimflags = 0, double pitch = 1e37);
	native void SpawnTeleportFog(Vector3 pos, bool beforeTele, bool setTarget);
	native Actor RoughMonsterSearch(int distance, bool onlyseekable = false, bool frontonly = false, double fov = 0);
	native clearscope int ApplyDamageFactor(Name damagetype, int damage);
	native int GetModifiedDamage(Name damagetype, int damage, bool passive, Actor inflictor = null, Actor source = null, int flags = 0, double angle = 0.);
	native bool CheckBossDeath();
	native bool CheckFov(Actor target, double fov);

	void A_Light(int extralight) { if (player) player.extralight = clamp(extralight, -20, 20); }
	void A_Light0() { if (player) player.extralight = 0; }
	void A_Light1() { if (player) player.extralight = 1; }
	void A_Light2() { if (player) player.extralight = 2; }
	void A_LightInverse() { if (player) player.extralight = 0x80000000; }

	native Actor OldSpawnMissile(Actor dest, class<Actor> type, Actor owner = null);
	native Actor SpawnPuff(class<Actor> pufftype, vector3 pos, double hitdir, double particledir, int updown, int flags = 0, Actor victim = null);

	// [HITCALLBACKS] GZSelaco c7527eead1, 90bdbba77c: hitscans (LineAttack) and rails report to their puff. The engine
	// calls these only on a puff class that overrides them; the native versions do nothing.
	// PuffSplash: the attack crossed water. Return true when you handled the splash, and the engine makes none. A
	// hitscan puff class that overrides this is called on a temporary puff of its type at the water (in its Splash
	// state, if it has one), which may be destroyed right after the call.
	virtual native bool PuffSplash(Vector3 position, Vector3 direction, Sector sect, F3DFloor floor3D);
	// PuffHit: the attack stopped at a wall, floor or ceiling (not an actor), described as a LineTrace result.
	virtual native void PuffHit(FLineTraceData trace);
	// PuffThrough: the attack went through victim and hurt it: a HITSCANTHRU actor for a hitscan (on a puff in its
	// HitThrough state, often temporary), every pierced actor for a rail.
	virtual native void PuffThrough(Actor victim, Vector3 pos, Vector3 dir);

	native Actor SpawnBlood (Vector3 pos1, double dir, int damage);
	native void BloodSplatter (Vector3 pos, double hitangle, bool axe = false);
	native bool HitWater (sector sec, Vector3 pos, bool checkabove = false, bool alert = true, bool force = false, int flags = 0);
	native void PlaySpawnSound(Actor missile);
	native clearscope bool CountsAsKill() const;

	native bool Teleport(Vector3 pos, double angle, int flags);
	native void TraceBleed(int damage, Actor missile);
	native void TraceBleedAngle(int damage, double angle, double pitch);

	native void SetIdle(bool nofunction = false);
	native bool CheckMeleeRange(double range = -1);
	native bool TriggerPainChance(Name mod, bool forcedPain = false);
	native virtual int DamageMobj(Actor inflictor, Actor source, int damage, Name mod, int flags = 0, double angle = 0);
	native virtual bool ReactToDamage(Actor inflictor, Actor source, int damage, Name mod, int flags, int originaldamage);
	native void PoisonMobj (Actor inflictor, Actor source, int damage, int duration, int period, Name type);
	native double AimLineAttack(double angle, double distance, out FTranslatedLineTarget pLineTarget = null, double vrange = 0., int flags = 0, Actor target = null, Actor friender = null);
	native Actor, int LineAttack(double angle, double distance, double pitch, int damage, Name damageType, class<Actor> pufftype, int flags = 0, out FTranslatedLineTarget victim = null, double offsetz = 0., double offsetforward = 0., double offsetside = 0.);
	native bool LineTrace(double angle, double distance, double pitch, int flags = 0, double offsetz = 0., double offsetforward = 0., double offsetside = 0., out FLineTraceData data = null);
	native bool CheckSight(Actor target, int flags = 0);
	native bool IsVisible(Actor other, bool allaround, LookExParams params = null);
	native bool, Actor, double PerformShadowChecks (Actor other, Vector3 pos);
	native bool HitFriend();
	native bool MonsterMove();

	native SeqNode StartSoundSequenceID (int sequence, int type, int modenum, bool nostop = false);
	native SeqNode StartSoundSequence (Name seqname, int modenum);
	native void StopSoundSequence();

	native void FindFloorCeiling(int flags = 0);
	native double, double GetFriction();
	native bool, Actor TestMobjZ(bool quick = false);
	native clearscope static bool InStateSequence(State newstate, State basestate);

	bool TryWalk ()
	{
		if (!MonsterMove ())
		{
			return false;
		}
		movecount = random[TryWalk](0, 15);
		return true;
	}

	native bool TryMove(vector2 newpos, int dropoff, bool missilecheck = false, FCheckPosition tm = null);
	native bool CheckMove(vector2 newpos, int flags = 0, FCheckPosition tm = null);
	native void NewChaseDir();
	native void RandomChaseDir();
	native bool CheckMissileRange();
	native bool SetState(state st, bool nofunction = false);
	clearscope native state FindState(statelabel st, bool exact = false) const;
	clearscope native state FindStateByString(string st, bool exact = false) const;
	bool SetStateLabel(statelabel st, bool nofunction = false) { return SetState(FindState(st), nofunction); }
	native action state ResolveState(statelabel st);	// this one, unlike FindState, is context aware.
	native void LinkToWorld(LinkContext ctx = null);
	native void UnlinkFromWorld(out LinkContext ctx = null);
	native bool CanSeek(Actor target);
	native clearscope double AngleTo(Actor target, bool absolute = false) const;
	native void AddZ(double zadd, bool moving = true);
	native void SetZ(double z);
	native clearscope vector2 Vec2To(Actor other) const;
	native clearscope vector3 Vec3To(Actor other) const;
	native clearscope vector3 Vec3Offset(double x, double y, double z, bool absolute = false) const;
	native clearscope vector3 Vec3Angle(double length, double angle, double z = 0, bool absolute = false) const;
	native clearscope vector2 Vec2Angle(double length, double angle, bool absolute = false) const;
	native clearscope vector2 Vec2Offset(double x, double y, bool absolute = false) const;
	native clearscope vector3 Vec2OffsetZ(double x, double y, double atz, bool absolute = false) const;
	native double PitchFromVel();
	native void VelIntercept(Actor targ, double speed = -1, bool aimpitch = true, bool oldvel = false, bool resetvel = false);
	native void VelFromAngle(double speed = 1e37, double angle = 1e37);
	native void Vel3DFromAngle(double speed, double angle, double pitch);
	native vector3 AttackDir(Actor actor, double angle, double pitch);
	native vector3 OffhandDir(Actor actor, double angle, double pitch);
	native void Thrust(double speed = 1e37, double angle = 1e37);
	native clearscope bool isFriend(Actor other) const;
	native clearscope bool isHostile(Actor other) const;
	native clearscope bool ShouldPassThroughPlayer(Actor other) const;
	native void AdjustFloorClip();
	native clearscope DropItem GetDropItems() const;
	native void CopyFriendliness (Actor other, bool changeTarget, bool resetHealth = true);
	native bool LookForMonsters();
	native bool LookForTid(bool allaround, LookExParams params = null);
	native bool LookForEnemies(bool allaround, LookExParams params = null);
	native bool LookForPlayers(bool allaround, LookExParams params = null);
	native int LookForEnemiesEx(out Array<Actor> targets, double range = -1, bool noPlayers = true, bool allaround = false, LookExParams params = null);
	native bool TeleportMove(Vector3 pos, bool telefrag, bool modifyactor = true);
	native clearscope double DistanceBySpeed(Actor other, double speed) const;
	native name GetSpecies();
	native void PlayActiveSound();
	native void Howl();
	native void DrawSplash (int count, double angle, int kind);
	native void GiveSecret(bool printmsg = true, bool playsound = true);
	native clearscope double GetCameraHeight() const;
	native clearscope double GetGravity() const;
	native void DoMissileDamage(Actor target);
	native void PlayPushSound();
	native bool BounceActor(Actor blocking, bool onTop);
	native bool BounceWall(Line l = null);
	native bool BouncePlane(readonly<SecPlane> plane, bool is3DFloor = false);
	native void PlayBounceSound(bool onFloor, double volume = 1.0);
	native bool ReflectOffActor(Actor blocking);

	clearscope double PitchTo(Actor target, double zOfs = 0, double targZOfs = 0, bool absolute = false) const
	{
		Vector3 origin = (pos.xy, pos.z - floorClip + zOfs);
		Vector3 dest = (target.pos.xy, target.pos.z - target.floorClip + targZOfs);

		Vector3 diff;
		if (!absolute)
			diff = level.Vec3Diff(origin, dest);
		else
			diff = dest - origin;

		return -atan2(diff.z, diff.xy.Length());
	}

	//==========================================================================
	//
	// AActor :: GetLevelSpawnTime
	//
	// Returns the time when this actor was spawned,
	// relative to the current level.
	//
	//==========================================================================

	clearscope int GetLevelSpawnTime() const
	{
		return SpawnTime - level.totaltime + level.time;
	}

	//==========================================================================
	//
	// AActor :: GetAge
	//
	// Returns the number of ticks passed since this actor was spawned.
	//
	//==========================================================================

	clearscope int GetAge() const
	{
		return level.totaltime - SpawnTime;
	}

	clearscope double AccuracyFactor() const
	{
		return 1. / (1 << (accuracy * 5 / 100));
	}

	protected native void DestroyAllInventory();	// This is not supposed to be called by user code!
	native clearscope Inventory FindInventory(class<Inventory> itemtype, bool subclass = false) const;
	native Inventory GiveInventoryType(class<Inventory> itemtype);
	native bool UsePuzzleItem(int PuzzleItemType);

	action native void SetCamera(Actor cam, bool revert = false);
	native bool Warp(Actor dest, double xofs = 0, double yofs = 0, double zofs = 0, double angle = 0, int flags = 0, double heightoffset = 0, double radiusoffset = 0, double pitch = 0);

	// DECORATE compatible functions
	native double GetZAt(double px = 0, double py = 0, double angle = 0, int flags = 0, int pick_pointer = AAPTR_DEFAULT);
	native clearscope int GetSpawnHealth() const;
	native double GetCrouchFactor(int ptr = AAPTR_PLAYER1);
	native double GetCVar(string cvar);
	native string GetCVarString(string cvar);
	native int GetPlayerInput(int inputnum, int ptr = AAPTR_DEFAULT);
	native int CountProximity(class<Actor> classname, double distance, int flags = 0, int ptr = AAPTR_DEFAULT);
	native int GetMissileDamage(int mask, int add, int ptr = AAPTR_DEFAULT);
	native int CalculateMissileDamage();	// [ABSDAMAGE] GZSelaco 96096c0228: the fixed damage value (DamageVal); negative for a damage expression
	action native int OverlayID();
	action native double OverlayX(int layer = 0);
	action native double OverlayY(int layer = 0);
	action native double OverlayAlpha(int layer = 0);

	// DECORATE setters - it probably makes more sense to set these values directly now...
	void A_SetMass(int newmass) { mass = newmass; }
	void A_SetInvulnerable() { bInvulnerable = true; }
	void A_UnSetInvulnerable() { bInvulnerable = false; }
	void A_SetReflective() { bReflective = true; }
	void A_UnSetReflective() { bReflective = false; }
	void A_SetReflectiveInvulnerable() { bInvulnerable = true; bReflective = true; }
	void A_UnSetReflectiveInvulnerable() { bInvulnerable = false; bReflective = false; }
	void A_SetShootable() { bShootable = true; bNonShootable = false; }
	void A_UnSetShootable() { bShootable = false; bNonShootable = true; }
	void A_NoGravity() { bNoGravity = true; }
	void A_Gravity() { bNoGravity = false; Gravity = 1; }
	void A_LowGravity() { bNoGravity = false; Gravity = 0.125; }
	void A_SetGravity(double newgravity) { gravity = clamp(newgravity, 0., 10.); }
	void A_SetFloorClip() { bFloorClip = true; AdjustFloorClip(); }
	void A_UnSetFloorClip() { bFloorClip = false;  FloorClip = 0; }
	void A_HideThing() { bInvisible = true; }
	void A_UnHideThing() { bInvisible = false; }
	void A_SetArg(int arg, int val) { if (arg >= 0 && arg < 5) args[arg] = val;	}
	void A_Turn(double turn = 0) { angle += turn; }
	void A_SetDamageType(name newdamagetype) { damagetype = newdamagetype; }
	void A_SetSolid() { bSolid = true; }
	void A_UnsetSolid() { bSolid = false; }
	void A_SetFloat() { bFloat = true; }
	void A_UnsetFloat() { bFloat = false; }
	void A_SetFloatBobPhase(int bob) { if (bob >= 0 && bob <= 63) FloatBobPhase = bob; }
	void A_SetRipperLevel(int level) { RipperLevel = level; }
	void A_SetRipMin(int minimum) { RipLevelMin = minimum; }
	void A_SetRipMax(int maximum) { RipLevelMax = maximum; }
	void A_ScreamAndUnblock() { A_Scream(); A_NoBlocking(); }
	void A_ActiveAndUnblock() { A_ActiveSound(); A_NoBlocking(); }

	//---------------------------------------------------------------------------
	//
	// FUNC P_SpawnMissileAngle
	//
	// Returns NULL if the missile exploded immediately, otherwise returns
	// a mobj_t pointer to the missile.
	//
	//---------------------------------------------------------------------------

	Actor SpawnMissileAngle (class<Actor> type, double angle, double vz)
	{
		return SpawnMissileAngleZSpeed (pos.z + 32 + GetBobOffset(), type, angle, vz, GetDefaultSpeed (type));
	}

	Actor SpawnMissileAngleZ (double z, class<Actor> type, double angle, double vz, Actor owner = null)
	{
		return SpawnMissileAngleZSpeed (z, type, angle, vz, GetDefaultSpeed (type), owner);
	}

	void A_SetScale(double scalex, double scaley = 0, int ptr = AAPTR_DEFAULT, bool usezero = false)
	{
		Actor aptr = GetPointer(ptr);
		if (aptr)
		{
			aptr.Scale.X = scalex;
			aptr.Scale.Y = scaley != 0 || usezero? scaley : scalex;	// use scalex here, too, if only one parameter.
		}
	}
	void A_SetSpeed(double speed, int ptr = AAPTR_DEFAULT)
	{
		Actor aptr = GetPointer(ptr);
		if (aptr) aptr.Speed = speed;
	}
	void A_SetFloatSpeed(double speed, int ptr = AAPTR_DEFAULT)
	{
		Actor aptr = GetPointer(ptr);
		if (aptr) aptr.FloatSpeed = speed;
	}
	void A_SetPainThreshold(int threshold, int ptr = AAPTR_DEFAULT)
	{
		Actor aptr = GetPointer(ptr);
		if (aptr) aptr.PainThreshold = threshold;
	}
	bool A_SetSpriteAngle(double angle = 0, int ptr = AAPTR_DEFAULT)
	{
		Actor aptr = GetPointer(ptr);
		if (!aptr) return false;
		aptr.SpriteAngle = angle;
		return true;
	}
	bool A_SetSpriteRotation(double angle = 0, int ptr = AAPTR_DEFAULT)
	{
		Actor aptr = GetPointer(ptr);
		if (!aptr) return false;
		aptr.SpriteRotation = angle;
		return true;
	}

	deprecated("2.3", "This function does nothing and is only for Zandronum compatibility") void A_FaceConsolePlayer(double MaxTurnAngle = 0) {}

	void A_SetSpecial(int spec, int arg0 = 0, int arg1 = 0, int arg2 = 0, int arg3 = 0, int arg4 = 0)
	{
		special = spec;
		args[0] = arg0;
		args[1] = arg1;
		args[2] = arg2;
		args[3] = arg3;
		args[4] = arg4;
	}

	void A_ClearTarget()
	{
		target = null;
		lastheard = null;
		lastenemy = null;
	}

	void A_ChangeLinkFlags(int blockmap = FLAG_NO_CHANGE, int sector = FLAG_NO_CHANGE)
	{
		UnlinkFromWorld();
		if (blockmap != FLAG_NO_CHANGE) bNoBlockmap = blockmap;
		if (sector != FLAG_NO_CHANGE) bNoSector = sector;
		LinkToWorld();
	}

	// killough 11/98: kill an object
	void A_Die(name damagetype = "none")
	{
		DamageMobj(null, null, health, damagetype, DMG_FORCED);
	}

	void SpawnDirt (double radius)
	{
		static const class<Actor> chunks[] = { "Dirt1", "Dirt2", "Dirt3", "Dirt4", "Dirt5", "Dirt6" };
		double zo = random[Dirt]() / 128. + 1;
		Vector3 pos = Vec3Angle(radius, random[Dirt]() * (360./256), zo);

		Actor mo = Spawn (chunks[random[Dirt](0, 5)], pos, ALLOW_REPLACE);
		if (mo)
		{
			mo.Vel.Z = random[Dirt]() / 64.;
		}
	}

	//
	// A_SinkMobj
	// Sink a mobj incrementally into the floor
	//

	bool SinkMobj (double speed)
	{
		if (Floorclip < Height)
		{
			Floorclip += speed;
			return false;
		}
		return true;
	}

	//
	// A_RaiseMobj
	// Raise a mobj incrementally from the floor to
	//

	bool RaiseMobj (double speed)
	{
		// Raise a mobj from the ground
		if (Floorclip > 0)
		{
			Floorclip -= speed;
			if (Floorclip <= 0)
			{
				Floorclip = 0;
				return true;
			}
			else
			{
				return false;
			}
		}
		return true;
	}

	Actor AimTarget()
	{
		FTranslatedLineTarget t;
		BulletSlope(t, ALF_PORTALRESTRICT);
		return t.linetarget;
	}

	void RestoreRenderStyle()
	{
		bShadow = default.bShadow;
		bGhost = default.bGhost;
		RenderStyle = default.RenderStyle;
		Alpha = default.Alpha;
	}

	virtual bool ShouldSpawn()
	{
		return true;
	}

	native void A_Face(Actor faceto, double max_turn = 0, double max_pitch = 270, double ang_offset = 0, double pitch_offset = 0, int flags = 0, double z_ofs = 0);

	void A_FaceTarget(double max_turn = 0, double max_pitch = 270, double ang_offset = 0, double pitch_offset = 0, int flags = 0, double z_ofs = 0)
	{
		A_Face(target, max_turn, max_pitch, ang_offset, pitch_offset, flags, z_ofs);
	}
	void A_FaceTracer(double max_turn = 0, double max_pitch = 270, double ang_offset = 0, double pitch_offset = 0, int flags = 0, double z_ofs = 0)
	{
		A_Face(tracer, max_turn, max_pitch, ang_offset, pitch_offset, flags, z_ofs);
	}
	void A_FaceMaster(double max_turn = 0, double max_pitch = 270, double ang_offset = 0, double pitch_offset = 0, int flags = 0, double z_ofs = 0)
	{
		A_Face(master, max_turn, max_pitch, ang_offset, pitch_offset, flags, z_ofs);
	}

	// Action functions
	// Meh, MBF redundant functions. Only for DeHackEd support.
	native bool A_LineEffect(int boomspecial = 0, int tag = 0);
	// End of MBF redundant functions.

	native void A_MonsterRail();
	native void A_Pain();
	native void A_NoBlocking(bool drop = true);
	void A_Fall() { A_NoBlocking(); }
	native void A_Look();
	native void A_Chase(statelabel melee = '_a_chase_default', statelabel missile = '_a_chase_default', int flags = 0);
	native void A_DoChase(State melee, State missile, int flags = 0);
	native void A_VileChase();
	native bool A_CheckForResurrection(State state = null, Sound snd = 0);
	native void A_BossDeath();
	bool A_CallSpecial(int special, int arg1=0, int arg2=0, int arg3=0, int arg4=0, int arg5=0)
	{
		return Level.ExecuteSpecial(special, self, null, false, arg1, arg2, arg3, arg4, arg5);
	}

	native void A_FastChase();
	native void A_PlayerScream();
	native void A_CheckTerrain();

	native void A_Wander(int flags = 0);
	native void A_Look2();

	deprecated("2.3", "Use A_CustomBulletAttack() instead") native void A_BulletAttack();
	native void A_WolfAttack(int flags = 0, sound whattoplay = "weapons/pistol", double snipe = 1.0, int maxdamage = 64, int blocksize = 128, int pointblank = 2, int longrange = 4, double runspeed = 160.0, class<Actor> pufftype = "BulletPuff");
	deprecated("4.3", "Use A_StartSound() instead") action native clearscope void A_PlaySound(sound whattoplay = "weapons/pistol", int slot = CHAN_BODY, double volume = 1.0, bool looping = false, double attenuation = ATTN_NORM, bool local = false, double pitch = 0.0);
	action native clearscope void A_StartSound(sound whattoplay, int slot = CHAN_BODY, int flags = 0, double volume = 1.0, double attenuation = ATTN_NORM, double pitch = 0.0, double startTime = 0.0);
	action native clearscope void A_StartSoundIfNotSame(sound whattoplay, sound checkagainst, int slot = CHAN_BODY, int flags = 0, double volume = 1.0, double attenuation = ATTN_NORM, double pitch = 0.0, double startTime = 0.0);
	// [SOUNDHANDLES] A_StartSound returning the sound's handle (engine/base.zs SoundHandleStruct). Not an action function, so a
	// weapon state's CHAN_WEAPON is not moved to CHAN_OFFWEAPON for the off hand as A_StartSound does. GZSelaco's signature.
	native clearscope SoundHandle StartSound(sound whattoplay, int slot = CHAN_BODY, int flags = 0, double volume = 1.0, double attenuation = ATTN_NORM, double pitch = 0.0, double startTime = 0.0);
	action native void A_SoundVolume(int slot, double volume);
	action native void A_SoundPitch(int slot, double pitch);
	deprecated("2.3", "Use A_StartSound(<sound>, CHAN_WEAPON) instead") void A_PlayWeaponSound(sound whattoplay, bool fullvol = false) { A_StartSound(whattoplay, CHAN_WEAPON, 0, 1, fullvol? ATTN_NONE : ATTN_NORM); }
	action native void A_StopSound(int slot = CHAN_VOICE);	// Bad default but that's what is originally was...
	void A_StopAllSounds()	{	A_StopSounds(0,0);	}
	native void A_StopSounds(int chanmin, int chanmax);
	deprecated("2.3", "Use A_StartSound() instead") native void A_PlaySoundEx(sound whattoplay, name slot, bool looping = false, int attenuation = 0);
	deprecated("2.3", "Use A_StopSound() instead") native void A_StopSoundEx(name slot);
	native clearscope bool IsActorPlayingSound(int channel, Sound snd = -1);
	native void A_SeekerMissile(int threshold, int turnmax, int flags = 0, int chance = 50, int distance = 10);
	native action state A_Jump(int chance, statelabel label, ...);
	native Actor A_SpawnProjectile(class<Actor> missiletype, double spawnheight = 32, double spawnofs_xy = 0, double angle = 0, int flags = 0, double pitch = 0, int ptr = AAPTR_TARGET);
	native void A_CustomRailgun(int damage, int spawnofs_xy = 0, color color1 = 0, color color2 = 0, int flags = 0, int aim = 0, double maxdiff = 0, class<Actor> pufftype = "BulletPuff", double spread_xy = 0, double spread_z = 0, double range = 0, int duration = 0, double sparsity = 1.0, double driftspeed = 1.0, class<Actor> spawnclass = null, double spawnofs_z = 0, int spiraloffset = 270, int limit = 0, double veleffect = 3);
	native void A_Print(string whattoprint, double time = 0, name fontname = "none");
	native void A_PrintBold(string whattoprint, double time = 0, name fontname = "none");
	native void A_Log(string whattoprint, bool local = false);
	native void A_LogInt(int whattoprint, bool local = false);
	native void A_LogFloat(double whattoprint, bool local = false);
	native void A_SetTranslucent(double alpha, int style = 0);
	native void A_SetRenderStyle(double alpha, int style);
	native void A_FadeIn(double reduce = 0.1, int flags = 0);
	native void A_FadeOut(double reduce = 0.1, int flags = 1); //bool remove == true
	native void A_FadeTo(double target, double amount = 0.1, int flags = 0);
	native void A_SpawnDebris(class<Actor> spawntype, bool transfer_translation = false, double mult_h = 1, double mult_v = 1);
	native void A_SpawnParticle(color color1, int flags = 0, int lifetime = TICRATE, double size = 1, double angle = 0, double xoff = 0, double yoff = 0, double zoff = 0, double velx = 0, double vely = 0, double velz = 0, double accelx = 0, double accely = 0, double accelz = 0, double startalphaf = 1, double fadestepf = -1, double sizestep = 0);
	native void A_SpawnParticleEx(color color1, TextureID texture, int style = STYLE_None, int flags = 0, int lifetime = TICRATE, double size = 1, double angle = 0, double xoff = 0, double yoff = 0, double zoff = 0, double velx = 0, double vely = 0, double velz = 0, double accelx = 0, double accely = 0, double accelz = 0, double startalphaf = 1, double fadestepf = -1, double sizestep = 0, double startroll = 0, double rollvel = 0, double rollacc = 0, double fadeoutstepf = 0);
	native void A_ExtChase(bool usemelee, bool usemissile, bool playactive = true, bool nightmarefast = false);
	native void A_DropInventory(class<Inventory> itemtype, int amount = -1);
	native void A_SetBlend(color color1, double alpha, int tics, color color2 = 0, double alpha2 = 0.);
	deprecated("2.3", "Use 'b<FlagName> = [true/false]' instead") native void A_ChangeFlag(string flagname, bool value);
	native void A_ChangeCountFlags(int kill = FLAG_NO_CHANGE, int item = FLAG_NO_CHANGE, int secret = FLAG_NO_CHANGE);
	action native void A_ChangeModel(name modeldef, int modelindex = 0, string modelpath = "", name model = "", int skinindex = 0, string skinpath = "", name skin = "", int flags = 0, int generatorindex = -1, int animationindex = 0, string animationpath = "", name animation = "");

	// RS FORK -- native state remap (FORK_CHANGES.md, "Native state remap").
	// After A_ChangeModel has bound a model, register which mesh frame each
	// of this actor's states should display; the renderer then resolves
	// frames against the psprite's own current state natively, with no
	// per-tick script. Registration fails (returns false) until modelData
	// exists, i.e. call A_ChangeModel first.
	native bool RegisterModelStateFrame(State st, int frameNum, int frameNext);

	// ---- ONE WRITER PER SURFACE. READ THIS BEFORE USING THE CALLS BELOW ----
	//
	// A surface can be driven by exactly ONE slot. If two slots name the same
	// (modelindex, surface), the LOWEST SLOT NUMBER is drawn and the other is
	// ignored -- silently, every frame, deterministically. It is not "last
	// writer wins" and it is not a race; do not go looking for one.
	//
	// THE VALUES DO NOT ADD UP. There is no base-plus-delta channel. If two
	// things want to move one part at once -- a recoil kick on top of a slide
	// your hand is already dragging, or a cylinder that both steps round and
	// swings out -- you must add them together yourself and write the single
	// combined result through ONE slot. Reaching for a second slot to layer a
	// second motion looks like it should work and quietly does nothing.
	//
	// Slots are per actor and there are 16 of them.
	//
	// Drive one SURFACE of this actor's model. The psprite equivalent is a set
	// of fields; DActorModelData is not exposed to script, so a world model
	// goes through these.
	//
	// pos IS A FRACTIONAL FRAME INDEX -- 27.4 is 40% of the way from mesh frame
	// 27 to 28. Not map units, not 0..1. Nothing downstream can catch this
	// being wrong; it only ever shows up as motion that looks slightly off.
	//
	// Needs A_ChangeModel to have run on this actor first.
	native bool SetModelSurfacePos(int slot, int modelindex, int surface, double pos);
	native bool SetModelSurfaceHidden(int slot, int modelindex, int surface, bool hidden);
	// Show a surface partway toward a second texture: amount 0 is the surface as it
	// is, 1 is fully `skin`. Same slots as SetModelSurfaceHidden. Render-only.
	native bool SetModelSurfaceBlend(int slot, int modelindex, int surface, TextureID skin, double amount);

	// A LIVE TRANSFORM ON TOP OF THE FRAME.
	//
	// SetModelSurfacePos picks a POSE -- a baked snapshot, and nothing else --
	// so a part driven by frames alone can only be where the author baked it.
	// That is why a hand-driven part and a frame-driven part can never quite
	// agree, and why every handoff between the two has a seam. Blending
	// adjacent frames narrows the seam; it cannot remove it.
	//
	// Every part this is used on is rigid: it never changes shape as it moves,
	// only position. So this gives a surface a position independent of its
	// shape -- what one bone would do for it, if MD3 had bones.
	//
	// The offset is in the MODEL's own axes, so it stays right however the
	// weapon is held. ADDITIVE with the frame, not instead of it: the frame
	// still chooses the pose, this moves it. rot is a quaternion (identity =
	// (0,0,0,1) = no rotation). ClearModelSurfaceOffset stops transforming a
	// slot and costs the renderer nothing thereafter.
	native bool SetModelSurfaceOffset(int slot, int modelindex, int surface, Vector3 ofs, Quat rot);
	native bool ClearModelSurfaceOffset(int slot);

	// ---- DRIVE A PART FROM YOUR HAND, AT DISPLAY RATE --------------------
	//
	// Everything above is written by script, 35 times a second, and smoothed to
	// the drawn frame. Smooth is not the same as GLUED: the value being smoothed
	// toward is still a tic old, so a part you are dragging trails your hand no
	// matter how nicely it gets there. On a fast reload snatch that is
	// centimetres of lag, and it is exactly what a slow test cannot show you.
	//
	// A DRIVEN part skips script entirely. The renderer reads your controller on
	// the frame it is drawing and places the part from it, so the part and your
	// hand come from the same pose at the same instant and cannot separate.
	//
	// hand: 0 main, 1 off. axis and distance are in the MESH's own units -- the
	// same ones its vertices use. There is no scale constant anywhere in this
	// path; the conversion is derived from the model's own matrix, so there is
	// nothing to remember and nothing to get wrong.
	//
	// startValue resumes from where the part already is, so taking hold of a
	// half-open bolt does not snap it shut or fling it open.
	//
	// Call ClearModelSurfaceDrive when the hand lets go -- without it the part
	// keeps following, and "working" and "stuck on" look identical.
	native bool SetModelSurfaceDrive(int slot, int modelindex, int surface, int hand, Vector3 axis, double distance, double startValue);
	native bool ClearModelSurfaceDrive(int slot);

	// AND TURN AS IT GOES. Call after SetModelSurfaceDrive on the same slot
	// (which resets it to a pure slide). At drive value v the part turns
	// v * degrees about `axis` through `pivot`, then slides -- a magazine that
	// rocks into its well, a bolt that lifts as it draws. Same space and sense
	// as Quat.AxisAngle given to SetModelSurfaceOffset, so a part posed at rest
	// and the same part in the hand agree at every value. degrees 0 turns it
	// off. Refused on a slot that is not being driven.
	native bool SetModelSurfaceDriveRotation(int slot, Vector3 axis, double degrees, Vector3 pivot);

	// A PART THAT ONLY TURNS, DRIVEN BY YOUR HAND. The sibling of
	// SetModelSurfaceDrive for a lever, a handle, a latch or a barrel on its
	// hinge. At value v the part turns v * degrees about `axis` through `pivot`
	// and does not slide. Your hand is read by its ANGLE round that line, not
	// along a straight axis, so the part turns exactly as far as your hand
	// swings round the pivot, however far out you hold it. |degrees| must be
	// under 180. hand, startValue and ClearModelSurfaceDrive work as they do for
	// SetModelSurfaceDrive. SetModelSurfaceDriveRotation is refused on a hinge
	// drive, because the hinge IS its turn.
	native bool SetModelSurfaceDriveHinge(int slot, int modelindex, int surface, int hand, Vector3 axis, double degrees, Vector3 pivot, double startValue);

	// THEN A SECOND MOTION, IN THE SAME PULL. Call after SetModelSurfaceDrive or
	// SetModelSurfaceDriveHinge on the same slot, which becomes stage 1. The
	// slot's one drawn value 0..1 is then [0, split] for stage 1 and [split, 1]
	// for stage 2. Stage 1 is held fully applied through all of stage 2: a
	// bolt's handle stays up while it draws back.
	//
	// One hand drives both, each along its OWN direction of travel. Stage 2
	// cannot move until stage 1 is complete, and stage 1 cannot move while
	// stage 2 is under way, which is the L-shaped track a bolt runs in. Pushing
	// back past the split returns to stage 1. The renderer does the handoff on
	// the frame it is drawing, so nothing snaps at the corner, even on a pull
	// fast enough to cross it inside one frame.
	//
	// kind: DRIVESTAGE_Slide (amount = mesh units along axis, pivot ignored) or
	// DRIVESTAGE_Hinge (amount = degrees about axis through pivot, |amount| under
	// 180). DRIVESTAGE_None removes the second stage. axis and pivot are in the
	// mesh's own space where the part stands once stage 1 is complete.
	//
	// The drive's startValue is read as the COMBINED value, so a bolt taken
	// hold of while open resumes open. GetModelSurfaceDrawnValue returns the
	// combined value, and ClearModelSurfaceDrive removes the stage too. Refused
	// on a slot that is not being driven, for split outside [0.001, 0.999], and
	// for any NaN or infinite axis, amount or pivot.
	enum EModelSurfaceDriveStage
	{
		DRIVESTAGE_None  = 0,
		DRIVESTAGE_Slide = 1,
		DRIVESTAGE_Hinge = 2,
	};
	native bool SetModelSurfaceDriveStage(int slot, int kind, Vector3 axis, double amount, Vector3 pivot, double split);

	// WHAT WAS DRAWN, 0..1. Read this rather than trusting script's own estimate:
	// script runs at 35Hz and the renderer draws at 90+, so on fast motion they
	// are different numbers, and deciding gameplay on the one that ISN'T on
	// screen is how you get a magazine that seats while visibly still out.
	native double GetModelSurfaceDrawnValue(int slot);

	// Translate only, which is what a slide, a magazine and a pump all want.
	// Spelled out so the common case does not have to name an identity
	// quaternion it does not care about.
	bool SetModelSurfaceShift(int slot, int modelindex, int surface, Vector3 ofs)
	{
		return SetModelSurfaceOffset(slot, modelindex, surface, ofs, Quat(0, 0, 0, 1));
	}
	native void ClearModelSurfaces();
	native void ClearModelStateFrames();

	// ---- ASK THE MESH WHAT ITS PARTS ARE CALLED --------------------------
	//
	// The surface drivers above take an INDEX, which is a fact about how the
	// mesh happened to be exported rather than about the weapon. Re-export
	// with the surfaces in another order and every hardcoded index is quietly
	// pointing at the wrong part. These let a part map be written against
	// NAMES, which survive that -- and which are what a person actually knows
	// about a gun ("the slide") in the first place.
	//
	// Case-insensitive: 'slide' finds a surface exported as "Slide".
	// -1 / 0 / None when there is no such surface, no such model index, or no
	// model at all. Works on plain MD3 props -- unlike the bone entry points,
	// these do not require decoupled animations, because a surface is not a
	// bone and a weapon prop usually has no skeleton whatsoever.
	//
	// ON A RIGGED IQM each mesh is a surface, named as in the file (surface index = mesh index). Only
	// SetModelSurfaceHidden applies to one -- hide a rig's own arms mesh, or an attachment. Its meshes are
	// placed by their bones, so SetModelSurfacePos, SetModelSurfaceOffset and the surface drives do nothing
	// there: move a rigged part by its joint.
	native int  FindModelSurfaceIndex(int modelindex, Name surface);
	native int  GetModelSurfaceCount(int modelindex);
	native Name GetModelSurfaceName(int modelindex, int surface);

	// How many poses the mesh has, so a frame number can be checked against
	// the model instead of against a memory of it. -1 if the format does not
	// know its own count.
	native int  GetModelFrameCount(int modelindex);

	// WHICH MODEL `modelindex` NAMES, for every query here: the model A_ChangeModel set at that index; else the class's
	// MODELDEF BaseFrame block; else (RS fork) the class's MODELDEF block for the sprite and frame the actor is in --
	// the block the renderer draws it from -- unless the actor has decoupled animations, which draw from BaseFrame
	// alone. So a part on model 1 or 2 of an ordinary MODELDEF actor is found by name. Game state only: no render
	// setting and no voxel changes the answer, so it is the same on every machine.

	// JOINTS BY MODEL INDEX. FindBoneIndex asks model 0 only and needs decoupled animations and a BaseFrame; these ask
	// any model index of any actor with a model, as the surface queries do, and change nothing on the actor.
	// Case-insensitive. -1 / 0 / None when that model has no such joint, there is no model at that index, or its
	// format has no joints (only a rigged IQM has any).
	native int  FindModelJointIndex(int modelindex, Name joint);
	native int  GetModelJointCount(int modelindex);
	native Name GetModelJointName(int modelindex, int joint);

	// THE SAME QUESTIONS ASKED OF A CLASS, with no actor: for a data check that runs before any level exists
	// (DataValidator), or anything that wants a class's model without spawning one. The model the class's MODELDEF
	// gives that index -- its BaseFrame block, else the block for its spawn state's sprite and frame: what the actor
	// queries answer for a fresh actor of the class that has changed none of its models.
	// GetClassModelFile is that model's file as MODELDEF named it, path included ("models/pistols/pistolet.md3");
	// "" when there is none.
	native clearscope static String GetClassModelFile(class<Actor> cls, int modelindex);
	native clearscope static int    GetClassModelSurfaceCount(class<Actor> cls, int modelindex);
	native clearscope static Name   GetClassModelSurfaceName(class<Actor> cls, int modelindex, int surface);
	native clearscope static int    GetClassModelJointCount(class<Actor> cls, int modelindex);
	native clearscope static Name   GetClassModelJointName(class<Actor> cls, int modelindex, int joint);

	// Full state-label enumeration, sorted by state address (= source
	// declaration order). FindState can only probe names known in advance;
	// this returns every label the class actually defines, including
	// mod-custom ones, so a walker can attribute "RealFire" or "Work1" to
	// the standard label they were written under. Null-state labels are
	// skipped; top level only.
	clearscope native static int CountStateLabels(class<Actor> cls);
	clearscope native static Name, State GetStateLabelAt(class<Actor> cls, int index);

	void A_SetFriendly (bool set)
	{
		if (CountsAsKill() && health > 0) level.total_monsters--;
		bFriendly = set;
		if (CountsAsKill() && health > 0) level.total_monsters++;
	}

	native void A_RaiseMaster(int flags = 0);
	native void A_RaiseChildren(int flags = 0);
	native void A_RaiseSiblings(int flags = 0);
	native bool A_RaiseSelf(int flags = 0);
	native bool RaiseActor(Actor other, int flags = 0);
	native bool CanRaise();
	native void Revive();
	native void A_Weave(int xspeed, int yspeed, double xdist, double ydist);

	action native state, bool A_Teleport(statelabel teleportstate = null, class<SpecialSpot> targettype = "BossSpot", class<Actor> fogtype = "TeleportFog", int flags = 0, double mindist = 128, double maxdist = 0, int ptr = AAPTR_DEFAULT);
	action native state, bool A_Warp(int ptr_destination, double xofs = 0, double yofs = 0, double zofs = 0, double angle = 0, int flags = 0, statelabel success_state = null, double heightoffset = 0, double radiusoffset = 0, double pitch = 0);
	native void A_CountdownArg(int argnum, statelabel targstate = null);
	native state A_MonsterRefire(int chance, statelabel label);
	native void A_LookEx(int flags = 0, double minseedist = 0, double maxseedist = 0, double maxheardist = 0, double fov = 0, statelabel label = null);

	// [UZDXREMA] must stay an 'action' function: the native implementation uses
	// ACTION_CALL_FROM_PSPRITE()/stateinfo to pick the off-hand recoil direction.
	action native void A_Recoil(double xyvel);
	native int A_RadiusGive(class<Inventory> itemtype, double distance, int flags, int amount = 0, class<Actor> filter = null, name species = "None", double mindist = 0, int limit = 0);
	native void A_CustomMeleeAttack(int damage = 0, sound meleesound = "", sound misssound = "", name damagetype = "none", bool bleed = true);
	native void A_CustomComboAttack(class<Actor> missiletype, double spawnheight, int damage, sound meleesound = "", name damagetype = "none", bool bleed = true);
	native void A_Burst(class<Actor> chunktype);
	native void A_RadiusDamageSelf(int damage = 128, double distance = 128.0, int flags = 0, class<Actor> flashtype = null);
	native int GetRadiusDamage(Actor thing, int damage, double distance, double fulldmgdistance = 0.0, bool oldradiusdmg = false, bool circular = false);
	native int RadiusAttack(Actor bombsource, int bombdamage, double bombdistance, Name bombmod = 'none', int flags = RADF_HURTSOURCE, double fulldamagedistance = 0.0, name species = "None");

	native void A_Respawn(int flags = 1);
	native void A_RestoreSpecialPosition();
	native void A_QueueCorpse();
	native void A_DeQueueCorpse();
	native void A_ClearLastHeard();

	native void A_ClassBossHealth();
	native void A_SetAngle(double angle = 0, int flags = 0, int ptr = AAPTR_DEFAULT);
	native void A_SetPitch(double pitch, int flags = 0, int ptr = AAPTR_DEFAULT);
	native void A_SetRoll(double roll, int flags = 0, int ptr = AAPTR_DEFAULT);
	native void A_SetViewAngle(double angle = 0, int flags = 0, int ptr = AAPTR_DEFAULT);
	native void A_SetViewPitch(double pitch, int flags = 0, int ptr = AAPTR_DEFAULT);
	native void A_SetViewRoll(double roll, int flags = 0, int ptr = AAPTR_DEFAULT);
	native void SetViewPos(Vector3 offset, int flags = -1);
	deprecated("2.3", "User variables are deprecated in ZScript. Actor variables are directly accessible") native void A_SetUserVar(name varname, int value);
	deprecated("2.3", "User variables are deprecated in ZScript. Actor variables are directly accessible") native void A_SetUserArray(name varname, int index, int value);
	deprecated("2.3", "User variables are deprecated in ZScript. Actor variables are directly accessible") native void A_SetUserVarFloat(name varname, double value);
	deprecated("2.3", "User variables are deprecated in ZScript. Actor variables are directly accessible") native void A_SetUserArrayFloat(name varname, int index, double value);

	// NOT deprecated, unlike its siblings above: those exist for old DECORATE
	// code that predates being able to just cast to a known type and touch a
	// field directly. This one solves a problem casting cannot: writing a
	// Name-typed field on an actor whose CLASS is defined in a pk3 that is
	// loaded (and therefore compiled) after the caller's own pk3, which is a
	// direct cast can never resolve -- the class is not a known type yet at
	// the point the caller's file is compiled, whatever order the two pk3s end
	// up in relative to each other at runtime. Two independently-loaded mods
	// naming a field to each other by string is the intended use.
	native void A_SetUserVarName(name varname, name value);
	native void A_Quake(double intensity, int duration, double damrad, double tremrad, sound sfx = "world/quake");
	native void A_QuakeEx(double intensityX, double intensityY, double intensityZ, int duration, double damrad, double tremrad, sound sfx = "world/quake", int flags = 0, double mulWaveX = 1, double mulWaveY = 1, double mulWaveZ = 1, double falloff = 0, int highpoint = 0, double rollIntensity = 0, double rollWave = 0, double damageMultiplier = 1, double thrustMultiplier = 0.5, int damage = 0);
	action native void A_SetTics(int tics);
	native void A_DamageSelf(int amount, name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_DamageTarget(int amount, name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_DamageMaster(int amount, name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_DamageTracer(int amount, name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_DamageChildren(int amount, name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_DamageSiblings(int amount, name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_KillTarget(name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_KillMaster(name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_KillTracer(name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_KillChildren(name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_KillSiblings(name damagetype = "none", int flags = 0, class<Actor> filter = null, name species = "None", int src = AAPTR_DEFAULT, int inflict = AAPTR_DEFAULT);
	native void A_RemoveTarget(int flags = 0, class<Actor> filter = null, name species = "None");
	native void A_RemoveMaster(int flags = 0, class<Actor> filter = null, name species = "None");
	native void A_RemoveTracer(int flags = 0, class<Actor> filter = null, name species = "None");
	native void A_RemoveChildren(bool removeall = false, int flags = 0, class<Actor> filter = null, name species = "None");
	native void A_RemoveSiblings(bool removeall = false, int flags = 0, class<Actor> filter = null, name species = "None");
	native void A_Remove(int removee, int flags = 0, class<Actor> filter = null, name species = "None");
	native void A_SetTeleFog(class<Actor> oldpos, class<Actor> newpos);
	native void A_SwapTeleFog();
	native void A_SetHealth(int health, int ptr = AAPTR_DEFAULT);
	native void A_ResetHealth(int ptr = AAPTR_DEFAULT);
	native void A_SetSpecies(name species, int ptr = AAPTR_DEFAULT);
	native void A_SetChaseThreshold(int threshold, bool def = false, int ptr = AAPTR_DEFAULT);
	native bool A_FaceMovementDirection(double offset = 0, double anglelimit = 0, double pitchlimit = 0, int flags = 0, int ptr = AAPTR_DEFAULT);
	native int A_ClearOverlays(int sstart = 0, int sstop = 0, bool safety = true);
	native bool A_CopySpriteFrame(int from, int to, int flags = 0);
	native bool A_SetVisibleRotation(double anglestart = 0, double angleend = 0, double pitchstart = 0, double pitchend = 0, int flags = 0, int ptr = AAPTR_DEFAULT);
	native void A_SetTranslation(name transname);
	native bool A_SetSize(double newradius = -1, double newheight = -1, bool testpos = false);
	native void A_SprayDecal(String name, double dist = 172, vector3 offset = (0, 0, 0), vector3 direction = (0, 0, 0), bool useBloodColor = false, color decalColor = 0, TranslationID translation = 0);
	native void A_SetMugshotState(String name);
	native void CopyBloodColor(readonly<Actor> other);

	native void A_RearrangePointers(int newtarget, int newmaster = AAPTR_DEFAULT, int newtracer = AAPTR_DEFAULT, int flags=0);
	native void A_TransferPointer(int ptr_source, int ptr_recipient, int sourcefield, int recipientfield=AAPTR_DEFAULT, int flags=0);
	native void A_CopyFriendliness(int ptr_source = AAPTR_MASTER);

	action native bool A_Overlay(int layer, statelabel start = null, bool nooverride = false);
	action native void A_WeaponOffset(double wx = 0, double wy = 32, int flags = 0);
	action native void A_OverlayScale(int layer, double wx = 1, double wy = 0, int flags = 0);
	action native void A_OverlayRotate(int layer, double degrees = 0, int flags = 0);
	action native void A_OverlayPivot(int layer, double wx = 0.5, double wy = 0.5, int flags = 0);
	action native void A_OverlayPivotAlign(int layer, int halign, int valign);
	action native void A_OverlayVertexOffset(int layer, int index, double x, double y, int flags = 0);
	action native void A_OverlayOffset(int layer = PSP_WEAPON, double wx = 0, double wy = 32, int flags = 0);
	action native void A_OverlayFlags(int layer, int flags, bool set);
	action native void A_OverlayAlpha(int layer, double alph);
	action native void A_OverlayRenderStyle(int layer, int style);
	action native void A_OverlayTranslation(int layer, name trname);

	native bool A_AttachLightDef(Name lightid, Name lightdef);
	native bool A_AttachLight(Name lightid, int type, Color lightcolor, int radius1, int radius2, int flags = 0, Vector3 ofs = (0,0,0), double param = 0, double spoti = 10, double spoto = 25, double spotp = 0, double intensity = 1.0);
	native bool A_RemoveLight(Name lightid);
	// [round2 B1] Hold an attached light in a tracked pose, re-posed every frame
	// instead of every tic. mode 0 none (follows this actor, as always), 1 main
	// hand, 2 off hand, 3 head; offset is (forward, right, up) in map units in the
	// pose's own frame. While anchored the light takes position, yaw and pitch
	// from the pose (A_AttachLight's offset and spotp are ignored).
	//
	// Finds the light by the id given to A_AttachLight / A_AttachLightDef, as
	// A_RemoveLight does, and may be called straight after them. The anchor stays
	// with that id through light rebuilds and savegames until mode 0 or
	// A_RemoveLight. On a DynamicLight actor (SpotLight and the rest) the id is
	// ignored and the actor's own light is anchored.
	//
	// The pose is this actor's player's when the actor is a player pawn, and the
	// local player's otherwise. Look-only: no gameplay reads a light's position.
	// Returns false when no light of that id exists.
	native bool SetAttachedLightAnchor(Name lightid, int mode, Vector3 offset = (0,0,0));

	//================================================
	//
	// Bone Offset Setters
	//
	//================================================

	native version("4.15.1") void SetBoneRotation(int boneIndex, Quat rotation, int mode = SB_ADD, double interpolation_duration = 1.0);
	native version("4.15.1") void SetNamedBoneRotation(Name boneName, Quat rotation, int mode = SB_ADD, double interpolation_duration = 1.0);

	version("4.15.1") void SetBoneRotationAngles(int boneIndex, double yaw, double pitch, double roll, int mode = SB_ADD, double interpolation_duration = 1.0)
	{
		SetBoneRotation(boneIndex, Quat.FromAngles(yaw, pitch, roll), mode, interpolation_duration);
	}

	version("4.15.1") void SetNamedBoneRotationAngles(Name boneName, double yaw, double pitch, double roll, int mode = SB_ADD, double interpolation_duration = 1.0)
	{
		SetNamedBoneRotation(boneName, Quat.FromAngles(yaw, pitch, roll), mode, interpolation_duration);
	}

	version("4.15.1") void ClearBoneRotation(int boneIndex, double interpolation_duration = 1.0)
	{
		SetBoneRotation(boneIndex, Quat(0, 0, 0, 1), 0, interpolation_duration);
	}

	version("4.15.1") void ClearNamedBoneRotation(Name boneName, double interpolation_duration = 1.0)
	{
		SetNamedBoneRotation(boneName, Quat(0, 0, 0, 1), 0, interpolation_duration);
	}

	native version("4.15.1") void SetBoneTranslation(int boneIndex, Vector3 translation, int mode = SB_ADD, double interpolation_duration = 1.0);
	native version("4.15.1") void SetNamedBoneTranslation(Name boneName, Vector3 translation, int mode = SB_ADD, double interpolation_duration = 1.0);

	version("4.15.1") void ClearBoneTranslation(int boneIndex, double interpolation_duration = 1.0)
	{
		SetBoneTranslation(boneIndex, (0, 0, 0), 0, interpolation_duration);
	}

	version("4.15.1") void ClearNamedBoneTranslation(Name boneName, double interpolation_duration = 1.0)
	{
		SetNamedBoneTranslation(boneName, (0, 0, 0), 0, interpolation_duration);
	}

	native version("4.15.1") void SetBoneScaling(int boneIndex, Vector3 scaling, int mode = SB_ADD, double interpolation_duration = 1.0);
	native version("4.15.1") void SetNamedBoneScaling(Name boneName, Vector3 scaling, int mode = SB_ADD, double interpolation_duration = 1.0);

	version("4.15.1") void ClearBoneScaling(int boneIndex, double interpolation_duration = 1.0)
	{
		SetBoneScaling(boneIndex, (0, 0, 0), 0, interpolation_duration);
	}

	version("4.15.1") void ClearNamedBoneScaling(Name boneName, double interpolation_duration = 1.0)
	{
		SetNamedBoneScaling(boneName, (0, 0, 0), 0, interpolation_duration);
	}

	native version("4.15.1") void ClearBoneOffsets();

	//================================================
	//
	// Bone Offset Getters
	//
	//================================================

	/* rotation, translation, scaling */
	native version("4.15.1") Quat, Vector3, Vector3 GetBoneOffset(int boneIndex);
	native version("4.15.1") Quat, Vector3, Vector3 GetNamedBoneOffset(Name boneName);

	//================================================
	//
	// Bone Info Getters
	//
	//================================================

	native version("4.15.1") void GetRootBones(out Array<int> rootBones);

	native version("4.15.1") Name GetBoneName(int boneIndex);
	native version("4.15.1") int GetBoneIndex(Name boneName);

	native version("4.15.1") int GetBoneParent(int boneIndex);
	native version("4.15.1") int GetNamedBoneParent(Name boneName); // return value lower than 0 means it's a root bone, and as such has no parent

	native version("4.15.1") void GetBoneChildren(int boneIndex, out Array<int> children);
	native version("4.15.1") void GetNamedBoneChildren(Name boneName, out Array<int> children);

	/* rotation, translation, scaling */
	native version("4.15.1") Quat, Vector3, Vector3 GetBoneBaseTRS(int boneIndex);
	native version("4.15.1") Quat, Vector3, Vector3 GetNamedBoneBaseTRS(Name boneName);

	native version("4.15.1") Vector3 GetBoneBasePosition(int boneIndex);
	native version("4.15.1") Vector3 GetNamedBoneBasePosition(Name boneName);

	native version("4.15.1") Quat GetBoneBaseRotation(int boneIndex);
	native version("4.15.1") Quat GetNamedBoneBaseRotation(Name boneName);

	native version("4.15.1") int GetBoneCount();

	//================================================
	//
	// Bone Pose Getters
	//
	//================================================

	native version("4.15.1") int GetAnimStartFrame(Name animName);
	native version("4.15.1") int GetAnimEndFrame(Name animName);
	native version("4.15.1") double GetAnimFramerate(Name animName);

	/* rotation, translation, scaling */
	native version("4.15.1") Quat, Vector3, Vector3 GetBoneFramePose(int boneIndex, int frame);
	native version("4.15.1") Quat, Vector3, Vector3 GetNamedBoneFramePose(Name boneName, int frame);

	//================================================
	//
	// Bone TRS Getters
	//
	//================================================

	/* rotation, translation, scaling, doesn't include parent bones */
	native version("4.15.1") Quat, Vector3, Vector3 GetBoneTRS(int boneIndex, bool include_offsets = true);
	native version("4.15.1") Quat, Vector3, Vector3 GetNamedBoneTRS(Name boneName, bool include_offsets = true);

	/* angle, pitch, roll, includes parent bones */
	native version("4.15.1") Vector3 GetBoneEulerAngles(int boneIndex, bool include_offsets = true);
	native version("4.15.1") Vector3 GetNamedBoneEulerAngles(Name boneName, bool include_offsets = true);

	//input position/direction vectors are in xzy, model space
	native version("4.15.1") Vector3, Vector3, Vector3 TransformByBone(int boneIndex, Vector3 position, Vector3 forward = (1,0,0), Vector3 up = (0,0,1), bool include_offsets = true);
	// RS fork -- VR_WORLDACTOROFFSET.
	//
	// Take a point in this actor's MODEL space and return where it is in the
	// WORLD, plus the model's forward and up axes there. Runs the renderer's own
	// object-to-world matrix, so it carries the actor transform, every MODELDEF
	// correction, and -- for a FollowMainHand/FollowOffHand model -- the whole
	// controller transform.
	//
	// TransformByNamedBone stops at model space and cannot tell you where a bone
	// is in the room. Compose the two and a bone becomes a world position:
	// seating a gun into a hand is then subtracting one from the other, and the
	// firing line is the returned forward axis rather than a guess rebuilt out
	// of Euler angles.
	//
	// IN: the renderer's model space, y up -- the MD3 loader stores every vertex
	// as (x, z, y), and surface offsets are in this space too. OUT: map order,
	// (x, y, z) with z up, like every other position script handles and like
	// GetBonePosition. It answered in the renderer's (x, z, y) until 2026-09-11;
	// see ModelWorldTransform in models.cpp.
	native Vector3, Vector3, Vector3 ModelPointToWorld(double mx, double my, double mz);

	// RS fork -- where a CHILD of this actor is drawn (FollowActor): the seat
	// (X forward, Y left, Z up, frame units) as a world point, plus the frame's
	// three axes UNNORMALISED -- one frame unit each, in map units. Answers from
	// the frame the renderer really seats a child in, which leaves out this
	// actor's scale; ModelPointToWorld keeps it, so on a scaled or mirrored model
	// the two disagree. Use this one to work out a child's seat.
	native Vector3, Vector3, Vector3, Vector3 ModelFollowFrameToWorld(double sx, double sy, double sz);

	// RS fork -- is this actor solid, or a billboard? Anything drawn in a frame of
	// its own (FollowHandMode, FollowBodyMode, FollowActor) must be a MODEL; a
	// sprite ignores those and is drawn where the actor stands. HasModelFrame: its
	// ordinary lookup finds a model or voxel. HasVoxelFrame: a voxel exists for its
	// current frame -- ask before setting VoxelOverride, since a pack is optional.
	native bool HasModelFrame();
	native bool HasVoxelFrame();

	// RS fork -- does this actor's model carry this bone? -1 if not.
	//
	// The model declares what it is: a mesh with MARKER_grip has a grip, one
	// with MARKER_muzzle is a firearm. Every other bone call fails silently on a
	// missing name -- the setters Printf and no-op, TransformByNamedBone returns
	// the origin -- so none of them can answer "is it there".
	native int FindBoneIndex(Name bone);

	// RS fork -- DRAW-TIME JOINT POSES (render only; src/r_data/model_reach.h).
	//
	// Bend a joint of this actor's rigged model where it is DRAWN, on top of its
	// animation and the stock bone overrides above, every frame -- so it answers at
	// the display rate and with a menu open. Works on decoupled and non-decoupled
	// models alike, needs no model data, and is never saved.
	//
	// RENDER ONLY, ON PURPOSE: nothing here changes what GetBoneMatrix,
	// TransformByNamedBone or GetBonePosition return, and there is no getter. A pose
	// built from this machine's controllers can change only this machine's pixels, so
	// it can never decide anything in a netgame. Never use it for something the game
	// must agree on -- use SetBoneRotation for that.
	//
	// Modes: MJP_Multiply turns the joint by rotation in its own local frame (the
	// drawn local rotation times rotation), MJP_Replace sets its local rotation,
	// MJP_Hide collapses the joint and everything under it, MJP_Clear removes the
	// pose. Joints are named; a name not on the drawn model is ignored (logged once).
	// Calling again with the same arguments changes nothing, so re-asserting every
	// tic is fine -- and is how a pose survives a savegame load.
	//
	// SetModelJointDrawOffset is the translation half of the same per-joint entry, in the
	// joint's PARENT's local units like a bone translation: MJO_Add adds to the drawn local
	// translation, MJO_Replace sets it, MJO_Clear removes it. Rotation and translation
	// together turn a joint about a point that is not its own origin.
	enum EModelJointDrawPose
	{
		MJP_Clear    = 0,
		MJP_Multiply = 1,
		MJP_Replace  = 2,
		MJP_Hide     = 3,

		MJO_Clear    = 0,
		MJO_Add      = 1,
		MJO_Replace  = 2,
	};
	native bool SetModelJointDrawPose(Name joint, Quat rotation, int mode = MJP_Multiply, int modelIndex = 0);
	native bool SetModelJointDrawOffset(Name joint, Vector3 offset, int mode = MJO_Add, int modelIndex = 0);
	// 'None' clears every joint; modelIndex -1 clears every model index. Both halves.
	native void ClearModelJointDrawPose(Name joint = 'None', int modelIndex = -1);

	// RS fork -- JOINT OFFSETS AND JOINT DRIVES (render only, like the joint poses above; src/r_data/model_reach.cpp,
	// Engine docs/MODEL_JOINT_DRIVE_PLAN.md pieces B and C). A rigged part moved exactly as a mesh surface is moved by
	// SetModelSurfaceOffset and the SetModelSurfaceDrive family -- the same meanings, the same arguments, the same
	// solver -- but on a NAMED JOINT, for a gun whose parts are bones (an IQM).
	//
	// ALL VECTORS ARE IN MODEL SPACE, as the surface calls take them: the renderer's, y up, the file's (x, z, y). The
	// transform is laid on the joint as drawn -- its animation and the draw poses above included -- and everything under
	// the joint rides it. A reach chain overwrites it on the chain's own joints.
	//
	// SetModelJointOffset: turn about the model origin by rot, then move by ofs, interpolated from the last tic's value to
	// the drawn instant. Re-assert it every tic, as a surface offset is. Refused for a missing joint name or a NaN.
	// While a joint has a drive, its offset is not drawn.
	native bool   SetModelJointOffset(Name joint, Vector3 ofs, Quat rot, int modelIndex = 0);
	native bool   ClearModelJointOffset(Name joint, int modelIndex = 0);
	// THE HAND DRIVE ON A JOINT, drawn from the live controller on the frame being drawn. Each call is its
	// SetModelSurfaceDrive* twin: a plain slide (distance along axis, mesh units), a turn as it slides (Rotation, after
	// Drive), a hinge (degrees about axis through pivot, under 180), and a second stage (Stage, after Drive or Hinge;
	// kind DRIVESTAGE_*, split in [0.001, 0.999]). startValue resumes where the part is. Up to 16 joints per actor.
	// ClearModelJointDrive hands the joint back to script: the drive switches off and its entry stays, as a surface
	// slot does -- set a joint's drive and offset once and keep updating them; do not add and remove them per grab.
	// GetModelJointDrawnValue is what was DRAWN, 0..1, with GetModelSurfaceDrawnValue's contract (decide gameplay on
	// what was on screen); read it before clearing.
	native bool   SetModelJointDrive(Name joint, int modelIndex, int hand, Vector3 axis, double distance, double startValue);
	native bool   SetModelJointDriveRotation(Name joint, int modelIndex, Vector3 axis, double degrees, Vector3 pivot);
	native bool   SetModelJointDriveHinge(Name joint, int modelIndex, int hand, Vector3 axis, double degrees, Vector3 pivot, double startValue);
	native bool   SetModelJointDriveStage(Name joint, int modelIndex, int kind, Vector3 axis, double amount, Vector3 pivot, double split);
	native bool   ClearModelJointDrive(Name joint, int modelIndex = 0);
	native double GetModelJointDrawnValue(Name joint, int modelIndex = 0);

	// RS fork -- REACH CHAINS (render only; src/r_data/model_reach.h).
	//
	// Three joints of this actor's model -- root -> mid -> end, e.g. upper arm,
	// forearm, wrist -- bent every drawn frame so the end joint lands on a point of
	// ANOTHER actor's model as that model is drawn this frame: its controller, its
	// MODELDEF offsets and its live placement sliders all included. The target leads;
	// it is never moved. Up to 4 chains per actor (0..3). Render only, never saved,
	// no getter -- the same contract as the joint poses above.
	//
	// ALL VECTORS ARE IN MODEL SPACE the way ModelPointToWorld takes them: the
	// renderer's, y up, so a point or direction read from the model FILE as (x, y, z)
	// goes in as (x, z, y).
	//
	// SetModelReachChain: the joints, and the TUNING cvar prefix the renderer reads
	// every frame (an absent cvar keeps its default): <tuning>_stretch_max 1.25,
	// _soft_start 0.90, _pole_out 1.0, _pole_down 0.6, _pole_back 0.35, _align 1.0,
	// _align_max 40, _align_fade_lo 0.10, _align_fade_span 0.25, _align_conf_lo 0.05,
	// _align_conf_span 0.20, _swivel_rate 0, _twist 1.0, _twist_taper 110,
	// _twist_conf_lo 0.15, _twist_conf_span 0.30, _twist_ofs 0, _twist_rate 0,
	// _follow 0.25, _follow_max 25, _clear_radius 0 (off), _clear_max 100,
	// _clear_rate 0, _aim 1.0, _aim_max (SetModelReachTargetJoint's maxDeg). Call this
	// first; the calls below need it.
	// Every joint along each bone rides it by where it sits on it: a stretch slides it
	// along the bone, and along the mid bone the twist turns it in proportion (0 at
	// mid, 1 at end). Everything under the end joint rides the end joint.
	native bool SetModelReachChain(int chain, Name rootJoint, Name midJoint, Name endJoint, Name tuning = 'None', int modelIndex = 0);
	// The rig's own directions in this model's space. The elbow bends toward
	// outward * _pole_out + down * _pole_down + back * _pole_back, so the arm's SIDE is
	// the sign of outward (a negative _pole_out slider flips it live). twistRef: the
	// direction on the end bone that must roll onto the target's twistRef (for an arm,
	// its own index-finger side at its animated pose); zero turns twist off.
	native bool SetModelReachFrame(int chain, Vector3 outward, Vector3 down, Vector3 back, Vector3 twistRef = (0,0,0));
	// What to reach. point is on reachTarget's model, in ITS model space and model
	// units (so a hand Scale change does not move it); <pointCVar>_ofs_x/_y/_z are added
	// live. fingerDir (reachTarget's model space, from point toward the fingers) turns
	// the elbow so the forearm follows the hand; zero turns that off. twistRef
	// (reachTarget's model space) is the target's index-finger side. A null reachTarget
	// stands the chain down: the model is drawn as animated.
	native bool SetModelReachTarget(int chain, Actor reachTarget, Vector3 point, Vector3 fingerDir = (0,0,0), Vector3 twistRef = (0,0,0), Name pointCVar = 'None');
	// HOW HARD THIS CHAIN TRIES TO REACH. 0 = capped (the default, and the reference): the arm
	// stops short once the target is further away than <tuning>_stretch_max allows, which is
	// itself clamped in the solve. 1 = ABSOLUTE: the cap is not consulted and the bones scale so
	// the end joint lands EXACTLY on the target.
	//
	// Mode 1 is for VR, where the hand must be AT the controller and the player's real hand is
	// ground truth: a stretched arm looks wrong, a DETACHED HAND looks broken. A desktop caller
	// wants the opposite and should stay on 0.
	native bool SetModelReachStretchMode(int chain, int mode);
	// DOES THE END BONE TAKE THE TARGET'S FACING, as well as its position? 0 = no (the default,
	// and what every chain did before this existed): the solve places the end joint and leaves
	// its orientation to the animation. 1 = yes: the end bone turns so its own frame lands on
	// the target's, about the point the solve just placed, so the position is untouched.
	//
	// On an arm this is THE WRIST. Without it the hand arrives at the controller and then
	// ignores how the controller is HELD -- the wrist never bends and the hand never rotates --
	// because align swivels the elbow and twist rolls the forearm, and neither is the end bone.
	// A foot taking a slope, or a head taking a look direction, is the same field.
	//
	// It needs no new numbers: the frames are the ones already declared, the rig's twistRef
	// from SetModelReachFrame against fingerDir and twistRef from SetModelReachTarget. Give
	// those honestly and the wrist is right; leave either zero and this does nothing rather
	// than guessing. weight blends the turn in, 1.0 for the full match.
	// midRollShare: how much of the aim's ROLL the MID bone takes instead of the end one.
	// 0 (the default) is the old behaviour. Rolling only the end bone shears the skin
	// between it and its parent into the candy-wrapper pinch -- on a wrist that is a hand
	// collapsing to a point at large rolls, because a linear blend has nothing in between
	// to spread the turn over. Rigs with a forearm twist bone do not need this; rigs whose
	// forearm parents the hand directly very much do. Only the ROLL is shared, never the
	// bend, so the end joint does not move at all -- a twist about the bone's own axis
	// moves nothing lying on that axis. Around 0.5 to 0.7 looks right on a human arm.
	native bool SetModelReachEndAim(int chain, int mode, double weight = 1.0, double midRollShare = 0.0);
	// A bind-pose point ON THE END BONE, in THIS model's own units, that the solve lands on the
	// target instead of the bone's origin. An arm chain ends at the WRIST, so reaching a hand
	// puts the wrist on the target and the hand carries past it by its own wrist-to-palm length;
	// state that length here, once, on the side that actually knows it. Note the space: this is
	// the CHAIN's model units, where SetModelReachTarget's `point` is the TARGET's. (0,0,0) is
	// the end bone's origin, exactly as before. Not a palm fix -- a spine chain placing the base
	// of a skull, or a leg chain placing the ball of a foot, is the same field.
	native bool SetModelReachEndOfs(int chain, double x, double y, double z);
	// An ancestor of the root that leans a little toward the target first (a clavicle):
	// _follow of the swing, capped at _follow_max degrees. 'None' turns it off.
	native bool SetModelReachFollowJoint(int chain, Name joint);
	// CLEARANCE: with <tuning>_clear_radius above 0 -- in this model's own units, so it
	// grows with the model's fit -- the elbow swings round its circle out of walls, step
	// faces, ledge edges, solid 3D floors and polyobjects near the chain. _clear_max caps
	// the swing in degrees (100), _clear_rate eases it per second (0 = instant). Absent or
	// 0: off. The level is only read.
	//
	// After solving, turn a joint of the TARGET's model (its model index
	// targetModelIndex) about pivot so its axis points back along the solved end bone,
	// at most maxDeg -- <tuning>_aim_max wins live when it exists, and <tuning>_aim 0..1
	// weights it. axis (0,0,0) means minus fingerDir. keepChildren: the joint's direct
	// children keep their drawn place, so only what is skinned to the joint itself turns.
	// pivot and axis are in the TARGET's model space, at rest. The RS hand:
	// SetModelReachTargetJoint(0, 'Root_joint', (0,0,0), 70) turns its wrist stub along
	// the forearm while HANDPALM and the fingers stay on the controller. 'None' turns it off.
	native bool SetModelReachTargetJoint(int chain, Name joint, Vector3 pivot = (0,0,0), double maxDeg = 70, Vector3 axis = (0,0,0), bool keepChildren = true, int targetModelIndex = 0);
	// chain -1 clears every chain.
	native void ClearModelReachChain(int chain = -1);

	native version("4.15.1") Vector3, Vector3, Vector3 TransformByNamedBone(Name boneName, Vector3 position, Vector3 forward = (1,0,0), Vector3 up = (0,0,1), bool include_offsets = true);

	version("4.15.1") Vector3, Vector3, Vector3 GetBonePosition(int boneIndex, bool include_offsets = true)
	{
		let [a, b, c] = TransformByBone(boneIndex, GetBoneBasePosition(boneIndex), include_offsets:include_offsets);
		return a, b, c;
	}

	version("4.15.1") Vector3, Vector3, Vector3 GetNamedBonePosition(name boneName, bool include_offsets = true)
	{
		let [a, b, c] = GetBonePosition(GetBoneIndex(boneName), include_offsets);
		return a, b, c;
	}

	//================================================
	//
	// Bone Matrix Getters
	//
	//================================================

	//outMatrix will be a 16-length array containing the raw matrix data
	native version("4.15.1") void GetBoneMatrixRaw(int boneIndex, out Array<double> outMatrix, bool include_offsets = true);
	native version("4.15.1") void GetNamedBoneMatrixRaw(Name boneName, out Array<double> outMatrix, bool include_offsets = true);

	native version("4.15.1") void GetObjectToWorldMatrixRaw(out Array<double> outMatrix);



	//================================================
	//
	// Animation Sequence
	//
	//================================================

	native version("4.15.1") AnimationLayer SetAnimationLayerAnimation(AnimationLayer layer, Name animName, double framerate = -1, int startFrame = -1, int loopFrame = -1, int endFrame = -1, int interpolateTics = -1, int flags = 0);
	native version("4.15.1") ui AnimationLayer SetAnimationLayerAnimationUI(AnimationLayer layer, Name animName, double framerate = -1, int startFrame = -1, int loopFrame = -1, int endFrame = -1, int interpolateTics = -1, int flags = 0);

	native version("4.15.1") AnimationLayer SetAnimationLayerFrameRate(AnimationLayer layer, double framerate);
	native version("4.15.1") ui AnimationLayer SetAnimationLayerFrameRateUI(AnimationLayer layer, double framerate);

	native version("4.15.1") PrecalculatedAnimationFrame CalculateAnimation(readonly<AnimationLayer> layer);
	native version("4.15.1") ui PrecalculatedAnimationFrame CalculateAnimationUI(readonly<AnimationLayer> layer);

	native version("4.15.1") static clearscope PrecalculatedAnimationFrame BlendAnimationFrames(PrecalculatedAnimationFrame a, PrecalculatedAnimationFrame b, double t);
	native version("4.15.1") static clearscope PrecalculatedAnimationFrame OffsetAnimationFrame(PrecalculatedAnimationFrame frame, PrecalculatedAnimationFrame offset);

	native version("4.15.1") clearscope PrecalculatedAnimationFrame CalculateAnimationFrame(readonly<InterpolatedFrame> frame);

	// tic should be Level.totaltime + fractic
	//
	// returns AnimationFrame frame1, InterpolatedFrame frame2, double inter
	// frame1 is the frame to interpolate from, it may be either a PrecalculatedAnimationFrame or a InterpolatedFrame, if inter is -1, frame1 will be null, and frame2 should be used in full instead
	// frame2 is the frame to interpolate to, always an InterpolatedFrame
	// inter is the ratio between frame1 and frame2, if the animation isn't interpolating, it will be 1 and frame 1 will be null
	// frame1/2 will both be null if an invalid tic or layer are passed
	//
	// NOTE: while interpolating, an animation may need to perform up to 4-way blending if both frame1 and frame2 are InterpolatedFrame
	//
	native version("4.15.1") static clearscope AnimationFrame, InterpolatedFrame, double FindAnimationFrameAt(readonly<AnimationLayer> layer, double tic);

	native version("4.15.1") clearscope AnimationFrame, InterpolatedFrame, double FindAnimationFrame(readonly<AnimationLayer> layer);
	native version("4.15.1") clearscope AnimationFrame, InterpolatedFrame, double FindAnimationFrameUI(readonly<AnimationLayer> layer);

	native version("4.15.1") void SetBones(PrecalculatedAnimationFrame bones, int mode = SB_ADD, double interpolation_duration = 1.0);
	native version("4.15.1") ui void SetBonesUI(PrecalculatedAnimationFrame bones, int mode = SB_ADD, double interpolation_duration = 1.0);
	native version("4.15.1") ui void OverwriteBones(PrecalculatedAnimationFrame bones, int mode = SB_ADD); // no interpolation, faster

	native version("4.15.1") void SetBonesRange(PrecalculatedAnimationFrame bones, int start, int length, int mode = SB_ADD, double interpolation_duration = 1.0);
	native version("4.15.1") ui void SetBonesRangeUI(PrecalculatedAnimationFrame bones, int start, int length, int mode = SB_ADD, double interpolation_duration = 1.0);
	native version("4.15.1") ui void OverwriteBonesRange(PrecalculatedAnimationFrame bones, int start, int length, int mode = SB_ADD); // no interpolation, faster

	native version("4.15.1") void SetBonesMask(PrecalculatedAnimationFrame bones, Array<bool> mask, int mode = SB_ADD, double interpolation_duration = 1.0);
	native version("4.15.1") ui void SetBonesMaskUI(PrecalculatedAnimationFrame bones, Array<bool> mask, int mode = SB_ADD, double interpolation_duration = 1.0);
	native version("4.15.1") ui void OverwriteBonesMask(PrecalculatedAnimationFrame bones, Array<bool> mask, int mode = SB_ADD); // no interpolation, faster

	native version("4.15.1") void ForceRecalculateBones(); // slow if called often, try and keep it to at most once per tick

	version("4.15.1") ui virtual void AnimateBones(double ticfrac){}
	//================================================
	//
	//
	//
	//================================================

	native version("4.12") void SetAnimation(Name animName, double framerate = -1, int startFrame = -1, int loopFrame = -1, int endFrame = -1, int interpolateTics = -1, int flags = 0);
	native version("4.12") ui void SetAnimationUI(Name animName, double framerate = -1, int startFrame = -1, int loopFrame = -1, int endFrame = -1, int interpolateTics = -1, int flags = 0);

	native version("4.12") void SetAnimationFrameRate(double framerate);
	native version("4.12") ui void SetAnimationFrameRateUI(double framerate);

	native version("4.12") void SetModelFlag(int flag, int iqmFlags = 0);
	native version("4.12") void ClearModelFlag(int flag, int iqmFlags = 0);
	native version("4.12") void ResetModelFlags(bool resetModel = true, bool resetIqm = false);

	action version("4.12") void A_SetAnimation(Name animName, double framerate = -1, int startFrame = -1, int loopFrame = -1, int endFrame = -1, int interpolateTics = -1, int flags = 0)
	{
		invoker.SetAnimation(animName, framerate, startFrame, loopFrame, endFrame, interpolateTics, flags);
	}

	action version("4.12") void A_SetAnimationFrameRate(double framerate)
	{
		invoker.SetAnimationFrameRate(framerate);
	}

	action version("4.12") void A_SetModelFlag(int flag)
	{
		invoker.SetModelFlag(flag);
	}

	action version("4.12") void A_ClearModelFlag(int flag)
	{
		invoker.ClearModelFlag(flag);
	}

	action version("4.12") void A_ResetModelFlags()
	{
		invoker.ResetModelFlags();
	}

	int ACS_NamedExecute(name script, int mapnum=0, int arg1=0, int arg2=0, int arg3=0)
	{
		return ACS_Execute(-int(script), mapnum, arg1, arg2, arg3);
	}
	int ACS_NamedSuspend(name script, int mapnum=0)
	{
		return ACS_Suspend(-int(script), mapnum);
	}
	int ACS_NamedTerminate(name script, int mapnum=0)
	{
		return ACS_Terminate(-int(script), mapnum);
	}
	int ACS_NamedLockedExecute(name script, int mapnum=0, int arg1=0, int arg2=0, int lock=0)
	{
		return ACS_LockedExecute(-int(script), mapnum, arg1, arg2, lock);
	}
	int ACS_NamedLockedExecuteDoor(name script, int mapnum=0, int arg1=0, int arg2=0, int lock=0)
	{
		return ACS_LockedExecuteDoor(-int(script), mapnum, arg1, arg2, lock);
	}
	int ACS_NamedExecuteWithResult(name script, int arg1=0, int arg2=0, int arg3=0, int arg4=0)
	{
		return ACS_ExecuteWithResult(-int(script), arg1, arg2, arg3, arg4);
	}
	int ACS_NamedExecuteAlways(name script, int mapnum=0, int arg1=0, int arg2=0, int arg3=0)
	{
		return ACS_ExecuteAlways(-int(script), mapnum, arg1, arg2, arg3);
	}
	int ACS_ScriptCall(name script, int arg1=0, int arg2=0, int arg3=0, int arg4=0)
	{
		return ACS_ExecuteWithResult(-int(script), arg1, arg2, arg3, arg4);
	}

	//===========================================================================
	//
	// Sounds
	//
	//===========================================================================

	void A_Scream()
	{
		if (DeathSound)
		{
			A_StartSound(DeathSound, CHAN_VOICE, CHANF_DEFAULT|CHANF_NORUMBLE, 1, bBoss || bFullvolDeath? ATTN_NONE : ATTN_NORM);
		}
	}

	void A_XScream()
	{
		A_StartSound(player? Sound("*gibbed") : Sound("misc/gibbed"), CHAN_VOICE, CHANF_NORUMBLE);
	}

	void A_ActiveSound()
	{
		if (ActiveSound)
		{
			A_StartSound(ActiveSound, CHAN_VOICE, CHANF_NORUMBLE);
		}
	}

	virtual void PlayerLandedMakeGruntSound(actor onmobj)
	{
		bool grunted;

		// [RH] only make noise if alive
		if (self.health > 0 && !Alternative)
		{
			grunted = false;
			// Why should this number vary by gravity?
			if (self.Vel.Z < -self.player.mo.GruntSpeed)
			{
				A_StartSound("*grunt", CHAN_VOICE, CHANF_NORUMBLE);
				grunted = true;
			}
			bool isliquid = (pos.Z <= floorz) && GetFloorTerrain().IsLiquid;
			if (onmobj != NULL || !isliquid)
			{
				if (!grunted)
				{
					A_StartSound("*land", CHAN_AUTO, CHANF_NORUMBLE);
				}
				else
				{
					A_StartSoundIfNotSame("*land", "*grunt", CHAN_AUTO, CHANF_NORUMBLE);
				}
			}
		}
	}

	virtual void PlayerSquatView(Actor onmobj)
	{
		if (!self.player)
			return;

		if (self.player.mo == self)
		{
			self.player.deltaviewheight = self.Vel.Z / 8.;
		}
	}

	virtual void PlayDiveOrSurfaceSounds(int oldlevel)
	{
		if (oldlevel < 3 && WaterLevel == 3)
		{
			// Our head just went under.
			A_StartSound("*dive", CHAN_VOICE, CHANF_NORUMBLE, attenuation: ATTN_NORM);
		}
		else if (oldlevel == 3 && WaterLevel < 3)
		{
			// Our head just came up.
			if (player.air_finished > Level.maptime)
			{
				// We hadn't run out of air yet.
				A_StartSound("*surface", CHAN_VOICE, CHANF_NORUMBLE, attenuation: ATTN_NORM);
			}
			// If we were running out of air, then ResetAirSupply() will play *gasp.
		}
	}

	//----------------------------------------------------------------------------
	//
	// player rumble events
	//
	//----------------------------------------------------------------------------

	virtual void PlayerLandedMakeRumble(actor onmobj)
	{
		if (!CVar.GetCVar("haptics_do_world").GetBool()) return;

		bool isliquid = (pos.Z <= floorz) && GetFloorTerrain().IsLiquid;
		if (onmobj != NULL || !isliquid)
		{
			Haptics.Rumble("*land");
		}
		else if (self.Vel.Z < -self.player.mo.GruntSpeed)
		{
			Haptics.Rumble("*grunt");
		}
	}

	virtual void PlayerHurtMakeRumble(actor source)
	{
		if (!CVar.GetCVar("haptics_do_damage").GetBool()) return;

		Haptics.Rumble("*pain");
	}

	virtual void PlayerDiedMakeRumble(actor source)
	{
		if (!CVar.GetCVar("haptics_do_damage").GetBool()) return;

		Haptics.Rumble("*death");
	}

	virtual void PlayerUsedSomethingMakeRumble(int activationType, int levelNum, int lineNum, int lineSpecial)
	{
		if (!CVar.GetCVar("haptics_do_action").GetBool()) return;

		Haptics.Rumble("*usesuccess");
	}

	virtual void PlayerTeleportedMakeRumble()
	{
		if (!CVar.GetCVar("haptics_do_world").GetBool()) return;

		Haptics.Rumble("misc/teleport");
	}

	virtual void PlayerPushedSomethingMakeRumble(actor thing)
	{
		if (!CVar.GetCVar("haptics_do_world").GetBool()) return;

		Haptics.Rumble("misc/push");
	}

	virtual void PlayerWasPushedMakeRumble(actor source)
	{
		if (!CVar.GetCVar("haptics_do_world").GetBool()) return;

		Haptics.Rumble("misc/pushed");
	}

	//----------------------------------------------------------------------------
	//
	// PROC A_CheckSkullDone
	//
	//----------------------------------------------------------------------------

	void A_CheckPlayerDone()
	{
		if (player == NULL) Destroy();
	}

	States(Actor, Overlay, Weapon, Item)
	{
	Spawn:
		TNT1 A -1;
		Stop;
	Null:
		TNT1 A 1;
		Stop;
	GenericFreezeDeath:
		// Generic freeze death frames. Woo!
		#### # 5 A_GenericFreezeDeath;
		---- A 1 A_FreezeDeathChunks;
		Wait;
	GenericCrush:
		POL5 A -1;
		Stop;
	DieFromSpawn:
		TNT1 A 1;
		TNT1 A 1 { self.Die(null, null); }
	}

	// Internal functions
	deprecated("2.3") private native int __decorate_internal_int__(int i);
	deprecated("2.3") private native bool __decorate_internal_bool__(bool b);
	deprecated("2.3") private native double __decorate_internal_float__(double f);
}
