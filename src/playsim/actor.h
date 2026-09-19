/*
** actor.h
**
** Map Objects, MObj, definition and handling.
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

#ifndef __P_MOBJ_H__
#define __P_MOBJ_H__

// Basics.


// We need the thinker_t stuff.
#include "dthinker.h"


// States are tied to finite states are tied to animation frames.
#include "info.h"

#include "doomdef.h"
#include "textures.h"
#include "renderstyle.h"
#include "s_sound.h"
#include "memarena.h"
#include "g_level.h"
#include "tflags.h"
#include "portal.h"
#include "bonecomponents.h"
#include "model_handdrive.h"	// RS fork -- FHandDrive, DActorModelData::SurfDrive

struct subsector_t;
struct FBlockNode;
struct FPortalGroupArray;
struct visstyle_t;
class FLightDefaults;
struct FSection;
struct FLevelLocals;
struct FDynamicLight;

//
// NOTES: AActor
//
// Actors are used to tell the refresh where to draw an image,
// tell the world simulation when objects are contacted,
// and tell the sound driver how to position a sound.
//
// The refresh uses the next and prev links to follow
// lists of things in sectors as they are being drawn.
// The sprite, frame, and angle elements determine which patch_t
// is used to draw the sprite if it is visible.
// The sprite and frame values are almost always set
// from state_t structures.
// The statescr.exe utility generates the states.h and states.c
// files that contain the sprite/frame numbers from the
// statescr.txt source file.
// The xyz origin point represents a point at the bottom middle
// of the sprite (between the feet of a biped).
// This is the default origin position for patch_ts grabbed
// with lumpy.exe.
// A walking creature will have its z equal to the floor
// it is standing on.
//
// The sound code uses the x,y, and sometimes z fields
// to do stereo positioning of any sound emitted by the actor.
//
// The play simulation uses the blocklinks, x,y,z, radius, height
// to determine when AActors are touching each other,
// touching lines in the map, or hit by trace lines (gunshots,
// lines of sight, etc).
// The AActor->flags element has various bit flags
// used by the simulation.
//
// Every actor is linked into a single sector
// based on its origin coordinates.
// The subsector_t is found with PointInSector(x,y).
// The sector links are only used by the rendering code,
// the play simulation does not care about them at all.
//
// Any actor that needs to be acted upon by something else
// in the play world (block movement, be shot, etc) will also
// need to be linked into the blockmap.
// If the thing has the MF_NOBLOCK flag set, it will not use
// the block links. It can still interact with other things,
// but only as the instigator (missiles will run into other
// things, but nothing can run into a missile).
// Each block in the grid is 128*128 units, and knows about
// every line_t that it contains a piece of, and every
// interactable actor that has its origin contained.
//
// A valid actor is an actor that has the proper subsector_t
// filled in for its xy coordinates and is linked into the
// sector from which the subsector was made, or has the
// MF_NOSECTOR flag set (the subsector_t needs to be valid
// even if MF_NOSECTOR is set), and is linked into a blockmap
// block or has the MF_NOBLOCKMAP flag set.
// Links should only be modified by the P_[Un]SetThingPosition()
// functions.
// Do not change the MF_NO* flags while a thing is valid.
//
// Any questions?
//



// --- mobj.flags ---
enum ActorFlag
{
	MF_SPECIAL			= 0x00000001,	// call P_SpecialThing when touched
	MF_SOLID			= 0x00000002,
	MF_SHOOTABLE		= 0x00000004,
	MF_NOSECTOR			= 0x00000008,	// don't use the sector links
										// (invisible but touchable)
	MF_NOBLOCKMAP		= 0x00000010,	// don't use the blocklinks
										// (inert but displayable)
	MF_AMBUSH			= 0x00000020,	// not activated by sound; deaf monster
	MF_JUSTHIT			= 0x00000040,	// try to attack right back
	MF_JUSTATTACKED		= 0x00000080,	// take at least one step before attacking
	MF_SPAWNCEILING		= 0x00000100,	// hang from ceiling instead of floor
	MF_NOGRAVITY		= 0x00000200,	// don't apply gravity every tic

// movement flags
	MF_DROPOFF			= 0x00000400,	// allow jumps from high places
	MF_PICKUP			= 0x00000800,	// for players to pick up items
	MF_NOCLIP			= 0x00001000,	// player cheat
	MF_SLIDE			= 0x00002000,	// Not used anymore but needed for MBF21 flag checkers.
	MF_FLOAT			= 0x00004000,	// allow moves to any height, no gravity
	MF_TELEPORT			= 0x00008000,	// don't cross lines or look at heights
	MF_MISSILE			= 0x00010000,	// don't hit same species, explode on block

	MF_DROPPED			= 0x00020000,	// dropped by a demon, not level spawned
	MF_SHADOW			= 0x00040000,	// actor is hard for monsters to see
	MF_NOBLOOD			= 0x00080000,	// don't bleed when shot (use puff)
	MF_CORPSE			= 0x00100000,	// don't stop moving halfway off a step
	MF_INFLOAT			= 0x00200000,	// floating to a height for a move, don't
										// auto float to target's height
	MF_INBOUNCE			= 0x00200000,	// used by Heretic bouncing missiles

	MF_COUNTKILL		= 0x00400000,	// count towards intermission kill total
	MF_COUNTITEM		= 0x00800000,	// count towards intermission item total

	MF_SKULLFLY			= 0x01000000,	// skull in flight
	MF_NOTDMATCH		= 0x02000000,	// don't spawn in death match (key cards)

	MF_SPAWNSOUNDSOURCE	= 0x04000000,	// Plays missile's see sound at spawning object.
	MF_FRIENDLY			= 0x08000000,	// [RH] Friendly monsters for Strife (and MBF)
	MF_UNMORPHED		= 0x10000000,	// [RH] Actor is the unmorphed version of something else
	MF_NOLIFTDROP		= 0x20000000,	// [RH] Used with MF_NOGRAVITY to avoid dropping with lifts
	MF_STEALTH			= 0x40000000,	// [RH] Andy Baker's stealth monsters
	MF_ICECORPSE		= 0x80000000,	// a frozen corpse (for blasting) [RH] was 0x800000
};

// --- mobj.flags2 ---
enum ActorFlag2
{
	MF2_DONTREFLECT		= 0x00000001,	// this projectile cannot be reflected
	MF2_WINDTHRUST		= 0x00000002,	// gets pushed around by the wind specials
	MF2_DONTSEEKINVISIBLE=0x00000004,	// For seeker missiles: Don't home in on invisible/shadow targets
	MF2_BLASTED			= 0x00000008,	// actor will temporarily take damage from impact
	MF2_FLY				= 0x00000010,	// fly mode is active
	MF2_FLOORCLIP		= 0x00000020,	// if feet are allowed to be clipped
	MF2_SPAWNFLOAT		= 0x00000040,	// spawn random float z
	MF2_NOTELEPORT		= 0x00000080,	// does not teleport
	MF2_RIP				= 0x00000100,	// missile rips through solid targets
	MF2_PUSHABLE		= 0x00000200,	// can be pushed by other moving actors
	MF2_SLIDE			= 0x00000400,	// slides against walls
	MF2_ONMOBJ			= 0x00000800,	// actor is resting on top of another actor
	MF2_PASSMOBJ		= 0x00001000,	// Enable z block checking. If on,
										// this flag will allow the actor to
										// pass over/under other actors.
	MF2_CANNOTPUSH		= 0x00002000,	// cannot push other pushable mobjs
	MF2_THRUGHOST		= 0x00004000,	// missile will pass through ghosts [RH] was 8
	MF2_BOSS			= 0x00008000,	// mobj is a major boss

	MF2_DONTTRANSLATE	= 0x00010000,	// Don't apply palette translations
	MF2_NODMGTHRUST		= 0x00020000,	// does not thrust target when damaging
	MF2_TELESTOMP		= 0x00040000,	// mobj can stomp another
	MF2_FLOATBOB		= 0x00080000,	// use float bobbing z movement
	MF2_THRUACTORS		= 0x00100000,	// performs no actor<->actor collision checks
	MF2_IMPACT			= 0x00200000, 	// an MF_MISSILE mobj can activate SPAC_IMPACT
	MF2_PUSHWALL		= 0x00400000, 	// mobj can push walls
	MF2_MCROSS			= 0x00800000,	// can activate monster cross lines
	MF2_PCROSS			= 0x01000000,	// can activate projectile cross lines
	MF2_CANTLEAVEFLOORPIC=0x02000000,	// stay within a certain floor type
	MF2_NONSHOOTABLE	= 0x04000000,	// mobj is totally non-shootable,
										// but still considered solid
	MF2_INVULNERABLE	= 0x08000000,	// mobj is invulnerable
	MF2_DORMANT			= 0x10000000,	// thing is dormant
	MF2_ARGSDEFINED		= 0x20000000,	// Internal flag used by DECORATE to signal that the args should not be taken from the mapthing definition
	MF2_SEEKERMISSILE	= 0x40000000,	// is a seeker (for reflection)
	MF2_REFLECTIVE		= 0x80000000,	// reflects missiles
};

// --- mobj.flags3 ---
enum ActorFlag3
{
	MF3_FLOORHUGGER		= 0x00000001,	// Missile stays on floor
	MF3_CEILINGHUGGER	= 0x00000002,	// Missile stays on ceiling
	MF3_NORADIUSDMG		= 0x00000004,	// Actor does not take radius damage
	MF3_GHOST			= 0x00000008,	// Actor is a ghost
	MF3_ALWAYSPUFF		= 0x00000010,	// Puff always appears, even when hit nothing
	MF3_SPECIALFLOORCLIP= 0x00000020,	// Actor uses floorclip for special effect (e.g. Wraith)
	MF3_DONTSPLASH		= 0x00000040,	// Thing doesn't make a splash
	MF3_NOSIGHTCHECK	= 0x00000080,	// Go after first acceptable target without checking sight
	MF3_DONTOVERLAP		= 0x00000100,	// Don't pass over/under other things with this bit set
	MF3_DONTMORPH		= 0x00000200,	// Immune to arti_egg
	MF3_DONTSQUASH		= 0x00000400,	// Death ball can't squash this actor
	MF3_EXPLOCOUNT		= 0x00000800,	// Don't explode until special2 counts to special1
	MF3_FULLVOLACTIVE	= 0x00001000,	// Active sound is played at full volume
	MF3_ISMONSTER		= 0x00002000,	// Actor is a monster
	MF3_SKYEXPLODE		= 0x00004000,	// Explode missile when hitting sky
	MF3_STAYMORPHED		= 0x00008000,	// Monster cannot unmorph
	MF3_DONTBLAST		= 0x00010000,	// Actor cannot be pushed by blasting
	MF3_CANBLAST		= 0x00020000,	// Actor is not a monster but can be blasted
	MF3_NOTARGET		= 0x00040000,	// This actor not targetted when it hurts something else
	MF3_DONTGIB			= 0x00080000,	// Don't gib this corpse
	MF3_NOBLOCKMONST	= 0x00100000,	// Can cross ML_BLOCKMONSTERS lines
	MF3_CRASHED			= 0x00200000,	// Actor entered its crash state
	MF3_FULLVOLDEATH	= 0x00400000,	// DeathSound is played full volume (for missiles)
	MF3_AVOIDMELEE		= 0x00800000,	// Avoids melee attacks (same as MBF's monster_backing but must be explicitly set)
	MF3_SCREENSEEKER    = 0x01000000,	// Fails the IsOkayToAttack test if potential target is outside player FOV
	MF3_FOILINVUL		= 0x02000000,	// Actor can hurt MF2_INVULNERABLE things
	MF3_NOTELEOTHER		= 0x04000000,	// Monster is unaffected by teleport other artifact
	MF3_BLOODLESSIMPACT	= 0x08000000,	// Projectile does not leave blood
	MF3_NOEXPLODEFLOOR	= 0x10000000,	// Missile stops at floor instead of exploding
	MF3_WARNBOT			= 0x20000000,	// Missile warns bot
	MF3_PUFFONACTORS	= 0x40000000,	// Puff appears even when hit bleeding actors
	MF3_HUNTPLAYERS		= 0x80000000,	// Used with TIDtoHate, means to hate players too
};

// --- mobj.flags4 ---
enum ActorFlag4
{
	MF4_NOHATEPLAYERS	= 0x00000001,	// Ignore player attacks
	MF4_QUICKTORETALIATE= 0x00000002,	// Always switch targets when hurt
	MF4_NOICEDEATH		= 0x00000004,	// Actor never enters an ice death, not even the generic one
	MF4_BOSSDEATH		= 0x00000008,	// A_FreezeDeathChunks calls A_BossDeath
	MF4_RANDOMIZE		= 0x00000010,	// Missile has random initial tic count
	MF4_NOSKIN			= 0x00000020,	// Player cannot use skins
	MF4_FIXMAPTHINGPOS	= 0x00000040,	// Fix this actor's position when spawned as a map thing
	MF4_ACTLIKEBRIDGE	= 0x00000080,	// Pickups can "stand" on this actor / cannot be moved by any sector action.
	MF4_STRIFEDAMAGE	= 0x00000100,	// Strife projectiles only do up to 4x damage, not 8x

	MF4_CANUSEWALLS		= 0x00000200,	// Can activate 'use' specials
	//		= 0x00000400,
	//		= 0x00000800,
	MF4_FORCERADIUSDMG	= 0x00001000,	// if put on an object it will override MF3_NORADIUSDMG
	MF4_DONTFALL		= 0x00002000,	// Doesn't have NOGRAVITY disabled when dying.
	MF4_SEESDAGGERS		= 0x00004000,	// This actor can see you striking with a dagger
	MF4_INCOMBAT		= 0x00008000,	// Don't alert others when attacked by a dagger
	MF4_LOOKALLAROUND	= 0x00010000,	// Monster has eyes in the back of its head
	MF4_STANDSTILL		= 0x00020000,	// Monster should not chase targets unless attacked?
	MF4_SPECTRAL		= 0x00040000,
	MF4_SCROLLMOVE		= 0x00080000,	// velocity has been applied by a scroller
	MF4_NOSPLASHALERT	= 0x00100000,	// Splashes don't alert this monster
	MF4_SYNCHRONIZED	= 0x00200000,	// For actors spawned at load-time only: Do not randomize tics
	MF4_NOTARGETSWITCH	= 0x00400000,	// monster never switches target until current one is dead
	MF4_VFRICTION		= 0x00800000,	// Internal flag used by A_PainAttack to push a monster down
	MF4_DONTHARMCLASS	= 0x01000000,	// Don't hurt one's own kind with explosions (hitscans, too?)
	MF4_SHIELDREFLECT	= 0x02000000,
	MF4_DEFLECT			= 0x04000000,	// different projectile reflection styles
	MF4_ALLOWPARTICLES	= 0x08000000,	// this puff type can be replaced by particles
	MF4_NOEXTREMEDEATH	= 0x10000000,	// this projectile or weapon never gibs its victim
	MF4_EXTREMEDEATH	= 0x20000000,	// this projectile or weapon always gibs its victim
	MF4_FRIGHTENED		= 0x40000000,	// Monster runs away from player
	MF4_BOSSSPAWNED		= 0x80000000,	// Spawned by a boss spawn cube
};

// --- mobj.flags5 ---

enum ActorFlag5
{
	MF5_DONTDRAIN		= 0x00000001,	// cannot be drained health from.
	MF5_GETOWNER		= 0x00000002,
	MF5_NODROPOFF		= 0x00000004,	// cannot drop off under any circumstances.
	MF5_NOFORWARDFALL	= 0x00000008,	// Does not make any actor fall forward by being damaged by this
	MF5_COUNTSECRET		= 0x00000010,	// From Doom 64: actor acts like a secret
	MF5_AVOIDINGDROPOFF = 0x00000020,	// Used to move monsters away from dropoffs
	MF5_NODAMAGE		= 0x00000040,	// Actor can be shot and reacts to being shot but takes no damage
	MF5_CHASEGOAL		= 0x00000080,	// Walks to goal instead of target if a valid goal is set.
	MF5_BLOODSPLATTER	= 0x00000100,	// Blood splatter like in Raven's games.
	MF5_OLDRADIUSDMG	= 0x00000200,	// Use old radius damage code (for barrels and boss brain)
	MF5_DEHEXPLOSION	= 0x00000400,	// Use the DEHACKED explosion options when this projectile explodes
	MF5_PIERCEARMOR		= 0x00000800,	// Armor doesn't protect against damage from this actor
	MF5_NOBLOODDECALS	= 0x00001000,	// Actor bleeds but doesn't spawn blood decals
	MF5_USESPECIAL		= 0x00002000,	// Actor executes its special when being 'used'.
	MF5_NOPAIN			= 0x00004000,	// If set the pain state won't be entered
	MF5_ALWAYSFAST		= 0x00008000,	// always uses 'fast' attacking logic
	MF5_NEVERFAST		= 0x00010000,	// never uses 'fast' attacking logic
	MF5_ALWAYSRESPAWN	= 0x00020000,	// always respawns, regardless of skill setting
	MF5_NEVERRESPAWN	= 0x00040000,	// never respawns, regardless of skill setting
	MF5_DONTRIP			= 0x00080000,	// Ripping projectiles explode when hitting this actor
	MF5_NOINFIGHTING	= 0x00100000,	// This actor doesn't switch target when it's hurt
	MF5_NOINTERACTION	= 0x00200000,	// Thing is completely excluded from any gameplay related checks
	MF5_NOTIMEFREEZE	= 0x00400000,	// Actor is not affected by time freezer
	MF5_PUFFGETSOWNER	= 0x00800000,	// [BB] Sets the owner of the puff to the player who fired it
	MF5_SPECIALFIREDAMAGE=0x01000000,	// Special treatment of PhoenixFX1 turned into a flag to remove
										// dependence of main engine code of specific actor types.
	MF5_SUMMONEDMONSTER	= 0x02000000,	// To mark the friendly Minotaur. Hopefully to be generalized later.
	MF5_NOVERTICALMELEERANGE=0x04000000,// Does not check vertical distance for melee range
	MF5_BRIGHT			= 0x08000000,	// Actor is always rendered fullbright
	MF5_CANTSEEK		= 0x10000000,	// seeker missiles cannot home in on this actor
	MF5_INCONVERSATION	= 0x20000000,	// Actor is having a conversation
	MF5_PAINLESS		= 0x40000000,	// Actor always inflicts painless damage.
	MF5_MOVEWITHSECTOR	= 0x80000000,	// P_ChangeSector() will still process this actor if it has MF_NOBLOCKMAP
};

// --- mobj.flags6 ---
enum ActorFlag6
{
	MF6_NOBOSSRIP		= 0x00000001,	// For rippermissiles: Don't rip through bosses.
	MF6_THRUSPECIES		= 0x00000002,	// Actors passes through other of the same species.
	MF6_MTHRUSPECIES	= 0x00000004,	// Missile passes through actors of its shooter's species.
	MF6_FORCEPAIN		= 0x00000008,	// forces target into painstate (unless it has the NOPAIN flag)
	MF6_NOFEAR			= 0x00000010,	// Not scared of frightening players
	MF6_BUMPSPECIAL		= 0x00000020,	// Actor executes its special when being collided (as the ST flag)
	MF6_DONTHARMSPECIES = 0x00000040,	// Don't hurt one's own species with explosions (hitscans, too?)
	MF6_STEPMISSILE		= 0x00000080,	// Missile can "walk" up steps
	MF6_NOTELEFRAG		= 0x00000100,	// [HW] Actor can't be telefragged
	MF6_TOUCHY			= 0x00000200,	// From MBF: killough 11/98: dies when solids touch it
	MF6_CANJUMP			= 0x00000400,	// From MBF: a dedicated flag instead of the BOUNCES+FLOAT+sentient combo
	MF6_JUMPDOWN		= 0x00000800,	// From MBF: generalization of dog behavior wrt. dropoffs.
	MF6_VULNERABLE		= 0x00001000,	// Actor can be damaged (even if not shootable).
	MF6_ARMED			= 0x00002000,	// From MBF: Object is armed (for MF6_TOUCHY objects)
	MF6_FALLING			= 0x00004000,	// From MBF: Object is falling (for pseudotorque simulation)
	MF6_LINEDONE		= 0x00008000,	// From MBF: Object has already run a line effect
	MF6_NOTRIGGER		= 0x00010000,	// actor cannot trigger any line actions
	MF6_SHATTERING		= 0x00020000,	// marks an ice corpse for forced shattering
	MF6_KILLED			= 0x00040000,	// Something that was killed (but not necessarily a corpse)
	MF6_BLOCKEDBYSOLIDACTORS = 0x00080000, // Blocked by solid actors, even if not solid itself
	MF6_ADDITIVEPOISONDAMAGE	= 0x00100000,
	MF6_ADDITIVEPOISONDURATION	= 0x00200000,
	MF6_NOMENU			= 0x00400000,	// Player class should not appear in the class selection menu.
	MF6_BOSSCUBE		= 0x00800000,	// Actor spawned by A_BrainSpit, flagged for timefreeze reasons.
	MF6_SEEINVISIBLE	= 0x01000000,	// Monsters can see invisible player.
	MF6_DONTCORPSE		= 0x02000000,	// [RC] Don't autoset MF_CORPSE upon death and don't force Crash state change.
	MF6_POISONALWAYS	= 0x04000000,	// Always apply poison, even when target can't take the damage.
	MF6_DOHARMSPECIES	= 0x08000000,	// Do hurt one's own species with projectiles.
	MF6_INTRYMOVE		= 0x10000000,	// Executing P_TryMove
	MF6_NOTAUTOAIMED	= 0x20000000,	// Do not subject actor to player autoaim.
	MF6_NOTONAUTOMAP	= 0x40000000,	// will not be shown on automap with the 'scanner' powerup.
	MF6_RELATIVETOFLOOR	= 0x80000000,	// [RC] Make flying actors be affected by lifts.
};

// --- mobj.flags7 ---
enum ActorFlag7
{
	MF7_NEVERTARGET		= 0x00000001,	// can not be targetted at all, even if monster friendliness is considered.
	MF7_NOTELESTOMP		= 0x00000002,	// cannot telefrag under any circumstances (even when set by MAPINFO)
	MF7_ALWAYSTELEFRAG	= 0x00000004,	// will unconditionally be telefragged when in the way. Overrides all other settings.
	MF7_HANDLENODELAY	= 0x00000008,	// respect NoDelay state flag
	MF7_WEAPONSPAWN		= 0x00000010,	// subject to DF_NO_COOP_WEAPON_SPAWN dmflag
	MF7_HARMFRIENDS		= 0x00000020,	// is allowed to harm friendly monsters.
	MF7_BUDDHA			= 0x00000040,	// Behaves just like the buddha cheat.
	MF7_FOILBUDDHA		= 0x00000080,	// Similar to FOILINVUL, foils buddha mode.
	MF7_DONTTHRUST		= 0x00000100,	// Thrusting functions do not take, and do not give thrust (damage) to actors with this flag.
	MF7_ALLOWPAIN		= 0x00000200,	// Invulnerable or immune (via damagefactors) actors can still react to taking damage even if they don't.
	MF7_CAUSEPAIN		= 0x00000400,	// Damage sources with this flag can cause similar effects like ALLOWPAIN.
	MF7_THRUREFLECT		= 0x00000800,	// Actors who are reflective cause the missiles to not slow down or change angles.
	MF7_MIRRORREFLECT	= 0x00001000,	// Actor is turned directly 180 degrees around when reflected.
	MF7_AIMREFLECT		= 0x00002000,	// Actor is directly reflected straight back at the one who fired the projectile.
	MF7_HITTARGET		= 0x00004000,	// The actor the projectile dies on is set to target, provided it's targetable anyway.
	MF7_HITMASTER		= 0x00008000,	// Same as HITTARGET, except it's master instead of target.
	MF7_HITTRACER		= 0x00010000,	// Same as HITTARGET, but for tracer.
	MF7_FLYCHEAT		= 0x00020000,	// must be part of the actor so that it can be tracked properly
	MF7_NODECAL			= 0x00040000,	// [ZK] Forces puff to have no impact decal
	MF7_FORCEDECAL		= 0x00080000,	// [ZK] Forces puff's decal to override the weapon's.
	MF7_LAXTELEFRAGDMG	= 0x00100000,	// [MC] Telefrag damage can be reduced.
	MF7_ICESHATTER		= 0x00200000,	// [MC] Shatters ice corpses regardless of damagetype.
	MF7_ALLOWTHRUFLAGS	= 0x00400000,	// [MC] Allow THRUACTORS and the likes on puffs to prevent mod breakage.
	MF7_USEKILLSCRIPTS	= 0x00800000,	// [JM] Use "KILL" Script on death if not forced by GameInfo.
	MF7_NOKILLSCRIPTS	= 0x01000000,	// [JM] No "KILL" Script on death whatsoever, even if forced by GameInfo.
	MF7_SPRITEANGLE		= 0x02000000,	// [MC] Utilize the SpriteAngle property and lock the rotation to the degrees specified.
	MF7_SMASHABLE		= 0x04000000,	// dies if hitting the floor.
	MF7_NOSHIELDREFLECT = 0x08000000,	// will not be reflected by shields.
	MF7_FORCEZERORADIUSDMG = 0x10000000,// passes zero radius damage on to P_DamageMobj, this is necessary in some cases where DoSpecialDamage gets overrideen.
	MF7_NOINFIGHTSPECIES = 0x20000000,	// don't start infights with one's own species.
	MF7_FORCEINFIGHTING	= 0x40000000,	// overrides a map setting of 'no infighting'.
	MF7_INCHASE			= 0x80000000,	// [RH] used by A_Chase and A_Look to avoid recursion
};

// --- mobj.flags8 ---
enum ActorFlag8
{
	MF8_FRIGHTENING		= 0x00000001,	// for those moments when halloween just won't do
	MF8_INSCROLLSEC		= 0x00000002,	// actor is partially inside a scrolling sector
	MF8_BLOCKASPLAYER	= 0x00000004,	// actor is blocked by player-blocking lines even if not a player
	MF8_DONTFACETALKER	= 0x00000008,	// don't alter the angle to face the player in conversations
	MF8_HITOWNER		= 0x00000010,	// projectile can hit the actor that fired it
	MF8_NOFRICTION		= 0x00000020,	// friction doesn't apply to the actor at all
	MF8_NOFRICTIONBOUNCE	= 0x00000040,	// don't bounce off walls when on icy floors
	MF8_RETARGETAFTERSLAM	= 0x00000080,	// Forces jumping to the idle state after slamming into something
	MF8_RECREATELIGHTS	= 0x00000100,	// Internal flag that signifies that the light attachments need to be recreated at the
	MF8_STOPRAILS		= 0x00000200,	// [MC] Prevent rails from going further if an actor has this flag.
	MF8_ABSVIEWANGLES	= 0x00000400,	// [MC] By default view angle/pitch/roll is an offset. This will make it absolute instead.
	MF8_FALLDAMAGE		= 0x00000800,	// Monster will take fall damage regardless of map settings.
	MF8_MINVISIBLE		= 0x00001000,	// Actor not visible to monsters
	MF8_MVISBLOCKED		= 0x00002000,	// Monster(only) sight checks to actor always fail
	MF8_BLOCKLOF		= 0x00004000,	// [LOFBLOCKERS] stops CheckLOF like cover (GZSelaco 7c117a8013, same bit)
	MF8_ALLOWTHRUBITS	= 0x00008000,	// [MC] Enable ThruBits property
	MF8_FULLVOLSEE		= 0x00010000,	// Play see sound at full volume
	MF8_E1M8BOSS		= 0x00020000,	// MBF21 boss death.
	MF8_E2M8BOSS		= 0x00040000,	// MBF21 boss death.
	MF8_E3M8BOSS		= 0x00080000,	// MBF21 boss death.
	MF8_E4M8BOSS		= 0x00100000,	// MBF21 boss death.
	MF8_E4M6BOSS		= 0x00200000,	// MBF21 boss death.
	MF8_MAP07BOSS1		= 0x00400000,	// MBF21 boss death.
	MF8_MAP07BOSS2		= 0x00800000,	// MBF21 boss death.
	MF8_AVOIDHAZARDS	= 0x01000000,	// MBF AI enhancement.
	MF8_STAYONLIFT		= 0x02000000,	// MBF AI enhancement.
	MF8_DONTFOLLOWPLAYERS	= 0x04000000,	// [inkoalawetrust] Friendly monster will not follow players.
	MF8_SEEFRIENDLYMONSTERS	= 0X08000000,	// [inkoalawetrust] Hostile monster can see friendly monsters.
	MF8_CROSSLINECHECK	= 0x10000000,	// [MC] Enables CanCrossLine virtual
	MF8_MASTERNOSEE		= 0x20000000,	// Don't show object in first person if their master is the current camera.
	MF8_ADDLIGHTLEVEL	= 0x40000000,	// [MC] Actor light level is additive with sector.
	MF8_ONLYSLAMSOLID	= 0x80000000,	// [B] Things with skullfly will ignore non-solid Actors.
};

// --- mobj.flags9 ---
enum ActorFlag9
{
	MF9_SHADOWAIM				= 0x00000001,	// [inkoalawetrust] Monster still gets aim penalty from aiming at shadow actors even with MF6_SEEINVISIBLE on.
	MF9_DOSHADOWBLOCK			= 0x00000002,	// [inkoalawetrust] Should the monster look for SHADOWBLOCK actors ?
	MF9_SHADOWBLOCK				= 0x00000004,	// [inkoalawetrust] Actors in the line of fire with this flag trigger the MF_SHADOW aiming penalty.
	MF9_SHADOWAIMVERT			= 0x00000008,	// [inkoalawetrust] Monster aim is also offset vertically when aiming at shadow actors.
	MF9_DECOUPLEDANIMATIONS		= 0x00000010,	// [Jay] Decouple model animations from states
	MF9_NOSECTORDAMAGE			= 0x00000020,	// [inkoalawetrust] Actor ignores any sector-based damage (i.e damaging floors, NOT crushers)
	MF9_ISPUFF					= 0x00000040,	// [AA] Set on actors by P_SpawnPuff
	MF9_FORCESECTORDAMAGE		= 0x00000080,	// [inkoalawetrust] Actor ALWAYS takes hurt floor damage if there's any. Even if the floor doesn't have SECMF_HURTMONSTERS.
	MF9_NOAUTOOFFSKULLFLY		= 0x00000100,	// Don't automatically disable MF_SKULLFLY if velocity is 0.
	MF9_PRECACHEALWAYS			= 0x00000200,	// [Selaco] Load this class's graphics at every level start, placed or not (p_setup.cpp PrecacheLevel, hw_precache.cpp)
	// RS FORK -- WORLD CLOCK ("Engine docs/SLOWMO_PLAN.md"). +REALTIME keeps this actor
	// on the REAL clock: it ticks every real tic and is drawn on the real fraction, even
	// when the world is slowed. It is for things that follow the player's body -- VR
	// hands, held gun props and markers, weapon-wheel parts, the Lance's anchor -- and
	// each owning mod sets it on its own actors. OFF by default; with the world at full
	// speed nothing ever asks.
	MF9_REALTIME				= 0x00000400,
	// GZSelaco flags whose MF8 bits are taken here, so they live in MF9's top bits (scripts only use the flag names).
	MF9_HITSCANTHRU				= 0x20000000,	// [HITCALLBACKS] hitscans hurt this actor and carry on through it (c7527eead1)
	MF9_ABSDAMAGE				= 0x40000000,	// [ABSDAMAGE] missile/puff damage is DamageVal exactly, no dice roll (96096c0228)
	MF9_BLOCKLOS				= 0x80000000,	// [LOFBLOCKERS] stops CheckLOF like cover unless CLOFF_SKIPLOS (fe21bdb831, same bit)
};

// --- mobj.renderflags ---
enum ActorRenderFlag
{
	RF_XFLIP			= 0x0001,	// Flip sprite horizontally
	RF_YFLIP			= 0x0002,	// Flip sprite vertically
	RF_ONESIDED			= 0x0004,	// Wall/floor sprite is visible from front only
	RF_FULLBRIGHT		= 0x0010,	// Sprite is drawn at full brightness

	RF_RELMASK			= 0x0300,	// ---Relative z-coord for bound actors (these obey texture pegging)
	RF_RELABSOLUTE		= 0x0000,	// Actor z is absolute
	RF_RELUPPER			= 0x0100,	// Actor z is relative to upper part of wall
	RF_RELLOWER			= 0x0200,	// Actor z is relative to lower part of wall
	RF_RELMID			= 0x0300,	// Actor z is relative to middle part of wall

	RF_CLIPMASK			= 0x0c00,	// ---Clipping for bound actors
	RF_CLIPFULL			= 0x0000,	// Clip sprite to full height of wall
	RF_CLIPUPPER		= 0x0400,	// Clip sprite to upper part of wall
	RF_CLIPMID			= 0x0800,	// Clip sprite to mid part of wall
	RF_CLIPLOWER		= 0x0c00,	// Clip sprite to lower part of wall

	RF_DECALMASK		= RF_RELMASK|RF_CLIPMASK,

	RF_SPRITETYPEMASK	= 0x7000,	// ---Different sprite types, not all implemented
	RF_FACESPRITE		= 0x0000,	// Face sprite
	RF_WALLSPRITE		= 0x1000,	// Wall sprite
	RF_FLATSPRITE		= 0x2000,	// Flat sprite
	RF_VOXELSPRITE		= 0x3000,	// Voxel object
	RF_INVISIBLE		= 0x8000,	// Don't bother drawing this actor
	RF_FORCEYBILLBOARD	= 0x10000,	// [BB] OpenGL only: draw with y axis billboard, i.e. anchored to the floor (overrides gl_billboard_mode setting)
	RF_FORCEXYBILLBOARD	= 0x20000,	// [BB] OpenGL only: draw with xy axis billboard, i.e. unanchored (overrides gl_billboard_mode setting)
	RF_ROLLSPRITE		= 0x40000,	//[marrub]roll the sprite billboard
	RF_DONTFLIP			= 0x80000,	// Don't flip it when viewed from behind.
	RF_ROLLCENTER		= 0x00100000, // Rotate from the center of sprite instead of offsets
	RF_MASKROTATION		= 0x00200000, // [MC] Only draw the actor when viewed from a certain angle range.
	RF_ABSMASKANGLE		= 0x00400000, // [MC] The mask rotation does not offset by the actor's angle.
	RF_ABSMASKPITCH		= 0x00800000, // [MC] The mask rotation does not offset by the actor's pitch.
	RF_INTERPOLATEANGLES = 0x01000000, // [MC] Allow interpolation of the actor's angle, pitch and roll.
	RF_MAYBEINVISIBLE	= 0x02000000,
	RF_DONTINTERPOLATE	= 0x04000000,	// no render interpolation ever!

	RF_SPRITEFLIP		= 0x08000000,	// sprite flipped on x-axis
	RF_ZDOOMTRANS		= 0x10000000,	// is not normally transparent in Vanilla Doom
	RF_CASTSPRITESHADOW = 0x20000000,	// actor will cast a sprite shadow
	RF_NOINTERPOLATEVIEW = 0x40000000,	// don't interpolate the view next frame if this actor is a camera.
	RF_NOSPRITESHADOW	= 0x80000000,	// actor will not cast a sprite shadow
};

enum ActorRenderFlag2
{
	RF2_INVISIBLEINMIRRORS		= 0x0001,	// [Nash] won't render in mirrors
	RF2_ONLYVISIBLEINMIRRORS	= 0x0002,	// [Nash] only renders in mirrors
	RF2_BILLBOARDFACECAMERA		= 0x0004,	// Sprite billboard face camera (override gl_billboard_faces_camera)
	RF2_BILLBOARDNOFACECAMERA	= 0x0008,	// Sprite billboard face camera angle (override gl_billboard_faces_camera)
	RF2_FLIPSPRITEOFFSETX		= 0x0010,
	RF2_FLIPSPRITEOFFSETY		= 0x0020,
	RF2_CAMFOLLOWSPLAYER		= 0x0040,	// Matches the cam's base position and angles to the main viewpoint.
	RF2_ISOMETRICSPRITES		= 0x0080,
	RF2_SQUAREPIXELS			= 0x0100,	// apply +ROLLSPRITE scaling math so that non rolling sprites get the same scaling
	RF2_STRETCHPIXELS			= 0x0200,	// don't apply SQUAREPIXELS for ROLLSPRITES
	RF2_LIGHTMULTALPHA			= 0x0400,	// attached lights use alpha as intensity multiplier
	RF2_ANGLEDROLL				= 0x0800,	// Sprite roll amount depends on (actor.Angle - actor.AngledRollOffset)
	RF2_INTERPOLATESCALE		= 0x1000,
	RF2_INTERPOLATEALPHA		= 0x2000,
	RF2_NODYNAMICLIGHTING		= 0x4000,	// [MC] Disable dynamic lighting effects on sprites/models
};

// This translucency value produces the closest match to Heretic's TINTTAB.
// ~40% of the value of the overlaid image shows through.
const double HR_SHADOW = (0x6800 / 65536.);
// Hexen's TINTTAB is the same as Heretic's, just reversed.
const double HX_SHADOW = (0x9800 / 65536.);
const double HX_ALTSHADOW = (0x6800 / 65536.);

// This could easily be a bool but then it'd be much harder to find later. ;)
enum replace_t
{
	NO_REPLACE = 0,
	ALLOW_REPLACE = 1
};

enum ActorBounceFlag
{
	BOUNCE_Walls = 1<<0,		// bounces off of walls
	BOUNCE_Floors = 1<<1,		// bounces off of floors
	BOUNCE_Ceilings = 1<<2,		// bounces off of ceilings
	BOUNCE_Actors = 1<<3,		// bounces off of some actors
	BOUNCE_AllActors = 1<<4,	// bounces off of all actors (requires BOUNCE_Actors to be set, too)
	BOUNCE_AutoOff = 1<<5,		// when bouncing off a sector plane, if the new Z velocity is below 3.0, disable further bouncing
	BOUNCE_HereticType = 1<<6,	// goes into Death state when bouncing on floors or ceilings

	BOUNCE_UseSeeSound = 1<<7,	// compatibility fallback. This will only be set by
								// the compatibility handlers for the old bounce flags.
	BOUNCE_NoWallSound = 1<<8,	// don't make noise when bouncing off a wall
	BOUNCE_Quiet = 1<<9,		// Strife's grenades don't make a bouncing sound
	BOUNCE_ExplodeOnWater = 1<<10,	// explodes when hitting a water surface
	BOUNCE_CanBounceWater = 1<<11,	// can bounce on water
	// MBF bouncing is a bit different from other modes as Killough coded many special behavioral cases
	// for them that are not present in ZDoom, so it is necessary to identify it properly.
	BOUNCE_MBF = 1<<12,			// This in itself is not a valid mode, but replaces MBF's MF_BOUNCE flag.
	BOUNCE_AutoOffFloorOnly = 1<<13,		// like BOUNCE_AutoOff, but only on floors
	BOUNCE_UseBounceState = 1<<14,	// Use Bounce[.*] states
	BOUNCE_NotOnShootables = 1<<15,	// do not bounce off shootable actors if we are a projectile. Explode instead.
	BOUNCE_BounceOnUnrips = 1<<16,	// projectile bounces on actors with DONTRIP
	BOUNCE_NotOnSky = 1<<17,		// Don't bounce on sky floors / ceilings / walls
	BOUNCE_DEH = 1<<18,				// Flag was set through Dehacked.
	BOUNCE_KeepAngle = 1<<19,		// Don't change yaw when bouncing off a surface.
	BOUNCE_ModifyPitch = 1<<20,		// Change pitch when bouncing off a surface.

	BOUNCE_TypeMask = BOUNCE_Walls | BOUNCE_Floors | BOUNCE_Ceilings | BOUNCE_Actors | BOUNCE_AutoOff | BOUNCE_HereticType | BOUNCE_MBF,

	// The three "standard" types of bounciness are:
	// HERETIC - Missile will only bounce off the floor once and then enter
	//			 its death state. It does not bounce off walls at all.
	// HEXEN -	 Missile bounces off of walls and floors indefinitely.
	// DOOM -	 Like Hexen, but the bounce turns off if its vertical velocity
	//			 is too low.
	BOUNCE_None = 0,
	BOUNCE_Heretic = BOUNCE_Floors | BOUNCE_Ceilings | BOUNCE_HereticType,
	BOUNCE_Doom = BOUNCE_Walls | BOUNCE_Floors | BOUNCE_Ceilings | BOUNCE_Actors | BOUNCE_AutoOff,
	BOUNCE_Hexen = BOUNCE_Walls | BOUNCE_Floors | BOUNCE_Ceilings | BOUNCE_Actors,
	BOUNCE_Grenade = BOUNCE_MBF | BOUNCE_Doom,		// Bounces on walls and flats like ZDoom bounce.
	BOUNCE_Classic = BOUNCE_MBF | BOUNCE_Floors | BOUNCE_Ceilings,	// Bounces on flats only, but
																	// does not die when bouncing.

	// combined types
	BOUNCE_DoomCompat = BOUNCE_Doom | BOUNCE_UseSeeSound,
	BOUNCE_HereticCompat = BOUNCE_Heretic | BOUNCE_UseSeeSound,
	BOUNCE_HexenCompat = BOUNCE_Hexen | BOUNCE_UseSeeSound

	// The distinction between BOUNCE_Actors and BOUNCE_AllActors: A missile with
	// BOUNCE_Actors set will bounce off of reflective and "non-sentient" actors.
	// A missile that also has BOUNCE_AllActors set will bounce off of any actor.
	// For compatibility reasons when BOUNCE_Actors was implied by the bounce type
	// being "Doom" or "Hexen" and BOUNCE_AllActors was the separate
	// MF5_BOUNCEONACTORS, you must set BOUNCE_Actors for BOUNCE_AllActors to have
	// an effect.


};

// this is a special flag set that is exposed directly to DECORATE/ZScript
// these flags are for filtering actor visibility based on certain conditions of the renderer's feature support.
// currently, no renderer supports every single one of these features.
enum ActorRenderFeatureFlag
{
	RFF_FLATSPRITES		= 1<<0, // flat sprites
	RFF_MODELS			= 1<<1, // 3d models
	RFF_SLOPE3DFLOORS	= 1<<2, // sloped 3d floor support
	RFF_TILTPITCH		= 1<<3, // full free-look
	RFF_ROLLSPRITES		= 1<<4, // roll sprites
	RFF_UNCLIPPEDTEX	= 1<<5, // midtex and sprite can render "into" flats and walls
	RFF_MATSHADER		= 1<<6, // material shaders
	RFF_POSTSHADER		= 1<<7, // post-process shaders (renderbuffers)
	RFF_BRIGHTMAP		= 1<<8, // brightmaps
	RFF_COLORMAP		= 1<<9, // custom colormaps (incl. ability to fullbright certain ranges, ala Strife)
	RFF_POLYGONAL		= 1<<10, // uses polygons instead of wallscans/visplanes (i.e. softpoly and hardware opengl)
	RFF_TRUECOLOR		= 1<<11, // renderer is currently truecolor
	RFF_VOXELS		= 1<<12, // renderer is capable of voxels
};

// [TP] Flagset definitions
typedef TFlags<ActorFlag> ActorFlags;
typedef TFlags<ActorFlag2> ActorFlags2;
typedef TFlags<ActorFlag3> ActorFlags3;
typedef TFlags<ActorFlag4> ActorFlags4;
typedef TFlags<ActorFlag5> ActorFlags5;
typedef TFlags<ActorFlag6> ActorFlags6;
typedef TFlags<ActorFlag7> ActorFlags7;
typedef TFlags<ActorFlag8> ActorFlags8;
typedef TFlags<ActorFlag9> ActorFlags9;
typedef TFlags<ActorRenderFlag> ActorRenderFlags;
typedef TFlags<ActorRenderFlag2> ActorRenderFlags2;
typedef TFlags<ActorBounceFlag> ActorBounceFlags;
typedef TFlags<ActorRenderFeatureFlag> ActorRenderFeatureFlags;
DEFINE_TFLAGS_OPERATORS (ActorFlags)
DEFINE_TFLAGS_OPERATORS (ActorFlags2)
DEFINE_TFLAGS_OPERATORS (ActorFlags3)
DEFINE_TFLAGS_OPERATORS (ActorFlags4)
DEFINE_TFLAGS_OPERATORS (ActorFlags5)
DEFINE_TFLAGS_OPERATORS (ActorFlags6)
DEFINE_TFLAGS_OPERATORS (ActorFlags7)
DEFINE_TFLAGS_OPERATORS (ActorFlags8)
DEFINE_TFLAGS_OPERATORS (ActorFlags9)
DEFINE_TFLAGS_OPERATORS (ActorRenderFlags)
DEFINE_TFLAGS_OPERATORS (ActorRenderFlags2)
DEFINE_TFLAGS_OPERATORS (ActorBounceFlags)
DEFINE_TFLAGS_OPERATORS (ActorRenderFeatureFlags)

// Used to affect the logic for thing activation through death, USESPECIAL and BUMPSPECIAL
// "thing" refers to what has the flag and the special, "trigger" refers to what used or bumped it
enum EThingSpecialActivationType
{
	THINGSPEC_Default			= 0,		// Normal behavior: a player must be the trigger, and is the activator
	THINGSPEC_ThingActs			= 1,		// The thing itself is the activator of the special
	THINGSPEC_ThingTargets		= 1<<1,		// The thing changes its target to the trigger
	THINGSPEC_TriggerTargets	= 1<<2,		// The trigger changes its target to the thing
	THINGSPEC_MonsterTrigger	= 1<<3,		// The thing can be triggered by a monster
	THINGSPEC_MissileTrigger	= 1<<4,		// The thing can be triggered by a projectile
	THINGSPEC_ClearSpecial		= 1<<5,		// Clears special after successful activation
	THINGSPEC_NoDeathSpecial	= 1<<6,		// Don't activate special on death
	THINGSPEC_TriggerActs		= 1<<7,		// The trigger is the activator of the special
											// (overrides LEVEL_ACTOWNSPECIAL Hexen hack)
	THINGSPEC_Activate			= 1<<8,		// The thing is activated when triggered
	THINGSPEC_Deactivate		= 1<<9,		// The thing is deactivated when triggered
	THINGSPEC_Switch			= 1<<10,	// The thing is alternatively activated and deactivated when triggered
};

#define ONFLOORZ		FIXED_MIN
#define ONCEILINGZ		FIXED_MAX
#define FLOATRANDZ		(FIXED_MAX-1)


class FDecalBase;

// Null when the class does not exist: -nostockactors leaves the stock game classes out (GZSelaco 766321f356).
inline AActor *GetDefaultByName (const char *name)
{
	PClass *pc = PClass::FindClass(name);
	return pc != nullptr ? (AActor *)pc->Defaults : nullptr;
}

inline AActor* GetDefaultByName(FName name)
{
	PClass *pc = PClass::FindClass(name);
	return pc != nullptr ? (AActor *)pc->Defaults : nullptr;
}

inline AActor *GetDefaultByType (const PClass *type)
{
	return (AActor *)(type->Defaults);
}

template<class T>
inline T *GetDefault ()
{
	return (T *)(RUNTIME_CLASS_CASTLESS(T)->Defaults);
}

struct line_t;
struct secplane_t;
struct msecnode_t;
struct FStrifeDialogueNode;

struct FLinkContext
{
	msecnode_t *sector_list = nullptr;
	msecnode_t *render_list = nullptr;
};

struct FDropItem
{
	FDropItem *Next;
	FName Name;
	int Probability;
	int Amount;
};

enum EViewPosFlags // [MC] Flags for SetViewPos.
{
	VPSF_ABSOLUTEOFFSET =		1 << 1,			// Don't include angles.
	VPSF_ABSOLUTEPOS =			1 << 2,			// Use absolute position.
	VPSF_ALLOWOUTOFBOUNDS =	1 << 3,			// Allow viewpoint to go out of bounds (hardware renderer only).
	VPSF_ORTHOGRAPHIC =		1 << 4,			// Use orthographic projection (hardware renderer only).
};

struct ModelOverride
{
	int modelID;
	TArray<FTextureID> surfaceSkinIDs;
};

struct AnimModelOverride
{
	int id;

	AnimModelOverride() = default;

	AnimModelOverride(int i) : id(i) {}
	operator int() { return id; }
};

enum EModelDataFlags
{
	MODELDATA_HADMODEL =				1 << 0,
	MODELDATA_OVERRIDE_FLAGS =			1 << 1,
	MODELDATA_GET_BONE_INFO  =			1 << 2,
	MODELDATA_GET_BONE_INFO_RECALC  =	1 << 3, // RECALCULATE BONE INFO INSTANTLY WHEN STATE/ANIMATION CHANGES, MIGHT GET EXPENSIVE








	MODELDATA_IQMFLAGS = MODELDATA_GET_BONE_INFO | MODELDATA_GET_BONE_INFO_RECALC,
};

class DActorModelData : public DObject
{
	DECLARE_CLASS(DActorModelData, DObject);
public:
	PClass *					 modelDef;
	TArray<ModelOverride>		 models;
	TArray<FTextureID>			 skinIDs;
	TArray<AnimModelOverride>	 animationIDs;
	TArray<int>					 modelFrameGenerators;
	TArray<TArray<BoneOverride>> modelBoneOverrides;
	TArray<BoneInfo>			 modelBoneInfo;
	int							 flags;
	int							 overrideFlagsSet;
	int							 overrideFlagsClear;

	AnimInfo anims;

	// RS FORK -- state -> model frame remap (FORK_CHANGES.md, "Native state
	// remap"). Keyed by FState pointer cast to intptr_t; the value packs two
	// non-negative int32s: (frame << 32) | next. Filled from ZScript at bind
	// time via Actor.RegisterModelStateFrame; consulted every rendered frame
	// by CalcModelFrame / CalcModelOverrides, which makes the psprite's own
	// current state the animation clock -- no per-tick script anywhere.
	// Deliberately NOT serialized: state pointers do not survive a session,
	// and binds re-register the table on load anyway.
	TMap<intptr_t, int64_t>		stateRemap;

	// RS FORK -- PER-SURFACE OVERRIDES FOR A WORLD MODEL.
	//
	// The same feature DPSprite carries, in the same names, for a model that is
	// NOT on a psprite. A gun held in world space has no psprite to hang these
	// on, so without them a world weapon's slide cannot move at all -- which is
	// the single thing standing between a VR gun in the hand and a VR gun you
	// can operate.
	//
	// SAME NAMES ON PURPOSE. Script that pulls a slide should read identically
	// whichever way the gun is drawn; a parallel world-only vocabulary is two
	// APIs that drift apart the first time one of them gains a feature.
	//
	// HERE RATHER THAN ON AActor because this class is already the per-actor
	// model state, is already handed to CalcModelOverrides, is allocated only
	// for actors that use models at all, and already serializes -- so a save
	// keeps a half-pulled slide where it was left. Sixteen slots times eight
	// arrays on every actor in a map would be real memory for nothing.
	//
	// THE UNIT OF SurfOvPos IS FRAMES: 3.5 is halfway between mesh frame 3 and
	// mesh frame 4. Not map units, not a 0..1 fraction. Stated here because
	// nothing downstream can check it -- see the same note on DPSprite.
	static constexpr int RS_SURF_SLOTS = 16;

	int   SurfOvModel  [RS_SURF_SLOTS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
	int   SurfOvSurface[RS_SURF_SLOTS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
	int   SurfOvFrame  [RS_SURF_SLOTS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
	int   SurfOvNext   [RS_SURF_SLOTS] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
	float SurfOvLerp   [RS_SURF_SLOTS] =
		{ -1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f };
	bool  SurfOvHidden [RS_SURF_SLOTS] = {};
	float SurfOvPos    [RS_SURF_SLOTS] =
		{ -1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f };
	float SurfOvPosPrev[RS_SURF_SLOTS] =
		{ -1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f };

	// A LIVE TRANSFORM ON TOP OF THE FRAME, per slot. See
	// FModelSurfaceOverride in model.h for what this is for -- in short, a
	// frame is a baked pose and a rigid part also needs to be able to be
	// somewhere the author did not bake.
	//
	// Rotation is a quaternion, xyzw. All-zero means never written and is
	// read as identity, so a caller that only ever translates never has to
	// think about it.
	bool     SurfOvHasXf[RS_SURF_SLOTS] = {};
	FVector3 SurfOvOfs  [RS_SURF_SLOTS] = {};
	FVector4 SurfOvRot  [RS_SURF_SLOTS] = {};

	// LAST TIC'S TRANSFORM, for the same reason SurfOvPosPrev exists.
	//
	// The frame path got display-rate smoothing and the transform path did
	// not, so a part driven by SetModelSurfaceOffset stepped at 35 Hz while
	// the frame underneath it glided -- which is worse than either alone,
	// because the two halves of one part's motion disagreed.
	FVector3 SurfOvOfsPrev[RS_SURF_SLOTS] = {};
	FVector4 SurfOvRotPrev[RS_SURF_SLOTS] = {};

	// ---- E2: DRAW-RATE HAND DRIVE -----------------------------------------
	//
	// THE ONE THING TIC-RATE MOTION CANNOT DO.
	//
	// Everything above is written by script at 35 Hz and interpolated to the
	// drawn instant. That is smooth, and smooth is not the same as GLUED: the
	// value being interpolated toward is still a tic old, so a part a hand is
	// dragging trails the hand by up to one tic no matter how prettily it gets
	// there. At a fast reload snatch that is centimetres.
	//
	// A DRIVEN slot skips script entirely. The renderer reads the live
	// controller pose on the frame it is drawing, projects it onto the part's
	// own travel axis, and places the part -- so the part and the hand are
	// resolved from the same pose at the same instant and cannot separate.
	//
	// ONE FHandDrive PER SLOT (r_data/model_handdrive.h): armed not anchored, a
	// turn as it travels, a hinge, a second stage, and the drawn value published
	// back for script (GetModelSurfaceDrawnValue). What every field means, and
	// the solver that reads and writes them, live there -- shared with every
	// other kind of part a hand drives, so the two can never disagree.
	//
	// NOT SERIALIZED: a drive is a live hand, and a save has no hand in it to
	// restore.
	FHandDrive SurfDrive[RS_SURF_SLOTS];

	bool AnySurfaceOverride() const
	{
		for (int i = 0; i < RS_SURF_SLOTS; i++)
			if (SurfOvModel[i] >= 0) return true;
		return false;
	}

	// Once per tic, BEFORE script runs, so the renderer has both ends to blend
	// between. Without it a slide your own hand is pulling steps at 35 Hz
	// instead of gliding -- the exact problem SurfOvPos exists to solve, and it
	// does not solve it unless somebody keeps last tic's value.
	//
	// "BEFORE SCRIPT RUNS" IS LOAD-BEARING AND WAS WRONG FOR THE WHOLE LIFE OF
	// THIS FEATURE. This used to be called from AActor::Tick, which runs under
	// RunThinkers -- and RunThinkers runs AFTER localEventManager->WorldTick(),
	// which is where every EventHandler-driven prop in this project writes its
	// surface positions. So the shift happened after the new value had already
	// landed: Prev was copied from a Pos that was already this tic's, the two
	// were identical every frame, and the renderer dutifully interpolated
	// between a value and itself. Every world prop stepped at tic rate and the
	// smoothing this function exists for never ran once.
	//
	// It is now called from P_Ticker's ClearInterpolation sweep, at the same
	// instant AActor::Prev is taken and beams are snapshotted -- the one point
	// in the tic where the arrays still hold exactly what was drawn for the tic
	// that just ended, and before any writer has run.
	void ShiftSurfacePositions()
	{
		for (int i = 0; i < RS_SURF_SLOTS; i++)
		{
			SurfOvPosPrev[i] = SurfOvPos[i];
			SurfOvOfsPrev[i] = SurfOvOfs[i];
			SurfOvRotPrev[i] = SurfOvRot[i];
		}
	}

	DActorModelData() = default;
	virtual void Serialize(FSerializer& arc) override;
	virtual void OnDestroy() override;
};

class DViewPosition : public DObject
{
	DECLARE_CLASS(DViewPosition, DObject);
public:
	// Variables
	// Exposed to ZScript
	DVector3	Offset;
	int			Flags;

	// Functions
	void Set(DVector3 &off, int f = -1)
	{
		ZeroSubnormalsF(off.X);
		ZeroSubnormalsF(off.Y);
		ZeroSubnormalsF(off.Z);
		Offset = off;

		if (f > -1)
			Flags = f;
	}

	bool isZero() const
	{
		return Offset.isZero();
	}

	void Serialize(FSerializer& arc) override;
};

class DBehavior final : public DObject
{
	DECLARE_CLASS(DBehavior, DObject)
	HAS_OBJECT_POINTERS
public:
	TObjPtr<AActor*> Owner;
	FLevelLocals* Level;

	void Serialize(FSerializer& arc) override;
	void OnDestroy() override;
};

const double MinVel = EQUAL_EPSILON;

// Map Object definition.
// Flat aim direction from yaw and pitch. The default for AActor::AttackDir and
// OffhandDir until a VR mode installs a controller-aware one (VRMode::SetUp).
// Without it both pointers are null for the tics between a pawn spawning and
// the first rendered frame, while the playsim has already set
// OverrideAttackPosDir -- and every caller trusts that flag. That window
// crashed desktop OpenXR at map start (RIP=0 from JIT ZScript).
class AActor;
DVector3 P_FlatWeaponDir(AActor* actor, DAngle yaw, DAngle pitch);

class AActor final : public DThinker
{
	DECLARE_CLASS_WITH_META (AActor, DThinker, PClassActor)
	HAS_OBJECT_POINTERS
public:
	AActor() = default;
	AActor(const AActor &other) = delete;	// Calling this would be disastrous.
	AActor &operator= (const AActor &other) = delete;
	~AActor () = default;

	virtual void OnDestroy() override;
	virtual void Serialize(FSerializer &arc) override;
	virtual size_t PropagateMark() override;
	virtual void PostSerialize() override;
	virtual void PostBeginPlay() override;		// Called immediately before the actor's first tick
	virtual void Tick() override;
	void EnableNetworking(const bool enable) override;

	int GetModelTimer();

	void CalcBones(bool recalc);
	TRS GetBoneTRS(int model_index, int bone_index, bool with_override);

	//outmat must be double[16]
	void GetBoneMatrix(int model_index, int bone_index, bool with_override, double *outMat);

	DVector3 GetBoneEulerAngles(class FModel * mdl, int model_index, int bone_index, bool with_override);
	void GetBonePosition(class FModel * mdl, int model_index, int bone_index, bool with_override, DVector3 &pos, DVector3 &fwd, DVector3 &up);
	void GetObjectToWorldMatrix(double *outMat);

	static AActor *StaticSpawn (FLevelLocals *Level, PClassActor *type, const DVector3 &pos, replace_t allowreplacement, bool SpawningMapThing = false);

	inline AActor *GetDefault () const
	{
		return (AActor *)(this->GetClass()->Defaults);
	}

	FActorInfo *GetInfo() const
	{
		return static_cast<PClassActor*>(GetClass())->ActorInfo();
	}


	FDropItem *GetDropItems() const;

	// Adjusts the angle for deflection/reflection of incoming missiles
	// Returns true if the missile should be allowed to explode anyway
	bool AdjustReflectionAngle (AActor *thing, DAngle &angle);
	int AbsorbDamage(int damage, FName dmgtype, AActor *inflictor, AActor *source, int flags, DAngle angle);
	void AlterWeaponSprite(visstyle_t *vis);

	bool CheckNoDelay();

	void BeginPlay();			// Called immediately after the actor is created
	void CallBeginPlay();

	// [ZZ] custom postbeginplay (calls E_WorldThingSpawned)
	void CallPostBeginPlay() override;

	void LevelSpawned();				// Called after BeginPlay if this actor was spawned by the world
	void HandleSpawnFlags();	// Translates SpawnFlags into in-game flags.

	void Activate (AActor *activator);
	void CallActivate(AActor *activator);

	void Deactivate(AActor *activator);
	void CallDeactivate(AActor *activator);

	// Called when actor dies
	void Die (AActor *source, AActor *inflictor, int dmgflags = 0, FName MeansOfDeath = NAME_None);
	void CallDie(AActor *source, AActor *inflictor, int dmgflags = 0, FName MeansOfDeath = NAME_None);

	// Perform some special damage action. Returns the amount of damage to do.
	// Returning -1 signals the damage routine to exit immediately
	int DoSpecialDamage (AActor *target, int damage, FName damagetype);
	int CallDoSpecialDamage(AActor *target, int damage, FName damagetype, int flags, DAngle angle);

	// Like DoSpecialDamage, but called on the actor receiving the damage.
	int TakeSpecialDamage (AActor *inflictor, AActor *source, int damage, FName damagetype);
	int CallTakeSpecialDamage(AActor *inflictor, AActor *source, int damage, FName damagetype, int flags, DAngle angle);

	// Actor had MF_SKULLFLY set and rammed into something
	// Returns false to stop moving and true to keep moving
	bool Slam(AActor *victim);
	bool CallSlam(AActor *victim);

	// Something just touched this actor.
	void CallTouch(AActor *toucher);

	// Apply gravity and/or make actor sink in water.
	void FallAndSink(double grav, double oldfloorz);
	void CallFallAndSink(double grav, double oldfloorz);

	// Centaurs and ettins squeal when electrocuted, poisoned, or "holy"-ed
	// Made a metadata property so no longer virtual
	void Howl ();

	// plays bouncing sound
	void PlayBounceSound(bool onfloor, double volume);

	// plays pushing sound
	void PlayPushSound();

	// Called when an actor with MF_MISSILE and MF2_FLOORBOUNCE hits the floor
	bool FloorBounceMissile (secplane_t &plane, bool is3DFloor);

	// Called by RoughBlockCheck
	bool IsOkayToAttack (AActor *target);

	// Plays the actor's ActiveSound if its voice isn't already making noise.
	void PlayActiveSound ();

	void RestoreSpecialPosition();

	// Called by PIT_CheckThing() and needed for some Hexen things.
	// Returns -1 for normal behavior, 0 to return false, and 1 to return true.
	// I'm not sure I like it this way, but it will do for now.
	// (virtual on the script side only)
	int SpecialMissileHit (AActor *victim);

	// Called when bouncing to allow for custom behavior.
	// Returns -1 for normal behavior, 0 to stop, and 1 to keep going.
	// (virtual on the script side only)
	int SpecialBounceHit(AActor* bounceMobj, line_t* bounceLine, secplane_t* bouncePlane, bool is3DFloor);

	// Returns true if it's okay to switch target to "other" after being attacked by it.
	bool CallOkayToSwitchTarget(AActor *other);
	bool OkayToSwitchTarget (AActor *other);

	// Uses an item and removes it from the inventory.
	bool UseInventory (AActor *item);

	// Tosses an item out of the inventory.
	AActor *DropInventory (AActor *item, int amt = -1);

	// Returns true if this view is considered "local" for the player.
	bool CheckLocalView() const;
	// Allows for enabling/disabling client-side rendering in a way the playsim can't access.
	void DisableLocalRendering(const unsigned int pNum, const bool disable);
	bool ShouldRenderLocally() const;

	// Finds the first item of a particular type.
	AActor *FindInventory (PClassActor *type, bool subclass=false);
	AActor *FindInventory (FName type, bool subclass = false);
	template<class T> T *FindInventory ()
	{
		return static_cast<T *> (FindInventory (RUNTIME_CLASS(T)));
	}

	// Adds one item of a particular type. Returns NULL if it could not be added.
	AActor *GiveInventoryType (PClassActor *type);

	// Set the alphacolor field properly
	void SetShade (uint32_t rgb);
	void SetShade (int r, int g, int b);

	// Plays a conversation animation
	void ConversationAnimation (int animnum);

	// Make this actor hate the same things as another actor
	void CopyFriendliness (AActor *other, bool changeTarget, bool resetHealth=true);

	// Moves the other actor's inventory to this one
	void ObtainInventory (AActor *other);

	// Die. Now.
	bool Massacre ();

	// Transforms the actor into a finely-ground paste
	bool CallGrind(bool items);

	// Get this actor's team
	int GetTeam();

	// Is the other actor on my team?
	bool IsTeammate (AActor *other);

	// Is the other actor my friend?
	bool IsFriend (AActor *other);

	// Do I hate the other actor?
	bool IsHostile (AActor *other);

	inline bool IsNoClip2() const;
	void CheckPortalTransition(bool islinked);
	DVector3 GetPortalTransition(double byoffset, sector_t **pSec = NULL);

	// What species am I?
	FName GetSpecies();

	// set translation
	void SetTranslation(FName trname);

	double GetBobOffset(double ticfrac = 0) const;

	// Enter the crash state
	void Crash();

	// Return starting health adjusted by skill level
	double AttackOffset(double offset = 0);
	int SpawnHealth() const;
	int GetMaxHealth(bool withupgrades = false) const;
	int GetGibHealth() const;
	double GetCameraHeight() const;

	inline bool isMissile(bool precise=true)
	{
		return (flags&MF_MISSILE) || (precise && GetDefault()->flags&MF_MISSILE);
	}

	// Check for monsters that count as kill but excludes all friendlies.
	bool CountsAsKill() const
	{
		return (flags & MF_COUNTKILL) && !(flags & MF_FRIENDLY);
	}

	// These also set CF_INTERPVIEW for players.
	DAngle ClampPitch(DAngle p);
	void SetPitch(DAngle p, int fflags);
	void SetAngle(DAngle ang, int fflags);
	void SetRoll(DAngle roll, int fflags);

	// These also set CF_INTERPVIEWANGLES for players.
	void SetViewPitch(DAngle p, int fflags);
	void SetViewAngle(DAngle ang, int fflags);
	void SetViewRoll(DAngle roll, int fflags);

	double GetFOV(double ticFrac);

	PClassActor *GetBloodType(int type = 0) const;

	double Distance2DSquared(AActor *other, bool absolute = false)
	{
		DVector3 otherpos = absolute ? other->Pos() : other->PosRelative(this);
		return (Pos().XY() - otherpos.XY()).LengthSquared();
	}

	double Distance2D(AActor *other, bool absolute = false) const
	{
		DVector3 otherpos = absolute ? other->Pos() : other->PosRelative(this);
		return (Pos().XY() - otherpos.XY()).Length();
	}

	double Distance2D(double x, double y) const
	{
		return DVector2(X() - x, Y() - y).Length();
	}

	double Distance2D(AActor *other, double xadd, double yadd, bool absolute = false) const
	{
		DVector3 otherpos = absolute ? other->Pos() : other->PosRelative(this);
		return DVector2(X() - otherpos.X + xadd, Y() - otherpos.Y + yadd).Length();
	}


	// a full 3D version of the above
	double Distance3DSquared(AActor *other, bool absolute = false)
	{
		DVector3 otherpos = absolute ? other->Pos() : other->PosRelative(this);
		return (Pos() - otherpos).LengthSquared();
	}

	double Distance3D(AActor *other, bool absolute = false)
	{
		DVector3 otherpos = absolute ? other->Pos() : other->PosRelative(this);
		return (Pos() - otherpos).Length();
	}

	DAngle AngleTo(AActor *other, bool absolute = false)
	{
		DVector3 otherpos = absolute ? other->Pos() : other->PosRelative(this);
		return VecToAngle(otherpos.XY() - Pos().XY());
	}

	DAngle AngleTo(AActor *other, double oxofs, double oyofs, bool absolute = false) const
	{
		DVector3 otherpos = absolute ? other->Pos() : other->PosRelative(this);
		return VecToAngle(otherpos.XY() - Pos().XY() + DVector2(oxofs, oyofs));
	}

	DVector2 Vec2To(AActor *other) const
	{
		return other->PosRelative(this).XY() - Pos().XY();
	}

	DVector3 Vec3To(AActor *other) const
	{
		return other->PosRelative(this) - Pos();
	}

	DVector2 Vec2Offset(double dx, double dy, bool absolute = false);
	DVector3 Vec2OffsetZ(double dx, double dy, double atz, bool absolute = false);
	DVector2 Vec2Angle(double length, DAngle angle, bool absolute = false);
	DVector3 Vec3Offset(double dx, double dy, double dz, bool absolute = false);
	DVector3 Vec3Offset(const DVector3 &ofs, bool absolute = false);
	DVector3 Vec3Angle(double length, DAngle angle, double dz, bool absolute = false);

	void ClearInterpolation();
	void ClearFOVInterpolation();

	void Move(const DVector3 &vel)
	{
		SetOrigin(Pos() + vel, true);
	}
	void SetOrigin(double x, double y, double z, bool moving);
	void SetOrigin(const DVector3 & npos, bool moving)
	{
		SetOrigin(npos.X, npos.Y, npos.Z, moving);
	}

	inline void SetFriendPlayer(player_t *player);

	bool IsVisibleToPlayer() const;
	bool IsInsideVisibleAngles() const;

	// Calculate amount of missile damage
	int GetMissileDamage(int mask, int add);

	bool CanSeek(AActor *target) const;

	double GetGravity() const;
	bool IsSentient() const;
	const char *GetTag(const char *def = NULL) const;
	void SetTag(const char *def);
	const char *GetCharacterName() const;

	// Triggers SECSPAC_Exit/SECSPAC_Enter and related events if oldsec != current sector
	void CheckSectorTransition(sector_t *oldsec);
	void UpdateRenderSectorList();
	void ClearRenderSectorList();
	void ClearRenderLineList();

	void AttachLight(unsigned int count, const FLightDefaults *lightdef);
	void SetDynamicLights();

// info for drawing
	AActor			*snext, **sprev;	// links in sector (if needed)
	DVector3		__Pos;		// double underscores so that it won't get used by accident. Access to this should be exclusively through the designated access functions.

	DAngle			SpriteAngle;
	DAngle			SpriteRotation;
	DAngle			AngledRollOffset;	// Offset for angle-dependent sprite rolling (see RF2_ANGLEDROLL)
	DVector2		AutomapOffsets;		// Offset the actors' sprite view on the automap by these coordinates.
	float			isoscaleY;				// Y-scale to compensate for Y-billboarding for isometric sprites
	float			isotheta;				// Rotation angle to compensate for Y-billboarding for isometric sprites
	DRotator		Angles;
	DRotator		ViewAngles;			// Angle offsets for cameras
	TObjPtr<DViewPosition*> ViewPos;			// Position offsets for cameras
	DVector2		Scale;				// Scaling values; 1 is normal size
	double			Alpha;				// Since P_CheckSight makes an alpha check this can't be a float. It has to be a double.

	int				sprite;				// used to find patch_t and flip value
	uint8_t			frame;				// sprite frame to draw
	uint8_t			effects;			// [RH] see p_effect.h
	uint8_t			fountaincolor;		// Split out of 'effect' to have easier access.
	FRenderStyle	RenderStyle;		// Style to draw this actor with
	FTextureID		picnum;				// Draw this instead of sprite if valid
	uint32_t			fillcolor;			// Color to draw when STYLE_Shaded
	FTranslationID			Translation;
	FTextureID		LastPatch;
	int				lastScaleFlags;
	int				lastModelSprite;
	uint8_t			lastModelFrame;
	// RS FORK -- r_voxeldistance / r_voxeldistance_band: which side of the voxel
	// distance cull this actor was on last frame (0 voxel, 1 sprite), so the
	// swap has hysteresis instead of flickering on the boundary. A render cache
	// like the two above: written by HWSprite::Process on the main pass only
	// (never a portal or sprite-shadow pass), not serialized, not exposed to
	// script. A one-byte race between BSP threads costs one frame's choice.
	uint8_t			VoxelFarLatch;

	uint32_t			RenderRequired;		// current renderer must have this feature set
	uint32_t			RenderHidden;		// current renderer must *not* have any of these features

	bool				NoLocalRender;		// DO NOT EXPORT THIS! This is a way to disable rendering such that the playsim cannot access it.
	ActorRenderFlags	renderflags;		// Different rendering flags
	ActorRenderFlags2	renderflags2;		// More rendering flags...
	ActorFlags		flags;
	ActorFlags2		flags2;			// Heretic flags
	ActorFlags3		flags3;			// [RH] Hexen/Heretic actor-dependant behavior made flaggable
	ActorFlags4		flags4;			// [RH] Even more flags!
	ActorFlags5		flags5;			// OMG! We need another one.
	ActorFlags6		flags6;			// Shit! Where did all the flags go?
	ActorFlags7		flags7;			// WHO WANTS TO BET ON 8!?
	ActorFlags8		flags8;			// I see your 8, and raise you a bet for 9.
	ActorFlags9		flags9;			// Happy ninth actor flag field GZDoom !
	double			Floorclip;		// value to use for floor clipping
	double			radius, Height;		// for movement checking

	FAngle			VisibleStartAngle;
	FAngle			VisibleStartPitch;
	FAngle			VisibleEndAngle;
	FAngle			VisibleEndPitch;

	DVector3		OldRenderPos;
	DVector3		Vel;
	DVector2		SpriteOffset;
	DVector3		WorldOffset;
	double			Speed;
	double			FloatSpeed;
	TObjPtr<DActorModelData*>		modelData;

	// [BB] Draw this actor as its voxel, if it has one, regardless of
	// r_drawvoxels and in preference to any model. See FindModelFrame in
	// r_data/models.cpp for why both of those are deliberate.
	//
	// RS FORK -- r_voxels_mode: in mode 1 (auto with a voxel pack) this flag is
	// the ONLY way an actor is drawn as a voxel; mode 2 refuses it too, except
	// for a frame that has no sprite. VoxelFarLatch above is its cull companion.
	//
	// Exists so a single object can become a real 3D thing for as long as
	// something is true of it -- being held in a hand, most obviously, since a
	// billboard cannot be turned over and a voxel can. Voxel selection is
	// otherwise keyed on the sprite frame and gated by one global cvar, so
	// there was no way to ask for it per-actor at all.
	bool			VoxelOverride;

	// RS FORK -- HONOUR THIS ACTOR'S PITCH AND ROLL WHATEVER ITS MODEL SAYS.
	//
	// MDL_USEACTORPITCH and MDL_USEACTORROLL are opt-in per MODELDEF, and yaw
	// is not -- so an actor that BORROWS another class's model definition can
	// be turned in yaw and cannot be pitched or rolled at all. Its pitch and
	// roll are simply discarded, silently, with nothing anywhere to say why.
	//
	// That is exactly what a holstered weapon is: a prop wearing the real
	// weapon's model via A_ChangeModel. Setting the flags on the borrowed
	// definition is not an option -- it is shared with the actual weapon, so
	// tuning how a pistol sits in a holster would change how the pistol is
	// drawn in your hand.
	//
	// Per-actor, therefore, and read as a local OR over the flags at draw time
	// so nothing leaks to anything else drawn from the same definition.
	bool			ForceModelAngles;

	// RS FORK -- WHICH BUTTONS ARE DOWN ON A HAND WHOSE POSE HAS CLAIMED THEM.
	//
	// Bit 0 grip, bit 1 face pad, bit 2 trigger, for whichever hand is armed;
	// main and off are kept in the low and high nibbles so one field carries
	// both.
	//
	// Exists because a pose that binds actions to buttons has to take those
	// buttons AWAY from what they normally do -- pull the trigger to fire a wrist
	// mount and you must not also fire the gun in that hand. The engine is the
	// only place that can suppress the normal emission, but the DECISION about
	// what a mount does belongs in script. So the engine suppresses and publishes
	// here, and script reads this instead of cmd.buttons.
	//
	// The alternative was emitting invented key codes for script to bind, which
	// would have meant hunting six made-up keys in Customize Controls from inside
	// a headset before anything worked at all.
	int				HardpointButtons;

	// RS FORK -- WORN ON THE BODY, PLACED AT DRAW RATE.
	//
	// FollowBodyMode 1 draws this actor in the player's body frame -- head
	// position and yaw, read fresh every frame through VRMode::GetHmdTransform --
	// with FollowBodyOfs as its seat in that frame. 0, the default, leaves the
	// actor placed the ordinary way and is what everything that never asks for
	// this gets.
	//
	// WHY PER-ACTOR AND NOT A MODELDEF FLAG. MDL_FOLLOWMAINHAND works as a flag
	// because there is one main hand and everything riding it wants the same
	// transform. A worn rig is the opposite: a dozen props share one class and
	// each sits at its own place on the body, chosen by the player. A per-class
	// MODELDEF prefix cannot say "this one is on my left hip and that one is
	// behind my shoulder", so the seat travels with the instance.
	//
	// WHAT IT REPLACES. Script could only sample the head pose once a tic and
	// re-place each prop with SetOrigin, so between two samples the whole rig
	// swam -- and a prop far from the anchor swept a wide arc for a small head
	// movement, which is why the low ones looked worst. Interpolation cannot
	// help: it smooths between two stale samples of a pose that moved at 90Hz.
	//
	// Offsets are in the body's own frame: X forward, Y right, Z up, in map
	// units, matching _ofs_x/_ofs_y/_ofs_z everywhere else in this fork.
	// Mode 1: the body frame's heading is the renderer's own (doomYaw).
	// Mode 2: the heading is FollowBodyYaw below, supplied by the caller.
	//
	// Mode 2 exists because doomYaw IS NOT REACHABLE FROM SCRIPT and is not the
	// same number as AActor::HmdYaw -- doomYaw accumulates the per-frame head-turn
	// delta on top of it. A caller that filters its own heading (hips that do not
	// follow every glance) cannot express a seat in the renderer's basis, because
	// it cannot see that basis; projecting onto HmdYaw instead leaves everything
	// out by exactly the head turn, so a filtered body still tracks the head and
	// the filter appears to do nothing.
	//
	// So the caller owns the heading rather than trying to match it.
	int				FollowBodyMode;
	double			FollowBodyYaw;
	DVector3		FollowBodyOfs;

	// RS FORK -- FollowBodyYaw AT DRAW RATE, OPT IN.
	//
	// A caller writes FollowBodyYaw once a tic, so a body-frame part -- torso,
	// holsters, boots, arms -- turns in 35 Hz steps while the hands riding the
	// controllers move every frame: a tick at the elbow in every turn. With this
	// set the renderer draws last tic's heading turned toward this tic's by the
	// shortest way, ticFrac of the way (DrawFollowBodyYaw), as it draws Angles.
	//
	// false, the default, is exactly the old behaviour: FollowBodyYaw drawn as
	// written. NOT SAVED -- it is presentation, and a rig sets it every tic with
	// the heading. A snap turn stays a snap when the caller calls
	// ClearInterpolation() after writing the new heading, or clears this for
	// that tic.
	bool			FollowBodyYawInterp;
	// Last tic's FollowBodyYaw, and whether FollowBodyYawInterp was on when it was
	// taken. Snapshotted in ClearInterpolation (actorinlines.h), which P_Ticker
	// calls for every actor before any script writes this tic's heading -- the
	// same instant Prev is taken, so heading and position are one snapshot and
	// whatever resets one resets the other. The flag's own snapshot is what keeps
	// the tic an actor spawns, loads or first opts in from swinging in from a
	// stale heading. Not saved, not readable from script.
	bool			PrevFollowBodyYawLive;
	double			PrevFollowBodyYaw;

	// RS FORK -- HELD IN A HAND, PLACED AT DRAW RATE. The hand-frame twin of
	// FollowBodyMode/FollowBodyOfs above, and deliberately the same shape.
	//
	// FollowHandMode overrides WHICH controller a model rides:
	//     0  the MODELDEF decides (MDL_FOLLOWMAINHAND / MDL_FOLLOWOFFHAND).
	//        The default, and what every existing caller keeps getting.
	//     1  the main hand, whatever the MODELDEF says.
	//     2  the off hand, whatever the MODELDEF says.
	//
	// WHY AN OVERRIDE AND NOT JUST AN OFFSET. A MODELDEF flag is per CLASS, and
	// which hand a thing is in is a property of the MOMENT. The case that forced
	// this: an off hand reaching for a pistol's slide. The slide is part of the
	// gun, the gun rides the MAIN controller, so for the off hand to be drawn
	// touching the slide it has to be placed in the MAIN hand's frame -- while
	// the player's real off hand is somewhere more comfortable. Expressed the
	// other way round, script would have to recompute the gap between two
	// controllers every tic and rebuild the off hand's basis to express it,
	// which is the hand-rolled-basis trap that has cost this tree twice.
	//
	// This is not a favour to one gun. "Draw this thing as though it were in
	// that hand" is the same primitive a magazine carried to a weapon needs, and
	// a holstered gun being drawn, and a two-handed grip's support hand.
	//
	// FollowHandOfs is where in that hand's frame it sits. It is added to the
	// model's own placement offsets -- the same numbers MODELDEF Offset and the
	// _ofs_x/_ofs_y/_ofs_z placement cvars drive -- so it means exactly what the
	// sliders mean and there is no second convention to learn. Zero, the
	// default, changes nothing.
	//
	// Kept separate from FollowBodyOfs rather than reusing it: the two frames
	// are different, an actor may legitimately want one and not the other, and
	// collapsing them is the same mistake as collapsing followedBody and
	// followedHand -- see the note in models.cpp.
	//
	// THE ONE TRAP, AND IT IS WORTH READING BEFORE USING THIS ON A NEW MODEL.
	// The two hand frames are not necessarily the same handedness.
	// VRMode::GetWeaponTransform (hw_vrmodes.cpp) applies scale(-1, 1, 1) to the
	// OFF hand's frame and not the main one, so on a model that allows auto
	// reverse, moving it between the frames MIRRORS IT and flips the sign of
	// FollowHandOfs.X with it.
	//
	// A model carrying MODELDEF's NOAUTOREVERSE is exempt -- the mirror is
	// skipped in both frames, the handedness matches, and an X offset keeps its
	// meaning across a mode change. The RS world hands are all NOAUTOREVERSE
	// (they are purpose-built left and right meshes and do their own mirroring
	// with a negative Scale), which is why the first user of this field did not
	// have to think about it. The second one might.
	int				FollowHandMode;
	DVector3		FollowHandOfs;

	// RS FORK -- A TURN OF THE HAND'S FRAME, OWNED BY SCRIPT.
	//
	// FollowHandOfs moves a hand-held model within the hand's frame; this turns
	// that frame, about the hand itself, before the model's seat is applied. So
	// the whole seat swings with it -- MODELDEF Offset, FollowHandOfs, the live
	// placement sliders, the base orientation -- the way a wrist turns what it
	// holds: a gun's recoil climb lifts the muzzle about the grip rather than
	// spinning the gun about its own origin, and anything riding the model
	// (FollowActor) turns with it. Built for recoil; the same primitive is a
	// holster draw flick, a caught gun settling, a pump tossed back to the grip.
	//
	// Degrees, about the HAND'S axes, with the engine's own angle signs:
	//   X  yaw    + turns the model left, as adding to Angle does.
	//   Y  pitch  + tips the muzzle down, as Pitch does.
	//   Z  roll   + tips the model's top to the right, seen from behind.
	// Zero, the default, changes nothing, and it does nothing unless the model is
	// being drawn in a hand's frame (FollowHandMode or the MODELDEF hand flags, in
	// VR, not riding the body or another model).
	//
	// IT READS THE SAME IN BOTH HANDS -- the one place it differs from
	// FollowHandOfs. The off-hand frame may be mirrored (the trap above), and a
	// turn inside a mirror comes out reflected, so the renderer undoes the mirror
	// for this turn (models.cpp) and the same numbers point the model the same
	// way in either hand. A turn that has to match where the rounds went needs
	// exactly that.
	//
	// Drawn interpolated from last tic's value (PrevFollowHandRot, captured in
	// ClearInterpolation), so a kick easing home over a few tics does not step at
	// 35 Hz on a 90 Hz headset. Script-owned animation: no slider writes it; the
	// player's seat rotation is the placement prefix below.
	DVector3		FollowHandRot;
	DVector3		PrevFollowHandRot;

	// AND WHERE ITS TUNING NUMBERS COME FROM.
	//
	// THE SLIDERS HAVE TO MOVE THE MODEL WHILE THE MENU IS OPEN. That is the
	// hard requirement, it has cost this project more time than any other single
	// thing, and it rules out every design where script holds the number: the
	// playsim does not tick while a menu is up, so a script-read value only
	// lands once the menu closes -- which is indistinguishable from a slider
	// that does nothing. See EngineDocs5.0.x/PLACEMENT.md.
	//
	// Only ONE channel in this engine satisfies it: MODELDEF's PlacementCVars,
	// read by the RENDERER out of <prefix>_ofs_x/_y/_z, _yaw/_pitch/_roll and
	// _scale on every frame it draws. So the fix is not another field for script
	// to write -- it is letting an actor say WHICH PREFIX the renderer should
	// read for it right now.
	//
	// Empty (the default) means use the MODELDEF's own, so nothing that never
	// sets this changes at all. Set it and that actor's placement is tunable
	// live, on its own sliders, in its own mod's menu -- the same mechanism the
	// world hands and the grab ovals already use, which is the one everybody
	// knows works.
	//
	// The case it was built for: an off hand pinned to a pistol's slide adopts
	// 'rs_tp_slide', so the six numbers that say where on the slide the hand
	// sits and how the wrist is turned are ordinary sliders on the pistol's own
	// menu page. A hand pinned to a magazine adopts 'rs_tp_mag' and gets a
	// second, independent set. Neither needs an engine change to add.
	//
	// TWO CHANNELS, ONE WRITER EACH, and keeping them apart is the whole design.
	// This prefix is the TUNING -- owned by the player, never written by script.
	// FollowHandOfs above is the ANIMATION -- the travel of a slide being
	// dragged, owned by script, never touched by a slider. They are added
	// together by the renderer. Putting both in one number is what "two writers
	// on one transform" means, and it reads as the slider being dead.
	FName			PlacementPrefix = NAME_None;

	// RS FORK -- DRAWN INSIDE ANOTHER ACTOR'S MODEL, AT DRAW RATE.
	//
	// FollowBodyMode puts a model in the player's body frame, FollowHandMode in
	// a controller's. This puts it in ANOTHER MODEL'S frame, as that model is
	// drawn this frame: its seat, its own body or hand follow, and its live
	// placement sliders included. A stored gun rides its holster through
	// whatever moves the holster -- including a holster page's sliders with the
	// menu open, which no script-side placement can follow, because script does
	// not run behind a menu. The same primitive serves a scope on a rifle, or a
	// hand riding the slide another hand is dragging.
	//
	// THE FRAME (models.cpp, ModelFollowFrame) is the parent model's drawn
	// origin, turned by the parent's own rotation and its placement rotation.
	// NOT the parent's scale -- a big holster must not stretch the gun in it --
	// and NOT its MODELDEF base orientation, so swapping the parent's mesh does
	// not turn the child. It keeps the parent's PATH units: a parent riding a
	// controller is in the hand frame, so a child of it is too.
	//
	// FollowActorOfs is the child's seat in that frame, Doom-local map units
	// (X forward, Y left, Z up) -- the axes GetModelWorldOffset answers in, so a
	// centring correction from it goes straight in. The child's own Angles
	// apply on top, RELATIVE to the frame.
	//
	// FollowActorSlot -1 (the default) follows the whole model. 0..15 follows
	// that surface-override slot of the parent's model data as it is drawn this
	// frame -- a script-set part transform or a live hand drive -- so a child
	// rides a moving part. World actors only: a psprite's slots have no actor.
	//
	// Outranks FollowBodyMode and FollowHandMode while the parent has a model to
	// follow; with none this frame it falls through to them. INERT UNTIL SET:
	// null draws exactly as before. The child's own position still decides
	// whether it is drawn at all, so keep it near the parent.
	TObjPtr<AActor*>	FollowActor;		// GC-registered in p_mobj.cpp (IMPLEMENT_POINTER)
	int				FollowActorSlot = -1;
	DVector3		FollowActorOfs;

	// RS FORK -- OR A JOINT OF THE PARENT'S MODEL, AS DRAWN (Engine docs/MODEL_JOINT_DRIVE_PLAN.md piece E).
	//
	// FollowActorSlot rides a mesh SURFACE's motion; a rigged model (an IQM) keeps its parts on JOINTS. Name one here and
	// the child rides that joint of model index FollowActorJointModel exactly as the parent's draw skinned it -- its
	// animation, the joint draw poses, joint offsets and hand drives, reach chains and aims all in: a hand seat staying on
	// the slide the other hand drags, a sight on tag_rail_attach, a wrist display on an animated gauntlet. While set it
	// takes FollowActorSlot's place.
	//
	// THE MOTION is the joint's own, as a rigid part transform in the parent's model space, and the identity at the bind
	// pose, so a child seated against the whole model sits the same against the joint. A FollowActorOfsInModel point is a
	// point on the BIND-POSE mesh (the IQM file's vertex, (x, z, y)) and lands where a vertex skinned wholly to the joint
	// is drawn. The joint's origin rides exactly and its rotation turns the child; its scale or a mirror never reaches
	// the child, as the parent's never does (models.cpp, model_jointfollow.h).
	//
	// READ FROM WHAT WAS DRAWN, NEVER SOLVED FOR THE CHILD (the Body IK lane's condition 8): the parent's world-model draw
	// publishes its finished palette and the child's draw reads it (model_reach.cpp). A child drawn after its parent rides
	// this frame's; one drawn before rides the last frame's, one frame late. The choice is made once per frame for every
	// child of that parent, so both eyes see the same one. Read only inside RenderModel: ModelPointToWorld,
	// ModelFollowFrameToWorld, GetBonePosition and every other script query see the whole model's frame (FollowActorSlot
	// is not consulted either), so nothing a playsim decision reads carries this machine's hands (netplay).
	//
	// FALLS BACK TO THE WHOLE MODEL, and says why once in the log ([FOLLOWJOINT]): no such joint, the parent not drawn with
	// bones at that model index yet, or the joint drawn collapsed (hidden). On a +DECOUPLEDANIMATIONS or
	// MODELSAREATTACHMENTS parent every model is skinned with the first model's bones, so the joint is taken from that one
	// palette whatever FollowActorJointModel says. INERT UNTIL SET: NAME_None reads exactly as before, and while no child
	// anywhere names a joint, nothing is published.
	FName			FollowActorJoint = NAME_None;
	int				FollowActorJointModel = 0;

	// THE SEAT AS A POINT ON THE PARENT'S MESH.
	//
	// FollowActorOfs is normally a seat in the follow frame, and that frame
	// deliberately leaves out the parent's scale, any mirror a negative MODELDEF
	// Scale puts in, and its MODELDEF base orientation. A point read off the
	// parent's mesh -- a weapon card's grab point, a bone, a marker -- lives in
	// the space the mesh's vertices do, which has all three, so handed over as a
	// frame seat it lands the wrong size, turned, and on the wrong side. Found on
	// a rifle drawn at Scale -0.82.
	//
	// With this set, FollowActorOfs IS that point: a position in the parent's
	// MODEL space, in the order ModelPointToWorld takes -- the renderer's, y up,
	// every MD3 vertex stored (x, z, y) -- and the renderer carries it into the
	// follow frame itself (models.cpp, FollowSeatFromModelPoint):
	// seat = frame^-1 * parentMat * P. The child is then drawn at parentMat * P;
	// with FollowActorSlot, at parentMat * part * P, so it rides the moving part.
	//
	// POSITION ONLY. The child still turns with the follow frame, so its Angles
	// and any wrist sliders mean what they meant. FollowActorOfsCVar and
	// FollowActorOfsCVar2 still add on top, in the frame's axes and units.
	//
	// INERT UNTIL SET: false reads FollowActorOfs exactly as before.
	bool			FollowActorOfsInModel = false;

	// A SEAT A HUMAN CAN TUNE WHILE THE MENU IS OPEN.
	//
	// Names a placement set: the RENDERER adds <prefix>_ofs_x/_ofs_y/_ofs_z to
	// FollowActorOfs every frame it draws, in the follow frame's own axes and
	// units, so a slider moves the child WHILE THE PLAYSIM IS FROZEN -- which is
	// exactly when a person is looking at the slider. Script cannot do this: it
	// does not run behind a menu, so anything it seats stands still until the
	// menu closes.
	//
	// NOT DIVIDED BY THE CHILD'S SCALE, unlike a placement set on the child's own
	// model (step 4 in ObjectToWorldMatrix). The seat lives in the parent's
	// frame, so it must not change when the child is resized -- one cvar set can
	// then serve many children of different sizes, which is the whole point for
	// markers and gauges.
	//
	// The same suffixes a MODELDEF PlacementCVars set uses, so one name reads the
	// same wherever it appears. INERT UNTIL SET: NAME_None adds nothing.
	FName			FollowActorOfsCVar;

	// A SECOND SET, SUMMED WITH THE FIRST. One seat, two sliders that both move
	// it and cannot share a name: a correction applied to a whole family of
	// children (every grab point on one gun) and a nudge for this one alone. Both
	// have to answer live, so neither can be the one script folds in.
	FName			FollowActorOfsCVar2;

	// RS FORK -- TRACE THIS ACTOR IN NEON, FROM ITS OWN SPRITE.
	//
	// The drawing is func_spriteoutline.fp: a Sobel edge detect over the
	// sprite the actor is already showing, so the outline is exactly the right
	// size and shape for ANY actor, including one a mod added this morning.
	// Nothing here names a monster and nothing measures a body.
	//
	// This lives per actor rather than per scene because the previous fork put
	// it per scene -- one global cvar, bound in gldefs to fifteen hardcoded
	// Doom sprite names -- and that could not light one corpse, could not fade,
	// and knew nothing about anybody else's monsters.
	//
	// INERT UNTIL SET. OutlineMode 0 is off, which is the default, and off
	// costs one float compare in the fragment shader. Read in
	// HWSprite::DrawSprite.
	PalEntry		OutlineColorA;
	PalEntry		OutlineColorB;
	double			OutlineStrength;	// master; 0 is off, and a fade passes through it
	double			OutlineThickness;	// how wide the traced line is, in texels
	double			OutlineThreshold;	// how much contrast counts as an edge
	double			OutlineGlow;		// how far the halo reaches off the line
	double			OutlinePulse;		// A-to-B crossfade speed; 0 holds on A
	int				OutlineMode;		// 0 off, 1 edge, 2 wire, 3 ghost

	// [SCENEMASK] WHAT THIS PIXEL CAME FROM ("Engine docs/SCENE_MASK_PLAN.md").
	//
	// One unsigned byte the scene pass stamps into a post-process-readable attachment for
	// every pixel this actor draws. A post-process shader that declares the scene mask then
	// knows which pixels came from this actor and can treat them differently: keep blood red
	// while the world greys out, outline the wheel's current selection, show only the warm
	// things, keep a marker legible through a grade.
	//
	// THE ENGINE NEVER LEARNS WHAT A NUMBER MEANS. 0 is "nothing special" and is the default,
	// so every actor in every existing mod is untouched and costs nothing. 1..255 belong to
	// the content: two mods may use different numbers and a third may read both. Nothing in
	// the engine ever writes a non-zero value here and nothing branches on it outside the
	// renderer, so this is presentation only and cannot move the playsim.
	//
	// Read in HWSprite::DrawSprite, which covers this actor's sprite AND its model.
	uint8_t			PostMask;

	// RS FORK -- DRAWN ONLY WHILE A SETTING SAYS SO.
	//
	// Names a cvar; the renderer draws this actor only while that cvar is above
	// zero, reading it every frame it draws. That is the whole point: while a
	// menu is open the playsim is frozen and script cannot switch anything on
	// or off, but the renderer is still drawing -- so a tuning page can light
	// up the very thing it is editing (a holster, a hand, a stored gun) by
	// setting one cvar from its UI code. Pair it with PlacementPrefix for a
	// thing that must also MOVE live.
	//
	// INERT UNTIL SET: NAME_None, the default, draws as always and costs one
	// compare. Read in HWSprite::Process.
	FName			VisibleCVar;

	// RS FORK -- DRAWN AT THE FADE A SETTING SAYS, not the one script last set.
	//
	// Names a cvar; while it exists the renderer draws this actor at that alpha,
	// every frame it draws. Same reason as VisibleCVar above: behind a menu the
	// playsim is frozen, so an Alpha script assigns each tic stops changing at
	// the exact moment someone is dragging the fade slider for it. A marker, a
	// gauge or any drawn aid can hand its fade over and have the slider answer
	// at once.
	//
	// INERT UNTIL SET: NAME_None keeps the actor's own Alpha. A cvar that does
	// not exist is ignored rather than read as zero, so a typo cannot make a
	// thing vanish. Read in HWSprite::Process.
	FName			AlphaCVar;

	// RS FORK -- DRAWN AT THE SIZE A SETTING SAYS.
	//
	// The third of the same family (VisibleCVar, AlphaCVar): while the named cvar
	// is above zero the renderer draws this model at that size, every frame it
	// draws, so a size slider answers with the playsim frozen behind a menu.
	// At zero -- or with no such cvar -- the actor's own Scale is used, so a
	// slider set to "off" hands the size straight back to whatever set it.
	//
	// ScaleCVarUnit is what ONE of that cvar means, as a scale. A slider in map
	// units of radius drives a mesh whose own radius is not one and which may sit
	// in a frame whose units are not map units; the caller knows both and states
	// the conversion once, rather than every reader of the cvar guessing it.
	//
	// INERT UNTIL SET. Read in FSpriteModelFrame::ObjectToWorldMatrix.
	FName			ScaleCVar;
	double			ScaleCVarUnit = 1.0;

	// RS FORK -- A DRAWN THING THAT BREATHES, IN WALL-CLOCK TIME.
	//
	// PulseHz above zero fades this actor in and out that many times a second,
	// between full alpha and PulseDepth of it (0.4 = down to 40%). The renderer
	// does it from the WALL CLOCK, not the playsim clock, for the same reason the
	// cvar channels above exist: behind a menu the playsim is frozen, and a thing
	// that breathes to say "this is the one you are editing" has to keep
	// breathing exactly then. It is also why this is not script fading Alpha.
	//
	// A FADE, NOT A FLASH: the curve is a sine, never a step, and never reaches
	// zero. A hard flash is a photosensitivity problem; a breath is not.
	//
	// INERT UNTIL SET: PulseHz 0, the default, draws exactly as before. Read in
	// HWSprite::Process beside AlphaCVar.
	double			PulseHz = 0.0;
	double			PulseDepth = 0.5;

	// RS FORK -- A COLOUR THAT TINTS WHAT IS DRAWN, WHATEVER THE RENDER STYLE.
	//
	// fillcolor (SetShade) only reaches the renderer for the styles that REPLACE
	// the texture with a colour -- Stencil, Shaded -- so a textured model asked
	// to look green either stayed its own colour or became a flat green
	// silhouette. This multiplies the drawn colour instead: the texture stays,
	// tinted. A magazine coloured by how full it is was the first caller; any
	// model or sprite that wants to say something by its colour is the next.
	// Fullbright (+BRIGHT) plus a tint reads as glowing in that colour.
	//
	// INERT UNTIL SET: 0, the default, draws exactly as before -- black is not a
	// tint. Read in HWSprite::Process where ThingColor is chosen.
	uint32_t		TintColor = 0;

	// RS FORK -- A MODEL WITH THREE DIFFERENT SIZES.
	//
	// Actor::Scale has two numbers, width and height, because a sprite has two.
	// A MODEL has three, and a volume drawn to show a REACH needs all three: a
	// grab that is long along a barrel and narrow across it is an oval, and a
	// sphere drawn for it is a lie about what will be caught.
	//
	// Multiplies the model's own scale, per axis, in the model's own space
	// (x, y, z as the mesh is authored). A placement set's _scale_x/_y/_z does
	// the same thing from cvars and both apply; this is the per-ACTOR one, for
	// when every instance needs a different shape and no slider is involved.
	//
	// INERT UNTIL SET: zero or less on an axis means "leave that axis", so the
	// default (0,0,0) draws exactly as before.
	DVector3		ScaleAxes;

// interaction info
	FBlockNode		*BlockNode;			// links in blocks (if needed)
	struct sector_t	*Sector;
	subsector_t *		subsector;
	FSection *			section;
	double			floorz, ceilingz;	// closest together of contacted secs
	double			dropoffz;		// killough 11/98: the lowest floor over all contacted Sectors.

	uint32_t		ThruBits;
	uint32_t		lineBlockBits;	// [BLOCKBITS] blocked by lines whose blockBits share a bit (P_IsBlockedByLine). 0 = none
	FTextureID		floorpic;			// contacted sec floorpic
	int				floorterrain;
	FTextureID		ceilingpic;			// contacted sec ceilingpic

	struct sector_t	*floorsector;
	struct sector_t	*ceilingsector;
	double			renderradius;

	double			projectilepassheight;	// height for clipping projectile movement against this actor
	double			CameraHeight;	// Height of camera when used as such
	double			CameraFOV;

	double			RadiusDamageFactor;		// Radius damage factor
	double			SelfDamageFactor;
	double			StealthAlpha;	// Minmum alpha for MF_STEALTH.
	int				WoundHealth;		// Health needed to enter wound state

	int32_t			tics;				// state tic counter
	FState			*state;
	//VMFunction		*Damage;			// For missiles and monster railgun
	int				DamageVal;
	int				projectileKickback;
	VMFunction		*DamageFunc;

	// [BB] If 0, everybody can see the actor, if > 0, only members of team (VisibleToTeam-1) can see it.

	int				special1;		// Special info
	int				special2;		// Special info
	double			specialf1;		// With floats we cannot use the int versions for storing position or angle data without reverting to fixed point (which we do not want.)
	double			specialf2;

	uint32_t			VisibleToTeam;
	int				weaponspecial;	// Special info for weapons.
	int 			health;
	int32_t			reactiontime;	// if non 0, don't attack yet; used by
									// player to freeze a bit after teleporting
	int32_t			threshold;		// if > 0, the target will be chased
	int32_t			DefThreshold;	// [MC] Default threshold which the actor will reset its threshold to after switching targets

	uint8_t			movedir;		// 0-7
	int8_t			visdir;
	int16_t			movecount;		// when 0, select a new dir

	int16_t			strafecount;	// for MF3_AVOIDMELEE
	int16_t			LightLevel;		// Allows for overriding sector light levels.
	uint16_t			SpawnAngle;

	TObjPtr<AActor*> target;			// thing being chased/attacked (or NULL)
									// also the originator for missiles
	TObjPtr<AActor*>	lastenemy;		// Last known enemy -- killough 2/15/98
	TObjPtr<AActor*> LastHeard;		// [RH] Last actor this one heard
									// no matter what (even if shot)
	player_t		*player;		// only valid if type of PlayerPawn
	TObjPtr<AActor*>	LastLookActor;	// Actor last looked for (if TIDtoHate != 0)
	DVector3		SpawnPoint; 	// For nightmare respawn
	int				StartHealth;
	uint8_t			WeaveIndexXY;	// Separated from special2 because it's used by globally accessible functions.
	uint8_t			WeaveIndexZ;
	uint16_t		skillrespawncount;
	int				TIDtoHate;			// TID of things to hate (0 if none)
	FName		Species;		// For monster families
	TObjPtr<AActor*>	alternative;	// (Un)Morphed actors stored here. Those with the MF_UNMORPHED flag are the originals.
	TObjPtr<AActor*>	tracer;			// Thing being chased/attacked for tracers
	TObjPtr<AActor*>	master;			// Thing which spawned this one (prevents mutual attacks)
	TObjPtr<AActor*>	damagesource;	// [AA] Thing that fired a hitscan using this actor as a puff

	int				tid;			// thing identifier
	int				special;		// special
	int				args[5];		// special arguments

	int		accuracy, stamina;		// [RH] Strife stats -- [XA] moved here for DECORATE/ACS access.

	AActor			*inext, **iprev;// Links to other mobjs in same bucket
	TObjPtr<AActor*> goal;			// Monster's goal if not chasing anything
	int				waterlevel;		// 0=none, 1=feet, 2=waist, 3=eyes
	double			waterdepth;		// Stores how deep into water you are, in map units
	uint8_t			boomwaterlevel;	// splash information for non-swimmable water sectors
	uint8_t			MinMissileChance;// [RH] If a random # is > than this, then missile attack.
	int8_t			LastLookPlayerNumber;// Player number last looked for (if TIDtoHate == 0)
	ActorBounceFlags	BounceFlags;	// which bouncing type?
	uint32_t			SpawnFlags;		// Increased to uint32_t because of Doom 64
	double			meleerange;		// specifies how far a melee attack reaches.
	double			meleethreshold;	// Distance below which a monster doesn't try to shoot missiles anynore
									// but instead tries to come closer for a melee attack.
									// This is not the same as meleerange
	double			maxtargetrange;	// any target farther away cannot be attacked
	double			missilechancemult; // distance multiplier for CheckMeleeRange, formerly done with MISSILE(EVEN)MORE flags.
	double			bouncefactor;	// Strife's grenades use 50%, Hexen's Flechettes 70.
	double			wallbouncefactor;	// The bounce factor for walls can be different.
	double			Gravity;		// [GRB] Gravity factor
	double			Friction;
	double			pushfactor;
	double			ShadowAimFactor;	// [inkoalawetrust] How much the actors' aim is affected when attacking shadow actors.
	double			ShadowPenaltyFactor;// [inkoalawetrust] How much the shadow actor affects its' shooters' aim.
	int				bouncecount;	// Strife's grenades only bounce twice before exploding
	int 			FastChaseStrafeCount;
	int				lastpush;
	int				activationtype;	// How the thing behaves when activated with USESPECIAL or BUMPSPECIAL
	int				lastbump;		// Last time the actor was bumped, used to control BUMPSPECIAL
	int				Score;			// manipulated by score items, ACS or DECORATE. The engine doesn't use this itself for anything.
	FString *		Tag;			// Strife's tag name.
	int				DesignatedTeam;	// Allow for friendly fire cacluations to be done on non-players.
	int				friendlyseeblocks;	// allow to override friendly search distance calculation

	TObjPtr<AActor*> BlockingMobj;	// Actor that blocked the last move
	line_t			*BlockingLine;	// Line that blocked the last move
	line_t			*MovementBlockingLine; // Line that stopped the Actor's movement in P_XYMovement
	sector_t		*Blocking3DFloor;	// 3D floor that blocked the last move (if any)
	sector_t		*BlockingCeiling;	// Sector that blocked the last move (ceiling plane slope)
	sector_t		*BlockingFloor;		// Sector that blocked the last move (floor plane slope)

	DAngle			ThrustAngleOffset; //For VR: offset thrust angles by this amount
	uint32_t		freezetics;	// actor has actions completely frozen (including movement) for this many tics, but they still get Tick() calls

	int PoisonDamage; // Damage received per tic from poison.
	FName PoisonDamageType; // Damage type dealt by poison.
	int PoisonDuration; // Duration left for receiving poison damage.
	int PoisonPeriod; // How often poison damage is applied. (Every X tics.)

	int PoisonDamageReceived; // Damage received per tic from poison.
	FName PoisonDamageTypeReceived; // Damage type received by poison.
	int PoisonDurationReceived; // Duration left for receiving poison damage.
	int PoisonPeriodReceived; // How often poison damage is applied. (Every X tics.)
	TObjPtr<AActor*> Poisoner; // Last source of received poison damage.

	// a linked list of sectors where this object appears
	struct msecnode_t	*touching_sectorlist;				// phares 3/14/98
	struct msecnode_t	*touching_sectorportallist;		// same for cross-sectorportal rendering
	struct portnode_t	*touching_lineportallist;		// and for cross-lineportal
	struct msecnode_t	*touching_rendersectors; // this is the list of sectors that this thing interesects with it's max(radius, renderradius).
	int validcount;


	TObjPtr<AActor*>	Inventory;		// [RH] This actor's inventory
	uint32_t			InventoryID;	// A unique ID to keep track of inventory items

	uint8_t smokecounter;
	uint8_t FloatBobPhase;
	uint8_t FriendPlayer;				// [RH] Player # + 1 this friendly monster works for (so 0 is no player, 1 is player 0, etc)
	double FloatBobStrength;
	double FloatBobFactor;
	PalEntry BloodColor;
	FTranslationID BloodTranslation;

	// [RH] Stuff that used to be part of an Actor Info
	FSoundID SeeSound;
	FSoundID AttackSound;
	FSoundID PainSound;
	FSoundID DeathSound;
	FSoundID ActiveSound;
	FSoundID UseSound;		// [RH] Sound to play when an actor is used.
	FSoundID BounceSound;
	FSoundID WallBounceSound;
	FSoundID CrushPainSound;

	double MaxDropOffHeight;
	double MaxStepHeight;
	double MaxSlopeSteepness;

	int32_t Mass;
	int16_t PainChance;
	int PainThreshold;
	FName DamageType;
	FName DamageTypeReceived;
	double DamageFactor;
	double DamageMultiply;

	FName PainType;
	FName DeathType;
	PClassActor *TeleFogSourceType;
	PClassActor *TeleFogDestType;
	int RipperLevel;
	int RipLevelMin;
	int RipLevelMax;

	int MinRespawnTics; // id24: Minimal Tics to Respawn the Actor in skills with respawning or with ALWAYSRESPAWN flag, negative values are seconds and 0 uses the Skill value
	int RespawnDice; // id24: Chance for respawning

	int ConversationRoot;				// THe root of the current dialogue
	FStrifeDialogueNode* Conversation;	// [RH] The dialogue to show when this actor is "used."

	FState *SpawnState;
	FState *SeeState;
	FState *MeleeState;
	FState *MissileState;


	// [RH] Decal(s) this weapon/projectile generates on impact.
	FDecalBase *DecalGenerator;

	// [RH] Used to interpolate the view to get >35 FPS
	DVector3 Prev;
	// TODO: Reduce the size of these, this much accuracy isn't needed. Unfortunately a lot
	// of this data is built around using doubles so this will require changing a lot of things.
	DRotator PrevAngles;
	DVector2 PrevScale;
	double PrevAlpha;
	DAngle   PrevFOV;
	TArray<FDynamicLight *> AttachedLights;
	TDeletingArray<FLightDefaults *> UserLights;
	int PrevPortalGroup;

	// When was this actor spawned?
	int SpawnTime;
	uint32_t SpawnOrder;

	int UnmorphTime;
	int MorphFlags;
	int PremorphProperties;
	PClassActor* MorphExitFlash;
	// landing speed from a jump with normal gravity (squats the player's view)
	// (note: this is put into AActor instead of the PlayerPawn because non-players also use the value)
	double LandingSpeed;
	TMap<FName, TObjPtr<DBehavior*>> Behaviors;


	// ThingIDs
	void SetTID (int newTID);

private:
	void AddToHash ();
	void RemoveFromHash ();

	static inline int TIDHASH (int key) { return key & 127; }

public:
	static FSharedStringArena mStringPropertyData;
private:
	friend class FActorIterator;

	bool FixMapthingPos();

public:
	void LinkToWorld (FLinkContext *ctx, bool spawningmapthing=false, sector_t *sector = NULL);
	void UnlinkFromWorld(FLinkContext *ctx);
	void AdjustFloorClip ();
	bool IsMapActor();
	bool SetState (FState *newstate, bool nofunction=false);
	void SplashCheck();
	void PlayDiveOrSurfaceSounds(int oldlevel = 0);
	bool UpdateWaterLevel (bool splash=true);
	bool isFast();
	bool isSlow();
	void SetIdle(bool nofunction=false);
	void ClearCounters();
	FState *GetRaiseState();
	void Revive();

	void SetDamage(int dmg)
	{
		DamageVal = dmg;
		DamageFunc = nullptr;
	}

	bool IsZeroDamage() const
	{
		return DamageVal == 0 && DamageFunc == nullptr;
	}

	FState *FindState (FName label) const
	{
		return GetClass()->FindState(1, &label);
	}

	FState *FindState (FName label, FName sublabel, bool exact = false) const
	{
		FName names[] = { label, sublabel };
		return GetClass()->FindState(2, names, exact);
	}

	FState *FindState(int numnames, FName *names, bool exact = false) const
	{
		return GetClass()->FindState(numnames, names, exact);
	}

	DBehavior* FindBehavior(FName type) const
	{
		auto b = Behaviors.CheckKey(type);
		return b != nullptr ? b->Get() : nullptr;
	}
	bool IsValidBehavior(const DBehavior& b) const
	{
		return !(b.ObjectFlags & OF_EuthanizeMe) && b.Owner.ForceGet() == this;
	}
	DBehavior* AddBehavior(PClass& type);
	bool RemoveBehavior(FName type);
	void TickBehaviors();
	void MoveBehaviors(AActor& from);
	void ClearBehaviors(PClass* type = nullptr);
	// Internal only, mostly for traveling.
	void UnlinkBehaviorsFromLevel();
	void LinkBehaviorsToLevel();

	bool HasSpecialDeathStates () const;

	double X() const
	{
		return __Pos.X;
	}
	double Y() const
	{
		return __Pos.Y;
	}
	double Z() const
	{
		return __Pos.Z;
	}
	DVector3 Pos() const
	{
		return __Pos;
	}
	// Note: Never compare z directly with a plane height if you want to know if the actor is *on* the plane. Some very minor inaccuracies may creep in. Always use these inline functions!
	// Comparing with floorz is ok because those values come from the same calculations.
	bool isAbove(double checkz) const
	{
		return Z() > checkz + EQUAL_EPSILON;
	}
	bool isBelow(double checkz) const
	{
		return Z() < checkz - EQUAL_EPSILON;
	}
	bool isAtZ(double checkz) const
	{
		return fabs(Z() - checkz) < EQUAL_EPSILON;
	}

	double RenderRadius() const
	{
		return max(radius, renderradius);
	}

	DVector3 PosRelative(int grp) const;
	DVector3 PosRelative(const AActor *other) const;
	DVector3 PosRelative(sector_t *sec) const;
	DVector3 PosRelative(const line_t *line) const;

	FVector3 SoundPos() const
	{
		// the sound system switches y and z axes so this function must, too.
		// fixme: This still needs portal handling
		return{ float(X()), float(Z()), float(Y()) };
	}
	// RS FORK -- WORLD CLOCK: which clock this actor lives on.
	//
	// The player's pawn, and anything a mod marked +REALTIME. Two loads and a test:
	// the tic pass asks it of every actor, and at full speed it is never asked at all.
	//
	// What an actor CARRIES is not decided here. An item's owner lives in ZScript
	// (inventory.zs), not in AActor, so the carried-item rule belongs where the
	// carrier is known: p_tick.cpp walks a real-time actor's Inventory chain, which
	// is what keeps the player's weapons and ammo cycling at full speed.
	bool IsRealTimeActor() const
	{
		return player != nullptr || (flags9 & MF9_REALTIME);
	}

	// RS FORK -- WORLD CLOCK: the fraction this actor is DRAWN at. p_mobj.cpp.
	//
	// THE ONE PLACE a draw fraction is chosen, so every interpolator below and every
	// caller that passes vp.TicFrac gets the right one without knowing the clock
	// exists. A world actor's Prev spans a whole world step, so it is drawn on the
	// world fraction; a real-time actor's Prev spans one real tic, so it keeps
	// TicFrac. At full speed both are TicFrac, the same double, unchanged.
	double DrawFrac(double ticFrac) const;

	DVector3 InterpolatedPosition(double ticFrac) const
	{
		if (renderflags & RF_DONTINTERPOLATE) return Pos();
		const double f = DrawFrac(ticFrac);
		return Prev * (1.0 - f) + Pos() * f;
	}
	DRotator InterpolatedAngles(double ticFrac) const
	{
		const double f = DrawFrac(ticFrac);
		DRotator result;
		result.Yaw = PrevAngles.Yaw + deltaangle(PrevAngles.Yaw, Angles.Yaw) * f;
		result.Pitch = PrevAngles.Pitch + deltaangle(PrevAngles.Pitch, Angles.Pitch) * f;
		result.Roll = PrevAngles.Roll + deltaangle(PrevAngles.Roll, Angles.Roll) * f;
		return result;
	}
	// RS FORK -- FollowBodyYaw as the renderer draws it (see FollowBodyYawInterp):
	// the value as written unless the flag is on now AND was on at the last snapshot.
	double DrawFollowBodyYaw(double ticFrac) const
	{
		if (!FollowBodyYawInterp || !PrevFollowBodyYawLive) return FollowBodyYaw;
		return PrevFollowBodyYaw + deltaangle(DAngle::fromDeg(PrevFollowBodyYaw), DAngle::fromDeg(FollowBodyYaw)).Degrees() * ticFrac;
	}
	// RS FORK -- FollowHandRot as the renderer draws it: last tic's turn to this
	// tic's, like the position. A plain lerp, not deltaangle: it is an offset a
	// script eases, not a heading that wraps.
	DVector3 DrawFollowHandRot(double ticFrac) const
	{
		return PrevFollowHandRot * (1.0 - ticFrac) + FollowHandRot * ticFrac;
	}
	DVector2 InterpolatedScale(double ticFrac) const
	{
		if (!(renderflags2 & RF2_INTERPOLATESCALE)) return Scale;
		const double f = DrawFrac(ticFrac);	// RS FORK -- WORLD CLOCK
		return PrevScale * (1.0 - f) + Scale * f;
	}
	double InterpolatedAlpha(double ticFrac) const
	{
		if (!(renderflags2 & RF2_INTERPOLATEALPHA)) return Alpha;
		const double f = DrawFrac(ticFrac);	// RS FORK -- WORLD CLOCK
		return PrevAlpha * (1.0 - f) + Alpha * f;
	}
	float GetSpriteOffset(bool y) const
	{
		if (y)	return (float)(renderflags2 & RF2_FLIPSPRITEOFFSETY ? SpriteOffset.Y : -SpriteOffset.Y);
		else	return (float)(renderflags2 & RF2_FLIPSPRITEOFFSETX ? SpriteOffset.X : -SpriteOffset.X);
	}
	DAngle GetSpriteAngle(DAngle viewangle, double ticFrac)
	{
		if (flags7 & MF7_SPRITEANGLE)
		{
			return SpriteAngle;
		}
		else
		{
			DAngle thisang;
			if (renderflags & RF_INTERPOLATEANGLES) thisang = PrevAngles.Yaw + deltaangle(PrevAngles.Yaw, Angles.Yaw) * ticFrac;
			else thisang = Angles.Yaw;
			return viewangle - (thisang + SpriteRotation);
		}
	}
	DVector3 PosPlusZ(double zadd) const
	{
		return { X(), Y(), Z() + zadd };
	}
	DVector3 PosAtZ(double zadd) const
	{
		return{ X(), Y(), zadd };
	}
	double Top() const
	{
		return Z() + Height;
	}
	double CenterOffset() const
	{
		return Height / 2;
	}
	double Center() const
	{
		return Z() + CenterOffset();
	}
	void SetZ(double newz, bool moving = true)
	{
		__Pos.Z = newz;
	}
	void AddZ(double newz, bool moving = true)
	{
		__Pos.Z += newz;
		if (!moving) Prev.Z = Z();
	}

	void SetXY(const DVector2 &npos)
	{
		__Pos.X = npos.X;
		__Pos.Y = npos.Y;
	}
	void SetXYZ(double xx, double yy, double zz)
	{
		__Pos = { xx,yy,zz };
	}
	void SetXYZ(const DVector3 &npos)
	{
		__Pos = npos;
	}

	double VelXYToSpeed() const
	{
		return DVector2(Vel.X, Vel.Y).Length();
	}

	double VelToSpeed() const
	{
		return Vel.Length();
	}

	void AngleFromVel()
	{
		Angles.Yaw = VecToAngle(Vel.X, Vel.Y);
	}

	void VelFromAngle()
	{
		Vel.X = Speed * Angles.Yaw.Cos();
		Vel.Y = Speed * Angles.Yaw.Sin();
	}

	void VelFromAngle(double speed)
	{
		Vel.X = speed * Angles.Yaw.Cos();
		Vel.Y = speed * Angles.Yaw.Sin();
	}

	void VelFromAngle(double speed, DAngle angle)
	{
		Vel.X = speed * angle.Cos();
		Vel.Y = speed * angle.Sin();
	}

	void Thrust()
	{
		Thrust(Angles.Yaw, Speed);
	}

	void Thrust(double speed)
	{
		Thrust(Angles.Yaw, speed);
	}

	void Thrust(DAngle angle, double speed)
	{
		Vel.X += speed * angle.Cos();
		Vel.Y += speed * angle.Sin();
	}

	void Thrust(const DVector3& vel)
	{
		Vel += vel;
	}

	void Vel3DFromAngle(DAngle angle, DAngle pitch, double speed)
	{
		double cospitch = pitch.Cos();
		Vel.X = speed * cospitch * angle.Cos();
		Vel.Y = speed * cospitch * angle.Sin();
		Vel.Z = speed * -pitch.Sin();
	}

	void Vel3DFromAngle(DAngle pitch, double speed)
	{
		double cospitch = pitch.Cos();
		Vel.X = speed * cospitch * Angles.Yaw.Cos();
		Vel.Y = speed * cospitch * Angles.Yaw.Sin();
		Vel.Z = speed * -pitch.Sin();
	}

	// This is used by many vertical velocity calculations.
	// Better have it in one place, if something needs to be changed about the formula.
	double DistanceBySpeed(AActor *dest, double speed) const
	{
		return max(1., Distance2D(dest) / speed);
	}

	int GetLightLevel(sector_t* rendersector);
	int ApplyDamageFactor(FName damagetype, int damage) const;
	int GetModifiedDamage(FName damagetype, int damage, bool passive, AActor *inflictor, AActor *source, int flags, DAngle angle);
	void DeleteAttachedLights();
	bool isFrozen() const;

	bool				hasmodel;

	//For VR, override firing position - Thank-you Fishbiter for this code!!
	bool OverrideAttackPosDir;

	DVector3 AttackPos;
	DAngle   AttackPitch;
	DAngle   AttackAngle;
	DAngle   AttackRoll;

	// The main hand's REAL wrist roll, which AttackRoll cannot carry.
	//
	// AttackPitch and AttackAngle survive the playsim because the usercmd has
	// a weaponpitch and a weaponyaw to rebuild them from; there is no
	// weaponroll (see d_protocol.h), so UpdateCanonicalMainHandPose zeroes
	// AttackRoll every tic to keep peers deterministic. The VR backends had
	// already written the true value a moment earlier, and it was thrown away
	// before any script could read it -- so the held model visibly rolled with
	// the wrist while ZScript was told the wrist was level.
	//
	// This field is renderer-owned like the Hmd* block below: written by the
	// VR backends, never touched by the playsim, never serialised. Read it
	// for presentation -- anything welded to the held weapon -- and keep
	// reading AttackRoll for anything that must agree across a network.
	// OffhandRoll needs no equivalent: nothing zeroes it outside multiplayer.
	DAngle   MainHandRoll;

	// REAL controller velocity, from OpenXR's own sensor fusion via
	// XrSpaceVelocity -- not inferred by differencing two AttackPos samples
	// 28ms apart in ZScript, which amplifies tracking jitter and throws away
	// everything the render thread saw between tics. Renderer-owned like
	// AttackPos itself: written every frame by the VR backend, never
	// serialised, zeroed (not left stale) on any frame the runtime does not
	// report a valid velocity. Linear is map-units/second in the same frame
	// AttackPos lives in; angular is radians/second about the MAP axes (X, Y,
	// Z up) -- NOT the hand's own. Spin about the barrel is its projection
	// onto the hand's forward axis. What a swung weapon's tip is doing that
	// the hand's linear velocity alone cannot express.
	DVector3 AttackVel;
	DVector3 AttackAngularVel;

	// RS FORK -- DIRECT MODEL FRAME ADDRESSING FOR WORLD ACTORS.
	//
	// The same three fields DPSprite has carried since the psprite hands were
	// posed (p_pspr.h). They are the ONLY mechanism in this tree that has ever
	// actually driven a rigged hand to a chosen shape: the decoupled animation
	// path resolves a frame through the sprite letter table, which caps at
	// MAX_SPRITE_FRAMES, and the hand rig's poses live at frames 0-10 and
	// 1289-1297. There is no letter that names frame 1293.
	//
	// Absent these, a world-actor hand had no way to be posed at all -- which is
	// why the manipulation set has been authored and unreachable the whole time.
	//
	// ModelFrame < 0 means "resolve normally", so an actor that never touches
	// them behaves exactly as before. ModelFrameLerp in 0..1 blends ModelFrame
	// toward ModelFrameNext by that factor, blending BONE MATRICES -- the
	// fingers travel between shapes instead of the hand snapping.
	//
	// Renderer-owned and not serialised, same as the Hmd* block: a pose is
	// re-decided every tic from live state, so persisting one would restore a
	// grip on a gun that is no longer held.
	int   ModelFrame     = -1;
	int   ModelFrameNext = -1;
	float ModelFrameLerp = -1.f;

	DVector3 (*AttackDir)(AActor* actor, DAngle yaw, DAngle pitch) = P_FlatWeaponDir;

	DVector3 OffhandPos;
	DAngle   OffhandPitch;
	DAngle   OffhandAngle;
	DAngle   OffhandRoll;

	// Same as AttackVel/AttackAngularVel above, off hand.
	DVector3 OffhandVel;
	DVector3 OffhandAngularVel;

	// Real headset position/orientation in map units, world space. Unlike
	// AttackPos/OffhandPos this is not a per-hand aim ray -- it is where the
	// player's actual head is right now, for body-relative UI (holsters,
	// menus anchored to the player) that needs to reason about the player's
	// physical pose rather than where they are aiming.
	DVector3 HmdPos;
	DAngle   HmdYaw;
	DAngle   HmdPitch;
	DAngle   HmdRoll;

	// Two-hand stabilize reach for the current ready weapon, in real-world
	// inches. ZScript-owned: copied each tic from ReadyWeapon.StabilizeDistance
	// (see RS_StabilizeSync/equivalent). 0 = use vr_stabilize_distance_inches,
	// negative = stabilize disabled for this weapon. Native reads this instead
	// of a fixed literal so the reach is per-weapon, not one global constant.
	double StabilizeReach;

	// Grip-priority claims: while true for a hand, that hand's grip is being
	// spent on body-relative UI (a holster reach) this frame, and every other
	// consumer of that hand's grip -- stabilize, the secondary-button modifier
	// layer, the plain grip keybind -- must stand down for it. ZScript-owned,
	// written by whatever holds the holster anchors and proximity check; native
	// only reads these. This is the single arbitration point: one hand, one
	// meaning, decided in priority order (holster beats everything else) before
	// any other grip consumer runs.
	bool HolsterClaimMain;
	bool HolsterClaimOff;

	// THIS HAND HAS SOMETHING IT COULD TAKE HOLD OF. Script-owned, exactly like
	// HolsterClaim* above, and read by the same arbiter.
	//
	// The engine cannot work this out. Whether anything is in reach depends on a
	// reach volume, a grabbability table and a targeting cone that all live in
	// script; there is nothing here to test against. So script claims and the
	// arbiter decides.
	//
	// It exists because the dominant grip has two jobs. With
	// vr_secondary_button_mappings on, holding it is the shift layer -- and that
	// layer stands analog turning down, so reaching for a barrel stops you
	// turning. Both jobs are legitimate; what was missing was any way to tell
	// them apart. A hand pointed at something it can grab is grabbing. A hand
	// pointed at nothing is still your modifier.
	bool GrabClaimMain;
	bool GrabClaimOff;

	// THIS HAND IS AT A BODY HARDPOINT. Script-owned, and SEPARATE from
	// HolsterClaim* even though both are body-anchored volumes.
	//
	// They were the same field, and the two mods that drive them both wrote it
	// unconditionally every tic: whichever handler ran second won, so a hand
	// genuinely inside a holster had its claim erased by the hardpoint mod
	// reporting "not at a hardpoint", and the reverse. One boolean cannot carry
	// two independent facts, and nothing logged the loss.
	//
	// Separate fields also make the states legible downstream, which is the
	// point: holstering, using a hardpoint, stabilizing and grabbing are four
	// different things a hand can be doing and each needs to be nameable.
	bool HardpointClaimMain;
	bool HardpointClaimOff;

	// What the grip arbiter decided each hand's grip means this frame; see
	// EGripContext in vk_openxrdevice.h. Engine-owned and read-only to script,
	// the mirror image of HolsterClaim* above: script says what it wants to
	// claim, the engine says what actually won.
	int GripContextMain;
	int GripContextOff;

	// The RAW squeeze, per hand, arbiter-independent. Engine-owned.
	//
	// GripContext above cannot answer this. It is deliberately published even
	// while the grip is NOT held -- a hand keeps holding a magazine when the
	// player relaxes the squeeze, and the pose has to keep showing that -- so
	// the moment script claims a subject, GripContext latches to GRIPCTX_Object
	// and stays there. Any script testing `GripContext != 0` for "is the grip
	// down" is correct right up until something claims, and then reads as a
	// grip held forever, so a toggle can never see its release edge.
	//
	// Held state needs the button itself and nothing else. This is that button.
	bool GripHeldMain;
	bool GripHeldOff;

	// What each hand is closed on, EGripSubject. Same script-claims /
	// engine-arbitrates split as the pair above: script writes GripClaim*,
	// because only script can know that a shell is being held, and the engine
	// writes GripSubject* with whatever actually won -- which is the claim,
	// unless the hand is in a holster, which the engine resolves itself.
	//
	// A claim also stands two-hand stabilize down for that hand. That matters
	// more than it sounds: stabilize is a bare proximity test between the two
	// controllers, and reloading puts the hands together, so without this every
	// reload gesture reads as a two-hand grip on the weapon.
	int GripClaimMain;
	int GripClaimOff;
	int GripSubjectMain;
	int GripSubjectOff;

	// The off hand is genuinely ON the main hand's weapon -- its grip, its
	// forend, its foregrip -- as opposed to merely near it. Engine-owned,
	// derived from the subject above.
	//
	// It deliberately does NOT move the weapon. The old stabilize did, and that
	// was the half of it worth removing -- a gun that repositions itself because
	// two controllers came close is a gun that is not where your hands are.
	//
	// *** LOCAL ONLY. NOT REPLICATED. DO NOT LET IT REACH THE PLAYSIM. ***
	//
	// vk_openxrdevice.cpp writes this on consolePawn ALONE, from this machine's
	// OpenXR devices. Nothing carries it to anyone else, so on every other
	// machine a remote player's copy is permanently false.
	//
	// The comment here used to say "weapons read it and tighten their spread".
	// THAT WOULD HAVE BEEN A DESYNC: spread decides where a bullet goes, so a
	// two-handed shot would have been tight on the shooter's screen and wide on
	// everybody else's, with no error anywhere. Corrected 2026-09-19, before the
	// first reader existed -- which is the only reason it was cheap.
	//
	// SAFE: anything this machine merely DRAWS -- a pose, a hand, a HUD.
	// NOT SAFE: spread, damage, recoil that moves a round, or any decision the
	// playsim makes. If a two-handed hold must affect a shot, it has to travel
	// as playsim state or in the usercmd, the way every other gameplay input
	// does. See Engine docs/CROSSPLATFORM_COOP_RULE.md.
	//
	// It is listed in p_vrdemo.cpp's X() set, so a DEMO replays it correctly for
	// the recording player. That is not replication and does not make it safe.
	bool TwoHandedHold;

	// Capacitive finger contact, FINGERTOUCH_* bits. Contact is not a press:
	// this says where a finger RESTS, which is what a hand pose needs.
	int FingerTouchMain;
	int FingerTouchOff;

	// HOW FAR THE TRIGGER AND THE SQUEEZE ARE PULLED, 0..1.
	//
	// GripHeld* above is the same physical control reduced to a bool by the
	// runtime's own threshold, and that reduction throws away everything a
	// part driven by a finger needs: a trigger with a real break point and
	// reset, a pull that stops short of firing, a grip that tightens on a
	// slide rather than merely closing on it.
	//
	// Both are published because they are different questions, and the
	// boolean is NOT redundant -- it carries the runtime's own idea of where
	// "pressed" is, which is the right threshold for anything that just wants
	// a button and should not be re-derived per mod from the analog value.
	//
	// 0 or 1 with no travel in between on hardware whose squeeze is a click
	// (Vive wand, WMR). That is the honest answer for those controllers, not
	// a missing feature.
	double TriggerValueMain;
	double TriggerValueOff;
	double GripValueMain;
	double GripValueOff;

	// Thumbstick position per hand, each axis -1..1, centred at (0,0).
	//
	// FingerTouch* above can say a thumb is resting somewhere; it cannot say
	// WHERE along a range, which is what a thumb sliding a fire selector or
	// stepping a sight dial actually needs. These axes were already read from
	// the runtime every frame and discarded before script could see them.
	DVector2 ThumbPosMain;
	DVector2 ThumbPosOff;

	// Accumulated CONTROLLER-driven yaw (snap turn and stick turn), in degrees.
	// HmdYaw is physical head yaw PLUS this, and separating them matters: a
	// body-relative anchor must follow controller rotation exactly, because
	// that turns the whole virtual body, while physical head rotation should
	// be allowed a neck's worth of freedom first. Without the split a 45
	// degree snap turn hides inside the neck deadzone and every snap leaves
	// body-anchored things permanently offset.
	double VRTurnYaw;

	// What the laser sight's own trace is currently resting on, per hand --
	// engine-owned, written every render frame from the same Trace() call
	// that already draws the beam (hw_weapon.cpp: GetLaserBeamEndpoints).
	// TObjPtr, not a raw pointer: the target can die between the frame this
	// is written and the tic a script reads it, and this needs to go null
	// when that happens rather than dangle.
	//
	// Exists so a mod can ask "is the sight actually on a head" without
	// running its own second trace that could disagree with the one the
	// beam is drawn from -- offsets, per-hand quirks and the script-side
	// beam-shortening hook (VR_IsScriptLaserForcedFor) all live in that one
	// trace, and reimplementing it from AttackPos/AttackAngle alone would
	// drift from what the player actually sees.
	TObjPtr<AActor*> LaserTraceTargetMain;
	TObjPtr<AActor*> LaserTraceTargetOff;
	DVector3 LaserTraceHitPosMain;
	DVector3 LaserTraceHitPosOff;

	// The other direction: did a mod decide the point above is a headshot?
	// Script-owned (a class list of what even has a head is mod data, not
	// engine data -- see the Headshots gameplay mod), native reads this to
	// react on the sight. One tic of lag behind LaserTraceTarget* is fine;
	// this is a cosmetic reaction, not a hit determination.
	bool LaserHeadshotLinedUpMain;
	bool LaserHeadshotLinedUpOff;

	DVector3 (*OffhandDir)(AActor* actor, DAngle yaw, DAngle pitch) = P_FlatWeaponDir;
};

class FActorIterator
{
	friend struct FLevelLocals;
protected:
	FActorIterator (AActor **hash, int i) : TIDHash(hash), base (nullptr), id (i)
	{
	}
	FActorIterator (AActor **hash, int i, AActor *start) : TIDHash(hash), base (start), id (i)
	{
	}
public:
	AActor *Next ()
	{
		if (id == 0)
			return nullptr;
		if (!base)
			base = TIDHash[id & 127];
		else
			base = base->inext;

		while (base && base->tid != id)
			base = base->inext;

		return base;
	}
	void Reinit()
	{
		base = nullptr;
	}

private:
	AActor **TIDHash;
	AActor *base;
	int id;
};

class NActorIterator : public FActorIterator
{
	friend struct FLevelLocals;
	const PClass *type;
protected:
	NActorIterator (AActor **hash, const PClass *cls, int id) : FActorIterator (hash, id) { type = cls; }
	NActorIterator (AActor **hash, FName cls, int id) : FActorIterator (hash, id) { type = PClass::FindClass(cls); }
public:
	AActor *Next ()
	{
		AActor *actor;
		if (type == nullptr) return nullptr;
		do
		{
			actor = FActorIterator::Next ();
		} while (actor && !actor->IsKindOf (type));
		return actor;
	}
};

PClassActor *ClassForSpawn(FName classname);

inline AActor *Spawn(FLevelLocals *Level, PClassActor *type)
{
	return AActor::StaticSpawn(Level, type, DVector3(0, 0, 0), NO_REPLACE);
}

inline AActor *Spawn(FLevelLocals *Level, PClassActor *type, const DVector3 &pos, replace_t allowreplacement)
{
	return AActor::StaticSpawn(Level, type, pos, allowreplacement);
}

inline AActor *Spawn(FLevelLocals *Level, FName type)
{
	return AActor::StaticSpawn(Level, ClassForSpawn(type), DVector3(0, 0, 0), NO_REPLACE);
}

inline AActor *Spawn(FLevelLocals *Level, FName type, const DVector3 &pos, replace_t allowreplacement)
{
	return AActor::StaticSpawn(Level, ClassForSpawn(type), pos, allowreplacement);
}

template<class T> inline T *Spawn(FLevelLocals *Level, const DVector3 &pos, replace_t allowreplacement)
{
	return static_cast<T *>(AActor::StaticSpawn(Level, RUNTIME_CLASS(T), pos, allowreplacement));
}

template<class T> inline T *Spawn(FLevelLocals *Level)	// for inventory items we do not need coordinates and replacement info.
{
	return static_cast<T *>(AActor::StaticSpawn(Level, RUNTIME_CLASS(T), DVector3(0, 0, 0), NO_REPLACE));
}

inline PClassActor *PClass::FindActor(FName name)
{
	auto cls = FindClass(name);
	return cls && cls->IsDescendantOf(RUNTIME_CLASS(AActor)) ? static_cast<PClassActor*>(cls) : nullptr;
}

inline PClassActor *ValidateActor(PClass *cls)
{
	return cls && cls->IsDescendantOf(RUNTIME_CLASS(AActor)) ? static_cast<PClassActor*>(cls) : nullptr;
}

void PrintMiscActorInfo(AActor * query);
AActor *P_LinePickActor(AActor *t1, DAngle angle, double distance, DAngle pitch, ActorFlags actorMask, uint32_t wallMask);

// If we want to make P_AimLineAttack capable of handling arbitrary portals, it needs to pass a lot more info than just the linetarget actor.
struct FTranslatedLineTarget
{
	AActor *linetarget;
	DAngle angleFromSource;
	DAngle attackAngleFromSource;
	bool unlinked;	// found by a trace that went through an unlinked portal.
};

void PlayerPointerSubstitution(AActor* oldPlayer, AActor* newPlayer, bool removeOld);
int MorphPointerSubstitution(AActor* from, AActor* to);

#define S_FREETARGMOBJ	1

#endif // __P_MOBJ_H__
